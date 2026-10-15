#include "Capture.h"
#include "Settings.h"
#include <engine/format/WavWriter.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace acidulous {

namespace {
// Four seconds of stereo slack, so a filesystem stall doesn't drop audio.
constexpr int64_t kRingFrames = 48000 * 4;
} // namespace

bool Capture::start(const std::string &path, int32_t sampleRate, Source source, std::string &error) {
    if (running.load(std::memory_order_acquire)) {
        error = "already recording";
        return false;
    }
    // Check the file can be written before the audio thread starts pushing.
    WavWriter probe;
    const int32_t bits = EngineSettings::get().recordBits.load(std::memory_order_relaxed);
    if (!probe.open(path, sampleRate, bits, error)) return false;
    probe.close();

    outPath = path;
    rate = sampleRate;
    depth = bits;
    which = source;
    ring.assign(static_cast<size_t>(kRingFrames) * 2, 0.0f);
    writeIndex.store(0, std::memory_order_relaxed);
    readIndex.store(0, std::memory_order_relaxed);
    written.store(0, std::memory_order_relaxed);
    peakLevel.store(0.0f, std::memory_order_relaxed);
    overflow.store(false, std::memory_order_relaxed);
    wasDeaf.store(false, std::memory_order_relaxed);
    if (worker.joinable()) worker.join(); // a writer that stopped itself (see stop)
    running.store(true, std::memory_order_release);
    worker = std::thread([this] { drain(); });
    return true;
}

void Capture::stop() {
    running.store(false, std::memory_order_release);
    // Joined even when it wasn't running: a writer that couldn't open its
    // file stops itself, and a thread that's never joined ends the app when
    // the next take assigns over it.
    if (worker.joinable()) worker.join();
}

void Capture::push(const float *interleaved, int32_t frames) {
    if (!running.load(std::memory_order_acquire)) return;
    const int64_t w = writeIndex.load(std::memory_order_relaxed);
    const int64_t r = readIndex.load(std::memory_order_acquire);
    const int64_t space = kRingFrames - (w - r);
    const int32_t n = static_cast<int32_t>(std::min<int64_t>(frames, space));
    if (n < frames) overflow.store(true, std::memory_order_relaxed);
    float peak = peakLevel.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < n; ++i) {
        const int64_t slot = (w + i) % kRingFrames;
        const float l = interleaved[static_cast<size_t>(i) * 2];
        const float rr = interleaved[static_cast<size_t>(i) * 2 + 1];
        ring[static_cast<size_t>(slot) * 2] = l;
        ring[static_cast<size_t>(slot) * 2 + 1] = rr;
        peak = std::fmax(peak, std::fmax(std::fabs(l), std::fabs(rr)));
    }
    peakLevel.store(peak, std::memory_order_relaxed);
    writeIndex.store(w + n, std::memory_order_release);
}

void Capture::pushSilence(int32_t frames) {
    if (!running.load(std::memory_order_acquire)) return;
    wasDeaf.store(true, std::memory_order_relaxed);
    const int64_t w = writeIndex.load(std::memory_order_relaxed);
    const int64_t r = readIndex.load(std::memory_order_acquire);
    const int64_t space = kRingFrames - (w - r);
    const int32_t n = static_cast<int32_t>(std::min<int64_t>(frames, space));
    if (n < frames) overflow.store(true, std::memory_order_relaxed);
    for (int32_t i = 0; i < n; ++i) {
        const int64_t slot = (w + i) % kRingFrames;
        ring[static_cast<size_t>(slot) * 2] = 0.0f;
        ring[static_cast<size_t>(slot) * 2 + 1] = 0.0f;
    }
    writeIndex.store(w + n, std::memory_order_release);
}

void Capture::drain() {
    WavWriter writer;
    std::string error;
    if (!writer.open(outPath, rate, depth, error)) {
        running.store(false, std::memory_order_release);
        return;
    }
    std::vector<float> chunk(4096 * 2);
    while (true) {
        const bool live = running.load(std::memory_order_acquire);
        const int64_t w = writeIndex.load(std::memory_order_acquire);
        int64_t r = readIndex.load(std::memory_order_relaxed);
        if (r >= w) {
            if (!live) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const int32_t n = static_cast<int32_t>(std::min<int64_t>(w - r, 4096));
        for (int32_t i = 0; i < n; ++i) {
            const int64_t slot = (r + i) % kRingFrames;
            chunk[static_cast<size_t>(i) * 2] = ring[static_cast<size_t>(slot) * 2];
            chunk[static_cast<size_t>(i) * 2 + 1] = ring[static_cast<size_t>(slot) * 2 + 1];
        }
        writer.write(chunk.data(), n);
        r += n;
        readIndex.store(r, std::memory_order_release);
        written.store(writer.framesWritten(), std::memory_order_relaxed);
    }
    writer.close();
    written.store(writer.framesWritten(), std::memory_order_relaxed);
}

} // namespace acidulous
