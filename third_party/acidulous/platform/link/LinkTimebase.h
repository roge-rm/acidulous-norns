#pragma once
#include <atomic>
#include <cstdint>
#include <engine/core/Timebase.h>
#include <memory>

// Ableton Link behind the engine's Timebase.
//
// Only this file and the .cpp know about Link. The engine just sees a tempo
// and a beat, so following can be tested without a network.
//
// Link is header-only and pulls in asio, so ableton::Link is kept behind a
// pimpl. Otherwise every file that includes the engine host would compile
// megabytes of networking headers.
namespace acidulous {

class LinkTimebase final : public Timebase {
  public:
    LinkTimebase();
    ~LinkTimebase() override;

    /** UI thread. Off means the peer discovery sockets are closed. */
    void setEnabled(bool on);
    bool enabled() const;

    /** How many other machines are in the session. UI thread. */
    int32_t peers() const;
    /** The session tempo, or 0 when Link is off. UI thread. */
    double sessionTempo() const;

    /**
     * Beats per bar, from the song's signature. Link's phase is per bar, so
     * without this peers would agree on the beat but not where the bar starts.
     */
    void setQuantum(double beats);

    /**
     * Ties the engine's frame count to the clock: frame [frame] is heard at
     * [nanos] on CLOCK_MONOTONIC, at [rate] Hz. Refreshed from the UI thread.
     * Until there is one, the fallback latency is used.
     *
     * Link needs the time a block will be heard, not when it was computed.
     * Getting that wrong puts us out by the output buffer (20 ms at a
     * 960-frame burst), which is clearly audible against another device.
     */
    void setAnchor(int64_t frame, int64_t nanos, int32_t rate);
    void setFallbackLatency(int64_t micros);

    /** UI thread: send the song's tempo to Link without waiting for a block. */
    void tempoFromApp(double bpm);

    // --- Timebase, on the audio thread --------------------------------------
    State capture(int64_t framesRendered) override;
    void proposeTempo(double bpm) override;
    void proposePlaying(bool playing) override;

    /** Frames in an engine block, for working out where the block ends. */
    void setBlockFrames(int32_t frames, int32_t sampleRate);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace acidulous
