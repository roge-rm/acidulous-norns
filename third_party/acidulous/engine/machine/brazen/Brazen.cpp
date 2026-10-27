#include "Brazen.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, used as its nominal
// level. Measured after volume x2, peak -21.5 dB. Set it too high and the
// drive knob acts like a volume knob.
constexpr float kNominal = 0.085f;
// Output gain that puts Init at the same level as the other patches (at
// 2.0 it measured 4.4 dB quieter than Trombone on the same note).
constexpr float kHouse = 3.32f;

/**
 * Below this the tube counts as silent and the voice can be freed. It's
 * about 90 dB below full scale, low enough that cutting it doesn't tick.
 */
constexpr float kSilent = 3.0e-5f;
// How long a sounding voice fades before a tongued mono note restarts it, 2 ms.
constexpr int32_t kFadeFrames = 96;

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

Brazen::Brazen() { initParams(); }

const ParamDef *Brazen::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        // 0 is a tuba's wide slow bore, 1 a trumpet's narrow bright one. It
        // sets how much of the wave the bell sends back.
        {"size", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bell", 0.05f, 0.95f, 0.55f, Curve::Linear, 0, ""},
        {"loss", 0.97f, 1.0f, 0.999f, Curve::Linear, 0, ""},
        {"mute", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"mutetone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"tension", 0.45f, 1.35f, 1.0f, Curve::Linear, 0, ""},
        {"lipdamp", 0.05f, 0.95f, 0.6f, Curve::Linear, 0, ""},
        {"pressure", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"breath", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        {"bite", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"brass", 0.0f, 1.0f, 0.45f, Curve::Linear, 0, ""},
        {"growl", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"growlrate", 10.0f, 120.0f, 45.0f, Curve::Exponential, 0, "Hz"},
        {"players", 1.0f, 4.0f, 1.0f, Curve::Stepped, 4, ""},
        {"spread", 0.0f, 60.0f, 8.0f, Curve::Linear, 0, "cents"},
        {"scatter", 0.0f, 120.0f, 15.0f, Curve::Linear, 0, "ms"},
        {"lock", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"drift", 0.0f, 50.0f, 6.0f, Curve::Linear, 0, "cents"},
        {"width", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"attack", 0.002f, 2.0f, 0.05f, Curve::Exponential, 0, "s"},
        {"decay", 0.005f, 4.0f, 0.3f, Curve::Exponential, 0, "s"},
        {"sustain", 0.0f, 1.0f, 0.85f, Curve::Linear, 0, ""},
        {"release", 0.005f, 4.0f, 0.25f, Curve::Exponential, 0, "s"},
        {"vibrato", 0.0f, 60.0f, 10.0f, Curve::Linear, 0, "cents"},
        {"vibratorate", 0.5f, 12.0f, 5.0f, Curve::Exponential, 0, "Hz"},
        {"vibratodelay", 0.0f, 2.0f, 0.35f, Curve::Linear, 0, "s"},
        {"cutoff", 200.0f, 18000.0f, 18000.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"filtertype", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""},
        {"mono", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"glide", 0.0f, 1.0f, 0.06f, Curve::Linear, 0, "s"},
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
        {"flutter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"flutterrate", 10.0f, 40.0f, 25.0f, Curve::Exponential, 0, "Hz"},
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
        // Legato is on by default, so a patch saved without it slurs onto a held key.
        defs[Legato] = ParamDef{"legato", 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, ""};
        defs[Choke] = ParamDef{"choke", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""};
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Brazen::Count];
    DestMap() {
        using B = Brazen;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {B::Size, B::McSize}, {B::Bell, B::McBell}, {B::Loss, B::McLoss}, {B::MuteTone, B::McMuteTone},
            {B::Tension, B::McTension}, {B::LipDamp, B::McLipDamp}, {B::Pressure, B::McPressure},
            {B::Breath, B::McBreath}, {B::Bite, B::McBite}, {B::Brassiness, B::McBrassiness},
            {B::Growl, B::McGrowl}, {B::GrowlRate, B::McGrowlRate},
            {B::Spread, B::McSpread}, {B::Scatter, B::McScatter}, {B::Lock, B::McLock},
            {B::Drift, B::McDrift}, {B::Width, B::McWidth},
            {B::Attack, B::McAttack}, {B::Decay, B::McDecay}, {B::Sustain, B::McSustain},
            {B::Release, B::McRelease},
            {B::Vibrato, B::McVibrato}, {B::VibratoRate, B::McVibratoRate}, {B::VibratoDelay, B::McVibratoDelay},
            {B::Cutoff, B::McCutoff}, {B::Resonance, B::McResonance}, {B::Glide, B::McGlide},
            {B::VelocityAmount, B::McVelocity},
            {B::Drive, B::McDrive}, {B::Volume, B::McVolume}, {B::Pan, B::McPan}, {B::MpeTimbre, B::McMpeTimbre},
            {B::Swell, B::McSwell}, {B::SwellTime, B::McSwellTime},
            {B::Flutter, B::McFlutter}, {B::FlutterRate, B::McFlutterRate},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Brazen::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : mac_[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Brazen::updateMacros(bool goal) {
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

void Brazen::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.amp.setSampleRate(sampleRate);
        v.filterL.setSampleRate(sampleRate);
        v.filterR.setSampleRate(sampleRate);
        for (auto &p : v.players) p.bore.prepare(sampleRate);
    }
    reset();
}

void Brazen::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.amp.kill();
        v.filterL.reset();
        v.filterR.reset();
        v.muteLpL = v.muteLpR = v.muteHpL = v.muteHpR = 0.0f;
        v.ring = 0.0f;
        for (auto &p : v.players) { p.bore.clear(); p.rng = Player::kSeed; }
        v.swellFrom = v.swellPos = 1.0f;
        v.swellStep = 0.0f;
        v.flutterPhase = 0.0f;
        v.fadeLeft = 0;
        v.pendingNote = -1;
        v.pendingOff = false;
    }
    for (auto &m : mac_) m = 0.0f;
    lastFlutter = 0.0f;
    growlPhase = 0.0f;
    rng = kRngSeed;
}

void Brazen::allNotesOff() {
    for (auto &v : voices) {
        if (!v.used) continue;
        if (v.pendingNote >= 0) v.pendingOff = true;
        v.gate = false;
        v.amp.release();
    }
}

Brazen::Voice *Brazen::allocate() {
    for (auto &v : voices) if (!v.used) return &v;
    // Of the released notes, steal the quietest instead of the oldest.
    // Starting a note clears the tube, and an old note isn't always the
    // quietest since a tuba rings much longer than a trumpet.
    Voice *best = nullptr;
    for (auto &v : voices) {
        if (v.gate) continue;
        if (best == nullptr || v.ring < best->ring) best = &v;
    }
    if (best != nullptr) return best;
    for (auto &v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

bool Brazen::slursOnto(const Voice &v) const {
    // Mono with legato slurs onto a held note, gliding over `glide` or
    // jumping at 0. Without legato, or onto a released voice with choke, it
    // slurs only while glide is on and is tongued otherwise. Poly glides
    // when it takes over a voice that is still sounding.
    const bool glide = goalOf(Glide) > 0.001f;
    if (steppedTargetOf(Mono) == 0) return glide && v.used;
    if (v.used && v.gate) return steppedTargetOf(Legato) != 0 || glide;
    return v.used && steppedTargetOf(Choke) != 0 && glide;
}

void Brazen::startVoice(Voice &v, uint8_t note, uint8_t velocity) {
    updateMacros(true); // a note reads where the knobs are going, so renders repeat
    const bool gliding = slursOnto(v);
    // A slur carries on the swell and flutter of the note it joined.
    // Anything else starts them again.
    if (!(gliding && v.gate)) {
        // 1 - swell of the set pressure, so +1 starts from nothing and -1
        // at twice it.
        const float swell = goalOf(Swell);
        v.swellFrom = 1.0f - swell;
        v.swellPos = swell == 0.0f ? 1.0f : 0.0f;
        v.swellStep = 1.0f / std::max(1.0f, goalOf(SwellTime) * sampleRate);
        v.flutterPhase = 0.0f;
    }
    v.glideFrom = gliding ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = gliding ? 0.0f : 1.0f;
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

    const int32_t players = std::clamp(steppedTargetOf(Players), 1, kPlayers);
    const float spread = goalOf(Spread);
    const float scatter = goalOf(Scatter) * 0.001f * sampleRate;
    for (int32_t i = 0; i < kPlayers; ++i) {
        Player &p = v.players[i];
        p.rng = rng = rng * 1664525u + 1013904223u;
        const float pos = players == 1 ? 0.0f : (static_cast<float>(i) / (players - 1) * 2.0f - 1.0f);
        // Each player is off the note by their own amount.
        p.home = pos * spread * 0.5f + (nextRandom() * 2.0f - 1.0f) * spread * 0.5f;
        p.offsetCents = p.home;
        p.walk = 0.0f;
        const float late = i == 0 ? 0.0f : nextRandom() * scatter;
        // A player already sounding slurs on; only a fresh tube comes in late.
        p.delayLeft = gliding ? 0.0f : late;
        p.entry = p.delayLeft > 0.0f ? 0.0f : 1.0f;
        p.breath = 1.0f - nextRandom() * 0.2f;
        p.pan = pos;
        p.pushScale = p.pushBias = 0.0f;
        if (!gliding) {
            p.bore.clear();
            // The tongue pushes on the lips while the wave builds up, so a
            // low note speaks about as fast as a high one. It needs the note
            // and a solved loop, so the frequency is set here and `tongue`
            // is called after the tune in render.
            p.bore.setFrequency(noteHz(static_cast<float>(note)));
            p.tongue = true;
        }
    }
    if (gliding) {
        // The tube is still sounding, so the breath and the filters carry on
        // from where they are. Zeroing them dips the note under the slur.
        v.amp.trigger();
        return;
    }
    // Clear the mute filters so a stolen voice doesn't start with the old
    // note's state, which would click and make renders differ.
    v.muteLpL = v.muteLpR = v.muteHpL = v.muteHpR = 0.0f;
    v.ring = 0.0f;
    v.filterL.reset();
    v.filterR.reset();
    v.amp.retrigger();
}

void Brazen::noteOn(uint8_t note, uint8_t velocity) {
    // Mono slurs onto the note that is held. A note played after the last
    // one let go is tongued on a voice of its own, so the old tail rings out
    // instead of being cut.
    // With choke, it takes the last voice let go instead and cuts its tail.
    // A tongued note there fades the old one out first, so it doesn't stop
    // dead with a click.
    Voice *v = nullptr;
    if (steppedTargetOf(Mono) != 0) {
        for (auto &w : voices) {
            if (w.used && (w.gate || w.pendingNote >= 0)) { v = &w; break; }
        }
        if (v == nullptr && steppedTargetOf(Choke) != 0) {
            for (auto &w : voices) {
                if (w.used && (v == nullptr || w.age > v->age)) v = &w;
            }
        }
        if (v != nullptr) {
            updateMacros(true);
            if (v->pendingNote >= 0 || !slursOnto(*v)) {
                // Waiting already, this one takes its place and the fade
                // goes on where it is. Otherwise start the fade.
                if (v->pendingNote < 0) v->fadeLeft = kFadeFrames;
                v->pendingNote = note;
                v->pendingVel = velocity;
                v->pendingOff = false;
                return;
            }
        }
    }
    if (v == nullptr) v = allocate();
    if (v == nullptr) return;
    startVoice(*v, note, velocity);
}

void Brazen::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.pendingNote == note) {
            v.pendingOff = true; // let go before it started: start it, then release
            continue;
        }
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
        }
    }
}

void Brazen::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Brazen::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Brazen::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }
void Brazen::onBlock(int64_t, int64_t, float) {}

float Brazen::sectionSpreadCents() const { return lastSpreadCents; }

bool Brazen::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;

    const float dt = 1.0f / sampleRate;
    const int32_t players = std::clamp(steppedOf(Players), 1, kPlayers);
    const float size = paramOf(Size);
    // A wide tube reflects nearly everything, mostly lows. A narrow one
    // loses more and lets more out. The tube must be close to lossless or
    // the harmonics die out and it sounds like a flute.
    const float bellCut = paramOf(Bell) * (0.15f + size * 0.55f);
    const float reflect = 0.99f - size * 0.04f;
    const float loss = paramOf(Loss);
    const float tension = paramOf(Tension);
    const float lipDamp = paramOf(LipDamp);
    const float mouth = paramOf(Pressure);
    const float breathNoise = paramOf(Breath);
    const float bite = paramOf(Bite);
    const float brass = paramOf(Brassiness);
    const float growl = paramOf(Growl), growlRate = paramOf(GrowlRate);
    const float lock = paramOf(Lock), drift = paramOf(Drift);
    const float width = paramOf(Width);
    const float vibrato = paramOf(Vibrato), vibratoRate = paramOf(VibratoRate);
    const float glide = paramOf(Glide);
    const float bendMul = std::pow(2.0f, bend * paramOf(BendRange) / 12.0f);
    float tune = std::pow(2.0f, (paramOf(Octave) * 12.0f + paramOf(Transpose) + paramOf(Fine) * 0.01f) / 12.0f);
    // A macro's pitch, 24 semitones a unit. Apart so the sum above stays as it was.
    if (mac_[McPitch] != 0.0f) tune *= std::pow(2.0f, mac_[McPitch] * 2.0f);
    const float velAmount = paramOf(VelocityAmount);
    const int32_t mute = steppedOf(MuteKind);
    const float muteTone = paramOf(MuteTone);
    // A mute passes a band and cuts the rest. Tone slides the band up and
    // down.
    const float muteShift = std::pow(2.0f, muteTone * 2.0f - 1.0f);
    const float muteHpHz = (mute == Straight ? 700.0f : mute == Cup ? 220.0f : 1100.0f) * muteShift;
    const float muteLpHz = (mute == Straight ? 6000.0f : mute == Cup ? 2000.0f : 3600.0f) * muteShift;
    const float muteGain = mute == Straight ? 1.6f : mute == Cup ? 1.3f : 1.9f;
    // One-poles using the exact exponential coefficient. The small-angle
    // version `min(1, 2 pi f / sr)` hits 1 at high frequencies (like the
    // straight mute's 12 kHz at mutetone 1) and the filter stops filtering.
    const float muteHp = dsp::onePoleCoeff(1.0f / (kTwoPi * muteHpHz), sampleRate);
    const float muteLp = dsp::onePoleCoeff(1.0f / (kTwoPi * muteLpHz), sampleRate);
    const float drive = paramOf(Drive), volume = paramOf(Volume);
    const float panKnob = paramOf(Pan);
    const float panL = std::cos((panKnob + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((panKnob + 1.0f) * 0.25f * 3.14159265f);

    // 5 ms, so it follows the tube's decay and not its waveform.
    const float ringCoeff = dsp::onePoleCoeff(0.005f, sampleRate);
    // Matches the amp envelope's attack, so a late player comes in the same
    // way the first one did.
    const float entryCoeff = dsp::onePoleCoeff(paramOf(Attack) * 0.4f, sampleRate);

    // Flutter, its depth ramped across the block so a moving knob can't
    // step it. Off unless either end is above 0.
    const float flutterFrom = lastFlutter, flutterTo = paramOf(Flutter);
    lastFlutter = flutterTo;
    const bool fluttering = flutterFrom > 0.0f || flutterTo > 0.0f;
    const float flutterStep = paramOf(FlutterRate) * dt;
    const float flutterSlope = (flutterTo - flutterFrom) / static_cast<float>(frames);

    const float growlStart = growlPhase;
    const float growlStep = growlRate * dt;
    growlPhase += growlStep * static_cast<float>(frames);
    while (growlPhase >= 1.0f) growlPhase -= 1.0f;

    for (auto &v : voices) {
        if (!v.used) continue;
        if (v.pendingNote >= 0 && v.fadeLeft <= 0) {
            // The old note has faded. Start the waiting one at the block's
            // start, then put the block's macros back.
            startVoice(v, static_cast<uint8_t>(v.pendingNote), v.pendingVel);
            v.pendingNote = -1;
            if (v.pendingOff) {
                v.pendingOff = false;
                v.gate = false;
                v.amp.release();
            }
            updateMacros();
        }
        v.amp.set(0.0f, paramOf(Attack), paramOf(Decay), paramOf(Sustain), paramOf(Release), false);
        v.filterL.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType), dsp::MultiFilter::Clean, 0.0f);
        v.filterR.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType), dsp::MultiFilter::Clean, 0.0f);

        if (v.glidePos < 1.0f) {
            v.glidePos = std::min(1.0f, v.glidePos + (glide > 0.001f ? dt * frames / glide : 1.0f));
            v.freq = v.glideFrom + (noteHz(static_cast<float>(v.note)) - v.glideFrom) * v.glidePos;
        } else {
            v.freq = noteHz(static_cast<float>(v.note));
        }

        // --- the section tunes to itself -------------------------------------
        // Each player has their own idea of the note, drifts off it slowly,
        // and is pulled toward the section's average. At lock 0 they ignore
        // each other. At 1 they quickly settle on the average and sound like
        // one horn.
        float mean = 0.0f;
        for (int32_t i = 0; i < players; ++i) mean += v.players[i].offsetCents;
        mean /= static_cast<float>(players);
        const float rate = std::min(0.5f, static_cast<float>(frames) / (0.08f * sampleRate));
        float spreadNow = 0.0f;
        for (int32_t i = 0; i < players; ++i) {
            Player &p = v.players[i];
            p.rng = p.rng * 1664525u + 1013904223u;
            const float r = static_cast<float>(p.rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
            p.walk = std::clamp(p.walk + r * drift * 0.02f * rate * 8.0f, -drift, drift);
            const float ownPull = (p.home + p.walk) - p.offsetCents;
            const float sectionPull = (mean - p.offsetCents) * lock * 8.0f;
            p.offsetCents += (ownPull + sectionPull) * rate;
            spreadNow = std::max(spreadNow, std::fabs(p.offsetCents - mean));
        }
        lastSpreadCents = spreadNow * 2.0f;

        const float vibratoDepth = v.vibratoLeft > 0.0f ? 0.0f : vibrato * (0.35f + modWheel * 0.65f);
        if (v.vibratoLeft > 0.0f) v.vibratoLeft -= dt * frames;
        v.vibratoPhase += vibratoRate * dt * frames;
        while (v.vibratoPhase >= 1.0f) v.vibratoPhase -= 1.0f;
        const float vib = std::sin(v.vibratoPhase * kTwoPi) * vibratoDepth;

        // Velocity sets how hard the player blows (harder is brighter) and
        // also the level, with the same law as every machine. Blowing alone
        // can't make a note quiet, since below a point the lips don't sound.
        const float vel = 1.0f - 0.6f * velAmount * (1.0f - v.velocity);
        // Ramped across the block, since a slurred note changes it mid-sound.
        const float gainTo = velocityGain(v.velocity, velAmount);
        const float gainStep = (gainTo - v.outGain) / static_cast<float>(frames);
        const float growlNow = growl * (0.5f + 0.5f * std::sin(growlPhase * kTwoPi));
        const float env0 = v.amp.value();
        // The swell moves the breath as the pressure knob would: the solve
        // sees it once a block and the breath below follows it every sample.
        const bool swelling = v.swellPos < 1.0f;
        const float swell0 = swelling ? swellAt(v.swellFrom, v.swellPos) : 1.0f;
        const float flutterPhase0 = v.flutterPhase;

        // Set up the horn once a block. tune() is expensive and the note
        // doesn't change audibly within a block.
        for (int32_t pi = 0; pi < players; ++pi) {
            Player &p = v.players[pi];
            const float hz = v.freq * tune * bendMul * noteBendMul(v) *
                             std::pow(2.0f, (p.offsetCents + vib) / 1200.0f);
            const float prs = v.pressure >= 0.0f ? v.pressure : pressure;
            // Two expressions so a note with no swell is worked out exactly
            // as before.
            const float push = swelling
                ? mouth * env0 * vel * p.breath * (1.0f - growlNow * 0.5f) * swell0 + prs * 0.3f
                : mouth * env0 * vel * p.breath * (1.0f - growlNow * 0.5f) + prs * 0.3f;
            p.bore.setFrequency(hz);
            p.bore.setSmooth(swelling);
            // Lips tighten as the player blows harder, so the note arrives
            // before the tone does. The growl pushes on the lips and the air
            // at once so it buzzes instead of just wobbling. Slide (MPE
            // timbre) also tightens the lips, like embouchure.
            const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
            p.bore.setLips(tension * (0.94f + env0 * bite * 0.12f) * (1.0f + growlNow * 0.06f) *
                               (1.0f + slide * paramOf(MpeTimbre) * 0.35f),
                           lipDamp);
            p.bore.setLipGain(0.7f + bite * 0.6f);
            p.bore.setBell(reflect, bellCut);
            // Brassiness also closes the lips, since a narrow pulse is part of
            // what makes a loud horn bright.
            p.bore.setRest(0.35f);
            p.bore.setBite(bite * 0.9f + brass * 0.5f);
            // The steepening depends on how loud the wave is, so it follows
            // the pressure and not the envelope.
            p.bore.setBrass(brass * std::min(1.0f, push * 1.4f));
            p.bore.setLoss(loss);
            p.bore.setPressure(push);
            // While it swells, the loop keeps the gain of the pressure it's
            // swelling to. See Bore::setHeldPressure.
            p.bore.setHeldPressure(swelling ? mouth * env0 * vel * p.breath * (1.0f - growlNow * 0.5f) + prs * 0.3f
                                            : -1.0f);
            p.bore.tune();
            // Where this player sits. Worked out once a block to save the
            // trig calls per sample.
            const float panNow = std::clamp(p.pan * width, -1.0f, 1.0f);
            const float panAngle = (panNow + 1.0f) * 0.25f * 3.14159265f;
            p.panL = std::cos(panAngle);
            p.panR = std::sin(panAngle);
            // The parts of `push` that don't change within the block, so the
            // sample loop can apply the envelope per sample. See step() below.
            p.pushScale = mouth * vel * p.breath * (1.0f - growlNow * 0.5f);
            p.pushBias = prs * 0.3f;
            if (p.tongue) {
                // After the loop is solved for this note, since `tongue` uses
                // the solved gain.
                p.bore.tune();
                p.bore.tongue();
                p.tongue = false;
            }
        }

        if (fluttering) {
            v.flutterPhase += flutterStep * static_cast<float>(frames);
            v.flutterPhase -= std::floor(v.flutterPhase);
        }

        for (int32_t i = 0; i < frames; ++i) {
            const float env = v.amp.next();
            // Free the voice once the player has stopped and the tube is
            // silent. See Voice::ring.
            if (env <= 0.0000005f && !v.gate && v.ring < kSilent && v.pendingNote < 0) { v.used = false; break; }
            // The growl is a tremolo on the output, so it's worked out per
            // sample. Per block it would give audible steps.
            const float growlAmNow =
                growl > 0.0001f
                    ? 1.0f - growl * (0.5f + 0.5f * std::sin((growlStart + growlStep * static_cast<float>(i)) * kTwoPi)) * 0.35f
                    : 1.0f;
            // The swell and the flutter scale the player's own breath, every
            // sample. The flutter is a raised cosine from 0 at the tongued
            // note, so the note starts open and the air is cut smoothly.
            float air = 1.0f, lips = 1.0f;
            const bool shaped = swelling || fluttering;
            if (swelling) {
                v.swellPos = std::min(1.0f, v.swellPos + v.swellStep);
                air = swellAt(v.swellFrom, v.swellPos);
            }
            if (fluttering) {
                const float fi = static_cast<float>(i + 1);
                const float depth = flutterFrom + flutterSlope * fi;
                const float ph = flutterPhase0 + flutterStep * fi;
                lips = 1.0f - depth * 0.5f * (1.0f - std::cos(ph * kTwoPi));
                air *= lips;
            }
            float l = 0.0f, r = 0.0f;
            for (int32_t pi = 0; pi < players; ++pi) {
                Player &p = v.players[pi];
                if (p.delayLeft > 0.0f) { p.delayLeft -= 1.0f; continue; }
                rng = rng * 1664525u + 1013904223u;
                const float hiss = (static_cast<float>(rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * breathNoise * 0.25f;
                // Mouth pressure follows the envelope per sample, so the
                // attack is a smooth ramp and not steps that would buzz. The
                // block value still feeds setPressure, setBrass and the solve.
                // A late player also gets its own attack, see Player::entry.
                p.entry += (1.0f - p.entry) * entryCoeff;
                const float scaleNow = shaped ? p.pushScale * air : p.pushScale;
                const float pushNow = (scaleNow * env + p.pushBias) * p.entry;
                const float s = p.bore.step(pushNow, hiss * pushNow, lips) * growlAmNow;
                l += s * p.panL;
                r += s * p.panR;
            }
            v.ring += (std::fabs(l) + std::fabs(r) - v.ring) * ringCoeff;
            // The mute passes a band and cuts the rest. Straight is bright
            // and thin, cup is dark and close, harmon is nasal.
            if (mute != Open) {
                v.muteLpL += (l - v.muteLpL) * muteHp;
                v.muteLpR += (r - v.muteLpR) * muteHp;
                l -= v.muteLpL;
                r -= v.muteLpR;
                v.muteHpL += (l - v.muteHpL) * muteLp;
                v.muteHpR += (r - v.muteHpR) * muteLp;
                l = v.muteHpL * muteGain;
                r = v.muteHpR * muteGain;
            }
            v.outGain += gainStep;
            float fade = 1.0f;
            if (v.pendingNote >= 0) {
                // Fading, or faded and waiting for the next block.
                fade = v.fadeLeft > 0 ? static_cast<float>(v.fadeLeft) / static_cast<float>(kFadeFrames) : 0.0f;
                if (v.fadeLeft > 0) --v.fadeLeft;
            }
            L[i] += v.filterL.process(l) * v.outGain * fade;
            R[i] += v.filterR.process(r) * v.outGain * fade;
        }
        v.outGain = gainTo;
    }

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i] * volume * kHouse, r = R[i] * volume * kHouse;
        if (drive > 0.0001f) {
            const float k = 1.0f + drive * 8.0f;
            // Normalised on the nominal level so the drive knob changes the
            // character and not the level.
            const float norm = kNominal / dsp::fastTanh(kNominal * k);
            l = dsp::fastTanh(l * k) * norm;
            r = dsp::fastTanh(r * k) * norm;
        }
        L[i] = l * panL * 1.4142f;
        R[i] = r * panR * 1.4142f;
    }
    return true;
}

void Brazen::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Brazen::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Brazen::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

} // namespace acidulous::machine
