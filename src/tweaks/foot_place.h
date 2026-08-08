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
// SessionTweaks -- FOOT PLACEMENT. The shoe hovers above the griptape instead of sitting on it.
// Read-only probe of the foot auto-adjust, plus ONE default-off write (the auto-adjust Z clamp),
// which is the leading suspect. See the .cpp for the measured mechanism.
#pragma once
#include <cstdint>
struct OmpMenuApi;
void FootPlace_ReadConfig(const char* iniText);
void FootPlace_SaveConfig(char* iniText, size_t cap);
void FootPlace_ResetDefaults();
void FootPlace_Install();
void FootPlace_PumpFrame();                       // GAME THREAD: samples, judges, logs
void FootPlace_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
bool FootPlace_Enabled();
void FootPlace_SetEnabled(bool on);
// Board/rider state, read from the anim instance this module already tracks. Shared so other fixes
// do not each re-resolve the skater -> mesh -> anim chain. False when there is no live anim yet.
bool FootPlace_Grounded();        // the skater is back on the ground
bool FootPlace_SettingUpTrick();  // cranking -- the crouch before a pop
