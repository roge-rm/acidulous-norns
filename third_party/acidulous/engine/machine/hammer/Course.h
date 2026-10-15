#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/dsp/Math.h>
#include <engine/machine/hammer/Dispersion.h>
#include <vector>

// The strings of one key: one, two or three, each a loop of velocity waves
// with the bridge at the join, tuned a hair apart and sharing the bridge.
//
// Each string's loop, from the bridge round to the bridge:
//   - the line, an integer number of samples;
//   - a first-order Thiran allpass for the fraction, which passes every
//     frequency whole (linear interpolation is a lowpass that changes with
//     the fraction, so treble notes would brighten and ring differently key
//     by key);
//   - a chain of first-order allpasses for the stiffness, its coefficient
//     solved offline so the first 30 partials land where the string's
//     inharmonicity puts them (Dispersion.h); or, for the long bass
//     strings, second-order sections designed at the note (see
//     designSections), which follow the stretch up to 5 kHz;
//   - a loss designed from three decay times, the fundamental's, the third
//     partial's and the seventh's, so the string rings for the time measured:
//     a pole and a zero close together, a shelf, since the loss rises above
//     the fundamental and then levels off, which a one-pole (rising for ever)
//     can't follow, and close together it barely moves the phase.
// No DC blocker in the loop: its phase lead near the note is a tenth of a
// radian, which flattens the low partials against the fundamental by a
// percent, and nothing tunes that out. The hammer pushes as much forward as
// back, so it puts no DC in, and what round-off leaves dies with the note.
// The loop is tuned from its own phase at the note, every filter counted
// (the Filament lesson): the line takes whatever is left of the period.
//
// The bridge takes a share of what the strings do together and none of what
// they do apart. Tuned a hair apart, together turns into apart and back: the
// prompt sound dies fast into the bridge, and what's left - the aftersound -
// rings on, beating. That's the piano's double decay, from its own physics.
// The board takes far more of the upper partials than of the fundamental
// (at middle C, -8 dB/s at the first, -39 at the seventh), so the share is
// b0 + b1 (1 - cos w): no phase, so it only takes, never detunes. The
// strings meet the bridge where they're read, where the next sample is
// already in the line, which is what lets the share look both ways.
//
// A string also moves across the board as well as into it. That motion is
// fed by the bridge, barely loses anything into it, and so rings on long after
// the motion the hammer gave has gone: the aftersound of a key with one
// string. It's one more lane, [polar], that the hammer doesn't strike.
//
// The hammer reads the strings' velocity where it strikes, from the two taps
// that are that point on the way out and on the way back, and pushes there.
namespace acidulous::machine::hammer {
using dsp::clampf;

class Course {
  public:
    static constexpr int kLanes = 4;
    static constexpr int kStages = 16;
    /** The most second-order stiffness sections a string can have, and their share the late wave passes through. */
    static constexpr int kSections = 40, kGapSections = 6;
    /** How high the sections follow the stretch, Hz, and how wide each is against its share of the band. */
    static constexpr double kSectionsTopHz = 5000.0, kSectionWidth = 1.8;
    /** Ring length in samples, a power of two longer than the lowest note's loop at 48 kHz. */
    static constexpr int kSize = 4096;
    /**
     * Cents apart per unit of bridge share (at the third partial) that
     * strings can be and still decay as one, then ring on: past about this,
     * they drift in and out of step, beat hard, and the aftersound drains
     * into the bridge (the probe at C4: 0.1 to 0.15 cents for a share of
     * 0.0037).
     */
    static constexpr float kCentsPerShare = 33.0f;
    /** The fastest the bridge takes the high end, at kHighHz (the recordings: 2.5 to 4 s in the bass). */
    static constexpr double kHighHz = 4000.0, kHighT60 = 5.0;
    /** The shortest the strings' own loss lets the high end ring, at kHighHz. */
    static constexpr double kHighAfterT60 = 15.0;
    /** Below this, a string's seventh partial is too low to fit its high end from. */
    static constexpr double kLowStringHz = 1000.0;

    /** How long it rings, T60s in seconds: the strings' own loss (after) and with the bridge's (prompt). */
    struct Decay {
        /** The aftersound at the fundamental, the third and the seventh partial. */
        float after1 = 30.0f, after3 = 15.0f, after7 = 10.0f;
        /** The prompt sound at the fundamental, the third and the seventh partial. */
        float prompt1 = 8.0f, prompt3 = 5.0f, prompt7 = 3.0f;
        bool operator==(const Decay &o) const {
            return after1 == o.after1 && after3 == o.after3 && after7 == o.after7 && prompt1 == o.prompt1 &&
                   prompt3 == o.prompt3 && prompt7 == o.prompt7;
        }
    };

    /**
     * The loss designed for one note, kept where every voice can use it, as
     * the sections are: its fitting takes most of a note-on's tuning, and a
     * voice that moves to a key with nothing changed gets the same answer.
     * What it was made from, and what it made.
     */
    struct Loss {
        bool made = false;
        float f0 = 0.0f, sr = 0.0f, highRing = 0.0f, couple = 0.0f;
        Decay decay;
        float zero = 0.0f, pole = 0.0f, gain = 0.0f, bridgeLow = 0.0f, bridgeRise = 0.0f;
    };

    /**
     * The stiffness sections designed for one note, kept where every voice
     * can use them: designing them takes 50 to 100 us, and a bass note can't
     * wait that long for its strings at every note-on (Hammer designs its
     * bass keys' as it's loaded). What they were made for, the curve, the
     * sections in order of frequency, and the late wave's share.
     */
    struct Sections {
        float hz = 0.0f, B = 0.0f, bend = 0.0f;
        int cap = 0, made = 0;
        double line = 0.0, top = 1.0, total = 0.0;
        float a1[kSections] = {}, a2[kSections] = {};
        float gapFor = 0.0f;
        int gapCount = 0;
        float gapA1[kGapSections] = {}, gapA2[kGapSections] = {};
    };

    /**
     * Something put on the strings at one point, as the force it puts on
     * them there, in the strings' own units (a force over twice their
     * impedance, a sample at a time): rubber is a [loss] and a [spring] to
     * the frame, a screw a [mass] that moves with the strings, a bolt or
     * paper a [rattle] mass that sits loose, [gap] apart, and only meets
     * them past that (with its own [rattleLoss], and paper a
     * [rattleSpring] back to where it was put). All zero is nothing.
     */
    struct Prep {
        float at = 0.3f;
        float loss = 0.0f, spring = 0.0f, mass = 0.0f;
        float rattle = 0.0f, gap = 0.0f, rattleLoss = 0.0f, rattleSpring = 0.0f;
        bool any() const { return loss > 0.0f || spring > 0.0f || mass > 0.0f || rattle > 0.0f; }
        bool operator==(const Prep &o) const {
            return at == o.at && loss == o.loss && spring == o.spring && mass == o.mass && rattle == o.rattle &&
                   gap == o.gap && rattleLoss == o.rattleLoss && rattleSpring == o.rattleSpring;
        }
    };

    struct Design {
        /** The note, Hz, and the strings the hammer strikes (with [polar], up to kLanes in all). */
        float hz = 261.6f;
        int lanes = 3;
        /**
         * How much of the bridge's motion the strings take across the board,
         * as a lane of its own (0 for none). Its tuning is unison[lanes].
         */
        float polar = 0.0f;
        /**
         * Each string's tuning off the note, in two parts: [unison] in
         * coupling widths (1 is as far apart as the bridge's share lets
         * strings be and still ring as one, so it follows the key), and
         * [detune] in cents on top, for a piano left out of tune.
         */
        float unison[kLanes] = {0.0f, 0.0f, 0.0f, 0.0f};
        float detune[kLanes] = {0.0f, 0.0f, 0.0f, 0.0f};
        /** The bridge's share against the one the decay times ask for. */
        float couple = 1.0f;
        /** Inharmonicity, and the allpass stages that make it. */
        float B = 2.5e-4f;
        int stages = 4;
        /**
         * The most second-order sections to make the stiffness from instead
         * of [stages] (0 for none). Below about 120 Hz the 30 partials the
         * stages follow end under 3.5 kHz, and above them a long string's
         * partials go flat by whole semitones: A0's 60th was 160 cents
         * under the recording's, and its top sat on a harmonic series.
         */
        int sections = 0;
        /** How much less the sections stretch the high partials than [B] says: B / (1 + bend k) at partial k. */
        float bend = 0.0f;
        /** Where to keep the sections designed for this note (null: the course's own). */
        Sections *kept = nullptr;
        /** Where to keep the loss designed for this note (null: designed each time). Not compared: it's a place, not a setting. */
        Loss *keptLoss = nullptr;
        Decay decay;
        /** Where the hammer strikes, a fraction of the string from the far end. */
        float strike = 0.12f;
        /** What's on the strings, if anything. */
        Prep prep;
        /**
         * How long the high end rings at the least, against the grand's
         * (kHighT60, kHighAfterT60): shorter, thinner strings let the top
         * go sooner.
         */
        float highRing = 1.0f;

        /** Field by field: the bytes between them (around [kept]) are anything. */
        bool operator==(const Design &o) const {
            for (int i = 0; i < kLanes; ++i) {
                if (unison[i] != o.unison[i] || detune[i] != o.detune[i]) return false;
            }
            return hz == o.hz && lanes == o.lanes && polar == o.polar && couple == o.couple && B == o.B &&
                   stages == o.stages && sections == o.sections && bend == o.bend && kept == o.kept &&
                   decay.after1 == o.decay.after1 && decay.after3 == o.decay.after3 && decay.after7 == o.decay.after7 &&
                   decay.prompt1 == o.decay.prompt1 && decay.prompt3 == o.decay.prompt3 &&
                   decay.prompt7 == o.decay.prompt7 && strike == o.strike && prep == o.prep && highRing == o.highRing;
        }
    };

    void prepare(float sampleRate) {
        sr = sampleRate;
        buffer.assign(static_cast<size_t>(kSize) * kLanes, 0.0f);
        clear();
    }

    void clear() {
        pitch = 1.0f;
        // The line (64 KB) is zeroed only as far back as the new strings
        // read, as they're tuned (keepClean).
        write = 0;
        dirty = true;
        cleanFrom = kSize;
        for (int i = 0; i < kLanes; ++i) {
            thiranState[i] = lossIn[i] = lossOut[i] = 0.0f;
            for (int s = 0; s < kStages; ++s) allpassState[s][i] = 0.0f;
            for (int s = 0; s < kSections; ++s) sectionS1[s][i] = sectionS2[s][i] = 0.0f;
            for (int s = 0; s < kGapSections; ++s) gapS1[s][i] = gapS2[s][i] = 0.0f;
        }
        bridgeBefore = 0.0f;
        for (int i = 0; i < kLanes; ++i) prepY[i] = prepV[i] = rattleY[i] = rattleV[i] = pushing[i] = pushed[i] = 0.0f;
        gapLive = 0;
        for (int i = 0; i < kLanes; ++i) {
            gapIn[i] = 0.0f;
            for (int g = 0; g < kGapStages; ++g) gapState[g][i] = 0.0f;
        }
        level = 0.0f;
    }

    /** Sets every coefficient for [d]. Not per sample: a few thousand operations. */
    void tune(const Design &d) {
        design = d;
        struck = d.lanes < 1 ? 1 : (d.lanes > kLanes ? kLanes : d.lanes);
        across = d.polar > 0.0f && struck < kLanes ? d.polar : 0.0f;
        lanes = struck + (across > 0.0f ? 1 : 0);
        for (int i = 0; i < kLanes; ++i) {
            laneOn[i] = i < lanes ? 1.0f : 0.0f;
            takes[i] = i < struck ? 1.0f : (i < lanes ? across : 0.0f);
        }
        stages = d.stages < 0 ? 0 : (d.stages > kStages ? kStages : d.stages);
        const float f0 = clampf(d.hz, 20.0f, sr * 0.45f);
        designLoss(f0, d.decay, d.keptLoss);
        // The loss's phase sharpens the upper partials a little too: the
        // stiffness makes up the rest, judged at a partial that matters for
        // the register (P B (K^2 - 1) / 2 is how much less delay a stiff
        // string's partial K wants than its fundamental, in samples).
        const int K = std::clamp(static_cast<int>(std::lround(2000.0f / f0)), 2, 10);
        const float w1 = 6.28318530718f * f0 / sr;
        const float lossDrop = lossDelay(w1) - lossDelay(w1 * static_cast<float>(K));
        const float wanted = 0.5f * (sr / f0) * d.B * static_cast<float>(K * K - 1);
        const float stiffB = d.B * std::fmax(0.0f, 1.0f - lossDrop / std::fmax(wanted, 1e-9f));
        sections = 0;
        sd = d.kept != nullptr ? d.kept : &own;
        if (d.sections > 0 && d.B > 0.0f) designSections(f0, stiffB, std::fmax(d.bend, 0.0f), std::min(d.sections, kSections));
        if (sections > 0) {
            stages = 0;
            disperse = 0.0f;
        } else {
            designStiffness(f0, stiffB);
        }
        const float share3 = bridgeAt(std::fmin(3.0f, 0.45f * sr / f0) * 6.28318530718f * f0 / sr);
        for (int i = 0; i < lanes; ++i) {
            const float cents = d.unison[i] * kCentsPerShare * share3 + d.detune[i];
            laneHz[i] = f0 * std::exp2(cents / 1200.0f);
            const float w = 6.28318530718f * laneHz[i] / sr;
            double lag = 0.0;
            for (int m = 0; m < sections; ++m) lag += sectionLag(sd->a1[m], sd->a2[m], w);
            laneStiff[i] = static_cast<float>(stages) * allpassDelay(disperse, w) + static_cast<float>(lag / w);
            solveLane(i, laneHz[i] * pitch);
        }
        // The strike point on the way out and on the way back, in samples
        // old. Striking 1/n of the way along silences partial n, and 2n, 3n
        // and so on: a stiff string's modes keep their shapes. Here the
        // stiffness is lumped in one chain, so a plain gap between the taps
        // would put only the first of those notches right (at A0 the third
        // came at the 22nd and a half, not the 24th, taking the top of the
        // strike with it). So the wave that leaves late goes through the
        // gap's share of the stiffness first, and the rest of the gap is
        // line: one period of partial n apart, as it actually sounds.
        const float half = 0.5f * static_cast<float>(delay[0]);
        const float n = 1.0f / clampf(d.strike, 0.02f, 0.5f);
        const float wn = 6.28318530718f * n * f0 * std::sqrt(1.0f + d.B * n * n) / sr;
        gapStages = std::clamp(static_cast<int>(std::lround(static_cast<float>(stages) / n)), 0, kGapStages);
        layGapSections(n);
        auto gapOf = [&]() {
            return 6.28318530718f / wn - static_cast<float>(gapStages) * allpassDelay(disperse, wn) - gapSectionsDelay(wn);
        };
        float gap = gapOf();
        while (gap < 2.0f && (gapStages > 0 || gapSections > 0)) {
            if (gapStages > 0) --gapStages; else if (--gapSections > 0) lay(gapSections, gapA1, gapA2);
            gap = gapOf();
        }
        gap = std::fmax(gap, 1.0f);
        tapOut = clampf(half - 0.5f * gap, 1.0f, static_cast<float>(delay[0]) - 2.0f);
        tapBack = clampf(half + 0.5f * gap, 1.0f, static_cast<float>(delay[0]) - 2.0f);
        // A push between two samples lands partly in the cell the same tap
        // reads the next sample: frac (1 - frac) of it, at each tap. At the
        // top, where the strike is so near the end that the taps are a
        // sample or two apart, the hammer heard its own push as the string
        // running away from it, stayed on for 4 ms instead of 0.6, and every
        // key whose taps fell on half samples came out 10 to 20 dB under its
        // neighbours. There, what it reads of its own last push is taken
        // back out. (Pushing a sample later instead made every key stick:
        // the reflection it needs to feel came late too. Further down the
        // taps are far apart and the grand was calibrated as it is.)
        {
            auto overlap = [](float age) { const float f = age - std::floor(age); return f * (1.0f - f); };
            selfRead = gap < 3.0f ? overlap(tapOut) + overlap(tapBack) : 0.0f;
        }
        // A preparation's point, the same way round: out and back, on whole
        // samples. Between two, what it pushes into the later one is read
        // back a sample on, and a light rattle fed on itself and screamed
        // (paper, from about C5 up).
        prepared = d.prep.any();
        if (prepared) {
            const float span = clampf(d.prep.at, 0.02f, 0.48f) * static_cast<float>(delay[0]);
            prepOut = std::round(clampf(half - 0.5f * span, 1.0f, static_cast<float>(delay[0]) - 2.0f));
            prepBack = std::round(clampf(half + 0.5f * span, 1.0f, static_cast<float>(delay[0]) - 2.0f));
            if (prepBack <= prepOut) prepBack = prepOut + 1.0f;
        }
        // A stiff string's highs travel faster than its lows, so they reach
        // the bridge first: at A0 the blow is heard bright about 10 ms
        // before the low part of the note arrives. With all the stiffness
        // at the bridge the blow would get there with none of it, every
        // frequency at once, so the share of the stages that goes with the
        // way from the strike point to the bridge sits in the line there.
        int shortest = delay[0];
        for (int i = 1; i < lanes; ++i) shortest = std::min(shortest, delay[i]);
        junction = static_cast<int>(std::ceil(tapBack)) + 2;
        const float way = static_cast<float>(shortest - junction) / static_cast<float>(shortest);
        junctionStages = junction < shortest - 2 ? std::clamp(static_cast<int>(std::lround(way * static_cast<float>(stages))), 0, stages) : 0;
        junctionSections = junction < shortest - 2 ? std::clamp(static_cast<int>(std::lround(way * static_cast<float>(sections))), 0, sections) : 0;
        placeSections();
        placePickups();
        arrival = static_cast<float>(shortest - junction) + static_cast<float>(junctionStages) * allpassDelay(disperse, w1) +
                  sectionsDelay(w1, 0, junctionSections);
    }

    /** The string's velocity where the hammer strikes, before this sample's push. */
    float strikeVelocity(int lane) const {
        return readAge(tapOut, lane) - readAge(tapBack, lane) - selfRead * pushed[lane];
    }

    /** Pushes a velocity wave of [dv] both ways from the strike point. */
    void push(int lane, float dv) {
        // The wave on its way back is stored as it will be after the far
        // end turns it over.
        pushing[lane] += dv;
        addAge(tapBack, lane, -dv);
        if (gapStages == 0 && gapSections == 0) {
            addAge(tapOut, lane, dv);
        } else {
            // Through the gap's share of the stiffness, in step().
            gapIn[lane] += dv;
            gapLive = kGapTail;
        }
    }

    /**
     * What the bridge's motion gives a string it doesn't belong to, [x] into
     * every struck lane as it leaves the bridge: an unplayed string ringing
     * in sympathy. Called after step().
     */
    void drive(float x) {
        for (int i = 0; i < struck; ++i) addAge(1.0f, i, x);
    }

    /** One sample: every string round its loop and through the bridge. Returns what reaches the bridge. */
    float step() {
        for (int i = 0; i < kLanes; ++i) {
            pushed[i] = pushing[i];
            pushing[i] = 0.0f;
        }
        if (prepared) stepPrep();
        // Every filter runs over all four lanes at once, a lane that isn't
        // used held at zero: four floats side by side, which the compiler
        // turns into one vector operation each.
        if (gapLive > 0) {
            --gapLive;
            float y[kLanes];
            for (int i = 0; i < kLanes; ++i) {
                y[i] = gapIn[i];
                gapIn[i] = 0.0f;
            }
            for (int g = 0; g < gapStages; ++g) {
                for (int i = 0; i < kLanes; ++i) {
                    const float z = disperse * y[i] + gapState[g][i];
                    gapState[g][i] = y[i] - disperse * z;
                    y[i] = z;
                }
            }
            for (int g = 0; g < gapSections; ++g) sections4(gapA1[g], gapA2[g], gapS1[g], gapS2[g], y);
            for (int i = 0; i < struck; ++i) addAge(tapOut, i, y[i]);
        }
        // The stiffness on the way from the strike point to the bridge.
        if (junctionStages > 0 || junctionSections > 0) {
            float *cell = &buffer[index(write - junction)];
            float y[kLanes];
            for (int i = 0; i < kLanes; ++i) y[i] = cell[i];
            for (int s = 0; s < junctionStages; ++s) {
                for (int i = 0; i < kLanes; ++i) {
                    const float z = disperse * y[i] + allpassState[s][i];
                    allpassState[s][i] = y[i] - disperse * z;
                    y[i] = z;
                }
            }
            for (int s = 0; s < junctionSections; ++s) sections4(sectionA1[s], sectionA2[s], sectionS1[s], sectionS2[s], y);
            for (int i = 0; i < kLanes; ++i) cell[i] = y[i] * laneOn[i];
        }
        // What the struck strings bring to the bridge now and next sample.
        float together = 0.0f, next = 0.0f;
        for (int i = 0; i < struck; ++i) {
            together += buffer[index(write - delay[i]) + static_cast<size_t>(i)];
            next += buffer[index(write - delay[i] + 1) + static_cast<size_t>(i)];
        }
        const float share = 1.0f / static_cast<float>(struck);
        together *= share;
        next *= share;
        // b0 + b1 (1 - cos w), looking a sample back and a sample ahead.
        const float taken = (bridgeLow + bridgeRise) * together - 0.5f * bridgeRise * (bridgeBefore + next);
        bridgeBefore = together;
        float y[kLanes] = {};
        float arriving = 0.0f;
        for (int i = 0; i < lanes; ++i) {
            // The bridge's share of what the strings do together; across the
            // board, what the bridge's motion gives.
            const float x = buffer[index(write - delay[i]) + static_cast<size_t>(i)] - takes[i] * taken;
            // Across the board, the strings move the bridge only as much as
            // the bridge moves them.
            arriving += takes[i] * x;
            // The fraction.
            y[i] = eta[i] * x + thiranState[i];
            thiranState[i] = x - eta[i] * y[i];
        }
        // The rest of the stiffness.
        for (int s = junctionStages; s < stages; ++s) {
            for (int i = 0; i < kLanes; ++i) {
                const float z = disperse * y[i] + allpassState[s][i];
                allpassState[s][i] = y[i] - disperse * z;
                y[i] = z;
            }
        }
        for (int s = junctionSections; s < sections; ++s) sections4(sectionA1[s], sectionA2[s], sectionS1[s], sectionS2[s], y);
        // The loss. Past 4 it rounds off, a tanh: a runaway can't get past
        // it, and the hardest blows in the middle just reach it. This and
        // the bridge above read each lane from its own place in the line,
        // which doesn't go four at once, so they take only the lanes used.
        float *out = &buffer[index(write)];
        for (int i = 0; i < lanes; ++i) {
            float v = lossGain * (y[i] - lossZero * lossIn[i]) + lossPole * lossOut[i];
            lossIn[i] = y[i];
            lossOut[i] = v;
            if (v > 4.0f || v < -4.0f) v = 4.0f * std::tanh(v * 0.25f);
            out[i] = v;
        }
        for (int i = lanes; i < kLanes; ++i) out[i] = 0.0f;
        write = (write + 1) & (kSize - 1);
        if (write == 0) dirty = false; // round once since clear(): every cell is this note's
        level += (std::fabs(arriving) - level) * 0.0005f;
        return arriving;
    }

    /**
     * Where two pickups sit under the strings, as shares of the string from
     * the bridge; then pickup() is what each hears.
     */
    void setPickups(float first, float second) {
        pickAt[0] = first;
        pickAt[1] = second;
        placePickups();
    }

    /** What a magnetic pickup hears, [which] of the two: the struck strings' velocity over it. */
    float pickup(int which) const {
        float v = 0.0f;
        for (int i = 0; i < struck; ++i) v += readAge(pickOut[which], i) - readAge(pickBack[which], i);
        return v;
    }

    /** A slow follower of what reaches the bridge, for retiring a voice. */
    float loudness() const { return level; }
    /** How long the blow takes to reach the bridge from where it's struck, samples. */
    float strikeToBridge() const { return arrival; }
    /** Every lane, the one across the board too. */
    int lanesUsed() const { return lanes; }
    int lanesStruck() const { return struck; }
    int delayOf(int lane) const { return delay[lane]; }
    float stiffnessCoefficient() const { return disperse; }
    /** The bridge's share at [w] radians a sample. */
    float bridgeAt(float w) const { return bridgeLow + bridgeRise * (1.0f - std::cos(w)); }

    /**
     * Re-designs the loss, for a damper coming down or lifting, and retunes
     * the line for the loss's phase, which moves with it: cheap enough per
     * block.
     */
    void setDecay(const Decay &decay) {
        designLoss(clampf(design.hz, 20.0f, sr * 0.45f), decay);
        for (int i = 0; i < lanes; ++i) solveLane(i, laneHz[i] * pitch);
    }

    /**
     * Bends the strings by [ratio] of their tuned pitch, ringing on: only the
     * lines' lengths move, the stiffness and loss stay as designed, which
     * holds over a few semitones.
     */
    void setPitch(float ratio) {
        if (ratio == pitch) return;
        pitch = ratio;
        for (int i = 0; i < lanes; ++i) solveLane(i, laneHz[i] * pitch);
    }

  private:
    static size_t index(int32_t i) { return static_cast<size_t>(i & (kSize - 1)) * kLanes; }

    /**
     * Since clear(), zeroes whatever a read [age] samples back could find of
     * the strings before: the cells behind where this note started writing.
     * Every read in the loop is within a lane's delay, so this runs as each
     * delay is set, and only zeroes what no earlier call did.
     */
    void keepClean(int age) {
        if (!dirty) return;
        const int back = age + 2 - write; // reaching this far behind cell 0
        if (back <= 0) return;
        const int from = std::max(0, kSize - back);
        if (from >= cleanFrom) return;
        std::fill(buffer.begin() + static_cast<std::ptrdiff_t>(index(from)),
                  buffer.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(cleanFrom) * kLanes), 0.0f);
        cleanFrom = from;
    }

    /** The pickups' places in the line: a wave leaving the bridge, and coming back to it. */
    void placePickups() {
        const float d = static_cast<float>(delay[0]);
        for (int p = 0; p < 2; ++p) {
            const float way = clampf(pickAt[p], 0.01f, 0.5f) * 0.5f * d;
            pickOut[p] = clampf(way, 1.0f, d - 2.0f);
            pickBack[p] = clampf(d - way, 1.0f, d - 2.0f);
        }
    }

    /**
     * The preparation, a sample: each struck string's velocity where it
     * sits, the force it answers with, pushed both ways as the hammer's is.
     * Everything is solved together with the string's own velocity this
     * sample (backward Euler), the rattle's contact too: from where the
     * string and the rattle would be with no force between them, whether
     * they meet, and the force that keeps them the gap apart. Worked out
     * from last sample's places instead, a light rattle gave energy at
     * every bounce and buzzed on for ever.
     */
    void stepPrep() {
        const Prep &p = design.prep;
        const float rigid = 1.0f + p.loss + p.spring + p.mass + kHold;
        const float heavy = p.rattle * (1.0f + p.rattleLoss) + p.rattleSpring;
        for (int i = 0; i < struck; ++i) {
            const float vin = readAge(prepOut, i) - readAge(prepBack, i);
            const float free = vin - (p.spring + kHold) * prepY[i] + p.mass * prepV[i];
            float hit = 0.0f;
            if (p.rattle > 0.0f) {
                const float rattleFree = (p.rattle * rattleV[i] - p.rattleSpring * rattleY[i]) / heavy;
                const float apart = prepY[i] + free / rigid - rattleY[i] - rattleFree;
                const float past = std::fabs(apart) - p.gap;
                if (past > 0.0f) {
                    hit = kRattleStiff * (apart > 0.0f ? past : -past) / (1.0f + kRattleStiff / rigid + kRattleStiff / heavy);
                }
                rattleV[i] = rattleFree + hit / heavy;
                rattleY[i] += rattleV[i];
            }
            const float v = (free - hit) / rigid;
            prepV[i] = v;
            prepY[i] += v;
            const float dv = v - vin;
            addAge(prepBack, i, -dv);
            addAge(prepOut, i, dv);
        }
    }

    float readAge(float age, int lane) const {
        const float back = static_cast<float>(write) - age;
        const float fl = std::floor(back);
        const float frac = back - fl;
        const auto i0 = static_cast<int32_t>(fl);
        return buffer[index(i0) + static_cast<size_t>(lane)] * (1.0f - frac) +
               buffer[index(i0 + 1) + static_cast<size_t>(lane)] * frac;
    }

    /** The adjoint of readAge: [value] shared between the two cells a read at [age] takes. */
    void addAge(float age, int lane, float value) {
        const float back = static_cast<float>(write) - age;
        const float fl = std::floor(back);
        const float frac = back - fl;
        const auto i0 = static_cast<int32_t>(fl);
        buffer[index(i0) + static_cast<size_t>(lane)] += value * (1.0f - frac);
        buffer[index(i0 + 1) + static_cast<size_t>(lane)] += value * frac;
    }

    // --- phase at a frequency (radians a sample), the delay it makes, in samples ---

    static float allpassDelay(float coef, float w) {
        // H = (c + z^-1) / (1 + c z^-1)
        const float ph = std::atan2(-std::sin(w), coef + std::cos(w)) -
                         std::atan2(-coef * std::sin(w), 1.0f + coef * std::cos(w));
        return -ph / w;
    }
    float lossDelay(float w) const {
        // H = g (1 - a z^-1) / (1 - b z^-1); in double, since with the pole
        // and zero both near 1 this is a small difference of near angles.
        const double a = lossZero, b = lossPole, wd = w;
        const double ph = std::atan2(a * std::sin(wd), 1.0 - a * std::cos(wd)) -
                          std::atan2(b * std::sin(wd), 1.0 - b * std::cos(wd));
        return static_cast<float>(-ph / wd);
    }
    /** The integer line and the Thiran fraction for a loop of exactly the period of [hz]. */
    void solveLane(int lane, float hz) {
        const float w = 6.28318530718f * hz / sr;
        const float want = sr / hz - laneStiff[lane] - lossDelay(w);
        int n = static_cast<int>(std::floor(want - 0.5f));
        n = n < 2 ? 2 : (n > kSize - 4 ? kSize - 4 : n);
        const float fracWanted = want - static_cast<float>(n);
        // A Thiran's delay at the note is close to its design delay; three
        // corrections make it exact.
        float d = clampf(fracWanted, 0.5f, 1.5f);
        for (int it = 0; it < 4; ++it) {
            const float e = (1.0f - d) / (1.0f + d);
            d = clampf(d + (fracWanted - allpassDelay(e, w)), 0.4f, 1.6f);
        }
        delay[lane] = n;
        keepClean(n);
        eta[lane] = (1.0f - d) / (1.0f + d);
    }

    /**
     * The stiffness coefficient for [B] at this note, from the table
     * tools/hammer_reference/dispersion.py solves offline (Dispersion.h):
     * read between notes and between Bs.
     */
    void designStiffness(float f0, float B) {
        namespace t = dispersion;
        if (stages == 0 || B <= 0.0f) { disperse = 0.0f; return; }
        int which = 0;
        while (which + 1 < t::kStageCounts && t::kStages[which + 1] <= stages) ++which;
        stages = t::kStages[which];
        const float key = 69.0f + 12.0f * std::log2(f0 * (48000.0f / sr) / 440.0f);
        const float x = clampf((key - t::kFirstKey) / t::kKeyStep, 0.0f, static_cast<float>(t::kKeys - 1) - 1e-3f);
        const float y = clampf((std::log10(B) - t::kFirstLogB) / t::kLogBStep, 0.0f, static_cast<float>(t::kBs - 1) - 1e-3f);
        const int xi = static_cast<int>(x), yi = static_cast<int>(y);
        const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
        const auto &c = t::kCoefficient[which];
        const float lo = c[xi][yi] + (c[xi][yi + 1] - c[xi][yi]) * fy;
        const float hi = c[xi + 1][yi] + (c[xi + 1][yi + 1] - c[xi + 1][yi]) * fy;
        disperse = lo + (hi - lo) * fx;
    }

    // --- second-order stiffness sections ---

    /** One section, (a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2) transposed, on every lane. */
    static void sections4(float a1, float a2, float *s1, float *s2, float *x) {
        for (int i = 0; i < kLanes; ++i) {
            const float y = a2 * x[i] + s1[i];
            s1[i] = a1 * (x[i] - y) + s2[i];
            s2[i] = x[i] - a2 * y;
            x[i] = y;
        }
    }

    /** A section's phase lag at [w], radians: from its poles, so it never wraps. */
    static double sectionLag(float a1, float a2, double w) {
        const double rho = std::sqrt(static_cast<double>(a2));
        const double theta = std::acos(std::clamp(-static_cast<double>(a1) / (2.0 * rho), -1.0, 1.0));
        return 2.0 * w + 2.0 * (std::atan2(-rho * std::sin(theta - w), 1.0 - rho * std::cos(theta - w)) +
                                std::atan2(rho * std::sin(theta + w), 1.0 - rho * std::cos(theta + w)));
    }

    /** The delay of the loop's sections [from] to [to] at [w], samples. */
    float sectionsDelay(float w, int from, int to) const {
        double lag = 0.0;
        for (int s = from; s < to; ++s) lag += sectionLag(sectionA1[s], sectionA2[s], w);
        return static_cast<float>(lag / w);
    }

    float gapSectionsDelay(float w) const {
        double lag = 0.0;
        for (int s = 0; s < gapSections; ++s) lag += sectionLag(gapA1[s], gapA2[s], w);
        return static_cast<float>(lag / w);
    }

    /**
     * The stiffness as second-order allpass sections, for the long strings
     * a chain of first-order stages can't follow far: a first-order stage's
     * delay is all at DC, and the stretch wants delay taken away all the way
     * up. The loop's phase is what puts partial k at k f0 sqrt(1 + B k^2);
     * less the line's (the delay the loop has at the top the sections reach,
     * [kSectionsTopHz] if [cap] of them get that far), what's left is the
     * stiffness's, and each section brings 2 pi of it: its pole sits where
     * the phase wanted passes the middle of its share, as wide as that share
     * of the band. At A0 all 40 put the partials within a few cents to the
     * 80th. With [bend], partial k is at k f0 sqrt(1 + B k^2 / (1 + bend k)).
     */
    void designSections(float f0, float B, float bend, int cap) {
        // A few cents off is the same design: the loop is tuned from the
        // sections' own phase, so the note is exact either way.
        if (std::fabs(f0 - sd->hz) > 0.005f * f0 || B != sd->B || bend != sd->bend || cap != sd->cap) {
            sd->hz = f0;
            sd->B = B;
            sd->bend = bend;
            sd->cap = cap;
            sd->gapFor = 0.0f;
            constexpr double tau2pi = 6.283185307179586;
            auto totalTo = [&](double top) { return wantedPhase(tau2pi * top / sr, groupDelayAt(top)); };
            double top = std::fmin(kSectionsTopHz, 0.42 * sr);
            if (totalTo(top) > tau2pi * cap) {
                double lo = 2.0 * f0, hi = top;
                for (int it = 0; it < 40; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    (totalTo(mid) > tau2pi * cap ? hi : lo) = mid;
                }
                top = lo;
            }
            sd->line = groupDelayAt(top);
            sd->top = tau2pi * top / sr;
            sd->total = totalTo(top);
            sd->made = std::min(cap, static_cast<int>(std::lround(sd->total / tau2pi)));
            if (sd->made >= 2) lay(sd->made, sd->a1, sd->a2); else sd->made = 0;
        }
        sections = sd->made;
    }

    /** Where partial k sits, as a multiple of f0, squared, for the sections' curve; and its slope. */
    double stretchSquare(double k) const { return k * k + sd->B * k * k * k * k / (1.0 + sd->bend * k); }
    double stretchSlope(double k) const {
        const double q = 1.0 + sd->bend * k;
        return 2.0 * k + sd->B * (4.0 * k * k * k * q - sd->bend * k * k * k * k) / (q * q);
    }
    /** Which partial (fractional) is at [hz]. */
    double partialAt(double hz) const {
        const double r = hz / sd->hz, b = std::fmax(static_cast<double>(sd->B), 1e-7);
        double k = std::sqrt((-1.0 + std::sqrt(1.0 + 4.0 * b * r * r)) / (2.0 * b));
        if (sd->bend > 0.0f) {
            for (int it = 0; it < 8; ++it) k = std::fmax(k - (stretchSquare(k) - r * r) / stretchSlope(k), 0.0);
        }
        return k;
    }
    /** The loop's group delay at [hz], samples: sr dk/df. */
    double groupDelayAt(double hz) const {
        const double k = partialAt(hz);
        if (k < 1e-9) return sr / sd->hz;
        return sr / sd->hz * 2.0 * std::sqrt(stretchSquare(k)) / stretchSlope(k);
    }
    /** The stiffness's phase at [w], with the line's [line] samples taken out. */
    double wantedPhase(double w, double line) const {
        return 6.283185307179586 * partialAt(w * sr / 6.283185307179586) - w * line;
    }

    /**
     * [n] sections for the whole of the made curve's phase, or, with fewer,
     * for that share of it (each still brings 2 pi), into [a1] and [a2].
     */
    void lay(int n, float *a1, float *a2) const {
        // Where the wanted phase reaches [phase]: Newton, kept inside a
        // bracket (the slope falls to nothing at the top).
        double from = 0.0;
        auto reach = [&](double phase) {
            double lo = from, hi = sd->top, w = 0.5 * (lo + hi);
            for (int it = 0; it < 60; ++it) {
                const double e = wantedPhase(w, sd->line) - phase;
                if (std::fabs(e) < 1e-9) break;
                (e > 0.0 ? hi : lo) = w;
                const double slope = groupDelayAt(w * sr / 6.283185307179586) - sd->line;
                double next = slope > 1e-12 ? w - e / slope : 0.5 * (lo + hi);
                if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
                w = next;
            }
            from = w;
            return w;
        };
        double edge = 0.0;
        for (int i = 0; i < n; ++i) {
            const double centre = reach((i + 0.5) * sd->total / n);
            const double next = i + 1 < n ? reach((i + 1.0) * sd->total / n) : sd->top;
            const double rho = std::exp(-0.5 * kSectionWidth * (next - edge));
            a1[i] = static_cast<float>(-2.0 * rho * std::cos(centre));
            a2[i] = static_cast<float>(rho * rho);
            edge = next;
        }
    }

    /**
     * Puts [junctionSections] of the made sections first, for the junction,
     * spread evenly over the band so they bring that share of the stiffness
     * at every frequency; the rest follow in the loop.
     */
    void placeSections() {
        int j = 0, r = junctionSections;
        for (int i = 0; i < sections; ++i) {
            const bool toJunction = (i + 1) * junctionSections / std::max(sections, 1) > i * junctionSections / std::max(sections, 1);
            const int at = toJunction && j < junctionSections ? j++ : r++;
            sectionA1[at] = sd->a1[i];
            sectionA2[at] = sd->a2[i];
        }
    }

    /**
     * The late wave's share of the stiffness, 1 / [n] of the loop's, as
     * sections of its own: a few of the loop's would bring that share only
     * near their own frequencies, and the notches past the first would
     * wander (at A2 the 25th to the 34th partials came 20 to 45 dB too
     * strong).
     */
    void layGapSections(float n) {
        gapSections = sections > 0 ? std::clamp(static_cast<int>(std::lround(sd->total / n / 6.283185307179586)), 0, kGapSections) : 0;
        if (gapSections == 0) return;
        if (n != sd->gapFor || gapSections != sd->gapCount) {
            sd->gapFor = n;
            sd->gapCount = gapSections;
            lay(gapSections, sd->gapA1, sd->gapA2);
        }
        std::copy(sd->gapA1, sd->gapA1 + gapSections, gapA1);
        std::copy(sd->gapA2, sd->gapA2 + gapSections, gapA2);
    }

    /**
     * The strings' own loss, a shelf for the aftersound's T60s at the
     * fundamental, the third and the seventh partial (a one-pole from the
     * first two if the seventh is past what the line can hold); then the
     * bridge's share, for the prompt sound's at the fundamental and the
     * seventh (the third, likewise). A T60 is a time: the loss per turn
     * follows from it and the period, so a note rings as long at any pitch.
     */
    void designLoss(float f0, const Decay &t, Loss *keep = nullptr) {
        if (keep != nullptr && keep->made && keep->f0 == f0 && keep->sr == sr && keep->highRing == design.highRing &&
            keep->couple == design.couple && keep->decay == t) {
            lossZero = keep->zero;
            lossPole = keep->pole;
            lossGain = keep->gain;
            bridgeLow = keep->bridgeLow;
            bridgeRise = keep->bridgeRise;
            return;
        }
        // In double: a long bass string loses a few parts in a hundred
        // thousand a turn, and in float the shelf's pole and zero came out on
        // top of each other near 1 with the loss far too high (C2 rang 2 s).
        const double period = static_cast<double>(sr) / f0;
        auto perTurn = [&](double t60) { return std::exp(-6.907755 * period / (sr * std::fmax(t60, 0.01))); };
        const double w1 = 6.283185307179586 * f0 / sr;
        const double h3 = std::fmin(3.0, 0.45 * sr / f0);
        const double w3 = w1 * h3, w7 = w1 * 7.0;
        const bool seventh = 7.0 * f0 < 0.45 * sr;
        const double t1 = t.after1;
        const double t3 = std::fmin(h3 >= 3.0 ? t.after3 : t.after1 + (t.after3 - t.after1) * (h3 - 1.0) / 2.0, t1);
        const double t7 = std::fmin(static_cast<double>(t.after7), t3);
        const double g1 = perTurn(t1);
        // Squared magnitude against the fundamental's, for zero a and pole b.
        // Every frequency here is one of a few, so their cosines are worked
        // out once: this runs at each note-on and each block a damper moves.
        const double cos1 = std::cos(w1), cos3 = std::cos(w3), cos7 = std::cos(w7);
        const double wHc = std::fmin(6.283185307179586 * kHighHz / sr, 3.0), cosH = std::cos(wHc);
        auto cosOf = [&](double w) {
            return w == w1 ? cos1 : w == w3 ? cos3 : w == w7 ? cos7 : w == wHc ? cosH : w == 0.0 ? 1.0 : std::cos(w);
        };
        auto mag2 = [&](double a, double b, double w) {
            const double c = cosOf(w);
            return (1.0 - 2.0 * a * c + a * a) / (1.0 - 2.0 * b * c + b * b);
        };
        auto shape = [&](double a, double b, double w) { return mag2(a, b, w) / mag2(a, b, w1); };
        const double want3 = std::fmin(1.0 - 1e-12, (perTurn(t3) / g1) * (perTurn(t3) / g1));
        // A one-pole from the first two: (1 - 2b c1 + b^2) / (1 - 2b c3 + b^2) = q^2.
        auto onePole = [&]() {
            const double c1 = std::cos(w1), c3 = std::cos(w3);
            const double qa = 1.0 - want3, qb = -2.0 * (c1 - want3 * c3);
            const double disc = qb * qb - 4.0 * qa * qa;
            return (qa > 1e-15 && disc >= 0.0) ? std::clamp((-qb - std::sqrt(disc)) / (2.0 * qa), 0.0, 0.9995) : 0.0;
        };
        // A shelf through the fundamental, the third, and [wB] at [wantB]:
        // for a pole, the zero that gives wB its loss (the shelf flattens as
        // the zero nears the pole); then the pole that gives the third its
        // own. It has to fall above the fundamental: below it there's
        // nothing to lose, and a gain at DC that's over 1 turn after turn
        // would have to be clamped, taking the note's own gain with it.
        const double dcLimit = (0.99999 / g1) * (0.99999 / g1);
        auto shelf = [&](double wB, double wantB, double &za, double &pb) {
            // For a pole, the zero that gives wB its loss, solved: with
            // N(a, w) = 1 - 2a cos w + a^2, N(a, wB) / N(a, w1) = K is a
            // quadratic in a, and of its two roots (their product is 1) the
            // one inside the circle is the one.
            const double cB = cosOf(wB), c1 = cos1;
            auto zeroFor = [&](double pole) {
                if (shape(0.0, pole, wB) >= wantB) return 0.0;
                const double K = wantB * mag2(0.0, pole, w1) / mag2(0.0, pole, wB);
                const double qa = 1.0 - K, qb = cB - K * c1;
                const double disc = qb * qb - qa * qa;
                if (std::fabs(qa) < 1e-15 || disc < 0.0) return pole;
                const double r1 = (qb - std::sqrt(disc)) / qa, r2 = (qb + std::sqrt(disc)) / qa;
                const double a = std::fabs(r1) < std::fabs(r2) ? r1 : r2;
                return std::clamp(a, 0.0, pole);
            };
            double lo = 0.0, hi = 0.9995;
            for (int it = 0; it < 32; ++it) {
                const double mid = 0.5 * (lo + hi);
                const double z = zeroFor(mid);
                if (shape(z, mid, 0.0) > dcLimit) hi = mid;
                else if (shape(z, mid, w3) > want3) lo = mid;
                else hi = mid;
            }
            pb = 0.5 * (lo + hi);
            za = zeroFor(pb);
            // Only if it does what was asked.
            const double e3 = std::fabs(shape(za, pb, w3) - want3) / std::fmax(1.0 - want3, 1e-12);
            return za < pb && e3 < 0.05 && shape(za, pb, 0.0) <= dcLimit;
        };
        // The highest the loss may go: the high end of a string keeps ringing
        // for kHighAfterT60 at kHighHz at least (a one-pole fitted at a bass
        // string's first partials took 4 kHz away at hundreds of dB/s).
        const double wH = wHc;
        const double highAfter = kHighAfterT60 * design.highRing;
        const double wantH = std::fmin(want3, (perTurn(std::fmin(highAfter, t3)) / g1) * (perTurn(std::fmin(highAfter, t3)) / g1));
        double a = 0.0, b = onePole();
        double za = 0.0, pb = 0.0;
        // On a low string the seventh partial is a few hundred hertz, and a
        // shelf fitted there rings the whole top as long as it: the
        // recordings' bass strings lose their high end at an even 8 to 22
        // dB/s from 0.5 to 8 kHz. There the shelf goes through the limit
        // instead.
        const bool lowString = 7.0 * f0 < kLowStringHz;
        if (lowString && wH > w3) {
            if (shelf(wH, wantH, za, pb)) { a = za; b = pb; }
        } else if (seventh) {
            const double want7 = std::fmin(want3, (perTurn(t7) / g1) * (perTurn(t7) / g1));
            if (shelf(w7, want7, za, pb)) { a = za; b = pb; }
        }
        if (wH > w3 && shape(a, b, wH) < wantH) {
            // Too much at the top: through the limit there instead.
            if (shelf(wH, wantH, za, pb)) { a = za; b = pb; }
            else { a = 0.0; b = 0.0; }
        }
        auto lane = [&](double w) { return std::sqrt(mag2(a, b, w)); };
        const double gain = std::fmin(g1 / lane(w1), 0.99999 / std::fmax(lane(0.0), 1e-9));
        lossZero = static_cast<float>(a);
        lossPole = static_cast<float>(b);
        lossGain = static_cast<float>(gain);
        // The bridge's share at a partial: what takes the strings together
        // from their own decay down to the prompt sound's.
        auto shareAt = [&](double w, double prompt, double after) {
            return std::clamp(1.0 - perTurn(std::fmin(prompt, after)) / std::fmax(gain * lane(w), 1e-9), 0.0, 0.5);
        };
        const double s1 = shareAt(w1, t.prompt1, t1);
        const double hHigh = seventh ? 7.0 : h3;
        const double sHigh = seventh ? shareAt(w7, t.prompt7, t7) : shareAt(w3, t.prompt3, t3);
        const double rise = (1.0 - std::cos(w1 * hHigh)) - (1.0 - std::cos(w1));
        double riseShare = rise > 1e-12 ? std::fmax(0.0, (sHigh - s1) / rise) : 0.0;
        // The rise goes as the square of frequency. Fitted at the seventh
        // partial of a bass string (200 Hz), by a few kHz it had the bridge
        // taking the strike's brightness in milliseconds - the high end dying
        // at 300 dB/s where the recordings lose 15 to 25. So it's held to what
        // they show: nothing faster than kHighT60 at kHighHz.
        const double wHigh = std::fmin(6.283185307179586 * kHighHz / sr, 3.0);
        if (wHigh > w1 * hHigh) {
            const double cap = shareAt(wHigh, kHighT60 * design.highRing, kHighT60 * design.highRing * 4.0);
            const double room = (1.0 - std::cos(wHigh)) - (1.0 - std::cos(w1));
            if (room > 1e-12) riseShare = std::fmin(riseShare, std::fmax(0.0, (cap - s1) / room));
        }
        double lowShare = std::fmax(0.0, s1 - riseShare * (1.0 - std::cos(w1)));
        lowShare *= design.couple;
        riseShare *= design.couple;
        // Never more than all of it, up to the top.
        const double most = lowShare + 2.0 * riseShare;
        if (most > 0.9) {
            lowShare *= 0.9 / most;
            riseShare *= 0.9 / most;
        }
        bridgeLow = static_cast<float>(lowShare);
        bridgeRise = static_cast<float>(riseShare);
        if (keep != nullptr) *keep = {true, f0, sr, design.highRing, design.couple, t, lossZero, lossPole, lossGain, bridgeLow, bridgeRise};
    }

    std::vector<float> buffer;
    float sr = 48000.0f;
    int32_t write = 0;
    /** Since clear(), the line still holds the old strings from [cleanFrom] up (see keepClean). */
    bool dirty = false;
    int cleanFrom = kSize;
    Design design;
    int lanes = 1, struck = 1, stages = 0;
    float across = 0.0f;
    int delay[kLanes] = {100, 100, 100, 100};
    /** The bend on the strings, as a ratio of their tuned pitch. */
    float pitch = 1.0f;
    float laneHz[kLanes] = {261.6f, 261.6f, 261.6f, 261.6f};
    float eta[kLanes] = {};
    float disperse = 0.0f;
    float lossZero = 0.0f, lossPole = 0.3f, lossGain = 0.999f;
    float bridgeLow = 0.0f, bridgeRise = 0.0f, bridgeBefore = 0.0f;
    float tapOut = 10.0f, tapBack = 20.0f;
    /** Where the stiffness between the strike point and the bridge sits, samples old, and how many stages. */
    int junction = 30, junctionStages = 0, junctionSections = 0;
    float arrival = 10.0f;
    float thiranState[kLanes] = {}, lossIn[kLanes] = {}, lossOut[kLanes] = {};
    /** The gap's share of the stiffness, for the wave that leaves late (see tune()). */
    static constexpr int kGapStages = 4;
    /** How much of its own last push the strike point reads back (see tune()), and that push, by lane. */
    float selfRead = 0.0f;
    float pushing[kLanes] = {}, pushed[kLanes] = {};
    /** How long the gap's stages ring on after the last push, samples. */
    static constexpr int kGapTail = 4800;
    int gapStages = 0, gapSections = 0, gapLive = 0;
    float gapIn[kLanes] = {}, gapState[kGapStages][kLanes] = {};
    float gapA1[kGapSections] = {}, gapA2[kGapSections] = {}, gapS1[kGapSections][kLanes] = {}, gapS2[kGapSections][kLanes] = {};
    /** The sections as designed: the course's own, or kept for it by the machine. */
    Sections own;
    Sections *sd = &own;
    /** The sections in the loop, the junction's first, and each lane's stiffness delay at its note. */
    int sections = 0;
    float sectionA1[kSections] = {}, sectionA2[kSections] = {};
    float sectionS1[kSections][kLanes] = {}, sectionS2[kSections][kLanes] = {};
    /** Per lane: whether it's used, and how much of the bridge it takes and gives (1 struck, [across] across, 0 unused). */
    float laneOn[kLanes] = {}, takes[kLanes] = {};
    float laneStiff[kLanes] = {};
    float allpassState[kStages][kLanes] = {};
    /** The preparation: where it sits (out and back), the string's motion there, and the rattle's. */
    /** How stiffly a rattle meets the strings, and a spring too weak to hear that keeps the point from drifting. */
    static constexpr float kRattleStiff = 1.0f, kHold = 1e-4f;
    bool prepared = false;
    float pickAt[2] = {0.1f, 0.25f}, pickOut[2] = {5.0f, 10.0f}, pickBack[2] = {50.0f, 45.0f};
    float prepOut = 10.0f, prepBack = 20.0f;
    float prepY[kLanes] = {}, prepV[kLanes] = {}, rattleY[kLanes] = {}, rattleV[kLanes] = {};
    float level = 0.0f;
};

} // namespace acidulous::machine::hammer
