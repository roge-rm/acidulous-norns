#include "Cipher.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace acidulous::machine {

// Make-up gain for the normalised band sum. The bands are close to
// uncorrelated, so the sum of N grows about as sqrt(N) and the band count
// doesn't need to be in it.
constexpr float kBandMakeup = 16.0f;
// The levels the summed bands and the carrier reach before their saturators,
// used as nominal so the drive knobs change the tone and not the volume.
constexpr float kNominal = 0.3f;
constexpr float kCarrierNominal = 0.5f;
// The level of the consonant (sibilance) path. It should sit under the
// vocoded signal, not over it.
constexpr float kSibilanceMakeup = 2.0f;
// Output gain that puts Init at the same level as other machines.
constexpr float kHouse = 0.94f;
// The volume knob's maximum, shared by the knob and the clamp in render.
constexpr float kVolumeMax = 2.0f;
using namespace dsp;

namespace {
constexpr float kPiF = 3.14159265f;
} // namespace

Cipher::Cipher() { initParams(); }

const ParamDef *Cipher::paramDefs(int32_t &count) const {
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

        put(Bands, "bands", 4.0f, 40.0f, 16.0f, Curve::Stepped, 37, "");
        // The bottom of the bank defaults to 80 Hz, low enough to catch most
        // of a speaking voice's fundamental. Going lower makes each band
        // wider and hurts intelligibility.
        exp_(LowHz, "low", 40.0f, 600.0f, 80.0f, "Hz");
        exp_(HighHz, "high", 1500.0f, 16000.0f, 8000.0f, "Hz");
        lin(BandQ, "q", 0.0f, 1.0f, 0.55f);
        step(Slope, "slope", 2, 1.0f); // one pole pair, or two
        step(RoleMode, "role", RoleCount, 0.0f);
        exp_(Attack, "attack", 0.0005f, 0.2f, 0.004f, "s");
        exp_(Release, "release", 0.005f, 2.0f, 0.06f, "s");
        lin(Smear, "smear", -1.0f, 1.0f, 0.0f);
        lin(Gate, "gate", 0.0f, 1.0f, 0.0f);
        lin(GateDepth, "gatedepth", 0.0f, 1.0f, 1.0f);
        lin(Shift, "shift", -12.0f, 12.0f, 0.0f);
        lin(Stretch, "stretch", -1.0f, 1.0f, 0.0f);
        step(RemapMode, "remap", RemapCount, 0.0f);
        lin(RemapAmount, "remapamt", 0.0f, 1.0f, 1.0f);
        put(Seed, "seed", 0.0f, 31.0f, 0.0f, Curve::Stepped, 32, "");
        step(Freeze, "freeze", 2, 0.0f);
        lin(FreezeMorph, "frzmorph", 0.0f, 1.0f, 1.0f);
        exp_(FreezeDecay, "frzdecay", 0.05f, 30.0f, 30.0f, "s");
        lin(Sibilance, "sibilance", 0.0f, 1.0f, 0.4f);
        exp_(SibilanceHz, "sibhz", 1500.0f, 12000.0f, 4500.0f, "Hz");
        lin(SibilanceLevel, "siblevel", 0.0f, 1.0f, 0.5f);
        step(PitchTrack, "track", 2, 0.0f);
        exp_(TrackGlide, "trackglide", 0.001f, 0.5f, 0.03f, "s");
        lin(TrackAmount, "trackamt", 0.0f, 1.0f, 1.0f);
        lin(Feedback, "feedback", 0.0f, 0.95f, 0.0f);
        lin(FeedbackTone, "fbtone", 0.0f, 1.0f, 0.5f);
        step(CarrierWaveA, "wave a", CarrierWaveCount, 0.0f);
        step(CarrierWaveB, "wave b", CarrierWaveCount, 2.0f);
        lin(CarrierMix, "mix", 0.0f, 1.0f, 0.35f);
        lin(Detune, "detune", 0.0f, 50.0f, 12.0f, "c");
        lin(PulseWidth, "pw", 0.05f, 0.95f, 0.35f);
        lin(SubLevel, "sub", 0.0f, 1.0f, 0.2f);
        lin(NoiseLevel, "noise", 0.0f, 1.0f, 0.08f);
        lin(CarrierDrive, "cardrive", 0.0f, 1.0f, 0.25f);
        lin(Unvoiced, "unvoiced", 0.0f, 1.0f, 0.0f);
        lin(Dry, "dry", 0.0f, 1.0f, 0.0f);
        lin(Wet, "wet", 0.0f, 1.0f, 1.0f);
        lin(Drive, "drive", 0.0f, 1.0f, 0.15f);
        // Goes up to 2.0 since the output level depends on the input voice,
        // and patches that throw away a lot of spectrum need the extra room.
        lin(Volume, "volume", 0.0f, kVolumeMax, 0.8f);
        lin(Pan, "pan", -1.0f, 1.0f, 0.0f);
        exp_(AmpAttack, "ampatk", 0.001f, 4.0f, 0.01f, "s");
        exp_(AmpDecay, "ampdec", 0.005f, 8.0f, 0.5f, "s");
        lin(AmpSustain, "ampsus", 0.0f, 1.0f, 1.0f);
        exp_(AmpRelease, "amprel", 0.005f, 8.0f, 0.2f, "s");
        exp_(Eg1A, "eg1atk", 0.001f, 8.0f, 0.01f, "s");
        exp_(Eg1D, "eg1dec", 0.005f, 12.0f, 0.5f, "s");
        lin(Eg1S, "eg1sus", 0.0f, 1.0f, 0.6f);
        exp_(Eg1R, "eg1rel", 0.005f, 12.0f, 0.4f, "s");
        exp_(Eg2A, "eg2atk", 0.001f, 8.0f, 0.8f, "s");
        exp_(Eg2D, "eg2dec", 0.005f, 12.0f, 2.0f, "s");
        lin(Eg2S, "eg2sus", 0.0f, 1.0f, 1.0f);
        exp_(Eg2R, "eg2rel", 0.005f, 12.0f, 1.0f, "s");
        step(Lfo1Wave, "lfo1wave", LfoGen::WaveCount, 0.0f);
        exp_(Lfo1Rate, "lfo1rate", 0.01f, 20.0f, 0.3f, "Hz");
        step(Lfo1Sync, "lfo1sync", 6, 0.0f);
        lin(Lfo1Depth, "lfo1depth", 0.0f, 1.0f, 1.0f);
        step(Lfo2Wave, "lfo2wave", LfoGen::WaveCount, 1.0f);
        exp_(Lfo2Rate, "lfo2rate", 0.01f, 20.0f, 2.0f, "Hz");
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
        exp_(Glide, "glide", 0.0f + 0.001f, 2.0f, 0.001f, "s");
        lin(BendRange, "bend", 0.0f, 12.0f, 2.0f);
        lin(Octave, "octave", -3.0f, 3.0f, 0.0f);
        lin(Transpose, "transpose", -12.0f, 12.0f, 0.0f);
        lin(Fine, "fine", -50.0f, 50.0f, 0.0f, "c");
        lin(VelocityAmount, "vel", 0.0f, 1.0f, 1.0f);
        built = true;
    }
    count = Count;
    return defs;
}

int32_t Cipher::steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }

void Cipher::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    invSampleRate = 1.0f / sampleRate;
    for (auto &b : bands) {
        b.analysis1.setSampleRate(sampleRate);
        b.analysis2.setSampleRate(sampleRate);
        b.synthesis1.setSampleRate(sampleRate);
        b.synthesis2.setSampleRate(sampleRate);
    }
    sibilanceFilter.setSampleRate(sampleRate);
    sibilanceShaper.setSampleRate(sampleRate);
    for (auto &v : voices) v.amp.setSampleRate(sampleRate);
    for (auto &e : eg) e.setSampleRate(sampleRate);
    lastCount = -1;
    for (auto &k : lastBandKey) k = NAN; // the coefficients are for the old rate
    rebuildBands();
    reset();
}

// The bank is spaced logarithmically, like hearing. A linear bank would put
// half its filters above 10 kHz, where speech has little.
void Cipher::rebuildBands() {
    const int32_t count = std::max(4, std::min(kMaxBands, steppedOf(Bands)));
    // The edges can be modulated by the matrix, two octaves either way at
    // full depth.
    const float low = mod[DstBandLow] == 0.0f ? paramOf(LowHz)
                                              : clampf(paramOf(LowHz) * std::exp2(mod[DstBandLow] * 2.0f), 40.0f, 600.0f);
    const float high = mod[DstBandHigh] == 0.0f ? paramOf(HighHz)
                                                : clampf(paramOf(HighHz) * std::exp2(mod[DstBandHigh] * 2.0f), 1500.0f, 16000.0f);
    const float q = paramOf(BandQ);
    if (count == lastCount && std::fabs(low - lastLow) < 0.5f && std::fabs(high - lastHigh) < 0.5f &&
        std::fabs(q - lastQ) < 0.002f) {
        return;
    }
    lastCount = count;
    lastLow = low;
    lastHigh = high;
    lastQ = q;
    bandCount = count;
    const float ratio = std::log(high / low) / static_cast<float>(count - 1);
    for (int i = 0; i < count; ++i) {
        const float hz = low * std::exp(ratio * static_cast<float>(i));
        bands[i].centre = hz;
    }
}

// A band should be about as wide as the gap to its neighbour. Narrower
// leaves holes, wider makes the bands overlap and sound muddy. So resonance
// comes from how many bands cover how many octaves, and the knob adjusts it.
float Cipher::bandResonance(float q) const {
    // The edges the bank was actually laid out on, including modulation.
    const float octaves = std::log2(std::fmax(1.01f, lastHigh / lastLow));
    const float perOctave = static_cast<float>(bandCount - 1) / std::fmax(0.5f, octaves);
    const float wanted = std::fmax(0.7f, perOctave * 1.45f * (0.45f + 1.1f * q));
    return clampf((2.0f - 1.0f / wanted) / 1.96f, 0.0f, 0.995f);
}

void Cipher::reset() {
    for (auto &v : voices) { v.used = false; v.gate = false; v.amp.kill(); }
    for (int i = 0; i < kMaxBands; ++i) {
        bands[i].analysis1.reset(); bands[i].analysis2.reset();
        bands[i].synthesis1.reset(); bands[i].synthesis2.reset();
        bands[i].envelope = 0.0f;
        bands[i].held = 0.0f;
    }
    sibilanceFilter.reset();
    sibilanceShaper.reset();
    for (auto &e : eg) e.reset();
    feedbackSample = 0.0f;
    loudness = brightness = 0.0f;
    quietBlocks = 0;
    asleep = false;
}

void Cipher::noteOn(uint8_t note, uint8_t velocity) {
    // The two mod envelopes belong to the machine, not a voice, so the first
    // note of a phrase starts them and notes added to a held chord don't.
    // tools/modsource_test.sh checks they run.
    bool held = false;
    for (const auto &cand : voices) if (cand.used && cand.gate) { held = true; break; }
    if (!held) for (auto &e : eg) e.retrigger();

    Voice *v = nullptr;
    for (auto &cand : voices) if (!cand.used) { v = &cand; break; }
    if (v == nullptr) {
        v = &voices[0];
        for (auto &cand : voices) if (!cand.gate) { v = &cand; break; }
    }
    const bool wasIdle = !v->used;
    v->used = true;
    v->gate = true;
    v->note = note;
    v->bend = 0.0f;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->velGain = velocityGain(v->velocity, targetOf(VelocityAmount));
    v->key01 = clampf((static_cast<float>(note) - 24.0f) / 72.0f, 0.0f, 1.0f);
    v->target = noteHz(static_cast<float>(note));
    if (wasIdle) v->freq = v->target;
    v->amp.set(0.0f, targetOf(AmpAttack), targetOf(AmpDecay), targetOf(AmpSustain), targetOf(AmpRelease), false);
    v->amp.retrigger();
}

void Cipher::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
        }
    }
    bool held = false;
    for (const auto &cand : voices) if (cand.used && cand.gate) { held = true; break; }
    if (!held) for (auto &e : eg) e.release();
}

void Cipher::allNotesOff() {
    for (auto &v : voices) { v.gate = false; v.amp.release(); }
    for (auto &e : eg) e.release();
}

void Cipher::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Cipher::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Cipher::pitchBend(int16_t value14) {
    bendSemis = (static_cast<float>(value14) / 8192.0f) * paramOf(BendRange);
}

void Cipher::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}
void Cipher::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

float Cipher::sourceValue(int32_t src) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    case SrcPressure: return pressure;
    case SrcVelocity: {
        float v = 0.0f;
        for (const auto &voice : voices) if (voice.used && voice.gate) v = std::fmax(v, voice.velocity);
        return v;
    }
    case SrcKeyTrack: {
        for (const auto &voice : voices) if (voice.used && voice.gate) return voice.key01;
        return 0.0f;
    }
    case SrcEg1: return eg[0].value();
    case SrcEg2: return eg[1].value();
    case SrcLfo1: return lfoValue[0];
    case SrcLfo2: return lfoValue[1];
    // The modulator's loudness, brightness and pitch are also mod sources.
    case SrcLoudness: return loudness;
    case SrcBrightness: return brightness;
    case SrcPitchTrack: return clampf((trackedNote - 36.0f) / 48.0f, 0.0f, 1.0f);
    default: return 0.0f;
    }
}

void Cipher::applyMatrix() {
    for (int32_t d = 0; d < DestCount; ++d) mod[d] = 0.0f;
    for (int m = 0; m < kMatrixSlots; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        const int32_t src = steppedOf(base + XSrc);
        const int32_t dst = steppedOf(base + XDest);
        if (src == SrcOff || dst == DstOff) continue;
        mod[dst] += sourceValue(src) * paramOf(base + XDepth);
    }
}

// Which analysis band drives this synthesis band.
int32_t Cipher::mappedBand(int32_t band) const {
    const int32_t n = bandCount;
    const int32_t last = n - 1;
    switch (steppedOf(RemapMode)) {
    case MapReverse: return last - band;
    case MapMirror: return band < n / 2 ? band * 2 : last - (band - n / 2) * 2;
    case MapOddEven: return band < n / 2 ? band * 2 : (band - n / 2) * 2 + 1;
    case MapFold: return band < n / 2 ? band : last - band;
    case MapShuffle: return shuffleMap[band];
    default: return band;
    }
}

float Cipher::carrierSample(Voice &v, float glideK, float detuneMul, float pitchScale, int32_t waveA,
                            int32_t waveB, float mix, float pw, float sub) {
    v.freq += (v.target - v.freq) * glideK;
    const float f = v.freq * pitchScale * noteBendMul(v);
    const float inc = f * invSampleRate;
    v.phaseA += inc;
    if (v.phaseA >= 1.0f) v.phaseA -= 1.0f;
    v.phaseB += inc * detuneMul;
    if (v.phaseB >= 1.0f) v.phaseB -= 1.0f;
    v.phaseSub += inc * 0.5f;
    if (v.phaseSub >= 1.0f) v.phaseSub -= 1.0f;

    auto shape = [&](int32_t wave, float phase) -> float {
        switch (wave) {
        case WavePulse: return phase < pw ? 1.0f : -1.0f;
        case WaveSuper: {
            // Three slightly detuned saws. A dense carrier works best since
            // the filters shape it anyway.
            float s = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float p = std::fmod(phase * (1.0f + 0.004f * static_cast<float>(i - 1)) + 0.31f * i, 1.0f);
                s += 2.0f * p - 1.0f;
            }
            return s * 0.4f;
        }
        case WaveNoise: {
            rngState = rngState * 1664525u + 1013904223u;
            return (static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f;
        }
        case WaveRing: return (2.0f * phase - 1.0f) * std::sin(6.2831853f * phase * 3.0f);
        default: return 2.0f * phase - 1.0f;
        }
    };
    const float a = shape(waveA, v.phaseA);
    const float b = shape(waveB, v.phaseB);
    const float subOut = (v.phaseSub < 0.5f ? 1.0f : -1.0f) * sub;
    return a * (1.0f - mix) + b * mix + subOut;
}

bool Cipher::render(float *L, float *R, int32_t frames) {
    params_.tick();
    applyMatrix(); // first, since the band edges can be modulated
    rebuildBands();

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
    // Only stepped in the sample loop if the matrix uses one. See Filament.
    bool egWanted = false;
    for (int m = 0; m < kMatrixSlots && !egWanted; ++m) {
        const int base = MatrixBase + m * kMatrixParams;
        if (steppedOf(base + XDest) == DstOff) continue;
        const int32_t src = steppedOf(base + XSrc);
        egWanted = src == SrcEg1 || src == SrcEg2;
    }

    // The shuffle is only rebuilt when the seed or band count changes, so it
    // stays stable.
    const int32_t seed = steppedOf(Seed);
    if (seed != shuffleSeed || lastCount != bandCount) {
        shuffleSeed = seed;
        for (int i = 0; i < bandCount; ++i) shuffleMap[i] = i;
        uint32_t s = static_cast<uint32_t>(seed) * 2654435761u + 12345u;
        for (int i = bandCount - 1; i > 0; --i) {
            s = s * 1664525u + 1013904223u;
            const int j = static_cast<int>((s >> 16) % static_cast<uint32_t>(i + 1));
            std::swap(shuffleMap[i], shuffleMap[j]);
        }
    }

    const int32_t role = steppedOf(RoleMode);
    const bool twoPole = steppedOf(Slope) != 0;
    const float attack = paramOf(Attack), release = paramOf(Release);
    const float smear = clampf(paramOf(Smear) + mod[DstSmear], -1.0f, 1.0f);
    const float q = clampf(paramOf(BandQ) + mod[DstQ], 0.0f, 1.0f);
    const float shift = clampf(paramOf(Shift) + mod[DstShift] * 12.0f, -24.0f, 24.0f);
    const float stretch = clampf(paramOf(Stretch) + mod[DstStretch], -1.0f, 1.0f);
    const float remapAmount = clampf(paramOf(RemapAmount) + mod[DstRemapAmount], 0.0f, 1.0f);
    const bool freeze = steppedOf(Freeze) != 0;
    const float freezeMorph = clampf(paramOf(FreezeMorph) + mod[DstFreezeMorph], 0.0f, 1.0f);
    // Per sample, since it's applied every sample. A per-block value here
    // would make the freeze decay 64 times too fast.
    const float freezeDecay = std::exp(-1.0f / (paramOf(FreezeDecay) * sampleRate));
    const float gate = clampf(paramOf(Gate) + mod[DstGate], 0.0f, 1.0f);
    const float gateDepth = paramOf(GateDepth);
    const float sibilance = paramOf(Sibilance);
    const float sibLevel = paramOf(SibilanceLevel);
    const float feedback = clampf(paramOf(Feedback) + mod[DstFeedback], 0.0f, 0.95f);
    const float fbTone = paramOf(FeedbackTone);
    const int32_t waveA = steppedOf(CarrierWaveA), waveB = steppedOf(CarrierWaveB);
    const float mix = clampf(paramOf(CarrierMix) + mod[DstCarrierMix], 0.0f, 1.0f);
    const float detune = paramOf(Detune), pw = paramOf(PulseWidth), sub = paramOf(SubLevel);
    // The second oscillator's detune and the glide coefficient, worked out
    // once a block.
    const float detuneMul = std::exp2(detune / 1200.0f);
    const float glideSeconds = paramOf(Glide);
    const float glideK = glideSeconds <= 0.002f ? 1.0f
                                                : clampf(invSampleRate / glideSeconds, 0.0f, 1.0f);
    const float noiseLevel = clampf(paramOf(NoiseLevel) + mod[DstNoise], 0.0f, 1.0f);
    const float carrierDrive = paramOf(CarrierDrive);
    const float dry = paramOf(Dry), wet = paramOf(Wet);
    const float drive = clampf(paramOf(Drive) + mod[DstDrive], 0.0f, 1.0f);
    const float carrierDriveK = 1.0f + carrierDrive * 6.0f;
    const float carrierDriveNorm = kCarrierNominal / std::tanh(kCarrierNominal * carrierDriveK);
    const float driveK = 1.0f + drive * 8.0f;
    const float driveNorm = kNominal / std::tanh(kNominal * driveK);
    // Clamped to the knob's own maximum.
    const float volume = clampf(paramOf(Volume) + mod[DstVolume], 0.0f, kVolumeMax);
    const float pan = clampf(paramOf(Pan) + mod[DstPan], -1.0f, 1.0f);
    // Pan and drive compensation are worked out once a block, not per
    // sample.
    const float panAngle = (pan + 1.0f) * 0.25f * kPiF;
    const float panL = std::cos(panAngle) * 1.4142f, panR = std::sin(panAngle) * 1.4142f;
    const bool track = steppedOf(PitchTrack) != 0;
    const float trackAmount = paramOf(TrackAmount);
    const float pitchScale = std::pow(2.0f, (bendSemis + paramOf(Octave) * 12.0f + paramOf(Transpose) +
                                             paramOf(Fine) * 0.01f + mod[DstCarrierPitch] * 12.0f) / 12.0f);

    sibilanceFilter.set(paramOf(SibilanceHz), 0.4f);
    sibilanceShaper.set(paramOf(SibilanceHz), 0.25f);

    // Band tuning for the synthesis side: shifted, stretched or both. The
    // analysis bank stays put, so a shift moves the formants and not the
    // whole sound.
    //
    // Only redone when something it depends on changed, since it's a lot of
    // tan, pow and exp calls at 40 bands.
    const float bandKey[] = {static_cast<float>(bandCount), shift, stretch, q, smear, attack, release,
                             lastLow, lastHigh};
    bool bandsMoved = false;
    for (size_t k = 0; k < sizeof(bandKey) / sizeof(bandKey[0]); ++k) {
        if (bandKey[k] != lastBandKey[k]) { bandsMoved = true; lastBandKey[k] = bandKey[k]; }
    }
    for (int i = 0; bandsMoved && i < bandCount; ++i) {
        const float t = bandCount > 1 ? static_cast<float>(i) / static_cast<float>(bandCount - 1) : 0.0f;
        const float warp = 1.0f + stretch * (t - 0.5f) * 1.6f;
        const float hz = clampf(bands[i].centre * std::pow(2.0f, shift / 12.0f) * warp, 20.0f, sampleRate * 0.45f);
        const float res = bandResonance(q);
        bands[i].synthesis1.set(hz, res);
        bands[i].synthesis2.set(hz, res);
        bands[i].analysis1.set(bands[i].centre, res);
        bands[i].analysis2.set(bands[i].centre, res);
        // Smear: the release time is scaled across the bank, so one end of
        // the spectrum fades before the other.
        const float spread = std::pow(4.0f, smear * (t - 0.5f) * 2.0f);
        bands[i].attackCoeff = 1.0f - std::exp(-1.0f / std::fmax(1.0f, attack * sampleRate));
        bands[i].releaseCoeff = 1.0f - std::exp(-1.0f / std::fmax(1.0f, release * spread * sampleRate));
    }

    const InputBus &bus = InputBus::get();
    const float *in = bus.live() ? bus.block() : nullptr;
    float sumLoud = 0.0f, sumBright = 0.0f, sumWeight = 0.0f;

    // With no note, no input and an empty bank, skip the work. Otherwise the
    // whole filter bank runs on zeros, which is expensive.
    //
    // Empty means: no voice, no input over -120 dB, every band's envelope
    // and frozen hold below that too, and two quiet output blocks. The
    // filters are cleared on the way to sleep, so waking up is the same as
    // after a reset.
    bool anyVoice = false;
    for (const auto &v : voices) anyVoice = anyVoice || v.used;
    float inputPeak = 0.0f;
    if (in != nullptr) {
        for (int32_t n = 0; n < frames * 2; ++n) inputPeak = std::fmax(inputPeak, std::fabs(in[n]));
    }
    bool bankQuiet = true;
    for (int i = 0; i < bandCount && bankQuiet; ++i) bankQuiet = bands[i].envelope < 1e-6f && bands[i].held < 1e-6f;
    if (!anyVoice && inputPeak < 1e-6f && bankQuiet && quietBlocks >= 2) {
        if (!asleep) {
            for (int i = 0; i < kMaxBands; ++i) {
                Band &b = bands[i];
                b.analysis1.reset(); b.analysis2.reset();
                b.synthesis1.reset(); b.synthesis2.reset();
                b.envelope = b.held = 0.0f;
            }
            sibilanceFilter.reset();
            sibilanceShaper.reset();
            sibilanceEnv = feedbackLp = feedbackSample = 0.0f;
            loudness = brightness = 0.0f;
            asleep = true;
        }
        if (egWanted) for (int32_t n = 0; n < frames; ++n) { eg[0].next(); eg[1].next(); }
        for (int32_t n = 0; n < frames; ++n) L[n] = R[n] = 0.0f;
        return true;
    }
    asleep = false;

    for (int32_t n = 0; n < frames; ++n) {
        if (egWanted) { eg[0].next(); eg[1].next(); }
        const float external = in != nullptr ? 0.5f * (in[static_cast<size_t>(n) * 2] + in[static_cast<size_t>(n) * 2 + 1]) : 0.0f;

        // The carrier: the internal oscillators, or the input if the roles
        // are swapped.
        float carrier = 0.0f;
        float ampSum = 0.0f;
        for (auto &v : voices) {
            if (!v.used) continue;
            const float env = v.amp.next();
            if (!v.gate && env < 0.0003f) { v.used = false; continue; }
            if (track && trackedHz > 20.0f) {
                v.target = trackedHz * trackAmount + noteHz(static_cast<float>(v.note)) * (1.0f - trackAmount);
            } else {
                v.target = noteHz(static_cast<float>(v.note));
            }
            carrier += carrierSample(v, glideK, detuneMul, pitchScale, waveA, waveB, mix, pw, sub) *
                       env * v.velGain;
            ampSum += env;
        }
        if (noiseLevel > 0.0f) {
            rngState = rngState * 1664525u + 1013904223u;
            carrier += ((static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f) * noiseLevel;
        }
        if (carrierDrive > 0.0f) {
            // The carrier's saturation, normalised on its nominal level so
            // turning it up doesn't make the output quieter.
            carrier = std::tanh(carrier * carrierDriveK) * carrierDriveNorm;
        }
        carrier *= 0.5f;

        float modulator = external;
        float source = carrier;
        if (role == InputIsCarrier) {
            modulator = carrier;
            source = external;
        }
        if (feedback > 0.0f) {
            feedbackLp += (feedbackSample - feedbackLp) * (0.02f + 0.9f * fbTone);
            modulator += feedbackLp * feedback;
        }

        // Analyse, map, synthesise.
        float out = 0.0f;
        float loud = 0.0f, bright = 0.0f, weight = 0.0f;
        for (int i = 0; i < bandCount; ++i) {
            Band &b = bands[i];
            // Normalised, so a band measures how loud the modulator is in its
            // range regardless of how narrow the band is.
            float a = b.analysis1.bandpass(modulator) * b.analysis1.bandNorm();
            if (twoPole) a = b.analysis2.bandpass(a) * b.analysis2.bandNorm();
            const float rectified = std::fabs(a);
            const float coeff = rectified > b.envelope ? b.attackCoeff : b.releaseCoeff;
            b.envelope += (rectified - b.envelope) * coeff;
            loud += b.envelope;
            bright += b.envelope * static_cast<float>(i);
            weight += 1.0f;
        }
        for (int i = 0; i < bandCount; ++i) {
            Band &b = bands[i];
            const int32_t from = mappedBand(i);
            const float direct = bands[i].envelope;
            const float remapped = bands[from < 0 ? 0 : (from >= bandCount ? bandCount - 1 : from)].envelope;
            float amount = direct + (remapped - direct) * remapAmount;
            if (freeze) {
                b.held = b.held * freezeDecay;
                if (b.held < amount) b.held = amount;
                amount = b.held + (amount - b.held) * (1.0f - freezeMorph);
            } else {
                b.held = amount;
            }
            if (gate > 0.0f) {
                const float threshold = gate * 0.25f;
                if (amount < threshold) amount *= 1.0f - gateDepth;
            }
            // The same normalisation on the way out. `Svf::bandpass` has a
            // peak gain of Q, and Q depends on the band count, so without
            // `bandNorm()` the band count would change the level by up to
            // 60 dB.
            float s = b.synthesis1.bandpass(source) * b.synthesis1.bandNorm();
            if (twoPole) s = b.synthesis2.bandpass(s) * b.synthesis2.bandNorm();
            out += s * amount * kBandMakeup;
        }
        if (weight > 0.0f) {
            sumLoud += loud / weight;
            sumBright += bandCount > 1 ? (bright / std::fmax(1e-6f, loud)) / static_cast<float>(bandCount - 1) : 0.0f;
            sumWeight += 1.0f;
        }

        // Sibilance: without a path for consonants every "s" would drop out.
        // Noise is added when the top of the modulator is loud enough.
        if (sibilance > 0.0f) {
            const float hiss = sibilanceFilter.highpass(modulator);
            const float level = std::fabs(hiss);
            sibilanceEnv += (level - sibilanceEnv) * (level > sibilanceEnv ? 0.02f : 0.0008f);
            // Turning the knob up gives more sibilance (it lowers the
            // threshold).
            if (sibilanceEnv > (1.0f - sibilance) * 0.05f) {
                rngState = rngState * 1664525u + 1013904223u;
                const float noise = (static_cast<float>((rngState >> 9) & 0xffff) / 32768.0f) - 1.0f;
                // Band-passed around where an "s" sits, and quiet enough to
                // stay under the vocoder. Plain or high-passed white noise
                // is far too bright and drowns out the vocoder.
                const float shaped =
                    sibilanceShaper.bandpass(noise) * sibilanceShaper.bandNorm();
                out += shaped * sibilanceEnv * sibLevel * kSibilanceMakeup;
            }
        }

        // Pitch tracking: zero crossings over a window. Rough compared to
        // autocorrelation but far cheaper, and good enough to steer a
        // carrier.
        if (track) {
            if ((zeroPrev <= 0.0f) != (modulator <= 0.0f)) ++zeroCount;
            zeroPrev = modulator;
            if (++zeroWindow >= 1024) {
                const float hz = static_cast<float>(zeroCount) * sampleRate / (2.0f * 1024.0f);
                if (hz > 40.0f && hz < 2000.0f) {
                    const float glideK = clampf(1024.0f / (paramOf(TrackGlide) * sampleRate), 0.0f, 1.0f);
                    trackedHz += (hz - trackedHz) * glideK;
                    trackedNote = 69.0f + 12.0f * std::log2(std::fmax(20.0f, trackedHz) / 440.0f);
                }
                zeroCount = 0;
                zeroWindow = 0;
            }
        }

        float x = out * wet + (role == InputIsCarrier ? external : carrier) * dry;
        if (drive > 0.0f) {
            // Normalised on the nominal level so drive changes the tone, not
            // the level.
            x = std::tanh(x * driveK) * driveNorm;
        }
        feedbackSample = x;
        x *= volume * kHouse;
        L[n] = x * panL;
        R[n] = x * panR;
    }

    if (sumWeight > 0.0f) {
        loudness = clampf(sumLoud / sumWeight * 8.0f, 0.0f, 1.0f);
        brightness = clampf(sumBright / sumWeight, 0.0f, 1.0f);
    }
    if (anyVoice || inputPeak >= 1e-6f) {
        quietBlocks = 0;
    } else {
        float peak = 0.0f;
        for (int32_t n = 0; n < frames; ++n) peak = std::fmax(peak, std::fmax(std::fabs(L[n]), std::fabs(R[n])));
        quietBlocks = peak < 1e-6f ? quietBlocks + 1 : 0;
    }
    return true;
}

} // namespace acidulous::machine
