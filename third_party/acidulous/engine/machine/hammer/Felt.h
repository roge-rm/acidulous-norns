#pragma once
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/hammer/Course.h>

// A felt hammer: a mass thrown at the strings, pushed back by felt that gets
// stiffer the more it's squashed (force K·c^p, with p between 2 and 3) and
// gives back a little less than it took (hysteresis).
//
// Worked out every sample while it touches the strings, against their
// velocity where it strikes, so the strings' motion shapes the blow: a harder
// blow squashes the felt further into its stiff part, the contact gets
// shorter, and the note brighter. That is where a piano's brightness against
// velocity comes from, and it can't be drawn on. Out of contact it costs
// nothing.
//
// The felt is one piece and meets the course as one string: it feels their
// mean velocity and the impedance of all of them. The strings don't take the
// blow equally (heights, wear, tension), and that difference is what's left
// to ring as the aftersound once their motion together has gone into the
// bridge; the felt never feels it, or it would even it out.
namespace acidulous::machine::hammer {
using dsp::clampf;

class Felt {
  public:
    /** The hammer for a key: mass (kg), felt exponent and stiffness, one string's impedance (kg/s). */
    void set(float massKg, float exponent, float stiffness, float impedance, float hysteresis) {
        mass = massKg;
        p = exponent;
        K = stiffness;
        z = impedance;
        alpha = hysteresis;
    }

    /**
     * Throws the hammer at [speed] m/s at the [lanes] struck strings, each
     * taking [takes] of the blow (their mean is 1).
     */
    void strike(float speed, float sampleRate, const float *takes, int lanes) {
        dt = 1.0f / sampleRate;
        u = speed;
        y = 0.0f;
        ys = 0.0f;
        force = 0.0f;
        squashed = 0.0f;
        struck = lanes < 1 ? 1 : (lanes > Course::kLanes ? Course::kLanes : lanes);
        float sum = 0.0f;
        for (int i = 0; i < struck; ++i) sum += takes[i];
        for (int i = 0; i < Course::kLanes; ++i) take[i] = i < struck ? takes[i] * struck / sum : 0.0f;
        active = true;
        samples = 0;
        pushed = 0.0f;
        smooth = 1.0f - std::exp(-6.28318530718f * kSmoothHz / sampleRate);
    }

    bool touching() const { return active; }
    int contactSamples() const { return samples; }

    /** One sample against [course]: pushes into it and returns the force (N). */
    float step(Course &course) {
        if (!active) return 0.0f;
        const int lanes = struck < course.lanesStruck() ? struck : course.lanesStruck();
        float vin = 0.0f;
        for (int i = 0; i < lanes; ++i) vin += course.strikeVelocity(i);
        vin /= static_cast<float>(lanes);
        const float z2 = 2.0f * z * static_cast<float>(lanes);
        const float yNext = y + dt * u;
        const float base = yNext - (ys + dt * vin);
        // F = K c^p (1 + alpha·dc/dt), c = base - dt F / 2Z: one root, since
        // the right side only falls as F grows. g(F) = F - K c^p rate rises
        // with F (its slope is at least 1), from g(0) <= 0 to g > 0 once the
        // felt is pushed off the strings, so Newton with the slope worked out
        // in full, kept inside that bracket, takes three or four steps. (With
        // the hysteresis left out of the slope it crawled, and most samples
        // ended in forty halvings: a chord's first blocks cost several times
        // a whole block on a phone.)
        const float h = dt / z2;
        const float ap = alpha * p / dt;
        auto residual = [&](float f, float &slope) {
            const float c = base - h * f;
            if (c <= 0.0f) { slope = 1.0f; return f; }
            const float cp = std::pow(c, p);
            // K (c^p + alpha d(c^p)/dt) = K c^p (1 + alpha p (dc/dt) / c):
            // stiffer squashing in, softer letting go.
            const float rate = 1.0f + ap * (c - squashed) / c;
            if (rate <= 0.0f) { slope = 1.0f; return f; }
            slope = 1.0f + K * h * (p * cp / c * rate + cp * ap * squashed / (c * c));
            return f - K * cp * rate;
        };
        float F = 0.0f;
        if (base > 0.0f) {
            float lo = 0.0f, hi = base / h;
            F = clampf(force, lo, hi);
            for (int it = 0; it < 30; ++it) {
                float slope;
                const float g = residual(F, slope);
                if (g > 0.0f) hi = F; else lo = F;
                float next = F - g / slope;
                if (!(next > lo && next < hi)) next = 0.5f * (lo + hi);
                const bool close = std::fabs(next - F) <= 1e-6f * (1.0f + F);
                F = next;
                if (close || hi - lo <= 1e-6f * (1.0f + F)) break;
            }
        }
        if (!(F >= 0.0f) || F > 1e6f) F = 0.0f;
        force = F;
        const float dv = F / z2;
        ys += dt * (vin + dv);
        squashed = std::fmax(0.0f, yNext - ys);
        // The felt can't push faster than it gives: what the string gets is
        // the blow smoothed (the recordings' strikes have nothing like the
        // model's above 8 kHz, kinks where contact starts and ends).
        pushed += smooth * (dv - pushed);
        for (int i = 0; i < lanes; ++i) course.push(i, pushed * take[i]);
        u -= dt * F / mass;
        y = yNext;
        ++samples;
        // Done once it's let go and is on its way back.
        if ((F <= 0.0f && u < 0.0f) || samples > kMaxContact) active = false;
        return F;
    }

    /**
     * The felt stiffness that keeps a hammer of [massKg] on a rigid surface
     * for [contactSeconds] when thrown at 2 m/s: what the tables give, turned
     * into what the force law needs.
     */
    static float stiffnessFor(float massKg, float exponent, float contactSeconds) {
        // A power-law contact lasts as K^(-1/(p+1)): from a first guess (the
        // squash its energy reaches, crossed at about pi/2 of its speed),
        // each throw scales K by how far off it was. Four throws, where
        // halving took fifty: every instrument's keys are worked out as
        // Hammer loads.
        const float v = 2.0f, p1 = exponent + 1.0f;
        const float squash = contactSeconds * v / 3.14159265f;
        float K = 0.5f * p1 * massKg * v * v / std::pow(squash, p1);
        for (int it = 0; it < 4; ++it) K *= std::pow(contactOf(massKg, exponent, contactSeconds, K) / contactSeconds, p1);
        return K;
    }

  private:
    /** How long a hammer stays on a rigid surface with felt [k], thrown at 2 m/s, stepped finely for [contactSeconds]. */
    static float contactOf(float massKg, float exponent, float contactSeconds, float k) {
        const float h = contactSeconds / 400.0f;
        float y = 0.0f, u = 2.0f, t = 0.0f;
        while (t < contactSeconds * 20.0f) {
            const float F = y > 0.0f ? k * std::pow(y, exponent) : 0.0f;
            u -= h * F / massKg;
            y += h * u;
            t += h;
            if (y < 0.0f && u < 0.0f) break;
        }
        return t;
    }

    /** Longest a blow can last before it's ended anyway, in samples (a bounce that never leaves). */
    static constexpr int kMaxContact = 960;
    /** Where the felt smooths the push, Hz. */
    static constexpr float kSmoothHz = 6000.0f;
    float pushed = 0.0f, smooth = 1.0f;

    float mass = 8e-3f, p = 2.5f, K = 1e9f, z = 2.2f, alpha = 0.0f, dt = 1.0f / 48000.0f;
    float y = 0.0f, u = 0.0f, ys = 0.0f, force = 0.0f, squashed = 0.0f;
    float take[Course::kLanes] = {};
    int struck = 1;
    bool active = false;
    int samples = 0;
};

} // namespace acidulous::machine::hammer
