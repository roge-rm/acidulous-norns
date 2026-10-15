#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/Math.h>
#include <engine/machine/Machine.h>
#include <engine/machine/diction/Phones.h>
#include <engine/machine/diction/RecordedVoice.h>
#include <engine/machine/diction/Throat.h>
#include <vector>

// Diction sings.
//
// One singer, legato. A note with words sings them: the consonants before
// its vowel, the vowel held for as long as the note, and the consonants after
// it when the note ends. A note without words sings the vowel control's vowel.
//
// The voice is made as it sings, pulse by pulse through a throat whose
// formants move from sound to sound. The voice control moves the throat on
// its own, apart from the pitch, which makes the one voice a man, a woman or
// neither.
//
// Or it sings in a voice somebody recorded: each vowel and consonant is the
// singer's own, laid down a glottal pulse at a time at the note's pitch, as
// Molt does.
namespace acidulous::machine {

class Diction final : public Machine {
  public:
    /** Held keys remembered for legato, so letting go of one returns to the last still held. */
    static constexpr int kHeld = 16;
    /** The most sounds a note's words can have. */
    static constexpr int kMaxPhones = 32;
    /** Stops and diphthongs are several steps each, and a note carries the last one's ending in. */
    static constexpr int kMaxSteps = 96;
    /** Samples between moves of the formants. */
    static constexpr int kControl = 16;
    /** While the formants move, the levels are worked out again every this many control periods. */
    static constexpr int kLevelsEvery = 4;
    /**
     * A recorded voice's overlap-add buffer, a power of two so the ring wraps
     * by mask: two periods at the lowest pitch, with room for the formant.
     */
    static constexpr int kAccum = 4096;

    enum P : int32_t {
        Vowel = 0, Formant, Breath, Consonants, Accent,
        Vibrato, VibratoRate, VibratoDelay, Drift,
        Glide, Attack, Release, VelocityAmount,
        BendRange, Octave, Transpose,
        Volume, Pan,
        // A recorded voice's consonants: how loud against its vowels, and for
        // each, which of the singer's takes it's formed from (see kFromOrder).
        ConsonantLevel,
        From,
        Clean = From + 24,
        // How the voice comes out: how hard it's sung, how rough, whispered,
        // how it arrives at a note, and whether the throat follows the pitch.
        Effort, Rasp, Growl, Whisper, Scoop, Track,
        // More singers from the one voice, how unlike each other they are,
        // and whether a chord sings on every note.
        Singers, Spread, Harmony,
        // A recorded voice crossed with the built-in one: the built-in
        // folds for the singer's, and the built-in throat for the singer's.
        CrossSource, CrossThroat,
        // Toward a second recorded voice: its throat, sung by the first's source.
        Morph,
        // Another track's sound for the folds, mouthing the words: a talkbox.
        Talk,
        Count
    };
    /** The consonants the From parameters are for, in order. */
    static const char *const kFromOrder[24];

    Diction();

    const char *typeName() const override { return "Diction"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void lyric(const uint8_t *phones, int32_t count) override;
    bool wantsWordsAhead() const override { return true; }
    void wordsAhead(const uint8_t *phones, int32_t count, uint8_t note, uint8_t velocity, int32_t inFrames) override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;
    /** Slot 0 is a recorded voice (diction::RecordedVoice), or null for the built-in one. */
    void *swapObject(int32_t slot, void *object) override;

  private:
    /**
     * One step of a note's words: where the formants go and how fast, what
     * sounds and for how long. A stop is three: closed, the burst, and the
     * breath after it.
     */
    struct Step {
        float f[3];
        /** The phone it came from, for the hiss's colour. 0 for none. */
        uint8_t phone;
        float voice, air, hiss, nasal;
        /** Seconds; negative is held until the note is let go. */
        float length;
        /** Seconds for the formants to get there. */
        float glide;
        /** Seconds for the sources to fade to their new levels. */
        float edge;
        /** The vowel control's vowel, followed live, instead of [f]. */
        bool knob;
        /** A recorded voice's consonant, and the part of it this step plays, in frames of its sound. */
        const diction::RecordedVoice::Join *join = nullptr;
        int32_t from = 0, to = 0;
        /** How much faster than it was sung the consonant itself is read. */
        float speed = 1.0f;
        /** And all of it, squeezed to fit between quick notes. */
        float rate = 1.0f;
        /**
         * A recorded diphthong: its first vowel held, or with [once], its move
         * to the second played once through, between [from] and [to].
         */
        const diction::RecordedVoice::Unit *unit = nullptr;
        bool once = false;
    };

    /** The steps for [count] phones. Returns how many were added to [out]. */
    int32_t planSyllable(const uint8_t *phones, int32_t count, Step *out, int32_t capacity) const;
    /** The same in a recorded voice: its vowels, and each consonant as it was sung. */
    int32_t planRecorded(const uint8_t *phones, int32_t count, Step *out, int32_t capacity) const;
    /** The steps for a note's words, or the vowel control's vowel when it has none. */
    int32_t planWords(const uint8_t *phones, int32_t count, Step *out) const;
    /** Starts note [n] singing [planned]. */
    void startPlanned(uint8_t n, uint8_t vel, bool legato, const Step *planned, int32_t count);
    /** Seconds before its vowel that a word's steps start: its consonants before the vowel. */
    float onsetOf(const Step *planned, int32_t count) const;
    /** Whether [s] is where a note's own time begins: its vowel, or anything held. */
    bool beginsNote(const Step &s) const;
    /** Shortens a timed step to [f] of its length, a recorded one read that much faster. */
    static void squeeze(Step &s, float f);
    /** Whether a step is an R, L, W or Y, a movement that squeezing turns into another sound. */
    bool keepsLength(const Step &s) const;
    /** Seconds the word being sung still has to say before another can start: what's after the held sound. */
    float stillToSay() const;
    /** Once a block: starts a word known ahead when its time comes, and lets go of one whose note never came. */
    void wordsDue(int32_t frames);
    /** Start the steps in [steps], keeping whatever the last note still had to say. */
    void beginSteps(const Step *steps, int32_t count, bool fromSilence);
    void enterStep(int32_t index);
    /** Once per control period: move through the steps and the formants. */
    void control();
    /** The gains a step closer to where the levels were last worked out for. */
    void followLevels(const Step &s, float dt);
    /** Where the formants should be now, before the voice control. */
    void currentTargets(float f[3]) const;
    /** The vowel control's formants: between the two vowels it's between. */
    void knobFormants(float f[3]) const;
    /** The next period of the folds, in samples, with the pitch, vibrato, drift and scoop moved on by one. */
    float nextPeriod();
    float targetNote() const;
    void startNote(uint8_t note, uint8_t velocity, bool legato);

    /**
     * Where a recorded sound is being read: which, between where, and how
     * far into it. A held one goes back and forth between [from] and [to];
     * one played once goes on through, into what was sung after it.
     */
    struct Reader {
        /** The unit or join, to tell one from another. */
        const void *owner = nullptr;
        const audio::Utterance *sound = nullptr;
        /** Its throat and source, for crossing with the built-in voice. */
        const diction::RecordedVoice::Tract *tract = nullptr;
        /**
         * The same sound in the voice being morphed to, and where the two
         * line up: [aFrom] to [aTo] here is [bFrom] to [bTo] there.
         */
        const audio::Utterance *soundB = nullptr;
        const diction::RecordedVoice::Tract *tractB = nullptr;
        float gainB = 1.0f;
        int32_t aFrom = 0, aTo = 0, bFrom = 0, bTo = 0;
        float gain = 1.0f;
        int32_t from = 0, to = 0;
        bool held = true;
        float pos = 0.0f;
        float dir = 1.0f;
        /**
         * A consonant's own part, turned down by [quiet] (see
         * RecordedVoice::Join::consonantGain) and read [speed] times faster
         * than it was sung. The vowel either side is read as it was.
         */
        int32_t quietFrom = 0, quietTo = 0;
        float quiet = 1.0f;
        float speed = 1.0f;
        float rate = 1.0f;
        /** A hiss taken off a little at the top; see RecordedVoice::Join::bright. */
        bool soften = false;
    };
    /** In [control]: the recorded vowel for the step being sung, crossfaded to when it changes. */
    void chooseUnit(const Step &s, const float target[3]);
    /** Crossfades to [r] over [seconds]. */
    void startReading(const Reader &r, float seconds);
    /** The pitch mark [r] is at, or null. */
    static const audio::Epoch *epochOf(const Reader &r);
    /** Lays one grain of [r] at [weight] into the buffer, for a period [period] frames long. */
    void layGrain(const Reader &r, float weight, float period, float ratio);
    /** Where a grain read at [pos] comes from and how it's laid. */
    struct GrainShape {
        int32_t idx = 0, n = 0;
        float half = 0.0f, gain = 0.0f, wStep = 0.0f, span = 0.0f, from = 0.0f;
    };
    bool shapeGrain(const Reader &r, float pos, float weight, float period, float ratio, GrainShape &g) const;
    /** Moves [r] on a frame. */
    static void advance(Reader &r);

    float paramOf(int32_t i) const { return params_.get(i); }
    // Rounded, not truncated: octave and transpose go below zero, where
    // adding a half and truncating made -1 into 0.
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(std::lround(paramOf(p))); }

    float sampleRate = 48000.0f;
    const diction::Phone *phones = nullptr;
    int32_t phoneCount = 0;
    diction::Throat throat;

    // The next note's words, from the clip, until it starts.
    uint8_t pending[kMaxPhones]{};
    int32_t pendingCount = 0;

    // Words known ahead of their notes, oldest first, each started early by
    // the length of its consonants so its vowel lands on its note.
    struct Ahead {
        uint8_t phones[kMaxPhones];
        int32_t count;
        uint8_t note, velocity;
        /** Frames until its note is due. */
        int32_t in;
        /** Seconds of consonants before its vowel. */
        float onset;
    };
    static constexpr int kAhead = 4;
    Ahead ahead[kAhead]{};
    int32_t aheadCount = 0;
    /**
     * Notes begun early, oldest first, waiting for their note-ons. The last
     * is let go if its note-on hasn't come [wait] frames on.
     */
    struct Early {
        uint8_t note;
        int32_t wait;
    };
    Early earlies[kAhead]{};
    int32_t earlyCount = 0;
    /** The newest word's vowel in [steps], or -1 for none. */
    int32_t newestVowel = -1;
    /** Whether the word being sung has sung enough of its vowel for the next to begin. */
    bool readyForNext() const;
    /**
     * The last word's end is sung at its own pitch: the next note's comes in
     * [pitchIn] frames, once it's said, and is then [nextNote].
     */
    int32_t pitchIn = 0;
    uint8_t nextNote = 0;

    // The one singer.
    uint8_t held[kHeld]{};
    int32_t heldCount = 0;
    uint8_t note = 0;
    bool gate = false;
    bool sounding = false;
    float velocity = 1.0f;
    /** The sung pitch in semitones, gliding to the target. */
    float pitch = 0.0f;
    /** A glide goes from here to the note and arrives in the glide time. [glideDone] runs 0 to 1. */
    float glideFrom = 0.0f, glideDone = 1.0f;
    /** A singer arrives at a note from a little below. Semitones, decaying to nothing. */
    float scoop = 0.0f;
    float bend = 0.0f;
    float pressure = 0.0f;
    float modWheel = 0.0f;
    /** Seconds since the note began, for the vibrato's delay. */
    float sinceOnset = 0.0f;
    float vibratoPhase = 0.0f;
    /** A slow wander in the pitch, as no voice holds a note perfectly still. */
    float wander = 0.0f, wanderTarget = 0.0f;
    int32_t wanderCountdown = 0;
    uint32_t seed = 1;
    uint32_t noiseSeed = 1;
    /** Rasp's own randomness, and which of growl's pair of pulses is next. */
    uint32_t pulseSeed = 1;
    bool oddPulse = false;
    /**
     * Rasp and growl for the next pulse: its period moved, and the size it's
     * laid at returned.
     */
    float pulseCharacter(float &period) { return pulseCharacter(period, oddPulse, pulseSeed); }
    float pulseCharacter(float &period, bool &odd, uint32_t &random);
    /** Effort's tilt: the source below about 1 kHz, the rest turned up or down against it. */
    float tiltLow = 0.0f;
    /** Track's move of the throat, for a voice whose own pitch is [rootHz], at the pitch sung. */
    float tracked(float rootHz) const {
        const float t = dsp::clampf(paramOf(Track), -1.0f, 1.0f);
        return t == 0.0f ? 1.0f : dsp::clampf(std::pow(sungHz / rootHz, t), 0.35f, 3.0f);
    }

    // The folds.
    float phase = 1.0f;
    float phaseStep = 0.0f;
    float strength = 1.0f;
    float previousFlow = 0.0f;
    /** The note's pitch without the vibrato, which the throat's shape and levels follow. */
    float sungHz = 150.0f;
    /** The pulse's size at this pitch against the reference the levels are worked out at. */
    float pulseScale = 1.0f;

    // The words being sung.
    Step steps[kMaxSteps]{};
    int32_t stepCount = 0;
    int32_t stepIndex = 0;
    float stepTime = 0.0f;
    /** The note has been let go, so a held step moves on. */
    bool letGo = false;
    /** Nothing left to say: the amp is released once the words run out. */
    bool wordsDone = false;

    // Where the throat is and where it's heading.
    float formants[3] = {500, 1500, 2500};
    float glideStart[3] = {500, 1500, 2500};
    float nasal = 0.0f, nasalStart = 0.0f;
    float level[3] = {0, 0, 0};     // voice, air, hiss now
    float levelStep[3] = {0, 0, 0}; // per sample, towards the step's
    float voiceGain = 1.0f, airGain = 1.0f;
    float voiceGainTarget = 1.0f, airGainTarget = 1.0f;
    /** What the gains were last worked out for, so they're only worked out again when that moves. */
    float gainsFor[5] = {-1, -1, -1, -1, -1};
    int32_t untilControl = 0;
    int32_t sinceLevels = 0;
    /** The next control period jumps the levels to their targets instead of easing. */
    bool snapLevels = false;

    dsp::Adsr amp;

    // A recorded voice, when one is chosen.
    const diction::RecordedVoice *voice = nullptr;
    /** The vowel sung now, and the one it's taking over from while [fade] runs 0 to 1. */
    Reader reading, fading;
    float fade = 1.0f, fadeStep = 0.0f;
    /** Frames until the next grain. */
    float untilGrain = 0.0f;
    std::vector<float> acc = std::vector<float>(kAccum, 0.0f);
    int32_t accHead = 0;
    /**
     * The clean knob's comb: what came out, how much of it is mixed back, and
     * the last grain's period and whether it was of a held vowel.
     */
    std::vector<float> combLine = std::vector<float>(kAccum, 0.0f);
    int32_t combHead = 0;
    float comb = 0.0f;
    float grainPeriod = 1.0f;
    bool grainHeld = false;
    /** What the grains made before clean, for whisper, and its level and whisper's, followed. */
    std::vector<float> rawLine = std::vector<float>(kAccum, 0.0f);
    float loudIn = 0.0f, loudOut = 0.0f;

    // --- more singers from the one voice ------------------------------------------
    // Each is a clock of its own laying grains from the same readers: a
    // choir's copies a little late, off pitch, with their own throat and
    // place, and a chord's other notes at their own pitch.
    static constexpr int kClocks = 12;
    static constexpr int kHarmony = 4;
    static constexpr int kRing = 8192;
    static constexpr int kGrains = 8;
    /** One of another singer's grains, played a sample at a time. */
    struct Grain {
        const audio::Utterance *sound = nullptr;
        float from = 0.0f, ratio = 1.0f, wStep = 0.0f, gain = 0.0f;
        int32_t k = 0, n = 0;
    };
    struct Clock {
        /** Wanted: fading in or sounding. Let go, it fades out and is free once silent. */
        bool on = false;
        /** 0 follows the lead; otherwise the harmony note it sings. */
        uint8_t note = 0;
        /** 0 is the note itself, then its choir copies. */
        int32_t copy = 0;
        float gain = 0.0f;
        float untilGrain = 0.0f;
        /** A copy's own wander off the pitch, in cents. */
        float cents = 0.0f, centsTarget = 0.0f;
        float wanderIn = 0.0f;
        uint32_t random = 1;
        bool odd = false;
        /** How late it is in frames, its throat against the lead's, its place, and how far on it reads. */
        int32_t delay = 0;
        float ratio = 1.0f;
        float panL = 0.70710678f, panR = 0.70710678f;
        float shift = 0.0f;
        // The built-in voice's folds.
        float phase = 1.0f, phaseStep = 0.0f, strength = 1.0f, previousFlow = 0.0f, pulseScale = 1.0f;
        // A recorded voice's grains.
        Grain grains[kGrains]{};
        int32_t grainCount = 0;
    };
    Clock clocks[kClocks]{};
    /** Whether any clock is sounding, so a voice without any runs as it always did. */
    bool anyClocks = false;
    /** A chord's other notes, sung as harmony. */
    uint8_t harmony[kHarmony]{};
    int32_t harmonyCount = 0;
    /** The lead's pitch less its note, in semitones: vibrato, wander and scoop, which the others follow. */
    float wobble = 0.0f;
    std::vector<float> ringL = std::vector<float>(kRing, 0.0f), ringR = std::vector<float>(kRing, 0.0f);
    int32_t ringHead = 0;
    float tiltLowL = 0.0f, tiltLowR = 0.0f;
    /** Brings the singers together to about one's level. */
    float together = 1.0f;

    // --- crossed with the built-in voice, and morphed to another ---------------------
    /** The voice being morphed to, if one is chosen. */
    const diction::RecordedVoice *voiceB = nullptr;
    /** Pairs [r] with the same sound in [voiceB]: the unit [unit] or the consonant [join] of the first voice. */
    void pairWithB(Reader &r, const diction::RecordedVoice::Unit *unit, const diction::RecordedVoice::Join *join) const;
    /** Where [pos] in [r]'s sound is in its pair's. */
    static float mapToB(const Reader &r, float pos);
    /** The throat between the two, as the lattice wants it. */
    float morphK[diction::RecordedVoice::Tract::kOrder]{};
    static constexpr int kOrder = diction::RecordedVoice::Tract::kOrder;
    /** The singer's source, laid in grains beside the voice when the built-in throat is wanted. */
    std::vector<float> accSource = std::vector<float>(kAccum, 0.0f);
    bool sourceGrains = false;
    /** The singer's throat as it's reached, heading for the lead's mark's, and the lattice's memory. */
    float latK[kOrder]{}, latB[kOrder + 1]{};
    float latGain = 0.0f;
    const float *latTarget = nullptr;
    float latGainTarget = 0.0f;
    /** How voiced it is now, 0 to 1, heading for whether the last grain was: crossing is only for what's voiced. */
    float crossVoiced = 0.0f;
    bool crossWanted = false;
    /** The lead's last voiced period, which the built-in folds follow. */
    float voicedPeriod = 0.0f;
    /** Levels followed, to bring each path to the recorded voice's. */
    float levelSung = 0.0f, levelThroat = 0.0f, levelFolds = 0.0f, levelSource = 0.0f;
    float levelThroatOut = 0.0f, levelSungOut = 0.0f, throatMatch = 1.0f;
    float tiltLowCross = 0.0f;
    /** The folds' pulse with its tilts taken off: its memory, the emphasis put back, and its level. */
    float foldsBefore = 0.0f, foldsEarlier = 0.0f, deemphasis = 0.0f, levelEven = 0.0f;
    /**
     * Talking, the other track is taken at a nominal level, a synth at about
     * -14 dB, and so follows it up and down as a talkbox does: brought to the
     * built-in folds' own level ([foldsRms], from their pulse at the
     * reference pitch) for the built-in throat, and to the voice's level
     * through the singer's. Followed levels instead, starting from nothing,
     * made every note's start ten times too loud.
     */
    static constexpr float kTalkNominal = 0.2f;
    float foldsRms = 1.0f;
    bool harmonyOn() const { return steppedOf(Harmony) > 0; }
    void addHarmony(uint8_t n);
    void dropHarmony(uint8_t n);
    /** The clocks wanted for the singers and the harmony, and what each copy is like. In [control]. */
    void assignClocks();
    /** A clock's pitch before any wobble, in semitones. */
    float clockPitch(const Clock &c) const;
    /** A clock's next period, moving its wander on. */
    float clockPeriod(Clock &c);
    /** Begins a grain of [r] for [c], at [weight]. */
    void startGrain(Clock &c, const Reader &r, float weight, float period, float ratio);
    /** [c]'s grains' next sample. */
    float nextOf(Clock &c);
};

} // namespace acidulous::machine
