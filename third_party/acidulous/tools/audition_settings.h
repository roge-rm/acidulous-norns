#pragma once
#include <engine/effect/Effect.h>
#include <engine/machine/Machine.h>
#include <engine/machine/formulate/Expr.h>
#include <engine/machine/nexus/Graph.h>
#include <engine/machine/nexus/Modules.h>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Mounts a Nexus graph. Shared by `audition` and `bank_test` so they can't
// drift apart.
namespace acidulous::audition {

/**
 * Builds the graph a Nexus patch describes and hands it to the machine.
 *
 * Returns false and prints why if the text won't parse. [named] is the set of
 * parameters the patch sets itself. Everything else takes the module's own
 * default, since slot knobs default to zero and an oscillator at level zero
 * makes no sound.
 */
/**
 * An effect's word settings, as the app hands them over: a Formula effect's
 * "formula" is compiled and swapped in. Returns false and prints why if it
 * won't parse.
 */
inline bool applyEffectSettings(Effect *fx, const std::vector<std::pair<std::string, std::string>> &settings) {
    for (const auto &kv : settings) {
        if (kv.first != "formula") continue;
        auto *expr = new machine::formulate::Expr();
        std::string error;
        if (!kv.second.empty() && !machine::formulate::Expr::parse(kv.second, *expr, error)) {
            fprintf(stderr, "formula: %s\n", error.c_str());
            delete expr;
            return false;
        }
        void *back = fx->swapObject(0, expr);
        delete static_cast<machine::formulate::Expr *>(back);
    }
    return true;
}

inline bool mountNexusGraph(Machine *m, const std::string &text, float sampleRate,
                            const std::set<std::string> &named,
                            std::unique_ptr<machine::nexus::Graph> &store) {
    namespace nx = machine::nexus;
    if (text.empty()) return true;
    std::string error;
    nx::Graph *g = nx::Graph::parse(text, sampleRate, error);
    if (g == nullptr) {
        std::fprintf(stderr, "  the graph did not parse: %s\n", error.c_str());
        return false;
    }
    std::vector<float> knobs(static_cast<size_t>(nx::kSlots) * nx::kKnobs, 0.0f);
    g->defaultKnobs(knobs.data());
    int32_t count = 0;
    const ParamDef *defs = m->paramDefs(count);
    for (int32_t i = 0; i < count; ++i) {
        const std::string name = defs[i].name;
        if (name.size() < 6 || name[0] != 's' || name[3] != '_' || name[4] != 'p') continue;
        if (named.count(name) != 0) continue; // the patch said so itself
        const int32_t slot = (name[1] - '0') * 10 + (name[2] - '0');
        const int32_t knob = name[5] - '1';
        if (slot < 0 || slot >= nx::kSlots || knob < 0 || knob >= nx::kKnobs) continue;
        m->params().set(i, defs[i].unmap(knobs[static_cast<size_t>(slot) * nx::kKnobs + knob]));
    }
    m->params().jumpAll();
    // Swap first, then release the old graph. `swapObject` reads the old
    // graph to carry playing voices over, so freeing it first would crash.
    // Engine.cpp does the same.
    m->swapObject(0, g);
    store.reset(g);
    return true;
}

} // namespace acidulous::audition
