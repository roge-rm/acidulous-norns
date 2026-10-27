#include "Reflux.h"
#include <engine/machine/Voices.h>
#include <engine/core/Constants.h>
#include <engine/dsp/Lfo.h>
#include <algorithm>
#include <cstdio>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, measured at the
// oscillator before volume x kHouse. The drive is normalised to it. Set it too
// high and the drive knob mostly changes the volume.
constexpr float kNominal = 0.45f;

namespace {

/**
 * The house level. Every machine scales its output so its default patch lands
 * at about -21 dB on the test harness's loudness measure (the loudest 400 ms
 * of one note). That way switching machines doesn't change the song's volume
 * and the volume knob means the same thing everywhere.
 */
constexpr float kHouse = 0.2f;

constexpr int kSyncCount = 1 + dsp::Lfo::kRates;

const ParamDef kBase[Reflux::MacroBase] = {
    {"wave", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},          // 0 saw, 1 pulse
    {"tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "st"},
    {"cutoff", 40.0f, 12000.0f, 700.0f, Curve::Exponential, 0, "Hz"},
    {"resonance", 0.0f, 1.0f, 0.55f, Curve::Linear, 0, ""},
    {"envmod", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
    {"decay", 30.0f, 3000.0f, 300.0f, Curve::Exponential, 0, "ms"},
    {"accent", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"slide", 5.0f, 500.0f, 60.0f, Curve::Exponential, 0, "ms"},
    {"drive", 0.0f, 1.0f, 0.1f, Curve::Linear, 0, ""},
    // Up to 1.5, like most machines, so patches with drive have room to
    // reach the bank's level.
    {"volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
    {"pw", 0.05f, 0.95f, 0.5f, Curve::Linear, 0, ""},          // pulse width, pulse wave only
    {"sub", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},           // square an octave down
    {"mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},         // 0 lowpass, 1 bandpass
    {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
    {"mix", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},            // toward the other wave
    {"filter", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},       // lp24, lp12, hp, notch
    {"lfo_rate", 0.05f, 20.0f, 2.0f, Curve::Exponential, 0, "Hz"},
    {"lfo_sync", 0.0f, kSyncCount - 1.0f, 0.0f, Curve::Stepped, kSyncCount, ""}, // free, then note values
    {"lfo_shape", 0.0f, Reflux::ShapeCount - 1.0f, 0.0f, Curve::Stepped, Reflux::ShapeCount, ""},
    {"lfo_retrig", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},   // 0 free running, 1 restarts on a note
    {"lfo_cut", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},      // 4 octaves at full
    {"lfo_pitch", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},    // 12 semitones at full
    {"lfo_pw", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},       // 0.45 of width at full
};

// The base table plus the macros' knobs and routes.
struct Table {
    ParamDef defs[Reflux::Count];
    char names[Reflux::kMacros * Reflux::MacroParams][12];
    Table() {
        for (int32_t i = 0; i < Reflux::MacroBase; ++i) defs[i] = kBase[i];
        for (int k = 0; k < Reflux::kMacros; ++k) {
            const int32_t b = Reflux::MacroBase + k * Reflux::MacroParams;
            char *n = names[k * Reflux::MacroParams];
            std::snprintf(n, 12, "mac%d", k + 1);
            defs[b] = {n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            for (int r = 0; r < 4; ++r) {
                char *d = names[k * Reflux::MacroParams + 1 + r];
                std::snprintf(d, 12, "mac%d_d%d", k + 1, r + 1);
                defs[b + 1 + r] = {d, 0.0f, Reflux::DestCount - 1.0f, 0.0f, Curve::Stepped, Reflux::DestCount, ""};
                char *a = names[k * Reflux::MacroParams + 5 + r];
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

// Which param each macro destination moves in its own range; -1 for the ones
// with their own units (pitch, LFO rate).
constexpr int32_t kDestParam[Reflux::DestCount] = {
    -1, Reflux::Cutoff, Reflux::Resonance, Reflux::EnvMod, Reflux::Decay, Reflux::Accent, Reflux::Slide, Reflux::Drive,
    Reflux::Volume, Reflux::PulseWidth, Reflux::Sub, Reflux::Mix, -1, -1, Reflux::LfoCutoff, Reflux::LfoPitch, Reflux::LfoPw,
};

} // namespace

Reflux::Reflux() { initParams(); }

const ParamDef *Reflux::paramDefs(int32_t &count) const {
    count = Count;
    return table().defs;
}

void Reflux::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    osc.setSampleRate(sampleRate);
    sub.setSampleRate(sampleRate);
    svf1.setSampleRate(sampleRate);
    svf2.setSampleRate(sampleRate);
    filterEnv.setSampleRate(sampleRate);
    accentEnv.setSampleRate(sampleRate);
    ampEnv.setSampleRate(sampleRate);
    accentEnv.setTimes(0.001f, 0.12f);
    ampEnv.setTimes(0.002f, 0.010f);
    params_.jumpAll();
    reset();
}

void Reflux::reset() {
    stackSize = 0;
    gliding = false;
    accented = false;
    velLevel = 1.0f;
    filterEnv.kill();
    accentEnv.kill();
    ampEnv.kill();
    svf1.reset();
    svf2.reset();
    // Reset the oscillators and the glide target too. The oscillators
    // free-run and aren't restarted by notes, so without this two renders of
    // the same song would differ.
    osc.reset();
    sub.reset();
    pitch = targetPitch = 48.0f;
    coeffCountdown = 0;
    lfoPhase = 0.0f;
    lfoHeld = 0.0f;
    lfoSeed = 0x2545f491u;
}

void Reflux::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

float Reflux::macroSum(int dest, bool target) const {
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

float Reflux::moved(int32_t p, float off, bool target) const {
    const float v = target ? params_.target(p) : params_.get(p);
    if (off == 0.0f) return v;
    const ParamDef &d = params_.def(p);
    return d.map(d.unmap(v) + off);
}

float Reflux::lfoShape(int shape) const {
    const float p = lfoPhase;
    switch (shape) {
    case Triangle: return dsp::Lfo::triangle(p);
    case Square: return p < 0.5f ? 1.0f : -1.0f;
    case Saw: return 1.0f - 2.0f * p; // falls, so a cutoff wobble closes each cycle
    case Random: return lfoHeld;
    default: return dsp::Lfo::sine(p);
    }
}

void Reflux::startNote(uint8_t note, bool legato, bool accent, float level) {
    targetPitch = static_cast<float>(note);
    if (legato) {
        // Slide: glide there, keep the envelopes running.
        gliding = true;
        glideCoeff = dsp::onePoleCoeff(moved(Slide, macroSum(DstSlide, true), true) * 0.001f, sampleRate);
    } else {
        pitch = targetPitch;
        gliding = false;
        accented = accent;
        velLevel = level;
        // An accented note gets a shorter filter decay, so the sweep is snappier.
        filterEnv.setTimes(0.003f, moved(Decay, macroSum(DstDecay, true), true) * 0.001f * (accent ? 0.6f : 1.0f));
        filterEnv.trigger();
        if (accent) accentEnv.trigger();
        ampEnv.gate(true);
        if (steppedTargetOf(LfoRetrig) == 1) {
            lfoPhase = 0.0f;
            lfoSeed ^= lfoSeed << 13; lfoSeed ^= lfoSeed >> 17; lfoSeed ^= lfoSeed << 5;
            lfoHeld = static_cast<float>(lfoSeed & 0xffffff) / 8388608.0f - 1.0f;
        }
    }
}

void Reflux::noteOn(uint8_t note, uint8_t velocity) {
    const bool legato = stackSize > 0;
    // Push onto the held-note stack, or move to the top.
    int32_t found = -1;
    for (int32_t i = 0; i < stackSize; ++i) {
        if (stack[i] == note) { found = i; break; }
    }
    if (found >= 0) {
        for (int32_t i = found; i < stackSize - 1; ++i) stack[i] = stack[i + 1];
        --stackSize;
    }
    if (stackSize == kStack) {
        for (int32_t i = 0; i < kStack - 1; ++i) stack[i] = stack[i + 1];
        --stackSize;
    }
    stack[stackSize++] = note;
    startNote(note, legato, velocity >= kAccentVelocity, velocityGain(static_cast<float>(velocity) / 127.0f, targetOf(Velocity)));
}

void Reflux::noteOff(uint8_t note) {
    int32_t found = -1;
    for (int32_t i = 0; i < stackSize; ++i) {
        if (stack[i] == note) { found = i; break; }
    }
    if (found < 0) return;
    const bool wasTop = (found == stackSize - 1);
    for (int32_t i = found; i < stackSize - 1; ++i) stack[i] = stack[i + 1];
    --stackSize;
    if (stackSize == 0) {
        ampEnv.gate(false);
    } else if (wasTop) {
        // Slide back to the previous held note.
        startNote(stack[stackSize - 1], true, false);
    }
}

void Reflux::allNotesOff() {
    stackSize = 0;
    ampEnv.gate(false);
}

bool Reflux::render(float *L, float * /*R*/, int32_t frames) {
    params_.tick();
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

    const bool pulse = params_.get(Wave) >= 0.5f;
    const float tune = params_.get(Tune) + mac[DstPitch] * 24.0f;
    const float baseCutoff = value(DstCutoff);
    const float resonance = value(DstResonance);
    const float envMod = value(DstEnvMod);
    const float accentAmt = value(DstAccent);
    const float drive = value(DstDrive);
    const float volume = value(DstAmp);
    const float pwKnob = value(DstPw);
    const float subLevel = value(DstSub);
    const float mix = value(DstMix);
    const bool bandpass = params_.get(Mode) >= 0.5f;
    const int filter = bandpass ? 0 : static_cast<int>(params_.get(Filter) + 0.5f);

    // The LFO runs whether or not anything listens, so its phase doesn't
    // depend on the depths.
    const int sync = static_cast<int>(params_.get(LfoSync) + 0.5f);
    float lfoHz = sync > 0 ? bpm / (60.0f * dsp::Lfo::beatsOf(sync - 1)) : params_.get(LfoRate);
    if (mac[DstLfoRate] != 0.0f) lfoHz *= std::exp2(mac[DstLfoRate] * 4.0f);
    const float lfoInc = lfoHz / sampleRate;
    const int shape = static_cast<int>(params_.get(LfoShape) + 0.5f);
    const float lfoCut = value(DstLfoCutoff) * 4.0f;   // octaves
    const float lfoPitch = value(DstLfoPitch) * 12.0f; // semitones
    const float lfoPw = value(DstLfoPw) * 0.45f;
    const bool lfoOn = lfoCut != 0.0f || lfoPitch != 0.0f || lfoPw != 0.0f;

    if (!ampEnv.active()) {
        for (int32_t i = 0; i < frames; ++i) L[i] = 0.0f;
        // Keep the LFO turning while silent, a block at a time.
        lfoPhase += lfoInc * static_cast<float>(frames);
        if (lfoPhase >= 1.0f) {
            lfoPhase -= std::floor(lfoPhase);
            lfoSeed ^= lfoSeed << 13; lfoSeed ^= lfoSeed >> 17; lfoSeed ^= lfoSeed << 5;
            lfoHeld = static_cast<float>(lfoSeed & 0xffffff) / 8388608.0f - 1.0f;
        }
        return false;
    }

    const float driveGain = 1.0f + drive * 7.0f;
    // Normalised on the nominal level so drive changes the tone and not the
    // level.
    const float driveComp = kNominal / dsp::fastTanh(kNominal * driveGain);

    for (int32_t i = 0; i < frames; ++i) {
        lfoPhase += lfoInc;
        if (lfoPhase >= 1.0f) {
            lfoPhase -= 1.0f;
            lfoSeed ^= lfoSeed << 13; lfoSeed ^= lfoSeed >> 17; lfoSeed ^= lfoSeed << 5;
            lfoHeld = static_cast<float>(lfoSeed & 0xffffff) / 8388608.0f - 1.0f;
        }
        const float lfo = lfoOn ? lfoShape(shape) : 0.0f;
        if (gliding) {
            pitch += (targetPitch - pitch) * glideCoeff;
            if (std::fabs(targetPitch - pitch) < 0.001f) { pitch = targetPitch; gliding = false; }
        }
        const float hz = noteHz(pitch + tune + lfo * lfoPitch);
        osc.setFrequency(hz);
        sub.setFrequency(hz * 0.5f);
        const float pw = lfoPw == 0.0f ? pwKnob : std::clamp(pwKnob + lfo * lfoPw, 0.05f, 0.95f);
        float s;
        if (mix == 0.0f) {
            s = pulse ? osc.pulse(pw) : osc.saw();
        } else {
            float sw, pl;
            osc.both(pw, sw, pl);
            s = pulse ? pl + (sw - pl) * mix : sw + (pl - sw) * mix;
        }
        if (subLevel > 0.0f) s += sub.pulse(0.5f) * subLevel;

        const float fenv = filterEnv.next();
        const float aenv = accentEnv.next();
        // Envelope sweeps the cutoff in octaves; accent adds up to 1.5 more.
        const float octaves = fenv * envMod * 4.0f * (1.0f + (accented ? accentAmt * 1.5f : 0.0f)) + lfo * lfoCut;
        if (coeffCountdown-- <= 0) {
            const float fc = baseCutoff * std::exp2(octaves);
            svf1.set(fc, resonance);
            svf2.set(fc, resonance * 0.3f);
            coeffCountdown = 3; // every 4 samples is plenty for a sweep
        }
        // Two filter stages with a little saturation between them, so the
        // resonance rounds off instead of ringing clean. 12 dB and notch use
        // the first stage only.
        switch (filter) {
        case 1:
            s = svf1.lowpass(s);
            s = dsp::fastTanh(s * 1.3f) * 0.77f;
            break;
        case 2:
            s = svf1.highpass(s);
            s = dsp::fastTanh(s * 1.3f) * 0.77f;
            s = svf2.highpass(s);
            break;
        case 3: {
            const auto o = svf1.step(s);
            s = dsp::fastTanh((o.lp + o.hp) * 1.3f) * 0.77f;
            break;
        }
        default:
            s = bandpass ? svf1.bandpass(s) : svf1.lowpass(s);
            s = dsp::fastTanh(s * 1.3f) * 0.77f;
            s = svf2.lowpass(s);
        }
        s = dsp::fastTanh(s * driveGain) * driveComp;

        const float amp = ampEnv.next() * volume * kHouse * velLevel *
                          (1.0f + (accented ? accentAmt * aenv * 0.6f : 0.0f));
        L[i] = s * amp;
    }
    return false;
}

} // namespace acidulous::machine
