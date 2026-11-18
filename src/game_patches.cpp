// Values of the runtime-selectable game patches (src/game_patches.h).
#include "game_patches.h"

#include <cstdlib>

namespace
{
    bool environmentFlag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && *value && *value != '0';
    }
}

namespace OrochiGamePatches
{
    bool widescreen = environmentFlag("OROCHI_WIDESCREEN");
    bool fixedFieldOffset = !environmentFlag("OROCHI_FIELD_OFFSET");
}
