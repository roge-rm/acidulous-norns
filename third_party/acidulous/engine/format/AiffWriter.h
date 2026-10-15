#pragma once
#include <cstdint>
#include <cstdio>
#include <engine/format/AudioSink.h>

// Writes stereo AIFF and fills in the header on close.
//
// Like WavWriter but big-endian, with chunks inside a FORM and the sample rate
// stored as an 80-bit IEEE extended float.
//
// 16 and 24-bit PCM write plain AIFF. Plain AIFF can't store floats, so 32-bit
// writes AIFF-C: FORM type AIFC, a format version chunk and compression type
// 'fl32' (uncompressed floats). The extension stays .aiff either way.

namespace acidulous {

class AiffWriter : public AudioSink {
  public:
    ~AiffWriter() override { close(); }

    bool open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) override;
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
