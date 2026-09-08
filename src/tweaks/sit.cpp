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
// SessionTweaks -- SITTING while off the board.
//
// Off the board the skater is the same ASkaterCharacterBase in the game's "on foot" mode, so the
// skater accessor, the anim instance's IsOnBoard flag and the UPlayerInput::InputKey hook all still
// apply. A press of the sit key looks for something to sit on and, finding it, plays a procedural
// sit: the pose is not an animation asset (the game ships none) but a set of joint TARGETS solved
// against the live rig every frame, which is what lets the feet answer the ledge's height.
//
// WHERE THE POSE IS WRITTEN. The host detours USkeletalMeshComponent::FinalizeBoneTransform, whose
// first act is USkinnedMeshComponent::FlipEditableSpaceBases -- a real function, unique in both
// builds, and unhooked. A pre-hook on it sees the finished component-space pose in the editable
// buffer, after the host's own writes and just before it is published to the renderer. That is the
// last honest write point this DLL can own.
//
// RIG-AGNOSTIC POSING. Which local axis a bone bends about is not known and is not assumed. Every
// bone is re-aimed by the rotation that carries its MEASURED child direction onto the wanted one,
// so a thigh points at the knee target and a calf at the foot target whatever the rig's axes are.
// Legs and arms are analytic two-bone IK with the bone lengths measured from the live pose; the
// pelvis is placed, the spine leans by small rotations about the seat's right axis. Targets live
// in a frame built from the mesh transform (forward = the seat's facing, up = world up), so the
// mesh's own orientation convention never enters the arithmetic either.
//
// THE CHARACTER DOES NOT MOVE. The seat point is expressed in mesh space, so the body glides onto
// the ledge while the capsule stays where it stood with movement set to None. No teleport, no
// collision surprise, no camera pop; standing up is the blend played backwards.
//
// FINDING THE SEAT. Two readings of "walk up to an edge": standing BELOW a ledge facing it (find
// the face with forward traces at several heights, then its top, its depth and the headroom), or
// standing ON it facing the drop (walk the floor forward with down-traces until it vanishes). Feet:
// if the ground under the knee is within shin reach the foot plants there, otherwise the leg
// dangles. Nothing to sit on: the ground, one knee up, the other leg out, an arm propped behind.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "sit.h"
#include "catch_tweaks.h"   // CatchTweaks_Skater
#include "foot_place.h"     // FootPlace_AnimInstance
#include "sit_ui.h"         // the on-screen prompt, built from the game's own widgets
#include "grind_pop.h"      // GrindPop_FNameToString
#include "scoop_speed.h"    // ScoopSpeed_StickRaw: the right stick, for the look
#include "camera_height.h"  // CameraHeight_ViewForward: where the camera looks, for the head that follows it
#include <psapi.h>          // the SessionOpenMP bridge is found by export name
#include "MinHook.h"

// ------------------------------------------------------------------ knobs
static int   g_on        = 1;       // SitEnabled
static int   g_floor     = 1;       // SitFloor: sit on the ground when nothing else is there
static float g_maxLedge  = 120.0f;  // SitMaxLedgeCm: tallest ledge worth climbing onto
static float g_minLedge  = 22.0f;   // SitMinLedgeCm: below this it is a bump, not a seat
static float g_reach     = 110.0f;  // SitReachCm: how far ahead a ledge face is looked for
static float g_lean      = 0.0f;    // SitLeanDeg: extra torso lean, + forward
static int   g_channel   = 3;       // SitTraceChannel: ECC_Visibility
static int   g_blendMs   = 450;     // SitBlendMs: sit-down / stand-up time
static int   g_holdMs    = 320;     // SitHoldMs: how long the key must be held to stand up
static int   g_styleMs   = 260;     // SitStyleBlendMs: how long one sitting style takes to become the next
static int   g_debug     = 0;       // SitDebug: one chain dump per sit
static int   g_boardRest = 1;      // SitBoardRest: set the board down beside you and let it settle
static float g_boardOut  = 46.0f;  // SitBoardOutCm: how far out from the hip it lands
static float g_boardDrop = 9.0f;   // SitBoardDropCm: how far above the surface it is let go
static float g_boardAhead = 62.0f; // SitBoardAheadCm: how far in front of you it is set down
static float g_pelvisSeat  = 9.0f;  // SitPelvisSeatCm: pelvis bone above the seat surface
static float g_pelvisFloor = 11.0f; // SitPelvisFloorCm: pelvis bone above the ground
static float g_insetMul  = 0.9f;    // SitInsetMul: pelvis this many thigh lengths in from the edge
static char  g_keyName[64] = "Gamepad_FaceButton_Right";   // SitKey
// A hand's palm normal is measured as fingers x across-the-palm, and "across" runs the opposite way on
// the two hands -- so the normal comes out of the palm on one and into it on the other, which is a 180
// degree roll on one wrist. Which side needs the flip depends on the order the rig lists the hand's
// children, so it is a setting rather than an assumption: SitPalmFlipLeft / SitPalmFlipRight.
static int   g_palmFlip[2] = { 1, 0 };
static float g_shoulderDrop = 0.12f;   // SitShoulderDropPct: how far the shoulders settle while seated
static float g_shoulderFollow = 0.32f; // SitShoulderFollowPct: how much of a reach the collarbone carries
static int   g_realMove  = 1;       // SitRealMove: sit down and stand up as a movement with weight, not a crossfade
static float g_settle    = 0.72f;   // SitSettlePct: damping of that movement; under 100 arrives with a give and settles
static float g_stagger   = 1.0f;    // SitStaggerPct: how far the parts of the body trail one another (0 = all at once)
static float g_footRadius = 7.0f;   // SitFootRadiusCm: half a foot; the ground under a foot is the highest thing under any of it
static float g_toeRoom   = 16.0f;   // SitToeRoomCm: how far short of an obstacle the ankle stops (the toes reach past it)
static int   g_fpOn      = 1;       // SitFirstPerson: the view key puts the camera at the eyes while seated
static char  g_fpKeyName[64] = "Gamepad_FaceButton_Bottom";   // SitFirstPersonKey: A on Xbox, cross on PlayStation
static float g_lookYawMax  = 80.0f; // SitLookYawDeg: how far either way a seated head turns...
static float g_lookUpMax   = 45.0f; // SitLookUpDeg / SitLookDownDeg: ...and up, and down
static float g_lookDownMax = 60.0f;
static float g_lookSpeed   = 150.0f;// SitLookSpeedDps: a full stick turns the head this fast
static int   g_lookInvert  = 0;     // SitLookInvert: bit 0 flips the stick's X, bit 1 its Y (a convention is unknowable without a pad in hand)
static float g_fpFov       = 0.0f;  // SitFirstPersonFov: the field of view at the eyes; 0 = leave the game's
static float g_eyeUp       = 7.0f;  // SitEyeUpCm / SitEyeFwdCm: where the eyes are, off the head bone
static float g_eyeFwd      = 9.0f;
static int   g_fpViewMs    = 350;   // SitViewBlendMs: the dolly in to the eyes, and back out
static int   g_hideHead    = 1;     // SitHideHead: the head is not drawn while the camera is inside it
static int   g_headLook    = 1;     // HeadFollowsCamera: off the board, the head looks where the camera looks (menu)
static float g_holdBand    = 35.0f; // HeadLookHoldDeg: past the limit the head holds at it this much further round, then returns to the front
static float g_camLookRate = 8.0f;  // HeadLookRate: how fast the head follows the camera (1/s) -- a head, not a turret
static float g_watchRange  = 3000.0f; // SitWatchRangeCm: a player further off than this is not followed
static int   g_bailOnHit   = 1;     // SitBailOnHit: another skater running into you knocks you over
static float g_hitRadius   = 75.0f; // SitHitRadiusCm: how close their capsule has to get
static float g_hitRise     = 130.0f;// SitHitRiseCm: ...and how far above or below you it may be
static float g_hitSpeed    = 140.0f;// SitHitSpeedCm: how fast they must be going for it to count as a hit

// ------------------------------------------------------------------ layouts (PDB, both builds)
enum {
    AN_ON_BOARD   = 0x300,   // USkaterAnimInstance::IsOnBoard
    ACT_ROOT      = 0x130,   // AActor::RootComponent
    CH_MESH       = 0x280,   // ACharacter::Mesh
    CH_MOVE       = 0x288,   // ACharacter::CharacterMovement
    CH_CAPSULE    = 0x290,   // ACharacter::CapsuleComponent
    BD_MOVECOMP   = 0x298,   // ASkateboardEx::_skateboardMovement, whose PlaceInHand does the carry
    BM_MODE       = 0x188,   // ...its movement mode byte, read only: 9 is "in hand". WRITING it is a
                             // trap -- see the note on EnableRagDoll below.
    BD_IFACE      = 0x280,   // the board's interface sub-object: what EnableRagDoll is called on
    SC_C2W        = 0x1c0,   // USceneComponent::ComponentToWorld (FTransform: quat, +0x10 pos, +0x20 scale)
    CAP_HALF      = 0x468,   // UCapsuleComponent::CapsuleHalfHeight
    MOVE_VEL      = 0xc4,    // UMovementComponent::Velocity
    MOVE_MODE     = 0x168,   // UCharacterMovementComponent::MovementMode, +1 CustomMovementMode
    SKM_MESH      = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SKM_CST       = 0x4b0,   // USkinnedMeshComponent::ComponentSpaceTransformsArray[2], 0x10 apart
    SKM_EDIT      = 0x4f0,   // CurrentEditableComponentTransforms
    SKM_READ      = 0x4f4,   // CurrentReadComponentTransforms
    SM_REFSKEL    = 0x1b0,   // USkeletalMesh::RefSkeleton
    RS_FINAL_INFO = 0x20,    // FReferenceSkeleton::FinalRefBoneInfo (FMeshBoneInfo: FName, int parent; 12 B)
    AN_FOOT2BOARD = 0x308,   // USkaterAnimInstance::FootToBoardTransitionType / BoardToFootTransitionType (+1)
    UOBJ_INDEX    = 0x0c,    // UObjectBase::InternalIndex = GetUniqueID()
    RM_MODE       = 0x2a8,   // AReplayManager::_currentReplayMode (EReplayMode); 2 = playback
    SK_BOARD      = 0x568,   // ASkaterCharacterBase::_skateboard
    SK_CAN_BAIL   = 0x649,   // ASkaterCharacterBase::_canBail -- the game's own bail veto
    SC_ATTACH_PARENT = 0xc0, // USceneComponent::AttachParent
    SC_ATTACH_SOCKET = 0xc8, // ...AttachSocketName
    SC_REL_LOC    = 0x11c,   // ...RelativeLocation / RelativeRotation / RelativeScale3D
    SC_REL_ROT    = 0x128,
    SC_REL_SCALE  = 0x134,
    SC_VISIBLE    = 0x14c,   // USceneComponent::bVisible / bHiddenInGame / Mobility
    SC_HIDDEN_IN_GAME = 0x14d,
    SC_MOBILITY   = 0x14f,   // 0 static, 1 stationary, 2 movable -- a teleport does nothing unless movable
    ACT_HIDDEN    = 0x58,    // AActor::bHidden
    HIT_SIZE      = 136, HIT_IMPACT = 0x18, HIT_IMPACT_N = 0x30,
    QP_SIZE       = 112,
};
enum { MAX_BONES = 200 };

// ------------------------------------------------------------------ engine functions
// void USkinnedMeshComponent::FlipEditableSpaceBases()
static const char* SIG_FLIP =
    "40 53 48 83 EC 20 0F B6 81 08 06 00 00 48 8B D9 84 C0 0F 89 ?? ?? ?? ?? 24 7F 88 81 08 06 00 00";
// bool UWorld::LineTraceSingleByChannel(FHitResult&, const FVector&, const FVector&, ECollisionChannel,
//                                       const FCollisionQueryParams&, const FCollisionResponseParams&)
static const char* SIG_TRACE =
    "48 8B C4 48 89 58 08 48 89 68 10 56 57 41 56 48 83 EC 70 F2 41 0F 10 01 48 8B EA F2 0F 11 40 18";
// void UCharacterMovementComponent::SetMovementMode(EMovementMode, uint8)
static const char* SIG_SET_MODE =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 33 F6 41 0F B6 C0 83 FA 06 8B FA 48 8B D9";
// void USkateboardExMovementComponent::PlaceInHand(...) -- what carries the board while you are on
// foot. It runs EVERY FRAME and writes the board's transform from hand-socket maths on the skater mesh,
// which is why a board set down simply walked back: nothing had taken it out of the game's hands. It is
// detoured and skipped for our skater while seated, and left alone for everyone else.
static const char* SIG_PLACE_IN_HAND =
    "48 8B C4 48 89 58 10 55 48 8D 68 D8 48 81 EC 20 01 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8";
// void ASkaterCharacterBase::Bail(const FString& reason, bool, bool bRagdoll) -- the SMALL overload, the
// one the game's own callers use. It gates on _canBail itself and synthesizes the bail location, whose
// last fallback derefs the skater's board link, so it is only called with that link up. Signature from
// the co-op host, which has shipped on it (dual-verified there); this module only CALLS it.
static const char* SIG_BAIL =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 70 80 B9 49 06 00 00 00 41 0F B6 F9 41 0F B6 F0 48 8B EA 48 8B D9";
// bool AActor::TeleportTo(const FVector&, const FRotator&, bool isTest, bool noCheck)
static const char* SIG_TELEPORT =
    "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 55 41 57 48 8D 6C 24 D1 48 81 EC B0 00 00 00 48 8D B9 30 01 00 00";
// void USceneComponent::DetachFromComponent(const FDetachmentTransformRules&)
static const char* SIG_DETACH =
    "48 89 5C 24 18 56 48 83 EC 40 48 83 B9 C0 00 00 00 00 48 8B F2 48 8B D9 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ??";
// bool USceneComponent::AttachToComponent(USceneComponent*, const FAttachmentTransformRules&, FName socket)
static const char* SIG_ATTACH =
    "40 55 53 56 41 54 41 56 48 8D AC 24 80 FA FF FF 48 81 EC 80 06 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4";
// void ASkateboardEx::SetSimulatePhysics(bool, bool) -- the WHOLE board. A board is a compound of a
// dozen parts (deck, trucks, wheels, rails), so simulating its root component alone leaves the rest
// kinematic and nothing falls; this is the call the game itself uses. Signature taken from the co-op
// host, which has shipped on it.
static const char* SIG_BOARD_SIM =
    "48 89 5C 24 18 55 56 57 48 83 EC 40 48 8D 99 88 02 00 00 48 8B F1 48 8B 03 48 8B CB 41 0F B6 F8";
// void ASkateboardEx::EnableRagDoll() / ::DisableRagDoll() -- THE way to let a board go. It is what the
// game itself uses when a board has to end up loose on the ground, and it does the whole job: switches
// the collision profile, simulates every part of the board, and leaves it in the state where its own
// tick reports collisions and rolling. Called on the board's INTERFACE sub-object (board + 0x280), not
// the actor -- the first thing it does is subtract that offset to get back to the actor.
//
// This replaces an attempt to set the board's movement mode by hand. That crashed: mode 0 turns out to
// be a throwdown, so the movement component started running throwdown physics on a board with no
// throwdown to run, and dereferenced its way off the end of a PID controller. Do not write that byte.
static const char* SIG_RAGDOLL_ON =
    "40 53 48 83 EC 20 48 8B 01 48 8B D9 ?? ?? ?? 84 C0 ?? ?? 48 8D 8B 80 FD FF FF 45 33 C0 B2 01 E8 ?? ?? ?? ??";
static const char* SIG_RAGDOLL_OFF =
    "40 53 48 83 EC 20 48 8B 01 48 8B D9 ?? ?? ?? 84 C0 ?? ?? 80 A3 70 01 00 00 FE 80 BB 71 01 00 00 00";
// void UPrimitiveComponent::SetSimulatePhysics(bool)
static const char* SIG_SET_SIM =
    "48 81 C1 C8 02 00 00 45 33 C0 E9 ?? ?? ?? ?? CC F2 0F 10 02 F2 0F 11 81 58 05 00 00 8B 42 08 89 81 60 05 00 00";
// UWorld* AActor::GetWorld()
static const char* SIG_GET_WORLD =
    "40 53 48 83 EC 20 8B 41 08 48 8B D9 C1 E8 04 A8 01 ?? ?? 48 8B 51 20 48 85 D2 ?? ?? 8B 42 08 C1 E8 0F";
typedef void  (*FlipFn)(void* mesh);
typedef bool  (*TraceFn)(void* world, void* hit, const float* a, const float* b, int channel, const void* qp, const void* rp);
typedef void  (*SetModeFn)(void* cmc, unsigned char mode, unsigned char custom);
typedef void* (*GetWorldFn)(void* actor);
typedef bool  (*TeleportFn)(void* actor, const float* loc, const float* rot, bool isTest, bool noCheck);
typedef void  (*BailFn)(void* skater, const void* reasonFString, bool a, bool bRagdoll);
typedef void  (*DetachFn)(void* comp, const void* rules);
typedef bool  (*AttachFn)(void* comp, void* parent, const void* rules, uint64_t socket);
typedef void  (*SetSimFn)(void* comp, bool simulate);
typedef void  (*BoardSimFn)(void* board, bool simulate, bool alsoFlipper);
typedef void  (*RagDollFn)(void* boardInterface);
static FlipFn     g_origFlip = nullptr;
static void*      g_flipAt   = nullptr;
static TraceFn    g_trace    = nullptr;
static SetModeFn  g_setMode  = nullptr;
static GetWorldFn g_getWorld = nullptr;
static TeleportFn g_teleport = nullptr;
static BailFn     g_bail     = nullptr;
static DetachFn   g_detach   = nullptr;
static AttachFn   g_attach   = nullptr;
static SetSimFn   g_setSim   = nullptr;
static BoardSimFn g_boardSim = nullptr;
static RagDollFn  g_ragOn  = nullptr;
static RagDollFn  g_ragOff = nullptr;
typedef void (*PlaceInHandFn)(void* moveComp, void* a, void* b, void* c);
static PlaceInHandFn g_origPlaceInHand = nullptr;
static void*         g_placeAt = nullptr;
static bool       g_ok       = false;

// ------------------------------------------------------------------ small math (UE FQuat order x y z w)
struct V3 { float x, y, z; };
struct Q4 { float x, y, z, w; };
static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static V3 mulv(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(V3 a) { return sqrtf(dot(a, a)); }
static V3 norm(V3 a) { const float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : v3(0, 0, 1); }
static V3 lerp(V3 a, V3 b, float t) { return add(a, mul(sub(b, a), t)); }
static Q4 q4(float x, float y, float z, float w) { Q4 q = { x, y, z, w }; return q; }
static Q4 qconj(Q4 q) { return q4(-q.x, -q.y, -q.z, q.w); }
// Hamilton product a*b: rotate by b first, then by a. UE composes child under parent as parent*child.
static Q4 qmul(Q4 a, Q4 b) {
    return q4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
static V3 qrot(Q4 q, V3 v) {
    const V3 u = v3(q.x, q.y, q.z);
    const V3 t = mul(cross(u, v), 2.0f);
    return add(add(v, mul(t, q.w)), cross(u, t));
}
static V3 qinv(Q4 q, V3 v) { return qrot(qconj(q), v); }
static Q4 qnorm(Q4 q) {
    const float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? q4(q.x / l, q.y / l, q.z / l, q.w / l) : q4(0, 0, 0, 1);
}
static Q4 qaxis(V3 axis, float deg) {
    const V3 a = norm(axis); const float h = deg * 0.0174532925f * 0.5f, s = sinf(h);
    return q4(a.x * s, a.y * s, a.z * s, cosf(h));
}
// The shortest rotation carrying direction a onto direction b.
static Q4 qfromto(V3 a, V3 b) {
    a = norm(a); b = norm(b);
    const float d = dot(a, b);
    if (d > 0.99999f) return q4(0, 0, 0, 1);
    if (d < -0.99999f) {
        V3 ax = cross(v3(1, 0, 0), a);
        if (len(ax) < 1e-4f) ax = cross(v3(0, 1, 0), a);
        ax = norm(ax);
        return q4(ax.x, ax.y, ax.z, 0.0f);
    }
    const V3 c = cross(a, b);
    return qnorm(q4(c.x, c.y, c.z, 1.0f + d));
}
static Q4 qblend(Q4 a, Q4 b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b = q4(-b.x, -b.y, -b.z, -b.w); }
    return qnorm(q4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ------------------------------------------------------------------ the rig
struct Bone { char name[48]; int parent; };
static Bone  g_bones[MAX_BONES];
static int   g_nBones = 0;
static void* g_bonesFor = nullptr;           // the USkeletalMesh the table was read from
static int   g_pelvis = -1, g_spine[4] = { -1, -1, -1, -1 }, g_nSpine = 0, g_neck = -1, g_head = -1;
static int   g_thigh[2], g_calf[2], g_foot[2], g_ball[2], g_clav[2], g_uarm[2], g_larm[2], g_hand[2];   // [0] left, [1] right
static int   g_handKids[2][6], g_nHandKids[2] = { 0, 0 };   // the hand's children: the palm, measured
// A foot's ROLL was never controlled: aiming the toes says nothing about which way the sole faces, so it
// kept whatever the animation had and a foot could sit toes-forward with its sole pointing at the sky.
// Both axes are measured off the STANDING pose, where the sole is known to face down, and kept in the
// foot bone's own space -- so a pose can ask for a sole direction and get it, on any rig.
static V3    g_toeLocal[2], g_soleLocal[2];
static bool  g_footFrameOk[2] = { false, false };
static bool  g_rigOk = false;
static bool  g_dumped = false;
// The parts of the body trail one another through a sit-down, so each bone blends by its own part's
// progress rather than one number for the whole skeleton.
enum { PART_LEGS = 0, PART_PELVIS, PART_SPINE, PART_HEAD, PART_ARMS, PART_N };
static int   g_group[MAX_BONES];
static uint8_t g_underHead[MAX_BONES]; // the head and everything hanging off it: hidden from inside
static uint8_t g_underNeck[MAX_BONES]; // the neck and everything hanging off it, head included: turned with the neck

static int SideOf(const char* n) {
    const size_t l = strlen(n);
    if (strstr(n, "_l_") || (l >= 2 && n[l - 2] == '_' && n[l - 1] == 'l')) return 0;
    if (strstr(n, "_r_") || (l >= 2 && n[l - 2] == '_' && n[l - 1] == 'r')) return 1;
    return -1;
}
static int ChildNamed(int parent, const char* sub, int side) {
    for (int i = 0; i < g_nBones; i++)
        if (g_bones[i].parent == parent && strstr(g_bones[i].name, sub) && (side < 0 || SideOf(g_bones[i].name) == side))
            return i;
    return -1;
}
static int AnyNamed(const char* sub) {
    for (int i = 0; i < g_nBones; i++) if (strstr(g_bones[i].name, sub)) return i;
    return -1;
}
static const char* BN(int i) { return i >= 0 ? g_bones[i].name : "-"; }

static bool ResolveRig(void* meshComp) {
    void* skel = twkP(meshComp, SKM_MESH);
    if (!skel) return false;
    if (skel == g_bonesFor) return g_rigOk;
    g_bonesFor = skel; g_rigOk = false; g_nBones = 0;
    const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
    const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
    const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
    if (!info || n <= 0 || n > MAX_BONES) { TwkLog("[sit] rig: %d bones -- unusable", n); return false; }
    for (int i = 0; i < n; i++) {
        char nb[96];
        if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) snprintf(nb, sizeof(nb), "bone%d", i);
        for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        snprintf(g_bones[i].name, sizeof(g_bones[i].name), "%s", nb);
        g_bones[i].parent = *(const int*)(info + i * 12 + 8);
        if (g_bones[i].parent >= i) g_bones[i].parent = -1;    // parents precede children; anything else is not a tree we can walk
    }
    g_nBones = n;
    g_pelvis = AnyNamed("pelvis");
    g_nSpine = 0;
    int s = g_pelvis >= 0 ? ChildNamed(g_pelvis, "spine", -1) : -1;
    while (s >= 0 && g_nSpine < 4) { g_spine[g_nSpine++] = s; s = ChildNamed(s, "spine", -1); }
    const int top = g_nSpine ? g_spine[g_nSpine - 1] : g_pelvis;
    g_neck = top >= 0 ? ChildNamed(top, "neck", -1) : -1;
    g_head = ChildNamed(g_neck >= 0 ? g_neck : top, "head", -1);
    for (int sd = 0; sd < 2; sd++) {
        g_thigh[sd] = g_pelvis >= 0 ? ChildNamed(g_pelvis, "thigh", sd) : -1;
        g_calf[sd]  = g_thigh[sd] >= 0 ? ChildNamed(g_thigh[sd], "calf", sd) : -1;
        g_foot[sd]  = g_calf[sd]  >= 0 ? ChildNamed(g_calf[sd],  "foot", sd) : -1;
        g_ball[sd]  = g_foot[sd]  >= 0 ? ChildNamed(g_foot[sd],  "ball", sd) : -1;
        if (g_ball[sd] < 0 && g_foot[sd] >= 0) g_ball[sd] = ChildNamed(g_foot[sd], "toe", sd);
        g_clav[sd] = -1;
        for (int k = g_nSpine - 1; k >= 0 && g_clav[sd] < 0; k--) g_clav[sd] = ChildNamed(g_spine[k], "clavicle", sd);
        g_uarm[sd] = g_clav[sd] >= 0 ? ChildNamed(g_clav[sd], "upperarm", sd) : (top >= 0 ? ChildNamed(top, "upperarm", sd) : -1);
        g_larm[sd] = g_uarm[sd] >= 0 ? ChildNamed(g_uarm[sd], "lowerarm", sd) : -1;
        if (g_larm[sd] < 0 && g_uarm[sd] >= 0) g_larm[sd] = ChildNamed(g_uarm[sd], "forearm", sd);
        g_hand[sd] = g_larm[sd] >= 0 ? ChildNamed(g_larm[sd], "hand", sd) : -1;
        // Everything hanging off the hand: enough to measure which way the fingers point and which way
        // the palm faces, without knowing a thing about the rig's axes.
        g_nHandKids[sd] = 0;
        if (g_hand[sd] >= 0)
            for (int i = 0; i < g_nBones && g_nHandKids[sd] < 6; i++)
                if (g_bones[i].parent == g_hand[sd]) g_handKids[sd][g_nHandKids[sd]++] = i;
    }
    // which part each bone moves with: the named ones by name, everything else with its parent (the
    // table is parents-first, so one pass does it)
    for (int i = 0; i < n; i++) {
        int g = -1;
        if (i == g_pelvis) g = PART_PELVIS;
        else if (i == g_neck || i == g_head) g = PART_HEAD;
        for (int k = 0; k < g_nSpine; k++) if (i == g_spine[k]) g = PART_SPINE;
        for (int sd = 0; sd < 2; sd++) {
            if (i == g_thigh[sd] || i == g_calf[sd] || i == g_foot[sd] || i == g_ball[sd]) g = PART_LEGS;
            if (i == g_clav[sd] || i == g_uarm[sd] || i == g_larm[sd] || i == g_hand[sd]) g = PART_ARMS;
        }
        if (g < 0) g = (g_bones[i].parent >= 0 && g_bones[i].parent < i) ? g_group[g_bones[i].parent] : PART_PELVIS;
        g_group[i] = g;
    }
    for (int i = 0; i < n; i++) {   // what hangs off the head (the first-person hide) and off the neck (the head turn)
        const int hb = g_head >= 0 ? g_head : g_neck;
        uint8_t uh = 0, un = 0;
        for (int b = i; b >= 0 && b < n; b = g_bones[b].parent) {
            if (b == hb) uh = 1;
            if (b == g_neck) un = 1;
            if (g_bones[b].parent >= b) break;
        }
        g_underHead[i] = uh; g_underNeck[i] = un ? 1 : uh;
    }
    g_rigOk = g_pelvis >= 0 && g_thigh[0] >= 0 && g_thigh[1] >= 0 && g_calf[0] >= 0 && g_calf[1] >= 0 &&
              g_foot[0] >= 0 && g_foot[1] >= 0;
    TwkLog("[sit] rig: %d bones, pelvis %s, spine x%d (%s..%s), neck %s, head %s", n, BN(g_pelvis), g_nSpine,
           BN(g_nSpine ? g_spine[0] : -1), BN(top), BN(g_neck), BN(g_head));
    TwkLog("[sit] rig: legs L %s > %s > %s > %s | R %s > %s > %s > %s", BN(g_thigh[0]), BN(g_calf[0]), BN(g_foot[0]), BN(g_ball[0]),
           BN(g_thigh[1]), BN(g_calf[1]), BN(g_foot[1]), BN(g_ball[1]));
    TwkLog("[sit] rig: arms L %s > %s > %s > %s | R %s > %s > %s > %s%s", BN(g_clav[0]), BN(g_uarm[0]), BN(g_larm[0]), BN(g_hand[0]),
           BN(g_clav[1]), BN(g_uarm[1]), BN(g_larm[1]), BN(g_hand[1]), g_rigOk ? "" : " -- LEGS UNRESOLVED, sitting off");
    for (int sd = 0; sd < 2; sd++) {
        char kids[200] = ""; int used = 0;
        for (int k = 0; k < g_nHandKids[sd]; k++) {
            const int w = snprintf(kids + used, sizeof(kids) - used, "%s%s", used ? ", " : "", BN(g_handKids[sd][k]));
            if (w > 0 && used + w < (int)sizeof(kids) - 1) used += w;
        }
        TwkLog("[sit] rig: %s hand children x%d: %s", sd ? "right" : "left", g_nHandKids[sd], kids[0] ? kids : "(none -- palms stay as animated)");
    }
    if (!g_rigOk && !g_dumped) {   // the names did not match the expected words: print them all, once
        g_dumped = true;
        for (int i = 0; i < n; i++) TwkLog("[sit]   bone %3d  parent %3d  %s", i, g_bones[i].parent, g_bones[i].name);
    }
    return g_rigOk;
}

// ------------------------------------------------------------------ state
enum { M_NONE = 0, M_LEDGE = 1, M_FLOOR = 2 };
static int   g_mode = M_NONE;
static int   g_sitting = 0;          // 1 = seated or sitting down; 0 = standing up or idle
static float g_alpha = 0.0f;         // 0 standing .. 1 seated
static float g_avel  = 0.0f;         // ...and how fast it is getting there (the spring, SitRealMove)
// What each part blends by is the body's progress read some way BACK -- a short history of it, so the
// trailing is a real delay in time rather than a shape given to a curve.
static float g_partA[PART_N] = { 0, 0, 0, 0, 0 };
static float g_anyA = 0.0f;          // the furthest along of them: the pose is live while this is above 0
struct AHist { float t, a; };
enum { HIST_N = 96 };
static AHist g_hist[HIST_N]; static int g_histN = 0, g_histAt = 0;
static float g_now = 0.0f;
// Which part trails which, as a share of the blend time. Down: the legs fold, the hips drop onto them,
// the spine follows, and the head and hands come last. Up: the chest goes forward over the feet and the
// hands push, then the hips rise and the legs straighten under them.
static const float kLagDown[PART_N] = { 0.00f, 0.05f, 0.14f, 0.22f, 0.20f };
static const float kLagUp[PART_N]   = { 0.06f, 0.12f, 0.00f, 0.08f, 0.02f };
// The first-person view. The look is two angles off the seat's facing, integrated from the right
// stick and clamped to what a seated head can do; the camera and the head bones share them, so what
// you see and what others see of you agree.
static int   g_fp = 0;               // the view key's toggle
static float g_fpBlend = 0.0f;       // 0 the game's camera .. 1 the eyes
static float g_lookYaw = 0.0f, g_lookPitch = 0.0f;     // where the stick has put the look, degrees (+ right, + up)
static float g_lookYawS = 0.0f, g_lookPitchS = 0.0f;   // ...smoothed: what the pose and the camera use
static V3    g_eyeLocal;             // the eyes in the HEAD bone's own space, measured standing
static bool  g_eyeOk = false;
static volatile LONG g_reqView = 0;  // the view key, waiting for the pump
static volatile LONG g_reqWatch = 0; // the d-pad: +1 next, -1 previous, waiting for the pump
static uint64_t g_mountUntilMs = 0;  // a Y on foot is the game's "get on the board": no sitting until it has played
// ---- the head look has three SOURCES: the stick (first person, free look), a watched player
// (first person, spectating) and the camera's own forward (everything else off the board). One pair
// of angles either way; the pose applies them seated, ApplyWalkLook applies them on the animation.
enum { LOOK_NONE = 0, LOOK_STICK, LOOK_CAMERA, LOOK_WATCH };
static int   g_lookSrc = LOOK_NONE;
static bool  g_lookLost = false;     // past the hold band: the head has given up and gone to the front
static float g_walkW = 0.0f;         // the off-board additive path's weight, eased on and off the board
static void* g_walkMesh = nullptr;   // the mesh the off-board head look is on
static void* g_walkSeen = nullptr;
static V3    g_bodyFwdW = { 1.0f, 0.0f, 0.0f };   // the skater's facing, world, each pump
static V3    g_eyeW = { 0.0f, 0.0f, 0.0f };       // the eyes, world, each pump (the seat's, or off the skater)
// ---- spectating: the d-pad's choice and the players there are to watch (the SessionOpenMP bridge)
enum { WATCH_FREE = 0, WATCH_AUTO = 1, WATCH_PEER = 2 };   // WATCH_PEER + k = the k-th player in the list
static int   g_watch = WATCH_FREE;
static int   g_watchPeerIdx = -1;    // a specific player, by the peer number (stable), not list position
struct Peer { void* actor; int idx; char name[40]; V3 pos; float speed; float stillS; bool fresh; };
static Peer  g_peers[16]; static int g_nPeers = 0;
static void* g_curWatch = nullptr;   // whom the auto attention is on
static float g_dwell = 0.0f, g_attnClock = 0.0f, g_outOfReachS = 0.0f;
static bool  g_watchInRange = false;
typedef int (*ProxyListFn)(void**, int);
typedef int (*ProxyIdxFn)(void*);
typedef int (*ProxyNameFn)(void*, char*, int);
typedef void (*SetHeadFn)(float, float);
typedef int  (*ProxyHeadFn)(void*, float*, float*);
static ProxyListFn g_proxyList = nullptr;
static ProxyIdxFn  g_proxyIdx  = nullptr;
static ProxyNameFn g_proxyName = nullptr;   // optional: an older host has numbers only
static SetHeadFn   g_setOwnHead = nullptr;  // optional: the head look on the wire, both ways
static ProxyHeadFn g_proxyHead  = nullptr;
// ---- other players' heads: their transported look, turned on THEIR rig at the pose flip. A proxy can
// wear a different garment merge and so a different skeleton, so the neck, the head and what hangs off
// them are resolved per skeletal mesh rather than assumed to match ours; the head's own axes are the
// base rig's and are shared.
struct PxRig  { void* skel; int n; int head, neck; uint8_t underNeck[MAX_BONES]; uint8_t underHead[MAX_BONES]; bool ok; };
struct PxHead { void* mesh; V3 bodyFwdW; float yaw, pitch, w; const PxRig* rig; };   // w eases with their on/off-board
static PxRig  g_pxRigs[4]; static int g_pxRigN = 0;
static PxHead g_px[16];    static int g_nPx = 0;
static uint64_t    g_bridgeTryMs = 0;
static bool        g_bridgeLogged = false;
static void* g_mesh = nullptr;       // the mesh being posed
static void* g_skater = nullptr;
static uint8_t g_savedMode = 0, g_savedCustom = 0;
static V3    g_seatW, g_faceW;       // the seat point and its facing, WORLD space (the capsule may yet be nudged)
static V3    g_fcur;                 // the character's own forward in mesh space: constant, the mesh rides the capsule
static V3    g_os, g_f, g_r, g_u;    // seat origin + frame in the mesh's component space, rebuilt every frame
static float g_turnDeg = 0.0f;       // yaw from the character's facing to the seat's, about up (+ = left)
static float g_blendEff = 450.0f;    // this sit's blend time: longer for a bigger turn
static int   g_style = 0, g_prevStyle = 0;   // which way of sitting, and the one being blended out of
static float g_styleBlend = 1.0f;
static float g_groundDz = 0.0f;      // ground height relative to the seat plane (<= 0)
// A floor sit used to assume the floor is flat, so on a slope or a kerb the feet hung in the air or sank
// into it. The body does not move while seated, so ONE patch of ground sampled at sit time is enough for
// every foot to find the surface actually under it -- and for every style, since the patch covers the
// whole area any of them reach into. Heights are relative to the seat plane; a spot with nothing under
// it keeps the seat's own level rather than dropping the foot into a hole.
//
// Two kinds of thing are out there, told apart by height. Up to kSupportUp above the seat a surface is
// something a foot rests ON, and the patch records it; anything standing taller is something the legs
// stop AT, and a fan of rays out from the seat at that height measures how far they can go on each
// bearing before they would be in it. A kerb lifts a foot; a wall folds the leg.
enum { GRID_NF = 20, GRID_NR = 13, RAY_N = 11 };
static const float kGridS  = 10.0f;                       // 10 cm cells...
static const float kGridF0 = -40.0f, kGridR0 = -60.0f;    // ...from 40 cm behind to 150 ahead, 60 either side
static const float kSupportUp = 45.0f;                    // resting height: a seated shin reaches no higher
static const float kProbeDown = 160.0f;
static const float kRaySpanDeg = 75.0f;                   // the fan: 75 deg either side of forward
static const float kRayReach = 170.0f;
static float g_grid[GRID_NF][GRID_NR];
static float g_rayDist[RAY_N];                            // first thing in the way out along each bearing, cm
static bool  g_gridOk = false, g_raysOk = false;
static float g_ankleH = 8.0f;        // ankle bone above the sole, measured standing
enum { REQ_NONE = 0, REQ_SIT = 1, REQ_CYCLE = 2, REQ_STAND = 3 };
static volatile LONG g_req = REQ_NONE;   // what the key asked for, waiting for the pump
static int   g_swallowRelease = 0;
static int   g_pressDown = 0, g_pressConsumed = 0, g_holdFired = 0;
static LONGLONG g_pressQpc = 0;
static volatile LONGLONG g_replayQpc = 0;   // when the replay manager last reported its mode
static volatile LONG g_replayMode = 0;      // ...and what that mode was
static bool InReplay();                     // defined with the rest of the replay gate below
static uint64_t g_keyFName = 0, g_fpKeyFName = 0;
static uint64_t g_notKey[24]; static int g_notKeyN = 0;
static volatile LONG g_applies = 0;
static int   g_faults = 0;
static int   g_dumpFrames = 0;
static char  g_last[160] = "idle";

static void HistPush(float t, float a) {
    g_hist[g_histAt].t = t; g_hist[g_histAt].a = a;
    g_histAt = (g_histAt + 1) % HIST_N; if (g_histN < HIST_N) g_histN++;
}
static float HistAt(float t) {   // the progress as it was at time t; the oldest kept, if that is further back
    if (g_histN == 0) return g_alpha;
    int i = (g_histAt - 1 + HIST_N) % HIST_N;
    AHist newer = g_hist[i];
    if (t >= newer.t) return newer.a;
    for (int k = 1; k < g_histN; k++) {
        i = (i - 1 + HIST_N) % HIST_N;
        const AHist older = g_hist[i];
        if (t >= older.t) {
            const float span = newer.t - older.t;
            return span > 1e-6f ? older.a + (newer.a - older.a) * (t - older.t) / span : newer.a;
        }
        newer = older;
    }
    return newer.a;
}
static void HistReset(float a) {
    g_histN = 0; g_histAt = 0; g_avel = 0.0f; g_anyA = a;
    for (int p = 0; p < PART_N; p++) g_partA[p] = a;
    if (a <= 0.0f) { g_fp = 0; g_fpBlend = 0.0f; g_watch = WATCH_FREE; g_watchPeerIdx = -1; g_curWatch = nullptr; }
}

// ------------------------------------------------------------------ traces (game thread)
static uint32_t g_ignoreUid = 0, g_ignoreUid2 = 0;   // the skater, and the board hanging off it
static bool Trace(void* world, V3 a, V3 b, V3* pt, V3* nrm) {
    alignas(16) uint8_t hit[HIT_SIZE]; memset(hit, 0, sizeof(hit));
    alignas(16) uint8_t qp[QP_SIZE];   memset(qp, 0, sizeof(qp));
    qp[0x15] = 1;                                   // bIgnoreTouches: blocking hits only
    *(int*)(qp + 0x48) = 0; *(int*)(qp + 0x4c) = 8; // IgnoreComponents: empty, inline capacity
    int nIgn = 0;                                   // IgnoreActors: skater and board, in the inline slots
    if (g_ignoreUid)  *(uint32_t*)(qp + 0x50 + 4 * nIgn++) = g_ignoreUid;
    if (g_ignoreUid2) *(uint32_t*)(qp + 0x50 + 4 * nIgn++) = g_ignoreUid2;
    *(int*)(qp + 0x68) = nIgn; *(int*)(qp + 0x6c) = 4;
    uint8_t rp[32]; memset(rp, 2, sizeof(rp));      // ECR_Block on every channel
    const float s[3] = { a.x, a.y, a.z }, e[3] = { b.x, b.y, b.z };
    bool ok = false;
    __try { ok = g_trace(world, hit, s, e, g_channel, qp, rp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
    if (!ok) return false;
    if (pt)  *pt  = *(const V3*)(hit + HIT_IMPACT);
    if (nrm) *nrm = *(const V3*)(hit + HIT_IMPACT_N);
    return true;
}

struct Seat { int kind; V3 point; V3 facing; float groundDz; float height; };

// Standing on a surface, a drop ahead: walk the floor forward with down-traces until it vanishes.
static bool FindDropAhead(void* world, V3 P, V3 fwd, float floorZ, float inset, Seat* out) {
    float lastOn = -1.0f, firstOff = -1.0f, dropZ = 0.0f;
    for (float d = 20.0f; d <= 120.0f; d += 15.0f) {
        const V3 xy = add(P, mul(fwd, d));
        V3 pt;
        const bool hit = Trace(world, v3(xy.x, xy.y, floorZ + 40.0f), v3(xy.x, xy.y, floorZ - 400.0f), &pt, nullptr);
        if (hit && pt.z >= floorZ - 12.0f) { lastOn = d; continue; }
        firstOff = d; dropZ = hit ? pt.z : floorZ - 400.0f; break;
    }
    if (lastOn < 0.0f || firstOff < 0.0f) return false;
    if (floorZ - dropZ < g_minLedge) return false;
    // two bisections put the edge within 4 cm
    for (int k = 0; k < 2; k++) {
        const float d = (lastOn + firstOff) * 0.5f;
        const V3 xy = add(P, mul(fwd, d));
        V3 pt;
        const bool hit = Trace(world, v3(xy.x, xy.y, floorZ + 40.0f), v3(xy.x, xy.y, floorZ - 400.0f), &pt, nullptr);
        if (hit && pt.z >= floorZ - 12.0f) lastOn = d; else firstOff = d;
    }
    const float edge = (lastOn + firstOff) * 0.5f;
    if (edge - inset < -20.0f) return false;           // the edge is behind the pelvis: not this one
    out->kind = M_LEDGE;
    out->point = add(P, mul(fwd, edge - inset)); out->point.z = floorZ;
    out->facing = fwd;
    out->groundDz = dropZ - floorZ;
    out->height = floorZ - dropZ;
    return true;
}

// Standing below a ledge, facing it: the face ahead, then its top, its depth, and the headroom.
static bool FindLedgeAhead(void* world, V3 P, V3 fwd, float floorZ, float inset, Seat* out) {
    V3 face, faceN; bool found = false; float best = 1e9f;
    const float hs[5] = { 35.0f, 55.0f, 75.0f, 95.0f, 115.0f };
    for (int i = 0; i < 5; i++) {
        if (hs[i] > g_maxLedge + 10.0f) break;
        const V3 a = v3(P.x, P.y, floorZ + hs[i]);
        V3 pt, n;
        if (!Trace(world, a, add(a, mul(fwd, g_reach)), &pt, &n)) continue;
        if (fabsf(n.z) > 0.5f || dot(n, fwd) > -0.4f) continue;   // not a wall facing us
        const float d = len(sub(pt, a));
        if (d < best) { best = d; face = pt; faceN = n; found = true; }
    }
    if (!found) return false;
    const V3 inward = norm(v3(-faceN.x, -faceN.y, 0.0f));
    V3 topPt, topN;
    const V3 t0 = add(face, mul(inward, 15.0f));
    if (!Trace(world, v3(t0.x, t0.y, floorZ + g_maxLedge + 40.0f), v3(t0.x, t0.y, floorZ + 5.0f), &topPt, &topN)) return false;
    if (topN.z < 0.75f) return false;
    const float h = topPt.z - floorZ;
    if (h < g_minLedge || h > g_maxLedge) return false;
    V3 deep;                                                       // the seat continues inward
    const V3 t1 = add(face, mul(inward, inset + 18.0f));
    if (!Trace(world, v3(t1.x, t1.y, topPt.z + 40.0f), v3(t1.x, t1.y, topPt.z - 10.0f), &deep, nullptr)) return false;
    if (fabsf(deep.z - topPt.z) > 8.0f) return false;
    const V3 seat = v3(face.x + inward.x * inset, face.y + inward.y * inset, topPt.z);
    if (Trace(world, add(seat, v3(0, 0, 10.0f)), add(seat, v3(0, 0, 130.0f)), nullptr, nullptr)) return false;   // no headroom
    out->kind = M_LEDGE;
    out->point = seat;
    out->facing = v3(-inward.x, -inward.y, 0.0f);
    out->groundDz = floorZ - topPt.z;
    out->height = h;
    return true;
}

// ------------------------------------------------------------------ the pose (inside the hook)
struct TF { Q4 q; V3 p; V3 s; };
static TF   g_C[MAX_BONES], g_L[MAX_BONES], g_Ct[MAX_BONES], g_Lt[MAX_BONES], g_Ltmp[MAX_BONES];
static bool g_inSub[MAX_BONES];

static void LocalOf(int i, const TF* comp, TF* loc) {
    const int p = g_bones[i].parent;
    loc[i].s = comp[i].s;
    if (p < 0) { loc[i].q = comp[i].q; loc[i].p = comp[i].p; return; }
    loc[i].q = qmul(qconj(comp[p].q), comp[i].q);
    V3 d = qinv(comp[p].q, sub(comp[i].p, comp[p].p));
    const V3 ps = comp[p].s;
    loc[i].p = v3(d.x / (fabsf(ps.x) > 1e-6f ? ps.x : 1.0f), d.y / (fabsf(ps.y) > 1e-6f ? ps.y : 1.0f), d.z / (fabsf(ps.z) > 1e-6f ? ps.z : 1.0f));
}
static void CompOf(int i, const TF* loc, TF* comp) {
    const int p = g_bones[i].parent;
    comp[i].s = loc[i].s;
    if (p < 0) { comp[i].q = loc[i].q; comp[i].p = loc[i].p; return; }
    comp[i].q = qmul(comp[p].q, loc[i].q);
    comp[i].p = add(comp[p].p, qrot(comp[p].q, mulv(loc[i].p, comp[p].s)));
}
static void RebuildSub(int i) {          // g_Lt -> g_Ct for everything under (not including) bone i
    for (int j = 0; j < g_nBones; j++) g_inSub[j] = false;
    g_inSub[i] = true;
    for (int j = i + 1; j < g_nBones; j++) {
        const int p = g_bones[j].parent;
        if (p >= 0 && g_inSub[p]) { CompOf(j, g_Lt, g_Ct); g_inSub[j] = true; }
    }
}
static void SetComp(int i, Q4 q, V3 p) { g_Ct[i].q = q; g_Ct[i].p = p; LocalOf(i, g_Ct, g_Lt); RebuildSub(i); }
static void TurnComp(int i, Q4 q) { SetComp(i, qmul(q, g_Ct[i].q), g_Ct[i].p); }
static void AimBone(int i, V3 from, V3 to) { if (len(from) > 1e-3f && len(to) > 1e-3f) TurnComp(i, qfromto(from, to)); }
// The middle joint of a two-bone chain from H to F with lengths a and b, bending toward pole.
static V3 MidJoint(V3 H, V3 F, float a, float b, V3 pole) {
    V3 hf = sub(F, H);
    float d = len(hf);
    const float reach = (a + b) * 0.995f;
    if (d > reach) { hf = mul(hf, reach / d); d = reach; }
    if (d < 1e-3f) return add(H, mul(norm(pole), a));
    const V3 along = mul(hf, 1.0f / d);
    const float cosA = clampf((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const float sinA = sqrtf(1.0f - cosA * cosA);
    V3 perp = sub(pole, mul(along, dot(pole, along)));
    if (len(perp) < 1e-4f) perp = cross(along, v3(0, 0, 1));
    perp = norm(perp);
    return add(add(H, mul(along, a * cosA)), mul(perp, a * sinA));
}
// A hand planted on something needs its own orientation, not just a place to be: the arm solve aims the
// forearm and leaves the wrist holding whatever the animation had, which is what makes a propped hand
// dangle. Both the finger direction and the palm plane are MEASURED off the hand's own children -- the
// mean child is where the fingers go, the spread between the outermost two lies across the palm -- so
// the roll is solved rather than guessed, on any rig.
static void HandOrient(int sd, V3 fingersWant, V3 palmNormalWant) {
    const int h = g_hand[sd];
    if (h < 0 || g_nHandKids[sd] < 1 || len(fingersWant) < 1e-4f) return;
    V3 mean = v3(0.0f, 0.0f, 0.0f);
    for (int k = 0; k < g_nHandKids[sd]; k++) mean = add(mean, g_Ct[g_handKids[sd][k]].p);
    mean = mul(mean, 1.0f / (float)g_nHandKids[sd]);
    V3 f0 = sub(mean, g_Ct[h].p);
    if (len(f0) < 1e-3f) return;
    f0 = norm(f0);
    const float palmSign = g_palmFlip[sd] ? -1.0f : 1.0f;
    const V3 f1 = norm(fingersWant);
    Q4 r = qfromto(f0, f1);
    if (g_nHandKids[sd] >= 2 && len(palmNormalWant) > 1e-4f) {
        const V3 across = sub(g_Ct[g_handKids[sd][g_nHandKids[sd] - 1]].p, g_Ct[g_handKids[sd][0]].p);
        if (len(across) > 1e-3f) {
            V3 n0 = mul(cross(f0, norm(across)), palmSign);
            V3 n1 = sub(palmNormalWant, mul(f1, dot(palmNormalWant, f1)));   // the part square to the fingers
            if (len(n0) > 1e-3f && len(n1) > 1e-3f) {
                n0 = qrot(r, norm(n0));
                r = qmul(qfromto(n0, norm(n1)), r);      // a pure roll about the fingers: both are square to them
            }
        }
    }
    TurnComp(h, r);
}
static V3 g_tu, g_tr, g_tf;      // the torso's own frame, rebuilt each pose by TorsoFrame() below
// The idle animation does not carry the shoulders level -- one arm rides higher than the other -- and
// since the arm solve begins at whatever the shoulder is doing, that lopsidedness survives into every
// seated pose. Both clavicles are re-aimed so the arms leave the body at the same angle: the forward and
// outward parts are averaged, and the height is taken from the LOWER of the two, so the raised side
// comes down to meet the other rather than the settled one being hoisted up. Mirrored across the body,
// so nothing here assumes which way a clavicle points on this rig.
static void LevelShoulders(float drop) {
    if (g_clav[0] < 0 || g_clav[1] < 0 || g_uarm[0] < 0 || g_uarm[1] < 0) return;
    V3 d[2];
    for (int sd = 0; sd < 2; sd++) {
        const V3 v = sub(g_Ct[g_uarm[sd]].p, g_Ct[g_clav[sd]].p);
        if (len(v) < 1e-3f) return;
        d[sd] = norm(v);
    }
    const float fwd  = (dot(d[0], g_tf) + dot(d[1], g_tf)) * 0.5f;
    const float side = (fabsf(dot(d[0], g_tr)) + fabsf(dot(d[1], g_tr))) * 0.5f;
    const float up   = fminf(dot(d[0], g_tu), dot(d[1], g_tu)) - drop;
    for (int sd = 0; sd < 2; sd++) {
        const float sg = sd ? 1.0f : -1.0f;
        AimBone(g_clav[sd], sub(g_Ct[g_uarm[sd]].p, g_Ct[g_clav[sd]].p),
                add(add(mul(g_tf, fwd), mul(g_tr, sg * side)), mul(g_tu, up)));
    }
}
// THE TORSO'S OWN FRAME. Everything about a shoulder is relative to the chest it hangs on, not to the
// world: levelling them against world up means any lean in the pose fights the levelling, and a setting
// that looked right at one lean is wrong at another. The chest's up is measured live from the posed
// spine, so the shoulders follow whatever the torso is doing, lean setting included.
static void TorsoFrame() {
    g_tu = g_u; g_tr = g_r; g_tf = g_f;
    const int top = g_neck >= 0 ? g_neck : (g_nSpine ? g_spine[g_nSpine - 1] : g_head);
    if (top < 0 || g_pelvis < 0) return;
    const V3 up = sub(g_Ct[top].p, g_Ct[g_pelvis].p);
    if (len(up) < 1e-3f) return;
    g_tu = norm(up);
    V3 r = cross(g_tu, g_f);
    if (len(r) < 1e-3f) return;
    g_tr = norm(r);
    g_tf = norm(cross(g_tr, g_tu));
}
// Point the toes, and if asked, roll the foot so its sole faces a given way -- the two together are a
// full orientation, solved the same way a planted hand is: aim first, then roll about the aimed axis.
static void FootOrient(int sd, V3 toeWant, V3 soleWant) {
    const int ft = g_foot[sd];
    if (ft < 0 || !g_footFrameOk[sd] || len(toeWant) < 1e-4f) return;
    const Q4 q = g_Ct[ft].q;
    const V3 t0 = qrot(q, g_toeLocal[sd]);
    if (len(t0) < 1e-3f) return;
    const V3 t1 = norm(toeWant);
    Q4 r = qfromto(norm(t0), t1);
    if (len(soleWant) > 1e-4f) {
        const V3 s0 = qrot(qmul(r, q), g_soleLocal[sd]);
        const V3 s1 = sub(soleWant, mul(t1, dot(soleWant, t1)));      // the part square to the toes
        if (len(s0) > 1e-3f && len(s1) > 1e-3f) r = qmul(qfromto(norm(s0), norm(s1)), r);
    }
    TurnComp(ft, r);
}
static void Leg(int sd, V3 K, V3 F, V3 footDir, V3 soleDir) {
    const int th = g_thigh[sd], ca = g_calf[sd], ft = g_foot[sd], ba = g_ball[sd];
    AimBone(th, sub(g_Ct[ca].p, g_Ct[th].p), sub(K, g_Ct[th].p));
    AimBone(ca, sub(g_Ct[ft].p, g_Ct[ca].p), sub(F, g_Ct[ca].p));
    if (g_footFrameOk[sd]) FootOrient(sd, footDir, soleDir);
    else if (ba >= 0) AimBone(ft, sub(g_Ct[ba].p, g_Ct[ft].p), footDir);
}
// A shoulder follows the arm. Left out, the collarbone stays put and the UPPER ARM has to make up the
// whole angle to wherever the hand is going -- and an upper arm swung far from where the collarbone
// points is exactly what collapses the deltoid, which is the pinched, narrow-shouldered look. So the
// collarbone takes a share of the reach first, the way a real one does when you prop a hand behind you,
// and the arm solve starts from a shoulder that is already facing the right way.
static void ShoulderFollow(int sd, V3 handTarget, float amount) {
    const int c = g_clav[sd], ua = g_uarm[sd];
    if (c < 0 || ua < 0 || amount <= 0.0f) return;
    const V3 cur = sub(g_Ct[ua].p, g_Ct[c].p);
    V3 want = sub(handTarget, g_Ct[c].p);
    if (len(cur) < 1e-3f || len(want) < 1e-3f) return;
    // ONLY THE SWING, never the height. A hand planted on the ground is below the shoulder, so aiming
    // the collarbone at it rotates the shoulder DOWNWARD and the whole shoulder line sinks -- which
    // looks worse than the pinch it was meant to fix. A real shoulder rolls back toward a reach behind
    // you and stays where it is vertically, so the wanted direction keeps the collarbone's own height
    // and only turns to follow the hand.
    want = sub(want, mul(g_tu, dot(want, g_tu)));
    if (len(want) < 1e-3f) return;
    want = add(norm(want), mul(g_tu, dot(norm(cur), g_tu)));
    TurnComp(c, qblend(q4(0.0f, 0.0f, 0.0f, 1.0f), qfromto(norm(cur), norm(want)), clampf(amount, 0.0f, 1.0f)));
}
static void Arm(int sd, V3 Hh, V3 pole) {
    const int ua = g_uarm[sd], la = g_larm[sd], ha = g_hand[sd];
    if (ua < 0 || la < 0 || ha < 0) return;
    const V3 S = g_Ct[ua].p;
    const float a1 = len(sub(g_Ct[la].p, S)), a2 = len(sub(g_Ct[ha].p, g_Ct[la].p));
    V3 d = sub(Hh, S); const float dl = len(d);
    if (dl > (a1 + a2) * 0.98f) Hh = add(S, mul(d, (a1 + a2) * 0.98f / dl));
    const V3 E = MidJoint(S, Hh, a1, a2, pole);
    AimBone(ua, sub(g_Ct[la].p, S), sub(E, S));
    AimBone(la, sub(g_Ct[ha].p, g_Ct[la].p), sub(Hh, g_Ct[la].p));
}
static V3 At(float fx, float ry, float uz) { return add(add(add(g_os, mul(g_f, fx)), mul(g_r, ry)), mul(g_u, uz)); }
static float Up(V3 p) { return dot(sub(p, g_os), g_u); }

// ------------------------------------------------------------------ the styles
// Four ways to sit, cycled with a tap of the sit key. A style is nothing but a set of targets in the
// seat frame; the solver above is what turns them into a pose, so a new one is a case in this switch.
enum { STYLE_COUNT = 4 };
static const char* const kStyleName[2][STYLE_COUNT] = {
    { "legs down",      "leaning back", "slouched",     "one leg folded" },   // on a ledge
    { "one knee up",    "cross-legged", "leaning back", "knees hugged" },   // on the ground
};
static const char* StyleName() { return kStyleName[g_mode == M_LEDGE ? 0 : 1][g_style & (STYLE_COUNT - 1)]; }

// The ground under a point of the seat frame, relative to the seat plane, read out of the sampled patch.
static float GroundAt(float fx, float ry) {
    if (!g_gridOk) return 0.0f;
    float a = (fx - kGridF0) / kGridS, b = (ry - kGridR0) / kGridS;
    a = clampf(a, 0.0f, (float)(GRID_NF - 1)); b = clampf(b, 0.0f, (float)(GRID_NR - 1));
    const int i0 = (int)a, j0 = (int)b;
    const int i1 = i0 + 1 < GRID_NF ? i0 + 1 : i0, j1 = j0 + 1 < GRID_NR ? j0 + 1 : j0;
    const float ti = a - (float)i0, tj = b - (float)j0;
    const float h0 = g_grid[i0][j0] + (g_grid[i0][j1] - g_grid[i0][j0]) * tj;
    const float h1 = g_grid[i1][j0] + (g_grid[i1][j1] - g_grid[i1][j0]) * tj;
    float h = h0 + (h1 - h0) * ti;
    // A foot is not a point. Read between samples a step edge is a ramp, and the foot lies through its
    // corner; the highest thing under any part of the foot is what it actually rests on.
    const float rc = g_footRadius / kGridS;
    const int d = (int)ceilf(rc);
    for (int i = i0 - d; i <= i1 + d; i++) for (int j = j0 - d; j <= j1 + d; j++) {
        if (i < 0 || j < 0 || i >= GRID_NF || j >= GRID_NR) continue;
        const float da = (float)i - a, db = (float)j - b;
        if (da * da + db * db <= rc * rc && g_grid[i][j] > h) h = g_grid[i][j];
    }
    return h;
}
// How far out along a bearing in the seat frame the legs can go before something is in the way.
static float ClearDist(float fx, float ry) {
    if (!g_raysOk) return 1e9f;
    const float ang = atan2f(ry, fx) * 57.2957795f;
    if (fabsf(ang) > kRaySpanDeg) return 1e9f;
    const float a = (ang + kRaySpanDeg) / (2.0f * kRaySpanDeg) * (float)(RAY_N - 1);
    const int k0 = (int)a, k1 = k0 + 1 < RAY_N ? k0 + 1 : k0;
    return fminf(g_rayDist[k0], g_rayDist[k1]);       // the nearer of the two rays either side
}
// Drop a foot onto whatever is under it, keeping the height the pose asked for: the pose says how the
// foot sits, the ground says where the floor is, and shifting rather than replacing keeps both.
static V3 OnGround(V3 F) {
    const V3 d = sub(F, g_os);
    return add(F, mul(g_u, GroundAt(dot(d, g_f), dot(d, g_r))));
}
// Stop a foot short of whatever stands in its way, on its own bearing out from the seat. The ankle is
// what gets placed and the toes reach on past it, so it stops a foot-length before the obstacle.
static V3 ShortOf(V3 F) {
    const V3 d = sub(F, g_os);
    const float fx = dot(d, g_f), ry = dot(d, g_r), uz = dot(d, g_u);
    const float r = sqrtf(fx * fx + ry * ry);
    const float room = ClearDist(fx, ry) - g_toeRoom;
    if (r < 1e-3f || r <= room) return F;
    const float sc = fmaxf(room, 6.0f) / r;
    return add(add(add(g_os, mul(g_f, fx * sc)), mul(g_r, ry * sc)), mul(g_u, uz));
}

// A leg hanging over the edge: planted when the ground is within shin reach, dangling when it is not.
static void LedgeLeg(int sd, float T, float C, float spread, float fwd, V3* knee, V3* foot, V3* dir) {
    const float sg = sd ? 1.0f : -1.0f;
    const V3 H = g_Ct[g_thigh[sd]].p;
    const V3 K = add(add(add(H, mul(g_f, T * fwd)), mul(g_u, -T * 0.12f)), mul(g_r, sg * spread));
    const float drop = Up(K) - (g_groundDz + g_ankleH);
    V3 F, fd;
    if (drop < C * 0.97f) {                       // the ground is within reach: plant the foot on it
        const float x = sqrtf(fmaxf(C * C - drop * drop, 0.0f));
        F = add(add(add(K, mul(g_f, x * 0.9f)), mul(g_u, -drop)), mul(g_r, sg * 3.0f));
        fd = add(g_f, mul(g_u, 0.12f));
    } else {                                      // nothing under it: let it hang
        F = add(add(add(K, mul(g_u, -C * 0.93f)), mul(g_f, C * 0.3f)), mul(g_r, sg * 2.0f));
        fd = add(mul(g_f, 0.55f), mul(g_u, -0.8f));
    }
    *knee = K; *foot = F; *dir = fd;
}

static void BuildStyle(int style) {
    for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
    const float T[2]  = { len(sub(g_C[g_calf[0]].p, g_C[g_thigh[0]].p)), len(sub(g_C[g_calf[1]].p, g_C[g_thigh[1]].p)) };
    const float Cl[2] = { len(sub(g_C[g_foot[0]].p, g_C[g_calf[0]].p)), len(sub(g_C[g_foot[1]].p, g_C[g_calf[1]].p)) };
    const bool ledge = g_mode == M_LEDGE;
    style &= (STYLE_COUNT - 1);
    // how the spine carries, per style
    float pelvisPitch, spinePitch, headPitch, seatBack = 0.0f, pelvisDrop = 0.0f;
    if (ledge) {
        switch (style) {
        default: pelvisPitch =  2.0f; spinePitch =   8.0f; headPitch = -7.0f; break;   // legs down
        case 1:  pelvisPitch = -9.0f; spinePitch = -14.0f; headPitch =  8.0f; seatBack = 3.0f; break;   // leaning back
        case 2:  pelvisPitch =  7.0f; spinePitch =  22.0f; headPitch = 12.0f; break;   // slouched
        case 3:  pelvisPitch =  1.0f; spinePitch =   8.0f; headPitch = -5.0f; break;   // one leg folded
        }
    } else {
        switch (style) {
        default: pelvisPitch = -12.0f; spinePitch =  -4.0f; headPitch = 12.0f; break;  // one knee up
        case 1:  pelvisPitch =  -4.0f; spinePitch =   2.0f; headPitch =  4.0f; pelvisDrop = 3.0f; break;  // cross-legged
        case 2:  pelvisPitch = -20.0f; spinePitch = -12.0f; headPitch = 10.0f; seatBack = 2.0f; break;  // leaning back
        case 3:  pelvisPitch = -12.0f; spinePitch =  14.0f; headPitch =  2.0f; break;  // knees hugged
        }
    }
    spinePitch += g_lean;
    // The whole body is turned onto the seat's facing first: the legs and hands below are placed in
    // that frame, so a pelvis left on the animated facing twists everything above the hips.
    const Q4 turned = qmul(qaxis(g_u, g_turnDeg), g_C[g_pelvis].q);
    SetComp(g_pelvis, qmul(qaxis(g_r, pelvisPitch), turned),
            At((ledge ? -2.0f : 0.0f) - seatBack, 0.0f, (ledge ? g_pelvisSeat : g_pelvisFloor) - pelvisDrop));
    for (int k = 0; k < g_nSpine; k++) TurnComp(g_spine[k], qaxis(g_r, spinePitch / (float)g_nSpine));
    if (g_head >= 0) TurnComp(g_head, qaxis(g_r, headPitch));
    else if (g_neck >= 0) TurnComp(g_neck, qaxis(g_r, headPitch));
    // The head goes where you look (the first-person view), split over the top of the spine, the
    // neck and the head the way a real turn is. Done BEFORE the arms are solved, so the shoulders'
    // share of a big turn is something the arms reach from rather than something that drags the
    // hands off the knees.
    if (fabsf(g_lookYawS) > 0.01f || fabsf(g_lookPitchS) > 0.01f) {
        const V3 rY = qrot(qaxis(g_u, g_lookYawS), g_r);          // the turned head's right, for the pitch
        if (g_nSpine > 0) TurnComp(g_spine[g_nSpine - 1], qaxis(g_u, g_lookYawS * 0.20f));
        if (g_neck >= 0)  TurnComp(g_neck, qmul(qaxis(rY, -g_lookPitchS * 0.30f), qaxis(g_u, g_lookYawS * 0.35f)));
        if (g_head >= 0)  TurnComp(g_head, qmul(qaxis(rY, -g_lookPitchS * (g_neck >= 0 ? 0.70f : 1.00f)),
                                                qaxis(g_u, g_lookYawS * (g_neck >= 0 ? 0.45f : 0.80f))));
    }

    TorsoFrame();
    LevelShoulders(g_shoulderDrop);

    V3 knee[2], foot[2];
    for (int sd = 0; sd < 2; sd++) {
        const float sg = sd ? 1.0f : -1.0f;
        const V3 H = g_Ct[g_thigh[sd]].p;
        V3 K = H, F = H, footDir = g_f, soleDir = v3(0.0f, 0.0f, 0.0f);
        V3 pole = g_u;                 // the knee's way out, for the legs placed by where the foot goes
        bool byDir = ledge;            // ...as against the ones built from directions, which are left be
        if (ledge) {
            if (style == 3 && sd == 1) {
                // ONE LEG FOLDED FLAT ON THE LEDGE, the other hanging. A knee pointing UP cannot look
                // relaxed here: the foot would have to stand a shin's length from the hip and the ledge
                // simply is not that deep, so the leg folds to an extreme angle whatever the numbers.
                // Turned onto its side it lies along the surface instead -- knee out, shin coming back
                // in toward the hanging thigh, which is a pose the geometry can actually hold.
                K = add(H, mul(norm(add(add(mul(g_f, 0.45f), mul(g_r, sg * 0.82f)), mul(g_u, -0.06f))), T[sd]));
                const V3 shin = norm(add(add(mul(g_r, -sg * 0.86f), mul(g_f, 0.24f)), mul(g_u, -0.04f)));
                F = add(K, mul(shin, Cl[sd]));
                // Toes forward, and the foot laid on its OUTER edge so its side is flat on the ledge:
                // the sole faces in toward the body rather than down or at the sky.
                footDir = add(mul(g_f, 0.92f), mul(g_r, -sg * 0.15f));
                soleDir = mul(g_r, -sg);
            } else {
                const float spread = style == 2 ? 7.0f : style == 1 ? 3.0f : 4.0f;
                const float fwd    = style == 1 ? 0.88f : 0.95f;
                LedgeLeg(sd, T[sd], Cl[sd], spread, fwd, &K, &F, &footDir);
            }
        } else if (style == 1) {
            // CROSS-LEGGED. Placed by DIRECTION, not by a foot position: the solver only rotates
            // bones, so a knee target one thigh-length along a direction lands exactly there, and a
            // foot one calf-length on from the knee lands exactly there too. Asking instead for a foot
            // near the body left the shin free to fold back UNDER the hips -- which read as kneeling.
            //
            // The shin runs inward AND BACKWARD, which is what tucks each foot under the opposite
            // thigh; carrying it straight across instead (no backward part) drove both feet into the
            // same spot in the middle, where they stacked with the soles up. The two shins are given
            // DIFFERENT depths so one crosses in front of the other, as legs actually cross, and
            // neither foot is asked to occupy the other's place.
            K = add(H, mul(norm(add(add(mul(g_f, 0.62f), mul(g_r, sg * 0.75f)), mul(g_u, -0.05f))), T[sd]));
            const V3 shin = norm(add(add(mul(g_r, -sg * 0.85f), mul(g_f, sd ? -0.12f : -0.42f)), mul(g_u, -0.02f)));
            F = add(K, mul(shin, Cl[sd]));
            footDir = add(shin, mul(g_u, 0.30f));           // the foot rests on its outer edge
            byDir = true;
        } else if (style == 2) {                            // both legs out in front
            F = At((T[sd] + Cl[sd]) * 0.94f, sg * T[sd] * 0.22f, g_ankleH);
            pole = add(g_u, mul(g_r, sg * 0.15f));
            // heels down, toes up and falling away from each other, the way a relaxed leg lies
            footDir = add(add(mul(g_f, 0.55f), mul(g_u, 0.70f)), mul(g_r, sg * 0.25f));
        } else if (style == 3) {                            // both knees up, feet drawn in close
            // Drawn in tighter than a knees-up sit: a hug only reads as one if the arms can actually
            // get round the shins, and at arm's length they cannot.
            F = At(T[sd] * 0.60f, sg * T[sd] * 0.24f, g_ankleH);
            pole = add(g_u, mul(g_f, 0.30f));
            footDir = add(g_f, mul(g_r, sg * 0.15f));
        } else if (sd == 1) {                               // the shipped floor pose: right knee up
            F = At(T[sd] * 0.5f, T[sd] * 0.35f, g_ankleH);
            pole = add(g_u, mul(g_f, 0.4f));
            footDir = g_f;
        } else {                                            // ...left leg stretched out
            F = At((T[sd] + Cl[sd]) * 0.92f, -T[sd] * 0.22f, g_ankleH + 2.0f);
            pole = g_u;
            footDir = add(add(mul(g_f, 0.80f), mul(g_u, 0.28f)), mul(g_r, sg * 0.30f));
        }
        if (!byDir) {
            // The floor is neither flat nor empty: the foot stops short of anything in its way, goes ON
            // whatever is under where it ends up, and the knee is solved for that place -- a foot up on
            // a kerb lifts the knee, one pulled back from a wall raises it. The ledge probes its own.
            F = ShortOf(F);
            F = OnGround(F);
            K = MidJoint(H, F, T[sd], Cl[sd], pole);
        }
        knee[sd] = K; foot[sd] = F;
        Leg(sd, K, F, footDir, soleDir);
    }

    // ...and what the hands do about it
    for (int sd = 0; sd < 2; sd++) {
        const float sg = sd ? 1.0f : -1.0f;
        V3 Hh, pole = add(add(mul(g_r, sg * 0.9f), mul(g_f, -0.5f)), mul(g_u, -0.2f));
        bool planted = false;         // this hand takes weight on the ground: flatten the palm onto it
        V3 fingersHint = v3(0.0f, 0.0f, 0.0f);   // where the fingers should point, if the arm's own line is wrong
        if (ledge) {
            switch (style) {
            default: Hh = add(add(knee[sd], mul(g_u, 8.0f)), mul(g_f, -8.0f)); break;          // on the thighs
            case 1:  // planted behind and WIDE, the same opening out the floor version wanted: hands in
                     // close leave the arms cramped against the ribs
                     Hh = At(-T[sd] * 0.48f, sg * T[sd] * 0.80f, 4.0f);
                     pole = add(add(mul(g_r, sg * 1.2f), mul(g_f, -0.7f)), mul(g_u, -0.25f));
                     planted = true; break;
            case 2:  Hh = add(add(add(knee[sd], mul(g_f, 7.0f)), mul(g_u, -4.0f)),             // elbows on knees
                              mul(g_r, -sg * 3.0f));
                     pole = add(add(mul(g_r, sg * 1.0f), mul(g_f, -0.3f)), mul(g_u, -0.6f)); break;
            case 3:  if (sd == 1) {      // the hand lies ON the shin, palm down along it, not hovering over
                         Hh = add(add(lerp(knee[1], foot[1], 0.45f), mul(g_u, 10.0f)), mul(g_r, sg * 2.0f));
                         pole = add(add(mul(g_r, sg * 1.0f), mul(g_f, -0.3f)), mul(g_u, -0.4f));
                         fingersHint = sub(foot[1], knee[1]);     // fingers run down the leg
                         planted = true;                          // ...and the palm lies flat on it
                     } else {                // ...the other hand rests on the ledge beside the hip
                         Hh = At(-T[0] * 0.16f, -T[0] * 0.62f, 4.0f);
                         pole = add(add(mul(g_r, -1.0f), mul(g_f, -0.55f)), mul(g_u, -0.2f));
                         // Reaching straight out sideways, the arm's own line points the fingers away
                         // from the body; a hand resting beside you on a ledge points them along it.
                         fingersHint = add(mul(g_f, 0.85f), mul(g_r, sg * 0.35f));
                         planted = true;
                     }
                     break;
            }
        } else {
            switch (style) {
            default: if (sd == 1) { Hh = add(add(add(knee[1], mul(g_u, 5.0f)), mul(g_r, 3.0f)), mul(g_f, -3.0f)); }
                     else { Hh = At(-14.0f, -30.0f, 3.0f);          // propped on the ground behind
                            Hh = OnGround(Hh);
                            pole = add(add(mul(g_r, -0.7f), mul(g_f, -0.6f)), mul(g_u, 0.1f));
                            planted = true; }
                     break;
            case 1:  Hh = add(add(knee[sd], mul(g_u, 6.0f)), mul(g_f, 1.0f)); break;           // hands on the knees
            case 2:  // propped behind and WIDE: the hands go down flat on the ground out past the
                     // shoulders, and the pole is mostly sideways so the arms splay rather than run
                     // straight down beside the hips
                     Hh = OnGround(At(-T[sd] * 0.45f, sg * T[sd] * 0.82f, 4.0f));
                     pole = add(add(mul(g_r, sg * 1.2f), mul(g_f, -0.7f)), mul(g_u, -0.25f));
                     planted = true; break;
            case 3: {   // ARMS AROUND THE SHINS. The hands have to clear the FRONT of the legs: a point
                        // on the centreline is the gap BETWEEN the knees, and that is where the arms
                        // fell. So the reach is measured from the front of the nearer shin and pushed
                        // out past it, with the elbows carried outside the knees by the pole.
                     // The grip rides the shins nearer the KNEES than the ankles, and the pole is
                     // mostly sideways: that is what carries the elbows out wide instead of tucking
                     // them against the legs.
                     const V3 mid = mul(add(lerp(knee[0], foot[0], 0.34f), lerp(knee[1], foot[1], 0.34f)), 0.5f);
                     Hh = add(add(add(mid, mul(g_f, 13.0f)), mul(g_r, sg * 4.0f)), mul(g_u, 2.0f));
                     pole = add(add(mul(g_r, sg * 1.7f), mul(g_f, -0.25f)), mul(g_u, -0.25f));
                     break; }
            }
        }
        ShoulderFollow(sd, Hh, g_shoulderFollow);
        Arm(sd, Hh, pole);
        // Palm flat down, and the fingers carry on the way the FOREARM is already pointing rather than
        // toward a fixed compass direction: a hand held square to the arm looks planted, one twisted off
        // it looks broken, and only the roll is ours to decide.
        if (planted && g_larm[sd] >= 0 && g_hand[sd] >= 0) {
            V3 fa = fingersHint;
            if (len(fa) < 1e-3f) {
                fa = sub(g_Ct[g_hand[sd]].p, g_Ct[g_larm[sd]].p);
                fa = sub(fa, mul(g_u, dot(fa, g_u)));           // flattened onto the ground
                if (len(fa) < 1e-3f) fa = mul(g_f, -1.0f);
                fa = add(norm(fa), mul(g_r, sg * 0.18f));
            }
            HandOrient(sd, fa, mul(g_u, -1.0f));
        }
    }
}

// The pose for this frame: the current style, blended out of the one it is replacing.
static void BuildTarget() {
    BuildStyle(g_style);
    if (g_styleBlend < 1.0f && g_prevStyle != g_style) {
        for (int i = 0; i < g_nBones; i++) g_Ltmp[i] = g_Lt[i];
        BuildStyle(g_prevStyle);
        const float t = g_styleBlend * g_styleBlend * (3.0f - 2.0f * g_styleBlend);
        for (int i = 0; i < g_nBones; i++) {
            g_Lt[i].q = qblend(g_Lt[i].q, g_Ltmp[i].q, t);
            g_Lt[i].p = lerp(g_Lt[i].p, g_Ltmp[i].p, t);
        }
    }
}

static void ReadC2W(const void* comp, Q4* q, V3* p, V3* sc) {
    const float* t = (const float*)((const uint8_t*)comp + SC_C2W);
    *q = q4(t[0], t[1], t[2], t[3]); *p = v3(t[4], t[5], t[6]);
    if (sc) *sc = v3(fabsf(t[8]) > 1e-6f ? t[8] : 1.0f, fabsf(t[9]) > 1e-6f ? t[9] : 1.0f, fabsf(t[10]) > 1e-6f ? t[10] : 1.0f);
}
// The seat frame in the mesh's own space. World-anchored so nothing the capsule does can move the
// seat; the turn is measured here too, from the character's fixed forward to the seat's facing.
static void SeatFrame(const void* mesh) {
    Q4 mq; V3 mp, ms; ReadC2W(mesh, &mq, &mp, &ms);
    g_u = norm(qinv(mq, v3(0, 0, 1)));
    g_f = qinv(mq, g_faceW); g_f = norm(sub(g_f, mul(g_u, dot(g_f, g_u))));
    g_r = norm(cross(g_u, g_f));
    const V3 d = qinv(mq, sub(g_seatW, mp));
    g_os = v3(d.x / ms.x, d.y / ms.y, d.z / ms.z);
    V3 fc = sub(g_fcur, mul(g_u, dot(g_fcur, g_u)));
    if (len(fc) < 1e-4f) { g_turnDeg = 0.0f; return; }
    fc = norm(fc);
    g_turnDeg = atan2f(dot(cross(fc, g_f), g_u), dot(fc, g_f)) * 57.2957795f;
}
static void ApplyPose(void* mesh) {
    SeatFrame(mesh);
    const int idx = *(const int*)((const uint8_t*)mesh + SKM_EDIT);
    if (idx < 0 || idx > 1) return;
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + idx * 0x10;
    uint8_t* data = *(uint8_t**)arr;
    const int n = *(const int*)(arr + 8);
    if (!data || n != g_nBones) return;
    for (int i = 0; i < n; i++) {
        const float* t = (const float*)(data + i * 48);
        g_C[i].q = q4(t[0], t[1], t[2], t[3]); g_C[i].p = v3(t[4], t[5], t[6]); g_C[i].s = v3(t[8], t[9], t[10]);
    }
    for (int i = 0; i < n; i++) LocalOf(i, g_C, g_L);
    BuildTarget();
    // blend in local space so limbs swing rather than slide -- each part of the body by its own progress,
    // which is what lets the hips lead and the head and hands follow -- then publish component space
    for (int i = 0; i < n; i++) {
        const float a = g_partA[g_group[i]];
        g_Lt[i].q = qblend(g_L[i].q, g_Lt[i].q, a);
        g_Lt[i].p = lerp(g_L[i].p, g_Lt[i].p, a);
    }
    for (int i = 0; i < n; i++) CompOf(i, g_Lt, g_Ct);
    // With the camera inside the head, the head is not drawn: its scale (and its children's) goes to
    // zero in the published pose, which collapses the skin to a point. The co-op held pose carries
    // rotation and position only, so other players keep seeing your head.
    const bool hide = g_hideHead && g_fpBlend >= 0.6f;
    for (int i = 0; i < n; i++) {
        float* t = (float*)(data + i * 48);
        t[0] = g_Ct[i].q.x; t[1] = g_Ct[i].q.y; t[2] = g_Ct[i].q.z; t[3] = g_Ct[i].q.w;
        t[4] = g_Ct[i].p.x; t[5] = g_Ct[i].p.y; t[6] = g_Ct[i].p.z;
        if (hide && g_underHead[i]) { t[8] = 0.0f; t[9] = 0.0f; t[10] = 0.0f; }
    }
    InterlockedIncrement(&g_applies);
    if (g_dumpFrames > 0 && g_alpha >= 0.999f) {
        g_dumpFrames--;
        for (int sd = 0; sd < 2; sd++)
            TwkLog("[sit] %s hip (%.0f %.0f %.0f) knee (%.0f %.0f %.0f) foot (%.0f %.0f %.0f) hand (%.0f %.0f %.0f)", sd ? "R" : "L",
                   g_Ct[g_thigh[sd]].p.x, g_Ct[g_thigh[sd]].p.y, g_Ct[g_thigh[sd]].p.z,
                   g_Ct[g_calf[sd]].p.x, g_Ct[g_calf[sd]].p.y, g_Ct[g_calf[sd]].p.z,
                   g_Ct[g_foot[sd]].p.x, g_Ct[g_foot[sd]].p.y, g_Ct[g_foot[sd]].p.z,
                   g_hand[sd] >= 0 ? g_Ct[g_hand[sd]].p.x : 0.0f, g_hand[sd] >= 0 ? g_Ct[g_hand[sd]].p.y : 0.0f, g_hand[sd] >= 0 ? g_Ct[g_hand[sd]].p.z : 0.0f);
    }
}

// Turn a head to a look, on a mesh's editable pose, on top of whatever the animation did (so a walk
// cycle keeps its bob and the head still turns). The turn is ADDITIVE: yaw and pitch are degrees off
// the body's facing, so they are applied as rotations about the BODY's own up and right, and a look of
// zero leaves the animator's head pose exactly alone.
//
// It used to be absolute -- measure which way the head bone points, rotate it to where the look wants
// it -- and that needs a bone-axis convention this rig will not give up. Measured off the animated
// pose the head came out pitched at the ground (the idle looks down); off the reference pose it came
// out yawed a quarter turn (the bind pose does not face the way the clips do); carried through the
// pelvis it was still 45 deg out, because the animated pelvis has a yaw of its own (the log measured
// 40 deg of it). Every one of those is a frame question, and an additive turn never asks it. The neck
// takes a share and the head the rest, as a real turn does, each a RIGID turn of the bone and
// everything hanging off it (hair, a hat) about the bone's own origin -- so a proxy's rig, whose bone
// tables are not ours, works the same way.
static void HeadTurnOn(void* mesh, int n, int neck, int head, const uint8_t* underNeck, const uint8_t* underHead,
                       V3 bodyFwdW, float yaw, float pitch, float w) {
    if (head < 0 || w <= 0.001f) return;
    yaw *= w; pitch *= w;
    if (fabsf(yaw) < 0.02f && fabsf(pitch) < 0.02f) return;
    const int idx = *(const int*)((const uint8_t*)mesh + SKM_EDIT);
    if (idx < 0 || idx > 1) return;
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + idx * 0x10;
    uint8_t* data = *(uint8_t**)arr;
    const int bn = *(const int*)(arr + 8);
    if (!data || bn != n) return;
    Q4 mq; V3 mp; ReadC2W(mesh, &mq, &mp, nullptr);
    const V3 up = norm(qinv(mq, v3(0.0f, 0.0f, 1.0f)));
    V3 bf = qinv(mq, bodyFwdW); bf = norm(sub(bf, mul(up, dot(bf, up))));
    const V3 rY = qrot(qaxis(up, yaw), cross(up, bf));   // the right axis AFTER the yaw: the pitch rides it
    auto boneP = [&](int b) { const float* t = (const float*)(data + b * 48); return v3(t[4], t[5], t[6]); };
    auto turnBy = [&](float f) { return qmul(qaxis(rY, -pitch * f), qaxis(up, yaw * f)); };
    auto turn = [&](int pivot, const uint8_t* under, Q4 d) {
        const V3 piv = boneP(pivot);
        for (int b = 0; b < n; b++) {
            if (!under[b]) continue;
            float* t = (float*)(data + b * 48);
            const Q4 q = qmul(d, q4(t[0], t[1], t[2], t[3]));
            const V3 pp = add(piv, qrot(d, sub(v3(t[4], t[5], t[6]), piv)));
            t[0] = q.x; t[1] = q.y; t[2] = q.z; t[3] = q.w; t[4] = pp.x; t[5] = pp.y; t[6] = pp.z;
        }
    };
    if (neck >= 0 && neck != head) { turn(neck, underNeck, turnBy(0.35f)); turn(head, underHead, turnBy(0.65f)); }
    else                            { turn(head, underHead, turnBy(1.0f)); }
}
// Ours: the look the walk path is applying this frame.
static void ApplyWalkLook(void* mesh) {
    const float w = g_walkW * g_walkW * (3.0f - 2.0f * g_walkW);
    const int hb = g_head >= 0 ? g_head : g_neck;
    HeadTurnOn(mesh, g_nBones, g_head >= 0 ? g_neck : -1, hb, g_underNeck, g_underHead, g_bodyFwdW, g_lookYawS, g_lookPitchS, w);
}
// A proxy's rig: its neck, its head, and what hangs off each, read off ITS skeletal mesh once.
static const PxRig* PxRigFor(void* mesh) {
    void* skel = twkP(mesh, SKM_MESH);
    if (!skel) return nullptr;
    for (int i = 0; i < g_pxRigN; i++) if (g_pxRigs[i].skel == skel) return g_pxRigs[i].ok ? &g_pxRigs[i] : nullptr;
    PxRig* r = &g_pxRigs[g_pxRigN < 4 ? g_pxRigN++ : 3];
    memset(r, 0, sizeof(*r)); r->skel = skel; r->head = -1; r->neck = -1;
    const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
    const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
    const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
    if (!info || n <= 0 || n > MAX_BONES) return nullptr;
    int parent[MAX_BONES]; int headLoose = -1;
    for (int i = 0; i < n; i++) {
        char nb[96];
        if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) nb[0] = 0;
        for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        parent[i] = *(const int*)(info + i * 12 + 8);
        if (parent[i] >= i) parent[i] = -1;
        if (r->neck < 0 && strstr(nb, "neck")) r->neck = i;
        if (r->head < 0 && strcmp(nb, "head") == 0) r->head = i;
        if (headLoose < 0 && strstr(nb, "head") && r->neck >= 0 && parent[i] == r->neck) headLoose = i;
    }
    if (r->head < 0) r->head = headLoose;
    r->n = n;
    if (r->head < 0) { TwkLog("[sit] a player's rig has no head bone we know -- their head look is not applied"); return nullptr; }
    for (int i = 0; i < n; i++) {
        uint8_t uh = 0, un = 0;
        for (int b = i; b >= 0 && b < n; b = parent[b]) { if (b == r->head) uh = 1; if (b == r->neck) un = 1; }
        r->underHead[i] = uh; r->underNeck[i] = un ? 1 : uh;
    }
    r->ok = true;
    TwkLog("[sit] a player's rig: %d bones, neck %d, head %d -- their head look applies", n, r->neck, r->head);
    return r;
}

static void __fastcall hkFlip(void* mesh) {
    if (mesh && g_rigOk && !InReplay()) {
        if (mesh == g_mesh && g_anyA > 0.0f) {
            __try { ApplyPose(mesh); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_faults++; g_mesh = nullptr; g_sitting = 0; g_alpha = 0.0f; HistReset(0.0f);
                TwkLog("[sit] fault while posing -- released");
            }
        } else if (mesh == g_walkMesh && g_walkW > 0.0f) {
            __try { ApplyWalkLook(mesh); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_faults++; g_walkMesh = nullptr; g_headLook = 0;
                TwkLog("[sit] fault in the head look -- off for this run");
            }
        } else {
            for (int i = 0; i < g_nPx; i++) {
                if (g_px[i].mesh != mesh) continue;
                __try {
                    const PxRig* r = g_px[i].rig;
                    HeadTurnOn(mesh, r->n, r->neck, r->head, r->underNeck, r->underHead, g_px[i].bodyFwdW, g_px[i].yaw, g_px[i].pitch,
                               g_px[i].w * g_px[i].w * (3.0f - 2.0f * g_px[i].w));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    g_faults++; g_nPx = 0; g_proxyHead = nullptr;
                    TwkLog("[sit] fault turning a player's head -- theirs off for this run");
                }
                break;
            }
        }
    }
    g_origFlip(mesh);
}

// ------------------------------------------------------------------ the board
static void* g_boardMoveComp = nullptr;      // whose carry to skip while the board is down
static void __fastcall hkPlaceInHand(void* moveComp, void* a, void* b, void* c) {
    if (moveComp && moveComp == g_boardMoveComp) return;    // ours, and it is sitting on the ground
    if (g_origPlaceInHand) g_origPlaceInHand(moveComp, a, b, c);
}
// While you sit, the board is set down rather than dropped or left floating in your hand. Set down: it
// is detached, tossed to a spot beside or behind you and then SIMULATED, so it finds its own rest --
// which means the placement never has to be exact, and a slope or a kerb sorts itself out. Everything
// is remembered and put back when you stand, so nothing about the board is left changed: no item to
// pick up again, nothing that can roll away while the game thinks you are still carrying it.
static void* g_board = nullptr, *g_boardComp = nullptr, *g_boardParent = nullptr;
static uint64_t g_boardSocket = 0;
static float g_boardRel[9];              // location, rotation and scale, as they were
static bool  g_boardResting = false;
static V3    g_boardPut = { 0.0f, 0.0f, 0.0f };   // where we put it, to see whether it stayed
static int   g_boardWatch = 0;
static unsigned g_rand = 0;
static float Rand01() { g_rand = g_rand * 1664525u + 1013904223u; return (float)((g_rand >> 8) & 0xFFFF) / 65535.0f; }
static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

static void RestBoard(void* skater, const V3& seatPoint, const V3& facing, bool ledge, float groundDz) {
    g_boardResting = false; g_board = nullptr; g_boardComp = nullptr;
    if (!g_boardRest || !g_detach || !g_teleport || !g_setSim || !skater) return;
    __try {
        void* board = twkP(skater, SK_BOARD);
        void* comp = board ? twkP(board, ACT_ROOT) : nullptr;
        if (!board || !comp) return;
        g_boardParent = twkP(comp, SC_ATTACH_PARENT);
        g_boardSocket = *(const uint64_t*)((const uint8_t*)comp + SC_ATTACH_SOCKET);
        memcpy(g_boardRel + 0, (const uint8_t*)comp + SC_REL_LOC,   12);
        memcpy(g_boardRel + 3, (const uint8_t*)comp + SC_REL_ROT,   12);
        memcpy(g_boardRel + 6, (const uint8_t*)comp + SC_REL_SCALE, 12);
        // where it goes: beside you on the floor, behind you on a ledge (the front of a ledge is the
        // drop, and a board left there would simply fall off it)
        const V3 fwdW = norm(v3(facing.x, facing.y, 0.0f));
        const V3 rightW = norm(cross(v3(0.0f, 0.0f, 1.0f), fwdW));
        // AHEAD of you rather than beside: off a ledge that means it drops past the face and can come to
        // rest leaning against it, and on the flat it simply lands in front where you can see it.
        const float side = Rand01() < 0.5f ? -1.0f : 1.0f;
        const float out  = g_boardOut * RandRange(0.20f, 0.75f);
        const float along = g_boardAhead * RandRange(0.80f, 1.35f);
        V3 at = add(add(seatPoint, mul(rightW, side * out)), mul(fwdW, along));
        at.z += (ledge ? 0.0f : groundDz) + g_boardDrop;
        // turned any which way and tipped, so it topples and settles the way a set-down board does
        const float yaw = atan2f(fwdW.y, fwdW.x) * 57.2957795f + RandRange(-60.0f, 60.0f);
        const float rot[3] = { RandRange(-22.0f, 22.0f), yaw, RandRange(-28.0f, 28.0f) };   // pitch, yaw, roll
        const float loc[3] = { at.x, at.y, at.z };
        const uint8_t keepWorld[4] = { 1, 1, 1, 0 };        // detach and stay where you are
        g_detach(comp, keepWorld);
        *(unsigned char*)((uint8_t*)comp + SC_MOBILITY) = 2;    // movable, or a teleport is refused outright
        g_teleport(board, loc, rot, false, true);
        const bool moved = g_teleport ? true : false;
        if (g_ragOn)         g_ragOn((uint8_t*)board + BD_IFACE);   // the game's own way to let it go
        else if (g_boardSim) g_boardSim(board, true, false);
        else                 g_setSim(comp, true);
        g_boardMoveComp = twkP(board, BD_MOVECOMP);
        g_board = board; g_boardComp = comp; g_boardResting = true;
        g_boardPut = at; g_boardWatch = 1;
        const float* w = (const float*)((const uint8_t*)comp + SC_C2W);
        char bn[64] = "?", cn[64] = "?";
        {   void* bc = *(void**)((uint8_t*)board + 0x10);
            if (bc) GrindPop_FNameToString((const uint8_t*)bc + 0x18, bn, sizeof(bn));
            void* cc = *(void**)((uint8_t*)comp + 0x10);
            if (cc) GrindPop_FNameToString((const uint8_t*)cc + 0x18, cn, sizeof(cn)); }
        TwkLog("[sit] board set down %.0f cm to the %s, %.0f cm ahead, from %.0f cm up | %s / %s, "
               "actor hidden %u, comp visible %u hiddenInGame %u mobility %u, teleport %s, now at (%.0f %.0f %.0f) wanted (%.0f %.0f %.0f)",
               out, side < 0.0f ? "left" : "right", along, g_boardDrop, bn, cn, (unsigned)*(const unsigned char*)((const uint8_t*)board + ACT_HIDDEN),
               (unsigned)*(const unsigned char*)((const uint8_t*)comp + SC_VISIBLE),
               (unsigned)*(const unsigned char*)((const uint8_t*)comp + SC_HIDDEN_IN_GAME),
               (unsigned)*(const unsigned char*)((const uint8_t*)comp + SC_MOBILITY),
               moved ? "ok" : "FAILED", w[4], w[5], w[6], at.x, at.y, at.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; g_boardResting = false; }
}
static void ReleaseBoard() {
    if (!g_boardResting || !g_boardComp) { g_boardResting = false; return; }
    __try {
        if (g_ragOff && g_board)        g_ragOff((uint8_t*)g_board + BD_IFACE);
        else if (g_boardSim && g_board)  g_boardSim(g_board, false, false);
        else if (g_setSim)               g_setSim(g_boardComp, false);
        memcpy((uint8_t*)g_boardComp + SC_REL_LOC,   g_boardRel + 0, 12);
        memcpy((uint8_t*)g_boardComp + SC_REL_ROT,   g_boardRel + 3, 12);
        memcpy((uint8_t*)g_boardComp + SC_REL_SCALE, g_boardRel + 6, 12);
        const uint8_t keepRelative[4] = { 0, 0, 0, 0 };
        if (g_attach && g_boardParent) g_attach(g_boardComp, g_boardParent, keepRelative, g_boardSocket);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    g_boardResting = false; g_board = nullptr; g_boardComp = nullptr; g_boardMoveComp = nullptr;
}

// ------------------------------------------------------------------ sit / stand (game thread)
// The replay editor drives the skater itself and owns the sit key (it is the exit button). Sitting has
// no business running in there: the pose would be written over the replay's own, and swallowing the key
// would trap the player in the editor. cloth_sim owns the AReplayManager::Tick detour and calls this.
//
// THE MANAGER TICKING IS NOT THE SIGNAL. It is an actor in every level and ticks every frame to record,
// so "it ticked recently" is true the whole time you are skating -- which is what stopped the sit key
// working at all. Its MODE is the signal: 2 is replay playback, the same value SessionOpenMP has always
// used for "the local player is in a replay". The mode is logged on every change, so the other values
// name themselves in the field if this ever needs to be finer.
static bool InReplay() {
    if (g_replayMode != 2) return false;
    const LONGLONG t = g_replayQpc;
    if (!t) return false;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    return (now.QuadPart - t) < f.QuadPart * 2;    // and the manager is still reporting it
}
static void StandUp(const char* why);
void Sit_NoteReplayTick(void* replayManager) {
    if (!replayManager) return;
    LONG mode = 0;
    __try { mode = *(const unsigned char*)((const unsigned char*)replayManager + RM_MODE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    g_replayQpc = t.QuadPart;
    const LONG was = InterlockedExchange(&g_replayMode, mode);
    if (was != mode) TwkLog("[sit] replay mode %ld -> %ld%s", was, mode, mode == 2 ? " (playback: sitting stands down)" : "");
    if (mode == 2 && (g_sitting || g_anyA > 0.0f)) { StandUp("the replay editor opened"); g_alpha = 0.0f; HistReset(0.0f); }
}
static bool OnFoot(void* sk) {
    void* ai = FootPlace_AnimInstance();
    return sk && ai && twkB(ai, AN_ON_BOARD) == 0;
}
static void StandUp(const char* why) {
    if (!g_sitting) return;
    g_sitting = 0;
    g_fp = 0;                       // the camera comes back with the body
    if (g_realMove) g_avel = -0.6f / ((g_blendEff > 50.0f ? g_blendEff : 50.0f) * 0.001f);   // the push off: up is brisker than down
    ReleaseBoard();
    void* move = g_skater ? twkP(g_skater, CH_MOVE) : nullptr;
    if (move && g_setMode) { __try { g_setMode(move, g_savedMode, g_savedCustom); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; } }
    snprintf(g_last, sizeof(g_last), "standing (%s)", why);
    TwkLog("[sit] standing up: %s", why);
}
static void TrySit(void* sk) {
    void* mesh = twkP(sk, CH_MESH);
    void* root = twkP(sk, ACT_ROOT);
    void* cap  = twkP(sk, CH_CAPSULE);
    void* move = twkP(sk, CH_MOVE);
    if (!mesh || !root || !cap || !move) { TwkLog("[sit] skater has no mesh/root/capsule/movement"); return; }
    {   // Only from plain walking (UE MOVE_Walking = 1): the mount is a transition and a sit begun under
        // it lands in a state neither side can leave (field: "sitting while skating, stuck"). The Y
        // hold-off covers the frames before the mode changes.
        const int mm = twkB(move, MOVE_MODE), cm = twkB(move, MOVE_MODE + 1);
        const uint64_t nowMs = GetTickCount64();
        if (mm != 1 || nowMs < g_mountUntilMs) {
            snprintf(g_last, sizeof(g_last), "not now (movement %d/%d%s)", mm, cm, nowMs < g_mountUntilMs ? ", getting on the board" : "");
            TwkLog("[sit] not sitting: movement %d/%d%s", mm, cm, nowMs < g_mountUntilMs ? " -- getting on the board" : " -- not plain walking");
            return;
        }
    }
    if (!ResolveRig(mesh)) return;
    void* world = nullptr;
    __try { world = g_getWorld(sk); } __except (EXCEPTION_EXECUTE_HANDLER) { world = nullptr; }
    if (!world) { TwkLog("[sit] no world"); return; }
    g_ignoreUid = *(const uint32_t*)((const uint8_t*)sk + UOBJ_INDEX);
    { void* bd = twkP(sk, SK_BOARD); g_ignoreUid2 = bd ? *(const uint32_t*)((const uint8_t*)bd + UOBJ_INDEX) : 0; }
    Q4 rq; V3 P; ReadC2W(root, &rq, &P, nullptr);
    V3 fwd = qrot(rq, v3(1, 0, 0)); fwd.z = 0.0f; fwd = norm(fwd);
    const float half = twkF(cap, CAP_HALF);
    float floorZ = P.z - half;
    V3 fp;
    if (Trace(world, P, v3(P.x, P.y, P.z - half - 80.0f), &fp, nullptr)) floorZ = fp.z;
    // the rig's standing measures, off the READ buffer (the pose on screen now)
    Q4 mq; V3 mp; ReadC2W(mesh, &mq, &mp, nullptr);
    const int ridx = *(const int*)((const uint8_t*)mesh + SKM_READ);
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + (ridx == 1 ? 0x10 : 0);
    const uint8_t* data = *(const uint8_t* const*)arr;
    const int n = *(const int*)(arr + 8);
    if (!data || n != g_nBones) { TwkLog("[sit] pose buffer has %d bones, rig %d", n, g_nBones); return; }
    const V3 up = qinv(mq, v3(0, 0, 1));
    float thigh = 0.0f, ankle = 1e9f;
    for (int sd = 0; sd < 2; sd++) {
        const float* th = (const float*)(data + g_thigh[sd] * 48), *ca = (const float*)(data + g_calf[sd] * 48), *ft = (const float*)(data + g_foot[sd] * 48);
        thigh += len(sub(v3(ca[4], ca[5], ca[6]), v3(th[4], th[5], th[6]))) * 0.5f;
        const float ah = dot(v3(ft[4], ft[5], ft[6]), up);
        if (ah < ankle) ankle = ah;
    }
    for (int sd = 0; sd < 2; sd++) {
        g_footFrameOk[sd] = false;
        if (g_foot[sd] < 0 || g_ball[sd] < 0) continue;
        const float* ftr = (const float*)(data + g_foot[sd] * 48);
        const float* bar = (const float*)(data + g_ball[sd] * 48);
        const V3 toe = sub(v3(bar[4], bar[5], bar[6]), v3(ftr[4], ftr[5], ftr[6]));
        if (len(toe) < 1e-3f) continue;
        const Q4 fq = q4(ftr[0], ftr[1], ftr[2], ftr[3]);
        g_toeLocal[sd]  = qinv(fq, norm(toe));          // where the toes point, in the foot's own space
        g_soleLocal[sd] = qinv(fq, mul(up, -1.0f));     // ...and the sole, which right now faces the floor
        g_footFrameOk[sd] = true;
    }
    {   // The eyes, in the head bone's own space, off the standing pose: forward of the head bone and
        // up from it. Measured there and carried in the bone's frame, they ride the head wherever a
        // style pitches it or a look turns it, on any rig.
        const int hb = g_head >= 0 ? g_head : g_neck;
        g_eyeOk = false;
        if (hb >= 0) {
            const float* hd = (const float*)(data + hb * 48);
            const Q4 hq = q4(hd[0], hd[1], hd[2], hd[3]);
            const V3 fwdM = norm(qinv(mq, fwd));
            g_eyeLocal = qinv(hq, add(mul(fwdM, g_eyeFwd), mul(up, g_eyeUp)));
            g_eyeOk = true;
        }
    }
    if (thigh < 15.0f || thigh > 90.0f) { TwkLog("[sit] thigh length %.1f cm -- not a rig we understand", thigh); return; }
    g_ankleH = clampf(ankle, 0.0f, 25.0f);
    const float inset = thigh * g_insetMul;
    Seat seat; memset(&seat, 0, sizeof(seat));
    bool have = FindDropAhead(world, P, fwd, floorZ, inset, &seat);
    const char* how = "on a ledge, facing the drop";
    if (!have) { have = FindLedgeAhead(world, P, fwd, floorZ, inset, &seat); how = "up onto the ledge ahead"; }
    if (!have) {
        if (!g_floor) { snprintf(g_last, sizeof(g_last), "nothing to sit on"); TwkLog("[sit] nothing to sit on here"); return; }
        seat.kind = M_FLOOR; seat.point = v3(P.x, P.y, floorZ); seat.facing = fwd; seat.groundDz = 0.0f; seat.height = 0.0f;
        how = "on the ground";
    }
    g_gridOk = false; g_raysOk = false;
    if (seat.kind == M_FLOOR) {   // the ledge legs probe for themselves; this is the floor's map
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
        const V3 fwdW = norm(v3(seat.facing.x, seat.facing.y, 0.0f));
        const V3 rightW = norm(cross(v3(0.0f, 0.0f, 1.0f), fwdW));
        int found = 0; float lo = 0.0f, hi = 0.0f;
        for (int i = 0; i < GRID_NF; i++) for (int j = 0; j < GRID_NR; j++) {
            const V3 c = add(add(seat.point, mul(fwdW, kGridF0 + kGridS * (float)i)), mul(rightW, kGridR0 + kGridS * (float)j));
            V3 hit;
            if (Trace(world, add(c, v3(0.0f, 0.0f, kSupportUp)), add(c, v3(0.0f, 0.0f, -kProbeDown)), &hit, nullptr)) {
                const float h = hit.z - seat.point.z;
                g_grid[i][j] = h; found++;
                if (h < lo) lo = h;
                if (h > hi) hi = h;
            } else g_grid[i][j] = 0.0f;
        }
        g_gridOk = found > 0;
        // the fan: what stands taller than a foot can rest on, and how far out it is on each bearing
        const V3 o = add(seat.point, v3(0.0f, 0.0f, kSupportUp));
        float nearest = 1e9f, nearAng = 0.0f;
        for (int k = 0; k < RAY_N; k++) {
            const float ang = (-kRaySpanDeg + 2.0f * kRaySpanDeg * (float)k / (float)(RAY_N - 1)) * 0.0174532925f;
            const V3 dir = add(mul(fwdW, cosf(ang)), mul(rightW, sinf(ang)));
            V3 hit;
            g_rayDist[k] = Trace(world, o, add(o, mul(dir, kRayReach)), &hit, nullptr) ? len(sub(hit, o)) : 1e9f;
            if (g_rayDist[k] < nearest) { nearest = g_rayDist[k]; nearAng = ang * 57.2957795f; }
        }
        g_raysOk = true;
        QueryPerformanceCounter(&t1);
        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        if (nearest < 1e8f)
            TwkLog("[sit] ground: %d of %d samples, %.0f..%.0f cm about the seat; nearest thing in the legs' way %.0f cm out at %.0f deg | %.1f ms",
                   found, GRID_NF * GRID_NR, lo, hi, nearest, nearAng, ms);
        else
            TwkLog("[sit] ground: %d of %d samples, %.0f..%.0f cm about the seat; nothing in the legs' way | %.1f ms",
                   found, GRID_NF * GRID_NR, lo, hi, ms);
    }
    g_seatW = seat.point; g_faceW = norm(v3(seat.facing.x, seat.facing.y, 0.0f));
    g_fcur = norm(qinv(mq, fwd));
    g_groundDz = seat.groundDz;
    SeatFrame(mesh);
    g_blendEff = (float)g_blendMs * (1.0f + fabsf(g_turnDeg) / 180.0f * 0.8f);   // a turn-around takes longer
    g_savedMode = (uint8_t)twkB(move, MOVE_MODE); g_savedCustom = (uint8_t)twkB(move, MOVE_MODE + 1);
    __try {
        g_setMode(move, 0, 0);                                   // MOVE_None: the capsule stays put
        float* vel = (float*)((uint8_t*)move + MOVE_VEL); vel[0] = vel[1] = vel[2] = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    g_rand ^= (unsigned)GetTickCount64() ^ (unsigned)(seat.point.x * 7.0f) ^ ((unsigned)(seat.point.y * 13.0f) << 8);
    RestBoard(sk, seat.point, seat.facing, seat.kind == M_LEDGE, seat.groundDz);
    g_mode = seat.kind; g_mesh = mesh; g_skater = sk; g_sitting = 1;
    if (g_debug) g_dumpFrames = 1;
    snprintf(g_last, sizeof(g_last), "sitting %s, %s (ledge %.0f cm)", how, StyleName(), seat.height);
    TwkLog("[sit] sitting %s: seat (%.0f %.0f %.0f) height %.0f cm, ground %.0f cm below the seat, thigh %.0f cm, ankle %.0f cm, "
           "turn %.0f deg, style %d (%s), movement %d/%d -> None", how, seat.point.x, seat.point.y, seat.point.z, seat.height,
           -seat.groundDz, thigh, g_ankleH, g_turnDeg, g_style, StyleName(), (int)g_savedMode, (int)g_savedCustom);
}

bool Sit_PoseHeld() { return g_mesh != nullptr; }

// The eyes in the world, off the mesh's FINISHED pose (the read buffer -- what is on screen).
static bool EyeWorld(void* mesh, V3* out) {
    if (!mesh || !g_eyeOk) return false;
    const int hb = g_head >= 0 ? g_head : g_neck;
    if (hb < 0) return false;
    Q4 mq, hq; V3 mp, ms, hp;
    __try {
        ReadC2W(mesh, &mq, &mp, &ms);
        const int ridx = *(const int*)((const uint8_t*)mesh + SKM_READ);
        const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + (ridx == 1 ? 0x10 : 0);
        const uint8_t* data = *(const uint8_t* const*)arr;
        const int n = *(const int*)(arr + 8);
        if (!data || n != g_nBones) return false;
        const float* t = (const float*)(data + hb * 48);
        hq = q4(t[0], t[1], t[2], t[3]); hp = v3(t[4], t[5], t[6]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
    *out = add(mp, qrot(mq, mulv(add(hp, qrot(hq, g_eyeLocal)), ms)));
    return true;
}
// A world direction as yaw (+ right) and pitch (+ up) off a facing.
static void AnglesTo(V3 dirW, V3 refW, float* yaw, float* pitch) {
    const V3 h = norm(v3(dirW.x, dirW.y, 0.0f)), r = norm(v3(refW.x, refW.y, 0.0f));
    *yaw = atan2f(r.x * h.y - r.y * h.x, r.x * h.x + r.y * h.y) * 57.2957795f;
    *pitch = atan2f(dirW.z, sqrtf(dirW.x * dirW.x + dirW.y * dirW.y)) * 57.2957795f;
}
// What a head can do with a wanted look. Inside the limit it is followed; past it, for the hold band,
// the head holds at the limit waiting for the thing to come back; further round than that it is out
// of sight and the head returns to the front. With hysteresis, so nothing flutters at the edge.
static void ClampLook(float yaw, float pitch, float* outYaw, float* outPitch) {
    const float ay = fabsf(yaw), lim = g_lookYawMax, edge = lim + g_holdBand;
    if (!g_lookLost && ay > edge) g_lookLost = true;
    else if (g_lookLost && ay < edge - 12.0f) g_lookLost = false;
    if (g_lookLost) { *outYaw = 0.0f; *outPitch = 0.0f; return; }
    *outYaw = ay <= lim ? yaw : (yaw > 0.0f ? lim : -lim);
    *outPitch = clampf(pitch, -g_lookDownMax, g_lookUpMax);
}

// ---- the players there are to watch, off SessionOpenMP (either mod may be installed alone)
static bool BindBridge() {
    if (g_proxyList && g_proxyIdx) return true;
    const uint64_t ms = GetTickCount64();
    if (ms - g_bridgeTryMs < 2000) return false;
    g_bridgeTryMs = ms;
    HMODULE mods[512]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    int n = (int)(needed / sizeof(HMODULE)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        auto l = (ProxyListFn)GetProcAddress(mods[i], "OmpSession_ProxyActors");
        if (!l) continue;
        g_proxyList = l;
        g_proxyIdx = (ProxyIdxFn)GetProcAddress(mods[i], "OmpSession_ProxyPeerIndex");
        g_proxyName = (ProxyNameFn)GetProcAddress(mods[i], "OmpSession_ProxyPeerName");
        g_setOwnHead = (SetHeadFn)GetProcAddress(mods[i], "OmpSession_SetOwnHeadLook");
        g_proxyHead  = (ProxyHeadFn)GetProcAddress(mods[i], "OmpSession_ProxyHeadLook");
        if (!g_proxyIdx) { g_proxyList = nullptr; if (!g_bridgeLogged) { g_bridgeLogged = true; TwkLog("[sit] SessionOpenMP found but without OmpSession_ProxyPeerIndex (older build) -- nobody to watch"); } return false; }
        if (!g_bridgeLogged) { g_bridgeLogged = true; TwkLog("[sit] SessionOpenMP bridge bound -- other players can be watched from a seat"); }
        return true;
    }
    return false;
}
static int CmpPeer(const void* a, const void* b) { return ((const Peer*)a)->idx - ((const Peer*)b)->idx; }
// The list, refreshed each frame while it matters: positions, and how fast each is going.
static void RefreshPeers(float dt) {
    if (!BindBridge()) { g_nPeers = 0; return; }
    void* acts[16]; int n = 0;
    __try { n = g_proxyList(acts, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < g_nPeers; i++) g_peers[i].fresh = false;
    Peer next[16]; int m = 0;
    for (int i = 0; i < n && m < 16; i++) {
        void* a = acts[i];
        if (!a) continue;
        V3 pos; int idx = -1; char nm[40] = "";
        __try {
            void* root = twkP(a, ACT_ROOT);
            if (!root) continue;
            Q4 rq; ReadC2W(root, &rq, &pos, nullptr);
            idx = g_proxyIdx(a);
            if (g_proxyName && !g_proxyName(a, nm, sizeof(nm))) continue;   // away: hidden somewhere else, nothing to watch
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!(fabsf(pos.x) < 1e7f && fabsf(pos.y) < 1e7f && fabsf(pos.z) < 1e7f)) continue;
        Peer pr; pr.actor = a; pr.idx = idx; pr.pos = pos; pr.speed = 0.0f; pr.stillS = 0.0f; pr.fresh = true;
        strncpy_s(pr.name, sizeof(pr.name), nm, _TRUNCATE);
        for (int k = 0; k < g_nPeers; k++) if (g_peers[k].actor == a) {   // carry the motion history
            const float v = dt > 1e-4f ? len(sub(pos, g_peers[k].pos)) / dt : g_peers[k].speed;
            pr.speed = g_peers[k].speed + (fminf(v, 3000.0f) - g_peers[k].speed) * 0.2f;
            pr.stillS = pr.speed < 40.0f ? g_peers[k].stillS + dt : 0.0f;
            break;
        }
        next[m++] = pr;
    }
    qsort(next, (size_t)m, sizeof(Peer), CmpPeer);
    for (int i = 0; i < m; i++) g_peers[i] = next[i];
    g_nPeers = m;
}
// What to call someone: the skater name they chose, else their number.
static const char* PeerLabel(const Peer* p, char* buf, size_t cap) {
    if (p && p->name[0]) snprintf(buf, cap, "%s", p->name);
    else snprintf(buf, cap, "player %d", p ? p->idx + 1 : 0);
    return buf;
}
static const Peer* PeerByIdx(int idx) { for (int i = 0; i < g_nPeers; i++) if (g_peers[i].idx == idx) return &g_peers[i]; return nullptr; }
static const Peer* PeerByActor(void* a) { for (int i = 0; i < g_nPeers; i++) if (g_peers[i].actor == a) return &g_peers[i]; return nullptr; }
// Whoever is skating: attention goes to the nearer and the faster, stays a while, drifts off someone
// who has stopped, and leaves anyone who goes out of range. Evaluated a few times a second.
static const Peer* AutoWatch(float dt) {
    g_attnClock += dt; g_dwell -= dt;
    const Peer* cur = PeerByActor(g_curWatch);
    if (cur && len(sub(cur->pos, g_eyeW)) > g_watchRange) cur = nullptr;
    if (g_attnClock < 0.25f && cur) return cur;
    g_attnClock = 0.0f;
    bool moving = false;
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].actor != g_curWatch && len(sub(g_peers[i].pos, g_eyeW)) <= g_watchRange && g_peers[i].stillS < 2.0f) moving = true;
    const bool bored = cur && cur->stillS > 4.0f && moving;
    if (cur && g_dwell > 0.0f && !bored) return cur;
    const Peer* best = nullptr; float bestScore = -1.0f;
    for (int i = 0; i < g_nPeers; i++) {
        const Peer& pr = g_peers[i];
        const float d = len(sub(pr.pos, g_eyeW));
        if (d > g_watchRange) continue;
        if (pr.actor == g_curWatch && g_nPeers > 1 && (bored || g_dwell <= 0.0f)) continue;   // look at someone else
        const float score = (1.0f + fminf(pr.speed / 500.0f, 2.0f)) / (1.0f + d / 800.0f) * (pr.stillS > 4.0f ? 0.4f : 1.0f);
        if (score > bestScore) { bestScore = score; best = &pr; }
    }
    if (!best) best = cur;
    if (best && best->actor != g_curWatch) {
        g_curWatch = best->actor;
        g_dwell = 7.0f + Rand01() * 9.0f;
        char lb[48]; TwkLog("[sit] watching %s (%.0f m off, %s)", PeerLabel(best, lb, sizeof(lb)), len(sub(best->pos, g_eyeW)) * 0.01f, best->stillS > 2.0f ? "standing about" : "skating");
    } else if (best && g_dwell <= 0.0f) g_dwell = 7.0f + Rand01() * 9.0f;
    if (!best) g_curWatch = nullptr;
    return best;
}
static const char* WatchLabel(char* buf, size_t cap) {
    char lb[48];
    if (g_watch == WATCH_FREE) snprintf(buf, cap, "Watch: free look");
    else if (g_watch == WATCH_AUTO) {
        const Peer* c = PeerByActor(g_curWatch);
        if (c) snprintf(buf, cap, "Watch: whoever is skating (%s)", PeerLabel(c, lb, sizeof(lb)));
        else snprintf(buf, cap, "Watch: whoever is skating (%s)", g_nPeers ? "nobody near" : "nobody here");
    } else {
        const Peer* c = PeerByIdx(g_watchPeerIdx);
        if (c) snprintf(buf, cap, "Watch: %s%s", PeerLabel(c, lb, sizeof(lb)), g_watchInRange ? "" : " (too far)");
        else snprintf(buf, cap, "Watch: player %d (gone)", g_watchPeerIdx + 1);
    }
    return buf;
}
// The d-pad's choices, in order: free look, whoever is skating, then every player there is.
static void CycleWatch(int dir) {
    const int count = 2 + g_nPeers;
    int cur = g_watch == WATCH_FREE ? 0 : g_watch == WATCH_AUTO ? 1 : -1;
    if (cur < 0) { cur = 1; for (int i = 0; i < g_nPeers; i++) if (g_peers[i].idx == g_watchPeerIdx) cur = 2 + i; }
    cur = (cur + dir + count) % count;
    if (cur == 0) { g_watch = WATCH_FREE; g_watchPeerIdx = -1; }
    else if (cur == 1) { g_watch = WATCH_AUTO; g_watchPeerIdx = -1; g_curWatch = nullptr; g_dwell = 0.0f; }
    else { g_watch = WATCH_PEER; g_watchPeerIdx = g_peers[cur - 2].idx; }
    g_lookLost = false;
    char b[96]; TwkLog("[sit] %s", WatchLabel(b, sizeof(b)));
}
// Somebody skated into you. Stand up first -- the seat pose drives the skeleton every frame and would
// fight the ragdoll for it -- then let the GAME bail us: its own Bail means the ragdoll, the sound and
// the score-run reset all behave exactly as they do for any other bail, and the co-op lane carries it
// to everyone else for free (a real local bail is already replicated).
static void BailFromHit(const Peer* by) {
    void* sk = g_skater;
    char lb[48];
    TwkLog("[sit] %s skated into us -- bailing", by ? PeerLabel(by, lb, sizeof(lb)) : "someone");
    StandUp("knocked over");
    if (!g_bail || !sk) return;
    __try {
        if (!twkP(sk, SK_BOARD)) return;        // the location fallback derefs the board link
        static wchar_t reasonChars[] = L"Knocked over while sitting";
        const struct { const wchar_t* d; int n; int max; } reason =
            { reasonChars, (int)(sizeof(reasonChars) / 2), (int)(sizeof(reasonChars) / 2) };
        const uint8_t was = twkB(sk, SK_CAN_BAIL);
        *((uint8_t*)sk + SK_CAN_BAIL) = 1;      // lifted for exactly this call, as the host does
        g_bail(sk, &reason, true, true);        // (reason, 1, 1): the in-game pattern, the last 1 = ragdoll
        *((uint8_t*)sk + SK_CAN_BAIL) = was;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++; g_bailOnHit = 0;
        TwkLog("[sit] fault bailing -- knock-overs off for this run");
    }
}
// Anyone close enough, and moving fast enough, to have run into us. Their capsule against ours: the
// actor roots within a capsule's width horizontally and a body's height vertically. Someone standing
// next to you is not a hit, which is what the speed is for.
static const Peer* WhoHitUs() {
    if (!g_bailOnHit || !g_skater) return nullptr;
    V3 me;
    __try {
        void* root = twkP(g_skater, ACT_ROOT);
        if (!root) return nullptr;
        Q4 rq; ReadC2W(root, &rq, &me, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return nullptr; }
    for (int i = 0; i < g_nPeers; i++) {
        const Peer& pr = g_peers[i];
        if (pr.speed < g_hitSpeed) continue;
        const V3 d = sub(pr.pos, me);
        if (fabsf(d.z) > g_hitRise) continue;
        if (len(v3(d.x, d.y, 0.0f)) > g_hitRadius) continue;
        return &pr;
    }
    return nullptr;
}


// The first-person view, for the camera module: the eyes in the world off the mesh's FINISHED pose
// (the read buffer -- what is on screen), the look as a world rotation, the dolly weight, the FOV.
bool Sit_FirstPersonView(float eye[3], float look[4], float* weight, float* fov) {
    if (!g_mesh || g_fpBlend <= 0.0f) return false;
    V3 ew;
    if (!EyeWorld(g_mesh, &ew)) return false;
    eye[0] = ew.x; eye[1] = ew.y; eye[2] = ew.z;
    // the look: the seat's facing turned by the yaw, then pitched about the turned right axis (a
    // camera looks down its +X; a negative turn about +Y lifts it)
    const float yaw = atan2f(g_faceW.y, g_faceW.x) * 57.2957795f + g_lookYawS;
    const Q4 q = qmul(qaxis(v3(0.0f, 0.0f, 1.0f), yaw), qaxis(v3(0.0f, 1.0f, 0.0f), -g_lookPitchS));
    look[0] = q.x; look[1] = q.y; look[2] = q.z; look[3] = q.w;
    *weight = g_fpBlend * g_fpBlend * (3.0f - 2.0f * g_fpBlend);
    *fov = g_fpFov;
    return true;
}

void Sit_PumpFrame() {
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    float dt = last ? (float)(now - last) * 0.001f : 0.016f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    void* sk = CatchTweaks_Skater();
    const bool onFoot = OnFoot(sk);
    // the skater's facing, and the mesh the off-board head look rides (its rig, and its head's axes)
    if (sk) {
        __try {
            void* root = twkP(sk, ACT_ROOT);
            if (root) { Q4 rq; V3 rp; ReadC2W(root, &rq, &rp, nullptr); V3 f = qrot(rq, v3(1.0f, 0.0f, 0.0f)); f.z = 0.0f; if (len(f) > 1e-3f) g_bodyFwdW = norm(f); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    if (sk) {   // the mount's signature, for an exact gate later: movement mode + the anim transition bytes, on change
        __try {
            void* move = twkP(sk, CH_MOVE); void* ai = FootPlace_AnimInstance();
            if (move && ai) {
                const int mm = twkB(move, MOVE_MODE), cm = twkB(move, MOVE_MODE + 1), f2b = twkB(ai, AN_FOOT2BOARD), b2f = twkB(ai, AN_FOOT2BOARD + 1), ob = twkB(ai, AN_ON_BOARD);
                static int lastSig = -1;
                const int sig = (mm & 15) | (cm & 15) << 4 | (f2b & 15) << 8 | (b2f & 15) << 12 | (ob & 1) << 16;
                if (sig != lastSig) { lastSig = sig; TwkLog("[sit] state: movement %d/%d, foot->board %d, board->foot %d, onBoard %d", mm, cm, f2b, b2f, ob); }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    if (onFoot && sk && g_headLook && !InReplay()) {
        void* mesh = twkP(sk, CH_MESH);
        if (mesh && mesh != g_walkSeen) {
            g_walkSeen = mesh;
            if (!ResolveRig(mesh)) mesh = nullptr;
        }
        if (mesh && g_rigOk) g_walkMesh = mesh;
    } else if (g_walkW <= 0.0f) g_walkMesh = nullptr;
    if (g_sitting) {
        void* move = g_skater ? twkP(g_skater, CH_MOVE) : nullptr;
        if (!onFoot) StandUp("back on the board");
        else if (sk != g_skater) StandUp("skater changed");
        else if (move && twkB(move, MOVE_MODE) != 0) StandUp("the game took the movement back");
    }
    // hold the key down to stand: fires while it is still held, like the menu's hold-to-confirm
    if (g_pressDown && g_sitting && !g_holdFired && !g_pressConsumed) {
        LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        if ((t.QuadPart - g_pressQpc) * 1000 > f.QuadPart * (LONGLONG)g_holdMs) {
            g_holdFired = 1;
            InterlockedExchange(&g_req, REQ_STAND);
        }
    }
    switch (InterlockedExchange(&g_req, REQ_NONE)) {
    case REQ_SIT:   if (g_on && g_ok && onFoot && !g_sitting) TrySit(sk); break;
    case REQ_STAND: StandUp("the sit key, held"); break;
    case REQ_CYCLE:
        if (g_sitting) {
            g_prevStyle = g_style;
            g_style = (g_style + 1) & (STYLE_COUNT - 1);
            g_styleBlend = 0.0f;
            snprintf(g_last, sizeof(g_last), "sitting %s", StyleName());
            TwkLog("[sit] style %d: %s", g_style, StyleName());
        }
        break;
    default: break;
    }
    // the view key: first person on or off, while seated
    if (InterlockedExchange(&g_reqView, 0) && g_sitting && g_fpOn) {
        g_fp = !g_fp;
        TwkLog("[sit] %s", g_fp ? "first person: the camera goes to the eyes" : "third person: the camera comes back");
    }
    // the d-pad: the next or the previous thing to watch
    { const LONG dw = InterlockedExchange(&g_reqWatch, 0); if (dw && g_sitting && g_fp) CycleWatch(dw > 0 ? 1 : -1); }
    {   // Where the head looks, and from what. Seated in first person: the stick, or the watched
        // player; seated in third person, or walking: the camera. A stick is integrated (a rate for a
        // head, not a place); the other two want a direction and get it through the clamp.
        int src = LOOK_NONE; float rate = 18.0f;
        if (g_sitting || g_mesh) {
            if (g_fp && g_sitting && g_watch != WATCH_FREE) src = LOOK_WATCH;
            else if (g_fp && g_sitting) src = LOOK_STICK;
            else if (g_headLook) src = LOOK_CAMERA;
        } else if (onFoot && sk && g_headLook && g_walkMesh) src = LOOK_CAMERA;
        if (src != g_lookSrc) { g_lookSrc = src; g_lookLost = false; }
        {   // the eyes this frame, for the watch: the seat's, else about head height off the skater
            V3 e;
            if (g_mesh && EyeWorld(g_mesh, &e)) g_eyeW = e;
            else if (sk) { __try { void* root = twkP(sk, ACT_ROOT); if (root) { Q4 rq; V3 rp; ReadC2W(root, &rq, &rp, nullptr); g_eyeW = add(rp, v3(0.0f, 0.0f, 60.0f)); } } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; } }
        }
        if (g_sitting) RefreshPeers(dt); else g_nPeers = 0;
        // ...and one of them may have just run us over. After the refresh (their speed is this frame's)
        // and before the look, which the stand-up makes moot anyway.
        if (g_sitting && g_alpha > 0.5f) { const Peer* hit = WhoHitUs(); if (hit) BailFromHit(hit); }
        if (src == LOOK_STICK) {
            float sx = 0.0f, sy = 0.0f;
            if (ScoopSpeed_StickRaw(true, &sx, &sy)) {
                if (g_lookInvert & 1) sx = -sx;
                if (g_lookInvert & 2) sy = -sy;
                const float dz = 0.18f;
                const float ax = fabsf(sx) > dz ? (sx - (sx > 0.0f ? dz : -dz)) / (1.0f - dz) : 0.0f;
                const float ay = fabsf(sy) > dz ? (sy - (sy > 0.0f ? dz : -dz)) / (1.0f - dz) : 0.0f;
                g_lookYaw   = clampf(g_lookYaw   + ax * g_lookSpeed * dt, -g_lookYawMax, g_lookYawMax);
                g_lookPitch = clampf(g_lookPitch + ay * g_lookSpeed * dt, -g_lookDownMax, g_lookUpMax);
            }
        } else if (src == LOOK_CAMERA) {
            float f[3];
            if (CameraHeight_ViewForward(f)) {
                float y, pch; AnglesTo(v3(f[0], f[1], f[2]), g_sitting || g_mesh ? g_faceW : g_bodyFwdW, &y, &pch);
                ClampLook(y, pch, &g_lookYaw, &g_lookPitch);
            }
            rate = g_camLookRate;
        } else if (src == LOOK_WATCH) {
            const Peer* tgt = nullptr;
            if (g_watch == WATCH_AUTO) tgt = AutoWatch(dt);
            else { tgt = PeerByIdx(g_watchPeerIdx); if (!tgt && g_nPeers == 0) {} else if (!tgt) { g_watch = WATCH_AUTO; g_watchPeerIdx = -1; TwkLog("[sit] that player left -- watching whoever is skating"); } }
            const bool inRange = tgt && len(sub(tgt->pos, g_eyeW)) <= g_watchRange;
            if (inRange != g_watchInRange && g_watch == WATCH_PEER) { char lb[48]; TwkLog("[sit] %s %s", PeerLabel(tgt, lb, sizeof(lb)), inRange ? "in range -- following" : "too far off -- not followed"); }
            g_watchInRange = inRange;
            if (inRange) {
                float y, pch; AnglesTo(sub(add(tgt->pos, v3(0.0f, 0.0f, 55.0f)), g_eyeW), g_faceW, &y, &pch);
                ClampLook(y, pch, &g_lookYaw, &g_lookPitch);
                g_outOfReachS = g_lookLost ? g_outOfReachS + dt : 0.0f;
                if (g_lookLost && g_watch == WATCH_AUTO && g_outOfReachS > 2.5f) { g_dwell = 0.0f; g_outOfReachS = 0.0f; }   // out of sight: look for someone else
            } else { g_lookYaw = 0.0f; g_lookPitch = 0.0f; }
            rate = 6.0f;
        } else { g_lookYaw = 0.0f; g_lookPitch = 0.0f; }
        const float k = 1.0f - expf(-dt * rate);
        g_lookYawS   += (g_lookYaw   - g_lookYawS)   * k;
        g_lookPitchS += (g_lookPitch - g_lookPitchS) * k;
        const float wt = (onFoot && sk && g_headLook && g_walkMesh) ? 1.0f : 0.0f;
        const float ws = dt / 0.3f;
        if (g_walkW < wt) { g_walkW += ws; if (g_walkW > wt) g_walkW = wt; }
        else if (g_walkW > wt) { g_walkW -= ws; if (g_walkW < wt) g_walkW = wt; }
    }
    // Our head look onto the wire: what the walk path applies this frame, and NOTHING while a held
    // pose carries it (the seat), so the other end never turns a head twice. Pushed on change.
    if (BindBridge() && g_setOwnHead) {
        float py = 0.0f, pp = 0.0f;
        if (!g_mesh && g_walkMesh && g_walkW > 0.0f) { py = g_lookYawS; pp = g_lookPitchS; }
        static int lastY = 0, lastP = 0;
        const int qy = (int)lroundf(py), qp = (int)lroundf(pp);
        if (qy != lastY || qp != lastP) { lastY = qy; lastP = qp; __try { g_setOwnHead(py, pp); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; } }
    }
    // ...and theirs off it: each proxy's look, its facing and its rig, for the flip to apply. Lightly
    // smoothed here so the packet-rate steps do not show.
    {
        PxHead prev[16]; const int nPrev = g_nPx; memcpy(prev, g_px, sizeof(PxHead) * (size_t)(nPrev > 0 ? nPrev : 0));
        g_nPx = 0;
        if (g_headLook && g_proxyHead && g_proxyList && !InReplay()) {
            void* acts[16]; int n = 0;
            __try { n = g_proxyList(acts, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
            const float k = 1.0f - expf(-dt * 20.0f);
            for (int i = 0; i < n && g_nPx < 16; i++) {
                float y = 0.0f, pch = 0.0f;
                __try {
                    const int st = acts[i] ? g_proxyHead(acts[i], &y, &pch) : 0;   // 1 off the board, 2 riding
                    if (!st) continue;
                    void* mesh = twkP(acts[i], CH_MESH); void* root = twkP(acts[i], ACT_ROOT);
                    if (!mesh || !root) continue;
                    Q4 rq; V3 rp; ReadC2W(root, &rq, &rp, nullptr);
                    V3 f = qrot(rq, v3(1.0f, 0.0f, 0.0f)); f.z = 0.0f;
                    if (len(f) < 1e-3f) continue;
                    const PxRig* rig = PxRigFor(mesh);
                    if (!rig) continue;
                    PxHead& h = g_px[g_nPx];
                    h.mesh = mesh; h.bodyFwdW = norm(f); h.rig = rig; h.yaw = y; h.pitch = pch; h.w = 0.0f;
                    const float wt = st == 1 ? 1.0f : 0.0f;
                    for (int j = 0; j < nPrev; j++) if (prev[j].mesh == mesh) {
                        h.yaw = prev[j].yaw + (y - prev[j].yaw) * k; h.pitch = prev[j].pitch + (pch - prev[j].pitch) * k;
                        h.w = prev[j].w; break;
                    }
                    const float ws = dt / 0.3f;
                    if (h.w < wt) { h.w += ws; if (h.w > wt) h.w = wt; } else if (h.w > wt) { h.w -= ws; if (h.w < wt) h.w = wt; }
                    if (h.w <= 0.0f) continue;   // riding, and faded: nothing to turn
                    g_nPx++;
                } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
            }
        }
        const float vt = (g_fp && g_sitting) ? 1.0f : 0.0f;
        const float vs = dt / ((g_fpViewMs > 50 ? (float)g_fpViewMs : 50.0f) * 0.001f);
        if (g_fpBlend < vt) { g_fpBlend += vs; if (g_fpBlend > vt) g_fpBlend = vt; }
        else if (g_fpBlend > vt) { g_fpBlend -= vs; if (g_fpBlend < vt) g_fpBlend = vt; }
    }
    if (g_styleBlend < 1.0f) {
        g_styleBlend += dt / ((float)(g_styleMs > 50 ? g_styleMs : 50) * 0.001f);
        if (g_styleBlend >= 1.0f) { g_styleBlend = 1.0f; g_prevStyle = g_style; }
    }
    const float Ts = (g_blendEff > 50.0f ? g_blendEff : 50.0f) * 0.001f;
    const float target = g_sitting ? 1.0f : 0.0f;
    g_now += dt;
    if (g_realMove) {
        // A spring, not a ramp. The target is where the body is going, the stiffness gets it there in
        // about the blend time, and the damping is a little short of critical, so it arrives with a
        // small give past the pose and settles back -- weight landing on a seat. Stepped at 240 Hz
        // whatever the frame rate, so a hitch cannot fling it.
        const float w = 3.4f / Ts, z = clampf(g_settle, 0.3f, 1.2f);
        const int nsub = dt > 0.0042f ? (int)ceilf(dt / 0.0042f) : 1;
        const float h = dt / (float)nsub;
        for (int k = 0; k < nsub; k++) {
            g_avel += ((target - g_alpha) * w * w - 2.0f * z * w * g_avel) * h;
            g_alpha += g_avel * h;
            if (!g_sitting && g_alpha <= 0.0f) { g_alpha = 0.0f; g_avel = 0.0f; break; }   // stood: done, no bounce
        }
        if (g_alpha > 1.05f) g_alpha = 1.05f;
        HistPush(g_now, g_alpha);
        const float* lag = g_sitting ? kLagDown : kLagUp;
        g_anyA = 0.0f;
        for (int p = 0; p < PART_N; p++) {
            g_partA[p] = HistAt(g_now - lag[p] * g_stagger * Ts);
            if (g_partA[p] > g_anyA) g_anyA = g_partA[p];
        }
    } else {
        const float step = dt / Ts;
        if (g_alpha < target) { g_alpha += step; if (g_alpha > target) g_alpha = target; }
        else if (g_alpha > target) { g_alpha -= step; if (g_alpha < target) g_alpha = target; }
        const float a = g_alpha * g_alpha * (3.0f - 2.0f * g_alpha);
        for (int p = 0; p < PART_N; p++) g_partA[p] = a;
        g_anyA = a;
    }
    if (!g_sitting && g_alpha <= 0.0f && g_anyA <= 0.0005f && g_fpBlend <= 0.0f && g_mesh) {   // the camera is back too
        g_mesh = nullptr; g_skater = nullptr; g_mode = M_NONE; HistReset(0.0f);
    }
    {   // the prompt: on while seated, and its ring fills as the key is held for the stand-up
        float held = 0.0f;
        if (g_pressDown && g_sitting && !g_pressConsumed && !g_holdFired) {
            LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
            const float ms = (float)((double)(t.QuadPart - g_pressQpc) * 1000.0 / (double)f.QuadPart);
            held = clampf(ms / (float)(g_holdMs > 50 ? g_holdMs : 50), 0.0f, 1.0f);
        }
        // the bar's entries, top of the list at the right: the tap, then the hold with its ring
        char wl[96]; WatchLabel(wl, sizeof(wl));
        SitPromptEntry entries[4]; int ne = 0;
        entries[ne++] = { "Change how you sit", 'B', 0.0f };
        if (g_fpOn && g_fp) entries[ne++] = { wl, 'R', 0.0f };
        if (g_fpOn) entries[ne++] = { "First person view", 'A', 0.0f };
        entries[ne++] = { "Hold to stand up", 'B', held };
        SitUI_PumpFrame(sk, g_on && g_ok && g_sitting && !InReplay(), entries, ne);
    }
    // The board's movement mode, whenever it changes. This is how the value the GAME uses when it drops
    // a board of its own gets named: do that once and the number is in the log.
    if (sk) {
        __try {
            void* bd = twkP(sk, SK_BOARD);
            void* mv = bd ? twkP(bd, BD_MOVECOMP) : nullptr;
            if (mv) {
                static int last = -1;
                const int now = twkB(mv, BM_MODE);
                if (now != last) {
                    TwkLog("[sit] board movement mode %d -> %d%s", last, now, now == 9 ? " (in hand: the game mutes its audio here)" : "");
                    last = now;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    // Did the board stay where it was put, or did the game take it back? One line, half a second in.
    if (g_boardWatch && g_boardResting && g_boardComp) {
        static uint64_t when = 0;
        if (g_boardWatch == 1) { when = GetTickCount64() + 600; g_boardWatch = 2; }
        else if (GetTickCount64() > when) {
            g_boardWatch = 0;
            __try {
                const float* w = (const float*)((const uint8_t*)g_boardComp + SC_C2W);
                void* par = twkP(g_boardComp, SC_ATTACH_PARENT);
                const V3 now = v3(w[4], w[5], w[6]);
                TwkLog("[sit] board half a second on: at (%.0f %.0f %.0f), %.0f cm from where it was put, %s",
                       now.x, now.y, now.z, len(sub(now, g_boardPut)), par ? "RE-ATTACHED by the game" : "still loose");
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
        }
    }
}

// ------------------------------------------------------------------ the key
// Which of ours a key is: 1 the sit key, 2 the view key, 0 neither. A name is resolved once per FName.
static uint64_t g_dpadL = 0, g_dpadR = 0, g_keyTop = 0, g_keyLeft = 0;
static int KeyKind(const void* key) {
    const uint64_t nm = *(const uint64_t*)key;
    if (!nm) return 0;
    if (nm == g_keyFName) return 1;
    if (nm == g_fpKeyFName) return 2;
    if (nm == g_dpadL) return 3;
    if (nm == g_dpadR) return 4;
    if (nm == g_keyTop) return 5;
    if (nm == g_keyLeft) return 6;
    for (int i = 0; i < g_notKeyN; i++) if (nm == g_notKey[i]) return 0;
    char nb[96];
    if (!GrindPop_FNameToString(key, nb, sizeof(nb))) return 0;
    if (strcmp(nb, g_keyName) == 0)   { g_keyFName = nm;   return 1; }
    if (strcmp(nb, g_fpKeyName) == 0) { g_fpKeyFName = nm; return 2; }
    if (strcmp(nb, "Gamepad_DPad_Left") == 0)  { g_dpadL = nm; return 3; }
    if (strcmp(nb, "Gamepad_DPad_Right") == 0) { g_dpadR = nm; return 4; }
    if (strcmp(nb, "Gamepad_FaceButton_Top") == 0)  { g_keyTop = nm; return 5; }
    if (strcmp(nb, "Gamepad_FaceButton_Left") == 0) { g_keyLeft = nm; return 6; }
    if (g_notKeyN < (int)(sizeof(g_notKey) / sizeof(g_notKey[0]))) g_notKey[g_notKeyN++] = nm;
    return 0;
}
bool Sit_OnInputKey(const void* key, int ev) {
    if (!g_on || !g_ok || !key || (ev != 0 && ev != 1)) return false;
    const int kind = KeyKind(key);
    if (!kind) return false;
    if (InReplay()) return false;                                       // the editor's exit button
    if (kind == 2) {                                                    // the view key: ours only while seated
        if (!g_sitting || !g_fpOn) return false;
        if (ev == 0) InterlockedExchange(&g_reqView, 1);
        return true;
    }
    if (kind == 3 || kind == 4) {                                       // the d-pad: ours only at the eyes
        if (!g_sitting || !g_fpOn || !g_fp) return false;
        if (ev == 0) InterlockedExchange(&g_reqWatch, kind == 4 ? 1 : -1);
        return true;
    }
    if (kind == 5 || kind == 6) {
        // Y (get on the board) and X (jump) while SEATED are swallowed: either one drove the game
        // into a state the seat cannot follow (skating while sitting, stuck). On foot, a Y is the
        // game's own mount request, and a sit must not start under the mount animation -- the same
        // stuck state from the other side -- so it holds the sit key off for the length of one.
        if (g_sitting) return true;
        if (kind == 5 && ev == 0 && OnFoot(CatchTweaks_Skater())) g_mountUntilMs = GetTickCount64() + 1200;
        return false;
    }
    if (ev == 0) {
        if (!g_sitting && !OnFoot(CatchTweaks_Skater())) return false;   // on the board: not ours
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        g_pressQpc = t.QuadPart; g_pressDown = 1; g_holdFired = 0;
        // Sitting down happens on the press, so it feels immediate; that press must not then also
        // count as a tap-to-cycle or start the hold-to-stand timer against the sit it just began.
        g_pressConsumed = !g_sitting;
        if (g_pressConsumed) InterlockedExchange(&g_req, REQ_SIT);
        g_swallowRelease = 1;
        return true;
    }
    g_pressDown = 0;
    if (g_swallowRelease) {
        g_swallowRelease = 0;
        if (g_sitting && !g_pressConsumed && !g_holdFired) InterlockedExchange(&g_req, REQ_CYCLE);
        g_pressConsumed = 0;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ config / menu / install
void Sit_ReadConfig(const char* buf) {
    g_on       = TwkIniInt(buf, "SitEnabled", 1) ? 1 : 0;
    g_floor    = TwkIniInt(buf, "SitFloor", 1) ? 1 : 0;
    g_maxLedge = (float)TwkIniInt(buf, "SitMaxLedgeCm", 120);
    g_minLedge = (float)TwkIniInt(buf, "SitMinLedgeCm", 22);
    g_reach    = (float)TwkIniInt(buf, "SitReachCm", 110);
    g_lean     = (float)TwkIniInt(buf, "SitLeanDeg", 0);
    g_channel  = TwkIniInt(buf, "SitTraceChannel", 3);
    g_shoulderDrop = (float)TwkIniInt(buf, "SitShoulderDropPct", 12) * 0.01f;
    g_shoulderFollow = (float)TwkIniInt(buf, "SitShoulderFollowPct", 32) * 0.01f;
    g_palmFlip[0] = TwkIniInt(buf, "SitPalmFlipLeft", 1) ? 1 : 0;
    g_palmFlip[1] = TwkIniInt(buf, "SitPalmFlipRight", 0) ? 1 : 0;
    g_blendMs  = TwkIniInt(buf, "SitBlendMs", 450);
    g_holdMs   = TwkIniInt(buf, "SitHoldMs", 320);
    g_styleMs  = TwkIniInt(buf, "SitStyleBlendMs", 260);
    g_debug    = TwkIniInt(buf, "SitDebug", 0) ? 1 : 0;
    g_boardRest = TwkIniInt(buf, "SitBoardRest", 1) ? 1 : 0;
    g_boardOut  = (float)TwkIniInt(buf, "SitBoardOutCm", 46);
    g_boardDrop = (float)TwkIniInt(buf, "SitBoardDropCm", 9);
    g_boardAhead = (float)TwkIniInt(buf, "SitBoardAheadCm", 62);
    g_pelvisSeat  = (float)TwkIniInt(buf, "SitPelvisSeatCm", 9);
    g_pelvisFloor = (float)TwkIniInt(buf, "SitPelvisFloorCm", 11);
    g_insetMul = (float)TwkIniInt(buf, "SitInsetPct", 90) * 0.01f;
    g_realMove = TwkIniInt(buf, "SitRealMove", 1) ? 1 : 0;
    g_settle   = (float)TwkIniInt(buf, "SitSettlePct", 72) * 0.01f;
    g_stagger  = (float)TwkIniInt(buf, "SitStaggerPct", 100) * 0.01f;
    g_footRadius = (float)TwkIniInt(buf, "SitFootRadiusCm", 7);
    g_toeRoom  = (float)TwkIniInt(buf, "SitToeRoomCm", 16);
    TwkIniStr(buf, "SitKey", g_keyName, sizeof(g_keyName), "Gamepad_FaceButton_Right");
    TwkIniStr(buf, "SitFirstPersonKey", g_fpKeyName, sizeof(g_fpKeyName), "Gamepad_FaceButton_Bottom");
    g_fpOn        = TwkIniInt(buf, "SitFirstPerson", 1) ? 1 : 0;
    g_lookYawMax  = (float)TwkIniInt(buf, "SitLookYawDeg", 80);
    g_lookUpMax   = (float)TwkIniInt(buf, "SitLookUpDeg", 45);
    g_lookDownMax = (float)TwkIniInt(buf, "SitLookDownDeg", 60);
    g_lookSpeed   = (float)TwkIniInt(buf, "SitLookSpeedDps", 150);
    g_lookInvert  = TwkIniInt(buf, "SitLookInvert", 0);
    g_fpFov       = (float)TwkIniInt(buf, "SitFirstPersonFov", 0);
    g_eyeUp       = (float)TwkIniInt(buf, "SitEyeUpCm", 7);
    g_eyeFwd      = (float)TwkIniInt(buf, "SitEyeFwdCm", 9);
    g_fpViewMs    = TwkIniInt(buf, "SitViewBlendMs", 350);
    g_hideHead    = TwkIniInt(buf, "SitHideHead", 1) ? 1 : 0;
    g_headLook    = TwkIniInt(buf, "HeadFollowsCamera", 1) ? 1 : 0;
    g_holdBand    = (float)TwkIniInt(buf, "HeadLookHoldDeg", 35);
    g_camLookRate = (float)TwkIniInt(buf, "HeadLookRate", 8);
    g_watchRange  = (float)TwkIniInt(buf, "SitWatchRangeCm", 3000);
    g_bailOnHit   = TwkIniInt(buf, "SitBailOnHit", 1) ? 1 : 0;
    g_hitRadius   = (float)TwkIniInt(buf, "SitHitRadiusCm", 75);
    g_hitRise     = (float)TwkIniInt(buf, "SitHitRiseCm", 130);
    g_hitSpeed    = (float)TwkIniInt(buf, "SitHitSpeedCm", 140);
    SitUI_ReadConfig(buf);
    g_keyFName = 0; g_fpKeyFName = 0; g_dpadL = 0; g_dpadR = 0; g_keyTop = 0; g_keyLeft = 0; g_notKeyN = 0;
    g_maxLedge = clampf(g_maxLedge, 30.0f, 200.0f); g_reach = clampf(g_reach, 40.0f, 200.0f); g_lean = clampf(g_lean, -25.0f, 25.0f);
}
void Sit_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "SitEnabled",       g_on);
    TwkIniSetInt(buf, cap, "SitFloor",         g_floor);
    TwkIniSetInt(buf, cap, "SitMaxLedgeCm",    (int)g_maxLedge);
    TwkIniSetInt(buf, cap, "SitMinLedgeCm",    (int)g_minLedge);
    TwkIniSetInt(buf, cap, "SitReachCm",       (int)g_reach);
    TwkIniSetInt(buf, cap, "SitLeanDeg",       (int)g_lean);
    TwkIniSetInt(buf, cap, "SitTraceChannel",  g_channel);
    TwkIniSetInt(buf, cap, "SitShoulderDropPct", (int)(g_shoulderDrop * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "SitShoulderFollowPct", (int)(g_shoulderFollow * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "SitPalmFlipLeft",  g_palmFlip[0]);
    TwkIniSetInt(buf, cap, "SitPalmFlipRight", g_palmFlip[1]);
    TwkIniSetInt(buf, cap, "SitBlendMs",       g_blendMs);
    TwkIniSetInt(buf, cap, "SitHoldMs",        g_holdMs);
    TwkIniSetInt(buf, cap, "SitStyleBlendMs",  g_styleMs);
    TwkIniSetInt(buf, cap, "SitDebug",         g_debug);
    TwkIniSetInt(buf, cap, "SitBoardRest",     g_boardRest);
    TwkIniSetInt(buf, cap, "SitBoardOutCm",    (int)g_boardOut);
    TwkIniSetInt(buf, cap, "SitBoardDropCm",   (int)g_boardDrop);
    TwkIniSetInt(buf, cap, "SitBoardAheadCm",  (int)g_boardAhead);
    TwkIniSetInt(buf, cap, "SitPelvisSeatCm",  (int)g_pelvisSeat);
    TwkIniSetInt(buf, cap, "SitPelvisFloorCm", (int)g_pelvisFloor);
    TwkIniSetInt(buf, cap, "SitInsetPct",      (int)(g_insetMul * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "SitRealMove",      g_realMove);
    TwkIniSetInt(buf, cap, "SitSettlePct",     (int)(g_settle * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "SitStaggerPct",    (int)(g_stagger * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "SitFootRadiusCm",  (int)g_footRadius);
    TwkIniSetInt(buf, cap, "SitToeRoomCm",     (int)g_toeRoom);
    TwkIniSetStr(buf, cap, "SitKey",           g_keyName);
    TwkIniSetStr(buf, cap, "SitFirstPersonKey", g_fpKeyName);
    TwkIniSetInt(buf, cap, "SitFirstPerson",   g_fpOn);
    TwkIniSetInt(buf, cap, "SitLookYawDeg",    (int)g_lookYawMax);
    TwkIniSetInt(buf, cap, "SitLookUpDeg",     (int)g_lookUpMax);
    TwkIniSetInt(buf, cap, "SitLookDownDeg",   (int)g_lookDownMax);
    TwkIniSetInt(buf, cap, "SitLookSpeedDps",  (int)g_lookSpeed);
    TwkIniSetInt(buf, cap, "SitLookInvert",    g_lookInvert);
    TwkIniSetInt(buf, cap, "SitFirstPersonFov", (int)g_fpFov);
    TwkIniSetInt(buf, cap, "SitEyeUpCm",       (int)g_eyeUp);
    TwkIniSetInt(buf, cap, "SitEyeFwdCm",      (int)g_eyeFwd);
    TwkIniSetInt(buf, cap, "SitViewBlendMs",   g_fpViewMs);
    TwkIniSetInt(buf, cap, "SitHideHead",      g_hideHead);
    TwkIniSetInt(buf, cap, "HeadFollowsCamera", g_headLook);
    TwkIniSetInt(buf, cap, "HeadLookHoldDeg",  (int)g_holdBand);
    TwkIniSetInt(buf, cap, "HeadLookRate",     (int)g_camLookRate);
    TwkIniSetInt(buf, cap, "SitWatchRangeCm",  (int)g_watchRange);
    TwkIniSetInt(buf, cap, "SitBailOnHit",     g_bailOnHit);
    TwkIniSetInt(buf, cap, "SitHitRadiusCm",   (int)g_hitRadius);
    TwkIniSetInt(buf, cap, "SitHitRiseCm",     (int)g_hitRise);
    TwkIniSetInt(buf, cap, "SitHitSpeedCm",    (int)g_hitSpeed);
    SitUI_SaveConfig(buf, cap);
}
void Sit_ResetDefaults() { g_on = 1; g_floor = 1; g_maxLedge = 120.0f; g_reach = 110.0f; g_lean = 0.0f; }
bool  Sit_Enabled()             { return g_on != 0; }
void  Sit_SetEnabled(bool o)    { g_on = o ? 1 : 0; if (!g_on) StandUp("sitting switched off"); TwkMarkDirty(); }
bool  Sit_FloorEnabled()        { return g_floor != 0; }
void  Sit_SetFloorEnabled(bool o) { g_floor = o ? 1 : 0; TwkMarkDirty(); }
float Sit_MaxLedgeCm()          { return g_maxLedge; }
void  Sit_SetMaxLedgeCm(float v){ g_maxLedge = clampf(v, 30.0f, 200.0f); TwkMarkDirty(); }
float Sit_ReachCm()             { return g_reach; }
void  Sit_SetReachCm(float v)   { g_reach = clampf(v, 40.0f, 200.0f); TwkMarkDirty(); }
float Sit_LeanDeg()             { return g_lean; }
bool  Sit_HeadLook()            { return g_headLook != 0; }
void  Sit_SetHeadLook(bool o)   { g_headLook = o ? 1 : 0; TwkMarkDirty(); }
void  Sit_SetLeanDeg(float v)   { g_lean = clampf(v, -25.0f, 25.0f); TwkMarkDirty(); }

void Sit_DrawMenu(const OmpMenuApi* api) {
    bool on = g_on != 0, fl = g_floor != 0, dbg = g_debug != 0;
    if (api->Checkbox("Sit down (B while off the board)", &on)) Sit_SetEnabled(on);
    api->SameLine(); api->TextDisabled(g_ok ? "(press again to stand)" : "(unavailable this build -- see the log)");
    if (api->Checkbox("Sit on the ground when there is no ledge", &fl)) Sit_SetFloorEnabled(fl);
    float v = g_maxLedge;
    if (api->SliderFloat("Tallest ledge (cm)", &v, 30.0f, 200.0f, "%.0f")) Sit_SetMaxLedgeCm(v);
    v = g_reach;
    if (api->SliderFloat("Reach for a ledge (cm)", &v, 40.0f, 200.0f, "%.0f")) Sit_SetReachCm(v);
    v = g_lean;
    if (api->SliderFloat("Torso lean (deg, + forward)", &v, -25.0f, 25.0f, "%.0f")) Sit_SetLeanDeg(v);
    bool hl = g_headLook != 0;
    if (api->Checkbox("Head follows the camera (off the board)", &hl)) Sit_SetHeadLook(hl);
    if (api->Checkbox("Log one pose dump per sit", &dbg)) { g_debug = dbg ? 1 : 0; TwkMarkDirty(); }
    char b[220];
    snprintf(b, sizeof(b), "%s | blend %.2f | %ld frames posed | %d faults", g_last, g_alpha, g_applies, g_faults);
    api->TextDisabled("Tap the sit key while seated to change how you sit; hold it to stand.");
    char pb[180]; snprintf(pb, sizeof(pb), "on-screen prompt: %s", SitUI_Status());
    api->TextDisabled(pb);
    api->TextDisabled(b);
}

void Sit_Install() {
    g_flipAt   = TwkScanExe(SIG_FLIP);
    g_trace    = (TraceFn)TwkScanExe(SIG_TRACE);
    g_setMode  = (SetModeFn)TwkScanExe(SIG_SET_MODE);
    g_getWorld = (GetWorldFn)TwkScanExe(SIG_GET_WORLD);
    g_teleport = (TeleportFn)TwkScanExe(SIG_TELEPORT);
    g_bail     = (BailFn)TwkScanExe(SIG_BAIL);
    g_placeAt  = TwkScanExe(SIG_PLACE_IN_HAND);
    if (g_placeAt && (MH_CreateHook(g_placeAt, (void*)&hkPlaceInHand, (void**)&g_origPlaceInHand) != MH_OK ||
                      MH_EnableHook(g_placeAt) != MH_OK)) { g_placeAt = nullptr; g_origPlaceInHand = nullptr; }
    g_detach   = (DetachFn)TwkScanExe(SIG_DETACH);
    g_attach   = (AttachFn)TwkScanExe(SIG_ATTACH);
    g_setSim   = (SetSimFn)TwkScanExe(SIG_SET_SIM);
    g_boardSim = (BoardSimFn)TwkScanExe(SIG_BOARD_SIM);
    g_ragOn  = (RagDollFn)TwkScanExe(SIG_RAGDOLL_ON);
    g_ragOff = (RagDollFn)TwkScanExe(SIG_RAGDOLL_OFF);
    if (!g_flipAt || !g_trace || !g_setMode || !g_getWorld) {
        TwkLog("[sit] %s%s%s%s-- sitting unavailable (game updated?)",
               g_flipAt ? "" : "FlipEditableSpaceBases sig NOT FOUND ", g_trace ? "" : "LineTraceSingleByChannel sig NOT FOUND ",
               g_setMode ? "" : "SetMovementMode sig NOT FOUND ", g_getWorld ? "" : "AActor::GetWorld sig NOT FOUND ");
        return;
    }
    if (MH_CreateHook(g_flipAt, (void*)&hkFlip, (void**)&g_origFlip) != MH_OK || MH_EnableHook(g_flipAt) != MH_OK) {
        TwkLog("[sit] hook failed on FlipEditableSpaceBases -- sitting unavailable");
        return;
    }
    g_ok = true;
    if (!g_bail) TwkLog("[sit] Bail sig NOT FOUND -- being skated into will not knock you over");
    TwkLog("[sit] armed: pose seam @ %p, trace @ %p, movement mode @ %p, key %s", g_flipAt, (void*)g_trace, (void*)g_setMode, g_keyName);
    TwkLog("[sit] board rest: %s (teleport %s, detach %s, attach %s, physics %s, carry hook %s)",
           g_boardRest && g_teleport && g_detach && g_setSim ? "on" : "unavailable",
           g_teleport ? "ok" : "MISSING", g_detach ? "ok" : "MISSING", g_attach ? "ok" : "MISSING",
           g_boardSim ? "whole board" : g_setSim ? "root only" : "MISSING", g_placeAt ? "ok" : "MISSING");
    TwkLog("[sit] board let go by %s", g_ragOn && g_ragOff ? "the game's own ragdoll call" : "raw physics (ragdoll call MISSING)");
    SitUI_Install();
}
