#pragma once
#include <cstdint>
#include <cstdio>
#include <engine/format/AudioSink.h>
#include <vector>

// Writes stereo FLAC, following the spec (RFC 9639).
//
// It writes FLAC's "Subset" profile, which every decoder must handle. It uses
// fixed predictors of order 0 to 4 instead of LPC. LPC would only save about
// 5% more and is a lot more code to get right. A typical mix comes out around
// half the size of the WAV.
//
// tools/flac_test.cpp encodes, decodes with another decoder and checks the
// samples match exactly.

namespace acidulous {

class FlacWriter : public AudioSink {
  public:
    ~FlacWriter() override { close(); }

    /** [bits]: 16 or 24. FLAC can't store floats, so 32 becomes 24. */
    bool open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) override;
    void write(const float *interleaved, int32_t frames) override;
    bool close() override;
    int64_t framesWritten() const override { return frames; }

  private:
    /** The Subset caps this at 4608 for 48 kHz and under; 4096 is usual. */
    static constexpr int32_t kBlock = 4096;

    void flushBlock();
    void writeStreamInfo();

    FILE *file = nullptr;
    int32_t rate = 48000;
    int32_t bps = 24;
    int64_t frames = 0;
    uint32_t frameNumber = 0;

    // One block's worth, de-interleaved, as integers.
    std::vector<int32_t> left, right;
    // Smallest and largest frame we actually wrote, for STREAMINFO.
    uint32_t minFrame = 0xffffffffu, maxFrame = 0;
    uint8_t md5Digest[16] = {};

    // MD5 of the unencoded samples as little-endian, as the format asks, so
    // `flac -t` can verify the file. State carries across blocks.
    struct Md5 {
        uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
        uint64_t length = 0;
        uint8_t buffer[64] = {};
        size_t have = 0;
        void update(const uint8_t *data, size_t n);
        void finish(uint8_t out[16]);
        void block(const uint8_t *p);
    } md5;
};

} // namespace acidulous
