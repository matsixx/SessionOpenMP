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
// SessionTweaks -- FOOT STEERING. The thumbsticks offset the foot IK targets IN THE AIR, so the feet
// can be tweaked and boned rather than only played back. Slow deliberate stick travel steers; fast
// flicks stay the catch.
#pragma once
#include <cstddef>
struct OmpMenuApi;
void FootSteer_ReadConfig(const char* iniText);
void FootSteer_SaveConfig(char* iniText, size_t cap);
void FootSteer_ResetDefaults();
void FootSteer_Install();
void FootSteer_PumpFrame();                       // GAME THREAD: liveness watch only
void FootSteer_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)

// The pause-menu accessors run on the GAME THREAD (the other menu_ext contract).
bool  FootSteer_Enabled();      void FootSteer_SetEnabled(bool on);
float FootSteer_ReachCm();      void FootSteer_SetReachCm(float cm);
float FootSteer_ResponseMs();   void FootSteer_SetResponseMs(float ms);
float FootSteer_DeadzonePct();  void FootSteer_SetDeadzonePct(float pct);
// What the stick directions are measured against, and which basis axis each stick component drives.
// Live because the answer is something to look at in-game, not to derive: frame 0..3, axes 0..5.
float FootSteer_Frame();        void FootSteer_SetFrame(float f);
float FootSteer_AxisX();        void FootSteer_SetAxisX(float a);
float FootSteer_AxisY();        void FootSteer_SetAxisY(float a);
// How far the foot twists with a forward/back push, and which axis of the chosen frame it turns
// about. Degrees at full stick (0 = position only); axis 0..5 = +X/+Y/+Z then the same negated.
float FootSteer_TwistDeg();     void FootSteer_SetTwistDeg(float deg);
float FootSteer_TwistAxis();    void FootSteer_SetTwistAxis(float a);
const char* FootSteer_FrameName();

// The FILTERED steer for one stick (-1..1), i.e. what the feet are actually doing after the rate
// limit, the flick veto and the arm/ease gates. False when steering is not driving that foot.
// Anything else that wants to move with the feet must take this rather than the raw stick, or the
// two disagree by exactly the amount of the filter.
bool FootSteer_Steer(bool rightStick, float* x, float* y);

// Called from foot_place's UpdateFootAnchors POST hook -- inside the animation update, which is the
// only phase where a write to the foot sockets survives into the rendered pose. ADDS this module's
// offset (cm, in the anim instance's own space) onto the caller's per-foot deltas and returns
// whether it contributed anything. This module hooks nothing itself: only one MinHook detour may
// exist per address, and foot_place owns that one.
bool FootSteer_AddOffset(void* animInstance, float dt, float outL[3], float outR[3]);
