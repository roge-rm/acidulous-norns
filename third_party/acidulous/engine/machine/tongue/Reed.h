#pragma once
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

// A jaw harp's reed: a strip fixed at one end, ringing in the modes of a
// clamped bar, set going by a pluck and, blown or drawn hard, kept going by
// the air.
namespace acidulous::machine::tongue {
using dsp::clampf;

class Reed {
  public:
    static constexpr int kModes = 3;
    /** What's left of a ringing reed's swing when the finger meets it to pluck again. */
    static constexpr float kCaught = 0.3f;

    void prepare(float sampleRate) { sr = sampleRate; clear(); }

    /**
     * The second and third modes against the first. A steel strip of even
     * thickness is a clamped-free bar, 6.267 and 17.55; a reed cut and
     * tapered from bamboo or brass sits lower. Takes effect at the next tune().
     */
    void setRatios(float second, float third) {
        ratio[1] = second;
        ratio[2] = third;
    }

    void clear() {
        for (int m = 0; m < kModes; ++m) y1[m] = y2[m] = 0.0f;
        pulseLeft = snapLeft = pullLeft = 0;
        x = xBefore = v = 0.0f;
    }

    /**
     * The note (Hz), how long it rings (T60, seconds), how much of the pluck
     * reaches the overtones and how long they ring against the note. A reed
     * that's ringing keeps its motion.
     */
    void tune(float hz, float ring, float overtones, float overRing) {
        note = hz;
        for (int m = 0; m < kModes; ++m) {
            const float f = hz * ratio[m];
            const float was = w[m];
            w[m] = 6.28318530718f * clampf(f, 10.0f, 0.45f * sr) / sr;
            // A ringing mode keeps its swing and phase at the new pitch: the
            // last sample is rebuilt from them, or a drop in pitch would
            // grow the swing by the ratio.
            if (was > 0.0f && was != w[m]) {
                const float c = (y1[m] * std::cos(was) - y2[m]) / std::sin(was);
                y2[m] = y1[m] * std::cos(w[m]) - c * std::sin(w[m]);
            }
            // The finger's push reaches the note; the overtones are set going
            // by the snap as it leaves (pluck()), [overtones] of the note's
            // swing times 1/ratio, so each moves the air about as much as the note.
            gain[m] = m == 0 && f < 0.45f * sr ? std::sin(w[m]) : 0.0f;
            snapLevel[m] = m > 0 && f < 0.45f * sr ? overtones / ratio[m] : 0.0f;
        }
        rings[0] = ring;
        rings[1] = ring * overRing;
        rings[2] = ring * overRing * 0.4f;
        setDrive(drive);
    }

    /**
     * Air feeding the reed, nepers a second added to its own loss: 0 lets it
     * die as plucked, enough cancels the loss and it sustains. Per block.
     */
    void setDrive(float nepers) {
        drive = nepers;
        for (int m = 0; m < kModes; ++m) {
            // Only the note is fed: the air pushes at the reed's own swing.
            const float rate = -6.907755f / std::fmax(rings[m], 0.005f) + (m == 0 ? nepers : 0.0f);
            const float r = std::fmin(std::exp(rate / sr), 0.99999f);
            a1[m] = 2.0f * r * std::cos(w[m]);
            a2[m] = -r * r;
        }
    }

    /**
     * A pluck: half a sine of force [contact] seconds long, swinging it by
     * about [swing], then the snap as the finger leaves, [snap] 0 to 1, which
     * sets the overtones going. The finger meets the reed on the way, so most
     * of what was ringing is taken out first.
     */
    void pluck(float swing, float contact, float snap) {
        catchIt();
        pullLeft = 0;
        // A finger can't touch a high reed for longer than a third of its
        // swing, or the push would cancel itself out.
        const float touch = std::fmin(contact, 0.35f / note);
        pulseLength = touch * sr < 2.0f ? 2 : static_cast<int>(touch * sr);
        pulseLeft = pulseLength;
        // What a half sine of force reaches at the note, against a quick tap.
        const float fT = note * static_cast<float>(pulseLength) / sr;
        const float reach = std::cos(3.14159265f * fT) / (1.0f - 4.0f * fT * fT);
        pulseScale = swing * 3.14159265f / (2.0f * static_cast<float>(pulseLength)) / std::fmax(reach, 0.3f);
        setSnap(swing, snap);
    }

    /**
     * A pull on a string tied to the frame: the frame is drawn back over
     * [ramp] seconds, bending the reed by about [swing], then let go, and
     * the reed springs back, with [snap] of the overtones as it does.
     */
    void pull(float swing, float ramp, float snap) {
        catchIt();
        pulseLeft = 0;
        pullLength = std::max(2, static_cast<int>(ramp * sr));
        pullLeft = pullLength;
        // A steady force F holds the note's mode at F / w, near enough.
        pullForce = swing * w[0];
        setSnap(swing, snap);
    }

    /** One sample; [push] is any other force on it. Returns where the reed is. */
    float step(float push) {
        float force = push;
        if (pullLeft > 0) {
            // Drawn back along a ramp; at the end the force stops at once.
            force += pullForce * static_cast<float>(pullLength - pullLeft + 1) / static_cast<float>(pullLength);
            --pullLeft;
            if (pullLeft == 0) snapLeft = snapLength;
        }
        if (pulseLeft > 0) {
            force += pulseScale * std::sin(3.14159265f * static_cast<float>(pulseLength - pulseLeft) / static_cast<float>(pulseLength));
            --pulseLeft;
            if (pulseLeft == 0) snapLeft = snapLength;
        }
        float snapForce = 0.0f;
        if (snapLeft > 0) {
            snapForce = std::sin(3.14159265f * static_cast<float>(snapLength - snapLeft) / static_cast<float>(snapLength));
            --snapLeft;
        }
        float out = 0.0f;
        for (int m = 0; m < kModes; ++m) {
            const float y = gain[m] * force + snapScale[m] * snapForce + a1[m] * y1[m] + a2[m] * y2[m];
            y2[m] = y1[m];
            y1[m] = y;
            out += y;
        }
        xBefore = x;
        x = out;
        // Velocity in the swing's units a radian of the note: a reed swinging
        // by 1 reaches about 1.
        v = (x - xBefore) / w[0];
        return x;
    }

    float position() const { return x; }
    float velocity() const { return v; }
    bool plucking() const { return pulseLeft > 0 || snapLeft > 0 || pullLeft > 0; }
    float hz() const { return note; }

  private:
    /** The finger meets a ringing reed: most of its swing is taken out. */
    void catchIt() {
        for (int m = 0; m < kModes; ++m) {
            y1[m] *= kCaught;
            y2[m] *= kCaught;
        }
        x *= kCaught;
        xBefore *= kCaught;
    }

    /**
     * The snap as the finger or string leaves: a quarter of the first
     * overtone's period, short enough to set it going, scaled for what a
     * pulse that long reaches at each.
     */
    void setSnap(float swing, float snap) {
        snapLength = std::max(2, static_cast<int>(0.25f * sr / (note * ratio[1])));
        snapLeft = 0;
        for (int m = 1; m < kModes; ++m) {
            const float fT = note * ratio[m] * static_cast<float>(snapLength) / sr;
            const float reachM = std::cos(3.14159265f * fT) / (1.0f - 4.0f * fT * fT);
            snapScale[m] = std::sin(w[m]) * swing * snap * snapLevel[m] * 3.14159265f / (2.0f * static_cast<float>(snapLength)) /
                           std::fmax(reachM, 0.3f);
        }
    }

    float sr = 48000.0f, note = 200.0f, drive = 0.0f;
    float ratio[kModes] = {1.0f, 6.267f, 17.55f};
    int pullLeft = 0, pullLength = 2;
    float pullForce = 0.0f;
    float w[kModes] = {}, gain[kModes] = {}, a1[kModes] = {}, a2[kModes] = {};
    float rings[kModes] = {2.0f, 0.3f, 0.1f};
    float y1[kModes] = {}, y2[kModes] = {};
    int pulseLeft = 0, pulseLength = 2;
    float pulseScale = 0.0f;
    int snapLeft = 0, snapLength = 2;
    float snapLevel[kModes] = {}, snapScale[kModes] = {};
    float x = 0.0f, xBefore = 0.0f, v = 0.0f;
};

/**
 * The frame's own ring when it's plucked: a woody tock on bamboo, a buzzy
 * ring on a thin brass plate, a small tick on a steel frame. One mode,
 * struck once a pluck.
 */
class Knock {
  public:
    void set(float hz, float ring, float sampleRate) {
        const float w = 6.28318530718f * clampf(hz, 20.0f, 0.45f * sampleRate) / sampleRate;
        const float r = std::exp(-6.907755f / (std::fmax(ring, 0.002f) * sampleRate));
        a1 = 2.0f * r * std::cos(w);
        a2 = -r * r;
        kick = std::sin(w);
    }
    /** Rings at about [amount] from the next sample. */
    void strike(float amount) { y1 += amount * kick; }
    float step() {
        const float y = a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
    void clear() { y1 = y2 = 0.0f; }

  private:
    float a1 = 0.0f, a2 = 0.0f, kick = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

} // namespace acidulous::machine::tongue
