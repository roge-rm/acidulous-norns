#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <engine/core/Constants.h>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/Convolver.h>
#include <engine/dsp/DelayLine.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * What a Venue mounts: an impulse, already shaped, cut into partitions and
 * transformed. Built off the audio thread and handed over with swapObject.
 */
struct VenueImpulse {
    VenueImpulse(const float *left, const float *right, int32_t frames) : conv(left, right, frames) {}
    ~VenueImpulse() { delete next; }
    dsp::Convolver conv;
    // More to retire along with this one: swapObject hands back a chain.
    VenueImpulse *next = nullptr;
};

/** How the impulse is shaped before it's built, in the knobs' own units. */
struct VenueShape {
    float length = 10.0f; // seconds kept, the end faded
    float decay = 20.0f;  // the 60 dB time of an extra fade, off at the top
    float stretch = 1.0f; // 0.5 to 2 times as long
};

/**
 * Off the audio thread: the impulse stretched, trimmed and faded as [shape]
 * says, and built. [right] null for a mono impulse.
 */
std::unique_ptr<VenueImpulse> buildVenueImpulse(const float *left, const float *right, int32_t frames,
                                                int32_t sampleRate, const VenueShape &shape);

/**
 * Scales an impulse so noise through it comes out at the level it went in.
 * [right] empty for mono.
 */
void normaliseImpulse(std::vector<float> &left, std::vector<float> &right);

/**
 * A room, or anything else with an impulse: the track convolved with a
 * recorded or captured impulse. With no impulse the track passes untouched.
 *
 * length, decay and stretch reshape the impulse, so they take effect when
 * the impulse is built again off the audio thread (EngineHost::loadImpulse).
 * A new impulse takes the input from then on while the old one's tail fades
 * out over kFadeSeconds.
 */
class Venue final : public Effect {
  public:
    enum P { Predelay, Length, Decay, Lowcut, Highcut, Stretch, Width, Mix, Gain, Count };
    static constexpr float kFadeSeconds = 0.3f;
    static constexpr float kMaxPredelayMs = 250.0f;

    Venue() { initParams(); }
    ~Venue() override;
    const char *typeName() const override { return "Venue"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    /** Slot 0 is the impulse, a VenueImpulse or null for none. Returns what's done with. */
    void *swapObject(int32_t slot, void *object) override;
    /** The shaping knobs move once a block: they only matter to the next build. */
    bool glides(int32_t index) const override { return index != Length && index != Decay && index != Stretch; }

    /** The shape the knobs ask for now, in their own units. */
    VenueShape shape() const;

  private:
    void retire(VenueImpulse *&x);
    void filters(float lowHz, float highHz);

    VenueImpulse *cur = nullptr;     // takes the input
    VenueImpulse *old = nullptr;     // rings out on silence, fading
    VenueImpulse *pending = nullptr; // waits for the fade to end
    bool hasPending = false;
    VenueImpulse *spent = nullptr;   // handed back on the next swap, when there is no retirer
    float fade = 0.0f, fadeStep = 0.0f;
    // 1 with an impulse, 0 without; ramps so the dry level doesn't jump.
    float presence = 0.0f, presenceStep = 0.0f;

    float sr = 48000.0f;
    dsp::ForgetfulDelayLine pre[2];
    dsp::Biquad low[2], high[2];
    float lowHz = 0.0f, highHz = 0.0f;
    bool lowOn = false, highOn = false;
};

} // namespace acidulous::effect
