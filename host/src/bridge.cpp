#include "bridge.h"

#include "core.h"

#include <lo/lo.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace bridge
{
namespace
{

lo_server_thread  osc = nullptr;
lo_address        to  = nullptr;
std::mutex        send_lock; // replies come from the OSC thread and the core's worker
std::thread       pump;
std::atomic<bool> running{false};
core::Core*       target = nullptr;

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
int on_any(const char* path, const char* types, lo_arg** a, int argc, lo_message, void*)
{
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
    while(running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if(!running)
            break;
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
    pump    = std::thread(pump_main);
    return true;
}

void stop()
{
    if(!running)
        return;
    running = false;
    pump.join();
    lo_server_thread_free(osc);
    osc = nullptr;
    std::lock_guard<std::mutex> l(send_lock);
    lo_address_free(to);
    to = nullptr;
}

} // namespace bridge
