#include "Hexbeat.h"
#include <engine/machine/Voices.h>
#include <engine/core/Constants.h>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

namespace {
const ParamDef kDefs[Hexbeat::Count] = {
    {"kick_tune", 35.0f, 120.0f, 55.0f, Curve::Exponential, 0, "Hz"},
    {"kick_decay", 50.0f, 2000.0f, 400.0f, Curve::Exponential, 0, "ms"},
    {"kick_punch", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
    {"kick_level", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
    {"snare_tune", 120.0f, 400.0f, 190.0f, Curve::Exponential, 0, "Hz"},
    {"snare_decay", 50.0f, 1000.0f, 220.0f, Curve::Exponential, 0, "ms"},
    {"snare_snappy", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"snare_tone", 800.0f, 6000.0f, 2200.0f, Curve::Exponential, 0, "Hz"},
    {"snare_level", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
    {"tom_lo_tune", 60.0f, 200.0f, 90.0f, Curve::Exponential, 0, "Hz"},
    {"tom_mid_tune", 90.0f, 300.0f, 140.0f, Curve::Exponential, 0, "Hz"},
    {"tom_hi_tune", 120.0f, 400.0f, 200.0f, Curve::Exponential, 0, "Hz"},
    {"tom_decay", 50.0f, 1500.0f, 350.0f, Curve::Exponential, 0, "ms"},
    {"tom_level", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
    {"hat_tune", 0.5f, 2.0f, 1.0f, Curve::Exponential, 0, "x"},
    {"hat_closed_decay", 15.0f, 300.0f, 60.0f, Curve::Exponential, 0, "ms"},
    {"hat_open_decay", 100.0f, 2000.0f, 500.0f, Curve::Exponential, 0, "ms"},
    {"hat_tone", 4000.0f, 12000.0f, 8000.0f, Curve::Exponential, 0, "Hz"},
    {"hat_level", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"cym_decay", 200.0f, 4000.0f, 1500.0f, Curve::Exponential, 0, "ms"},
    {"cym_tone", 2000.0f, 9000.0f, 4500.0f, Curve::Exponential, 0, "Hz"},
    {"cym_level", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
    {"ride_decay", 200.0f, 3000.0f, 900.0f, Curve::Exponential, 0, "ms"},
    {"ride_level", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
    {"clap_decay", 50.0f, 800.0f, 200.0f, Curve::Exponential, 0, "ms"},
    {"clap_tone", 700.0f, 3000.0f, 1300.0f, Curve::Exponential, 0, "Hz"},
    {"clap_level", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    {"rim_tune", 300.0f, 1500.0f, 800.0f, Curve::Exponential, 0, "Hz"},
    {"rim_level", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    {"bell_tune", 300.0f, 1200.0f, 560.0f, Curve::Exponential, 0, "Hz"},
    {"bell_decay", 30.0f, 600.0f, 180.0f, Curve::Exponential, 0, "ms"},
    {"bell_level", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"clave_tune", 1200.0f, 4000.0f, 2500.0f, Curve::Exponential, 0, "Hz"},
    {"clave_level", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"accent", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
    {"volume", 0.0f, 1.5f, 0.9f, Curve::Linear, 0, ""},
    {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
};
// The classic six metal frequencies, as ratios of the lowest.
const float kMetalRatios[6] = {1.0f, 1.483f, 1.800f, 2.546f, 2.634f, 3.902f};
constexpr float kMetalBase = 205.0f;

// How hard the voices drive the output tanh. A single voice at full accent
// comes out around a third of full scale and stays clean. It takes several
// hits at once to saturate.
constexpr float kDrive = 0.375f; // -8.5 dB into the clipper

// The house level. kDrive sets how dirty the kit is, this sets where a kit
// sits on the volume knob so a levelled bank lands near the middle. Chosen
// against the Init kit at the -11 dB peak the drum machines level to.
constexpr float kHouse = 0.96f; // -0.4 dB

// Per-voice level trims so `level` means the same on every voice. The
// oscillator voices (kick, toms, snare, rim) all give -16.6 dB at level 1,
// and these bring the noise and metal voices down to match.
constexpr float kVoiceTrim[Hexbeat::VoiceCount] = {
    1.0f,   // Kick
    1.0f,   // Rim
    1.0f,   // Snare
    0.355f, // Clap     -9.0 dB
    1.0f,   // TomLo
    1.0f,   // TomMid
    1.0f,   // TomHi
    0.586f, // HatClosed -4.6 dB
    0.586f, // HatOpen
    0.402f, // Cymbal   -7.9 dB
    0.523f, // Ride     -5.6 dB
    0.410f, // Cowbell  -7.7 dB
    0.681f, // Clave    -3.3 dB
};
} // namespace

Hexbeat::Hexbeat() { initParams(); }

const ParamDef *Hexbeat::paramDefs(int32_t &count) const { count = Count; return kDefs; }

void Hexbeat::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (int32_t v = 0; v < VoiceCount; ++v) { bp[v].setSampleRate(sr); bp2[v].setSampleRate(sr); }
    params_.jumpAll();
    reset();
}

void Hexbeat::reset() {
    // Assign fresh structs so every field is cleared, including ones added
    // later.
    for (int32_t v = 0; v < VoiceCount; ++v) {
        amp[v] = Env();
        pitchEnv[v] = Env();
        aux[v] = Env();
        osc[v] = Osc();
        osc2[v] = Osc();
        gain[v] = 0.0f;
        bp[v].reset();
        bp2[v].reset();
    }
    for (auto &o : metal) o = Osc();
    clapPulse = 0;
    clapTimer = 0.0f;
    rng = kRngSeed;
}

void Hexbeat::allNotesOff() {} // nothing sustains; let tails ring

void Hexbeat::noteOn(uint8_t note, uint8_t velocity) {
    const int32_t v = static_cast<int32_t>(note) - kBaseNote;
    if (v < 0 || v >= VoiceCount) return;
    const float acc = velocity >= 100 ? params_.get(Accent) : 0.0f;
    trigger(v, acc, velocityGain(static_cast<float>(velocity) / 127.0f, params_.get(Velocity)));
}

float Hexbeat::metallic(float tune) {
    float s = 0.0f;
    for (int i = 0; i < 6; ++i) s += square(metal[i], kMetalBase * kMetalRatios[i] * tune);
    return s * (1.0f / 6.0f);
}

void Hexbeat::trigger(int32_t v, float acc, float level) {
    const float ms = 0.001f;
    gain[v] = (1.0f + acc * 0.8f) * level;
    switch (v) {
    case Kick:
        amp[v].fire(sr, params_.get(KickDecay) * ms);
        pitchEnv[v].fireTau(sr, 0.035f);
        aux[v].fireTau(sr, 0.004f); // click
        osc[v].phase = 0.0f;
        break;
    case Snare:
        amp[v].fire(sr, params_.get(SnareDecay) * ms * 0.7f);
        aux[v].fire(sr, params_.get(SnareDecay) * ms);
        osc[v].phase = osc2[v].phase = 0.0f;
        break;
    case TomLo: case TomMid: case TomHi:
        amp[v].fire(sr, params_.get(TomDecay) * ms);
        pitchEnv[v].fireTau(sr, 0.05f);
        osc[v].phase = 0.0f;
        break;
    case HatClosed:
        amp[v].fire(sr, params_.get(HatClosedDecay) * ms);
        if (amp[HatOpen].active) amp[HatOpen].fireTau(sr, 0.02f, amp[HatOpen].level); // choke
        break;
    case HatOpen: amp[v].fire(sr, params_.get(HatOpenDecay) * ms); break;
    case Cymbal: amp[v].fire(sr, params_.get(CymDecay) * ms); aux[v].fire(sr, params_.get(CymDecay) * ms * 0.3f); break;
    case Ride: amp[v].fire(sr, params_.get(RideDecay) * ms); aux[v].fireTau(sr, 0.25f); osc[v].phase = 0.0f; break;
    case Clap:
        amp[v].fire(sr, params_.get(ClapDecay) * ms);
        aux[v].fireTau(sr, 0.008f);
        clapPulse = 3;
        clapTimer = 0.0f;
        break;
    case Rim: amp[v].fireTau(sr, 0.02f); aux[v].fireTau(sr, 0.003f); osc[v].phase = 0.0f; break;
    case Cowbell: amp[v].fire(sr, params_.get(BellDecay) * ms); break;
    case Clave: amp[v].fireTau(sr, 0.04f); osc[v].phase = 0.0f; break;
    default: break;
    }
}

void Hexbeat::renderVoice(int32_t v, float *out, int32_t frames) {
    const float g = gain[v];
    switch (v) {
    case Kick: {
        const float base = params_.get(KickTune), punch = params_.get(KickPunch), level = params_.get(KickLevel) * kVoiceTrim[v];
        for (int32_t i = 0; i < frames; ++i) {
            const float pe = pitchEnv[v].next();
            const float hz = base * (1.0f + punch * 6.0f * pe);
            float s = sine(osc[v], hz) * amp[v].next();
            s += noise() * aux[v].next() * 0.4f;
            out[i] += dsp::fastTanh(s * 1.6f * g) * level;
        }
        break;
    }
    case Snare: {
        const float f = params_.get(SnareTune), snappy = params_.get(SnareSnappy), level = params_.get(SnareLevel) * kVoiceTrim[v];
        bp[v].set(params_.get(SnareTone), 0.3f);
        for (int32_t i = 0; i < frames; ++i) {
            const float tone = (sine(osc[v], f) * 0.6f + sine(osc2[v], f * 1.6f) * 0.4f) * amp[v].next();
            const float n = bp[v].bandpass(noise()) * aux[v].next() * 2.5f;
            out[i] += dsp::fastTanh((tone * (1.0f - snappy * 0.5f) + n * snappy) * 1.4f * g) * level;
        }
        break;
    }
    case TomLo: case TomMid: case TomHi: {
        const float base = params_.get(v == TomLo ? TomLoTune : v == TomMid ? TomMidTune : TomHiTune), level = params_.get(TomLevel) * kVoiceTrim[v];
        for (int32_t i = 0; i < frames; ++i) {
            const float hz = base * (1.0f + 1.5f * pitchEnv[v].next());
            out[i] += dsp::fastTanh(sine(osc[v], hz) * amp[v].next() * 1.4f * g) * level;
        }
        break;
    }
    case HatClosed: case HatOpen: {
        const float tune = params_.get(HatTune), level = params_.get(HatLevel) * kVoiceTrim[v];
        bp[v].set(params_.get(HatTone), 0.55f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(tune);
            out[i] += bp[v].bandpass(m) * amp[v].next() * 2.2f * g * level;
        }
        break;
    }
    case Cymbal: {
        const float level = params_.get(CymLevel) * kVoiceTrim[v], tone = params_.get(CymTone);
        bp[v].set(tone, 0.5f);
        bp2[v].set(tone * 0.4f, 0.4f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(params_.get(HatTune)) * 0.7f + noise() * 0.3f;
            const float a = amp[v].next();
            out[i] += (bp[v].bandpass(m) * a + bp2[v].bandpass(m) * aux[v].next() * 1.5f) * 2.0f * g * level;
        }
        break;
    }
    case Ride: {
        const float level = params_.get(RideLevel) * kVoiceTrim[v];
        bp[v].set(5200.0f, 0.6f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(params_.get(HatTune));
            const float ping = sine(osc[v], 1850.0f) * aux[v].next() * 0.5f;
            out[i] += (bp[v].bandpass(m) * 1.8f + ping) * amp[v].next() * g * level;
        }
        break;
    }
    case Clap: {
        const float level = params_.get(ClapLevel) * kVoiceTrim[v];
        bp[v].set(params_.get(ClapTone), 0.45f);
        for (int32_t i = 0; i < frames; ++i) {
            // three fast pulses ten milliseconds apart, then the tail
            if (clapPulse > 0) {
                clapTimer += 1.0f / sr;
                if (clapTimer >= 0.010f) { clapTimer = 0.0f; --clapPulse; aux[v].fireTau(sr, 0.008f); }
            }
            const float e = aux[v].next() * 1.5f + amp[v].next() * 0.6f;
            out[i] += bp[v].bandpass(noise()) * e * 2.5f * g * level;
        }
        break;
    }
    case Rim: {
        const float f = params_.get(RimTune), level = params_.get(RimLevel) * kVoiceTrim[v];
        bp[v].set(f * 2.5f, 0.6f);
        for (int32_t i = 0; i < frames; ++i) {
            const float s = sine(osc[v], f) * amp[v].next() + bp[v].bandpass(noise()) * aux[v].next() * 3.0f;
            out[i] += dsp::fastTanh(s * 2.0f * g) * level;
        }
        break;
    }
    case Cowbell: {
        const float f = params_.get(BellTune), level = params_.get(BellLevel) * kVoiceTrim[v];
        bp[v].set(f * 1.15f, 0.35f);
        for (int32_t i = 0; i < frames; ++i) {
            const float s = (square(osc[v], f) + square(osc2[v], f * 1.48f)) * 0.5f;
            out[i] += bp[v].bandpass(s) * amp[v].next() * 2.0f * g * level;
        }
        break;
    }
    case Clave: {
        const float f = params_.get(ClaveTune), level = params_.get(ClaveLevel) * kVoiceTrim[v];
        for (int32_t i = 0; i < frames; ++i) out[i] += sine(osc[v], f) * amp[v].next() * g * level;
        break;
    }
    default: break;
    }
}

bool Hexbeat::render(float *L, float * /*R*/, int32_t frames) {
    params_.tick();
    const float volume = params_.get(Volume);
    for (int32_t i = 0; i < frames; ++i) L[i] = 0.0f;
    for (int32_t v = 0; v < VoiceCount; ++v) {
        if (amp[v].active || aux[v].active) renderVoice(v, L, frames);
    }
    // Drive before the clipper and volume after it, so the volume doesn't
    // change how dirty the kit sounds.
    for (int32_t i = 0; i < frames; ++i) L[i] = dsp::fastTanh(L[i] * kDrive) * volume * kHouse;
    return false;
}

} // namespace acidulous::machine
