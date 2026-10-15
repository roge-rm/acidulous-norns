#pragma once
#include "Math.h"
#include <cstdint>
#include <vector>

// A circular buffer with a fractional read. Allocated in prepare().
namespace acidulous::dsp {

class DelayLine {
  public:
    void prepare(int32_t maxSamples) { buf.assign(static_cast<size_t>(maxSamples < 2 ? 2 : maxSamples), 0.0f); wr = 0; }
    // Resets the write head as well as the buffer, so a used line ends up the
    // same as a freshly prepared one.
    void clear() {
        for (auto &v : buf) v = 0.0f;
        wr = 0;
    }
    int32_t capacity() const { return static_cast<int32_t>(buf.size()); }
    void write(float v) { buf[static_cast<size_t>(wr)] = v; if (++wr >= capacity()) wr = 0; }
    /**
     * Reads `samples` back from the write position, 1 <= samples < capacity.
     *
     * Always wrap with `wrappedReadIndex` rather than subtracting here. Doing
     * it by hand is easy to get wrong and can read past the end of the buffer
     * (see the note on that function).
     */
    float read(float samples) const {
        const int32_t cap = capacity();
        if (samples < 1.0f) samples = 1.0f;
        if (samples > static_cast<float>(cap - 2)) samples = static_cast<float>(cap - 2);
        float frac = 0.0f;
        const int32_t i0 = wrappedReadIndex(wr, samples, cap, frac);
        const int32_t i1 = i0 + 1 >= cap ? 0 : i0 + 1;
        return buf[static_cast<size_t>(i0)] * (1.0f - frac) + buf[static_cast<size_t>(i1)] * frac;
    }

  private:
    std::vector<float> buf;
    int32_t wr = 0;
};

} // namespace acidulous::dsp
