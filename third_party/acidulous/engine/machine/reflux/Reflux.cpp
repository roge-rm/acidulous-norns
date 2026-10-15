#include "Reflux.h"
#include <engine/machine/Voices.h>
#include <engine/core/Constants.h>

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

const ParamDef kDefs[Reflux::Count] = {
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
};
} // namespace

Reflux::Reflux() { initParams(); }

const ParamDef *Reflux::paramDefs(int32_t &count) const {
    count = Count;
    return kDefs;
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
}

void Reflux::startNote(uint8_t note, bool legato, bool accent, float level) {
    targetPitch = static_cast<float>(note);
    if (legato) {
        // Slide: glide there, keep the envelopes running.
        gliding = true;
        glideCoeff = dsp::onePoleCoeff(params_.get(Slide) * 0.001f, sampleRate);
    } else {
        pitch = targetPitch;
        gliding = false;
        accented = accent;
        velLevel = level;
        // An accented note gets a shorter filter decay, so the sweep is snappier.
        filterEnv.setTimes(0.003f, params_.get(Decay) * 0.001f * (accent ? 0.6f : 1.0f));
        filterEnv.trigger();
        if (accent) accentEnv.trigger();
        ampEnv.gate(true);
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
    startNote(note, legato, velocity >= kAccentVelocity, velocityGain(static_cast<float>(velocity) / 127.0f, params_.get(Velocity)));
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
    const bool pulse = params_.get(Wave) >= 0.5f;
    const float tune = params_.get(Tune);
    const float baseCutoff = params_.get(Cutoff);
    const float resonance = params_.get(Resonance);
    const float envMod = params_.get(EnvMod);
    const float accentAmt = params_.get(Accent);
    const float drive = params_.get(Drive);
    const float volume = params_.get(Volume);
    const float pw = params_.get(PulseWidth);
    const float subLevel = params_.get(Sub);
    const bool bandpass = params_.get(Mode) >= 0.5f;

    if (!ampEnv.active()) {
        for (int32_t i = 0; i < frames; ++i) L[i] = 0.0f;
        return false;
    }

    const float driveGain = 1.0f + drive * 7.0f;
    // Normalised on the nominal level so drive changes the tone and not the
    // level.
    const float driveComp = kNominal / dsp::fastTanh(kNominal * driveGain);

    for (int32_t i = 0; i < frames; ++i) {
        if (gliding) {
            pitch += (targetPitch - pitch) * glideCoeff;
            if (std::fabs(targetPitch - pitch) < 0.001f) { pitch = targetPitch; gliding = false; }
        }
        const float hz = noteHz(pitch + tune);
        osc.setFrequency(hz);
        sub.setFrequency(hz * 0.5f);
        float s = pulse ? osc.pulse(pw) : osc.saw();
        if (subLevel > 0.0f) s += sub.pulse(0.5f) * subLevel;

        const float fenv = filterEnv.next();
        const float aenv = accentEnv.next();
        // Envelope sweeps the cutoff in octaves; accent adds up to 1.5 more.
        const float octaves = fenv * envMod * 4.0f * (1.0f + (accented ? accentAmt * 1.5f : 0.0f));
        if (coeffCountdown-- <= 0) {
            const float fc = baseCutoff * std::exp2(octaves);
            svf1.set(fc, resonance);
            svf2.set(fc, resonance * 0.3f);
            coeffCountdown = 3; // every 4 samples is plenty for a sweep
        }
        // Two filter stages with a little saturation between them, so the
        // resonance rounds off instead of ringing clean.
        s = bandpass ? svf1.bandpass(s) : svf1.lowpass(s);
        s = dsp::fastTanh(s * 1.3f) * 0.77f;
        s = svf2.lowpass(s);
        s = dsp::fastTanh(s * driveGain) * driveComp;

        const float amp = ampEnv.next() * volume * kHouse * velLevel *
                          (1.0f + (accented ? accentAmt * aenv * 0.6f : 0.0f));
        L[i] = s * amp;
    }
    return false;
}

} // namespace acidulous::machine
