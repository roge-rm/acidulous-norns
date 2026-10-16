// The Acidulous engine with its commands, shared by the SuperCollider UGen
// and the offline render tool. The engine is the app's EngineHost, a
// singleton, so only one Core may exist at a time. process() pulls its blocks
// through the norns AudioDriver (src/norns/drivers), which opens no device.
//
// Threads:
// - process() runs on the audio thread. It never allocates or locks.
// - command() runs on one control thread (the OSC server, or the render
//   tool's main loop). Notes, CCs, bend and pressure go straight to the
//   engine's queues from there. Everything else (mounting, patches, params,
//   replies that read files) runs in order on a worker thread, or inline when
//   the core is made with `inline_jobs`.
// - Replies go to the Replier from either the control or the worker thread.
#pragma once

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acidulous
{
class EngineHost;
namespace machine::nexus
{
class Graph;
}
namespace audition
{
struct BankPatch;
}
} // namespace acidulous

namespace core
{

// One OSC-style argument.
struct Value
{
    char        type = 'f'; // 'i', 'f' or 's'
    double      num  = 0;
    std::string str;

    static Value i(int v) { Value x; x.type = 'i'; x.num = v; return x; }
    static Value f(double v) { Value x; x.type = 'f'; x.num = v; return x; }
    static Value s(const std::string& v) { Value x; x.type = 's'; x.str = v; return x; }
};
using Args = std::vector<Value>;

// Where replies go. Called from the control and worker threads, so it must be
// safe to call from both.
struct Replier
{
    virtual ~Replier() = default;
    virtual void reply(const char* path, const Args& args) = 0;
};

constexpr int kParts = 8;

class Core
{
  public:
    Core(const std::string& bank_dir, Replier* replier, bool inline_jobs);
    ~Core();

    // Audio thread. Any n; the engine runs in 64-frame blocks behind a FIFO,
    // so the output is 64 frames late. Inputs may be null.
    void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n);
    // Audio thread, before the first process(): empties the FIFO.
    void reset_fifo();

    // Control thread. `path` without arguments, e.g. "/note".
    // Returns false for an unknown path or missing arguments.
    bool command(const std::string& path, const Args& args);

    float load_percent() const;
    // Each part's output peak since the last call, linear. Any thread.
    void levels(float* out);
    // The worst block since the last call, as a percentage of a block's length.
    float worst_percent();

    // Control thread. Back to an empty engine: all notes off, every part
    // unmounted, MPE off. Returns once that has run.
    void reset();

    // Waits until every queued job has run.
    void drain();

    // Jobs only. Waits until the mounts queued so far are in place, for
    // EngineHost's loaders, which want the machine already there. Inline (the
    // render tool) there's no audio thread, so it renders a block now, which
    // plays before the next one: everything after it is a block later.
    void settle();

  private:
    struct Part
    {
        std::string machine;
        std::string patch;
        bool        program = false; // a Formulate program is mounted
        // The machine's params as last sent, 0-1, for Cumulus's cloud.
        std::vector<float> values;
    };

    void post(std::function<void()> job);
    void post_part(int part, std::function<void()> job);
    void wait_applied();
    void worker_loop();
    void reply(const char* path, const Args& args);
    void error(const std::string& text);

    // Worker thread.
    bool mount(int part, const std::string& machine);
    void request_cloud(int part);
    void forget_cloud(int part);
    void release_object(int part);
    void load_nexus(int part, const std::string& text);
    void load(int part, const std::string& kind, const std::string& path, int arg);
    std::string load_kit(int part, const std::string& folder, int& count);
    std::string load_folder_map(int part, const std::string& folder);
    std::string load_take(int part, const std::string& path, bool sung);
    void loaded(int part, const std::string& kind, const std::string& path, int arg, const std::string& name);
    void load_error(int part, const std::string& kind, const std::string& path, int arg, const std::string& why);
    void send_presets(const std::string& path);
    void send_shape(const std::string& path);
    void hear(const std::string& path);
    void mix_hear(float* out);
    Args nexus_macros(const acidulous::machine::nexus::Graph& g, const acidulous::audition::BankPatch& patch);
    void cloud_loop();
    void build_cloud(int part, const std::vector<float>& values, uint64_t epoch);
    void unmount(int part);
    void load_patch(int part, const std::string& machine, const std::string& name);
    void send_defs(const std::string& machine);
    void send_patches(const std::string& machine);
    void send_state(int part);
    void set_param(int part, const Value& which, float v01);
    void set_channel(int part, const Value& which, float v01);
    bool push_param(int part, const char* unit, const std::string& name, float v01);
    struct BankCache;
    std::shared_ptr<BankCache> bank(const std::string& machine); // null if it can't be read

    acidulous::EngineHost&             host;
    std::string                        bank_dir;
    Replier*                           replier;
    bool                               inline_jobs;

    Part                                       parts[kParts];
    std::map<std::string, std::shared_ptr<BankCache>> banks;

    // Cumulus clouds build on their own thread, so notes and other jobs never
    // wait behind one. A request waits kCloudSettleMs for more changes, and a
    // newer one replaces it, so one build follows the last change.
    struct CloudRequest
    {
        bool                                  want = false;
        std::vector<float>                    values;
        std::chrono::steady_clock::time_point due;
        uint64_t                              epoch = 0; // bumped on each mount
        std::vector<float>                    built;     // what the cloud was last built from
    };
    std::thread             cloud_thread;
    std::mutex              cloud_lock;
    std::condition_variable cloud_cv;
    CloudRequest            clouds[kParts];
    // Held across a cloud build, across a /load and around queuing a part's
    // mount or unmount. Taken before cloud_lock.
    std::mutex              mount_locks[kParts];
    bool                    cloud_quit = false;

    std::thread                       worker;
    std::mutex                        jobs_lock;
    std::condition_variable           jobs_cv, idle_cv;
    std::deque<std::function<void()>> jobs;
    bool                              busy = false;
    bool                              quitting = false;
    std::atomic<bool>                 stopping{false};

    // Jobs queued per part, so live messages know whether to wait behind them.
    std::atomic<int> pending[kParts] = {};
    // Blocks the audio thread has finished.
    std::atomic<uint64_t> blocks{0};

    // MPE zone as last sent, for /mpe_state and routing. Control thread.
    std::atomic<int>      mpe_kind{0}, mpe_members{15};
    std::atomic<float>    mpe_bend{48};
    // For /mpe_state: what each member channel last sent the engine (note,
    // bend 0-16383, pressure, CC74), -1 for nothing yet. Control thread.
    struct Finger
    {
        int note = -1, bend = -1, pressure = -1, timbre = -1;
    };
    Finger fingers[16];

    // A file being heard from the picker: interleaved stereo at the engine
    // rate, mixed in after the engine. The worker swaps it; the audio thread
    // sets hear_busy while it reads it, so the worker waits for that to clear
    // before freeing the old one.
    std::atomic<std::vector<float>*> hear_buf{nullptr};
    std::atomic<int64_t>             hear_pos{0};
    std::atomic<bool>                hear_busy{false};

    // FIFO, audio thread only.
    float fifo_in[64 * 2]  = {};
    float fifo_out[64 * 2] = {};
    int   fifo_pos         = 0;
    // Inline only: blocks settle() rendered ahead, and whether a mount has
    // been queued since the last block.
    using Block = std::array<float, 64 * 2>;
    std::deque<Block> early;
    std::atomic<bool> mounts_queued{false};
};

} // namespace core
