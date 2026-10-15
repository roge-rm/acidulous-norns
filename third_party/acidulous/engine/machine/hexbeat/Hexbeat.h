#pragma once
#include <algorithm>
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

// Hexbeat is a small-box style drum synthesizer with nothing sampled. Kick
// and toms are resonant bursts with a pitch sweep, the snare is two tones
// over filtered noise, hats and cymbals are inharmonic squares through a band
// pass, the cowbell is two squares and the clap is noise pulses. Thirteen
// voices on C2..C3, accent from velocity >= 100.
namespace acidulous::machine {

class Hexbeat final : public Machine {
  public:
    enum P : int32_t {
        KickTune, KickDecay, KickPunch, KickLevel,
        SnareTune, SnareDecay, SnareSnappy, SnareTone, SnareLevel,
        TomLoTune, TomMidTune, TomHiTune, TomDecay, TomLevel,
        HatTune, HatClosedDecay, HatOpenDecay, HatTone, HatLevel,
        CymDecay, CymTone, CymLevel,
        RideDecay, RideLevel,
        ClapDecay, ClapTone, ClapLevel,
        RimTune, RimLevel,
        BellTune, BellDecay, BellLevel,
        ClaveTune, ClaveLevel,
        Accent,
        // New parameters go on the end. A parameter's position here is its
        // index in saved songs.
        Volume,
        // How much velocity sets the level, using the shared velocity curve.
        Velocity,
        Count
    };
    enum Voice : int32_t { Kick, Rim, Snare, Clap, TomLo, TomMid, TomHi, HatClosed, HatOpen, Cymbal, Ride, Cowbell, Clave, VoiceCount };
    static constexpr uint8_t kBaseNote = 36;

    Hexbeat();
    const char *typeName() const override { return "Hexbeat"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t) override {} // drums do not sustain
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct Env { // exponential decay with a fast attack
        float level = 0.0f, coeff = 0.01f;
        bool active = false;
        /** [seconds] is the time to fall 60 dB. */
        void fire(float sr, float seconds, float amp = 1.0f) {
            level = amp;
            constexpr float kLn1000 = 6.907755f; // 60 dB, in time constants
            coeff = 1.0f - std::exp(-kLn1000 / (std::max(0.002f, seconds) * sr));
            active = true;
        }
        /**
         * The same curve with [seconds] as one time constant. Used for the
         * envelopes tuned by ear as shapes: pitch sweeps, the click, clap
         * pulses, the hat choke, and the fixed rim and clave lengths.
         */
        void fireTau(float sr, float seconds, float amp = 1.0f) { level = amp; coeff = 1.0f - std::exp(-1.0f / (std::max(0.002f, seconds) * sr)); active = true; }
        float next() { level -= level * coeff; if (level < 1e-4f) { level = 0.0f; active = false; } return level; }
    };
    struct Osc { float phase = 0.0f; float step(float hz, float sr) { phase += hz / sr; if (phase >= 1.0f) phase -= 1.0f; return phase; } };

    float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng) * (2.0f / 4294967296.0f) - 1.0f; }
    float metallic(float tune);      // the hat/cymbal source: six squares, inharmonic
    float square(Osc &o, float hz) { return o.step(hz, sr) < 0.5f ? 1.0f : -1.0f; }
    float sine(Osc &o, float hz) { return std::sin(o.step(hz, sr) * 6.2831853f); }

    void trigger(int32_t voice, float accent, float level);
    void renderVoice(int32_t voice, float *out, int32_t frames);

    float sr = 48000.0f;
    static constexpr uint32_t kRngSeed = 0x9e3779b9u;
    uint32_t rng = kRngSeed;

    // per-voice state
    Env amp[VoiceCount];
    Env pitchEnv[VoiceCount];
    Env aux[VoiceCount];      // snare noise / clap tail / rim click
    float gain[VoiceCount]{}; // accent-scaled trigger level
    Osc osc[VoiceCount];
    Osc osc2[VoiceCount];
    Osc metal[6];
    dsp::Svf bp[VoiceCount];
    dsp::Svf bp2[VoiceCount];
    int32_t clapPulse = 0;
    float clapTimer = 0.0f;
    float mix[64];
};

} // namespace acidulous::machine
