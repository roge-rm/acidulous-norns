#include "Brazen.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

// The level the signal reaches before the drive stage, used as its nominal
// level. Measured after volume x2, peak -21.5 dB. Set it too high and the
// drive knob acts like a volume knob.
constexpr float kNominal = 0.085f;
// Output gain that puts Init at the same level as the other patches (at
// 2.0 it measured 4.4 dB quieter than Trombone on the same note).
constexpr float kHouse = 3.32f;

/**
 * Below this the tube counts as silent and the voice can be freed. It's
 * about 90 dB below full scale, low enough that cutting it doesn't tick.
 */
constexpr float kSilent = 3.0e-5f;

namespace {
constexpr float kTwoPi = 6.28318530718f;
} // namespace

Brazen::Brazen() { initParams(); }

const ParamDef *Brazen::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // 0 is a tuba's wide slow bore, 1 a trumpet's narrow bright one. It
        // sets how much of the wave the bell sends back.
        {"size", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"bell", 0.05f, 0.95f, 0.55f, Curve::Linear, 0, ""},
        {"loss", 0.97f, 1.0f, 0.999f, Curve::Linear, 0, ""},
        {"mute", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""},
        {"mutetone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"tension", 0.45f, 1.35f, 1.0f, Curve::Linear, 0, ""},
        {"lipdamp", 0.05f, 0.95f, 0.6f, Curve::Linear, 0, ""},
        {"pressure", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"breath", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        {"bite", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"brass", 0.0f, 1.0f, 0.45f, Curve::Linear, 0, ""},
        {"growl", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"growlrate", 10.0f, 120.0f, 45.0f, Curve::Exponential, 0, "Hz"},
        {"players", 1.0f, 4.0f, 1.0f, Curve::Stepped, 4, ""},
        {"spread", 0.0f, 60.0f, 8.0f, Curve::Linear, 0, "cents"},
        {"scatter", 0.0f, 120.0f, 15.0f, Curve::Linear, 0, "ms"},
        {"lock", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"drift", 0.0f, 50.0f, 6.0f, Curve::Linear, 0, "cents"},
        {"width", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"attack", 0.002f, 2.0f, 0.05f, Curve::Exponential, 0, "s"},
        {"decay", 0.005f, 4.0f, 0.3f, Curve::Exponential, 0, "s"},
        {"sustain", 0.0f, 1.0f, 0.85f, Curve::Linear, 0, ""},
        {"release", 0.005f, 4.0f, 0.25f, Curve::Exponential, 0, "s"},
        {"vibrato", 0.0f, 60.0f, 10.0f, Curve::Linear, 0, "cents"},
        {"vibratorate", 0.5f, 12.0f, 5.0f, Curve::Exponential, 0, "Hz"},
        {"vibratodelay", 0.0f, 2.0f, 0.35f, Curve::Linear, 0, "s"},
        {"cutoff", 200.0f, 18000.0f, 18000.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"filtertype", 0.0f, 11.0f, 1.0f, Curve::Stepped, 12, ""},
        {"mono", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"glide", 0.0f, 1.0f, 0.06f, Curve::Linear, 0, "s"},
        {"bendrange", 0.0f, 24.0f, 2.0f, Curve::Stepped, 25, ""},
        {"octave", -3.0f, 3.0f, 0.0f, Curve::Stepped, 7, ""},
        {"transpose", -12.0f, 12.0f, 0.0f, Curve::Stepped, 25, ""},
        {"fine", -50.0f, 50.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"mpetimbre", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void Brazen::prepare(int32_t sr) {
    sampleRate = static_cast<float>(sr);
    for (auto &v : voices) {
        v.amp.setSampleRate(sampleRate);
        v.filterL.setSampleRate(sampleRate);
        v.filterR.setSampleRate(sampleRate);
        for (auto &p : v.players) p.bore.prepare(sampleRate);
    }
    reset();
}

void Brazen::reset() {
    for (auto &v : voices) {
        v.used = v.gate = false;
        v.amp.kill();
        v.filterL.reset();
        v.filterR.reset();
        v.muteLpL = v.muteLpR = v.muteHpL = v.muteHpR = 0.0f;
        v.ring = 0.0f;
        for (auto &p : v.players) { p.bore.clear(); p.rng = Player::kSeed; }
    }
    growlPhase = 0.0f;
    rng = kRngSeed;
}

void Brazen::allNotesOff() {
    for (auto &v : voices) {
        if (!v.used) continue;
        v.gate = false;
        v.amp.release();
    }
}

Brazen::Voice *Brazen::allocate() {
    for (auto &v : voices) if (!v.used) return &v;
    // Of the released notes, steal the quietest instead of the oldest.
    // Starting a note clears the tube, and an old note isn't always the
    // quietest since a tuba rings much longer than a trumpet.
    Voice *best = nullptr;
    for (auto &v : voices) {
        if (v.gate) continue;
        if (best == nullptr || v.ring < best->ring) best = &v;
    }
    if (best != nullptr) return best;
    for (auto &v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

void Brazen::startVoice(Voice &v, uint8_t note, uint8_t velocity) {
    const float glide = targetOf(Glide);
    const bool gliding = glide > 0.001f && v.used;
    v.glideFrom = gliding ? v.freq : noteHz(static_cast<float>(note));
    v.glidePos = gliding ? 0.0f : 1.0f;
    if (!v.used) v.outGain = velocityGain(static_cast<float>(velocity) / 127.0f, targetOf(VelocityAmount));
    v.freq = v.glideFrom;
    v.used = true;
    v.gate = true;
    v.note = note;
    v.bend = 0.0f;
    v.pressure = v.timbre = -1.0f;
    v.velocity = static_cast<float>(velocity) / 127.0f;
    v.age = ++ageCounter;
    v.vibratoPhase = 0.0f;
    v.vibratoLeft = targetOf(VibratoDelay);

    const int32_t players = std::clamp(steppedTargetOf(Players), 1, kPlayers);
    const float spread = targetOf(Spread);
    const float scatter = targetOf(Scatter) * 0.001f * sampleRate;
    for (int32_t i = 0; i < kPlayers; ++i) {
        Player &p = v.players[i];
        p.rng = rng = rng * 1664525u + 1013904223u;
        const float pos = players == 1 ? 0.0f : (static_cast<float>(i) / (players - 1) * 2.0f - 1.0f);
        // Each player is off the note by their own amount.
        p.home = pos * spread * 0.5f + (nextRandom() * 2.0f - 1.0f) * spread * 0.5f;
        p.offsetCents = p.home;
        p.walk = 0.0f;
        p.delayLeft = i == 0 ? 0.0f : nextRandom() * scatter;
        p.entry = p.delayLeft > 0.0f ? 0.0f : 1.0f;
        p.breath = 1.0f - nextRandom() * 0.2f;
        p.pan = pos;
        p.pushScale = p.pushBias = 0.0f;
        if (!gliding) {
            p.bore.clear();
            // The tongue pushes on the lips while the wave builds up, so a
            // low note speaks about as fast as a high one. It needs the note
            // and a solved loop, so the frequency is set here and `tongue`
            // is called after the tune in render.
            p.bore.setFrequency(noteHz(static_cast<float>(note)));
            p.tongue = true;
        }
    }
    // Clear the mute filters so a stolen voice doesn't start with the old
    // note's state, which would click and make renders differ.
    v.muteLpL = v.muteLpR = v.muteHpL = v.muteHpR = 0.0f;
    v.ring = 0.0f;
    v.filterL.reset();
    v.filterR.reset();
    v.amp.retrigger();
}

void Brazen::noteOn(uint8_t note, uint8_t velocity) {
    Voice *v = steppedTargetOf(Mono) != 0 ? &voices[0] : allocate();
    if (v == nullptr) return;
    startVoice(*v, note, velocity);
}

void Brazen::noteOff(uint8_t note) {
    for (auto &v : voices) {
        if (v.used && v.gate && v.note == note) {
            v.gate = false;
            v.amp.release();
        }
    }
}

void Brazen::controlChange(uint8_t cc, uint8_t value) {
    if (cc == 1) modWheel = static_cast<float>(value) / 127.0f;
}
void Brazen::channelPressure(uint8_t value) { pressure = static_cast<float>(value) / 127.0f; }
void Brazen::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }
void Brazen::onBlock(int64_t, int64_t, float) {}

float Brazen::sectionSpreadCents() const { return lastSpreadCents; }

bool Brazen::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;

    const float dt = 1.0f / sampleRate;
    const int32_t players = std::clamp(steppedOf(Players), 1, kPlayers);
    const float size = paramOf(Size);
    // A wide tube reflects nearly everything, mostly lows. A narrow one
    // loses more and lets more out. The tube must be close to lossless or
    // the harmonics die out and it sounds like a flute.
    const float bellCut = paramOf(Bell) * (0.15f + size * 0.55f);
    const float reflect = 0.99f - size * 0.04f;
    const float loss = paramOf(Loss);
    const float tension = paramOf(Tension);
    const float lipDamp = paramOf(LipDamp);
    const float mouth = paramOf(Pressure);
    const float breathNoise = paramOf(Breath);
    const float bite = paramOf(Bite);
    const float brass = paramOf(Brassiness);
    const float growl = paramOf(Growl), growlRate = paramOf(GrowlRate);
    const float lock = paramOf(Lock), drift = paramOf(Drift);
    const float width = paramOf(Width);
    const float vibrato = paramOf(Vibrato), vibratoRate = paramOf(VibratoRate);
    const float glide = paramOf(Glide);
    const float bendMul = std::pow(2.0f, bend * paramOf(BendRange) / 12.0f);
    const float tune = std::pow(2.0f, (paramOf(Octave) * 12.0f + paramOf(Transpose) + paramOf(Fine) * 0.01f) / 12.0f);
    const float velAmount = paramOf(VelocityAmount);
    const int32_t mute = steppedOf(MuteKind);
    const float muteTone = paramOf(MuteTone);
    // A mute passes a band and cuts the rest. Tone slides the band up and
    // down.
    const float muteShift = std::pow(2.0f, muteTone * 2.0f - 1.0f);
    const float muteHpHz = (mute == Straight ? 700.0f : mute == Cup ? 220.0f : 1100.0f) * muteShift;
    const float muteLpHz = (mute == Straight ? 6000.0f : mute == Cup ? 2000.0f : 3600.0f) * muteShift;
    const float muteGain = mute == Straight ? 1.6f : mute == Cup ? 1.3f : 1.9f;
    // One-poles using the exact exponential coefficient. The small-angle
    // version `min(1, 2 pi f / sr)` hits 1 at high frequencies (like the
    // straight mute's 12 kHz at mutetone 1) and the filter stops filtering.
    const float muteHp = dsp::onePoleCoeff(1.0f / (kTwoPi * muteHpHz), sampleRate);
    const float muteLp = dsp::onePoleCoeff(1.0f / (kTwoPi * muteLpHz), sampleRate);
    const float drive = paramOf(Drive), volume = paramOf(Volume);
    const float panKnob = paramOf(Pan);
    const float panL = std::cos((panKnob + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((panKnob + 1.0f) * 0.25f * 3.14159265f);

    // 5 ms, so it follows the tube's decay and not its waveform.
    const float ringCoeff = dsp::onePoleCoeff(0.005f, sampleRate);
    // Matches the amp envelope's attack, so a late player comes in the same
    // way the first one did.
    const float entryCoeff = dsp::onePoleCoeff(paramOf(Attack) * 0.4f, sampleRate);

    const float growlStart = growlPhase;
    const float growlStep = growlRate * dt;
    growlPhase += growlStep * static_cast<float>(frames);
    while (growlPhase >= 1.0f) growlPhase -= 1.0f;

    for (auto &v : voices) {
        if (!v.used) continue;
        v.amp.set(0.0f, paramOf(Attack), paramOf(Decay), paramOf(Sustain), paramOf(Release), false);
        v.filterL.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType), dsp::MultiFilter::Clean, 0.0f);
        v.filterR.set(paramOf(Cutoff), paramOf(Resonance), steppedOf(FilterType), dsp::MultiFilter::Clean, 0.0f);

        if (v.glidePos < 1.0f) {
            v.glidePos = std::min(1.0f, v.glidePos + (glide > 0.001f ? dt * frames / glide : 1.0f));
            v.freq = v.glideFrom + (noteHz(static_cast<float>(v.note)) - v.glideFrom) * v.glidePos;
        } else {
            v.freq = noteHz(static_cast<float>(v.note));
        }

        // --- the section tunes to itself -------------------------------------
        // Each player has their own idea of the note, drifts off it slowly,
        // and is pulled toward the section's average. At lock 0 they ignore
        // each other. At 1 they quickly settle on the average and sound like
        // one horn.
        float mean = 0.0f;
        for (int32_t i = 0; i < players; ++i) mean += v.players[i].offsetCents;
        mean /= static_cast<float>(players);
        const float rate = std::min(0.5f, static_cast<float>(frames) / (0.08f * sampleRate));
        float spreadNow = 0.0f;
        for (int32_t i = 0; i < players; ++i) {
            Player &p = v.players[i];
            p.rng = p.rng * 1664525u + 1013904223u;
            const float r = static_cast<float>(p.rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
            p.walk = std::clamp(p.walk + r * drift * 0.02f * rate * 8.0f, -drift, drift);
            const float ownPull = (p.home + p.walk) - p.offsetCents;
            const float sectionPull = (mean - p.offsetCents) * lock * 8.0f;
            p.offsetCents += (ownPull + sectionPull) * rate;
            spreadNow = std::max(spreadNow, std::fabs(p.offsetCents - mean));
        }
        lastSpreadCents = spreadNow * 2.0f;

        const float vibratoDepth = v.vibratoLeft > 0.0f ? 0.0f : vibrato * (0.35f + modWheel * 0.65f);
        if (v.vibratoLeft > 0.0f) v.vibratoLeft -= dt * frames;
        v.vibratoPhase += vibratoRate * dt * frames;
        while (v.vibratoPhase >= 1.0f) v.vibratoPhase -= 1.0f;
        const float vib = std::sin(v.vibratoPhase * kTwoPi) * vibratoDepth;

        // Velocity sets how hard the player blows (harder is brighter) and
        // also the level, with the same law as every machine. Blowing alone
        // can't make a note quiet, since below a point the lips don't sound.
        const float vel = 1.0f - 0.6f * velAmount * (1.0f - v.velocity);
        // Ramped across the block, since a slurred note changes it mid-sound.
        const float gainTo = velocityGain(v.velocity, velAmount);
        const float gainStep = (gainTo - v.outGain) / static_cast<float>(frames);
        const float growlNow = growl * (0.5f + 0.5f * std::sin(growlPhase * kTwoPi));
        const float env0 = v.amp.value();

        // Set up the horn once a block. tune() is expensive and the note
        // doesn't change audibly within a block.
        for (int32_t pi = 0; pi < players; ++pi) {
            Player &p = v.players[pi];
            const float hz = v.freq * tune * bendMul * noteBendMul(v) *
                             std::pow(2.0f, (p.offsetCents + vib) / 1200.0f);
            const float prs = v.pressure >= 0.0f ? v.pressure : pressure;
            const float push = mouth * env0 * vel * p.breath * (1.0f - growlNow * 0.5f) + prs * 0.3f;
            p.bore.setFrequency(hz);
            // Lips tighten as the player blows harder, so the note arrives
            // before the tone does. The growl pushes on the lips and the air
            // at once so it buzzes instead of just wobbling. Slide (MPE
            // timbre) also tightens the lips, like embouchure.
            const float slide = v.timbre >= 0.0f ? v.timbre : 0.0f;
            p.bore.setLips(tension * (0.94f + env0 * bite * 0.12f) * (1.0f + growlNow * 0.06f) *
                               (1.0f + slide * paramOf(MpeTimbre) * 0.35f),
                           lipDamp);
            p.bore.setLipGain(0.7f + bite * 0.6f);
            p.bore.setBell(reflect, bellCut);
            // Brassiness also closes the lips, since a narrow pulse is part of
            // what makes a loud horn bright.
            p.bore.setRest(0.35f);
            p.bore.setBite(bite * 0.9f + brass * 0.5f);
            // The steepening depends on how loud the wave is, so it follows
            // the pressure and not the envelope.
            p.bore.setBrass(brass * std::min(1.0f, push * 1.4f));
            p.bore.setLoss(loss);
            p.bore.setPressure(push);
            p.bore.tune();
            // Where this player sits. Worked out once a block to save the
            // trig calls per sample.
            const float panNow = std::clamp(p.pan * width, -1.0f, 1.0f);
            const float panAngle = (panNow + 1.0f) * 0.25f * 3.14159265f;
            p.panL = std::cos(panAngle);
            p.panR = std::sin(panAngle);
            // The parts of `push` that don't change within the block, so the
            // sample loop can apply the envelope per sample. See step() below.
            p.pushScale = mouth * vel * p.breath * (1.0f - growlNow * 0.5f);
            p.pushBias = prs * 0.3f;
            if (p.tongue) {
                // After the loop is solved for this note, since `tongue` uses
                // the solved gain.
                p.bore.tune();
                p.bore.tongue();
                p.tongue = false;
            }
        }

        for (int32_t i = 0; i < frames; ++i) {
            const float env = v.amp.next();
            // Free the voice once the player has stopped and the tube is
            // silent. See Voice::ring.
            if (env <= 0.0000005f && !v.gate && v.ring < kSilent) { v.used = false; break; }
            // The growl is a tremolo on the output, so it's worked out per
            // sample. Per block it would give audible steps.
            const float growlAmNow =
                growl > 0.0001f
                    ? 1.0f - growl * (0.5f + 0.5f * std::sin((growlStart + growlStep * static_cast<float>(i)) * kTwoPi)) * 0.35f
                    : 1.0f;
            float l = 0.0f, r = 0.0f;
            for (int32_t pi = 0; pi < players; ++pi) {
                Player &p = v.players[pi];
                if (p.delayLeft > 0.0f) { p.delayLeft -= 1.0f; continue; }
                rng = rng * 1664525u + 1013904223u;
                const float hiss = (static_cast<float>(rng >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * breathNoise * 0.25f;
                // Mouth pressure follows the envelope per sample, so the
                // attack is a smooth ramp and not steps that would buzz. The
                // block value still feeds setPressure, setBrass and the solve.
                // A late player also gets its own attack, see Player::entry.
                p.entry += (1.0f - p.entry) * entryCoeff;
                const float pushNow = (p.pushScale * env + p.pushBias) * p.entry;
                const float s = p.bore.step(pushNow, hiss * pushNow) * growlAmNow;
                l += s * p.panL;
                r += s * p.panR;
            }
            v.ring += (std::fabs(l) + std::fabs(r) - v.ring) * ringCoeff;
            // The mute passes a band and cuts the rest. Straight is bright
            // and thin, cup is dark and close, harmon is nasal.
            if (mute != Open) {
                v.muteLpL += (l - v.muteLpL) * muteHp;
                v.muteLpR += (r - v.muteLpR) * muteHp;
                l -= v.muteLpL;
                r -= v.muteLpR;
                v.muteHpL += (l - v.muteHpL) * muteLp;
                v.muteHpR += (r - v.muteHpR) * muteLp;
                l = v.muteHpL * muteGain;
                r = v.muteHpR * muteGain;
            }
            v.outGain += gainStep;
            L[i] += v.filterL.process(l) * v.outGain;
            R[i] += v.filterR.process(r) * v.outGain;
        }
        v.outGain = gainTo;
    }

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i] * volume * kHouse, r = R[i] * volume * kHouse;
        if (drive > 0.0001f) {
            const float k = 1.0f + drive * 8.0f;
            // Normalised on the nominal level so the drive knob changes the
            // character and not the level.
            const float norm = kNominal / dsp::fastTanh(kNominal * k);
            l = dsp::fastTanh(l * k) * norm;
            r = dsp::fastTanh(r * k) * norm;
        }
        L[i] = l * panL * 1.4142f;
        R[i] = r * panR * 1.4142f;
    }
    return true;
}

void Brazen::noteBend(uint8_t note, float semitones) {
    if (Voice *v = voiceForNote(voices, note)) v->bend = semitones;
}

void Brazen::notePressure(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->pressure = static_cast<float>(value) / 127.0f;
}

void Brazen::noteTimbre(uint8_t note, uint8_t value) {
    if (Voice *v = voiceForNote(voices, note)) v->timbre = static_cast<float>(value) / 127.0f;
}

} // namespace acidulous::machine
