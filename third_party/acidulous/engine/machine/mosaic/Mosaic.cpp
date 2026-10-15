#include "Mosaic.h"
#include <array>
#include <algorithm>
#include <engine/machine/Voices.h>
#include <engine/dsp/Lfo.h>

#include <cstdio>

namespace acidulous::machine {

using dsp::clampf;
using dsp::kPi;
using dsp::kTwoPi;
using dsp::mtof;

namespace {
// The house level. It sets where a patch sits on the volume knob, leaving
// room to move in both directions.
constexpr float kHouse = 1.92f; // set from Init, which carries no volume line of its own

// Free, then the tempo-locked LFOs' note values (dsp::Lfo).
constexpr int kSyncCount = 1 + dsp::Lfo::kRates;
inline float syncBeats(int sync) { return sync <= 0 ? 0.0f : dsp::Lfo::beatsOf(sync - 1); }

inline float rnd(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 16777216.0f;
}
} // namespace

Mosaic::Mosaic() { initParams(); }

const ParamDef *Mosaic::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static char names[Count][16];
    static bool built = false;
    if (!built) {
        auto putN = [&](int32_t i, const char *name, float mn, float mx, float df, Curve c, int32_t steps,
                        const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), "%s", name);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        auto put = [&](int32_t i, const char *fmt, int arg, float mn, float mx, float df, Curve c, int32_t steps,
                       const char *unit) {
            std::snprintf(names[i], sizeof(names[i]), fmt, arg);
            defs[i] = {names[i], mn, mx, df, c, steps, unit};
        };
        putN(LayerScan, "scan", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(ScanAmount, "scanamt", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(KeyFade, "keyfade", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "st");
        putN(VelFade, "velfade", 0.0f, 64.0f, 0.0f, Curve::Linear, 0, "");
        putN(Start, "start", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(LoopModeIndex, "loop", 0.0f, LoopModeCount - 1.0f, 0.0f, Curve::Stepped, LoopModeCount, "");
        putN(Reverse, "reverse", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        putN(EnvSourceIndex, "envfrom", 0.0f, EnvSourceCount - 1.0f, 0.0f, Curve::Stepped, EnvSourceCount, "");
        putN(FileMods, "filemod", 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, "");
        putN(GrainMode, "grain", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        putN(GrainPos, "gpos", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(GrainRate, "grate", -2.0f, 2.0f, 1.0f, Curve::Linear, 0, "");
        putN(GrainSize, "gsize", 5.0f, 500.0f, 80.0f, Curve::Exponential, 0, "ms");
        putN(GrainDensity, "gdensity", 1.0f, 120.0f, 20.0f, Curve::Exponential, 0, "");
        putN(GrainSpray, "gspray", 0.0f, 1.0f, 0.05f, Curve::Linear, 0, "");
        putN(GrainPitch, "gpitch", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "st");
        putN(AmpAttack, "a_attack", 0.001f, 10.0f, 0.002f, Curve::Exponential, 0, "s");
        putN(AmpDecay, "a_decay", 0.002f, 15.0f, 1.0f, Curve::Exponential, 0, "s");
        putN(AmpSustain, "a_sustain", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
        putN(AmpRelease, "a_release", 0.002f, 15.0f, 0.15f, Curve::Exponential, 0, "s");
        putN(FilterType, "f_type", 0.0f, dsp::MultiFilter::TypeCount - 1.0f, 3.0f, Curve::Stepped,
             dsp::MultiFilter::TypeCount, "");
        putN(FilterFreq, "f_freq", 20.0f, 20000.0f, 18000.0f, Curve::Exponential, 0, "Hz");
        putN(FilterRes, "f_res", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterEnv, "f_env", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterKey, "f_key", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(FilterAttack, "f_attack", 0.001f, 10.0f, 0.002f, Curve::Exponential, 0, "s");
        putN(FilterDecay, "f_decay", 0.002f, 15.0f, 0.8f, Curve::Exponential, 0, "s");
        putN(FilterSustain, "f_sustain", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(FilterRelease, "f_release", 0.002f, 15.0f, 0.3f, Curve::Exponential, 0, "s");
        putN(Coarse, "coarse", -24.0f, 24.0f, 0.0f, Curve::Stepped, 49, "st");
        putN(Fine, "fine", -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "");
        putN(Glide, "glide", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s");
        putN(GlideMode, "glidemode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, "");
        putN(BendRange, "bend", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, "st");
        putN(Octave, "octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, "");
        putN(Transpose, "transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, "st");
        putN(VoiceMode, "voicemode", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, "");
        // Up to 1.5, like the other machines. Values are saved normalised, so
        // changing this range rescales saved volumes.
        putN(Volume, "volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, "");
        putN(Pan, "pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        putN(VelocityAmount, "velamt", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, "");
        putN(VelToFilter, "veltofilter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        for (int l = 0; l < kLfos; ++l) {
            const int32_t b = LfoBase + l * LfoParams;
            const int n = l + 1;
            put(b + LWave, "l%d_wave", n, 0.0f, dsp::LfoGen::WaveCount - 1.0f, 0.0f, Curve::Stepped,
                dsp::LfoGen::WaveCount, "");
            put(b + LRate, "l%d_rate", n, 0.01f, 40.0f, 3.0f, Curve::Exponential, 0, "Hz");
            put(b + LSync, "l%d_sync", n, 0.0f, kSyncCount - 1.0f, 0.0f, Curve::Stepped, kSyncCount, "");
            put(b + LDelay, "l%d_delay", n, 0.0f, 5.0f, 0.0f, Curve::Linear, 0, "s");
            put(b + LPhase, "l%d_phase", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + LKeySync, "l%d_keysync", n, 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, "");
        }
        for (int e = 0; e < kModEgs; ++e) {
            const int32_t b = EgBase + e * EgParams;
            const int n = e + 1;
            put(b + EAttack, "e%d_attack", n, 0.001f, 10.0f, 0.002f, Curve::Exponential, 0, "s");
            put(b + EDecay, "e%d_decay", n, 0.002f, 15.0f, 0.5f, Curve::Exponential, 0, "s");
            put(b + ESustain, "e%d_sustain", n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
            put(b + ERelease, "e%d_release", n, 0.002f, 15.0f, 0.3f, Curve::Exponential, 0, "s");
        }
        for (int m = 0; m < kMatrixSlots; ++m) {
            const int32_t b = MatrixBase + m * MatrixParams;
            const int n = m + 1;
            put(b + XSrc, "m%02d_src", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XSrc2, "m%02d_src2", n, 0.0f, SourceCount - 1.0f, 0.0f, Curve::Stepped, SourceCount, "");
            put(b + XDest, "m%02d_dest", n, 0.0f, DestCount - 1.0f, 0.0f, Curve::Stepped, DestCount, "");
            put(b + XDepth, "m%02d_depth", n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, "");
        }
        putN(MpeTimbre, "mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        putN(MpePressure, "mpepressure", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, "");
        built = true;
    }
    count = Count;
    return defs;
}

void Mosaic::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (auto &v : voices) {
        v.amp.setSampleRate(sampleRate);
        v.filterEg.setSampleRate(sampleRate);
        for (auto &e : v.modEg) e.setSampleRate(sampleRate);
        v.filter.setSampleRate(sampleRate);
    }
    reset();
}

void Mosaic::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        // Back to -1 (never set), so a reset voice follows the channel again.
        v.pressure = v.timbre = -1.0f;
        v.prsGlide = 0.0f;
        v.layerCount = 0;
        v.amp.kill();
        v.filterEg.kill();
        for (auto &e : v.modEg) e.kill();
        v.filter.reset();
        v.filterR.reset();
        for (auto &g : v.grain) g.active = false;
        for (auto &m : v.mod) m = 0.0f;
    }
    modWheel = pressure = bend = 0.0f;
    // SoundFont default modulators read volume from CC7 and CC11 and pan from
    // CC10, so they start at a synth's power-up values instead of zero.
    // Otherwise an untouched instrument would be silent and hard left.
    for (auto &v : cc) v = 0.0f;
    cc[7] = 1.0f;
    cc[11] = 1.0f;
    cc[10] = 64.0f / 127.0f;
}

void *Mosaic::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    // Voices hold pointers into the old map, so they stop with it.
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.layerCount = 0;
        v.amp.kill();
        for (auto &g : v.grain) g.active = false;
    }
    void *old = const_cast<SampleMap *>(map);
    map = static_cast<const SampleMap *>(object);
    return old;
}

Mosaic::Voice *Mosaic::allocate() {
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

void Mosaic::noteOn(uint8_t note, uint8_t velocity) {
    if (map == nullptr || map->zones.empty()) return;
    const int mode = stepOf(VoiceMode);
    Voice *vp = (mode == 1 || mode == 2) ? &voices[0] : allocate();
    if (vp == nullptr) return;
    Voice &v = *vp;
    const bool wasSounding = v.used;
    const bool wasHeld = v.used && v.gate;
    const bool retrigger = !(mode == 2 && wasHeld);

    const float glideSeconds = targetOf(Glide);
    const bool gliding = glideSeconds > 0.001f && v.used && (stepOf(GlideMode) == 0 || v.gate);
    v.glideFrom = gliding ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = gliding ? 0.0f : 1.0f;
    v.freq = v.glideFrom;
    v.used = true;
    // A new note has no pressure of its own yet.
    v.pressure = v.timbre = -1.0f;
    v.prsGlide = 0.0f;
    v.gate = true;
    v.note = note;
    v.bend = 0.0f;
    v.velocity = velocity;
    v.age = ageCounter++;
    v.random = rnd(v.rng) * 2.0f - 1.0f;
    // Evaluate the matrix for this note first, because scan and start read
    // `v.mod` below. The envelopes are read as they are, before being
    // triggered.
    evalMatrix(v);

    // Which layers sound. Scan moves the velocity axis away from the played
    // velocity, so a knob can walk through the zones.
    const float scanAmount = clampf(targetOf(ScanAmount), 0.0f, 1.0f);
    const float scanValue = clampf(targetOf(LayerScan) + v.mod[DstScan], 0.0f, 1.0f) * 127.0f;

    // Scan spans velocities 1 to 127. At 0 it would miss every zone and the
    // note wouldn't sound.
    const float scanned = 1.0f + scanValue * 126.0f / 127.0f;
    const float effectiveVel = static_cast<float>(velocity) * (1.0f - scanAmount) + scanned * scanAmount;

    int32_t zones[kZonesPerVoice];
    float gains[kZonesPerVoice];
    const int32_t n = map->select(note, static_cast<int>(effectiveVel + 0.5f), targetOf(KeyFade), targetOf(VelFade),
                                  zones, gains, kZonesPerVoice);
    v.layerCount = n;
    const float startAt = clampf(targetOf(Start) + v.mod[DstStart], 0.0f, 0.999f);
    const bool reversed = targetOf(Reverse) >= 0.5f;
    for (int32_t i = 0; i < n; ++i) {
        const MapZone &z = map->zones[static_cast<size_t>(zones[i])];
        Layer &L = v.layer[i];
        const SampleData *newSample = &map->samples[static_cast<size_t>(z.sample)];
        const float newGain = gains[i] * z.gain;
        // Defer whenever a sounding layer changes sample, even without a
        // retrigger. A legato line can still cross into another zone, and
        // swapping the sample under a running playhead clicks.
        const bool defer = wasSounding && L.sample != nullptr && !L.finished &&
                           (retrigger || L.sample != newSample);
        if (!defer) {
            L.zone = &z;
            L.sample = newSample;
            L.gain = newGain;
            L.pan = z.pan;
        }
        L.finished = false;
        // When reversed, `start` counts in from the end, so turning reverse on
        // plays the whole sample backwards.
        if (defer && !retrigger) {
            // Same note carrying on: keep the position and let the fade
            // cover the sample change.
            L.pendingPos = L.pos;
            L.pendingZone = &z;
            L.pendingSample = newSample;
            L.pendingGain = newGain;
            L.pendingPan = z.pan;
        }
        if (retrigger) {
            const auto frames = static_cast<double>(L.sample->frames);
            double want = (reversed ? 1.0 - startAt : startAt) * frames;
            if (reversed) want = std::min(want, frames - 2.0);
            if (defer) {
                // The voice is still sounding. Leave the playhead and sample
                // where they are and let `fade` take it down. Both move once
                // it's silent.
                L.pendingPos = want;
                L.pendingZone = &z;
                L.pendingSample = newSample;
                L.pendingGain = newGain;
                L.pendingPan = z.pan;
            } else {
                L.pos = want;
                L.pendingPos = -1.0;
                L.fade = 1.0f;
                // Ramp in unless the note starts at the very front of the
                // sample.
                L.fadeIn = startAt <= 0.0001f ? 1.0f : 0.0f;
            }
        }
    }

    if (!retrigger) return;
    // The envelope can come from the panel or from the file.
    const bool fromFile = stepOf(EnvSourceIndex) == EnvFile && n > 0 && v.layer[0].zone->hasEnvelope;
    if (fromFile) {
        const MapZone &z = *v.layer[0].zone;
        v.amp.set(0.0f, z.attack, z.decay, z.sustain, z.release, false);
    } else {
        v.amp.set(0.0f, targetOf(AmpAttack), targetOf(AmpDecay), targetOf(AmpSustain), targetOf(AmpRelease), false);
    }
    v.amp.trigger();
    v.filterEg.set(0.0f, targetOf(FilterAttack), targetOf(FilterDecay), targetOf(FilterSustain), targetOf(FilterRelease), false);
    v.filterEg.trigger();
    for (int e = 0; e < kModEgs; ++e) {
        const int32_t b = EgBase + e * EgParams;
        v.modEg[e].set(0.0f, targetOf(b + EAttack), targetOf(b + EDecay), targetOf(b + ESustain), targetOf(b + ERelease), false);
        v.modEg[e].trigger();
    }
    for (int l = 0; l < kLfos; ++l) {
        const int32_t b = LfoBase + l * LfoParams;
        if (targetOf(b + LKeySync) >= 0.5f) v.lfo[l].trigger(targetOf(b + LPhase), targetOf(b + LDelay));
    }
    v.grainOffset = 0.0;
    v.grainTimer = 0.0f;
    for (auto &g : v.grain) g.active = false;
}

void Mosaic::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
            v.filterEg.release();
            for (auto &e : v.modEg) e.release();
        }
    }
}

void Mosaic::allNotesOff() {
    for (auto &v : voices) {
        v.gate = false;
        v.used = false;
        v.layerCount = 0;
        v.amp.kill();
        for (auto &g : v.grain) g.active = false;
    }
}

void Mosaic::onBlock(int64_t, int64_t, float bpmNow) { bpm = bpmNow; }
void Mosaic::controlChange(uint8_t number, uint8_t value) {
    cc[number & 0x7f] = static_cast<float>(value) / 127.0f;
    if (number == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Mosaic::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Mosaic::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

void Mosaic::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Mosaic::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

float Mosaic::sourceValue(const Voice &v, int src) const {
    switch (src) {
    case SrcOn: return 1.0f;
    case SrcModWheel: return modWheel;
    // This voice's own pressure if it has one, otherwise the channel's.
    case SrcPressure: return v.pressure >= 0.0f ? v.pressure : pressure;
    case SrcVelocity: return static_cast<float>(v.velocity) / 127.0f;
    case SrcKeyTrack: return (static_cast<float>(v.note) - 60.0f) / 48.0f;
    case SrcRandom: return v.random;
    case SrcAmpEg: return v.amp.value();
    case SrcFilterEg: return v.filterEg.value();
    case SrcEg1: return v.modEg[0].value();
    case SrcEg2: return v.modEg[1].value();
    case SrcLfo1: return v.lfo[0].value();
    case SrcLfo2: return v.lfo[1].value();
    default: return 0.0f;
    }
}

void Mosaic::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

/**
 * The file's own modulators, evaluated for one voice. Their sources include
 * continuous controllers, so this runs every block. Attenuation, pan and
 * tuning are per zone. The filter is per voice, so it uses the loudest
 * layer's value.
 */
void Mosaic::applyFileMods(Voice &v) {
    v.modCutoffCents = 0.0f;
    v.fileDrivesLevel = false;
    const bool honour = paramOf(FileMods) >= 0.5f;
    float bestGain = -1.0f;
    for (int32_t i = 0; i < v.layerCount; ++i) {
        Layer &L = v.layer[i];
        L.modGain = 1.0f;
        L.modPan = 0.0f;
        L.modTuneCents = 0.0f;
        if (!honour || L.zone == nullptr) continue;
        float attenuation = 0.0f, cutoff = 0.0f;
        for (int32_t m = 0; m < L.zone->modCount; ++m) {
            const ZoneMod &zm = L.zone->mods[m];
            float x;
            switch (zm.source) {
            case ZoneMod::SrcVelocity: x = static_cast<float>(v.velocity) / 127.0f; break;
            case ZoneMod::SrcKeyNumber: x = static_cast<float>(v.note) / 127.0f; break;
            case ZoneMod::SrcChannelPressure:
            case ZoneMod::SrcPolyPressure: x = pressure; break;
            case ZoneMod::SrcCc: x = cc[zm.cc & 0x7f]; break;
            default: continue;
            }
            const float out = zm.apply(x) * zm.amount;
            switch (zm.dest) {
            case ZoneMod::DstAttenuation: attenuation += out; break;
            case ZoneMod::DstFilterCutoff: cutoff += out; break;
            case ZoneMod::DstPan: L.modPan += out / 500.0f; break;
            default: L.modTuneCents += out; break;
            }
        }
        if (attenuation > 0.0f) L.modGain = std::pow(10.0f, -attenuation / 200.0f);
        if (L.zone->velocityToLevel) v.fileDrivesLevel = true;
        if (L.gain > bestGain) { bestGain = L.gain; v.modCutoffCents = cutoff; }
    }
}

void Mosaic::updateVoiceMod(Voice &v, float blockSeconds) {
    applyFileMods(v);
    for (int l = 0; l < kLfos; ++l) {
        const int32_t b = LfoBase + l * LfoParams;
        const int sync = stepOf(b + LSync);
        float hz = paramOf(b + LRate);
        if (sync > 0) hz = bpm / (60.0f * syncBeats(sync));
        hz *= std::exp2(v.mod[DstLfo1Rate + l] * 4.0f);
        v.lfo[l].advance(stepOf(b + LWave), hz, blockSeconds, 0.0f, false);
    }
    evalMatrix(v);
}

void Mosaic::evalMatrix(Voice &v) {
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

float Mosaic::readSample(const SampleData &s, double pos) {
    if (pos < 0.0) pos = 0.0;
    const int32_t i = static_cast<int32_t>(pos);
    if (i >= s.frames - 1) return s.frames > 0 ? s.left[static_cast<size_t>(s.frames - 1)] : 0.0f;
    const float f = static_cast<float>(pos - i);
    return s.left[static_cast<size_t>(i)] * (1.0f - f) + s.left[static_cast<size_t>(i + 1)] * f;
}

namespace {
/** `0.5 - 0.5 cos(2 pi t)` for t in [0, 1], from a table built once. */
float hannAt(float t) {
    constexpr int kSize = 1024;
    static const auto table = [] {
        std::array<float, kSize + 1> w{};
        for (int i = 0; i <= kSize; ++i) w[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(static_cast<float>(i) / kSize * kTwoPi);
        return w;
    }();
    const float x = clampf(t, 0.0f, 1.0f) * kSize;
    const int i = std::min(static_cast<int>(x), kSize - 1);
    const float f = x - static_cast<float>(i);
    return table[static_cast<size_t>(i)] + (table[static_cast<size_t>(i + 1)] - table[static_cast<size_t>(i)]) * f;
}
} // namespace

void Mosaic::cacheLayer(Layer &L, float panBase) {
    if (L.zone == nullptr || L.sample == nullptr) return;
    L.rootHz = mtof(static_cast<float>(L.zone->rootKey));
    L.tuneMul = std::exp2((L.zone->tuneCents + L.modTuneCents) / 1200.0f);
    L.rateRatio = static_cast<double>(L.sample->rate) / static_cast<double>(sampleRate);
    L.incFreq = -1.0f; // recalculate the rate on the next sample
    const float angle = (clampf(panBase + L.pan + L.modPan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    L.panC = std::cos(angle);
    L.panS = std::sin(angle);
}

void Mosaic::renderVoice(Voice &v, int32_t frames, float *outL, float *outR) {
    // A 2 ms fade, as a step per frame.
    const float endFadeStep = 1.0f / (0.002f * sampleRate);
    const float bendSemis = bend * paramOf(BendRange) + v.bend;
    const float semis = paramOf(Coarse) + paramOf(Fine) * 0.01f + paramOf(Transpose) + 12.0f * paramOf(Octave) +
                        bendSemis + v.mod[DstPitch] * 24.0f;
    const float pitchMul = std::exp2(semis / 12.0f);
    const float vel = static_cast<float>(v.velocity) / 127.0f;
    // A power curve, since some SoundFont dynamics come from modulators this
    // reader ignores. When the file sets level from velocity, the panel's
    // curve is skipped so it doesn't apply twice.
    const float velAmp = v.fileDrivesLevel ? 1.0f : velocityGain(vel, paramOf(VelocityAmount));
    const float volume = clampf(paramOf(Volume) + v.mod[DstAmp], 0.0f, 2.0f) * velAmp * kHouse;
    const float panBase = clampf(paramOf(Pan) + v.mod[DstPan], -1.0f, 1.0f);
    const int loopMode = stepOf(LoopModeIndex);
    const bool reverse = paramOf(Reverse) >= 0.5f;
    const bool grains = paramOf(GrainMode) >= 0.5f;

    const float filterBase = paramOf(FilterFreq) *
        std::exp2(paramOf(FilterKey) * (static_cast<float>(v.note) - 60.0f) / 12.0f +
                  v.mod[DstFilterFreq] * 6.0f + paramOf(VelToFilter) * vel * 4.0f +
                  // Slide opens the filter, as in Trinity.
                  (v.timbre >= 0.0f ? v.timbre : 0.0f) * paramOf(MpeTimbre) * 4.0f +
                  v.modCutoffCents / 1200.0f);
    const float filterEnvAmt = paramOf(FilterEnv);
    const float filterRes = clampf(paramOf(FilterRes) + v.mod[DstFilterRes], 0.0f, 1.0f);
    const int filterType = stepOf(FilterType);

    const float glideSeconds = paramOf(Glide);
    const float glideStep = glideSeconds > 0.001f ? 1.0f / (glideSeconds * sampleRate) : 1.0f;
    const float targetFreq = noteHz(static_cast<float>(v.note));
    const float glideOctaves = v.glidePos < 1.0f ? std::log2(targetFreq / v.glideFrom) : 0.0f;

    // Grain settings, all modulatable.
    const float gPos = clampf(paramOf(GrainPos) + v.mod[DstGrainPos], 0.0f, 1.0f);
    const float gRate = clampf(paramOf(GrainRate) + v.mod[DstGrainRate] * 2.0f, -4.0f, 4.0f);
    const float gSizeMs = clampf(paramOf(GrainSize) * std::exp2(v.mod[DstGrainSize] * 2.0f), 2.0f, 1000.0f);
    const float gDensity = clampf(paramOf(GrainDensity) * std::exp2(v.mod[DstGrainDensity] * 2.0f), 0.5f, 400.0f);
    const float gSpray = clampf(paramOf(GrainSpray) + v.mod[DstGrainSpray], 0.0f, 1.0f);
    const float gPitch = clampf(paramOf(GrainPitch) + v.mod[DstGrainPitch] * 24.0f, 0.0f, 48.0f);

    for (int32_t z = 0; z < v.layerCount; ++z) cacheLayer(v.layer[z], panBase);
    // The rate for a layer at the voice's current frequency, only recalculated
    // when the frequency has moved.
    const auto rateOf = [&](Layer &L) {
        if (v.freq != L.incFreq) {
            L.incFreq = v.freq;
            L.incNow = static_cast<double>(v.freq) / L.rootHz * L.tuneMul * pitchMul * L.rateRatio;
        }
        return L.incNow;
    };

    for (int32_t i = 0; i < frames; ++i) {
        if (v.glidePos < 1.0f) {
            v.glidePos += glideStep;
            if (v.glidePos >= 1.0f) { v.glidePos = 1.0f; v.freq = targetFreq; }
            else v.freq = v.glideFrom * std::exp2(glideOctaves * v.glidePos);
        } else {
            v.freq = targetFreq;
        }

        const float env = v.amp.next();
        const float fenv = v.filterEg.next();
        // Only the ones a matrix row reads. See Trinity's envMask.
        for (int e = 0; e < kModEgs; ++e) {
            if (egUsed & (1 << e)) v.modEg[e].next();
        }

        float l = 0.0f, r = 0.0f;
        if (grains && v.layerCount > 0) {
            // Grain mode only plays the loudest layer, which keeps it to eight
            // readers per voice instead of 32.
            int32_t best = 0;
            for (int32_t z = 1; z < v.layerCount; ++z) if (v.layer[z].gain > v.layer[best].gain) best = z;
            Layer &L = v.layer[best];
            if (L.sample != nullptr && L.sample->frames > 1) {
                const double natural = rateOf(L);
                v.grainOffset += natural * static_cast<double>(gRate);
                const double span = static_cast<double>(L.sample->frames);
                if (v.grainOffset > span) v.grainOffset -= span;
                if (v.grainOffset < -span) v.grainOffset += span;

                v.grainTimer -= 1.0f;
                if (v.grainTimer <= 0.0f) {
                    v.grainTimer = sampleRate / gDensity;
                    for (auto &g : v.grain) {
                        if (g.active) continue;
                        double p = static_cast<double>(gPos) * span + v.grainOffset +
                                   static_cast<double>((rnd(v.rng) * 2.0f - 1.0f) * gSpray) * span * 0.5;
                        while (p < 0.0) p += span;
                        while (p >= span) p -= span;
                        g.inc = natural * std::exp2((rnd(v.rng) * 2.0f - 1.0f) * gPitch / 12.0f);
                        g.length = static_cast<int32_t>(gSizeMs * 0.001f * sampleRate);
                        if (g.length < 8) g.length = 8;
                        // Start far enough from the end that the whole grain
                        // fits. A grain that wraps mid-window crackles, and
                        // on long notes `grainOffset` reaches the end so it
                        // would happen to every grain.
                        const double reach = static_cast<double>(g.length) * g.inc;
                        if (reach < span) p = std::min(p, span - reach - 1.0);
                        g.pos = p;
                        g.age = 0;
                        g.gain = 1.0f;
                        g.active = true;
                        break;
                    }
                }
                float sum = 0.0f;
                for (auto &g : v.grain) {
                    if (!g.active) continue;
                    // Hann window, so grains fade in and out without clicking.
                    // Read from a table like Pollen's, which is much cheaper
                    // than a cos per sample and within 5e-6 of the curve.
                    const float t = static_cast<float>(g.age) / static_cast<float>(g.length);
                    const float w = hannAt(t);
                    sum += readSample(*L.sample, g.pos) * w;
                    g.pos += g.inc;
                    if (g.pos >= span) g.pos -= span;
                    if (g.pos < 0.0) g.pos += span;
                    if (++g.age >= g.length) g.active = false;
                }
                sum *= L.gain * L.modGain;
                l += sum * L.panC * 1.4142f;
                r += sum * L.panS * 1.4142f;
            }
        } else {
            for (int32_t z = 0; z < v.layerCount; ++z) {
                Layer &L = v.layer[z];
                if (L.sample == nullptr || L.finished || L.sample->frames < 2) continue;
                const double inc = rateOf(L);
                const float s = readSample(*L.sample, L.pos) * L.gain * L.modGain * L.fade * L.fadeIn;
                if (L.pendingPos >= 0.0) {
                    // Waiting to move: fade out here, then jump and fade back
                    // in. About 4 ms in total.
                    L.fade -= endFadeStep;
                    if (L.fade <= 0.0f) {
                        L.pos = L.pendingPos;
                        if (L.pendingSample != nullptr) {
                            L.zone = L.pendingZone;
                            L.sample = L.pendingSample;
                            L.gain = L.pendingGain;
                            L.pan = L.pendingPan;
                            L.pendingSample = nullptr;
                            L.pendingZone = nullptr;
                            cacheLayer(L, panBase); // new zone, so new rate and pan
                        }
                        L.pendingPos = -1.0;
                        L.fade = 1.0f;
                        L.fadeIn = 0.0f;
                    }
                } else if (L.fadeIn < 1.0f) {
                    L.fadeIn = std::min(1.0f, L.fadeIn + endFadeStep);
                }
                l += s * L.panC * 1.4142f;
                r += s * L.panS * 1.4142f;

                L.pos += reverse ? -inc : inc;
                const bool wantLoop = loopMode == LoopForward ||
                                      (loopMode == LoopFromFile && L.zone->loopStart >= 0 && L.zone->loopEnd > L.zone->loopStart);
                const double loopA = L.zone->loopStart >= 0 ? static_cast<double>(L.zone->loopStart) : 0.0;
                const double loopB = L.zone->loopEnd > L.zone->loopStart ? static_cast<double>(L.zone->loopEnd)
                                                                         : static_cast<double>(L.sample->frames - 1);
                if (wantLoop && loopB > loopA + 1.0) {
                    // Wrap into the loop in one go. The playhead can start
                    // well past the loop end (with a high `start`), and
                    // stepping back one loop length per sample would crackle.
                    const double len = loopB - loopA;
                    if (L.pos >= loopB) L.pos = loopA + std::fmod(L.pos - loopA, len);
                    if (L.pos < loopA && reverse) L.pos = loopB - std::fmod(loopA - L.pos, len);
                } else if (L.pos >= static_cast<double>(L.sample->frames - 1) || L.pos < 0.0) {
                    // Off the end with no loop: ramp out over about 2 ms.
                    // The playhead is held so the ramp fades the last sample
                    // instead of reading past the buffer.
                    L.pos = std::min(std::max(L.pos, 0.0), static_cast<double>(L.sample->frames - 2));
                    L.fade -= endFadeStep;
                    if (L.fade <= 0.0f) { L.fade = 0.0f; L.finished = true; }
                }
            }
        }

        if ((i & 15) == 0) {
            const float fc = filterBase * std::exp2(filterEnvAmt * fenv * 4.0f);
            v.filter.set(fc, filterRes, filterType, 0, 0.0f);
            v.filterR.set(fc, filterRes, filterType, 0, 0.0f);
        }
        const float g = env * volume;
        float oL = v.filter.process(l) * g;
        float oR = v.filterR.process(r) * g;
        outL[i] = oL;
        outR[i] = oR;
    }
}

bool Mosaic::render(float *L, float *R, int32_t frames) {
    params_.tick();
    egUsed = 0;
    for (int s = 0; s < kMatrixSlots; ++s) {
        const int32_t b = MatrixBase + s * MatrixParams;
        if (stepOf(b + XDest) == DstOff) continue;
        const int a = stepOf(b + XSrc), b2 = stepOf(b + XSrc2);
        if (a >= SrcEg1 && a <= SrcEg2) egUsed |= 1 << (a - SrcEg1);
        if (b2 >= SrcEg1 && b2 <= SrcEg2) egUsed |= 1 << (b2 - SrcEg1);
    }
    for (int32_t i = 0; i < frames; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    if (map == nullptr) return true;
    const float blockSeconds = static_cast<float>(frames) / sampleRate;
    for (auto &v : voices) {
        if (!v.used) continue;
        updateVoiceMod(v, blockSeconds);
        renderVoice(v, frames, voiceL, voiceR);
        for (int32_t i = 0; i < frames; ++i) { L[i] += voiceL[i]; R[i] += voiceR[i]; }
        bool anyPlaying = paramOf(GrainMode) >= 0.5f;
        for (int32_t z = 0; z < v.layerCount && !anyPlaying; ++z) if (!v.layer[z].finished) anyPlaying = true;
        if ((!v.gate && !v.amp.active()) || !anyPlaying) {
            v.used = false;
            v.layerCount = 0;
        }
    }
    return true;
}

} // namespace acidulous::machine
