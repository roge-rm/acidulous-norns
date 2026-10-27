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
 * or up, fast trills, a warbled song in phrases, a two-note call answered by
 * another bird, a dawn chorus of different birds mostly taking turns,
 * slow falling coos, or harsh caws in twos and threes.
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
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit. In a kit the routes move the knobs every
        // pad shares.
        MacroBase = PadFirst + 16 * 4,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. Continuous params only: the pattern, tune,
    // rate, flock, bend range, octave and the pads are left out.
    enum MacroDest : int32_t {
        McOff, McLength, McSweep, McRasp, McTwo, McInterval, McBreath, McThroat,
        McBeak, McSpread, McSpace, McVelocity, McVolume,
        McPitch,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static constexpr int kPads = 16, kPadParams = 4;
    enum PadParam : int32_t { PadSong = 0, PadNote, PadFlock, PadLevel };
    static constexpr int32_t padParam(int pad, int which) { return PadFirst + pad * kPadParams + which; }
    static constexpr uint8_t kBaseNote = 36;
    static_assert(Count <= kMaxParams, "too many parameters");

    // Coo and Caw come after the first six so saved patterns keep their place.
    enum Song : int32_t { Whistle = 0, Chirp, Trill, Warble, Call, Chorus, Coo, Caw, SongCount };
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
        /** Coo and caw: samples of the slot sung, the rise in the middle of a
         *  caw (semitones), whether the pitch eases out (a coo's scoop up), and
         *  the place in the phrase and its number of notes. */
        float sung = 1.0f, arch = 0.0f;
        bool easeOut = false;
        int phraseAt = 0, phraseLen = 0;
        /** A caw's half-speed phase, for the croak on every other cycle. */
        float sub = 0.0f;
        /** A rate a little off the others', for a chorus. */
        float drift = 1.0f;
        /** How far off it sits, 0 near to 1 far: quieter and duller. Flock birds only. */
        float far = 0.0f;
        /** A warble: the motif's shape (a WarbleShape), its centre and depth
         *  (semitones), its length in syllable units, how many more times it's
         *  sung, the pitch curve (0 linear, 1 easing out, 2 easing in, 3 both),
         *  a buzz's rate (Hz, 0 for none) and the syllable's loudness. */
        int shape = 0, motifLeft = 0, curve = 0;
        float centre = 0.0f, depth = 0.0f, motifLen = 1.0f, buzz = 0.0f, loud = 1.0f;
        /** Singing a phrase (rather than resting between them), and singing
         *  them in a flock, so it waits for the others' phrases to end. */
        bool inPhrase = false, shy = false;
        /** The throat and beak, and the trachea's echo. */
        dsp::Svf throat;
        float beak = 0.0f;
        float trachea[32] = {};
        int tracheaAt = 0;
        float builtHz = -1.0f;
        /** The folds' steps a sample and the beak's coefficient, set every
         *  kRetuneEvery samples, or on the next sample once [retune] is set. */
        float step0 = 0.0f, step1 = 0.0f, beakCoef = 0.5f;
        bool retune = true;
        /** The bird's own breath noise. */
        uint32_t noise = 0x1f123bb5u;
    };
    struct Voice {
        Bird birds[kBirds];
        int birdCount = 1;
        bool used = false, held = false;
        /** Singing the chorus pattern. */
        bool chorus = false;
        uint8_t note = 0;
        /** The pitch it sings around (the note, or its pad's), and its pad's level. */
        float played = 60.0f, padLevel = 1.0f;
        float velocity = 0.8f;
        float noteBend = 0.0f, pressure = -1.0f;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on and syllable seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    /** Fills [macGoal_] from the macro knobs' targets and their routes. */
    void updateMacroGoals();
    /** Fills [mac_] from the smoothed macro knobs, and the goals too. */
    void updateMacros();
    Voice *voiceFor(uint8_t note);
    /** Starts a bird's next syllable. */
    void nextSyllable(Voice &v, Bird &b, int index);
    /** A warble's next syllable: bird-shaped motifs, each sung a few times, in phrases with pauses. */
    void nextWarble(Voice &v, Bird &b, int index, float unit, float sweep, bool first);
    /** The pause after a flock bird's phrase, samples; longer if another is still singing. */
    int32_t flockPause(const Voice &v, Bird &b, int index, float beat, float least, float most);
    static float rand01(uint32_t &s) { return 0.5f * (rand(s) + 1.0f); }
    static float rand(uint32_t &s) {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    float spaceLp[2] = {0.0f, 0.0f};
    uint32_t clock = 0;
    /** Samples rendered, for retuning on the same samples at any block size. */
    uint32_t retuneClock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};
};

} // namespace acidulous::machine
