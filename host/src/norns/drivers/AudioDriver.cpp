#include "AudioDriver.h"

#include <thread>

namespace {
// The started driver, and whether pull() is inside its callback. Sequentially
// consistent, so stop() either sees the pull busy or the pull sees it gone.
std::atomic<AudioDriver *> sLive{nullptr};
std::atomic<bool> sBusy{false};
std::atomic<int64_t> sBlocks{0};
} // namespace

bool AudioDriver::start() {
    if (!callback) return false;
    attached.store(true);
    sLive.store(this);
    return true;
}

void AudioDriver::stop() {
    if (!attached.exchange(false)) return;
    AudioDriver *me = this;
    sLive.compare_exchange_strong(me, nullptr);
    while (sBusy.load()) std::this_thread::yield();
}

bool AudioDriver::pull(float *in, float *out) {
    sBusy.store(true);
    AudioDriver *d = sLive.load();
    const bool ok = d != nullptr;
    if (ok) {
        d->callback(in, out, acidulous::kBlockFrames);
        sBlocks.fetch_add(1, std::memory_order_release);
    }
    sBusy.store(false);
    return ok;
}

int64_t AudioDriver::blocks() { return sBlocks.load(std::memory_order_acquire); }
