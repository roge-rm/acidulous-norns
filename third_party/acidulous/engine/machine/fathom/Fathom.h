#pragma once
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

namespace acidulous::machine {

/**
 * Fathom: water and weather.
 *
 * Most of the sound of water is bubbles: a pocket of air caught under the
 * surface rings like a spring at a pitch set by its size (the smaller, the
 * higher), and rises in pitch as it nears the surface. A drip is a bubble and
 * the tap of the drop; rain is many drips, and the sound of what they land
 * on; a stream is a dense cloud of small bubbles and a few big ones; surf is
 * a wave's rush rising and falling. Wind is air rushing past things, in gusts,
 * and whistling where it's caught in a gap; fire is crackles, hiss and roar.
 *
 * A held note keeps the texture going, its pitch setting the size of the
 * bubbles or the note the wind whistles, its velocity how much.
 */
class Fathom final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Density, Size, Rise, Decay, Surface,
        Gust, Whistle, Tone, Swell, Spread, Fade,
        VelocityAmount, BendRange, Octave, Volume,
        // Kit mode: each key from kBaseNote is a pad with its own sound, pitch
        // (a MIDI note), density and level, kPadParams values each from PadFirst.
        Kit, PadFirst,
        Count = PadFirst + 16 * 4
    };
    static constexpr int kPads = 16, kPadParams = 4;
    enum PadParam : int32_t { PadModel = 0, PadNote, PadDensity, PadLevel };
    static constexpr int32_t padParam(int pad, int which) { return PadFirst + pad * kPadParams + which; }
    static constexpr uint8_t kBaseNote = 36;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t { Bubbles = 0, Drips, Rain, Stream, Surf, Wind, Fire,
                         Thunder, Hail, Waterfall, Sizzle, Ice, Snow, Splash, Underwater, Rainstick, KindCount };
    enum SurfaceKind : int32_t { Water = 0, Leaves, Tin, Glass, SurfaceCount };
    static constexpr int kVoices = 4;
    static constexpr int kGrains = 64;

    Fathom();

    const char *typeName() const override { return "Fathom"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Notes still sounding, and grains (bubbles, drops, crackles) started so far, for tests. */
    int activeVoices() const;
    uint32_t grainsStarted() const { return started; }

  private:
    /** One event: a bubble, a drop's tap, a struck surface or a crackle. */
    struct Grain {
        bool used = false;
        int kind = 0;
        /** A ringing grain: its phase, pitch now and how it moves, its level and decay. */
        float phase = 0.0f, hz = 0.0f, rise = 0.0f;
        float amp = 0.0f, fall = 0.0f;
        /** A noisy grain: its tone. */
        float lp = 0.0f, lpCoef = 0.5f, hpLast = 0.0f;
        float pan = 0.0f;
        int32_t age = 0;
    };
    enum GrainKind : int32_t { Ring = 0, Tap, Crackle };
    struct Voice {
        bool used = false, held = false;
        uint8_t note = 0;
        /** The sound it is, and in kit mode the pad it came from, or -1. */
        int kind = 0, pad = -1;
        float velocity = 0.8f;
        /** How strongly the texture's going, rising and falling with the note. */
        float level = 0.0f;
        /** Samples to the next grain. */
        float until = 0.0f;
        float noteBend = 0.0f, pressure = -1.0f;
        /** The texture's beds of noise: their filters and where the gusts and waves are. */
        dsp::Svf bed, whistle;
        float bedLp = 0.0f, roarLp = 0.0f, roarLp2 = 0.0f;
        float gustNow = 0.0f, gustAim = 0.0f;
        int32_t gustLeft = 0;
        double waveAt = 0.0;
        float peak = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    void spawn(Voice &v, int kind, float hz, float amp, float decaySeconds, float rise);
    /** The voice's events: what kind, and the next one's grains. */
    void event(Voice &v, float hz);
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    float uniform() { return 0.5f * (white() + 1.0f); }

    Voice voices[kVoices];
    Grain grains[kGrains];
    /** A struck tin roof or window: a few modes shared by every drop. */
    struct SurfaceMode {
        float a1 = 0.0f, a2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };
    SurfaceMode surface[4];
    int builtSurface = -1;
    float surfaceIn = 0.0f;
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    uint32_t noise = 0x4f6cdd1du;
    uint32_t clock = 0, started = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
