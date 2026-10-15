#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/core/Take.h>
#include <vector>

// Where to cut one file to spread it across a set of pads.
//
// Kept out of EngineHost, its only caller, so it can be tested outside the app.
namespace acidulous::audio {

enum class SliceMode : int { Transients = 0, Even = 1 };

/**
 * [count] + 1 boundaries as fractions of the file's length, from 0 to 1 in
 * order. Slice n runs from boundary n to boundary n + 1.
 *
 * Transients use the same detector as Dice and Pollen. With enough of them,
 * each pad snaps to the strongest onset in its own stretch of the file. With
 * fewer onsets than pads it falls back to even slices.
 */
inline std::vector<float> slicePoints(const SampleData &s, SliceMode mode, int count, float sampleRate) {
    if (count < 1) count = 1;
    std::vector<float> out;
    if (s.frames <= 0) return out;

    std::vector<int32_t> cuts;
    if (mode == SliceMode::Transients) {
        Take take;
        take.frames = s.frames;
        take.left = s.left;
        take.right = s.stereo && !s.right.empty() ? s.right : s.left;
        take.detect(sampleRate);
        const std::vector<int32_t> &onsets = take.onsets;
        if (static_cast<int>(onsets.size()) >= count) {
            // Split the file into [count] equal stretches and take the
            // strongest onset in each. Taking the loudest onsets overall
            // bunches the cuts together on a long track. A stretch with no
            // onsets keeps its even cut.
            const auto window = static_cast<int32_t>(sampleRate * 0.02f);
            cuts.assign(static_cast<size_t>(count), 0);
            size_t at = 0;
            for (int i = 0; i < count; ++i) {
                const auto from = static_cast<int32_t>(static_cast<int64_t>(s.frames) * i / count);
                const auto to = static_cast<int32_t>(static_cast<int64_t>(s.frames) * (i + 1) / count);
                while (at < onsets.size() && onsets[at] < from) ++at;
                int32_t best = -1;
                float bestPeak = -1.0f;
                for (size_t j = at; j < onsets.size() && onsets[j] < to; ++j) {
                    float peak = 0.0f;
                    const int32_t until = std::min(s.frames, onsets[j] + window);
                    for (int32_t k = onsets[j]; k < until; ++k) {
                        peak = std::max(peak, std::fabs(s.left[static_cast<size_t>(k)]));
                    }
                    if (peak > bestPeak) {
                        bestPeak = peak;
                        best = onsets[j];
                    }
                }
                cuts[static_cast<size_t>(i)] = best >= 0 ? best : from;
            }
        }
    }
    if (cuts.empty()) {
        cuts.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            cuts.push_back(static_cast<int32_t>(static_cast<int64_t>(s.frames) * i / count));
        }
    }
    // The first slice always starts at 0, so nothing before the first
    // transient is lost.
    cuts[0] = 0;

    out.reserve(cuts.size() + 1);
    for (int32_t at : cuts) out.push_back(static_cast<float>(at) / static_cast<float>(s.frames));
    out.push_back(1.0f);
    return out;
}

} // namespace acidulous::audio
