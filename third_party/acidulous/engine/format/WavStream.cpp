#include "WavStream.h"
#include <cstring>
#include <vector>

namespace acidulous {

namespace {
uint32_t u32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t u16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
} // namespace

bool WavStream::open(const std::string &path, std::string &error) {
    close();
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot open";
        return false;
    }
    unsigned char head[12];
    if (std::fread(head, 1, 12, file) != 12 || std::memcmp(head, "RIFF", 4) != 0 ||
        std::memcmp(head + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        close();
        return false;
    }

    uint16_t format = 0, bits = 0;
    // Walk the chunks by their headers. `fmt ` is small and read whole. `data`
    // isn't read here, only its size and offset are noted.
    while (true) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, file) != 8) break;
        const uint32_t len = u32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
            std::vector<unsigned char> body(len);
            if (std::fread(body.data(), 1, len, file) != len) break;
            format = u16(body.data());
            chans = u16(body.data() + 2);
            srcRate = static_cast<int32_t>(u32(body.data() + 4));
            bits = u16(body.data() + 14);
            if (format == 0xFFFE && len >= 26) format = u16(body.data() + 24);
            if ((len & 1) != 0) std::fseek(file, 1, SEEK_CUR);
            continue;
        }
        if (std::memcmp(ch, "data", 4) == 0) {
            dataOffset = std::ftell(file);
            floatFormat = format == 3;
            bytesPerSample = bits / 8;
            if (chans <= 0 || chans > 2 || srcRate <= 0 || bytesPerSample <= 0) break;
            if (!(format == 1 || floatFormat)) {
                error = "compressed WAV";
                close();
                return false;
            }
            if (!((floatFormat && bits == 32) ||
                  (!floatFormat && (bits == 8 || bits == 16 || bits == 24 || bits == 32)))) {
                error = "unsupported bit depth";
                close();
                return false;
            }
            frameCount = static_cast<int64_t>(len) / (bytesPerSample * chans);
            if (frameCount <= 0) break;
            return true;
        }
        if (std::fseek(file, static_cast<long>(len + (len & 1)), SEEK_CUR) != 0) break;
    }
    error = "missing fmt or data";
    close();
    return false;
}

void WavStream::close() {
    if (file != nullptr) std::fclose(file);
    file = nullptr;
    frameCount = 0;
    chans = 0;
}

int64_t WavStream::read(int64_t from, int32_t ch, float *out, int64_t count) {
    if (file == nullptr || out == nullptr || count <= 0) return 0;
    if (ch < 0 || ch >= chans) ch = 0;
    if (from < 0) from = 0;
    if (from >= frameCount) return 0;
    const int64_t want = from + count > frameCount ? frameCount - from : count;

    const int32_t frameBytes = bytesPerSample * chans;
    // One seek and one read per call. A chunk is tens of thousands of frames,
    // so the cost per frame is just the conversion below.
    if (std::fseek(file, static_cast<long>(dataOffset + from * frameBytes), SEEK_SET) != 0) return 0;
    std::vector<unsigned char> buf(static_cast<size_t>(want * frameBytes));
    const size_t got = std::fread(buf.data(), 1, buf.size(), file);
    const int64_t frames = static_cast<int64_t>(got) / frameBytes;

    for (int64_t i = 0; i < frames; ++i) {
        const unsigned char *p = buf.data() + (i * chans + ch) * bytesPerSample;
        float v;
        if (floatFormat) {
            const uint32_t bitsv = u32(p);
            std::memcpy(&v, &bitsv, 4);
        } else if (bytesPerSample == 1) {
            v = (static_cast<int>(p[0]) - 128) / 128.0f;
        } else if (bytesPerSample == 2) {
            v = static_cast<int16_t>(u16(p)) / 32768.0f;
        } else if (bytesPerSample == 3) {
            const int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
            v = static_cast<float>(s >> 8) / 8388608.0f;
        } else {
            v = static_cast<int32_t>(u32(p)) / 2147483648.0f;
        }
        out[i] = v;
    }
    return frames;
}

} // namespace acidulous
