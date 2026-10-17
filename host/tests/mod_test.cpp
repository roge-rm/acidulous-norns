// Modifiers on a part, with the core's worker thread and a fake audio thread,
// as the plugin runs. The part's MIDI out is on, so the notes that reach the
// machine come back out of EngineHost::drainMidiOut.
// - Scale (D dorian, degree mode) then Chord (diatonic) in slots 0 and 1:
//   a key plays the chord of its scale degree
// - /mods reads back the types, bypass and values
// - bypass, a machine change keeping the modifiers, and unmounting
// - an Arp stepping with the transport stopped
// Usage: acidulous-mod-test BANK_DIR
#include "core.h"

#include <EngineHost.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
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
    bool wait(const char* path, Reply& out)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, 10s, [&] {
            for(const auto& r : got)
                if(r.path == path)
                {
                    out = r;
                    return true;
                }
            return false;
        });
    }
};

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if(!ok)
        ++failures;
}

struct Note
{
    int on, note;
};

// What has come out of MIDI out since the last call.
std::vector<Note> drain()
{
    std::vector<Note> out;
    int64_t           buf[256];
    for(;;)
    {
        const int n = acidulous::EngineHost::instance().drainMidiOut(buf, 128);
        for(int k = 0; k < n; ++k)
        {
            const int64_t v    = buf[k * 2 + 1];
            const int     kind = int((v >> 16) & 0xf0), d1 = int((v >> 8) & 0x7f), d2 = int(v & 0x7f);
            if(kind == 0x90 || kind == 0x80)
                out.push_back({kind == 0x90 && d2 > 0, d1});
        }
        if(n < 128)
            return out;
    }
}

std::set<int> ons(const std::vector<Note>& notes)
{
    std::set<int> s;
    for(const auto& n : notes)
        if(n.on)
            s.insert(n.note);
    return s;
}

std::string show(const std::set<int>& s)
{
    std::string t;
    for(int n : s)
        t += (t.empty() ? "" : " ") + std::to_string(n);
    return "{" + t + "}";
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::fprintf(stderr, "usage: acidulous-mod-test BANK_DIR\n");
        return 2;
    }
    Log   log;
    auto* c = new core::Core(argv[1], &log, false);

    std::atomic<bool> go{true};
    std::thread       audio([&] {
        float l[64], r[64];
        while(go)
        {
            c->process(nullptr, nullptr, l, r, 64);
            std::this_thread::sleep_for(300us);
        }
    });
    const auto cmd   = [&](const char* path, core::Args a) { c->command(path, a); };
    const auto settle = [&] {
        // a /mods round trip waits for everything before it to land
        log.clear();
        cmd("/mods", {Value::i(0)});
        Reply r;
        log.wait("/mods", r);
        return r;
    };
    // The notes a key sends to the machine, on and off.
    const auto play = [&](int note) {
        drain();
        cmd("/note", {Value::i(0), Value::i(1), Value::i(note), Value::i(100)});
        std::this_thread::sleep_for(60ms);
        cmd("/note", {Value::i(0), Value::i(1), Value::i(note), Value::i(0)});
        std::this_thread::sleep_for(60ms);
        return drain();
    };
    const auto offs_match = [](const std::vector<Note>& notes) {
        int held = 0;
        for(const auto& n : notes)
            held += n.on ? 1 : -1;
        return held == 0;
    };
    const auto mod_param = [&](int slot, const char* name, double v) {
        cmd("/mod_param", {Value::i(0), Value::i(slot), Value::s(name), Value::f(v)});
    };

    log.clear();
    cmd("/mount", {Value::i(0), Value::s("Trinity")});
    Reply r;
    check(log.wait("/mounted", r), "Trinity mounted");
    cmd("/channel", {Value::i(0), Value::s("midimode"), Value::f(1.0)}); // MIDI out only

    // No modifiers: the key itself.
    settle();
    check(ons(play(60)) == std::set<int>{60}, "no modifiers: 60 plays 60");

    // Scale: D dorian, degree mode.
    cmd("/mod", {Value::i(0), Value::i(0), Value::s("Scale")});
    mod_param(0, "key", 2.0 / 11.0);
    mod_param(0, "scale", 1.0 / 32.0);
    mod_param(0, "mode", 1.0);
    settle();
    check(ons(play(60)) == std::set<int>{62}, "scale degree mode: 60 plays D (62)");
    check(ons(play(62)) == std::set<int>{65}, "scale degree mode: 62 plays the 3rd degree, F (65)");

    // Chord, diatonic in D dorian, after the scale.
    cmd("/mod", {Value::i(0), Value::i(1), Value::s("Chord")});
    mod_param(1, "key", 2.0 / 11.0);
    mod_param(1, "scale", 1.0 / 32.0);
    mod_param(1, "mode", 1.0);
    r = settle();
    {
        auto notes = play(60);
        check(ons(notes) == std::set<int>{62, 65, 69}, "scale + chord: 60 plays D minor " + show(ons(notes)));
        check(offs_match(notes), "scale + chord: every note let go");
        notes = play(61);
        check(ons(notes) == std::set<int>{64, 67, 71}, "scale + chord: 61 plays E minor " + show(ons(notes)));
    }

    // Read back.
    {
        const auto& a  = r.args;
        bool        ok = a.size() > 5 && a[1].num == 0 && a[2].str == "Scale" && a[3].num == 0;
        size_t      k  = 5;
        float       key = -1, mode = -1;
        if(ok)
        {
            const int count = int(a[4].num);
            for(int j = 0; j < count && k + 1 < a.size(); ++j, k += 2)
            {
                if(a[k].str == "key")
                    key = float(a[k + 1].num);
                if(a[k].str == "mode")
                    mode = float(a[k + 1].num);
            }
        }
        check(ok && std::abs(key - 2.f / 11.f) < 1e-4f && mode == 1.f, "/mods reads back the scale's key and mode");
        check(k + 1 < a.size() && a[k].num == 1 && a[k + 1].str == "Chord", "/mods lists the chord in slot 1");
    }

    // Bypass the scale: 62 is D again, so the chord is D minor.
    cmd("/mod_bypass", {Value::i(0), Value::i(0), Value::i(1)});
    r = settle();
    check(r.args.size() > 3 && r.args[3].num == 1, "/mods reads back the bypass");
    {
        auto notes = play(62);
        check(ons(notes) == std::set<int>{62, 65, 69}, "scale bypassed: 62 plays D minor " + show(ons(notes)));
    }
    cmd("/mod_bypass", {Value::i(0), Value::i(0), Value::i(0)});

    // A machine change keeps them.
    log.clear();
    cmd("/mount", {Value::i(0), Value::s("Ratio")});
    check(log.wait("/mounted", r), "Ratio mounted");
    cmd("/channel", {Value::i(0), Value::s("midimode"), Value::f(1.0)});
    r = settle();
    check(r.args.size() > 2 && r.args[2].str == "Scale", "machine change keeps the scale");
    {
        auto notes = play(60);
        check(ons(notes) == std::set<int>{62, 65, 69}, "after the machine change: 60 plays D minor " + show(ons(notes)));
    }

    // A param for a slot with nothing in it is an error, not a crash.
    cmd("/mod", {Value::i(0), Value::i(1), Value::s("")});
    log.clear();
    mod_param(1, "key", 0.5);
    check(log.wait("/error", r), "a param for an empty slot is an error");
    {
        auto notes = play(60);
        check(ons(notes) == std::set<int>{62}, "chord unmounted: 60 plays 62 " + show(ons(notes)));
    }

    // An arp with the transport stopped: hold a key for a second.
    cmd("/mod", {Value::i(0), Value::i(0), Value::s("")});
    cmd("/mod", {Value::i(0), Value::i(1), Value::s("Arp")});
    settle();
    drain();
    cmd("/note", {Value::i(0), Value::i(1), Value::i(60), Value::i(100)});
    cmd("/note", {Value::i(0), Value::i(1), Value::i(64), Value::i(100)});
    std::this_thread::sleep_for(1000ms);
    cmd("/note", {Value::i(0), Value::i(1), Value::i(60), Value::i(0)});
    cmd("/note", {Value::i(0), Value::i(1), Value::i(64), Value::i(0)});
    std::this_thread::sleep_for(100ms);
    {
        const auto notes = drain();
        int        count = 0;
        for(const auto& n : notes)
            count += n.on;
        std::printf("  arp: %d steps in a second with the transport stopped\n", count);
        check(count >= 3 && ons(notes) == std::set<int>{60, 64}, "arp steps with the transport stopped");
        check(offs_match(notes), "arp: every note let go");
    }
    cmd("/mod", {Value::i(0), Value::i(1), Value::s("")});
    r = settle();
    check(r.args.size() > 6 && r.args[2].str.empty() && r.args[6].str.empty(), "both slots empty");

    go = false;
    audio.join();
    delete c;
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
