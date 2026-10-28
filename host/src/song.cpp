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
#include <sequencer/Transport.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <map>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace core
{

using namespace acidulous;

namespace
{

// The most a song zip may hold, unpacked, and the most the zip itself may
// be. The zip is mapped from disk, not read into memory, but unzip holds
// every entry inflated at once until each is written out, so 128 MB leaves
// room for scsynth and matron on a 1 GB norns. That's about five and a half
// minutes of frozen clips (float32 stereo at 48 kHz).
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
                char c = line[i++];
                if(c == '\\' && i < line.size())
                {
                    c = line[i++];
                    if(c == 'n')
                        c = '\n';
                    else if(c == 'r')
                        c = '\r';
                    else if(c == 't')
                        c = '\t';
                    else if(c == 'x' && i + 2 <= line.size())
                    {
                        c = char(std::strtol(line.substr(i, 2).c_str(), nullptr, 16));
                        i += 2;
                    }
                }
                w += c;
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

std::string mb(int64_t bytes)
{
    char text[32];
    const double m = double(bytes) / (1 << 20);
    std::snprintf(text, sizeof text, m < 10 ? "%.1f MB" : "%.0f MB", m);
    return text;
}

// [path], from the current folder if it's relative, with links resolved as
// far as it exists and the rest as written. "" if it has a "." or ".." in it.
std::string canonical(const std::string& given)
{
    if(given.empty())
        return "";
    std::string path = given;
    if(path[0] != '/')
    {
        char cwd[4096];
        if(!getcwd(cwd, sizeof cwd))
            return "";
        path = std::string(cwd) + "/" + path;
    }
    for(size_t at = 0; at < path.size();)
    {
        size_t end = path.find('/', at + 1);
        if(end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(at + 1, end - at - 1);
        if(part == "." || part == "..")
            return "";
        at = end;
    }
    std::string head = path, tail;
    while(head.size() > 1 && head.back() == '/')
        head.pop_back();
    for(;;)
    {
        if(char* real = realpath(head.c_str(), nullptr))
        {
            std::string out = real;
            std::free(real);
            return out == "/" ? (tail.empty() ? out : tail) : out + tail;
        }
        const size_t slash = head.rfind('/');
        if(slash == std::string::npos || head == "/")
            return path;
        tail = head.substr(slash) + tail;
        head = slash == 0 ? std::string("/") : head.substr(0, slash);
    }
}

// Whether [path] is [dir] or inside it, both canonical.
bool inside(const std::string& path, const std::string& dir)
{
    return !dir.empty() && (path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                                            (dir.back() == '/' || path[dir.size()] == '/')));
}

// A file mapped read-only, so its pages come from the disk cache and can be
// dropped under memory pressure, where a copy couldn't.
struct Mapped
{
    const uint8_t* data = nullptr;
    size_t         size = 0;
    ~Mapped()
    {
        if(data)
            munmap(const_cast<uint8_t*>(data), size);
    }
    bool open(const std::string& path, size_t max, std::string& why)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0)
        {
            why = std::string("can't read ") + path + ": " + std::strerror(errno);
            return false;
        }
        struct stat st{};
        if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        {
            ::close(fd);
            why = "can't read " + path;
            return false;
        }
        if(size_t(st.st_size) > max)
        {
            ::close(fd);
            why = "too big to be a song";
            return false;
        }
        void* p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if(p == MAP_FAILED)
        {
            why = std::string("can't read ") + path + ": " + std::strerror(errno);
            return false;
        }
        data = static_cast<const uint8_t*>(p);
        size = size_t(st.st_size);
        return true;
    }
};

uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

// What a zip's entries add up to inflated, from its central directory, or -1
// if it can't be read. unzip checks it again as it goes.
int64_t inflated_size(const uint8_t* p, size_t n)
{
    if(n < 22)
        return -1;
    size_t       end  = n - 22;
    const size_t stop = n > 22 + 0xffff ? n - 22 - 0xffff : 0;
    while(le32(p + end) != 0x06054b50u)
    {
        if(end == stop)
            return -1;
        --end;
    }
    const uint32_t count = le16(p + end + 10);
    size_t         at    = le32(p + end + 16);
    int64_t        total = 0;
    for(uint32_t k = 0; k < count; ++k)
    {
        if(at + 46 > end || le32(p + at) != 0x02014b50u)
            return -1;
        total += le32(p + at + 24);
        at += 46 + le16(p + at + 28) + le16(p + at + 30) + le16(p + at + 32);
    }
    return total;
}

// The audio a song's lines load, decoded, as the core's /load counts it:
// per rack and held slot (a Forage pad's own slot, slot 0 for the one
// sample, SoundFont or map a machine holds), and frozen clips, which belong
// to no part, by rack and scene. A machine mounted on a rack frees what
// the rack held. Paths are resolved as the replay does.
struct SongAudio
{
    std::map<int, std::map<int, int64_t>>     racks;
    std::map<std::pair<int, std::string>, int64_t> frozen;

    int64_t rack_total(int r) const
    {
        int64_t sum = 0;
        auto    it  = racks.find(r);
        if(it != racks.end())
            for(const auto& kv : it->second)
                sum += kv.second;
        return sum;
    }
    int64_t frozen_total() const
    {
        int64_t sum = 0;
        for(const auto& kv : frozen)
            sum += kv.second;
        return sum;
    }
    int64_t total() const
    {
        int64_t sum = frozen_total();
        for(const auto& kv : racks)
            sum += rack_total(kv.first);
        return sum;
    }
};

SongAudio song_audio(const std::string& text, const std::string& root, int held_slots)
{
    SongAudio  out;
    const auto resolve = [&](const std::string& p) {
        return p.compare(0, 4, "zip:") == 0 ? root + "/" + p.substr(4) : p;
    };
    const auto file_size = [](const std::string& p) {
        struct stat st{};
        return stat(p.c_str(), &st) == 0 ? int64_t(st.st_size) : int64_t(0);
    };
    // The paths in a spec's fields numbered [fields], resolved.
    const auto spec_paths = [&](const std::string& spec, std::initializer_list<int> fields) {
        std::vector<std::string> paths;
        std::string              field;
        int                      index = 0;
        for(size_t k = 0; k <= spec.size(); ++k)
        {
            const char c = k == spec.size() ? '\n' : spec[k];
            if(c != '|' && c != '\n')
            {
                field += c;
                continue;
            }
            for(int f : fields)
                if(f == index && !field.empty())
                    paths.push_back(resolve(field));
            index = c == '|' ? index + 1 : 0;
            field.clear();
        }
        return paths;
    };
    size_t at = 0;
    while(at < text.size())
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at                     = end + 1;
        if(line.compare(0, 4, "Load") != 0 && line.compare(0, 5, "Mount") != 0 && line.compare(0, 11, "ClearFrozen") != 0)
            continue;
        const auto w = words(line);
        if(w.empty())
            continue;
        const auto n    = [&](size_t k) { return k < w.size() ? std::atoi(w[k].c_str()) : -1; };
        const auto slot = [&](int s) { return s < 0 ? 0 : s >= held_slots ? held_slots - 1 : s; };
        const int  rack = n(1);
        const std::string& call = w[0];
        if(call == "ClearFrozen")
            out.frozen.clear();
        else if(call == "MountMachine" && w.size() >= 3)
            out.racks.erase(rack);
        else if(call == "MountFrozen" && w.size() >= 4)
            out.frozen[{rack, w[2]}] = decoded_bytes(resolve(w[3]), 0, 0, true);
        else if(call == "LoadSample" && w.size() >= 5)
            out.racks[rack][slot(n(2))] = decoded_bytes(resolve(w[3]), kSampleRate, n(4) > 0 ? n(4) : 0, false);
        else if(call == "LoadSoundFont" && w.size() >= 3)
            out.racks[rack][0] = file_size(resolve(w[2])) * 2;
        else if(call == "LoadZoneMap" && w.size() >= 3)
        {
            int64_t sum = 0;
            for(const auto& p : spec_paths(w[2], {0}))
                sum += decoded_bytes(p, 0, 0, false);
            out.racks[rack][0] = sum;
        }
        else if((call == "LoadTake" || call == "LoadUtterance") && w.size() >= 3)
            out.racks[rack][0] = decoded_bytes(resolve(w[2]), kSampleRate, 0, true);
        else if(call == "LoadReel" && w.size() >= 3)
        {
            int64_t sum = 0;
            for(const auto& p : spec_paths(w[2], {2}))
                sum += decoded_bytes(p, kSampleRate, 0, true);
            out.racks[rack][1] = sum;
        }
        else if(call == "LoadVoice" && w.size() >= 4)
        {
            int64_t sum = 0;
            for(const auto& p : spec_paths(w[3], {2, 3}))
                sum += decoded_bytes(p, 0, 0, false);
            out.racks[rack][slot(n(2))] = sum;
        }
    }
    for(auto it = out.racks.begin(); it != out.racks.end();)
        it = it->first < 0 || it->first >= kParts ? out.racks.erase(it) : std::next(it);
    return out;
}

// Writes [bytes] to [path], false with [why] set if it couldn't.
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string& why)
{
    FILE* f  = std::fopen(path.c_str(), "wb");
    bool  ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if(f)
        ok = std::fclose(f) == 0 && ok;
    if(!ok)
        why = "can't write " + path + ": " + std::strerror(errno);
    return ok;
}

} // namespace

// Notes what a mount line the engine took put in place, for what EngineHost
// doesn't say back: modifiers, sends, master and group inserts. The last
// line for a slot wins.
void Core::note_song_mount(const std::string& line)
{
    if(line.compare(0, 5, "Mount") != 0)
        return;
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
// to the core's 1.0). With a song loaded, the sends, tempo, swing unit and
// loop-song go back to what the plugin had before it. Replies /mounted ""
// and, unless [quiet], one /inserts for each part, as an unmount does.
// Worker thread.
void Core::clear_song(bool quiet)
{
    const bool had_song = !song_folder.empty();
    host.transportStop();
    host.panic();
    const int64_t empty = host.snapshotBegin();
    if(empty)
        host.snapshotCommit(empty);
    host.clearFrozen();
    quiet_inserts = quiet;
    for(int p = 0; p < kParts; ++p)
    {
        // The song's inserts go first, so the unmount's /inserts shows the
        // slots empty.
        for(int s = 0; s < kEffectSlots; ++s)
            if(*host.mountedEffect(p, s))
                host.mountEffect(p, s, "");
        patch_fx[p]      = PatchTrackState{};
        part_sends[p][0] = part_sends[p][1] = 0.f;
        clear_mods(p);
        unmount(p);
        host.setTuning(p, nullptr);
    }
    quiet_inserts = false;
    song_defaults(false);
    if(had_song)
        restore_sends();
    {
        std::lock_guard<std::mutex> l(held_lock);
        song_held = 0;
    }
    song_master_fx.clear();
    song_group_fx.clear();
    song_mods.clear();
    song_sends.clear();
    song_folder.clear();
    song_scenes.clear();
    song_on.store(false);
    wait_applied();
}

// The sends back to the plugin's own: a song's lines may have mounted others
// or set their params, so each comes off and the ones the plugin had go back
// on fresh, with the values the script set.
void Core::restore_sends()
{
    for(int s = 0; s < kSends; ++s)
    {
        if(send_mounted[s] || !song_sends.empty())
            host.mountSend(s, "");
        send_mounted[s] = false;
        if(pre_song_send[s])
            ensure_send(s);
    }
}

// Back to an empty engine's: the master and group inserts the song mounted
// go, and every track's tuning, channel strip and the master's and perform's
// params return to their defaults (the master volume to the core's 1.0), as
// do the tempo (to the plugin's before the song), the swing unit and
// loop-song (the plugin never sets them, so the engine's defaults:
// sixteenths and on). [now] sets params without waiting on a full queue,
// for the destructor.
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
    if(!song_folder.empty())
    {
        host.setTempo(pre_song_tempo);
        host.setSwingUnit(0);
        host.setLoopSong(true);
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
    song_scenes.clear();
    song_on.store(false);
}

void Core::unload_song()
{
    if(!song_folder.empty())
        clear_song(false);
    reply("/song_unloaded", {});
}

namespace
{
// Thrown out of the replay when blocks stop coming.
struct Stalled
{
    int line;
};
} // namespace

// Unpacks [path] (a song zip, or a folder holding an unpacked one) into
// songs_dir/<name>/, empties the engine, replays engine.txt and reads every
// rack back. Replies /song_loaded. Worker thread, with every part pending.
// Anything refused before the replay leaves what's loaded, and the song's
// old folder, as they were.
void Core::load_song(const std::string& path, const std::string& songs_dir_in)
{
    std::string folder, staged;
    std::string why;
    const auto  fail = [&](const std::string& what) {
        if(!staged.empty())
            remove_tree(staged);
        error("song: " + what);
        reply("/song_loaded", {Value::s(path), Value::s(folder), Value::s(what), Value::i(0), Value::i(0)});
    };

    // The songs folder: the default, or one under dust/data.
    const char*       home_env = std::getenv("HOME");
    const std::string home     = home_env && *home_env ? home_env : "/home/we";
    std::string       root     = songs_dir_in.empty() ? home + "/dust/data/acidulous/songs" : songs_dir_in;
    while(root.size() > 1 && root.back() == '/')
        root.pop_back();
    if(!songs_dir_in.empty())
    {
        const std::string real = canonical(root), data = canonical(home + "/dust/data");
        if(real.empty() || !inside(real, data) || real == data)
            return fail("the songs folder must be in " + home + "/dust/data: " + songs_dir_in);
        root = real;
    }

    // Where the files are read from for the budget and the replay: the
    // folder given, or the zip unpacked into a staging folder beside the
    // song's, renamed into place once the song is known to fit.
    std::string from;
    if(is_dir(path))
        folder = from = path;
    else
    {
        folder                 = root + "/" + song_name(path);
        const std::string real = canonical(path);
        if(real.empty())
            return fail("can't read " + path);
        if(inside(real, canonical(folder)))
            return fail("the zip is inside the folder it would unpack into");
        std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
        {
            Mapped zip;
            if(!zip.open(path, kMaxSongBytes, why))
                return fail(why);
            // Unpacking holds every entry at once; if that beside what's
            // loaded would go over the sample budget, the old song goes
            // first.
            const int64_t inflated = inflated_size(zip.data, zip.size);
            if(inflated < 0)
                return fail("not a zip");
            if(inflated > int64_t(kMaxSongBytes))
                return fail("too big to be a song");
            if(held_total() + inflated > sample_budget_bytes())
            {
                std::fprintf(stderr, "acidulous: song: unloading the parts first to unpack %s\n", path.c_str());
                clear_song(false);
            }
            if(!presetfile::unzip(zip.data, zip.size, entries, why, kMaxSongBytes))
                return fail(why == "too big to be a preset file" ? std::string("too big to be a song") : why);
        }
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
        if(!make_dirs(root))
            return fail("can't make " + root + ": " + std::strerror(errno));
        staged = root + "/." + song_name(path) + ".unpacking";
        if(!remove_tree(staged) || mkdir(staged.c_str(), 0755) != 0)
        {
            const std::string what = "can't make " + staged + ": " + std::strerror(errno);
            staged.clear();
            return fail(what);
        }
        from = staged;
        for(auto& e : entries)
        {
            if(e.first.empty() || e.first.back() == '/')
                continue;
            const std::string out = staged + "/" + e.first;
            if(!make_dirs(out.substr(0, out.rfind('/'))))
                return fail("can't make a folder for " + e.first);
            if(!write_file(out, e.second, why))
                return fail(why);
            e.second = {};
            e.second.shrink_to_fit();
        }
    }

    std::vector<uint8_t> bytes;
    if(!read_all(from + "/engine.txt", bytes, kMaxSongBytes, why))
        return fail("no engine.txt in " + folder);
    const std::string text(bytes.begin(), bytes.end());
    bytes = {};
    if(!engine_lines(text))
        return fail("not engine lines");

    // The song's audio against the sample budget, before anything goes.
    const SongAudio audio = song_audio(text, from, kHeldSlots);
    if(audio.total() > sample_budget_bytes())
        return fail("its samples and frozen clips need " + mb(audio.total()) + ", over the " +
                    mb(sample_budget_bytes()) + " for samples");

    // Nothing plays during the load, and blocks must be running for the
    // mounts to land.
    if(!next_block())
        return fail("the engine isn't running");

    if(!staged.empty())
    {
        // The old folder goes only once the new one is in place.
        const std::string old = root + "/." + song_name(path) + ".old";
        remove_tree(old);
        const bool had = is_dir(folder);
        if(had && std::rename(folder.c_str(), old.c_str()) != 0)
            return fail("can't replace " + folder + ": " + std::strerror(errno));
        if(std::rename(staged.c_str(), folder.c_str()) != 0)
        {
            const std::string what = "can't make " + folder + ": " + std::strerror(errno);
            if(had)
                std::rename(old.c_str(), folder.c_str());
            return fail(what);
        }
        staged.clear();
        remove_tree(old);
    }

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

    clear_song(true);
    pre_song_tempo = host.tempo();
    // The song's tempo rules from here: the script's /tempo and /beat are
    // ignored until it goes, and a beat being followed is let go.
    {
        std::lock_guard<std::mutex> l(beat_lock);
        song_on.store(true);
        host.releaseBeat();
    }
    for(int s = 0; s < kSends; ++s)
        pre_song_send[s] = send_mounted[s];
    song_mods.assign(size_t(kParts) * kModSlots, std::string());
    song_folder = folder;

    // Each line's text by number, for noting the mounts that landed.
    std::vector<std::pair<size_t, size_t>> line_at;
    for(size_t at = 0; at <= text.size();)
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        line_at.emplace_back(at, end - at);
        at = end + 1;
    }

    static JNIEnv            env;
    lines::Replay            replay;
    std::vector<std::string> problems;
    replay.env  = &env;
    replay.root = folder;
    replay.log  = [&](const std::string& p) {
        std::fprintf(stderr, "acidulous: song: %s\n", p.c_str());
        if(problems.size() < 8)
            problems.push_back(p);
    };
    // A block after every line, as acidulous-song does: mounts apply at the
    // top of a block, LoadNexusPatch and BuildCloud want their machine in
    // place, and the 512-entry param queue never fills. If blocks stop, the
    // replay stops there.
    int refused = 0, skipped = 0;
    replay.afterLine = [&](int n) {
        if(replay.refused == refused && replay.skipped == skipped && n >= 1 && size_t(n) <= line_at.size())
            note_song_mount(text.substr(line_at[size_t(n) - 1].first, line_at[size_t(n) - 1].second));
        refused = replay.refused;
        skipped = replay.skipped;
        if(!next_block())
            throw Stalled{n};
    };
    bool read    = false;
    int  stalled = 0;
    try
    {
        read = lines::replay(text, replay);
    }
    catch(const Stalled& s)
    {
        stalled = s.line;
    }
    jniweb::arena().clear();
    for(const auto& p : problems)
        error("song: " + p);
    if(stalled)
    {
        clear_song(false);
        return fail("the audio stopped at line " + std::to_string(stalled) + " of engine.txt; nothing is loaded");
    }
    wait_applied();
    if(!read)
    {
        clear_song(false);
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
    // What the song's audio holds, so a later /load sees it: a rack's own
    // while it keeps its machine, frozen clips until the song goes.
    {
        std::lock_guard<std::mutex> l(held_lock);
        for(const auto& kv : audio.racks)
            if(!parts[kv.first].machine.empty())
                for(const auto& slot : kv.second)
                    held[kv.first][slot.first] = slot.second;
        song_held = audio.frozen_total();
    }
    for(const auto& s : song_sends)
        if(s.first >= 0 && s.first < kSends)
            send_mounted[s.first] = !s.second.empty();
    read_scenes(text, &song);
    send_scenes();
    Args out{Value::s(path), Value::s(folder), Value::s(""), Value::i(replay.skipped), Value::i(count)};
    out.insert(out.end(), list.begin(), list.end());
    reply("/song_loaded", out);
}

// The scenes of the last committed snapshot in [text], in order, which is
// the transport's scene index, named from song.json's scenes in the same
// order (the app pushes them in that order). Worker thread.
void Core::read_scenes(const std::string& text, const void* song_ptr)
{
    const Json*            song = static_cast<const Json*>(song_ptr);
    std::vector<SongScene> building, done;
    for(size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if(end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at                     = end + 1;
        if(line.compare(0, 8, "Snapshot") != 0)
            continue;
        const auto w = words(line);
        if(w.empty())
            continue;
        if(w[0] == "SnapshotBegin")
            building.clear();
        else if(w[0] == "SnapshotAddScene" && w.size() >= 5)
        {
            SongScene sc;
            sc.engine_id = std::strtoll(w[2].c_str(), nullptr, 10);
            sc.repeat    = std::max(1, std::atoi(w[4].c_str()));
            building.push_back(sc);
        }
        else if(w[0] == "SnapshotCommit")
            done = building;
    }
    const Json* scenes = song ? song->get("scenes") : nullptr;
    for(size_t k = 0; k < done.size(); ++k)
    {
        const Json* sc = scenes && scenes->kind == Json::Array && k < scenes->items.size() ? &scenes->items[k] : nullptr;
        if(sc)
        {
            done[k].name = sc->text("name");
            done[k].id   = sc->text("id");
        }
        if(done[k].name.empty())
            done[k].name = "scene " + std::to_string(k + 1);
    }
    song_scenes = std::move(done);
}

void Core::send_scenes()
{
    Args out{Value::i(int(song_scenes.size()))};
    for(const auto& sc : song_scenes)
    {
        out.push_back(Value::s(sc.name));
        out.push_back(Value::s(sc.id));
        out.push_back(Value::s(std::to_string(sc.engine_id)));
        out.push_back(Value::i(sc.repeat));
    }
    reply("/song_scenes", out);
}

// Queues a scene to follow the current one's repeats: by index, or by
// song.json id or engine id as a string. -1 cancels. Worker thread.
void Core::queue_scene(const Value& which)
{
    if(song_folder.empty())
        return error("song: no song loaded");
    int idx = -1;
    if(which.type == 's')
    {
        for(size_t k = 0; k < song_scenes.size() && idx < 0; ++k)
            if(which.str == song_scenes[k].id || which.str == std::to_string(song_scenes[k].engine_id))
                idx = int(k);
        if(idx < 0)
            return error("song: no scene " + which.str);
    }
    else
    {
        idx = int(which.num);
        if(idx >= int(song_scenes.size()))
            return error("song: no scene " + std::to_string(idx));
        if(idx < 0)
            idx = -1;
    }
    host.queueScene(idx);
}

bool Core::song_position(Args& out) const
{
    if(!song_on.load())
        return false;
    const auto    p    = seq::Transport::unpack(host.positionPacked());
    const int64_t tick = std::min<int64_t>(p.tickInIteration, INT32_MAX);
    out = {Value::i(host.isPlaying() ? 1 : 0), Value::i(p.scene), Value::i(p.repeat), Value::i(int(tick)),
           Value::i(host.queuedScene()), Value::f(host.tempo())};
    return true;
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
