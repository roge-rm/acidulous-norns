#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <engine/dsp/Math.h>
#include <engine/dsp/PitchFollow.h>
#include <engine/machine/Machine.h>
#include <engine/machine/MachineRegistry.h>
#include <engine/machine/brazen/Bore.h>
#include <engine/machine/diction/Throat.h>
#include <engine/machine/draw/DrawKinds.h>
#include <engine/machine/draw/DrawTuning.h>
#include <engine/machine/draw/FreeReed.h>
#include <engine/machine/formulate/Expr.h>
#include <engine/machine/nexus/NexusModule.h>
#include <engine/machine/timber/Pipe.h>
#include <engine/machine/tongue/Reed.h>

// The other machines' instruments as modules: Brazen's horn, Timber's pipe,
// Draw's free reed, Tongue's jaw harp reed, Diction's throat, Formulate's
// expressions, Molt's pitch follower and the whole of Hammer.
//
// Each is the machine's own part, driven the way the machine drives it, so
// it sounds the same; the machine's voice handling, envelopes and effects
// around it are left to the patch.
namespace acidulous::machine::nexus {

using dsp::clampf;

namespace instruments {

inline float linOf(float k, float lo, float hi) { return lo + (hi - lo) * clampf(k, 0.0f, 1.0f); }
inline float expoOf(float k, float lo, float hi) { return lo * std::pow(hi / lo, clampf(k, 0.0f, 1.0f)); }
inline float noteHz(float note) { return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f); }

/** The note a pitch input asks for (0..1 is note 0..127), or the voice's when nothing's in it. */
inline float noteFrom(float in, const Context &c) { return in != 0.0f ? in * 127.0f : c.voicePitch(); }

/**
 * The air for a blown instrument. With a cable in its breath input that's
 * the breath, so an envelope or a pressure source plays it; with none, the
 * voice's own gate and velocity blow it, up in 4 ms and down in 25 ms as
 * Timber's player does, so it plays from the keys with nothing wired.
 */
class Breath {
  public:
    void prepare(float sr) {
        rise = 1.0f - std::exp(-1.0f / (0.004f * sr));
        fall = 1.0f - std::exp(-1.0f / (0.025f * sr));
        reset();
    }
    void reset() { air = 0.0f; }
    float next(float in, bool wired, const Context &c) {
        if (wired) return std::max(in, 0.0f);
        const float want = c.voiceGate() > 0.5f ? c.voiceVelocity() : 0.0f;
        air += (want - air) * (want > air ? rise : fall);
        return air;
    }
  private:
    float air = 0.0f, rise = 0.1f, fall = 0.01f;
};

/** White noise from a small generator, -1..1. */
inline float white(uint32_t &rng) {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/** How often, in samples, the expensive part of a model is set up again. */
constexpr int32_t kRetune = 32;

} // namespace instruments

/**
 * Brazen's horn: a pair of lips on a tube with a bell. Blow it from the
 * breath input; the lips lock to the tube's resonances and get brassier
 * the harder they're blown.
 */
class BoreMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        bore.prepare(sr);
        breath.prepare(sr);
        reset();
    }
    void reset() override {
        bore.clear();
        breath.reset();
        count = 0;
        blowing = false;
        rng = 0x2545f491u;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        semis = linOf(k[0], -24.0f, 24.0f);
        tension = linOf(k[1], 0.45f, 1.35f);
        bell = linOf(k[2], 0.05f, 0.95f);
        size = clampf(k[3], 0.0f, 1.0f);
        brass = clampf(k[4], 0.0f, 1.0f);
        bite = clampf(k[5], 0.0f, 1.0f);
        air = clampf(k[6], 0.0f, 1.0f);
        level = linOf(k[7], 0.0f, 2.0f);
    }
    void setConnected(uint32_t mask) override { breathWired = (mask & 1u) != 0; }
    void step(const float *in, float *out, const Context &c) override {
        using namespace instruments;
        const float push = breath.next(in[0], breathWired, c) * 0.6f;
        // Set up the horn every few samples, as Brazen does once a block:
        // tune() is expensive and the note can't change audibly in between.
        if (count-- <= 0) {
            count = kRetune;
            bore.setFrequency(noteHz(noteFrom(in[1], c) + semis));
            bore.setLips(tension * (0.94f + bite * 0.12f), 0.6f);
            bore.setLipGain(0.7f + bite * 0.6f);
            bore.setBell(0.99f - size * 0.04f, bell * (0.15f + size * 0.55f));
            bore.setRest(0.35f);
            bore.setBite(bite * 0.9f + brass * 0.5f);
            bore.setBrass(brass * std::min(1.0f, push * 1.4f));
            bore.setLoss(0.999f);
            bore.setPressure(push);
            bore.tune();
            // A new breath gets the tongue, so a low note speaks as fast as a high one.
            const bool now = push > 0.02f;
            if (now && !blowing) bore.tongue();
            blowing = now;
        }
        const float hiss = white(rng) * air * 0.25f;
        out[0] = bore.step(push, hiss * push) * level;
    }
  private:
    brazen::Bore bore;
    instruments::Breath breath;
    bool breathWired = false, blowing = false;
    int32_t count = 0;
    uint32_t rng = 0x2545f491u;
    float semis = 0.0f, tension = 1.0f, bell = 0.55f, size = 0.6f, brass = 0.45f, bite = 0.4f, air = 0.15f, level = 1.0f;
};

/**
 * Timber's pipe: a reed, a double reed or an air jet on a tube with a row
 * of holes. The kind sets which, and the tube's shape with it: a single
 * reed on a cylinder (odd partials, like a clarinet), a double reed on a
 * cone, a jet on an open pipe.
 */
class PipeMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        pipe.prepare(sr);
        breath.prepare(sr);
        reset();
    }
    void reset() override {
        pipe.clear();
        breath.reset();
        count = 0;
        blowing = false;
        rng = 0x68e31da4u;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        kind = std::min(2, static_cast<int>(clampf(k[0], 0.0f, 0.9999f) * 3.0f));
        semis = linOf(k[1], -24.0f, 24.0f);
        reed = clampf(k[2], 0.0f, 1.0f);
        lip = clampf(k[3], 0.0f, 1.0f);
        holes = expoOf(k[4], 300.0f, 6000.0f);
        bell = linOf(k[5], 0.05f, 0.9f);
        air = clampf(k[6], 0.0f, 1.0f);
        level = linOf(k[7], 0.0f, 2.0f);
    }
    void setConnected(uint32_t mask) override { breathWired = (mask & 1u) != 0; }
    void step(const float *in, float *out, const Context &c) override {
        using namespace instruments;
        const float push = breath.next(in[0], breathWired, c) * 0.85f;
        if (count-- <= 0) {
            count = kRetune;
            const float hz = noteHz(noteFrom(in[1], c) + semis);
            pipe.setNote(hz);
            pipe.setShape(kind == 0, 1);
            // The instrument an octave below the note played, where Timber's
            // patches play.
            pipe.setTube(hz * 0.5f);
            pipe.setLattice(holes, 0.0f, 0.4f);
            pipe.setBelow(1.0f);
            pipe.setFork(0.2f);
            // The same two knobs do for the reed and the jet: stiffness and
            // embouchure, or how long the jet takes to cross and where it aims.
            pipe.setReed(kind, 0.05f + reed * 0.95f, 0.25f + lip * 0.65f);
            pipe.setJet(0.5f * std::pow(2.0f, (reed - 0.47f) * 2.4f), clampf(0.3f + (lip - 0.46f) * 1.6f, -0.8f, 0.8f));
            pipe.setBell(0.9f, bell);
            pipe.setLoss(0.999f);
            pipe.setPressure(push);
            pipe.setDrive(1.0f);
            pipe.tune();
            const bool now = push > 0.02f;
            if (now && !blowing) {
                pipe.tune();
                pipe.lift();
            }
            blowing = now;
        }
        out[0] = pipe.step(push, white(rng) * air * 0.35f * push) * level;
    }
  private:
    timber::Pipe pipe;
    instruments::Breath breath;
    bool breathWired = false, blowing = false;
    int32_t count = 0, kind = 0;
    uint32_t rng = 0x68e31da4u;
    float semis = 0.0f, reed = 0.47f, lip = 0.46f, holes = 1500.0f, bell = 0.4f, air = 0.12f, level = 1.0f;
};

/**
 * Draw's free reed: a brass or steel tongue swinging through its slot, as
 * in a harmonica, an accordion, a melodica, a harmonium or a concertina.
 * The kind picks the maker's reed; it speaks slowly when blown softly and
 * chokes when blown far too hard, as real ones do.
 */
class ReedMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        reed.prepare(sr);
        breath.prepare(sr);
        reset();
    }
    void reset() override {
        reed.clear();
        reed.seed(0x1b873593u);
        breath.reset();
        count = 0;
        madeHz = -1.0f;
        madeKind = -1;
        madeAir = -1.0f;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        kind = std::min(draw::kMakes - 1, static_cast<int>(clampf(k[0], 0.0f, 0.9999f) * draw::kMakes));
        semis = linOf(k[1], -24.0f, 24.0f);
        // How hard against the pressure the kind is made and tuned for.
        blow = expoOf(k[2], 0.25f, 3.0f);
        air = linOf(k[3], 0.0f, 3.0f);
        level = linOf(k[4], 0.0f, 2.0f);
    }
    void setConnected(uint32_t mask) override { breathWired = (mask & 1u) != 0; }
    void step(const float *in, float *out, const Context &c) override {
        using namespace instruments;
        const float wind = breath.next(in[0], breathWired, c) * blow * kPressure[kind];
        if (count-- <= 0) {
            count = kRetune;
            // A free reed plays a little under its own frequency, so it's
            // filed sharp by Draw's table to sound its note.
            const float note = clampf(noteFrom(in[1], c) + semis, 0.0f, 127.0f);
            const int lo = std::min(126, static_cast<int>(note));
            const float *table = draw::kReedTuning[kind];
            const float cents = table[lo] + (table[lo + 1] - table[lo]) * (note - static_cast<float>(lo));
            const float hz = clampf(noteHz(note) * std::pow(2.0f, cents / 1200.0f), 27.5f, 4500.0f);
            // Made again only when something about it changed: making a reed
            // isn't free, and a sounding reed keeps its swing across it.
            if (std::fabs(hz - madeHz) > madeHz * 0.0003f || kind != madeKind || air != madeAir) {
                draw::ReedMake make = draw::kMakes_[kind];
                make.turbulence *= air;
                reed.make(hz, make);
                madeHz = hz;
                madeKind = kind;
                madeAir = air;
            }
        }
        const float s = reed.step(wind, kSupply[kind]);
        out[0] = std::isfinite(s) ? s * kScale * kVoiced[kind] * level : 0.0f;
    }
  private:
    /** How freely each kind's wind feeds its reed, as Draw's kinds have it. */
    static constexpr float kSupply[draw::kMakes] = {5e-6f, 2e-5f, 4e-6f, 2e-5f, 2e-5f};
    /** The pressure each kind is played at, Pa, as Draw's kinds have it. */
    static constexpr float kPressure[draw::kMakes] = {600.0f, 450.0f, 550.0f, 900.0f, 450.0f};
    /** How loud each kind is voiced, as Draw's kinds have it. */
    static constexpr float kVoiced[draw::kMakes] = {0.85f, 0.7f, 0.82f, 0.33f, 0.9f};
    /** From the flow's change to about the level of the other modules. */
    static constexpr float kScale = 0.08f;
    draw::FreeReed reed;
    instruments::Breath breath;
    bool breathWired = false;
    int32_t count = 0, kind = 0, madeKind = -1;
    float madeHz = -1.0f, madeAir = -1.0f;
    float semis = 0.0f, blow = 1.0f, air = 1.0f, level = 1.0f;
};

/**
 * Tongue's jaw harp reed: a strip clamped at one end, ringing in the modes
 * of a bar, plucked from the trigger input and kept going, if asked, by the
 * air on the drive input. Its tune comes from the mouth: patch it into a
 * throat and move the vowel.
 */
class JawMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        rate = sr;
        reed.prepare(sr);
        reset();
    }
    void reset() override {
        reed.clear();
        count = 0;
        wasHigh = false;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        kind = std::min(2, static_cast<int>(clampf(k[0], 0.0f, 0.9999f) * 3.0f));
        semis = linOf(k[1], -24.0f, 24.0f);
        ring = expoOf(k[2], 0.05f, 6.0f);
        overtones = clampf(k[3], 0.0f, 1.0f);
        overRing = linOf(k[4], 0.05f, 1.0f);
        snap = clampf(k[5], 0.0f, 1.0f);
        drive = clampf(k[6], 0.0f, 1.0f);
        level = linOf(k[7], 0.0f, 2.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        using namespace instruments;
        if (count-- <= 0) {
            count = kRetune;
            reed.setRatios(kRatios[kind][0], kRatios[kind][1]);
            reed.tune(noteHz(noteFrom(in[1], c) + semis), ring, overtones, overRing);
            // Enough air cancels the reed's own loss and it sustains.
            const float air = clampf(drive + in[2], 0.0f, 1.0f);
            reed.setDrive(air * (6.907755f / std::max(ring, 0.05f) + 6.0f));
        }
        // A pluck on each rising edge, as hard as the trigger is high.
        const bool high = in[0] > 0.5f;
        if (high && !wasHigh) reed.pluck(std::min(in[0], 2.0f), 1.3e-3f, snap);
        wasHigh = high;
        out[0] = reed.step(0.0f) * 0.5f * level;
    }
  private:
    /** The bar's second and third modes against the first: steel, brass, bamboo, as Tongue's kinds have them. */
    static constexpr float kRatios[3][2] = {{6.267f, 17.55f}, {5.6f, 15.5f}, {5.3f, 14.5f}};
    tongue::Reed reed;
    float rate = 48000.0f;
    int32_t count = 0, kind = 0;
    bool wasHigh = false;
    float semis = 0.0f, ring = 1.0f, overtones = 0.3f, overRing = 0.4f, snap = 0.5f, drive = 0.0f, level = 1.0f;
};

/**
 * Diction's throat: five formants, a nose and the ring above them, as a
 * filter. Anything put through it is shaped into a vowel, from oo through
 * oh, ah and eh to ee, and the size moves every formant as a bigger or
 * smaller throat would.
 */
class ThroatMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        throat.prepare(sr);
        reset();
    }
    void reset() override {
        throat.reset();
        count = 0;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        vowel = clampf(k[0], 0.0f, 1.0f);
        size = clampf(k[1], 0.0f, 1.0f);
        nasal = clampf(k[2], 0.0f, 1.0f);
        ringDb = linOf(k[3], -12.0f, 12.0f);
        level = linOf(k[4], 0.0f, 2.0f);
    }
    void step(const float *in, float *out, const Context &) override {
        using namespace instruments;
        if (count-- <= 0) {
            count = 16;
            // Between two of the five vowels, as Diction's vowel control does.
            const float at = clampf(vowel + in[1], 0.0f, 1.0f) * 4.0f;
            const int lower = std::min(3, static_cast<int>(at));
            const float t = at - static_cast<float>(lower);
            float f[3];
            for (int i = 0; i < 3; ++i) {
                f[i] = diction::kKnobVowels[lower][i] + (diction::kKnobVowels[lower + 1][i] - diction::kKnobVowels[lower][i]) * t;
            }
            // Size: a longer throat is lower in every formant.
            const float ratio = std::pow(2.0f, (0.5f - clampf(size + in[2], 0.0f, 1.0f)) * 0.8f);
            const diction::Shape shape = diction::shapeOf(f, nasal, ratio, diction::Throat::kReferenceHz);
            throat.setShape(shape);
            throat.setRing(ringDb + diction::ringFor(f, shape));
        }
        const float y = throat.process(in[0] * kIn, 0.0f, 0.0f);
        out[0] = std::isfinite(y) ? y * level : 0.0f;
    }
  private:
    /** What goes in, scaled so a full-level saw comes out at about the level it went in. */
    static constexpr float kIn = 0.25f;
    diction::Throat throat;
    int32_t count = 0;
    float vowel = 0.5f, size = 0.5f, nasal = 0.0f, ringDb = 0.0f, level = 1.0f;
};

/**
 * Formulate's expressions: a formula of t, the note and three knobs,
 * computed a sample at a time as an 8-bit wave. The input comes in as x,
 * so a formula can shape another sound as well as make one. Its text is
 * kept in the patch with the module.
 */
class FormulaMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        rate = sr;
        reset();
    }
    void reset() override {
        t = 0;
        clock = 0.0;
        rng = 0x9e3779b9u;
    }
    bool setText(const std::string &text, std::string &error) override {
        if (text.empty()) { expr = formulate::Expr(); return true; }
        return formulate::Expr::parse(text, expr, error);
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        semis = linOf(k[0], -24.0f, 24.0f);
        speed = expoOf(k[1], 0.0625f, 16.0f);
        a = static_cast<int32_t>(clampf(k[2], 0.0f, 1.0f) * 255.0f);
        b = static_cast<int32_t>(clampf(k[3], 0.0f, 1.0f) * 255.0f);
        cKnob = static_cast<int32_t>(clampf(k[4], 0.0f, 1.0f) * 255.0f);
        keyed = k[5] >= 0.5f;
        level = linOf(k[6], 0.0f, 1.0f);
    }
    void step(const float *in, float *out, const Context &c) override {
        using namespace instruments;
        if (expr.empty()) { out[0] = 0.0f; return; }
        const float note = noteFrom(in[1], c) + semis;
        const float hz = noteHz(note);
        // The formula's clock, as Formulate's: keyed, it runs at a rate set
        // by the note, so the same formula plays in tune.
        clock += keyed ? (hz / 55.0) * speed : static_cast<double>(speed);
        while (clock >= 1.0) { clock -= 1.0; ++t; }
        rng = rng * 1664525u + 1013904223u;
        formulate::Vars v;
        v.t = t;
        v.f = static_cast<int32_t>(hz);
        v.n = static_cast<int32_t>(std::lround(note));
        v.v = static_cast<int32_t>(c.voiceVelocity() * 127.0f);
        v.x = static_cast<int32_t>(clampf(in[0], -1.0f, 1.0f) * 127.0f) + 128;
        v.a = a;
        v.b = b;
        v.c = cKnob;
        v.r = static_cast<int32_t>((rng >> 16) & 0xff);
        v.sr = static_cast<int32_t>(rate);
        const int32_t value = expr.eval(v) & 0xff;
        out[0] = (static_cast<float>(value) - 128.0f) / 128.0f * level;
    }
  private:
    formulate::Expr expr;
    float rate = 48000.0f;
    int32_t t = 0;
    double clock = 0.0;
    uint32_t rng = 0x9e3779b9u;
    float semis = 0.0f, speed = 1.0f, level = 0.5f;
    int32_t a = 0, b = 0, cKnob = 0;
    bool keyed = true;
};

/**
 * Molt's ear: the pitch of whatever's in its input, as a pitch for other
 * modules, a gate while it's sure of it, and the input's level. Sing or
 * play into it and the patch follows.
 */
class FollowMod final : public Module {
  public:
    void prepare(float sr, int32_t) override {
        rate = sr;
        follow.prepare(sr);
        reset();
    }
    void reset() override {
        follow.reset();
        count = 0;
        note = 0.0f;
        gate = 0.0f;
        level = 0.0f;
    }
    void setKnobs(const float *k) override {
        using namespace instruments;
        sure = linOf(k[0], 0.3f, 0.95f);
        glide = dsp::onePoleCoeff(expoOf(k[1], 0.001f, 0.5f), rate);
        snap = k[2] >= 0.5f;
    }
    void step(const float *in, float *out, const Context &) override {
        follow.push(&in[0], 1);
        level += (std::fabs(in[0]) - level) * 0.002f;
        if (count-- <= 0) {
            // A whole look spread over a few calls, so no sample carries it.
            count = 64;
            follow.update(0.25f);
            const float hz = follow.pitch();
            if (follow.sureness() >= sure && hz > 0.0f) {
                gate = 1.0f;
                target = 69.0f + 12.0f * std::log2(hz / 440.0f);
                if (snap) target = std::round(target);
            } else {
                gate = 0.0f;
            }
        }
        if (gate > 0.0f) note += (target - note) * glide;
        out[0] = clampf(note, 0.0f, 127.0f) / 127.0f;
        out[1] = gate;
        out[2] = level;
    }
  private:
    dsp::PitchFollow follow;
    float rate = 48000.0f;
    int32_t count = 0;
    float note = 0.0f, target = 0.0f, gate = 0.0f, level = 0.0f;
    float sure = 0.6f, glide = 0.01f;
    bool snap = false;
};

/**
 * A whole machine as a module, played from a pitch and a gate: a rising gate
 * plays the pitch's note, a falling one lets it go. The knobs are the
 * machine's parameters of the same names, as 0..1. It renders in blocks of
 * [kBlock], which is its latency, two thirds of a millisecond.
 *
 * Used for Hammer, whose strings are tuned key by key from its own tables
 * and can't be lifted out on their own.
 */
class MachineMod final : public Module {
  public:
    static constexpr int kBlock = 32;

    MachineMod(int32_t type, const char *machine) : type(type), machine(machine) {}
    ~MachineMod() override { delete host; }

    void prepare(float sr, int32_t) override;
    void reset() override {
        if (host != nullptr) host->reset();
        std::fill(std::begin(outL), std::end(outL), 0.0f);
        std::fill(std::begin(outR), std::end(outR), 0.0f);
        pos = 0;
        playing = -1;
        wasHigh = false;
    }
    void setKnobs(const float *k) override {
        for (int i = 0; i < kKnobs; ++i) knob[i] = k[i];
    }
    void step(const float *in, float *out, const Context &c) override {
        out[0] = outL[pos];
        out[1] = outR[pos];
        if (pos == 0) {
            blockTick = c.tick;
            // Notes start and stop on block boundaries: a block is a third
            // of the shortest note anyone plays.
            const bool high = in[1] > 0.5f;
            const int note = std::clamp(static_cast<int>(std::lround(instruments::noteFrom(in[0], c))), 0, 127);
            if (host != nullptr) {
                if (high && (!wasHigh || note != playing)) {
                    if (playing >= 0) host->noteOff(static_cast<uint8_t>(playing));
                    const float vel = in[2] > 0.0f ? in[2] : c.voiceVelocity();
                    host->noteOn(static_cast<uint8_t>(note), static_cast<uint8_t>(std::clamp(static_cast<int>(vel * 127.0f), 1, 127)));
                    playing = note;
                } else if (!high && wasHigh && playing >= 0) {
                    host->noteOff(static_cast<uint8_t>(playing));
                    playing = -1;
                }
            }
            wasHigh = high;
        }
        if (++pos < kBlock) return;
        pos = 0;
        if (host == nullptr) return;
        // The machine ticks its own parameters as it renders.
        for (int i = 0; i < kKnobs; ++i) if (param[i] >= 0) host->params().set(param[i], clampf(knob[i], 0.0f, 1.0f));
        host->onBlock(static_cast<int64_t>(blockTick), static_cast<int64_t>(blockTick + c.tickInc * kBlock), c.bpm);
        std::fill(std::begin(outL), std::end(outL), 0.0f);
        std::fill(std::begin(outR), std::end(outR), 0.0f);
        if (!host->render(outL, outR, kBlock)) std::copy(std::begin(outL), std::end(outL), outR);
    }
    bool monoCapable() const override { return true; }
    bool polyCapable() const override { return false; }
  private:
    int32_t type;
    const char *machine;
    Machine *host = nullptr;
    int32_t param[kKnobs]{};
    float knob[kKnobs]{};
    float outL[kBlock]{}, outR[kBlock]{};
    double blockTick = 0.0;
    int pos = 0, playing = -1;
    bool wasHigh = false;
};

} // namespace acidulous::machine::nexus
