// Loads the app's Squelch song (host/tests/songs/squelch.zip) through the
// core's /song_load, with the worker thread and a fake audio thread as the
// plugin runs, and checks:
// - the zip is unpacked into the songs folder and every rack is read back:
//   machine, patch name, params (against engine.txt's SetParam lines and the
//   engine), sends, inserts and their values, modifiers, macro names
// - a message for a part sent after /song_load waits for it
// - it plays when the transport runs
// - loading the unpacked folder again, /song_unload back to empty, a missing
//   file, a zip without engine.txt and a zip with a "../" entry
// - a songs folder outside dust/data, a zip inside its own folder and a
//   failed unpack are refused and leave the old folder and song
// - a song's samples and frozen clips count in the sample budget: one over it
//   is refused, and a /load after one sees what it holds
// - audio stopping mid-replay stops it with an error
// - /song_unload puts the tempo back to the plugin's (EngineHost has no
//   getter for a send's params)
// - /song_scenes, /song_play, /song_queue, /song_stop and /song_position,
//   and that /tempo and /beat leave a loaded song's tempo alone
// - a failed load after the parts were emptied loads the old song again
//   (its /song_loaded with the zip it came from), or says /song_unloaded 1;
//   a teardown without audio finishes once it's back, with the script's
//   master volume; Mosaic zones and the master volume are read back
// - a teardown that stalls before a replay that runs leaves the new song
//   playing, and a /master while it's loaded moves only the song's master
// - /strip gives a song part's gain and pan, and /preset_save is refused
//   while a song is loaded
// - deleting the core with a song loaded
// Usage: acidulous-song-test BANK_DIR SCRATCH_DIR SONG_ZIP
#include "core.h"

#include <EngineHost.h>
#include <engine/effect/EffectRegistry.h>
#include <engine/machine/MachineRegistry.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using core::Value;

namespace
{

struct Reply
{
    std::string path;
    core::Args  args;
};

struct Log : core::Replier
{
    std::mutex              lock;
    std::condition_variable cv;
    std::vector<Reply>      got;
    void reply(const char* path, const core::Args& args) override
    {
        std::lock_guard<std::mutex> l(lock);
        got.push_back({path, args});
        cv.notify_all();
    }
    void clear()
    {
        std::lock_guard<std::mutex> l(lock);
        got.clear();
    }
    bool wait(const char* path, Reply& out, std::chrono::seconds limit = 120s)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, limit, [&] {
            for(const auto& r : got)
                if(r.path == path)
                {
                    out = r;
                    return true;
                }
            return false;
        });
    }
    // The last reply to [path] for [part] (its first arg).
    bool last(const char* path, int part, Reply& out)
    {
        std::lock_guard<std::mutex> l(lock);
        bool found = false;
        for(const auto& r : got)
            if(r.path == path && !r.args.empty() && r.args[0].type == 'i' && int(r.args[0].num) == part)
                out = r, found = true;
        return found;
    }
    // Waits for a reply to [path] for [part].
    bool wait_part(const char* path, int part, std::chrono::seconds limit = 60s)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, limit, [&] {
            for(const auto& r : got)
                if(r.path == path && !r.args.empty() && r.args[0].type == 'i' && int(r.args[0].num) == part)
                    return true;
            return false;
        });
    }
    // How many replies to [path] there are for [part].
    int count(const char* path, int part)
    {
        std::lock_guard<std::mutex> l(lock);
        int n = 0;
        for(const auto& r : got)
            if(r.path == path && !r.args.empty() && r.args[0].type == 'i' && int(r.args[0].num) == part)
                ++n;
        return n;
    }
    // Waits for the [n]th reply to [path], 1 the first.
    bool wait_nth(const char* path, int n, Reply& out, std::chrono::seconds limit = 120s)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, limit, [&] {
            int seen = 0;
            for(const auto& r : got)
                if(r.path == path && ++seen == n)
                {
                    out = r;
                    return true;
                }
            return false;
        });
    }
    // Where the first reply to [path] is in the log, or -1.
    int index(const char* path)
    {
        std::lock_guard<std::mutex> l(lock);
        for(size_t i = 0; i < got.size(); ++i)
            if(got[i].path == path)
                return int(i);
        return -1;
    }
};

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if(!ok)
        ++failures;
}

// Arg [k]'s string, "" when there's none.
std::string str(const Reply& r, size_t k)
{
    return k < r.args.size() ? r.args[k].str : std::string();
}

bool exists(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::string read_text(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A zip of stored entries, enough for the refusal tests.
uint32_t crc32(const std::string& s)
{
    uint32_t c = 0xffffffffu;
    for(unsigned char b : s)
    {
        c ^= b;
        for(int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void put(std::string& o, uint32_t v, int bytes)
{
    for(int k = 0; k < bytes; ++k)
        o += char((v >> (8 * k)) & 0xff);
}

void write_zip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files)
{
    std::string out, dir;
    for(const auto& f : files)
    {
        const uint32_t at = uint32_t(out.size()), crc = crc32(f.second), n = uint32_t(f.second.size());
        put(out, 0x04034b50u, 4), put(out, 20, 2), put(out, 0, 2), put(out, 0, 2), put(out, 0, 4);
        put(out, crc, 4), put(out, n, 4), put(out, n, 4), put(out, uint32_t(f.first.size()), 2), put(out, 0, 2);
        out += f.first + f.second;
        put(dir, 0x02014b50u, 4), put(dir, 20, 2), put(dir, 20, 2), put(dir, 0, 2), put(dir, 0, 2), put(dir, 0, 4);
        put(dir, crc, 4), put(dir, n, 4), put(dir, n, 4), put(dir, uint32_t(f.first.size()), 2), put(dir, 0, 2);
        put(dir, 0, 2), put(dir, 0, 2), put(dir, 0, 2), put(dir, 0, 4), put(dir, at, 4);
        dir += f.first;
    }
    const uint32_t dir_at = uint32_t(out.size());
    out += dir;
    put(out, 0x06054b50u, 4), put(out, 0, 2), put(out, 0, 2), put(out, uint32_t(files.size()), 2);
    put(out, uint32_t(files.size()), 2), put(out, uint32_t(dir.size()), 4), put(out, dir_at, 4), put(out, 0, 2);
    std::ofstream(path, std::ios::binary) << out;
}

// A 16-bit mono WAV at 48 kHz of [frames] frames of a quiet tone.
std::string wav(int frames)
{
    std::string o = "RIFF";
    put(o, uint32_t(36 + frames * 2), 4);
    o += "WAVEfmt ";
    put(o, 16, 4), put(o, 1, 2), put(o, 1, 2), put(o, 48000, 4), put(o, 96000, 4), put(o, 2, 2), put(o, 16, 2);
    o += "data";
    put(o, uint32_t(frames * 2), 4);
    for(int i = 0; i < frames; ++i)
        put(o, uint32_t(uint16_t(int16_t(std::lround(1000.0 * std::sin(i * 0.05))))), 2);
    return o;
}

struct Line
{
    int         rack;
    std::string unit, name;
    float       value;
};

// engine.txt's SetParam lines, the last value for each rack, unit and name.
std::vector<Line> set_params(const std::string& text)
{
    std::vector<Line>  out;
    std::istringstream in(text);
    std::string        line;
    while(std::getline(in, line))
    {
        if(line.compare(0, 9, "SetParam ") != 0)
            continue;
        Line l;
        char unit[64] = {}, name[64] = {};
        if(std::sscanf(line.c_str(), "SetParam %d \"%63[^\"]\" \"%63[^\"]\" %f", &l.rack, unit, name, &l.value) != 4)
            continue;
        l.unit = unit, l.name = name;
        bool replaced = false;
        for(auto& o : out)
            if(o.rack == l.rack && o.unit == l.unit && o.name == l.name)
                o = l, replaced = true;
        if(!replaced)
            out.push_back(l);
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 4)
    {
        std::fprintf(stderr, "usage: acidulous-song-test BANK_DIR SCRATCH_DIR SONG_ZIP\n");
        return 2;
    }
    const std::string banks = argv[1], scratch = std::string(argv[2]) + "/songs-test", zip = argv[3];
    // Songs folders from OSC must be under $HOME/dust/data.
    const std::string songs = scratch + "/dust/data/acidulous/songs";
    std::system(("rm -rf '" + scratch + "' && mkdir -p '" + scratch + "/dust/data'").c_str());
    setenv("HOME", scratch.c_str(), 1);
    auto& host = acidulous::EngineHost::instance();
    Log   log;
    auto* c = new core::Core(banks, &log, false);

    std::atomic<bool>  go{true}, paused{false};
    std::atomic<float> peak{0.f};
    std::mutex         peak_mx;  // a block is rendered and counted whole, so a reset can't be overwritten by an older one
    std::thread        audio([&] {
        float l[64], r[64];
        while(go)
        {
            if(paused)
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(peak_mx);
                c->process(nullptr, nullptr, l, r, 64);
                float p = peak.load();
                for(int i = 0; i < 64; ++i)
                    p = std::max(p, std::max(std::fabs(l[i]), std::fabs(r[i])));
                peak.store(p);
            }
            std::this_thread::sleep_for(300us);
        }
    });
    const auto cmd = [&](const char* path, core::Args a) { c->command(path, a); };
    const auto load = [&](const std::string& path, Reply& r, const std::string& frozen = "") {
        log.clear();
        if(frozen.empty())
            cmd("/song_load", {Value::s(path), Value::s(songs)});
        else
            cmd("/song_load", {Value::s(path), Value::s(songs), Value::s(frozen)});
        return log.wait("/song_loaded", r) && r.args.size() >= 5;
    };
    const auto state = [&](int part, Reply& r) {
        log.clear();
        cmd("/state", {Value::i(part)});
        return log.wait("/state", r, 10s) && r.args.size() >= 4;
    };

    // The plugin's own tempo and Reverb, for the song to change and the
    // unload to put back.
    cmd("/tempo", {Value::f(100.f)});
    cmd("/send", {Value::i(0), Value::s("size"), Value::f(0.7f)});
    // And swing in eighths with loop-song off, which the song changes.
    host.setSwingUnit(1);
    host.setLoopSong(false);
    cmd("/channel", {Value::i(1), Value::s("sendreverb"), Value::f(0.5f)});
    // The script's own master volume, which comes back when a song goes.
    cmd("/master", {Value::s("volume"), Value::f(0.6f)});
    // Something on part 0 for the song to replace, with a modifier.
    cmd("/patch", {Value::i(0), Value::s("Trinity"), Value::s("Saw Lead")});
    cmd("/mod", {Value::i(0), Value::i(0), Value::s("Scale")});
    {
        Reply r;
        state(0, r);
        check(r.args.size() > 1 && r.args[1].str == "Trinity", "part 0 has Trinity before the song");
    }

    // Load it, with a /state for part 3 straight after: it waits for the song.
    Reply loaded;
    log.clear();
    cmd("/song_load", {Value::s(zip), Value::s(songs)});
    cmd("/state", {Value::i(3)});
    const bool got = log.wait("/song_loaded", loaded) && loaded.args.size() >= 5;
    check(got, "/song_loaded came");
    {
        Reply st;
        log.wait("/state", st, 10s);
        check(log.index("/state") > log.index("/song_loaded") && st.args.size() > 1 && st.args[1].str == "Reflux",
              "a /state sent after /song_load waits for it and sees Reflux");
    }
    if(got)
    {
        const auto& a = loaded.args;
        check(a[2].str.empty(), "no error (" + a[2].str + ")");
        check(int(a[3].num) == 0, "no line skipped (" + std::to_string(int(a[3].num)) + ")");
        check(a[1].str == songs + "/squelch", "folder is songs/squelch (" + a[1].str + ")");
        check(int(a[4].num) == 9, "9 racks (" + std::to_string(int(a[4].num)) + ")");
        const char* want[][4] = {
            {"0", "Genesis", "Straight", "Drums"}, {"1", "Hexbeat", "Snug", "Perc"},
            {"2", "Reflux", "Squelch", "Acid"},    {"3", "Reflux", "Wasp", "Acid 2"},
            {"4", "Ratio", "Sync Stab", "Stabs"},  {"5", "Trinity", "Pluck Wide", "Arp"},
            {"6", "Cumulus", "Deep Wash", "Pad"},  {"7", "Nexus", "Subtractive", "Bleeps"},
            {"8", "Trinity", "Noise Sweep", "Riser"},
        };
        bool list_ok = a.size() == 5 + 9 * 4;
        for(int k = 0; list_ok && k < 9; ++k)
            list_ok = int(a[size_t(5 + k * 4)].num) == std::atoi(want[k][0]) && a[size_t(6 + k * 4)].str == want[k][1] &&
                      a[size_t(7 + k * 4)].str == want[k][2] && a[size_t(8 + k * 4)].str == want[k][3];
        check(list_ok, "the rack list: part, machine, patch and track name");
    }
    check(exists(songs + "/squelch/engine.txt") && exists(songs + "/squelch/song.json"), "unpacked into the folder");

    // Every rack against the engine and engine.txt.
    const std::string text  = read_text(songs + "/squelch/engine.txt");
    const auto        lines = set_params(text);
    int               machine_lines = 0, machine_bad = 0, fx_lines = 0, fx_bad = 0;
    for(int r = 0; r < core::kParts; ++r)
    {
        Reply st;
        state(r, st);
        const std::string m = st.args.size() > 1 ? st.args[1].str : "?";
        if(r >= 9)
        {
            check(m.empty() && std::string(host.mountedMachine(r)).empty(), "rack " + std::to_string(r) + " is empty");
            continue;
        }
        check(m == host.mountedMachine(r) && !m.empty(), "rack " + std::to_string(r) + " has " + m);
        Reply values;
        const bool vok = log.last("/values", r, values);
        (void)vok;
        int32_t                    count = 0;
        const acidulous::ParamDef* defs  = acidulous::MachineRegistry::paramDefs(m.c_str(), count);
        // The core's /state values are the engine's; each SetParam line on
        // the machine landed.
        for(const auto& l : lines)
        {
            if(l.rack != r)
                continue;
            if(l.unit == "machine")
            {
                int index = -1;
                for(int32_t i = 0; i < count; ++i)
                    if(l.name == defs[i].name)
                        index = i;
                if(index < 0)
                    continue;
                ++machine_lines;
                const float v = std::min(1.f, std::max(0.f, l.value));
                if(size_t(index + 4) >= st.args.size() || std::fabs(st.args[size_t(index + 4)].num - v) > 1e-4)
                {
                    if(machine_bad++ < 5)
                        std::printf("  rack %d %s: line %g, state %g\n", r, l.name.c_str(), v,
                                    size_t(index + 4) < st.args.size() ? st.args[size_t(index + 4)].num : -1.0);
                }
            }
        }
    }
    check(machine_lines > 300 && machine_bad == 0,
          "machine params match engine.txt (" + std::to_string(machine_lines) + " lines, " +
              std::to_string(machine_bad) + " off)");

    // What the load replied for each rack.
    log.clear();
    for(int r = 0; r < 9; ++r)
        cmd("/mods", {Value::i(r)});
    {
        Reply r;
        log.wait("/mods", r, 10s);
        std::this_thread::sleep_for(200ms);
        Reply m5;
        const bool ok = log.last("/mods", 5, m5) && m5.args.size() > 1;
        // slot 2 of part 5 is the Arp: i:part then per slot i:slot s:type i:bypass i:count ...
        bool arp = false;
        for(size_t k = 1; ok && k + 1 < m5.args.size(); ++k)
            if(m5.args[k].type == 's' && m5.args[k].str == "Arp" && int(m5.args[k - 1].num) == 2)
                arp = true;
        check(arp, "part 5 has the Arp in slot 2");
        Reply m0;
        bool  empty0 = log.last("/mods", 0, m0);
        for(size_t k = 1; empty0 && k < m0.args.size(); ++k)
            if(m0.args[k].type == 's' && !m0.args[k].str.empty())
                empty0 = false;
        check(empty0, "part 0's Scale is gone");
    }
    // The song's replies, sent at load: inserts with the song's values,
    // sends and macro names. Load again from the folder to see them.
    Reply again;
    check(load(songs + "/squelch", again) && str(again, 2).empty() && int(again.args[4].num) == 9,
          "loads again from the unpacked folder");
    {
        Reply m;
        check(log.wait("/song_master", m, 1s) && m.args.size() == 1 && std::fabs(m.args[0].num - 0.42666665) < 1e-5 &&
                  log.index("/song_master") < log.index("/song_loaded"),
              "/song_master gives the song's master volume before /song_loaded");
    }
    {
        Reply ins;
        const bool ok = log.last("/inserts", 2, ins);
        bool       dist = false, delay = false;
        float      drive = -1.f;
        for(size_t k = 1; ok && k < ins.args.size(); ++k)
        {
            if(ins.args[k].type != 's')
                continue;
            dist  = dist || ins.args[k].str == "Distortion";
            delay = delay || ins.args[k].str == "Delay";
            if(ins.args[k].str == "drive" && drive < 0.f && k + 7 < ins.args.size())
                drive = float(ins.args[k + 7].num);
        }
        check(dist && delay, "part 2's inserts are Distortion and Delay");
        // Each slot: slot, type, count, 8 args per param, then the mod
        // params the same way, ending on the last arg.
        int32_t mod_count = 0;
        acidulous::EffectRegistry::paramDefs("~mod", mod_count);
        bool   whole = ok && mod_count > 0;
        size_t k     = 1;
        int    slots = 0;
        while(whole && k < ins.args.size())
        {
            whole = k + 2 < ins.args.size() && ins.args[k].num >= 0 && ins.args[k].num < acidulous::kEffectSlots;
            k += 3 + 8 * size_t(whole ? ins.args[k + 2].num : 0);
            whole = whole && k + 1 < ins.args.size() && int(ins.args[k].num) == mod_count &&
                    ins.args[k + 1].str == "mod_source";
            k += 1 + 8 * size_t(mod_count);
            ++slots;
        }
        check(whole && k == ins.args.size() && slots == 2, "part 2's /inserts has each slot's mod params");
        check(log.count("/inserts", 2) == 1 && log.count("/inserts", 12) == 1, "one /inserts per part for the load");
        for(const auto& l : lines)
            if(l.rack == 2 && l.unit == "effect1" && l.name == "drive")
            {
                ++fx_lines;
                if(std::fabs(drive - l.value) > 1e-4)
                    ++fx_bad;
            }
        check(fx_lines == 1 && fx_bad == 0, "its Distortion's drive is the song's (" + std::to_string(drive) + ")");
        Reply sends;
        check(log.last("/sends", 1, sends) && sends.args.size() >= 3 && std::fabs(sends.args[1].num - 0.18) < 1e-4 &&
                  std::fabs(sends.args[2].num - 0.1) < 1e-4,
              "part 1's sends are 0.18 and 0.1");
        Reply strip;
        check(log.last("/strip", 1, strip) && strip.args.size() == 5 &&
                  std::fabs(strip.args[1].num - 0.30666667 * 1.5) < 1e-4 && std::fabs(strip.args[2].num - 0.4) < 1e-4 &&
                  int(strip.args[3].num) == 0 && int(strip.args[4].num) == 0,
              "part 1's strip is the song's gain and pan");
        cmd("/preset_save", {Value::i(1), Value::s(scratch + "/refused.acpreset"), Value::s("x"), Value::i(0)});
        Reply saved;
        check(log.wait("/preset_saved", saved, 10s) && saved.args.size() >= 3 &&
                  saved.args[2].str == "not while a song is loaded" && !exists(scratch + "/refused.acpreset"),
              "/preset_save is refused while a song is loaded");
        Reply names;
        check(log.last("/macro_names", 0, names) && names.args.size() == 9 && names.args[1].str == "Tighter" &&
                  names.args[8].str == "Room",
              "part 0's macro names");
        Reply graph;
        check(log.last("/graph", 7, graph), "part 7's Nexus graph is sent");
        Reply mounted;
        check(log.last("/mounted", 8, mounted) && mounted.args[1].str == "Trinity", "part 8 /mounted Trinity");
    }

    // It plays.
    { std::lock_guard<std::mutex> lock(peak_mx); peak.store(0.f); }
    host.transportPlay(0);
    std::this_thread::sleep_for(3s);
    host.transportStop();
    check(peak.load() > 0.01f, "plays with the transport running (peak " + std::to_string(peak.load()) + ")");

    // Transport over OSC: the scene list, play, queue, stop and the position.
    {
        // The scene list came just before /song_loaded, from the last load.
        Reply sc;
        const bool got_scenes = log.wait("/song_scenes", sc, 1s);
        check(got_scenes && log.index("/song_scenes") < log.index("/song_loaded"),
              "/song_scenes comes before /song_loaded");
        const char* names[] = {"Intro", "Groove", "Break", "Build", "Drop", "Outro"};
        const int   repeats[] = {1, 2, 1, 1, 2, 1};
        bool        right     = got_scenes && sc.args.size() == 1 + 6 * 4 && int(sc.args[0].num) == 6;
        for(int k = 0; right && k < 6; ++k)
            right = sc.args[size_t(1 + k * 4)].str == names[k] &&
                    sc.args[size_t(2 + k * 4)].str == std::string("s-") + [&] {
                        std::string n = names[k];
                        for(auto& ch : n)
                            ch = char(std::tolower(ch));
                        return n;
                    }() &&
                    int(sc.args[size_t(4 + k * 4)].num) == repeats[k];
        check(right, "six scenes with song.json's names, ids and repeats");
        check(got_scenes && sc.args.size() > 3 && sc.args[3].str == "-41773852288867351",
              "the first scene's engine id is engine.txt's");
        const std::string drop_engine_id = got_scenes && sc.args.size() > 19 ? sc.args[19].str : "";

        const auto position = [&](Reply& r) {
            log.clear();
            cmd("/song_position", {});
            return log.wait("/song_position", r, 5s) && r.args.size() == 6;
        };
        // The stop above lands on the next block.
        Reply pos;
        bool  stopped = false;
        for(int k = 0; k < 40 && !stopped; ++k)
        {
            stopped = position(pos) && pos.args[0].num == 0;
            if(!stopped)
                std::this_thread::sleep_for(50ms);
        }
        check(stopped, "stopped: /song_position says not playing");

        // The script's tempo and beat don't move the song's.
        cmd("/tempo", {Value::f(90.f)});
        for(int k = 0; k < 4; ++k)
        {
            cmd("/beat", {Value::f(float(k)), Value::f(90.f)});
            std::this_thread::sleep_for(50ms);
        }
        cmd("/song_play", {Value::i(1)});
        std::this_thread::sleep_for(500ms);
        check(position(pos) && pos.args[0].num == 1 && pos.args[1].num == 1 && pos.args[2].num == 0 &&
                  pos.args[3].num > 0 && pos.args[4].num == -1,
              "/song_play 1 plays Groove, first pass, tick " + std::to_string(pos.args.size() > 3 ? pos.args[3].num : -1));
        check(pos.args.size() == 6 && std::fabs(pos.args[5].num - 126.0) < 0.01,
              "it plays at the song's 126, not the script's 90 (" +
                  std::to_string(pos.args.size() == 6 ? pos.args[5].num : 0) + ")");

        cmd("/song_queue", {Value::i(3)});
        check(position(pos) && pos.args[4].num == 3, "/song_queue 3 queues Build");
        cmd("/song_queue", {Value::s("s-drop")});
        check(position(pos) && pos.args[4].num == 4, "/song_queue s-drop queues Drop");
        cmd("/song_queue", {Value::s("-41773852288867351")});
        check(position(pos) && pos.args[4].num == 0, "/song_queue by engine id queues Intro");
        log.clear();
        cmd("/song_queue", {Value::i(6)});
        Reply err;
        check(log.wait("/error", err, 5s) && str(err, 0) == "song: no scene 6", "/song_queue 6 is refused");
        log.clear();
        cmd("/song_queue", {Value::s("nope")});
        check(log.wait("/error", err, 5s), "/song_queue nope is refused");
        check(position(pos) && pos.args[4].num == 0, "and Intro stays queued");
        cmd("/song_queue", {Value::i(-1)});
        check(position(pos) && pos.args[4].num == -1, "/song_queue -1 cancels");

        // Queue Drop and let Groove's two passes run out, fast.
        cmd("/song_queue", {Value::s(drop_engine_id)});
        host.setTempo(999.f);
        bool second = false, dropped = false;
        for(int k = 0; k < 400 && !dropped; ++k)
        {
            std::this_thread::sleep_for(50ms);
            if(!position(pos))
                break;
            second  = second || (pos.args[1].num == 1 && pos.args[2].num == 1);
            dropped = pos.args[1].num == 4;
        }
        check(second, "Groove plays its second pass");
        check(dropped && pos.args[4].num == -1, "then the queued Drop plays and nothing is queued");

        cmd("/song_stop", {});
        stopped = false;
        for(int k = 0; k < 40 && !stopped; ++k)
        {
            std::this_thread::sleep_for(50ms);
            stopped = position(pos) && pos.args[0].num == 0;
        }
        check(stopped, "/song_stop stops it");
        host.setTempo(126.f);
    }

    // Refusals. None of them may touch what's loaded.
    {
        Reply r;
        bool  ok = load(scratch + "/nope.zip", r);
        check(ok && !str(r, 2).empty(), "a missing file: " + str(r, 2));
        write_zip(scratch + "/empty.zip", {{"song.json", "{}"}});
        ok = load(scratch + "/empty.zip", r);
        check(ok && str(r, 2) == "no engine.txt in it", "a zip without engine.txt: " + str(r, 2));
        write_zip(scratch + "/bad.zip", {{"engine.txt", "acidulous-engine 4\n"}, {"../evil.txt", "x"}});
        ok = load(scratch + "/bad.zip", r);
        check(ok && str(r, 2).find("bad name") != std::string::npos && !exists(songs + "/evil.txt") &&
                  !exists(scratch + "/evil.txt"),
              "a zip with ../: " + str(r, 2));
        write_zip(scratch + "/notlines.zip", {{"engine.txt", "hello\n"}});
        ok = load(scratch + "/notlines.zip", r);
        check(ok && str(r, 2) == "not engine lines", "not engine lines: " + str(r, 2));
        Reply st;
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str == "Genesis", "a refused song leaves the loaded one");
        check(!exists(songs + "/notlines"), "and writes no folder");

        // A songs folder outside dust/data.
        log.clear();
        cmd("/song_load", {Value::s(zip), Value::s(scratch + "/elsewhere")});
        ok = log.wait("/song_loaded", r) && r.args.size() >= 5;
        check(ok && str(r, 2).find("must be in") != std::string::npos && !exists(scratch + "/elsewhere"),
              "a songs folder outside dust/data: " + str(r, 2));
        log.clear();
        cmd("/song_load", {Value::s(zip), Value::s(scratch + "/dust/data/../../elsewhere")});
        ok = log.wait("/song_loaded", r) && r.args.size() >= 5;
        check(ok && str(r, 2).find("must be in") != std::string::npos, "and one with ..: " + str(r, 2));

        // The zip inside the folder it would unpack into.
        const std::string original = read_text(songs + "/squelch/engine.txt");
        std::system(("cp '" + zip + "' '" + songs + "/squelch/squelch.zip'").c_str());
        ok = load(songs + "/squelch/squelch.zip", r);
        check(ok && str(r, 2).find("inside the folder") != std::string::npos &&
                  exists(songs + "/squelch/squelch.zip"),
              "a zip inside its own folder: " + str(r, 2));
        std::remove((songs + "/squelch/squelch.zip").c_str());

        // An unpack that fails part way: engine.txt is a file, then wanted
        // as a folder.
        std::system(("mkdir -p '" + scratch + "/other'").c_str());
        write_zip(scratch + "/other/squelch.zip",
                  {{"engine.txt", "acidulous-engine 4\n"}, {"engine.txt/x", "y"}});
        ok = load(scratch + "/other/squelch.zip", r);
        check(ok && !str(r, 2).empty(), "an unpack that fails: " + str(r, 2));
        check(read_text(songs + "/squelch/engine.txt") == original && !exists(songs + "/.squelch.unpacking"),
              "leaves the old folder as it was and no staging folder");
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str == "Genesis", "and the loaded song");
    }

    // The sample budget, at 1 MB: a.wav and b.wav each decode to 640000
    // bytes, a frozen clip of a.wav (two channels) to 1280000.
    {
        setenv("ACIDULOUS_SAMPLE_LIMIT_MB", "1", 1);
        const std::string a = wav(160000), b = wav(160000);
        std::ofstream(scratch + "/b.wav", std::ios::binary) << b;
        write_zip(scratch + "/forage.zip",
                  {{"engine.txt", "acidulous-engine 4\nMountMachine 0 \"Forage\"\nLoadSample 0 0 \"zip:a.wav\" 0\n"},
                   {"a.wav", a}});
        write_zip(scratch + "/forage2.zip",
                  {{"engine.txt", "acidulous-engine 4\nMountMachine 0 \"Forage\"\nLoadSample 0 0 \"zip:a.wav\" 0\n"
                                  "LoadSample 0 1 \"zip:b.wav\" 0\n"},
                   {"a.wav", a},
                   {"b.wav", b}});
        write_zip(scratch + "/frozen.zip",
                  {{"engine.txt", "acidulous-engine 4\nMountMachine 0 \"Trinity\"\n"
                                  "MountFrozen 0 1 \"zip:a.wav\" 120.0 1536 160000 0 0.1\n"},
                   {"a.wav", a}});
        Reply r;
        bool  ok = load(scratch + "/forage.zip", r);
        check(ok && str(r, 2).empty() && int(r.args[4].num) == 1, "a Forage song with one sample loads: " + str(r, 2));
        log.clear();
        cmd("/load", {Value::i(0), Value::s("pad"), Value::s(scratch + "/b.wav"), Value::i(1)});
        Reply e;
        ok = log.wait("/error", e, 20s);
        check(ok && str(e, 0).find("in use") != std::string::npos, "a /load after it sees the song's sample: " + str(e, 0));
        // The frozen clip's zip unpacks beside the song's sample within the
        // budget, so the song stays until the budget check refuses it.
        // Auto drops the clip of a song with no arrangement; frozen keeps it.
        ok = load(scratch + "/frozen.zip", r);
        check(ok && str(r, 2).empty(), "auto leaves out a frozen clip nothing needs: " + str(r, 2));
        std::system(("rm -rf '" + songs + "/frozen'").c_str());
        ok = load(scratch + "/forage.zip", r);
        ok = load(scratch + "/frozen.zip", r, "frozen");
        check(ok && str(r, 2).find("over the 1.0 MB") != std::string::npos, "frozen clips count: " + str(r, 2));
        Reply st;
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str == "Forage" && !exists(songs + "/frozen"),
              "a song over the budget is refused before anything goes");
        // This one's files beside the song's sample would go over, so the
        // parts are emptied before it's unpacked; when it's refused the
        // Forage song is loaded again from its folder, before the refusal.
        log.clear();
        cmd("/song_load", {Value::s(scratch + "/forage2.zip"), Value::s(songs)});
        Reply again, refused;
        ok = log.wait_nth("/song_loaded", 2, refused) && refused.args.size() >= 5;
        check(ok && str(refused, 2).find("over the 1.0 MB") != std::string::npos && !exists(songs + "/forage2"),
              "samples over the budget: " + str(refused, 2));
        check(log.wait_nth("/song_loaded", 1, again) && str(again, 0) == scratch + "/forage.zip" && str(again, 2).empty() &&
                  log.index("/song_unloaded") < 0,
              "the Forage song is loaded again first: " + str(again, 0) + " " + str(again, 2));
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str == "Forage", "part 0 has Forage again");
        unsetenv("ACIDULOUS_SAMPLE_LIMIT_MB");
    }

    // Frozen racks: an unknown machine's rack plays its clip
    // from a stand-in and is an empty part; a Reflux rack keeps its machine.
    {
        unsetenv("ACIDULOUS_SAMPLE_LIMIT_MB");
        const std::string a = wav(4800);
        write_zip(scratch + "/fz.zip",
                  {{"engine.txt", "acidulous-engine 4\nClearFrozen\nMountMachine 0 \"Zork\"\nMountMachine 1 \"Reflux\"\n"
                                  "MountFrozen 0 5 \"zip:a.wav\" 120.0 1536 4800 0 0.1\n"
                                  "MountFrozen 1 5 \"zip:a.wav\" 120.0 1536 4800 0 0.1\n"},
                   {"a.wav", a}});
        for(const char* mode : {"auto", "notes", "frozen"})
        {
            Reply r, fr;
            const bool ok = load(scratch + "/fz.zip", r, mode);
            check(ok && str(r, 2).empty() && int(r.args[3].num) == 0, std::string("frozen racks load, mode ") + mode + ": " + str(r, 2));
            check(int(r.args[4].num) == 1 && str(r, 6) == "Reflux", std::string("the Reflux is a part and the Zork rack isn't, mode ") + mode);
            const bool got = log.wait("/song_frozen", fr);
            const size_t want = std::string(mode) == "frozen" ? 2 : 1;
            check(got && size_t(fr.args[0].num) == want && int(fr.args[1].num) == 0 && int(fr.args[2].num) == 0 &&
                      (want == 1 || (int(fr.args[4].num) == 1 && int(fr.args[5].num) == 1)),
                  std::string("/song_frozen lists the racks that play clips, mode ") + mode);
        }
    }

    // The app's tape song: its Bias rack is a part with its Reel takes.
    {
        const std::string tape = zip.substr(0, zip.rfind('/') + 1) + "tape.zip";
        Reply r, st;
        const bool ok = load(tape, r);
        check(ok && str(r, 2).empty(), "tape.zip loads: " + str(r, 2));
        state(1, st);
        check(st.args.size() > 1 && st.args[1].str == "Bias", "the Bias rack is part 1");
        // Unloaded and loaded again: the Reel is freed with the rack and made anew.
        for(int round = 0; round < 2; ++round)
        {
            log.clear();
            cmd("/song_unload", {});
            Reply un;
            check(log.wait("/song_unloaded", un, 60s) && un.args.size() == 1 && un.args[0].num == 1,
                  "the tape song unloads");
            state(1, st);
            check(st.args.size() > 1 && st.args[1].str.empty(), "the Bias rack is gone");
            Reply again;
            const bool ok2 = load(tape, again);
            check(ok2 && str(again, 2).empty(), "tape.zip loads again: " + str(again, 2));
            state(1, st);
            check(st.args.size() > 1 && st.args[1].str == "Bias", "the Bias rack is back as part 1");
        }
    }

    // A song's Mosaic zones are the part's, with absolute paths.
    {
        write_zip(scratch + "/zones.zip",
                  {{"engine.txt", "acidulous-engine 4\nMountMachine 0 \"Mosaic\"\n"
                                  "LoadZoneMap 0 \"zip:z/a.wav|0|63|48|0|127|0|1|0|0\\nzip:z/b.wav|64|127|72|0|127|0|1|0|0\\n\" \"keys\"\n"},
                   {"z/a.wav", wav(4800)},
                   {"z/b.wav", wav(4800)}});
        Reply r;
        const bool ok = load(scratch + "/zones.zip", r);
        const std::string want = songs + "/zones/z/a.wav|0|63|48|0|127|0|1|0|0\n" + songs +
                                 "/zones/z/b.wav|64|127|72|0|127|0|1|0|0\n";
        check(ok && str(r, 2).empty() && c->zones(0) == want, "Mosaic's zones are read back: " + c->zones(0));
    }

    // Audio stops part way through the replay: once the parts are emptied,
    // the fake audio thread pauses until the load has answered. The zones
    // song can't be loaded again without audio, so /song_unloaded 1 comes
    // first, and the teardown is finished once the audio is back.
    {
        log.clear();
        cmd("/song_load", {Value::s(zip), Value::s(songs)});
        check(log.wait_part("/mounted", core::kParts - 1), "the parts are emptied for the song");
        std::this_thread::sleep_for(20ms);
        paused = true;
        Reply r, gone;
        const bool ok = log.wait("/song_loaded", r) && r.args.size() >= 5;
        paused = false;
        check(ok && str(r, 2).find("audio stopped at line") != std::string::npos, "audio stopping: " + str(r, 2));
        check(log.wait("/song_unloaded", gone, 1s) && gone.args.size() == 1 && gone.args[0].num == 1 &&
                  log.index("/song_unloaded") < log.index("/song_loaded"),
              "/song_unloaded 1 comes before it");
        bool empty = false;
        for(int k = 0; k < 100 && !empty; ++k)
        {
            std::this_thread::sleep_for(50ms);
            empty = true;
            for(int p = 0; p < core::kParts; ++p)
                empty = empty && !*host.mountedMachine(p) && !*host.mountedEffect(p, 0) && !*host.mountedEffect(p, 1);
        }
        check(empty, "once the audio is back every rack and insert is empty");
        check(std::fabs(host.masterParamNormalized("volume") - 0.6f) < 1e-4,
              "and the master volume is the script's 0.6 (" + std::to_string(host.masterParamNormalized("volume")) + ")");
        Reply st;
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str.empty(), "leaves the parts empty");
    }

    // The teardown before a replay stalls and the audio comes back for the
    // replay: the song that loads keeps its arrangement and transport. The
    // pause is tried at a few lengths until one stalls the teardown and still
    // lets the replay run.
    check(load(zip, loaded) && str(loaded, 2).empty(), "loads the zip for the stalled teardown");
    {
        bool stalled = false;
        for(int ms : {1100, 1300, 900, 1600, 700, 2000})
        {
            const int before = c->stalled_teardowns();
            log.clear();
            cmd("/song_load", {Value::s(zip), Value::s(songs)});
            log.wait_part("/mounted", core::kParts - 1);
            paused = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            paused = false;
            Reply r;
            const bool ok = log.wait("/song_loaded", r) && r.args.size() >= 5 && str(r, 2).empty();
            if(!ok)
                load(zip, r);
            if(ok && c->stalled_teardowns() > before)
            {
                stalled = true;
                break;
            }
        }
        check(stalled, "a teardown stalled and the song still loaded");
        // The worker would finish a teardown left dirty within 250 ms.
        std::this_thread::sleep_for(800ms);
        log.clear();
        { std::lock_guard<std::mutex> lock(peak_mx); peak.store(0.f); }
        cmd("/song_play", {});
        std::this_thread::sleep_for(1500ms);
        cmd("/song_position", {});
        Reply pos;
        check(log.wait("/song_position", pos, 5s) && !pos.args.empty() && pos.args[0].num == 1,
              "the song still plays after the stalled teardown");
        check(peak.load() > 0.01f, "and it's heard (peak " + std::to_string(peak.load()) + ")");
        cmd("/song_stop", {});
        // A /master with the song loaded moves the song's master only.
        cmd("/master", {Value::s("volume"), Value::f(0.3f)});
        std::this_thread::sleep_for(100ms);
        check(std::fabs(host.masterParamNormalized("volume") - 0.3f) < 1e-4, "/master moves the song's master");
    }

    // Unload back to empty.
    check(load(zip, loaded) && str(loaded, 2).empty(), "loads the zip again");
    check(std::fabs(host.sendParamNormalized(0, "size") - 0.38f) < 1e-4,
          "the song's Reverb size 0.38 while it's loaded (" + std::to_string(host.sendParamNormalized(0, "size")) + ")");
    {
        Reply sends;
        check(log.wait("/song_sends", sends, 1s) && sends.args.size() == 2 && std::fabs(sends.args[0].num - 0.38) < 1e-4 &&
                  std::fabs(sends.args[1].num - host.sendParamNormalized(1, "feedback")) < 1e-5 &&
                  log.index("/song_sends") < log.index("/song_loaded"),
              "/song_sends gives the song's Reverb size and Delay feedback before /song_loaded");
        // /send with the song loaded moves the song's Reverb only
        cmd("/send", {Value::i(0), Value::s("size"), Value::f(0.25f)});
        std::this_thread::sleep_for(100ms);
        check(std::fabs(host.sendParamNormalized(0, "size") - 0.25f) < 1e-4,
              "/send moves the song's Reverb size (" + std::to_string(host.sendParamNormalized(0, "size")) + ")");
    }
    log.clear();
    cmd("/song_unload", {});
    {
        Reply r;
        check(log.wait("/song_unloaded", r, 60s) && r.args.size() == 1 && r.args[0].num == 1, "/song_unloaded 1 came");
        check(log.count("/inserts", 2) == 1 && log.count("/inserts", 12) == 1, "one /inserts per part for the unload");
        bool empty = true;
        for(int p = 0; p < core::kParts; ++p)
            empty = empty && std::string(host.mountedMachine(p)).empty() && std::string(host.mountedEffect(p, 0)).empty() &&
                    std::string(host.mountedEffect(p, 1)).empty();
        check(empty, "every rack and insert is empty");
        Reply st;
        state(0, st);
        check(st.args.size() >= 4 && std::fabs(st.args[3].num - 1.0 / 1.5) < 1e-4, "part 0's gain is back to unity");
        check(std::fabs(host.tempo() - 100.f) < 1e-3, "the tempo is back to 100 (" + std::to_string(host.tempo()) + ")");
        check(std::fabs(host.masterParamNormalized("volume") - 0.6f) < 1e-4,
              "the master volume is the script's 0.6 again (" + std::to_string(host.masterParamNormalized("volume")) + ")");
        check(host.swingUnit() == 1 && !host.loopSong(), "swing in eighths and loop-song off again");
        check(std::string(host.mountedSend(0)) == "Reverb" && std::fabs(host.sendParamNormalized(0, "size") - 0.7f) < 1e-4,
              "the Reverb is back with the plugin's size 0.7 (" + std::to_string(host.sendParamNormalized(0, "size")) + ")");
        bool fx_gone = true;
        for(int slot = 0; slot < 2; ++slot)
            fx_gone = fx_gone && !*host.mountedMasterInsert(slot);
        for(int g = 0; g < 4; ++g)
            for(int slot = 0; slot < 2; ++slot)
                fx_gone = fx_gone && !*host.mountedGroupInsert(g, slot);
        check(fx_gone, "no master or group inserts");
        log.clear();
        cmd("/song_position", {});
        cmd("/tempo", {});
        Reply t;
        check(log.wait("/tempo", t, 5s) && log.index("/song_position") < 0, "no /song_position without a song");
        cmd("/tempo", {Value::f(110.f)});
        std::this_thread::sleep_for(100ms);
        check(std::fabs(host.tempo() - 110.f) < 1e-3, "/tempo works again (" + std::to_string(host.tempo()) + ")");
        cmd("/tempo", {Value::f(100.f)});
        std::this_thread::sleep_for(100ms);
        log.clear();
        cmd("/mods", {Value::i(5)});
        Reply m5;
        bool  none = log.wait("/mods", m5, 10s);
        for(size_t k = 1; none && k < m5.args.size(); ++k)
            if(m5.args[k].type == 's' && !m5.args[k].str.empty())
                none = false;
        check(none, "part 5's Arp is gone");
        { std::lock_guard<std::mutex> lock(peak_mx); peak.store(0.f); }
        host.transportPlay(0);
        std::this_thread::sleep_for(1s);
        host.transportStop();
        check(peak.load() < 1e-4f, "silent with the transport running (peak " + std::to_string(peak.load()) + ")");
    }

    // Deleted with a song loaded.
    check(load(zip, loaded) && str(loaded, 2).empty(), "loads once more before the core goes");
    go = false;
    audio.join();
    delete c;

    std::system(("rm -rf '" + scratch + "'").c_str());
    std::printf(failures ? "%d failed\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
