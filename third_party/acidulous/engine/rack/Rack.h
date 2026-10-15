#pragma once
#include <cstdint>
#include <engine/dsp/Wsola.h>
#include <atomic>
#include <engine/core/Constants.h>
#include <engine/core/Frozen.h>
#include <engine/core/Messages.h>
#include <engine/core/Params.h>
#include <engine/core/Settings.h>
#include <engine/effect/Effect.h>
#include <engine/inputmod/InputMod.h>
#include <engine/machine/Machine.h>
#include <sequencer/ClipPlayer.h>

// One rack: clip player -> modifiers -> machine -> effects -> channel strip.
// Renders one block into bufL/bufR. Audio thread only. The Engine calls
// swap*() on the audio thread when a Mount arrives.
namespace acidulous {

class Rack {
  public:
    enum ChannelParam : int32_t { Gain, Pan, Mute, Solo, SendReverb, SendDelay, MidiMode, MidiChannel, Swing, Output,
                                  Transpose, Velocity, ChannelCount };

    /**
     * A channel parameter's target, not its smoothed value. Used for swing,
     * which the scheduler reads once a block. Ramping swing would slide the
     * offbeats across a bar and make exports unrepeatable.
     */
    float channelTarget(ChannelParam p) const { return channel.target(p); }

    /** Internal: the machine only. Both: machine and hardware. MIDI: the
     *  hardware only, and the machine isn't run, so it uses no CPU. */
    enum MidiOutMode : int32_t { OutInternal = 0, OutBoth, OutMidi };

    Rack();

    seq::ClipPlayer clipPlayer;

    bool isActive() const { return machine != nullptr; }
    Machine *currentMachine() const { return machine; }
    Effect *currentEffect(int32_t slot) const { return (slot >= 0 && slot < kEffectSlots) ? effects[slot] : nullptr; }
    InputMod *currentInputMod(int32_t slot) const { return (slot >= 0 && slot < kInputModSlots) ? modifiers[slot] : nullptr; }

    /**
     * Where notes go to be recorded after the modifiers. The engine implements
     * it. What's recorded into the clip is what the modifiers produced, so the
     * recording tap is at the end of the chain, not on the raw input.
     */
    struct ModifiedNoteSink {
        virtual ~ModifiedNoteSink() = default;
        virtual void onModifiedNote(int32_t rack, uint8_t status, uint8_t d1, uint8_t d2) = 0;
    };
    void setModifiedNoteSink(ModifiedNoteSink *sink) { modifiedSink = sink; }

    /**
     * Live MIDI (a finger or a controller) enters here and runs through the
     * modifier chain. Whatever comes out is played and, while recording,
     * recorded.
     */
    void handleMidi(uint8_t status, uint8_t d1, uint8_t d2);

    /**
     * A note from a clip, straight to the machine without the modifiers.
     *
     * Modifiers are applied once on the way in and the result is in the clip.
     * Running it through them again would apply them twice (an arpeggio of an
     * arpeggio), and this way a clip plays exactly what the piano roll shows.
     */
    void playSequenced(uint8_t status, uint8_t d1, uint8_t d2);
    /** A clip note's words, sent just before its note-on. See Machine::lyric. */
    void lyric(const uint8_t *phones, int32_t count);
    /** Whether the machine wants a clip's words ahead of their notes. See Machine::wantsWordsAhead. */
    bool wantsWordsAhead() const { return machine != nullptr && machine->wantsWordsAhead(); }
    /** A clip note's words ahead of it, [inFrames] before it's due, transposed as its note-on will be. */
    void wordsAhead(const uint8_t *phones, int32_t count, uint8_t note, uint8_t velocity, int32_t inFrames);
    void allNotesOff();

    /**
     * Per-note expression, straight to the machine and not through the
     * modifiers. An arpeggiator turns one note into several and there's no
     * right answer for which of them a finger's pressure belongs to, so the
     * expression follows the note that was played.
     */
    void noteExpression(uint8_t kind, uint8_t note, uint8_t d1, uint8_t d2, float bendSemis);

    /**
     * The same, from a clip instead of a finger, as a normalised 0..1 value
     * rather than MIDI. [kind] is an acidulous::Expr. Also skips the modifiers.
     */
    void noteExpressionValue(int32_t kind, uint8_t note, float v01);

    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm);
    void render(int32_t frames);

    /**
     * While the scheduler runs, what it sends the track (notes, lanes, words,
     * expression) is queued in order and played at the start of the track's
     * own render, so a note-on's cost (some machines do real work there) is
     * part of the track's job rather than the scheduler's. Every event in a
     * block lands at its start either way, so nothing sounds different.
     */
    void queue() { queueing = true; }
    /** Plays what's queued, in the order it came, and stops queueing. */
    void playQueued();

    /**
     * While a track renders it may be on a worker thread, so what it sends out
     * of itself (MIDI out, notes to the recorder, perform lanes to the master)
     * is kept until sendHeld(), which the audio thread calls for each track in
     * render order. That's the order they went out in when one thread
     * rendered everything.
     */
    void holdOutgoing() { holding = true; }
    /** Audio thread: sends what holdOutgoing() kept, in order, and stops holding. */
    void sendHeld();

  private:
    /** The frozen clip at its own rate, a plain read from memory. */
    void readFrozenPlain(int32_t frames);

    /** One queued event: what it was, and its values (see queue()). */
    struct Queued {
        enum Kind : uint8_t { Note, Lyric, Words, Expression, Param } kind;
        uint8_t a, b, c;
        int32_t i;
        float v;
        int32_t from, count; // its phones, in queuedPhones
    };
    static constexpr int32_t kMaxQueued = 512, kMaxQueuedPhones = 2048;
    Queued queued[kMaxQueued];
    uint8_t queuedPhones[kMaxQueuedPhones];
    int32_t queuedCount = 0, queuedPhoneCount = 0;
    bool queueing = false;

    /** One write out of the track, kept while holding. */
    struct Outgoing {
        enum Kind : uint8_t { Midi, Recorded, Perform } kind;
        uint8_t status, d1, d2;
        int32_t index;
        float v;
        int64_t frame;
    };
    static constexpr int32_t kMaxOutgoing = 512;
    Outgoing outgoing[kMaxOutgoing];
    int32_t outgoingCount = 0;
    bool holding = false;
    /** Sends [o] now, or keeps it while holding. Past kMaxOutgoing in one block the rest are dropped. */
    void sendOut(const Outgoing &o);
    void emit(const Outgoing &o);
    /**
     * Keeps [h] (and [count] phones) if queueing. Full, it plays what's queued
     * so far and says no, so the caller plays this one now: in the same order.
     */
    bool keep(Queued h, const uint8_t *phones = nullptr, int32_t count = 0);
    /** Plays what's queued and empties it; still queueing after. */
    void replayQueued();

  public:

    // --- Freeze ---------------------------------------------------------
    // The playing scene decides whether this rack plays its machine or its
    // frozen audio. Called before the scheduler fires notes, since a frozen
    // rack isn't sent any.
    /**
     * [ramping] relaxes the tempo match and nothing else.
     *
     * Frozen clips only play at the tempo they were rendered at. A scene with a
     * smooth tempo change is between two tempos for its first bar, so frozen
     * racks would fall back to their machines right when freezing matters most.
     * While the clock ramps the audio is stretched to the current tempo
     * instead.
     *
     * Only while ramping. Playing freezes at any tempo would be a bigger change
     * (the UI tells people frozen clips are tempo-bound in two places).
     */
    void updateFrozen(int64_t sceneId, float bpm, bool playing, bool ramping = false);
    /** Pass the rack's place in the arrangement to a machine that wants it. */
    void updateScene(int64_t sceneId, int64_t cycleTick, bool playing);
    bool frozenActive() const { return frozenNow != nullptr; }
    /** Where in the frozen clip this block starts. Called before render(). */
    void syncFrozen(int64_t tickInIteration, float bpm);
    /** Returns the old set for the caller to retire. */
    const FrozenSet *swapFrozen(const FrozenSet *next) {
        const FrozenSet *old = frozenSet;
        frozenSet = next;
        frozenNow = nullptr; // decided again next block
        // Stop any tail ringing out of the old set, since the tail clip belongs
        // to it.
        tailClip = nullptr;
        return old;
    }

    /**
     * Freezing: copy the rack's output after its effects and before its
     * channel strip, so the fader, pan, sends and mute still work on the
     * frozen audio. Only set by the offline render.
     */
    bool tapDry = false;
    float dryL[kBlockFrames]{};
    float dryR[kBlockFrames]{};
    bool isStereo() const { return stereo; }

    // Returns the old machine for the caller to retire.
    Machine *swapMachine(Machine *next);
    Effect *swapEffect(int32_t slot, Effect *next);
    InputMod *swapInputMod(int32_t slot, InputMod *next);

    /**
     * [jump] skips smoothing. A stepped lane changes on a step, and a step
     * lock that glided in would chirp at the start of its own note.
     */
    void setParam(Unit unit, int32_t index, float v01, bool jump = false);

    /**
     * The track's tuning as a ratio to equal temperament for each of the 128
     * notes, or null for equal temperament. Any thread. The machine picks it up
     * next block. Not used for drum or audio tracks, where notes pick a sound
     * rather than a pitch.
     */
    void setTuning(const float *ratios);

    // While recording, a parameter the user moves overrides its lane for the
    // rest of the current pass so the lane doesn't fight the knob. Cleared at
    // each iteration boundary.
    void touch(Unit unit, int32_t index) {
        const uint32_t key = (static_cast<uint32_t>(unit) << 16) | static_cast<uint32_t>(index & 0xffff);
        for (int32_t i = 0; i < touchedCount; ++i) if (touched[i] == key) return;
        if (touchedCount < kMaxTouched) touched[touchedCount++] = key;
    }
    bool isTouched(Unit unit, int32_t index) const {
        const uint32_t key = (static_cast<uint32_t>(unit) << 16) | static_cast<uint32_t>(index & 0xffff);
        for (int32_t i = 0; i < touchedCount; ++i) if (touched[i] == key) return true;
        return false;
    }
    void clearTouched() { touchedCount = 0; }

    float tuningTables[2][128]{};
    std::atomic<int> tuningIndex{-1};

    float bufL[kBlockFrames]{};
    float bufR[kBlockFrames]{};
    /**
     * What a sidechain listening to this rack hears: mono, after the inserts,
     * before the fader and mute. Written by every render (see
     * Engine::renderRacks for when it's read).
     */
    float keyBuf[kBlockFrames]{};
    /** keyBuf as last block left it, for a listener that renders first (Engine::keyFor). */
    float keyPrev[kBlockFrames]{};

    /**
     * The mixer group this rack's output should go to (0..3), or -1 for the
     * master. routedTo is what the engine settled on this block.
     */
    int32_t outputRequested() const { return static_cast<int32_t>(channel.target(Output) + 0.5f) - 1; }
    int32_t routedTo = -1;

    /** Jump the fader, pan and sends to their targets without gliding (panic). */
    void jumpChannel() { channel.jumpAll(); }

    /**
     * Where Unit::Perform goes: the master's performance effects. Presses are
     * sent to a rack so they're recorded into that rack's clip and played back
     * from it, and this is how they reach the effects. Set by the engine.
     */
    ParamSet *performSink = nullptr;

    // Read by the master after render(). Post-fader.
    bool soloed() const { return channel.get(Solo) >= 0.5f; }
    bool muted() const { return channel.get(Mute) >= 0.5f; }
    /**
     * How much of this rack goes to send [slot].
     *
     * The two channel parameters are still called sendreverb and senddelay
     * because those names are saved in songs and controller mappings. The
     * sends now hold whatever effect the master has in each slot, so this is
     * numbered.
     */
    float sendAmount(int32_t slot) const {
        return channel.get(slot == 0 ? SendReverb : SendDelay);
    }
    float readPeak() { return peakHold.exchange(0.0f, std::memory_order_relaxed); }
    float channelNormalized(int32_t index) const { return channel.normalized(index); }
    /** A channel parameter's index by name, or -1 if there's none. */
    int32_t channelIndexOf(const char *name) const { return channel.indexOf(name); }

    /** Where this rack's notes go when they're sent to external MIDI. */
    void bindMidiOut(MidiOutQueue *queue, int32_t index) {
        outQueue = queue;
        rackIndex = index;
    }
    /**
     * Once a block: the frame its notes will be stamped with, and a chance to
     * notice the mode changing, so a track switched away from MIDI out
     * mid-note doesn't leave the note hanging on the hardware.
     */
    void updateMidiOut(int64_t frame);
    int32_t midiOutMode() const;
    bool midiOutBound() const { return outQueue != nullptr; }

  private:
    struct Sink final : MidiSink {
        Rack *rack = nullptr;
        int32_t stage = 0; // which modifier slot output this is
        void send(uint8_t status, uint8_t d1, uint8_t d2) override;
    };

    MidiOutQueue *outQueue = nullptr;
    int32_t rackIndex = 0;
    int64_t outFrame = 0;
    int32_t lastOutMode = OutInternal;
    uint8_t lastOutChannel = 0;

    void deliver(int32_t fromStage, uint8_t status, uint8_t d1, uint8_t d2);
    // Everything going to the machine goes through here, so the voice limit
    // is in one place and notes made by modifiers are counted too.
    /** [live] means it came through the modifier chain and can be recorded. */
    void toMachine(uint8_t status, uint8_t d1, uint8_t d2, bool live);
    ModifiedNoteSink *modifiedSink = nullptr;
    void forgetHeld(uint8_t note);

    // Held notes, oldest first. Bigger than the largest voice limit so the
    // count stays right when the limit is off.
    static constexpr int32_t kMaxHeld = 128;
    uint8_t held[kMaxHeld]{};
    int32_t heldCount = 0;

    /**
     * Where each started note went after the track's transpose, so its
     * note-off, pressure and bend find the right voice even if the transpose
     * has changed since. Each entry is its own note while nothing is held.
     */
    uint8_t sentTo[128]{};
    void resetSentTo() {
        for (int32_t n = 0; n < 128; ++n) sentTo[n] = static_cast<uint8_t>(n);
    }

    /**
     * The pedals, handled here rather than in each machine so every machine
     * gets them. Sustain holds every note past its key, sostenuto only the keys
     * down when it was pressed, and soft plays notes softer. A note held only
     * by a pedal has its note-off waiting in pedalHeld and is released when
     * neither pedal needs it. A machine that takes the pedals (a piano with
     * half-pedal) gets how far down each one is, and handles sustain and soft
     * itself.
     */
    uint8_t pedalValue[3]{};
    bool sustainDown = false;
    bool sostenutoDown = false;
    bool softDown = false;
    bool keyDown[128]{};
    bool pedalHeld[128]{};
    bool sostenutoSet[128]{};
    void setPedal(int32_t which, uint8_t value);
    bool machineTakesPedals() const { return machine != nullptr && machine->takesPedals(); }
    void releasePedalled();
    void resetPedals();

    const FrozenSet *frozenSet = nullptr;
    const FrozenClip *frozenNow = nullptr;
    int64_t frozenCursor = 0;
    /**
     * The ring-out tail, read after the loop.
     *
     * It has its own clip pointer because what's ringing out is usually the
     * clip we just left. There's one cursor, so a loop shorter than its tail
     * rings out the newest pass rather than stacking every pass.
     */
    const FrozenClip *tailClip = nullptr;
    int64_t tailCursor = 0;
    /**
     * How fast the frozen audio is read, as a multiple of the rendered rate.
     * Exactly 1 except during a tempo ramp, and 1 uses the plain buffer read
     * instead of the stretcher, which is much cheaper and doesn't smear
     * transients.
     */
    float frozenRate = 1.0f;
    dsp::StereoStretch frozenStretch;
    /** Last block's read position from the tick, to notice a new cycle starting. */
    int64_t frozenSyncTarget = 0;
    /** Which clip the stretcher currently holds, at this position. */
    const FrozenClip *stretching = nullptr;
    /**
     * How much of the output is the stretched read rather than the plain one,
     * ramping between them.
     *
     * Both switches would click. Going in, the stretcher's first hop has
     * nothing to overlap with, so it fades in over a hop (about 15 ms at half
     * level). Coming out, sourcePosition() is where the next hop will read,
     * which is up to a hop ahead of what's been played, so switching straight
     * to the plain read there would skip up to 15 ms.
     *
     * A 10 ms crossfade covers both. The plain read is skipped entirely once
     * the blend is fully over.
     */
    float frozenBlend = 0.0f;
    static constexpr int32_t kBlendFrames = 480; // 10 ms at 48k

    Machine *machine = nullptr;
    Effect *effects[kEffectSlots]{};
    InputMod *modifiers[kInputModSlots]{};
    Sink sinks[kInputModSlots + 1];

    ParamSet channel;
    bool stereo = false;
    std::atomic<float> peakHold{0.0f};
    static constexpr int32_t kMaxTouched = 16;
    uint32_t touched[kMaxTouched]{};
    int32_t touchedCount = 0;
};

} // namespace acidulous
