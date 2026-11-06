#pragma once
#include <atomic>
#include <cstdint>
#include "Colour.h"
#include <engine/dsp/Wsola.h>
#include <engine/core/Reel.h>
#include <engine/machine/Machine.h>

// Bias, a four-track recorder that runs the length of the song.
//
// Instead of playing notes it follows the arrangement. It's told which cell
// the rack is in and how far through that cell's cycle (Machine::onScene),
// and plays whatever was recorded there.
//
// Four lanes play together so takes can be layered. Each lane's level and
// mute are ordinary parameters, so they can be automated, mapped and
// recorded like any other.
//
// With Stretch off, a take starts on the bar and plays at its own speed, so
// at another tempo it drifts by at most one cycle. Past the end of its region
// a lane goes silent, it doesn't loop.
//
// A lane can also be a comp: a stack of takes and the pieces of them to
// play, joined by short crossfades (see `Reel::Region::pieces`).
namespace acidulous::machine {

class Bias final : public Machine {
  public:
    enum P : int32_t {
        Lane1, Lane2, Lane3, Lane4,
        Mute1, Mute2, Mute3, Mute4,
        Gain,
        // The tape colour, see bias/Colour.h. It changes the output, never
        // the recordings.
        /**
         * Whether the takes follow the song's tempo.
         *
         * Off, a take starts on the bar and plays at the speed it was
         * recorded. On, each lane goes through `dsp::Wsola` at the ratio of
         * song tempo to take tempo, so it lasts as long as the cell and keeps
         * its pitch.
         */
        Stretch,
        /**
         * Plays the audio input through this track.
         *
         * It's mixed in before the inserts, so you can play a guitar through
         * the track's effects (like fx.Amp) while recording. The recording
         * stays dry because capture takes the input bus, not the rack, so the
         * effects can be changed afterwards.
         *
         * Off by default so the mic isn't open unless asked for.
         */
        Monitor,
        Hiss, HissTone, LowCut, HighCut, Bump, BumpFreq,
        Sat, Comp, Wow, Flutter, Speed, Bleed, Drop,
        Bits, Rate, Smear, Width,
        Count
    };

    Bias() { initParams(); }

    const char *typeName() const override { return "Bias"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;

    void onScene(int64_t sceneId, int64_t cycleTick, bool playing, bool clipMuted) override;
    /** The song's tempo, used for the stretch ratio. */
    void onBlock(int64_t, int64_t, float bpm) override { songBpm = bpm; }
    bool render(float *L, float *R, int32_t frames) override;

    /**
     * Renders the lanes without the tape colour, for a comp.
     *
     * The colour is applied on playback, so baking it into a comp would apply
     * it twice. Only set offline with the transport stopped.
     */
    void setColourBypass(bool on) { colourBypass = on; }

    /**
     * Slot 0 is the reel. Slot 1 is listening through a phrase (an
     * `audio::Listen`, or null to stop), which plays instead of the song
     * whether or not the transport runs, and leaves the reel alone.
     */
    void *swapObject(int32_t slot, void *object) override;

    /**
     * Frames played since listening started, or -1 when not listening. Which
     * pass is playing is this over the loop's length. Any thread.
     */
    int64_t listenPlayed() const { return listenFrames.load(std::memory_order_relaxed); }
    int64_t heldBytes() const override;
    // Frees what was mounted and not swapped back out (on the retire worker).
    ~Bias() override;

    // Bias ignores notes. What it plays depends on where the song is.
    void noteOn(uint8_t, uint8_t) override {}
    void noteOff(uint8_t) override {}
    void allNotesOff() override {}

  private:
    bias::ColourSpec colourOf();
    void renderListen(float *L, float *R, int32_t frames);
    void addInput(float *L, float *R, int32_t frames);

    bias::Colour colour;
    dsp::Biquad bleedHp[2];
    /**
     * One stretcher per lane, reset at the start of every cycle. Stereo with
     * one search for both sides so a stereo take keeps its image. A mono
     * take uses its one channel on both.
     */
    dsp::Stretcher<int16_t, 2> stretcher[audio::kReelLanes];
    float songBpm = 120.0f;
    int64_t lastCycleTick = -1;
    /** Set when a new cycle starts, the only time a stretcher may be moved. */
    bool reseed = true;
    bool colourBypass = false;
    const audio::Reel *reel = nullptr;
    const audio::Reel::Cell *cell = nullptr;
    audio::FrameCursor cursors[audio::kReelLanes];
    /** The piece each comped lane last read, so reading on finds the next without a search. */
    int32_t pieceHint[audio::kReelLanes] = {};
    int64_t cycleTick = 0;
    const audio::Listen *listen = nullptr;
    int64_t listenAt = 0;
    int32_t listenHint = 0;
    std::atomic<int64_t> listenFrames{-1};
    float sampleRate = 48000.0f;
    bool playing = false;
    bool muted = false;
};

} // namespace acidulous::machine
