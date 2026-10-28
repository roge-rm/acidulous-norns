// Songs: a song zip from the app unpacked into the songs folder, its
// engine.txt replayed into EngineHost on the worker, and each rack read back
// into the core's part state. See /song_load in bridge.h.
#include "core.h"

#include <EngineHost.h>
#include <drivers/AudioDriver.h>
#include <engine/core/PresetFile.h>
#include <engine/inputmod/InputModRegistry.h>
#include <engine/machine/MachineRegistry.h>
#include <engine/machine/nexus/Graph.h>
#include <engine/rack/MasterBus.h>
#include <jni.h>
#include <platform/lines/EngineLines.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <memory>
#include <sys/stat.h>

namespace core
{

using namespace acidulous;

namespace
{

// The most a song zip may hold, unpacked, and the most the zip itself may
// be. unzip holds every entry in memory at once beside the zip, and a norns
// has 1 GB, so 128 MB leaves room for scsynth and matron. That's about five
// and a half minutes of frozen clips (float32 stereo at 48 kHz).
constexpr size_t kMaxSongBytes = size_t(128) << 20;

// A track's channel strip defaults, normalised, as in Rack.cpp's kChannelDefs.
const std::pair<const char*, float> kChannelDefaults[] = {
    {"gain", 1.f / 1.5f}, {"pan", 0.5f},        {"mute", 0.f},        {"solo", 0.f},
    {"sendreverb", 0.f},  {"senddelay", 0.f},   {"midimode", 0.f},    {"midichannel", 0.f},
    {"swing", 0.f},       {"output", 0.f},      {"transpose", 0.5f},  {"velocity", 0.f},
};

const char* const kModUnitNames[] = {"mod1", "mod2", "mod3"};

// --- A small JSON reader, enough for song.json -------------------------------

struct Json
{
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double                                   num = 0;
    std::string                              str;
    std::vector<Json>                        items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const
    {
        for(const auto& kv : fields)
            if(kv.first == key)
                return &kv.second;
        return nullptr;
    }
    std::string text(const char* key) const
    {
        const Json* j = get(key);
        return j && j->kind == String ? j->str : std::string();
    }
};

struct JsonReader
{
    const std::string& s;
    size_t             i = 0;
    int                depth = 0;
    bool               ok = true;

    void space()
    {
        while(i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }
    void put_utf8(std::string& out, uint32_t c)
    {
        if(c < 0x80)
            out += char(c);
        else if(c < 0x800)
            out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
        else if(c < 0x10000)
            out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
        else
            out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)),
                out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
    }
    bool hex4(uint32_t& c)
    {
        if(i + 4 > s.size())
            return false;
        c = 0;
        for(int k = 0; k < 4; ++k)
        {
            const char h = s[i++];
            c <<= 4;
            if(h >= '0' && h <= '9')
                c |= uint32_t(h - '0');
            else if(h >= 'a' && h <= 'f')
                c |= uint32_t(h - 'a' + 10);
            else if(h >= 'A' && h <= 'F')
                c |= uint32_t(h - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string& out)
    {
        if(i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while(i < s.size() && s[i] != '"')
        {
            char c = s[i++];
            if(c != '\\')
            {
                out += c;
                continue;
            }
            if(i >= s.size())
                return false;
            c = s[i++];
            switch(c)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                uint32_t u = 0;
                if(!hex4(u))
                    return false;
                if(u >= 0xd800 && u < 0xdc00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    i += 2;
                    uint32_t lo = 0;
                    if(!hex4(lo))
                        return false;
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                }
                put_utf8(out, u);
                break;
            }
            default: out += c;
            }
        }
        if(i >= s.size())
            return false;
        ++i;
        return true;
    }
    Json value()
    {
        Json v;
        space();
        if(!ok || i >= s.size() || ++depth > 64)
        {
            ok = false;
            return v;
        }
        const char c = s[i];
        if(c == '{')
        {
            v.kind = Json::Object;
            ++i;
            space();
            if(i < s.size() && s[i] == '}')
                ++i;
            else
                for(;;)
                {
                    space();
                    std::string key;
                    if(!string(key))
                    {
                        ok = false;
                        break;
                    }
                    space();
                    if(i >= s.size() || s[i] != ':')
                    {
                        ok = false;
                        break;
                    }
                    ++i;
                    v.fields.emplace_back(std::move(key), value());
                    space();
                    if(i < s.size() && s[i] == ',')
                    {
                        ++i;
                        continue;
                    }
                    if(i < s.size() && s[i] == '}')
                        ++i;
                    else
                        ok = false;
                    break;
                }
        }
        else if(c == '[')
        {
            v.kind = Json::Array;
            ++i;
            space();
            if(i < s.size() && s[i] == ']')
                ++i;
            else
                for(;;)
                {
                    v.items.push_back(value());
                    space();
                    if(i < s.size() && s[i] == ',')
                    {
                        ++i;
                        continue;
                    }
                    if(i < s.size() && s[i] == ']')
                        ++i;
                    else
                        ok = false;
                    break;
                }
        }
        else if(c == '"')
        {
            v.kind = Json::String;
            ok     = string(v.str) && ok;
        }
        else if(s.compare(i, 4, "true") == 0)
            v.kind = Json::Bool, v.num = 1, i += 4;
        else if(s.compare(i, 5, "false") == 0)
            v.kind = Json::Bool, i += 5;
        else if(s.compare(i, 4, "null") == 0)
            i += 4;
        else
        {
            char*       end   = nullptr;
            const char* start = s.c_str() + i;
            v.kind            = Json::Number;
            v.num             = std::strtod(start, &end);
            if(end == start)
                ok = false;
            i += size_t(end - start);
        }
        --depth;
        return v;
    }
};

// --- Files -------------------------------------------------------------------

bool read_all(const std::string& path, std::vector<uint8_t>& out, size_t max, std::string& why)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if(!f)
    {
        why = std::string("can't read ") + path + ": " + std::strerror(errno);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if(size < 0 || size_t(size) > max)
    {
        std::fclose(f);
        why = size < 0 ? "can't read " + path : "too big to be a song";
        return false;
    }
    out.resize(size_t(size));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if(!ok)
        why = "can't read " + path;
    return ok;
}

bool is_dir(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dirs(const std::string& dir)
{
    for(size_t at = 1; at <= dir.size(); ++at)
        if(at == dir.size() || dir[at] == '/')
        {
            const std::string sub = dir.substr(0, at);
            if(mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    return true;
}

int remove_one(const char* path, const struct stat*, int, struct FTW*)
{
    return std::remove(path);
}

// Deletes [dir] and everything in it, without following links.
bool remove_tree(const std::string& dir)
{
    if(!is_dir(dir))
        return true;
    return nftw(dir.c_str(), remove_one, 16, FTW_DEPTH | FTW_PHYS) == 0;
}

// A zip entry's name is safe to write under the song's folder: relative, no
// "..", nothing odd.
bool safe_entry(const std::string& name)
{
    if(name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    size_t at = 0;
    while(at <= name.size())
    {
        size_t end = name.find('/', at);
        if(end == std::string::npos)
            end = name.size();
        const std::string part = name.substr(at, end - at);
        if(part == "..")
            return false;
        at = end + 1;
    }
    return true;
}

// The song's folder name: the zip's name without .zip, with anything but
// letters, digits, space, - _ and . made _.
std::string song_name(const std::string& path)
{
    std::string base = path;
    while(!base.empty() && base.back() == '/')
        base.pop_back();
    const size_t slash = base.rfind('/');
    if(slash != std::string::npos)
        base = base.substr(slash + 1);
    if(base.size() > 4 && (base.compare(base.size() - 4, 4, ".zip") == 0 || base.compare(base.size() - 4, 4, ".ZIP") == 0))
        base.resize(base.size() - 4);
    for(char& c : base)
        if(!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.'))
            c = '_';
    if(base.empty() || base == "." || base == "..")
        return "song";
    return base;
}

// Whether [text] starts as engine lines do, any version.
bool engine_lines(const std::string& text)
{
    return text.compare(0, 17, "acidulous-engine ") == 0;
}

// The words of an engine line, with quoted strings unquoted. Enough for the
// mount lines.
std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    size_t                   i = 0;
    while(i < line.size())
    {
        while(i < line.size() && line[i] == ' ')
            ++i;
        if(i >= line.size())
            break;
        std::string w;
        if(line[i] == '"')
        {
            ++i;
            while(i < line.size() && line[i] != '"')
            {
                if(line[i] == '\\' && i + 1 < line.size())
                    ++i;
                w += line[i++];
            }
            ++i;
        }
        else
            while(i < line.size() && line[i] != ' ')
                w += line[i++];
        out.push_back(w);
    }
    return out;
}

} // namespace

// What a song's lines mount that EngineHost doesn't say back: modifiers,
// sends, master and group inserts. The last line for a slot wins.
void Core::scan_song_mounts(const std::string& text)
{
    song_mods.assign(size_t(kParts) * kModSlots, std::string());
    song_master_fx.clear();
    song_group_fx.clear();
    song_sends.clear();
    size_t at = 0;
    while(at < text.size())
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at                     = end + 1;
        if(line.compare(0, 5, "Mount") != 0)
            continue;
        const auto w = words(line);
        const auto n = [&](size_t k) { return k < w.size() ? std::atoi(w[k].c_str()) : -1; };
        if(w[0] == "MountInputMod" && w.size() >= 4 && n(1) >= 0 && n(1) < kParts && n(2) >= 0 && n(2) < kModSlots)
            song_mods[size_t(n(1)) * kModSlots + size_t(n(2))] = w[3];
        else if(w[0] == "MountSend" && w.size() >= 3)
            song_sends.emplace_back(n(1), w[2]);
        else if(w[0] == "MountMasterInsert" && w.size() >= 3)
            song_master_fx.push_back(n(1));
        else if(w[0] == "MountGroupInsert" && w.size() >= 4)
            song_group_fx.emplace_back(n(1), n(2));
    }
}

// Waits for the audio thread to finish a block, or renders one inline. False
// if no block came in half a second.
bool Core::next_block()
{
    if(inline_jobs)
    {
        float in[kBlockFrames * 2] = {}, out[kBlockFrames * 2];
        AudioDriver::pull(in, out);
        mounts_queued = false;
        blocks.fetch_add(1, std::memory_order_release);
        return true;
    }
    const uint64_t want = blocks.load(std::memory_order_acquire) + 1;
    for(int tries = 0; tries < 2500 && !stopping; ++tries)
    {
        if(blocks.load(std::memory_order_acquire) >= want)
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

// Empties every rack and what a song put around them: the arrangement,
// frozen clips, modifiers, inserts, master and group inserts, tunings and the
// channel strips, the master's and perform's params (the master volume back
// to the core's 1.0). Replies /mounted "" and /inserts for each part, as an
// unmount does. Worker thread.
void Core::clear_song()
{
    host.transportStop();
    host.panic();
    const int64_t empty = host.snapshotBegin();
    if(empty)
        host.snapshotCommit(empty);
    host.clearFrozen();
    for(int p = 0; p < kParts; ++p)
    {
        unmount(p);
        clear_mods(p);
        for(int s = 0; s < kEffectSlots; ++s)
            if(*host.mountedEffect(p, s))
                host.mountEffect(p, s, "");
        patch_fx[p] = PatchTrackState{};
        part_sends[p][0] = part_sends[p][1] = 0.f;
        const PatchInsert* none[kEffectSlots] = {};
        send_inserts(p, none);
        host.setTuning(p, nullptr);
        }
    song_defaults(false);
    // The sends stay mounted, with the params the script set.
    for(int s = 0; s < kSends; ++s)
        if(send_mounted[s])
            for(const auto& kv : send_values[s])
                push_param(0, s == 0 ? "send1" : "send2", kv.first, kv.second);
    song_master_fx.clear();
    song_group_fx.clear();
    song_mods.clear();
    song_sends.clear();
    song_folder.clear();
    wait_applied();
}

// Back to an empty engine's: the master and group inserts the song mounted
// go, and every track's tuning, channel strip and the master's and perform's
// params return to their defaults (the master volume to the core's 1.0).
// [now] sets them without waiting on a full queue, for the destructor.
void Core::song_defaults(bool now)
{
    const auto set = [&](int rack, const char* unit, const std::string& name, float v) {
        if(now)
            host.setParam(rack, unit, name, v, false);
        else
            push_param(rack, unit, name, v);
    };
    for(int slot : song_master_fx)
        host.mountMasterInsert(slot, "");
    for(const auto& g : song_group_fx)
        host.mountGroupInsert(g.first, g.second, "");
    for(int p = 0; p < kParts; ++p)
    {
        host.setTuning(p, nullptr);
        for(const auto& kv : kChannelDefaults)
            set(p, "channel", kv.first, kv.second);
    }
    // Defaults read from a fresh copy, so new params come back too.
    auto bus = std::make_unique<MasterBus>();
    for(int32_t i = 0; i < bus->params().size(); ++i)
    {
        const ParamDef& d = bus->params().def(i);
        set(0, "master", d.name, std::strcmp(d.name, "volume") == 0 ? 1.f : d.unmap(d.def));
    }
    for(int32_t i = 0; i < bus->perform.params().size(); ++i)
    {
        const ParamDef& d = bus->perform.params().def(i);
        set(0, "perform", d.name, d.unmap(d.def));
    }
}

// The destructor's part of clear_song: no replies, no waiting. The racks
// themselves are emptied by the destructor.
void Core::drop_song()
{
    if(song_folder.empty())
        return;
    host.transportStop();
    const int64_t empty = host.snapshotBegin();
    if(empty)
        host.snapshotCommit(empty);
    host.clearFrozen();
    song_defaults(true);
    song_folder.clear();
}

void Core::unload_song()
{
    if(!song_folder.empty())
        clear_song();
    reply("/song_unloaded", {});
}

// Unpacks [path] (a song zip, or a folder holding an unpacked one) into
// songs_dir/<name>/, empties the engine, replays engine.txt and reads every
// rack back. Replies /song_loaded. Worker thread, with every part pending.
void Core::load_song(const std::string& path, const std::string& songs_dir)
{
    std::string folder;
    std::string why;
    const auto  fail = [&](const std::string& what) {
        error("song: " + what);
        reply("/song_loaded", {Value::s(path), Value::s(folder), Value::s(what), Value::i(0), Value::i(0)});
    };

    if(is_dir(path))
        folder = path;
    else
    {
        std::vector<uint8_t> zip;
        if(!read_all(path, zip, kMaxSongBytes, why))
            return fail(why);
        std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
        if(!presetfile::unzip(zip.data(), zip.size(), entries, why, kMaxSongBytes))
            return fail(why == "too big to be a preset file" ? std::string("too big to be a song") : why);
        zip = {};
        const std::vector<uint8_t>* lines_entry = nullptr;
        for(const auto& e : entries)
        {
            if(!e.first.empty() && e.first.back() != '/' && !safe_entry(e.first))
                return fail("a file in it has a bad name: " + e.first);
            if(e.first == "engine.txt")
                lines_entry = &e.second;
        }
        if(!lines_entry)
            return fail("no engine.txt in it");
        if(!engine_lines(std::string(lines_entry->begin(), lines_entry->end())))
            return fail("not engine lines");
        std::string root = songs_dir;
        while(root.size() > 1 && root.back() == '/')
            root.pop_back();
        if(root.empty())
            return fail("no songs folder");
        folder = root + "/" + song_name(path);
        if(!remove_tree(folder) || !make_dirs(folder))
            return fail("can't make " + folder + ": " + std::strerror(errno));
        for(auto& e : entries)
        {
            if(e.first.empty() || e.first.back() == '/')
                continue;
            const std::string out = folder + "/" + e.first;
            const size_t      cut = out.rfind('/');
            if(!make_dirs(out.substr(0, cut)))
                return fail("can't make a folder for " + e.first);
            FILE* f  = std::fopen(out.c_str(), "wb");
            bool  ok = f && std::fwrite(e.second.data(), 1, e.second.size(), f) == e.second.size();
            if(f)
                ok = std::fclose(f) == 0 && ok;
            if(!ok)
                return fail("can't write " + out + ": " + std::strerror(errno));
            e.second = {};
        }
    }

    std::vector<uint8_t> bytes;
    if(!read_all(folder + "/engine.txt", bytes, kMaxSongBytes, why))
        return fail("no engine.txt in " + folder);
    const std::string text(bytes.begin(), bytes.end());
    bytes = {};
    if(!engine_lines(text))
        return fail("not engine lines");
    Json song;
    {
        std::vector<uint8_t> js;
        std::string          ignore;
        if(read_all(folder + "/song.json", js, kMaxSongBytes, ignore))
        {
            const std::string s(js.begin(), js.end());
            JsonReader        r{s};
            song = r.value();
            if(!r.ok)
                song = Json{};
        }
    }

    // Nothing plays during the load, and blocks must be running for the
    // mounts to land.
    if(!next_block())
        return fail("the engine isn't running");
    clear_song();
    scan_song_mounts(text);
    song_folder = folder;

    static JNIEnv          env;
    lines::Replay          replay;
    std::vector<std::string> problems;
    bool                     stalled = false;
    replay.env       = &env;
    replay.root      = folder;
    replay.log       = [&](const std::string& p) {
        std::fprintf(stderr, "acidulous: song: %s\n", p.c_str());
        if(problems.size() < 8)
            problems.push_back(p);
    };
    // A block after every line, as acidulous-song does: mounts apply at the
    // top of a block, LoadNexusPatch and BuildCloud want their machine in
    // place, and the 512-entry param queue never fills.
    replay.afterLine = [&](int) {
        if(!stalled && !next_block())
            stalled = true;
    };
    const bool read = lines::replay(text, replay);
    jniweb::arena().clear();
    wait_applied();
    for(const auto& p : problems)
        error("song: " + p);
    if(!read)
    {
        clear_song();
        return fail(problems.empty() ? std::string("not engine lines") : problems.front());
    }

    // The app's tracks, in rack order.
    const Json* tracks = song.get("tracks");
    Args        list;
    int         count = 0;
    for(int r = 0; r < kParts; ++r)
    {
        const Json* track = tracks && tracks->kind == Json::Array && size_t(r) < tracks->items.size()
                                ? &tracks->items[size_t(r)]
                                : nullptr;
        read_back(r, track);
        if(parts[r].machine.empty())
            continue;
        ++count;
        list.push_back(Value::i(r));
        list.push_back(Value::s(parts[r].machine));
        list.push_back(Value::s(parts[r].patch));
        list.push_back(Value::s(track ? track->text("name") : std::string()));
    }
    for(const auto& s : song_sends)
        if(s.first >= 0 && s.first < kSends)
            send_mounted[s.first] = !s.second.empty();
    Args out{Value::s(path), Value::s(folder), Value::s(stalled ? "audio stopped during the load" : ""),
             Value::i(replay.skipped), Value::i(count)};
    out.insert(out.end(), list.begin(), list.end());
    reply("/song_loaded", out);
}

// Sets part [r]'s state from what the rack holds after a song, and sends what
// a /patch would: /mounted, /macro_names, /macros and /graph for Nexus,
// /values, /patch, /sends, /inserts and /mods. [track] is the song.json
// track for the rack, or null; its patch name, Nexus graph and Formulate
// program are taken when its machine is the rack's.
void Core::read_back(int r, const void* track_ptr)
{
    const Json* track   = static_cast<const Json*>(track_ptr);
    const char* mounted = host.mountedMachine(r);
    parts[r]            = Part{};
    parts[r].machine    = mounted ? mounted : "";
    patch_fx[r]         = PatchTrackState{};
    for(int s = 0; s < kSends; ++s)
    {
        const float v    = host.paramNormalized(r, "channel", s == 0 ? "sendreverb" : "senddelay");
        part_sends[r][s] = v < 0.f ? 0.f : v;
    }
    for(int s = 0; s < kModSlots; ++s)
    {
        const size_t k     = size_t(r) * kModSlots + size_t(s);
        mod_types[r][s]    = k < song_mods.size() ? song_mods[k] : std::string();
        mod_bypass[r][s]   = !mod_types[r][s].empty() && host.paramNormalized(r, kModUnitNames[s], "bypass") >= 0.5f;
    }
    const std::string& machine = parts[r].machine;
    if(!machine.empty())
    {
        int32_t         count = 0;
        const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
        for(int32_t i = 0; i < count; ++i)
        {
            const float v = host.paramNormalized(r, "machine", defs[i].name);
            parts[r].values.push_back(v >= 0.f ? v : defs[i].unmap(defs[i].def));
        }
        const Json* m = track ? track->get("machine") : nullptr;
        if(m && m->text("type") == machine)
        {
            const Json* settings = m->get("settings");
            if(settings)
            {
                parts[r].patch = settings->text("patch_name");
                if(machine == "Nexus")
                    parts[r].nexus = settings->text("nexus");
                if(machine == "Formulate")
                {
                    for(const char* key : {"formula", "arp", "duty", "vol"})
                    {
                        const std::string v = settings->text(key);
                        if(!v.empty())
                            parts[r].program_settings.emplace_back(key, v);
                    }
                    parts[r].program = !parts[r].program_settings.empty();
                }
            }
        }
        reply("/mounted", {Value::i(r), Value::s(machine)});
        send_macro_names(r);
        if(machine == "Nexus" && !parts[r].nexus.empty())
        {
            std::string why;
            std::unique_ptr<machine::nexus::Graph> g(
                machine::nexus::Graph::parse(parts[r].nexus, float(kSampleRate), why));
            if(g)
            {
                Args macros = nexus_macros(*g, {});
                if(!macros.empty())
                {
                    Args out{Value::i(r), Value::s(machine), Value::s(parts[r].patch)};
                    out.insert(out.end(), macros.begin(), macros.end());
                    reply("/macros", out);
                }
            }
        }
        Args values{Value::i(r), Value::s(machine), Value::s(parts[r].patch)};
        for(float v : parts[r].values)
            values.push_back(Value::f(v));
        reply("/values", values);
        reply("/patch", {Value::i(r), Value::s(machine), Value::s(parts[r].patch)});
        if(machine == "Nexus")
            send_graph(r);
    }
    reply("/sends", {Value::i(r), Value::f(part_sends[r][0]), Value::f(part_sends[r][1])});
    const PatchInsert* none[kEffectSlots] = {};
    send_inserts(r, none, true);
    send_mods(r);
}

} // namespace core
