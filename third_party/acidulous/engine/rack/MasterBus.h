#pragma once
#include <cstdint>
#include "Rack.h"
#include <atomic>
#include <engine/core/Params.h>
#include <engine/dsp/Click.h>
#include <engine/dsp/Limiter.h>
#include <engine/rack/Perform.h>
#include <engine/dsp/Loudness.h>
#include <engine/effect/Effect.h>

// The master section: sums the racks, feeds two send buses, then master
// fader -> limiter -> scene fade -> metronome -> meter.
namespace acidulous {

/** How many send buses the master carries. */
constexpr int32_t kSendSlots = 2;
/** How many insert slots the master has, before its fader and limiter. */
constexpr int32_t kMasterInsertSlots = 2;
/**
 * How many groups the mixer has. A group is a mixer strip, not a track.
 * Tracks route into it and it has two inserts and its own fader.
 */
constexpr int32_t kGroupSlots = 4;
/** How many inserts each group has. */
constexpr int32_t kGroupInsertSlots = 2;

class MasterBus {
  public:
    /**
     * The master's own parameters.
     *
     * The sends aren't here. Each send is a slot that can hold any effect, so
     * its parameters belong to that effect, addressed as send1 and send2, the
     * same as an insert's.
     */
    enum P : int32_t {
        Volume, LimiterOn, LimiterDrive,
        ClickOn, ClickVolume, ClickVoice, ClickDiv, ClickWhen,
        // The four groups' faders: gain, mute and solo each, added at the end.
        G1Gain, G1Mute, G1Solo, G2Gain, G2Mute, G2Solo,
        G3Gain, G3Mute, G3Solo, G4Gain, G4Mute, G4Solo,
        // And their pans, added after those.
        G1Pan, G2Pan, G3Pan, G4Pan,
        Count
    };
    /** Group [g]'s gain parameter. Mute and solo follow it. */
    static constexpr int32_t groupParam(int32_t g) { return G1Gain + g * 3; }

    MasterBus();
    void prepare(int32_t sampleRate); // not on the audio thread

    ParamSet &params() { return params_; }

    /**
     * The performance effects: on the whole mix after the master inserts and
     * before the fader, or on one group after its inserts and before its fader.
     */
    Perform perform;

    // Per block. fade is the scene fade multiplier (1 = none). A click may be
    // pending from the metronome. The tick range is passed on to the sends,
    // since some effects sync their LFOs to the transport.
    void process(Rack *racks, int32_t rackCount, float *outInterleaved, int32_t frames, float bpm, float fade,
                 int64_t tickStart = 0, int64_t tickEnd = 0);

    /**
     * Put [next] on send [slot] and return what was there, to be retired off
     * this thread. The master's version of Rack::swapEffect.
     */
    Effect *swapSend(int32_t slot, Effect *next) {
        if (slot < 0 || slot >= kSendSlots) return next; // the caller retires it
        Effect *old = sends[slot];
        sends[slot] = next;
        return old;
    }

    /** What's on send [slot], or null. Its own ParamSet holds its parameters. */
    Effect *send(int32_t slot) { return (slot >= 0 && slot < kSendSlots) ? sends[slot] : nullptr; }

    /** Put [next] on master insert [slot]. Returns what was there for the caller to retire. */
    Effect *swapInsert(int32_t slot, Effect *next) {
        if (slot < 0 || slot >= kMasterInsertSlots) return next;
        Effect *old = inserts[slot];
        inserts[slot] = next;
        return old;
    }
    Effect *insert(int32_t slot) { return (slot >= 0 && slot < kMasterInsertSlots) ? inserts[slot] : nullptr; }

    /** Put [next] on group [g]'s insert [slot]. Returns what was there for the caller to retire. */
    Effect *swapGroupInsert(int32_t g, int32_t slot, Effect *next) {
        if (g < 0 || g >= kGroupSlots || slot < 0 || slot >= kGroupInsertSlots) return next;
        Effect *old = groupInserts[g][slot];
        groupInserts[g][slot] = next;
        return old;
    }
    Effect *groupInsert(int32_t g, int32_t slot) {
        return (g >= 0 && g < kGroupSlots && slot >= 0 && slot < kGroupInsertSlots) ? groupInserts[g][slot] : nullptr;
    }
    /** Group [g]'s output this block, after its inserts and fader. This is its stem. */
    const float *groupOutL(int32_t g) const { return groupL[g]; }
    const float *groupOutR(int32_t g) const { return groupR[g]; }
    float readGroupPeak(int32_t g) {
        return (g >= 0 && g < kGroupSlots) ? groupPeakHold[g].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
    }

    /** [accent] is dsp::Click::Bar, Beat or Division. */
    void clickAt(int32_t accent, int32_t offsetSamples) { click.trigger(accent, offsetSamples); }
    void setClickVoice(int32_t voice) { click.setVoice(voice); }
    bool clickEnabled() const { return params_.get(ClickOn) >= 0.5f; }
    /**
     * A count-in is running, so the click sounds whatever the metronome says.
     *
     * The engine queues count-in clicks even with the metronome off, so they
     * have to be rendered too. Otherwise you get the wait with no count, and
     * the queued clicks (Click::process is what empties the queue) would go
     * off later at the wrong moment.
     */
    void setCountingIn(bool on) { countingIn = on; }
    bool clickAudible() const { return clickEnabled() || countingIn; }

    /**
     * Whether the click should sound while the transport runs. "Recording
     * only" is what most people use.
     */
    bool clickAllowed(bool recordArmed) const {
        const int32_t when = static_cast<int32_t>(params_.normalized(ClickWhen) * 2.0f + 0.5f);
        if (when == 1) return recordArmed;
        if (when == 2) return false; // the count-in ignores this
        return true;
    }

    /**
     * How often it clicks, in ticks: a bar or a division of the beat.
     *
     * Reads the target rather than the smoothed value, like every stepped
     * control, or it would briefly tick the values in between when changed.
     */
    int64_t clickStepTicks() const {
        const int32_t index = static_cast<int32_t>(params_.normalized(ClickDiv) * 4.0f + 0.5f);
        switch (index) {
        case 0: return 0;             // the bar, whatever the signature says it is
        case 2: return kPPQN / 2;     // eighths
        case 3: return kPPQN / 4;     // sixteenths
        case 4: return kPPQN / 3;     // eighth triplets
        default: return kPPQN;        // the beat
        }
    }

    float readPeak() { return peakHold.exchange(0.0f, std::memory_order_relaxed); }

    /**
     * Loudness of the master output: momentary, short-term and integrated
     * LUFS, and true peak in dBTP.
     *
     * Only measured while something is reading it. The meter costs a few
     * microseconds a block (mostly the 4x oversampling for true peak), so each
     * read keeps it running for another second, counted down on the audio
     * thread. The integrated figure covers the time it was watched since the
     * last reset.
     */
    void readLoudness(float *out4) {
        loudnessWatch.store(kWatchBlocks, std::memory_order_relaxed);
        out4[0] = lufsM.load(std::memory_order_relaxed);
        out4[1] = lufsS.load(std::memory_order_relaxed);
        out4[2] = lufsI.load(std::memory_order_relaxed);
        out4[3] = truePeakDb.load(std::memory_order_relaxed);
    }
    /** Restart the integrated figure, at play or when asked. */
    void resetLoudness() { loudnessResetWanted.store(true, std::memory_order_relaxed); }
    float currentFade() const { return fadeNow.load(std::memory_order_relaxed); }

    /** Clear every tail and come back from silence. Audio thread. */
    void panic();

  private:
    ParamSet params_;
    /** Where the performance effects ran last block: a group, or -1 for the whole mix. */
    int32_t performWas = -1;
    Effect *sends[kSendSlots]{};
    Effect *inserts[kMasterInsertSlots]{};
    Effect *groupInserts[kGroupSlots][kGroupInsertSlots]{};
    float groupL[kGroupSlots][kBlockFrames]{};
    float groupR[kGroupSlots][kBlockFrames]{};
    std::atomic<float> groupPeakHold[kGroupSlots]{};
    dsp::Loudness loudness;
    static constexpr int32_t kWatchBlocks = 750; // a second at 64 frames
    std::atomic<int32_t> loudnessWatch{0};
    std::atomic<bool> loudnessResetWanted{false};
    std::atomic<float> lufsM{dsp::Loudness::kSilent}, lufsS{dsp::Loudness::kSilent},
        lufsI{dsp::Loudness::kSilent}, truePeakDb{dsp::Loudness::kSilent};
    dsp::Limiter<kBlockFrames> limiter;
    float sampleRate = 48000.0f;
    float panicRamp = 1.0f;
    dsp::Click click;
    bool countingIn = false;
    Smoothed fadeSmooth;
    bool fadeJump = false; // see panic()
    float sumL[kBlockFrames]{}, sumR[kBlockFrames]{};
    /** What each send is fed, summed mono across the racks. */
    float sendSum[kSendSlots][kBlockFrames]{};
    /**
     * The stereo pair a send is processed in. A send bus is a mono sum, so it's
     * copied into this pair, the effect runs on it in place, and the result is
     * added to the mix. That also lets a send effect have its own stereo image.
     */
    float wetL[kBlockFrames]{}, wetR[kBlockFrames]{};
    std::atomic<float> peakHold{0.0f};
    std::atomic<float> fadeNow{1.0f};
};

} // namespace acidulous
