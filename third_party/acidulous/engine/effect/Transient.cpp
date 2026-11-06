#include "Transient.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <sequencer/Swing.h>

namespace acidulous::effect {

namespace {
/** The followers at time 1, in seconds. */
constexpr float kFastAttack = 0.0003f, kSlowAttack = 0.015f, kRelease = 0.05f;
constexpr float kHoldRelease = 0.4f, kFallRelease = 0.04f;
/** Added to every follower, so silence and hiss read as no change (-80 dB). */
constexpr float kFloor = 1.0e-4f;
/**
 * How far the followers may part, in octaves of level (6 dB each), and how
 * much of that `attack` or `sustain` at 1 turns into gain: up to 12 dB.
 */
constexpr float kMostApart = 4.0f, kDepth = 0.5f;
/** The gain's range, in octaves: -24 to +12 dB. */
constexpr float kLeast = -4.0f, kMost = 2.0f;
/** At the bottom of `split` the whole sound is shaped. */
constexpr float kSplitOff = 21.0f;
/** The grid's step lengths in ticks: 1/16 triplet, 1/16, 1/8. */
constexpr int64_t kStepTicks[3] = {kPPQN / 6, kPPQN / 4, kPPQN / 2};
/**
 * The hit detector's high way: energy above kHighFrom Hz over kHighAvg
 * seconds. A hit is when it's kHighRise times (2.6 dB) its peak over the
 * last few ms: what it was kHighLate seconds ago, or before that, falling in
 * kRecentFall seconds. So a roll's hits count over the last one's ring, and
 * an organ's ripple or a noise's grain doesn't.
 *
 * A saw's click every cycle would count too, so a jump of kSpike times (6 dB)
 * is a spike, and a hit must come kSpikeGap seconds after the last spike
 * started, longer than a 31 Hz cycle and shorter than a 32nd at 200 bpm.
 * Sooner, as in a flam, it must be kPeakRise times its recent peak (5 dB),
 * which hears it as late and falls in kHighFall seconds, and later at least
 * that peak, so a click coming up out of a hit's tail isn't a hit.
 */
constexpr float kHighFrom = 400.0f, kHighAvg = 0.001f, kHighRise = 1.8f, kHighLate = 0.002f, kRecentFall = 0.008f;
constexpr float kSpike = 4.0f, kSpikeGap = 0.033f, kPeakRise = 3.2f, kHighFall = 0.03f;
/**
 * Energy under kHighFloor (-80 dB), or under kHighShare of the level's
 * (-30 dB), is nothing to the high way: what's left of a bass note's
 * ripple after the filter stays under it.
 */
constexpr float kHighFloor = 1.0e-8f, kHighShare = 1.0e-3f;
/**
 * The low way: the envelope falls in kEnvRelease seconds, slower than a 20 Hz
 * half wave, and a hit is when it's kEnvRise times its recent low (6 dB),
 * which climbs in kEnvUp seconds.
 */
constexpr float kEnvRelease = 0.04f, kEnvRise = 2.0f, kEnvUp = 0.03f;
/** After a hit starts, no other can for this long (seconds): a flam is 20 ms. */
constexpr float kWait = 0.008f;
/**
 * The accent comes in as the attack followers part from kAccentFrom to 1
 * octave, so it leaves the ring between hits alone.
 */
constexpr float kAccentFrom = 0.3f;
/**
 * The step holds at least kHitHold seconds and until the hit's envelope is
 * kHitEnd of its peak or its attack is over, or kHitMost seconds at most (all
 * at time 1), then goes back to flat, so a roll's next hit doesn't start on
 * the last one's step.
 */
constexpr float kHitHold = 0.015f, kHitEnd = 0.5f, kHitMost = 0.035f;
/** The most the accent may move the gain from one sample to the next, in dB. */
constexpr float kAccentSlew = 0.05f;
/**
 * In a hit's first kQuick seconds, while it has a click of its own, which
 * covers the change, the accent may move up to kQuickSlew dB a sample. A
 * click is a high way hit, or a spike kClickPeak times (3 dB) the recent peak.
 * Only a hit whose level lifts kQuickLift times (2.3 dB) gets it: a bass's
 * click coming up out of the last hit's tail can start a hit, but not loudly.
 */
constexpr float kQuickSlew = 0.75f, kQuick = 0.003f, kClickPeak = 2.0f, kQuickLift = 1.3f;
/**
 * Turned on, the grid listens this long (seconds) before a hit can start, so
 * the detector isn't fooled by what it missed while off.
 */
constexpr float kWarmUp = 0.02f;
/**
 * A hit up to a quarter step early counts as the step it's early for, so
 * a played-in or rounded note isn't put on the step before.
 */
constexpr int64_t kLeadDivisor = 4;

inline float follow(float &env, float x, float attack, float release) {
    env = dsp::guardDenormal(env + (x - env) * (x > env ? attack : release));
    return env;
}
} // namespace

const ParamDef *Transient::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"attack", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"sustain", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"time", 0.25f, 4.0f, 1.0f, Curve::Exponential, 0, ""},
        {"split", 20.0f, 8000.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"grid", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"depth", 0.0f, 12.0f, 6.0f, Curve::Linear, 0, "dB"},
        {"rate", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""}, // 1/16T 1/16 1/8
        {"length", 1.0f, 16.0f, 16.0f, Curve::Stepped, 16, ""},
        // The steps: 0 down, 1 flat, 2 up.
        {"a01", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a02", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a03", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a04", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a05", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a06", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a07", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a08", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a09", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a10", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a11", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a12", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a13", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a14", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a15", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
        {"a16", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""},
    };
    count = Count;
    return defs;
}

void Transient::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    timeWas = -1.0f;
    hpCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * kHighFrom), sr);
    hiECoeff = dsp::onePoleCoeff(kHighAvg, sr);
    hiFall = 1.0f - dsp::onePoleCoeff(kHighFall, sr);
    spikeGapMost = static_cast<int32_t>(kSpikeGap * sr);
    recentFall = 1.0f - dsp::onePoleCoeff(kRecentFall, sr);
    pastLength = std::clamp(static_cast<int32_t>(kHighLate * sr), 1, kPastMost);
    envRelease = dsp::onePoleCoeff(kEnvRelease, sr);
    envUp = dsp::onePoleCoeff(kEnvUp, sr);
    accentStep = kAccentSlew / 6.0206f;
    quickStep = kQuickSlew / 6.0206f;
    reset();
}

void Transient::reset() {
    low[0] = low[1] = 0.0f;
    fastA = slowA = holdS = fallS = 0.0f;
    for (auto &side : hpLow) for (float &lp : side) lp = 0.0f;
    hiE = hiPeak = hiRecent = 0.0f;
    inSpike = false;
    sinceSpike = spikeGap = spikeGapMost;
    std::fill(hiPast, hiPast + kPastMost, 0.0f);
    pastAt = 0;
    env = envLow = hitPeak = 0.0f;
    hitFrames = wait = quick = 0;
    inHit = asleep = false;
    accent = accentNow = 0.0f;
}

float Transient::stepAccent(int64_t songTick) const {
    const auto &p = params_;
    const int64_t step = kStepTicks[std::clamp(static_cast<int>(p.get(Rate) + 0.5f), 0, 2)];
    const int64_t length = std::clamp(static_cast<int64_t>(p.get(Length) + 0.5f), int64_t{1}, int64_t{16});
    // Back to where the hit's note was written, swing counted from its clip's start, then onto the grid.
    const ModTransport &t = modTransport_;
    const int64_t inClip = songTick - t.swingFrom;
    const int64_t straight = inClip >= 0 ? seq::Swing::from(inClip, t.swing, t.swingPair) + t.swingFrom : songTick;
    const int64_t at = ((straight + step / kLeadDivisor) / step) % length;
    return std::round(p.get(A01 + static_cast<int32_t>(at))) - 1.0f;
}

bool Transient::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float time = p.get(Time);
    if (time != timeWas) {
        timeWas = time;
        atkFast = dsp::onePoleCoeff(kFastAttack, sr);
        atkSlow = dsp::onePoleCoeff(kSlowAttack * time, sr);
        relA = dsp::onePoleCoeff(kRelease * time, sr);
        relHold = dsp::onePoleCoeff(kHoldRelease * time, sr);
        relFall = dsp::onePoleCoeff(kFallRelease * time, sr);
    }
    const float attack = p.get(Attack) * kDepth, sustain = p.get(Sustain) * kDepth;
    const float split = p.get(Split);
    const bool splitting = split > kSplitOff;
    const float lc = splitting ? dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * split), sr) : 0.0f;
    const int chans = stereoIn ? 2 : 1;
    // The low filter only runs while split is on; it starts from silence each time.
    if (!splitting) low[0] = low[1] = 0.0f;
    // The grid: only while the song is moving.
    const bool grid = p.get(Grid) >= 0.5f;
    const bool rolling = modTransport_.rolling;
    const auto hold = static_cast<int32_t>(kHitHold * time * sr), most = static_cast<int32_t>(kHitMost * time * sr);
    const double ticksPerFrame = frames > 0 ? static_cast<double>(blockEnd - blockStart) / blockFramesOf(frames) : 0.0;
    const double songStart = static_cast<double>(blockStart + modTransport_.songOffset);
    const float depth = p.get(Depth) / 6.0206f; // in octaves, as the gain is worked out
    for (int32_t i = 0; i < frames; ++i) {
        // The part that's shaped: above the split, or all of it.
        float hi[2] = {L[i], stereoIn ? R[i] : 0.0f};
        if (splitting) {
            for (int c = 0; c < chans; ++c) {
                low[c] = dsp::guardDenormal(low[c] + (hi[c] - low[c]) * lc);
                hi[c] -= low[c];
            }
        }
        const float x = std::max(std::fabs(hi[0]), std::fabs(hi[1]));
        follow(fastA, x, atkFast, relA);
        follow(slowA, x, atkSlow, relA);
        follow(holdS, x, atkFast, relHold);
        follow(fallS, x, atkFast, relFall);
        const float apart = std::clamp(std::log2((fastA + kFloor) / (slowA + kFloor)), 0.0f, kMostApart);
        const float ring = std::clamp(std::log2((holdS + kFloor) / (fallS + kFloor)), 0.0f, kMostApart);
        float shape = attack * apart + sustain * ring;
        if (grid || accentNow != 0.0f) {
            if (asleep) {
                // Back from off: start listening from silence, and wait while it catches up.
                asleep = false;
                for (auto &side : hpLow) for (float &lp : side) lp = 0.0f;
                std::fill(hiPast, hiPast + kPastMost, 0.0f);
                hiE = hiPeak = hiRecent = env = envLow = hitPeak = 0.0f;
                inSpike = false;
                sinceSpike = spikeGap = spikeGapMost;
                wait = static_cast<int32_t>(kWarmUp * sr);
                quick = 0;
                inHit = false;
                accent = 0.0f;
            }
            // The high way, on the part that's shaped.
            float e = 0.0f;
            for (int c = 0; c < chans; ++c) {
                float h = hi[c];
                for (float &lp : hpLow[c]) {
                    lp = dsp::guardDenormal(lp + (h - lp) * hpCoeff);
                    h -= lp;
                }
                e = std::max(e, h * h);
            }
            hiE = dsp::guardDenormal(hiE + (e - hiE) * hiECoeff);
            const float late = hiPast[pastAt];
            hiPast[pastAt] = hiE;
            if (++pastAt >= pastLength) pastAt = 0;
            hiPeak = std::max(dsp::guardDenormal(hiPeak * hiFall), late);
            hiRecent = std::max(dsp::guardDenormal(hiRecent * recentFall), late);
            // The low way.
            env = dsp::guardDenormal(x > env ? x : env + (x - env) * envRelease);
            envLow = env < envLow ? env : dsp::guardDenormal(envLow + (env - envLow) * envUp);
            const bool spike = hiE > kSpike * late + kHighFloor;
            if (spike && !inSpike) {
                spikeGap = sinceSpike;
                sinceSpike = 0;
            } else if (sinceSpike < spikeGapMost) {
                ++sinceSpike;
            }
            inSpike = spike;
            const bool high = hiE > kHighRise * hiRecent + kHighFloor && hiE > kHighShare * env * env &&
                              (((spike ? spikeGap : sinceSpike) >= spikeGapMost && hiE > hiPeak) || hiE > kPeakRise * hiPeak);
            // Its own click: a hit by the high way, or a spike well over the recent peak.
            const bool click = high || (spike && hiE > kClickPeak * hiPeak && hiE > kHighShare * env * env);
            if (wait > 0) {
                // Waiting: the low way follows the hit up, so it can't start again on its
                // own rise. The high way hears it 2 ms late anyway.
                --wait;
                envLow = std::max(envLow, env);
            } else if (high || env > kEnvRise * envLow + kFloor) {
                wait = static_cast<int32_t>(kWait * sr);
                quick = env > kQuickLift * envLow ? static_cast<int32_t>(kQuick * sr) : 0;
                envLow = env;
                inHit = true;
                hitPeak = env;
                hitFrames = 0;
                const double tick = songStart + static_cast<double>(i + subOffset_) * ticksPerFrame;
                accent = grid && rolling ? stepAccent(static_cast<int64_t>(std::floor(tick))) : 0.0f;
            }
            if (inHit) {
                hitPeak = std::max(hitPeak, env);
                if (++hitFrames > hold && (env < kHitEnd * hitPeak || apart < kAccentFrom || hitFrames > most)) {
                    // The hit is over: back to flat.
                    inHit = false;
                    accent = 0.0f;
                }
            }
            // Turned off, it eases out and then stops, back to the plain sum above.
            const float to = grid ? accent * depth * std::clamp((apart - kAccentFrom) / (1.0f - kAccentFrom), 0.0f, 1.0f) : 0.0f;
            // A hit's own click covers a quicker change.
            float slew = accentStep;
            if (quick > 0) {
                --quick;
                if (click) slew = std::max(slew, quickStep);
            }
            accentNow += std::clamp(to - accentNow, -slew, slew);
            shape += accentNow;
        } else {
            asleep = true;
        }
        const float g = std::exp2(std::clamp(shape, kLeast, kMost));
        if (splitting) {
            L[i] = low[0] + hi[0] * g;
            if (stereoIn) R[i] = low[1] + hi[1] * g;
        } else {
            L[i] *= g;
            if (stereoIn) R[i] *= g;
        }
    }
    return stereoIn;
}

} // namespace acidulous::effect
