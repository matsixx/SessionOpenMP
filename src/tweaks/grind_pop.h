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
// SessionTweaks -- GRIND POP probe. Read-only measurement of the grind-exit pop pipeline: one hook
// on USkaterMovementComponent::JumpForTrick records the pop inputs and the resulting pelvis delay
// per trick. Nothing is written to the game; this module exists to decide whether the grind-exit
// pop is throttled by data (per-trick MaxCrankRatioOnGrinds) or by the crank-ratio formula.
#pragma once
struct OmpMenuApi;
void GrindPop_ReadConfig(const char* iniText);
void GrindPop_SaveConfig(char* iniText, size_t cap);
void GrindPop_Install();
void GrindPop_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
void GrindPop_PumpFrame();                       // GAME THREAD: drains the record ring and logs
void GrindPop_ResetDefaults();
bool GrindPop_Enabled();
void GrindPop_SetEnabled(bool on);
// Pause-menu accessors (GAME THREAD, like every other module's).
bool  GrindPop_PitchEnabled();     void GrindPop_SetPitchEnabled(bool on);
float GrindPop_PitchScale();       void GrindPop_SetPitchScale(float pct);
bool  GrindPop_SwingEnabled();     void GrindPop_SetSwingEnabled(bool on);
float GrindPop_SwingBlend();       void GrindPop_SetSwingBlend(float pct);
// Resolve an FName to text, cached by FName value (this module owns the FName::ToString address, and
// only one module may resolve it). `fnamePtr` points AT the FName field, not at the owning object.
// False if the resolver is unavailable or the read failed -- callers must have a fallback.
bool GrindPop_NameOfFName(const void* fnamePtr, char* out, int cap);
// FName -> ascii, cached; the module's proven resolver, exported for body_feel's
// bone-name classification. False when the name pool helper is unresolved.
bool GrindPop_FNameToString(const void* fname, char* out, int cap);
