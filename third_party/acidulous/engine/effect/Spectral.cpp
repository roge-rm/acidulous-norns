#include "Spectral.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
constexpr float kTwoPi = 6.2831853f;
/** A frame, in seconds; the size is the power of two nearest it at the rate. */
constexpr float kFrameSeconds = 0.043f;
/** Hann windows in and out, four frames over each sample: together they add up to 1.5. */
constexpr float kOverlap = 1.0f / 1.5f;

/** Ticks in a quarter note. */
constexpr double kTicksPerBeat = 240.0;
/** Each `grid` choice's step, in ticks: 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32. */
constexpr int64_t kGridTicks[] = {240, 160, 120, 80, 60, 40, 30};
/** Hops the stutter takes to fade back into the track, and how much of its level each keeps: about -37 dB by the end. */
constexpr int32_t kLetGoHops = 12;
constexpr float kLetGoKeep = 0.7f;
/** Each `renew` choice's laps before the loop is taken again; 0 keeps it. */
constexpr int64_t kRenewLaps[] = {0, 1, 2, 4, 8};

float wrap(float a) {
    a = std::fmod(a + static_cast<float>(M_PI), kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - static_cast<float>(M_PI);
}
} // namespace

const ParamDef *Spectral::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"freeze", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"blur", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"smear", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"robot", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"peaks", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tilt", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"stutter", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"grid", 0.0f, 6.0f, 4.0f, Curve::Stepped, 7, ""},
        {"length", 1.0f, 16.0f, 4.0f, Curve::Stepped, 16, ""},
        {"order", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"fade", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"renew", 0.0f, 4.0f, 0.0f, Curve::Stepped, 5, ""},
    };
    count = Count;
    return defs;
}

void Spectral::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    size = 1 << static_cast<int>(std::lround(std::log2(kFrameSeconds * sr)));
    hop = size / 4;
    fft = std::make_unique<dsp::Fft>(size);
    const auto n = static_cast<size_t>(size), bins = n / 2 + 1;
    window.resize(n);
    for (size_t i = 0; i < n; ++i) window[i] = 0.5f - 0.5f * std::cos(kTwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(size));
    re.assign(n, 0.0f);
    im.assign(n, 0.0f);
    lean.assign(bins, 1.0f);
    leanDb = 1000.0f;
    leanDirty = false;
    octaves.resize(bins);
    const float binHz = sr / static_cast<float>(size);
    for (size_t k = 0; k < bins; ++k) {
        const float hz = std::max(50.0f, static_cast<float>(k) * binHz);
        octaves[k] = std::log2(hz / 1000.0f);
    }
    for (int c = 0; c < 2; ++c) {
        in[c].assign(n, 0.0f);
        out[c].assign(n, 0.0f);
        held[c].assign(bins, 0.0f);
        last[c].assign(bins, 0.0f);
        turn[c].assign(bins, 0.0f);
        played[c].assign(bins, 0.0f);
        dry[c].assign(n + 1, 0.0f);
        ringMag[c].assign(bins * kRing, 0.0f);
        ringTurn[c].assign(bins * kRing, 0.0f);
        ringPhase[c].assign(bins * kRing, 0.0f);
        stutterMag[c].assign(bins, 0.0f);
        stutterPhase[c].assign(bins, 0.0f);
    }
    playMag.assign(bins, 0.0f);
    playPhase.assign(bins, 0.0f);
    reset();
}

void Spectral::reset() {
    for (int c = 0; c < 2; ++c) {
        for (auto *v : {&in[c], &out[c], &held[c], &last[c], &turn[c], &played[c], &dry[c], &ringMag[c], &ringTurn[c], &ringPhase[c], &stutterMag[c], &stutterPhase[c]}) std::fill(v->begin(), v->end(), 0.0f);
    }
    fill = 0;
    dryAt = 0;
    frozen = false;
    rng = 0x9e3779b9u;
    stuttering = false;
    have = 0;
    lastStep = 0;
    looped = 0;
    slot = 0;
    starts = take = false;
    stepLevel = 1.0f;
    letGo = 0;
}

void Spectral::stutterStep(double now) {
    // A step shorter than a hop (1/32 above about 500 bpm) can pass between
    // two hops unseen; the loop just skips it.
    const auto &p = params_;
    const int grid = std::clamp(static_cast<int>(p.get(Grid) + 0.5f), 0, 6);
    const auto stepTicks = static_cast<double>(kGridTicks[grid]);
    const double at = now / stepTicks;
    const auto step = static_cast<int64_t>(std::floor(at));
    const int32_t length = std::clamp(static_cast<int32_t>(p.get(Length) + 0.5f), 1, kRing);
    take = starts = false;
    if (!stuttering) {
        // It starts at once, with what's playing now as the first step.
        stuttering = true;
        have = 1;
        looped = 0;
        slot = 0;
        lastStep = step;
        take = starts = true;
    } else if (step != lastStep) {
        lastStep = step;
        starts = true;
        const int64_t renew = kRenewLaps[std::clamp(static_cast<int>(p.get(Renew) + 0.5f), 0, 4)];
        if (renew > 0 && have >= length && looped >= renew * length) {
            // Played enough laps: the loop is taken again from the track.
            have = 0;
            looped = 0;
        }
        if (have < length) {
            // The first time through, each step is taken as it comes.
            slot = have++;
            take = true;
        } else {
            const int64_t n = length;
            const int64_t k = looped++;
            switch (static_cast<int>(p.get(Order) + 0.5f)) {
            case Reverse: slot = static_cast<int32_t>(n - 1 - k % n); break;
            case Bounce: {
                const int64_t period = std::max<int64_t>(1, 2 * n - 2);
                const int64_t q = k % period;
                slot = static_cast<int32_t>(q < n ? q : period - q);
                break;
            }
            case Random: {
                // Hashed from the step's count since the loop began, so the same each time.
                auto h = static_cast<uint32_t>(k) * 2654435761u + 0x7f4a7c15u;
                h ^= h >> 15;
                h *= 0x2c1b3c6du;
                h ^= h >> 12;
                slot = static_cast<int32_t>(h % static_cast<uint32_t>(n));
                break;
            }
            default: slot = static_cast<int32_t>(k % n); break;
            }
        }
    }
    // The fade: from held to -48 dB by the step's end.
    const float fade = p.get(Fade);
    const auto into = static_cast<float>(at - static_cast<double>(step));
    stepLevel = fade > 0.0f ? std::pow(10.0f, -fade * 48.0f * into / 20.0f) : 1.0f;
}

void Spectral::copyStutterToRight() {
    const auto bins = stutterMag[0].size();
    if (take) {
        const auto at = static_cast<std::ptrdiff_t>(static_cast<size_t>(slot) * bins), n = static_cast<std::ptrdiff_t>(bins);
        std::copy(ringMag[0].begin() + at, ringMag[0].begin() + at + n, ringMag[1].begin() + at);
        std::copy(ringTurn[0].begin() + at, ringTurn[0].begin() + at + n, ringTurn[1].begin() + at);
        std::copy(ringPhase[0].begin() + at, ringPhase[0].begin() + at + n, ringPhase[1].begin() + at);
    }
    std::copy(stutterMag[0].begin(), stutterMag[0].end(), stutterMag[1].begin());
    std::copy(stutterPhase[0].begin(), stutterPhase[0].end(), stutterPhase[1].begin());
}

void Spectral::frame(int c) {
    if (leanDirty) {
        // Tilt turns about 1 kHz, a few octaves either way. It lifts by no
        // more than 6 dB, so leaning dark doesn't boom and bright doesn't
        // hiss. Built here since only a frame reads it.
        leanDirty = false;
        for (size_t k = 0; k < lean.size(); ++k) lean[k] = std::pow(10.0f, std::min(6.0f, leanDb * octaves[k]) / 20.0f);
    }
    const auto &p = params_;
    const int32_t half = size / 2;
    for (int32_t i = 0; i < size; ++i) {
        re[static_cast<size_t>(i)] = in[c][static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
        im[static_cast<size_t>(i)] = 0.0f;
    }
    fft->transform(re.data(), im.data(), false);

    const bool freeze = frozen;
    // Blur: how much of each bin's level carries over from one frame to the
    // next, up to a few seconds' fade at the top.
    const float blur = p.get(Blur);
    const float keep = blur <= 0.0f ? 0.0f : std::pow(blur, 0.15f) * 0.995f;
    // The stutter plays its own spectrum, S and SP, over the track's, which
    // runs on underneath, a freeze included. Let go, it fades under the track's.
    const bool stutter = stuttering, fading = !stutter && letGo > 0;
    const size_t ring = static_cast<size_t>(slot) * static_cast<size_t>(half + 1);
    float *RM = ringMag[c].data() + ring, *RT = ringTurn[c].data() + ring, *RP = ringPhase[c].data() + ring;
    float *S = stutterMag[c].data(), *SP = stutterPhase[c].data();
    const float smear = p.get(Smear), robot = p.get(Robot), peaks = p.get(Peaks);
    float *H = held[c].data(), *Last = last[c].data(), *Turn = turn[c].data(), *Played = played[c].data();

    if (stutter || fading) {
        // A step's spectrum, taken now or out of the ring. Each step starts
        // from the phase it was taken with, so it plays back as it began.
        // Let go, it fades as the bins go on turning.
        for (int32_t k = 0; k <= half; ++k) {
            const auto i = static_cast<size_t>(k);
            if (fading) {
                S[k] *= kLetGoKeep;
                SP[k] = wrap(SP[k] + RT[k]);
                continue;
            }
            if (take) {
                const float phase = std::atan2(im[i], re[i]);
                RM[k] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
                RT[k] = wrap(phase - Last[k]);
                RP[k] = phase;
            }
            S[k] = RM[k] * stepLevel;
            SP[k] = starts ? RP[k] : wrap(SP[k] + RT[k]);
        }
    }

    float loudest = 0.0f;
    for (int32_t k = 0; k <= half; ++k) {
        const auto i = static_cast<size_t>(k);
        const float mag = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        const float phase = std::atan2(im[i], re[i]);
        if (!freeze && mag >= H[k] * keep) {
            // How far the bin turned since the last frame: its frequency, held
            // for a freeze or while it fades.
            Turn[k] = wrap(phase - Last[k]);
            H[k] = mag;
            Played[k] = phase;
        } else {
            // Frozen, or fading under blur: the bin goes on turning as it did.
            if (!freeze) H[k] *= keep;
            Played[k] = wrap(Played[k] + Turn[k]);
        }
        Last[k] = phase;
        loudest = std::max(loudest, H[k]);
    }
    // What the frame plays: the track's bins, or the stutter's, or while it
    // fades whichever is louder.
    const float *Mag = H, *Phase = Played;
    if (stutter || fading) {
        float *M = playMag.data(), *P = playPhase.data();
        loudest = 0.0f;
        for (int32_t k = 0; k <= half; ++k) {
            const bool own = stutter || S[k] > H[k];
            M[k] = own ? S[k] : H[k];
            P[k] = own ? SP[k] : Played[k];
            loudest = std::max(loudest, M[k]);
        }
        Mag = M;
        Phase = P;
    }
    // Scrambled or thrown-away timing cancels between overlapping frames
    // and the level drops; held levels pile up a little. Made up here, so
    // turning these keeps about the same loudness.
    const float makeup = std::pow(10.0f, (robot * 15.0f * (1.0f - 0.4f * smear) + smear * 9.0f - blur * 4.0f) / 20.0f);
    // Peaks: everything further under the loudest bin than this goes.
    const float floor = peaks > 0.0f ? loudest * std::pow(10.0f, -(1.0f - peaks) * 72.0f / 20.0f) : 0.0f;
    for (int32_t k = 0; k <= half; ++k) {
        const auto i = static_cast<size_t>(k);
        float mag = Mag[k];
        if (mag < floor) mag = 0.0f;
        float phase = Phase[k];
        if (smear > 0.0f) phase += smear * static_cast<float>(M_PI) * random();
        phase *= 1.0f - robot;
        mag *= lean[i] * makeup;
        re[i] = mag * std::cos(phase);
        im[i] = mag * std::sin(phase);
    }
    im[0] = 0.0f;
    im[static_cast<size_t>(half)] = 0.0f;
    for (int32_t k = 1; k < half; ++k) {
        re[static_cast<size_t>(size - k)] = re[static_cast<size_t>(k)];
        im[static_cast<size_t>(size - k)] = -im[static_cast<size_t>(k)];
    }
    fft->transform(re.data(), im.data(), true);

    float *o = out[c].data();
    std::memmove(o, o + hop, sizeof(float) * static_cast<size_t>(size - hop));
    std::fill(o + size - hop, o + size, 0.0f);
    for (int32_t i = 0; i < size; ++i) o[i] += re[static_cast<size_t>(i)] * window[static_cast<size_t>(i)] * kOverlap;
    float *x = in[c].data();
    std::memmove(x, x + hop, sizeof(float) * static_cast<size_t>(size - hop));
}

bool Spectral::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float mix = p.get(Mix);
    const float tilt = p.get(Tilt);
    if (std::fabs(tilt - leanDb) > 0.01f) {
        leanDb = tilt;
        leanDirty = true;
    }
    stereo = stereoIn;
    const bool wantFreeze = p.get(Freeze) > 0.5f;
    const bool wantStutter = p.get(Stutter) > 0.5f;
    const auto dryLength = static_cast<int32_t>(dry[0].size());
    // The song's position at this slice's first frame, in ticks, and how far a frame moves it.
    const double ticksPerFrame = static_cast<double>(bpm) * kTicksPerBeat / (60.0 * static_cast<double>(sr));
    const double sliceTick = static_cast<double>(tick) + static_cast<double>(subOffset_) * ticksPerFrame;

    for (int32_t i = 0; i < frames; ++i) {
        const float x[2] = {L[i], stereoIn ? R[i] : L[i]};
        float d[2], y[2];
        int32_t at = dryAt - size;
        if (at < 0) at += dryLength;
        for (int c = 0; c < 2; ++c) {
            dry[c][static_cast<size_t>(dryAt)] = x[c];
            d[c] = dry[c][static_cast<size_t>(at)];
            in[c][static_cast<size_t>(size - hop + fill)] = x[c];
            y[c] = out[c][static_cast<size_t>(fill)];
        }
        if (++dryAt >= dryLength) dryAt = 0;
        if (++fill >= hop) {
            fill = 0;
            // A freeze starts or ends on a frame.
            frozen = wantFreeze;
            if (letGo > 0) --letGo;
            if (wantStutter) {
                // Steps are counted from a quarter into the frame, so the frame
                // that starts a step is mostly that step's sound, its start included.
                stutterStep(sliceTick + (static_cast<double>(i + 1) - 0.75 * static_cast<double>(size)) * ticksPerFrame);
            } else if (stuttering) {
                stuttering = false;
                letGo = kLetGoHops;
            }
            frame(0);
            if (stereoIn) {
                frame(1);
            } else {
                std::copy(out[0].begin(), out[0].end(), out[1].begin());
                // The right channel's stutter follows the left's, so a track that
                // turns stereo part way through a loop plays it on both sides.
                if (stuttering || letGo > 0) copyStutterToRight();
            }
        }
        L[i] = d[0] + (y[0] - d[0]) * mix;
        if (stereoIn) R[i] = d[1] + (y[1] - d[1]) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
