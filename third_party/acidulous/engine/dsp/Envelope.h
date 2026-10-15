#pragma once
#include "Math.h"

namespace acidulous::dsp {

// Attack then exponential decay to zero: the shape an acid filter wants.
class DecayEnv {
  public:
    void setSampleRate(float sr) { sampleRate = sr; }
    void setTimes(float attackSec, float decaySec) {
        attackCoeff = onePoleCoeff(attackSec, sampleRate);
        decayCoeff = onePoleCoeff(decaySec, sampleRate);
    }
    void trigger() { rising = true; }
    void kill() { rising = false; level = 0.0f; }

    float next() {
        if (rising) {
            level += (1.0f - level) * attackCoeff;
            if (level > 0.995f) rising = false;
        } else {
            level -= level * decayCoeff;
        }
        return level;
    }
    float value() const { return level; }

  private:
    float sampleRate = 48000.0f;
    float attackCoeff = 0.1f, decayCoeff = 0.001f;
    float level = 0.0f;
    bool rising = false;
};

// Gate-driven attack/sustain/release amplitude envelope.
class AsrEnv {
  public:
    void setSampleRate(float sr) { sampleRate = sr; }
    void setTimes(float attackSec, float releaseSec) {
        attackCoeff = onePoleCoeff(attackSec, sampleRate);
        releaseCoeff = onePoleCoeff(releaseSec, sampleRate);
    }
    void gate(bool on) { gateOn = on; }
    /**
     * Straight to silence, with no release. Use this for a reset, since
     * `gate(false)` leaves the last note fading out.
     */
    void kill() { gateOn = false; level = 0.0f; }
    bool active() const { return gateOn || level > 1e-4f; }

    float next() {
        if (gateOn) {
            level += (1.0f - level) * attackCoeff;
        } else {
            level -= level * releaseCoeff;
        }
        return level;
    }

  private:
    float sampleRate = 48000.0f;
    float attackCoeff = 0.1f, releaseCoeff = 0.01f;
    float level = 0.0f;
    bool gateOn = false;
};

} // namespace acidulous::dsp
