#include "bridge.h"

#include "core.h"

#include <lo/lo.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace bridge
{
namespace
{

// These are never destroyed, so a thread that is still replying while the
// process exits can't touch a destroyed mutex. stop() joins the threads.
lo_server_thread  osc = nullptr;
lo_address        to  = nullptr;
std::mutex&       send_lock = *new std::mutex; // replies come from the OSC thread and the core's worker
std::thread*      pump      = nullptr;
std::atomic<bool> running{false};
core::Core*       target = nullptr;
bool              local_only = true;

bool from_loopback(lo_message msg)
{
    lo_address src = lo_message_get_source(msg);
    const char* h  = src ? lo_address_get_hostname(src) : nullptr;
    if(!h)
        return false;
    return !std::strncmp(h, "127.", 4) || !std::strcmp(h, "::1") || !std::strncmp(h, "::ffff:127.", 11) ||
           !std::strcmp(h, "localhost");
}

struct OscReplier : core::Replier
{
    void reply(const char* path, const core::Args& args) override
    {
        lo_message m = lo_message_new();
        for(const auto& a : args)
        {
            if(a.type == 's')
                lo_message_add_string(m, a.str.c_str());
            else if(a.type == 'i')
                lo_message_add_int32(m, int32_t(a.num));
            else
                lo_message_add_float(m, float(a.num));
        }
        {
            std::lock_guard<std::mutex> l(send_lock);
            if(to)
                lo_send_message(to, path, m);
        }
        lo_message_free(m);
    }
};
OscReplier replier;

// norns sends every Lua number as a float, so numbers keep their own type
// and the core converts.
int on_any(const char* path, const char* types, lo_arg** a, int argc, lo_message msg, void*)
{
    if(local_only && !from_loopback(msg))
        return 0;
    core::Args args;
    for(int i = 0; i < argc; ++i)
    {
        switch(types[i])
        {
            case 'i': args.push_back(core::Value::i(a[i]->i)); break;
            case 'h': args.push_back(core::Value::i(int(a[i]->h))); break;
            case 'f': args.push_back(core::Value::f(a[i]->f)); break;
            case 'd': args.push_back(core::Value::f(a[i]->d)); break;
            case 's':
            case 'S': args.push_back(core::Value::s(&a[i]->s)); break;
            case 'T': args.push_back(core::Value::i(1)); break;
            case 'F': args.push_back(core::Value::i(0)); break;
            default: args.push_back(core::Value::f(0)); break;
        }
    }
    if(target && !target->command(path, args))
        std::fprintf(stderr, "acidulous: unknown OSC %s,%s\n", path, types);
    return 0;
}

void pump_main()
{
    int ticks = 0, scope_ticks = 0, song_ticks = 0;
    int song_was = -1; // playing (1) or not (0) in the last /song_position, -1 none
    while(running)
    {
        // Short sleeps so stop() doesn't wait a quarter of a second.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(!running)
            break;
        ++ticks;
        if(ticks % 5 == 0)
        {
            float l[core::kParts];
            target->levels(l);
            core::Args args;
            for(float v : l)
                args.push_back(core::Value::f(v));
            replier.reply("/levels", args);
            if(target->metering_on())
            {
                float peak = 0, lufs = 0;
                target->master_meters(peak, lufs);
                replier.reply("/meters", {core::Value::f(peak), core::Value::f(lufs)});
            }
        }
        // The scope about 14 times a second, under the screen's 15.
        if(++scope_ticks == 7)
        {
            scope_ticks = 0;
            float     trace[126];
            int       part = -1, slot = -1;
            const int n = target->scope_trace(trace, 126, part, slot);
            if(n > 0)
            {
                core::Args args{core::Value::i(part), core::Value::i(slot)};
                for(int i = 0; i < n; ++i)
                    args.push_back(core::Value::f(trace[i]));
                replier.reply("/scope", args);
            }
        }
        // A loaded song's position 5 times a second while it plays, and once
        // a second while it's stopped.
        if(++song_ticks % 20 == 0)
        {
            core::Args pos;
            if(!target->song_position(pos))
                song_was = -1;
            else
            {
                const int playing = int(pos[0].num);
                if(playing || song_was != 0 || song_ticks >= 100)
                    replier.reply("/song_position", pos);
                song_was = playing;
            }
            if(song_ticks >= 100)
                song_ticks = 0;
        }
        if(ticks < 25)
            continue;
        ticks = 0;
        replier.reply("/load", {core::Value::f(target->load_percent()), core::Value::f(target->worst_percent())});
    }
}

void on_error(int num, const char* msg, const char* where)
{
    std::fprintf(stderr, "acidulous: liblo error %d: %s %s\n", num, msg ? msg : "", where ? where : "");
}

} // namespace

core::Replier* replier_for_core()
{
    return &replier;
}

bool start(core::Core* core, const std::string& port, const std::string& reply)
{
    if(running)
        return true;
    // liblo can only bind every interface, so the handler drops anything that
    // doesn't come from this machine, unless ACIDULOUS_OSC_ALL is set.
    const char* all = std::getenv("ACIDULOUS_OSC_ALL");
    local_only      = !(all && *all && std::strcmp(all, "0"));
    osc = lo_server_thread_new(port.c_str(), on_error);
    if(!osc)
        return false;
    target = core;
    {
        std::lock_guard<std::mutex> l(send_lock);
        to = lo_address_new("127.0.0.1", reply.c_str());
    }
    lo_server_thread_add_method(osc, nullptr, nullptr, on_any, nullptr);
    lo_server_thread_start(osc);
    running = true;
    pump    = new std::thread(pump_main);
    return true;
}

void stop()
{
    if(!running)
        return;
    running = false;
    pump->join();
    delete pump;
    pump = nullptr;
    // Stops the OSC thread, so no command runs after this.
    lo_server_thread_free(osc);
    osc    = nullptr;
    target = nullptr;
    std::lock_guard<std::mutex> l(send_lock);
    lo_address_free(to);
    to = nullptr;
}

} // namespace bridge
