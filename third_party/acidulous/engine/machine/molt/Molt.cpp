#include "Molt.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

using dsp::clampf;
using dsp::fastTanh;
using dsp::kTwoPi;
using dsp::mtof;

namespace {

/**
 * A Hann window table, looked up instead of computed per sample since it's
 * read several times a frame per voice. 1024 points is fine enough.
 */
constexpr int32_t kWindowSize = 1024;

const float *hannTable() {
    static float table[kWindowSize];
    static const bool built = [] {
        for (int32_t i = 0; i < kWindowSize; ++i) {
            table[i] = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) /
                                              static_cast<float>(kWindowSize - 1));
        }
        return true;
    }();
    (void)built;
    return table;
}

} // namespace

Molt::Molt() { initParams(); }

const ParamDef *Molt::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // The take is a file mounted like other machines' material. See the
        // recording window.
        {"start", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"loop", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},

        // How hard the take is pulled onto the written note. At 0 it keeps
        // the sung line and is just transposed, at 1 it lands exactly on the
        // note.
        {"tune", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        // How long it takes to get there. 0 is hard tune, 40 ms sounds like a
        // singer correcting themselves.
        {"rate", 0.0f, 400.0f, 40.0f, Curve::Linear, 0, "ms"},
        // Every mark takes the written pitch, voiced or not.
        {"robot", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},

        // Formant shift, independent of the note.
        {"formant", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "st"},
        {"mega", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},

        {"cutoff", 40.0f, 18000.0f, 18000.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"filtertype", 0.0f, static_cast<float>(dsp::MultiFilter::TypeCount - 1), 1.0f,
         Curve::Stepped, dsp::MultiFilter::TypeCount, ""},

        {"ampattack", 0.001f, 2.0f, 0.005f, Curve::Exponential, 0, "s"},
        {"ampdecay", 0.001f, 4.0f, 0.2f, Curve::Exponential, 0, "s"},
        {"ampsustain", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"amprelease", 0.001f, 4.0f, 0.08f, Curve::Exponential, 0, "s"},

        {"glide", 0.0f, 2.0f, 0.0f, Curve::Linear, 0, "s"},
        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Molt::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.acc.assign(kAccum, 0.0f);
        v.amp.setSampleRate(sampleRate);
    }
    filter.setSampleRate(sampleRate);
    horn.setSampleRate(sampleRate);
    hannTable();
    reset();
}

void Molt::reset() {
    for (auto &v : voices) {
        v.used = false;
        v.gate = false;
        v.note = 0;
        v.velocity = 1.0f;
        v.bend = 0.0f;
        v.pressure = 0.0f;
        v.logPitch = 0.0f;
        v.primed = false;
        v.untilGrain = 0.0f;
        v.accHead = 0;
        std::fill(v.acc.begin(), v.acc.end(), 0.0f);
        v.amp.reset();
    }
    filter.reset();
    horn.reset();
    head = 0.0;
    running = false;
    channelBend = 0.0f;
}

void Molt::noteOn(uint8_t note, uint8_t velocity) {
    Voice *v = nullptr;
    for (auto &c : voices) {
        if (!c.used) { v = &c; break; }
    }
    if (v == nullptr) {
        // Steal the quietest voice.
        v = &voices[0];
        for (auto &c : voices) {
            if (c.amp.value() < v->amp.value()) v = &c;
        }
    }
    v->used = true;
    v->gate = true;
    v->note = note;
    v->velocity = velocityGain(static_cast<float>(velocity) / 127.0f, targetOf(VelocityAmount));
    v->bend = channelBend;
    v->primed = false;
    v->untilGrain = 0.0f;
    v->amp.retrigger();

    // The first note starts the phrase. Later notes join where it's got to
    // instead of restarting it, so chords sing together.
    if (!running) {
        const audio::Utterance *u = source;
        if (u != nullptr && u->usable()) {
            head = static_cast<double>(targetOf(Start)) * static_cast<double>(u->frames);
            running = true;
        }
    }
}

void Molt::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
        }
    }
}

void Molt::allNotesOff() {
    for (auto &v : voices) {
        v.gate = false;
        v.amp.release();
    }
    // A transport stop rewinds the phrase.
    running = false;
    head = 0.0;
}

void Molt::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) {
        // The mod wheel sets the formant.
        const float v = static_cast<float>(value) / 127.0f;
        params_.set(Formant, 0.5f + v * 0.5f);
    }
}

void Molt::pitchBend(int16_t value14) {
    channelBend = static_cast<float>(value14) / 8192.0f * paramOf(BendRange);
    for (auto &v : voices) {
        if (v.used) v.bend = channelBend;
    }
}

void Molt::noteBend(uint8_t note, float semitones) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) v.bend = semitones;
    }
}

void Molt::notePressure(uint8_t note, uint8_t value) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

void *Molt::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    // Voices hold positions in the old take, so they stop with it.
    for (auto &v : voices) {
        v.used = false;
        v.gate = false;
        v.amp.kill();
        std::fill(v.acc.begin(), v.acc.end(), 0.0f);
    }
    running = false;
    head = 0.0;
    void *old = const_cast<audio::Utterance *>(source);
    source = static_cast<const audio::Utterance *>(object);
    return old;
}

float Molt::layGrain(Voice &v, float formantRatio, float tune, float rateSec, bool robot) {
    const audio::Utterance *u = source;
    const int32_t idx = u->epochAt(static_cast<float>(head));
    if (idx < 0) return sampleRate * 0.005f;
    const audio::Epoch &e = u->epochs[static_cast<size_t>(idx)];
    const float srcPeriod = clampf(e.period, 2.0f, 2000.0f);

    // --- the pitch this grain is pulled to ---------------------------------
    float targetPeriod;
    if (e.voiced || robot) {
        const float pitchHz = noteHz(static_cast<float>(v.note) +
                                  static_cast<float>(steppedOf(Transpose)) +
                                  12.0f * static_cast<float>(steppedOf(Octave)) + v.bend);
        const float logNote = std::log2(pitchHz);
        float want = logNote;
        if (!robot && u->rootHz > 0.0f && e.voiced) {
            // Keep part of this moment's distance from the take's root. At
            // tune 0 the sung line's shape is kept, at 1 it's flattened onto
            // the note.
            const float logSrc = std::log2(sampleRate / srcPeriod);
            want = logNote + (1.0f - tune) * (logSrc - std::log2(u->rootHz));
        }
        if (!v.primed) {
            v.logPitch = want;
            v.primed = true;
        } else if (rateSec <= 0.0005f) {
            v.logPitch = want;
        } else {
            // One pole per grain rather than per sample, since there's only
            // something new to correct once per period.
            const float coeff = 1.0f - std::exp(-srcPeriod / (rateSec * sampleRate));
            v.logPitch += (want - v.logPitch) * coeff;
        }
        targetPeriod = sampleRate / std::exp2(v.logPitch);
    } else {
        // Unvoiced sounds have no pitch to move, so they're copied at their
        // own rate.
        targetPeriod = srcPeriod;
    }
    targetPeriod = clampf(targetPeriod, 4.0f, 2000.0f);

    // --- and how it's read -------------------------------------------------
    // Half a grain is exactly the source's period, so a grain holds one
    // glottal pulse. Sizing it to the target period would let a formant
    // shifted grain hold two pulses and sound an octave down.
    const float half = srcPeriod;
    const int32_t n = static_cast<int32_t>(2.0f * half / formantRatio);
    if (n < 2 || n >= kAccum) return targetPeriod;

    // Overlap gain correction. Hann windows sum to 1 at 50% overlap, more
    // when laid tighter and less when wider. It's capped at 1 because when
    // grains don't overlap at all (the target is far below the source, which
    // happens when the pitch tracker gets the octave wrong) no correction is
    // needed, and boosting them makes loud isolated bursts.
    //
    // The head advances one frame per sample whatever the note is, so gaps
    // only fill themselves when the source pitch is low.
    const float gain = clampf(2.0f * targetPeriod / static_cast<float>(n), 0.0f, 1.0f);
    const float *window = hannTable();
    const float wStep = static_cast<float>(kWindowSize - 1) / static_cast<float>(n - 1);
    // A grain near either end of the take is slid inside it rather than
    // having samples skipped, so the whole window is always used. Otherwise
    // the grain would start mid-window and click.
    const int32_t frames = u->frames;
    const float span = static_cast<float>(n - 1) * formantRatio;
    const float from = clampf(static_cast<float>(e.at) - half, 0.0f,
                              std::max(0.0f, static_cast<float>(frames - 2) - span));

    for (int32_t k = 0; k < n; ++k) {
        const float sp = from + static_cast<float>(k) * formantRatio;
        const int32_t i0 = static_cast<int32_t>(sp);
        if (sp < 0.0f || i0 + 1 >= frames) continue;
        const float frac = sp - static_cast<float>(i0);
        const float s = u->mono[static_cast<size_t>(i0)] * (1.0f - frac) +
                        u->mono[static_cast<size_t>(i0 + 1)] * frac;
        const float w = window[static_cast<int32_t>(static_cast<float>(k) * wStep)];
        v.acc[static_cast<size_t>((v.accHead + k) & (kAccum - 1))] += s * w * gain;
    }
    return targetPeriod;
}

bool Molt::render(float *L, float *R, int32_t frames) {
    params_.tick();
    // The rack's buffer still holds its last block; the voices add into a clear one.
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    // --- the block's settings ----------------------------------------------
    const audio::Utterance *u = source;
    const bool haveTake = u != nullptr && u->usable();
    const float formantRatio = std::exp2(paramOf(Formant) / 12.0f);
    const float tune = clampf(paramOf(Tune), 0.0f, 1.0f);
    const float rateSec = paramOf(Rate) * 0.001f;
    const bool robot = rawOf(Robot) >= 0.5f;
    const bool loop = rawOf(Loop) >= 0.5f;
    const float startFrame = haveTake ? paramOf(Start) * static_cast<float>(u->frames) : 0.0f;

    for (auto &v : voices) {
        v.amp.set(0.0f, paramOf(AmpAttack), paramOf(AmpDecay), paramOf(AmpSustain),
                  paramOf(AmpRelease), false);
    }
    filter.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType), dsp::MultiFilter::Clean,
               0.0f);
    const float mega = clampf(paramOf(Mega), 0.0f, 1.0f);
    // The megaphone is a band pass with clipping drive. The band narrows as
    // the amount rises.
    horn.set(1500.0f, 0.25f + mega * 0.45f, dsp::MultiFilter::BP12, dsp::MultiFilter::Clip, mega);

    const float drive = paramOf(Drive);
    const float volume = paramOf(Volume);
    const float panKnob = paramOf(Pan);
    const float panL = std::cos((panKnob + 1.0f) * 0.25f * dsp::kPi);
    const float panR = std::sin((panKnob + 1.0f) * 0.25f * dsp::kPi);

    for (int32_t i = 0; i < frames; ++i) {
        if (running && haveTake) {
            head += 1.0;
            if (head >= static_cast<double>(u->frames)) {
                if (loop) {
                    head = static_cast<double>(startFrame);
                } else {
                    running = false;
                }
            }
        }

        float mix = 0.0f;
        for (auto &v : voices) {
            if (!v.used) continue;
            if (running && haveTake) {
                while (v.untilGrain <= 0.0f) {
                    v.untilGrain += layGrain(v, formantRatio, tune, rateSec, robot);
                }
                v.untilGrain -= 1.0f;
            }
            const float s = v.acc[static_cast<size_t>(v.accHead)];
            v.acc[static_cast<size_t>(v.accHead)] = 0.0f;
            v.accHead = (v.accHead + 1) & (kAccum - 1);
            const float env = v.amp.next();
            if (!v.amp.active()) {
                v.used = false;
                continue;
            }
            // Pressure raises the level.
            mix += s * env * v.velocity * (1.0f + v.pressure * 0.5f);
        }

        if (mega > 0.0001f) {
            const float band = horn.process(mix);
            mix = mix * (1.0f - mega) + band * mega * 1.8f;
        }
        mix = filter.process(mix);
        if (drive > 0.0001f) mix = fastTanh(mix * (1.0f + drive * 6.0f)) / (1.0f + drive * 1.5f);
        mix *= volume;
        L[i] += mix * panL;
        R[i] += mix * panR;
    }
    return true;
}

} // namespace acidulous::machine
