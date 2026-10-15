#pragma once
#include <cstdint>
#include <string>

// Converts a take too long to hold in memory into a file the engine can map.
//
// Kept separate from EngineHost so tools/bias_test.sh can test it on its own:
// it converts a wav, maps it and checks it reads the same as the normal reader.
namespace acidulous::audio {

struct ReelCache {
    /**
     * Converts [path] into [dest] as planar int16 at the engine rate.
     *
     * Returns the frame count written, or 0 with the reason in [error]. Works
     * in chunks, so memory use doesn't grow with the length of the file.
     */
    static int64_t convert(const std::string &path, const std::string &dest, bool &stereoOut,
                           std::string &error);

    /**
     * The converted file's name: a hash of the source path, size and modified
     * time. Re-importing the same file reuses the conversion, and a file
     * edited in place gets a new one.
     */
    static std::string nameFor(const std::string &path);
};

} // namespace acidulous::audio
