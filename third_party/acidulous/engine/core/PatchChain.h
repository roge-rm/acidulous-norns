#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <engine/core/Constants.h>

// The effects a machine patch brings with it: up to kEffectSlots inserts with
// their parameters, and its reverb and delay send levels.
//
// A track remembers which insert slots its current patch filled, as a bit per
// slot. Loading the next patch clears only those, then puts the new patch's
// inserts in free slots, its old slots first. Inserts the player added stay.
// When no slot is free a patch's insert is skipped. An insert naming an effect
// this build doesn't have is left out before any slot is chosen. Every slot a
// patch fills gets a new effect, even of the same type, so no tail carries on.
//
// The app keeps the same rule on its song document (PatchChain.kt), since the
// song is what it saves and undoes; EngineHost::applyPatchChain is the same
// thing for a host that drives the engine directly. Keep the two alike.
namespace acidulous {

struct PatchInsert {
    std::string type;                                            // an EffectRegistry name
    std::vector<std::pair<std::string, float>> params;           // normalised 0..1, by name
    std::vector<std::pair<std::string, std::string>> settings;   // words, like a Formula's "formula"
    bool bypass = false;                                         // in the slot but out of the signal path
};

struct PatchChain {
    std::vector<PatchInsert> inserts;
    // A patch that names any send sets both, the one it leaves out to zero.
    // One that names none leaves the track's sends alone.
    bool hasSends = false;
    float sendReverb = 0.0f; // 0..1, the channel's sendreverb
    float sendDelay = 0.0f;  // 0..1, the channel's senddelay
};

struct InsertPlan {
    /** The slot each of the patch's inserts goes in, in order, or -1 for skipped. */
    int32_t slotOf[kEffectSlots]{};
    /** Slots to empty first: the last patch's, a bit per slot. */
    uint32_t clear = 0;
    /** The slots the patch now owns. */
    uint32_t owned = 0;
    /** How many of the patch's inserts found no slot. */
    int32_t skipped = 0;
};

/**
 * What a host keeps per track between patch loads, and hands back each time.
 * Save it with the song like the app does (Track.fromPatch, Track.patchSends).
 */
struct PatchTrackState {
    uint32_t owned = 0;         // insert slots the patch filled, a bit per slot
    bool sendsHeld = false;     // the patch owns the sends
    float heldReverb = 0.0f;    // the player's levels from before the patch set them
    float heldDelay = 0.0f;
    float patchReverb = 0.0f;   // what the patch set, so a send moved by hand can be told
    float patchDelay = 0.0f;
};

struct SendPlan {
    bool set = false; // false: leave the sends as they are
    float reverb = 0.0f, delay = 0.0f;
};

/**
 * The send levels a patch load sets, given the track's levels [nowReverb]
 * and [nowDelay]. A patch that names sends sets them, keeping the player's
 * levels the first time; one that names none puts the player's back. A send
 * moved since the last patch set it makes the sends the player's again, so
 * nothing is put back. Updates [state].
 */
inline SendPlan planPatchSends(const PatchChain &chain, float nowReverb, float nowDelay, PatchTrackState &state) {
    if (state.sendsHeld && (nowReverb != state.patchReverb || nowDelay != state.patchDelay)) state.sendsHeld = false;
    SendPlan plan;
    if (chain.hasSends) {
        if (!state.sendsHeld) {
            state.sendsHeld = true;
            state.heldReverb = nowReverb;
            state.heldDelay = nowDelay;
        }
        state.patchReverb = chain.sendReverb;
        state.patchDelay = chain.sendDelay;
        plan.set = true;
        plan.reverb = chain.sendReverb;
        plan.delay = chain.sendDelay;
    } else if (state.sendsHeld) {
        state.sendsHeld = false;
        plan.set = true;
        plan.reverb = state.heldReverb;
        plan.delay = state.heldDelay;
    }
    return plan;
}

/**
 * Where a patch's [wanted] inserts go. [occupied] is what each slot holds now,
 * [owned] the bits of the slots the last patch filled.
 *
 * The slots taken are the last patch's first, then free ones, low to high,
 * and the patch's inserts fill them in slot order so its chain keeps its
 * order whichever slots it gets.
 */
inline InsertPlan planPatchInserts(const bool occupied[kEffectSlots], uint32_t owned, int32_t wanted) {
    InsertPlan plan;
    for (int32_t s = 0; s < kEffectSlots; ++s) plan.slotOf[s] = -1;
    if (wanted < 0) wanted = 0;
    int32_t chosen[kEffectSlots];
    int32_t n = 0;
    for (int32_t s = 0; s < kEffectSlots; ++s) {
        if ((owned >> s) & 1u) plan.clear |= 1u << s;
    }
    // The last patch's slots, then free ones.
    for (int32_t s = 0; s < kEffectSlots && n < wanted; ++s) {
        if ((plan.clear >> s) & 1u) chosen[n++] = s;
    }
    for (int32_t s = 0; s < kEffectSlots && n < wanted; ++s) {
        if (((plan.clear >> s) & 1u) == 0 && !occupied[s]) chosen[n++] = s;
    }
    // Low to high, so the chain keeps its order.
    for (int32_t i = 1; i < n; ++i) {
        for (int32_t j = i; j > 0 && chosen[j - 1] > chosen[j]; --j) std::swap(chosen[j - 1], chosen[j]);
    }
    for (int32_t i = 0; i < n; ++i) {
        plan.slotOf[i] = chosen[i];
        plan.owned |= 1u << chosen[i];
    }
    plan.skipped = wanted - n;
    return plan;
}

} // namespace acidulous
