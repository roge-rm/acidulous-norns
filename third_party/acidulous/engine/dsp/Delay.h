#pragma once
#include "Math.h"
#include <cstdint>
#include <vector>

// A stereo delay on a send bus, synced to the clock. Time is a note value,
// turned into samples every block. The read position glides toward it so a
// tempo change bends the echoes instead of clicking. Feedback runs through a
// one-pole tone filter, and ping-pong crosses the channels.
namespace acidulous::dsp {

class Delay {
  public:
    static constexpr int kTimes = 7;
    // In quarter notes: 1/16, 1/8T, 1/8, 1/8., 1/4, 1/4., 1/2
    static constexpr float kBeats[kTimes] = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    static const char *timeName(int i) {
        static const char *names[kTimes] = {"1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
        return names[i < 0 ? 0 : (i >= kTimes ? kTimes - 1 : i)];
    }

    void prepare(int32_t sampleRate) {
        this->sampleRate = static_cast<float>(sampleRate);
        maxSamples = sampleRate * 2; // two seconds
        for (int c = 0; c < 2; ++c) buf[c].assign(static_cast<size_t>(maxSamples), 0.0f);
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c) { for (auto &v : buf[c]) v = 0.0f; lp[c] = 0.0f; }
        wr = 0;
        readSamples = targetSamples;
    }

    // timeIndex into kBeats; feedback, tone 0..1; pingPong on/off. Call per block.
    void set(int timeIndex, float feedbackAmt, float tone, bool pingPongOn, float bpm) {
        const float beats = kBeats[timeIndex < 0 ? 0 : (timeIndex >= kTimes ? kTimes - 1 : timeIndex)];
        targetSamples = clampf(beats * 60.0f / (bpm < 20.0f ? 20.0f : bpm) * sampleRate, 1.0f, static_cast<float>(maxSamples - 2));
        feedback = clampf(feedbackAmt, 0.0f, 0.95f);
        const float hz = 500.0f * std::exp2(clampf(tone, 0.0f, 1.0f) * 5.0f); // 500 Hz .. 16 kHz
        toneCoeff = 1.0f - std::exp(-kTwoPi * hz / sampleRate);
        pingPong = pingPongOn;
    }

    /**
     * Where to read, for a write head at [wr] and a delay of [samples]. Uses
     * `wrappedReadIndex`, which guards against float rounding landing the
     * index one past the end of the buffer.
     */
    static int readIndex(int32_t wr, float samples, int32_t size, float &frac) {
        return wrappedReadIndex(wr, samples, size, frac);
    }

    // In: a mono send. Out: added to L/R (100% wet).
    void process(const float *in, float *outL, float *outR, int32_t frames) {
        // Glide the read position by at most ~1% per block, so a tempo change
        // bends smoothly.
        const float maxStep = 0.01f * readSamples + 1.0f;
        float delta = targetSamples - readSamples;
        if (delta > maxStep) delta = maxStep; else if (delta < -maxStep) delta = -maxStep;
        const float perSample = delta / static_cast<float>(frames);

        for (int32_t i = 0; i < frames; ++i) {
            readSamples += perSample;
            float frac = 0.0f;
            const int r0 = readIndex(wr, readSamples, maxSamples, frac);
            const int r1 = (r0 + 1) % maxSamples;
            const float dl = buf[0][static_cast<size_t>(r0)] * (1.0f - frac) + buf[0][static_cast<size_t>(r1)] * frac;
            const float dr = buf[1][static_cast<size_t>(r0)] * (1.0f - frac) + buf[1][static_cast<size_t>(r1)] * frac;

            lp[0] += (dl - lp[0]) * toneCoeff;
            lp[1] += (dr - lp[1]) * toneCoeff;
            const float x = in[i];
            if (pingPong) {
                buf[0][static_cast<size_t>(wr)] = x + lp[1] * feedback;
                buf[1][static_cast<size_t>(wr)] = lp[0] * feedback;
            } else {
                buf[0][static_cast<size_t>(wr)] = x + lp[0] * feedback;
                buf[1][static_cast<size_t>(wr)] = x + lp[1] * feedback;
            }
            if (++wr >= maxSamples) wr = 0;
            outL[i] += dl;
            outR[i] += dr;
        }
    }

  private:
    std::vector<float> buf[2];
    int maxSamples = 96000;
    int wr = 0;
    float readSamples = 24000.0f, targetSamples = 24000.0f;
    float feedback = 0.4f, toneCoeff = 0.5f;
    bool pingPong = true;
    float lp[2]{};
    float sampleRate = 48000.0f;
};

} // namespace acidulous::dsp
