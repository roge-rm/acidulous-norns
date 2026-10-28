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
// - deleting the core with a song loaded
// Usage: acidulous-song-test BANK_DIR SCRATCH_DIR SONG_ZIP
#include "core.h"

#include <EngineHost.h>
#include <engine/machine/MachineRegistry.h>

#include <atomic>
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
    std::thread        audio([&] {
        float l[64], r[64];
        while(go)
        {
            if(paused)
            {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            c->process(nullptr, nullptr, l, r, 64);
            float p = peak.load();
            for(int i = 0; i < 64; ++i)
                p = std::max(p, std::max(std::fabs(l[i]), std::fabs(r[i])));
            peak.store(p);
            std::this_thread::sleep_for(300us);
        }
    });
    const auto cmd = [&](const char* path, core::Args a) { c->command(path, a); };
    const auto load = [&](const std::string& path, Reply& r) {
        log.clear();
        cmd("/song_load", {Value::s(path), Value::s(songs)});
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
    cmd("/channel", {Value::i(1), Value::s("sendreverb"), Value::f(0.5f)});
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
    peak.store(0.f);
    host.transportPlay(0);
    std::this_thread::sleep_for(3s);
    host.transportStop();
    check(peak.load() > 0.01f, "plays with the transport running (peak " + std::to_string(peak.load()) + ")");

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
        ok = load(scratch + "/frozen.zip", r);
        check(ok && str(r, 2).find("over the 1.0 MB") != std::string::npos, "frozen clips count: " + str(r, 2));
        Reply st;
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str == "Forage" && !exists(songs + "/frozen"),
              "a song over the budget is refused before anything goes");
        // This one's files beside the song's sample would go over, so the
        // parts are emptied before it's unpacked.
        ok = load(scratch + "/forage2.zip", r);
        check(ok && str(r, 2).find("over the 1.0 MB") != std::string::npos && !exists(songs + "/forage2"),
              "samples over the budget: " + str(r, 2));
        unsetenv("ACIDULOUS_SAMPLE_LIMIT_MB");
    }

    // Audio stops part way through the replay: once the parts are emptied,
    // the fake audio thread pauses.
    {
        log.clear();
        cmd("/song_load", {Value::s(zip), Value::s(songs)});
        check(log.wait_part("/mounted", core::kParts - 1), "the parts are emptied for the song");
        std::this_thread::sleep_for(20ms);
        paused = true;
        Reply r;
        const bool ok = log.wait("/song_loaded", r) && r.args.size() >= 5;
        paused = false;
        check(ok && str(r, 2).find("audio stopped at line") != std::string::npos, "audio stopping: " + str(r, 2));
        Reply st;
        state(0, st);
        check(st.args.size() > 1 && st.args[1].str.empty(), "leaves the parts empty");
    }

    // Unload back to empty.
    check(load(zip, loaded) && str(loaded, 2).empty(), "loads the zip again");
    log.clear();
    cmd("/song_unload", {});
    {
        Reply r;
        check(log.wait("/song_unloaded", r, 60s), "/song_unloaded came");
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
        log.clear();
        cmd("/mods", {Value::i(5)});
        Reply m5;
        bool  none = log.wait("/mods", m5, 10s);
        for(size_t k = 1; none && k < m5.args.size(); ++k)
            if(m5.args[k].type == 's' && !m5.args[k].str.empty())
                none = false;
        check(none, "part 5's Arp is gone");
        peak.store(0.f);
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
