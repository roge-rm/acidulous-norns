#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

// Reads a WAV a piece at a time instead of all at once.
//
// WavReader loads the whole file into memory, which is fine for a drum hit
// but too much for a song-length recording. This reads the header once and
// then returns frames [from, from+count) of one channel on request, so memory
// use doesn't grow with the file length.
//
// There's no thread or ring buffer here. It feeds the converter, which writes
// the engine's flat format once so the audio thread can memory-map it. The
// audio thread never reads files.
namespace acidulous {

class WavStream {
  public:
    ~WavStream() { close(); }
    WavStream() = default;
    WavStream(const WavStream &) = delete;
    WavStream &operator=(const WavStream &) = delete;

    /** Reads the header only. Returns false with [error] set if it can't read it. */
    bool open(const std::string &path, std::string &error);
    void close();

    int32_t channels() const { return chans; }
    int32_t rate() const { return srcRate; }
    /** Frames in the file, at the file's own rate. */
    int64_t frames() const { return frameCount; }
    bool isOpen() const { return file != nullptr; }

    /**
     * [count] frames of channel [ch], starting at file frame [from].
     *
     * Reads at the file's own rate. The caller resamples, since it knows how
     * the chunks join up. Returns how many frames were written, which is
     * fewer than [count] only at the end of the file.
     */
    int64_t read(int64_t from, int32_t ch, float *out, int64_t count);

  private:
    std::FILE *file = nullptr;
    int64_t dataOffset = 0;
    int64_t frameCount = 0;
    int32_t chans = 0;
    int32_t srcRate = 0;
    int32_t bytesPerSample = 0;
    bool floatFormat = false;
};

} // namespace acidulous
