#include "WavWriter.h"
#include <cmath>
#include <cstring>

namespace acidulous {

namespace {
void put16(FILE *f, uint32_t v) { const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}; std::fwrite(b, 1, 2, f); }
void put32(FILE *f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
constexpr int32_t kChannels = 2;
} // namespace

bool WavWriter::open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) {
    close();
    file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) { error = "can't create " + path; return false; }
    rate = sampleRate;
    floatFormat = bits == 32;
    bytesPerSample = floatFormat ? 4 : (bits == 16 ? 2 : 3);
    frames = 0;
    writeHeader();
    return true;
}

void WavWriter::writeHeader() {
    const uint32_t dataBytes = static_cast<uint32_t>(frames * kChannels * bytesPerSample);
    std::fseek(file, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file);
    put32(file, 36 + dataBytes);
    std::fwrite("WAVE", 1, 4, file);
    std::fwrite("fmt ", 1, 4, file);
    put32(file, 16);
    put16(file, floatFormat ? 3 : 1); // 1 PCM, 3 IEEE float
    put16(file, kChannels);
    put32(file, static_cast<uint32_t>(rate));
    put32(file, static_cast<uint32_t>(rate * kChannels * bytesPerSample));
    put16(file, static_cast<uint32_t>(kChannels * bytesPerSample));
    put16(file, static_cast<uint32_t>(8 * bytesPerSample));
    std::fwrite("data", 1, 4, file);
    put32(file, dataBytes);
}

void WavWriter::write(const float *interleaved, int32_t framesIn) {
    if (file == nullptr) return;
    uint8_t buf[64 * kChannels * 4];
    int32_t done = 0;
    while (done < framesIn) {
        const int32_t n = framesIn - done < 64 ? framesIn - done : 64;
        for (int32_t i = 0; i < n * kChannels; ++i) {
            float v = interleaved[(done * kChannels) + i];
            if (floatFormat) {
                // Float is written as is, without clamping.
                std::memcpy(buf + i * 4, &v, 4);
                continue;
            }
            if (bytesPerSample == 2) {
                const int32_t s = quantise(v, 16);
                buf[i * 2] = static_cast<uint8_t>(s);
                buf[i * 2 + 1] = static_cast<uint8_t>(s >> 8);
            } else {
                const int32_t s = quantise(v, 24);
                buf[i * 3] = static_cast<uint8_t>(s);
                buf[i * 3 + 1] = static_cast<uint8_t>(s >> 8);
                buf[i * 3 + 2] = static_cast<uint8_t>(s >> 16);
            }
        }
        std::fwrite(buf, 1, static_cast<size_t>(n * kChannels * bytesPerSample), file);
        done += n;
        frames += n;
    }
}

bool WavWriter::close() {
    if (file == nullptr) return true;
    writeHeader();
    const bool ok = std::fclose(file) == 0;
    file = nullptr;
    return ok;
}

} // namespace acidulous
