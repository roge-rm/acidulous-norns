#pragma once
#include <cmath>
#include <cstdint>
#include <engine/core/Constants.h>

// Follows an external MIDI clock.
//
// 24 pulses per quarter note arrive, each stamped with the frame it was heard
// at. Working the tempo out from each gap would make the song wobble with the
// transport's jitter (about 1 ms on USB, much worse on BLE, which batches on
// a 7.5-15 ms connection interval).
//
// So this is a second-order delay-locked loop over the period, like a DAW
// uses. It tracks where it expects the next pulse and how long a pulse is,
// and corrects both from the error, the rate slowly and the phase a bit
// faster. The bandwidth is well under 1 Hz, so jitter averages out and a real
// tempo change is still followed within a bar or two.
//
// The position is read from the loop's model, not from the last pulse, so
// it's smooth. Pure maths over frames, so it can be tested on its own.

namespace acidulous::seq {

class ClockFollower {
  public:
    /** Ticks between one MIDI clock pulse and the next: 240 / 24. */
    static constexpr int32_t kTicksPerPulse = kPPQN / 24;

    void reset(float sr) {
        sampleRate = sr > 0.0f ? sr : static_cast<float>(kSampleRate);
        seen = 0;
        gathered = 0;
        anchorPulse = 0;
        anchorFrame = 0.0;
        expected = 0.0;
        periodFrames = 0.0;
        lastFrame = 0;
        errorFrames = 0.0;
        settled = 0;
    }

    /** In Hz. Lower follows jitter less and real tempo changes more slowly. */
    void setBandwidth(float hz) { bandwidth = hz < 0.001f ? 0.001f : (hz > 5.0f ? 5.0f : hz); }

    /**
     * A clock byte arrived, heard at [frame].
     *
     * The first few set the pulse length from the median interval. A single
     * interval can be off by half on a jittery transport, and a loop started
     * from a bad period never recovers because the runaway guard scales with
     * the period too. The median throws outliers away.
     */
    void pulse(int64_t frame) {
        if (seen == 0) {
            anchorPulse = 0;
            anchorFrame = static_cast<double>(frame);
            lastFrame = frame;
            gathered = 0;
            seen = 1;
            return;
        }
        anchorPulse += 1;

        if (seen == 1) {
            if (gathered < kGather) {
                intervals[gathered++] = frame - lastFrame;
            }
            lastFrame = frame;
            if (gathered < kGather) {
                return;
            }
            int64_t sorted[kGather];
            for (int i = 0; i < kGather; ++i) {
                sorted[i] = intervals[i];
            }
            for (int i = 1; i < kGather; ++i) { // insertion sort, only 9 items
                const int64_t v = sorted[i];
                int j = i - 1;
                while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
                sorted[j + 1] = v;
            }
            periodFrames = static_cast<double>(sorted[kGather / 2]);
            periodFrames = clampPeriod(periodFrames);
            anchorFrame = static_cast<double>(frame);
            expected = anchorFrame + periodFrames;
            settled = 0;
            seen = 2;
            return;
        }

        lastFrame = frame;
        // Where the loop expected this pulse versus where it landed.
        const double e = static_cast<double>(frame) - expected;
        errorFrames = e;

        // A jump much bigger than a pulse isn't jitter, the master was stopped,
        // relocated or unplugged. Work the period out again from scratch so a
        // wrong one can't survive.
        if (std::fabs(e) > periodFrames * 4.0) {
            seen = 1;
            gathered = 0;
            settled = 0;
            return;
        }

        // No single pulse may move the model by more than half a pulse. On a
        // transport that batches, like Bluetooth, pulses can land close enough
        // to be out of order, and an unclamped loop would drift off. Clamped,
        // the noise just averages out.
        const double bound = periodFrames * 0.5;
        const double ce = e > bound ? bound : (e < -bound ? -bound : e);

        // Second-order loop. omega is in radians per pulse, so the bandwidth in
        // Hz is divided by the pulse rate.
        const double pulseRate = sampleRate / (periodFrames > 1.0 ? periodFrames : 1.0);
        const double omega = 6.283185307179586 * static_cast<double>(bandwidth) / pulseRate;
        const double b = 1.4142135623730951 * omega; // 2 zeta omega, zeta = 1/sqrt(2)
        const double c = omega * omega;

        anchorFrame = expected + b * ce; // the corrected position of this pulse
        periodFrames = clampPeriod(periodFrames + c * ce);
        expected = anchorFrame + periodFrames;

        if (std::fabs(e) < periodFrames * 0.05) {
            if (settled < 64) {
                ++settled;
            }
        } else {
            settled = 0;
        }
    }

    /** No pulses for a while, so the master has stopped. */
    bool stale(int64_t frameNow) const {
        if (seen < 2) {
            return true;
        }
        return static_cast<double>(frameNow - lastFrame) > periodFrames * 8.0;
    }

    bool running() const { return seen >= 2; }
    /** Settled long enough to trust. */
    bool locked() const { return settled >= 8; }

    double framesPerPulse() const { return periodFrames; }
    double framesPerTick() const {
        return periodFrames > 0.0 ? periodFrames / static_cast<double>(kTicksPerPulse) : 0.0;
    }

    float bpm() const {
        if (periodFrames <= 0.0) {
            return 0.0f;
        }
        return static_cast<float>(60.0 * static_cast<double>(sampleRate) / (periodFrames * 24.0));
    }

    /**
     * Where the external clock is in ticks at [frame], from the model rather
     * than the last pulse so it doesn't carry the jitter.
     */
    double tickAt(int64_t frame) const {
        if (seen < 2) {
            return 0.0;
        }
        const double perTick = framesPerTick();
        if (perTick <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(anchorPulse) * kTicksPerPulse +
               (static_cast<double>(frame) - anchorFrame) / perTick;
    }

    /** The last pulse's error in milliseconds, for the readout. */
    float phaseErrorMs() const {
        return static_cast<float>(errorFrames * 1000.0 / static_cast<double>(sampleRate));
    }
    int64_t pulseCount() const { return anchorPulse; }

    /** A Song Position Pointer arrived. The external position is now [tick]. */
    void relocate(int64_t tick) { anchorPulse = tick / kTicksPerPulse; }

  private:
    /** Enough intervals for a useful median (48 a second at 120 bpm). */
    static constexpr int kGather = 9;

    /** Clamp to 20-300 bpm. */
    double clampPeriod(double p) const {
        const double fastest = static_cast<double>(sampleRate) * 60.0 / (300.0 * 24.0);
        const double slowest = static_cast<double>(sampleRate) * 60.0 / (20.0 * 24.0);
        return p < fastest ? fastest : (p > slowest ? slowest : p);
    }

    int64_t intervals[kGather] = {};
    int32_t gathered = 0;
    float sampleRate = static_cast<float>(kSampleRate);
    float bandwidth = 0.5f;
    int32_t seen = 0;
    int32_t settled = 0;
    int64_t anchorPulse = 0;
    double anchorFrame = 0.0;
    double expected = 0.0;
    double periodFrames = 0.0;
    int64_t lastFrame = 0;
    double errorFrames = 0.0;
};

} // namespace acidulous::seq
