#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/Fft.h>
#include <engine/dsp/PitchFollow.h>
#include <engine/machine/abacus/PartialBank.h>

// What Abacus borrows its levels from: another track's sound, analysed as it
// plays, or a file, analysed once into a table of frames.
//
// Both go through the same analysis, one frame per hop of 512 at 48 kHz over
// a 2048-point window: the source high-passed at 70 Hz (rumble is not
// colour), a Hann window, an FFT, and from it
//   - the envelope: the log spectrum smoothed through its cepstrum (only
//     quefrencies under 3 ms are kept, so harmonics under ~330 Hz apart blur
//     into one curve), read at 128 points spaced evenly in octaves. The
//     lifter is applied as what it is in the spectrum, a convolution with its
//     own transform, and only at the bins the points read: far cheaper than
//     the two transforms there and back;
//   - the harmonics: the level at each multiple of the source's own pitch
//     (dsp::PitchFollow), kept from the last frame it was sure of;
//   - the level: the frame's rms against a reference.
// The shapes are log2 with their loudest point at 0, so the level alone says
// how loud the source is.
namespace acidulous::machine::abacus {

constexpr int kFftSize = 2048;
constexpr int kHop = 512;
constexpr int kBins = kFftSize / 2 + 1;
constexpr int kEnvPoints = 128;
constexpr float kEnvLowHz = 30.0f, kEnvTopHz = 24000.0f;
/** Where shapes stop, log2 (-180 dB). */
constexpr float kShapeFloor = -30.0f;
/** A track at this rms borrows at a level of 1: about the house level (-27.5 LUFS). */
constexpr float kTrackRef = 0.042f;
/** The longest stretch of a file that is analysed, seconds. */
constexpr float kFileSeconds = 60.0f;

struct Frame {
    /** The envelope at kEnvPoints points from kEnvLowHz up, log2, its loudest 0. */
    float env[kEnvPoints];
    /** Harmonic n + 1's level, log2, the loudest 0. */
    float harm[kMaxPartials];
    /** The rms against the reference. */
    float level;
};

/** The envelope's points per octave. */
float envPointsPerOctave();

/**
 * One frame's analysis, in steps so a live source can spread it over the
 * blocks of a hop. Allocates when built; the steps don't.
 */
class Analyser {
  public:
    /** The steps a frame takes after begin(). */
    static constexpr int kSteps = 6;
    Analyser();
    void prepare(float sampleRate);
    void reset();
    /**
     * Analyses [window], kFftSize high-passed samples oldest first, into
     * [out]. [pitchHz] and [sure] are the source's pitch; [levelRef] the rms
     * that reads as 1. A silent window keeps the shapes [out] already has.
     */
    void frame(const float *window, float pitchHz, float sure, float levelRef, Frame &out);
    /**
     * Starts a frame of [window] (read until the frame is done) on top of
     * [from]; step() then does the work and says when it's done. Without
     * [envelope] only the level and harmonics are read, and the envelope is
     * [from]'s.
     */
    void begin(const float *window, float pitchHz, float sure, float levelRef, const Frame &from, bool envelope = true);
    bool step();
    bool busy() const { return stage != 0; }
    void cancel() { stage = 0; }
    /** The frame step() finished last. */
    const Frame &result() const { return work; }
    /** A frame with nothing in it: flat envelope, a saw's harmonics, level 0. */
    static void blank(Frame &f);

  private:
    /** Envelope points [from, to). */
    void envelope(int from, int to);

    float sampleRate = 48000.0f;
    /** Half the size: the real window goes in as kFftSize / 2 complex pairs. */
    dsp::Fft fft{kFftSize / 2};
    std::vector<float> win, re, im, mag, logMag, twr, twi;
    /** The lifter as a smoothing kernel on the log spectrum, its taps from the centre out. */
    std::vector<float> kernel;
    float winPower = 1.0f;
    /** Each envelope point's bin and fraction, and the first point the loudest is looked for from. */
    std::vector<int> pointBin;
    std::vector<float> pointFrac;
    int firstLoud = 0;
    /** The frame under way: its step (0 for none), input and what's carried between steps. */
    int stage = 0;
    const float *src = nullptr;
    float pitch = 0.0f, sureness = 0.0f, ref = 1.0f, top = 0.0f, loudest = -1e30f, sk0 = 0.0f, sk1 = 0.0f;
    int lastK = -1;
    bool wantEnv = true;
    Frame work{};
};

/** The source's high-pass: six poles at kRumbleHz, Butterworth. */
constexpr float kRumbleHz = 70.0f;
struct Rumble {
    dsp::Biquad a, b, c;
    void prepare(float sampleRate) {
        a.highpass(kRumbleHz, 0.5176381f, sampleRate);
        b.highpass(kRumbleHz, 0.7071068f, sampleRate);
        c.highpass(kRumbleHz, 1.9318517f, sampleRate);
        reset();
    }
    void reset() {
        a.reset();
        b.reset();
        c.reset();
    }
    float process(float x) { return c.process(b.process(a.process(x))); }
};

/**
 * Another track's sound as it plays. push() every block, then advance():
 * when a hop has come in it takes the latest window and works through its
 * analysis a step at a time, so no block carries a whole frame. Audio
 * thread, nothing allocated after construction.
 */
class LiveSource {
  public:
    void prepare(float sampleRate);
    void reset();
    /** Takes [n] samples of the track. [pitch] feeds the pitch follower too, and [look] lets it look. */
    void push(const float *x, int n, bool pitch, bool look);
    /**
     * Runs [steps] steps of the analysis, starting a frame when a hop has
     * come in. After a sleep the first frame is done whole, so a note
     * starts on what the track is playing now.
     */
    void advance(int steps);
    /** Drops a frame under way; [sleep] when the machine has gone quiet. */
    void cancel(bool sleep) {
        analyser.cancel();
        stale = stale || sleep;
    }
    const Frame &current() const { return frame; }
    /** Counts the frames finished, so readers know when current() changed. */
    uint32_t serial() const { return frames; }

  private:
    Analyser analyser;
    Rumble rumble;
    dsp::PitchFollow follow;
    /** Partials mode: the pitch is followed and the envelope isn't read. */
    bool harmOnly = false;
    float ring[kFftSize] = {};
    float window[kFftSize] = {};
    float hp[kFftSize] = {};
    int write = 0, fresh = 0;
    uint32_t frames = 0;
    bool stale = true;
    Frame frame{};
};

/** A file analysed into frames, one a hop. Built on a worker, then mounted. */
struct Spectra {
    std::string name;
    float hopSeconds = static_cast<float>(kHop) / 48000.0f;
    /** The file's length analysed, seconds. */
    float seconds = 0.0f;
    std::vector<Frame> frames;

    /**
     * Analyses [frames] of mono audio at [sampleRate] (only the first
     * kFileSeconds). Frame k is centred on sample k * kHop. Levels are set
     * so the loudest frames but a twentieth read 1. Takes a few hundred ms
     * for a minute; never on the audio thread.
     */
    static std::unique_ptr<Spectra> analyse(const float *mono, int32_t frames, float sampleRate);

    int64_t bytes() const { return static_cast<int64_t>(frames.capacity() * sizeof(Frame)); }
};

} // namespace acidulous::machine::abacus
