#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <engine/core/Take.h>
#include <vector>

// Pollen is the granular synth. Each note plays a grain cloud over a buffer,
// which is either a sample or a live recording of the phone's input. 96
// grains across eight voices, stereo, four window shapes, jittered births
// and pan per grain.
//
//   - Pollination: a dying grain can seed a child near it with mutated
//     position, pitch, size and pan, down to a set number of generations.
//     Each generation is quieter so the cloud settles.
//   - Harmonic scatter: the random per-grain pitch can be quantised to
//     octaves, fifths, a triad or any of the 33 scales.
//   - Onset snap: grains are pulled towards transients in the buffer (found
//     on a worker for a file, or while recording for the live ring).
//   - Feedback: the output goes back into the live ring.
namespace acidulous::machine {

class Pollen final : public Machine {
  public:
    static constexpr int kVoices = 8;
    static constexpr int kGrains = 96;
    /** Extra slots that only hold stolen grains while they fade out. */
    static constexpr int kSpare = 8;
    static constexpr int kWindowSize = 1024;
    static constexpr int kLiveSeconds = 8;
    static constexpr int kLiveOnsets = 64;

    enum SourceKind : int32_t { FromSample, FromLive, SourceCount };
    enum Scatter : int32_t { Free, Octaves, Fifths, Triad, InScale, ScatterCount };
    enum Window : int32_t { Hann, Tukey, Percussive, Reverse, WindowCount };

    enum P : int32_t {
        SourceMode = 0, InGain, BufferSeconds, Freeze, Capture, Feedback,
        Position, Scan, Spray, Snap, ReverseProb,
        Size, SizeSpread, Density, Jitter, WindowShape, Skew,
        Pitch, Fine, KeyTrack, Spread, ScatterMode, ScaleIndex, Key,
        Bloom, Drift, Mutate, Generations,
        PanSpread, Width,
        Bits, Crush, Wobble, WobbleRate,
        Cutoff, Resonance, FilterType,
        AmpAttack, AmpDecay, AmpSustain, AmpRelease,
        Mono, Glide, BendRange, Octave, Transpose, VelocityAmount,
        Drive, Dry, Volume, Pan,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate, and
        // a grain takes its values when it's born. Pitch is 24 semitones per
        // unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. The source, the live switches, the stepped
    // choices, fine, key track, the tuning knobs and pan are left out.
    enum MacroDest : int32_t {
        McOff, McPosition, McScan, McSpray, McSnap, McReverse,
        McSize, McSizeSpread, McDensity, McJitter, McSkew,
        McPitch, McSpread, McBloom, McDrift, McMutate,
        McPanSpread, McWidth, McBits, McCrush, McWobble,
        McCutoff, McResonance, McAttack, McRelease,
        McDrive, McFeedback, McDry, McVelocity, McVolume,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "too many parameters");

    Pollen();

    const char *typeName() const override { return "Pollen"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    bool render(float *L, float *R, int32_t frames) override;
    void *swapObject(int32_t slot, void *object) override;
    int64_t heldBytes() const override;
    // Frees what was mounted and not swapped back out (on the retire worker).
    ~Pollen() override;

    // Grain counts for the test harness and the panel's readout.
    int64_t grainsBorn() const { return births; }
    int32_t grainsAlive() const {
        int32_t n = 0;
        for (const auto &g : grains) if (g.active) ++n;
        return n;
    }
    int32_t onsetsFound() const { return liveOnsetCount; }

  private:
    struct Grain {
        bool active = false;
        int32_t voice = -1;
        double pos = 0.0, inc = 1.0;
        int32_t age = 0, length = 1;
        float gainL = 0.5f, gainR = 0.5f;
        /** Where the window's middle falls in the grain, 0.5 unskewed, fixed at birth. */
        float middle = 0.5f;
        int32_t generation = 0;
        /** Frames left of a stolen grain's fade out, 0 for a grain playing normally. */
        int32_t fade = 0;
    };
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 60;
        float velocity = 1.0f;
        float freq = 261.63f, glideFrom = 261.63f, glidePos = 1.0f;
        double playhead = 0.0;   // where this voice is reading, in frames
        double scanOffset = 0.0; // how far the scan has carried it from the base
        float timer = 0.0f;    // frames until the next grain
        /** This voice's current envelope value, read by its grains. Grains
         *  follow it live so a long grain can't outlast the release. */
        float envNow = 0.0f;
        int32_t living = 0;    // grains currently belonging to this voice
        dsp::Adsr amp;
        int64_t age = 0;
        // Per-note bend (MPE) in semitones, added to the channel bend.
        float bend = 0.0f;
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const;
    /** Fills [mac_] from the smoothed macro knobs and their routes. */
    void updateMacros();
    /** The target rather than the smoothed value, for momentary and switch controls. */
    float rawOf(int32_t p) const;
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }

    Voice *allocate();
    /** A free slot, or -1 when the pool is full. [force] steals one instead. */
    int32_t takeGrain(bool force);
    void spawn(Voice &v, int32_t voiceIndex, const audio::View &view, float env);
    void pollinate(const Grain &parent, const audio::View &view);
    float windowAt(int32_t shape, float phase, float middle) const;
    float scatterSemis(float amount, uint32_t &state) const;
    audio::View resolveView(int32_t mode, int32_t liveLen) const;

    float nextRandom() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    }

    float sampleRate = 48000.0f;
    const audio::Take *source = nullptr;

    // The live ring, owned here and written on the audio thread.
    std::vector<float> ringL, ringR;
    int32_t ringCapacity = 0, writePos = 0, liveLength = 0;
    int32_t captureLeft = 0;
    bool lastCapture = false;
    audio::OnsetFinder finder;
    int32_t liveOnsets[kLiveOnsets] = {};
    int32_t liveOnsetCount = 0, liveOnsetNext = 0;

    std::vector<float> window[WindowCount];
    /** kGrains to play, then kSpare holding stolen grains as they fade. */
    Grain grains[kGrains + kSpare];
    /** A stolen grain's fade out, in frames (2.5 ms). */
    int32_t stealFrames = 120;
    /** How many grains the cloud may use this block: all, or half in lean mode. */
    int32_t grainPool = kGrains;
    Voice voices[kVoices];
    int32_t grainCursor = 0;
    int64_t births = 0;
    int64_t ageCounter = 0;
    float bend = 0.0f, modWheel = 0.0f;
    float wobblePhase = 0.0f;
    float feedbackL = 0.0f, feedbackR = 0.0f;
    dsp::MultiFilter filterL, filterR;
    float crushAcc = 0.0f, heldL = 0.0f, heldR = 0.0f;
    // DC blocker state for the feedback path, so DC doesn't build up in the
    // ring.
    float dcInL = 0.0f, dcOutL = 0.0f, dcInR = 0.0f, dcOutR = 0.0f;
    static constexpr uint32_t kRngSeed = 0x51ed270bu;
    uint32_t rng = kRngSeed;
    float mac_[MacroDestCount]{};
};

} // namespace acidulous::machine
