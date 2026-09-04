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
// SessionTweaks -- CATCH LEVELING: ease the board flat when it hits your foot (the catch),
// restoring the auto-leveling an update removed -- but keyed to the CATCH, never mid-trick.
#pragma once
struct OmpMenuApi;
void  CatchLevel_ReadConfig(const char* iniText);
void  CatchLevel_SaveConfig(char* iniText, size_t cap);
void  CatchLevel_ResetDefaults();
void  CatchLevel_Install();
// The resolved address of FlipTricksHandler::GetBoardExtraPitchAngle, or null if it was never found.
// EXPOSED BECAUSE A HOOK DESTROYS ITS OWN SIGNATURE: MinHook overwrites the prologue with a jump,
// so once this module hooks that function a byte-signature scan for it can never match again. Any
// later module needing the address (pitch_range wants it only as a return-address range) must take
// it from here rather than scan for it -- scanning silently fails depending on install order.
const void* CatchLevel_ExtraPitchFn();
// The real movement component (null until one trick has executed). See the .cpp for why
// skater+0x550 is not it.
void* CatchLevel_MovementComponent();
// POST-PHYSICS re-assert of the level setpoint, called from foot_place's UpdateFootAnchors detour
// (inside the animation update = after the physics pass). While a held-over stick keeps re-arming
// the trick's pitch trajectory past the catch, the physics write lands AFTER the pump's setpoint
// each frame and wins the race -- this runs later still, so the leveller wins instead. Gated on
// CatchTweaks_HeldOverStale(): a fresh post-catch orient press stands down.
void CatchLevel_PostPhysAssert();
void  CatchLevel_PumpFrame();                     // game thread, once per input tick
void  CatchLevel_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)
// pause-menu accessors (GAME THREAD), same shape as the other modules
bool  CatchLevel_Enabled();
void  CatchLevel_SetEnabled(bool on);
// The ease's time constant in ms (how fast the board eases flat), 20..200.
float CatchLevel_ResponseMs();
void  CatchLevel_SetResponseMs(float ms);
