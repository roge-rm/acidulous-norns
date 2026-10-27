#include "Cumulus.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, used as its nominal
// level. Measured after volume, peak -8.0 dB. Set it too high and the drive
// knob acts like a volume knob.
constexpr float kNominal = 0.12f;

using cumulus::CloudSet;
using cumulus::CloudTable;

namespace {
constexpr float kSemitone = 1.0594630943592953f;
} // namespace

Cumulus::Cumulus() { initParams(); }

namespace {
const ParamDef kBase[Cumulus::MacroBase] = {
        // --- the spectrum: these rebuild the tables ---------------------
        {"partials", 1.0f, 128.0f, 48.0f, Curve::Linear, 0, ""},
        {"tilt", -24.0f, 6.0f, -9.0f, Curve::Linear, 0, "dB/oct"},
        {"odd", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"comb", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"combperiod", 1.0f, 12.0f, 3.0f, Curve::Linear, 0, ""},
        {"vowel", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"vowelamount", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bandwidth", 2.0f, 600.0f, 40.0f, Curve::Exponential, 0, "cents"},
        {"bwscale", 0.5f, 3.0f, 1.0f, Curve::Linear, 0, ""},
        {"stretch", -0.02f, 0.06f, 0.0f, Curve::Linear, 0, ""},
        {"seed", 0.0f, 15.0f, 1.0f, Curve::Stepped, 16, ""},
        {"btilt", -18.0f, 18.0f, 6.0f, Curve::Linear, 0, "dB/oct"},
        {"bbandwidth", -200.0f, 400.0f, 60.0f, Curve::Linear, 0, "cents"},
        {"bstretch", -0.04f, 0.04f, 0.0f, Curve::Linear, 0, ""},
        {"bcomb", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bvowel", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bodd", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // --- live -------------------------------------------------------
        {"morph", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"morphkey", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"shimmer", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"shimmerint", 0.0f, 3.0f, 1.0f, Curve::Stepped, 4, ""}, // 5th, 8ve, 8ve+5th, 2 8ves
        {"width", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"scatter", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"drift", 0.0f, 50.0f, 6.0f, Curve::Linear, 0, "cents"},
        {"driftrate", 0.01f, 4.0f, 0.15f, Curve::Exponential, 0, "Hz"},
        {"spread", 1.0f, 3.0f, 2.0f, Curve::Stepped, 3, ""},
        {"detune", 0.0f, 50.0f, 8.0f, Curve::Linear, 0, "cents"},
        {"spreadwidth", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"cutoff", 30.0f, 18000.0f, 12000.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.1f, Curve::Linear, 0, ""},
        {"filtertype", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""},
        {"filterenv", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"filterkey", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"filterdrive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"ampattack", 0.001f, 8.0f, 0.6f, Curve::Exponential, 0, "s"},
        {"ampdecay", 0.005f, 8.0f, 1.0f, Curve::Exponential, 0, "s"},
        {"ampsustain", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
        {"amprelease", 0.005f, 12.0f, 1.6f, Curve::Exponential, 0, "s"},
        {"filtattack", 0.001f, 8.0f, 0.8f, Curve::Exponential, 0, "s"},
        {"filtdecay", 0.005f, 8.0f, 1.5f, Curve::Exponential, 0, "s"},
        {"filtsustain", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"filtrelease", 0.005f, 12.0f, 1.6f, Curve::Exponential, 0, "s"},
        {"lfo1wave", 0.0f, 8.0f, 0.0f, Curve::Stepped, 9, ""},
        {"lfo1rate", 0.01f, 20.0f, 0.12f, Curve::Exponential, 0, "Hz"},
        {"lfo1sync", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"lfo1morph", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"lfo1pitch", 0.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"lfo2wave", 0.0f, 8.0f, 0.0f, Curve::Stepped, 9, ""},
        {"lfo2rate", 0.01f, 20.0f, 0.3f, Curve::Exponential, 0, "Hz"},
        {"lfo2sync", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"lfo2cutoff", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"lfo2pan", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.5f, 1.07f, Curve::Linear, 0, ""}, // puts Init at the standard level
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"glide", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s"},
        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},
        {"fine", -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mpepressure", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
};

// The base table plus the macros' knobs and routes.
struct Table {
    ParamDef defs[Cumulus::Count];
    char names[Cumulus::kMacros * Cumulus::MacroParams][12];
    Table() {
        for (int32_t i = 0; i < Cumulus::MacroBase; ++i) defs[i] = kBase[i];
        for (int k = 0; k < Cumulus::kMacros; ++k) {
            const int32_t b = Cumulus::MacroBase + k * Cumulus::MacroParams;
            char *n = names[k * Cumulus::MacroParams];
            std::snprintf(n, 12, "mac%d", k + 1);
            defs[b] = {n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            for (int r = 0; r < 4; ++r) {
                char *d = names[k * Cumulus::MacroParams + 1 + r];
                std::snprintf(d, 12, "mac%d_d%d", k + 1, r + 1);
                defs[b + 1 + r] = {d, 0.0f, Cumulus::DestCount - 1.0f, 0.0f, Curve::Stepped, Cumulus::DestCount, ""};
                char *a = names[k * Cumulus::MacroParams + 5 + r];
                std::snprintf(a, 12, "mac%d_a%d", k + 1, r + 1);
                defs[b + 5 + r] = {a, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            }
        }
    }
};
const Table &table() {
    static const Table t;
    return t;
}

// Which param each macro destination moves in its own range; -1 for pitch.
constexpr int32_t kDestParam[Cumulus::DestCount] = {
    -1, Cumulus::Morph, Cumulus::Shimmer, Cumulus::Width, Cumulus::Scatter, Cumulus::Drift, Cumulus::Detune,
    Cumulus::SpreadWidth, Cumulus::Cutoff, Cumulus::Resonance, Cumulus::FilterEnv, Cumulus::FilterDrive,
    Cumulus::Drive, Cumulus::Volume, Cumulus::Pan, Cumulus::AmpAttack, Cumulus::AmpDecay, Cumulus::AmpRelease,
    Cumulus::FiltDecay, Cumulus::Lfo1Rate, Cumulus::Lfo1Morph, Cumulus::Lfo2Rate, Cumulus::Lfo2Cutoff,
    Cumulus::Lfo2Pan, -1,
};
} // namespace

const ParamDef *Cumulus::paramDefs(int32_t &count) const {
    count = Count;
    return table().defs;
}

float Cumulus::macroSum(int dest, bool target) const {
    float sum = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = target ? params_.target(b) : params_.get(b);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const float d = target ? params_.target(b + 1 + r) : params_.get(b + 1 + r);
            if (static_cast<int>(d + 0.5f) == dest)
                sum += knob * (target ? params_.target(b + 5 + r) : params_.get(b + 5 + r));
        }
    }
    return sum;
}

float Cumulus::moved(int32_t p, float off, bool target) const {
    const float v = target ? params_.target(p) : params_.get(p);
    if (off == 0.0f) return v;
    const ParamDef &d = params_.def(p);
    return d.map(d.unmap(v) + off);
}

int32_t Cumulus::steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }

cumulus::CloudSpec Cumulus::spec(const float *norm01, int32_t count) const {
    int32_t n = 0;
    const ParamDef *defs = paramDefs(n);
    auto at = [&](int32_t p) {
        const bool given = norm01 != nullptr && p < count && !std::isnan(norm01[p]);
        // The target value, not the smoothed one, or the build could use an
        // old value.
        return defs[p].map(given ? norm01[p] : params_.normalized(p));
    };
    cumulus::CloudSpec s;
    s.partials = static_cast<int32_t>(at(Partials) + 0.5f);
    s.tilt = at(Tilt);
    s.odd = at(Odd);
    s.comb = at(Comb);
    s.combPeriod = at(CombPeriod);
    s.formant = at(Vowel);
    s.formantAmount = at(VowelAmount);
    s.bandwidth = at(Bandwidth);
    s.bwScale = at(BandwidthScale);
    s.stretch = at(Stretch);
    s.seed = static_cast<uint32_t>(at(Seed) + 0.5f);
    s.bTilt = at(BTilt);
    s.bBandwidth = at(BBandwidth);
    s.bStretch = at(BStretch);
    s.bComb = at(BComb);
    s.bFormant = at(BVowel);
    s.bOdd = at(BOdd);
    return s;
}

void Cumulus::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.amp.setSampleRate(sampleRate);
        v.fenv.setSampleRate(sampleRate);
        v.filterL.setSampleRate(sampleRate);
        v.filterR.setSampleRate(sampleRate);
    }
    reset();
}

void Cumulus::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.amp.kill();
        v.fenv.kill();
        v.filterL.reset();
        v.filterR.reset();
    }
    // A fresh LFO, since reset() keeps the random waves' seed.
    lfo[0] = dsp::LfoGen();
    lfo[1] = dsp::LfoGen();
    lfo[0].reset(0.0f);
    lfo[1].reset(0.25f);
    // Reset the rng, since it sets where each reader starts and how it
    // drifts. Otherwise two exports of the same song would differ.
    rng = kRngSeed;
    ageCounter = 0;
    for (auto &v : voices) v.age = 0;
}

void *Cumulus::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    // Voices read directly from the old tables, so they stop with them.
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.amp.kill();
        v.fenv.kill();
    }
    void *old = const_cast<CloudSet *>(cloud);
    cloud = static_cast<const CloudSet *>(object);
    return old;
}

Cumulus::~Cumulus() {
    delete cloud;
}

Cumulus::Voice *Cumulus::allocate() {
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

void Cumulus::startVoice(Voice &v, uint8_t note, uint8_t velocity) {
    const float glide = targetOf(Glide);
    const float target = noteHz(static_cast<float>(note));
    const bool gliding = glide > 0.001f && v.used;
    v.glideFrom = gliding ? v.freq : target;
    v.glidePos = gliding ? 0.0f : 1.0f;
    v.freq = v.glideFrom;
    v.used = true;
    v.gate = true;
    v.note = note;
    v.bend = 0.0f;
    v.pressure = v.timbre = -1.0f;
    v.prsGlide = 0.0f;
    v.velocity = static_cast<float>(velocity) / 127.0f;
    v.key01 = std::clamp((static_cast<float>(note) - 24.0f) / 72.0f, 0.0f, 1.0f);
    v.zone = CloudSet::zoneFor(note);
    v.age = ++ageCounter;

    const int32_t unison = std::clamp(steppedTargetOf(Spread), 1, kUnison);
    const float detune = moved(Detune, macroSum(DstDetune, true), true);
    const float spreadWidth = moved(SpreadWidth, macroSum(DstSpreadWidth, true), true);
    const float scatter = moved(Scatter, macroSum(DstScatter, true), true);
    const int32_t size = cloud != nullptr ? cloud->tables[v.zone][0].size : 1;
    for (int i = 0; i < kUnison; ++i) {
        Reader &r = v.readers[i];
        const float offset = unison == 1 ? 0.0f : (static_cast<float>(i) / (unison - 1) * 2.0f - 1.0f);
        r.rateMul = std::pow(2.0f, offset * detune / 1200.0f);
        r.pan = i < unison ? offset * spreadWidth : 0.0f;
        // Scatter: where in the cloud this reader starts. The table is over a
        // second long, so different start points sound like different
        // textures.
        r.pos = scatter > 0.0001f ? nextRandom() * scatter * static_cast<float>(size) : 0.0f;
        r.drift = 0.0f;
        r.driftTarget = nextRandom() * 2.0f - 1.0f;
    }
    v.shimmerPos = v.readers[0].pos;
    v.amp.retrigger();
    v.fenv.retrigger();
}

void Cumulus::noteOn(uint8_t note, uint8_t velocity) {
    Voice *v = allocate();
    if (v == nullptr) return;
    startVoice(*v, note, velocity);
}

void Cumulus::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
            v.fenv.release();
        }
    }
}

void Cumulus::allNotesOff() {
    for (auto &v : voices) {
        if (!v.used) continue;
        v.gate = false;
        v.amp.release();
        v.fenv.release();
    }
}

void Cumulus::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Cumulus::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Cumulus::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Cumulus::onBlock(int64_t, int64_t, float tempo) { bpm = tempo; }

float Cumulus::readTable(const CloudTable &t, float pos) const {
    const int32_t i = static_cast<int32_t>(pos);
    const float f = pos - static_cast<float>(i);
    const float *d = t.data.data();
    // Linear interpolation. At these table lengths a voice reads about one
    // sample per sample, so the error is inaudible, and it's much cheaper
    // than cubic.
    return d[i] + (d[i + 1] - d[i]) * f;
}

void Cumulus::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Cumulus::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Cumulus::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

bool Cumulus::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (cloud == nullptr) return true;

    const float dt = 1.0f / sampleRate;
    // The macros' sums for this block, one per destination.
    float mac[DestCount]{};
    for (int k = 0; k < kMacros; ++k) {
        const int32_t base = MacroBase + k * MacroParams;
        const float knob = params_.get(base);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(params_.get(base + 1 + r) + 0.5f);
            if (dest > DstOff && dest < DestCount) mac[dest] += knob * params_.get(base + 5 + r);
        }
    }
    auto value = [&](int dest) { return moved(kDestParam[dest], mac[dest], false); };

    const float ampA = value(DstAmpAttack), ampD = value(DstAmpDecay), ampS = paramOf(AmpSustain), ampR = value(DstAmpRelease);
    const float fA = paramOf(FiltAttack), fD = value(DstFiltDecay), fS = paramOf(FiltSustain), fR = paramOf(FiltRelease);
    const float cutoff = value(DstCutoff), reso = value(DstResonance);
    const int32_t ftype = steppedOf(FilterType);
    const float fenvAmt = value(DstFilterEnv), fkey = paramOf(FilterKey), fdrive = value(DstFilterDrive);
    const float glide = paramOf(Glide);
    const float bendRange = paramOf(BendRange);
    const float tune = std::pow(2.0f, (paramOf(Octave) * 12.0f + paramOf(Transpose) + paramOf(Fine) * 0.01f + mac[DstPitch] * 24.0f) / 12.0f);
    const float velAmount = paramOf(VelocityAmount);
    const float width = value(DstWidth);
    const float shimmer = value(DstShimmer);
    const float shimmerRatio = [&] {
        switch (steppedOf(ShimmerInterval)) {
        case 0: return 1.5f;   // a fifth
        case 2: return 3.0f;   // an octave and a fifth
        case 3: return 4.0f;   // two octaves
        default: return 2.0f;  // an octave
        }
    }();
    const float driftCents = value(DstDrift), driftRate = paramOf(DriftRate);
    const float drive = value(DstDrive), volume = value(DstAmp), pan = value(DstPan);
    const int32_t unison = std::clamp(steppedOf(Spread), 1, kUnison);
    const float unisonNorm = 1.0f / std::sqrt(static_cast<float>(unison));

    // LFOs, once a block. One for the morph, one for the filter and pan.
    const float sync1 = steppedOf(Lfo1Sync) != 0 ? bpm / 60.0f : 1.0f;
    lfoValue[0] = lfo[0].advance(steppedOf(Lfo1Wave), value(DstLfo1Rate) * sync1, dt * frames, 0.0f, false);
    const float sync2 = steppedOf(Lfo2Sync) != 0 ? bpm / 60.0f : 1.0f;
    lfoValue[1] = lfo[1].advance(steppedOf(Lfo2Wave), value(DstLfo2Rate) * sync2, dt * frames, 0.0f, false);

    const float morphBase = value(DstMorph) + lfoValue[0] * value(DstLfo1Morph) + modWheel * 0.0f;
    const float lfoPitch = std::pow(2.0f, lfoValue[0] * paramOf(Lfo1Pitch) / 1200.0f);
    const float morphKey = paramOf(MorphKey);
    const float panL = std::cos((pan + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((pan + 1.0f) * 0.25f * 3.14159265f);

    for (auto &v : voices) {
        if (!v.used) continue;
        v.amp.set(0.0f, ampA, ampD, ampS, ampR, false);
        v.fenv.set(0.0f, fA, fD, fS, fR, false);

        // Per-note MPE, if sent: slide moves the morph, pressure opens the
        // filter and raises the level.
        const float slide = (v.timbre >= 0.0f ? v.timbre : 0.0f) * paramOf(MpeTimbre);
        const float prs = glidePressure(v.prsGlide, v.pressure, pressure) * paramOf(MpePressure);

        const CloudTable *frames4 = cloud->tables[v.zone];
        const int32_t size = frames4[0].size;
        // This voice's morph position: the knob, the LFO, and key tracking so
        // the top of the keyboard can use a different cloud from the bottom.
        const float m = std::clamp(morphBase + morphKey * (v.key01 - 0.5f) * 2.0f + slide, 0.0f, 1.0f) *
                        static_cast<float>(CloudSet::kFrames - 1);
        const int f0 = std::min(static_cast<int>(m), CloudSet::kFrames - 1);
        const int f1 = std::min(f0 + 1, CloudSet::kFrames - 1);
        const float mf = m - static_cast<float>(f0);
        const CloudTable &ta = frames4[f0];
        const CloudTable &tb = frames4[f1];

        // Glide, then everything that multiplies pitch.
        if (v.glidePos < 1.0f) {
            const float step = glide > 0.001f ? (dt * frames) / glide : 1.0f;
            v.glidePos = std::min(1.0f, v.glidePos + step);
            v.freq = v.glideFrom + (noteHz(static_cast<float>(v.note)) - v.glideFrom) * v.glidePos;
        } else {
            v.freq = noteHz(static_cast<float>(v.note));
        }
        const float bendMul = std::pow(2.0f, bend * bendRange / 12.0f);
        const float baseRate = v.freq * bendMul * noteBendMul(v) * tune * lfoPitch / ta.baseHz;

        const float widthOffset = width * static_cast<float>(size) * 0.25f;
        const float cutoffHz = std::clamp(
            cutoff * std::pow(2.0f, fenvAmt * 6.0f * v.fenv.value() + fkey * (v.key01 - 0.5f) * 6.0f +
                                        lfoValue[1] * value(DstLfo2Cutoff) * 4.0f + prs * 2.4f),
            30.0f, sampleRate * 0.45f);
        v.filterL.set(cutoffHz, reso, ftype, dsp::MultiFilter::Valve, fdrive);
        v.filterR.set(cutoffHz, reso, ftype, dsp::MultiFilter::Valve, fdrive);

        const float vel = velocityGain(v.velocity, velAmount) * (1.0f + prs * 0.5f);
        const float voicePan = lfoValue[1] * value(DstLfo2Pan);

        for (int32_t i = 0; i < frames; ++i) {
            const float env = v.amp.next();
            v.fenv.next();
            if (env <= 0.0000005f && !v.gate) { v.used = false; break; }
            float l = 0.0f, r = 0.0f;
            for (int u = 0; u < unison; ++u) {
                Reader &rd = v.readers[u];
                // Drift: a slow walk toward a new random detune, so the static
                // table sounds alive.
                //
                // Updated every 16 samples to save a pow per reader per
                // sample. The coefficient includes the stride so the walk
                // takes the same time.
                if ((i & 15) == 0) {
                    rd.drift += (rd.driftTarget * driftCents - rd.drift) * driftRate * dt * 96.0f;
                    if (std::fabs(rd.driftTarget * driftCents - rd.drift) < 0.05f) rd.driftTarget = nextRandom() * 2.0f - 1.0f;
                    rd.rate = baseRate * rd.rateMul * std::exp2(rd.drift * (1.0f / 1200.0f));
                }
                const float rate = rd.rate;

                float p = rd.pos;
                float pr = p + widthOffset;
                if (pr >= static_cast<float>(size)) pr -= static_cast<float>(size);
                // When the morph sits exactly on a frame (most of the time),
                // only one table is read.
                float sl, sr2;
                if (mf < 0.0005f) {
                    sl = readTable(ta, p);
                    sr2 = readTable(ta, pr);
                } else if (mf > 0.9995f) {
                    sl = readTable(tb, p);
                    sr2 = readTable(tb, pr);
                } else {
                    const float a = readTable(ta, p), b = readTable(tb, p);
                    const float ar = readTable(ta, pr), br = readTable(tb, pr);
                    sl = a + (b - a) * mf;
                    sr2 = ar + (br - ar) * mf;
                }
                const float pl = 0.5f - rd.pan * 0.5f, prr = 0.5f + rd.pan * 0.5f;
                l += sl * pl;
                r += sr2 * prr;

                rd.pos += rate;
                while (rd.pos >= static_cast<float>(size)) rd.pos -= static_cast<float>(size);
            }
            if (shimmer > 0.0001f) {
                const float s = readTable(ta, v.shimmerPos);
                l += s * shimmer * 0.7f;
                r += s * shimmer * 0.7f;
                v.shimmerPos += baseRate * shimmerRatio;
                while (v.shimmerPos >= static_cast<float>(size)) v.shimmerPos -= static_cast<float>(size);
            }
            const float g = env * vel * unisonNorm;
            l = v.filterL.process(l * g);
            r = v.filterR.process(r * g);
            const float pl = std::clamp(1.0f - voicePan, 0.0f, 2.0f);
            const float pr2 = std::clamp(1.0f + voicePan, 0.0f, 2.0f);
            L[i] += l * pl;
            R[i] += r * pr2;
        }
    }

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i] * volume, r = R[i] * volume;
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

} // namespace acidulous::machine
