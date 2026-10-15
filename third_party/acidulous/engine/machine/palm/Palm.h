#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

namespace acidulous::machine {

/**
 * Palm: hand drums.
 *
 * Each note is a drum head tuned to the note: a handful of resonant modes at
 * the ratios a circular membrane rings at, or, for a tabla, the harmonic
 * ratios its loaded centre tunes it to. The hand decides the rest: where it
 * strikes (the middle sounds the low modes, the edge the high ones), how
 * long it stays on the head, and whether it stays there to damp it.
 *
 * Five strokes (open, slap, muted, bass and rim) or the velocity choosing
 * between them; the drum's body, a djembe's or a cajón's air ringing low
 * under a bass stroke; snares, rattles and jingles; a head that goes sharp
 * when struck hard and settles; the wrist pressing a bayan's or a talking
 * drum's head to bend it up; and rolls on held notes.
 */
class Palm final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Stroke, Position, Hand, Decay, Damp,
        Drop, Squeeze, Rattle, Body, Roll, Spread,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        // Kit mode: each key from kBaseNote is a pad with its own drum, stroke,
        // pitch (a MIDI note) and level, kPadParams values each from PadFirst.
        Kit, PadFirst,
        Count = PadFirst + 16 * 4
    };
    static constexpr int kPads = 16, kPadParams = 4;
    enum PadParam : int32_t { PadModel = 0, PadStroke, PadNote, PadLevel };
    static constexpr int32_t padParam(int pad, int which) { return PadFirst + pad * kPadParams + which; }
    static constexpr uint8_t kBaseNote = 36;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t { Tabla = 0, Bayan, Djembe, Cajon, Frame, Talking, Conga, Bongo, Darbuka,
                         Riq, Tar, Bendir, Kanjira, Bata, Mridangam, Dholak, Ashiko,
                         Udu, Cuica, KindCount };
    enum StrokeKind : int32_t { Open = 0, Slap, Muted, Bass, Rim, ByVelocity, StrokeCount };
    static constexpr int kVoices = 8;
    static constexpr int kModes = 10;

    Palm();

    const char *typeName() const override { return "Palm"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Drums sounding, for tests. */
    int activeVoices() const;

  private:
    struct Mode {
        float a1 = 0.0f, a2 = 0.0f, b0 = 0.0f;
        float ratio = 1.0f, amp = 0.0f, t60 = 1.0f, r = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
    };
    struct Voice {
        Mode modes[kModes + 1]; // the head's, then the body's
        int modeCount = 0;
        /** A re-strike's damping of what's still ringing, spread over a few milliseconds so it doesn't click. */
        float settle = 1.0f;
        int32_t settleLeft = 0;
        bool used = false, held = false;
        uint8_t note = 0;
        /** A cuíca's stick, rubbed while the note is held: where in its cycle, and how far a sample. */
        float rubPhase = 0.0f, rubStep = 0.0f;
        /** The drum it is, and in kit mode the pad it came from, or -1. */
        int kind = 0, pad = -1;
        float velocity = 0.8f;
        float pitch = 60.0f, builtPitch = -1000.0f;
        float gain = 0.0f, pan = 0.0f;
        int stroke = Open;
        std::vector<float> strike;
        int32_t strikeLength = 0, strikeAt = 0;
        /** A head struck hard goes sharp and settles: semitones left. */
        float drop = 0.0f;
        /** The crack of a slap or the knock of a rim, and the snares. */
        float crack = 0.0f, crackLast = 0.0f;
        dsp::Svf crackTone, rattleTone;
        float shake = 0.0f;
        /** The hand pressing the head: pressure bends it up, slide damps it. */
        float noteBend = 0.0f, pressure = -1.0f, squeezed = 0.0f;
        float press = 0.0f, builtPress = -1.0f;
        int32_t rollLeft = 0;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    int strokeFor(const Voice &v, float velocity) const;
    /** Sets up the modes for the voice's drum and stroke. */
    void build(Voice &v);
    /** Tunes the modes to the voice's pitch now. */
    void retune(Voice &v);
    void strike(Voice &v, float velocity);
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    int retuneCountdown = 0;
    uint32_t noise = 0x68e31da4u;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
