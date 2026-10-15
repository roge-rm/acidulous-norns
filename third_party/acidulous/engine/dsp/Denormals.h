#pragma once

// Flush-to-zero for the threads that do DSP.
//
// Denormals are tiny floats that most CPUs handle very slowly, and every
// decaying tail (reverb, delay feedback, resonators, envelope releases) ends
// in them. That causes CPU spikes when several voices finish at once. Setting
// the flag on the thread makes the hardware flush them to zero everywhere, so
// there's no need for `dsp::undenormal` guards all over the code.
//
// Set it on every thread that renders, not just the audio thread, so an
// offline export matches live playback exactly.
#include <cstdint>

namespace acidulous::dsp {

/**
 * Turns on flush-to-zero for the calling thread. Cheap and safe to repeat.
 *
 * Returns false on targets that don't have the hardware mode.
 */
inline bool flushDenormals() {
#if defined(__aarch64__)
    // FPCR bit 24 is FZ. AArch64 has no DAZ bit, FZ covers both inputs and
    // results.
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    return true;
#elif defined(__arm__)
    uint32_t fpscr = 0;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr));
    return true;
#elif defined(__SSE__) || defined(__x86_64__)
    // x86 needs both: FTZ flushes results and DAZ reads denormal inputs as zero.
    uint32_t csr = __builtin_ia32_stmxcsr();
    csr |= 0x8040u; // FTZ (1 << 15) | DAZ (1 << 6)
    __builtin_ia32_ldmxcsr(csr);
    return true;
#else
    return false;
#endif
}

/** Sets it once per thread, for somewhere that is called often. */
inline void flushDenormalsOnce() {
    static thread_local bool done = false;
    if (!done) {
        done = true;
        flushDenormals();
    }
}

} // namespace acidulous::dsp
