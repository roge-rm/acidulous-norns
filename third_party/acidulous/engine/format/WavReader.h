#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/format/Decoded.h>
#include <memory>
#include <string>

// Reads uncompressed WAV (PCM 8/16/24/32-bit and 32-bit float, mono or
// stereo, any rate) and resamples to the engine rate.
namespace acidulous {

class WavReader {
  public:
    // Returns nullptr on failure and `error` says why. A targetRate of 0 or
    // less keeps the file's own rate, which multisamples use. maxSeconds is
    // how much of a long file to take, either kMaxDecodeSeconds or
    // kMaxSliceSeconds.
    static std::unique_ptr<SampleData> read(const std::string &path, int32_t targetRate, std::string &error,
                                            int32_t maxSeconds = kMaxDecodeSeconds);
    // Kept for callers. The value lives in Decoded.h so every reader uses the
    // same one.
    static constexpr int32_t kMaxSeconds = kMaxDecodeSeconds;
};

} // namespace acidulous
