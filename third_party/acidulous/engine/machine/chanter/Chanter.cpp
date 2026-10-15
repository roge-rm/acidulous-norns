#include "Chanter.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level at full, set so the bank sits with the other machines. */
constexpr float kPipeHouse = 1.6f, kStringHouse = 0.2f;
constexpr float kSilent = 2e-5f;
/** How hard the bag blows the chanter and the drones. */
constexpr float kChanterPush = 0.95f, kDronePush = 0.85f;
/** How quickly the bag fills and empties, seconds. */
constexpr float kFills = 0.03f, kEmpties = 0.3f;
/** A closed chanter's gap between notes, seconds. */
constexpr float kShut = 0.012f;
/** A coup: how much it pushes the wheel, and how quickly that passes, seconds. */
constexpr float kCoupPush = 0.8f, kCoupFalls = 0.06f;
/** How long the strings ring once the wheel stops bowing them, seconds. */
constexpr float kStringT60 = 2.5f, kStoppedT60 = 0.15f;
/** The nudge that starts a string on the wheel. */
constexpr float kKick = 0.6f;
/** How hard the wheel pushes the strings. */
constexpr float kBowGain = 0.12f;
/** The dog's buzz at full, and the most it can ring. */
constexpr float kDogGain = 150.0f, kDogMost = 1.0f;
/** The pipes speak a little flat of the note they're tuned to; this much makes it up, cents. */
constexpr float kPipeTrim = 10.0f;
/** Drift: how far the bag's pressure wanders, and the pitch with it, cents. */
constexpr float kDriftPush = 0.06f, kDriftCents = 6.0f;

/**
 * Each kind: its drones, as semitones from the key note (A3 for A), their
 * level, the chanter's bore and reed, where its holes cut off the top, and
 * whether it's closed between notes.
 */
struct Make {
    int drones;
    float droneAt[Chanter::kDrones];
    float droneCents[Chanter::kDrones];
    float droneLevel;
    bool cylinder;
    float lattice;
    float reed;
    bool closed;
    float level;
    /** How loud a second chanter plays the key held before the last, against the first; 0 for none. */
    float second = 0.0f;
};
constexpr Make kMakes[Chanter::KindCount] = {
    // Highland pipes: two tenor drones on the key and a bass an octave down, a loud, bright chanter.
    {3, {0.0f, 0.0f, -12.0f}, {0.0f, 3.0f, -2.0f}, 0.5f, false, 3000.0f, 0.7f, false, 1.0f},
    // Smallpipes: a quiet, closed chanter, drones on the key, the fifth and the octave below.
    {3, {0.0f, 7.0f, -12.0f}, {0.0f, 2.0f, -2.0f}, 0.4f, true, 2200.0f, 0.5f, true, 1.0f},
    // Gaita: one bass drone and a conical chanter, sweeter.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.55f, false, 2200.0f, 0.55f, false, 1.0f},
    // Hurdy-gurdy: bourdon an octave down and mouche a fourth below the key.
    {2, {-12.0f, -5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, false, 0.0f, 0.0f, false, 1.0f},
    // Uilleann pipes: blown from bellows, a sweet, quiet conical chanter
    // played closed, drones on the key and one and two octaves below. A key
    // held under the melody sounds on the regulators, quieter.
    {3, {0.0f, -12.0f, -24.0f}, {0.0f, 2.0f, -2.0f}, 0.35f, false, 2600.0f, 0.45f, true, 0.8f, 0.45f},
    // Gaida: a goatskin bag, one deep drone two octaves down, a bright,
    // reedy chanter, open between notes for the fast ornaments.
    {1, {-24.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, true, 3500.0f, 0.75f, false, 0.9f},
    // Cornemuse: the French bagpipe, a big and a small drone, a conical
    // chanter played half closed.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 3.0f, 0.0f}, 0.45f, false, 2400.0f, 0.6f, true, 0.9f},
    // Musette de cour: small and courtly, a soft, narrow chanter, its
    // drones quiet in a short barrel.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, 0.3f, true, 1800.0f, 0.4f, true, 0.7f},
    // Säckpipa: the Swedish pipe, one drone an octave down, a narrow
    // cylinder played closed.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.45f, true, 2000.0f, 0.5f, true, 0.8f},
    // Dudy: the Czech pipe, one long drone two octaves down, loud, and a
    // cylindrical chanter.
    {1, {-24.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.6f, true, 1800.0f, 0.6f, false, 0.9f},
    // Zampogna: two conical chanters, one for each hand, and drones on the key
    // and the octave below.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, 0.4f, false, 2600.0f, 0.65f, false, 0.8f, 0.8f},
    // Tulum: no drone, two cylindrical chanters side by side.
    {0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, true, 3000.0f, 0.7f, false, 0.9f, 0.9f},
    // Launeddas: three cane pipes with single reeds, blown without a bag by
    // breathing in through the nose: a drone, and two melody pipes.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, true, 2800.0f, 0.6f, false, 0.9f, 0.8f},
};

constexpr float kCoupsPerBeat[4] = {0.0f, 1.0f, 2.0f, 4.0f};

} // namespace

Chanter::Chanter() { initParams(); }

const ParamDef *Chanter::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""}, // highland, smallpipes, gaita, hurdy-gurdy, uilleann, gaida, cornemuse, musette, säckpipa, dudy, zampogna, tulum, launeddas
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // The key the drones are tuned to, C to B.
        {"key", 0.0f, 11.0f, 9.0f, Curve::Stepped, 12, ""},
        {"drones", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // How long the drones go on after the last key's let go, seconds.
        {"bag", 0.0f, 8.0f, 1.5f, Curve::Linear, 0, "s"},
        {"reed", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // A quick high note before each one; 0 for none.
        {"grace", 0.0f, 60.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"drift", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"air", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        // How fast the hurdy-gurdy's wheel turns.
        {"wheel", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"rosin", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"dog", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How fast the wheel must turn before the dog buzzes.
        {"threshold", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"coup", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // off, 1, 2 or 4 a beat
        {"velocity", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Chanter::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    chanter.prepare(sampleRate);
    second.prepare(sampleRate);
    for (timber::Pipe &d : drones) d.prepare(sampleRate);
    for (Bowed &b : melody) b.wave.prepare(sampleRate);
    for (Bowed &b : gurdyDrones) b.wave.prepare(sampleRate);
    trompette.wave.prepare(sampleRate);
    dogTone.setSampleRate(sampleRate);
    dogTone.set(2200.0f, 0.55f);
    box.setSampleRate(sampleRate);
    box.set(280.0f, 0.4f);
    reset();
}

void Chanter::reset() {
    chanter.clear();
    second.clear();
    secondLifted = false;
    for (timber::Pipe &d : drones) d.clear();
    for (Bowed *b : {&melody[0], &melody[1], &gurdyDrones[0], &gurdyDrones[1], &trompette}) {
        b->wave.clear();
        b->dc = 0.0f;
        b->kickLeft = 0;
    }
    heldCount = 0;
    velocity = 0.8f;
    pitch = gracePitch = 60.0f;
    graceLeft = shutLeft = 0;
    sounding = lifted = false;
    kickMelody = kickDrones = false;
    bag = 0.0f;
    sinceRelease = 1e9f;
    driftNow = driftAim = 0.0f;
    driftLeft = 0;
    bpm = 120.0f;
    beatAt = 0.0;
    coupNow = dogNow = 0.0f;
    bend = wheel = channelPressure_ = 0.0f;
    notePress = -1.0f;
    noteBend_ = 0.0f;
    retuneCountdown = 0;
    dogTone.reset();
    dogLast = 0.0f;
    box.reset();
    dcIn = dcOut = 0.0f;
    noise = 0x3b9aca07u;
    quietSamples = 0;
    asleep = true;
}

void Chanter::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

void Chanter::startNote(int note, float vel, bool legato) {
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const float keyNote = 57.0f + static_cast<float>(std::clamp(steppedTargetOf(Key), 0, 11) - 9);
    pitch = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    velocity = vel;
    noteBend_ = 0.0f;
    notePress = -1.0f;
    // There's no tonguing a bagpipe: notes are set apart by a grace note, a
    // quick high G (for A) before each, or a step above for the top notes.
    const float grace = targetOf(Grace);
    if (grace > 0.0f && kind != Gurdy) {
        const float highG = keyNote + 22.0f;
        gracePitch = pitch < highG - 0.5f ? highG : pitch + 2.0f;
        graceLeft = static_cast<int32_t>(grace * 0.001f * sampleRate);
    } else {
        graceLeft = 0;
    }
    // A closed chanter stops between notes.
    if (legato && kMakes[kind].closed) shutLeft = static_cast<int32_t>(kShut * sampleRate);
    if (!legato) lifted = false;
    // The wheel catches a string at once; a nudge starts it, where the
    // rosin alone would take a moment to.
    if (kind == Gurdy) {
        kickMelody = true;
        if (bag < 0.05f) kickDrones = true;
    }
}

void Chanter::noteOn(uint8_t note, uint8_t vel) {
    const bool legato = heldCount > 0;
    for (int i = 0; i < heldCount; ++i) {
        if (held[i] == note) {
            for (int j = i; j + 1 < heldCount; ++j) held[j] = held[j + 1];
            --heldCount;
            break;
        }
    }
    if (heldCount >= kHeld) {
        for (int j = 0; j + 1 < heldCount; ++j) held[j] = held[j + 1];
        --heldCount;
    }
    held[heldCount++] = note;
    startNote(note, static_cast<float>(vel) / 127.0f, legato);
    asleep = false;
    quietSamples = 0;
}

void Chanter::noteOff(uint8_t note) {
    const int before = heldNote();
    for (int i = 0; i < heldCount; ++i) {
        if (held[i] == note) {
            for (int j = i; j + 1 < heldCount; ++j) held[j] = held[j + 1];
            --heldCount;
            break;
        }
    }
    if (heldCount == 0) {
        sinceRelease = 0.0f;
        graceLeft = 0;
    } else if (heldNote() != before) {
        // Back to a key still held, without a grace note.
        pitch = static_cast<float>(heldNote()) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
        graceLeft = 0;
    }
}

void Chanter::allNotesOff() {
    if (heldCount > 0) sinceRelease = 0.0f;
    heldCount = 0;
    graceLeft = 0;
}

void Chanter::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel squeezes the bag, or pushes the wheel.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Chanter::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Chanter::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Chanter::noteBend(uint8_t note, float semitones) {
    if (heldCount > 0 && heldNote() == note) noteBend_ = semitones;
}

void Chanter::notePressure(uint8_t note, uint8_t value) {
    if (heldCount > 0 && heldNote() == note) notePress = static_cast<float>(value) / 127.0f;
}

float Chanter::bow(Bowed &s, float speed, float grip) {
    // Rosin's friction, as Filament's bow: it grips while the wheel and the
    // string move together and lets go when the string slips. The curve
    // falls away both sides, or the wheel would only add energy.
    // A nudge: half a period's push, the way the wheel first catches the string.
    float nudge = 0.0f;
    if (s.kickLeft > 0) {
        nudge = kKick * std::sin(3.14159265f * static_cast<float>(s.kickLeft) / static_cast<float>(s.kickLength));
        --s.kickLeft;
    }
    if (speed <= 0.0f) return s.wave.step(nudge);
    // The wheel's speed is kept just past where the rosin lets go, where a
    // faster wheel drives the string harder rather than skating over it.
    const float width = 0.05f + 0.25f * (1.0f - grip);
    const float relative = width * speed - s.wave.velocity();
    const float force = relative / (width + relative * relative / width);
    // A little grit from the rosin starts the string going.
    float excite = (force * (0.3f + 0.7f * grip) + white() * 0.02f) * kBowGain;
    // The steady part of the push isn't a wave.
    s.dc += (excite - s.dc) * 0.002f;
    excite -= s.dc;
    return s.wave.step(excite + nudge);
}

bool Chanter::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    const float keyNote = 57.0f + static_cast<float>(std::clamp(steppedTargetOf(Key), 0, 11) - 9) + targetOf(Tune) / 100.0f;
    const float dt = static_cast<float>(frames) / sampleRate;
    const float volume = paramOf(Volume);
    const float droneLevel = clampf(paramOf(Drones), 0.0f, 1.0f) * k.droneLevel;
    const float press = std::fmax(notePress >= 0.0f ? notePress : channelPressure_, wheel);
    const float air = clampf(paramOf(Air), 0.0f, 1.0f);

    // The bag fills while a key's held and keeps the drones going after;
    // the wheel likewise turns on.
    const bool playing = heldCount > 0;
    if (!playing) sinceRelease += dt;
    const float aim = playing || sinceRelease < paramOf(Bag) ? 1.0f : 0.0f;
    bag += (aim - bag) * (1.0f - std::exp(-dt / (aim > bag ? kFills : kEmpties)));
    if (bag < 1e-4f && aim == 0.0f) bag = 0.0f;

    // Drift: the bag's pressure wanders as the arm squeezes it.
    driftLeft -= frames;
    if (driftLeft <= 0) {
        driftAim = white();
        driftLeft = static_cast<int32_t>((0.25f + 0.2f * (white() + 1.0f)) * sampleRate);
    }
    driftNow += (driftAim - driftNow) * (1.0f - std::exp(-dt / 0.2f));
    const float drift = clampf(paramOf(Drift), 0.0f, 1.0f) * driftNow;

    const float melodyPitch = pitch + bend * paramOf(BendRange) + noteBend_;
    const float level = velocityGain(velocity, targetOf(VelocityAmount));
    bool any = bag > 0.0f || playing;

    if (kind != Gurdy) {
        // --- bagpipes -----------------------------------------------------
        const float push = (1.0f + kDriftPush * drift + 0.1f * press) * bag;
        const float cents = drift * kDriftCents;
        bool chanterOn = playing && bag > 0.3f;
        if (shutLeft > 0) {
            chanter.setTongue(1.0f);
            shutLeft -= frames;
        } else {
            chanter.setTongue(0.0f);
        }
        const float now = graceLeft > 0 ? gracePitch : melodyPitch;
        if (graceLeft > 0) graceLeft -= frames;
        chanter.setNote(noteHz(now + (cents + kPipeTrim) * 0.01f));
        chanter.setShape(k.cylinder, 1);
        chanter.setTube(noteHz(keyNote + 10.0f));
        chanter.setLattice(k.lattice, 0.0f, 0.4f);
        chanter.setBelow(1.0f);
        chanter.setFork(0.2f);
        chanter.setReed(k.cylinder ? 0 : 1, clampf(k.reed + (paramOf(Reed) - 0.5f) * 0.6f, 0.05f, 1.0f), 0.45f);
        chanter.setBell(0.9f, 0.4f);
        chanter.setLoss(0.999f);
        chanter.setPressure(chanterOn ? kChanterPush * push : 0.0f);
        chanter.setDrive(1.0f);
        chanter.tune();
        if (chanterOn && !lifted) {
            chanter.tune();
            chanter.lift();
            lifted = true;
        }
        if (!chanterOn) lifted = false;
        // The other hand: the key held before the last, on the second chanter.
        const bool secondOn = chanterOn && k.second > 0.0f && heldCount >= 2;
        if (k.second > 0.0f) {
            const float other = static_cast<float>(held[std::max(heldCount - 2, 0)]) +
                                12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
            second.setTongue(0.0f);
            second.setNote(noteHz(other + bend * paramOf(BendRange) + (cents + kPipeTrim) * 0.01f));
            second.setShape(k.cylinder, 1);
            second.setTube(noteHz(keyNote + 10.0f));
            second.setLattice(k.lattice, 0.0f, 0.4f);
            second.setBelow(1.0f);
            second.setFork(0.2f);
            second.setReed(k.cylinder ? 0 : 1, clampf(k.reed + (paramOf(Reed) - 0.5f) * 0.6f, 0.05f, 1.0f), 0.45f);
            second.setBell(0.9f, 0.4f);
            second.setLoss(0.999f);
            second.setPressure(secondOn ? kChanterPush * push : 0.0f);
            second.setDrive(1.0f);
            second.tune();
            if (secondOn && !secondLifted) {
                second.tune();
                second.lift();
                secondLifted = true;
            }
            if (!secondOn) secondLifted = false;
        }
        const float secondPush = secondOn ? kChanterPush * push : 0.0f;
        const float secondGain = k.level * k.second * level;
        for (int d = 0; d < k.drones; ++d) {
            timber::Pipe &p = drones[d];
            const float hz = noteHz(keyNote + k.droneAt[d] + (k.droneCents[d] + cents + kPipeTrim) * 0.01f);
            p.setNote(hz);
            p.setShape(true, 1);
            p.setTube(hz);
            p.setLattice(6000.0f, 0.0f, 0.0f);
            p.setBelow(1.0f);
            p.setFork(0.0f);
            p.setReed(0, 0.5f, 0.55f);
            p.setBell(0.9f, 0.6f);
            p.setLoss(0.999f);
            p.setPressure(kDronePush * push);
            p.setDrive(1.0f);
            const bool speaking = bag > 0.05f;
            p.tune();
            if (speaking && !sounding) {
                p.tune();
                p.lift();
            }
        }
        sounding = bag > 0.05f;
        const float chanterPush = chanterOn ? kChanterPush * push : 0.0f;
        for (int32_t i = 0; i < frames; ++i) {
            float y = chanter.step(chanterPush, white() * air * 0.35f * chanterPush) * k.level * level;
            if (k.second > 0.0f) y += second.step(secondPush, white() * air * 0.35f * secondPush) * secondGain;
            float drone = 0.0f;
            for (int d = 0; d < k.drones; ++d) {
                const float dp = kDronePush * push;
                drone += drones[d].step(dp, white() * air * 0.2f * dp);
            }
            y += drone * droneLevel;
            y *= kPipeHouse * volume;
            const float hp = y - dcIn + (1.0f - 6.2831853f * 15.0f / sampleRate) * dcOut;
            dcIn = y;
            dcOut = hp;
            L[i] = R[i] = hp;
        }
    } else {
        // --- hurdy-gurdy --------------------------------------------------
        // Coups: the wheel pushed on the beat, so the dog buzzes in time.
        const float perBeat = kCoupsPerBeat[std::clamp(steppedTargetOf(Coup), 0, 3)];
        const double before = beatAt;
        beatAt += static_cast<double>(dt * bpm / 60.0f);
        if (perBeat > 0.0f && bag > 0.0f && std::floor(beatAt * perBeat) != std::floor(before * perBeat)) coupNow = kCoupPush;
        if (!playing && bag <= 0.0f) beatAt = 0.0;
        coupNow *= std::exp(-dt / kCoupFalls);
        const float turning = clampf(paramOf(Wheel), 0.0f, 1.0f) * bag;
        const float speed = turning * (1.0f + coupNow + press);
        const float grip = clampf(paramOf(Rosin), 0.0f, 1.0f);
        // The dog lifts once the wheel's fast enough, as a player's wrist
        // jerks it.
        const float dogAim = clampf((speed - paramOf(Threshold)) * 6.0f, 0.0f, 1.0f);
        dogNow += (dogAim - dogNow) * (1.0f - std::exp(-dt / 0.01f));
        const float dog = clampf(paramOf(Dog), 0.0f, 1.0f);
        // The wheel slows to a stop as the player stops cranking, and once
        // it's below where the rosin lets go, the strings stop speaking.
        const float bowSpeed = (1.2f + 2.0f * clampf(paramOf(Wheel), 0.0f, 1.0f) * (1.0f + coupNow + press)) * bag;
        const float tone = 0.55f + 0.3f * grip;
        auto tuneString = [&](Bowed &s, float note, bool bowing) {
            s.hz = noteHz(note + drift * kDriftCents * 0.002f);
            s.wave.setFrequency(s.hz);
            const float t60 = bowing ? kStringT60 : kStoppedT60;
            s.wave.setDamping(std::pow(10.0f, -3.0f / (t60 * s.hz)), tone);
        };
        tuneString(melody[0], melodyPitch, playing);
        tuneString(melody[1], melodyPitch + 0.03f, playing);
        for (int d = 0; d < k.drones; ++d) tuneString(gurdyDrones[d], keyNote + k.droneAt[d], bag > 0.0f);
        tuneString(trompette, keyNote, bag > 0.0f);
        const bool wheelOn = bag > 0.05f;
        auto kick = [&](Bowed &b) {
            b.kickLength = std::max(2, static_cast<int>(0.5f * sampleRate / std::fmax(b.hz, 20.0f)));
            b.kickLeft = b.kickLength;
        };
        if (kickMelody) {
            for (Bowed &m : melody) kick(m);
            kickMelody = false;
        }
        if (kickDrones) {
            for (int d = 0; d < k.drones; ++d) kick(gurdyDrones[d]);
            kick(trompette);
            kickDrones = false;
        }
        for (int32_t i = 0; i < frames; ++i) {
            const float melodySpeed = playing && wheelOn ? bowSpeed + 1e-3f : 0.0f;
            float y = (bow(melody[0], melodySpeed, grip) + bow(melody[1], melodySpeed, grip)) * level;
            const float droneSpeed = wheelOn ? bowSpeed + 1e-3f : 0.0f;
            float drone = 0.0f;
            for (int d = 0; d < k.drones; ++d) drone += bow(gurdyDrones[d], droneSpeed, grip);
            const float t = bow(trompette, droneSpeed, grip);
            // The dog: lifted, it strikes the soundboard on every swing of
            // the trompette, and that rings: the buzz.
            float buzz = 0.0f;
            if (dogNow > 0.0f && dog > 0.0f) {
                const float past = t - 0.01f;
                const float c = past > 0.0f ? past * past / (past + 0.01f) : 0.0f;
                buzz = dog * dogNow * kDogMost * std::tanh(kDogGain / kDogMost * dogTone.step(c - dogLast).bp);
                dogLast = c;
            }
            y += (drone + 0.5f * t) * droneLevel + buzz;
            y += 0.6f * box.step(y).bp;
            y *= kStringHouse * volume;
            const float hp = y - dcIn + (1.0f - 6.2831853f * 15.0f / sampleRate) * dcOut;
            dcIn = y;
            dcOut = hp;
            L[i] = R[i] = hp;
        }
        any = any || melody[0].wave.level() > kSilent || trompette.wave.level() > kSilent;
    }

    float peak = 0.0f;
    for (int32_t i = 0; i < frames; ++i) peak = std::fmax(peak, std::fabs(L[i]));
    if (!any && peak < kSilent) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return false;
}

} // namespace acidulous::machine
