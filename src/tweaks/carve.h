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
// SessionTweaks -- CARVING: the board's lean builds into a turn with weight and flows back out, instead
// of snapping to the trigger.
#pragma once
#include <cstddef>

struct OmpMenuApi;

void  Carve_ReadConfig(const char* iniText);
void  Carve_SaveConfig(char* iniText, size_t cap);
void  Carve_ResetDefaults();
void  Carve_Install();
void  Carve_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)

bool  Carve_Enabled();      void Carve_SetEnabled(bool on);
float Carve_LeanMs();       void Carve_SetLeanMs(float ms);      // time to lean all the way in
float Carve_CurvePct();     void Carve_SetCurvePct(float pct);   // trigger response, 100 = as pulled
float Carve_FlowPct();      void Carve_SetFlowPct(float pct);    // damping of the lean, lower = more swing
