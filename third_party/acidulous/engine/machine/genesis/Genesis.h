#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

// Genesis is the big analogue style drum machine (Hexbeat is the small one).
// A long pitch-swept kick, a snare made of two tones and noise, six detuned
// squares through a high pass for the metal voices, and a clap made of four
// bursts and a room.
//
//   - Drift moves tune, decay and level a little on every hit, so repeated
//     hits don't sound identical.
//   - A bus compressor has the kick on its side chain for pumping.
namespace acidulous::machine {

class Genesis final : public Machine {
  public:
    enum Voice : int32_t {
        Kick, Snare, Clap, Rim, TomLo, TomMid, TomHi,
        HatClosed, HatOpen, Crash, Ride, Cowbell, VoiceCount
    };
    static constexpr uint8_t kBaseNote = 36;

    enum P : int32_t {
        KickTune, KickDecay, KickPunch, KickSweep, KickClick, KickDrive, KickLevel,
        SnareTune, SnareDecay, SnareSnap, SnareTone, SnareLevel,
        ClapSpread, ClapDecay, ClapTone, ClapLevel,
        RimTune, RimDecay, RimLevel,
        TomLoTune, TomMidTune, TomHiTune, TomDecay, TomBend, TomLevel,
        HatTune, HatClosedDecay, HatOpenDecay, HatTone, HatLevel,
        CrashDecay, CrashTone, CrashLevel,
        RideDecay, RideTone, RideBell, RideLevel,
        BellTune, BellDecay, BellLevel,
        Drift, Accent,
        CompAmount, CompAttack, CompRelease, Duck,
        Drive, Volume, Pan,
        Count
    };
    static_assert(Count <= kMaxParams, "Genesis declares more parameters than a unit can hold");

    Genesis();
    const char *typeName() const override { return "Genesis"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t) override {} // drums do not sustain
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct Env {
        float level = 0.0f, coeff = 0.01f;
        bool active = false;
        /**
         * [seconds] is the time to fall 60 dB, so a decay knob in seconds
         * means what it says.
         */
        void fire(float sr, float seconds, float amp = 1.0f) {
            level = amp;
            constexpr float kLn1000 = 6.907755f; // 60 dB, in time constants
            coeff = 1.0f - std::exp(-kLn1000 / (std::max(0.002f, seconds) * sr));
            active = true;
        }

        /**
         * The same curve with [seconds] as one time constant. Used for the
         * envelopes that shape a sound (the kick's pitch sweep, the tom bend,
         * the click), which were tuned by ear this way. Switching them to
         * [fire] would make them about seven times faster.
         */
        void fireTau(float sr, float seconds, float amp = 1.0f) {
            level = amp;
            coeff = 1.0f - std::exp(-1.0f / (std::max(0.002f, seconds) * sr));
            active = true;
        }
        float next() {
            level -= level * coeff;
            if (level < 1e-5f) { level = 0.0f; active = false; }
            return level;
        }
    };
    struct Osc {
        float phase = 0.0f;
        float step(float hz, float sr) {
            phase += hz / sr;
            if (phase >= 1.0f) phase -= 1.0f;
            return phase;
        }
    };

    float noise() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return static_cast<float>(rng) * (2.0f / 4294967296.0f) - 1.0f;
    }
    /** A random factor around 1 for drift, +/- [amount]. */
    float wobble(float amount) { return 1.0f + noise() * amount; }
    float metallic(float tune);
    void trigger(int32_t voice, float velocity);

    float sr = 48000.0f;
    static constexpr uint32_t kRngSeed = 0x1f123bb5u;
    uint32_t rng = kRngSeed;

    Env amp[VoiceCount], pitch[VoiceCount], aux[VoiceCount];
    float gain[VoiceCount]{};
    float tuneOf[VoiceCount]{};   // this hit's tune, drift included
    Osc osc[VoiceCount], osc2[VoiceCount];
    Osc metal[6];
    dsp::Svf bp[VoiceCount];
    dsp::Svf hp[VoiceCount];
    float clapPhase = 0.0f;
    int32_t clapLeft = 0;

    // Bus compressor and kick ducking envelopes.
    float compEnv = 0.0f, duckEnv = 0.0f;
};

} // namespace acidulous::machine
