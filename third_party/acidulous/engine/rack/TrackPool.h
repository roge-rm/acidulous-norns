#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace acidulous {

/**
 * Worker threads that render tracks alongside the audio thread.
 *
 * Each block the audio thread hands over up to 32 jobs, each with the jobs
 * that must be done before it can start (a sidechain listener waits for its
 * source). The audio thread works too: it takes jobs like any worker and
 * never waits for a job nobody has started, so if the workers are slow to
 * wake it renders everything itself, as with no pool at all. It only waits
 * for a job a worker is part way through.
 *
 * No locks and no allocation while running. Workers spin for a short while
 * after a block, then sleep on a futex until the next one. A round is tagged
 * with its own number, so a worker that wakes late can't take a job from the
 * next block.
 *
 * start() and stop() aren't real-time and mustn't run while run() does.
 */
class TrackPool {
  public:
    using JobFn = void (*)(void *context, int32_t job);
    static constexpr int32_t kMaxJobs = 32;
    static constexpr int32_t kMaxWorkers = 7;

    TrackPool() = default;
    ~TrackPool() { stop(); }
    TrackPool(const TrackPool &) = delete;
    TrackPool &operator=(const TrackPool &) = delete;

    /** Starts [workers] threads (0 for none, at most kMaxWorkers); [onStart] runs first on each, e.g. to pin it. */
    void start(int32_t workers, void (*onStart)(int32_t worker) = nullptr);
    void stop();
    /** Threads started. */
    int32_t workers() const { return workerCount; }
    /** How many of them take jobs; the rest sleep. Any thread, any time. */
    void setActive(int32_t n);
    int32_t activeWorkers() const { return static_cast<int32_t>(active.load(std::memory_order_relaxed)); }
    /** Microseconds the last run() spent waiting for a job a worker had, on the audio thread. */
    int32_t lastWaitUs() const { return waitedUs; }

    /** Physical cores (not hardware threads), or 0 where it can't tell. */
    static int32_t physicalCores();

    /**
     * Audio thread: runs jobs 0..count-1 and returns when all are done. Job j
     * starts only once every job in needs[j] (a bit per job) is done. Jobs are
     * tried in [pick] order, so put the heaviest first. Without [useWorkers], or
     * with no workers, it runs them all on this thread and the workers stay
     * asleep: for a block light enough that waking them costs more than it
     * saves.
     */
    void run(int32_t count, const uint32_t *needs, const int32_t *pick, JobFn fn, void *context, bool useWorkers = true);
    /**
     * Audio thread: wakes the sleeping workers now, ahead of a run() soon, so
     * they're up and spinning by the time it starts rather than waking then.
     */
    void wakeEarly();

  private:
    void workerLoop(int32_t index, void (*onStart)(int32_t));
    /** Takes and runs jobs of round [round] until none is left to take. */
    void work(uint32_t round);
    /** Claims a job that's ready, or -1. */
    int32_t claim(uint32_t round);


    std::thread threads[kMaxWorkers];
    int32_t workerCount = 0;
    // A worker at or past this sleeps on it until it changes.
    std::atomic<uint32_t> active{0};
    std::atomic<bool> quit{false};
    int32_t waitedUs = 0;

    // The round's work, written by run() before it bumps the round. Atomic
    // (relaxed) because a worker still finishing an old round may read them
    // as they change; its claim then fails on the round number.
    std::atomic<int32_t> jobCount{0};
    std::atomic<uint32_t> allJobs{0};
    std::atomic<uint32_t> jobNeeds[kMaxJobs]{};
    std::atomic<int32_t> jobPick[kMaxJobs]{};
    std::atomic<JobFn> jobFn{nullptr};
    std::atomic<void *> jobContext{nullptr};

    // The round number, and its taken and done bits, each packed as
    // (round << 32) | bits so a stale worker's compare-and-swap fails.
    alignas(64) std::atomic<uint32_t> round{0};
    alignas(64) std::atomic<uint64_t> taken{0};
    alignas(64) std::atomic<uint64_t> done{0};
    alignas(64) std::atomic<int32_t> sleepers{0};
    // Counts jobs finished, for the audio thread to sleep on while it waits
    // for one; [waiter] says it's asleep there.
    alignas(64) std::atomic<uint32_t> finished{0};
    std::atomic<uint32_t> waiter{0};
};

} // namespace acidulous
