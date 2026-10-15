#include "AudioDecoder.h"
#include "AiffReader.h"
#include "Decoded.h"
#include "FlacReader.h"
#include "Mp3Reader.h"
#include "WavReader.h"
#include <cstring>

namespace acidulous {

namespace {
/**
 * How much of the start of a file is read to recognise it. Only the head is
 * read since sniff and foreignKind both run before the reader loads the file.
 */
constexpr size_t kHeadBytes = 64 * 1024;
bool head(const std::string &path, std::vector<unsigned char> &bytes) {
    std::string ignored;
    return slurpHead(path, bytes, kHeadBytes, ignored);
}
} // namespace

AudioFormat sniff(const std::string &path) {
    std::vector<unsigned char> bytes;
    if (!head(path, bytes)) return AudioFormat::Unknown;
    const size_t n = bytes.size();
    const unsigned char *b = bytes.data();
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) == 0) return AudioFormat::Wav;
    if (n >= 12 && std::memcmp(b, "FORM", 4) == 0 &&
        (std::memcmp(b + 8, "AIFF", 4) == 0 || std::memcmp(b + 8, "AIFC", 4) == 0)) {
        return AudioFormat::Aiff;
    }
    if (n >= 4 && std::memcmp(b, "fLaC", 4) == 0) return AudioFormat::Flac;
    // Checked before the frame-sync scan below, since these containers hold
    // compressed audio with bytes that look like a sync.
    if (foreignKind(path) != nullptr) return AudioFormat::Unknown;

    // MP3 has no header of its own. Look for a frame sync (eleven bits set)
    // followed by a version and layer that aren't reserved values, otherwise
    // lots of binary files would pass. Skip any ID3 tag first using the same
    // function as the decoder, so both agree on where the audio starts.
    if (n >= 10 && std::memcmp(b, "ID3", 3) == 0 && Mp3Reader::audioStart(b, n) == 0) {
        return AudioFormat::Mp3; // the tag runs past what we read; let the decoder say so
    }
    const size_t at = Mp3Reader::audioStart(b, n);
    for (size_t i = at; i + 1 < n && i < at + 8192; ++i) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        if ((b[i + 1] & 0x18) == 0x08) continue; // reserved MPEG version
        if ((b[i + 1] & 0x06) == 0x00) continue; // reserved layer
        return AudioFormat::Mp3;
    }
    return AudioFormat::Unknown;
}

const char *foreignKind(const std::string &path) {
    std::vector<unsigned char> bytes;
    if (!head(path, bytes)) return nullptr;
    const size_t n = bytes.size();
    const unsigned char *b = bytes.data();
    // MP4 and related formats (AAC in m4a, ALAC, video): a size first, then
    // `ftyp` at offset 4.
    if (n >= 12 && std::memcmp(b + 4, "ftyp", 4) == 0) {
        if (std::memcmp(b + 8, "M4A", 3) == 0) return "an M4A file";
        return "an MP4 file";
    }
    if (n >= 4 && std::memcmp(b, "OggS", 4) == 0) return "an Ogg file";
    if (n >= 4 && std::memcmp(b, "\x1a\x45\xdf\xa3", 4) == 0) return "a Matroska or WebM file";
    if (n >= 4 && std::memcmp(b, "\x30\x26\xb2\x75", 4) == 0) return "a WMA or ASF file";
    if (n >= 4 && std::memcmp(b, "caff", 4) == 0) return "a CAF file";
    if (n >= 4 && std::memcmp(b, ".snd", 4) == 0) return "an AU file";
    if (n >= 4 && std::memcmp(b, "wvpk", 4) == 0) return "a WavPack file";
    if (n >= 4 && std::memcmp(b, "MAC ", 4) == 0) return "a Monkey's Audio file";
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) != 0) {
        return "a RIFF file that is not audio";
    }
    return nullptr;
}

const char *formatName(AudioFormat f) {
    switch (f) {
    case AudioFormat::Wav: return "WAV";
    case AudioFormat::Aiff: return "AIFF";
    case AudioFormat::Flac: return "FLAC";
    case AudioFormat::Mp3: return "MP3";
    default: return "unknown";
    }
}

std::unique_ptr<SampleData> decodeAudio(const std::string &path, int32_t targetRate, std::string &error,
                                        int32_t maxSeconds) {
    const AudioFormat format = sniff(path);
    std::unique_ptr<SampleData> out;
    switch (format) {
    case AudioFormat::Wav: out = WavReader::read(path, targetRate, error, maxSeconds); break;
    case AudioFormat::Aiff: out = AiffReader::read(path, targetRate, error, maxSeconds); break;
    case AudioFormat::Flac: out = FlacReader::read(path, targetRate, error, maxSeconds); break;
    case AudioFormat::Mp3: out = Mp3Reader::read(path, targetRate, error, maxSeconds); break;
    default: {
        // Say what the file is when we can tell.
        const char *kind = foreignKind(path);
        error = kind != nullptr ? std::string(kind) + ", which this cannot read"
                                : "not an audio file this can read";
        return nullptr;
    }
    }
    // Say which format was tried.
    if (!out && !error.empty()) error = std::string(formatName(format)) + ": " + error;
    return out;
}

} // namespace acidulous
