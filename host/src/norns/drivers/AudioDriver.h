#pragma once
#include <atomic>
#include <cstdint>
#include <engine/core/Constants.h>
#include <functional>

// The audio "device" for norns: it opens nothing. scsynth's audio thread is
// the stream, and the Acidulous UGen pulls each engine block through pull().
//
// Same class and public API as the app's Oboe, desktop and web drivers, so
// EngineHost builds unchanged. Like the app's no-device driver for replaying
// engine lines (platform/lines/drivers), but pulled by the UGen instead of
// rendering on a thread of its own. EngineHost registers its render callback and
// start() attaches it; stop() detaches it and waits for a pull in progress,
// so an offline render or freeze can then call the engine by hand.
//
// There's no input stream (the UGen's inputs come in through pull), no
// scheduler hint and no presentation time.

class AudioDriver {
  public:
    AudioDriver() = default;
    ~AudioDriver() { stop(); }

    AudioDriver(const AudioDriver &) = delete;
    AudioDriver &operator=(const AudioDriver &) = delete;

    void registerCallback(std::function<void(float *, float *, unsigned long)> cb) { callback = std::move(cb); }

    bool start();
    void stop();

    /**
     * Audio thread. Renders one engine block: [in] and [out] are kBlockFrames
     * of interleaved stereo, [in] may be null. Returns false and leaves [out]
     * alone when no started driver is attached.
     */
    static bool pull(float *in, float *out);
    /** Blocks pulled so far, so a host can wait for the queues to drain. */
    static int64_t blocks();

    bool startInput(int32_t = 0) { return false; }
    void stopInput() {}
    void setInputClean(bool) {}
    int32_t inputSession() const { return 0; }
    bool isInputRunning() const { return false; }
    int32_t inputChannels() const { return 0; }
    int32_t inputRate() const { return 0; }
    int32_t inputDevice() const { return 0; }
    float readInputPeak() { return 0.0f; }

    bool isRunning() const { return attached.load(); }

    void setBufferBursts(int32_t) {}
    int32_t getBufferBursts() const { return 1; }
    int32_t getBufferFrames() const { return acidulous::kBlockFrames; }
    void setBufferFloor(int32_t) {}
    int32_t bufferFloor() const { return 0; }

    int32_t getSampleRate() const { return acidulous::kSampleRate; }
    int32_t getFramesPerBurst() const { return acidulous::kBlockFrames; }
    bool isLowLatency() const { return true; }
    int64_t getXRunCount() const { return 0; }
    bool hintRunning() const { return false; }
    bool hintAvailable() const { return false; }
    int32_t hintState() const { return 0; }
    int32_t fastCores() const { return 0; }
    void setHintWanted(bool) {}
    static int32_t workerCores() { return 0; }
    static void prepareWorker(int32_t) {}

    int32_t readCallbackPeakUs() { return 0; }
    int32_t recentCallbackUs() const { return 0; }
    int32_t readCallbackCpuPeakUs() { return 0; }
    int64_t getStalledCallbacks() const { return 0; }
    int64_t getLateCallbacks() const { return 0; }
    int32_t callbackBudgetUs() const {
        return static_cast<int32_t>(int64_t(acidulous::kBlockFrames) * 1000000 / acidulous::kSampleRate);
    }

    bool presentationAnchor(int64_t &, int64_t &) const { return false; }
    float readPeakLevel() { return 0.0f; }

  private:
    std::function<void(float *, float *, unsigned long)> callback;
    std::atomic<bool> attached{false};
};
