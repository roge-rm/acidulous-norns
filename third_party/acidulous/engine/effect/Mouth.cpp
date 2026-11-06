#include "Mouth.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Lfo.h>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

using dsp::clampf;
namespace diction = machine::diction;

namespace {
/**
 * What goes in. A vowel keeps only the partials near its formants, so a
 * mixed track comes out quieter than a saw does; this brings a typical part
 * back to about the level it went in.
 */
constexpr float kIn = 0.75f;
/** Samples between moves of the formants, as Diction moves them. */
constexpr int32_t kStep = 16;
enum MoveBy { ByLfo = 0, ByLevel, ByKey };
/** A word's steps glide over this long, seconds, as speech moves. */
constexpr float kSayGlide = 0.02f;
/** How long the mouth takes to go back to the move once a word is over, seconds. */
constexpr float kBackGlide = 0.04f;
/** How fast the mouth closes and opens on a consonant, seconds. */
constexpr float kOpenTime = 0.004f;
/** A word's hiss against the track's level: an s comes out at about a quarter of it. */
constexpr float kHiss = 2.0f;
/** Samples the hiss's bands ring on once it stops. */
constexpr int32_t kHissRing = 2048;
/** The rms of noise from -1 to 1. */
const float kNoiseRms = 1.0f / std::sqrt(3.0f);
/** The nose a nasal opens, at the least. */
constexpr float kSayNasal = 0.8f;

/** How far the mouth stays open on [s]: shut on a stop, nearly on a hiss, open on a vowel. */
float openOf(const diction::Words::Step &s) {
    switch (s.kind) {
    case diction::Kind::Vowel:
    case diction::Kind::Diphthong: return 1.0f;
    case diction::Kind::Glide: return 0.85f;
    case diction::Kind::Liquid: return 0.75f;
    case diction::Kind::Flap: return 0.6f;
    case diction::Kind::Aspirate: return 0.5f;
    case diction::Kind::Nasal: return 0.45f;
    case diction::Kind::Fricative: return s.voiced ? 0.45f : 0.2f;
    case diction::Kind::Affricate: return s.voiced ? 0.35f : 0.15f;
    case diction::Kind::Stop: return s.voiced ? 0.2f : 0.06f;
    }
    return 1.0f;
}
} // namespace

const ParamDef *Mouth::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"vowel", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"size", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"nasal", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"move", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""}, // LFO, level, key
        {"rate", 0.0f, 16.0f, 8.0f, Curve::Stepped, dsp::Lfo::kRates, ""},
        {"depth", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
        // How far a clip's words move the mouth, against the move.
        {"words", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Mouth::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &t : throat) t.prepare(sr);
    reset();
}

void Mouth::reset() {
    for (auto &t : throat) t.reset();
    follow = 0.0f;
    countdown = 0;
    speech.reset();
    wordNote = -1;
    held = 0;
    for (bool &d : down) d = false;
    letGo = false;
    endLeft = -1;
    sayMix = 0.0f;
    for (int k = 0; k < 3; ++k) said[k] = diction::kKnobVowels[2][k];
    saidNasal = 0.0f;
    openGoal = open = 1.0f;
    hiss = 0.0f;
    level = 0.0f;
    noise = 0x9e3779b9u;
    for (auto &b : hissBand) b.clear();
    hissGain[0] = hissGain[1] = 0.0f;
    hissRinging = 0;
}

void Mouth::colourHiss() {
    const auto &band = speech.now().band;
    // Each band scaled so noise comes out at its share of 1: a band [width]
    // wide carries about pi * width / rate of the noise's power.
    const float total = std::sqrt(band[0][2] * band[0][2] + band[1][2] * band[1][2]);
    for (int k = 0; k < 2; ++k) {
        hissBand[k].set(band[k][0], band[k][1], sr);
        const float width = std::clamp(band[k][1], 1.0f, 0.4f * sr);
        const float carried = kNoiseRms * std::sqrt(3.14159265f * width / sr);
        hissGain[k] = total > 0.0f ? band[k][2] / total / carried : 0.0f;
    }
}

void Mouth::onNote(int32_t note, float) {
    // A note with words says them. One without goes back to the move, unless
    // it's a chord's other note under a word still being said.
    if (speech.hasPending()) {
        speech.plan(sr);
        wordNote = note;
        letGo = false;
        endLeft = -1;
        if (speech.saying) colourHiss();
    } else if (held == 0) {
        speech.saying = false;
    }
    const int32_t k = note & 0x7f;
    if (!down[k]) {
        down[k] = true;
        ++held;
    }
}

void Mouth::onNoteOff(int32_t note) {
    const int32_t k = note & 0x7f;
    if (!down[k]) return;
    down[k] = false;
    --held;
    if (note == wordNote && speech.saying && !letGo) {
        letGo = true;
        endLeft = -1;
        if (speech.release()) colourHiss();
    }
}

void Mouth::onAllNotesOff() {
    for (bool &d : down) d = false;
    held = 0;
    if (speech.saying && !letGo) {
        letGo = true;
        endLeft = -1;
        if (speech.release()) colourHiss();
    }
}

bool Mouth::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float vowel = p.get(Vowel), size = p.get(Size), nasal = p.get(Nasal), depth = p.get(Depth), mix = p.get(Mix);
    const int move = std::clamp(static_cast<int>(p.get(Move) + 0.5f), 0, 2);
    const float beats = dsp::Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    const float inc = dsp::Lfo::phaseInc(beats, bpm, sr);
    float phase = slicePhase(dsp::Lfo::phaseAt(tick, beats), inc);
    const float followCoef = dsp::onePoleCoeff(0.03f, sr);
    const float words = clampf(p.get(Words), 0.0f, 1.0f);
    if (speech.advance(frames)) colourHiss();
    if (letGo && speech.saying) {
        // Let go before the vowel was reached: its end follows it.
        if (speech.at == speech.held && speech.release()) colourHiss();
        // The last sound plays out its own length, then the word is over.
        if (speech.at == speech.count - 1) {
            if (endLeft < 0) endLeft = std::max(0, speech.now().length);
            endLeft -= frames;
            if (endLeft <= 0) {
                speech.saying = false;
                letGo = false;
            }
        }
    }
    const float sayGlide = 1.0f - std::exp(-static_cast<float>(kStep + 1) / (kSayGlide * sr));
    const float backGlide = 1.0f - std::exp(-static_cast<float>(kStep + 1) / (kBackGlide * sr));
    const float openCoef = dsp::onePoleCoeff(kOpenTime, sr);

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        // The level the mouth follows: this track, or the sidechain's.
        const float heard = move == ByKey && key_ != nullptr ? std::fabs(key_[i]) : std::max(std::fabs(inL), std::fabs(inR));
        follow += (heard - follow) * followCoef;
        level += (std::max(std::fabs(inL), std::fabs(inR)) - level) * followCoef;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        if (countdown-- <= 0) {
            countdown = kStep;
            float by = 0.0f;
            if (move == ByLfo) by = 0.5f - 0.5f * std::cos(phase * 6.2831853f);
            else by = clampf(follow * 4.0f, 0.0f, 1.0f); // a quarter of full scale opens it fully
            vowelNow = clampf(vowel + by * depth, 0.0f, 1.0f);
            // Between two of the five vowels, as Diction's vowel control does.
            const float at = vowelNow * 4.0f;
            const int lower = std::min(3, static_cast<int>(at));
            const float t = at - static_cast<float>(lower);
            float f[3];
            for (int k = 0; k < 3; ++k) {
                f[k] = diction::kKnobVowels[lower][k] + (diction::kKnobVowels[lower + 1][k] - diction::kKnobVowels[lower][k]) * t;
            }
            const float ratio = std::pow(2.0f, (0.5f - size) * 0.8f);
            // A note's words take the mouth where they say, as far as the
            // words knob goes, and let it go again when they stop.
            float nose = nasal;
            const bool say = speech.saying && words > 0.0f;
            if (say || sayMix > 0.0f) {
                const diction::Words::Step &s = speech.now();
                if (say) {
                    if (sayMix == 0.0f) {
                        for (int k = 0; k < 3; ++k) said[k] = s.f[k];
                        saidNasal = nasal;
                    }
                    for (int k = 0; k < 3; ++k) said[k] += (s.f[k] - said[k]) * sayGlide;
                    const float noseGoal = s.kind == diction::Kind::Nasal ? std::max(nasal, kSayNasal) : nasal;
                    saidNasal += (noseGoal - saidNasal) * sayGlide;
                }
                sayMix += ((say ? words : 0.0f) - sayMix) * (say ? sayGlide : backGlide);
                if (!say && sayMix < 1e-4f) sayMix = 0.0f;
                for (int k = 0; k < 3; ++k) f[k] += (said[k] - f[k]) * sayMix;
                nose += (saidNasal - nose) * sayMix;
                openGoal = 1.0f + ((say ? openOf(s) : 1.0f) - 1.0f) * sayMix;
                hiss = say ? s.noise * sayMix * kHiss : 0.0f;
            }
            const diction::Shape shape = diction::shapeOf(f, nose, ratio, diction::Throat::kReferenceHz);
            for (auto &th : throat) {
                th.setShape(shape);
                th.setRing(diction::ringFor(f, shape));
            }
        }
        open += (openGoal - open) * openCoef;
        float h = 0.0f;
        if (hiss > 0.0f || hissRinging > 0) {
            float n = 0.0f;
            if (hiss > 0.0f) {
                noise = noise * 1664525u + 1013904223u;
                n = (static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f) * hiss * level;
                hissRinging = kHissRing;
            } else if (--hissRinging == 0) {
                // Rung out: a band left holding an old state would kick when it's started again.
                for (auto &b : hissBand) b.clear();
            }
            if (hissRinging > 0) h = hissGain[0] * hissBand[0].process(n) + hissGain[1] * hissBand[1].process(n);
        }
        float outL = throat[0].process(inL * kIn * open, 0.0f, 0.0f) + h;
        float outR = stereoIn ? throat[1].process(inR * kIn * open, 0.0f, 0.0f) + h : outL;
        if (!std::isfinite(outL)) outL = 0.0f;
        if (!std::isfinite(outR)) outR = 0.0f;
        L[i] = inL + (outL - inL) * mix;
        if (stereoIn) R[i] = inR + (outR - inR) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
