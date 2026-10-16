#include "core.h"

#include <EngineHost.h>
#include <drivers/AudioDriver.h>
#include <engine/core/Constants.h>
#include <engine/core/Handover.h>
#include <engine/core/Messages.h>
#include <engine/dsp/Denormals.h>
#include <engine/machine/MachineRegistry.h>
#include <engine/machine/cumulus/Cumulus.h>
#include <engine/machine/formulate/Program.h>
#include <engine/machine/nexus/Graph.h>
#include <engine/machine/nexus/Modules.h>
#include <tools/patchbank.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace core
{

using namespace acidulous;
namespace ab = acidulous::audition;

static_assert(kBlockFrames == 64, "the FIFO is sized for 64-frame blocks");
static_assert(kParts <= kRackCount, "a part is a rack");

// The channel strip params /channel takes by index, from Rack::ChannelParam.
// Any other channel param can be set by name.
static const char* const kChannelNames[] = {"gain", "pan", "mute", "solo"};

// Cumulus's params before this one shape its spectrum, so changing one
// rebuilds the cloud (Cumulus::P, and CLOUD_PARAMS in the app's EngineSync).
constexpr int32_t kCloudParams = machine::Cumulus::Morph;

struct Core::BankCache
{
    ab::Bank bank;
    bool     ok = false;
};

namespace
{

template <typename T> void delete_as(void* p)
{
    delete static_cast<T*>(p);
}

int to_int(const Value& v)
{
    return v.type == 's' ? std::atoi(v.str.c_str()) : int(std::lround(v.num));
}

float to_float(const Value& v)
{
    return v.type == 's' ? float(std::atof(v.str.c_str())) : float(v.num);
}

std::string to_str(const Value& v)
{
    if(v.type == 's')
        return v.str;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v.num);
    return buf;
}

uint8_t clamp7(int v)
{
    return uint8_t(v < 0 ? 0 : v > 127 ? 127 : v);
}

bool lower_equal(const std::string& a, const std::string& b)
{
    if(a.size() != b.size())
        return false;
    for(size_t i = 0; i < a.size(); ++i)
        if(std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

} // namespace

Core::Core(const std::string& bank_dir_, Replier* replier_, bool inline_jobs_)
: host(EngineHost::instance()), bank_dir(bank_dir_), replier(replier_), inline_jobs(inline_jobs_)
{
    // Every track renders on the audio thread, since scsynth wants its core.
    // ACIDULOUS_WORKERS in the environment overrides it.
    setenv("ACIDULOUS_WORKERS", "0", 0);
    host.start();
    // The master fader at its top, 1.5, which is 5.5 dB over the app's
    // default of 0.8, so one Reflux part peaks around -9 dBFS. The limiter
    // (on, ceiling 0.95) keeps four loud parts from clipping.
    host.setParam(0, "master", "volume", 1.f, false);
    if(!inline_jobs)
    {
        worker       = std::thread([this] { worker_loop(); });
        cloud_thread = std::thread([this] { cloud_loop(); });
    }
}

Core::~Core()
{
    stopping = true;
    if(cloud_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> l(cloud_lock);
            cloud_quit = true;
        }
        cloud_cv.notify_all();
        cloud_thread.join();
    }
    if(worker.joinable())
    {
        {
            std::lock_guard<std::mutex> l(jobs_lock);
            quitting = true;
        }
        jobs_cv.notify_all();
        worker.join();
    }
    // Empties the racks so the next Core starts clean: the unmounts apply at
    // the top of a block, pulled here since the UGen no longer runs this core.
    for(int p = 0; p < kParts; ++p)
        host.unmountMachine(p);
    host.setMpeZone(0, 15, 48);
    // The UGen has stopped calling process(), but a pull may still be
    // finishing, so this tries for up to 50 ms.
    float in[kBlockFrames * 2] = {}, out[kBlockFrames * 2];
    for(int tries = 0; tries < 50 && !AudioDriver::pull(in, out); ++tries)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    host.stop();
}

// --- Audio thread ------------------------------------------------------------

void Core::reset_fifo()
{
    std::memset(fifo_in, 0, sizeof(fifo_in));
    std::memset(fifo_out, 0, sizeof(fifo_out));
    fifo_pos = 0;
}

void Core::process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n)
{
    dsp::flushDenormalsOnce();
    for(int i = 0; i < n; ++i)
    {
        fifo_in[fifo_pos * 2]     = in_l ? in_l[i] : 0.f;
        fifo_in[fifo_pos * 2 + 1] = in_r ? in_r[i] : 0.f;
        out_l[i]                  = fifo_out[fifo_pos * 2];
        out_r[i]                  = fifo_out[fifo_pos * 2 + 1];
        if(++fifo_pos == kBlockFrames)
        {
            // Silence while EngineHost has the driver stopped (an offline
            // render or a freeze). Blocks settle() rendered early go out
            // first; that only happens inline, in the render tool.
            if(early.empty())
            {
                if(!AudioDriver::pull(fifo_in, fifo_out))
                    std::memset(fifo_out, 0, sizeof(fifo_out));
            }
            else
            {
                Block b;
                if(!AudioDriver::pull(fifo_in, b.data()))
                    b.fill(0.f);
                early.push_back(b);
                std::memcpy(fifo_out, early.front().data(), sizeof(fifo_out));
                early.pop_front();
            }
            mounts_queued = false;
            blocks.fetch_add(1, std::memory_order_release);
            fifo_pos = 0;
        }
    }
}

float Core::load_percent() const
{
    return host.loadPercent();
}

float Core::worst_percent()
{
    const float block_us = 1e6f * kBlockFrames / kSampleRate;
    return 100.f * float(host.worstBlockUs()) / block_us;
}

// --- Control thread ----------------------------------------------------------

void Core::reply(const char* path, const Args& args)
{
    if(replier)
        replier->reply(path, args);
}

void Core::error(const std::string& text)
{
    std::fprintf(stderr, "acidulous: %s\n", text.c_str());
    reply("/error", {Value::s(text)});
}

void Core::post(std::function<void()> job)
{
    if(inline_jobs)
    {
        job();
        return;
    }
    {
        std::lock_guard<std::mutex> l(jobs_lock);
        jobs.push_back(std::move(job));
    }
    jobs_cv.notify_one();
}

void Core::post_part(int part, std::function<void()> job)
{
    pending[part].fetch_add(1, std::memory_order_acq_rel);
    post([this, part, job = std::move(job)] {
        job();
        pending[part].fetch_sub(1, std::memory_order_acq_rel);
    });
}

// A queued mount is applied at the start of the next block that starts after
// it, so two finished blocks are enough. If nothing is rendering this gives up
// after half a second. The render tool runs jobs inline between blocks, where
// the order is already right.
void Core::wait_applied()
{
    if(inline_jobs)
        return;
    const uint64_t want = blocks.load(std::memory_order_acquire) + 2;
    for(int tries = 0; tries < 250 && !stopping && blocks.load(std::memory_order_acquire) < want; ++tries)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void Core::settle()
{
    if(!inline_jobs)
    {
        wait_applied();
        return;
    }
    if(!mounts_queued)
        return;
    float in[kBlockFrames * 2] = {};
    Block b;
    if(!AudioDriver::pull(in, b.data()))
        b.fill(0.f);
    early.push_back(b);
    mounts_queued = false;
    blocks.fetch_add(1, std::memory_order_release);
}

void Core::reset()
{
    host.panic();
    mpe_kind.store(0);
    mpe_members.store(15);
    host.setMpeZone(0, 15, mpe_bend.load());
    for(int p = 0; p < kParts; ++p)
        post_part(p, [this, p] { unmount(p); });
    drain();
}

void Core::drain()
{
    if(inline_jobs)
        return;
    std::unique_lock<std::mutex> l(jobs_lock);
    idle_cv.wait(l, [this] { return jobs.empty() && !busy; });
}

void Core::worker_loop()
{
    std::unique_lock<std::mutex> l(jobs_lock);
    for(;;)
    {
        jobs_cv.wait(l, [this] { return quitting || !jobs.empty(); });
        if(quitting)
            return;
        auto job = std::move(jobs.front());
        jobs.pop_front();
        busy = true;
        l.unlock();
        job();
        l.lock();
        busy = false;
        if(jobs.empty())
            idle_cv.notify_all();
    }
}

bool Core::command(const std::string& path, const Args& a)
{
    const auto part_ok = [&](int p) {
        if(p >= 0 && p < kParts)
            return true;
        error("no part " + std::to_string(p));
        return false;
    };
    // Channels are 1-16 as norns numbers them; the engine wants 0-15.
    const auto chan = [&](const Value& v) { return uint8_t(std::max(0, std::min(15, to_int(v) - 1))); };
    const auto member = [&](uint8_t ch) {
        const int kind = mpe_kind.load(), members = mpe_members.load();
        if(kind == 1)
            return ch >= 1 && ch <= members;
        if(kind == 2)
            return ch <= 14 && ch >= 15 - members;
        return false;
    };
    // Messages for a part with a mount or patch still on the worker wait behind
    // it, in order. Otherwise they go straight to the engine.
    const auto live = [&](int part, std::function<void()> fn) {
        if(inline_jobs || pending[part].load(std::memory_order_acquire) == 0)
            fn();
        else
            post_part(part, std::move(fn));
    };
    // The mod wheel, pedals and channel pressure go as performance params,
    // as the app does, so the machine sees them the same way.
    // EngineHost sends CC 1, 64, 66, 67 and pressure that way.
    const auto perf_cc = [&](int part, uint8_t cc, uint8_t value) {
        live(part, [this, part, cc, value] { host.controlChange(part, cc, value, false); });
    };
    const auto perf_pressure = [&](int part, uint8_t value) {
        live(part, [this, part, value] { host.channelPressure(part, value, false); });
    };
    // Anything else goes in as MIDI on its channel.
    const auto midi = [&](int part, const MidiMessage& m) {
        live(part, [this, part, m] { host.midiEvent(part, m.status, m.data1, m.data2, m.channel); });
    };

    if(path == "/note" && a.size() >= 4)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]), note = clamp7(to_int(a[2])), vel = clamp7(to_int(a[3]));
        const uint8_t status = uint8_t((vel ? 0x90 : 0x80) | part);
        const MidiMessage m{status, note, vel, ch};
        if(member(ch))
        {
            if(vel)
                fingers[ch] = Finger{note, -1, -1, -1};
            else if(fingers[ch].note == note)
                fingers[ch].note = -1;
        }
        midi(part, m);
        return true;
    }
    if(path == "/cc" && a.size() >= 4)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]), num = clamp7(to_int(a[2])), val = clamp7(to_int(a[3]));
        if(!member(ch) && (num == 1 || num == 64 || num == 66 || num == 67))
            perf_cc(part, num, val);
        else
        {
            const MidiMessage m{uint8_t(0xb0 | part), num, val, ch};
            if(num == 74 && member(ch))
                fingers[ch].timbre = val;
            midi(part, m);
        }
        return true;
    }
    if(path == "/bend" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        int v = to_int(a[2]);
        v     = v < 0 ? 0 : v > 16383 ? 16383 : v;
        const MidiMessage m{uint8_t(0xe0 | part), uint8_t(v & 0x7f), uint8_t(v >> 7), chan(a[1])};
        if(member(m.channel))
            fingers[m.channel].bend = v;
        midi(part, m);
        return true;
    }
    if(path == "/pressure" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(!part_ok(part))
            return true;
        const uint8_t ch = chan(a[1]);
        if(member(ch))
        {
            const MidiMessage m{uint8_t(0xd0 | part), clamp7(to_int(a[2])), 0, ch};
            fingers[ch].pressure = m.data1;
            midi(part, m);
        }
        else
            perf_pressure(part, clamp7(to_int(a[2])));
        return true;
    }
    if(path == "/mount" && a.size() >= 2)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, m = to_str(a[1])] {
                if(mount(part, m) && parts[part].machine == "Cumulus")
                    request_cloud(part);
            });
        return true;
    }
    if(path == "/unmount" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { unmount(part); });
        return true;
    }
    if(path == "/param" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, which = a[1], v = to_float(a[2])] { set_param(part, which, v); });
        return true;
    }
    if(path == "/channel" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, which = a[1], v = to_float(a[2])] { set_channel(part, which, v); });
        return true;
    }
    if(path == "/patch" && a.size() >= 3)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part, m = to_str(a[1]), n = to_str(a[2])] { load_patch(part, m, n); });
        return true;
    }
    if(path == "/state" && a.size() >= 1)
    {
        const int part = to_int(a[0]);
        if(part_ok(part))
            post_part(part, [this, part] { send_state(part); });
        return true;
    }
    if(path == "/defs" && a.size() >= 1)
    {
        post([this, m = to_str(a[0])] { send_defs(m); });
        return true;
    }
    if(path == "/patches" && a.size() >= 1)
    {
        post([this, m = to_str(a[0])] { send_patches(m); });
        return true;
    }
    if(path == "/machines")
    {
        Args out;
        for(int32_t i = 0; i < MachineRegistry::count(); ++i)
            out.push_back(Value::s(MachineRegistry::name(i)));
        reply("/machines", out);
        return true;
    }
    if(path == "/mpe" && a.size() >= 3)
    {
        // members 0 turns MPE off. The zone is the lower one: master channel
        // 1, members from 2 up. The part only matters to Lua's routing.
        const int members = to_int(a[1]);
        mpe_kind.store(members > 0 ? 1 : 0);
        mpe_members.store(members > 0 ? std::min(members, 15) : 15);
        mpe_bend.store(to_float(a[2]));
        // Here rather than on the audio thread, as the app does, so the routing
        // below and EngineHost's agree from the next message on.
        host.setMpeZone(mpe_kind.load(), mpe_members.load(), mpe_bend.load());
        for(Finger& f : fingers)
            f = Finger{};
        return true;
    }
    if(path == "/mpe_state")
    {
        // After everything queued so far, so the engine has seen it. The held
        // mask is read off the audio thread, which is fine for a test readout.
        post([this, f = std::vector<Finger>(fingers, fingers + 16)] {
            wait_applied();
            Args out{Value::i(mpe_kind.load()), Value::i(mpe_members.load()), Value::f(mpe_bend.load()),
                     Value::i(host.mpeHeldMask())};
            for(int c = 0; c < 16; ++c)
                if(f[size_t(c)].note >= 0)
                    for(int v : {c + 1, f[size_t(c)].note, f[size_t(c)].bend, f[size_t(c)].pressure, f[size_t(c)].timbre})
                        out.push_back(Value::i(v));
            reply("/mpe_state", out);
        });
        return true;
    }
    return false;
}

// --- Worker thread -----------------------------------------------------------

bool Core::mount(int part, const std::string& name)
{
    // The registry's own spelling of the name, which the replies use.
    int32_t i = 0;
    while(i < MachineRegistry::count() && !lower_equal(MachineRegistry::name(i), name))
        ++i;
    if(i == MachineRegistry::count())
    {
        error("no machine " + name);
        return false;
    }
    const std::string type = MachineRegistry::name(i);
    mounts_queued = true;
    if(!host.mountMachine(part, type))
    {
        error("mount queue full");
        return false;
    }
    wait_applied();
    forget_cloud(part);
    parts[part] = Part{};
    parts[part].machine = type;
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(type.c_str(), count);
    for(int32_t k = 0; k < count; ++k)
        parts[part].values.push_back(defs[k].unmap(defs[k].def));
    reply("/mounted", {Value::i(part), Value::s(parts[part].machine)});
    return true;
}

void Core::unmount(int part)
{
    mounts_queued = true;
    host.unmountMachine(part);
    wait_applied();
    forget_cloud(part);
    parts[part] = Part{};
    reply("/mounted", {Value::i(part), Value::s("")});
}

void Core::set_param(int part, const Value& which, float v01)
{
    const Part& p = parts[part];
    if(p.machine.empty())
        return;
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(p.machine.c_str(), count);
    int32_t         index = -1;
    if(which.type != 's')
        index = to_int(which);
    else
        for(int32_t i = 0; i < count; ++i)
            if(which.str == defs[i].name)
                index = i;
    if(index < 0 || index >= count)
    {
        error(p.machine + " has no param " + to_str(which));
        return;
    }
    if(!push_param(part, "machine", defs[index].name, v01))
        return;
    if(size_t(index) < p.values.size())
        parts[part].values[size_t(index)] = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
    if(p.machine == "Cumulus" && index < kCloudParams)
        request_cloud(part);
}

// How long a cloud request waits for more changes before it builds, so a
// patch followed by its edits (a PSET) builds once.
constexpr int kCloudSettleMs = 50;

// Cumulus is silent until it has a cloud, built from its spectrum params and
// mounted. Worker thread. Inline (the render tool) it builds now.
void Core::request_cloud(int part)
{
    const Part&        p = parts[part];
    std::vector<float> values(p.values.begin(), p.values.begin() + std::min<size_t>(p.values.size(), kCloudParams));
    if(inline_jobs)
    {
        settle();
        if(values != clouds[part].built)
            build_cloud(part, values, clouds[part].epoch);
        return;
    }
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        CloudRequest& c = clouds[part];
        c.want   = true;
        c.values = std::move(values);
        c.due    = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloudSettleMs);
    }
    cloud_cv.notify_one();
}

// After a mount or unmount: the new machine has no cloud, and a waiting
// request was for the old one. Worker thread.
void Core::forget_cloud(int part)
{
    std::lock_guard<std::mutex> l(cloud_lock);
    CloudRequest& c = clouds[part];
    c.want = false;
    c.built.clear();
    ++c.epoch;
}

// Cloud thread, or the render tool's main loop.
void Core::build_cloud(int part, const std::vector<float>& values, uint64_t epoch)
{
    const std::string why = host.buildCloud(part, values.data(), int32_t(values.size()));
    {
        std::lock_guard<std::mutex> l(cloud_lock);
        if(clouds[part].epoch != epoch)
            return; // remounted while it built: a request for the new one follows
        if(why.empty())
            clouds[part].built = values;
    }
    if(!why.empty())
        error("Cumulus: " + why);
}

void Core::cloud_loop()
{
    std::unique_lock<std::mutex> l(cloud_lock);
    for(;;)
    {
        if(cloud_quit)
            return;
        int  next = -1;
        auto now  = std::chrono::steady_clock::now();
        for(int p = 0; p < kParts; ++p)
            if(clouds[p].want && (next < 0 || clouds[p].due < clouds[next].due))
                next = p;
        if(next < 0)
        {
            cloud_cv.wait(l);
            continue;
        }
        if(clouds[next].due > now)
        {
            cloud_cv.wait_until(l, clouds[next].due);
            continue;
        }
        CloudRequest&      c      = clouds[next];
        c.want                    = false;
        std::vector<float> values = c.values;
        const uint64_t     epoch  = c.epoch;
        if(values == c.built)
            continue;
        l.unlock();
        build_cloud(next, values, epoch);
        l.lock();
    }
}

// A Nexus patch's graph, a line per module and cable, built and mounted by
// EngineHost.
void Core::load_nexus(int part, const std::string& text)
{
    settle();
    const std::string why = host.loadNexusPatch(part, text);
    if(!why.empty())
        error("Nexus: " + why);
}

// The track's channel strip, in Rack::ChannelParam's order. Applies with or
// without a machine.
void Core::set_channel(int part, const Value& which, float v01)
{
    std::string name;
    if(which.type == 's')
        name = which.str;
    else if(to_int(which) >= 0 && to_int(which) < 4)
        name = kChannelNames[to_int(which)];
    if(name.empty() || host.paramIndex("", "channel", name) < 0)
    {
        error("no channel param " + to_str(which));
        return;
    }
    push_param(part, "channel", name, v01);
}

// The engine's param queue holds 512 and the audio thread empties it every
// block, so when it's full this waits for blocks to render and tries again.
// It gives up only if no block renders for a second. Worker thread; inline
// jobs (the render tool) run between blocks, so they can't wait.
// The name must be one the unit has, so a false from setParam means a full queue.
bool Core::push_param(int part, const char* unit, const std::string& name, float v01)
{
    const float v = v01 < 0.f ? 0.f : v01 > 1.f ? 1.f : v01;
    const auto  push = [&] { return host.setParam(part, unit, name, v, false); };
    if(push())
        return true;
    if(!inline_jobs)
    {
        uint64_t seen = blocks.load(std::memory_order_acquire);
        int      idle = 0;
        while(!stopping && idle < 1000)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(push())
                return true;
            const uint64_t now = blocks.load(std::memory_order_acquire);
            if(now != seen)
                seen = now, idle = 0;
            else
                ++idle;
        }
    }
    error("param queue full");
    return false;
}

void Core::levels(float* out)
{
    for(int p = 0; p < kParts; ++p)
        out[p] = host.rackPeak(p);
}

std::shared_ptr<Core::BankCache> Core::bank(const std::string& machine)
{
    auto it = banks.find(machine);
    if(it != banks.end())
        return it->second;
    auto        cache = std::make_shared<BankCache>();
    std::string why;
    cache->ok = ab::readBank(bank_dir + "/" + machine + ".bank", cache->bank, why);
    if(!cache->ok)
    {
        error(why);
        return nullptr;
    }
    banks[machine] = cache;
    return cache;
}

void Core::load_patch(int part, const std::string& machine, const std::string& name)
{
    const auto b = bank(machine);
    if(!b)
        return;
    const ab::BankPatch* patch = nullptr;
    for(const auto& p : b->bank.patches)
        if(p.name == name)
            patch = &p;
    if(!patch)
        for(const auto& p : b->bank.patches)
            if(lower_equal(p.name, name))
                patch = &p;
    if(!patch)
    {
        error("no " + machine + " patch " + name);
        return;
    }
    if(parts[part].machine != machine && !mount(part, machine))
        return;

    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    ab::Resolved    r     = ab::resolve(*patch, defs, count);
    for(const auto& p : r.problems)
        std::fprintf(stderr, "acidulous: %s %s: %s\n", machine.c_str(), patch->name.c_str(), p.c_str());

    // A Nexus patch names only the knobs it changes; the rest take their
    // module's default from the graph, since every slot knob defaults to zero
    // and an oscillator at level zero is silent. As the app's audition does.
    std::string nexus;
    Args        macros;
    if(machine == "Nexus")
    {
        for(const auto& kv : r.settings)
            if(kv.first == "nexus")
                nexus = kv.second;
        std::string why;
        std::unique_ptr<machine::nexus::Graph> g;
        if(!nexus.empty())
            g.reset(machine::nexus::Graph::parse(nexus, float(kSampleRate), why));
        if(g)
        {
            namespace nx = machine::nexus;
            std::vector<float> knobs(size_t(nx::kSlots) * nx::kKnobs, 0.f);
            g->defaultKnobs(knobs.data());
            std::set<std::string> named;
            for(const auto& v : patch->values)
                named.insert(v.name);
            for(int32_t i = 0; i < count; ++i)
            {
                const std::string n = defs[i].name;
                if(n.size() < 6 || n[0] != 's' || n[3] != '_' || n[4] != 'p' || named.count(n))
                    continue;
                const int slot = (n[1] - '0') * 10 + (n[2] - '0'), knob = n[5] - '1';
                if(slot >= 0 && slot < nx::kSlots && knob >= 0 && knob < nx::kKnobs)
                    r.norm[size_t(i)] = defs[i].unmap(knobs[size_t(slot) * nx::kKnobs + size_t(knob)]);
            }
            macros = nexus_macros(*g, *patch);
        }
        else
        {
            error(machine + " " + patch->name + ": " + (nexus.empty() ? std::string("no graph") : why));
            nexus.clear();
        }
    }

    // Every param, so nothing is left over from the last patch. mount() has
    // waited for the new machine to be in place.
    for(int32_t i = 0; i < count; ++i)
        if(!push_param(part, "machine", defs[i].name, r.norm[size_t(i)]))
            return;
    parts[part].values = r.norm;

    // Formulate's formula and tables are compiled here and swapped in. A
    // patch without them clears the last one, which plays the plain oscillator.
    if(machine == "Formulate")
    {
        std::string formula, arp, duty, vol;
        for(const auto& kv : r.settings)
        {
            if(kv.first == "formula")
                formula = kv.second;
            else if(kv.first == "arp")
                arp = kv.second;
            else if(kv.first == "duty")
                duty = kv.second;
            else if(kv.first == "vol")
                vol = kv.second;
        }
        std::unique_ptr<machine::formulate::Program> program;
        if(!(formula.empty() && arp.empty() && duty.empty() && vol.empty()))
        {
            std::string why;
            program = machine::formulate::compile(formula, arp, duty, vol, why);
            if(!program)
                error(machine + " " + patch->name + ": " + why);
        }
        if(program || parts[part].program)
        {
            Mount mt;
            mt.kind    = Mount::Kind::Object;
            mt.rack    = part;
            mt.slot    = 0;
            mt.object  = program.get();
            mt.deleter = delete_as<machine::formulate::Program>;
            // Frees the program itself if the queue stays full.
            const bool had = program != nullptr;
            program.release();
            mounts_queued = true;
            if(host.mountObjectWithRetry(mt))
                parts[part].program = had;
            else
                error("mount queue full");
        }
    }
    else if(machine == "Nexus")
    {
        if(!nexus.empty())
            load_nexus(part, nexus);
        else
        {
            // No graph: clear the last patch's, so the part is silent.
            Mount mt;
            mt.kind    = Mount::Kind::Object;
            mt.rack    = part;
            mt.slot    = 0;
            mt.object  = nullptr;
            mt.deleter = delete_as<machine::nexus::Graph>;
            settle();
            if(!host.mountObjectWithRetry(mt))
                error("mount queue full");
        }
    }
    else if(machine == "Cumulus")
        request_cloud(part);
    else if(!r.settings.empty())
        std::fprintf(stderr, "acidulous: %s %s: settings are not supported yet\n", machine.c_str(),
                     patch->name.c_str());

    parts[part].patch = patch->name;
    if(!macros.empty())
    {
        Args out{Value::i(part), Value::s(machine), Value::s(patch->name)};
        out.insert(out.end(), macros.begin(), macros.end());
        reply("/macros", out);
    }
    Args values{Value::i(part), Value::s(machine), Value::s(patch->name)};
    for(int32_t i = 0; i < count; ++i)
        values.push_back(Value::f(r.norm[size_t(i)]));
    reply("/values", values);
    reply("/patch", {Value::i(part), Value::s(machine), Value::s(patch->name)});
}

// A Nexus patch's macros: the knobs the patch sets, in its order, then the
// knobs of the modules in its graph, as param and label pairs, up to five.
Args Core::nexus_macros(const machine::nexus::Graph& g, const audition::BankPatch& patch)
{
    namespace nx = machine::nexus;
    const char* knob_name[nx::kSlots][nx::kKnobs] = {};
    for(int32_t i = 0; i < g.nodeCount(); ++i)
    {
        const auto& n = g.nodeAt(i);
        if(n.slot < 0 || n.slot >= nx::kSlots || n.unknown)
            continue;
        const auto& info = nx::infoFor(n.type);
        for(int k = 0; k < nx::kKnobs; ++k)
            if(info.knob[k] && *info.knob[k])
                knob_name[n.slot][k] = info.knob[k];
    }
    std::vector<std::pair<int, int>> picks;
    const auto add = [&](int slot, int knob) {
        if(picks.size() < 5 && knob_name[slot][knob] &&
           std::find(picks.begin(), picks.end(), std::make_pair(slot, knob)) == picks.end())
            picks.push_back({slot, knob});
    };
    for(const auto& v : patch.values)
    {
        const std::string& n = v.name;
        if(n.size() != 6 || n[0] != 's' || n[3] != '_' || n[4] != 'p' || !std::isdigit((unsigned char)n[1]) ||
           !std::isdigit((unsigned char)n[2]))
            continue;
        const int slot = (n[1] - '0') * 10 + (n[2] - '0'), knob = n[5] - '1';
        if(slot < nx::kSlots && knob >= 0 && knob < nx::kKnobs)
            add(slot, knob);
    }
    // Then the graph's own modules, leaving out its voice and output.
    for(int32_t i = 0; i < g.nodeCount(); ++i)
    {
        const auto& n = g.nodeAt(i);
        if(n.slot < 0 || n.slot >= nx::kSlots || n.unknown)
            continue;
        const char* type = nx::infoFor(n.type).name;
        if(type && (!std::strcmp(type, "voice") || !std::strcmp(type, "out")))
            continue;
        for(int k = 0; k < nx::kKnobs; ++k)
            add(n.slot, k);
    }
    Args                  out;
    std::set<std::string> labels;
    for(const auto& pk : picks)
    {
        char param[32];
        std::snprintf(param, sizeof(param), "s%02d_p%d", pk.first, pk.second + 1);
        std::string label = std::string(knob_name[pk.first][pk.second]).substr(0, 6);
        if(labels.count(label))
            label = (std::to_string(pk.first) + knob_name[pk.first][pk.second]).substr(0, 6);
        labels.insert(label);
        out.push_back(Value::s(param));
        out.push_back(Value::s(label));
    }
    return out;
}

// What the engine holds for the part once everything queued before has been
// applied, for tests: the gain and the machine's params, 0-1.
void Core::send_state(int part)
{
    wait_applied();
    Args out{Value::i(part), Value::s(parts[part].machine), Value::s(parts[part].patch),
             Value::f(host.paramNormalized(part, "channel", "gain"))};
    int32_t count = 0;
    if(const ParamDef* defs = MachineRegistry::paramDefs(host.mountedMachine(part), count))
        for(int32_t i = 0; i < count; ++i)
            out.push_back(Value::f(host.paramNormalized(part, "machine", defs[i].name)));
    reply("/state", out);
}

void Core::send_defs(const std::string& machine)
{
    int32_t         count = 0;
    const ParamDef* defs  = MachineRegistry::paramDefs(machine.c_str(), count);
    if(!defs)
    {
        error("no machine " + machine);
        count = 0;
    }
    for(int32_t i = 0; i < count; ++i)
    {
        const ParamDef& d = defs[i];
        reply("/def", {Value::s(machine), Value::i(i), Value::s(d.name), Value::f(d.min), Value::f(d.max),
                       Value::f(d.def), Value::s(d.unit ? d.unit : ""),
                       Value::i(d.curve == Curve::Stepped ? d.steps : 0),
                       Value::s(d.curve == Curve::Exponential ? "exp" : d.curve == Curve::Stepped ? "step" : "lin")});
    }
    reply("/defs_end", {Value::s(machine), Value::i(count)});
}

void Core::send_patches(const std::string& machine)
{
    const auto b = bank(machine);
    Args       out{Value::s(machine)};
    if(b)
        for(const auto& p : b->bank.patches)
            out.push_back(Value::s(p.name));
    reply("/patches", out);
}

} // namespace core
