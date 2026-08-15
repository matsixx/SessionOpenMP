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
    COMP_WORLD_QUAT       = 0x1c0,  // ComponentToWorld.Rotation (FQuat)
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
static int      g_pitchDead = 0;
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

// The pitch lever, applied AFTER the game's Tick so the actor rotation it composes from is this
// frame's. The write is ABSOLUTE every frame -- actor rotation (game-owned, never ours) times the
// component's stock relative times the slider -- so it cannot compound and cannot feed back: nothing
// we wrote is ever an input. Publishing what an actor has reached instead of what it was told is the
// bug class this shape exists to avoid.
static void applyPitch(void* cam) {
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
        g_pitchApplied = 0;                          // fresh component: nothing of ours on it yet
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
    const int want = (g_pitchDeg != 0.0f) ? 1 : 0;
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
        const float pyr[3] = { g_pitchDeg, 0, 0 };
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

static void hkCameraTick(void* self, float dt) {
    if (!g_dead) {
        __try { applyFrame(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_dead = 1;
            TwkLog("[camh] FAULT applying camera data -- camera height module off for this run");
        }
    }
    ((void(*)(void*, float))g_origTick)(self, dt);
    if (!g_pitchDead) {
        __try { applyPitch(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_pitchDead = 1;
            TwkLog("[camh] FAULT applying camera pitch -- pitch slider off for this run");
        }
    }
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
    if (g_pitchDeg < -30.0f) g_pitchDeg = -30.0f;
    if (g_pitchDeg >  30.0f) g_pitchDeg =  30.0f;
}
void CameraHeight_SaveConfig(char* iniText, size_t cap) {
    TwkIniSetInt(iniText, cap, "CameraFollowHeight",    g_follow);
    TwkIniSetInt(iniText, cap, "CameraPitchBeforeDrop", g_pitchOnDrop);
    TwkIniSetInt(iniText, cap, "CameraDropDebug",       g_dropDebug);
    TwkIniSetInt(iniText, cap, "CameraPitchDeg",   (int)g_pitchDeg);
}
void CameraHeight_ResetDefaults() {
    g_follow = 0; g_pitchOnDrop = 1; g_dropDebug = 0; g_pitchDeg = 0;
    TwkMarkDirty();
}

// Every setter marks the config dirty ITSELF -- the auto-save fires on TwkMarkDirty(), and the pause
// menu writes through these accessors with no marking of its own. A setter that skips it is the
// shell's known quiet failure: the tweak works for the session and is gone on the next launch.
bool CameraHeight_FollowEnabled()            { return g_follow != 0; }
void CameraHeight_SetFollowEnabled(bool on)  { g_follow = on ? 1 : 0; TwkMarkDirty(); }
bool CameraHeight_PitchOnDropEnabled()       { return g_pitchOnDrop != 0; }
void CameraHeight_SetPitchOnDropEnabled(bool on) { g_pitchOnDrop = on ? 1 : 0; TwkMarkDirty(); }
float CameraHeight_PitchDeg()           { return g_pitchDeg; }
void CameraHeight_SetPitchDeg(float d)  {
    if (d < -30.0f) d = -30.0f;
    if (d >  30.0f) d =  30.0f;
    g_pitchDeg = d;
    TwkMarkDirty();
}

void CameraHeight_DrawMenu(const OmpMenuApi* api) {
    bool f = g_follow != 0, p = g_pitchOnDrop != 0, dbg = g_dropDebug != 0;
    if (api->Checkbox("Camera always follows height", &f)) CameraHeight_SetFollowEnabled(f);
    api->SameLine(); api->TextDisabled("(every air, not just onto higher obstacles)");
    if (api->Checkbox("Pitch camera before drop", &p)) CameraHeight_SetPitchOnDropEnabled(p);
    api->SameLine(); api->TextDisabled("(stock: tilt down at the edge instead of descending)");
    float pd = g_pitchDeg;
    if (api->SliderFloat("Pitch (deg, + looks up)", &pd, -30.0f, 30.0f, "%.0f")) CameraHeight_SetPitchDeg(pd);
    if (api->Checkbox("Draw the game's drop-detection debug", &dbg)) { g_dropDebug = dbg ? 1 : 0; TwkMarkDirty(); }
    if (g_data) {
        char b[160];
        snprintf(b, sizeof(b), "stock: flatAirMax %.0f/%.0f, dropDetect %d",
                 g_stockMaxLanding, g_stockMaxPop, (int)g_stockDropEnable);
        api->TextDisabled(b);
    }
    if (g_dead) api->TextDisabled("FAULTED this run -- see SessionTweaks.log");
}
