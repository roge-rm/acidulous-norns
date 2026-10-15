#pragma once
#include <atomic>
#include <cstdint>

// Engine-wide settings that belong to the device, not the song, such as how
// hard the engine is allowed to work.
//
// Read on the audio thread and written from the UI, so each one is an atomic.
// The defaults match how the app behaved before each setting existed.
namespace acidulous {

struct EngineSettings {
    /**
     * Notes a rack may hold at once, 0 for no limit. Counts held notes, not
     * voices, since releasing voices are cheap and each machine steals its own.
     */
    std::atomic<int32_t> voiceLimit{0};

    /**
     * 1 is full quality. 0 saves CPU on slow phones by lowering the master
     * reverb's density and the distortion's oversampling. It never changes a
     * song's parameters, and never applies to a render (see [offlineRender]).
     */
    std::atomic<int32_t> quality{1};

    /** Bits per sample in a recorded or exported WAV: 24 or 16. */
    std::atomic<int32_t> recordBits{24};

    /**
     * Set while the engine is rendering to a file instead of the speaker.
     *
     * Exports and freezes run `renderBlock` in a loop with the stream stopped,
     * so there's no deadline and they always use full quality. This is a
     * separate flag instead of saving and restoring [quality], because the
     * automatic quality watcher can change [quality] from the UI while a
     * render runs on a worker thread.
     */
    std::atomic<bool> offlineRender{false};

    static EngineSettings &get() {
        static EngineSettings instance;
        return instance;
    }
};

/**
 * True for full quality, and always true while rendering to a file. Every
 * lean branch in the engine should check this.
 */
inline bool fullQuality() {
    const EngineSettings &s = EngineSettings::get();
    return s.offlineRender.load(std::memory_order_relaxed) ||
           s.quality.load(std::memory_order_relaxed) != 0;
}

} // namespace acidulous
