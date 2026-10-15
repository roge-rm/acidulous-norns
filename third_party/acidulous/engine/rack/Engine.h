#pragma once
#include <cstdint>
#include "MasterBus.h"
#include "Rack.h"
#include "TrackPool.h"
#include <atomic>
#include <chrono>
#include <engine/core/Audition.h>
#include <engine/core/Capture.h>
#include <engine/core/Expression.h>
#include <engine/core/InputBus.h>
#include <engine/core/Handover.h>
#include <engine/core/Messages.h>
#include <engine/core/RtQueue.h>
#include <engine/core/Timebase.h>
#include <engine/core/Tuner.h>
#include <sequencer/RecordQueue.h>
#include <sequencer/SceneScheduler.h>
#include <sequencer/TickClock.h>
#include <sequencer/CaptureMarks.h>
#include <sequencer/Transport.h>

// The engine: everything the audio thread owns, and the queues the rest of
// the app uses to talk to it. renderBlock() is the audio thread's entry point.
namespace acidulous {

class Engine : public Rack::ModifiedNoteSink {
  public:
    Engine();
    ~Engine();

    void start(); // starts the retire worker. The host runs the audio stream
    void stop();

    // --- Audio thread: exactly kBlockFrames interleaved stereo frames ------------
    // in is one block of interleaved stereo from the input stream, or null
    // when no input is open. It's published on the input bus for the whole
    // block, monitored if asked, and captured if a recording is armed.
    void renderBlock(const float *inInterleaved, float *outInterleaved);

    // --- Any thread ------------------------------------------------------------------
    bool mount(const Mount &m) { return mounts.push(m); }
    bool pushMidi(const MidiMessage &m) { return midiIn.push(m); }
    bool pushClock(const MidiInEvent &e) { return clockIn.push(e); }
    bool pushParam(const ParamMessage &m) { return paramsIn.push(m); }
    /**
     * Hold while reading a machine, effect or mounted object from outside the
     * audio thread, so it isn't deleted mid-read if it's being replaced.
     */
    [[nodiscard]] Retirer::ReadGuard readLive() { return Retirer::ReadGuard(retirer); }

    seq::Transport transport;
    seq::TickClock clock;
    seq::SceneScheduler scheduler;
    seq::RecordQueue recordQueue;
    /** Notes and clock going to hardware. Drained by the MIDI sender. */
    MidiOutQueue midiOut;
    /** Realtime bytes from an external clock, stamped with when they arrived. */
    MidiClockQueue clockIn;
    seq::ClockFollower follower;

    /**
     * A network timebase (Link) or null. Set by the host before Link is
     * switched on and cleared after. The audio thread only reads it, and only
     * uses it when the transport says Link controls the tempo.
     */
    std::atomic<Timebase *> timebase{nullptr};
    /** Whether a peer starting or stopping starts and stops us too. */
    std::atomic<bool> syncStartStop{true};
    MasterBus master;
    Rack racks[kRackCount];

    // --- MPE ----------------------------------------------------------------
    /**
     * The MPE zone. [kind] is 0 off, 1 lower (master channel 1, members from
     * 2 up), 2 upper (master 16, members from 15 down).
     *
     * One zone for the engine rather than per rack, because it's a property of
     * the controller, and notes follow whichever track is open.
     */
    void setMpeZone(int32_t kind, int32_t members, float bendSemis) {
        mpeKind = kind < 0 ? 0 : (kind > 2 ? 2 : kind);
        mpeMembers = members < 1 ? 1 : (members > 15 ? 15 : members);
        mpeBendSemis = bendSemis < 1.0f ? 1.0f : (bendSemis > 96.0f ? 96.0f : bendSemis);
        // Held notes are kept, since a zone switched on mid-chord (auto, from
        // the second finger) needs to know where the first one is.
        for (auto &ch : lastExprSent) for (float &v : ch) v = -1.0f;
    }
    bool mpeMember(uint8_t channel) const {
        if (mpeKind == 0 || channel > 15) return false;
        if (mpeKind == 1) return channel >= 1 && channel <= mpeMembers;
        return channel <= 14 && channel >= 15 - mpeMembers;
    }
    /** Which member channels are holding a note, one bit per channel. */
    int32_t mpeHeldMask() const {
        int32_t mask = 0;
        for (int32_t c = 0; c < 16; ++c) if (mpeChannelNote[c] >= 0) mask |= 1 << c;
        return mask;
    }
    Retirer retirer;

    /**
     * Where a block's time went, roughly. Five phases rather than per rack,
     * since that's enough to tell which part of the engine is slow.
     */
    enum class Phase : int32_t { Input, Sequencer, Racks, Master, Capture, Count };
    static constexpr size_t kPhases = static_cast<size_t>(Phase::Count);

    float loadPercent() const { return load.load(std::memory_order_relaxed); }

    /**
     * The worst block since the last read, in microseconds. Reading clears it,
     * like the output meter. load above is heavily smoothed and hides spikes,
     * so this is for finding dropouts.
     */
    int32_t worstBlockUs() { return blockPeak.exchange(0, std::memory_order_relaxed); }

    /**
     * How often a block is interrupted rather than slow, 0 to 100. The peaks
     * are only taken from uninterrupted blocks. High here means the fix is
     * priority and scheduling. Low here with a high worst block means the fix
     * is the DSP.
     *
     * Below: the worst single rack in microseconds (cleared by reading), and
     * whether it was playing frozen audio when it set that peak. Read
     * worstRackWasFrozen before worstRackUs, which clears.
     */
    float interruptedPercent() const { return interruptedPct.load(std::memory_order_relaxed); }

    bool worstRackWasFrozen(int32_t rack) const {
        return rack >= 0 && rack < kRackCount && rackPeakFrozen[rack].load(std::memory_order_relaxed);
    }

    int32_t worstRackUs(int32_t rack) {
        return rack >= 0 && rack < kRackCount ? rackPeak[rack].exchange(0, std::memory_order_relaxed) : 0;
    }

    /**
     * What a rack costs as a percentile rather than its worst moment.
     *
     * Peaks over a song vary a lot between runs (3% to 26% per track on the
     * same phone and build), so real improvements get lost in the noise. A
     * high percentile needs 1% of blocks to agree before it moves, so one
     * interrupt or scene change can't set it. Each rack keeps a histogram with
     * eight buckets per octave (about 9% each).
     *
     * Only counts blocks where the rack did something, so a track playing in
     * one scene reports what it costs while playing.
     *
     * Not cleared by reading. Use resetRackCosts to reset.
     */
    int32_t rackPercentileUs(int32_t rack, int32_t perMille = 990) const;
    /**
     * The key signal an effect on rack [self] hears from rack [source]: null
     * for its own input (no source, or itself), silence for an empty rack, and
     * otherwise that rack's pre-fader tap: this block's if [source] renders
     * before [self] (see renderOrder), last block's if not. [self] is -1 for
     * a send, which hears this block's.
     */
    const float *keyFor(int32_t source, int32_t self) const;
    /**
     * One rack's share of a block, a job on its own: the events held for it,
     * its keys, its render, and what it cost. Every rack it listens to has
     * rendered by then, except across a loop, where it hears the last block.
     */
    void renderRack(int32_t r, int32_t *usOut, bool *frozenOut);
    /** Each rack's place in this block's order (renderOrder), for keyFor. */
    int32_t renderPlace[kRackCount]{};
    /**
     * Starts [workers] threads that render tracks beside the audio thread, or
     * none for every track on the audio thread. Not while blocks are being
     * rendered.
     */
    void setWorkers(int32_t workers, void (*onStart)(int32_t) = nullptr) { pool.start(workers, onStart); }
    int32_t workers() const { return pool.workers(); }
    /** How many of the workers take tracks; the rest sleep. Any thread. */
    void setActiveWorkers(int32_t n) { pool.setActive(n); }
    int32_t activeWorkers() const { return pool.activeWorkers(); }
    /** The longest the audio thread waited for a worker's track since the last read, in microseconds. Reading clears it. */
    int32_t readWorkerWaitPeakUs() { return workerWaitPeakUs.exchange(0, std::memory_order_relaxed); }
    std::atomic<int32_t> workerWaitPeakUs{0};
    /** The workers that render tracks beside the audio thread (see TrackPool). */
    TrackPool pool;
    // This block's jobs: the rack each renders, and what it cost and whether it
    // played frozen, written by whichever thread rendered it.
    int32_t jobRack[kRackCount]{};
    int32_t blockRackUs[kRackCount]{};
    bool blockRackFrozen[kRackCount]{};
    /** Last block's cost of each rack, to start the heaviest first. */
    int32_t lastRackUs[kRackCount]{};
    /**
     * Below this many microseconds of tracks last block, every track renders
     * on the audio thread and the workers sleep. 0 always wakes them.
     */
    int32_t wakeFloorUs = 200;
    static void rackJob(void *engine, int32_t job);
    /**
     * The order racks render this block, with every sidechain source before its
     * listeners. Groups are in the master, after every rack, so they don't
     * need a place.
     */
    void sidechainOrder(int32_t *order) const;
    /** Where each rack's output goes this block (see Rack::routedTo). */
    void settleRouting();

    void resetRackCosts();

    /**
     * What this rack has cost lately, in microseconds. Not cleared by reading.
     * It decays a fixed amount per block on the audio thread, so the grid and
     * the settings page can both read it without flickering.
     */
    int32_t rackCostUs(int32_t rack) const {
        return rack >= 0 && rack < kRackCount ? rackRecent[rack].load(std::memory_order_relaxed) : 0;
    }

    /** The same for the five phases of a block. Cleared by reading. */
    int32_t worstPhaseUs(Phase p) {
        const auto i = static_cast<size_t>(p);
        return i < kPhases ? phasePeak[i].exchange(0, std::memory_order_relaxed) : 0;
    }

    // Input, monitoring and recording. Set on the UI thread and read on the
    // audio thread. Plain atomics since they're single values.
    std::atomic<float> inputGain{1.0f};
    /**
     * The tuner, which listens to the input before anything else.
     *
     * Before the gain and input chain on purpose, since a gate or amp there
     * could cut off a quiet string or bury the fundamental. Off until the
     * record window asks for it, and then push returns straight away.
     */
    audio::Tuner tuner;
    /**
     * Which notes the swing moves: Swing::kSixteenths or kEighths.
     *
     * Song-wide because it's the song's feel. The amount is per track on the
     * channel strip.
     */
    std::atomic<int64_t> swingPair{seq::Swing::kSixteenths};
    std::atomic<float> monitorLevel{0.0f};
    /**
     * The input chain: effects on the incoming audio before anything else sees
     * it, including the recorder, the monitor, and every machine that reads the
     * input bus.
     *
     * Before InputBus::publish so the effects are recorded into the take
     * without the capture knowing anything about them.
     */
    Effect *inputFx[kInputSlots] = {nullptr, nullptr};

    Capture capture;
    /**
     * Where the song was during the capture (see CaptureMarks).
     *
     * Needs a rack, since in clip mode every rack is somewhere different.
     * kNoRack means nothing is armed and nothing is stamped.
     */
    static constexpr int32_t kNoRack = -1;
    seq::CaptureMarks marks;
    std::atomic<int32_t> armedRack{kNoRack};
    /** Playing one file to hear what it is (see engine/core/Audition.h). */
    Audition audition;

    /**
     * Stop everything now. Set from any thread and acted on at the next block
     * boundary, the only safe moment to reset machines the audio thread uses.
     */
    std::atomic<bool> panicFlag{false};
    // Frames left of a count-in, and the frames per tick it was measured at,
    // so the screen can show it in ticks.
    double countInFrames = 0.0;
    double countInPerTick = 0.0;

    /**
     * Armed, running, and past the count-in.
     *
     * transport.isRecording() is just playing and armed. During a count-in the
     * transport is playing but the scheduler hasn't started, so notes would be
     * recorded at the wrong position.
     */
    bool recordingNow() const { return transport.isRecording() && countInFrames <= 0.0; }
    /**
     * The last moments of the count-in, where a note is early rather than
     * unwanted.
     *
     * People counted in tend to play slightly before the beat, and the take
     * should start with that note. Otherwise the first chord of every live
     * take is lost.
     *
     * A thirty-second note before the downbeat, so it scales with tempo.
     */
    bool countInPreRoll() const {
        return transport.isRecording() && countInFrames > 0.0 && countInFrames <= preRollFrames;
    }
    /**
     * An early note, held until the scheduler has started.
     *
     * During the count-in the scheduler isn't running, so there's no scene to
     * file it under. Kept here for at most a thirty-second note, then pushed
     * with tick 0 once the scene is known.
     */
    struct EarlyNote {
        int32_t rack = 0;
        uint8_t status = 0, d1 = 0, d2 = 0;
    };
    static constexpr int32_t kMaxEarlyNotes = 16;
    static constexpr int32_t kPreRollTicks = kPPQN / 8; // a thirty-second
    EarlyNote earlyNotes[kMaxEarlyNotes];
    int32_t earlyCount = 0;
    double preRollFrames = 0.0;
    /** The song was started by a note while armed (Transport::startsOnNote); the notes in earlyNotes go at its start. */
    bool noteStart = false;
    /** Recording: since when, and with Transport::recordsOnce the tick it stops at (-1 not known yet). */
    bool wasRecording = false;
    int64_t recordFrom = 0, passEnd = -1;
    /** Notes when recording starts, and stops it after one pass if asked to. */
    void endPassIfDone();
    float inputScratch[kBlockFrames * 2] = {};
    /** The input chain, on the block about to be published. */
    void runInputChain();
    void onModifiedNote(int32_t rack, uint8_t status, uint8_t d1, uint8_t d2) override;

  private:
    void applyMounts();
    void applyMount(const Mount &m);
    /** The effect in a slot, by its unit (and rack, for a track insert), or null. */
    Effect *effectAt(int32_t rack, Unit unit);
    static constexpr int32_t kMaxMountsPerBlock = 8;
    void drainMidi();
    /** A member channel's expression, recorded against the note it belongs to. */
    void recordExpression(int32_t rack, uint8_t channel, uint8_t note, uint8_t status, uint8_t d1, uint8_t d2);
    /** The last value each member channel recorded, per curve. -1 for none yet. */
    float lastExprSent[16][3] = {};

    // Which note each member channel is holding, or -1. MPE puts one note on
    // a channel at a time, so this is exact. It's how a bend on channel 4
    // finds the finger that made it.
    int32_t mpeChannelNote[16] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                  -1, -1, -1, -1, -1, -1, -1, -1};
    int32_t mpeKind = 0;
    int32_t mpeMembers = 15;
    float mpeBendSemis = 48.0f;
    void drainParams();
    /** Apply a rack parameter: move it, and record it if it's a gesture while recording. */
    void applyRackParam(const ParamMessage &p);
    /** What's waiting for a bar line on each rack (see ParamMessage::quantise). */
    struct PendingParam {
        bool waiting = false;
        ParamMessage message;
        int64_t due = 0; // in clock ticks
    };
    PendingParam pendingParams[kRackCount];

    SharedQueue<Mount, 64> mounts;
    SharedQueue<MidiMessage, 256> midiIn;
    SharedQueue<ParamMessage, 512> paramsIn;

    void emitClock(int64_t blockStartTick, int64_t blockEndTick);
    void drainClockIn();
    void followExternal();
    void followTimebase();
    void emitTransport(bool nowPlaying);

    bool playing = false;
    bool startPending = false;
    /** Play was pressed and we're waiting for the session's downbeat. */
    bool linkWaiting = false;
    /** Whether anyone else was in the session at the last capture. */
    bool linkInSession = false;
    /** What we last told the session we were doing, to detect changes. */
    bool linkToldPlaying = false;
    /** What the session was last seen doing, and whether we've looked yet. */
    bool linkSawPlaying = false;
    bool linkSeen = false;
    /** Frames the engine has produced since the stream started. Same timeline
     *  the audio stream presents on, so a MIDI event's frame can be turned
     *  into a clock time. */
    int64_t framesRendered = 0;
    /** Frames played since the song started (after any count-in); kept after a stop. */
    int64_t playedFrames = 0;
    bool timing = false;
    int64_t lastClockTick = -1;
    std::atomic<float> load{0.0f};
    std::atomic<int32_t> blockPeak{0};
    std::atomic<int32_t> phasePeak[kPhases]{};
    /**
     * Wall time beyond CPU time that still counts as an uninterrupted block.
     * Not 0, because the clock reads, cache misses and small interrupts cost a
     * little. 50 us is 4% of a block at 48k, well below a deschedule and well
     * above the noise.
     */
    static constexpr int32_t kPreemptedUs = 50;
    std::atomic<float> interruptedPct{0.0f};
    std::atomic<int32_t> rackPeak[kRackCount]{};
    /**
     * Eight buckets per octave, up to about 4 ms. The index is the leading
     * bit's position plus the three bits under it, so it's a clz and two
     * shifts with no log on the audio thread.
     */
    static constexpr int32_t kCostBuckets = 96;
    std::atomic<int32_t> rackHist[kRackCount][kCostBuckets]{};

    /** Which bucket a cost falls in, and the cost a bucket stands for. */
    static int32_t bucketOf(int32_t us) {
        if (us <= 0) return 0;
        const auto v = static_cast<uint32_t>(us);
        const int32_t oct = 31 - __builtin_clz(v);
        const int32_t sub = oct >= 3 ? static_cast<int32_t>((v >> (oct - 3)) & 7u)
                                     : static_cast<int32_t>((v << (3 - oct)) & 7u);
        const int32_t idx = oct * 8 + sub;
        return idx < kCostBuckets ? idx : kCostBuckets - 1;
    }
    static int32_t usOf(int32_t bucket) {
        const int32_t oct = bucket / 8, sub = bucket % 8;
        // The top of the bucket, so a reading is never lower than what was seen.
        const int32_t top = 8 + sub + 1;
        return oct >= 3 ? (top << (oct - 3)) : (top >> (3 - oct));
    }
    std::atomic<bool> rackPeakFrozen[kRackCount]{};
    std::atomic<int32_t> rackRecent[kRackCount]{};

    /**
     * A peak that decays by itself: max(now, previous * k). 249/250 a block
     * falls to a third in about half a second, slow enough to see and fast
     * enough to follow a scene change.
     */
    static void keepDecaying(std::atomic<int32_t> &slot, int32_t us) {
        const int32_t was = slot.load(std::memory_order_relaxed);
        const int32_t faded = static_cast<int32_t>(static_cast<int64_t>(was) * 249 / 250);
        slot.store(us > faded ? us : faded, std::memory_order_relaxed);
    }

    /** Raise a peak-hold to [us] if it's higher. Relaxed, since nothing depends on its order. */
    static void keepPeak(std::atomic<int32_t> &slot, int32_t us) {
        int32_t seen = slot.load(std::memory_order_relaxed);
        while (us > seen && !slot.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
        }
    }
};

} // namespace acidulous
