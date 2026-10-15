#pragma once
#include <cstring>
#include <cmath>
#include <cstdint>
#include <engine/core/Params.h>

// A sound source in a rack. Built and prepare()d on a normal thread, then
// mounted; everything after that runs on the audio thread and must not
// allocate, lock, or block.
namespace acidulous {

class Machine {
  public:
    virtual ~Machine() = default;

    virtual const char *typeName() const = 0;
    virtual const ParamDef *paramDefs(int32_t &count) const = 0;

    // Mount thread, before hand-over.
    virtual void prepare(int32_t sampleRate) = 0;

    // --- Audio thread ---------------------------------------------------------
    // Once per block before render(), with the block's tick range and tempo,
    // for anything synced to the transport (like Trinity's LFOs).
    virtual void onBlock(int64_t /*tickStart*/, int64_t /*tickEnd*/, float /*bpm*/) {}
    /**
     * Where the rack is in the arrangement, for a machine that plays the song
     * instead of notes (Bias).
     *
     * Bias needs to know which cell it's in and how far through that cell's
     * cycle, which only the scheduler knows since it handles both arranger
     * and launcher positions. `cycleTick` counts through repeats, unlike the
     * tick notes fire against. See SceneScheduler::rackCycleTick.
     *
     * Machines that play notes ignore it.
     */
    virtual void onScene(int64_t /*sceneId*/, int64_t /*cycleTick*/, bool /*playing*/,
                         bool /*clipMuted*/) {}
    /**
     * The track's tuning: each MIDI note's ratio to equal temperament, or
     * null for equal temperament. The rack sets it before every block, and
     * machines read it through [noteHz].
     */
    void setTuning(const float *ratios) { tuning_ = ratios; }

    virtual void reset() = 0; // silence, forget held notes
    /**
     * The words the next note-on sings, as phone codes (see
     * machine/diction/Phones.h), sent from a clip just before its note. The
     * array is only good for the call, so a machine keeps a copy. Only a
     * singer listens.
     */
    virtual void lyric(const uint8_t * /*phones*/, int32_t /*count*/) {}
    /**
     * A singer starts a word's consonants before its note, so the vowel lands
     * on it. A machine that does says so here, and is then told of a clip
     * note's words ahead of time with [wordsAhead]: the note, and how many
     * frames until it's due. Its lyric() and note-on still come on time.
     */
    virtual bool wantsWordsAhead() const { return false; }
    virtual void wordsAhead(const uint8_t * /*phones*/, int32_t /*count*/, uint8_t /*note*/, uint8_t /*velocity*/,
                            int32_t /*inFrames*/) {}
    virtual void noteOn(uint8_t note, uint8_t velocity) = 0;
    virtual void noteOff(uint8_t note) = 0;
    virtual void allNotesOff() = 0;
    virtual void controlChange(uint8_t /*cc*/, uint8_t /*value*/) {}
    /**
     * The rack holds notes for the sustain pedal. This also tells a machine
     * that models dampers that they're lifted, so unplayed strings can ring
     * in sympathy. Most machines ignore it.
     */
    virtual void setDampers(bool /*lifted*/) {}
    /**
     * A machine that takes the pedals itself, how far down each is: half a
     * pedal is half lifted dampers on a piano. The rack then leaves its
     * sustain and soft to it (it still holds notes for sostenuto). Every
     * other machine gets them as on and off, and its note-offs held.
     */
    virtual bool takesPedals() const { return false; }
    /** [which] is kPerfSustain, kPerfSostenuto or kPerfSoft (Messages.h); [level01] how far down. */
    virtual void pedal(int32_t /*which*/, float /*level01*/) {}
    virtual void channelPressure(uint8_t /*value*/) {}
    virtual void pitchBend(int16_t /*value14*/) {}

    // --- Per-note expression (MPE) ---------------------------------------
    //
    // Bend, pressure and slide for one note instead of the whole channel. The
    // rack works out which note a member channel is holding and calls these.
    //
    // By default they call the channel-wide versions, so machines without
    // per-voice support still respond, just not per note.
    //
    // [semitones] is signed and already scaled by the zone's bend range, so a
    // machine just adds it to the voice's pitch.
    virtual void noteBend(uint8_t /*note*/, float semitones) {
        // As a channel bend with a range of two semitones, clamped. An MPE
        // finger can slide 48 semitones, which would overflow the 16 bits.
        const float v = semitones / 2.0f * 8192.0f;
        pitchBend(static_cast<int16_t>(v < -8192.0f ? -8192.0f : (v > 8191.0f ? 8191.0f : v)));
    }
    virtual void notePressure(uint8_t /*note*/, uint8_t value) { channelPressure(value); }
    /** Slide, CC 74. There's no channel-wide version to fall back to. */
    virtual void noteTimbre(uint8_t /*note*/, uint8_t /*value*/) {}

    // Render `frames` samples. Return true if R was written (stereo), false if
    // the output is mono in L and the rack should copy it.
    virtual bool render(float *L, float *R, int32_t frames) = 0;

    ParamSet &params() { return params_; }
    const ParamSet &params() const { return params_; }

    // Audio thread. Hands over an object built elsewhere (like a decoded
    // sample) for `slot`. Returns the object it replaces for the caller to
    // free. A machine that doesn't use it returns `object` itself.
    virtual void *swapObject(int32_t /*slot*/, void *object) { return object; }

    /**
     * The rack this machine listens to, as an effect's detector does (see
     * Effect::sidechainRack): a stepped parameter named `sidechain`, 0 for
     * none and 1..16 for a rack. -1 for none, or a machine without one.
     */
    int32_t sidechainRack() const {
        if (sidechainIndex_ < 0) return -1;
        return static_cast<int32_t>(params_.get(sidechainIndex_) + 0.5f) - 1;
    }
    /** That rack's sound this block, mono, or null. Set by the engine before render. */
    void setKey(const float *key) { key_ = key; }

    void handleMidi(uint8_t status, uint8_t d1, uint8_t d2) {
        switch (status & 0xf0) {
        case 0x90:
            if (d2 == 0) noteOff(d1); else noteOn(d1, d2);
            break;
        case 0x80: noteOff(d1); break;
        case 0xb0: controlChange(d1, d2); break;
        case 0xd0: channelPressure(d1); break;
        case 0xa0: notePressure(d1, d2); break;
        case 0xe0: pitchBend(static_cast<int16_t>((d2 << 7) | d1) - 8192); break;
        default: break;
        }
    }

  protected:
    void initParams() {
        int32_t n = 0;
        const ParamDef *defs = paramDefs(n);
        params_.init(defs, n);
        sidechainIndex_ = -1;
        for (int32_t i = 0; i < n; ++i) if (std::strcmp(defs[i].name, "sidechain") == 0) sidechainIndex_ = i;
    }
    ParamSet params_;
    /** The sidechain for this block, or null: see `setKey`. */
    const float *key_ = nullptr;
    int32_t sidechainIndex_ = -1;

    /**
     * The ways to read a parameter.
     *
     * `paramOf` is the smoothed value. Use it for audio so knob moves don't
     * click.
     *
     * `targetOf` and `steppedTargetOf` are the value it's heading to. Use them
     * for anything read once at note-on to set up per-note state (glide time,
     * envelope stage, voice count, spread). Otherwise a note depends on how
     * recently the knob moved and two exports of one song can differ.
     * `tools/noteon_check.py` checks this.
     */
    float paramOfIndex(int32_t p) const { return params_.get(p); }
    float targetOf(int32_t p) const { return params_.target(p); }
    int32_t steppedTargetOf(int32_t p) const {
        // To the nearest step either side of zero: an octave of -1 is -1.
        return static_cast<int32_t>(std::lround(params_.target(p)));
    }

  protected:
    /**
     * A note's pitch in hertz, in the track's tuning.
     *
     * Fractional notes (glides, bends) interpolate the tuning on a log scale,
     * so a slide from a tuned C to a tuned D passes evenly between them.
     */
    float noteHz(float note) const {
        const float equal = 440.0f * std::exp2((note - 69.0f) / 12.0f);
        if (tuning_ == nullptr) return equal;
        const float n = note < 0.0f ? 0.0f : (note > 127.0f ? 127.0f : note);
        const int i = static_cast<int>(n);
        const float frac = n - static_cast<float>(i);
        const float a = tuning_[i];
        const float r = (frac <= 0.0f || i >= 127) ? a : a * std::pow(tuning_[i + 1] / a, frac);
        return equal * r;
    }

    /** The tuning table, for a machine that tunes something other than voices, like the organ's wheels. */
    const float *tuningTable() const { return tuning_; }

  private:
    const float *tuning_ = nullptr;
};

/**
 * What a panic does to a machine, shared by the engine and the test harness.
 *
 * Silence, reset the channel's controllers, reset the machine, then jump
 * parameters to their targets. Controllers are reset because a machine keeps
 * the last wheel, bend and pressure, so a render would otherwise start with
 * whatever was played before.
 */
inline void panicMachine(Machine &m) {
    m.allNotesOff();
    m.pitchBend(0);
    m.controlChange(1, 0);
    m.channelPressure(0);
    m.reset();
    m.params().jumpAll();
}

} // namespace acidulous
