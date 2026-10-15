#include "Tine.h"
#include <algorithm>
#include <cmath>
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

/** Decay per sample for a ring time. */
float poleFor(float t60, float sampleRate) { return std::pow(10.0f, -3.0f / (std::fmax(t60, 0.005f) * sampleRate)); }

} // namespace

Tine::Tine() { initParams(); }

const ParamDef *Tine::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
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
    };
    count = Count;
    return defs;
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
        for (Mode &m : v.modes) m.y1 = m.y2 = 0.0f;
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
    bend = wheel = channelPressure_ = 0.0f;
    dampersUp = false;
    motorPhase = 0.0f;
    retuneCountdown = 0;
    noise = 0x5bd1e995u;
    clock = 0;
    quietSamples = 0;
    asleep = true;
}

int Tine::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
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
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float f = noteHz(pitchOf(v));
    const float bright = clampf(targetOf(Bright), 0.0f, 1.0f);
    const float position = clampf(targetOf(Position), 0.0f, 1.0f);
    // Lower notes ring longer: a longer, heavier bar.
    const float t60 = clampf(k.t60 * std::pow(4.0f, targetOf(Decay) - 0.5f) * std::pow(kRefHz / f, k.pitchFall), 0.05f, 40.0f);
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
}

void Tine::retune(Voice &v) {
    const float pitch = pitchOf(v);
    if (std::fabs(pitch - v.builtPitch) < 0.002f) return;
    v.builtPitch = pitch;
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
    const float hard = clampf(targetOf(Mallet) + 0.4f * (velocity - 0.6f), 0.0f, 1.0f);
    float seconds = k.contact * (0.0045f - 0.0041f * hard);
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
    v.click = k.click * velocity * (0.3f + hard);
}

void Tine::damp(Voice &v) {
    const float damp = clampf(paramOf(Damp), 0.0f, 1.0f);
    if (damp <= 0.0f || v.damped) return;
    v.damped = true;
    for (int i = 0; i < v.modeCount; ++i) {
        Mode &m = v.modes[i];
        const float t60 = m.t60 * std::pow(std::fmin(1.0f, kDampedT60 / m.t60), damp);
        m.r = poleFor(t60, sampleRate);
    }
    v.builtPitch = -1000.0f;
    retune(v);
}

void Tine::noteOn(uint8_t note, uint8_t velocity) {
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    Voice *v = voiceFor(note);
    const bool fresh = !v->used || v->note != note;
    v->note = note;
    v->pitch = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    const float vel = static_cast<float>(velocity) / 127.0f;
    v->gain = kHouse * k.level * velocityGain(vel, targetOf(VelocityAmount));
    if (fresh) {
        for (Mode &m : v->modes) m.y1 = m.y2 = 0.0f;
        std::fill(v->tube.begin(), v->tube.end(), 0.0f);
        v->bowing = 0.0f;
    }
    // A note struck again rings on from where it was, as a bar does.
    build(*v);
    strike(*v, vel);
    const float spread = clampf(targetOf(Spread), 0.0f, 1.0f);
    v->pan = spread * clampf((v->pitch - 66.0f) / 30.0f, -1.0f, 1.0f);
    const float roll = targetOf(Roll);
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
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const float tube = clampf(paramOf(Tube), 0.0f, 1.0f);
    const float motorHz = paramOf(Motor);
    const float motorDepth = motorHz > 0.0f ? clampf(paramOf(Depth) + wheel, 0.0f, 1.0f) : 0.0f;
    const float bloom = clampf(paramOf(Bloom), 0.0f, 1.0f);
    const float buzz = clampf(paramOf(Buzz), 0.0f, 1.0f);
    const float roll = paramOf(Roll);
    const float volume = paramOf(Volume);
    const float clickFall = std::exp(-1.0f / (0.0015f * sampleRate));
    const auto tubeSize = static_cast<int32_t>(voices[0].tube.size());
    bool any = false;

    for (Voice &v : voices) {
        if (!v.used) continue;
        float peak = 0.0f;
        const float pl = std::sqrt(0.5f * (1.0f - v.pan)), pr = std::sqrt(0.5f * (1.0f + v.pan));
        float phase = motorPhase;
        const float phaseStep = motorHz / sampleRate;
        int countdown = retuneCountdown;
        for (int32_t i = 0; i < frames; ++i) {
            if (countdown-- <= 0) {
                countdown = kRetuneEvery;
                retune(v);
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
            float y = 0.0f, first = 0.0f;
            if (v.settleLeft > 0) {
                --v.settleLeft;
                for (int m = 0; m < v.modeCount; ++m) {
                    v.modes[m].y1 *= v.settle;
                    v.modes[m].y2 *= v.settle;
                }
            }
            for (int m = 0; m < v.modeCount; ++m) {
                Mode &md = v.modes[m];
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
