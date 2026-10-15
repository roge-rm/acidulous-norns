#pragma once
#include <cstdint>
#include "Filter.h"
#include "Math.h"

// Twelve filter types from two SVF stages and a one-pole, with a drive stage
// at the input. Resonance is on the first stage.
namespace acidulous::dsp {

// The typical level of the signal reaching a drive stage, used to normalise
// its output.
constexpr float kDriveNominal = 0.3f;

class MultiFilter {
  public:
    enum Type : int32_t { LP6, LP12, LP18, LP24, HP6, HP12, HP18, HP24, BP6, BP12, Notch, Peak, TypeCount };
    enum Drive : int32_t { Clean, Valve, Diode, Clip, Fold, Crush, DriveCount };

    static const char *typeName(int t) {
        static const char *n[TypeCount] = {"LP6", "LP12", "LP18", "LP24", "HP6", "HP12", "HP18", "HP24", "BP6", "BP12", "notch", "peak"};
        return n[t < 0 ? 0 : (t >= TypeCount ? TypeCount - 1 : t)];
    }
    static const char *driveName(int d) {
        static const char *n[DriveCount] = {"clean", "valve", "diode", "clip", "fold", "crush"};
        return n[d < 0 ? 0 : (d >= DriveCount ? DriveCount - 1 : d)];
    }

    void setSampleRate(float sr) {
        sampleRate = sr;
        a.setSampleRate(sr);
        b.setSampleRate(sr);
        lastFc = -1.0f; // force set() to recompute at the new rate
    }
    void reset() { a.reset(); b.reset(); z = 0.0f; }

    /**
     * Sets the coefficients. Called four times a block per voice, so it
     * avoids libm calls where it can:
     *
     *  - nothing is recomputed if cutoff, resonance and type haven't changed;
     *  - `b` (the second stage) is only set for the three types that use it;
     *  - `onePole` (the 6 dB path) is only set for the four types that use it.
     *
     * The drive is stored first because it can change on its own.
     */
    void set(float cutoffHz, float resonance01, int type, int drive, float driveAmount) {
        this->type = type < 0 ? 0 : (type >= TypeCount ? TypeCount - 1 : type);
        this->drive = drive;
        this->driveAmount = driveAmount;
        const float fc = clampf(cutoffHz, 20.0f, sampleRate * 0.45f);
        if (fc == lastFc && resonance01 == lastRes && this->type == lastType) return;
        lastFc = fc;
        lastRes = resonance01;
        lastType = this->type;
        a.set(fc, resonance01);
        if (this->type == LP24 || this->type == HP24 || this->type == BP12) b.set(fc, 0.0f);
        if (this->type == LP6 || this->type == LP18 || this->type == HP6 || this->type == HP18) {
            onePole = clampf(1.0f - std::exp(-kTwoPi * fc / sampleRate), 0.0f, 1.0f);
        }
    }

    float process(float x) {
        x = shape(x);
        switch (type) {
        case LP6: return lp1(x);
        case LP12: return a.step(x).lp;
        case LP18: return lp1(a.step(x).lp);
        case LP24: return b.step(a.step(x).lp).lp;
        case HP6: return x - lp1(x);
        case HP12: return a.step(x).hp;
        case HP18: { const float y = a.step(x).hp; return y - lp1(y); }
        case HP24: return b.step(a.step(x).hp).hp;
        case BP6: return a.step(x).bp * a.bandNorm();
        case BP12: return b.step(a.step(x).bp * a.bandNorm()).bp * b.bandNorm();
        case Notch: { const auto o = a.step(x); return o.lp + o.hp; }
        default: { const auto o = a.step(x); return o.lp - o.hp; }
        }
    }

  private:
    float lastFc = -1.0f, lastRes = -1.0f;
    int lastType = -1;

    float lp1(float x) { z = guardDenormal(z + (x - z) * onePole); return z; }
    float shape(float x) {
        if (drive == Clean || driveAmount <= 0.0f) return x;
        const float g = 1.0f + driveAmount * 24.0f;
        switch (drive) {
        // Normalised so a signal at kDriveNominal comes out the same size it
        // went in. Dividing by sqrt(g) instead boosts quiet signals by up to
        // 10 dB and cuts loud ones, which can make feedback loops oscillate.
        case Valve: return fastTanh(x * g) * (kDriveNominal / fastTanh(kDriveNominal * g));
        case Diode: {
            // Remove the bias offset before normalising so its DC isn't scaled.
            const float bias = fastTanh(0.35f);
            const float at = fastTanh(kDriveNominal * g + 0.35f) - bias;
            const float norm = at > 1e-6f ? kDriveNominal / at : 1.0f;
            return (fastTanh(x * g + 0.35f) - bias) * norm;
        }
        case Clip: return clampf(x * g, -1.0f, 1.0f) / std::sqrt(g);
        case Fold: { const float t = x * g * 0.25f + 0.25f; return (4.0f * std::fabs(t - std::floor(t + 0.5f)) - 1.0f) / std::sqrt(g); }
        default: { // Crush: bit reduction, harsher the more drive
            const float levels = std::exp2(12.0f - driveAmount * 9.0f);
            return std::round(x * levels) / levels;
        }
        }
    }

    Svf a, b;
    float sampleRate = 48000.0f, onePole = 0.5f, z = 0.0f, driveAmount = 0.0f;
    int type = LP12, drive = Clean;
};

} // namespace acidulous::dsp
