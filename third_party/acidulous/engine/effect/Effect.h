#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/core/Params.h>

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
    // In place. Return true if the output is stereo.
    virtual bool process(float *L, float *R, int32_t frames, bool stereoIn) = 0;
    /**
     * Takes an object compiled on a worker (a formula, say) in place of the
     * one held in [slot], and returns the old one to be retired. Audio
     * thread. Effects that hold none refuse by handing the new one back.
     */
    virtual void *swapObject(int32_t /*slot*/, void *object) { return object; }
    ParamSet &params() { return params_; }

    // What the rack calls: bypass short-circuits, params are ticked here.
    bool run(float *L, float *R, int32_t frames, bool stereoIn) {
        if (bypass_) return stereoIn;
        params_.tick();
        const bool stereo = process(L, R, frames, stereoIn);
        applyGain(L, R, frames, stereo || stereoIn);
        return stereo;
    }

    // Bypass keeps state (a delay's tail survives being switched back on).
    void setBypass(bool on) { bypass_ = on; }
    bool bypassed() const { return bypass_; }

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

  protected:
    ParamSet params_;
    bool bypass_ = false;
};

} // namespace acidulous
