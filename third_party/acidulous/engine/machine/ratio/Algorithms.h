#pragma once
#include <cstdint>

// Ratio's algorithm table. An algorithm is just a routing: which operator
// feeds which, and which are heard. They're stored as edge lists so two can be
// blended (see Ratio's morph).
//
// Operators are numbered 1..6 in the names and indexed 0..5 in the edges.
namespace acidulous::machine::ratio {

struct Algorithm {
    const char *name;
    uint8_t carriers;      // bit i set: operator i+1 reaches the output
    uint8_t edgeCount;
    uint8_t edges[10][2];  // {source, destination}, 0-based
};

constexpr int kAlgorithmCount = 32;

// A carrier mask from operator numbers, for readability below.
constexpr uint8_t car(int a, int b = 0, int c = 0, int d = 0, int e = 0, int f = 0) {
    return static_cast<uint8_t>((a ? 1 << (a - 1) : 0) | (b ? 1 << (b - 1) : 0) | (c ? 1 << (c - 1) : 0) |
                                (d ? 1 << (d - 1) : 0) | (e ? 1 << (e - 1) : 0) | (f ? 1 << (f - 1) : 0));
}

constexpr Algorithm kAlgorithms[kAlgorithmCount] = {
    // --- Single chains, deepest first
    {"6-5-4-3-2-1", car(1), 5, {{5, 4}, {4, 3}, {3, 2}, {2, 1}, {1, 0}}},
    {"6-5-4-3-2 1", car(1, 2), 4, {{5, 4}, {4, 3}, {3, 2}, {2, 1}}},
    {"6-5-4-3 2-1", car(3, 1), 4, {{5, 4}, {4, 3}, {3, 2}, {1, 0}}},
    {"6-5-4 3-2-1", car(4, 1), 4, {{5, 4}, {4, 3}, {2, 1}, {1, 0}}},
    {"6-5 4-3-2-1", car(5, 1), 4, {{5, 4}, {3, 2}, {2, 1}, {1, 0}}},

    // --- Two and three parallel stacks
    {"6-5-4 : 3-2-1", car(4, 1), 4, {{5, 4}, {4, 3}, {2, 1}, {1, 0}}},
    {"6-5 : 4-3 : 2-1", car(5, 3, 1), 3, {{5, 4}, {3, 2}, {1, 0}}},
    {"6-5-4-3 : 2-1", car(3, 1), 4, {{5, 4}, {4, 3}, {3, 2}, {1, 0}}},
    {"6-4 5-4 4-3-2-1", car(1), 5, {{5, 3}, {4, 3}, {3, 2}, {2, 1}, {1, 0}}},
    {"6-5-3 4-3 3-2-1", car(1), 5, {{5, 4}, {4, 2}, {3, 2}, {2, 1}, {1, 0}}},

    // --- One modulator, many carriers: the bell and organ shapes
    {"6 - 1,2,3,4,5", car(1, 2, 3, 4, 5), 5, {{5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}}},
    {"6,5 - 1,2,3,4", car(1, 2, 3, 4), 8, {{5, 0}, {5, 1}, {5, 2}, {5, 3}, {4, 0}, {4, 1}, {4, 2}, {4, 3}}},
    {"6-5 - 1,2,3,4", car(1, 2, 3, 4), 5, {{5, 4}, {4, 0}, {4, 1}, {4, 2}, {4, 3}}},
    {"5,6 - 4 : 3 - 1,2", car(1, 2, 4), 4, {{5, 3}, {4, 3}, {2, 0}, {2, 1}}},

    // --- Many modulators, one carrier
    {"2,3,4,5,6 - 1", car(1), 5, {{5, 0}, {4, 0}, {3, 0}, {2, 0}, {1, 0}}},
    {"4,5,6 - 1 : 2-1 3-1", car(1), 5, {{5, 0}, {4, 0}, {3, 0}, {2, 0}, {1, 0}}},
    {"5-4 6-4 4-1 3-2 2-1", car(1), 5, {{4, 3}, {5, 3}, {3, 0}, {2, 1}, {1, 0}}},
    {"6-5 5-2 4-3 3-2 2-1", car(1), 5, {{5, 4}, {4, 1}, {3, 2}, {2, 1}, {1, 0}}},

    // --- Pairs
    {"6-5 : 4-3 : 2-1 (wide)", car(5, 3, 1), 3, {{5, 4}, {3, 2}, {1, 0}}},
    // edgeCount is the loop bound and must match the list, otherwise the
    // unused {0, 0} entries become an edge from operator 1 to itself.
    {"6-5 4-5 : 3-2 : 1", car(5, 3, 1), 3, {{5, 4}, {3, 4}, {2, 1}}},
    {"6-4 5-3 : 2-1", car(4, 3, 1), 3, {{5, 3}, {4, 2}, {1, 0}}},
    {"6-3 5-2 4-1", car(1, 2, 3), 3, {{5, 2}, {4, 1}, {3, 0}}},

    // --- Wide and additive
    {"all six", car(1, 2, 3, 4, 5, 6), 0, {}},
    {"5 carriers, 6 free", car(1, 2, 3, 4, 5), 0, {}},
    {"6-1 : 2,3,4,5", car(1, 2, 3, 4, 5), 1, {{5, 0}}},
    {"6-5-4 : 3 : 2 : 1", car(4, 3, 2, 1), 2, {{5, 4}, {4, 3}}},

    // --- Cross-fed, where a morph has somewhere to go
    {"ring: 6-5-4-3-2-1-6", car(1), 6, {{5, 4}, {4, 3}, {3, 2}, {2, 1}, {1, 0}, {0, 5}}},
    {"6-5 5-4 4-3 3-2 2-1 6-1", car(1), 6, {{5, 4}, {4, 3}, {3, 2}, {2, 1}, {1, 0}, {5, 0}}},
    {"fan in 3, fan out 3", car(1, 2, 3), 9, {{5, 0}, {5, 1}, {5, 2}, {4, 0}, {4, 1}, {4, 2}, {3, 0}, {3, 1}, {3, 2}}},
    {"6-2 6-4 5-1 5-3", car(1, 2, 3, 4), 4, {{5, 1}, {5, 3}, {4, 0}, {4, 2}}},
    {"6-5-4-2 3-2 2-1", car(1), 5, {{5, 4}, {4, 3}, {3, 1}, {2, 1}, {1, 0}}},
    {"6-5-1 4-3-1 2-1", car(1), 5, {{5, 4}, {4, 0}, {3, 2}, {2, 0}, {1, 0}}},
};

} // namespace acidulous::machine::ratio
