#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <engine/machine/formulate/Expr.h>

// A Formulate program: one formula and three step tables (arpeggio, duty and
// volume), like the frame-rate tables in chip music. Built on a worker thread
// from four strings and handed over as one object.
namespace acidulous::machine::formulate {

/** A step table: values, and where it loops back to when it runs out. */
struct Table {
    std::vector<int32_t> steps;
    int32_t loopFrom = 0;

    bool empty() const { return steps.empty(); }
    /** The value at step [i], following the loop. */
    int32_t at(int64_t i) const {
        if (steps.empty()) return 0;
        const int32_t n = static_cast<int32_t>(steps.size());
        if (i < n) return steps[static_cast<size_t>(i)];
        const int32_t span = n - loopFrom;
        if (span <= 0) return steps[static_cast<size_t>(n - 1)];
        return steps[static_cast<size_t>(loopFrom + (i - loopFrom) % span)];
    }
    /** Whether the table has run past its end and does not loop. */
    bool finished(int64_t i) const { return !steps.empty() && loopFrom >= static_cast<int32_t>(steps.size()) && i >= static_cast<int32_t>(steps.size()); }
};

struct Program {
    Expr formula;
    Table arp;   // semitones
    Table duty;  // 0..255
    Table vol;   // 0..255
};

/**
 * Parse all four. A table is "0 4 7 | 12 -12": numbers, with an optional bar
 * marking where it loops back to. Returns null with a reason on the first
 * error.
 */
std::unique_ptr<Program> compile(const std::string &formula, const std::string &arp,
                                 const std::string &duty, const std::string &vol,
                                 std::string &error);

} // namespace acidulous::machine::formulate
