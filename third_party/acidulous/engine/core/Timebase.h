#pragma once
#include <cstdint>

// An outside clock for the engine to follow. Ableton Link implements it over
// the network.
//
// The interface lives here so nothing in engine/ has to include a networking
// stack, and so tempo following and phase sync can be tested against a simple
// fake timebase.
namespace acidulous {

class Timebase {
  public:
    virtual ~Timebase() = default;

    struct State {
        /** True when there's a session to follow. False while it's off. */
        bool valid = false;
        double bpm = 120.0;
        /**
         * The session position in beats at the moment this block will be
         * heard, not when it's rendered. The two differ by the output buffer
         * length.
         */
        double beat = 0.0;
        /** Beats per bar, which phase is measured against. */
        double quantum = 4.0;
        /** True when the session is running. Only used with start/stop sync. */
        bool playing = false;
        /** Beats per block, so a caller can see where the block ends. */
        double beatsPerBlock = 0.0;
        /**
         * How many other devices are in the session. 0 doesn't mean it's
         * off: a session of one still has a tempo and phase to follow, but
         * there's no point waiting for a downbeat.
         */
        int32_t peers = 0;
    };

    /**
     * Audio thread, once a block. Must not lock, allocate or block.
     *
     * [framesRendered] is the engine's frame count at the start of this
     * block, used to work out when the block will be heard.
     */
    virtual State capture(int64_t framesRendered) = 0;

    /**
     * Audio thread: how many beats are in our bar. The engine sets this
     * because the bar comes from the song, e.g. a scene in 7/8.
     */
    virtual void setQuantum(double beats) = 0;

    /** Audio thread: offers the song's tempo to the session. */
    virtual void proposeTempo(double bpm) = 0;

    /** Audio thread: tells the session we started or stopped. */
    virtual void proposePlaying(bool playing) = 0;
};

} // namespace acidulous
