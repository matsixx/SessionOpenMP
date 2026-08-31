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
// SessionTweaks -- POP CONTROL probe (read-only; writes nothing to the game).
//
// Groundwork for a redesigned pop: crouch depth on one stick, the pop as its own flick on the
// other, the trick flick after. Before anything is built, this measures what the redesign has to
// reproduce: the raw two-stick stream around every crank and pop, how long a crank is held against
// what the game turns that into (the pop ratio handed to JumpForTrick), how pocket/corner input
// shows up, and which anim-instance float drives the crouch depth (the crank blendspace alpha --
// the flag next to it is known, the alpha is not).
//
// No hooks of its own: fed per input tick from the shell pump (sticks via ScoopSpeed_StickRaw,
// crank/grounded via foot_place's anim instance) and per trick from grind_pop's drain, where the
// trick name is already resolved and the pop values already read.
#pragma once
struct OmpMenuApi;
void PopProbe_ReadConfig(const char* iniText);
void PopProbe_SaveConfig(char* iniText, size_t cap);
void PopProbe_ResetDefaults();
void PopProbe_Install();                         // the pad-level hook (injection mode 3)
void PopProbe_PumpFrame();                       // GAME THREAD, runs on the input tick
// The round-2 injection experiment's two possible write points, called from scoop_speed's
// InputHandler::Tick hook: Early = after the physical sticks are sampled, before the game's own
// tick runs; Late = after it. Which of the two makes the game crank is the experiment. Both are
// no-ops unless PopProbeInject selects them.
void PopProbe_TickEarly(void* inputHandler);
// Mode 4, the shipped injection: called from the InputHandler::Tick hook with the tick's own
// 4-float stick buffer (LSx, LSy, RSx, RSy), BEFORE the original runs. Captures the physical
// values and replaces them with the gesture machine's -- any controller, any backend.
void PopProbe_TickSticks(float* sticks);
void PopProbe_TickLate(void* inputHandler);
void PopProbe_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
// Called from grind_pop's pump after it drains a JumpForTrick record -- names resolved, values
// read, and outside the game's callstack. Flat and grind pops both.
void PopProbe_OnJump(const char* trick, int onGrind, float argHeight, float argPopRatio,
                     float grindRatio, float trickPopRatio);
// The second per-trick feed, from grind_pop's pitch drain (mode START fires once per trick). Carries
// only the skater's TrickPopRatio; the two feeds dedupe against each other, so whichever arrives
// first logs the correlation and the other stays quiet.
void PopProbe_OnArm(float trickPopRatio);
// The user's PHYSICAL sticks, captured by the pad hook before any rewrite. False when the pad
// scheme is not running (or the pad has gone quiet), in which case the input fields ARE physical
// and callers should read those as they always did. Exists because the flick trackers (flip/scoop
// speed) measured the REWRITTEN fields once the scheme shipped -- the trick flick reached them
// clamped, and flip speed went inconsistent with thumb depth (field report).
bool PopProbe_PhysSticks(float* lx, float* ly, float* rx, float* ry);
// The pop scheme's live crouch depth, 0..1 (0 when not crouched or the scheme is off).
// Read-only, for modules that shape the body to the load (body_feel).
float PopProbe_CrouchDepth01();
// The pause-menu page's accessors (GAME THREAD, menu_ext contract): plain int reads/writes,
// every setter marks the ini dirty. "Scheme" = pad-level injection mode 3, the shipped scheme.
bool  PopProbe_SchemeEnabled();      void PopProbe_SetSchemeEnabled(bool on);
float PopProbe_TrickWindowMs();      void PopProbe_SetTrickWindowMs(float v);
float PopProbe_CrouchGatePct();      void PopProbe_SetCrouchGatePct(float v);
float PopProbe_CrankVisTimeMs();     void PopProbe_SetCrankVisTimeMs(float v);
float PopProbe_CrankVisMinMs();      void PopProbe_SetCrankVisMinMs(float v);
float PopProbe_CrankVisSmoothMs();   void PopProbe_SetCrankVisSmoothMs(float v);
