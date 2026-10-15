#include "AiffReader.h"
#include "Decoded.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// The counterpart of AiffWriter. AIFF is big-endian, its chunks are inside a
// FORM, and its sample rate is an 80-bit IEEE extended float.
//
// AIFF-C adds a compression type in COMM. We only read the uncompressed ones:
// `NONE` is big-endian PCM, `fl32` (or `FL32`) is 32-bit float as our writer
// makes it, and `sowt` is little-endian PCM. macOS writes `sowt` by default,
// so it's the most common.
namespace acidulous {

namespace {
uint16_t be16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/**
 * Reads the sample rate from an 80-bit IEEE extended float.
 *
 * Sign, 15 bits of exponent biased by 16383, then a 64-bit mantissa with its
 * leading one written out. The mantissa read as an integer is the fraction
 * scaled by 2^63, so ldexp takes the exponent minus the bias minus 63.
 */
double extended(const unsigned char *p) {
    const uint32_t biased = be16(p) & 0x7FFFu;
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
    if (biased == 0 && mantissa == 0) return 0.0;
    const double v = std::ldexp(static_cast<double>(mantissa), static_cast<int>(biased) - 16383 - 63);
    return (p[0] & 0x80u) != 0 ? -v : v;
}
} // namespace

std::unique_ptr<SampleData> AiffReader::read(const std::string &path, int32_t targetRate, std::string &error,
                                            int32_t maxSeconds) {
    std::vector<unsigned char> bytes;
    if (!slurp(path, bytes, error, slurpCeilingFor(maxSeconds))) return nullptr;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "FORM", 4) != 0) {
        error = "not an AIFF file";
        return nullptr;
    }
    const bool aifc = std::memcmp(bytes.data() + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(bytes.data() + 8, "AIFF", 4) != 0) {
        error = "not an AIFF file";
        return nullptr;
    }

    uint16_t channels = 0, bits = 0;
    uint32_t frameCount = 0;
    double rate = 0.0;
    char compression[5] = "NONE";
    const unsigned char *data = nullptr;
    size_t dataLen = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char *id = bytes.data() + pos;
        const size_t len = be32(bytes.data() + pos + 4);
        const unsigned char *body = bytes.data() + pos + 8;
        // What's left after the header, compared so nothing can wrap (see
        // WavReader).
        const size_t room = bytes.size() - pos - 8;
        if (std::memcmp(id, "SSND", 4) == 0 && std::min(len, room) >= 8) {
            // Eight bytes of offset and block size come before the samples.
            // The offset is almost always zero but not always. A length past
            // the end is a cut-off file, so take what's there.
            const size_t have = std::min(len, room);
            const size_t offset = be32(body);
            if (offset <= have - 8) {
                data = body + 8 + offset;
                dataLen = have - 8 - offset;
            }
            if (len > room) break;
        } else if (len > room) {
            break;
        } else if (std::memcmp(id, "COMM", 4) == 0 && len >= 18) {
            channels = be16(body);
            frameCount = be32(body + 2);
            bits = be16(body + 6);
            rate = extended(body + 8);
            if (aifc && len >= 22) std::memcpy(compression, body + 18, 4);
        }
        pos += 8 + len + (len & 1); // chunks are padded to even, like RIFF
    }

    if (data == nullptr || channels == 0 || rate <= 0.0) { error = "missing COMM or SSND"; return nullptr; }
    if (!(rate < static_cast<double>(kMaxFileRate))) { error = "unsupported sample rate"; return nullptr; }
    if (channels > 2) { error = "more than two channels"; return nullptr; }

    const bool isFloat = std::memcmp(compression, "fl32", 4) == 0 || std::memcmp(compression, "FL32", 4) == 0;
    const bool swapped = std::memcmp(compression, "sowt", 4) == 0;
    if (!isFloat && !swapped && std::memcmp(compression, "NONE", 4) != 0) {
        error = "compressed AIFF";
        return nullptr;
    }
    if (!((isFloat && bits == 32) || (!isFloat && (bits == 8 || bits == 16 || bits == 24 || bits == 32)))) {
        error = "unsupported bit depth";
        return nullptr;
    }

    const size_t bytesPerSample = bits / 8;
    const size_t frameBytes = bytesPerSample * channels;
    const uint64_t frames = std::min<uint64_t>(frameCount, dataLen / frameBytes);
    if (frames == 0) { error = "empty"; return nullptr; }

    DecodedAudio got;
    got.rate = static_cast<int32_t>(rate + 0.5);
    got.stereo = channels == 2;
    const uint64_t cap = static_cast<uint64_t>(maxSeconds) * static_cast<uint64_t>(got.rate);
    got.truncated = frames > cap;
    got.frames = static_cast<int32_t>(std::min(frames, cap));
    for (uint16_t c = 0; c < channels; ++c) got.ch[c].resize(static_cast<size_t>(got.frames));

    for (int32_t i = 0; i < got.frames; ++i) {
        for (uint16_t c = 0; c < channels; ++c) {
            const unsigned char *raw = data + (static_cast<size_t>(i) * channels + c) * bytesPerSample;
            unsigned char p[4];
            if (swapped) {
                for (uint32_t k = 0; k < bytesPerSample; ++k) p[k] = raw[bytesPerSample - 1 - k];
            } else {
                for (uint32_t k = 0; k < bytesPerSample; ++k) p[k] = raw[k];
            }
            float v;
            if (isFloat) {
                const uint32_t w = be32(p);
                std::memcpy(&v, &w, 4);
            } else if (bits == 8) {
                // AIFF's 8-bit is signed, unlike WAV's.
                v = static_cast<float>(static_cast<int8_t>(p[0])) / 128.0f;
            } else if (bits == 16) {
                v = static_cast<int16_t>(be16(p)) / 32768.0f;
            } else if (bits == 24) {
                const int32_t s = (p[0] << 24) | (p[1] << 16) | (p[2] << 8);
                v = static_cast<float>(s >> 8) / 8388608.0f;
            } else {
                v = static_cast<float>(static_cast<int32_t>(be32(p))) / 2147483648.0f;
            }
            got.ch[c][static_cast<size_t>(i)] = v;
        }
    }
    auto out = assemble(got, path, targetRate, maxSeconds);
    if (!out) error = "empty";
    return out;
}

} // namespace acidulous
