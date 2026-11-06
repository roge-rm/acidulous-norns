#include "Bias.h"
#include <engine/core/HeldBytes.h>
#include <cmath>
#include <engine/core/Constants.h>
#include <engine/core/InputBus.h>
#include <engine/core/Settings.h>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

namespace {
/** A comped lane read by the stretcher, in region frames. */
struct CompReader {
    const audio::Reel::Region *region;
    int32_t *hint;
    float scaled(int32_t ch, int64_t i) const {
        float l = 0.0f, r = 0.0f;
        region->compAt(i, *hint, l, r);
        return ch == 0 ? l : r;
    }
    float raw(int32_t ch, int64_t i) const { return scaled(ch, i); }
};
} // namespace

const ParamDef *Bias::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // The four lanes, each with a level and a mute. They're parameters so
        // they can be automated, mapped and recorded.
        {"lane1", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"lane2", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"lane3", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"lane4", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mute1", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"mute2", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"mute3", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"mute4", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"gain", -18.0f, 18.0f, 0.0f, Curve::Linear, 0, "dB"},
        // Off by default, so a take plays at the speed it was recorded.
        {"stretch", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"monitor", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // The tape colour. Every default does nothing, so a new track plays
        // back untouched.
        {"hiss", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"hisstone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"lowcut", 20.0f, 800.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"highcut", 1000.0f, 20000.0f, 20000.0f, Curve::Exponential, 0, "Hz"},
        {"bump", -6.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"bumpfreq", 30.0f, 200.0f, 90.0f, Curve::Exponential, 0, "Hz"},
        {"sat", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"comp", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"wow", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"flutter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"speed", 0.25f, 4.0f, 1.0f, Curve::Exponential, 0, ""},
        {"bleed", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"drop", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bits", 4.0f, 24.0f, 24.0f, Curve::Stepped, 21, ""},
        {"rate", 0.05f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"smear", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"width", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Bias::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    colour.prepare(sampleRate);
    for (auto &w : stretcher) w.prepare();
    // A low shelf instead of a highpass, so the bleed gets thinner in the
    // lows without cutting them off completely.
    bleedHp[0].lowShelf(300.0f, -18.0f, sampleRate);
    bleedHp[1].lowShelf(300.0f, -18.0f, sampleRate);
    reset();
}

/**
 * Reads the tape colour settings once a block.
 *
 * Uses smoothed values, since these either scale samples (where a jump would
 * click) or set coefficients that are only recomputed when they move.
 */
bias::ColourSpec Bias::colourOf() {
    bias::ColourSpec s;
    s.hiss = paramOfIndex(Hiss);
    s.hissTone = paramOfIndex(HissTone);
    s.lowCut = paramOfIndex(LowCut);
    s.highCut = paramOfIndex(HighCut);
    s.bump = paramOfIndex(Bump);
    s.bumpFreq = paramOfIndex(BumpFreq);
    s.sat = paramOfIndex(Sat);
    s.comp = paramOfIndex(Comp);
    s.wow = paramOfIndex(Wow);
    s.flutter = paramOfIndex(Flutter);
    s.speed = paramOfIndex(Speed);
    s.drop = paramOfIndex(Drop);
    s.bits = paramOfIndex(Bits);
    s.rate = paramOfIndex(Rate);
    s.smear = paramOfIndex(Smear);
    s.width = paramOfIndex(Width);
    // With every control at neutral the whole chain is skipped. Running it
    // would cost CPU and change the samples through rounding, and the stem
    // export test expects them to be identical.
    s.any = s.hiss > 0.0f || s.lowCut > 21.0f || s.highCut < 19500.0f ||
            s.bump != 0.0f || s.sat > 0.0f || s.comp > 0.0f || s.wow > 0.0f ||
            s.flutter > 0.0f || s.drop > 0.0f || s.bits < 23.5f || s.rate < 0.999f ||
            s.smear > 0.0f || std::fabs(s.width - 1.0f) > 0.001f;
    return s;
}

void Bias::reset() {
    colour.reset();
    bleedHp[0].reset();
    bleedHp[1].reset();
    for (auto &c : cursors) c.invalidate();
    for (auto &h : pieceHint) h = 0;
    for (auto &w : stretcher) w.reset();
    lastCycleTick = -1;
    cell = nullptr;
    cycleTick = 0;
    playing = false;
    muted = false;
    params_.jumpAll();
}

int64_t Bias::heldBytes() const { return held::bytes(reel); }

void *Bias::swapObject(int32_t slot, void *object) {
    if (slot == 1) {
        // Listening plays instead of the song, so a listen still queued when
        // an export or freeze starts is handed back to be deleted rather than
        // heard in the file. Stopping one is always allowed.
        if (object != nullptr && EngineSettings::get().offlineRender.load(std::memory_order_relaxed)) return object;
        auto *old = const_cast<audio::Listen *>(listen);
        listen = static_cast<const audio::Listen *>(object);
        listenAt = 0;
        listenHint = 0;
        listenFrames.store(listen != nullptr ? 0 : -1, std::memory_order_relaxed);
        return old;
    }
    if (slot != 0) return object; // nothing else is mounted here, so hand it back
    auto *old = const_cast<audio::Reel *>(reel);
    reel = static_cast<const audio::Reel *>(object);
    // The cell points into the old reel, so it's no longer valid.
    cell = nullptr;
    for (auto &c : cursors) c.invalidate();
    for (auto &h : pieceHint) h = 0;
    return old;
}

Bias::~Bias() {
    delete reel;
    delete listen;
}

void Bias::onScene(int64_t sceneId, int64_t tick, bool isPlaying, bool clipMuted) {
    playing = isPlaying;
    muted = clipMuted;
    // A stretcher may only be moved at the start of a cycle. Seeking one
    // mid-phrase clicks. Letting it run freely between cycles is safe since
    // its clock is exact and the position can't drift.
    const bool wrapped = tick < lastCycleTick;
    lastCycleTick = tick;
    cycleTick = tick;
    const audio::Reel::Cell *want = (reel != nullptr) ? reel->find(sceneId) : nullptr;
    if (want == cell) {
        if (wrapped) reseed = true;
        return;
    }
    reseed = true;
    // Moving to another cell: every lane restarts from where the new cell
    // says.
    cell = want;
    for (auto &c : cursors) c.invalidate();
    for (auto &h : pieceHint) h = 0;
}

bool Bias::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    bias::ColourSpec spec = colourOf();
    if (colourBypass) spec.any = false;
    colour.setBlock(spec);
    if (listen != nullptr && listen->playable()) {
        renderListen(L, R, frames);
        addInput(L, R, frames);
        if (spec.any) colour.process(L, R, frames);
        return true;
    }
    if (cell == nullptr || !playing || muted) {
        // A muted or empty cell still runs the tape colour, since blank tape
        // still hisses. It's skipped when there's no colour (the default).
        //
        // It also still monitors, since an empty track is the one you're
        // about to record onto.
        const float mon = paramOfIndex(Monitor);
        if (mon > 0.0001f) {
            const InputBus &bus = InputBus::get();
            if (bus.live()) {
                const float *in = bus.block();
                const int32_t n = bus.frames() < frames ? bus.frames() : frames;
                for (int32_t i = 0; i < n; ++i) {
                    L[i] += in[static_cast<size_t>(i) * 2] * mon;
                    R[i] += in[static_cast<size_t>(i) * 2 + 1] * mon;
                }
            }
        }
        if (spec.any) colour.process(L, R, frames);
        return true;
    }

    const float gain = dsp::dbToGain(paramOfIndex(Gain));
    // Muted lanes are still rendered when there's bleed, since on a real
    // four-track a muted track still leaks faintly onto its neighbours.
    // Nothing extra is rendered when bleed is off.
    const float bleed = paramOfIndex(Bleed);
    const bool bleeding = bleed > 0.001f;
    const bool stretching = steppedTargetOf(Stretch) >= 1;
    float bleedL[kBlockFrames] = {0.0f};
    float bleedR[kBlockFrames] = {0.0f};

    for (int32_t lane = 0; lane < audio::kReelLanes; ++lane) {
        if (!cell->has(lane)) continue;
        const bool laneMuted = steppedTargetOf(Mute1 + lane) >= 1;
        if (laneMuted && !bleeding) continue;
        const float raw = paramOfIndex(Lane1 + lane) * gain;
        const float level = laneMuted ? 0.0f : raw;
        if (level <= 0.0f && !bleeding) continue;

        const audio::Reel::Region &r = cell->lanes[lane];
        const audio::Reel::Source &src = *r.source;
        const int64_t into = cycleTick - r.startTick;
        if (into < 0) continue; // a punch-in the song hasn't reached yet

        // Stretch on: `dsp::Wsola` reads the take at the ratio of the two
        // tempos without changing its pitch, so it lasts exactly as long as
        // the cell. Off: the take starts on the bar and plays at its own
        // speed, and the cell shows an amber warning at another tempo.
        if (stretching && r.bpm > 1.0f && std::fabs(songBpm / r.bpm - 1.0f) > 0.002f) {
            const float rate = songBpm / r.bpm;
            // Loops wrap inside the stretcher, so the end joins the start
            // smoothly without losing the last window. A comp is read in
            // region frames, a single take in its file's.
            const bool comp = r.comped();
            const int64_t origin = comp ? 0 : r.offset;
            const int64_t regionEnd =
                comp ? r.frames
                     : r.offset + (r.frames < src.frames - r.offset ? r.frames : src.frames - r.offset);
            stretcher[lane].setLoop(r.loop);
            // Set at the start of the cycle and free-running after that. Its
            // clock is exact, so nothing drifts.
            if (reseed) {
                // Where in the take the cycle starts, at the song's tempo,
                // since that's what the cell is measured in.
                const double perSongTick =
                    static_cast<double>(sampleRate) * 60.0 / (static_cast<double>(songBpm) * kPPQN);
                const auto out = static_cast<int64_t>(static_cast<double>(into) * perSongTick);
                auto from = static_cast<int64_t>(out * rate);
                if (r.loop && regionEnd > origin) from %= regionEnd - origin;
                stretcher[lane].seek(origin + from);
            }
            float takenL[kBlockFrames] = {0.0f}, takenR[kBlockFrames] = {0.0f};
            float *taken[2] = {takenL, takenR};
            int32_t got = 0;
            if (comp) {
                got = stretcher[lane].fillFrom(taken, CompReader{&r, &pieceHint[lane]}, 0, regionEnd, frames, rate);
            } else {
                const int16_t *from2[2] = {src.lp, src.stereo && src.rp != nullptr ? src.rp : src.lp};
                got = stretcher[lane].fill(taken, from2, r.offset, regionEnd, frames, rate);
            }
            // The fade is measured in the take's own frames, so a stretched
            // take fades over the same audio and crossfades still line up
            // when the tempo changes.
            const int64_t base = stretcher[lane].sourcePosition() - origin;
            for (int32_t i = 0; i < got; ++i) {
                int64_t at = base + static_cast<int64_t>(i * rate);
                if (r.loop && r.frames > 0) at %= r.frames;
                const float env = r.fadeAt(at);
                const float l = takenL[i] * env, rr = takenR[i] * env;
                L[i] += l * level;
                R[i] += rr * level;
                if (bleeding) {
                    bleedL[i] += l * raw;
                    bleedR[i] += rr * raw;
                }
            }
            continue;
        }

        // Where in the region this block starts, as real time since the entry,
        // using the song's tempo. The take's tempo must not be used here: the
        // cursor moves one frame per frame, so at another tempo the anchor
        // and cursor would disagree and FrameCursor would keep jumping back,
        // making it stutter.
        const double perTick =
            static_cast<double>(sampleRate) * 60.0 / (static_cast<double>(songBpm) * kPPQN);
        const int64_t want = static_cast<int64_t>(static_cast<double>(into) * perTick);
        if (!r.loop && want >= r.frames) continue; // past the end: silent, no wrap
        cursors[lane].anchor(r.loop && r.frames > 0 ? want % r.frames : want);

        int64_t at = cursors[lane].at;
        if (r.comped()) {
            // The pieces, each from its own take, crossfading where they meet.
            for (int32_t i = 0; i < frames; ++i) {
                if (at >= r.frames) {
                    if (!r.loop) break;
                    at = 0;
                }
                float l = 0.0f, rr = 0.0f;
                r.compAt(at, pieceHint[lane], l, rr);
                const float env = r.fadeAt(at);
                l *= env;
                rr *= env;
                L[i] += l * level;
                R[i] += rr * level;
                if (bleeding) {
                    bleedL[i] += l * raw;
                    bleedR[i] += rr * raw;
                }
                ++at;
            }
            cursors[lane].at = at;
            continue;
        }
        for (int32_t i = 0; i < frames; ++i) {
            if (at >= r.frames) {
                if (!r.loop) break;
                at = 0;
            }
            const int64_t s = r.offset + at;
            if (s >= 0 && s < src.frames) {
                // Read through the pointers, so takes in memory and takes
                // mapped from a cache file work the same.
                const float env = r.fadeAt(at);
                const float l = static_cast<float>(src.lp[static_cast<size_t>(s)]) * (1.0f / 32768.0f) * env;
                const float rr = src.stereo
                                     ? static_cast<float>(src.rp[static_cast<size_t>(s)]) * (1.0f / 32768.0f) * env
                                     : l;
                L[i] += l * level;
                R[i] += rr * level;
                if (bleeding) {
                    bleedL[i] += l * raw;
                    bleedR[i] += rr * raw;
                }
            }
            ++at;
        }
        cursors[lane].at = at;
    }

    if (bleeding) {
        // Thinned out, since bleed with full bass sounds like a second copy
        // of the track.
        const float amount = bleed * bleed * 0.25f;
        for (int32_t i = 0; i < frames; ++i) {
            L[i] += bleedHp[0].process(bleedL[i]) * amount;
            R[i] += bleedHp[1].process(bleedR[i]) * amount;
        }
    }

    // The live input, added before the tape colour and the inserts, so it
    // goes through both.
    const float monitor = paramOfIndex(Monitor);
    if (monitor > 0.0001f) {
        const InputBus &bus = InputBus::get();
        if (bus.live()) {
            const float *in = bus.block();
            const int32_t n = bus.frames() < frames ? bus.frames() : frames;
            for (int32_t i = 0; i < n; ++i) {
                L[i] += in[static_cast<size_t>(i) * 2] * monitor;
                R[i] += in[static_cast<size_t>(i) * 2 + 1] * monitor;
            }
        }
    }
    if (spec.any) colour.process(L, R, frames);
    reseed = false;
    return true; // always stereo, since lanes can differ
}

/** The audio input at the Monitor level, if it's on. */
void Bias::addInput(float *L, float *R, int32_t frames) {
    const float monitor = paramOfIndex(Monitor);
    if (monitor <= 0.0001f) return;
    const InputBus &bus = InputBus::get();
    if (!bus.live()) return;
    const float *in = bus.block();
    const int32_t n = bus.frames() < frames ? bus.frames() : frames;
    for (int32_t i = 0; i < n; ++i) {
        L[i] += in[static_cast<size_t>(i) * 2] * monitor;
        R[i] += in[static_cast<size_t>(i) * 2 + 1] * monitor;
    }
}

/**
 * Listening through: the loop at the lane's level, its mute ignored, each
 * pass from the next take. It plays at the takes' own speed, and fades in
 * and out at the loop's ends so the jump back doesn't click.
 */
void Bias::renderListen(float *L, float *R, int32_t frames) {
    const audio::Listen &ls = *listen;
    const int32_t len = ls.length();
    const auto passes = static_cast<int64_t>(ls.passes.size());
    const int32_t lane = ls.lane >= 0 && ls.lane < audio::kReelLanes ? ls.lane : 0;
    const float level = paramOfIndex(Lane1 + lane) * dsp::dbToGain(paramOfIndex(Gain));
    const int32_t half = len / 2 > 0 ? len / 2 : 1;
    const int32_t edge = audio::kListenEdgeFrames < half ? audio::kListenEdgeFrames : half;
    const float perEdge = 1.0f / static_cast<float>(edge);
    for (int32_t i = 0; i < frames; ++i) {
        const int64_t pass = listenAt / len;
        const auto in = static_cast<int32_t>(listenAt % len);
        if (in == 0) listenHint = 0;
        const audio::Reel::Region &r = ls.passes[static_cast<size_t>(pass % passes)];
        const int64_t at = ls.from + in;
        ++listenAt;
        // A pass with nothing left in it (settle() emptied it) is silence.
        if (at < 0 || at >= r.frames) continue;
        float l = 0.0f, rr = 0.0f;
        r.soundAt(at, listenHint, l, rr);
        float g = r.fadeAt(at) * level;
        const float a = (static_cast<float>(in) + 0.5f) * perEdge;
        const float b = (static_cast<float>(len - in) - 0.5f) * perEdge;
        const float e = a < b ? a : b;
        if (e < 1.0f) g *= e;
        L[i] += l * g;
        R[i] += rr * g;
    }
    listenFrames.store(listenAt, std::memory_order_relaxed);
}

} // namespace acidulous::machine
