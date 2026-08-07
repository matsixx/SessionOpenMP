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
// SessionTweaks -- CLOTH/WIND recon probe. Read-only, on-demand dump of everything that decides
// whether clothes can be made to move: the skater's merged mesh and every prop component (clothing
// sim data, physics assets/bodies), each material slot's instance -> parent chain with its scalar/
// vector/texture/static-switch parameters (the wind wobble some shirts show is material-side; two
// dumps -- windy shirt vs plain -- name the mechanism), plus a global census of loaded clothing
// assets, material parameter collections and wind sources. Writes nothing to the game.
#pragma once
struct OmpMenuApi;
void ClothPhys_ReadConfig(const char* iniText);
void ClothPhys_SaveConfig(char* iniText, size_t cap);
void ClothPhys_Install();
void ClothPhys_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
void ClothPhys_PumpFrame();                       // GAME THREAD: runs a requested dump
void ClothPhys_ResetDefaults();
