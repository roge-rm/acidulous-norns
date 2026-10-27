#include "Hexbeat.h"
#include <engine/machine/Voices.h>
#include <engine/core/Constants.h>
#include <engine/dsp/Math.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace acidulous::machine {

namespace {
const ParamDef kBase[Hexbeat::MacroBase] = {
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
    // Linear so the default is exactly the old fixed 0.375.
    {"drive", 0.125f, 2.125f, 0.375f, Curve::Linear, 0, ""},
};

// The base table plus the macros' knobs and routes.
struct Table {
    ParamDef defs[Hexbeat::Count];
    char names[Hexbeat::kMacros * Hexbeat::MacroParams][12];
    Table() {
        for (int32_t i = 0; i < Hexbeat::MacroBase; ++i) defs[i] = kBase[i];
        for (int k = 0; k < Hexbeat::kMacros; ++k) {
            const int32_t b = Hexbeat::MacroBase + k * Hexbeat::MacroParams;
            char *n = names[k * Hexbeat::MacroParams];
            std::snprintf(n, 12, "mac%d", k + 1);
            defs[b] = {n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            for (int r = 0; r < 4; ++r) {
                char *d = names[k * Hexbeat::MacroParams + 1 + r];
                std::snprintf(d, 12, "mac%d_d%d", k + 1, r + 1);
                defs[b + 1 + r] = {d, 0.0f, Hexbeat::DestCount - 1.0f, 0.0f, Curve::Stepped, Hexbeat::DestCount, ""};
                char *a = names[k * Hexbeat::MacroParams + 5 + r];
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

// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Hexbeat::Count];
    DestMap() {
        using H = Hexbeat;
        for (auto &d : of) d = -1;
        of[H::KickTune] = H::DstKickTune; of[H::KickDecay] = H::DstKickDecay;
        of[H::KickPunch] = H::DstKickPunch; of[H::KickLevel] = H::DstKickLevel;
        of[H::SnareTune] = H::DstSnareTune; of[H::SnareDecay] = H::DstSnareDecay;
        of[H::SnareSnappy] = H::DstSnareSnappy; of[H::SnareTone] = H::DstSnareTone;
        of[H::SnareLevel] = H::DstSnareLevel;
        of[H::TomLoTune] = of[H::TomMidTune] = of[H::TomHiTune] = H::DstTomTune;
        of[H::TomDecay] = H::DstTomDecay; of[H::TomLevel] = H::DstTomLevel;
        of[H::HatTune] = H::DstHatTune;
        of[H::HatClosedDecay] = of[H::HatOpenDecay] = H::DstHatDecay;
        of[H::HatTone] = H::DstHatTone; of[H::HatLevel] = H::DstHatLevel;
        of[H::CymDecay] = H::DstCymDecay; of[H::CymTone] = H::DstCymTone;
        of[H::CymLevel] = H::DstCymLevel;
        of[H::RideDecay] = H::DstRideDecay; of[H::RideLevel] = H::DstRideLevel;
        of[H::ClapDecay] = H::DstClapDecay; of[H::ClapTone] = H::DstClapTone;
        of[H::ClapLevel] = H::DstClapLevel;
        of[H::RimTune] = H::DstRimTune; of[H::RimLevel] = H::DstRimLevel;
        of[H::BellTune] = H::DstBellTune; of[H::BellDecay] = H::DstBellDecay;
        of[H::BellLevel] = H::DstBellLevel;
        of[H::ClaveTune] = H::DstClaveTune; of[H::ClaveLevel] = H::DstClaveLevel;
        of[H::Accent] = H::DstAccent; of[H::Drive] = H::DstDrive; of[H::Volume] = H::DstVolume;
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
// The classic six metal frequencies, as ratios of the lowest.
const float kMetalRatios[6] = {1.0f, 1.483f, 1.800f, 2.546f, 2.634f, 3.902f};
constexpr float kMetalBase = 205.0f;

// The house level. The drive param (default 0.375, -8.5 dB into the clipper
// so a single hit stays clean and it takes several at once to saturate) sets how dirty the kit is, this sets where a kit
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

const ParamDef *Hexbeat::paramDefs(int32_t &count) const { count = Count; return table().defs; }

void Hexbeat::updateMacros(bool goal) {
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, goal);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(read(b + 1 + r, goal) + 0.5f);
            if (dest > DstOff && dest < DestCount) mac_[dest] += knob * read(b + 5 + r, goal);
        }
    }
    // Two octaves either way keeps the clave (4 kHz) under the Nyquist.
    mac_[DstPitch] = std::clamp(mac_[DstPitch], -1.0f, 1.0f);
    pitchMul_ = mac_[DstPitch] == 0.0f ? 1.0f : std::exp2(mac_[DstPitch] * 2.0f);
}

float Hexbeat::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : mac_[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

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
    updateMacros(true); // a hit reads where the knobs are going, so renders repeat
    const float acc = velocity >= 100 ? val(Accent, true) : 0.0f;
    trigger(v, acc, velocityGain(static_cast<float>(velocity) / 127.0f, params_.target(Velocity)));
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
        amp[v].fire(sr, val(KickDecay, true) * ms);
        pitchEnv[v].fireTau(sr, 0.035f);
        aux[v].fireTau(sr, 0.004f); // click
        osc[v].phase = 0.0f;
        break;
    case Snare:
        amp[v].fire(sr, val(SnareDecay, true) * ms * 0.7f);
        aux[v].fire(sr, val(SnareDecay, true) * ms);
        osc[v].phase = osc2[v].phase = 0.0f;
        break;
    case TomLo: case TomMid: case TomHi:
        amp[v].fire(sr, val(TomDecay, true) * ms);
        pitchEnv[v].fireTau(sr, 0.05f);
        osc[v].phase = 0.0f;
        break;
    case HatClosed:
        amp[v].fire(sr, val(HatClosedDecay, true) * ms);
        if (amp[HatOpen].active) amp[HatOpen].fireTau(sr, 0.02f, amp[HatOpen].level); // choke
        break;
    case HatOpen: amp[v].fire(sr, val(HatOpenDecay, true) * ms); break;
    case Cymbal: amp[v].fire(sr, val(CymDecay, true) * ms); aux[v].fire(sr, val(CymDecay, true) * ms * 0.3f); break;
    case Ride: amp[v].fire(sr, val(RideDecay, true) * ms); aux[v].fireTau(sr, 0.25f); osc[v].phase = 0.0f; break;
    case Clap:
        amp[v].fire(sr, val(ClapDecay, true) * ms);
        aux[v].fireTau(sr, 0.008f);
        clapPulse = 3;
        clapTimer = 0.0f;
        break;
    case Rim: amp[v].fireTau(sr, 0.02f); aux[v].fireTau(sr, 0.003f); osc[v].phase = 0.0f; break;
    case Cowbell: amp[v].fire(sr, val(BellDecay, true) * ms); break;
    case Clave: amp[v].fireTau(sr, 0.04f); osc[v].phase = 0.0f; break;
    default: break;
    }
}

void Hexbeat::renderVoice(int32_t v, float *out, int32_t frames) {
    const float g = gain[v];
    switch (v) {
    case Kick: {
        const float base = val(KickTune) * pitchMul_, punch = val(KickPunch), level = val(KickLevel) * kVoiceTrim[v];
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
        const float f = val(SnareTune) * pitchMul_, snappy = val(SnareSnappy), level = val(SnareLevel) * kVoiceTrim[v];
        bp[v].set(val(SnareTone), 0.3f);
        for (int32_t i = 0; i < frames; ++i) {
            const float tone = (sine(osc[v], f) * 0.6f + sine(osc2[v], f * 1.6f) * 0.4f) * amp[v].next();
            const float n = bp[v].bandpass(noise()) * aux[v].next() * 2.5f;
            out[i] += dsp::fastTanh((tone * (1.0f - snappy * 0.5f) + n * snappy) * 1.4f * g) * level;
        }
        break;
    }
    case TomLo: case TomMid: case TomHi: {
        const float base = val(v == TomLo ? TomLoTune : v == TomMid ? TomMidTune : TomHiTune) * pitchMul_, level = val(TomLevel) * kVoiceTrim[v];
        for (int32_t i = 0; i < frames; ++i) {
            const float hz = base * (1.0f + 1.5f * pitchEnv[v].next());
            out[i] += dsp::fastTanh(sine(osc[v], hz) * amp[v].next() * 1.4f * g) * level;
        }
        break;
    }
    case HatClosed: case HatOpen: {
        const float tune = val(HatTune) * pitchMul_, level = val(HatLevel) * kVoiceTrim[v];
        bp[v].set(val(HatTone), 0.55f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(tune);
            out[i] += bp[v].bandpass(m) * amp[v].next() * 2.2f * g * level;
        }
        break;
    }
    case Cymbal: {
        const float level = val(CymLevel) * kVoiceTrim[v], tone = val(CymTone), htune = val(HatTune) * pitchMul_;
        bp[v].set(tone, 0.5f);
        bp2[v].set(tone * 0.4f, 0.4f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(htune) * 0.7f + noise() * 0.3f;
            const float a = amp[v].next();
            out[i] += (bp[v].bandpass(m) * a + bp2[v].bandpass(m) * aux[v].next() * 1.5f) * 2.0f * g * level;
        }
        break;
    }
    case Ride: {
        const float level = val(RideLevel) * kVoiceTrim[v], htune = val(HatTune) * pitchMul_;
        bp[v].set(5200.0f, 0.6f);
        for (int32_t i = 0; i < frames; ++i) {
            const float m = metallic(htune);
            const float ping = sine(osc[v], 1850.0f) * aux[v].next() * 0.5f;
            out[i] += (bp[v].bandpass(m) * 1.8f + ping) * amp[v].next() * g * level;
        }
        break;
    }
    case Clap: {
        const float level = val(ClapLevel) * kVoiceTrim[v];
        bp[v].set(val(ClapTone), 0.45f);
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
        const float f = val(RimTune) * pitchMul_, level = val(RimLevel) * kVoiceTrim[v];
        bp[v].set(f * 2.5f, 0.6f);
        for (int32_t i = 0; i < frames; ++i) {
            const float s = sine(osc[v], f) * amp[v].next() + bp[v].bandpass(noise()) * aux[v].next() * 3.0f;
            out[i] += dsp::fastTanh(s * 2.0f * g) * level;
        }
        break;
    }
    case Cowbell: {
        const float f = val(BellTune) * pitchMul_, level = val(BellLevel) * kVoiceTrim[v];
        bp[v].set(f * 1.15f, 0.35f);
        for (int32_t i = 0; i < frames; ++i) {
            const float s = (square(osc[v], f) + square(osc2[v], f * 1.48f)) * 0.5f;
            out[i] += bp[v].bandpass(s) * amp[v].next() * 2.0f * g * level;
        }
        break;
    }
    case Clave: {
        const float f = val(ClaveTune) * pitchMul_, level = val(ClaveLevel) * kVoiceTrim[v];
        for (int32_t i = 0; i < frames; ++i) out[i] += sine(osc[v], f) * amp[v].next() * g * level;
        break;
    }
    default: break;
    }
}

bool Hexbeat::render(float *L, float * /*R*/, int32_t frames) {
    params_.tick();
    updateMacros();
    const float volume = val(Volume), drive = val(Drive);
    for (int32_t i = 0; i < frames; ++i) L[i] = 0.0f;
    for (int32_t v = 0; v < VoiceCount; ++v) {
        if (amp[v].active || aux[v].active) renderVoice(v, L, frames);
    }
    // Drive before the clipper and volume after it, so the volume doesn't
    // change how dirty the kit sounds.
    for (int32_t i = 0; i < frames; ++i) L[i] = dsp::fastTanh(L[i] * drive) * volume * kHouse;
    return false;
}

} // namespace acidulous::machine
