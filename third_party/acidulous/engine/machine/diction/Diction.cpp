#include "Diction.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

using dsp::clampf;
using dsp::kTwoPi;
using namespace diction;

namespace {

uint32_t nextRandom(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float bipolar(uint32_t &s) { return static_cast<float>(nextRandom(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

/** Seconds the gains take to follow the levels worked out for them. */
constexpr float kLevelsFollow = 0.003f;
/** Seconds for hiss to stop once its sound is over. */
constexpr float kHissGone = 0.008f;
bool isVowel(Kind k) { return k == Kind::Vowel || k == Kind::Diphthong; }
/** A sound that can be held when a note has no vowel: mmm, sss. */
bool holdable(Kind k) {
    return k == Kind::Nasal || k == Kind::Liquid || k == Kind::Glide || k == Kind::Fricative || k == Kind::Aspirate;
}

} // namespace

const char *const Diction::kFromOrder[24] = {
    "P", "B", "T", "D", "K", "G", "CH", "JH", "F", "V", "TH", "DH", "S", "Z", "SH", "ZH", "HH",
    "M", "N", "NG", "L", "R", "W", "Y",
};

Diction::Diction() { initParams(); }

const ParamDef *Diction::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // oo, oh, ah, eh, ee, from the back of the mouth to the front, so
        // sweeping it moves through the vowels in between. What a note with
        // no words sings.
        {"vowel", 0.0f, 4.0f, 2.0f, Curve::Linear, 0, ""},
        // The size of the throat, apart from the pitch. Down is bigger and
        // darker, up is smaller and brighter.
        {"formant", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "st"},
        {"breath", 0.0f, 1.0f, 0.12f, Curve::Linear, 0, ""},
        // How long the consonants take: half as long to half as long again.
        {"consonants", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How the words are said: prairie, central or american. The engine
        // doesn't read it; the words are turned into sounds with it before
        // they get here.
        {"accent", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""},

        {"vibrato", 0.0f, 100.0f, 30.0f, Curve::Linear, 0, "ct"},
        {"vibratorate", 3.0f, 9.0f, 5.5f, Curve::Linear, 0, "Hz"},
        // A singer holds a note straight for a moment before the vibrato comes in.
        {"vibratodelay", 0.0f, 2.0f, 0.35f, Curve::Linear, 0, "s"},
        // How unsteady the voice is: the wander in a held note and the scoop
        // up into a new one.
        {"drift", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},

        {"glide", 0.0f, 1.0f, 0.08f, Curve::Linear, 0, "s"},
        {"attack", 0.005f, 1.0f, 0.04f, Curve::Exponential, 0, "s"},
        {"release", 0.01f, 2.0f, 0.18f, Curve::Exponential, 0, "s"},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},

        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},

        {"volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},

        // A recorded voice's consonants against its vowels.
        {"consonantlevel", -12.0f, 6.0f, 0.0f, Curve::Linear, 0, "dB"},
        // Which take each consonant is formed from: sung between ahs, ees or
        // oos, or whichever is nearest the word's own vowel.
#define FROM(name) {"from" name, 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}
        FROM("p"), FROM("b"), FROM("t"), FROM("d"), FROM("k"), FROM("g"), FROM("ch"), FROM("jh"),
        FROM("f"), FROM("v"), FROM("th"), FROM("dh"), FROM("s"), FROM("z"), FROM("sh"), FROM("zh"), FROM("hh"),
        FROM("m"), FROM("n"), FROM("ng"), FROM("l"), FROM("r"), FROM("w"), FROM("y"),
#undef FROM
        // A recorded voice with its breath taken out: none to as much as can be.
        {"clean", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},

        // Soft to belted: the source's top turned down or up against the rest.
        {"effort", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Pulses uneven in time and size.
        {"rasp", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Every other pulse weaker, down to an octave below: fry, then a growl.
        {"growl", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"whisper", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // How far below its note each note starts (above, below zero), as
        // well as the drift's own.
        {"scoop", -2.0f, 2.0f, 0.0f, Curve::Linear, 0, "st"},
        // The throat moving with the pitch: all the way up, played up an
        // octave it's a throat half the size; below zero, the other way.
        {"track", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},

        // How many sing, and how unlike each other: late, off pitch, a
        // throat of their own and spread across the stereo.
        {"singers", 1.0f, 6.0f, 1.0f, Curve::Stepped, 6, ""},
        {"spread", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Off, or a chord sings the words on every note.
        {"harmony", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},

        // The built-in voice's folds instead of the singer's, and its throat.
        {"source", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"throat", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"morph", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Off, or the track whose sound it mouths the words with. Named as an
        // effect's, so the engine and the app treat it the same.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
    };
    count = Count;
    return defs;
}

void Diction::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    phones = phoneTable(phoneCount);
    throat.prepare(sampleRate);
    amp.setSampleRate(sampleRate);
    // The folds' pulse at the reference pitch, as the throat gets it, for talking.
    {
        const auto period = static_cast<int32_t>(sampleRate / Throat::kReferenceHz);
        double power = 0.0;
        float before = throat.flow(0.0f);
        for (int32_t i = 1; i <= period; ++i) {
            const float f = throat.flow(static_cast<float>(i) / static_cast<float>(period));
            power += static_cast<double>(f - before) * (f - before);
            before = f;
        }
        foldsRms = static_cast<float>(std::sqrt(power / period));
    }
    reset();
}

namespace {

/** [line] at [back] frames before [head], a ring of [size] (a power of two), read by four-point Hermite. */
float ringAt(const std::vector<float> &line, int32_t head, float back, int32_t size) {
    const float at = static_cast<float>(head) - back;
    const float wrapped = at < 0.0f ? at + static_cast<float>(size) : at;
    const auto i0 = static_cast<int32_t>(wrapped);
    const float t = wrapped - static_cast<float>(i0);
    auto y = [&](int32_t k) { return line[static_cast<size_t>((i0 + k) & (size - 1))]; };
    const float ym = y(-1), y0 = y(0), y1 = y(1), y2 = y(2);
    const float c1 = 0.5f * (y1 - ym);
    const float c2 = ym - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + y0;
}

} // namespace

float Diction::pulseCharacter(float &period, bool &odd, uint32_t &random) {
    const float rasp = clampf(paramOf(Rasp), 0.0f, 1.0f);
    const float growl = clampf(paramOf(Growl), 0.0f, 1.0f);
    float gain = 1.0f;
    if (rasp > 0.0f) {
        period *= 1.0f + 0.06f * rasp * bipolar(random);
        gain *= 1.0f + 0.4f * rasp * bipolar(random);
    }
    if (growl > 0.0f) {
        // A pair of pulses, the second weaker and later, the first as much
        // earlier, so the pitch is still the note.
        odd = !odd;
        if (odd) gain *= 1.0f - 0.85f * growl;
        period *= 1.0f + (odd ? 0.12f : -0.12f) * growl;
    }
    return gain;
}

void Diction::addHarmony(uint8_t n) {
    for (int32_t i = 0; i < harmonyCount; ++i) if (harmony[i] == n) return;
    if (harmonyCount < kHarmony) harmony[harmonyCount++] = n;
}

void Diction::dropHarmony(uint8_t n) {
    int32_t kept = 0;
    for (int32_t i = 0; i < harmonyCount; ++i) if (harmony[i] != n) harmony[kept++] = harmony[i];
    harmonyCount = kept;
}

void Diction::assignClocks() {
    const int32_t singers = std::clamp(steppedOf(Singers), 1, 6);
    // The clocks wanted: the lead's copies, then each harmony note and its copies.
    struct Want { uint8_t note; int32_t copy; };
    Want want[kClocks];
    int32_t wanted = 0;
    for (int32_t k = 1; k < singers && wanted < kClocks; ++k) want[wanted++] = {0, k};
    if (harmonyOn()) {
        for (int32_t h = 0; h < harmonyCount; ++h) {
            for (int32_t k = 0; k < singers && wanted < kClocks; ++k) want[wanted++] = {harmony[h], k};
        }
    }
    bool have[kClocks]{};
    for (Clock &c : clocks) {
        if (!c.on && c.gain <= 0.0f) continue;
        c.on = false;
        for (int32_t w = 0; w < wanted; ++w) {
            if (!have[w] && want[w].note == c.note && want[w].copy == c.copy) { c.on = have[w] = true; break; }
        }
    }
    for (int32_t w = 0; w < wanted; ++w) {
        if (have[w]) continue;
        for (Clock &c : clocks) {
            if (c.on || c.gain > 0.0f) continue;
            c = Clock{};
            c.on = true;
            c.note = want[w].note;
            c.copy = want[w].copy;
            c.random = 0x9e3779b9u * static_cast<uint32_t>(c.copy + 1) + 0x7f4a7c15u * c.note + 1u;
            break;
        }
    }
    // What each copy is like, from its number, so it's the same copy whenever it's there.
    const float spread = clampf(paramOf(Spread), 0.0f, 1.0f);
    const bool had = anyClocks;
    anyClocks = false;
    for (Clock &c : clocks) {
        if (!c.on && c.gain <= 0.0f) continue;
        anyClocks = true;
        if (c.copy == 0) continue;
        uint32_t h = 0x2545f491u * static_cast<uint32_t>(c.copy) + 0x51ee7u;
        auto unit = [&h]() { return 0.5f * (bipolar(h) + 1.0f); };
        c.delay = static_cast<int32_t>(spread * (0.006f + 0.028f * unit()) * sampleRate);
        c.ratio = std::exp2(spread * bipolar(h) * 1.5f / 12.0f);
        const float side = (c.copy % 2 == 1 ? -1.0f : 1.0f) * spread * (0.35f + 0.65f * unit());
        c.panL = std::cos((side + 1.0f) * 0.25f * dsp::kPi);
        c.panR = std::sin((side + 1.0f) * 0.25f * dsp::kPi);
        c.shift = spread * (0.05f + 0.2f * unit()) * sampleRate;
    }
    // The last singer gone quiet: nothing left in the ring for the next ones.
    if (had && !anyClocks) {
        std::fill(ringL.begin(), ringL.end(), 0.0f);
        std::fill(ringR.begin(), ringR.end(), 0.0f);
        together = 1.0f;
    }
}

float Diction::clockPitch(const Clock &c) const {
    if (c.note == 0) return pitch;
    return static_cast<float>(c.note) + static_cast<float>(steppedOf(Transpose)) + 12.0f * static_cast<float>(steppedOf(Octave)) + bend;
}

float Diction::clockPeriod(Clock &c) {
    const float period = clampf(sampleRate / noteHz(clockPitch(c) + wobble + c.cents * 0.01f), 8.0f, 2000.0f);
    // A copy wanders off the pitch on its own, up to 18 cents at full spread.
    if (c.copy > 0) {
        c.cents += (c.centsTarget - c.cents) * (1.0f - std::exp(-period / (0.3f * sampleRate)));
        c.wanderIn -= period;
        if (c.wanderIn <= 0.0f) {
            c.centsTarget = bipolar(c.random) * 18.0f * clampf(paramOf(Spread), 0.0f, 1.0f);
            c.wanderIn = sampleRate * (0.3f + 0.25f * (bipolar(c.random) + 1.0f));
        }
    }
    return period;
}

void Diction::reset() {
    pendingCount = 0;
    heldCount = 0;
    note = 0;
    gate = false;
    sounding = false;
    velocity = 1.0f;
    pitch = 0.0f;
    glideFrom = 0.0f;
    glideDone = 1.0f;
    scoop = 0.0f;
    bend = 0.0f;
    pressure = 0.0f;
    modWheel = 0.0f;
    sinceOnset = 0.0f;
    vibratoPhase = 0.0f;
    wander = wanderTarget = 0.0f;
    wanderCountdown = 0;
    seed = 0x2545f491u;
    noiseSeed = 0x51ee7u;
    pulseSeed = 0x9e3779b9u;
    oddPulse = false;
    tiltLow = 0.0f;
    loudIn = loudOut = 0.0f;
    for (Clock &c : clocks) c = Clock{};
    anyClocks = false;
    harmonyCount = 0;
    wobble = 0.0f;
    std::fill(ringL.begin(), ringL.end(), 0.0f);
    std::fill(ringR.begin(), ringR.end(), 0.0f);
    ringHead = 0;
    tiltLowL = tiltLowR = 0.0f;
    together = 1.0f;
    std::fill(accSource.begin(), accSource.end(), 0.0f);
    sourceGrains = false;
    for (float &k : latK) k = 0.0f;
    for (float &b : latB) b = 0.0f;
    latGain = latGainTarget = 0.0f;
    latTarget = nullptr;
    crossVoiced = 0.0f;
    crossWanted = false;
    voicedPeriod = 0.0f;
    levelSung = levelThroat = levelFolds = levelSource = 0.0f;
    levelThroatOut = levelSungOut = 0.0f;
    throatMatch = 1.0f;
    tiltLowCross = 0.0f;
    foldsBefore = foldsEarlier = deemphasis = levelEven = 0.0f;
    for (float &k : morphK) k = 0.0f;
    std::fill(rawLine.begin(), rawLine.end(), 0.0f);
    phase = 1.0f;
    phaseStep = 0.0f;
    strength = 1.0f;
    previousFlow = 0.0f;
    pulseScale = 1.0f;
    sungHz = Throat::kReferenceHz;
    snapLevels = false;
    stepCount = 0;
    stepIndex = 0;
    stepTime = 0.0f;
    letGo = false;
    wordsDone = false;
    for (int32_t k = 0; k < 3; ++k) {
        formants[k] = glideStart[k] = kKnobVowels[2][k];
        level[k] = levelStep[k] = 0.0f;
    }
    nasal = nasalStart = 0.0f;
    voiceGain = airGain = voiceGainTarget = airGainTarget = 1.0f;
    for (float &g : gainsFor) g = -1.0f;
    untilControl = 0;
    sinceLevels = 0;
    throat.reset();
    amp.reset();
    reading = fading = Reader{};
    fade = 1.0f;
    fadeStep = 0.0f;
    untilGrain = 0.0f;
    std::fill(acc.begin(), acc.end(), 0.0f);
    accHead = 0;
    std::fill(combLine.begin(), combLine.end(), 0.0f);
    combHead = 0;
    comb = 0.0f;
    grainPeriod = 1.0f;
    grainHeld = false;
    aheadCount = 0;
    earlyCount = 0;
    pitchIn = 0;
    nextNote = 0;
}

void *Diction::swapObject(int32_t slot, void *object) {
    if (slot == 1) {
        // What's paired belongs to the old voice to morph to.
        for (Reader *r : {&reading, &fading}) {
            r->soundB = nullptr;
            r->tractB = nullptr;
        }
        void *old = const_cast<RecordedVoice *>(voiceB);
        voiceB = static_cast<const RecordedVoice *>(object);
        return old;
    }
    if (slot != 0) return object;
    // What's being read belongs to the old voice.
    reading = fading = Reader{};
    for (Step &s : steps) {
        s.join = nullptr;
        s.unit = nullptr;
        s.once = false;
    }
    fade = 1.0f;
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (Clock &c : clocks) c.grainCount = 0;
    std::fill(ringL.begin(), ringL.end(), 0.0f);
    std::fill(ringR.begin(), ringR.end(), 0.0f);
    void *old = const_cast<RecordedVoice *>(voice);
    voice = static_cast<const RecordedVoice *>(object);
    return old;
}

// --- a recorded voice ------------------------------------------------------------

namespace {

constexpr int32_t kWindowSize = 1024;

const float *hannTable() {
    // Built once, by whichever thread asks first: the static's initialiser
    // runs exactly once even with two Diction tracks starting together.
    static float table[kWindowSize];
    static const bool built = [] {
        for (int32_t i = 0; i < kWindowSize; ++i) {
            table[i] = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(kWindowSize - 1));
        }
        return true;
    }();
    (void)built;
    return table;
}

/** Seconds to cross from one recorded vowel to the next, at least. */
constexpr float kShortestFade = 0.03f;
/** Seconds a recorded consonant ending the words takes to fade, rather than run on into the vowel after it. */
constexpr float kRecordedGone = 0.03f;
/** How much of its vowel a quick note's word sings before the next word may begin. */
constexpr float kVowelFirst = 0.06f;
/** Seconds to cross into a recorded consonant. */
constexpr float kJoinFade = 0.015f;
/**
 * Seconds of the vowel kept either side of a recorded consonant, where it
 * meets a vowel: the way in and out. More, and the vowel it was sung with
 * was heard instead of the word's.
 */
constexpr float kIntoVowel = 0.025f;
constexpr float kOutOfVowel = 0.025f;
/** And where it meets another consonant. */
constexpr float kIntoConsonant = 0.005f;

} // namespace

void Diction::chooseUnit(const Step &s, const float target[3]) {
    // Only a vowel, or the vowel control, chooses: through a consonant the
    // vowel before it carries on, under the consonant's own level.
    const Kind kind = phones[s.phone < phoneCount ? s.phone : 0].kind;
    if (!s.knob && !isVowel(kind)) return;
    // A diphthong's move is read once through, set up as its step begins.
    if (s.once) return;
    // A vowel is sung as itself; the vowel control and the ends of a
    // diphthong by what they sound nearest to.
    const RecordedVoice::Unit *u = s.unit != nullptr ? s.unit
                                   : !s.knob && kind == Kind::Vowel ? voice->forPhone(s.phone) : nullptr;
    if (u == nullptr) u = voice->nearest(target);
    if (u == nullptr || u == reading.owner) return;
    Reader r;
    r.owner = u;
    r.sound = &u->sound;
    r.tract = &u->tract;
    pairWithB(r, u, nullptr);
    r.gain = u->gain;
    r.from = u->from;
    r.to = u->to;
    r.held = true;
    r.pos = static_cast<float>(u->from);
    startReading(r, std::max(kShortestFade, s.glide));
}

void Diction::pairWithB(Reader &r, const RecordedVoice::Unit *unit, const RecordedVoice::Join *join) const {
    r.soundB = nullptr;
    r.tractB = nullptr;
    if (voiceB == nullptr) return;
    if (unit != nullptr) {
        // A diphthong with the other's diphthong, a vowel with its vowel or the nearest.
        const RecordedVoice::Unit *b = unit->glideTo > 0 ? voiceB->diphthong(unit->phone) : voiceB->forPhone(unit->phone);
        if (b == nullptr) b = voiceB->nearest(unit->f);
        if (b == nullptr) return;
        r.soundB = &b->sound;
        r.tractB = &b->tract;
        r.gainB = b->gain;
        r.aFrom = unit->from;
        r.aTo = unit->to;
        r.bFrom = b->from;
        r.bTo = b->to;
    } else if (join != nullptr) {
        // The consonant between the same vowels, or the other's nearest.
        const RecordedVoice::Join *b = nullptr;
        for (const RecordedVoice::Join &j : voiceB->joins) {
            if (j.phone == join->phone && j.carrier == join->carrier) { b = &j; break; }
        }
        if (b == nullptr) b = voiceB->consonant(join->phone, join->f, RecordedVoice::From::Word);
        if (b == nullptr) return;
        r.soundB = &b->sound;
        r.tractB = &b->tract;
        r.gainB = b->gain;
        r.aFrom = join->from;
        r.aTo = join->to;
        r.bFrom = b->from;
        r.bTo = b->to;
    }
}

float Diction::mapToB(const Reader &r, float pos) {
    // Three stretches: before the part that matters, the part, and after it.
    const auto framesA = static_cast<float>(r.sound->frames), framesB = static_cast<float>(r.soundB->frames);
    const auto aFrom = static_cast<float>(r.aFrom), aTo = static_cast<float>(r.aTo);
    const auto bFrom = static_cast<float>(r.bFrom), bTo = static_cast<float>(r.bTo);
    float at;
    if (pos < aFrom) at = aFrom > 0.0f ? pos / aFrom * bFrom : bFrom;
    else if (pos < aTo) at = bFrom + (pos - aFrom) / std::max(1.0f, aTo - aFrom) * (bTo - bFrom);
    else at = bTo + (pos - aTo) / std::max(1.0f, framesA - aTo) * (framesB - bTo);
    return clampf(at, 0.0f, framesB - 1.0f);
}

void Diction::startReading(const Reader &r, float seconds) {
    if (reading.sound == nullptr) {
        reading = r;
        fade = 1.0f;
        return;
    }
    fading = reading;
    reading = r;
    fade = 0.0f;
    fadeStep = 1.0f / (std::max(0.005f, seconds) * sampleRate);
}

void Diction::advance(Reader &r) {
    if (r.sound == nullptr) return;
    if (!r.held) {
        // Once through, and on into what was sung after it while it fades.
        const bool inside = r.pos >= static_cast<float>(r.quietFrom) && r.pos < static_cast<float>(r.quietTo);
        r.pos = std::min(r.pos + (inside ? r.speed : 1.0f) * r.rate, static_cast<float>(r.sound->frames - 1));
        return;
    }
    r.pos += r.dir;
    if (r.pos >= static_cast<float>(r.to - 1)) r.dir = -1.0f;
    else if (r.pos <= static_cast<float>(r.from)) r.dir = 1.0f;
}

const audio::Epoch *Diction::epochOf(const Reader &r) {
    if (r.sound == nullptr) return nullptr;
    const int32_t idx = r.sound->epochAt(r.pos);
    return idx < 0 ? nullptr : &r.sound->epochs[static_cast<size_t>(idx)];
}

bool Diction::shapeGrain(const Reader &r, float pos, float weight, float period, float ratio, GrainShape &g) const {
    if (r.sound == nullptr) return false;
    const audio::Utterance &u = *r.sound;
    const int32_t idx = u.epochAt(pos);
    if (idx < 0) return false;
    const audio::Epoch &e = u.epochs[static_cast<size_t>(idx)];
    // Half a grain is the source's period, so a grain holds one glottal
    // pulse, read faster or slower to move the formants.
    const float half = clampf(e.period, 2.0f, 2000.0f);
    const auto n = static_cast<int32_t>(2.0f * half / ratio);
    if (n < 2 || n >= kAccum) return false;
    // Laid closer than half overlap as the pitch rises, the windows sum to
    // more, but the pulses they hold don't line up, so they don't add up in
    // full: the square root of the windows' correction keeps a scale on one
    // vowel within a few dB of its own note, where the full correction lost
    // five by a fifth up.
    float gain = std::sqrt(clampf(2.0f * period / static_cast<float>(n), 0.0f, 1.0f)) * weight * r.gain;
    if (r.quiet != 1.0f) {
        // Eased in and out over 10 ms either side, so turning it down doesn't click.
        const float ramp = 0.01f * sampleRate;
        const float at = static_cast<float>(e.at);
        const float inside = std::min(clampf((at - static_cast<float>(r.quietFrom) + ramp) / ramp, 0.0f, 1.0f),
                                      clampf((static_cast<float>(r.quietTo) + ramp - at) / ramp, 0.0f, 1.0f));
        gain *= 1.0f + (r.quiet - 1.0f) * inside;
    }
    const int32_t frames = u.frames;
    const float span = static_cast<float>(n - 1) * ratio;
    g.idx = idx;
    g.half = half;
    g.n = n;
    g.gain = gain;
    g.wStep = static_cast<float>(kWindowSize - 1) / static_cast<float>(n - 1);
    g.span = span;
    g.from = clampf(static_cast<float>(e.at) - half, 0.0f, std::max(0.0f, static_cast<float>(frames - 2) - span));
    return true;
}

void Diction::layGrain(const Reader &r, float weight, float period, float ratio) {
    GrainShape g;
    if (!shapeGrain(r, r.pos, weight, period, ratio, g)) return;
    const audio::Utterance &u = *r.sound;
    const audio::Epoch &e = u.epochs[static_cast<size_t>(g.idx)];
    const float half = g.half, gain = g.gain, wStep = g.wStep, span = g.span, from = g.from;
    const int32_t n = g.n, idx = g.idx, frames = u.frames;
    const float *window = hannTable();

    // The singer's breath. Each pulse is blended with the one either side,
    // lined up on their own pitch marks: the voice repeats from pulse to
    // pulse and stays, breath doesn't and averages away. On a phone's mic
    // an inch away it lifted the harmonics 4 to 10 dB clear of the noise
    // between them. The breath control keeps as much as it's turned up.
    const float side = e.voiced ? 0.25f * (1.0f - clampf(paramOf(Breath), 0.0f, 1.0f)) : 0.0f;
    float fromPrev = from, fromNext = from;
    auto neighbour = [&](int32_t j, float &start) {
        if (j < 0 || j >= static_cast<int32_t>(u.epochs.size())) return false;
        const audio::Epoch &o = u.epochs[static_cast<size_t>(j)];
        if (!o.voiced || std::fabs(o.period - e.period) > 0.15f * e.period) return false;
        start = static_cast<float>(o.at) - half;
        return start >= 0.0f && start + span < static_cast<float>(frames - 2);
    };
    const bool prev = side > 0.0f && neighbour(idx - 1, fromPrev);
    const bool next = side > 0.0f && neighbour(idx + 1, fromNext);
    auto at = [&](float sp) {
        const auto i0 = static_cast<int32_t>(sp);
        const float frac = sp - static_cast<float>(i0);
        return u.mono[static_cast<size_t>(i0)] * (1.0f - frac) + u.mono[static_cast<size_t>(i0 + 1)] * frac;
    };
    // A one-pole low-pass at about 8 kHz for a hiss measured brighter than
    // heard (RecordedVoice::Join::bright): a few dB off at 10 and above.
    const float soft = r.soften ? 1.0f - std::exp(-kTwoPi * 8000.0f / sampleRate) : 1.0f;
    float lowpassed = 0.0f;
    // The singer's source, laid the same way, for the built-in throat.
    const float *source = sourceGrains && r.tract != nullptr && static_cast<int32_t>(r.tract->source.size()) == frames
                              ? r.tract->source.data() : nullptr;
    for (int32_t k = 0; k < n; ++k) {
        const float step = static_cast<float>(k) * ratio;
        const float sp = from + step;
        if (static_cast<int32_t>(sp) + 1 >= frames) break;
        float x = at(sp);
        if (r.soften) x = lowpassed += soft * (x - lowpassed);
        if (side > 0.0f) {
            const float xp = prev ? at(fromPrev + step) : x;
            const float xn = next ? at(fromNext + step) : x;
            x = (1.0f - 2.0f * side) * x + side * (xp + xn);
        }
        acc[static_cast<size_t>((accHead + k) & (kAccum - 1))] += x * window[static_cast<int32_t>(static_cast<float>(k) * wStep)] * gain;
        if (source != nullptr) {
            const auto i0 = static_cast<int32_t>(sp);
            const float frac = sp - static_cast<float>(i0);
            const float y = source[i0] * (1.0f - frac) + source[i0 + 1] * frac;
            accSource[static_cast<size_t>((accHead + k) & (kAccum - 1))] += y * window[static_cast<int32_t>(static_cast<float>(k) * wStep)] * gain;
        }
    }
}

void Diction::startGrain(Clock &c, const Reader &r, float weight, float period, float ratio) {
    // Another singer reads a held vowel a little further on, back and forth
    // inside it as the reader goes, so its pulse is its own.
    float pos = r.pos;
    if (c.shift != 0.0f && r.held && r.to - 1 > r.from) {
        const auto len = static_cast<float>(r.to - 1 - r.from);
        float q = std::fmod(pos - static_cast<float>(r.from) + c.shift, 2.0f * len);
        if (q > len) q = 2.0f * len - q;
        pos = static_cast<float>(r.from) + q;
    }
    GrainShape g;
    if (!shapeGrain(r, pos, weight, period, ratio, g)) return;
    // Full, the oldest goes: it's the one nearly finished.
    if (c.grainCount == kGrains) {
        for (int32_t j = 1; j < kGrains; ++j) c.grains[j - 1] = c.grains[j];
        --c.grainCount;
    }
    Grain &out = c.grains[c.grainCount++];
    out.sound = r.sound;
    out.from = g.from;
    out.ratio = ratio;
    out.k = 0;
    out.n = g.n;
    out.wStep = g.wStep;
    out.gain = g.gain;
}

float Diction::nextOf(Clock &c) {
    // One sample of each of its grains, so a singer's work is spread over
    // its period rather than done a grain at a time: laid whole, a dozen
    // singers' grains landing in one block took four times the average.
    const float *window = hannTable();
    float v = 0.0f;
    for (int32_t j = 0; j < c.grainCount;) {
        Grain &g = c.grains[j];
        const float sp = g.from + static_cast<float>(g.k) * g.ratio;
        const auto i0 = static_cast<int32_t>(sp);
        if (g.k >= g.n || i0 + 1 >= g.sound->frames) {
            g = c.grains[--c.grainCount];
            continue;
        }
        const float frac = sp - static_cast<float>(i0);
        const float x = g.sound->mono[static_cast<size_t>(i0)] * (1.0f - frac) + g.sound->mono[static_cast<size_t>(i0 + 1)] * frac;
        v += x * window[static_cast<int32_t>(static_cast<float>(g.k) * g.wStep)] * g.gain;
        ++g.k;
        ++j;
    }
    return v;
}

// --- words into steps ---------------------------------------------------------

int32_t Diction::planSyllable(const uint8_t *p, int32_t count, Step *out, int32_t capacity) const {
    const float stretch = 0.5f + clampf(targetOf(Consonants), 0.0f, 1.0f);
    auto phoneAt = [&](int32_t i) -> const Phone & {
        const int32_t c = i >= 0 && i < count ? p[i] : 0;
        return phones[c < phoneCount ? c : 0];
    };
    auto named = [&](int32_t i, const char *name) { return i >= 0 && i < count && std::strcmp(phoneAt(i).name, name) == 0; };

    // The one sound held for as long as the note: the last vowel, or with no
    // vowel the last sound that can be held.
    int32_t hold = -1;
    for (int32_t i = count - 1; i >= 0 && hold < 0; --i) if (isVowel(phoneAt(i).kind)) hold = i;
    for (int32_t i = count - 1; i >= 0 && hold < 0; --i) if (holdable(phoneAt(i).kind)) hold = i;

    // Where the formants are going once past [i]: the next vowel, or the one before.
    auto vowelNear = [&](int32_t i, float f[3]) {
        for (int32_t j = i + 1; j < count; ++j) {
            if (isVowel(phoneAt(j).kind)) { std::memcpy(f, phoneAt(j).f, sizeof(float) * 3); return true; }
        }
        for (int32_t j = i - 1; j >= 0; --j) {
            if (isVowel(phoneAt(j).kind)) { std::memcpy(f, phoneAt(j).to, sizeof(float) * 3); return true; }
        }
        std::memcpy(f, phones[0].f, sizeof(float) * 3);
        return false;
    };

    int32_t n = 0;
    auto add = [&](const float f[3], uint8_t code, float voice, float air, float hiss, float nose, float length,
                   float glide, float edge) {
        if (n >= capacity) return;
        Step &s = out[n++];
        std::memcpy(s.f, f, sizeof(s.f));
        s.phone = code;
        s.voice = voice;
        s.air = air;
        s.hiss = hiss;
        s.nasal = nose;
        s.length = length;
        s.glide = glide;
        s.edge = edge;
        s.knob = false;
    };

    // How fast the formants move into the next sound, and the sources fade:
    // set by the sound before it.
    float glideIn = 0.05f, edgeIn = 0.02f;
    for (int32_t i = 0; i < count; ++i) {
        const Phone &ph = phoneAt(i);
        const auto code = static_cast<uint8_t>(p[i]);
        const bool held = i == hold;
        const bool coda = hold >= 0 && i > hold;
        const float length = held ? -1.0f : ph.length * stretch;
        switch (ph.kind) {
        case Kind::Vowel:
        case Kind::Diphthong: {
            // Before an R, caught's vowel is sore's in every accent.
            const bool beforeR = named(i + 1, "R") && (named(i, "AO") || named(i, "OC"));
            const float *f = beforeR ? phones[phoneCode("OR", 2)].f : ph.f;
            if (ph.kind == Kind::Vowel) {
                add(f, code, 1.0f, 0, 0, 0, held ? -1.0f : ph.length, glideIn, edgeIn);
            } else {
                // A diphthong sits on its first vowel and moves to the second at
                // its end: held, it moves when the note is let go.
                add(f, code, 1.0f, 0, 0, 0, held ? -1.0f : 0.09f, glideIn, edgeIn);
                add(ph.to, code, 1.0f, 0, 0, 0, held ? 0.1f : 0.08f, 0.1f, 0.02f);
            }
            glideIn = 0.05f;
            edgeIn = 0.02f;
            break;
        }
        case Kind::Stop: {
            float f[3];
            std::memcpy(f, ph.f, sizeof(f));
            float next[3];
            const bool vowelNext = vowelNear(i, next) && i + 1 < count && isVowel(phoneAt(i + 1).kind);
            if (ph.place == Place::Velum) {
                // Where the tongue meets the palate depends on the vowel: K
                // before ee is further forward than before oo.
                f[1] = clampf(next[1] + 150.0f, 1300.0f, 2300.0f);
                f[2] = std::max(f[1] + 250.0f, 2200.0f);
            }
            // Closed. A voiced stop hums behind its closure.
            add(f, code, ph.voiced ? 0.1f : 0.0f, 0, 0, 0, ph.length * stretch, 0.04f, 0.012f);
            // The burst as it opens: a click of a few milliseconds, softer at
            // the end of a word.
            add(f, code, ph.voiced ? 0.15f : 0.0f, 0, ph.noise * (coda ? 0.6f : 1.0f), 0, 0.004f, 0.0f, 0.0005f);
            const Kind after = i + 1 < count ? phoneAt(i + 1).kind : Kind::Vowel;
            const bool sonorantNext = i + 1 < count && (after == Kind::Liquid || after == Kind::Glide);
            if (!ph.voiced && vowelNext && !named(i - 1, "S")) {
                // Breath before the vowel starts, with the formants already on
                // their way to it. Not after an S: spin's P has none.
                add(next, 0, 0, 0.15f, 0, 0, 0.035f * stretch, 0.05f, 0.003f);
                glideIn = 0.03f;
            } else if (!ph.voiced && sonorantNext && !named(i - 1, "S")) {
                // Before an L, R, W or Y the breath goes through that sound's
                // shape, as in twin and play: the burst runs on into the
                // voice instead of standing alone.
                add(phoneAt(i + 1).f, 0, 0, 0.15f, 0, 0, 0.025f * stretch, 0.03f, 0.003f);
                glideIn = 0.03f;
            } else if (!ph.voiced && coda) {
                add(f, 0, 0, 0.25f, 0, 0, 0.03f, 0.03f, 0.004f);
                glideIn = 0.045f;
            } else {
                glideIn = 0.045f;
            }
            edgeIn = 0.008f;
            break;
        }
        case Kind::Affricate:
            add(ph.f, code, ph.voiced ? 0.1f : 0.0f, 0, 0, 0, 0.04f * stretch, 0.04f, 0.012f);
            add(ph.f, code, ph.voiced ? 0.35f : 0.0f, 0, ph.noise, 0, held ? -1.0f : (ph.length - 0.04f) * stretch,
                0.02f, 0.004f);
            glideIn = 0.04f;
            edgeIn = 0.01f;
            break;
        case Kind::Fricative:
            add(ph.f, code, ph.voiced ? 0.4f : 0.0f, 0, ph.noise, 0, length, 0.04f, 0.015f);
            // F and TH hiss alike; it's the way the vowel moves away from
            // them that tells them apart, so the move is slow enough to hear.
            glideIn = ph.place == Place::Teeth || ph.place == Place::Lips ? 0.07f : 0.04f;
            // The voice comes in as the hiss goes, so the two overlap rather
            // than leaving a gap between them.
            edgeIn = kHissGone;
            break;
        case Kind::Aspirate: {
            // H is breath through the vowel's own shape.
            float next[3];
            vowelNear(i, next);
            add(next, code, 0, 0.6f, 0, 0, length, 0.02f, 0.015f);
            glideIn = 0.03f;
            edgeIn = 0.02f;
            break;
        }
        case Kind::Nasal:
            add(ph.f, code, 0.5f, 0, 0, 1.0f, length, 0.035f, 0.015f);
            glideIn = 0.035f;
            edgeIn = 0.015f;
            break;
        case Kind::Liquid: {
            // An L after the vowel is the dark one: the back of the tongue up.
            static const float kDarkL[3] = {450, 900, 2600};
            const bool dark = coda && named(i, "L");
            const bool l = named(i, "L");
            add(dark ? kDarkL : ph.f, code, 0.8f, 0, 0, 0, length, l ? 0.03f : 0.04f, 0.02f);
            // The tongue lets go of an L quickly; slower, it's a W.
            glideIn = l ? 0.03f : 0.06f;
            edgeIn = 0.02f;
            break;
        }
        case Kind::Glide:
            add(ph.f, code, 0.85f, 0, 0, 0, length, 0.04f, 0.02f);
            glideIn = 0.08f;
            edgeIn = 0.02f;
            break;
        case Kind::Flap:
            // The tongue taps the gum on its way past: a quick, clear dip in
            // the voice. Gentler, it didn't register.
            add(ph.f, code, 0.25f, 0, 0, 0, ph.length * stretch, 0.012f, 0.004f);
            glideIn = 0.025f;
            edgeIn = 0.005f;
            break;
        }
    }
    return n;
}

int32_t Diction::planRecorded(const uint8_t *p, int32_t count, Step *out, int32_t capacity) const {
    const float stretch = 0.5f + clampf(targetOf(Consonants), 0.0f, 1.0f);
    auto phoneAt = [&](int32_t i) -> const Phone & {
        const int32_t c = i >= 0 && i < count ? p[i] : 0;
        return phones[c < phoneCount ? c : 0];
    };
    int32_t hold = -1;
    for (int32_t i = count - 1; i >= 0 && hold < 0; --i) if (isVowel(phoneAt(i).kind)) hold = i;
    for (int32_t i = count - 1; i >= 0 && hold < 0; --i) if (holdable(phoneAt(i).kind)) hold = i;

    int32_t n = 0;
    auto add = [&](const float f[3], uint8_t code, float length, float glide) -> Step * {
        if (n >= capacity) return nullptr;
        Step &s = out[n++];
        s = Step{};
        std::memcpy(s.f, f, sizeof(s.f));
        s.phone = code;
        s.voice = 1.0f;
        s.length = length;
        s.glide = glide;
        s.edge = 0.01f;
        return &s;
    };
    for (int32_t i = 0; i < count; ++i) {
        const Phone &ph = phoneAt(i);
        const auto code = static_cast<uint8_t>(p[i]);
        const bool held = i == hold;
        if (ph.kind == Kind::Vowel) {
            add(ph.f, code, held ? -1.0f : ph.length, 0.04f);
            continue;
        }
        if (ph.kind == Kind::Diphthong) {
            if (const RecordedVoice::Unit *d = voice->diphthong(code)) {
                // As the singer sang it: the first vowel held, then their own
                // move to the second, at its own pace. Made from two vowels,
                // the prairie oh barely moved at all and row lost its w.
                Step *first = add(ph.f, code, held ? -1.0f : 0.09f, 0.04f);
                // On past where the move is marked done (70% of the way) by as
                // much again, into the second vowel. A slow glide, sung all
                // the way through a take, is read faster so the whole fits.
                const int32_t end = std::min(d->sound.frames - 1, d->glideTo + (d->glideTo - d->glideFrom));
                const float sung = static_cast<float>(end - d->glideFrom) / sampleRate;
                const float move = clampf(sung, 0.06f, 0.25f);
                Step *second = add(ph.to, code, move, 0.02f);
                if (first == nullptr || second == nullptr) break;
                first->unit = d;
                second->unit = d;
                second->once = true;
                second->from = d->glideFrom;
                second->to = end;
                second->rate = std::max(1.0f, sung / move);
                continue;
            }
            add(ph.f, code, held ? -1.0f : 0.09f, 0.04f);
            add(ph.to, code, held ? 0.1f : 0.08f, 0.1f);
            continue;
        }
        // A consonant, from between the vowels most like the word's: the next
        // vowel for one before the vowel, the last for one after.
        const Phone *near = nullptr;
        if (hold < 0 || i < hold) {
            for (int32_t j = i + 1; j < count && near == nullptr; ++j) if (isVowel(phoneAt(j).kind)) near = &phoneAt(j);
        }
        for (int32_t j = i - 1; j >= 0 && near == nullptr; --j) if (isVowel(phoneAt(j).kind)) near = &phoneAt(j);
        const float *vowel = near != nullptr ? (i < hold ? near->f : near->to) : kKnobVowels[2];
        // Which take it's formed from, as its track says; the flap of butter
        // goes with the D.
        int32_t take = 0;
        const char *name = std::strcmp(ph.name, "DX") == 0 ? "D" : ph.name;
        for (int32_t k = 0; k < 24; ++k) {
            if (std::strcmp(kFromOrder[k], name) == 0) { take = steppedOf(From + k); break; }
        }
        const RecordedVoice::Join *join = voice->consonant(code, vowel, static_cast<RecordedVoice::From>(std::clamp(take, 0, 3)));
        if (join == nullptr) continue;
        // Some of the vowel either side, where it meets one: the way in and
        // out is the consonant too. Not where it meets another consonant, or
        // the cluster of street would have vowels in it.
        const float into = i > 0 && isVowel(phoneAt(i - 1).kind) ? kOutOfVowel : kIntoConsonant;
        const float outOf = i + 1 < count && isVowel(phoneAt(i + 1).kind) ? kIntoVowel : kIntoConsonant;
        const int32_t frames = join->sound.frames;
        const int32_t from = held ? join->from : std::max(0, join->from - static_cast<int32_t>(into * sampleRate));
        int32_t to = held ? join->to : std::min(frames, join->to + static_cast<int32_t>(outOf * sampleRate));
        // After an S (or F, SH, TH) a stop has no breath after its burst: st,
        // not st-h. Sung alone it has, and the breath made stream rough.
        const bool afterHiss = i > 0 && phoneAt(i - 1).kind == Kind::Fricative;
        if (!held && afterHiss && ph.kind == Kind::Stop && join->burst > join->from) {
            to = std::min(to, join->burst + static_cast<int32_t>(0.012f * sampleRate));
        }
        // A stop ending a word is its closure and burst: sung between two
        // vowels it went on into the next one, and a G after an ee-like vowel
        // opened into a y, so bag came out baj.
        const bool endsWord = hold >= 0 && i > hold && (i + 1 >= count || !isVowel(phoneAt(i + 1).kind));
        if (!held && endsWord && ph.kind == Kind::Stop && join->burst > join->from) {
            to = std::min(to, join->burst + static_cast<int32_t>(0.025f * sampleRate));
        }
        if (to - from < 2) continue;
        // Sung for recording, a consonant is slow and clear, two to four times
        // as long as in a song, and ran each word into the next note. Read
        // faster, down to about the built-in voice's length, and never slower
        // than it was sung. A stop gets half again, for its burst and breath.
        // A W, Y, L or R is a movement, and read five times faster a W's lips
        // were gone before they'd rounded: twinkle came out tyinkle. Those
        // get twice, an R three times (chosen by ear: shorter, it had an L
        // in it), and a nasal twice. A hiss or an H gets twice too, and a
        // voiced one, whose buzz hides part of its hiss, three times: at the
        // built-in length a recorded SH, V or TH kept 20 to 30 ms of its hiss,
        // against the 60 to 125 it was sung with.
        float longer = 1.0f;
        if (ph.kind == Kind::Stop || ph.kind == Kind::Affricate) longer = 1.5f;
        else if (std::strcmp(ph.name, "R") == 0) longer = 3.0f;
        else if (ph.kind == Kind::Glide || ph.kind == Kind::Liquid || ph.kind == Kind::Nasal) longer = 2.0f;
        else if (ph.kind == Kind::Fricative) longer = ph.voiced ? 3.0f : 2.0f;
        else if (ph.kind == Kind::Aspirate) longer = 2.0f;
        const float want = ph.length * stretch * longer * sampleRate;
        const auto sung = static_cast<float>(std::min(join->to, to) - join->from);
        // The last sound of the words, if it's one a singer can hold (an R,
        // L, M, N), isn't hurried: rushed, the R ending "are" never got its
        // third formant down before it faded.
        const bool last = i + 1 == count && hold >= 0 && i > hold;
        const bool sonorant = ph.kind == Kind::Liquid || ph.kind == Kind::Nasal;
        const float speed = held || (last && sonorant) ? 1.0f : std::max(1.0f, sung / std::max(1.0f, want));
        const float played = static_cast<float>(to - from) - sung + sung / speed;
        Step *s = add(vowel, code, held ? -1.0f : played / sampleRate, 0.02f);
        if (s == nullptr) break;
        s->join = join;
        s->from = from;
        s->to = to;
        s->speed = speed;
    }
    return n;
}

void Diction::beginSteps(const Step *next, int32_t count, bool fromSilence) {
    Step merged[kMaxSteps];
    int32_t m = 0;
    if (!fromSilence && stepIndex < stepCount) {
        // What the last note still had to say: the end of a held word, or the
        // rest of one already ending.
        const Step &now = steps[stepIndex];
        int32_t from = stepIndex + 1;
        if (now.length >= 0.0f && !wordsDone) {
            merged[m] = now;
            merged[m].length = std::max(0.005f, now.length - stepTime);
            ++m;
        }
        for (int32_t i = from; i < stepCount && m < kMaxSteps; ++i) merged[m++] = steps[i];
    }
    // The same held sound ending one note and starting the next is one
    // sound, as in twinkle little: sung twice, the L between went through W.
    if (m > 0 && count > 0 && next[0].phone != 0 && next[0].phone == merged[m - 1].phone &&
        next[0].phone < phoneCount) {
        const Kind k = phones[next[0].phone].kind;
        if (k == Kind::Liquid || k == Kind::Nasal || k == Kind::Fricative || k == Kind::Glide) --m;
    }
    newestVowel = -1;
    for (int32_t i = 0; i < count && m < kMaxSteps; ++i) {
        if (newestVowel < 0 && beginsNote(next[i])) newestVowel = m;
        merged[m++] = next[i];
    }
    std::memcpy(steps, merged, sizeof(Step) * static_cast<size_t>(m));
    stepCount = m;
    letGo = false;
    wordsDone = m == 0;
    if (m == 0) return;
    if (fromSilence) {
        // From silence the throat is already in place, and a closed stop has
        // nothing to be quiet after.
        Step &first = steps[0];
        if (first.voice <= 0.1f && first.air == 0.0f && first.hiss == 0.0f && first.length > 0.0f) {
            first.length = std::min(first.length, 0.015f);
        }
        float f[3];
        if (first.knob) knobFormants(f); else std::memcpy(f, first.f, sizeof(f));
        std::memcpy(formants, f, sizeof(f));
        nasal = first.nasal;
    }
    enterStep(0);
    // From silence the levels start where they should be, not easing over
    // from the last note's.
    snapLevels = fromSilence;
}

void Diction::enterStep(int32_t index) {
    stepIndex = index;
    stepTime = 0.0f;
    const Step &s = steps[index];
    std::memcpy(glideStart, formants, sizeof(glideStart));
    nasalStart = nasal;
    const float target[3] = {s.voice, s.air, s.hiss};
    const float samples = std::max(1.0f, s.edge * sampleRate);
    for (int32_t k = 0; k < 3; ++k) levelStep[k] = (target[k] - level[k]) / samples;
    // Hiss stops as the narrowing opens, whatever comes next. Faded over the
    // next sound's time, a T's burst lasted as long as the W after it.
    if (s.hiss < level[2]) levelStep[2] = (s.hiss - level[2]) / std::max(1.0f, std::min(s.edge, kHissGone) * sampleRate);
    if (s.hiss > 0.0f && s.phone > 0 && s.phone < phoneCount) throat.setHiss(phones[s.phone].band);
    for (float &g : gainsFor) g = -1.0f; // work them out for where this step is going
    if (voice != nullptr && s.once && s.unit != nullptr) {
        Reader r;
        r.owner = &s;
        r.sound = &s.unit->sound;
        r.tract = &s.unit->tract;
        pairWithB(r, s.unit, nullptr);
        r.gain = s.unit->gain;
        r.from = s.from;
        r.to = s.to;
        r.held = false;
        r.pos = static_cast<float>(s.from);
        r.rate = s.rate;
        startReading(r, 0.02f);
    }
    if (voice != nullptr && s.join != nullptr) {
        Reader r;
        r.owner = &s;
        r.sound = &s.join->sound;
        r.tract = &s.join->tract;
        pairWithB(r, nullptr, s.join);
        r.gain = s.join->gain;
        r.from = s.from;
        r.to = s.to;
        r.held = s.length < 0.0f;
        r.pos = static_cast<float>(s.from);
        r.quietFrom = s.join->from;
        r.quietTo = s.join->to;
        r.quiet = s.join->consonantGain * std::pow(10.0f, paramOf(ConsonantLevel) / 20.0f);
        r.speed = s.speed;
        r.rate = s.rate;
        r.soften = s.join->bright;
        startReading(r, kJoinFade * std::min(1.0f, 1.0f / s.rate));
    }
}

void Diction::knobFormants(float f[3]) const {
    const float v = clampf(paramOf(Vowel), 0.0f, 4.0f);
    const int32_t lower = std::min(static_cast<int32_t>(v), 3);
    const float t = v - static_cast<float>(lower);
    for (int32_t k = 0; k < 3; ++k) f[k] = kKnobVowels[lower][k] + (kKnobVowels[lower + 1][k] - kKnobVowels[lower][k]) * t;
}

void Diction::currentTargets(float f[3]) const {
    const Step &s = steps[stepIndex];
    if (s.knob) knobFormants(f); else std::memcpy(f, s.f, sizeof(float) * 3);
}

void Diction::control() {
    const float dt = static_cast<float>(kControl) / sampleRate;
    if (anyClocks || steppedOf(Singers) > 1 || harmonyCount > 0) assignClocks();
    if (stepCount == 0) return;

    // --- through the words ----------------------------------------------------
    // The first call for a step only sets it up; time moves from the next.
    if (!snapLevels) stepTime += dt;
    const Step &now = steps[stepIndex];
    const bool done = now.length < 0.0f ? letGo && stepTime >= 0.04f : stepTime >= now.length;
    if (done && !wordsDone) {
        if (stepIndex + 1 < stepCount) {
            enterStep(stepIndex + 1);
        } else {
            wordsDone = true;
            // The last sound has had its time. A voice dies away with the
            // release, but hiss and breath stop: left on, a final S hissed for
            // as long as the release.
            const float samples = kHissGone * sampleRate;
            levelStep[1] = -level[1] / samples;
            levelStep[2] = -level[2] / samples;
            steps[stepIndex].air = 0.0f;
            steps[stepIndex].hiss = 0.0f;
            // A recorded consonant goes on into the vowel sung after it: a
            // final T would come out as ta.
            if (steps[stepIndex].join != nullptr) {
                // Faded rather than stopped: cut in 8 ms, a final S ended
                // sharp. An R, L, M or N dies away slower still, as a voice does.
                const Kind k = phones[steps[stepIndex].phone < phoneCount ? steps[stepIndex].phone : 0].kind;
                const float gone = k == Kind::Liquid || k == Kind::Nasal ? 2.0f * kRecordedGone : kRecordedGone;
                levelStep[0] = -level[0] / (gone * sampleRate);
                steps[stepIndex].voice = 0.0f;
            }
        }
    }
    if (wordsDone && !gate) amp.release();

    // --- the formants on their way ------------------------------------------------
    const Step &s = steps[stepIndex];
    float target[3];
    currentTargets(target);
    if (voice != nullptr) chooseUnit(s, target);
    const float t = s.glide > 0.0f ? std::min(1.0f, stepTime / s.glide) : 1.0f;
    const float eased = t * t * (3.0f - 2.0f * t);
    for (int32_t k = 0; k < 3; ++k) formants[k] = glideStart[k] + (target[k] - glideStart[k]) * eased;
    nasal = nasalStart + (s.nasal - nasalStart) * eased;
    const float ratio = std::exp2(paramOf(Formant) / 12.0f) * tracked(Throat::kReferenceHz);
    const Shape shape = shapeOf(formants, nasal, ratio, sungHz);
    throat.setShape(shape);
    throat.setRing(ringFor(formants, shape));

    // The levels for where the formants are, worked out again only when that
    // moves: on the way from one sound to the next (every few periods, not
    // every one), the vowel control swept, or the formant control turned.
    // Worked out for where they were heading instead, a vowel came in loud
    // from a P, whose shape the same gain makes louder.
    const bool moving = t < 1.0f;
    // A new step, or a note from silence, works them out at once.
    if (moving && !snapLevels && gainsFor[0] >= 0.0f && ++sinceLevels < kLevelsEvery) return followLevels(s, dt);
    sinceLevels = 0;
    const float *at = moving ? formants : target;
    const float key[5] = {at[0], at[1], at[2], ratio, sungHz};
    bool moved = false;
    for (int32_t k = 0; k < 5; ++k) moved |= std::fabs(key[k] - gainsFor[k]) > 0.002f * std::fabs(key[k]);
    if (moved) {
        std::memcpy(gainsFor, key, sizeof(key));
        const Shape heading = shapeOf(at, moving ? nasal : s.nasal, ratio, sungHz);
        throat.levelsFor(heading, ringFor(at, heading), sungHz, voiceGainTarget, airGainTarget);
    }
    followLevels(s, dt);
}

void Diction::followLevels(const Step &s, float dt) {
    // Close behind: the levels are worked out for where the formants are, so
    // lagging them only lets a formant crossing a harmonic through.
    (void)s;
    const float follow = snapLevels ? 1.0f : 1.0f - std::exp(-dt / kLevelsFollow);
    voiceGain += (voiceGainTarget - voiceGain) * follow;
    airGain += (airGainTarget - airGain) * follow;
    snapLevels = false;
}

// --- notes ----------------------------------------------------------------------

void Diction::lyric(const uint8_t *p, int32_t count) {
    pendingCount = std::clamp(count, 0, static_cast<int32_t>(kMaxPhones));
    if (pendingCount > 0) std::memcpy(pending, p, static_cast<size_t>(pendingCount));
}

int32_t Diction::planWords(const uint8_t *p, int32_t count, Step *out) const {
    int32_t n = 0;
    if (count > 0) n = voice != nullptr ? planRecorded(p, count, out, kMaxSteps) : planSyllable(p, count, out, kMaxSteps);
    if (n == 0) {
        Step &s = out[n++];
        s = Step{};
        s.voice = 1.0f;
        s.length = -1.0f;
        s.glide = 0.05f;
        s.edge = 0.02f;
        s.knob = true;
    }
    return n;
}

bool Diction::beginsNote(const Step &s) const {
    return s.knob || s.length < 0.0f || isVowel(phones[s.phone < phoneCount ? s.phone : 0].kind);
}

float Diction::onsetOf(const Step *planned, int32_t count) const {
    float seconds = 0.0f;
    for (int32_t i = 0; i < count && !beginsNote(planned[i]); ++i) seconds += planned[i].length;
    return seconds;
}

bool Diction::keepsLength(const Step &s) const {
    const Kind k = phones[s.phone < phoneCount ? s.phone : 0].kind;
    return k == Kind::Liquid || k == Kind::Glide;
}

void Diction::squeeze(Step &s, float f) {
    if (s.length <= 0.0f) return;
    s.length *= f;
    s.glide *= f;
    s.edge *= f;
    s.rate /= f;
}

float Diction::stillToSay() const {
    if (!sounding || wordsDone || stepCount == 0) return 0.0f;
    float seconds = 0.0f;
    const Step &now = steps[stepIndex];
    if (now.length >= 0.0f) seconds += std::max(0.0f, now.length - stepTime);
    for (int32_t i = stepIndex + 1; i < stepCount; ++i) seconds += std::max(0.0f, steps[i].length);
    return seconds;
}

void Diction::wordsAhead(const uint8_t *p, int32_t count, uint8_t n, uint8_t vel, int32_t inFrames) {
    if (aheadCount == kAhead) return; // the oldest are the ones due first; this one comes on time
    Ahead &a = ahead[aheadCount++];
    a.count = std::clamp(count, 0, static_cast<int32_t>(kMaxPhones));
    std::memcpy(a.phones, p, static_cast<size_t>(a.count));
    a.note = n;
    a.velocity = vel;
    a.in = inFrames;
    Step planned[kMaxSteps];
    a.onset = onsetOf(planned, planWords(a.phones, a.count, planned));
}

bool Diction::readyForNext() const {
    // The newest word's vowel, not the last word's: its end can still be
    // sounding under the newest word's start.
    if (!sounding || wordsDone || stepCount == 0 || newestVowel < 0) return true;
    return stepIndex > newestVowel || (stepIndex == newestVowel && stepTime >= kVowelFirst);
}

void Diction::wordsDue(int32_t frames) {
    for (int32_t i = 0; i < earlyCount; ++i) earlies[i].wait -= frames;
    while (earlyCount > 0 && earlies[0].wait <= 0) {
        // Begun for a note that never came: the transport stopped, or another
        // clip was launched. The one being sung is let go.
        if (earlyCount == 1) {
            gate = false;
            letGo = true;
            amp.release();
        }
        for (int32_t i = 1; i < earlyCount; ++i) earlies[i - 1] = earlies[i];
        --earlyCount;
    }
    if (pitchIn > 0 && (pitchIn -= frames) <= 0) {
        // The last word's said: over to the new note's pitch.
        note = nextNote;
        glideFrom = pitch;
        glideDone = targetOf(Glide) > 0.0005f ? 0.0f : 1.0f;
    }
    // The next word waits for this one to sing a little of its vowel, so a
    // quick note's word is heard at all; after that it has the time it needs,
    // as a singer gives a quick note's vowel only a moment.
    if (aheadCount == 0 || earlyCount == kAhead || !readyForNext()) {
        for (int32_t i = 0; i < aheadCount; ++i) ahead[i].in -= frames;
        return;
    }
    Ahead &a = ahead[0];
    float still = sounding ? stillToSay() : 0.0f;
    const float lead = a.onset + still;
    if (static_cast<float>(a.in) <= lead * sampleRate) {
        Step planned[kMaxSteps];
        const int32_t count = planWords(a.phones, a.count, planned);
        pendingCount = 0;
        // Between quick notes there isn't time for all of it: the last word's
        // end and this one's start are said faster, as a singer does, down to
        // about a third.
        const float time = std::max(0.0f, static_cast<float>(a.in) / sampleRate);
        if (lead > time && lead > 0.0f) {
            // An R, L, W or Y keeps at least 120 ms: squeezed with the rest,
            // stream's R lost its low third formant and came out an L, and
            // kept whole, it made the vowel after it land 200 ms late. The
            // others take up the difference, as far as they can.
            constexpr float kShortestMovement = 0.12f;
            const float even = time / lead;
            auto kept = [&](const Step &s) {
                return s.length > 0.0f ? std::min(1.0f, std::max(even, kShortestMovement / s.length)) : 1.0f;
            };
            float movements = 0.0f, keptLength = 0.0f;
            for (int32_t i = 0; i < count && !beginsNote(planned[i]); ++i) {
                if (keepsLength(planned[i])) { movements += planned[i].length; keptLength += planned[i].length * kept(planned[i]); }
            }
            for (int32_t i = stepIndex + 1; i < stepCount; ++i) {
                if (keepsLength(steps[i]) && steps[i].length > 0.0f) { movements += steps[i].length; keptLength += steps[i].length * kept(steps[i]); }
            }
            const float rest = lead - movements;
            const float f = rest > 0.0f ? clampf((time - keptLength) / rest, 0.35f, 1.0f) : 1.0f;
            for (int32_t i = 0; i < count && !beginsNote(planned[i]); ++i) squeeze(planned[i], keepsLength(planned[i]) ? kept(planned[i]) : f);
            for (int32_t i = stepIndex + 1; i < stepCount; ++i) squeeze(steps[i], keepsLength(steps[i]) ? kept(steps[i]) : f);
            still = stillToSay();
        }
        const bool joined = gate && sounding;
        const uint8_t was = note;
        startPlanned(a.note, a.velocity, gate, planned, count);
        if (joined && still > 0.0f && was != a.note) {
            // Still at the last note's pitch while its word ends.
            nextNote = a.note;
            note = was;
            glideDone = 1.0f;
            pitchIn = static_cast<int32_t>(still * sampleRate);
        }
        earlies[earlyCount++] = Early{a.note, a.in + static_cast<int32_t>(0.1f * sampleRate)};
        for (int32_t i = 1; i < aheadCount; ++i) ahead[i - 1] = ahead[i];
        --aheadCount;
    }
    for (int32_t i = 0; i < aheadCount; ++i) ahead[i].in -= frames;
}

void Diction::startNote(uint8_t n, uint8_t vel, bool legato) {
    Step planned[kMaxSteps];
    const int32_t count = planWords(pending, pendingCount, planned);
    pendingCount = 0;
    startPlanned(n, vel, legato, planned, count);
}

void Diction::startPlanned(uint8_t n, uint8_t vel, bool legato, const Step *planned, int32_t count) {
    // With harmony, a new word while a key is still down leaves that key
    // singing as harmony: a chord, not a move from one note to the next.
    bool chord = false;
    if (harmonyOn() && sounding && gate && n != note) {
        for (int32_t i = 0; i < heldCount; ++i) chord |= held[i] == note;
        if (chord) addHarmony(note);
    }
    dropHarmony(n);
    note = n;
    sinceOnset = 0.0f;
    // From a note still sounding, the pitch glides over from where it is.
    // From silence there's nothing to glide from.
    glideFrom = pitch;
    glideDone = sounding && !chord && targetOf(Glide) > 0.0005f ? 0.0f : 1.0f;

    if (legato) {
        // Joined to the note before: the glide carries the pitch across and
        // the voice doesn't start again, but the words move on.
        gate = true;
        beginSteps(planned, count, false);
        return;
    }
    velocity = velocityGain(static_cast<float>(vel) / 127.0f, targetOf(VelocityAmount));
    // Arriving from a little below, more for a less steady voice.
    scoop = -0.5f * targetOf(Drift) - targetOf(Scoop);
    if (sounding) {
        amp.trigger(); // from where the release had got to, so no click
        beginSteps(planned, count, false);
    } else {
        amp.retrigger();
        // The throat takes its shape for this note's pitch from the start.
        sungHz = noteHz(targetNote());
        phase = 1.0f;
        previousFlow = 0.0f;
        throat.reset();
        for (float &l : level) l = 0.0f;
        untilControl = 0;
        beginSteps(planned, count, true);
    }
    gate = true;
    sounding = true;
}

void Diction::noteOn(uint8_t n, uint8_t vel) {
    int32_t begun = -1;
    for (int32_t i = 0; i < earlyCount && begun < 0; ++i) if (earlies[i].note == n) begun = i;
    if (begun >= 0) {
        // Begun early for its words: only the key is new.
        for (int32_t i = begun + 1; i < earlyCount; ++i) earlies[i - 1] = earlies[i];
        --earlyCount;
        pendingCount = 0;
        int32_t kept = 0;
        for (int32_t i = 0; i < heldCount; ++i) if (held[i] != n) held[kept++] = held[i];
        heldCount = std::min(kept, kHeld - 1);
        held[heldCount++] = n;
        gate = true;
        return;
    }
    // Words known ahead for this note that never got started: it sings them now, on time.
    if (aheadCount > 0 && ahead[0].note == n) {
        for (int32_t i = 1; i < aheadCount; ++i) ahead[i - 1] = ahead[i];
        --aheadCount;
    }
    // Remembered newest last. A key already held moves to the end.
    int32_t kept = 0;
    for (int32_t i = 0; i < heldCount; ++i) if (held[i] != n) held[kept++] = held[i];
    heldCount = kept;
    if (heldCount == kHeld) {
        for (int32_t i = 1; i < kHeld; ++i) held[i - 1] = held[i];
        --heldCount;
    }
    held[heldCount++] = n;
    // With harmony, a key with no words of its own while something's sung
    // joins it: the chord sings the words on every note.
    if (harmonyOn() && pendingCount == 0 && ((sounding && gate) || earlyCount > 0) && n != note) {
        addHarmony(n);
        return;
    }
    startNote(n, vel, gate);
}

void Diction::noteOff(uint8_t n) {
    int32_t kept = 0;
    for (int32_t i = 0; i < heldCount; ++i) if (held[i] != n) held[kept++] = held[i];
    heldCount = kept;
    for (int32_t i = 0; i < harmonyCount; ++i) {
        if (harmony[i] == n) { dropHarmony(n); return; }
    }
    // A note begun early can't end before it's begun: until it has, this is
    // an older note letting go under the new word.
    if (earlyCount > 0) return;
    if (!gate || n != note) return;
    if (heldCount > 0) {
        // Back to the last key still down, as a singer would, still on the
        // same word. With harmony, that key's own singer takes the lead.
        note = held[heldCount - 1];
        dropHarmony(note);
        glideFrom = pitch;
        glideDone = targetOf(Glide) > 0.0005f ? 0.0f : 1.0f;
    } else {
        // The word finishes: the end of a diphthong and the consonants after
        // the vowel, then the release.
        gate = false;
        letGo = true;
        if (wordsDone || stepCount == 0) amp.release();
    }
}

void Diction::allNotesOff() {
    harmonyCount = 0;
    aheadCount = 0;
    earlyCount = 0;
    pitchIn = 0;
    heldCount = 0;
    gate = false;
    letGo = true;
    amp.release();
}

void Diction::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel adds vibrato on top of the knob's.
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}

void Diction::pitchBend(int16_t value14) {
    bend = static_cast<float>(value14) / 8192.0f * static_cast<float>(steppedOf(BendRange));
}

void Diction::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }

float Diction::targetNote() const {
    return static_cast<float>(note) + static_cast<float>(steppedOf(Transpose)) +
           12.0f * static_cast<float>(steppedOf(Octave)) + bend;
}

float Diction::nextPeriod() {
    // A glide arrives on the note in the glide time and stops there, eased at
    // both ends. A curve that only approached it would leave the note flat or
    // sharp for as long as it lasted.
    const float want = targetNote();
    if (glideDone < 1.0f) {
        const float t = glideDone;
        const float eased = t * t * (3.0f - 2.0f * t);
        pitch = glideFrom + (want - glideFrom) * eased;
        glideDone += (sampleRate / noteHz(pitch)) / (std::max(0.001f, paramOf(Glide)) * sampleRate);
    } else {
        pitch = want;
    }
    sungHz = noteHz(pitch);
    // Vibrato, brought in over a third of a second once its delay has passed.
    const float delay = paramOf(VibratoDelay);
    const float fadeIn = clampf((sinceOnset - delay) / 0.3f, 0.0f, 1.0f);
    const float depth = (paramOf(Vibrato) + modWheel * 60.0f) * 0.01f * fadeIn;
    const float vibrato = depth * std::sin(vibratoPhase);
    // Ten cents of wander at most: enough to sound held by a person, and
    // still in tune.
    const float sung = pitch + vibrato + wander * paramOf(Drift) * 0.1f + scoop;
    wobble = sung - pitch;
    const float period = clampf(sampleRate / noteHz(sung), 8.0f, 2000.0f);

    // Move the slow things on by one period.
    const float seconds = period / sampleRate;
    vibratoPhase += kTwoPi * paramOf(VibratoRate) * seconds;
    if (vibratoPhase > kTwoPi) vibratoPhase -= kTwoPi;
    // A wider scoop takes longer to arrive.
    scoop *= std::exp(-seconds / (0.06f + 0.05f * std::fabs(paramOf(Scoop))));
    wander += (wanderTarget - wander) * (1.0f - std::exp(-seconds / 0.25f));
    wanderCountdown -= static_cast<int32_t>(period);
    if (wanderCountdown <= 0) {
        wanderTarget = bipolar(seed);
        wanderCountdown = static_cast<int32_t>(sampleRate * (0.3f + 0.3f * (bipolar(seed) + 1.0f)));
    }
    return period;
}

bool Diction::render(float *L, float *R, int32_t frames) {
    params_.tick();
    // The rack's buffer still holds its last block: written through, even
    // asleep. Left, it came round every block as a 750 Hz tone.
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    wordsDue(frames);
    if (!sounding) {
        pendingCount = 0; // words for a note that never came
        return true;      // asleep: nothing held and nothing ringing
    }

    const float breath = clampf(paramOf(Breath), 0.0f, 1.0f);
    amp.set(0.0f, paramOf(Attack), 0.001f, 1.0f, paramOf(Release), false);
    const float volume = paramOf(Volume);
    const float pan = paramOf(Pan);
    const float panL = std::cos((pan + 1.0f) * 0.25f * dsp::kPi);
    const float panR = std::sin((pan + 1.0f) * 0.25f * dsp::kPi);
    const float referencePeriod = sampleRate / Throat::kReferenceHz;
    // Up to 0.8: harmonics 19 dB clear of what's between them, and a pulse
    // down to a tenth in ten periods.
    const float clean = 0.8f * clampf(paramOf(Clean), 0.0f, 1.0f);
    const float combStep = 1.0f / (0.005f * sampleRate);
    const float whisper = clampf(paramOf(Whisper), 0.0f, 1.0f);
    const float follow = 1.0f - std::exp(-1.0f / (0.05f * sampleRate));
    // Effort: above about 1 kHz up to 9 dB up or down.
    const float effortTop = std::exp2(1.5f * clampf(paramOf(Effort), -1.0f, 1.0f));
    const float tiltCoef = 1.0f - std::exp(-kTwoPi * 1000.0f / sampleRate);
    auto tiltWith = [&](float x, float &low) {
        // Level, it passes as it is: low + (x - low) doesn't round back to x.
        if (effortTop == 1.0f) return low = x;
        low += tiltCoef * (x - low);
        return low + (x - low) * effortTop;
    };
    auto tilt = [&](float x) { return tiltWith(x, tiltLow); };
    const float crossSource = voice != nullptr ? clampf(paramOf(CrossSource), 0.0f, 1.0f) : 0.0f;
    const float crossThroat = voice != nullptr ? clampf(paramOf(CrossThroat), 0.0f, 1.0f) : 0.0f;
    const float morph = voice != nullptr && voiceB != nullptr ? clampf(paramOf(Morph), 0.0f, 1.0f) : 0.0f;
    // Talking: another track's sound in place of the folds (sidechainRack()).
    const float *talkKey = key_;
    const bool talking = talkKey != nullptr;
    const float talkToFolds = foldsRms / kTalkNominal;
    const bool crossing = crossSource > 0.0f || crossThroat > 0.0f || morph > 0.0f || (talking && voice != nullptr);
    float crossSung = 0.0f, throatShare = 0.0f;
    // The built-in throat's path needs the singer's source in grains, and
    // the folds' level is followed whenever either is wanted.
    sourceGrains = (crossThroat > 0.0f || morph > 0.0f) && crossSource < 1.0f;
    const float crossStep = 1.0f / (0.005f * sampleRate);
    const float slow = 1.0f - std::exp(-1.0f / (0.1f * sampleRate));
    const float latSmooth = 1.0f - std::exp(-1.0f / (0.004f * sampleRate));
    const float clockStep = 1.0f / (0.03f * sampleRate);
    const float togetherStep = 1.0f - std::exp(-1.0f / (0.02f * sampleRate));
    // Each clock on its way in or out; what they come to together.
    auto moveClock = [&](Clock &c, float &total) {
        c.gain = clampf(c.gain + (c.on ? clockStep : -clockStep), 0.0f, 1.0f);
        total += c.gain;
    };

    for (int32_t i = 0; i < frames; ++i) {
        if (--untilControl < 0) {
            control();
            untilControl = kControl - 1;
        }

        float glottal = 0.0f;
        float recorded = 0.0f;
        if (voice != nullptr) {
            // A recorded voice: one of the singer's own pulses a period.
            if (untilGrain <= 0.0f) {
                // A voiced part at the note's pitch; an unvoiced one (an S, the
                // breath of a P) copied at its own rate, since it has no pitch.
                const Reader &lead = fade >= 0.5f || fading.sound == nullptr ? reading : fading;
                const audio::Epoch *e = epochOf(lead);
                const bool unvoiced = e != nullptr && !e->voiced;
                float period = unvoiced ? clampf(e->period, 16.0f, 2000.0f) : nextPeriod();
                const float character = unvoiced ? 1.0f : pulseCharacter(period);
                float ratio = std::exp2(paramOf(Formant) / 12.0f);
                if (lead.sound != nullptr && lead.sound->rootHz > 0.0f) ratio *= tracked(lead.sound->rootHz);
                if (fade < 1.0f) layGrain(fading, (1.0f - fade) * character, period, ratio);
                layGrain(reading, fade * character, period, ratio);
                untilGrain += period;
                grainPeriod = period;
                grainHeld = lead.held && (e == nullptr || e->voiced);
                if (crossing && !unvoiced) {
                    voicedPeriod = period;
                    // The singer's throat at this mark, which the built-in folds sing through.
                    if (e != nullptr && lead.tract != nullptr && !lead.tract->gain.empty()) {
                        const auto idx = static_cast<size_t>(e - lead.sound->epochs.data());
                        if (idx < lead.tract->gain.size()) {
                            latTarget = &lead.tract->k[idx * kOrder];
                            latGainTarget = lead.tract->gain[idx] * lead.gain;
                            // Morphing: the throat between this mark's and the
                            // other voice's at the same place, blended as line
                            // spectral frequencies (RecordedVoice::Tract::lsf).
                            if (morph > 0.0f && lead.soundB != nullptr && lead.tractB != nullptr) {
                                const int32_t idxB = lead.soundB->epochAt(mapToB(lead, lead.pos));
                                if (idxB >= 0 && static_cast<size_t>(idxB) < lead.tractB->gain.size()) {
                                    const float *la = lead.tract->lsf.empty() ? nullptr : &lead.tract->lsf[idx * kOrder];
                                    const float *lb = lead.tractB->lsf.empty() ? nullptr : &lead.tractB->lsf[static_cast<size_t>(idxB) * kOrder];
                                    if (la != nullptr && lb != nullptr && la[0] >= 0.0f && lb[0] >= 0.0f) {
                                        float between[kOrder];
                                        for (int32_t k = 0; k < kOrder; ++k) between[k] = la[k] + morph * (lb[k] - la[k]);
                                        RecordedVoice::Tract::fromLsf(between, morphK);
                                    } else {
                                        // A mark without them: as log area ratios.
                                        const float *a = latTarget, *b = &lead.tractB->k[static_cast<size_t>(idxB) * kOrder];
                                        for (int32_t k = 0; k < kOrder; ++k) {
                                            auto area = [](float c) {
                                                const float x = clampf(c, -0.999f, 0.999f);
                                                return std::log((1.0f + x) / (1.0f - x));
                                            };
                                            morphK[k] = std::tanh(0.5f * ((1.0f - morph) * area(a[k]) + morph * area(b[k])));
                                        }
                                    }
                                    latTarget = morphK;
                                    latGainTarget += morph * (lead.tractB->gain[static_cast<size_t>(idxB)] * lead.gainB - latGainTarget);
                                }
                            }
                        }
                    }
                }
                crossWanted = !unvoiced;
            }
            untilGrain -= 1.0f;
            if (anyClocks) {
                // The other singers, from the same readers into the stereo
                // ring, each where it's late to. Quiet through an S: one
                // hiss, not a crowd's. Whispering, they fall silent.
                const Reader &lead = fade >= 0.5f || fading.sound == nullptr ? reading : fading;
                const audio::Epoch *e = nullptr;
                bool looked = false;
                float total = 0.0f;
                for (Clock &c : clocks) {
                    if (!c.on && c.gain <= 0.0f) continue;
                    moveClock(c, total);
                    if (c.untilGrain <= 0.0f) {
                        float period = clockPeriod(c);
                        const float character = pulseCharacter(period, c.odd, c.random);
                        if (!looked) { e = epochOf(lead); looked = true; }
                        if (e == nullptr || e->voiced) {
                            float ratio = std::exp2(paramOf(Formant) / 12.0f) * c.ratio;
                            if (lead.sound != nullptr && lead.sound->rootHz > 0.0f) ratio *= tracked(lead.sound->rootHz);
                            const float w = c.gain * character * (1.0f - whisper);
                            if (fade < 1.0f) startGrain(c, fading, (1.0f - fade) * w, period, ratio);
                            startGrain(c, reading, fade * w, period, ratio);
                        }
                        c.untilGrain += period;
                    }
                    c.untilGrain -= 1.0f;
                    // Into the ring where it's late to, in its own place.
                    const float v = nextOf(c);
                    const auto at = static_cast<size_t>((ringHead + c.delay) & (kRing - 1));
                    ringL[at] += v * c.panL;
                    ringR[at] += v * c.panR;
                }
                together += (1.0f / std::sqrt(1.0f + total) - together) * togetherStep;
            }
            recorded = acc[static_cast<size_t>(accHead)];
            acc[static_cast<size_t>(accHead)] = 0.0f;
            // Whisper: what doesn't repeat from pulse to pulse, the voice
            // taken away by a second difference a period apart, which has
            // nothing left of anything that repeats. Brought up towards the
            // level of what was sung.
            const float sung = recorded;
            float whispered = 0.0f;
            if (whisper > 0.0f) {
                const float p1 = ringAt(rawLine, combHead, grainPeriod, kAccum);
                const float p2 = ringAt(rawLine, combHead, 2.0f * grainPeriod, kAccum);
                const float r = (sung - 2.0f * p1 + p2) * 0.408f;
                loudIn += follow * (sung * sung - loudIn);
                loudOut += follow * (r * r - loudOut);
                whispered = r * std::min(8.0f, std::sqrt(loudIn / (loudOut + 1e-12f))) * 0.6f;
            }
            rawLine[static_cast<size_t>(combHead)] = sung;
            // Clean: what's sung, part mixed with itself a period ago. What
            // repeats from pulse to pulse adds up and stays; breath doesn't.
            // Each grain is a different pulse of the singer's, and the
            // difference between them is the breath heard, so it's taken
            // out here, at the pitch being sung. Only on a held vowel: a
            // consonant's hiss rides on its voice (a V, a Z), and the comb
            // rang on into a P's closure.
            const float wantComb = grainHeld ? clean : 0.0f;
            comb += clampf(wantComb - comb, -combStep, combStep);
            if (comb > 0.0f) {
                const float before = ringAt(combLine, combHead, grainPeriod, kAccum);
                recorded += comb * (before - recorded);
            }
            combLine[static_cast<size_t>(combHead)] = recorded;
            combHead = (combHead + 1) & (kAccum - 1);
            if (whisper > 0.0f) recorded += whisper * (whispered - recorded);
            if (crossing) {
                // Crossed with the built-in voice, while it's voiced: the
                // built-in folds, at the lead's pitch, through the singer's
                // throat; and the built-in throat, sung through by the
                // singer's source or the built-in folds.
                crossVoiced = clampf(crossVoiced + (crossWanted ? crossStep : -crossStep), 0.0f, 1.0f);
                // Talking, the other track is the source and the source knob
                // has nothing to choose.
                const float sE = talking ? 0.0f : crossSource * crossVoiced, tE = crossThroat * crossVoiced;
                // Equal power: the paths aren't in step, and half of each
                // added plainly came out 3 dB quiet.
                const float sIn = std::sin(0.5f * dsp::kPi * sE), sOut = std::cos(0.5f * dsp::kPi * sE);
                const float tIn = std::sin(0.5f * dsp::kPi * tE), tOut = std::cos(0.5f * dsp::kPi * tE);
                throatShare = tIn;
                const float sung = recorded;
                crossSung = sung;
                levelSung += slow * (sung * sung - levelSung);
                const float talk = talking ? talkKey[i] : 0.0f;
                // The built-in folds.
                float folds = 0.0f;
                if (voicedPeriod > 0.0f) {
                    if (phase >= 1.0f) {
                        phase -= 1.0f;
                        phaseStep = 1.0f / voicedPeriod;
                        pulseScale = voicedPeriod / referencePeriod;
                    }
                    const float f = throat.flow(phase);
                    folds = (f - previousFlow) * pulseScale;
                    previousFlow = f;
                    phase += phaseStep;
                    levelFolds += slow * (folds * folds - levelFolds);
                }
                // The singer's source, in grains beside the voice.
                const float own = accSource[static_cast<size_t>(accHead)];
                levelSource += slow * (own * own - levelSource);
                // Through the singer's throat, or one morphed toward the other
                // voice's: sung by the singer's own source, or with source up by
                // the built-in folds' pulse with two tilts taken off. The
                // throat measured keeps some of the singer's tilt, and the
                // folds' own tilt on top of it took an ee's second formant
                // down 7 dB; taken off, Dan's vowels came out within 3 dB a
                // band of his voice as it is.
                float mixed = sung;
                // Morphing, all of it comes this way (brought in over the first
                // tenth of the knob) and the source knob chooses what sings it;
                // not, it's the source knob's share.
                // Talking, all of what's voiced: the track through the throat.
                const float share = talking ? crossVoiced : morph > 0.0f ? std::min(1.0f, morph * 10.0f) * crossVoiced : sIn;
                if (share > 0.0f && tE < 1.0f && latTarget != nullptr) {
                    for (int32_t k = 0; k < kOrder; ++k) latK[k] += latSmooth * (latTarget[k] - latK[k]);
                    latGain += latSmooth * (latGainTarget - latGain);
                    const float even = folds - 1.8f * foldsBefore + 0.81f * foldsEarlier;
                    foldsEarlier = foldsBefore;
                    foldsBefore = folds;
                    levelEven += slow * (even * even - levelEven);
                    // The emphasis the analysis took off, put back on the folds;
                    // the singer's source has it already.
                    deemphasis = even / std::sqrt(levelEven + 1e-12f) * latGain + 0.97f * deemphasis;
                    float f = talking ? talk : morph > 0.0f ? own * sOut + deemphasis * sIn : deemphasis;
                    for (int32_t k = kOrder - 1; k >= 0; --k) {
                        f -= latK[k] * latB[k];
                        latB[k + 1] = latK[k] * f + latB[k];
                    }
                    latB[0] = f;
                    levelThroat += slow * (f * f - levelThroat);
                    float matched;
                    if (talking) {
                        // A filter like this one raises the power of an even
                        // input by 1 / prod(1 - k^2), so that's taken off, and
                        // the nominal synth comes out at the voice's level.
                        double gain = 1.0;
                        for (int32_t k = 0; k < kOrder; ++k) gain *= 1.0 - static_cast<double>(latK[k]) * latK[k];
                        matched = f * static_cast<float>(std::sqrt(std::max(gain, 1e-12))) * (Throat::kLevel / kTalkNominal);
                    } else {
                        matched = f * std::min(10.0f, std::sqrt(levelSung / (levelThroat + 1e-12f)));
                    }
                    mixed = sung * std::sqrt(std::max(0.0f, 1.0f - share * share)) + matched * share;
                }
                // Into the built-in throat, at the built-in folds' level.
                float into = 0.0f;
                if (tE > 0.0f) {
                    const float ownMatched = own * std::min(10.0f, std::sqrt(levelFolds / (levelSource + 1e-12f)));
                    // Brought to the recorded voice's level coming out, since the
                    // singer's source fills the throat unlike the built-in folds.
                    // Talking, the track at the folds' level, and the throat's
                    // own levels set for the folds: nothing to match afterwards.
                    into = talking ? tiltWith(talk * talkToFolds, tiltLowCross) * tIn
                                   : tiltWith(ownMatched * sOut + folds * sIn, tiltLowCross) * tIn * throatMatch;
                }
                recorded = tilt(mixed) * tOut;
                glottal = into;
            } else {
                recorded = tilt(recorded);
            }
            accSource[static_cast<size_t>(accHead)] = 0.0f;
            accHead = (accHead + 1) & (kAccum - 1);
            advance(reading);
            if (fade < 1.0f) {
                advance(fading);
                fade = std::min(1.0f, fade + fadeStep);
            }
        } else {
            // The folds: a pulse a period, a little stronger or weaker each time,
            // scaled so every pitch meets the throat at the same level.
            if (phase >= 1.0f) {
                phase -= 1.0f;
                float period = nextPeriod();
                const float character = pulseCharacter(period);
                phaseStep = 1.0f / period;
                strength = (1.0f + 0.03f * bipolar(noiseSeed)) * character;
                pulseScale = period / referencePeriod;
            }
            const float g = throat.flow(phase) * strength;
            // The mouth radiates the change in flow, not the flow itself.
            glottal = g - previousFlow;
            previousFlow = g;
            // A little breath while the folds are open.
            if (phase < 0.56f) glottal += 0.004f * bipolar(noiseSeed);
            if (anyClocks) {
                // The other singers: more folds into the one throat.
                float others = 0.0f, total = 0.0f;
                for (Clock &c : clocks) {
                    if (!c.on && c.gain <= 0.0f) continue;
                    moveClock(c, total);
                    if (c.phase >= 1.0f) {
                        c.phase -= 1.0f;
                        float period = clockPeriod(c);
                        const float character = pulseCharacter(period, c.odd, c.random);
                        c.phaseStep = 1.0f / period;
                        c.strength = (1.0f + 0.03f * bipolar(c.random)) * character;
                        c.pulseScale = period / referencePeriod;
                    }
                    const float f = throat.flow(c.phase) * c.strength;
                    others += (f - c.previousFlow) * c.pulseScale * c.gain * (1.0f - whisper);
                    c.previousFlow = f;
                    c.phase += c.phaseStep;
                }
                together += (1.0f / std::sqrt(1.0f + total) - together) * togetherStep;
                glottal = tilt((glottal * pulseScale + others) * together);
            } else {
                glottal = tilt(glottal * pulseScale);
            }
            phase += phaseStep;
            if (talking) {
                // Talking: the other track in place of the folds, brought to
                // their level, through the throat that follows the words.
                // The words' own hiss and breath still come from the throat.
                glottal = tiltWith(talkKey[i] * talkToFolds, tiltLowCross);
            }
        }

        // The sources on their way to the step's levels.
        const Step &s = steps[stepIndex];
        const float want[3] = {s.voice, s.air, s.hiss};
        for (int32_t k = 0; k < 3; ++k) {
            const float d = want[k] - level[k];
            level[k] = std::fabs(d) <= std::fabs(levelStep[k]) ? want[k] : level[k] + levelStep[k];
        }
        // A breathy voice gives part of its tone to the air. A recorded voice
        // has its own breath, which the grains keep or take away instead.
        const float airy = voice != nullptr ? 0.0f : breath;
        // The built-in voice whispers by giving its tone to the air; a
        // recorded one has done it above.
        const float hushed = voice != nullptr ? 0.0f : whisper;
        const float voiced = level[0] * (1.0f - 0.5f * airy) * (1.0f - hushed);
        const float air = level[1] + level[0] * (airy + hushed);
        const float fromThroat = throat.process(glottal * voiceGain * voiced,
                                                air > 0.0f ? bipolar(noiseSeed) * airGain * air : 0.0f,
                                                level[2] > 0.0f ? bipolar(noiseSeed) * level[2] : 0.0f);
        if (crossing && !talking && throatShare > 0.05f) {
            // The built-in throat's level for what goes in at full, against the recorded voice's.
            const float unit = fromThroat / (throatShare * throatMatch);
            levelThroatOut += slow * (unit * unit - levelThroatOut);
            levelSungOut += slow * (crossSung * crossSung * voiced * voiced - levelSungOut);
            throatMatch = clampf(std::sqrt(levelSungOut / (levelThroatOut + 1e-12f)), 0.1f, 10.0f);
        }
        const float sung = fromThroat + recorded * voiced;

        const float env = amp.next();
        const float out = sung * env * velocity * (1.0f + pressure * 0.3f) * volume;
        if (anyClocks && voice != nullptr) {
            // The recorded voice's other singers, out of their ring, at the
            // lead's level, and all of them brought to about one's. Each is
            // panned on its own, then the lot by the pan (1 at the centre).
            const float otherL = tiltWith(ringL[static_cast<size_t>(ringHead)], tiltLowL);
            const float otherR = tiltWith(ringR[static_cast<size_t>(ringHead)], tiltLowR);
            ringL[static_cast<size_t>(ringHead)] = ringR[static_cast<size_t>(ringHead)] = 0.0f;
            ringHead = (ringHead + 1) & (kRing - 1);
            const float level = voiced * env * velocity * (1.0f + pressure * 0.3f) * volume * together * 1.41421356f;
            L[i] += out * together * panL + otherL * level * panL;
            R[i] += out * together * panR + otherR * level * panR;
        } else {
            L[i] += out * panL;
            R[i] += out * panR;
        }
        if (!amp.active()) {
            // Done: sleep until the next note, with nothing left ringing.
            sounding = false;
            stepCount = 0;
            throat.reset();
            reading = fading = Reader{};
            fade = 1.0f;
            std::fill(acc.begin(), acc.end(), 0.0f);
            std::fill(combLine.begin(), combLine.end(), 0.0f);
            comb = 0.0f;
            for (Clock &c : clocks) c = Clock{};
            anyClocks = false;
            harmonyCount = 0;
            std::fill(ringL.begin(), ringL.end(), 0.0f);
            std::fill(ringR.begin(), ringR.end(), 0.0f);
            together = 1.0f;
            untilGrain = 0.0f;
            break;
        }
    }
    sinceOnset += static_cast<float>(frames) / sampleRate;
    pendingCount = 0;
    return true;
}

} // namespace acidulous::machine
