#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/filament/Waveguide.h>

namespace acidulous::machine {

/**
 * Sympath: plucked strings over a buzzing bridge, with strings ringing in
 * sympathy.
 *
 * Each note is a string (Filament's waveguide) lying over a wide, curved
 * bridge. Once it swings far enough towards the bridge it strikes it, harder
 * the further it swings, and every strike rings the bridge: a bright buzz on
 * every cycle, which keeps going as long as the string swings wide enough to
 * reach and fades as the note dies down.
 *
 * Under the played strings lie sympathetic strings, never touched, tuned to
 * the scale of the piece from its tonic: they pick up whatever's played that
 * lands on their notes and ring on after it. Drone strings struck with each
 * note, a tanpura that plucks its four strings by itself in time, slides from
 * note to note, and a finger pulling the string sideways to bend it up.
 */
class Sympath final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Sa, Scale, Bridge, Curve,
        Pluck, Position, Sustain, Bright,
        Tarbs, Chikari, First, Cycle,
        Meend, Gamak,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t { Sitar = 0, Tanpura, Veena, Shamisen,
                         Sarod, RudraVeena, Swarmandal, Biwa, Sanxian, Ektara, ElectricSitar,
                         Sarangi, Esraj, Dilruba, Hardanger, Nyckelharpa, KindCount };
    static constexpr int kVoices = 4;
    /** Strings in one voice: one, or a tanpura's four. */
    static constexpr int kStrings = 4;
    static constexpr int kTarbs = 11;
    static constexpr int kDrones = 3;
    static constexpr int kScales = 10;

    Sympath();

    const char *typeName() const override { return "Sympath"; }
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

    /** Notes sounding, for tests. */
    int activeVoices() const;

  private:
    /** A string and what's still going into it. */
    struct String {
        Waveguide wave;
        std::vector<float> stroke;
        int32_t strokeLength = 0, strokeAt = 0;
        /** Semitones from the voice's note: a tanpura's strings sit below and an octave down. */
        float offset = 0.0f;
        bool sounding = false;
        /** The bow's steady push, taken out so only its waves reach the string. */
        float bowDc = 0.0f;
        /** Samples since the bow touched the string, for its gentle start. */
        int32_t bowAt = 0;
    };
    struct Voice {
        String strings[kStrings];
        int stringCount = 1;
        bool used = false, held = false;
        uint8_t note = 0;
        float velocity = 0.8f;
        float pitch = 60.0f, aim = 60.0f;
        float gain = 0.0f;
        float noteBend = 0.0f, pressure = -1.0f, pull = 0.0f;
        /** A tanpura's place in its cycle, samples, and which string is next. */
        double cycleAt = 0.0;
        int nextString = 0;
        /** The bridge ringing where the strings strike it, and how hard they struck it last. */
        dsp::Svf zing;
        float contactLast = 0.0f;
        /** Samples since a string was last plucked. */
        int32_t sincePluck = 0;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };
    struct Free {
        Waveguide wave;
        std::vector<float> stroke;
        int32_t strokeLength = 0, strokeAt = 0;
        float hz = 0.0f, pan = 0.0f;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    void start(uint8_t note, uint8_t velocity);
    /** Shapes a pluck for a string at [hz] into [stroke]. */
    int32_t shapePluck(std::vector<float> &stroke, float hz, float velocity, float amount);
    void pluck(Voice &v, int s);
    void tuneString(Waveguide &w, float hz, float t60, float tone);
    float stringT60(float hz, bool held) const;
    float stringTone(float velocity) const;
    /** Tunes the sympathetic and drone strings to the tonic and scale. */
    void tuneFree();
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    Free tarbs[kTarbs];
    Free drones[kDrones];
    int tarbCount = 0, droneCount = 0;
    float builtSa = -1.0f, builtScale = -1.0f, builtKind = -1.0f, builtTune = -1000.0f;
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    float gamakPhase = 0.0f;
    int retuneCountdown = 0;
    /** The gourd. */
    dsp::Svf body;
    float bodyBuiltFor = -1.0f;
    /** The neck under a shamisen's low string. */
    dsp::Svf tarbZing;
    float tarbContactLast = 0.0f;
    /** A shamisen's skin, struck with the plectrum. */
    float skin = 0.0f, skinLast = 0.0f;
    dsp::Svf skinTone;
    float dcIn[2] = {0.0f, 0.0f}, dcOut[2] = {0.0f, 0.0f};
    uint32_t noise = 0x2c1b3c6du;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
