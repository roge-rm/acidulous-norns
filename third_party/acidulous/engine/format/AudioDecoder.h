#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/format/AudioSink.h>
#include <engine/format/Decoded.h>
#include <memory>
#include <string>

// Reads every audio file that comes in: WAV, AIFF, FLAC and MP3. The format
// is detected from the file's first bytes, not its extension, since
// downloaded files are often named wrongly.
//
// Import decodes once and writes a WAV into the sample folder, so machines
// and harnesses only ever deal with WAV and nothing is decoded twice.
namespace acidulous {

/** The file's format, from its first bytes. */
AudioFormat sniff(const std::string &path);

/** The name to show a player: "WAV", "AIFF", "FLAC", "MP3". */
const char *formatName(AudioFormat f);

/**
 * A name for a file we recognise but can't read, or null.
 *
 * An m4a is full of byte pairs that look like an MPEG frame sync, so without
 * this it would be reported as a broken mp3. It's checked before the mp3 scan
 * for that reason.
 */
const char *foreignKind(const std::string &path);

/**
 * Decodes any of the four. [targetRate] behaves as in WavReader::read.
 *
 * Null on failure with [error] set. The error starts with the detected
 * format so it makes sense for non-WAV files.
 *
 * [maxSeconds] is how much of a long file to take: kMaxDecodeSeconds for a
 * pad sample, kMaxSliceSeconds for the one file a whole machine slices.
 */
std::unique_ptr<SampleData> decodeAudio(const std::string &path, int32_t targetRate, std::string &error,
                                        int32_t maxSeconds = kMaxDecodeSeconds);

} // namespace acidulous
