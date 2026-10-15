#pragma once
#include <cstdint>
#include <engine/core/InputBus.h>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/Filter.h>
#include <engine/dsp/LfoGen.h>
#include <engine/machine/Machine.h>

// Cipher, a vocoder.
//
// It measures how loud each band of one sound is through a filter bank and
// applies that shape to another sound. The analysis and synthesis banks are
// connected through a map:
//
//   - Remap: band order can be reversed, mirrored, folded, split odd/even or
//     shuffled from a seed. Speech through a reversed bank keeps its rhythm
//     but can't be understood.
//   - Shift and stretch: the synthesis bank can be offset or have its
//     spacing warped, moving formants without changing pitch.
//   - Freeze: holds the measured shape so a vowel sustains, and morphs
//     between it and the live one.
//   - Smear: each band's release is scaled across the bank, so the top fades
//     before the bottom.
//   - Swap: the input can be the carrier instead of the modulator, with the
//     internal oscillators as the modulator.
//   - Track: the modulator's pitch can drive the carrier, so your voice
//     plays the synth.
//   - Feedback: the output can be fed back into the analysis.
namespace acidulous::machine {

class Cipher final : public Machine {
  public:
    static constexpr int kMaxBands = 40;
    static constexpr int kVoices = 8;
    static constexpr int kMatrixSlots = 8;
    static constexpr int kMatrixParams = 3;

    enum Remap : int32_t { MapDirect, MapReverse, MapMirror, MapOddEven, MapShuffle, MapFold, RemapCount };
    enum Role : int32_t { InputIsModulator, InputIsCarrier, RoleCount };
    enum CarrierWave : int32_t { WaveSaw, WavePulse, WaveSuper, WaveNoise, WaveRing, CarrierWaveCount };

    enum ModSource : int32_t {
        SrcOff, SrcOn, SrcModWheel, SrcPressure, SrcVelocity, SrcKeyTrack,
        SrcEg1, SrcEg2, SrcLfo1, SrcLfo2, SrcLoudness, SrcBrightness, SrcPitchTrack, SourceCount
    };
    enum ModDest : int32_t {
        DstOff, DstShift, DstStretch, DstRemapAmount, DstFreezeMorph, DstSmear, DstQ,
        DstCarrierPitch, DstCarrierMix, DstNoise, DstFeedback, DstDrive, DstVolume, DstPan,
        DstBandLow, DstBandHigh, DstGate, DestCount
    };

    enum P : int32_t {
        Bands = 0, LowHz, HighHz, BandQ, Slope,
        RoleMode, Attack, Release, Smear, Gate, GateDepth,
        Shift, Stretch, RemapMode, RemapAmount, Seed,
        Freeze, FreezeMorph, FreezeDecay,
        Sibilance, SibilanceHz, SibilanceLevel,
        PitchTrack, TrackGlide, TrackAmount,
        Feedback, FeedbackTone,
        CarrierWaveA, CarrierWaveB, CarrierMix, Detune, PulseWidth, SubLevel, NoiseLevel, CarrierDrive,
        Unvoiced, Dry, Wet, Drive, Volume, Pan,
        AmpAttack, AmpDecay, AmpSustain, AmpRelease,
        Eg1A, Eg1D, Eg1S, Eg1R, Eg2A, Eg2D, Eg2S, Eg2R,
        Lfo1Wave, Lfo1Rate, Lfo1Sync, Lfo1Depth,
        Lfo2Wave, Lfo2Rate, Lfo2Sync, Lfo2Depth,
        MatrixBase,
        VoiceBase = MatrixBase + kMatrixSlots * kMatrixParams,
        VoiceMode = VoiceBase, Glide, BendRange, Octave, Transpose, Fine, VelocityAmount,
        Count
    };
    enum MatP { XSrc = 0, XDest, XDepth };

    Cipher();

    const char *typeName() const override { return "Cipher"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0;
        float velocity = 1.0f, key01 = 0.5f;
        float velGain = 1.0f; // see velocityGain, fixed at the note-on
        float phaseA = 0.0f, phaseB = 0.0f, phaseSub = 0.0f;
        float freq = 220.0f, target = 220.0f;
        dsp::Adsr amp;
        // Per-note expression (MPE). `bend` is in semitones and adds to the
        // channel bend.
        float bend = 0.0f;
    };
    struct Band {
        dsp::Svf analysis1, analysis2;
        dsp::Svf synthesis1, synthesis2;
        float envelope = 0.0f;
        float held = 0.0f;
        float attackCoeff = 0.01f, releaseCoeff = 0.001f;
        float centre = 100.0f;
    };

    float paramOf(int32_t p) const { return params_.get(p); }
    int32_t steppedOf(int32_t p) const;
    void rebuildBands();
    float bandResonance(float q) const;
    int32_t mappedBand(int32_t band) const;
    float sourceValue(int32_t src) const;
    void applyMatrix();
    /**
     * One sample of one voice's carrier.
     *
     * [glideK] and [detuneMul] are worked out once a block by the caller, so
     * the divide and the pow aren't done per voice per sample.
     */
    float carrierSample(Voice &v, float glideK, float detuneMul, float pitchScale, int32_t waveA,
                        int32_t waveB, float mix, float pw, float sub);

    float sampleRate = 48000.0f;
    float invSampleRate = 1.0f / 48000.0f;
    Band bands[kMaxBands];
    int32_t bandCount = 16;
    /** What the band coefficients were last solved for, see `render`. NaN never matches. */
    float lastBandKey[9] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
    /** Blocks in a row with no voice, no input and nothing over -120 dB, see `render`. */
    int32_t quietBlocks = 0;
    bool asleep = false;
    float lastLow = -1.0f, lastHigh = -1.0f, lastQ = -1.0f;
    int32_t lastCount = -1;

    Voice voices[kVoices];
    dsp::Adsr eg[2];
    dsp::LfoGen lfo[2];
    float lfoValue[2] = {0.0f, 0.0f};
    float mod[DestCount] = {};

    // What the modulator is doing, for the matrix: how loud it is, where its
    // energy sits, and the nearest note.
    float loudness = 0.0f, brightness = 0.0f, trackedHz = 0.0f, trackedNote = 0.0f;
    float zeroPrev = 0.0f;
    int32_t zeroCount = 0, zeroWindow = 0;
    float sibilanceEnv = 0.0f;
    dsp::Svf sibilanceFilter;
    // The sibilance noise gets the same high-pass the detector uses. Plain
    // white noise doesn't sound like an "s" and buries the vocoder.
    dsp::Svf sibilanceShaper;
    float feedbackSample = 0.0f;
    float feedbackLp = 0.0f;

    float bendSemis = 0.0f, modWheel = 0.0f, pressure = 0.0f;
    float bpm = 120.0f;
    static constexpr uint32_t kRngSeed = 0x2f6e1cu;
    uint32_t rngState = kRngSeed;
    int32_t shuffleMap[kMaxBands] = {};
    int32_t shuffleSeed = -1;
};

} // namespace acidulous::machine
