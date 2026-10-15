#pragma once
#include <atomic>
#include <cstdint>
#include <engine/core/RtQueue.h>

// What the UI thread sends the audio thread, besides objects to mount.
namespace acidulous {

/**
 * An outgoing MIDI message, stamped with the frame it belongs on.
 *
 * On the Java side the frame is turned into a wall-clock time using the audio
 * stream's timestamp, and Android schedules the send. So events need to be
 * sent early, not just quickly.
 */
struct MidiOutEvent {
    int64_t frame = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint8_t rack = 0xff; // 0xff: from the transport, not a track
};

/** An incoming realtime byte, stamped with the frame it arrived on. */
struct MidiInEvent {
    int64_t frame = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

struct MidiMessage {
    uint8_t status = 0; // channel in the low nibble == rack
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    /**
     * The channel it arrived on, 0-15, or [kNoChannel] for messages the app
     * made itself. The status nibble holds the rack, and MPE needs the real
     * channel, so it has its own byte.
     */
    uint8_t channel = 0xff;
};

constexpr uint8_t kNoChannel = 0xff;

// Only append to this list. The ordinal crosses the queue and JNI, and
// Recorder.UNITS on the Kotlin side must be in the same order. Automation
// lanes are saved by unit name, so adding a unit doesn't affect old songs.
enum class Unit : uint8_t {
    Machine, Effect1, Effect2, Mod1, Mod2, Mod3, Channel, Master,
    /**
     * The two send buses' effects. Addressed like an insert: a slot holding
     * any effect with its own parameter table. They belong to the song, so
     * the message's rack is ignored.
     */
    Send1, Send2,
    /**
     * The two input effects. Same as the sends (song-level slots, rack
     * ignored), but they run before the input is published, so they're
     * recorded into the take.
     */
    Input1, Input2,
    /**
     * The performance strip: mod wheel, pressure and pedals. A pseudo-unit so
     * they can be recorded into lanes like any knob. They're played back as
     * MIDI.
     */
    Performance,
    /**
     * The master's two inserts, after the sends return and before the fader
     * and limiter. Same slot shape as a send, and the rack is ignored.
     */
    MasterFx1, MasterFx2,
    /** The four mixer groups' two inserts each, group-major. The rack is ignored. */
    Group1Fx1, Group1Fx2, Group2Fx1, Group2Fx2, Group3Fx1, Group3Fx2, Group4Fx1, Group4Fx2,
    /**
     * The held effects on the master: repeat, tape stop and the pad. Sent
     * with a rack only so a press can be recorded into that track's clip.
     * They always act on the master.
     */
    Perform,
};

/** Indices within Unit::Performance. */
constexpr int32_t kPerfMod = 0;      // CC 1
constexpr int32_t kPerfPressure = 1; // channel aftertouch
// The pedals. The index crosses the record queue, so these never move.
constexpr int32_t kPerfSustain = 2;   // CC 64, the dampers
constexpr int32_t kPerfSostenuto = 3; // CC 66, the keys already down
constexpr int32_t kPerfSoft = 4;      // CC 67, una corda

struct ParamMessage {
    int32_t rack = 0;
    Unit unit = Unit::Machine;
    int32_t index = 0; // into the unit's ParamDef table
    float value = 0.0f; // normalised 0..1
    bool record = false; // a user gesture: may be recorded into a lane and overrides that lane this pass
    /**
     * In ticks, or 0 for now. While playing, the message waits for the rack's
     * next multiple of this (e.g. a bar). Used by the perform page's mutes.
     */
    int32_t quantise = 0;
};

/** 1024 events is plenty of headroom for sixteen tracks of notes plus a
 *  clock pulse every ten ticks, if the sender is ever late. */
/**
 * The MIDI out queue, with a hold. While an offline render (freeze or
 * export) runs, nothing is queued, so the render doesn't also play on the
 * external hardware.
 */
class MidiOutQueue {
  public:
    bool push(const MidiOutEvent &e) { return held.load(std::memory_order_relaxed) || q.push(e); }
    bool pop(MidiOutEvent &e) { return q.pop(e); }
    bool empty() const { return q.empty(); }
    void hold(bool on) { held.store(on, std::memory_order_relaxed); }

  private:
    RtQueue<MidiOutEvent, 1024> q;
    std::atomic<bool> held{false};
};
using MidiClockQueue = SharedQueue<MidiInEvent, 256>; // every port that sends clock

} // namespace acidulous
