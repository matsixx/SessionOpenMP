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
// SessionTweaks -- CATCH LEVELING. The board used to level itself out during a flip trick and an
// update removed it. This restores the useful half, re-keyed to the CATCH: it levels only once the
// board hits the foot, never mid-trick.
//
// The feature was removed in code, not merely switched off by data: a full-.text disp32 scan
// (61 MB, byte prescan + capstone verification) finds ZERO float-typed accesses to ANY of the
// per-trick catch-align fields on UFlipTrickDefinition -- `CatchTargetPitchAngle` (+0x26c),
// `CatchTargetRollAngle` (+0x270), `CatchPitchAlignDelay` (+0x274), `CatchPitchAlignSmoothing`
// (+0x278), `YawAlignMinCatchRatio` (+0x288). The assets still author them; nothing reads them.
// (Scope: this proves there is no NATIVE reader -- UE reflection uses runtime property offsets and
// never shows up as a disp32.)
// Not to be confused with the LIVE path `UCatchOrientsDatabase` -> `GetCatchOrientPitch` (0x10251e0)
// -> `FCatchOrientSettings::GetPitchAngle` (0x1027270), consumed at PhysFalling+0x8bd: that lerps
// Left/Right pitch by catch YAW ratio, which is catch ORIENTATION, not leveling. Do not fight it.
//
// HOW IT DRIVES THE BOARD -- setpoint only, never the pose.
// The game integrates pitch itself each substep (PhysFalling / PhysSkateboarding both carry the same
// block): alpha = curTime/totalTime, Pitch = lerp(start, target, curve(alpha)), and when the move
// completes it sets `_pitchCurrentMode = 0` and skips the block forever after. So:
//   * writing `_pitchTargetAngle` alone does NOTHING once a move has completed -- the integrator has
//     switched itself off. It must be RE-ARMED.
//   * `_pitchCurrentTime = 0` is a FIXED POINT and produces no motion: with start = pitchNow and
//     alpha = 0, lerp(now, target, 0) == now, so every tick re-states "you are where you are". Step
//     a fraction of the remaining distance each tick (kStepAlpha) for an exponential ease.
// `_localAnimationRotator.Pitch` is never written here; the game stays the sole writer of the pose.
//
// The setpoint is driven from the frame pump rather than a Phys* hook because `PhysFalling`
// (0xfc6250) takes a float in XMM1 and has ZERO direct call sites (it is virtual), so its arity
// cannot be verified at a call site. Driving the setpoint idempotently from `Tweaks_PumpFrame` needs
// no new thunk and carries no arity risk.
//
// KNOWN RISK: these component offsets have read ZERO off `skater+0x550` even on tricks where pitch
// visibly worked, so the offsets must be proven on a case that works. This module therefore logs
// what it reads, warns once if the whole pitch block reads zero during a trick, and ships DEFAULT
// OFF, so a wrong pointer costs a log line rather than a session.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "catch_level.h"
#include "catch_tweaks.h"   // CatchTweaks_Skater() -- so a proxy's component is never adopted
#include "flip_speed.h"    // FlipSpeed_MsSinceTrick() -- the extra-pitch probe window
#include "foot_place.h"
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    FTH_SKATER            = 0x08,   // FlipTricksHandler -> _skater
    SK_CATCH_ORIENT_STATE = 0x63e,  // ASkaterCharacterBase -> _catchOrientState (non-zero = CAUGHT)
    SK_MOVE_COMP          = 0x550,  // -> _movementComponent
    // Pitch integrator, COMPONENT-relative (PDB offsets; the interface sub-object at comp+0x138 sees
    // these 0x138 LOWER -- know which pointer you hold).
    MC_PITCH_NOW          = 0x620,  // _localAnimationRotator.Pitch  (the POSE -- read only, never write)
    MC_PITCH_MODE         = 0x870,  // _pitchCurrentMode (byte; self-clears to 0 when a move completes)
    MC_PITCH_CUR_TIME     = 0x874,  // _pitchCurrentTime
    MC_PITCH_TOTAL_TIME   = 0x878,  // _pitchTotalTime
    MC_PITCH_START        = 0x87c,  // _pitchStartAngle
    MC_PITCH_TARGET       = 0x880,  // _pitchTargetAngle
    MC_PITCH_EXTRA        = 0x884,  // _pitchExtraAngle
    DEF_CATCH_TGT_PITCH   = 0x26c,  // UFlipTrickDefinition -> CatchTargetPitchAngle (authored, unread)
    DEF_CATCH_ALIGN_SMOOTH= 0x278,  // -> CatchPitchAlignSmoothing (authored, unread)
    PITCH_MODE_SETTLE     = 2,      // the mode whose target the settle uses
};
// The ease is a time constant, NOT a fixed fraction per tick. It used to take 25% of the
// remaining distance every TICK, which converges twice as fast at 120 fps as at 60 -- the same
// frame-rate dependence the flick measurement had. `alpha = 1 - exp(-dt/tau)` makes a given
// CatchLevelResponseMs mean the same thing on any machine. Never 0: the interpolator treats a zero
// current-time as its fixed point and would never move.
static const float kMinAlpha = 0.02f;
static const float kMaxDelayMs = 5.0f;   // see CatchLevelDelayMs -- above this it only hurts

// ------------------------------------------------------------------ knobs (ALL above the reader)
// User intent and runtime health are kept apart. `g_on` is the user's setting and the only thing
// SaveConfig writes; `g_ok` is a runtime kill-switch that is NEVER saved. If a fault (or a
// false-positive sanity gate, see below) could clear `g_on`, the next save would write CatchLevel=0
// into the ini and a one-frame hiccup would become a permanent setting.
static int   g_on         = 0;       // CatchLevel: DEFAULT OFF -- unproven offsets, see the header
static int   g_ok         = 1;       // runtime health, NEVER persisted
static float g_targetDeg  = -999.0f; // CatchLevelTargetDeg: -999 = use the trick's authored value
// CatchLevelResponseMs: the ease's TIME CONSTANT -- how fast it eases flat.
// 35, not 120+. This number changed MEANING when the ease became frame-rate independent. It used
// to scale a fixed 25%-of-the-remainder-per-TICK step, which at 120 fps works out to a ~29 ms time
// constant; as a true time constant, the old default was roughly 4x slower than the behaviour it
// replaced, and levelling stopped finishing inside the catch-to-landing window. 35 ms reproduces
// what the per-tick version actually did at a high frame rate, and now does it at any frame rate.
static float g_responseMs = 35.0f;
// CatchLevelDelayMs: wait after the catch. FIELD RESULT: raising this only ever makes levelling fire
// LESS often -- it is time taken out of the window between the catch and the landing, and the ease
// needs that window. Kept as a knob because 1-5 ms occasionally helps it settle, but CAPPED so it can
// no longer be turned up into a setting that quietly breaks the feature.
static float g_delayMs    = 0.0f;    // (0 = immediate; the
                                     // authored CatchPitchAlignDelay is 0.5s and would feel mid-trick)
static float g_maxMs      = 900.0f;  // CatchLevelMaxMs: hard stop so it can never stay armed
static int   g_log        = 1;       // CatchLevelLog: one block per trick
static volatile LONG g_faults = 0;
static volatile LONG g_uiLevels = 0;

static int    g_traceLevel = 0;        // CatchLevelTrace -- per-frame tug-of-war trace

void CatchLevel_ReadConfig(const char* buf) {
    g_on         = TwkIniInt(buf, "CatchLevel", 0);
    g_targetDeg  = (float)TwkIniInt(buf, "CatchLevelTargetDeg", -999);
    g_responseMs = (float)TwkIniInt(buf, "CatchLevelResponseMs", 35);
    // A value saved under the old meaning is not wrong, it is just interpreted differently now -- and
    // as a time constant it is slow enough to stop levelling finishing. Say so rather than silently
    // behaving worse than the version before it.
    if (g_responseMs > 80.0f)
        TwkLog("[level] CatchLevelResponseMs=%.0f is a TIME CONSTANT now, not a per-frame step -- at "
               "this value the ease will not finish before you land. Try 30-60.", g_responseMs);
    g_delayMs    = (float)TwkIniInt(buf, "CatchLevelDelayMs", 0);
    if (g_delayMs < 0.0f) g_delayMs = 0.0f;
    if (g_delayMs > kMaxDelayMs) {
        TwkLog("[level] CatchLevelDelayMs %.0f is past the %.0f ms cap -- clamped. Raising it only "
               "eats the catch-to-landing window and makes levelling fire less often.",
               g_delayMs, kMaxDelayMs);
        g_delayMs = kMaxDelayMs;
    }
    g_maxMs      = (float)TwkIniInt(buf, "CatchLevelMaxMs", 900);
    g_traceLevel = TwkIniInt(buf, "CatchLevelTrace", 0);
    g_log        = TwkIniInt(buf, "CatchLevelLog", 1);
    if (g_responseMs < 20.0f) g_responseMs = 20.0f;
    TwkLog("[level] config: CatchLevel=%d TargetDeg=%.0f ResponseMs=%.0f DelayMs=%.0f MaxMs=%.0f Log=%d",
           g_on, g_targetDeg, g_responseMs, g_delayMs, g_maxMs, g_log);
}
void CatchLevel_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "CatchLevel",           g_on);
    TwkIniSetInt(buf, cap, "CatchLevelTargetDeg",  (int)g_targetDeg);
    TwkIniSetInt(buf, cap, "CatchLevelResponseMs", (int)g_responseMs);
    TwkIniSetInt(buf, cap, "CatchLevelDelayMs",    (int)g_delayMs);
}
void CatchLevel_ResetDefaults() {
    g_on = 0; g_targetDeg = -999.0f; g_responseMs = 35.0f; g_delayMs = 0.0f; g_maxMs = 900.0f;
    g_ok = 1;                        // re-arm: a past fault is not a preference
}
bool  CatchLevel_Enabled()             { return g_on != 0; }
// Switching it ON is also an explicit "try again": otherwise a runtime kill would leave the toggle
// looking enabled while nothing happened.
void  CatchLevel_SetEnabled(bool on)   { g_on = on ? 1 : 0; if (on) g_ok = 1; TwkMarkDirty(); }

// ------------------------------------------------------------------ sig (dual-exe-verified)
// FlipTricksHandler::GetBoardExtraPitchAngle -- Epic 0x10234c0 / Steam 0xfe3630. Used ONLY as the
// "a trick just fired" signal: it supplies the skater and the executing trick definition, which is
// where the authored catch target lives. Arity verified at BOTH call sites (CheckForTrick+0xd56,
// CheckForLateTrick+0xb5f) and against the callee prologue: five args, no float args.
static const char* SIG_EXTRA_PITCH =
    "4C 8B DC 49 89 5B 20 56 41 56 41 57 48 81 EC F0 00 00 00 48 8B 41 08 49 8B D9 45 0F B6 F8";

// USkateboardExMovementComponent::SetBoardPitchExtraAngle -- Epic 0xfd6f40 / Steam 0xf96d70.
// Hooked ONLY to learn a trustworthy component pointer. Whole function disassembled: `(this, float
// in XMM1)`; the float is declared `double` and forwarded UNCONVERTED, per the standing thunk rule.
// Its `this` is the pitch SUB-OBJECT at comp+0x138, so comp = this - 0x138, and the derivation is
// self-checking because comp+0x340 must then be the skater already held.
static const char* SIG_SET_PITCH =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 08 02 00 00 F3 0F 59 89 F8 09 00 00 F3 0F 11 8B 4C 07 00 00";
enum { SUBOBJ_TO_COMP = 0x138, MC_OWNER_SKATER = 0x340 };

typedef void* (*ExtraPitchFn)(void*, void*, uint64_t, void*, void*);
typedef void  (*SetPitchFn)(void*, double);
static void* g_orig = nullptr, *g_start = nullptr;
// Exposed for pitch_range: see the header -- once we hook this function its byte signature no
// longer matches, so a later module cannot resolve the address by scanning for it.
const void* CatchLevel_ExtraPitchFn() { return g_start; }
static void* g_setOrig = nullptr, *g_setStart = nullptr;
static void* g_realComp = nullptr;      // learned + validated; the object that owns the pitch state
static float g_assertWant = -999.0f;    // the setpoint the post-physics assert drives toward
static volatile LONG g_assertWins = 0;  // frames the assert re-levelled (proves the race, in-log)

// ------------------------------------------------------------------ per-trick state (game thread)
static void*  g_skater     = nullptr;   // armed at trick fire, cleared when the window ends
static float  g_authored   = 0.0f;      // the trick's CatchTargetPitchAngle
static double g_armT       = 0.0;
static int    g_lastCatch  = 0;         // for the rising edge
static double g_catchT     = -1.0;      // when the catch engaged (-1 = not yet)
static bool   g_levelling  = false;
static float  g_startedMs  = 0.0f;   // when the ease began, relative to the catch (for the log)
static bool   g_reachedLevel = false; // hit tolerance at least once this trick (logging + re-take)
static float  g_wroteWant  = -1000.0f; // what we asked for last frame, to compare against what returns
static bool   g_loggedTrick= false;
static bool   g_sawLive    = false;   // the pitch block has read non-zero at least once, ever
static int    g_deadCatches= 0;       // consecutive all-zero catches while never having seen live
// QPC, not GetTickCount64: the latter steps in ~15.6 ms, which is coarser than a frame at 120 fps,
// so both the ease's dt and the "ms after the catch" figures were quantised to garbage.
static double NowSec() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// Learn the real component. Read-only with respect to the game: the original always runs, unchanged.
// The `comp+0x340 == skater` check is a DIAGNOSTIC, not a gate -- `this+0x208` is believed to be the
// skater but is not proved, and gating on an unproved fact would fail silently. The real judge is
// the pitch snapshot logged beside it: a live component cannot read all-zero mid-trick.
// The REAL USkateboardExMovementComponent, learned from SetBoardPitchExtraAngle's own `this` rather
// than from skater+0x550 (which points somewhere else -- every Ex-specific field read zero through
// it). Shared so other fixes do not have to re-derive it.
void* CatchLevel_MovementComponent() { return g_realComp; }

static void hkSetPitch(void* self, double angle) {
    __try {
        void* comp = (void*)((uint8_t*)self - SUBOBJ_TO_COMP);
        // Adopt OUR skater's component only. This fires for every skater, so in a co-op session a
        // remote player's component would be adopted here and then written to -- both by the
        // levelling in this module and by catch_tweaks' flip-rate stop, which takes this same
        // pointer. The owner check was already computed for the log below; here it is a gate.
        // Unknown local skater = accept, so solo is unaffected.
        void* const mineC = CatchTweaks_Skater();
        void* const ownerC = twkP(comp, MC_OWNER_SKATER);
        const bool oursC = !(mineC && ownerC && mineC != ownerC);
        if (oursC && comp != g_realComp) {
            const bool first = (g_realComp == nullptr);
            g_realComp = comp;
            if (first && g_log) {
                void* byOffset = g_skater ? twkP(g_skater, SK_MOVE_COMP) : nullptr;
                TwkLog("[level] component from SetBoardPitchExtraAngle: %p | skater+0x550 gave %p -> %s",
                       comp, byOffset, (comp == byOffset) ? "SAME (so the pointer was never the problem)"
                                                          : "DIFFERENT (that was the bug)");
                TwkLog("[level]   through it: pitch=%.2f mode=%d curT=%.3f totT=%.3f start=%.2f target=%.2f extra=%.2f | owner+0x340=%p skater=%p",
                       twkF(comp, MC_PITCH_NOW), twkB(comp, MC_PITCH_MODE),
                       twkF(comp, MC_PITCH_CUR_TIME), twkF(comp, MC_PITCH_TOTAL_TIME),
                       twkF(comp, MC_PITCH_START), twkF(comp, MC_PITCH_TARGET),
                       twkF(comp, MC_PITCH_EXTRA), twkP(comp, MC_OWNER_SKATER), g_skater);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { ((SetPitchFn)g_setOrig)(self, angle); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[level] caught fatal in SetBoardPitchExtraAngle -> recovered");
    }
}

static void* hkExtraPitch(void* self, void* trickDef, uint64_t inputType, void* inputs, void* out) {
    void* r = nullptr;
    __try { r = ((ExtraPitchFn)g_orig)(self, trickDef, inputType, inputs, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[level] caught fatal in GetBoardExtraPitchAngle -> recovered");
        return nullptr;
    }
    __try {
        void* skater = self ? twkP(self, FTH_SKATER) : nullptr;
        if (skater) {
            g_skater      = skater;
            g_authored    = trickDef ? twkF(trickDef, DEF_CATCH_TGT_PITCH) : 0.0f;
            if (g_authored < -180.0f || g_authored > 180.0f) g_authored = 0.0f;
            g_armT        = NowSec();
            g_lastCatch   = twkB(skater, SK_CATCH_ORIENT_STATE);
            g_catchT      = -1.0;
            g_levelling   = false;
            g_reachedLevel = false;
            g_wroteWant = -1000.0f;
            g_loggedTrick = false;
            if (g_log)
                TwkLog("[level] trick armed: authored CatchTargetPitch=%.2f (align smoothing %.2f)",
                       g_authored, trickDef ? twkF(trickDef, DEF_CATCH_ALIGN_SMOOTH) : 0.0f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_log = 0; }
    return r;
}

// The whole feature: watch for the catch, then keep the game's own integrator aimed at level.
void CatchLevel_PostPhysAssert() {
    __try {
        void* comp = g_realComp;
        const float want = g_assertWant;
        if (!comp || want < -900.0f || !g_on || !g_ok) return;
        // Only while a held-over stick is what keeps the trick's pitch trajectory alive -- a fresh
        // post-catch orient press makes HeldOverStale false and this stands down entirely.
        if (!CatchTweaks_HeldOverStale()) return;
        const float pitchNow = twkF(comp, MC_PITCH_NOW);
        if (!(pitchNow > -180.0f && pitchNow < 180.0f)) return;
        // Runs AFTER the physics pass (called from the UpdateFootAnchors detour inside the
        // animation update), so this frame's re-arm by the game has already written: easing the
        // rendered pitch from here means the leveller's value is what renders, and start/target
        // are re-seated so the game's integrator continues from it next tick instead of snapping.
        const float tau = ((g_responseMs > 5.0f) ? g_responseMs : 5.0f) / 1000.0f;
        const float alpha = 1.0f - expf(-0.0166f / tau);
        const float eased = pitchNow + (want - pitchNow) * alpha;
        *(float*)((uint8_t*)comp + MC_PITCH_NOW)    = eased;
        *(float*)((uint8_t*)comp + MC_PITCH_START)  = eased;
        *(float*)((uint8_t*)comp + MC_PITCH_TARGET) = want;
        InterlockedIncrement(&g_assertWins);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void CatchLevel_PumpFrame() {
    if (!g_skater) return;
    __try {
        void* skater = g_skater;
        const double now = NowSec();
        if (now - g_armT > 6.0) { g_skater = nullptr; g_assertWant = -999.0f; return; }   // stale trick
        // Use the LEARNED component, never `skater+0x550`. Writing the pitch block through
        // skater+0x550 does nothing: `target` reads back only the values written into it, the game
        // never overwrites it, and `pitch` sits at 0.00 at every catch -- that address is dead
        // memory for this class. `g_realComp` is derived from a function whose `this` is known by
        // disassembly and validated against the skater, so it is not a guess.
        void* comp = g_realComp;
        if (!comp) {
            static bool waited = false;
            if (!waited && g_log) { waited = true; TwkLog("[level] waiting to learn the real component (needs one trick to execute)"); }
            return;
        }

        const int   catchState = twkB(skater, SK_CATCH_ORIENT_STATE);
        const float pitchNow   = twkF(comp, MC_PITCH_NOW);
        const int   mode       = twkB(comp, MC_PITCH_MODE);
        const float target     = twkF(comp, MC_PITCH_TARGET);
        const float extra      = twkF(comp, MC_PITCH_EXTRA);
        if (pitchNow < -100000.0f) { g_skater = nullptr; return; }   // unreadable

        // ---- the offset sanity gate. An "obvious" component offset can read 0 on cases that
        // demonstrably work, so if the entire pitch block is zero at the catch the pointer is wrong
        // and nothing may be written through it.
        if (catchState != 0 && !g_loggedTrick) {
            g_loggedTrick = true;
            if (g_log)
                TwkLog("[level] CATCH: state=%d pitch=%.2f mode=%d target=%.2f extra=%.2f (authored %.2f)",
                       catchState, pitchNow, mode, target, extra, g_authored);
            // ---- compare with an epsilon, never `== 0.0f`: a dead pointer can hold tiny nonzero
            // values that merely PRINT as 0.00.
            const bool allDead = fabsf(pitchNow) < 1e-4f && mode == 0 &&
                                 fabsf(target) < 1e-4f && fabsf(extra) < 1e-4f;
            if (!allDead) g_sawLive = true;
            // A single all-zero read is NOT evidence of a bad pointer: a run-out resets the board
            // state, so the very next catch frame legitimately reads pitch/mode/target/extra all
            // zero. A wrong pointer reads zero ALWAYS. So complain only after several catches with
            // no live value ever seen, and even then take down `g_ok`, never the user's setting.
            if (allDead && !g_sawLive && ++g_deadCatches >= 6) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    TwkLog("[level] the pitch block has read ZERO on %d consecutive catches and never "
                           "once read live -- the component offsets do not describe this object. "
                           "Leveling paused (your setting is untouched; toggle it to retry).",
                           g_deadCatches);
                }
                g_ok = 0;
                g_skater = nullptr;
                return;
            }
        }

        // ---- rising edge of the catch = "the board hit the foot". Never a timer from trick start:
        // that is what made the original feature level mid-trick.
        if (catchState != 0 && g_lastCatch == 0 && g_catchT < 0.0) g_catchT = now;
        g_lastCatch = catchState;
        if (g_catchT < 0.0) return;                                   // not caught yet -- hands off

        // ---- LET GO once the board is not ours to aim any more. The setpoint below is rewritten
        // EVERY FRAME until the pitch arrives or `g_maxMs` expires, and neither of those noticed that
        // the trick had ended -- so a level still running when the rider landed kept forcing a pitch
        // target onto a board back on the ground, and the next ollie's crouch popped with the nose or
        // tail already lifted. Levelling belongs to the airborne window between the catch and the
        // landing; on the ground, and while a new trick is being set up, the pitch is the game's.
        if (FootPlace_Grounded() || FootPlace_SettingUpTrick()) {
            // Walking away is not enough: the last setpoint we wrote is still live, so the game's
            // integrator would go on pulling toward it. Park start AND target on the CURRENT pitch --
            // the interpolation then has nothing left to do and the board holds where it is, with no
            // jump. The mode is left alone; the game rewrites it at the next trick.
            if (g_levelling) {
                *(float*)((uint8_t*)comp + MC_PITCH_START)  = pitchNow;
                *(float*)((uint8_t*)comp + MC_PITCH_TARGET) = pitchNow;
                g_assertWant = -999.0f;
                if (g_assertWins) {
                    if (g_log) TwkLog("[level] post-phys assert won %d frames against a held-over "
                                      "stick's pitch drive", (int)g_assertWins);
                    g_assertWins = 0;
                }
                if (g_log)
                    TwkLog("[level] RELEASED at %.2f, %.1f deg from %.2f (%s) -- setpoint parked",
                           pitchNow, fabsf(pitchNow - ((g_targetDeg > -180.0f) ? g_targetDeg : g_authored)),
                           (g_targetDeg > -180.0f) ? g_targetDeg : g_authored,
                           FootPlace_SettingUpTrick() ? "setting up the next trick" : "landed");
            }
            g_skater = nullptr;
            return;
        }

        const double sinceCatch = (now - g_catchT) * 1000.0;
        if (sinceCatch < (double)g_delayMs) return;
        if (sinceCatch > (double)g_maxMs) {
            if (g_levelling && g_log)
                TwkLog("[level] GAVE UP at %.2f (target %.2f) -- CatchLevelMaxMs %.0f reached",
                       pitchNow, (g_targetDeg > -180.0f) ? g_targetDeg : g_authored, g_maxMs);
            g_skater = nullptr; g_assertWant = -999.0f; return;
        }
        if (!g_on || !g_ok) return;

        const float want = (g_targetDeg > -180.0f) ? g_targetDeg : g_authored;
        // BEING LEVEL RIGHT NOW IS NOT THE SAME AS BEING DONE, and treating it as done is what
        // made this fire only some of the time. This used to disarm the whole feature the moment the
        // pitch was within tolerance -- including on the FIRST eligible frame, before any levelling
        // had happened. The board is still turning and settling at the catch, so it passes through
        // level routinely; whether the feature ran at all came down to where the pitch happened to be
        // on that one frame. Nothing brought it back afterwards either, so a board that drifted off
        // level a frame later stayed off.
        // Now: within tolerance means "nothing to do THIS frame", not "finished". The arm is held
        // until the board is genuinely no longer ours -- landing, a new trick, or CatchLevelMaxMs.
        if (fabsf(pitchNow - want) < 0.25f) {
            if (g_levelling && !g_reachedLevel) {
                g_reachedLevel = true;
                if (g_log)
                    TwkLog("[level] LEVELLED to %.2f in %.0f ms (started +%.0f ms after the catch) "
                           "-- holding until landing", pitchNow, sinceCatch - g_startedMs, g_startedMs);
            }
            return;
        }
        if (g_reachedLevel) {                    // drifted back off level: take it again, quietly
            g_reachedLevel = false;
            if (g_log) TwkLog("[level] drifted to %.2f -- levelling again", pitchNow);
        }
        // Re-arm the game's integrator and aim it at level. Setpoint only -- the pose stays the
        // game's to write. currentTime is a FRACTION of totalTime, never 0 (the fixed point).
        // Time since the previous tick, for the frame-rate-independent ease. Clamped: a hitch must
        // not turn into a jump, and a zero/negative dt must not stall the approach.
        static double lastTick = 0.0;
        float dt = (lastTick > 0.0) ? (float)(now - lastTick) : (1.0f / 60.0f);
        lastTick = now;
        if (!(dt > 0.0f) || dt > 0.1f) dt = 1.0f / 60.0f;

        const float tau = g_responseMs / 1000.0f;
        float alpha = 1.0f - expf(-dt / tau);
        if (alpha < kMinAlpha) alpha = kMinAlpha; else if (alpha > 1.0f) alpha = 1.0f;
        const float resp = tau;
        // Setpoint only. `_pitchExtraAngle` is deliberately NOT touched: that is the FLIP TRICK's
        // pitch -- the boned/rocket angle from the pop flick -- and it has nothing to do with
        // catching. Writing it was tried against a suspected tug-of-war, changed the convergence rate
        // by nothing at all, and meddled with an unrelated system; do not reach for it again.
        *(uint8_t*)((uint8_t*)comp + MC_PITCH_MODE)       = (uint8_t)PITCH_MODE_SETTLE;
        *(float*)  ((uint8_t*)comp + MC_PITCH_START)      = pitchNow;
        *(float*)  ((uint8_t*)comp + MC_PITCH_TARGET)     = want;
        *(float*)  ((uint8_t*)comp + MC_PITCH_TOTAL_TIME) = resp;
        *(float*)  ((uint8_t*)comp + MC_PITCH_CUR_TIME)   = resp * alpha;
        // Whether our setpoint SURVIVES to the next frame is the whole question, and no summary can
        // answer it: this prints what came back against what we wrote, so an overwrite by the game is
        // visible as a value that is not ours rather than inferred from a convergence rate.
        if (g_traceLevel && g_wroteWant > -1000.0f)
            TwkLog("[level] t+%4.0fms pitch=%6.2f | came back: mode=%d tgt=%7.2f extra=%7.2f "
                   "| we wrote %6.2f (alpha %.3f, dt %.1f ms)",
                   sinceCatch, pitchNow, mode, target, extra, g_wroteWant, alpha, dt * 1000.0f);
        g_wroteWant = want;
        if (!g_levelling) {
            g_levelling = true;
            g_startedMs = (float)sinceCatch;
            InterlockedIncrement(&g_uiLevels);
            if (g_log) TwkLog("[level] levelling %.2f -> %.2f", pitchNow, want);
            g_assertWant = want;   // the post-physics assert mirrors the live setpoint
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_skater = nullptr; g_ok = 0;        // g_ok, NOT g_on -- a fault must not rewrite the ini
        TwkLog("[level] caught fatal while levelling -> paused (your setting is untouched; game unaffected)");
    }
}

void CatchLevel_Install() {
    // Every failure here takes down `g_ok`, NOT `g_on`. A missing sig after a game patch is a
    // property of the BUILD, not a preference: writing it to the ini would silently erase the
    // user's setting, and it would stay erased once the game was fixed.
    g_start = TwkScanExe(SIG_EXTRA_PITCH);
    if (!g_start) { TwkLog("[level] GetBoardExtraPitchAngle sig NOT FOUND -- catch leveling off (game updated?)"); g_ok = 0; return; }
    if (MH_CreateHook(g_start, (void*)&hkExtraPitch, &g_orig) != MH_OK || MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[level] hook failed on GetBoardExtraPitchAngle -- catch leveling off");
        g_start = nullptr; g_ok = 0; return;
    }
    // Second hook: its only purpose is to supply a component pointer that is not a guess.
    // Pass-through.
    g_setStart = TwkScanExe(SIG_SET_PITCH);
    if (!g_setStart) {
        TwkLog("[level] SetBoardPitchExtraAngle sig NOT FOUND -- no trustworthy component pointer, leveling off");
        g_ok = 0;
    } else if (MH_CreateHook(g_setStart, (void*)&hkSetPitch, &g_setOrig) != MH_OK ||
               MH_EnableHook(g_setStart) != MH_OK) {
        TwkLog("[level] hook failed on SetBoardPitchExtraAngle -- no trustworthy component pointer, leveling off");
        g_setStart = nullptr; g_ok = 0;
    }
    TwkLog("[level] installed @ %p (+ component probe @ %p) -- board levels when it hits your foot (currently %s)",
           g_start, g_setStart, (g_on && g_ok) ? "ON" : "off");
}

void CatchLevel_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    if (!g_start) { api->TextDisabled("Catch leveling: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Level the board on catch", &on)) { g_on = on ? 1 : 0; if (on) g_ok = 1; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(restores removed auto-level, catch-only)");
    if (on) {
        api->Indent();
        // A runtime pause must be VISIBLE, or the box says on while nothing happens.
        if (!g_ok) api->TextDisabled("PAUSED by a fault this session -- untick and retick to retry");
        float r = g_responseMs;
        if (api->SliderFloat("Ease speed (ms)", &r, 15.0f, 200.0f, "%.0f")) { g_responseMs = r; TwkMarkDirty(); }
        float d = g_delayMs;
        // 0..5, not 0..400: the field result is that raising this only makes levelling fire less
        // often, so the slider no longer offers a range that quietly breaks the feature.
        if (api->SliderFloat("Wait after catch (ms)", &d, 0.0f, kMaxDelayMs, "%.0f")) { g_delayMs = d; TwkMarkDirty(); }
        float t = (g_targetDeg > -180.0f) ? g_targetDeg : 0.0f;
        if (api->SliderFloat("Level angle (deg)", &t, -20.0f, 20.0f, "%.0f")) { g_targetDeg = t; TwkMarkDirty(); }
        snprintf(b, sizeof(b), "%s   levels applied: %d",
                 (g_targetDeg > -180.0f) ? "using your angle" : "using the trick's authored angle",
                 (int)g_uiLevels);
        api->TextDisabled(b);
        api->Unindent();
    }
}
