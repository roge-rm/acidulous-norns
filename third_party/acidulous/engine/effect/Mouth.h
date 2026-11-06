#pragma once
#include <cstdint>
#include <engine/effect/Effect.h>
#include <engine/machine/diction/Throat.h>
#include <engine/machine/diction/Words.h>

namespace acidulous::effect {

/**
 * Diction's throat on any track: five formants and a nose, so whatever goes
 * through it is shaped into a vowel, from oo through oh, ah and eh to ee.
 *
 * `move` says what moves the vowel: an LFO on the tempo, the track's own
 * level (louder opens the mouth, like a wah that talks), or another track
 * through the sidechain. `depth` is how far it moves from `vowel`.
 *
 * `words` is how far a clip's words take the mouth: on a track insert each
 * note's sounds move it through their vowels, closing it on a stop and
 * hissing on an s, and the note's last vowel is held until it's let go.
 */
class Mouth final : public Effect {
  public:
    enum P { Vowel, Size, Nasal, Move, Rate, Depth, Mix, Gain, Sidechain, Words, Count };
    Mouth() { initParams(-5.5f); }
    const char *typeName() const override { return "Mouth"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t, float bpm) override { tick = tickStart; this->bpm = bpm; }
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    void onNote(int32_t note, float hz) override;
    void onNoteOff(int32_t note) override;
    void onAllNotesOff() override;
    void onLyric(const uint8_t *phones, int32_t count) override { speech.take(phones, count); }

    /** Whether a word is being said, and where its formants are going, Hz; for tests. */
    bool saying() const { return speech.saying; }
    float wordFormant(int k) const { return speech.now().f[k]; }
    /** Whether the word's note has been let go; for tests. */
    bool released() const { return letGo; }

  private:
    machine::diction::Throat throat[2];
    float sr = 48000.0f, bpm = 120.0f, follow = 0.0f, vowelNow = 0.5f;
    int64_t tick = 0;
    int32_t countdown = 0;
    /** The words: their steps, and the note that said them. */
    machine::diction::Words speech;
    int32_t wordNote = -1, held = 0;
    bool down[128] = {};
    /** The word's note is let go: once its end sounds have played, the mouth goes back to the move. */
    bool letGo = false;
    int32_t endLeft = -1;
    /**
     * Gliding toward the word's step: how far the words have the mouth (0
     * without any), the formants, the nose, how open the mouth is and how
     * hard it hisses.
     */
    float sayMix = 0.0f, said[3] = {730, 1090, 2440}, saidNasal = 0.0f, openGoal = 1.0f, open = 1.0f, hiss = 0.0f;
    float level = 0.0f;
    uint32_t noise = 0x9e3779b9u;
    /** The hiss: two bands of noise, as the word's step colours it, and how long they ring on once it stops. */
    machine::diction::Bandpass hissBand[2];
    float hissGain[2] = {0.0f, 0.0f};
    int32_t hissRinging = 0;
    /** Points the hiss's bands at the step the words are on. */
    void colourHiss();
};

} // namespace acidulous::effect
