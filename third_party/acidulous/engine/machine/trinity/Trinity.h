#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/LfoGen.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/dsp/Osc.h>
#include <engine/dsp/Wavetable.h>
#include <engine/machine/Machine.h>

// Trinity, the polyphonic machine with three oscillators. Laid out like a
// modern wavetable poly synth: three equal oscillators with analogue waves
// or wavetables, density stacking, virtual sync, two filters with drive,
// six envelopes, three LFOs and a modulation matrix.
//
// On top of that it has FM between the oscillators and per-voice drift so
// held chords move a little.
namespace acidulous::machine {

class Trinity final : public Machine {
  public:
    static constexpr int kOscs = 3;
    static constexpr int kDensity = 8;
    static constexpr int kEnvs = 6;
    static constexpr int kLfos = 3;
    static constexpr int kMatrixSlots = 12;
    static constexpr int kVoices = 16;
    /** How many released voices can ring at once in lean mode (see cutTails). */
    static constexpr int kLeanTails = 6;

    // Waves 0..3 are the analogue ones. 4 and up index the wavetable bank.
    enum WaveKind : int32_t { WSaw, WSquare, WTriangle, WSine, WFirstTable, WaveCount = WFirstTable + dsp::WavetableBank::kTables };

    enum ModSource : int32_t {
        SrcOff, SrcOn, SrcModWheel, SrcAftertouch, SrcVelocity, SrcKeyTrack, SrcRandom,
        SrcEnvAmp, SrcEnvFilter, SrcEnv3, SrcEnv4, SrcEnv5, SrcEnv6,
        SrcLfo1, SrcLfo2, SrcLfo3, SourceCount
    };
    enum ModDest : int32_t {
        DstOff, DstPitch, DstPitch1, DstPitch2, DstPitch3,
        DstPos1, DstPos2, DstPos3, DstLevel1, DstLevel2, DstLevel3,
        DstPw1, DstPw2, DstPw3, DstSync1, DstSync2, DstSync3,
        DstDetune, DstNoise, DstRing12, DstRing23, DstFm21, DstFm32,
        DstF1Freq, DstF2Freq, DstF1Res, DstF2Res, DstBalance, DstDrive,
        DstAmp, DstPan, DstLfo1Rate, DstLfo2Rate, DstLfo3Rate, DestCount
    };

    // Parameter indices. Blocks are regular so the table can be generated.
    enum P : int32_t {
        OscBase = 0,                                   // 3 x OscParams
        OscParams = 12,
        MixBase = OscBase + kOscs * OscParams,         // 6
        MixParams = 6,
        FilterBase = MixBase + MixParams,              // 2 x 7 plus route, balance
        FilterParams = 7,
        RouteIndex = FilterBase + 2 * FilterParams,
        BalanceIndex,
        EnvBase,                                       // 6 x 6
        EnvParams = 6,
        LfoBase = EnvBase + kEnvs * EnvParams,         // 3 x 8
        LfoParams = 8,
        MatrixBase = LfoBase + kLfos * LfoParams,      // 12 x 4
        MatrixParams = 4,
        VoiceBase = MatrixBase + kMatrixSlots * MatrixParams,
        VoiceMode = VoiceBase, UnisonCount, UnisonDetune, UnisonSpread,
        Glide, GlideMode, BendRange, Octave, Transpose, Volume, Pan, VelocityAmount,
        // Added at the end, which is safe because songs address parameters by
        // name and an unknown name gets its default. Changing a stepped
        // parameter's step count isn't safe, since the saved value is
        // normalised against it. That's why slide is a depth knob here and not
        // another modulation source.
        MpeTimbre,
        // What a finger's pressure does when the matrix doesn't use it: opens
        // the filters and pushes the level.
        MpePressure,
        // What the mod wheel does when the matrix doesn't use it: opens the
        // filters.
        WheelFilter,
        Count
    };
    // Offsets inside a block.
    enum OscP { OWave, OPos, OWarp, OCoarse, OFine, OLevel, ODensity, ODetune, OSync, OHard, OPw, ODrift };
    enum MixP { MRing12 = 0, MRing23, MFm21, MFm32, MNoise, MNoiseColour };
    enum FiltP { FType = 0, FFreq, FRes, FDriveType, FDrive, FEnv, FKey };
    enum EnvP { EDelay = 0, EAttack, EDecay, ESustain, ERelease, ERepeat };
    enum LfoP { LWave = 0, LRate, LSync, LDelay, LPhase, LSlew, LKeySync, LOneShot };
    enum MatP { XSrc = 0, XSrc2, XDest, XDepth };

    Trinity();

    const char *typeName() const override { return "Trinity"; }
    const ParamDef *paramDefs(int32_t &count) const override;

    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct OscState {
        float phase[kDensity]{};
        float syncPhase = 0.0f;
        float hardZ = 0.0f;
        float driftTarget = 0.0f, drift = 0.0f;
    };

    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0, velocity = 0;
        uint32_t age = 0;
        float freq = 440.0f, glideFrom = 440.0f, glidePos = 1.0f;
        float random = 0.0f, detuneCents = 0.0f, panOffset = 0.0f;
        /**
         * Whether this note started while the engine was running lean.
         *
         * Read once at note-on and kept for the life of the voice. The unison
         * stack is worked out per block, so reading the setting there would thin
         * a note that's already sounding when the watcher switches. The next note
         * gets the new setting.
         */
        bool bornLean = false;
        /** Released and being faded out early by cutTails. */
        bool cut = false;
        // Per-note expression (MPE). bend is in semitones and adds to the
        // machine's own. pressure and timbre are -1 until this finger sends
        // them, so a voice without its own expression uses the channel's and
        // nothing changes for a normal keyboard.
        float bend = 0.0f, pressure = -1.0f, timbre = -1.0f;
        float prsGlide = 0.0f; // see glidePressure
        OscState osc[kOscs];
        dsp::Adsr env[kEnvs];
        dsp::LfoGen lfo[kLfos];
        dsp::MultiFilter filter[2];
        float mod[DestCount]{};
        static constexpr uint32_t kSeed = 0x2f6e2b1u;
        uint32_t rng = kSeed;
    };

    void startVoice(Voice &v, uint8_t note, uint8_t velocity, bool retrigger);
    Voice *allocate();
    void updateVoiceMod(Voice &v, float blockSeconds);
    /**
     * Which envelopes are actually used, one bit per envelope.
     *
     * Only the amp and filter envelopes are wired by name. The other four are
     * for routing, and ticking them every sample when nothing reads them cost
     * about 6% of the machine.
     *
     * Recomputed once a block from the matrix, so a slot that starts using an
     * envelope mid-note gets one that carries on from where it was left.
     */
    int32_t envMask() const;
    /**
     * In lean mode, no more than kLeanTails released voices ring at once.
     *
     * A patch with a long release costs its tails, not its notes. A long
     * release on a sixteenth arp can hold 11-14 voices with only one held, and
     * halving the unison stack doesn't help a patch that doesn't stack.
     *
     * The oldest tails go first, faded over 10 ms, and held notes are never
     * touched. Checked every block rather than at note-on (unlike the stack)
     * since nobody's playing a tail and going lean is exactly when the room
     * is needed.
     */
    void cutTails();
    float sourceValue(const Voice &v, int src) const;
    float renderVoice(Voice &v, int32_t frames, float *out);
    float paramOf(int32_t index) const { return params_.get(index); }
    int stepOf(int32_t index) const { return static_cast<int>(params_.get(index) + 0.5f); }

    Voice voices[kVoices];
    const dsp::WavetableBank *bank = nullptr;
    float sampleRate = 48000.0f;
    float invSampleRate = 1.0f / 48000.0f;
    uint32_t ageCounter = 1;
    float modWheel = 0.0f, aftertouch = 0.0f, bend = 0.0f, bpm = 120.0f;
    float noiseZ = 0.0f;
    int32_t envUsed = 0x3f;
    static constexpr uint32_t kNoiseSeed = 0x13579bdfu;
    uint32_t noiseRng = kNoiseSeed;
    float voiceBuf[64]{};
};

} // namespace acidulous::machine
