// The Acidulous engine with its commands, shared by the SuperCollider UGen
// and the offline render tool.
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
class Engine;
struct ParamMessage;
}

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

  private:
    struct Part
    {
        std::string machine;
        std::string patch;
        bool        program = false; // a Formulate program is mounted
    };

    void post(std::function<void()> job);
    void post_part(int part, std::function<void()> job);
    void wait_applied();
    void worker_loop();
    void reply(const char* path, const Args& args);
    void error(const std::string& text);

    // Worker thread.
    bool mount(int part, const std::string& machine);
    void unmount(int part);
    void load_patch(int part, const std::string& machine, const std::string& name);
    void send_defs(const std::string& machine);
    void send_patches(const std::string& machine);
    void send_state(int part);
    void set_param(int part, const Value& which, float v01);
    void set_channel(int part, const Value& which, float v01);
    bool push_param(const acidulous::ParamMessage& m);
    struct BankCache;
    std::shared_ptr<BankCache> bank(const std::string& machine); // null if it can't be read

    std::unique_ptr<acidulous::Engine> engine;
    std::string                        bank_dir;
    Replier*                           replier;
    bool                               inline_jobs;

    Part                                       parts[kParts];
    std::map<std::string, std::shared_ptr<BankCache>> banks;

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

    // MPE zone, set by the worker and applied on the audio thread.
    std::atomic<uint32_t> mpe_serial{0};
    std::atomic<int>      mpe_kind{0}, mpe_members{15};
    std::atomic<float>    mpe_bend{48};
    uint32_t              mpe_applied = 0;

    // FIFO, audio thread only.
    float fifo_in[64 * 2]  = {};
    float fifo_out[64 * 2] = {};
    int   fifo_pos         = 0;
};

} // namespace core
