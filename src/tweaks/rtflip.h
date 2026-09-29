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
//
// SessionTweaks -- REAL-TIME FLIPS (test): after a flick pop, the other stick spins the board live.
#pragma once
// REAL-TIME FLIPS are disabled and hidden for now: no menu, the ini cannot turn them on, no hooks installed.
// 1 brings them back.
#define TWK_RTFLIP 0
#include <cstddef>

struct OmpMenuApi;

void  RtFlip_ReadConfig(const char* iniText);
void  RtFlip_SaveConfig(char* iniText, size_t cap);
void  RtFlip_ResetDefaults();
void  RtFlip_Install();
void* RtFlip_TrickDef();                                         // our flip in the air: its kickflip/heelflip, else null                                          // the body: the SetFlipTrick hook
void  RtFlip_TickSticks(float* sticks);                            // scoop_speed's tick hook, before the game's tick
float RtFlip_BoardRoll(void* boardMoveComp);                       // added to the board's target roll (grind_rock's read)
float RtFlip_BoardPitch(void* boardMoveComp);                      // added to its target pitch: the flick's pitch
void  RtFlip_AddOffset(void* anim, float dt, float dL[3], float dR[3]);   // foot_place's detour: the feet (kick, tuck, catch)
void  RtFlip_PreAnchors(void* anim);                               // before the game's foot update: our rotations back out
void  RtFlip_PreSteer(void* anim, float dL[3], float dR[3]);     // after it, before foot_steer: the deck's spin out (foot control)
void  RtFlip_DrawMenu(const OmpMenuApi* api);
