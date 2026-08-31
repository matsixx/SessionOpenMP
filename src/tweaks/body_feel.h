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
// SessionTweaks -- BODY FEEL: the skater's physical-animation stiffness, made alive.
//
// The game drives its skeleton toward the animated pose with physics motors (stock UE
// UPhysicalAnimationComponent) at ONE fixed strength -- the body reacts identically at a crawl
// and at full speed, on a curb hop and off a ten-stair. Real bodies modulate stiffness
// constantly: braced at speed, loose in the air, tensed loading a pop, and they give on impact.
// This module writes the skater's own PhysicalAnimationBlendWeightMultiplier every frame from
// live skating state, so the same authored physics reads as a body instead of a mannequin.
//
// No hooks of its own: ticked from the shell pump, reading the anim instance and skater the
// other modules already resolve. Own player only; stands down in ragdolls and replays.
#pragma once
struct OmpMenuApi;
void BodyFeel_ReadConfig(const char* iniText);
void BodyFeel_SaveConfig(char* iniText, size_t cap);
void BodyFeel_ResetDefaults();
void BodyFeel_PumpFrame();                       // GAME THREAD, runs on the input tick
// The write that actually lands: called from foot_place's detour INSIDE the animation update
// (after the Blueprint's own per-frame weight writes, before the physics blend consumes them)
// -- the same surviving write point catch_level uses. The pump computes the feel; this applies.
void BodyFeel_PostPhysApply();
// Menu accessors (menu_ext contract: plain int reads/writes, setters mark the ini dirty).
bool  BodyFeel_Enabled();       void BodyFeel_SetEnabled(bool on);
float BodyFeel_AmountPct();     void BodyFeel_SetAmountPct(float v);
// Physical-animation page: strengths as % of the shipped tuning, times in ms.
bool  BodyFeel_BraceEnabled();  void BodyFeel_SetBraceEnabled(bool on);
float BodyFeel_ReachPct();      void BodyFeel_SetReachPct(float v);
float BodyFeel_CarryPct();      void BodyFeel_SetCarryPct(float v);
float BodyFeel_ArmPct();        void BodyFeel_SetArmPct(float v);
float BodyFeel_FallPct();       void BodyFeel_SetFallPct(float v);
float BodyFeel_FlailMs();       void BodyFeel_SetFlailMs(float v);
float BodyFeel_FlailDelayMs();  void BodyFeel_SetFlailDelayMs(float v);
float BodyFeel_FlailPct();      void BodyFeel_SetFlailPct(float v);
float BodyFeel_GrabMs();        void BodyFeel_SetGrabMs(float v);
float BodyFeel_GrabDelayMs();   void BodyFeel_SetGrabDelayMs(float v);
float BodyFeel_GrabPct();       void BodyFeel_SetGrabPct(float v);
