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
// SessionTweaks -- MANUAL catch fixes (auto catch is deliberately untouched):
//  * wider catch window: the shipped pre-catch angles are a ~31 ms window at measured flip speed;
//    widened x2 while (and only while) the Catch Mode setting is manual.
//  * catch input beats dark slides: the dark-slide input reservation eats well-timed catch flicks,
//    so it is masked around the catch decision whenever a dark slide is geometrically impossible.
#pragma once
struct OmpMenuApi;
void CatchTweaks_ReadConfig(const char* iniText);  // parse this module's keys + echo them
void CatchTweaks_SaveConfig(char* iniText, size_t cap);  // write the live values back into the text
void CatchTweaks_Install();                        // sig-scan + hook; per-symbol non-fatal
void CatchTweaks_DrawMenu(const OmpMenuApi* api);  // RENDER THREAD (menu_ext contract)
// The accessors below back the pause-menu page and run on the GAME THREAD (the other menu_ext
// contract). Restore this module's shipped defaults (see the .cpp for what is deliberately
// excluded):
void CatchTweaks_ResetDefaults();
bool CatchTweaks_Enabled();
void CatchTweaks_SetEnabled(bool on);
// PERCENT, matching the ini's own CatchWindowMult (200 = x2.00) -- integers for the slider row.
float CatchTweaks_WindowMultPct();      void CatchTweaks_SetWindowMultPct(float pct);
float CatchTweaks_DarkslideZoneDeg();   void CatchTweaks_SetDarkslideZoneDeg(float deg);
int  CatchTweaks_ManualMode();                     // the ECatchMode value meaning MANUAL (-1 unknown)
// The last ASkaterCharacterBase seen by the catch system, for modules that need to poll skater state
// from the pump (catch_sound's catch-edge watch). Null until the first CanCatchOrient call.
void* CatchTweaks_Skater();
// The highest skater world-Z (cm) seen in the last ~1.5 s, sampled per frame from the
// CanCatchOrient hook -- run_out computes the drop height of the air that is ending as
// (this - live Z). -999999 = no fresh samples (no catch-system activity, e.g. a trickless fall).
// Airtime counters cannot be used for this: _inAirTime and _inAirPopTime both read 0 through
// popped-trick airs, so the height is measured from geometry instead.
float CatchTweaks_RecentMaxZ();
// The true travel velocity right now, derived from POSITION deltas over the last ~0.2 s of samples.
// `UMovementComponent::Velocity` is FROZEN while skating (custom movement mode) and holds the
// mount-time direction, so momentum must come from position history, which cannot go stale.
// Returns false when there is no fresh sample pair.
bool CatchTweaks_TravelVel(float* vx, float* vy, float* vz);
// Per-frame board-rotation watch (the flip trace). GAME THREAD.
void CatchTweaks_PumpFrame();
// A registered catch ends the flip at griptape-up instead of letting it spin another revolution.
bool CatchTweaks_StopsFlip();  void CatchTweaks_SetStopsFlip(bool on);
