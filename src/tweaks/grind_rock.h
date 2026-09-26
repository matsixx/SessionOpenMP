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
// SessionTweaks -- BOARDSLIDE ROCKING. On a grind that takes both sticks (a boardslide), easing off
// one stick takes pressure off that foot's end of the board: it rises and the other end dips, the way
// easing a tailslide's stick lets the nose drop.
#pragma once
#include <cstddef>
struct OmpMenuApi;
void GrindRock_ReadConfig(const char* iniText);
void GrindRock_SaveConfig(char* iniText, size_t cap);
void GrindRock_ResetDefaults();
void GrindRock_Install();
void GrindRock_PumpFrame();                        // GAME THREAD: eases a leftover tilt out after the grind
void GrindRock_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)

// The pause-menu accessors run on the GAME THREAD (the other menu_ext contract).
bool  GrindRock_Enabled();  void GrindRock_SetEnabled(bool on);
float GrindRock_MaxDeg();   void GrindRock_SetMaxDeg(float deg);
float GrindRock_Softness(); void GrindRock_SetSoftness(float v);   // 0..20, curve = 1 + v/10
float GrindRock_SwayDeg();  void GrindRock_SetSwayDeg(float deg);  // the natural rock, 0..10
