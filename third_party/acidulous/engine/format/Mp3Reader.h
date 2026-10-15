#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/format/Decoded.h>
#include <memory>
#include <string>

// Reads MPEG audio (layer III, and layers I and II since the decoder handles
// them), mono or stereo, any rate. Uses LAME's mpglib, the decoder that comes
// with the LAME encoder.
namespace acidulous {

class Mp3Reader {
  public:
    /**
     * Where the MPEG audio starts, after any ID3v2 tag.
     *
     * The tag can hold hundreds of KB of JPEG album art, which is full of
     * 0xFF bytes that look like frame syncs. Decoding from byte zero can
     * lock onto one and play the picture as noise.
     */
    static size_t audioStart(const unsigned char *bytes, size_t size);

    /**
     * Null on failure, with [error] saying why. See WavReader::read.
     *
     * [maxSeconds] is how much of a long file to take. The rest is dropped
     * and `truncated` is set on the result.
     */
    static std::unique_ptr<SampleData> read(const std::string &path, int32_t targetRate, std::string &error,
                                           int32_t maxSeconds = kMaxDecodeSeconds);
};

} // namespace acidulous
