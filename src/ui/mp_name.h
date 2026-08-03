// SessionOpenMP -- co-op multiplayer for Session, as an overlay on N solo games.
// Copyright (C) 2026 matsix
//
// This program is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version. It is
// distributed WITHOUT ANY WARRANTY; see the GNU GPL (LICENSE) for details.
//
// Additional permission under GNU GPL version 3 section 7: you may link and convey this
// work combined with the Epic Online Services SDK and the proprietary game runtime it
// loads into. See LICENSE-EXCEPTION.txt.
// SessionOpenMP -- the player's multiplayer name.
//
// It rides the EXISTING wire: the cosmetics packet already carries a `skaterName` string, so the
// sender simply publishes THIS name in that field instead of the one off the game instance. No new
// packet, no magic bump. (Deliberately NOT defaulted to the in-game skater name -- that is a private
// profile detail and it should take an explicit choice to broadcast it.)
//
// Names are checked against a word list COMPILED INTO the mod -- a list shipped as a file next to it
// is a filter the player can simply delete. See mp_name.cpp for how matching works and, more
// importantly, for why half of it is whole-word only.
#pragma once

// Longest name that is shown or sent. The cosmetics field is 40 bytes, so this stays well inside it.
enum { OMP_NAME_MAX = 20 };

// `dir` is the folder holding SessionOpenMP.log (the game's Win64 dir), where the chosen name is
// remembered. A saved name is re-validated against the current list on load.
void        MpName_Init(const char* dir, void (*logf)(const char*));
const char* MpName_Get();                    // never null, never empty -- "Skater" until set
// Validate + apply + persist. Returns false and fills `reason` (a short human sentence) if the name
// is too short/long, has unusable characters, or trips the filter. The name is only stored on success.
bool        MpName_Set(const char* candidate, char* reason, int reasonCap);
