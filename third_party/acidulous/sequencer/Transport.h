#pragma once
#include <atomic>
#include <cstdint>
#include <engine/core/Constants.h>

// Play/stop state and the handshake that moves it between threads. The UI
// requests and the audio thread applies at a block boundary, so state never
// changes in the middle of a render.

namespace acidulous::seq {

class Transport {
  public:
    Transport() {
        // A zeroed slot means "playing the first scene", not "nothing", so
        // start every rack at kLaunchIdle until the launcher has run.
        for (auto &v : launchForUi) {
            v.store(kLaunchIdle, std::memory_order_relaxed);
        }
    }

  public:
    enum class Request : uint8_t { None, Play, Stop, Continue };
    static constexpr int32_t kCurrentScene = -1;

    // --- UI thread ------------------------------------------------------------
    // Play from the top of `sceneIdx`, or of whichever scene the scheduler is
    // on when kCurrentScene.
    void requestPlay(int32_t sceneIdx = kCurrentScene) {
        startScene.store(sceneIdx, std::memory_order_relaxed);
        request.store(Request::Play, std::memory_order_release);
    }
    void requestStop() { request.store(Request::Stop, std::memory_order_release); }

    /**
     * Back to the top of the song, without starting it.
     *
     * This is a separate flag because a Request is skipped when the
     * transport is already in the asked-for state, and a rewind asks for
     * "not playing" while already stopped. Play resets the clock itself, and
     * Stop leaves the playhead where it stopped.
     */
    void requestRewind() { rewindFlag.store(true, std::memory_order_release); }

    /**
     * Carry on from where the playhead is, rather than from the top of a
     * scene. Used for MIDI Continue (0xFB), since Play resets the clock and
     * restarts the scene.
     */
    void requestContinue() { request.store(Request::Continue, std::memory_order_release); }
    bool takeContinued() { return continued.exchange(false, std::memory_order_relaxed); }

    /**
     * Let the current scene finish its repeats and then stop. Cleared by
     * turning it off, by any play or stop request, or by the scheduler when
     * it fires.
     */
    void setStopAtEnd(bool on) {
        stopAtEndFlag.store(on, std::memory_order_relaxed);
        if (on) queuedScene.store(-1, std::memory_order_relaxed); // the two are alternatives
    }
    bool stopAtEndArmed() const { return stopAtEndFlag.load(std::memory_order_relaxed); }

    /**
     * Queue a scene to start when the current one has finished its repeats.
     * -1 cancels. Queuing a scene and stopping at the end are alternatives,
     * so each clears the other.
     */
    void queueScene(int32_t idx) {
        queuedScene.store(idx, std::memory_order_relaxed);
        if (idx >= 0) stopAtEndFlag.store(false, std::memory_order_relaxed);
    }
    int32_t queuedSceneIndex() const { return queuedScene.load(std::memory_order_relaxed); }

    // Two loop toggles: the transport button loops the current scene; the one
    // beside Add Scene loops the whole song.
    void setLoopScene(bool on) { loopSceneFlag.store(on, std::memory_order_relaxed); }

    // REC arms recording: while armed, playing (or starting to play) records.
    void setRecordArmed(bool on) { recordArmed.store(on, std::memory_order_relaxed); }
    bool isRecordArmed() const { return recordArmed.load(std::memory_order_relaxed); }
    /**
     * How a take starts and ends: with [onNote], armed and stopped, the first
     * note played starts the song and is recorded at its start; with [once],
     * recording stops by itself after one pass of the clip.
     */
    void setRecordModes(bool onNote, bool once) {
        recordOnNote.store(onNote, std::memory_order_relaxed);
        recordOnce.store(once, std::memory_order_relaxed);
    }
    bool startsOnNote() const { return recordOnNote.load(std::memory_order_relaxed); }
    bool recordsOnce() const { return recordOnce.load(std::memory_order_relaxed); }
    void setLoopSong(bool on) { loopSongFlag.store(on, std::memory_order_relaxed); }

    // --- Clip mode ------------------------------------------------------------
    // Same idea as queueScene: the UI stores, the audio thread exchanges at a
    // boundary. One slot per rack, since in clip mode every rack has its own
    // next clip.

    /**
     * Who owns the tempo: off, MIDI clock or Link. Only one at a time.
     *
     * Everything that sets a tempo checks `externalSync()` first, including
     * the four places in the scheduler, so a new source only has to set this.
     */
    enum Sync : int32_t { SyncOff = 0, SyncMidi = 1, SyncLink = 2 };
    void setSyncSource(int32_t s) {
        syncFlag.store(s < 0 || s > SyncLink ? SyncOff : s, std::memory_order_relaxed);
    }
    int32_t syncSource() const { return syncFlag.load(std::memory_order_relaxed); }
    bool externalSync() const { return syncSource() != SyncOff; }
    bool followingMidi() const { return syncSource() == SyncMidi; }
    bool followingLink() const { return syncSource() == SyncLink; }
    /** On/off switch for MIDI clock sync. */
    void setExternalSync(bool on) {
        if (on) setSyncSource(SyncMidi);
        else if (followingMidi()) setSyncSource(SyncOff);
    }

    // What the follower is doing, for the display: packed bpm and error.
    void publishSync(int64_t packed) { syncForUi.store(packed, std::memory_order_relaxed); }
    int64_t syncState() const { return syncForUi.load(std::memory_order_relaxed); }

    void setClockOut(bool on) { clockOutFlag.store(on, std::memory_order_relaxed); }
    bool clockOut() const { return clockOutFlag.load(std::memory_order_relaxed); }

    void setLauncher(bool on) { launcherFlag.store(on, std::memory_order_relaxed); }
    bool launcherMode() const { return launcherFlag.load(std::memory_order_relaxed); }

    /**
     * Whether a Fill trig may sound: true while the Fill button is held.
     *
     * It lives here because it depends on what the player is doing. An offline
     * render always sees it off, so exports are repeatable.
     */
    void setFill(bool on) { fillFlag.store(on, std::memory_order_relaxed); }
    bool fill() const { return fillFlag.load(std::memory_order_relaxed); }

    /** 0 swaps at the end of the playing clip's cycle; otherwise a tick grid. */
    void setLaunchQuantise(int32_t ticks) { launchQ.store(ticks < 0 ? 0 : ticks, std::memory_order_relaxed); }
    int32_t launchQuantise() const { return launchQ.load(std::memory_order_relaxed); }

    /**
     * A cell was tapped. Whether that starts, cancels or stops a clip is
     * decided on the audio thread against what's actually playing, so it
     * never uses a stale readback.
     */
    void launchClip(int32_t rack, int64_t sceneId) {
        if (rack >= 0 && rack < kRackCount) {
            queuedClip[rack].store(sceneId, std::memory_order_relaxed);
        }
    }
    int64_t takeQueuedClip(int32_t rack) {
        return (rack >= 0 && rack < kRackCount) ? queuedClip[rack].exchange(0, std::memory_order_relaxed) : 0;
    }
    /** A scene's header in clip mode: its clips in, everything else out, together. See Launcher::requestScene. */
    void launchScene(int64_t sceneId) { launchedScene.store(sceneId, std::memory_order_relaxed); }
    int64_t takeLaunchedScene() { return launchedScene.exchange(0, std::memory_order_relaxed); }
    void requestStopAll() { stopAllFlag.store(true, std::memory_order_relaxed); }
    bool takeStopAll() { return stopAllFlag.exchange(false, std::memory_order_relaxed); }

    // Per-rack state for the grid: the scene playing, the scene queued, and
    // how far through its cycle it is. One atomic each, so a cell never sees
    // a torn pair.
    static constexpr int32_t kNoScene = 0xff;   // nothing there
    static constexpr int32_t kStopQueued = 0xfe; // queued to stop
    static constexpr int64_t packLaunch(int32_t scene, int32_t pending, int64_t tickInCycle) {
        return (static_cast<int64_t>(scene & 0xff) << 56) |
               (static_cast<int64_t>(pending & 0xff) << 48) |
               (tickInCycle & 0xffffffffffffLL);
    }
    /**
     * What a rack that has never launched anything reads as. The array
     * starts at this, because a zeroed slot unpacks to "playing scene one
     * and queued to play it again".
     */
    // Written out rather than using packLaunch, which can't be called in a
    // constant while the class is still incomplete.
    static constexpr int64_t kLaunchIdle =
        (static_cast<int64_t>(kNoScene) << 56) | (static_cast<int64_t>(kNoScene) << 48);
    void publishLaunch(int32_t rack, int64_t packed) {
        if (rack >= 0 && rack < kRackCount) {
            launchForUi[rack].store(packed, std::memory_order_relaxed);
        }
    }
    int64_t launchState(int32_t rack) const {
        return (rack >= 0 && rack < kRackCount) ? launchForUi[rack].load(std::memory_order_relaxed) : kLaunchIdle;
    }
    void clearLaunchRequests() {
        for (auto &q : queuedClip) {
            q.store(0, std::memory_order_relaxed);
        }
        stopAllFlag.store(false, std::memory_order_relaxed);
        launchedScene.store(0, std::memory_order_relaxed);
    }

    // --- Audio thread ---------------------------------------------------------
    /** True once after requestRewind(). */
    bool takeRewind() { return rewindFlag.exchange(false, std::memory_order_acquire); }
    bool takeStopAtEnd() { return stopAtEndFlag.exchange(false, std::memory_order_relaxed); }
    int32_t takeQueuedScene() { return queuedScene.exchange(-1, std::memory_order_relaxed); }

    // Applies a pending play/stop request. Returns true if the state changed.
    bool applyRequests() {
        const Request r = request.exchange(Request::None, std::memory_order_acq_rel);
        if (r == Request::None) {
            return false;
        }
        stopAtEndFlag.store(false, std::memory_order_relaxed); // a new play or stop cancels both
        queuedScene.store(-1, std::memory_order_relaxed);
        // Launch requests are not cleared here. The UI queues the first clip
        // and then asks for play, and clearing the queue here would delay
        // that clip by a whole cycle.
        if (r == Request::Continue) {
            continued.store(true, std::memory_order_relaxed);
        }
        const bool wanted = (r == Request::Play || r == Request::Continue);
        if (wanted == playing) {
            return false;
        }
        playing = wanted;
        playingForUi.store(playing, std::memory_order_relaxed);
        return true;
    }
    int32_t requestedStartScene() const { return startScene.load(std::memory_order_relaxed); }

    // --- Count-in ----------------------------------------------------------------
    /**
     * How many bars of clicks to play before the song starts.
     *
     * Read when playback starts, so changing the setting during a count-in
     * doesn't change the count that's running.
     */
    void setCountInBars(int32_t bars) { countInBars.store(bars < 0 ? 0 : bars, std::memory_order_relaxed); }
    int32_t countInBarsWanted() const { return countInBars.load(std::memory_order_relaxed); }

    /** Ticks left of the count, for the screen. 0 when not counting. */
    void publishCountIn(int64_t ticksLeft) { countInLeft.store(ticksLeft, std::memory_order_relaxed); }
    int64_t countInRemaining() const { return countInLeft.load(std::memory_order_relaxed); }
    bool loopScene() const { return loopSceneFlag.load(std::memory_order_relaxed); }
    bool loopSong() const { return loopSongFlag.load(std::memory_order_relaxed); }

    // The song ran out and loopSong is off.
    void stopFromAudioThread() {
        playing = false;
        playingForUi.store(false, std::memory_order_relaxed);
        stopAtEndFlag.store(false, std::memory_order_relaxed);
        queuedScene.store(-1, std::memory_order_relaxed);
        clearLaunchRequests();
    }
    bool isPlaying() const { return playing; }
    bool isRecording() const { return playing && recordArmed.load(std::memory_order_relaxed); }

    // Packed position: scene (8 bits) | repeat (8 bits) | tick within the
    // current iteration (48 bits). One atomic, so the UI never sees a torn
    // scene/tick pair.
    static int64_t pack(int32_t scene, int32_t repeat, int64_t tickInIteration) {
        return (static_cast<int64_t>(scene & 0xff) << 56) |
               (static_cast<int64_t>(repeat & 0xff) << 48) |
               (tickInIteration & 0xffffffffffffLL);
    }
    void publishPosition(int64_t packed) { positionForUi.store(packed, std::memory_order_relaxed); }
    /** Milliseconds played since play, not counting a count-in. Held after a stop. */
    void publishElapsed(int64_t ms) { elapsedForUi.store(ms, std::memory_order_relaxed); }

    // --- Either thread --------------------------------------------------------
    int64_t position() const { return positionForUi.load(std::memory_order_relaxed); }
    bool isPlayingForUi() const { return playingForUi.load(std::memory_order_relaxed); }
    int64_t elapsedMs() const { return elapsedForUi.load(std::memory_order_relaxed); }

  private:
    std::atomic<Request> request{Request::None};
    std::atomic<int32_t> countInBars{0};
    std::atomic<int64_t> countInLeft{0};
    std::atomic<int32_t> startScene{kCurrentScene};
    std::atomic<bool> loopSceneFlag{false};
    std::atomic<bool> loopSongFlag{true};
    std::atomic<bool> stopAtEndFlag{false};
    std::atomic<bool> rewindFlag{false};
    std::atomic<int32_t> queuedScene{-1};
    std::atomic<bool> recordArmed{false};
    std::atomic<bool> recordOnNote{false};
    std::atomic<bool> recordOnce{false};
    std::atomic<bool> launcherFlag{false};
    std::atomic<bool> fillFlag{false};
    std::atomic<bool> clockOutFlag{false};
    std::atomic<int32_t> syncFlag{SyncOff};
    std::atomic<bool> continued{false};
    std::atomic<int64_t> syncForUi{0};
    std::atomic<int32_t> launchQ{0};
    std::atomic<bool> stopAllFlag{false};
    std::atomic<int64_t> queuedClip[kRackCount]{};
    std::atomic<int64_t> launchedScene{0};
    std::atomic<int64_t> launchForUi[kRackCount]{};
    bool playing = false; // audio-thread truth
    std::atomic<bool> playingForUi{false};
    std::atomic<int64_t> positionForUi{0};
    std::atomic<int64_t> elapsedForUi{0};
};

} // namespace acidulous::seq
