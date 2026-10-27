#include "Fathom.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level at full, set so the bank sits with the other machines. */
constexpr float kHouse = 0.7f;
constexpr float kSilent = 1e-5f;
constexpr float kTwoPi = 6.28318530718f;
/** How sharply a bubble rings: about this many cycles before it's faded. */
constexpr float kBubbleQ = 25.0f;
/** Events a second at full density, by kind; wind has none. */
constexpr float kRates[Fathom::KindCount] = {12.0f, 2.0f, 120.0f, 300.0f, 200.0f, 0.0f, 15.0f,
                                             0.0f, 40.0f, 600.0f, 500.0f, 1.5f, 6.0f, 1.0f, 3.0f, 150.0f};
/** Where a struck roof or window rings, Hz, and for how long, seconds. */
constexpr float kTinModes[8] = {530.0f, 910.0f, 1370.0f, 1780.0f, 2120.0f, 2890.0f, 3610.0f, 4730.0f};
constexpr float kGlassModes[8] = {2800.0f, 3460.0f, 4100.0f, 5020.0f, 5900.0f, 6610.0f, 7300.0f, 8650.0f};
constexpr float kTinRing = 0.15f, kGlassRing = 0.06f;
/** Brings the four-pole rumble up to about where the other beds sit. */
constexpr float kThunderGain = 11.0f;
/** Rain on open water has no roof to ring and few bubbles, so it's brought up to the rest. */
constexpr float kOpenRain = 3.0f;

} // namespace

Fathom::Fathom() { initParams(); }

const ParamDef *Fathom::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""}, // bubbles, drips, rain, stream, surf, wind, fire
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"density", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How far the bubbles' sizes, and so their pitches, spread from the note, semitones.
        {"size", 0.0f, 24.0f, 5.0f, Curve::Linear, 0, "st"},
        // How far a bubble's pitch rises as it nears the surface.
        {"rise", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"decay", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"surface", 0.0f, static_cast<float>(SurfaceCount - 1), 0.0f, Curve::Stepped, SurfaceCount, ""}, // water, leaves, tin, glass
        // How much the wind, the fire and the waves come and go.
        {"gust", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Wind caught in a gap, whistling on the note.
        {"whistle", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Beats from one wave to the next.
        {"swell", 1.0f, 16.0f, 4.0f, Curve::Linear, 0, "beats"},
        {"spread", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        // How long the texture takes to come in and to die away, seconds.
        {"fade", 0.01f, 4.0f, 0.3f, Curve::Exponential, 0, "s"},
        {"velocity", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // Kit mode, and the default kit: fire, wind, rain, drips, bubbles, a stream and surf.
        {"kit", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"p01_model", 0.0f, static_cast<float>(KindCount - 1), 6.0f, Curve::Stepped, KindCount, ""},
        {"p01_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p01_density", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"p01_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p02_model", 0.0f, static_cast<float>(KindCount - 1), 6.0f, Curve::Stepped, KindCount, ""},
        {"p02_note", 24.0f, 96.0f, 36.0f, Curve::Stepped, 73, ""},
        {"p02_density", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"p02_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p03_model", 0.0f, static_cast<float>(KindCount - 1), 5.0f, Curve::Stepped, KindCount, ""},
        {"p03_note", 24.0f, 96.0f, 40.0f, Curve::Stepped, 73, ""},
        {"p03_density", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"p03_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p04_model", 0.0f, static_cast<float>(KindCount - 1), 5.0f, Curve::Stepped, KindCount, ""},
        {"p04_note", 24.0f, 96.0f, 52.0f, Curve::Stepped, 73, ""},
        {"p04_density", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"p04_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p05_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p05_note", 24.0f, 96.0f, 36.0f, Curve::Stepped, 73, ""},
        {"p05_density", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"p05_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p06_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p06_note", 24.0f, 96.0f, 36.0f, Curve::Stepped, 73, ""},
        {"p06_density", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"p06_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p07_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p07_note", 24.0f, 96.0f, 43.0f, Curve::Stepped, 73, ""},
        {"p07_density", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
        {"p07_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p08_model", 0.0f, static_cast<float>(KindCount - 1), 1.0f, Curve::Stepped, KindCount, ""},
        {"p08_note", 24.0f, 96.0f, 40.0f, Curve::Stepped, 73, ""},
        {"p08_density", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"p08_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p09_model", 0.0f, static_cast<float>(KindCount - 1), 1.0f, Curve::Stepped, KindCount, ""},
        {"p09_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p09_density", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"p09_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p10_model", 0.0f, static_cast<float>(KindCount - 1), 1.0f, Curve::Stepped, KindCount, ""},
        {"p10_note", 24.0f, 96.0f, 55.0f, Curve::Stepped, 73, ""},
        {"p10_density", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"p10_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p11_model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"p11_note", 24.0f, 96.0f, 36.0f, Curve::Stepped, 73, ""},
        {"p11_density", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"p11_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p12_model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"p12_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p12_density", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"p12_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p13_model", 0.0f, static_cast<float>(KindCount - 1), 3.0f, Curve::Stepped, KindCount, ""},
        {"p13_note", 24.0f, 96.0f, 40.0f, Curve::Stepped, 73, ""},
        {"p13_density", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"p13_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p14_model", 0.0f, static_cast<float>(KindCount - 1), 3.0f, Curve::Stepped, KindCount, ""},
        {"p14_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p14_density", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"p14_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p15_model", 0.0f, static_cast<float>(KindCount - 1), 4.0f, Curve::Stepped, KindCount, ""},
        {"p15_note", 24.0f, 96.0f, 31.0f, Curve::Stepped, 73, ""},
        {"p15_density", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"p15_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p16_model", 0.0f, static_cast<float>(KindCount - 1), 4.0f, Curve::Stepped, KindCount, ""},
        {"p16_note", 24.0f, 96.0f, 36.0f, Curve::Stepped, 73, ""},
        {"p16_density", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"p16_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
    };
    static ParamDef defs[Count];
    static char names[kMacros * MacroParams][12];
    static bool built = false;
    if (!built) {
        for (int32_t i = 0; i < MacroBase; ++i) defs[i] = first[i];
        auto put = [&](int32_t i, const char *n, float mn, float mx, float df, Curve c, int32_t steps) {
            char *name = names[i - MacroBase];
            std::snprintf(name, 12, "%s", n);
            defs[i] = ParamDef{name, mn, mx, df, c, steps, ""};
        };
        for (int k = 0; k < kMacros; ++k) {
            char n[12];
            const int32_t b = MacroBase + k * MacroParams;
            std::snprintf(n, sizeof(n), "mac%d", k + 1);
            put(b, n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0);
            for (int r = 0; r < 4; ++r) {
                std::snprintf(n, sizeof(n), "mac%d_d%d", k + 1, r + 1);
                put(b + 1 + r, n, 0.0f, static_cast<float>(MacroDestCount - 1), 0.0f, Curve::Stepped, MacroDestCount);
                std::snprintf(n, sizeof(n), "mac%d_a%d", k + 1, r + 1);
                put(b + 5 + r, n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0);
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
    int8_t of[Fathom::Count];
    DestMap() {
        using P = Fathom;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {P::Density, P::McDensity}, {P::Size, P::McSize}, {P::Rise, P::McRise}, {P::Decay, P::McDecay},
            {P::Gust, P::McGust}, {P::Whistle, P::McWhistle}, {P::Tone, P::McTone}, {P::Swell, P::McSwell},
            {P::Spread, P::McSpread}, {P::Fade, P::McFade}, {P::VelocityAmount, P::McVelocity},
            {P::Volume, P::McVolume},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
        // The density route moves every pad's own density as well.
        for (int pad = 0; pad < P::kPads; ++pad) of[P::padParam(pad, P::PadDensity)] = static_cast<int8_t>(P::McDensity);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Fathom::paramOf(int32_t p) const {
    const float v = params_.get(p);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : mac_[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Fathom::updateMacros() {
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = params_.get(b);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            // The routes' destinations are steps, so their targets.
            const int dest = static_cast<int>(params_.target(b + 1 + r) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) mac_[dest] += knob * params_.get(b + 5 + r);
        }
    }
    // Two octaves either way.
    mac_[McPitch] = std::clamp(mac_[McPitch], -1.0f, 1.0f);
}

void Fathom::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        v.bed.setSampleRate(sampleRate);
        v.whistle.setSampleRate(sampleRate);
    }
    reset();
}

void Fathom::reset() {
    for (Voice &v : voices) {
        v.used = v.held = false;
        v.level = 0.0f;
        v.until = 0.0f;
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.bed.reset();
        v.whistle.reset();
        v.bedLp = v.roarLp = v.roarLp2 = 0.0f;
        v.gustNow = v.gustAim = 0.0f;
        v.gustLeft = 0;
        v.rollNow = v.rollAim = v.burst = 0.0f;
        v.rollLeft = 0;
        v.waveAt = 0.0;
        v.peak = 0.0f;
        v.quietBlocks = 0;
    }
    for (Grain &g : grains) g.used = false;
    for (SurfaceMode &m : surface) m.y1 = m.y2 = 0.0f;
    for (float &m : mac_) m = 0.0f;
    builtSurface = -1;
    surfaceIn = surfaceBurst = 0.0f;
    bpm = 120.0f;
    bend = wheel = channelPressure_ = 0.0f;
    noise = 0x4f6cdd1du;
    clock = started = 0;
    quietSamples = 0;
    asleep = true;
}

void Fathom::onBlock(int64_t, int64_t, float tempo) { bpm = tempo > 1.0f ? tempo : 120.0f; }

int Fathom::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Fathom::Voice *Fathom::voiceFor(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.note == note) return &v;
    }
    for (Voice &v : voices) {
        if (!v.used) return &v;
    }
    Voice *pick = &voices[0];
    for (Voice &v : voices) {
        if (v.age < pick->age) pick = &v;
    }
    return pick;
}

Fathom::Grain *Fathom::spawn(Voice &, int kind, float hz, float amp, float decaySeconds, float rise) {
    // The quietest grain makes way if they're all busy.
    Grain *g = nullptr;
    for (Grain &c : grains) {
        if (!c.used) {
            g = &c;
            break;
        }
    }
    if (g == nullptr) {
        g = &grains[0];
        for (Grain &c : grains) {
            if (c.amp < g->amp) g = &c;
        }
    }
    g->used = true;
    g->kind = kind;
    g->phase = 0.0f;
    g->hz = hz;
    // A bubble's pitch rises over its life as it nears the surface.
    g->rise = rise * hz / std::fmax(decaySeconds * sampleRate, 1.0f);
    g->amp = amp;
    g->fall = std::exp(-1.0f / std::fmax(decaySeconds * sampleRate, 1.0f));
    g->lp = g->hpLast = 0.0f;
    g->lpCoef = clampf(1.0f - std::exp(-kTwoPi * hz / sampleRate), 0.01f, 1.0f);
    g->pan = clampf(paramOf(Spread), 0.0f, 1.0f) * white();
    g->age = 0;
    g->wait = 0;
    ++started;
    return g;
}

void Fathom::event(Voice &v, float hz) {
    const int kind = v.kind;
    const float size = paramOf(Size);
    const float rise = clampf(paramOf(Rise), 0.0f, 1.0f);
    const float decay = clampf(paramOf(Decay), 0.0f, 1.0f);
    // A bubble rings for about kBubbleQ cycles, longer with the decay knob.
    auto ring = [&](float f) { return (0.3f + 1.4f * decay) * kBubbleQ / (3.14159265f * f); };
    auto sized = [&](float f, float spread) { return f * std::exp2(spread * white() / 12.0f); };
    switch (kind) {
    case Bubbles: {
        const float f = sized(hz, size);
        spawn(v, Ring, f, 0.4f + 0.6f * uniform(), ring(f), rise);
        break;
    }
    case Drips: {
        const float f = sized(hz, size * 0.5f);
        spawn(v, Tap, 6000.0f, 0.25f, 0.002f, 0.0f);
        spawn(v, Ring, f, 1.0f, ring(f) * 1.5f, rise + 0.4f);
        break;
    }
    case Rain: {
        const int surf = std::clamp(steppedTargetOf(Surface), 0, SurfaceCount - 1);
        const float amp = 0.15f + 0.45f * uniform() * uniform();
        switch (surf) {
        case Leaves: spawn(v, Tick, 1500.0f + 2500.0f * uniform(), amp * 1.4f, 0.006f, 0.0f); break;
        case Tin:
        case Glass:
            spawn(v, Tick, 6000.0f, amp * 0.5f, 0.0015f, 0.0f);
            surfaceIn += amp * (surf == Tin ? 0.5f : 0.35f) * (0.5f + 0.5f * uniform());
            break;
        default:
            spawn(v, Tick, 3000.0f + 4000.0f * uniform(), amp * 0.6f * kOpenRain, 0.002f, 0.0f);
            // A few drops on water catch a bubble.
            if (uniform() < 0.08f) {
                const float f = sized(hz * 2.0f, size);
                spawn(v, Ring, f, amp * 0.4f * kOpenRain, ring(f) * 0.5f, rise);
            }
            break;
        }
        break;
    }
    case Stream: {
        if (uniform() < 0.05f) {
            const float f = sized(hz * 0.5f, size * 0.5f);
            spawn(v, Ring, f, 0.8f, ring(f), rise);
        } else {
            const float f = sized(hz * 2.0f, size);
            spawn(v, Ring, f, 0.15f + 0.2f * uniform(), ring(f) * 0.6f, rise);
        }
        break;
    }
    case Surf: {
        const float f = sized(hz * 4.0f, size);
        spawn(v, Ring, f, 0.08f + 0.1f * uniform(), ring(f) * 0.4f, rise);
        break;
    }
    case Fire: {
        // Crackles: mostly small, now and then a loud pop.
        const float u = uniform();
        spawn(v, Crackle, 2500.0f + 5000.0f * uniform(), 0.1f + 1.2f * u * u * u * u, 0.0008f + 0.002f * uniform(), 0.0f);
        break;
    }
    case Hail: {
        // Ice on a hard surface: a hard tick, and often a smaller one as it bounces.
        const float amp = 0.6f + 1.2f * uniform();
        const float f = (2000.0f + 3000.0f * uniform()) * (0.6f + 0.8f * clampf(paramOf(Tone), 0.0f, 1.0f));
        spawn(v, Tick, f, amp * 2.0f, 0.0025f, 0.0f);
        if (uniform() < 0.6f) spawn(v, Tick, f * 1.2f, amp * 0.7f, 0.0015f, 0.0f)->wait =
            static_cast<int32_t>((0.02f + 0.06f * uniform()) * sampleRate);
        const int surf = std::clamp(steppedTargetOf(Surface), 0, SurfaceCount - 1);
        if (surf == Tin || surf == Glass) surfaceIn += amp * (surf == Tin ? 0.15f : 0.1f);
        break;
    }
    case Waterfall: {
        // Countless small bubbles in the roar.
        const float f = sized(hz * 2.0f, size * 1.5f);
        spawn(v, Ring, f, 0.1f + 0.15f * uniform(), ring(f) * 0.5f, rise);
        break;
    }
    case Sizzle: {
        // Steam and fat: tiny crackles, dense and high.
        spawn(v, Crackle, 4000.0f + 6000.0f * uniform(), 0.15f + 0.75f * uniform() * uniform(), 0.0004f + 0.0008f * uniform(), 0.0f);
        break;
    }
    case Ice: {
        // A crack and the glassy ring it sets going in the sheet.
        const float f = sized(hz * 3.0f, size);
        spawn(v, Crackle, 1500.0f + 3000.0f * uniform(), 1.0f + 0.8f * uniform(), 0.002f, 0.0f);
        spawn(v, Ring, f, 0.8f, ring(f) * 2.0f, 0.0f);
        break;
    }
    case Snow: {
        // A step: a cluster of soft crunches.
        // Brighter with the tone knob: fresh powder is soft, packed snow squeaks.
        const float bright = 0.5f + clampf(paramOf(Tone), 0.0f, 1.0f);
        const int n = 5 + static_cast<int>(6.0f * uniform());
        for (int i = 0; i < n; ++i) spawn(v, Crackle, (800.0f + 2500.0f * uniform()) * bright, 0.3f + 0.4f * uniform(), 0.002f + 0.006f * uniform(), 0.0f);
        break;
    }
    case Splash: {
        // A hand in the water: a slap, then a burst of bubbles.
        spawn(v, Tap, 2500.0f, 0.5f, 0.004f, 0.0f);
        for (int i = 0; i < 7; ++i) {
            const float f = sized(hz, size * 1.5f);
            spawn(v, Ring, f, 0.12f + 0.2f * uniform(), ring(f), rise);
        }
        break;
    }
    case Underwater: {
        // Big, slow bubbles heard from below the surface.
        const float f = sized(hz * 0.5f, size);
        spawn(v, Ring, f, 0.6f + 0.4f * uniform(), ring(f) * 2.0f, rise * 0.5f);
        break;
    }
    case Rainstick: {
        // Pebbles falling past the pins: thinning out as the stick empties.
        const float left = std::exp(-static_cast<float>(v.waveAt) / (0.8f + 2.5f * decay));
        if (uniform() < left) spawn(v, Tap, 2000.0f + 4000.0f * uniform(), 0.6f + 1.2f * uniform(), 0.004f, 0.0f);
        break;
    }
    default: break;
    }
}

void Fathom::noteOn(uint8_t note, uint8_t velocity) {
    const bool kit = steppedTargetOf(Kit) != 0;
    const int pad = static_cast<int>(note) - kBaseNote;
    if (kit && (pad < 0 || pad >= kPads)) return;
    const int kind = std::clamp(steppedTargetOf(kit ? padParam(pad, PadModel) : static_cast<int32_t>(Model)), 0, KindCount - 1);
    Voice *v = voiceFor(note);
    const bool fresh = !v->used || v->note != note || v->kind != kind;
    v->note = note;
    v->kind = kind;
    v->pad = kit ? pad : -1;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    if (fresh) {
        v->level = 0.0f;
        v->bed.reset();
        v->whistle.reset();
        v->bedLp = v->roarLp = v->roarLp2 = 0.0f;
        v->waveAt = 0.0;
        v->gustNow = 0.5f;
        v->gustLeft = 0;
        v->rollNow = v->rollAim = v->burst = 0.0f;
        v->rollLeft = 0;
    }
    // A bubble or a drip comes at once.
    v->until = 0.0f;
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Fathom::noteOff(uint8_t note) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.held = false;
    }
}

void Fathom::allNotesOff() {
    for (Voice &v : voices) v.held = false;
}

void Fathom::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel thickens it, as pressure does.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Fathom::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Fathom::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Fathom::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Fathom::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

bool Fathom::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const float density = clampf(paramOf(Density), 0.0f, 1.0f);
    const float densityScale = 0.05f * std::pow(80.0f, density); // 0.05 to 4
    bool rainHeard = false;
    const float tone = clampf(paramOf(Tone), 0.0f, 1.0f);
    const float gustDepth = clampf(paramOf(Gust), 0.0f, 1.0f);
    const float whistleAmount = clampf(paramOf(Whistle), 0.0f, 1.0f);
    const float fade = paramOf(Fade);
    const float fadeCoef = 1.0f - std::exp(-1.0f / (fade * sampleRate * 0.3f));
    const float volume = paramOf(Volume);
    const float dt = 1.0f / sampleRate;
    const float swellBeats = clampf(paramOf(Swell), 1.0f, 16.0f);
    const int surf = std::clamp(steppedTargetOf(Surface), 0, SurfaceCount - 1);

    // The roof or window the rain falls on.
    if (surf != builtSurface) {
        builtSurface = surf;
        const float *hz = surf == Glass ? kGlassModes : kTinModes;
        const float ringFor = surf == Glass ? kGlassRing : kTinRing;
        float meanSin = 0.0f;
        for (int m = 0; m < kSurfaceModes; ++m) meanSin += std::sin(kTwoPi * hz[m] / sampleRate) / kSurfaceModes;
        for (int m = 0; m < kSurfaceModes; ++m) {
            const float r = std::pow(10.0f, -3.0f / (ringFor / (1.0f + 0.25f * static_cast<float>(m)) * sampleRate));
            const float w = kTwoPi * hz[m] / sampleRate;
            surface[m].a1 = 2.0f * r * std::cos(w);
            surface[m].a2 = -r * r;
            // A resonator's gain grows as 1/sin(w), so without this the lowest mode drowns the rest.
            surface[m].in = std::sin(w) / meanSin;
        }
    }

    bool any = false;
    for (Voice &v : voices) {
        if (!v.used) continue;
        const int kind = v.kind;
        rainHeard = rainHeard || kind == Rain || kind == Hail;
        // A pad has its own density and level.
        const float padDensity = v.pad >= 0 ? clampf(paramOf(padParam(v.pad, PadDensity)), 0.0f, 1.0f) : density;
        const float voiceScale = v.pad >= 0 ? 0.05f * std::pow(80.0f, padDensity) : densityScale;
        const float padLevel = v.pad >= 0 ? clampf(paramOf(padParam(v.pad, PadLevel)), 0.0f, 1.0f) / 0.75f : 1.0f;
        const float press = std::fmax(v.held ? (v.pressure >= 0.0f ? v.pressure : channelPressure_) : 0.0f, wheel);
        const float played = v.pad >= 0 ? static_cast<float>(steppedTargetOf(padParam(v.pad, PadNote))) : static_cast<float>(v.note);
        const float hz = noteHz(played + 12.0f * static_cast<float>(steppedTargetOf(Octave)) +
                                paramOf(Tune) / 100.0f + bend * paramOf(BendRange) + v.noteBend +
                                (mac_[McPitch] != 0.0f ? mac_[McPitch] * 24.0f : 0.0f));
        const float aim = v.held ? padLevel * velocityGain(v.velocity, paramOf(VelocityAmount)) * (1.0f + 0.5f * press) : 0.0f;
        const float rate = kRates[kind] * voiceScale * (1.0f + press);
        // The beds: what each texture is besides its grains.
        float thunderLow = 0.0f;
        if (kind == Wind) {
            const float centre = std::fmin(hz * (0.5f + 2.0f * tone) * std::exp2(2.0f * (v.gustNow - 0.5f) * gustDepth), sampleRate * 0.4f);
            v.bed.set(centre, 0.2f);
            v.whistle.set(hz, 0.9f + 0.09f * whistleAmount);
        } else if (kind == Rain) {
            v.bed.set(std::fmin(2000.0f + 8000.0f * tone, sampleRate * 0.4f), 0.1f);
        } else if (kind == Thunder) {
            // The rumble sits under the note, brighter with the tone; the rip higher up.
            const float rumbleHz = clampf((120.0f + 200.0f * tone) * std::sqrt(hz / 82.4f), 20.0f, 400.0f);
            v.bed.set(rumbleHz, 0.1f);
            thunderLow = 1.0f - std::exp(-kTwoPi * rumbleHz / sampleRate);
            v.whistle.set(400.0f + 1500.0f * tone, 0.15f);
        }
        float peak = 0.0f;
        for (int32_t i = 0; i < frames; ++i) {
            // Thunder starts with the strike; the fade only takes it away.
            v.level += (aim - v.level) * (kind == Thunder && aim > v.level ? 1.0f : fadeCoef);
            if (v.held && rate > 0.0f) {
                v.until -= 1.0f;
                if (v.until <= 0.0f) {
                    event(v, hz);
                    // The next one comes at random, at the rate on average.
                    v.until += -std::log(std::fmax(uniform(), 1e-6f)) * sampleRate / rate;
                }
            }
            // Gusts: the wind, the fire and the foam wander.
            if (--v.gustLeft <= 0) {
                v.gustAim = uniform();
                v.gustLeft = static_cast<int32_t>((0.4f + 1.6f * uniform()) * sampleRate);
            }
            v.gustNow += (v.gustAim - v.gustNow) * (dt / 0.4f);
            const float gust = 1.0f - gustDepth + gustDepth * v.gustNow;
            float bed = 0.0f;
            switch (kind) {
            case Rain:
                bed = v.bed.step(white()).bp * 0.3f * std::sqrt(densityScale) * (surf == Water ? kOpenRain : 1.0f);
                break;
            case Stream:
                v.bedLp += (white() - v.bedLp) * 0.08f;
                bed = v.bedLp * 0.25f;
                break;
            case Surf: {
                // A wave: a quick rise to the crash, then a long wash back.
                v.waveAt += static_cast<double>(dt * bpm / 60.0f / swellBeats);
                const float p = static_cast<float>(v.waveAt - std::floor(v.waveAt));
                float wave = p < 0.3f ? (p / 0.3f) * (p / 0.3f) : std::exp(-(p - 0.3f) * 4.0f);
                wave = 1.0f - gustDepth * 0.8f * (1.0f - wave);
                const float c = 1.0f - std::exp(-kTwoPi * (200.0f + 6000.0f * tone * wave) / sampleRate);
                v.bedLp += (white() - v.bedLp) * c;
                bed = v.bedLp * wave * 0.6f;
                // Foam on the crest.
                if (v.held && uniform() < wave * densityScale * 0.004f) event(v, hz);
                break;
            }
            case Wind: {
                const float rush = v.bed.step(white()).bp * gust;
                bed = rush * 6.5f * (1.0f - 0.5f * whistleAmount) + whistleAmount * v.whistle.step(rush).bp * 5.5f;
                break;
            }
            case Thunder: {
                // Up close a crack and a rip of tearing bursts, then the rumble rolling away in swells.
                const float t = static_cast<float>(v.waveAt);
                v.waveAt += static_cast<double>(dt);
                const float tau = 1.5f + 4.0f * clampf(paramOf(Decay), 0.0f, 1.0f);
                const float near = clampf((tone - 0.3f) / 0.6f, 0.0f, 1.0f);
                const float crack = near * std::exp(-t / 0.006f) * white();
                // Each burst of the rip starts at random, thick at first and thinning out.
                const float ripRate = 1500.0f * std::exp(-t / (0.08f + 0.3f * near));
                if (uniform() < ripRate * dt) v.burst = 0.4f + 0.6f * uniform();
                v.burst *= 0.9985f;
                const float rip = v.whistle.step(white() * v.burst).bp * near * std::exp(-t / 0.35f);
                // The rumble: noise through four poles, so no hiss is left above it.
                v.roarLp += (white() - v.roarLp) * thunderLow;
                v.roarLp2 += (v.roarLp - v.roarLp2) * thunderLow;
                const float deep = v.bed.step(v.roarLp2).lp;
                if (--v.rollLeft <= 0) {
                    v.rollAim = uniform();
                    v.rollLeft = static_cast<int32_t>((0.08f + 0.5f * uniform()) * sampleRate);
                }
                v.rollNow += (v.rollAim - v.rollNow) * (dt / 0.12f);
                const float roll = 1.0f - gustDepth + gustDepth * (0.2f + 1.6f * v.rollNow * v.rollNow);
                // Far off it swells up slowly; close by it's there at once, with a boom.
                const float attack = 0.01f + 0.6f * (1.0f - near);
                const float rumble = (1.0f - std::exp(-t / attack)) * std::exp(-t / tau) * roll +
                                     1.0f * near * std::exp(-t / 0.3f);
                bed = crack * 3.0f + rip * 10.0f + deep * rumble * kThunderGain;
                break;
            }
            case Waterfall: {
                // The roar: broad noise, brighter with the tone knob.
                const float c = 1.0f - std::exp(-kTwoPi * (800.0f + 6000.0f * tone) / sampleRate);
                v.bedLp += (white() - v.bedLp) * c;
                // Less its own slow drift, which would be an offset.
                v.roarLp += (v.bedLp - v.roarLp) * 0.002f;
                bed = (v.bedLp - v.roarLp) * 0.5f * (0.8f + 0.2f * gust);
                break;
            }
            case Sizzle: {
                // A high hiss under the crackles.
                const float n = white();
                bed = (n - v.bedLp) * 0.12f * (0.5f + tone);
                v.bedLp = n;
                break;
            }
            case Underwater: {
                // Everything muffled: a dark, slow wash.
                v.roarLp += (white() - v.roarLp) * 0.01f;
                v.roarLp2 += (v.roarLp - v.roarLp2) * 0.01f;
                bed = v.roarLp2 * 2.5f * gust;
                break;
            }
            case Rainstick:
                v.waveAt += static_cast<double>(dt);
                break;
            case Fire: {
                // Hiss on top, and the roar low down.
                const float n = white();
                v.roarLp += (n - v.roarLp) * 0.015f;
                v.roarLp2 += (v.roarLp - v.roarLp2) * 0.015f;
                bed = (n - v.bedLp) * 0.03f * tone + v.roarLp2 * 3.0f * gust;
                v.bedLp = n;
                break;
            }
            default: break;
            }
            const float y = bed * v.level * kHouse * volume;
            L[i] += y;
            R[i] += y;
            peak = std::fmax(peak, std::fabs(y));
        }
        v.peak = peak;
        if (!v.held && v.level < 1e-4f) {
            v.used = false;
        }
        any = any || v.used;
    }

    // The grains, shared by every note.
    const float grainGain = kHouse * volume;
    const float tickRamp = 1.0f / (0.0007f * sampleRate);
    float level = 0.0f;
    for (const Voice &v : voices) level = std::fmax(level, v.level);
    bool grainsLeft = false;
    for (Grain &g : grains) {
        if (!g.used) continue;
        const float pl = std::sqrt(0.5f * (1.0f - g.pan)), pr = std::sqrt(0.5f * (1.0f + g.pan));
        for (int32_t i = 0; i < frames; ++i) {
            if (g.wait > 0) {
                --g.wait;
                continue;
            }
            float y = 0.0f;
            if (g.kind == Ring) {
                y = g.amp * std::sin(kTwoPi * g.phase);
                g.phase += g.hz / sampleRate;
                if (g.phase >= 1.0f) g.phase -= 1.0f;
                g.hz = std::fmin(g.hz + g.rise, sampleRate * 0.45f);
            } else if (g.kind == Tick) {
                // A drop or a hailstone: a band of noise with no low end, eased in so it doesn't pop.
                const float n = white() * g.amp;
                g.lp += (n - g.lp) * g.lpCoef;
                g.hpLast += (g.lp - g.hpLast) * g.lpCoef * 0.2f;
                y = (g.lp - g.hpLast) * std::fmin(g.phase, 1.0f);
                g.phase += tickRamp;
            } else {
                // A tap or a crackle: a burst of noise, its top shaped.
                const float n = white() * g.amp;
                g.lp += (n - g.lp) * g.lpCoef;
                y = g.kind == Crackle ? n - g.lp : g.lp;
            }
            g.amp *= g.fall;
            y *= grainGain * (0.3f + 0.7f * level);
            L[i] += y * pl;
            R[i] += y * pr;
        }
        if (g.amp < kSilent) g.used = false;
        grainsLeft = grainsLeft || g.used;
    }
    // The roof or window under the rain.
    if (rainHeard && (surf == Tin || surf == Glass)) {
        const float surfaceFall = std::exp(-1.0f / (0.002f * sampleRate));
        for (int32_t i = 0; i < frames; ++i) {
            // Each drop strikes it with a short burst of noise, not a click, so it rattles rather than rings a note.
            if (i == 0) surfaceBurst += surfaceIn;
            const float x = white() * surfaceBurst * 0.3f;
            surfaceBurst *= surfaceFall;
            float y = 0.0f;
            for (SurfaceMode &m : surface) {
                const float out = x * m.in + m.a1 * m.y1 + m.a2 * m.y2;
                m.y2 = m.y1;
                m.y1 = out;
                y += out;
            }
            y *= 0.08f * grainGain;
            L[i] += y;
            R[i] += y;
        }
        surfaceIn = 0.0f;
        for (const SurfaceMode &m : surface) grainsLeft = grainsLeft || std::fabs(m.y1) > kSilent;
    }

    if (!any && !grainsLeft) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return true;
}

} // namespace acidulous::machine
