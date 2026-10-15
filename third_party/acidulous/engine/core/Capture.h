#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Records audio to a file without the audio thread touching the file.
//
// The callback pushes frames into a ring and a writer thread drains it to
// disk. If the ring fills, the callback drops what doesn't fit and sets a
// flag, so a gap in the recording can be reported instead of glitching the
// output.
namespace acidulous {

class Capture {
  public:
    enum Source : int32_t { FromInput = 0, FromMaster = 1 };

    ~Capture() { stop(); }

    bool start(const std::string &path, int32_t sampleRate, Source source, std::string &error);
    void stop();

    bool armed() const { return running.load(std::memory_order_acquire); }
    Source source() const { return which; }
    int64_t frames() const { return written.load(std::memory_order_relaxed); }
    /**
     * Frames accepted into the ring. This is the audio thread's count and the
     * one to stamp marks with. [frames] is the writer thread's count and can
     * lag by up to four seconds. Everything accepted ends up in the file.
     */
    int64_t pushed() const { return writeIndex.load(std::memory_order_relaxed); }
    float peak() const { return peakLevel.load(std::memory_order_relaxed); }
    bool overflowed() const { return overflow.load(std::memory_order_relaxed); }
    /**
     * True once an input capture has recorded with no input arriving (no
     * stream open, or one that went away). Separate from `overflowed` so the
     * UI can say there was nothing to record, not that the take has a gap.
     */
    bool deaf() const { return wasDeaf.load(std::memory_order_relaxed); }
    const std::string &file() const { return outPath; }

    // Audio thread. Interleaved stereo.
    void push(const float *interleaved, int32_t frames);
    /** Audio thread. As `push`, with nothing to push: see `deaf()`. */
    void pushSilence(int32_t frames);

  private:
    void drain();

    std::vector<float> ring;
    std::atomic<int64_t> writeIndex{0};
    std::atomic<int64_t> readIndex{0};
    std::atomic<bool> running{false};
    std::atomic<bool> overflow{false};
    std::atomic<bool> wasDeaf{false};
    std::atomic<int64_t> written{0};
    std::atomic<float> peakLevel{0.0f};
    std::thread worker;
    std::string outPath;
    int32_t rate = 48000;
    // Fixed when recording starts, so changing the setting mid-take doesn't
    // change the file's format.
    int32_t depth = 24;
    Source which = FromInput;
};

} // namespace acidulous
