#include "Genesis.h"
#include <engine/machine/Voices.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>

namespace acidulous::machine {

namespace {
// The input level each saturator passes at unity gain, whatever its drive,
// so drive changes the tone and not the level. It's set at a typical working
// level, not the peak. Quieter signals get lifted, louder ones bend. Same as
// Resonance's per-pad drive.
constexpr float kKickNominal = 0.5f;
constexpr float kBusNominal = 0.5f;

// The house level. It sets where a kit sits on the volume knob and keeps the
// output below full scale at any drive setting (with no drive, Init would
// otherwise peak at +3.9 dBFS).
constexpr float kHouse = 0.57f; // -4.9 dB, set so Init sits on the house line

// Per-voice level trims, like Hexbeat's. Without them the voices are up to
// 13.5 dB apart with the rim loudest. They were tuned by measuring through
// the bus until each voice sat within 0.4 dB of a classic kit balance: kick
// on top, snare just under, cymbals about 5 dB back.
constexpr float kLevelTrim[] = {
    1.000f, // Kick      reference
    0.680f, // Snare     -0.2 dB
    0.854f, // Clap      +1.4
    0.227f, // Rim       -9.1
    0.579f, // Tom       -3.6
    0.461f, // Cowbell   -3.5
    0.962f, // Hat       +5.3
    0.949f, // Crash     +1.8
    0.746f, // Ride      +1.6
};
enum Trim { TKick, TSnare, TClap, TRim, TTom, TBell, THat, TCrash, TRide };

constexpr float kTwoPi = 6.28318530718f;
/** Frequency ratios of the six metal squares, inharmonic on purpose. */
constexpr float kMetalRatios[6] = {1.0f, 1.4471f, 1.6170f, 1.9265f, 2.5028f, 2.6637f};
} // namespace

Genesis::Genesis() { initParams(); }

namespace {
const ParamDef kBase[Genesis::MacroBase] = {
        {"kicktune", 30.0f, 90.0f, 48.0f, Curve::Exponential, 0, "Hz"},
        {"kickdecay", 0.05f, 3.0f, 0.85f, Curve::Exponential, 0, "s"},
        {"kickpunch", 0.0f, 1.0f, 0.55f, Curve::Linear, 0, ""},
        {"kicksweep", 0.005f, 0.2f, 0.045f, Curve::Exponential, 0, "s"},
        {"kickclick", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"kickdrive", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"kicklevel", 0.0f, 1.5f, 1.0f, Curve::Linear, 0, ""},
        {"snaretune", 120.0f, 400.0f, 185.0f, Curve::Exponential, 0, "Hz"},
        {"snaredecay", 0.05f, 1.2f, 0.22f, Curve::Exponential, 0, "s"},
        {"snaresnap", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"snaretone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"snarelevel", 0.0f, 1.5f, 0.9f, Curve::Linear, 0, ""},
        {"clapspread", 0.005f, 0.05f, 0.018f, Curve::Exponential, 0, "s"},
        {"clapdecay", 0.05f, 1.5f, 0.35f, Curve::Exponential, 0, "s"},
        {"claptone", 400.0f, 3000.0f, 1100.0f, Curve::Exponential, 0, "Hz"},
        {"claplevel", 0.0f, 1.5f, 0.8f, Curve::Linear, 0, ""},
        {"rimtune", 800.0f, 2500.0f, 1700.0f, Curve::Exponential, 0, "Hz"},
        {"rimdecay", 0.01f, 0.3f, 0.05f, Curve::Exponential, 0, "s"},
        {"rimlevel", 0.0f, 1.5f, 0.7f, Curve::Linear, 0, ""},
        {"tomlotune", 60.0f, 200.0f, 90.0f, Curve::Exponential, 0, "Hz"},
        {"tommidtune", 80.0f, 300.0f, 130.0f, Curve::Exponential, 0, "Hz"},
        {"tomhitune", 100.0f, 400.0f, 185.0f, Curve::Exponential, 0, "Hz"},
        {"tomdecay", 0.05f, 2.0f, 0.5f, Curve::Exponential, 0, "s"},
        {"tombend", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"tomlevel", 0.0f, 1.5f, 0.85f, Curve::Linear, 0, ""},
        {"hattune", 200.0f, 1200.0f, 540.0f, Curve::Exponential, 0, "Hz"},
        {"hatclosed", 0.01f, 0.4f, 0.06f, Curve::Exponential, 0, "s"},
        {"hatopen", 0.05f, 2.0f, 0.5f, Curve::Exponential, 0, "s"},
        {"hattone", 2000.0f, 12000.0f, 7000.0f, Curve::Exponential, 0, "Hz"},
        {"hatlevel", 0.0f, 1.5f, 0.7f, Curve::Linear, 0, ""},
        {"crashdecay", 0.2f, 6.0f, 2.2f, Curve::Exponential, 0, "s"},
        {"crashtone", 1500.0f, 9000.0f, 4000.0f, Curve::Exponential, 0, "Hz"},
        {"crashlevel", 0.0f, 1.5f, 0.6f, Curve::Linear, 0, ""},
        {"ridedecay", 0.2f, 6.0f, 1.6f, Curve::Exponential, 0, "s"},
        {"ridetone", 2000.0f, 12000.0f, 5500.0f, Curve::Exponential, 0, "Hz"},
        {"ridebell", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"ridelevel", 0.0f, 1.5f, 0.6f, Curve::Linear, 0, ""},
        {"belltune", 400.0f, 1200.0f, 800.0f, Curve::Exponential, 0, "Hz"},
        {"belldecay", 0.05f, 1.0f, 0.35f, Curve::Exponential, 0, "s"},
        {"belllevel", 0.0f, 1.5f, 0.6f, Curve::Linear, 0, ""},
        {"drift", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"accent", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"comp", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"compattack", 0.0005f, 0.1f, 0.008f, Curve::Exponential, 0, "s"},
        {"comprelease", 0.02f, 1.0f, 0.18f, Curve::Exponential, 0, "s"},
        {"duck", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.5f, 0.9f, Curve::Linear, 0, ""},
        {"pan", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
};

// The base table plus the macros' knobs and routes.
struct Table {
    ParamDef defs[Genesis::Count];
    char names[Genesis::kMacros * Genesis::MacroParams][12];
    Table() {
        for (int32_t i = 0; i < Genesis::MacroBase; ++i) defs[i] = kBase[i];
        for (int k = 0; k < Genesis::kMacros; ++k) {
            const int32_t b = Genesis::MacroBase + k * Genesis::MacroParams;
            char *n = names[k * Genesis::MacroParams];
            std::snprintf(n, 12, "mac%d", k + 1);
            defs[b] = {n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            for (int r = 0; r < 4; ++r) {
                char *d = names[k * Genesis::MacroParams + 1 + r];
                std::snprintf(d, 12, "mac%d_d%d", k + 1, r + 1);
                defs[b + 1 + r] = {d, 0.0f, Genesis::DestCount - 1.0f, 0.0f, Curve::Stepped, Genesis::DestCount, ""};
                char *a = names[k * Genesis::MacroParams + 5 + r];
                std::snprintf(a, 12, "mac%d_a%d", k + 1, r + 1);
                defs[b + 5 + r] = {a, -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
            }
        }
    }
};
const Table &table() {
    static const Table t;
    return t;
}

// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Genesis::Count];
    DestMap() {
        using G = Genesis;
        for (auto &d : of) d = -1;
        of[G::KickTune] = G::DstKickTune; of[G::KickDecay] = G::DstKickDecay;
        of[G::KickPunch] = G::DstKickPunch; of[G::KickSweep] = G::DstKickSweep;
        of[G::KickClick] = G::DstKickClick; of[G::KickDrive] = G::DstKickDrive;
        of[G::KickLevel] = G::DstKickLevel;
        of[G::SnareTune] = G::DstSnareTune; of[G::SnareDecay] = G::DstSnareDecay;
        of[G::SnareSnap] = G::DstSnareSnap; of[G::SnareTone] = G::DstSnareTone;
        of[G::SnareLevel] = G::DstSnareLevel;
        of[G::ClapSpread] = G::DstClapSpread; of[G::ClapDecay] = G::DstClapDecay;
        of[G::ClapTone] = G::DstClapTone; of[G::ClapLevel] = G::DstClapLevel;
        of[G::RimTune] = G::DstRimTune; of[G::RimDecay] = G::DstRimDecay; of[G::RimLevel] = G::DstRimLevel;
        of[G::TomLoTune] = of[G::TomMidTune] = of[G::TomHiTune] = G::DstTomTune;
        of[G::TomDecay] = G::DstTomDecay; of[G::TomBend] = G::DstTomBend; of[G::TomLevel] = G::DstTomLevel;
        of[G::HatTune] = G::DstHatTune;
        of[G::HatClosedDecay] = of[G::HatOpenDecay] = G::DstHatDecay;
        of[G::HatTone] = G::DstHatTone; of[G::HatLevel] = G::DstHatLevel;
        of[G::CrashDecay] = G::DstCrashDecay; of[G::CrashTone] = G::DstCrashTone;
        of[G::CrashLevel] = G::DstCrashLevel;
        of[G::RideDecay] = G::DstRideDecay; of[G::RideTone] = G::DstRideTone;
        of[G::RideBell] = G::DstRideBell; of[G::RideLevel] = G::DstRideLevel;
        of[G::BellTune] = G::DstBellTune; of[G::BellDecay] = G::DstBellDecay;
        of[G::BellLevel] = G::DstBellLevel;
        of[G::Drift] = G::DstDrift; of[G::Accent] = G::DstAccent;
        of[G::CompAmount] = G::DstComp; of[G::Duck] = G::DstDuck;
        of[G::Drive] = G::DstDrive; of[G::Volume] = G::DstVolume;
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

const ParamDef *Genesis::paramDefs(int32_t &count) const { count = Count; return table().defs; }

void Genesis::updateMacros(bool goal) {
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, goal);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(read(b + 1 + r, goal) + 0.5f);
            if (dest > DstOff && dest < DestCount) mac_[dest] += knob * read(b + 5 + r, goal);
        }
    }
    // Two octaves either way.
    mac_[DstPitch] = std::clamp(mac_[DstPitch], -1.0f, 1.0f);
    pitchMul_ = mac_[DstPitch] == 0.0f ? 1.0f : std::exp2(mac_[DstPitch] * 2.0f);
}

float Genesis::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : mac_[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Genesis::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &f : bp) f.setSampleRate(sr);
    for (auto &f : hp) f.setSampleRate(sr);
    reset();
}

void Genesis::reset() {
    for (int32_t v = 0; v < VoiceCount; ++v) {
        // Reset whole structs so every field (coeff, active, phase) clears.
        amp[v] = Env();
        pitch[v] = Env();
        aux[v] = Env();
        osc[v] = Osc();
        osc2[v] = Osc();
        gain[v] = 0.0f;
        tuneOf[v] = 0.0f;
        bp[v].reset();
        hp[v].reset();
    }
    for (auto &o : metal) o = Osc();
    compEnv = 0.0f;
    duckEnv = 0.0f;
    clapPhase = 0.0f;
    clapLeft = 0;
    rng = kRngSeed;
}

void Genesis::allNotesOff() { reset(); }

float Genesis::metallic(float tune) {
    float sum = 0.0f;
    for (int i = 0; i < 6; ++i) sum += metal[i].step(tune * kMetalRatios[i], sr) < 0.5f ? 1.0f : -1.0f;
    return sum * (1.0f / 6.0f);
}

void Genesis::trigger(int32_t voice, float velocity) {
    const float accent = val(Accent, true);
    const float drift = val(Drift, true) * 0.06f; // a few per cent
    const float level = velocityGain(velocity, accent) * wobble(drift * 1.5f);
    gain[voice] = level;

    switch (voice) {
    case Kick:
        tuneOf[voice] = val(KickTune, true) * pitchMul_ * wobble(drift);
        amp[voice].fire(sr, val(KickDecay, true) * wobble(drift), level);
        pitch[voice].fireTau(sr, val(KickSweep, true) * wobble(drift), 1.0f);
        aux[voice].fireTau(sr, 0.004f, val(KickClick, true) * level);
        break;
    case Snare:
        tuneOf[voice] = val(SnareTune, true) * pitchMul_ * wobble(drift);
        amp[voice].fire(sr, val(SnareDecay, true) * wobble(drift), level);
        aux[voice].fire(sr, val(SnareDecay, true) * (0.6f + val(SnareSnap, true)) * wobble(drift), level);
        break;
    case Clap:
        amp[voice].fire(sr, val(ClapDecay, true) * wobble(drift), level);
        clapLeft = 3; // three more bursts after this one
        clapPhase = val(ClapSpread, true) * sr;
        break;
    case Rim:
        tuneOf[voice] = val(RimTune, true) * pitchMul_ * wobble(drift);
        amp[voice].fire(sr, val(RimDecay, true) * wobble(drift), level);
        break;
    case TomLo:
    case TomMid:
    case TomHi: {
        const float tune = voice == TomLo ? val(TomLoTune, true)
                                          : (voice == TomMid ? val(TomMidTune, true) : val(TomHiTune, true));
        tuneOf[voice] = tune * pitchMul_ * wobble(drift);
        amp[voice].fire(sr, val(TomDecay, true) * wobble(drift), level);
        pitch[voice].fireTau(sr, 0.06f, val(TomBend, true));
        break;
    }
    case HatClosed:
        amp[voice].fire(sr, val(HatClosedDecay, true) * wobble(drift), level);
        amp[HatOpen].level = 0.0f; // a closed hat chokes the open one
        amp[HatOpen].active = false;
        break;
    case HatOpen:
        amp[voice].fire(sr, val(HatOpenDecay, true) * wobble(drift), level);
        break;
    case Crash:
        amp[voice].fire(sr, val(CrashDecay, true) * wobble(drift), level);
        break;
    case Ride:
        amp[voice].fire(sr, val(RideDecay, true) * wobble(drift), level);
        aux[voice].fire(sr, val(RideDecay, true) * 0.35f, level * val(RideBell, true));
        break;
    case Cowbell:
        tuneOf[voice] = val(BellTune, true) * pitchMul_ * wobble(drift);
        amp[voice].fire(sr, val(BellDecay, true) * wobble(drift), level);
        break;
    default:
        break;
    }
}

void Genesis::noteOn(uint8_t note, uint8_t velocity) {
    const int32_t voice = note - kBaseNote;
    if (voice < 0 || voice >= VoiceCount) return;
    updateMacros(true); // a hit reads where the knobs are going, so renders repeat
    trigger(voice, static_cast<float>(velocity) / 127.0f);
}

bool Genesis::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();

    const float hatTone = val(HatTone);
    const float crashTone = val(CrashTone);
    const float rideTone = val(RideTone);
    const float clapTone = val(ClapTone);
    const float hatTune = val(HatTune) * pitchMul_;
    const float comp = val(CompAmount);
    const float compAttack = 1.0f - std::exp(-1.0f / (val(CompAttack) * sr));
    const float compRelease = 1.0f - std::exp(-1.0f / (val(CompRelease) * sr));
    const float duck = val(Duck);
    const float drive = val(Drive), volume = val(Volume);
    const float pan = val(Pan);
    const float panL = std::cos((pan + 1.0f) * 0.25f * 3.14159265f);
    const float panR = std::sin((pan + 1.0f) * 0.25f * 3.14159265f);

    bp[HatClosed].set(hatTone, 0.3f);
    bp[HatOpen].set(hatTone, 0.3f);
    bp[Crash].set(crashTone, 0.2f);
    bp[Ride].set(rideTone, 0.25f);
    bp[Clap].set(clapTone, 0.5f);
    bp[Snare].set(1600.0f, 0.35f);
    bp[Rim].set(val(RimTune) * pitchMul_, 0.8f);
    bp[Cowbell].set(val(BellTune) * pitchMul_ * 1.5f, 0.6f);

    const float vKickPunch = val(KickPunch);
    const float vKickDrive = val(KickDrive);
    const float vKickLevel = val(KickLevel);
    const float vSnareTone = val(SnareTone);
    const float vSnareSnap = val(SnareSnap);
    const float vSnareLevel = val(SnareLevel);
    const float vClapSpread = val(ClapSpread);
    const float vClapLevel = val(ClapLevel);
    const float vRimLevel = val(RimLevel);
    const float vTomLevel = val(TomLevel);
    const float vBellLevel = val(BellLevel);
    const float vHatLevel = val(HatLevel);
    const float vCrashLevel = val(CrashLevel);
    const float vRideLevel = val(RideLevel);

    for (int32_t i = 0; i < frames; ++i) {
        float mix = 0.0f;
        float kickOut = 0.0f;

        // --- kick: a sine that sweeps down to its tuned pitch
        if (amp[Kick].active || aux[Kick].active) {
            const float env = amp[Kick].next();
            const float sweep = pitch[Kick].next();
            const float hz = tuneOf[Kick] * (1.0f + sweep * vKickPunch * 12.0f);
            float s = std::sin(osc[Kick].step(hz, sr) * kTwoPi) * env;
            const float click = aux[Kick].next();
            if (click > 0.0f) s += noise() * click * 0.5f;
            const float d = vKickDrive;
            if (d > 0.0001f) {
                // Normalised on kKickNominal so drive changes the shape of
                // the kick and not its level.
                const float k = 1.0f + d * 6.0f;
                s = dsp::fastTanh(s * k) * (kKickNominal / dsp::fastTanh(kKickNominal * k));
            }
            kickOut = s * vKickLevel * kLevelTrim[TKick];
            mix += kickOut;
        }

        // --- snare: two tones and noise
        if (amp[Snare].active || aux[Snare].active) {
            const float env = amp[Snare].next();
            const float nEnv = aux[Snare].next();
            const float tone = vSnareTone;
            const float body = (std::sin(osc[Snare].step(tuneOf[Snare], sr) * kTwoPi) * 0.6f +
                                std::sin(osc2[Snare].step(tuneOf[Snare] * 1.48f, sr) * kTwoPi) * 0.4f) * env;
            const float rattle = bp[Snare].step(noise()).bp * nEnv * vSnareSnap * 1.6f;
            mix += (body * (1.0f - tone * 0.5f) + rattle) * vSnareLevel * kLevelTrim[TSnare];
        }

        // --- clap: four bursts and a room
        if (amp[Clap].active) {
            float env = amp[Clap].next();
            if (clapLeft > 0) {
                clapPhase -= 1.0f;
                if (clapPhase <= 0.0f) {
                    --clapLeft;
                    clapPhase = vClapSpread * sr;
                    amp[Clap].level = gain[Clap]; // the next burst
                    env = amp[Clap].level;
                }
            }
            mix += bp[Clap].step(noise()).bp * env * vClapLevel * kLevelTrim[TClap] * 1.4f;
        }

        // --- rim, toms, cowbell
        if (amp[Rim].active) {
            const float env = amp[Rim].next();
            mix += bp[Rim].step(noise() * 0.4f + (osc[Rim].step(tuneOf[Rim], sr) < 0.5f ? 1.0f : -1.0f)).bp *
                   env * vRimLevel * kLevelTrim[TRim];
        }
        for (int32_t v = TomLo; v <= TomHi; ++v) {
            if (!amp[v].active) continue;
            const float env = amp[v].next();
            const float bend = pitch[v].next();
            const float hz = tuneOf[v] * (1.0f + bend * 1.2f);
            mix += std::sin(osc[v].step(hz, sr) * kTwoPi) * env * vTomLevel * kLevelTrim[TTom];
        }
        if (amp[Cowbell].active) {
            const float env = amp[Cowbell].next();
            const float a = osc[Cowbell].step(tuneOf[Cowbell], sr) < 0.5f ? 1.0f : -1.0f;
            const float b = osc2[Cowbell].step(tuneOf[Cowbell] * 1.5f, sr) < 0.5f ? 1.0f : -1.0f;
            mix += bp[Cowbell].step((a + b) * 0.4f).bp * env * vBellLevel * kLevelTrim[TBell];
        }

        // --- all metal voices share one stack of six squares
        const bool anyMetal = amp[HatClosed].active || amp[HatOpen].active || amp[Crash].active || amp[Ride].active;
        if (anyMetal) {
            const float source = metallic(hatTune);
            if (amp[HatClosed].active) {
                mix += bp[HatClosed].step(source).hp * amp[HatClosed].next() * vHatLevel * kLevelTrim[THat];
            }
            if (amp[HatOpen].active) {
                mix += bp[HatOpen].step(source).hp * amp[HatOpen].next() * vHatLevel * kLevelTrim[THat];
            }
            if (amp[Crash].active) {
                mix += bp[Crash].step(source * 0.8f + noise() * 0.2f).hp * amp[Crash].next() * vCrashLevel * kLevelTrim[TCrash];
            }
            if (amp[Ride].active) {
                const float bell = std::sin(osc[Ride].step(hatTune * 2.5f, sr) * kTwoPi) * aux[Ride].next();
                mix += (bp[Ride].step(source).hp * amp[Ride].next() + bell * 0.5f) * vRideLevel * kLevelTrim[TRide];
            }
        }

        // --- bus: compression, and ducking from the kick
        const float rectified = std::fabs(mix);
        compEnv = dsp::guardDenormal(compEnv + (rectified - compEnv) * (rectified > compEnv ? compAttack : compRelease));
        const float over = std::max(0.0f, compEnv - 0.25f);
        const float compGain = 1.0f / (1.0f + over * comp * 4.0f);
        const float kickLevel = std::fabs(kickOut);
        duckEnv = dsp::guardDenormal(duckEnv + (kickLevel - duckEnv) * (kickLevel > duckEnv ? 0.02f : 0.0006f));
        const float duckGain = 1.0f / (1.0f + duckEnv * duck * 6.0f);
        // The kick isn't ducked by itself.
        float out = (mix - kickOut) * compGain * duckGain + kickOut * compGain;

        if (drive > 0.0001f) {
            // Normalised on kBusNominal, like the kick, so drive changes the
            // shape and not the loudness.
            const float k = 1.0f + drive * 8.0f;
            out = dsp::fastTanh(out * k) * (kBusNominal / dsp::fastTanh(kBusNominal * k));
        }
        out *= volume * kHouse;
        L[i] = out * panL * 1.4142f;
        R[i] = out * panR * 1.4142f;
    }
    return true;
}

} // namespace acidulous::machine
