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
// SessionTweaks -- FOOT PLACEMENT. The shoe floats above the griptape instead of sitting on it.
//
// ---- THE CAUSE (measured) --------------------------------------------------------------------------
// `USkaterAnimInstance::AutoAdjustFoot` (Epic 0xf4f620) sweeps each foot's collision box downward on
// channel 23 and lifts the foot by whatever it hits. The sweep does not find the deck: the LEFT
// foot's sweep hits nothing at all (hit == sweep end, every sample) and the RIGHT foot hits 2.6 cm
// BELOW its own deck anchor -- the road being rolled over, not the board being stood on. That bogus
// hit lands in `_lastRightFootAutoOffset.Z` as roughly 0.56 cm and lifts the shoe off the griptape.
//
// The state was pinned by a differential the player could produce on demand: an IMPOSSIBLE clears
// the correction and nothing re-arms it until the next trick lands, so the offset reads 0.56 while
// the shoe hovers, exactly 0.00 after an impossible, and 0.56 again after the next kickflip. Only
// impossibles do it because only they run that catch path (CF_Tail -> CF_BothFeet, where a kickflip
// runs CF_LeftFoot/CF_RightFoot).
//
// ---- THE FIX ---------------------------------------------------------------------------------------
// Clear `FootIKAutoAdjust` (+0x3e8, a plain bool) for the duration of the animation update's own
// call, while on the board and grounded and not mid-catch. Saved on the way in and restored on the
// way out, so only that one read ever sees it and nothing the game owns is left modified -- walking,
// throwdowns and the catch pose all keep the vanilla behaviour. Correct for every shoe by
// construction, because it removes a wrong correction rather than adding a compensating one.
//
// This REPLACED per-shoe nudge sliders that compensated for the bug by hand, shoe by shoe. Those are
// gone: a fix at the cause needs no per-shoe data. (Old `FootNudge*` / `FootShoe*` lines may still
// sit in an existing SessionTweaks.ini -- nothing reads them, and nothing ever deletes an ini key.)
//
// ---- THE SEAM ---------------------------------------------------------------------------------------
// A hook on `USkaterAnimInstance::UpdateFootAnchors` (Epic 0xf6c370 / Steam 0xf2c180). It is the
// function that computes the foot anchors, it runs INSIDE NativeUpdateAnimation (call site
// 0xf5f1b9), and it has exactly one xref. Phase is the whole point: the anim update recomputes the
// foot from scratch every frame, so anything written from the input tick is gone before it renders.
// ARITY read at the call site, not assumed: `lea r9,[rbp-0x70] / movaps xmm1,xmm11 / lea
// r8,[rbp-0x60] / mov rcx,rdi` => (this, FLOAT dt, void*, void*). The float is declared `double` and
// forwarded UNCONVERTED, so the bits sit in the LOW 32 -- reinterpret, never convert.
//
// THIS MODULE OWNS THAT DETOUR. `foot_steer` installs no hook of its own and is CALLED from here
// instead, because MinHook overwrites the prologue and a second scan of a hooked function silently
// fails. Its offsets are applied through the same single write below.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "foot_place.h"
#include "foot_steer.h"       // shares this hook: only one detour may exist on UpdateFootAnchors
#include "catch_tweaks.h"     // CatchTweaks_Skater() -- the live skater, without a hook of our own
#include "catch_level.h"      // CatchLevel_PostPhysAssert() -- the post-physics level re-assert
#include "body_feel.h"       // BodyFeel_PostPhysApply() -- per-body blend-weight scaling
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets
enum {
    SK_MESH        = 0x280,   MESH_ANIM   = 0x6b0,
    AN_SKATER      = 0x608,   // USkaterAnimInstance::_skater -- whose anim instance this call is for
    AN_ON_BOARD    = 0x300,
    AN_CATCH_ST    = 0x312,   // CatchOrientState -- non-zero while a catch owns the feet
    AN_AUTOADJUST  = 0x3e8,   // FootIKAutoAdjust  <-- the fix
    AN_GROUNDED    = 0x5fa,
    // MEASURED FACTS from a REMOVED feature (second-foot smoothing, 2026-08-11). Kept because
    // each one cost a test round and any future foot work will hit them again:
    //   * HasLeftFootCatchOrient (+0x313) / HasRightFootCatchOrient (+0x314) read ZERO through real
    //     catches. "Which foot caught" is the SKATER's ECatchOrientState +0x63e (1 = left,
    //     2 = right, 5/10/11 = both, 7/8/9 = dark slide). The anim booleans look authoritative and
    //     are not -- do not gate on them.
    //   * IsLanding (+0x5fd) reads 1 through the whole DESCENT, not at touchdown. It is useless as
    //     an "airborne work is over" test; AN_GROUNDED is the real one.
    //   * IsBoardFlipping (+0x495) / IsBoardRotating (+0x496) are both FALSE by the time a catch
    //     registers (the flip has been stopped at grip-up), so "is this a trick" must be LATCHED
    //     over the air, not sampled at the catch.
    // The feature itself was removed: easing the non-catching foot back left the feet wrong after a
    // trick, and pinning it (tried twice before that) fought the authored whole-body catch pose.
    // The foot motion on a catch is ANIMATION -- no C++ foot function on USkaterAnimInstance reads a
    // catch field at all -- so anything here is fighting the anim graph. Look elsewhere.
    // The foot sockets the graph consumes. foot_steer writes through these; nothing else here does.
    AN_L_SOCK_LOC  = 0x404,   AN_R_SOCK_LOC = 0x41c,
    AN_L_SOCK_ROT  = 0x410,   AN_R_SOCK_ROT = 0x428,   // the socket rotators (FRotator, mesh space)
    AN_L_ALPHA     = 0x3fc,   AN_R_ALPHA    = 0x400,   // Left/RightFootIKAlpha: 0 = socket ignored
    AN_L_AUTO_OFF  = 0x718,   AN_R_AUTO_OFF = 0x724,   // _last{Left,Right}FootAutoOffset (FVector)
    SK_BOARD       = 0x568,   // ASkaterCharacterBase -> _skateboard (ASkateboardEx*)
    BOARD_FLIPPER  = 0x4e8,   // ASkateboardEx -> _flipper: the deck mesh -- its frame IS the deck's
    COMP_CTW_POS   = 0x1d0,   // USceneComponent ComponentToWorld translation (the quat sits at +0x1c0)
    AN_IS_SWITCH   = 0x303,   // USkaterAnimInstance::IsSkatingSwitch -- the reversed stances
    ACTOR_ROOT     = 0x130,   // AActor::RootComponent -- the skater's capsule, for its heading
    // The trick-setup crouch, exported for catch_level. Both are ASSET-GATED: they only mean
    // anything while CrankLoopBlendSpace is non-null, and read stale otherwise.
    AN_IS_CRANKING = 0x497,   AN_CRANK_BS = 0x4a8,
};

// ------------------------------------------------------------------ knobs
// The fix has no user-facing setting: it removes a wrong correction, so there is nothing to tune.
// The ini key remains as a kill switch in case a future game patch makes the suppression wrong.
static int g_on = 1;             // FootFixShoeHeight
static int g_ok = 1;             // runtime health, NEVER persisted (the catch_level lesson)
// FootFixSoleLiftMm: a constant lift of both foot sockets along the DECK's normal while riding (the
// same gate as the suppression). With the auto-adjust gone the foot sits exactly on the socket, and
// in the pocket -- where the kick curves up into the sole -- that reads as the shoe sinking a little
// into the deck (field). The auto-adjust's accidental ~0.5 cm had been masking it. Along the deck's
// normal rather than world up, so it stays a lift off the deck on a bank. 0 = off (the socket as the
// game places it). Millimetres, so the ini stays integer like every other key. 7 = the value the
// field settled on (0.7 cm: the pocket stops sinking, the flat does not start hovering).
static int g_soleLiftMm = 7;
// FootFixSoleLiftSwitchMm: the same lift while skating SWITCH (the anim's IsSkatingSwitch, which
// covers the reversed stances). Separate because the game's own switch placement already floats a
// touch at 0 (field: "the feet float slightly above when skating switch, always has"), so the value
// that is right for regular is wrong there -- and the right one may well be NEGATIVE, a drop. Default
// 0 = the game's own switch placement, exactly as before.
static int g_soleLiftSwMm = 0;
// FootFixProbe: while riding, print each foot's socket in the DECK's frame whenever it moves (x along
// the deck, z above the flipper pivot, the foot's axes against the deck normal). The one question it
// answers: does the socket FOLLOW the kick in the pocket (then only a sole lift is missing) or stay
// on the flat plane (then the kick geometry is). Change-triggered, at most four lines a second.
// Opt-in, like every other diagnostic in the mod.
static int g_probe = 0;
// FootFixSidewaysHold: while a shove has been stopped where it was caught (catch_tweaks'
// CatchShoveStopHold -- the board parked at an odd yaw, up to sideways), a foot socket that LEAPS
// more than FootFixSidewaysJumpCm in one frame keeps last frame's placement. Field: "a very sideways
// catch makes the feet freak out" -- both feet oscillating between two positions. The nose/tail TYPE
// is measured NOT to be it (type flips 0 on every such catch), so the next suspect is the socket
// itself: the game deciding each frame which end of a near-90-deg board each foot belongs to, and
// flipping. A jump filter fixes that if so and touches nothing otherwise: the ride's own socket
// motion is centimetres a frame, the flip-flop is tens. Released on touchdown.
// CONFIRMED (3.19.265 field): the socket was the oscillator and the hold stopped the freak-out. What
// remained: the hold kept whichever placement it saw FIRST, and when the game's first pick was the
// far end of a sideways board the wrong end got locked in ("the feet try to connect to the wrong
// socket"). So on a flip-flop the two placements are judged against the foot's RIDING socket --
// where that foot sits on the board when just rolling, captured every rolling frame -- and the nearer
// one wins: the near end is the end a real foot reaches for. FootFixSidewaysNearCm is the margin.
static int g_sideHold   = 1;
static int g_sideNearCm = 5;     // FootFixSidewaysNearCm -- how much nearer the other end must be to switch
// (FootFixSidewaysGlideCm/GlideDeg -- 6 cm / 10 deg per-frame gates meant to catch a GLIDE of the foot
// to the other end -- were built in 3.19.270 and REMOVED in 3.19.271: "the feet instantly move to the
// board when caught, and it still has the socket issue". The 20 cm jump filter is what ships.)
// (FootFixSidewaysPlace -- placing the feet ourselves from a deck-frame copy of the riding socket --
// was built in 3.19.268 and REMOVED in 3.19.269: it fired on near-perfect catches (a shove 6-27 deg
// "past" its mark is inside the render lag), chose ends per FOOT so one foot swapped while the other
// did not, and its rotation swept with the settling deck mesh. "Wrong socket placements on almost
// every catch." Any retry needs a per-BOARD end decision and a reference that does not move with the
// mesh, and must not run on catches within the render lag of their mark.)
static int g_sideJumpCm = 20;    // FootFixSidewaysJumpCm
// FootFixSidewaysTrace: 24 lines per sideways stop -- both sockets (loc + rot), alphas, and the
// per-frame jump -- so the oscillator is named from the log whether or not the hold catches it.
static int g_sideTrace  = 0;    // opt-in: 24 lines per sideways catch (1834 lines in one session)
void FootPlace_ReadConfig(const char* buf) {
    g_on = TwkIniInt(buf, "FootFixShoeHeight", 1) ? 1 : 0;
    g_soleLiftMm = TwkIniInt(buf, "FootFixSoleLiftMm", 7);
    if (g_soleLiftMm < -20) g_soleLiftMm = -20;
    if (g_soleLiftMm > 30)  g_soleLiftMm = 30;
    g_soleLiftSwMm = TwkIniInt(buf, "FootFixSoleLiftSwitchMm", 0);
    if (g_soleLiftSwMm < -20) g_soleLiftSwMm = -20;
    if (g_soleLiftSwMm > 30)  g_soleLiftSwMm = 30;
    g_probe = TwkIniInt(buf, "FootFixProbe", 0) ? 1 : 0;
    g_sideHold   = TwkIniInt(buf, "FootFixSidewaysHold", 1) ? 1 : 0;
    g_sideJumpCm = TwkIniInt(buf, "FootFixSidewaysJumpCm", 20);
    g_sideNearCm = TwkIniInt(buf, "FootFixSidewaysNearCm", 5);
    if (g_sideNearCm < 0) g_sideNearCm = 0; if (g_sideNearCm > 100) g_sideNearCm = 100;
    if (g_sideJumpCm < 3) g_sideJumpCm = 3; if (g_sideJumpCm > 200) g_sideJumpCm = 200;
    g_sideTrace  = TwkIniInt(buf, "FootFixSidewaysTrace", 0) ? 1 : 0;
}
void FootPlace_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "FootFixShoeHeight",      g_on);
    TwkIniSetInt(buf, cap, "FootFixSoleLiftMm",      g_soleLiftMm);
    TwkIniSetInt(buf, cap, "FootFixSoleLiftSwitchMm", g_soleLiftSwMm);
    TwkIniSetInt(buf, cap, "FootFixProbe",           g_probe);
    TwkIniSetInt(buf, cap, "FootFixSidewaysHold",    g_sideHold);
    TwkIniSetInt(buf, cap, "FootFixSidewaysJumpCm",  g_sideJumpCm);
    TwkIniSetInt(buf, cap, "FootFixSidewaysNearCm",  g_sideNearCm);
    TwkIniSetInt(buf, cap, "FootFixSidewaysTrace",   g_sideTrace);
}
void FootPlace_ResetDefaults()     { g_on = 1; g_ok = 1; g_soleLiftMm = 7; g_soleLiftSwMm = 0; g_probe = 0; }
bool FootPlace_Enabled()           { return g_on != 0; }
void FootPlace_SetEnabled(bool on) { g_on = on ? 1 : 0; if (on) g_ok = 1; TwkMarkDirty(); }

// ------------------------------------------------------------------ state
static void* g_skater = nullptr;
static void* g_anim   = nullptr;
static bool  g_installed = false;

// ------------------------------------------------------------------ the hook
static const char* SIG_UPDATE_ANCHORS =
    "40 55 53 56 57 48 8D AC 24 F8 F6 FF FF 48 81 EC 08 0A 00 00 44 0F 29 84 24 C0 09 00 00 48 8B 05 "
    "?? ?? ?? ?? 48 33 C4 48 89 85 40 08 00 00";
typedef void (*UpdateAnchorsFn)(void*, double, void*, void*);
static void* g_origAnchors = nullptr, *g_startAnchors = nullptr;
static volatile LONG g_faults = 0;
static volatile LONG g_corrections = 0;

// Is this the frame to suppress the auto-adjust? On the board, on the ground, and not mid-catch --
// the states where the sweep is wrong. Walking and catch poses keep the game's own behaviour.
static bool SuppressWanted(void* anim) {
    __try {
        return twkB(anim, AN_ON_BOARD) == 1 && twkB(anim, AN_GROUNDED) == 1 &&
               twkB(anim, AN_CATCH_ST) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The deck's normal in the MESH's space (where the sockets live), pointing skyward. The flipper's
// local X is the long axis (the flip axis) and Z is normal to the deck, but which way its Z points
// on this asset is not assumed: the sign is taken from world up, which is only safe while grounded
// on the board -- the only time this is used.
static bool DeckNormalMesh(void* a, float out[3]) {
    void* sk   = twkP(a, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    void* bd   = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* flp  = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    float qm[4], qf[4];
    if (!mesh || !flp || !TwkCompQuat(mesh, qm) || !TwkCompQuat(flp, qf)) return false;
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float nw[3];
    TwkQuatRotate(qf, z, nw);
    const float s = (nw[2] < 0.0f) ? -1.0f : 1.0f;
    nw[0] *= s; nw[1] *= s; nw[2] *= s;
    TwkQuatInvRotate(qm, nw, out);
    return true;
}

// FootFixProbe (see the knob). x along the deck, y across, z above the flipper's pivot along its
// normal (sky-positive); n(...) = the foot's own X/Y/Z axes dotted with that normal -- whichever
// reads +-1 on the flat is the sole's normal, and how it moves in the pocket is the sole's tilt.
// Reads the sockets AFTER this frame's write, so the sole lift shows in z.
static void ProbeDeckFrame(void* a) {
    static LARGE_INTEGER fq = {};
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    static LONGLONG last = 0;
    static float px[2] = { -999.0f, -999.0f }, pz[2] = { -999.0f, -999.0f }, pn[2][3] = {};
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (last && (t.QuadPart - last) < fq.QuadPart / 4) return;
    void* sk   = twkP(a, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    void* bd   = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* flp  = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    float qm[4], qf[4];
    if (!mesh || !flp || !TwkCompQuat(mesh, qm) || !TwkCompQuat(flp, qf)) return;
    float tm[3], tf[3];
    for (int i = 0; i < 3; i++) {
        tm[i] = twkF(mesh, COMP_CTW_POS + i * 4);
        tf[i] = twkF(flp,  COMP_CTW_POS + i * 4);
        if (fabsf(tm[i]) > 1e6f || fabsf(tf[i]) > 1e6f) return;
    }
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float nw[3]; TwkQuatRotate(qf, z, nw);
    const float s = (nw[2] < 0.0f) ? -1.0f : 1.0f;
    // Reference sanity. Most of the first field log read the foot HUNDREDS of cm above the "deck",
    // climbing linearly, with x and y sane: one of the two translations was not where its component
    // renders. Printed as the mesh-to-flipper distance, and a line whose heights are implausible is
    // counted rather than printed, so the log is not 2500 lines of a stale reference.
    static int skipped = 0;
    const float ref = sqrtf((tm[0] - tf[0]) * (tm[0] - tf[0]) + (tm[1] - tf[1]) * (tm[1] - tf[1]) +
                            (tm[2] - tf[2]) * (tm[2] - tf[2]));
    // The skater's heading against the deck's long axis: +1 = the deck's +X points where the
    // skater faces, -1 = the other way. Says whether "switch" turns the board or the skater.
    float fx = 0.0f;
    {
        void* rt = sk ? twkP(sk, ACTOR_ROOT) : nullptr;
        float qr[4];
        if (rt && TwkCompQuat(rt, qr)) {
            const float e[3] = { 1.0f, 0.0f, 0.0f };
            float fw[3], xw[3];
            TwkQuatRotate(qr, e, fw);
            TwkQuatRotate(qf, e, xw);
            fx = fw[0] * xw[0] + fw[1] * xw[1] + fw[2] * xw[2];
        }
    }
    const int sw = twkB(a, AN_IS_SWITCH);
    float x[2], y[2], zz[2], n[2][3], au[2];
    for (int f = 0; f < 2; f++) {
        const int locOff = f ? AN_R_SOCK_LOC : AN_L_SOCK_LOC;
        const int rotOff = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
        float sock[3], rot[3];
        for (int i = 0; i < 3; i++) {
            sock[i] = twkF(a, locOff + i * 4);
            rot[i]  = twkF(a, rotOff + i * 4);
            if (fabsf(sock[i]) > 1e5f || fabsf(rot[i]) > 1e5f) return;
        }
        float w[3]; TwkQuatRotate(qm, sock, w);
        const float rel[3] = { w[0] + tm[0] - tf[0], w[1] + tm[1] - tf[1], w[2] + tm[2] - tf[2] };
        float d[3]; TwkQuatInvRotate(qf, rel, d);
        x[f] = d[0]; y[f] = d[1]; zz[f] = d[2] * s;
        float qs[4]; TwkRotatorToQuat(rot, qs);
        for (int ax = 0; ax < 3; ax++) {
            float e[3] = { 0.0f, 0.0f, 0.0f }; e[ax] = 1.0f;
            float em[3], ew[3];
            TwkQuatRotate(qs, e, em);       // the foot's axis in mesh space
            TwkQuatRotate(qm, em, ew);      // ... in world
            n[f][ax] = (ew[0] * nw[0] + ew[1] * nw[1] + ew[2] * nw[2]) * s;
        }
        au[f] = twkF(a, (f ? AN_R_AUTO_OFF : AN_L_AUTO_OFF) + 8);
    }
    if (fabsf(zz[0]) > 60.0f || fabsf(zz[1]) > 60.0f) { skipped++; return; }   // a stale reference
    bool changed = false;
    for (int f = 0; f < 2; f++) {
        if (fabsf(zz[f] - pz[f]) >= 0.3f || fabsf(x[f] - px[f]) >= 3.0f) changed = true;
        for (int ax = 0; ax < 3; ax++) if (fabsf(n[f][ax] - pn[f][ax]) >= 0.05f) changed = true;
    }
    if (!changed) return;
    last = t.QuadPart;
    for (int f = 0; f < 2; f++) {
        px[f] = x[f]; pz[f] = zz[f];
        for (int ax = 0; ax < 3; ax++) pn[f][ax] = n[f][ax];
    }
    TwkLog("[foot] deck-frame sw=%d fwd.X=%+.2f ref=%.0f  L x=%.1f y=%.1f z=%.1f n(%.2f,%.2f,%.2f) "
           "auto=%.2f | R x=%.1f y=%.1f z=%.1f n(%.2f,%.2f,%.2f) auto=%.2f | lift %.1f cm | %d stale skipped",
           sw, fx, ref,
           x[0], y[0], zz[0], n[0][0], n[0][1], n[0][2], au[0],
           x[1], y[1], zz[1], n[1][0], n[1][1], n[1][2], au[1],
           (float)((sw > 0) ? g_soleLiftSwMm : g_soleLiftMm) * 0.1f, skipped);
}

static void hkUpdateFootAnchors(void* self, double dt, void* a, void* b) {
    // OUR SKATER ONLY, and decided BEFORE the original runs, because the fix writes on the way in.
    // This hook fires once per SKATER, and in a co-op session that includes remote players' proxies:
    // without the gate our writes land on their feet and every per-frame static foot_steer keeps
    // gets driven alternately by us and by them. Measured: the rate doubles with one proxy, and a
    // remote player being airborne armed foot steering while the local player stood still.
    // An unknown local skater (before the catch system has run once) means solo behaviour, not off.
    bool mine = true;
    if (self) {
        void* m = CatchTweaks_Skater();
        void* t = twkP(self, AN_SKATER);
        if (m && t && m != t) mine = false;
    }
    // The post-physics level re-assert rides this detour because it runs INSIDE the animation
    // update, after the physics pass -- the one phase where a pitch write beats the game's per-frame
    // trajectory re-arm to the renderer. Our skater only; the function gates itself further.
    if (mine) CatchLevel_PostPhysAssert();
    if (mine) BodyFeel_PostPhysApply();     // per-body physics-blend scaling (same surviving write point)
    if (mine) CatchTweaks_PostPhysHold();   // the scoop-foot hold (same surviving write point)
    unsigned char savedAA = 0;
    bool suppressed = false;
    if (mine && g_ok && g_on && self && SuppressWanted(self)) {
        __try {
            unsigned char* aa = (unsigned char*)self + AN_AUTOADJUST;
            savedAA = *aa;
            *aa = 0;
            suppressed = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { suppressed = false; }
    }
    __try { ((UpdateAnchorsFn)g_origAnchors)(self, dt, a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (suppressed) { __try { *((unsigned char*)self + AN_AUTOADJUST) = savedAA; }
                          __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[foot] caught fatal in UpdateFootAnchors -> recovered");
        return;
    }
    if (suppressed) {
        __try { *((unsigned char*)self + AN_AUTOADJUST) = savedAA; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        // Announce once. A correction that writes to the game must be able to prove it ran, or
        // "it does not work" is indistinguishable from "the code never executed".
        if (InterlockedIncrement(&g_corrections) == 1)
            TwkLog("[foot] shoe-height fix is live -- the foot auto-adjust is suppressed while riding");
    }
    if (!g_ok || !self || !mine) return;

    // ---- foot_steer rides this hook. It has no detour of its own, so its offsets are applied here,
    // through the same single write as the sole lift below.
    __try {
        // `dt` is a FLOAT forwarded through a `double` parameter (the thunk rule keeps the bits
        // unconverted for the original), so the float sits in the LOW 32 BITS. Casting reads garbage
        // -- it once gave ~0 and froze a blend at zero. REINTERPRET, never convert.
        uint64_t dtBits; memcpy(&dtBits, &dt, 8);
        uint32_t dtLo = (uint32_t)dtBits;
        float realDt; memcpy(&realDt, &dtLo, 4);
        if (!(realDt > 0.0f) || realDt > 0.25f) realDt = 1.0f / 60.0f;   // NaN/garbage fallback

        float dL[3] = { 0.0f, 0.0f, 0.0f };
        float dR[3] = { 0.0f, 0.0f, 0.0f };
        FootSteer_AddOffset(self, realDt, dL, dR);      // false = nothing of its own to add
        // ---- the sole lift (FootFixSoleLiftMm): both feet along the deck's normal, riding only --
        // the same states the suppression covers. Per foot, weighted by the game's own IK alpha the
        // way foot_steer is: a zeroed socket plus a delta is not an offset.
        const bool riding = SuppressWanted(self);
        const int  liftMm = (twkB(self, AN_IS_SWITCH) > 0) ? g_soleLiftSwMm : g_soleLiftMm;
        if (riding && liftMm != 0) {
            float nm[3];
            if (DeckNormalMesh(self, nm)) {
                const float lift = (float)liftMm * 0.1f;
                float wL = twkF(self, AN_L_ALPHA), wR = twkF(self, AN_R_ALPHA);
                if (!(wL >= 0.0f && wL <= 1.0f)) wL = 1.0f;
                if (!(wR >= 0.0f && wR <= 1.0f)) wR = 1.0f;
                if (wL < 0.02f) wL = 0.0f;
                if (wR < 0.02f) wR = 0.0f;
                for (int i = 0; i < 3; i++) { dL[i] += nm[i] * lift * wL; dR[i] += nm[i] * lift * wR; }
            }
        }
        if (dL[0] != 0.0f || dL[1] != 0.0f || dL[2] != 0.0f ||
            dR[0] != 0.0f || dR[1] != 0.0f || dR[2] != 0.0f) {
            // A DELTA on what the game just computed, never an absolute placement: absolute height
            // was tried and jittered and clipped through the deck, because the animation's own
            // micro-motion IS the desired motion. Offset preserves it; replacing it fights it every
            // frame.
            for (int i = 0; i < 3; i++) {
                *(float*)((uint8_t*)self + AN_L_SOCK_LOC + i * 4) = twkF(self, AN_L_SOCK_LOC + i * 4) + dL[i];
                *(float*)((uint8_t*)self + AN_R_SOCK_LOC + i * 4) = twkF(self, AN_R_SOCK_LOC + i * 4) + dR[i];
            }
        }
        if (g_probe && riding) ProbeDeckFrame(self);

        // ---- the sideways-catch socket hold (FootFixSidewaysHold) ---------------------------------
        {
            static bool  have[2] = { false, false }, haveRide[2] = { false, false };
            static float held[2][6];               // loc xyz + rot pyr per foot, the last accepted frame
            static float ride[2][3];               // each foot's socket while just rolling -- the natural end
            static int   rej[2] = { 0, 0 }, sw[2] = { 0, 0 }, frames = 0, jumps = 0;
            const bool grounded = twkB(self, AN_GROUNDED) != 0;
            const bool holding  = g_sideHold && CatchTweaks_ShoveStopHold() && !grounded;
            if (!holding && riding && grounded) {
                // The riding placement, refreshed every rolling frame and frozen the moment the board
                // leaves the ground: a foot the IK is placing (alpha > 0.5) on a grounded, ridden board.
                const int locOffR[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC };
                const int alpOffR[2] = { AN_L_ALPHA,    AN_R_ALPHA };
                for (int f = 0; f < 2; f++) {
                    if (twkF(self, alpOffR[f]) > 0.5f) {
                        for (int i = 0; i < 3; i++) ride[f][i] = twkF(self, locOffR[f] + i * 4);
                        haveRide[f] = true;
                    }
                }
            }
            if (holding) {
                const int locOff[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC };
                const int rotOff[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
                const int alpOff[2] = { AN_L_ALPHA,    AN_R_ALPHA };
                float jump[2] = { 0.0f, 0.0f };
                float cur[2][6];
                for (int f = 0; f < 2; f++) {
                    for (int i = 0; i < 3; i++) cur[f][i]     = twkF(self, locOff[f] + i * 4);
                    for (int i = 0; i < 3; i++) cur[f][3 + i] = twkF(self, rotOff[f] + i * 4);
                    const float alpha = twkF(self, alpOff[f]);
                    if (have[f]) {
                        const float dx = cur[f][0] - held[f][0], dy = cur[f][1] - held[f][1], dz = cur[f][2] - held[f][2];
                        jump[f] = sqrtf(dx * dx + dy * dy + dz * dz);
                    }
                    // Only a foot the IK is actually placing (alpha > 0.5) is held; a foot still in
                    // the catch pose is left to the animation.
                    if (alpha > 0.5f && have[f] && jump[f] > (float)g_sideJumpCm) {
                        // A flip-flop. Of the two placements on offer, the one nearer the foot's riding
                        // socket wins -- the near end. Without a riding socket to judge by, the first
                        // placement stands (the old rule).
                        float dNew = 0.0f, dHeld = 0.0f;
                        if (haveRide[f]) {
                            for (int i = 0; i < 3; i++) {
                                const float a = cur[f][i] - ride[f][i], b = held[f][i] - ride[f][i];
                                dNew += a * a; dHeld += b * b;
                            }
                            dNew = sqrtf(dNew); dHeld = sqrtf(dHeld);
                        }
                        if (haveRide[f] && dNew + (float)g_sideNearCm < dHeld) {
                            for (int i = 0; i < 6; i++) held[f][i] = cur[f][i];     // switch to the nearer end
                            ++sw[f];
                        } else {
                            for (int i = 0; i < 3; i++) *(float*)((uint8_t*)self + locOff[f] + i * 4) = held[f][i];
                            for (int i = 0; i < 3; i++) *(float*)((uint8_t*)self + rotOff[f] + i * 4) = held[f][3 + i];
                            ++rej[f];
                        }
                        ++jumps;
                    } else {
                        for (int i = 0; i < 6; i++) held[f][i] = cur[f][i];
                        have[f] = true;
                    }
                }
                if (g_sideTrace && frames < 24)
                    TwkLog("[foot] side f%02d | L (%.0f,%.0f,%.0f) r(%.0f,%.0f,%.0f) a=%.2f jump %.0f%s | R (%.0f,%.0f,%.0f) "
                           "r(%.0f,%.0f,%.0f) a=%.2f jump %.0f%s", frames,
                           cur[0][0], cur[0][1], cur[0][2], cur[0][3], cur[0][4], cur[0][5], twkF(self, AN_L_ALPHA), jump[0],
                           (jump[0] > (float)g_sideJumpCm) ? " HELD" : "",
                           cur[1][0], cur[1][1], cur[1][2], cur[1][3], cur[1][4], cur[1][5], twkF(self, AN_R_ALPHA), jump[1],
                           (jump[1] > (float)g_sideJumpCm) ? " HELD" : "");
                ++frames;
            } else if (frames) {
                TwkLog("[foot] sideways hold over %d frames: rejected jumps L=%d R=%d, switched to the nearer end "
                       "L=%d R=%d (riding socket known L=%d R=%d)%s", frames, rej[0], rej[1], sw[0], sw[1],
                       haveRide[0] ? 1 : 0, haveRide[1] ? 1 : 0,
                       jumps ? "   <-- the socket WAS flip-flopping" : "   (no socket jumps -- the oscillation is elsewhere)");
                have[0] = have[1] = false; rej[0] = rej[1] = 0; sw[0] = sw[1] = 0; frames = 0; jumps = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[foot] caught fatal applying the foot offsets -> paused (game unaffected)");
    }
}

// ------------------------------------------------------------------ per-frame upkeep
// Only resolves the anim instance other modules ask us for. Everything the old probe did here --
// settle detection, hover sampling, per-shoe profile upkeep -- went with the sliders.
void FootPlace_PumpFrame() {
    if (!g_installed || !g_ok) return;
    __try {
        void* skater = CatchTweaks_Skater();
        if (!skater) return;
        if (skater != g_skater) { g_skater = skater; g_anim = nullptr; }   // respawn / map switch
        void* mesh = twkP(skater, SK_MESH);
        void* anim = mesh ? twkP(mesh, MESH_ANIM) : nullptr;
        if (anim) g_anim = anim;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[foot] caught fatal resolving the skater -> paused (game unaffected)");
    }
}

// ------------------------------------------------------------------ shared with other modules
void* FootPlace_AnimInstance() { return g_anim; }

bool FootPlace_Grounded() {
    void* anim = g_anim;
    if (!anim) return false;
    __try { return twkB(anim, AN_GROUNDED) == 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FootPlace_SettingUpTrick() {
    void* anim = g_anim;
    if (!anim) return false;
    // Asset-gated: with no CrankLoopBlendSpace the crank fields are stale, and a stale "cranking"
    // would look like a permanent trick setup.
    __try { return twkP(anim, AN_CRANK_BS) && twkB(anim, AN_IS_CRANKING) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ------------------------------------------------------------------ install
void FootPlace_Install() {
    g_installed = true;
    g_startAnchors = TwkScanExe(SIG_UPDATE_ANCHORS);
    if (!g_startAnchors) {
        TwkLog("[foot] UpdateFootAnchors sig NOT FOUND -- the shoe-height fix cannot be applied, and "
               "foot steering has no seam either (game updated?)");
        return;
    }
    if (MH_CreateHook(g_startAnchors, (void*)&hkUpdateFootAnchors, &g_origAnchors) != MH_OK ||
        MH_EnableHook(g_startAnchors) != MH_OK) {
        TwkLog("[foot] hook failed on UpdateFootAnchors -- the shoe-height fix cannot be applied");
        g_startAnchors = nullptr;
        return;
    }
    TwkLog("[foot] installed @ %p (inside the animation update, where a write survives) -- shoe-height "
           "fix %s", g_startAnchors, g_on ? "ON" : "off");
}

void FootPlace_DrawMenu(const OmpMenuApi* api) {
    if (!g_installed || !g_startAnchors) {
        api->TextDisabled("Foot placement: not installed");
        return;
    }
    api->TextDisabled(g_ok ? (g_on ? "Shoe height: fixed (the game's foot auto-adjust is suppressed "
                                     "while you ride)"
                                   : "Shoe height: fix disabled in SessionTweaks.ini")
                           : "Shoe height: PAUSED by a fault this session");
    api->Indent();
    api->TextDisabled("The game sweeps each foot down onto the surface below and lifts the shoe by");
    api->TextDisabled("what it hits -- but that query reaches the road, not the deck. Nothing to tune.");
    api->Unindent();
    float lift = (float)g_soleLiftMm * 0.1f;
    if (api->SliderFloat("Sole lift while riding (cm)", &lift, -1.0f, 2.0f, "%.1f")) {
        int mm = (int)floorf(lift * 10.0f + 0.5f);
        if (mm < -20) mm = -20;
        if (mm > 30)  mm = 30;
        if (mm != g_soleLiftMm) { g_soleLiftMm = mm; TwkMarkDirty(); }
    }
    float liftSw = (float)g_soleLiftSwMm * 0.1f;
    if (api->SliderFloat("Sole lift while riding SWITCH (cm)", &liftSw, -1.0f, 2.0f, "%.1f")) {
        int mm = (int)floorf(liftSw * 10.0f + 0.5f);
        if (mm < -20) mm = -20;
        if (mm > 30)  mm = 30;
        if (mm != g_soleLiftSwMm) { g_soleLiftSwMm = mm; TwkMarkDirty(); }
    }
    api->Indent();
    api->TextDisabled("Moves both feet along the deck's normal while you ride. 0 = the socket exactly as");
    api->TextDisabled("the game places it; a few mm up hides the shoe sinking in at the kick. Switch has");
    api->TextDisabled("its own value because the game's switch placement already sits a touch high --");
    api->TextDisabled("negative is a drop.");
    api->Unindent();


}
