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
// SessionTweaks -- FIRST PERSON while skating (see cam_fp.cpp). Driven from the camera module.
#pragma once
#include <cstddef>
struct OmpMenuApi;
void CamFp_ReadConfig(const char* iniText);
void CamFp_SaveConfig(char* iniText, size_t cap);
void CamFp_ResetDefaults();
void CamFp_Install();                            // sig-scan; non-fatal
void CamFp_DrawMenu(const OmpMenuApi* api);      // RENDER THREAD (menu_ext contract)
bool CamFp_Enabled();   void CamFp_SetEnabled(bool on);
// GAME THREAD, from the camera actor's Tick (after the game's own): the first-person view this frame.
// `actorQ` is the game camera's rotation, which the view takes its direction from. Out: the eye (world),
// the look (world FQuat, level horizon), the dolly weight 0..1 and the wanted FOV (0 = the game's).
// False = nothing to apply.
bool CamFp_View(const float actorQ[4], float dt, float eye[3], float look[4], float* weight, float* fov);
