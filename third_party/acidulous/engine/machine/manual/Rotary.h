#pragma once
#include <cstdint>
#include <cmath>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/Math.h>

// The rotary speaker cabinet: a horn spinning one way over a drum spinning
// the other, heard by two microphones. The mics hear level changes, Doppler
// pitch bends and the tone dulling as each rotor points away.
//
// The rotors speed up faster than they slow down, like the real heavy ones.
// Both phases can be read as mod sources in Manual's matrix.
namespace acidulous::machine {

class Rotary {
  public:
    void prepare(float sr) {
        sampleRate = sr;
        hornL.prepare(static_cast<int32_t>(sr * 0.01f));
        hornR.prepare(static_cast<int32_t>(sr * 0.01f));
        drumL.prepare(static_cast<int32_t>(sr * 0.01f));
        drumR.prepare(static_cast<int32_t>(sr * 0.01f));
        lowSplit.lowpass(800.0f, 0.707f, sr);
        highSplit.lowpass(800.0f, 0.707f, sr);
        hornTone.lowpass(4200.0f, 0.6f, sr);
        upSec = downSec = -1.0f; // the coefficients depend on the sample rate
        reset();
    }

    void reset() {
        hornPhase = 0.0f;
        drumPhase = 0.25f;
        hornHz = drumHz = 0.0f;
        hornL.clear(); hornR.clear(); drumL.clear(); drumR.clear();
        lowSplit.reset(); highSplit.reset(); hornTone.reset();
        lpL = lpR = 0.0f;
    }

    // Sets the rotor target speeds and ramp times. Nexus's cabinet block calls
    // this every sample, so the ramp coefficients are only recalculated when
    // the ramp times change. See Waveguide::setFrequency for the same thing.
    void setTargets(float hornTargetHz, float drumTargetHz, float rampUpSec, float rampDownSec) {
        hornTarget = hornTargetHz;
        drumTarget = drumTargetHz;
        if (rampUpSec != upSec || rampDownSec != downSec) {
            upSec = rampUpSec;
            downSec = rampDownSec;
            upCoeff = dsp::onePoleCoeff(rampUpSec, sampleRate);
            downCoeff = dsp::onePoleCoeff(rampDownSec, sampleRate);
        }
    }

    void setMic(float distance01, float angle01, float spread01) {
        // Close mics get more level swing and Doppler, distant ones less.
        // Depth, angle and spread are kept small so the cabinet doesn't turn
        // into an auto-panner. Most of the effect should come from Doppler.
        depth = 0.08f + 0.24f * (1.0f - distance01);
        doppler = (0.25f + 0.75f * (1.0f - distance01)) * 0.0016f * sampleRate;
        // Half the angle between the mics, up to a third of a turn in total.
        angle = angle01 * 1.0471976f;
        spread = spread01;
    }

    /**
     * Advances the rotors without processing audio. The same speed and phase
     * maths as `process`, called while the organ is asleep so the cabinet is
     * in the right place and at the right speed when the next note arrives.
     */
    void spin(int32_t frames) {
        for (int32_t i = 0; i < frames; ++i) {
            hornHz += (hornTarget - hornHz) * (hornTarget > hornHz ? upCoeff : downCoeff);
            drumHz += (drumTarget - drumHz) * (drumTarget > drumHz ? upCoeff : downCoeff);
            hornPhase += hornHz / sampleRate;
            if (hornPhase >= 1.0f) hornPhase -= 1.0f;
            drumPhase -= drumHz / sampleRate;
            if (drumPhase < 0.0f) drumPhase += 1.0f;
        }
    }

    // One sample in, stereo out. `x` is the driven signal.
    void process(float x, float &outL, float &outR) {
        hornHz += (hornTarget - hornHz) * (hornTarget > hornHz ? upCoeff : downCoeff);
        drumHz += (drumTarget - drumHz) * (drumTarget > drumHz ? upCoeff : downCoeff);
        hornPhase += hornHz / sampleRate;
        if (hornPhase >= 1.0f) hornPhase -= 1.0f;
        drumPhase -= drumHz / sampleRate; // the drum turns the other way
        if (drumPhase < 0.0f) drumPhase += 1.0f;

        const float low = lowSplit.process(x);
        const float high = hornTone.process(x - low);

        const float hw = 6.2831853f * hornPhase, dw = 6.2831853f * drumPhase;
        const float hl = std::cos(hw + angle), hr = std::cos(hw - angle);
        const float dl = std::cos(dw + angle), dr = std::cos(dw - angle);

        // Doppler: the mouth's distance to each mic, as a delay.
        hornL.write(high);
        hornR.write(high);
        drumL.write(low);
        drumR.write(low);
        const float hdL = hornL.read(doppler * (1.0f + hl));
        const float hdR = hornR.read(doppler * (1.0f + hr));
        const float ddL = drumL.read(doppler * 0.35f * (1.0f + dl));
        const float ddR = drumR.read(doppler * 0.35f * (1.0f + dr));

        // Pointing away is quieter and duller. One pole is enough for the
        // dulling.
        const float ampHL = 1.0f + depth * hl, ampHR = 1.0f + depth * hr;
        const float ampDL = 1.0f + depth * 0.7f * dl, ampDR = 1.0f + depth * 0.7f * dr;
        float l = hdL * ampHL + ddL * ampDL;
        float r = hdR * ampHR + ddR * ampDR;
        // The dulling scales with mic distance (through depth), like the
        // level swing.
        const float tilt = depth * 1.1f;
        const float k = std::fmax(0.08f, 0.62f - tilt * (1.0f - hl) * 0.5f);
        lpL += (l - lpL) * k;
        lpR += (r - lpR) * k;
        l = lpL; r = lpR;

        // Spread only narrows what the mics heard. It never widens past it.
        const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * (0.25f + 0.75f * spread);
        outL = mid + side;
        outR = mid - side;
    }

    float horn01() const { return hornPhase; }
    float drum01() const { return drumPhase; }
    float hornRateHz() const { return hornHz; }

  private:
    float sampleRate = 48000.0f;
    float hornPhase = 0.0f, drumPhase = 0.25f;
    float hornHz = 0.0f, drumHz = 0.0f;
    float hornTarget = 0.0f, drumTarget = 0.0f;
    float upCoeff = 0.001f, downCoeff = 0.0005f;
    // The ramp times the coefficients were worked out for, so setTargets can
    // skip the work. Negative means not worked out yet.
    float upSec = -1.0f, downSec = -1.0f;
    float depth = 0.5f, doppler = 40.0f, angle = 1.5f, spread = 0.7f;
    float lpL = 0.0f, lpR = 0.0f;
    dsp::DelayLine hornL, hornR, drumL, drumR;
    dsp::Biquad lowSplit, highSplit, hornTone;
};

} // namespace acidulous::machine
