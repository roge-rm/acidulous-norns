#include "Tine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a note at velocity 100, set so the bank sits with the other machines. */
constexpr float kHouse = 0.5f;
constexpr float kSilent = 2e-5f;
constexpr float kTwoPi = 6.28318530718f;
/** Samples between retunings: bends and the bow move this often. */
constexpr int kRetuneEvery = 16;
/** The pitch the kinds' ring times are given at (middle C), Hz. */
constexpr float kRefHz = 261.63f;
/** How long a note rings under the damper at full, seconds. */
constexpr float kDampedT60 = 0.08f;
/** The tube's feedback: how sharply it picks out its note. */
constexpr float kTubeFeedback = 0.9f;
/** How much of a ringing bar is left when it's struck again. */
/** How long a re-strike takes to damp what's still ringing, seconds. */
constexpr float kSettleSeconds = 0.003f;
constexpr float kRestrikeKeeps = 0.6f;
/** The lowest note a tube is long enough for, Hz. */
constexpr float kTubeLowest = 20.0f;

/**
 * Each kind: its modes as ratios of the note with their share of a strike,
 * how long it rings at middle C, how that changes up the range (as a power
 * of the pitch) and from mode to mode (as a power of the ratio), the knock of
 * whatever strikes it, and how long that stays in contact at its softest.
 */
struct Make {
    int modes;
    float ratio[Tine::kModes];
    float amp[Tine::kModes];
    float t60;
    float pitchFall, modeFall;
    float click;
    float contact;
    float level;
};
constexpr Make kMakes[Tine::KindCount] = {
    // Marimba: a wooden bar undercut so its overtones sit two octaves and two octaves and a third up.
    {4, {1.0f, 3.99f, 9.95f, 17.6f}, {1.0f, 0.35f, 0.12f, 0.05f}, 1.6f, 0.6f, 1.0f, 0.06f, 1.0f, 1.0f},
    // Vibraphone: a metal bar tuned the same way, ringing for seconds.
    {4, {1.0f, 4.0f, 10.0f, 17.8f}, {1.0f, 0.25f, 0.08f, 0.03f}, 7.0f, 0.5f, 0.8f, 0.03f, 1.0f, 0.8f},
    // Xylophone: a harder wood, undercut to a twelfth, short and bright.
    {5, {1.0f, 3.0f, 6.0f, 9.9f, 14.4f}, {1.0f, 0.5f, 0.25f, 0.12f, 0.06f}, 0.9f, 0.6f, 0.7f, 0.12f, 0.6f, 1.6f},
    // Glockenspiel: a steel bar left as it is, its overtones far from the series.
    {5, {1.0f, 2.756f, 5.404f, 8.933f, 13.34f}, {1.0f, 0.4f, 0.2f, 0.1f, 0.05f}, 4.0f, 0.3f, 0.6f, 0.05f, 0.4f, 1.1f},
    // Thumb piano: a tine held at one end.
    {4, {1.0f, 6.27f, 17.55f, 34.39f}, {1.0f, 0.25f, 0.08f, 0.03f}, 2.5f, 0.4f, 0.9f, 0.08f, 1.5f, 1.0f},
    // Music box: the teeth of a steel comb, plucked by a pin.
    {4, {1.0f, 6.27f, 17.55f, 34.39f}, {1.0f, 0.4f, 0.15f, 0.05f}, 2.0f, 0.4f, 0.7f, 0.25f, 0.25f, 1.2f},
    // Steel pan: a hammered note tuned to the octave and the twelfth.
    {5, {1.0f, 2.0f, 3.0f, 4.01f, 5.02f}, {1.0f, 0.3f, 0.15f, 0.08f, 0.04f}, 1.6f, 0.4f, 0.6f, 0.04f, 0.8f, 1.5f},
    // Handpan: the same, wider and rounder, struck with the fingers.
    {4, {1.0f, 2.0f, 3.0f, 4.0f}, {1.0f, 0.45f, 0.25f, 0.06f}, 4.5f, 0.3f, 0.5f, 0.03f, 1.8f, 0.9f},
    // Tongue drum: tongues cut into a steel shell.
    {3, {1.0f, 2.92f, 5.72f}, {1.0f, 0.2f, 0.06f}, 3.0f, 0.3f, 0.7f, 0.03f, 1.6f, 1.0f},
    // Tubular bell: a hanging brass tube. The note heard is an octave below its
    // fourth mode, so the modes are placed around that: the fourth at 2.
    {5, {0.618f, 1.209f, 2.0f, 2.987f, 4.17f}, {0.3f, 0.9f, 1.0f, 0.6f, 0.3f}, 9.0f, 0.2f, 0.5f, 0.06f, 0.5f, 1.0f},
    // Crotale: a small, thick bronze disc, high and long.
    {4, {1.0f, 2.31f, 4.04f, 6.18f}, {1.0f, 0.35f, 0.15f, 0.06f}, 6.0f, 0.2f, 0.6f, 0.08f, 0.3f, 1.1f},
    // Saron: a gamelan's bronze bar over a trough, bright and loose of the series.
    {4, {1.0f, 2.68f, 5.15f, 8.4f}, {1.0f, 0.45f, 0.2f, 0.08f}, 3.5f, 0.3f, 0.6f, 0.05f, 0.7f, 1.1f},
    // Bonang: a gamelan's bronze kettle gong, round, with a boss struck in the middle.
    {5, {1.0f, 1.52f, 2.0f, 2.84f, 3.98f}, {1.0f, 0.3f, 0.45f, 0.15f, 0.08f}, 4.0f, 0.3f, 0.6f, 0.04f, 1.2f, 1.0f},
    // Gong: a big bronze disc, its modes crowded close, ringing for many seconds.
    {6, {1.0f, 1.47f, 1.98f, 2.52f, 3.15f, 3.72f}, {1.0f, 0.6f, 0.45f, 0.3f, 0.2f, 0.12f}, 12.0f, 0.1f, 0.4f, 0.02f, 2.5f, 0.9f},
    // Singing bowl: a thick bowl struck with a padded stick, its partials beating as they ring.
    {4, {1.0f, 2.71f, 5.15f, 8.27f}, {1.0f, 0.5f, 0.2f, 0.08f}, 15.0f, 0.1f, 0.5f, 0.01f, 2.0f, 0.9f},
    // Slit drum: a hollowed log with tongues cut in its slot, warm and short.
    {3, {1.0f, 1.95f, 3.2f}, {1.0f, 0.25f, 0.08f}, 0.6f, 0.4f, 0.9f, 0.1f, 1.4f, 1.3f},
    // Temple block: a hollow wooden block, a dry knock with a pitch.
    {3, {1.0f, 2.3f, 4.1f}, {1.0f, 0.3f, 0.1f}, 0.15f, 0.3f, 1.0f, 0.2f, 0.4f, 3.6f},
    // Cowbell and agogô: a clanking metal bell, two strong modes a fifth or so apart.
    {4, {1.0f, 1.5f, 2.48f, 3.32f}, {1.0f, 0.8f, 0.3f, 0.15f}, 0.6f, 0.2f, 0.7f, 0.15f, 0.3f, 2.2f},
    // Triangle: a bent steel rod, its modes dense and high, ringing.
    {6, {1.0f, 2.03f, 3.05f, 4.09f, 5.15f, 6.24f}, {1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.15f}, 4.0f, 0.1f, 0.4f, 0.1f, 0.2f, 0.8f},
};

/** Samples from one retune to the next: the countdown runs from kRetuneEvery down through 0. */
constexpr int kStep = kRetuneEvery + 1;
/** How far down a mode the other model doesn't have fades by the end of a melt: -60 dB. */
constexpr float kMeltFloor = -6.9077553f;
/** The shortest and longest melt, seconds, at the knob's first step and its top. */
constexpr float kMeltShortest = 0.05f, kMeltLongest = 20.0f;

/** Decay per sample for a ring time. */
float poleFor(float t60, float sampleRate) { return std::pow(10.0f, -3.0f / (std::fmax(t60, 0.005f) * sampleRate)); }

} // namespace

Tine::Tine() { initParams(); }

const ParamDef *Tine::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        // marimba, vibraphone, xylophone, glockenspiel, thumb piano, music box, steel pan, handpan, tongue drum
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // How hard the mallet: soft yarn to bare metal.
        {"mallet", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Where it's struck: the middle to the end.
        {"position", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"decay", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bright", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How much a let-go note is damped.
        {"damp", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tube", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // The discs turning in the tubes, Hz; 0 stops them.
        {"motor", 0.0f, 10.0f, 0.0f, Curve::Linear, 0, "Hz"},
        {"depth", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bloom", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"buzz", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Held notes struck again this many times a second; 0 strikes once.
        {"roll", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "Hz"},
        {"spread", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"voices", 1.0f, static_cast<float>(kVoices), 12.0f, Curve::Stepped, kVoices, ""},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"pair", 0.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"},
    };
    static const ParamDef last[Count - Into] = {
        // The model a note melts into, in the same order as model.
        {"into", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        // How long a struck note takes to melt: 0 off, then 0.05 to 20 s on a log scale.
        {"melt", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // How much velocity and pressure hurry the melt.
        {"heat", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Once, or back and forth.
        {"return", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
    };
    static ParamDef defs[Count];
    static char names[kMacros * MacroParams][12];
    static bool built = false;
    if (!built) {
        for (int32_t i = 0; i < MacroBase; ++i) defs[i] = first[i];
        auto put = [&](int32_t i, const char *n, float mn, float mx, float df, Curve c, int32_t steps) {
            char *name = names[i - MacroBase];
            std::snprintf(name, 12, "%s", n);
            defs[i] = ParamDef{name, mn, mx, df, c, steps, ""};
        };
        for (int k = 0; k < kMacros; ++k) {
            char n[12];
            const int32_t b = MacroBase + k * MacroParams;
            std::snprintf(n, sizeof(n), "mac%d", k + 1);
            put(b, n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0);
            for (int r = 0; r < 4; ++r) {
                std::snprintf(n, sizeof(n), "mac%d_d%d", k + 1, r + 1);
                put(b + 1 + r, n, 0.0f, static_cast<float>(MacroDestCount - 1), 0.0f, Curve::Stepped, MacroDestCount);
                std::snprintf(n, sizeof(n), "mac%d_a%d", k + 1, r + 1);
                put(b + 5 + r, n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0);
            }
        }
        for (int32_t i = Into; i < Count; ++i) defs[i] = last[i - Into];
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Tine::Count];
    DestMap() {
        using T = Tine;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {T::Mallet, T::McMallet}, {T::Position, T::McPosition}, {T::Decay, T::McDecay}, {T::Bright, T::McBright},
            {T::Damp, T::McDamp}, {T::Tube, T::McTube}, {T::Motor, T::McMotor}, {T::Depth, T::McDepth},
            {T::Bloom, T::McBloom}, {T::Buzz, T::McBuzz}, {T::Roll, T::McRoll}, {T::Spread, T::McSpread},
            {T::VelocityAmount, T::McVelocity}, {T::Volume, T::McVolume}, {T::Pair, T::McPair},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Tine::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : (goal ? macGoal_ : mac_)[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Tine::updateMacroGoals() {
    for (float &m : macGoal_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, true);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(read(b + 1 + r, true) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) macGoal_[dest] += knob * read(b + 5 + r, true);
        }
    }
    // Two octaves either way.
    macGoal_[McPitch] = std::clamp(macGoal_[McPitch], -1.0f, 1.0f);
}

void Tine::updateMacros() {
    updateMacroGoals();
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, false);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            // The routes' destinations are steps, so their targets.
            const int dest = static_cast<int>(read(b + 1 + r, true) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) mac_[dest] += knob * read(b + 5 + r, false);
        }
    }
    mac_[McPitch] = std::clamp(mac_[McPitch], -1.0f, 1.0f);
}

void Tine::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        // The softest, longest strike on the lowest note.
        v.strike.assign(static_cast<size_t>(sampleRate * 0.01f) + 4, 0.0f);
        v.tube.assign(static_cast<size_t>(sampleRate / (2.0f * kTubeLowest)) + 4, 0.0f);
    }
    reset();
}

void Tine::reset() {
    for (Voice &v : voices) {
        v.settleLeft = 0;
        // Whole modes, the melt's tuning (cw, sw, re) with them, so what follows doesn't hang on what went before.
        for (Mode &m : v.modes) m = Mode();
        for (Mode &m : v.pairModes) m = Mode();
        v.modeCount = 0;
        v.paired = false;
        v.builtPair = -1.0f;
        v.macAt = v.macNow = 0.0f;
        v.dampBy = 0.0f;
        v.from = v.to = Shape();
        v.canMelt = v.melted = false;
        v.hit = 0.8f;
        v.meltU = v.meltPos = 0.0f;
        v.tunedPos = -1.0f;
        v.tunedPitch = -1000.0f;
        for (float &a : v.lastLogAmp) a = 0.0f;
        for (float &a : v.fedShare) a = 0.0f;
        for (float &a : v.fedLevel) a = 0.0f;
        v.fed = 0;
        v.sinceStep = 0;
        v.breathing = false;
        std::fill(v.tube.begin(), v.tube.end(), 0.0f);
        v.tubeAt = 0;
        v.used = v.held = v.damped = false;
        v.strikeLength = v.strikeAt = 0;
        v.click = v.clickLast = v.buzzLast = 0.0f;
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.bowing = 0.0f;
        v.builtPitch = -1000.0f;
        v.level = 0.0f;
        v.quietBlocks = 0;
    }
    for (float &m : mac_) m = 0.0f;
    for (float &m : macGoal_) m = 0.0f;
    bend = wheel = channelPressure_ = 0.0f;
    dampersUp = false;
    motorPhase = 0.0f;
    retuneCountdown = 0;
    noise = 0x5bd1e995u;
    feedNoise = 0x27d4eb2fu;
    clock = 0;
    quietSamples = 0;
    asleep = true;
}

int Tine::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

float Tine::meltOf() const {
    for (const Voice &v : voices) {
        if (v.used) return v.melted ? v.meltPos : -1.0f;
    }
    return -1.0f;
}

float Tine::modeRatio(int i) const {
    for (const Voice &v : voices) {
        if (v.used) return i < v.modeCount ? v.modes[i].ratio : 0.0f;
    }
    return 0.0f;
}

Tine::Voice *Tine::voiceFor(uint8_t note) {
    const int cap = std::clamp(steppedTargetOf(Voices), 1, kVoices);
    for (int i = 0; i < cap; ++i) {
        if (voices[i].used && voices[i].note == note) return &voices[i];
    }
    for (int i = 0; i < cap; ++i) {
        if (!voices[i].used) return &voices[i];
    }
    Voice *pick = nullptr;
    for (int i = 0; i < cap; ++i) {
        if (!voices[i].held && (pick == nullptr || voices[i].age < pick->age)) pick = &voices[i];
    }
    if (pick != nullptr) return pick;
    pick = &voices[0];
    for (int i = 0; i < cap; ++i) {
        if (voices[i].age < pick->age) pick = &voices[i];
    }
    return pick;
}

void Tine::build(Voice &v) {
    // A melting note keeps every mode it's ringing, so one the models no longer have can ring out.
    const int ringing = v.modeCount;
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float f = noteHz(pitchOf(v));
    const float bright = clampf(goalOf(Bright), 0.0f, 1.0f);
    const float position = clampf(goalOf(Position), 0.0f, 1.0f);
    // Lower notes ring longer: a longer, heavier bar.
    const float t60 = clampf(k.t60 * std::pow(4.0f, goalOf(Decay) - 0.5f) * std::pow(kRefHz / f, k.pitchFall), 0.05f, 40.0f);
    // A bright bar keeps its overtones longer; a dull one loses them first.
    const float modeFall = k.modeFall * (1.5f - bright);
    v.modeCount = 0;
    v.bloomMode = -1;
    for (int i = 0; i < k.modes; ++i) {
        if (f * k.ratio[i] > sampleRate * 0.45f) break;
        Mode &m = v.modes[v.modeCount];
        m.ratio = k.ratio[i];
        m.t60 = t60 * std::pow(1.0f / k.ratio[i], modeFall);
        m.r = poleFor(m.t60, sampleRate);
        // Where it's struck: in the middle a bar's second mode has a node and
        // stays quiet; towards the end the note thins and the overtones come up.
        float where = 1.0f;
        if (i == 0) where = 1.0f - 0.6f * position;
        else if (i % 2 == 1) where = 0.15f + 0.85f * position;
        m.amp = k.amp[i] * where * std::pow(2.0f, (bright - 0.5f) * 2.0f * std::sqrt(static_cast<float>(i)));
        // A bow sets the note going, and a little of the next mode.
        m.bowShare = i == 0 ? 1.0f : (i == 1 ? 0.2f : 0.0f);
        if (std::fabs(k.ratio[i] - 2.0f) < 0.05f) v.bloomMode = v.modeCount;
        ++v.modeCount;
    }
    v.damped = false;
    v.builtPitch = -1000.0f;
    retune(v);
    // The melt: both models as built for this note, and where the blend is.
    const int into = std::clamp(steppedTargetOf(Into), 0, KindCount - 1);
    v.canMelt = into != std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    if (!v.canMelt && !v.melted) return;
    shapeOf(std::clamp(steppedTargetOf(Model), 0, KindCount - 1), f, v.from);
    if (v.canMelt) {
        shapeOf(into, f, v.to);
        // The note's gain has the model's level in it, so the other's shares carry the difference.
        const float level = kMakes[into].level / kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)].level;
        for (int i = 0; i < v.to.count; ++i) {
            v.to.amp[i] *= level;
            v.to.logAmp[i] = std::log(v.to.amp[i]);
        }
    } else {
        // Into is the model now: a melted note struck again goes back to it, each mode carried from where it is.
        v.to = v.from;
    }
    meltReturn(v);
    if (v.melted) {
        // A note struck again while melting is struck where the melt is.
        v.modeCount = std::max({ringing, v.from.count, v.to.count});
        meltTune(v);
    } else if (meltBlend(v, macGoal_[McMelt]) > 0.0f) {
        v.meltPos = meltBlend(v, macGoal_[McMelt]);
        meltBegin(v, true);
    }
}

void Tine::shapeOf(int kind, float f, Shape &s) const {
    // The same sums as build(), kept as logs so the melt can glide between two of them.
    const Make &k = kMakes[kind];
    const float bright = clampf(goalOf(Bright), 0.0f, 1.0f);
    const float position = clampf(goalOf(Position), 0.0f, 1.0f);
    const float t60 = clampf(k.t60 * std::pow(4.0f, goalOf(Decay) - 0.5f) * std::pow(kRefHz / f, k.pitchFall), 0.05f, 40.0f);
    const float modeFall = k.modeFall * (1.5f - bright);
    s.count = 0;
    s.bloom = -1;
    for (int i = 0; i < k.modes; ++i) {
        if (f * k.ratio[i] > sampleRate * 0.45f) break;
        s.logRatio[i] = std::log(k.ratio[i]);
        s.logT60[i] = std::log(t60 * std::pow(1.0f / k.ratio[i], modeFall));
        float where = 1.0f;
        if (i == 0) where = 1.0f - 0.6f * position;
        else if (i % 2 == 1) where = 0.15f + 0.85f * position;
        s.amp[i] = k.amp[i] * where * std::pow(2.0f, (bright - 0.5f) * 2.0f * std::sqrt(static_cast<float>(i)));
        s.logAmp[i] = std::log(s.amp[i]);
        s.bowShare[i] = i == 0 ? 1.0f : (i == 1 ? 0.2f : 0.0f);
        if (std::fabs(k.ratio[i] - 2.0f) < 0.05f) s.bloom = i;
        ++s.count;
    }
    s.click = k.click;
    s.contact = k.contact;
}

float Tine::meltBlend(const Voice &v, float mac) const {
    float base = 0.0f;
    if (v.meltU > 0.0f) {
        if (v.breathing) {
            // Back and forth: a cosine, so it turns round without a corner.
            base = 0.5f - 0.5f * std::cos(3.14159265f * v.meltU);
        } else {
            // Once: eased in and out.
            const float u = std::fmin(v.meltU, 1.0f);
            base = u * u * (3.0f - 2.0f * u);
        }
    }
    return clampf(base + mac, 0.0f, 1.0f);
}

void Tine::meltReturn(Voice &v) const {
    const bool breathe = steppedTargetOf(Return) > 0;
    if (breathe == v.breathing) return;
    if (v.meltU > 0.0f) {
        // The same blend in the other count: the cosine's or the ease's inverse, on the way there.
        const float b = meltBlend(v, 0.0f);
        v.meltU = breathe ? std::acos(clampf(1.0f - 2.0f * b, -1.0f, 1.0f)) / 3.14159265f
                          : 0.5f - std::sin(std::asin(clampf(1.0f - 2.0f * b, -1.0f, 1.0f)) / 3.0f);
    }
    v.breathing = breathe;
}

void Tine::meltBegin(Voice &v, bool struck, int32_t span) {
    v.melted = true;
    const int n = std::max(v.from.count, v.to.count);
    // Modes the note didn't start with start silent.
    for (int i = v.modeCount; i < n; ++i) {
        v.modes[i].y1 = v.modes[i].y2 = 0.0f;
        v.pairModes[i].y1 = v.pairModes[i].y2 = 0.0f;
    }
    v.modeCount = n;
    // Where each ringing mode is tuned now, for carry().
    for (int i = 0; i < n; ++i) {
        for (Mode *m : {&v.modes[i], &v.pairModes[i]}) {
            m->re = std::sqrt(std::fmax(-m->a2, 0.0f));
            m->cw = m->re > 0.0f ? clampf(m->a1 / (2.0f * m->re), -1.0f, 1.0f) : 1.0f;
            m->sw = std::sqrt(std::fmax(0.0f, 1.0f - m->cw * m->cw));
        }
    }
    // The ringing modes are at the model's shares, unless a strike at the blend is about to land.
    const float p = struck ? v.meltPos : 0.0f;
    for (int i = 0; i < n; ++i) {
        const bool inA = i < v.from.count, inB = i < v.to.count;
        float la = 0.0f;
        if (inA && inB) la = v.from.logAmp[i] * (1.0f - p) + v.to.logAmp[i] * p;
        else if (inA) la = v.from.logAmp[i] + kMeltFloor * p;
        v.lastLogAmp[i] = la;
        v.fedShare[i] = v.fedLevel[i] = 0.0f;
    }
    meltTune(v, 0, span);
}

void Tine::carry(Mode &m, float w, float re) {
    const float c = std::cos(w), s = std::sin(w);
    // y1 = A sin(t), r y2 = A sin(t - w): find A cos(t) at the old step and
    // write y2 for the new one, so phase and level go on unchanged.
    if (m.sw > 1e-6f && re > 0.0f) {
        const float ac = (m.y1 * m.cw - m.re * m.y2) / m.sw;
        m.y2 = (m.y1 * c - ac * s) / re;
    }
    m.cw = c;
    m.sw = s;
    m.re = re;
    m.a1 = 2.0f * re * c;
    m.a2 = -re * re;
}

void Tine::meltTune(Voice &v, int32_t samples, int32_t span) {
    // Between blocks the next step is the next block's first.
    if (span < 0) span = retuneCountdown;
    const float glide = static_cast<float>(std::max(span, 1));
    const float p = v.meltPos;
    const float pitch = pitchOf(v);
    const float pair = v.paired ? clampf(paramOf(Pair), 0.0f, 50.0f) : 0.0f;
    if (samples > 0 && p == v.tunedPos && pitch == v.tunedPitch && (!v.paired || pair == v.builtPair)) {
        // Settled: the tuning stands. Levels the last move spread over the pole go
        // back to the plain pole, and the modes being fed follow the note down.
        for (int i = 0; i < v.modeCount; ++i) {
            for (Mode *m : {&v.modes[i], &v.pairModes[i]}) {
                if (m->re == m->r) continue;
                m->re = m->r;
                m->a1 = 2.0f * m->r * m->cw;
                m->a2 = -m->r * m->r;
            }
            if (i < v.from.count || i >= v.to.count || v.fedLevel[i] <= 0.0f) continue;
            Mode &m = v.modes[i];
            const float y2 = m.re * m.y2, sn = std::fmax(m.sw, 1e-4f);
            const float have = std::sqrt(std::fmax(0.0f, m.y1 * m.y1 + y2 * y2 - 2.0f * m.cw * m.y1 * y2)) / sn;
            v.fedLevel[i] = std::fmax(v.fedLevel[i] * std::pow(m.r, static_cast<float>(samples)), have);
            m.feed = 0.05f * m.sw * std::sqrt(6.0f * (1.0f - m.r * m.r)) * v.fedLevel[i];
            if (v.paired) v.pairModes[i].feed = m.feed;
        }
        return;
    }
    v.tunedPos = p;
    v.tunedPitch = pitch;
    v.builtPitch = pitch;
    v.builtPair = pair;
    const float f = noteHz(pitch);
    const int n = v.modeCount;
    float w[kModes], amp[kModes], now[kModes];
    // How loud the note is ringing, per unit of share: from the modes both models have.
    float ringing = 0.0f, unit = 0.0f;
    for (int i = 0; i < n; ++i) {
        const bool inA = i < v.from.count, inB = i < v.to.count;
        Mode &m = v.modes[i];
        // A mode neither model has any more keeps its ratio and rings out.
        if (inA || inB) {
            const float lr = inA && inB ? v.from.logRatio[i] + (v.to.logRatio[i] - v.from.logRatio[i]) * p
                                        : (inA ? v.from.logRatio[i] : v.to.logRatio[i]);
            m.ratio = std::exp(lr);
        }
        w[i] = kTwoPi * std::fmin(f * m.ratio, sampleRate * 0.49f) / sampleRate;
        // How loud it's ringing, from where it is in its cycle.
        const float y2 = m.re * m.y2, sn = std::fmax(m.sw, 1e-4f);
        now[i] = std::fmax(0.0f, m.y1 * m.y1 + y2 * y2 - 2.0f * m.cw * m.y1 * y2) / (sn * sn);
        if (inA && inB) {
            ringing += now[i];
            const float a = std::exp(v.lastLogAmp[i]);
            unit += a * a;
        }
    }
    const float energy = unit > 0.0f ? std::sqrt(ringing / unit) : 0.0f;
    v.fed = 0;
    for (int i = 0; i < n; ++i) {
        const bool inA = i < v.from.count, inB = i < v.to.count;
        Mode &m = v.modes[i];
        if (!inA && !inB) {
            // Left over from a model the note was melting from or into: no share, its own ring time.
            float t60 = m.t60;
            if (v.damped) t60 *= std::pow(std::fmin(1.0f, kDampedT60 / t60), v.dampBy);
            m.r = std::exp(-6.9077553f / (std::fmax(t60, 0.005f) * sampleRate));
            m.amp = m.bowShare = 0.0f;
            v.lastLogAmp[i] = 0.0f;
            v.fedShare[i] = v.fedLevel[i] = 0.0f;
            carry(m, w[i], m.r);
            m.b0 = m.bow = m.feed = 0.0f;
            continue;
        }
        float lt = inA && inB ? v.from.logT60[i] + (v.to.logT60[i] - v.from.logT60[i]) * p
                              : (inA ? v.from.logT60[i] : v.to.logT60[i]);
        m.t60 = std::exp(lt);
        float t60 = m.t60;
        if (v.damped) t60 *= std::pow(std::fmin(1.0f, kDampedT60 / t60), v.dampBy);
        // poleFor(), as an exp: -3 decades over the ring time.
        const float perSample = -6.9077553f / (std::fmax(t60, 0.005f) * sampleRate);
        m.r = std::exp(perSample);
        // Its share of a strike, and how far its ringing has to move to match.
        float scale = 1.0f;
        if (inA) {
            const float la = inB ? v.from.logAmp[i] * (1.0f - p) + v.to.logAmp[i] * p
                                 : v.from.logAmp[i] + kMeltFloor * p;
            amp[i] = std::exp(la);
            // Spread over the step as a change in the pole, so the level glides rather than steps.
            // exp() of at most 2/17, by its series: plenty close, and 1 exactly when nothing moves.
            const float x = clampf(la - v.lastLogAmp[i], -2.0f, 2.0f) / glide;
            scale = 1.0f + x * (1.0f + x * (0.5f + x * (1.0f / 6.0f + x * (1.0f / 24.0f))));
            v.lastLogAmp[i] = la;
        } else {
            // A mode the note didn't start with takes energy from the note as its share
            // grows, and rings that down at its own rate, as if struck by the melt.
            // It's held to that level, grown from the noise that feeds it below.
            amp[i] = v.to.amp[i] * p;
            // What the last strike's mallet would have given a mode this high: a
            // half-sine's spectrum, 1 at the bottom, falling away above 1/length.
            const float fl = w[i] / kTwoPi * static_cast<float>(std::max(v.strikeLength, 2));
            const float d = 1.0f - 4.0f * fl * fl;
            const float mallet = std::fabs(d) < 1e-3f ? 0.7854f : std::fabs(std::cos(3.14159265f * fl) / d);
            if (samples > 0) v.fedLevel[i] *= std::exp(perSample * static_cast<float>(samples));
            if (amp[i] >= v.fedShare[i]) v.fedLevel[i] += (amp[i] - v.fedShare[i]) * mallet * energy;
            else v.fedLevel[i] *= amp[i] / v.fedShare[i];
            v.fedShare[i] = amp[i];
            // Only ever lifted: a strike that rings it louder is kept, and rings down.
            const float have = std::sqrt(now[i]);
            v.fedLevel[i] = std::fmax(v.fedLevel[i], have);
            const float want = v.fedLevel[i];
            if (want > 1e-9f && have > 1e-12f) scale = std::exp(clampf(0.3f * std::log(want / have), 0.0f, 0.5f) / static_cast<float>(kStep));
        }
        m.amp = amp[i];
        m.bowShare = inA && inB ? v.from.bowShare[i] + (v.to.bowShare[i] - v.from.bowShare[i]) * p
                                : (inA ? v.from.bowShare[i] * (1.0f - p) : v.to.bowShare[i] * p);
        carry(m, w[i], m.r * scale);
        const float s = m.sw;
        m.b0 = m.amp * s;
        const float noiseGain = s * std::sqrt(6.0f * (1.0f - m.r * m.r));
        m.bow = m.bowShare * noiseGain;
        // A mode the note didn't start with gets a little noise to grow from, well
        // under its level: more would feed back through fedLevel and run away.
        m.feed = inA ? 0.0f : 0.05f * noiseGain * v.fedLevel[i];
        if (m.feed > 0.0f) ++v.fed;
    }
    // The pan's octave: driven from whichever model's octave mode, as far as the blend has gone.
    const float bloomGain = 1.0f / std::fmax(v.gain, 1e-3f);
    for (int i = 0; i < n; ++i) {
        const float share = (i == v.from.bloom ? 1.0f - p : 0.0f) + (i == v.to.bloom ? p : 0.0f);
        const Mode &m = v.modes[i];
        v.modes[i].bloomK = share > 0.0f ? share * 8.0f * (1.0f - m.r) * m.sw * bloomGain : 0.0f;
    }
    if (v.paired) {
        const float fp = f * std::exp2(pair / 1200.0f);
        for (int i = 0; i < n; ++i) {
            const Mode &m = v.modes[i];
            Mode &c = v.pairModes[i];
            c.b0 = m.b0;
            c.amp = m.amp;
            c.bowShare = m.bowShare;
            c.bow = m.bow;
            c.ratio = m.ratio;
            c.t60 = m.t60;
            c.r = m.r;
            c.feed = m.feed;
            c.bloomK = m.bloomK;
            carry(c, kTwoPi * std::fmin(fp * c.ratio, sampleRate * 0.49f) / sampleRate, m.re);
        }
    }
    // The tube follows the note's first mode as it moves.
    const float first = n > 0 ? v.modes[0].ratio / std::exp(v.from.logRatio[0]) : 1.0f;
    v.tubeDelay = clampf(sampleRate / (2.0f * f * first), 1.0f, static_cast<float>(v.tube.size() - 2));
}

void Tine::retune(Voice &v) {
    const float pitch = pitchOf(v);
    const float pair = v.paired ? clampf(paramOf(Pair), 0.0f, 50.0f) : 0.0f;
    if (std::fabs(pitch - v.builtPitch) < 0.002f && (!v.paired || std::fabs(pair - v.builtPair) < 0.01f)) return;
    v.builtPitch = pitch;
    v.builtPair = pair;
    const float f = noteHz(pitch);
    for (int i = 0; i < v.modeCount; ++i) {
        Mode &m = v.modes[i];
        const float w = kTwoPi * std::fmin(f * m.ratio, sampleRate * 0.49f) / sampleRate;
        const float s = std::sin(w);
        m.a1 = 2.0f * m.r * std::cos(w);
        m.a2 = -m.r * m.r;
        // An impulse of one sets the mode ringing at its share.
        m.b0 = m.amp * s;
        // White noise at a third of a unit of power rings a mode at this
        // level, whatever its decay: the bow's level is the note's.
        m.bow = m.bowShare * s * std::sqrt(6.0f * (1.0f - m.r * m.r));
    }
    // The pair: the same modes, ringing as long, a few cents sharp.
    if (v.paired) {
        const float fp = f * std::exp2(pair / 1200.0f);
        for (int i = 0; i < v.modeCount; ++i) {
            const Mode &m = v.modes[i];
            Mode &c = v.pairModes[i];
            c.ratio = m.ratio;
            c.t60 = m.t60;
            c.r = m.r;
            c.amp = m.amp;
            c.bowShare = m.bowShare;
            const float w = kTwoPi * std::fmin(fp * c.ratio, sampleRate * 0.49f) / sampleRate;
            const float s = std::sin(w);
            c.a1 = 2.0f * c.r * std::cos(w);
            c.a2 = -c.r * c.r;
            c.b0 = c.amp * s;
            c.bow = c.bowShare * s * std::sqrt(6.0f * (1.0f - c.r * c.r));
        }
    }
    // The pan's octave grows from the note: the note squared has an octave in
    // it, which the octave mode picks out. Sized so at full it can grow as
    // loud as the note, however long both ring.
    if (v.bloomMode >= 0) {
        const Mode &m = v.modes[v.bloomMode];
        const float w = kTwoPi * std::fmin(f * m.ratio, sampleRate * 0.49f) / sampleRate;
        v.bloomCoef = 8.0f * (1.0f - m.r) * std::sin(w) / std::fmax(v.gain, 1e-3f);
    }
    // The tube: half a period long, fed back inverted, so it rings at the note and its odd overtones.
    v.tubeDelay = clampf(sampleRate / (2.0f * f), 1.0f, static_cast<float>(v.tube.size() - 2));
}

void Tine::strike(Voice &v, float velocity) {
    // A mallet landing on a bar that's still swinging stops some of it first,
    // so a roll builds to a level rather than without end.
    // Over about 3 ms, not at once: a step in a ringing mode is a click. Only
    // when something's ringing, so a fresh note isn't kept waiting.
    bool ringing = false;
    for (int m = 0; m < v.modeCount; ++m) ringing = ringing || std::fabs(v.modes[m].y1) > 1e-6f;
    v.settleLeft = ringing ? std::max(1, static_cast<int32_t>(kSettleSeconds * sampleRate)) : 0;
    v.settle = ringing ? std::pow(kRestrikeKeeps, 1.0f / static_cast<float>(v.settleLeft)) : 1.0f;
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    const float f = noteHz(pitchOf(v));
    // The mallet's contact: a soft one stays on the bar for milliseconds and
    // can't set the overtones going; a hard one is gone before they start.
    // Struck harder, a mallet flattens less and leaves sooner. Never longer
    // than half the note's period, or it would choke the note itself.
    const float hard = clampf(goalOf(Mallet) + 0.4f * (velocity - 0.6f), 0.0f, 1.0f);
    // A melting note is struck as the blend: its knock and contact between the two.
    const float contact = v.melted ? v.from.contact + (v.to.contact - v.from.contact) * v.meltPos : k.contact;
    float seconds = contact * (0.0045f - 0.0041f * hard);
    seconds = std::fmin(seconds, 0.5f / f);
    const int len = std::clamp(static_cast<int>(seconds * sampleRate), 2, static_cast<int>(v.strike.size()));
    // A half-sine of unit area: the low modes get the same push whatever the mallet.
    const float peak = 3.14159265f / (2.0f * static_cast<float>(len));
    for (int i = 0; i < len; ++i) {
        v.strike[static_cast<size_t>(i)] = peak * std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(len));
    }
    v.strikeLength = len;
    v.strikeAt = 0;
    v.velocity = velocity;
    // The knock of the mallet, the thumb or the pin.
    const float click = v.melted ? v.from.click + (v.to.click - v.from.click) * v.meltPos : k.click;
    v.click = click * velocity * (0.3f + hard);
}

float Tine::ring(Mode *modes, const Voice &v, float x, float bow, float bloom) {
    float y = 0.0f, first = 0.0f;
    for (int m = 0; m < v.modeCount; ++m) {
        Mode &md = modes[m];
        float in = md.b0 * x + md.bow * bow;
        if (m == v.bloomMode && bloom > 0.0f) {
            // Limited to the note's struck level, so a bowed or rolled note can't run away with it.
            const float n = clampf(first, -v.gain, v.gain);
            in += bloom * v.bloomCoef * n * n;
        }
        const float out = in + md.a1 * md.y1 + md.a2 * md.y2;
        md.y2 = md.y1;
        md.y1 = out;
        if (m == 0) first = out;
        y += out;
    }
    return y;
}

float Tine::ringMelt(Mode *modes, const Voice &v, float x, float bow, float bloom, float nz) {
    float y = 0.0f, first = 0.0f;
    for (int m = 0; m < v.modeCount; ++m) {
        Mode &md = modes[m];
        float in = md.b0 * x + md.bow * bow + md.feed * nz;
        if (md.bloomK != 0.0f && bloom > 0.0f) {
            const float n = clampf(first, -v.gain, v.gain);
            in += bloom * md.bloomK * n * n;
        }
        const float out = in + md.a1 * md.y1 + md.a2 * md.y2;
        md.y2 = md.y1;
        md.y1 = out;
        if (m == 0) first = out;
        y += out;
    }
    return y;
}

void Tine::damp(Voice &v) {
    const float damp = clampf(paramOf(Damp), 0.0f, 1.0f);
    if (damp <= 0.0f || v.damped) return;
    v.damped = true;
    v.dampBy = damp;
    if (v.melted) {
        meltTune(v);
        return;
    }
    for (int i = 0; i < v.modeCount; ++i) {
        Mode &m = v.modes[i];
        const float t60 = m.t60 * std::pow(std::fmin(1.0f, kDampedT60 / m.t60), damp);
        m.r = poleFor(t60, sampleRate);
    }
    v.builtPitch = -1000.0f;
    retune(v);
}

void Tine::noteOn(uint8_t note, uint8_t velocity) {
    updateMacroGoals(); // a note reads where the knobs are going, so renders repeat
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    Voice *v = voiceFor(note);
    const bool fresh = !v->used || v->note != note;
    v->note = note;
    v->pitch = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    // The pitch macro where it's going is in the note; a move after it bends it.
    v->macAt = v->macNow = macGoal_[McPitch];
    if (v->macAt != 0.0f) v->pitch += v->macAt * 24.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    const float vel = static_cast<float>(velocity) / 127.0f;
    v->gain = kHouse * k.level * velocityGain(vel, goalOf(VelocityAmount));
    if (fresh) {
        for (Mode &m : v->modes) m.y1 = m.y2 = 0.0f;
        std::fill(v->tube.begin(), v->tube.end(), 0.0f);
        v->bowing = 0.0f;
        v->paired = false;
        v->melted = false;
        v->meltU = v->meltPos = 0.0f;
        v->sinceStep = 0;
        v->breathing = steppedTargetOf(Return) > 0;
    }
    v->hit = vel;
    // A pair starts where the note is, so a ringing note struck again with
    // pair on goes on without a step. Once paired a note stays so.
    if (!v->paired && goalOf(Pair) > 0.0f) {
        v->paired = true;
        for (int i = 0; i < kModes; ++i) {
            Mode &c = v->pairModes[i];
            const Mode &m = v->modes[i];
            c.y1 = m.y1;
            c.y2 = m.y2;
            // The melt's tuning too, so a melting note's pair carries on from the same swing.
            c.cw = m.cw;
            c.sw = m.sw;
            c.re = m.re;
        }
    }
    // A note struck again rings on from where it was, as a bar does.
    build(*v);
    strike(*v, vel);
    const float spread = clampf(goalOf(Spread), 0.0f, 1.0f);
    v->pan = spread * clampf((v->pitch - 66.0f) / 30.0f, -1.0f, 1.0f);
    const float roll = goalOf(Roll);
    v->rollLeft = roll > 0.0f ? static_cast<int32_t>(sampleRate / roll) : 0;
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Tine::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.held = false;
            if (!dampersUp) damp(v);
        }
    }
}

void Tine::allNotesOff() {
    for (Voice &v : voices) {
        if (!v.used) continue;
        v.held = false;
        damp(v);
    }
}

void Tine::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel turns the discs further.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Tine::setDampers(bool lifted) {
    dampersUp = lifted;
    if (lifted) return;
    for (Voice &v : voices) {
        if (v.used && !v.held) damp(v);
    }
}

void Tine::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Tine::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Tine::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Tine::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

bool Tine::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const float tube = clampf(paramOf(Tube), 0.0f, 1.0f);
    const float motorHz = paramOf(Motor);
    const float motorDepth = motorHz > 0.0f ? clampf(paramOf(Depth) + wheel, 0.0f, 1.0f) : 0.0f;
    const float bloom = clampf(paramOf(Bloom), 0.0f, 1.0f);
    const float buzz = clampf(paramOf(Buzz), 0.0f, 1.0f);
    const float roll = paramOf(Roll);
    const float volume = paramOf(Volume);
    // The melt: how long one takes, how much a hit or pressure hurries it, and the macro's place.
    const float meltKnob = clampf(paramOf(Melt), 0.0f, 1.0f);
    // Per sample: a step moves the melt by the samples since the last.
    const float meltRate = meltKnob > 0.0f ? 1.0f / (sampleRate * kMeltShortest * std::pow(kMeltLongest / kMeltShortest, meltKnob)) : 0.0f;
    const float heat = clampf(paramOf(Heat), 0.0f, 1.0f);
    const float meltMac = mac_[McMelt];
    // Where the first step of the next block falls, as set below, so a melt step knows how long it has.
    int nextCountdown = retuneCountdown - frames;
    while (nextCountdown < 0) nextCountdown += kRetuneEvery;
    const float clickFall = std::exp(-1.0f / (0.0015f * sampleRate));
    const auto tubeSize = static_cast<int32_t>(voices[0].tube.size());
    bool any = false;

    for (Voice &v : voices) {
        if (!v.used) continue;
        v.macNow = mac_[McPitch];
        float peak = 0.0f;
        const float pl = std::sqrt(0.5f * (1.0f - v.pan)), pr = std::sqrt(0.5f * (1.0f + v.pan));
        float phase = motorPhase;
        const float phaseStep = motorHz / sampleRate;
        int countdown = retuneCountdown;
        // The last step, counted from this block's start.
        int32_t lastStep = -v.sinceStep;
        for (int32_t i = 0; i < frames; ++i) {
            if (countdown-- <= 0) {
                countdown = kRetuneEvery;
                // The samples since the last step, and to the next: the cadence isn't even across blocks.
                const int32_t since = i - lastStep;
                const int32_t span = i + kStep < frames ? kStep : frames - i + nextCountdown;
                lastStep = i;
                if (v.canMelt) {
                    meltReturn(v);
                    // Hard hits and pressed notes melt faster: velocity 76 or so is the knob's own time.
                    if (meltRate > 0.0f) {
                        const float speed = heat > 0.0f ? std::exp2(heat * (4.0f * (v.hit - 0.6f) + 3.0f * v.bowing)) : 1.0f;
                        v.meltU += meltRate * speed * static_cast<float>(since);
                        if (v.breathing) {
                            if (v.meltU >= 2.0f) v.meltU -= 2.0f;
                        } else if (v.meltU > 1.0f) {
                            v.meltU = 1.0f;
                        }
                    }
                    const float pos = meltBlend(v, meltMac);
                    if (!v.melted && pos > 0.0f) {
                        v.meltPos = pos;
                        meltBegin(v, false, span);
                    } else if (v.melted) {
                        v.meltPos = pos;
                        meltTune(v, since, span);
                    }
                } else if (v.melted) {
                    // Into set back to the model: it still follows the pitch, and its leftovers ring out.
                    meltTune(v, since, span);
                }
                if (!v.melted) retune(v);
                // A bow drawn under pressure, eased in and out.
                const float press = v.held ? (v.pressure >= 0.0f ? v.pressure : channelPressure_) : 0.0f;
                v.bowing += (press - v.bowing) * 0.02f;
                if (v.bowing < 1e-5f && press == 0.0f) v.bowing = 0.0f;
            }
            // Rolls: a held note struck again and again, never quite evenly.
            if (v.held && roll > 0.0f && --v.rollLeft <= 0) {
                strike(v, clampf(v.velocity * (0.8f + 0.15f * white()), 0.05f, 1.0f));
                v.rollLeft = static_cast<int32_t>(sampleRate / roll * (1.0f + 0.08f * white()));
            }
            float x = 0.0f;
            // A re-struck note waits for what was ringing to be damped, then lands whole.
            if (v.settleLeft == 0 && v.strikeAt < v.strikeLength) x = v.strike[static_cast<size_t>(v.strikeAt++)] * v.gain;
            const float bow = v.bowing > 0.0f ? v.bowing * v.gain * white() : 0.0f;
            if (v.settleLeft > 0) {
                --v.settleLeft;
                for (int m = 0; m < v.modeCount; ++m) {
                    v.modes[m].y1 *= v.settle;
                    v.modes[m].y2 *= v.settle;
                }
                if (v.paired) {
                    for (int m = 0; m < v.modeCount; ++m) {
                        v.pairModes[m].y1 *= v.settle;
                        v.pairModes[m].y2 *= v.settle;
                    }
                }
            }
            float y;
            if (v.melted) {
                const float nz = v.fed > 0 ? feedWhite() : 0.0f;
                y = ringMelt(v.modes, v, x, bow, bloom, nz);
                if (v.paired) y = 0.5f * (y + ringMelt(v.pairModes, v, x, bow, bloom, nz));
            } else {
                y = ring(v.modes, v, x, bow, bloom);
                // Half each, so a pair at no offset is the note alone.
                if (v.paired) y = 0.5f * (y + ring(v.pairModes, v, x, bow, bloom));
            }
            // The tube under the bar, and the discs turning in it.
            if (tube > 0.0f) {
                float read = static_cast<float>(v.tubeAt) - v.tubeDelay;
                if (read < 0.0f) read += static_cast<float>(tubeSize);
                const auto i0 = static_cast<int32_t>(read);
                const float frac = read - static_cast<float>(i0);
                const int32_t i1 = i0 + 1 >= tubeSize ? 0 : i0 + 1;
                const float back = v.tube[static_cast<size_t>(i0)] + frac * (v.tube[static_cast<size_t>(i1)] - v.tube[static_cast<size_t>(i0)]);
                const float c = y - kTubeFeedback * back;
                v.tube[static_cast<size_t>(v.tubeAt)] = c;
                if (++v.tubeAt >= tubeSize) v.tubeAt = 0;
                float open = 1.0f;
                if (motorDepth > 0.0f) open = 1.0f - motorDepth * (0.5f - 0.5f * std::cos(kTwoPi * phase));
                y += tube * open * (1.0f - kTubeFeedback) * c;
            }
            phase += phaseStep;
            if (phase >= 1.0f) phase -= 1.0f;
            // A rattle that touches the tine when it swings wide.
            if (buzz > 0.0f) {
                // It only touches past a point, and rattles no louder however wide the swing.
                const float touch = clampf((std::fabs(y) - 0.1f * v.gain) / (0.3f * v.gain), 0.0f, 1.0f);
                const float r = buzz * white() * touch * 0.08f * v.gain;
                y += r - v.buzzLast;
                v.buzzLast = r;
            }
            if (v.click > 1e-6f) {
                const float n = white() * v.click;
                y += n - v.clickLast;
                v.clickLast = n;
                v.click *= clickFall;
            }
            y *= volume;
            L[i] += y * pl;
            R[i] += y * pr;
            peak = std::fmax(peak, std::fabs(y));
        }
        v.sinceStep = frames - lastStep;
        v.level = peak;
        const bool keeps = v.held && (roll > 0.0f || v.bowing > 0.0f);
        if (peak < kSilent && v.strikeAt >= v.strikeLength && !keeps) {
            if (++v.quietBlocks > 4) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
        any = any || v.used;
    }
    retuneCountdown -= frames;
    while (retuneCountdown < 0) retuneCountdown += kRetuneEvery;
    motorPhase += motorHz / sampleRate * static_cast<float>(frames);
    motorPhase -= std::floor(motorPhase);

    if (!any) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return true;
}

} // namespace acidulous::machine
