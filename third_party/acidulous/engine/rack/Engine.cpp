#include "Engine.h"
#include <sequencer/ClockFollower.h>
#include <sequencer/LinkFollower.h>
#include <sequencer/Song.h>
#include <cmath>
#include <ctime>

namespace acidulous {

Engine::Engine() {
    clock.setSampleRate(kSampleRate);
    scheduler.bind(racks, kRackCount, &clock, &transport);
    // Every rack reports what leaves its modifier chain, so a recording keeps
    // what was heard rather than what was pressed.
    for (int32_t r = 0; r < kRackCount; ++r) racks[r].setModifiedNoteSink(this);
    for (int32_t r = 0; r < kRackCount; ++r) racks[r].performSink = &master.perform.params();
}

Engine::~Engine() { stop(); }

void Engine::start() {
    master.prepare(kSampleRate);
    retirer.start();
}
void Engine::stop() { retirer.stop(); }

/**
 * The input chain, on the interleaved block about to be published. Separate
 * from renderBlock because it's the one place that deinterleaves and puts
 * back.
 *
 * The tick range is the previous block's since the clock hasn't advanced
 * yet, so a tempo-synced effect on the input is one block (1.3 ms) behind
 * the same effect on a track.
 */
void Engine::runInputChain() {
    float L[kBlockFrames], R[kBlockFrames];
    for (int32_t i = 0; i < kBlockFrames; ++i) {
        L[i] = inputScratch[i * 2];
        R[i] = inputScratch[i * 2 + 1];
    }
    bool stereo = true;
    for (int32_t s = 0; s < kInputSlots; ++s) {
        if (inputFx[s] == nullptr) continue;
        inputFx[s]->onBlock(clock.blockStart(), clock.blockEnd(), clock.bpm());
        stereo = inputFx[s]->run(L, R, kBlockFrames, stereo);
    }
    for (int32_t i = 0; i < kBlockFrames; ++i) {
        inputScratch[i * 2] = L[i];
        inputScratch[i * 2 + 1] = stereo ? R[i] : L[i];
    }
}

/**
 * This thread's CPU time in microseconds. Copied from AudioDriver so the
 * engine doesn't depend on the platform layer.
 *
 * Used alongside the wall clock, not instead of it. The deadline is wall
 * time. This tells how much of it was spent computing.
 */
static int64_t threadCpuUs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void Engine::renderBlock(const float *in, float *out) {
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t cpu0 = threadCpuUs();
    // If the tracks will be spread this block, the workers wake while the
    // input and the sequencer run, rather than after.
    {
        int32_t lastTotal = 0;
        for (int32_t r = 0; r < kRackCount; ++r) lastTotal += lastRackUs[r];
        if (lastTotal >= wakeFloorUs) pool.wakeEarly();
    }

    // The tuner hears the input first, as it arrived. Costs one branch when
    // it's off, which is almost always.
    tuner.push(in, in != nullptr ? kBlockFrames : 0);

    // Publish the input before anything renders, so a machine reading it
    // gets this block's audio and not the last one's.
    const float gain = inputGain.load(std::memory_order_relaxed);
    const bool chained = inputFx[0] != nullptr || inputFx[1] != nullptr;
    if (in != nullptr && (gain != 1.0f || chained)) {
        for (int32_t i = 0; i < kBlockFrames * 2; ++i) inputScratch[i] = in[i] * gain;
        if (chained) runInputChain();
        InputBus::get().publish(inputScratch, kBlockFrames);
    } else {
        InputBus::get().publish(in, in != nullptr ? kBlockFrames : 0);
    }

    const auto tInput = std::chrono::steady_clock::now();

    // Panic first, so the next thing out of the engine is silence.
    if (panicFlag.exchange(false, std::memory_order_acq_rel)) {
        transport.stopFromAudioThread();
        playing = false;
        startPending = false;
        scheduler.allNotesOff();
        // Reset the clip players' pass count and free-roll seed. A render
        // panics first, so this is where "from the beginning" happens.
        scheduler.resetClipPlayers();
        // Panic means silence, so stop any audition too.
        audition.stop();
        for (auto &n : mpeChannelNote) n = -1;
        for (int32_t r = 0; r < kRackCount; ++r) {
            racks[r].allNotesOff();
            // Parameters jump rather than glide, so the start of a render doesn't
            // depend on what was playing before it.
            if (Machine *m = racks[r].currentMachine()) panicMachine(*m);
            racks[r].jumpChannel();
            for (int32_t s = 0; s < kEffectSlots; ++s) {
                if (Effect *e = racks[r].currentEffect(s)) { e->reset(); e->params().jumpAll(); }
            }
            // The input chain too, once. A delay on the input would otherwise
            // keep repeating after a panic.
            if (r == 0) {
                for (Effect *e : inputFx) {
                    if (e != nullptr) { e->reset(); e->params().jumpAll(); }
                }
            }
            // The modifiers too. An arpeggiator keeps a step, so without this
            // a render (which panics first) would start part way through its
            // pattern and two exports of the same song would differ.
            for (int32_t s = 0; s < kInputModSlots; ++s) {
                if (InputMod *e = racks[r].currentInputMod(s)) e->reset();
            }
        }
        master.panic();
    }

    // Transport: apply a play/stop the UI asked for, only between blocks.
    if (transport.applyRequests()) {
        if (transport.isPlaying()) {
            clock.reset();
            playedFrames = 0;
            // A count-in is some bars of clicks before the song moves. The
            // clock runs through them (it counts the clicks) but the scheduler
            // isn't started, so nothing sounds or records until it's done. The
            // bars are the song's, so 7/8 counts seven.
            //
            // Only when armed. A count-in is for recording, so a plain play
            // doesn't make you sit through four bars of clicks.
            // Started by a note (Transport::startsOnNote), the note was the
            // downbeat: no count-in, and the notes waiting are kept.
            const int32_t bars = transport.isRecordArmed() && !noteStart ? transport.countInBarsWanted() : 0;
            const int64_t ticks = bars > 0 ? static_cast<int64_t>(bars) * scheduler.songTicksPerBar() : 0;
            // Counted in frames, as a double. A block is 0.64 ticks at 120 bpm,
            // so counting whole ticks per block would drain the count too fast
            // and at some tempos never drain at all.
            countInFrames = static_cast<double>(ticks) * clock.samplesPerTickNow();
            countInPerTick = clock.samplesPerTickNow();
            preRollFrames = countInPerTick * static_cast<double>(kPreRollTicks);
            if (!noteStart) earlyCount = 0;
            startPending = countInFrames <= 0.0;
            // With Link, a plain play waits for the session's next downbeat so
            // we join its phase. A count-in has its own bar line, so it doesn't
            // wait. The pull brings its bar into line over the next one.
            //
            // Only when there are peers. Link is often left on with nobody else
            // in the session, and waiting up to a bar alone just looks like play
            // is broken.
            linkWaiting = startPending && transport.followingLink() &&
                          timebase.load(std::memory_order_acquire) != nullptr && linkInSession;
            if (linkWaiting) startPending = false;
        } else {
            scheduler.allNotesOff();
            scheduler.stopLauncher();
            transport.clearLaunchRequests();
            linkWaiting = false;
            timing = false;
            // If a lane pressed repeat and playback stopped before release, the
            // song would keep looping a beat in silence.
            master.perform.release();
            // And any mute waiting for a bar that won't come now.
            for (PendingParam &waiting : pendingParams) waiting.waiting = false;
            noteStart = false;
            passEnd = -1;
            // Stop goes back to the start of the song. There's no separate
            // pause, and a stop button is expected to go back to the top.
            //
            // Not in clip mode: every track is somewhere different so there's no
            // start to go back to, and stopping there has its own two-step
            // behaviour.
            if (!transport.launcherMode()) {
                clock.reset();
                scheduler.start(0);
            }
        }
        playing = transport.isPlaying();
        emitTransport(playing);
    }
    // Counting in. The clock is advanced by hand here because the
    // scheduler, which normally drives it, isn't running yet.
    if (countInFrames > 0.0) {
        countInFrames -= static_cast<double>(kBlockFrames);
        if (countInFrames <= 0.0) {
            countInFrames = 0.0;
            startPending = true; // the bar line the count-in was counting to
        }
    }
    transport.publishCountIn(countInPerTick > 0.0
                                 ? static_cast<int64_t>(countInFrames / countInPerTick)
                                 : 0);

    // A rewind while stopped: move the playhead to the start of the song so
    // the readout shows it. Before the start below, so a play in the same
    // block starts from there.
    if (transport.takeRewind()) {
        clock.reset();
        scheduler.start(0);
    }

    if (startPending) {
        if (transport.takeContinued()) {
            scheduler.resume();
        } else {
            scheduler.start(transport.requestedStartScene());
            playedFrames = 0;
        }
        timing = true;
        startPending = false;
    }

    drainClockIn();
    followExternal();
    followTimebase();

    if (racks[0].midiOutBound() == false) {
        for (int32_t r = 0; r < kRackCount; ++r) racks[r].bindMidiOut(&midiOut, r);
    }
    for (int32_t r = 0; r < kRackCount; ++r) racks[r].updateMidiOut(framesRendered);

    clock.advance(kBlockFrames);
    // Mounts before parameters. The UI queues a unit and then its values, so
    // this order puts the values on the new unit, not the old one.
    applyMounts();
    // Before the notes, so the block that starts recording knows when it did
    // and the block that ends a pass records none.
    endPassIfDone();
    drainMidi();
    drainParams();

    // Does any rack play frozen audio? Decided before the scheduler fires,
    // since a frozen rack gets no notes.
    {
        // Per rack, because in clip mode racks can be on different scenes
        // and frozen audio is stored per track and scene.
        for (int32_t r = 0; r < kRackCount; ++r) {
            racks[r].updateFrozen(scheduler.rackSceneId(r), clock.bpm(), playing, clock.isRamping());
            // And tell the machine, for machines that play the arrangement
            // rather than notes.
            racks[r].updateScene(scheduler.rackSceneId(r), scheduler.rackCycleTick(r), playing);
        }
    }

    // Where in the scene this block starts, before the scheduler moves on.
    const int64_t tickStart = scheduler.currentTickInIteration();
    const seq::SceneInfo *sceneBefore = scheduler.currentSceneInfo();
    const int32_t repeatBefore = scheduler.currentRepeat();

    // While counting in, the transport is playing (the clock runs and
    // counts the clicks) but the scheduler mustn't, or the song would
    // play under its own count-in.
    const bool counting = countInFrames > 0.0;
    // The bus renders count-in clicks even with the metronome off, and
    // borrows the limiter's headroom for them.
    master.setCountingIn(counting);
    // What the scheduler sends each rack is queued for the rack's own job.
    for (int32_t r = 0; r < kRackCount; ++r) racks[r].queue();
    if (playing && !counting) {
        scheduler.setSwingPair(swingPair.load(std::memory_order_relaxed));
        if (!scheduler.process(clock.blockStart(), clock.blockEnd())) {
            scheduler.allNotesOff();
            transport.stopFromAudioThread();
            playing = false;
        }
    } else if (!counting) {
        scheduler.applyIdleTempo();
    }

    // 24 pulses per quarter note, which at 240 PPQN is exactly every tenth
    // tick at any tempo. The clock runs whether or not the transport does,
    // as the MIDI spec asks.
    emitClock(clock.blockStart(), clock.blockEnd());

    // Metronome: every click boundary this block crossed, at its own sample
    // offset. The step is a bar or a division of the beat, and the accent
    // marks which, so fast divisions don't hide the beat.
    // A count-in always clicks, even with the metronome off.
    if ((playing && master.clickEnabled() && master.clickAllowed(transport.isRecordArmed())) || counting) {
        const int64_t stepTicks = master.clickStepTicks();
        auto accentFor = [](int64_t tickInBar, int64_t ticksPerBar) {
            if (ticksPerBar > 0 && tickInBar % ticksPerBar == 0) return static_cast<int32_t>(dsp::Click::Bar);
            if (tickInBar % kPPQN == 0) return static_cast<int32_t>(dsp::Click::Beat);
            return static_cast<int32_t>(dsp::Click::Division);
        };

        if (counting || scheduler.launcherActive()) {
            // No scene owns the bar line in clip mode, so the song's
            // signature counts from the transport's zero.
            const int64_t ticksPerBar = scheduler.songTicksPerBar();
            // A count-in counts beats, whatever the metronome is set to.
            const int64_t step = counting ? kPPQN
                                          : (stepTicks > 0 ? stepTicks : (ticksPerBar > 0 ? ticksPerBar : kPPQN));
            const int64_t from = clock.blockStart(), to = clock.blockEnd();
            int64_t t = (from / step) * step;
            if (t < from) t += step;
            for (; t < to; t += step) {
                const double at = clock.frameOffsetOfTick(t, kBlockFrames);
                master.clickAt(accentFor(t % ticksPerBar, ticksPerBar),
                               static_cast<int32_t>(at < 0.0 ? 0.0 : at));
            }
        } else if (sceneBefore != nullptr) {
            const int64_t tickEnd = scheduler.currentTickInIteration();
            const int64_t ticksPerBar = sceneBefore->ticksPerBar;
            const int64_t iterLen = sceneBefore->iterationTicks() > 0 ? sceneBefore->iterationTicks() : ticksPerBar;
            const int64_t step = stepTicks > 0 ? stepTicks : (ticksPerBar > 0 ? ticksPerBar : kPPQN);
            // The offset comes from the clock, which knows the sub-tick
            // phase. Working it out from the block start would make clicks
            // up to a tick late (2 ms at 120 bpm). An offset past this block
            // carries into the next one, which fast divisions need.
            const int64_t absStart = clock.blockStart();
            auto clicksIn = [&](int64_t from, int64_t to, int64_t baseOffsetTicks) {
                int64_t t = (from / step) * step;
                if (t < from) t += step;
                for (; t < to; t += step) {
                    const double at = clock.frameOffsetOfTick(absStart + baseOffsetTicks + t - from, kBlockFrames);
                    master.clickAt(accentFor(t % ticksPerBar, ticksPerBar),
                                   static_cast<int32_t>(at < 0.0 ? 0.0 : at));
                }
            };
            if (tickEnd >= tickStart) {
                clicksIn(tickStart, tickEnd, 0);
            } else { // wrapped an iteration inside this block
                clicksIn(tickStart, iterLen, 0);
                clicksIn(0, tickEnd, iterLen - tickStart);
            }
        }
    }

    // Scene fades: in over the first bar of the first pass, out over the last
    // bar of the last pass. Worked out from the position each block.
    float fade = 1.0f;
    if (playing && sceneBefore != nullptr && !scheduler.launcherActive()) {
        const int64_t tpb = sceneBefore->ticksPerBar;
        const int64_t iterLen = sceneBefore->iterationTicks();
        if (sceneBefore->fadeIn && repeatBefore == 0 && tickStart < tpb) {
            fade = static_cast<float>(tickStart) / static_cast<float>(tpb);
        } else if (sceneBefore->fadeOut && repeatBefore == sceneBefore->repeat - 1 && tickStart >= iterLen - tpb) {
            fade = static_cast<float>(iterLen - tickStart) / static_cast<float>(tpb);
        }
    }

    const auto tSeq = std::chrono::steady_clock::now();

    // Render the racks, then the master.
    int32_t *rackUsThisBlock = blockRackUs;
    bool *rackFrozenThisBlock = blockRackFrozen;
    for (int32_t r = 0; r < kRackCount; ++r) {
        blockRackUs[r] = 0;
        blockRackFrozen[r] = false;
    }
    // Sources before listeners. A rack whose compressor, gate or filter
    // listens to another is rendered after it, so the duck lands in the same
    // block as the kick. Worked out every block because a sidechain is a
    // parameter and can be automated. If two racks listen to each other, the
    // lower-numbered one goes first and hears the other a block late.
    int32_t order[kRackCount];
    sidechainOrder(order);
    for (int32_t n = 0; n < kRackCount; ++n) renderPlace[order[n]] = n;
    // Last block's taps, for a listener that renders before its source.
    for (int32_t r = 0; r < kRackCount; ++r) std::memcpy(racks[r].keyPrev, racks[r].keyBuf, sizeof(racks[r].keyBuf));
    // Each playing rack is a job. One that listens to a rack rendering before
    // it waits for that rack; one that renders after hears last block's key
    // and waits for nothing. On the workers, the audio thread works too.
    int32_t jobOf[kRackCount];
    int32_t jobs = 0;
    for (int32_t n = 0; n < kRackCount; ++n) {
        const int32_t r = order[n];
        jobOf[r] = -1;
        if (racks[r].isActive()) {
            jobOf[r] = jobs;
            jobRack[jobs++] = r;
        }
    }
    uint32_t needs[kRackCount]{};
    int32_t pick[kRackCount];
    for (int32_t j = 0; j < jobs; ++j) {
        const int32_t r = jobRack[j];
        const auto need = [&](int32_t source) {
            if (source >= 0 && source < kRackCount && source != r && jobOf[source] >= 0 &&
                renderPlace[source] < renderPlace[r]) {
                needs[j] |= 1u << jobOf[source];
            }
        };
        for (int32_t s = 0; s < kEffectSlots; ++s) {
            if (Effect *e = racks[r].currentEffect(s)) need(e->sidechainRack());
        }
        if (Machine *m = racks[r].currentMachine()) need(m->sidechainRack());
        // Heaviest first, by last block's cost; ties keep the render order.
        int32_t at = j;
        while (at > 0 && lastRackUs[jobRack[pick[at - 1]]] < lastRackUs[r]) {
            pick[at] = pick[at - 1];
            --at;
        }
        pick[at] = j;
    }
    int32_t lastTotal = 0;
    for (int32_t r = 0; r < kRackCount; ++r) lastTotal += lastRackUs[r];
    pool.run(jobs, needs, pick, &Engine::rackJob, this, lastTotal >= wakeFloorUs);
    if (const int32_t waited = pool.lastWaitUs(); waited > workerWaitPeakUs.load(std::memory_order_relaxed)) {
        workerWaitPeakUs.store(waited, std::memory_order_relaxed);
    }
    // What the racks sent out while rendering, in render order.
    for (int32_t j = 0; j < jobs; ++j) racks[jobRack[j]].sendHeld();
    for (int32_t r = 0; r < kRackCount; ++r) lastRackUs[r] = blockRackUs[r];
    // Anything queued for a rack with nothing to render it.
    for (int32_t r = 0; r < kRackCount; ++r) racks[r].playQueued();
    // Read after the racks, which is where a lane on a rack's output lands.
    settleRouting();

    const auto tRacks = std::chrono::steady_clock::now();

    // The same tick range the racks give their inserts, so a tempo-synced
    // effect behaves the same on a track or a send. Every rack has rendered
    // by now, so the sends' sidechain keys are always this block's.
    for (int32_t s = 0; s < kSendSlots; ++s) {
        if (Effect *e = master.send(s)) e->setKey(keyFor(e->sidechainRack(), -1));
    }
    for (int32_t s = 0; s < kMasterInsertSlots; ++s) {
        if (Effect *e = master.insert(s)) e->setKey(keyFor(e->sidechainRack(), -1));
    }
    for (int32_t g = 0; g < kGroupSlots; ++g) {
        for (int32_t s = 0; s < kGroupInsertSlots; ++s) {
            if (Effect *e = master.groupInsert(g, s)) e->setKey(keyFor(e->sidechainRack(), -1));
        }
    }
    // Where this block began to a fraction of a tick: the tick at the
    // block's end minus the frames between the block's start and that tick.
    master.perform.setTransport(
        playing, static_cast<double>(clock.blockEnd()) -
                     clock.frameOffsetOfTick(clock.blockEnd(), kBlockFrames) / clock.samplesPerTickNow());
    master.process(racks, kRackCount, out, kBlockFrames, clock.bpm(), fade, clock.blockStart(), clock.blockEnd());

    const auto tMaster = std::chrono::steady_clock::now();

    // Monitoring is after the master so it's heard at the master's level,
    // and isn't recorded when capturing the input.
    const InputBus &bus = InputBus::get();
    if (capture.armed()) {
        const int64_t framesBefore = capture.pushed();
        if (capture.source() != Capture::FromInput) {
            capture.push(out, kBlockFrames);
        } else if (bus.live()) {
            capture.push(bus.block(), kBlockFrames);
        } else {
            // No input open: record silence, and the screen can show it via
            // deaf(). Never fall back to recording the output.
            capture.pushSilence(kBlockFrames);
        }
        // Stamp where the song was. The mark uses the frame count from
        // before this block's push, which is the frame the cell starts at.
        //
        // If the ring dropped anything, every frame index after it is wrong,
        // so the marks are poisoned.
        //
        // Not while counting in or stopped. The scheduler's tick stands still
        // during a count-in, so a mark there would put the count-in's silence
        // at the top of the take.
        const int32_t armed = armedRack.load(std::memory_order_relaxed);
        if (playing && !counting && armed >= 0 && armed < kRackCount) {
            if (capture.overflowed()) marks.poison();
            marks.observe(framesBefore, scheduler.rackSceneId(armed),
                          scheduler.rackCycleTick(armed), scheduler.rackCycleTicks(armed),
                          clock.bpm());
        }
    }
    // After the capture, like the monitor, so auditioning a file doesn't
    // get recorded into the take.
    audition.mix(out, kBlockFrames);

    const float monitor = monitorLevel.load(std::memory_order_relaxed);
    if (monitor > 0.0001f && bus.live()) {
        const float *src = bus.block();
        for (int32_t i = 0; i < kBlockFrames * 2; ++i) out[i] += src[i] * monitor;
    }

    transport.publishPosition(scheduler.packedPosition());
    if (timing) playedFrames += kBlockFrames;
    transport.publishElapsed(playedFrames * 1000 / kSampleRate);
    framesRendered += kBlockFrames;

    // Block budget at 48 kHz / 64 frames is 1333 us.
    const auto tEnd = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - t0).count();
    const float pct = static_cast<float>(us) / 1333.3f * 100.0f;
    load.store(load.load(std::memory_order_relaxed) * 0.95f + pct * 0.05f, std::memory_order_relaxed);

    // The same span again as a peak rather than an average. The EMA above
    // decays before anyone reads it, so this shows how bad a dropout got.
    //
    // Was this block interrupted, or slow? Every span above is wall time, so
    // if the thread is taken off its core mid-block, whatever was being timed
    // gets blamed for it (a frozen rack showing 0.92 ms, or 20 ms in the
    // sequencer). Comparing the block's wall and CPU time catches this with
    // two clock reads. Timing every rack on the CPU clock would take 22, and
    // CLOCK_THREAD_CPUTIME_ID is a real syscall here. If the whole block ran
    // uninterrupted, every span inside it can be trusted.
    const int64_t cpuUs = threadCpuUs() - cpu0;
    // Time spent waiting for a worker's track isn't an interruption, though
    // the thread may have slept through it.
    const int32_t waitedUs = pool.lastWaitUs();
    const bool interrupted = us - cpuUs - waitedUs > kPreemptedUs;
    // An EMA of how often blocks are interrupted, so a per-track list that
    // never updates can be told apart from one with nothing to report. Like
    // the driver's stall counter, it says whether to optimise the DSP or
    // look at scheduling.
    const float wasInterrupted = interrupted ? 100.0f : 0.0f;
    interruptedPct.store(interruptedPct.load(std::memory_order_relaxed) * 0.99f + wasInterrupted * 0.01f,
                         std::memory_order_relaxed);
    if (interrupted) return;

    // CPU time rather than elapsed time. They're the same on a clean block,
    // and this only runs on clean blocks, unless it waited for a worker, when
    // the block took as long as the wait made it.
    keepPeak(blockPeak, static_cast<int32_t>(waitedUs > 0 ? us : cpuUs));
    const auto span = [](auto a, auto b) {
        return static_cast<int32_t>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    keepPeak(phasePeak[static_cast<size_t>(Phase::Input)], span(t0, tInput));
    keepPeak(phasePeak[static_cast<size_t>(Phase::Sequencer)], span(tInput, tSeq));
    keepPeak(phasePeak[static_cast<size_t>(Phase::Racks)], span(tSeq, tRacks));
    keepPeak(phasePeak[static_cast<size_t>(Phase::Master)], span(tRacks, tMaster));
    keepPeak(phasePeak[static_cast<size_t>(Phase::Capture)], span(tMaster, tEnd));
    for (int32_t r = 0; r < kRackCount; ++r) {
        if (rackUsThisBlock[r] > rackPeak[r].load(std::memory_order_relaxed)) {
            rackPeakFrozen[r].store(rackFrozenThisBlock[r], std::memory_order_relaxed);
        }
        keepPeak(rackPeak[r], rackUsThisBlock[r]);
        keepDecaying(rackRecent[r], rackUsThisBlock[r]);
        // Only blocks where this rack did something, so a track playing in
        // one scene reports what it costs while playing.
        if (rackUsThisBlock[r] > 0) {
            std::atomic<int32_t> &bin = rackHist[r][bucketOf(rackUsThisBlock[r])];
            bin.store(bin.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }
}

void Engine::rackJob(void *engine, int32_t job) {
    Engine &e = *static_cast<Engine *>(engine);
    const int32_t r = e.jobRack[job];
    e.renderRack(r, &e.blockRackUs[r], &e.blockRackFrozen[r]);
}

void Engine::renderRack(int32_t r, int32_t *usOut, bool *frozenOut) {
    Rack &rack = racks[r];
    const auto tRack = std::chrono::steady_clock::now();
    // On a worker, what it sends out waits for the audio thread (sendHeld).
    rack.holdOutgoing();
    // The notes, lanes and words the scheduler sent, first, as they would
    // have landed at the block's start.
    rack.playQueued();
    // Give each detector its key for this block (see keyFor), or silence for
    // an empty rack.
    for (int32_t s = 0; s < kEffectSlots; ++s) {
        if (Effect *e = rack.currentEffect(s)) e->setKey(keyFor(e->sidechainRack(), r));
    }
    // A machine can listen too: Diction mouthing another track's sound.
    if (Machine *m = rack.currentMachine()) m->setKey(keyFor(m->sidechainRack(), r));
    if (rack.frozenActive()) {
        rack.syncFrozen(scheduler.rackTick(r), clock.bpm());
    } else {
        rack.onBlock(clock.blockStart(), clock.blockEnd(), clock.bpm());
    }
    rack.render(kBlockFrames);
    // Held rather than published, since whether it was a cost or an
    // interruption isn't known until the whole block is timed.
    *usOut = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tRack).count());
    *frozenOut = rack.frozenActive();
}

void Engine::settleRouting() {
    // output 1..4 is a mixer group, anything else is the master.
    for (int32_t r = 0; r < kRackCount; ++r) {
        Rack &rack = racks[r];
        const int32_t g = rack.outputRequested();
        rack.routedTo = rack.isActive() && g >= 0 && g < kGroupSlots ? g : -1;
    }
}

const float *Engine::keyFor(int32_t source, int32_t self) const {
    static const float kSilence[kBlockFrames] = {};
    if (source < 0 || source >= kRackCount || source == self) return nullptr; // its own input
    if (!racks[source].isActive()) return kSilence;
    return self < 0 || renderPlace[source] < renderPlace[self] ? racks[source].keyBuf : racks[source].keyPrev;
}

void Engine::sidechainOrder(int32_t *order) const {
    bool placed[kRackCount] = {};
    const auto ready = [&](int32_t r) {
        const auto waiting = [&](int32_t src) {
            return src >= 0 && src < kRackCount && src != r && racks[src].isActive() && !placed[src];
        };
        for (int32_t s = 0; s < kEffectSlots; ++s) {
            const Effect *e = racks[r].currentEffect(s);
            if (e != nullptr && waiting(e->sidechainRack())) return false;
        }
        if (const Machine *m = racks[r].currentMachine(); m != nullptr && waiting(m->sidechainRack())) return false;
        return true;
    };
    int32_t n = 0;
    while (n < kRackCount) {
        bool progressed = false;
        for (int32_t r = 0; r < kRackCount; ++r) {
            if (placed[r] || !ready(r)) continue;
            placed[r] = true;
            order[n++] = r;
            progressed = true;
        }
        if (progressed) continue;
        // A loop: let the lowest rack still waiting go first.
        for (int32_t r = 0; r < kRackCount; ++r) {
            if (placed[r]) continue;
            placed[r] = true;
            order[n++] = r;
            break;
        }
    }
}

int32_t Engine::rackPercentileUs(int32_t rack, int32_t perMille) const {
    if (rack < 0 || rack >= kRackCount) return 0;
    int64_t total = 0;
    int32_t counts[kCostBuckets];
    for (int32_t i = 0; i < kCostBuckets; ++i) {
        counts[i] = rackHist[rack][i].load(std::memory_order_relaxed);
        total += counts[i];
    }
    if (total == 0) return 0;
    // From the top down: the bucket the tail reaches. perMille 990 means the
    // worst block in a hundred, so it needs at least a hundred blocks (a
    // tenth of a second) to mean anything.
    const int64_t want = total - total * perMille / 1000;
    int64_t seen = 0;
    for (int32_t i = kCostBuckets - 1; i >= 0; --i) {
        seen += counts[i];
        if (seen > want) return usOf(i);
    }
    return 0;
}

void Engine::resetRackCosts() {
    for (int32_t r = 0; r < kRackCount; ++r) {
        for (int32_t i = 0; i < kCostBuckets; ++i) rackHist[r][i].store(0, std::memory_order_relaxed);
    }
}

/**
 * A MIDI clock pulse at every tenth tick this block crossed, on the exact
 * frame it falls on rather than the block's first frame.
 */
void Engine::emitClock(int64_t blockStartTick, int64_t blockEndTick) {
    if (!transport.clockOut()) {
        lastClockTick = blockEndTick;
        return;
    }
    // (start, end]: the range whose frames the clock can place exactly.
    int64_t t = blockStartTick + 1;
    if (lastClockTick >= blockStartTick) t = lastClockTick + 1;
    for (; t <= blockEndTick; ++t) {
        if (t % (kPPQN / 24) != 0) continue;
        const double at = clock.frameOffsetOfTick(t, kBlockFrames);
        const int64_t frame = framesRendered + static_cast<int64_t>(at < 0.0 ? 0.0 : at);
        midiOut.push({frame, 0xf8, 0, 0, 0xff});
    }
    lastClockTick = blockEndTick;
}

/**
 * Start and stop. When the playhead isn't at the start of the song, a Song
 * Position Pointer followed by Continue, so a hardware sequencer joins at
 * the right bar.
 */
void Engine::emitTransport(bool nowPlaying) {
    if (!transport.clockOut()) return;
    if (!nowPlaying) {
        midiOut.push({framesRendered, 0xfc, 0, 0, 0xff});
        return;
    }
    int64_t songTick = 0;
    const seq::SongSnapshot *snap = scheduler.snapshot();
    if (snap != nullptr && !transport.launcherMode()) {
        songTick = snap->songTickAt(scheduler.currentScene(), scheduler.currentRepeat(),
                                    scheduler.currentTickInIteration());
    }
    // A MIDI beat is a sixteenth: 60 ticks at 240 PPQN.
    const int64_t beats = songTick / (kPPQN / 4);
    if (beats > 0) {
        midiOut.push({framesRendered, 0xf2, static_cast<uint8_t>(beats & 0x7f),
                      static_cast<uint8_t>((beats >> 7) & 0x7f), 0xff});
        midiOut.push({framesRendered, 0xfb, 0, 0, 0xff});
    } else {
        midiOut.push({framesRendered, 0xfa, 0, 0, 0xff});
    }
}

/**
 * Incoming MIDI clock. Each byte's frame was worked out from the audio
 * stream's anchor, so it's on the same timeline the clock counts in.
 */
void Engine::drainClockIn() {
    MidiInEvent e;
    while (clockIn.pop(e)) {
        switch (e.status) {
        case 0xf8:
            follower.pulse(e.frame);
            break;
        case 0xfa: // start: from the top
            follower.relocate(0);
            if (transport.followingMidi()) {
                scheduler.locateTo(0);
                transport.requestPlay(0);
            }
            break;
        case 0xfb: // continue: from wherever the Song Position Pointer left us
            if (transport.followingMidi()) {
                transport.requestContinue();
            }
            break;
        case 0xfc:
            if (transport.followingMidi()) {
                transport.requestStop();
            }
            break;
        case 0xf2: { // song position, in sixteenths
            const int64_t beats = static_cast<int64_t>(e.data1) | (static_cast<int64_t>(e.data2) << 7);
            const int64_t songTick = beats * (kPPQN / 4);
            follower.relocate(songTick);
            if (transport.followingMidi()) {
                scheduler.locateTo(songTick);
            }
            break;
        }
        default:
            break;
        }
    }
}

/**
 * Run the clock at the follower's rate and pull it gently until the
 * engine's position agrees with the master's.
 *
 * The rate alone keeps time but drifts in phase. The pull is slow (a few
 * percent of the error a block) so it sounds like easing into line rather
 * than a wavering tempo.
 */
void Engine::followExternal() {
    if (!transport.followingMidi() || !follower.running()) {
        return;
    }
    const double perTick = follower.framesPerTick();
    if (perTick < 1.0) {
        return;
    }
    double want = perTick;
    if (playing && follower.locked()) {
        const double theirs = follower.tickAt(framesRendered);
        const double ours = static_cast<double>(clock.position());
        const double errTicks = ours - theirs;
        const double pull = errTicks > 8.0 ? 8.0 : (errTicks < -8.0 ? -8.0 : errTicks);
        want = perTick * (1.0 + pull * 0.002);
    }
    clock.setExternalFramesPerTick(want);
    if (follower.stale(framesRendered)) {
        transport.publishSync(0);
    } else {
        const int32_t bpmMilli = static_cast<int32_t>(follower.bpm() * 100.0f);
        const int32_t errMicro = static_cast<int32_t>(follower.phaseErrorMs() * 1000.0f);
        transport.publishSync((static_cast<int64_t>(follower.locked() ? 1 : 0) << 56) |
                              (static_cast<int64_t>(bpmMilli & 0xffffff) << 32) |
                              (static_cast<int64_t>(errMicro) & 0xffffffffLL));
    }
}

/**
 * Following a Link session: the tempo and the bar line are the session's.
 *
 * Like the MIDI follower, but Link's state is already smooth so there's no
 * loop. Every block we run at the session tempo and pull gently until our
 * bar line matches theirs.
 *
 * Phase, not position. Link doesn't know about songs, only where in the
 * bar everyone is, so two devices playing different songs at the same tempo
 * are in time.
 */
void Engine::followTimebase() {
    Timebase *tb = timebase.load(std::memory_order_acquire);
    if (tb == nullptr || !transport.followingLink()) {
        linkWaiting = false;
        linkSeen = false;
        linkToldPlaying = false;
        linkInSession = false;
        return;
    }
    // Our bar length, before asking the session anything, since a phase only
    // means something against a bar both sides agree on.
    const double barTicks = static_cast<double>(scheduler.barTicks());
    if (barTicks > 0.0) tb->setQuantum(barTicks / static_cast<double>(kPPQN));

    const Timebase::State s = tb->capture(framesRendered);
    if (!s.valid || s.bpm < 1.0) {
        linkInSession = false;
        return;
    }
    // Read every block, because the decision that uses it (whether play
    // waits for a downbeat) is made at the top of a block before this runs.
    // A block old is close enough.
    linkInSession = seq::LinkFollower::waitsForDownbeat(s);

    const double perTick = clock.framesPerTickAt(s.bpm);
    const seq::LinkFollower::Advice advice = seq::LinkFollower::advise(
        s, static_cast<double>(scheduler.currentTickInIteration()), barTicks, perTick,
        playing && !linkWaiting);
    clock.setExternalFramesPerTick(advice.framesPerTick);

    // The downbeat we were waiting for.
    if (linkWaiting && advice.downbeat) {
        startPending = true;
        linkWaiting = false;
    }

    // Start and stop go both ways when the setting is on, and only on
    // changes, not on the current state. Otherwise a session nobody has
    // started reads as stopped, so pressing play would be undone every block
    // and the playhead would never move.
    //
    // Waiting for the downbeat isn't playing yet, so it isn't announced, or a
    // peer following it would start a bar before we did.
    const bool reallyPlaying = playing && !linkWaiting;
    if (!linkSeen) {
        linkSawPlaying = s.playing;
        linkToldPlaying = reallyPlaying;
        linkSeen = true;
    }
    if (syncStartStop.load(std::memory_order_relaxed)) {
        if (reallyPlaying != linkToldPlaying) {
            tb->proposePlaying(reallyPlaying);
            linkToldPlaying = reallyPlaying;
        }
        if (s.playing != linkSawPlaying) {
            if (s.playing && !playing) {
                transport.requestPlay(seq::Transport::kCurrentScene);
            } else if (!s.playing && playing) {
                transport.requestStop();
            }
        }
    } else {
        linkToldPlaying = reallyPlaying;
    }
    linkSawPlaying = s.playing;

    // Same packing as the MIDI follower so one readout handles both: locked,
    // the tempo in hundredths, and the error in microseconds.
    const int32_t bpmMilli = static_cast<int32_t>(s.bpm * 100.0);
    const double errMs = advice.errorTicks * perTick * 1000.0 / static_cast<double>(clock.rate());
    const int32_t errMicro = static_cast<int32_t>(errMs * 1000.0);
    transport.publishSync((static_cast<int64_t>(1) << 56) |
                          (static_cast<int64_t>(bpmMilli & 0xffffff) << 32) |
                          (static_cast<int64_t>(errMicro) & 0xffffffffLL));
}

void Engine::endPassIfDone() {
    // When recording starts, and with Transport::recordsOnce where its pass
    // ends: one pass of the scene in song mode; in clip mode it's known at
    // the first note (onModifiedNote), from that track's clip.
    const bool rec = recordingNow();
    if (rec && !wasRecording) {
        recordFrom = clock.position();
        passEnd = -1;
        if (transport.recordsOnce() && !scheduler.launcherActive()) {
            if (const seq::SceneInfo *sc = scheduler.currentSceneInfo()) {
                passEnd = recordFrom + std::max<int64_t>(1, sc->iterationTicks());
            }
        }
    }
    wasRecording = rec;
    if (rec && passEnd >= 0 && clock.position() >= passEnd) {
        transport.setRecordArmed(false);
        passEnd = -1;
    }
}

void Engine::drainMidi() {
    // Record the notes played just before the downbeat, now that there's a
    // scene. They go on tick 0, since the player meant the start of the bar.
    if (recordingNow() && earlyCount > 0) {
        for (int32_t i = 0; i < earlyCount; ++i) {
            const EarlyNote &e = earlyNotes[i];
            seq::RecordedEvent ev;
            ev.absTick = clock.position();
            ev.sceneId = scheduler.rackSceneId(e.rack);
            ev.tickInIteration = 0;
            ev.rack = e.rack;
            ev.cmd = e.status;
            ev.p1 = e.d1;
            ev.p2 = e.d2;
            recordQueue.push(ev);
        }
        earlyCount = 0;
        noteStart = false;
    }
    MidiMessage m;
    while (midiIn.pop(m)) {
        const int32_t rack = m.status & 0x0f;
        uint8_t status = m.status & 0xf0;
        uint8_t d2 = m.data2;
        if (status == 0x90 && d2 == 0) status = 0x80;
        if (!racks[rack].isActive()) continue;

        // Poly aftertouch is per-key pressure without MPE. The message names
        // its note, so it needs no zone or channel lookup.
        if (status == 0xa0) {
            racks[rack].noteExpression(0xd0, m.data1, d2, 0, mpeBendSemis);
            recordExpression(rack, 0xff, m.data1, 0xd0, d2, 0);
            continue;
        }
        // A member channel is one finger. Its note goes the normal way,
        // through the modifiers. Its bend, pressure and slide belong to that
        // note and go straight to the machine. Other controllers on a
        // member channel (mod wheel, sustain) aren't per finger and go to the
        // whole track, as they would on the master channel.
        const bool fingerCc = status == 0xb0 && m.data1 == 74;
        // Track which note each channel holds even before MPE is on. An auto
        // zone switches on at the second finger, and the first finger's bend
        // needs to find its note.
        if (m.channel < 16) {
            if (status == 0x90 && d2 > 0) {
                mpeChannelNote[m.channel] = m.data1;
                // A new finger starts a new curve, so clear what the last one
                // left so it isn't mistaken for a repeat.
                for (float &v : lastExprSent[m.channel]) v = -1.0f;
            } else if (status == 0x80 && mpeChannelNote[m.channel] == m.data1) {
                mpeChannelNote[m.channel] = -1;
            }
        }
        if (mpeMember(m.channel) && (status != 0xb0 || fingerCc)) {
            if (status != 0x90 && status != 0x80) {
                const int32_t held = mpeChannelNote[m.channel];
                // Expression for a finger that isn't down has nowhere to go.
                if (held < 0) continue;
                racks[rack].noteExpression(status, static_cast<uint8_t>(held), m.data1, d2,
                                           mpeBendSemis);
                recordExpression(rack, m.channel, static_cast<uint8_t>(held), status, m.data1, d2);
                continue;
            }
        }
        // Into the modifiers, and out the other end into onModifiedNote,
        // which records it if recording. What comes out of a chord or arp
        // is what the song keeps, not what the finger sent.
        racks[rack].handleMidi(status, m.data1, d2);
    }
}

/**
 * A note that came out of a rack's modifier chain.
 *
 * On the audio thread, inside the rack's delivery. Everything the chain
 * produces arrives here (the key itself, each note of a chord, each step
 * of an arpeggio) and while recording that's what goes into the clip.
 */
void Engine::onModifiedNote(int32_t rack, uint8_t status, uint8_t d1, uint8_t d2) {
    if (rack < 0 || rack >= kRackCount) return;
    // Played just before the downbeat while the scheduler isn't running yet:
    // hold it rather than lose it. Flushed at the start of the first block
    // that's actually recording, when the scene is known.
    if (!recordingNow()) {
        // Armed and stopped, waiting for a note: the first one starts the
        // song, and it and any played with it go at the start of the take.
        const bool on = (status & 0xf0) == 0x90 && d2 > 0;
        if (on && !noteStart && !playing && transport.isRecordArmed() && transport.startsOnNote()) {
            noteStart = true;
            transport.requestPlay();
        }
        if ((countInPreRoll() || noteStart) && earlyCount < kMaxEarlyNotes) {
            earlyNotes[earlyCount++] = {rack, status, d1, d2};
        }
        return;
    }
    // Recording once in clip mode: one pass of the clip the first note goes
    // into, counted from when recording started.
    if (transport.recordsOnce() && passEnd < 0 && scheduler.launcherActive()) {
        passEnd = recordFrom + std::max<int64_t>(1, scheduler.rackCycleTicks(rack));
    }
    seq::RecordedEvent ev;
    ev.absTick = clock.position();
    // The rack's own clip, not the scheduler's, so in clip mode a take lands
    // in the launched clip's cell.
    ev.sceneId = scheduler.rackSceneId(rack);
    ev.tickInIteration = scheduler.rackTick(rack);
    ev.rack = rack;
    ev.cmd = status;
    ev.p1 = d1;
    ev.p2 = d2;
    recordQueue.push(ev);
}

/**
 * A finger's bend, pressure or slide, recorded into the note's clip.
 *
 * Recorded against the note, not the channel, since the channel is just
 * how the controller sends it. Values are normalised the way songs store
 * them (bend in semitones scaled to MPE's maximum, not raw 14-bit), so a
 * take made with a 24-semitone controller plays back the same anywhere.
 */
void Engine::recordExpression(int32_t rack, uint8_t channel, uint8_t note, uint8_t status, uint8_t d1,
                              uint8_t d2) {
    if (!recordingNow()) return;
    Expr kind;
    float value;
    switch (status) {
    case 0xe0: {
        const float bend14 = static_cast<float>((d2 << 7) | d1) - 8192.0f;
        kind = Expr::Bend;
        value = exprBendTo01(bend14 / 8192.0f * mpeBendSemis);
        break;
    }
    case 0xd0:
        kind = Expr::Pressure;
        value = expr7To01(d1);
        break;
    case 0xb0:
        if (d1 != 74) return; // CC 74 is slide, other CCs aren't per note
        kind = Expr::Timbre;
        value = expr7To01(d2);
        break;
    default: return;
    }
    // A finger holding still sends the same value over and over, especially
    // 7-bit pressure. Dropping repeats here keeps five fingers from
    // overflowing the queue.
    if (channel < 16) {
        float &last = lastExprSent[channel][static_cast<int32_t>(kind)];
        if (last == value) return;
        last = value;
    }
    seq::RecordedEvent ev;
    ev.absTick = clock.position();
    ev.sceneId = scheduler.rackSceneId(rack);
    ev.tickInIteration = scheduler.rackTick(rack);
    ev.rack = rack;
    ev.cmd = seq::kRecNoteExpression;
    ev.p1 = note;
    ev.p2 = static_cast<uint8_t>(kind);
    ev.value = value;
    recordQueue.push(ev);
}

void Engine::drainParams() {
    ParamMessage p;
    while (paramsIn.pop(p)) {
        if (p.unit == Unit::Master) {
            master.params().set(p.index, p.value);
        } else if (p.unit >= Unit::Group1Fx1 && p.unit <= Unit::Group4Fx2) {
            const int32_t k = static_cast<int32_t>(p.unit) - static_cast<int32_t>(Unit::Group1Fx1);
            Effect *fx = master.groupInsert(k / kGroupInsertSlots, k % kGroupInsertSlots);
            if (fx != nullptr) {
                if (p.index == kEffectBypassIndex) fx->setBypass(p.value >= 0.5f);
                else fx->params().set(p.index, p.value);
            }
        } else if (p.unit == Unit::MasterFx1 || p.unit == Unit::MasterFx2) {
            Effect *fx = master.insert(p.unit == Unit::MasterFx1 ? 0 : 1);
            if (fx != nullptr) {
                if (p.index == kEffectBypassIndex) fx->setBypass(p.value >= 0.5f);
                else fx->params().set(p.index, p.value);
            }
        } else if (p.unit == Unit::Send1 || p.unit == Unit::Send2) {
            Effect *fx = master.send(p.unit == Unit::Send1 ? 0 : 1);
            if (fx != nullptr) {
                if (p.index == kEffectBypassIndex) fx->setBypass(p.value >= 0.5f);
                else fx->params().set(p.index, p.value);
            }
        } else if (p.unit == Unit::Input1 || p.unit == Unit::Input2) {
            Effect *fx = inputFx[p.unit == Unit::Input1 ? 0 : 1];
            if (fx != nullptr) {
                if (p.index == kEffectBypassIndex) fx->setBypass(p.value >= 0.5f);
                else fx->params().set(p.index, p.value);
            }
        } else if (p.rack >= 0 && p.rack < kRackCount) {
            if (p.quantise > 0 && playing) {
                // Waiting for the rack's next bar line. A newer one replaces it.
                const int64_t q = p.quantise;
                const int64_t into = scheduler.rackTick(p.rack) % q;
                PendingParam &waiting = pendingParams[p.rack];
                waiting.message = p;
                waiting.message.quantise = 0;
                waiting.due = clock.position() + (into == 0 ? 0 : q - into);
                waiting.waiting = true;
            } else {
                applyRackParam(p);
            }
        }
    }
    // And apply whatever has reached its bar line.
    for (int32_t r = 0; r < kRackCount; ++r) {
        PendingParam &waiting = pendingParams[r];
        if (waiting.waiting && clock.position() >= waiting.due) {
            waiting.waiting = false;
            applyRackParam(waiting.message);
        }
    }
}

void Engine::applyRackParam(const ParamMessage &p) {
    racks[p.rack].setParam(p.unit, p.index, p.value);
    if (p.record && recordingNow()) {
        racks[p.rack].touch(p.unit, p.index);
        seq::RecordedEvent ev;
        ev.absTick = clock.position();
        ev.sceneId = scheduler.rackSceneId(p.rack);
        ev.tickInIteration = scheduler.rackTick(p.rack);
        ev.rack = p.rack;
        ev.cmd = seq::kRecParam;
        ev.p1 = static_cast<uint8_t>(p.unit);
        ev.p2 = 0;
        ev.paramIndex = p.index;
        ev.value = p.value;
        recordQueue.push(ev);
    }
}

// Mounts per block are capped to bound the worst case. The UI's builder
// retries when the queue is full.
void Engine::applyMounts() {
    // Every mount queued so far, up to the cap. A swap is a pointer exchange
    // and a retire push, so a burst (a song loading its machines and effects)
    // lands in one block, ahead of the parameters queued after it.
    for (int32_t n = 0; n < kMaxMountsPerBlock; ++n) {
        Mount m;
        if (!mounts.pop(m)) return;
        applyMount(m);
    }
}

Effect *Engine::effectAt(int32_t rack, Unit unit) {
    const int u = static_cast<int>(unit);
    switch (unit) {
    case Unit::Effect1:
    case Unit::Effect2:
        return rack >= 0 && rack < kRackCount ? racks[rack].currentEffect(unit == Unit::Effect1 ? 0 : 1) : nullptr;
    case Unit::Send1:
    case Unit::Send2:
        return master.send(unit == Unit::Send1 ? 0 : 1);
    case Unit::MasterFx1:
    case Unit::MasterFx2:
        return master.insert(unit == Unit::MasterFx1 ? 0 : 1);
    case Unit::Input1:
    case Unit::Input2:
        return inputFx[unit == Unit::Input1 ? 0 : 1];
    default:
        break;
    }
    if (u >= static_cast<int>(Unit::Group1Fx1) && u <= static_cast<int>(Unit::Group4Fx2)) {
        const int k = u - static_cast<int>(Unit::Group1Fx1);
        return master.groupInsert(k / kGroupInsertSlots, k % kGroupInsertSlots);
    }
    return nullptr;
}

void Engine::applyMount(const Mount &m) {
    switch (m.kind) {
    case Mount::Kind::Machine:
        if (m.rack >= 0 && m.rack < kRackCount) {
            retirer.retire(racks[m.rack].swapMachine(static_cast<Machine *>(m.object)), deleteAs<Machine>);
        } else {
            retirer.retire(m.object, deleteAs<Machine>);
        }
        break;
    case Mount::Kind::Effect:
        if (m.rack >= 0 && m.rack < kRackCount) {
            retirer.retire(racks[m.rack].swapEffect(m.slot, static_cast<Effect *>(m.object)), deleteAs<Effect>);
        } else {
            retirer.retire(m.object, deleteAs<Effect>);
        }
        break;
    case Mount::Kind::Input:
        if (m.slot >= 0 && m.slot < kInputSlots) {
            Effect *wasThere = inputFx[m.slot];
            inputFx[m.slot] = static_cast<Effect *>(m.object);
            retirer.retire(wasThere, deleteAs<Effect>);
        } else {
            retirer.retire(m.object, deleteAs<Effect>);
        }
        break;
    case Mount::Kind::GroupInsert:
        // The rack field carries the group.
        retirer.retire(master.swapGroupInsert(m.rack, m.slot, static_cast<Effect *>(m.object)), deleteAs<Effect>);
        break;
    case Mount::Kind::MasterInsert:
        retirer.retire(master.swapInsert(m.slot, static_cast<Effect *>(m.object)), deleteAs<Effect>);
        break;
    case Mount::Kind::Send:
        // swapSend checks the slot and returns whatever it replaced, or the
        // new one if the slot was invalid.
        retirer.retire(master.swapSend(m.slot, static_cast<Effect *>(m.object)), deleteAs<Effect>);
        break;
    case Mount::Kind::InputMod:
        if (m.rack >= 0 && m.rack < kRackCount) {
            retirer.retire(racks[m.rack].swapInputMod(m.slot, static_cast<InputMod *>(m.object)), deleteAs<InputMod>);
        } else {
            retirer.retire(m.object, deleteAs<InputMod>);
        }
        break;
    case Mount::Kind::Object: {
        void *back = m.object;
        if (m.rack >= 0 && m.rack < kRackCount && racks[m.rack].currentMachine() != nullptr) {
            back = racks[m.rack].currentMachine()->swapObject(m.slot, m.object);
        }
        if (m.deleter != nullptr) retirer.retire(back, m.deleter);
        break;
    }
    case Mount::Kind::EffectObject: {
        Effect *target = effectAt(m.rack, static_cast<Unit>(m.slot));
        void *back = target != nullptr ? target->swapObject(0, m.object) : m.object;
        if (m.deleter != nullptr) retirer.retire(back, m.deleter);
        break;
    }
    case Mount::Kind::Frozen: {
        if (m.rack >= 0 && m.rack < kRackCount) {
            const FrozenSet *old = racks[m.rack].swapFrozen(static_cast<const FrozenSet *>(m.object));
            retirer.retire(const_cast<FrozenSet *>(old), deleteAs<FrozenSet>);
        } else if (m.deleter != nullptr) {
            retirer.retire(m.object, m.deleter);
        }
        break;
    }
    case Mount::Kind::Song: {
        const seq::SongSnapshot *old = scheduler.swapSnapshot(static_cast<const seq::SongSnapshot *>(m.object));
        retirer.retire(const_cast<seq::SongSnapshot *>(old), deleteAs<seq::SongSnapshot>);
        break;
    }
    default:
        break;
    }
}

} // namespace acidulous
