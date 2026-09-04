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
// =====================================================================================================
// SessionTweaks -- RUN OUT ON MISSED TRICKS.
//
// WHAT IT DOES: a bail that comes from MISSING a trick (missed/bad catch landing) while low to the
// ground becomes the game's own "trick failed" consequence -- skater on foot, board dropped into
// free physics -- instead of a ragdoll. Big air still bails for real. Manual catch only.
//
// HOW (measured via PDB + disassembly):
//  * The native consequence is `ASkaterCharacterBase::SetOnFootMode(this, byte)` -- skater off the
//    board (SetIsOnBoard false), board -> ASkateboardEx::SetOnFootMode -> SetSimulatePhysics (free
//    tumble), full listener surface notified (challenges, trick display widget, camera). It is what
//    the game's own toggle/throwdown paths call: a designed transition, not a re-init.
//  * `Bail` is the ragdoll route (it flips into the bailing movement mode); its own tail calls
//    SetOnFootMode from the same post-physics context, so substituting synchronously at a bail site
//    is safe.
//  * Landing then resolves naturally: `USkaterMovementComponent::SetPostLandedPhysics` picks
//    walking vs skating purely off `_isOnBoard` (+0xe20) -- after SetOnFootMode you land on foot.
//  * Bail causes are PER-FUNCTION in this game. The two TRICK-miss sources are
//    `CheckForBailFalling` (missed-catch landings -- three Bail sites) and `CheckForBailBoardAngle`
//    (bad-orientation landings). Collision/skateboarding bails are different functions, untouched.
//
// REQUIRED SHAPE:
//  * The check functions run PER FRAME and usually decide NOT to bail, so the policy must live in a
//    hook on Bail itself with the checks acting as pure MARKERS: the caller is identified by being
//    inside it. Acting on the check call instead turns every mount into a dismount.
//  * One marker does not cover the surface: missed-catch landings bail through Falling, not
//    BoardAngle. Both are markered, and a Bail from an UNMARKED source logs its caller RVA
//    (rate-limited) so an uncovered path names itself instead of hiding.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "run_out.h"
#include "catch_tweaks.h"
#include <intrin.h>
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    SK_CATCH_MODE   = 0x63d,   // ASkaterCharacterBase -> ECatchMode (the menu's Catch Mode)
    SK_MOVE_COMP    = 0x550,   // ASkaterCharacterBase -> _movementComponent
    SK_BOARD        = 0x568,   // ASkaterCharacterBase -> _skateboard (ASkateboardEx*)
    SK_THROWDOWN    = 0xa28,   // ASkaterCharacterBase -> _isDoingThrowdown (Bail routes these itself)
    MC_IS_ON_BOARD  = 0xe20,   // USkaterMovementComponent -> _isOnBoard (Bail's own SetOnFootMode guard)
    MC_VELOCITY     = 0xc4,    // UMovementComponent -> Velocity (FVector) -- saved/restored so the
                               // run-out KEEPS the riding momentum (the on-foot transition kills it)
    MC_MOVEMENT_MODE = 0x168,  // UCharacterMovementComponent -> MovementMode (6 = Custom)
    MC_CUSTOM_MODE   = 0x169,  // -> CustomMovementMode (Bail sets 6/5 = bailing)
    // Height-gate dead ends, do NOT revisit: mc+0x798 _inAirTime and +0x79c _inAirPopTime both read
    // 0 through popped airs (and at bail time); the Big Drop bitfield (mc+0xc84) is a constant 0x77
    // on flat-ground kickflips; the flipper floor probe (FBoardFloorResult at mc+0xb60) reads
    // FloorDist=0 with bBlockingHit set all through the air. Nothing measures the ground below
    // MID-AIR, so a mid-air bad catch converts immediately and the height check happens where two Z
    // reads exist -- the conversion point and the on-foot LANDING. A fall past the threshold
    // triggers the real bail at impact (RunOut_PumpFrame).
    BOARD_IFACE     = 0x280,   // ASkateboardEx + 0x280 = the interface EnableRagDoll is a method of
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int   g_runOut        = 1;     // master: convert low trick bails to the native run-out
static float g_runOutMaxDrop = 200.0f;// RunOutMaxDropCm: real bail when the air's height (apex Z -
                                      // landing Z) reaches this. A flat ollie apexes ~50-100 cm up.
static int   g_keepMomentum  = 1;     // RunOutKeepMomentum: restore the riding velocity after the
                                      // on-foot switch so you RUN it out instead of stopping dead
static volatile LONG g_faults = 0;
static volatile LONG g_uiRunOuts = 0, g_uiRealBails = 0;

void RunOut_ReadConfig(const char* buf) {
    g_runOut        = TwkIniInt(buf, "RunOut", 1);
    g_runOutMaxDrop = (float)TwkIniInt(buf, "RunOutMaxDropCm", 200);
    g_keepMomentum  = TwkIniInt(buf, "RunOutKeepMomentum", 1);
    TwkLog("[runout] config: RunOut=%d RunOutMaxDropCm=%.0f RunOutKeepMomentum=%d",
           g_runOut, g_runOutMaxDrop, g_keepMomentum);
}

void RunOut_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "RunOut",             g_runOut);
    TwkIniSetInt(buf, cap, "RunOutMaxDropCm",    (int)g_runOutMaxDrop);
    TwkIniSetInt(buf, cap, "RunOutKeepMomentum", g_keepMomentum);
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
static const char* SIG_BAIL =           // ASkaterCharacterBase::Bail -- `(this, FString*, byte, byte)`,
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 70 80 B9 49 06 00 00 00 41 0F B6 F9 41 0F B6 F0"; // no floats, no stack args. Epic 0xfeb310 / Steam 0xfab140
static const char* SIG_BAIL_ANGLE =     // USkateboardExMovementComponent::CheckForBailBoardAngle -- (this)
    "48 8B C4 48 89 58 10 56 48 81 EC C0 00 00 00 0F 29 70 E8 48 8B F1 48 8B 89 40 03 00 00 0F 29 78 D8"; // Epic 0xfb2680 / Steam 0xf72490
static const char* SIG_BAIL_FALLING =   // USkateboardExMovementComponent::CheckForBailFalling -- (this)
    "40 55 53 56 41 54 41 57 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 48 8D 99 38 01 00 00 4C 8B F9"; // Epic 0xfb2ec0 / Steam 0xf72cd0
static const char* SIG_SET_ON_FOOT =    // ASkaterCharacterBase::SetOnFootMode -- `(this, byte)`; CALLED, not hooked
    "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA C6 81 9A 05 00 00 00 48 8B D9 66 C7 81 28 0A 00 00 00 00"; // Epic 0x1005fa0 / Steam 0xfc5dd0
static const char* SIG_ENABLE_RAGDOLL = // ASkateboardEx::EnableRagDoll -- `(this)` where THIS = board+0x280
    "40 53 48 83 EC 20 48 8B 01 48 8B D9 ?? ?? ?? 84 C0 ?? ?? 48 8D 8B 80 FD FF FF";                   // (interface-adjusted); CALLED, not hooked. Epic 0xf84c50 / Steam 0xf44a60
static const char* SIG_SET_CATCH_ORIENT = // ASkaterCharacterBase::SetCatchOrient -- NEVER called or
    "40 55 53 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 48 8B 81 90 05 00 00 0F B6 FA";                   // hooked: a RANGE ANCHOR for the return-address marker. Epic 0x1004120 (size 0x565) / Steam 0xfc3f50
static const char* SIG_SET_ACTOR_LOCROT = // AActor::SetActorLocationAndRotation -- the FQUAT
    "48 81 EC E8 00 00 00 48 8B 89 30 01 00 00 48 85 C9 74 76 0F 10 99 D0 01 00 00 F3 0F 10 02";       // overload, FOUR floats (call pattern: loc3, quat4, false, nullptr, 0). CALLED, not hooked

// ------------------------------------------------------------------ the markers
// Depth counter set around the two trick-bail checks; a Bail arriving while it is nonzero came from
// a missed/bad-trick landing. Game thread only -- plain int is fine.
static int g_inTrickBailCheck = 0;

typedef void* (*CheckFn)(void*);
static void* g_origBailAngle = nullptr, *g_startBailAngle = nullptr;
static void* g_origBailFall  = nullptr, *g_startBailFall  = nullptr;
static void* hkBailAngle(void* self) {
    g_inTrickBailCheck++;
    void* r = nullptr;
    __try { r = ((CheckFn)g_origBailAngle)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal in CheckForBailBoardAngle -> recovered");
    }
    g_inTrickBailCheck--;
    return r;
}
static void* hkBailFalling(void* self) {
    g_inTrickBailCheck++;
    void* r = nullptr;
    __try { r = ((CheckFn)g_origBailFall)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal in CheckForBailFalling -> recovered");
    }
    g_inTrickBailCheck--;
    return r;
}

// ------------------------------------------------------------------ the policy (on Bail itself)
typedef void  (*SetOnFootFn)(void*, uint64_t);
typedef void  (*RagdollFn)(void*);
typedef void* (*BailFn)(void*, void*, uint64_t, uint64_t);
typedef void  (*SetLocRotFn)(void*, const float*, const float*, bool, void*, unsigned char);
static void* g_origBail = nullptr, *g_startBail = nullptr;
static volatile LONG g_bailCalls = 0;   // Bail calls seen on our skater (catch_tweaks' verdict probe)
static SetOnFootFn g_setOnFoot = nullptr;
static RagdollFn   g_enableRagdoll = nullptr;
static SetLocRotFn g_setLocRot = nullptr;
// The bad-catch bail (SetCatchOrient decides good/bad and Bails on bad) is marked by RETURN ADDRESS
// range, not a hook: SetCatchOrient's arity is unmeasured and a marker only needs to know the Bail
// came from inside it. The observed site is caller exe+0x1004580 = SetCatchOrient+0x460, the most
// frequent trick bail in play (flick, wrong orientation, ragdoll).
static uint64_t g_catchOrientRva = 0;                 // this exe's SetCatchOrient start
enum { CATCH_ORIENT_SPAN = 0x600 };                   // fn is 0x565 in Epic; padded bound

// Pending facing correction (queued by doRunOut, applied by RunOut_PumpFrame). It carries its own
// one-shot disable so a fault here only ever costs the facing nicety, never the run-out. Must be
// declared ABOVE doRunOut.
static void*  g_faceSkater = nullptr;
static float  g_faceYaw = 0.0f;
static int    g_faceOK = 1;
// Post-conversion instrumentation: every conversion samples velocity + facing for ~0.6 s so the log
// shows when and how the heading is redirected.
static void*  g_traceSkater = nullptr;
static double g_traceT0 = 0.0, g_traceLast = 0.0;
static float  g_tracePx = 0.0f, g_tracePy = 0.0f;   // previous trace position: the ACTUAL heading,
static bool   g_tracePosOK = false;                 // since the velocity field is unreliable here

// The conversion itself, shared by both trick-bail paths. It follows Bail's own order: free the
// board FIRST (EnableRagDoll = simulate physics + collision profile + the ragdolled flag every
// attach path respects; without it the board teleports into the hand), then the on-foot transition,
// then put the riding velocity back (the switch kills it; walking friction decays it naturally).
// Returns the kept horizontal speed for the log.
static float doRunOut(void* skater, void* mc) {
    // `mc->Velocity` is FROZEN while skating -- it holds the mount-time direction, so after a carved
    // turn it is stable but wrong. Momentum therefore comes from position over time:
    // CatchTweaks_TravelVel derives the travel vector from the sampler's position ring. The
    // component field is only the no-fresh-samples fallback.
    float vel[3] = {0, 0, 0};
    bool haveVel = false;
    if (g_keepMomentum) {
        if (CatchTweaks_TravelVel(&vel[0], &vel[1], &vel[2])) {
            haveVel = true;
            TwkLog("[runout] travel from position history: (%.0f,%.0f,%.0f)", vel[0], vel[1], vel[2]);
        } else if (mc) {
            vel[0] = twkF(mc, MC_VELOCITY);     vel[1] = twkF(mc, MC_VELOCITY + 4);
            vel[2] = twkF(mc, MC_VELOCITY + 8);
            haveVel = vel[0] > -100000.0f;
            if (haveVel) TwkLog("[runout] travel FALLBACK: component velocity (%.0f,%.0f) -- may be stale", vel[0], vel[1]);
        }
    }
    if (g_enableRagdoll) {
        void* board = twkP(skater, SK_BOARD);
        if (board) g_enableRagdoll((uint8_t*)board + BOARD_IFACE);
    }
    g_setOnFoot(skater, 0);                            // the native "trick failed" run-out
    // The on-foot state DOES use this field (walking mode) even though skating freezes it, so the
    // write is what carries the momentum across. `mc` is re-checked: haveVel can now come from the
    // position history alone, which does not imply a movement component.
    if (haveVel && mc) {
        *(float*)((uint8_t*)mc + MC_VELOCITY)     = vel[0];
        *(float*)((uint8_t*)mc + MC_VELOCITY + 4) = vel[1];
        *(float*)((uint8_t*)mc + MC_VELOCITY + 8) = vel[2];
    }
    // Face the travel direction: riding switch/fakie the character faces opposite their travel and
    // the on-foot state moves off the FACING, so a correct velocity alone can still launch the
    // run-out backwards. The rotation is only QUEUED here and never applied inside the Bail
    // callstack -- rotating there faults on re-entrant movement callbacks deep in SetCatchOrient,
    // and the policy's SEH kill-switch would then disable the whole run-out. RunOut_PumpFrame
    // applies it on the next input tick under its own guard. Speed-gated, because a near-standstill
    // heading is noise.
    if (haveVel && (vel[0] * vel[0] + vel[1] * vel[1]) > 100.0f * 100.0f) {
        g_faceYaw    = atan2f(vel[1], vel[0]);
        g_faceSkater = skater;
    }
    g_traceSkater = skater;
    g_traceT0 = g_traceLast = GetTickCount64() / 1000.0;
    g_tracePosOK = false;
    InterlockedIncrement(&g_uiRunOuts);
    return haveVel ? sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) : 0.0f;
}

// The armed post-conversion fall watch: a mid-air bad catch converts with NO height known, since
// nothing measures the ground below in flight (see the offsets note). The two Z reads that do exist
// are the conversion point and the on-foot landing; if that fall reaches the threshold, the real
// bail is delivered AT IMPACT via the game's own Bail.
static void*  g_armSkater = nullptr;                  // game thread only
static float  g_armZ = 0.0f;
static double g_armT = 0.0;
static bool   g_armFalling = false;

static void* hkBail(void* skater, void* reason, uint64_t b1, uint64_t b2) {
    // OUR skater only. Bail is per-skater, and in a co-op session SessionOpenMP deliberately calls
    // the game's own Bail on remote players' proxies to replicate their bails -- so this hook fires
    // for them. Everything below WRITES (ragdoll, on-foot mode, a velocity restore, a queued
    // rotation), and a remote player must bail exactly as their own machine intended.
    // Unknown local skater (before the catch system has run once) = solo behaviour, not off.
    // The two bail-CHECK markers are deliberately left ungated: they only bump a re-entrancy counter
    // around the original call, and with this gate in place a foreign check can no longer reach any
    // decision of ours.
    void* const mineB = CatchTweaks_Skater();
    const bool ours = !(mineB && skater && mineB != skater);
    if (ours) InterlockedIncrement(&g_bailCalls);       // every Bail on our skater, whatever the policy does
    if (ours && g_runOut && g_setOnFoot && skater) {
        __try {
            const uint64_t callerRva = (uint64_t)_ReturnAddress() - (uint64_t)GetModuleHandleA(nullptr);
            const bool fromCatch = g_catchOrientRva &&
                                   callerRva >= g_catchOrientRva && callerRva < g_catchOrientRva + CATCH_ORIENT_SPAN;
            if (!g_inTrickBailCheck && !fromCatch) {
                // Unmarked source (collision/impact/...) -- always a real bail, but name the caller
                // so an uncovered trick path shows itself in one round.
                static uint64_t lastMs = 0;
                const uint64_t ms = GetTickCount64();
                if (ms - lastMs > 500) {
                    lastMs = ms;
                    TwkLog("[runout] bail from unmarked source (caller exe+0x%llx) -- passing through", callerRva);
                }
            } else {
                const int  mm     = CatchTweaks_ManualMode();
                const bool manual = (mm >= 0) && (twkB(skater, SK_CATCH_MODE) == mm);
                void* mc = twkP(skater, SK_MOVE_COMP);
                const int   onBoard = mc ? twkB(mc, MC_IS_ON_BOARD) : 0;
                if (fromCatch) {
                    // Mid-air bad catch: convert IMMEDIATELY (loose board plus kept momentum) and
                    // arm the fall watch; the height verdict comes at the on-foot landing, where a
                    // real fall distance exists.
                    if (manual && onBoard > 0 && twkB(skater, SK_THROWDOWN) == 0) {
                        const float kept = doRunOut(skater, mc);
                        g_armSkater  = skater;
                        g_armZ       = TwkActorZ(skater);
                        g_armT       = GetTickCount64() / 1000.0;
                        g_armFalling = false;
                        TwkLog("[runout] RUN OUT at the bad catch (kept %.0f cm/s) -- fall watch armed at z=%.0f",
                               kept, g_armZ);
                        return nullptr;                            // Bail never runs
                    }
                    InterlockedIncrement(&g_uiRealBails);
                    TwkLog("[runout] real bail (bad catch, onBoard=%d, manual=%d)", onBoard, manual ? 1 : 0);
                } else {
                    // Landing bails: the height of the air that is ending = apex Z (per-frame
                    // samples) minus the skater's Z right now. Unknown -> conservative real bail.
                    float height = -1.0f;
                    const float apexZ = CatchTweaks_RecentMaxZ();
                    const float nowZ  = TwkActorZ(skater);
                    if (apexZ > -100000.0f && nowZ > -100000.0f) height = apexZ - nowZ;
                    const bool bigAir = height < 0.0f || height >= g_runOutMaxDrop;
                    if (manual && !bigAir && onBoard > 0 && twkB(skater, SK_THROWDOWN) == 0) {
                        const float kept = doRunOut(skater, mc);
                        TwkLog("[runout] RUN OUT instead of bail (landing, height %.0f cm < %.0f cm, kept %.0f cm/s)",
                               height, g_runOutMaxDrop, kept);
                        return nullptr;                            // Bail never runs
                    }
                    InterlockedIncrement(&g_uiRealBails);
                    TwkLog("[runout] real bail (landing, height %.0f cm vs %.0f cm, onBoard=%d, manual=%d)",
                           height, g_runOutMaxDrop, onBoard, manual ? 1 : 0);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal in the bail policy -> feature off");
            g_runOut = 0;
        }
    }
    __try { return ((BailFn)g_origBail)(skater, reason, b1, b2); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal in Bail -> recovered");
        return nullptr;
    }
}

// A bail delivered on request from OUTSIDE the game's bail callstack: catch_tweaks' over-rotation
// limit (CatchOverBailDeg), decided on a catch's first frame. Same policy as the game's own mid-air
// bad-catch verdict gets in hkBail -- on manual with run-out enabled the catch converts to a run-out
// on the spot and the fall watch is armed; otherwise the game's own Bail. Counted like any Bail on
// our skater, so the verdict probe's before/after count sees it.
void RunOut_BailNow(void* skater) {
    if (!skater || !g_origBail) return;
    __try {
        InterlockedIncrement(&g_bailCalls);
        void* mc = twkP(skater, SK_MOVE_COMP);
        const int  mm      = CatchTweaks_ManualMode();
        const bool manual  = (mm >= 0) && (twkB(skater, SK_CATCH_MODE) == mm);
        const int  onBoard = mc ? twkB(mc, MC_IS_ON_BOARD) : 0;
        if (g_runOut && g_setOnFoot && manual && mc && onBoard > 0 && twkB(skater, SK_THROWDOWN) == 0) {
            const float kept = doRunOut(skater, mc);
            g_armSkater  = skater;
            g_armZ       = TwkActorZ(skater);
            g_armT       = GetTickCount64() / 1000.0;
            g_armFalling = false;
            InterlockedIncrement(&g_uiRunOuts);
            TwkLog("[runout] RUN OUT at the over-rotated catch (kept %.0f cm/s) -- fall watch armed at z=%.0f",
                   kept, g_armZ);
            return;
        }
        static wchar_t overChars[] = L"SessionTweaks over-rotation";
        static struct { wchar_t* d; int n; int max; } overStr =
            { overChars, (int)(sizeof(overChars) / 2), (int)(sizeof(overChars) / 2) };
        InterlockedIncrement(&g_uiRealBails);
        TwkLog("[runout] real bail (over-rotated catch, onBoard=%d, manual=%d)", onBoard, manual ? 1 : 0);
        ((BailFn)g_origBail)(skater, &overStr, 1, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[runout] caught fatal delivering an over-rotation bail -> skipped");
    }
}

// The fall watch + deferred facing, one poll per input tick (game thread).
void RunOut_PumpFrame() {
    // ---- a MISSED early catch has landed (catch_tweaks refused the press; CatchEarlyMiss): this
    // landing is a bail whatever the deck did, through the same policy as a marked landing bail --
    // low air on manual -> the run-out where enabled, otherwise the game's own Bail. Delivered here,
    // outside the game's bail callstack, exactly like the fall watch's impact bail below.
    if (CatchTweaks_TakeMissedLanding()) {
        __try {
            void* skater = CatchTweaks_Skater();
            void* mc = skater ? twkP(skater, SK_MOVE_COMP) : nullptr;
            if (skater && mc) {
                if (twkB(mc, MC_MOVEMENT_MODE) == 6 && twkB(mc, MC_CUSTOM_MODE) == 5) {
                    TwkLog("[runout] missed catch: the game is already bailing at touchdown");
                } else {
                    const int  mm      = CatchTweaks_ManualMode();
                    const bool manual  = (mm >= 0) && (twkB(skater, SK_CATCH_MODE) == mm);
                    const int  onBoard = twkB(mc, MC_IS_ON_BOARD);
                    float height = -1.0f;
                    const float apexZ = CatchTweaks_RecentMaxZ();
                    const float nowZ  = TwkActorZ(skater);
                    if (apexZ > -100000.0f && nowZ > -100000.0f) height = apexZ - nowZ;
                    const bool bigAir = height < 0.0f || height >= g_runOutMaxDrop;
                    if (g_runOut && g_setOnFoot && manual && !bigAir && onBoard > 0 &&
                        twkB(skater, SK_THROWDOWN) == 0) {
                        const float kept = doRunOut(skater, mc);
                        InterlockedIncrement(&g_uiRunOuts);
                        TwkLog("[runout] missed catch: RUN OUT (height %.0f cm < %.0f cm, kept %.0f cm/s)",
                               height, g_runOutMaxDrop, kept);
                    } else if (g_origBail) {
                        static wchar_t missChars[] = L"SessionTweaks missed catch";
                        static struct { wchar_t* d; int n; int max; } missStr =
                            { missChars, (int)(sizeof(missChars) / 2), (int)(sizeof(missChars) / 2) };
                        InterlockedIncrement(&g_uiRealBails);
                        TwkLog("[runout] missed catch: BAIL (height %.0f cm, onBoard=%d, manual=%d)",
                               height, onBoard, manual ? 1 : 0);
                        ((BailFn)g_origBail)(skater, &missStr, 1, 1);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal delivering a missed catch -> skipped");
        }
    }
    // ---- pending facing correction, applied OUTSIDE the game's bail callstack
    if (g_faceSkater) {
        void* sk = g_faceSkater;
        g_faceSkater = nullptr;
        if (g_faceOK && g_setLocRot) {
            __try {
                void* root = twkP(sk, 0x130);          // AActor::RootComponent
                const float qx = root ? twkF(root, 0x1c0) : -999999.0f;
                const float qy = root ? twkF(root, 0x1c4) : 0.0f;
                const float qz = root ? twkF(root, 0x1c8) : 0.0f;
                const float qw = root ? twkF(root, 0x1cc) : 0.0f;
                alignas(16) float loc[4]  = { root ? twkF(root, 0x1d0) : -999999.0f,
                                              root ? twkF(root, 0x1d4) : 0.0f,
                                              root ? twkF(root, 0x1d8) : 0.0f, 0.0f };
                if (qx > -100000.0f && loc[0] > -100000.0f) {
                    const float faceYaw = atan2f(2.0f * (qx * qy + qw * qz), 1.0f - 2.0f * (qy * qy + qz * qz));
                    float diff = g_faceYaw - faceYaw;
                    while (diff >  3.14159265f) diff -= 6.28318531f;
                    while (diff < -3.14159265f) diff += 6.28318531f;
                    alignas(16) float quat[4] = { 0.0f, 0.0f, sinf(g_faceYaw * 0.5f), cosf(g_faceYaw * 0.5f) };
                    g_setLocRot(sk, loc, quat, false, nullptr, 0);
                    TwkLog("[runout] facing set to travel heading (was %.0f deg off)", fabsf(diff) * 57.2957795f);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_faceOK = 0;
                TwkLog("[runout] facing correction faulted -> that nicety is off (run-out unaffected)");
            }
        }
    }
    // ---- the post-conversion velocity trace (~6 lines per conversion, 0.1 s apart)
    if (g_traceSkater) {
        __try {
            const double now = GetTickCount64() / 1000.0;
            if (now - g_traceT0 > 0.65) g_traceSkater = nullptr;
            else if (now - g_traceLast >= 0.1) {
                const double dt = now - g_traceLast;
                g_traceLast = now;
                void* mc   = twkP(g_traceSkater, SK_MOVE_COMP);
                void* root = twkP(g_traceSkater, 0x130);
                const float vx = mc ? twkF(mc, MC_VELOCITY)     : -999999.0f;
                const float vy = mc ? twkF(mc, MC_VELOCITY + 4) : 0.0f;
                const float px = root ? twkF(root, 0x1d0) : -999999.0f;
                const float py = root ? twkF(root, 0x1d4) : 0.0f;
                float faceDeg = -999.0f;
                if (root) {
                    const float qx = twkF(root, 0x1c0), qy = twkF(root, 0x1c4);
                    const float qz = twkF(root, 0x1c8), qw = twkF(root, 0x1cc);
                    if (qx > -100000.0f)
                        faceDeg = atan2f(2.0f * (qx * qy + qw * qz), 1.0f - 2.0f * (qy * qy + qz * qz)) * 57.2957795f;
                }
                // ACTUAL heading = where the skater really moved since the last trace sample. The
                // velocity field can be frozen while skating; position cannot go stale.
                float moveDeg = -999.0f, moveSpeed = 0.0f;
                if (px > -100000.0f && g_tracePosOK && dt > 0.0) {
                    const float dx = px - g_tracePx, dy = py - g_tracePy;
                    moveSpeed = sqrtf(dx * dx + dy * dy) / (float)dt;
                    if (moveSpeed > 5.0f) moveDeg = atan2f(dy, dx) * 57.2957795f;
                }
                if (px > -100000.0f) { g_tracePx = px; g_tracePy = py; g_tracePosOK = true; }
                if (vx > -100000.0f || px > -100000.0f)
                    TwkLog("[runout] trace t+%.1fs vel=(%.0f,%.0f) velHeading=%.0f facing=%.0f | ACTUAL heading=%.0f speed=%.0f",
                           now - g_traceT0, vx, vy, atan2f(vy, vx) * 57.2957795f, faceDeg, moveDeg, moveSpeed);
                else g_traceSkater = nullptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_traceSkater = nullptr; }
    }
    // ---- the armed fall watch. Landing = downward speed seen, then gone.
    void* skater = g_armSkater;
    if (!skater) return;
    __try {
        const double now = GetTickCount64() / 1000.0;
        if (!g_runOut || now - g_armT > 4.0) { g_armSkater = nullptr; return; }   // stale; forget it
        void* mc = twkP(skater, SK_MOVE_COMP);
        const float z  = TwkActorZ(skater);
        const float vz = mc ? twkF(mc, MC_VELOCITY + 8) : -999999.0f;
        if (z < -100000.0f || vz < -100000.0f) { g_armSkater = nullptr; return; } // unreadable; forget it
        if (vz < -100.0f) g_armFalling = true;
        if (!g_armFalling || vz <= -20.0f) return;                                // still airborne
        g_armSkater = nullptr;                                                    // landed: decide
        const float fall = g_armZ - z;
        // already bailing (the game's own impact machinery got there first) -> nothing to add
        if (mc && twkB(mc, MC_MOVEMENT_MODE) == 6 && twkB(mc, MC_CUSTOM_MODE) == 5) {
            TwkLog("[runout] fall watch: game already bailing at impact (fell %.0f cm)", fall);
            return;
        }
        if (fall >= g_runOutMaxDrop && g_origBail) {
            // FString layout {data,num,max}, num counts the terminator; Bail treats the reason as a
            // const ref, so a static buffer is fine. (reason, 1, 1) matches the in-game caller
            // pattern; the last 1 is bRagdoll.
            static wchar_t reasonChars[] = L"SessionTweaks run-out impact";
            static struct { wchar_t* d; int n; int max; } reasonStr =
                { reasonChars, (int)(sizeof(reasonChars) / 2), (int)(sizeof(reasonChars) / 2) };
            InterlockedIncrement(&g_uiRealBails);
            TwkLog("[runout] fall watch: fell %.0f cm >= %.0f cm -> BAIL AT IMPACT", fall, g_runOutMaxDrop);
            ((BailFn)g_origBail)(skater, &reasonStr, 1, 1);
        } else {
            TwkLog("[runout] fall watch: fell %.0f cm < %.0f cm -- run-out stands", fall, g_runOutMaxDrop);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[runout] caught fatal in the fall watch -> disarmed");
        g_armSkater = nullptr;
    }
}

// ------------------------------------------------------------------ install + menu
static bool hookOne(const char* sig, void* hk, void** orig, void** start, const char* name) {
    *start = TwkScanExe(sig);
    if (!*start) { TwkLog("[runout] %s sig NOT FOUND (game updated?)", name); return false; }
    if (MH_CreateHook(*start, hk, orig) != MH_OK || MH_EnableHook(*start) != MH_OK) {
        TwkLog("[runout] %s hook failed", name); *start = nullptr; return false;
    }
    return true;
}
void RunOut_Install() {
    g_setOnFoot = (SetOnFootFn)TwkScanExe(SIG_SET_ON_FOOT);
    if (!g_setOnFoot) { TwkLog("[runout] SetOnFootMode sig NOT FOUND -- run-out off (game updated?)"); g_runOut = 0; return; }
    g_enableRagdoll = (RagdollFn)TwkScanExe(SIG_ENABLE_RAGDOLL);
    if (!g_enableRagdoll) TwkLog("[runout] EnableRagDoll sig NOT FOUND -- run-out board may go to the hand");
    g_setLocRot = (SetLocRotFn)TwkScanExe(SIG_SET_ACTOR_LOCROT);
    if (!g_setLocRot) TwkLog("[runout] SetActorLocationAndRotation sig NOT FOUND -- run-out keeps the old facing");
    // catch_tweaks HOOKS SetCatchOrient (the flicked-foot fix) and installs BEFORE this, so its
    // first bytes are a detour by now and SIG_SET_CATCH_ORIENT no longer matches -- one detour per
    // address, a hook destroys its own signature. Take the address it already resolved, and only
    // fall back to scanning when that module did not hook it.
    if (void* co = CatchTweaks_SetCatchOrientAddr())
        g_catchOrientRva = (uint64_t)((uint8_t*)co - (uint8_t*)GetModuleHandleA(nullptr));
    else if (uint8_t* co2 = TwkScanExe(SIG_SET_CATCH_ORIENT))
        g_catchOrientRva = (uint64_t)(co2 - (uint8_t*)GetModuleHandleA(nullptr));
    else TwkLog("[runout] SetCatchOrient sig NOT FOUND -- bad-catch bails will NOT convert");
    if (!hookOne(SIG_BAIL_ANGLE,   (void*)&hkBailAngle,   &g_origBailAngle, &g_startBailAngle, "CheckForBailBoardAngle") ||
        !hookOne(SIG_BAIL_FALLING, (void*)&hkBailFalling, &g_origBailFall,  &g_startBailFall,  "CheckForBailFalling")   ||
        !hookOne(SIG_BAIL,         (void*)&hkBail,        &g_origBail,      &g_startBail,      "Bail")) {
        g_runOut = 0;
        TwkLog("[runout] run-out off (missing hook)");
        return;
    }
    TwkLog("[runout] installed -- trick bails with a drop under %.0f cm become the native run-out "
           "(SetOnFootMode @ %p, EnableRagDoll @ %p, SetCatchOrient @ exe+0x%llx)",
           g_runOutMaxDrop, (void*)g_setOnFoot, (void*)g_enableRagdoll, g_catchOrientRva);
}

long RunOut_BailCalls() { return g_bailCalls; }
bool RunOut_Enabled() { return g_runOut != 0; }
void RunOut_SetEnabled(bool on) { g_runOut = on ? 1 : 0; TwkMarkDirty(); }
void RunOut_ResetDefaults() {
    g_runOut = 1; g_runOutMaxDrop = 200.0f; g_keepMomentum = 1;
    TwkMarkDirty();
}
float RunOut_MaxDropCm() { return g_runOutMaxDrop; }
void  RunOut_SetMaxDropCm(float cm) { g_runOutMaxDrop = cm; TwkMarkDirty(); }

void RunOut_DrawMenu(const OmpMenuApi* api) {
    char b[160];
    if (!g_startBail) { api->TextDisabled("Run out on missed tricks: not installed"); return; }
    bool ro = g_runOut != 0;
    if (api->Checkbox("Run out instead of bail (missed tricks)", &ro)) { g_runOut = ro ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(manual catch only; the game's own trick-fail)");
    if (ro) {
        api->Indent();
        float m = g_runOutMaxDrop / 100.0f;
        if (api->SliderFloat("Real bail if drop over (m)", &m, 0.5f, 6.0f, "%.1f")) { g_runOutMaxDrop = m * 100.0f; TwkMarkDirty(); }
        bool km = g_keepMomentum != 0;
        if (api->Checkbox("Keep momentum (run it out)", &km)) { g_keepMomentum = km ? 1 : 0; TwkMarkDirty(); }
        snprintf(b, sizeof(b), "run-outs: %d    real bails: %d", (int)g_uiRunOuts, (int)g_uiRealBails);
        api->TextDisabled(b);
        api->Unindent();
    }
}
