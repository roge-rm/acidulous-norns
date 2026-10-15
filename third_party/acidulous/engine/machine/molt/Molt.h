#pragma once
#include <cmath>
#include <cstdint>
#include <engine/core/Utterance.h>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <vector>

// Molt turns a recorded vocal take into an instrument.
//
// It moves pitch and formant independently using pitch-synchronous
// overlap-add (PSOLA). Laying the take's glottal pulses down at a new spacing
// moves the pitch and keeps the formants. Resampling each pulse before laying
// it moves the formants and keeps the pitch.
//
// The notes in the clip are the target pitches, so drawing a chord makes the
// take sing that chord. There's one read head shared by four voices, so every
// note sings the same word at the same moment. Robot, hard tune and megaphone
// are also available.
namespace acidulous::machine {

class Molt final : public Machine {
  public:
    static constexpr int kVoices = 4;
    /**
     * The overlap-add buffer, a power of two so the ring wraps by mask. Two
     * periods at the lowest tracked pitch (70 Hz) is 1371 frames and a
     * formant an octave down doubles that, so 4096 leaves room.
     */
    static constexpr int kAccum = 4096;

    enum P : int32_t {
        Start = 0, Loop,
        Tune, Rate, Robot,
        Formant, Mega,
        Cutoff, Resonance, FilterType,
        AmpAttack, AmpDecay, AmpSustain, AmpRelease,
        Glide, BendRange, Octave, Transpose, VelocityAmount,
        Drive, Volume, Pan,
        Count
    };

    Molt();

    const char *typeName() const override { return "Molt"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;
    void *swapObject(int32_t slot, void *object) override;

    /** Where the read head is, 0..1 through the take, for the panel. */
    float headPosition() const {
        const audio::Utterance *u = source;
        if (u == nullptr || u->frames <= 1) return 0.0f;
        return static_cast<float>(head) / static_cast<float>(u->frames);
    }
    bool takeLoaded() const { return source != nullptr && source->usable(); }

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0;
        float velocity = 1.0f;
        float bend = 0.0f, pressure = 0.0f;
        /** The pitch it is being pulled to, in log2 Hz, glided by `rate`. */
        float logPitch = 0.0f;
        bool primed = false;
        /** Frames until the next grain is laid down. */
        float untilGrain = 0.0f;
        dsp::Adsr amp;
        int32_t accHead = 0;
        std::vector<float> acc;
    };

    /**
     * Lay one grain for [v] at the read head and return the spacing until the
     * next. Picks which pulse to copy, how fast to read it (formant) and how
     * far apart to lay them (pitch).
     */
    float layGrain(Voice &v, float formantRatio, float tune, float rateSec, bool robot);

    float paramOf(int32_t i) const { return params_.get(i); }
    /**
     * The target rather than the smoothed value, for switches, since
     * smoothing would turn an edge into a ramp.
     */
    float rawOf(int32_t i) const { return params_.normalized(i); }
    // Rounded, not truncated: octave and transpose go below zero, where
    // adding a half and truncating made -1 into 0.
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(std::lround(paramOf(p))); }

    float sampleRate = 48000.0f;
    const audio::Utterance *source = nullptr;

    // The read head is shared by all voices.
    double head = 0.0;
    bool running = false;

    Voice voices[kVoices];
    /** One filter for the whole mono mix, before panning. */
    dsp::MultiFilter filter;
    /** The megaphone's band, with its own clipping drive. */
    dsp::MultiFilter horn;

    float channelBend = 0.0f;
};

} // namespace acidulous::machine
