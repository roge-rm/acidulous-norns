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
}

bool Mouth::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float vowel = p.get(Vowel), size = p.get(Size), nasal = p.get(Nasal), depth = p.get(Depth), mix = p.get(Mix);
    const int move = std::clamp(static_cast<int>(p.get(Move) + 0.5f), 0, 2);
    const float beats = dsp::Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    float phase = dsp::Lfo::phaseAt(tick, beats);
    const float inc = dsp::Lfo::phaseInc(beats, bpm, sr);
    const float followCoef = dsp::onePoleCoeff(0.03f, sr);

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        // The level the mouth follows: this track, or the sidechain's.
        const float heard = move == ByKey && key_ != nullptr ? std::fabs(key_[i]) : std::max(std::fabs(inL), std::fabs(inR));
        follow += (heard - follow) * followCoef;
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
            const diction::Shape shape = diction::shapeOf(f, nasal, ratio, diction::Throat::kReferenceHz);
            for (auto &th : throat) {
                th.setShape(shape);
                th.setRing(diction::ringFor(f, shape));
            }
        }
        float outL = throat[0].process(inL * kIn, 0.0f, 0.0f);
        float outR = stereoIn ? throat[1].process(inR * kIn, 0.0f, 0.0f) : outL;
        if (!std::isfinite(outL)) outL = 0.0f;
        if (!std::isfinite(outR)) outR = 0.0f;
        L[i] = inL + (outL - inL) * mix;
        if (stereoIn) R[i] = inR + (outR - inR) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
