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
#include <utility>
#include <vector>

#include <engine/core/Constants.h>
#include <engine/core/PatchChain.h>

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
// Send buses: 0 reverb, 1 delay.
constexpr int kSends = 2;
// Modifier slots a part has.
constexpr int kModSlots = acidulous::kInputModSlots;

// The bytes a file takes decoded to floats: at `to_rate` (0 keeps its own),
// the first `max_seconds` (0 all of it), mono as one channel unless `both`.
// From the header for WAV, AIFF and FLAC, otherwise eight times the file's size.
int64_t decoded_bytes(const std::string& path, int to_rate, int max_seconds, bool both);

class Core
{
  public:
    static constexpr int kHeldSlots = 16;
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
    // The part's Trinity macro names, joined by newlines, "" for none. For tests.
    std::string macro_names(int part) const;
    // Each part's output peak since the last call, linear. Any thread.
    void levels(float* out);
    // The worst block since the last call, as a percentage of a block's length.
    float worst_percent();
    // Whether /meters asked for the master's meters. Any thread.
    bool metering_on() const { return metering.load(); }
    // The master's peak since the last call, linear, and its short-term
    // loudness in LUFS. Measuring loudness costs the audio thread a little,
    // and it stops a moment after the last call. One caller only.
    void master_meters(float& peak, float& lufs);
    // The open scope's trace, up to max points from -1 to 1, and its part.
    // 0 when no scope is open. One caller only.
    // slot is the module it's cabled from.
    int scope_trace(float* out, int max, int& part, int& slot);

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
        std::string        nexus;           // a Nexus patch's graph as loaded
        int                scope_slot = -1; // the module the scope is cabled from
        // A Formulate patch's formula, arp, duty and vol as loaded, for saving.
        std::vector<std::pair<std::string, std::string>> program_settings;
        // Mosaic's zones from a folder or a preset, one line per zone with its
        // absolute path, as EngineHost::loadZoneMap takes them. "" for none.
        std::string zones;
        int         low = -1, high = -1; // the loaded patch's note range, -1 for none
    };

    // A patch to put on a part, from a bank or a preset file.
    struct PatchSource
    {
        std::string                                      name;
        std::vector<float>                               norm;     // every machine param, 0-1
        std::vector<std::pair<std::string, std::string>> settings;
        std::vector<std::string>                         named;    // the params it names, in its order
        acidulous::PatchChain                            chain;
        int                                              low = -1, high = -1;
    };

    void post(std::function<void()> job);
    void post_part(int part, std::function<void()> job);
    void wait_applied();
    void worker_loop();
    void reply(const char* path, const Args& args);
    void error(const std::string& text);
    void send_macro_names(int part);

    // Worker thread.
    bool mount(int part, const std::string& machine);
    void request_cloud(int part);
    void forget_cloud(int part);
    void clear_objects(int part);
    bool load_nexus(int part, const std::string& text);
    void load(int part, const std::string& kind, const std::string& path, int arg,
              const std::vector<std::string>& roots = {});
    void load_now(int part, const std::string& kind, const std::string& path, int arg,
                  const std::vector<std::string>& roots);
    int64_t held_total();
    bool room_for(int64_t need, std::string& why);
    void set_held(int part, int slot, int64_t bytes);
    void clear_held(int part);
    std::string load_kit(int part, const std::string& folder, int& count);
    std::string load_folder_map(int part, const std::string& folder);
    std::string load_take(int part, const std::string& path, bool sung);
    void loaded(int part, const std::string& kind, const std::string& path, int arg, const std::string& name);
    void load_error(int part, const std::string& kind, const std::string& path, int arg, const std::string& why);
    void send_presets(const std::string& path);
    void send_shape(const std::string& path);
    void hear(const std::string& path);
    void mix_hear(float* out);
    Args nexus_macros(const acidulous::machine::nexus::Graph& g, const std::vector<std::string>& named);
    void cloud_loop();
    void build_cloud(int part, const std::vector<float>& values, uint64_t epoch);
    void unmount(int part);
    void load_patch(int part, const std::string& machine, const std::string& name);
    void patch_failed(int part, const std::string& what);
    bool apply_patch(int part, const std::string& machine, PatchSource& src, const std::string& tag);
    void load_preset(int part, const std::string& path, int index, const std::string& tag, bool files,
                     const std::string& want_machine, const std::string& want_name,
                     const std::vector<std::string>& roots);
    void patch_failed_on(int part, const std::string& machine, const std::string& tag, const std::string& what);
    void load_preset_files(int part, const std::string& path, int index,
                           const std::vector<std::pair<std::string, std::string>>& settings,
                           const std::vector<std::string>& roots);
    void save_preset(int part, const std::string& path, const std::string& name,
                     std::vector<std::pair<std::string, std::string>> settings, const std::vector<std::string>& roots);
    void scan_presets(const std::string& folder);
    void delete_preset(const std::string& path, int index);
    std::string load_zones(int part, const std::string& path, int index, const std::vector<std::string>& roots,
                           int& missing);
    void send_defs(const std::string& machine);
    void send_patches(const std::string& machine);
    void send_state(int part);
    void set_param(int part, const Value& which, float v01);
    void set_channel(int part, const Value& which, float v01);
    void ensure_send(int slot);
    void apply_chain(int part, const acidulous::PatchChain& chain, const std::string& what);
    void send_inserts(int part, const acidulous::PatchInsert* const at[acidulous::kEffectSlots]);
    void set_fx(int part, int slot, const std::string& name, float v01);
    void set_send(int slot, const std::string& name, float v01);
    void set_mod(int part, int slot, const std::string& type);
    void set_mod_param(int part, int slot, const std::string& name, float v01);
    void send_mods(int part);
    void send_mod_defs();
    void clear_mods(int part);
    void mods_off(int part);
    void send_graph(int part);
    void set_scope(int part, int slot);
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
    // Decoded bytes of samples each part's slots hold, for the sample budget.
    std::mutex              held_lock;
    int64_t                 held[kParts][kHeldSlots] = {};
    bool                    cloud_quit = false;

    std::thread                       worker;
    std::mutex                        jobs_lock;
    std::condition_variable           jobs_cv, idle_cv;
    std::deque<std::function<void()>> jobs;
    bool                              busy = false;
    bool                              quitting = false;
    std::atomic<bool>                 stopping{false};

    // Worker only: whether each send's effect is mounted, and its params as
    // set, applied when it is.
    bool                         send_mounted[kSends] = {};
    std::map<std::string, float> send_values[kSends];
    // Worker only: each part's reverb and delay send levels as last set, 0-1,
    // and what its patches put on its track (the insert slots they filled and
    // the sends they hold). Both outlive a machine swap, like the track's
    // inserts and channel do.
    float                       part_sends[kParts][kSends] = {};
    acidulous::PatchTrackState  patch_fx[kParts];
    std::string                 mod_types[kParts][kModSlots]; // on the worker: what each modifier slot has
    bool                        mod_bypass[kParts][kModSlots] = {}; // on the worker
    std::atomic<bool>            metering{false};
    // The part whose scope is open, or -1, and the module slot it's cabled
    // from. Written by the worker.
    std::atomic<int> scope_part{-1}, scope_slot_live{-1};
    // No trace before this, while the remounted scope's ring still holds the
    // last module's, in steady_clock milliseconds.
    std::atomic<int64_t> scope_from{0};

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
