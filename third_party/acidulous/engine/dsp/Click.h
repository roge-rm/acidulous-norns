#pragma once
#include "Math.h"
#include <cmath>
#include <cstdint>

// The metronome sound. There are three voices, since a sine blip is easy to
// hear over a synth but gets lost under drums:
//
//   Blip    - a decaying sine, higher on the downbeat.
//   Stick   - short filtered noise. Stays audible in a busy mix without
//             being loud.
//   Cowbell - two detuned squares. For when the drums are loud.
//
// Three accent levels (bar, beat and subdivision) so you can still hear where
// the beat is when it ticks sixteenths.

namespace acidulous::dsp {

class Click {
  public:
    enum Voice : int32_t { Blip = 0, Stick = 1, Cowbell = 2 };
    /** Bar, beat, and everything between: three levels, loudest first. */
    enum Accent : int32_t { Bar = 0, Beat = 1, Division = 2 };

    /** The loudest a click can be, for anything that needs to leave headroom. */
    static constexpr float kPeak = 0.6f;
    static float peakFor(float volume) { return volume * kPeak; }

    void prepare(int32_t sampleRate) {
        this->sampleRate = static_cast<float>(sampleRate);
        decayCoeff = onePoleCoeff(0.025f, this->sampleRate);
        shortCoeff = onePoleCoeff(0.008f, this->sampleRate);
        clear();
    }

    void setVoice(int32_t v) { voice = v < 0 ? 0 : (v > Cowbell ? Cowbell : v); }

    void clear() {
        queued = 0;
        env = 0.0f;
        for (auto &p : phase) p = 0.0f;
        noiseA = noiseB = 0.0f;
    }

    /**
     * Sounds a click [offset] samples from the start of the next process().
     * An offset past the end of the block is carried over to the next one,
     * and several clicks can be queued at once.
     */
    void trigger(int32_t accent, int32_t offset) {
        if (queued >= kMaxQueued) {
            return; // more than four in 64 frames isn't a real tempo
        }
        pending[queued].offset = offset < 0 ? 0 : offset;
        pending[queued].accent = accent < Bar ? Bar : (accent > Division ? Division : accent);
        ++queued;
    }

    void process(float *L, float *R, int32_t frames, float volume) {
        for (int32_t i = 0; i < frames; ++i) {
            for (int32_t q = 0; q < queued; ++q) {
                if (pending[q].offset == i) {
                    strike(pending[q].accent);
                }
            }
            if (env > 1e-4f) {
                const float s = sample() * env * volume * kPeak;
                env -= env * (voice == Blip ? decayCoeff : shortCoeff);
                L[i] += s;
                R[i] += s;
            }
        }
        // Carry anything that didn't fit over to the next block.
        int32_t kept = 0;
        for (int32_t q = 0; q < queued; ++q) {
            if (pending[q].offset >= frames) {
                pending[kept].offset = pending[q].offset - frames;
                pending[kept].accent = pending[q].accent;
                ++kept;
            }
        }
        queued = kept;
    }

  private:
    static constexpr int32_t kMaxQueued = 4;

    struct Pending {
        int32_t offset = 0;
        int32_t accent = Bar;
    };

    void strike(int32_t accent) {
        // Full level on the bar, less on a beat, quieter on a subdivision.
        env = accent == Bar ? 1.0f : (accent == Beat ? 0.7f : 0.35f);
        phase[0] = phase[1] = 0.0f;
        switch (voice) {
        case Stick:
            // Noise through a narrow band. The accent moves the band as well
            // as the level.
            bandHz = accent == Bar ? 2600.0f : (accent == Beat ? 2000.0f : 1700.0f);
            break;
        case Cowbell:
            // Two squares, a slightly flat fifth apart.
            inc[0] = (accent == Bar ? 840.0f : 620.0f) / sampleRate;
            inc[1] = inc[0] * 1.4983f;
            break;
        default:
            inc[0] = (accent == Bar ? 2000.0f : (accent == Beat ? 1400.0f : 1050.0f)) / sampleRate;
            break;
        }
    }

    float sample() {
        switch (voice) {
        case Stick: {
            rng = rng * 1664525u + 1013904223u;
            const float white = static_cast<float>(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
            // A band-pass, as a hand-written two-pole state variable filter.
            const float f = 2.0f * std::sin(kPi * bandHz / sampleRate);
            noiseA += f * noiseB;
            noiseB += f * (white - noiseA - 0.6f * noiseB);
            return noiseB * 0.8f;
        }
        case Cowbell: {
            float v = 0.0f;
            for (int32_t o = 0; o < 2; ++o) {
                v += (phase[o] < 0.5f ? 1.0f : -1.0f) * 0.35f;
                phase[o] += inc[o];
                if (phase[o] >= 1.0f) phase[o] -= 1.0f;
            }
            return v;
        }
        default: {
            const float v = std::sin(phase[0] * kTwoPi);
            phase[0] += inc[0];
            if (phase[0] >= 1.0f) phase[0] -= 1.0f;
            return v;
        }
        }
    }

    float sampleRate = 48000.0f;
    float decayCoeff = 0.001f, shortCoeff = 0.005f;
    float env = 0.0f;
    float phase[2] = {0.0f, 0.0f};
    float inc[2] = {0.0f, 0.0f};
    float bandHz = 2000.0f;
    float noiseA = 0.0f, noiseB = 0.0f;
    uint32_t rng = 0x9f1c2b3du;
    int32_t voice = Blip;
    Pending pending[kMaxQueued];
    int32_t queued = 0;
};

} // namespace acidulous::dsp
