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
// SessionTweaks -- flip (kickflip/heelflip) speed from how fast the stick was actually flicked,
// on a return-value substitution over FlipTricksHandler::GetBoardFlipSpeedMultiplier.
#pragma once
struct OmpMenuApi;
void FlipSpeed_ReadConfig(const char* iniText);
void FlipSpeed_SaveConfig(char* iniText, size_t cap);
void FlipSpeed_Install();
void FlipSpeed_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
// GAME THREAD (pause-menu callbacks run inside engine menu code):
void FlipSpeed_ResetDefaults();
bool FlipSpeed_Enabled();
void FlipSpeed_SetEnabled(bool on);
// The game's own stance answer for a skater -- this module resolved IsSkatingGoofy/IsSkatingSwitch
// for its input normalisation, and the pop remap needs the same truth for its context gate. False =
// the resolvers are unavailable (sig miss); outputs untouched then.
bool FlipSpeed_Stance(void* skater, bool* goofy, bool* switchStance);
// The flick-speed range, in stick-units/second x100 -- integers, so they read correctly on a
// pause-menu slider (which prints "%d").
float FlipSpeed_VelMin();  void FlipSpeed_SetVelMin(float v);
float FlipSpeed_VelMax();  void FlipSpeed_SetVelMax(float v);
// The trick most recently selected (for logs that have no trick of their own).
const char* FlipSpeed_LastTrickName();
// Ticks once per trick SELECTION, so a watcher can detect a fresh trick without the catch path.
long FlipSpeed_TrickSerial();
// Milliseconds since the last trick selection (-1 = none yet) -- catch-engage latency probing.
int FlipSpeed_MsSinceTrick();
