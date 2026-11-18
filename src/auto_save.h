#pragma once
class PS2Runtime;

// Autosave (OROCHI_AUTOSAVE=1, set by the setup screen): the end-of-battle
// "Save game data?" is answered by the port, so the game's own save runs
// without the player. Only that flow is touched: any other message, such as a
// full card or an offer to format, stops it and waits for the player.
// OROCHI_AUTOSAVE=all also answers the pause menu's interim save (for testing).
bool installOrochiAutoSave(PS2Runtime &runtime);
