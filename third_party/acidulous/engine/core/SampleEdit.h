#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <string>

// Offline edits to a recording: crop, fade, level, filter and so on.
//
// Not on the audio thread. A file is read with WavReader, changed here and
// written with WavWriter. These are plain functions over `SampleData` so the
// harness in tools/ can test them without an engine.
//
// The operations always run in this order: crop first so the rest has less
// audio to work on, then reverse, filters, compressor and level, and the
// fades last so the ends still reach zero.
namespace acidulous::audio {

/**
 * One edit, as the screen describes it.
 *
 * Every field does nothing at its default, so an empty `SampleOps` is a copy.
 * That lets the screen pass the whole struct every time.
 */
struct SampleOps {
    /** Keep [from] until [to], in frames. `to <= from` means to the end. */
    int32_t from = 0;
    int32_t to = 0;
    float fadeInMs = 0.0f;
    float fadeOutMs = 0.0f;
    /** Level, in decibels, applied before the peak is normalised. */
    float gainDb = 0.0f;
    /** Brings the loudest sample to this, 0..1. 0 leaves the level alone. */
    float normaliseTo = 0.0f;
    bool reverse = false;
    /** High pass, e.g. for room rumble under a voice. 0 is off. */
    float lowCutHz = 0.0f;
    /** The tone filter. 0 is off. The type is a dsp::MultiFilter::Type. */
    float cutoffHz = 0.0f;
    float resonance = 0.0f;
    int32_t filterType = 0;
    /**
     * Compressor amount, 0..1, plus attack and release. One knob sets both
     * threshold and ratio: 0 is off, and 1 is -24 dB at 8:1, with makeup gain
     * that keeps the peak where it was.
     */
    float squash = 0.0f;
    float squashAttackMs = 10.0f;
    float squashReleaseMs = 120.0f;
};

/** Applies everything in the fixed order. Returns false with [error] set on failure. */
bool applyEdit(SampleData &data, const SampleOps &ops, std::string &error);

// --- each step on its own, for testing ---------------------------------------

/** Keeps [from] until [to]. Clamped. An inverted range is left alone. */
void cropTo(SampleData &data, int32_t from, int32_t to);
/** Linear fades over this many frames at each end. */
void fadeEnds(SampleData &data, int32_t inFrames, int32_t outFrames);
/** Multiply. */
void applyGain(SampleData &data, float linear);
/** Scale so the loudest sample is [peak]. Silence is left silent. */
void normalisePeak(SampleData &data, float peak);
void reverseInPlace(SampleData &data);
/** One pass of `dsp::MultiFilter`, per channel, at the data's own rate. */
void filterInPlace(SampleData &data, float cutoffHz, float resonance, int32_t type);
/** See `SampleOps::squash`. */
void compressInPlace(SampleData &data, float amount, float attackMs, float releaseMs);

} // namespace acidulous::audio
