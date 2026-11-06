#include "ReelCache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
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
 * It writes a file of its own and renames it to [dest] when it's whole, so
 * two threads converting the same take at once don't write into each other
 * and nothing maps [dest] half written.
 *
 * Returns the frame count written, or 0 with the reason in [error].
 */
int64_t ReelCache::convert(const std::string &path, const std::string &dest, bool &stereoOut,
                          std::string &error) {
    WavStream in;
    if (!in.open(path, error)) return 0;
    stereoOut = in.channels() == 2;
    const double ratio = static_cast<double>(in.rate()) / static_cast<double>(kSampleRate);
    const int64_t outFrames = framesFor(in.frames(), in.rate());
    if (outFrames <= 0) {
        error = "empty";
        return 0;
    }
    if (outFrames > static_cast<int64_t>(kMaxReelSeconds) * kSampleRate) {
        error = "longer than half an hour";
        return 0;
    }

    static std::atomic<uint64_t> serial{0};
    const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    char tag[64];
    std::snprintf(tag, sizeof(tag), ".%llx-%llx-%llx.part", static_cast<unsigned long long>(thread),
                  static_cast<unsigned long long>(now), static_cast<unsigned long long>(serial.fetch_add(1)));
    const std::string part = dest + tag;
    std::FILE *out = std::fopen(part.c_str(), "wb");
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
    if (std::fclose(out) != 0 && ok) {
        ok = false;
        error = "the cache couldn't be written";
    }
    if (!ok) {
        std::remove(part.c_str());
        return 0;
    }
    if (std::rename(part.c_str(), dest.c_str()) != 0) {
        // Windows won't rename over a file. If another thread got there
        // first, its file is whole and the same, so that one does.
        std::remove(part.c_str());
        if (sizeFor(outFrames, stereoOut) != fileSize(dest)) {
            error = "the cache couldn't be written";
            return 0;
        }
    }
    return outFrames;
}

int64_t ReelCache::framesFor(int64_t frames, int32_t rate) {
    if (rate <= 0) return 0;
    const double ratio = static_cast<double>(rate) / static_cast<double>(kSampleRate);
    return static_cast<int64_t>(static_cast<double>(frames) / ratio);
}

int64_t ReelCache::sizeFor(int64_t frames, bool stereo) {
    return frames * static_cast<int64_t>(sizeof(int16_t)) * (stereo ? 2 : 1);
}

int64_t ReelCache::fileSize(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
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
