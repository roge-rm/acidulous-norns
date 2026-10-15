#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/machine/draw/FreeReed.h>

// One hole of a harmonica, and the player's mouth behind it.
//
// A hole is a channel of the comb with two reeds: the blow reed inside it,
// which blowing pushes into its slot, and the draw reed outside it, which
// drawing pulls into its own. Each is an opening reed for the other way of
// playing. Behind the lips is the player's mouth: the lungs push or pull
// through the constriction the tongue makes, into the front of the mouth,
// and through the lips into the channel. Where the tongue is sets the
// mouth's resonance, and that is what bends a note and lets the other reed
// speak in an overblow (Bahnson, Antaki & Beery 1998; Johnston 1987): it
// isn't told to happen.
//
//   lungs -[tongue: inertance, loss]- front of the mouth -[lips: inertance,
//   loss]- channel (the cell) -[each reed's slot: a jet]- outside
//
// SI units. The channel's pressure is solved with the jets for each step (a
// quadratic, as the jets are stiff when the reeds are in their slots).
namespace acidulous::machine::draw {

/** A Richter harp in C, hole by hole: the blow and draw notes, semitones from C4. */
inline constexpr int kHarpHoles = 10;
inline constexpr int kRichterBlow[kHarpHoles] = {0, 4, 7, 12, 16, 19, 24, 28, 31, 36};
inline constexpr int kRichterDraw[kHarpHoles] = {2, 7, 11, 14, 17, 21, 23, 26, 29, 33};
/** Harps in the keys they're sold in, G (the lowest) up to F#: semitones from a C harp. */
inline constexpr int kHarpKeys = 12;
inline constexpr int kHarpKeyShift[kHarpKeys] = {-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6};

/** Where the tongue sits for an unbent note: the mouth's resonance well above both reeds, Hz. */
inline float openMouth(float blowHz, float drawHz) { return std::fmax(3000.0f, 3.5f * std::fmax(blowHz, drawHz)); }

class HarpHole {
  public:
    /** The two reeds of a hole. */
    enum Which : int32_t { Blow = 0, Draw = 1 };

    void prepare(float sampleRate) {
        sr = sampleRate;
        dt = 1.0f / (sr * static_cast<float>(kSteps));
        steadyFollow = 1.0f - std::exp(-6.2831853f * kSteadyHz * dt);
        minPeriod = static_cast<int>(sr / 5000.0f);
        maxPeriod = static_cast<int>(sr / 25.0f);
        middleFollow = 1.0f - std::exp(-6.2831853f * 15.0f / sr);
        clear();
    }

    void clear() {
        for (Leaf &l : leaves) {
            l.v = 0.0f;
            l.x = l.x0;
        }
        tongueFlow = front = lipFlow = cell = outWas = 0.0f;
        tongueSteady = lipSteady = 0.0f;
        heardCount = 0;
        heardPeriod = 0.0f;
        heardWas = heardMiddle = 0.0f;
        noiseLow = 0.0f;
    }

    void seed(uint32_t s) { noise = s ? s : 1u; }

    /**
     * Valves, as a chromatic harp has: a flap over each reed's slot that the
     * other way of playing holds shut, so only the reed being played sounds.
     */
    void valves(bool on) { valved = on; }

    /** Makes the hole's reeds: the blow reed for [blowHz], the draw reed for [drawHz], both of [make]. */
    void make(float blowHz, float drawHz, const ReedMake &k) {
        makeLeaf(leaves[Blow], blowHz, k, +1.0f);
        makeLeaf(leaves[Draw], drawHz, k, -1.0f);
        turbulence = k.turbulence;
        noisePole = std::exp(-6.2831853f * std::fmin(8.0f * std::fmax(blowHz, drawHz), 0.4f * sr) / sr);
        // The channel: a comb's cell, and the lips' opening into it.
        cellStiffness = kRho * kC * kC / kCellVolume;
        lipInertance = kRho * kLipLength / kLipArea;
    }

    /**
     * Shapes the mouth: its front resonance at [hz] (where the tongue is), with
     * a loss that gives it [q]. The front of the mouth is smaller for a higher
     * resonance, as a player's is when the tongue rises toward the front.
     */
    void shapeMouth(float hz, float q) {
        hz = std::clamp(hz, 120.0f, 6000.0f);
        // Front cavity: 60 cm^3 at 300 Hz, up to 100 low down and down to 1.5
        // with the tongue right up behind the teeth.
        const float volume = std::clamp(60e-6f * std::pow(300.0f / hz, 1.2f), 1.5e-6f, 100e-6f);
        frontStiffness = kRho * kC * kC / volume;
        // The front resonates through both necks: w^2 = K (1/Lt + 1/Lh); solved for the tongue's.
        const float w = 6.2831853f * hz;
        const float perTongue = w * w / frontStiffness - 1.0f / lipInertance;
        tongueInertance = perTongue > 1e-6f ? 1.0f / perTongue : 1e6f;
        // Its loss: a resistance in the tongue's constriction, w L / Q.
        tongueLoss = w * tongueInertance / std::fmax(q, 0.5f);
        lipLoss = w * lipInertance / (4.0f * std::fmax(q, 0.5f));
    }

    /**
     * One sample with the lungs at [pressure] Pa: positive blows, negative
     * draws. Returns the change in flow out of the harp, m^3/s a second.
     */
    float step(float pressure) {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        const float white = static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
        noiseLow = white + noisePole * (noiseLow - white);
        const float stir = turbulence * noiseLow;
        float jets = 0.0f;
        // With valves, blowing holds the draw reed's flap shut and drawing the blow reed's.
        const int shut = valved ? (pressure >= 0.0f ? Draw : Blow) : -1;
        // The reed the breath pushes away from its slot.
        const int opener = pressure >= 0.0f ? Draw : Blow;
        for (int s = 0; s < kSteps; ++s) {
            // The tongue's constriction and the lips: each an inertance with a
            // loss. The loss is the mouth's damping, so it acts on the flow's
            // swing about its slow average and not on the steady breath.
            const float rt = dt * tongueLoss / tongueInertance, rl = dt * lipLoss / lipInertance;
            tongueFlow = (tongueFlow + dt / tongueInertance * (pressure - front) + rt * tongueSteady) / (1.0f + rt);
            tongueSteady += (tongueFlow - tongueSteady) * steadyFollow;
            front += frontStiffness * (tongueFlow - lipFlow) * dt;
            lipFlow = (lipFlow + dt / lipInertance * (front - cell) + rl * lipSteady) / (1.0f + rl);
            lipSteady += (lipFlow - lipSteady) * steadyFollow;
            // The cell: fed through the lips, emptied by both reeds' jets and
            // grown or shrunk by their swing. Solved for the new pressure with
            // the jets: p + c J(p) = rhs, J(p) = A sign(p) sqrt(2|p| / rho).
            float swell = 0.0f, opening = 0.0f;
            for (int r = 0; r < 2; ++r) {
                if (r == shut) continue;
                const Leaf &l = leaves[r];
                swell += -l.side * l.swept * l.v;
                opening += l.alpha * sectionOf(l) * (r == opener ? kOpenShare : 1.0f);
            }
            const float c = cellStiffness * dt;
            const float rhs = cell + c * (lipFlow - swell);
            const float beta = c * opening * std::sqrt(2.0f / kRho);
            // In s = sign(p) sqrt|p|: s|s| + beta s = rhs.
            const float s2 = rhs >= 0.0f ? 2.0f * rhs / (beta + std::sqrt(beta * beta + 4.0f * rhs))
                                         : -2.0f * -rhs / (beta + std::sqrt(beta * beta + 4.0f * -rhs));
            cell = s2 * std::fabs(s2);
            const float speed = (cell >= 0.0f ? 1.0f : -1.0f) * std::sqrt(2.0f * std::fabs(cell) / kRho);
            jets = 0.0f;
            // The reeds: the cell's pressure pushes the blow reed toward its
            // slot (side +1) and the draw reed away from its own (side -1).
            for (int r = 0; r < 2; ++r) {
                if (r == shut) continue;
                Leaf &l = leaves[r];
                const float jet = l.alpha * sectionOf(l) * (r == opener ? kOpenShare : 1.0f) * speed;
                jets += jet;
                const float push = -l.side * cell * (1.0f + kReedStir * stir);
                const float acc = -l.loss * l.v - l.w0 * l.w0 * (l.x - l.x0) + push * l.perMass - l.dragPerMass * std::fabs(l.v) * l.v;
                l.v += acc * dt;
                l.x += l.v * dt;
            }
        }
        // What the player hears: the played reed's period, from its upward
        // swings through where it sits on average (both reeds swing at the
        // note once it speaks). Its place is smoother than its speed.
        const float place = leaves[pressure >= 0.0f ? Blow : Draw].x;
        heardMiddle += (place - heardMiddle) * middleFollow;
        const float swing = place - heardMiddle;
        ++heardCount;
        if (heardWas < 0.0f && swing >= 0.0f && heardCount > minPeriod) {
            heardPeriod = heardPeriod > 0.0f ? heardPeriod + (static_cast<float>(heardCount) - heardPeriod) * 0.25f : static_cast<float>(heardCount);
            heardCount = 0;
        }
        if (heardCount > 4 * maxPeriod) heardPeriod = 0.0f;
        heardWas = swing;
        // The harp radiates the air its reeds let out, with the jets' turbulence.
        const float out = jets * (1.0f + kOutStir * stir);
        const float change = (out - outWas) * sr;
        outWas = out;
        return change;
    }

    /** For tests: a reed's tip against its rest, metres; the cell's pressure, Pa. */
    float tip(int which) const { return leaves[which].x - leaves[which].x0; }
    /** The pitch the hole is playing, Hz, as the player hears it; 0 while it isn't speaking. */
    float heard() const { return heardPeriod > 0.0f ? sr / heardPeriod : 0.0f; }
    float cellPressure() const { return cell; }

  private:
    static constexpr float kRho = 1.2f, kC = 343.0f;
    static constexpr int kSteps = 2;
    /** Below this the breath counts as steady, Hz: under the lowest reed. */
    static constexpr float kSteadyHz = 15.0f;
    static constexpr float kReedStir = 10.0f;
    /** How much of the turbulence is heard: the partner reed's steady leak carries it too. */
    static constexpr float kOutStir = 0.05f;
    /**
     * The share of a reed's sweep that reaches the channel. Two reeds share a
     * cell this small, and at the full sweep each pulls its partner's note a
     * semitone flat and chokes it; most of the swept air goes round the
     * reed's sides instead.
     */
    static constexpr float kSweepShare = 0.3f;
    /**
     * A reed pushed away from its slot opens less than its tip says: it bends
     * along its length and its sides stay in the slot's shadow.
     */
    static constexpr float kOpenShare = 0.1f;
    /** A comb's channel, and the lips' opening into it (a hole about 4 by 3 mm, 5 mm deep with the lips). */
    static constexpr float kCellVolume = 0.3e-6f, kLipArea = 12e-6f, kLipLength = 5e-3f;

    /** A reed: its swing, its make, and which side of its plate it sits (+1 in the channel, -1 outside). */
    struct Leaf {
        float x = 5e-4f, v = 0.0f, x0 = 5e-4f;
        float w0 = 2790.0f, loss = 29.0f, perMass = 1.0f, dragPerMass = 0.0f;
        float plate = 9e-4f, perimeter = 1e-2f, leak = 1e-6f, swept = 1e-5f, alpha = 0.6f;
        float side = 1.0f;
    };

    void makeLeaf(Leaf &l, float hz, const ReedMake &k, float side) {
        hz = std::clamp(hz, 27.5f, 4500.0f);
        const float s = std::clamp(std::sqrt(444.0f / hz), 0.35f, 4.0f);
        const float L = k.length * s;
        const float area = L * k.width;
        const float K = k.stiffness * std::sqrt(hz / 444.0f);
        l.w0 = 6.2831853f * hz;
        const float mass = K / (l.w0 * l.w0);
        l.perMass = area / mass;
        l.loss = l.w0 / k.q;
        l.dragPerMass = k.drag * kRho * area / mass;
        const float rest = k.set * k.nominal * area / K;
        if (std::fabs(l.x - l.x0) < 1e-9f) l.x = rest;
        l.x0 = rest;
        l.plate = k.plate;
        l.perimeter = k.width + 0.8f * L;
        l.leak = (2.0f * L + k.width) * k.gap;
        l.swept = area * k.sweep * kSweepShare;
        l.alpha = k.contraction;
        l.side = side;
    }

    /** The gap a reed leaves its jet: small in the slot, opening on either side of it. */
    static float sectionOf(const Leaf &l) {
        const float above = std::fmax(0.0f, l.x), below = std::fmax(0.0f, -l.plate - l.x);
        return l.leak + l.perimeter * (above + below);
    }

    Leaf leaves[2];
    float sr = 48000.0f, dt = 1.0f / 96000.0f;
    float tongueInertance = 200.0f, tongueLoss = 1e5f, frontStiffness = 3e9f, lipInertance = 500.0f, lipLoss = 1e5f;
    float cellStiffness = 5e11f;
    float tongueFlow = 0.0f, front = 0.0f, lipFlow = 0.0f, cell = 0.0f, outWas = 0.0f;
    float tongueSteady = 0.0f, lipSteady = 0.0f, steadyFollow = 0.001f;
    float turbulence = 0.0f, noisePole = 0.9f, noiseLow = 0.0f;
    bool valved = false;
    int heardCount = 0, minPeriod = 9, maxPeriod = 1920;
    float heardPeriod = 0.0f, heardWas = 0.0f, heardMiddle = 0.0f, middleFollow = 0.002f;
    uint32_t noise = 0x9e3779b9u;
};

} // namespace acidulous::machine::draw
