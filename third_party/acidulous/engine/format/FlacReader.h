#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/format/Decoded.h>
#include <memory>
#include <string>

// Reads FLAC: any bit depth up to 32, mono or stereo, any rate. It handles
// LPC as well as the fixed predictors FlacWriter uses, since other encoders
// use LPC.
namespace acidulous {

class FlacReader {
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
