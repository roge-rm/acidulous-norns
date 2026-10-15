#include "ReelCache.h"
#include <algorithm>
#include <cstdio>
#include <engine/core/Constants.h>
#include <engine/core/Reel.h>
#include <engine/format/WavStream.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace acidulous::audio {

/**
 * Converts a long take once into planar int16 at the engine rate, which is
 * what `Reel::Source` reads, so the audio thread can map and index it
 * directly. Unlike `WavReader` it works in chunks, so memory use doesn't grow
 * with the file.
 *
 * Stereo is written in two passes over the source, one per channel, to avoid
 * buffering a whole channel.
 *
 * Returns the frame count written, or 0 with the reason in [error].
 */
int64_t ReelCache::convert(const std::string &path, const std::string &dest, bool &stereoOut,
                          std::string &error) {
    WavStream in;
    if (!in.open(path, error)) return 0;
    stereoOut = in.channels() == 2;
    const double ratio = static_cast<double>(in.rate()) / static_cast<double>(kSampleRate);
    const int64_t outFrames = static_cast<int64_t>(static_cast<double>(in.frames()) / ratio);
    if (outFrames <= 0) {
        error = "empty";
        return 0;
    }
    if (outFrames > static_cast<int64_t>(kMaxReelSeconds) * kSampleRate) {
        error = "longer than half an hour";
        return 0;
    }

    std::FILE *out = std::fopen(dest.c_str(), "wb");
    if (out == nullptr) {
        error = "can't write the cache";
        return 0;
    }

    constexpr int64_t kChunk = 1 << 15; // 32k frames out: 64 kB of int16
    std::vector<float> src(static_cast<size_t>(kChunk * 2 + 4));
    std::vector<int16_t> dst(static_cast<size_t>(kChunk));
    bool ok = true;
    for (int32_t ch = 0; ch < (stereoOut ? 2 : 1) && ok; ++ch) {
        int64_t written = 0;
        while (written < outFrames && ok) {
            const int64_t n = std::min<int64_t>(kChunk, outFrames - written);
            // The source frames these output frames come from, plus one extra
            // so the last interpolation has a right-hand neighbour.
            const int64_t first = static_cast<int64_t>(static_cast<double>(written) * ratio);
            const int64_t last = static_cast<int64_t>(static_cast<double>(written + n) * ratio) + 2;
            const int64_t span = last - first;
            if (span > static_cast<int64_t>(src.size())) src.resize(static_cast<size_t>(span));
            const int64_t got = in.read(first, ch, src.data(), span);
            for (int64_t i = 0; i < n; ++i) {
                const double at = static_cast<double>(written + i) * ratio - static_cast<double>(first);
                const auto i0 = static_cast<int64_t>(at);
                const int64_t i1 = i0 + 1 < got ? i0 + 1 : i0;
                if (i0 < 0 || i0 >= got) {
                    dst[static_cast<size_t>(i)] = 0;
                    continue;
                }
                const auto frac = static_cast<float>(at - static_cast<double>(i0));
                const float v = src[static_cast<size_t>(i0)] * (1.0f - frac) +
                                src[static_cast<size_t>(i1)] * frac;
                dst[static_cast<size_t>(i)] = toI16(v);
            }
            if (std::fwrite(dst.data(), sizeof(int16_t), static_cast<size_t>(n), out) !=
                static_cast<size_t>(n)) {
                ok = false;
                error = "the cache couldn't be written";
            }
            written += n;
        }
    }
    std::fclose(out);
    if (!ok) {
        std::remove(dest.c_str());
        return 0;
    }
    return outFrames;
}

/**
 * The converted file's name: a hash of the source path, size and modified
 * time. Re-importing the same file reuses the conversion, and a file edited
 * in place gets a new one. Hashed so the result is a valid filename.
 */
std::string ReelCache::nameFor(const std::string &path) {
    struct stat st {};
    const long long size = ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : 0;
    const long long when = ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;
    uint64_t h = 1469598103934665603ull;
    const std::string key = path + "|" + std::to_string(size) + "|" + std::to_string(when);
    for (char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.i16", static_cast<unsigned long long>(h));
    return name;
}


} // namespace acidulous::audio
