#include "Horn.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

using dsp::clampf;

namespace {
enum Kinds { Brass = 0, Clarinet, Oboe, Flute };
/** Samples between set-ups of the instrument, as the Nexus modules do: tuning is expensive. */
constexpr int32_t kRetune = 32;
/** Samples between looks for the pitch, each a quarter of a whole look. */
constexpr int32_t kLook = 64;
/** How sure the follower must be before the instrument changes note. */
constexpr float kSure = 0.6f;
/** The lips speak a little sharp of the tube they're set to, about 35 cents; the tube is set that much flatter. */
constexpr float kBrassTrim = 0.9800f; // 2^(-35/1200)
/** The instrument's level against the track's: a horn played full is far louder than the voice that leads it. */
constexpr float kOut[4] = {3.0f, 3.5f, 3.2f, 1.8f};

float noteHz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }
} // namespace

const ParamDef *Horn::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"kind", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // brass, clarinet, oboe, flute
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"tone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bell", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"air", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        {"glide", 1.0f, 500.0f, 30.0f, Curve::Exponential, 0, "ms"},
        {"breath", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"snap", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Horn::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    bore.prepare(sr);
    pipe.prepare(sr);
    follow.prepare(sr);
    reset();
}

void Horn::reset() {
    bore.clear();
    pipe.clear();
    follow.reset();
    note = target = 60.0f;
    push = 0.0f;
    heard = blowing = false;
    countdown = looking = 0;
    kind = -1;
    rng = 0x6d2b79f5u;
}

bool Horn::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int k = std::clamp(static_cast<int>(p.get(Kind) + 0.5f), 0, 3);
    if (k != kind) {
        // A new instrument starts from rest.
        kind = k;
        bore.clear();
        pipe.clear();
        blowing = false;
        countdown = 0;
    }
    const float octave = std::round(p.get(Octave));
    const float tone = p.get(Tone), bell = p.get(Bell), air = p.get(Air), mix = p.get(Mix);
    const bool snap = p.get(Snap) > 0.5f;
    const float glide = dsp::onePoleCoeff(p.get(Glide) * 0.001f, sr);
    // How hard a level blows: at the middle, a track at a quarter of full scale blows full.
    const float sense = 1.0f * std::exp2(p.get(Breath) * 4.0f);
    const float rise = dsp::onePoleCoeff(0.004f, sr), fall = dsp::onePoleCoeff(0.04f, sr);
    const float most = k == Brass ? 0.6f : 0.85f;

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        const float mono = 0.5f * (inL + inR);
        follow.push(&mono, 1);
        if (looking-- <= 0) {
            looking = kLook;
            follow.update(0.25f);
            const float hz = follow.pitch();
            if (follow.sureness() >= kSure && hz > 0.0f) {
                float n = 69.0f + 12.0f * std::log2(hz / 440.0f) + 12.0f * octave;
                if (snap) n = std::round(n);
                target = n;
                // The first note found, it starts there rather than gliding in from wherever.
                if (!heard) note = n;
                heard = true;
            }
        }
        note += (target - note) * glide;
        // The breath follows the track's level, quickly up and more slowly down.
        const float level = std::max(std::fabs(inL), std::fabs(inR));
        push += (std::min(1.0f, level * sense) - push) * (level * sense > push ? rise : fall);
        const float blow = heard ? push * most : 0.0f;

        if (countdown-- <= 0) {
            countdown = kRetune;
            const float hz = noteHz(clampf(note, 12.0f, 115.0f));
            const bool now = blow > 0.02f;
            if (k == Brass) {
                // As the Nexus horn sets Brazen's, tone standing for its bite;
                // the lips at their default tension, where the horn plays in tune.
                const float tension = 0.988f;
                bore.setFrequency(hz * kBrassTrim);
                bore.setLips(tension, 0.6f);
                bore.setLipGain(0.7f + tone * 0.6f);
                bore.setBell(0.966f, bell * 0.48f);
                bore.setRest(0.35f);
                bore.setBite(tone * 0.9f + 0.25f);
                bore.setBrass(0.45f * std::min(1.0f, blow * 1.4f));
                bore.setLoss(0.999f);
                bore.setPressure(blow);
                bore.tune();
                if (now && !blowing) bore.tongue();
            } else {
                const int reed = k - Clarinet; // single, double, jet
                pipe.setNote(hz);
                pipe.setShape(reed == 0, 1);
                pipe.setTube(hz * 0.5f);
                pipe.setLattice(1500.0f, 0.0f, 0.4f);
                pipe.setBelow(1.0f);
                pipe.setFork(0.2f);
                pipe.setReed(reed, 0.05f + tone * 0.95f, 0.5f);
                pipe.setJet(0.5f * std::exp2((tone - 0.47f) * 2.4f), 0.3f);
                pipe.setBell(0.9f, 0.05f + bell * 0.85f);
                pipe.setLoss(0.999f);
                pipe.setPressure(blow);
                pipe.setDrive(1.0f);
                pipe.tune();
                if (now && !blowing) pipe.lift();
            }
            blowing = now;
        }
        const float noise = random() * air * (k == Brass ? 0.25f : 0.35f) * blow;
        float out = (k == Brass ? bore.step(blow, noise) : pipe.step(blow, noise)) * kOut[k];
        if (!std::isfinite(out)) {
            out = 0.0f;
            bore.clear();
            pipe.clear();
        }
        L[i] = inL + (out - inL) * mix;
        if (stereoIn) R[i] = inR + (out - inR) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
