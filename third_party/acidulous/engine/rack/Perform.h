#pragma once

#include <engine/core/Constants.h>
#include <engine/core/Params.h>
#include <engine/dsp/Biquad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace acidulous {

/**
 * The performance effects you hold rather than set: beat repeat, tape stop,
 * and an XY pad with a filter across and a delay throw up.
 *
 * They run on the whole mix after the master inserts and before the master
 * fader and limiter, in that order, so the tape stops what's repeated and
 * the echo gets what the filter shaped.
 *
 * The held controls are normal parameters. Repeat, stop and the pad's two
 * axes arrive as ParamMessages on Unit::Perform, so pressing one while
 * recording writes a lane like turning a knob does, and a lane plays it
 * back. The rest (stop length, echo time and feedback) are song settings in
 * the same table.
 *
 * When nothing is held the mix passes through bit for bit. Every stage has
 * a resting state that returns its input unchanged and the echo sleeps once
 * its tail is silent, so exports of songs that don't use this never change.
 */
class Perform {
  public:
    enum P : int32_t {
        /** 0 off, 1..5 a slice of 1, 1/2, 1/4, 1/8 or 1/16 of a beat. */
        Repeat,
        /** Held: the tape slows to a stop. Release and it spins back up. */
        Stop,
        /** The pad's filter: low pass left of centre, high pass right. */
        X,
        /** The pad's throw: how much of the mix goes into the echo. */
        Y,
        /** How long the tape takes to stop: 1/4, 1/2, 1 or 2 beats. */
        StopLen,
        /** The echo's time: 1/16, 1/8, dotted 1/8, 1/4 or dotted 1/4. */
        ThrowTime,
        /** The echo's feedback. */
        Feedback,
        /** Held: the last beat, backwards, looped in time. */
        Reverse,
        /** 0 off, 1..5 chops at 1/8, 1/16, 1/32, 1/8 triplets or 1/16 triplets. */
        Gate,
        /** Held: cuts the lows, mids or highs. */
        KillLow, KillMid, KillHigh,
        /** Held: a rising high pass with noise rising under it, over [RiserLen]. */
        Riser,
        /** How long the riser takes to reach the top: 1, 2 or 4 bars. */
        RiserLen,
        /** What the pad's X axis does: 0 filter, 1 crush. */
        XMode,
        /** What the pad's Y axis does: 0 echo, 1 wash. */
        YMode,
        /** Where they run: 0 the whole mix, 1..4 one of the mixer's groups. */
        Target,
        Count
    };

    /** Each buffer's length, about 2.7 s at 48 kHz. Every time here fits within it. */
    static constexpr int32_t kSize = 1 << 17;
    static constexpr int32_t kMask = kSize - 1;

    Perform() {
        static const ParamDef kDefs[Count] = {
            {"repeat", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
            {"stop", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"x", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
            {"y", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
            {"stoplen", 0.0f, 3.0f, 2.0f, Curve::Stepped, 4, ""},
            {"throwtime", 0.0f, 4.0f, 2.0f, Curve::Stepped, 5, ""},
            {"feedback", 0.0f, 0.9f, 0.55f, Curve::Linear, 0, ""},
            {"reverse", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"gate", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
            {"killlow", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"killmid", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"killhigh", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"riser", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"riserlen", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
            {"xmode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"ymode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
            {"target", 0.0f, 4.0f, 0.0f, Curve::Stepped, 5, ""},
        };
        params_.init(kDefs, Count);
        // Also allocated here so an engine that's never started (every host
        // harness) has buffers to write to.
        prepare(static_cast<float>(kSampleRate));
    }

    /** Not on the audio thread: this allocates the buffers. */
    void prepare(float sampleRate) {
        sr = sampleRate;
        ring[0].assign(kSize, 0.0f);
        ring[1].assign(kSize, 0.0f);
        slice[0].assign(kSize, 0.0f);
        slice[1].assign(kSize, 0.0f);
        rslice[0].assign(kSize, 0.0f);
        rslice[1].assign(kSize, 0.0f);
        echo[0].assign(kSize, 0.0f);
        echo[1].assign(kSize, 0.0f);
        smoothA = 1.0f - std::exp(-1.0f / (0.005f * sr));
        edgeFrames = std::max(1, static_cast<int32_t>(0.0015f * sr));
        mixStep = 1.0f / std::max(1.0f, 0.005f * sr);
        gateStep = 1.0f / std::max(1.0f, 0.002f * sr);
        riserStep = 1.0f / std::max(1.0f, 0.010f * sr);
        envRelease = std::exp(-1.0f / (0.04f * sr));
        // The wash's diffusers: all-passes at prime lengths covering about one
        // lap of the wash, different on each side so the two sides smear apart.
        const int32_t lens[2][4] = {{557, 1123, 1601, 2203}, {613, 1051, 1709, 2141}};
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 4; ++k) {
                diffuse[c][k].len = std::clamp(static_cast<int32_t>(lens[c][k] * sr / 48000.0f), 1, Diffuser::kMax - 1);
            }
        }
        // The crossovers: Linkwitz-Riley (two Butterworth sections each) so the
        // three bands sum back flat. The low band also goes through the upper
        // crossover's all-pass to stay in phase with the other two.
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 2; ++k) {
                xo[c].lo[k].lowpass(250.0f, 0.70710678f, sr);
                xo[c].rest[k].highpass(250.0f, 0.70710678f, sr);
                xo[c].mid[k].lowpass(2500.0f, 0.70710678f, sr);
                xo[c].high[k].highpass(2500.0f, 0.70710678f, sr);
                xo[c].apLo[k].lowpass(2500.0f, 0.70710678f, sr);
                xo[c].apHi[k].highpass(2500.0f, 0.70710678f, sr);
            }
        }
        rejoinStep = 1.0f / std::max(1.0f, 0.010f * sr);
        params_.jumpAll();
        reset();
    }

    ParamSet &params() { return params_; }

    /**
     * Back to a known state. The held controls are reset by [release].
     *
     * Only the echo is cleared. This runs on the audio thread at a panic, and
     * the other two buffers are always written before they're read: the repeat
     * reads no further back than [filled], which restarts at 0, and the tape
     * reads from where it was pressed.
     */
    void reset() {
        for (int c = 0; c < 2; ++c) {
            std::fill(echo[c].begin(), echo[c].end(), 0.0f);
            ic1[c] = ic2[c] = 0.0f;
            fbLow[c] = fbHigh[c] = 0.0f;
        }
        w = 0;
        filled = 0;
        repHeld = false;
        repActive = false;
        repMix = 0.0f;
        repK = 0;
        revHeld = revActive = false;
        revMix = 0.0f;
        gateActive = false;
        gateK = 0;
        gateG = 1.0f;
        killsOn = false;
        killMix = 0.0f;
        for (float &g : killGain) g = 1.0f;
        for (auto &x : xo) x.reset();
        crushAcc = 1.0f;
        crushEnv = 0.0f;
        heldL = heldR = 0.0f;
        for (auto &side : diffuse) for (auto &d : side) d.reset();
        riserOn = false;
        riserHeld = false;
        riserMix = 0.0f;
        riserHp[0] = riserHp[1] = RiserSvf{};
        riserNoise = RiserSvf{};
        noiseSeed = 0x1234567u;
        tape = Tape::Off;
        rate = 1.0f;
        rejoin = 0.0f;
        xs = 0.5f;
        ys = 0.0f;
        filterOn = false;
        echoLen = -1.0f;
        lastLoud = -(int64_t{1} << 40);
    }

    /** The group they run on, or -1 for the whole mix (see MasterBus::process). */
    int32_t wantedGroup() const { return static_cast<int32_t>(params_.get(Target) + 0.5f) - 1; }

    /** Every held control back to rest, on a transport stop or a panic. */
    void release() {
        params_.set(Repeat, 0.0f);
        params_.set(Stop, 0.0f);
        params_.set(X, 0.5f);
        params_.set(Y, 0.0f);
        params_.set(Reverse, 0.0f);
        params_.set(Gate, 0.0f);
        params_.set(KillLow, 0.0f);
        params_.set(KillMid, 0.0f);
        params_.set(KillHigh, 0.0f);
        params_.set(Riser, 0.0f);
    }

    /**
     * Where the transport is at the start of the next block, in fractional
     * ticks. A repeat started while playing takes its slice from the grid line
     * before the press so it's in time. When stopped, from the press.
     *
     * Fractional because a block is well under a tick (64 frames against about
     * 100 at 120 bpm), so whole ticks can be up to a tick late.
     */
    void setTransport(bool playing, double tick) {
        transportPlaying = playing;
        transportTick = tick;
    }

    /** In place, on the mix. */
    void process(float *L, float *R, int32_t frames, float bpm) {
        params_.tick();
        const float spb = 60.0f / std::max(bpm, 1.0f) * sr; // samples per beat
        const bool playing = transportPlaying;

        // --- Repeat: engage, change length, let go -------------------------
        const int32_t k = static_cast<int32_t>(params_.get(Repeat) + 0.5f);
        if (k > 0) {
            const float beats = 1.0f / static_cast<float>(1 << (k - 1));
            const int32_t len = std::clamp(static_cast<int32_t>(beats * spb + 0.5f), 64, kSize);
            if (!repActive) {
                // A new slice, starting at the last grid line of its own length. The
                // part since then is already in the ring.
                int32_t offset = 0;
                if (playing) {
                    const double tickLen = static_cast<double>(beats) * kPPQN;
                    const double perTick = static_cast<double>(len) / tickLen;
                    const double into = std::fmod(std::max(0.0, transportTick), tickLen);
                    offset = std::clamp(static_cast<int32_t>(into * perTick + 0.5), 0, len - 1);
                }
                for (int32_t j = 0; j < offset; ++j) {
                    const int64_t at = w - offset + j;
                    const bool known = w - at <= filled;
                    slice[0][j] = known ? ring[0][at & kMask] : 0.0f;
                    slice[1][j] = known ? ring[1][at & kMask] : 0.0f;
                }
                repCaptured = offset;
                repElapsed = offset;
                repLen = len;
                repActive = true;
            } else if (k != repK) {
                // Shorter is always available. Longer only as far as was captured.
                repLen = repCaptured >= repLen ? std::min(len, repCaptured) : len;
            }
            repK = k;
            repHeld = true;
        } else {
            repHeld = false;
        }

        // --- Reverse: the beat before the last beat line, backwards ---------
        // It's already fully in the ring, so it plays from the press. The ring
        // holds the repeat's output, so a held repeat reverses too.
        const bool revNow = params_.get(Reverse) >= 0.5f;
        if (revNow && !revActive) {
            const int32_t len = std::clamp(static_cast<int32_t>(spb + 0.5f), 64, kSize / 2);
            int32_t offset = 0;
            if (playing) {
                const double into = std::fmod(std::max(0.0, transportTick), static_cast<double>(kPPQN));
                offset = std::clamp(static_cast<int32_t>(into * len / kPPQN + 0.5), 0, len - 1);
            }
            for (int32_t j = 0; j < len; ++j) {
                const int64_t at = w - offset - len + j;
                const bool known = w - at <= filled;
                rslice[0][j] = known ? ring[0][at & kMask] : 0.0f;
                rslice[1][j] = known ? ring[1][at & kMask] : 0.0f;
            }
            revLen = len;
            revElapsed = offset;
            revActive = true;
        }
        revHeld = revNow;

        // --- Gate: a square chop in time ----------------------------------
        gateK = static_cast<int32_t>(params_.get(Gate) + 0.5f);
        if (gateK > 0 && !gateActive) {
            gateActive = true;
            gateFrom = w;
        }
        const double gateBeats[5] = {0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0};
        const double gatePeriod = gateK > 0 ? gateBeats[gateK - 1] : 1.0;
        const double samplesPerTick = static_cast<double>(spb) / kPPQN;

        // --- Kills ---------------------------------------------------------
        const bool killHeld[3] = {params_.get(KillLow) >= 0.5f, params_.get(KillMid) >= 0.5f,
                                  params_.get(KillHigh) >= 0.5f};
        const bool anyKill = killHeld[0] || killHeld[1] || killHeld[2];
        if (anyKill && !killsOn) {
            killsOn = true;
            for (auto &x : xo) x.reset();
        }

        // --- Riser ---------------------------------------------------------
        const bool riserWas = riserHeld;
        riserHeld = params_.get(Riser) >= 0.5f;
        if (riserHeld && !riserWas) {
            // Pressed again while still fading out: the climb restarts on the
            // filters it already has, so nothing clicks.
            if (!riserOn) {
                riserHp[0] = riserHp[1] = RiserSvf{};
                riserNoise = RiserSvf{};
            }
            riserOn = true;
            riserFrom = w;
            riserCoefAge = 0;
        }
        const float riserFrames =
            4.0f * spb * static_cast<float>(1 << static_cast<int32_t>(params_.get(RiserLen) + 0.5f));

        // --- Tape: stop and start ------------------------------------------
        const bool stopHeld = params_.get(Stop) >= 0.5f;
        const float stopBeats = 0.25f * static_cast<float>(1 << static_cast<int32_t>(params_.get(StopLen) + 0.5f));
        const float stopFrames = std::min(stopBeats * spb, static_cast<float>(kSize) * 0.5f);
        if (stopHeld && tape != Tape::Stopping) {
            if (tape == Tape::Off) readPos = static_cast<double>(w);
            tape = Tape::Stopping;
        } else if (!stopHeld && tape == Tape::Stopping) {
            // Released before it was silent: spin up from where it got to.
            // After that: from now, since nothing was being heard anyway.
            if (rate < 0.01f) readPos = static_cast<double>(w);
            tape = Tape::Starting;
        }

        // --- The pad ------------------------------------------------------
        const float xTarget = params_.target(X);
        const float yTarget = params_.target(Y);
        const bool crush = params_.get(XMode) >= 0.5f;
        const bool wash = params_.get(YMode) >= 0.5f;
        const float throwBeats[5] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
        // A wash is short and fixed rather than a note value. That short time
        // is what makes it a smear instead of repeats.
        const float echoTarget = wash ? 0.09f * sr : std::min(
            throwBeats[std::clamp(static_cast<int32_t>(params_.get(ThrowTime) + 0.5f), 0, 4)] * spb,
            static_cast<float>(kSize - 4));
        if (echoLen < 0.0f || wash != washWas) echoLen = echoTarget; // a mode change jumps, a tempo change glides
        washWas = wash;
        const float fb = wash ? std::min(0.93f, 0.7f + 0.3f * params_.get(Feedback)) : params_.get(Feedback);
        const bool echoAwake = yTarget > 0.0f || ys > 1e-7f || w - lastLoud <= static_cast<int64_t>(echoLen) + 2;
        const bool padIdle = xTarget == 0.5f && xs == 0.5f && !filterOn;

        if (!repActive && !revActive && !gateActive && tape == Tape::Off && !riserOn && !killsOn && padIdle &&
            !echoAwake) {
            // At rest: only the ring is fed, so a repeat or stop pressed next has
            // audio behind it. The echo is written silent so waking it never replays
            // something from a lap of the buffer ago.
            for (int32_t i = 0; i < frames; ++i) {
                ring[0][w & kMask] = L[i];
                ring[1][w & kMask] = R[i];
                echo[0][w & kMask] = echo[1][w & kMask] = 0.0f;
                ++w;
            }
            filled = std::min<int64_t>(filled + frames, kSize);
            ys = 0.0f;
            return;
        }

        const float lpCoef = 1.0f - std::exp(-6.2831853f * 3500.0f / sr);
        const float hpCoef = 1.0f - std::exp(-6.2831853f * 120.0f / sr);
        for (int32_t i = 0; i < frames; ++i) {
            float l = L[i], r = R[i];

            // Repeat.
            if (repActive) {
                if (repCaptured < repLen) {
                    slice[0][repCaptured] = l;
                    slice[1][repCaptured] = r;
                    ++repCaptured;
                }
                const int32_t pos = static_cast<int32_t>(repElapsed % repLen);
                float edge = 1.0f;
                if (repElapsed >= repLen && pos < edgeFrames) edge = static_cast<float>(pos) / edgeFrames;
                if (repLen - pos <= edgeFrames) edge = std::min(edge, static_cast<float>(repLen - pos) / edgeFrames);
                const float sL = slice[0][pos] * edge, sR = slice[1][pos] * edge;
                ++repElapsed;
                repMix = repHeld ? std::min(1.0f, repMix + mixStep) : std::max(0.0f, repMix - mixStep);
                l += repMix * (sL - l);
                r += repMix * (sR - r);
                if (!repHeld && repMix <= 0.0f) repActive = false;
            }

            // Reverse.
            if (revActive) {
                const int32_t pos = static_cast<int32_t>(revElapsed % revLen);
                const int32_t back = revLen - 1 - pos;
                float edge = 1.0f;
                if (pos < edgeFrames) edge = static_cast<float>(pos) / edgeFrames;
                if (revLen - pos <= edgeFrames) edge = std::min(edge, static_cast<float>(revLen - pos) / edgeFrames);
                ++revElapsed;
                revMix = revHeld ? std::min(1.0f, revMix + mixStep) : std::max(0.0f, revMix - mixStep);
                l += revMix * (rslice[0][back] * edge - l);
                r += revMix * (rslice[1][back] * edge - r);
                if (!revHeld && revMix <= 0.0f) revActive = false;
            }

            // Gate.
            if (gateActive) {
                float open = 1.0f;
                if (gateK > 0) {
                    const double phase = playing
                        ? std::fmod(transportTick + i / samplesPerTick, gatePeriod * kPPQN) / (gatePeriod * kPPQN)
                        : std::fmod(static_cast<double>(w - gateFrom) / (gatePeriod * spb), 1.0);
                    open = phase < 0.5 ? 1.0f : 0.0f;
                }
                gateG += std::clamp(open - gateG, -gateStep, gateStep);
                l *= gateG;
                r *= gateG;
                if (gateK == 0 && gateG >= 1.0f) gateActive = false;
            }

            // The ring gets the repeat, reverse and gate output, so that's what the
            // tape stops.
            ring[0][w & kMask] = l;
            ring[1][w & kMask] = r;
            if (filled < kSize) ++filled;

            // Tape.
            if (tape != Tape::Off) {
                if (tape == Tape::Stopping) rate = std::max(0.0f, rate - 1.0f / stopFrames);
                else rate = std::min(1.0f, rate + 2.0f / stopFrames);
                readPos = std::min(readPos + rate, static_cast<double>(w));
                const int64_t base = static_cast<int64_t>(readPos);
                const float frac = static_cast<float>(readPos - static_cast<double>(base));
                const int64_t next = std::min<int64_t>(base + 1, w);
                const float gain = std::min(1.0f, rate * 2.5f);
                float tl = (ring[0][base & kMask] + frac * (ring[0][next & kMask] - ring[0][base & kMask])) * gain;
                float tr = (ring[1][base & kMask] + frac * (ring[1][next & kMask] - ring[1][base & kMask])) * gain;
                if (tape == Tape::Starting && rate >= 1.0f) {
                    // Up to speed but behind: crossfade to the live mix.
                    rejoin = std::min(1.0f, rejoin + rejoinStep);
                    tl += rejoin * (l - tl);
                    tr += rejoin * (r - tr);
                    if (rejoin >= 1.0f) { tape = Tape::Off; rejoin = 0.0f; }
                } else {
                    rejoin = 0.0f;
                }
                l = tl;
                r = tr;
            }
            ++w;

            // Riser: the high pass and noise rise exponentially to the top at
            // [riserFrames] and hold there.
            if (riserOn) {
                if (riserCoefAge-- <= 0) {
                    const float p = std::min(1.0f, static_cast<float>(w - riserFrom) / riserFrames);
                    riserHp[0].tune(20.0f * std::pow(100.0f, p), 0.9f, sr);
                    riserHp[1].a1 = riserHp[0].a1; riserHp[1].a2 = riserHp[0].a2; riserHp[1].a3 = riserHp[0].a3;
                    riserHp[1].k = riserHp[0].k;
                    riserNoise.tune(500.0f * std::pow(16.0f, p), 2.0f, sr);
                    riserLevel = 0.125f * p * p;
                    riserCoefAge = 15;
                }
                noiseSeed = noiseSeed * 1664525u + 1013904223u;
                const float white = static_cast<float>(noiseSeed >> 8) / 8388608.0f - 1.0f;
                const float noise = riserNoise.band(white) * riserLevel;
                riserMix = riserHeld ? std::min(1.0f, riserMix + riserStep) : std::max(0.0f, riserMix - riserStep);
                l += riserMix * (riserHp[0].high(l) + noise - l);
                r += riserMix * (riserHp[1].high(r) + noise - r);
                if (!riserHeld && riserMix <= 0.0f) riserOn = false;
            }

            // Kills: the bands summed back without the killed ones.
            if (killsOn) {
                bool settled = !anyKill;
                for (int b = 0; b < 3; ++b) {
                    const float target = killHeld[b] ? 0.0f : 1.0f;
                    killGain[b] += std::clamp(target - killGain[b], -mixStep, mixStep);
                    if (killGain[b] != 1.0f) settled = false;
                }
                killMix = settled ? std::max(0.0f, killMix - mixStep) : std::min(1.0f, killMix + mixStep);
                const float yl = xo[0].split(l, killGain), yr = xo[1].split(r, killGain);
                l += killMix * (yl - l);
                r += killMix * (yr - r);
                if (settled && killMix <= 0.0f) killsOn = false;
            }

            // The filter.
            xs += (xTarget - xs) * smoothA;
            if (std::fabs(xs - xTarget) < 1e-6f) xs = xTarget;
            const float d = xs - 0.5f;
            const float amt = std::clamp((std::fabs(d) - 0.02f) / 0.06f, 0.0f, 1.0f);
            if (amt > 0.0f) {
                if (!filterOn) { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; filterOn = true; coefAge = 0; }
                if (coefAge-- <= 0) {
                    const float t = std::clamp((std::fabs(d) - 0.02f) / 0.48f, 0.0f, 1.0f);
                    const float f = d < 0.0f ? 20000.0f * std::pow(100.0f / 20000.0f, t)
                                             : 20.0f * std::pow(6000.0f / 20.0f, t);
                    const float g = std::tan(3.14159265f * std::min(f, 0.45f * sr) / sr);
                    a1 = 1.0f / (1.0f + g * (g + kDamp));
                    a2 = g * a1;
                    a3 = g * a2;
                    highPass = d > 0.0f;
                    coefAge = 15;
                }
                if (!crush) {
                    l += amt * (svf(0, l) - l);
                    r += amt * (svf(1, r) - r);
                } else {
                    const float t = std::clamp((std::fabs(d) - 0.02f) / 0.48f, 0.0f, 1.0f);
                    crushEnv = std::max({std::fabs(l), std::fabs(r), crushEnv * envRelease});
                    float cl = l, cr = r;
                    if (d < 0.0f) {
                        // Left: sample rate reduction.
                        crushAcc += 1.0f / (1.0f + t * 15.0f);
                        if (crushAcc >= 1.0f) { crushAcc -= 1.0f; heldL = l; heldR = r; }
                        cl = heldL;
                        cr = heldR;
                    } else {
                        // Right: bit reduction, relative to the signal's level rather than
                        // full scale, so a quiet mix crushes the same as a loud one.
                        const float step = std::max(crushEnv, 1e-4f) * std::exp2(-(15.0f - t * 12.0f));
                        cl = std::round(l / step) * step;
                        cr = std::round(r / step) * step;
                    }
                    l += amt * (cl - l);
                    r += amt * (cr - r);
                }
            } else if (filterOn && xs == 0.5f) {
                filterOn = false;
            }

            // The throw.
            ys += (yTarget - ys) * smoothA;
            if (ys < 1e-7f && yTarget == 0.0f) ys = 0.0f;
            if (echoAwake) {
                echoLen += std::clamp(echoTarget - echoLen, -1.0f, 1.0f); // glide, don't jump
                float frac = 0.0f;
                const float at = static_cast<float>(w & kMask) - echoLen;
                const float wrapped = at < 0.0f ? at + static_cast<float>(kSize) : at;
                const int32_t i0 = static_cast<int32_t>(wrapped) & kMask;
                frac = wrapped - std::floor(wrapped);
                const int32_t i1 = (i0 + 1) & kMask;
                float dl = echo[0][i0] + frac * (echo[0][i1] - echo[0][i0]);
                float dr = echo[1][i0] + frac * (echo[1][i1] - echo[1][i0]);
                if (wash) {
                    // Smeared on the way out so both what's heard and what feeds back
                    // are a wash rather than repeats.
                    for (auto &d : diffuse[0]) dl = d.process(dl);
                    for (auto &d : diffuse[1]) dr = d.process(dr);
                }
                // Crossed so each repeat comes from the other side, and darker and
                // thinner on every pass like a tape echo.
                const float inL = l * ys + fb * tone(0, dr, lpCoef, hpCoef);
                const float inR = r * ys + fb * tone(1, dl, lpCoef, hpCoef);
                echo[0][w & kMask] = inL;
                echo[1][w & kMask] = inR;
                if (std::fabs(inL) > 1e-6f || std::fabs(inR) > 1e-6f) lastLoud = w;
                l += dl;
                r += dr;
            } else {
                echo[0][w & kMask] = echo[1][w & kMask] = 0.0f;
            }

            L[i] = l;
            R[i] = r;
        }
    }

  private:
    enum class Tape : uint8_t { Off, Stopping, Starting };

    /** One channel's three-band split. */
    struct Crossover {
        dsp::Biquad lo[2], rest[2], mid[2], high[2], apLo[2], apHi[2];
        void reset() {
            for (int k = 0; k < 2; ++k) {
                lo[k].reset(); rest[k].reset(); mid[k].reset(); high[k].reset(); apLo[k].reset(); apHi[k].reset();
            }
        }
        float split(float x, const float *gain) {
            const float l0 = lo[1].process(lo[0].process(x));
            const float r0 = rest[1].process(rest[0].process(x));
            const float m = mid[1].process(mid[0].process(r0));
            const float h = high[1].process(high[0].process(r0));
            const float lowAligned = apLo[1].process(apLo[0].process(l0)) + apHi[1].process(apHi[0].process(l0));
            return gain[0] * lowAligned + gain[1] * m + gain[2] * h;
        }
    };

    /** A Schroeder all-pass, for the wash. */
    struct Diffuser {
        static constexpr int32_t kMax = 4096;
        float buf[kMax] = {};
        int32_t len = 100, at = 0;
        void reset() { std::fill(buf, buf + kMax, 0.0f); at = 0; }
        float process(float x) {
            const float delayed = buf[at];
            const float y = -0.6f * x + delayed;
            buf[at] = x + 0.6f * y;
            if (++at >= len) at = 0;
            return y;
        }
    };

    /** A small state-variable filter for the riser: high pass, or band pass for its noise. */
    struct RiserSvf {
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        void tune(float hz, float q, float sr) {
            const float g = std::tan(3.14159265f * std::min(hz, 0.45f * sr) / sr);
            k = 1.0f / q;
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        void step(float x, float &v1, float &v2) {
            const float v3 = x - ic2;
            v1 = a1 * ic1 + a2 * v3;
            v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
        }
        float high(float x) { float v1, v2; step(x, v1, v2); return x - k * v1 - v2; }
        float band(float x) { float v1, v2; step(x, v1, v2); return v1; }
    };

    float svf(int c, float x) {
        const float v3 = x - ic2[c];
        const float v1 = a1 * ic1[c] + a2 * v3;
        const float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
        ic1[c] = 2.0f * v1 - ic1[c];
        ic2[c] = 2.0f * v2 - ic2[c];
        return highPass ? x - kDamp * v1 - v2 : v2;
    }

    float tone(int c, float x, float lp, float hp) {
        fbLow[c] += (x - fbLow[c]) * lp;
        fbHigh[c] += (fbLow[c] - fbHigh[c]) * hp;
        return fbLow[c] - fbHigh[c];
    }

    ParamSet params_;
    float sr = 48000.0f;
    bool transportPlaying = false;
    double transportTick = 0.0;
    float smoothA = 0.004f;
    float mixStep = 0.004f;
    float rejoinStep = 0.002f;
    int32_t edgeFrames = 72;

    std::vector<float> ring[2], slice[2], rslice[2], echo[2];
    int64_t w = 0;       // frames written to the ring, ever
    int64_t filled = 0;  // how many of the ring's frames are real

    bool repHeld = false, repActive = false;
    int32_t repK = 0, repLen = 0, repCaptured = 0;
    int64_t repElapsed = 0;
    float repMix = 0.0f;

    bool revHeld = false, revActive = false;
    int32_t revLen = 0;
    int64_t revElapsed = 0;
    float revMix = 0.0f;

    Crossover xo[2];
    Diffuser diffuse[2][4];
    bool washWas = false;
    float crushAcc = 1.0f, crushEnv = 0.0f, envRelease = 0.9995f, heldL = 0.0f, heldR = 0.0f;
    bool killsOn = false;
    float killMix = 0.0f;
    float killGain[3] = {1.0f, 1.0f, 1.0f};

    bool riserOn = false, riserHeld = false;
    int64_t riserFrom = 0;
    float riserMix = 0.0f, riserStep = 0.002f, riserLevel = 0.0f;
    int32_t riserCoefAge = 0;
    RiserSvf riserHp[2], riserNoise;
    uint32_t noiseSeed = 0x1234567u;

    bool gateActive = false;
    int32_t gateK = 0;
    int64_t gateFrom = 0;
    float gateG = 1.0f;
    float gateStep = 0.01f;

    Tape tape = Tape::Off;
    float rate = 1.0f;
    float rejoin = 0.0f;
    double readPos = 0.0;

    float xs = 0.5f, ys = 0.0f;
    bool filterOn = false, highPass = false;
    int32_t coefAge = 0;
    static constexpr float kDamp = 0.9f; // a little resonance, like a DJ filter
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1[2] = {}, ic2[2] = {};

    float echoLen = -1.0f;
    int64_t lastLoud = 0;
    float fbLow[2] = {}, fbHigh[2] = {};
};

} // namespace acidulous
