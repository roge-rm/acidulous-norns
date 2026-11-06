#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/machine/diction/Phones.h>

namespace acidulous::machine::diction {

/**
 * A note's words as steps for a mouth that isn't singing them: where the
 * formants go, the hiss, and for how long. Shared by Tongue and the Mouth
 * effect. The last vowel is held while the note is; the sounds after it
 * are said when it's let go.
 *
 * Fixed sizes, nothing allocated: take() and start() run on the audio
 * thread, from a lyric and the note-on after it.
 */
struct Words {
    static constexpr int kMaxPhones = 32;
    static constexpr int kSteps = 24;

    struct Step {
        /** The phone's own formants, Hz. */
        float f[3];
        float noise;
        float band[2][3];
        Kind kind;
        bool voiced;
        /** Samples, or -1 for held. */
        int32_t samples;
        /** Samples it would last if it weren't held at the end, or -1 for the held vowel. */
        int32_t length;
    };

    uint8_t pending[kMaxPhones] = {};
    int pendingCount = 0;
    Step steps[kSteps];
    int count = 0, at = 0, held = 0;
    int32_t left = 0;
    bool saying = false;

    void reset() {
        pendingCount = count = at = 0;
        left = 0;
        saying = false;
    }

    /** The next note's sounds, as phone codes. */
    void take(const uint8_t *phones, int32_t n) {
        pendingCount = std::clamp(n, 0, kMaxPhones);
        for (int i = 0; i < pendingCount; ++i) pending[i] = phones[i];
    }

    bool hasPending() const { return pendingCount > 0; }
    const Step &now() const { return steps[at]; }

    /**
     * Turns the pending sounds into steps and starts on the first. Most
     * consonants are quicker than in a song, so the mouth only passes
     * through them; a hiss keeps its length, to be heard.
     */
    void plan(float sampleRate) {
        int32_t tableSize = 0;
        const Phone *table = phoneTable(tableSize);
        // The held sound: the last vowel, or failing one the last sound.
        int last = -1;
        for (int i = 0; i < pendingCount; ++i) {
            const int c = pending[i];
            if (c <= 0 || c >= tableSize) continue;
            const Kind kind = table[c].kind;
            if (kind == Kind::Vowel || kind == Kind::Diphthong || last < 0) last = i;
        }
        count = 0;
        held = 0;
        const auto add = [&](const Phone &ph, const float *f, float seconds) {
            if (count == kSteps) return;
            Step &s = steps[count++];
            for (int k = 0; k < 3; ++k) s.f[k] = f[k];
            s.noise = ph.noise;
            for (int b = 0; b < 2; ++b) {
                for (int k = 0; k < 3; ++k) s.band[b][k] = ph.band[b][k];
            }
            s.kind = ph.kind;
            s.voiced = ph.voiced;
            s.samples = seconds < 0.0f ? -1 : std::max(1, static_cast<int32_t>(seconds * sampleRate));
            s.length = s.samples;
        };
        for (int i = 0; i < pendingCount; ++i) {
            const int c = pending[i];
            if (c <= 0 || c >= tableSize) continue;
            const Phone &ph = table[c];
            const bool isHeld = i == last;
            if (isHeld) held = ph.kind == Kind::Diphthong ? count + 1 : count;
            if (ph.kind == Kind::Diphthong) {
                add(ph, ph.f, isHeld ? 0.12f : ph.length * 0.5f);
                add(ph, ph.to, isHeld ? -1.0f : ph.length * 0.5f);
            } else {
                const bool vowel = ph.kind == Kind::Vowel;
                const bool hissing = ph.kind == Kind::Fricative || ph.kind == Kind::Affricate;
                add(ph, ph.f, isHeld ? -1.0f : std::fmax(0.015f, ph.length * (vowel ? 0.8f : hissing ? 1.0f : 0.6f)));
            }
        }
        // The mouth stays on the last sound.
        if (count > 0) steps[count - 1].samples = -1;
        held = std::min(held, count - 1);
        pendingCount = 0;
        at = 0;
        left = count > 0 ? steps[0].samples : 0;
        saying = count > 0;
    }

    /** The note is let go: the sounds after the held one. Returns whether the step moved. */
    bool release() {
        if (!saying || at != held || held + 1 >= count) return false;
        ++at;
        left = steps[at].samples;
        return true;
    }

    /** [frames] on: the steps move on, the held one stays. Returns whether the step moved. */
    bool advance(int32_t frames) {
        if (!saying || left < 0) return false;
        bool moved = false;
        left -= frames;
        while (left < 0 && at + 1 < count) {
            const int32_t over = -left;
            ++at;
            moved = true;
            left = steps[at].samples < 0 ? -1 : steps[at].samples - over;
            if (steps[at].samples < 0) break;
        }
        return moved;
    }
};

} // namespace acidulous::machine::diction
