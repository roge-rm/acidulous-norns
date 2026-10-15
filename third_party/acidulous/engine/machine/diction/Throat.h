#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Diction's throat: the folds' pulses, air, and hiss, through formants that
// move as the singer moves from one sound to the next.
//
// A vowel is made rather than played back, so a word can travel between its
// sounds: a B heads the formants low and a D high, and the ear hears which
// from the way the vowel arrives, not from the consonant itself.
namespace acidulous::machine::diction {

constexpr int32_t kFormants = 5;

/** Where the formants are: centres and widths in hertz, and how far the nose is open, 0 to 1. */
struct Shape {
    float f[kFormants];
    float bw[kFormants];
    float nasal;
};

/** A two-pole resonance with unity gain at DC, so five in a row keep a vowel's natural balance. */
struct Resonator {
    float a = 1, b = 0, c = 0, y1 = 0, y2 = 0;
    void set(float hz, float bw, float sr);
    float process(float x) {
        const float y = a * x + b * y1 + c * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
    void clear() { y1 = y2 = 0; }
};

/** A resonance turned inside out: a notch, for the nose's zero. */
struct AntiResonator {
    float a = 1, b = 0, c = 0, x1 = 0, x2 = 0;
    void set(float hz, float bw, float sr);
    float process(float x) {
        const float y = a * x + b * x1 + c * x2;
        x2 = x1;
        x1 = x;
        return y;
    }
    void clear() { x1 = x2 = 0; }
};

/** A band of hiss: constant peak gain, from the cookbook. */
struct Bandpass {
    float b0 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(float hz, float width, float sr);
    float process(float x) {
        const float y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

/** A second-order high-pass at Q 0.707, for taking the rumble out of the air. */
struct Highpass {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(float hz, float sr);
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

/** A high shelf from the cookbook, slope 1. */
struct Shelf {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(float hz, float db, float sr);
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

class Throat {
  public:
    /** Every vowel is scaled to this level, so what's sung changes the colour and not the loudness. */
    static constexpr float kLevel = 0.13f;
    /** The pitch the levels are worked out at. The folds' pulses are scaled to match at every other. */
    static constexpr float kReferenceHz = 150.0f;
    /** The lowest pitch the levels are right for: the E below the bass clef's. */
    static constexpr float kLowestHz = 40.0f;

    void prepare(float sampleRate);
    void reset();

    /** Point the voice and the air at [shape]. Called every few samples as the formants move. */
    void setShape(const Shape &shape);
    /**
     * Lifts or lowers everything above about 1.5 kHz by [db]. In a chain of
     * formants the lower ones set the level of the higher, so moving the
     * throat down drops its ring as well; this puts the ring back.
     */
    void setRing(float db);
    /** The hiss's two bands: centre, width and share, as in a Phone. */
    void setHiss(const float band[2][3]);

    /**
     * What to scale the voice and the air by for [shape], with the ring at
     * [ringDb], sung at [hz], to sound at kLevel.
     * Worked out from the filters' response rather than by listening, so any
     * shape can be asked for, including one between two vowels.
     */
    void levelsFor(const Shape &shape, float ringDb, float hz, float &voice, float &air) const;

    /**
     * One sample. [glottal] is the folds' pulse, [breath] the air at the
     * folds, [hiss] the air at a narrowing further up; each already scaled.
     */
    float process(float glottal, float breath, float hiss) {
        float v = nasalPole.process(glottal);
        v = nasalZero.process(v);
        for (auto &r : voice) v = r.process(v);
        float a = 0.0f;
        if (breath != 0.0f || airRinging > 0) {
            airRinging = breath != 0.0f ? kRing : airRinging - 1;
            a = airCut2.process(airCut1.process(breath));
            for (auto &r : air) a = r.process(a);
        }
        float h = 0.0f;
        if (hiss != 0.0f || hissRinging > 0) {
            hissRinging = hiss != 0.0f ? kRing : hissRinging - 1;
            h = hiss1.process(hiss) * hissShare[0] + hiss2.process(hiss) * hissShare[1];
        }
        return ring.process(v + a) + h;
    }

    /** One sample of the folds' flow over a period, 0 to 1 through it. */
    float flow(float t) const;

  private:
    /** Samples to keep filtering once a source falls silent, so what it set ringing dies away instead of stopping. */
    static constexpr int32_t kRing = 4096;

    float sampleRate = 48000.0f;
    AntiResonator nasalZero;
    Resonator nasalPole;
    Resonator voice[kFormants];
    Highpass airCut1, airCut2;
    Resonator air[kFormants];
    Bandpass hiss1, hiss2;
    Shelf ring;
    float ringDb = 0.0f;
    float hissShare[2] = {0, 0};
    int32_t airRinging = 0, hissRinging = 0;

    // For levelsFor: the folds' pulse at the reference pitch, harmonic by
    // harmonic, and a grid of frequencies for the air.
    std::vector<float> harmonicPower;
    std::vector<float> gridCos, gridSin, gridCut;
    std::vector<float> flowTable;
};

/** Moves a formant by the formant control: fully below 1.2 kHz, a third as far above 1.8 kHz. */
float shiftFormant(float hz, float ratio);

// Shared with Tongue, whose mouth is this throat.

/** The vowel control's vowels, oo oh ah eh ee: from the back of the mouth to the front. F1 to F3, Hz. */
constexpr float kKnobVowels[5][3] = {
    {300, 870, 2240}, {520, 900, 2400}, {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010},
};
/** The fourth and fifth formants, where a voice gathers its ring. Much the same for every sound. */
constexpr float kHigh[2] = {3350, 3800};
constexpr float kHighWidth[2] = {200, 250};

/** How wide formant [k] is where it is, Hz: wider the higher it sits, as measured in real voices. */
inline float widthOf(int32_t k, float hz) {
    switch (k) {
    case 0: return 50.0f + 0.04f * hz;
    case 1: return 75.0f + 0.015f * hz;
    default: return 30.0f + 0.045f * hz;
    }
}

// Also shared with Nexus's throat module.

/** How much of the ring a moved throat loses is put back. All of it overshoots: the formants' skirts overlap. */
inline constexpr float kRingBack = 0.75f;

/**
 * The shape the throat takes for [f], with the formant control's move, sung
 * at [hz]. A note above the first formant would miss it and go thin, so the
 * first formant rises to stay above the note, as a soprano opens her mouth
 * wider on a high note, and the formants widen a little with the pitch.
 */
inline Shape shapeOf(const float f[3], float nasal, float ratio, float hz) {
    Shape s{};
    for (int32_t k = 0; k < kFormants; ++k) {
        const float at = k < 3 ? f[k] : kHigh[k - 3];
        const float bw = k < 3 ? widthOf(k, at) : kHighWidth[k - 3];
        const float moved = shiftFormant(at, ratio);
        s.f[k] = moved;
        s.bw[k] = std::max(bw * moved / at, k < 2 ? 0.3f * hz : 0.0f);
    }
    s.f[0] = std::max(s.f[0], 1.15f * hz);
    s.f[1] = std::max(s.f[1], s.f[0] + 200.0f);
    s.nasal = nasal;
    return s;
}

/**
 * Each of the lower three formants sets the level above it by about the
 * square of how far it moved, so a moved throat's ring is put back by that
 * much, in dB.
 */
inline float ringFor(const float f[3], const Shape &moved) {
    float db = 0.0f;
    for (int32_t k = 0; k < 3; ++k) db -= 40.0f * std::log10(moved.f[k] / f[k]);
    return kRingBack * db;
}

} // namespace acidulous::machine::diction
