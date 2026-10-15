#pragma once
#include <cmath>
#include <cstdint>
#include <engine/dsp/Math.h>
#include <vector>

// One player's instrument: a tube, a bell and a pair of lips.
//
// A pressure wave goes round a loop. The lips are a valve, and the pressure
// behind them and inside the tube decide how far they open. That's the
// nonlinearity that makes notes lock to the tube's resonances and the tone
// get brighter as you blow harder.
//
// The pitch depends on everything in the loop (lips, bell, DC blocker), not
// just the line length, and models like this often play flat because of it.
// So tune() linearises the loop at the note and sets the line length so the
// total phase comes to exactly one period.
//
// Two additions to the textbook loop:
//
//   - Brassiness: a loud wave steepens as it travels, nearly into a shock
//     wave, which is why a loud trombone sounds so different from a quiet
//     one. The steepening here grows with the wave's amplitude.
//   - The output is the part the bell doesn't reflect, so the highs come out
//     and the lows stay inside.
namespace acidulous::machine::brazen {

using dsp::clampf;

/**
 * The DC blocker's pole, a corner at 22.9 Hz.
 *
 * This takes some of a tuba's fundamental (moving it to 3.8 Hz gives back
 * about 18% at F2), but it also shapes the timbre of every patch. Moving it
 * makes all the instruments bright and alike, so changing it means
 * re-voicing all the patches.
 *
 * Used twice: as the filter and in `tune`, where its phase is part of the
 * loop. Changing one without the other detunes the instrument.
 */
constexpr float kDcPole = 0.997f;

/** How far over the steady ceiling a tongue may lean. See tune(). */
constexpr float kLiftCeiling = 3.0f;

/**
 * The corner of the breath noise lowpass, as a one-pole coefficient at
 * 48 kHz.
 *
 * Breath noise is broadband and tilted, not white. Lower corners cut the
 * harsh onset noise more, but below about 2 kHz the patches that use
 * `breath` as part of their tone (harmon, piccolo, straight mute) lose it.
 */
constexpr float kAirPole = 0.2298f; // 1 - exp(-2 pi 2000 / 48000)

constexpr float kRiseP = 3.0f, kFallP = 12.0f;

class Bore {
  public:
    void prepare(float sampleRate) {
        sr = sampleRate;
        delayGlide = dsp::onePoleCoeff(0.006f, sampleRate);
        // Long enough for a tuba's pedal note, plus room for the read to wrap.
        line.assign(static_cast<size_t>(sampleRate / 18.0f) + 8, 0.0f);
        clear();
    }

    /**
     * Clears all of the tube's state, not just some of it.
     *
     * Anything left over (like `lastArrive`, which bends the next read, or
     * `delay`) carries into the next note and makes renders after a panic
     * differ. The machine sets every parameter each block, so everything can
     * be reset, and `dirty` makes the loop get solved again.
     */
    void clear() {
        for (auto &v : line) v = 0.0f;
        write = 0;
        bellState = 0.0f;
        lipX1 = lipX2 = lipY1 = lipY2 = 0.0f;
        lipEnv = 0.0f;
        lipRise = lipFall = 0.0006f;
        dcIn = dcOut = 0.0f;
        radiated = 0.0f;
        lastArrive = 0.0f;
        delayTarget = 0.0f;
        noiseLp = 0.0f;
        loopMag = 0.0f;
        delay = 0.0f;
        onsetBoost = 1.0f;
        onsetFall = 0.0f;
        // Reset the pressure too. setPressure ignores changes under 0.02, so
        // an old pressure could stick and the tuning would depend on what
        // the voice played before.
        pressure = 0.5f;
        dirty = true;
    }

    /** The note. tune() works out the line length for it. */
    void setFrequency(float hz) {
        const float f = clampf(hz, 20.0f, 4000.0f);
        if (f != freq) { freq = f; dirty = true; }
    }

    /**
     * [tension] scales the lips' buzz frequency against the note. At 1 they
     * buzz at the note, below it the tone gets darker and above it brighter.
     * The loop is retuned for it, so it changes the tone and not the pitch.
     */
    void setLips(float tension, float damping) {
        const float t = clampf(tension, 0.25f, 3.0f), d = clampf(damping, 0.0f, 1.0f);
        if (t != lipTension || d != lipDamp) { lipTension = t; lipDamp = d; dirty = true; }
    }

    /** How loud the lips ring back into the valve. */
    void setLipGain(float g) {
        const float v = clampf(g, 0.0f, 4.0f);
        if (v != lipGain) { lipGain = v; dirty = true; }
    }

    /** How hard the player is blowing. The tuning depends on it. */
    void setPressure(float p) {
        const float v = clampf(p, 0.0f, 2.0f);
        if (std::fabs(v - pressure) > 0.02f) { pressure = v; dirty = true; }
    }

    /** Bell: how much is reflected and how dull the reflection is. */
    void setBell(float reflection, float cutoff01) {
        const float rf = clampf(reflection, 0.5f, 0.995f), c = clampf(cutoff01, 0.02f, 0.98f);
        if (rf != reflect || c != bellCoeff) { reflect = rf; bellCoeff = c; dirty = true; }
    }
    void setBrass(float amount) { brass = clampf(amount, 0.0f, 1.0f); }
    /** How far the lips close as the note builds. */
    void setBite(float amount) { bite = clampf(amount, 0.0f, 1.6f); }
    /** How far open the lips are at rest. */
    void setRest(float amount) {
        const float v = clampf(amount, 0.0f, 0.9f);
        if (v != rest) { rest = v; dirty = true; }
    }
    void setLoss(float amount) { loss = clampf(amount, 0.8f, 1.0f); }

    /**
     * The tongue: push harder on the note for its first few round trips.
     *
     * The loop grows by the same factor every round trip, and a round trip
     * is one period, so without this low notes take much longer to speak
     * (about 16 times longer at 44 Hz than at 700 Hz). Like tonguing, the
     * loop gain is lifted at the start and relaxes over a fixed time, so the
     * onset takes about the same time at every pitch.
     *
     * To grow by a factor A in T seconds at frequency f the loop needs
     * ln(A)/(f T) per round trip, and the boost is that divided by what the
     * loop already has. It's 1 above a few hundred hertz and climbs for
     * longer tubes. Nothing is injected into the tube, it still starts empty.
     *
     * Starting with the lips already tensioned seems like an alternative but
     * sounds worse: the note takes longer to build and the onset gets
     * noisier.
     */
    void tongue(float seconds = 0.13f) {
        if (!(freq > 0.0f)) return;
        constexpr float kGrowth = 6.9f; // ln(1000): silence to a sounding note
        // Sized against the loop gain once the note is going, not the current
        // one. At note-on the envelope is still at zero so the loop gain
        // would be too low. `tune` settles at 1.23 at every pitch.
        constexpr float kSettled = 1.23f;
        const float want = std::exp(kGrowth / (freq * seconds));
        onsetBoost = clampf(want / kSettled, 1.0f, 4.0f);
        // Relaxed over the same time it was sized for, per block since that's
        // how often the loop is solved.
        onsetFall = std::exp(-64.0f / (seconds * sr));
        dirty = true;
    }

    /**
     * Works out the line length so the loop takes exactly one period of the
     * note.
     *
     * The valve is linearised where it sits, giving a complex gain G at the
     * note. The bell filter and DC blocker give two more. Their phases add
     * up to part of the loop, and the line makes up the rest. Only runs when
     * something changed, never per sample.
     */
    void tune() {
        if (onsetBoost > 1.001f) {
            dirty = true; // the loop is changing under us while the lift lasts
        } else {
            onsetBoost = 1.0f;
        }
        if (!dirty) return;
        dirty = false;

        lipHz = clampf(freq * lipTension, 20.0f, sr * 0.45f);
        // The lip resonance is set by Q, not pole radius. A fixed radius makes
        // low notes' filter very wide, so a higher mode wins and a tuba plays
        // the wrong note.
        const float q = 0.7f + (1.0f - lipDamp) * 12.0f;
        const float lw = 6.28318530718f * lipHz / sr;
        // But no narrower than 15 Hz. Otherwise a tuba's lip filter is only
        // a few hertz wide and it comes out as a sine wave.
        float bw = lw / (2.0f * q);
        const float floorBw = 6.28318530718f * 15.0f / sr;
        if (bw < floorBw) bw = floorBw;
        const float r = clampf(1.0f - bw, 0.3f, 0.9995f);
        lipA1 = 2.0f * r * std::cos(lw);
        lipA2 = -r * r;
        // Zeros at DC and Nyquist so the lips only respond to changes in
        // pressure. Otherwise the steady pressure pushes them wide open and
        // the note dies.
        //
        // Negative because lips are blown open, unlike a reed which is blown
        // shut. With a reed's sign this model can't play.
        lipB0 = -1.0f;
        {   // Normalise, so lipGain is the gain at the lips' own frequency.
            const float c1 = std::cos(lw), s1 = std::sin(lw);
            const float c2 = std::cos(2.0f * lw), s2 = std::sin(2.0f * lw);
            const float nr = 1.0f - c2, ni = s2;
            const float dr = 1.0f - lipA1 * c1 - lipA2 * c2, di = lipA1 * s1 + lipA2 * s2;
            const float m = std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di + 1e-20f));
            lipB0 = -1.0f / (m > 1e-6f ? m : 1e-6f);
        }

        const float w = 6.28318530718f * freq / sr;
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);

        // The lips: b0(1 - z^-2) over 1 - a1 z^-1 - a2 z^-2.
        float nr = lipB0 * (1.0f - c2), ni = lipB0 * s2;
        float dr = 1.0f - lipA1 * c1 - lipA2 * c2, di = lipA1 * s1 + lipA2 * s2;
        float den = dr * dr + di * di + 1e-20f;
        const float hr = (nr * dr + ni * di) / den, hi = (ni * dr - nr * di) / den;

        // The DC blocker: (1 - z^-1) / (1 - 0.997 z^-1).
        nr = 1.0f - c1; ni = s1;
        dr = 1.0f - kDcPole * c1; di = kDcPole * s1;
        den = dr * dr + di * di + 1e-20f;
        const float br = (nr * dr + ni * di) / den, bi = (ni * dr - nr * di) / den;

        // The bell's reflection: c / (1 - (1-c) z^-1).
        nr = bellCoeff; ni = 0.0f;
        dr = 1.0f - (1.0f - bellCoeff) * c1; di = (1.0f - bellCoeff) * s1;
        den = dr * dr + di * di + 1e-20f;
        const float lr = (nr * dr + ni * di) / den, li = (ni * dr - nr * di) / den;

        // The embouchure. Pick the loop gain the note should have (just over
        // one, more when blowing harder) and solve for the lip drive that
        // gives it. The other settings can then change the tone without
        // stopping the note from sounding, at any pitch.
        const float open0 = clampf(rest, 0.0f, 1.0f);
        const float a0 = 1.0f - open0 * open0;
        const float k = 2.0f * open0 * clampf(pressure, 0.0f, 2.0f) + 1e-6f;
        const float outside = std::sqrt((br * br + bi * bi) * (lr * lr + li * li)) * reflect * loss;
        // At note start the target is lifted by the tongue boost (see
        // `tongue`). The boost goes on the target, not on the solved drive,
        // since G = a0 - k u H isn't linear in u.
        // During the lift the ceiling is kLiftCeiling instead of 1.9, or low
        // notes wouldn't get the boost they need.
        const float steady = 0.86f + 0.62f * pressure * lipGain;
        const float ceiling = onsetBoost > 1.001f ? kLiftCeiling : 1.9f;
        const float want = clampf(steady * onsetBoost, 0.0f, ceiling);
        const float t = want / (outside > 1e-6f ? outside : 1e-6f);

        // G = a0 - k u H. Solve |G| = t for a lip drive u >= 0.
        const float hh = hr * hr + hi * hi + 1e-20f;
        const float disc = a0 * a0 * hr * hr - hh * (a0 * a0 - t * t);
        float u = 0.0f;
        if (disc >= 0.0f) {
            const float root = (a0 * hr + std::sqrt(disc)) / (k * hh);
            if (root > 0.0f) u = root;
        }
        if (u > 40.0f) u = 40.0f;
        lipB0 *= u;
        const float gr = a0 - k * u * hr, gi = -k * u * hi;

        float phase = std::atan2(gi, gr) + std::atan2(bi, br) + std::atan2(li, lr);
        // Into (-pi, pi], so the line lands within half a period of nominal.
        const float twoPi = 6.28318530718f;
        while (phase > 3.14159265359f) phase -= twoPi;
        while (phase <= -3.14159265359f) phase += twoPi;
        const float period = sr / freq;
        // How quickly the lips find their working tension, in cycles of the
        // note so it behaves the same at every pitch. Fast rise and slow fall,
        // like real lips, so it keeps up with a growing note. A slow rise
        // leaves the valve too far open at the onset and gives a burst of
        // noise on low notes.
        lipRise = 1.0f - std::exp(-1.0f / (kRiseP * period));
        lipFall = 1.0f - std::exp(-1.0f / (kFallP * period));
        delayTarget = clampf(period + phase / w, 4.0f, static_cast<float>(line.size() - 3));
        // On a new note there's nothing to glide from, so jump straight there.
        if (delay < 4.0f) delay = delayTarget;
        onsetBoost = 1.0f + (onsetBoost - 1.0f) * onsetFall;
        loopMag = std::sqrt((gr * gr + gi * gi) * (br * br + bi * bi) * (lr * lr + li * li)) * reflect * loss;
    }

    /**
     * One sample. [mouth] is the pressure behind the lips (0 is silence) and
     * [noise] the breath noise. Returns what comes out of the bell.
     */
    float step(float mouth, float noise) {
        const int32_t size = static_cast<int32_t>(line.size());
        // Steepening. In a loud wave the crest travels faster than the
        // trough, so the delay depends on the wave: shorter where it's loud.
        // Bending the read this way adds harmonics without changing the loop
        // gain, so the horn gets brighter and stays in tune. It's scaled as a
        // fraction of the line length so it works the same at every pitch.
        //
        // The line length is solved once a block and moves during the attack.
        // It's glided here so it doesn't jump each block, which would buzz.
        delay += (delayTarget - delay) * delayGlide;
        float read = delay - brass * delay * 0.06f * lastArrive;
        if (read < 4.0f) read = 4.0f;
        else if (read > static_cast<float>(size - 3)) read = static_cast<float>(size - 3);
        const int32_t at = static_cast<int32_t>(read);
        const float frac = read - static_cast<float>(at);
        const size_t i0 = static_cast<size_t>((write - at + size) % size);
        const size_t i1 = i0 == 0 ? line.size() - 1 : i0 - 1;
        const float arrive = line[i0] + (line[i1] - line[i0]) * frac;

        // The bell reflects the lows back down the tube and lets the highs
        // out.
        bellState += (arrive - bellState) * bellCoeff;
        // The output is what the bell doesn't reflect, but not all of the
        // reflected part is removed, or the fundamental would be missing.
        radiated = arrive - 0.8f * bellState;
        const float bore = bellState * reflect;
        lastArrive = arrive;

        // The pressure across the lips: the player behind, the tube in front.
        // Breath noise is lowpassed first since real turbulence rolls off at
        // the top. White noise would come out of the bell as a harsh hiss at
        // the start of each note.
        noiseLp += (noise - noiseLp) * kAirPole;
        const float breath = mouth + noiseLp;
        const float delta = breath - bore;

        const float lip = lipB0 * (delta - lipX2) + lipA1 * lipY1 + lipA2 * lipY2;
        lipX2 = lipX1;
        lipX1 = delta;
        lipY2 = lipY1;
        lipY1 = lip;

        // How far the valve is open, squared since the gap closes from both
        // sides. Open, the tube gets the player's breath. Shut, it gets its
        // own reflection back.
        // As the note builds, the lips settle closed and only open on the
        // pressure peaks, so the valve goes from a sine when quiet to a
        // narrow pulse when loud. The follower is scaled by 2/pi because the
        // rest point was voiced against the mean of the drive, and a
        // fast-rise follower settles at the peak.
        const float working = std::fabs(lip) * 0.63661977f;
        lipEnv += (working - lipEnv) * (working > lipEnv ? lipRise : lipFall);
        float open = rest - bite * lipEnv + lip;
        if (open < 0.0f) open = 0.0f;
        else if (open > 1.0f) open = 1.0f;
        const float opening = open * open;
        float in = opening * breath + (1.0f - opening) * bore;

        in = dsp::fastTanh(in);

        // A DC blocker, or the loop fills up with the steady breath pressure.
        const float hp = in - dcIn + kDcPole * dcOut;
        dcIn = in;
        dcOut = hp;

        line[static_cast<size_t>(write)] = hp * loss;
        write = (write + 1) % size;
        return radiated;
    }

    float lastRadiated() const { return radiated; }
    float lastInside() const { return lastArrive; }
    /** |loop| at the note: over one and it sounds, under and it dies. */
    float loopGain() const { return loopMag; }
    float lineDelay() const { return delay; }

  private:
    std::vector<float> line;
    int32_t write = 0;
    float sr = 48000.0f, freq = 220.0f, delay = 434.0f, delayTarget = 434.0f;
    /** How fast the read follows the solved length. See step(). */
    float delayGlide = 1.0f;
    float lipHz = 220.0f, lipGain = 1.0f, lipTension = 1.0f, lipDamp = 0.4f, pressure = 0.5f;
    bool dirty = true;
    float lipA1 = 0.0f, lipA2 = 0.0f, lipB0 = 0.0f;
    float onsetBoost = 1.0f, onsetFall = 0.0f;
    float lipX1 = 0.0f, lipX2 = 0.0f, lipY1 = 0.0f, lipY2 = 0.0f;
    float reflect = 0.9f, bellCoeff = 0.3f, bellState = 0.0f;
    float brass = 0.3f, loss = 0.995f, rest = 0.35f, bite = 0.0f, lipEnv = 0.0f;
    /** Rise and fall rates of the lip follower. See tune(). */
    float lipRise = 0.0006f, lipFall = 0.0006f;
    float dcIn = 0.0f, dcOut = 0.0f, radiated = 0.0f, loopMag = 0.0f, lastArrive = 0.0f;
    /** The lowpassed breath noise. See kAirPole. */
    float noiseLp = 0.0f;
};

} // namespace acidulous::machine::brazen
