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
// SessionTweaks -- SITTING while off the board. Press a button (B by default) to sit on the edge of
// the ledge you are facing or standing at, legs over the side and feet placed for its height, or on
// the ground when there is nothing to sit on. Press it again to stand. See sit.cpp.
#pragma once
#include <stddef.h>
struct OmpMenuApi;
void Sit_ReadConfig(const char* iniText);
void Sit_SaveConfig(char* iniText, size_t cap);
void Sit_ResetDefaults();
void Sit_Install();                          // sig-scan + the pose hook; non-fatal if missing
void Sit_PumpFrame();                        // GAME THREAD: the sit/stand state machine
void Sit_DrawMenu(const OmpMenuApi* api);    // RENDER THREAD (menu_ext contract)
// From the UPlayerInput::InputKey hook (catch_tweaks.cpp): the FKey and the event (0 press, 1 release).
// Returns true when the key was ours and must not reach the game.
bool Sit_OnInputKey(const void* fkey, int ev);
// From cloth_sim's AReplayManager::Tick detour (only one detour may exist on it): the replay editor
// is up, so sitting stands down and lets go of the key.
void Sit_NoteReplayTick(void* replayManager);   // the AReplayManager whose Tick this is
// GAME THREAD, from any hook that keeps ticking when ours does not: if the sit pump has stalled, the
// on-screen prompt is hidden. Cheap, and safe to call from anything on the game thread.
void Sit_WatchdogTick();
bool Sit_PoseHeld();                         // the sit pose is on the skeleton right now (seated, or blending either way)
// GAME THREAD, for the camera module: the seated first-person view -- the eyes (world), the look (world
// FQuat), the dolly weight 0..1 and the wanted FOV (0 = the game's). False = the camera is the game's.
bool Sit_FirstPersonView(float eye[3], float look[4], float* weight, float* fov);
// Pause-menu accessors (GAME THREAD).
bool  Sit_Enabled();        void Sit_SetEnabled(bool on);
bool  Sit_FloorEnabled();   void Sit_SetFloorEnabled(bool on);
float Sit_MaxLedgeCm();     void Sit_SetMaxLedgeCm(float v);
float Sit_ReachCm();        void Sit_SetReachCm(float v);
float Sit_LeanDeg();        void Sit_SetLeanDeg(float v);
bool  Sit_HeadLook();       void Sit_SetHeadLook(bool on);   // off the board, the head looks where the camera looks
