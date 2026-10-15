#include "WavReader.h"
#include "Decoded.h"
#include <algorithm>
#include <cstring>

namespace acidulous {

namespace {
uint32_t u32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

std::unique_ptr<SampleData> WavReader::read(const std::string &path, int32_t targetRate, std::string &error,
                                           int32_t maxSeconds) {
    std::vector<unsigned char> bytes;
    if (!slurp(path, bytes, error, slurpCeilingFor(maxSeconds))) return nullptr;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return nullptr;
    }

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char *data = nullptr;
    size_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char *id = bytes.data() + pos;
        const size_t len = u32(bytes.data() + pos + 4);
        const unsigned char *body = bytes.data() + pos + 8;
        // What's left after the header. Compared this way round, never as
        // pos + 8 + len, which wraps on a 32-bit build.
        const size_t room = bytes.size() - pos - 8;
        if (std::memcmp(id, "data", 4) == 0) {
            // A recorder that crashed or streamed leaves a length that runs
            // past the end, often 0xFFFFFFFF. Take what's there.
            data = body;
            dataLen = std::min(len, room);
            if (len > room) break;
        } else if (len > room) {
            break;
        } else if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && len >= 26) format = u16(body + 24); // WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
        }
        pos += 8 + len + (len & 1);
    }
    if (data == nullptr || channels == 0 || rate == 0) { error = "missing fmt or data"; return nullptr; }
    if (channels > 2) { error = "more than two channels"; return nullptr; }
    const bool isFloat = format == 3;
    if (!(format == 1 || isFloat)) { error = "compressed WAV"; return nullptr; }
    if (!((isFloat && bits == 32) || (!isFloat && (bits == 8 || bits == 16 || bits == 24 || bits == 32)))) {
        error = "unsupported bit depth";
        return nullptr;
    }
    if (rate > kMaxFileRate) { error = "unsupported sample rate"; return nullptr; }
    const size_t bytesPerSample = bits / 8;
    const size_t frameBytes = bytesPerSample * channels;
    const uint64_t frames = dataLen / frameBytes;
    if (frames == 0) { error = "empty"; return nullptr; }

    DecodedAudio got;
    got.rate = static_cast<int32_t>(rate);
    got.stereo = channels == 2;
    const uint64_t cap = static_cast<uint64_t>(maxSeconds) * rate;
    got.truncated = frames > cap;
    got.frames = static_cast<int32_t>(std::min(frames, cap));
    for (uint16_t c = 0; c < channels; ++c) got.ch[c].resize(static_cast<size_t>(got.frames));
    for (int32_t i = 0; i < got.frames; ++i) {
        for (uint16_t c = 0; c < channels; ++c) {
            const unsigned char *p = data + (static_cast<size_t>(i) * channels + c) * bytesPerSample;
            float v;
            if (isFloat) {
                uint32_t bitsv = u32(p);
                std::memcpy(&v, &bitsv, 4);
            } else if (bits == 8) {
                v = (static_cast<int>(p[0]) - 128) / 128.0f;
            } else if (bits == 16) {
                v = static_cast<int16_t>(u16(p)) / 32768.0f;
            } else if (bits == 24) {
                int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
                v = static_cast<float>(s >> 8) / 8388608.0f;
            } else {
                v = static_cast<int32_t>(u32(p)) / 2147483648.0f;
            }
            got.ch[c][static_cast<size_t>(i)] = v;
        }
    }
    auto out = assemble(got, path, targetRate, maxSeconds);
    if (!out) error = "empty";
    return out;
}

} // namespace acidulous
