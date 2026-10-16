// Remounts a Cumulus part as Nexus while its cloud may be building, many
// times, with the core's worker and cloud threads and a fake audio thread.
// A cloud mounted into the Nexus would crash or show under ASan.
// Usage: acidulous-cloud-test BANK_DIR [ROUNDS]
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
    std::atomic<int> errors{0};
    void reply(const char* path, const core::Args& args) override
    {
        if(!std::strcmp(path, "/error"))
        {
            ++errors;
            std::printf("  /error %s\n", args.empty() ? "" : args[0].str.c_str());
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    const std::string banks  = argc > 1 ? argv[1] : "data/banks";
    const int         rounds = argc > 2 ? std::atoi(argv[2]) : 200;
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
    std::mt19937 rng(1);
    for(int k = 0; k < rounds; ++k)
    {
        c->command("/mount", {Value::i(0), Value::s("Cumulus")});
        c->command("/note", {Value::i(0), Value::i(1), Value::i(60), Value::i(100)});
        c->command("/param", {Value::i(0), Value::s("stretch"), Value::f(0.3 + 0.001 * (k % 100))});
        // Sometimes before the build starts, sometimes during it.
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 120));
        c->command("/patch", {Value::i(0), Value::s("Nexus"), Value::s("Init")});
        c->command("/note", {Value::i(0), Value::i(1), Value::i(64), Value::i(100)});
        if(k % 10 == 0)
            c->drain();
    }
    c->drain();
    std::this_thread::sleep_for(200ms);
    go = false;
    audio.join();
    delete c;
    const bool ok = finite && log.errors == 0;
    std::printf("%s: %d remounts while a cloud builds\n", ok ? "ok  " : "FAIL", rounds);
    return ok ? 0 : 1;
}
