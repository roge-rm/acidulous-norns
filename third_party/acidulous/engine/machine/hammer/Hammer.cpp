#include "Hammer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/core/Messages.h>
#include <engine/core/Settings.h>
#include <engine/dsp/Lfo.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {
using dsp::clampf;

namespace {

/** Brings Init to the house level (audition: loudness -30, as the other machines' banks). */
constexpr float kHouse = 0.230f;
/** The hammer's speed at the softest and hardest velocities, m/s. */
constexpr float kSlowest = 0.3f, kFastest = 6.0f;
/** How long the gain and the dampers take to move: 5 ms and 20 ms. */
constexpr float kGainRamp = 0.005f, kDamperTime = 0.02f;
/** How hard what reaches the bridge drives an unplayed string, at sympathy 1. */
constexpr float kSympathyDrive = 0.0027f;
/**
 * The pedal's thump against a hammer's knock, for dampers coming all the way
 * off: felt, under the notes. Ten times this was 4 dB under a mf phrase and
 * rang the board like a short note at every pedal change.
 */
constexpr float kPedalThump = 0.002f;
/** How long a voice made to give way takes to fade out, seconds. */
constexpr float kRetireTime = 0.01f;
/** The phantom partials' level at the default tension, and their high-pass (below them, only the difference tones). */
constexpr float kPhantom = 0.0f;
constexpr float kPhantomPole = 0.98f;
/** The knock's level against the note's, before each key's own (Keys.h). */
constexpr float kKnock = 0.1f;
/**
 * How far the strings swing where a bolt sits, played mf, in their own units
 * (velocity times samples): about 10 at C3, half that two octaves up. A
 * bolt sits loose by a share of it, so loud notes rattle more, and a note
 * stops rattling as it dies away.
 */
float swingAt(int key) { return 10.0f * std::exp2((48.0f - static_cast<float>(key)) / 24.0f); }
/** How stiffly paper springs back, against its mass: its own ring at about 400 Hz. */
constexpr float kPaperSpring = 0.0027f;
/**
 * How far a tine or reed swings per m/s of the hammer, in the pickup's
 * reach at its middle setting: 0.12 at velocity 30, 0.9 at 115, so a soft
 * note is nearly pure and a hard one barks (tools/hammer_reference: the
 * recordings' second harmonic 27 dB under the note soft, level with it hard).
 */
constexpr float kSwing = 0.2f;
/** The electric pianos' level against the grand's, and the tangent's pickups' against its bridge. */
constexpr float kElectricLevel = 0.8f, kPickupLevel = 2.4f;
/** A bar's knock against a string's, for its blow's units: a celesta's action 25 dB under its notes, a toy's clack 12. */
constexpr float kBarKnock = 28.0f;
/** The level a mf electric note reaches at the amp, for its drive. */
constexpr float kAmpNominal = 0.1f;
/** A voice let go and this quiet (on its slow follower) for a few blocks is free. */
constexpr float kSilent = 2e-5f;

} // namespace

Hammer::Hammer() { initParams(); }

const ParamDef *Hammer::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // grand, upright, honky, fortepiano, electric grand, tine, reed,
        // tangent, celesta, toy, dulcimer, cimbalom (hammer::Model).
        {"model", 0.0f, 11.0f, 0.0f, Curve::Stepped, 12, ""},
        {"size", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"age", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"seed", 0.0f, 7.0f, 0.0f, Curve::Stepped, 8, ""},

        // The felt: softer to harder, by key, heavier or lighter, where it strikes.
        {"hardness", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"hardkey", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"weight", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"position", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"felt", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tacks", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},

        // How long the strings ring, as a multiple of the measured times
        // (4^x), and how fast the top goes against the rest.
        {"sustain", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"sustainkey", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tone", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tonekey", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"stiffness", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"stretch", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"unison", 0.0f, 3.0f, 1.0f, Curve::Linear, 0, ""},
        // auto, 1, 2, 3.
        {"strings", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"couple", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"polar", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"tension", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"clang", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},

        {"dampers", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"damp time", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"pedal at", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"pedal span", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"una corda", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"sympathy", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"duplex", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"noises", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},

        {"board", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"lid", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"tail", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // player, audience, close, room.
        {"mic", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"width", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},

        {"pickup", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"offset", -1.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"tonebar", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // upper, lower, both, both reversed.
        {"pickups", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"mute", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"tremolo", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tremrate", 0.5f, 10.0f, 4.5f, Curve::Exponential, 0, "Hz"},
        // free, then the tempo-locked note values.
        {"tremsync", 0.0f, 17.0f, 0.0f, Curve::Stepped, 18, ""},
        {"tremwide", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},

        // none, rubber, screw, bolt, paper, mixed.
        {"prep", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
        // all, white, black, low, high, random.
        {"prepkeys", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
        {"prep at", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"prepamt", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},

        // auto follows the quality setting; full is full detail whatever it says.
        {"detail", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // 4, 6, 8, 10, 12, 16, 20, 24, 32 at most.
        {"voices", 0.0f, 8.0f, 6.0f, Curve::Stepped, 9, ""},
        {"volume", 0.0f, 1.5f, 1.0f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},
        {"fine", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
    };
    count = Count;
    return defs;
}

void Hammer::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (int m = 0; m < kKeyModels; ++m) {
        for (int k = 0; k < kKeys; ++k) keys[m][k] = hammer::keyFor(m, clampf(static_cast<float>(k), 21.0f, 108.0f));
    }
    refreshKeys();
    for (Voice &v : voices) {
        v.course.prepare(sampleRate);
        v.bar.prepare(sampleRate);
    }
    amp.prepare(sampleRate);
    for (Voice &b : bank) b.course.prepare(sampleRate);
    warmer.prepare(sampleRate);
    board.prepare(sampleRate);
    reset();
    designSections();
    warmed = true;
    warmKey = kSectionKeys;
    warmFor[0] = static_cast<float>(modelNow());
    warmFor[1] = targetOf(Size);
    warmFor[2] = targetOf(Stiffness);
    warmFor[3] = targetOf(Stretch) + (fullDetail() ? 10.0f : 0.0f);
}

void Hammer::refreshKeys() {
    for (int m = 0; m < kKeyModels; ++m) {
        for (int k = 0; k < kKeys; ++k) {
            stiffness[m][k] = hammer::Felt::stiffnessFor(keys[m][k].mass, keys[m][k].exponent, keys[m][k].contact);
        }
    }
    for (Voice &v : voices) v.designed = false;
    if (warmed) designSections();
}

void Hammer::reset() {
    for (Voice &v : voices) {
        v.used = v.held = v.designed = v.retiring = false;
        v.course.clear();
        v.felt = hammer::Felt();
        v.damp = v.dampTarget = 0.0f;
        v.gain = v.gainTarget = 0.0f;
        v.force = 0.0f;
        v.phantomIn = v.phantomOut = 0.0f;
        std::fill(std::begin(v.knockLine), std::end(v.knockLine), 0.0f);
        v.knockAt = v.knockLive = 0;
        v.age = 0;
        v.quietBlocks = 0;
        v.bar.clear();
        v.pick.reset();
        v.barX = 0.0f;
        v.speed = 0.0f;
    }
    board.clear();
    amp.clear();
    modWheel = 0.0f;
    sustainPedal = softPedal = 0.0f;
    pedalThump = 0.0f;
    bankNext = 0;
    for (Voice &b : bank) {
        b.used = b.held = b.designed = b.retiring = false;
        b.course.clear();
        b.damp = b.dampTarget = 0.0f;
        b.gain = b.gainTarget = 0.0f;
    }
    bend = 0.0f;
    clock = 0;
    lastContact = 0;
    quietBlocks = 0;
    asleep = true;
}

bool Hammer::fullDetail() const { return fullQuality() || steppedTargetOf(Detail) == 1; }

int Hammer::modelNow() const {
    const int m = steppedTargetOf(Model);
    return m >= 0 && m < kKeyModels ? m : hammer::Grand;
}

namespace {
/** How much of a key is bass, for what a smaller instrument changes: 1 at A0, none from middle C. */
float bassOf(int key) { return clampf((60.0f - static_cast<float>(key)) / 39.0f, 0.0f, 1.0f); }
} // namespace

float Hammer::hzOf(int key, float shifted) const {
    // A smaller instrument's bass strings are shorter and stiffer, and its
    // tuning stretched further to follow them.
    const float smaller = 1.0f - targetOf(Size);
    const float cents = keys[modelNow()][key].stretchCents * targetOf(Stretch) * (1.0f + 0.5f * smaller * bassOf(key));
    return noteHz(shifted) * std::exp2(cents / 1200.0f);
}

hammer::Board::Voicing Hammer::voicing() const {
    hammer::Board::Voicing v;
    switch (modelNow()) {
    case hammer::Upright:
    case hammer::Honky:
        // A board half the size, nearer the wall: less bass, more middle.
        v.radiateHz = 110.0f;
        v.bassBodyDb = 4.0f;
        v.presenceDb = modelNow() == hammer::Honky ? 4.0f : 3.0f;
        v.topDb = modelNow() == hammer::Honky ? -9.0f : -12.0f;
        v.modes = 1.3f;
        v.room = 0.8f;
        break;
    case hammer::Fortepiano:
        v.radiateHz = 120.0f;
        v.body = 0.7f;
        v.bassBodyDb = 3.0f;
        v.presenceDb = 4.0f;
        v.topDb = -9.0f;
        v.modes = 1.6f;
        v.room = 0.8f;
        break;
    case hammer::ElectricGrand:
        // The pickups hear the strings, fundamentals and all, and hardly
        // any board.
        v.radiateHz = 40.0f;
        v.body = 0.0f;
        v.bassBodyDb = 0.0f;
        v.presenceDb = 3.0f;
        v.topDb = -6.0f;
        v.modes = 0.2f;
        v.room = 0.15f;
        break;
    case hammer::Celesta:
        // A cabinet with a box under each bar: warm, no bass to speak of.
        v.radiateHz = 180.0f;
        v.body = 0.5f;
        v.bassBodyDb = 0.0f;
        v.presenceDb = 1.0f;
        v.topDb = -10.0f;
        v.modes = 0.8f;
        break;
    case hammer::Toy:
        // A small plastic box: a honk in the middle, nothing below it.
        v.radiateHz = 300.0f;
        v.body = 0.0f;
        v.bassBodyDb = 0.0f;
        v.presenceDb = 6.0f;
        v.topDb = -6.0f;
        v.modes = 2.0f;
        v.room = 0.6f;
        break;
    case hammer::Dulcimer:
        // A small trapezoid box: bright, a little hollow.
        v.radiateHz = 130.0f;
        v.body = 0.6f;
        v.bassBodyDb = 2.0f;
        v.presenceDb = 4.0f;
        v.topDb = -8.0f;
        v.modes = 1.8f;
        v.room = 0.8f;
        break;
    case hammer::Cimbalom:
        v.radiateHz = 90.0f;
        v.body = 0.8f;
        v.bassBodyDb = 4.0f;
        v.presenceDb = 3.0f;
        v.topDb = -10.0f;
        v.modes = 1.4f;
        break;
    default:
        break;
    }
    const float smaller = 1.0f - targetOf(Size);
    if (modelNow() != hammer::ElectricGrand) {
        v.radiateHz *= 1.0f + 0.6f * smaller;
        v.modes *= 1.0f + 0.3f * smaller;
    }
    // A closed lid takes the top and some of the room.
    const float shut = 1.0f - targetOf(Lid);
    v.topDb -= 6.0f * shut;
    v.presenceDb -= 3.0f * shut;
    v.room *= 1.0f - 0.3f * shut;
    switch (steppedTargetOf(Mic)) {
    case 1: v.presenceDb -= 1.0f; v.room *= 1.6f; break; // audience
    case 2: v.presenceDb += 2.0f; v.topDb += 3.0f; v.room *= 0.4f; break; // close
    case 3: v.presenceDb -= 2.0f; v.topDb -= 3.0f; v.room *= 3.0f; break; // room
    default: break; // player
    }
    return v;
}

namespace {
/** Where the keys sit across the stereo for each mic: the player's, the audience's (the other way round), close, the room. */
constexpr float kMicPan[4] = {1.0f, -0.6f, 1.3f, 0.35f};
} // namespace

hammer::Course::Prep Hammer::prepFor(int key, float impedance, float *makeup) const {
    hammer::Course::Prep p;
    if (makeup != nullptr) *makeup = 1.0f;
    int kind = steppedTargetOf(Prep);
    if (kind <= 0) return p;
    uint32_t h = static_cast<uint32_t>(key) * 2246822519u + static_cast<uint32_t>(steppedTargetOf(Seed)) * 3266489917u + 374761393u;
    auto hash = [&]() { h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16; return static_cast<float>(h >> 8) / 16777216.0f; };
    static constexpr bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    const bool black = kBlack[key % 12];
    bool on = true;
    switch (steppedTargetOf(PrepKeys)) {
    case 1: on = !black; break;
    case 2: on = black; break;
    case 3: on = key < 60; break;
    case 4: on = key >= 60; break;
    case 5: on = hash() < 0.5f; break;
    default: break;
    }
    if (!on) return p;
    // Mixed: each key its own, as the seed has it.
    if (kind == 5) kind = 1 + std::min(3, static_cast<int>(hash() * 4.0f));
    const float amount = clampf(targetOf(PrepAmt), 0.0f, 1.0f);
    // Never quite the same place twice.
    p.at = (0.03f + 0.45f * clampf(targetOf(PrepAt), 0.0f, 1.0f)) * (0.9f + 0.2f * hash());
    // A mass in the strings' units: kg over twice their impedance, a sample at a time.
    const float perKg = sampleRate / (2.0f * impedance);
    // What each takes from a note's loudness, mostly made up: a prepared
    // note is played as loud as any other. (Rubber stops a note within the
    // ear's 400 ms, and needs the most.)
    static constexpr float kMakeup[5] = {1.0f, 3.5f, 2.5f, 2.5f, 2.0f};
    if (makeup != nullptr) *makeup = kMakeup[kind];
    switch (kind) {
    case 1: // rubber, wedged between the strings: mostly a loss, a little spring
        p.loss = 0.08f + 0.7f * amount;
        // (Ten times this was a spring that held the note 27 cents sharp.)
        p.spring = 0.0001f + 0.001f * amount;
        break;
    case 2: // a screw: a mass that goes with the strings
        p.mass = (1.0e-3f + 9.0e-3f * amount) * perKg;
        p.loss = 0.01f;
        break;
    case 3: // a bolt: a little held, the rest loose and rattling
        p.mass = 1.0e-3f * perKg;
        p.rattle = std::fmax(0.2f, (3.0e-3f + 12.0e-3f * amount) * perKg);
        p.gap = 0.5f * swingAt(key) * (1.2f - amount);
        p.rattleLoss = 0.002f;
        break;
    default: // paper, woven through: light, springing back against the strings, buzzing
        p.rattle = std::fmax(0.2f, 0.3e-3f * perKg);
        p.rattleSpring = p.rattle * kPaperSpring;
        // Loose enough to touch only near the top of a loud note's swing:
        // pressed on the strings all the time, it was a spring and the note
        // 24 cents sharp.
        p.gap = 1.5f * swingAt(key) * (1.2f - amount);
        p.rattleLoss = 0.01f;
        p.loss = 0.005f + 0.02f * amount;
        break;
    }
    return p;
}

int Hammer::voiceCap() const {
    static constexpr int kCaps[9] = {4, 6, 8, 10, 12, 16, 20, 24, 32};
    const int asked = kCaps[std::clamp(steppedTargetOf(Voices), 0, 8)];
    // Lean, at most 8: a piano's notes are the heaviest thing a track plays
    // (about 5 us each here, sixty on a phone, and the board on top).
    return fullDetail() ? asked : std::max(4, std::min(asked, 8));
}

int Hammer::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used && !v.retiring ? 1 : 0;
    return n;
}

Hammer::Voice *Hammer::voiceFor(int key) {
    for (Voice &v : voices) if (v.used && v.key == key) return &v;
    Voice *free = nullptr;
    for (Voice &v : voices) {
        if (!v.used) { free = &v; break; }
    }
    if (free != nullptr && activeVoices() < voiceCap()) return free;
    // Full: the quietest let go, or failing that the quietest of all, fades
    // out over 10 ms while the new note takes a free slot. Only with none
    // free is it cut off there and then.
    Voice *best = nullptr;
    float quietest = 1e30f;
    for (int pass = 0; pass < 2 && best == nullptr; ++pass) {
        for (Voice &v : voices) {
            if (!v.used || v.retiring || (pass == 0 && v.held)) continue;
            const float loud = loudnessOf(v);
            if (loud < quietest) { quietest = loud; best = &v; }
        }
    }
    if (best == nullptr) return free;
    if (free != nullptr) {
        best->retiring = true;
        best->held = false;
        best->fade = best->gain / (kRetireTime * sampleRate);
        return free;
    }
    best->designed = false; // a different key's strings
    return best;
}

void Hammer::noteOn(uint8_t note, uint8_t velocity) {
    const float shifted = static_cast<float>(note) + static_cast<float>(steppedTargetOf(Transpose)) +
                          12.0f * static_cast<float>(steppedTargetOf(Octave));
    const int key = std::clamp(static_cast<int>(std::lround(shifted)), 0, kKeys - 1);
    Voice *v = voiceFor(key);
    if (v == nullptr) return;
    v->note = note;
    v->noteBend = 0.0f;
    const int model = modelNow();
    if (isBar(model)) strikeBar(*v, model, key, hzOf(key, shifted + targetOf(Fine) / 100.0f), static_cast<float>(velocity) / 127.0f);
    else strike(*v, key, hzOf(key, shifted + targetOf(Fine) / 100.0f), static_cast<float>(velocity) / 127.0f);
    // A new note takes the wheel where it is.
    bendVoice(*v);
    asleep = false;
}

hammer::Course::Design Hammer::designFor(int key, float hz, bool full) {
    const int model = modelNow();
    const hammer::KeySpec &k = keys[model][key];
    // A smaller instrument's bass strings are shorter: stiffer, and they
    // ring for less. An older one's unisons have drifted, its keys are out
    // of tune with each other, and it rings shorter and darker.
    const float smaller = 1.0f - targetOf(Size), bass = bassOf(key);
    const float age = targetOf(Age);
    hammer::Course::Design d;
    d.hz = hz;
    const int asked = steppedTargetOf(Strings);
    int lanes = asked == 0 ? k.strings : asked;
    if (!full) lanes = std::min(lanes, 2);
    d.lanes = lanes;
    // Tuned a hair apart, each key its own way: the seed and the key choose.
    // Up to 1 the unison knob moves them within what rings as one; past it,
    // the piano goes out of tune, in cents.
    const float unison = targetOf(Unison);
    const float within = k.unison * std::fmin(unison, 1.0f) * (1.0f + 1.5f * age);
    // A honky-tonk is the unisons tuned apart on purpose, in cents, around
    // the string the hammer meets hardest (the first): spread around the
    // middle, the note took the flat string's pitch, 6 cents under.
    const float honky = model == hammer::Honky ? 5.0f : 0.0f;
    static constexpr float kHonky[hammer::Course::kLanes] = {0.0f, 1.0f, -0.85f, 0.0f};
    const float beyond = (unison > 1.0f ? 4.0f * (unison - 1.0f) * (unison - 1.0f) : 0.0f) + 2.5f * age;
    uint32_t h = static_cast<uint32_t>(key) * 2654435761u + static_cast<uint32_t>(steppedTargetOf(Seed)) * 40503u;
    auto jitter = [&]() { h = h * 1664525u + 1013904223u; return 0.7f + 0.6f * static_cast<float>(h >> 8) / 16777216.0f; };
    // The whole key off, with age: up to 3 cents either way.
    const float keyOff = 3.0f * age * (jitter() - 1.0f) / 0.3f;
    // Where each string sits against the others: one flat, one about in the
    // middle, one sharp, never quite evenly spaced.
    static constexpr float kShape[3][3] = {{0.0f, 0.0f, 0.0f}, {-0.5f, 0.5f, 0.0f}, {-1.0f, 0.1f, 0.93f}};
    const auto &shape = kShape[std::clamp(lanes, 1, 3) - 1];
    for (int i = 0; i < hammer::Course::kLanes; ++i) {
        const float place = i < lanes ? shape[i] : 0.0f;
        const float j = jitter();
        d.unison[i] = within * place * j;
        d.detune[i] = beyond * place * j + (i < lanes ? honky * kHonky[i] * j : 0.0f) + keyOff;
    }
    // The strings' motion across the board, if there's a lane left for it:
    // tuned a little off the motion into it.
    // (Not a tangent keyboard's: its pickups hear the strings' motion one way.)
    if (lanes < (full ? hammer::Course::kLanes : 2) && targetOf(Polar) > 0.0f && model != hammer::Tangent) {
        d.polar = std::fmin(1.0f, targetOf(Polar) * k.across);
        d.unison[lanes] = 0.4f * within * jitter();
        d.detune[lanes] = keyOff;
    }
    d.couple = targetOf(Couple);
    d.B = k.B * targetOf(Stiffness) * (1.0f + 3.0f * smaller * bass * bass);
    d.bend = k.bend;
    // The stretch is the string's own f0, as measured; its first partial sits
    // a little above that.
    d.hz = hz * std::sqrt(1.0f + d.B);
    // Stiffness stages: the long bass strings need the most to follow.
    d.stages = key < 40 ? 16 : (key < 60 ? 8 : (key < 76 ? 4 : 2));
    if (!full) d.stages = std::max(1, d.stages / 2);
    // Below C2 they need more than stages can give: sections, which follow
    // the stretch up to 5 kHz (all of it at A0 takes 40; lean, about 1.2).
    d.sections = key < kSectionKeys ? (full ? 40 : 10) : 0;
    // How long the strings ring, as a multiple of the measured times; tone
    // moves the third partial's ring against the fundamental's.
    // A tangent keyboard's mute: a felt strip on the strings by the bridge,
    // shorter and duller.
    const float mute = model == hammer::Tangent ? targetOf(Mute) : 0.0f;
    const float ring = std::pow(4.0f, targetOf(Sustain) + targetOf(SustainKey) * (static_cast<float>(key) - 64.0f) / 44.0f) *
                       (1.0f - 0.4f * smaller * bass) * (1.0f - 0.25f * age) * (1.0f - 0.85f * mute);
    const float tone = std::pow(2.0f, targetOf(Tone) + targetOf(ToneKey) * (static_cast<float>(key) - 64.0f) / 44.0f - 0.6f * age - 1.5f * mute);
    hammer::Course::Decay &t = d.decay;
    t.after1 = k.after1 * ring;
    t.after3 = std::fmin(k.after3 * ring * tone, t.after1);
    t.after7 = std::fmin(k.after7 * ring * tone * tone, t.after3);
    t.prompt1 = std::fmin(k.prompt1 * ring, t.after1);
    t.prompt3 = std::fmin(k.prompt3 * ring * tone, t.after3);
    t.prompt7 = std::fmin(k.prompt7 * ring * tone, t.prompt3);
    d.strike = clampf(k.strike * std::exp2(0.5f * targetOf(Position)), 0.03f, 0.3f);
    d.prep = prepFor(key, k.impedance, nullptr);
    d.highRing = k.highRing;
    if (key < kSectionKeys) d.kept = &kept[key][full ? 1 : 0];
    d.keptLoss = &keptLoss[key][full ? 1 : 0];
    return d;
}

void Hammer::designSections() {
    // On the thread that loads the machine, so a bass key's first note
    // doesn't wait for its sections (50 to 100 us here, ten times that on
    // a phone). Notes played with other settings design their own once.
    for (int key = 21; key < kSectionKeys; ++key) {
        for (bool full : {false, true}) warmer.tune(designFor(key, hzOf(key, static_cast<float>(key)), full));
    }
}

void Hammer::strike(Voice &v, int key, float hz, float velocity01) {
    const int model = modelNow();
    const hammer::KeySpec &k = keys[model][key];
    v.spec = &k;
    v.model = model;
    const bool full = fullDetail();
    const hammer::Course::Design d = designFor(key, hz, full);
    // Struck again with nothing changed, the strings keep ringing: no retune.
    const bool same = v.designed && v.key == key && v.design == d;
    if (!v.used || !same) {
        if (!v.used || v.key != key) v.course.clear();
        v.course.tune(d);
        v.design = d;
        v.designed = true;
    }
    // A tangent keyboard's two pickups, under the strings near the bridge.
    if (model == hammer::Tangent) v.course.setPickups(0.06f, 0.16f);
    v.key = key;
    v.used = true;
    v.held = true;
    v.retiring = false;
    // Off the strings. Struck again while its damper was on its way down (a
    // pedal lifted just before), the strings still have the damper's loss:
    // they get their own back, or the note dies as if let go.
    if (same && v.damp > 0.0f) v.course.setDecay(d.decay);
    v.damp = v.dampTarget = 0.0f;
    // The hammer: its felt by key, harder or softer.
    const float age = targetOf(Age), tacks = targetOf(Tacks), moderator = targetOf(FeltSoft);
    // Old hammers are packed hard where they meet the strings.
    const float hard = targetOf(Hardness) + targetOf(HardKey) * (static_cast<float>(key) - 64.0f) / 44.0f + 0.4f * age;
    // The soft pedal. On an upright it moves the hammers nearer the strings,
    // so they're thrown slower (half blow). Elsewhere it's una corda: the
    // action slides so the hammer misses one string of three (half of one
    // of two) and meets them with felt the strings haven't worn hard:
    // quieter, and darker.
    const float corda = softPedal * targetOf(UnaCorda);
    const bool halfBlow = model == hammer::Upright || model == hammer::Honky;
    const float shift = halfBlow ? 0.0f : corda;
    const float speed = kSlowest * std::pow(kFastest / kSlowest, clampf(velocity01, 0.0f, 1.0f)) *
                        (halfBlow ? 1.0f - 0.35f * corda : 1.0f);
    // Felt hardens the harder it's thrown, faster than its power law says:
    // its inner layers, and what the strings do at ff, which the course
    // doesn't model yet. As it is against the strings, the contact is
    // their impedance's to set and hardly changes with the blow.
    // Tacks in the felt make a soft blow nearly as hard as a loud one; a
    // strip of felt between hammer and strings (the moderator) makes every
    // blow soft.
    // The moderator softens the treble less: a soft blow lasting several
    // periods of a high note cancels itself, and with it on, the top two
    // octaves of an upright were 10 to 15 dB under the middle.
    const float treble = clampf((static_cast<float>(key) - 60.0f) / 36.0f, 0.0f, 1.0f);
    const float K = stiffness[model][key] * std::pow(10.0f, 1.5f * hard - 0.4f * shift + 0.9f * tacks - 1.6f * moderator * (1.0f - 0.6f * treble)) *
                    std::pow(speed / 2.0f, k.hardening * (1.0f - 0.6f * tacks));
    v.felt.set(k.mass * std::exp2(targetOf(Weight)), k.exponent, K, k.impedance, 1.5e-4f);
    // The strings never take the blow quite equally.
    const float u = d.lanes > 1 ? k.uneven : 0.0f;
    float takes[hammer::Course::kLanes] = {1.0f + u, 1.0f - (d.lanes > 2 ? 0.6f : 1.0f) * u, 1.0f - 0.4f * u, 1.0f};
    if (d.lanes == 3) takes[2] *= 1.0f - 0.95f * shift;
    else if (d.lanes == 2) takes[1] *= 1.0f - 0.5f * shift;
    v.felt.strike(speed, sampleRate, takes, d.lanes);
    v.speed = speed;
    // The level is the house velocity law; the physics only sets the colour.
    // What reaches the bridge grows with the hammer's speed and the strings'
    // impedance, so both are divided out.
    float makeup = 1.0f;
    prepFor(key, k.impedance, &makeup);
    v.gainTarget = velocityGain(velocity01, targetOf(VelocityAmount)) * kHouse * k.level * makeup /
                   (speed * k.impedance * static_cast<float>(d.lanes)) * (1.0f - 0.3f * corda) *
                   // The moderator darkens the treble, and took its level too:
                   // with it on, the upright's top half sat 15 to 20 dB under
                   // its bass. The level is given back up the keyboard, 12 dB
                   // by C6; the darkness stays. (A fortepiano's light leather
                   // hammers never lost it.)
                   std::pow(10.0f, (model == hammer::Fortepiano ? 0.0f : 12.0f) * moderator *
                                       clampf((static_cast<float>(key) - 48.0f) / 36.0f, 0.0f, 1.0f) / 20.0f);
    if (v.gain <= 0.0f) v.gain = v.gainTarget;
    // The knock grows more slowly than the note: against it, 4 to 11 dB
    // louder at velocity 30 than at 124 in the recordings. The key's level
    // is the strings' (the top's quiet strings are brought up to the
    // recordings' by 9x), so it's taken back out of the board's thump.
    v.knock = k.knock * std::sqrt(2.0f / speed) / k.level * (1.0f + 1.5f * tacks) * (1.0f + age);
    v.knockDelay = std::clamp(static_cast<int>(v.course.strikeToBridge()), 0, kKnockLine - 1);
    const float pan = clampf(k.pan * targetOf(Width) / 0.7f * kMicPan[std::clamp(steppedTargetOf(Mic), 0, 3)], -1.0f, 1.0f);
    v.panL = std::cos((pan + 1.0f) * 0.785398f);
    v.panR = std::sin((pan + 1.0f) * 0.785398f);
    v.age = clock;
    v.quietBlocks = 0;
    if (!isElectric(model)) wakeSympathy(key);
}

void Hammer::strikeBar(Voice &v, int model, int key, float hz, float velocity01) {
    const hammer::KeySpec &k = keys[model][key];
    if (!v.used || v.key != key) {
        v.bar.clear();
        v.barX = 0.0f;
    }
    v.spec = &k;
    v.model = model;
    v.key = key;
    v.used = true;
    v.held = true;
    v.retiring = false;
    v.designed = false;
    v.damp = v.dampTarget = 0.0f;
    // How long it rings, and how long its bell does, as the strings' knobs say.
    const float ring = std::pow(4.0f, targetOf(Sustain) + targetOf(SustainKey) * (static_cast<float>(key) - 64.0f) / 44.0f);
    const float tone = std::pow(2.0f, targetOf(Tone) + targetOf(ToneKey) * (static_cast<float>(key) - 64.0f) / 44.0f);
    hammer::Bar::Spec b;
    // A toy piano is never quite in tune: each key its own way, up to 15
    // cents, as the seed has it.
    if (model == hammer::Toy) {
        uint32_t h = static_cast<uint32_t>(key) * 2654435761u + static_cast<uint32_t>(steppedTargetOf(Seed)) * 97u + 1u;
        h ^= h >> 13;
        h *= 1274126177u;
        h ^= h >> 16;
        hz *= std::exp2(15.0f * (static_cast<float>(h >> 8) / 8388608.0f - 1.0f) / 1200.0f);
    }
    b.hz = hz;
    // A tine's tonebar, or a celesta's box, which blooms and lets go sooner.
    b.tonebar = model == hammer::Tine ? targetOf(Tonebar) : k.resonatorShare;
    b.ring = k.prompt1 * ring;
    b.barRing = (model == hammer::Celesta ? k.resonator * k.prompt1 : k.after1) * ring;
    b.ratio[0] = k.overtone;
    b.ratio[1] = k.overtone2 > 0.0f ? k.overtone2 : k.overtone * 2.8f;
    const float hard = targetOf(Hardness) + targetOf(HardKey) * (static_cast<float>(key) - 64.0f) / 44.0f;
    b.level[0] = k.overtoneLevel * std::exp2(hard);
    b.level[1] = 0.3f * b.level[0];
    b.overRing[0] = k.overtoneT60 * tone;
    b.overRing[1] = 0.3f * k.overtoneT60 * tone;
    v.barSpec = b;
    v.bar.tune(b);
    // The blow: harder is a shorter one, which reaches the bell.
    const float speed = kSlowest * std::pow(kFastest / kSlowest, clampf(velocity01, 0.0f, 1.0f));
    // No longer than a third of the note's period: a blow three quarters of
    // a period long cancels most of its own push at the note, and the tine's
    // top octave played 25 dB under its middle.
    const float contact = std::fmin(k.contact * std::pow(10.0f, -0.5f * hard) * std::exp2(targetOf(Weight)) * std::pow(speed / 2.0f, -0.3f),
                                    0.33f / hz);
    // A reed swings half as far as a tine for the same blow, nearer its
    // plate (below): soft, it's nearly pure; hard, its 2nd harmonic comes up
    // 16 dB (the recording, 15; with a tine's swing, 11).
    const float swing = kSwing * speed * (model == hammer::Reed ? 0.5f : 1.0f);
    v.bar.strike(swing, contact);
    v.speed = speed;
    // Nearer the pickup bends sooner; further off its axis, purer. A reed
    // already sits off its plate, so offset moves it nearer, and it sits
    // closer to begin with: at a tine's distance its hardest notes had
    // their 3rd harmonic 23 dB under the note, the recording's 1 dB.
    const float reach = (1.6f - 1.2f * targetOf(Pickup)) * (model == hammer::Reed ? 0.4f : 1.0f);
    const float off = model == hammer::Tine ? 1.5f * targetOf(Offset) : 0.15f + 0.25f * targetOf(Offset);
    // A celesta and a toy piano are heard through the air: no pickup's curve.
    const hammer::Pickup::Kind kind = model == hammer::Tine ? hammer::Pickup::Magnetic
                                    : model == hammer::Reed ? hammer::Pickup::Electrostatic : hammer::Pickup::Linear;
    v.pick.set(kind, reach, off, 6.28318530718f * hz / sampleRate);
    v.pick.reset(v.barX);
    // The level is the house law against the swing the pickup hears.
    v.gainTarget = velocityGain(velocity01, targetOf(VelocityAmount)) * kHouse * kElectricLevel * k.level / swing;
    if (v.gain <= 0.0f) v.gain = v.gainTarget;
    // What the case hears of the blow: a celesta's action, a toy's clack.
    // (The blow is in the swing's units and the gain already over the
    // swing: no more of it here.)
    v.knock = k.knock * kBarKnock;
    v.force = 0.0f;
    v.knockLive = 0;
    const float pan = clampf(k.pan * targetOf(Width) / 0.7f, -1.0f, 1.0f);
    v.panL = std::cos((pan + 1.0f) * 0.785398f);
    v.panR = std::sin((pan + 1.0f) * 0.785398f);
    v.age = clock;
    v.quietBlocks = 0;
}

float Hammer::loudnessOf(const Voice &v) const {
    return (isBar(v.model) ? v.bar.loudness() : v.course.loudness() * v.spec->impedance) * v.gain;
}

void Hammer::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}

void Hammer::onBlock(int64_t tickStart, int64_t, float tempo) {
    tick = tickStart;
    bpm = tempo > 1.0f ? tempo : 120.0f;
}

void Hammer::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.held = false;
            v.dampTarget = damperFor(v);
            // A tangent leaving its string lets it snap back as the yarn
            // takes it: a small pluck as the note stops.
            if (v.model == hammer::Tangent && v.dampTarget > 0.0f) v.course.push(0, -0.3f * v.speed);
        }
    }
}

float Hammer::lift() const {
    // A damper starts to leave its strings part of the way down and is clear
    // of them a little further on; between, it only touches, which takes the
    // top of the sound first (applyDamper).
    const float at = targetOf(PedalAt), span = std::fmax(targetOf(PedalSpan), 0.02f);
    const float x = clampf((sustainPedal - (at - 0.5f * span)) / span, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float Hammer::damperFor(const Voice &v) const {
    if (v.held || v.spec == nullptr || !v.spec->damper) return 0.0f;
    // A tangent keyboard's yarn is always on its strings: no pedal lifts it.
    if (v.model == hammer::Tangent) return clampf(targetOf(Dampers), 0.0f, 1.0f);
    return clampf(targetOf(Dampers), 0.0f, 1.0f) * (1.0f - lift());
}

void Hammer::pedal(int32_t which, float level01) {
    if (which == kPerfSoft) {
        softPedal = clampf(level01, 0.0f, 1.0f);
        return;
    }
    if (which != kPerfSustain) return; // sostenuto's notes are held by the rack
    const float was = lift();
    sustainPedal = clampf(level01, 0.0f, 1.0f);
    const float now = lift();
    // The dampers leaving the strings and landing on them again: a thump
    // through the board, more for a pedal stamped than eased.
    if (!isElectric(modelNow())) pedalThump += targetOf(Noises) * (now - was);
    for (Voice &v : voices) {
        if (v.used && !v.held && !v.retiring) v.dampTarget = damperFor(v);
    }
    for (Voice &b : bank) {
        if (b.used) b.dampTarget = damperFor(b);
    }
    if (now > 0.0f) asleep = false;
}

void Hammer::wakeSympathy(int key) {
    const float level = targetOf(Sympathy);
    // A dulcimer's strings are never damped: they always ring along.
    if (level <= 0.0f || (lift() <= 0.0f && modelNow() != hammer::Dulcimer)) return;
    const bool full = fullDetail();
    const int slots = full ? kBank : kLeanBank;
    if (slots <= 0) return;
    // The strongest: an octave either way, then the twelfth and the fifth.
    static constexpr int kRelated[] = {12, -12, 19, 7, -19, 24};
    int woken = 0;
    for (int interval : kRelated) {
        if (woken >= (full ? 2 : 1)) break;
        const int other = key + interval;
        if (other < 21 || other > 108) continue;
        bool taken = false;
        for (const Voice &v : voices) taken = taken || (v.used && v.key == other);
        for (int i = 0; i < slots; ++i) taken = taken || (bank[i].used && bank[i].key == other);
        if (taken) continue;
        Voice &b = bank[bankNext % slots];
        bankNext = (bankNext + 1) % slots;
        hammer::Course::Design d = designFor(other, hzOf(other, static_cast<float>(other)), full);
        // One string each, struck by nothing: what's left of a unison
        // that only the bridge moves.
        d.lanes = 1;
        d.polar = 0.0f;
        for (float &u : d.unison) u = 0.0f;
        for (float &u : d.detune) u = 0.0f;
        if (!b.designed || b.key != other || !(b.design == d)) {
            if (b.key != other) b.course.clear();
            b.course.tune(d);
            b.design = d;
            b.designed = true;
        }
        b.key = other;
        b.spec = &keys[modelNow()][other];
        b.used = true;
        b.held = false;
        b.quietBlocks = 0;
        b.damp = b.dampTarget = damperFor(b);
        applyDamper(b);
        const float pan = clampf(b.spec->pan * targetOf(Width) / 0.7f * kMicPan[std::clamp(steppedTargetOf(Mic), 0, 3)], -1.0f, 1.0f);
        b.panL = std::cos((pan + 1.0f) * 0.785398f);
        b.panR = std::sin((pan + 1.0f) * 0.785398f);
        ++woken;
    }
}

void Hammer::allNotesOff() {
    for (Voice &v : voices) {
        if (!v.used) continue;
        v.held = false;
        if (v.spec != nullptr && v.spec->damper) v.dampTarget = 1.0f;
    }
}

void Hammer::setDampers(bool lifted) { pedal(kPerfSustain, lifted ? 1.0f : 0.0f); }

void Hammer::bendVoice(Voice &v) {
    const float ratio = std::exp2((bend + v.noteBend) / 12.0f);
    if (isBar(v.model)) v.bar.setPitch(ratio);
    else v.course.setPitch(ratio);
}

void Hammer::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.noteBend = semitones;
            bendVoice(v);
        }
    }
}

void Hammer::pitchBend(int16_t value14) {
    bend = static_cast<float>(value14) / 8192.0f * static_cast<float>(steppedTargetOf(BendRange));
    for (Voice &v : voices) {
        if (v.used) bendVoice(v);
    }
}

void Hammer::applyDamper(Voice &v) {
    const hammer::KeySpec &k = *v.spec;
    const float e = clampf(v.damp, 0.0f, 1.0f);
    // A damper's T60 against the free string's, blended on a log scale; the
    // top goes first, as a damper only touching takes the highs.
    const float damped = k.dampedT60 * std::pow(4.0f, targetOf(DampTime));
    auto blend = [&](float free, float stopped, float bias) {
        const float t = clampf(e * bias, 0.0f, 1.0f);
        return std::exp(std::log(free) * (1.0f - t) + std::log(std::fmin(stopped, free)) * t);
    };
    if (isBar(v.model)) {
        const hammer::Bar::Spec &b = v.barSpec;
        v.bar.setRing(blend(b.ring, damped, 1.0f), blend(b.barRing, damped, 1.0f), blend(b.overRing[0], damped * 0.3f, 1.6f),
                      blend(b.overRing[1], damped * 0.3f, 1.6f));
        return;
    }
    const hammer::Course::Decay &free = v.design.decay;
    hammer::Course::Decay t;
    t.after1 = blend(free.after1, damped, 1.0f);
    t.after3 = blend(free.after3, damped * 0.5f, 1.3f);
    t.after7 = blend(free.after7, damped * 0.3f, 1.6f);
    t.prompt1 = blend(free.prompt1, damped, 1.0f);
    t.prompt3 = blend(free.prompt3, damped * 0.5f, 1.3f);
    t.prompt7 = blend(free.prompt7, damped * 0.3f, 1.6f);
    v.course.setDecay(t);
}

bool Hammer::render(float *L, float *R, int32_t frames) {
    params_.tick();
    // The bass keys' sections for what's set now, a key a block.
    const float sig[4] = {static_cast<float>(modelNow()), targetOf(Size), targetOf(Stiffness),
                          targetOf(Stretch) + (fullDetail() ? 10.0f : 0.0f)};
    if (!std::equal(sig, sig + 4, warmFor)) {
        std::copy(sig, sig + 4, warmFor);
        warmKey = 21;
    }
    if (warmKey < kSectionKeys && !isBar(modelNow())) {
        warmer.tune(designFor(warmKey, hzOf(warmKey, static_cast<float>(warmKey)), fullDetail()));
        ++warmKey;
    }
    board.voice(voicing());
    if (asleep) {
        for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
        return true;
    }
    for (int32_t i = 0; i < frames; ++i) busL[i] = busR[i] = lowL[i] = lowR[i] = knockBus[i] = elecL[i] = elecR[i] = 0.0f;
    bool electric = false, acoustic = false;
    const int pickups = steppedTargetOf(Pickups);
    // The board's share of each blow: the knock under the note.
    const float knockLevel = kKnock * paramOf(BoardLevel) / 0.7f;
    const float ramp = 1.0f / (kGainRamp * sampleRate);
    const float phantom = kPhantom * paramOf(Tension) / 0.2f;
    const float damperStep = static_cast<float>(frames) / (kDamperTime * sampleRate);
    bool any = false;
    for (Voice &v : voices) {
        if (!v.used) continue;
        any = true;
        if (v.damp != v.dampTarget) {
            v.damp = v.damp < v.dampTarget ? std::fmin(v.dampTarget, v.damp + damperStep)
                                           : std::fmax(v.dampTarget, v.damp - damperStep);
            applyDamper(v);
        }
        if (isBar(v.model)) {
            // A bar through its pickup, to the amp; a celesta's or a toy's
            // through the air, to the board (their case), its blow too.
            const bool viaAmp = isElectric(v.model);
            (viaAmp ? electric : acoustic) = true;
            float *outL = viaAmp ? elecL : busL, *outR = viaAmp ? elecR : busR;
            const bool knocks = !viaAmp && v.knock > 0.0f;
            for (int32_t i = 0; i < frames; ++i) {
                v.barX = v.bar.step();
                const float s = v.pick.hear(v.barX);
                if (v.retiring) v.gain = std::fmax(0.0f, v.gain - v.fade);
                else v.gain += clampf(v.gainTarget - v.gain, -ramp * v.gainTarget, ramp * v.gainTarget);
                const float out = s * v.gain;
                outL[i] += out * v.panL;
                outR[i] += out * v.panR;
                if (knocks) {
                    const float f = v.bar.blow();
                    knockBus[i] += (f - v.force) * v.gain * v.knock * knockLevel;
                    v.force = f;
                }
            }
            const float loud = loudnessOf(v);
            if (v.retiring && v.gain <= 0.0f) {
                v.used = v.retiring = false;
                v.gain = 0.0f;
            } else if (!v.held && !v.bar.striking() && loud < kSilent) {
                if (++v.quietBlocks > 8) v.used = false;
            } else {
                v.quietBlocks = 0;
            }
            continue;
        }
        const bool tangent = v.model == hammer::Tangent;
        // A tangent keyboard is heard by its pickups, not through a board.
        auto heard = [&]() {
            const float atBridge = v.course.step();
            if (!tangent) return atBridge;
            const float a = v.course.pickup(0), b = v.course.pickup(1);
            return kPickupLevel * (pickups == 0 ? a : pickups == 1 ? b : pickups == 2 ? 0.5f * (a + b) : a - b);
        };
        const float impedance = v.spec->impedance;
        float *busOutL = tangent ? elecL : (v.spec->zone == 0 ? lowL : busL);
        float *busOutR = tangent ? elecR : (v.spec->zone == 0 ? lowR : busR);
        (tangent ? electric : acoustic) = true;
        if (!v.felt.touching() && v.force == 0.0f && v.knockLive == 0 && phantom == 0.0f && !v.retiring) {
            // Only ringing: the strings, the gain and the pan.
            for (int32_t i = 0; i < frames; ++i) {
                const float s = heard();
                v.gain += clampf(v.gainTarget - v.gain, -ramp * v.gainTarget, ramp * v.gainTarget);
                const float out = s * v.gain * impedance;
                busOutL[i] += out * v.panL;
                busOutR[i] += out * v.panR;
            }
        } else for (int32_t i = 0; i < frames; ++i) {
            if (v.felt.touching() || v.force != 0.0f || v.knockLive > 0) {
                // The board radiates the change in the blow's force, not the
                // force: a soft blow's long, smooth push is a click and a thud
                // rather than only a thud. It hears it when the string has
                // carried it to the bridge.
                const float f = v.felt.touching() ? v.felt.step(v.course) : 0.0f;
                v.knockLine[v.knockAt] = (f - v.force) * v.gain * v.knock * knockLevel;
                v.force = f;
                if (!v.felt.touching() && f == 0.0f) lastContact = v.felt.contactSamples();
                knockBus[i] += v.knockLine[(v.knockAt - v.knockDelay) & (kKnockLine - 1)];
                v.knockAt = (v.knockAt + 1) & (kKnockLine - 1);
                v.knockLive = (v.felt.touching() || v.force != 0.0f) ? v.knockDelay + 1 : v.knockLive - 1;
            }
            const float s = heard();
            if (v.retiring) v.gain = std::fmax(0.0f, v.gain - v.fade);
            else v.gain += clampf(v.gainTarget - v.gain, -ramp * v.gainTarget, ramp * v.gainTarget);
            // The strings stretch as they swing, and their tension follows
            // the square of the motion: tones at the sums of the partials,
            // the bright metallic half of a bass note (1.4 to 2.8 kHz was
            // 10 to 16 dB short without them), growing faster than the note.
            const float sq = s * s * phantom;
            v.phantomOut = sq - v.phantomIn + kPhantomPole * v.phantomOut;
            v.phantomIn = sq;
            const float out = (s + v.phantomOut) * v.gain * impedance;
            busOutL[i] += out * v.panL;
            busOutR[i] += out * v.panR;
        }
        // Let go and fallen silent: free.
        const float loud = loudnessOf(v);
        if (v.retiring && v.gain <= 0.0f) {
            v.used = v.retiring = v.designed = false;
            v.gain = 0.0f;
        } else if (!v.held && !v.felt.touching() && loud < kSilent) {
            if (++v.quietBlocks > 8) { v.used = false; v.designed = false; }
        } else {
            v.quietBlocks = 0;
        }
    }
    // The strings nobody played, driven by what the played ones bring the
    // bridge (not by each other, or by themselves).
    const float lifted = lift();
    bool banked = false;
    for (const Voice &b : bank) banked = banked || b.used;
    if (banked) {
        for (int32_t i = 0; i < frames; ++i) excite[i] = busL[i] + busR[i] + lowL[i] + lowR[i];
        const float drive = kSympathyDrive * targetOf(Sympathy);
        for (Voice &b : bank) {
            if (!b.used) continue;
            any = true;
            if (b.damp != b.dampTarget) {
                b.damp = b.damp < b.dampTarget ? std::fmin(b.dampTarget, b.damp + damperStep)
                                               : std::fmax(b.dampTarget, b.damp - damperStep);
                applyDamper(b);
            }
            float *outL = b.spec->zone == 0 ? lowL : busL;
            float *outR = b.spec->zone == 0 ? lowR : busR;
            const float impedance = b.spec->impedance;
            for (int32_t i = 0; i < frames; ++i) {
                const float s = b.course.step();
                b.course.drive(excite[i] * drive / impedance);
                const float out = s * impedance;
                outL[i] += out * b.panL;
                outR[i] += out * b.panR;
            }
            // With the pedal up it goes once it's quiet. A dulcimer's are
            // never damped, so they get a second to start ringing first:
            // freed after eight quiet blocks, a quiet note's never did.
            const bool undamped = b.spec != nullptr && !b.spec->damper;
            if ((lifted <= 0.0f || undamped) && b.course.loudness() * impedance < kSilent) {
                if (++b.quietBlocks > (undamped ? 750 : 8)) b.used = false;
            } else {
                b.quietBlocks = 0;
            }
        }
    }
    // The pedal's thump, as a blow on the board.
    if (pedalThump != 0.0f) {
        knockBus[0] += kPedalThump * pedalThump * paramOf(BoardLevel) / 0.7f;
        pedalThump = 0.0f;
        any = true;
    }
    board.setRoom(paramOf(Tail) / 0.5f, fullDetail());
    const float volume = paramOf(Volume);
    const float pan = paramOf(Pan);
    const float gl = std::cos((pan + 1.0f) * 0.785398f) * 1.41421f, gr = std::sin((pan + 1.0f) * 0.785398f) * 1.41421f;
    // The board only while something's in it or it still rings: the
    // electric pianos don't go through it.
    if (acoustic || banked || knockBus[0] != 0.0f || board.loudness() > 1e-7f) {
        for (int32_t i = 0; i < frames; ++i) {
            float l, r;
            float bl = lowL[i], br = lowR[i];
            board.spreadBass(bl, br);
            board.step(busL[i] + bl, busR[i] + br, knockBus[i], l, r);
            L[i] = l * volume * gl;
            R[i] = r * volume * gr;
        }
    } else {
        for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    }
    if (electric) {
        const int model = modelNow();
        if (isElectric(model)) amp.voice(model == hammer::Tine ? hammer::Amp::Tine : model == hammer::Reed ? hammer::Amp::Reed : hammer::Amp::Tangent);
        // The mod wheel brings the tremolo in, up to all of it.
        const float depth = clampf(targetOf(Tremolo) + (1.0f - targetOf(Tremolo)) * modWheel, 0.0f, 1.0f);
        const int sync = steppedTargetOf(TremSync);
        float rate = paramOf(TremRate), phase = -1.0f;
        if (sync > 0) {
            const float beats = dsp::Lfo::beatsOf(sync - 1);
            rate = bpm / (60.0f * beats);
            // Locked to the song while it moves; stopped, it runs on.
            if (tick != syncedTick) phase = dsp::Lfo::phaseAt(tick, beats);
            syncedTick = tick;
        }
        amp.process(elecL, elecR, frames, paramOf(Drive), kAmpNominal, depth, rate, phase, paramOf(TremWide));
        for (int32_t i = 0; i < frames; ++i) {
            L[i] += elecL[i] * volume * gl;
            R[i] += elecR[i] * volume * gr;
        }
    }
    clock += frames;
    if (!any && board.loudness() < 1e-6f) {
        if (++quietBlocks > 2) { asleep = true; board.clear(); }
    } else {
        quietBlocks = 0;
    }
    return true;
}

} // namespace acidulous::machine
