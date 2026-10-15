#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Loudness as ITU-R BS.1770-4 and EBU R128 measure it: momentary (400 ms),
// short-term (3 s), integrated (the whole programme, gated) and true peak.
//
// Streaming services normalise to these figures. Fixed memory and no
// allocation after prepare(), so it can run on the audio thread for a whole
// song.
namespace acidulous::dsp {

class Loudness {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        hopFrames = static_cast<int32_t>(sampleRate * 0.1f + 0.5f); // 100 ms
        kWeighting();
        // 4x interpolation for true peak: a windowed sinc, twelve taps a phase.
        for (int32_t i = 0; i < kTpTaps; ++i) {
            const double t = (static_cast<double>(i) - kTpCentre) / 4.0;
            const double sinc = t == 0.0 ? 1.0 : std::sin(3.141592653589793 * t) / (3.141592653589793 * t);
            const double w = 0.42 - 0.5 * std::cos(6.283185307179586 * i / (kTpTaps - 1)) +
                             0.08 * std::cos(12.566370614359172 * i / (kTpTaps - 1));
            tpKernel[i] = static_cast<float>(sinc * w);
        }
        reset();
    }

    void reset() {
        for (auto &s : stage) s = {};
        hopSum = 0.0;
        hopFill = 0;
        hops = 0;
        hopAt = 0;
        for (double &h : hopRing) h = 0.0;
        for (int32_t i = 0; i < kBins; ++i) { binCount[i] = 0; binEnergy[i] = 0.0; }
        momentaryLufs = shortTermLufs = integratedLufs = kSilent;
        truePeak = 0.0f;
        for (auto &h : tpHist) for (float &x : h) x = 0.0f;
        tpAt = 0;
    }

    /** One block of the programme, stereo. */
    void process(const float *L, const float *R, int32_t frames) {
        for (int32_t i = 0; i < frames; ++i) {
            const double l = weigh(0, L[i]), r = weigh(1, R[i]);
            hopSum += l * l + r * r;
            peakOf(L[i], R[i]);
            if (++hopFill >= hopFrames) endHop();
        }
    }

    float momentary() const { return momentaryLufs; }
    float shortTerm() const { return shortTermLufs; }
    float integrated() const { return integratedLufs; }
    /** The highest true peak so far, in dBTP. */
    float truePeakDb() const { return truePeak > 0.0f ? 20.0f * std::log10(truePeak) : kSilent; }

    /** The reading for silence. */
    static constexpr float kSilent = -120.0f;

  private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, w1 = 0, w2 = 0; };

    /**
     * The K-weighting filter: a high shelf for the head's effect, then a high
     * pass for the ear's low sensitivity to bass.
     *
     * At 48 kHz the coefficients come from the standard's table. At other
     * rates they're derived from the same analogue prototype.
     */
    void kWeighting() {
        if (sr == 48000.0f) {
            shelf = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
            highpass = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
            return;
        }
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double A = std::pow(10.0, G / 40.0), w0 = 2.0 * 3.141592653589793 * f0 / sr;
            const double c = std::cos(w0), alpha = std::sin(w0) / (2.0 * Q), sa = 2.0 * std::sqrt(A) * alpha;
            const double a0 = (A + 1) - (A - 1) * c + sa;
            shelf = {A * ((A + 1) + (A - 1) * c + sa) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0,
                     A * ((A + 1) + (A - 1) * c - sa) / a0, 2 * ((A - 1) - (A + 1) * c) / a0,
                     ((A + 1) - (A - 1) * c - sa) / a0};
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double w0 = 2.0 * 3.141592653589793 * f0 / sr, c = std::cos(w0), alpha = std::sin(w0) / (2.0 * Q);
            const double a0 = 1 + alpha;
            highpass = {1.0, -2.0, 1.0, -2 * c / a0, (1 - alpha) / a0};
            // Unity numerator, like the standard's table.
        }
    }

    double weigh(int ch, float in) {
        State &s = stage[ch];
        const double x = in;
        const double y = shelf.b0 * x + shelf.b1 * s.x1 + shelf.b2 * s.x2 - shelf.a1 * s.y1 - shelf.a2 * s.y2;
        s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
        const double w = highpass.b0 * y + highpass.b1 * s.u1 + highpass.b2 * s.u2 - highpass.a1 * s.w1 - highpass.a2 * s.w2;
        s.u2 = s.u1; s.u1 = y; s.w2 = s.w1; s.w1 = w;
        return w;
    }

    static float lufsOf(double meanSquare) {
        return meanSquare > 1e-20 ? static_cast<float>(-0.691 + 10.0 * std::log10(meanSquare)) : kSilent;
    }

    /**
     * Runs every 100 ms: adds a gating block of the last 400 ms (75% overlap)
     * and updates the momentary, short-term and integrated readings.
     *
     * The integrated figure ignores blocks under -70 LUFS, then blocks more
     * than 10 LU below the average of the rest. Blocks are kept in a
     * histogram of 0.1 LU bins so memory stays fixed however long the song
     * is. It's exact to within a bin at the relative gate.
     */
    void endHop() {
        hopRing[hopAt] = hopSum / static_cast<double>(hopFrames);
        hopAt = (hopAt + 1) % kShortHops;
        hopSum = 0.0;
        hopFill = 0;
        if (hops < kShortHops) ++hops;
        const auto meanOf = [&](int32_t n) {
            double sum = 0.0;
            for (int32_t k = 1; k <= n; ++k) sum += hopRing[(hopAt - k + kShortHops) % kShortHops];
            return sum / n;
        };
        if (hops >= 4) {
            const double block = meanOf(4);
            momentaryLufs = lufsOf(block);
            if (momentaryLufs > -70.0f) {
                const int32_t bin = std::clamp(static_cast<int32_t>((momentaryLufs + 70.0f) * 10.0f), 0, kBins - 1);
                ++binCount[bin];
                binEnergy[bin] += block;
            }
            integratedLufs = gated();
        }
        if (hops >= kShortHops) shortTermLufs = lufsOf(meanOf(kShortHops));
    }

    float gated() const {
        int64_t count = 0;
        double energy = 0.0;
        for (int32_t i = 0; i < kBins; ++i) { count += binCount[i]; energy += binEnergy[i]; }
        if (count == 0) return kSilent;
        const float relative = lufsOf(energy / static_cast<double>(count)) - 10.0f;
        count = 0;
        energy = 0.0;
        for (int32_t i = 0; i < kBins; ++i) {
            const float binLufs = -70.0f + (static_cast<float>(i) + 0.5f) * 0.1f;
            if (binLufs < relative) continue;
            count += binCount[i];
            energy += binEnergy[i];
        }
        return count == 0 ? kSilent : lufsOf(energy / static_cast<double>(count));
    }

    /**
     * Checks the sample and the three points between it and the previous one,
     * per channel, for the true peak.
     *
     * The history is written twice, a window apart, so each phase's twelve
     * taps read one contiguous run without wrapping.
     */
    void peakOf(float l, float r) {
        const float in[2] = {l, r};
        for (int32_t ch = 0; ch < 2; ++ch) {
            tpHist[ch][tpAt] = tpHist[ch][tpAt + kTpPhaseTaps] = in[ch];
            // Newest first: tap k of a phase multiplies the sample k back.
            const float *x = &tpHist[ch][tpAt + kTpPhaseTaps];
            for (int32_t p = 0; p < 4; ++p) {
                float y = 0.0f;
                for (int32_t k = 0; k < kTpPhaseTaps; ++k) y += tpKernel[p + 4 * k] * x[-k];
                const float a = std::fabs(y);
                if (a > truePeak) truePeak = a;
            }
        }
        tpAt = (tpAt + 1) % kTpPhaseTaps;
    }

    static constexpr int32_t kShortHops = 30;          // 3 s of 100 ms hops
    static constexpr int32_t kBins = 800;              // -70 .. +10 LUFS in 0.1 LU
    static constexpr int32_t kTpPhaseTaps = 12;
    static constexpr int32_t kTpTaps = 4 * kTpPhaseTaps + 1;
    static constexpr double kTpCentre = (kTpTaps - 1) / 2.0;

    float sr = 48000.0f;
    int32_t hopFrames = 4800;
    Biquad shelf, highpass;
    State stage[2];
    double hopSum = 0.0;
    int32_t hopFill = 0, hops = 0, hopAt = 0;
    double hopRing[kShortHops]{};
    int32_t binCount[kBins]{};
    double binEnergy[kBins]{};
    float momentaryLufs = kSilent, shortTermLufs = kSilent, integratedLufs = kSilent;
    float tpKernel[kTpTaps]{};
    float tpHist[2][2 * kTpPhaseTaps]{};
    int32_t tpAt = 0;
    float truePeak = 0.0f;
};

} // namespace acidulous::dsp
