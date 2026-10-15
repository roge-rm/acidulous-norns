// Runs the core with its worker thread, as in scsynth, and the OSC bridge:
// - /patch then /note straight after plays the note on the new patch
// - the bridge and the core start and stop twice without hanging or aborting
// - reset() empties the parts
// Usage: acidulous-test BANK_DIR
#include "bridge.h"
#include "core.h"

#include <lo/lo.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

namespace
{

struct Log : core::Replier
{
    std::atomic<int> mounted{0}, errors{0};
    void reply(const char* path, const core::Args&) override
    {
        if(!std::strcmp(path, "/mounted"))
            ++mounted;
        if(!std::strcmp(path, "/error"))
            ++errors;
    }
};

int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok)
        ++failures;
}

// Plays like a sound card: 64-frame blocks in real time, tracking the peak.
struct Audio
{
    core::Core&         c;
    std::atomic<bool>   go{true};
    std::atomic<double> peak{0};
    std::thread         t;
    explicit Audio(core::Core& core) : c(core) { t = std::thread([this] { run(); }); }
    ~Audio() { stop(); }
    void stop()
    {
        go = false;
        if(t.joinable())
            t.join();
    }
    void run()
    {
        float l[64], r[64];
        while(go)
        {
            c.process(nullptr, nullptr, l, r, 64);
            double p = peak;
            for(int i = 0; i < 64; ++i)
                p = std::fmax(p, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            peak = p;
            std::this_thread::sleep_for(1ms);
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    const std::string banks = argc > 1 ? argv[1] : "data/banks";
    for(int round = 0; round < 2; ++round)
    {
        Log log;
        auto* c = new core::Core(banks, &log, false);
        check(bridge::start(c, "57311", "57312"), "bridge starts");

        Audio audio(*c);
        c->command("/patch", {core::Value::i(0), core::Value::s("Reflux"), core::Value::s("Classic")});
        c->command("/patch", {core::Value::i(1), core::Value::s("Hexbeat"), core::Value::s("Straight")});
        c->drain();
        // The part has another machine; the note follows the remount at once.
        c->command("/patch", {core::Value::i(0), core::Value::s("Trinity"), core::Value::s("Wide Saws")});
        c->command("/note", {core::Value::i(0), core::Value::i(1), core::Value::i(48), core::Value::i(110)});
        c->drain();
        audio.peak = 0;
        std::this_thread::sleep_for(800ms);
        const double peak = audio.peak;
        check(peak > 0.01, "note right after /patch is heard");
        check(log.errors == 0, "no errors");

        // Over OSC, from the loopback address.
        lo_address me = lo_address_new("127.0.0.1", "57311");
        lo_send(me, "/unmount", "i", 1);
        lo_address_free(me);
        std::this_thread::sleep_for(300ms);
        c->drain();
        const int before = log.mounted;
        c->reset();
        check(log.mounted > before, "reset unmounts");

        audio.stop();
        bridge::stop();
        delete c;
        check(true, "stopped");
    }
    return failures ? 1 : 0;
}
