#pragma once
#include <cstdint>

// The fixed points of the engine. Everything else is derived from these.
namespace acidulous {

constexpr int32_t kSampleRate = 48000; // Oboe resamples if the device disagrees
constexpr int32_t kBlockFrames = 64;   // one render block; the sequencer's event resolution
constexpr int32_t kRackCount = 16;     // one per MIDI channel; a 4x4 picker
constexpr int32_t kPPQN = 240;         // ticks per quarter note
constexpr int32_t kEffectSlots = 2;    // per rack, for now
/**
 * Effects on the input, before anything hears it.
 *
 * These are recorded into the take. A track's inserts are applied on playback
 * and can still be changed afterwards.
 */
constexpr int32_t kInputSlots = 2;
// One per modifier. The keyboard strip has a control each for chord, scale
// and arp, so all three can run at once.
constexpr int32_t kInputModSlots = 3;
constexpr int32_t kMaxParams = 256;    // per unit (Forage: 14 per pad x 13 pads, plus globals)

} // namespace acidulous
