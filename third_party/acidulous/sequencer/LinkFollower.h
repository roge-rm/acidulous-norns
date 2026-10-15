#pragma once
#include <cmath>
#include <cstdint>
#include <engine/core/Constants.h>
#include <engine/core/Timebase.h>

// The maths for following a Link session, kept apart from the network so it
// can be tested.
//
// Link already gives a smooth tempo and beat, so unlike the MIDI clock
// follower there's no loop here. What's left is the easy-to-get-wrong part:
// the phase error has to wrap the short way round, the correction has to be
// gentle enough not to be heard, and a downbeat has to be caught in the block
// it falls in. The engine passes in where it is and does what this says.

namespace acidulous::seq {

class LinkFollower {
  public:
    /**
     * How much to adjust the tempo per tick of error. At 0.002 a full
     * eight-tick error changes the rate by 1.6%, which closes a third of a
     * beat over a bar or two without audible wobble.
     */
    static constexpr double kPull = 0.002;
    /** No more than this many ticks of error are acted on at once. */
    static constexpr double kMaxPullTicks = 8.0;

    struct Advice {
        /** Run the clock at this. */
        double framesPerTick = 0.0;
        /** The phase error in ticks: ours minus theirs, the short way round. */
        double errorTicks = 0.0;
        /** A downbeat falls inside this block, so a waiting transport starts. */
        bool downbeat = false;
    };

    /**
     * Whether pressing play waits for the session's next downbeat. Only when
     * there are peers. Alone, waiting up to a bar gains nothing and just looks
     * like play not working, and Link is often left on with no one else there.
     */
    static bool waitsForDownbeat(const Timebase::State &s) { return s.valid && s.peers > 0; }

    /**
     * [ourTickInBar] is where we are in our bar, [barTicks] how long the bar
     * is, and [framesPerTickAtTempo] how long a tick is at the session's
     * tempo. [pulling] is false when the phase shouldn't be corrected (stopped
     * or waiting to start), so only the tempo is followed.
     */
    static Advice advise(const Timebase::State &s,
                         double ourTickInBar,
                         double barTicks,
                         double framesPerTickAtTempo,
                         bool pulling) {
        Advice out;
        out.framesPerTick = framesPerTickAtTempo;
        if (!s.valid || s.quantum <= 0.0 || barTicks <= 0.0) return out;

        // Does the session's bar line fall inside this block? Checks the whole
        // block's span, not a single instant, so no downbeat is missed.
        const double endBeat = s.beat + s.beatsPerBlock;
        out.downbeat = std::floor(s.beat / s.quantum) != std::floor(endBeat / s.quantum);

        if (!pulling) return out;

        // Both phases as a fraction of a bar, so a 7/8 bar and a 4/4 quantum
        // still compare. Wrapped so 0.02 and 0.98 are 0.04 apart, not 0.96.
        const double ours = wrap01(std::fmod(ourTickInBar, barTicks) / barTicks);
        const double theirs = wrap01(std::fmod(s.beat, s.quantum) / s.quantum);
        double err = ours - theirs;
        if (err > 0.5) err -= 1.0;
        if (err < -0.5) err += 1.0;
        out.errorTicks = err * barTicks;

        const double pull = out.errorTicks > kMaxPullTicks
                                ? kMaxPullTicks
                                : (out.errorTicks < -kMaxPullTicks ? -kMaxPullTicks : out.errorTicks);
        // A positive error means we're ahead, so our ticks get longer.
        out.framesPerTick = framesPerTickAtTempo * (1.0 + pull * kPull);
        return out;
    }

  private:
    static double wrap01(double v) { return v < 0.0 ? v + 1.0 : v; }
};

} // namespace acidulous::seq
