#pragma once
#include "RtQueue.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// How objects cross into and out of the audio thread.
//
// Anything with a lifetime (a machine, a song snapshot) is built on a normal
// thread and mounted by pushing a Mount record. The audio thread applies one
// per block, at the block boundary. Whatever it replaces goes back out as a
// Retire record and a worker thread runs the deleter, so the audio thread
// never calls new or delete.
namespace acidulous {

struct Mount {
    // EffectObject: an object for an effect's swapObject. The slot carries the
    // effect's Unit (Effect1, Send2, MasterFx1, Group3Fx2, Input1...) and the
    // rack is the track's for a track insert.
    enum class Kind : uint8_t { None, Machine, Song, Effect, InputMod, Object, Frozen, Send, Input, MasterInsert, GroupInsert, EffectObject };
    Kind kind = Kind::None;
    int32_t rack = 0;
    int32_t slot = 0;
    void *object = nullptr;
    // Object mounts: how to retire whatever the machine hands back (or this, if refused).
    void (*deleter)(void *) = nullptr;
};

struct Retire {
    void *object = nullptr;
    void (*deleter)(void *) = nullptr;
};

class Retirer {
  public:
    /**
     * Held by a thread other than the audio one while it reads something the
     * audio thread can swap out: a machine's knobs, a pad's sample, Nexus's
     * scope. Nothing retired is deleted while one is held, so the read can't
     * land on freed memory. Keep it short: deleting waits for it.
     */
    class ReadGuard {
      public:
        explicit ReadGuard(Retirer &r) : owner(r) { owner.readers.fetch_add(1); }
        ~ReadGuard() { owner.readers.fetch_sub(1); }
        ReadGuard(const ReadGuard &) = delete;
        ReadGuard &operator=(const ReadGuard &) = delete;

      private:
        Retirer &owner;
    };

    ~Retirer() { stop(); }

    void start() {
        if (running.exchange(true)) return;
        worker = std::thread([this] { run(); });
    }

    void stop() {
        if (!running.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lock(mtx);
            wake = true;
        }
        cv.notify_one();
        if (worker.joinable()) worker.join();
        drain();
    }

    // Audio thread. Never blocks. If the queue is full the object leaks instead.
    void retire(void *object, void (*deleter)(void *)) {
        if (object == nullptr) return;
        Retire r;
        r.object = object;
        r.deleter = deleter;
        queue.push(r);
        // Just a flag, no notify. The worker also polls, so at worst deletion
        // waits one poll interval.
        pendingSignal.store(true, std::memory_order_release);
    }

  private:
    void run() {
        while (running.load(std::memory_order_relaxed)) {
            drain();
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return wake || pendingSignal.load(std::memory_order_acquire); });
            wake = false;
            pendingSignal.store(false, std::memory_order_relaxed);
        }
    }

    void drain() {
        Retire r;
        while (queue.pop(r)) {
            // A reader that began before the swap may still hold the old
            // object. One that begins after this check sees the new one: the
            // swap came before the retire it was popped from. Both sides are
            // sequentially consistent (see ReadGuard).
            while (readers.load() > 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            if (r.deleter) r.deleter(r.object);
        }
    }

    RtQueue<Retire, 256> queue;
    std::atomic<bool> running{false};
    std::atomic<bool> pendingSignal{false};
    std::atomic<int32_t> readers{0};
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    bool wake = false;
};

template <typename T>
void deleteAs(void *p) { delete static_cast<T *>(p); }

} // namespace acidulous
