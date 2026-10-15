#pragma once
#include <cstdint>
#include <engine/core/Constants.h>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/Filter.h>
#include <engine/dsp/Lfo.h>
#include <engine/dsp/Math.h>
#include <engine/dsp/Swell.h>
#include <engine/effect/Effect.h>

// The insert effects. Each is the classic effect plus an extra or two.
namespace acidulous::effect {

#define ACIDULOUS_EFFECT_COMMON(Name)                                         \
    const char *typeName() const override { return #Name; }                  \
    const ParamDef *paramDefs(int32_t &count) const override;                 \
    void prepare(int32_t sampleRate) override;                                \
    void reset() override;                                                    \
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

class Delay final : public Effect {
  public:
    enum P { Time, Feedback, Tone, PingPong, Mix, Duck, Wobble, Gain, Count };
    Delay() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Delay)
    void onBlock(int64_t, int64_t, float bpm) override { this->bpm = bpm; }
  private:
    dsp::DelayLine line[2];
    float readSamples = 24000.0f, lp[2]{}, duckEnv = 0.0f, wobblePhase = 0.0f, sr = 48000.0f, bpm = 120.0f;
    bool snapRead = true; // see reset()
};

/**
 * Reverb.
 *
 * Based on Freeverb: eight combs and four allpasses a side, with `size`,
 * `damp` and `predelay`. The extras:
 *
 *   - Shimmer: the tail is fed back an octave up, so each pass climbs into a
 *     rising chord. The pitch shift is a delay line read at double speed with
 *     two crossfaded taps, and it sits inside the feedback so it builds up.
 *   - Bits and crush: only the tail goes through a bit quantiser and a
 *     sample-and-hold, so the dry stays clean and the room sounds like an old
 *     sampler.
 *   - Wobble: the comb lengths drift slowly and out of step, so a long tail
 *     never sits still, like tape.
 *
 * Freeze holds the tail forever and gate cuts it off.
 */
class Reverb final : public Effect {
  public:
    enum P { Size, Damp, Tone, PreDelay, Mix, Freeze, Gate, Shimmer, Bits, Crush, Wobble, Gain, Count };
    Reverb() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Reverb)
  private:
    struct Comb { dsp::DelayLine line; float store = 0.0f; int32_t len = 1; };
    struct Allpass { dsp::DelayLine line; int32_t len = 1; };
    /** Eight combs and four allpasses a side at full quality, half at lean. */
    static constexpr int32_t kCombs = 8;
    static constexpr int32_t kAps = 4;
    Comb combs[2][kCombs];
    Allpass aps[2][kAps];
    dsp::DelayLine pre[2];
    /** The shimmer's line: the tail, read back an octave up. */
    dsp::DelayLine shimmerLine[2];
    float shimmerPhase[2]{}, shimmerFb[2]{};
    /** The shimmer path's DC blocker. See the note where it's used. */
    float shimDcX[2]{}, shimDcY[2]{}, shimLp[2]{};
    float crushAcc[2]{}, crushHeld[2]{};
    float wobblePhase = 0.0f;
    float lp[2]{}, gateEnv = 0.0f, inputEnv = 0.0f, sr = 48000.0f;
    int32_t gateHold = 0;
};

class Eq final : public Effect {
  public:
    enum P { LowGain, LowFreq, MidGain, MidFreq, MidQ, HighGain, HighFreq, Tilt, Gain, Count };
    Eq() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Eq)
  private:
    dsp::Biquad low[2], mid[2], high[2], tiltLo[2], tiltHi[2];
    float sr = 48000.0f;
};

class Distortion final : public Effect {
  public:
    enum P { Drive, Tone, Mix, Mode, Bias, Gain, Count };
    Distortion() { initParams(-10.5f); }
    ACIDULOUS_EFFECT_COMMON(Distortion)
  private:
    dsp::Biquad tone[2];
    // The downsampling filter runs at twice the engine rate, so it can't
    // reuse the tone filter.
    dsp::Biquad halfband[2];
    float dcIn[2]{}, dcOut[2]{}, prevIn[2]{}, sr = 48000.0f;
};

class Compressor final : public Effect {
  public:
    enum P { Threshold, Ratio, Attack, Release, Makeup, Pump, PumpRate, Gain, Sidechain, Count };
    Compressor() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Compressor)
    void onBlock(int64_t tickStart, int64_t, float bpm) override { tick = tickStart; this->bpm = bpm; }
  private:
    float env = 0.0f, gain = 1.0f, sr = 48000.0f, bpm = 120.0f;
    int64_t tick = 0;
    /**
     * The audio runs this many frames behind the detector (1 ms), so the gain
     * is already coming down when a hit arrives. Without it a fast attack
     * still lets the front of each hit through at full make-up. Only on
     * its own input, not with a sidechain.
     */
    static constexpr int32_t kAheadMax = 256;
    int32_t ahead = 48, at = 0;
    float aheadL[kAheadMax]{}, aheadR[kAheadMax]{};
};

class Filter final : public Effect {
  public:
    enum P { Cutoff, Reso, Mode, LfoRate, LfoDepth, EnvDepth, Gain, Sidechain, Count };
    Filter() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Filter)
    void onBlock(int64_t tickStart, int64_t, float bpm) override { tick = tickStart; this->bpm = bpm; }
  private:
    dsp::Svf svf[2];
    float follower = 0.0f, sr = 48000.0f, bpm = 120.0f;
    float fcSet = -1.0f, resoSet = -1.0f; // what the filters were last solved for
    int64_t tick = 0;
};

class Bitcrusher final : public Effect {
  public:
    enum P { Bits, Rate, Jitter, Tone, Mix, Gain, Count };
    Bitcrusher() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Bitcrusher)
  private:
    float hold[2]{}, phase = 0.0f, period = 1.0f, sr = 48000.0f;
    uint32_t rng = 0x2545F491u;
    dsp::Biquad tone[2];
};

class Phaser final : public Effect {
  public:
    enum P { Rate, Depth, Feedback, Stages, Spread, Mix, Gain, Count };
    Phaser() { initParams(2.5f); }
    ACIDULOUS_EFFECT_COMMON(Phaser)
    void onBlock(int64_t tickStart, int64_t, float) override { tick = tickStart; }
  private:
    dsp::Biquad ap[2][8];
    float fb[2]{}, sr = 48000.0f;
    int64_t tick = 0;
};

/**
 * Chorus: two, three or four taps at different delays, each swept by its own
 * phase of the LFO, so it sounds like a section and not a doubling.
 *
 * The extra is `drift`, a slow random walk on each voice's delay, like a
 * bucket-brigade chip with an unsteady clock. At 0 it's a clean digital
 * chorus. Turned up, the voices keep drifting out of tune with each other.
 */
class Chorus final : public Effect {
  public:
    enum P { Rate, Depth, Voices, Spread, Drift, Mix, Gain, Count };
    Chorus() { initParams(5.0f); }
    ACIDULOUS_EFFECT_COMMON(Chorus)
    void onBlock(int64_t tickStart, int64_t, float) override { tick = tickStart; }
  private:
    dsp::DelayLine line[2];
    float drift[4]{}, sr = 48000.0f;
    uint32_t rng = 0x9e3779b9u;
    int64_t tick = 0;
};

/**
 * Tremolo and auto-pan, with rate, depth and shape.
 *
 * `pan` shifts the LFO phase between the two channels. At 0 both channels
 * dip together (tremolo), at full they're opposite (auto-pan), and in
 * between is a mix of both. `skew` bends the waveform so the dip can be a
 * stab or a swell.
 */
class Tremolo final : public Effect {
  public:
    enum P { Rate, Depth, Shape, Pan, Skew, Mix, Gain, Count };
    Tremolo() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Tremolo)
    void onBlock(int64_t tickStart, int64_t, float b) override { tick = tickStart; bpm = b; }
  private:
    float sr = 48000.0f, bpm = 120.0f;
    int64_t tick = 0;
};

/**
 * Width: mid and side, with the side scaled. Under 1 narrows toward mono,
 * over 1 widens.
 *
 * `below` makes everything under a frequency mono, since stereo bass falls
 * apart on club systems and phone speakers. `haas` delays one side by up to
 * 20 ms, which widens using arrival time instead of level, and folds to mono
 * as a comb instead of cancelling.
 */
class Width final : public Effect {
  public:
    enum P { Amount, Below, Haas, Rotate, Gain, Count };
    Width() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Width)
  private:
    dsp::DelayLine haasLine;
    dsp::Biquad lowL, lowR;
    float sr = 48000.0f;
};

/**
 * Frequency shifter.
 *
 * It adds the same number of hertz to every partial (100, 200, 300 becomes
 * 180, 280, 380), so the sound stops being harmonic. A pitch shifter would
 * multiply them instead. A few hertz gives slow phasing and a few hundred
 * sounds metallic.
 *
 * A Hilbert pair (two allpass chains 90 degrees apart across the band) and a
 * quadrature oscillator do the shift. `spread` shifts the two channels in
 * opposite directions. `feedback` sends the output back in, so each pass is
 * shifted again.
 */
class Shifter final : public Effect {
  public:
    enum P { Shift, Fine, Spread, Feedback, Mix, Gain, Count };
    Shifter() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Shifter)
  private:
    // A second-order allpass, the section a Hilbert chain is built from.
    struct Ap2 {
        float a = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        float process(float x) {
            const float y = a * a * (x + y2) - x2;
            x2 = x1; x1 = x; y2 = y1; y1 = dsp::guardDenormal(y);
            return y;
        }
        void clear() { x1 = x2 = y1 = y2 = 0.0f; }
    };
    Ap2 apI[2][4], apQ[2][4];
    float delayed[2]{}, fb[2]{}, phase = 0.0f, sr = 48000.0f;
};

/**
 * Harmonizer that shifts by scale degrees instead of semitones.
 *
 * It tracks the incoming note, finds its place in the scale, counts up the
 * chosen number of degrees and shifts by that, so a third can be major or
 * minor depending on the note. It uses the same 33 scales as the modifiers.
 *
 * Two voices. When the tracker isn't confident (drums, noise, silence) it
 * falls back to the plain chromatic interval.
 *
 * The shifter is a delay line read at a different speed, with two taps half
 * a window apart crossfaded so the wrap never lands mid-note. `window` sets
 * the length: short is tight but burbles, long is smooth but smears.
 */
class Harmonizer final : public Effect {
  public:
    enum P { Interval, Interval2, Scale, Key, Window, Feedback, Mix, Gain, Count };
    Harmonizer() { initParams(3.0f); }
    ACIDULOUS_EFFECT_COMMON(Harmonizer)
  private:
    /** One shifted voice: a read point sliding through the line. */
    struct Voice {
        float phase = 0.0f;
        float ratio = 1.0f;   // smoothed, so an interval change is a glide
    };
    dsp::DelayLine line[2];
    Voice voice[2][2];        // [channel][voice]
    // The tracker counts zero crossings over a window, like Cipher's carrier
    // tracking. Much cheaper than autocorrelation and good enough to find
    // the note.
    float zeroPrev = 0.0f, trackedHz = 0.0f;
    int32_t zeroCount = 0, zeroWindow = 0;
    float confidence = 0.0f;
    float fb[2]{}, sr = 48000.0f;
};

class Flanger final : public Effect {
  public:
    enum P { Rate, Depth, Feedback, Negative, Spread, Mix, Gain, Count };
    Flanger() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Flanger)
    void onBlock(int64_t tickStart, int64_t, float bpm) override { tick = tickStart; this->bpm = bpm; }
  private:
    dsp::DelayLine line[2];
    float sr = 48000.0f, bpm = 120.0f;
    int64_t tick = 0;
};

/**
 * Noise gate with threshold, attack, hold and release.
 *
 * `key` high-passes the detector, not the audio, so hum and handling noise
 * don't open the gate but the note still keeps its low end.
 *
 * `duck` sets how far down the gate closes. Fully closed for noise, or about
 * 12 dB down on drums so the room just gets quieter between hits.
 *
 * There's no `mix`, since half a gate just lets half the noise through.
 * Without one the base class doesn't touch it on a send bus, where gating
 * the send is what you'd want anyway.
 */
class Gate final : public Effect {
  public:
    enum P { Threshold, Hyst, Attack, Hold, Release, Duck, Key, Gain, Sidechain, Count };
    Gate() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Gate)
  private:
    /**
     * One detector for both channels. It takes the louder of the two and
     * both get the same gain, otherwise the stereo image would shift at
     * every note.
     */
    float env = 0.0f, gain = 0.0f, sr = 48000.0f;
    float holdLeft = 0.0f; // seconds still to run before the release starts
    bool open = false;
    dsp::Svf key[2];
    float keyHz = -1.0f; // what the key filters are set to, so they aren't rebuilt every block
};

/**
 * Upward compression: everything above the floor is pulled toward the
 * ceiling, so the quiet parts of a sound come up to meet the loud ones.
 * Gentle, it's loudness and detail; pushed, it's a wall of sound with every
 * tail and breath brought forward.
 *
 * `split` blends from one band to three (low, mid, high), so each part of
 * the spectrum is brought up on its own. `release` sets how fast it
 * recovers after a peak: slow is close to normalising, fast flattens
 * everything into a wall. See dsp/Swell.h.
 */
class Swell final : public Effect {
  public:
    enum P { Floor, Ceiling, Amount, Split, Release, Mix, Gain, Count };
    Swell() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Swell)
  private:
    dsp::Swell swell;
};

#undef ACIDULOUS_EFFECT_COMMON

} // namespace acidulous::effect
