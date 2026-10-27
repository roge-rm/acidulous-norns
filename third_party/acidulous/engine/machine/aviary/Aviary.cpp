#include "Aviary.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a bird at velocity 100, set so the bank sits with the other machines. */
constexpr float kHouse = 0.22f;
constexpr float kSilent = 1e-4f;
constexpr float kTwoPi = 6.28318530718f;
/** A bird sings this many semitones above the key played. */
constexpr float kAbove = 24.0f;
/** How quickly the folds start and stop swinging, per second of pressure past the threshold. */
constexpr float kOnset = 1400.0f;
/** The pressure when a bird stops blowing: below the threshold, so the folds come to rest. */
constexpr float kRest = -0.5f;
/** Samples between pitch updates. */
constexpr int kRetuneEvery = 16;
/** The trachea's echo: how far back, seconds, and how much comes back (inverted, an open end). */
constexpr float kTracheaSeconds = 0.00025f, kTracheaEcho = -0.4f;
/** Syllables a beat, by the rate knob. */
constexpr float kRates[5] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
/** A whistle's slide into its note, seconds. */
constexpr float kWhistleGlide = 0.035f;
/** A coo's or caw's swell in and fade out, as parts of the note sung. */
constexpr float kCooIn = 0.25f, kCooOut = 0.4f, kCawIn = 0.04f, kCawOut = 0.25f;
/** A caw's pitch shakes by this much, as a part of the note, at full rasp. */
constexpr float kCawJitter = 0.025f;
/** A warble's syllable shapes: a fast fall, a fast rise, an arch, a short
 *  held note, a trill (one quick fall, sung many times), a buzz and a slur
 *  from one pitch to another. */
enum WarbleShape { Fall = 0, Rise, Arch, Note, TrillNote, Buzz, Slur, ShapeCount };
/** Each shape's length in syllable units: a trill note is short, a buzz long. */
constexpr float kShapeLength[ShapeCount] = {0.6f, 0.6f, 1.0f, 1.2f, 0.3f, 1.5f, 0.9f};
/** The shortest warble syllable, seconds: a quick trill note at the top rate and a fast tempo is still a note, not a click. */
constexpr float kSyllableFloor = 0.005f;
/** A buzz's pitch shakes this far, semitones, many times a second. */
constexpr float kBuzzDepth = 0.7f;
/** How much quieter and duller the furthest bird of a flock is. */
constexpr float kFarLevel = 0.65f, kFarDull = 0.6f;
/** A warble's syllables are short and swell in and out, so they're sung this much louder to sit with the other songs. */
constexpr float kWarbleLift = 1.4f;
/** A chorus's birds mostly take turns rather than sing over each other, so each is this much louder. */
constexpr float kChorusLift = 1.6f;

} // namespace

Aviary::Aviary() { initParams(); }

const ParamDef *Aviary::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        {"pattern", 0.0f, static_cast<float>(SongCount - 1), 1.0f, Curve::Stepped, SongCount, ""}, // whistle, chirp, trill, warble, call, chorus, coo, caw
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"rate", 0.0f, 4.0f, 2.0f, Curve::Stepped, 5, ""}, // 1, 2, 4, 8, 16 a beat
        // How much of each syllable's slot it sings.
        {"length", 0.05f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How far each syllable sweeps, semitones: falling from above, or rising from below.
        {"sweep", -24.0f, 24.0f, 7.0f, Curve::Linear, 0, "st"},
        {"rasp", 0.0f, 1.0f, 0.1f, Curve::Linear, 0, ""},
        // The syrinx's other side, singing at once.
        {"two", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"interval", -12.0f, 12.0f, 7.0f, Curve::Linear, 0, "st"},
        {"breath", 0.0f, 1.0f, 0.1f, Curve::Linear, 0, ""},
        {"throat", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"beak", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"spread", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How far away: further is quieter and duller.
        {"space", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"velocity", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // Kit mode, and the default kit: whistles, chirps, trills, warbles, calls and choruses.
        {"kit", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"p01_song", 0.0f, static_cast<float>(SongCount - 1), 0.0f, Curve::Stepped, SongCount, ""},
        {"p01_note", 24.0f, 96.0f, 60.0f, Curve::Stepped, 73, ""},
        {"p01_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p01_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p02_song", 0.0f, static_cast<float>(SongCount - 1), 0.0f, Curve::Stepped, SongCount, ""},
        {"p02_note", 24.0f, 96.0f, 67.0f, Curve::Stepped, 73, ""},
        {"p02_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p02_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p03_song", 0.0f, static_cast<float>(SongCount - 1), 1.0f, Curve::Stepped, SongCount, ""},
        {"p03_note", 24.0f, 96.0f, 64.0f, Curve::Stepped, 73, ""},
        {"p03_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p03_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p04_song", 0.0f, static_cast<float>(SongCount - 1), 1.0f, Curve::Stepped, SongCount, ""},
        {"p04_note", 24.0f, 96.0f, 72.0f, Curve::Stepped, 73, ""},
        {"p04_flock", 1.0f, static_cast<float>(kBirds), 2.0f, Curve::Stepped, kBirds, ""},
        {"p04_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p05_song", 0.0f, static_cast<float>(SongCount - 1), 2.0f, Curve::Stepped, SongCount, ""},
        {"p05_note", 24.0f, 96.0f, 62.0f, Curve::Stepped, 73, ""},
        {"p05_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p05_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p06_song", 0.0f, static_cast<float>(SongCount - 1), 2.0f, Curve::Stepped, SongCount, ""},
        {"p06_note", 24.0f, 96.0f, 69.0f, Curve::Stepped, 73, ""},
        {"p06_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p06_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p07_song", 0.0f, static_cast<float>(SongCount - 1), 3.0f, Curve::Stepped, SongCount, ""},
        {"p07_note", 24.0f, 96.0f, 60.0f, Curve::Stepped, 73, ""},
        {"p07_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p07_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p08_song", 0.0f, static_cast<float>(SongCount - 1), 3.0f, Curve::Stepped, SongCount, ""},
        {"p08_note", 24.0f, 96.0f, 65.0f, Curve::Stepped, 73, ""},
        {"p08_flock", 1.0f, static_cast<float>(kBirds), 2.0f, Curve::Stepped, kBirds, ""},
        {"p08_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p09_song", 0.0f, static_cast<float>(SongCount - 1), 4.0f, Curve::Stepped, SongCount, ""},
        {"p09_note", 24.0f, 96.0f, 55.0f, Curve::Stepped, 73, ""},
        {"p09_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p09_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p10_song", 0.0f, static_cast<float>(SongCount - 1), 4.0f, Curve::Stepped, SongCount, ""},
        {"p10_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p10_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p10_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p11_song", 0.0f, static_cast<float>(SongCount - 1), 4.0f, Curve::Stepped, SongCount, ""},
        {"p11_note", 24.0f, 96.0f, 60.0f, Curve::Stepped, 73, ""},
        {"p11_flock", 1.0f, static_cast<float>(kBirds), 2.0f, Curve::Stepped, kBirds, ""},
        {"p11_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p12_song", 0.0f, static_cast<float>(SongCount - 1), 5.0f, Curve::Stepped, SongCount, ""},
        {"p12_note", 24.0f, 96.0f, 60.0f, Curve::Stepped, 73, ""},
        {"p12_flock", 1.0f, static_cast<float>(kBirds), 3.0f, Curve::Stepped, kBirds, ""},
        {"p12_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p13_song", 0.0f, static_cast<float>(SongCount - 1), 5.0f, Curve::Stepped, SongCount, ""},
        {"p13_note", 24.0f, 96.0f, 64.0f, Curve::Stepped, 73, ""},
        {"p13_flock", 1.0f, static_cast<float>(kBirds), 4.0f, Curve::Stepped, kBirds, ""},
        {"p13_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p14_song", 0.0f, static_cast<float>(SongCount - 1), 1.0f, Curve::Stepped, SongCount, ""},
        {"p14_note", 24.0f, 96.0f, 76.0f, Curve::Stepped, 73, ""},
        {"p14_flock", 1.0f, static_cast<float>(kBirds), 3.0f, Curve::Stepped, kBirds, ""},
        {"p14_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p15_song", 0.0f, static_cast<float>(SongCount - 1), 2.0f, Curve::Stepped, SongCount, ""},
        {"p15_note", 24.0f, 96.0f, 74.0f, Curve::Stepped, 73, ""},
        {"p15_flock", 1.0f, static_cast<float>(kBirds), 2.0f, Curve::Stepped, kBirds, ""},
        {"p15_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p16_song", 0.0f, static_cast<float>(SongCount - 1), 0.0f, Curve::Stepped, SongCount, ""},
        {"p16_note", 24.0f, 96.0f, 52.0f, Curve::Stepped, 73, ""},
        {"p16_flock", 1.0f, static_cast<float>(kBirds), 1.0f, Curve::Stepped, kBirds, ""},
        {"p16_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
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
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Aviary::Count];
    DestMap() {
        using P = Aviary;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {P::Length, P::McLength}, {P::Sweep, P::McSweep}, {P::Rasp, P::McRasp}, {P::Two, P::McTwo},
            {P::Interval, P::McInterval}, {P::Breath, P::McBreath}, {P::Throat, P::McThroat}, {P::Beak, P::McBeak},
            {P::Spread, P::McSpread}, {P::Space, P::McSpace}, {P::VelocityAmount, P::McVelocity},
            {P::Volume, P::McVolume},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Aviary::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : (goal ? macGoal_ : mac_)[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Aviary::updateMacroGoals() {
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

void Aviary::updateMacros() {
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

void Aviary::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        for (Bird &b : v.birds) b.throat.setSampleRate(sampleRate);
    }
    reset();
}

void Aviary::reset() {
    for (Voice &v : voices) {
        for (Bird &b : v.birds) {
            b.sides[0] = b.sides[1] = Side{};
            b.at = 0;
            b.slot = 1;
            b.syllable = 0;
            b.from = b.to = b.pitch = 0.0f;
            b.singing = false;
            b.throat.reset();
            b.beak = 0.0f;
            std::fill(std::begin(b.trachea), std::end(b.trachea), 0.0f);
            b.tracheaAt = 0;
            b.builtHz = -1.0f;
            b.step0 = b.step1 = 0.0f;
            b.beakCoef = 0.5f;
            b.retune = true;
            b.sung = 1.0f;
            b.arch = 0.0f;
            b.easeOut = false;
            b.phraseAt = b.phraseLen = 0;
            b.sub = 0.0f;
            b.far = 0.0f;
            b.shape = b.motifLeft = b.curve = 0;
            b.centre = b.depth = b.buzz = 0.0f;
            b.motifLen = b.loud = 1.0f;
            b.inPhrase = b.shy = false;
        }
        v.used = v.held = false;
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.level = 0.0f;
        v.quietBlocks = 0;
    }
    bpm = 120.0f;
    bend = wheel = channelPressure_ = 0.0f;
    spaceLp[0] = spaceLp[1] = 0.0f;
    for (float &m : mac_) m = 0.0f;
    for (float &m : macGoal_) m = 0.0f;
    clock = 0;
    retuneClock = 0;
    for (int vi = 0; vi < kVoices; ++vi) {
        for (int bi = 0; bi < kBirds; ++bi) voices[vi].birds[bi].noise = 0x1f123bb5u + 0x9e3779b9u * static_cast<uint32_t>(vi * kBirds + bi);
    }
    quietSamples = 0;
    asleep = true;
}

void Aviary::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

int Aviary::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Aviary::Voice *Aviary::voiceFor(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.note == note) return &v;
    }
    for (Voice &v : voices) {
        if (!v.used) return &v;
    }
    Voice *pick = &voices[0];
    for (Voice &v : voices) {
        if (v.age < pick->age) pick = &v;
    }
    return pick;
}

void Aviary::nextSyllable(Voice &v, Bird &b, int index) {
    const float perBeat = kRates[std::clamp(steppedTargetOf(Rate), 0, 4)];
    const float beat = 60.0f / bpm * sampleRate;
    const float sweep = goalOf(Sweep) * (1.0f + wheel);
    const bool first = b.syllable == 0;
    ++b.syllable;
    b.singing = v.held;
    switch (b.song) {
    case Whistle:
        // One long note, sliding up into it at first.
        b.slot = static_cast<int32_t>(beat * 4.0f);
        b.from = first ? -std::fabs(sweep) * 0.5f : b.pitch;
        b.to = 0.0f;
        break;
    case Trill:
        if (b.shy) {
            // In a chorus: a trill of eight to twenty notes, then a long pause.
            if (first || b.phraseLen == 0) {
                b.phraseLen = first && index > 0 ? 0 : 8 + static_cast<int>(rand01(b.seed) * 12.0f);
                b.phraseAt = 0;
            }
            if (b.phraseAt >= b.phraseLen) {
                b.singing = false;
                b.inPhrase = false;
                b.slot = first ? static_cast<int32_t>(beat * (0.5f + 3.0f * rand01(b.seed)))
                               : flockPause(v, b, index, beat, 2.0f, 7.0f);
                b.from = b.to = b.pitch;
                b.phraseLen = 0;
                break;
            }
            b.inPhrase = true;
            ++b.phraseAt;
            b.slot = std::max(static_cast<int32_t>(0.03f * sampleRate), static_cast<int32_t>(beat / (perBeat * 2.0f) * b.drift));
            b.from = sweep * 0.3f;
            b.to = 0.0f;
            break;
        }
        b.slot = std::max(static_cast<int32_t>(0.015f * sampleRate), static_cast<int32_t>(beat / (perBeat * 4.0f) * b.drift));
        b.from = sweep * 0.3f;
        b.to = 0.0f;
        break;
    case Warble:
        nextWarble(v, b, index, beat / perBeat * b.drift, sweep, first);
        if (b.singing) b.singing = v.held;
        break;
    case Call: {
        // Two notes, the second a third lower; the rest of the flock answers in the gaps.
        b.slot = static_cast<int32_t>(beat / perBeat);
        const int which = (b.syllable - 1 + index) % 2;
        // Each note droops half a semitone, mostly heard near its end, so it starts a little above.
        b.from = (which == 0 ? 0.0f : -4.0f) + 0.4f;
        b.to = b.from - 0.5f;
        if (index > 0 && first) {
            // An answering bird waits its turn.
            b.singing = false;
            b.slot = static_cast<int32_t>(beat / perBeat) * index;
        }
        break;
    }
    case Coo:
    case Caw: {
        // Coo: three to five slow notes, each lower than the last, the first
        // scooping up into the note and the last held and falling; then a rest.
        // Caw: two or three short harsh calls, each rising a little then
        // falling and each a touch lower; then a longer rest.
        const bool coo = b.song == Coo;
        const float length = clampf(goalOf(Length), 0.05f, 1.0f);
        const float fall = std::fabs(sweep);
        const int32_t unit = static_cast<int32_t>(beat / perBeat * 2.0f * b.drift);
        if (first) b.phraseAt = b.phraseLen = 0;
        if (index > 0 && first) {
            // The rest of the flock comes in a phrase later, one after another;
            // a flock of caws comes in when it likes.
            b.singing = false;
            b.slot = b.shy ? static_cast<int32_t>(unit * (1.0f + 5.0f * rand01(b.seed)))
                           : unit * (coo ? 3 : 2) * index;
            b.from = b.to = b.pitch;
            b.arch = 0.0f;
            break;
        }
        if (b.phraseLen == 0) {
            b.phraseLen = coo ? 3 + static_cast<int>((rand(b.seed) + 1.0f) * 1.5f) % 3
                              : 2 + static_cast<int>((rand(b.seed) + 1.0f)) % 2;
            // A rook in a flock calls once to four times.
            if (b.shy) b.phraseLen = 1 + static_cast<int>(rand01(b.seed) * 4.0f) % 4;
        }
        const int k = b.phraseAt++;
        b.arch = 0.0f;
        b.easeOut = false;
        if (k >= b.phraseLen) {
            // The rest between phrases; in a flock each bird's is its own.
            b.singing = false;
            b.slot = b.shy ? flockPause(v, b, index, unit / 2.0f, 3.0f, 12.0f) : unit * (coo ? 2 : 3);
            b.from = b.to = b.pitch;
            b.phraseAt = b.phraseLen = 0;
            break;
        }
        const bool last = k == b.phraseLen - 1;
        if (coo) {
            const float level = -0.25f * fall * static_cast<float>(k);
            b.slot = last ? unit * 3 : unit;
            b.sung = static_cast<float>(b.slot) * (last ? 0.6f + 0.35f * length : 0.45f + 0.5f * length);
            if (k == 0) {
                b.from = level - 0.3f * fall;
                b.to = level;
                b.easeOut = true;
            } else {
                b.from = level + 0.08f * fall;
                b.to = level - (last ? 0.3f : 0.12f) * fall;
            }
        } else {
            const float level = -0.5f * static_cast<float>(k);
            // A caw in a flock comes when it likes, not on the beat.
            b.slot = b.shy ? static_cast<int32_t>(static_cast<float>(unit) * (0.8f + 0.5f * rand01(b.seed))) : unit;
            b.sung = static_cast<float>(b.slot) * (0.35f + 0.5f * length);
            b.from = level;
            b.to = level - 0.5f * sweep;
            b.arch = 0.3f * sweep;
        }
        break;
    }
    default: // Chirp
        if (b.shy) {
            // In a chorus: two to six chips, then a pause.
            if (first || b.phraseLen == 0) {
                b.phraseLen = first && index > 0 ? 0 : 2 + static_cast<int>(rand01(b.seed) * 5.0f);
                b.phraseAt = 0;
            }
            if (b.phraseAt >= b.phraseLen) {
                b.singing = false;
                b.inPhrase = false;
                b.slot = first ? static_cast<int32_t>(beat * (0.5f + 3.0f * rand01(b.seed)))
                               : flockPause(v, b, index, beat, 1.5f, 5.0f);
                b.from = b.to = b.pitch;
                b.phraseLen = 0;
                break;
            }
            b.inPhrase = true;
            ++b.phraseAt;
            b.slot = static_cast<int32_t>(beat / perBeat * 2.0f * b.drift * (0.8f + 0.5f * rand01(b.seed)));
            b.from = sweep * (0.7f + 0.5f * rand01(b.seed));
            b.to = 0.0f;
            break;
        }
        b.slot = static_cast<int32_t>(beat / perBeat * b.drift);
        b.from = sweep;
        b.to = 0.0f;
        break;
    }
    b.slot = std::max(b.slot, 16);
    b.at = 0;
}

int32_t Aviary::flockPause(const Voice &v, Bird &b, int index, float beat, float least, float most) {
    float pause = beat * (least + (most - least) * rand01(b.seed));
    // Birds mostly take turns: if another is mid-phrase, wait a little longer.
    for (int j = 0; j < v.birdCount; ++j) {
        if (j != index && v.birds[j].inPhrase && v.birds[j].singing) {
            pause += beat * (0.5f + 1.5f * rand01(b.seed));
            break;
        }
    }
    return static_cast<int32_t>(pause);
}

void Aviary::nextWarble(Voice &v, Bird &b, int index, float unit, float sweep, bool first) {
    const float length = clampf(goalOf(Length), 0.05f, 1.0f);
    const float width = std::fmax(std::fabs(sweep), 1.0f);
    b.buzz = 0.0f;
    b.arch = 0.0f;
    if (first) {
        b.inPhrase = false;
        b.phraseLen = b.motifLeft = 0;
        if (index > 0) {
            // The rest of the flock comes in when it likes.
            b.singing = false;
            b.slot = static_cast<int32_t>(unit * (2.0f + 10.0f * rand01(b.seed)));
            b.from = b.to = b.pitch;
            return;
        }
    }
    if (b.inPhrase && b.motifLeft <= 0 && b.phraseLen <= 0) {
        // The phrase is over: a pause, longer for a shorter length, and longer in a flock.
        b.inPhrase = false;
        b.singing = false;
        const float pause = unit * (2.0f + 16.0f * (1.0f - length)) * (0.5f + rand01(b.seed));
        b.slot = b.shy ? std::max(static_cast<int32_t>(pause), flockPause(v, b, index, 60.0f / bpm * sampleRate, 1.5f, 5.0f))
                       : static_cast<int32_t>(pause);
        b.from = b.to = b.pitch;
        return;
    }
    if (!b.inPhrase) {
        // A phrase of three to seven motifs.
        b.inPhrase = true;
        b.phraseLen = 3 + static_cast<int>(rand01(b.seed) * 5.0f);
        b.motifLeft = 0;
    }
    if (b.motifLeft <= 0) {
        // A new motif: a shape, where it sits and how deep, sung once or a few times.
        b.shape = static_cast<int>(rand01(b.seed) * static_cast<float>(ShapeCount)) % ShapeCount;
        // A slur leaves from where the last motif was; anything else jumps.
        const float last = b.centre;
        b.centre = width * 0.8f * rand(b.seed);
        b.depth = width * (0.6f + 0.9f * rand01(b.seed));
        b.motifLeft = b.shape == TrillNote ? 4 + static_cast<int>(rand01(b.seed) * 7.0f)
                    : b.shape == Buzz || b.shape == Slur ? 1
                    : 1 + static_cast<int>(rand01(b.seed) * 3.0f);
        b.motifLen = kShapeLength[b.shape] * (0.7f + 0.6f * rand01(b.seed));
        b.loud = 0.7f + 0.3f * rand01(b.seed);
        if (b.shape == Slur) b.depth = last - b.centre;
        if (b.shape == Buzz) b.buzz = 35.0f + 35.0f * rand01(b.seed);
        --b.phraseLen;
    }
    --b.motifLeft;
    if (b.shape == Buzz) b.buzz = std::fmax(b.buzz, 35.0f);
    const float c = b.centre, d = b.depth;
    switch (b.shape) {
    case Fall: b.from = c + d; b.to = c - 0.3f * d; b.curve = 1; break;
    case Rise: b.from = c - d; b.to = c + 0.3f * d; b.curve = 2; break;
    case Arch: b.from = c - 0.4f * d; b.to = c - 0.6f * d; b.arch = d; b.curve = 0; break;
    case Note: b.from = c + 0.15f; b.to = c - 0.25f; b.curve = 0; break;
    case TrillNote: b.from = c + 0.5f * d; b.to = c - 0.2f * d; b.curve = 1; break;
    case Buzz: b.from = c; b.to = c - 0.3f; b.curve = 0; break;
    default: b.from = c + d; b.to = c; b.curve = 3; break; // Slur: from the last motif's pitch
    }
    b.singing = true;
    const float jitter = 0.9f + 0.2f * rand01(b.seed);
    // Syllables are quick; the rate sets them and the gaps between motifs together.
    b.slot = std::max(static_cast<int32_t>(kSyllableFloor * sampleRate), static_cast<int32_t>(0.6f * unit * b.motifLen * jitter));
    // Sung for most of the slot, with a gap after; a trill's notes are spaced evenly.
    b.sung = static_cast<float>(b.slot) * (b.shape == TrillNote ? 0.6f : 0.5f + 0.35f * length);
    // A longer gap after the last of a motif.
    if (b.motifLeft == 0) b.slot += static_cast<int32_t>(unit * (0.2f + 0.6f * rand01(b.seed)));
}

void Aviary::noteOn(uint8_t note, uint8_t velocity) {
    const bool kit = steppedTargetOf(Kit) != 0;
    const int pad = static_cast<int>(note) - kBaseNote;
    if (kit && (pad < 0 || pad >= kPads)) return;
    Voice *v = voiceFor(note);
    const bool fresh = !v->used;
    v->note = note;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    const int song = std::clamp(steppedTargetOf(kit ? padParam(pad, PadSong) : static_cast<int32_t>(Pattern)), 0, SongCount - 1);
    v->birdCount = std::clamp(steppedTargetOf(kit ? padParam(pad, PadFlock) : static_cast<int32_t>(Flock)), 1, kBirds);
    v->played = kit ? static_cast<float>(steppedTargetOf(padParam(pad, PadNote))) : static_cast<float>(note);
    // A pad's level is 0.75 for as loud as the bird played on its own.
    v->padLevel = kit ? clampf(targetOf(padParam(pad, PadLevel)), 0.0f, 1.0f) / 0.75f : 1.0f;
    updateMacroGoals(); // a note reads where the knobs are going, so renders repeat
    const float spread = clampf(goalOf(Spread), 0.0f, 1.0f);
    // A chorus: different birds, each in its own register. Which comes first,
    // and so which register each takes, is the voice's own.
    uint32_t chorusSeed = 0x2545f491u + note * 7919u + clock * 104729u;
    const int firstKind = static_cast<int>(rand01(chorusSeed) * 3.0f) % 3;
    const bool flockOfCaws = song == Caw && v->birdCount > 1;
    v->chorus = song == Chorus;
    for (int i = 0; i < v->birdCount; ++i) {
        Bird &b = v->birds[i];
        b.seed = 0x9e3779b9u * static_cast<uint32_t>(i + 1) + note * 7919u + clock * 104729u;
        b.far = 0.0f;
        b.shy = false;
        b.inPhrase = false;
        b.centre = 0.0f;
        b.motifLeft = 0;
        if (song == Chorus) {
            // A songbird, a bird that chips and one that trills, then another
            // songbird; each with its own pace and distance, in its own register
            // and not in tune with the others. They sing in phrases with pauses.
            const int songs[3] = {Warble, Chirp, Trill};
            const float registers[4] = {0.0f, 5.0f, -4.0f, 9.0f};
            b.song = songs[(firstKind + i) % 3];
            b.offset = registers[(firstKind + i) % 4] + 1.2f * rand(b.seed);
            b.drift = 0.75f + 0.6f * rand01(b.seed);
            b.far = i == 0 ? 0.15f * rand01(b.seed) : 0.25f + 0.65f * rand01(b.seed);
            b.shy = true;
        } else if (flockOfCaws) {
            // A flock of caws: each bird at its own pitch, pace and distance.
            b.song = song;
            b.offset = i == 0 ? 0.0f : 4.0f * rand(b.seed);
            b.drift = i == 0 ? 1.0f : 1.0f + 0.15f * rand(b.seed);
            b.far = i == 0 ? 0.0f : 0.2f + 0.7f * rand01(b.seed);
            b.shy = true;
        } else {
            b.song = song;
            b.offset = i == 0 ? 0.0f : std::round(rand(b.seed) * 3.0f);
            b.drift = i == 0 ? 1.0f : 1.0f + 0.06f * rand(b.seed);
            b.shy = song == Warble && v->birdCount > 1;
        }
        b.pan = v->birdCount > 1 ? spread * (-1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(v->birdCount - 1)) : 0.0f;
        if (fresh || !b.singing) {
            b.sides[0] = b.sides[1] = Side{};
            b.throat.reset();
            b.beak = 0.0f;
            std::fill(std::begin(b.trachea), std::end(b.trachea), 0.0f);
            b.builtHz = -1.0f;
            b.pitch = 0.0f;
            b.sub = 0.0f;
        }
        b.syllable = 0;
        b.retune = true;
        nextSyllable(*v, b, i);
        b.pitch = b.from;
    }
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Aviary::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.held = false;
            for (Bird &b : v.birds) b.singing = false;
        }
    }
}

void Aviary::allNotesOff() {
    for (Voice &v : voices) {
        v.held = false;
        for (Bird &b : v.birds) b.singing = false;
    }
}

void Aviary::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel widens the sweeps.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Aviary::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Aviary::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Aviary::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Aviary::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

bool Aviary::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const float length = clampf(paramOf(Length), 0.05f, 1.0f);
    const float rasp = clampf(paramOf(Rasp), 0.0f, 1.0f);
    const float two = clampf(paramOf(Two), 0.0f, 1.0f);
    const float twoRatio = std::exp2(paramOf(Interval) / 12.0f);
    const float breath = clampf(paramOf(Breath), 0.0f, 1.0f);
    const float throat = clampf(paramOf(Throat), 0.0f, 1.0f);
    const float beakOpen = clampf(paramOf(Beak), 0.0f, 1.0f);
    const float volume = paramOf(Volume);
    const float space = clampf(paramOf(Space), 0.0f, 1.0f);
    // The pitch macro counts in octaves here, so a route of half matches the octave knob at +1.
    const float base = kAbove + 12.0f * (static_cast<float>(steppedTargetOf(Octave)) + 2.0f * mac_[McPitch]) +
                       paramOf(Tune) / 100.0f + bend * paramOf(BendRange);
    const float dt = 1.0f / sampleRate;
    const float whistleGlide = 1.0f - std::exp(-1.0f / (kWhistleGlide * sampleRate));
    const int32_t echo = std::clamp(static_cast<int32_t>(kTracheaSeconds * sampleRate), 1, 31);
    bool any = false;

    struct Pre {
        float pl, pr, raspNow, beakFrom, beakBy, farGain, farDull;
        bool coo, caw, warble;
    };
    Pre pres[kVoices][kBirds];
    float blows[kVoices], peak[kVoices];
    bool live[kVoices];
    const float gain = kHouse;
    for (int vi = 0; vi < kVoices; ++vi) {
        Voice &v = voices[vi];
        live[vi] = v.used;
        peak[vi] = 0.0f;
        if (!v.used) continue;
        const float press = v.held ? (v.pressure >= 0.0f ? v.pressure : channelPressure_) : 0.0f;
        // How hard the bird blows: louder notes and pressure push past the threshold further.
        blows[vi] = (0.4f + 0.6f * velocityGain(v.velocity, paramOf(VelocityAmount))) * (1.0f + 0.6f * press);
        for (int bi = 0; bi < v.birdCount; ++bi) {
            const Bird &b = v.birds[bi];
            Pre &k = pres[vi][bi];
            k.pl = std::sqrt(0.5f * (1.0f - b.pan));
            k.pr = std::sqrt(0.5f * (1.0f + b.pan));
            k.coo = b.song == Coo;
            k.caw = b.song == Caw;
            // A coo is sung with the beak shut and little rasp; a caw is forced harsh.
            k.raspNow = k.caw ? 0.6f + 0.4f * rasp : k.coo ? 0.3f * rasp : rasp;
            k.beakFrom = k.caw ? 3.0f : k.coo ? 1.0f : 1.5f;
            k.beakBy = k.caw ? 5.0f : k.coo ? 2.0f : 4.0f;
            k.warble = b.song == Warble;
            // A far bird is quieter and has lost its top.
            k.farGain = (1.0f - kFarLevel * b.far) * (v.chorus ? kChorusLift : 1.0f) * (k.warble ? kWarbleLift : 1.0f);
            k.farDull = 1.0f - kFarDull * b.far;
        }
    }

    for (int vi = 0; vi < kVoices; ++vi) {
        if (!live[vi]) continue;
        Voice &v = voices[vi];
        const float blow = blows[vi];
        // A bird reads the others only as a syllable starts, so the birds run
        // one at a time up to the next start and meet there, in order. Each
        // has its own noise, so the same comes out at any block size.
        for (int32_t from = 0; from < frames;) {
            int32_t to = frames - 1;
            for (int bi = 0; bi < v.birdCount; ++bi) to = std::min(to, from + std::max(0, v.birds[bi].slot - v.birds[bi].at));
            for (int bi = 0; bi < v.birdCount; ++bi) {
                Bird &b = v.birds[bi];
                const Pre &k = pres[vi][bi];
                for (int32_t i = from; i <= to; ++i) {
                    const uint32_t clockNow = retuneClock + static_cast<uint32_t>(i);
                    const float pl = k.pl, pr = k.pr, raspNow = k.raspNow, beakFrom = k.beakFrom, beakBy = k.beakBy;
                    const float farGain = k.farGain, farDull = k.farDull;
                    const bool coo = k.coo, caw = k.caw, warble = k.warble;
                    if (b.at >= b.slot) nextSyllable(v, b, bi);
                    const float active = b.song == Whistle ? static_cast<float>(b.slot)
                                         : (coo || caw || warble) ? b.sung
                                                                  : length * static_cast<float>(b.slot);
                    const bool on = v.held && b.singing && static_cast<float>(b.at) < active;
                    // How hard it blows, as a part of [blow]: a coo swells in and out, a caw bursts.
                    float swell = 1.0f;
                    if (b.song == Whistle) {
                        b.pitch += (b.to - b.pitch) * whistleGlide;
                    } else if (coo || caw) {
                        if (on) {
                            const float p = static_cast<float>(b.at) / std::fmax(active, 1.0f);
                            const float shape = b.easeOut ? 1.0f - (1.0f - p) * (1.0f - p) : p * p;
                            b.pitch = b.from + (b.to - b.from) * shape + b.arch * 4.0f * p * (1.0f - p);
                            const float in = coo ? kCooIn : kCawIn, out = coo ? kCooOut : kCawOut;
                            swell = std::fmax(0.04f, std::fmin(1.0f, p / in) * std::fmin(1.0f, (1.0f - p) / out));
                            // The folds' size goes as the root of the pressure, so a coo's is squared to swell evenly.
                            if (coo) swell = std::fmax(0.04f, swell * swell);
                        }
                    } else if (warble) {
                        if (on) {
                            // A bird-shaped syllable: its curve, an arch, a buzz; a quick swell in and out.
                            const float p = static_cast<float>(b.at) / std::fmax(active, 1.0f);
                            const float shape = b.curve == 1 ? 1.0f - (1.0f - p) * (1.0f - p)
                                                : b.curve == 2 ? p * p
                                                : b.curve == 3 ? p * p * (3.0f - 2.0f * p)
                                                               : p;
                            b.pitch = b.from + (b.to - b.from) * shape + b.arch * 4.0f * p * (1.0f - p);
                            if (b.buzz > 0.0f) b.pitch += kBuzzDepth * std::sin(kTwoPi * b.buzz * static_cast<float>(b.at) * dt);
                            swell = b.loud * std::fmax(0.04f, std::fmin(1.0f, p / 0.08f) * std::fmin(1.0f, (1.0f - p) / 0.2f));
                        }
                    } else if (on) {
                        const float p = static_cast<float>(b.at) / std::fmax(active, 1.0f);
                        const float shape = 1.0f - (1.0f - p) * (1.0f - p);
                        b.pitch = b.from + (b.to - b.from) * shape;
                    }
                    ++b.at;
                    if ((clockNow & (kRetuneEvery - 1)) == 0 || b.retune) {
                        b.retune = false;
                        const float hz = noteHz(v.played + base + v.noteBend + b.offset + b.pitch);
                        // A caw's pitch shakes, the folds never quite settling.
                        const float shake = caw ? 1.0f + kCawJitter * raspNow * rand(b.seed) : 1.0f;
                        b.step0 = std::fmin(hz * shake, sampleRate * 0.45f) / sampleRate;
                        b.step1 = std::fmin(hz * shake * twoRatio, sampleRate * 0.45f) / sampleRate;
                        if (std::fabs(hz - b.builtHz) > b.builtHz * 0.002f) {
                            b.builtHz = hz;
                            // The throat is tuned to the note; the beak opens wider for higher notes.
                            b.throat.set(hz, 0.3f + 0.6f * throat);
                        }
                        const float cutoff = std::fmin(hz * (beakFrom + beakBy * beakOpen) * farDull, sampleRate * 0.45f);
                        b.beakCoef = 1.0f - std::exp(-kTwoPi * cutoff / sampleRate);
                    }
                    // The folds: past the threshold they swing up to a size set by the
                    // pressure; below it they come to rest.
                    const float mu0 = on ? blow * swell : kRest;
                    const float mu1 = on && two > 0.0f ? blow * swell * two : kRest;
                    Side &s0 = b.sides[0], &s1 = b.sides[1];
                    if (on && s0.size < 1e-3f) s0.size = 1e-3f;
                    if (mu1 > 0.0f && s1.size < 1e-3f) s1.size = 1e-3f;
                    s0.size = std::fmax(0.0f, s0.size + dt * kOnset * (mu0 - s0.size * s0.size) * s0.size);
                    s1.size = std::fmax(0.0f, s1.size + dt * kOnset * (mu1 - s1.size * s1.size) * s1.size);
                    s0.phase += b.step0;
                    if (s0.phase >= 1.0f) s0.phase -= 1.0f;
                    s1.phase += b.step1;
                    if (s1.phase >= 1.0f) s1.phase -= 1.0f;
                    if (s0.size < kSilent && s1.size < kSilent && !on) continue;
                    const float w0 = kTwoPi * s0.phase;
                    float x;
                    if (caw) {
                        // The folds slap shut hard: a buzz of harmonics, every other
                        // cycle weaker (the croak), and air let through as they open.
                        const float s = std::sin(w0);
                        float h = s;
                        const float harm[5] = {0.5f, 0.4f, 0.3f, 0.22f, 0.15f};
                        for (int n = 0; n < 5; ++n) {
                            if (static_cast<float>(n + 2) * b.step0 < 0.45f) h += raspNow * harm[n] * std::sin(static_cast<float>(n + 2) * w0);
                        }
                        b.sub += 0.5f * b.step0;
                        if (b.sub >= 1.0f) b.sub -= 1.0f;
                        h *= 1.0f + 0.45f * raspNow * std::sin(kTwoPi * b.sub);
                        x = s0.size * h;
                        x += s1.size * std::sin(kTwoPi * s1.phase);
                        x += (0.12f + 0.5f * breath) * (s0.size + s1.size) * rand(b.noise) * (0.6f + 0.4f * s);
                    } else {
                        // Rasp: the folds slapping shut, an octave and a touch of noise on top.
                        x = s0.size * (std::sin(w0) + raspNow * (0.5f * std::sin(2.0f * w0) + 0.25f * std::sin(3.0f * w0)));
                        x += s1.size * std::sin(kTwoPi * s1.phase);
                        x += breath * 0.15f * (s0.size + s1.size) * rand(b.noise);
                    }
                    // The trachea, the throat and the beak.
                    const float back = b.trachea[(b.tracheaAt - echo + 32) & 31];
                    const float y = x + kTracheaEcho * back;
                    b.trachea[b.tracheaAt] = y;
                    b.tracheaAt = (b.tracheaAt + 1) & 31;
                    const float shaped = y * (1.0f - 0.7f * throat) + throat * b.throat.step(y).bp * b.throat.bandNorm();
                    b.beak += (shaped - b.beak) * b.beakCoef;
                    const float out = b.beak * gain * volume * farGain;
                    L[i] += out * pl * v.padLevel;
                    R[i] += out * pr * v.padLevel;
                    peak[vi] = std::fmax(peak[vi], std::fabs(out));
                }
            }
            from = to + 1;
        }
    }
    retuneClock += static_cast<uint32_t>(frames);

    for (int vi = 0; vi < kVoices; ++vi) {
        Voice &v = voices[vi];
        if (!live[vi]) continue;
        v.level = peak[vi];
        if (!v.held && peak[vi] < kSilent * 0.1f) {
            if (++v.quietBlocks > 4) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
        any = any || v.used;
    }

    // Distance: a far bird is quieter and has lost its top.
    if (space > 0.0f) {
        const float c = 1.0f - std::exp(-kTwoPi * (16000.0f * std::pow(0.12f, space)) / sampleRate);
        const float level = 1.0f - 0.5f * space;
        for (int32_t i = 0; i < frames; ++i) {
            spaceLp[0] += (L[i] - spaceLp[0]) * c;
            spaceLp[1] += (R[i] - spaceLp[1]) * c;
            L[i] = spaceLp[0] * level;
            R[i] = spaceLp[1] * level;
        }
    }

    if (!any) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return true;
}

} // namespace acidulous::machine
