#include "Mp3Reader.h"
#include "Decoded.h"
#include <algorithm>
#include <cstring>
#include <lame.h>

// Decodes MP3 with LAME's mpglib, which ships with the LAME encoder we
// already include. Having `mpglib/` in the build (HAVE_MPGLIB) is what makes
// `hip_decode` work. It's in the same .so under the same LGPL licence.
//
// MP3 is lossy, so a round trip won't come back sample for sample.
namespace acidulous {

namespace {
/**
 * mpglib returns 16-bit samples in pieces of any size.
 *
 * One call in can give several frames out or none, since a frame's data can
 * span several calls. So every push is followed by draining until empty, and
 * the end of the file by one last drain.
 */
constexpr int kOutSamples = 16384; // per channel, which is far more than a frame
} // namespace

namespace {

/**
 * The padding an encoder added at each end, which the decoder should remove.
 *
 * An encoder adds a few hundred silent samples at the front to prime its
 * filter bank and more at the back to fill the last frame. Without removing
 * them a loop starts about 25 ms late. LAME writes both counts into an
 * `Info`/`Xing` frame at the start of the file.
 *
 * The tag sits in the first frame after the side information, whose length
 * depends on the version and on mono or stereo. Four bytes of flags say which
 * optional fields follow, then comes the LAME extension. Delay and padding are
 * 12 bits each, 21 bytes into it.
 *
 * 529 is the decoder's delay, a constant of the format. mpglib, ffmpeg and
 * LAME's frontend all add it to the encoder's number.
 *
 * Leaves both at 0 when there's no tag.
 */
void lameTrim(const unsigned char *b, size_t n, size_t at, int32_t &skip, int32_t &trim) {
    skip = 0;
    trim = 0;
    if (at + 4 > n || b[at] != 0xFFu || (b[at + 1] & 0xE0u) != 0xE0u) return;
    const int version = (b[at + 1] >> 3) & 3;   // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    const bool mono = ((b[at + 3] >> 6) & 3) == 3;
    const size_t side = version == 3 ? (mono ? 17u : 32u) : (mono ? 9u : 17u);
    const size_t xing = at + 4 + side;
    if (xing + 8 > n) return;
    if (std::memcmp(b + xing, "Xing", 4) != 0 && std::memcmp(b + xing, "Info", 4) != 0) return;

    const uint32_t flags = (static_cast<uint32_t>(b[xing + 4]) << 24) |
                           (static_cast<uint32_t>(b[xing + 5]) << 16) |
                           (static_cast<uint32_t>(b[xing + 6]) << 8) | b[xing + 7];
    size_t lame = xing + 8;
    if ((flags & 1u) != 0) lame += 4;   // the frame count
    if ((flags & 2u) != 0) lame += 4;   // the byte count
    if ((flags & 4u) != 0) lame += 100; // the seek table
    if ((flags & 8u) != 0) lame += 4;   // the quality
    if (lame + 24 > n) return;

    const int32_t delay = (static_cast<int32_t>(b[lame + 21]) << 4) | (b[lame + 22] >> 4);
    const int32_t padding = ((static_cast<int32_t>(b[lame + 22]) & 0x0F) << 8) | b[lame + 23];
    // Ignore values that are clearly wrong, or we'd cut the start of the
    // music.
    if (delay < 0 || delay > 3000 || padding < 0 || padding > 3000) return;
    skip = delay + 529;
    trim = padding > 529 ? padding - 529 : 0;
}

} // namespace

size_t Mp3Reader::audioStart(const unsigned char *b, size_t n) {
    if (n < 10 || std::memcmp(b, "ID3", 3) != 0) return 0;
    // A syncsafe length: four bytes of seven bits each, not counting the
    // 10-byte header, plus 10 more if the footer flag is set.
    const size_t size = (static_cast<size_t>(b[6] & 0x7Fu) << 21) | (static_cast<size_t>(b[7] & 0x7Fu) << 14) |
                        (static_cast<size_t>(b[8] & 0x7Fu) << 7) | static_cast<size_t>(b[9] & 0x7Fu);
    const size_t at = 10 + size + ((b[5] & 0x10u) != 0 ? 10u : 0u);
    // If the tag claims to be longer than the file, start at 0 instead.
    return at < n ? at : 0;
}

std::unique_ptr<SampleData> Mp3Reader::read(const std::string &path, int32_t targetRate, std::string &error,
                                           int32_t maxSeconds) {
    std::vector<unsigned char> bytes;
    if (!slurp(path, bytes, error, slurpCeilingFor(maxSeconds))) return nullptr;

    hip_t hip = hip_decode_init();
    if (hip == nullptr) { error = "no decoder"; return nullptr; }

    mp3data_struct info;
    std::memset(&info, 0, sizeof(info));
    std::vector<short> left(kOutSamples), right(kOutSamples);
    DecodedAudio got;
    bool any = false;
    int64_t cap = 0; // set once the rate is known

    const auto take = [&](int samples) {
        if (samples <= 0) return;
        if (!any && info.samplerate > 0) {
            got.rate = info.samplerate;
            got.stereo = info.stereo == 2;
            cap = static_cast<int64_t>(maxSeconds) * got.rate;
            any = true;
        }
        if (!any) return;
        if (static_cast<int64_t>(got.ch[0].size()) + samples > cap) got.truncated = true;
        for (int i = 0; i < samples && static_cast<int64_t>(got.ch[0].size()) < cap; ++i) {
            got.ch[0].push_back(left[static_cast<size_t>(i)] / 32768.0f);
            if (got.stereo) got.ch[1].push_back(right[static_cast<size_t>(i)] / 32768.0f);
        }
    };

    // Fed in pieces and drained after each. A call returns at most one frame,
    // and 0 when it took data without finishing a frame (always the case on
    // the first call), so a loop can't stop at the first 0.
    int errors = 0;
    // Feed 1 KB at a time. mpglib copies input into a 3904-byte buffer
    // (`bsspace[2][MAXFRAMESIZE + 1024]`) and silently drops what doesn't
    // fit, so bigger chunks lose audio or decode nothing at all. LAME's own
    // frontend also uses 1024.
    const size_t chunk = 1024;
    // Drains decoded frames. It also has to run once after the last chunk,
    // or the final frame stays inside the decoder and the file comes back
    // 1152 frames short.
    auto drain = [&](int n) {
        while (n > 0) {
            take(n);
            if (cap > 0 && static_cast<int64_t>(got.ch[0].size()) >= cap) break;
            n = hip_decode1_headers(hip, nullptr, 0, left.data(), right.data(), &info);
        }
        return n;
    };
    // Start where the audio starts, after any tag. See audioStart.
    for (size_t off = audioStart(bytes.data(), bytes.size()); off < bytes.size(); off += chunk) {
        const size_t len = std::min(chunk, bytes.size() - off);
        int n = drain(hip_decode1_headers(hip, bytes.data() + off, len, left.data(), right.data(), &info));
        // Keep going after an error. Real files often have album art, APE or
        // Lyrics3 tags or a cut-off frame, and mpglib returns -1 on those but
        // carries on fine. Whether the file worked is judged at the end by
        // whether any audio came out.
        if (n < 0) ++errors;
        if (cap > 0 && static_cast<int64_t>(got.ch[0].size()) >= cap) break;
    }
    // One last drain for whatever the last chunk left inside.
    if (cap <= 0 || static_cast<int64_t>(got.ch[0].size()) < cap) {
        if (drain(hip_decode1_headers(hip, nullptr, 0, left.data(), right.data(), &info)) < 0) ++errors;
    }
    hip_decode_exit(hip);
    if (!any || got.ch[0].empty()) {
        error = errors > 0 ? "no MPEG audio in it" : "no audio in it";
        return nullptr;
    }

    // Remove the encoder's padding from both ends. See lameTrim.
    {
        int32_t skip = 0, trim = 0;
        lameTrim(bytes.data(), bytes.size(), audioStart(bytes.data(), bytes.size()), skip, trim);
        for (auto &ch : got.ch) {
            if (ch.empty()) continue;
            const auto have = static_cast<int32_t>(ch.size());
            const int32_t front = std::min(skip, have);
            const int32_t back = std::min(trim, have - front);
            if (back > 0) ch.resize(static_cast<size_t>(have - back));
            if (front > 0) ch.erase(ch.begin(), ch.begin() + front);
        }
    }

    got.frames = static_cast<int32_t>(got.ch[0].size());
    if (got.stereo && static_cast<int32_t>(got.ch[1].size()) < got.frames) {
        got.frames = static_cast<int32_t>(got.ch[1].size());
    }
    auto out = assemble(got, path, targetRate, maxSeconds);
    if (!out) error = "empty";
    return out;
}

} // namespace acidulous
