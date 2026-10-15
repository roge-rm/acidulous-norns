#pragma once
#include <algorithm>
#include <cstdint>
#include <engine/core/Sample.h>
#include <memory>
#include <string>
#include <vector>

// What every audio reader produces before it becomes a SampleData, and the
// code they share.
//
// Each reader stops at channel planes at the file's own rate. Truncation,
// resampling and naming happen here, once, for all four formats.
namespace acidulous {

/** Channel planes at the file's own rate, before anything is done to them. */
struct DecodedAudio {
    std::vector<float> ch[2];
    int32_t frames = 0;
    int32_t rate = 48000;
    bool stereo = false;
    /**
     * Set by a reader that stopped early because it hit the cap.
     *
     * Readers stop decoding at the cap, so `assemble` gets planes that are
     * already the right length and can't tell anything was cut. This flag
     * lets the "only the first N seconds" message show.
     */
    bool truncated = false;
};

/**
 * The longest a sample can be, in seconds.
 *
 * A pad sample is one of thirteen, and 30 seconds is 11.5 MB each, so 150 MB
 * for all of them. A slice source is one file for the whole machine (mounted
 * at Forage::kSharedSlot, each pad reads a region of it), so it can be a whole
 * track. Ten minutes of stereo float at 48 kHz is 230 MB.
 */
constexpr int32_t kMaxDecodeSeconds = 30;
constexpr int32_t kMaxSliceSeconds = 600;

/**
 * The highest sample rate a file may claim. Real files stop at 768 kHz. A
 * larger number is a corrupt header, and it would overflow the frame cap and
 * the resampler's sizes.
 */
constexpr uint32_t kMaxFileRate = 1536000;

/**
 * Turns decoded planes into a SampleData.
 *
 * A [targetRate] of zero or less keeps the file's own rate. Multisamples use
 * that since they account for the rate when pitching, and resampling would
 * only lose quality. Everything else resamples, because every machine except
 * Mosaic assumes the engine rate.
 */
inline std::unique_ptr<SampleData> assemble(DecodedAudio &in, const std::string &path, int32_t targetRate,
                                           int32_t maxSeconds = kMaxDecodeSeconds) {
    if (in.frames <= 0) return nullptr;
    const int64_t cap = static_cast<int64_t>(maxSeconds) * in.rate;
    const bool cut = in.frames > cap;
    if (cut) in.frames = static_cast<int32_t>(cap);

    auto out = std::make_unique<SampleData>();
    // Cut here or by the reader.
    out->truncated = cut || in.truncated;
    out->stereo = in.stereo;
    const size_t slash = path.find_last_of('/');
    out->name = slash == std::string::npos ? path : path.substr(slash + 1);

    // Nothing to resample: the caller wants the file's own rate, or the file
    // is already at the target rate (the usual case, most audio is 48 kHz).
    // The planes are moved instead of copied, which saves a full copy of the
    // audio in memory for long files.
    if (targetRate <= 0 || targetRate == in.rate) {
        out->rate = in.rate;
        out->frames = in.frames;
        out->left = std::move(in.ch[0]);
        out->left.resize(static_cast<size_t>(in.frames));
        if (out->stereo) {
            out->right = std::move(in.ch[1]);
            out->right.resize(static_cast<size_t>(in.frames));
        }
        out->measure();
        return out;
    }

    // Linear interpolation. Good enough for drums, and a better one can
    // replace it without changing any callers.
    out->rate = targetRate;
    const double ratio = static_cast<double>(in.rate) / static_cast<double>(targetRate);
    const auto outFrames = static_cast<int32_t>(static_cast<double>(in.frames) / ratio);
    if (outFrames <= 0) return nullptr;
    out->frames = outFrames;
    out->left.resize(static_cast<size_t>(outFrames));
    if (out->stereo) out->right.resize(static_cast<size_t>(outFrames));
    for (int32_t i = 0; i < outFrames; ++i) {
        const double srcPos = i * ratio;
        const auto i0 = static_cast<int32_t>(srcPos);
        const int32_t i1 = i0 + 1 < in.frames ? i0 + 1 : i0;
        const float frac = static_cast<float>(srcPos - i0);
        out->left[static_cast<size_t>(i)] =
            in.ch[0][static_cast<size_t>(i0)] * (1.0f - frac) + in.ch[0][static_cast<size_t>(i1)] * frac;
        if (out->stereo) {
            out->right[static_cast<size_t>(i)] =
                in.ch[1][static_cast<size_t>(i0)] * (1.0f - frac) + in.ch[1][static_cast<size_t>(i1)] * frac;
        }
    }
    out->measure();
    return out;
}

/** Reads the whole file into memory, or returns false with [error] set. [ceiling] is in bytes. */
bool slurp(const std::string &path, std::vector<unsigned char> &bytes, std::string &error,
           size_t ceiling = 64u * 1024u * 1024u);

/** Reads at most [maxBytes] from the start of a file, enough to recognise it. */
bool slurpHead(const std::string &path, std::vector<unsigned char> &bytes, size_t maxBytes, std::string &error);

/** The most a WAV of [maxSeconds] could weigh, as a ceiling for slurp. */
inline size_t slurpCeilingFor(int32_t maxSeconds) {
    // 48 kHz stereo at 24 bits is 288 kB a second, plus half again for other
    // rates and bit depths.
    const size_t bytes = static_cast<size_t>(maxSeconds) * 288u * 1024u * 3u / 2u;
    return bytes < 64u * 1024u * 1024u ? 64u * 1024u * 1024u : bytes;
}

} // namespace acidulous
