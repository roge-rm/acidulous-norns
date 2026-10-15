#include "Ratio.h"
#include <engine/machine/Voices.h>
#include <engine/dsp/Lfo.h>

#include <cstdio>

namespace acidulous::machine {

using dsp::clampf;
using dsp::kPi;
using dsp::kTwoPi;
using dsp::mtof;
using namespace acidulous::machine::ratio;

namespace {

// How many radians of phase modulation a full-level operator delivers.
constexpr float kIndex = 7.0f;
// Free, then the tempo-locked LFOs' note values (dsp::Lfo).
constexpr int kSyncCount = 1 + dsp::Lfo::kRates;
inline float syncBeats(int sync) { return sync <= 0 ? 0.0f : dsp::Lfo::beatsOf(sync - 1); }
// Partials of a struck bar, for the bell snap.
constexpr float kBellPartials[8] = {0.5f, 1.0f, 2.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f};

inline float rnd(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 16777216.0f;
}

inline float foldInto(float x) {
    const float t = x * 0.25f + 0.25f;
    return 4.0f * std::fabs(t - std::floor(t + 0.5f)) - 1.0f;
}

} // namespace

// The operator waveforms. They're not band-limited on purpose, since the
// aliasing of non-sine FM operators is part of the sound.
float Ratio::waveAt(int wave, float p, OpState &st) {
    switch (wave) {
    case 0: return std::sin(p * kTwoPi);
    case 1: { const float s = std::sin(p * kTwoPi); return std::round(s * 2048.0f) * (1.0f / 2048.0f); }
    case 2: { const float s = std::sin(p * kTwoPi); return std::round(s * 64.0f) * (1.0f / 64.0f); }
    case 3: return p < 0.5f ? std::sin(p * kTwoPi) : 0.0f;                        // half
    case 4: return std::fabs(std::sin(p * kPi));                                   // rectified
    case 5: return (p < 0.25f || (p >= 0.5f && p < 0.75f)) ? std::sin(p * kTwoPi) : 0.0f; // quarter
    case 6: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;                   // triangle
    case 7: return 2.0f * p - 1.0f;                                                // saw
    case 8: return p < 0.5f ? 1.0f : -1.0f;                                        // square
    case 9: return p < 0.25f ? 1.0f : -1.0f;                                       // pulse 25%
    case 10: return (std::sin(p * kTwoPi) + 0.5f * std::sin(p * 2.0f * kTwoPi)) * 0.667f;
    case 11: return (std::sin(p * kTwoPi) + 0.5f * std::sin(p * 3.0f * kTwoPi)) * 0.667f;
    case 12: return (std::sin(p * kTwoPi) + 0.5f * std::sin(p * 2.0f * kTwoPi) + 0.33f * std::sin(p * 3.0f * kTwoPi)) * 0.55f;
    case 13: return (std::sin(p * kTwoPi) + 0.33f * std::sin(p * 3.0f * kTwoPi) + 0.2f * std::sin(p * 5.0f * kTwoPi)) * 0.65f;
    case 14: // sample and hold, one new value per cycle
        if (p < st.noisePhase) st.noiseHeld = rnd(st.noiseRng) * 2.0f - 1.0f;
        st.noisePhase = p;
        return st.noiseHeld;
    default: return rnd(st.noiseRng) * 2.0f - 1.0f;
    }
}

Ratio::Ratio() { initParams(); }

// --- Parameters ------------------------------------------------------------------

const ParamDef *Ratio::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static char names[Count][16];
    static bool built = false;
    if (!built) {
        auto put = [&](int32_t i, const char *fmt, int arg, float mn, float mx, float df, Curve c, int32_t steps,
                       const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), fmt, arg);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        auto putN = [&](int32_t i, const char *name, float mn, float mx, float df, Curve c, int32_t steps,
                        const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), "%s", name);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        for (int o = 0; o < kOps; ++o) {
            const int32_t b = OpBase + o * OpParams;
            const int n = o + 1;
            // Operator 1 is the carrier in most algorithms, so it starts
            // audible and the rest silent, which makes a usable init patch.
            const bool carrier = o == 0;
            put(b + OWave, "o%d_wave", n, 0.0f, kWaveCount - 1.0f, 0.0f, Curve::Stepped, kWaveCount, "");
            put(b + OMode, "o%d_mode", n, 0.0f, ModeCount - 1.0f, 0.0f, Curve::Stepped, ModeCount, "");
            put(b + ORatio, "o%d_ratio", n, 0.25f, 64.0f, 1.0f, Curve::Exponential, 0, "");
            put(b + OFine, "o%d_fine", n, -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OFixed, "o%d_fixed", n, 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
            put(b + OLevel, "o%d_level", n, 0.0f, 1.0f, carrier ? 0.8f : 0.0f, Curve::Linear, 0, "");
            put(b + OFeedback, "o%d_fb", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OAttack, "o%d_attack", n, 0.001f, 10.0f, 0.003f, Curve::Exponential, 0, "s");
            put(b + ODecay, "o%d_decay", n, 0.002f, 15.0f, 0.8f, Curve::Exponential, 0, "s");
            put(b + OSustain, "o%d_sustain", n, 0.0f, 1.0f, carrier ? 0.7f : 0.5f, Curve::Linear, 0, "");
            put(b + ORelease, "o%d_release", n, 0.002f, 15.0f, 0.25f, Curve::Exponential, 0, "s");
            put(b + OVel, "o%d_vel", n, 0.0f, 1.0f, carrier ? 0.5f : 0.3f, Curve::Linear, 0, "");
            put(b + OKey, "o%d_key", n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + OPan, "o%d_pan", n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        }
        putN(AlgoA, "algoa", 0.0f, kAlgorithmCount - 1.0f, 0.0f, Curve::Stepped, kAlgorithmCount, "");
        putN(AlgoB, "algob", 0.0f, kAlgorithmCount - 1.0f, 22.0f, Curve::Stepped, kAlgorithmCount, "");
        putN(Morph, "morph", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(SnapMode, "snap", 0.0f, SnapCount - 1.0f, 1.0f, Curve::Stepped, SnapCount, "");
        putN(Skew, "skew", -0.5f, 0.5f, 0.0f, Curve::Linear, 0, "");
        putN(FilterType, "f_type", 0.0f, dsp::MultiFilter::TypeCount - 1.0f, 3.0f, Curve::Stepped,
             dsp::MultiFilter::TypeCount, "");
        putN(FilterFreq, "f_freq", 20.0f, 20000.0f, 18000.0f, Curve::Exponential, 0, "Hz");
        putN(MpeTimbre, "mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(MpePressure, "mpepressure", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(FilterRes, "f_res", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterEnv, "f_env", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterKey, "f_key", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterAttack, "f_attack", 0.001f, 10.0f, 0.002f, Curve::Exponential, 0, "s");
        putN(FilterDecay, "f_decay", 0.002f, 15.0f, 0.8f, Curve::Exponential, 0, "s");
        putN(FilterSustain, "f_sustain", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, "");
        putN(FilterRelease, "f_release", 0.002f, 15.0f, 0.3f, Curve::Exponential, 0, "s");
        for (int e = 0; e < kModEgs; ++e) {
            const int32_t b = EgBase + e * EgParams;
            const int n = e + 1;
            put(b + EAttack, "e%d_attack", n, 0.001f, 10.0f, 0.002f, Curve::Exponential, 0, "s");
            put(b + EDecay, "e%d_decay", n, 0.002f, 15.0f, 0.5f, Curve::Exponential, 0, "s");
            put(b + ESustain, "e%d_sustain", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + ERelease, "e%d_release", n, 0.002f, 15.0f, 0.3f, Curve::Exponential, 0, "s");
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
            put(b + LKeySync, "l%d_keysync", n, 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, "");
        }
        for (int m = 0; m < kMatrixSlots; ++m) {
            const int32_t b = MatrixBase + m * MatrixParams;
            const int n = m + 1;
            put(b + XSrc, "m%02d_src", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XSrc2, "m%02d_src2", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XDest, "m%02d_dest", n, 0.0f, DestCount - 1.0f, 0.0f, Curve::Stepped, DestCount, "");
            put(b + XDepth, "m%02d_depth", n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        }
        putN(VoiceMode, "voicemode", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, "");
        putN(Glide, "glide", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s");
        putN(GlideMode, "glidemode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        putN(BendRange, "bend", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, "st");
        putN(Octave, "octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, "");
        putN(Transpose, "transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, "st");
        putN(Volume, "volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, "");
        putN(Pan, "pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(VelocityAmount, "velamt", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
        built = true;
    }
    count = Count;
    return defs;
}

// --- Lifecycle -------------------------------------------------------------------

void Ratio::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    invSampleRate = 1.0f / sampleRate;
    for (auto &v : voices) {
        for (auto &e : v.env) e.setSampleRate(sampleRate);
        for (auto &e : v.modEg) e.setSampleRate(sampleRate);
        v.filterEg.setSampleRate(sampleRate);
        v.filter.setSampleRate(sampleRate);
    }
    reset();
}

void Ratio::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        for (auto &e : v.env) e.kill();
        for (auto &e : v.modEg) e.kill();
        v.filterEg.kill();
        v.filter.reset();
        for (auto &o : v.op) o = OpState();
        for (auto &m : v.mod) m = 0.0f;
        v.rng = Voice::kSeed;
    }
    modWheel = pressure = bend = 0.0f;
    buildRouting();
}

// Blend the two algorithms into one matrix, then flatten it to the edges that
// carry something. Runs once per block, not per voice.
void Ratio::fillRouting(Routing &routing, float morph) const {
    const Algorithm &a = kAlgorithms[stepOf(AlgoA) % kAlgorithmCount];
    const Algorithm &b = kAlgorithms[stepOf(AlgoB) % kAlgorithmCount];
    const float m = clampf(morph, 0.0f, 1.0f);
    for (int s = 0; s < kOps; ++s) {
        for (int d = 0; d < kOps; ++d) routing.amount[s][d] = 0.0f;
    }
    for (int i = 0; i < a.edgeCount; ++i) routing.amount[a.edges[i][0]][a.edges[i][1]] += 1.0f - m;
    for (int i = 0; i < b.edgeCount; ++i) routing.amount[b.edges[i][0]][b.edges[i][1]] += m;
    routing.edgeCount = 0;
    for (int s = 0; s < kOps; ++s) {
        for (int d = 0; d < kOps; ++d) {
            if (routing.amount[s][d] > 1e-4f) {
                routing.order[routing.edgeCount][0] = s;
                routing.order[routing.edgeCount][1] = d;
                ++routing.edgeCount;
            }
        }
    }
    float sum = 0.0f;
    for (int o = 0; o < kOps; ++o) {
        const float ca = (a.carriers >> o) & 1 ? 1.0f : 0.0f;
        const float cb = (b.carriers >> o) & 1 ? 1.0f : 0.0f;
        routing.carrier[o] = ca * (1.0f - m) + cb * m;
        sum += routing.carrier[o];
    }
    routing.carrierSum = sum > 0.5f ? std::sqrt(sum) : 1.0f;
}

void Ratio::buildRouting() {
    fillRouting(routing, paramOf(Morph));

    // Also note which mod envelopes any matrix slot uses, so unused ones can
    // be skipped. See Trinity's `envMask`.
    egUsed = 0;
    for (int s = 0; s < kMatrixSlots; ++s) {
        const int32_t b = MatrixBase + s * MatrixParams;
        if (stepOf(b + XDest) == DstOff) continue;
        const int a = stepOf(b + XSrc), b2 = stepOf(b + XSrc2);
        if (a >= SrcEg1 && a <= SrcEg3) egUsed |= 1 << (a - SrcEg1);
        if (b2 >= SrcEg1 && b2 <= SrcEg3) egUsed |= 1 << (b2 - SrcEg1);
    }
}

Ratio::Voice *Ratio::allocate() {
    Voice *best = nullptr;
    for (auto &v : voices) if (!v.used) return &v;
    for (auto &v : voices) {
        if (v.gate) continue;
        if (best == nullptr || v.age < best->age) best = &v;
    }
    if (best != nullptr) return best;
    for (auto &v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

void Ratio::startVoice(Voice &v, uint8_t note, uint8_t velocity, bool retrigger) {
    const float glideSeconds = targetOf(Glide);
    const bool gliding = glideSeconds > 0.001f && v.used && (stepOf(GlideMode) == 0 || v.gate);
    v.glideFrom = gliding ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = gliding ? 0.0f : 1.0f;
    v.freq = v.glideFrom;
    v.used = true;
    v.gate = true;
    v.note = note;
    v.bend = 0.0f;
    v.pressure = v.timbre = -1.0f;
    v.prsGlide = 0.0f;
    v.velocity = velocity;
    v.age = ageCounter++;
    v.random = rnd(v.rng) * 2.0f - 1.0f;
    if (!retrigger) return;
    for (int o = 0; o < kOps; ++o) {
        const int32_t b = OpBase + o * OpParams;
        v.env[o].set(0.0f, targetOf(b + OAttack), targetOf(b + ODecay), targetOf(b + OSustain), targetOf(b + ORelease), false);
        v.env[o].trigger();
        v.op[o].phase = 0.0f; // FM needs the same phase relationship every note
        v.op[o].out = 0.0f;
    }
    for (int e = 0; e < kModEgs; ++e) {
        const int32_t b = EgBase + e * EgParams;
        v.modEg[e].set(0.0f, targetOf(b + EAttack), targetOf(b + EDecay), targetOf(b + ESustain), targetOf(b + ERelease), false);
        v.modEg[e].trigger();
    }
    v.filterEg.set(0.0f, targetOf(FilterAttack), targetOf(FilterDecay), targetOf(FilterSustain), targetOf(FilterRelease), false);
    v.filterEg.trigger();
    for (int l = 0; l < kLfos; ++l) {
        const int32_t b = LfoBase + l * LfoParams;
        if (targetOf(b + LKeySync) >= 0.5f) v.lfo[l].trigger(targetOf(b + LPhase), targetOf(b + LDelay));
    }
}

void Ratio::noteOn(uint8_t note, uint8_t velocity) {
    const int mode = stepOf(VoiceMode); // 0 poly, 1 mono, 2 legato
    if (mode == 1 || mode == 2) {
        Voice &v = voices[0];
        const bool held = v.used && v.gate;
        startVoice(v, note, velocity, !(mode == 2 && held));
        return;
    }
    Voice *v = allocate();
    if (v != nullptr) startVoice(*v, note, velocity, true);
}

void Ratio::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            for (auto &e : v.env) e.release();
            for (auto &e : v.modEg) e.release();
            v.filterEg.release();
        }
    }
}

void Ratio::allNotesOff() {
    for (auto &v : voices) {
        v.gate = false;
        for (auto &e : v.env) e.kill();
        for (auto &e : v.modEg) e.kill();
        v.filterEg.kill();
        v.used = false;
    }
}

void Ratio::onBlock(int64_t, int64_t, float bpmNow) { bpm = bpmNow; }
void Ratio::controlChange(uint8_t cc, uint8_t value) { if (cc == 1) modWheel = static_cast<float>(value) / 127.0f; }
void Ratio::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Ratio::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

// --- Ratios ----------------------------------------------------------------------

float Ratio::snapRatio(float r, int mode, float skew) const {
    switch (mode) {
    case SnapHarmonic: r = std::round(r); if (r < 1.0f) r = 1.0f; break;
    case SnapSub: { float n = std::round(1.0f / r); if (n < 1.0f) n = 1.0f; r = 1.0f / n; break; }
    case SnapOdd: { float n = std::round((r - 1.0f) * 0.5f); if (n < 0.0f) n = 0.0f; r = 1.0f + 2.0f * n; break; }
    case SnapSemitone: r = std::exp2(std::round(12.0f * std::log2(r)) / 12.0f); break;
    case SnapBell: {
        float best = kBellPartials[0], bestD = 1e9f;
        for (float p : kBellPartials) {
            const float d = std::fabs(std::log2(p) - std::log2(r));
            if (d < bestD) { bestD = d; best = p; }
        }
        r = best;
        break;
    }
    default: break;
    }
    if (std::fabs(skew) > 1e-4f) r = std::pow(r, 1.0f + skew);
    return clampf(r, 0.02f, 96.0f);
}

void Ratio::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Ratio::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Ratio::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

// --- Modulation ------------------------------------------------------------------

float Ratio::sourceValue(const Voice &v, int src) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    // This note's own pressure if it has one, otherwise the channel's.
    case SrcPressure: return v.pressure >= 0.0f ? v.pressure : pressure;
    case SrcVelocity: return static_cast<float>(v.velocity) / 127.0f;
    case SrcKeyTrack: return (static_cast<float>(v.note) - 60.0f) / 48.0f;
    case SrcRandom: return v.random;
    case SrcEg1: return v.modEg[0].value();
    case SrcEg2: return v.modEg[1].value();
    case SrcEg3: return v.modEg[2].value();
    case SrcFilterEg: return v.filterEg.value();
    case SrcLfo1: return v.lfo[0].value();
    case SrcLfo2: return v.lfo[1].value();
    case SrcLfo3: return v.lfo[2].value();
    default: return 0.0f;
    }
}

void Ratio::updateVoiceMod(Voice &v, float blockSeconds) {
    for (int l = 0; l < kLfos; ++l) {
        const int32_t b = LfoBase + l * LfoParams;
        const int sync = stepOf(b + LSync);
        float hz = paramOf(b + LRate);
        if (sync > 0) hz = bpm / (60.0f * syncBeats(sync));
        hz *= std::exp2(v.mod[DstLfo1Rate + l] * 4.0f);
        v.lfo[l].advance(stepOf(b + LWave), hz, blockSeconds, 0.0f, false);
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
        const float bm = src2 == SrcOff ? 1.0f : sourceValue(v, src2);
        v.mod[dest] += a * bm * paramOf(b + XDepth);
    }
    // Pressure makes the note brighter and louder, on top of the matrix.
    const float prs = glidePressure(v.prsGlide, v.pressure, pressure) * paramOf(MpePressure);
    v.mod[DstFilterFreq] += prs * 0.4f;
    v.mod[DstAmp] += prs * 0.4f;
}

// --- Render ----------------------------------------------------------------------

void Ratio::renderVoice(Voice &v, int32_t frames, float *out) {
    struct OpCfg {
        int wave, mode;
        float freqMul, fixedHz, level, feedback, keyGain, velGain;
        bool fixed;
    } cfg[kOps];

    const float bendSemis = bend * paramOf(BendRange) + v.bend;
    const float globalSemis = paramOf(Transpose) + 12.0f * paramOf(Octave) + bendSemis + v.mod[DstPitch] * 24.0f;
    const float pitchMul = std::exp2(globalSemis / 12.0f);
    const int snapMode = stepOf(SnapMode);
    const float skew = clampf(paramOf(Skew) + v.mod[DstSkew], -0.9f, 0.9f);
    const float keyOffset = static_cast<float>(v.note) - 60.0f;
    const float vel = static_cast<float>(v.velocity) / 127.0f;

    for (int o = 0; o < kOps; ++o) {
        const int32_t b = OpBase + o * OpParams;
        OpCfg &c = cfg[o];
        c.wave = stepOf(b + OWave);
        c.mode = stepOf(b + OMode);
        c.fixed = paramOf(b + OFixed) >= 0.5f;
        const float ratio = snapRatio(paramOf(b + ORatio), c.fixed ? SnapFree : snapMode, c.fixed ? 0.0f : skew);
        c.freqMul = ratio * std::exp2(paramOf(b + OFine) * 0.01f / 12.0f + v.mod[DstRatio1 + o] * 2.0f);
        c.fixedHz = ratio * 55.0f;
        // Key scaling tilts an operator's level across the keyboard so a
        // patch stays even instead of getting harsh at the top.
        c.keyGain = clampf(1.0f + paramOf(b + OKey) * keyOffset / 36.0f, 0.0f, 2.0f);
        c.velGain = 1.0f - paramOf(b + OVel) * (1.0f - vel);
        c.level = clampf(paramOf(b + OLevel) + v.mod[DstLevel1 + o], 0.0f, 2.0f) * c.keyGain * c.velGain;
        c.feedback = clampf(paramOf(b + OFeedback) + v.mod[DstFeedback], 0.0f, 1.0f);
    }

    // The house level, so this machine's default matches the others. See
    // Reflux's kHouse.
    constexpr float kHouse = 0.55f;
    const float volume = clampf(paramOf(Volume) + v.mod[DstAmp], 0.0f, 2.0f) * kHouse;
    const float velAmp = velocityGain(vel, paramOf(VelocityAmount));
    // Slide opens the filter by the MpeTimbre amount.
    const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
    const float filterBase = paramOf(FilterFreq) *
        std::exp2(paramOf(FilterKey) * keyOffset / 12.0f + v.mod[DstFilterFreq] * 6.0f +
                  slide * paramOf(MpeTimbre) * 4.0f);
    const float filterEnvAmt = paramOf(FilterEnv);
    const float filterRes = clampf(paramOf(FilterRes) + v.mod[DstFilterRes], 0.0f, 1.0f);
    const int filterType = stepOf(FilterType);
    const float glideSeconds = paramOf(Glide);
    const float glideStep = glideSeconds > 0.001f ? 1.0f / (glideSeconds * sampleRate) : 1.0f;
    const float targetFreq = noteHz(static_cast<float>(v.note));
    const float glideOctaves = v.glidePos < 1.0f ? std::log2(targetFreq / v.glideFrom) : 0.0f;
    // The block's routing is shared by all voices. A voice with its morph
    // modulated builds its own blend instead.
    Routing own;
    const bool morphed = v.mod[DstMorph] != 0.0f;
    if (morphed) fillRouting(own, paramOf(Morph) + v.mod[DstMorph]);
    const Routing &routing = morphed ? own : this->routing;
    const float invCarriers = 1.0f / routing.carrierSum;

    for (int32_t i = 0; i < frames; ++i) {
        if (v.glidePos < 1.0f) {
            v.glidePos += glideStep;
            if (v.glidePos >= 1.0f) { v.glidePos = 1.0f; v.freq = targetFreq; }
            else v.freq = v.glideFrom * std::exp2(glideOctaves * v.glidePos);
        } else {
            v.freq = targetFreq;
        }
        const float base = v.freq * pitchMul;

        // Everything reads the previous sample, so any routing is legal.
        float modIn[kOps] = {0, 0, 0, 0, 0, 0};
        for (int e = 0; e < routing.edgeCount; ++e) {
            modIn[routing.order[e][1]] += v.op[routing.order[e][0]].out * routing.amount[routing.order[e][0]][routing.order[e][1]];
        }

        float mix = 0.0f;
        for (int o = 0; o < kOps; ++o) {
            OpCfg &c = cfg[o];
            OpState &st = v.op[o];
            const float env = v.env[o].next();
            const float amp = env * c.level;
            const float hz = clampf(c.fixed ? c.fixedHz : base * c.freqMul, 0.05f, sampleRate * 0.49f);
            // Multiply by the reciprocal instead of dividing, since this runs
            // for every operator of every voice every sample.
            const float inc = hz * invSampleRate;
            float y = 0.0f;
            const float self = st.out * c.feedback;
            switch (c.mode) {
            case ModeRing: // the operator's own tone, gated by what feeds it
                y = waveAt(c.wave, st.phase, st) * modIn[o] * 2.0f;
                break;
            case ModeFilter: { // a one-pole lowpass tuned to the operator
                const float k = clampf(1.0f - std::exp(-kTwoPi * hz / sampleRate), 0.0f, 1.0f);
                st.filterZ += (modIn[o] - st.filterZ) * k;
                y = st.filterZ;
                break;
            }
            case ModeFilterFm: { // the same filter, its cutoff swept by the operator
                const float sweep = 1.0f + waveAt(c.wave, st.phase, st) * 0.9f;
                const float k = clampf(1.0f - std::exp(-kTwoPi * clampf(hz * sweep, 20.0f, sampleRate * 0.45f) / sampleRate), 0.0f, 1.0f);
                st.filterZ += (modIn[o] - st.filterZ) * k;
                y = st.filterZ;
                break;
            }
            case ModeFold:
                y = foldInto(modIn[o] * (1.0f + c.feedback * 12.0f));
                break;
            case ModeSync: { // hard sync to whatever feeds it
                if (st.syncArmed < 0.0f && modIn[o] >= 0.0f) st.phase = 0.0f;
                st.syncArmed = modIn[o];
                y = waveAt(c.wave, st.phase, st);
                break;
            }
            case ModePhase: { // phase distortion: the cycle is bent
                // Push the knee toward the start of the cycle, so the first half
                // of the wave is fast and the second half drags. That gives the
                // resonant edge.
                const float bendAmt = 0.5f - c.feedback * 0.45f;
                const float p = st.phase < bendAmt ? st.phase * 0.5f / bendAmt
                                                   : 0.5f + (st.phase - bendAmt) * 0.5f / (1.0f - bendAmt);
                y = waveAt(c.wave, p, st);
                break;
            }
            case ModeCrush: {
                const float levels = std::exp2(1.0f + (1.0f - c.feedback) * 11.0f);
                y = std::round(modIn[o] * levels) / levels;
                break;
            }
            default: { // FM
                float p = st.phase + (modIn[o] * kIndex + self) * 0.159154943f; // radians -> turns
                p -= std::floor(p);
                y = waveAt(c.wave, p, st);
                break;
            }
            }
            st.phase += inc;
            if (st.phase >= 1.0f) st.phase -= std::floor(st.phase);
            st.out = y * amp;
            if (routing.carrier[o] > 0.0f) mix += st.out * routing.carrier[o];
        }
        mix *= invCarriers;

        const float fenv = v.filterEg.next();
        for (int e = 0; e < kModEgs; ++e) {
            if (egUsed & (1 << e)) v.modEg[e].next();
        }
        if ((i & 15) == 0) {
            v.filter.set(filterBase * std::exp2(filterEnvAmt * fenv * 4.0f), filterRes, filterType, 0, 0.0f);
        }
        out[i] = v.filter.process(mix) * velAmp * volume * 0.6f;
    }
}

bool Ratio::render(float *L, float *R, int32_t frames) {
    params_.tick();
    buildRouting();
    for (int32_t i = 0; i < frames; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    const float blockSeconds = static_cast<float>(frames) / sampleRate;
    const float panBase = paramOf(Pan);
    for (auto &v : voices) {
        if (!v.used) continue;
        updateVoiceMod(v, blockSeconds);
        renderVoice(v, frames, voiceBuf);
        const float pan = clampf(panBase + v.mod[DstPan], -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * 0.25f * kPi;
        const float gl = std::cos(angle) * 1.4142f, gr = std::sin(angle) * 1.4142f;
        for (int32_t i = 0; i < frames; ++i) {
            L[i] += voiceBuf[i] * gl;
            R[i] += voiceBuf[i] * gr;
        }
        bool alive = false;
        for (auto &e : v.env) if (e.active()) { alive = true; break; }
        if (!v.gate && !alive) v.used = false;
    }
    return true;
}

} // namespace acidulous::machine
