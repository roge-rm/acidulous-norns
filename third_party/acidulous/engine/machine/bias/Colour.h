#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/Math.h>

// The sound of a recording medium (tape, lo-fi digital), applied to Bias's
// output. The recordings on disk are never changed. Freeze can print it.
//
// One medium per machine, not per lane, like one tape in a four-track. Bleed
// is the exception and is taken between lanes before they're summed (in
// Bias.cpp).
//
// Things to keep right here: noise must be deterministic or two exports
// differ, saturation is scaled on the nominal level and not full scale,
// per-block values applied per sample must be interpolated or they click,
// and wrapped fractional reads go through `dsp::wrappedReadIndex`.
namespace acidulous::machine::bias {

/** The medium settings. Each one is a parameter. */
struct ColourSpec {
    float hiss = 0.0f;      // 0..1, the noise floor
    float hissTone = 0.5f;  // 0 dark, 1 bright
    float lowCut = 20.0f;   // Hz
    float highCut = 20000.0f;
    float bump = 0.0f;      // dB at bumpFreq
    float bumpFreq = 90.0f;
    float sat = 0.0f;       // 0..1
    float comp = 0.0f;      // 0..1
    float wow = 0.0f;       // 0..1, slow
    float flutter = 0.0f;   // 0..1, fast
    float speed = 1.0f;     // how unstable the transport is, as a rate scale
    float drop = 0.0f;      // 0..1, dropouts
    float bits = 24.0f;     // quantiser, 24 is off
    float rate = 1.0f;      // sample-and-hold ratio, 1 is off
    float smear = 0.0f;     // codec diffusion
    float width = 1.0f;     // 0 mono, 1 as recorded, >1 wider
    bool any = false;       // false when there's nothing to do
};

/**
 * The medium's processing, stereo.
 *
 * Only `prepare` allocates. `reset` puts it back exactly as `prepare` left
 * it, including the noise seed, so two exports of a song match sample for
 * sample.
 */
class Colour {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        // 60 ms is more than any wobble needs, and leaves room for the smear
        // to read behind the wobble without overlapping.
        wobbleL.prepare(static_cast<int32_t>(sr * 0.06f));
        wobbleR.prepare(static_cast<int32_t>(sr * 0.06f));
        reset();
    }

    void reset() {
        wobbleL.clear();
        wobbleR.clear();
        for (auto &b : band) b.reset();
        bumpL.reset();
        bumpR.reset();
        smearL.reset();
        smearR.reset();
        hissL.reset();
        hissR.reset();
        // The seed and everything derived from it. See the note above.
        noise = kSeed;
        wowPhase = 0.0f;
        flutterPhase = 0.25f;
        dropCountdown = 0;
        dropDepth = 1.0f;
        env = 0.0f;
        holdL = holdR = 0.0f;
        holdPos = 0.0f;
        cutNow = -1.0f;
        bumpNow = -1.0f;
    }

    /**
     * Coefficients are set here once a block, only when they've moved.
     * Anything that multiplies a sample is interpolated in [process]
     * instead, since stepping a gain once a block clicks.
     */
    void setBlock(const ColourSpec &s) {
        spec = s;
        if (std::fabs(s.highCut - cutNow) > 0.5f || std::fabs(s.lowCut - lowNow) > 0.5f) {
            cutNow = s.highCut;
            lowNow = s.lowCut;
            // Two poles each way. One pole would sound like a tone control.
            band[0].lowpass(s.highCut, 0.707f, sr);
            band[1].lowpass(s.highCut, 0.707f, sr);
            band[2].peak(s.lowCut, 0.0f, 0.707f, sr); // placeholder, set below
            highPass(band[2], s.lowCut);
            highPass(band[3], s.lowCut);
        }
        if (std::fabs(s.bump - bumpNow) > 0.01f || std::fabs(s.bumpFreq - bumpFreqNow) > 0.5f) {
            bumpNow = s.bump;
            bumpFreqNow = s.bumpFreq;
            bumpL.peak(s.bumpFreq, s.bump, 0.8f, sr);
            bumpR.peak(s.bumpFreq, s.bump, 0.8f, sr);
        }
        if (std::fabs(s.hissTone - toneNow) > 0.01f) {
            toneNow = s.hissTone;
            const float hz = 400.0f * std::pow(30.0f, s.hissTone); // 400 Hz .. 12 kHz
            hissL.lowpass(hz, 0.707f, sr);
            hissR.lowpass(hz, 0.707f, sr);
        }
        if (std::fabs(s.smear - smearNow) > 0.01f) {
            smearNow = s.smear;
            smearL.allpass(1200.0f, 0.4f + s.smear * 3.0f, sr);
            smearR.allpass(1700.0f, 0.4f + s.smear * 3.0f, sr);
        }
    }

    /** In place, one block, interleaved as two planes. */
    void process(float *L, float *R, int32_t frames) {
        if (!spec.any) return;
        const float wowRate = 0.7f * spec.speed;
        const float flutRate = 11.0f * spec.speed;
        const float wowDepth = spec.wow * spec.wow * sr * 0.004f;   // up to 4 ms
        const float flutDepth = spec.flutter * spec.flutter * sr * 0.0006f;
        const float base = sr * 0.02f; // the centre of the wobble
        const float levels = spec.bits >= 23.5f ? 0.0f : std::pow(2.0f, spec.bits - 1.0f);
        const float hold = spec.rate >= 0.999f ? 0.0f : spec.rate;

        for (int32_t i = 0; i < frames; ++i) {
            float l = L[i];
            float r = R[i];

            // --- Wow and flutter: a modulated delay, read with
            // dsp::wrappedReadIndex.
            if (wowDepth > 0.0f || flutDepth > 0.0f) {
                wowPhase += wowRate / sr;
                flutterPhase += flutRate / sr;
                if (wowPhase >= 1.0f) wowPhase -= 1.0f;
                if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
                const float mod = std::sin(dsp::kTwoPi * wowPhase) * wowDepth +
                                  std::sin(dsp::kTwoPi * flutterPhase) * flutDepth;
                wobbleL.write(l);
                wobbleR.write(r);
                // The two channels read slightly apart, since a tape's tracks
                // don't wobble in lockstep.
                l = wobbleL.read(base + mod);
                r = wobbleR.read(base - mod * 0.85f);
            }

            // --- The frequency band, the biggest difference between media.
            l = band[2].process(band[0].process(l));
            r = band[3].process(band[1].process(r));

            // --- Head bump.
            if (spec.bump != 0.0f) {
                l = bumpL.process(l);
                r = bumpR.process(r);
            }

            // --- Compression, with one envelope for both channels so the
            // stereo image doesn't shift.
            if (spec.comp > 0.0f) {
                const float peak = std::fmax(std::fabs(l), std::fabs(r));
                const float coeff = peak > env ? attack : release;
                env += (peak - env) * coeff;
                const float over = env / kNominal;
                if (over > 1.0f) {
                    const float g = 1.0f / (1.0f + (over - 1.0f) * spec.comp * 4.0f);
                    l *= g;
                    r *= g;
                }
            }

            // --- Saturation, scaled on kNominal instead of full scale.
            // Otherwise the same setting leaves quiet takes clean and wrecks
            // loud ones.
            if (spec.sat > 0.0f) {
                const float drive = 1.0f + spec.sat * 8.0f;
                const float norm = 1.0f / std::tanh(drive);
                l = std::tanh(l * drive / kNominal) * norm * kNominal;
                r = std::tanh(r * drive / kNominal) * norm * kNominal;
            }

            // --- Dropouts, like tape lifting off the head for a moment.
            if (spec.drop > 0.0f) {
                if (dropCountdown <= 0) {
                    // Roughly Poisson timing, so they don't line up with the
                    // music.
                    const float rate = 0.05f + spec.drop * 3.0f; // per second
                    dropCountdown = static_cast<int32_t>((0.2f + random()) * sr / rate);
                    dropDepth = 1.0f;
                } else {
                    --dropCountdown;
                }
                const float want = dropCountdown < static_cast<int32_t>(sr * 0.03f)
                                       ? 1.0f - spec.drop * 0.9f
                                       : 1.0f;
                dropDepth += (want - dropDepth) * 0.002f;
                l *= dropDepth;
                r *= dropDepth;
            }

            // --- The digital media: quantise, then hold.
            if (levels > 0.0f) {
                l = std::round(l * levels) / levels;
                r = std::round(r * levels) / levels;
            }
            if (hold > 0.0f) {
                holdPos += hold;
                if (holdPos >= 1.0f) {
                    holdPos -= 1.0f;
                    holdL = l;
                    holdR = r;
                }
                l = holdL;
                r = holdR;
            }

            // --- Smear: a short diffusion that softens transients, a bit
            // like a lossy codec.
            if (spec.smear > 0.0f) {
                l += (smearL.process(l) - l) * spec.smear;
                r += (smearR.process(r) - r) * spec.smear;
            }

            // --- Hiss. Deterministic, and added after the band filter.
            if (spec.hiss > 0.0f) {
                const float a = spec.hiss * spec.hiss * 0.05f;
                l += hissL.process(random2()) * a;
                r += hissR.process(random2()) * a;
            }

            // --- Width, like azimuth error on tape or joint stereo on a
            // codec.
            if (std::fabs(spec.width - 1.0f) > 0.001f) {
                const float m = (l + r) * 0.5f;
                const float s = (l - r) * 0.5f * spec.width;
                l = m + s;
                r = m - s;
            }

            L[i] = l;
            R[i] = r;
        }
    }

  private:
    /** The nominal level a mixed signal reaches, used as 0 dB here. */
    static constexpr float kNominal = 0.3f;
    static constexpr uint32_t kSeed = 0x1f35c0deu;

    void highPass(dsp::Biquad &b, float hz) {
        // A low shelf cutting everything below, since tape's low end loss is
        // a slope that keeps going and not a sharp corner.
        b.lowShelf(dsp::clampf(hz, 20.0f, sr * 0.45f), -24.0f, sr);
    }

    float random() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (1.0f / 16777216.0f);
    }
    float random2() { return random() * 2.0f - 1.0f; }

    ColourSpec spec;
    float sr = 48000.0f;
    dsp::DelayLine wobbleL, wobbleR;
    dsp::Biquad band[4], bumpL, bumpR, smearL, smearR, hissL, hissR;
    uint32_t noise = kSeed;
    float wowPhase = 0.0f, flutterPhase = 0.25f;
    float env = 0.0f;
    float attack = 0.01f, release = 0.0006f;
    int32_t dropCountdown = 0;
    float dropDepth = 1.0f;
    float holdL = 0.0f, holdR = 0.0f, holdPos = 0.0f;
    float cutNow = -1.0f, lowNow = -1.0f, bumpNow = -1.0f, bumpFreqNow = -1.0f;
    float toneNow = -1.0f, smearNow = -1.0f;
};

} // namespace acidulous::machine::bias
