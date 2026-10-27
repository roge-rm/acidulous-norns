#include "Timber.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, measured after
// volume x 1.4 (peak -22.4 dB). The drive is normalised to it. Set it too
// high and the drive knob mostly changes the volume.
constexpr float kNominal = 0.076f;
// The house level. Sets where the bank sits on the volume knob, measured from
// Init at a note in the instrument's range.
constexpr float kHouse = 1.82f;

/** How long a pad takes to close, in seconds. */
constexpr float kKeyClick = 0.004f;
/**
 * How loud a pad is at `keys` 1. Kept low because the notes take 50 to 250 ms
 * to speak, and a louder pad noise clicks on the front of every note.
 */
constexpr float kKeyLevel = 0.1f;

/** How much of the breath a jet's flutter takes away at its deepest. */
constexpr float kJetFlutterBreath = 0.5f;

namespace {
constexpr float kTwoPi = 6.28318530718f;

/**
 * The swell's share of the set pressure, [pos] of the way from [from] to 1.
 * An S curve, so it leaves and arrives without a corner, and exactly 1 at the
 * end.
 */
inline float swellAt(float from, float pos) {
    if (pos >= 1.0f) return 1.0f;
    const float s = pos * pos * (3.0f - 2.0f * pos);
    return from + (1.0f - from) * s;
}
} // namespace

Timber::Timber() { initParams(); }

const ParamDef *Timber::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        // The exciter (single reed, double reed, jet) and the bore shape
        // (cylinder, cone). Together they pick the woodwind family.
        {"family", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""},
        {"bore", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // The instrument's lowest note. The tube below the fingers follows
        // from this and the note being played.
        {"body", 30.0f, 500.0f, 146.8f, Curve::Exponential, 0, "Hz"},
        {"lattice", 300.0f, 6000.0f, 1500.0f, Curve::Exponential, 0, "Hz"},
        {"holes", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"fingering", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"below", 0.2f, 3.0f, 1.0f, Curve::Linear, 0, ""},
        {"answer", 0.0f, 0.55f, 0.2f, Curve::Linear, 0, ""},
        {"register", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""},
        {"reed", 0.05f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"embouchure", 0.25f, 0.9f, 0.55f, Curve::Linear, 0, ""},
        {"pressure", 0.0f, 1.3f, 0.85f, Curve::Linear, 0, ""},
        {"breath", 0.0f, 1.0f, 0.12f, Curve::Linear, 0, ""},
        {"jet", 0.2f, 1.2f, 0.5f, Curve::Linear, 0, ""},
        {"aim", -0.8f, 0.8f, 0.3f, Curve::Linear, 0, ""},
        {"bell", 0.05f, 0.9f, 0.4f, Curve::Linear, 0, ""},
        {"loss", 0.97f, 1.0f, 0.999f, Curve::Linear, 0, ""},
        {"tongue", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"tonguetime", 0.002f, 0.12f, 0.02f, Curve::Exponential, 0, "s"},
        {"flutter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"flutterrate", 8.0f, 45.0f, 25.0f, Curve::Exponential, 0, "Hz"},
        {"keys", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"attack", 0.002f, 2.0f, 0.03f, Curve::Exponential, 0, "s"},
        {"decay", 0.005f, 4.0f, 0.4f, Curve::Exponential, 0, "s"},
        {"sustain", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
        {"release", 0.005f, 4.0f, 0.15f, Curve::Exponential, 0, "s"},
        {"vibrato", 0.0f, 60.0f, 8.0f, Curve::Linear, 0, "cents"},
        {"vibratorate", 0.5f, 12.0f, 5.0f, Curve::Exponential, 0, "Hz"},
        {"vibratodelay", 0.0f, 2.0f, 0.3f, Curve::Linear, 0, "s"},
        {"cutoff", 200.0f, 18000.0f, 18000.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"filtertype", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""},
        {"mono", 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, ""},
        {"glide", 0.0f, 1.0f, 0.04f, Curve::Linear, 0, "s"},
        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},
        {"fine", -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"swell", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"swelltime", 0.01f, 8.0f, 0.5f, Curve::Exponential, 0, "s"},
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
                put(b + 1 + r, n, 0.0f, static_cast<float>(MacroDestCount - 1), 0.0f, Curve::Stepped,
                    MacroDestCount);
                std::snprintf(n, sizeof(n), "mac%d_a%d", k + 1, r + 1);
                put(b + 5 + r, n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0);
            }
        }
        // Legato is off by default, so a patch saved without it slurs only with glide on.
        defs[Legato] = ParamDef{"legato", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""};
        defs[Choke] = ParamDef{"choke", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""};
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Timber::Count];
    DestMap() {
        using T = Timber;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {T::Body, T::McBody}, {T::Lattice, T::McLattice}, {T::Holes, T::McHoles},
            {T::Fingering, T::McFingering}, {T::Below, T::McBelow}, {T::Answer, T::McAnswer},
            {T::Reed, T::McReed}, {T::Embouchure, T::McEmbouchure}, {T::Pressure, T::McPressure},
            {T::Breath, T::McBreath}, {T::Jet, T::McJet}, {T::Aim, T::McAim},
            {T::Bell, T::McBell}, {T::Loss, T::McLoss},
            {T::Tongue, T::McTongue}, {T::TongueTime, T::McTongueTime}, {T::Flutter, T::McFlutter},
            {T::FlutterRate, T::McFlutterRate}, {T::Keys, T::McKeys},
            {T::Attack, T::McAttack}, {T::Decay, T::McDecay}, {T::Sustain, T::McSustain},
            {T::Release, T::McRelease},
            {T::Vibrato, T::McVibrato}, {T::VibratoRate, T::McVibratoRate}, {T::VibratoDelay, T::McVibratoDelay},
            {T::Cutoff, T::McCutoff}, {T::Resonance, T::McResonance}, {T::Glide, T::McGlide},
            {T::VelocityAmount, T::McVelocity},
            {T::Drive, T::McDrive}, {T::Volume, T::McVolume}, {T::Pan, T::McPan}, {T::MpeTimbre, T::McMpeTimbre},
            {T::Swell, T::McSwell}, {T::SwellTime, T::McSwellTime},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Timber::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : mac_[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Timber::updateMacros(bool goal) {
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, goal);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(read(b + 1 + r, goal) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) mac_[dest] += knob * read(b + 5 + r, goal);
        }
    }
    // Two octaves either way.
    mac_[McPitch] = std::clamp(mac_[McPitch], -1.0f, 1.0f);
}

void Timber::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.amp.setSampleRate(sampleRate);
        v.filter.setSampleRate(sampleRate);
        v.pipe.prepare(sampleRate);
    }
    reset();
}

void Timber::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.amp.kill();
        v.filter.reset();
        v.pipe.clear();
        v.tongueLeft = v.keyLeft = v.keyState = v.breathScale = 0.0f;
        v.lift = false;
        v.fadeLeft = 0;
        v.pendingNote = -1;
        v.pendingOff = false;
    }
    for (auto &v : voices) {
        v.swellFrom = v.swellPos = 1.0f;
        v.swellStep = 0.0f;
    }
    for (auto &m : mac_) m = 0.0f;
    flutterPhase = 0.0f;
    rng = kRngSeed; // restart the breath noise
}

void Timber::allNotesOff() {
    for (auto &v : voices) {
        if (!v.used) continue;
        v.gate = false;
        v.amp.release();
    }
}

Timber::Voice *Timber::allocate() {
    for (auto &v : voices) if (!v.used) return &v;
    Voice *best = nullptr;
    for (auto &v : voices) {
        if (v.gate) continue;
        if (best == nullptr || v.age < best->age) best = &v;
    }
    if (best != nullptr) return best;
    for (auto &v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

/** How long a still-sounding voice fades out before it restarts. */
constexpr int32_t kFadeFrames = 96;

void Timber::noteOn(uint8_t note, uint8_t velocity) {
    updateMacros(true); // a note reads where the knobs are going, so renders repeat
    // Mono slurs onto the note that is held, or takes the place of one
    // waiting to start. A note played after the last one let go is tongued
    // on a voice of its own, so the old tail rings out instead of being cut.
    // With choke, it takes the last voice let go instead and cuts its tail.
    Voice *vp = nullptr;
    const bool mono = steppedTargetOf(Mono) != 0;
    if (mono) {
        for (auto &w : voices) {
            if (w.used && (w.gate || w.pendingNote >= 0)) { vp = &w; break; }
        }
        if (vp == nullptr && steppedTargetOf(Choke) != 0) {
            for (auto &w : voices) {
                if (w.used && (vp == nullptr || w.age > vp->age)) vp = &w;
            }
        }
    }
    if (vp == nullptr) vp = allocate();
    if (vp == nullptr) return;
    Voice &v = *vp;

    if (v.pendingNote >= 0) {
        // Already fading out for a waiting note. This one takes its place,
        // and the fade goes on where it is, since starting it again would
        // step the old note back up.
        v.pendingNote = note;
        v.pendingVel = velocity;
        v.pendingOff = false;
        return;
    }

    const float glide = goalOf(Glide);
    // A slurred note arrives without the tongue and without restarting the
    // instrument. A tongued note is stopped and started again.
    // Legato slurs onto a held key at any glide, and choke onto the tail of
    // a released one while glide is on.
    const bool held = v.used && v.gate && v.pendingNote < 0;
    const bool slurred = held ? glide > 0.001f || (mono && steppedTargetOf(Legato) != 0)
                              : mono && v.used && steppedTargetOf(Choke) != 0 && glide > 0.001f &&
                                    v.pendingNote < 0;
    if (!slurred && v.used && v.amp.value() > 0.0001f) {
        // Still sounding (usually its own release, since the patches are
        // mono). Cutting it now would click, so fade it over 2 ms and start
        // the new note in the next block. Nothing else changes yet, not even
        // v.note, because render retunes the pipe from it. noteOff checks
        // pendingNote first.
        v.pendingNote = note;
        v.pendingVel = velocity;
        v.pendingOff = false;
        v.fadeLeft = kFadeFrames;
        return;
    }
    startVoice(v, note, velocity, slurred);
}

void Timber::startVoice(Voice &v, uint8_t note, uint8_t velocity, bool slurred) {
    updateMacros(true);
    const bool wasHeld = v.gate;
    v.glideFrom = slurred ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = slurred ? 0.0f : 1.0f;
    if (!v.used) v.outGain = velocityGain(static_cast<float>(velocity) / 127.0f, goalOf(VelocityAmount));
    v.freq = v.glideFrom;
    v.used = true;
    v.gate = true;
    v.note = note;
    v.bend = 0.0f;
    v.pressure = v.timbre = -1.0f;
    v.velocity = static_cast<float>(velocity) / 127.0f;
    v.age = ++ageCounter;
    v.vibratoPhase = 0.0f;
    v.vibratoLeft = goalOf(VibratoDelay);
    // The key pads closing on the body.
    v.keyLeft = goalOf(Keys) > 0.001f ? kKeyClick * sampleRate : 0.0f;
    v.keyState = 0.0f; // clear the last pad's filter state
    if (!slurred) {
        v.tongueLeft = goalOf(TongueTime) * sampleRate;
        // The swell starts with the tongued note. 1 - swell of the set
        // pressure, so +1 starts from nothing and -1 at twice it.
        const float swell = goalOf(Swell);
        v.swellFrom = 1.0f - swell;
        v.swellPos = swell == 0.0f ? 1.0f : 0.0f;
        v.swellStep = 1.0f / std::max(1.0f, goalOf(SwellTime) * sampleRate);
        // retrigger zeroes the envelope, so the voice is silent and the tube
        // can be cleared without a click. The new note then starts from its
        // own breath instead of the old note's pitch still in the tube.
        v.amp.retrigger();
        v.pipe.clear();
        v.breathScale = 0.0f; // the breath ramps back up behind the tongue
        // Boost the attack so the note speaks quickly despite the moderate
        // loop gain. It needs the note and a solved loop, so it's applied in
        // render.
        v.lift = true;
    } else {
        v.tongueLeft = 0.0f;
        // A choke slurs onto a tail in its release, so the breath comes back.
        if (!wasHeld) v.amp.trigger();
    }
}

void Timber::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (!v.used) continue;
        if (v.pendingNote == note) {
            v.pendingOff = true; // released before it started: start it, then release
            continue;
        }
        if (!v.gate || v.note != note) continue;
        v.gate = false;
        v.amp.release();
    }
}

void Timber::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Timber::channelPressure(uint8_t value) { aftertouch = static_cast<float>(value) / 127.0f; }
void Timber::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }
void Timber::onBlock(int64_t, int64_t, float) {}

bool Timber::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;

    const float dt = 1.0f / sampleRate;
    const int32_t family = std::clamp(steppedOf(Family), 0, 2);
    const bool cylinder = steppedOf(Bore) == 0;
    const float body = paramOf(Body);
    const float lattice = paramOf(Lattice);
    const float holes = paramOf(Holes);
    const float fingering = paramOf(Fingering);
    const float below = paramOf(Below);
    const float answer = paramOf(Answer);
    const int32_t reg = std::clamp(steppedOf(Reg), 0, 2);
    const float reed = paramOf(Reed);
    const float embouchure = paramOf(Embouchure);
    const float mouth = paramOf(Pressure);
    const float breathNoise = paramOf(Breath);
    const float jet = paramOf(Jet), aim = paramOf(Aim);
    const float bell = paramOf(Bell), loss = paramOf(Loss);
    const float tongueDepth = paramOf(Tongue);
    const float flutter = paramOf(Flutter), flutterRate = paramOf(FlutterRate);
    const float keys = paramOf(Keys);
    const float vibrato = paramOf(Vibrato), vibratoRate = paramOf(VibratoRate);
    const float glide = paramOf(Glide);
    const float bendMul = std::pow(2.0f, bend * paramOf(BendRange) / 12.0f);
    const float tuneMul = std::pow(2.0f, (paramOf(Octave_) * 12.0f + paramOf(Transpose) +
                                          paramOf(Fine) * 0.01f + mac_[McPitch] * 24.0f) / 12.0f);
    const float velAmount = paramOf(VelocityAmount);
    const float drive = paramOf(Drive), volume = paramOf(Volume);
    const float panKnob = paramOf(Pan);
    const float panL = std::cos((panKnob + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((panKnob + 1.0f) * 0.25f * 3.14159265f);

    // The register vent opens a small hole to stop the fundamental, so the
    // note sits on a higher partial. A cylinder has no second partial, so its
    // octave register gives a twelfth (the third partial) instead.
    const int32_t mode = reg == Natural ? 1 : (reg == Octave ? (cylinder ? 3 : 2) : 3);

    // The flutter is read every sample from where the block started, since
    // it taps the reed shut or cuts the jet's gain and a per-block value
    // would step it.
    const bool fluttering = flutter > 0.0f;
    const bool jetFlutter = family == timber::Pipe::Jet && fluttering;
    const float flutterFrom = flutterPhase, flutterStep = flutterRate * dt;
    flutterPhase += flutterRate * dt * static_cast<float>(frames);
    while (flutterPhase >= 1.0f) flutterPhase -= 1.0f;

    // How long the tongue takes to leave the reed, and how long a pad takes
    // to close. Both are smooth windows; see the sample loop.
    const float tongueTime = paramOf(TongueTime) * sampleRate;
    const float keyInv = 1.0f / (kKeyClick * sampleRate);
    const float breathRise = 1.0f - std::exp(-1.0f / (0.004f * sampleRate));
    const float breathFall = 1.0f - std::exp(-1.0f / (0.025f * sampleRate));

    lastLattice = lattice * (1.0f - fingering * 0.7f);

    for (auto &v : voices) {
        if (!v.used) continue;
        if (v.pendingNote >= 0 && v.fadeLeft <= 0) {
            // The fade is done. Start the waiting note at the block boundary
            // so the solve below sees it first.
            startVoice(v, static_cast<uint8_t>(v.pendingNote), v.pendingVel, false);
            v.pendingNote = -1;
            if (v.pendingOff) {
                v.pendingOff = false;
                v.gate = false;
                v.amp.release();
            }
            updateMacros(); // startVoice left the targets in
        }
        v.amp.set(0.0f, paramOf(Attack), paramOf(Decay), paramOf(Sustain), paramOf(Release), false);
        v.filter.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType),
                     dsp::MultiFilter::Clean, 0.0f);

        if (v.glidePos < 1.0f) {
            v.glidePos = std::min(1.0f, v.glidePos + (glide > 0.001f ? dt * frames / glide : 1.0f));
            v.freq = v.glideFrom + (noteHz(static_cast<float>(v.note)) - v.glideFrom) * v.glidePos;
        } else {
            v.freq = noteHz(static_cast<float>(v.note));
        }

        const float vibratoDepth = v.vibratoLeft > 0.0f ? 0.0f : vibrato * (0.35f + modWheel * 0.65f);
        if (v.vibratoLeft > 0.0f) v.vibratoLeft -= dt * frames;
        v.vibratoPhase += vibratoRate * dt * frames;
        while (v.vibratoPhase >= 1.0f) v.vibratoPhase -= 1.0f;
        const float vib = std::sin(v.vibratoPhase * kTwoPi) * vibratoDepth;

        // The player's breath pressure. It isn't scaled by the envelope,
        // because the breath is already up behind the tongue before the note
        // sounds, and ramping it would slow the attack. The envelope shapes
        // the output instead, and the breath has its own short ramp below.
        //
        // Velocity blows harder (brighter) and also sets the level with the
        // shared velocity curve, since blowing softer alone can't make a
        // note quiet without it failing to speak.
        const float vel = 1.0f - 0.5f * velAmount * (1.0f - v.velocity);
        // Ramped across the block, since a slurred note changes it mid-sound.
        const float gainTo = velocityGain(v.velocity, velAmount);
        const float gainStep = (gainTo - v.outGain) / static_cast<float>(frames);
        const float at = v.pressure >= 0.0f ? v.pressure : aftertouch;
        const float push = mouth * vel * (1.0f + at * 0.3f);
        // The swell moves the breath as the pressure knob would: the solve
        // sees it once a block and the breath below follows it every sample.
        const bool swelling = v.swellPos < 1.0f;
        const float solvePush = swelling ? push * swellAt(v.swellFrom, v.swellPos) : push;

        const float hz = v.freq * tuneMul * bendMul * noteBendMul(v) * std::pow(2.0f, vib / 1200.0f);
        v.pipe.setNote(hz);
        // The tongue leaves the reed in 3 ms, or under 45 Hz over up to one
        // period of the note. A 3 ms edge on a note that long kicks the
        // cone's flat second mode (about 1.87x), which then takes the note.
        const float lowNote = std::clamp(8.0f * (1.0f - hz / 45.0f), 0.0f, 1.0f);
        const float tongueOff = std::min(std::max(0.003f * sampleRate, lowNote * sampleRate / hz), tongueTime);
        const float tongueOffInv = tongueOff > 1.0f ? 1.0f / tongueOff : 1.0f;
        v.pipe.setShape(cylinder, mode);
        v.pipe.setTube(body);
        v.pipe.setLattice(lattice, fingering, holes);
        v.pipe.setBelow(below);
        v.pipe.setFork(answer);
        // Slide stiffens the reed, which brightens the tone.
        const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
        v.pipe.setReed(family, reed * (1.0f + slide * paramOf(MpeTimbre) * 0.8f), embouchure);
        v.pipe.setJet(jet, aim);
        v.pipe.setBell(0.9f, bell);
        v.pipe.setLoss(loss);
        v.pipe.setPressure(solvePush);
        v.pipe.setDrive(1.0f);
        v.pipe.tune();
        if (v.lift) {
            // After the solve, so the lift is sized against the actual loop
            // gain.
            v.pipe.tune();
            v.pipe.lift();
            v.lift = false;
        }

        if (!jetFlutter) v.pipe.setJetCut(0.0f);

        for (int32_t i = 0; i < frames; ++i) {
            const float env = v.amp.next();
            if (env <= 0.0000005f && !v.gate) { v.used = false; break; }

            // The tongue: on the reed at the start of a note, and again as
            // often as flutter asks for. It comes off with a raised cosine
            // rather than a hard gate, which would click.
            float cut = 0.0f;
            if (fluttering) {
                const float ph = flutterFrom + flutterStep * static_cast<float>(i + 1);
                cut = flutter * 0.5f * (1.0f - std::cos(ph * kTwoPi));
            }
            float stop = cut * tongueDepth;
            if (v.tongueLeft > 0.0f) {
                v.tongueLeft -= 1.0f;
                const float r = v.tongueLeft < tongueOff ? v.tongueLeft * tongueOffInv : 1.0f;
                stop = std::max(stop, tongueDepth * (0.5f - 0.5f * std::cos(dsp::kPi * r)));
            }
            v.pipe.setTongue(stop);

            rng = rng * 1664525u + 1013904223u;
            const float white = static_cast<float>(rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
            // The breath itself: up in 4 ms and down in 25 ms.
            v.breathScale += ((v.gate ? 1.0f : 0.0f) - v.breathScale) *
                             (v.gate ? breathRise : breathFall);
            float breath = push * v.breathScale;
            if (swelling) {
                v.swellPos = std::min(1.0f, v.swellPos + v.swellStep);
                breath *= swellAt(v.swellFrom, v.swellPos);
            }
            if (jetFlutter) {
                // Flutter on a jet: the tongue breaks the air stream up, so
                // the jet and the breath behind it both drop with it.
                v.pipe.setJetCut(cut);
                breath *= 1.0f - kJetFlutterBreath * cut;
            }
            float s = v.pipe.step(breath, white * breathNoise * 0.35f * breath);

            // A pad closing on the body: a short burst of noise, windowed
            // with sin squared like Filament's hammer so neither end clicks.
            if (v.keyLeft > 0.0f) {
                v.keyLeft -= 1.0f;
                v.keyState = v.keyState * 0.85f + white * 0.15f;
                const float soft = std::sin(dsp::kPi * (1.0f - v.keyLeft * keyInv));
                s += v.keyState * keys * kKeyLevel * soft * soft;
            }

            float out = v.filter.process(s) * env;
            if (v.pendingNote >= 0) {
                // Fading, or faded and waiting for the block boundary. Keep
                // the rest of the block silent so the old note doesn't come
                // back.
                out *= v.fadeLeft > 0 ? static_cast<float>(v.fadeLeft) / static_cast<float>(kFadeFrames) : 0.0f;
                if (v.fadeLeft > 0) --v.fadeLeft;
            }
            v.outGain += gainStep;
            out *= v.outGain;
            L[i] += out;
            R[i] += out;
        }
        v.outGain = gainTo;
    }

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i] * volume * kHouse, r = R[i] * volume * kHouse;
        if (drive > 0.0001f) {
            const float k = 1.0f + drive * 8.0f;
            // Normalised on the nominal level, so drive changes the character
            // and not the level.
            const float norm = kNominal / dsp::fastTanh(kNominal * k);
            l = dsp::fastTanh(l * k) * norm;
            r = dsp::fastTanh(r * k) * norm;
        }
        L[i] = l * panL * 1.4142f;
        R[i] = r * panR * 1.4142f;
    }
    return true;
}

void Timber::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Timber::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Timber::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

} // namespace acidulous::machine
