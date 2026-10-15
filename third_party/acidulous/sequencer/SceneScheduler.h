#pragma once
#include "ClockFollower.h"
#include "Launcher.h"
#include "Song.h"
#include "TickClock.h"
#include "Swing.h"
#include "Transport.h"
#include <algorithm>
#include <cstdint>
#include <engine/rack/Rack.h>

// Plays through the scenes, or in clip mode lets every rack play its own.
//
// The arranger keeps the playback position as
//   (sceneIdx, repeatIdx, iterationOrigin)
// where iterationOrigin is the absolute clock tick the current pass through
// the current scene began at. Which clip each rack plays, when OneShot clips
// re-arm and the UI's bar.beat all come from that.
//
// Clip mode has one of those per rack, kept in the Launcher. ClipPlayer is
// always given its origin as a parameter, so each rack can play a clip from
// a different scene with its own origin. The arranger path below is used
// whenever clip mode is off.
//
// Audio thread only.

namespace acidulous::seq {

class SceneScheduler {
  public:
    /** Sixteenths or eighths: which notes the swing moves. Song-wide. */
    void setSwingPair(int64_t pair) { swingPair = pair >= 2 ? pair : Swing::kSixteenths; }

    void bind(Rack *racks, int32_t rackCount, TickClock *clock, Transport *transport) {
        this->racks = racks;
        this->rackCount = rackCount;
        this->clock = clock;
        this->transport = transport;
    }

    // Returns the previous snapshot for the caller to queue for destruction.
    //
    // The playing scene is found again by id, not index, so inserting or
    // deleting scenes before it doesn't move playback. If it was deleted,
    // playback moves to the next remaining scene (or the last one). If it got
    // shorter than the playhead's offset, the origin is moved so the phase in
    // the shorter loop is kept.
    const SongSnapshot *swapSnapshot(const SongSnapshot *next) {
        const SongSnapshot *old = snap;
        snap = next;
        if (snap == nullptr || snap->scenes.empty()) {
            sceneIdx = 0;
            repeatIdx = 0;
            launcher.clearAll();
            launcher.takeChanged();
            for (int32_t r = 0; r < rackCount; ++r) {
                racks[r].clipPlayer.setClip(nullptr);
            }
            return old;
        }
        if (transport != nullptr && transport->launcherMode()) {
            // Clip mode tracks scenes by id, so inserting or deleting scenes can't
            // point a rack at a different clip. A deleted scene silences its rack.
            for (int32_t r = 0; r < rackCount; ++r) {
                const int64_t id = launcher.sceneId(r);
                if (id == Launcher::kNone) {
                    racks[r].clipPlayer.setClip(nullptr);
                    continue;
                }
                const int32_t idx = snap->indexOfScene(id);
                if (idx < 0) {
                    launcher.dropRack(r);
                    racks[r].clipPlayer.setClip(nullptr);
                    continue;
                }
                launcher.reshape(r, cycleTicks(r, idx));
                racks[r].clipPlayer.setClip(snap->clipFor(r, idx));
            }
            launcher.takeChanged();
            return old;
        }

        const auto count = static_cast<int32_t>(snap->scenes.size());
        const int64_t pos = clock->position();

        int32_t resolved = -1;
        if (old != nullptr && sceneIdx < static_cast<int32_t>(old->scenes.size())) {
            // Same scene, wherever it moved to.
            resolved = snap->indexOfScene(old->scenes[sceneIdx].id);
            if (resolved < 0) {
                // Deleted: use the next scene that still exists.
                for (size_t i = sceneIdx + 1; i < old->scenes.size() && resolved < 0; ++i) {
                    resolved = snap->indexOfScene(old->scenes[i].id);
                }
            }
        }
        if (resolved < 0) {
            resolved = std::min(sceneIdx, count - 1);
        }

        if (resolved != sceneIdx) {
            sceneIdx = resolved;
            repeatIdx = 0;
            iterationOrigin = pos;
        } else {
            const int64_t iterLen = std::max<int64_t>(1, snap->scenes[sceneIdx].iterationTicks());
            const int64_t elapsed = pos - iterationOrigin;
            if (elapsed >= iterLen) {
                iterationOrigin = pos - (elapsed % iterLen);
            }
        }
        repeatIdx = std::min(repeatIdx, snap->scenes[sceneIdx].repeat - 1);
        pointClipPlayers();
        return old;
    }
    const SongSnapshot *snapshot() const { return snap; }

    // On play. Call after the clock has been reset. kCurrentScene restarts
    // the current scene from its start.
    /** 0xFB (MIDI Continue): carry on from the playhead without restarting. */
    void resume() {
        beginning();
        if (snap == nullptr || snap->scenes.empty()) {
            return;
        }
        iterationOrigin = clock->position() - lastTickInIteration;
        pointClipPlayers();
    }

    /** Move the playhead to an absolute song tick, for an incoming Song Position Pointer. */
    void locateTo(int64_t songTick) {
        if (snap == nullptr || snap->scenes.empty()) {
            return;
        }
        int32_t sc = 0, rp = 0;
        int64_t tickIn = 0;
        snap->locate(songTick, sc, rp, tickIn);
        sceneIdx = sc;
        repeatIdx = rp;
        lastTickInIteration = tickIn;
        iterationOrigin = clock->position() - tickIn;
        rampHeld = false;
        pointClipPlayers();
    }

    void start(int32_t requestedScene) {
        beginning();
        if (transport != nullptr && transport->launcherMode()) {
            // Nothing plays until a clip is tapped. The clock runs from zero and
            // the grid stays silent.
            launcher.reset();
            lastTickInIteration = 0;
            for (int32_t r = 0; r < rackCount; ++r) {
                racks[r].clipPlayer.setClip(nullptr);
            }
            return;
        }
        if (snap == nullptr || snap->scenes.empty()) {
            sceneIdx = 0;
            return;
        }
        const auto count = static_cast<int32_t>(snap->scenes.size());
        int32_t idx = (requestedScene == Transport::kCurrentScene) ? sceneIdx : requestedScene;
        idx = std::clamp(idx, 0, count - 1);
        iterationOrigin = clock->position();
        lastTickInIteration = 0;
        enterScene(idx, /*allowSmooth=*/false);
    }

    /**
     * Every rack takes over the playing scene at its current phase. The origin
     * is the scene's iteration origin rather than now, so a clip half way
     * through stays half way through and switching modes is inaudible.
     */
    void adoptPlayingScene() {
        if (snap == nullptr || snap->scenes.empty() || sceneIdx < 0 ||
            sceneIdx >= static_cast<int32_t>(snap->scenes.size())) {
            return;
        }
        const SceneInfo &sc = snap->scenes[sceneIdx];
        // Keep the tempo. Use the scene's own override rather than clock->bpm(),
        // which may be part way through a ramp.
        launcherTempo = sc.bpmOverride > 0.0f ? sc.bpmOverride : clock->songTempoRequested();
        launcherTempoFrom = clock->songTempoRequested();
        const int64_t id = sc.id;
        for (int32_t r = 0; r < rackCount; ++r) {
            if (snap->clipFor(r, sceneIdx) == nullptr) {
                continue; // a track with nothing here stays silent
            }
            launcher.adopt(r, id, cycleTicks(r, sceneIdx), iterationOrigin);
            racks[r].clipPlayer.setClip(snap->clipFor(r, sceneIdx));
        }
    }

    /**
     * Leaving clip mode: every rack moves onto one scene at the bar line, in
     * phase.
     *
     * The scene most racks are already playing wins, so the fewest racks change
     * and those carry on untouched. Ties go to the lowest scene index so the
     * result is stable.
     *
     * The phase comes from a rack already on that scene, so the arrangement
     * continues from there instead of restarting or jumping back to where scene
     * mode left off.
     */
    void handBackToScenes(int64_t at) {
        if (snap == nullptr || snap->scenes.empty()) {
            return;
        }
        const auto count = static_cast<int32_t>(snap->scenes.size());
        int32_t votes[kRackCount] = {};
        int32_t best = -1, bestVotes = 0;
        int64_t bestOrigin = at;
        for (int32_t r = 0; r < rackCount; ++r) {
            if (!launcher.playing(r)) continue;
            const int32_t idx = snap->indexOfScene(launcher.sceneId(r));
            if (idx < 0 || idx >= count) continue;
            const int32_t v = ++votes[idx];
            if (v > bestVotes || (v == bestVotes && best >= 0 && idx < best)) {
                bestVotes = v;
                best = idx;
                bestOrigin = launcher.origin(r);
            }
        }
        if (best < 0) {
            return; // nothing was playing, scene mode resumes where it was
        }
        enterScene(best, /*allowSmooth=*/false);
        // Keep the phase those racks had. The origin may be well behind the
        // handover tick, and the scene path moves it forward.
        iterationOrigin = bestOrigin;
        lastTickInIteration = at - bestOrigin;
        launcher.clearAll();
        launcher.takeChanged();
        launcherNow = 0;
        if (transport != nullptr) {
            for (int32_t r = 0; r < rackCount; ++r) {
                transport->publishLaunch(r, Transport::packLaunch(Transport::kNoScene, Transport::kNoScene, 0));
            }
        }
    }

    /** Stopped: nothing launched, nothing queued, the grid goes dark. */
    void stopLauncher() {
        launcher.clearAll();
        launcher.takeChanged();
        launcherNow = 0;
        // A stop ends any handover in progress. The flag itself is set in
        // beginning(), where we can tell "mode changed while running" from
        // "starting in this mode".
        returnPending = false;
        launcherTempo = 0.0f; // nothing adopted, so no tempo is held
        if (transport != nullptr) {
            for (int32_t r = 0; r < rackCount; ++r) {
                transport->publishLaunch(r, Transport::packLaunch(Transport::kNoScene, Transport::kNoScene, 0));
            }
        }
    }

    void allNotesOff() {
        for (int32_t r = 0; r < rackCount; ++r) {
            if (racks[r].isActive()) {
                Rack &rack = racks[r];
                rack.clipPlayer.allNotesOff([&rack](uint8_t c, uint8_t a, uint8_t b) { rack.playSequenced(c, a, b); });
            }
        }
    }

    /**
     * Playback is starting, so no mode change is in progress.
     *
     * process() treats a change in the launcher flag as "clip mode was just
     * pressed" and hands the launcher the playing scene. That's right mid-song
     * but wrong at the start: pressing clip while stopped and then play would
     * otherwise start the whole first scene. So the flag is latched here, when
     * playback begins, and nowhere else.
     */
    void beginning() {
        rampHeld = false;
        if (transport != nullptr) {
            launcherWas = transport->launcherMode();
        }
        returnPending = false;
    }

    /**
     * Resets every clip player's random seed and pass count.
     *
     * Not part of allNotesOff, which runs on every normal stop. Re-seeding
     * there would make a free-rolling clip play the same variation every
     * time. This is for the panic path only.
     */
    void resetClipPlayers() {
        for (int32_t r = 0; r < rackCount; ++r) {
            racks[r].clipPlayer.reset();
        }
    }

    // While stopped the song tempo from the UI applies directly.
    void applyIdleTempo() {
        if (transport != nullptr && transport->externalSync()) {
            return; // something else controls the tempo
        }
        const float want = clock->songTempoRequested();
        if (!clock->isRamping() && want != clock->bpm()) {
            clock->setTempo(want);
        }
    }

    // Fire everything in [blockStart, blockEnd). Returns false when the song
    // has run out and loopSong is off, and the caller stops the transport.
    bool process(int64_t blockStart, int64_t blockEnd) {
        if (snap == nullptr || snap->scenes.empty()) {
            return true;
        }
        // Read the fill button once a block and give it to every player, so all
        // tracks agree for the whole block.
        const bool filling = transport != nullptr && transport->fill();
        for (int32_t r = 0; r < rackCount; ++r) {
            racks[r].clipPlayer.setFill(filling);
            // Swing too, once a block, so every track agrees on where the beat is.
            // Uses the target rather than the smoothed value, since ramping swing
            // would slide the offbeats across a bar and exports couldn't repeat it.
            racks[r].clipPlayer.setSwing(racks[r].channelTarget(Rack::Swing), swingPair);
        }
        // Entering clip mode while playing: hand the launcher the scene every
        // rack is already playing, in phase, so nothing stops. Otherwise the
        // song would go silent until each clip is tapped.
        const bool launching = transport->launcherMode();
        if (launching && !launcherWas) {
            adoptPlayingScene();
            returnPending = false;
        }
        // Leaving clip mode waits for the next bar line. The launcher keeps
        // running until then and hands over there, rather than mid-bar.
        if (!launching && launcherWas && launcher.anyPlaying()) {
            const int64_t bar = std::max<int64_t>(1, songTicksPerBar());
            returnAt = ((blockStart / bar) + 1) * bar;
            returnPending = true;
        }
        launcherWas = launching;
        if (returnPending) {
            if (blockEnd <= returnAt) {
                return processLauncher(blockStart, blockEnd); // not there yet
            }
            // The bar line is inside this block: play the launcher up to it, hand
            // over, and let the scene path do the rest.
            if (returnAt > blockStart) {
                processLauncher(blockStart, returnAt);
            }
            handBackToScenes(returnAt);
            returnPending = false;
            blockStart = returnAt;
        }
        if (launching) {
            return processLauncher(blockStart, blockEnd);
        }
        followTempo();

        int64_t cur = blockStart;
        while (cur < blockEnd) {
            const SceneInfo &sc = snap->scenes[sceneIdx];
            const int64_t iterLen = std::max<int64_t>(1, sc.iterationTicks());
            const int64_t iterEnd = iterationOrigin + iterLen;
            const int64_t segEnd = std::min(blockEnd, iterEnd);
            startRamp(sc, iterEnd, segEnd);
            if (segEnd > cur) {
                fire(cur, segEnd);
                cur = segEnd;
            }
            if (cur >= iterEnd) {
                // Iteration boundary. Moving the origin re-arms OneShot clips and
                // keeps the loop maths right.
                iterationOrigin = iterEnd;
                if (++repeatIdx >= sc.repeat) {
                    repeatIdx = 0;
                    // A scene that plays again (looped, or the only one) starts at its
                    // own tempo, not where its ramp ended.
                    rampHeld = false;
                    // Anything queued from the UI lands at this boundary: a waiting
                    // scene or an armed finish.
                    const int32_t queued = transport->takeQueuedScene();
                    if (queued >= 0 && queued < static_cast<int32_t>(snap->scenes.size())) {
                        enterScene(queued, /*allowSmooth=*/true);
                        continue;
                    }
                    if (transport->takeStopAtEnd()) {
                        lastTickInIteration = 0;
                        return false;
                    }
                    if (!transport->loopScene()) {
                        int32_t next = sceneIdx + 1;
                        if (next >= static_cast<int32_t>(snap->scenes.size())) {
                            if (!transport->loopSong()) {
                                sceneIdx = 0; // next Play starts from the top
                                lastTickInIteration = 0;
                                return false;
                            }
                            next = 0;
                        }
                        if (next != sceneIdx) {
                            enterScene(next, /*allowSmooth=*/true);
                        }
                    }
                }
            }
        }
        lastTickInIteration = blockEnd - iterationOrigin;
        return true;
    }

    int64_t packedPosition() const { return Transport::pack(sceneIdx, repeatIdx, lastTickInIteration); }

    // For stamping recorded events. Valid between blocks, i.e. from pollMidiIn().
    int64_t currentSceneId() const {
        return (snap != nullptr && sceneIdx < static_cast<int32_t>(snap->scenes.size())) ? snap->scenes[sceneIdx].id : 0;
    }
    int64_t currentTickInIteration() const { return lastTickInIteration; }
    const SceneInfo *currentSceneInfo() const {
        return (snap != nullptr && sceneIdx < static_cast<int32_t>(snap->scenes.size())) ? &snap->scenes[sceneIdx] : nullptr;
    }
    int32_t currentRepeat() const { return repeatIdx; }
    int32_t currentScene() const { return sceneIdx; }

    /** True while clip mode is driving playback. */
    bool launcherActive() const { return transport != nullptr && transport->launcherMode(); }

    /**
     * The scene a rack is playing. In the arranger it's the same for every
     * rack, in clip mode it isn't, so anything per rack (recording, frozen
     * audio) must ask this rather than use the scheduler's own position.
     */
    int64_t rackSceneId(int32_t rack) const {
        if (!launcherActive()) {
            return currentSceneId();
        }
        const int64_t id = launcher.sceneId(rack);
        return id == Launcher::kNone ? 0 : id;
    }
    int64_t rackTick(int32_t rack) const {
        if (!launcherActive()) {
            return lastTickInIteration;
        }
        return launcher.playing(rack) ? launcherNow - launcher.origin(rack) : 0;
    }

    /**
     * How far into this rack's cell it is, counting repeats.
     *
     * rackTick resets every repeat, which is right for notes (a one-bar clip
     * in a four-bar scene plays four times) but wrong for audio. A take sung
     * across a scene played twice is one eight-bar performance.
     *
     * This counts from the start of a cycle of bars x repeat, which is what
     * the launcher's origin already is. Both modes agree on it, so one
     * recording works in both.
     */
    int64_t rackCycleTick(int32_t rack) const {
        if (!launcherActive()) {
            if (snap == nullptr || snap->scenes.empty()) return lastTickInIteration;
            const SceneInfo &sc = snap->scenes[static_cast<size_t>(sceneIdx)];
            const int64_t iter = std::max<int64_t>(1, sc.iterationTicks());
            const int64_t absolute = static_cast<int64_t>(repeatIdx) * iter + lastTickInIteration;
            // Wrapped onto the clip's own cycle, which is what the launcher
            // counts. When a clip is shorter than its scene the launcher's cycle
            // is lengthTicks() * repeat and the arranger's would be the whole
            // scene, so without this the same cell would read different frames in
            // each mode.
            //
            // For a clip as long as its scene (the usual case) len == iter *
            // repeat, so the wrap does nothing.
            const int64_t len = cycleTicks(rack, sceneIdx);
            return len > 0 ? absolute % len : absolute;
        }
        return launcher.playing(rack) ? launcherNow - launcher.origin(rack) : 0;
    }

    /**
     * The length of the cycle rackCycleTick counts within, in ticks. A
     * recording being split needs both, since it records the cell and how far
     * into it.
     */
    int32_t rackCycleTicks(int32_t rack) const {
        if (!launcherActive()) {
            if (snap == nullptr || snap->scenes.empty()) return 0;
            const int64_t own = cycleTicks(rack, sceneIdx);
            if (own > 0) return static_cast<int32_t>(own);
            // A cell that doesn't exist yet still has a length. cycleTicks returns
            // 0 for a rack with no clip in this scene, which the launcher needs.
            // But a recording across the song reaches scenes the track has no clip
            // in yet, and the split is about to create those cells. Returning 0
            // would put the whole take in one cell.
            const SceneInfo &sc = snap->scenes[static_cast<size_t>(sceneIdx)];
            return static_cast<int32_t>(sc.iterationTicks() * std::max(1, sc.repeat));
        }
        return launcher.playing(rack) ? static_cast<int32_t>(launcher.cycle(rack)) : 0;
    }

    /** The bar a phase is measured against: the scene's or the song's. */
    int32_t barTicks() const {
        if (snap == nullptr || snap->scenes.empty() || launcherActive() ||
            sceneIdx >= static_cast<int32_t>(snap->scenes.size())) {
            return songTicksPerBar();
        }
        return snap->scenes[sceneIdx].ticksPerBar;
    }

    /** Clip mode has no single scene to take a signature from, so use the song's. */
    int32_t songTicksPerBar() const {
        return (snap != nullptr && !snap->scenes.empty()) ? snap->scenes[0].ticksPerBar : 4 * kPPQN;
    }

    /** Clip mode: what each rack is doing, for the grid. */
    void publishLaunchStates(int64_t now) const {
        for (int32_t r = 0; r < rackCount; ++r) {
            const int64_t id = launcher.sceneId(r);
            const int32_t cur = id == Launcher::kNone ? Transport::kNoScene : snap->indexOfScene(id);
            const int64_t pend = launcher.pendingId(r);
            int32_t pendIdx = Transport::kNoScene;
            if (pend == Launcher::kStopId) {
                pendIdx = Transport::kStopQueued;
            } else if (pend != Launcher::kNone) {
                pendIdx = snap->indexOfScene(pend);
            }
            const int64_t tick = launcher.playing(r) ? now - launcher.origin(r) : 0;
            transport->publishLaunch(r, Transport::packLaunch(cur < 0 ? Transport::kNoScene : cur,
                                                              pendIdx < 0 ? Transport::kNoScene : pendIdx,
                                                              tick < 0 ? 0 : tick));
        }
    }

    const Launcher &launchState() const { return launcher; }

  private:
    /**
     * Clip mode. The block is split at every tick where something changes (a
     * cycle ending, a launch landing) so swaps are as sample-accurate as the
     * arranger's, and each rack plays from its own origin.
     */
    bool processLauncher(int64_t blockStart, int64_t blockEnd) {
        // With clips from several scenes no scene owns the tempo, so the song
        // tempo applies and scene overrides, ramps and fades are skipped.
        //
        // Except after switching into clip mode from a scene with its own
        // tempo. That tempo is latched when the launcher adopts the scene, so
        // the tempo doesn't jump in the middle of a bar, and held until the
        // player changes it.
        if (!clock->isRamping() && !(transport != nullptr && transport->externalSync())) {
            const float song = clock->songTempoRequested();
            // Once the song tempo differs from when the latch was taken, the
            // player has changed it, so drop the latch.
            if (launcherTempo > 0.0f && song != launcherTempoFrom) {
                launcherTempo = 0.0f;
            }
            const float want = launcherTempo > 0.0f ? launcherTempo : song;
            if (want != clock->bpm()) {
                clock->setTempo(want);
            }
        }
        launcher.setQuantise(transport->launchQuantise());

        if (transport->takeStopAll()) {
            launcher.requestStopAll(blockStart);
        }
        // A scene launch before single taps, so a tap after it still wins.
        if (const int64_t scene = transport->takeLaunchedScene(); scene != 0) {
            const int32_t idx = snap->indexOfScene(scene);
            if (idx >= 0) {
                int64_t cycles[kRackCount] = {};
                for (int32_t r = 0; r < rackCount && r < kRackCount; ++r) {
                    if (snap->clipFor(r, idx) != nullptr) cycles[r] = cycleTicks(r, idx);
                }
                launcher.requestScene(scene, cycles, kRackCount, blockStart);
            }
        }
        for (int32_t r = 0; r < rackCount; ++r) {
            const int64_t id = transport->takeQueuedClip(r);
            if (id == 0) {
                continue;
            }
            if (id == Launcher::kCancelId) {
                launcher.cancel(r);
                continue;
            }
            const int32_t idx = snap->indexOfScene(id);
            if (idx < 0 || snap->clipFor(r, idx) == nullptr) {
                // An empty cell isn't a launch. The grid only queues cells with a
                // clip, but launching into nothing would leave a lit cell playing
                // silence forever.
                continue;
            }
            launcher.request(r, id, cycleTicks(r, idx), blockStart);
        }

        int64_t cur = blockStart;
        while (cur < blockEnd) {
            launcher.applyDue(cur);
            const uint32_t changed = launcher.takeChanged();
            if (changed != 0) {
                pointLaunched(changed);
            }
            const int64_t segEnd = std::min(blockEnd, launcher.nextEvent(cur));
            fireLauncher(cur, segEnd);
            cur = segEnd;
        }
        lastTickInIteration = 0;
        launcherNow = blockEnd;
        publishLaunchStates(blockEnd);
        return true;
    }

    /** bars x repeat: what a swap waits for and what re-arms a OneShot. */
    int64_t cycleTicks(int32_t rack, int32_t sceneIdx_) const {
        const Clip *c = snap->clipFor(rack, sceneIdx_);
        if (c == nullptr) {
            return 0;
        }
        const int32_t repeat = std::max(1, snap->scenes[sceneIdx_].repeat);
        return c->lengthTicks() * repeat;
    }

    void pointLaunched(uint32_t mask) {
        for (int32_t r = 0; r < rackCount; ++r) {
            if ((mask & (1u << r)) == 0) {
                continue;
            }
            const int64_t id = launcher.sceneId(r);
            const int32_t idx = (id == Launcher::kNone) ? -1 : snap->indexOfScene(id);
            racks[r].clipPlayer.setClip(idx < 0 ? nullptr : snap->clipFor(r, idx));
        }
    }

    void fireLauncher(int64_t from, int64_t to) {
        if (to <= from) {
            return;
        }
        for (int32_t r = 0; r < rackCount; ++r) {
            if (!racks[r].isActive() || racks[r].frozenActive()) {
                continue;
            }
            Rack &rack = racks[r];
            // A silent rack is still processed because process() sends the
            // note-offs it owes before looking at the clip.
            const int64_t origin = launcher.playing(r) ? launcher.origin(r) : from;
            if (rack.clipPlayer.originChanged(origin)) {
                rack.clearTouched();
            }
            // Lanes first, so a parameter change on a note's step is in place
            // before the note plays.
            if (launcher.playing(r)) {
                rack.clipPlayer.processLanes(
                    to, origin,
                    [&rack](Unit u, int32_t i, float v, bool jump) { rack.setParam(u, i, v, jump); },
                    [&rack](Unit u, int32_t i) { return rack.isTouched(u, i); });
            }
            if (launcher.playing(r)) sendWordsAhead(rack, from, to, origin, 0);
            rack.clipPlayer.process(from, to, origin,
                                    [&rack](uint8_t c, uint8_t a, uint8_t b) { rack.playSequenced(c, a, b); },
                                    [&rack](const uint8_t *p, int32_t n) { rack.lyric(p, n); });
            rack.clipPlayer.processExpression(
                to, [&rack](uint8_t n, int32_t k, float v) { rack.noteExpressionValue(k, n, v); });
        }
    }

    /**
     * Starts the scene's tempo ramp when its last pass gets within the ramp's
     * bars of the end. The clock's glide does the work, timed to arrive at the
     * scene's end. After that the tempo is held at the target until the next
     * scene, otherwise followTempo would put it back.
     */
    void startRamp(const SceneInfo &sc, int64_t iterEnd, int64_t segEnd) {
        if (rampHeld || sc.rampToBpm <= 0.0f || sc.rampBars <= 0 || repeatIdx != sc.repeat - 1) return;
        if (transport != nullptr && transport->externalSync()) return;
        const int64_t length = std::min<int64_t>(sc.iterationTicks(), static_cast<int64_t>(sc.rampBars) * sc.ticksPerBar);
        if (segEnd <= iterEnd - length) return;
        rampHeld = true;
        clock->rampTempo(sc.rampToBpm, std::max<int64_t>(1, iterEnd - clock->position()));
    }

    void enterScene(int32_t idx, bool allowSmooth) {
        rampHeld = false;
        sceneIdx = idx;
        repeatIdx = 0;
        pointClipPlayers();
        const SceneInfo &sc = snap->scenes[idx];
        if (transport != nullptr && transport->externalSync()) {
            return; // something else controls the tempo
        }
        const float want = sc.bpmOverride > 0.0f ? sc.bpmOverride : clock->songTempoRequested();
        if (allowSmooth && sc.smooth && want != clock->bpm()) {
            clock->rampTempo(want, sc.ticksPerBar); // over the first bar of the new scene
        } else {
            clock->setTempo(want);
        }
    }

    // A scene without its own tempo follows the UI's song tempo live.
    void followTempo() {
        // Four places set the tempo every block: here, applyIdleTempo, the
        // launcher and enterScene. Each must skip when synced to an external
        // clock, or it overwrites the follower every block.
        if (transport != nullptr && transport->externalSync()) {
            return;
        }
        if (clock->isRamping()) {
            return;
        }
        const SceneInfo &sc = snap->scenes[sceneIdx];
        const float want = rampHeld ? sc.rampToBpm
            : sc.bpmOverride > 0.0f ? sc.bpmOverride : clock->songTempoRequested();
        if (want != clock->bpm()) {
            clock->setTempo(want);
        }
    }

    void pointClipPlayers() {
        for (int32_t r = 0; r < rackCount; ++r) {
            racks[r].clipPlayer.setClip(snap->clipFor(r, sceneIdx));
        }
    }

    /**
     * A singer's words, half a second ahead of their notes (see
     * Machine::wantsWordsAhead). [loop] is the pass length when the next pass
     * is the same clip again, negative for the last pass, which they don't
     * reach past, and 0 for a launched clip, which loops on.
     */
    void sendWordsAhead(Rack &rack, int64_t from, int64_t to, int64_t origin, int64_t loop) {
        if (!rack.wantsWordsAhead() || clock == nullptr) return;
        const double perTick = std::max(1.0, clock->samplesPerTickNow());
        const auto ahead = static_cast<int64_t>(std::ceil(0.5 * kSampleRate / perTick));
        int64_t end = to + ahead;
        if (loop < 0) end = std::min(end, origin - loop);
        rack.clipPlayer.processWordsAhead(
            from + ahead, end, origin, loop > 0 ? loop : 0,
            [&](const uint8_t *p, int32_t n, uint8_t note, uint8_t vel, int64_t tick) {
                rack.wordsAhead(p, n, note, vel, static_cast<int32_t>(static_cast<double>(tick - from) * perTick));
            });
    }

    void fire(int64_t from, int64_t to) {
        for (int32_t r = 0; r < rackCount; ++r) {
            // Frozen racks get nothing. Their notes and automation are already in
            // the audio, and running them again would waste the CPU freezing saves.
            if (racks[r].isActive() && !racks[r].frozenActive()) {
                Rack &rack = racks[r];
                if (rack.clipPlayer.originChanged(iterationOrigin)) rack.clearTouched();
                // Lanes first, as above.
                rack.clipPlayer.processLanes(
                    to, iterationOrigin,
                    [&rack](Unit u, int32_t i, float v, bool jump) { rack.setParam(u, i, v, jump); },
                    [&rack](Unit u, int32_t i) { return rack.isTouched(u, i); });
                // Into the next pass only when it's this scene again.
                const SceneInfo &sc = snap->scenes[sceneIdx];
                const int64_t iterLen = std::max<int64_t>(1, sc.iterationTicks());
                sendWordsAhead(rack, from, to, iterationOrigin, repeatIdx + 1 < sc.repeat ? iterLen : -iterLen);
                rack.clipPlayer.process(from, to, iterationOrigin,
                                        [&rack](uint8_t c, uint8_t a, uint8_t b) { rack.playSequenced(c, a, b); },
                                        [&rack](const uint8_t *p, int32_t n) { rack.lyric(p, n); });
                rack.clipPlayer.processExpression(
                    to, [&rack](uint8_t n, int32_t k, float v) { rack.noteExpressionValue(k, n, v); });
            }
        }
    }

    Rack *racks = nullptr;
    int32_t rackCount = 0;
    TickClock *clock = nullptr;
    Transport *transport = nullptr;
    const SongSnapshot *snap = nullptr;

    Launcher launcher;
    int64_t launcherNow = 0;
    /** Whether the last block was in launcher mode, to catch the change. */
    bool launcherWas = false;
    // The tempo clip mode holds because it was playing when clip mode began.
    // 0 means it follows the song tempo. See processLauncher.
    float launcherTempo = 0.0f;
    float launcherTempoFrom = 0.0f;
    /** Clip mode has been switched off and is playing out to the bar line. */
    bool returnPending = false;
    int64_t returnAt = 0;
    int32_t sceneIdx = 0;
    int32_t repeatIdx = 0;
    int64_t iterationOrigin = 0;
    /** The scene's ramp has started this pass (see startRamp). */
    bool rampHeld = false;
    int64_t swingPair = Swing::kSixteenths;
    int64_t lastTickInIteration = 0;
};

} // namespace acidulous::seq
