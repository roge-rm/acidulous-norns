#pragma once
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

namespace acidulous::machine {

/**
 * Aviary: birdsong.
 *
 * A bird sings from its syrinx: two sound sources, one in each branch of the
 * airway, each a pair of folds that start to swing once the air pressure
 * passes a threshold and settle at a size set by how far past it the
 * pressure is. The two can sing at once, two pitches from one bird. Above
 * them a short trachea, the throat (a cavity the bird tunes to the note it's
 * singing) and the beak, opened wider for higher notes.
 *
 * A held note sets a bird singing a song pattern around that pitch, two
 * octaves up, in time with the tempo: a plain whistle, chirps sweeping down
 * or up, fast trills, a wandering warble, a two-note call answered by
 * another bird, or a dawn chorus of several birds each doing its own thing.
 */
class Aviary final : public Machine {
  public:
    enum P : int32_t {
        Pattern = 0, Tune, Rate, Length, Sweep, Rasp, Two, Interval,
        Breath, Throat, Beak, Flock, Spread, Space,
        VelocityAmount, BendRange, Octave, Volume,
        // Kit mode: each key from kBaseNote is a pad with its own song, pitch
        // (a MIDI note), number of birds and level, kPadParams values each from PadFirst.
        Kit, PadFirst,
        Count = PadFirst + 16 * 4
    };
    static constexpr int kPads = 16, kPadParams = 4;
    enum PadParam : int32_t { PadSong = 0, PadNote, PadFlock, PadLevel };
    static constexpr int32_t padParam(int pad, int which) { return PadFirst + pad * kPadParams + which; }
    static constexpr uint8_t kBaseNote = 36;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Song : int32_t { Whistle = 0, Chirp, Trill, Warble, Call, Chorus, SongCount };
    static constexpr int kVoices = 4;
    static constexpr int kBirds = 4;

    Aviary();

    const char *typeName() const override { return "Aviary"; }
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

    /** Birds singing, for tests. */
    int activeVoices() const;

  private:
    /** One side of the syrinx: a pair of folds swinging, or still. */
    struct Side {
        float phase = 0.0f, size = 0.0f;
    };
    struct Bird {
        Side sides[2];
        /** Which song this bird sings (a chorus gives each its own), and its own seed. */
        int song = Whistle;
        uint32_t seed = 1;
        /** Where it sits, and how far its notes sit from the voice's. */
        float pan = 0.0f, offset = 0.0f;
        /** Its place in the song: samples into the syllable, the syllable's length, and which it is. */
        int32_t at = 0, slot = 1;
        int32_t syllable = 0;
        /** The syllable's pitch now and where it's going, semitones from the voice's note. */
        float from = 0.0f, to = 0.0f, pitch = 0.0f;
        bool singing = false;
        /** A rate a little off the others', for a chorus. */
        float drift = 1.0f;
        /** The throat and beak, and the trachea's echo. */
        dsp::Svf throat;
        float beak = 0.0f;
        float trachea[32] = {};
        int tracheaAt = 0;
        float builtHz = -1.0f;
    };
    struct Voice {
        Bird birds[kBirds];
        int birdCount = 1;
        bool used = false, held = false;
        uint8_t note = 0;
        /** The pitch it sings around (the note, or its pad's), and its pad's level. */
        float played = 60.0f, padLevel = 1.0f;
        float velocity = 0.8f;
        float noteBend = 0.0f, pressure = -1.0f;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    /** Starts a bird's next syllable. */
    void nextSyllable(Voice &v, Bird &b, int index);
    static float rand(uint32_t &s) {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    float spaceLp[2] = {0.0f, 0.0f};
    uint32_t noise = 0x1f123bb5u;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
