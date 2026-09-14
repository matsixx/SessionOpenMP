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
//
// FIRST PERSON WHILE SKATING. A camera at the eyes that is pleasant to skate with, which is a harder
// thing than a camera at the eyes: the head whips round in a flip, drops half a metre in a crank,
// jolts on every landing and flails in a bail. So each part of the view comes from the thing that
// already does that part well.
//
//  * YAW comes from the GAME'S OWN CAMERA, not the head: it has already solved smoothed turns and
//    carves, and it is game-owned every frame, so taking it can never feed back. The horizon is level.
//  * EXCEPT A SPIN. The chase camera does not turn with a 180 (field, 376), so whatever the BODY turns
//    faster than the camera is added on top, measured as the pelvis's world yaw DELTA (no bone axis is
//    ever assumed). Once the spin stops the view finishes the turn to a whole revolution in the direction
//    it was spinning -- a 180 carries on round to face the way you roll, it does not swing back -- and a
//    small body turn (into a slide) settles back instead. Riding only: walking turns are the game's.
//  * A LITTLE OF THE HEAD leaks in, measured as the head's rotation relative to the capsule AGAINST ITS
//    OWN SLOW AVERAGE -- a pure delta, so no idea which head axis is "forward" is needed (the question
//    that cost three rounds on the head look).
//  * POSITION is the head, relative to the capsule: a slow average of its offset (the stance) plus only a
//    SHARE of the quick motion around it, then smoothed. The whole bob at the eyes was "jarring" (376).
//    ON A LEASH: never more than CamFpLeashCm from where the head really is. Unleashed, a pop or a lean
//    left the eye 20 cm behind a body that had moved, which is inside the neck and chest (378 field).
//  * KEPT OUT OF THE BODY: the eye is pushed out of capsules round the chest, upper back and shoulders,
//    with a near-plane margin. Sized to leave the riding pose alone; arms are left out on purpose, a
//    flailing arm must not shove the view.
//  * PITCH, while riding, FRAMES THE BOARD. From the eyes the deck is ~80 degrees down and a 100 degree
//    horizontal FOV only reaches ~34 below centre (375). The pitch is solved so a point on the board lands
//    at a chosen height in the frame: the nose end near the bottom edge while rolling, the deck centre
//    lower down through the crouch before a pop, and near the middle of the frame in the air, on a rail
//    and in a manual -- where a skater watches the board (378). Off the board the game camera's pitch.
//  * The final yaw and pitch are smoothed together, after every source above.
//  * The HEAD IS HIDDEN with USkeletalMeshComponent::HideBone -- render-only, field-confirmed by the pose
//    probe (375), so replays keep the real head. ACCESSORIES near the head (glasses, 376) are separate
//    components HideBone never reaches: every mesh component attached under the skater whose origin sits
//    within CamFpAccessoryCm of the head is hidden with SetHiddenInGame, and given back on the way out.
//  * A BAIL drops to third person, quickly, as do the replay and prop editors.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "cam_fp.h"
#include "tweaks_common.h"
#include "catch_tweaks.h"   // CatchTweaks_Skater
#include "catch_sound.h"    // CatchSound_ObjName -- component and class names for the accessory log
#include "grind_pop.h"      // GrindPop_FNameToString -- bone names
#include "sit.h"            // Sit_EditorOpen -- the replay and prop editors own the camera
#include "foot_place.h"     // FootPlace_AnimInstance / Grounded / SettingUpTrick -- riding, in a trick
#include "ui/menu_ext.h"

enum {
    OBJ_CLASS     = 0x10,    // UObjectBase::ClassPrivate
    ACT_ROOT      = 0x130,   // AActor::RootComponent
    SC_CHILDREN   = 0xd0,    // USceneComponent::AttachChildren (TArray<USceneComponent*>)
    SC_FLAGS14D   = 0x14d,   // bit 0x04 = bHiddenInGame
    CH_MESH       = 0x280,   // ACharacter::Mesh
    SC_C2W        = 0x1c0,   // USceneComponent::ComponentToWorld: quat +0, translation +0x10, scale +0x20
    SKM_MESH      = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SKM_CST       = 0x4b0,   // ComponentSpaceTransformsArray[2], 0x10 apart
    SKM_READ      = 0x4f4,   // CurrentReadComponentTransforms
    SM_REFSKEL    = 0x1b0,   // USkeletalMesh::RefSkeleton
    RS_FINAL_INFO = 0x20,    // FReferenceSkeleton::FinalRefBoneInfo (FName, parent int; 12 B)
    SK_FLAGS710   = 0x710,   // bit 0x02 = _isRagDoll -- the co-op host's bail signal, proven there
    SK_BOARD      = 0x568,   // ASkaterCharacterBase::_skateboard
    BOARD_FLIPPER = 0x4e8,   // ASkateboardEx::_flipper -- the deck mesh (foot_place reads its frame the same way)
    AN_ON_BOARD   = 0x300,   // USkaterAnimInstance::IsOnBoard
    AN_GRINDING   = 0x33c,   // USkaterAnimInstance::IsGrinding, +0x33d IsGrindingInLiptrick (body_feel reads the same)
    SK_MANUAL_BITS = 0x918,  // ASkaterCharacterBase bitfield: 0x02 _isInManual, 0x04 _isInNoseManual (pop_probe)
    MAX_RIG       = 512,
    MAX_ACC       = 16,
};
static const float RAD2DEG = 57.2957795f, DEG2RAD = 0.0174532925f;

// void USkeletalMeshComponent::HideBone(int32 BoneIndex, EPhysBodyOp) -- Epic 0x2b5bd00 / Steam 0x2b1e540
// void USkeletalMeshComponent::UnHideBone(int32 BoneIndex)          -- Epic 0x2b69a10 / Steam 0x2b2c250
// void USceneComponent::SetHiddenInGame(bool, EVisibilityPropagation) -- Epic 0x2b643e0 / Steam 0x2b26c20
//   (the enum form: r8b == 2 is Propagate; the bool UFUNCTION wrapper passes DirtyOnly = 1 by default)
// All sigmake-unique in both builds. Real bodies, not the tiny accessors ICF folds together.
static const char* SIG_HIDE_BONE =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 FA 41 8B F0 8B D7 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB 80 04 00 00 00";
static const char* SIG_UNHIDE_BONE =
    "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB 80 04 00 00 00 ?? ?? 48 8D 8B 88 04 00 00";
static const char* SIG_SET_HIDDEN_IN_GAME =
    "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 B9 48 81 EC 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
    "48 89 45 37 41 80 F8 02 48 8B F9 0F B6 89 4D 01 00 00 41 0F B6 F0 0F 94 C3 0F B6 C1 C0 E8 02 44 0F B6 F2 24 "
    "01 3A D0 ?? ?? 8D 46 FF 3C 01 0F B6 C2 0F 96 C3 80 E1 FB C0 E0 02 0A C8";
typedef void (*HideBoneFn)(void* comp, int boneIndex, int physBodyOp);   // PBO_None = 0
typedef void (*UnHideBoneFn)(void* comp, int boneIndex);
typedef void (*SetHiddenFn)(void* comp, bool hidden, uint8_t propagation);
static HideBoneFn   g_hide      = nullptr;
static UnHideBoneFn g_unhide    = nullptr;
static SetHiddenFn  g_setHidden = nullptr;

// ------------------------------------------------------------------ settings
static int   g_on        = 0;       // CameraFirstPerson -- the Camera page toggle
static float g_eyeUp     = 8.0f;    // CamFpEyeUpCm  -- above the head bone
static float g_eyeFwd    = 10.0f;   // CamFpEyeFwdCm -- along the view: far enough out that the face is behind the near plane
static float g_pitchOff  = 0.0f;    // CamFpPitchDeg -- added on top of the pitch below; + looks up
static float g_fov       = 115.0f;  // CamFpFovDeg   -- horizontal; 0 keeps the game's (100). Wide enough for board + road
static int   g_frame     = 1;       // CamFpFrameBoard -- riding pitch frames the board (0 = the game camera's pitch)
static int   g_ridePct   = 82;      // CamFpRideFramePct  -- rolling: the board's nose this far from centre to the bottom edge
static int   g_setupPct  = 52;      // CamFpSetupFramePct -- the crouch before a pop: the deck centre this far down
static int   g_trickPct  = 20;      // CamFpTrickFramePct -- air, grind, manual: the deck centre this far down (lower = look further down)
static float g_leadCm    = 45.0f;   // CamFpBoardLeadCm   -- rolling: frame this far ahead of the deck centre (the nose)
static int   g_lookMs    = 140;     // CamFpLookMs        -- how smoothly the final view direction follows
static int   g_trickInMs = 150, g_trickOutMs = 450;   // CamFpTrickInMs / CamFpTrickOutMs -- the glance down and back up
static int   g_smoothMs  = 70;      // CamFpSmoothMs -- how much of the head's jolting the position soaks up
static int   g_posPct    = 40;      // CamFpHeadFollowPct -- share of the head's QUICK motion the eye position takes
static int   g_headPct   = 20;      // CamFpHeadPct  -- how much of the head's own turning reaches the view
static float g_headMax   = 12.0f;    // CamFpHeadMaxDeg -- and never more than this
static int   g_spin      = 1;       // CamFpTurnWithBody -- the view turns with a spin the game camera does not
static int   g_spinLo    = 150, g_spinHi = 280;   // CamFpSpinLoDps / CamFpSpinHiDps -- body-over-camera rate that counts as a spin
static int   g_spinMs    = 300;     // CamFpSpinSettleMs -- finishing the turn after the spin
static int   g_spinLog   = 1;       // CamFpSpinLog -- 10 Hz yaw lines during spins (capped)
static int   g_blendMs   = 350;     // CamFpBlendMs  -- the dolly in and out
static int   g_hideHead  = 1;       // CamFpHideHead
static int   g_accCm     = 30;      // CamFpAccessoryCm -- mesh components this close to the head are hidden (0 = none)
static int   g_leashCm   = 6;       // CamFpLeashCm -- the smoothed eye never strays further than this from the real head
static int   g_clear     = 1;       // CamFpBodyClear -- keep the eye out of the chest and shoulders
static int   g_chestR    = 12, g_shoulderR = 7, g_clearCm = 6;   // CamFpChestCm / CamFpShoulderCm / CamFpClearCm (radius + margin)
static int   g_rev       = 378;     // CamFpRev -- the settings revision written; older files get the retuned framing

// ------------------------------------------------------------------ state
static bool     g_ok = true;
static void*    g_sk = nullptr;               // the skater everything below was gathered under
static void*    g_skel = nullptr; static int g_head = -1, g_pelvis = -1, g_nBones = 0;
static int      g_neck = -1, g_chest = -1, g_spine = -1, g_clav[2] = { -1, -1 }, g_uarm[2] = { -1, -1 };
static int      g_clearLines = 0; static uint64_t g_clearLogMs = 0; static bool g_clearRestLogged = false;
static float    g_blend = 0.0f;
static bool     g_filtInit = false, g_avgInit = false;
static float    g_rel[3] = {0,0,0}, g_relV[3] = {0,0,0};   // the eye position's offset from the capsule, smoothed
static float    g_relAvg[3] = {0,0,0};                     // the head's usual offset (the stance)
static float    g_avgQ[4] = {0,0,0,1};                     // the head's usual rotation relative to the capsule
static bool     g_lookInit = false;
static float    g_yawS = 0.0f, g_yawV = 0.0f, g_pitchS = 0.0f, g_pitchV = 0.0f;   // the final view, smoothed
static float    g_trickW = 0.0f, g_setupW = 0.0f, g_rideW = 0.0f;   // air/grind/manual, crouch, on-the-board weights, eased
static bool     g_pelInit = false; static float g_pelPrev[4] = {0,0,0,1};
static bool     g_camInit = false; static float g_camPrev = 0.0f;
static float    g_spinRate = 0.0f, g_spinO = 0.0f; static int g_spinSign = 1;
static float    g_spinArm = 0.0f;   // seconds the view has been fully in: spins count only past 0.5
static int      g_spinLines = 0; static uint64_t g_spinLogMs = 0;
static void*    g_hidMesh = nullptr; static int g_hidBone = -1; static uint64_t g_hideMs = 0;
static int      g_probeFrames = 0; static bool g_probed = false;
static void*    g_acc[MAX_ACC]; static int g_nAcc = 0; static uint64_t g_accMs = 0;
static void*    g_accLoggedFor = nullptr;
static const char* g_offWhy = "";
static char     g_status[200] = "off";

// ------------------------------------------------------------------ math (UE FQuat order x y z w)
static void qmul(const float a[4], const float b[4], float r[4]) {
    r[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    r[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    r[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    r[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qconj(const float q[4], float r[4]) { r[0] = -q[0]; r[1] = -q[1]; r[2] = -q[2]; r[3] = q[3]; }
static void qrot(const float q[4], const float v[3], float r[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    r[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    r[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    r[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
static void qnorm(float q[4]) {
    const float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n < 1e-8f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
static float wrap180(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}
// yaw about +Z, then pitch (+ looks up). The same composition the seated view uses, field-confirmed.
static void qFromYawPitch(float yawDeg, float pitchDeg, float q[4]) {
    const float hy = yawDeg * 3.14159265f / 360.0f, hp = -pitchDeg * 3.14159265f / 360.0f;
    const float qy[4] = { 0.0f, 0.0f, sinf(hy), cosf(hy) };
    const float qp[4] = { 0.0f, sinf(hp), 0.0f, cosf(hp) };
    qmul(qy, qp, q);
}
static float smoothDamp(float cur, float target, float& vel, float smoothTime, float dt) {
    if (smoothTime < 1e-4f) { vel = 0.0f; return target; }
    const float omega = 2.0f / smoothTime, x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = cur - target, temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * e;
    return target + (change + temp) * e;
}
static bool readC2W(void* comp, float q[4], float p[3], float s[3]) {
    if (!comp) return false;
    const float* t = (const float*)((const uint8_t*)comp + SC_C2W);
    for (int i = 0; i < 4; i++) q[i] = t[i];
    for (int i = 0; i < 3; i++) p[i] = t[4 + i];
    if (s) for (int i = 0; i < 3; i++) s[i] = (fabsf(t[8 + i]) > 1e-6f) ? t[8 + i] : 1.0f;
    return true;
}

// ------------------------------------------------------------------ the rig: head and pelvis
// Found by NAME off the mesh's own reference skeleton, once per skeleton: an exact "head", or else the
// neck's child with "head" in its name (this rig's is amxx_head). The pelvis is only ever read for its
// world yaw DELTA, so which of its axes points where does not matter.
static bool resolveHead(void* mesh) {
    void* skel = twkP(mesh, SKM_MESH);
    if (!skel) return false;
    if (skel == g_skel) return g_head >= 0;
    g_skel = skel; g_head = -1; g_pelvis = -1; g_nBones = 0;
    g_neck = g_chest = g_spine = -1; g_clav[0] = g_clav[1] = g_uarm[0] = g_uarm[1] = -1; g_clearRestLogged = false;
    const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
    const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
    const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
    if (!info || n <= 0 || n > MAX_RIG) return false;
    static int parent[MAX_RIG];
    int neck = -1, exact = -1, loose = -1;
    char headName[96] = "";
    char list[2400]; int ll = 0; list[0] = 0;   // every bone name, once per skeleton
    static bool isClav[MAX_RIG], isUarm[MAX_RIG];
    for (int i = 0; i < n; i++) {
        char nb[96];
        if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) nb[0] = 0;
        for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        parent[i] = *(const int*)(info + i * 12 + 8);
        if (parent[i] >= i) parent[i] = -1;
        if (neck < 0 && strstr(nb, "neck")) neck = i;
        if (g_pelvis < 0 && strstr(nb, "pelvis")) g_pelvis = i;
        isClav[i] = strstr(nb, "clavicle") != nullptr;
        isUarm[i] = strstr(nb, "upperarm") != nullptr;
        if (exact < 0 && strcmp(nb, "head") == 0) { exact = i; strncpy_s(headName, nb, _TRUNCATE); }
        if (loose < 0 && strstr(nb, "head") && neck >= 0 && parent[i] == neck) {
            loose = i; if (exact < 0) strncpy_s(headName, nb, _TRUNCATE);
        }
        const char* shortName = strncmp(nb, "amxx_", 5) == 0 ? nb + 5 : nb;
        if (ll < (int)sizeof(list) - 40) ll += snprintf(list + ll, sizeof(list) - ll, "%s%d:%s<%d", i ? " " : "", i, shortName, parent[i]);
    }
    g_head = exact >= 0 ? exact : loose;
    g_nBones = n;
    // the torso chain the eye is kept out of: neck -> its parent (chest) -> its parent; clavicles off the chest
    g_neck = neck;
    g_chest = neck >= 0 ? parent[neck] : -1;
    g_spine = g_chest >= 0 ? parent[g_chest] : -1;
    {
        int nc = 0;
        for (int i = 0; i < n && nc < 2; i++) {
            if (!isClav[i] || parent[i] != g_chest) continue;
            int ua = -1;
            for (int j = i + 1; j < n; j++) if (isUarm[j] && parent[j] == i) { ua = j; break; }
            if (ua >= 0) { g_clav[nc] = i; g_uarm[nc] = ua; nc++; }
        }
    }
    if (g_head >= 0) TwkLog("[fp] rig: %d bones, the head is %s (%d), pelvis %d | kept out of: neck %d chest %d spine %d, "
                            "shoulders %d>%d %d>%d", n, headName, g_head, g_pelvis, g_neck, g_chest, g_spine,
                            g_clav[0], g_uarm[0], g_clav[1], g_uarm[1]);
    else             TwkLog("[fp] rig: %d bones and no head bone we recognise -- first person unavailable on it", n);
    TwkLog("[fp] bones (index:name<parent): %s", list);
    return g_head >= 0;
}

// A bone's component-space transform off the READ buffer: the pose on screen now.
static const float* boneCst(void* mesh, int bone) {
    const int ridx = *(const int*)((const uint8_t*)mesh + SKM_READ);
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + (ridx == 1 ? 0x10 : 0);
    const uint8_t* data = *(const uint8_t* const*)arr;
    const int cnt = *(const int*)(arr + 8);
    if (!data || cnt != g_nBones || bone < 0 || bone >= cnt) return nullptr;
    return (const float*)(data + bone * 48);
}

static bool boneWorld(void* mesh, int bone, const float mq[4], const float mp[3], const float ms[3], float out[3]) {
    const float* t = boneCst(mesh, bone);
    if (!t) return false;
    const float v[3] = { t[4] * ms[0], t[5] * ms[1], t[6] * ms[2] };
    qrot(mq, v, out);
    for (int i = 0; i < 3; i++) out[i] += mp[i];
    return true;
}
// Distance from p to segment ab, and the nearest point on it.
static float segDist(const float p[3], const float a[3], const float b[3], float nearest[3]) {
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-4f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    for (int i = 0; i < 3; i++) nearest[i] = a[i] + ab[i] * t;
    const float d[3] = { p[0] - nearest[0], p[1] - nearest[1], p[2] - nearest[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
// Out of a capsule, radially. Returns how far it moved.
static float pushOutOf(float p[3], const float a[3], const float b[3], float minDist) {
    float q[3];
    const float dist = segDist(p, a, b, q);
    if (dist >= minDist) return 0.0f;
    float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
    if (dist < 1e-3f) { d[0] = 0.0f; d[1] = 0.0f; d[2] = 1.0f; }
    else for (int i = 0; i < 3; i++) d[i] /= dist;
    for (int i = 0; i < 3; i++) p[i] = q[i] + d[i] * minDist;
    return minDist - dist;
}

// ------------------------------------------------------------------ the head, hidden from inside
static void updateHide(void* mesh, bool haveRig) {
    const bool want = g_hideHead && haveRig && mesh && g_blend >= 0.5f && g_hide && g_unhide;
    if (want) {
        const uint64_t ms = GetTickCount64();
        // Re-asserted once a second: a re-dress rebuilds the mesh's visibility states, and the head's
        // index can change with the skeleton, which resolveHead has already picked up.
        if (g_hidMesh != mesh || g_hidBone != g_head || ms - g_hideMs > 1000) {
            __try {
                g_hide(mesh, g_head, 0);
                if (!g_probed && g_hidMesh != mesh) g_probeFrames = 3;
                g_hidMesh = mesh; g_hidBone = g_head; g_hideMs = ms;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_hide = nullptr; g_hidMesh = nullptr;
                TwkLog("[fp] fault hiding the head -- it stays visible from inside for this run");
            }
        }
    } else if (g_hidMesh) {
        // Only ever on the live mesh it was hidden on: a new level resets g_hidMesh without a call,
        // because the old component is gone and its address may already be someone else's.
        if (g_hidMesh == mesh && g_unhide && g_hidBone >= 0 && g_hidBone < g_nBones) {
            __try { g_unhide(mesh, g_hidBone); } __except (EXCEPTION_EXECUTE_HANDLER) { g_unhide = nullptr; }
        }
        g_hidMesh = nullptr; g_hidBone = -1;
    }
}

// ------------------------------------------------------------------ accessories near the head
// Everything attached under the skater, depth-first. Only pointers that are live children right now,
// so "still in this list" is also the proof a component we hid has not been destroyed since.
static int collectAttached(void* comp, void** out, int n, int cap, int depth) {
    if (!comp || depth > 5) return n;
    void** data = *(void***)((uint8_t*)comp + SC_CHILDREN);
    const int cnt = *(const int*)((uint8_t*)comp + SC_CHILDREN + 8);
    if (!data || cnt <= 0 || cnt > 64) return n;
    for (int i = 0; i < cnt && n < cap; i++) {
        void* c = data[i];
        if (!c) continue;
        out[n++] = c;
        n = collectAttached(c, out, n, cap, depth + 1);
    }
    return n;
}
static bool isHiddenInGame(void* c) { return (twkB(c, SC_FLAGS14D) & 0x04) != 0; }

static void updateAccessories(void* sk, void* mesh, const float headW[3], bool want) {
    if (!g_setHidden) return;
    void* all[128];
    int n = 0;
    __try { n = collectAttached(twkP(sk, ACT_ROOT), all, 0, 128, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    auto alive = [&](void* c) { for (int i = 0; i < n; i++) if (all[i] == c) return true; return false; };
    if (!want) {
        for (int i = 0; i < g_nAcc; i++) {
            void* c = g_acc[i];
            __try { if (alive(c) && isHiddenInGame(c)) g_setHidden(c, false, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_nAcc = 0;
        return;
    }
    const uint64_t ms = GetTickCount64();
    if (g_nAcc > 0 && ms - g_accMs < 1000) return;   // re-checked once a second: a re-dress makes new components
    g_accMs = ms;
    // forget what is gone
    int k = 0;
    for (int i = 0; i < g_nAcc; i++) if (alive(g_acc[i])) g_acc[k++] = g_acc[i];
    g_nAcc = k;
    const bool logAll = g_accLoggedFor != mesh;
    g_accLoggedFor = mesh;
    const float r2 = (float)g_accCm * (float)g_accCm;
    for (int i = 0; i < n; i++) {
        void* c = all[i];
        if (c == mesh) continue;
        __try {
            char cls[80] = "", nm[96] = "";
            CatchSound_ObjName(twkP(c, OBJ_CLASS), cls, sizeof(cls));
            const bool isMesh = strstr(cls, "MeshComponent") != nullptr;
            const float* t = (const float*)((const uint8_t*)c + SC_C2W);
            const float dx = t[4] - headW[0], dy = t[5] - headW[1], dz = t[6] - headW[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            const bool nearHead = isMesh && d2 < r2;
            bool mine = false;
            for (int j = 0; j < g_nAcc; j++) if (g_acc[j] == c) mine = true;
            if (logAll) {
                CatchSound_ObjName(c, nm, sizeof(nm));
                TwkLog("[fp] attached: %s '%s' %.0f cm from the head%s%s", cls, nm, sqrtf(d2),
                       isHiddenInGame(c) ? " (already hidden)" : "", nearHead ? " -> hidden while in first person" : "");
            }
            if (!nearHead) continue;
            if (mine) { if (!isHiddenInGame(c)) g_setHidden(c, true, 1); continue; }   // the game showed it again
            if (isHiddenInGame(c) || g_nAcc >= MAX_ACC) continue;   // the game's own hidden state is not ours to undo
            g_setHidden(c, true, 1);
            g_acc[g_nAcc++] = c;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ------------------------------------------------------------------ the view
bool CamFp_View(const float actorQ[4], float dt, float eye[3], float look[4], float* weight, float* fov) {
    if (!(dt > 0.0f) || dt > 0.25f) dt = 1.0f / 60.0f;
    void* sk = CatchTweaks_Skater();
    if (sk != g_sk) {   // a new level: every pointer held below belongs to a world that has gone
        g_sk = sk; g_skel = nullptr; g_head = -1; g_pelvis = -1; g_nBones = 0;
        g_hidMesh = nullptr; g_hidBone = -1; g_blend = 0.0f; g_filtInit = false; g_avgInit = false;
        g_lookInit = false; g_trickW = 0.0f; g_setupW = 0.0f; g_rideW = 0.0f; g_pelInit = false; g_camInit = false;
        g_spinO = 0.0f; g_spinRate = 0.0f; g_nAcc = 0; g_accLoggedFor = nullptr;
    }
    if (!g_ok || !sk || !actorQ || !eye || !look || !weight || !fov) return false;

    bool ragdoll = false, haveRig = false;
    void* mesh = nullptr;
    __try {
        const int f = twkB(sk, SK_FLAGS710);
        ragdoll = f > 0 && (f & 0x02) != 0;
        mesh = twkP(sk, CH_MESH);
        // resolved only while the view is wanted or still on screen: silent for anyone with it off
        if (g_on || g_blend > 0.0f || g_hidMesh || g_nAcc) haveRig = mesh && resolveHead(mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) { haveRig = false; }
    const bool editor = Sit_EditorOpen();
    const bool active = g_on && haveRig && !ragdoll && !editor;

    {   // the dolly: in over the blend time, out the same -- except a bail, which leaves quickly
        static bool was = false;
        const float tIn  = (float)(g_blendMs > 30 ? g_blendMs : 30) * 0.001f;
        const float tOut = ragdoll ? (tIn < 0.15f ? tIn : 0.15f) : tIn;
        if (active) { g_blend += dt / tIn;  if (g_blend > 1.0f) g_blend = 1.0f; }
        else        { g_blend -= dt / tOut; if (g_blend < 0.0f) g_blend = 0.0f; }
        if (active != was) {
            was = active;
            g_offWhy = active ? "" : (!g_on ? "switched off" : ragdoll ? "a bail" : editor ? "an editor" : "no head bone");
            if (g_on || !active) TwkLog("[fp] first person %s%s", active ? "in" : "out: ", g_offWhy);
        }
    }
    __try { updateHide(mesh, haveRig); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_blend <= 0.0f || !haveRig) {
        if (g_nAcc) updateAccessories(sk, mesh, nullptr, false);
        g_filtInit = false; g_avgInit = false; g_lookInit = false; g_pelInit = false; g_camInit = false;
        g_spinO = 0.0f; g_spinRate = 0.0f;
        snprintf(g_status, sizeof(g_status), "%s", g_on ? (g_offWhy[0] ? g_offWhy : "waiting") : "off");
        return false;
    }

    __try {
        float mq[4], mp[3], ms[3], rq[4], rp[3];
        if (!readC2W(mesh, mq, mp, ms)) return false;
        if (!readC2W(twkP(sk, ACT_ROOT), rq, rp, nullptr)) return false;
        const float* t = boneCst(mesh, g_head);
        if (!t) return false;
        const float hq[4] = { t[0], t[1], t[2], t[3] };
        const float hpS[3] = { t[4] * ms[0], t[5] * ms[1], t[6] * ms[2] };

        // The pose check for the hide, a few frames after the first one: does the POSE still carry the
        // head at full scale? If it does, HideBone is render-only and a replay records the real head.
        if (g_probeFrames > 0 && --g_probeFrames == 0) {
            g_probed = true;
            TwkLog("[fp] head hidden via HideBone -- the pose reads its scale (%.2f %.2f %.2f): %s", t[8], t[9], t[10],
                   (fabsf(t[8]) > 0.5f) ? "render-only, the replay sees the real head"
                                        : "ZEROED IN THE POSE -- a replay would record a hidden head");
        }

        // ---- position: the head's offset from the capsule -- its slow average plus a share of the quick part
        float headW[3]; qrot(mq, hpS, headW);
        for (int i = 0; i < 3; i++) headW[i] += mp[i];
        updateAccessories(sk, mesh, headW, g_accCm > 0 && g_blend >= 0.5f);
        const float d[3] = { headW[0] - rp[0], headW[1] - rp[1], headW[2] - rp[2] };
        float rqc[4]; qconj(rq, rqc);
        float rel[3]; qrot(rqc, d, rel);
        const float st = (float)g_smoothMs * 0.001f;
        const float jump = sqrtf((rel[0]-g_rel[0])*(rel[0]-g_rel[0]) + (rel[1]-g_rel[1])*(rel[1]-g_rel[1]) + (rel[2]-g_rel[2])*(rel[2]-g_rel[2]));
        if (!g_filtInit || jump > 60.0f) {   // a first frame, or a teleport: no spring through that
            for (int i = 0; i < 3; i++) { g_rel[i] = rel[i]; g_relV[i] = 0.0f; g_relAvg[i] = rel[i]; }
            g_filtInit = true;
        } else {
            const float kA = 1.0f - expf(-dt / 0.6f);
            const float share = (float)g_posPct * 0.01f;
            for (int i = 0; i < 3; i++) {
                g_relAvg[i] += (rel[i] - g_relAvg[i]) * kA;
                const float want = g_relAvg[i] + (rel[i] - g_relAvg[i]) * share;
                g_rel[i] = smoothDamp(g_rel[i], want, g_relV[i], st, dt);
            }
            const float lx = g_rel[0] - rel[0], ly = g_rel[1] - rel[1], lz = g_rel[2] - rel[2];
            const float ld = sqrtf(lx * lx + ly * ly + lz * lz), leash = (float)g_leashCm;
            if (ld > leash && ld > 1e-4f) {   // on the leash: back to its end, and the spring loses the outward speed
                const float k = leash / ld;
                g_rel[0] = rel[0] + lx * k; g_rel[1] = rel[1] + ly * k; g_rel[2] = rel[2] + lz * k;
                const float vd = (g_relV[0] * lx + g_relV[1] * ly + g_relV[2] * lz) / ld;
                if (vd > 0.0f) for (int i = 0; i < 3; i++) g_relV[i] -= vd * (i == 0 ? lx : i == 1 ? ly : lz) / ld;
            }
        }
        float headF[3]; qrot(rq, g_rel, headF);
        for (int i = 0; i < 3; i++) headF[i] += rp[i];

        // ---- riding state
        const float X[3] = { 1.0f, 0.0f, 0.0f };
        float fB[3]; qrot(actorQ, X, fB);
        const float camYaw = atan2f(fB[1], fB[0]) * RAD2DEG;
        const float gamePitch = atan2f(fB[2], sqrtf(fB[0] * fB[0] + fB[1] * fB[1])) * RAD2DEG;
        bool riding = false, trick = false, setup = false, haveDeck = false;
        float deck[3] = { 0.0f, 0.0f, 0.0f };
        {
            void* a = FootPlace_AnimInstance();
            riding = a && twkB(a, AN_ON_BOARD) > 0;
            const bool grind  = riding && (twkB(a, AN_GRINDING) | twkB(a, AN_GRINDING + 1)) != 0;
            const bool manual = riding && (twkB(sk, SK_MANUAL_BITS) & 0x06) != 0;
            trick = riding && (!FootPlace_Grounded() || grind || manual);
            setup = riding && !trick && FootPlace_SettingUpTrick();
            if (g_frame) {
                void* bd  = twkP(sk, SK_BOARD);
                void* flp = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
                float dq[4];
                haveDeck = flp && readC2W(flp, dq, deck, nullptr) &&
                           fabsf(deck[0] - headF[0]) < 400.0f && fabsf(deck[1] - headF[1]) < 400.0f &&
                           fabsf(deck[2] - headF[2]) < 400.0f;   // a board left behind (bail, carry) is not framed
            }
        }
        {
            const float kR = 1.0f - expf(-dt / 0.25f);
            g_rideW += (((riding && haveDeck) ? 1.0f : 0.0f) - g_rideW) * kR;
            const float tT = (float)(trick ? g_trickInMs : g_trickOutMs) * 0.001f;
            const float kT = tT > 1e-3f ? 1.0f - expf(-dt / tT) : 1.0f;
            g_trickW += ((trick ? 1.0f : 0.0f) - g_trickW) * kT;
            const float tU = (float)(setup ? g_trickInMs : g_trickOutMs) * 0.001f;
            const float kU = tU > 1e-3f ? 1.0f - expf(-dt / tU) : 1.0f;
            g_setupW += ((setup ? 1.0f : 0.0f) - g_setupW) * kU;
        }

        // ---- yaw: the game camera's, plus the part of a body spin the camera does not turn
        const float camD = g_camInit ? wrap180(camYaw - g_camPrev) : 0.0f;
        g_camPrev = camYaw; g_camInit = true;
        float pelD = 0.0f; bool havePel = false;
        if (g_pelvis >= 0) {
            const float* pc = boneCst(mesh, g_pelvis);
            if (pc) {
                const float pq[4] = { pc[0], pc[1], pc[2], pc[3] };
                float pW[4]; qmul(mq, pq, pW); qnorm(pW);
                if (g_pelInit) {
                    float pinv[4]; qconj(g_pelPrev, pinv);
                    float dq[4]; qmul(pW, pinv, dq);                 // world-frame delta this frame
                    pelD = wrap180(2.0f * atan2f(dq[2], dq[3]) * RAD2DEG);   // its twist about world up
                    havePel = true;
                }
                for (int i = 0; i < 4; i++) g_pelPrev[i] = pW[i];
                g_pelInit = true;
            }
        }
        // Not while the view is still arriving: getting up after a bail turned the body at 1100 dps and swung
        // the view 110 degrees and back (377 log). Nor across a cut -- a respawn or camera jump in one frame.
        if (g_blend >= 0.999f) g_spinArm += dt; else g_spinArm = 0.0f;
        const bool cut = fabsf(camD) > 20.0f || fabsf(pelD) > 40.0f;
        const float relD = (havePel && !cut && g_spinArm > 0.5f) ? wrap180(pelD - camD) : 0.0f;
        {
            const float kS = 1.0f - expf(-dt / 0.06f);
            g_spinRate += (relD / dt - g_spinRate) * kS;
        }
        float g = 0.0f;
        if (g_spin && riding && g_spinHi > g_spinLo) {
            g = (fabsf(g_spinRate) - (float)g_spinLo) / (float)(g_spinHi - g_spinLo);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            g = g * g * (3.0f - 2.0f * g);
        }
        g_spinO += relD * g;
        if (g > 0.3f && relD != 0.0f) g_spinSign = relD > 0.0f ? 1 : -1;
        {   // settle to a whole number of turns: past ~135 degrees carries on round, less comes back
            const float m = g_spinO / 360.0f;
            const float tgt = 360.0f * floorf(m + 0.5f + 0.125f * (float)g_spinSign);
            const float tS = (float)(g_spinMs > 30 ? g_spinMs : 30) * 0.001f;
            const float kO = (1.0f - expf(-dt / tS)) * (1.0f - g);
            g_spinO += (tgt - g_spinO) * kO;
            if (tgt != 0.0f && fabsf(g_spinO - tgt) < 0.5f) g_spinO -= tgt;   // a whole turn done: nothing owed
            if (g_spinO > 720.0f) g_spinO -= 360.0f; else if (g_spinO < -720.0f) g_spinO += 360.0f;
        }
        const float yawBase = camYaw + g_spinO;
        if (!g_lookInit) {
            g_yawS = yawBase; g_yawV = 0.0f; g_pitchS = gamePitch; g_pitchV = 0.0f;
            g_lookInit = true;
        }
        const float fx0 = cosf(g_yawS * DEG2RAD), fy0 = sinf(g_yawS * DEG2RAD);   // this frame's facing, off the view

        // ---- pitch: framed on the board while riding, the game camera's otherwise
        float pitchWant = gamePitch;
        if (g_rideW > 0.001f && haveDeck) {
            const float tw = g_trickW, uw = g_setupW;
            const float centre = tw > uw ? tw : uw;
            const float lead = g_leadCm * (1.0f - centre);   // ahead of the deck centre while rolling, the centre otherwise
            const float tp[3] = { deck[0] + fx0 * lead, deck[1] + fy0 * lead, deck[2] };
            const float ex = headF[0] + fx0 * g_eyeFwd, ey = headF[1] + fy0 * g_eyeFwd, ez = headF[2] + g_eyeUp;
            const float dx = tp[0] - ex, dy = tp[1] - ey, dz = tp[2] - ez;
            const float along = dx * fx0 + dy * fy0;          // in the view's vertical plane
            const float angB = atan2f(dz, along > 1.0f ? along : 1.0f) * RAD2DEG;
            // vertical half-FOV from the horizontal one (16:9), then the pitch that puts the point at that height
            const float fovH = (g_fov > 1.0f ? g_fov : 100.0f) * 0.5f * DEG2RAD;
            const float tanV = tanf(fovH) * 0.5625f;
            float pctR = (float)g_ridePct + ((float)g_setupPct - (float)g_ridePct) * uw;
            pctR += ((float)g_trickPct - pctR) * tw;
            float framed = angB + atanf(pctR * 0.01f * tanV) * RAD2DEG;
            if (framed > 10.0f) framed = 10.0f; else if (framed < -85.0f) framed = -85.0f;
            pitchWant = gamePitch + (framed - gamePitch) * g_rideW;
        }

        // ---- a capped share of the head's own turning, as yaw and pitch deltas
        float leakYaw = 0.0f, leakPitch = 0.0f;
        float hW[4]; qmul(mq, hq, hW);
        float hR[4]; qmul(rqc, hW, hR); qnorm(hR);
        if (!g_avgInit) { for (int i = 0; i < 4; i++) g_avgQ[i] = hR[i]; g_avgInit = true; }
        else {
            float dotq = 0.0f; for (int i = 0; i < 4; i++) dotq += g_avgQ[i] * hR[i];
            const float sg = dotq < 0.0f ? -1.0f : 1.0f;
            const float k = 1.0f - expf(-dt / 1.5f);
            for (int i = 0; i < 4; i++) g_avgQ[i] += (hR[i] * sg - g_avgQ[i]) * k;
            qnorm(g_avgQ);
        }
        if (g_headPct > 0) {
            float ac[4]; qconj(g_avgQ, ac);
            float dS[4]; qmul(hR, ac, dS); qnorm(dS);             // the motion, in the capsule's frame
            if (dS[3] < 0.0f) for (int i = 0; i < 4; i++) dS[i] = -dS[i];
            const float ang = 2.0f * acosf(dS[3] > 1.0f ? 1.0f : dS[3]) * RAD2DEG;
            float want = ang * (float)g_headPct * 0.01f;
            if (want > g_headMax) want = g_headMax;
            const float s = sqrtf(1.0f - dS[3] * dS[3]);
            if (ang > 0.01f && s > 1e-5f) {
                const float h = want * DEG2RAD * 0.5f;
                const float dq[4] = { dS[0] / s * sinf(h), dS[1] / s * sinf(h), dS[2] / s * sinf(h), cosf(h) };
                float tmp[4], dW[4];
                qmul(rq, dq, tmp); qmul(tmp, rqc, dW);            // into the world through the capsule
                float b[4]; qFromYawPitch(yawBase, pitchWant, b);
                float f0[3]; qrot(b, X, f0);
                float f1[3]; qrot(dW, f0, f1);
                leakYaw   = wrap180(atan2f(f1[1], f1[0]) * RAD2DEG - yawBase);
                leakPitch = atan2f(f1[2], sqrtf(f1[0] * f1[0] + f1[1] * f1[1])) * RAD2DEG - pitchWant;
            }
        }

        // ---- the final view, smoothed as one
        const float lookSt = (float)g_lookMs * 0.001f;
        const float yawT = yawBase + leakYaw;
        g_yawS = smoothDamp(g_yawS, g_yawS + wrap180(yawT - g_yawS), g_yawV, lookSt, dt);
        if (g_yawS > 3600.0f || g_yawS < -3600.0f) g_yawS = wrap180(g_yawS);
        float pitchT = pitchWant + leakPitch + g_pitchOff;
        if (pitchT > 85.0f) pitchT = 85.0f; else if (pitchT < -85.0f) pitchT = -85.0f;
        g_pitchS = smoothDamp(g_pitchS, pitchT, g_pitchV, lookSt, dt);
        if (g_pitchS > 85.0f) g_pitchS = 85.0f; else if (g_pitchS < -85.0f) g_pitchS = -85.0f;
        qFromYawPitch(g_yawS, g_pitchS, look);

        // ---- the eye: in front of the head along the view (flattened), and up
        const float fx = cosf(g_yawS * DEG2RAD), fy = sinf(g_yawS * DEG2RAD);
        eye[0] = headF[0] + fx * g_eyeFwd;
        eye[1] = headF[1] + fy * g_eyeFwd;
        eye[2] = headF[2] + g_eyeUp;

        // ---- kept out of the chest, upper back and shoulders
        if (g_clear && g_chest >= 0 && g_neck >= 0) {
            float pN[3], pC[3], pS[3], c0[3], u0[3], c1[3], u1[3];
            const bool hN = boneWorld(mesh, g_neck, mq, mp, ms, pN), hC = boneWorld(mesh, g_chest, mq, mp, ms, pC);
            const bool hS = g_spine >= 0 && boneWorld(mesh, g_spine, mq, mp, ms, pS);
            const bool h0 = g_clav[0] >= 0 && boneWorld(mesh, g_clav[0], mq, mp, ms, c0) && boneWorld(mesh, g_uarm[0], mq, mp, ms, u0);
            const bool h1 = g_clav[1] >= 0 && boneWorld(mesh, g_clav[1], mq, mp, ms, c1) && boneWorld(mesh, g_uarm[1], mq, mp, ms, u1);
            const float chestMin = (float)(g_chestR + g_clearCm), shMin = (float)(g_shoulderR + g_clearCm);
            if (!g_clearRestLogged && hN && hC) {   // where the eye sits against them at the first frame in: the radii's check
                g_clearRestLogged = true;
                float q[3];
                TwkLog("[fp] clearance at the start: chest %.0f cm (min %.0f), upper back %.0f, shoulders %.0f / %.0f (min %.0f)",
                       segDist(eye, pC, pN, q), chestMin, hS ? segDist(eye, pS, pC, q) : -1.0f,
                       h0 ? segDist(eye, c0, u0, q) : -1.0f, h1 ? segDist(eye, c1, u1, q) : -1.0f, shMin);
            }
            float moved = 0.0f;
            for (int pass = 0; pass < 2; pass++) {   // two passes: one capsule's push can land in its neighbour
                if (hN && hC) moved += pushOutOf(eye, pC, pN, chestMin);
                if (hS && hC) moved += pushOutOf(eye, pS, pC, chestMin);
                if (h0) moved += pushOutOf(eye, c0, u0, shMin);
                if (h1) moved += pushOutOf(eye, c1, u1, shMin);
            }
            if (moved > 1.0f && g_clearLines < 80) {
                const uint64_t nowMs = GetTickCount64();
                if (nowMs - g_clearLogMs >= 500) {
                    g_clearLogMs = nowMs; g_clearLines++;
                    TwkLog("[fp] clear: eye pushed %.0f cm out of the body (pitch %.0f%s)", moved, g_pitchS,
                           trick ? ", air/rail/manual" : setup ? ", setting up" : "");
                }
            }
        }

        if (g_spinLog && g_spinLines < 400 && (g > 0.05f || fabsf(g_spinO) > 5.0f)) {
            const uint64_t nowMs = GetTickCount64();
            if (nowMs - g_spinLogMs >= 100) {
                g_spinLogMs = nowMs; g_spinLines++;
                TwkLog("[fp] spin: cam %.0f (%.0f dps) body-over-cam %.0f dps g %.2f | owed %.0f | view %.0f%s",
                       camYaw, camD / dt, g_spinRate, g, g_spinO, wrap180(g_yawS), trick ? " | air/trick" : "");
            }
        }

        *weight = g_blend * g_blend * (3.0f - 2.0f * g_blend);
        *fov = g_fov;
        snprintf(g_status, sizeof(g_status), "on | view %.0f/%.0f deg | board %s%s | spin %.0f | head %s, %d accessories hidden",
                 wrap180(g_yawS), g_pitchS, (g_rideW > 0.5f) ? "framed" : "not framed",
                 (g_trickW > 0.5f) ? ", air/rail/manual" : (g_setupW > 0.5f) ? ", setting up" : "",
                 g_spinO, g_hidMesh ? "hidden" : "shown", g_nAcc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = false;
        TwkLog("[fp] fault building the first-person view -- off for this run");
        return false;
    }
}

// ------------------------------------------------------------------ shell surface
void CamFp_Install() {
    g_hide      = (HideBoneFn)TwkScanExe(SIG_HIDE_BONE);
    g_unhide    = (UnHideBoneFn)TwkScanExe(SIG_UNHIDE_BONE);
    g_setHidden = (SetHiddenFn)TwkScanExe(SIG_SET_HIDDEN_IN_GAME);
    TwkLog("[fp] first person: HideBone %s, UnHideBone %s, SetHiddenInGame %s%s", g_hide ? "ok" : "MISSING",
           g_unhide ? "ok" : "MISSING", g_setHidden ? "ok" : "MISSING",
           (g_hide && g_unhide) ? "" : " -- the head will be visible from inside");
}
static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
void CamFp_ReadConfig(const char* buf) {
    g_on       = TwkIniInt(buf, "CameraFirstPerson", 0) ? 1 : 0;
    g_eyeUp    = (float)TwkIniInt(buf, "CamFpEyeUpCm", 8);
    g_eyeFwd   = (float)TwkIniInt(buf, "CamFpEyeFwdCm", 10);
    g_pitchOff = (float)clampI(TwkIniInt(buf, "CamFpPitchDeg", 0), -60, 60);
    g_fov      = (float)clampI(TwkIniInt(buf, "CamFpFovDeg", 115), 0, 150);   // renamed from CamFpFov: 375 saved a 0
    g_frame    = TwkIniInt(buf, "CamFpFrameBoard", 1) ? 1 : 0;
    g_ridePct  = clampI(TwkIniInt(buf, "CamFpRideFramePct", 82), -100, 100);
    const int rev = TwkIniInt(buf, "CamFpRev", 0);
    g_setupPct = clampI(TwkIniInt(buf, "CamFpSetupFramePct", 52), -100, 100);
    g_trickPct = clampI(TwkIniInt(buf, "CamFpTrickFramePct", 20), -100, 100);
    g_leadCm   = (float)clampI(TwkIniInt(buf, "CamFpBoardLeadCm", 45), -100, 150);
    g_lookMs   = clampI(TwkIniInt(buf, "CamFpLookMs", 140), 0, 1000);
    g_trickInMs  = clampI(TwkIniInt(buf, "CamFpTrickInMs", 150), 0, 2000);
    g_trickOutMs = clampI(TwkIniInt(buf, "CamFpTrickOutMs", 450), 0, 3000);
    g_smoothMs = clampI(TwkIniInt(buf, "CamFpSmoothMs", 70), 0, 500);
    g_posPct   = clampI(TwkIniInt(buf, "CamFpHeadFollowPct", 40), 0, 100);
    g_headPct  = clampI(TwkIniInt(buf, "CamFpHeadPct", 20), 0, 100);
    g_headMax  = (float)clampI(TwkIniInt(buf, "CamFpHeadMaxDeg", 12), 0, 45);
    g_spin     = TwkIniInt(buf, "CamFpTurnWithBody", 1) ? 1 : 0;
    g_spinLo   = clampI(TwkIniInt(buf, "CamFpSpinLoDps", 150), 0, 2000);
    g_spinHi   = clampI(TwkIniInt(buf, "CamFpSpinHiDps", 280), 1, 3000);
    g_spinMs   = clampI(TwkIniInt(buf, "CamFpSpinSettleMs", 300), 0, 3000);
    g_spinLog  = TwkIniInt(buf, "CamFpSpinLog", 1) ? 1 : 0;
    g_blendMs  = TwkIniInt(buf, "CamFpBlendMs", 350);
    g_hideHead = TwkIniInt(buf, "CamFpHideHead", 1) ? 1 : 0;
    g_accCm    = clampI(TwkIniInt(buf, "CamFpAccessoryCm", 30), 0, 100);
    g_leashCm  = clampI(TwkIniInt(buf, "CamFpLeashCm", 6), 0, 60);
    g_clear    = TwkIniInt(buf, "CamFpBodyClear", 1) ? 1 : 0;
    g_chestR   = clampI(TwkIniInt(buf, "CamFpChestCm", 12), 0, 40);
    g_shoulderR = clampI(TwkIniInt(buf, "CamFpShoulderCm", 7), 0, 30);
    g_clearCm  = clampI(TwkIniInt(buf, "CamFpClearCm", 6), 0, 30);
    if (rev < 378) {
        // Files from 376/377 carry framing and spin values that were only ever defaults, and the running game
        // wrote them back over a hand edit once. Retuned in 378 at the user's request; later edits stick.
        g_ridePct = 82; g_trickPct = 20; g_spinLo = 150; g_spinHi = 280;
    }
    g_rev = 378;
}
void CamFp_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "CameraFirstPerson",  g_on);
    TwkIniSetInt(buf, cap, "CamFpEyeUpCm",       (int)g_eyeUp);
    TwkIniSetInt(buf, cap, "CamFpEyeFwdCm",      (int)g_eyeFwd);
    TwkIniSetInt(buf, cap, "CamFpPitchDeg",      (int)g_pitchOff);
    TwkIniSetInt(buf, cap, "CamFpFovDeg",        (int)g_fov);
    TwkIniSetInt(buf, cap, "CamFpFrameBoard",    g_frame);
    TwkIniSetInt(buf, cap, "CamFpRideFramePct",  g_ridePct);
    TwkIniSetInt(buf, cap, "CamFpSetupFramePct", g_setupPct);
    TwkIniSetInt(buf, cap, "CamFpTrickFramePct", g_trickPct);
    TwkIniSetInt(buf, cap, "CamFpBoardLeadCm",   (int)g_leadCm);
    TwkIniSetInt(buf, cap, "CamFpLookMs",        g_lookMs);
    TwkIniSetInt(buf, cap, "CamFpTrickInMs",     g_trickInMs);
    TwkIniSetInt(buf, cap, "CamFpTrickOutMs",    g_trickOutMs);
    TwkIniSetInt(buf, cap, "CamFpSmoothMs",      g_smoothMs);
    TwkIniSetInt(buf, cap, "CamFpHeadFollowPct", g_posPct);
    TwkIniSetInt(buf, cap, "CamFpHeadPct",       g_headPct);
    TwkIniSetInt(buf, cap, "CamFpHeadMaxDeg",    (int)g_headMax);
    TwkIniSetInt(buf, cap, "CamFpTurnWithBody",  g_spin);
    TwkIniSetInt(buf, cap, "CamFpSpinLoDps",     g_spinLo);
    TwkIniSetInt(buf, cap, "CamFpSpinHiDps",     g_spinHi);
    TwkIniSetInt(buf, cap, "CamFpSpinSettleMs",  g_spinMs);
    TwkIniSetInt(buf, cap, "CamFpSpinLog",       g_spinLog);
    TwkIniSetInt(buf, cap, "CamFpBlendMs",       g_blendMs);
    TwkIniSetInt(buf, cap, "CamFpHideHead",      g_hideHead);
    TwkIniSetInt(buf, cap, "CamFpAccessoryCm",   g_accCm);
    TwkIniSetInt(buf, cap, "CamFpLeashCm",       g_leashCm);
    TwkIniSetInt(buf, cap, "CamFpBodyClear",     g_clear);
    TwkIniSetInt(buf, cap, "CamFpChestCm",       g_chestR);
    TwkIniSetInt(buf, cap, "CamFpShoulderCm",    g_shoulderR);
    TwkIniSetInt(buf, cap, "CamFpClearCm",       g_clearCm);
    TwkIniSetInt(buf, cap, "CamFpRev",           g_rev);
}
void CamFp_ResetDefaults() {
    g_on = 0; g_eyeUp = 8.0f; g_eyeFwd = 10.0f; g_pitchOff = 0.0f; g_fov = 115.0f;
    g_frame = 1; g_ridePct = 82; g_setupPct = 52; g_trickPct = 20; g_leadCm = 45.0f; g_lookMs = 140; g_trickInMs = 150; g_trickOutMs = 450;
    g_smoothMs = 70; g_posPct = 40; g_headPct = 20; g_headMax = 12.0f;
    g_spin = 1; g_spinLo = 150; g_spinHi = 280; g_spinMs = 300; g_spinLog = 1;
    g_blendMs = 350; g_hideHead = 1; g_accCm = 30;
    g_leashCm = 6; g_clear = 1; g_chestR = 12; g_shoulderR = 7; g_clearCm = 6;
    TwkMarkDirty();
}
bool CamFp_Enabled()          { return g_on != 0; }
void CamFp_SetEnabled(bool o) { g_on = o ? 1 : 0; TwkMarkDirty(); }
void CamFp_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    bool on = g_on != 0;
    if (api->Checkbox("First person", &on)) CamFp_SetEnabled(on);
    api->SameLine(); api->TextDisabled("(through your skater's eyes; third person again on a bail)");
    char b[240]; snprintf(b, sizeof(b), "first person: %s", g_status);
    api->TextDisabled(b);
}
