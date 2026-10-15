#include "AiffWriter.h"
#include <cmath>
#include <cstring>

namespace acidulous {

namespace {
constexpr int32_t kChannels = 2;

void put16(FILE *f, uint32_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    std::fwrite(b, 1, 2, f);
}
void put32(FILE *f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    std::fwrite(b, 1, 4, f);
}

/**
 * Writes the sample rate as an 80-bit IEEE extended float.
 *
 * Sign, 15 bits of exponent biased by 16383, then a 64-bit mantissa with its
 * leading one written out (other IEEE floats leave it implied). frexp gives a
 * fraction in [0.5, 1), so the bias used is one less.
 */
void putExtended(FILE *f, double v) {
    uint8_t b[10] = {};
    if (v != 0.0) {
        int exponent = 0;
        const double fraction = std::frexp(v, &exponent);
        const auto biased = static_cast<uint16_t>(exponent + 16382);
        const auto mantissa = static_cast<uint64_t>(std::ldexp(fraction, 64));
        b[0] = static_cast<uint8_t>(biased >> 8);
        b[1] = static_cast<uint8_t>(biased);
        for (int i = 0; i < 8; ++i) {
            b[2 + i] = static_cast<uint8_t>(mantissa >> (56 - 8 * i));
        }
    }
    std::fwrite(b, 1, 10, f);
}
} // namespace

bool AiffWriter::open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) {
    close();
    file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "cannot create " + path;
        return false;
    }
    rate = sampleRate;
    floatFormat = bits == 32;
    bytesPerSample = floatFormat ? 4 : (bits == 16 ? 2 : 3);
    frames = 0;
    writeHeader();
    return true;
}

void AiffWriter::writeHeader() {
    const auto dataBytes = static_cast<uint32_t>(frames * kChannels * bytesPerSample);
    // COMM is 18 bytes for PCM. For floats it adds a four-character type and
    // a Pascal string naming it.
    const uint32_t commBytes = floatFormat ? 18 + 4 + 6 : 18;
    const uint32_t fverBytes = floatFormat ? 8 + 4 : 0; // header + one long
    const uint32_t formBytes = 4 + fverBytes + (8 + commBytes) + (8 + 8 + dataBytes);

    std::fseek(file, 0, SEEK_SET);
    std::fwrite("FORM", 1, 4, file);
    put32(file, formBytes);
    std::fwrite(floatFormat ? "AIFC" : "AIFF", 1, 4, file);

    if (floatFormat) {
        std::fwrite("FVER", 1, 4, file);
        put32(file, 4);
        put32(file, 0xA2805140u); // the only AIFF-C version stamp
    }

    std::fwrite("COMM", 1, 4, file);
    put32(file, commBytes);
    put16(file, static_cast<uint32_t>(kChannels));
    put32(file, static_cast<uint32_t>(frames));
    put16(file, static_cast<uint32_t>(8 * bytesPerSample));
    putExtended(file, static_cast<double>(rate));
    if (floatFormat) {
        std::fwrite("fl32", 1, 4, file);
        // A Pascal string: one length byte, then the text, padded to even.
        const uint8_t len = 5;
        std::fwrite(&len, 1, 1, file);
        std::fwrite("Float", 1, 5, file);
    }

    std::fwrite("SSND", 1, 4, file);
    put32(file, 8 + dataBytes);
    put32(file, 0); // offset
    put32(file, 0); // block size
}

void AiffWriter::write(const float *interleaved, int32_t framesIn) {
    if (file == nullptr) {
        return;
    }
    uint8_t buf[64 * kChannels * 4];
    int32_t done = 0;
    while (done < framesIn) {
        const int32_t n = framesIn - done < 64 ? framesIn - done : 64;
        for (int32_t i = 0; i < n * kChannels; ++i) {
            float v = interleaved[(done * kChannels) + i];
            if (floatFormat) {
                // Big-endian byte order.
                uint32_t bits = 0;
                std::memcpy(&bits, &v, 4);
                buf[i * 4] = static_cast<uint8_t>(bits >> 24);
                buf[i * 4 + 1] = static_cast<uint8_t>(bits >> 16);
                buf[i * 4 + 2] = static_cast<uint8_t>(bits >> 8);
                buf[i * 4 + 3] = static_cast<uint8_t>(bits);
                continue;
            }
            if (bytesPerSample == 2) {
                const int32_t s = quantise(v, 16);
                buf[i * 2] = static_cast<uint8_t>(s >> 8);
                buf[i * 2 + 1] = static_cast<uint8_t>(s);
            } else {
                const int32_t s = quantise(v, 24);
                buf[i * 3] = static_cast<uint8_t>(s >> 16);
                buf[i * 3 + 1] = static_cast<uint8_t>(s >> 8);
                buf[i * 3 + 2] = static_cast<uint8_t>(s);
            }
        }
        std::fwrite(buf, 1, static_cast<size_t>(n * kChannels * bytesPerSample), file);
        done += n;
        frames += n;
    }
}

bool AiffWriter::close() {
    if (file == nullptr) {
        return true;
    }
    // An odd number of data bytes needs a pad byte. There's no chunk after
    // it, but readers may expect the padding anyway.
    if ((frames * kChannels * bytesPerSample) % 2 != 0) {
        const uint8_t pad = 0;
        std::fwrite(&pad, 1, 1, file);
    }
    writeHeader();
    const bool ok = std::fclose(file) == 0;
    file = nullptr;
    return ok;
}

} // namespace acidulous
