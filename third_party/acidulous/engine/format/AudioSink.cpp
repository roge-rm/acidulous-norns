#include "AudioSink.h"
#include "AiffWriter.h"
#include "FlacWriter.h"
#include "Mp3Writer.h"
#include "WavWriter.h"

namespace acidulous {

std::unique_ptr<AudioSink> makeSink(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav:
        return std::make_unique<WavWriter>();
    case AudioFormat::Aiff:
        return std::make_unique<AiffWriter>();
    case AudioFormat::Flac:
        return std::make_unique<FlacWriter>();
    case AudioFormat::Mp3:
        return std::make_unique<Mp3Writer>();
    }
    return nullptr;
}

const char *extensionFor(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav:
        return ".wav";
    case AudioFormat::Aiff:
        return ".aiff";
    case AudioFormat::Flac:
        return ".flac";
    case AudioFormat::Mp3:
        return ".mp3";
    }
    return ".wav";
}

bool supportsFloat(AudioFormat format) {
    // FLAC only stores integers, and MP3 has no bit depth at all.
    return format != AudioFormat::Flac && format != AudioFormat::Mp3;
}

} // namespace acidulous
