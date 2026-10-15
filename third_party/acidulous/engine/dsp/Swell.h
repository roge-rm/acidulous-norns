#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/dsp/Bands.h>
#include <engine/dsp/Math.h>

namespace acidulous::dsp {

/**
 * Upward compression in up to three bands: everything above the floor is
 * pulled toward the ceiling, so quiet detail comes up and the loudest peaks
 * come down to meet it. At full amount and a slow release it acts like a
 * normaliser; at a fast release it follows the waveform and squashes a sound
 * into a wall.
 *
 * Shared by the Swell effect and Nexus's swell module, one sample at a time.
 *
 * The bands are split at 300 Hz and 5 kHz by dsp/Bands.h, so with no gain
 * on them they add back to exactly the input: no phase shift, no dip at the
 * crossovers. `split` blends from one gain for the whole signal (0) to a gain
 * per band (1). A band's gain is kept within [kSpread] dB of the whole
 * signal's, so a band with almost nothing in it can't be pulled up on its own
 * and drag the tone around.
 *
 * The detector looks [kLook] samples ahead: the audio is delayed by that much
 * and the gain is set from the loudest sample in the window, so a peak is
 * caught before it arrives. That's the effect's whole latency.
 */
class Swell {
  public:
    static constexpr int kLook = 8;

    struct Settings {
        float floorDb = -40.0f;   // below this (and its knee) nothing is touched
        float ceilingDb = -3.0f;  // what everything above the floor is pulled toward
        float amount = 0.5f;      // 0 nothing, 1 all the way to the ceiling
        float split = 1.0f;       // 0 one band, 1 three
        float releaseSec = 0.15f; // how fast the gain comes back up after a peak
        float mix = 1.0f;         // dry to wet
    };

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (auto &c : ch) c.split.prepare(sr);
        holdSamples = static_cast<int32_t>(0.005f * sr);
        // A cut slides in over about the look-ahead window, so it has
        // arrived before the peak it was set for. A boost comes up over a
        // couple of milliseconds instead: a note starting out of silence
        // would otherwise jump by the whole boost in eight samples, a click.
        glideDown = 1.0f - std::exp(-3.0f / kLook);
        glideUp = onePoleCoeff(0.002f, sr);
        reset();
    }

    void reset() {
        // Cleared field by field, since the split keeps its coefficients.
        for (auto &c : ch) {
            c.split.reset();
            for (float &v : c.dry) v = 0.0f;
            for (auto &b : c.band) for (float &v : b) v = 0.0f;
        }
        for (auto &d : det) d = Detector{};
        for (float &g : gain) g = 0.0f;
        at = 0;
    }

    void set(const Settings &s) {
        floorDb = s.floorDb;
        ceilingDb = s.ceilingDb;
        amount = clampf(s.amount, 0.0f, 1.0f);
        split = clampf(s.split, 0.0f, 1.0f);
        release = onePoleCoeff(std::max(s.releaseSec, 0.001f), sr);
        mix = clampf(s.mix, 0.0f, 1.0f);
        // A slight dip in the mids when split, so three boosted bands come
        // out sounding flat rather than honky. Gone at amount 0, so the
        // effect is transparent there.
        midTrimDb = -1.5f * split * amount;
    }

    /** Changes just the amount, cheaply enough to do every sample. */
    void setAmount(float a) {
        amount = clampf(a, 0.0f, 1.0f);
        midTrimDb = -1.5f * split * amount;
    }

    /** One stereo sample, in place. */
    void process(float &l, float &r) {
        float band[2][kBands];
        ch[0].split.split(l, band[0]);
        ch[1].split.split(r, band[1]);

        // Detectors: the whole signal, then each band, on the louder side.
        float level[kDetectors];
        level[0] = std::max(std::fabs(l), std::fabs(r));
        for (int b = 0; b < kBands; ++b) level[b + 1] = std::max(std::fabs(band[0][b]), std::fabs(band[1][b]));

        // Write this sample into the look-ahead line and read the one kLook ago.
        const int out = (at + 1) % kLine;
        float bandOut[2][kBands], dryOut[2];
        for (int c = 0; c < 2; ++c) {
            ch[c].dry[at] = c == 0 ? l : r;
            for (int b = 0; b < kBands; ++b) ch[c].band[b][at] = band[c][b];
            dryOut[c] = ch[c].dry[out];
            for (int b = 0; b < kBands; ++b) bandOut[c][b] = ch[c].band[b][out];
        }

        float targetDb[kDetectors], weight = 0.0f;
        for (int d = 0; d < kDetectors; ++d) targetDb[d] = gainFor(follow(det[d], level[d]), d == 0 ? &weight : nullptr);
        at = out;

        // Each band's gain: from the whole signal's at split 0 to its own at
        // split 1, never far from the whole signal's.
        const float whole = targetDb[0];
        float wet[2] = {0.0f, 0.0f};
        for (int b = 0; b < kBands; ++b) {
            float g = whole + (targetDb[b + 1] - whole) * split;
            g = clampf(g, whole - kSpread, whole + kSpread);
            // The mid dip only where the effect is working, so a sound under
            // the floor comes through untouched.
            if (b == 1) g += midTrimDb * weight;
            gain[b] += (g - gain[b]) * (g > gain[b] ? glideUp : glideDown);
            gain[b] = guardDenormal(gain[b]);
            const float lin = std::exp(gain[b] * 0.115129255f); // dB to gain
            wet[0] += bandOut[0][b] * lin;
            wet[1] += bandOut[1][b] * lin;
        }
        l = dryOut[0] + (wet[0] - dryOut[0]) * mix;
        r = dryOut[1] + (wet[1] - dryOut[1]) * mix;
    }

    /** One mono sample. */
    float process(float x) {
        float l = x, r = x;
        process(l, r);
        return l;
    }

  private:
    static constexpr int kBands = 3;
    static constexpr int kDetectors = kBands + 1;
    static constexpr int kLine = kLook + 1;
    /** How far a band's gain may stray from the whole signal's, in dB. */
    static constexpr float kSpread = 9.0f;
    /** Most a quiet sound is brought up, in dB, so a noise floor can't be pulled up to the ceiling. */
    static constexpr float kMaxBoost = 36.0f;
    /** The knee under the floor, in dB, over which the effect fades in. */
    static constexpr float kKnee = 12.0f;

    struct Channel {
        ThreeBands split;
        float dry[kLine]{};
        float band[kBands][kLine]{};
    };
    struct Detector {
        float window[kLine]{}; // the last kLine levels, for the look-ahead peak
        int pos = 0;
        float env = 0.0f;
        int32_t hold = 0;
    };

    /**
     * The loudest level over the look-ahead window, held for a few
     * milliseconds and then let go at the release rate. Peaks are caught
     * at once, since the window has already seen them.
     */
    float follow(Detector &d, float level) {
        d.window[d.pos] = level;
        d.pos = (d.pos + 1) % kLine;
        float peak = 0.0f;
        for (float v : d.window) peak = std::max(peak, v);
        if (peak >= d.env) {
            d.env = peak;
            d.hold = holdSamples;
        } else if (d.hold > 0) {
            --d.hold;
        } else {
            d.env += (peak - d.env) * release;
            d.env = guardDenormal(d.env);
        }
        return d.env;
    }

    /**
     * The gain in dB for a detected level: toward the ceiling above the
     * floor, nothing below, eased in across the knee. [weight], if given,
     * gets how far into the knee the level is, 0 to 1.
     */
    float gainFor(float env, float *weight) const {
        if (env < 1e-6f || amount <= 0.0f) return 0.0f;
        const float level = 20.0f * std::log10(env);
        float w = (level - (floorDb - kKnee)) / kKnee;
        if (w <= 0.0f) return 0.0f;
        if (w < 1.0f) w = w * w * (3.0f - 2.0f * w);
        else w = 1.0f;
        if (weight != nullptr) *weight = w;
        return std::min(amount * w * (ceilingDb - level), kMaxBoost);
    }

    float sr = 48000.0f;
    float glideUp = 0.01f, glideDown = 0.3f, release = 0.001f;
    int32_t holdSamples = 240;
    float floorDb = -40.0f, ceilingDb = -3.0f, amount = 0.5f, split = 1.0f, mix = 1.0f, midTrimDb = 0.0f;
    Channel ch[2];
    Detector det[kDetectors];
    float gain[kBands]{};
    int at = 0;
};

} // namespace acidulous::dsp
