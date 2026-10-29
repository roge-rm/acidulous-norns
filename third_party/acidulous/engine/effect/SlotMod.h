#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <engine/core/Constants.h>
#include <engine/core/Params.h>
#include <engine/dsp/Lfo.h>

// The mod slot every insert has: one source (a tempo LFO, a step pattern, a
// follower on the insert's own input or on another track) moving up to two of
// the effect's knobs. Its settings are parameters of the slot, addressed like
// bypass with indices below zero, so they record, lock and save like knobs.
//
// The modulation is an offset on the knob's normalised value, put through the
// knob's own smoother once a block. The knob keeps its value, so a lane or a
// hand on it still works underneath.
namespace acidulous {

/** The table's name for EffectRegistry::paramDefs, so the app reads it like an effect's. */
constexpr const char *kSlotModType = "~mod";
/** Targets name an effect's parameter by index + 1; 0 is none. */
constexpr int32_t kSlotModTargets = 64;
constexpr int32_t kSlotModSteps = 16;
/** Slot parameter k is index kSlotModIndexBase - k. Bypass is -2. */
constexpr int32_t kSlotModIndexBase = -16;

namespace slotmod {
enum Param : int32_t {
    Source, Shape, Rate, Length, Attack, Release, Key, Target1, Depth1, Target2, Depth2, Step1,
    Count = Step1 + kSlotModSteps
};
enum SourceKind : int32_t { Off, Lfo, Steps, Follow, Keyed, SourceCount };
enum ShapeKind : int32_t { Sine, Triangle, RampUp, RampDown, Square, RandomHold, RandomSmooth, ShapeCount };

inline const ParamDef *defs(int32_t &count) {
    // Steps default to a falling four, so choosing "steps" is heard at once.
    static const ParamDef d[Count] = {
        {"mod_source", 0.0f, SourceCount - 1, 0.0f, Curve::Stepped, SourceCount, ""},
        {"mod_shape", 0.0f, ShapeCount - 1, 0.0f, Curve::Stepped, ShapeCount, ""},
        {"mod_rate", 0.0f, dsp::Lfo::kRates - 1, 8.0f, Curve::Stepped, dsp::Lfo::kRates, ""}, // 1/4
        {"mod_length", 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, ""},                             // 8, 16
        {"mod_attack", 1.0f, 500.0f, 10.0f, Curve::Exponential, 0, "ms"},
        {"mod_release", 10.0f, 2000.0f, 150.0f, Curve::Exponential, 0, "ms"},
        {"mod_key", 0.0f, kRackCount, 0.0f, Curve::Stepped, kRackCount + 1, ""},        // tracks 1-16, none
        {"mod_target1", 0.0f, kSlotModTargets, 0.0f, Curve::Stepped, kSlotModTargets + 1, ""},
        {"mod_depth1", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"mod_target2", 0.0f, kSlotModTargets, 0.0f, Curve::Stepped, kSlotModTargets + 1, ""},
        {"mod_depth2", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"mod_step1", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mod_step2", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"mod_step3", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mod_step4", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"mod_step5", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mod_step6", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"mod_step7", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mod_step8", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"mod_step9", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mod_step10", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"mod_step11", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mod_step12", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"mod_step13", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mod_step14", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"mod_step15", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mod_step16", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
    };
    count = Count;
    return d;
}
} // namespace slotmod

inline bool isSlotModIndex(int32_t index) {
    return index <= kSlotModIndexBase && index > kSlotModIndexBase - slotmod::Count;
}
inline int32_t slotModIndexOf(const char *name) {
    if (name == nullptr || std::strncmp(name, "mod_", 4) != 0) return -1;
    int32_t n = 0;
    const ParamDef *d = slotmod::defs(n);
    for (int32_t k = 0; k < n; ++k) {
        if (std::strcmp(d[k].name, name) == 0) return kSlotModIndexBase - k;
    }
    return -1;
}

/** What the engine tells every mod once a block. */
struct ModTransport {
    int64_t songOffset = 0; // clock tick to song tick
    bool started = false;   // this block is play's first: the followers empty
};

/** One insert's mod. Audio thread only; no allocation. */
class SlotMod {
  public:
    SlotMod() {
        int32_t n = 0;
        const ParamDef *d = slotmod::defs(n);
        for (int32_t k = 0; k < n; ++k) set(k, d[k].unmap(d[k].def), nullptr);
    }

    bool active() const { return source_ != slotmod::Off; }

    float normalized(int32_t k) const { return (k >= 0 && k < slotmod::Count) ? norm_[k] : -1.0f; }

    /**
     * Sets slot parameter [k]. A knob that stops being a target, or every
     * target when the source goes off, is put back on its own value.
     */
    void set(int32_t k, float v01, ParamSet *params) {
        if (k < 0 || k >= slotmod::Count) return;
        int32_t n = 0;
        const ParamDef &d = slotmod::defs(n)[k];
        norm_[k] = v01 < 0.0f ? 0.0f : (v01 > 1.0f ? 1.0f : v01);
        const float v = d.map(norm_[k]);
        const auto whole = [&] { return static_cast<int32_t>(v + 0.5f); };
        switch (k) {
        case slotmod::Source: {
            const int32_t was = source_;
            source_ = whole();
            if (source_ == slotmod::Off && params != nullptr) {
                release(target_[0], *params);
                release(target_[1], *params);
            }
            if (was == slotmod::Off && source_ != slotmod::Off) {
                // Picks the transport up afresh rather than from wherever it was left.
                synced_ = false;
                env_ = 0.0f;
            }
            break;
        }
        case slotmod::Shape: shape_ = whole(); break;
        case slotmod::Rate: beats_ = dsp::Lfo::beatsOf(whole()); break;
        case slotmod::Length: length_ = whole() == 0 ? 8 : 16; break;
        case slotmod::Attack: attack_ = coeff(v); break;
        case slotmod::Release: release_ = coeff(v); break;
        case slotmod::Key: key_ = whole(); break;
        case slotmod::Target1:
        case slotmod::Target2: {
            const int32_t j = k == slotmod::Target1 ? 0 : 1;
            const int32_t t = whole() - 1;
            if (t != target_[j] && params != nullptr && target_[j] != target_[1 - j]) release(target_[j], *params);
            target_[j] = t;
            break;
        }
        case slotmod::Depth1: depth_[0] = v; break;
        case slotmod::Depth2: depth_[1] = v; break;
        default: step_[k - slotmod::Step1] = v; break;
        }
    }

    /**
     * Once a block, with the whole tick the block starts in and the
     * transport as the engine sees it. The position follows the clock, which
     * runs at the tempo stopped or playing, so a render repeats. A loop or a
     * jump in the song only moves the song offset, so the followers carry on
     * through it: only a play start, a panic and a freeze's start empty them.
     */
    void block(int64_t tickStart, float bpm, const ModTransport &t) {
        if (t.started) {
            env_ = 0.0f;
            synced_ = false;
        }
        // A block is often less than a tick, so the guess keeps the fraction
        // and the phase doesn't stair-step a tick at a time.
        const double per = static_cast<double>(kBlockFrames) * bpm * kPPQN / (60.0 * kSampleRate);
        const double start = static_cast<double>(tickStart);
        const double guess = clock_ + per;
        if (!synced_ || guess < start - 1.0 || guess > start + 2.0) {
            // Play, the mod just switched on, or the clock jumped (a stop
            // puts it back to 0): start from here, as a render does.
            clock_ = start;
            snap_ = true;
            preroll_ = true;
        } else {
            clock_ = guess < start ? start : (guess >= start + 1.0 ? start + 0.999999 : guess);
        }
        synced_ = true;
        per_ = per;
        tick_ = clock_ + static_cast<double>(t.songOffset);
    }

    /** Another track's sound as last block left it, or null for silence; and a seed for the random shapes. */
    void setKey(const float *key, uint32_t seed) {
        keyBuf_ = key;
        seed_ = seed;
    }
    /** The track a keyed mod follows, 0-based; kRackCount (none) or -1 hears silence. */
    int32_t keyTrack() const { return source_ == slotmod::Keyed ? key_ : -1; }

    /** Before the effect's params tick: offsets each target for this block. */
    void apply(const float *L, const float *R, int32_t frames, bool stereo, ParamSet &params) {
        float m = 0.0f;
        switch (source_) {
        case slotmod::Lfo:
        case slotmod::Steps: m = at(tick_); break;
        case slotmod::Follow: {
            float peak = 0.0f;
            for (int32_t i = 0; i < frames; ++i) peak = std::fmax(peak, std::fabs(L[i]));
            if (stereo) for (int32_t i = 0; i < frames; ++i) peak = std::fmax(peak, std::fabs(R[i]));
            m = follow(peak);
            break;
        }
        case slotmod::Keyed: {
            float peak = 0.0f;
            if (keyBuf_ != nullptr) for (int32_t i = 0; i < kBlockFrames; ++i) peak = std::fmax(peak, std::fabs(keyBuf_[i]));
            m = follow(peak);
            break;
        }
        default: return;
        }
        const int32_t n = params.size();
        const int32_t t0 = target_[0], t1 = target_[1];
        const bool v0 = t0 >= 0 && t0 < n, v1 = t1 >= 0 && t1 < n && t1 != t0;
        const float d0 = depth_[0] + (t1 == t0 ? depth_[1] : 0.0f), d1 = depth_[1];
        // A play start or a jump with a source that is only the position: run
        // the knobs' smoothers through the blocks before this one, so the
        // block lands where playing into it would have it rather than at the
        // value itself. A scene played alone or frozen then starts as the
        // song has it.
        prerolled_ = snap_ && preroll_ && (source_ == slotmod::Lfo || source_ == slotmod::Steps);
        if (prerolled_) {
            for (int32_t k = kPreroll; k >= 1; --k) {
                const float p = at(tick_ - static_cast<double>(k) * per_);
                if (v0) params.offset(t0, p * d0, k == kPreroll);
                if (v1) params.offset(t1, p * d1, k == kPreroll);
                if (k == kPreroll) continue;
                if (v0) params.tickOne(t0);
                if (v1) params.tickOne(t1);
            }
        }
        const bool jump = snap_ && !prerolled_;
        snapped_ = jump;
        if (v0) params.offset(t0, m * d0, jump);
        if (v1) params.offset(t1, m * d1, jump);
        snap_ = false;
        preroll_ = false;
    }

    /** A panic or a render's start: the follower empties and the next block lands without a glide. */
    void reset() {
        env_ = 0.0f;
        clock_ = 0.0;
        tick_ = 0.0;
        synced_ = false;
        snap_ = true;
        preroll_ = false;
    }
    /** A step lock jumped a knob; the offset jumps with it rather than gliding back on. */
    void jumpNext() {
        snap_ = true;
        preroll_ = false;
    }
    bool targets(int32_t index) const { return index >= 0 && (index == target_[0] || index == target_[1]); }
    /** Target [j] (0 or 1) as an effect parameter index, or -1. */
    int32_t target(int32_t j) const { return target_[j]; }
    /** The song position, in ticks, this block reads. */
    double position() const { return tick_; }
    /** Whether this block's apply() jumped its targets rather than gliding. */
    bool snapped() const { return snapped_; }
    /** Whether this block's apply() ran the smoothers in from before; the knobs hold last block's value. */
    bool prerolled() const { return prerolled_; }

  private:
    static float coeff(float ms) {
        const float blockMs = 1000.0f * kBlockFrames / kSampleRate;
        return 1.0f - std::exp(-blockMs / ms);
    }
    static void release(int32_t t, ParamSet &params) {
        if (t >= 0 && t < params.size()) params.offset(t, 0.0f, false);
    }
    static float hash01(int64_t n, uint32_t seed) {
        uint64_t z = static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull + seed * 0xBF58476D1CE4E5B9ull + 1;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / 16777216.0f;
    }
    /** The LFO's or the steps' value at song tick [tick]. */
    float at(double tick) const {
        if (source_ == slotmod::Steps) {
            const double len = static_cast<double>(beats_) * kPPQN;
            const auto i = static_cast<int64_t>(std::floor(tick / len));
            return step_[static_cast<int32_t>(((i % length_) + length_) % length_)];
        }
        return lfo(tick);
    }
    /** -1..1, from the position. */
    float lfo(double tick) const {
        const double len = static_cast<double>(beats_) * kPPQN;
        const double cycles = tick / len;
        const auto n = static_cast<int64_t>(std::floor(cycles));
        const auto p = static_cast<float>(cycles - static_cast<double>(n));
        switch (shape_) {
        case slotmod::Triangle: return dsp::Lfo::triangle(p);
        case slotmod::RampUp: return 2.0f * p - 1.0f;
        case slotmod::RampDown: return 1.0f - 2.0f * p;
        case slotmod::Square: return p < 0.5f ? 1.0f : -1.0f;
        case slotmod::RandomHold: return hash01(n, seed_) * 2.0f - 1.0f;
        case slotmod::RandomSmooth: {
            const float a = hash01(n, seed_), b = hash01(n + 1, seed_);
            return (a + (b - a) * p * p * (3.0f - 2.0f * p)) * 2.0f - 1.0f;
        }
        default: return dsp::Lfo::sine(p);
        }
    }
    /** 0..1 over -48..0 dBFS of the followed peak. */
    float follow(float peak) {
        env_ += (peak - env_) * (peak > env_ ? attack_ : release_);
        if (env_ < 1e-6f) {
            env_ = 0.0f;
            return 0.0f;
        }
        const float db = 20.0f * std::log10(env_);
        const float m = 1.0f + db / 48.0f;
        return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
    }

    float norm_[slotmod::Count]{};
    int32_t source_ = slotmod::Off;
    int32_t shape_ = 0;
    float beats_ = 1.0f;
    int32_t length_ = 16;
    float attack_ = 0.1f, release_ = 0.01f;
    int32_t key_ = 0;
    int32_t target_[2] = {-1, -1};
    float depth_[2] = {0.0f, 0.0f};
    float step_[kSlotModSteps]{};

    /** Blocks a preroll runs: the smoother (0.35 a block) is within 1e-7 of its target by then. */
    static constexpr int32_t kPreroll = 40;
    double clock_ = 0.0; // the clock's tick, which a loop in the song doesn't move
    double per_ = 0.0;   // ticks a block
    double tick_ = 0.0;  // the song's
    bool synced_ = false; // clock_ follows the clock
    bool snap_ = true;
    bool snapped_ = true;
    bool preroll_ = false;   // the next snap is a resync to the position, not a step lock
    bool prerolled_ = false;
    float env_ = 0.0f;
    const float *keyBuf_ = nullptr;
    uint32_t seed_ = 0;
};

} // namespace acidulous
