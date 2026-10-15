#pragma once
#include <cstdint>
#include <engine/core/Params.h>
#include <engine/effect/Effect.h>

// Effect types by name, for mounting and for describing parameters to the UI.
namespace acidulous {

class EffectRegistry {
  public:
    static Effect *create(const char *typeName);
    static const ParamDef *paramDefs(const char *typeName, int32_t &count);
    static int32_t count();
    static const char *name(int32_t index);
};

} // namespace acidulous
