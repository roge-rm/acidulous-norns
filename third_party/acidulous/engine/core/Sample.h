#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A decoded sample at the engine rate. Built on a normal thread, mounted into a
// machine's slot, never modified afterwards.
namespace acidulous {

struct SampleData {
    std::string name;
    std::vector<float> left;
    std::vector<float> right; // empty when mono
    int32_t frames = 0;
    bool stereo = false;
    /**
     * The rate the data is at. WavReader resamples to the engine rate. A
     * multisample keeps its source rate and playback handles the ratio, so
     * its loop points stay exact.
     */
    int32_t rate = 48000;
    // Loop in frames, -1 for none. Set from the source file if it has one.
    int32_t loopStart = -1;
    int32_t loopEnd = -1;
    /**
     * The loudest sample in the file, 0..1, or 0 if it wasn't measured.
     * Stored here because a panel polls it twice a second and the data never
     * changes after it's built.
     */
    float peak = 0.0f;

    /**
     * True when the file was longer than the decoder allows, so the UI can
     * tell the user only part of it was loaded.
     */
    bool truncated = false;

    /** Fill [peak]. Call once, on the thread that built the data. */
    void measure() {
        float p = 0.0f;
        for (float v : left) p = v < 0.0f ? (-v > p ? -v : p) : (v > p ? v : p);
        for (float v : right) p = v < 0.0f ? (-v > p ? -v : p) : (v > p ? v : p);
        peak = p;
    }
};

} // namespace acidulous
