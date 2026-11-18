#pragma once
class PS2Runtime;

// More characters drawn in full (OROCHI_CROWD=<count>, set by the Setup's
// "Distância de visão"): the game gives bodies only to the nearest 51 or
// fewer officers and soldiers, so farther ones showed just a health bar.
// Values up to the game's own maximum leave it unchanged.
bool installOrochiDrawDistance(PS2Runtime &runtime);
