#include "Mp3Writer.h"

#include <lame.h>

namespace acidulous {

namespace {

/**
 * The output buffer size LAME asks for: 1.25 times the sample count plus
 * 7200 bytes.
 */
int32_t encodeRoom(int32_t frames) { return static_cast<int32_t>(frames * 1.25f) + 7200; }

constexpr int32_t kDefaultBitrate = 256;

} // namespace

Mp3Writer::~Mp3Writer() { close(); }

bool Mp3Writer::open(const std::string &path, int32_t sampleRate, int32_t bits, std::string &error) {
    // Opened "w+b" because on close LAME reads back the first frame to
    // replace it with the Xing header. With "wb" that fails and the file has
    // no duration.
    file = std::fopen(path.c_str(), "w+b");
    if (file == nullptr) {
        error = "couldn't open " + path;
        return false;
    }
    lame_t g = lame_init();
    if (g == nullptr) {
        error = "the MP3 encoder didn't start";
        std::fclose(file);
        file = nullptr;
        return false;
    }
    lame_set_in_samplerate(g, sampleRate);
    lame_set_num_channels(g, 2);
    // "bits" is the bitrate in kbit here. Anything that isn't a valid bitrate
    // (like a bit depth of 24 passed by mistake) uses the default.
    lame_set_brate(g, (bits >= 32 && bits <= 320) ? bits : kDefaultBitrate);
    lame_set_mode(g, JOINT_STEREO);
    // Quality 2 on a 0..9 scale. 0 is barely better and several times slower,
    // and the user is waiting for the export.
    lame_set_quality(g, 2);
    if (lame_init_params(g) < 0) {
        error = "the MP3 encoder refused those settings";
        lame_close(g);
        std::fclose(file);
        file = nullptr;
        return false;
    }
    lame = g;
    frames = 0;
    closed = false;
    return true;
}

void Mp3Writer::write(const float *interleaved, int32_t count) {
    if (lame == nullptr || file == nullptr || count <= 0) return;
    buffer.resize(static_cast<size_t>(encodeRoom(count)));
    // The ieee float version takes -1..1. lame_encode_buffer_float expects
    // -32768..32768 and would give near silence.
    const int written = lame_encode_buffer_interleaved_ieee_float(
        static_cast<lame_t>(lame), interleaved, count, buffer.data(),
        static_cast<int>(buffer.size()));
    if (written > 0) std::fwrite(buffer.data(), 1, static_cast<size_t>(written), file);
    frames += count;
}

bool Mp3Writer::close() {
    if (closed) return true;
    closed = true;
    if (lame != nullptr && file != nullptr) {
        // Write the last frame, then the Xing header with the total length so
        // players can show a duration and seek. It rewrites the first frame,
        // so the file must still be open and seekable.
        buffer.resize(7200);
        const int flushed = lame_encode_flush(static_cast<lame_t>(lame), buffer.data(),
                                              static_cast<int>(buffer.size()));
        if (flushed > 0) std::fwrite(buffer.data(), 1, static_cast<size_t>(flushed), file);
        lame_mp3_tags_fid(static_cast<lame_t>(lame), file);
    }
    if (lame != nullptr) {
        lame_close(static_cast<lame_t>(lame));
        lame = nullptr;
    }
    if (file != nullptr) {
        std::fclose(file);
        file = nullptr;
    }
    return true;
}

} // namespace acidulous
