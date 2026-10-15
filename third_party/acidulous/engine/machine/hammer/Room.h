#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// The room a piano is heard in: what keeps the knock and the note going
// between and below the partials for a second or two after the strings
// themselves have moved on. The recordings have it under every note, 10 to
// 28 dB more than the dry model at 60-250 Hz under a treble note, a second
// in; without it the model sounds like a microphone inside the case.
//
// Eight delay lines (four lean) fed back through a Hadamard mix, each with
// a one-pole that makes the highs die sooner than the lows: a T60 of [low]
// at the bottom and [high] at the top, set as times like the strings'.
namespace acidulous::machine::hammer {

class Room {
  public:
    static constexpr int kLines = 8;

    void prepare(float sampleRate) {
        sr = sampleRate;
        // Spread so no two share a factor much: 29.7 to 73.3 ms.
        static constexpr float kMs[kLines] = {29.7f, 37.1f, 41.1f, 43.7f, 53.0f, 59.9f, 67.1f, 73.3f};
        size = 1;
        while (static_cast<float>(size) < sr * 0.08f) size <<= 1;
        for (int i = 0; i < kLines; ++i) {
            length[i] = static_cast<int>(kMs[i] * 0.001f * sr);
            line[i].assign(static_cast<size_t>(size), 0.0f);
        }
        setDecay(1.5f, 0.5f);
        clear();
    }

    void clear() {
        for (int i = 0; i < kLines; ++i) {
            std::fill(line[i].begin(), line[i].end(), 0.0f);
            damp[i] = 0.0f;
        }
        at = 0;
        energy = 0.0f;
    }

    /** T60 at the bottom and at the top, seconds. */
    void setDecay(float low, float high) {
        for (int i = 0; i < kLines; ++i) {
            const float d = static_cast<float>(length[i]);
            const float gl = std::pow(10.0f, -3.0f * d / (low * sr));
            const float gh = std::pow(10.0f, -3.0f * d / (high * sr));
            // A one-pole with DC gain gl and Nyquist gain gh.
            gain[i] = gl;
            pole[i] = (gl - gh) / (gl + gh);
        }
    }

    /** One sample in (the board's left and right), the room's left and right out. */
    void step(float inL, float inR, int lines, float &outL, float &outR) {
        const int n = lines >= kLines ? kLines : 4;
        float y[kLines];
        for (int i = 0; i < n; ++i) {
            const float x = line[i][static_cast<size_t>((at - length[i]) & (size - 1))];
            damp[i] = gain[i] * (1.0f - pole[i]) * x + pole[i] * damp[i];
            y[i] = damp[i];
        }
        // Hadamard, normalised.
        for (int h = 1; h < n; h <<= 1) {
            for (int i = 0; i < n; i += h << 1) {
                for (int j = i; j < i + h; ++j) {
                    const float a = y[j], b = y[j + h];
                    y[j] = a + b;
                    y[j + h] = a - b;
                }
            }
        }
        const float norm = n == kLines ? 0.35355339f : 0.5f;
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float back = y[i] * norm;
            line[i][static_cast<size_t>(at & (size - 1))] = back + ((i & 1) == 0 ? inL : inR);
            if ((i & 1) == 0) l += back; else r += back;
        }
        at = (at + 1) & (size - 1);
        outL = l * norm;
        outR = r * norm;
        energy += (std::fabs(outL) + std::fabs(outR) + std::fabs(inL) + std::fabs(inR) - energy) * 0.0005f;
    }

    float loudness() const { return energy; }

  private:
    float sr = 48000.0f;
    int size = 4096;
    int at = 0;
    int length[kLines] = {};
    float gain[kLines] = {}, pole[kLines] = {}, damp[kLines] = {};
    std::vector<float> line[kLines];
    float energy = 0.0f;
};

} // namespace acidulous::machine::hammer
