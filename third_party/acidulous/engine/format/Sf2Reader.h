#pragma once
#include <cstdint>
#include <engine/core/SampleMap.h>
#include <memory>
#include <string>
#include <vector>

// SoundFont 2 reader. It reads what a multisample player needs: presets,
// their instruments, key and velocity zones, root keys, tuning, loops,
// attenuation, pan and the volume envelope. Modulators, chorus and reverb
// sends and anything the engine does itself are ignored.
//
// Only the chosen preset's samples are decoded, so loading one instrument
// from a large bank stays cheap.
namespace acidulous {

class Sf2Reader {
  public:
    struct PresetInfo {
        int32_t bank = 0;
        int32_t preset = 0;
        std::string name;
    };

    // Reads only the headers, so it's cheap enough to fill a picker.
    static bool listPresets(const std::string &path, std::vector<PresetInfo> &out, std::string &error);

    // Builds one preset into a playable map. `presetIndex` indexes the list
    // above, in file order.
    static std::unique_ptr<SampleMap> load(const std::string &path, int32_t presetIndex, std::string &error);

    static constexpr int64_t kMaxFileBytes = 320ll * 1024 * 1024;
    static constexpr int64_t kMaxDecodedFrames = 48000ll * 60 * 12; // twelve minutes of audio
};

} // namespace acidulous
