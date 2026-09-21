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
// SessionTweaks -- THE RADIAL MENU. Off the board, click a stick (the right by default, the left if
// RadialLeftStick is set): a wheel of things to do opens around the middle of the screen, drawn with
// the game's own widgets. The RIGHT stick points at an entry whichever one opened it, A takes it, B
// steps back (or closes), another click closes. Walking is never taken, so you keep moving while it is
// open; the camera is held still, because its stick is the wheel's. See radial.cpp.
#pragma once
#include <stddef.h>
struct OmpMenuApi;
void Radial_ReadConfig(const char* iniText);
void Radial_SaveConfig(char* iniText, size_t cap);
void Radial_ResetDefaults();
void Radial_Install();                        // sig-scan; every symbol optional
void Radial_PumpFrame();                      // GAME THREAD: the wheel's state machine and its widgets
void Radial_DrawMenu(const OmpMenuApi* api);  // RENDER THREAD (menu_ext contract)
bool Radial_Enabled();      void Radial_SetEnabled(bool on);
// Which stick's CLICK opens it. The right stick still steers the wheel either way.
bool Radial_LeftStick();   void Radial_SetLeftStick(bool on);
bool Radial_Open();                           // the wheel is on screen (it owns the prompt bar then)
void* Radial_SpawnActor(void* anyActorInTheWorld, void* cls, const float loc[3], const float rotPYR[3]);   // props: GAME THREAD
void  Radial_DestroyActor(void* actor);
void  Radial_PlaceComp(void* sceneComponent, const float loc[3], const float quat[4]);
void  Radial_SetMovable(void* sceneComponent);
bool Radial_Busy();                           // ...or the store it opened is: no other module takes a button
// From the UPlayerInput::InputKey hook (catch_tweaks.cpp), before anything else looks at the key.
// 0 = not ours, pass it on. 1 = ours, swallow it. 2 = pass it on with *amount as rewritten here --
// how the camera's stick is held at zero without the game being left holding its last value.
int  Radial_OnInputKey(const void* fkey, int ev, float* amount);
// From the InputHandler::Tick hook (scoop_speed.cpp), local handler only: the tick's stick buffer
// (LX, LY, RX, RY). The right stick is read from it, and zeroed in it while the wheel is open.
void Radial_TickSticks(float* sticks);
