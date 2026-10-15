#include "Dice.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level a sliced loop reaches before the output stage, used as the drive
// stage's nominal level.
constexpr float kNominal = 0.3f;
// Output gain that puts Init (the defaults) at the same level as the other
// patches, leaving room on the volume knob for quieter patches like Dust.
constexpr float kHouse = 0.82f;
// 60 dB as a multiple of the time constant, so a decay in seconds is the
// time to fall 60 dB.
constexpr float kLn1000 = 6.907755f;
// A per-slice decay at or above the top of its range means no decay at all.
constexpr float kDecayOff = 4.0f;
// The fades at a slice's edges, in frames at 48 kHz. Short enough to keep a
// kick's attack, long enough not to click.
constexpr int32_t kFadeIn = 24;   // half a millisecond
constexpr int32_t kFadeOut = 96;  // two milliseconds
// A following voice's buffer of stretched audio, and how much is pulled from
// the stretcher at a time. The margin keeps enough audio that when the loop
// runs out mid-slice there's enough left to fade out (four times the fade at
// the highest pitch).
constexpr int32_t kHeld = 4096;
constexpr int32_t kPull = 256;
constexpr int32_t kHeldMargin = 512;
// 1/2, then 1 to 16: the bars knob values after auto.
constexpr float kBarsOf[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};

Dice::Dice() { initParams(); }

const ParamDef *Dice::paramDefs(int32_t &count) const {
    static ParamDef defs[Count];
    static bool built = false;
    static char names[kSlices * SliceParamCount][16];
    if (!built) {
        struct Spec { const char *name; float min, max, def; Curve curve; int32_t steps; const char *unit; };
        static const Spec kSlice[SliceParamCount] = {
            {"level", 0.0f, 1.5f, 1.0f, Curve::Linear, 0, ""},
            {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
            {"pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, ""},
            {"decay", 0.01f, 4.0f, 4.0f, Curve::Exponential, 0, "s"},
            {"dir", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        };
        for (int32_t s = 0; s < kSlices; ++s) {
            for (int32_t i = 0; i < SliceParamCount; ++i) {
                const int32_t at = s * SliceParamCount + i;
                std::snprintf(names[at], sizeof(names[at]), "s%02d_%s", s, kSlice[i].name);
                defs[at] = {names[at], kSlice[i].min, kSlice[i].max, kSlice[i].def, kSlice[i].curve, kSlice[i].steps, kSlice[i].unit};
            }
        }
        defs[CutMode] = {"cut", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}; // onsets, grid
        defs[SliceCount] = {"slices", 2.0f, 16.0f, 8.0f, Curve::Stepped, 15, ""};
        defs[Gate] = {"gate", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""};
        defs[Rate] = {"rate", 0.25f, 4.0f, 1.0f, Curve::Exponential, 0, ""};
        defs[RootPitch] = {"pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Fine] = {"fine", -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"};
        defs[Swap] = {"swap", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Reverse] = {"reverse", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Stutter] = {"stutter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[StutterDiv] = {"stutterdiv", 2.0f, 8.0f, 4.0f, Curve::Stepped, 7, ""};
        defs[Drop] = {"drop", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Jump] = {"jump", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[JumpRange] = {"jumprange", 1.0f, 12.0f, 12.0f, Curve::Stepped, 12, ""};
        defs[Hold] = {"hold", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""};
        defs[Seed] = {"seed", 0.0f, 63.0f, 1.0f, Curve::Stepped, 64, ""};
        defs[Cutoff] = {"cutoff", 40.0f, 18000.0f, 18000.0f, Curve::Exponential, 0, "Hz"};
        defs[Resonance] = {"resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[FilterType] = {"filtertype", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""};
        defs[Drive] = {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Volume] = {"volume", 0.0f, 1.5f, 0.9f, Curve::Linear, 0, ""};
        defs[MasterPan] = {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Accent] = {"accent", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""};
        defs[Follow] = {"follow", 0.0f, 1.0f, 1.0f, Curve::Stepped, 2, ""};
        defs[Bars] = {"bars", 0.0f, 6.0f, 0.0f, Curve::Stepped, 7, ""}; // auto, 1/2, 1, 2, 4, 8, 16
        built = true;
    }
    count = Count;
    return defs;
}

void Dice::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.filter.setSampleRate(sampleRate);
        v.filterR.setSampleRate(sampleRate);
        v.stretch.prepare();
        for (auto &h : v.held) h.assign(static_cast<size_t>(kHeld), 0.0f);
    }
    reset();
}

void Dice::reset() {
    for (auto &v : voices) {
        v.used = false;
        v.filter.reset();
    }
    triggers = 0;
}

void Dice::allNotesOff() {
    for (auto &v : voices) v.used = false;
}

void *Dice::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    for (auto &v : voices) v.used = false; // they hold positions in the old loop
    void *old = const_cast<audio::Take *>(take);
    take = static_cast<const audio::Take *>(object);
    builtFrames = -1; // recut against the new loop
    return old;
}

/**
 * Works out where the cuts fall: at the loop's transients (best for breaks)
 * or on an even grid (for pads, or when the detector finds nothing useful).
 */
void Dice::recut() {
    slices = 0;
    if (take == nullptr || take->frames <= 1) return;
    const int32_t want = std::clamp(steppedOf(SliceCount), 2, kSlices);
    const int32_t mode = steppedOf(CutMode);
    if (mode == 0 && !take->onsets.empty()) {
        // With more onsets than slices, pick them evenly across the loop.
        // Taking the first N would only slice the first bar.
        const int32_t found = static_cast<int32_t>(take->onsets.size());
        const int32_t n = std::min(want, found);
        for (int32_t i = 0; i < n; ++i) {
            bounds[i] = take->onsets[static_cast<size_t>(static_cast<int64_t>(i) * found / n)];
        }
        slices = n;
    } else {
        for (int32_t i = 0; i < want; ++i) {
            bounds[i] = static_cast<int32_t>(static_cast<int64_t>(i) * take->frames / want);
        }
        slices = want;
    }
    bounds[slices] = take->frames;
    builtMode = mode;
    builtCount = want;
    builtFrames = take->frames;
}

float Dice::loopBpm() const {
    if (take == nullptr || take->frames <= 1) return 0.0f;
    const int32_t at = std::clamp(steppedTargetOf(Bars), 0, 6);
    const float bars = at == 0 ? take->bars : kBarsOf[at - 1];
    if (bars <= 0.0f) return 0.0f;
    return bars * 4.0f * 60.0f * sampleRate / static_cast<float>(take->frames);
}

/**
 * Points a following voice's stretcher at the start of its slice.
 *
 * The stretcher's first hop has nothing to overlap, so it comes out faded in
 * by the rising half of the window, which would soften the kick. The missing
 * half is added here so the first hop is the untouched source.
 */
void Dice::startStretch(Voice &v) const {
    // Wrap the loop's end into its start so the last slice isn't a window
    // short.
    v.stretch.setLoop(true);
    v.stretch.seek(v.start);
    v.heldHave = 0;
    v.heldPos = 0.0;
    const float *left = take->left.data();
    const float *right = take->right.empty() ? left : take->right.data();
    constexpr int32_t hop = dsp::StereoStretch::kHop;
    float *dst[2] = {v.held[0].data(), v.held[1].data()};
    const float *src[2] = {left, right};
    const int32_t made = v.stretch.fill(dst, src, 0, take->frames, hop, v.stretchRate);
    const float *w = dsp::StereoStretch::hann();
    for (int32_t j = 0; j < made && v.start + j < take->frames; ++j) {
        v.held[0][static_cast<size_t>(j)] += left[v.start + j] * (1.0f - w[j]);
        v.held[1][static_cast<size_t>(j)] += right[v.start + j] * (1.0f - w[j]);
    }
    v.heldHave = made;
}

/** Adds stretched audio to a following voice's buffer. False when the loop has run out. */
bool Dice::pullStretch(Voice &v, const float *left, const float *right) const {
    if (v.heldPos >= kHeld / 2) {
        const int32_t k = static_cast<int32_t>(v.heldPos);
        for (auto &h : v.held) std::copy(h.begin() + k, h.begin() + v.heldHave, h.begin());
        v.heldHave -= k;
        v.heldPos -= k;
    }
    const int32_t n = std::min(kPull, kHeld - v.heldHave);
    if (n <= 0) return true;
    float *dst[2] = {v.held[0].data() + v.heldHave, v.held[1].data() + v.heldHave};
    const float *src[2] = {left, right};
    const int32_t made = v.stretch.fill(dst, src, 0, take->frames, n, v.stretchRate);
    v.heldHave += made;
    return made > 0;
}

Dice::Voice *Dice::allocate() {
    for (auto &v : voices) if (!v.used) return &v;
    // Always steal a voice, and steal the oldest one. It's nearest its end,
    // so it's the least missed and the quietest place to cut.
    Voice *oldest = &voices[0];
    for (auto &v : voices) if (v.age > oldest->age) oldest = &v;
    return oldest;
}

/**
 * The random seed for one trigger. With Hold, the rolls come only from the
 * slice and the seed, so every pass is the same. Otherwise they come from a
 * counter that never repeats.
 */
uint32_t Dice::rollFor(int32_t slice) {
    if (steppedOf(Hold) != 0) {
        uint32_t h = static_cast<uint32_t>(steppedOf(Seed)) * 2654435761u + static_cast<uint32_t>(slice) * 40503u + 1u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return h;
    }
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

void Dice::noteOn(uint8_t note, uint8_t velocity) {
    if (take == nullptr || slices <= 0) return;
    const int32_t asked = note - kBaseNote;
    if (asked < 0 || asked >= slices) return;

    uint32_t roll = rollFor(asked);
    auto chance = [&](float probability) {
        roll = roll * 1664525u + 1013904223u;
        return static_cast<float>(roll >> 8) * (1.0f / 16777216.0f) < probability;
    };
    auto uniform = [&]() {
        roll = roll * 1664525u + 1013904223u;
        return static_cast<float>(roll >> 8) * (1.0f / 16777216.0f);
    };

    ++triggers;
    if (chance(targetOf(Drop))) return; // dropped

    int32_t slice = asked;
    if (chance(targetOf(Swap))) slice = static_cast<int32_t>(uniform() * slices) % slices;

    const bool sliceReversed = static_cast<int32_t>(sliceParam(slice, Direction) + 0.5f) != 0;
    const bool reversed = sliceReversed != chance(targetOf(Reverse));

    float semis = targetOf(RootPitch) + targetOf(Fine) * 0.01f + sliceParam(slice, Pitch);
    if (chance(targetOf(Jump))) {
        const float range = targetOf(JumpRange);
        semis += (uniform() < 0.5f ? -1.0f : 1.0f) * std::round(uniform() * range);
    }

    int32_t repeats = 0;
    if (chance(targetOf(Stutter))) repeats = steppedTargetOf(StutterDiv);

    Voice *v = allocate();
    v->used = true;
    v->note = note;
    v->slice = slice;
    v->start = bounds[slice];
    v->end = bounds[slice + 1];
    if (v->end <= v->start) v->end = std::min(take->frames, v->start + 64);
    // When following, the tempo ratio sets the speed and the pitch knob only
    // the pitch. The stretcher only reads forwards, so a reversed slice just
    // plays faster or slower like tape, which shifts its pitch a little.
    const float lb = loopBpm();
    const bool follow = steppedTargetOf(Follow) != 0 && lb > 0.0f && take->frames > dsp::StereoStretch::kWindow * 2;
    const float tempo = follow ? songBpm / lb : 1.0f;
    const float pitch = std::pow(2.0f, semis / 12.0f);
    const int32_t span = v->end - v->start;
    v->repeatLen = repeats > 0 ? std::max(64, span / repeats) : span;
    v->repeats = repeats;
    v->follows = follow && !reversed;
    if (v->follows) {
        v->timeRate = std::max(0.05f, tempo * targetOf(Rate));
        v->pitch = pitch;
        v->stretchRate = v->timeRate / pitch;
        v->left = static_cast<int32_t>(v->repeatLen / v->timeRate);
        startStretch(*v);
    } else {
        v->inc = pitch * targetOf(Rate) * tempo * (reversed ? -1.0 : 1.0);
        v->pos = reversed ? v->end - 1 : v->start;
        v->left = static_cast<int32_t>(v->repeatLen / std::max(0.05, std::fabs(v->inc)));
    }

    const float accent = targetOf(Accent);
    const float vel = velocityGain(static_cast<float>(velocity) / 127.0f, accent);
    const float pan = std::clamp(sliceParam(slice, Pan), -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    const float level = sliceParam(slice, Level) * vel;
    v->gainL = std::cos(angle) * level * 1.4142f;
    v->gainR = std::sin(angle) * level * 1.4142f;
    v->env = 1.0f;
    // Gate at 1 lets a slice run to its end. Lower values shorten it.
    //
    // The decay is the time to fall 60 dB. The top of the range means no
    // decay, so a loop played straight doesn't fade every slice.
    const float decay = sliceParam(slice, Decay) * std::max(0.02f, targetOf(Gate));
    v->envCoeff = decay >= kDecayOff ? 0.0f
                                     : 1.0f - std::exp(-kLn1000 / (decay * sampleRate));
    v->filter.reset();
    v->filterR.reset();
}

void Dice::noteOff(uint8_t) {} // a slice plays its full length

bool Dice::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (take == nullptr || take->frames <= 1) return true;

    if (builtFrames != take->frames || builtMode != steppedOf(CutMode) || builtCount != steppedOf(SliceCount)) {
        recut();
    }
    if (slices <= 0) return true;

    const float cutoff = paramOf(Cutoff), reso = paramOf(Resonance);
    const int32_t ftype = steppedOf(FilterType);
    const float drive = paramOf(Drive), volume = paramOf(Volume);
    const float masterPan = paramOf(MasterPan);
    const float panL = std::cos((masterPan + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((masterPan + 1.0f) * 0.25f * 3.14159265f);
    const float *left = take->left.data();
    const float *right = take->right.empty() ? left : take->right.data();
    const int32_t last = take->frames - 1;

    for (auto &v : voices) {
        if (!v.used) continue;
        v.filter.set(cutoff, reso, ftype, dsp::MultiFilter::Clean, 0.0f);
        v.filterR.set(cutoff, reso, ftype, dsp::MultiFilter::Clean, 0.0f);
        for (int32_t i = 0; i < frames; ++i) {
            if (!v.used) break;
            float sl, sr;
            if (v.follows) {
                if (static_cast<int32_t>(v.heldPos) + kHeldMargin >= v.heldHave && !pullStretch(v, left, right)) {
                    // The loop ends under this slice: fade out on what is left.
                    const int32_t remain = static_cast<int32_t>((v.heldHave - 2 - v.heldPos) / v.pitch);
                    if (remain < v.left) {
                        v.left = remain;
                        v.repeats = 0;
                    }
                    if (v.left <= 0) { v.used = false; break; }
                }
                const int32_t idx = static_cast<int32_t>(v.heldPos);
                const float f = static_cast<float>(v.heldPos - idx);
                const float *h0 = v.held[0].data(), *h1 = v.held[1].data();
                sl = h0[idx] + (h0[idx + 1] - h0[idx]) * f;
                sr = h1[idx] + (h1[idx + 1] - h1[idx]) * f;
            } else {
                const int32_t idx = std::clamp(static_cast<int32_t>(v.pos), 0, last);
                const int32_t next = std::min(idx + 1, last);
                const float f = static_cast<float>(v.pos - idx);
                sl = left[idx] + (left[next] - left[idx]) * f;
                sr = right[idx] + (right[next] - right[idx]) * f;
            }
            // Faded at both ends so slices don't click. The fade out is
            // longer since an onset cut ends right on the next transient. The
            // fade in only has to cover a grid cut landing mid-waveform, and
            // longer would soften the attack.
            const float rampIn = v.age < kFadeIn
                                     ? static_cast<float>(v.age) / static_cast<float>(kFadeIn)
                                     : 1.0f;
            const float rampOut = v.left < kFadeOut
                                      ? static_cast<float>(v.left) / static_cast<float>(kFadeOut)
                                      : 1.0f;
            const float e = v.env * rampIn * rampOut;
            ++v.age;
            v.env -= v.env * v.envCoeff;
            L[i] += v.filter.process(sl * e) * v.gainL;
            R[i] += v.filterR.process(sr * e) * v.gainR;
            if (v.follows) v.heldPos += v.pitch;
            else v.pos += v.inc;
            if (--v.left <= 0) {
                if (v.repeats > 1) {
                    // A stutter repeats the same piece.
                    --v.repeats;
                    if (v.follows) {
                        startStretch(v);
                        v.left = static_cast<int32_t>(v.repeatLen / v.timeRate);
                    } else {
                        v.pos = v.inc < 0 ? v.end - 1 : v.start;
                        v.left = static_cast<int32_t>(v.repeatLen / std::max(0.05, std::fabs(v.inc)));
                    }
                    v.age = 0; // a repeat starts over and needs the same fade in
                } else {
                    v.used = false;
                }
            }
            if (v.env < 1e-4f) v.used = false;
        }
    }

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i] * volume * kHouse, r = R[i] * volume * kHouse;
        if (drive > 0.0001f) {
            // Normalised on the nominal level so the drive knob changes the
            // character and not the level.
            const float k = 1.0f + drive * 10.0f;
            const float norm = kNominal / dsp::fastTanh(kNominal * k);
            l = dsp::fastTanh(l * k) * norm;
            r = dsp::fastTanh(r * k) * norm;
        }
        L[i] = l * panL * 1.4142f;
        R[i] = r * panR * 1.4142f;
    }
    return true;
}

} // namespace acidulous::machine
