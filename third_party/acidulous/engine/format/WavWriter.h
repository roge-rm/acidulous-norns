#pragma once
#include <cstdint>
#include <cstdio>
#include <engine/format/AudioSink.h>
#include <string>

// Writes stereo WAV and fills in the header on close.
//
// 24-bit PCM by default. 16-bit halves the file size. 32 writes floats, which
// freezing uses because a rack's output before its fader can go past full
// scale and clamping it would add distortion.
namespace acidulous {

class WavWriter : public AudioSink {
  public:
    ~WavWriter() override { close(); }
    /** [bits]: 16 or 24 for PCM, 32 for float. */
    bool open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) override;
    // Interleaved stereo floats, clipped to -1..1 unless writing float.
    void write(const float *interleaved, int32_t frames) override;
    bool close() override;
    int64_t framesWritten() const override { return frames; }

  private:
    void writeHeader();
    FILE *file = nullptr;
    int32_t rate = 48000;
    int32_t bytesPerSample = 3;
    bool floatFormat = false;
    int64_t frames = 0;
};

} // namespace acidulous
