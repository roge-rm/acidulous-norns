#include "Fret.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a note at velocity 100, set so the bank sits with the other machines. */
constexpr float kHouse = 0.9f;
constexpr float kSilent = 2e-5f;
/** Samples between retunings: vibrato, slides and the dampers move this often. */
constexpr int kRetuneEvery = 16;
/** The partner string of a twelve-string course: a unison this far sharp, cents. */
constexpr float kUnisonCents = 4.0f;
/** How long a released string rings under the fingers, seconds. */
constexpr float kReleaseT60 = 0.12f;
/** Where a palm rests, as a fraction of the string from the bridge. */
constexpr float kPalmAt = 0.035f;
/** The second coil of a humbucker, this much further along the string. */
constexpr float kSecondCoil = 0.03f;
/** A fret starts to buzz when the string swings past this. */
constexpr float kFretGap = 0.22f;
/** How far behind the strings the amp's sound reaches them again, seconds. */
constexpr float kAirDelay = 0.007f;
/** The drive stage is set to stay near its own level where a guitar usually sits. */
constexpr float kNominal = 0.32f;
/** Vibrato's rate, Hz, and its depth at full, semitones. */
constexpr float kVibratoHz = 5.5f, kVibratoDepth = 0.35f;

/** Each kind's strings and pickups. Positions are fractions of the string from the bridge. */
struct Make {
    float lowest;     // the open low string, MIDI
    bool octaves;     // twelve-string: the low four courses an octave apart
    float sustain;    // seconds the low string rings, at the sustain knob's middle
    float bright;     // how much top the string keeps, 0 to 1
    float stiffness;  // dispersion: a bass string's partials run sharp
    float neck, bridge;
    float level;
};
constexpr Make kMakes[Fret::KindCount] = {
    {40.0f, false, 6.0f, 0.93f, 0.0f, 0.25f, 0.07f, 1.0f},  // guitar
    {40.0f, true, 6.0f, 0.93f, 0.0f, 0.25f, 0.07f, 0.75f},  // twelve-string
    {35.0f, false, 7.0f, 0.9f, 0.08f, 0.22f, 0.065f, 1.0f},  // baritone
    {28.0f, false, 9.0f, 0.8f, 0.12f, 0.2f, 0.09f, 1.15f},   // bass
    {23.0f, false, 9.0f, 0.8f, 0.14f, 0.2f, 0.09f, 1.15f},   // five-string bass
    {40.0f, false, 3.5f, 0.86f, 0.0f, 0.3f, 0.08f, 1.1f},    // hollowbody: an archtop, warm and shorter, flatwounds
    {40.0f, false, 10.0f, 0.96f, 0.0f, 0.2f, 0.07f, 0.9f},   // steel: a lap or pedal steel, long and pure
    {28.0f, false, 7.0f, 0.72f, 0.1f, 0.22f, 0.1f, 0.75f},   // fretless bass: no frets, a vocal swell
};

/**
 * A harmonic: a finger on the 12th, 7th or 5th fret leaves the string
 * sounding its 2nd, 3rd or 4th mode alone. As semitones above the note.
 */
constexpr float kHarmonicSemis[4] = {0.0f, 12.0f, 19.0195f, 24.0f};

} // namespace

/** The highest pitch a string plays; see Fret::hz. A guitar's top fret is about E6, 88. */
constexpr float kHighestPitch = 100.0f;

Fret::Fret() { initParams(); }

const ParamDef *Fret::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""}, // guitar, 12-string, baritone, bass, 5-string, hollowbody, steel, fretless
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"pickup", 0.0f, 2.0f, 2.0f, Curve::Stepped, 3, ""}, // neck, both, bridge
        {"coil", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},   // single, humbucker
        {"tone", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""}, // pick, finger, slap
        {"hardness", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Where the string is struck, as a fraction of it from the bridge.
        {"position", 0.04f, 0.5f, 0.15f, Curve::Linear, 0, ""},
        {"mute", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"sustain", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"bright", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"buzz", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"harmonic", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // off, 12th, 7th, 5th
        // Chords spread across the strings: the time between strings.
        {"strum", 0.0f, 80.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"direction", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""}, // down, up, both ways in turn
        // With one voice, a new note slides from the last instead of being picked.
        {"slide", 0.0f, 400.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"vibrato", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"feedback", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"voices", 1.0f, static_cast<float>(kVoices), static_cast<float>(kVoices), Curve::Stepped, kVoices, ""},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Fret::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        v.string.prepare(sampleRate);
        v.partner.prepare(sampleRate);
        // The longest stroke: a slow finger plus a pluck halfway along the lowest string.
        v.stroke.assign(static_cast<size_t>(sampleRate * 0.06f), 0.0f);
    }
    coil.setSampleRate(sampleRate);
    cabinet.prepare(sampleRate);
    air.assign(static_cast<size_t>(sampleRate * 0.02f), 0.0f);
    reset();
}

void Fret::reset() {
    for (Voice &v : voices) {
        v.string.clear();
        v.partner.clear();
        v.used = v.held = false;
        v.strokeLength = v.strokeAt = 0;
        v.slapLeft = 0;
        v.harmonic = 0;
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.level = 0.0f;
        v.quietBlocks = 0;
    }
    waitingCount = 0;
    strumUp = false;
    bend = wheel = channelPressure_ = 0.0f;
    vibratoPhase = 0.0f;
    retuneCountdown = 0;
    coil.reset();
    coilBuiltFor = -1.0f;
    cabinet.reset();
    std::fill(air.begin(), air.end(), 0.0f);
    airAt = 0;
    dcIn = dcOut = 0.0f;
    noise = 0x7f4a7c15u;
    clock = 0;
    quietSamples = 0;
    asleep = true;
}

int Fret::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Fret::Voice *Fret::voiceFor(uint8_t note) {
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

float Fret::hz(const Voice &v, float vibrato) const {
    // Nothing above a string's reach: higher notes fold down an octave at a
    // time. Past about note 105 the string's loop is too short to lose its
    // energy, and it rings for ever.
    float p = v.pitch + bend * paramOf(BendRange) + v.noteBend + vibrato;
    while (p > kHighestPitch) p -= 12.0f;
    return noteHz(p);
}

void Fret::retune(Voice &v, float vibrato) {
    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    const float f = hz(v, vibrato);
    const float mute = clampf(paramOf(Mute) + wheel, 0.0f, 1.0f);
    // How long the string rings: the low string's time, shorter up the neck
    // (as the square root of the pitch), cut short by a palm or a lifted finger.
    const float lowHz = noteHz(k.lowest);
    float t60 = k.sustain * std::pow(4.0f, paramOf(Sustain) - 0.5f) * std::sqrt(lowHz / std::fmax(f, lowHz));
    t60 *= 1.0f - 0.93f * mute;
    if (!v.held) t60 = std::fmin(t60, kReleaseT60 * (k.lowest < 30.0f ? 1.6f : 1.0f));
    const float loop = std::pow(10.0f, -3.0f / (std::fmax(t60, 0.01f) * f));
    // The loop's one-pole: near 1 the string keeps its top for seconds; a
    // tenth lower and it's gone in a fraction of one. So the knob works in
    // the loss (1 - k), on a log scale.
    float loss = (1.0f - k.bright) * std::pow(4.0f, 0.5f - paramOf(Bright)) * std::pow(2.0f, 0.7f - v.velocity);
    if (v.harmonic > 0) loss *= 2.0f;
    loss = loss + (0.85f - loss) * mute;
    const float tone = clampf(1.0f - loss, 0.03f, 1.0f);
    for (int s = 0; s < (v.hasPartner ? 2 : 1); ++s) {
        Waveguide &w = s == 0 ? v.string : v.partner;
        w.setFrequency(s == 0 ? f : f * std::exp2(v.partnerAt / 12.0f));
        w.setDcCorner(0.01f);
        w.setDispersion(k.stiffness, k.stiffness > 0.0f ? 2 : 0);
        w.setDamping(loop, tone);
        // A palm by the bridge.
        if (mute > 0.0f) w.setDamper(kPalmAt, mute * 0.6f);
        else w.setDamper(0.5f, 0.0f);
    }
}

void Fret::shapeStroke(Voice &v) {
    // The stroke: a short push, sharper the harder the pick or finger, then
    // the same push taken away a little later, which is where along the string
    // it was struck (a comb, as plucking near the bridge thins a note).
    const int stroke = std::clamp(steppedTargetOf(Stroke), 0, StrokeCount - 1);
    const float hard = clampf(targetOf(Hardness), 0.0f, 1.0f);
    float seconds = 0.0f, grit = 0.0f;
    switch (stroke) {
    case Finger: seconds = 0.003f - 0.002f * hard; break;
    case Slap: seconds = 0.0003f; grit = 0.5f; break;
    default: seconds = 0.0009f - 0.0007f * hard; grit = 0.25f * hard; break;
    }
    const int len = std::max(4, static_cast<int>(seconds * sampleRate));
    const float period = sampleRate / hz(v, 0.0f);
    const int shift = std::max(1, static_cast<int>(std::lround(clampf(targetOf(Position), 0.04f, 0.5f) * period)));
    const int total = std::min(static_cast<int>(v.stroke.size()), len + shift);
    std::fill(v.stroke.begin(), v.stroke.begin() + total, 0.0f);
    // A slap is over in a fraction of a millisecond; it's struck harder to put in as much.
    const float amp = stroke == Slap ? 1.1f : 0.6f;
    for (int i = 0; i < len && i < total; ++i) {
        const float shape = std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(len));
        v.stroke[static_cast<size_t>(i)] += amp * (shape + grit * white());
    }
    for (int i = total - 1; i >= shift; --i) v.stroke[static_cast<size_t>(i)] -= v.stroke[static_cast<size_t>(i - shift)];
    v.strokeLength = total;
    v.strokeAt = 0;
    v.slapLeft = stroke == Slap ? static_cast<int32_t>(0.06f * sampleRate) : 0;
}

void Fret::start(uint8_t note, uint8_t velocity) {
    const int kindIndex = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kindIndex];
    const float pitch = static_cast<float>(note) + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    const float slide = targetOf(Slide);
    // With one voice, a held note slides to the new one: no new pick, just a
    // finger sliding along the fret, and a little of a hammer-on.
    if (steppedTargetOf(Voices) == 1 && slide > 0.0f && voices[0].used && voices[0].held) {
        Voice &v = voices[0];
        v.note = note;
        v.aim = pitch + kHarmonicSemis[v.harmonic];
        v.velocity = static_cast<float>(velocity) / 127.0f;
        v.age = ++clock;
        return;
    }
    Voice *v = voiceFor(note);
    v->note = note;
    v->pitch = v->aim = pitch;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->held = true;
    v->released = 0;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    // A twelve-string's low four courses have an octave string; the rest a unison a hair apart.
    v->hasPartner = kindIndex == Twelve;
    v->partnerAt = k.octaves && note < k.lowest + 17.0f ? 12.0f : kUnisonCents / 100.0f;
    // A harmonic sounds its mode of the string: higher, and purer, since the
    // touch took away everything that doesn't share the node.
    v->harmonic = std::clamp(steppedTargetOf(Harmonic), 0, 3);
    v->pitch = v->aim = pitch + kHarmonicSemis[v->harmonic];
    // A palm on the strings takes most of a note's ring away; some of the
    // level is made up, as a player digs in harder to be heard.
    v->gain = kHouse * k.level * velocityGain(v->velocity, targetOf(VelocityAmount)) * (1.0f + 3.0f * clampf(targetOf(Mute), 0.0f, 1.0f));
    if (!v->used) {
        v->string.clear();
        v->partner.clear();
    }
    retune(*v, 0.0f);
    shapeStroke(*v);
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Fret::noteOn(uint8_t note, uint8_t velocity) {
    if (targetOf(Strum) <= 0.0f) {
        start(note, velocity);
        return;
    }
    // Strummed: the notes wait for the block, and go across the strings in order.
    if (waitingCount >= kWaiting) return;
    waiting[waitingCount++] = {note, velocity, 0, false};
    asleep = false;
    quietSamples = 0;
}

void Fret::noteOff(uint8_t note) {
    for (int i = 0; i < waitingCount; ++i) {
        if (waiting[i].note == note) {
            waiting[i] = waiting[--waitingCount];
            --i;
        }
    }
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.held = false;
    }
}

void Fret::allNotesOff() {
    waitingCount = 0;
    for (Voice &v : voices) {
        if (v.used) v.held = false;
    }
}

void Fret::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel rests the palm on the strings.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Fret::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Fret::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Fret::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Fret::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

bool Fret::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const Make &k = kMakes[std::clamp(steppedTargetOf(Model), 0, KindCount - 1)];
    // A strum: the notes that arrived go across the strings, low to high on a
    // down stroke, high to low on an up.
    {
        int fresh = 0;
        for (int i = 0; i < waitingCount; ++i) fresh += waiting[i].placed ? 0 : 1;
        if (fresh > 0) {
            const int direction = std::clamp(steppedTargetOf(Direction), 0, 2);
            const bool up = direction == 1 || (direction == 2 && strumUp);
            if (direction == 2) strumUp = !strumUp;
            std::sort(waiting, waiting + waitingCount, [&](const Waiting &a, const Waiting &b) {
                if (a.placed != b.placed) return a.placed;
                return up ? a.note > b.note : a.note < b.note;
            });
            const int32_t gap = static_cast<int32_t>(targetOf(Strum) * 0.001f * sampleRate);
            int rank = 0;
            for (int i = 0; i < waitingCount; ++i) {
                if (waiting[i].placed) continue;
                waiting[i].delay = rank++ * gap;
                waiting[i].placed = true;
            }
        }
    }

    // The pickups: which, and the coil's resonance under the tone knob.
    const int pickup = std::clamp(steppedTargetOf(Pickup), 0, 2);
    const bool humbucker = steppedTargetOf(Coil) == 1;
    const float tone = clampf(paramOf(Tone), 0.0f, 1.0f);
    {
        const float key = tone + (humbucker ? 2.0f : 0.0f);
        if (std::fabs(key - coilBuiltFor) > 1e-4f) {
            coilBuiltFor = key;
            const float peak = humbucker ? 3000.0f : 5500.0f;
            coil.set(peak * (0.15f + 0.85f * tone * tone), (humbucker ? 0.3f : 0.4f) * (0.3f + 0.7f * tone));
        }
    }
    // A pickup by the bridge hears the low harmonics weakly; its winding makes up some of that.
    auto hears = [&](float at) { return 1.0f / std::pow(std::fmax(0.5f, 2.0f * std::sin(3.14159265f * at)), 0.35f); };
    const float neckGain = hears(k.neck), bridgeGain = hears(k.bridge);
    const auto heard = [&](const Waveguide &w, float out) {
        auto one = [&](float at, float g) {
            float s = out - w.tap(at);
            if (humbucker) s = 0.5f * (s + out - w.tap(at + kSecondCoil));
            return s * g;
        };
        if (pickup == 0) return one(k.neck, neckGain);
        if (pickup == 2) return one(k.bridge, bridgeGain);
        return 0.5f * (one(k.neck, neckGain) + one(k.bridge, bridgeGain));
    };

    const float drive = clampf(paramOf(Drive), 0.0f, 1.0f);
    const float driveGain = std::pow(10.0f, drive * 36.0f / 20.0f);
    const float driveComp = kNominal / std::tanh(kNominal * driveGain);
    const float speaker = clampf(drive * 4.0f, 0.0f, 1.0f); // the cabinet comes in as the amp does
    if (speaker > 0.0f) cabinet.set(0.5f, 0.5f, 0.4f, 0.3f, 0.1f);
    const float feedback = clampf(paramOf(Feedback), 0.0f, 1.0f);
    const auto airDelay = static_cast<int32_t>(kAirDelay * sampleRate);
    const auto airSize = static_cast<int32_t>(air.size());
    const float buzz = clampf(paramOf(Buzz), 0.0f, 1.0f);
    const float volume = paramOf(Volume);
    const float vibratoDepth = kVibratoDepth;
    const float slideCoef = targetOf(Slide) > 0.0f ? 1.0f - std::exp(-static_cast<float>(kRetuneEvery) / (targetOf(Slide) * 0.001f * sampleRate / 3.0f)) : 1.0f;
    const float dcPole = 1.0f - 6.2831853f * 15.0f / sampleRate;
    bool any = false;

    for (int32_t i = 0; i < frames; ++i) {
        for (int w = 0; w < waitingCount; ++w) {
            if (waiting[w].delay == i) start(waiting[w].note, waiting[w].velocity);
        }
        if (retuneCountdown-- <= 0) {
            retuneCountdown = kRetuneEvery;
            vibratoPhase += kVibratoHz * static_cast<float>(kRetuneEvery) / sampleRate;
            if (vibratoPhase >= 1.0f) vibratoPhase -= 1.0f;
            const float swing = std::sin(vibratoPhase * 6.2831853f);
            for (Voice &v : voices) {
                if (!v.used) continue;
                v.pitch += (v.aim - v.pitch) * slideCoef;
                const float press = v.pressure >= 0.0f ? v.pressure : channelPressure_;
                const float depth = clampf(paramOf(Vibrato) + press, 0.0f, 1.0f) * vibratoDepth;
                retune(v, swing * depth);
            }
        }
        // What the amp gave out a moment ago, back into the held strings.
        float back = 0.0f;
        if (feedback > 0.0f) {
            int32_t at = airAt - airDelay;
            if (at < 0) at += airSize;
            back = air[static_cast<size_t>(at)] * feedback * 0.02f;
        }
        float sum = 0.0f;
        for (Voice &v : voices) {
            if (!v.used) continue;
            float in = 0.0f;
            if (v.strokeAt < v.strokeLength) in = v.stroke[static_cast<size_t>(v.strokeAt++)] * (0.5f + 0.5f * v.velocity);
            if (v.held && back != 0.0f) in += back;
            float out = v.string.step(in);
            // The fret: a string swung past it slaps against it, and the buzz takes some of its swing.
            const float gap = kFretGap * (1.0f - 0.6f * buzz);
            const float over = std::fabs(out) - gap;
            if ((buzz > 0.0f || v.slapLeft > 0) && over > 0.0f) {
                const float amount = std::fmax(buzz, v.slapLeft > 0 ? 0.6f : 0.0f);
                const float hit = (out > 0.0f ? over : -over) * amount;
                v.string.excite(-hit * 0.3f);
                out += white() * over * amount * 0.6f;
            }
            if (v.slapLeft > 0) --v.slapLeft;
            float s = heard(v.string, out);
            if (v.hasPartner) {
                const float p = v.partner.step(in);
                s += heard(v.partner, p);
            }
            s *= v.gain;
            v.level += (std::fabs(s) - v.level) * 0.001f;
            sum += s;
        }
        // The coil, then the amp.
        float y = coil.step(sum).lp;
        if (drive > 0.0f) {
            const float driven = std::tanh(y * driveGain) * driveComp;
            y = y + (driven - y) * clampf(drive * 8.0f, 0.0f, 1.0f);
        }
        if (speaker > 0.0f) y = y + (cabinet.process(y) - y) * speaker;
        air[static_cast<size_t>(airAt)] = y;
        if (++airAt >= airSize) airAt = 0;
        const float out = y - dcIn + dcPole * dcOut;
        dcIn = y;
        dcOut = dsp::guardDenormal(out);
        L[i] = R[i] = out * volume;
        any = any || std::fabs(out) > kSilent;
    }
    // The strum's waiting notes move on by the block.
    for (int w = 0; w < waitingCount; ++w) {
        if (waiting[w].delay < frames) {
            waiting[w] = waiting[--waitingCount];
            --w;
        } else {
            waiting[w].delay -= frames;
        }
    }

    for (Voice &v : voices) {
        if (!v.used) continue;
        if (!v.held && v.level < kSilent && v.strokeAt >= v.strokeLength) {
            if (++v.quietBlocks > 8) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
    }
    quietSamples = any || activeVoices() > 0 || waitingCount > 0 ? 0 : quietSamples + frames;
    if (quietSamples > 8192) asleep = true;
    return true;
}

} // namespace acidulous::machine
