#include "Filament.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, used as its nominal
// level. Measured before volume 0.8, peak -8.6 dB. Set it too high and the
// drive knob acts like a volume knob.
constexpr float kNominal = 0.14f;
using namespace dsp;

namespace {
constexpr float kPiF = 3.14159265f;
// Intervals in semitones for the sympathetic bank, by tuning.
const int kSymIntervals[Filament::SymCount][Filament::kSympathetic] = {
    {-24, -12, 0, 12, 24, 36},   // octaves
    {-12, -5, 0, 7, 12, 19},     // fifths
    {0, 4, 7, 12, 16, 19},       // major
    {0, 3, 7, 12, 15, 19},       // minor
    {0, 12, 19, 24, 28, 31},     // harmonic series, near enough
    {-1, 0, 1, 11, 12, 13},      // unison, detuned: a course of six
};
} // namespace

Filament::Filament() { initParams(); }

const ParamDef *Filament::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static char names[Count][12];
    static bool built = false;
    if (!built) {
        auto put = [&](int32_t i, const char *n, float mn, float mx, float df, Curve c, int32_t steps,
                       const char *u) {
            std::snprintf(names[i], sizeof(names[i]), "%s", n);
            defs[i] = ParamDef{names[i], mn, mx, df, c, steps, u};
        };
        auto lin = [&](int32_t i, const char *n, float mn, float mx, float df, const char *u = "") {
            put(i, n, mn, mx, df, Curve::Linear, 0, u);
        };
        auto exp_ = [&](int32_t i, const char *n, float mn, float mx, float df, const char *u = "") {
            put(i, n, mn, mx, df, Curve::Exponential, 0, u);
        };
        auto step = [&](int32_t i, const char *n, int32_t c, float df) {
            put(i, n, 0.0f, static_cast<float>(c - 1), df, Curve::Stepped, c, "");
        };

        step(ExciterMode, "exciter", ExciterCount, 0.0f);
        lin(Position, "position", 0.02f, 0.5f, 0.22f);
        lin(MpeTimbre, "mpetimbre", 0.0f, 1.0f, 0.5f);
        lin(MpePressure, "mpepressure", 0.0f, 1.0f, 0.5f);
        lin(Hardness, "hardness", 0.0f, 1.0f, 0.4f);
        lin(Pressure, "pressure", 0.0f, 1.0f, 0.5f);
        lin(Speed, "speed", 0.0f, 1.0f, 0.4f);
        lin(Noise, "grit", 0.0f, 1.0f, 0.5f);
        exp_(ExcitLength, "length", 0.0005f, 0.2f, 0.006f, "s");
        lin(ExternalGain, "in gain", 0.0f, 4.0f, 1.0f);

        lin(Damping, "sustain", 0.0f, 1.0f, 0.8f);
        lin(DampingKey, "sustainkey", 0.0f, 1.0f, 0.5f);
        lin(Tone, "tone", 0.0f, 1.0f, 0.45f);
        lin(ToneKey, "tonekey", 0.0f, 1.0f, 0.4f);
        lin(Dispersion, "stiffness", 0.0f, 1.0f, 0.0f);
        put(DispersionStages, "stages", 0.0f, 4.0f, 2.0f, Curve::Stepped, 5, "");
        lin(Tension, "tension", 0.0f, 1.0f, 0.15f);

        lin(DamperPos, "damper at", 0.0f, 1.0f, 0.5f);
        lin(DamperPressure, "damper", 0.0f, 1.0f, 0.0f);
        lin(Rattle, "rattle", 0.0f, 1.0f, 0.0f);
        lin(RattleThreshold, "rattle at", 0.01f, 1.0f, 0.35f);

        lin(Detune, "detune", 0.0f, 50.0f, 3.0f, "c");
        lin(Spread, "spread", 0.0f, 1.0f, 0.35f);
        lin(Couple, "couple", 0.0f, 1.0f, 0.25f);

        step(SympatheticOn, "sympathy", 2, 0.0f);
        step(SympatheticTune, "symtune", SymCount, 0.0f);
        lin(SympatheticLevel, "symlevel", 0.0f, 1.0f, 0.35f);
        lin(SympatheticDamping, "symsustain", 0.0f, 1.0f, 0.9f);
        lin(SympatheticSpread, "symwide", 0.0f, 1.0f, 0.7f);

        step(BodyOn, "body", 2, 1.0f);
        lin(BodySize, "size", 0.0f, 1.0f, 0.5f);
        lin(BodyMix, "bodymix", 0.0f, 1.0f, 0.35f);
        lin(BodyDamp, "bodydamp", 0.0f, 1.0f, 0.5f);

        lin(Drive, "drive", 0.0f, 1.0f, 0.1f);
        // Goes up to 1.5 so quieter patches like Steel can reach the
        // standard level.
        lin(Volume, "volume", 0.0f, 1.5f, 0.8f);
        lin(Pan, "pan", -1.0f, 1.0f, 0.0f);
        lin(Dry, "exciter out", 0.0f, 1.0f, 0.0f);

        exp_(AmpAttack, "attack", 0.0005f, 0.5f, 0.001f, "s");
        exp_(AmpRelease, "damp time", 0.01f, 4.0f, 0.25f, "s");
        exp_(Eg1A, "eg1atk", 0.001f, 8.0f, 0.01f, "s");
        exp_(Eg1D, "eg1dec", 0.005f, 12.0f, 0.5f, "s");
        lin(Eg1S, "eg1sus", 0.0f, 1.0f, 0.5f);
        exp_(Eg1R, "eg1rel", 0.005f, 12.0f, 0.4f, "s");
        exp_(Eg2A, "eg2atk", 0.001f, 8.0f, 1.0f, "s");
        exp_(Eg2D, "eg2dec", 0.005f, 12.0f, 2.0f, "s");
        lin(Eg2S, "eg2sus", 0.0f, 1.0f, 1.0f);
        exp_(Eg2R, "eg2rel", 0.005f, 12.0f, 1.0f, "s");
        step(Lfo1Wave, "lfo1wave", LfoGen::WaveCount, 0.0f);
        exp_(Lfo1Rate, "lfo1rate", 0.01f, 20.0f, 0.4f, "Hz");
        step(Lfo1Sync, "lfo1sync", 6, 0.0f);
        lin(Lfo1Depth, "lfo1depth", 0.0f, 1.0f, 1.0f);
        step(Lfo2Wave, "lfo2wave", LfoGen::WaveCount, 1.0f);
        exp_(Lfo2Rate, "lfo2rate", 0.01f, 20.0f, 3.0f, "Hz");
        step(Lfo2Sync, "lfo2sync", 6, 0.0f);
        lin(Lfo2Depth, "lfo2depth", 0.0f, 1.0f, 1.0f);
        for (int m = 0; m < kMatrixSlots; ++m) {
            char n[12];
            const int base = MatrixBase + m * kMatrixParams;
            std::snprintf(n, sizeof(n), "m%d_src", m + 1);
            step(base + XSrc, n, SourceCount, 0.0f);
            std::snprintf(n, sizeof(n), "m%d_dst", m + 1);
            step(base + XDest, n, DestCount, 0.0f);
            std::snprintf(n, sizeof(n), "m%d_amt", m + 1);
            lin(base + XDepth, n, -1.0f, 1.0f, 0.0f);
        }
        step(VoiceMode, "voicemode", 3, 0.0f);
        exp_(Glide, "glide", 0.001f, 2.0f, 0.001f, "s");
        lin(BendRange, "bend", 0.0f, 12.0f, 2.0f);
        lin(Octave, "octave", -3.0f, 3.0f, 0.0f);
        lin(Transpose, "transpose", -12.0f, 12.0f, 0.0f);
        lin(Fine, "fine", -50.0f, 50.0f, 0.0f, "c");
        lin(VelocityAmount, "velocity", 0.0f, 1.0f, 1.0f);
        step(Release, "on release", 2, 1.0f); // ring on, or damp
        built = true;
    }
    count = Count;
    return defs;
}

int32_t Filament::steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }

void Filament::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.a.prepare(sampleRate);
        v.b.prepare(sampleRate);
        // Half the longest string, the furthest a pick can be from the
        // bridge.
        v.pick.assign(static_cast<size_t>(sampleRate / 36.0f) + 4, 0.0f);
        v.pickWrite = 0;
    }
    for (auto &s : sympathetic) s.prepare(sampleRate);
    for (auto &e : eg) e.setSampleRate(sampleRate);
    lastTuning = -1;
    reset();
}

void Filament::reset() {
    for (auto &v : voices) {
        v.used = false;
        v.gate = false;
        v.a.clear();
        v.b.clear();
        v.exciteLeft = 0;
        v.exciteDc = v.lastPick = 0.0f;
        std::fill(v.pick.begin(), v.pick.end(), 0.0f);
        v.pickWrite = 0;
        // Reset the bow phase too, or bowed and blown notes would start
        // where the last one left off and renders after a panic would
        // differ.
        v.bowPhase = 0.0f;
        v.damp = 0.0f;
        v.bend = 0.0f;
        v.pressure = v.timbre = -1.0f;
        v.prsGlide = 0.0f;
    }
    for (auto &s : sympathetic) s.clear();
    dampersUp = false;
    damperMix = 0.0f;
    // Retune from the next note instead of keeping the last one.
    lastRoot = 0.0f;
    lastTuning = -1;
    for (auto &l : lfo) l.reset(0.0f);
    lfoValue[0] = lfoValue[1] = 0.0f;
    for (auto &bq : body) bq.reset();
    for (auto &e : eg) e.reset();
    stringLevel = 0.0f;
    rngState = kRngSeed;
    quietBlocks = 0;
    asleep = false;
}

// The sympathetic bank follows the last note played, so it stays in the same
// key instead of being a fixed drone.
void Filament::retuneSympathetic(float rootHz) {
    const int32_t tuning = steppedOf(SympatheticTune);
    if (std::fabs(rootHz - lastRoot) < 0.5f && tuning == lastTuning) return;
    lastRoot = rootHz;
    lastTuning = tuning;
    const float spread = paramOf(SympatheticSpread);
    for (int i = 0; i < kSympathetic; ++i) {
        const float semis = static_cast<float>(kSymIntervals[tuning][i]);
        // Slightly detuned on purpose, so they shimmer.
        const float cents = (static_cast<float>(i) - 2.5f) * 2.0f * spread;
        sympatheticHz[i] = rootHz * std::pow(2.0f, (semis + cents * 0.01f) / 12.0f);
        sympathetic[i].setFrequency(sympatheticHz[i]);
    }
}

void Filament::noteOn(uint8_t note, uint8_t velocity) {
    // The two mod envelopes belong to the machine, like the LFOs. They start
    // on the first note of a phrase and release when the last note does. A
    // note added on top of a held one doesn't restart them, which would
    // cause a jump.
    bool held = false;
    for (const auto &cand : voices) if (cand.used && cand.gate) { held = true; break; }
    if (!held) for (auto &e : eg) e.retrigger();

    Voice *v = nullptr;
    for (auto &cand : voices) if (!cand.used) { v = &cand; break; }
    if (v == nullptr) {
        // Steal the quietest string instead of the oldest, since it'll be
        // missed least.
        float quietest = 1e9f;
        for (auto &cand : voices) {
            const float l = cand.a.level();
            if (l < quietest) { quietest = l; v = &cand; }
        }
    }
    v->used = true;
    v->gate = true;
    v->note = note;
    v->bend = 0.0f;
    v->pressure = v->timbre = -1.0f;
    v->prsGlide = 0.0f;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->key01 = clampf((static_cast<float>(note) - 24.0f) / 72.0f, 0.0f, 1.0f);
    v->target = noteHz(static_cast<float>(note));
    v->freq = v->target;
    v->damp = 0.0f;
    rngState = rngState * 1664525u + 1013904223u;
    v->pan = ((static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f);
    const float velAmt = targetOf(VelocityAmount);
    v->exciteGain = velocityGain(v->velocity, velAmt);
    const int32_t mode = steppedTargetOf(ExciterMode);
    const float lengthSeconds = mode == Bow || mode == Breath || mode == External
                                    ? 0.0f
                                    : targetOf(ExcitLength);
    // A pluck or pick displaces the whole string, so its excitation lasts at
    // least one full turn of the loop. A shorter burst would just go round as
    // a pulse train instead of sounding like a string.
    //
    // A hammer hits one point and leaves, so it isn't stretched to a turn.
    // Its contact time scales with the note instead (`length` is the time at
    // middle C), like a piano's lighter treble hammers, so high notes aren't
    // much quieter than low ones.
    const float turn = sampleRate / std::max(20.0f, v->target);
    const bool spread = mode == Pluck || mode == Pick;
    const float contact = mode == Hammer ? lengthSeconds * 261.63f / std::max(20.0f, v->target)
                                         : lengthSeconds;
    // A shorter contact carries less energy, so the gain goes up with pitch
    // to keep the level even across the range.
    if (mode == Hammer) {
        v->exciteGain *= clampf(std::sqrt(v->target / 261.63f), 0.35f, 3.0f);
    }
    v->exciteLeft = static_cast<int32_t>(spread ? std::max(contact * sampleRate, turn)
                                                : contact * sampleRate);
    retuneSympathetic(v->target);
}

void Filament::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            // On release the string is either damped or left ringing,
            // depending on the release setting.
            v.damp = steppedOf(Release) != 0 ? 1.0f : 0.0f;
        }
    }
    bool held = false;
    for (const auto &cand : voices) if (cand.used && cand.gate) { held = true; break; }
    if (!held) for (auto &e : eg) e.release();
}

void Filament::allNotesOff() {
    for (auto &v : voices) { v.gate = false; v.damp = 1.0f; }
    for (auto &e : eg) e.release();
}

void Filament::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Filament::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Filament::pitchBend(int16_t value14) {
    bendSemis = (static_cast<float>(value14) / 8192.0f) * paramOf(BendRange);
}

void Filament::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Filament::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Filament::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}
void Filament::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

float Filament::sourceValue(int32_t src, const Voice &v) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    // A finger's own pressure if it sent any, the channel's otherwise.
    case SrcPressure: return v.pressure >= 0.0f ? v.pressure : pressure;
    case SrcVelocity: return v.velocity;
    case SrcKeyTrack: return v.key01;
    case SrcRandom: return v.pan * 0.5f + 0.5f;
    case SrcEg1: return eg[0].value();
    case SrcEg2: return eg[1].value();
    case SrcLfo1: return lfoValue[0];
    case SrcLfo2: return lfoValue[1];
    case SrcStringLevel: return clampf(v.a.level() * 8.0f, 0.0f, 1.0f);
    default: return 0.0f;
    }
}

void Filament::applyMatrix(const Voice &v, float *dest) {
    for (int32_t d = 0; d < DestCount; ++d) dest[d] = 0.0f;
    for (int m = 0; m < kMatrixSlots; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        const int32_t src = steppedOf(base + XSrc);
        const int32_t dst = steppedOf(base + XDest);
        if (src == SrcOff || dst == DstOff) continue;
        dest[dst] += sourceValue(src, v) * paramOf(base + XDepth);
    }
}

bool Filament::render(float *L, float *R, int32_t frames) {
    params_.tick();
    const float dt = static_cast<float>(frames) / sampleRate;
    static const float kSyncBeats[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f};
    for (int i = 0; i < 2; ++i) {
        const int32_t sync = steppedOf(i == 0 ? Lfo1Sync : Lfo2Sync);
        const float free = paramOf(i == 0 ? Lfo1Rate : Lfo2Rate);
        const float hz = sync == 0 ? free : (bpm / 60.0f) / kSyncBeats[sync];
        lfoValue[i] = lfo[i].advance(steppedOf(i == 0 ? Lfo1Wave : Lfo2Wave), hz, dt, 0.0f, false) *
                      paramOf(i == 0 ? Lfo1Depth : Lfo2Depth);
    }
    eg[0].set(0.0f, paramOf(Eg1A), paramOf(Eg1D), paramOf(Eg1S), paramOf(Eg1R), false);
    eg[1].set(0.0f, paramOf(Eg2A), paramOf(Eg2D), paramOf(Eg2S), paramOf(Eg2R), false);
    // Only stepped below if the matrix uses one.
    bool egWanted = false;
    for (int m = 0; m < kMatrixSlots && !egWanted; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        if (steppedOf(base + XDest) == DstOff) continue;
        const int32_t src = steppedOf(base + XSrc);
        egWanted = src == SrcEg1 || src == SrcEg2;
    }

    // With no voices and the output under -120 dB for two blocks, skip the
    // body, sympathetic strings and drive, which would otherwise run on
    // silence. Their state is cleared on the way to sleep, so waking up is
    // the same as after a reset.
    bool anyVoice = false;
    for (const auto &v : voices) anyVoice = anyVoice || v.used;
    if (!anyVoice && quietBlocks >= 2) {
        if (!asleep) {
            for (auto &sym : sympathetic) sym.clear();
            for (auto &bq : body) bq.reset();
            asleep = true;
        }
        damperMix = dampersUp ? 1.0f : 0.0f;
        for (int32_t n = 0; n < frames; ++n) L[n] = R[n] = 0.0f;
        return true;
    }
    asleep = false;

    const int32_t mode = steppedOf(ExciterMode);
    const float position = paramOf(Position);
    const float hardness = paramOf(Hardness);
    const float bowPressure = paramOf(Pressure);
    const float bowSpeed = paramOf(Speed);
    const float grit = paramOf(Noise);
    const float externalGain = paramOf(ExternalGain);
    const float dispersion = paramOf(Dispersion);
    const int32_t stages = steppedOf(DispersionStages);
    const float tension = paramOf(Tension);
    const float detune = paramOf(Detune);
    const float spread = paramOf(Spread);
    const float couple = paramOf(Couple);
    const bool symOn = steppedOf(SympatheticOn) != 0;
    const float symLevel = paramOf(SympatheticLevel);
    // With the pedal down the bank rings nearly as long as it can.
    const float symDamping = dampersUp ? std::fmax(paramOf(SympatheticDamping), 0.95f)
                                       : paramOf(SympatheticDamping);
    const float damperCoeff = 1.0f - std::exp(-1.0f / (0.03f * sampleRate));
    const float damperTarget = dampersUp ? 1.0f : 0.0f;
    const bool bodyOn = steppedOf(BodyOn) != 0;
    const float bodyMix = paramOf(BodyMix);
    const float drive = paramOf(Drive);
    // Normalised on the nominal level, so turning drive up doesn't make it
    // quieter. Worked out once a block.
    const float driveK = 1.0f + drive * 8.0f;
    const float driveNorm = kNominal / std::tanh(kNominal * driveK);
    const float volume = paramOf(Volume);
    const float panBase = paramOf(Pan);
    const float dry = paramOf(Dry);
    // 30 Hz: below every note this machine plays and above the slow part of
    // the drive.
    const float exciteDcCoeff = 1.0f - std::exp(-2.0f * kPiF * 30.0f / sampleRate);
    const float pitchScale = std::pow(2.0f, (bendSemis + paramOf(Octave) * 12.0f + paramOf(Transpose) +
                                             paramOf(Fine) * 0.01f) / 12.0f);
    const float releaseCoeff = 1.0f - std::exp(-1.0f / std::fmax(1.0f, paramOf(AmpRelease) * sampleRate));
    const float rattle = paramOf(Rattle);
    const float rattleAt = paramOf(RattleThreshold);

    if (bodyOn) {
        // Four resonant modes for the body. Size moves them together, damp
        // widens them.
        static const float kModeHz[kBodyModes] = {110.0f, 220.0f, 400.0f, 780.0f};
        const float size = std::pow(2.0f, (0.5f - paramOf(BodySize)) * 2.2f);
        const float q = 1.0f + (1.0f - paramOf(BodyDamp)) * 9.0f;
        for (int i = 0; i < kBodyModes; ++i) {
            body[i].peak(clampf(kModeHz[i] * size, 40.0f, 6000.0f), 9.0f, q, sampleRate);
        }
    }
    for (int i = 0; i < kSympathetic; ++i) {
        // Sympathetic string damping is a time too, worked out from each
        // string's own pitch so high ones don't die much faster than low ones.
        const float symRing = 0.1f * std::pow(60.0f, symDamping);
        sympathetic[i].setDamping(
            std::fmin(0.99995f, std::exp(-1.0f / (std::fmax(20.0f, sympatheticHz[i]) * symRing))),
            0.2f + 0.6f * paramOf(Tone));
        sympathetic[i].setDispersion(dispersion * 0.7f, stages);
    }

    const InputBus &bus = InputBus::get();
    const float *in = bus.live() ? bus.block() : nullptr;
    float symFeed = 0.0f;
    // The sympathetic bank belongs to the machine, so it uses the matrix of
    // the last voice that sounded.
    float symMod = 0.0f;
    // The same for the body and the drive.
    float bodyMod = 0.0f, driveMod = 0.0f;
    float driveKNow = driveK, driveNormNow = driveNorm, driveSolved = drive;

    for (int32_t n = 0; n < frames; ++n) {
        if (egWanted) { eg[0].next(); eg[1].next(); }
        float mixL = 0.0f, mixR = 0.0f, exciterOut = 0.0f;
        float sumForSympathy = 0.0f;
        float loudest = 0.0f;

        for (auto &v : voices) {
            if (!v.used) continue;
            // The string's settings are updated every 16 samples, since the
            // matrix and coefficients only change as fast as the knobs.
            //
            // `v.damp` does change within a block, so its ramp is multiplied
            // by the stride to keep the same damping speed. The excitation
            // below stays per sample.
            if ((n & 15) == 0) {
            applyMatrix(v, v.mod);
            {
                // MPE pressure, applied on top of the matrix.
                const float prs = glidePressure(v.prsGlide, v.pressure, pressure) * paramOf(MpePressure);
                v.mod[DstPressure] += prs * 0.6f;
                v.mod[DstBrightness] += prs * 0.5f;
                v.mod[DstVolume] += prs * 0.4f;
            }

            // Damping, brightness and tuning per note. Higher notes can be
            // darker (tonekey) and ring shorter, like real short strings.
            const float keyTone = 1.0f - paramOf(ToneKey) * v.key01 * 0.5f;
            const float tone = clampf((paramOf(Tone) + v.mod[DstTone]) * keyTone, 0.02f, 1.0f);
            const float freq = v.freq * pitchScale * noteBendMul(v) * std::pow(2.0f, v.mod[DstPitch]);

            // How long the string rings is a time, not a loop gain. A string
            // goes round f times a second, so a fixed gain per turn would make
            // high notes die much faster than low ones.
            //
            // `sustain` goes from 0.05 s at 0 to a little over half a second
            // in the middle and 8 s at the top, and the loop gain is worked
            // out from it and the pitch. `sustainkey` shortens the top of the
            // keyboard a little, like real short strings.
            //
            // Releasing shortens the time instead of scaling the gain, so a
            // damped high string stops as fast as a low one.
            if (v.damp > 0.0f) v.damp = std::fmin(1.0f, v.damp + releaseCoeff * 64.0f);
            const float keyDamp = 1.0f - paramOf(DampingKey) * v.key01 * 0.35f;
            const float sustain = clampf(paramOf(Damping) + v.mod[DstDamping], 0.0f, 1.0f);
            const float ring = 0.05f * std::pow(160.0f, sustain) * keyDamp * (1.0f - 0.92f * v.damp);
            const float damped = std::fmin(
                0.99995f, std::exp(-1.0f / (std::fmax(20.0f, freq) * std::fmax(0.002f, ring))));

            v.freqNow = freq;
            v.a.setFrequency(freq);
            v.b.setFrequency(freq * std::pow(2.0f, (detune + v.mod[DstDetune] * 50.0f) / 1200.0f));
            v.a.setDamping(damped, tone);
            v.b.setDamping(damped, tone);
            v.a.setDispersion(clampf(dispersion + v.mod[DstDispersion], 0.0f, 1.0f), stages);
            v.b.setDispersion(clampf(dispersion + v.mod[DstDispersion], 0.0f, 1.0f), stages);
            v.a.setTension(clampf(tension + v.mod[DstTension], 0.0f, 1.0f));
            v.b.setTension(clampf(tension + v.mod[DstTension], 0.0f, 1.0f));
            const float damperP = clampf(paramOf(DamperPos) + v.mod[DstDamperPos], 0.0f, 1.0f);
            const float damperF = clampf(paramOf(DamperPressure) + v.mod[DstDamperPressure], 0.0f, 1.0f);
            v.a.setDamper(damperP, damperF);
            v.b.setDamper(damperP, damperF);
            // Pan, here since it only moves when the matrix does.
            const float pan = clampf(panBase + v.pan * spread + v.mod[DstPan], -1.0f, 1.0f);
            const float angle = (pan + 1.0f) * 0.25f * kPiF;
            v.panL = std::cos(angle);
            v.panR = std::sin(angle);
            }

            // Excitation. The matrix's `brightness` destination sets the grit
            // of a pluck, pick, bow or breath, and the hardness of a hammer.
            const float bright = v.mod[DstBrightness];
            const float gritNow = bright == 0.0f ? grit : clampf(grit + bright, 0.0f, 1.0f);
            const float hardNow = bright == 0.0f ? hardness : clampf(hardness + bright, 0.0f, 1.0f);
            float excite = 0.0f;
            rngState = rngState * 1664525u + 1013904223u;
            const float noise = (static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f;
            switch (mode) {
            case Pluck:
            case Pick:
                if (v.exciteLeft > 0) {
                    // A pick is a harder contact than a finger, so it gets a
                    // first difference, which tilts the noise up 6 dB per
                    // octave.
                    const float raw = noise * (0.3f + 0.7f * gritNow);
                    excite = (mode == Pick ? (raw - v.lastPick * 0.7f) * 1.25f : raw) * v.exciteGain;
                    v.lastPick = raw;
                    --v.exciteLeft;
                }
                break;
            case Hammer:
                if (v.exciteLeft > 0) {
                    // A hammer is a soft impulse, softened further by the
                    // hardness setting, like felt compressing.
                    const float t = 1.0f - static_cast<float>(v.exciteLeft) / std::fmax(1.0f, paramOf(ExcitLength) * sampleRate);
                    const float soft = std::sin(kPiF * t);
                    excite = soft * soft * (1.0f - hardNow * 0.6f) * v.exciteGain * 2.0f;
                    --v.exciteLeft;
                }
                break;
            case Bow: {
                if (!v.gate) break;
                // Bow friction: the bow grips while the bow and string move
                // together and lets go when the string slips. The curve must
                // fall away on both sides or the bow only adds energy and
                // the note runs away.
                const float p = clampf(bowPressure + v.mod[DstPressure], 0.0f, 1.0f);
                const float relative = bowSpeed * 0.5f - v.a.velocity();
                const float width = 0.08f + 0.5f * (1.0f - p);
                const float grip = relative / (width + relative * relative / width);
                excite = (grip * p * 0.12f + noise * gritNow * 0.01f) * v.exciteGain;
                break;
            }
            case Breath:
                if (!v.gate) break;
                // An air jet saturates as the resonator fills. Past a point,
                // blowing harder makes it overblow instead of getting louder,
                // which is intended.
                excite = (noise * (0.2f + 0.8f * gritNow) * 0.25f + 0.02f) *
                         clampf(bowPressure + v.mod[DstPressure], 0.0f, 1.0f) * v.exciteGain *
                         (1.0f - std::tanh(std::fabs(v.a.velocity()) * 1.6f) * 0.9f);
                break;
            case External:
                // The audio input excites the string.
                if (in != nullptr) {
                    excite = 0.5f * (in[static_cast<size_t>(n) * 2] + in[static_cast<size_t>(n) * 2 + 1]) *
                             externalGain * v.exciteGain;
                }
                break;
            default: break;
            }
            // Remove the slow part of the excitation. A bow's steady force or
            // a jet's steady pressure isn't a wave, and a hammer's blow is
            // never negative, so without this they'd push a DC offset into
            // the string. See also kDcBelow in Waveguide.h.
            v.exciteDc += (excite - v.exciteDc) * exciteDcCoeff;
            excite -= v.exciteDc;
            exciterOut += excite;

            // Pick position. A string plucked a fraction b along its length
            // gets harmonic n in proportion to sin(n.pi.b), so harmonics with
            // a node under the pick are missing. Near the bridge it sounds
            // thin, in the middle it's round. This is a comb on the
            // excitation, `1 - z^-bL` for a string L samples long, whose
            // nulls fall exactly there.
            //
            // MPE slide moves the pick along the string.
            const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
            const float pos = clampf(position + v.mod[DstPosition] +
                                         slide * paramOf(MpeTimbre) * 0.28f,
                                     0.02f, 0.5f);
            // Pluck, pick and hammer act at one point and leave, so they get
            // the comb (like a piano hammer placed to lose the 7th harmonic).
            // A bow or jet acts for the whole note, so it doesn't.
            const bool atAPoint = mode == Pluck || mode == Pick || mode == Hammer;
            if (atAPoint && !v.pick.empty()) {
                const auto size = static_cast<int32_t>(v.pick.size());
                const int32_t back = std::clamp(
                    static_cast<int32_t>(pos * sampleRate / std::max(20.0f, v.freqNow)), 1, size - 1);
                const int32_t at = (v.pickWrite - back + size) % size;
                const float earlier = v.pick[static_cast<size_t>(at)];
                v.pick[static_cast<size_t>(v.pickWrite)] = excite;
                v.pickWrite = (v.pickWrite + 1) % size;
                // Halved, since a comb peaks at twice its input.
                excite = (excite - earlier) * 0.5f;
            }
            const float a = v.a.step(excite);
            const float b = v.b.step(excite);
            const float coupled = (a + b) * 0.5f;
            // The two strings of a course share a bridge, which moves energy
            // between them without adding any. Each gets a fraction of the
            // difference, so what one gains the other loses. The fraction is
            // per turn, not per sample, so it's the same at every pitch and
            // can't run away.
            const float share = (b - a) * couple * 0.05f;
            v.a.exciteOverTurn(share);
            v.b.exciteOverTurn(-share);

            float voiceOut = coupled;
            const float rattleNow = v.mod[DstRattle] == 0.0f ? rattle : clampf(rattle + v.mod[DstRattle], 0.0f, 1.0f);
            if (rattleNow > 0.0f && std::fabs(voiceOut) > rattleAt) {
                // Something loose on the string that only buzzes when driven
                // hard.
                const float over = std::fabs(voiceOut) - rattleAt;
                voiceOut += (voiceOut > 0.0f ? -1.0f : 1.0f) * over * rattleNow * 1.6f;
            }
            // Per-voice volume modulation, scaled like the knob. The machine
            // volume is applied once at the end.
            if (v.mod[DstVolume] != 0.0f && volume > 0.0f)
                voiceOut *= clampf(volume + v.mod[DstVolume], 0.0f, 1.5f) / volume;
            sumForSympathy += voiceOut;
            loudest = std::fmax(loudest, v.a.level());

            mixL += voiceOut * v.panL * 1.4142f;
            mixR += voiceOut * v.panR * 1.4142f;
            symMod = v.mod[DstSympathetic];
            bodyMod = v.mod[DstBody];
            driveMod = v.mod[DstDrive];

            if (!v.gate && v.a.level() < 0.00005f && v.b.level() < 0.00005f && v.exciteLeft <= 0) {
                v.used = false;
            }
        }

        // The sympathetic bank is fed by all the voices. It's always on when
        // switched on, otherwise it follows the sustain pedal.
        damperMix += (damperTarget - damperMix) * damperCoeff;
        const float symGate = symOn ? 1.0f : damperMix;
        if (symGate > 1.0e-4f) {
            // Fed per turn too, so low strings don't build up and ring
            // forever.
            symFeed = sumForSympathy * clampf(symLevel + symMod, 0.0f, 1.0f) * 0.6f;
            float symOut = 0.0f;
            for (int i = 0; i < kSympathetic; ++i)
                symOut += sympathetic[i].step(symFeed * sympathetic[i].turnScale());
            symOut *= 0.2f * clampf(symLevel + symMod, 0.0f, 1.0f) * symGate;
            mixL += symOut;
            mixR += symOut * 0.85f;
        }

        float outL = mixL, outR = mixR;
        if (bodyOn) {
            float bl = 0.0f, br = 0.0f;
            for (int i = 0; i < kBodyModes; ++i) {
                const float s = body[i].process(i == 0 ? mixL : mixR);
                if (i % 2 == 0) bl += s; else br += s;
            }
            const float mixNow = bodyMod == 0.0f ? bodyMix : clampf(bodyMix + bodyMod, 0.0f, 1.0f);
            outL = mixL * (1.0f - mixNow) + bl * mixNow * 0.5f;
            outR = mixR * (1.0f - mixNow) + br * mixNow * 0.5f;
        }
        outL += exciterOut * dry;
        outR += exciterOut * dry;
        const float driveNow = driveMod == 0.0f ? drive : clampf(drive + driveMod, 0.0f, 1.0f);
        if (driveNow > 0.0f) {
            if (driveNow != driveSolved) { // only recomputed when modulation moves it
                driveSolved = driveNow;
                driveKNow = 1.0f + driveNow * 8.0f;
                driveNormNow = kNominal / std::tanh(kNominal * driveKNow);
            }
            outL = std::tanh(outL * driveKNow) * driveNormNow;
            outR = std::tanh(outR * driveKNow) * driveNormNow;
        }
        L[n] = outL * volume;
        R[n] = outR * volume;
        stringLevel = loudest;
    }
    if (anyVoice) {
        quietBlocks = 0;
    } else {
        float peak = 0.0f;
        for (int32_t n = 0; n < frames; ++n) peak = std::fmax(peak, std::fmax(std::fabs(L[n]), std::fabs(R[n])));
        quietBlocks = peak < 1e-6f ? quietBlocks + 1 : 0;
    }
    return true;
}

} // namespace acidulous::machine
