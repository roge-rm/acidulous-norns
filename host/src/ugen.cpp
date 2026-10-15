// The Acidulous engine inside SuperCollider, as the UGen Acidulous.
//
// Plugin command, sent before the first synth:
//   /cmd AcidulousStart s:bank_folder [i:osc_port]
// makes the engine and its OSC bridge (bridge.h) on scsynth's non-real-time
// thread. The UGen then runs it:
//   Acidulous.ar(in_l, in_r) -> [out_l, out_r]
// Only one instance runs the engine; others output silence. The engine is
// fixed at 48 kHz and runs in 64-frame blocks behind a FIFO, so any scsynth
// block size works, with 64 frames of extra latency.
#include "SC_PlugIn.h"

#include "bridge.h"
#include "core.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef ACIDULOUS_OSC_PORT
#define ACIDULOUS_OSC_PORT 57150
#endif

static InterfaceTable* ft;

namespace
{

std::atomic<core::Core*> engine{nullptr};
std::atomic<Unit*>       owner{nullptr};

struct StartCmd
{
    char banks[512];
    int  port;
};

bool start_stage2(World* world, void* data)
{
    auto* cmd = static_cast<StartCmd*>(data);
    if(engine.load())
        return true;
    if(world->mSampleRate != 48000.0)
        std::fprintf(stderr, "acidulous: scsynth runs at %g Hz; the engine is 48 kHz and will be off pitch\n",
                     world->mSampleRate);
    auto* c = new core::Core(cmd->banks, bridge::replier_for_core(), false);
    if(!bridge::start(c, std::to_string(cmd->port), "10111"))
        std::fprintf(stderr, "acidulous: can't open OSC port %d\n", cmd->port);
    engine.store(c);
    return true;
}

void start_cleanup(World* world, void* data)
{
    RTFree(world, data);
}

void start_cmd(World* world, void*, sc_msg_iter* args, void* reply_addr)
{
    const char* banks = args->gets("");
    const int   port  = args->geti(ACIDULOUS_OSC_PORT);
    auto*       cmd   = static_cast<StartCmd*>(RTAlloc(world, sizeof(StartCmd)));
    if(!cmd)
        return;
    std::strncpy(cmd->banks, banks, sizeof(cmd->banks) - 1);
    cmd->banks[sizeof(cmd->banks) - 1] = 0;
    cmd->port                          = port;
    DoAsynchronousCommand(world, reply_addr, "AcidulousStart", cmd, start_stage2, nullptr, nullptr,
                          start_cleanup, 0, nullptr);
}

} // namespace

struct Acidulous : public Unit
{
};

static void Acidulous_next(Acidulous* unit, int n)
{
    float*      outl = OUT(0);
    float*      outr = OUT(1);
    core::Core* c    = engine.load(std::memory_order_acquire);
    if(!c || owner.load() != unit)
    {
        std::memset(outl, 0, n * sizeof(float));
        std::memset(outr, 0, n * sizeof(float));
        return;
    }
    c->process(IN(0), IN(1), outl, outr, n);
}

static void Acidulous_Ctor(Acidulous* unit)
{
    Unit* none = nullptr;
    if(owner.compare_exchange_strong(none, unit))
    {
        if(core::Core* c = engine.load(std::memory_order_acquire))
            c->reset_fifo();
    }
    SETCALC(Acidulous_next);
    OUT0(0) = 0.f;
    OUT0(1) = 0.f;
}

static void Acidulous_Dtor(Acidulous* unit)
{
    Unit* me = unit;
    owner.compare_exchange_strong(me, nullptr);
}

PluginLoad(Acidulous)
{
    ft = inTable;
    DefineDtorUnit(Acidulous);
    DefinePlugInCmd("AcidulousStart", start_cmd, nullptr);
}
