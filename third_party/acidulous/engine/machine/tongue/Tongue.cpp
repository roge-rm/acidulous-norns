#include "Tongue.h"
#include <engine/core/Constants.h>
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <complex>

namespace acidulous::machine {

using dsp::clampf;
using namespace tongue;

namespace {

/** Level for a pluck at velocity 100 through the mouth's "ah", set so the bank sits with the other machines. */
constexpr float kHouse = 0.635f;
/** Air through the slot against the reed's own push, before breath. */
constexpr float kAirLevel = 0.4f;
/** The gap left when the reed fills the slot, in slot half-widths. */
constexpr float kGap = 0.05f;
/** Where breath keeps the reed swinging, in the swing's units. */
constexpr float kHeldSwing = 0.6f;
/** Below this a voice is silent. */
constexpr float kSilent = 2e-5f;

uint32_t nextRandom(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float white(uint32_t &s) { return static_cast<float>(nextRandom(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

/** Where the knobs sit by default: a kind's own set, edge and pluck are what these mean. */
constexpr float kSetHome = 0.8f, kEdgeHome = 0.55f, kPluckHome = 0.6f;

/** Each chord's reeds, in semitones from the note. Unison is a few cents apart, as two harps never quite agree. */
constexpr float kChordSteps[Tongue::kChords][5] = {
    {0, 0, 0, 0, 0},
    {0, 0.07f, -0.06f, 0.12f, -0.11f},
    {0, 12, 24, -12, 36},
    {0, 7, 12, 19, 24},
    {0, 4, 7, 12, 16},
    {0, 3, 7, 12, 15},
    {0, 2, 4, 7, 9},
};

/** A plucking rhythm: a step length in ticks and each step's weight, 0 for a rest. */
struct Rhythm {
    int32_t step;
    int32_t length;
    float weight[16];
};
constexpr int32_t k16th = kPPQN / 4;
constexpr Rhythm kRhythms[Tongue::kPatterns] = {
    {k16th, 1, {0}},
    {kPPQN / 2, 2, {1.0f, 0.7f}},
    {k16th, 4, {1.0f, 0.55f, 0.75f, 0.55f}},
    // Long, short short: an eighth and two sixteenths.
    {k16th, 4, {1.0f, 0.0f, 0.65f, 0.65f}},
    {kPPQN / 3, 3, {1.0f, 0.6f, 0.6f}},
    // Quick runs of pulls and a rest, as a genggong is played.
    {kPPQN / 8, 8, {1.0f, 0.6f, 0.6f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f}},
    // Sixteenths grouped 4 + 3 + 3 + 3 + 3, each group's first stressed.
    {k16th, 16, {1.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f}},
};
/** The picking resonance's share against the vowel's formants: it leads. */
constexpr float kPickShare = 2.0f;

/**
 * A speech formant (Diction's sounds) where a harp player's mouth puts it:
 * higher, as kMouths is. A straight line per formant through Diction's knob
 * vowels and Tongue's mouths, so the vowels keep their order.
 */
float harpFormant(int k, float hz) {
    static constexpr float kSlope[3] = {2.184f, 0.692f, 1.016f};
    static constexpr float kOffset[3] = {-96.7f, 1142.9f, 485.6f};
    return kSlope[k] * hz + kOffset[k];
}
/** A word's steps glide over this long, seconds: quicker than the glide knob, as speech is. */
constexpr float kSayGlide = 0.02f;
/** A word's hiss against a sound's own noise level: a talking player hisses hard to be heard over the buzz. */
constexpr float kSayHiss = 8.0f;

} // namespace

// Steel, khomus, morsing, brass, munnharpe and temir komuz are fitted to
// recordings (tools/tongue_reference/fit.py); the bamboo kinds, which no open
// recording covers, from what's written about them and by ear.
//                                             set   edge  pluck ring  over  ratio2 ratio3 lowCut frameHz ring   level pull  air  level reeds chord
const Tongue::KindVoice Tongue::kKindVoices[kKinds] = {
    /* steel      */ {0.80f, 0.55f, 0.60f, 1.00f, 0.30f, 6.267f, 17.55f, 0.0f, 4500.0f, 0.08f, 0.05f, 0.0f, 1.0f, 1.00f, 1, Unison},
    /* munnharpe  */ {0.60f, 0.30f, 0.60f, 0.80f, 0.30f, 6.267f, 17.55f, 400.0f, 4000.0f, 0.10f, 0.05f, 0.0f, 0.8f, 1.60f, 1, Unison},
    /* khomus     */ {0.80f, 0.55f, 0.60f, 1.00f, 0.30f, 6.267f, 17.55f, 0.0f, 3800.0f, 0.10f, 0.05f, 0.0f, 1.2f, 0.98f, 1, Unison},
    /* morsing    */ {1.00f, 0.30f, 1.00f, 0.60f, 0.25f, 6.267f, 17.55f, 0.0f, 2500.0f, 0.15f, 0.10f, 0.0f, 0.8f, 1.33f, 1, Unison},
    /* temir      */ {0.80f, 0.95f, 0.60f, 0.90f, 0.30f, 6.267f, 17.55f, 0.0f, 5000.0f, 0.10f, 0.05f, 0.0f, 0.8f, 1.35f, 1, Unison},
    /* brass      */ {1.00f, 0.95f, 1.00f, 1.20f, 0.40f, 5.600f, 15.50f, 200.0f, 3200.0f, 0.25f, 0.25f, 0.0f, 1.5f, 1.02f, 1, Unison},
    /* bamboo     */ {0.60f, 0.35f, 0.50f, 0.25f, 0.20f, 5.300f, 14.50f, 150.0f, 1400.0f, 0.04f, 0.30f, 0.0f, 1.8f, 1.92f, 1, Unison},
    /* mukkuri    */ {0.60f, 0.40f, 0.50f, 0.35f, 0.20f, 5.300f, 14.50f, 150.0f, 1100.0f, 0.05f, 0.35f, 25.0f, 1.5f, 1.52f, 1, Unison},
    /* genggong   */ {0.70f, 0.50f, 0.60f, 0.30f, 0.25f, 5.500f, 15.00f, 150.0f, 1600.0f, 0.06f, 0.40f, 12.0f, 1.3f, 1.53f, 1, Unison},
    /* kouxian    */ {0.70f, 0.60f, 0.60f, 0.50f, 0.30f, 5.600f, 15.00f, 150.0f, 2600.0f, 0.10f, 0.15f, 0.0f, 1.2f, 1.20f, 3, Pentatonic},
};

Tongue::Tongue() { initParams(); }

const ParamDef *Tongue::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"model", 0.0f, static_cast<float>(kKinds - 1), 0.0f, Curve::Stepped, kKinds, ""},
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // Where the reed rests in its slot: off the middle, the two pulses a
        // cycle part unevenly and the even harmonics come up against the odd.
        {"set", -1.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        // How closely the reed fits its slot: closer is sharper pulses, brighter.
        {"edge", 0.0f, 1.0f, 0.55f, Curve::Linear, 0, ""},
        {"ring", 0.2f, 10.0f, 6.0f, Curve::Exponential, 0, "s"},
        {"pluck", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"overtones", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"mouth", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"focus", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        // How much of the sound comes through the mouth, against straight
        // out of the slot: the formants' peaks over a flat drone.
        {"depth", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"glide", 5.0f, 500.0f, 60.0f, Curve::Exponential, 0, "ms"},
        {"breath", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"air", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"sustain", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"stop", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"voices", 1.0f, 4.0f, 1.0f, Curve::Stepped, 4, ""},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // 0 is auto: the kind's own.
        {"reeds", 0.0f, static_cast<float>(kReeds), 0.0f, Curve::Stepped, kReeds + 1, ""},
        {"chord", 0.0f, static_cast<float>(kChords - 1), 0.0f, Curve::Stepped, kChords, ""},
        {"strum", 0.0f, 150.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"order", 0.0f, static_cast<float>(kOrders - 1), 0.0f, Curve::Stepped, kOrders, ""},
        {"play", 0.0f, static_cast<float>(kPlays - 1), 0.0f, Curve::Stepped, kPlays, ""},
        // The drone's note in mouth mode, as MIDI.
        {"drone", 36.0f, 72.0f, 50.0f, Curve::Stepped, 37, ""},
        {"repluck", 0.0f, static_cast<float>(kReplucks - 1), 1.0f, Curve::Stepped, kReplucks, ""},
        {"pattern", 0.0f, static_cast<float>(kPatterns - 1), 0.0f, Curve::Stepped, kPatterns, ""},
        {"accent", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"ratchet", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Another track the mouth follows: 0 for none, 1..16 for a track.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
        // How far a clip's words move the mouth, against the vowel knob.
        {"words", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Tongue::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        for (Tine &t : v.tines) t.reed.prepare(sampleRate);
    }
    follower.prepare(sampleRate);
    reset();
}

void Tongue::reset() {
    for (Voice &v : voices) {
        for (Tine &t : v.tines) {
            t.reed.clear();
            t.gain = t.q = t.air = t.level = t.swing = 0.0f;
            t.wait = -1;
        }
        v.knock.clear();
        v.used = v.held = false;
        v.next = 0;
        v.quietBlocks = 0;
    }
    for (int k = 0; k < 3; ++k) {
        mouth[k].clear();
        mouthAir[k].clear();
    }
    bend = wheel = pressure = blown = 0.0f;
    dcIn = dcOut = cutIn = cutOut = 0.0f;
    noise = 0x9e3779b9u;
    clock = 0;
    controlLeft = 0;
    quietSamples = 0;
    asleep = true;
    for (int k = 0; k < 3; ++k) formants[k] = kMouths[2][k];
    pick.clear();
    pickOn = false;
    follower.reset();
    pendingCount = saidCount = saidAt = 0;
    saidLeft = 0;
    saying = false;
    hissBands[0].clear();
    hissBands[1].clear();
    hissLevel = 0.0f;
    followLeft = followLost = 0;
    following = false;
    followHz = 0.0f;
    pickHz = pickAt = 0.0f;
    keyCount = 0;
    lastVelocity = 0.8f;
    nextStep = -1;
    stepCount = 0;
    now = 0;
    dueCount = 0;
    lastKeyPluck = -1000000;
    strikeCount = 0;
    sampleNow = 0;
    moveMouth(true);
    mouthGain = mouthGainTarget;
}

int Tongue::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Tongue::Voice *Tongue::voiceFor(uint8_t note) {
    const int cap = std::clamp(steppedTargetOf(Voices), 1, kVoices);
    // One harp a note; at one voice, one harp replucked.
    for (int i = 0; i < cap; ++i) {
        if (voices[i].used && voices[i].note == note) return &voices[i];
    }
    for (int i = 0; i < cap; ++i) {
        if (!voices[i].used) return &voices[i];
    }
    Voice *oldest = &voices[0];
    for (int i = 0; i < cap; ++i) {
        if (voices[i].age < oldest->age) oldest = &voices[i];
    }
    return oldest;
}

void Tongue::retune(Voice &v) {
    const KindVoice &k = kKindVoices[v.kind];
    // Let go with stop up, a finger on the reed shortens its ring.
    const float ring = targetOf(Ring) * k.ring;
    const float stopped = v.held ? ring : ring + (0.04f - ring) * std::sqrt(clampf(targetOf(Stop), 0.0f, 1.0f));
    for (int i = 0; i < v.count; ++i) {
        Tine &t = v.tines[i];
        const float hz = noteHz(v.baseNote + t.offset + bend * paramOf(BendRange) + v.noteBend);
        t.reed.setRatios(k.ratio2, k.ratio3);
        t.reed.tune(hz, stopped, targetOf(Overtones), k.overRing);
        t.perRadian = sampleRate / (6.28318530718f * hz);
        t.retuned = true;
    }
}

void Tongue::pluckTine(const Voice &v, Tine &t, float swing, float contact, float snap, float gain) {
    const KindVoice &k = kKindVoices[v.kind];
    if (k.pullMs > 0.0f) {
        t.reed.pull(swing, contact / 1.3e-3f * k.pullMs * 0.001f, snap);
    } else {
        t.reed.pluck(swing, contact, snap);
    }
    // The sound grows as the swing cubed; the level is the house law instead.
    // A reed still ringing keeps some of its swing through the catch, so the
    // swing it will have is both together.
    const float after = std::hypot(swing, tongue::Reed::kCaught * t.swing);
    t.gain = gain * kHeldSwing / (after * after * after);
}

void Tongue::noteOn(uint8_t note, uint8_t velocity) {
    const float vel = static_cast<float>(velocity) / 127.0f;
    lastVelocity = vel;
    // A note with words says them; one without goes back to the vowel knob.
    if (pendingCount > 0) {
        planWords();
        pendingCount = 0;
    } else {
        saying = false;
    }
    if (steppedTargetOf(Play) == MouthKeys) {
        // One harp on the drone; the key moves the mouth.
        for (int k = 0; k < keyCount; ++k) {
            if (keys[k] == note) {
                for (int m = k; m + 1 < keyCount; ++m) keys[m] = keys[m + 1];
                --keyCount;
                break;
            }
        }
        if (keyCount == kKeys) {
            for (int m = 0; m + 1 < kKeys; ++m) keys[m] = keys[m + 1];
            --keyCount;
        }
        keys[keyCount++] = note;
        Voice &v = voices[0];
        const bool sounding = v.used && v.held;
        const int when = steppedTargetOf(Repluck);
        const bool pluck = !sounding || when == EveryNote || (when == LoudNotes && velocity >= 100);
        const uint8_t drone = static_cast<uint8_t>(std::clamp(steppedTargetOf(Drone), 0, 127));
        if (pluck) {
            startHarp(v, drone, static_cast<float>(drone), vel);
        } else {
            v.held = true;
        }
        pickHz = harmonicFor(note);
        if (!pickOn) pickAt = pickHz;
        pickOn = true;
        return;
    }
    pickOn = false;
    Voice *v = voiceFor(note);
    startHarp(*v, note, static_cast<float>(note), vel);
}

void Tongue::startHarp(Voice &v, uint8_t note, float noteNumber, float vel) {
    const bool ringing = v.used && v.note == note;
    v.kind = std::clamp(steppedTargetOf(Model), 0, kKinds - 1);
    const KindVoice &k = kKindVoices[v.kind];
    const int reeds = steppedTargetOf(Reeds);
    const int count = std::clamp(reeds > 0 ? reeds : k.reeds, 1, kReeds);
    const int chordKnob = steppedTargetOf(Chord);
    const int chord = std::clamp(chordKnob > 0 ? chordKnob : k.chord, 1, kChords - 1);
    if (!ringing || count != v.count) {
        // A new harp: reeds that weren't sounding start still.
        for (int i = v.count; i < count; ++i) {
            v.tines[i].reed.clear();
            v.tines[i].q = v.tines[i].air = v.tines[i].level = v.tines[i].swing = 0.0f;
        }
        v.next = 0;
    }
    v.count = count;
    for (int i = 0; i < count; ++i) v.tines[i].offset = kChordSteps[chord][i];
    v.note = note;
    v.baseNote = noteNumber + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    v.held = true;
    if (!ringing) {
        v.noteBend = v.slide = 0.0f;
        v.pressure = -1.0f;
    }
    retune(v);
    strike(v, vel);
    lastKeyPluck = now;
    v.used = true;
    v.quietBlocks = 0;
    v.age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Tongue::strike(Voice &v, float vel) {
    strikeLog[strikeCount++ & 63] = sampleNow;
    const KindVoice &k = kKindVoices[v.kind];
    const int count = v.count;
    // Harder is a bigger swing (brighter, as more of it is out of the slot)
    // and a shorter flick of the finger (more of the overtones). The flick
    // is timed for a G3 reed and scales with the reed, so every note has
    // the same colour.
    const float pluck = clampf(k.pluck + targetOf(Pluck) - kPluckHome, 0.0f, 1.0f);
    const float swing = 0.45f + 0.55f * vel;
    // Reeds plucked together share the level; one at a time, each has it all.
    const int order = steppedTargetOf(Order);
    const float share = order == InTurn ? 1.0f : 1.0f / std::sqrt(static_cast<float>(count));
    const float gain = kHouse * k.level * velocityGain(vel, targetOf(VelocityAmount)) * share;
    // A sharp flick snaps off the reed: the ping of its overtones.
    const float snap = (0.25f + 0.75f * pluck) * (0.6f + 0.4f * vel);
    const auto contactFor = [&](const Tine &t) {
        return (3.0f - 2.7f * pluck) * (1.15f - 0.3f * vel) * 0.001f * (196.0f / t.reed.hz());
    };
    // A strum: each reed a step later, in the order asked for. In turn plucks
    // one reed a note, the next each time.
    const int step = static_cast<int>(targetOf(Strum) * 0.001f * sampleRate);
    int scatter[kReeds] = {0, 1, 2, 3, 4};
    if (order == Scatter) {
        for (int i = count - 1; i > 0; --i) std::swap(scatter[i], scatter[nextRandom(noise) % static_cast<uint32_t>(i + 1)]);
    }
    for (int i = 0; i < count; ++i) {
        Tine &t = v.tines[i];
        int place = i;
        if (order == Down) place = count - 1 - i;
        else if (order == Scatter) place = scatter[i];
        else if (order == InTurn) place = i == v.next % count ? 0 : -1;
        if (place < 0) continue;
        const float c = contactFor(t);
        if (place * step == 0) {
            pluckTine(v, t, swing, c, snap, gain);
            t.wait = -1;
        } else {
            t.wait = place * step;
            t.waitSwing = swing;
            t.waitContact = c;
            t.waitSnap = snap;
            t.waitGain = gain;
        }
    }
    if (order == InTurn) v.next = (v.next + 1) % count;
    // The frame rings too, once a pluck.
    v.knock.set(k.frameHz, k.frameRing, sampleRate);
    v.knock.strike(k.frameLevel * kHouse * k.level * velocityGain(vel, targetOf(VelocityAmount)) * swing);
}

void Tongue::noteOff(uint8_t note) {
    // Mouth mode: the mouth goes back to the key still down; the harp is let
    // go with the last.
    for (int k = 0; k < keyCount; ++k) {
        if (keys[k] == note) {
            for (int m = k; m + 1 < keyCount; ++m) keys[m] = keys[m + 1];
            --keyCount;
            if (keyCount > 0) {
                pickHz = harmonicFor(keys[keyCount - 1]);
            } else if (voices[0].used && voices[0].held) {
                voices[0].held = false;
                retune(voices[0]);
                endWords();
            }
            return;
        }
    }
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.held = false;
            retune(v);
            if (v.age == clock) endWords();
        }
    }
}

void Tongue::allNotesOff() {
    keyCount = 0;
    dueCount = 0;
    for (Voice &v : voices) {
        if (v.used && v.held) {
            v.held = false;
            retune(v);
        }
    }
}

void Tongue::pitchBend(int16_t value14) {
    bend = static_cast<float>(value14) / 8192.0f;
    for (Voice &v : voices) {
        if (v.used) retune(v);
    }
}

float Tongue::droneHz() const {
    float hz = 0.0f;
    for (const Voice &v : voices) {
        if (!v.used) continue;
        for (int r = 0; r < v.count; ++r) {
            const float f = v.tines[r].reed.hz();
            if (hz == 0.0f || f < hz) hz = f;
        }
    }
    if (hz > 0.0f) return hz;
    return noteHz(static_cast<float>(steppedTargetOf(Drone)) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f);
}

float Tongue::harmonicNear(float hz, bool fold) const {
    const float drone = std::fmax(droneHz(), 1.0f);
    float ratio = hz / drone;
    // Another track's note is wanted for its name, not its octave: brought
    // into the harmonics a mouth picks out best.
    if (fold) {
        while (ratio < 2.5f) ratio *= 2.0f;
        while (ratio > 12.5f) ratio *= 0.5f;
    }
    // The nearest harmonic in pitch, from the 2nd up, below the formants' reach.
    const float lower = std::floor(ratio), upper = lower + 1.0f;
    float n = lower >= 1.0f && std::log(ratio / lower) < std::log(upper / ratio) ? lower : upper;
    const float top = std::floor(std::fmin(5000.0f, 0.45f * sampleRate) / drone);
    n = std::clamp(n, 2.0f, std::fmax(2.0f, top));
    return n * drone;
}

void Tongue::letGoOfFollow() {
    if (keyCount > 0) {
        pickHz = harmonicFor(keys[keyCount - 1]);
    } else if (steppedTargetOf(Play) != MouthKeys) {
        pickOn = false;
    }
}

float Tongue::harmonicFor(uint8_t note) const { return harmonicNear(noteHz(static_cast<float>(note)), false); }

void Tongue::onBlock(int64_t tickStart, int64_t, float bpm) {
    samplesPerTick = static_cast<double>(sampleRate) * 60.0 / (static_cast<double>(bpm > 1.0f ? bpm : 120.0f) * kPPQN);
    // Kept to the sample between blocks; a jump (a locate, a loop) starts again.
    if (std::fabs(ticks - static_cast<double>(tickStart)) > 2.0) {
        ticks = static_cast<double>(tickStart);
        nextStep = -1;
    }
}

void Tongue::addDue(int64_t at, float velocity) {
    if (dueCount == kDue) return;
    int i = dueCount++;
    while (i > 0 && due[i - 1].at > at) {
        due[i] = due[i - 1];
        --i;
    }
    due[i] = {at, velocity};
}

void Tongue::schedulePattern(int32_t frames) {
    const int which = std::clamp(steppedTargetOf(Pattern), 0, kPatterns - 1);
    bool held = false;
    for (const Voice &v : voices) held = held || (v.used && v.held);
    const double end = ticks + static_cast<double>(frames) / samplesPerTick;
    if (which == NoPattern || !held) {
        nextStep = -1;
        return;
    }
    const Rhythm &r = kRhythms[which];
    // On the song's grid, so a pattern lines up with the beat.
    if (nextStep < 0) nextStep = static_cast<int64_t>(std::ceil(ticks / r.step)) * r.step;
    const float accent = clampf(paramOf(Accent), 0.0f, 1.0f);
    const float ratchet = clampf(paramOf(Ratchet), 0.0f, 1.0f);
    const double stepSamples = static_cast<double>(r.step) * samplesPerTick;
    while (static_cast<double>(nextStep) < end) {
        const float weight = r.weight[(nextStep / r.step) % r.length];
        const int64_t at = now + static_cast<int64_t>(std::llround((static_cast<double>(nextStep) - ticks) * samplesPerTick));
        nextStep += r.step;
        // A step too close after a key's own pluck is the key's.
        if (weight <= 0.0f || static_cast<double>(at - lastKeyPluck) < stepSamples / 3.0) continue;
        const float vel = lastVelocity * (1.0f - accent * (1.0f - weight));
        // Now and then a step is two or three quick plucks; more on the weak steps.
        const float chance = ratchet * (weight < 1.0f ? 1.0f : 0.5f);
        const bool split = chance > 0.0f && static_cast<float>(nextRandom(noise) >> 8) * (1.0f / 16777216.0f) < chance;
        const int parts = split ? 2 + static_cast<int>(nextRandom(noise) % 2u) : 1;
        for (int p = 0; p < parts; ++p) {
            addDue(at + static_cast<int64_t>(stepSamples * p / parts), vel * (p == 0 ? 1.0f : 0.8f));
        }
    }
}

void Tongue::repluckHeld(float velocity) {
    if (keyCount > 0) {
        if (voices[0].used && voices[0].held) strike(voices[0], velocity);
        return;
    }
    for (Voice &v : voices) {
        if (v.used && v.held) strike(v, velocity);
    }
}

void Tongue::lyric(const uint8_t *phones, int32_t count) {
    pendingCount = std::clamp(count, 0, kMaxPhones);
    for (int i = 0; i < pendingCount; ++i) pending[i] = phones[i];
}

void Tongue::planWords() {
    int32_t tableSize = 0;
    const diction::Phone *table = diction::phoneTable(tableSize);
    // The held sound: the last vowel, or failing one the last sound.
    int held = -1;
    for (int i = 0; i < pendingCount; ++i) {
        const int c = pending[i];
        if (c <= 0 || c >= tableSize) continue;
        const diction::Kind kind = table[c].kind;
        if (kind == diction::Kind::Vowel || kind == diction::Kind::Diphthong || held < 0) held = i;
    }
    saidCount = 0;
    heldStep = 0;
    const auto add = [&](const diction::Phone &ph, const float *f, float seconds) {
        if (saidCount == kSaid) return;
        Said &s = said[saidCount++];
        for (int k = 0; k < 3; ++k) s.f[k] = harpFormant(k, f[k]);
        s.noise = ph.noise;
        for (int b = 0; b < 2; ++b) {
            for (int k = 0; k < 3; ++k) s.band[b][k] = ph.band[b][k];
        }
        s.samples = seconds < 0.0f ? -1 : std::max(1, static_cast<int32_t>(seconds * sampleRate));
    };
    for (int i = 0; i < pendingCount; ++i) {
        const int c = pending[i];
        if (c <= 0 || c >= tableSize) continue;
        const diction::Phone &ph = table[c];
        const bool last = i == held;
        if (last) heldStep = ph.kind == diction::Kind::Diphthong ? saidCount + 1 : saidCount;
        if (ph.kind == diction::Kind::Diphthong) {
            add(ph, ph.f, last ? 0.12f : ph.length * 0.5f);
            add(ph, ph.to, last ? -1.0f : ph.length * 0.5f);
        } else {
            // Most consonants are quicker on a harp than in a song: the mouth
            // only passes through them. A hiss keeps its length, to be heard.
            const bool vowel = ph.kind == diction::Kind::Vowel;
            const bool hissing = ph.kind == diction::Kind::Fricative || ph.kind == diction::Kind::Affricate;
            add(ph, ph.f, last ? -1.0f : std::fmax(0.015f, ph.length * (vowel ? 0.8f : hissing ? 1.0f : 0.6f)));
        }
    }
    // The sounds after the held one are the note's end, said when it's let go;
    // the mouth stays on the last.
    if (saidCount > 0) said[saidCount - 1].samples = -1;
    heldStep = std::min(heldStep, saidCount - 1);
    saidAt = 0;
    saidLeft = saidCount > 0 ? said[0].samples : 0;
    saying = saidCount > 0;
    if (saying) startSaid();
}

void Tongue::endWords() {
    if (!saying || saidAt != heldStep || heldStep + 1 >= saidCount) return;
    ++saidAt;
    startSaid();
    saidLeft = said[saidAt].samples;
}

void Tongue::startSaid() {
    const Said &s = said[saidAt];
    for (int b = 0; b < 2; ++b) {
        hissBands[b].set(s.band[b][0], s.band[b][1], sampleRate);
        hissShare[b] = s.band[b][2];
    }
    hissLevel = s.noise;
}

void Tongue::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Tongue::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }

void Tongue::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) {
            v.noteBend = semitones;
            retune(v);
        }
    }
}

void Tongue::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

void Tongue::noteTimbre(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.slide = static_cast<float>(value) / 127.0f;
    }
}

void Tongue::moveMouth(bool jump) {
    // A jump (a reset) takes where the knobs are going, not where they are.
    const auto knob = [&](int32_t p) { return jump ? targetOf(p) : paramOf(p); };
    // The vowel: oo oh ah eh ee along the control, the mod wheel on top.
    const float m = clampf(knob(Mouth) + wheel, 0.0f, 1.0f) * 4.0f;
    const int lo = std::min(3, static_cast<int>(m));
    const float t = m - static_cast<float>(lo);
    float target[3];
    for (int k = 0; k < 3; ++k) target[k] = kMouths[lo][k] + (kMouths[lo + 1][k] - kMouths[lo][k]) * t;
    // A note's words take the mouth where they say, as far as the words knob goes.
    const float words = clampf(knob(Words), 0.0f, 1.0f);
    if (saying && words > 0.0f) {
        for (int k = 0; k < 3; ++k) target[k] += (said[saidAt].f[k] - target[k]) * words;
    }
    for (int k = 0; k < 3; ++k) mouthGoal[k] = target[k];
    const float seconds = saying && words > 0.0f ? kSayGlide : knob(Glide) * 0.001f;
    const float follow = jump ? 1.0f : 1.0f - std::exp(-static_cast<float>(kControl) / (seconds * sampleRate));
    for (int k = 0; k < 3; ++k) formants[k] += (target[k] - formants[k]) * follow;
    if (pickOn) pickAt += (pickHz - pickAt) * follow;
    // Focus narrows the lower formants until each brings out one harmonic.
    const float focus = clampf(knob(Focus), 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) {
        const float bw = diction::widthOf(k, formants[k]);
        const float narrow = k < 2 ? std::fmax(12.0f, bw * (1.0f - 0.9f * focus)) : bw * (1.0f - 0.5f * focus);
        mouth[k].set(formants[k], narrow, sampleRate);
        mouthAir[k].set(formants[k], 2.0f * bw, sampleRate);
    }
    // The mouth gives back the power it's given: what its peaks add is
    // worked out from their response at each harmonic of the lowest reed,
    // weighted as the drone measures (the 1st to 3rd weaker, level from the
    // 4th to the 16th, falling above), and taken out again.
    float hz = 0.0f;
    for (const Voice &v : voices) {
        if (!v.used) continue;
        for (int r = 0; r < v.count; ++r) {
            const float f = v.tines[r].reed.hz();
            if (hz == 0.0f || f < hz) hz = f;
        }
    }
    if (hz <= 0.0f) hz = 196.0f;
    const float lift = kMouthBoost * clampf(knob(Depth), 0.0f, 1.0f);
    // Mouth mode's pick: narrow enough to stand on one harmonic of the drone.
    if (pickOn) pick.set(pickAt, std::fmax(8.0f, hz * (0.5f - 0.4f * focus)), sampleRate);
    // Only worked out again when the mouth, its depth or the pitch has moved.
    const float state[8] = {formants[0], formants[1], formants[2], focus, lift, hz, pickOn ? pickAt : 0.0f, pickOn ? 1.0f : 0.0f};
    bool moved = jump;
    for (int k = 0; k < 8; ++k) moved = moved || std::fabs(state[k] - lastMouth[k]) > 1e-3f * std::fabs(state[k]) + 1e-6f;
    if (!moved) return;
    for (int k = 0; k < 8; ++k) lastMouth[k] = state[k];
    // At most 24 of the first 48 harmonics, spread evenly.
    const int top = std::min(48, static_cast<int>(0.45f * sampleRate / hz));
    const int stride = (top + 23) / 24;
    double sum = 0.0, weights = 0.0;
    for (int h = 1; h <= top; h += stride) {
        const double w = 6.283185307179586 * h * hz / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        std::complex<double> response = 1.0;
        for (int k = 0; k < 3; ++k) {
            const diction::Bandpass &f = mouth[k];
            response += static_cast<double>(lift * kFormantShare[k]) * (static_cast<double>(f.b0) + static_cast<double>(f.b2) * z2) /
                        (1.0 + static_cast<double>(f.a1) * z1 + static_cast<double>(f.a2) * z2);
        }
        if (pickOn) {
            response += static_cast<double>(lift * kPickShare) * (static_cast<double>(pick.b0) + static_cast<double>(pick.b2) * z2) /
                        (1.0 + static_cast<double>(pick.a1) * z1 + static_cast<double>(pick.a2) * z2);
        }
        const double low = (h / 3.5) * (h / 3.5), high = (h / 16.0) * (h / 16.0);
        const double weight = low / (1.0 + low) / (1.0 + high);
        sum += std::norm(response) * weight;
        weights += weight;
    }
    mouthGainTarget = weights > 0.0 ? static_cast<float>(1.0 / std::sqrt(std::fmax(sum / weights, 1e-6))) : 1.0f;
}

bool Tongue::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    const double blockTicks = static_cast<double>(frames) / samplesPerTick;
    if (asleep) {
        ticks += blockTicks;
        now += frames;
        sampleNow = now;
        return true;
    }
    schedulePattern(frames);
    // The words move on a step at a time; the held one stays.
    if (saying && saidLeft >= 0) {
        saidLeft -= frames;
        while (saying && saidLeft < 0 && saidAt + 1 < saidCount) {
            const int32_t over = -saidLeft;
            ++saidAt;
            startSaid();
            saidLeft = said[saidAt].samples < 0 ? -1 : said[saidAt].samples - over;
            if (said[saidAt].samples < 0) break;
        }
    }
    const float wordsHiss = saying ? kSayHiss * hissLevel * clampf(paramOf(Words), 0.0f, 1.0f) * kHouse * lastVelocity : 0.0f;
    // Following another track: a look at its pitch spread over four blocks;
    // sure, the mouth goes to it, and a while unsure, back to the keys.
    if (sidechainRack() >= 0 && key_ != nullptr) {
        follower.push(key_, frames);
        follower.update(0.25f);
        if (--followLeft <= 0) {
            followLeft = 4;
            if (follower.sureness() > 0.8f) {
                followHz = follower.pitch();
                following = true;
                followLost = 0;
            } else if (following && ++followLost > 8) {
                following = false;
                letGoOfFollow();
            }
        }
        if (following) {
            pickHz = harmonicNear(followHz, true);
            if (!pickOn) pickAt = pickHz;
            pickOn = true;
        }
    } else if (following) {
        following = false;
        letGoOfFollow();
    }

    // The block's settings.
    const float edgeKnob = paramOf(Edge) - kEdgeHome;
    const float setKnob = paramOf(Set) - kSetHome;
    const float depth = clampf(paramOf(Depth), 0.0f, 1.0f);
    const float breath = clampf(paramOf(Breath) + pressure, 0.0f, 1.0f);
    const float airNoise = clampf(paramOf(Air), 0.0f, 1.0f);
    const float sustain = clampf(paramOf(Sustain), 0.0f, 1.0f);
    const float volume = paramOf(Volume);
    const float ringKnob = paramOf(Ring);
    // The kind's low cut, from the model knob: one for the whole machine.
    const float lowCut = kKindVoices[std::clamp(steppedTargetOf(Model), 0, kKinds - 1)].lowCut;
    const float cutPole = lowCut > 0.0f ? std::exp(-6.28318530718f * lowCut / sampleRate) : 1.0f;
    float offCentre[kVoices] = {};
    for (int n = 0; n < kVoices; ++n) {
        Voice &v = voices[n];
        if (!v.used) continue;
        const KindVoice &k = kKindVoices[v.kind];
        // A finger's slide fits that harp's reed closer in its frame, as the fit knob does.
        const float halfWidth = 0.25f - 0.23f * clampf(k.edge + edgeKnob + 0.5f * v.slide, 0.0f, 1.0f);
        // A finger's pressure is that harp's own breath; otherwise the channel's.
        const float voiceBreath = v.pressure >= 0.0f ? clampf(paramOf(Breath) + v.pressure, 0.0f, 1.0f) : breath;
        // The reed rests off the slot's middle by up to five eighths of a
        // full swing: the pulses come unevenly and the even harmonics up with
        // them. Never more than kReach of the swing it has, so a dying note
        // still passes through the slot.
        offCentre[n] = clampf(k.set + setKnob, -1.0f, 1.0f) * 0.625f;
        const float ring = ringKnob * k.ring;
        for (int i = 0; i < v.count; ++i) {
            Tine &t = v.tines[i];
            // While the key is held, breath feeds the reed until it swings at
            // kHeldSwing: more than its loss below that, less above.
            const float lack = 1.0f - (t.level * t.level) / (kHeldSwing * kHeldSwing);
            // The reed takes at least a sample and a half to cross the slot at a
            // held swing, or the pulses fall between samples: high notes get a
            // looser fit.
            t.width = std::fmax(halfWidth, 0.9f * 6.28318530718f * t.reed.hz() / sampleRate);
            // A pulse's power goes with the slot's width: scaled back to the
            // default fit's (edge 0.55) so the fit changes the colour, not the level.
            t.fit = std::sqrt(t.width * kDefaultWidth);
            t.reed.setDrive(v.held ? sustain * voiceBreath * (6.907755f / ring + 6.0f) * lack : 0.0f);
        }
    }
    // One breath for the player: the hardest any held harp is blown.
    float breathHeard = breath;
    for (const Voice &v : voices) {
        if (v.used && v.held && v.pressure >= 0.0f) breathHeard = std::fmax(breathHeard, clampf(paramOf(Breath) + v.pressure, 0.0f, 1.0f));
    }
    const float breathStep = (breathHeard - blown) / static_cast<float>(frames);
    const float dcPole = 1.0f - 6.28318530718f * 20.0f / sampleRate;
    const float gainFollow = 1.0f - std::exp(-1.0f / (0.005f * sampleRate));

    bool any = false;
    for (int32_t i = 0; i < frames; ++i) {
        if (--controlLeft <= 0) {
            controlLeft = kControl;
            moveMouth(false);
        }
        mouthGain += (mouthGainTarget - mouthGain) * gainFollow;
        sampleNow = now + i;
        while (dueCount > 0 && due[0].at <= now + i) {
            const float velocity = due[0].velocity;
            for (int d = 1; d < dueCount; ++d) due[d - 1] = due[d];
            --dueCount;
            repluckHeld(velocity);
        }
        float push = 0.0f, air = 0.0f, hiss = 0.0f;
        // Breath moves across the block: the air is a change, so a step would click.
        blown += breathStep;
        float knocks = 0.0f;
        for (int n = 0; n < kVoices; ++n) {
            Voice &v = voices[n];
            if (!v.used) continue;
            const float airScale = kKindVoices[v.kind].air;
            knocks += v.knock.step();
            for (int r = 0; r < v.count; ++r) {
                Tine &t = v.tines[r];
                if (t.wait >= 0 && t.wait-- == 0) pluckTine(v, t, t.waitSwing, t.waitContact, t.waitSnap, t.waitGain);
                const float x = t.reed.step(0.0f);
                // How far the reed swings, from where it is and how fast it's going.
                const float vel = t.reed.velocity();
                t.swing += (std::sqrt(x * x + vel * vel) - t.swing) * 0.02f;
                const float reach = kReach * t.swing;
                const float u = (x - clampf(offCentre[n], -reach, reach)) / t.width;
                const float u2 = u * u;
                // In the slot (|u| < 1) the reed pushes the air; out of it, it doesn't.
                const float inSlot = 1.0f / (1.0f + u2 * u2);
                // Pitch and slot width are taken out. A wider swing pushes more air
                // through the slot (measured: a dying pluck's fundamental falls with
                // the swing), so it grows as the swing cubed.
                const float q = vel * inSlot * t.fit * t.swing / kHeldSwing;
                // Breath through the opening the reed leaves.
                const float open = blown * (kGap + 1.0f - inSlot);
                // A new pitch changes the scale of both: they start again from here.
                if (t.retuned) {
                    t.q = q;
                    t.air = open;
                    t.retuned = false;
                }
                push += (q - t.q) * t.gain * t.perRadian;
                t.q = q;
                air += (open - t.air) * kAirLevel * kHouse * t.perRadian * t.width;
                hiss += white(noise) * open * airNoise * airScale * kHouse;
                t.air = open;
                t.level += (std::fabs(x) - t.level) * 0.0005f;
            }
        }
        // The kind's low cut, on the harp's own sound.
        float dry = push + air + knocks;
        if (lowCut > 0.0f) {
            const float cut = cutPole * (cutOut + dry - cutIn);
            cutIn = dry;
            cutOut = cut;
            dry = cut;
        } else {
            cutIn = cutOut = 0.0f;
        }
        // The mouth's peaks are added to the sound, so they can only lift a
        // harmonic: a formant's centre is in phase with what passes through.
        float peaks = 0.0f, breathed = 0.0f;
        for (int k = 0; k < 3; ++k) {
            peaks += kFormantShare[k] * mouth[k].process(dry);
            breathed += kFormantShare[k] * mouthAir[k].process(hiss);
        }
        if (pickOn) peaks += kPickShare * pick.process(dry);
        const float shaped = dry + kMouthBoost * depth * peaks;
        float wet = mouthGain * shaped + breathed;
        // A word's hissing sounds, in their own colours.
        if (wordsHiss > 0.0f) {
            const float n = white(noise) * wordsHiss;
            wet += hissShare[0] * hissBands[0].process(n) + hissShare[1] * hissBands[1].process(n);
        }
        // No DC out of a mouth.
        const float y = wet - dcIn + dcPole * dcOut;
        dcIn = wet;
        dcOut = y;
        L[i] = R[i] = y * volume;
        any = any || std::fabs(y) > kSilent;
    }

    for (Voice &v : voices) {
        if (!v.used) continue;
        bool quiet = true;
        for (int r = 0; r < v.count; ++r) {
            const Tine &t = v.tines[r];
            quiet = quiet && t.level < kSilent && !t.reed.plucking() && t.wait < 0;
        }
        if (quiet) {
            if (++v.quietBlocks > 8) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
    }
    ticks += blockTicks;
    now += frames;
    sampleNow = now;
    quietSamples = any || activeVoices() > 0 ? 0 : quietSamples + frames;
    if (quietSamples > 8192) asleep = true;
    return true;
}

} // namespace acidulous::machine
