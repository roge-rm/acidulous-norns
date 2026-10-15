#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/format/Decoded.h>
#include <memory>
#include <string>

// Reads AIFF and AIFF-C: 8, 16, 24 and 32-bit PCM and 32-bit float, mono or
// stereo, any rate. The counterpart of AiffWriter.
namespace acidulous {

class AiffReader {
  public:
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
