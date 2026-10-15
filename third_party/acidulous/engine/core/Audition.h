#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// Plays a file once so you can hear it from the sample browser.
//
// This sits outside the song: it's one buffer the master mixes in, and it
// isn't recorded, exported or frozen.
//
// Two buffers and an index instead of a queue. The audio thread reads `which`
// once and only touches that buffer for the whole block. The UI thread only
// writes the other one and then publishes it. Nothing is freed on the audio
// thread.
//
// A block that read `which` just before a publish is still reading the old
// buffer, which is the next one to be written. So a writer waits for `busy`
// to clear first. Dekker-style, as in the drivers: mix sets busy before it
// reads which, a writer publishes which before it reads busy, all
// sequentially consistent. A preview replaces the buffer on every knob turn,
// so this matters.
namespace acidulous {

class Audition {
  public:
    /** UI thread. Interleaved stereo at the engine rate. Starts it playing. */
    void play(const float *interleaved, int64_t frames) {
        publish(interleaved, frames);
        position.store(0, std::memory_order_relaxed);
        playing.store(true, std::memory_order_release);
    }

    /**
     * UI thread. Swaps in new audio where the old is playing, without
     * starting over or starting it if it has stopped. A preview does this as
     * a knob turns.
     */
    void replace(const float *interleaved, int64_t frames) { publish(interleaved, frames); }

    /** Either thread. */
    void stop() { playing.store(false, std::memory_order_release); }
    bool active() const { return playing.load(std::memory_order_relaxed); }
    /** How far through, 0..1, for a playhead; -1 when nothing is playing. Any thread. */
    float progress() const {
        const int64_t total = length.load(std::memory_order_relaxed);
        if (!active() || total <= 0) return -1.0f;
        return static_cast<float>(static_cast<double>(position.load(std::memory_order_relaxed)) / static_cast<double>(total));
    }

    /** Audio thread. Adds what is left into [out], and stops at the end. */
    void mix(float *out, int32_t frames) {
        if (!playing.load(std::memory_order_acquire)) return;
        busy.store(true);
        const int slot = which.load();
        if (slot < 0) { busy.store(false); return; }
        const std::vector<float> &buffer = pcm[static_cast<size_t>(slot)];
        const auto total = static_cast<int64_t>(buffer.size() / 2);
        int64_t at = position.load(std::memory_order_relaxed);
        for (int32_t i = 0; i < frames && at < total; ++i, ++at) {
            out[i * 2] += buffer[static_cast<size_t>(at) * 2];
            out[i * 2 + 1] += buffer[static_cast<size_t>(at) * 2 + 1];
        }
        position.store(at, std::memory_order_relaxed);
        if (at >= total) playing.store(false, std::memory_order_release);
        busy.store(false);
    }

  private:
    /** Fills the buffer not being played and makes it the one that is. */
    void publish(const float *interleaved, int64_t frames) {
        // `which` is -1 before anything has played, so use buffer 0 then.
        // `1 - which` would give 2, which is out of bounds.
        const int now = which.load();
        const int spare = now < 0 ? 0 : 1 - now;
        while (busy.load()) std::this_thread::yield(); // a block may still be reading the spare
        auto &buffer = pcm[static_cast<size_t>(spare)];
        buffer.assign(interleaved, interleaved + static_cast<size_t>(frames) * 2);
        length.store(frames, std::memory_order_relaxed);
        if (position.load(std::memory_order_relaxed) > frames) position.store(frames, std::memory_order_relaxed);
        which.store(spare);
    }

    std::vector<float> pcm[2];
    std::atomic<int> which{-1};
    std::atomic<int64_t> position{0};
    std::atomic<int64_t> length{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> busy{false};
};

} // namespace acidulous
