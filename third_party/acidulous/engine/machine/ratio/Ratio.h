#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/LfoGen.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/ratio/Algorithms.h>

// Ratio is a six-operator FM synth. Besides FM, each operator can ring
// modulate, filter, fold, sync, phase distort or crush its input.
//
//   - Two algorithms are loaded at once and morphed between. Morph is a
//     modulation destination, so an envelope can sweep the routing.
//   - Operator ratios are snapped (harmonic, subharmonic, odd, semitone or
//     bell partials) and then skewed together, stretching or compressing the
//     whole series.
//
// Every modulation input is read one sample late, so any routing, including
// loops, is allowed and stays stable.
namespace acidulous::machine {

class Ratio final : public Machine {
  public:
    static constexpr int kOps = 6;
    static constexpr int kModEgs = 3;
    static constexpr int kLfos = 3;
    static constexpr int kMatrixSlots = 10;
    static constexpr int kVoices = 12;
    static constexpr int kWaveCount = 16;

    enum OpMode : int32_t { ModeFm, ModeRing, ModeFilter, ModeFilterFm, ModeFold, ModeSync, ModePhase, ModeCrush, ModeCount };
    enum Snap : int32_t { SnapFree, SnapHarmonic, SnapSub, SnapOdd, SnapSemitone, SnapBell, SnapCount };

    enum ModSource : int32_t {
        SrcOff, SrcOn, SrcModWheel, SrcPressure, SrcVelocity, SrcKeyTrack, SrcRandom,
        SrcEg1, SrcEg2, SrcEg3, SrcFilterEg, SrcLfo1, SrcLfo2, SrcLfo3, SourceCount
    };
    enum ModDest : int32_t {
        DstOff, DstPitch, DstMorph, DstSkew,
        DstLevel1, DstLevel2, DstLevel3, DstLevel4, DstLevel5, DstLevel6,
        DstRatio1, DstRatio2, DstRatio3, DstRatio4, DstRatio5, DstRatio6,
        DstFeedback, DstFilterFreq, DstFilterRes, DstAmp, DstPan,
        DstLfo1Rate, DstLfo2Rate, DstLfo3Rate, DestCount
    };

    enum P : int32_t {
        OpBase = 0,
        OpParams = 14,
        AlgoA = OpBase + kOps * OpParams, AlgoB, Morph, SnapMode, Skew,
        FilterType, FilterFreq, FilterRes, FilterEnv, FilterKey,
        FilterAttack, FilterDecay, FilterSustain, FilterRelease,
        EgBase,
        EgParams = 4,
        LfoBase = EgBase + kModEgs * EgParams,
        LfoParams = 6,
        MatrixBase = LfoBase + kLfos * LfoParams,
        MatrixParams = 4,
        VoiceBase = MatrixBase + kMatrixSlots * MatrixParams,
        VoiceMode = VoiceBase, Glide, GlideMode, BendRange, Octave, Transpose, Volume, Pan, VelocityAmount,
        // Added later. Parameters are looked up by name, so this is safe.
        MpeTimbre,
        // What per-note pressure does outside the matrix: opens the filter
        // and raises the level.
        MpePressure,
        Count
    };
    enum OpP { OWave = 0, OMode, ORatio, OFine, OFixed, OLevel, OFeedback, OAttack, ODecay, OSustain, ORelease, OVel, OKey, OPan };
    enum EgP { EAttack = 0, EDecay, ESustain, ERelease };
    enum LfoP { LWave = 0, LRate, LSync, LDelay, LPhase, LKeySync };
    enum MatP { XSrc = 0, XSrc2, XDest, XDepth };

    Ratio();

    const char *typeName() const override { return "Ratio"; }
    const ParamDef *paramDefs(int32_t &count) const override;

    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct OpState {
        float phase = 0.0f;
        float out = 0.0f;      // last sample, read by everything downstream
        float filterZ = 0.0f;  // the filter and filter-FM modes
        float syncArmed = 0.0f;
        // State for the two noise waveforms.
        float noiseHeld = 0.0f;
        float noisePhase = 0.0f;
        uint32_t noiseRng = 0x1234567u;
    };

    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0, velocity = 0;
        uint32_t age = 0;
        float freq = 440.0f, glideFrom = 440.0f, glidePos = 1.0f;
        float random = 0.0f;
        OpState op[kOps];
        dsp::Adsr env[kOps];       // one per operator
        dsp::Adsr modEg[kModEgs];
        dsp::Adsr filterEg;
        dsp::LfoGen lfo[kLfos];
        dsp::MultiFilter filter;
        float mod[DestCount]{};
        static constexpr uint32_t kSeed = 0x7f4a7c15u;
        uint32_t rng = kSeed;
        // Per-note expression (MPE). `bend` is in semitones, added to the
        // channel bend. `pressure` and `timbre` are -1 until the note sends
        // them, so a normal keyboard falls back to the channel values.
        float bend = 0.0f, pressure = -1.0f, timbre = -1.0f;
        float prsGlide = 0.0f; // see glidePressure
    };

    /** The blended routing, rebuilt once per block rather than per voice. */
    struct Routing {
        float amount[kOps][kOps]{}; // [source][destination]
        float carrier[kOps]{};
        int32_t order[kOps * kOps][2]{};
        int32_t edgeCount = 0;
        float carrierSum = 1.0f;
    };

    static float waveAt(int wave, float phase, OpState &st);
    void fillRouting(Routing &routing, float morph) const;
    void buildRouting();
    void startVoice(Voice &v, uint8_t note, uint8_t velocity, bool retrigger);
    Voice *allocate();
    void updateVoiceMod(Voice &v, float blockSeconds);
    float sourceValue(const Voice &v, int src) const;
    void renderVoice(Voice &v, int32_t frames, float *out);
    float paramOf(int32_t index) const { return params_.get(index); }
    int stepOf(int32_t index) const { return static_cast<int>(params_.get(index) + 0.5f); }
    float snapRatio(float ratio, int mode, float skew) const;

    Voice voices[kVoices];
    Routing routing;
    float sampleRate = 48000.0f;
    float invSampleRate = 1.0f / 48000.0f;
    /** Bit mask of the mod envelopes the matrix uses. See Trinity. */
    int32_t egUsed = 0x7;
    uint32_t ageCounter = 1;
    float modWheel = 0.0f, pressure = 0.0f, bend = 0.0f, bpm = 120.0f;
    float voiceBuf[64]{};
};

} // namespace acidulous::machine
