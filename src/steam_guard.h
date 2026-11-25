#pragma once

// Returns true if the game is running as a genuine owned Steam copy (AppID 6060).
// If false, outReason explains why.
bool VerifySteamOwnership(bool requireSteam, const char** outReason);
