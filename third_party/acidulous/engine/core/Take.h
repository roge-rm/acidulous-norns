#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// A piece of audio with its transients marked. Pollen's grains read it and
// Dice cuts it into slices.
//
// A cloud can be fed from a decoded file or from the machine's own ring of
// recent microphone input. Both are read through one View, resolved once a
// block, so grains don't need to know which.
namespace acidulous::audio {

/**
 * Finds transients by comparing each hop's energy to a slow average. Cheap,
 * works on drums and voices, and can run on the audio thread as a live buffer
 * fills.
 */
class OnsetFinder {
  public:
    void reset(float sampleRate) {
        sr = sampleRate;
        hopSize = 256;
        minGap = static_cast<int32_t>(sampleRate * 0.03f); // 30 ms: two hits, not one hit twice
        energy = 0.0f;
        mean = 0.0f;
        hop = 0;
        sinceLast = minGap;
    }

    /** Takes one frame. Returns true when it starts an onset. */
    bool push(float l, float r) {
        energy += std::abs(l) + std::abs(r);
        ++sinceLast;
        if (++hop < hopSize) return false;
        hop = 0;
        const float e = energy;
        energy = 0.0f;
        const bool onset = e > mean * 1.7f && e > 0.02f * static_cast<float>(hopSize) && sinceLast >= minGap;
        mean += (e - mean) * 0.15f;
        if (onset) sinceLast = 0;
        return onset;
    }

  private:
    float sr = 48000.0f, energy = 0.0f, mean = 0.0f;
    int32_t hopSize = 256, minGap = 1440, hop = 0, sinceLast = 0;
};

/** A decoded file, with its transients. Built on a worker, never changed. */
struct Take {
    std::vector<float> left, right; // both always filled, mono is copied to both
    int32_t frames = 0;
    std::vector<int32_t> onsets; // sorted, in frames
    std::string name;
    /**
     * How many 4/4 bars the take most likely is, for playing it at the song's
     * tempo: 0.5, 1, 2, 4, 8 or 16. 0 when no bar count puts it between 70
     * and 180 bpm.
     */
    float bars = 0.0f;

    void detect(float sampleRate);
    float guessBars(float sampleRate) const;
};

/** The audio a reader reads. Resolved once per block. */
struct View {
    const float *l = nullptr, *r = nullptr;
    int32_t frames = 0;
    const int32_t *onsets = nullptr;
    int32_t onsetCount = 0;

    bool usable() const { return l != nullptr && frames > 1; }

    /** The onset nearest [pos], or [pos] itself when there are none. */
    float nearestOnset(float pos) const {
        if (onsetCount <= 0) return pos;
        float best = static_cast<float>(onsets[0]);
        float bestD = std::abs(best - pos);
        for (int32_t i = 1; i < onsetCount; ++i) {
            const float d = std::abs(static_cast<float>(onsets[i]) - pos);
            if (d < bestD) { bestD = d; best = static_cast<float>(onsets[i]); }
        }
        return best;
    }
};

} // namespace acidulous::audio
