#include <engine/core/Settings.h>
#include "Trinity.h"
#include <engine/machine/Voices.h>
#include <engine/dsp/Lfo.h>

#include <cstdio>
#include <cstring>

namespace acidulous::machine {

using dsp::clampf;
using dsp::kTwoPi;
using dsp::mtof;
using dsp::WavetableBank;

namespace {

// PolyBLEP like dsp::Osc, but standalone because a density stack keeps its
// own phases and can't use the class.
inline float polyBlep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

inline float sawAt(float phase, float dt) { return (2.0f * phase - 1.0f) - polyBlep(phase, dt); }

inline float pulseAt(float phase, float dt, float width) {
    float v = phase < width ? 1.0f : -1.0f;
    v += polyBlep(phase, dt);
    float t = phase + 1.0f - width;
    if (t >= 1.0f) t -= 1.0f;
    return v - polyBlep(t, dt);
}

inline float triangleAt(float phase) { return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; }

// Detune spread for a density stack: symmetric, widest at the edges.
constexpr float kSpread[Trinity::kDensity] = {0.0f, -1.0f, 1.0f, -0.55f, 0.62f, -0.28f, 0.34f, 0.85f};

// LFO sync note values, in quarter notes.
// Free, then the tempo-locked LFOs' note values (dsp::Lfo).
constexpr int kSyncCount = 1 + dsp::Lfo::kRates;
inline float syncBeats(int sync) { return sync <= 0 ? 0.0f : dsp::Lfo::beatsOf(sync - 1); }

inline float rnd(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 16777216.0f;
}

} // namespace

Trinity::Trinity() { initParams(); }

// --- Parameters ------------------------------------------------------------------
//
// 178 of them, in regular blocks so the table can be generated: three
// oscillators, the mixer, two filters, six envelopes, three LFOs, twelve
// matrix slots and the voice section.

const ParamDef *Trinity::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static char names[Count][16];
    static bool built = false;
    if (!built) {
        auto put = [&](int32_t i, const char *fmt, int arg, float mn, float mx, float df, Curve c, int32_t steps,
                       const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), fmt, arg);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        for (int o = 0; o < kOscs; ++o) {
            const int32_t b = OscBase + o * OscParams;
            const int n = o + 1;
            put(b + OWave, "o%d_wave", n, 0.0f, WaveCount - 1.0f, 0.0f, Curve::Stepped, WaveCount, "");
            put(b + OPos, "o%d_pos", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OWarp, "o%d_warp", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OCoarse, "o%d_coarse", n, -24.0f, 24.0f, 0.0f, Curve::Stepped, 49, "st");
            put(b + OFine, "o%d_fine", n, -50.0f, 50.0f, o == 1 ? 7.0f : 0.0f, Curve::Linear, 0, "");
            put(b + OLevel, "o%d_level", n, 0.0f, 1.0f, o == 0 ? 0.8f : (o == 1 ? 0.5f : 0.0f), Curve::Linear, 0, "");
            put(b + ODensity, "o%d_density", n, 1.0f, 8.0f, 1.0f, Curve::Stepped, 8, "");
            put(b + ODetune, "o%d_detune", n, 0.0f, 1.0f, 0.35f, Curve::Linear, 0, "");
            put(b + OSync, "o%d_sync", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OHard, "o%d_hard", n, 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
            put(b + OPw, "o%d_pw", n, 0.02f, 0.98f, 0.5f, Curve::Linear, 0, "");
            put(b + ODrift, "o%d_drift", n, 0.0f, 1.0f, 0.15f, Curve::Linear, 0, "");
        }
        auto putN = [&](int32_t i, const char *name, float mn, float mx, float df, Curve c, int32_t steps,
                        const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), "%s", name);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        putN(MixBase + MRing12, "ring12", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(MixBase + MRing23, "ring23", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(MixBase + MFm21, "fm21", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(MixBase + MFm32, "fm32", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(MixBase + MNoise, "noise", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(MixBase + MNoiseColour, "noisecol", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
        for (int f = 0; f < 2; ++f) {
            const int32_t b = FilterBase + f * FilterParams;
            const int n = f + 1;
            put(b + FType, "f%d_type", n, 0.0f, dsp::MultiFilter::TypeCount - 1.0f, f == 0 ? 3.0f : 0.0f,
                Curve::Stepped, dsp::MultiFilter::TypeCount, "");
            put(b + FFreq, "f%d_freq", n, 20.0f, 20000.0f, f == 0 ? 3000.0f : 18000.0f, Curve::Exponential, 0, "Hz");
            put(b + FRes, "f%d_res", n, 0.0f, 1.0f, f == 0 ? 0.15f : 0.0f, Curve::Linear, 0, "");
            put(b + FDriveType, "f%d_drivetype", n, 0.0f, dsp::MultiFilter::DriveCount - 1.0f, 0.0f, Curve::Stepped,
                dsp::MultiFilter::DriveCount, "");
            put(b + FDrive, "f%d_drive", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + FEnv, "f%d_env", n, -1.0f, 1.0f, f == 0 ? 0.3f : 0.0f, Curve::Linear, 0, "");
            put(b + FKey, "f%d_key", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        }
        putN(RouteIndex, "route", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, "");
        putN(BalanceIndex, "balance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        static const char *const envNames[kEnvs] = {"a", "f", "e3", "e4", "e5", "e6"};
        for (int e = 0; e < kEnvs; ++e) {
            const int32_t b = EnvBase + e * EnvParams;
            auto putE = [&](int32_t off, const char *suffix, float mn, float mx, float df, Curve c, int32_t steps,
                            const char *unit) {
                std::snprintf(names[b + off], sizeof(names[0]), "%s_%s", envNames[e], suffix);
                defs[b + off] = {names[b + off], mn, mx, df, c, steps, unit};
            };
            putE(EDelay, "delay", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s");
            putE(EAttack, "attack", 0.001f, 10.0f, e == 0 ? 0.005f : 0.002f, Curve::Exponential, 0, "s");
            putE(EDecay, "decay", 0.002f, 15.0f, e == 0 ? 0.6f : 0.8f, Curve::Exponential, 0, "s");
            putE(ESustain, "sustain", 0.0f, 1.0f, e == 0 ? 0.7f : 0.2f, Curve::Linear, 0, "");
            putE(ERelease, "release", 0.002f, 15.0f, e == 0 ? 0.3f : 0.3f, Curve::Exponential, 0, "s");
            putE(ERepeat, "repeat", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        }
        for (int l = 0; l < kLfos; ++l) {
            const int32_t b = LfoBase + l * LfoParams;
            const int n = l + 1;
            put(b + LWave, "l%d_wave", n, 0.0f, dsp::LfoGen::WaveCount - 1.0f, 0.0f, Curve::Stepped,
                dsp::LfoGen::WaveCount, "");
            put(b + LRate, "l%d_rate", n, 0.01f, 40.0f, 4.0f, Curve::Exponential, 0, "Hz");
            put(b + LSync, "l%d_sync", n, 0.0f, kSyncCount - 1.0f, 0.0f, Curve::Stepped, kSyncCount, "");
            put(b + LDelay, "l%d_delay", n, 0.0f, 5.0f, 0.0f, Curve::Linear, 0, "s");
            put(b + LPhase, "l%d_phase", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + LSlew, "l%d_slew", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + LKeySync, "l%d_keysync", n, 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, "");
            put(b + LOneShot, "l%d_oneshot", n, 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        }
        for (int m = 0; m < kMatrixSlots; ++m) {
            const int32_t b = MatrixBase + m * MatrixParams;
            const int n = m + 1;
            put(b + XSrc, "m%02d_src", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XSrc2, "m%02d_src2", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XDest, "m%02d_dest", n, 0.0f, DestCount - 1.0f, 0.0f, Curve::Stepped, DestCount, "");
            put(b + XDepth, "m%02d_depth", n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        }
        putN(VoiceMode, "voicemode", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, "");
        putN(UnisonCount, "unison", 1.0f, 8.0f, 1.0f, Curve::Stepped, 8, "");
        putN(UnisonDetune, "unidetune", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, "");
        putN(UnisonSpread, "unispread", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(Glide, "glide", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s");
        putN(GlideMode, "glidemode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        putN(BendRange, "bend", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, "st");
        putN(Octave, "octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, "");
        putN(Transpose, "transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, "st");
        putN(Volume, "volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, "");
        putN(Pan, "pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(VelocityAmount, "velamt", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
        // How far a finger's slide opens the filters. 0 by default so patches
        // made before MPE sound the same.
        putN(MpeTimbre, "mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(MpePressure, "mpepressure", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        // How far the mod wheel (often a controller's tilt) opens the filters,
        // regardless of the matrix.
        putN(WheelFilter, "wheel", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        built = true;
    }
    count = Count;
    return defs;
}

// --- Lifecycle -------------------------------------------------------------------

void Trinity::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    invSampleRate = sampleRate > 0.0f ? 1.0f / sampleRate : 0.0f;
    bank = &WavetableBank::instance(); // builds the tables on this thread, once per process
    for (auto &v : voices) {
        for (auto &e : v.env) e.setSampleRate(sampleRate);
        for (auto &f : v.filter) f.setSampleRate(sampleRate);
    }
    reset();
}

void Trinity::reset() {
    for (auto &v : voices) {
        v.used = v.gate = v.cut = false;
        for (auto &e : v.env) e.kill();
        for (auto &f : v.filter) f.reset();
        for (auto &o : v.osc) o = OscState();
        for (auto &m : v.mod) m = 0.0f;
        v.rng = Voice::kSeed;
        v.bend = 0.0f;
        v.pressure = v.timbre = -1.0f;
        v.prsGlide = 0.0f;
    }
    modWheel = aftertouch = bend = 0.0f;
    noiseZ = 0.0f;
    noiseRng = kNoiseSeed;
}

Trinity::Voice *Trinity::allocate() {
    Voice *best = nullptr;
    for (auto &v : voices) if (!v.used) return &v;
    // Nothing free: take the oldest released voice, otherwise the oldest.
    for (auto &v : voices) {
        if (v.gate) continue;
        if (best == nullptr || v.age < best->age) best = &v;
    }
    if (best != nullptr) return best;
    for (auto &v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

void Trinity::startVoice(Voice &v, uint8_t note, uint8_t velocity, bool retrigger) {
    const float glideSeconds = targetOf(Glide);
    const bool gliding = glideSeconds > 0.001f && v.used && (stepOf(GlideMode) == 0 || v.gate);
    v.glideFrom = gliding ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = gliding ? 0.0f : 1.0f;
    v.used = true;
    v.gate = true;
    v.bornLean = !fullQuality();
    v.cut = false;
    v.note = note;
    v.velocity = velocity;
    v.age = ageCounter++;
    v.random = rnd(v.rng) * 2.0f - 1.0f;
    v.bend = 0.0f;
    v.pressure = v.timbre = -1.0f;
    v.prsGlide = 0.0f;
    v.freq = v.glideFrom;
    if (retrigger) {
        for (int e = 0; e < kEnvs; ++e) {
            v.env[e].set(targetOf(EnvBase + e * EnvParams + EDelay), targetOf(EnvBase + e * EnvParams + EAttack),
                         targetOf(EnvBase + e * EnvParams + EDecay), targetOf(EnvBase + e * EnvParams + ESustain),
                         targetOf(EnvBase + e * EnvParams + ERelease), targetOf(EnvBase + e * EnvParams + ERepeat) >= 0.5f);
            v.env[e].trigger();
        }
        for (int l = 0; l < kLfos; ++l) {
            const int32_t b = LfoBase + l * LfoParams;
            if (targetOf(b + LKeySync) >= 0.5f) v.lfo[l].trigger(targetOf(b + LPhase), targetOf(b + LDelay));
        }
        for (int k = 0; k < kOscs; ++k) {
            // Random start phases, not evenly spread. Evenly spaced saws cancel
            // their fundamental until the detune pulls them apart, so a stack
            // would start thin.
            for (int c = 0; c < kDensity; ++c) v.osc[k].phase[c] = rnd(v.rng);
            v.osc[k].syncPhase = 0.0f;
        }
    }
}

void Trinity::noteOn(uint8_t note, uint8_t velocity) {
    const int mode = stepOf(VoiceMode); // 0 poly, 1 mono, 2 legato, 3 unison
    if (mode == 1 || mode == 2) {
        Voice &v = voices[0];
        const bool wasHeld = v.used && v.gate;
        startVoice(v, note, velocity, !(mode == 2 && wasHeld));
        return;
    }
    if (mode == 3) {
        const int n = stepOf(UnisonCount);
        const float detune = targetOf(UnisonDetune) * 25.0f;
        const float spread = targetOf(UnisonSpread);
        for (int i = 0; i < n; ++i) {
            Voice *v = allocate();
            if (v == nullptr) break;
            startVoice(*v, note, velocity, true);
            v->detuneCents = n > 1 ? kSpread[i % kDensity] * detune : 0.0f;
            v->panOffset = n > 1 ? kSpread[i % kDensity] * spread : 0.0f;
        }
        return;
    }
    Voice *v = allocate();
    if (v == nullptr) return;
    startVoice(*v, note, velocity, true);
    v->detuneCents = 0.0f;
    v->panOffset = 0.0f;
}

void Trinity::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            for (auto &e : v.env) e.release();
        }
    }
}

void Trinity::allNotesOff() {
    for (auto &v : voices) {
        v.gate = false;
        for (auto &e : v.env) e.kill();
        v.used = false;
    }
}

void Trinity::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}

void Trinity::channelPressure(uint8_t value) { aftertouch = static_cast<float>(value) / 127.0f; }

void Trinity::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Trinity::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Trinity::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

void Trinity::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Trinity::onBlock(int64_t, int64_t, float bpmNow) { bpm = bpmNow; }

// --- Modulation ------------------------------------------------------------------

float Trinity::sourceValue(const Voice &v, int src) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    // A finger's own pressure if it sent any, otherwise the channel's.
    case SrcAftertouch: return v.pressure >= 0.0f ? v.pressure : aftertouch;
    case SrcVelocity: return static_cast<float>(v.velocity) / 127.0f;
    case SrcKeyTrack: return (static_cast<float>(v.note) - 60.0f) / 48.0f;
    case SrcRandom: return v.random;
    case SrcEnvAmp: return v.env[0].value();
    case SrcEnvFilter: return v.env[1].value();
    case SrcEnv3: return v.env[2].value();
    case SrcEnv4: return v.env[3].value();
    case SrcEnv5: return v.env[4].value();
    case SrcEnv6: return v.env[5].value();
    case SrcLfo1: return v.lfo[0].value();
    case SrcLfo2: return v.lfo[1].value();
    case SrcLfo3: return v.lfo[2].value();
    default: return 0.0f;
    }
}

int32_t Trinity::envMask() const {
    // The first two (amp and filter) are always used. The rest only run if
    // a matrix slot uses them.
    int32_t mask = 0x3;
    for (int s = 0; s < kMatrixSlots; ++s) {
        const int32_t b = MatrixBase + s * MatrixParams;
        if (stepOf(b + XDest) == DstOff) continue;
        const int src = stepOf(b + XSrc);
        const int src2 = stepOf(b + XSrc2);
        if (src >= SrcEnvAmp && src <= SrcEnv6) mask |= 1 << (src - SrcEnvAmp);
        if (src2 >= SrcEnvAmp && src2 <= SrcEnv6) mask |= 1 << (src2 - SrcEnvAmp);
    }
    return mask;
}

void Trinity::cutTails() {
    int tails = 0;
    for (const auto &v : voices) tails += v.used && !v.gate && !v.cut;
    for (; tails > kLeanTails; --tails) {
        Voice *oldest = nullptr;
        for (auto &v : voices) {
            if (!v.used || v.gate || v.cut) continue;
            if (oldest == nullptr || v.age < oldest->age) oldest = &v;
        }
        if (oldest == nullptr) break;
        oldest->cut = true;
        oldest->env[0].hasten(0.01f);
    }
}

void Trinity::updateVoiceMod(Voice &v, float blockSeconds) {
    for (int l = 0; l < kLfos; ++l) {
        const int32_t b = LfoBase + l * LfoParams;
        const int sync = stepOf(b + LSync);
        float hz = paramOf(b + LRate);
        if (sync > 0) hz = bpm / (60.0f * syncBeats(sync));
        hz *= std::exp2(v.mod[DstLfo1Rate + l] * 4.0f);
        v.lfo[l].advance(stepOf(b + LWave), hz, blockSeconds, paramOf(b + LSlew), paramOf(b + LOneShot) >= 0.5f);
    }
    for (auto &m : v.mod) m = 0.0f;
    for (int s = 0; s < kMatrixSlots; ++s) {
        const int32_t b = MatrixBase + s * MatrixParams;
        const int dest = stepOf(b + XDest);
        if (dest == DstOff) continue;
        const int src = stepOf(b + XSrc);
        if (src == SrcOff) continue;
        const int src2 = stepOf(b + XSrc2);
        const float a = sourceValue(v, src);
        const float bmul = src2 == SrcOff ? 1.0f : sourceValue(v, src2);
        v.mod[dest] += a * bmul * paramOf(b + XDepth);
    }
    // A finger's pressure makes it brighter and louder, whatever the matrix says.
    const float prs = glidePressure(v.prsGlide, v.pressure, aftertouch) * paramOf(MpePressure);
    v.mod[DstF1Freq] += prs * 0.4f;
    v.mod[DstF2Freq] += prs * 0.4f;
    v.mod[DstAmp] += prs * 0.4f;
    // The mod wheel opens both filters, up to about five octaves.
    const float wheel = modWheel * paramOf(WheelFilter) * 0.8f;
    v.mod[DstF1Freq] += wheel;
    v.mod[DstF2Freq] += wheel;
}

// --- Render ----------------------------------------------------------------------

float Trinity::renderVoice(Voice &v, int32_t frames, float *out) {
    // Block-rate reads. Everything the sample loop needs is worked out once
    // here, including the pitch multipliers. An exp2 per sample per
    // oscillator is the difference between eight voices and sixteen on a phone.
    struct OscCfg {
        int wave, table, density, mip, frame;
        bool needed;
        float frac, level, pw, sync, hardK, drift, pitchMul, detuneMul[kDensity];
        /**
         * 1 / sqrt(density), worked out once a block rather than per sample
         * since it can't change inside a block.
         */
        float densityNorm;
        /** The phase increment, updated with the mip rather than per sample. */
        float baseInc, inc;
        /** The pair WavetableBank::between reads, updated with the mip. */
        const float *rowA;
        const float *rowB;
    } cfg[kOscs];

    const float ring12 = clampf(paramOf(MixBase + MRing12) + v.mod[DstRing12], 0.0f, 1.0f);
    const float ring23 = clampf(paramOf(MixBase + MRing23) + v.mod[DstRing23], 0.0f, 1.0f);
    const float fm21 = clampf(paramOf(MixBase + MFm21) + v.mod[DstFm21], 0.0f, 1.0f) * 0.5f;
    const float fm32 = clampf(paramOf(MixBase + MFm32) + v.mod[DstFm32], 0.0f, 1.0f) * 0.5f;
    const float noiseLevel = clampf(paramOf(MixBase + MNoise) + v.mod[DstNoise], 0.0f, 1.0f);
    const float noiseK = clampf(0.02f + paramOf(MixBase + MNoiseColour) * 0.98f, 0.0f, 1.0f);

    const float bendSemis = bend * paramOf(BendRange) + v.bend;
    const float globalSemis = paramOf(Transpose) + 12.0f * paramOf(Octave) + bendSemis +
                              v.mod[DstPitch] * 24.0f + v.detuneCents * 0.01f;
    for (int k = 0; k < kOscs; ++k) {
        const int32_t b = OscBase + k * OscParams;
        OscCfg &c = cfg[k];
        c.wave = stepOf(b + OWave);
        c.table = c.wave - WFirstTable;
        // Half the unison stack when the note was started in lean mode.
        //
        // The stack multiplies the cost of every oscillator of every voice,
        // so it's where lean mode has to cut on this machine.
        //
        // Halved rather than flattened, and never below two, so an eight-voice
        // stack at four sounds the same but narrower. Densities of one or two
        // are left alone.
        //
        // densityNorm is 1 / sqrt(n) of the stack actually summed, so the level
        // doesn't jump.
        const int32_t asked = stepOf(b + ODensity);
        c.density = (v.bornLean && asked > 2) ? std::max(2, asked / 2) : asked;
        c.densityNorm = c.density > 1 ? 1.0f / std::sqrt(static_cast<float>(c.density)) : 1.0f;
        c.rowA = nullptr;
        c.rowB = nullptr;
        c.level = clampf(paramOf(b + OLevel) + v.mod[DstLevel1 + k], 0.0f, 2.0f);
        c.pw = clampf(paramOf(b + OPw) + v.mod[DstPw1 + k] * 0.5f, 0.02f, 0.98f);
        c.sync = clampf(paramOf(b + OSync) + v.mod[DstSync1 + k], 0.0f, 1.0f);
        c.drift = paramOf(b + ODrift);
        const float semis = globalSemis + paramOf(b + OCoarse) + paramOf(b + OFine) * 0.01f + v.mod[DstPitch1 + k] * 24.0f;
        c.pitchMul = std::exp2(semis / 12.0f);
        const float detune = clampf(paramOf(b + ODetune) + v.mod[DstDetune], 0.0f, 1.0f) * 28.0f;
        for (int d = 0; d < kDensity; ++d) c.detuneMul[d] = std::exp2(kSpread[d] * detune * 0.01f / 12.0f);
        const float hardHz = 200.0f * std::exp2(paramOf(b + OHard) * 7.0f);
        c.hardK = clampf(1.0f - std::exp(-kTwoPi * hardHz / sampleRate), 0.0f, 1.0f);
        const float pos = clampf(paramOf(b + OPos) + v.mod[DstPos1 + k], 0.0f, 1.0f) *
                          static_cast<float>(WavetableBank::kFrames - 1);
        c.frame = static_cast<int>(pos);
        c.frac = pos - static_cast<float>(c.frame);
        // Warp turns the crossfade between frames into a switch, smooth to glitchy.
        const float warp = paramOf(b + OWarp);
        if (warp > 0.0f) c.frac += ((c.frac < 0.5f ? 0.0f : 1.0f) - c.frac) * warp;
        c.mip = 0;
    }
    // An oscillator only runs if it's heard or something reads it.
    cfg[0].needed = cfg[0].level > 0.0001f || ring12 > 0.0f;
    cfg[1].needed = cfg[1].level > 0.0001f || ring12 > 0.0f || ring23 > 0.0f || fm21 > 0.0f;
    cfg[2].needed = cfg[2].level > 0.0001f || ring23 > 0.0f || fm32 > 0.0f;

    const int route = stepOf(RouteIndex);
    const float balance = clampf(paramOf(BalanceIndex) + v.mod[DstBalance], 0.0f, 1.0f);
    const float keyOffset = static_cast<float>(v.note) - 60.0f;
    float fFreq[2], fRes[2], fDrive[2], fEnvAmt[2];
    int fType[2], fDriveType[2];
    for (int f = 0; f < 2; ++f) {
        const int32_t b = FilterBase + f * FilterParams;
        fType[f] = stepOf(b + FType);
        fDriveType[f] = stepOf(b + FDriveType);
        fDrive[f] = clampf(paramOf(b + FDrive) + v.mod[DstDrive], 0.0f, 1.0f);
        fRes[f] = clampf(paramOf(b + FRes) + v.mod[DstF1Res + f], 0.0f, 1.0f);
        fEnvAmt[f] = paramOf(b + FEnv);
        const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
        fFreq[f] = paramOf(b + FFreq) * std::exp2(paramOf(b + FKey) * keyOffset / 12.0f +
                                                  v.mod[DstF1Freq + f] * 6.0f +
                                                  slide * paramOf(MpeTimbre) * 4.0f);
    }

    const float velAmp = velocityGain(static_cast<float>(v.velocity) / 127.0f, paramOf(VelocityAmount));
    // The app's common level, so this machine's default is as loud as every
    // other machine's (see Reflux's kHouse).
    constexpr float kHouse = 0.61f;
    const float volume = clampf(paramOf(Volume) + v.mod[DstAmp], 0.0f, 2.0f) * kHouse;
    const float glideSeconds = paramOf(Glide);
    const float glideStep = glideSeconds > 0.001f ? 1.0f / (glideSeconds * sampleRate) : 1.0f;
    const float targetFreq = noteHz(static_cast<float>(v.note));
    const float glideOctaves = v.glidePos < 1.0f ? std::log2(targetFreq / v.glideFrom) : 0.0f;
    float peak = 0.0f;
    // A bit mask: pitch is updated when (i & pitchStride) is 0, so 15 is one
    // sample in 16 and 0 is every sample. Keep it one less than a power of two.
    // A variable so the paired harness can time both in one process.
    const int32_t pitchStride = 15;

    for (int32_t i = 0; i < frames; ++i) {
        const float envAmp = v.env[0].next();
        const float envFilter = v.env[1].next();
        for (int e = 2; e < kEnvs; ++e) {
            if (envUsed & (1 << e)) v.env[e].next();
        }

        if (v.glidePos < 1.0f) {
            v.glidePos += glideStep;
            if (v.glidePos >= 1.0f) { v.glidePos = 1.0f; v.freq = targetFreq; }
            else v.freq = v.glideFrom * std::exp2(glideOctaves * v.glidePos); // glide in pitch, not in Hz
        } else {
            v.freq = targetFreq;
        }

        // Filter coefficients at 1/16 of the sample rate. The envelope can't
        // move much in 16 samples and tan() is expensive.
        if ((i & 15) == 0) {
            const float envOct = envFilter * 4.0f;
            v.filter[0].set(fFreq[0] * std::exp2(fEnvAmt[0] * envOct), fRes[0], fType[0], fDriveType[0], fDrive[0]);
            v.filter[1].set(fFreq[1] * std::exp2(fEnvAmt[1] * envOct), fRes[1], fType[1], fDriveType[1], fDrive[1]);
        }

        // Oscillator 3 first: FM runs 3 -> 2 -> 1 inside the sample.
        float o[kOscs] = {0.0f, 0.0f, 0.0f};
        for (int k = kOscs - 1; k >= 0; --k) {
            OscCfg &c = cfg[k];
            if (!c.needed) continue;
            OscState &st = v.osc[k];
            // Pitch is updated every 16 samples, the same as the filter
            // coefficients. Nothing here moves at audio rate: drift is a slow
            // random walk and v.freq only changes while gliding, where a 3 kHz
            // update rate is inaudible. It saves a clamp, three multiplies and a
            // compare per oscillator per voice per sample.
            //
            // The drift coefficient is multiplied by the stride so the walk takes
            // the same time.
            if ((i & pitchStride) == 0) {
                st.drift += (st.driftTarget - st.drift) * 0.00048f;
                if (std::fabs(st.drift - st.driftTarget) < 0.01f) st.driftTarget = rnd(v.rng) * 2.0f - 1.0f;
                const float hz = clampf(v.freq * c.pitchMul * (1.0f + st.drift * c.drift * 0.0046f), 1.0f,
                                        sampleRate * 0.49f);
                // Multiply by the reciprocal rather than divide, since divides are
                // much slower on phone CPUs.
                c.baseInc = hz * invSampleRate;
                c.inc = c.baseInc * (1.0f + c.sync * 3.0f);
            }
            const float baseInc = c.baseInc;
            const float inc = c.inc;
            if (c.sync > 0.0f) {
                st.syncPhase += baseInc;
                if (st.syncPhase >= 1.0f) {
                    st.syncPhase -= 1.0f;
                    for (int d = 0; d < c.density; ++d) st.phase[d] = 0.0f;
                }
            }
            if (c.wave >= WFirstTable && (i & 15) == 0) {
                c.mip = WavetableBank::mipFor(c.inc * sampleRate);
                bank->rowsFor(c.table, c.frame, c.mip, c.rowA, c.rowB);
            }
            const float fmIn = k == 0 ? o[1] * fm21 : (k == 1 ? o[2] * fm32 : 0.0f);
            float sum = 0.0f;
            for (int d = 0; d < c.density; ++d) {
                float ph = st.phase[d] + fmIn;
                ph -= std::floor(ph);
                switch (c.wave) {
                case WSaw: sum += sawAt(ph, inc); break;
                case WSquare: sum += pulseAt(ph, inc, c.pw); break;
                case WTriangle: sum += triangleAt(ph); break;
                case WSine: sum += std::sin(ph * kTwoPi); break;
                default:
                    sum += WavetableBank::between(c.rowA, c.rowB, c.frac, ph);
                    break;
                }
                st.phase[d] += inc * c.detuneMul[d];
                if (st.phase[d] >= 1.0f) st.phase[d] -= 1.0f;
            }
            if (c.density > 1) sum *= c.densityNorm;
            st.hardZ += (sum - st.hardZ) * c.hardK;
            o[k] = st.hardZ;
        }

        float mix = o[0] * cfg[0].level + o[1] * cfg[1].level + o[2] * cfg[2].level;
        if (ring12 > 0.0f) mix += o[0] * o[1] * ring12;
        if (ring23 > 0.0f) mix += o[1] * o[2] * ring23;
        if (noiseLevel > 0.0f) {
            const float white = rnd(noiseRng) * 2.0f - 1.0f;
            noiseZ += (white - noiseZ) * noiseK;
            mix += noiseZ * noiseLevel;
        }

        float filtered;
        if (route == 1) { // parallel: the same signal through both
            filtered = v.filter[0].process(mix) * (1.0f - balance) + v.filter[1].process(mix) * balance;
        } else if (route == 2) { // split: filter 2 fed from filter 1
            const float a = v.filter[0].process(mix);
            filtered = a * (1.0f - balance) + v.filter[1].process(a) * balance;
        } else { // serial
            filtered = v.filter[1].process(v.filter[0].process(mix));
        }

        const float s = filtered * envAmp * velAmp * volume * 0.5f;
        out[i] = s;
        const float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    return peak;
}

bool Trinity::render(float *L, float *R, int32_t frames) {
    params_.tick();
    envUsed = envMask();
    if (!fullQuality()) cutTails();
    for (int32_t i = 0; i < frames; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    const float blockSeconds = static_cast<float>(frames) / sampleRate;
    const float panBase = paramOf(Pan);
    for (auto &v : voices) {
        if (!v.used) continue;
        updateVoiceMod(v, blockSeconds);
        renderVoice(v, frames, voiceBuf);
        const float pan = clampf(panBase + v.mod[DstPan] + v.panOffset, -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * 0.25f * dsp::kPi;
        const float gl = std::cos(angle) * 1.4142f, gr = std::sin(angle) * 1.4142f;
        for (int32_t i = 0; i < frames; ++i) {
            L[i] += voiceBuf[i] * gl;
            R[i] += voiceBuf[i] * gr;
        }
        if (!v.gate && !v.env[0].active()) v.used = false;
    }
    return true;
}

} // namespace acidulous::machine
