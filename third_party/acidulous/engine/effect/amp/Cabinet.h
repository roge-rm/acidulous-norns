#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/Filter.h>
#include <engine/dsp/Math.h>

// A modelled speaker cabinet. A model is used instead of an impulse response
// so the cabinet size can be changed smoothly, from a small combo to a 4x12
// or a bass 8x10, and so the app doesn't ship recordings it didn't make.
//
// The parts of a guitar cab's response that matter:
//
//   - a box rolloff with a bump at its cutoff, from ~130 Hz on a sealed 1x12
//     down to ~50 Hz on an 8x10
//   - baffle colour in the low mids
//   - cone breakup between 1.5 and 4 kHz, the speaker's voice
//   - a steep cliff at 4-6 kHz where the cone stops moving as one piece. This
//     matters most. Without it the amp sounds like a distortion pedal with EQ.
//
// The size law is the same as Filament's.
namespace acidulous::effect::amp {

class Cabinet {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        box.setSampleRate(sr);
        comb.prepare(128);
        room.prepare(static_cast<int32_t>(sr * 0.05f));
        reset();
    }

    void reset() {
        box.reset();
        for (auto &b : peaks) b.reset();
        cliffA.reset();
        cliffB.reset();
        roomTone.reset();
        comb.clear();
        room.clear();
        builtFor = -1.0f;
    }

    /**
     * Updates coefficients once a block, only when something moved, since
     * the std::pow calls add up across many racks. The threshold is 2e-4
     * because a smoothed parameter never quite reaches its target.
     */
    void set(float size, float cone, float mic, float edge, float roomAmt) {
        const float key = size + cone * 3.0f + mic * 7.0f + edge * 11.0f + roomAmt * 13.0f;
        if (std::fabs(key - builtFor) < 2e-4f) return;
        builtFor = key;
        this->edgeAmt = edge;
        this->roomAmt = roomAmt;

        // Filament's law: 2.14 at 0, 1.0 in the middle, 0.466 at the top.
        const float s = std::pow(2.0f, (0.5f - size) * 2.2f);

        // The box scales the low end almost in proportion, but breakup comes
        // from the cone, which changes less with size, hence the fractional
        // exponents. Scaling everything by s would just make a tone control.
        const float boxHz = 95.0f * s;
        const float coneScale = std::pow(s, 0.5f);
        const float cliffScale = std::pow(s, 0.35f) * std::pow(2.0f, (cone - 0.5f) * 0.7f) *
                                 std::pow(2.0f, -1.4f * mic) * std::pow(2.0f, -0.5f * edge) *
                                 std::pow(2.0f, -0.25f * roomAmt);

        boxCorner = dsp::clampf(boxHz, 30.0f, 400.0f);
        box.set(boxCorner, 0.42f);
        // The bump at the box's corner, like a sealed cabinet.
        peaks[0].peak(dsp::clampf(110.0f * s, 30.0f, 400.0f), 4.5f - 2.0f * size + 1.5f * mic, 1.6f, sr);
        // The baffle, and the woody mid a rim mic hears.
        peaks[1].peak(dsp::clampf(210.0f * std::pow(s, 0.9f), 60.0f, 900.0f),
                      -2.5f + 1.2f * size + 2.0f * edge, 1.2f, sr);
        // Cone breakup: three peaks. Two sound like one wide hump and a fourth
        // isn't audible.
        const float flat = 1.0f - 0.55f * mic;
        const float q = 1.2f + 2.3f * cone;
        peaks[2].peak(dsp::clampf(1500.0f * coneScale, 400.0f, 6000.0f),
                      (-2.0f + 6.0f * cone) * flat, q, sr);
        peaks[3].peak(dsp::clampf(2600.0f * coneScale, 600.0f, 9000.0f),
                      (-4.0f + 10.0f * cone) * (1.0f - 0.25f * size) * flat, q, sr);
        peaks[4].peak(dsp::clampf(4000.0f * std::pow(s, 0.4f), 900.0f, 12000.0f),
                      (-3.0f + 7.0f * cone) * flat, q, sr);
        cliffA.lowpass(dsp::clampf(4800.0f * cliffScale, 700.0f, 16000.0f), 0.9f, sr);
        cliffB.lowpass(dsp::clampf(6200.0f * cliffScale, 900.0f, 18000.0f), 0.6f, sr);
        roomTone.lowpass(3500.0f, 0.707f, sr);

        // The comb a rim mic hears, between the near and far edge of the cone.
        combDelay = (0.12f + 0.28f * edge) * 0.001f * sr;
        trim = measuredTrim();
    }

    /** One sample, one channel. */
    float process(float x) {
        float y = box.highpass(x);
        for (auto &b : peaks) y = b.process(y);
        y = cliffB.process(cliffA.process(y));
        if (edgeAmt > 1e-4f) {
            comb.write(y);
            y -= 0.45f * edgeAmt * comb.read(combDelay);
        }
        if (roomAmt > 1e-4f) {
            room.write(y);
            // Three early reflections and no feedback, so the room can't ring.
            // The spacings aren't harmonic, so the taps don't form a pitch.
            const float r = 0.42f * room.read(0.0068f * sr) + 0.30f * room.read(0.0113f * sr) +
                            0.22f * room.read(0.0191f * sr);
            y += roomAmt * 0.55f * roomTone.process(r);
        }
        return y * trim;
    }

    /** What the chain does at [hz], for the harness and for the trim. */
    float magnitudeAt(float hz) const {
        float g = 1.0f;
        for (const auto &b : peaks) g *= b.magnitudeAt(hz, sr);
        g *= cliffA.magnitudeAt(hz, sr) * cliffB.magnitudeAt(hz, sr);
        // The box is an `Svf` with no magnitudeAt, so it's approximated by a
        // two-pole highpass at its current corner. That's close enough for
        // level matching, but it must use the current corner or the trim
        // won't follow `size`.
        const float w = hz / boxCorner;
        const float hp = w * w / (1.0f + w * w);
        g *= hp;
        return g;
    }

    float ringSeconds(int32_t which) const { return peaks[which].ringSeconds(sr); }
    /**
     * The level compensation, which [magnitudeAt] leaves out because the
     * trim is computed from it. Multiply the two for the actual response.
     */
    float outputTrim() const { return trim; }
    static constexpr int32_t kPeaks = 5;

  private:
    /**
     * Keeps every cabinet setting within a couple of dB of the others, so
     * the knobs don't also change volume. Measured from the chain's own
     * response at twelve log-spaced frequencies.
     */
    float measuredTrim() const {
        double sum = 0.0;
        for (int32_t i = 0; i < 12; ++i) {
            const float hz = 80.0f * std::pow(2.0f, static_cast<float>(i) * 0.55f);
            const float g = magnitudeAt(hz);
            sum += static_cast<double>(g) * g;
        }
        const double rms = std::sqrt(sum / 12.0);
        return rms > 1e-6 ? static_cast<float>(dsp::clampf(0.55f / static_cast<float>(rms), 0.25f, 4.0f))
                          : 1.0f;
    }

    dsp::Svf box;
    dsp::Biquad peaks[kPeaks], cliffA, cliffB, roomTone;
    dsp::DelayLine comb, room;
    float sr = 48000.0f;
    float combDelay = 8.0f, edgeAmt = 0.0f, roomAmt = 0.0f, trim = 1.0f;
    float boxCorner = 95.0f;
    float builtFor = -1.0f;
};

} // namespace acidulous::effect::amp
