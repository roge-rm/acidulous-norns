#include "Smash.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <sequencer/Swing.h>

namespace acidulous::effect {

namespace {
/**
 * Each band's attack and release at time 1, in seconds. The lows move
 * slowest, since a fast detector on a bass note follows its cycles and
 * distorts it; the highs fastest, so a hi-hat is caught.
 */
constexpr float kAttack[3] = {0.040f, 0.020f, 0.010f};
constexpr float kRelease[3] = {0.28f, 0.28f, 0.13f};
/** Under this nothing is pulled up, faded in over kKnee, so hiss and hum stay down. */
constexpr float kFloor = -60.0f, kKnee = 12.0f;
/** The most a band is pulled up, in dB. */
constexpr float kMaxBoost = 30.0f;
/** At `up` 1, a band under the threshold comes this much of the way up. */
constexpr float kUpMost = 0.75f;

inline float dbToGain(float db) { return std::exp(db * 0.115129255f); }

/** A sixteenth, in ticks; the beat's rhythms are a bar of sixteen of them, counted from the song's start. */
constexpr int64_t kStep = kPPQN / 4;
/**
 * Each duck setting's rhythm, a bit for each sixteenth of the bar, after
 * off (0) and sidechain (1): beats, offbeats, 8ths, 16ths, 2 and 4, halves,
 * 3-3-2, dotted 8ths.
 */
constexpr int kKeyed = 1;
constexpr uint32_t kRhythms[] = {
    0x1111u, // x . . . x . . . x . . . x . . .
    0x4444u, // . . x . . . x . . . x . . . x .
    0x5555u, // x . x . x . x . x . x . x . x .
    0xffffu, // every sixteenth
    0x1010u, // . . . . x . . . . . . . x . . .
    0x0101u, // x . . . . . . . x . . . . . . .
    0x4949u, // x . . x . . x . x . . x . . x .
    0x1249u, // x . . x . . x . . x . . x . . .
};
constexpr int kRhythmCount = static_cast<int>(sizeof(kRhythms) / sizeof(kRhythms[0]));
/** A beat duck starts going down this long before its hit, so it's down as the hit lands. */
constexpr float kLead = 0.005f;
/** The fastest a duck moves, in dB a second: 24 dB in 3 ms. Keeps a keyed duck or a change of rhythm from clicking. */
constexpr float kDuckSlew = 8000.0f;
/** How fast the sidechain's level is caught, in seconds. */
constexpr float kKeyAttack = 0.001f;
} // namespace

const ParamDef *Smash::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"depth", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"time", 0.1f, 10.0f, 1.0f, Curve::Exponential, 0, ""},
        {"threshold", -48.0f, 0.0f, -24.0f, Curve::Linear, 0, "dB"},
        {"down", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
        {"up", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"low", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"mid", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"high", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"in", -12.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        // The beat: 0 is the effect's own input, 1..16 the track a band at sidechain ducks under.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
        // Each band's duck: off, sidechain, then kRhythms.
        {"lowduck", 0.0f, 9.0f, 0.0f, Curve::Stepped, 10, ""},
        {"midduck", 0.0f, 9.0f, 0.0f, Curve::Stepped, 10, ""},
        {"highduck", 0.0f, 9.0f, 0.0f, Curve::Stepped, 10, ""},
        {"lowdepth", 0.0f, 24.0f, 9.0f, Curve::Linear, 0, "dB"},
        {"middepth", 0.0f, 24.0f, 9.0f, Curve::Linear, 0, "dB"},
        {"highdepth", 0.0f, 24.0f, 9.0f, Curve::Linear, 0, "dB"},
        {"recover", 20.0f, 1000.0f, 150.0f, Curve::Exponential, 0, "ms"},
    };
    count = Count;
    return defs;
}

void Smash::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &s : split) s.prepare(sr);
    timeWas = -1.0f;
    reset();
}

void Smash::reset() {
    for (auto &s : split) s.reset();
    for (float &e : env) e = 0.0f;
    for (float &e : fast) e = 0.0f;
    synced = newBlock = false;
    clock = 0.0;
    advanced = 0;
    keyEnv = 0.0f;
    for (int b = 0; b < kBands; ++b) {
        duckNow[b] = 0.0f;
        hitLast[b] = hitNext[b] = 0.0;
        maskWas[b] = 0u;
    }
}

double Smash::stepTick(int64_t step) const {
    const int64_t straight = step * kStep;
    // Swing counts from the clip's start, as the track's notes do.
    const ModTransport &t = modTransport_;
    const int64_t inClip = straight - t.swingFrom;
    if (inClip < 0) return static_cast<double>(straight);
    return static_cast<double>(seq::Swing::at(inClip, t.swing, t.swingPair) + t.swingFrom);
}

void Smash::findHits(int b, uint32_t mask, double tick) {
    maskWas[b] = mask;
    // The sixteenth the tick is in, by straight time; swing keeps every
    // sixteenth inside its pair, so it's at most one off, which the checks
    // either side put right.
    int64_t at = static_cast<int64_t>(std::floor(tick / static_cast<double>(kStep)));
    if (stepTick(at) > tick) --at;
    else if (stepTick(at + 1) <= tick) ++at;
    const auto hits = [mask](int64_t step) { return (mask >> static_cast<uint32_t>(((step % 16) + 16) % 16)) & 1u; };
    int64_t last = at;
    while (last > at - 16 && hits(last) == 0u) --last;
    int64_t next = at + 1;
    while (next < at + 17 && hits(next) == 0u) ++next;
    hitLast[b] = stepTick(last);
    hitNext[b] = stepTick(next);
}

bool Smash::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float time = p.get(Time);
    if (time != timeWas) {
        timeWas = time;
        for (int b = 0; b < kBands; ++b) {
            attack[b] = dsp::onePoleCoeff(kAttack[b] * time, sr);
            release[b] = dsp::onePoleCoeff(kRelease[b] * time, sr);
        }
    }
    const float depth = p.get(Depth), thr = p.get(Threshold);
    const float down = p.get(Down), up = p.get(Up) * kUpMost;
    const float inGain = dbToGain(p.get(In));
    const float out[kBands] = {dbToGain(p.get(Low)), dbToGain(p.get(Mid)), dbToGain(p.get(High))};

    // The beat's clock: the block's start as the engine gave it, unless it's
    // where the frames already run put it, to a fraction of a tick.
    const double ticksPerFrame = static_cast<double>(bpmNow) * kPPQN / (60.0 * static_cast<double>(sr));
    if (newBlock) {
        newBlock = false;
        const double start = static_cast<double>(blockStart);
        const double guess = clock + static_cast<double>(advanced) * ticksPerFrame;
        if (!synced || guess < start - 1.0 || guess > start + 2.0) clock = start;
        else clock = guess < start ? start : (guess >= start + 1.0 ? start + 0.999999 : guess);
        synced = true;
        advanced = 0;
    }
    // Each band's duck: a rhythm (only while the song moves), keyed, or off.
    uint32_t mask[kBands] = {};
    bool keyed[kBands] = {};
    float duckDepth[kBands] = {};
    bool ducking = false, anyKeyed = false;
    const bool rolling = modTransport_.rolling && ticksPerFrame > 0.0;
    for (int b = 0; b < kBands; ++b) {
        const int choice = static_cast<int>(p.get(LowDuck + b) + 0.5f);
        keyed[b] = choice == kKeyed && key_ != nullptr;
        anyKeyed = anyKeyed || keyed[b];
        if (choice > kKeyed && choice - kKeyed - 1 < kRhythmCount && rolling) mask[b] = kRhythms[choice - kKeyed - 1];
        duckDepth[b] = mask[b] != 0u || keyed[b] ? p.get(LowDepth + b) : 0.0f;
        // A duck still on its way back up carries on until it's there.
        ducking = ducking || duckDepth[b] > 0.0f || duckNow[b] > 0.0f;
    }
    const float recoverFrames = p.get(Recover) * 0.001f * sr;
    // A band's fall since its last hit, worked out once here and after each
    // hit, then taken down a frame at a time.
    const float fall = std::exp(-1.0f / recoverFrames);
    float tail[kBands] = {};
    bool tailKnown[kBands] = {};
    const float recoverCoeff = dsp::onePoleCoeff(p.get(Recover) * 0.001f, sr);
    const float keyAttack = dsp::onePoleCoeff(kKeyAttack, sr);
    const float slew = kDuckSlew / sr;
    const double lead = static_cast<double>(kLead * sr) * ticksPerFrame;
    const double songAt = clock + static_cast<double>(modTransport_.songOffset) + static_cast<double>(advanced) * ticksPerFrame;
    const float dryShare = 1.0f / inGain;

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        float bl[kBands], br[kBands];
        split[0].split(inL * inGain, bl);
        split[1].split(inR * inGain, br);
        float wetL = 0.0f, wetR = 0.0f;
        // The beat's ducks, as gains on each band.
        float duckGain[kBands] = {1.0f, 1.0f, 1.0f};
        if (ducking) {
            const double tick = songAt + static_cast<double>(i) * ticksPerFrame;
            float keyDb = -120.0f;
            if (anyKeyed) {
                const float ka = std::fabs(key_[i]);
                const float k = std::isfinite(ka) ? ka : 0.0f;
                keyEnv = dsp::guardDenormal(keyEnv + (k - keyEnv) * (k > keyEnv ? keyAttack : recoverCoeff));
                keyDb = 20.0f * std::log10(std::max(keyEnv, 1e-6f));
            }
            for (int b = 0; b < kBands; ++b) {
                float want = 0.0f;
                if (keyed[b]) {
                    want = std::clamp(keyDb - thr, 0.0f, duckDepth[b]);
                } else if (mask[b] != 0u) {
                    if (mask[b] != maskWas[b] || tick < hitLast[b] || tick >= hitNext[b]) {
                        findHits(b, mask[b], tick);
                        tailKnown[b] = false;
                    }
                    // Down at each hit and back up over `recover`, starting down just before it.
                    if (tailKnown[b]) {
                        tail[b] *= fall;
                    } else {
                        tail[b] = std::exp(-static_cast<float>((tick - hitLast[b]) / ticksPerFrame) / recoverFrames);
                        tailKnown[b] = true;
                    }
                    want = duckDepth[b] * tail[b];
                    const double ahead = hitNext[b] - tick;
                    if (ahead < lead) {
                        float w = 1.0f - static_cast<float>(ahead / lead);
                        w = w * w * (3.0f - 2.0f * w);
                        want = std::max(want, duckDepth[b] * w);
                    }
                }
                // Under a hundredth of a dB it's back.
                if (want < 0.01f) want = 0.0f;
                duckNow[b] += std::clamp(want - duckNow[b], -slew, slew);
                if (duckNow[b] < 0.01f) duckNow[b] = 0.0f;
                if (duckNow[b] > 0.0f) duckGain[b] = dbToGain(-duckNow[b]);
            }
        }
        for (int b = 0; b < kBands; ++b) {
            // One detector for both sides, so the image holds still.
            const float a = std::max(std::fabs(bl[b]), std::fabs(br[b]));
            env[b] += (a - env[b]) * (a > env[b] ? attack[b] : release[b]);
            env[b] = dsp::guardDenormal(env[b]);
            // The lift listens to a detector that catches a peak at once, so
            // a loud sound arriving after a quiet one isn't lifted by the
            // boost the quiet one had while the slow detector catches up.
            fast[b] = std::max(a, fast[b] + (a - fast[b]) * release[b]);
            fast[b] = dsp::guardDenormal(fast[b]);
            float g = 0.0f;
            if (env[b] > 1e-6f) {
                const float level = 20.0f * std::log10(env[b]);
                const float now = 20.0f * std::log10(std::max(fast[b], 1e-6f));
                if (level > thr) {
                    g = -(level - thr) * down;
                } else if (now < thr) {
                    // Under the threshold it comes up, eased in above the floor.
                    float w = (now - (kFloor - kKnee)) / kKnee;
                    if (w > 0.0f) {
                        if (w < 1.0f) w = w * w * (3.0f - 2.0f * w);
                        else w = 1.0f;
                        g = std::min((thr - now) * up * w, kMaxBoost);
                    }
                }
            }
            float lin = dbToGain(g) * out[b];
            if (ducking) lin *= duckGain[b];
            wetL += bl[b] * lin;
            wetR += br[b] * lin;
        }
        // Driven hard with the band gains up, a slow attack lets the start
        // of a loud sound through at many times full scale. Over full scale
        // it's rounded off; under it nothing changes.
        wetL = dsp::feedbackCeiling(wetL);
        wetR = dsp::feedbackCeiling(wetR);
        // Depth is how much of it is mixed in with the track as it was.
        if (ducking) {
            // The track as it was ducks too, band by band, so the beat is as deep at any depth.
            float dryL = inL, dryR = inR;
            for (int b = 0; b < kBands; ++b) {
                dryL += bl[b] * dryShare * (duckGain[b] - 1.0f);
                dryR += br[b] * dryShare * (duckGain[b] - 1.0f);
            }
            L[i] = dryL + (wetL - dryL) * depth;
            if (stereoIn) R[i] = dryR + (wetR - dryR) * depth;
        } else {
            L[i] = inL + (wetL - inL) * depth;
            if (stereoIn) R[i] = inR + (wetR - inR) * depth;
        }
    }
    advanced += frames;
    return stereoIn;
}

} // namespace acidulous::effect
