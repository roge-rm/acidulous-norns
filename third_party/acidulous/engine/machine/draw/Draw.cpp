#include "Draw.h"
#include <engine/core/Constants.h>
#include <engine/dsp/Math.h>
#include <engine/machine/diction/Phones.h>
#include <engine/machine/Voices.h>
#include <engine/machine/draw/DrawHarp.h>
#include <engine/machine/draw/DrawTuning.h>
#include <algorithm>
#include <cmath>

namespace acidulous::machine {

using dsp::clampf;
using namespace draw;

namespace {

/** Level for a note at velocity 100 at its kind's pressure, set so the bank sits with the other machines. */
constexpr float kHouse = 0.107f;
/**
 * A reed sounds louder as it's blown harder; the house law sets the level
 * instead, so this much of that growth is taken out (as a power of the
 * pressure against the kind's own).
 */
constexpr float kLoudPower = 0.5f;
/** The breath's own noise against the reed. */
constexpr float kHiss = 0.03f;
/**
 * A reed behind a valve loses no air to a partner and sounds louder than a
 * hole: this evens a harp's notes played either way.
 */
constexpr float kValvedReed = 0.62f;
/** A harp hole sounds weaker than a lone reed at the same gain: the hiss against it, as a share. */
constexpr float kHarpHiss = 0.5f;
/**
 * White noise smoothed by two 4 Hz poles keeps about a tenth of its swing;
 * this brings it back so a kind's wander is the share it says.
 */
constexpr float kWanderScale = 10.0f;
/** A note's other reeds against its first: two reeds never match. */
constexpr float kOtherReed = 0.8f;
/** A 16' reed and a 4' against an 8', as an accordion's ranks are voiced: level, and how much darker. */
constexpr float kLowRank = 0.9f, kHighRank = 0.6f, kLowDark = 0.5f, kHighDark = 0.7f;
/** The cassotto: a chamber the 16' and first 8' reeds speak into, darker above this, Hz, by this share. */
constexpr float kCassottoHz = 1000.0f, kCassottoDepth = 0.7f;
/** Many reeds drawing on one bellows lower its pressure: the share lost per reed beyond two. */
constexpr float kSag = 0.012f;
/**
 * The bellows shaken: how sharply the pressure dips at each turn (higher is
 * a briefer dip), and how unevenly a hand keeps time, a share.
 */
constexpr float kShakeEdge = 3.0f, kShakeSpread = 0.12f;
/** A note's pull reeds against its push reeds: filed to match, never quite, cents at most. */
constexpr float kPullCents = 2.5f;
/** A pipe's reed against true, cents at most: a shō's doubled octaves beat a few times a second. */
constexpr float kFiled = 1.5f;
/** Bellows turns a quarter note for each setting of `sync` (0 is free, the shake knob's own speed). */
constexpr int kSyncTurns[7] = {0, 1, 2, 3, 4, 6, 8};
/** Breath from a track: the track's peak that blows as hard as the knobs say, and the follower's times, seconds. */
constexpr float kTrackFull = 0.25f, kTrackRise = 0.005f, kTrackFall = 0.08f;
/**
 * A talking harp: how far the words' second formant moves the mouth (as a
 * power of it against a neutral 1500 Hz), how fast the mouth moves between
 * sounds, seconds, and how much of the sound the words' formants colour.
 */
constexpr float kTalkMouth = 0.6f, kTalkGlide = 0.03f, kTalkColour = 0.9f;
/** The mouth's sharpness behind a harp, as the hole tables were made with. */
constexpr float kMouthQ = 6.0f;
/** How often the mouth and hands are moved, samples. */
constexpr int kMouthEvery = 16;
/**
 * The tongue moves as a muscle does, a little past where it's going, Hz and
 * damping; and is never held quite still, a share of the mouth's resonance.
 * An unbent note hardly notices; a bend, which hangs on the mouth, wanders.
 */
constexpr float kTongueHz = 10.0f, kTongueDamping = 0.6f, kTongueWander = 0.12f, kTongueWanderHz = 3.0f;
/**
 * The player's ear: on a bend, the tongue is moved toward the note heard
 * against the note wanted, octaves of mouth a second per octave out; and
 * never further than this from where the tables put it, octaves.
 */
constexpr float kEarGain = 12.0f, kEarReach = 0.15f;
/** A player draws harder into a bend: the breath's rise per semitone bent, up to two. */
constexpr float kBendBreath = 0.8f;
/** Where a bent note starts: this far from the bend toward the unbent note, in octaves of the mouth as a share. */
constexpr float kScoop = 0.3f;
/**
 * The throat's vibrato at full: mostly the airway narrowing, which takes the
 * level down with little change of pitch, and a little of the breath and of
 * the mouth's resonance. Shares of each.
 */
constexpr float kThroatLevel = 0.5f, kThroatPressure = 0.08f, kThroatMouth = 0.004f;
/** No two vibrato cycles alike: how far each one's rate and depth stray. */
constexpr float kVibratoRateSpread = 0.3f, kVibratoDepthSpread = 0.7f;
/** The hands: how fast they move, Hz and damping, and how much they tremble, a share of their travel. */
constexpr float kHandHz = 4.0f, kHandDamping = 0.7f, kHandTremor = 0.05f, kHandTremorHz = 6.0f;
/** Cupped hands: the low-pass open and closed, Hz; and what leaks past the fingers closed. */
constexpr float kCupOpen = 7000.0f, kCupClosed = 900.0f, kCupLeak = 0.25f;
/** Below this a voice is silent. */
constexpr float kSilent = 2e-5f;

bool isHarp(int32_t kind) { return kind <= OctaveHarp; }
/** Kinds whose reeds sound into pipes. */
bool isPipe(int32_t kind) { return kind == Sheng || kind == Sho || kind == Khaen; }
/** How far a pipe's reed is filed from true, cents: fixed for the pipe, so a chord's octaves beat slowly. */
float filedCents(float note) {
    uint32_t h = static_cast<uint32_t>(note * 16.0f) * 2246822519u;
    h ^= h >> 13;
    return kFiled * (static_cast<float>(h & 0xffff) / 32767.5f - 1.0f);
}
/** Kinds blown by bellows a hand moves, which can be shaken. */
bool isBellows(int32_t kind) { return kind == Accordion || kind == Bandoneon || kind == Concertina; }

/** How far a note's pull reed for [rank] sits from its push reed, cents: fixed for the note, as filed. */
float pullCents(int note, int rank) {
    uint32_t h = static_cast<uint32_t>(note * 131 + rank * 7919) * 2654435761u;
    h ^= h >> 15;
    return kPullCents * (static_cast<float>(h & 0xffff) / 32767.5f - 1.0f);
}

/** A valved reed's tuning at [note] (DrawHarp.h), between notes in a straight line. */
float singleTune(float note) {
    const float at = clampf(note - static_cast<float>(kHarpSingleLow), 0.0f, static_cast<float>(kHarpSingleCount - 1));
    const int lo = std::min(kHarpSingleCount - 2, static_cast<int>(at));
    return kHarpSingle[lo] + (kHarpSingle[lo + 1] - kHarpSingle[lo]) * (at - static_cast<float>(lo));
}

/** The mouth for a bend of [quarters] quarter semitones on a hole, as deep as the hole goes. */
float bendMouthAt(const HarpHoleMap &m, float quarters) {
    int deepest = 0;
    while (deepest < 12 && m.bendMouth[deepest + 1] > 0.0f) ++deepest;
    const float q = clampf(quarters, 0.0f, static_cast<float>(deepest));
    const int lo = std::min(static_cast<int>(q), std::max(deepest - 1, 0));
    if (deepest == 0) return m.bendMouth[0];
    const float t = q - static_cast<float>(lo);
    return m.bendMouth[lo] * std::pow(m.bendMouth[lo + 1] / m.bendMouth[lo], t);
}

uint32_t nextRandom(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float white(uint32_t &s) { return static_cast<float>(nextRandom(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

} // namespace

Draw::Draw() { initParams(); }

const ParamDef *Draw::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"model", 0.0f, static_cast<float>(kKinds - 1), static_cast<float>(Accordion), Curve::Stepped, kKinds, ""},
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // How hard the reeds are blown: half to twice the kind's own pressure.
        {"pressure", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"attack", 5.0f, 600.0f, 40.0f, Curve::Exponential, 0, "ms"},
        {"release", 5.0f, 800.0f, 60.0f, Curve::Exponential, 0, "ms"},
        // The reeds' rest gap ("set"): closer speaks sooner and brighter, and chokes sooner.
        {"set", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // The size of each reed's cell: its resonance against the reed's.
        {"chamber", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // The air heard: the breath's hiss and the jet's turbulence, from none to twice the kind's own.
        {"air", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"voices", 1.0f, static_cast<float>(kVoices), static_cast<float>(kVoices), Curve::Stepped, kVoices, ""},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // 0 is auto: the kind's own (an accordion's two 8' reeds); then by footage, as kRegisters.
        {"register", 0.0f, static_cast<float>(kRegisterCount - 1), 0.0f, Curve::Stepped, kRegisterCount, ""},
        // How far a note's reeds are tuned apart: dry at 0, wet at 25 and more.
        {"detune", 0.0f, 40.0f, 15.0f, Curve::Linear, 0, "cents"},
        // A harmonica's key, G up to F#; C by default.
        {"harp key", 0.0f, static_cast<float>(kHarpKeys - 1), 5.0f, Curve::Stepped, kHarpKeys, ""},
        // Like a player (each note on a hole, bent where a player bends it) or straight (each note a reed of its own).
        {"playing", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // The hands round the harp, open to closed; the mod wheel adds to it.
        {"cup", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"vibrato", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"vib rate", 2.0f, 9.0f, 5.0f, Curve::Linear, 0, "Hz"},
        // The 16' and first 8' reeds speaking into a tone chamber.
        {"cassotto", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // The bellows turned back and forth, turns a second; 0 is steady.
        {"shake", 0.0f, 16.0f, 0.0f, Curve::Linear, 0, "/s"},
        // The bellows turned on the song's grid: free (the shake knob's speed), then turns a quarter (kSyncTurns).
        {"sync", 0.0f, 6.0f, 0.0f, Curve::Stepped, 7, ""},
        // Breath from a track: 0 none, else the rack whose level blows the reeds.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
        // How much a harp player's words shape the mouth.
        {"words", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        // Where the register's 4' and 16' ranks sound, semitones: an octave either way, or any interval.
        {"high rank", 1.0f, 24.0f, 12.0f, Curve::Stepped, 24, "st"},
        {"low rank", -24.0f, -1.0f, -12.0f, Curve::Stepped, 24, "st"},
    };
    count = Count;
    return defs;
}

void Draw::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        for (FreeReed &r : v.reeds) r.prepare(sampleRate);
        for (HarpHole &h : v.holes) h.prepare(sampleRate);
        for (PipeReed &p : v.pipes) p.prepare(sampleRate);
    }
    reset();
}

void Draw::reset() {
    uint32_t seed = 0x51f15e5u;
    for (Voice &v : voices) {
        for (FreeReed &r : v.reeds) {
            r.clear();
            r.seed(seed += 0x9e3779b9u);
        }
        for (HarpHole &h : v.holes) {
            h.clear();
            h.seed(seed += 0x9e3779b9u);
        }
        for (PipeReed &p : v.pipes) {
            p.clear();
            p.seed(seed += 0x9e3779b9u);
        }
        v.pipeCount = 0;
        v.holeCount = 0;
        for (float &c : v.chamberLow) c = 0.0f;
        v.wander1 = v.wander2 = 0.0f;
        v.used = v.held = false;
        v.blown = v.aim = 0.0f;
        v.pressure = -1.0f;
        v.gain = v.level = 0.0f;
        v.quietBlocks = 0;
    }
    bend = channelPressure_ = 0.0f;
    lowState = 0.0f;
    bodyX1 = bodyX2 = bodyY1 = bodyY2 = 0.0f;
    dcIn = dcOut = 0.0f;
    shakePhase = 0.0f;
    shakeRate = sag = 1.0f;
    ticks = 0.0;
    trackLevel = 0.0f;
    pendingCount = saidCount = saidAt = 0;
    saidLeft = 0;
    talkF1 = 500.0f;
    talkF2 = 1500.0f;
    talkLips = 0.0f;
    talking = false;
    talkLow1 = talkBand1 = talkLow2 = talkBand2 = 0.0f;
    pulling = false;
    cupLow = cupBand = 0.0f;
    hand = handSpeed = handNoise1 = handNoise2 = 0.0f;
    cupG = 0.0f;
    cupK = cupLeak = 1.0f;
    wheel = vibratoPhase = 0.0f;
    vibratoRate = vibratoDepth = 1.0f;
    mouthCountdown = 0;
    noise = 0x2545f491u;
    clock = 0;
    quietSamples = 0;
    asleep = true;
}

int Draw::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

const FreeReed *Draw::reedFor(uint8_t note) const {
    for (const Voice &v : voices) {
        if (v.used && v.note == note) return &v.reeds[0];
    }
    return nullptr;
}

Draw::Voice *Draw::voiceFor(uint8_t note) {
    const int cap = std::clamp(steppedTargetOf(Voices), 1, kVoices);
    for (int i = 0; i < cap; ++i) {
        if (voices[i].used && voices[i].note == note) return &voices[i];
    }
    for (int i = 0; i < cap; ++i) {
        if (!voices[i].used) return &voices[i];
    }
    // All busy: the one let go longest ago, or failing that the oldest.
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

float Draw::bendOf(const Voice &v) const { return bend * paramOf(BendRange) + v.noteBend; }

ReedMake Draw::makeFor(const Voice &v) const {
    ReedMake k = kMakes_[kKindVoices[v.kind].make];
    // The knobs move each kind's make from where it sits: twice or half either way.
    k.set *= std::pow(2.0f, (clampf(targetOf(Set), 0.0f, 1.0f) - 0.5f) * 1.0f);
    k.cell *= std::pow(2.0f, (clampf(targetOf(Chamber), 0.0f, 1.0f) - 0.5f) * 1.5f);
    k.turbulence *= 2.0f * clampf(targetOf(Air), 0.0f, 1.0f);
    // A finger's slide brings that note's reeds closer to their slots, which brightens them.
    k.set *= std::pow(2.0f, -0.8f * v.slide);
    return k;
}

void Draw::retune(Voice &v) {
    if (v.holeCount > 0) {
        retuneHarp(v);
        return;
    }
    if (v.pipeCount > 0) {
        // Each pipe tuned to its note; its reed filed to sound it (the reed sets the pitch, the pipe rings with it).
        const float *table = kReedTuning[kKindVoices[v.kind].make];
        const ReedMake make = makeFor(v);
        const PipeMake &pipe = kPipeMakes[v.kind - Sheng];
        const float shift = v.baseNote - static_cast<float>(v.note) + bendOf(v);
        for (int i = 0; i < v.pipeCount; ++i) {
            const float sounds = v.pipeNote[i] + shift;
            const int pc = static_cast<int>(std::lround(v.pipeNote[i])) % 12;
            // The shō is tuned in fifths from A.
            const float temper = v.kind == Sho ? kPythagorean[(pc + 12) % 12] : 0.0f;
            const float at = clampf(sounds, 0.0f, 127.0f);
            const int lo = std::min(126, static_cast<int>(at));
            const float cents = table[lo] + (table[lo + 1] - table[lo]) * (at - static_cast<float>(lo));
            const float hz = noteHz(sounds) * std::pow(2.0f, (temper + filedCents(v.pipeNote[i])) / 1200.0f);
            v.pipes[i].make(hz, hz * std::pow(2.0f, cents / 1200.0f), make, pipe);
        }
        return;
    }
    const float note = v.baseNote + bendOf(v);
    // Each reed is filed to sound its note: the table says how far its own
    // frequency sits from what it plays, between notes in a straight line.
    const float *table = kReedTuning[kKindVoices[v.kind].make];
    const ReedMake make = makeFor(v);
    // The register's ranks: 16' an octave down, 4' an octave up, the 8's
    // tuned apart by the detune knob for musette.
    const float apart = clampf(targetOf(Detune), 0.0f, 40.0f);
    const bool cassotto = steppedTargetOf(Cassotto) != 0;
    const draw::Register &reg = kRegisters[v.stops];
    const float high = static_cast<float>(steppedTargetOf(HighRank)), low = static_cast<float>(steppedTargetOf(LowRank));
    for (int i = 0; i < v.count; ++i) {
        const Rank &rank = reg.ranks[i];
        const float sounds = note + (rank.octave > 0 ? high : rank.octave < 0 ? low : 0.0f);
        const float at = clampf(sounds, 0.0f, 127.0f);
        const int lo = std::min(126, static_cast<int>(at));
        float cents = table[lo] + (table[lo + 1] - table[lo]) * (at - static_cast<float>(lo)) + apart * static_cast<float>(rank.apart);
        // Pulled, a bellows instrument sounds its other set of reeds.
        if (pulling && isBellows(v.kind)) cents += pullCents(v.note, i);
        // Free reeds are made from about 27 Hz to 4.5 kHz; keys beyond play the nearest.
        v.reeds[i].make(clampf(noteHz(sounds) * std::pow(2.0f, cents / 1200.0f), 27.5f, 4500.0f), make);
        // A harmonium's octave stop is voiced as strongly as its 8'; an accordion's 4' is quieter and darker.
        const bool fullOctave = v.kind == Harmonium && rank.octave > 0;
        v.rankLevel[i] = (rank.octave < 0 ? kLowRank : fullOctave ? 1.0f : rank.octave > 0 ? kHighRank : 1.0f) * (rank.apart != 0 ? kOtherReed : 1.0f);
        // Darkened by its rank's voicing and, for the 16' and first 8', the cassotto.
        const float own = rank.octave < 0 ? kLowDark : rank.octave > 0 && !fullOctave ? kHighDark : 0.0f;
        const bool inside = cassotto && (rank.octave < 0 || (rank.octave == 0 && rank.apart == 0));
        v.chamber[i] = 1.0f - (1.0f - own) * (inside ? 1.0f - kCassottoDepth : 1.0f);
    }
}

void Draw::planPipes(Voice &v) {
    v.pipeCount = 0;
    const int key = static_cast<int>(v.note);
    const int aitake = v.kind == Sho && steppedTargetOf(Playing) == 0 ? kAitakeFor[key % 12] : -1;
    if (aitake >= 0) {
        // Played like a player, a shō key sounds its chord, at the shō's own pitch, or moved by octaves toward the key.
        const int chord = key % 12 == 9 && key >= 81 ? 9 : aitake;
        const int root = kAitake[chord == 9 ? 9 : chord][0];
        const int rootPc = kAitake[chord][0] % 12;
        (void)root;
        int base = kAitake[chord][0];
        for (int i = 0; i < kPipes && kAitake[chord][i] != 0; ++i) {
            if (kAitake[chord][i] % 12 == key % 12) { base = kAitake[chord][i]; break; }
        }
        (void)rootPc;
        const int octaves = static_cast<int>(std::lround(static_cast<float>(key - base) / 12.0f)) * 12;
        for (int i = 0; i < kPipes && kAitake[chord][i] != 0; ++i) v.pipeNote[v.pipeCount++] = static_cast<float>(kAitake[chord][i] + octaves);
        return;
    }
    // Otherwise the register's ranks, each a pipe: a sheng often doubles at the octave.
    const int knob = std::clamp(steppedTargetOf(Register), 0, kRegisterCount - 1);
    const draw::Register &reg = kRegisters[knob > 0 ? knob : 1];
    const int high = steppedTargetOf(HighRank), low = steppedTargetOf(LowRank);
    for (int i = 0; i < reg.count && i < kPipes; ++i) {
        const int8_t o = reg.ranks[i].octave;
        v.pipeNote[v.pipeCount++] = static_cast<float>(key + (o > 0 ? high : o < 0 ? low : 0));
    }
}

void Draw::planHarp(Voice &v) {
    v.key = std::clamp(steppedTargetOf(HarpKey), 0, kHarpKeys - 1);
    v.holeCount = v.kind == TremoloHarp || v.kind == OctaveHarp ? 2 : 1;
    v.way = Single;
    v.sign = 1.0f;
    v.hole = v.bentBy = 0;
    if (v.kind != Diatonic || steppedTargetOf(Playing) != 0) return;
    // As a player finds it: a hole's own note first, then a bend.
    const int note = static_cast<int>(std::lround(v.baseNote)) - 60 - kHarpKeyShift[v.key];
    for (int h = 0; h < kHarpHoles; ++h) {
        if (note == kRichterBlow[h] || note == kRichterDraw[h]) {
            v.way = Natural;
            v.hole = h;
            v.sign = note == kRichterBlow[h] ? 1.0f : -1.0f;
            return;
        }
    }
    for (int h = 0; h < kHarpHoles; ++h) {
        const int high = std::max(kRichterBlow[h], kRichterDraw[h]), low = std::min(kRichterBlow[h], kRichterDraw[h]);
        if (note > low && note < high && kHarpMaps[v.key][h].bendMouth[4 * (high - note)] > 0.0f) {
            v.way = Bent;
            v.hole = h;
            v.bentBy = high - note;
            v.sign = kRichterDraw[h] > kRichterBlow[h] ? -1.0f : 1.0f;
            return;
        }
    }
    // Anything else, overblows and overdraws among it, sounds on a reed of
    // its own: an overblow is the one reed speaking above its note, and
    // only steady in a narrow band of the breath and the mouth.
}

void Draw::retuneHarp(Voice &v) {
    const ReedMake make = makeFor(v);
    const float wheelBend = bendOf(v);
    if (v.way == Single) {
        // A reed of its own behind a valve; the wheel moves the note.
        const float note = v.baseNote + wheelBend;
        const float apart = clampf(targetOf(Detune), 0.0f, 40.0f);
        for (int h = 0; h < v.holeCount; ++h) {
            // A tremolo harp's second reed is tuned apart; an octave harp's an octave up.
            const float at = v.kind == OctaveHarp && h == 1 ? note + 12.0f : note;
            const float off = v.kind == TremoloHarp && h == 1 ? apart : 0.0f;
            const float hz = noteHz(at) * std::pow(2.0f, (singleTune(at) + off) / 1200.0f);
            v.holes[h].make(clampf(hz, 27.5f, 4500.0f), clampf(hz * 1.122462f, 27.5f, 4500.0f), make);
            v.holes[h].valves(true);
        }
        v.mouthAim = openMouth(noteHz(note), noteHz(note) * 1.122462f);
        v.listening = false;
        v.breathAim = 1.0f;
        return;
    }
    const HarpHoleMap &m = kHarpMaps[v.key][v.hole];
    const float shift = static_cast<float>(kHarpKeyShift[v.key]);
    // What the note's tune leaves over the hole's own notes is put right by moving both reeds.
    float cents = (v.baseNote - std::round(v.baseNote)) * 100.0f;
    const bool bends = v.way == Bent || (kRichterDraw[v.hole] > kRichterBlow[v.hole]) == (v.sign < 0.0f);
    if (bends) {
        // The wheel down bends further with the tongue, as deep as the hole goes; up moves the reeds.
        const float quarters = 4.0f * (static_cast<float>(v.bentBy) + std::fmax(0.0f, -wheelBend));
        v.mouthAim = bendMouthAt(m, quarters);
        // A bend is held by ear, to the note it's meant to be, and blown harder.
        v.listening = quarters > 0.0f;
        v.breathAim = 1.0f + kBendBreath * std::fmin(2.0f, 0.25f * quarters);
        v.wanted = noteHz(v.baseNote + wheelBend);
        cents += 100.0f * std::fmax(0.0f, wheelBend);
    } else {
        v.listening = false;
        v.breathAim = 1.0f;
        v.mouthAim = m.bendMouth[0];
        cents += 100.0f * wheelBend;
    }
    const float blowHz = noteHz(60.0f + shift + static_cast<float>(kRichterBlow[v.hole])) * std::pow(2.0f, (m.blowTune + cents) / 1200.0f);
    const float drawHz = noteHz(60.0f + shift + static_cast<float>(kRichterDraw[v.hole])) * std::pow(2.0f, (m.drawTune + cents) / 1200.0f);
    v.holes[0].make(clampf(blowHz, 27.5f, 4500.0f), clampf(drawHz, 27.5f, 4500.0f), make);
    v.holes[0].valves(false);
}

float Draw::aimFor(const Voice &v) const {
    const KindVoice &k = kKindVoices[v.kind];
    // The knob sets half to twice the kind's pressure; velocity blows harder
    // as far as the velocity knob lets it; pressure (the note's own, or the
    // channel's) adds up to half again.
    const float knob = std::pow(2.0f, (clampf(targetOf(Pressure), 0.0f, 1.0f) - 0.5f) * 2.0f);
    const float amount = clampf(targetOf(VelocityAmount), 0.0f, 1.0f);
    const float byVelocity = 1.0f - amount * 0.6f * (1.0f - v.velocity);
    const float squeeze = v.pressure >= 0.0f ? v.pressure : channelPressure_;
    return std::fmin(k.most, k.pressure * knob * byVelocity * (1.0f + 0.5f * squeeze));
}

void Draw::noteOn(uint8_t note, uint8_t velocity) {
    Voice *v = voiceFor(note);
    const bool sounding = v->used && v->note == note;
    v->kind = std::clamp(steppedTargetOf(Model), 0, kKinds - 1);
    v->note = note;
    v->baseNote = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->held = true;
    v->pressure = -1.0f;
    v->noteBend = v->slide = 0.0f;
    const int knob = std::clamp(steppedTargetOf(Register), 0, kRegisterCount - 1);
    const int32_t stops = isHarp(v->kind) ? 1 : (knob > 0 ? knob : kKindVoices[v->kind].stops);
    const int count = kRegisters[stops].count;
    const int wasHoles = v->holeCount, wasHole = v->hole, wasWay = v->way;
    const float wasSign = v->sign;
    const int wasPipes = v->pipeCount;
    v->holeCount = 0;
    v->pipeCount = 0;
    if (isHarp(v->kind)) planHarp(*v);
    if (isPipe(v->kind)) planPipes(*v);
    // Played again the same way, the reeds go on swinging; otherwise they start from rest.
    const bool same = sounding && stops == v->stops && v->holeCount == wasHoles && v->pipeCount == wasPipes &&
                      (v->holeCount == 0 || (v->hole == wasHole && v->way == wasWay && v->sign == wasSign));
    if (!same) {
        for (FreeReed &r : v->reeds) r.clear();
        for (HarpHole &h : v->holes) h.clear();
        for (PipeReed &p : v->pipes) p.clear();
        v->blown = 0.0f;
    }
    v->count = count;
    v->stops = stops;
    // Reeds sounding together share the level, though more of them are a little louder.
    v->share = v->holeCount > 1 ? 1.0f / std::sqrt(static_cast<float>(v->holeCount))
                                : std::pow(static_cast<float>(v->pipeCount > 0 ? v->pipeCount : count), -0.35f);
    retune(*v);
    if (!same) {
        // A bend is scooped into from a little above, as a player does: the reed speaks at once and is taken down.
        v->mouth = v->holeCount > 0 && v->way == Bent ? v->mouthAim * std::pow(kHarpMaps[v->key][v->hole].bendMouth[0] / v->mouthAim, kScoop) : v->mouthAim;
        v->breath = v->breathAim;
        v->mouthSpeed = v->tongue1 = v->tongue2 = v->ear = 0.0f;
        for (int h = 0; h < v->holeCount; ++h) v->holes[h].shapeMouth(v->mouth, kMouthQ);
    }
    v->aim = aimFor(*v);
    v->struck = std::fmax(v->aim, 1.0f);
    v->squeeze = 1.0f;
    // The level follows the house law; how much louder a harder-blown reed
    // is, is taken out.
    const KindVoice &k = kKindVoices[v->kind];
    v->gain = kHouse * k.level * velocityGain(v->velocity, targetOf(VelocityAmount)) /
              std::pow(std::fmax(v->aim, 1.0f) / k.pressure, kLoudPower);
    if (v->holeCount > 0 && v->way == Single) v->gain *= kValvedReed;
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    // A harp player says the words waiting, over the note.
    if (pendingCount > 0) {
        if (isHarp(v->kind)) planWords();
        else pendingCount = 0;
    }
    asleep = false;
    quietSamples = 0;
}

void Draw::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.held = false;
            v.aim = 0.0f;
        }
    }
}

void Draw::allNotesOff() {
    for (Voice &v : voices) {
        if (v.used && v.held) {
            v.held = false;
            v.aim = 0.0f;
        }
    }
}

void Draw::lyric(const uint8_t *phones, int32_t count) {
    pendingCount = std::clamp(count, 0, kMaxPhones);
    for (int i = 0; i < pendingCount; ++i) pending[i] = phones[i];
}

void Draw::onBlock(int64_t tickStart, int64_t, float bpm) {
    samplesPerTick = static_cast<double>(sampleRate) * 60.0 / (static_cast<double>(bpm > 1.0f ? bpm : 120.0f) * kPPQN);
    // Kept to the sample between blocks; a jump (a locate, a loop) starts again.
    if (std::fabs(ticks - static_cast<double>(tickStart)) > 2.0) ticks = static_cast<double>(tickStart);
}

void Draw::planWords() {
    int32_t tableSize = 0;
    const diction::Phone *table = diction::phoneTable(tableSize);
    // The held sound: the last vowel, or failing one the last sound. Sounds after it are left unsaid.
    int held = -1;
    for (int i = 0; i < pendingCount; ++i) {
        const int c = pending[i];
        if (c <= 0 || c >= tableSize) continue;
        if (table[c].kind == diction::Kind::Vowel || table[c].kind == diction::Kind::Diphthong || held < 0) held = i;
    }
    saidCount = 0;
    const auto add = [&](const float *f, bool lips, float seconds) {
        if (saidCount == kSaid) return;
        said[saidCount++] = {f[0], f[1], lips, seconds < 0.0f ? -1 : std::max(1, static_cast<int32_t>(seconds * sampleRate))};
    };
    for (int i = 0; i <= held; ++i) {
        const int c = pending[i];
        if (c <= 0 || c >= tableSize) continue;
        const diction::Phone &ph = table[c];
        const bool last = i == held;
        const bool lips = ph.place == diction::Place::Lips;
        if (ph.kind == diction::Kind::Diphthong) {
            add(ph.f, lips, last ? 0.12f : ph.length * 0.5f);
            add(ph.to, lips, last ? -1.0f : ph.length * 0.5f);
        } else {
            add(ph.f, lips, last ? -1.0f : std::fmax(0.03f, ph.length * 0.7f));
        }
    }
    pendingCount = 0;
    saidAt = 0;
    saidLeft = saidCount > 0 ? said[0].samples : 0;
    talking = saidCount > 0;
}

void Draw::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Draw::pitchBend(int16_t value14) {
    bend = static_cast<float>(value14) / 8192.0f;
    for (Voice &v : voices) {
        if (v.used) retune(v);
    }
}

void Draw::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.noteBend = semitones;
            retune(v);
        }
    }
}

void Draw::noteTimbre(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.slide = static_cast<float>(value) / 127.0f;
            retune(v);
        }
    }
}

void Draw::channelPressure(uint8_t value) {
    channelPressure_ = static_cast<float>(value) / 127.0f;
    for (Voice &v : voices) {
        if (v.used && v.held) v.aim = aimFor(v);
    }
}

void Draw::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.pressure = static_cast<float>(value) / 127.0f;
            v.aim = aimFor(v);
        }
    }
}

bool Draw::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const KindVoice &body = kKindVoices[std::clamp(steppedTargetOf(Model), 0, kKinds - 1)];
    // The body: a one-pole low-pass, and a resonance added on top (a band-pass, peak gain 1).
    const float lowPole = std::exp(-6.2831853f * body.lowPass / sampleRate);
    const float bw = 6.2831853f * body.bodyHz / sampleRate;
    const float q = std::fmax(0.3f, body.bodyHz / std::fmax(1.0f, body.bodyWidth));
    const float alpha = std::sin(bw) / (2.0f * q), a0 = 1.0f + alpha;
    const float bb0 = alpha / a0, ba1 = -2.0f * std::cos(bw) / a0, ba2 = (1.0f - alpha) / a0;
    const float dcPole = 1.0f - 6.2831853f * 20.0f / sampleRate;
    const float airAmount = clampf(paramOf(Air), 0.0f, 1.0f);
    const float volume = paramOf(Volume);
    const float attack = std::fmax(0.005f, targetOf(Attack) * 0.001f) * sampleRate;
    const float release = std::fmax(0.005f, targetOf(Release) * 0.001f) * sampleRate;
    const float squeezeFollow = 1.0f - std::exp(-1.0f / (0.02f * sampleRate));
    const float wanderFollow = 1.0f - std::exp(-6.2831853f * 4.0f / sampleRate);
    // The tongue and hands move every kMouthEvery samples, as a muscle does:
    // a spring toward where they're going, a little underdamped.
    const float control = static_cast<float>(kMouthEvery) / sampleRate;
    const float tongueW = 6.2831853f * kTongueHz, handW = 6.2831853f * kHandHz;
    const float tongueNoiseFollow = 1.0f - std::exp(-6.2831853f * kTongueWanderHz * control);
    const float handNoiseFollow = 1.0f - std::exp(-6.2831853f * kHandTremorHz * control);
    // The throat's vibrato moves the breath and, a little, the mouth.
    const float vibrato = clampf(paramOf(Vibrato), 0.0f, 1.0f);
    const float vibratoStep = clampf(paramOf(VibratoRate), 2.0f, 9.0f) / sampleRate;
    const float handAim = clampf(paramOf(Cup) + wheel, 0.0f, 1.0f);
    const float breathFollow = 1.0f - std::exp(-1.0f / (0.03f * sampleRate));
    const float chamberFollow = 1.0f - std::exp(-6.2831853f * kCassottoHz / sampleRate);
    // One bellows for every note: the more reeds draw on it, the lower it sits.
    {
        int drawing = 0;
        for (const Voice &v : voices) {
            if (v.used && v.holeCount == 0 && v.blown > 0.0f && (isBellows(v.kind) || v.kind == Harmonium)) drawing += v.count;
        }
        const float sagAim = 1.0f / (1.0f + kSag * static_cast<float>(std::max(0, drawing - 2)));
        sag += (sagAim - sag) * (1.0f - std::exp(-static_cast<float>(frames) / (0.1f * sampleRate)));
    }
    const float shakeHz = clampf(paramOf(Shake), 0.0f, 16.0f) * 0.5f;
    // On the grid, the bellows turn kSyncTurns a quarter: their phase is the song's clock.
    const int syncTurns = kSyncTurns[std::clamp(steppedTargetOf(Sync), 0, 6)];
    const double ticksPerTurn = syncTurns > 0 ? static_cast<double>(kPPQN) / syncTurns : 0.0;
    const double tickStep = 1.0 / samplesPerTick;
    // Breath from a track: its level, followed, blows the reeds.
    const float *track = sidechainRack() >= 0 ? key_ : nullptr;
    const bool fromTrack = sidechainRack() >= 0;
    const float trackRise = 1.0f - std::exp(-1.0f / (kTrackRise * sampleRate));
    const float trackFall = 1.0f - std::exp(-1.0f / (kTrackFall * sampleRate));
    // A harp player's words: the mouth's formants glide between sounds.
    const float words = clampf(paramOf(Words), 0.0f, 1.0f);
    const bool harpModel = isHarp(std::clamp(steppedTargetOf(Model), 0, kKinds - 1));
    const float talkFollow = 1.0f - std::exp(-static_cast<float>(kMouthEvery) / (kTalkGlide * sampleRate));

    // Each voice's breath or bellows: a straight rise to the pressure over
    // the attack, and down over the release.
    float rise[kVoices] = {}, fall[kVoices] = {};
    for (int n = 0; n < kVoices; ++n) {
        const Voice &v = voices[n];
        if (!v.used) continue;
        const KindVoice &k = kKindVoices[v.kind];
        rise[n] = k.pressure / attack;
        fall[n] = k.pressure / release;
    }

    bool any = false;
    for (int32_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        vibratoPhase += vibratoStep * vibratoRate;
        if (vibratoPhase >= 1.0f) {
            // Each cycle its own speed and depth, as a throat's are.
            vibratoPhase -= 1.0f;
            vibratoRate = 1.0f + kVibratoRateSpread * white(noise);
            vibratoDepth = 1.0f + kVibratoDepthSpread * white(noise);
        }
        const float throat = vibrato > 0.0f ? vibrato * vibratoDepth * std::sin(6.2831853f * vibratoPhase) : 0.0f;
        // The bellows shaken: the pressure dips at each turn, and the other set of reeds speaks.
        float shake = 1.0f;
        const double tickWas = ticks;
        ticks += tickStep;
        if (syncTurns > 0 || shakeHz > 0.0f) {
            const float was = shakePhase;
            if (syncTurns > 0) {
                // A push and a pull are two turns of the grid.
                const double turns = ticks / ticksPerTurn;
                shakePhase = static_cast<float>(0.5 * turns - std::floor(0.5 * turns));
                (void)tickWas;
            } else {
                shakePhase += shakeHz * shakeRate / sampleRate;
                if (shakePhase >= 1.0f) shakePhase -= 1.0f;
            }
            if ((was < 0.5f) != (shakePhase < 0.5f)) {
                pulling = !pulling;
                // A hand on its own keeps time unevenly; on the grid the song keeps it.
                shakeRate = syncTurns > 0 ? 1.0f : 1.0f + kShakeSpread * white(noise);
                for (Voice &v : voices) {
                    if (v.used && isBellows(v.kind)) retune(v);
                }
            }
            shake = std::fmin(1.0f, kShakeEdge * std::fabs(std::sin(6.2831853f * shakePhase)));
        }
        // The other track's level, followed quickly up and slowly down.
        float fromTrackScale = 1.0f;
        if (fromTrack) {
            const float x = track != nullptr ? std::fabs(track[i]) : 0.0f;
            trackLevel += (x - trackLevel) * (x > trackLevel ? trackRise : trackFall);
            fromTrackScale = std::fmin(1.5f, trackLevel / kTrackFull);
        }
        const bool reshape = --mouthCountdown <= 0;
        if (reshape) {
            mouthCountdown = kMouthEvery;
            // The words: step through the sounds, the mouth gliding to each.
            if (talking) {
                if (saidLeft > 0) {
                    saidLeft -= kMouthEvery;
                    if (saidLeft <= 0 && saidAt + 1 < saidCount) saidLeft = said[++saidAt].samples;
                }
                const Said &now = said[saidAt];
                talkF1 += (now.f1 - talkF1) * talkFollow;
                talkF2 += (now.f2 - talkF2) * talkFollow;
                talkLips += ((now.lips ? 1.0f : 0.0f) - talkLips) * talkFollow;
            }
            // The hands, trembling only when they're round the harp.
            handNoise1 += (white(noise) - handNoise1) * handNoiseFollow;
            handNoise2 += (handNoise1 - handNoise2) * handNoiseFollow;
            // Lips shut on a word close the hands too, as a talking-harp player cups on m, b, p and w.
            const float aimNow = clampf(handAim + (talking ? words * talkLips : 0.0f), 0.0f, 1.0f);
            const float tremor = kHandTremor * kWanderScale * handNoise2 * std::fmin(1.0f, 4.0f * aimNow);
            handSpeed += (handW * handW * (aimNow + tremor - hand) - 2.0f * kHandDamping * handW * handSpeed) * control;
            hand = clampf(hand + handSpeed * control, 0.0f, 1.0f);
            if (hand > 1e-4f) {
                cupG = std::tan(3.14159265f * kCupOpen * std::pow(kCupClosed / kCupOpen, hand) / sampleRate);
                cupK = 1.0f / (0.6f + 0.4f * hand);
                cupLeak = 1.0f - (1.0f - kCupLeak) * hand;
            }
        }
        for (int n = 0; n < kVoices; ++n) {
            Voice &v = voices[n];
            if (!v.used) continue;
            if (v.blown < v.aim) v.blown = std::fmin(v.aim, v.blown + rise[n]);
            else if (v.blown > v.aim) v.blown = std::fmax(v.aim, v.blown - fall[n]);
            const KindVoice &k = kKindVoices[v.kind];
            // No breath or bellows is perfectly even: a slow wander below about 4 Hz.
            const float w = white(noise);
            v.wander1 += (w - v.wander1) * wanderFollow;
            v.wander2 += (v.wander1 - v.wander2) * wanderFollow;
            const float blowing = v.blown * (1.0f + k.wander * kWanderScale * v.wander2) * fromTrackScale;
            // Pressure added after the note started is louder as well as
            // brighter, as a player squeezing a key expects.
            if (v.held) v.squeeze += (v.aim / v.struck - v.squeeze) * squeezeFollow;
            float out = 0.0f;
            if (v.holeCount > 0) {
                if (reshape) {
                    // The ear: a bend heard sharp takes the tongue down, flat up.
                    // Anything far off the note (the reed not yet speaking, or caught on the other's) isn't listened to.
                    const float heard = v.holes[0].heard();
                    const float off = heard > 0.0f ? std::log2(heard / v.wanted) : 1.0f;
                    if (v.listening && std::fabs(off) < 0.1f) {
                        v.ear = clampf(v.ear - kEarGain * off * control, -kEarReach, kEarReach);
                    } else {
                        v.ear *= 1.0f - 4.0f * control;
                    }
                    // The tongue, in octaves of the mouth's resonance, and its unsteadiness.
                    const float at = std::log2(v.mouth);
                    v.mouthSpeed += (tongueW * tongueW * (std::log2(v.mouthAim) + v.ear - at) - 2.0f * kTongueDamping * tongueW * v.mouthSpeed) * control;
                    v.mouth = std::exp2(at + v.mouthSpeed * control);
                    v.tongue1 += (white(noise) - v.tongue1) * tongueNoiseFollow;
                    v.tongue2 += (v.tongue1 - v.tongue2) * tongueNoiseFollow;
                    // A talking player's tongue moves with the words' second formant, and the note bends with it where it can.
                    const float talk = talking ? std::pow(talkF2 / 1500.0f, kTalkMouth * words) : 1.0f;
                    const float shaped = v.mouth * talk * (1.0f + kThroatMouth * throat) * (1.0f + kTongueWander * kWanderScale * v.tongue2);
                    for (int h = 0; h < v.holeCount; ++h) v.holes[h].shapeMouth(shaped, kMouthQ);
                }
                v.breath += (v.breathAim - v.breath) * breathFollow;
                const float breath = blowing * v.breath * (1.0f + kThroatPressure * throat) * v.sign;
                for (int h = 0; h < v.holeCount; ++h) {
                    const float one = v.holes[h].step(breath) * (h == 0 ? 1.0f : kOtherReed);
                    if (std::isfinite(one)) out += one;
                    else v.holes[h].clear();
                }
            } else if (v.pipeCount > 0) {
                for (int p = 0; p < v.pipeCount; ++p) {
                    const float one = v.pipes[p].step(blowing, k.supply);
                    if (std::isfinite(one)) out += one;
                    else v.pipes[p].clear();
                }
            } else {
                const bool bellows = isBellows(v.kind) || v.kind == Harmonium;
                const float wind = blowing * (bellows ? sag : 1.0f) * (isBellows(v.kind) ? shake : 1.0f);
                for (int r = 0; r < v.count; ++r) {
                    // No two reeds are quite alike: each rank voiced against the first.
                    float one = v.reeds[r].step(wind, k.supply) * v.rankLevel[r];
                    // A reed driven somewhere it can't follow starts again rather than sounding.
                    if (!std::isfinite(one)) {
                        v.reeds[r].clear();
                        continue;
                    }
                    if (v.chamber[r] > 0.0f) {
                        v.chamberLow[r] += (one - v.chamberLow[r]) * chamberFollow;
                        one -= v.chamber[r] * (one - v.chamberLow[r]);
                    }
                    out += one;
                }
            }
            out *= v.gain * v.squeeze * v.share;
            if (v.holeCount > 0) out *= 1.0f + kThroatLevel * 0.5f * throat;
            // The breath's own noise, as strong as it's blown.
            const float hiss = white(noise) * 2.0f * airAmount * kHiss * (v.holeCount > 0 ? kHarpHiss : 1.0f) * v.gain * v.blown / k.pressure;
            const float s = out + hiss;
            v.level += (std::fabs(out) - v.level) * 0.001f;
            sum += s;
        }
        lowState = sum + lowPole * (lowState - sum);
        const float bodyIn = lowState;
        const float bp = bb0 * bodyIn - bb0 * bodyX2 - ba1 * bodyY1 - ba2 * bodyY2;
        bodyX2 = bodyX1;
        bodyX1 = bodyIn;
        bodyY2 = bodyY1;
        bodyY1 = bp;
        float wet = bodyIn + body.bodyLift * bp;
        // The words' formants on the harp's sound: two band-passes, the first strongest.
        if (talking && harpModel && words > 0.0f) {
            const float g1 = std::tan(3.14159265f * clampf(talkF1, 150.0f, 0.4f * sampleRate) / sampleRate);
            const float g2 = std::tan(3.14159265f * clampf(talkF2, 300.0f, 0.4f * sampleRate) / sampleRate);
            const float kq = 1.0f / 6.0f;
            const auto band = [&](float g, float &low, float &bandState) {
                const float hi = (wet - (kq + g) * bandState - low) / (1.0f + g * (kq + g));
                const float b = g * hi + bandState;
                const float l = g * b + low;
                bandState = g * hi + b;
                low = g * b + l;
                return b * kq;
            };
            const float b1 = band(g1, talkLow1, talkBand1), b2 = band(g2, talkLow2, talkBand2);
            const float colour = kTalkColour * words;
            wet = wet * (1.0f - colour) + colour * 3.0f * (b1 + b2);
        }
        if (hand > 1e-4f) {
            // A state-variable low-pass, trapezoidal.
            const float high = (wet - (cupK + cupG) * cupBand - cupLow) / (1.0f + cupG * (cupK + cupG));
            const float band = cupG * high + cupBand;
            const float low = cupG * band + cupLow;
            cupBand = cupG * high + band;
            cupLow = cupG * band + low;
            // Some always gets past the fingers.
            wet = cupLeak * wet + (1.0f - cupLeak) * low;
        }
        const float y = wet - dcIn + dcPole * dcOut;
        dcIn = wet;
        dcOut = y;
        L[i] = R[i] = y * volume;
        any = any || std::fabs(y) > kSilent;
    }

    for (Voice &v : voices) {
        if (!v.used) continue;
        if (!v.held && v.blown <= 0.0f && v.level < kSilent) {
            if (++v.quietBlocks > 8) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
    }
    quietSamples = any || activeVoices() > 0 ? 0 : quietSamples + frames;
    if (quietSamples > 8192) asleep = true;
    return true;
}

} // namespace acidulous::machine
