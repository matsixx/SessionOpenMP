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
// SessionTweaks -- CATCH SOUND: the board-catch sound is inconsistent and sometimes quiet. It is an
// ANIM NOTIFY that flip-trick catch animations do not carry at all; this module floors the volume,
// attaches the sound to the skater instead of a fixed world point, plays the cue itself when nothing
// else does, and measures what is left.
#pragma once
struct OmpMenuApi;
void  CatchSound_ReadConfig(const char* iniText);
void  CatchSound_SaveConfig(char* iniText, size_t cap);
void  CatchSound_ResetDefaults();
void  CatchSound_Install();
void  CatchSound_PumpFrame();                     // game thread, once per input tick
void  CatchSound_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)
// pause-menu accessors (GAME THREAD), same shape as the other modules
bool  CatchSound_Enabled();
void  CatchSound_SetEnabled(bool on);
// PERCENT, matching the ini's own CatchSoundVolumePct (100 = stock) -- integers for the slider row.
float CatchSound_VolumePct();
void  CatchSound_SetVolumePct(float pct);
