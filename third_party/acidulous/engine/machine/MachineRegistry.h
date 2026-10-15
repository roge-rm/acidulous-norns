#pragma once
#include <cstdint>
#include "Machine.h"
#include <cstring>

// Every machine the app knows, by type name. The document stores the name.
namespace acidulous {

class MachineRegistry {
  public:
    static Machine *create(const char *typeName);
    static const ParamDef *paramDefs(const char *typeName, int32_t &count);
    static int32_t count();
    static const char *name(int32_t index);
};

} // namespace acidulous
