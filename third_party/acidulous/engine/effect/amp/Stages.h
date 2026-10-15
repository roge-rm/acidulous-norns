#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/Filter.h>
#include <engine/dsp/Math.h>
#include <engine/dsp/MultiFilter.h>

// The stages between the guitar and the speaker: two preamp stages, an
// interactive tone stack, and a power stage whose supply sags.
//
// The order follows a real amp:
//
//   input HP -> low shelf -> bright cap -> NL A -> interstage HP -> NL B
//   -> tone stack -> presence -> [sag detector] -> NL C -> output transformer
//
// Presence comes before the power stage. In a real amp it's inside the
// negative feedback loop and makes the output stage work harder in the upper
// mids. After the power stage it would just be a treble knob.
//
// The interstage highpass keeps high gain tight, like the coupling capacitor
// between stages in a real amp. It also removes the DC offset stage A's bias
// creates, which stage B would otherwise clip around.
//
// There are no hard clamps, since those alias badly even at 2x. The power
// stage is a tanh inside a tanh, which gives a harder knee while staying
// smooth, so 2x oversampling is enough.
namespace acidulous::effect::amp {

/** The amp type. It changes much more than the tone stack. */
enum Stack : int32_t { Us = 0, Uk = 1, Modern = 2, StackCount = 3 };

/** Everything set by the amp type, looked up once a block. */
struct Voicing {
    float bassHz, bassLo, bassHi;
    float midRef, scoopBase, scoopSpan, midQ;
    float trebRef, trebLo, trebHi;
    float inShelfDb, inShelfHz;
    float brightDb, brightHz;
    float interHz;
    float stageBFrom;
    float sagScale;
};

inline Voicing voicingOf(int32_t stack) {
    switch (stack) {
    // A passive stack can only cut, so every shelf goes from a large cut up
    // to about zero instead of from a cut to a boost. The stage after it
    // makes up the gain.
    case Uk:
        return {90.0f, -15.0f, 2.0f, 650.0f, 3.0f, 12.0f, 0.70f, 3000.0f, -14.0f, 2.0f,
                -4.0f, 150.0f, 5.0f, 3000.0f, 90.0f, 0.30f, 0.8f};
    case Modern:
        // The least scooped of the three, since a modern high-gain amp gets
        // its scoop from the gain stages and has a flatter stack.
        return {70.0f, -12.0f, 3.0f, 800.0f, 2.0f, 5.0f, 1.00f, 4500.0f, -10.0f, 3.0f,
                -8.0f, 220.0f, 0.0f, 3000.0f, 160.0f, 0.15f, 0.5f};
    default:
        return {120.0f, -14.0f, 2.0f, 500.0f, 3.0f, 9.0f, 0.55f, 2200.0f, -16.0f, 2.0f,
                -3.0f, 120.0f, 3.0f, 2200.0f, 72.0f, 0.35f, 1.0f};
    }
}

/**
 * The tone stack: three sections whose coefficients are cross-coupled.
 *
 * This isn't a solved RC network. The real Fender/Marshall stack is third
 * order and would need a cubic factorisation per block on the audio thread,
 * which can give NaNs at extreme settings, and it gets close to unstable
 * with all pots at zero. A cascade of cookbook sections can't go unstable,
 * which matters since anything can be automated.
 *
 * What you hear from a real stack:
 *
 *   - mid isn't a fixed boost or cut, it sets the floor of a scoop whose
 *     corner slides
 *   - bass and treble up deepens that scoop, since a passive stack can only
 *     cut. A Marshall on all ten is about 10 dB down at 1 kHz
 *   - treble's corner moves with mid, since they share a node
 *
 * The scoop depth follows bass times treble, so turning either one down
 * fills the mids back in. Separate shelves wouldn't do that.
 */
class ToneStack {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        reset();
    }
    void reset() {
        low.reset();
        mid.reset();
        high.reset();
    }

    void set(const Voicing &v, float b, float m, float t) {
        // Make-up gain so the stack doesn't also change the level. It's
        // measured from the stack's own response with every control halfway,
        // and only redone when the voicing changes since it's not cheap.
        if (v.midRef != builtFor) {
            builtFor = v.midRef;
            dsp::Biquad rl, rm, rh;
            rl.lowShelf(v.bassHz, (v.bassLo + v.bassHi) * 0.5f, sr);
            rm.peak(v.midRef, -(v.scoopBase + v.scoopSpan * 0.25f) * 0.5f, v.midQ + 0.175f, sr);
            rh.highShelf(v.trebRef * 0.7071f, (v.trebLo + v.trebHi) * 0.5f, sr);
            const float at = rl.magnitudeAt(1000.0f, sr) * rm.magnitudeAt(1000.0f, sr) *
                             rh.magnitudeAt(1000.0f, sr);
            trim = at > 1e-6f ? 1.0f / at : 1.0f;
        }
        low.lowShelf(v.bassHz, v.bassLo + (v.bassHi - v.bassLo) * b, sr);
        const float midHz = dsp::clampf(v.midRef * std::pow(2.0f, -1.0f * t + 0.35f * b), 60.0f, 4000.0f);
        // At least 0.5. `Biquad::peak` clamps Q at 0.1, and a 15 dB cut at Q
        // 0.1 is a three-octave hole instead of a scoop.
        const float q = dsp::clampf(v.midQ + 0.7f * b * t, 0.5f, 4.0f);
        mid.peak(midHz, -(v.scoopBase + v.scoopSpan * b * t) * (1.0f - m), q, sr);
        high.highShelf(dsp::clampf(v.trebRef * std::pow(2.0f, -0.5f * m), 300.0f, sr * 0.4f),
                       v.trebLo + (v.trebHi - v.trebLo) * t, sr);
    }

    float process(float x) { return high.process(mid.process(low.process(x))) * trim; }

    /** The chain's magnitude at [hz], for the harness. */
    float magnitudeAt(float hz) const {
        return low.magnitudeAt(hz, sr) * mid.magnitudeAt(hz, sr) * high.magnitudeAt(hz, sr) * trim;
    }

  private:
    dsp::Biquad low, mid, high;
    float sr = 48000.0f, trim = 1.0f, builtFor = -1.0f;
};

/**
 * The power stage and its sagging supply.
 *
 * Sag is detected from the stage's input (feedforward). The output stage is
 * what pulls the rail down in a real amp, and detecting on the output would
 * make a feedback loop that oscillates at 30-80 Hz with high sag and master.
 *
 * 12 ms attack and 220 ms release. A rectifier only conducts on peaks, so
 * the rail refills much more slowly than it empties, and that's the sound.
 */
class PowerStage {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        atk = dsp::onePoleCoeff(0.012f, sr);
        rel = dsp::onePoleCoeff(0.220f, sr);
        reset();
    }
    void reset() {
        demand = 0.0f;
        railNow = 1.0f;
        ot.reset();
        otLow.reset();
        block.reset();
    }

    void set(float master, float sagAmt, float sagScale) {
        gm = 1.0f + master * 14.0f;
        sag = sagAmt * sagScale;
        // The normaliser is worked out once a block from the rail at the start
        // of the block. Doing it per sample would cost a divide and a tanh
        // for under 0.5 dB of difference.
        const float atNom = std::tanh(1.2f * std::tanh(dsp::kDriveNominal * gm / railNow));
        norm = atNom > 1e-6f ? dsp::kDriveNominal / atNom : 1.0f;
        // The output transformer: a peak whose depth follows the rail, so a
        // sagging amp sounds loose and not just compressed.
        ot.peak(85.0f, 2.5f * railNow, 0.8f, sr);
        otLow.lowpass(11000.0f, 0.707f, sr);
        block.setSampleRate(sr);
        block.set(22.0f, 0.299f); // Butterworth: k = 1/Q, so Q 0.707 is res 0.299
    }

    float process(float x) {
        const float a = std::fabs(x);
        demand += (a - demand) * (a > demand ? atk : rel);
        demand = dsp::undenormal(demand);
        const float load = dsp::clampf(demand * gm / 3.0f, 0.0f, 1.0f);
        railNow = 1.0f - sag * 0.45f * load;
        // Drive harder and clip lower as the rail drops, which compresses.
        const float y = std::tanh(1.2f * std::tanh(x * gm / railNow)) * norm * railNow;
        // The transformer can't pass DC. This comes after the sag because a
        // moving offset would thump.
        return block.highpass(otLow.process(ot.process(y)));
    }

    float rail() const { return railNow; }

  private:
    dsp::Biquad ot, otLow;
    dsp::Svf block;
    float sr = 48000.0f, atk = 0.001f, rel = 1e-4f;
    float demand = 0.0f, railNow = 1.0f, gm = 1.0f, sag = 0.0f, norm = 1.0f;
};

} // namespace acidulous::effect::amp
