#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

// The file an offline render writes into. The render loop opens it, writes
// interleaved stereo and closes it, whatever the format. Close fills in the
// header.

namespace acidulous {

/**
 * Converts a float to a PCM integer, matching how the readers convert back.
 *
 * Scaled by 2^(b-1) because the readers divide by that, so a file written
 * and read back comes out exactly the same (sink_test checks this). +1.0
 * can't be represented and is clamped to the largest code, while -1.0 lands
 * exactly on the smallest.
 */
inline int32_t quantise(float v, int32_t bits) {
    const auto scale = static_cast<float>(1 << (bits - 1));
    const int32_t lo = -(1 << (bits - 1));
    const int32_t hi = (1 << (bits - 1)) - 1;
    auto s = static_cast<int32_t>(std::lrint(v * scale));
    if (s < lo) s = lo;
    if (s > hi) s = hi;
    return s;
}


/**
 * An audio file format, used both for writing and for what `sniff` detected.
 * `Unknown` only ever comes from `sniff`.
 */
enum class AudioFormat : int32_t {
    Wav = 0,
    Aiff = 1,
    Flac = 2,
    /** Uses LAME. See Mp3Writer and Mp3Reader. */
    Mp3 = 3,
    Unknown = -1,
};

class AudioSink {
  public:
    virtual ~AudioSink() = default;

    /** [bits]: 16 or 24 for PCM, 32 for float where the format allows it. */
    virtual bool open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) = 0;
    /** [frames] frames of interleaved stereo. */
    virtual void write(const float *interleaved, int32_t frames) = 0;
    /** Patches the header and closes. Safe to call twice. */
    virtual bool close() = 0;
    virtual int64_t framesWritten() const = 0;
};

/** Null for a format that has no writer, which the caller must report. */
std::unique_ptr<AudioSink> makeSink(AudioFormat format);

/** ".wav", ".aiff", ".flac" or ".mp3", including the dot. */
const char *extensionFor(AudioFormat format);

/** True where [bits] == 32 means IEEE floats rather than PCM. */
bool supportsFloat(AudioFormat format);

} // namespace acidulous
