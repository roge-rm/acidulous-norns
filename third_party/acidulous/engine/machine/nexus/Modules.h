#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iterator>
#include <engine/core/InputBus.h>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/LfoGen.h>
#include <engine/dsp/Math.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/dsp/Osc.h>
#include <engine/dsp/Swell.h>
#include <engine/dsp/Wavetable.h>
#include <engine/effect/EffectRegistry.h>
#include <engine/inputmod/Scales.h>
#include <engine/machine/filament/Waveguide.h>
#include <engine/machine/manual/Rotary.h>
#include <engine/machine/manual/Wheels.h>
#include <engine/machine/nexus/NexusModule.h>

// The modules. Many of them reuse the other machines: the string is
// Filament's waveguide, the wheels are Manual's generator, the cabinet is
// Manual's rotary and the wavetable is Trinity's bank.
//
// Every knob is 0..1 and each module maps its own ranges. That keeps the 128
// slot parameters interchangeable, so a slot keeps its automation when its
// module changes.
namespace acidulous::machine::nexus {
using namespace dsp;

enum Type : int32_t {
    TBlank = 0, TVoice, TPerf, TMacro, TOut, TScope,
    TOsc, TWtOsc, TNoise, TString, TWheels, TOp, TGrain, TAudioIn,
    TFilter, TVca, TMix, TMath, TDelay, TRotary, TBands,
    TEnv, TLfo, TSnh, TSlew,
    TClock, TEuclid, TProb, TRand, TQuant, TLogic,
    // New modules go on the end.
    TTouch, TSwell,
    TReverb, TChorus, TPhaser, TCrush, TShift, TDrive,
    TBore, TPipe, TReed, TJaw, TPiano, TThroat, TFormula, TFollow,
    TGuitar, TMallets, TSitar, TDrum, TPipes, TBird, TWater,
    TypeCount
};

enum Capability : uint8_t { CapPoly = 1, CapMono = 2, CapBoth = 3 };

struct ModuleInfo {
    const char *name;
    const char *knob[kKnobs];
    float def[kKnobs];
    const char *in[kPorts];
    const char *out[kPorts];
    uint8_t cap;
};

// --- knob helpers -----------------------------------------------------------
inline float lin(float k, float lo, float hi) { return lo + (hi - lo) * clampf(k, 0.0f, 1.0f); }
inline float expo(float k, float lo, float hi) { return lo * std::pow(hi / lo, clampf(k, 0.0f, 1.0f)); }
inline int stepOf(float k, int n) { return static_cast<int>(clampf(k, 0.0f, 0.9999f) * n); }
inline float hzOf(float note) { return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f); }

// --- the blocks -------------------------------------------------------------

/** The keyboard, as a module. */
class VoiceMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override { bendRange = lin(k[0], 0.0f, 24.0f); glide = expo(k[1], 0.001f, 2.0f); }
    void step(const float *, float *out, const Context &c) override {
        const float want = c.voicePitch() + c.bend * bendRange;
        pitch += (want - pitch) * clampf(1.0f / (glide * c.sampleRate), 0.0f, 1.0f);
        out[0] = pitch / 127.0f;              // pitch as 0..1 of the MIDI range
        out[1] = c.voiceGate();
        out[2] = c.voiceVelocity();
        out[3] = c.voiceRandom();
        out[4] = c.voiceTrigger();
    }
    bool monoCapable() const override { return false; }
  private:
    float pitch = 60.0f, bendRange = 2.0f, glide = 0.001f;
};

/**
 * Per-note MPE input: this voice's pressure (or the track's if the note
 * sends none) and its slide. It's separate from the voice module to keep
 * that one's jacks from crowding.
 */
class TouchMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *) override {}
    void step(const float *, float *out, const Context &c) override {
        out[0] = c.voicePressure();
        out[1] = c.voiceTimbre();
    }
    bool monoCapable() const override { return false; }
};

/** Mod wheel, pressure and bend. */
class PerfMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *) override {}
    void step(const float *, float *out, const Context &c) override {
        out[0] = c.modWheel;
        out[1] = c.pressure;
        out[2] = c.bend;
    }
    bool polyCapable() const override { return false; }
};

/** The eight macro knobs, so a cable depth can be put on one. */
class MacroMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *) override {}
    void step(const float *, float *out, const Context &c) override {
        for (int i = 0; i < kMacros && i < kPorts; ++i) out[i] = c.macro[i];
    }
    bool polyCapable() const override { return false; }
};

/**
 * The output. It has left and right inputs so stereo modules like the rotary
 * cabinet stay stereo. If `in R` is empty it mirrors `in`, so mono patches
 * work as they are. When both are wired, `pan` acts as a balance.
 */
class OutMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override { level = lin(k[0], 0.0f, 2.0f); pan = lin(k[1], -1.0f, 1.0f); }
    void setConnected(uint32_t mask) override { stereo = (mask & (1u << 2)) != 0; }
    void step(const float *in, float *out, const Context &) override {
        // Feedback patches can blow up, so the output saturates to keep them
        // from reaching the speakers as huge values.
        float l = in[0] * level;
        float r = (stereo ? in[2] : in[0]) * level;
        if (l > 1.0f || l < -1.0f) l = std::tanh(l);
        if (r > 1.0f || r < -1.0f) r = std::tanh(r);
        if (!std::isfinite(l)) l = 0.0f;
        if (!std::isfinite(r)) r = 0.0f;
        const float p = clampf(pan + in[1], -1.0f, 1.0f);
        const float angle = (p + 1.0f) * 0.25f * 3.14159265f;
        // Equal power pan for mono, balance for stereo, using the same gains.
        // At centre each side gets 0.7071 * 1.4142, which is unity.
        out[0] = l * std::cos(angle) * 1.4142f;
        out[1] = r * std::sin(angle) * 1.4142f;
        last = 0.5f * (l + r);
    }
    float lastOut() const { return last; }
  private:
    float level = 1.0f, pan = 0.0f, last = 0.0f;
    bool stereo = false;
};

/** Passes its input through and keeps the last few thousand samples to draw. */
class ScopeMod final : public Module {
  public:
    static constexpr int kPoints = 512;
    void prepare(float, int32_t) override { reset(); }
    void reset() override { for (auto &v : ring) v = 0.0f; write = 0; hold = 0; }
    void setKnobs(const float *k) override { stride = 1 + stepOf(k[0], 64); }
    void step(const float *in, float *out, const Context &) override {
        out[0] = in[0];
        out[1] = in[1];
        if (++hold >= stride) {
            hold = 0;
            ring[static_cast<size_t>(write)] = in[0];
            write = (write + 1) % kPoints;
        }
    }
    bool polyCapable() const override { return false; }
    int32_t copyTo(float *dest, int32_t max) const {
        const int32_t n = max < kPoints ? max : kPoints;
        for (int32_t i = 0; i < n; ++i) dest[i] = ring[static_cast<size_t>((write + i) % kPoints)];
        return n;
    }
  private:
    float ring[kPoints] = {};
    int32_t write = 0, stride = 8, hold = 0;
};

/** Saw, pulse, triangle and sine, band-limited. */
class OscMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { osc.setSampleRate(sr); rate = sr; }
    void reset() override { osc.reset(0.0f); }
    void setKnobs(const float *k) override {
        wave = stepOf(k[0], 4);
        semis = lin(k[1], -24.0f, 24.0f);
        fine = lin(k[2], -0.5f, 0.5f);
        width = lin(k[3], 0.05f, 0.95f);
        fmAmount = lin(k[4], 0.0f, 48.0f);
        level = lin(k[5], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float note = (in[0] != 0.0f ? in[0] * 127.0f : c.voicePitch()) + semis + fine + in[1] * fmAmount;
        osc.setFrequency(clampf(hzOf(note), 0.05f, rate * 0.45f));
        float v = 0.0f;
        switch (wave) {
        case 1: v = osc.pulse(clampf(width + in[2], 0.02f, 0.98f)); break;
        case 2: { const float s = osc.saw(); tri += (s - tri) * 0.35f; v = tri * 1.8f; break; }
        case 3: { const float s = osc.saw(); v = std::sin(3.14159265f * s); break; }
        default: v = osc.saw(); break;
        }
        out[0] = v * level;
    }
  private:
    Osc osc;
    float rate = 48000.0f, semis = 0.0f, fine = 0.0f, width = 0.5f, fmAmount = 0.0f, level = 1.0f, tri = 0.0f;
    int wave = 0;
};

/** Trinity's wavetable bank, as a block. */
class WtOscMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; bank = &WavetableBank::instance(); }
    void reset() override { phase = 0.0f; }
    void setKnobs(const float *k) override {
        table = stepOf(k[0], 8);
        frame = clampf(k[1], 0.0f, 0.999f);
        semis = lin(k[2], -24.0f, 24.0f);
        fmAmount = lin(k[3], 0.0f, 48.0f);
        level = lin(k[4], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float note = (in[0] != 0.0f ? in[0] * 127.0f : c.voicePitch()) + semis + in[1] * fmAmount;
        const float hz = clampf(hzOf(note), 0.05f, rate * 0.45f);
        phase += hz / rate;
        if (phase >= 1.0f) phase -= 1.0f;
        const float f = clampf(frame + in[2], 0.0f, 0.999f) * 7.0f;
        const int fi = static_cast<int>(f);
        out[0] = bank->sample(table, fi, f - static_cast<float>(fi), WavetableBank::mipFor(hz), phase) * level;
    }
  private:
    const WavetableBank *bank = nullptr;
    float rate = 48000.0f, phase = 0.0f, frame = 0.0f, semis = 0.0f, fmAmount = 0.0f, level = 1.0f;
    int table = 0;
};

class NoiseMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { pink = 0.0f; }
    void setKnobs(const float *k) override { colour = clampf(k[0], 0.0f, 1.0f); level = lin(k[1], 0.0f, 1.0f); }
    void step(const float *, float *out, const Context &) override {
        state = state * 1664525u + 1013904223u;
        const float white = (static_cast<float>((state >> 9) & 0xffff) / 32768.0f) - 1.0f;
        pink += (white - pink) * 0.05f;
        out[0] = (white * (1.0f - colour) + pink * colour * 3.0f) * level;
    }
  private:
    uint32_t state = 0x1234567u;
    float pink = 0.0f, colour = 0.0f, level = 1.0f;
};

/** Filament's waveguide, with a jack on the exciter. */
class StringMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { string.prepare(sr); }
    void reset() override { string.clear(); }
    void setKnobs(const float *k) override {
        semis = lin(k[0], -24.0f, 24.0f);
        sustain = clampf(k[1], 0.0f, 1.0f);
        tone = clampf(k[2], 0.02f, 1.0f);
        stiffness = clampf(k[3], 0.0f, 1.0f);
        tension = clampf(k[4], 0.0f, 1.0f);
        damperPos = clampf(k[5], 0.0f, 1.0f);
        damperPressure = clampf(k[6], 0.0f, 1.0f);
        level = lin(k[7], 0.0f, 2.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float note = (in[1] != 0.0f ? in[1] * 127.0f : c.voicePitch()) + semis;
        string.setFrequency(hzOf(note));
        string.setDamping(0.9f + 0.0999f * clampf(sustain + in[2], 0.0f, 1.0f), tone);
        string.setDispersion(stiffness, 4);
        string.setTension(tension);
        string.setDamper(damperPos, damperPressure);
        out[0] = string.step(in[0]) * level;
        out[1] = string.level();
    }
  private:
    Waveguide string;
    float semis = 0.0f, sustain = 0.9f, tone = 0.5f, stiffness = 0.0f, tension = 0.0f;
    float damperPos = 0.5f, damperPressure = 0.0f, level = 1.0f;
};

/** A tap off Manual's tonewheel generator. */
class WheelsMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; bank = &WheelBank::shared(sr); }
    void reset() override { phase = 0.0f; }
    void setKnobs(const float *k) override {
        timbre = stepOf(k[0], WheelBank::kTimbres);
        semis = std::round(lin(k[1], -24.0f, 24.0f));
        level = lin(k[2], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float note = (in[0] != 0.0f ? in[0] * 127.0f : c.voicePitch()) + semis;
        int wheel = static_cast<int>(std::round(note)) - WheelBank::kLowestNote;
        while (wheel >= WheelBank::kWheels) wheel -= 12;
        while (wheel < 0) wheel += 12;
        phase += bank->freq(wheel) / rate;
        if (phase >= 1.0f) phase -= 1.0f;
        out[0] = bank->sample(wheel, timbre, phase) * level;
    }
  private:
    const WheelBank *bank = nullptr;
    float rate = 48000.0f, phase = 0.0f, semis = 0.0f, level = 1.0f;
    int timbre = 1;
};

/** One FM operator, in Ratio's eight flavours. Patch the algorithm yourself. */
class OpMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; }
    void reset() override { phase = 0.0f; last = 0.0f; }
    void setKnobs(const float *k) override {
        mode = stepOf(k[0], 8);
        ratio = std::round(lin(k[1], 0.5f, 16.0f) * 2.0f) * 0.5f;
        fine = lin(k[2], -0.5f, 0.5f);
        feedback = lin(k[3], 0.0f, 1.0f);
        level = lin(k[4], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float note = (in[2] != 0.0f ? in[2] * 127.0f : c.voicePitch());
        const float hz = clampf(hzOf(note) * ratio + fine, 0.05f, rate * 0.45f);
        phase += hz / rate;
        if (phase >= 1.0f) phase -= 1.0f;
        const float mod = in[0] + last * feedback * 0.5f;
        const float ph = phase + mod;
        float v = std::sin(6.2831853f * ph);
        switch (mode) {
        case 1: v *= in[1]; break;                                  // ring
        case 2: lp += (v - lp) * clampf(0.05f + in[1], 0.001f, 1.0f); v = lp; break;
        case 3: v = std::sin(6.2831853f * ph) * (0.5f + 0.5f * in[1]); break;
        case 4: v = std::sin(6.2831853f * ph * (1.0f + std::fabs(mod))); break; // fold-ish
        case 5: if (phase < hz / rate) sync = 0.0f; v = std::sin(6.2831853f * (ph + sync)); break;
        case 6: v = std::sin(6.2831853f * (ph * ph)); break;        // phase distortion
        case 7: { const float q = 1.0f / (1.0f + 30.0f * (1.0f - feedback)); v = std::round(v / q) * q; break; }
        default: break;                                              // plain fm
        }
        last = v;
        out[0] = v * level;
    }
  private:
    float rate = 48000.0f, phase = 0.0f, ratio = 1.0f, fine = 0.0f, feedback = 0.0f, level = 1.0f;
    float last = 0.0f, lp = 0.0f, sync = 0.0f;
    int mode = 0;
};

/** A grain cloud over whatever has just gone into it. */
class GrainMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        rate = sr;
        buffer.assign(static_cast<size_t>(sr * 2.0f), 0.0f);
        reset();
    }
    void reset() override {
        for (auto &v : buffer) v = 0.0f;
        write = 0;
        for (auto &g : grain) g.life = 0;
        timer = 0;
    }
    void setKnobs(const float *k) override {
        position = clampf(k[0], 0.0f, 1.0f);
        sizeSec = expo(k[1], 0.005f, 0.4f);
        density = expo(k[2], 0.5f, 80.0f);
        spray = clampf(k[3], 0.0f, 1.0f);
        pitch = lin(k[4], -24.0f, 24.0f);
        level = lin(k[5], 0.0f, 2.0f);
    }
    void step(const float *in, float *out, const Context &) override {
        buffer[static_cast<size_t>(write)] = in[0];
        write = (write + 1) % static_cast<int32_t>(buffer.size());

        if (--timer <= 0) {
            timer = static_cast<int32_t>(rate / clampf(density + in[2] * 60.0f, 0.5f, 200.0f));
            for (auto &g : grain) {
                if (g.life > 0) continue;
                seed = seed * 1664525u + 1013904223u;
                const float r = (static_cast<float>((seed >> 9) & 0xffff) / 65536.0f) - 0.5f;
                const float pos = clampf(position + in[1] + r * spray, 0.0f, 1.0f);
                g.read = static_cast<float>(write) - pos * static_cast<float>(buffer.size() - 4) - 4.0f;
                while (g.read < 0.0f) g.read += static_cast<float>(buffer.size());
                g.life = g.total = static_cast<int32_t>(sizeSec * rate);
                g.inc = std::pow(2.0f, pitch / 12.0f);
                break;
            }
        }
        float sum = 0.0f;
        for (auto &g : grain) {
            if (g.life <= 0) continue;
            const int32_t i0 = static_cast<int32_t>(g.read) % static_cast<int32_t>(buffer.size());
            const float w = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(g.total - g.life) /
                                                   static_cast<float>(g.total));
            sum += buffer[static_cast<size_t>(i0)] * w;
            g.read += g.inc;
            if (g.read >= static_cast<float>(buffer.size())) g.read -= static_cast<float>(buffer.size());
            --g.life;
        }
        out[0] = sum * level * 0.5f;
    }
  private:
    struct Grain { float read = 0.0f, inc = 1.0f; int32_t life = 0, total = 1; };
    std::vector<float> buffer;
    Grain grain[12];
    int32_t write = 0, timer = 0;
    uint32_t seed = 0x77777u;
    float rate = 48000.0f, position = 0.5f, sizeSec = 0.08f, density = 12.0f, spray = 0.2f;
    float pitch = 0.0f, level = 1.0f;
};

/** The external audio input, as a module. */
class AudioInMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override { gain = lin(k[0], 0.0f, 4.0f); }
    void step(const float *, float *out, const Context &c) override {
        const InputBus &bus = InputBus::get();
        const int32_t frame = c.voice < 0 ? cursor : cursor;
        if (bus.live() && frame < bus.frames()) {
            out[0] = bus.block()[static_cast<size_t>(frame) * 2] * gain;
            out[1] = bus.block()[static_cast<size_t>(frame) * 2 + 1] * gain;
        } else {
            out[0] = out[1] = 0.0f;
        }
    }
    void setCursor(int32_t f) { cursor = f; }
    bool polyCapable() const override { return false; }
  private:
    float gain = 1.0f;
    int32_t cursor = 0;
};

class FilterMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { filter.setSampleRate(sr); rate = sr; }
    void reset() override {}
    void setKnobs(const float *k) override {
        type = stepOf(k[0], 12);
        cutoff = clampf(k[1], 0.0f, 1.0f);
        res = clampf(k[2], 0.0f, 1.0f);
        drive = stepOf(k[3], 6);
        driveAmount = clampf(k[4], 0.0f, 1.0f);
        keyTrack = clampf(k[5], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        const float key = keyTrack * (c.voicePitch() - 60.0f) / 60.0f;
        const float hz = expo(clampf(cutoff + in[1] + key, 0.0f, 1.0f), 20.0f, 18000.0f);
        filter.set(clampf(hz, 20.0f, rate * 0.45f), clampf(res + in[2], 0.0f, 1.0f), type, drive, driveAmount);
        out[0] = filter.process(in[0]);
    }
  private:
    MultiFilter filter;
    float rate = 48000.0f, cutoff = 0.5f, res = 0.0f, driveAmount = 0.0f, keyTrack = 0.0f;
    int type = 0, drive = 0;
};

class VcaMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override { offset = clampf(k[0], 0.0f, 1.0f); expo_ = k[1] > 0.5f; }
    void step(const float *in, float *out, const Context &) override {
        float g = clampf(in[1] + offset, 0.0f, 4.0f);
        if (expo_) g = g * g;
        out[0] = in[0] * g;
    }
  private:
    float offset = 0.0f;
    bool expo_ = false;
};

class MixMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override {
        for (int i = 0; i < 4; ++i) level[i] = lin(k[i], -2.0f, 2.0f);
        out_ = lin(k[4], 0.0f, 2.0f);
    }
    void step(const float *in, float *out, const Context &) override {
        out[0] = (in[0] * level[0] + in[1] * level[1] + in[2] * level[2] + in[3] * level[3]) * out_;
    }
  private:
    float level[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out_ = 1.0f;
};

class MathMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *k) override { mode = stepOf(k[0], 9); amount = lin(k[1], -2.0f, 2.0f); }
    void step(const float *in, float *out, const Context &) override {
        const float a = in[0], b = in[1] + amount;
        switch (mode) {
        case 1: out[0] = a - b; break;
        case 2: out[0] = a * b; break;
        case 3: out[0] = std::fmin(a, b); break;
        case 4: out[0] = std::fmax(a, b); break;
        case 5: out[0] = std::fabs(a); break;
        case 6: out[0] = -a; break;
        case 7: out[0] = a > 0.0f ? a : 0.0f; break;
        case 8: out[0] = a > b ? 1.0f : 0.0f; break;
        default: out[0] = a + b; break;
        }
    }
  private:
    float amount = 0.0f;
    int mode = 0;
};

class DelayMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; line.prepare(static_cast<int32_t>(sr * 2.0f)); }
    void reset() override { line.clear(); }
    void setKnobs(const float *k) override {
        timeSec = expo(k[0], 0.002f, 2.0f);
        feedback = clampf(k[1], 0.0f, 1.05f);
        tone = clampf(k[2], 0.02f, 1.0f);
        mix = clampf(k[3], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &) override {
        const float t = clampf((timeSec + in[1]) * rate, 2.0f, static_cast<float>(rate * 2.0f - 2.0f));
        const float wet = line.read(t);
        lp += (wet - lp) * tone;
        if (!std::isfinite(lp)) lp = 0.0f;
        // Feedback above unity is allowed, but the line saturates so it
        // doesn't grow without limit.
        line.write(clampf(std::tanh(in[0] + lp * feedback), -2.0f, 2.0f));
        out[0] = in[0] * (1.0f - mix) + lp * mix;
    }
    bool polyCapable() const override { return false; }
  private:
    DelayLine line;
    float rate = 48000.0f, timeSec = 0.25f, feedback = 0.3f, tone = 0.5f, mix = 0.5f, lp = 0.0f;
};

/** The Swell effect's upward compressor, with a cable on the amount. */
class SwellMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { swell.prepare(sr); }
    void reset() override { swell.reset(); }
    void setKnobs(const float *k) override {
        dsp::Swell::Settings s;
        s.floorDb = lin(k[0], -80.0f, 0.0f);
        s.ceilingDb = lin(k[1], -30.0f, 0.0f);
        s.amount = amount = clampf(k[2], 0.0f, 1.0f);
        s.split = clampf(k[3], 0.0f, 1.0f);
        s.releaseSec = expo(k[4], 0.005f, 2.0f);
        s.mix = clampf(k[5], 0.0f, 1.0f);
        swell.set(s);
    }
    void step(const float *in, float *out, const Context &) override {
        if (in[1] != 0.0f || moved) {
            swell.setAmount(amount + in[1]);
            moved = in[1] != 0.0f;
        }
        out[0] = swell.process(in[0]);
    }
  private:
    dsp::Swell swell;
    float amount = 0.5f;
    bool moved = false; // the amount cable was in use last sample, so put the knob's back when it stops
};

const ModuleInfo &infoFor(int32_t type);

/**
 * One of the insert effects as a module: the effect itself, so it sounds
 * exactly as it does on a track.
 *
 * The module's knobs are the effect's parameters of the same names, as
 * 0..1, and its second input is added to the parameter it's named after.
 * The effect runs in blocks of [kBlock], which is the module's latency:
 * a third of a millisecond, short enough not to matter for these.
 */
class FxMod final : public Module {
  public:
    static constexpr int kBlock = 16;

    FxMod(int32_t type, const char *effect) : type(type), effect(effect) {}
    ~FxMod() override { delete fx; }

    void prepare(float sr, int32_t) override {
        if (fx == nullptr) fx = EffectRegistry::create(effect);
        if (fx == nullptr) return;
        fx->prepare(static_cast<int32_t>(sr));
        const ModuleInfo &info = infoFor(type);
        for (int i = 0; i < kKnobs; ++i) param[i] = info.knob[i] != nullptr ? fx->params().indexOf(info.knob[i]) : -1;
        cvParam = info.in[1] != nullptr ? fx->params().indexOf(info.in[1]) : -1;
        reset();
    }
    void reset() override {
        if (fx != nullptr) fx->reset();
        std::fill(std::begin(inBuf), std::end(inBuf), 0.0f);
        std::fill(std::begin(outL), std::end(outL), 0.0f);
        std::fill(std::begin(outR), std::end(outR), 0.0f);
        pos = 0;
    }
    void setKnobs(const float *k) override {
        for (int i = 0; i < kKnobs; ++i) knob[i] = k[i];
    }
    void step(const float *in, float *out, const Context &c) override {
        out[0] = outL[pos];
        out[1] = outR[pos];
        inBuf[pos] = in[0];
        if (pos == 0) {
            // What the block's first sample saw, which is when the effect
            // will think the block starts.
            cv = in[1];
            blockTick = c.tick;
        }
        if (++pos < kBlock) return;
        pos = 0;
        if (fx == nullptr) return;
        for (int i = 0; i < kKnobs; ++i) {
            if (param[i] < 0) continue;
            const float v = knob[i] + (param[i] == cvParam ? cv : 0.0f);
            fx->params().set(param[i], clampf(v, 0.0f, 1.0f));
        }
        fx->onBlock(static_cast<int64_t>(blockTick), static_cast<int64_t>(blockTick + c.tickInc * kBlock), c.bpm);
        std::copy(std::begin(inBuf), std::end(inBuf), outL);
        std::copy(std::begin(inBuf), std::end(inBuf), outR);
        if (!fx->run(outL, outR, kBlock, false)) std::copy(std::begin(outL), std::end(outL), outR);
    }
  private:
    int32_t type;
    const char *effect;
    Effect *fx = nullptr;
    int32_t param[kKnobs]{};
    int32_t cvParam = -1;
    float knob[kKnobs]{};
    float cv = 0.0f;
    double blockTick = 0.0;
    float inBuf[kBlock]{}, outL[kBlock]{}, outR[kBlock]{};
    int pos = 0;
};

/** Manual's rotary cabinet. */
class RotaryMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rotary.prepare(sr); }
    void reset() override { rotary.reset(); }
    void setKnobs(const float *k) override {
        horn = expo(k[0], 0.1f, 10.0f);
        drum = expo(k[1], 0.1f, 8.0f);
        ramp = expo(k[2], 0.05f, 4.0f);
        distance = clampf(k[3], 0.0f, 1.0f);
        angle = clampf(k[4], 0.0f, 1.0f);
        spread = clampf(k[5], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &) override {
        rotary.setTargets(horn * (1.0f + in[1]), drum * (1.0f + in[1]), ramp, ramp * 1.7f);
        rotary.setMic(distance, angle, spread);
        rotary.process(in[0], out[0], out[1]);
    }
    bool polyCapable() const override { return false; }
  private:
    Rotary rotary;
    float horn = 6.6f, drum = 5.3f, ramp = 0.9f, distance = 0.35f, angle = 0.8f, spread = 0.7f;
};

/** Sixteen bands of one signal imposed on another. */
class BandsMod final : public Module {
  public:
    static constexpr int kBands = 16;
    /**
     * Makeup gain for the sixteen normalised band passes, so the block comes
     * out at a usable level. Cipher has the same constant.
     */
    static constexpr float kMakeup = 8.0f;
    void prepare(float sr, int32_t) override {
        rate = sr;
        for (int i = 0; i < kBands; ++i) {
            const float hz = 120.0f * std::pow(60.0f, static_cast<float>(i) / (kBands - 1));
            centre[i] = hz;
            analysis[i].setSampleRate(sr);
            synthesis[i].setSampleRate(sr);
        }
        reset();
    }
    void reset() override {
        for (int i = 0; i < kBands; ++i) { analysis[i].reset(); synthesis[i].reset(); env[i] = 0.0f; }
    }
    /**
     * The 32 filters are tuned here once a block instead of in `step()`,
     * since nothing depends on the signal. Doing it per sample was far too
     * slow on a phone.
     */
    void setKnobs(const float *k) override {
        shift = lin(k[0], -12.0f, 12.0f);
        follow = expo(k[1], 0.002f, 0.5f);
        width = clampf(k[2], 0.0f, 1.0f);
        level = lin(k[3], 0.0f, 4.0f);
        const float res = clampf((2.0f - 1.0f / (3.6f * (0.5f + width))) / 1.96f, 0.0f, 0.99f);
        coeff = 1.0f - std::exp(-1.0f / std::fmax(1.0f, follow * rate));
        const float ratio = std::pow(2.0f, shift / 12.0f);
        for (int i = 0; i < kBands; ++i) {
            analysis[i].set(centre[i], res);
            synthesis[i].set(clampf(centre[i] * ratio, 20.0f, rate * 0.45f), res);
            // The band pass output is scaled by Q, so this normalises it and
            // `width` doesn't change the level. Same as Cipher.
            aNorm[i] = analysis[i].bandNorm();
            sNorm[i] = synthesis[i].bandNorm();
        }
    }
    void step(const float *in, float *out, const Context &) override {
        float sum = 0.0f, loud = 0.0f;
        for (int i = 0; i < kBands; ++i) {
            const float a = std::fabs(analysis[i].bandpass(in[1]) * aNorm[i]);
            env[i] = undenormal(env[i] + (a - env[i]) * coeff);
            loud += env[i];
            sum += synthesis[i].bandpass(in[0]) * sNorm[i] * env[i];
        }
        out[0] = sum * level * kMakeup;
        out[1] = clampf(loud * 0.5f, 0.0f, 1.0f);
    }
    bool polyCapable() const override { return false; }
  private:
    Svf analysis[kBands], synthesis[kBands];
    float centre[kBands] = {}, env[kBands] = {};
    float aNorm[kBands] = {}, sNorm[kBands] = {};
    float rate = 48000.0f, shift = 0.0f, follow = 0.02f, width = 0.5f, level = 1.0f;
    float coeff = 0.001f;
};

class EnvMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { env.setSampleRate(sr); }
    void reset() override { env.kill(); gateWas = false; }
    void setKnobs(const float *k) override {
        a = expo(k[0], 0.0005f, 8.0f); d = expo(k[1], 0.002f, 12.0f);
        s = clampf(k[2], 0.0f, 1.0f);  r = expo(k[3], 0.002f, 12.0f);
        loop = k[4] > 0.5f;
    }
    void step(const float *in, float *out, const Context &) override {
        env.set(0.0f, a, d, s, r, loop);
        const bool gate = in[0] > 0.5f;
        if (gate && !gateWas) env.retrigger();
        if (!gate && gateWas) env.release();
        gateWas = gate;
        out[0] = env.next();
        out[1] = env.active() ? 1.0f : 0.0f;
    }
  private:
    Adsr env;
    float a = 0.01f, d = 0.3f, s = 0.7f, r = 0.2f;
    bool loop = false, gateWas = false;
};

class LfoMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; lfo.reset(0.0f); }
    void reset() override { lfo.reset(0.0f); }
    void setKnobs(const float *k) override {
        wave = stepOf(k[0], LfoGen::WaveCount);
        hz = expo(k[1], 0.01f, 40.0f);
        sync = stepOf(k[2], 6);
        slew = clampf(k[3], 0.0f, 1.0f);
        depth = lin(k[4], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        static const float beats[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f};
        const float f = sync == 0 ? hz * (1.0f + in[0] * 4.0f) : (c.bpm / 60.0f) / beats[sync];
        out[0] = lfo.advance(wave, f, 1.0f / rate, slew, false) * depth;
        out[1] = out[0] * 0.5f + 0.5f;
    }
  private:
    LfoGen lfo;
    float rate = 48000.0f, hz = 1.0f, slew = 0.0f, depth = 1.0f;
    int wave = 0, sync = 0;
};

class SnhMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { held = 0.0f; wasHigh = false; }
    void setKnobs(const float *k) override { track = k[0] > 0.5f; }
    void step(const float *in, float *out, const Context &) override {
        const bool high = in[1] > 0.5f;
        if (track ? high : (high && !wasHigh)) held = in[0];
        wasHigh = high;
        out[0] = held;
    }
  private:
    float held = 0.0f;
    bool track = false, wasHigh = false;
};

class SlewMod final : public Module {
  public:
    void prepare(float sr, int32_t) override { rate = sr; }
    void reset() override { value = 0.0f; }
    void setKnobs(const float *k) override { rise = expo(k[0], 0.0005f, 8.0f); fall = expo(k[1], 0.0005f, 8.0f); }
    void step(const float *in, float *out, const Context &) override {
        const float t = in[0] > value ? rise : fall;
        value += (in[0] - value) * clampf(1.0f / (t * rate), 0.0f, 1.0f);
        out[0] = value;
    }
  private:
    float rate = 48000.0f, rise = 0.01f, fall = 0.01f, value = 0.0f;
};

/** The song's transport, divided. */
class ClockMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { last = -1.0; }
    void setKnobs(const float *k) override { division = stepOf(k[0], 9); pulseWidth = clampf(k[1], 0.05f, 0.95f); }
    void step(const float *, float *out, const Context &c) override {
        // 240 ticks to the quarter note; the divisions are note values.
        static const double ticks[9] = {960, 480, 240, 120, 80, 60, 40, 30, 15};
        const double period = ticks[division];
        const double pos = std::fmod(c.tick, period) / period;
        out[0] = pos < pulseWidth ? 1.0f : 0.0f;
        out[1] = static_cast<float>(pos);
        out[2] = (last >= 0.0 && pos < last) ? 1.0f : 0.0f; // one-sample restart
        last = pos;
    }
    bool polyCapable() const override { return false; }
  private:
    double last = -1.0;
    float pulseWidth = 0.5f;
    int division = 2;
};

class EuclidMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { step_ = 0; wasHigh = false; }
    void setKnobs(const float *k) override {
        steps = 1 + stepOf(k[0], 16);
        pulses = stepOf(k[1], 17);
        rotate = stepOf(k[2], 16);
    }
    void step(const float *in, float *out, const Context &) override {
        const bool high = in[0] > 0.5f;
        if (high && !wasHigh) step_ = (step_ + 1) % steps;
        if (in[1] > 0.5f) step_ = 0;
        wasHigh = high;
        // Euclidean rhythm, worked out directly: a step is on when the running
        // count of pulses crosses an integer. Same pattern as Bjorklund.
        const int idx = (step_ + rotate) % steps;
        const int p = pulses > steps ? steps : pulses;
        const bool on = p > 0 && ((idx * p) % steps) < p;
        out[0] = (on && high) ? 1.0f : 0.0f;
        out[1] = on ? 1.0f : 0.0f;
    }
  private:
    int steps = 8, pulses = 4, rotate = 0, step_ = 0;
    bool wasHigh = false;
};

class ProbMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { wasHigh = false; pass = false; }
    void setKnobs(const float *k) override { chance = clampf(k[0], 0.0f, 1.0f); }
    void step(const float *in, float *out, const Context &) override {
        const bool high = in[0] > 0.5f;
        if (high && !wasHigh) {
            seed = seed * 1664525u + 1013904223u;
            const float r = static_cast<float>((seed >> 9) & 0xffff) / 65536.0f;
            pass = r < clampf(chance + in[1], 0.0f, 1.0f);
        }
        wasHigh = high;
        out[0] = (high && pass) ? 1.0f : 0.0f;
        out[1] = (high && !pass) ? 1.0f : 0.0f;
    }
  private:
    uint32_t seed = 0xbeef1u;
    float chance = 0.5f;
    bool wasHigh = false, pass = false;
};

class RandMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { value = 0.0f; wasHigh = false; }
    void setKnobs(const float *k) override {
        bipolar = k[0] > 0.5f;
        quantSteps = stepOf(k[1], 17);
    }
    void step(const float *in, float *out, const Context &) override {
        const bool high = in[0] > 0.5f;
        if (high && !wasHigh) {
            seed = seed * 1664525u + 1013904223u;
            float r = static_cast<float>((seed >> 9) & 0xffff) / 65536.0f;
            if (quantSteps > 1) r = std::round(r * (quantSteps - 1)) / (quantSteps - 1);
            value = bipolar ? r * 2.0f - 1.0f : r;
        }
        wasHigh = high;
        out[0] = value;
    }
  private:
    uint32_t seed = 0x5eed1u;
    float value = 0.0f;
    int quantSteps = 0;
    bool bipolar = false, wasHigh = false;
};

/** Pulls a control voltage onto a scale. */
class QuantMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { last = -999.0f; }
    void setKnobs(const float *k) override {
        scale = stepOf(k[0], music::kScaleCount);
        root = stepOf(k[1], 12);
    }
    void step(const float *in, float *out, const Context &) override {
        const float note = in[0] * 127.0f;
        const auto &def = music::kScales[scale];
        const int octave = static_cast<int>(std::floor((note - root) / 12.0f));
        const float within = note - root - static_cast<float>(octave) * 12.0f;
        float best = def.intervals[0], bestD = 99.0f;
        for (int i = 0; i < def.count; ++i) {
            const float d = std::fabs(within - static_cast<float>(def.intervals[i]));
            if (d < bestD) { bestD = d; best = static_cast<float>(def.intervals[i]); }
        }
        const float snapped = static_cast<float>(root) + static_cast<float>(octave) * 12.0f + best;
        out[0] = snapped / 127.0f;
        out[1] = std::fabs(snapped - last) > 0.01f ? 1.0f : 0.0f;
        last = snapped;
    }
  private:
    float last = -999.0f;
    int scale = 0, root = 0;
};

class LogicMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override { flip = false; wasA = false; }
    void setKnobs(const float *k) override { mode = stepOf(k[0], 6); }
    void step(const float *in, float *out, const Context &) override {
        const bool a = in[0] > 0.5f, b = in[1] > 0.5f;
        bool r = false;
        switch (mode) {
        case 1: r = a || b; break;
        case 2: r = a != b; break;
        case 3: r = !(a && b); break;
        case 4: if (a && !wasA) flip = !flip; r = flip; break;
        case 5: r = a && !b; break;
        default: r = a && b; break;
        }
        wasA = a;
        out[0] = r ? 1.0f : 0.0f;
        out[1] = r ? 0.0f : 1.0f;
    }
  private:
    int mode = 0;
    bool flip = false, wasA = false;
};

/** A module type this build doesn't know. Holds its place and outputs nothing. */
class BlankMod final : public Module {
  public:
    void prepare(float, int32_t) override {}
    void reset() override {}
    void setKnobs(const float *) override {}
    void step(const float *, float *out, const Context &) override { out[0] = 0.0f; }
};

const ModuleInfo &infoFor(int32_t type);
int32_t typeFromName(const char *name);
Module *makeModule(int32_t type);

} // namespace acidulous::machine::nexus
