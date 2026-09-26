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
// Board stance -- where your feet sit on the deck while you ride.
//
// The game's rolling idle parks the feet in one authored place (the back foot up on the tail). This
// moves each foot from there, in the DECK's own frame, only while the animation is in that idle, so
// a rider can set the stance they actually roll in without touching setups, manuals, catches or
// landings. It owns no hook: the write is foot_place's UpdateFootAnchors post-hook, the same single
// socket write the sole lift and foot_steer go through.
#pragma once
#include <cstddef>
struct OmpMenuApi;

void Stance_ReadConfig(const char* iniText);
void Stance_SaveConfig(char* buf, size_t cap);
void Stance_ResetDefaults();
void Stance_DrawMenu(const OmpMenuApi* api);      // RENDER thread

// Called from foot_place's hook, once per frame for OUR skater, with the deltas it is about to add
// to the foot sockets. `riding` is that module's own gate (on the board, grounded, no catch); the
// finer gate -- only the rolling idle -- is this module's.
void Stance_AddOffset(void* anim, bool riding, float dt, float dL[3], float dR[3]);

// Called from the same hook BEFORE the game's UpdateFootAnchors runs: takes our rotation back out
// of the foot sockets, because the game slerps from their previous value.
void Stance_PreAnchors(void* anim);

// From sit.cpp's FlipEditableSpaceBases seam, for every mesh: in nollie and fakie, OUR skater's toe joints
// go to the regular idle's bend (the socket does not reach that joint).
void Stance_OnFlip(void* mesh);

// The landing lean on the trucks, added to the board's rotation read (deg; 0 = none). grind_rock's hook.
float Stance_LandingRoll(void* boardMoveComp);
