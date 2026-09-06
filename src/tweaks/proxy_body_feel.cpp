// proxy_body_feel.cpp -- SessionTweaks' riding body on REMOTE players (SessionOpenMP proxies).
//
// MIRROR of body_feel.cpp's riding layer -- the RIDING ARMS / TORSO AND HEAD INTENT / landing surge /
// physics-visibility sections of BodyFeel_PumpFrame, plus ApplyPaDrives and BodyFeel_PostPhysApply --
// rebuilt around a per-skater state block instead of that module's file statics, so it can run for
// every proxy in the lobby at once, each with its OWNER'S settings. body_feel.cpp is the field-tuned
// original and stays untouched; a change to how the riding arms feel there needs the same change
// here. Deliberately NOT mirrored:
//   * the bail brace -- a peer's ragdoll arrives as their transported skeleton, brace already in it;
//   * every asset-level edit (arm-torso collision pairs, CCD, joint drives, shoulder limits) -- those
//     live on the physics ASSET, shared by every skater wearing it, and the local module made them;
//   * the crouch term of the visibility target -- it reads the LOCAL pad's depth (PopProbe), which is
//     nobody else's; a proxy simply has no coil.
// A proxy only gets physics at all because SessionOpenMP switches the game's physical animation on
// for it (it never is otherwise); this module then shapes it. Nothing here runs when the player has
// peer body physics off, or when the peer sent no settings (an older SessionTweaks, or none).
#include "tweaks_common.h"
#include "proxy_body_feel.h"
#include "body_feel.h"       // the local knobs, read back through their getters for the wire
#include "grind_pop.h"       // GrindPop_FNameToString -- bone and class names
#include <windows.h>
#include <psapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---- offsets: the same PDB-named set body_feel.cpp drives through
enum {
    SK_MESH            = 0x280,   // ACharacter::Mesh
    SK_PHYSANIM_ON     = 0x711,   // ASkaterCharacterBase bitfield byte; bit 0x10 = _isPhysicalAnimationEnabled
    SK_ROOT            = 0x130,   // AActor::RootComponent
    MESH_ANIM          = 0x6b0,   // USkeletalMeshComponent::AnimScriptInstance
    CMP_BODIES         = 0x980,   // USkeletalMeshComponent::Bodies (TArray<FBodyInstance*>)
    BI_BLEND_WEIGHT    = 0x11c,   // FBodyInstance::PhysicsBlendWeight
    BI_BONE_INDEX      = 0x01c,   // FBodyInstance::InstanceBoneIndex (int16)
    CMP_SKELMESH       = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SKM_REFSKEL_INFO   = 0x1b0 + 0x20, // USkeletalMesh::RefSkeleton.FinalRefBoneInfo (TArray)
    BONEINFO_STRIDE    = 12,      // FMeshBoneInfo { FName Name; int32 ParentIndex; }
    AN_SPEED_RATIO     = 0x2f4,   // USkaterAnimInstance::SpeedRatio (0..1)
    AN_GROUNDED_BF     = 0x5fa,   // the grounded byte foot_place/pop_probe read
    AN_IS_GRINDING_BF  = 0x33c,   // IsGrinding (+0x33d IsGrindingInLiptrick right behind)
    AN_LAND_DROP       = 0x604,   // LandDropHeightRatio (0..1)
    CTW_QUAT           = 0x1c0,   // USceneComponent ComponentToWorld (FTransform: quat, pos, scale)
    CTW_TRANSLATION    = 0x1c0 + 0x10,
    CMP_CST_ARR        = 0x4b0,   // USkinnedMeshComponent::ComponentSpaceTransformsArray[2]
    CMP_CST_READIDX    = 0x4f4,   // CurrentReadComponentTransforms
    ACT_BP_COMPS       = 0x200,   // AActor::BlueprintCreatedComponents
    ACT_INST_COMPS     = 0x1f0,   // AActor::InstanceComponents
    ACT_OWNED_COMPS    = 0x1a0,   // AActor::OwnedComponents (TSet)
    UOBJ_CLASS         = 0x010,   // UObjectBase::ClassPrivate
    UOBJ_NAME          = 0x018,   // UObjectBase::NamePrivate
    USTRUCT_SUPER      = 0x040,   // UStruct::SuperStruct
    PA_STRENGTH_MULT   = 0x0b0,   // UPhysicalAnimationComponent::StrengthMultiplyer
    PA_MESH            = 0x0b8,   // ::SkeletalMeshComponent
    PA_RUNTIME         = 0x0c0,   // ::RuntimeInstanceData TArray<{FConstraintInstance*, PxRigidDynamic*}>
    PA_DRIVEDATA       = 0x0d0,   // ::DriveData TArray<FPhysicalAnimationData> (36B)
    PAD_STRIDE         = 36,
    PAD_BODYNAME       = 0x00,
    PAD_LOCAL          = 0x08,
    PAD_ORIENT         = 0x0c,
    PAD_ANGVEL         = 0x10,
    PAD_POS            = 0x14,
    PAD_VEL            = 0x18,
    PAD_MAXLIN         = 0x1c,
    PAD_MAXANG         = 0x20,
    CI_SWING_STIFF     = 0x8c + 0xc4 + 0x10,   // FConstraintInstance::ProfileInstance.AngularDrive.SwingDrive.Stiffness
    CI_LIN_STIFF       = 0x8c + 0x78 + 0x00,   // ProfileInstance.LinearDrive.XDrive.Stiffness
};
enum { kMaxBodies = 64, kMaxRigs = 16 };

// ---- the tuning constants body_feel keeps as non-knobs (same values, so the curves match)
static const float kInertiaMax  = 2500.0f;   // cm/s^2 cap on the inertial reaction
static const float kAccSmooth   = 0.25f;     // per-tick smoothing on the root acceleration
static const float kAirMore     = 0.25f;     // more physics while airborne
static const float kBrace       = 0.15f;     // less at full speed
static const float kRailSet     = 0.15f;     // less while grinding
static const float kLandGive    = 0.60f;     // the landing surge at a max-height drop
static const float kLandMinGive = 0.15f;     // even a curb hop shows a little give
static const int   kTauMs       = 130;       // ease time constant
static const int   kRecoverMs   = 340;       // how long a landing surge takes to breathe back in

// ---- the owner's knobs, in the units body_feel keeps them in
struct Cfg {
    bool  on = false;
    int   amount = 100;
    float armK = 1.7f, armLoose = 0.65f, armHold = 0.35f, armDamp = 1.2f, armInertia = 0.6f;
    float spreadAccel = 900.0f, landAccent = 600.0f;
    bool  torsoOn = true;
    float torsoLoose = 0.60f, torsoHold = 0.50f, torsoDamp = 1.20f, torsoInertia = 0.60f;
    float headLoose = 0.50f, headHold = 0.45f, headDamp = 1.30f, headInertia = 0.40f;
};
static void CfgFromWire(const int16_t* v, int n, Cfg& c) {
    c = Cfg{};
    auto has = [&](int i) { return i < n; };
    auto pct = [&](int i, float dflt) { return has(i) ? (float)v[i] / 100.0f : dflt; };
    if (has(BF_ON))        c.on = v[BF_ON] != 0;
    if (has(BF_AMOUNT))    c.amount = v[BF_AMOUNT];
    c.armK        = pct(BF_ARM_PCT, c.armK);
    c.armLoose    = pct(BF_ARM_LOOSE, c.armLoose);
    c.armHold     = pct(BF_ARM_HOLD, c.armHold);
    c.armDamp     = pct(BF_ARM_DAMP, c.armDamp);
    c.armInertia  = pct(BF_ARM_INERTIA, c.armInertia);
    if (has(BF_ARM_SPREAD))   c.spreadAccel = (float)v[BF_ARM_SPREAD];
    if (has(BF_ARM_LANDDROP)) c.landAccent  = (float)v[BF_ARM_LANDDROP];
    if (has(BF_TORSO_ON))     c.torsoOn = v[BF_TORSO_ON] != 0;
    c.torsoLoose   = pct(BF_TORSO_LOOSE, c.torsoLoose);
    c.torsoHold    = pct(BF_TORSO_HOLD, c.torsoHold);
    c.torsoDamp    = pct(BF_TORSO_DAMP, c.torsoDamp);
    c.torsoInertia = pct(BF_TORSO_LEAN, c.torsoInertia);
    c.headLoose    = pct(BF_HEAD_LOOSE, c.headLoose);
    c.headHold     = pct(BF_HEAD_HOLD, c.headHold);
    c.headDamp     = pct(BF_HEAD_DAMP, c.headDamp);
    c.headInertia  = pct(BF_HEAD_LAG, c.headInertia);
    if (c.amount < 0) c.amount = 0; else if (c.amount > 300) c.amount = 300;
    if (c.armK < 0.0f) c.armK = 0.0f; else if (c.armK > 5.0f) c.armK = 5.0f;
}

// ---- one proxy's riding body: everything body_feel keeps per skater, for this skater
struct Rig {
    void*  actor = nullptr;
    Cfg    cfg;
    bool   fault = false;
    // the mesh and the blend-weight captures
    void*  mesh = nullptr;
    int    nBodies = 0;
    float  authored[kMaxBodies];
    float  written[kMaxBodies];
    bool   owned[kMaxBodies];
    float  feel = 1.0f, pulse = 0.0f;
    bool   wasAir = false;
    double airSince = 0.0, lastT = 0.0;
    bool   apply = false;
    // body classification (the riding subset of the brace classifier)
    bool   reacher[kMaxBodies], handBody[kMaxBodies], armBody[kMaxBodies];
    bool   spineBody[kMaxBodies], clavBody[kMaxBodies], headBody[kMaxBodies];
    float  leanW[kMaxBodies];
    int    pelvisBody = -1, spineFallback = -1, nReachers = 0;
    bool   classified = false;
    double classifyAt = 0.0;
    // the root, tracked for the arms' inertial reaction
    float  pos[3] = {0,0,0}, vel[3] = {0,0,0}, velPrev[3] = {0,0,0}, rootAcc[3] = {0,0,0};
    bool   posValid = false;
    float  airIntent = 0.0f;
    // the physical-animation component and the softened drives
    void*  paObj = nullptr, *paComp = nullptr;
    double paFindAt = 0.0;
    bool   paBound = false, paSoft = false;
    uint64_t paName[kMaxBodies];
    int8_t   paKind[kMaxBodies];
    float  armFloorNow = 0.0f, torsoFloorNow = 0.0f, headFloorNow = 0.0f;
    bool   armed = false;                 // one "armed" line per mesh
};
static Rig  g_rigs[kMaxRigs];
static int  g_nRigs = 0;

static double NowS() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static void QuatRotate(const float* q, float* v) {   // v' = q * v (q = xyzw)
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.0f * (qy * v[2] - qz * v[1]);
    const float ty = 2.0f * (qz * v[0] - qx * v[2]);
    const float tz = 2.0f * (qx * v[1] - qy * v[0]);
    v[0] += qw * tx + (qy * tz - qz * ty);
    v[1] += qw * ty + (qz * tx - qx * tz);
    v[2] += qw * tz + (qx * ty - qy * tx);
}

// ---- the engine entry points body_feel uses, resolved once per process (same signatures)
typedef void (*AddForceFn)(void*, const float*, bool, bool);
typedef void (*SetDriveFn)(void*, float, float, float);
static AddForceFn g_addForce = nullptr;
static SetDriveFn g_setAngDrive = nullptr, g_setLinDrive = nullptr;
static bool g_sigsTried = false;
static const char* SIG_ADDFORCE =
    "4C 8B DC 45 88 4B 20 45 88 43 18 48 83 EC 58 49 8D 43 18 49 89 4B D8 49 89 43 E8 "
    "48 81 C1 ?? ?? 00 00 49 8D 43 20 49 89 53 E0";
static const char* SIG_SET_ANG_DRIVE =
    "48 83 EC 38 48 8D 44 24 40 F3 0F 11 89 60 01 00 00 48 89 44 24 28 48 8D 54 24 20 48 8D 05 ?? ?? ?? ??";
static const char* SIG_SET_LIN_DRIVE =
    "48 83 EC 38 48 8D 44 24 40 F3 0F 11 89 1C 01 00 00 48 89 44 24 28 48 8D 54 24 20 48 8D 05 ?? ?? ?? ??";
static void ResolveSigs() {
    if (g_sigsTried) return;
    g_sigsTried = true;
    g_addForce    = (AddForceFn)TwkScanExe(SIG_ADDFORCE);
    g_setAngDrive = (SetDriveFn)TwkScanExe(SIG_SET_ANG_DRIVE);
    g_setLinDrive = (SetDriveFn)TwkScanExe(SIG_SET_LIN_DRIVE);
    TwkLog("[pbody] AddForce %s, SetAngularDriveParams %s, SetLinearDriveParams %s",
           g_addForce ? "resolved" : "SIG NOT FOUND -- peer arm reflexes inert",
           g_setAngDrive ? "resolved" : "SIG NOT FOUND -- peer muscle tone stays stock",
           g_setLinDrive ? "resolved" : "SIG NOT FOUND");
}

// ---- the bridge to SessionOpenMP, bound by name (either mod may be installed alone)
typedef void (*SetOwnFn)(const int16_t*, int, int);
typedef int  (*ListFn)(void**, int);
typedef int  (*GetFn)(void*, int16_t*, int, int*);
typedef int  (*OnFn)();
static SetOwnFn g_setOwn = nullptr;
static ListFn   g_list   = nullptr;
static GetFn    g_get    = nullptr;
static OnFn     g_on     = nullptr;
static uint64_t g_bindTryMs = 0;
static bool BindBridge() {
    if (g_setOwn && g_list && g_get && g_on) return true;
    const uint64_t ms = GetTickCount64();
    if (ms - g_bindTryMs < 2000) return false;
    g_bindTryMs = ms;
    HMODULE mods[512]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    int n = (int)(needed / sizeof(HMODULE)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        auto a = (SetOwnFn)GetProcAddress(mods[i], "OmpSession_SetOwnBodyFeel");
        if (!a) continue;
        g_setOwn = a;
        g_list   = (ListFn)GetProcAddress(mods[i], "OmpSession_ProxyActors");
        g_get    = (GetFn) GetProcAddress(mods[i], "OmpSession_ProxyBodyFeel");
        g_on     = (OnFn)  GetProcAddress(mods[i], "OmpSession_PeerBodyPhysicsOn");
        if (g_list && g_get && g_on) {
            TwkLog("[pbody] SessionOpenMP bridge bound -- remote players get their own riding body here");
            return true;
        }
        TwkLog("[pbody] SessionOpenMP found but its body-feel exports are incomplete (older build?) -- peers keep stock physics");
        g_setOwn = nullptr; g_list = nullptr; g_get = nullptr; g_on = nullptr;
        return false;
    }
    return false;
}

// Our own knobs onto the wire: sampled every pump, pushed on change. 18 getter calls; nothing to
// sample from the ini or the menu directly, so the two can never disagree.
static void PublishOwn() {
    if (!g_setOwn) return;
    int16_t v[BF_COUNT];
    auto i16 = [](float f) { long r = lroundf(f); if (r > 32767) r = 32767; if (r < -32768) r = -32768; return (int16_t)r; };
    v[BF_ON]           = BodyFeel_Enabled() ? 1 : 0;
    v[BF_AMOUNT]       = i16(BodyFeel_AmountPct());
    v[BF_ARM_PCT]      = i16(BodyFeel_ArmPct());
    v[BF_ARM_LOOSE]    = i16(BodyFeel_ArmLoosePct());
    v[BF_ARM_HOLD]     = i16(BodyFeel_ArmHoldPct());
    v[BF_ARM_DAMP]     = i16(BodyFeel_ArmDampPct());
    v[BF_ARM_INERTIA]  = i16(BodyFeel_ArmInertiaPct());
    v[BF_ARM_SPREAD]   = i16(BodyFeel_ArmSpread());
    v[BF_ARM_LANDDROP] = i16(BodyFeel_ArmLandDrop());
    v[BF_TORSO_ON]     = BodyFeel_TorsoEnabled() ? 1 : 0;
    v[BF_TORSO_LOOSE]  = i16(BodyFeel_TorsoLoosePct());
    v[BF_TORSO_HOLD]   = i16(BodyFeel_TorsoHoldPct());
    v[BF_TORSO_DAMP]   = i16(BodyFeel_TorsoDampPct());
    v[BF_TORSO_LEAN]   = i16(BodyFeel_TorsoLeanPct());
    v[BF_HEAD_LOOSE]   = i16(BodyFeel_HeadLoosePct());
    v[BF_HEAD_HOLD]    = i16(BodyFeel_HeadHoldPct());
    v[BF_HEAD_DAMP]    = i16(BodyFeel_HeadDampPct());
    v[BF_HEAD_LAG]     = i16(BodyFeel_HeadLagPct());
    static int16_t last[BF_COUNT];
    static bool haveLast = false;
    static uint64_t lastPushMs = 0;
    if (haveLast && memcmp(last, v, sizeof(v)) == 0) return;
    // A slider drag changes a value every frame; the peers want the latest, not every step. The
    // values are re-sampled every pump, so a change inside the window goes out at its end.
    const uint64_t ms = GetTickCount64();
    if (haveLast && ms - lastPushMs < 250) return;
    memcpy(last, v, sizeof(v)); haveLast = true; lastPushMs = ms;
    g_setOwn(v, BF_COUNT, kBfWireVer);
}

// ---- the physical-animation component, found by class in the actor's component lists (the skater
// has no member for it). Bound to THIS mesh wins; else any -- the game unbinds it while rolling fast.
static bool IsPhysAnimClass(void* obj) {
    if (!obj) return false;
    uint8_t* cls = *(uint8_t**)((uint8_t*)obj + UOBJ_CLASS);
    for (int depth = 0; cls && depth < 6; depth++) {
        char nm[64];
        if (GrindPop_FNameToString(cls + UOBJ_NAME, nm, sizeof(nm)) &&
            strcmp(nm, "PhysicalAnimationComponent") == 0) return true;
        cls = *(uint8_t**)(cls + USTRUCT_SUPER);
    }
    return false;
}
static void* FindPhysAnimIn(void* sk, void* mesh, bool strict) {
    static const int lists[2] = { ACT_BP_COMPS, ACT_INST_COMPS };
    for (int l = 0; l < 2; l++) {
        void** arr = *(void***)((uint8_t*)sk + lists[l]);
        const int n = *(int*)((uint8_t*)sk + lists[l] + 8);
        if (!arr || n <= 0 || n > 512) continue;
        for (int i = 0; i < n; i++) {
            void* c = arr[i];
            if (c && IsPhysAnimClass(c) && (!strict || *(void**)((uint8_t*)c + PA_MESH) == mesh)) return c;
        }
    }
    uint8_t* set = (uint8_t*)sk + ACT_OWNED_COMPS;
    uint8_t* data = *(uint8_t**)(set + 0x00);
    const int num = *(int*)(set + 0x08);
    const int maxBits = *(int*)(set + 0x2c);
    const uint32_t* bits = (maxBits <= 128) ? (const uint32_t*)(set + 0x10) : *(const uint32_t**)(set + 0x20);
    if (data && bits && num > 0 && num <= 512) {
        for (int i = 0; i < num; i++) {
            if (!((bits[i >> 5] >> (i & 31)) & 1u)) continue;
            void* c = *(void**)(data + (size_t)i * 16);
            if (c && IsPhysAnimClass(c) && (!strict || *(void**)((uint8_t*)c + PA_MESH) == mesh)) return c;
        }
    }
    return nullptr;
}
static void* FindPhysAnim(void* sk, void* mesh) {
    void* c = FindPhysAnimIn(sk, mesh, true);
    return c ? c : FindPhysAnimIn(sk, mesh, false);
}
static void ClassifyPaEntry(Rig& r, int i, const uint8_t* e) {
    const uint64_t raw = *(const uint64_t*)(e + PAD_BODYNAME);
    if (r.paKind[i] >= 0 && r.paName[i] == raw) return;
    r.paName[i] = raw; r.paKind[i] = -1;
    char nm[64];
    if (!GrindPop_FNameToString(e + PAD_BODYNAME, nm, sizeof(nm))) return;
    for (char* c = nm; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    r.paKind[i] = (strstr(nm, "upperarm") || strstr(nm, "lowerarm") || strstr(nm, "forearm") ||
                   strstr(nm, "hand")) ? 1
                : strstr(nm, "spine") ? 2 : strstr(nm, "clavicle") ? 3
                : (strstr(nm, "head") || strstr(nm, "neck")) ? 4 : 0;
}

// The drives: the profile's numbers x StrengthMultiplyer x the owner's hold/damp fractions, pushed
// through the engine's own setters exactly as body_feel does. Authored back with both flags false.
static int ApplyPaDrives(Rig& r, bool armSoft, bool torsoSoft) {
    uint8_t* pa = (uint8_t*)r.paComp;
    if (!pa || !g_setAngDrive) return 0;
    if (*(void**)(pa + PA_MESH) != r.mesh) { r.paComp = nullptr; return 0; }
    const uint8_t* dd = *(uint8_t**)(pa + PA_DRIVEDATA);
    const int n = *(int*)(pa + PA_DRIVEDATA + 8);
    uint8_t* rt = *(uint8_t**)(pa + PA_RUNTIME);
    const int rn = *(int*)(pa + PA_RUNTIME + 8);
    if (!dd || !rt || n <= 0 || rn < n) return 0;
    const float mult = *(float*)(pa + PA_STRENGTH_MULT);
    int wrote = 0;
    for (int i = 0; i < n && i < kMaxBodies; i++) {
        const uint8_t* e = dd + (size_t)i * PAD_STRIDE;
        ClassifyPaEntry(r, i, e);
        const int k = r.paKind[i];
        if (k < 1) continue;
        float kS = 1.0f, kD = 1.0f;
        if (k == 1)      { if (armSoft)   { kS = r.cfg.armHold;   kD = r.cfg.armDamp;   } }
        else if (k <= 3) { if (torsoSoft) { kS = r.cfg.torsoHold; kD = r.cfg.torsoDamp; } }
        else             { if (torsoSoft) { kS = r.cfg.headHold;  kD = r.cfg.headDamp;  } }
        const float orient = *(const float*)(e + PAD_ORIENT), angVel = *(const float*)(e + PAD_ANGVEL);
        const float pos    = *(const float*)(e + PAD_POS),    vel    = *(const float*)(e + PAD_VEL);
        uint8_t* ci = *(uint8_t**)(rt + (size_t)i * 16);
        if (!ci) continue;
        const float wantA = orient * mult * kS, liveA = *(float*)(ci + CI_SWING_STIFF);
        if (fabsf(liveA - wantA) > 0.01f + 0.001f * fabsf(wantA)) {
            g_setAngDrive(ci, wantA, angVel * mult * kD, *(const float*)(e + PAD_MAXANG) * mult);
            wrote++;
        }
        if (g_setLinDrive && !*(e + PAD_LOCAL)) {
            const float wantL = pos * mult * kS, liveL = *(float*)(ci + CI_LIN_STIFF);
            if (fabsf(liveL - wantL) > 0.01f + 0.001f * fabsf(wantL))
                g_setLinDrive(ci, wantL, vel * mult * kD, *(const float*)(e + PAD_MAXLIN) * mult);
        }
    }
    return wrote;
}
static void RestorePaDrives(Rig& r) {
    if (!r.paSoft) return;
    r.paSoft = false;
    __try { ApplyPaDrives(r, false, false); } __except (EXCEPTION_EXECUTE_HANDLER) { r.paComp = nullptr; }
}
static void ForgetMesh(Rig& r) {
    r.paComp = nullptr; r.paObj = nullptr; r.paBound = false; r.paSoft = false; r.paFindAt = 0.0;
    r.airIntent = 0.0f; r.armFloorNow = 0.0f; r.torsoFloorNow = 0.0f; r.headFloorNow = 0.0f;
    r.mesh = nullptr; r.nBodies = 0; r.feel = 1.0f; r.pulse = 0.0f; r.apply = false;
    r.classified = false; r.classifyAt = 0.0; r.armed = false;
    r.posValid = false;
}
// Hand the body back: authored drives, authored weights, forget. Safe on a live mesh only -- a rig
// whose actor is gone is dropped WITHOUT this (the game re-authors every weight each frame anyway,
// and the drive writes are absolute, so nothing stale survives a re-arm).
static void RestoreAll(Rig& r) {
    if (!r.mesh || !r.nBodies) { ForgetMesh(r); return; }
    RestorePaDrives(r);
    __try {
        uint8_t* arr = *(uint8_t**)((uint8_t*)r.mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)r.mesh + CMP_BODIES + 8);
        if (arr && n == r.nBodies) {
            for (int i = 0; i < n && i < kMaxBodies; i++) {
                if (!r.owned[i]) continue;
                uint8_t* bi = ((uint8_t**)arr)[i];
                if (!bi) continue;
                float* w = (float*)(bi + BI_BLEND_WEIGHT);
                // undo OUR write only: a body the game has re-authored since (a disable, a state
                // change) keeps the game's value -- nothing re-authors a proxy after us the way the
                // local skater's Blueprint does every frame
                if (fabsf(*w - r.written[i]) < 0.0001f) *w = r.authored[i];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ForgetMesh(r);
}

// ---- one proxy, one tick: body_feel's riding layer with this rig's state and this owner's knobs
static void PumpOne(Rig& r, void* sk, void* an, double t) {
    float dt = (r.lastT > 0.0) ? (float)(t - r.lastT) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; else if (dt > 0.1f) dt = 0.1f;
    r.lastT = t;
    __try {
        {   // the root, tracked every tick: the arms' inertial reaction reads its acceleration
            void* root = *(void**)((uint8_t*)sk + SK_ROOT);
            if (root) {
                const float* pos = (const float*)((uint8_t*)root + CTW_TRANSLATION);
                if (r.posValid && dt > 0.001f) {
                    for (int a = 0; a < 3; a++) {
                        const float v = (pos[a] - r.pos[a]) / dt;
                        r.vel[a] += (v - r.vel[a]) * 0.35f;
                        r.rootAcc[a] += ((r.vel[a] - r.velPrev[a]) / dt - r.rootAcc[a]) * kAccSmooth;
                        r.velPrev[a] = r.vel[a];
                    }
                }
                r.pos[0] = pos[0]; r.pos[1] = pos[1]; r.pos[2] = pos[2];
                r.posValid = true;
            } else r.posValid = false;
        }
        // Physical animation is what OpenMP switches on for a riding proxy and off for a walking or
        // bailing one; nothing to shape while it is off (and a bail is the owner's skeleton anyway).
        const bool physOn = ((*((uint8_t*)sk + SK_PHYSANIM_ON) >> 4) & 1) != 0;
        if (!r.cfg.on || r.cfg.amount == 0 || !physOn) {
            if (r.mesh) RestoreAll(r);
            return;
        }
        void* mesh = *(void**)((uint8_t*)sk + SK_MESH);
        if (!mesh) { if (r.mesh) RestoreAll(r); return; }
        uint8_t* arr = *(uint8_t**)((uint8_t*)mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)mesh + CMP_BODIES + 8);
        if (!arr || n <= 0 || n > kMaxBodies) { if (r.mesh) RestoreAll(r); return; }
        if (mesh != r.mesh || n != r.nBodies) {
            // New mesh (respawn, re-dress) -- fresh captures; nothing owned yet.
            if (r.mesh) RestoreAll(r);
            r.mesh = mesh; r.nBodies = n;
            for (int i = 0; i < n; i++) { r.owned[i] = false; r.written[i] = -1.0f; }
            for (int i = 0; i < kMaxBodies; i++) r.paKind[i] = -1;
            r.classified = false; r.classifyAt = 0.0;
        }
        // ---- classify the bodies by bone name: the riding subset of the brace classifier. Bone
        // names can resolve late, so a zero-reacher result is retried, not cached.
        if (!r.classified && t >= r.classifyAt) {
            r.classifyAt = t + 1.0;
            r.nReachers = 0; r.pelvisBody = -1; r.spineFallback = -1;
            void* skm = *(void**)((uint8_t*)mesh + CMP_SKELMESH);
            uint8_t* info = skm ? *(uint8_t**)((uint8_t*)skm + SKM_REFSKEL_INFO) : nullptr;
            const int nBones = skm ? *(int*)((uint8_t*)skm + SKM_REFSKEL_INFO + 8) : 0;
            for (int i = 0; i < n; i++) {
                r.reacher[i] = false; r.handBody[i] = false; r.armBody[i] = false;
                r.spineBody[i] = false; r.clavBody[i] = false; r.headBody[i] = false; r.leanW[i] = 0.0f;
                uint8_t* bi = ((uint8_t**)arr)[i];
                if (!bi || !info) continue;
                const int boneIdx = *(short*)(bi + BI_BONE_INDEX);
                if (boneIdx < 0 || boneIdx >= nBones) continue;
                char nm[64];
                if (!GrindPop_FNameToString(info + (size_t)boneIdx * BONEINFO_STRIDE, nm, sizeof(nm))) continue;
                for (char* c = nm; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                r.handBody[i]  = strstr(nm, "hand") != nullptr;
                r.armBody[i]   = strstr(nm, "upperarm") != nullptr;
                r.spineBody[i] = strstr(nm, "spine") != nullptr;
                r.clavBody[i]  = strstr(nm, "clavicle") != nullptr;
                r.leanW[i] = strstr(nm, "spine3") ? 1.0f : strstr(nm, "spine2") ? 0.6f :
                             strstr(nm, "spine1") ? 0.3f : r.clavBody[i] ? 0.4f :
                             strstr(nm, "head") ? 0.5f : strstr(nm, "neck") ? 0.3f : 0.0f;
                if (strstr(nm, "hand") || strstr(nm, "forearm") || strstr(nm, "lowerarm")) {
                    r.reacher[i] = true; r.armBody[i] = true; r.nReachers++;
                } else if (strstr(nm, "head") || strstr(nm, "neck")) {
                    r.headBody[i] = true;
                } else if (r.pelvisBody < 0 && strstr(nm, "pelvis")) {
                    r.pelvisBody = i;
                } else if (r.spineFallback < 0 && strstr(nm, "spine")) {
                    r.spineFallback = i;
                }
            }
            if (r.pelvisBody < 0) r.pelvisBody = r.spineFallback;
            r.classified = r.nReachers > 0;
        }
        r.apply = true;                 // the post-phys writer (foot_place detour) takes it from here

        const float speed = *(float*)((uint8_t*)an + AN_SPEED_RATIO);
        const bool grounded = *((uint8_t*)an + AN_GROUNDED_BF) != 0;
        // ---- RIDING ARMS: the floor, the muscle tone, the intent
        const float armK = r.cfg.armK;
        r.armFloorNow = 0.0f;
        const bool onRail = (*((uint8_t*)an + AN_IS_GRINDING_BF) |
                             *((uint8_t*)an + AN_IS_GRINDING_BF + 1)) != 0;
        {   // the balance intent, eased: an air or a rail fades the arms out and back, no pop
            const float want = !grounded ? 1.0f : (onRail ? 0.6f : 0.0f);
            const float tauI = (want > r.airIntent) ? 0.12f : 0.30f;
            r.airIntent += (want - r.airIntent) * (dt > 0.0f ? (1.0f - expf(-dt / tauI)) : 1.0f);
        }
        // the component: found once per mesh (retried at 1 Hz), then WATCHED every tick for whether
        // the game has it bound to this mesh -- it unbinds while rolling fast and rebinds after
        if (!r.paObj && t >= r.paFindAt) {
            r.paFindAt = t + 1.0;
            ResolveSigs();
            r.paObj = FindPhysAnim(sk, mesh);
        }
        {
            void* to = r.paObj ? *(void**)((uint8_t*)r.paObj + PA_MESH) : nullptr;
            const bool bound = r.paObj && to == mesh;
            if (bound != r.paBound) {
                r.paBound = bound;
                if (bound) r.paSoft = false;      // fresh constraints: the first apply is a full stamp
            }
            r.paComp = bound ? r.paObj : nullptr;
        }
        const bool armLayer   = armK > 0.0f && r.cfg.amount > 0;
        const bool torsoLayer = r.cfg.torsoOn && r.cfg.amount > 0;
        r.armFloorNow   = armLayer ? r.cfg.armLoose * (armK < 1.0f ? armK : 1.0f) : 0.0f;
        r.torsoFloorNow = (torsoLayer && r.paBound) ? r.cfg.torsoLoose : 0.0f;
        r.headFloorNow  = (torsoLayer && r.paBound) ? r.cfg.headLoose  : 0.0f;
        if (r.paComp && (armLayer || torsoLayer)) {
            r.paSoft = true;
            ApplyPaDrives(r, armLayer, torsoLayer);
        } else if (r.paSoft) {
            RestorePaDrives(r);
        }
        if (!r.armed) {
            r.armed = true;
            TwkLog("[pbody] proxy %p: %d bodies, owner's riding body armed (amount %d%%, arms %d%%, torso %s, PA %s)",
                   sk, n, r.cfg.amount, (int)(armK * 100.0f + 0.5f), r.cfg.torsoOn ? "on" : "off",
                   r.paComp ? "bound" : (r.paObj ? "found, unbound" : "not found yet"));
        }
        const float landMag = (grounded && r.pulse > 0.05f) ? r.pulse * r.cfg.landAccent : 0.0f;
        if (armLayer) {
            ResolveSigs();
            if (g_addForce) {
                uint8_t* mc2 = (uint8_t*)mesh;
                int ri2 = *(int*)(mc2 + CMP_CST_READIDX); if (ri2 != 1) ri2 = 0;
                uint8_t* cst2 = *(uint8_t**)(mc2 + CMP_CST_ARR + (size_t)ri2 * 0x10);
                const int cstN2 = *(int*)(mc2 + CMP_CST_ARR + (size_t)ri2 * 0x10 + 8);
                const float* mq2 = (const float*)(mc2 + CTW_QUAT);
                const float* mp2 = mq2 + 4; const float* ms2 = mq2 + 8;
                float rp[8][3]; int rIdx[8]; int nR = 0;
                float pel[3] = { r.pos[0], r.pos[1], r.pos[2] };
                if (cst2 && cstN2 > 0) {
                    for (int i = 0; i < n && i < kMaxBodies; i++) {
                        if (!r.reacher[i] && i != r.pelvisBody) continue;
                        uint8_t* bi = ((uint8_t**)arr)[i]; if (!bi) continue;
                        const int bx = *(short*)(bi + BI_BONE_INDEX);
                        if (bx < 0 || bx >= cstN2) continue;
                        const float* bp = (const float*)(cst2 + (size_t)bx * 48 + 0x10);
                        float w2[3] = { bp[0]*ms2[0], bp[1]*ms2[1], bp[2]*ms2[2] };
                        QuatRotate(mq2, w2);
                        w2[0] += mp2[0]; w2[1] += mp2[1]; w2[2] += mp2[2];
                        if (i == r.pelvisBody) { pel[0] = w2[0]; pel[1] = w2[1]; pel[2] = w2[2]; }
                        if (r.reacher[i] && nR < 8) {
                            rIdx[nR] = i;
                            rp[nR][0] = w2[0]; rp[nR][1] = w2[1]; rp[nR][2] = w2[2];
                            nR++;
                        }
                    }
                }
                // INERTIA: the arms lag the root's acceleration, on top of what the softened drives
                // already let through. Half weight on the vertical.
                float inr[3] = { -r.rootAcc[0] * r.cfg.armInertia, -r.rootAcc[1] * r.cfg.armInertia,
                                 -r.rootAcc[2] * r.cfg.armInertia * 0.5f };
                {
                    const float m = sqrtf(inr[0]*inr[0] + inr[1]*inr[1] + inr[2]*inr[2]);
                    if (m > kInertiaMax) { const float sc = kInertiaMax / m; inr[0] *= sc; inr[1] *= sc; inr[2] *= sc; }
                }
                const float spreadMag = r.cfg.spreadAccel * r.airIntent;
                for (int k = 0; k < nR; k++) {
                    const int i = rIdx[k];
                    uint8_t* bi = ((uint8_t**)arr)[i]; if (!bi) continue;
                    const float bs = r.handBody[i] ? 1.0f : 0.55f;
                    float f2[3] = { inr[0], inr[1], inr[2] };
                    if (spreadMag > 1.0f) {
                        float ox = rp[k][0] - pel[0], oy = rp[k][1] - pel[1];
                        const float om = sqrtf(ox*ox + oy*oy);
                        if (om > 5.0f) {
                            ox /= om; oy /= om;
                            f2[0] += ox * spreadMag * 0.45f;
                            f2[1] += oy * spreadMag * 0.45f;
                            f2[2] += spreadMag * 0.55f;
                        }
                    }
                    f2[2] -= landMag;
                    const float scl = armK * bs;
                    f2[0] *= scl; f2[1] *= scl; f2[2] *= scl;
                    if (f2[0] != 0.0f || f2[1] != 0.0f || f2[2] != 0.0f)
                        g_addForce(bi, f2, true, true);
                }
            }
        }
        // TORSO AND HEAD INTENT: the chest leans against the root's acceleration and the head lags
        // it, weighted up the spine. Only while the game has physical animation bound.
        if (torsoLayer && r.paBound) {
            ResolveSigs();
            if (g_addForce) {
                const float bodyK = (float)r.cfg.amount / 100.0f;
                for (int i = 0; i < n && i < kMaxBodies; i++) {
                    if (r.leanW[i] <= 0.0f) continue;
                    uint8_t* bi = ((uint8_t**)arr)[i]; if (!bi) continue;
                    const float gain = (r.headBody[i] ? r.cfg.headInertia : r.cfg.torsoInertia) * r.leanW[i] * bodyK;
                    float f3[3] = { -r.rootAcc[0] * gain, -r.rootAcc[1] * gain, -r.rootAcc[2] * gain * 0.5f };
                    const float m = sqrtf(f3[0]*f3[0] + f3[1]*f3[1] + f3[2]*f3[2]);
                    if (m > kInertiaMax) { const float sc = kInertiaMax / m; f3[0] *= sc; f3[1] *= sc; f3[2] *= sc; }
                    f3[2] -= landMag * 0.5f * r.leanW[i] * bodyK;
                    if (f3[0] != 0.0f || f3[1] != 0.0f || f3[2] != 0.0f) g_addForce(bi, f3, true, true);
                }
            }
        }
        // ---- the landing surge
        if (!grounded) { if (!r.wasAir) { r.wasAir = true; r.airSince = t; } }
        else {
            if (r.wasAir && t - r.airSince > 0.15) {
                float drop = *(float*)((uint8_t*)an + AN_LAND_DROP);
                if (!(drop >= 0.0f)) drop = 0.0f; else if (drop > 1.0f) drop = 1.0f;
                const float give = kLandMinGive + (kLandGive - kLandMinGive) * drop;
                if (give > r.pulse) r.pulse = give;
            }
            r.wasAir = false;
        }
        if (r.pulse > 0.0f) {
            r.pulse -= dt * 1000.0f / (float)kRecoverMs;
            if (r.pulse < 0.0f) r.pulse = 0.0f;
        }
        // ---- physics-visibility target, then the ease (no crouch term: that is the local pad's)
        float target = 1.0f;
        if (!grounded) target += kAirMore;
        target -= kBrace * (speed < 0.0f ? 0.0f : (speed > 1.0f ? 1.0f : speed));
        if (onRail) target -= kRailSet;
        target *= (1.0f + r.pulse);
        target = 1.0f + (target - 1.0f) * ((float)r.cfg.amount / 100.0f);
        if (target < 0.1f) target = 0.1f; else if (target > 3.0f) target = 3.0f;
        const float tau = (float)kTauMs / 1000.0f;
        r.feel += (target - r.feel) * (tau > 0.0f ? (1.0f - expf(-dt / tau)) : 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.fault = true;
        TwkLog("[pbody] proxy %p faulted -- its riding body is paused until it is re-dressed", sk);
    }
}

// The write that survives: inside the animation update, after the Blueprint re-authored the per-body
// weights and before the pose blend consumes them. Same self-healing capture as body_feel's.
static void PostPhys(Rig& r) {
    if (!r.apply) return;
    r.apply = false;
    if (!r.mesh || !r.nBodies) return;
    __try {
        uint8_t* arr = *(uint8_t**)((uint8_t*)r.mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)r.mesh + CMP_BODIES + 8);
        if (!arr || n != r.nBodies) return;
        for (int i = 0; i < n && i < kMaxBodies; i++) {
            uint8_t* bi = ((uint8_t**)arr)[i];
            if (!bi) continue;
            float* w = (float*)(bi + BI_BLEND_WEIGHT);
            const float cur = *w;
            if (!(cur >= 0.0f && cur <= 1.0f)) continue;
            if (!r.owned[i] || fabsf(cur - r.written[i]) > 0.0001f) {
                r.authored[i] = cur;
                r.owned[i] = cur > 0.0f;
            }
            float out;
            if (!r.owned[i] || r.authored[i] <= 0.0f) {
                if (!(r.armBody[i] && r.armFloorNow > 0.0f)) continue;
                out = r.armFloorNow;
            } else {
                out = r.authored[i] * r.feel;
                if (r.armBody[i] && out < r.armFloorNow) out = r.armFloorNow;
                else if ((r.spineBody[i] || r.clavBody[i]) && out < r.torsoFloorNow) out = r.torsoFloorNow;
                else if (r.headBody[i] && out < r.headFloorNow) out = r.headFloorNow;
            }
            if (out < 0.0f) out = 0.0f; else if (out > 1.0f) out = 1.0f;
            *w = out; r.written[i] = out;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.fault = true;
        TwkLog("[pbody] post-phys apply faulted on a proxy -- its riding body is paused");
    }
}

// ---- the rigs: one per live proxy, keyed by actor
static Rig* RigFor(void* actor) {
    for (int i = 0; i < g_nRigs; i++) if (g_rigs[i].actor == actor) return &g_rigs[i];
    return nullptr;
}
static Rig* RigAlloc(void* actor) {
    if (g_nRigs >= kMaxRigs) return nullptr;
    Rig& r = g_rigs[g_nRigs++];
    r = Rig{};
    r.actor = actor;
    for (int i = 0; i < kMaxBodies; i++) r.paKind[i] = -1;
    return &r;
}
static void RigDrop(int idx, bool restore) {
    if (restore) RestoreAll(g_rigs[idx]);
    g_rigs[idx] = g_rigs[--g_nRigs];          // the last rig fills the hole; the caller re-visits idx
}

void ProxyBodyFeel_PumpFrame() {
    if (!BindBridge()) return;
    PublishOwn();
    void* actors[kMaxRigs];
    const bool on = g_on() != 0;
    int nA = on ? g_list(actors, kMaxRigs) : 0;
    if (nA < 0) nA = 0;
    // rigs whose proxy is gone: dropped without a restore (the mesh may be gone with the world);
    // switched off by the player: restored, the actor is live
    for (int i = 0; i < g_nRigs; ) {
        bool listed = false;
        for (int k = 0; k < nA; k++) if (actors[k] == g_rigs[i].actor) { listed = true; break; }
        if (!listed) { RigDrop(i, !on && g_rigs[i].mesh != nullptr); continue; }
        i++;
    }
    if (!on) return;
    const double t = NowS();
    for (int k = 0; k < nA; k++) {
        void* sk = actors[k];
        if (!sk) continue;
        int16_t v[32]; int ver = 0;
        const int n = g_get(sk, v, 32, &ver);
        Rig* r = RigFor(sk);
        if (n <= 0 || ver <= 0) {              // no settings from this peer: stock physics only
            if (r) { RestoreAll(*r); r->cfg = Cfg{}; }
            continue;
        }
        if (!r) { r = RigAlloc(sk); if (!r) continue; }
        Cfg c; CfgFromWire(v, n, c);
        if (memcmp(&c, &r->cfg, sizeof(Cfg)) != 0) {
            const bool first = !r->armed;
            r->cfg = c;
            r->paSoft = false;                 // the next apply re-stamps every drive with the new hold/damp
            if (!first) {                      // a live edit on their side: one line a second, not one a step
                static double said = 0.0;
                const double now = NowS();
                if (now - said > 1.0) { said = now;
                    TwkLog("[pbody] proxy %p: owner changed their settings (amount %d%%, arms %d%%, torso %s)",
                           sk, c.amount, (int)(c.armK * 100.0f + 0.5f), c.torsoOn ? "on" : "off"); }
            }
        }
        void* mesh = twkP(sk, SK_MESH);
        void* an = mesh ? twkP(mesh, MESH_ANIM) : nullptr;
        if (!mesh || !an) { if (r->mesh) RestoreAll(*r); continue; }
        if (r->fault && r->mesh && r->mesh != mesh) { r->fault = false; ForgetMesh(*r); }   // re-dressed: try again
        if (r->fault) continue;
        PumpOne(*r, sk, an, t);
    }
}

void ProxyBodyFeel_PostPhysApply(void* skater) {
    if (!skater || !g_nRigs) return;
    Rig* r = RigFor(skater);
    if (r && !r->fault) PostPhys(*r);
}
