#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The pitch of a sound as it plays, for following another track's melody.
//
// Audio thread, nothing allocated. The sound is low-passed and kept at an
// eighth of the rate in a ring; the difference function (YIN's, normalised
// by its running mean) is taken over the last 33 ms, a share of it a call,
// and the first dip under the threshold is the period. Good to a few cents
// on a held note below 1 kHz, and it says how sure it is.
namespace acidulous::dsp {

class PitchFollow {
  public:
    static constexpr int kDown = 8;
    /** The ring and the window, at the lower rate. */
    static constexpr int kRing = 512;
    static constexpr int kWindow = 200;
    /** The lowest and highest pitch it looks for. */
    static constexpr float kLowest = 60.0f, kHighest = 1500.0f;

    void prepare(float sampleRate) {
        rate = sampleRate / kDown;
        // Two one-poles at 2 kHz before taking every 8th sample.
        pole = std::exp(-6.2831853f * 2000.0f / sampleRate);
        maxLag = static_cast<int>(rate / kLowest);
        minLag = static_cast<int>(rate / kHighest);
        if (maxLag > kRing - kWindow - 2) maxLag = kRing - kWindow - 2;
        reset();
    }

    void reset() {
        for (float &s : ring) s = 0.0f;
        write = 0;
        phase = 0;
        lp1 = lp2 = 0.0f;
        level = 0.0f;
        hz = 0.0f;
        sure = 0.0f;
        lag = 0;
    }

    /** Takes [frames] samples of the sound. */
    void push(const float *x, int frames) {
        for (int i = 0; i < frames; ++i) {
            lp1 = x[i] + pole * (lp1 - x[i]);
            lp2 = lp1 + pole * (lp2 - lp1);
            level += (std::fabs(x[i]) - level) * 0.001f;
            if (++phase == kDown) {
                phase = 0;
                ring[write] = lp2;
                write = (write + 1) % kRing;
            }
        }
    }

    /**
     * Looks a little further for the pitch: [share] of the lags, so a whole
     * look is spread over 1 / share calls and no block carries it alone. A
     * look starts from what's been pushed when it starts; when it ends,
     * [pitch] and [sureness] change.
     */
    void update(float share) {
        if (lag == 0) {
            if (level < 1e-4f) {
                sure = 0.0f;
                return;
            }
            // The ring in order, oldest first, so the sums below run straight.
            for (int i = 0; i < kRing; ++i) line[i] = ring[(write + i) % kRing];
            running = 0.0f;
            found = 0.0f;
            foundDepth = 1.0f;
            d[0] = 1.0f;
            lag = 1;
        }
        const float *now = line + kRing - kWindow;
        const int last = std::min(maxLag, lag + static_cast<int>(std::ceil(share * static_cast<float>(maxLag))) - 1);
        // d(lag): how unlike the window is to itself [lag] samples back,
        // against the mean of the lags so far.
        for (; lag <= last; ++lag) {
            const float *then = now - lag;
            float sum = 0.0f;
            for (int j = 0; j < kWindow; ++j) {
                const float e = now[j] - then[j];
                sum += e * e;
            }
            running += sum;
            d[lag] = running > 0.0f ? sum * static_cast<float>(lag) / running : 1.0f;
            // The first dip under the threshold, at its bottom.
            if (found == 0.0f && lag >= minLag + 2 && d[lag - 1] < kThreshold && d[lag - 1] <= d[lag] && d[lag - 1] <= d[lag - 2]) {
                const float a = d[lag - 2], b = d[lag - 1], c = d[lag];
                const float bend = a - 2.0f * b + c;
                const float shift = bend > 0.0f ? 0.5f * (a - c) / bend : 0.0f;
                found = static_cast<float>(lag - 1) + shift;
                foundDepth = b;
            }
        }
        if (lag > maxLag) {
            lag = 0;
            if (found > 0.0f) {
                hz = rate / found;
                sure = 1.0f - foundDepth;
            } else {
                sure = 0.0f;
            }
        }
    }

    /** The pitch found last, Hz, and how sure, 0 to 1 (0 when there's nothing to follow). */
    float pitch() const { return hz; }
    float sureness() const { return sure; }

  private:
    static constexpr float kThreshold = 0.15f;
    float ring[kRing] = {};
    float line[kRing] = {};
    float d[kRing] = {};
    int write = 0, phase = 0, minLag = 4, maxLag = 100;
    float rate = 6000.0f, pole = 0.7f, lp1 = 0.0f, lp2 = 0.0f, level = 0.0f;
    float hz = 0.0f, sure = 0.0f;
    /** A look under way: the next lag, the sums so far and what's been found. */
    int lag = 0;
    float running = 0.0f, found = 0.0f, foundDepth = 1.0f;
};

} // namespace acidulous::dsp
