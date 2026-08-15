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
// SessionTweaks -- make the camera's height follow the skater everywhere, not only onto obstacles
// higher than the launch point. Data-driven: one hook on ASkaterCameraActor::Tick captures the
// camera and steers USessionCameraData -- the game's own tunable asset -- rather than replacing any
// camera math. Three independent settings, no master switch -- each restores the stock value when
// it is turned off:
//   FOLLOW HEIGHT   the flat-air maxima, so no air classifies as "flat" (the hold-height mode)
//   PITCH ON DROP   _enableDropDetection, the stock feature that pitches down instead of descending;
//                   this one names the GAME'S behaviour, so ON is stock and OFF is our change
// Plus a PITCH slider the stock camera settings never offered: a fixed extra pitch on the CAMERA
// COMPONENT, composed from the actor's own rotation every frame -- the actor stays entirely the
// game's, so the offset can neither feed back into the camera's interpolation nor accumulate.
#pragma once
struct OmpMenuApi;
void CameraHeight_ReadConfig(const char* iniText);
void CameraHeight_SaveConfig(char* iniText, size_t cap);
void CameraHeight_Install();                       // sig-scan + hook; non-fatal if missing
void CameraHeight_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)
void CameraHeight_ResetDefaults();
// Pause-menu accessors (GAME THREAD, like every other module's).
bool CameraHeight_FollowEnabled();      void CameraHeight_SetFollowEnabled(bool on);
// ON = the game's stock drop pitch is left alone; OFF = we disable it so the camera descends with
// you. The stored sense is the LABEL's sense, not the mod's -- see the polarity note in the .cpp.
bool CameraHeight_PitchOnDropEnabled(); void CameraHeight_SetPitchOnDropEnabled(bool on);
float CameraHeight_PitchDeg();          void CameraHeight_SetPitchDeg(float deg);
