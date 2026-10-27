#include "Dice.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level a sliced loop reaches before the volume, used as the drive
// stage's nominal level. A drum loop's peaks sit well above it, so it's set
// where the loop's loudness holds as the drive turns up.
constexpr float kNominal = 0.1f;
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
// The shortest a gate leaves a slice, in seconds.
constexpr float kGateMin = 0.0025f;
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
        static char macNames[kMacros * MacroParams][12];
        for (int k = 0; k < kMacros; ++k) {
            const int32_t b = MacroBase + k * MacroParams;
            char *n = macNames[k * MacroParams];
            std::snprintf(n, 12, "mac%d", k + 1);
            defs[b] = {n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            for (int r = 0; r < 4; ++r) {
                char *d = macNames[k * MacroParams + 1 + r];
                std::snprintf(d, 12, "mac%d_d%d", k + 1, r + 1);
                defs[b + 1 + r] = {d, 0.0f, static_cast<float>(MacroDestCount - 1), 0.0f, Curve::Stepped, MacroDestCount, ""};
                char *a = macNames[k * MacroParams + 5 + r];
                std::snprintf(a, 12, "mac%d_a%d", k + 1, r + 1);
                defs[b + 5 + r] = {a, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            }
        }
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Dice::Count];
    DestMap() {
        using P = Dice;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {P::Swap, P::McSwap}, {P::Reverse, P::McReverse}, {P::Stutter, P::McStutter},
            {P::Drop, P::McDrop}, {P::Jump, P::McJump}, {P::StutterDiv, P::McRolls},
            {P::JumpRange, P::McJumpRange}, {P::Seed, P::McSeed}, {P::SliceCount, P::McSlices},
            {P::Gate, P::McGate}, {P::Rate, P::McRate}, {P::Cutoff, P::McCutoff},
            {P::Resonance, P::McResonance}, {P::Drive, P::McDrive}, {P::Volume, P::McVolume},
            {P::MasterPan, P::McPan}, {P::Accent, P::McAccent},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Dice::withMacros(int32_t p, float v, const float *mac) const {
    const int d = destMap().of[p];
    if (d < 0 || mac[d] == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + mac[d]);
}

void Dice::updateMacros() {
    for (float &m : mac_) m = 0.0f;
    for (float &m : macGoal_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = params_.get(b), goal = params_.target(b);
        if (knob == 0.0f && goal == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            // The routes' destinations are steps, so their targets.
            const int dest = static_cast<int>(params_.target(b + 1 + r) + 0.5f);
            if (dest <= McOff || dest >= MacroDestCount) continue;
            mac_[dest] += knob * params_.get(b + 5 + r);
            macGoal_[dest] += goal * params_.target(b + 5 + r);
        }
    }
    // Two octaves either way.
    mac_[McPitch] = std::clamp(mac_[McPitch], -1.0f, 1.0f);
    macGoal_[McPitch] = std::clamp(macGoal_[McPitch], -1.0f, 1.0f);
}

void Dice::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    filterL.setSampleRate(sampleRate);
    filterR.setSampleRate(sampleRate);
    for (auto &v : voices) {
        v.stretch.prepare();
        for (auto &h : v.held) h.assign(static_cast<size_t>(kHeld), 0.0f);
    }
    reset();
}

void Dice::reset() {
    // Field by field, since the stretcher and the held buffers were sized in
    // prepare().
    for (auto &v : voices) {
        v.used = false;
        v.leaving = false;
        v.age = 0;
        v.started = 0;
        v.fadeLen = kFadeOut;
        v.fadeTop = 1.0f;
        v.open = v.pieceOpen = v.gateFade = 0;
        v.env = 1.0f;
        v.heldHave = 0;
        v.heldPos = 0.0;
    }
    filterL.reset();
    filterR.reset();
    rng = kRngSeed;
    triggers = 0;
    updateMacros();
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

Dice::~Dice() {
    delete take;
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
        bounds[slices] = take->frames;
    } else {
        // The grid divides the loop's bars, so room before the first hit is
        // skipped and the cuts land on the beats. The last slice may ring on
        // past the bars to the end of the file (see noteOn).
        const int32_t from = std::clamp(take->barsStart, 0, take->frames - 1);
        const int32_t len = std::clamp(barsFrames(), 1, take->frames - from);
        for (int32_t i = 0; i < want; ++i) {
            bounds[i] = from + static_cast<int32_t>(static_cast<int64_t>(i) * len / want);
        }
        slices = want;
        bounds[slices] = from + len;
    }
    builtMode = mode;
    builtCount = want;
    builtFrames = take->frames;
}

float Dice::loopBpm() const {
    if (take == nullptr || take->frames <= 1) return 0.0f;
    const int32_t at = std::clamp(steppedTargetOf(Bars), 0, 6);
    const float bars = at == 0 ? take->bars : kBarsOf[at - 1];
    if (bars <= 0.0f) return 0.0f;
    return bars * 4.0f * 60.0f * sampleRate / static_cast<float>(barsFrames());
}

int32_t Dice::barsFrames() const {
    if (take == nullptr) return 0;
    return take->barsFrames > 0 ? take->barsFrames : take->frames;
}

/**
 * Where a following voice's stretcher loops back to: the first bar, so a
 * slice that runs out near the end doesn't read the room before it. From 0
 * for a slice that starts in that room, or when what's left is too short to
 * loop.
 */
int32_t Dice::stretchFrom(const Voice &v) const {
    const int32_t from = take->barsStart;
    if (from <= 0 || v.start < from || take->frames - from < dsp::StereoStretch::kWindow * 2) return 0;
    return from;
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
    const int32_t made = v.stretch.fill(dst, src, stretchFrom(v), take->frames, hop, v.stretchRate);
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
    const int32_t made = v.stretch.fill(dst, src, stretchFrom(v), take->frames, n, v.stretchRate);
    v.heldHave += made;
    return made > 0;
}

/**
 * A voice for a new slice. Once all but the spare voices are busy, the oldest
 * note fades out over the fade-out time to make way, so a free voice is there
 * for the next one. Only when every voice is busy (many triggers inside one
 * fade) is one cut: the one nearest its end.
 */
Dice::Voice *Dice::allocate() {
    Voice *freeOne = nullptr;
    int busy = 0;
    Voice *oldest = nullptr;
    for (auto &v : voices) {
        if (!v.used) {
            if (freeOne == nullptr) freeOne = &v;
            continue;
        }
        if (v.leaving) continue;
        ++busy;
        if (oldest == nullptr || v.started < oldest->started) oldest = &v;
    }
    if (busy >= kVoices - kSpare && oldest != nullptr) {
        oldest->leaving = true;
        oldest->repeats = 0;
        shorten(*oldest, kFadeOut);
    }
    if (freeOne != nullptr) return freeOne;
    Voice *nearest = &voices[0];
    for (auto &v : voices) if (v.left < nearest->left) nearest = &v;
    return nearest;
}

void Dice::shorten(Voice &v, int32_t frames) {
    if (frames >= v.left) return;
    const float now = v.left < v.fadeLen ? v.fadeTop * static_cast<float>(v.left) / static_cast<float>(v.fadeLen) : v.fadeTop;
    v.fadeTop = now;
    v.fadeLen = std::max(1, frames);
    v.left = frames;
}

/**
 * The random seed for one trigger. With Hold, the rolls come only from the
 * slice and the seed, so every pass is the same. Otherwise they come from a
 * counter that never repeats.
 */
uint32_t Dice::rollFor(int32_t slice) {
    if (steppedOf(Hold) != 0) {
        uint32_t h = static_cast<uint32_t>(steppedGoalOf(Seed)) * 2654435761u + static_cast<uint32_t>(slice) * 40503u + 1u;
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
    // A knob set in this block, before render, still counts.
    updateMacros();
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
    if (chance(goalOf(Drop))) return; // dropped

    int32_t slice = asked;
    if (chance(goalOf(Swap))) slice = static_cast<int32_t>(uniform() * slices) % slices;

    const bool sliceReversed = static_cast<int32_t>(sliceParam(slice, Direction) + 0.5f) != 0;
    const bool reversed = sliceReversed != chance(goalOf(Reverse));

    float semis = goalOf(RootPitch) + goalOf(Fine) * 0.01f + sliceParam(slice, Pitch) + 24.0f * macGoal_[McPitch];
    if (chance(goalOf(Jump))) {
        const float range = goalOf(JumpRange);
        semis += (uniform() < 0.5f ? -1.0f : 1.0f) * std::round(uniform() * range);
    }

    int32_t repeats = 0;
    if (chance(goalOf(Stutter))) repeats = steppedGoalOf(StutterDiv);

    Voice *v = allocate();
    v->used = true;
    v->leaving = false;
    v->age = 0;
    v->started = triggers;
    v->note = note;
    v->slice = slice;
    v->start = bounds[slice];
    v->end = bounds[slice + 1];
    if (v->end <= v->start) v->end = std::min(take->frames, v->start + 64);
    // A grid's last slice ends with the bars, but played forwards and whole
    // it rings on into whatever follows them in the file, so a cymbal left
    // ringing past the bar isn't cut off. The gate still measures the slice.
    int32_t ring = 0;
    if (slice == slices - 1 && !reversed && repeats == 0) ring = std::max(0, take->frames - v->end);
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
    int32_t ringLeft = 0;
    if (v->follows) {
        v->timeRate = std::max(0.05f, tempo * goalOf(Rate));
        v->pitch = pitch;
        v->stretchRate = v->timeRate / pitch;
        v->left = static_cast<int32_t>(v->repeatLen / v->timeRate);
        ringLeft = static_cast<int32_t>((v->repeatLen + ring) / v->timeRate);
        startStretch(*v);
    } else {
        v->inc = pitch * goalOf(Rate) * tempo * (reversed ? -1.0 : 1.0);
        v->pos = reversed ? v->end - 1 : v->start;
        v->left = static_cast<int32_t>(v->repeatLen / std::max(0.05, std::fabs(v->inc)));
        ringLeft = static_cast<int32_t>((v->repeatLen + ring) / std::max(0.05, std::fabs(v->inc)));
    }

    const float accent = goalOf(Accent);
    const float vel = velocityGain(static_cast<float>(velocity) / 127.0f, accent);
    const float pan = std::clamp(sliceParam(slice, Pan), -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    const float level = sliceParam(slice, Level) * vel;
    v->gainL = std::cos(angle) * level * 1.4142f;
    v->gainR = std::sin(angle) * level * 1.4142f;
    v->env = 1.0f;
    v->fadeLen = kFadeOut;
    v->fadeTop = 1.0f;
    // Gate at 1 lets a slice run to its end. Below that it stays open for
    // gate squared of the slice (of each stroke, on a stutter), so the
    // bottom of the knob is a stab a few milliseconds long and the top
    // most of the slice, at any tempo. The last quarter of that is a fade.
    const float gate = std::clamp(goalOf(Gate), 0.0f, 1.0f);
    const int32_t piece = v->left;
    if (gate >= 0.999f) {
        if (ring > 0) v->left = ringLeft;
        v->pieceOpen = v->left + 1;
        v->gateFade = 0;
    } else {
        const auto shortest = static_cast<int32_t>(kGateMin * sampleRate);
        v->pieceOpen = std::max(std::min(piece, shortest), static_cast<int32_t>(static_cast<float>(piece) * gate * gate));
        v->gateFade = std::min(v->pieceOpen, std::max(kFadeOut, v->pieceOpen / 4));
    }
    v->open = v->pieceOpen;
    // The decay is the time to fall 60 dB. The top of the range means no
    // decay, so a loop played straight doesn't fade every slice.
    const float decay = sliceParam(slice, Decay);
    v->envCoeff = decay >= kDecayOff ? 0.0f
                                     : 1.0f - std::exp(-kLn1000 / (decay * sampleRate));
}

void Dice::noteOff(uint8_t) {} // a slice plays its full length

bool Dice::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
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
        for (int32_t i = 0; i < frames; ++i) {
            if (!v.used) break;
            float sl, sr;
            if (v.follows) {
                if (static_cast<int32_t>(v.heldPos) + kHeldMargin >= v.heldHave && !pullStretch(v, left, right)) {
                    // The loop ends under this slice: fade out on what is left.
                    const int32_t remain = static_cast<int32_t>((v.heldHave - 2 - v.heldPos) / v.pitch);
                    if (remain < v.left) {
                        shorten(v, remain);
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
            const float rampOut = v.left < v.fadeLen
                                      ? v.fadeTop * static_cast<float>(v.left) / static_cast<float>(v.fadeLen)
                                      : v.fadeTop;
            float e = v.env * rampIn * rampOut;
            if (v.gateFade > 0) {
                if (v.open <= 0) e = 0.0f;
                else if (v.open < v.gateFade) e *= static_cast<float>(v.open) / static_cast<float>(v.gateFade);
                --v.open;
            }
            ++v.age;
            v.env -= v.env * v.envCoeff;
            L[i] += sl * e * v.gainL;
            R[i] += sr * e * v.gainR;
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
                    v.open = v.pieceOpen;
                } else {
                    v.used = false;
                }
            } else if (v.gateFade > 0 && v.open <= 0 && v.repeats <= 1) {
                v.used = false; // the gate has closed on the last stroke
            }
            if (v.env < 1e-4f) v.used = false;
        }
    }

    filterL.set(cutoff, reso, ftype, dsp::MultiFilter::Clean, 0.0f);
    filterR.set(cutoff, reso, ftype, dsp::MultiFilter::Clean, 0.0f);
    for (int32_t i = 0; i < frames; ++i) {
        float l, r;
        if (drive > 0.0001f) {
            // Before the volume and normalised on the level a loop reaches
            // there, so the knob changes the character and not the level, and
            // the volume knob doesn't change how hard it drives.
            const float k = 1.0f + drive * 10.0f;
            const float norm = kNominal / dsp::fastTanh(kNominal * k);
            l = dsp::fastTanh(filterL.process(L[i]) * kHouse * k) * norm * volume;
            r = dsp::fastTanh(filterR.process(R[i]) * kHouse * k) * norm * volume;
        } else {
            l = filterL.process(L[i]) * volume * kHouse;
            r = filterR.process(R[i]) * volume * kHouse;
        }
        L[i] = l * panL * 1.4142f;
        R[i] = r * panR * 1.4142f;
    }
    return true;
}

} // namespace acidulous::machine
