#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <engine/dsp/Fft.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * The sound of a small recordable disc's data-reduced formats. The track is
 * cut into short overlapping frames and split into bands, and each frame
 * gets a fixed number of bits to spend: the bands the ear would miss least
 * get fewest, or none, and the top end goes first. So the highs are cut,
 * quiet bands drop out and come back from frame to frame, attacks smear,
 * and at the lowest rates left and right share their bits and the upper
 * mids narrow towards mono.
 *
 * `mode` is the format: SP, LP2, LP4, HQ or XLP. `dubs` copies it again,
 * up to four generations, each losing a little more. It runs a frame or
 * more behind the track (more with each dub); `mix` keeps the dry signal
 * in step with it.
 */
class Magneto final : public Effect {
  public:
    enum P { Mode, Dubs, Mix, Gain, Count };
    static constexpr int kModes = 5, kMaxDubs = 4;

    /** How each format spends its bits; see Magneto.cpp. */
    struct Format {
        float frameAt44k;     // samples a frame holds at 44.1 kHz (a power of two)
        float kbps;           // the bitrate, both channels together
        float efficiency;     // how far its coding stretches a bit, against plain word lengths
        float topHz;          // nothing above this is kept
        float tilt;           // dB an octave above 1 kHz the highs are worth less
        float maskDb;         // how far under a band's neighbours it can hide
        float floorDb;        // the quietest level kept, dB below full scale at 1-4 kHz
        bool joint;           // left and right as middle and side, sharing the bits
        int sideCurve;        // when joint: which of kSideCurves the side follows, -1 for none
        float hold;           // 0 to 1: how much a frame's loudness is flattened before coding, so noise keeps out of the quiet before an attack
    };
    static const Format kFormats[kModes];
    /** How much of the side a joint format keeps, as (Hz, dB) points joined in log frequency; Hz 0 ends a curve. */
    static constexpr int kSidePoints = 8;
    static const float kSideCurves[2][kSidePoints][2];

    Magneto() { initParams(); }
    const char *typeName() const override { return "Magneto"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

    /** Samples the wet signal runs behind the dry, for the current settings. */
    int32_t latency() const { return static_cast<int32_t>(dubs) * frame; }

  private:
    /** One generation: a frame of each channel in, a frame of each out. */
    struct Copy {
        std::vector<float> in[2], out[2];
        int32_t fill = 0;
    };
    void configure(int mode, int dubCount);
    void codeFrame(Copy &c, bool stereo);
    void allocate(int channels, const float *energy, int32_t *bits, float budget) const;

    float sr = 44100.0f;
    int mode = -1, dubs = 1;
    int32_t frame = 512, hop = 256;
    const Format *format = nullptr;
    std::vector<std::unique_ptr<dsp::Fft>> ffts; // one for each frame size in use
    const dsp::Fft *fft = nullptr;
    std::vector<float> window;
    /** A frame's loudness, sixteen steps across it, taken out before coding and put back after. */
    static constexpr int kSteps = 16;
    std::vector<float> level;
    std::vector<float> re[2], im[2];
    /** The bands: first bin of each, and the last one past the end. */
    std::vector<int32_t> edge;
    std::vector<float> threshold; // the quietest a band can be and still be heard, as energy
    std::vector<float> worth;     // the tilt, as an energy factor for each band
    std::vector<float> narrow;    // when joint, how much of the side each band keeps
    int32_t bands = 0;
    Copy copies[kMaxDubs];
    /** The dry signal, held back to line up with the wet. */
    std::vector<float> dry[2];
    int32_t dryAt = 0, drySize = 1;
    /** A short fade in after the chain restarts, so a mode change doesn't click. */
    float fadeIn = 1.0f, fadeStep = 0.01f;
};

} // namespace acidulous::effect
