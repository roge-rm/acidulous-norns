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
 *
 * Cycles: a held note plays a bar-long stroke pattern in time with the song
 * (and its swing), a few bars round with a fill on the last, varied a little
 * each bar from a generator seeded by the bar's number, so a render repeats.
 * Two held notes share the pattern out: the bass strokes and low tones go
 * to the lower drum, the rest to the higher. The pattern's second hand
 * presses the head to bend it up, or lays on it to stop it ringing.
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
        // Each hit is two: a softer grace stroke, then the stroke this many
        // ms after it. 0 is off.
        Flam = PadFirst + 16 * 4,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit. The routes reach the drum params every
        // pad of a kit shares, not a pad's own.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        // Cycles, after the macros. The pattern (0 off), the bars in a cycle
        // (1, 2, 4 or 8, the last a fill), how much each bar varies, and how
        // far the second hand presses the head up, semitones.
        Cycle = MacroBase + 8 * MacroParams, Bars, Feel, Press,
        Count
    };
    // Where a macro route goes. Continuous params only: the model, tune,
    // stroke, voices, bend range, octave and the pads are left out.
    enum MacroDest : int32_t {
        McOff, McPosition, McHand, McDecay, McDamp, McDrop, McSqueeze, McRattle, McBody,
        McRoll, McSpread, McVelocity, McVolume, McFlam,
        McPitch,
        McFeel, McPress,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
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
    enum CycleKind : int32_t { CycleOff = 0, Tumbao, Kuku, Baladi, Maqsum, Theka, SixEight, Groove, Talk, CycleCount };

    Palm();

    const char *typeName() const override { return "Palm"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void onTransport(bool rolling, int64_t tickStart) override;
    void onBar(int64_t tickInBar, int32_t ticksPerBar, float swing, int64_t swingPair) override;
    void onCountIn(double ticksLeft) override { countLeft = ticksLeft; }
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
    /** A cycle's stroke waiting to land: samples to go, its velocity and its
     *  level against the note's, the stroke (-1 for the second hand laid on
     *  the head), semitones from the note, and whether the second hand presses. */
    struct Hit {
        int32_t wait = 0;
        float velocity = 0.0f, level = 0.0f, shift = 0.0f;
        int8_t stroke = 0;
        bool press = false;
    };
    static constexpr int kHits = 8;
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
        /** Samples until a flam's second stroke, 0 for none, and its velocity. */
        int32_t flamLeft = 0;
        float flamVelocity = 0.0f;
        /** The pitch macro at the note-on (in [pitch]) and now, in its 0..1 units. */
        float macAt = 0.0f, macNow = 0.0f;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
        /** Cycles: the note's velocity, strokes waiting, semitones the stroke
         *  is played from the note (a low tone), and the second hand's press
         *  now and where it's going, semitones. */
        float noteVelocity = 0.8f;
        Hit hits[kHits];
        int hitCount = 0;
        float shift = 0.0f, hand2 = 0.0f, hand2Aim = 0.0f;
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    /** Fills [mac_] from the smoothed macro knobs and [macGoal_] from their targets. */
    void updateMacros();
    /** Fills [macGoal_] alone, for a note-on between blocks. */
    void updateMacroGoals();
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    /** The voice's pitch now, with bend and the pitch macro's move since the note-on. */
    float pitchNow(const Voice &v) const {
        const float p = v.pitch + v.shift + bend * paramOf(BendRange) + v.noteBend;
        // Apart so a note with no pitch macro moving is worked out as before.
        return v.macNow != v.macAt ? p + (v.macNow - v.macAt) * 24.0f : p;
    }
    Voice *voiceFor(uint8_t note);
    int strokeFor(const Voice &v, float velocity) const;
    /** Sets up the modes for the voice's drum and stroke. */
    void build(Voice &v);
    /** Tunes the modes to the voice's pitch now. */
    void retune(Voice &v);
    /** Strikes with the param's stroke, or [stroke] if it's 0 or more. */
    void strike(Voice &v, float velocity, int stroke = -1);
    /** Queues the cycle's strokes that fall in this block. */
    void cycleStep(int32_t frames);
    /** Lands a queued cycle stroke on [v]. */
    void land(Voice &v, const Hit &h);
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
    /** Cycles: the tempo, the song's place (see Machine::onBar), a clock in
     *  ticks for when the song is stopped (from the first held note, or
     *  where the song stopped, or the count-in's), and the count-in left. */
    float bpm = 120.0f;
    bool songRolling = false;
    int64_t songTick = 0, barTick = 0, blockTicks = 0;
    int32_t barTicks = 0;
    float swingPercent = 50.0f;
    int64_t swingPairTicks = 0;
    double freeTick = 0.0, countLeft = 0.0;
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};
};

} // namespace acidulous::machine
