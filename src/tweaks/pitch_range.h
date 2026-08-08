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
// SessionTweaks -- BOARD PITCH RANGE. Stock board pitch saturates at ~35 deg of flick elevation, so
// most of the stick's throw does nothing. This widens the range the pitch is spread over WITHOUT
// touching input recognition. See the .cpp for why the two can be separated.
#pragma once
#include <cstdint>
struct OmpMenuApi;
void  PitchRange_ReadConfig(const char* iniText);
void  PitchRange_SaveConfig(char* iniText, size_t cap);
void  PitchRange_ResetDefaults();
void  PitchRange_Install();
void  PitchRange_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
bool  PitchRange_Enabled();
void  PitchRange_SetEnabled(bool on);
float PitchRange_MaxAngleDeg();
void  PitchRange_SetMaxAngleDeg(float deg);
