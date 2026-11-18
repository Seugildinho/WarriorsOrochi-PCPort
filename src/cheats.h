#pragma once
class PS2Runtime;

// Cheats chosen in the Setup program. OROCHI_CHEATS lists them, comma
// separated: invincible, life, musou, points. Empty or unset: none.
bool installOrochiCheats(PS2Runtime &runtime);
