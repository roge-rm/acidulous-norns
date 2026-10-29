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

/**
 * Delay on a note value, in four modes:
 *
 *   - normal: an echo a side.
 *   - ping-pong: the echoes bounce from side to side.
 *   - multi-tap: 2 to 4 taps inside the time, spread across the stereo
 *     field, with `spacing` bunching them early or late. The last tap, at
 *     the full time, feeds back.
 *   - reverse: each stretch of `time` is played backwards, with short
 *     crossfades between stretches. Feedback plays an echo forwards again.
 *
 * The extras are `duck` (echoes drop while the track plays) and `wobble`
 * (tape-like drift of the time).
 */
class Delay final : public Effect {
  public:
    enum P { Time, Feedback, Tone, Mode, Mix, Duck, Wobble, Gain, Taps, Spacing, Count };
    enum ModeKind { Normal, PingPong, MultiTap, Reverse };
    static constexpr int32_t kMaxTaps = 4;
    Delay() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Delay)
    void onBlock(int64_t tickStart, int64_t, float bpm) override;
  private:
    void runMultiTap(float *L, float *R, int32_t frames, bool stereoIn, float target, float toneCoeff);
    void runReverse(float *L, float *R, int32_t frames, bool stereoIn, float target, float toneCoeff, int timeIdx,
                    double ticksPerFrame);
    float wobbleDrift(float depth);
    float duckedMix(float inL, float inR);
    dsp::DelayLine line[2];
    // Reverse reads from these, long enough for a whole 3 s stretch. Every
    // mode writes them as it writes line, so a switch to or from reverse
    // carries on with the same echoes.
    dsp::ForgetfulDelayLine revLine[2];
    float readSamples = 24000.0f, lp[2]{}, duckEnv = 0.0f, wobblePhase = 0.0f, sr = 48000.0f, bpm = 120.0f;
    bool snapRead = true; // see reset()
    // Per-block, for the helpers: the knobs as process() read them.
    float fb_ = 0.0f, mix_ = 0.0f, duck_ = 0.0f, wobbleDepth_ = 0.0f, duckAtk_ = 0.0f, duckRel_ = 0.0f, glide_ = 0.0f;
    // Reverse: frames into the current stretch; its length and the tail it
    // plays on into the next, fixed when it starts; the same for the one
    // fading out.
    float revAt = 0.0f, revLen = 0.0f, revTail = 0.0f, revPrevLen = 0.0f;
    bool revStart = true;
    // The fade into the current stretch: frames into it and its length. It
    // fades from the stretch before, or from the other mode's tap at
    // revFadeD frames back when reverse has just come on.
    float revFadeAt = 0.0f, revFadeLen = 0.0f, revFadeD = 0.0f;
    bool revFadeNormal = false;
    // The current stretch's length in ticks; the clock jumped this block.
    double revTicks = 0.0;
    bool revJumped = false;
    // The clock's tick to a fraction, moved on by the frames played and kept
    // within a tick of the block's start; the stretch now playing as a count
    // of stretches from the song's start.
    double clock = 0.0;
    bool synced = false;
    int64_t revWindow = 0;
    /** A stretch's length in ticks: the note value, halved until twice it fits the line. */
    double stretchTicks(int timeIdx) const;
    void startStretch(double ticks, double ticksPerFrame, float into);
    /** How far a stretch may run before it starts over, so every read stays in revLine. */
    float stretchLimit() const;
};

class Eq final : public Effect {
  public:
    enum P { LowGain, LowFreq, MidGain, MidFreq, MidQ, HighGain, HighFreq, Tilt, Gain, Count };
    Eq() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Eq)
  private:
    dsp::Biquad low[2], mid[2], high[2], tiltLo[2], tiltHi[2];
    float sr = 48000.0f;
    /** What each band was designed from, so a later slice of a block redesigns only what moved. */
    float lowFor[2] = {}, midFor[3] = {}, highFor[2] = {}, tiltFor = 0.0f;
    void remember();
    void designMoved();
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
    void onBlock(int64_t tickStart, int64_t, float b) override { tick = tickStart; bpm = b; }
  private:
    dsp::Biquad ap[2][8];
    float fb[2]{}, sr = 48000.0f, bpm = 120.0f;
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
    void onBlock(int64_t tickStart, int64_t, float b) override { tick = tickStart; bpm = b; }
  private:
    dsp::DelayLine line[2];
    float drift[4]{}, sr = 48000.0f, bpm = 120.0f;
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
