#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Math.h>
#include <vector>

// One woodwind: a mouthpiece, a row of tone holes and the rest of the tube
// below them.
//
// A reed is modelled as a reflection at the mouthpiece whose strength falls
// as the pressure across the reed rises. The tube gets back
// `mouth + difference x reflection`, and the slope of that against the
// returning wave goes above one when the player blows. (A crossfade, like
// Brazen's lip valve, can't oscillate as a reed.)
//
// A flute has no reed. A jet of air crosses the mouth hole, and the time it
// takes is the instrument's second delay.
//
// The first open hole reflects the low end and lets the high end pass into
// the tube below, which is also modelled. That gives the woodwind cutoff,
// makes the same pitch fingered two ways sound different, and lets a forked
// fingering sound two modes at once.
namespace acidulous::machine::timber {

/**
 * The loop gain target and how far a breath attack may lift past it. 1.9
 * keeps a held note in bounds. The attack lift has its own higher ceiling,
 * like the brass, because growth per round trip is fixed and low notes would
 * otherwise be slow to speak.
 */
constexpr float kWantSlope = 0.4f, kWantMax = 1.9f, kLiftCeiling = 2.6f;
/** What the loop settles at once the note is under way, for sizing the lift. */
constexpr float kSettled = 1.15f;
/**
 * Wall loss on the round trip of the bottom note. The loss per round trip
 * goes as the square root of the partial number and one over the square root
 * of the note, since higher notes use a shorter tube. So it's set as a depth
 * at the bottom note and scaled from there, not as a fixed frequency.
 */
constexpr float kWallDepth = 0.35f;
/**
 * How curved the reed's table is. A real reed barely moves at low pressure
 * and then closes hard, so the slope at the operating point is several times
 * the slope from rest. That gives loop gain without the reed resting nearly
 * shut. With a cube, a loop gain of 1.24 costs about a sixth of the aperture,
 * and the gain ceiling goes from 2 - offset to 4 - 3 x offset. The curve
 * reaches exactly one at the closing pressure, so it ends smoothly.
 */
constexpr float kReedCurve = 3.0f;

using dsp::clampf;

/**
 * One section of the tone hole lattice, as a two-pole lowpass. A row of open
 * holes is a cutoff: below it the wave reflects almost whole, above it the
 * wave carries on down the bore. A one-pole droops too early and starves the
 * top of the range.
 */
struct Section {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setLowpass(float hz, float sr, float q) {
        const float k = std::tan(3.14159265358979f * clampf(hz / sr, 1e-4f, 0.49f));
        const float kk = k * k;
        const float norm = 1.0f / (1.0f + k / q + kk);
        b0 = kk * norm;
        b1 = 2.0f * b0;
        b2 = b0;
        a1 = 2.0f * (kk - 1.0f) * norm;
        a2 = (1.0f - k / q + kk) * norm;
    }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    /** The response at [w] radians per sample. */
    void at(float w, float &re, float &im) const {
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
        const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const float dr = 1.0f + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        const float den = dr * dr + di * di + 1e-20f;
        re = (nr * dr + ni * di) / den;
        im = (ni * dr - nr * di) / den;
    }
    void clear() { z1 = z2 = 0.0f; }
};

class Pipe {
  public:
    enum Excite : int32_t { Single = 0, Double, Jet };

    void prepare(float sampleRate) {
        sr = sampleRate;
        upper.assign(static_cast<size_t>(sr / 22.0f) + 8, 0.0f);
        lower.assign(static_cast<size_t>(sr / 22.0f) + 8, 0.0f);
        jetLine.assign(1024, 0.0f);
        clear();
    }

    void clear() {
        for (auto &v : upper) v = 0.0f;
        for (auto &v : lower) v = 0.0f;
        for (auto &v : jetLine) v = 0.0f;
        wUpper = wLower = wJet = 0;
        hole1.clear(); hole2.clear();
        bellLp = inertia = breathLp = ventLp = wallLp = 0.0f;
        dcIn = dcOut = 0.0f;
        radiated = 0.0f;
        // Reset the tube length too. tune() rewrites it before step() reads
        // it, but a cleared pipe shouldn't hold the last note's length. It
        // isn't glided like the brass, since that detunes the jet line.
        upperDelay = 0.0f;
        onsetBoost = 1.0f;
        onsetFall = 0.0f;
        // Reset the pressure (setPressure ignores tiny changes, so an old
        // value could stick) and mark dirty so the next note retunes even if
        // it's the same note. Otherwise exports aren't repeatable.
        pressure = 0.5f;
        dirty = true;
    }

    // --- player and instrument settings --------------------------------------

    void setNote(float hz) {
        const float f = clampf(hz, 20.0f, 5000.0f);
        if (f != freq) { freq = f; dirty = true; }
    }

    /**
     * [cylindrical] closes the tube at the mouthpiece, so it's a quarter
     * wavelength long with only odd partials and overblows a twelfth, like a
     * clarinet. A cone is half a wavelength, has every partial and overblows
     * an octave.
     * [mode] is which partial the register vent holds the note on.
     */
    void setShape(bool cylindrical, int32_t mode) {
        const int32_t m = mode < 1 ? 1 : (mode > 3 ? 3 : mode);
        if (cylindrical != cylinder || m != regMode) { cylinder = cylindrical; regMode = m; dirty = true; }
    }

    /** The whole instrument, as the lowest note it can play. */
    void setTube(float lowestHz) {
        const float f = clampf(lowestHz, 20.0f, 4000.0f);
        if (f != lowest) { lowest = f; dirty = true; }
    }

    /**
     * The row of open holes. [hz] is where the lattice stops reflecting and
     * starts radiating. [fingering] is how forked the fingering is: closing
     * holes below the open one pulls the cutoff down and veils the note.
     */
    void setLattice(float hz, float fingering, float holes) {
        const float h = clampf(hz, 200.0f, 8000.0f);
        const float f = clampf(fingering, 0.0f, 1.0f);
        const float n = clampf(holes, 0.0f, 1.0f);
        if (h != latticeHz || f != finger || n != holeDepth) {
            latticeHz = h; finger = f; holeDepth = n; dirty = true;
        }
    }

    /** How much of the bore below the holes reflects back. */
    void setFork(float amount) {
        const float a = clampf(amount, 0.0f, 1.0f);
        if (a != fork) { fork = a; dirty = true; }
    }

    /**
     * The length of the bore below, relative to what the note implies. At 1
     * it's the rest of the instrument. Away from 1 it gives two resonances
     * sharing one reed, which makes multiphonics.
     */
    void setBelow(float scale) {
        const float v = clampf(scale, 0.2f, 4.0f);
        if (v != belowScale) { belowScale = v; dirty = true; }
    }

    /**
     * [kind] single reed, double reed or air jet. [stiffness] sets how fast
     * the reed can follow. [embouchure] is how hard the lip holds it, which
     * sets the mouthpiece's rest reflection.
     */
    void setReed(int32_t kind, float stiffness, float embouchure) {
        const int32_t k = kind < 0 ? 0 : (kind > 2 ? 2 : kind);
        const float s = clampf(stiffness, 0.05f, 1.0f);
        const float e = clampf(embouchure, 0.25f, 0.9f);
        if (k != excite || s != reedStiff || e != offset) {
            excite = k; reedStiff = s; offset = e; dirty = true;
        }
    }

    /**
     * The flute's air jet: how long it takes to cross, as a fraction of the
     * note's period, and how far off the edge it's aimed. Blowing harder
     * shortens the crossing time, which is how a flute overblows.
     */
    void setJet(float ratio, float aim) {
        const float l = clampf(ratio, 0.05f, 2.0f);
        const float o = clampf(aim, -0.9f, 0.9f);
        if (l != jetRatio || o != jetAim) { jetRatio = l; jetAim = o; dirty = true; }
    }

    void setBell(float reflection, float cutoff01) {
        const float r = clampf(reflection, 0.3f, 0.995f), c = clampf(cutoff01, 0.02f, 0.95f);
        if (r != bellGain || c != bellCoeff) { bellGain = r; bellCoeff = c; dirty = true; }
    }

    void setPressure(float p) {
        const float v = clampf(p, 0.0f, 2.0f);
        if (std::fabs(v - pressure) > 0.015f) { pressure = v; dirty = true; }
    }

    /** How hard the player blows past the point where the note speaks. */
    void setDrive(float d) {
        const float v = clampf(d, 0.0f, 2.0f);
        if (v != drive) { drive = v; dirty = true; }
    }

    void setLoss(float amount) { loss = clampf(amount, 0.8f, 1.0f); }

    /** The tongue on the reed. 1 holds it shut. */
    void setTongue(float amount) { tongue = clampf(amount, 0.0f, 1.0f); }

    // --- tuning --------------------------------------------------------------

    /**
     * Boost the loop gain for the note's first few round trips. Same as the
     * brass: to grow by A in T seconds at frequency f the loop needs
     * ln(A)/(fT) per round trip, so low notes speak as quickly as high ones.
     */
    void lift(float seconds = 0.08f) {
        if (!(freq > 0.0f)) return;
        // Not for the jet. Its gain solve is steep, so a lift makes the flute
        // louder, peakier and flat. The jet is also started by breath noise
        // rather than the loop alone, so it doesn't need a lift.
        if (excite == Jet) return;
        constexpr float kGrowth = 6.9f; // ln(1000): silence to a sounding note
        onsetBoost = clampf(std::exp(kGrowth / (freq * seconds)) / kSettled, 1.0f, 4.0f);
        onsetFall = std::exp(-64.0f / (seconds * sr));
        dirty = true;
    }

    /**
     * Set both tubes so the loop comes round in exactly one turn at the note,
     * and solve the mouthpiece for the gain it needs to speak. Like Brazen,
     * plus the bore below the holes feeding back into the junction, which
     * pulls the pitch like a real cross fingering. The junction is evaluated
     * as one complex number with both paths, and the upper tube takes the
     * leftover phase.
     */
    void tune() {
        if (onsetBoost > 1.001f) {
            dirty = true; // the loop is changing under us while the lift lasts
        } else {
            onsetBoost = 1.0f;
        }
        if (!dirty) return;
        dirty = false;

        const float period = sr / freq;
        const float w = 6.28318530718f * freq / sr;

        // The lattice. A forked fingering pulls the cutoff down, but never
        // below the note it has to reflect or the note wouldn't speak.
        const float cut = clampf(latticeHz * (1.0f - finger * 0.7f),
                                 std::max(120.0f, freq * 1.6f), sr * 0.45f);
        // Two poles, or four when `holes` is high, since more open holes cut
        // off harder. Flat below the corner either way.
        latticeCut = cut;
        holeOrder4 = holeDepth > 0.5f;
        if (holeOrder4) {
            hole1.setLowpass(cut, sr, 0.5412f);
            hole2.setLowpass(cut, sr, 1.3066f);
        } else {
            hole1.setLowpass(cut, sr, 0.70711f);
        }
        holeReflect = 0.98f - fork * 0.12f;
        throat = 0.2f + fork * 0.75f;

        // How much tube is left below the hole the note is fingered on,
        // worked out from the note and the size of the instrument.
        const float base = cylinder ? 0.5f : 1.0f;
        const float whole = base * (sr / lowest);
        // A vent puts the note on a higher partial of a longer tube. If the
        // instrument isn't long enough, drop to a lower register, like a
        // player would. Otherwise the note lands between modes and plays
        // badly out of tune.
        int32_t useMode = regMode;
        while (useMode > 1 && base * static_cast<float>(useMode) * period > whole * 1.02f) {
            useMode = cylinder ? (useMode > 3 ? 3 : 1) : useMode - 1;
        }
        sounding = useMode;
        const float nominal = base * static_cast<float>(useMode) * period;

        // The register vent, as a high pass on the loop. It stops the
        // partials below the one wanted, cornered between the tube's
        // fundamental and the note.
        //
        // With no vent open it still sits under the note, at the higher of
        // 0.3 x the lowest note and 0.25 x the note. Without it a cone (and
        // the long tube below the holes on high notes) rings a spurious low
        // mode that can take over the note.
        float ventHz = std::max(lowest * 0.3f, freq * 0.25f);
        if (useMode > 1) ventHz = std::max(ventHz, (freq / static_cast<float>(useMode)) * 1.5f);
        ventCoeff = 1.0f - std::exp(-6.28318530718f * ventHz / sr);
        float below = (whole - nominal) * (1.0f + finger * 0.4f) * belowScale;
        lowerDelay = clampf(below, 1.0f, static_cast<float>(lower.size() - 3));

        // The reed's inertia, as a plain one-pole. A resonant reed would be
        // strong enough to pull the note onto the wrong mode (as in Brazen).
        reedCoeff = clampf(1.0f - std::exp(-6.28318530718f * (400.0f + reedStiff * 3600.0f) / sr), 0.01f, 0.999f);

        float jr, ji, br, bi, fr, fi;
        junction(w, jr, ji);
        dcBlock(w, br, bi);
        {   // the vent rides with the DC blocker: both are losses in the loop
            float vr, vi;
            ventAt(w, vr, vi);
            const float nr2 = br * vr - bi * vi, ni2 = br * vi + bi * vr;
            br = nr2; bi = ni2;
        }
        // Wall loss. A real bore loses more at higher frequencies, which
        // keeps the fundamental ahead of its upper partials so low notes
        // don't jump to a higher mode. It's a shelf, not a lowpass, because
        // the tube's resonance multiplies any loss and a lowpass dulls the
        // tone a lot. See kWallDepth.
        const float tubeHz = freq / static_cast<float>(useMode);
        wallCoeff = 1.0f - std::exp(-6.28318530718f * clampf(tubeHz, 30.0f, sr * 0.45f) / sr);
        wallDepth = clampf(kWallDepth * std::sqrt(lowest / std::max(tubeHz, 1.0f)), 0.0f, 0.6f);
        {
            float wr, wi;
            wallAt(w, wr, wi);
            const float nr2 = br * wr - bi * wi, ni2 = br * wi + bi * wr;
            br = nr2; bi = ni2;
        }

        // Loop gain from everything outside the mouthpiece.
        const float outside = std::sqrt((br * br + bi * bi) * (jr * jr + ji * ji)) * loss;
        const float steady = clampf(0.9f + kWantSlope * pressure * drive, 0.0f, kWantMax);
        const float ceiling = onsetBoost > 1.001f ? kLiftCeiling : kWantMax;
        const float want = clampf(steady * onsetBoost, 0.0f, ceiling);
        const float t = want / (outside > 1e-6f ? outside : 1e-6f);
        const float mouth = clampf(pressure, 0.05f, 2.0f);

        if (excite == Jet) {
            // The jet takes time to cross, and blowing harder shortens it.
            // It opposes what it finds, so F = 1 - g e^-jw.tau is largest
            // when the crossing takes half a period, which puts the note on
            // the peak of the comb and the octave in a trough. Blowing
            // harder shortens the crossing and brings the octave up, which
            // is how a flute overblows. Solve for g.
            jetDelay = clampf(jetRatio * period / (0.75f + pressure * 0.35f),
                              1.0f, static_cast<float>(jetLine.size() - 3));
            const float ph = -w * jetDelay;
            const float cp = std::cos(ph), sp = std::sin(ph);
            const float disc = cp * cp - 1.0f + t * t;
            const float g = disc > 0.0f ? clampf(cp + std::sqrt(disc), 0.0f, 40.0f) : 1.0f;
            const float sech = 1.0f / std::cosh(jetAim * 2.0f);
            jetSlope = sech * sech;
            jetGain = clampf(g / (jetSlope > 1e-3f ? jetSlope : 1e-3f), 0.0f, 60.0f);
            jetRest = std::tanh(jetAim);
            fr = 1.0f - g * cp;
            fi = -g * sp;
        } else {
            // The reed table: r rises from the embouchure to fully shut as
            // the cube of the pressure across the reed, as a fraction of the
            // closing pressure. Blowing steadily puts it S of the way there,
            // and the loop sees
            //     F(w) = offset + S . (1 + 3 L(w)).
            // Solve |F| = t for S. It's a quadratic, and the positive root is
            // the one where the reed closes as the bore fills.
            float lr, li;
            onePole(reedCoeff, std::cos(w), std::sin(w), lr, li);
            const float ar = 1.0f + kReedCurve * lr, ai = kReedCurve * li;
            const float aa = ar * ar + ai * ai + 1e-20f;
            const float bq = offset * ar;
            const float cq = offset * offset - t * t;
            const float disc = bq * bq - aa * cq;
            float shut = disc >= 0.0f ? (-bq + std::sqrt(disc)) / aa : 1.0f;
            // Keep the reed below 90% of the way shut, so it has room to
            // move. That still leaves a gain ceiling of 4 - 3 x offset.
            shut = clampf(shut, 0.0f, 0.9f * (1.0f - offset));
            reedShut = shut;
            rRest = offset + shut;
            // This sets the closing pressure. The reed sits at
            // a = cbrt(S / (1 - offset)) of the way at pressure `mouth`, so
            // the closing pressure is mouth / a.
            const float aOp = std::cbrt(shut / (1.0f - offset));
            reedScale = aOp > 1e-4f ? aOp / mouth : 0.0f;
            fr = offset + shut * ar;
            fi = shut * ai;
        }

        float phase = std::atan2(fi, fr) + std::atan2(bi, br) + std::atan2(ji, jr);
        // The leftover phase goes in the tube, at the length nearest the
        // physical one, so the note sits on the mode the vent chose.
        float extra = phase / w - nominal;
        while (extra > period * 0.5f) extra -= period;
        while (extra <= -period * 0.5f) extra += period;
        upperDelay = clampf(nominal + extra, 4.0f, static_cast<float>(upper.size() - 3));
        onsetBoost = 1.0f + (onsetBoost - 1.0f) * onsetFall;
        loopMag = std::sqrt((fr * fr + fi * fi) * (br * br + bi * bi) * (jr * jr + ji * ji)) * loss;
    }

    /**
     * The loop gain at [hz] with the current settings. Partials over one can
     * sound and the strongest wins, so this is for checking which mode will
     * sound.
     */
    float loopAt(float hz) const {
        const float w = 6.28318530718f * clampf(hz, 1.0f, sr * 0.49f) / sr;
        float jr, ji, br, bi, fr, fi, vr, vi, wr, wi;
        junction(w, jr, ji);
        dcBlock(w, br, bi);
        ventAt(w, vr, vi);
        wallAt(w, wr, wi);
        mouthpiece(w, fr, fi);
        return std::sqrt((fr * fr + fi * fi) * (br * br + bi * bi) * (jr * jr + ji * ji) *
                         (vr * vr + vi * vi) * (wr * wr + wi * wi)) * loss;
    }

    /**
     * The loop phase at [hz], in turns. A partial can only sound where this
     * is a whole number. loopAt() decides which of those wins.
     */
    float loopTurns(float hz) const {
        const float w = 6.28318530718f * clampf(hz, 1.0f, sr * 0.49f) / sr;
        float jr, ji, br, bi, fr, fi;
        junction(w, jr, ji);
        dcBlock(w, br, bi);
        mouthpiece(w, fr, fi);
        float vr, vi, wr, wi;
        ventAt(w, vr, vi);
        wallAt(w, wr, wi);
        const float phase = std::atan2(fi, fr) + std::atan2(bi, br) + std::atan2(ji, jr) +
                            std::atan2(vi, vr) + std::atan2(wi, wr) - w * upperDelay;
        return phase / 6.28318530718f;
    }

    // --- one sample ----------------------------------------------------------

    /**
     * [mouth] is the player's pressure, [noise] the breath noise in it.
     * Returns the radiated sound from the holes and the bell together, since
     * most of a woodwind's sound leaves through the holes.
     */
    float step(float mouth, float noise) {
        const float arrive = read(upper, wUpper, upperDelay);

        // The lattice splits the wave: the low end reflects, the high end
        // carries on down the rest of the instrument.
        float low = hole1.process(arrive);
        if (holeOrder4) low = hole2.process(low);
        const float past = (arrive - low) * throat;
        const float fromHoles = arrive - 0.8f * low;

        // The rest of the instrument below the holes.
        const float belowArrive = read(lower, wLower, lowerDelay);
        bellLp += (belowArrive - bellLp) * bellCoeff;
        const float fromBell = belowArrive - 0.8f * bellLp;
        write(lower, wLower, past);

        // An open hole is a pressure node, so it inverts, which is why a
        // cylinder only has odd partials. A cone has every partial, so its
        // return isn't inverted.
        const float sign = cylinder ? 1.0f : -1.0f;
        const float bore = sign * (-low * holeReflect - bellLp * bellGain * throat);

        const float breath = mouth + noise;
        float in;
        if (excite == Jet) {
            // The jet is driven by the sound in the mouth hole, not the
            // steady breath, which would pin it at one end of its travel.
            // The note is started by breath noise and the change in breath
            // pressure, so a flute patch with no breath noise won't speak.
            breathLp += (breath - breathLp) * 0.002f;
            write(jetLine, wJet, bore + noise + (breath - breathLp) * 0.5f);
            const float late = read(jetLine, wJet, jetDelay);
            in = bore - (dsp::fastTanh(jetGain * late + jetAim) - jetRest);
        } else {
            // mouth + difference x reflection, where the reflection is the
            // reed table read at the (lagged, for the reed's inertia)
            // pressure across it. The only per-block value is reedScale.
            //
            // The table is a cube from the embouchure at no pressure to fully
            // shut at the closing pressure, and to fully open (inverted) the
            // same amount the other way, so it saturates smoothly at both
            // ends. See kReedCurve.
            //
            // The tongue holds the reed shut: the reflection goes to one and
            // the flow to nothing. Releasing it is the tongued attack that
            // starts the note.
            const float pd = bore - breath;
            inertia += ((breath - bore) - inertia) * reedCoeff;
            const float a = clampf(inertia * reedScale, -1.0f, 1.0f);
            const float cube = a * a * a;
            const float rFree = offset + (cube >= 0.0f ? 1.0f - offset : 1.0f + offset) * cube;
            const float r = rFree + (1.0f - rFree) * tongue;
            in = breath + pd * r;
        }

        in = dsp::fastTanh(in);
        float hp = in - dcIn + 0.997f * dcOut;
        dcIn = in;
        dcOut = hp;
        if (ventCoeff > 0.0f) {
            ventLp += (hp - ventLp) * ventCoeff;
            hp -= ventLp;
        }
        wallLp += (hp - wallLp) * wallCoeff;
        write(upper, wUpper, (hp - wallDepth * (hp - wallLp)) * loss);

        radiated = fromHoles * 0.7f + fromBell * 0.5f;
        return radiated;
    }

    float lastRadiated() const { return radiated; }
    float loopGain() const { return loopMag; }
    float upperLength() const { return upperDelay; }
    /** Which partial the vent actually put the note on. */
    int32_t soundingMode() const { return sounding; }
    float lowerLength() const { return lowerDelay; }

  private:
    static void onePole(float c, float cw, float sw, float &re, float &im) {
        // c / (1 - (1-c) z^-1) on the unit circle.
        const float dr = 1.0f - (1.0f - c) * cw, di = (1.0f - c) * sw;
        const float den = dr * dr + di * di + 1e-20f;
        re = c * dr / den;
        im = -c * di / den;
    }

    /** The whole tube as one complex number: the holes and everything
     *  below them, seen from the mouthpiece. */
    void junction(float w, float &re, float &im) const {
        const float c1 = std::cos(w), s1 = std::sin(w);
        float lr, li;
        hole1.at(w, lr, li);
        if (holeOrder4) {
            float r2, i2;
            hole2.at(w, r2, i2);
            const float ar = lr, ai = li;
            lr = ar * r2 - ai * i2;
            li = ar * i2 + ai * r2;
        }
        re = -lr * holeReflect;
        im = -li * holeReflect;
        // What went past the holes, down the tube, off the bell and back.
        float bre, bim;
        onePole(bellCoeff, c1, s1, bre, bim);
        const float tr = (1.0f - lr) * throat, ti = -li * throat;
        const float ph = -w * lowerDelay;
        const float er = std::cos(ph), ei = std::sin(ph);
        const float ar = tr * er - ti * ei, ai = tr * ei + ti * er;
        const float cr = ar * bre - ai * bim, ci = ar * bim + ai * bre;
        const float g = -bellGain * throat;
        re += g * cr;
        im += g * ci;
        const float sign = cylinder ? 1.0f : -1.0f;
        re *= sign;
        im *= sign;
        // The upper tube's delay is left out on purpose. tune() uses this
        // to find the phase left over for that tube, so including it would
        // count it twice.
    }

    /** The walls' loss, as the loop sees it: 1 - depth x (1 - onepole). */
    void wallAt(float w, float &re, float &im) const {
        float lr, li;
        onePole(wallCoeff, std::cos(w), std::sin(w), lr, li);
        re = 1.0f - wallDepth * (1.0f - lr);
        im = wallDepth * li;
    }

    /** The register vent: 1 - onepole, or 1 when it's shut. */
    void ventAt(float w, float &re, float &im) const {
        if (ventCoeff <= 0.0f) { re = 1.0f; im = 0.0f; return; }
        float lr, li;
        onePole(ventCoeff, std::cos(w), std::sin(w), lr, li);
        re = 1.0f - lr;
        im = -li;
    }

    void dcBlock(float w, float &re, float &im) const {
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float nr = 1.0f - c1, ni = s1;
        const float dr = 1.0f - 0.997f * c1, di = 0.997f * s1;
        const float den = dr * dr + di * di + 1e-20f;
        re = (nr * dr + ni * di) / den;
        im = (ni * dr - nr * di) / den;
    }

    void mouthpiece(float w, float &re, float &im) const {
        if (excite == Jet) {
            const float ph = -w * jetDelay;
            const float g = jetGain * jetSlope;
            re = 1.0f - g * std::cos(ph);
            im = -g * std::sin(ph);
        } else {
            float lr, li;
            onePole(reedCoeff, std::cos(w), std::sin(w), lr, li);
            re = offset + reedShut * (1.0f + kReedCurve * lr);
            im = reedShut * kReedCurve * li;
        }
    }

    static float read(const std::vector<float> &line, int32_t w, float delay) {
        const int32_t size = static_cast<int32_t>(line.size());
        const int32_t at = static_cast<int32_t>(delay);
        const float frac = delay - static_cast<float>(at);
        const int32_t i0 = (w - at + size + size) % size;
        const int32_t i1 = i0 == 0 ? size - 1 : i0 - 1;
        return line[static_cast<size_t>(i0)] +
               (line[static_cast<size_t>(i1)] - line[static_cast<size_t>(i0)]) * frac;
    }
    static void write(std::vector<float> &line, int32_t &w, float v) {
        line[static_cast<size_t>(w)] = v;
        w = (w + 1) % static_cast<int32_t>(line.size());
    }

    std::vector<float> upper, lower, jetLine;
    int32_t wUpper = 0, wLower = 0, wJet = 0;

    float sr = 48000.0f, freq = 220.0f, lowest = 146.8f;
    bool cylinder = true, holeOrder4 = false, dirty = true;
    int32_t regMode = 1, excite = Single;

    float upperDelay = 100.0f, lowerDelay = 1.0f;
    int32_t sounding = 1;
    float latticeHz = 1500.0f, finger = 0.0f, holeDepth = 0.4f, latticeCut = 1500.0f;
    Section hole1, hole2;
    float holeReflect = 0.98f, throat = 0.2f, fork = 0.0f, belowScale = 1.0f;
    float bellGain = 0.9f, bellCoeff = 0.4f;

    float reedStiff = 0.5f, offset = 0.7f, reedCoeff = 0.2f, inertia = 0.0f;
    // How far the steady breath has shut the reed, one unit of pressure as a
    // fraction of the closing pressure, and the resulting rest reflection.
    float reedShut = 0.1f, reedScale = 1.0f, rRest = 0.7f;
    float jetRatio = 0.5f, jetAim = 0.0f, jetDelay = 48.0f, jetGain = 1.0f, jetSlope = 1.0f, jetRest = 0.0f;

    float pressure = 0.5f, drive = 1.0f, loss = 0.999f, tongue = 0.0f;
    float bellLp = 0.0f, breathLp = 0.0f;
    float ventCoeff = 0.0f, ventLp = 0.0f;
    float wallCoeff = 1.0f, wallLp = 0.0f, wallDepth = kWallDepth;
    float dcIn = 0.0f, dcOut = 0.0f;
    float radiated = 0.0f, loopMag = 0.0f;
    /** The attack's lift and how fast it lets go. See lift(). */
    float onsetBoost = 1.0f, onsetFall = 0.0f;
};

} // namespace acidulous::machine::timber
