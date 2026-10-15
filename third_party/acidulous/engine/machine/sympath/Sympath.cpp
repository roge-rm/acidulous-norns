#include "Sympath.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a note at velocity 100, set so the bank sits with the other machines. */
constexpr float kHouse = 1.0f;
constexpr float kSilent = 2e-5f;
constexpr int kRetuneEvery = 16;
/** How hard the played strings shake the sympathetic ones. */
constexpr float kSympathy = 1.5f;
/** How long the sympathetic and drone strings ring, seconds. */
constexpr float kTarbT60 = 5.0f, kDroneT60 = 3.5f;
/** Gamak at full, semitones, and its rate. */
constexpr float kGamakDepth = 0.6f, kGamakHz = 6.0f;
/** A finger pulling the string sideways at full pressure, semitones. */
constexpr float kPullSemis = 2.0f;
/** How far a string swings before it meets the bridge, at the curve knob's top. */
constexpr float kCurveSpan = 0.25f;
/** How round the bridge is where the string first meets it. */
constexpr float kContactRound = 0.01f;
/** How loud the bridge rings when struck, at full, and the most it can ring. */
/**
 * How hard a bow drives a string, as the hurdy-gurdy's wheel, and how much
 * louder a bowed string is heard: it swings far less than a plucked one.
 */
constexpr float kBowGain = 0.12f, kBowedHeard = 28.0f;
constexpr float kZing = 50.0f, kZingMost = 0.35f;

/**
 * Each kind: its lowest string and how long that rings, how much top the
 * strings keep, how long a let-go note rings, how much of the bridge knob it
 * has, how long a pluck stays on the string, its sympathetic and drone
 * strings, its gourd, and a skin for the shamisen.
 */
struct Make {
    float lowest;
    float sustain;
    float bright;
    float release;
    float bridge;
    float pluck;     // seconds, at the pluck knob's middle
    int tarbs;
    int drones;
    float droneAt[Sympath::kDrones]; // semitones over Sa
    float bodyHz, body;
    float zingHz;    // where the bridge rings when the string strikes it
    float skin;
    float stiffness;
    float level;
    /** Bowed rather than plucked: how hard the bow drives the string while the key is held, 0 for a plucked one. */
    float bowed = 0.0f;
};
constexpr Make kMakes[Sympath::KindCount] = {
    // Sitar: a wire plectrum, eleven sympathetic strings, two drone strings at Sa and the octave above.
    {48.0f, 6.0f, 0.92f, 2.0f, 1.0f, 0.0005f, 11, 2, {12.0f, 24.0f, 0.0f}, 220.0f, 0.4f, 3200.0f, 0.0f, 0.05f, 1.0f},
    // Tanpura: four long strings over a wide bridge, plucked softly with the finger.
    {36.0f, 10.0f, 0.9f, 6.0f, 1.0f, 0.0025f, 0, 0, {0.0f, 0.0f, 0.0f}, 150.0f, 0.5f, 2400.0f, 0.0f, 0.03f, 0.8f},
    // Veena: a flatter bridge, plucked with the nail, three drone strings at the side.
    {36.0f, 7.0f, 0.92f, 2.0f, 0.6f, 0.0008f, 0, 3, {12.0f, 19.0f, 24.0f}, 180.0f, 0.45f, 2800.0f, 0.0f, 0.04f, 1.4f},
    // Shamisen: a big plectrum that hits the skin too, and the low string buzzing on the neck.
    {45.0f, 2.5f, 0.88f, 0.3f, 0.35f, 0.0003f, 1, 0, {0.0f, 0.0f, 0.0f}, 400.0f, 0.3f, 1800.0f, 1.0f, 0.02f, 1.7f},
    // Sarod: no frets, a steel plate under the strings and a goatskin belly, a coconut-shell plectrum, sympathetic strings.
    {48.0f, 4.0f, 0.9f, 1.5f, 0.3f, 0.0004f, 11, 2, {12.0f, 24.0f, 0.0f}, 300.0f, 0.4f, 2400.0f, 0.5f, 0.04f, 1.2f},
    // Rudra veena: deep and slow, two big gourds, the jawari full.
    {36.0f, 9.0f, 0.88f, 4.0f, 1.0f, 0.0012f, 0, 3, {12.0f, 19.0f, 24.0f}, 120.0f, 0.6f, 2000.0f, 0.0f, 0.04f, 1.2f},
    // Swarmandal: a zither, its strings plucked open, bright and ringing, no buzzing bridge.
    {60.0f, 5.0f, 0.95f, 4.0f, 0.0f, 0.0004f, 0, 0, {0.0f, 0.0f, 0.0f}, 500.0f, 0.2f, 4000.0f, 0.0f, 0.02f, 0.9f},
    // Biwa: a Japanese lute, its high frets and sawari buzzing, the big plectrum striking the body.
    {45.0f, 2.0f, 0.86f, 0.5f, 0.8f, 0.0004f, 0, 0, {0.0f, 0.0f, 0.0f}, 350.0f, 0.4f, 1600.0f, 0.6f, 0.02f, 1.4f},
    // Sanxian: three strings over snakeskin, bright and dry.
    {45.0f, 2.0f, 0.88f, 0.4f, 0.1f, 0.0003f, 0, 0, {0.0f, 0.0f, 0.0f}, 450.0f, 0.35f, 2000.0f, 1.0f, 0.02f, 1.5f},
    // Ektara: one string over a gourd, a buzzing drone with a bend.
    {48.0f, 3.0f, 0.88f, 1.0f, 0.7f, 0.0005f, 0, 0, {0.0f, 0.0f, 0.0f}, 250.0f, 0.5f, 2200.0f, 0.4f, 0.02f, 1.4f},
    // Electric sitar: a guitar with a buzzing bridge and sympathetic strings, heard through a pickup.
    {40.0f, 5.0f, 0.93f, 1.0f, 1.0f, 0.0004f, 11, 0, {0.0f, 0.0f, 0.0f}, 200.0f, 0.1f, 3600.0f, 0.0f, 0.0f, 1.0f},
    // Sarangi: gut strings bowed over a skin belly, a forest of sympathetic strings, played by the nail's side.
    {43.0f, 3.0f, 0.88f, 0.4f, 0.2f, 0.0004f, 11, 0, {0.0f, 0.0f, 0.0f}, 350.0f, 0.5f, 2000.0f, 0.6f, 0.02f, 1.0f, 1.0f},
    // Esraj: a bowed, fretted neck on a small skin belly, softer, sympathetic strings.
    {43.0f, 3.0f, 0.9f, 0.4f, 0.15f, 0.0004f, 11, 0, {0.0f, 0.0f, 0.0f}, 300.0f, 0.4f, 2200.0f, 0.4f, 0.02f, 1.0f, 0.8f},
    // Dilruba: the esraj's larger cousin, deeper and fuller.
    {38.0f, 3.5f, 0.9f, 0.5f, 0.15f, 0.0004f, 11, 0, {0.0f, 0.0f, 0.0f}, 220.0f, 0.5f, 1900.0f, 0.4f, 0.02f, 1.0f, 0.9f},
    // Hardanger fiddle: a violin with four sympathetic strings under the fingerboard, bright and ringing.
    {55.0f, 3.0f, 0.93f, 0.4f, 0.0f, 0.0004f, 4, 0, {0.0f, 0.0f, 0.0f}, 450.0f, 0.4f, 3000.0f, 0.0f, 0.02f, 1.0f, 1.0f},
    // Nyckelharpa: a keyed fiddle, its sympathetic strings shimmering under every note.
    {48.0f, 3.5f, 0.92f, 0.5f, 0.0f, 0.0004f, 11, 0, {0.0f, 0.0f, 0.0f}, 280.0f, 0.45f, 2600.0f, 0.0f, 0.02f, 1.0f, 1.0f},
};

/** The ten parent scales, from the tonic. */
constexpr int kScaleSteps[Sympath::kScales][7] = {
    {0, 2, 4, 5, 7, 9, 11}, // Bilawal
    {0, 2, 4, 5, 7, 9, 10}, // Khamaj
    {0, 2, 3, 5, 7, 9, 10}, // Kafi
    {0, 2, 3, 5, 7, 8, 10}, // Asavari
    {0, 1, 3, 5, 7, 8, 10}, // Bhairavi
    {0, 1, 4, 5, 7, 8, 11}, // Bhairav
    {0, 2, 4, 6, 7, 9, 11}, // Kalyan
    {0, 1, 4, 6, 7, 9, 11}, // Marwa
    {0, 1, 4, 6, 7, 8, 11}, // Purvi
    {0, 1, 3, 6, 7, 8, 11}, // Todi
};

/** A tanpura's first string, below Sa: Pa, Ma, Ni, or Sa an octave down. */
constexpr float kFirstString[4] = {-5.0f, -7.0f, -1.0f, -12.0f};
/** Its cycle, in beats. */
constexpr float kCycleBeats[4] = {2.0f, 4.0f, 6.0f, 8.0f};

} // namespace

Sympath::Sympath() { initParams(); }

const ParamDef *Sympath::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""}, // sitar, tanpura, veena, shamisen, sarod, rudra veena, swarmandal, biwa, sanxian, ektara, electric sitar, sarangi, esraj, dilruba, hardanger, nyckelharpa
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // The tonic the drone and sympathetic strings are tuned to, C to B.
        {"sa", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""},
        // The scale the sympathetic strings are tuned to.
        {"scale", 0.0f, static_cast<float>(kScales - 1), 0.0f, Curve::Stepped, kScales, ""},
        // How much the bridge buzzes: 0 is a plain, sharp edge.
        {"bridge", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        // How far the string swings before it lies on the bridge.
        {"curve", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"pluck", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"position", 0.04f, 0.5f, 0.12f, Curve::Linear, 0, ""},
        {"sustain", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bright", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"tarbs", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // The drone strings, struck with each new note.
        {"chikari", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"first", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // pa, ma, ni, sa
        {"cycle", 0.0f, 3.0f, 1.0f, Curve::Stepped, 4, ""},  // 2, 4, 6, 8 beats
        // With one voice, a new note slides from the last instead of being plucked.
        {"meend", 0.0f, 600.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"gamak", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"voices", 1.0f, static_cast<float>(kVoices), static_cast<float>(kVoices), Curve::Stepped, kVoices, ""},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Sympath::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    const auto strokeSize = static_cast<size_t>(sampleRate * 0.06f);
    for (Voice &v : voices) {
        for (String &s : v.strings) {
            s.wave.prepare(sampleRate);
            s.stroke.assign(strokeSize, 0.0f);
        }
    }
    for (Free &f : tarbs) f.wave.prepare(sampleRate);
    for (Free &f : drones) {
        f.wave.prepare(sampleRate);
        f.stroke.assign(strokeSize, 0.0f);
    }
    body.setSampleRate(sampleRate);
    for (Voice &v : voices) v.zing.setSampleRate(sampleRate);
    tarbZing.setSampleRate(sampleRate);
    skinTone.setSampleRate(sampleRate);
    skinTone.set(1400.0f, 0.3f);
    reset();
}

void Sympath::reset() {
    for (Voice &v : voices) {
        for (String &s : v.strings) {
            s.wave.clear();
            s.strokeLength = s.strokeAt = 0;
            s.sounding = false;
            s.bowDc = 0.0f;
            s.bowAt = 0;
        }
        v.used = v.held = false;
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.pull = 0.0f;
        v.cycleAt = 0.0;
        v.nextString = 0;
        v.level = 0.0f;
        v.quietBlocks = 0;
        v.age = 0;
        v.sincePluck = 0;
        v.note = 0;
        v.velocity = 0.8f;
        v.pitch = v.aim = 60.0f;
        v.gain = 0.0f;
    }
    for (Free &f : tarbs) f.wave.clear();
    for (Free &f : drones) {
        f.wave.clear();
        f.strokeLength = f.strokeAt = 0;
    }
    builtSa = builtScale = builtKind = -1.0f;
    builtTune = -1000.0f;
    bpm = 120.0f;
    bend = wheel = channelPressure_ = 0.0f;
    gamakPhase = 0.0f;
    retuneCountdown = 0;
    body.reset();
    bodyBuiltFor = -1.0f;
    for (Voice &v : voices) {
        v.zing.reset();
        v.contactLast = 0.0f;
    }
    tarbZing.reset();
    tarbContactLast = 0.0f;
    skinTone.reset();
    skin = skinLast = 0.0f;
    dcIn[0] = dcIn[1] = dcOut[0] = dcOut[1] = 0.0f;
    noise = 0x2c1b3c6du;
    clock = 0;
    quietSamples = 0;
    asleep = true;
}

void Sympath::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

int Sympath::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Sympath::Voice *Sympath::voiceFor(uint8_t note) {
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

float Sympath::stringT60(float hz, bool held) const {
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float lowHz = noteHz(k.lowest);
    float t60 = k.sustain * std::pow(4.0f, paramOf(Sustain) - 0.5f) * std::sqrt(lowHz / std::fmax(hz, lowHz));
    if (!held) t60 = std::fmin(t60, k.release);
    return t60;
}

float Sympath::stringTone(float velocity) const {
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float loss = (1.0f - k.bright) * std::pow(4.0f, 0.5f - paramOf(Bright)) * std::pow(2.0f, 0.7f - velocity);
    return clampf(1.0f - loss, 0.03f, 1.0f);
}

/** The highest a string is tuned, about note 100; see tuneString. */
constexpr float kHighestHz = 2637.0f;

void Sympath::tuneString(Waveguide &w, float hz, float t60, float tone) {
    // Nothing above a string's reach: higher fold down an octave at a time.
    // Past about note 105 the loop is too short to lose its energy, and with
    // the bridge buzzing it rings for ever.
    while (hz > kHighestHz) hz *= 0.5f;
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    w.setFrequency(hz);
    w.setDcCorner(0.01f);
    w.setDispersion(k.stiffness, k.stiffness > 0.0f ? 2 : 0);
    w.setDamping(std::pow(10.0f, -3.0f / (std::fmax(t60, 0.01f) * hz)), tone);
}

void Sympath::tuneFree() {
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    const float sa = static_cast<float>(std::clamp(steppedTargetOf(Sa), 0, 11));
    const int scale = std::clamp(steppedTargetOf(Scale), 0, kScales - 1);
    const float tune = targetOf(Tune) / 100.0f;
    if (sa == builtSa && static_cast<float>(scale) == builtScale && static_cast<float>(kind) == builtKind && tune == builtTune) return;
    builtSa = sa;
    builtScale = static_cast<float>(scale);
    builtKind = static_cast<float>(kind);
    builtTune = tune;
    const float saNote = 48.0f + sa + tune;
    tarbCount = k.tarbs;
    for (int i = 0; i < tarbCount; ++i) {
        // A shamisen's one is its low string, buzzing on the neck; a sitar's
        // run up the scale from the Sa above the main string's.
        float note = saNote - 12.0f;
        if (kind != Shamisen) note = saNote + 12.0f + static_cast<float>(kScaleSteps[scale][i % 7] + 12 * (i / 7));
        Free &f = tarbs[i];
        f.hz = noteHz(note);
        f.pan = tarbCount > 1 ? -0.7f + 1.4f * static_cast<float>(i) / static_cast<float>(tarbCount - 1) : 0.0f;
        tuneString(f.wave, f.hz, kTarbT60, 0.97f);
    }
    droneCount = k.drones;
    for (int i = 0; i < droneCount; ++i) {
        Free &f = drones[i];
        f.hz = noteHz(saNote + k.droneAt[i]);
        f.pan = 0.3f;
        tuneString(f.wave, f.hz, kDroneT60, 0.95f);
    }
}

int32_t Sympath::shapePluck(std::vector<float> &stroke, float hz, float velocity, float amount) {
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float hard = clampf(targetOf(Pluck), 0.0f, 1.0f);
    const float seconds = k.pluck * std::pow(4.0f, 0.5f - hard);
    const int len = std::max(3, static_cast<int>(seconds * sampleRate));
    const float period = sampleRate / hz;
    const int shift = std::max(1, static_cast<int>(std::lround(clampf(targetOf(Position), 0.04f, 0.5f) * period)));
    const int total = std::min(static_cast<int>(stroke.size()), len + shift);
    std::fill(stroke.begin(), stroke.begin() + total, 0.0f);
    // The pluck goes into the string at the strength it was played: the
    // bridge buzzes more the wider the string swings.
    const float amp = 0.6f * amount * velocityGain(velocity, targetOf(VelocityAmount));
    for (int i = 0; i < len && i < total; ++i) {
        stroke[static_cast<size_t>(i)] = amp * std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(len));
    }
    for (int i = total - 1; i >= shift; --i) stroke[static_cast<size_t>(i)] -= stroke[static_cast<size_t>(i - shift)];
    return total;
}

void Sympath::pluck(Voice &v, int s) {
    String &st = v.strings[s];
    const float hz = noteHz(v.pitch + st.offset);
    // A tanpura's strings are plucked a little differently each time.
    const float amount = v.stringCount > 1 ? 0.85f + 0.15f * white() : 1.0f;
    st.strokeLength = shapePluck(st.stroke, hz, v.velocity, amount);
    st.strokeAt = 0;
    st.sounding = true;
    v.sincePluck = 0;
    if (kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)].skin > 0.0f) skin = std::fmax(skin, v.velocity);
}

void Sympath::start(uint8_t note, uint8_t velocity) {
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    const float pitch = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    // With one voice, a held note slides to the new one: meend, no new pluck.
    if (kind != Tanpura && steppedTargetOf(Voices) == 1 && targetOf(Meend) > 0.0f && voices[0].used && voices[0].held) {
        Voice &v = voices[0];
        v.note = note;
        v.aim = pitch;
        v.age = ++clock;
        return;
    }
    Voice *v = voiceFor(note);
    v->note = note;
    v->pitch = v->aim = pitch;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    v->pull = 0.0f;
    v->gain = kHouse * k.level;
    if (kind == Tanpura) {
        // Pa (or Ma, or Ni) below, Sa twice a hair apart, and Sa an octave down.
        const float first = kFirstString[std::clamp(steppedTargetOf(First), 0, 3)];
        const float offsets[kStrings] = {first, 0.02f, -0.02f, -12.0f};
        v->stringCount = kStrings;
        for (int s = 0; s < kStrings; ++s) v->strings[s].offset = offsets[s];
    } else {
        v->stringCount = 1;
        v->strings[0].offset = 0.0f;
    }
    if (!v->used) {
        for (String &s : v->strings) s.wave.clear();
        v->zing.reset();
        v->contactLast = 0.0f;
    }
    const float tone = stringTone(v->velocity);
    for (int s = 0; s < v->stringCount; ++s) {
        const float hz = noteHz(pitch + v->strings[s].offset);
        tuneString(v->strings[s].wave, hz, stringT60(hz, true), tone);
        v->strings[s].strokeLength = v->strings[s].strokeAt = 0;
    }
    v->cycleAt = 0.0;
    v->nextString = 0;
    // A bowed string isn't plucked: the bow starts it, below.
    if (kind != Tanpura && kMakes[kind].bowed <= 0.0f) pluck(*v, 0);
    v->strings[0].bowAt = 0;
    // The drone strings, struck with the note.
    tuneFree();
    const float chikari = clampf(targetOf(Chikari), 0.0f, 1.0f);
    if (chikari > 0.0f) {
        for (int i = 0; i < droneCount; ++i) {
            Free &f = drones[i];
            f.strokeLength = shapePluck(f.stroke, f.hz, v->velocity, chikari * (0.8f + 0.1f * white()));
            f.strokeAt = 0;
        }
    }
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Sympath::noteOn(uint8_t note, uint8_t velocity) { start(note, velocity); }

void Sympath::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.held = false;
    }
}

void Sympath::allNotesOff() {
    for (Voice &v : voices) {
        if (v.used) v.held = false;
    }
}

void Sympath::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel shakes the note: gamak.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Sympath::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Sympath::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Sympath::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Sympath::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

bool Sympath::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    // The pluck knob, bowed: how hard the bow presses, from airy to gripping.
    const float pluckKnob = clampf(paramOf(Pluck), 0.0f, 1.0f);
    tuneFree();
    const float bridge = clampf(paramOf(Bridge) * k.bridge, 0.0f, 1.0f);
    const float curve = clampf(paramOf(Curve), 0.0f, 1.0f) * kCurveSpan;
    const float tarbLevel = clampf(paramOf(Tarbs), 0.0f, 1.0f);
    const float volume = paramOf(Volume);
    const float gamak = clampf(paramOf(Gamak) + wheel, 0.0f, 1.0f) * kGamakDepth;
    const float meendCoef = targetOf(Meend) > 0.0f
        ? 1.0f - std::exp(-static_cast<float>(kRetuneEvery) / (targetOf(Meend) * 0.001f * sampleRate / 3.0f)) : 1.0f;
    const double cycleLength = static_cast<double>(kCycleBeats[std::clamp(steppedTargetOf(Cycle), 0, 3)] * 60.0f / bpm * sampleRate);
    if (k.bodyHz != bodyBuiltFor) {
        bodyBuiltFor = k.bodyHz;
        body.set(k.bodyHz, 0.5f);
        for (Voice &v : voices) v.zing.set(k.zingHz, 0.55f);
        tarbZing.set(k.zingHz, 0.55f);
    }
    const float skinFall = std::exp(-1.0f / (0.012f * sampleRate));
    const float dcPole = 1.0f - 6.2831853f * 15.0f / sampleRate;
    float peaks[kVoices] = {};
    bool any = false;

    for (int32_t i = 0; i < frames; ++i) {
        if (retuneCountdown-- <= 0) {
            retuneCountdown = kRetuneEvery;
            gamakPhase += kGamakHz * static_cast<float>(kRetuneEvery) / sampleRate;
            if (gamakPhase >= 1.0f) gamakPhase -= 1.0f;
            const float shake = gamak * std::sin(6.2831853f * gamakPhase);
            for (Voice &v : voices) {
                if (!v.used) continue;
                v.pitch += (v.aim - v.pitch) * meendCoef;
                const float press = v.held ? (v.pressure >= 0.0f ? v.pressure : channelPressure_) : 0.0f;
                v.pull += (press - v.pull) * 0.05f;
                const float base = v.pitch + bend * paramOf(BendRange) + v.noteBend + shake + v.pull * kPullSemis;
                const float tone = stringTone(v.velocity);
                for (int s = 0; s < v.stringCount; ++s) {
                    const float hz = noteHz(base + v.strings[s].offset);
                    tuneString(v.strings[s].wave, hz, stringT60(hz, v.held || kind == Tanpura), tone);
                }
            }
        }
        float played = 0.0f;
        for (int vi = 0; vi < kVoices; ++vi) {
            Voice &v = voices[vi];
            if (!v.used) continue;
            // A tanpura plucks its strings in turn, through its cycle, while the note's held.
            if (kind == Tanpura && v.held) {
                while (v.nextString < v.stringCount &&
                       v.cycleAt >= cycleLength * static_cast<double>(v.nextString) / 5.0) {
                    pluck(v, v.nextString++);
                }
                v.cycleAt += 1.0;
                if (v.cycleAt >= cycleLength) {
                    v.cycleAt -= cycleLength;
                    v.nextString = 0;
                }
            }
            float out = 0.0f, contact = 0.0f;
            for (int s = 0; s < v.stringCount; ++s) {
                String &st = v.strings[s];
                float in = 0.0f;
                if (st.strokeAt < st.strokeLength) in = st.stroke[static_cast<size_t>(st.strokeAt++)];
                // Bowed: rosin's friction, as Filament's and the hurdy-gurdy's, on the
                // played string while the key is held. Harder playing, a faster bow.
                if (k.bowed > 0.0f && s == 0 && v.held) {
                    const float speed = 0.4f + 0.6f * v.velocity;
                    const float width = 0.05f + 0.2f * (1.0f - pluckKnob);
                    const float relative = width * speed - st.wave.velocity();
                    const float force = relative / (width + relative * relative / width);
                    // Set down gently, the push coming in over the bow's first 40 ms.
                    const float onset = std::fmin(1.0f, static_cast<float>(st.bowAt++) / (0.04f * sampleRate));
                    float excite = (force * 0.65f + white() * 0.02f) * kBowGain * k.bowed * onset;
                    st.bowDc += (excite - st.bowDc) * 0.002f;
                    in += excite - st.bowDc;
                }
                const float y = st.wave.step(in);
                out += k.bowed > 0.0f && s == 0 ? y * kBowedHeard : y;
                // The bridge: once the string swings past its height it
                // strikes it, harder the further past.
                const float past = y - curve;
                if (past > 0.0f) contact += past * past / (past + kContactRound);
            }
            // Each strike rings the bridge: the buzz, on every swing that reaches it.
            if (bridge > 0.0f) out += bridge * kZingMost * std::tanh(kZing / kZingMost * v.zing.step(contact - v.contactLast).bp);
            v.contactLast = contact;
            out *= v.gain;
            played += out;
            peaks[vi] = std::fmax(peaks[vi], std::fabs(out));
        }
        // The sympathetic strings, shaken by everything played.
        float sympathy = 0.0f, sl = 0.0f, sr = 0.0f;
        for (int t = 0; t < tarbCount; ++t) {
            Free &f = tarbs[t];
            f.wave.exciteOverTurn(played * kSympathy);
            float y = f.wave.step(0.0f);
            // The shamisen's low string lies on the neck: sawari.
            if (kind == Shamisen) {
                const float past = y - curve * 0.2f;
                const float c = past > 0.0f ? past * past / (past + kContactRound) : 0.0f;
                y += kZingMost * std::tanh(kZing / kZingMost * tarbZing.step(c - tarbContactLast).bp);
                tarbContactLast = c;
            }
            sl += y * (0.5f - 0.5f * f.pan);
            sr += y * (0.5f + 0.5f * f.pan);
            sympathy += std::fabs(y);
        }
        sl *= tarbLevel;
        sr *= tarbLevel;
        float droneOut = 0.0f;
        for (int d = 0; d < droneCount; ++d) {
            Free &f = drones[d];
            float in = 0.0f;
            if (f.strokeAt < f.strokeLength) in = f.stroke[static_cast<size_t>(f.strokeAt++)];
            droneOut += f.wave.step(in);
        }
        droneOut *= kHouse * k.level;
        // The gourd, and the skin a shamisen's plectrum hits.
        float mono = played + 0.4f * droneOut;
        mono += k.body * body.step(mono).bp * body.bandNorm();
        if (skin > 1e-5f) {
            const float n = skinTone.step(white() * skin).bp * 0.5f;
            mono += n - skinLast;
            skinLast = n;
            skin *= skinFall;
        }
        float l = (mono + sl) * volume, r = (mono + sr + 0.3f * droneOut) * volume;
        // Off with any offset the bridge leaves.
        const float hl = l - dcIn[0] + dcPole * dcOut[0];
        dcIn[0] = l;
        dcOut[0] = hl;
        const float hr = r - dcIn[1] + dcPole * dcOut[1];
        dcIn[1] = r;
        dcOut[1] = hr;
        L[i] = hl;
        R[i] = hr;
        any = any || sympathy > kSilent;
    }
    for (int vi = 0; vi < kVoices; ++vi) {
        Voice &v = voices[vi];
        if (!v.used) continue;
        v.level = peaks[vi];
        bool plucking = false;
        for (int s = 0; s < v.stringCount; ++s) plucking = plucking || v.strings[s].strokeAt < v.strings[s].strokeLength;
        // A pluck takes a whole turn of the string to come out the far end.
        v.sincePluck += frames;
        const bool keeps = (kind == Tanpura && v.held) || v.sincePluck < static_cast<int32_t>(0.1f * sampleRate);
        if (v.level < kSilent && !plucking && !keeps) {
            if (++v.quietBlocks > 4) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
        any = any || v.used;
    }
    for (int d = 0; d < droneCount; ++d) any = any || drones[d].strokeAt < drones[d].strokeLength || drones[d].wave.level() > kSilent;

    if (!any) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return true;
}

} // namespace acidulous::machine
