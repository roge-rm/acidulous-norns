#include "Manual.h"
#include <engine/machine/Voices.h>
#include <cstdio>
#include <cstring>

namespace acidulous::machine {
using namespace dsp;

namespace {
constexpr float kPi = 3.14159265f;
// Footages, in semitones from the key: 16' 5⅓' 8' 4' 2⅔' 2' 1⅗' 1⅓' 1'
constexpr int kFootage[Manual::kBars] = {-12, 7, 0, 12, 19, 24, 28, 31, 36};
constexpr int kPedalFootage[Manual::kPedalBars] = {-12, 0};
const char *kModelNames[] = {"tonewheel", "transistor", "pipe", "reed"};
const char *kVibNames[] = {"V1", "V2", "V3", "C1", "C2", "C3"};

// The pipe model's stop list: how much of each rank every footage draws. A
// pipe organ can have several ranks at the same pitch (8' is a Diapason, a
// Gamba and a Trumpet together), so each footage mixes ranks:
//
//   16'    Bourdon (flute) with a Bombarde (reed) under it
//   5 1/3' Quint, a flute
//   8'     Diapason, Gamba and Trumpet (principal, string and reed together)
//   4'     Octave and a Flute 4
//   2 2/3' Nazard, a flute
//   2'     Fifteenth, a principal
//   1 3/5' Tierce, a flute
//   1 1/3' Larigot, a principal
//   1'     the mixture
//
// Each rank gets its own bus slot, because two ranks on one wheel are two
// pipes and load the wind separately.
constexpr float kPipeStops[Manual::kBars][5] = {
    // principal, flute, string, reed, mixture
    {0.55f, 1.00f, 0.00f, 0.55f, 0.00f}, // 16'    Bourdon, Bombarde
    {0.00f, 1.00f, 0.00f, 0.00f, 0.00f}, // 5 1/3' Quint
    {1.00f, 1.00f, 1.00f, 1.00f, 0.00f}, // 8'     Diapason, Rohrflote, Gamba, Trumpet
    {1.00f, 1.00f, 0.45f, 0.35f, 0.00f}, // 4'     Octave, Flute, Salicet, Clarion
    {0.00f, 1.00f, 0.00f, 0.00f, 0.00f}, // 2 2/3' Nazard
    {1.00f, 0.55f, 0.30f, 0.00f, 0.00f}, // 2'     Fifteenth, Piccolo
    {0.00f, 1.00f, 0.00f, 0.00f, 0.00f}, // 1 3/5' Tierce
    {0.80f, 0.00f, 0.00f, 0.00f, 0.45f}, // 1 1/3' Larigot
    {0.00f, 0.00f, 0.00f, 0.00f, 1.00f}, // 1'     Mixture
};
} // namespace

Manual::Manual() { initParams(); }

const ParamDef *Manual::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static char names[Count][12];
    static bool built = false;
    if (!built) {
        auto put = [&](int32_t i, const char *name, float mn, float mx, float df, Curve c, int32_t steps,
                       const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), "%s", name);
            defs[i] = ParamDef{names[i], mn, mx, df, c, steps, unit};
        };
        auto lin = [&](int32_t i, const char *n, float mn, float mx, float df, const char *u = "") {
            put(i, n, mn, mx, df, Curve::Linear, 0, u);
        };
        auto exp_ = [&](int32_t i, const char *n, float mn, float mx, float df, const char *u = "") {
            put(i, n, mn, mx, df, Curve::Exponential, 0, u);
        };
        auto step = [&](int32_t i, const char *n, int32_t count_, float df) {
            put(i, n, 0.0f, static_cast<float>(count_ - 1), df, Curve::Stepped, count_, "");
        };

        step(Model, "model", ModelCount, 0.0f);
        lin(Age, "age", 0.0f, 1.0f, 0.25f);
        // Leakage and hum sound even with no keys down, so they're off by
        // default. A newly added track shouldn't make any sound until it's
        // played. Patches that want them turn them up.
        lin(Leakage, "leakage", 0.0f, 1.0f, 0.0f);
        lin(Hum, "hum", 0.0f, 1.0f, 0.0f);
        lin(Click, "click", 0.0f, 1.0f, 0.35f);
        lin(ClickRelease, "clickoff", 0.0f, 1.0f, 0.2f);
        lin(ContactSpread, "contacts", 0.0f, 1.0f, 0.4f);

        lin(Split, "split", 0.0f, 127.0f, 60.0f);
        lin(PedalSplit, "pedsplit", 0.0f, 127.0f, 48.0f);
        lin(UpperLevel, "upper", 0.0f, 1.0f, 1.0f);
        lin(LowerLevel, "lower", 0.0f, 1.0f, 0.9f);
        lin(PedalLevel, "pedal", 0.0f, 1.0f, 0.9f);
        step(LowerOn, "loweron", 2, 0.0f);
        // On by default. With the pedals off, low notes go to a manual where
        // the 16' and 8' drawbars fold onto the same bottom wheel and double
        // up. The lower manual stays off by default.
        step(PedalOn, "pedalon", 2, 1.0f);
        lin(PedalSustain, "pedsus", 0.0f, 1.0f, 0.15f);

        static const char *barName[Manual::kBars] = {"16", "513", "8", "4", "223", "2", "135", "113", "1"};
        for (int b = 0; b < kBars; ++b) {
            char n[12];
            std::snprintf(n, sizeof(n), "ua_%s", barName[b]);
            lin(UpperA + b, n, 0.0f, 1.0f, (b == 0 || b == 2) ? 1.0f : (b == 3 ? 0.75f : 0.0f));
            std::snprintf(n, sizeof(n), "ub_%s", barName[b]);
            lin(UpperB + b, n, 0.0f, 1.0f, b >= 4 ? 0.8f : 0.25f);
            std::snprintf(n, sizeof(n), "la_%s", barName[b]);
            lin(LowerA + b, n, 0.0f, 1.0f, (b == 2 || b == 3) ? 0.8f : 0.0f);
            std::snprintf(n, sizeof(n), "lb_%s", barName[b]);
            lin(LowerB + b, n, 0.0f, 1.0f, b == 2 ? 1.0f : 0.2f);
        }
        for (int b = 0; b < kPedalBars; ++b) {
            char n[12];
            std::snprintf(n, sizeof(n), "pa_%d", b + 1);
            lin(PedalA + b, n, 0.0f, 1.0f, b == 0 ? 1.0f : 0.5f);
            std::snprintf(n, sizeof(n), "pb_%d", b + 1);
            lin(PedalB + b, n, 0.0f, 1.0f, 0.6f);
        }

        lin(Morph, "morph", 0.0f, 1.0f, 0.0f);
        step(MorphSource, "morphsrc", SourceCount, 0.0f);
        lin(MorphAmount, "morphamt", 0.0f, 1.0f, 1.0f);

        lin(Spray, "spray", 0.0f, 1.0f, 0.06f);
        exp_(SprayRate, "sprayrate", 0.05f, 8.0f, 0.7f, "Hz");
        lin(SprayWidth, "spraywide", 0.0f, 1.0f, 0.5f);
        step(SprayPattern, "spraypat", 3, 0.0f);

        step(PercOn, "perc", 2, 1.0f);
        step(PercHarmonic, "percharm", 2, 1.0f);
        lin(PercLevel, "perclvl", 0.0f, 1.0f, 0.6f);
        step(PercFast, "percfast", 2, 1.0f);
        exp_(PercDecay, "percdec", 0.05f, 4.0f, 0.35f, "s");
        step(PercPoly, "percpoly", 2, 0.0f);
        lin(PercKey, "perckey", 0.0f, 1.0f, 0.3f);
        step(PercSteal, "percsteal", 2, 1.0f);

        step(VibType, "vibtype", 6, 4.0f);
        exp_(VibRate, "vibrate", 2.0f, 12.0f, 6.9f, "Hz");
        lin(VibDepth, "vibdepth", 0.0f, 1.0f, 0.6f);
        step(VibUpper, "vibup", 2, 1.0f);
        step(VibLower, "viblow", 2, 0.0f);
        lin(VibStereo, "vibwide", 0.0f, 1.0f, 0.35f);

        lin(WindSag, "windsag", 0.0f, 1.0f, 0.12f);
        exp_(WindResponse, "windresp", 0.01f, 1.5f, 0.12f, "s");
        lin(WindNoise, "windnoise", 0.0f, 1.0f, 0.0f); // sounds with no keys down, so off (see Leakage)
        exp_(TremRate, "tremrate", 0.5f, 10.0f, 4.2f, "Hz");
        lin(TremDepth, "tremdepth", 0.0f, 1.0f, 0.0f);

        lin(RankPrincipal, "principal", 0.0f, 1.0f, 1.0f);
        lin(RankFlute, "flute", 0.0f, 1.0f, 0.8f);
        lin(RankString, "string", 0.0f, 1.0f, 0.5f);
        lin(RankReed, "reed", 0.0f, 1.0f, 0.35f);
        lin(RankMixture, "mixture", 0.0f, 1.0f, 0.4f);
        lin(Chiff, "chiff", 0.0f, 1.0f, 0.3f);
        lin(Tracker, "tracker", 0.0f, 1.0f, 0.2f);

        step(ComboWave, "combowave", 3, 0.0f);
        lin(Tab16, "tab16", 0.0f, 1.0f, 0.8f);
        lin(Tab8, "tab8", 0.0f, 1.0f, 1.0f);
        lin(Tab4, "tab4", 0.0f, 1.0f, 0.7f);
        lin(Tab2, "tab2", 0.0f, 1.0f, 0.0f);
        lin(TabII, "tab2r", 0.0f, 1.0f, 0.0f);
        lin(TabIV, "tab4r", 0.0f, 1.0f, 0.0f);
        lin(ComboReedy, "reedy", 0.0f, 1.0f, 0.45f);
        lin(ComboAttack, "comboatk", 0.0f, 1.0f, 0.1f);

        lin(ReedPressure, "pressure", 0.0f, 1.0f, 0.7f);
        lin(ReedBuzz, "buzz", 0.0f, 1.0f, 0.35f);
        lin(ReedTremolo, "reedtrem", 0.0f, 1.0f, 0.2f);

        step(RotOn, "rotary", 2, 1.0f);
        step(RotSpeed, "rotspeed", 3, 0.0f); // brake, slow, fast
        exp_(RotHornSlow, "hornslow", 0.1f, 2.0f, 0.8f, "Hz");
        exp_(RotHornFast, "hornfast", 2.0f, 10.0f, 6.6f, "Hz");
        exp_(RotDrumSlow, "drumslow", 0.1f, 2.0f, 0.65f, "Hz");
        exp_(RotDrumFast, "drumfast", 1.0f, 8.0f, 5.3f, "Hz");
        exp_(RotRampUp, "rampup", 0.05f, 4.0f, 0.9f, "s");
        exp_(RotRampDown, "rampdown", 0.05f, 6.0f, 1.6f, "s");
        lin(RotMicDistance, "micdist", 0.0f, 1.0f, 0.35f);
        lin(RotMicAngle, "micangle", 0.0f, 1.0f, 0.8f);
        lin(RotSpread, "rotwide", 0.0f, 1.0f, 0.75f);
        step(RotSync, "rotsync", 6, 0.0f); // free, 1/1, 1/2, 1/4, 1/8, 1/8T

        // Off by default. Drive is the amplifier, which only the tonewheel and
        // combo models have, and their patches set their own. Even 0.2 is
        // +8.5 dB of gain.
        lin(Drive, "drive", 0.0f, 1.0f, 0.0f);
        lin(Bias, "bias", -1.0f, 1.0f, 0.0f);
        lin(Bass, "bass", -12.0f, 12.0f, 0.0f, "dB");
        lin(Mid, "mid", -12.0f, 12.0f, 0.0f, "dB");
        lin(Treble, "treble", -12.0f, 12.0f, 0.0f, "dB");
        // Goes up to 2.0 because a single soft stop is much quieter than full
        // organ and needs the extra range to reach the bank's level.
        lin(Volume, "volume", 0.0f, 2.0f, 0.75f);
        lin(Pan, "pan", -1.0f, 1.0f, 0.0f);

        exp_(AmpAttack, "attack", 0.0005f, 0.5f, 0.004f, "s");
        exp_(AmpRelease, "release", 0.002f, 2.0f, 0.05f, "s");
        exp_(Eg1A, "eg1atk", 0.001f, 8.0f, 0.01f, "s");
        exp_(Eg1D, "eg1dec", 0.005f, 12.0f, 0.6f, "s");
        lin(Eg1S, "eg1sus", 0.0f, 1.0f, 0.7f);
        exp_(Eg1R, "eg1rel", 0.005f, 12.0f, 0.4f, "s");
        exp_(Eg2A, "eg2atk", 0.001f, 8.0f, 1.2f, "s");
        exp_(Eg2D, "eg2dec", 0.005f, 12.0f, 2.0f, "s");
        lin(Eg2S, "eg2sus", 0.0f, 1.0f, 1.0f);
        exp_(Eg2R, "eg2rel", 0.005f, 12.0f, 1.0f, "s");

        step(Lfo1Wave, "lfo1wave", LfoGen::WaveCount, 0.0f);
        exp_(Lfo1Rate, "lfo1rate", 0.01f, 20.0f, 0.4f, "Hz");
        step(Lfo1Sync, "lfo1sync", 6, 0.0f);
        lin(Lfo1Depth, "lfo1depth", 0.0f, 1.0f, 1.0f);
        lin(Lfo1Phase, "lfo1phase", 0.0f, 1.0f, 0.0f);
        step(Lfo2Wave, "lfo2wave", LfoGen::WaveCount, 1.0f);
        exp_(Lfo2Rate, "lfo2rate", 0.01f, 20.0f, 3.0f, "Hz");
        step(Lfo2Sync, "lfo2sync", 6, 0.0f);
        lin(Lfo2Depth, "lfo2depth", 0.0f, 1.0f, 1.0f);
        lin(Lfo2Phase, "lfo2phase", 0.0f, 1.0f, 0.25f);

        for (int m = 0; m < kMatrixSlots; ++m) {
            char n[12];
            const int base = MatrixBase + m * kMatrixParams;
            std::snprintf(n, sizeof(n), "m%d_src", m + 1);
            step(base + XSrc, n, SourceCount, 0.0f);
            std::snprintf(n, sizeof(n), "m%d_dst", m + 1);
            step(base + XDest, n, DestCount, 0.0f);
            std::snprintf(n, sizeof(n), "m%d_amt", m + 1);
            lin(base + XDepth, n, -1.0f, 1.0f, 0.0f);
        }

        lin(BendRange, "bend", 0.0f, 12.0f, 2.0f);
        lin(Octave, "octave", -3.0f, 3.0f, 0.0f);
        lin(Transpose, "transpose", -12.0f, 12.0f, 0.0f);
        lin(Fine, "fine", -50.0f, 50.0f, 0.0f, "c");
        lin(VelocityAmount, "vel", 0.0f, 1.0f, 1.0f);
        step(Expression, "express", 3, 1.0f); // off, mod wheel, pressure
        built = true;
    }
    count = Count;
    return defs;
}

int32_t Manual::steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }

void Manual::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    // Scales the 1.8 ms noise burst to the same energy as the single
    // full-height sample it replaces. Uniform noise has a third of a unit's
    // power, and a decay with time constant t sums to t/2 in squared gain.
    clickBurst = std::sqrt(6.0f / (0.0018f * sampleRate));
    bank = &WheelBank::shared(sampleRate);
    rotary.prepare(sampleRate);
    scanner.prepare(static_cast<int32_t>(sampleRate * 0.01f));
    for (auto &v : voices) v.amp.setSampleRate(sampleRate);
    for (auto &e : eg) e.setSampleRate(sampleRate);
    for (auto &l : lfo) l.reset(0.0f);
    bassEq.lowShelf(180.0f, 0.0f, sampleRate);
    midEq.peak(900.0f, 0.0f, 0.8f, sampleRate);
    trebleEq.highShelf(2600.0f, 0.0f, sampleRate);
    reedyFilter.peak(1500.0f, 6.0f, 1.4f, sampleRate);
    chiffFilter.lowpass(5200.0f, 0.7f, sampleRate);
    modelAge = -1.0f;
    rebuildTuning();
    reset();
}

// Gives every wheel its own small, fixed tuning and level error, like the
// gears of a real generator.
void Manual::rebuildTuning() {
    const float age = paramOf(Age);
    const float spray = paramOf(Spray);
    const int32_t pattern = steppedOf(SprayPattern);
    // The track's tuning is applied per wheel, so every drawbar tapping a
    // wheel hears the same pitch.
    const float *table = tuningTable();
    if (std::fabs(age - modelAge) < 0.0005f && std::fabs(spray - modelSpray) < 0.0005f && table == modelTuning) return;
    modelAge = age;
    modelSpray = spray;
    modelTuning = table;
    uint32_t s = 0x9e3779b9u;
    for (int w = 0; w < WheelBank::kWheels; ++w) {
        s = s * 1664525u + 1013904223u;
        const float r1 = (static_cast<float>((s >> 9) & 0xffff) / 32768.0f) - 1.0f;
        s = s * 1664525u + 1013904223u;
        const float r2 = (static_cast<float>((s >> 9) & 0xffff) / 32768.0f) - 1.0f;
        // Spray detunes across the whole generator rather than per key, so an
        // octave on two drawbars beats with itself. The pattern sets its
        // shape.
        const float t = static_cast<float>(w) / static_cast<float>(WheelBank::kWheels - 1);
        // How much this wheel's second rank is detuned, as a fraction of the
        // spray amount. It stays well above zero across the keyboard so every
        // note beats, like a celeste.
        const float shape = pattern == 0 ? 1.0f
                          : pattern == 1 ? 0.45f + 0.55f * t
                                         : 0.7f + 0.3f * std::sin(t * 12.566f);
        const float cents = r1 * age * 7.0f;
        const int note = std::min(127, WheelBank::kLowestNote + w);
        const float tuned = table != nullptr ? table[note] : 1.0f;
        wheelStep[w] = bank->freq(w) * tuned * std::pow(2.0f, cents / 1200.0f) / sampleRate;
        wheelTrim[w] = 1.0f + r2 * age * 0.12f;
        sprayDetune[w] = shape;
        // Where the second rank sits in the stereo field, low wheels on one
        // side and high on the other.
        sprayPan[w] = t * 2.0f - 1.0f;
    }
}

void Manual::reset() {
    // Replace whole voices so percussion, click, chiff and contact timing all
    // clear, then restore the envelope's sample rate, which prepare() owns.
    for (auto &v : voices) { v = Voice(); v.amp.setSampleRate(sampleRate); }

    // The wheels free-run, so they all need resetting here for renders to be
    // repeatable.
    for (auto &p : wheelPhase) p = 0.0f;
    for (auto &p : wheelOut) p = 0.0f;
    for (auto &p : sprayPhase) p = 0.0f;
    for (auto &slot : wheelPeak) for (auto &p : slot) p = 0.0f;
    // The stamp cache marks which wheels a slot touched this frame. Stale
    // stamps would match the restarted frame count and skip wheels.
    for (auto &slot : wheelStamp) for (auto &p : slot) p = 0;
    for (auto &slot : wheelGain) for (auto &p : slot) p = 0.0f;
    for (auto &slot : usedWheel) for (auto &p : slot) p = 0;
    for (auto &n : usedCount) n = 0;
    frameStamp = 0;

    // Derived from the model parameters. Zeroed and marked stale so the next
    // render rebuilds them.
    for (auto &p : wheelStep) p = 0.0f;
    for (auto &p : wheelTrim) p = 0.0f;
    for (auto &p : sprayPan) p = 0.0f;
    for (auto &p : sprayStep) p = 0.0f;
    modelAge = -1.0f;
    modelSpray = -1.0f;
    sprayDrift = 0.0f;

    for (auto &l : lfo) l.reset(0.0f);
    for (auto &v : lfoValue) v = 0.0f;
    for (auto &e : eg) e.reset();
    scanPhase = scanValue = 0.0f;
    tremPhase = 0.0f;
    quietBlocks = 0;
    presence = 0.0f;
    humPhase = 0.0f;

    rotary.reset();
    scanner.clear();

    // Clear the tone stack's filter history. The coefficients are set every
    // block.
    bassEq.reset();
    midEq.reset();
    trebleEq.reset();
    reedyFilter.reset();
    chiffFilter.reset();

    windPressure = 1.0f;
    heldCount = 0;
    bendSemis = 0.0f;
    modWheel = pressure = 0.0f;
    expression = 1.0f;
    for (auto &m : blockMod) m = 0.0f;
    leakSum = 0.0f;
    rngState = kRngSeed;
    clickRng = kClickSeed;
}

int32_t Manual::wheelFor(int32_t note, int32_t bar) const {
    const int32_t foot = kFootage[bar];
    int32_t w = note + foot - WheelBank::kLowestNote;
    // Fold back an octave when running out of wheels at either end.
    while (w >= WheelBank::kWheels) w -= 12;
    while (w < 0) w += 12;
    return w;
}

void Manual::noteOn(uint8_t note, uint8_t velocity) {
    const int32_t split = static_cast<int32_t>(targetOf(Split) + 0.5f);
    const int32_t pedSplit = static_cast<int32_t>(targetOf(PedalSplit) + 0.5f);
    const bool lowerOn = steppedTargetOf(LowerOn) != 0;
    const bool pedalOn = steppedTargetOf(PedalOn) != 0;
    int32_t manual = MUpper;
    if (pedalOn && note < pedSplit) manual = MPedal;
    else if (lowerOn && note < split) manual = MLower;

    Voice *v = nullptr;
    for (auto &cand : voices) if (!cand.used) { v = &cand; break; }
    if (v == nullptr) {
        v = &voices[0];
        for (auto &cand : voices) if (!cand.gate) { v = &cand; break; }
    }
    const bool wasSilent = heldCount == 0;
    v->used = true;
    v->gate = true;
    v->note = note;
    v->manual = static_cast<uint8_t>(manual);
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->velGain = velocityGain(v->velocity, targetOf(VelocityAmount));
    v->key01 = clampf((static_cast<float>(note) - 24.0f) / 72.0f, 0.0f, 1.0f);
    rngState = rngState * 1664525u + 1013904223u;
    v->rnd = static_cast<float>((rngState >> 9) & 0xffff) / 65536.0f;
    // The combo organ's attack knob adds up to a quarter of a second, like
    // the slow attack tab on those organs.
    float attackSec = targetOf(AmpAttack);
    if (steppedTargetOf(Model) == Transistor) attackSec += targetOf(ComboAttack) * 0.25f;
    v->amp.set(0.0f, attackSec, 0.0f, 1.0f, targetOf(AmpRelease), false);
    v->amp.retrigger();

    const int bars = manual == MPedal ? kPedalBars : kBars;
    for (int b = 0; b < kBars; ++b) {
        v->wheel[b] = manual == MPedal && b < kPedalBars
                          ? (note + kPedalFootage[b] - WheelBank::kLowestNote + 120) % WheelBank::kWheels
                          : wheelFor(note, b);
        // The nine contacts under a key close a couple of milliseconds
        // apart, which is what makes the key click.
        v->contactPhase[b] = b < bars ? (0.2f + 2.4f * targetOf(ContactSpread)) * 0.001f * sampleRate *
                                            (0.15f + 0.85f * std::fmod(v->rnd * (b + 3) * 7.13f, 1.0f))
                                      : 0.0f;
    }
    // Click and chiff are fixed at note-on, so they use the matrix's block
    // value since the voice has no per-voice modulation yet.
    v->click = clampf(targetOf(Click) + blockMod[DstClick], 0.0f, 1.5f);
    v->clickCoeff = std::exp(-1.0f / (0.0018f * sampleRate));
    v->chiff = steppedTargetOf(Model) == Pipe ? clampf(targetOf(Chiff) + blockMod[DstChiff], 0.0f, 1.5f) : 0.0f;
    v->chiffCoeff = std::exp(-1.0f / (0.035f * sampleRate));

    // Harmonic percussion only fires on the first key of a phrase, unless
    // it's set to polyphonic.
    if (steppedTargetOf(PercOn) != 0 && manual == MUpper && (steppedTargetOf(PercPoly) != 0 || wasSilent)) {
        const float keyScale = 1.0f - targetOf(PercKey) * v->key01;
        const float decay = targetOf(PercDecay) * (steppedTargetOf(PercFast) != 0 ? 0.35f : 1.0f) * keyScale;
        v->perc = 1.0f;
        v->percCoeff = std::exp(-1.0f / (std::fmax(0.01f, decay) * sampleRate));
    } else {
        v->perc = 0.0f;
    }
    // The two mod envelopes belong to the whole instrument, like the LFOs.
    // They only retrigger on the first key of a phrase, so adding a note to a
    // held chord doesn't restart them.
    if (wasSilent) for (auto &e : eg) e.retrigger();
    ++heldCount;
}

void Manual::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            const float sus = v.manual == MPedal ? paramOf(PedalSustain) : 0.0f;
            v.amp.set(0.0f, paramOf(AmpAttack), 0.0f, 1.0f, paramOf(AmpRelease) + sus * 1.5f, false);
            v.amp.release();
            // The release click. All nine contacts break together, so it
            // isn't staggered like the note-on click.
            v.click = paramOf(Click) * paramOf(ClickRelease);
            const int bars = v.manual == MPedal ? kPedalBars : kBars;
            for (int b = 0; b < bars; ++b) v.clickEnv += v.click * v.barLevel[b];
            if (heldCount > 0) --heldCount;
        }
    }
    if (heldCount == 0) for (auto &e : eg) e.release();
}

void Manual::allNotesOff() {
    for (auto &v : voices) { v.gate = false; v.amp.release(); }
    for (auto &e : eg) e.release();
    heldCount = 0;
}

void Manual::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
    if (cc == 11) expression = static_cast<float>(value) / 127.0f;
}
void Manual::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Manual::pitchBend(int16_t value14) {
    bendSemis = (static_cast<float>(value14) / 8192.0f) * paramOf(BendRange);
}


void Manual::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

float Manual::sourceValue(int32_t src, const Voice &v) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    case SrcPressure: return pressure;
    case SrcVelocity: return v.velocity;
    case SrcKeyTrack: return v.key01;
    case SrcRandom: return v.rnd;
    case SrcEg1: return eg[0].value();
    case SrcEg2: return eg[1].value();
    case SrcLfo1: return lfoValue[0];
    case SrcLfo2: return lfoValue[1];
    // Horn and drum phase as bipolar sources.
    case SrcHorn: return std::sin(6.2831853f * rotary.horn01());
    case SrcDrum: return std::sin(6.2831853f * rotary.drum01());
    case SrcScanner: return scanValue;
    case SrcWind: return windPressure - 1.0f;
    default: return 0.0f;
    }
}

void Manual::applyMatrix(const Voice &v, float *dest) const {
    for (int32_t d = 0; d < DestCount; ++d) dest[d] = 0.0f;
    for (int m = 0; m < kMatrixSlots; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        const int32_t src = static_cast<int32_t>(paramOf(base + XSrc) + 0.5f);
        const int32_t dst = static_cast<int32_t>(paramOf(base + XDest) + 0.5f);
        if (src == SrcOff || dst == DstOff) continue;
        dest[dst] += sourceValue(src, v) * paramOf(base + XDepth);
    }
}

int32_t Manual::timbreForRank(int32_t rank) const {
    static const int32_t rankTimbre[5] = {WheelBank::Principal, WheelBank::Flute, WheelBank::String,
                                          WheelBank::Reed, WheelBank::Principal};
    return rankTimbre[rank < 0 ? 0 : (rank > 4 ? 4 : rank)];
}

int32_t Manual::timbreFor(int32_t bar) const {
    (void)bar;
    switch (steppedOf(Model)) {
    case Transistor: {
        const int32_t w = steppedOf(ComboWave);
        return w == 0 ? WheelBank::Square : (w == 1 ? WheelBank::Pulse : WheelBank::Saw);
    }
    // Pipes can have several ranks per footage. The render loop handles them
    // slot by slot.
    case Pipe: return WheelBank::Principal;
    case ReedOrgan: return WheelBank::Reed;
    default: return WheelBank::Wheel;
    }
}

// One drawbar's level, after the morph between the two registrations and
// the matrix.
float Manual::drawbarLevel(int32_t manual, int32_t bar, const float *mod) const {
    const int32_t model = steppedOf(Model);
    float a = 0.0f, b = 0.0f;
    if (manual == MPedal) {
        if (bar >= kPedalBars) return 0.0f;
        a = paramOf(PedalA + bar);
        b = paramOf(PedalB + bar);
    } else if (manual == MLower) {
        a = paramOf(LowerA + bar);
        b = paramOf(LowerB + bar);
    } else {
        a = paramOf(UpperA + bar);
        b = paramOf(UpperB + bar);
    }
    if (model == Transistor && manual != MPedal) {
        // A combo organ has footage tabs and two mixture tabs instead of
        // drawbars. The mixtures only use octaves and fifths so chords stay
        // clean: II is the 2 2/3' and 1 1/3', IV adds the 2', 1 1/3' and 1'.
        // The tierce (1 3/5') is left out.
        static const int tabFor[kBars] = {Tab16, -1, Tab8, Tab4, TabII, Tab2, -1, TabII, -1};
        const int p = tabFor[bar];
        a = p < 0 ? 0.0f : paramOf(p) * (bar == 7 ? 0.6f : 1.0f);
        if (bar == 5) a += paramOf(TabIV) * 0.45f;
        else if (bar == 7) a += paramOf(TabIV) * 0.55f;
        else if (bar == 8) a += paramOf(TabIV);
        b = a;
    }
    // Pipe ranks aren't applied here. render() handles them one slot each.
    float morph = clampf(paramOf(Morph) + mod[DstMorph] * paramOf(MorphAmount), 0.0f, 1.0f);
    const int32_t msrc = static_cast<int32_t>(paramOf(MorphSource) + 0.5f);
    if (msrc != SrcOff) morph = clampf(morph, 0.0f, 1.0f);
    float level = a + (b - a) * morph;
    level += mod[DstBar1 + bar];
    level += manual == MUpper ? mod[DstUpperAll] : mod[DstLowerAll];
    return clampf(level, 0.0f, 1.5f);
}

bool Manual::render(float *L, float *R, int32_t frames) {
    params_.tick();
    rebuildTuning();
    const float blockSeconds = static_cast<float>(frames) / sampleRate;
    const int32_t model = steppedOf(Model);

    // Block-rate modulation: LFOs, envelopes and rotor speed.
    static const float kSyncBeats[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f};
    for (int i = 0; i < 2; ++i) {
        const int32_t sync = steppedOf(i == 0 ? Lfo1Sync : Lfo2Sync);
        const float free = paramOf(i == 0 ? Lfo1Rate : Lfo2Rate);
        const float hz = sync == 0 ? free : (bpm / 60.0f) / kSyncBeats[sync];
        lfoValue[i] = lfo[i].advance(steppedOf(i == 0 ? Lfo1Wave : Lfo2Wave), hz, blockSeconds, 0.0f, false) *
                      paramOf(i == 0 ? Lfo1Depth : Lfo2Depth);
    }
    eg[0].set(0.0f, paramOf(Eg1A), paramOf(Eg1D), paramOf(Eg1S), paramOf(Eg1R), false);
    eg[1].set(0.0f, paramOf(Eg2A), paramOf(Eg2D), paramOf(Eg2S), paramOf(Eg2R), false);
    // Only stepped in the sample loop if the matrix uses one. Same as Filament.
    bool egWanted = false;
    for (int m = 0; m < kMatrixSlots && !egWanted; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        if (steppedOf(base + XDest) == DstOff) continue;
        const int32_t src = steppedOf(base + XSrc);
        egWanted = src == SrcEg1 || src == SrcEg2;
    }

    Voice global;
    global.velocity = 1.0f;
    applyMatrix(global, blockMod);
    // Matrix and drawbar levels are worked out once a block, since their
    // sources are block-rate anyway.
    for (auto &v : voices) {
        if (!v.used) continue;
        applyMatrix(v, v.mod);
        const int bars = v.manual == MPedal ? kPedalBars : kBars;
        for (int b = 0; b < kBars; ++b) v.barLevel[b] = b < bars ? drawbarLevel(v.manual, b, v.mod) : 0.0f;
    }

    const int32_t rotSync = steppedOf(RotSync);
    const int32_t speed = steppedOf(RotSpeed);
    float hornHz = speed == 0 ? 0.0f : (speed == 1 ? paramOf(RotHornSlow) : paramOf(RotHornFast));
    float drumHz = speed == 0 ? 0.0f : (speed == 1 ? paramOf(RotDrumSlow) : paramOf(RotDrumFast));
    if (rotSync != 0) {
        hornHz = (bpm / 60.0f) / kSyncBeats[rotSync];
        drumHz = hornHz * 0.78f;
    }
    hornHz *= 1.0f + blockMod[DstRotorRate];
    drumHz *= 1.0f + blockMod[DstRotorRate];
    rotary.setTargets(hornHz, drumHz, paramOf(RotRampUp), paramOf(RotRampDown));
    rotary.setMic(paramOf(RotMicDistance), paramOf(RotMicAngle), paramOf(RotSpread));

    bassEq.lowShelf(180.0f, paramOf(Bass), sampleRate);
    midEq.peak(900.0f, paramOf(Mid), 0.8f, sampleRate);
    trebleEq.highShelf(2600.0f, clampf(paramOf(Treble) + blockMod[DstTreble] * 12.0f, -18.0f, 18.0f), sampleRate);

    const float spray = clampf(paramOf(Spray) + blockMod[DstSpray], 0.0f, 1.0f);
    // Spray detunes in cents so it's even across the keyboard. The slow drift
    // term keeps it moving.
    if (spray > 0.0005f) {
        sprayDrift += paramOf(SprayRate) * 0.08f * static_cast<float>(frames) / sampleRate;
        if (sprayDrift >= 1.0f) sprayDrift -= 1.0f;
        for (int w = 0; w < WheelBank::kWheels; ++w) {
            const float move = 1.0f + 0.35f * std::sin(6.2831853f * (sprayDrift + 0.11f * static_cast<float>(w)));
            const float cents = spray * 26.0f * sprayDetune[w] * move;
            sprayStep[w] = wheelStep[w] * (std::pow(2.0f, cents / 1200.0f) - 1.0f);
        }
    }
    const float sprayWidth = paramOf(SprayWidth);
    const float leak = paramOf(Leakage);
    const float hum = paramOf(Hum);
    const float windSag = clampf(paramOf(WindSag) + blockMod[DstWindSag], 0.0f, 1.0f);
    const float windCoeff = onePoleCoeff(paramOf(WindResponse), sampleRate);
    const float windNoise = paramOf(WindNoise);
    // The idle sounds come in quickly with the first note and fade slowly
    // after the last.
    const float presenceIn = onePoleCoeff(0.05f / 3.0f, sampleRate);
    const float presenceOut = onePoleCoeff(0.4f / 3.0f, sampleRate);
    // The two tremulant depths are combined like independent depths rather
    // than added, so the total stays below 1 and never inverts the signal.
    const float tremA = paramOf(TremDepth);
    const float tremB = model == ReedOrgan ? paramOf(ReedTremolo) : 0.0f;
    const float tremDepth = 1.0f - (1.0f - clampf(tremA, 0.0f, 1.0f)) * (1.0f - clampf(tremB, 0.0f, 1.0f));
    const float tremStep = paramOf(TremRate) / sampleRate;
    const float percLevel = clampf(paramOf(PercLevel) + blockMod[DstPercLevel], 0.0f, 1.5f);
    const int32_t percBar = steppedOf(PercHarmonic) != 0 ? 3 : 4; // 2nd is the 4', 3rd the 2⅔'
    const bool percSteal = steppedOf(PercSteal) != 0;
    const float drive = clampf(paramOf(Drive) + blockMod[DstDrive], 0.0f, 1.0f);
    const float bias = paramOf(Bias);
    const float volume = clampf(paramOf(Volume) + blockMod[DstVolume], 0.0f, 2.5f);
    const float pan = clampf(paramOf(Pan) + blockMod[DstPan], -1.0f, 1.0f);
    const int32_t expr = steppedOf(Expression);
    const float exprGain = expr == 0 ? 1.0f : (expr == 1 ? 0.25f + 0.75f * modWheel : 0.25f + 0.75f * pressure);
    const float upperGain = paramOf(UpperLevel), lowerGain = paramOf(LowerLevel), pedalGain = paramOf(PedalLevel);
    const float pitchScale = std::pow(2.0f, (bendSemis + paramOf(Octave) * 12.0f + paramOf(Transpose) +
                                             paramOf(Fine) * 0.01f + blockMod[DstPitch] * 12.0f) /
                                                12.0f);
    const float reedPress = model == ReedOrgan ? 0.35f + 0.65f * paramOf(ReedPressure) : 1.0f;
    const float buzz = model == ReedOrgan ? paramOf(ReedBuzz) : 0.0f;
    // The two saturations and the pan only depend on knobs, so they're
    // worked out once a block.
    const float buzzK = 1.0f + buzz * 4.0f;
    const float buzzBias = buzz * 0.45f; // the reed frame is closer on one side
    const float buzzBiasOut = std::tanh(buzzBias);
    // Normalised at the measured level that arrives here.
    constexpr float kBuzzNominal = 0.55f;
    const float buzzNorm = kBuzzNominal / std::tanh(kBuzzNominal * buzzK);
    const float driveG = 1.0f + drive * 5.0f;
    const float driveBias = bias * 0.5f;
    const float driveBiasOut = std::tanh(driveBias);
    const float driveNorm = 0.4f / std::tanh(0.4f * driveG);
    const float panAngle = (pan + 1.0f) * 0.25f * kPi;
    const float panL = std::cos(panAngle) * 1.4142f, panR = std::sin(panAngle) * 1.4142f;
    const float chiffAmt = model == Pipe ? paramOf(Chiff) : 0.0f;
    const float trackerAmt = model == Pipe ? paramOf(Tracker) : 0.0f;
    // Pipes and reeds are separate sources per note, so they add. A shared
    // generator doesn't.
    const bool busLoaded = model == Tonewheel || model == Transistor;
    const bool rotOn = steppedOf(RotOn) != 0;
    // The EQ runs whenever a knob or treble modulation moves it.
    const bool eqActive = std::fabs(paramOf(Bass)) + std::fabs(paramOf(Mid)) +
                              std::fabs(clampf(paramOf(Treble) + blockMod[DstTreble] * 12.0f, -18.0f, 18.0f)) >
                          0.05f;
    int slotTimbre[kSlots];
    for (int sl = 0; sl < kSlots; ++sl) slotTimbre[sl] = model == Pipe ? timbreForRank(sl) : timbreFor(0);
    // What each footage draws from each rank: the stop list times the five
    // rank knobs, once a block.
    float stopGain[kBars][kSlots] = {};
    if (model == Pipe) {
        static const int rankParam[kSlots] = {RankPrincipal, RankFlute, RankString, RankReed, RankMixture};
        for (int r = 0; r < kSlots; ++r) {
            const float knob = paramOf(rankParam[r]);
            for (int b = 0; b < kBars; ++b) stopGain[b][r] = kPipeStops[b][r] * knob;
        }
    }

    // The scanner: a delay swept by a triangle. The chorus positions mix in
    // the dry signal, the vibrato positions don't.
    const int32_t vibType = steppedOf(VibType);
    const float vibDepthP = clampf(paramOf(VibDepth) + blockMod[DstVibDepth], 0.0f, 1.0f);
    const bool chorusMode = vibType >= 3;
    const float vibStage = static_cast<float>((vibType % 3) + 1) / 3.0f;
    const float vibSamples = 0.0009f * sampleRate * vibStage * vibDepthP;
    const float vibStep = paramOf(VibRate) / sampleRate;
    const bool vibUp = steppedOf(VibUpper) != 0, vibLow = steppedOf(VibLower) != 0;

    // Demand on the wind supply for the sag. Every sounding voice adds to it.
    float demand = 0.0f;
    for (auto &v : voices) if (v.used) demand += v.amp.value() * (v.manual == MPedal ? 1.4f : 1.0f);

    bool anyVoice = false;
    for (auto &v : voices) if (v.used) { anyVoice = true; break; }
    // With no key down and the output quiet, the organ sleeps to save CPU.
    // It waits for the idle sounds to fade (see `presence`) and then for two
    // blocks under -120 dB, so the cabinet's tail isn't cut short.
    //
    // Everything that turns keeps turning: the wheels, the spray, both
    // rotors (including speed ramps), the tremulant, the scanner and the
    // wind. Only the sound is skipped.
    if (!anyVoice && presence < 1e-4f && quietBlocks >= 2) {
        presence = 0.0f;
        // Advanced once per block rather than per sample, which is most of
        // the saving. It rounds slightly differently, which doesn't matter
        // for free-running wheels.
        const float n = static_cast<float>(frames);
        for (int w = 0; w < WheelBank::kWheels; ++w) {
            wheelPhase[w] += wheelStep[w] * pitchScale * n;
            wheelPhase[w] -= std::floor(wheelPhase[w]);
        }
        if (spray > 0.0005f) {
            for (int w = 0; w < WheelBank::kWheels; ++w) {
                sprayPhase[w] += sprayStep[w] * n;
                sprayPhase[w] -= std::floor(sprayPhase[w]);
            }
        }
        windPressure = 1.0f + (windPressure - 1.0f) * std::pow(1.0f - windCoeff, n);
        if (tremDepth > 0.0005f) {
            tremPhase += tremStep * n;
            tremPhase -= std::floor(tremPhase);
        }
        scanPhase += vibStep * n;
        scanPhase -= std::floor(scanPhase);
        if (egWanted) for (int32_t i = 0; i < frames; ++i) { eg[0].next(); eg[1].next(); }
        if (rotOn) rotary.spin(frames);
        for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
        return true;
    }

    for (int32_t i = 0; i < frames; ++i) {
        if (egWanted) { eg[0].next(); eg[1].next(); }
        for (int w = 0; w < WheelBank::kWheels; ++w) {
            wheelPhase[w] += wheelStep[w] * pitchScale * (0.997f + 0.003f * windPressure);
            // A top wheel with the octave, transpose and bend all up can step
            // more than a whole cycle a sample, and one subtraction would
            // leave the table read past its end.
            if (wheelPhase[w] >= 1.0f) wheelPhase[w] -= std::floor(wheelPhase[w]);
        }
        if (spray > 0.0005f) {
            for (int w = 0; w < WheelBank::kWheels; ++w) {
                sprayPhase[w] += sprayStep[w];
                if (sprayPhase[w] >= 1.0f) sprayPhase[w] -= 1.0f;
                else if (sprayPhase[w] < 0.0f) sprayPhase[w] += 1.0f;
            }
        }

        // Wind: one supply shared by every voice. A full chord should lose a
        // few decibels and a dozen cents at most, which is what 0.16 gives.
        const float target = 1.0f / (1.0f + windSag * demand * 0.16f);
        windPressure += (target - windPressure) * windCoeff;
        float trem = 1.0f;
        if (tremDepth > 0.0005f) {
            tremPhase += tremStep;
            if (tremPhase >= 1.0f) tremPhase -= 1.0f;
            // Full depth is about 10 dB and never reaches silence.
            trem = 1.0f - tremDepth * 0.35f * (1.0f - std::cos(6.2831853f * tremPhase));
        }

        // `click` is the electric contact, scaled by the click knob. `mech`
        // is the key moving, for the pipe model's tracker noise. They're
        // kept separate so the click knob doesn't set the tracker noise.
        float dry = 0.0f, click = 0.0f, mech = 0.0f;
        ++frameStamp;
        for (int sl = 0; sl < kSlots; ++sl) usedCount[sl] = 0;
        for (auto &v : voices) {
            if (!v.used) continue;
            const float env = v.amp.next();
            // Keep the voice until its release click has finished.
            if (!v.gate && env < 0.0002f && v.clickEnv < 1e-4f) { v.used = false; continue; }
            const int bars = v.manual == MPedal ? kPedalBars : kBars;
            const float manualGain = v.manual == MUpper ? upperGain : (v.manual == MLower ? lowerGain : pedalGain);
            const float velGain = v.velGain;
            const float voiceGain = env * manualGain * velGain * (1.0f + v.mod[DstVolume] * 0.5f);
            for (int b = 0; b < bars; ++b) {
                float level = v.barLevel[b];
                if (percSteal && v.manual == MUpper && b == kBars - 1 && v.perc > 0.0f) level = 0.0f;
                if (level <= 0.0005f) continue;
                if (v.contactPhase[b] > 0.0f) {
                    v.contactPhase[b] -= 1.0f;
                    if (v.contactPhase[b] <= 0.0f) {
                        v.clickEnv += v.click * level; // contact closes
                        mech += level;              // key moves
                    }
                    continue;
                }
                const int w = v.wheel[b];
                const float g = level * voiceGain;
                // One entry per footage, except for pipes where each drawn
                // rank gets its own slot.
                auto draw = [&](int sl, float gain) {
                    if (gain <= 0.0005f) return;
                    if (wheelStamp[sl][w] != frameStamp) {
                        wheelStamp[sl][w] = frameStamp;
                        wheelGain[sl][w] = 0.0f;
                        wheelPeak[sl][w] = 0.0f;
                        usedWheel[sl][usedCount[sl]++] = static_cast<int16_t>(w);
                    }
                    wheelGain[sl][w] += gain;
                    if (gain > wheelPeak[sl][w]) wheelPeak[sl][w] = gain;
                };
                if (model == Pipe) {
                    for (int r = 0; r < kSlots; ++r) draw(r, g * stopGain[b][r]);
                } else {
                    draw(0, g);
                }
            }
            // The contact click is a 1.8 ms burst of decaying noise, scaled by
            // clickBurst. It uses its own noise generator so the chiff and
            // wind noise stay the same.
            if (v.clickEnv > 1e-5f) {
                clickRng = clickRng * 1664525u + 1013904223u;
                const float n = (static_cast<float>((clickRng >> 9) & 0xffff) / 32768.0f) - 1.0f;
                click += n * v.clickEnv * clickBurst;
                v.clickEnv *= v.clickCoeff;
            }
            if (v.perc > 0.0f) {
                const int w = v.wheel[percBar];
                dry += bank->sample(w, WheelBank::Sine, wheelPhase[w]) * v.perc * percLevel * 0.5f * voiceGain;
                v.perc *= v.percCoeff;
                if (v.perc < 0.0005f) v.perc = 0.0f;
            }
            if (v.chiff > 0.0f) {
                rngState = rngState * 1664525u + 1013904223u;
                const float n = (static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f;
                // Not scaled by the envelope, which is still near zero during
                // the chiff, but scaled by manual level and velocity so quiet
                // chords get quiet chiffs.
                dry += chiffFilter.process(n) * v.chiff * chiffAmt * 0.5f * manualGain * velGain;
                v.chiff *= v.chiffCoeff;
            }
        }
        // One pass over the wheels in use. On the shared generator models a
        // wheel feeding two keys is only a little louder than one, so chords
        // don't swell where notes share a harmonic.
        float wideL = 0.0f, wideR = 0.0f;
        for (int sl = 0; sl < kSlots; ++sl) {
            if (usedCount[sl] == 0) continue;
            const int timbre = slotTimbre[sl];
            for (int k = 0; k < usedCount[sl]; ++k) {
                const int w = usedWheel[sl][k];
                const float sum = wheelGain[sl][w], peak = wheelPeak[sl][w];
                const float g = busLoaded ? peak + (sum - peak) * 0.25f : sum;
                // Spray reads a second, detuned copy of the wheel and sums the
                // two, like a celeste. The difference between them is the
                // beating, and that's what gets spread across the stereo
                // field.
                const float x0 = bank->sample(w, timbre, wheelPhase[w]);
                float x = x0;
                float beat = 0.0f;
                if (spray > 0.0005f) {
                    float ph = wheelPhase[w] + sprayPhase[w];
                    if (ph >= 1.0f) ph -= 1.0f;
                    else if (ph < 0.0f) ph += 1.0f;
                    const float x1 = bank->sample(w, timbre, ph);
                    x = (x0 + x1) * 0.5f;
                    beat = (x1 - x0) * 0.5f;
                }
                const float gain = g * wheelTrim[w] * 0.32f;
                x *= gain;
                dry += x;
                if (sprayWidth > 0.0f && beat != 0.0f) {
                    wideL += beat * gain * sprayPan[w];
                    wideR -= beat * gain * sprayPan[w];
                }
            }
        }
        // Leakage, hum and blower noise. They're scaled by `presence`, which
        // fades in when the organ starts playing and out after its last note,
        // so an idle track stays silent.
        presence += ((anyVoice ? 1.0f : 0.0f) - presence) * (anyVoice ? presenceIn : presenceOut);
        if (leak > 0.0005f || hum > 0.0005f) {
            leakSum *= 0.995f;
            const int w = i % WheelBank::kWheels;
            leakSum += bank->sample(w, WheelBank::Wheel, wheelPhase[w]) * 0.05f;
            humPhase += 60.0f / sampleRate;
            if (humPhase >= 1.0f) humPhase -= 1.0f;
            dry += (leakSum * leak * 0.25f + std::sin(6.2831853f * humPhase) * hum * 0.004f) * presence;
        }
        if (windNoise > 0.0005f) {
            rngState = rngState * 1664525u + 1013904223u;
            const float n = (static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f;
            dry += n * windNoise * 0.01f * (0.3f + demand) * presence;
        }
        if (trackerAmt > 0.0005f && mech > 0.0f) dry += mech * trackerAmt * 0.06f;

        // Key click only on the two electric models. The contact timing still
        // runs on every model because the tracker noise uses it.
        if (model == Tonewheel || model == Transistor) dry += click * 0.6f;
        // The tremulant moves the wind pressure, which sets both how hard a
        // reed buzzes and how loud it is. So it's applied inside the buzz
        // saturation and again after it, otherwise the saturation flattens
        // it.
        dry *= windPressure * reedPress;
        if (buzz > 0.0f) {
            // Reed buzz is an asymmetric tanh, since a reed is stopped harder
            // one way than the other. It's monotonic, so more level always
            // means more buzz.
            //
            // The drive is per reed so chords don't saturate harder than
            // single notes. The mix is divided by the square root of the
            // demand, because notes on different wheels add incoherently.
            const float reeds = std::sqrt(std::fmax(1.0f, demand));
            const float one = dry / reeds;
            dry = reeds * (std::tanh(one * buzzK * trem + buzzBias) - buzzBiasOut) * buzzNorm;
        }
        dry *= trem;

        // Scanner vibrato, then the amp, then the cabinet.
        const bool vibActive = vibDepthP > 0.001f && (vibUp || vibLow);
        if (vibActive) scanner.write(dry);
        scanPhase += vibStep;
        if (scanPhase >= 1.0f) scanPhase -= 1.0f;
        scanValue = 2.0f * std::fabs(2.0f * scanPhase - 1.0f) - 1.0f;
        float wet = dry;
        if (vibActive) {
            const float d = scanner.read(vibSamples * (1.0f + scanValue) + 2.0f);
            wet = chorusMode ? (dry * 0.6f + d * 0.6f) : d;
        }
        float x = wet;
        if (drive > 0.001f) {
            // The amp's saturation. Normalised so a nominal 0.4 signal passes
            // at its own size, with the bias's offset taken back out so it
            // doesn't leave DC.
            x = (std::tanh(x * driveG + driveBias) - driveBiasOut) * driveNorm;
        }
        if (eqActive) x = trebleEq.process(midEq.process(bassEq.process(x)));
        if (model == Transistor) x += reedyFilter.process(x) * paramOf(ComboReedy) * 0.5f;
        // The house level (see Reflux's kHouse). It sets where the bank sits on
        // the volume knob, so both the thinnest stops and full organ fit
        // within its travel.
        constexpr float kHouse = 0.9f;
        x *= volume * exprGain * kHouse;

        float outL = x, outR = x;
        if (rotOn) rotary.process(x, outL, outR);
        const float wide = sprayWidth * spray * 0.5f;
        outL += wideL * wide * volume * exprGain * kHouse;
        outR += wideR * wide * volume * exprGain * kHouse;
        L[i] = outL * panL;
        R[i] = outR * panR;
    }
    if (anyVoice) {
        quietBlocks = 0;
    } else {
        float peak = 0.0f;
        for (int32_t i = 0; i < frames; ++i) peak = std::fmax(peak, std::fmax(std::fabs(L[i]), std::fabs(R[i])));
        quietBlocks = peak < 1e-6f ? quietBlocks + 1 : 0;
    }
    return true;
}

} // namespace acidulous::machine
