// SessionTweaks -- EMOTES: gestures performed on the skater's own skeleton, off the board.
//
// The game ships no emotes and no animations to make them from, so every one of these is PERFORMED: a
// few lines that say where a hand goes, which way a palm faces and how far each finger curls, as a
// function of time, and a small solver that turns that into a pose. They ride the seam sitting found --
// USkinnedMeshComponent::FlipEditableSpaceBases, called by the host's FinalizeBoneTransform, so a write
// there lands after the animation graph and after the host, and before the buffers flip. sit.cpp owns
// that detour (one DLL cannot hook an address twice) and calls Emote_OnFlip from it, after its own pose.
//
//   * ON TOP OF WHATEVER THE BODY IS DOING. An emote reads the pose the game made this frame and moves
//     only what it needs: an arm, the head, a lean of the spine. The legs keep the game's walk, so you
//     can wave while you walk; a seated body keeps its seat, so you can give a thumbs up from a ledge.
//     Hands are placed in the CHEST'S OWN FRAME -- measured off the shoulders and the spine each frame --
//     so a gesture rides the sway of a walk instead of floating beside it.
//   * NOTHING ABOUT THE RIG'S AXES IS ASSUMED (sitting's rule, and its solver's shape): bones are found by
//     name and PARENT CHAIN, lengths are measured off the live pose, every bone is turned by the shortest
//     rotation from where it points to where it should (qfromto), in component space. Arms and legs are
//     analytic two-bone IK. A rotation about an axis is only ever used with the body frame built as
//     forward = right x up, where the algebra fixes its sense: about right, + tips forward; about up,
//     + turns right; about forward, + leans LEFT.
//   * WHICH WAY A PALM FACES is anatomy, not calibration. With the fingers found by NAME (the biped's
//     Finger0 is the thumb, 1 the index, 4 the little finger), palm = (fingers x index-to-little), negated
//     on the right hand. sit.cpp reached the same two signs by trial in the field (SitPalmFlipLeft 1,
//     Right 0) from a different pair of bones; this derivation reproduces both. A finger curls about
//     (its direction x the palm normal), which by the same algebra always closes TOWARD the palm.
//   * THE BOARD IS IN ONE OF YOUR HANDS -- BUT NOT ON ITS BONE. First written as "it rides a hand socket, so
//     whatever that arm does the board does"; the field said otherwise (the arm went up, the board stayed by
//     the hip). USkateboardExMovementComponent::PlaceInHand, run after physics every frame on foot, puts
//     the board at the FLIPPER SOCKET of the skater's mesh, and that socket is on the skeleton's own BOARD
//     RIG (SKXX_SkateSkel_Root > _Flipper, trucks, wheels, hand anchors), which the carry animation drives
//     directly: no bone of it hangs off a hand. So whenever the carrying hand is moved the board rig is
//     carried with it, RIGIDLY, by the hand's own change (CarryBoard), and the game then puts the real
//     board there. One-handed emotes use the FREE hand (the nearer hand to the rig, while the board's
//     movement mode says on-foot, 5 -- NOT the 9 this module inherited); the taps and Rage are written for it.
//   * THE BOARD TAP IS PLAYED, NOT WATCHED. First it tapped by itself, from the grip the carry animation
//     has -- and the field measured that grip: 58 cm from the hand to the tail, the board 48 degrees off
//     upright, so the body had to crouch 38 cm and lean 33 degrees to land it. So the board is REGRIPPED BY
//     THE NOSE (its bones are ours to pose: the rig is turned tail-down and its nose put in the palm), a
//     whole board-length of reach, and nobody has to bend. And the RIGHT STICK works it: up lifts, letting
//     go drops it, down drives it into the ground, sideways swings the tail. The lift is a spring with
//     weight in it -- slower up than down, a small bounce on landing -- integrated in the pump, and a
//     landing faster than a touch is a TAP: the body takes it, and the board's own knock plays, as that
//     ground sounds. The camera's stick is held still while it lasts (radial.cpp's two seams), B ends it.
//     NOBODY BENDS FOR IT. The first nose grip still crouched 10 cm and leaned, and looked it (the field:
//     "you should just be standing normal", with a photograph). The log had the reason: the board is 82 cm,
//     the hand was modelled FLAT ON the nose with its wrist 2.5 cm above it, so the wrist had to be at 84 cm
//     -- and this skater's straight arm ends at 92. But that is not how a nose is held: the hand HANGS from
//     the wrist and the fingers hook the nose, wrist some 9-10 cm above it -- 92 cm: exactly where a relaxed
//     arm already is. So the hand hangs, the legs and spine are left to the game entirely, and what little
//     can still be missing is found where a person finds it: a shoulder let down a little, the hand a little
//     higher on the nose, a few degrees of lean -- never the knees. The board stands upright beside the leg
//     with its graphic to the front, and the ground under its tail is the REAL ground there, by a trace.
//   * THE SOUND IS THE GAME'S. ASkateboardEx::SpawnImpactAudio is the recipe: the cue out of the board's
//     USkateboardAudioDataAsset (`_boardHitOffBoardSoundCue` -- a board knocked about with nobody on it),
//     spawned through UReplayAudioManager::SpawnSoundAttached (so it is in the replay and reaches other
//     players, as the catch sound does), then FSessionUtils::UpdateAudioWithSurfaceType on the component.
//     The game takes the surface from under the RIDDEN board, which on foot is stale, so ours is read off a
//     short trace under the tail (Sit_TraceSurface; the hit's physical material, asked for explicitly).
//     Its SpawnBoardHitAudio is NOT usable: it reads loudness off the board's physics velocity, and a board
//     in the hand is moved kinematically -- always zero, always silent.
//   * RAGE THROWS THE REAL BOARD. sit.cpp already knows how to let go of it (detach, the game's own
//     rag-doll, PlaceInHand refused while it is out) and is the ONE owner of "the board is out of the
//     hand", so a seat and a throw cannot both hold it: Sit_BoardThrow. It is back in hand when you walk
//     up to it, press the mount key, stand up from a seat taken meanwhile, or after a while.
//   * IT TRAVELS. While an emote is up the pose-hold flag is raised (tweaks_mod ORs it with sitting's),
//     and SessionOpenMP's pose lane then ships the skeleton whenever it changes -- the lane a bail
//     already uses -- so other players see the gesture, fingers and all.
//   * IT CANNOT OUTLIVE ITS PUMP. The clock runs in Emote_PumpFrame, which rides the skater's tick and
//     stops in the editors and in any level without a skater; the pose hook does not stop. So the hook
//     refuses to write when the pump has been quiet for 300 ms (the radial menu's lesson), and every
//     object kept across frames is proven with SitUI_Alive before it is believed.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "tweaks_common.h"
#include "emote.h"
#include "sit.h"              // Sit_PoseHeld, Sit_EditorOpen
#include "sit_ui.h"           // SitObjRef / SitUI_Track / SitUI_Alive
#include "catch_tweaks.h"     // CatchTweaks_Skater, and the trigger axes
#include "radial.h"           // the wheel must not be up when the left trigger arms a throw
#include "foot_place.h"       // FootPlace_AnimInstance
#include "grind_pop.h"        // GrindPop_FNameToString
#include "camera_height.h"    // CameraHeight_ViewForward: where the camera looks, for Point
#include "ui/menu_ext.h"      // the F1 "Board tap hand" page
#include "catch_sound.h"      // CatchSound_SpawnAttached: the replay-aware one-shot, for the knock of the tap
#include "radio.h"            // Radio_LoadAsset: the clap's sound and falloff, by path (539)
#include "cloth_merge.h"      // ClothMerge_RootObject: the clap's sound stays loaded for other players' claps

namespace {

// ------------------------------------------------------------------ layouts (the same names and values as sit.cpp)
enum {
    AN_ON_BOARD   = 0x300,   // USkaterAnimInstance::IsOnBoard
    AN_THROWING_DOWN = 0x302,  // USkaterAnimInstance::IsThrowingDown
    SK_THROWDOWN  = 0xa28,   // ASkaterCharacterBase::_isDoingThrowdown (run_out reads the same)
    ACT_ROOT      = 0x130,   // AActor::RootComponent
    CH_MESH       = 0x280,   // ACharacter::Mesh
    CH_MOVE       = 0x288,   // ACharacter::CharacterMovement
    BD_MOVECOMP   = 0x298,   // ASkateboardEx::_skateboardMovement
    BM_MODE       = 0x534,   //   ...its _movementMode byte, READ ONLY (sit.cpp asks it: Sit_BoardInHand, 5 = on foot)
    SC_C2W        = 0x1c0,   // USceneComponent::ComponentToWorld (FTransform: quat, +0x10 pos, +0x20 scale)
    MOVE_VEL      = 0xc4,    // UMovementComponent::Velocity
    SKM_MESH      = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SKM_CST       = 0x4b0,   // USkinnedMeshComponent::ComponentSpaceTransformsArray[2], 0x10 apart
    SKM_EDIT      = 0x4f0,   // CurrentEditableComponentTransforms
    SKM_READ      = 0x4f4,   // CurrentReadComponentTransforms: what PlaceInHand hung the board from last frame
    SM_REFSKEL    = 0x1b0,   // USkeletalMesh::RefSkeleton
    RS_FINAL_INFO = 0x20,    // FReferenceSkeleton::FinalRefBoneInfo (FMeshBoneInfo: FName, int parent; 12 B)
    RS_FINAL_POSE = 0x30,    //   ...FinalRefBonePose (FTransform each, parent-relative): the board lies FLAT in it
    // THE SESSION MARKER, polled -- never hooked (the co-op host already owns a MinHook on
    // ASkaterCharacter::PopulateMarkerInfo, and one address cannot be hooked twice).
    // IT LIVES ON THE GAME'S OWN MARKER CONTROLLER, not on the pawn. The pawn's +0xac0 is the PENDING copy,
    // which `SpotMarkerController::GotoMarker` fills in ON THE WAY BACK as well -- field 3.19.385: reading it
    // made the return look like the marker being set again, which wiped what had been remembered about it.
    PAWN_CONTROLLER = 0x258,  // APawn::Controller (PDB)
    PC_SPOT_MARKER  = 0x7b0,  // ASessionPlayerController::_spotMarkerController
    SMC_ACTIVE      = 0x05,   // SpotMarkerController::_isMarkerActive
    SMC_INFO        = 0x40,   // ::_markerInfo (FSessionPlayerMarkerInfo: +0x00 IsSet, +0x04 FVector SkaterLoc)
    SK_BOARD      = 0x568,   // ASkaterCharacterBase::_skateboard
    BD_FLIPPER    = 0x4e8,   // ASkateboardEx::_flipper (UStaticMeshComponent*): THE DECK YOU SEE. Then, each its own component:
    BD_TRUCK_B    = 0x4f0,   //   ..._truckBack
    BD_TRUCK_F    = 0x500,   //   ..._truckFront
    BD_WHEEL_BL   = 0x510,   //   ..._wheelBackLeft, then BackRight, FrontLeft, FrontRight, 8 apart
    BM_FOOT_LOCSC = 0xa20,   // USkateboardExMovementComponent::_onFootBoardAnimLocationScale  } PlaceInHand does not copy the
    BM_FOOT_RELOF = 0xa24,   //   ..._onFootBoardAnimLocationRelOffset (FVector)                } socket: it scales and shifts it,
    BM_FOOT_ROTSC = 0xa30,   //   ..._onFootBoardAnimRotationScale                              } and turns it if the flag says.
    BM_HAND_REVRS = 0xa34,   //   ..._flipperInHandReversed                                     } Read only, and only to be SAID.
    BD_AUDIO_DATA = 0x330,   // ASkateboardEx::_audioData (USkateboardAudioDataAsset*)
    AD_GROUND_HIT = 0x3c0,   // USkateboardAudioDataAsset::_groundImpactSoundCue
    AD_BOARD_HIT  = 0x3e8,   //   ..._boardHitOffBoardSoundCue: the board knocked about with nobody on it
    AD_HIT_MAXVOL = 0x3fc,   //   ..._maximumVolumeBoardHitSound
    AD_HIT_PITCH0 = 0x400,   //   ..._minimumPitchBoardHitSound, then the maximum
};
enum { MAX_BONES = 200 };

// ------------------------------------------------------------------ vectors and quaternions
struct V3 { float x, y, z; };
struct Q4 { float x, y, z, w; };
V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
V3 mulv(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float len(V3 a) { return sqrtf(dot(a, a)); }
V3 norm(V3 a) { const float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : v3(0, 0, 1); }
V3 lerp(V3 a, V3 b, float t) { return add(a, mul(sub(b, a), t)); }
V3 mix3(V3 a, float ka, V3 b, float kb, V3 c, float kc) { return add(add(mul(a, ka), mul(b, kb)), mul(c, kc)); }
Q4 q4(float x, float y, float z, float w) { Q4 q = { x, y, z, w }; return q; }
Q4 qid() { return q4(0.0f, 0.0f, 0.0f, 1.0f); }
Q4 qconj(Q4 q) { return q4(-q.x, -q.y, -q.z, q.w); }
// Hamilton product a*b: rotate by b first, then by a. UE composes a child under its parent as parent*child.
Q4 qmul(Q4 a, Q4 b) {
    return q4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
V3 qrot(Q4 q, V3 v) {
    const V3 u = v3(q.x, q.y, q.z);
    const V3 t = mul(cross(u, v), 2.0f);
    return add(add(v, mul(t, q.w)), cross(u, t));
}
V3 qinv(Q4 q, V3 v) { return qrot(qconj(q), v); }
Q4 qnorm(Q4 q) {
    const float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? q4(q.x / l, q.y / l, q.z / l, q.w / l) : qid();
}
Q4 qaxis(V3 axis, float deg) {
    const V3 a = norm(axis); const float h = deg * 0.0174532925f * 0.5f, s = sinf(h);
    return q4(a.x * s, a.y * s, a.z * s, cosf(h));
}
// The shortest rotation carrying direction a onto direction b.
Q4 qfromto(V3 a, V3 b) {
    a = norm(a); b = norm(b);
    const float d = dot(a, b);
    if (d > 0.99999f) return qid();
    if (d < -0.99999f) {
        V3 ax = cross(v3(1, 0, 0), a);
        if (len(ax) < 1e-4f) ax = cross(v3(0, 1, 0), a);
        ax = norm(ax);
        return q4(ax.x, ax.y, ax.z, 0.0f);
    }
    const V3 c = cross(a, b);
    return qnorm(q4(c.x, c.y, c.z, 1.0f + d));
}
Q4 qblend(Q4 a, Q4 b, float t) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) b = q4(-b.x, -b.y, -b.z, -b.w);
    return qnorm(q4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float smooth(float x) { x = clampf(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
// 1 between t0 and t1, easing over `edge` seconds at each end.
float window(float t, float t0, float t1, float edge) { return smooth((t - t0) / edge) * smooth((t1 - t) / edge); }
const float TAU = 6.2831853f, PI = 3.14159265f;
// THE DANCES, named here because kDefs below counts them. What each one is: see THE DANCES, further down.
// A running man, a step-and-snap and a breakdance were built too, and thrown out in the field: "you did
// terrible with those". What is left is what reads as dancing on this rig -- weight, not choreography.
enum { DS_TWO_STEP = 0, DS_SWAY, DS_TWIST, DS_COUNT };

// ------------------------------------------------------------------ the emotes, in wheel order
// EM_CARRY is NOT ON THE WHEEL: a prop asks for it (radio.cpp) -- a box held under the free arm, long ways. Everything
// from EM_WHEEL on is internal; the wheel lists the ones before it.
enum { EM_NONE = -1, EM_WAVE = 0, EM_THUMBS, EM_POINT, EM_FACEPALM, EM_CLAP, EM_TAP, EM_RAGE, EM_DANCE, EM_CARRY, EM_COUNT, EM_WHEEL = EM_CARRY };
// EVERY EMOTE IS HELD UNTIL IT IS PUT AWAY (B, or picked again): `loop` is true for all of them now, and `dur`
// is only what a one-shot used to last. board: needs it in your hand.
// `wheel`: does it appear on the emote wheel? Some are reached by a button instead -- a throw is armed
// by holding the left trigger -- and those are hidden rather than renumbered, because an emote's index
// IS its identity everywhere else in this file.
// `variants`: how many ways there are to do it. B TAPPED goes round them, B HELD puts the emote away
// (an emote with one way is stopped by either -- there is nothing to go round).
struct Def { const char* name; float dur; bool loop; bool lower; bool board; bool wheel; int variants; };
const Def kDefs[EM_COUNT] = {
    { "Wave",      0.0f, true, false, false, true, 1 }, { "Thumbs up", 0.0f, true, false, false, true, 1 }, { "Point", 0.0f, true, false, false, true, 1 },
    { "Facepalm",  0.0f, true, false, false, true, 1 }, { "Clap",      0.0f, true, false, false, true, 1 },
    // EM_TAP is NOT on the wheel either: RB holds the board in the tap pose, and HOLDING RB takes the
    // right stick to work it. No lower body -- you stand as you stand.
    { "Board tap", 0.0f, true, false, true,  false, 1 },
    // EM_RAGE is NOT on the wheel: holding the left trigger off the board arms it, the right throws.
    { "Throw board", 0.0f, true, false, true, false, 1 }, { "Dance",  0.0f, true, true,  false, true, DS_COUNT },
    { "Carry",     0.0f, true, false, false, false, 1 },   // EM_CARRY: internal (a held prop)
};
// The wheel's entries are the ones flagged above, in order -- so its Nth entry is not necessarily
// emote N, and everything that takes a wheel index maps through here.
static int g_wheelIdx[EM_COUNT];
static int g_wheelN = -1;
static void BuildWheelIndex() {
    if (g_wheelN >= 0) return;
    g_wheelN = 0;
    for (int i = 0; i < EM_WHEEL; i++) if (kDefs[i].wheel) g_wheelIdx[g_wheelN++] = i;
}
// A RAGE: wound up by kRageWind, then HELD AND AIMED for as long as you like; the RIGHT TRIGGER throws it. The swing
// starts the moment the trigger is felt, the board leaves the hand kRageLetGo later, and HOW HARD is HOW FAST the
// trigger was pulled in between -- a pull takes about that long, so nothing waits on it.
const float kRageWind = 0.50f, kRageLetGo = 0.14f, kRageSwingLen = 0.30f;
// THE ONE EMOTE THAT ENDS ITSELF: held for as long as you aim, but once the board has gone he is angry about it for
// a bit and that is that -- the old one-shot's own length (it swung at 0.5 of 3.2 s), measured from the swing.
const float kRageAfter = 2.36f;
const float kTrigOn = 0.12f, kTrigOff = 0.07f;      // felt / let go (the pad's own "pressed" is at 0.12 too)
const float kBlendIn = 0.24f, kBlendOut = 0.34f, kBlendSwap = 0.12f;
const float kVarBlend = 0.45f;      // one way of doing it into the next

// ------------------------------------------------------------------ state (game thread: the pump and the hook both)
int       g_on = 1;                       // EmotesEnabled
int       g_id = EM_NONE, g_next = EM_NONE, g_req = EM_NONE;
float     g_t = 0.0f, g_w = 0.0f, g_lowerW = 0.0f, g_moveS = 0.0f;
// WHICH WAY OF DOING IT (the dances: two-step, sway, twist, running man, step and snap, breakdance), the one
// before it, and the change-over between the two. The choice is remembered per emote, so picking Dance again
// carries on with the one you were doing.
int       g_var = 0, g_varPrev = 0, g_lastVar[EM_COUNT] = { 0 };
float     g_varMix = 1.0f;
float     g_danceBpm = 120.0f;                 // EmoteDanceBpm: what the dances are counting to
// B: TAPPED it is the next variation, HELD it puts the emote away. Timed in the pump, since the key hook has
// no clock of its own; the bar's ring shows the hold filling up.
bool      g_bDown = false, g_bUsed = false; float g_bHeld = 0.0f;
float     g_stopHold = 0.28f;                  // EmoteStopHoldMs
// THE MARKER REMEMBERS THE BOARD TAP: set your marker with the board up in the tap pose and coming back to
// that marker puts it back up. Where the marker is, whether the tap was up when it was set, the goto flag as
// it was last seen, and the countdown that waits for the graph reset a return leaves behind.
int       g_mkTapOn = 1; float g_mkDelay = 0.35f;      // EmoteTapMarker, EmoteTapMarkerMs
bool      g_mkSeen = false, g_mkHasTap = false, g_mkFound = false, g_mkPosOk = false;
V3        g_mkLoc = { 0.0f, 0.0f, 0.0f }, g_mkPos = { 0.0f, 0.0f, 0.0f };
float     g_mkWait = -1.0f, g_mkTry = 0.0f;
int       g_mkSaid = 0;
bool      g_ending = false; float g_outTime = kBlendOut;
void*     g_skater = nullptr, *g_mesh = nullptr;
SitObjRef g_meshRef = { nullptr, 0, 0, nullptr };
int       g_carry = -2;                   // -2 not looked yet, -1 both hands free, 0 / 1 the hand with the board
bool      g_boardInHand = false;          // the board's own movement mode says it is being carried (the pump reads it)
const char* g_whyNot = "";
// Rage: which way it goes (chosen as it starts), what it leaves with, and what is left of the throw to do
float     g_rageYaw = 0.0f, g_rageSpeed = 800.0f, g_ragePitch = 0.0f, g_rageSpin[3] = { 0, 0, 0 };
// the aim: g_rageYaw (radians off straight ahead, unwrapped) and g_ragePitch (radians, -90..+90) FOLLOW THE
// CAMERA until the board goes;
// how much of it the spine has taken up (degrees); the clock the hook eases them by
float     g_rageTwist = 0.0f, g_rageLastT = 0.0f; bool g_rageAimSet = false;
// the trigger: as last fed (0..1), whether it has been let go since the Rage began (one held through the wheel
// must not fire it), when the swing started (emote time; < 0 = still aiming) and the FASTEST the trigger has been
// pulled since -- in trigger-units per second, which is what strength is made of
float     g_trigger = 0.0f, g_rageSwingAt = -1.0f, g_rageRate = 0.0f; bool g_trigArmed = false;
// ...a pull in progress: the most it has reached and when it last went further (a pull that has STOPPED is one
// whose strength is known), and where and when it STARTED, which is the other end of the rate; and whether the
// engine is handing over the real analogue value (else: the press)
bool      g_pulling = false, g_trigPolled = false; float g_pullHigh = 0.0f, g_pullRiseT = 0.0f; int g_trigSaid = 0;
float     g_pullLowV = 0.0f, g_pullLowT = 0.0f;
// HOW FAST COUNTS AS HARD, in trigger-units per second: a deliberate squeeze from nothing to the end over half a
// second is about 2, a brisk pull about 7, a snap 20 or more. Below kRateSoft is the gentlest throw there is,
// kRateHard and above is everything. `kRateMinDt` is a floor on the measured interval so a pull that happens
// inside ONE frame reads as very fast rather than as a divide by zero.
float     kRateSoft = 1.5f, kRateHard = 10.0f;
const float kRateMinDt = 0.008f;
// the clap: where in its cycle the pump last saw it, and the sound it makes (looked up by name, watched like any
// object -- a cue can be unloaded under us)
float     g_clapPhase = 0.0f; int g_clapCount = 0, g_clapMuted = 0;
float     g_clapEchoAt = -1.0f;   int g_clapDoubleMs = 45;   // the clap's second strike (see ClapSound)
// THE POSE SEAM'S OWN RECEIPT: which emote was last actually WRITTEN to the skeleton, and when.
int             g_posedId = -1;          // EM_NONE is 0-ish, so -1 = "nothing has been posed yet"
unsigned long long g_posedMs = 0;
// Was THIS emote's pose actually written to the skeleton in the last few frames? The pump runs on the
// skater's tick, but the pose is only written at sit's Flip seam, and the two can come apart (no seam
// while an editor owns the skeleton, a stopped seam, a body swapped underneath). A sound is the one
// part of an emote that is audible whether or not any of that happened -- so anything that makes a
// NOISE asks this first, and a clap you cannot see is never heard. Field 2026-09-19: "I'm hearing the
// clapping emote on my own character when I'm not clapping".
static bool PoseIsLive(int id) {
    return g_posedId == id && g_posedMs && (LONGLONG)GetTickCount64() - (LONGLONG)g_posedMs <= 120;
}
void*     g_clapCue = nullptr; SitObjRef g_clapCueRef = {}; bool g_clapCueTried = false;
char      g_clapSound[160] = "WAV_Cinematic_HandLand_03"; float g_clapPitch = 2.0f, g_clapVolume = 4.0f;
// THE CLAP'S SOUND AND ITS REACH (539). "it just doesnt sound good and also is heard from everywhere in the map".
// It was SCU_HandLand -- a palm landing on the ground, pitched up -- and that cue has NO attenuation, so it
// played at full volume anywhere; worse, OpenMP replays a peer's sound with attenuation left to the sound's own
// (null), so every player heard every clap map-wide. The falloff is put ON THE ASSET (USoundBase::
// AttenuationSettings +0x118, walking's own SAT_Sfx_Walk-Run: silent past 50 m), on every client, so the local
// clap and every replayed one fade alike with nothing on the wire; a cue's own override (+0x530 bit 0) would
// beat it, so that is cleared. 540: the user tried WAV_Cinematic_HandFace (539) and kept the palm -- pitch 200,
// volume 400, one strike. The game's own hand landings now fade the same way (they were map-wide too).
// 541: "the hand sound we have right now seems to be multiple different sounds. One of them sounds kinda like a
// clap, the rest sound like a thump" -- SCU_HandLand picks at random among WAV_Cinematic_HandLand_01..03.
// Decoded out of the pak: 03 is the clap (the sharpest onset 15 ms, centroid 4.6 kHz, 67% above 1.5 kHz, 13%
// below 300 Hz), 02 the thump (2.2 kHz, rings 186 ms), 01 between. The clap plays 03 alone (loaded with the
// cue, so every client finds it by name); the cue keeps its falloff too (older peers' claps play the cue).
char      g_clapAtten[160] = "";
static const char* const kClapAtten = "/Game/Audio/SAT_Sfx_Walk-Run.SAT_Sfx_Walk-Run";
enum { SB_ATTENUATION = 0x118,                // USoundBase::AttenuationSettings (PDB)
       SCUE_FLAGS = 0x530,                     // USoundCue: bit 0 bOverrideAttenuation (then AttenuationOverrides rule)
       SCUE_NODEFLAGS = 0x538 };               // USoundCue: bit 1 bHasAttenuationNode (an attenuation node in the graph)
static char g_clapUsing[64] = "";
// the tap: THE POP OF AN OLLIE (asked for: "that seems like it'd fit a lot better" than the knock of a loose board).
// The game keeps it as a cue of its own, SCU_Pop_Hi_Jump, with a recording per surface behind it -- so the same
// surface switch the knock had applies. Found by name and watched, like the clap's; the board's own knock is
// what is played if it is not to be had.
void*     g_tapCue = nullptr; SitObjRef g_tapCueRef = {}; bool g_tapCueTried = false;
char      g_tapSoundName[64] = "SCU_Pop_Hi_Jump"; float g_tapPopVolume = 1.0f;
// THE CARRY: the box being held, as its mesh has it (the bounds' centre and half sizes, in the mesh's own space),
// and where the pose wants the ACTOR (its origin, its turn) in the WORLD -- published by the hook every frame the
// pose is up, for the prop to be put there. Which arm is whichever has no board in it.
float     g_boxO[3] = { 0, 0, 0 }, g_boxE[3] = { 30, 14, 7 };
float     g_boxPosW[3] = { 0, 0, 0 }, g_boxQuatW[4] = { 0, 0, 0, 1 }; bool g_boxSet = false, g_boxWorldOk = false; int g_boxSide = 1;
uint64_t  g_boneName[MAX_BONES];          // each bone's FName, raw: a bone's name is a socket's name
float     g_throwDirW[3] = { 1, 0, 0 }; bool g_throwDirSet = false, g_thrown = false;
float     g_kickVel[3] = { 0, 0, 0 }; int g_kickLeft = 0;
float     g_outS = 0.0f;                  // how long the board has been out
int       g_outSaid = 0;                  //   ...and how many times its distance has been said
bool      g_tapSay = false;               // a tap's geometry, said once as it starts
// The tap, as played: the stick as last fed, the tail's height off the ground and how fast it is moving
// (cm, cm/s -- a spring, integrated in the pump), its swing to the side, the blow the body is still taking,
// how much it is being carried for a walk, and where in the WORLD the tail is (for what it lands on).
float     g_stickX = 0.0f, g_stickY = 0.0f;
float     g_tapLift = 0.0f, g_tapVel = 0.0f, g_tapSwing = 0.0f, g_tapHit = 0.0f, g_tapWalk = 0.0f;
bool      g_tapDown = false, g_boardPosed = false, g_tapStickLift = false;
// ini: EmoteTapVolumePct (how loud the knock is, percent of the game's own), EmoteTapRollDeg (the board turned
// about its length, should a rig ever show the wrong face forward).
float     g_tapVolume = 2.5f, g_tapRollDeg = 0.0f;
// THE GRIP ON THE NOSE, live so it can be tried against the screen rather than rebuilt at. `reach` is wrist
// to knuckles and `palmGap` how far the deck's face stands off the palm -- together they say where on the
// hand the end of the wood sits. `curl` is the four fingers coming round the tip; they must stay past 0.5,
// which is where Fingers() has a closed finger for the thumb to fold onto -- under it the thumb springs
// open beside the board again. How the hand is TURNED is not a knob: see the table in DoTap.
float     g_tapReach = 9.0f, g_tapPalmGap = 2.0f;
// THE HAND THAT HOLDS THE BOARD, every joint of it -- the F1 "Board tap hand" page and the matching
// EmoteTap* ini keys. Turning: roll about the palm, twist about the fingers, pitch about the thumb's side
// (DoTap says what each one looks like). Placing: a nudge in the BODY's terms, cm. The defaults are the
// pose the field settled on, which is kTapShipped; all zero is the frame DoTap builds on its own.
float     g_tapHandRoll = 15.0f, g_tapHandTwist = 12.0f, g_tapHandPitch = -45.0f;
float     g_tapGripFwd = 3.0f, g_tapGripRight = 0.0f, g_tapGripUp = 0.0f;
// HOW SHUT EACH FINGER IS (0 open, 1 a fist), index to little, and the thumb ON ITS OWN -- 0 open, 1
// folded across them. One number for all four was tried and is not enough: a hand holding a board wants
// its thumb somewhere the other four are not, and moving them together can only ever trade one for the
// other. The thumb is a BLEND rather than a curl because of what ThumbBlend says. Open the four too far
// and their tips stop crossing the deck, which is where the gate draws the line.
float     g_tapCurl[4]  = { 0.07f, 0.09f, 0.08f, 0.12f };
float     g_tapThumb = 0.50f;
// ...and each finger's tip nudged off where its curl put it, the same three axes the thumb has:
// [finger][0] along the fingers, [1] out through the palm, [2] across toward the thumb. In cm.
float     g_tapFingerOff[4][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
// ...and WHERE the thumb sits once the blend has placed it, nudged in the hand's own terms (cm): along
// the fingers, out through the palm, across toward them. The blend alone runs alone one line between two
// poses, which cannot reach round a board -- these are the other two axes, and they are what "full
// control of the thumb" means in practice.
float     g_tapThumbAlong = 2.9f, g_tapThumbOut = 4.2f, g_tapThumbAcross = 3.0f;
// ...and how the reach an arm falls short by is found: the shoulder let down, then the grip slid up the
// board. In cm. The slide is LAST RESORT now -- it is what used to lift the hand clear of the board.
float     g_tapDropMax = 7.0f, g_tapSlideMax = 2.0f;
float     g_tapTipW[3] = { 0, 0, 0 }; bool g_tapTipSet = false;
// ...and what is under it: found by the pump's trace (world), turned into a height in the body's terms by the
// hook. Believed only near where the feet say the floor is -- a tail poked out over a drop is not an
// invitation to reach down it.
float     g_tapGroundW[3] = { 0, 0, 0 }; bool g_tapGroundSet = false; float g_tapGroundU = 0.0f; bool g_tapGroundUSet = false;
// THE KEPT BOARD OF A SEAT (sit.cpp's SitKeepBoard; SeatBoard below). Per frame, off Sit_BoardSpot.
void*     g_seatMesh = nullptr;              // the seated body whose board is laid down (the pump keeps its rig up)
SitObjRef g_seatMeshRef = { nullptr, 0, 0, nullptr };
bool      g_seatSpot = false;                // the seat gave a spot this frame
float     g_seatSet = 0.0f;                  // how far the board is set down: 0 in the hand .. 1 on its spot
V3        g_seatOs = { 0.0f, 0.0f, 0.0f }, g_seatU = { 0.0f, 0.0f, 1.0f };   // the seat plane, component space
int       g_seatCarry = -1;                  // the hand it rode in (sit.cpp's read of the carry)
float     g_seatMeasureT = 0.0f;
bool      g_seatGap = true;                  // the seat had no board to lay down (thrown, or not seated)
LONGLONG  g_seatBackMs = 0;                  // when a fetched board came back to a seat already sat in
int       g_tapSurface = 0;
int       g_tapCount = 0;
// void FSessionUtils::UpdateAudioWithSurfaceType(UAudioComponent*, EPhysicalSurface) -- Epic 0x11981e0 /
// Steam 0x11589c0, unique in both. What makes one cue sound like concrete here and wood there.
const char* SIG_AUDIO_SURFACE =
    "48 8B C4 48 89 48 08 55 56 48 8B EC 48 83 EC 68 48 89 58 10 33 DB 48 89 78 E8 4C 89 70 D0 4C 8B F1 "
    "4C 89 78 C8 48 8D 4D C8 44 8B FA 48 89 5D C8";
typedef void (*AudioSurfaceFn)(void* audioComponent, int surface);
AudioSurfaceFn g_audioSurface = nullptr; bool g_audioLooked = false;
unsigned  g_rng = 0x9e3779b9u;
bool      g_seated = false;
float     g_speed = 0.0f;
float     g_ptYaw = 0.0f, g_ptPitch = 0.0f; bool g_ptSet = false;
LONGLONG  g_qpc = 0; double g_qpf = 0.0;
volatile LONGLONG g_pumpMs = 0;
int       g_faults = 0;

// ------------------------------------------------------------------ the rig
struct Bone { char name[48]; int parent; };
Bone  g_bones[MAX_BONES];
int   g_nBones = 0;
int   g_pelvis = -1, g_spine[4] = { -1, -1, -1, -1 }, g_nSpine = 0, g_neck = -1, g_head = -1;
int   g_thigh[2], g_calf[2], g_foot[2], g_clav[2], g_uarm[2], g_larm[2], g_hand[2];     // [0] left, [1] right
int   g_fing[2][5][3];                    // [side][0 thumb .. 4 little][joint], -1 = not there
int   g_boardRoot[4], g_nBoardRoot = 0;   // the board rig's top bones (SKXX_SkateSkel_*): what is carried with a hand
int   g_truckF = -1, g_truckB = -1, g_flipper = -1;
int   g_wheel[4] = { -1, -1, -1, -1 };       // what a board lying down rests on
bool  g_rigOk = false, g_legsOk = false, g_fingersOk[2] = { false, false };

int SideOf(const char* n) {
    const size_t l = strlen(n);
    if (strstr(n, "_l_") || (l >= 2 && n[l - 2] == '_' && n[l - 1] == 'l')) return 0;
    if (strstr(n, "_r_") || (l >= 2 && n[l - 2] == '_' && n[l - 1] == 'r')) return 1;
    return -1;
}
int ChildNamed(int parent, const char* sub, int side) {
    for (int i = 0; i < g_nBones; i++)
        if (g_bones[i].parent == parent && strstr(g_bones[i].name, sub) && (side < 0 || SideOf(g_bones[i].name) == side))
            return i;
    return -1;
}
int AnyNamed(const char* sub) { for (int i = 0; i < g_nBones; i++) if (strstr(g_bones[i].name, sub)) return i; return -1; }
const char* BN(int i) { return i >= 0 ? g_bones[i].name : "-"; }

// Who is who, from the bone table alone (lower-cased names, parents first). Kept apart from the reading
// of that table so it can be run against a made-up skeleton with no game behind it (tools/emotetest).
bool ResolveNames() {
    const int n = g_nBones;
    g_rigOk = false; g_legsOk = false;
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
        g_clav[sd] = -1;
        for (int k = g_nSpine - 1; k >= 0 && g_clav[sd] < 0; k--) g_clav[sd] = ChildNamed(g_spine[k], "clavicle", sd);
        g_uarm[sd] = g_clav[sd] >= 0 ? ChildNamed(g_clav[sd], "upperarm", sd) : (top >= 0 ? ChildNamed(top, "upperarm", sd) : -1);
        g_larm[sd] = g_uarm[sd] >= 0 ? ChildNamed(g_uarm[sd], "lowerarm", sd) : -1;
        if (g_larm[sd] < 0 && g_uarm[sd] >= 0) g_larm[sd] = ChildNamed(g_uarm[sd], "forearm", sd);
        g_hand[sd] = g_larm[sd] >= 0 ? ChildNamed(g_larm[sd], "hand", sd) : -1;
        // The fingers, by the digit that follows "finger" in a child of the hand, then down each chain.
        for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) g_fing[sd][f][j] = -1;
        if (g_hand[sd] >= 0)
            for (int i = 0; i < n; i++) {
                if (g_bones[i].parent != g_hand[sd]) continue;
                const char* at = strstr(g_bones[i].name, "finger");
                if (!at || at[6] < '0' || at[6] > '4' || at[7] != 0) continue;
                const int f = at[6] - '0';
                g_fing[sd][f][0] = i;
                g_fing[sd][f][1] = ChildNamed(i, "finger", -1);
                g_fing[sd][f][2] = g_fing[sd][f][1] >= 0 ? ChildNamed(g_fing[sd][f][1], "finger", -1) : -1;
            }
        g_fingersOk[sd] = g_fing[sd][1][0] >= 0 && g_fing[sd][4][0] >= 0 && g_fing[sd][1][1] >= 0 && g_fing[sd][4][1] >= 0;
    }
    g_rigOk = g_pelvis >= 0 && g_nSpine > 0 && (g_head >= 0 || g_neck >= 0) &&
              g_uarm[0] >= 0 && g_uarm[1] >= 0 && g_larm[0] >= 0 && g_larm[1] >= 0 && g_hand[0] >= 0 && g_hand[1] >= 0;
    g_legsOk = g_thigh[0] >= 0 && g_thigh[1] >= 0 && g_calf[0] >= 0 && g_calf[1] >= 0 && g_foot[0] >= 0 && g_foot[1] >= 0;
    // The board rig: every "skateskel" bone whose parent is not one. Moving those moves the lot.
    g_nBoardRoot = 0;
    for (int i = 0; i < n && g_nBoardRoot < 4; i++) {
        if (!strstr(g_bones[i].name, "skateskel")) continue;
        const int pa = g_bones[i].parent;
        if (pa < 0 || !strstr(g_bones[pa].name, "skateskel")) g_boardRoot[g_nBoardRoot++] = i;
    }
    g_truckF = AnyNamed("truck_front"); g_truckB = AnyNamed("truck_back"); g_flipper = AnyNamed("skateskel_flipper");
    { static const char* const kW[4] = { "wheel_fl", "wheel_fr", "wheel_bl", "wheel_br" }; for (int w = 0; w < 4; w++) g_wheel[w] = AnyNamed(kW[w]); }
    return g_rigOk;
}
void ReadRefPose(const uint8_t* refSkeleton, int n);      // below, with the pose buffers it needs
// Read at the START OF EVERY EMOTE, never cached by pointer: a change of clothes merges a new skeletal
// mesh, and it can land on the old one's address with a different bone table.
bool ResolveRig(void* meshComp) {
    g_rigOk = false; g_legsOk = false; g_nBones = 0;
    void* skel = twkP(meshComp, SKM_MESH);
    if (!skel) return false;
    const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
    const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
    const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
    if (!info || n <= 0 || n > MAX_BONES) { TwkLog("[emote] rig: %d bones -- unusable", n); return false; }
    for (int i = 0; i < n; i++) {
        char nb[96];
        if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) snprintf(nb, sizeof(nb), "bone%d", i);
        for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        snprintf(g_bones[i].name, sizeof(g_bones[i].name), "%s", nb);
        g_boneName[i] = *(const uint64_t*)(info + i * 12);
        g_bones[i].parent = *(const int*)(info + i * 12 + 8);
        if (g_bones[i].parent >= i) g_bones[i].parent = -1;      // parents precede children, or it is not a tree we can walk
    }
    g_nBones = n;
    ResolveNames();
    ReadRefPose(rs, n);
    static bool said = false;
    if (!said || !g_rigOk) {
        said = true;
        TwkLog("[emote] rig: %d bones | spine x%d, neck %s, head %s | arms L %s > %s > %s, R %s > %s > %s | fingers L %s R %s | legs %s%s",
               n, g_nSpine, BN(g_neck), BN(g_head), BN(g_uarm[0]), BN(g_larm[0]), BN(g_hand[0]), BN(g_uarm[1]), BN(g_larm[1]), BN(g_hand[1]),
               g_fingersOk[0] ? "ok" : "NOT FOUND (hands keep their animated shape)", g_fingersOk[1] ? "ok" : "NOT FOUND",
               g_legsOk ? "ok" : "not found (no lower body)", g_rigOk ? "" : " -- ARMS OR SPINE UNRESOLVED, emotes off");
        TwkLog("[emote] rig: board bones x%d (top %s), trucks %s / %s, flipper %s", g_nBoardRoot, g_nBoardRoot ? BN(g_boardRoot[0]) : "-",
               BN(g_truckF), BN(g_truckB), BN(g_flipper));
    }
    return g_rigOk;
}

// ------------------------------------------------------------------ the pose buffers, and how a bone is turned
struct TF { Q4 q; V3 p; V3 s; };
TF   g_C[MAX_BONES], g_L[MAX_BONES], g_Ct[MAX_BONES], g_Lt[MAX_BONES];
bool g_inSub[MAX_BONES];

void LocalOf(int i, const TF* comp, TF* loc) {
    const int p = g_bones[i].parent;
    loc[i].s = comp[i].s;
    if (p < 0) { loc[i].q = comp[i].q; loc[i].p = comp[i].p; return; }
    loc[i].q = qmul(qconj(comp[p].q), comp[i].q);
    const V3 d = qinv(comp[p].q, sub(comp[i].p, comp[p].p));
    const V3 ps = comp[p].s;
    loc[i].p = v3(d.x / (fabsf(ps.x) > 1e-6f ? ps.x : 1.0f), d.y / (fabsf(ps.y) > 1e-6f ? ps.y : 1.0f), d.z / (fabsf(ps.z) > 1e-6f ? ps.z : 1.0f));
}
void CompOf(int i, const TF* loc, TF* comp) {
    const int p = g_bones[i].parent;
    comp[i].s = loc[i].s;
    if (p < 0) { comp[i].q = loc[i].q; comp[i].p = loc[i].p; return; }
    comp[i].q = qmul(comp[p].q, loc[i].q);
    comp[i].p = add(comp[p].p, qrot(comp[p].q, mulv(loc[i].p, comp[p].s)));
}
void RebuildSub(int i) {          // g_Lt -> g_Ct for everything under (not including) bone i
    for (int j = 0; j < g_nBones; j++) g_inSub[j] = false;
    g_inSub[i] = true;
    for (int j = i + 1; j < g_nBones; j++) {
        const int p = g_bones[j].parent;
        if (p >= 0 && g_inSub[p]) { CompOf(j, g_Lt, g_Ct); g_inSub[j] = true; }
    }
}
void SetComp(int i, Q4 q, V3 p) { g_Ct[i].q = q; g_Ct[i].p = p; LocalOf(i, g_Ct, g_Lt); RebuildSub(i); }
void TurnComp(int i, Q4 q) { if (i >= 0) SetComp(i, qmul(q, g_Ct[i].q), g_Ct[i].p); }
void AimBone(int i, V3 from, V3 to) { if (i >= 0 && len(from) > 1e-3f && len(to) > 1e-3f) TurnComp(i, qfromto(from, to)); }
// WHICH FACE OF THE DECK IS ITS TOP. Asked of the REFERENCE pose, where the board lies flat under the feet and
// its top is simply UP, and kept in the FLIPPER bone's own frame -- the bone the real board is hung from
// (PlaceInHand: GetFlipperSocketName) -- so it can be read back off that bone however the carry has turned it.
// THE FIELD'S LESSON: this used to go by the foot anchors, and those are ANIMATED. In the carry they sit off to
// the deck's side, so the tap held the board a quarter turn round: wheels to the leg, graphic to nobody.
TF   g_R[MAX_BONES];
V3   g_deckTopL = { 0.0f, 0.0f, 1.0f }; int g_deckTopBone = -1; bool g_deckTopOk = false;
float g_deckRise = 4.0f;             // the deck's surface over the truck line, cm (measured there too)
int   g_deckTopSrc = 0;              // who said so: 0 the anchors' guess, 1 the reference pose, 2 measured off the drawn board
void DeckTopFromRef(const TF* ref, bool say) {
    g_deckTopOk = false;
    g_deckTopBone = g_flipper >= 0 ? g_flipper : (g_truckF >= 0 ? g_bones[g_truckF].parent : -1);
    if (g_deckTopBone < 0 || g_truckF < 0 || g_truckB < 0) return;
    V3 ax = sub(ref[g_truckF].p, ref[g_truckB].p);
    if (len(ax) < 5.0f) { if (say) TwkLog("[emote] rig: the reference pose has the board's bones in a heap (trucks %.1f cm apart) -- it says nothing about the deck", len(ax)); return; }
    ax = norm(ax);
    if (fabsf(ax.z) > 0.5f) { if (say) TwkLog("[emote] rig: the board does not lie flat in the reference pose (%.2f) -- deck top by the anchors", ax.z); return; }
    V3 top = norm(sub(v3(0.0f, 0.0f, 1.0f), mul(ax, ax.z)));
    // wheels are UNDER a deck: a reference pose with them over the flipper has the board on its back
    static const char* const kWheels[4] = { "wheel_fl", "wheel_fr", "wheel_bl", "wheel_br" };
    V3 wm = v3(0, 0, 0); int k = 0;
    for (int i = 0; i < 4; i++) { const int w = AnyNamed(kWheels[i]); if (w >= 0) { wm = add(wm, ref[w].p); k++; } }
    const float under = k ? dot(sub(ref[g_deckTopBone].p, mul(wm, 1.0f / (float)k)), top) : 0.0f;
    if (under < -1.0f) top = mul(top, -1.0f);
    g_deckTopL = qinv(ref[g_deckTopBone].q, top);
    g_deckTopOk = true; g_deckTopSrc = 1;
    // ...and how far the deck's SURFACE is over the truck line: there, the foot anchors are where feet stand
    const int fa = AnyNamed("foot_anchor_l"), fb = AnyNamed("foot_anchor_r");
    float rise = -1.0f;
    if (fa >= 0 && fb >= 0) rise = dot(sub(mul(add(ref[fa].p, ref[fb].p), 0.5f), mul(add(ref[g_truckF].p, ref[g_truckB].p), 0.5f)), top);
    g_deckRise = (rise > 0.5f && rise < 14.0f) ? rise : 4.0f;
    if (say) TwkLog("[emote] rig: deck top from the reference pose -- board flat (%.2f), wheels %.1f cm under %s%s, top in its frame (%.2f %.2f %.2f), feet stand %.1f cm over the truck line%s",
                    ax.z, under, g_bones[g_deckTopBone].name, under < -1.0f ? " = ON ITS BACK, turned over" : "", g_deckTopL.x, g_deckTopL.y, g_deckTopL.z,
                    rise, g_deckRise == rise ? "" : " (not believed: 4 used)");
}
// THE BOARD YOU SEE, MEASURED. The field twice over: the foot anchors lie in the idle carry (a quarter turn),
// and on the real skeleton the reference pose has the board's bones piled on one point, so that said nothing.
// But the board that is DRAWN is an actor of separate static meshes -- a deck, two trucks, four wheels --
// each with a place in the world, put there from these bones by PlaceInHand last frame. So what the top of
// the deck is, in the flipper bone's frame, is simply READ: square to the trucks' line and to an axle, on the
// side the deck is of its wheels. Positions only; no mesh's idea of which axis is up is trusted for it.
// `off` is how far the drawn board sits from the bones' (PlaceInHand scales and shifts the socket; a rig is
// free to) -- the tap puts the DRAWN board in the hand, not the bones.
struct Vis { Q4 meshQ; V3 meshP; float meshS; Q4 deckQ; V3 deck, truckF, truckB, wheel[4]; };      // world; wheels BL BR FL FR
V3    g_visOff = { 0.0f, 0.0f, 0.0f };        // drawn minus bones, component space
int   g_visOdd = 0;                           // frames running that the measurement has disagreed with what is kept
float g_visAxisOff = 0.0f, g_visSignH = 0.0f, g_visSignZ = 0.0f, g_visRise = 0.0f;      // kept to be said
bool  g_visSay = false;
bool MeasureFromVisible(const Vis& v, const TF* bones, bool snap, bool still) {
    if (g_flipper < 0 || g_truckF < 0 || g_truckB < 0 || v.meshS < 0.01f) return false;
    auto cs = [&](V3 w) { return mul(qinv(v.meshQ, sub(w, v.meshP)), 1.0f / v.meshS); };
    const V3 tf_ = cs(v.truckF), tb_ = cs(v.truckB), dk = cs(v.deck);
    V3 ax = sub(tf_, tb_);
    if (len(ax) < 5.0f) return false;
    ax = norm(ax);
    V3 n = cross(ax, sub(cs(v.wheel[3]), cs(v.wheel[2])));
    if (len(n) < 1.0f) return false;
    n = norm(n);
    V3 wm = v3(0, 0, 0);
    for (int i = 0; i < 4; i++) wm = add(wm, mul(cs(v.wheel[i]), 0.25f));
    const float h = dot(sub(dk, wm), n), z = dot(qrot(qmul(qconj(v.meshQ), v.deckQ), v3(0.0f, 0.0f, 1.0f)), n);
    float sgn = 0.0f;
    if (fabsf(h) > 1.0f) sgn = h > 0.0f ? 1.0f : -1.0f;               // the deck is OVER its wheels
    else if (fabsf(z) > 0.5f) sgn = z > 0.0f ? 1.0f : -1.0f;          // ...or, level with them, the deck mesh's own up
    if (sgn == 0.0f) return false;
    const V3 top = mul(n, sgn);
    const V3 bax = sub(bones[g_truckF].p, bones[g_truckB].p);
    if (len(bax) < 5.0f) return false;
    g_visAxisOff = acosf(clampf(dot(norm(bax), ax), -1.0f, 1.0f)) * 57.2957795f;
    g_visSignH = h; g_visSignZ = z;
    const V3 topL = qinv(bones[g_flipper].q, top);
    const V3 off = mul(add(sub(tf_, bones[g_truckF].p), sub(tb_, bones[g_truckB].p)), 0.5f);
    const float rise = dot(sub(dk, mul(add(tf_, tb_), 0.5f)), top);
    const bool first = g_deckTopSrc != 2;
    // a rig constant, so it is eased, not chased -- unless it has plainly CHANGED, several frames running
    if (!first && !snap && dot(topL, g_deckTopL) < 0.85f) { if (++g_visOdd < 5) return true; }
    const float a = (first || snap || g_visOdd >= 5) ? 1.0f : 0.2f;
    if (g_visOdd >= 5) g_visSay = true;                                // it CHANGED under us (another carry animation?): worth a line
    g_visOdd = 0;
    g_deckTopL = norm(lerp(g_deckTopL, topL, a));
    g_deckTopBone = g_flipper; g_deckTopOk = true; g_deckTopSrc = 2;
    // where it sits is only believed from a body standing still (a moving one's mesh has moved on since the
    // board was placed), or when there is nothing yet to go by
    if ((still || first || snap) && len(off) < 30.0f) g_visOff = lerp(g_visOff, off, (first || snap) ? 1.0f : 0.2f);
    g_visRise = rise;
    if (rise > 0.5f && rise < 14.0f) g_deckRise = rise;
    return true;
}
bool ReadC2W(void* comp, Q4* q, V3* p, float* s) {
    if (!comp) return false;
    const float x = twkF(comp, SC_C2W), y = twkF(comp, SC_C2W + 4), z = twkF(comp, SC_C2W + 8), w = twkF(comp, SC_C2W + 12);
    const float n2 = x * x + y * y + z * z + w * w;
    if (!(n2 > 0.9f && n2 < 1.1f)) return false;                       // the safe reader's fault value is not a rotation
    *q = q4(x, y, z, w);
    *p = v3(twkF(comp, SC_C2W + 0x10), twkF(comp, SC_C2W + 0x14), twkF(comp, SC_C2W + 0x18));
    if (s) *s = twkF(comp, SC_C2W + 0x20);
    return true;
}
void MeasureBoard(void* sk, void* mesh, bool snap) {
    void* board = twkP(sk, SK_BOARD);
    if (!board || !mesh || !g_rigOk) return;
    Vis v; Q4 q; float s;
    if (!ReadC2W(mesh, &v.meshQ, &v.meshP, &v.meshS)) return;
    if (!ReadC2W(twkP(board, BD_FLIPPER), &v.deckQ, &v.deck, &s)) return;
    if (!ReadC2W(twkP(board, BD_TRUCK_F), &q, &v.truckF, &s) || !ReadC2W(twkP(board, BD_TRUCK_B), &q, &v.truckB, &s)) return;
    for (int i = 0; i < 4; i++) if (!ReadC2W(twkP(board, BD_WHEEL_BL + i * 8), &q, &v.wheel[i], &s)) return;
    // the pose the board was hung from: the mesh's READ buffer
    const int ridx = twkI(mesh, SKM_READ);
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + (ridx == 1 ? 0x10 : 0);
    const float* data = *(const float* const*)arr;
    const int n = *(const int*)(arr + 8);
    if (!data || n != g_nBones) return;
    static TF bones[MAX_BONES];
    const int want[3] = { g_flipper, g_truckF, g_truckB };
    for (int k = 0; k < 3; k++) {
        const int i = want[k];
        if (i < 0) return;
        const float* t = data + (size_t)i * 12;
        bones[i].q = q4(t[0], t[1], t[2], t[3]); bones[i].p = v3(t[4], t[5], t[6]);
    }
    void* mv = twkP(sk, CH_MOVE);
    const float vx = mv ? twkF(mv, MOVE_VEL) : 0.0f, vy = mv ? twkF(mv, MOVE_VEL + 4) : 0.0f;
    const bool still = fabsf(vx) < 15.0f && fabsf(vy) < 15.0f && fabsf(g_tapVel) < 25.0f;
    const bool ok = MeasureFromVisible(v, bones, snap, still);
    if (g_visSay) {
        g_visSay = false;
        void* bm = twkP(board, BD_MOVECOMP);
        if (!ok) TwkLog("[emote] board: the drawn board could not be measured -- deck top by %s", g_deckTopSrc == 1 ? "the reference pose" : "the anchors");
        else TwkLog("[emote] board: measured off the DRAWN board -- deck top in the flipper's frame (%.2f %.2f %.2f) [deck %.1f cm over its wheels, its own up says %.2f], "
                    "drawn vs bones: line %.1f deg apart, sits (%.1f %.1f %.1f) cm off, deck %.1f cm over the truck line | PlaceInHand: location x%.2f + (%.1f %.1f %.1f), rotation x%.2f, reversed %d",
                    g_deckTopL.x, g_deckTopL.y, g_deckTopL.z, g_visSignH, g_visSignZ, g_visAxisOff, g_visOff.x, g_visOff.y, g_visOff.z, g_visRise,
                    bm ? twkF(bm, BM_FOOT_LOCSC) : -1.0f, bm ? twkF(bm, BM_FOOT_RELOF) : 0.0f, bm ? twkF(bm, BM_FOOT_RELOF + 4) : 0.0f, bm ? twkF(bm, BM_FOOT_RELOF + 8) : 0.0f,
                    bm ? twkF(bm, BM_FOOT_ROTSC) : -1.0f, bm ? twkB(bm, BM_HAND_REVRS) : -1);
    }
}
void ReadRefPose(const uint8_t* rs, int n) {
    g_deckTopOk = false; g_deckTopSrc = 0; g_visOff = v3(0.0f, 0.0f, 0.0f); g_visOdd = 0; g_deckRise = 4.0f;
    const float* pose = *(const float* const*)(rs + RS_FINAL_POSE);
    const int np = *(const int*)(rs + RS_FINAL_POSE + 8);
    static bool said = false;
    if (!pose || np != n) { if (!said) { said = true; TwkLog("[emote] rig: no reference pose to read (%d of %d) -- deck top by the anchors", np, n); } return; }
    static TF loc[MAX_BONES];
    for (int i = 0; i < n; i++) {
        const float* t = pose + (size_t)i * 12;
        loc[i].q = q4(t[0], t[1], t[2], t[3]); loc[i].p = v3(t[4], t[5], t[6]); loc[i].s = v3(t[8], t[9], t[10]);
        CompOf(i, loc, g_R);
    }
    DeckTopFromRef(g_R, !said);
    said = true;
}
// The middle joint of a two-bone chain from H to F with lengths a and b, bending toward pole.
V3 MidJoint(V3 H, V3 F, float a, float b, V3 pole) {
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

// ------------------------------------------------------------------ the body's frames
V3    U, BR, BF;                  // component space: up, and the body's right and forward off the HIPS (level)
V3    tu, tr, tf;                 // the CHEST'S own frame, off the posed spine and shoulders: what a hand is placed in
float g_arm[2] = { 55.0f, 55.0f }, SC = 1.0f;     // arm length per side, and the body's size against a 55 cm arm

void BodyFrame(const void* mesh) {
    const float* t = (const float*)((const uint8_t*)mesh + SC_C2W);
    U = norm(qinv(q4(t[0], t[1], t[2], t[3]), v3(0.0f, 0.0f, 1.0f)));
    V3 r = (g_thigh[0] >= 0 && g_thigh[1] >= 0) ? sub(g_Ct[g_thigh[1]].p, g_Ct[g_thigh[0]].p) : sub(g_Ct[g_uarm[1]].p, g_Ct[g_uarm[0]].p);
    r = sub(r, mul(U, dot(r, U)));
    BR = norm(r);
    BF = norm(cross(BR, U));
    for (int sd = 0; sd < 2; sd++) {
        const float a = len(sub(g_Ct[g_larm[sd]].p, g_Ct[g_uarm[sd]].p)) + len(sub(g_Ct[g_hand[sd]].p, g_Ct[g_larm[sd]].p));
        g_arm[sd] = a > 10.0f ? a : 55.0f;
    }
    SC = (g_arm[0] + g_arm[1]) * 0.5f / 55.0f;
}
void TorsoFrame() {
    tu = U; tr = BR; tf = BF;
    const int top = g_neck >= 0 ? g_neck : g_spine[g_nSpine - 1];
    const V3 up = sub(g_Ct[top].p, g_Ct[g_pelvis].p);
    if (len(up) < 1e-3f) return;
    tu = norm(up);
    V3 r = sub(g_Ct[g_uarm[1]].p, g_Ct[g_uarm[0]].p);
    r = sub(r, mul(tu, dot(r, tu)));
    if (len(r) < 1e-3f) return;
    tr = norm(r);
    tf = norm(cross(tr, tu));
}
float Sg(int sd) { return sd ? 1.0f : -1.0f; }
// A point in the chest's frame, in ARM LENGTHS from the shoulder: forward, OUTWARD (away from the middle,
// so the same numbers serve either arm), up.
V3 P(int sd, float fwd, float out, float up) {
    const float L = g_arm[sd];
    return add(g_Ct[g_uarm[sd]].p, mix3(tf, fwd * L, tr, Sg(sd) * out * L, tu, up * L));
}
V3 Out(int sd) { return mul(tr, Sg(sd)); }

// ------------------------------------------------------------------ the moves
// Degrees, shared out over the spine: pitch + = forward, lean + = to the RIGHT, yaw + = to the right.
void SpineLean(float pitch, float leanRight, float yawRight) {
    if (g_nSpine <= 0) return;
    const float k = 1.0f / (float)g_nSpine;
    const Q4 q = qmul(qaxis(U, yawRight * k), qmul(qaxis(BR, pitch * k), qaxis(BF, -leanRight * k)));
    for (int i = 0; i < g_nSpine; i++) TurnComp(g_spine[i], q);
}
// Degrees off the chest: yaw + = right, nod + = DOWN, tilt + = toward the right shoulder. The neck takes a
// third and the head the rest, each a rigid turn of the bone and what hangs off it. Returns the whole turn.
Q4 HeadTurn(float yawRight, float nodDown, float tiltRight) {
    const int hb = g_head >= 0 ? g_head : g_neck;
    auto part = [&](float f) { return qmul(qaxis(tu, yawRight * f), qmul(qaxis(tr, nodDown * f), qaxis(tf, -tiltRight * f))); };
    if (g_neck >= 0 && g_neck != hb) { TurnComp(g_neck, part(0.35f)); TurnComp(hb, part(0.65f)); }
    else TurnComp(hb, part(1.0f));
    return part(1.0f);
}
// A shoulder lifts with a raised arm: the collarbone is aimed a little toward the chest's up.
void RaiseShoulder(int sd, float amount) {
    const int c = g_clav[sd];
    if (c < 0 || amount <= 0.0f) return;
    const V3 cur = sub(g_Ct[g_uarm[sd]].p, g_Ct[c].p);
    if (len(cur) < 1e-3f) return;
    AimBone(c, cur, add(norm(cur), mul(tu, amount)));
}
// ...and swings toward a reach, level: only the turn, never the height (sit.cpp's ShoulderFollow, and why).
void ShoulderFollow(int sd, V3 handTarget, float amount) {
    const int c = g_clav[sd];
    if (c < 0 || amount <= 0.0f) return;
    const V3 cur = sub(g_Ct[g_uarm[sd]].p, g_Ct[c].p);
    V3 want = sub(handTarget, g_Ct[c].p);
    if (len(cur) < 1e-3f || len(want) < 1e-3f) return;
    want = sub(want, mul(tu, dot(want, tu)));
    if (len(want) < 1e-3f) return;
    want = add(norm(want), mul(tu, dot(norm(cur), tu)));
    TurnComp(c, qblend(qid(), qfromto(norm(cur), norm(want)), clampf(amount, 0.0f, 1.0f)));
}
void ArmTo(int sd, V3 wrist, V3 pole, float follow) {
    const int ua = g_uarm[sd], la = g_larm[sd], ha = g_hand[sd];
    ShoulderFollow(sd, wrist, follow);
    const V3 S = g_Ct[ua].p;
    const float a1 = len(sub(g_Ct[la].p, S)), a2 = len(sub(g_Ct[ha].p, g_Ct[la].p));
    V3 d = sub(wrist, S); const float dl = len(d);
    if (dl > (a1 + a2) * 0.98f) wrist = add(S, mul(d, (a1 + a2) * 0.98f / dl));
    const V3 E = MidJoint(S, wrist, a1, a2, pole);
    AimBone(ua, sub(g_Ct[la].p, S), sub(E, S));
    AimBone(la, sub(g_Ct[ha].p, g_Ct[la].p), sub(wrist, g_Ct[la].p));
}
// The hand as it is posed right now: which way the fingers run, which way the PALM faces, and the line
// from the index finger's knuckle to the little finger's. See the header for why the palm's sign is what it is.
bool HandAxes(int sd, V3* F, V3* N, V3* A) {
    if (!g_fingersOk[sd]) return false;
    const V3 h = g_Ct[g_hand[sd]].p;
    V3 mean = v3(0, 0, 0); int k = 0;
    for (int f = 1; f <= 4; f++) if (g_fing[sd][f][0] >= 0) { mean = add(mean, g_Ct[g_fing[sd][f][0]].p); k++; }
    if (!k) return false;
    const V3 fd = sub(mul(mean, 1.0f / (float)k), h);
    if (len(fd) < 1e-3f) return false;
    *F = norm(fd);
    V3 a = sub(g_Ct[g_fing[sd][4][0]].p, g_Ct[g_fing[sd][1][0]].p);
    a = sub(a, mul(*F, dot(a, *F)));
    if (len(a) < 1e-3f) return false;
    *A = norm(a);
    *N = mul(cross(*F, *A), sd ? -1.0f : 1.0f);
    return true;
}
// Fingers along one direction, palm facing another (only its part square to the fingers counts).
void HandPose(int sd, V3 fingersWant, V3 palmWant) {
    V3 F, N, A;
    if (!HandAxes(sd, &F, &N, &A) || len(fingersWant) < 1e-4f) return;
    const V3 f1 = norm(fingersWant);
    Q4 r = qfromto(F, f1);
    V3 n1 = sub(palmWant, mul(f1, dot(palmWant, f1)));
    if (len(n1) > 1e-3f) {
        n1 = norm(n1);
        const V3 n0 = norm(qrot(r, N));
        // a pure roll about the fingers, by the signed angle between the two: never the 180-degree case of
        // a shortest-arc rotation, whose axis is anybody's guess and need not be the fingers at all
        const float ang = atan2f(dot(cross(n0, n1), f1), dot(n0, n1));
        r = qmul(qaxis(f1, ang * 57.2957795f), r);
    }
    TurnComp(g_hand[sd], r);
}
enum { TH_KEEP = 0, TH_OUT, TH_IN, TH_OPEN };
// index, middle, ring, little: 0 straight .. 1 closed into a fist. `spread` fans them; `littleOut` fans the
// little finger further. The thumb is left alone, stuck up (a thumbs-up), folded ACROSS the fist, or held
// open beside a flat hand.
// THE FOLDED THUMB IS SOLVED ONTO THE FINGERS, NOT AIMED. Two rounds of aiming it failed in the field: first
// mostly OUT of the palm (with a palm facing down, a thumb pointing at the ground), then along the hand and
// across it -- better, but on the real rig it ran the whole height of the fist and out past the little
// finger, because the real thumb is longer than any number guessed for it. A direction cannot know how long
// the bones are; a TARGET can. So the thumb's first two bones are a two-bone reach that puts its last
// KNUCKLE on the first curled finger's middle bone -- the index finger of a fist, the middle finger of a
// pointing hand -- just proud of it on the palm side, and the tip is then turned on toward the next finger
// along. Whatever the rig's proportions, the thumb ends up lying on the fingers.
void Fingers(int sd, int thumb, float c1, float c2, float c3, float c4, float spread, float littleOut) {
    V3 F, N, A;
    if (!HandAxes(sd, &F, &N, &A)) return;
    static const float kFan[5] = { 0.0f, -0.11f, -0.03f, 0.05f, 0.13f };
    const float curl[5] = { 0.0f, c1, c2, c3, c4 };
    for (int f = 1; f <= 4; f++) {
        const int j0 = g_fing[sd][f][0], j1 = g_fing[sd][f][1], j2 = g_fing[sd][f][2];
        if (j0 < 0 || j1 < 0) continue;
        const V3 B = norm(add(F, mul(A, kFan[f] * spread + (f == 4 ? littleOut : 0.0f))));
        const V3 Nk = norm(sub(N, mul(B, dot(N, B))));
        const float c = clampf(curl[f], 0.0f, 1.0f);
        const float a1 = c * 80.0f * 0.0174532925f, a2 = a1 + c * 100.0f * 0.0174532925f;
        AimBone(j0, sub(g_Ct[j1].p, g_Ct[j0].p), add(mul(B, cosf(a1)), mul(Nk, sinf(a1))));
        if (j2 >= 0) {
            AimBone(j1, sub(g_Ct[j2].p, g_Ct[j1].p), add(mul(B, cosf(a2)), mul(Nk, sinf(a2))));
            // the tip has no child to aim at: it is turned about the finger's own hinge, which closes
            // toward the palm for a positive angle (the axis is direction x palm)
            if (c > 0.02f) TurnComp(j2, qaxis(cross(B, Nk), c * 65.0f));
        }
    }
    const int t0 = g_fing[sd][0][0], t1 = g_fing[sd][0][1], t2 = g_fing[sd][0][2];
    if (thumb == TH_KEEP || t0 < 0 || t1 < 0) return;
    if (thumb == TH_IN && t2 >= 0) {
        int on = 0;                                         // the first finger that is closed: what the thumb lies on
        for (int f = 1; f <= 4 && !on; f++) if (curl[f] > 0.5f && g_fing[sd][f][1] >= 0 && g_fing[sd][f][2] >= 0) on = f;
        if (on) {
            auto midBone = [&](int f) { return mul(add(g_Ct[g_fing[sd][f][1]].p, g_Ct[g_fing[sd][f][2]].p), 0.5f); };
            const V3 knuckle = add(midBone(on), mix3(N, 1.3f * SC, A, -0.4f * SC, F, 0.0f));      // just proud of it, a touch to the thumb's side
            const V3 H = g_Ct[t0].p;
            const float a = len(sub(g_Ct[t1].p, H)), b = len(sub(g_Ct[t2].p, g_Ct[t1].p));
            // Whatever length the thumb has to spare folds out at the SIDE of the fist, where a thumb's ball is --
            // not toward the palm, which with the palm down is a knuckle poking out underneath the fist.
            const V3 M = MidJoint(H, knuckle, a, b, mix3(A, -0.85f, F, 0.40f, N, 0.20f));
            AimBone(t0, sub(g_Ct[t1].p, H), sub(M, H));
            AimBone(t1, sub(g_Ct[t2].p, g_Ct[t1].p), sub(knuckle, g_Ct[t1].p));
            // the tip has no child to measure, so it is taken to run on from the bone before it, and turned
            // from there toward the next finger along, pressed a little into the fist
            const int next = on < 4 && g_fing[sd][on + 1][1] >= 0 && g_fing[sd][on + 1][2] >= 0 ? on + 1 : on;
            const V3 to = sub(add(midBone(next), mix3(N, 0.5f * SC, A, next == on ? 2.0f * SC : 0.0f, F, 0.0f)), g_Ct[t2].p);
            AimBone(t2, sub(g_Ct[t2].p, g_Ct[t1].p), to);
            return;
        }
        thumb = TH_OPEN;                                    // nothing closed to lie on: an open hand's thumb
    }
    V3 e0, e1;
    if (thumb == TH_OUT)       { e0 = mix3(A, -0.85f, F, 0.45f, N, -0.10f); e1 = mix3(A, -0.90f, F, 0.40f, N, 0.00f); }
    else                       { e0 = mix3(A, -0.74f, F, 0.62f, N,  0.15f); e1 = mix3(A, -0.66f, F, 0.72f, N, 0.10f); }   // open: a relaxed 45 degrees off the index
    AimBone(t0, sub(g_Ct[t1].p, g_Ct[t0].p), e0);
    if (t2 >= 0) AimBone(t1, sub(g_Ct[t2].p, g_Ct[t1].p), e1);
}
// A THUMB ANYWHERE BETWEEN OPEN AND FOLDED, for a hand that is holding something rather than making a
// fist or lying flat -- the two the modes above give. `t` 0 is the open thumb's pose, 1 is TH_IN's fold
// onto `on`'s middle bone, and in between the TARGET is what moves, never the aim: pointing a thumb by
// direction is the dead end written about over Fingers(), and it is no less dead halfway.
// Call after Fingers(..., TH_KEEP, ...), which leaves the thumb alone.
void ThumbBlend(int sd, int on, float t) {
    const int t0 = g_fing[sd][0][0], t1 = g_fing[sd][0][1], t2 = g_fing[sd][0][2];
    if (t0 < 0 || t1 < 0 || t2 < 0) return;
    if (on < 1 || on > 4 || g_fing[sd][on][1] < 0 || g_fing[sd][on][2] < 0) return;
    V3 F, N, A;
    if (!HandAxes(sd, &F, &N, &A)) return;
    t = clampf(t, 0.0f, 1.0f);
    const V3 H = g_Ct[t0].p;
    const float a = len(sub(g_Ct[t1].p, H)), b = len(sub(g_Ct[t2].p, g_Ct[t1].p));
    const V3 mid = mul(add(g_Ct[g_fing[sd][on][1]].p, g_Ct[g_fing[sd][on][2]].p), 0.5f);
    const V3 folded = add(mid, mix3(N, 1.3f * SC, A, -0.4f * SC, F, 0.0f));      // just proud of the finger
    const V3 open   = add(H, mul(norm(mix3(A, -0.74f, F, 0.62f, N, 0.15f)), (a + b) * 0.95f));
    const V3 want   = add(add(mul(open, 1.0f - t), mul(folded, t)),
                          mix3(F, g_tapThumbAlong * SC, N, g_tapThumbOut * SC, A, g_tapThumbAcross * SC));
    const V3 M = MidJoint(H, want, a, b, mix3(A, -0.85f, F, 0.40f, N, 0.20f));
    AimBone(t0, sub(g_Ct[t1].p, H), sub(M, H));
    AimBone(t1, sub(g_Ct[t2].p, g_Ct[t1].p), sub(want, g_Ct[t1].p));
    // the tip has no child to measure: folded it turns on toward the next finger, open it runs straight on
    const int next = (on < 4 && g_fing[sd][on + 1][1] >= 0 && g_fing[sd][on + 1][2] >= 0) ? on + 1 : on;
    const V3 nextMid = mul(add(g_Ct[g_fing[sd][next][1]].p, g_Ct[g_fing[sd][next][2]].p), 0.5f);
    const V3 toFold = add(nextMid, mix3(N, 0.5f * SC, A, next == on ? 2.0f * SC : 0.0f, F, 0.0f));
    const V3 run    = norm(sub(g_Ct[t2].p, g_Ct[t1].p));
    const V3 toOpen = add(g_Ct[t2].p, mul(run, 3.0f * SC));
    const V3 to = add(mul(toOpen, 1.0f - t), mul(toFold, t));
    AimBone(t2, sub(g_Ct[t2].p, g_Ct[t1].p), sub(to, g_Ct[t2].p));
}
// A FINGER'S TIP MOVED OFF WHERE ITS CURL PUT IT, in the hand's own terms (cm along the fingers, out
// through the palm, across toward the thumb). The same two-bone reach the thumb gets, and for the same
// reason: a fingertip is a PLACE, and the joint angles that put it there are arithmetic, not a guess.
// The curl still decides the shape -- the bend's existing plane is handed in as the pole, so the finger
// keeps the look the curl gave it and only reaches further or nearer. Call after Fingers().
void FingerNudge(int sd, int f, float along, float out, float across) {
    if (fabsf(along) < 0.01f && fabsf(out) < 0.01f && fabsf(across) < 0.01f) return;
    const int j0 = g_fing[sd][f][0], j1 = g_fing[sd][f][1], j2 = g_fing[sd][f][2];
    if (j0 < 0 || j1 < 0 || j2 < 0) return;
    V3 F, N, A;
    if (!HandAxes(sd, &F, &N, &A)) return;
    const V3 H = g_Ct[j0].p;
    const float a = len(sub(g_Ct[j1].p, H)), b = len(sub(g_Ct[j2].p, g_Ct[j1].p));
    const V3 want = add(g_Ct[j2].p, mix3(F, along * SC, N, out * SC, A, across * SC));
    const V3 pole = sub(g_Ct[j1].p, H);                 // where the curl already has it bending
    const V3 M = MidJoint(H, want, a, b, pole);
    AimBone(j0, sub(g_Ct[j1].p, H), sub(M, H));
    AimBone(j1, sub(g_Ct[j2].p, g_Ct[j1].p), sub(want, g_Ct[j1].p));
}
void OpenHand(int sd, float spread) { Fingers(sd, TH_OPEN, 0.0f, 0.0f, 0.0f, 0.0f, spread, 0.0f); }
void Fist(int sd, int thumb)        { Fingers(sd, thumb, 1.0f, 1.0f, 1.0f, 1.0f, 0.4f, 0.0f); }
int  FreeHand() { return g_carry == 1 ? 0 : 1; }          // the right, unless that is the one with the board
bool Holding(int sd) { return g_carry == sd; }

// ------------------------------------------------------------------ the performances
void DoWave(float t) {
    const int sd = FreeHand(); const float sg = Sg(sd);
    SpineLean(0.0f, -2.5f * sg, 0.0f);
    TorsoFrame();
    HeadTurn(0.0f, -3.0f, 5.0f * sg);
    const float sw = sinf(TAU * 2.3f * t);
    RaiseShoulder(sd, 0.30f);
    ArmTo(sd, P(sd, 0.26f, 0.48f + 0.17f * sw, 0.60f), mix3(Out(sd), 1.0f, tu, -0.8f, tf, -0.15f), 0.4f);
    HandPose(sd, add(tu, mul(Out(sd), 0.30f * sw)), tf);
    OpenHand(sd, 1.3f);
}
void DoThumbs(float t) {
    const int sd = FreeHand();
    TorsoFrame();
    HeadTurn(0.0f, 4.0f * sinf(TAU * 1.2f * t) * expf(-1.5f * t), 0.0f);
    const float pop = expf(-5.0f * t) * sinf(TAU * 2.2f * t) * 0.05f;
    ArmTo(sd, P(sd, 0.74f + pop, 0.10f, 0.04f), mix3(tu, -1.0f, Out(sd), 0.5f, tf, 0.0f), 0.35f);
    HandPose(sd, tf, mul(Out(sd), -1.0f));                 // knuckles forward, palm to the middle: the thumb is on top
    Fist(sd, TH_OUT);
}
void DoPoint(const void* mesh, float t) {
    const int sd = FreeHand(); const float sg = Sg(sd);
    float yaw = 0.0f, pitch = 4.0f, f[3];
    if (CameraHeight_ViewForward(f)) {
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const V3 d = norm(qinv(q4(m[0], m[1], m[2], m[3]), v3(f[0], f[1], f[2])));
        yaw   = atan2f(dot(d, BR), dot(d, BF)) * 57.2957795f;
        pitch = asinf(clampf(dot(d, U), -1.0f, 1.0f)) * 57.2957795f + 6.0f;      // the camera looks a little down on you
    }
    // an arm does not reach far across its own chest, nor behind its owner
    yaw = sd ? clampf(yaw, -35.0f, 85.0f) : clampf(yaw, -85.0f, 35.0f);
    pitch = clampf(pitch, -30.0f, 50.0f);
    if (!g_ptSet) { g_ptYaw = yaw; g_ptPitch = pitch; g_ptSet = true; }
    g_ptYaw += (yaw - g_ptYaw) * 0.18f; g_ptPitch += (pitch - g_ptPitch) * 0.18f;
    SpineLean(0.0f, 0.0f, g_ptYaw * 0.22f);
    TorsoFrame();
    HeadTurn(g_ptYaw * 0.55f, -g_ptPitch * 0.6f, 0.0f);
    const float y = g_ptYaw * 0.0174532925f, p = g_ptPitch * 0.0174532925f;
    const V3 dir = norm(add(mul(add(mul(BF, cosf(y)), mul(BR, sinf(y))), cosf(p)), mul(U, sinf(p))));
    const float jab = 1.0f - 0.10f * expf(-6.0f * t) - 0.015f * (0.5f + 0.5f * sinf(TAU * 0.9f * t));
    RaiseShoulder(sd, 0.12f + 0.2f * clampf(g_ptPitch / 50.0f, 0.0f, 1.0f));
    ArmTo(sd, add(g_Ct[g_uarm[sd]].p, mul(dir, g_arm[sd] * 0.97f * jab)), mix3(tu, -1.0f, tr, 0.4f * sg, tf, 0.0f), 0.45f);
    HandPose(sd, dir, mul(U, -1.0f));
    Fingers(sd, TH_IN, 0.0f, 1.0f, 1.0f, 1.0f, 0.3f, 0.0f);
}
void DoFacepalm(float t) {
    const int sd = FreeHand();
    SpineLean(7.0f, 0.0f, 0.0f);
    TorsoFrame();
    const float tt = fmodf(t, 5.5f);                                     // held until put away: the head shakes again every so often
    const float shake = 7.0f * sinf(TAU * 1.1f * tt) * window(tt, 0.8f, 2.8f, 0.35f);
    const Q4 hq = HeadTurn(shake, 24.0f, 0.0f);
    const int hb = g_head >= 0 ? g_head : g_neck;
    // The face, from the head bone (the top of the neck): forward and up of it, carried round by the head's
    // turn. First round this put the palm INSIDE the brow (the field: "a bit too far into the face", the
    // fingers over the top of a beanie): the face is further out than the bone suggests, a hat and a hood
    // further still, and the hand belongs over the eyes and brow rather than the crown. So: further out,
    // a little lower, and the palm held off it.
    const V3 face = add(g_Ct[hb].p, qrot(hq, add(mul(tf, 13.5f * SC), mul(tu, 7.5f * SC))));
    const V3 fingers = norm(qrot(hq, add(tu, mul(Out(sd), -0.35f))));
    const V3 palm = norm(qrot(hq, mul(tf, -1.0f)));
    RaiseShoulder(sd, 0.20f);
    ArmTo(sd, sub(sub(face, mul(fingers, 8.5f * SC)), mul(palm, 3.5f * SC)), mix3(tu, -1.0f, Out(sd), 0.7f, tf, 0.2f), 0.3f);
    HandPose(sd, fingers, palm);
    Fingers(sd, TH_OPEN, 0.15f, 0.15f, 0.20f, 0.25f, 1.0f, 0.0f);
}
void LegTo(int sd, V3 ankle, V3 pole) {
    const int th = g_thigh[sd], ca = g_calf[sd], ft = g_foot[sd];
    const V3 H = g_Ct[th].p;
    const float a = len(sub(g_Ct[ca].p, H)), b = len(sub(g_Ct[ft].p, g_Ct[ca].p));
    const V3 K = MidJoint(H, ankle, a, b, pole);
    AimBone(th, sub(g_Ct[ca].p, H), sub(K, H));
    AimBone(ca, sub(g_Ct[ft].p, g_Ct[ca].p), sub(ankle, g_Ct[ca].p));
}
// THE FEET STAY WHERE THE GAME PUT THEM. The hips move -- down, sideways, a twist -- and each leg is then
// solved back to the ankle it had (plus a lift), so the knees take it up and nothing slides on the floor.
void Hips(float down, float side, float yawDeg, float liftL, float liftR) {
    if (!g_legsOk) return;
    TF foot[2] = { g_Ct[g_foot[0]], g_Ct[g_foot[1]] };
    SetComp(g_pelvis, qmul(qaxis(U, yawDeg), g_Ct[g_pelvis].q), add(g_Ct[g_pelvis].p, add(mul(U, -down), mul(BR, side))));
    for (int sd = 0; sd < 2; sd++) {
        LegTo(sd, add(foot[sd].p, mul(U, sd ? liftR : liftL)), add(BF, mul(BR, 0.25f * Sg(sd))));
        SetComp(g_foot[sd], foot[sd].q, g_Ct[g_foot[sd]].p);                          // ...still facing the way it was
    }
}
float GroundU() {
    if (!g_legsOk) return dot(g_C[g_pelvis].p, U) - 95.0f * SC;
    const float a = dot(g_C[g_foot[0]].p, U), b = dot(g_C[g_foot[1]].p, U);
    return (a < b ? a : b) - 8.5f * SC;                     // the foot bone is the ANKLE: the floor is an ankle below it
}
// Which way the deck's TOP faces, as the game posed the board: read off the flipper bone with what the
// reference pose said its top is (DeckTopFromRef). Without that, the old guess: toward the foot anchors if
// the rig has them off the truck line, else square to the axle.
V3 DeckTopByAnchors(V3 axis0);
V3 DeckTop(V3 axis0) {
    if (g_deckTopOk && g_deckTopBone >= 0) {
        V3 d = qrot(g_C[g_deckTopBone].q, g_deckTopL);
        d = sub(d, mul(axis0, dot(d, axis0)));
        if (len(d) > 0.3f) return norm(d);
    }
    return DeckTopByAnchors(axis0);
}
V3 DeckTopByAnchors(V3 axis0) {
    const int fa = AnyNamed("foot_anchor_l"), fb = AnyNamed("foot_anchor_r");
    if (fa >= 0 && fb >= 0) {
        V3 d = sub(mul(add(g_C[fa].p, g_C[fb].p), 0.5f), mul(add(g_C[g_truckF].p, g_C[g_truckB].p), 0.5f));
        d = sub(d, mul(axis0, dot(d, axis0)));
        if (len(d) > 0.5f) return norm(d);
    }
    const int wl = AnyNamed("wheel_fl"), wr = AnyNamed("wheel_fr");
    if (wl >= 0 && wr >= 0 && len(sub(g_C[wr].p, g_C[wl].p)) > 1.0f) return norm(cross(axis0, sub(g_C[wr].p, g_C[wl].p)));
    return norm(cross(axis0, BR));
}
// A shoulder let down a little: the collarbone aimed below where it points. A few centimetres, the way an
// arm finds a little more reach without the body doing anything about it.
void DropShoulder(int sd, float cm) {
    const int c = g_clav[sd];
    if (c < 0 || cm <= 0.0f) return;
    const V3 cur = sub(g_Ct[g_uarm[sd]].p, g_Ct[c].p);
    const float l = len(cur);
    if (l < 1e-3f) return;
    AimBone(c, cur, sub(norm(cur), mul(tu, cm / l)));
}
// Where the floor is under the tail, as a height along the body's up: the trace's answer when it has one and
// it is near where the feet say the floor is, else the feet's.
float TapGround() {
    if (g_seated && g_seatSpot) {                    // seated: the surface you sit on, under the tail if the trace found it there
        const float seat = dot(g_seatOs, U);
        return g_tapGroundUSet ? clampf(g_tapGroundU, seat - 20.0f * SC, seat + 20.0f * SC) : seat;
    }
    const float feet = GroundU();
    if (!g_tapGroundUSet) return feet;
    return clampf(g_tapGroundU, feet - 35.0f * SC, feet + 35.0f * SC);
}
void DoTap(float) {
    const int c = g_carry;
    if (c < 0 || g_truckF < 0 || g_truckB < 0 || !g_nBoardRoot) return;
    const int h = g_hand[c]; const float sg = Sg(c);
    // the board as the game posed it: its line (tail to nose), its length, where its nose is, which face is up
    V3 axis0 = sub(g_C[g_truckF].p, g_C[g_truckB].p);
    const float wb = len(axis0);
    if (wb < 5.0f) return;
    axis0 = mul(axis0, 1.0f / wb);
    const V3 nose0 = add(g_C[g_truckF].p, mul(axis0, 0.47f * wb));
    const float L = wb * 1.94f;                                      // nose to tail: a deck runs about half a wheelbase past each truck
    const V3 top0 = DeckTop(axis0);
    TorsoFrame();                                                    // the body stands as the game has it: nothing here moves the spine yet
    // AS HELD (the photograph): the nose in the hanging hand, the TAIL set down OUT AHEAD of the foot and well
    // outside it -- so the board leans back to the hand, and a walking leg swings past it, not through it --
    // the GRAPHIC to the front, so the deck's top faces the skater (turned a touch to their middle); swung
    // by the stick.
    // SEATED it leans out to the carrying side and forward, a third off upright: the tail knocks on the seat's
    // surface beside the thigh and the nose comes to about shoulder height, where a sitting arm can hold it.
    const bool seat = g_seated && g_seatSpot;
    const V3 ax = seat ? norm(mix3(U, -1.0f, BF, 0.45f, BR, 0.55f * sg + 0.22f * g_tapSwing))
                       : norm(mix3(U, -1.0f, BF, 0.26f, BR, 0.05f * sg + 0.22f * g_tapSwing));           // nose -> tail
    V3 top = mix3(BF, -1.0f, BR, -0.12f * sg, U, 0.0f);
    top = norm(sub(top, mul(ax, dot(top, ax))));
    const Q4 r1 = qfromto(axis0, mul(ax, -1.0f));
    const V3 t1 = norm(qrot(r1, top0));
    const float roll = atan2f(dot(cross(t1, top), mul(ax, -1.0f)), dot(t1, top)) * 57.2957795f + g_tapRollDeg;
    const Q4 Rb = qmul(qaxis(mul(ax, -1.0f), roll), r1);
    // THE DECK IS NOT ON THE TRUCK LINE: its surface is over it (by what the reference pose measured), and both
    // ends kick up further. `e` takes a point on the line to the END OF THE WOOD -- what the hand holds at one
    // end and what knocks on the ground at the other.
    const V3 e = mul(top, g_deckRise + 3.0f * SC);
    // where the nose is: ahead of the shoulder and outside it, at the height that puts the tail's end on the ground
    const float hit = g_tapHit;
    const V3 S0 = g_Ct[g_uarm[c]].p;
    V3 Nc = add(S0, seat ? mix3(BF, 0.30f * g_arm[c], BR, 0.36f * g_arm[c] * sg, U, 0.0f)
                         : mix3(BF, 0.22f * g_arm[c], BR, 0.27f * g_arm[c] * sg, U, 0.0f));
    Nc = add(Nc, mul(U, TapGround() - L * dot(ax, U) - dot(e, U) - dot(Nc, U)));
    // THE HAND CLOSES ON THE NOSE'S END: the wrist above the tip, the fingers running straight DOWN the board
    // and curling ROUND the end onto the graphic, so the wood passes through the fist. `f` is the way the
    // fingers run, `p` the way the palm faces; `reach` is wrist to knuckles.
    //
    // WHICH WAY THE HAND IS TURNED took four measured tries, because two of the three things it has to be are
    // in tension and none of them is obvious from reading. It must be ALL of: fingers pointing down (or the
    // wrist is not above the tip and a hanging arm cannot reach it), the palm turned AT the deck (fingers curl
    // toward the palm, so a palm facing away swings them OFF the board -- which is why more curl used to make
    // it worse), and the palm not turned UP (a palm facing the sky is a tray; in the field it read as "the
    // hand is upside down"). On the test body, with the deck's own axes as the measure:
    //
    //   frame         palm.up  palm.fwd   fingers     what it was
    //   0 f0,p0         -0.68     -0.73    down-fwd   ORIGINAL: fingers never crossed the deck
    //   1 f0,-p0        +0.68     +0.73    down-fwd   grips, but the palm is turned up: "upside down"
    //   2 -p0,f0        -0.73     +0.67    up-fwd     grips, but the wrist lands BELOW the tip -- 10.8 cm
    //                                                 out of a hanging arm's reach
    //   3 p0,-f0        +0.73     -0.67    wrong side of everything
    //   4               -0.23     +0.97    straight   holds, but the palm is turned OUT IN FRONT: the field
    //                                      down      called it flipped the wrong way
    //   5 (below)       +0.23     -0.97    straight   SHIPPED
    //                                      down
    //
    // 4 and 5 are mirrors -- the hand gripping from one side of the deck or the other -- and they hold the
    // board equally well, so nothing in the geometry chooses between them. What chooses is how an arm HANGS:
    // at rest the palm faces BACK, which is 5. Both are frames 0/1's pair with the 28 degree tilt laid the
    // other way, so the fingers come off the wrist straight down the deck instead of angling out over the
    // end -- that is what lets the palm turn to the board without rolling the hand over to do it.
    // ...and HOW THE HAND IS TURNED ON TOP OF THAT is three ini keys, one per axis, because which of them is
    // wrong is a thing you can see and not a thing the geometry can be asked. They are applied in this order
    // and each is about the frame the one before it left:
    //   Roll  (EmoteTapHandRollDeg)  about the PALM's normal -- the fingers swing within the palm's own
    //                                plane, straight down the deck at 0 and across it at a quarter turn.
    //   Twist (EmoteTapHandTwistDeg) about the FINGERS -- the palm turns around them like a doorknob:
    //                                back at 0, then toward the body, then forward.
    //   Pitch (EmoteTapHandPitchDeg) about the THUMB's side -- the wrist cocks, tipping the fingers from
    //                                pointing down toward pointing out in front (or behind, negative).
    const float th = 28.0f * 0.01745329f;
    V3 p = norm(mix3(ax, -sinf(th), top, cosf(th), U, 0.0f));
    V3 f = norm(mix3(ax, cosf(th), top, sinf(th), U, 0.0f));
    if (fabsf(g_tapHandRoll)  > 0.5f) f = norm(qrot(qaxis(p, g_tapHandRoll), f));
    if (fabsf(g_tapHandTwist) > 0.5f) p = norm(qrot(qaxis(f, g_tapHandTwist), p));
    if (fabsf(g_tapHandPitch) > 0.5f) {
        const Q4 q = qaxis(norm(cross(f, p)), g_tapHandPitch);
        f = norm(qrot(q, f)); p = norm(qrot(q, p));
    }
    float reach = g_tapReach * SC;
    // WHERE ON THE BOARD THE HAND SITS, as a nudge in the body's own terms (cm, from the F1 page).
    // It has to come off BOTH sides of the solve -- the wrist the arm is sent to, and the place the board
    // is then hung from -- or it moves the hand and the board together and nothing changes between them.
    const V3 gripNudge = mix3(BF, g_tapGripFwd * SC, BR, g_tapGripRight * SC, U, g_tapGripUp * SC);
    auto wristFor = [&](V3 nose, float rc) { return sub(add(nose, e), add(mix3(f, rc, p, g_tapPalmGap * SC, U, 0.0f), gripNudge)); };
    // What a straight arm cannot reach is found where a person finds it, IN THIS ORDER, and never in the
    // knees: the shoulder let down, the grip slid up the board, a few degrees of lean. All of it sized from
    // the CONTACT, so none of it moves while the board does.
    auto shortBy = [&](float rc) { return len(sub(wristFor(Nc, rc), g_Ct[g_uarm[c]].p)) - 0.985f * g_arm[c]; };
    float sh = shortBy(reach), dropped = 0.0f, slid = 0.0f, leaned = 0.0f;
    // THE SHOULDER GOES FIRST AND GOES FURTHER THAN IT USED TO (3 cm), because the SLIDE is what costs the
    // grip: it overstates the wrist-to-knuckle distance, so the hand is placed that much further up the board
    // and ends up closing on the air above the tip -- measured, a 4.5 cm slide left the knuckles 3.3 cm past
    // the nose. A dropped shoulder buys the same reach and nothing in the pose pays for it.
    // ...and it is STEPPED, because one pass of 1.1x the shortfall undershoots: the collarbone aims down from
    // wherever it is now, so letting it down twice more costs nothing and gets the reach out of the shoulder
    // instead of out of the grip.
    for (int i = 0; i < 3 && sh > 0.0f && dropped < g_tapDropMax * SC; i++) {
        const float add = clampf(sh * 1.1f, 0.0f, g_tapDropMax * SC - dropped);
        DropShoulder(c, add); dropped += add; sh = shortBy(reach);
    }
    if (sh > 0.0f) { slid = clampf(sh * 1.8f, 0.0f, g_tapSlideMax * SC); reach += slid; sh = shortBy(reach); }   // along the hand, not up the arm: x1.8
    if (sh > 0.0f) { leaned = clampf(sh * 1.3f, 0.0f, 5.0f); SpineLean(0.0f, leaned * sg, 0.0f); TorsoFrame(); sh = shortBy(reach); }
    if (g_tapSay) {                  // once: what the solve found on THIS body with THIS board
        g_tapSay = false;
        const V3 guess = DeckTopByAnchors(axis0);
        TwkLog("[emote] tap: board %.0f cm, shoulder %.0f cm up, arm %.0f cm; the wrist wants to be %.0f cm up; "
               "shoulder let down %.1f cm, hand slid back %.1f cm, lean %.1f deg, still short %.1f cm | floor by %s",
               L / SC, (dot(S0, U) - TapGround()) / SC, g_arm[c] / SC, (dot(wristFor(Nc, g_tapReach * SC), U) - TapGround()) / SC,
               dropped / SC, slid / SC, leaned, sh > 0.0f ? sh / SC : 0.0f, g_tapGroundUSet ? "the trace under the tail" : "the feet");
        // ...and how the hand ended up turned, so EmoteTapHandRollDeg can be dialled against what is on screen
        // rather than described. `down` is how much the fingers run down the deck, `across` how much they run
        // over its width: 0 roll is all down, a quarter turn is all across.
        TwkLog("[emote] tap: hand roll %.0f / twist %.0f / pitch %.0f -- fingers %.0f%% down the deck, %.0f%% across, "
               "%.0f%% out through its face; palm %.0f%% onto the face, %.0f%% down, %.0f%% sideways",
               g_tapHandRoll, g_tapHandTwist, g_tapHandPitch,
               100.0f * fabsf(dot(f, ax)), 100.0f * fabsf(dot(f, norm(cross(ax, top)))), 100.0f * fabsf(dot(f, top)),
               100.0f * fabsf(dot(p, top)), 100.0f * fabsf(dot(p, ax)), 100.0f * fabsf(dot(p, norm(cross(ax, top)))));
        TwkLog("[emote] tap: deck top by %s (the anchors' guess is %.0f deg from it), deck %.1f cm over the truck line, turned %.0f deg to face front (+%.0f asked)",
               g_deckTopSrc == 2 ? "the drawn board" : g_deckTopSrc == 1 ? "the reference pose" : "THE ANCHORS (nothing better)", acosf(clampf(dot(guess, top0), -1.0f, 1.0f)) * 57.2957795f,
               g_deckRise, roll - g_tapRollDeg, g_tapRollDeg);
    }
    HeadTurn(5.0f * sg, 4.0f + 5.0f * hit, 0.0f);                   // head up; a glance down as it lands
    ArmTo(c, wristFor(add(Nc, mul(U, g_tapLift)), reach), mix3(tf, -0.9f, Out(c), 0.35f, tu, -0.1f), 0.15f);      // a hanging arm's elbow points back
    HandPose(c, f, p);
    // ...and the thumb CLOSES (TH_IN) rather than being left wherever the carry clip put it (TH_KEEP), which
    // in the field was out in the air. It cannot be planted on the tape by aiming: measured, it comes up 3 cm
    // short of the deck wherever it is pointed, because the board lies past its reach. Folding it onto the
    // fingers -- which are now round the far side of the nose -- shuts the hand ON the board instead.
    Fingers(c, TH_KEEP, g_tapCurl[0], g_tapCurl[1], g_tapCurl[2], g_tapCurl[3], 0.4f, 0.0f);
    for (int fi = 0; fi < 4; fi++) FingerNudge(c, fi + 1, g_tapFingerOff[fi][0], g_tapFingerOff[fi][1], g_tapFingerOff[fi][2]);
    ThumbBlend(c, 1, g_tapThumb);        // ...and the thumb separately, across the INDEX whatever the four do
    // THE BOARD GOES WHERE THE HAND ENDED UP, not where it was asked to: a reach the arm fell short of must
    // not pull the board out of the fingers.
    const V3 nose = sub(add(g_Ct[h].p, add(mix3(f, reach, p, g_tapPalmGap * SC, U, 0.0f), gripNudge)), e);
    for (int i = 0; i < g_nBoardRoot; i++) {
        const int b = g_boardRoot[i];
        SetComp(b, qmul(Rb, g_C[b].q), sub(add(nose, qrot(Rb, sub(g_C[b].p, nose0))), g_visOff));      // less what the drawn one sits off by
    }
    g_boardPosed = true;
    const V3 tail = add(add(nose, mul(ax, L)), e);
    g_tapTipW[0] = tail.x; g_tapTipW[1] = tail.y; g_tapTipW[2] = tail.z; g_tapTipSet = true;       // component space; Apply turns it to the world's
}
// RAGE IS AIMED. The way it goes is the way the CAMERA looks -- followed all through the wind-up, so you look
// where you want it and watch the arm, the shoulders and the head come round to it -- and fixed at the moment
// of letting go. How HIGH it goes is the look too: up lobs it, down throws it flat (a throw straight down a
// third-person camera's line would go into the ground at your feet: it looks a little down on you).
// Only the UPPER body turns (a Rage has no legs of its own, so you can walk through one): the spine takes what a
// spine can, the head looks the rest of the way, and the arm throws where it is told even if that is behind you.
float RageStrength(float rate);      // below, with the trigger: how fast a pull was, 0..1
void AimRage(const void* mesh, float t) {
    float f[3];
    float yaw = 0.0f, pitch = 0.0f;
    if (mesh && CameraHeight_ViewForward(f)) {
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const V3 d = norm(qinv(q4(m[0], m[1], m[2], m[3]), v3(f[0], f[1], f[2])));
        const V3 flat = sub(d, mul(U, dot(d, U)));
        if (len(flat) > 0.05f) yaw = atan2f(dot(flat, BR), dot(flat, BF));
        else yaw = g_rageYaw;                                            // looking straight up or down: keep the heading
        // WHERE YOU LOOK IS WHERE IT GOES, the whole way down and the whole way up. This was a rise-per-
        // unit-forward biased by 0.42 and clamped to 0.10..0.95, which is +6 to +44 degrees: level was a
        // lob and straight down was unreachable. The bias was there because a third-person camera looks a
        // little DOWN on the skater, so 1:1 threw at their feet -- but paying for that with the whole
        // lower half of the sphere is the wrong trade. It is a true elevation ANGLE now, and 1:1.
        pitch = asinf(clampf(dot(d, U), -1.0f, 1.0f));
    }
    const float dtH = clampf(t - g_rageLastT, 0.0f, 0.05f);
    g_rageLastT = t;
    if (!g_rageAimSet) { g_rageYaw = yaw; g_ragePitch = pitch; g_rageTwist = clampf(yaw * 57.2957795f * 0.6f, -65.0f, 65.0f); g_rageAimSet = true; return; }
    if (g_thrown || (g_rageSwingAt >= 0.0f && t >= g_rageSwingAt + kRageLetGo)) return;      // it has gone: the follow-through finishes the way it went
    // the short way round -- and kept UNWRAPPED, so a look passing directly behind does not whip the body
    // from one side to the other until it is plainly on the other side
    float dy = yaw - g_rageYaw;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    const float a = 1.0f - expf(-dtH * 10.0f);
    g_rageYaw += dy * a;
    if (g_rageYaw > 3.9f) g_rageYaw -= 6.2831853f;
    if (g_rageYaw < -3.9f) g_rageYaw += 6.2831853f;
    g_ragePitch += (pitch - g_ragePitch) * a;
    const float want = clampf(g_rageYaw * 57.2957795f * 0.6f, -65.0f, 65.0f);
    g_rageTwist += (want - g_rageTwist) * (1.0f - expf(-dtH * 8.0f));
}
void DoRage(const void* mesh, float t) {
    const int c = g_carry >= 0 ? g_carry : 1, o = 1 - c;
    AimRage(mesh, t);
    const V3 dirH = norm(add(mul(BF, cosf(g_rageYaw)), mul(BR, sinf(g_rageYaw))));
    const bool  going = g_rageSwingAt >= 0.0f;                            // the trigger has been pulled: until then it is held, and aimed
    const float T0 = going ? g_rageSwingAt : 1e9f;
    const float hard = going ? clampf(0.55f + 0.45f * RageStrength(g_rageRate), 0.55f, 1.0f) : 1.0f;   // a faster pull is a bigger throw
    const float wind = smooth(t / kRageWind), swing = smooth((t - T0) / kRageSwingLen), after = smooth((t - T0 - 0.35f) / 0.45f);
    const float seethe = going ? window(t, T0 + 0.50f, T0 + 2.50f, 0.4f) : 0.0f;      // angry about it for a bit -- then the pump ends it (kRageAfter)
    const float held = wind * (1.0f - swing);                             // cocked, and being aimed
    const float aimDeg = g_rageYaw * 57.2957795f;
    // the body: back for the wind-up and turned to where it is going, whipped forward through the throw, then
    // hunched and shaking
    SpineLean(-12.0f * held + 22.0f * hard * swing * (1.0f - after) + 9.0f * seethe, 0.0f,
              g_rageTwist * wind * (1.0f - after) + Sg(c) * (9.0f * held - 12.0f * swing * (1.0f - after)));      // toward the throwing side, then through
    TorsoFrame();
    HeadTurn(clampf(aimDeg - g_rageTwist, -70.0f, 70.0f) * wind * (1.0f - after) + 9.0f * sinf(TAU * 1.6f * t) * seethe,
             -8.0f * held - 12.0f * g_ragePitch * held + 20.0f * seethe, 0.0f);
    // the throwing arm: up and back -- AWAY from where it is going, and shaking with it while it is held --
    // then through along the way it goes (and up it, for a lob), then let fall
    const float L = g_arm[c];
    const V3 S = g_Ct[g_uarm[c]].p;
    const float loft = g_ragePitch * 0.7f;      // the arm follows the aim, a little short of it
    const float shake = 0.012f * L * sinf(TAU * 7.0f * t) * held * smooth((t - 0.5f) / 0.2f);
    const V3 back = add(S, mix3(tu, 0.80f * L + shake, dirH, -0.30f * L, Out(c), 0.18f * L));
    const V3 fwd  = add(S, mix3(dirH, 0.86f * L * cosf(loft), tu, 0.86f * L * sinf(loft) - 0.22f * L, Out(c), 0.05f * L));
    V3 tgt = lerp(g_Ct[g_hand[c]].p, back, wind);                                          // from wherever the body has it
    tgt = add(lerp(tgt, fwd, swing), mul(tu, 0.22f * L * sinf(3.14159265f * swing)));       // over the top, not through the chest
    tgt = lerp(tgt, g_Ct[g_hand[c]].p, after);                                              // ...and back to wherever the body has it
    RaiseShoulder(c, 0.5f * wind * (1.0f - after));
    if (after < 0.999f) ArmTo(c, tgt, mix3(Out(c), 1.0f, tf, -0.3f, tu, -0.3f), 0.4f);
    // both hands end up as fists; the throwing one only once the board has left it
    if (g_thrown || !g_boardInHand) { HandPose(c, add(tf, mul(tu, -0.6f)), mul(Out(c), -1.0f)); Fist(c, TH_IN); }
    if (seethe > 0.01f || wind > 0.5f) {
        const float tremble = 0.012f * sinf(TAU * 9.0f * t) * seethe;
        ArmTo(o, P(o, 0.12f + tremble, 0.22f, -0.78f), mix3(tu, -0.3f, Out(o), 1.0f, tf, -0.6f), 0.2f);
        HandPose(o, add(mul(tu, -1.0f), mul(tf, 0.2f)), mul(Out(o), -1.0f));
        Fist(o, TH_IN);
    }
    RaiseShoulder(o, 0.30f * seethe); RaiseShoulder(c, 0.30f * seethe);
    // which way the WORLD sees the throw go: the pump does the throwing, this only tells it where
    g_throwDirSet = true;
    const V3 dc = add(mul(dirH, cosf(g_ragePitch)), mul(U, sinf(g_ragePitch)));    // already unit: dirH is
    g_throwDirW[0] = dc.x; g_throwDirW[1] = dc.y; g_throwDirW[2] = dc.z;       // component space; turned to the world in Apply
}
// ------------------------------------------------------------------ THE DANCES
// SIX of them, and B goes round them: a TAP is the next dance, a HOLD puts the emote away. Five are built
// the same way -- a `DPose` filled in from the beat (hips, spine, head, where each ankle goes, where each
// wrist goes) and applied in ONE place. That is what makes another one cheap, and what makes two of them
// mixable: a change-over is the two sets of numbers lerped. The breakdance cannot be said in those terms --
// the body leaves its feet -- so it poses the skeleton directly, and is mixed the other way about:
// POSE ONE, KEEP IT, POSE THE OTHER, BLEND THE BONES (`PoseMix`), which also carries every change-over
// between styles, whichever two they are.
const char* const kDanceName[DS_COUNT] = { "two-step", "sway", "twist" };

// Everything a standing dance has to say. Feet are in CENTIMETRES off the ankle the game gave it (ahead /
// to the skater's RIGHT / up), wrists in ARM LENGTHS off the shoulder (ahead / OUTWARD / up), so the same
// numbers serve either arm. The float block must stay in one piece: MixPose lerps it by the yard.
struct DPose {
    float hipDown, hipSide, hipFwd, hipYaw, hipLean;
    float spinePitch, spineLean, spineYaw, headYaw, headNod, headTilt;
    V3    foot[2]; float footYaw[2], footPitch[2];
    float aF[2], aO[2], aU[2], pU[2], pO[2], pF[2];
    float fF[2], fU[2], fO[2], mF[2], mU[2], mO[2];
    float curl[2], shoulder[2];
    bool  arm[2]; int thumb[2];                   // NOT lerped: taken from whichever side of a mix is winning
};
DPose Rest() {
    DPose p = {};
    for (int sd = 0; sd < 2; sd++) {
        p.arm[sd] = true;
        p.aF[sd] = 0.30f; p.aO[sd] = 0.26f; p.aU[sd] = -0.42f;
        p.pU[sd] = -1.0f; p.pO[sd] = 0.55f; p.pF[sd] = -0.30f;
        p.fF[sd] = 1.0f;  p.mO[sd] = -1.0f;
        p.curl[sd] = 0.9f; p.thumb[sd] = TH_IN;
    }
    return p;
}
DPose MixPose(const DPose& a, const DPose& b, float m) {
    DPose r = m < 0.5f ? a : b;
    const float* fa = &a.hipDown; const float* fb = &b.hipDown; float* fr = &r.hipDown;
    const int n = (int)(((const char*)&a.arm[0] - (const char*)&a.hipDown) / (int)sizeof(float));
    for (int i = 0; i < n; i++) fr[i] = fa[i] + (fb[i] - fa[i]) * m;
    return r;
}
void ApplyDance(const DPose& p) {
    const float lw = g_legsOk ? smooth(g_lowerW) : 0.0f;
    if (lw > 0.001f) {
        const TF f0[2] = { g_Ct[g_foot[0]], g_Ct[g_foot[1]] };
        SetComp(g_pelvis, qmul(qmul(qaxis(U, p.hipYaw * lw), qaxis(BF, -p.hipLean * lw)), g_Ct[g_pelvis].q),
                add(g_Ct[g_pelvis].p, mul(mix3(U, -p.hipDown, BR, p.hipSide, BF, p.hipFwd), SC * lw)));
        for (int sd = 0; sd < 2; sd++) {
            LegTo(sd, add(f0[sd].p, mul(mix3(BF, p.foot[sd].x, BR, p.foot[sd].y, U, p.foot[sd].z), SC * lw)),
                  add(BF, mul(BR, 0.25f * Sg(sd))));
            SetComp(g_foot[sd], qmul(qmul(qaxis(U, p.footYaw[sd] * lw), qaxis(BR, p.footPitch[sd] * lw)), f0[sd].q),
                    g_Ct[g_foot[sd]].p);
        }
    }
    SpineLean(p.spinePitch, p.spineLean, p.spineYaw);
    TorsoFrame();
    HeadTurn(p.headYaw, p.headNod, p.headTilt);
    for (int sd = 0; sd < 2; sd++) {
        if (!p.arm[sd] || Holding(sd)) continue;          // the arm with the board rides the body, and the board rides it
        if (p.shoulder[sd] > 0.001f) RaiseShoulder(sd, p.shoulder[sd]);
        ArmTo(sd, P(sd, p.aF[sd], p.aO[sd], p.aU[sd]), mix3(tu, p.pU[sd], Out(sd), p.pO[sd], tf, p.pF[sd]), 0.3f);
        HandPose(sd, mix3(tf, p.fF[sd], tu, p.fU[sd], Out(sd), p.fO[sd]), mix3(tf, p.mF[sd], tu, p.mU[sd], Out(sd), p.mO[sd]));
        const float c = p.curl[sd];
        Fingers(sd, p.thumb[sd], c, c, c, c, 0.4f, 0.0f);
    }
}
// TWO POSES, BLENDED. Pose the first, keep its local transforms, put the skeleton back to the animation and
// pose the second, then lerp the two. Local space, like the emote's own blend: a limb swings across rather
// than sliding. Nested once at most (a style change-over during the breakdance's own).
typedef void (*PoseFn)(float);
TF  g_mixL[2][MAX_BONES]; int g_mixDepth = 0;
void PoseMix(PoseFn a, float ba, PoseFn b, float bb, float m) {
    if (!(m > 0.001f)) { a(ba); return; }
    if (m >= 0.999f)   { b(bb); return; }
    const int d = g_mixDepth < 2 ? g_mixDepth++ : 1;
    a(ba);
    for (int i = 0; i < g_nBones; i++) g_mixL[d][i] = g_Lt[i];
    for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
    b(bb);
    const float k = smooth(m);
    for (int i = 0; i < g_nBones; i++) {
        g_Lt[i].q = qblend(g_mixL[d][i].q, g_Lt[i].q, k);
        g_Lt[i].p = lerp(g_mixL[d][i].p, g_Lt[i].p, k);
    }
    for (int i = 0; i < g_nBones; i++) CompOf(i, g_Lt, g_Ct);
    if (g_mixDepth > 0) g_mixDepth--;
}

// ---- 1. the two-step: the one that shipped. Weight from foot to foot, a heel off the floor on its beat.
void StyleTwoStep(float b) {
    const float ph = TAU * b, half = PI * b;
    DPose p = Rest();
    p.hipDown = 3.2f * (0.5f - 0.5f * cosf(ph)); p.hipSide = 3.6f * sinf(half); p.hipYaw = 7.0f * sinf(half);
    const float upL = sinf(half), upR = -upL;
    p.foot[0].z = upL > 0.0f ? 2.6f * upL * upL : 0.0f;
    p.foot[1].z = upR > 0.0f ? 2.6f * upR * upR : 0.0f;
    p.spinePitch = 3.0f * sinf(ph); p.spineLean = 3.0f * sinf(half); p.spineYaw = -4.5f * sinf(half);
    p.headYaw = 6.0f * sinf(half + 0.6f); p.headNod = 5.0f * sinf(ph); p.headTilt = 3.0f * sinf(half);
    for (int sd = 0; sd < 2; sd++) {
        const float o = sd ? PI : 0.0f;
        p.aF[sd] = 0.40f + 0.10f * sinf(half + o); p.aO[sd] = 0.28f; p.aU[sd] = -0.30f + 0.16f * sinf(ph + o);
        p.pU[sd] = -1.0f; p.pO[sd] = 0.6f; p.pF[sd] = -0.3f;
        p.fF[sd] = 1.0f; p.fU[sd] = 0.35f; p.mO[sd] = -1.0f;
        p.curl[sd] = 1.0f; p.thumb[sd] = TH_IN;
    }
    ApplyDance(p);
}
// ---- 2. the sway: slow, for a slow song. The hips go round, the shoulders roll, the hands are open.
void StyleSway(float b) {
    const float half = PI * b, ph = TAU * b;
    DPose p = Rest();
    p.hipSide = 6.5f * sinf(half); p.hipDown = 2.0f * (0.5f - 0.5f * cosf(ph)); p.hipFwd = 2.0f * sinf(ph);
    p.hipYaw = 5.0f * sinf(half); p.hipLean = 3.0f * sinf(half);
    p.spinePitch = 2.0f * sinf(ph); p.spineLean = 5.0f * sinf(half); p.spineYaw = -3.0f * sinf(half);
    p.headYaw = 8.0f * sinf(half); p.headNod = 4.0f * sinf(ph); p.headTilt = 5.0f * sinf(half);
    for (int sd = 0; sd < 2; sd++) {
        const float o = sd ? PI : 0.0f;
        p.shoulder[sd] = 0.10f + 0.10f * sinf(ph + o);
        p.aF[sd] = 0.28f + 0.06f * sinf(half + o); p.aO[sd] = 0.34f + 0.07f * sinf(half + o);
        p.aU[sd] = -0.16f + 0.13f * sinf(half + o);
        p.pU[sd] = -0.7f; p.pO[sd] = 0.8f; p.pF[sd] = -0.2f;
        p.fF[sd] = 0.55f; p.fU[sd] = 0.85f; p.mF[sd] = 1.0f; p.mO[sd] = -0.25f;
        p.curl[sd] = 0.12f; p.thumb[sd] = TH_OPEN;
    }
    ApplyDance(p);
}
// ---- 3. the twist: heels pivot one way, the shoulders the other, knees soft.
void StyleTwist(float b) {
    const float w = TAU * b;
    DPose p = Rest();
    p.hipDown = 4.0f + 2.0f * (0.5f - 0.5f * cosf(2.0f * w)); p.hipYaw = 17.0f * sinf(w);
    p.spinePitch = 6.0f; p.spineYaw = -13.0f * sinf(w); p.spineLean = 2.0f * sinf(w);
    p.headYaw = 6.0f * sinf(w + 0.4f); p.headNod = 3.0f + 2.0f * sinf(2.0f * w);
    for (int sd = 0; sd < 2; sd++) {
        p.footYaw[sd] = 14.0f * sinf(w);                       // the heels turn with the hips
        p.foot[sd].z = 0.8f + 0.8f * sinf(w + (sd ? PI : 0.0f));
        p.aF[sd] = 0.26f + 0.05f * sinf(w); p.aO[sd] = 0.24f + 0.05f * sinf(w + (sd ? PI : 0.0f));
        p.aU[sd] = -0.34f + 0.05f * sinf(2.0f * w);
        p.pU[sd] = -0.8f; p.pO[sd] = 0.9f; p.pF[sd] = -0.2f;
        p.fF[sd] = 1.0f; p.fU[sd] = 0.2f; p.mO[sd] = -1.0f;
        p.curl[sd] = 1.0f; p.thumb[sd] = TH_IN;
    }
    ApplyDance(p);
}
PoseFn kDanceFn[DS_COUNT] = { StyleTwoStep, StyleSway, StyleTwist };

void DoDance(float t) {
    const float b = t * (g_danceBpm / 60.0f);
    const int cur = (g_var >= 0 && g_var < DS_COUNT) ? g_var : 0;
    const int prev = (g_varPrev >= 0 && g_varPrev < DS_COUNT) ? g_varPrev : cur;
    if (prev == cur || g_varMix >= 0.999f) kDanceFn[cur](b);
    else PoseMix(kDanceFn[prev], b, kDanceFn[cur], b, g_varMix);
}

// A CLAP. Where in its cycle it is: 0 = hands apart, closing faster and faster to CONTACT at kClapContact, then
// opening again, easing out. The pump reads the same clock to make the sound on the contact.
const float kClapRate = 3.1f, kClapStart = 0.40f, kClapContact = 0.38f;
float ClapPhase(float t) { return t < kClapStart ? 0.0f : fmodf((t - kClapStart) * kClapRate, 1.0f); }
float ClapGap(float ph)  { return ph < kClapContact ? 1.0f - (ph / kClapContact) * (ph / kClapContact)
                                                     : sinf(1.5707963f * (ph - kClapContact) / (1.0f - kClapContact)); }
// THE BOARD, TUCKED UNDER THE ARM that was carrying it: both hands are wanted. Along the side, nose forward and
// a little down, the grip tape to the ribs and the graphic out, held between its trucks by the upper arm.
void TuckBoard(int c) {
    if (c < 0 || g_truckF < 0 || g_truckB < 0 || !g_nBoardRoot) return;
    V3 axis0 = sub(g_C[g_truckF].p, g_C[g_truckB].p);
    const float wb = len(axis0);
    if (wb < 5.0f) return;
    axis0 = mul(axis0, 1.0f / wb);
    const V3 mid0 = mul(add(g_C[g_truckF].p, g_C[g_truckB].p), 0.5f);
    const V3 top0 = DeckTop(axis0);
    const V3 ax = norm(mix3(tf, 0.93f, tu, -0.30f, Out(c), 0.06f));                 // tail -> nose
    V3 top = mul(Out(c), -1.0f);
    top = norm(sub(top, mul(ax, dot(top, ax))));
    const Q4 r1 = qfromto(axis0, ax);
    const V3 t1 = norm(qrot(r1, top0));
    const float roll = atan2f(dot(cross(t1, top), ax), dot(t1, top)) * 57.2957795f;
    const Q4 Rb = qmul(qaxis(ax, roll), r1);
    const V3 S = g_Ct[g_uarm[c]].p;
    const V3 mid = sub(add(S, mix3(tf, 7.0f * SC, tu, -21.0f * SC, Out(c), -2.5f * SC)), g_visOff);
    for (int i = 0; i < g_nBoardRoot; i++) {
        const int b = g_boardRoot[i];
        SetComp(b, qmul(Rb, g_C[b].q), add(mid, qrot(Rb, sub(g_C[b].p, mid0))));
    }
    g_boardPosed = true;
}
void DoClap(float t) {
    const int c = (g_boardInHand && !g_thrown && g_carry >= 0) ? g_carry : -1;      // the arm with a board to keep hold of
    const float ph = ClapPhase(t), gap = ClapGap(ph);
    const float since = ph >= kClapContact ? (ph - kClapContact) / kClapRate : 1.0f; // seconds since the hands met
    const float hit = t < kClapStart ? 0.0f : expf(-since * 14.0f);
    SpineLean(3.0f + 1.5f * hit, 0.0f, 0.0f);
    TorsoFrame();
    HeadTurn(0.0f, -3.0f + 2.5f * hit, 0.0f);
    TuckBoard(c);
    // the hands meet in front of the lower chest, fingers up and away, a little crossed as clapping hands are;
    // one leads (the free one, else the right) and the other comes to meet it
    const int lead = c >= 0 ? 1 - c : 1;
    const V3 mid = mul(add(g_Ct[g_uarm[0]].p, g_Ct[g_uarm[1]].p), 0.5f);
    const float L = 0.5f * (g_arm[0] + g_arm[1]);
    const V3 C = add(mid, mix3(tf, 0.43f * L, tu, -0.28f * L + 0.6f * SC * hit, tr, 0.0f));
    for (int sd = 0; sd < 2; sd++) {
        const float share = sd == lead ? 0.62f : 0.38f;
        const float half = (0.2f + 17.0f * gap * share) * SC;                       // this hand's part of the gap
        const V3 fingers = norm(mix3(tf, 0.72f, tu, sd == lead ? 0.78f : 0.52f, Out(sd), -0.10f));
        const V3 palm = mul(Out(sd), -1.0f);                                        // toward the other hand
        const V3 wrist = sub(add(C, mul(Out(sd), half + 1.6f * SC)), mul(fingers, 7.5f * SC));
        ArmTo(sd, wrist, mix3(tu, -1.0f, Out(sd), sd == c ? 0.60f : 0.35f, tf, -0.30f), 0.35f);      // elbows by the ribs, not out like wings (the board's arm a little wider: it has a board under it)
        HandPose(sd, fingers, palm);
        Fingers(sd, TH_OPEN, 0.06f, 0.05f, 0.06f, 0.10f, 0.35f, 0.0f);
    }
}
// A BOX UNDER THE ARM, LONG WAYS (asked for: "hold it under your arm long ways"). Whatever the mesh is, it is
// carried by its SHAPE: its longest side runs forward (nose a little down), its thinnest lies across -- flat
// against the ribs -- and the other stands up; the top is in the armpit, the upper arm comes down its outer
// face and the hand cups its bottom edge from outside. The arm with no board in it does the carrying.
// Where the ACTOR has to be for that is worked out here, in the posed body's terms, and handed over in the world's
// (Apply): the prop is put there, then hung from the chest bone so it rides the body with no frame of lag.
void DoCarry(float) {
    const int sd = FreeHand(); const float sg = Sg(sd);
    g_boxSide = sd;
    TorsoFrame();
    int iL = 0, iS = 0;
    for (int i = 1; i < 3; i++) { if (g_boxE[i] > g_boxE[iL]) iL = i; if (g_boxE[i] < g_boxE[iS]) iS = i; }
    if (iL == iS) { iL = 0; iS = 2; }
    const int iM = 3 - iL - iS;
    const float eL = g_boxE[iL], eM = g_boxE[iM], eS = g_boxE[iS];
    auto unit = [](int i) { return v3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f); };
    const V3 wantLong = norm(mix3(tf, 0.97f, tu, -0.12f, Out(sd), 0.04f));
    V3 wantSmall = Out(sd);
    wantSmall = norm(sub(wantSmall, mul(wantLong, dot(wantSmall, wantLong))));
    const Q4 r1 = qfromto(unit(iL), wantLong);
    const V3 s1 = norm(qrot(r1, unit(iS)));
    const float roll = atan2f(dot(cross(s1, wantSmall), wantLong), dot(s1, wantSmall)) * 57.2957795f;
    const Q4 Rc = qmul(qaxis(wantLong, roll), r1);                                  // the mesh's space -> the body's (component) space
    const V3 up = norm(cross(wantSmall, wantLong));
    const V3 upB = dot(up, tu) < 0.0f ? mul(up, -1.0f) : up;                        // the way that is up, whichever way the mesh ended
    const V3 S = g_Ct[g_uarm[sd]].p;
    const V3 C = add(S, mix3(tf, 6.0f * SC, upB, -(10.0f * SC + eM), wantSmall, eS - 5.5f * SC));      // the box's CENTRE
    const V3 origin = sub(C, qrot(Rc, v3(g_boxO[0], g_boxO[1], g_boxO[2])));        // ...and so the actor's origin
    g_boxPosW[0] = origin.x; g_boxPosW[1] = origin.y; g_boxPosW[2] = origin.z;      // component space; Apply turns them to the world's
    g_boxQuatW[0] = Rc.x; g_boxQuatW[1] = Rc.y; g_boxQuatW[2] = Rc.z; g_boxQuatW[3] = Rc.w;
    g_boxSet = true;
    // the arm: down the outer face, the hand under the bottom edge a little ahead of the middle
    const float ahead = eL * 0.55f < 16.0f * SC ? eL * 0.55f : 16.0f * SC;
    const V3 wrist = add(C, mix3(wantLong, ahead, upB, -(eM + 1.5f * SC), wantSmall, eS * 0.6f));
    RaiseShoulder(sd, 0.10f);
    ArmTo(sd, wrist, mix3(Out(sd), 1.0f, tf, -0.6f, tu, -0.2f), 0.25f);
    HandPose(sd, norm(mix3(Out(sd), -0.85f, tf, 0.45f, tu, 0.10f)), upB);           // fingers in under it, palm up against its bottom
    Fingers(sd, TH_OPEN, 0.22f, 0.22f, 0.26f, 0.30f, 0.3f, 0.0f);
    (void)sg;
}
void Perform(int id, const void* mesh, float t) {
    switch (id) {
    case EM_WAVE:     DoWave(t); break;
    case EM_THUMBS:   DoThumbs(t); break;
    case EM_POINT:    DoPoint(mesh, t); break;
    case EM_FACEPALM: DoFacepalm(t); break;
    case EM_CLAP:     DoClap(t); break;
    case EM_TAP:      DoTap(t); break;
    case EM_RAGE:     DoRage(mesh, t); break;
    case EM_DANCE:    DoDance(t); break;
    case EM_CARRY:    DoCarry(t); break;
    default: break;
    }
}

// Which hand has the board: the one nearer the board rig, while the board's own movement mode says it is
// being carried. (First round this read the mode at the wrong offset -- a constant borrowed from a module
// that only ever logged it -- and so every emote believed both hands were free.)
void FindCarry() {
    g_carry = -1;
    if (!g_boardInHand) return;
    if (g_seatSpot && g_seatSet > 0.0f) {          // seated, the board laid down: only the tap and the throw take it up
        g_carry = (g_id == EM_TAP || g_id == EM_RAGE) ? g_seatCarry : -1;
        return;
    }
    const int b = g_flipper >= 0 ? g_flipper : (g_nBoardRoot ? g_boardRoot[0] : -1);
    if (b < 0) return;
    g_carry = len(sub(g_C[b].p, g_C[g_hand[0]].p)) < len(sub(g_C[b].p, g_C[g_hand[1]].p)) ? 0 : 1;
}
// The board goes where the hand that has it goes: the rig's top bones are moved by exactly the change the
// emote made to that hand, position and turn, so the grip the animation drew is the grip that is kept.
void CarryBoard() {
    if (g_carry < 0 || !g_nBoardRoot || g_thrown || g_boardPosed) return;      // posed: the emote put the board somewhere itself (the tap's regrip)
    if (g_seatSet > 0.0f && g_id != EM_RAGE) return;                           // laid down beside a seat: no hand has it
    const int h = g_hand[g_carry];
    const Q4 d = qmul(g_Ct[h].q, qconj(g_C[h].q));
    if (len(sub(g_Ct[h].p, g_C[h].p)) < 0.02f && fabsf(d.w) > 0.999999f) return;        // the hand did not move: nor does the board
    for (int i = 0; i < g_nBoardRoot; i++) {
        const int b = g_boardRoot[i];
        SetComp(b, qmul(d, g_C[b].q), add(g_Ct[h].p, qrot(d, sub(g_C[b].p, g_C[h].p))));
    }
}

// THE KEPT BOARD, LAID DOWN (sit.cpp's SitKeepBoard). sit.cpp keeps it riding the hand that carries it; here
// it goes onto the seat's spot for it -- flat, its wheels' bottoms on the surface, its nose where the spot says
// -- as the hands settle into the seat, and comes back up into the hand as they rise. The throw takes it back
// into the hand as the throw comes up (and the tap regrips it from where it lies, itself). No emote need run.
const float kWheelR = 2.7f;                  // a skate wheel's radius, cm
void SeatBoard(void* mesh) {
    g_seatSpot = false; g_seatSet = 0.0f;
    if (mesh != g_seatMesh || !g_rigOk || !g_nBoardRoot || g_truckF < 0 || g_truckB < 0) return;
    SitBoardSpot sp;
    if (!Sit_BoardSpot(&sp)) { g_seatGap = true; return; }
    // Back from a throw while seated (Y): it arrives in the hand, like standing, and is then set down again.
    // (A sit just begun has its hands at the start of the way down: that is its own set-down.)
    const LONGLONG nowMs = (LONGLONG)GetTickCount64();
    if (g_seatGap) { g_seatGap = false; g_seatBackMs = sp.arms > 0.99f ? nowMs : 0; }
    const float back = g_seatBackMs ? smooth((float)(nowMs - g_seatBackMs) / 600.0f) : 1.0f;
    if (back >= 1.0f) g_seatBackMs = 0;
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
    for (int i = 0; i < n; i++) { LocalOf(i, g_C, g_L); g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
    const V3 os = v3(sp.os[0], sp.os[1], sp.os[2]), f = v3(sp.f[0], sp.f[1], sp.f[2]);
    const V3 r = v3(sp.r[0], sp.r[1], sp.r[2]), u = v3(sp.u[0], sp.u[1], sp.u[2]);
    g_seatSpot = true; g_seatCarry = sp.carry; g_seatOs = os; g_seatU = u;
    // set down once the hands are most of the way there; picked up as soon as they start back
    float s = smooth((sp.arms - 0.35f) / 0.65f) * back;
    if (g_id == EM_RAGE) s *= 1.0f - smooth(g_w);          // the throwing hand takes it back up
    g_seatSet = s;
    if (s <= 0.0005f) return;
    V3 axis0 = sub(g_C[g_truckF].p, g_C[g_truckB].p);
    const float wb = len(axis0);
    if (wb < 5.0f) return;
    axis0 = mul(axis0, 1.0f / wb);
    const V3 mid0 = mul(add(g_C[g_truckF].p, g_C[g_truckB].p), 0.5f);
    const V3 top0 = DeckTop(axis0);
    const float yr = sp.yawDeg * 0.0174532925f;
    const V3 ax = norm(add(mul(f, cosf(yr)), mul(r, sinf(yr))));      // tail -> nose, level
    const Q4 r1 = qfromto(axis0, ax);
    const V3 t1 = norm(qrot(r1, top0));
    const float roll = atan2f(dot(cross(t1, u), ax), dot(t1, u)) * 57.2957795f;
    const Q4 Rb = qmul(qaxis(ax, roll), r1);                          // deck top up, wheels down
    // how far under the truck line its wheels' bottoms are once it is turned so: that is what meets the surface
    float low = 1e9f;
    for (int w = 0; w < 4; w++) if (g_wheel[w] >= 0) low = fminf(low, dot(qrot(Rb, sub(g_C[g_wheel[w]].p, mid0)), u));
    if (low > 1e8f) low = -(g_deckRise + 6.0f);
    low -= kWheelR;
    const V3 P = add(add(add(os, mul(f, sp.fx)), mul(r, sp.ry)), mul(u, sp.uz));    // the middle of its footprint, on the surface
    const V3 mid = add(P, mul(u, -low));
    for (int i = 0; i < g_nBoardRoot; i++) {
        const int b = g_boardRoot[i];
        const Q4 qT = qmul(Rb, g_C[b].q);
        const V3 pT = sub(add(mid, qrot(Rb, sub(g_C[b].p, mid0))), g_visOff);    // less what the drawn one sits off by
        SetComp(b, qblend(g_C[b].q, qT, s), lerp(g_C[b].p, pT, s));
    }
    for (int i = 0; i < n; i++) {
        float* t = (float*)(data + i * 48);
        t[0] = g_Ct[i].q.x; t[1] = g_Ct[i].q.y; t[2] = g_Ct[i].q.z; t[3] = g_Ct[i].q.w;
        t[4] = g_Ct[i].p.x; t[5] = g_Ct[i].p.y; t[6] = g_Ct[i].p.z;
    }
    static int s_said = 0;
    if (s >= 0.999f && s_said < 12) {
        s_said++;
        TwkLog("[emote] the board is down beside the seat: %.0f cm ahead, %.0f cm %s, %.0f cm %s the seat, nose %.0f deg off ahead | wheels x%d, deck top by %s",
               sp.fx, fabsf(sp.ry), sp.ry >= 0.0f ? "right" : "left", fabsf(sp.uz), sp.uz >= 0.0f ? "over" : "under", sp.yawDeg,
               (g_wheel[0] >= 0) + (g_wheel[1] >= 0) + (g_wheel[2] >= 0) + (g_wheel[3] >= 0),
               g_deckTopSrc == 2 ? "the drawn board" : g_deckTopSrc == 1 ? "the reference pose" : "the anchors");
    }
}

void Apply(void* mesh) {
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
    for (int i = 0; i < n; i++) { LocalOf(i, g_C, g_L); g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
    if (g_carry == -2) {
        FindCarry();
        TwkLog("[emote] %s: %s, %s", kDefs[g_id].name,
               g_carry < 0 ? "both hands free" : g_carry ? "the board is in the right hand, so the left does the talking" : "the board is in the left hand",
               g_seated ? "seated" : "on foot");
    }
    BodyFrame(mesh);
    g_boardPosed = false;
    g_tapGroundUSet = false;
    if (g_id == EM_TAP && g_tapGroundSet) {          // what the pump found under the tail, as a height along the body's up
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const V3 sc = v3(fabsf(m[8]) > 1e-6f ? m[8] : 1.0f, fabsf(m[9]) > 1e-6f ? m[9] : 1.0f, fabsf(m[10]) > 1e-6f ? m[10] : 1.0f);
        V3 pc = qinv(q4(m[0], m[1], m[2], m[3]), sub(v3(g_tapGroundW[0], g_tapGroundW[1], g_tapGroundW[2]), v3(m[4], m[5], m[6])));
        pc = v3(pc.x / sc.x, pc.y / sc.y, pc.z / sc.z);
        g_tapGroundU = dot(pc, U); g_tapGroundUSet = true;
    }
    g_boxSet = false;
    Perform(g_id, mesh, g_t);
    CarryBoard();
    if (g_id == EM_CARRY && g_boxSet) {            // where the carried prop's actor belongs, as the WORLD has it
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const Q4 mq = q4(m[0], m[1], m[2], m[3]);
        const V3 sc = v3(fabsf(m[8]) > 1e-6f ? m[8] : 1.0f, fabsf(m[9]) > 1e-6f ? m[9] : 1.0f, fabsf(m[10]) > 1e-6f ? m[10] : 1.0f);
        const V3 w = add(v3(m[4], m[5], m[6]), qrot(mq, mulv(v3(g_boxPosW[0], g_boxPosW[1], g_boxPosW[2]), sc)));
        const Q4 wq = qmul(mq, q4(g_boxQuatW[0], g_boxQuatW[1], g_boxQuatW[2], g_boxQuatW[3]));
        g_boxPosW[0] = w.x; g_boxPosW[1] = w.y; g_boxPosW[2] = w.z;
        g_boxQuatW[0] = wq.x; g_boxQuatW[1] = wq.y; g_boxQuatW[2] = wq.z; g_boxQuatW[3] = wq.w;
        g_boxWorldOk = true;
    }
    if (g_id == EM_TAP && g_tapTipSet) {           // the tail, as the WORLD has it: the pump asks what is under it
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const V3 sc = v3(fabsf(m[8]) > 1e-6f ? m[8] : 1.0f, fabsf(m[9]) > 1e-6f ? m[9] : 1.0f, fabsf(m[10]) > 1e-6f ? m[10] : 1.0f);
        const V3 w = add(v3(m[4], m[5], m[6]), qrot(q4(m[0], m[1], m[2], m[3]), mulv(v3(g_tapTipW[0], g_tapTipW[1], g_tapTipW[2]), sc)));
        g_tapTipW[0] = w.x; g_tapTipW[1] = w.y; g_tapTipW[2] = w.z;
    }
    if (g_id == EM_RAGE && g_throwDirSet) {       // DoRage left the way it goes in the body's terms: the pump wants the world's
        const float* m = (const float*)((const uint8_t*)mesh + SC_C2W);
        const V3 w = qrot(q4(m[0], m[1], m[2], m[3]), v3(g_throwDirW[0], g_throwDirW[1], g_throwDirW[2]));
        g_throwDirW[0] = w.x; g_throwDirW[1] = w.y; g_throwDirW[2] = w.z;
    }
    // blended in LOCAL space, so a limb swings into the gesture rather than sliding to it
    const float a = smooth(g_w);
    for (int i = 0; i < n; i++) { g_Lt[i].q = qblend(g_L[i].q, g_Lt[i].q, a); g_Lt[i].p = lerp(g_L[i].p, g_Lt[i].p, a); }
    for (int i = 0; i < n; i++) CompOf(i, g_Lt, g_Ct);
    for (int i = 0; i < n; i++) {
        float* t = (float*)(data + i * 48);
        t[0] = g_Ct[i].q.x; t[1] = g_Ct[i].q.y; t[2] = g_Ct[i].q.z; t[3] = g_Ct[i].q.w;
        t[4] = g_Ct[i].p.x; t[5] = g_Ct[i].p.y; t[6] = g_Ct[i].p.z;
    }
}

void Drop(const char* why) {
    if (g_id != EM_NONE && why) TwkLog("[emote] %s ended: %s", kDefs[g_id].name, why);
    g_id = EM_NONE; g_next = EM_NONE; g_w = 0.0f; g_t = 0.0f; g_ending = false; g_lowerW = 0.0f; g_moveS = 0.0f;
    g_mesh = nullptr; g_skater = nullptr; g_meshRef.obj = nullptr; g_carry = -2; g_ptSet = false;
    g_throwDirSet = false; g_thrown = false; g_rageSwingAt = -1.0f;
    g_bDown = false; g_bUsed = false; g_bHeld = 0.0f; g_varMix = 1.0f; g_mixDepth = 0;
}
float Rand01() { g_rng = g_rng * 1664525u + 1013904223u; return (float)((g_rng >> 8) & 0xFFFF) / 65535.0f; }
bool OnFoot(void* sk) {
    void* ai = FootPlace_AnimInstance();
    return sk && ai && twkB(ai, AN_ON_BOARD) == 0;
}
void PollTrigger();      // below, with the rest of the trigger
bool Begin(int id, void* sk) {
    void* mesh = twkP(sk, CH_MESH);
    if (!mesh) { TwkLog("[emote] %s: the skater has no mesh", kDefs[id].name); return false; }
    bool ok = false;
    __try { ok = ResolveRig(mesh); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; g_faults++; }
    if (!ok) { TwkLog("[emote] %s: the skeleton could not be read", kDefs[id].name); return false; }
    SitUI_Track(&g_meshRef, mesh);
    if (!g_meshRef.obj) { TwkLog("[emote] %s: the mesh is not in the object table as itself -- not played", kDefs[id].name); return false; }
    g_skater = sk; g_mesh = mesh; g_id = id; g_next = EM_NONE;
    g_t = 0.0f; g_w = 0.0f; g_ending = false; g_outTime = kBlendOut; g_lowerW = 0.0f; g_moveS = 0.0f; g_carry = -2; g_ptSet = false;
    g_throwDirSet = false; g_thrown = false; g_tapSay = (id == EM_TAP);
    g_var = g_lastVar[id] % (kDefs[id].variants > 0 ? kDefs[id].variants : 1); g_varPrev = g_var; g_varMix = 1.0f;
    g_bDown = false; g_bUsed = false; g_bHeld = 0.0f;
    if (id == EM_DANCE && g_var != DS_TWO_STEP) TwkLog("[emote] Dance: %s", kDanceName[g_var]);
    g_clapPhase = 0.0f; g_clapCount = 0; g_clapMuted = 0; g_clapEchoAt = -1.0f; if (!g_clapCue) g_clapCueTried = false;
    if (!g_tapCue) g_tapCueTried = false;
    g_tapLift = 14.0f; g_tapVel = 0.0f; g_tapSwing = 0.0f; g_tapHit = 0.0f; g_tapWalk = 0.0f; g_tapStickLift = false; g_tapDown = false; g_tapTipSet = false; g_tapCount = 0; g_tapGroundSet = false; g_tapGroundUSet = false; g_tapSurface = 0;
    g_boardInHand = Sit_BoardInHand(sk);
    if (g_boardInHand) {                             // what the drawn board says of itself, before the first frame is posed
        g_visSay = true;
        __try { MeasureBoard(sk, mesh, true); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    if (id == EM_RAGE) {
            g_rng ^= (unsigned)GetTickCount64();
        g_rageAimSet = false; g_rageLastT = 0.0f; g_rageTwist = 0.0f;   // WHERE it goes is the camera's to say (AimRage), all through the wind-up
        g_rageSwingAt = -1.0f; g_rageRate = 0.0f; g_pulling = false;    // WHEN, and HOW FAST, the right trigger's
        PollTrigger();
        g_trigArmed = g_trigger < kTrigOff;                             // ...once it has been let go: one held through the wheel does not fire it
        for (int i = 0; i < 3; i++) g_rageSpin[i] = (Rand01() * 2.0f - 1.0f) * 11.0f;      // rad/s
    }
    return true;
}

// THE RIGHT TRIGGER AND A RAGE. Pure: the pump calls it every frame with the emote's clock. Until the trigger has
// been let go once it is not listened to (one held while the wheel was up must not throw the moment it shuts);
// then the first pull past kTrigOn, once the wind-up is done, STARTS THE SWING, and until the board leaves the
// hand the most it has been pulled is kept -- that is how hard it goes.
// The right trigger as the ENGINE has it (catch_tweaks asks UPlayerInput's key-state map): the real analogue value,
// every frame a Rage is up. Without it there is only the trigger's press, which the key hook feeds (Emote_Trigger).
void PollTrigger() {
    float v = 0.0f;
    g_trigPolled = CatchTweaks_RightTrigger(&v);
    if (g_trigPolled) g_trigger = v;
}
// HOW HARD IS HOW FAST IT IS PULLED -- the rate from where the pull started to where it got to, not how far it
// got. A flick to halfway is a hard throw and a slow squeeze to the end is a gentle one, which is how throwing
// something actually works: what you put in is the speed of the hand, not how far it travelled.
// WHEN it goes is unchanged, and still waits for the pull to say it is done: the trigger reaches the end
// (kTrigFull), or stops going further for kTrigSettle, or is let go again. (An older cut took the most it reached
// in the 0.14 s after the first touch -- and, the field found, never saw the analogue value at all: see
// CatchTweaks_RightTrigger.) A pull still going while the arm comes through is still measured.
const float kTrigFull = 0.97f, kTrigSettle = 0.07f;
// The rate of the pull as it stands: from where it was first felt to the furthest it has got, over the time
// between. Measured across the WHOLE pull rather than frame to frame, because a per-frame difference is one
// sample of a noisy axis and this is an average of the lot.
static float PullRate() {
    const float dv = g_pullHigh - g_pullLowV;
    if (dv <= 0.0f) return 0.0f;
    const float dt = g_pullRiseT - g_pullLowT;
    return dv / (dt > kRateMinDt ? dt : kRateMinDt);
}
void RageTrigger(float t) {
    if (g_rageSwingAt < 0.0f) {
        if (!g_trigArmed) { if (g_trigger < kTrigOff) g_trigArmed = true; return; }
        if (t < kRageWind * 0.9f) return;
        if (g_trigger >= kTrigOn) {
            // WHERE THE PULL STARTED is one frame BEFORE it was first felt, not at kTrigOn: by the time the
            // trigger reads 0.12 a fast pull is already moving, and starting the clock there throws away the
            // part of the travel that says how fast it was.
            if (!g_pulling) { g_pulling = true; g_pullHigh = g_trigger; g_pullRiseT = t; g_pullLowV = 0.0f; g_pullLowT = t - kRateMinDt; }
            else if (g_trigger > g_pullHigh + 0.015f) { g_pullHigh = g_trigger; g_pullRiseT = t; }
            if (g_pullHigh >= kTrigFull || t - g_pullRiseT >= kTrigSettle) { g_rageSwingAt = t; g_rageRate = PullRate(); }
        } else if (g_pulling) { g_rageSwingAt = t; g_rageRate = PullRate(); }      // a blip: it goes with how fast it got there
    } else if (t < g_rageSwingAt + kRageLetGo) {
        // still being pulled while the arm comes through: it counts, but only if it is FASTER
        if (g_trigger > g_pullHigh + 0.015f) { g_pullHigh = g_trigger; g_pullRiseT = t; }
        const float r = PullRate();
        if (r > g_rageRate) g_rageRate = r;
    }
}
bool  RageDone(float t) { return g_rageSwingAt >= 0.0f && t >= g_rageSwingAt + kRageAfter; }      // thrown, and angry long enough
float RageStrength(float rate) { return clampf((rate - kRateSoft) / (kRateHard - kRateSoft), 0.0f, 1.0f); }
float RageSpeed(float rate) { return 420.0f + 1130.0f * powf(RageStrength(rate), 1.15f); }      // cm/s: a toss .. a hurl
// A clap's sound. The game ships no clap; the nearest things it has are a PALM landing on the ground
// (SCU_HandLand) and the slaps a board makes on feet and hands -- played pitched up, the first of them that is
// loaded. Found by NAME (only a loaded cue is found), watched like any object since a cue can be unloaded, and
// spawned through the replay's audio manager like the tap's knock: it is in replays and other players hear it.
// The sound, and its falloff put on it (see THE CLAP'S SOUND AND ITS REACH). Game thread. A name with a '/' is a
// full path (loaded if need be), else a short name (found only if loaded).
// Walking's falloff onto a sound, and a cue's own override cleared (see THE CLAP'S SOUND AND ITS REACH).
static const char* GiveFalloff(void* snd, const char* name, void* at) {
    const char* reach = "its own (could not be set)";
    __try {
        void** slot = (void**)((uint8_t*)snd + SB_ATTENUATION);
        if (!*slot || *slot == at) { *slot = at; reach = "walking's (silent past 50 m)"; }
        else reach = "its own (it already had one)";
        // A CUE can override its base attenuation (+0x530 bit 0 -> AttenuationOverrides) or carry an attenuation
        // node; either would beat the pointer set above. The override is cleared (the cue was heard map-wide, so
        // whatever it held did not attenuate); a node is only reported.
        if (CatchSound_FindObject(name, "SoundCue") == snd) {
            uint8_t* f = (uint8_t*)snd + SCUE_FLAGS;
            if (*f & 1) { *f &= (uint8_t)~1; reach = "walking's (silent past 50 m; the cue's own override cleared)"; }
            if (*((uint8_t*)snd + SCUE_NODEFLAGS) & 2)
                TwkLog("[emote] clap: '%s' has an attenuation node of its own -- it may not fade as set", name);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return reach;
}
static void ResolveClap() {
    if (g_clapCue && !SitUI_Alive(&g_clapCueRef)) { g_clapCue = nullptr; g_clapCueTried = false; }
    if (g_clapCue || g_clapCueTried) return;
    g_clapCueTried = true;
    const char* const names[5] = { g_clapSound, "WAV_Cinematic_HandLand_03", "SCU_HandLand", "SCU_FootOnBoard", "SCU_Board_Catch" };
    for (int i = 0; i < 5 && !g_clapCue; i++) {
        if (!names[i][0]) continue;
        const bool path = strchr(names[i], '/') != nullptr;
        void* o = nullptr;
        if (path) {
            const char* dot = strrchr(names[i], '.');
            o = CatchSound_FindSound(dot ? dot + 1 : names[i]);
            if (!o) o = Radio_LoadAsset(names[i]);
        } else o = CatchSound_FindSound(names[i]);
        if (!o) continue;
        SitUI_Track(&g_clapCueRef, o);
        if (!g_clapCueRef.obj) continue;
        g_clapCue = o;
        const char* nm = path ? (strrchr(names[i], '.') ? strrchr(names[i], '.') + 1 : names[i]) : names[i];
        snprintf(g_clapUsing, sizeof(g_clapUsing), "%s", nm);
        if (path) ClothMerge_RootObject(o, nm);          // stays loaded: other players' claps resolve to it
    }
    if (!g_clapCue) { g_clapUsing[0] = 0; TwkLog("[emote] clap: none of its sounds could be found or loaded -- a silent clap"); return; }
    const char* reach = "its own";
    if (_stricmp(g_clapAtten, "none") != 0) {
        void* at = Radio_LoadAsset(g_clapAtten[0] ? g_clapAtten : kClapAtten);
        if (!at && g_clapAtten[0]) at = Radio_LoadAsset(kClapAtten);
        if (at) {
            ClothMerge_RootObject(at, "clap falloff");
            reach = GiveFalloff(g_clapCue, g_clapUsing, at);
            // The whole cue too: an older player's clap plays it, and it is the game's hand landing (map-wide).
            if (_stricmp(g_clapUsing, "SCU_HandLand") != 0) {
                void* cue = CatchSound_FindObject("SCU_HandLand", "SoundCue");
                if (cue) TwkLog("[emote] clap: older players' claps (SCU_HandLand): %s", GiveFalloff(cue, "SCU_HandLand", at));
            }
        } else reach = "its own (the falloff asset could not be loaded)";
    }
    TwkLog("[emote] clap: the sound is '%s' at pitch x%.2f volume x%.2f, second strike %d ms; its reach: %s",
           g_clapUsing, g_clapPitch, g_clapVolume, g_clapDoubleMs, reach);
}
static void* ClapStrike(void* root, bool second) {
    if (!g_clapCue || !root) return nullptr;
    const float vol = g_clapVolume * (second ? 0.55f + 0.20f * Rand01() : 0.85f + 0.30f * Rand01());
    const float pitch = g_clapPitch * (second ? 1.04f + 0.08f * Rand01() : 0.95f + 0.10f * Rand01());
    return CatchSound_SpawnAttached(g_clapCue, root, vol, pitch);
}
void ClapSound(void* sk) {
    ResolveClap();
    void* root = sk ? twkP(sk, ACT_ROOT) : nullptr;
    if (!g_clapCue || !root) return;
    const float vol = g_clapVolume, pitch = g_clapPitch;
    void* ac = ClapStrike(root, false);
    // THE SECOND STRIKE, a moment later and pitched apart. The game has no clap asset, so a clap is
    // built out of SCU_HandLand -- which is the game's OWN hand-landing foley, played on your character
    // every time a hand touches down. One knock of it IS a hand landing, and the field report was
    // exactly that confusion: "I'm hearing the clapping emote on my own character when I'm not
    // clapping". Two quick strikes are a clap and nothing else is, so the emote stops borrowing another
    // sound's identity. EmoteClapDoubleMs = 0 returns it to the single knock.
    if (g_clapDoubleMs > 0) g_clapEchoAt = g_t + (float)g_clapDoubleMs / 1000.0f;
    // EVERY clap is logged, not the first three: the cap is what made "I hear clapping and nobody is
    // clapping" impossible to check against a log. Throttled so a long clap cannot flood one.
    g_clapCount++;
    static unsigned long long lastSaid = 0;
    const unsigned long long now = GetTickCount64();
    if (g_clapCount <= 3 || now - lastSaid > 2000) {
        lastSaid = now;
        TwkLog("[emote] clap #%d: volume %.2f pitch %.2f, %s", g_clapCount, vol, pitch, ac ? "played" : "NOT PLAYED");
    }
}
// The knock of the board on whatever is under its tail, as the game would make it (see the header).
void TapSound(void* sk, float hardness) {
    if (!g_audioLooked) { g_audioLooked = true; g_audioSurface = (AudioSurfaceFn)TwkScanExe(SIG_AUDIO_SURFACE);
                          if (!g_audioSurface) TwkLog("[emote] tap: no surface call in this build -- one sound for every ground"); }
    void* board = twkP(sk, SK_BOARD);
    void* data = board ? twkP(board, BD_AUDIO_DATA) : nullptr;
    void* root = board ? twkP(board, ACT_ROOT) : nullptr;
    // THE POP, if it is loaded (it is whenever there is a skater to ollie: the ollie's animation plays it)
    if (g_tapCue && !SitUI_Alive(&g_tapCueRef)) { g_tapCue = nullptr; g_tapCueTried = false; }
    if (!g_tapCue && !g_tapCueTried) {
        g_tapCueTried = true;
        void* o = g_tapSoundName[0] ? CatchSound_FindSound(g_tapSoundName) : nullptr;
        if (o) { SitUI_Track(&g_tapCueRef, o); if (g_tapCueRef.obj) g_tapCue = o; }
        TwkLog(g_tapCue ? "[emote] tap: the sound is '%s' (the pop), at volume x%.2f" : "[emote] tap: '%s' is not loaded here -- the board's own knock instead", g_tapSoundName, g_tapPopVolume);
    }
    void* cue = g_tapCue;
    const bool pop = cue != nullptr;
    if (!cue) { cue = data ? twkP(data, AD_BOARD_HIT) : nullptr; if (!cue && data) cue = twkP(data, AD_GROUND_HIT); }
    if (!cue || !root) { if (g_tapCount < 2) TwkLog("[emote] tap: nothing to play (audio data %p, cue %p)", data, cue); return; }
    float vol, pitch;
    if (pop) {
        // a pop is LOUD as the game mixes it -- it is the sound of the game -- so it is played at its own level, and a
        // soft set-down is a good deal quieter than a slam; its pitch is left very nearly alone
        vol = g_tapPopVolume * (0.35f + 0.65f * hardness);
        pitch = 0.97f + 0.06f * Rand01();
    } else {
        float maxVol = data ? twkF(data, AD_HIT_MAXVOL) : 1.0f, p0 = data ? twkF(data, AD_HIT_PITCH0) : 0.0f, p1 = data ? twkF(data, AD_HIT_PITCH0 + 4) : 0.0f;
        if (!(maxVol > 0.05f && maxVol < 8.0f)) maxVol = 1.0f;
        if (!(p0 > 0.3f && p0 < 3.0f && p1 >= p0 && p1 < 3.0f)) { p0 = 0.94f; p1 = 1.06f; }
        // THE FIELD: at the game's own level (x1, and 0.3 of it for a soft one) the knock was "pretty quiet" -- it is made
        // for a board clattering about by itself, heard past everything else. A multiplier over 1 is the engine's to honour.
        vol = maxVol * g_tapVolume * (0.40f + 0.60f * hardness); pitch = p0 + (p1 - p0) * Rand01();
    }
    void* ac = CatchSound_SpawnAttached(cue, root, vol, pitch);
    const int surface = g_tapSurface; const bool found = g_tapGroundSet;
    if (ac && found && g_audioSurface) { __try { g_audioSurface(ac, surface); } __except (EXCEPTION_EXECUTE_HANDLER) { g_audioSurface = nullptr; } }
    if (g_tapCount++ < 6) TwkLog("[emote] tap #%d (%s): hardness %.2f -> volume %.2f pitch %.2f, %s, ground surface %d%s", g_tapCount, pop ? "pop" : "knock", hardness, vol, pitch,
                                 ac ? "played" : "NOT PLAYED (no spawn call, or it refused)", surface, found ? "" : " (nothing found under the tail)");
}
// The stick works the board: the tail's height is a spring with weight in it, and coming down fast is a tap.
void PumpTap(void* sk, float dt) {
    if (sk && g_tapTipSet) {                        // what is under the tail, and what it is made of: one trace a frame
        const float a[3] = { g_tapTipW[0], g_tapTipW[1], g_tapTipW[2] + 55.0f }, b[3] = { g_tapTipW[0], g_tapTipW[1], g_tapTipW[2] - 90.0f };
        int surface = 0;
        g_tapGroundSet = Sit_TraceSurface(sk, a, b, g_tapGroundW, &surface);
        if (g_tapGroundSet) g_tapSurface = surface;
    }
    auto dead = [](float v) { return fabsf(v) < 0.14f ? 0.0f : (v - (v > 0.0f ? 0.14f : -0.14f)) / 0.86f; };
    const float y = dead(clampf(g_stickY, -1.0f, 1.0f)), x = dead(clampf(g_stickX, -1.0f, 1.0f));
    // HOW IT MOVES. The first cut was one stiff spring (k 640 coming down, plus a shove): 32 cm in about 60 ms,
    // FOUR TIMES FASTER THAN FALLING, which is what "abrupt" was. Now two things, as a hand has them:
    //  - HELD somewhere in the air (stick up, or carried for a walk): the arm takes it there, up OR down, on a
    //    soft spring that is nearly critically damped -- it arrives, it does not snap or ring;
    //  - LET DOWN (stick centred) or DRIVEN DOWN (stick down): not a spring at all but WEIGHT -- it has to reach
    //    the ground still moving, or there is no knock. Let go, it comes down a little under free fall (a hand is
    //    still on it): 32 cm in about a third of a second. Pulled down, up to about 2 g more.
    float target = y > 0.0f ? 32.0f * y : 0.0f;
    // WALKING, the tail is carried just clear of the ground, not dragged along it; stopping sets it down again
    g_tapWalk += ((g_speed > 35.0f ? 1.0f : 0.0f) - g_tapWalk) * clampf(dt * 6.0f, 0.0f, 1.0f);
    if (y >= 0.0f && target < 6.0f * g_tapWalk) target = 6.0f * g_tapWalk;
    if (y != 0.0f) g_tapStickLift = true;                               // the STICK has had a hand in it since it last landed
    float acc;
    if (target > 0.5f) {
        const float k = 120.0f;
        acc = k * (target - g_tapLift) - 2.0f * sqrtf(k) * 0.9f * g_tapVel;
    } else if (g_tapStickLift) {
        acc = -(650.0f + 1500.0f * (y < 0.0f ? -y : 0.0f)) - 1.2f * g_tapVel;
    } else {
        acc = -300.0f - 3.0f * g_tapVel;                                // set down after a walk, or at the start: lowered, not dropped
    }
    g_tapVel += acc * dt;
    g_tapLift += g_tapVel * dt;
    if (g_tapLift <= 0.0f) {
        const float speed = -g_tapVel;
        g_tapLift = 0.0f;
        if (speed > 28.0f && !g_tapDown) {                              // a LANDING, not a resting
            const float hardness = clampf((speed - 28.0f) / 300.0f, 0.0f, 1.0f);
            g_tapHit = 0.35f + 0.65f * hardness;
            TapSound(sk, hardness);
        }
        g_tapDown = true;
        g_tapStickLift = speed > 70.0f;                                 // ...and a bounce comes back down under its own weight, not lowered
        g_tapVel = speed > 70.0f ? speed * 0.15f : 0.0f;                // a small bounce off a hard one
    }
    if (g_tapLift > 2.5f) g_tapDown = false;                            // lifted clear: the next landing counts
    g_tapSwing += (x - g_tapSwing) * clampf(dt * 9.0f, 0.0f, 1.0f);
    g_tapHit *= expf(-dt * 8.0f);
}

} // namespace

// ------------------------------------------------------------------ the module's face
// The right stick is the tap's while a tap is up -- and ONLY while something is driving it: a pump that has
// stopped must never leave the camera's stick held (the radial menu's lesson, twice over).
// ---- BOARD TAP, ON RB -------------------------------------------------------------------------------
// A PRESS holds the board in the tap pose and leaves the camera alone -- you can look around while you
// stand there holding it. HOLDING RB then takes the right stick to work the tap, which necessarily
// locks the camera, because there is only one stick and it cannot be both.
// Pressing RB again while in the pose puts the board back down. A press that BEGAN the pose does not
// also end it, so the natural "hold it straight away" gesture is not read as an immediate cancel.
const LONGLONG kTapHoldMs = 220;                  // longer than a press, shorter than a deliberate hold
static bool     g_tapBtn = false, g_tapBeganHere = false;
static LONGLONG g_tapDownMs = 0;
// Is RB being HELD right now -- the stick is the tap's, and the camera is pinned?
// Close enough to the marker to be a RETURN to it, rather than any other placement? The co-op host fills in
// the same struct and sets the same flag to put you next to a peer, so "the flag went by" is not enough on
// its own -- but landing on the marker is.
bool MarkerLanded(float distCm) { return distCm >= 0.0f && distCm < 250.0f; }
// GAME THREAD, every pump frame. Watches two things and joins them: the marker being SET (its location
// changing while it says it is set) remembers whether the board was up; the pending-return flag going away
// means the game has just placed you, and the board goes back up a moment later -- after the reset, never
// into it.
void PumpMarker(void* sk, float dt) {
    if (!g_mkTapOn || !sk) return;
    // A NEW SKATER IS A NEW WORLD: the marker it had belongs to the level that has gone, and a stale one
    // would make some other placement look like a return to it.
    static void* mkFor = nullptr;
    if (sk != mkFor) {
        mkFor = sk; g_mkSeen = false; g_mkHasTap = false; g_mkPosOk = false;
        g_mkWait = -1.0f; g_mkTry = 0.0f; g_mkSaid = 0;
    }
    __try {
        void* pc  = twkP(sk, PAWN_CONTROLLER);
        void* smc = pc ? twkP(pc, PC_SPOT_MARKER) : nullptr;
        if (!smc) return;                                   // no controller of our own: a proxy, or not in play yet
        if (!g_mkFound) { g_mkFound = true; TwkLog("[emote] the game's marker controller found -- the marker can remember the board tap"); }
        // ---- SET: the controller's own marker moving. It is written when you save one, and at no other time.
        const bool has = twkB(smc, SMC_ACTIVE) != 0 || twkB(smc, SMC_INFO) != 0;
        const V3 loc = v3(twkF(smc, SMC_INFO + 4), twkF(smc, SMC_INFO + 8), twkF(smc, SMC_INFO + 12));
        if (has && (!g_mkSeen || len(sub(loc, g_mkLoc)) > 5.0f)) {
            g_mkSeen = true; g_mkLoc = loc;
            g_mkHasTap = (g_id == EM_TAP && !g_ending);
            TwkLog("[emote] session marker set%s", g_mkHasTap ? " with the board up: coming back to it puts the board back up" : "");
        }
        // ---- RETURN: spotted as what it physically is -- the skater JUMPS, and lands on the marker. Nothing
        // covers three metres in one frame by skating. The landing is what tells a marker return from the
        // co-op host putting you beside a peer, which uses the same pending struct and the same flag.
        void* root = twkP(sk, ACT_ROOT);
        const float* w = root ? (const float*)((const uint8_t*)root + SC_C2W) : nullptr;
        if (w) {
            const V3 now = v3(w[4], w[5], w[6]);
            if (g_mkPosOk) {
                const float moved = len(sub(now, g_mkPos));
                if (moved > 300.0f) {
                    const float d = g_mkSeen ? len(sub(now, g_mkLoc)) : -1.0f;
                    if (g_mkHasTap && MarkerLanded(d)) {
                        g_mkWait = g_mkDelay; g_mkTry = 1.5f;
                        TwkLog("[emote] marker return (a %.0f cm jump, landing %.0f cm from the marker): the board goes back up", moved, d);
                    } else if (g_mkSaid++ < 8) {
                        TwkLog("[emote] a %.0f cm jump, %.0f cm from the marker%s -- left as it is", moved, d,
                               g_mkHasTap ? "" : " (the marker was not set with the board up)");
                    }
                }
            }
            g_mkPos = now; g_mkPosOk = true;
        }
        if (g_mkWait > 0.0f) {
            g_mkWait -= dt;
            if (g_mkWait <= 0.0f) g_mkWait = 0.0f;           // ...the wait is up: from here it tries
        }
        // Keep trying for a moment once it is: the board may still be arriving in the hand after a return.
        if (g_mkWait == 0.0f && g_mkTry > 0.0f) {
            g_mkTry -= dt;
            if (g_id == EM_NONE && g_req == EM_NONE && !g_tapBtn && Sit_BoardInHand(sk)) {
                Emote_TapButton(true); Emote_TapButton(false);     // through the button's own gates, not round them
                if (g_id != EM_NONE || g_req != EM_NONE) { g_mkWait = -1.0f; g_mkTry = 0.0f; TwkLog("[emote] marker return: the board is up again"); }
            }
            if (g_mkTry <= 0.0f) { g_mkWait = -1.0f; TwkLog("[emote] marker return: the board could not go back up just now"); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
// GETTING ON THE BOARD (542: every emote, as 521 did for the throw). Either flag, whichever the game raises first
// -- and from the Y press itself (sit stamps it): the flags come up a beat after the press, and an emote asked for
// inside that beat still started in the middle of the throwdown.
static bool Mounting(void* sk, int* td1 = nullptr, int* td2 = nullptr) {
    bool m = Sit_MountPending();
    int a = -1, b = -1;
    void* ai = FootPlace_AnimInstance();
    __try { a = sk ? twkB(sk, SK_THROWDOWN) : -1; b = ai ? twkB(ai, AN_THROWING_DOWN) : -1; m = m || a > 0 || b > 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (td1) *td1 = a;
    if (td2) *td2 = b;
    return m;
}
static bool TapWorking() {
    return g_tapBtn && g_id == EM_TAP && !g_ending &&
           (LONGLONG)GetTickCount64() - g_tapDownMs >= kTapHoldMs;
}
void Emote_TapButton(bool down) {
    if (down == g_tapBtn) return;
    const LONGLONG now = (LONGLONG)GetTickCount64();
    g_tapBtn = down;
    if (down) {
        g_tapDownMs = now;
        g_tapBeganHere = false;
        if (g_id == EM_TAP && !g_ending) return;             // already in it: this press may end it on release
        void* sk = CatchTweaks_Skater();
        if (g_id != EM_NONE || g_req != EM_NONE) return;
        if (!g_on || !sk || Twk_IsProxy(sk) || !OnFoot(sk) || Sit_EditorOpen() || (Sit_PoseHeld() && !Sit_BoardKept())) return;
        if (Emote_Carrying() || Radial_Open() || Radial_Busy() || !Sit_BoardInHand(sk)) return;
        g_req = EM_TAP;
        g_tapBeganHere = true;
        TwkLog("[emote] board tap (RB)");
    } else if (!g_tapBeganHere && now - g_tapDownMs < kTapHoldMs && g_id == EM_TAP && !g_ending) {
        Emote_Stop();                                        // a second short press puts it away
    }
}
bool Emote_TapHeld() { return TapWorking(); }
bool Emote_WantsStick() {
    // ONLY WHILE RB IS HELD. The pose itself leaves the camera to you; taking the stick the whole time
    // the board is up would mean you could never look around while holding it.
    return TapWorking() && g_w > 0.0f && (LONGLONG)GetTickCount64() - g_pumpMs < 300;
}
void Emote_Stick(float rx, float ry) { g_stickX = rx; g_stickY = ry; }
// The right trigger, as the pad last reported it (it reports changes, not a stream) -- always kept, so a Rage
// knows whether it was already held when it began. It is the Rage's own, and nobody else's, while one is up.
void Emote_Trigger(float rt) { if (!g_trigPolled) g_trigger = clampf(rt, 0.0f, 1.0f); }      // the press, until the engine's own value is to be had
bool Emote_WantsTrigger() {
    return g_id == EM_RAGE && !g_ending && g_w > 0.0f && (LONGLONG)GetTickCount64() - g_pumpMs < 300;
}
// B puts ANY emote away -- and only that: whatever else B means (the seat's next position, sitting down) waits for
// the next press. False once it is already on its way out, and false with no pump: a stopped pump must never
// leave a button held (the radial menu's lesson).
// THE NEXT WAY OF DOING IT. The change-over is a blend of the two poses (PoseMix), so it can be taken at
// any moment of either.
void Emote_NextVariant() {
    const int n = (g_id >= 0 && g_id < EM_COUNT) ? kDefs[g_id].variants : 1;
    if (g_id == EM_NONE || g_ending || n <= 1) return;
    g_varPrev = g_var; g_var = (g_var + 1) % n; g_varMix = 0.0f;
    g_lastVar[g_id] = g_var;
    TwkLog("[emote] %s: %s", kDefs[g_id].name, g_id == EM_DANCE ? kDanceName[g_var] : "next");
}
// B, pressed and let go. A TAP is the next variation (or, for an emote that has only one way, the old
// "B stops it"); HELD past EmoteStopHoldMs the pump stops the emote, and this release has nothing left to do.
void Emote_StopButton(bool down) {
    if (down) {
        if (g_id == EM_NONE || g_ending) return;
        g_bDown = true; g_bUsed = false; g_bHeld = 0.0f;
        return;
    }
    if (!g_bDown) return;
    g_bDown = false;
    if (g_bUsed) return;                            // the hold already did it
    const int n = (g_id >= 0 && g_id < EM_COUNT) ? kDefs[g_id].variants : 1;
    if (n > 1) Emote_NextVariant(); else Emote_Stop();
}
float Emote_StopRing() { return g_bDown && !g_bUsed && g_stopHold > 0.01f ? clampf(g_bHeld / g_stopHold, 0.0f, 1.0f) : 0.0f; }
bool Emote_Stoppable() {
    return g_id != EM_NONE && g_id != EM_CARRY && !g_ending && (LONGLONG)GetTickCount64() - g_pumpMs < 300;      // a carry is the PROP's to end
}
// What the prompt bar says while an emote is held -- it is held until B, so B had better be on screen, and while
// one is up B is NOT the seat's "change how you sit". A Rage that is still aiming says what throws it.
int Emote_Prompts(SitPromptEntry* out, int cap) {
    if (out && cap >= 1 && g_id == EM_CARRY && !g_ending && g_w > 0.0f && (LONGLONG)GetTickCount64() - g_pumpMs < 300) {
        out[0] = { "Put radio down", 'B', 0.0f };          // a carry is not "an emote to stop": B is the prop's
        return 1;
    }
    if (!out || cap < 1 || !Emote_Stoppable() || g_w <= 0.0f) return 0;
    int n = 0;
    const bool aiming = g_id == EM_RAGE && !g_thrown && g_rageSwingAt < 0.0f;
    if (aiming) out[n++] = { "Throw", 'T', 0.0f };
    // The tap says what RB does now, because RB is what put the board up and it is not obvious that
    // holding it does something else again.
    if (g_id == EM_TAP && n < cap) out[n++] = { TapWorking() ? "Tapping" : "Hold to tap", 'S', 0.0f };
    if (n < cap) {
        const bool many = kDefs[g_id].variants > 1;
        out[n++] = { aiming ? "Cancel (hold)" : many ? "Next dance, hold to stop" : "Hold to stop",
                     'B', Emote_StopRing() };
    }
    return n;
}
// ---- the F1 "Board tap hand" page ------------------------------------------------------------------
// EVERY JOINT OF THE HAND THAT HOLDS THE BOARD, live. It began as a dev page: which axis of a hand is
// wrong is exactly what cannot be told from the code, and every round before it was an argument about
// axes settled by rebuilding the DLL. It stayed, and is named for what it does rather than for dev,
// because posing your own hand on the board turns out to be something people want.
// "Log this pose" prints the lot as ini keys, so a pose somebody likes can be kept or passed on.
// RENDER THREAD (the menu_ext contract). The knobs are plain floats the game thread reads next frame.
// THE FIELD'S OWN VALUES, which are also this page's Reset. Kept here rather than as literals in three
// places (the defaults above, the ini fallbacks, this button) so the three can never drift apart.
struct TapPose { float twist, pitch, roll, thumb, thAlong, thOut, thAcross; float curl[4]; float fwd, right, up; };
static const TapPose kTapShipped = { 12.0f, -45.0f, 15.0f, 0.50f, 2.9f, 4.2f, 3.0f,
                                     { 0.07f, 0.09f, 0.08f, 0.12f }, 3.0f, 0.0f, 0.0f };

void Emote_DrawTapHandMenu(const OmpMenuApi* api) {
    if (!api) return;
    api->Text("The hand that holds the board for a board tap");
    api->TextDisabled("Hold RB while off the board to see it. Every row is live, and saves itself.");
    // A CHANGE IS A SAVE, the same as every other setting here: `changed` collects what the rows report
    // and marks the file dirty once, so dragging a slider does not write the ini on every frame of the
    // drag (TwkMarkDirty settles for two seconds first).
    bool changed = false;
    api->Separator();
    api->Text("How the hand is turned");
    changed |= api->SliderFloat("Twist (palm turns around the fingers)", &g_tapHandTwist, -180.0f, 180.0f, "%.0f deg");
    changed |= api->SliderFloat("Pitch (the wrist cocks)",               &g_tapHandPitch, -180.0f, 180.0f, "%.0f deg");
    changed |= api->SliderFloat("Roll (fingers swing on the palm)",      &g_tapHandRoll,  -180.0f, 180.0f, "%.0f deg");
    api->Separator();
    api->Text("Where it holds the nose");
    changed |= api->SliderFloat("Forward", &g_tapGripFwd,   -30.0f, 30.0f, "%.1f cm");
    api->SameLine(); api->TextDisabled("(the arm still has to reach: see the log)");
    changed |= api->SliderFloat("Right",   &g_tapGripRight, -30.0f, 30.0f, "%.1f cm");
    changed |= api->SliderFloat("Up",      &g_tapGripUp,    -30.0f, 30.0f, "%.1f cm");
    api->Separator();
    api->Text("The thumb");
    changed |= api->SliderFloat("Thumb (0 open, 1 across the fingers)", &g_tapThumb,       0.0f,  1.0f, "%.2f");
    changed |= api->SliderFloat("Thumb along the fingers",              &g_tapThumbAlong, -10.0f, 10.0f, "%.1f cm");
    changed |= api->SliderFloat("Thumb out through the palm",           &g_tapThumbOut,   -10.0f, 10.0f, "%.1f cm");
    changed |= api->SliderFloat("Thumb across toward them",             &g_tapThumbAcross,-10.0f, 10.0f, "%.1f cm");
    // ...and the four the same way. The labels carry the finger's name so a row read on its own still
    // says what it moves, which matters in a list this long.
    static const char* const kFinger[4] = { "Index", "Middle", "Ring", "Little" };
    static const char* const kCurlRow[4] = { "Index (0 open, 1 a fist)", "Middle (0 open, 1 a fist)",
                                             "Ring (0 open, 1 a fist)",  "Little (0 open, 1 a fist)" };
    static const char* const kAlongRow[4] = { "Index along the fingers", "Middle along the fingers",
                                              "Ring along the fingers",  "Little along the fingers" };
    static const char* const kOutRow[4]   = { "Index out through the palm", "Middle out through the palm",
                                              "Ring out through the palm",  "Little out through the palm" };
    static const char* const kAcrossRow[4]= { "Index across toward the thumb", "Middle across toward the thumb",
                                              "Ring across toward the thumb",  "Little across toward the thumb" };
    for (int i = 0; i < 4; i++) {
        api->Separator();
        api->Text(kFinger[i]);
        changed |= api->SliderFloat(kCurlRow[i],   &g_tapCurl[i],          0.0f,  1.0f, "%.2f");
        changed |= api->SliderFloat(kAlongRow[i],  &g_tapFingerOff[i][0], -10.0f, 10.0f, "%.1f cm");
        changed |= api->SliderFloat(kOutRow[i],    &g_tapFingerOff[i][1], -10.0f, 10.0f, "%.1f cm");
        changed |= api->SliderFloat(kAcrossRow[i], &g_tapFingerOff[i][2], -10.0f, 10.0f, "%.1f cm");
    }
    api->Separator();
    if (api->version >= 2 && api->Button && api->Button("Log this pose")) {
        TwkLog("[emote] BOARD TAP HAND -- these are the ini keys, and this is their order in the file:");
        TwkLog("    EmoteTapHandTwistDeg=%.0f",   g_tapHandTwist);
        TwkLog("    EmoteTapHandPitchDeg=%.0f",   g_tapHandPitch);
        TwkLog("    EmoteTapHandRollDeg=%.0f",    g_tapHandRoll);
        TwkLog("    EmoteTapGripFwdCm=%.1f",      g_tapGripFwd);
        TwkLog("    EmoteTapGripRightCm=%.1f",    g_tapGripRight);
        TwkLog("    EmoteTapGripUpCm=%.1f",       g_tapGripUp);
        TwkLog("    EmoteTapThumbPct=%.0f",       g_tapThumb * 100.0f);
        TwkLog("    EmoteTapThumbAlongCm=%.1f",   g_tapThumbAlong);
        TwkLog("    EmoteTapThumbOutCm=%.1f",     g_tapThumbOut);
        TwkLog("    EmoteTapThumbAcrossCm=%.1f",  g_tapThumbAcross);
        static const char* const kKey[4] = { "Index", "Middle", "Ring", "Little" };
        for (int i = 0; i < 4; i++) {
            TwkLog("    EmoteTap%sPct=%.0f",      kKey[i], g_tapCurl[i] * 100.0f);
            TwkLog("    EmoteTap%sAlongCm=%.1f",  kKey[i], g_tapFingerOff[i][0]);
            TwkLog("    EmoteTap%sOutCm=%.1f",    kKey[i], g_tapFingerOff[i][1]);
            TwkLog("    EmoteTap%sAcrossCm=%.1f", kKey[i], g_tapFingerOff[i][2]);
        }
        g_tapSay = true;                 // ...and the solve's own line next time the pose is built
    }
    api->SameLine();
    if (api->version >= 2 && api->Button && api->Button("Back to the shipped pose")) {
        const TapPose& d = kTapShipped;
        g_tapHandTwist = d.twist; g_tapHandPitch = d.pitch; g_tapHandRoll = d.roll;
        g_tapGripFwd = d.fwd; g_tapGripRight = d.right; g_tapGripUp = d.up;
        g_tapThumb = d.thumb; g_tapThumbAlong = d.thAlong; g_tapThumbOut = d.thOut; g_tapThumbAcross = d.thAcross;
        for (int i = 0; i < 4; i++) {
            g_tapCurl[i] = d.curl[i];
            g_tapFingerOff[i][0] = g_tapFingerOff[i][1] = g_tapFingerOff[i][2] = 0.0f;
        }
        changed = true;
    }
    if (changed) TwkMarkDirty();
}

// Everything the F1 page can change, written back so a pose someone built survives the game closing.
// TwkIniSetInt APPENDS a key it cannot find, so a file written by an older build simply grows these.
void Emote_SaveConfig(char* buf, size_t cap) {
    TwkIniSetStr(buf, cap, "EmoteClapWave",        g_clapSound);          // the clap (ini only since 543)
    TwkIniSetInt(buf, cap, "EmoteClapPitchPct",    (int)(g_clapPitch * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteClapVolumePct",   (int)(g_clapVolume * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteClapDoubleMs",    g_clapDoubleMs);
    TwkIniSetStr(buf, cap, "EmoteClapAttenuation", g_clapAtten);
    TwkIniSetInt(buf, cap, "EmoteDanceBpm",   (int)(g_danceBpm + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteStopHoldMs", (int)(g_stopHold * 1000.0f + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteTapMarker",   g_mkTapOn);
    TwkIniSetInt(buf, cap, "EmoteTapMarkerMs", (int)(g_mkDelay * 1000.0f + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteTapHandTwistDeg",  (int)g_tapHandTwist);
    TwkIniSetInt(buf, cap, "EmoteTapHandPitchDeg",  (int)g_tapHandPitch);
    TwkIniSetInt(buf, cap, "EmoteTapHandRollDeg",   (int)g_tapHandRoll);
    TwkIniSetInt(buf, cap, "EmoteTapGripFwdCm",     (int)g_tapGripFwd);
    TwkIniSetInt(buf, cap, "EmoteTapGripRightCm",   (int)g_tapGripRight);
    TwkIniSetInt(buf, cap, "EmoteTapGripUpCm",      (int)g_tapGripUp);
    TwkIniSetInt(buf, cap, "EmoteTapThumbPct",      (int)(g_tapThumb * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "EmoteTapThumbAlongCm",  (int)g_tapThumbAlong);
    TwkIniSetInt(buf, cap, "EmoteTapThumbOutCm",    (int)g_tapThumbOut);
    TwkIniSetInt(buf, cap, "EmoteTapThumbAcrossCm", (int)g_tapThumbAcross);
    static const char* const kK[4] = { "Index", "Middle", "Ring", "Little" };
    static const char* const kAx[3] = { "AlongCm", "OutCm", "AcrossCm" };
    char key[64];
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "EmoteTap%sPct", kK[i]);
        TwkIniSetInt(buf, cap, key, (int)(g_tapCurl[i] * 100.0f + 0.5f));
        for (int x = 0; x < 3; x++) {
            snprintf(key, sizeof(key), "EmoteTap%s%s", kK[i], kAx[x]);
            TwkIniSetInt(buf, cap, key, (int)g_tapFingerOff[i][x]);
        }
    }
}

void Emote_ReadConfig(const char* buf) {
    { const int bpm = TwkIniIntQuiet(buf, "EmoteDanceBpm", 120);   g_danceBpm = (float)(bpm < 50 ? 50 : bpm > 220 ? 220 : bpm); }
    { const int ms  = TwkIniIntQuiet(buf, "EmoteStopHoldMs", 280); g_stopHold = (float)(ms < 80 ? 80 : ms > 1500 ? 1500 : ms) * 0.001f; }
    g_mkTapOn = TwkIniIntQuiet(buf, "EmoteTapMarker", 1) ? 1 : 0;
    { const int ms = TwkIniIntQuiet(buf, "EmoteTapMarkerMs", 350); g_mkDelay = (float)(ms < 0 ? 0 : ms > 3000 ? 3000 : ms) * 0.001f; }
    g_on = TwkIniIntQuiet(buf, "EmotesEnabled", 1) ? 1 : 0;
    g_tapVolume = clampf((float)TwkIniIntQuiet(buf, "EmoteTapVolumePct", 250), 0.0f, 400.0f) / 100.0f;
    // HOW FAST A PULL COUNTS AS HARD, in tenths of a trigger-unit per second (15 = 1.5/s, a deliberate
    // squeeze; 100 = 10/s, a brisk snap). Tenths because the ini holds integers.
    kRateSoft = clampf((float)TwkIniIntQuiet(buf, "EmoteThrowSoftRateX10",  15), 1.0f, 300.0f) * 0.1f;
    kRateHard = clampf((float)TwkIniIntQuiet(buf, "EmoteThrowHardRateX10", 100), 2.0f, 600.0f) * 0.1f;
    if (kRateHard < kRateSoft + 0.2f) kRateHard = kRateSoft + 0.2f;      // never a divide by nothing in RageStrength
    g_tapRollDeg = clampf((float)TwkIniIntQuiet(buf, "EmoteTapRollDeg", 0), -180.0f, 180.0f);
    g_tapHandRoll  = clampf((float)TwkIniIntQuiet(buf, "EmoteTapHandRollDeg",  (int)kTapShipped.roll),  -180.0f, 180.0f);
    g_tapHandTwist = clampf((float)TwkIniIntQuiet(buf, "EmoteTapHandTwistDeg", (int)kTapShipped.twist), -180.0f, 180.0f);
    g_tapHandPitch = clampf((float)TwkIniIntQuiet(buf, "EmoteTapHandPitchDeg", (int)kTapShipped.pitch), -180.0f, 180.0f);
    // THE BOARD TAP'S HAND. Every row of the F1 page has a key here, and the defaults ARE kTapShipped --
    // the pose the field settled on -- so a fresh install stands where the tuning ended rather than at
    // some neutral that was never looked at.
    const TapPose& d = kTapShipped;
    g_tapThumb       = clampf((float)TwkIniIntQuiet(buf, "EmoteTapThumbPct", (int)(d.thumb * 100.0f)), 0.0f, 100.0f) / 100.0f;
    g_tapThumbAlong  = clampf((float)TwkIniIntQuiet(buf, "EmoteTapThumbAlongCm",  (int)d.thAlong),  -10.0f, 10.0f);
    g_tapThumbOut    = clampf((float)TwkIniIntQuiet(buf, "EmoteTapThumbOutCm",    (int)d.thOut),    -10.0f, 10.0f);
    g_tapThumbAcross = clampf((float)TwkIniIntQuiet(buf, "EmoteTapThumbAcrossCm", (int)d.thAcross), -10.0f, 10.0f);
    {
        static const char* const kK[4] = { "Index", "Middle", "Ring", "Little" };
        char key[64];
        for (int i = 0; i < 4; i++) {
            snprintf(key, sizeof(key), "EmoteTap%sPct", kK[i]);
            g_tapCurl[i] = clampf((float)TwkIniIntQuiet(buf, key, (int)(d.curl[i] * 100.0f)), 0.0f, 100.0f) / 100.0f;
            static const char* const kAx[3] = { "AlongCm", "OutCm", "AcrossCm" };
            for (int x = 0; x < 3; x++) {
                snprintf(key, sizeof(key), "EmoteTap%s%s", kK[i], kAx[x]);
                g_tapFingerOff[i][x] = clampf((float)TwkIniIntQuiet(buf, key, 0), -10.0f, 10.0f);
            }
        }
    }
    g_tapGripFwd   = clampf((float)TwkIniIntQuiet(buf, "EmoteTapGripFwdCm",   (int)kTapShipped.fwd),   -30.0f, 30.0f);
    g_tapGripRight = clampf((float)TwkIniIntQuiet(buf, "EmoteTapGripRightCm", (int)kTapShipped.right), -30.0f, 30.0f);
    g_tapGripUp    = clampf((float)TwkIniIntQuiet(buf, "EmoteTapGripUpCm",    (int)kTapShipped.up),    -30.0f, 30.0f);
    // the tap's sound: a cue's short name (the ollie's pop; empty = the board's own knock) and its level. The older
    // EmoteTapVolumePct is the KNOCK's, which needed lifting; the pop does not.
    TwkIniStr(buf, "EmoteTapSound", g_tapSoundName, sizeof(g_tapSoundName), "SCU_Pop_Hi_Jump");
    g_tapPopVolume = clampf((float)TwkIniIntQuiet(buf, "EmoteTapPopVolumePct", 100), 0.0f, 400.0f) / 100.0f;
    // the clap's sound: a short name or a full path (empty = the built-in list), how it is played, and its
    // falloff (a SoundAttenuation path; empty = walking's; "none" = the sound's own). Saved since 539; ini only (543).
    TwkIniStr(buf, "EmoteClapWave", g_clapSound, sizeof(g_clapSound), "WAV_Cinematic_HandLand_03");   // 541 (was EmoteClapSound)
    g_clapPitch  = clampf((float)TwkIniIntQuiet(buf, "EmoteClapPitchPct", 200), 40.0f, 300.0f) / 100.0f;
    g_clapVolume = clampf((float)TwkIniIntQuiet(buf, "EmoteClapVolumePct", 400), 0.0f, 400.0f) / 100.0f;
    g_clapDoubleMs = (int)clampf((float)TwkIniIntQuiet(buf, "EmoteClapDoubleMs", 0), 0.0f, 400.0f);
    TwkIniStr(buf, "EmoteClapAttenuation", g_clapAtten, sizeof(g_clapAtten), "");
}
int  Emote_Count() { BuildWheelIndex(); return g_wheelN; }   // only the ones ON the wheel
const char* Emote_Name(int i) { BuildWheelIndex(); return (i >= 0 && i < g_wheelN) ? kDefs[g_wheelIdx[i]].name : ""; }
// A WHEEL index, not an emote index -- see BuildWheelIndex.
int Emote_FromWheel(int i) { BuildWheelIndex(); return (i >= 0 && i < g_wheelN) ? g_wheelIdx[i] : EM_NONE; }
// THE CHEST BONE'S NAME, off ANY character's mesh -- another player's, whose radio is hung from it. Worked out from
// that mesh's own bone table with NOTHING of this module's state touched (the rig here is the local skater's, and
// an emote may be mid-pose on it): pelvis, then the chain of "spine" children, the last of them -- as ResolveNames
// has it. Every character shares the names, and an FName is good for the life of the process, so the first answer
// is kept.
unsigned long long Emote_ChestBoneOf(void* meshComp) {
    static unsigned long long cached = 0;
    if (cached) return cached;
    if (!meshComp) return 0;
    __try {
        void* skel = twkP(meshComp, SKM_MESH);
        if (!skel) return 0;
        const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
        const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
        const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
        if (!info || n <= 0 || n > MAX_BONES) return 0;
        static char nm[MAX_BONES][64]; static int parent[MAX_BONES];
        for (int i = 0; i < n; i++) {
            char nb[96];
            if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) nb[0] = 0;
            for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
            snprintf(nm[i], sizeof(nm[i]), "%s", nb);
            parent[i] = *(const int*)(info + i * 12 + 8);
        }
        int cur = -1;
        for (int i = 0; i < n && cur < 0; i++) if (strstr(nm[i], "pelvis")) cur = i;
        int chest = -1;
        for (int hop = 0; hop < 4 && cur >= 0; hop++) {
            int next = -1;
            for (int i = 0; i < n && next < 0; i++) if (parent[i] == cur && strstr(nm[i], "spine")) next = i;
            if (next < 0) break;
            chest = next; cur = next;
        }
        if (chest < 0) return 0;
        cached = *(const unsigned long long*)(info + chest * 12);
        TwkLog("[emote] the chest bone is %s", nm[chest]);
        return cached;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// ---- A CARRIED PROP (radio.cpp). The pose is an emote like any other -- one at a time, so a carry and a wave do
// not mix: while something is carried the wheel's emotes are refused ("put it down first"), and B is not "stop
// emote" but the prop's own (it is put down). `origin` and `extent` are the mesh's bounds in its own space.
bool Emote_CarryStart(const float origin[3], const float extent[3]) {
    void* sk = CatchTweaks_Skater();
    if (!g_on || !sk || Twk_IsProxy(sk) || !OnFoot(sk) || Sit_EditorOpen() || Sit_PoseHeld()) return false;
    if (Mounting(sk)) return false;                            // not in the middle of getting on (542)
    for (int i = 0; i < 3; i++) { g_boxO[i] = origin[i]; g_boxE[i] = extent[i] > 1.0f ? extent[i] : 1.0f; }
    g_boxWorldOk = false;
    g_req = EM_CARRY;
    return true;
}
void Emote_CarryStop() { if (g_id == EM_CARRY) Emote_Stop(); if (g_req == EM_CARRY) g_req = EM_NONE; }
bool Emote_Carrying() { return (g_id == EM_CARRY && !g_ending) || g_req == EM_CARRY; }
// Where the carried prop's ACTOR belongs this frame (world), and how far the pose is blended in; the chest bone's
// name, to hang it from once it is there. false until the pose has been up for a frame.
bool Emote_CarryPlace(float posW[3], float quatW[4], float* weight, uint64_t* chestBone) {
    if (g_id != EM_CARRY || !g_boxWorldOk || !g_rigOk || g_nSpine < 1) return false;
    for (int i = 0; i < 3; i++) posW[i] = g_boxPosW[i];
    for (int i = 0; i < 4; i++) quatW[i] = g_boxQuatW[i];
    if (weight) *weight = g_ending ? 0.0f : g_w;
    if (chestBone) *chestBone = g_boneName[g_spine[g_nSpine - 1]];
    return true;
}
bool Emote_Active()   { return g_id != EM_NONE; }
bool Emote_PoseHeld() { return g_id != EM_NONE && g_w > 0.0f; }
void Emote_Stop() { if (g_id != EM_NONE && !g_ending) { g_ending = true; g_outTime = kBlendOut; g_next = EM_NONE; } }
const char* Emote_WhyNot() { return g_whyNot; }
// `wheelIndex` is a place ON THE WHEEL, which is not the same as an emote's number now that some are
// hidden from it (see BuildWheelIndex).
bool Emote_Play(int wheelIndex) {
    g_whyNot = "Not right now";
    const int index = Emote_FromWheel(wheelIndex);
    if (!g_on || index < 0 || index >= EM_WHEEL) return false;
    if (Emote_Carrying()) { g_whyNot = "Put the radio down first"; return false; }
    void* sk = CatchTweaks_Skater();
    if (!sk || Twk_IsProxy(sk) || !OnFoot(sk) || Sit_EditorOpen()) { TwkLog("[emote] %s: not now (on the board, in an editor, or no skater)", kDefs[index].name); return false; }
    if (Mounting(sk)) { g_whyNot = "Getting on the board"; TwkLog("[emote] %s: not while getting on the board", kDefs[index].name); return false; }
    if (kDefs[index].board && !Sit_BoardInHand(sk)) {
        g_whyNot = Sit_BoardOut() ? "Get your board back first (Y)" : "You need your board in hand";
        TwkLog("[emote] %s: the board is not in hand", kDefs[index].name);
        return false;
    }
    g_req = index;                   // taken up by the pump, which is the only place the state changes
    return true;
}
// ---- THE THROW, ON THE LEFT TRIGGER ------------------------------------------------------------------
// Not a wheel entry any more: off the board, with a board in hand, HOLDING the left trigger arms the
// throw -- the arm comes up and stays up for as long as it is held -- and the right trigger throws,
// as hard as it is pulled (RageTrigger, unchanged). Letting go of the left without throwing puts the
// arm down again.
// WHY A HOLD rather than a press: the pose is a wind-up you aim from, so the natural gesture is to
// hold it while you line the throw up. It also means there is no state to get stuck in -- release the
// trigger and it is over -- which a press-to-arm would need a second button to undo.
// Once the board has LEFT the hand the emote runs itself out (it is the one that ends itself), so the
// left trigger is not consulted again until the next throw.
const float kArmOn = 0.35f, kArmOff = 0.20f;    // hysteresis: a trigger resting near the edge must not flutter
static bool g_armed = false;
static bool g_armSaid = false;             // this press's refusal is in the log (a refused press retries every frame)
static void PollArmTrigger(void* sk) {
    float lt = 0.0f;
    if (!CatchTweaks_LeftTrigger(&lt)) {                     // no analogue value to be had: leave it alone
        static ULONGLONG s_ms = 0; const ULONGLONG now = GetTickCount64();
        if (now - s_ms > 10000) { s_ms = now; TwkLog("[emote] throw: the left trigger cannot be read -- %s", CatchTweaks_TriggerWhy()); }
        return;
    }
    if (lt < kArmOff) g_armSaid = false;
    const bool wants = g_armed ? (lt > kArmOff) : (lt > kArmOn);
    if (wants == g_armed) return;
    g_armed = wants;
    if (g_armed) {
        // The same gates the wheel applied, minus the wheel: on foot, board in hand, nothing else going on.
        // Every refusal names itself, once per press ("why cant I throw my board?" -- they were all silent).
        const char* why = nullptr;
        if (g_id != EM_NONE || g_req != EM_NONE) why = "an emote is already running or asked for";
        else if (!g_on) why = "emotes are off";
        else if (!sk || Twk_IsProxy(sk)) why = "no local skater";
        else if (!OnFoot(sk)) why = "not on foot";
        else if (Sit_EditorOpen()) why = "a replay or the prop editor is open";
        else if (Sit_PoseHeld() && !Sit_BoardKept()) why = "the sit pose is still held";
        else if (Emote_Carrying()) why = "carrying the radio";
        else if (Radial_Open() || Radial_Busy()) why = "the radial menu or the shop is up";
        else if (!Sit_BoardInHand(sk)) why = Sit_BoardOut() ? "the board is out (thrown, not picked up)" : "the board is not in hand";
        if (why) {
            g_armed = false;
            if (!g_armSaid) {
                g_armSaid = true;
                TwkLog("[emote] throw NOT armed (left trigger %.2f): %s | emote %d req %d, sit pose %d kept %d, radial %d/%d", lt, why,
                       (int)g_id, (int)g_req, Sit_PoseHeld() ? 1 : 0, Sit_BoardKept() ? 1 : 0, Radial_Open() ? 1 : 0, Radial_Busy() ? 1 : 0);
            }
            return;
        }
        // Not while getting on the board (Y): the board is still in hand and you are still on foot for the
        // start of the throwdown, so LT armed a throw in the middle of it. Either flag, whichever the game
        // raises first -- and from the Y press itself (sit stamps it): the flags come up a beat after the
        // press, and LT inside that beat still armed one (3.19.521, "press Y then LT right after").
        {
            int td1 = -1, td2 = -1;
            if (Mounting(sk, &td1, &td2)) {
                g_armed = false;
                if (!g_armSaid) { g_armSaid = true; TwkLog("[emote] throw NOT armed (left trigger %.2f): getting on the board (Y pressed %s, throwdown flags %d/%d)", lt,
                                                           Sit_MountPending() ? "just now" : "not recently", td1, td2); }
                return;
            }
        }
        g_req = EM_RAGE;
        TwkLog("[emote] throw armed (left trigger)");
    } else if (g_id == EM_RAGE && !g_ending && !g_thrown) {
        Emote_Stop();                                        // let go without throwing: the arm comes down
    }
}

void Emote_Watchdog() {
    if (g_id == EM_NONE) return;
    const LONGLONG last = g_pumpMs;
    if (last && (LONGLONG)GetTickCount64() - last > 300) { Drop("nothing is driving it (paused, or an editor opened)"); Twk_SetPoseHold(Sit_PoseHeld()); }
}
// ---- THE AIM (camera_height.cpp reads it): while a throw is up and has not gone, the camera eases over the
// shoulder AWAY from the throwing arm -- so the wind-up does not swing through the view -- and back out after.
static float g_aimW = 0.0f;
static int   g_aimSide = 1;                  // +1 over the right shoulder, -1 the left
static void PumpAim(float dt) {
    const bool want = g_id == EM_RAGE && !g_ending && !g_thrown;
    if (want && g_carry >= 0) g_aimSide = g_carry == 1 ? -1 : 1;
    const float rate = want ? 1.0f / 0.22f : 1.0f / 0.45f;
    g_aimW += clampf((want ? 1.0f : 0.0f) - g_aimW, -dt * rate, dt * rate);
}
// An emote is up (or asked for): Y must not get on the board under it (542). The carry (the radio's) and the
// board tap (543: "board tap is an action/state not a damn emote") are not emotes to the player and keep their
// own rules; one already blending out lets Y through.
static bool NotAnEmote(int id) { return id == EM_CARRY || id == EM_TAP; }
bool Emote_BlocksMount() {
    return (g_req != EM_NONE && !NotAnEmote(g_req)) || (g_id != EM_NONE && !NotAnEmote(g_id) && !g_ending);
}
bool Emote_ThrowHeld() {
    return g_armed || g_req == EM_RAGE || (g_id == EM_RAGE && !g_ending && !g_thrown);
}
bool Emote_AimView(float* weight, int* side) {
    if (g_aimW <= 0.0005f) return false;
    *weight = smooth(g_aimW); *side = g_aimSide;
    return true;
}
static const float kOutAt[4] = { 0.1f, 0.3f, 1.0f, 4.0f };   // when a thrown board says where it is
static float g_outPrev[3] = { 0, 0, 0 }, g_outPrevT = 0.0f; static bool g_outPrevOk = false;
void Emote_PumpFrame() {
    const LONGLONG nowMs = (LONGLONG)GetTickCount64();
    const bool gap = g_pumpMs && nowMs - g_pumpMs > 300;
    g_pumpMs = nowMs;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (g_qpf == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart; }
    float dt = g_qpc ? (float)((double)(q.QuadPart - g_qpc) / g_qpf) : 0.0f;
    g_qpc = q.QuadPart;
    if (dt > 0.1f) dt = 0.1f;

    void* sk = CatchTweaks_Skater();
    // THE CLAP'S SOUND, loaded and given its reach as soon as there is a skater -- not at the first clap:
    // another player's clap resolves by name, so it has to be in memory here before anyone claps (539).
    {
        static ULONGLONG s_clapLookMs = 0;
        const ULONGLONG ms = GetTickCount64();
        if (sk && ms - s_clapLookMs > 2000) {
            s_clapLookMs = ms;
            static int s_retries = 0;                               // a failed first look (assets not up yet) tries again
            if ((g_clapCue && !SitUI_Alive(&g_clapCueRef)) || (!g_clapCue && g_clapCueTried && s_retries < 5)) { g_clapCueTried = false; if (!g_clapCue) s_retries++; }
            __try { ResolveClap(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_clapCue = nullptr; }
        }
    }
    PollArmTrigger(sk);                  // holding the left trigger arms a throw; letting go lowers the arm
    PumpAim(dt);                         // ...and the camera goes over the shoulder while it is up
    PumpMarker(sk, dt);                  // the marker remembers the board tap, and a return puts it back up
    // ---- a seat that keeps the board: the rig and the board's measurements SeatBoard needs, emote or none
    {
        void* seatMesh = (sk && Sit_PoseHeld() && Sit_BoardInHand(sk)) ? twkP(sk, CH_MESH) : nullptr;
        if (!seatMesh) { g_seatMesh = nullptr; g_seatSpot = false; g_seatSet = 0.0f; }
        else if (seatMesh != g_seatMesh || !SitUI_Alive(&g_seatMeshRef)) {
            g_seatMesh = nullptr;
            bool ok = false;
            if (g_id != EM_NONE && g_mesh == seatMesh) ok = g_rigOk;         // the running emote read this very body
            else { __try { ok = ResolveRig(seatMesh); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; g_faults++; } }
            if (ok && g_nBoardRoot) {
                SitUI_Track(&g_seatMeshRef, seatMesh);
                if (g_seatMeshRef.obj) {
                    g_seatMesh = seatMesh; g_seatMeasureT = 0.0f;
                    g_visSay = true;
                    __try { MeasureBoard(sk, seatMesh, true); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
                }
            }
        } else if (g_id == EM_NONE && (g_seatMeasureT += dt) > 0.5f) {
            g_seatMeasureT = 0.0f;
            __try { MeasureBoard(sk, seatMesh, false); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
        }
    }
    // ---- a thrown board: the throw's second frame, and everything that brings it back
    if (Sit_BoardOut()) {
        g_outS += dt;
        if (g_kickLeft > 0) { g_kickLeft--; Sit_BoardKick(g_kickVel, g_rageSpin); }      // physics may not have been up for the first
        // IT STAYS WHERE IT LANDED until it is asked for (Y: sit.cpp's key hook), as the game's own does when
        // getting on is blocked. The first cut walked it back into the hand at arm's length or after 30 s;
        // that is gone. What is left is only what MUST give it back: no skater, the game getting on it by
        // itself, and a board that has left the world (a fall into the void ends in the actor being destroyed).
        float bw[3];
        if (!sk)                          Sit_BoardBack("the skater is gone");
        else if (!OnFoot(sk))             Sit_BoardBack("getting on the board");
        else if (Sit_BoardWhere(bw)) {
            const float* w = twkP(sk, ACT_ROOT) ? (const float*)((const uint8_t*)twkP(sk, ACT_ROOT) + SC_C2W) : nullptr;
            const float d = w ? len(v3(bw[0] - w[4], bw[1] - w[5], bw[2] - w[6])) : 0.0f;
            if (w && (d > 15000.0f || bw[2] < w[6] - 6000.0f)) Sit_BoardBack("it has left the world");
            else if (g_outSaid < 4 && g_outS > kOutAt[g_outSaid]) {    // did it get AWAY, did it FLY, where did it stop
                g_outSaid++;
                const float sp = (g_outPrevOk && g_outS > g_outPrevT) ? len(v3(bw[0] - g_outPrev[0], bw[1] - g_outPrev[1], bw[2] - g_outPrev[2])) / (g_outS - g_outPrevT) : -1.0f;
                TwkLog("[emote] Throw board: %.1f s on, the board is %.0f cm away, going %.0f cm/s%s", g_outS, d, sp, Sit_PoseHeld() ? " (seated)" : "");
            }
            g_outPrev[0] = bw[0]; g_outPrev[1] = bw[1]; g_outPrev[2] = bw[2]; g_outPrevT = g_outS; g_outPrevOk = true;
        }
    } else { g_outS = 0.0f; g_outPrevOk = false; }
    if (g_id != EM_NONE) {
        if (gap)                                   Drop("the pump had stopped");
        else if (!sk || sk != g_skater)            Drop("the skater is gone");
        else if (!SitUI_Alive(&g_meshRef) || twkP(sk, CH_MESH) != g_mesh) Drop("the body changed");
        else if (Sit_EditorOpen())                 Drop("an editor opened");
    }
    if (g_req != EM_NONE && g_req != EM_CARRY && g_req != EM_TAP && g_id == EM_NONE && Mounting(sk)) {
        TwkLog("[emote] %s: dropped -- getting on the board", kDefs[g_req].name);
        g_req = EM_NONE;
    }
    if (g_req != EM_NONE) {
        const int want = g_req; g_req = EM_NONE;
        if (g_id == EM_NONE) { if (sk && Begin(want, sk)) TwkLog("[emote] %s", kDefs[want].name); }
        else if (want == g_id && kDefs[g_id].loop) { Emote_Stop(); }                 // picked again: that is how a loop is left
        else { g_next = want; g_ending = true; g_outTime = kBlendSwap; }             // a quick hand-over to the next one
    }
    if (g_id == EM_NONE) return;

    g_seated = Sit_PoseHeld();
    V3 v = v3(twkF(twkP(sk, CH_MOVE), MOVE_VEL), twkF(twkP(sk, CH_MOVE), MOVE_VEL + 4), 0.0f);
    g_speed = (v.x < -900000.0f || v.y < -900000.0f) ? 0.0f : len(v);      // the safe reader's fault value is not a sprint
    const Def& d = kDefs[g_id];
    if (!OnFoot(sk) && !g_ending) { g_ending = true; g_outTime = 0.15f; g_next = EM_NONE; }      // the board's own animation takes the arms back
    const float lowerWant = (d.lower && !g_seated && g_speed < 25.0f) ? 1.0f : 0.0f;
    g_lowerW += clampf(lowerWant - g_lowerW, -dt * 4.0f, dt * 4.0f);
    if (d.loop && d.lower && !g_ending) {           // walking off is how a DANCE ends: it has the legs. A tap has not, and walks with you
        g_moveS = g_speed > 70.0f ? g_moveS + dt : 0.0f;
        if (g_moveS > 0.35f) Emote_Stop();
    }
    g_t += dt;
    if (g_varMix < 1.0f) { g_varMix += dt / kVarBlend; if (g_varMix > 1.0f) g_varMix = 1.0f; }
    if (g_bDown && !g_bUsed) {                     // held long enough: that is the emote put away
        g_bHeld += dt;
        if (g_bHeld >= g_stopHold) { g_bUsed = true; Emote_Stop(); }
    }
    g_boardInHand = Sit_BoardInHand(sk);
    if (g_id == EM_TAP && !g_ending) {
        if (!g_boardInHand)            Emote_Stop();                    // the board went somewhere (a seat, a throw)
        else {                                                          // it stays until it is put away: B, picked again, or the board goes
            __try { MeasureBoard(sk, g_mesh, false); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
            PumpTap(sk, dt);
        }
    }
    if (g_id == EM_CLAP && !g_ending) {             // the hands meet: that is when it is heard
        const float ph = ClapPhase(g_t);
        // ...and ONLY if the clap is actually on the skeleton. See PoseIsLive: the pump and the pose
        // seam can come apart, and a clap nobody can see must not be a clap everybody can hear.
        if (g_w > 0.6f && g_t >= kClapStart && g_clapPhase < kClapContact && ph >= kClapContact) {
            if (PoseIsLive(EM_CLAP)) ClapSound(sk);
            else if (g_clapMuted++ < 4)
                TwkLog("[emote] clap: NOT played -- the clap is not on the skeleton just now "
                       "(posed=%d, %lld ms ago, w=%.2f). This is the guard, not a fault.",
                       g_posedId, (long long)((LONGLONG)GetTickCount64() - (LONGLONG)g_posedMs), g_w);
        }
        g_clapPhase = ph;
        // the second strike of the same clap -- armed by the first, and held to the same "is it really
        // on the skeleton" rule, so stopping mid-clap cannot leave one behind
        if (g_clapEchoAt >= 0.0f && g_t >= g_clapEchoAt) {
            g_clapEchoAt = -1.0f;
            void* root = sk ? twkP(sk, ACT_ROOT) : nullptr;
            if (g_clapCue && root && PoseIsLive(EM_CLAP)) ClapStrike(root, true);
        }
    }
    if (g_id == EM_RAGE && !g_ending && !g_thrown) { PollTrigger(); RageTrigger(g_t); }
    // the moment of a Rage: the real board leaves the hand, the way the pose said it was going, as hard as it was pulled
    if (g_id == EM_RAGE && !g_thrown && g_rageSwingAt >= 0.0f && g_t >= g_rageSwingAt + kRageLetGo && g_throwDirSet && g_w > 0.5f) {
        const float strength = RageStrength(g_rageRate);
        g_rageSpeed = RageSpeed(g_rageRate) * (0.97f + 0.06f * Rand01());
        for (int i = 0; i < 3; i++) { g_kickVel[i] = g_throwDirW[i] * g_rageSpeed; g_rageSpin[i] *= 0.35f + 0.65f * strength; }
        TwkLog("[emote] Throw board: pulled at %.1f /s (%.0f%% of the way to hard, %s), aimed %.0f deg -> %.0f cm/s",
               g_rageRate, 100.0f * strength, g_trigPolled ? "analogue" : "ONLY ITS PRESS: no analogue value to be had",
               g_ragePitch * 57.2957795f, g_rageSpeed);
        if (Sit_BoardThrow(sk, g_kickVel, g_rageSpin)) { g_thrown = true; g_kickLeft = 1; g_outS = 0.0f; g_outSaid = 0; }
        else { g_thrown = true; TwkLog("[emote] Throw board: the board could not be let go of -- it stays in hand"); }
    }
    if (!d.loop && !g_ending && g_t >= d.dur - kBlendOut) { g_ending = true; g_outTime = kBlendOut; }
    if (g_id == EM_RAGE && !g_ending && RageDone(g_t)) { g_ending = true; g_outTime = kBlendOut; g_next = EM_NONE; }      // the one that ends itself
    if (g_ending) {
        g_w -= dt / (g_outTime > 0.01f ? g_outTime : 0.01f);
        if (g_w <= 0.0f) {
            const int next = g_next;
            Drop(next == EM_NONE ? "done" : nullptr);
            if (next != EM_NONE && sk && Begin(next, sk)) TwkLog("[emote] %s", kDefs[next].name);
        }
    } else if (g_w < 1.0f) { g_w += dt / (g_id == EM_TAP ? 0.36f : kBlendIn); if (g_w > 1.0f) g_w = 1.0f; }      // a regrip takes a moment longer
}
void Emote_OnFlip(void* mesh) {
    if (mesh && mesh == g_seatMesh) {                // first the seat's board, so an emote on top reads it where it lies
        if (g_rigOk && (LONGLONG)GetTickCount64() - g_pumpMs <= 300 && !Sit_EditorOpen()) {
            __try { SeatBoard(mesh); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_faults++; g_seatMesh = nullptr; g_seatSpot = false; g_seatSet = 0.0f;
                TwkLog("[emote] fault laying the board down -- it stays in the hand");
            }
        } else { g_seatSpot = false; g_seatSet = 0.0f; }
    }
    if (g_id == EM_NONE || mesh != g_mesh || g_w <= 0.0f || !g_rigOk) return;
    if ((LONGLONG)GetTickCount64() - g_pumpMs > 300) return;      // nobody is driving the clock: the skeleton is not ours to write
    if (Sit_EditorOpen()) return;
    __try { Apply(mesh); g_posedId = g_id; g_posedMs = GetTickCount64(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        TwkLog("[emote] fault while posing -- stopped");
        g_id = EM_NONE; g_w = 0.0f; g_mesh = nullptr;
    }
}
