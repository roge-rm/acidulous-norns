#include "Resonance.h"
#include <engine/machine/Voices.h>

#include <engine/core/Settings.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

namespace {
constexpr float kTwoPi = 6.28318530718f;

namespace {

/**
 * How many partials to build for the patch's `modes` setting. Lean mode
 * halves it, with a floor of four so the sound thins out but stays the same
 * instrument.
 */
int32_t modesFor(float asked) {
    const int32_t want = std::clamp(static_cast<int32_t>(asked + 0.5f), 1, Resonance::kMaxModes);
    return fullQuality() ? want : std::max(4, want / 2);
}

} // namespace

/**
 * The frequencies each shape rings at, as ratios of its lowest mode. These are
 * the real physical values: Bessel zeros for a drumhead, the free bar
 * equation for a bar, measured bell partials for a bowl.
 */
const float kRatios[Resonance::ShapeCount][Resonance::kMaxModes] = {
    // Membrane: a circular drumhead.
    {1.000f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f, 3.600f, 3.652f, 4.060f, 4.154f,
     4.230f, 4.378f, 4.832f, 5.000f, 5.412f, 5.550f, 5.892f, 6.155f, 6.500f, 6.780f, 7.100f, 7.500f},
    // Bar, free at both ends, like a marimba or glockenspiel.
    {1.000f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.82f, 31.87f, 39.80f, 48.60f, 58.28f, 68.83f,
     80.25f, 92.55f, 105.7f, 119.7f, 134.6f, 150.3f, 166.9f, 184.3f, 202.6f, 221.7f, 241.7f, 262.5f},
    // Plate: dense, close-packed modes.
    {1.000f, 2.040f, 3.010f, 3.660f, 4.580f, 5.540f, 6.120f, 7.020f, 7.940f, 8.480f, 9.320f, 10.15f,
     10.90f, 11.72f, 12.48f, 13.31f, 14.09f, 14.92f, 15.70f, 16.53f, 17.31f, 18.14f, 18.92f, 19.75f},
    // Tube, open: the harmonic series, so the only clearly pitched shape.
    {1.000f, 2.000f, 3.000f, 4.000f, 5.000f, 6.000f, 7.000f, 8.000f, 9.000f, 10.00f, 11.00f, 12.00f,
     13.00f, 14.00f, 15.00f, 16.00f, 17.00f, 18.00f, 19.00f, 20.00f, 21.00f, 22.00f, 23.00f, 24.00f},
    // Bowl: a bell.
    {1.000f, 2.000f, 3.010f, 4.170f, 5.430f, 6.790f, 8.210f, 9.700f, 11.24f, 12.83f, 14.47f, 16.15f,
     17.87f, 19.63f, 21.42f, 23.25f, 25.11f, 27.00f, 28.92f, 30.87f, 32.85f, 34.86f, 36.89f, 38.95f},
    // Metal: square roots, inharmonic on purpose.
    {1.000f, 1.414f, 1.732f, 2.236f, 2.646f, 3.162f, 3.606f, 4.123f, 4.583f, 5.099f, 5.568f, 6.083f,
     6.557f, 7.071f, 7.550f, 8.062f, 8.544f, 9.055f, 9.539f, 10.05f, 10.54f, 11.05f, 11.53f, 12.04f},
};
} // namespace

Resonance::Resonance() { initParams(); }

const ParamDef *Resonance::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static bool built = false;
    static char names[kPads * PadParamCount][16];
    if (!built) {
        // Eight identical pads, named like Forage's.
        struct Spec { const char *name; float min, max, def; Curve curve; int32_t steps; const char *unit; };
        static const Spec kPad[PadParamCount] = {
            {"kind", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
            {"tune", 30.0f, 2000.0f, 120.0f, Curve::Exponential, 0, "Hz"},
            {"decay", 0.02f, 8.0f, 0.6f, Curve::Exponential, 0, "s"},
            {"damp", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
            {"inharm", -0.15f, 0.35f, 0.0f, Curve::Linear, 0, ""},
            {"hit", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
            {"hard", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
            {"noise", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
            {"bend", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, ""},
            {"bendtime", 0.002f, 0.4f, 0.03f, Curve::Exponential, 0, "s"},
            {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
            {"level", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
            {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
            {"couple", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        };
        for (int32_t pad = 0; pad < kPads; ++pad) {
            for (int32_t i = 0; i < PadParamCount; ++i) {
                const int32_t at = pad * PadParamCount + i;
                std::snprintf(names[at], sizeof(names[at]), "p%02d_%s", pad, kPad[i].name);
                defs[at] = {names[at], kPad[i].min, kPad[i].max, kPad[i].def, kPad[i].curve, kPad[i].steps, kPad[i].unit};
            }
        }
        defs[Modes] = {"modes", 4.0f, 24.0f, 12.0f, Curve::Stepped, 6, ""};
        defs[Coupling] = {"coupling", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""};
        defs[Humanise] = {"humanise", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""};
        defs[Accent] = {"accent", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""};
        defs[Volume] = {"volume", 0.0f, 1.5f, 0.5f, Curve::Linear, 0, ""}; // Init on the house line
        defs[MasterPan] = {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        built = true;
    }
    count = Count;
    return defs;
}

void Resonance::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    reset();
}

void Resonance::reset() {
    // Reset whole pads. A default Pad has builtTune -1, which forces a
    // rebuild.
    for (auto &p : pads) p = Pad();
    knockBus = ringBus = 0.0f;
    rng = kRngSeed;
}

void Resonance::allNotesOff() {
    // Notes don't stop the ringing, but a panic does.
    for (auto &p : pads) {
        for (auto &m : p.modes) m.clear();
        p.exciteLeft = 0.0f;
        p.ringing = false;
        p.last = 0.0f;
        p.coupleLp = 0.0f;
    }
    knockBus = ringBus = 0.0f;
}

/**
 * Build one pad: its mode frequencies, each mode's level given where it was
 * hit, and each mode's decay.
 */
void Resonance::buildPad(int32_t pad) {
    Pad &p = pads[pad];
    const int32_t kind = std::clamp(padStep(pad, Kind), 0, ShapeCount - 1);
    const float tune = padParam(pad, Tune);
    const float decay = padParam(pad, Decay);
    const float damp = padParam(pad, Damp);
    const float inharm = padParam(pad, Inharm);
    const float hit = hitOf(pad);
    const int32_t want = modesFor(params_.get(Modes));

    p.modeCount = want;
    p.builtKind = kind;
    p.builtTune = tune;
    p.builtDecay = decay;
    p.builtDamp = damp;
    p.builtInharm = inharm;
    p.builtHit = hit;
    p.builtModes = want;

    // Keeps a pad's overall level from depending on its decay. A resonator
    // builds towards b0/(1-r), so long decays are much louder. The square
    // root is used because a strike is short compared to low modes and long
    // compared to high ones, and full correction overshoots.
    //
    // It's worked out once per pad from the pad's decay, not per mode.
    // Correcting each mode by its own shorter decay would boost the
    // overtones into a hiss.
    static const float kRefDecay = 0.5f;
    const float rRef = std::exp(-6.9078f / (kRefDecay * sr));
    const float r0 = std::exp(-6.9078f / (std::max(0.01f, decay) * sr));
    const float decayTrim = std::sqrt((1.0f - r0) / (1.0f - rRef));
    // The same number keeps the coupling loop stable at every decay. A
    // resonator's gain is about 1/sqrt(1-r), so scaling what it accepts by
    // sqrt(1-r) makes the round trip flat. Without it, long decays with high
    // coupling ring forever.
    p.couplingTrim = std::min(1.0f, decayTrim);

    for (int32_t k = 0; k < want; ++k) {
        // Stretch the partials, like stiffness in a real bar.
        const float ratio = std::pow(kRatios[kind][k], 1.0f + inharm);
        const float hz = std::clamp(tune * ratio, 20.0f, sr * 0.47f);
        // The hit position decides which modes get excited. A mode with a
        // node under the stick isn't struck.
        const float node = std::fabs(std::sin(kTwoPi * 0.5f * (k + 1) * std::clamp(hit, 0.02f, 0.98f)));
        // Higher modes die sooner, more so with more damping.
        const float t60 = std::max(0.01f, decay / (1.0f + damp * 6.0f * (ratio - 1.0f)));
        const float r = std::exp(-6.9078f / (t60 * sr));
        const float w = kTwoPi * hz / sr;
        Mode &m = p.modes[k];
        m.a1 = 2.0f * r * std::cos(w);
        m.a2 = -r * r;
        // Normalised so a mode's peak doesn't depend much on its frequency,
        // then rolled off up the series. It uses the square root of sin(w)
        // because a strike is impulsive for low modes and sustained for high
        // ones, and the full sin(w) makes the high modes far too loud.
        m.b0Base = std::sqrt(std::sin(w)) / std::pow(static_cast<float>(k + 1), 0.7f) * decayTrim;
        m.b0 = m.b0Base * node;
    }
    for (int32_t k = want; k < kMaxModes; ++k) p.modes[k].clear();
}

/**
 * The same pad, struck somewhere else. Only the node gains depend on the hit
 * position, so this updates just those (one sine per mode) instead of
 * rebuilding the whole pad.
 */
void Resonance::restrike(int32_t pad) {
    Pad &p = pads[pad];
    const float hit = hitOf(pad);
    p.builtHit = hit;
    for (int32_t k = 0; k < p.modeCount; ++k) {
        const float node =
            std::fabs(std::sin(kTwoPi * 0.5f * (k + 1) * std::clamp(hit, 0.02f, 0.98f)));
        p.modes[k].b0 = p.modes[k].b0Base * node;
    }
}

void Resonance::noteOn(uint8_t note, uint8_t velocity) {
    const int32_t pad = note - kBaseNote;
    if (pad < 0 || pad >= kPads) return;
    Pad &p = pads[pad];

    const float accent = params_.get(Accent);
    const float vel = static_cast<float>(velocity) / 127.0f;
    p.velocity = velocityGain(vel, accent);

    // Humanise moves the strike position a little each hit, so repeated hits
    // don't sound identical.
    const float humanise = params_.get(Humanise);
    const float wobble = humanise > 0.001f ? noise() * humanise * 0.12f : 0.0f;
    p.hit = std::clamp(padParam(pad, Hit) + wobble, 0.0f, 1.0f);

    const float hard = padParam(pad, Hard);
    // A hard stick gives a short, bright hit, a soft one a longer, duller one.
    const float length = (0.4f + (1.0f - hard) * 6.0f) * 0.001f * sr;
    p.exciteLeft = length;
    p.exciteStep = 1.0f / length;
    p.exciteGain = p.velocity * (0.6f + hard * 0.8f);
    p.bendDepth = padParam(pad, Bend);
    p.bendLeft = p.bendDepth > 0.01f ? 1.0f : 0.0f;
    p.bendCoeff = 1.0f - std::exp(-1.0f / (std::max(0.002f, padParam(pad, BendTime)) * sr));
    p.ringing = true;
}

namespace {
/**
 * Output trim for the modal bank, which is otherwise far above full scale.
 * The peak comes from the strike transient and is about the same for every
 * mode count and decay, so one measured constant covers it. It also acts as
 * the house level, setting where kits sit on the volume knob.
 *
 * It's applied after the coupling bus, because the bus goes through a tanh
 * and scaling before it would change how hard the pads drive each other.
 */
constexpr float kOutputTrim = 1.0f / 26.0f; // -28.3 dB
// One pole at about 1 kHz at 48 kHz, for the coupling bus.
constexpr float kBusPole = 0.125f;
// How much of a pad's ring reaches the bus. Kept low, which also keeps the
// A to B to A loop below unity.
constexpr float kRingToBus = 0.06f;
// How much of a pad's knock reaches the bus. Nearly all of it, since the
// strike is what sets neighbours ringing, and it can't form a loop.
constexpr float kKnockToBus = 0.9f;
} // namespace

bool Resonance::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;

    const float coupling = params_.get(Coupling);
    const float volume = params_.get(Volume);
    const float masterPan = params_.get(MasterPan);
    // Half the partials in lean mode, see `modesFor`. The ones dropped are the
    // quietest, since the series rolls off.
    const int32_t wantModes = modesFor(params_.get(Modes));

    // If no pad is ringing and the buses are empty there's nothing to do.
    // (With coupling on, a silent pad still has to run so its neighbours can
    // set it going, but only while something is ringing.)
    bool anyRinging = false;
    for (int32_t pad = 0; pad < kPads && !anyRinging; ++pad) anyRinging = pads[pad].ringing;
    if (!anyRinging && std::fabs(knockBus) < 1e-6f && std::fabs(ringBus) < 1e-6f) {
        knockBus = ringBus = 0.0f;
        return true; // L and R were cleared above
    }

    for (int32_t pad = 0; pad < kPads; ++pad) {
        Pad &p = pads[pad];
        // Rebuild the pad only when its knobs change, and just restrike it
        // when only the hit position moved (every note).
        if (p.builtKind != padStep(pad, Kind) || p.builtTune != padParam(pad, Tune) ||
            p.builtDecay != padParam(pad, Decay) || p.builtDamp != padParam(pad, Damp) ||
            p.builtInharm != padParam(pad, Inharm) || p.builtModes != wantModes) {
            buildPad(pad);
        } else if (p.builtHit != hitOf(pad)) {
            restrike(pad);
        }
    }

    for (int32_t i = 0; i < frames; ++i) {
        float mixL = 0.0f, mixR = 0.0f;
        const float knockIn = knockBus, ringIn = ringBus;
        float knockOut = 0.0f, ringOut = 0.0f;
        // A silent pad still runs while there's energy on the bus, so a
        // neighbour can set it going. It's skipped when the bus level times
        // its coupling is far below audible.
        const float busLevel = std::fabs(knockIn) + std::fabs(ringIn);

        for (int32_t pad = 0; pad < kPads; ++pad) {
            Pad &p = pads[pad];
            const float couple = padParam(pad, Couple) * coupling;
            if (!p.ringing && couple * busLevel < 1e-5f) continue;

            // The strike: a burst between a click and a puff of noise, shaped
            // by the beater's hardness.
            float x = 0.0f;
            if (p.exciteLeft > 0.0f) {
                const float shape = p.exciteLeft * p.exciteStep; // 1 -> 0
                const float noiseAmount = padParam(pad, Noise);
                const float click = shape * shape;
                x = (click * (1.0f - noiseAmount) + noise() * shape * noiseAmount) * p.exciteGain;
                p.exciteLeft -= 1.0f;
            }
            const float strike = x;
            // Coupling from the rest of the kit. Each pad takes its share of
            // the bus minus exactly what it put in (kRingToBus of its output,
            // and its own strike), so it never feeds back into itself. Same
            // as Filament's sympathetic strings.
            //
            // The loss filter is per pad and applied after the subtraction.
            // Filtering the shared bus instead would only cancel the pad's own
            // output at DC and cause feedback elsewhere.
            const float ringShare = (ringIn - p.last * kRingToBus) *
                                    (1.0f / static_cast<float>(kPads - 1)) * couple * 0.5f *
                                    p.couplingTrim;
            const float knockShare = (knockIn - strike * kKnockToBus) *
                                     (1.0f / static_cast<float>(kPads - 1)) * couple * 2.2f;
            p.coupleLp += ((ringShare + knockShare) - p.coupleLp) * kBusPole;
            x += p.coupleLp;

            // Feed the modes the difference x[n] - x[n-2], which puts a zero
            // at DC and at Nyquist. The modes are all poles and the strike is
            // always positive, so otherwise it would ring at DC. The history
            // is per pad since all modes see the same input.
            const float xin = x - p.x2;
            p.x2 = p.x1;
            p.x1 = x;

            float sum = 0.0f;
            for (int32_t k = 0; k < p.modeCount; ++k) sum += p.modes[k].step(xin);

            // Pitch bend on the strike, like a tom's head tightening.
            if (p.bendLeft > 0.0001f) {
                p.bendLeft -= p.bendLeft * p.bendCoeff;
                if (p.bendLeft < 0.0005f) p.bendLeft = 0.0f;
            }

            const float drive = padParam(pad, Drive);
            if (drive > 0.0001f) {
                // Normalised so a nominal signal passes at its own size. The
                // drive is inside the coupling loop, so it must never add
                // gain or the kit could ring forever.
                const float k = 1.0f + drive * 8.0f;
                const float norm = 0.35f / dsp::fastTanh(0.35f * k);
                sum = dsp::fastTanh(sum * k) * norm;
            }
            sum *= padParam(pad, Level);
            p.last = sum;
            // Both the ring and the strike itself go onto the buses. The
            // broadband strike is what excites neighbouring pads.
            ringOut += sum * kRingToBus;
            knockOut += strike * kKnockToBus;

            if (p.ringing && p.exciteLeft <= 0.0f && std::fabs(sum) < 1e-5f) {
                // It's stopped, so skip it until it's hit again.
                bool quiet = true;
                for (int32_t k = 0; k < p.modeCount && quiet; ++k) {
                    if (std::fabs(p.modes[k].y1) > 1e-5f) quiet = false;
                }
                if (quiet) p.ringing = false;
            }

            const float pan = std::clamp(padParam(pad, Pan), -1.0f, 1.0f);
            const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
            mixL += sum * std::cos(angle) * 1.4142f;
            mixR += sum * std::sin(angle) * 1.4142f;
        }

        // The buses, read one sample later. Each pad takes its share above.
        // The tanh on the ring bus is only a last resort to bound a runaway.
        // Keeping the ring's contribution small is what keeps the loop
        // stable.
        ringBus = dsp::fastTanh(ringOut);
        knockBus = knockOut;
        if (!std::isfinite(ringBus)) ringBus = 0.0f;
        if (!std::isfinite(knockBus)) knockBus = 0.0f;

        const float angle = (masterPan + 1.0f) * 0.25f * 3.14159265f;
        L[i] = mixL * volume * std::cos(angle) * 1.4142f * kOutputTrim;
        R[i] = mixR * volume * std::sin(angle) * 1.4142f * kOutputTrim;
    }
    return true;
}

} // namespace acidulous::machine
