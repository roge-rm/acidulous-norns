#include "Take.h"
#include <algorithm>
#include <cmath>

namespace acidulous::audio {

void Take::detect(float sampleRate) {
    onsets.clear();
    if (frames <= 0) return;
    OnsetFinder finder;
    finder.reset(sampleRate);
    for (int32_t i = 0; i < frames; ++i) {
        const float l = left[static_cast<size_t>(i)];
        const float r = right.empty() ? l : right[static_cast<size_t>(i)];
        if (finder.push(l, r)) onsets.push_back(i);
        if (onsets.size() >= 512) break; // plenty for a cloud
    }
    bars = guessBars(sampleRate);
}

/**
 * Guesses how many bars a loop is from its length and where its hits fall.
 *
 * A loop is trimmed to whole bars, so its length only allows a few tempos
 * (four seconds is two bars at 120, one at 60 or four at 240). Of those
 * between 70 and 180, the one whose sixteenth grid the hits land on wins.
 * Hits on a coarse grid are also on a finer one, so ties go to the tempo
 * nearest 120. The user can fix a wrong guess with the bars knob.
 */
float Take::guessBars(float sampleRate) const {
    if (frames <= 0 || sampleRate <= 0.0f) return 0.0f;
    static const float kCandidates[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    const double seconds = static_cast<double>(frames) / sampleRate;
    float best = 0.0f;
    double bestScore = -1e9;
    for (const float b : kCandidates) {
        const double bpm = b * 4.0 * 60.0 / seconds;
        if (bpm < 70.0 || bpm >= 180.0) continue;
        // The share of hits within a fifth of a sixteenth of the grid. Onsets
        // can be reported up to 5 ms late, which is well inside that.
        double onGrid = 1.0;
        if (!onsets.empty()) {
            const double step = static_cast<double>(frames) / (b * 16.0);
            int32_t near = 0;
            for (const int32_t at : onsets) {
                const double off = std::fmod(static_cast<double>(at), step);
                if (std::min(off, step - off) < step * 0.2) ++near;
            }
            onGrid = static_cast<double>(near) / static_cast<double>(onsets.size());
        }
        const double score = onGrid - 0.1 * std::fabs(std::log2(bpm / 120.0));
        if (score > bestScore) { bestScore = score; best = b; }
    }
    return best;
}

} // namespace acidulous::audio
