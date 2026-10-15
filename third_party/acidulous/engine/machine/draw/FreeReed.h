#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// A free reed: a tongue of brass or steel that swings through a slot in its
// plate, blown by the pressure across it.
//
// The minimal model (Millot & Baumann 2007): the player's pressure feeds a
// chamber, the chamber a short channel, the channel the reed. Air escapes
// through the gap the reed leaves (its "useful section"), which is small
// while the reed is in its slot and opens on either side of it: that, with
// the chamber's give, is what keeps the reed going. The sound is the change
// in the flow through it. Nothing in it is told to sustain: a reed blown hard
// enough plays, starts slow, settles just below its own note, and chokes
// when blown much too hard, as real reeds do.
//
// The jet is turbulent: its pressure on the reed and the flow it radiates
// carry a noise as strong as the jet, so the noise pulses with each opening
// and the reed's motion is never quite the same twice.
//
// SI units throughout. The channel and jet are solved for the new flow each
// step (a quadratic), as they're stiff with the reed in its slot.
namespace acidulous::machine::draw {

/** A reed's make: what a maker chooses for an instrument, against a reed at 444 Hz. */
struct ReedMake {
    /** The tongue at 444 Hz, metres; it gets shorter as the square root of the period. */
    float length = 12.95e-3f, width = 2.1e-3f;
    /** Plate thickness and the side gap, metres. */
    float plate = 900e-6f, gap = 50e-6f;
    /** Stiffness at 444 Hz, N/m; it grows as the square root of the pitch. */
    float stiffness = 47.9f;
    /** The rest gap ("set"), as a share of how far the nominal pressure bends the reed. */
    float set = 1.32f;
    /** The pressure the set is made for, Pa. */
    float nominal = 700.0f;
    /** The reed's own loss, and the air's drag on it. */
    float q = 95.0f, drag = 3.0f;
    /** The cell's resonance as a share of the reed's, and the channel, metres and square metres. */
    float cell = 0.5f, channel = 10e-3f, channelArea = 30e-6f;
    /** How the jet contracts, and how much of the reed's area sweeps air. */
    float contraction = 0.6f, sweep = 0.4f;
    /** The jet's turbulence: its noise against the jet, in the pressure on the reed and in the flow. */
    float turbulence = 0.0f;
};

class FreeReed {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        clear();
    }

    void clear() {
        v = p1 = u = uWas = 0.0f;
        noiseLow = 0.0f;
        x = x0;
    }

    /** Where this reed's turbulence starts: each reed its own. */
    void seed(uint32_t s) { noise = s ? s : 1u; }

    /** Makes the reed for [hz] (the reed's own frequency, before any tuning). */
    void make(float hz, const ReedMake &k) {
        const float s = std::clamp(std::sqrt(444.0f / hz), 0.35f, 4.0f);
        const float L = k.length * s;
        area = L * k.width;
        const float K = k.stiffness * std::sqrt(hz / 444.0f);
        w0 = 6.2831853f * hz;
        const float mass = K / (w0 * w0);
        perMass = area / mass;
        loss = w0 / k.q;
        dragPerMass = k.drag * kRho * area / mass;
        const float rest = k.set * k.nominal * area / K;
        // A reed that's sounding keeps where it is; a new one starts at rest.
        if (std::fabs(x - x0) < 1e-9f) x = rest;
        x0 = rest;
        plate = k.plate;
        perimeter = k.width + 0.8f * L;
        leak = (2.0f * L + k.width) * k.gap;
        swept = area * k.sweep;
        alpha = k.contraction;
        turbulence = k.turbulence;
        // The turbulence's colour: noise low-passed at a few times the reed's frequency.
        noisePole = std::exp(-6.2831853f * std::fmin(8.0f * hz, 0.4f * sr) / sr);
        const float kH = 6.2831853f * k.cell * hz / kC;
        channelInertance = kRho * k.channel / k.channelArea;
        // The cell: its volume puts its resonance with the channel at [cell] of the reed.
        const float volume = k.channelArea / (k.channel * kH * kH);
        stiffnessOfAir = kRho * kC * kC / volume;
        // Two steps a sample above 1.5 kHz, where one would drift sharp.
        steps = hz > 1500.0f ? 2 : 1;
        dt = 1.0f / (sr * static_cast<float>(steps));
    }

    /**
     * One sample, blown at [pressure] Pa from a supply that gives [supply]
     * (m^3/s a Pa: how freely the player's breath or the bellows feed the
     * cell). Returns the change in flow, m^3/s a second.
     */
    float step(float pressure, float supply) {
        // This sample's turbulence: white, then low-passed.
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        const float white = static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
        noiseLow = white + noisePole * (noiseLow - white);
        const float stir = turbulence * noiseLow;
        for (int k = 0; k < steps; ++k) {
            const float above = std::fmax(0.0f, x), below = std::fmax(0.0f, -plate - x);
            const float section = leak + perimeter * (above + below);
            // The channel's inertance and the jet, for the new flow j past the reed's own sweep:
            // a j + b j|j| = p1 + a (u - sweep), solved without losing precision.
            const float a = channelInertance / dt;
            const float as = alpha * section;
            const float b = 0.5f * kRho / (as * as);
            // The reed moving toward its slot (v < 0) leaves room above it,
            // which the channel fills: the swept flow is -swept v.
            const float sweepFlow = -swept * v;
            const float rhs = p1 + a * (u - sweepFlow);
            const float j = rhs >= 0.0f ? 2.0f * rhs / (a + std::sqrt(a * a + 4.0f * b * rhs))
                                        : -2.0f * -rhs / (a + std::sqrt(a * a + 4.0f * b * -rhs));
            const float onReed = b * j * std::fabs(j) * (1.0f + kReedStir * stir);
            u = j + sweepFlow;
            // The cell fills from the supply and empties through the reed;
            // the filling is solved for the step, as a small cell fills faster than one.
            const float kd = stiffnessOfAir * dt;
            p1 = (p1 + kd * (pressure * supply - u)) / (1.0f + kd * supply);
            // The reed: pushed toward its slot by the pressure on it, held by
            // its stiffness, slowed by its own loss and the air's drag.
            const float acc = -loss * v - w0 * w0 * (x - x0) - onReed * perMass - dragPerMass * std::fabs(v) * v;
            v += acc * dt;
            x += v * dt;
        }
        // The radiated flow carries the jet's turbulence too.
        const float radiated = u + stir * jetFlow();
        const float change = (radiated - uWas) * sr;
        uWas = radiated;
        return change;
    }

    /** Where the tip is against its rest, metres, and how far it swings at most since asked; for tests. */
    float tip() const { return x; }
    float rest() const { return x0; }
    float flow() const { return u; }
    /** The jet's own flow, past the reed's sweep, m^3/s. */
    float jetFlow() const { return u + swept * v; }

  private:
    static constexpr float kRho = 1.2f, kC = 343.0f;
    /**
     * The turbulence pushes the reed this much harder than it's heard in the
     * flow: a played reed's cycles vary in length more than in size.
     */
    static constexpr float kReedStir = 4.0f;
    float sr = 48000.0f, dt = 1.0f / 48000.0f;
    int steps = 1;
    float area = 2.7e-5f, w0 = 2790.0f, perMass = 1.0f, loss = 29.0f, dragPerMass = 0.0f;
    float x0 = 5e-4f, plate = 9e-4f, perimeter = 1e-2f, leak = 1e-6f, swept = 1e-5f, alpha = 0.6f;
    float channelInertance = 400.0f, stiffnessOfAir = 1e9f;
    float x = 5e-4f, v = 0.0f, p1 = 0.0f, u = 0.0f, uWas = 0.0f;
    float turbulence = 0.0f, noisePole = 0.9f, noiseLow = 0.0f;
    uint32_t noise = 0x9e3779b9u;
};

} // namespace acidulous::machine::draw
