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
// SessionTweaks -- MAX DETAIL: quality beyond the game's own highest settings (see max_detail.cpp).
#pragma once
struct OmpMenuApi;
void MaxDetail_ReadConfig(const char* iniText);
void MaxDetail_SaveConfig(char* iniText, size_t cap);
void MaxDetail_ResetDefaults();
void MaxDetail_PumpFrame();                     // GAME THREAD: applies and holds the console variables
void MaxDetail_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)

// Each group is 0 = the game's own setting, untouched. Above that, ours.
int  MaxDetail_Textures();    void MaxDetail_SetTextures(int v);
int  MaxDetail_Shadows();     void MaxDetail_SetShadows(int v);
int  MaxDetail_Distance();    void MaxDetail_SetDistance(int v);      // per cent of the game's draw distance
int  MaxDetail_Reflections(); void MaxDetail_SetReflections(int v);
int  MaxDetail_Fog();         void MaxDetail_SetFog(int v);
void MaxDetail_ApplyPreset(); // every group to its highest
const char* MaxDetail_Status();
