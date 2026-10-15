#include "EffectRegistry.h"
#include "Effects.h"
#include "FromMachines.h"
#include "Acid.h"
#include "Mouth.h"
#include "Formula.h"
#include "Horn.h"
#include "Magneto.h"
#include "Slicer.h"
#include "Smash.h"
#include "Spectral.h"
#include "Tape.h"
#include "amp/Amp.h"
#include <cstring>

namespace acidulous {

namespace {
using Factory = Effect *(*)();
struct Entry { const char *name; Factory make; };
template <typename T> Effect *make() { return new T(); }
const Entry kEntries[] = {
    {"Delay", make<effect::Delay>},
    {"Reverb", make<effect::Reverb>},
    {"Eq", make<effect::Eq>},
    {"Distortion", make<effect::Distortion>},
    {"Compressor", make<effect::Compressor>},
    {"Filter", make<effect::Filter>},
    {"Bitcrusher", make<effect::Bitcrusher>},
    {"Phaser", make<effect::Phaser>},
    {"Flanger", make<effect::Flanger>},
    {"Chorus", make<effect::Chorus>},
    {"Tremolo", make<effect::Tremolo>},
    {"Width", make<effect::Width>},
    {"Shifter", make<effect::Shifter>},
    {"Harmonizer", make<effect::Harmonizer>},
    {"Amp", make<effect::Amp>},
    {"Gate", make<effect::Gate>},
    {"Swell", make<effect::Swell>},
    {"Rotary", make<effect::Rotary>},
    {"Grain", make<effect::Grain>},
    {"Resonator", make<effect::Resonator>},
    {"Smash", make<effect::Smash>},
    {"Acid", make<effect::Acid>},
    {"Mouth", make<effect::Mouth>},
    {"Tape", make<effect::Tape>},
    {"Slicer", make<effect::Slicer>},
    {"Magneto", make<effect::Magneto>},
    {"Horn", make<effect::Horn>},
    {"Spectral", make<effect::Spectral>},
    {"Formula", make<effect::Formula>},
};
constexpr int32_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);

const Entry *find(const char *typeName) {
    if (typeName == nullptr) return nullptr;
    for (const auto &e : kEntries) if (std::strcmp(e.name, typeName) == 0) return &e;
    return nullptr;
}
} // namespace

Effect *EffectRegistry::create(const char *typeName) {
    const Entry *e = find(typeName);
    return e ? e->make() : nullptr;
}

const ParamDef *EffectRegistry::paramDefs(const char *typeName, int32_t &count) {
    // One probe instance per type, built on first use. The tables are static
    // inside each paramDefs(), the probe is just a way to reach them.
    static Effect *probes[kCount] = {};
    for (int32_t i = 0; i < kCount; ++i) {
        if (std::strcmp(kEntries[i].name, typeName ? typeName : "") == 0) {
            if (probes[i] == nullptr) probes[i] = kEntries[i].make();
            return probes[i]->paramDefs(count);
        }
    }
    count = 0;
    return nullptr;
}

int32_t EffectRegistry::count() { return kCount; }
const char *EffectRegistry::name(int32_t index) { return (index >= 0 && index < kCount) ? kEntries[index].name : ""; }

} // namespace acidulous
