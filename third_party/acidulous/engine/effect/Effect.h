#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/core/Params.h>
#include "SlotMod.h"

// An insert effect in a rack slot. Same lifecycle rules as Machine.
namespace acidulous {

// Bypass is addressed like a parameter (unit effect1/2, this index) so the UI,
// automation lanes and recording treat it as one, without it living in every
// effect's table.
constexpr int32_t kEffectBypassIndex = -2;

class Effect {
  public:
    virtual ~Effect() = default;
    virtual const char *typeName() const = 0;
    virtual const ParamDef *paramDefs(int32_t &count) const = 0;
    virtual void prepare(int32_t sampleRate) = 0;
    virtual void reset() = 0;
    // Once per block, before process(): tempo and the block's tick range, for
    // anything synced to the transport.
    virtual void onBlock(int64_t /*tickStart*/, int64_t /*tickEnd*/, float /*bpm*/) {}
    // A note-on reaching the track's machine, after transpose, with its pitch
    // in hertz in the track's tuning. Audio thread.
    virtual void onNote(int32_t /*note*/, float /*hz*/) {}
    // In place. Return true if the output is stereo.
    virtual bool process(float *L, float *R, int32_t frames, bool stereoIn) = 0;
    /**
     * Takes an object compiled on a worker (a formula, say) in place of the
     * one held in [slot], and returns the old one to be retired. Audio
     * thread. Effects that hold none refuse by handing the new one back.
     */
    virtual void *swapObject(int32_t /*slot*/, void *object) { return object; }
    /**
     * Whether a mod on parameter [index] glides across the block in slices.
     * An effect returns false for a knob that rebuilds a table or a cabinet,
     * so it moves once a block and the rebuild runs once.
     */
    virtual bool glides(int32_t /*index*/) const { return true; }
    ParamSet &params() { return params_; }

    // What the rack calls once a block before run(): the mod slot's clock, then the effect's.
    void block(int64_t tickStart, int64_t tickEnd, float bpm) {
        if (mod_.active()) mod_.block(tickStart, bpm, modTransport_);
        onBlock(tickStart, tickEnd, bpm);
    }

    // What the rack calls: bypass short-circuits, params are ticked here.
    bool run(float *L, float *R, int32_t frames, bool stereoIn) {
        if (bypass_) {
            modFresh_ = true;
            return stereoIn;
        }
        if (mod_.active()) return runModulated(L, R, frames, stereoIn);
        params_.tick();
        const bool stereo = process(L, R, frames, stereoIn);
        applyGain(L, R, frames, stereo || stereoIn);
        return stereo;
    }

    /**
     * Frames per slice when a mod's target glides across a block (see
     * runModulated). A test can set 1 for a reference or kBlockFrames for
     * one step a block.
     */
    static inline int32_t modSlice = 8;

    // Bypass keeps state (a delay's tail survives being switched back on).
    void setBypass(bool on) { bypass_ = on; }
    bool bypassed() const { return bypass_; }

    /**
     * Any of the slot's parameters by index: the effect's own, bypass
     * (kEffectBypassIndex) or the mod slot's (isSlotModIndex). [jump] is a
     * step lock's: no glide.
     */
    void setParamAt(int32_t index, float v01, bool jump = false) {
        if (index == kEffectBypassIndex) {
            setBypass(v01 >= 0.5f);
        } else if (isSlotModIndex(index)) {
            mod_.set(kSlotModIndexBase - index, v01, &params_);
        } else if (jump) {
            params_.jump(index, v01);
            if (mod_.targets(index)) mod_.jumpNext();
        } else {
            params_.set(index, v01);
        }
    }
    /** The normalised value setParamAt last set, or -1 for an index the slot hasn't got. */
    float normalizedAt(int32_t index) const {
        if (index == kEffectBypassIndex) return bypass_ ? 1.0f : 0.0f;
        if (isSlotModIndex(index)) return mod_.normalized(kSlotModIndexBase - index);
        if (index < 0 || index >= params_.size()) return -1.0f;
        return params_.normalized(index);
    }
    /** After reset() on a panic or a render's start: the mod starts from the top too. */
    void resetMod() {
        mod_.reset();
        modFresh_ = true;
    }
    /**
     * The track a keyed mod follows (0-based), or -1. The engine hands its
     * sound over with setModKey before the block.
     */
    int32_t modKeyTrack() const { return mod_.keyTrack(); }
    /**
     * Before block(): the keyed mod's sound, the slot's seed for the random
     * shapes, and the transport: the song's position and whether it plays.
     */
    void setModKey(const float *key, uint32_t seed, const ModTransport &transport) {
        mod_.setKey(key, seed);
        modTransport_ = transport;
    }
    bool modActive() const { return mod_.active(); }
    /** Before block(): the song's position, for an effect that lines up with the bar. */
    void setTransport(const ModTransport &transport) { modTransport_ = transport; }

    /**
     * The rack this effect's detector listens to, e.g. a compressor on the
     * bass ducking under the kick. `sidechain` is a stepped parameter, 0 for
     * its own input and 1..16 for a rack.
     *
     * Returns the rack index, or -1 for its own input or an effect with no
     * detector.
     */
    int32_t sidechainRack() const {
        if (sidechainIndex_ < 0) return -1;
        return static_cast<int32_t>(params_.get(sidechainIndex_) + 0.5f) - 1;
    }
    /**
     * The sidechain for this block: a mono buffer of the source rack's sound,
     * or null for the effect's own input. Set by the engine before the rack
     * renders and only valid for that call.
     */
    void setKey(const float *key) { key_ = key; }

  protected:
    /**
     * `trimDb` is added to the output gain, so that the effect at its default
     * settings comes out about as loud as it went in. A drive comes out
     * louder and a chorus quieter; without the trim, putting one on a track
     * would jump the level. Measured with tools/audition.sh levels.
     *
     * The trim follows `mix` up to its default, so at mix 0 the track still
     * comes through untouched. An effect with a trim needs a mix.
     */
    void initParams(float trimDb = 0.0f) {
        trimDb_ = trimDb;
        int32_t n = 0;
        const ParamDef *defs = paramDefs(n);
        params_.init(defs, n);
        // The output trim is applied here for every effect. It's found by
        // name, so an effect without a "gain" param just skips it.
        gainIndex_ = -1;
        sidechainIndex_ = -1;
        for (int32_t i = 0; i < n; ++i) {
            if (std::strcmp(defs[i].name, "gain") == 0) gainIndex_ = i;
            if (std::strcmp(defs[i].name, "sidechain") == 0) sidechainIndex_ = i;
            if (std::strcmp(defs[i].name, "mix") == 0) {
                mixIndex_ = i;
                mixDefault_ = defs[i].def;
            }
        }
    }

    /** The sidechain for this block, or null: see `setKey`. */
    const float *key_ = nullptr;

  private:
    /**
     * The output trim in dB, applied after the effect has run. Makes up level
     * lost in a wet/dry mix (a chorus comes out quieter) or tames an effect
     * that makes things louder.
     */
    void applyGain(float *L, float *R, int32_t frames, bool stereo) {
        if (gainIndex_ < 0) return;
        float trim = trimDb_;
        if (trim != 0.0f && mixIndex_ >= 0 && mixDefault_ > 0.0f) {
            trim *= std::min(1.0f, params_.get(mixIndex_) / mixDefault_);
        }
        const float db = params_.get(gainIndex_) + trim;
        if (db > -0.01f && db < 0.01f) return; // no trim costs nothing
        const float g = std::pow(10.0f, db * 0.05f);
        for (int32_t i = 0; i < frames; ++i) L[i] *= g;
        if (stereo) for (int32_t i = 0; i < frames; ++i) R[i] *= g;
    }
    int32_t gainIndex_ = -1;
    int32_t sidechainIndex_ = -1;
    float trimDb_ = 0.0f;
    int32_t mixIndex_ = -1;
    float mixDefault_ = 0.0f;

    /**
     * The mod offsets its targets, then the block runs in slices of
     * modSlice frames with each moving continuous target stepping from last
     * block's value to this one's, so a fast mod doesn't zipper. A step lock,
     * a jump in the song or the first block after a bypass or a target
     * change lands at once.
     */
    bool runModulated(float *L, float *R, int32_t frames, bool stereoIn) {
        mod_.apply(L, R, frames, stereoIn, params_);
        // After a preroll the knobs hold last block's value, so this block glides from it as in the song.
        const bool pre = mod_.prerolled();
        float before[2] = {0.0f, 0.0f};
        for (int32_t j = 0; j < 2 && pre; ++j) {
            const int32_t t = mod_.target(j);
            if (t >= 0 && t < params_.size()) before[j] = params_.get(t);
        }
        params_.tick();
        int32_t idx[2];
        float from[2], to[2];
        int32_t n = 0;
        for (int32_t j = 0; j < 2; ++j) {
            const int32_t t = mod_.target(j);
            const bool valid = t >= 0 && t < params_.size() && !params_.isStepped(t) && (j == 0 || t != mod_.target(0));
            const float now = valid ? params_.get(t) : 0.0f;
            const float was = pre ? before[j] : glideWas_[j];
            if (valid && glides(t) && (pre || (!modFresh_ && glideIndex_[j] == t)) && !mod_.snapped() && was != now) {
                idx[n] = t;
                from[n] = was;
                to[n] = now;
                ++n;
            }
            glideIndex_[j] = valid ? t : -1;
            glideWas_[j] = now;
        }
        modFresh_ = false;
        if (n == 0 || modSlice <= 0 || modSlice >= frames) {
            const bool stereo = process(L, R, frames, stereoIn);
            applyGain(L, R, frames, stereo || stereoIn);
            return stereo;
        }
        const float *key = key_;
        bool anyStereo = false, anyMono = false;
        bool sliceStereo[kBlockFrames];
        int32_t slices = 0;
        for (int32_t o = 0; o < frames; o += modSlice) {
            const int32_t len = std::min(modSlice, frames - o);
            const float f = static_cast<float>(o + len) / static_cast<float>(frames);
            for (int32_t k = 0; k < n; ++k) {
                // An exponential knob (a cutoff) glides in ratio, the rest in a line.
                const bool ratio = params_.isExponential(idx[k]) && from[k] > 0.0f && to[k] > 0.0f;
                params_.hold(idx[k], ratio ? from[k] * std::pow(to[k] / from[k], f) : from[k] + (to[k] - from[k]) * f);
            }
            subOffset_ = o;
            subBlock_ = frames;
            key_ = key != nullptr ? key + o : nullptr;
            const bool st = process(L + o, R + o, len, stereoIn);
            applyGain(L + o, R + o, len, st || stereoIn);
            if (slices < kBlockFrames) sliceStereo[slices++] = st;
            (st ? anyStereo : anyMono) = true;
        }
        key_ = key;
        subOffset_ = 0;
        subBlock_ = 0;
        for (int32_t k = 0; k < n; ++k) params_.hold(idx[k], to[k]);
        // An effect that came out stereo for some slices and mono for others:
        // the mono ones get their right channel.
        if (anyStereo && anyMono && !stereoIn) {
            for (int32_t j = 0, o = 0; j < slices; ++j, o += modSlice) {
                if (sliceStereo[j]) continue;
                const int32_t len = std::min(modSlice, frames - o);
                std::memcpy(R + o, L + o, sizeof(float) * static_cast<size_t>(len));
            }
        }
        return anyStereo;
    }

  protected:
    /**
     * Inside a block split by runModulated, how far into the block this
     * process() call starts and the whole block's length; 0 and 0 otherwise.
     * An effect that works out a phase from the block's tick adds the
     * offset, so its own LFO carries on across the slices.
     */
    int32_t subOffset_ = 0;
    int32_t subBlock_ = 0;
    /** [phase] at the block's start moved on by [inc] a frame to where this slice starts. */
    float slicePhase(float phase, float inc) const {
        if (subOffset_ == 0) return phase;
        const float p = phase + inc * static_cast<float>(subOffset_);
        return p - std::floor(p);
    }
    /** The frames the block's tick range is spread over: the whole block, even in a slice. */
    int32_t blockFramesOf(int32_t frames) const { return subBlock_ > 0 ? subBlock_ : frames; }

    ParamSet params_;
    bool bypass_ = false;
    SlotMod mod_;
    ModTransport modTransport_;
    bool modFresh_ = true; // no glide on the next block: after a bypass, a reset or at the start
    int32_t glideIndex_[2] = {-1, -1};
    float glideWas_[2] = {0.0f, 0.0f};
};

} // namespace acidulous
