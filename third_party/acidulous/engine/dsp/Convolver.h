#pragma once
#include <cstdint>
#include <vector>

// Stereo convolution with a long impulse and no latency.
//
// The first 32 taps run directly. The rest are cut into partitions that grow
// eightfold (32, 256, 2048, 16384 samples), each size an overlap-save FFT
// convolution with a frequency-domain delay line. The 32-sample stage runs
// whole every 32 samples. A larger stage's work is due a whole partition
// after its input is complete, so it's spread in equal shares over that
// time, a share every 32 samples, which keeps the cost per block flat.
//
// Both channels go through one complex FFT (left in the real part, right in
// the imaginary). Everything happens on a fixed 32-sample grid counted from
// reset(), so the output doesn't depend on how the caller splits its blocks.
//
// Built (allocated, impulse transformed) off the audio thread. process() and
// reset() don't allocate.
namespace acidulous::dsp {

class Convolver {
  public:
    /** The direct part and the smallest partition, in samples. */
    static constexpr int32_t kGrid = 32;

    /**
     * [right] null for a mono impulse, which plays the same on both sides.
     * [largest] caps the partition size, a power of two from kGrid to 16384;
     * kGrid alone is a uniform convolution, for measuring.
     */
    Convolver(const float *left, const float *right, int32_t frames, int32_t largest = 16384);

    /** Back to silence. Clears only what is read before it's written. */
    void reset();

    /** The wet signal only. In and out may be the same buffers. */
    void process(const float *inL, const float *inR, float *outL, float *outR, int32_t frames);

    int32_t length() const { return frames_; }
    bool stereo() const { return stereo_; }

  private:
    struct Stage {
        int32_t size = 0;    // partition, P
        int32_t fft = 0;     // 2P
        int32_t bits = 0;    // log2 of fft
        int32_t offset = 0;  // first tap
        int32_t parts = 0;
        bool spread = false; // run in shares rather than at once
        // Per partition, P+1 slots of left re, left im, right re, right im, side
        // by side, each slot one bin of the half spectrum (see forSlots() for
        // which). A mono impulse stores the left pair only.
        std::vector<float> h;
        int32_t hStride = 0;
        // The delay line of input spectra, the same layout, one slot per partition.
        std::vector<float> fdl;
        int32_t head = 0;
        int32_t filled = 0;  // slots written since reset
        std::vector<float> re, im, acc;
        // Twiddles for each pass, laid out end to end: a pass of span len
        // starts at len/2 - 1.
        std::vector<float> twc, tws;

        // The job in progress.
        bool busy = false;
        int64_t start = 0;   // the sample count when its input was complete
        int32_t phase = 0;
        int64_t index = 0;   // within the phase
        int64_t done = 0, total = 0;
        int32_t share = 0;
        int32_t activeParts = 0;
    };

    void tick();
    void begin(Stage &s);
    void advance(Stage &s, int64_t target);
    int64_t phaseUnits(const Stage &s, int32_t phase) const;
    void runPhase(Stage &s, int32_t phase, int64_t from, int64_t to);
    // Decimation in frequency: natural order in, bit-reversed out.
    static void forward(Stage &s, int64_t from, int64_t to);
    // Decimation in time: bit-reversed in, natural out. Same sign as forward.
    static void inverse(Stage &s, int64_t from, int64_t to);

    int32_t frames_ = 0;
    bool stereo_ = false;
    // Direct taps, reversed, and the input history twice over so a window
    // never wraps.
    float direct_[2][kGrid] = {};
    float hist_[2][2 * kGrid] = {};
    int32_t directTaps_ = 0;

    std::vector<Stage> stages_;
    // Input since reset, read by the stages' gathers.
    std::vector<float> in_[2];
    int32_t inMask_ = 0;
    // Wet output still to come, added to by the stages, cleared as it's read.
    std::vector<float> out_[2];
    int32_t outMask_ = 0;
    int64_t n_ = 0; // samples since reset
};

} // namespace acidulous::dsp
