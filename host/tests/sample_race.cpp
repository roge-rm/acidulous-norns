// Loads files into the sample machines, reloads them, remounts the part as
// another machine and clears it, many times, with the core's worker thread
// and a fake audio thread playing notes, while files are heard as the
// picker does. Run under ASan, a sample used after
// it's freed or one never freed shows up.
// Usage: acidulous-sample-test BANK_DIR MATERIAL_DIR [ROUNDS]
// MATERIAL_DIR is what host/tests/make_material.py makes.
#include "core.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

using namespace std::chrono_literals;

namespace
{

struct Log : core::Replier
{
    std::atomic<int> errors{0}, loads{0};
    void reply(const char* path, const core::Args& args) override
    {
        if(!std::strcmp(path, "/error"))
        {
            ++errors;
            std::printf("  /error %s\n", args.empty() ? "" : args[0].str.c_str());
        }
        else if(!std::strcmp(path, "/loaded"))
            ++loads;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::fprintf(stderr, "usage: acidulous-sample-test BANK_DIR MATERIAL_DIR [ROUNDS]\n");
        return 2;
    }
    const std::string banks  = argv[1];
    const std::string mat    = argv[2];
    const int         rounds = argc > 3 ? std::atoi(argv[3]) : 40;
    Log               log;
    auto*             c = new core::Core(banks, &log, false);

    std::atomic<bool> go{true};
    std::atomic<bool> finite{true};
    std::thread       audio([&] {
        float l[64], r[64];
        while(go)
        {
            c->process(nullptr, nullptr, l, r, 64);
            for(int i = 0; i < 64; ++i)
                if(!std::isfinite(l[i]) || !std::isfinite(r[i]))
                    finite = false;
            std::this_thread::sleep_for(300us);
        }
    });

    using core::Value;
    const auto cmd   = [&](const char* path, core::Args a) { c->command(path, a); };
    const auto notes = [&](int part) {
        for(int n : {36, 38, 43, 48, 60})
            cmd("/note", {Value::i(part), Value::i(1), Value::i(n), Value::i(100)});
    };
    std::mt19937 rng(1);
    for(int k = 0; k < rounds; ++k)
    {
        const auto nap = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 30)); };
        // Part 0: a kit, a pad reloaded over it, another patch, then a remount.
        cmd("/patch", {Value::i(0), Value::s("Forage"), Value::s("Init")});
        cmd("/load", {Value::i(0), Value::s("kit"), Value::s(mat + "/kit")});
        notes(0);
        nap();
        cmd("/load", {Value::i(0), Value::s("pad"), Value::s(mat + "/kit/03-snare.wav"), Value::i(0)});
        cmd("/patch", {Value::i(0), Value::s("Forage"), Value::s("Body")});
        notes(0);
        nap();
        // Part 1: a SoundFont, its other preset, then a folder.
        cmd("/patch", {Value::i(1), Value::s("Mosaic"), Value::s("Init")});
        cmd("/load", {Value::i(1), Value::s("sf2"), Value::s(mat + "/test.sf2"), Value::i(k % 2)});
        notes(1);
        nap();
        cmd("/load", {Value::i(1), Value::s("folder"), Value::s(mat + "/keys")});
        notes(1);
        // Part 2: takes and an utterance, reloaded and remounted.
        const char* m2 = k % 3 == 0 ? "Dice" : k % 3 == 1 ? "Pollen" : "Molt";
        cmd("/patch", {Value::i(2), Value::s(m2), Value::s("Init")});
        cmd("/load", {Value::i(2), Value::s("sample"), Value::s(mat + (k % 2 ? "/loop.wav" : "/voice.flac"))});
        notes(2);
        nap();
        cmd("/load", {Value::i(2), Value::s("sample"), Value::s(mat + "/voice.wav")});
        // The picker's shape and hearing, swapped and stopped while it plays.
        cmd("/shape", {Value::s(mat + "/kit/12-cowbell.flac")});
        cmd("/hear", {Value::s(mat + (k % 2 ? "/loop.wav" : "/kit/02-rim.aif"))});
        if(k % 3 == 0)
            cmd("/hear", {});
        // Remounts with the samples in, and a clear.
        cmd("/patch", {Value::i(0), Value::s(k % 2 ? "Mosaic" : "Reflux"), Value::s("Init")});
        cmd("/load", {Value::i(1), Value::s("clear"), Value::s("")});
        if(k % 4 == 3)
            cmd("/unmount", {Value::i(2)});
        if(k % 10 == 0)
            c->drain();
    }
    c->drain();
    std::this_thread::sleep_for(200ms);
    go = false;
    audio.join();
    delete c;
    const bool ok = finite && log.errors == 0;
    std::printf("%s: %d rounds, %d loads\n", ok ? "ok  " : "FAIL", rounds, log.loads.load());
    return ok ? 0 : 1;
}
