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
// SessionTweaks -- scoop (shove-it) speed from how fast the stick was actually swept. Both sticks
// are tracked, so nollie/switch work too. Two hooks: InputHandler::Tick (raw stick tracker,
// gesture-scoped) and a return-value substitution on
// FlipTricksHandler::GetBoardRotationSpeedMultiplier.
#pragma once
struct OmpMenuApi;
void ScoopSpeed_ReadConfig(const char* iniText);  // parse this module's keys + echo them
void ScoopSpeed_SaveConfig(char* iniText, size_t cap);  // write the live values back into the text
void ScoopSpeed_Install();                        // sig-scan + hook; per-symbol non-fatal
void ScoopSpeed_DrawMenu(const OmpMenuApi* api);  // RENDER THREAD (menu_ext contract)
// The accessors below back the pause-menu page and run on the GAME THREAD (the other menu_ext
// contract): the pause menu's callbacks run inside the engine's own menu code, not the render
// thread. Restore this module's shipped defaults:
void ScoopSpeed_ResetDefaults();
bool ScoopSpeed_Enabled();
void ScoopSpeed_SetEnabled(bool on);
// The gesture-speed range, in deg/s -- integers, so they read correctly on a pause-menu slider.
// Radial (flick) measurement off the SAME InputHandler::Tick samples this module already owns --
// only one detour may exist on that address, so flip_speed is FED from here rather than hooking it.
// Returns false when there is no live input tick to read.
bool ScoopSpeed_FlickMeasure(bool rightStick, float windowSec, float* outSpeed, float* outPeak,
                             float* outFrameMs);
// Where the push is considered to start, and how far out a gesture must reach to count as a flick.
// Live so they can be tuned against real samples rather than guessed at.
extern float ScoopSpeed_FlickStartMag, ScoopSpeed_FlickMinPeak;
// Log the raw magnitude samples behind the last `windowSec`.
void ScoopSpeed_DumpFlick(bool rightStick, float windowSec);

float ScoopSpeed_VelMin();  void ScoopSpeed_SetVelMin(float v);
float ScoopSpeed_VelMax();  void ScoopSpeed_SetVelMax(float v);
