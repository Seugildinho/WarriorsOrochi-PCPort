#pragma once
// Runtime choices for config/game_patches.json: tools/apply_game_patches.py
// rewrites the patched instructions in the generated code to read these.

namespace OrochiGamePatches
{
    // 16:9 (OROCHI_WIDESCREEN=1): wider horizontal FOV, enemies, names and
    // health bars drawn to the edges. Needs a 16:9 display (PCSX2 renderer).
    extern bool widescreen;
    // The game draws odd fields half a line lower for interlaced TVs; on a
    // progressive display that shakes the picture. On unless OROCHI_FIELD_OFFSET=1.
    extern bool fixedFieldOffset;
}
