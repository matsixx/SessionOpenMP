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
// SessionTweaks -- SINGLE-FOOT GRIND LEAN. On a nosegrind, crook, 5-0, tailslide... pushing the held
// stick a little to the side leans the rider's weight that way, through the game's own body banking
// (the lean it uses when you carve, which it switches off on grinds).
#pragma once
#include <cstddef>
struct OmpMenuApi;
void GrindLean_ReadConfig(const char* iniText);
void GrindLean_SaveConfig(char* iniText, size_t cap);
void GrindLean_ResetDefaults();
void GrindLean_Install();
void GrindLean_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
// Called from grind_rock's PhysGrinding hook every physics step on a grind (it owns that hook).
void GrindLean_OnPhysGrinding(void* boardMoveComp, float dt);

// The pause-menu accessors run on the GAME THREAD (the other menu_ext contract).
bool  GrindLean_Enabled();   void GrindLean_SetEnabled(bool on);
float GrindLean_Strength();  void GrindLean_SetStrength(float pct);
float GrindLean_RollPct();   void GrindLean_SetRollPct(float pct);   // % of your trucks' own max roll
float GrindLean_TruckGive(); void GrindLean_SetTruckGive(float pct); // the grinding truck's spring give at a full lean
float GrindLean_Friction();  void GrindLean_SetFriction(float pct);  // how much the lean digs in / rides light
// The board roll grind_rock's GetLocalAnimatorBoardQuat hook adds for the read (deg; 0 = none).
float GrindLean_BoardRoll(void* boardMoveComp);
float GrindLean_BoardPitch(void* boardMoveComp);  // 50-50 style foot pressure tilt (deg, +0x620 sense), 0 when none
float GrindLean_TruckWall(void* boardMoveComp);   // the trucks' own max lean (deg), 0 if unavailable
