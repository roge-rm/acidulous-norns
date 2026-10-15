#pragma once
#include <engine/dsp/Loudness.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <sequencer/Clip.h>
#include <engine/core/Reel.h>
#include <engine/core/SampleEdit.h>
#include <engine/format/AudioSink.h>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

// The app's handle on the engine. Owns the Engine and the audio stream and
// exposes the small surface the JNI bridge needs.
//
// Thread model:
//   - everything here is called from the JNI (UI) thread;
//   - nothing here blocks the audio thread: calls either enqueue, or build an
//     object and enqueue a Mount for it.

namespace acidulous {

class EngineHost {
  public:
    static EngineHost &instance();

    bool start();
    void stop();
    bool isRunning() const { return running; }

    // Builds the machine here, hands it to the audio thread through a Mount.
    bool mountMachine(int rack, const std::string &typeName);
    void unmountMachine(int rack);
    // Insert effects: two slots per rack. An empty type name clears the slot.
    bool mountEffect(int rack, int slot, const std::string &typeName);
    bool mountSend(int slot, const std::string &typeName);
    bool mountMasterInsert(int slot, const std::string &typeName);
    /** Group [group]'s insert [slot], in the mixer. */
    bool mountGroupInsert(int group, int slot, const std::string &typeName);
    float groupPeak(int group);
    /**
     * An effect on the input, before anything hears it. Unlike a track's
     * insert it's recorded into the take, since it runs before the capture.
     * Unlike a send, its mix knob is left alone, since it's in series.
     */
    bool mountInputEffect(int slot, const std::string &typeName);
    std::string loadReel(int rack, const std::string &spec);
    /**
     * Where converted long takes are stored. Set once at startup. If empty,
     * long takes are held in memory up to the resident limit instead.
     */
    void setCacheRoot(const std::string &path) { cacheRoot = path; }
    const char *mountedEffect(int rack, int slot) const;
    bool mountInputMod(int rack, int slot, const std::string &typeName);
    // Builds anything a machine needs before it can be mounted (Trinity's
    // wavetables). Safe to call from a worker at startup. Mounting waits on it.
    static void prewarm();

    // Decodes a WAV here and mounts it into the machine's `slot` (a pad). An
    // empty path clears the slot. Returns false if the file can't be read.
    // [maxSeconds] is how much of a long file to keep (see kMaxDecodeSeconds
    // and kMaxSliceSeconds).
    bool loadSample(int rack, int slot, const std::string &path, std::string &error, int maxSeconds = 0);

    // --- Multisample maps (Mosaic) ------------------------------------------
    // Both build the whole instrument on the calling thread and hand it over
    // as one object mount, so call them from a worker.
    static std::string soundFontPresets(const std::string &path, std::string &error);
    bool loadSoundFont(int rack, const std::string &path, int presetIndex, std::string &error);
    /** One zone per line: path|lowKey|highKey|rootKey|lowVel|highVel|cents|gain|pan|loop */
    bool loadZoneMap(int rack, const std::string &spec, const std::string &name, std::string &error);
    /** "name|zones|samples|seconds" for the mounted map, or "". */
    std::string sampleMapInfo(int rack) const;

    /** Builds a Nexus patch from its text and mounts it. Returns "" or an error. */
    std::string loadNexusPatch(int rack, const std::string &spec);
    /** The palette, so the editor doesn't keep its own copy. */
    std::string nexusPalette() const;
    /** The scope trace from a rack's Nexus, into a caller-owned array. */
    int32_t nexusScope(int rack, float *dest, int32_t max) const;
    /**
     * Where a file's slices fall, as fractions of its length.
     *
     * [mode] 0 finds transients and 1 divides evenly. Returns [count]+1
     * boundaries as "a,b,c,...", so slice n runs from boundary n to n+1.
     * Empty on failure, with [error] set.
     */
    std::string slicePoints(const std::string &path, int mode, int count, std::string &error) const;
    /** "bars|seconds" for a loop file, as guessed for playing at the song's tempo. */
    std::string loopShape(const std::string &path, std::string &error) const;

    /**
     * Converts an imported file into a WAV the rest of the app can read.
     *
     * Returns the path to use: the same one if it was already a WAV,
     * otherwise a new `.wav` beside it with the original removed. Empty on
     * failure, with [error] saying what the file was and what went wrong.
     *
     * Converting once here means everything else only has to open WAVs.
     */
    std::string importAudio(const std::string &path, std::string &error, int maxSeconds = 0) const;

    /**
     * A pad's sample as a waveform to draw: [columns] pairs of min and max.
     * [dest] needs 2 * [columns] floats. Returns how many columns were
     * filled, 0 when the pad is empty.
     *
     * [fromFrame] and [toFrame] set the range to draw, for zooming. An empty
     * range means the whole sample. Frames, not fractions, because a float
     * fraction isn't precise enough on a long file.
     */
    int32_t sampleShape(int rack, int pad, float *dest, int32_t columns, int32_t fromFrame = 0,
                        int32_t toFrame = 0) const;
    /**
     * The same as `sampleShape`, but for a file instead of a mounted pad, so
     * the sample editor works for any machine and for unmounted recordings.
     *
     * Decodes on the calling thread, which must be a worker.
     */
    int32_t fileShape(const std::string &path, float *dest, int32_t columns,
                      int32_t fromFrame = 0, int32_t toFrame = 0) const;
    /** "name|frames|channels|rate|peak" for a file on disk, "" if unreadable. */
    std::string fileInfo(const std::string &path) const;
    /**
     * [fileInfo] and [fileShape] from a single decode, for a take going onto
     * a Bias lane. See the note in `assemble`.
     */
    std::string fileSurvey(const std::string &path, float *dest, int32_t columns) const;
    /**
     * Cuts a recorded voice take for Diction (see diction::cutTake): kind 0
     * held, 1 glide, 2 between; [consonantNear] in seconds, or below 0 for
     * nowhere in particular. Returns "problem|start|end|holdFrom|holdTo|
     * glideFrom|glideTo|consonantFrom|consonantTo|rootHz|centsOff", in frames
     * at the engine rate.
     */
    std::string cutTake(const std::string &path, int32_t kind, float noteHz, float consonantNear) const;
    /**
     * Reads [src], applies [ops] and writes [dst]. Returns "" or an error.
     *
     * [dst] may be [src] to overwrite it. Written to a temporary file and
     * renamed, so a failure leaves the original intact.
     */
    std::string editSample(const std::string &src, const std::string &dst,
                           const audio::SampleOps &ops) const;
    /**
     * Plays a file once to preview it. An empty path stops it. Not part of
     * the song, so it isn't recorded, exported or frozen. Panic stops it.
     */
    std::string auditionFile(const std::string &path);
    /**
     * The Sound window's edit, heard and seen before it's applied. Applies
     * [ops] to [src] in memory, the kept part only and put back in place so
     * it lines up with the file, and fills [dest] like fileShape. If the
     * preview is playing, the new version takes over where it is. Nothing is
     * written. A worker thread, never the audio or main thread.
     */
    int32_t editPreview(const std::string &src, const audio::SampleOps &ops, float *dest, int32_t columns,
                        int32_t fromFrame, int32_t toFrame);
    /** Plays the last editPreview from the start. */
    std::string auditionPreview();
    bool auditioning() const;
    /** How far through the audition, 0..1, or -1 when nothing is playing. */
    float auditionProgress() const;

    int32_t nexusActivity(int rack, float *dest, int32_t max) const;
    // "name|frames|stereo" for a loaded slot, "" for none. UI thread.
    std::string sampleInfo(int rack, int slot) const;
    const char *mountedMachine(int rack) const;
    // Resolves a parameter name for a unit; -1 if unknown. UI thread.
    int paramIndex(const std::string &machineType, const std::string &unit, const std::string &name) const;

    // --- Offline render ---------------------------------------------------------
    /** One file of a render: the master mix, or one rack on its own. */
    struct RenderTarget {
        std::string path;
        int32_t rack = -1; // -1 is the master mix
    };

    // Blocks. Renders the whole song from the top (song loop and metronome
    // off) plus `tailSeconds` of tail, then gives the stream back to the
    // device. Call from a worker thread.
    bool renderSong(const std::string &path, float tailSeconds, AudioFormat format, int32_t bits,
                    std::string &error, int32_t startScene = 0, float maxSeconds = 0.0f);
    /**
     * Renders the song once, writing several files at the same time.
     *
     * Each rack's buffer is copied post-fader, post-pan and post-mute, which
     * is what it adds to the mix. Solo isn't applied, and the master bus
     * (sends, volume, limiter) isn't in any single stem.
     */
    bool renderStems(const std::vector<RenderTarget> &targets, float tailSeconds, AudioFormat format,
                     int32_t bits, std::string &error, int32_t startScene = 0, float maxSeconds = 0.0f);
    void cancelRender() { renderCancel.store(true, std::memory_order_relaxed); }
    bool isRendering() const { return rendering.load(std::memory_order_relaxed); }
    float renderedSeconds() const { return renderSeconds.load(std::memory_order_relaxed); }
    float renderedPeak() const { return renderPeak.load(std::memory_order_relaxed); }

    void noteOn(int rack, uint8_t note, uint8_t velocity);
    void noteOff(int rack, uint8_t note);
    // Performance controllers. Sent as MIDI so the modifier chain and a USB
    // controller share one path into the machine.
    void controlChange(int rack, uint8_t cc, uint8_t value, bool record = true);
    void channelPressure(int rack, uint8_t value, bool record = true);
    // A channel message straight from a MIDI port, re-addressed to [rack].
    void midiEvent(int rack, uint8_t status, uint8_t d1, uint8_t d2, uint8_t channel = kNoChannel);
    /** The input's MPE zone: kind 0 off, 1 lower, 2 upper. */
    void setMpeZone(int kind, int members, float bendSemis);
    bool mpeMemberChannel(uint8_t channel) const;
    /** Which member channels are holding a note, a bit per channel. */
    int mpeHeldMask() const;

    // unit: "machine" | "effect1" | "effect2" | "mod1" | "mod2" | "channel".
    // value is normalised 0..1. Names are resolved here, on the UI thread.
    // `record`: a user gesture that can be recorded, not the song syncing state.
    bool setParam(int rack, const std::string &unit, const std::string &name, float value, bool record,
                  int quantise = 0);

    // --- Transport -----------------------------------------------------------
    void transportPlay(int sceneIdx);
    void transportStop();
    void transportRewind();
    bool isPlaying() const;
    void setLoopScene(bool on);
    void setLoopSong(bool on);
    void setRecordArmed(bool on);
    void setStopAtEnd(bool on);
    bool isStopAtEndArmed() const;
    void queueScene(int idx);
    int queuedScene() const;
    bool isRecordArmed() const;
    void setTempo(float bpm);
    float tempo() const;
    int64_t positionPacked() const;

    // Clip mode: the grid as a launcher instead of an arranger.
    void setLauncher(bool on);
    /** Whether Fill trigs play. Set while the Fill button is held. */
    void setFill(bool on);
    void setLaunchQuantise(int32_t ticks);
    void launchClip(int32_t rack, int64_t sceneId);
    /** Clip mode: starts a scene's clips and stops every other track on the same tick. */
    void launchScene(int64_t sceneId);
    void stopAllClips();
    void cancelLaunch(int32_t rack);
    void launchStates(int64_t *out, int32_t count) const;

    // MIDI out: the queue the audio thread fills, and the anchor that turns
    // a frame into a time Android can schedule.
    void setClockOut(bool on);
    int drainMidiOut(int64_t *out, int maxEvents);
    bool audioAnchor(int64_t &frame, int64_t &nanos, int32_t &sampleRate) const;
    void setExternalSync(bool on);
    /** A track's tuning: 128 ratios to equal temperament, or null for none. */
    void setTuning(int rack, const float *ratios);

    // --- Ableton Link -------------------------------------------------------
    /** Turning it on opens the discovery sockets and offers our tempo. */
    void setLinkEnabled(bool on);
    bool linkEnabled() const;
    /** Whether a peer starting or stopping starts and stops us too. */
    void setLinkStartStop(bool on);
    /**
     * Peers and session tempo for the readout. Also passes the stream's
     * current anchor to Link, so poll it. Packed: peers in the top word,
     * tempo in hundredths in the bottom.
     */
    int64_t linkStatus();
    void midiClockIn(int64_t frame, uint8_t status, uint8_t d1, uint8_t d2);
    int64_t syncState() const;

    // Drain stamped live events into `out`, 5 longs per event:
    //   absTick, sceneId, tickInIteration, (rack << 24 | cmd << 16 | p1 << 8 | p2),
    //   and for parameter events (cmd 0xf0, p1 = unit): (index << 32 | float bits of value)
    int drainRecorded(int64_t *out, int maxEvents);
    uint32_t recordedDropped() const;

    // --- Song snapshot builder -------------------------------------------------
    int64_t snapshotBegin();
    bool snapshotAddScene(int64_t handle, int64_t sceneId, int ticksPerBar, int repeat, float bpmOverride,
                          float rampToBpm, int rampBars,
                          bool smooth, bool fadeIn, bool fadeOut);
    bool snapshotSetClipCached(int64_t handle, int rack, int scene, int64_t rev);
    // notes: flat [tick, length, pitch, velocity, curvePointCount, trig] x count,
    // where `trig` is seq::packTrig's word. `seed` is the clip's own dice seed.
    // Bit 1 of `playMode` makes the dice roll freely instead of seeded.
    bool snapshotSetClip(int64_t handle, int rack, int scene, int64_t rev, int bars, int playMode, bool mute,
                         int seed, const int32_t *notes, int noteCount, const float *expr, int exprCount,
                         const char *lyrics = nullptr);
    // points: flat [tick, value] × count, any order. unit/name resolve against
    // `machineType`'s table (for "machine") or the channel table.
    bool snapshotSetLane(int64_t handle, int rack, int scene, const std::string &machineType,
                         const std::string &unit, const std::string &name, bool linear,
                         const float *points, int pointCount);
    bool snapshotCommit(int64_t handle);
    void snapshotAbandon(int64_t handle);

    /**
     * Builds Cumulus's tables for a rack from its spectrum parameters and
     * mounts them. Takes tens of ms and a few MB, so worker thread only.
     */
    std::string buildCloud(int rack, const float *spectrum01, int32_t count);

    /**
     * Decodes a WAV, finds its transients and mounts the take for Pollen or
     * Dice. Worker thread only. Returns "" or the reason it failed.
     */
    std::string loadTake(int rack, const std::string &path);

    /**
     * A sung take for Molt: decoded, summed to mono and pitch-marked on a
     * worker, then mounted. An empty path clears it.
     */
    std::string loadUtterance(int rack, const std::string &path);
    /**
     * A recorded voice for the Diction on [rack], from [spec]: a line per
     * held vowel, "V|PHONE|path|holdFrom|holdTo", per diphthong,
     * "D|PHONE|path|holdFrom|holdTo|glideFrom|glideTo", and per consonant,
     * "C|PHONE|VOWEL|path|from|to", in frames. Empty puts the built-in voice
     * back. Returns "" or why it failed. Worker thread.
     */
    std::string loadVoice(int rack, int slot, const std::string &spec);

    /**
     * Compiles Formulate's expression and its three step tables and mounts
     * them. Returns "" or the parse error, which the panel shows.
     */
    std::string loadFormula(int rack, const std::string &formula, const std::string &arp,
                            const std::string &duty, const std::string &vol);

    /**
     * Compiles a Formula effect's expression and hands it to the effect in
     * [unit]'s slot ("effect1", "send2", "master1", "group3fx2", "input1"...;
     * [rack] for a track insert). Returns "" or the parse error.
     */
    std::string loadEffectFormula(int rack, const std::string &unit, const std::string &formula);

    // --- Freeze ---------------------------------------------------------
  private:
    bool renderTargets(const std::vector<RenderTarget> &targets, float tailSeconds, AudioFormat format,
                       int32_t bits, std::string &error, int32_t startScene, float maxSeconds,
                       dsp::Loudness *measure = nullptr);
  public:
    /**
     * Renders the song like an export, without writing a file, and measures
     * integrated LUFS and true peak dBTP. The first pass of a normalised
     * export. Renders are repeatable, so the second pass matches.
     */
    bool measureLoudness(float tailSeconds, int32_t startScene, float maxSeconds, float &lufs, float &truePeak,
                         std::string &error);
    /** Gain in dB applied to what the next renders write. 0 for none. */
    void setRenderGain(float db) { renderGainDb = db; }
  private:
    float renderGainDb = 0.0f;

  public:
    /**
     * Renders one clip to a WAV offline: the rack's output after its effects
     * and before its channel strip, exactly one clip long. Returns "" on
     * success and fills in the outputs, or returns the error.
     *
     * [tailSeconds] is the maximum ring-out. The render stops once the sound
     * has decayed, and [tailOut] says how much was kept. The tail is stored
     * after the clip and isn't part of the loop.
     */
    std::string freezeClip(int rack, int64_t sceneId, const std::string &path, float tailSeconds,
                           int32_t &framesOut, int32_t &tailOut, int32_t &ticksOut, float &bpmOut,
                           float &peakOut);

    /**
     * Mixes one Bias cell's four lanes down to one file (a comp).
     *
     * Doesn't use the freeze renderer, which would run the scheduler and
     * include the track's inserts. It walks the cell's cycle with the tape
     * medium switched off, so only the lanes are flattened.
     *
     * [frames] and [bpm] are the cell's, passed in from the song. Returns ""
     * or an error.
     */
    std::string compCell(int rack, int64_t sceneId, int32_t frames, float bpm,
                         const std::string &path, float &peakOut);

    /**
     * Gives a rack its frozen clips as pairs of scene id and WAV path, read
     * here and mounted as one object. An empty list unfreezes the rack.
     */
    std::string loadFrozenSet(int rack, const std::vector<std::pair<int64_t, std::string>> &clips,
                              const std::vector<float> &bpms, const std::vector<int32_t> &ticks,
                              const std::vector<int32_t> &tails);

    // --- Settings that belong to the device -----------------------------
    /** Output buffer depth in bursts: 1 tight, 2 default, 4 safe. */
    void setBufferBursts(int32_t bursts);
    /** The smallest output buffer this device holds, frames (Android; 0 elsewhere). See AudioDriver::tuneBuffer. */
    void setBufferFloor(int32_t frames);
    int32_t bufferFloor() const;
    int32_t bufferFrames() const;
    /** Held notes per rack, 0 for no limit. */
    void setVoiceLimit(int32_t notes);
    /** 1 full, 0 lean: reverb density and distortion oversampling. */
    void setQuality(int32_t level);
    /** Bits in a recorded or exported WAV: 24 or 16. */
    void setRecordBits(int32_t bits);

    // --- Diagnostics ----------------------------------------------------------
    int32_t sampleRate() const;
    int32_t framesPerBurst() const;
    bool lowLatency() const;
    int64_t xRunCount() const;
    float loadPercent() const;
    /**
     * Worst-case timings, which are what matter for dropouts.
     *
     * Each is peak-hold and cleared when read, so only the diagnostics poll
     * should read them.
     */
    int32_t worstBlockUs();
    int32_t worstCallbackUs();
    int32_t worstPhaseUs(int32_t phase);
    int32_t worstRackUs(int32_t rack);
    /** The rack's 99th-percentile block time, so one bad block doesn't set it. */
    int32_t rackPercentileUs(int32_t rack) const;
    void resetRackCosts();
    /** Whether the rack was frozen when it set its peak. Read before worstRackUs, which clears it. */
    bool worstRackWasFrozen(int32_t rack) const;
    /** How often a block is interrupted instead of slow, 0 to 100. */
    float interruptedPercent() const;
    /** Whether the scheduler hint for our deadline is running, and whether it's available. */
    bool hintRunning() const;
    bool hintAvailable() const;
    /** 0 no API, 1 waiting, 2 the audio thread never registered, 3 refused, 4 on. */
    int32_t hintState() const;
    /** How many fast cores the audio thread is pinned to; 0 when it isn't (one kind of core, or not a phone). */
    int32_t fastCores() const;
    int32_t rackCostUs(int32_t rack) const;
    int32_t worstCallbackCpuUs();
    int32_t recentCallbackUs() const;
    int64_t lateCallbacks() const;
    int64_t stalledCallbacks() const;
    int32_t callbackBudgetUs() const;
    float peakLevel() const;
    /** Momentary, short-term and integrated LUFS and true peak dBTP; see MasterBus::readLoudness. */
    void loudness(float *out4);
    void resetLoudness();
    float rackPeak(int rack) const;
    float masterFade() const;
    // --- Audio in -------------------------------------------------------
    /** [deviceId] from the platform's list, or 0 for the default. */
    bool startInput(int32_t deviceId = 0);
    /** True if this cut a recording short. See the definition. */
    bool stopInput();
    /** Raw or clean microphone. See AudioDriver::setInputClean. */
    void setInputClean(bool on);
    /** The input's audio session for the platform's effects, or 0. */
    int32_t inputSession() const;
    bool inputRunning() const;
    /** The open stream's actual format, which may differ from what was asked for. */
    int32_t inputChannels() const;
    int32_t inputRate() const;
    int32_t inputDevice() const;
    /** True once an input capture has run with nothing arriving. */
    bool captureDeaf() const;
    float inputPeak();
    void setInputGain(float gain);
    void setMonitorLevel(float level);
    /** Sixteenths (0) or eighths (1): which pair the swing bends. */
    void setSwingUnit(int32_t unit);
    /**
     * The tuner, on while the record window shows it. `tunerHz` runs the
     * analysis on the calling thread. Never call it from the audio thread.
     */
    void setTunerOn(bool on);
    float tunerHz();
    /** Records either the input or the master output. */
    std::string startCapture(const std::string &path, int source);
    void stopCapture();
    bool capturing() const;
    float capturedSeconds() const;
    /** The finished file's exact length. Seconds as a float would lose frames. */
    int64_t capturedFrames() const;
    float capturedPeak() const;
    bool captureOverflowed() const;

    /**
     * The rack a recording is for, or -1 for none.
     *
     * Only the armed rack gets its boundaries stamped (see `CaptureMarks`),
     * and there's only one capture. Arming resets the marks so a new take
     * doesn't inherit the last one's.
     */
    void armCapture(int rack);
    /**
     * The boundaries the last recording crossed, five longs each:
     *
     *     frame, sceneId, tick, cycleTicks, millibpm
     *
     * Tempo is in thousandths so everything fits in one array of longs.
     * Returns how many marks were written, or -1 if the capture dropped
     * frames, in which case the take can't be split.
     */
    int32_t captureMarks(int64_t *out, int32_t max) const;

    /** Stops everything and silences every tail. Safe from any thread. */
    void panic();
    /** Bars of count-in clicks before playback starts. 0 is none. */
    void setCountInBars(int32_t bars);
    /** See Transport::setRecordModes. */
    void setRecordModes(bool onNote, bool once);
    /** Ticks left of the count-in, for display. 0 when not counting. */
    int64_t countInRemaining() const;
    /** How many threads are started to render tracks beside the audio thread on this platform. */
    static int32_t trackWorkers();
    /** How many of them auto uses. */
    static int32_t autoWorkers();
    /** Cores to render tracks on, counting the audio thread's: 0 for auto. */
    void setCores(int32_t cores);
    /** Cores rendering tracks now (the audio thread's and the workers'), and the most it can be set to. */
    int32_t coresInUse() const;
    int32_t coresMax() const;
    /** The longest the audio thread waited on a worker since the last read, in us. */
    int32_t readWorkerWaitPeakUs();
    /** Milliseconds played since play (see Transport::publishElapsed). */
    int64_t elapsedMs() const;

    uint32_t notesOn(int rack) const;
    uint32_t notesOff(int rack) const;
    // Debug: the live (smoothed, unit-range) value of a mounted machine's parameter.
    float debugParam(int rack, const std::string &name) const;
    // The normalised 0..1 value of a rack unit's parameter, or -1. UI thread.
    float paramNormalized(int rack, const std::string &unit, const std::string &name) const;

  private:
    EngineHost() = default;
    ~EngineHost();
    EngineHost(const EngineHost &) = delete;
    EngineHost &operator=(const EngineHost &) = delete;

    bool mountWithRetry(struct Mount &m, void (*deleter)(void *));

  public:
    bool mountObjectWithRetry(struct Mount &m);

  private:
    /** One file as a source: held in memory if short, mapped if long. */
    std::shared_ptr<const audio::Reel::Source> sourceFor(const std::string &path,
                                                         int64_t &residentFrames, int &mappedCount);
    /** Where converted long takes are stored. See setCacheRoot. */
    std::string cacheRoot;

    bool running = false;
    /** The cores setting, kept so a restart of the stream keeps it. */
    int32_t coresWanted = 0;
    std::string mountedType[16];
    std::atomic<bool> rendering{false}, renderCancel{false};
    // The zone, read on the MIDI thread and written from the UI.
    std::atomic<int> mpeZoneKind{0};
    std::atomic<int> mpeZoneMembers{15};
    std::atomic<float> renderSeconds{0.0f}, renderPeak{0.0f};
    std::string mountedEffectType[16][2];
    /** What is on each send bus, for resolving its parameters by name. */
    std::string mountedSendType[2];
    std::string mountedMasterInsertType[2];
    std::string mountedGroupInsertType[4][2];
    /** What is on each input slot, for resolving its parameters by name. */
    std::string mountedInputType[2];
    std::string mountedModifierType[16][2];
    std::unordered_map<int64_t, std::shared_ptr<const seq::Clip>> clipCache;
};

} // namespace acidulous
