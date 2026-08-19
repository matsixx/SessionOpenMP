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
// SessionTweaks -- CATCH SOUND, rebuilt: the game's own catch sound (an anim notify most flip-trick
// catch animations lack, and which fires late where it exists) is disabled for the local skater, and
// OUR sound plays on every catch edge through UReplayAudioManager::SpawnSoundAttached -- so it is
// written into the replay recording (co-op's audio source of truth) and captured by OpenMP's sync
// funnel. One source, no race. The designers' no-catch-sound blacklist (ollies) is honoured.
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
// UObject -> its object name, for logging (e.g. naming a trick definition). Cached by FName value.
// Lives here because this module already resolves FName::ToString; false = unresolvable.
bool CatchSound_ObjName(const void* obj, char* out, int cap);
// Raw-FName -> text (a bone name is an FName inside a struct, not a UObject). Same cache and rules.
bool CatchSound_FNameText(const void* fname, char* out, int cap);
