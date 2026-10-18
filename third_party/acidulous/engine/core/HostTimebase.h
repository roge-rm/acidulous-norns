#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <engine/core/Constants.h>
#include <engine/core/Timebase.h>

// A beat handed in by whoever hosts the engine, like the norns plugin with
// its own clock. The engine follows it the same way it follows Link: the
// session's tempo, and our bar pulled gently into line with theirs.
//
// The host says which beat it was at a given engine output frame, a few times
// a second. Between calls the beat is carried forward at the host's tempo.
// No call for a second and it reads as gone, so the engine runs on at the
// last tempo.
namespace acidulous {

class HostTimebase final : public Timebase {
  public:
    /**
     * Any one thread, never the audio thread: the host was at [beat] at
     * engine output frame [frame], at [bpm]. A seqlock so the audio thread
     * reads the three together without a lock. One writing thread only.
     * False, and nothing stored, for a beat or tempo that isn't a number.
     */
    bool set(double beat, double bpm, int64_t frame) {
        if (!std::isfinite(beat) || !std::isfinite(bpm) || bpm < 1.0) return false;
        seq.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        hostBeat.store(beat, std::memory_order_relaxed);
        hostBpm.store(bpm, std::memory_order_relaxed);
        hostFrame.store(frame, std::memory_order_relaxed);
        seq.fetch_add(1, std::memory_order_release);
        return true;
    }

    /** Audio thread: the engine's sample rate, for frames to beats. */
    void setRate(int32_t rate) { sampleRate = rate > 0 ? rate : kSampleRate; }

    /**
     * Audio thread. The host's beat at [framesRendered], carried forward from
     * the last call. Not valid before the first call or once it's stale, but
     * [bpm] is still the last tempo so the engine can run on at it.
     */
    State capture(int64_t framesRendered) override {
        // A torn read (the host mid-write) keeps the copy from last block.
        const uint32_t before = seq.load(std::memory_order_acquire);
        if ((before & 1u) == 0) {
            const double b = hostBeat.load(std::memory_order_relaxed);
            const double t = hostBpm.load(std::memory_order_relaxed);
            const int64_t f = hostFrame.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == before && before != 0) {
                lastBeat = b;
                lastBpm = t;
                lastFrame = f;
                seen = true;
            }
        }
        State s;
        s.bpm = lastBpm;
        s.quantum = quantum;
        s.peers = 0;
        if (!seen) return s;
        const double beatsPerFrame = lastBpm / (60.0 * static_cast<double>(sampleRate));
        s.beat = lastBeat + static_cast<double>(framesRendered - lastFrame) * beatsPerFrame;
        s.beatsPerBlock = static_cast<double>(kBlockFrames) * beatsPerFrame;
        // A second with no call and the host is gone.
        s.valid = framesRendered - lastFrame <= static_cast<int64_t>(sampleRate);
        return s;
    }

    /** Any thread: changes with every call, to tell a new beat from an old one. */
    uint32_t stamp() const { return seq.load(std::memory_order_acquire); }

    /** Audio thread: whether a beat has arrived since the engine started. */
    bool hasBeat() const { return seen; }

    void setQuantum(double beats) override { quantum = beats > 0.0 ? beats : 4.0; }
    // The host owns its tempo and transport, so nothing is offered back.
    void proposeTempo(double) override {}
    void proposePlaying(bool) override {}

  private:
    std::atomic<uint32_t> seq{0};
    std::atomic<double> hostBeat{0.0}, hostBpm{120.0};
    std::atomic<int64_t> hostFrame{0};
    // Audio thread only.
    double lastBeat = 0.0, lastBpm = 120.0, quantum = 4.0;
    int64_t lastFrame = 0;
    int32_t sampleRate = kSampleRate;
    bool seen = false;
};

} // namespace acidulous
