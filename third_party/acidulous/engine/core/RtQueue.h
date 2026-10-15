#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>

// Single-producer, single-consumer ring. Wait-free on both sides. This is the
// only cross-thread structure the audio thread touches, in either direction.
// SharedQueue, below, is the same ring for more than one producer.
namespace acidulous {

template <typename T, size_t N>
class RtQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

  public:
    bool push(const T &item) {
        const size_t w = wr.load(std::memory_order_relaxed);
        const size_t next = (w + 1) & (N - 1);
        if (next == rd.load(std::memory_order_acquire)) {
            return false;
        }
        buf[w] = item;
        wr.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T &out) {
        const size_t r = rd.load(std::memory_order_relaxed);
        if (r == wr.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf[r];
        rd.store((r + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    bool empty() const { return rd.load(std::memory_order_acquire) == wr.load(std::memory_order_acquire); }

  private:
    T buf[N];
    std::atomic<size_t> wr{0};
    std::atomic<size_t> rd{0};
};

/**
 * An RtQueue that many threads can push into. The engine's input queues are
 * fed from the UI thread, each MIDI port's own thread and any worker loading
 * a sample, and two pushes at once could write the same slot and lose one.
 *
 * Pushers take turns on a lock. The consumer, the audio thread, never takes
 * it and pops as before.
 */
template <typename T, size_t N>
class SharedQueue {
  public:
    bool push(const T &item) {
        std::lock_guard<std::mutex> turn(pushing);
        return ring.push(item);
    }
    bool pop(T &out) { return ring.pop(out); }
    bool empty() const { return ring.empty(); }

  private:
    RtQueue<T, N> ring;
    std::mutex pushing;
};

} // namespace acidulous
