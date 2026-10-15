#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/Math.h>

// The electric pianos: what a hammer strikes when it isn't a string, and
// what hears it.
//
// A tine or a reed is a bar fixed at one end. Struck, it rings at its note,
// nearly pure, with a few overtones far above it that are gone in a moment
// (the bell in a tine's attack). A tine has a tonebar beside it, tuned to
// the same note, that takes its energy and gives it back: the long sustain.
//
// The bar barely makes a sound of its own. A pickup hears it, and the
// pickup is where the tone comes from: its field isn't even across the
// bar's swing, so a hard blow swings the bar into the curved part and the
// harmonics come up with it, as loud as the note itself at the hardest (the
// recordings: 27 dB under at middle C played softly, level with it played
// hard). That is the bark, and it can't be drawn on.
//   - Magnetic, for a tine: the flux through the coil is a bell around the
//     magnet's axis, 1 / (1 + u^2), with u the bar's place over its reach
//     plus how far it sits off the axis. On the axis, the note comes out an
//     octave up; off it, the note, and the further off the purer.
//   - Electrostatic, for a reed: the reed is one plate of a capacitor, and
//     what it holds grows as it swings nearer the other plate and shrinks
//     as it swings away, more one way than the other: 1 / (1 - 0.6 tanh u).
//     So the even harmonics come first. (As 1 / (1 - u) itself, a hard blow
//     brought the reed so near that the note spiked 18 dB over full scale.)
// Either way what's heard is how fast that changes: the voltage of a coil,
// the current of a charged plate.
namespace acidulous::machine::hammer {
using dsp::clampf;

/** A struck bar, fixed at one end: its note, its tonebar, two overtones. */
class Bar {
  public:
    static constexpr int kModes = 4;

    /** What one bar is: the note (Hz) and each mode's place, level and ring. */
    struct Spec {
        float hz = 261.6f;
        /** The tonebar against the bar: how much of the note it holds, and how far it's tuned off, cents. */
        float tonebar = 0.5f, tonebarCents = 0.6f;
        /** The bar's own ring at its note and the tonebar's, T60 seconds. */
        float ring = 10.0f, barRing = 20.0f;
        /** The overtones: ratio to the note, level against it, T60 seconds. */
        float ratio[2] = {7.1f, 20.0f}, level[2] = {0.25f, 0.08f}, overRing[2] = {0.3f, 0.08f};
    };

    void prepare(float sampleRate) { sr = sampleRate; clear(); }

    void clear() {
        pitch = 1.0f;
        for (int m = 0; m < kModes; ++m) y1[m] = y2[m] = 0.0f;
        pulseLeft = 0;
        lastForce = 0.0f;
        level = 0.0f;
    }

    /** Sets the modes for [s]; a bar that's ringing keeps its motion. */
    void tune(const Spec &s) {
        spec = s;
        const float hz[kModes] = {s.hz, s.hz * std::exp2(s.tonebarCents / 1200.0f), s.hz * s.ratio[0], s.hz * s.ratio[1]};
        const float amp[kModes] = {1.0f - 0.6f * s.tonebar, 0.6f * s.tonebar, s.level[0], s.level[1]};
        for (int m = 0; m < kModes; ++m) {
            baseHz[m] = hz[m];
            baseAmp[m] = amp[m];
            w[m] = 6.28318530718f * clampf(hz[m] * pitch, 10.0f, 0.45f * sr) / sr;
            // Past what the rate can hold, a mode isn't there.
            gain[m] = hz[m] * pitch < 0.45f * sr ? std::sin(w[m]) * amp[m] : 0.0f;
        }
        setRing(s.ring, s.barRing, s.overRing[0], s.overRing[1]);
    }

    /** Bends the bar by [ratio] of its tuned pitch, ringing on: each mode's decay kept. */
    void setPitch(float ratio) {
        if (ratio == pitch) return;
        pitch = ratio;
        for (int m = 0; m < kModes; ++m) {
            const float hz = baseHz[m] * ratio;
            w[m] = 6.28318530718f * clampf(hz, 10.0f, 0.45f * sr) / sr;
            gain[m] = hz < 0.45f * sr ? std::sin(w[m]) * baseAmp[m] : 0.0f;
            const float r = std::sqrt(std::fmax(-a2[m], 0.0f));
            a1[m] = 2.0f * r * std::cos(w[m]);
        }
    }

    /** How long each mode rings, T60s: the bar, the tonebar, the overtones (for a damper too). */
    void setRing(float bar, float tonebar, float over1, float over2) {
        const float t[kModes] = {bar, tonebar, over1, over2};
        for (int m = 0; m < kModes; ++m) {
            const float r = std::exp(-6.907755f / (std::fmax(t[m], 0.005f) * sr));
            a1[m] = 2.0f * r * std::cos(w[m]);
            a2[m] = -r * r;
        }
    }

    /**
     * Strikes it: a blow of [contact] seconds, half a sine of force, that
     * swings the bar by about [swing] at its note. A short blow reaches the
     * overtones; a long one is too slow for them.
     */
    void strike(float swing, float contact) {
        pulseLength = std::max(2, static_cast<int>(contact * sr));
        pulseLeft = pulseLength;
        // Half a sine of [n] samples sums to 2n / pi.
        pulseScale = swing * 3.14159265f / (2.0f * static_cast<float>(pulseLength));
    }

    /** One sample: where the bar is. */
    float step() {
        float force = 0.0f;
        if (pulseLeft > 0) {
            force = pulseScale * std::sin(3.14159265f * static_cast<float>(pulseLength - pulseLeft) / static_cast<float>(pulseLength));
            --pulseLeft;
        }
        lastForce = force;
        float x = 0.0f;
        for (int m = 0; m < kModes; ++m) {
            const float y = gain[m] * force + a1[m] * y1[m] + a2[m] * y2[m];
            y2[m] = y1[m];
            y1[m] = y;
            x += y;
        }
        level += (std::fabs(x) - level) * 0.0005f;
        return x;
    }

    bool striking() const { return pulseLeft > 0; }
    /** The blow's force this sample, in the swing's units: what a case hears of it. */
    float blow() const { return lastForce; }
    float loudness() const { return level; }

  private:
    float sr = 48000.0f;
    Spec spec;
    float w[kModes] = {}, gain[kModes] = {}, a1[kModes] = {}, a2[kModes] = {};
    float y1[kModes] = {}, y2[kModes] = {};
    /** The modes as tuned, before a bend, and the bend now. */
    float baseHz[kModes] = {}, baseAmp[kModes] = {}, pitch = 1.0f;
    int pulseLeft = 0, pulseLength = 1;
    float pulseScale = 0.0f, lastForce = 0.0f;
    float level = 0.0f;
};

/** A pickup, one per voice: what it hears of the bar's place, sample by sample. */
class Pickup {
  public:
    /** Linear: no pickup, the bar heard through the air (a celesta, a toy piano). */
    enum Kind { Magnetic, Electrostatic, Linear };

    /**
     * [kind]; [reach]: how far the bar swings before the curve bends, in
     * the bar's units (nearer the pickup is less); [off]: how far it sits
     * off the axis, in reaches; [w0]: the note in radians a sample, so a
     * key's level doesn't follow its pitch.
     */
    void set(Kind k, float reach, float off, float w0) {
        kind = k;
        inv = 1.0f / std::fmax(reach, 1e-3f);
        offset = off;
        perNote = 1.0f / std::fmax(w0, 1e-4f);
        // For the level: what it gives for a mf swing, one cycle of it,
        // against the swing. (Its slope at rest is nothing on the magnet's
        // axis, where it hears only the octave: divided by that, a tine
        // there came out 14 dB over the rest.)
        constexpr int kSteps = 64;
        double power = 0.0;
        float before = field(0.0f);
        for (int n = 1; n <= kSteps; ++n) {
            const float now = field(kRefSwing * inv * std::sin(6.28318530718f * static_cast<float>(n) / kSteps));
            const float d = (now - before) * static_cast<float>(kSteps) / 6.28318530718f;
            power += static_cast<double>(d) * d;
            before = now;
        }
        slope = std::fmax(static_cast<float>(std::sqrt(power / kSteps)) / (kRefSwing * 0.70710678f), 1e-3f);
        last = field(0.0f);
    }

    /** What it gives for the bar at [x]: the field's rate of change, per unit swing at rest. */
    float hear(float x) {
        // Through the air, as the bar moves: its overtones as loud as they
        // swing. (As a rate of change, 17 times the note's came 25 dB up and
        // a toy piano clipped.)
        if (kind == Linear) return x;
        const float f = field(x * inv);
        const float out = (f - last) * perNote / slope;
        last = f;
        return out;
    }

    /** Starts it over with the bar at [x] (a bar struck again is still swinging). */
    void reset(float x = 0.0f) { last = field(x * inv); }

  private:
    float field(float u) const {
        const float v = u + offset;
        if (kind == Magnetic) return 1.0f / (1.0f + v * v);
        if (kind == Linear) return u;
        return 1.0f / (1.0f - 0.6f * std::tanh(v));
    }

    /** A mf blow's swing, in the bar's units (Hammer's kSwing at 1 m/s). */
    static constexpr float kRefSwing = 0.2f;
    Kind kind = Magnetic;
    float inv = 1.0f, offset = 0.3f, perNote = 1.0f, slope = 1.0f, last = 0.0f;
};

/**
 * What the electric pianos go through: the preamp's tone, an amp that
 * rounds off as it's driven (against the level the pianos play at, so a
 * little drive is a little warmth at any volume), and a tremolo, which on
 * a tine piano pans side to side.
 */
class Amp {
  public:
    /** The tone of each: tine (a lift at the top, a dip in the low middle), reed (a small speaker), tangent. */
    enum Voicing { Tine, Reed, Tangent };

    void prepare(float sampleRate) {
        sr = sampleRate;
        voice(Tine);
        clear();
    }

    void clear() {
        for (int i = 0; i < 3; ++i) { eqL[i].reset(); eqR[i].reset(); }
        phase = 0.0f;
    }

    void voice(Voicing v) {
        if (v == voicing && designed) return;
        voicing = v;
        designed = true;
        switch (v) {
        case Tine:
            eqL[0].peak(450.0f, -3.0f, 0.8f, sr);
            eqL[1].highShelf(3000.0f, 3.0f, sr);
            eqL[2].highpass(40.0f, 0.707f, sr);
            break;
        case Reed:
            eqL[0].peak(1200.0f, 3.0f, 0.9f, sr);
            eqL[1].highShelf(6000.0f, -9.0f, sr);
            eqL[2].highpass(90.0f, 0.707f, sr);
            break;
        case Tangent:
            eqL[0].peak(2500.0f, 2.0f, 1.0f, sr);
            eqL[1].highShelf(8000.0f, -6.0f, sr);
            eqL[2].highpass(60.0f, 0.707f, sr);
            break;
        }
        for (int i = 0; i < 3; ++i) eqR[i] = eqL[i];
        for (int i = 0; i < 3; ++i) eqR[i].reset();
    }

    /**
     * [drive] 0 to 1: how far over [nominal], the level a mf note's peaks
     * reach, the amp rounds off (four times it, nothing to speak of, down to
     * a third of it, everything); the
     * tremolo [depth] 0 to 1 at [rateHz] (or at [phaseNow], 0 to 1, when
     * locked to the tempo; -1 for free) and how far it pans, [wide].
     */
    void process(float *L, float *R, int frames, float drive, float nominal, float depth, float rateHz, float phaseNow, float wide) {
        const float clean = 1.0f - drive;
        const float headroom = std::fmax(nominal, 1e-6f) * (0.3f + 4.0f * clean * clean);
        // Quiet notes pass as they are, and a driven amp is played louder.
        const float in = 1.0f / headroom, out = headroom * (1.0f + 0.5f * drive);
        const float step = rateHz / sr;
        if (phaseNow >= 0.0f) phase = phaseNow;
        for (int i = 0; i < frames; ++i) {
            float l = L[i], r = R[i];
            for (int b = 0; b < 3; ++b) {
                l = eqL[b].process(l);
                r = eqR[b].process(r);
            }
            if (drive > 0.0f) {
                l = std::tanh(l * in) * out;
                r = std::tanh(r * in) * out;
            }
            if (depth > 0.0f) {
                const float s = std::sin(6.28318530718f * phase);
                // The same swing on both sides is a tremolo; opposite, a pan.
                const float gl = 1.0f - depth * 0.5f * (1.0f - s * (1.0f - 2.0f * wide));
                const float gr = 1.0f - depth * 0.5f * (1.0f - s);
                l *= gl;
                r *= gr;
                phase += step;
                if (phase >= 1.0f) phase -= 1.0f;
            }
            L[i] = l;
            R[i] = r;
        }
    }

  private:
    float sr = 48000.0f;
    Voicing voicing = Tine;
    bool designed = false;
    dsp::Biquad eqL[3], eqR[3];
    float phase = 0.0f;
};

} // namespace acidulous::machine::hammer
