#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Math.h>
#include <vector>

// One string.
//
// A delay line one wavelength long with a filter in the loop, so the highs
// fade faster than the lows, like a real string. On top of that:
//
//   - Dispersion: real strings are stiff, so their partials run sharp. An
//     allpass chain in the loop does the same, like a piano.
//   - Tension: a string pulled hard is briefly sharp and settles as it
//     decays. The loop length follows the energy going round it.
//   - Damper: a second tap partway along is subtracted, like a finger on the
//     string. At a node it gives the harmonic instead of the note.
namespace acidulous::machine {
using dsp::clampf;

class Waveguide {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        buffer.assign(static_cast<size_t>(sampleRate / 18.0f) + 4, 0.0f); // down to ~18 Hz
        clear();
    }

    void clear() {
        for (auto &v : buffer) v = 0.0f;
        write = 0;
        loopState = 0.0f;
        pending = 0.0f;
        for (auto &a : allpassState) a = 0.0f;
        energy = 0.0f;
        dcIn = dcOut = 0.0f;
        // The bow and breath push against the last output sample, so reset it
        // too or a new note starts from the old one.
        lastOut = 0.0f;
    }

    /**
     * Does nothing if the note hasn't changed.
     *
     * `refreshLoop()` is expensive, and Nexus's string block calls these
     * setters every sample, so they skip the work when nothing changed.
     */
    void setFrequency(float hz) {
        const float d = clampf(sr / clampf(hz, 18.0f, 8000.0f), 2.0f,
                               static_cast<float>(buffer.size() - 2));
        if (d == baseDelay) return;
        baseDelay = d;
        refreshDispersion();
        refreshLoop();
    }

    /**
     * [loopGain] is the gain one whole turn should have, and the feedback is
     * set to get it.
     *
     * The tone filter and DC blocker also lose a little each turn, which adds
     * up fast at high turn rates. So their loss is divided out of the
     * feedback, and a sustain time in seconds comes out right.
     */
    void setDamping(float loopGain, float toneCutoff01) {
        gain = clampf(loopGain, 0.0f, 1.02f);
        const float t = clampf(toneCutoff01, 0.02f, 1.0f);
        if (t != tone) { tone = t; refreshLoop(); }
        else effGain = clampf(gain / loopKeeps, 0.0f, 1.05f);
    }
    void setDispersion(float amount01, int stages) {
        const float a = clampf(amount01, 0.0f, 1.0f);
        const int n = stages < 0 ? 0 : (stages > kAllpass ? kAllpass : stages);
        if (a == dispersion && n == allpassStages) return; // see setFrequency
        dispersion = a;
        allpassStages = n;
        refreshDispersion();
        refreshLoop();
    }
    void setTension(float amount01) { tension = clampf(amount01, 0.0f, 1.0f); }
    /**
     * Where the DC blocker sits, as a fraction of the note (kDcBelow unless
     * set). It leads in phase at the note, and the loop is shortened to make
     * up for it there, so the higher partials come out flat by as much: about
     * 27 cents at a tenth. A plucked string with nothing pushing it can sit
     * far lower and stay in tune all the way up.
     */
    void setDcCorner(float fraction) {
        const float f = clampf(fraction, 0.002f, 0.2f);
        if (f == dcBelow) return;
        dcBelow = f;
        refreshLoop();
    }
    /** Where the damper sits, 0..1 along the string, and how hard it presses. */
    void setDamper(float position01, float pressure01) {
        const float p = clampf(position01, 0.0f, 1.0f), f = clampf(pressure01, 0.0f, 1.0f);
        if (p != damperPos || f != damperPressure) {
            damperPos = p; damperPressure = f;
            refreshLoop();
        }
    }

    /**
     * Adds energy from outside, like a neighbouring string. It's held until
     * the next step instead of written to the buffer, since step() overwrites
     * that slot.
     */
    void excite(float value) { pending += value; }

    /**
     * The same, but scaled per turn round the string.
     *
     * A string is D samples round, so something added every sample goes in D
     * times a turn, and D varies ten times across the range. Scaling per turn
     * makes a coupling the same strength at every pitch, so it doesn't run
     * away on low strings.
     */
    void exciteOverTurn(float value) { pending += value / baseDelay; }
    /** One sample as a fraction of a turn. */
    float turnScale() const { return 1.0f / baseDelay; }

    float step(float input) {
        // Tension modulation: the harder it rings, the shorter the loop.
        // The buffer adds exactly `delay` samples. The rest of the loop has
        // its own phase delay, and `loopTrim` takes it off so the string
        // doesn't play flat. See refreshLoop().
        const float wanted = baseDelay * (1.0f - tension * 0.02f * energy) - loopTrim;
        const float delay = clampf(wanted, 2.0f, static_cast<float>(buffer.size() - 2));
        float out = read(delay);
        if (damperPressure > 0.0f) {
            // A damper can only take energy away. Subtracting a tap makes a
            // comb with peaks of 1 + k, which would add gain, so it's divided
            // by the peak. It still nulls the note at a node.
            const float k = damperPressure * 0.9f;
            const float tapped = read(clampf(delay * damperPos, 1.0f, delay - 1.0f));
            out = (out - tapped * k) / (1.0f + k);
        }
        // Loop filter: one pole, so the top goes first.
        loopState += (out - loopState) * tone;
        float fed = loopState * effGain;
        for (int i = 0; i < allpassStages; ++i) {
            const float y = disperse * fed + allpassState[i];
            allpassState[i] = fed - disperse * y;
            fed = y;
        }
        float next = fed + input + pending;
        pending = 0.0f;
        // A DC blocker, since a string with fixed ends can't hold a steady
        // offset. The hammer and breath exciters both add DC, which would
        // otherwise keep going round, waste headroom and thump.
        const float hp = next - dcIn + dcPole * dcOut;
        dcIn = next;
        dcOut = hp;
        next = hp;
        if (next > 1.0f || next < -1.0f) next = std::tanh(next);
        buffer[static_cast<size_t>(write)] = next;
        energy += (std::fabs(out) - energy) * 0.001f;
        lastOut = out;
        if (++write >= static_cast<int32_t>(buffer.size())) write = 0;
        return out;
    }

    /**
     * The string heard at [fraction] of a turn back from the last output, as
     * a pickup or a finger hears it there. Reading only: the loop is untouched.
     */
    float tap(float fraction) const {
        return read(clampf(baseDelay * fraction, 1.0f, static_cast<float>(buffer.size() - 2)));
    }

    float level() const { return energy; }
    /** The last output sample, for a bow to push against. */
    float velocity() const { return lastOut; }

  private:
    static constexpr int kAllpass = 4;
    /** How much of the string's length full stiffness can add as delay. */
    static constexpr float kStiffBudget = 0.085f;
    /**
     * Where the loop's high-pass sits, as a fraction of the note.
     *
     * Below the note the loop gain is close to one, and a bow's added gain
     * can push it over and make a slow wobble far below the note. A corner at
     * a tenth of the note keeps the gain down there under one while barely
     * touching the fundamental.
     */
    static constexpr float kDcBelow = 0.1f;

    // How much extra delay each allpass adds at low frequencies, which sets
    // where the dispersion happens. It's tied to the string length, otherwise
    // the effect ends up near Nyquist where there are no partials.
    //
    // Full stiffness is about a twelfth of the string (kStiffBudget). Much
    // more and the note loses a clear pitch. At 1 it's still stiffer than any
    // real wire.
    void refreshDispersion() {
        if (dispersion < 0.005f || allpassStages == 0) {
            disperse = 0.0f;
            stageDelay = allpassStages > 0 ? 1.0f : 0.0f;
            return;
        }
        const float budget = baseDelay * kStiffBudget / static_cast<float>(allpassStages);
        stageDelay = clampf(1.0f + dispersion * budget, 1.0f, 96.0f);
        disperse = (1.0f - stageDelay) / (1.0f + stageDelay);
    }

    /**
     * How much of the note's period the rest of the loop already uses.
     *
     * Besides the delay line, the loop has the one-pole filter, the DC
     * blocker, the allpass chain and the damper tap. Their phase at the note
     * (not at DC) decides the pitch, so it's taken off the line length.
     *
     * The tap position depends on the delay being solved for, so it's
     * iterated three times, which settles to well under a cent.
     */
    void refreshLoop() {
        const float w = 6.28318530718f / baseDelay; // radians a sample, at the note
        const float sw = std::sin(w), cw = std::cos(w);
        // The loop filter, one pole at `tone`.
        float phase = -std::atan2((1.0f - tone) * sw, 1.0f - (1.0f - tone) * cw);
        // The DC blocker, which leads instead of lags, at a tenth of the note.
        dcPole = std::exp(-6.28318530718f * dcBelow / baseDelay);
        {
            const float nr = 1.0f - cw, ni = sw;
            const float dr = 1.0f - dcPole * cw, di = dcPole * sw;
            phase += std::atan2(ni, nr) - std::atan2(di, dr);
        }
        // The stiffness chain. Counted even with no stiffness, since at a
        // coefficient of zero an allpass is still a one-sample delay. The
        // formula gives exactly -w there.
        if (allpassStages > 0) {
            const float one = std::atan2(-sw, disperse + cw) -
                              std::atan2(-disperse * sw, 1.0f + disperse * cw);
            phase += one * static_cast<float>(allpassStages);
        }
        float d = baseDelay + phase / w;
        if (damperPressure > 0.0f) {
            const float k = damperPressure * 0.9f;
            for (int i = 0; i < 3; ++i) {
                // The tap is nearer the write head than the main read, so it
                // leads it.
                const float back = w * d * (1.0f - damperPos);
                const float re = 1.0f - k * std::cos(back), im = -k * std::sin(back);
                // A tap can lead or lag, so the result can be longer or
                // shorter than the period.
                d = clampf(baseDelay + (phase + std::atan2(im, re)) / w,
                           2.0f, baseDelay * 1.5f);
            }
        }
        loopTrim = clampf(baseDelay - d, -baseDelay * 0.5f, baseDelay - 2.0f);

        // How much the loop's filters reduce a partial at the note, which the
        // feedback makes up so `sustain` is accurate in seconds. The allpass
        // chain doesn't change the level, so it's left out.
        const float toneMag = tone / std::sqrt((1.0f - (1.0f - tone) * cw) * (1.0f - (1.0f - tone) * cw) +
                                               ((1.0f - tone) * sw) * ((1.0f - tone) * sw));
        const float dcMag = std::sqrt((1.0f - cw) * (1.0f - cw) + sw * sw) /
                            std::sqrt((1.0f - dcPole * cw) * (1.0f - dcPole * cw) +
                                      (dcPole * sw) * (dcPole * sw));
        // Linear interpolation of the fractional delay is also a lowpass. It
        // loses little per turn, but high notes make many turns a second, so
        // it's counted too.
        const float frac = d - std::floor(d);
        const float ir = 1.0f - frac + frac * cw, ii = -frac * sw;
        const float interp = std::sqrt(ir * ir + ii * ii);
        loopKeeps = clampf(toneMag * dcMag * interp, 0.05f, 1.0f);
        effGain = clampf(gain / loopKeeps, 0.0f, 1.05f);
    }

    /**
     * Reads the loop `delay` samples back, through `wrappedReadIndex` (see
     * there). A wrong wrap here would keep going round the loop.
     */
    float read(float delay) const {
        const auto len = static_cast<int32_t>(buffer.size());
        float frac = 0.0f;
        const int32_t i0 = dsp::wrappedReadIndex(write, delay, len, frac);
        const int32_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
        return buffer[static_cast<size_t>(i0)] * (1.0f - frac) + buffer[static_cast<size_t>(i1)] * frac;
    }

    std::vector<float> buffer;
    float sr = 48000.0f;
    int32_t write = 0;
    float baseDelay = 200.0f;
    float gain = 0.995f;
    float tone = 0.5f;
    float loopState = 0.0f;
    float disperse = 0.0f;
    float dispersion = 0.0f;
    float stageDelay = 0.0f;
    int allpassStages = 0;
    float allpassState[kAllpass] = {};
    float loopTrim = 0.0f; // what the rest of the loop already costs, in samples
    float dcIn = 0.0f, dcOut = 0.0f, dcPole = 0.999f;
    float dcBelow = kDcBelow;
    // How much one turn keeps of a partial at the note, and the feedback
    // that makes up for it to give the requested gain.
    float loopKeeps = 1.0f, effGain = 0.995f;
    float tension = 0.0f;
    float damperPos = 0.5f;
    float damperPressure = 0.0f;
    float energy = 0.0f;
    float lastOut = 0.0f;
    float pending = 0.0f;
};

} // namespace acidulous::machine
