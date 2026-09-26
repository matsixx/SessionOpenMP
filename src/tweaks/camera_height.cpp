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
// SessionTweaks -- CAMERA HEIGHT FOLLOW.
//
// Symptom: the camera's height only tracks the skater when landing on something HIGHER than the
// launch point. Ordinary airs and drops leave the camera at its old height (drops pitch it down
// instead). Both behaviours are DATA, not emergent camera math:
//
//   ASkaterCameraActor::SetInAirCameraMode (Epic 0xf66070) classifies every air by measuring pop
//   height and drop height against four USessionCameraData thresholds:
//       _maximumLandingHeightForFlatAir (+0x2dc)   _maximumPopHeightForFlatAir (+0x2e0)
//       _minimumDropHeightForBigAir     (+0x2f0)   _minimumPopHeightForBigAir  (+0x2f4)
//   "Flat air" is the hold-height mode; the follow modes only engage past the thresholds -- which
//   is the reported asymmetry, described from the code's side.
//
//   ASkaterCameraActor::DropDetection (Epic 0xf51a90) is a whole feature whose job is pitching the
//   camera at a drop instead of descending with the skater, gated by _enableDropDetection (+0x308)
//   and configured by _dropDetectionDistance/_dropMinHeight/_dropMaxHeight (+0x30c/+0x314/+0x318).
//
// So the module never replaces camera math. It steers the game's own data asset -- reached through
// the camera actor's _cameraData (+0x7b0) -- and restores the stock values the moment a lever turns
// off. Both consumers re-read the asset every frame (SetInAirCameraMode fetches +0x7b0 fresh at
// 0xf66755; Tick movups-copies the drop block each pass), so a write takes effect immediately and a
// restore leaves no residue.
//
// The settings are independent, with no master switch: each is a separate thing the camera does, and
// each is written toward its own desired value every frame. Stock values are logged once on capture.
//
// POLARITY, easy to get backwards: "Pitch camera before drop" names the GAME'S behaviour, so ON
// means leave drop detection alone and OFF means we disable it. That is the opposite sense from the
// other setting, where ON means we act. The stored flag follows the LABEL so the menu can never lie;
// the negation happens once, at the point of use.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "camera_height.h"
#include "grind_pop.h"     // GrindPop_NameOfFName -- names the component class in the probe
#include "sit.h"           // Sit_FirstPersonView -- the seated first-person view this module writes
#include "cam_fp.h"        // CamFp_View -- first person while skating, the other view this module writes
#include "foot_place.h"    // FootPlace_AnimInstance -- riding, or walking around
#include "emote.h"         // Emote_AimView -- a throw held up: the over-the-shoulder aim this module writes
#include "catch_tweaks.h"  // CatchTweaks_Skater -- how far the camera is from you, for the aim's dolly
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    CAM_DATA              = 0x7b0,  // ASkaterCameraActor::_cameraData -> USessionCameraData*
    // USessionCameraData
    CD_FLATAIR_MAXLANDING = 0x2dc,  // _maximumLandingHeightForFlatAir (float)
    CD_FLATAIR_MAXPOP     = 0x2e0,  // _maximumPopHeightForFlatAir (float)
    CD_BIGAIR_MINDROP     = 0x2f0,  // _minimumDropHeightForBigAir (float) -- logged, not written
    CD_BIGAIR_MINPOP      = 0x2f4,  // _minimumPopHeightForBigAir (float)  -- logged, not written
    CD_DROP_ENABLE        = 0x308,  // _enableDropDetection (bool)
    CD_DROP_DIST          = 0x30c,  // _dropDetectionDistance (float) -- logged
    CD_DROP_MINH          = 0x314,  // _dropMinHeight (float) -- logged
    CD_DROP_MAXH          = 0x318,  // _dropMaxHeight (float) -- logged
    CD_DROP_DEBUG         = 0x341,  // _enableDropDetectionDebug (bool) -- the game's own visualiser
    // ACameraActor (the base class -- its size is 0x7b0, exactly where the skater camera's own
    // members begin, which is the layout proof)
    CAM_COMPONENT         = 0x228,  // UCameraComponent*
    // USceneComponent
    COMP_REL_ROT          = 0x128,  // RelativeRotation (FRotator) -- read once for the stock check
    AN_ON_BOARD           = 0x300,  // USkaterAnimInstance::IsOnBoard -- riding, not walking
    AN_THROWING_DOWN      = 0x302,  // USkaterAnimInstance::IsThrowingDown
    SK_THROWDOWN          = 0xa28,  // ASkaterCharacterBase::_isDoingThrowdown: set by DoBoardThrowdown, cleared on board
    COMP_WORLD_QUAT       = 0x1c0,  // ComponentToWorld.Rotation (FQuat)
    COMP_WORLD_POS        = 0x1d0,  // ComponentToWorld.Translation
    COMP_REL_LOC          = 0x11c,  // RelativeLocation (FVector)
    CAMC_FOV              = 0x1f8,  // UCameraComponent::FieldOfView
    ACTOR_ROOT            = 0x130,  // AActor::RootComponent
};

// The classifier takes fabs() of both heights before comparing (andps against the abs mask right
// ahead of the comiss at 0xf66774), so every measured height is non-negative and a NEGATIVE maximum
// can never be satisfied: nothing classifies as flat air, and the follow modes engage on every air.
static const float kNeverFlat = -100000.0f;

// USceneComponent::SetWorldRotation, the FQuat overload -- (comp, quat4, sweep, hit, teleport).
// The sig is the multiplayer mod's, dual-verified there and long proven on mesh components; the two
// mods scan independently, and this one only CALLS the function, so there is no hook to collide.
static const char* SIG_SET_WORLD_ROT =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 0F B6 F8 49 8B D9 4C 8B C2 48 8B F1 48 8D 54 24 40 E8";
typedef void (*SetWorldRotFn)(void* comp, const float* quat4, bool sweep, void* hit, unsigned char teleport);
// USceneComponent::SetWorldLocationAndRotation, the FQuat overload -- (comp, &loc3, &quat4, sweep, hit,
// teleport). Told from its FRotator twin by what it reads: a 16-byte movups off the rotation argument.
// Epic 0x2b669f0 / Steam 0x2b29230, 1-hit in both (sigmake).
static const char* SIG_SET_WORLD_LOCROT =
    "4C 8B DC 53 55 56 57 41 56 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00";
typedef void (*SetWorldLocRotFn)(void* comp, const float* loc3, const float* quat4, bool sweep, void* hit, unsigned char teleport);

// ASkaterCameraActor::Tick -- Epic 0xf69810 / Steam 0xf29620, 1-hit in both (sigmake).
// Hooked as the capture point and frame anchor: `this` IS the camera actor, and applying data writes
// here means they land between the game's reads, on the game thread, with no polling machinery.
static const char* SIG_CAMERA_TICK =
    "40 55 57 48 8D AC 24 18 FC FF FF 48 81 EC E8 04 00 00 44 0F 29 8C 24 80 04 00 00 48 8B 05 ?? "
    "?? ?? ?? 48 33 C4 48 89 85 60 03 00 00";

// ------------------------------------------------------------------ config (ini + menus)
static int g_follow      = 0; // CameraFollowHeight    -- no air classifies as "flat"
static int g_pitchOnDrop = 1; // CameraPitchBeforeDrop -- 1 = stock pitch kept, 0 = we disable it
static int g_dropDebug = 0;   // CameraDropDebug  -- the game's own drop visualiser, for field rounds
static float g_pitchDeg = 0;  // CameraPitchDeg   -- extra camera pitch, degrees; positive looks UP
static int   g_aimOn    = 1;    // CameraAimShoulder  -- holding LT to throw puts the camera over your shoulder
static float g_aimDolly = 50.0f;// CameraAimDollyPct  -- ...this much of the way in toward you
static float g_aimSide  = 45.0f;// CameraAimSideCm    -- ...and over to the side, away from the throwing arm
static float g_aimUp    = 65.0f;// CameraAimRaiseCm -- ...and up (the old CameraAimUpCm 6 is not read: too low, and a saved 6 would keep it there)
static float g_aimZoom  = 6.0f; // CameraAimZoomDeg   -- ...narrowed by this much
// THE ON-FOOT CAMERA (524). The game lets you set the riding camera but not the walking one. All at the stock
// values = the game's own camera, untouched.
static float g_footDist = 100.0f; // CameraFootDistPct  -- how far back, % of the game's own distance (100 = stock)
static float g_footUp   = 0.0f;   // CameraFootRaiseCm  -- straight up (world) or down
static float g_footSide = 0.0f;   // CameraFootSideCm   -- to the right (+) or left
static float g_footFov  = 0.0f;   // CameraFootFovDeg   -- added to the game's field of view
static float g_footTilt = 0.0f;   // CameraFootTiltDeg  -- + looks up
// SEATED (537): on top of the walking camera while the sit pose is held -- all at stock = the walking camera.
static float g_sitDist = 100.0f;  // CameraSitDistPct   -- % of the walking camera's distance
static float g_sitUp   = 0.0f;    // CameraSitRaiseCm   -- straight up (world) or down, added
static float g_sitSide = 0.0f;    // CameraSitSideCm    -- to the right (+) or left, added
static float g_sitFov  = 0.0f;    // CameraSitFovDeg    -- added to the field of view
static float g_sitTilt = 0.0f;    // CameraSitTiltDeg   -- + looks up, added
static bool SitCamOn() {
    return g_sitDist != 100.0f || g_sitUp != 0.0f || g_sitSide != 0.0f || g_sitFov != 0.0f || g_sitTilt != 0.0f;
}
static bool FootCamOn() {
    return g_footDist != 100.0f || g_footUp != 0.0f || g_footSide != 0.0f || g_footFov != 0.0f || g_footTilt != 0.0f;
}
static int   g_pitchBlendMs = 400;  // CameraPitchBlendMs -- how long the pitch takes to arrive when you
                                    //   get on the board, and to leave when you step off

// ------------------------------------------------------------------ live state (game thread only)
static uint8_t* g_hookAt   = nullptr;
static void*    g_origTick = nullptr;
// Stock values, captured per data-asset pointer BEFORE the first write. The pointer is the identity:
// a level change can re-instance the asset, and originals from a dead asset must never be written
// into a fresh one.
static void*    g_data = nullptr;
static float    g_stockMaxLanding = 0, g_stockMaxPop = 0;
static uint8_t  g_stockDropEnable = 0, g_stockDropDebug = 0;
// Edge memory, so state changes log once rather than per frame.
static int      g_airApplied = -1, g_dropApplied = -1, g_dbgApplied = -1;
// One-shot kill switch: a fault stops OUR writes and says so once; the game's Tick always runs.
static int      g_dead = 0;
// The pitch lever's own state and its OWN kill switch: a faulting nicety must never take the data
// levers down with it.
static SetWorldRotFn g_setWorldRot = nullptr;
static void*    g_pitchComp = nullptr;       // the camera component the stock relative was read from
static float    g_stockRel[3] = {0,0,0};     // its RelativeRotation (pitch/yaw/roll) before any write
static int      g_pitchApplied = 0;
static float    g_pitchW = 0.0f;        // 0 the game's framing .. 1 the slider's, eased across the mount
static int      g_pitchDead = 0;
// The seated first-person view's own state and its OWN kill switch.
static SetWorldLocRotFn g_setWorldLocRot = nullptr;
static void*    g_fpComp = nullptr;
static float    g_fpStockRelLoc[3] = {0,0,0}, g_fpStockRelRot[3] = {0,0,0};   // the component under the actor, as we found it
static float    g_fpStockFov = 0.0f;
static int      g_fpWrote = 0;
static int      g_fpDead = 0;
// The view forward this frame, published for the head look (sit.cpp).
static float    g_viewFwd[3] = {1,0,0};
static uint64_t g_viewFwdMs = 0;
// ---- the pitch PROBE. The slider visibly did nothing in the field, and five mechanisms would all
// look exactly like that from the outside: the value never reaching the module, a null component, the
// wrong component, the write being stomped by the game, or the view not reading the component at all.
// These lines tell them apart in one run; they only speak while the slider is non-zero (plus one
// component-identity line), and the per-frame ones are throttled to 1 Hz.
static float    g_probeLastSlider = 1e9f;    // edge: the value that last got logged
static float    g_lastWrote[4] = {0,0,0,1};  // what we wrote LAST frame, to test survival this frame
static int      g_haveLastWrote = 0;
static unsigned long long g_probeNextMs = 0;
static int      g_probeLines = 0;            // capped: diagnosis, not a running commentary

static int wrF(void* p, int off, float v) {
    __try { *(float*)((uint8_t*)p + off) = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int wrB(void* p, int off, uint8_t v) {
    __try { *(uint8_t*)((uint8_t*)p + off) = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The whole per-frame body. May fault (twk reads are guarded, but the writes deref a peer object);
// the caller catches and kills the module's writes, never the game's Tick.
static void applyFrame(void* cam) {
    void* data = twkP(cam, CAM_DATA);
    if (!data) return;

    if (data != g_data) {
        // First sight of this asset: capture stock BEFORE any write, and log it -- these numbers are
        // the measurement half of the feature, valid whatever the A/B verdict turns out to be.
        g_data = data;
        g_stockMaxLanding = twkF(data, CD_FLATAIR_MAXLANDING);
        g_stockMaxPop     = twkF(data, CD_FLATAIR_MAXPOP);
        g_stockDropEnable = (uint8_t)twkB(data, CD_DROP_ENABLE);
        g_stockDropDebug  = (uint8_t)twkB(data, CD_DROP_DEBUG);
        g_airApplied = g_dropApplied = g_dbgApplied = -1;        // re-assert against the fresh asset
        TwkLog("[camh] camera data %p captured: flatAirMax landing=%.1f pop=%.1f | bigAirMin drop=%.1f"
               " pop=%.1f | dropDetect=%d dist=%.1f minH=%.1f maxH=%.1f",
               data, g_stockMaxLanding, g_stockMaxPop,
               twkF(data, CD_BIGAIR_MINDROP), twkF(data, CD_BIGAIR_MINPOP),
               twkB(data, CD_DROP_ENABLE), twkF(data, CD_DROP_DIST),
               twkF(data, CD_DROP_MINH), twkF(data, CD_DROP_MAXH));
    }

    // Each lever is written toward its DESIRED value every frame (cheap: compare first), so the
    // module needs no transition handling -- enabling, disabling, resetting defaults and a reloaded
    // asset all converge through the same three statements. Logging is edge-only.
    const int wantAir = g_follow ? 1 : 0;
    if (wantAir != g_airApplied) {
        const float ml = wantAir ? kNeverFlat : g_stockMaxLanding;
        const float mp = wantAir ? kNeverFlat : g_stockMaxPop;
        if (wrF(data, CD_FLATAIR_MAXLANDING, ml) && wrF(data, CD_FLATAIR_MAXPOP, mp)) {
            g_airApplied = wantAir;
            TwkLog("[camh] flat-air classification %s (maxLanding=%.1f maxPop=%.1f)",
                   wantAir ? "DISABLED -- every air is followed" : "restored to stock", ml, mp);
        }
    }
    // The one negation: the setting is "let the game pitch", the write is "suppress the game's pitch".
    const int wantDrop = g_pitchOnDrop ? 0 : 1;
    if (wantDrop != g_dropApplied) {
        const uint8_t v = wantDrop ? 0 : g_stockDropEnable;
        if (wrB(data, CD_DROP_ENABLE, v)) {
            g_dropApplied = wantDrop;
            TwkLog("[camh] drop detection %s", wantDrop ? "DISABLED -- drops are followed, not pitched"
                                                        : "restored to stock");
        }
    }
    const int wantDbg = g_dropDebug ? 1 : 0;
    if (wantDbg != g_dbgApplied) {
        const uint8_t v = wantDbg ? 1 : g_stockDropDebug;
        if (wrB(data, CD_DROP_DEBUG, v)) g_dbgApplied = wantDbg;
    }
}

// UE's FRotator -> FQuat, verbatim (Rotator.cpp): needed to fold a non-identity stock relative in.
static void quatFromRotator(const float pyr[3], float q[4]) {
    const float d2r = 0.0174532925f * 0.5f;
    const float sp = sinf(pyr[0]*d2r), cp = cosf(pyr[0]*d2r);
    const float sy = sinf(pyr[1]*d2r), cy = cosf(pyr[1]*d2r);
    const float sr = sinf(pyr[2]*d2r), cr = cosf(pyr[2]*d2r);
    q[0] =  cr*sp*sy - sr*cp*cy;
    q[1] = -cr*sp*cy - sr*cp*sy;
    q[2] =  cr*cp*sy - sr*sp*cy;
    q[3] =  cr*cp*cy + sr*sp*sy;
}
static void quatMul(const float a[4], const float b[4], float r[4]) {
    r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    r[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    r[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// Riding, as opposed to walking around off the board. An unreadable state counts as NOT riding, so a
// bad read leaves the camera the game's rather than tilting it somewhere it was not asked to.
static bool OnBoard() {
    __try {
        void* a = FootPlace_AnimInstance();
        return a && twkB(a, AN_ON_BOARD) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// THE CAMERA'S "ON THE BOARD" starts with the throwdown (525). The game's own camera heads for its riding
// view as soon as you press Y; the animation says "on board" only once the throwdown is over. Keyed on the
// latter, the walking camera stayed on through the game's move and only then faded out, and the riding pitch
// only then faded in ("a weird zoom in first then smoothly moves back to my on board position").
static bool CamOnBoard() {
    if (OnBoard()) return true;
    __try {
        void* a = FootPlace_AnimInstance();
        void* sk = CatchTweaks_Skater();
        return (a && twkB(a, AN_THROWING_DOWN) != 0) || (sk && twkB(sk, SK_THROWDOWN) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The pitch lever, applied AFTER the game's Tick so the actor rotation it composes from is this
// frame's. The write is ABSOLUTE every frame -- actor rotation (game-owned, never ours) times the
// component's stock relative times the slider -- so it cannot compound and cannot feed back: nothing
// we wrote is ever an input. Publishing what an actor has reached instead of what it was told is the
// bug class this shape exists to avoid.
static void applyPitch(void* cam, float dt) {
    if (!g_setWorldRot) return;
    void* comp = twkP(cam, CAM_COMPONENT);
    if (!comp) return;
    if (g_pitchDeg != g_probeLastSlider) {
        g_probeLastSlider = g_pitchDeg;
        TwkLog("[camh] pitch slider -> %.0f deg (reached the module)", g_pitchDeg);
    }
    if (comp != g_pitchComp) {
        // Stock relative, captured BEFORE the first write ever lands on this component. Expected
        // identity (every camera-mode field is actor-level); said out loud if not, because then the
        // slider is composing on top of authored framing rather than on nothing.
        g_pitchComp = comp;
        g_stockRel[0] = twkF(comp, COMP_REL_ROT);
        g_stockRel[1] = twkF(comp, COMP_REL_ROT + 4);
        g_stockRel[2] = twkF(comp, COMP_REL_ROT + 8);
        g_pitchApplied = 0; g_pitchW = 0.0f;         // fresh component: nothing of ours on it yet
        if (g_stockRel[0] != 0 || g_stockRel[1] != 0 || g_stockRel[2] != 0)
            TwkLog("[camh] camera component %p has a NON-IDENTITY stock relative (%.2f %.2f %.2f)"
                   " -- folded into the pitch compose", comp, g_stockRel[0], g_stockRel[1], g_stockRel[2]);
        // Name the component's class: ACameraActor's member is declared UCameraComponent*, but
        // Session ships UCineCameraComponent too, and if the ACTIVE view component is a different
        // object entirely, every write here lands on scenery.
        {
            char cls[64] = {0};
            void* klass = twkP(comp, 0x10);                       // UObjectBase::ClassPrivate
            if (klass && GrindPop_NameOfFName((const char*)klass + 0x18, cls, sizeof(cls)))
                TwkLog("[camh] camera component class = %s", cls);
            else
                TwkLog("[camh] camera component class = <unresolved>");
        }
        g_haveLastWrote = 0;
    }
    // On the board only. Walking around, the camera is the game's -- and the seated first-person view
    // wants it untouched too (that path skips this one outright).
    //
    // The AMOUNT eases rather than switching, or getting on the board snapped the view by the whole
    // slider in one frame. The game has its own transition machinery for this change of camera
    // (ASkaterCameraActor::_currentCameraTransitionTime +0x994, _onBoardCameraModeRequested +0x988, a
    // _defaultTransitionCurve on the camera data) and it does not need to be decoded to be used: the
    // compose below starts from the ACTOR, which is exactly what that machinery moves, so the game's
    // own transition runs underneath this one and the two cannot fight. Nothing we write is an input.
    const float tgt = (g_pitchDeg != 0.0f && CamOnBoard()) ? 1.0f : 0.0f;
    if (dt > 0.0f && dt < 0.5f) {
        const float step = dt / ((g_pitchBlendMs > 30 ? (float)g_pitchBlendMs : 30.0f) * 0.001f);
        if (g_pitchW < tgt) { g_pitchW += step; if (g_pitchW > tgt) g_pitchW = tgt; }
        else if (g_pitchW > tgt) { g_pitchW -= step; if (g_pitchW < tgt) g_pitchW = tgt; }
    } else g_pitchW = tgt;
    const int want = (g_pitchW > 0.0005f) ? 1 : 0;
    if (!want && !g_pitchApplied) { g_haveLastWrote = 0; return; }   // stock and staying stock

    // SURVIVAL CHECK, before this frame's write: the component's rotation right now is whatever the
    // game's whole frame left it at. If it still equals what we wrote last frame, the write persists
    // and the view path is the suspect; if it snapped back, something re-writes the component and
    // that something is the real owner of this transform.
    const unsigned long long nowMs = GetTickCount64();
    if (g_haveLastWrote && want && nowMs >= g_probeNextMs && g_probeLines < 20) {
        g_probeLines++;
        float cur[4] = { twkF(comp, COMP_WORLD_QUAT),     twkF(comp, COMP_WORLD_QUAT + 4),
                         twkF(comp, COMP_WORLD_QUAT + 8), twkF(comp, COMP_WORLD_QUAT + 12) };
        float d = 0; for (int i = 0; i < 4; i++) { float e = cur[i] - g_lastWrote[i]; d += e*e; }
        // q and -q are the same rotation; treat a sign flip as a match.
        float d2 = 0; for (int i = 0; i < 4; i++) { float e = cur[i] + g_lastWrote[i]; d2 += e*e; }
        if (d2 < d) d = d2;
        TwkLog("[camh] probe: last write %s the game's frame (delta %.4f) | comp now (%.3f %.3f %.3f %.3f)",
               d < 0.0004f ? "SURVIVED" : "was STOMPED by", d, cur[0], cur[1], cur[2], cur[3]);
    }

    void* root = twkP(cam, ACTOR_ROOT);
    if (!root) return;
    float actorQ[4] = { twkF(root, COMP_WORLD_QUAT),     twkF(root, COMP_WORLD_QUAT + 4),
                        twkF(root, COMP_WORLD_QUAT + 8), twkF(root, COMP_WORLD_QUAT + 12) };
    float stockQ[4]; quatFromRotator(g_stockRel, stockQ);
    float base[4];   quatMul(actorQ, stockQ, base);
    float out[4];
    if (want) {
        const float w = g_pitchW * g_pitchW * (3.0f - 2.0f * g_pitchW);
        const float pyr[3] = { g_pitchDeg * w, 0, 0 };
        float pitchQ[4]; quatFromRotator(pyr, pitchQ);
        quatMul(base, pitchQ, out);                  // local pitch, about the camera's own right axis
    } else {
        out[0]=base[0]; out[1]=base[1]; out[2]=base[2]; out[3]=base[3];   // one restoring write
    }
    g_setWorldRot(comp, out, false, nullptr, 0);
    g_pitchApplied = want;
    if (want) {
        for (int i = 0; i < 4; i++) g_lastWrote[i] = out[i];
        g_haveLastWrote = 1;
        if (nowMs >= g_probeNextMs && g_probeLines < 20) {
            g_probeLines++;
            g_probeNextMs = nowMs + 1000;
            // Immediate readback: did the setter itself take the value? A refusal here (mobility, a
            // failed sweep) is a different diagnosis from a later stomp.
            float rb[4] = { twkF(comp, COMP_WORLD_QUAT),     twkF(comp, COMP_WORLD_QUAT + 4),
                            twkF(comp, COMP_WORLD_QUAT + 8), twkF(comp, COMP_WORLD_QUAT + 12) };
            float d = 0; for (int i = 0; i < 4; i++) { float e = rb[i] - out[i]; d += e*e; }
            float d2 = 0; for (int i = 0; i < 4; i++) { float e = rb[i] + out[i]; d2 += e*e; }
            if (d2 < d) d = d2;
            float da = 0; for (int i = 0; i < 4; i++) { float e = rb[i] - actorQ[i]; da += e*e; }
            float da2 = 0; for (int i = 0; i < 4; i++) { float e = rb[i] + actorQ[i]; da2 += e*e; }
            if (da2 < da) da = da2;
            TwkLog("[camh] probe: wrote pitch %.0f -> setter %s (delta %.4f) | comp-vs-actor delta %.4f"
                   " (0 = component just rides the actor)",
                   g_pitchDeg, d < 0.0004f ? "TOOK" : "REFUSED", d, da);
        }
    }
}

static void quatRotate(const float q[4], const float v[3], float r[3]) {   // v' = v + w*t + q x t, t = 2 q x v
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    r[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    r[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    r[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// The seated first-person view. sit.cpp says where the eyes are and which way they look, with a
// weight for the dolly in and out; this module owns the component, so it does the writing. The blend
// runs from the game's OWN view this frame to the eyes -- and the game's own view is the ACTOR (which
// the game keeps driving, post-Tick so it is current) under the component's stock relative, never the
// component's current transform, which after our first write is ours. The last write on the way out
// puts the component back on that stock relative. Returns true while it wrote; the pitch slider then
// stands aside for the frame.
// Blend one camera pose toward another: the position straight, the rotation the shorter way round.
static void blendView(const float pa[3], const float qa[4], const float pb[3], const float qb[4], float w,
                      float po[3], float qo[4]) {
    for (int i = 0; i < 3; i++) po[i] = pa[i] + (pb[i] - pa[i]) * w;
    float d = 0.0f; for (int i = 0; i < 4; i++) d += qa[i] * qb[i];
    const float sg = d < 0.0f ? -1.0f : 1.0f;
    float n = 0.0f;
    for (int i = 0; i < 4; i++) { qo[i] = qa[i] + (qb[i] * sg - qa[i]) * w; n += qo[i] * qo[i]; }
    n = n > 1e-8f ? 1.0f / sqrtf(n) : 1.0f;
    for (int i = 0; i < 4; i++) qo[i] *= n;
}

// TWO FIRST-PERSON SOURCES, layered: first person while skating (the Camera page) under the seated view
// (A while sitting). The seated one wins while it is up, and it blends in FROM the skating one rather
// than from the game's camera, so sitting down with both on never flicks the view out to third person
// and back in. Both are asked every tick so each keeps its own dolly and filters honest.
static bool applyFirstPerson(void* cam, float dt) {
    if (!g_setWorldLocRot) return false;
    void* comp = twkP(cam, CAM_COMPONENT);
    void* root = twkP(cam, ACTOR_ROOT);
    if (!comp || !root) return false;
    if (comp != g_fpComp) { g_fpComp = comp; g_fpWrote = 0; }     // a fresh component: nothing of ours on it
    float actorQ[4] = { twkF(root, COMP_WORLD_QUAT),     twkF(root, COMP_WORLD_QUAT + 4),
                        twkF(root, COMP_WORLD_QUAT + 8), twkF(root, COMP_WORLD_QUAT + 12) };
    float actorP[3] = { twkF(root, COMP_WORLD_POS), twkF(root, COMP_WORLD_POS + 4), twkF(root, COMP_WORLD_POS + 8) };
    float eyeK[3], lookK[4], wK = 0.0f, fovK = 0.0f;
    const bool skate = CamFp_View(actorQ, dt, eyeK, lookK, &wK, &fovK) && wK > 0.0f;
    float eyeS[3], lookS[4], wS = 0.0f, fovS = 0.0f;
    const bool seat = Sit_FirstPersonView(eyeS, lookS, &wS, &fovS) && wS > 0.0f;
    float wA = 0.0f; int sideA = 1;
    const bool aim = g_aimOn && Emote_AimView(&wA, &sideA) && wA > 0.0005f;
    // Off the board: the on-foot camera, eased in and out over 0.4 s so getting on or off never jumps.
    // Seated (537): the sitting camera's own settings ease in on top of the walking camera's, 0.5 s either way.
    static float s_footW = 0.0f, s_sitW = 0.0f;
    const bool seated = Sit_PoseHeld();
    {
        const float step = (dt > 0.0f && dt < 0.25f ? dt : 1.0f / 60.0f) / 0.4f;
        const float want = ((FootCamOn() || (SitCamOn() && (seated || s_sitW > 0.0f))) && !CamOnBoard()) ? 1.0f : 0.0f;
        s_footW += (want - s_footW > step) ? step : ((want - s_footW < -step) ? -step : want - s_footW);
        const float wantS = seated ? 1.0f : 0.0f, stepS = step * 0.8f;
        s_sitW += (wantS - s_sitW > stepS) ? stepS : ((wantS - s_sitW < -stepS) ? -stepS : wantS - s_sitW);
    }
    const float wF = s_footW * s_footW * (3.0f - 2.0f * s_footW);
    const bool foot = wF > 0.0005f;
    const float wSt = s_sitW * s_sitW * (3.0f - 2.0f * s_sitW);
    const float fDist = g_footDist * (1.0f + (g_sitDist * 0.01f - 1.0f) * wSt);
    const float fUp = g_footUp + g_sitUp * wSt, fSide = g_footSide + g_sitSide * wSt;
    const float fFov = g_footFov + g_sitFov * wSt, fTilt = g_footTilt + g_sitTilt * wSt;
    if (!skate && !seat && !aim && !foot) {
        if (g_fpWrote) {   // one restoring write: back under the actor, on the relative it had when we began
            float relQ[4]; quatFromRotator(g_fpStockRelRot, relQ);
            float off[3];  quatRotate(actorQ, g_fpStockRelLoc, off);
            const float loc[3] = { actorP[0] + off[0], actorP[1] + off[1], actorP[2] + off[2] };
            float q[4]; quatMul(actorQ, relQ, q);
            g_setWorldLocRot(comp, loc, q, false, nullptr, 0);
            if (g_fpStockFov > 0.0f) wrF(comp, CAMC_FOV, g_fpStockFov);
            g_fpWrote = 0;
            TwkLog("[camh] first person: camera handed back to the game");
        }
        return false;
    }
    if (!g_fpWrote) {
        for (int i = 0; i < 3; i++) { g_fpStockRelLoc[i] = twkF(comp, COMP_REL_LOC + 4 * i); g_fpStockRelRot[i] = twkF(comp, COMP_REL_ROT + 4 * i); }
        g_fpStockFov = twkF(comp, CAMC_FOV);
        TwkLog("[camh] first person: camera to the eyes (%s; stock relative (%.1f %.1f %.1f) / (%.1f %.1f %.1f), fov %.1f)",
               seat ? "seated" : skate ? "skating" : aim ? "over the shoulder, aiming a throw" : "your on-foot camera", g_fpStockRelLoc[0], g_fpStockRelLoc[1], g_fpStockRelLoc[2],
               g_fpStockRelRot[0], g_fpStockRelRot[1], g_fpStockRelRot[2], g_fpStockFov);
    }
    // the game's own view this frame, off the actor
    float relQ[4]; quatFromRotator(g_fpStockRelRot, relQ);
    float off[3];  quatRotate(actorQ, g_fpStockRelLoc, off);
    float gameP[3] = { actorP[0] + off[0], actorP[1] + off[1], actorP[2] + off[2] };
    float gameQ[4]; quatMul(actorQ, relQ, gameQ);
    // THE ON-FOOT CAMERA (524): the game's own walking view, moved back or in, up or down (world), to the side,
    // and tilted -- then everything else (the throw's aim, the seat) works from it. The game's view is clear
    // of walls; ours is traced from it and stops just short of anything in between.
    if (foot) {
        const float X[3] = { 1.0f, 0.0f, 0.0f }, Y[3] = { 0.0f, 1.0f, 0.0f };
        float fx[3], ry[3];
        quatRotate(gameQ, X, fx); quatRotate(gameQ, Y, ry);
        float d = 300.0f;
        void* sk = CatchTweaks_Skater();
        void* sroot = sk ? twkP(sk, ACTOR_ROOT) : nullptr;
        if (sroot) {
            const float dx = twkF(sroot, COMP_WORLD_POS) - gameP[0], dy = twkF(sroot, COMP_WORLD_POS + 4) - gameP[1],
                        dz = twkF(sroot, COMP_WORLD_POS + 8) + 40.0f - gameP[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!(d > 60.0f)) d = 60.0f; else if (d > 900.0f) d = 900.0f;
        }
        const float in = d * (1.0f - fDist * 0.01f) * wF;           // + = toward you
        float want[3];
        for (int i = 0; i < 3; i++) want[i] = gameP[i] + fx[i] * in + ry[i] * fSide * wF;
        want[2] += fUp * wF;
        // TRACED FROM THE BODY, LIKE A BOOM (536). "when angling the camera up (looking up) it sorta glitches a bit
        // and flickers to a different position for a small second. This doesnt happen when looking down." Looking up
        // swings the game's camera down to the floor, its own probe (simple collision) holding it just clear; the
        // trace started THERE, against complex geometry, and could hit within its first cm -- keeping 0 of the move
        // dropped the view onto the game's own spot for a frame. Now traced from the body (the point the distance is
        // measured from: always open air) to where the camera wants to be; a hit pulls it in at once, and it eases
        // back out when the way clears.
        static float s_reach = 1.0f;
        float from[3] = { gameP[0], gameP[1], gameP[2] };
        if (sroot) { from[0] = twkF(sroot, COMP_WORLD_POS); from[1] = twkF(sroot, COMP_WORLD_POS + 4); from[2] = twkF(sroot, COMP_WORLD_POS + 8) + 40.0f; }
        const float mv[3] = { want[0] - from[0], want[1] - from[1], want[2] - from[2] };
        const float ml = sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
        float frac = 1.0f;
        if (sk && ml > 1.0f) {
            float hit[3];
            if (Sit_TraceSurface(sk, from, want, hit, nullptr)) {
                const float hl = sqrtf((hit[0] - from[0]) * (hit[0] - from[0]) + (hit[1] - from[1]) * (hit[1] - from[1]) +
                                       (hit[2] - from[2]) * (hit[2] - from[2]));
                frac = fmaxf(0.0f, hl - 12.0f) / ml;
            }
        }
        const float easeOut = (dt > 0.0f && dt < 0.25f ? dt : 1.0f / 60.0f) / 0.25f;
        s_reach = frac < s_reach ? frac : fminf(frac, s_reach + easeOut);
        if (s_reach < 0.999f) for (int i = 0; i < 3; i++) want[i] = from[i] + mv[i] * s_reach;
        for (int i = 0; i < 3; i++) gameP[i] = want[i];
        if (fTilt != 0.0f) {
            // About the camera's own right axis; the sign is taken from which way the forward actually goes.
            const float a = fTilt * wF * 0.0174532925f * 0.5f;
            float qt[4] = { ry[0] * sinf(a), ry[1] * sinf(a), ry[2] * sinf(a), cosf(a) };
            float f2[3]; quatRotate(qt, fx, f2);
            if ((f2[2] > fx[2]) != (fTilt > 0.0f)) { qt[0] = -qt[0]; qt[1] = -qt[1]; qt[2] = -qt[2]; }
            float q2[4]; quatMul(qt, gameQ, q2);
            for (int i = 0; i < 4; i++) gameQ[i] = q2[i];
        }
    }
    // OVER THE SHOULDER while a throw is held up: the game's own view, its direction untouched (the throw goes
    // where the camera looks), brought in toward you and over to the side away from the throwing arm.
    if (aim) {
        const float X[3] = { 1.0f, 0.0f, 0.0f }, Y[3] = { 0.0f, 1.0f, 0.0f }, Z[3] = { 0.0f, 0.0f, 1.0f };
        float fx[3], ry[3], uz[3];
        quatRotate(gameQ, X, fx); quatRotate(gameQ, Y, ry); quatRotate(gameQ, Z, uz);
        float d = 300.0f;
        void* sk = CatchTweaks_Skater();
        void* sroot = sk ? twkP(sk, ACTOR_ROOT) : nullptr;
        if (sroot) {
            const float dx = twkF(sroot, COMP_WORLD_POS) - gameP[0], dy = twkF(sroot, COMP_WORLD_POS + 4) - gameP[1],
                        dz = twkF(sroot, COMP_WORLD_POS + 8) + 40.0f - gameP[2];
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!(d > 60.0f)) d = 60.0f; else if (d > 900.0f) d = 900.0f;
        }
        const float in = g_aimDolly * 0.01f * d * wA, side = (float)sideA * g_aimSide * wA, up = g_aimUp * wA;
        // Raised STRAIGHT UP in the world, not along the camera's own (pitched) up axis: the view's direction
        // is untouched, the camera just sits higher.
        for (int i = 0; i < 3; i++) gameP[i] += fx[i] * in + ry[i] * side;
        gameP[2] += up;
    }
    // game -> skating -> seated
    float p1[3], q1[4];
    if (skate) blendView(gameP, gameQ, eyeK, lookK, wK, p1, q1);
    else { for (int i = 0; i < 3; i++) p1[i] = gameP[i]; for (int i = 0; i < 4; i++) q1[i] = gameQ[i]; }
    float loc[3], q[4];
    if (seat) blendView(p1, q1, eyeS, lookS, wS, loc, q);
    else { for (int i = 0; i < 3; i++) loc[i] = p1[i]; for (int i = 0; i < 4; i++) q[i] = q1[i]; }
    g_setWorldLocRot(comp, loc, q, false, nullptr, 0);
    const float fovWant = (seat && fovS > 0.0f) ? fovS : ((skate && fovK > 0.0f) ? fovK : 0.0f);
    const float fovW    = (seat && fovS > 0.0f) ? wS   : wK;
    if (g_fpStockFov > 0.0f) {
        const float baseFov = g_fpStockFov + (foot ? fFov * wF : 0.0f);   // the on-foot camera's own (524)
        if (fovWant > 0.0f)  wrF(comp, CAMC_FOV, baseFov + (fovWant - baseFov) * fovW);
        else if (aim)        wrF(comp, CAMC_FOV, baseFov - g_aimZoom * wA);
        else                 wrF(comp, CAMC_FOV, baseFov);
    }
    g_fpWrote = 1;
    return true;
}

static void hkCameraTick(void* self, float dt) {
    // The camera actor keeps ticking when the input handler does not (the object dropper's prop editor,
    // menus), so it is where the sitting prompt finds out that its own pump has stopped.
    Sit_WatchdogTick();
    if (!g_dead) {
        __try { applyFrame(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_dead = 1;
            TwkLog("[camh] FAULT applying camera data -- camera height module off for this run");
        }
    }
    ((void(*)(void*, float))g_origTick)(self, dt);
    bool fp = false;
    if (!g_fpDead) {
        __try { fp = applyFirstPerson(self, dt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_fpDead = 1;
            TwkLog("[camh] FAULT in the seated first-person view -- off for this run");
        }
    }
    if (!fp && !g_pitchDead) {
        __try { applyPitch(self, dt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_pitchDead = 1;
            TwkLog("[camh] FAULT applying camera pitch -- pitch slider off for this run");
        }
    }
    // where the view looks now that everything that moves it has run
    __try {
        void* comp = twkP(self, CAM_COMPONENT);
        if (comp) {
            const float q[4] = { twkF(comp, COMP_WORLD_QUAT),     twkF(comp, COMP_WORLD_QUAT + 4),
                                 twkF(comp, COMP_WORLD_QUAT + 8), twkF(comp, COMP_WORLD_QUAT + 12) };
            const float x[3] = { 1.0f, 0.0f, 0.0f };
            float f[3]; quatRotate(q, x, f);
            if (fabsf(f[0]) <= 1.5f && fabsf(f[1]) <= 1.5f && fabsf(f[2]) <= 1.5f) {
                g_viewFwd[0] = f[0]; g_viewFwd[1] = f[1]; g_viewFwd[2] = f[2];
                g_viewFwdMs = GetTickCount64();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool CameraHeight_ViewForward(float out[3]) {
    if (!g_viewFwdMs || GetTickCount64() - g_viewFwdMs > 250) return false;
    out[0] = g_viewFwd[0]; out[1] = g_viewFwd[1]; out[2] = g_viewFwd[2];
    return true;
}

// ------------------------------------------------------------------ shell surface
void CameraHeight_Install() {
    g_hookAt = TwkScanExe(SIG_CAMERA_TICK);
    if (!g_hookAt) { TwkLog("[camh] camera Tick sig NOT FOUND -- camera height module off (game updated?)"); return; }
    if (MH_CreateHook(g_hookAt, (void*)&hkCameraTick, &g_origTick) != MH_OK ||
        MH_EnableHook(g_hookAt) != MH_OK) {
        TwkLog("[camh] hook failed on camera Tick -- camera height module off");
        g_hookAt = nullptr; return;
    }
    g_setWorldRot = (SetWorldRotFn)TwkScanExe(SIG_SET_WORLD_ROT);
    if (!g_setWorldRot) TwkLog("[camh] SetWorldRotation sig NOT FOUND -- pitch slider off, levers still work");
    g_setWorldLocRot = (SetWorldLocRotFn)TwkScanExe(SIG_SET_WORLD_LOCROT);
    if (!g_setWorldLocRot) TwkLog("[camh] SetWorldLocationAndRotation sig NOT FOUND -- seated and skating first person off");
    CamFp_Install();
    TwkLog("[camh] installed (camera Tick @ %p, followHeight=%d pitchBeforeDrop=%d pitch=%.0f deg)",
           g_hookAt, g_follow, g_pitchOnDrop, g_pitchDeg);
}

void CameraHeight_ReadConfig(const char* iniText) {
    // Migration from the older three-key layout (a master plus two levers, one of which was named
    // for OUR change rather than the game's). Its values are read only as DEFAULTS, so a present new
    // key always wins and the migration quietly stops mattering once the file has been rewritten.
    // Without this the same file would keep its numbers and change its meaning -- the rename would
    // silently flip the drop behaviour of anyone who had already set it.
    const int oldMaster = TwkIniInt(iniText, "CameraHeightFix",  -1);
    const int oldAir    = TwkIniInt(iniText, "CameraFollowAir",  -1);
    const int oldDrop   = TwkIniInt(iniText, "CameraFollowDrop", -1);
    int defFollow = 0, defPitchOnDrop = 1;          // fresh install: the stock camera, opt in per setting
    if (oldMaster == 0) {
        defFollow = 0; defPitchOnDrop = 1;          // master off meant a wholly stock camera
    } else if (oldMaster == 1) {
        if (oldAir  >= 0) defFollow      = oldAir;
        if (oldDrop >= 0) defPitchOnDrop = oldDrop ? 0 : 1;   // that lever was the inverse of this one
    }
    g_follow      = TwkIniInt(iniText, "CameraFollowHeight",    defFollow);
    g_pitchOnDrop = TwkIniInt(iniText, "CameraPitchBeforeDrop", defPitchOnDrop);
    g_dropDebug   = TwkIniInt(iniText, "CameraDropDebug",       0);
    g_pitchDeg  = (float)TwkIniInt(iniText, "CameraPitchDeg", 0);
    g_pitchBlendMs = TwkIniInt(iniText, "CameraPitchBlendMs", 400);
    g_aimOn    = TwkIniInt(iniText, "CameraAimShoulder", 1) ? 1 : 0;
    g_aimDolly = (float)TwkIniInt(iniText, "CameraAimDollyPct", 50);
    g_aimSide  = (float)TwkIniInt(iniText, "CameraAimSideCm", 45);
    g_aimUp    = (float)TwkIniInt(iniText, "CameraAimRaiseCm", 65);
    g_aimZoom  = (float)TwkIniInt(iniText, "CameraAimZoomDeg", 6);
    g_footDist = (float)TwkIniInt(iniText, "CameraFootDistPct", 100);
    g_footUp   = (float)TwkIniInt(iniText, "CameraFootRaiseCm", 0);
    g_footSide = (float)TwkIniInt(iniText, "CameraFootSideCm", 0);
    g_footFov  = (float)TwkIniInt(iniText, "CameraFootFovDeg", 0);
    g_footTilt = (float)TwkIniInt(iniText, "CameraFootTiltDeg", 0);
    g_sitDist  = (float)TwkIniInt(iniText, "CameraSitDistPct", 100);
    g_sitUp    = (float)TwkIniInt(iniText, "CameraSitRaiseCm", 0);
    g_sitSide  = (float)TwkIniInt(iniText, "CameraSitSideCm", 0);
    g_sitFov   = (float)TwkIniInt(iniText, "CameraSitFovDeg", 0);
    g_sitTilt  = (float)TwkIniInt(iniText, "CameraSitTiltDeg", 0);
    if (g_pitchDeg < -30.0f) g_pitchDeg = -30.0f;
    if (g_pitchDeg >  30.0f) g_pitchDeg =  30.0f;
    CamFp_ReadConfig(iniText);
}
void CameraHeight_SaveConfig(char* iniText, size_t cap) {
    TwkIniSetInt(iniText, cap, "CameraFollowHeight",    g_follow);
    TwkIniSetInt(iniText, cap, "CameraPitchBeforeDrop", g_pitchOnDrop);
    TwkIniSetInt(iniText, cap, "CameraDropDebug",       g_dropDebug);
    TwkIniSetInt(iniText, cap, "CameraPitchDeg",   (int)g_pitchDeg);
    TwkIniSetInt(iniText, cap, "CameraPitchBlendMs", g_pitchBlendMs);
    TwkIniSetInt(iniText, cap, "CameraAimShoulder", g_aimOn);
    TwkIniSetInt(iniText, cap, "CameraAimDollyPct", (int)g_aimDolly);
    TwkIniSetInt(iniText, cap, "CameraAimSideCm",   (int)g_aimSide);
    TwkIniSetInt(iniText, cap, "CameraAimRaiseCm",  (int)g_aimUp);
    TwkIniSetInt(iniText, cap, "CameraAimZoomDeg",  (int)g_aimZoom);
    TwkIniSetInt(iniText, cap, "CameraFootDistPct", (int)g_footDist);
    TwkIniSetInt(iniText, cap, "CameraFootRaiseCm", (int)g_footUp);
    TwkIniSetInt(iniText, cap, "CameraFootSideCm",  (int)g_footSide);
    TwkIniSetInt(iniText, cap, "CameraFootFovDeg",  (int)g_footFov);
    TwkIniSetInt(iniText, cap, "CameraFootTiltDeg", (int)g_footTilt);
    TwkIniSetInt(iniText, cap, "CameraSitDistPct",  (int)g_sitDist);
    TwkIniSetInt(iniText, cap, "CameraSitRaiseCm",  (int)g_sitUp);
    TwkIniSetInt(iniText, cap, "CameraSitSideCm",   (int)g_sitSide);
    TwkIniSetInt(iniText, cap, "CameraSitFovDeg",   (int)g_sitFov);
    TwkIniSetInt(iniText, cap, "CameraSitTiltDeg",  (int)g_sitTilt);
    CamFp_SaveConfig(iniText, cap);
}
void CameraHeight_ResetDefaults() {
    g_follow = 0; g_pitchOnDrop = 1; g_dropDebug = 0; g_pitchDeg = 0;
    g_aimOn = 1; g_aimDolly = 50.0f; g_aimSide = 45.0f; g_aimUp = 65.0f; g_aimZoom = 6.0f;   // the user's tuning (3.19.523)
    g_footDist = 100.0f; g_footUp = 0.0f; g_footSide = 0.0f; g_footFov = 0.0f; g_footTilt = 0.0f;
    g_sitDist = 100.0f; g_sitUp = 0.0f; g_sitSide = 0.0f; g_sitFov = 0.0f; g_sitTilt = 0.0f;
    CamFp_ResetDefaults();
    TwkMarkDirty();
}

// Every setter marks the config dirty ITSELF -- the auto-save fires on TwkMarkDirty(), and the pause
// menu writes through these accessors with no marking of its own. A setter that skips it is the
// shell's known quiet failure: the tweak works for the session and is gone on the next launch.
bool CameraHeight_FollowEnabled()            { return g_follow != 0; }
void CameraHeight_SetFollowEnabled(bool on)  { g_follow = on ? 1 : 0; TwkMarkDirty(); }
bool CameraHeight_PitchOnDropEnabled()       { return g_pitchOnDrop != 0; }
void CameraHeight_SetPitchOnDropEnabled(bool on) { g_pitchOnDrop = on ? 1 : 0; TwkMarkDirty(); }
static float CamClampI(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : floorf(v + 0.5f)); }
bool  CameraHeight_AimOn()              { return g_aimOn != 0; }
void  CameraHeight_SetAimOn(bool on)    { g_aimOn = on ? 1 : 0; TwkMarkDirty(); }
float CameraHeight_AimIn()              { return g_aimDolly; }
void  CameraHeight_SetAimIn(float v)    { g_aimDolly = CamClampI(v, 0.0f, 70.0f); TwkMarkDirty(); }
float CameraHeight_AimSide()            { return g_aimSide; }
void  CameraHeight_SetAimSide(float v)  { g_aimSide = CamClampI(v, 0.0f, 90.0f); TwkMarkDirty(); }
float CameraHeight_AimUp()              { return g_aimUp; }
void  CameraHeight_SetAimUp(float v)    { g_aimUp = CamClampI(v, 0.0f, 80.0f); TwkMarkDirty(); }
float CameraHeight_AimZoom()            { return g_aimZoom; }
void  CameraHeight_SetAimZoom(float v)  { g_aimZoom = CamClampI(v, 0.0f, 20.0f); TwkMarkDirty(); }
float CameraHeight_FootDist()           { return g_footDist; }
void  CameraHeight_SetFootDist(float v) { g_footDist = CamClampI(v, 50.0f, 200.0f); TwkMarkDirty(); }
float CameraHeight_FootUp()             { return g_footUp; }
void  CameraHeight_SetFootUp(float v)   { g_footUp = CamClampI(v, -60.0f, 120.0f); TwkMarkDirty(); }
float CameraHeight_FootSide()           { return g_footSide; }
void  CameraHeight_SetFootSide(float v) { g_footSide = CamClampI(v, -80.0f, 80.0f); TwkMarkDirty(); }
float CameraHeight_FootFov()            { return g_footFov; }
void  CameraHeight_SetFootFov(float v)  { g_footFov = CamClampI(v, -20.0f, 30.0f); TwkMarkDirty(); }
float CameraHeight_FootTilt()           { return g_footTilt; }
void  CameraHeight_SetFootTilt(float v) { g_footTilt = CamClampI(v, -20.0f, 20.0f); TwkMarkDirty(); }
float CameraHeight_SitDist()            { return g_sitDist; }
void  CameraHeight_SetSitDist(float v)  { g_sitDist = CamClampI(v, 30.0f, 200.0f); TwkMarkDirty(); }
float CameraHeight_SitUp()              { return g_sitUp; }
void  CameraHeight_SetSitUp(float v)    { g_sitUp = CamClampI(v, -80.0f, 120.0f); TwkMarkDirty(); }
float CameraHeight_SitSide()            { return g_sitSide; }
void  CameraHeight_SetSitSide(float v)  { g_sitSide = CamClampI(v, -80.0f, 80.0f); TwkMarkDirty(); }
float CameraHeight_SitFov()             { return g_sitFov; }
void  CameraHeight_SetSitFov(float v)   { g_sitFov = CamClampI(v, -20.0f, 30.0f); TwkMarkDirty(); }
float CameraHeight_SitTilt()            { return g_sitTilt; }
void  CameraHeight_SetSitTilt(float v)  { g_sitTilt = CamClampI(v, -20.0f, 20.0f); TwkMarkDirty(); }
float CameraHeight_PitchDeg()           { return g_pitchDeg; }
void CameraHeight_SetPitchDeg(float d)  {
    if (d < -30.0f) d = -30.0f;
    if (d >  30.0f) d =  30.0f;
    g_pitchDeg = d;
    TwkMarkDirty();
}

void CameraHeight_DrawMenu(const OmpMenuApi* api) {
    bool f = g_follow != 0, p = g_pitchOnDrop != 0, dbg = g_dropDebug != 0;
    api->Text("On board");
    if (api->Checkbox("Camera always follows height", &f)) CameraHeight_SetFollowEnabled(f);
    api->SameLine(); api->TextDisabled("(every air, not just onto higher obstacles)");
    if (api->Checkbox("Pitch camera before drop", &p)) CameraHeight_SetPitchOnDropEnabled(p);
    api->SameLine(); api->TextDisabled("(stock: tilt down at the edge instead of descending)");
    float pd = g_pitchDeg;
    if (api->SliderFloat("Pitch on the board (deg, + looks up)", &pd, -30.0f, 30.0f, "%.0f")) CameraHeight_SetPitchDeg(pd);
    if (api->Checkbox("Draw the game's drop-detection debug", &dbg)) { g_dropDebug = dbg ? 1 : 0; TwkMarkDirty(); }
    api->Separator();
    api->Text("Off board");
    api->SameLine(); api->TextDisabled("(walking around; all at stock = the game's own camera)");
    {
        float v = g_footDist;
        if (api->SliderFloat("Distance (%)##foot", &v, 50.0f, 200.0f, "%.0f")) CameraHeight_SetFootDist(v);
        api->SameLine(); api->TextDisabled("(100 = stock; lower is closer)");
        v = g_footUp;
        if (api->SliderFloat("Height (cm)##foot", &v, -60.0f, 120.0f, "%.0f")) CameraHeight_SetFootUp(v);
        v = g_footSide;
        if (api->SliderFloat("Side (cm)##foot", &v, -80.0f, 80.0f, "%.0f")) CameraHeight_SetFootSide(v);
        api->SameLine(); api->TextDisabled("(+ = to the right)");
        v = g_footFov;
        if (api->SliderFloat("Field of view (deg)##foot", &v, -20.0f, 30.0f, "%.0f")) CameraHeight_SetFootFov(v);
        v = g_footTilt;
        if (api->SliderFloat("Tilt (deg)##foot", &v, -20.0f, 20.0f, "%.0f")) CameraHeight_SetFootTilt(v);
        api->SameLine(); api->TextDisabled("(+ looks up)");
    }
    api->Text("Sitting");
    api->SameLine(); api->TextDisabled("(on top of the walking camera while seated; all at stock = the walking camera)");
    {
        float v = g_sitDist;
        if (api->SliderFloat("Distance (%)##sit", &v, 30.0f, 200.0f, "%.0f")) CameraHeight_SetSitDist(v);
        api->SameLine(); api->TextDisabled("(of the walking camera's)");
        v = g_sitUp;
        if (api->SliderFloat("Height (cm)##sit", &v, -80.0f, 120.0f, "%.0f")) CameraHeight_SetSitUp(v);
        v = g_sitSide;
        if (api->SliderFloat("Side (cm)##sit", &v, -80.0f, 80.0f, "%.0f")) CameraHeight_SetSitSide(v);
        v = g_sitFov;
        if (api->SliderFloat("Field of view (deg)##sit", &v, -20.0f, 30.0f, "%.0f")) CameraHeight_SetSitFov(v);
        v = g_sitTilt;
        if (api->SliderFloat("Tilt (deg)##sit", &v, -20.0f, 20.0f, "%.0f")) CameraHeight_SetSitTilt(v);
    }
    bool am = g_aimOn != 0;
    if (api->Checkbox("Over-the-shoulder aim (holding LT to throw the board)", &am)) { g_aimOn = am ? 1 : 0; TwkMarkDirty(); }
    if (am) {
        float v = g_aimDolly;
        if (api->SliderFloat("Aim: in toward you (%)", &v, 0.0f, 70.0f, "%.0f")) { g_aimDolly = floorf(v + 0.5f); TwkMarkDirty(); }
        v = g_aimSide;
        if (api->SliderFloat("Aim: over the shoulder (cm)", &v, 0.0f, 90.0f, "%.0f")) { g_aimSide = floorf(v + 0.5f); TwkMarkDirty(); }
        v = g_aimUp;
        if (api->SliderFloat("Aim: raise straight up (cm)", &v, 0.0f, 80.0f, "%.0f")) { g_aimUp = floorf(v + 0.5f); TwkMarkDirty(); }
        v = g_aimZoom;
        if (api->SliderFloat("Aim: zoom (deg)", &v, 0.0f, 20.0f, "%.0f")) { g_aimZoom = floorf(v + 0.5f); TwkMarkDirty(); }
    }
    if (g_data) {
        char b[160];
        snprintf(b, sizeof(b), "stock: flatAirMax %.0f/%.0f, dropDetect %d",
                 g_stockMaxLanding, g_stockMaxPop, (int)g_stockDropEnable);
        api->TextDisabled(b);
    }
    if (g_dead) api->TextDisabled("FAULTED this run -- see SessionTweaks.log");
    CamFp_DrawMenu(api);
}
