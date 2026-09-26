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
// SessionTweaks -- GRIND LEAN.
//
// The game leans the rider with BODY BANKING: USkaterMovementComponent::UpdateBodyBanking eases three
// angles -- _currentBankingLowerBodyAngle (+0xc6c), _currentBankingShoulderAngle (+0xc70),
// _currentBankingUpperBodyAngle (+0xc74) -- toward the turn's lean, scaled by USkaterConfig's authored
// BankingMaxGroundAlignmentAngle{Shoulder,UpperBody,LowerBody} (+0xa0/+0xa8/+0xb0) and eased with their
// Smoothing (+0xa4/+0xac/+0xb4) through FInterpTo; AlignBodyToGround{InAir,OnGround} read the three as
// one vector to pose the body. On a grind it switches the lean off: IsOnGrind() zeroes the factor the
// upper- and lower-body targets are multiplied by.
//
// This drives those same three angles on a SINGLE-FOOT grind (the grind def's CatchOrientState is
// FrontFoot or BackFoot: nosegrinds, crooks, noseslides, noseblunts, 5-0s, tailslides, suskis...) from
// the held stick pushed a little to the side -- measured against the direction held when the grind
// began, so up, down or diagonal holds all work -- at the game's own authored limits and smoothing.
// So the rider leans the way the game already leans him, steered on the rail.
//
// THE BOARD LEANS WITH HIM: the deck rolls onto the edge he leans toward -- toes or heels --
// pressing that edge into the rail, in step with the body (it follows the lower-body banking's own
// eased value). Added to _localAnimationRotator.Roll (+0x628) for the read in grind_rock's
// GetLocalAnimatorBoardQuat hook, the game's own value put back straight after. Which side of the deck
// the toes are on: the leading foot gives the way along the deck he is going, and the stance rule
// gives the side of it the toes are on (the board_stance rule).
//
// PAST THE TRUCK-TIGHTNESS WALL. The game's own board roll -- the deck leaning as you steer --
// is USkateboardExMovementComponent::ApplyForceOnBoard's: it writes +0x628 = TruckBankingRatioCurve(
// |banking ratio|) x sign x BankingFlipperMaxAngleCurve(tightness), reads it through
// GetLocalAnimatorBoardQuat and drives the deck there with a PID blended by BankingTightnessPIDCurve
// (tightness) between PIDSettingsBankingTruckMin/Max -- tightness = the mean of FSkateboardSettings'
// TruckTightnessFront/Back (board +0x4a0/+0x4a4, x0.5 at 0xfad4f3). So that roll stops dead at the
// curve's angle for your trucks: the "truck tightness wall" (user: "grind roll is limited by your truck
// tightness which is good, but it kinda adds a hard cutoff"). The lean's roll is now sized off that
// same wall -- % of YOUR trucks' own max roll, evaluated with the game's curve, so looser trucks still
// roll further -- and it builds over the WHOLE sideways push instead of maxing out at half the stick,
// so it carries on past the game's wall.
//
// THE TRUCK GIVES. The board is physics: each truck (and each wheel) is its own body held to the
// deck by a UPhysicsConstraintComponent (ASkateboardEx _truckBackConstraint +0x5d0 /
// _truckFrontConstraint +0x5d8), and ASkateboardEx::SetBoardSettings turns truck tightness into that
// constraint's ANGULAR DRIVE -- SetAngularDriveParams(TruckTightnessStrengthCurve(tightness), 0, 0) --
// a spring holding the hanger square to the deck. So the rolled deck took the rail-held truck with it
// (no pivot, no wheel bite: "the trucks still are just as stiff, they dont turn more") and the spring
// fought the roll (measured: 19.5 deg asked, the deck moved 4-5). While he leans, the GRINDING truck's
// spring now gives -- by GrindLeanTruckGive at a full lean -- so the deck rolls over the hanger the rail
// holds and the wheel comes up to the deck; it goes back to his own tightness exactly (the values read
// off the constraint) as the lean eases, when the grind ends, or when the lean is turned off.
//
// THE LEAN DIGS IN. The board is a physics body: each grind step
// USkateboardExMovementComponent::PhysGrindingMovement (Epic 0xfc8230, reached through the vtable;
// (this, float dt, FBodyInstance* body, ptr)) steers it along the rail and adds the change with
// FBodyInstance::AddImpulse(delta, bVelChange = true); the slowdown is the physics itself (the deck's
// contact friction, slopes). (The grind def's FrictionFactor is read only by UpdateVelocity, which only
// the BASE PhysGrinding/PhysSkateboarding call -- never the Ex grind the board runs -- so scaling it does
// nothing.) Straight after the game's own step, the same body gets a velocity change along its travel: leaning INTO a ledge digs in (up to
// kDigDecel at a full lean), leaning away rides light (gives back up to kLightGive of the grind's own
// slowdown, measured while not leaning). Into = toward the ledge's body, i.e. against the outward normal of the
// ledge's side face (_lastGrindEdgeResult.SideHitResult.ImpactNormal, comp +0x8e0+0xa8+0x30), taken
// against the way the toes point on the deck. A RAIL has no side: either way leans on it (half the dig).
// _grindType +0x8a9: EGrindType GRIND_Ledge / GRIND_Rail.
//
// THE LEAN COMMITS YOU. The game's grind alignment (its option; skater _grindAlignmentRatio +0x714) is read
// only in that same grind step: it takes that share of the board's off-line velocity away, each step -- a
// magnet onto the rail's line. For that one read it is changed and put back straight after: leaning into a
// ledge takes it toward a full lock, leaning away lets it go (up to kLetGo) while the drag pushes you toward
// the open edge (kCommitPush), so holding it rides you off. A rail gets half, either way. GrindLeanCommitPct.
//
// TWO-STICK GRINDS lean too (smith/feeble-type BothFeetFront/BackFoot, boardslide/lipslide/50-50
// BothFeet): push BOTH sticks toward the same side. Each stick's push is measured across its own held
// direction, + toward the toes -- clockwise off the front foot's stick, counter-clockwise off the back
// foot's -- and the two are averaged. The game reads the other gesture: on BothFeet
// GrindsHandler::GetGrindOrientRatios sets the yaw from the front stick's angle off up plus the back
// stick's off down, so turning both sticks the same way yaws the board; a shared push leaves that sum,
// and the lean, alone. The front foot's stick is that function's own pick: left when goofy == switch or
// in input mode 2, else right. No truck gives on BothFeet (a boardslide has none on the ledge).
//
// THE LEAN IS NOT A SCOOP. The game files a scoop from InputHandler::CheckForLeftStickCircular /
// CheckForRightStickCircular: over the buffered input history (_bufferedInput +0x88) it sums the stick's
// angle change while it stays past CircularMinInput -- a reversal restarts the sum -- and files an arc /
// eighth / quarter / half circle by the best sum. It measures how far the stick turned, not how fast,
// so a lean reads as a slow scoop and a pop straight after one comes out as a shove variation. For that
// check only, the held stick's buffered samples from while the lean owned it are FLATTENED to one
// direction (magnitude kept) and put back straight after: to where the stick is now, or -- once it has
// left the lean zone, i.e. a scoop -- to where it was when that scoop began, so the scoop itself is read
// from its true start (see THE SCOOP IS MEASURED FROM ITS OWN START, 528). A stick let go is never
// touched (a reinput). The rest of the game -- grind switching, the pop, its own response -- reads the
// stick as it was.
//
// THE LEAN KEEPS THE CRANK. A trick fires only if its crank type matches the current crank, and
// FlipTricksHandler::CheckForCrank re-picks the crank every frame: a crank definition matches when
// InputHandler::HasInputDirection finds its direction on the latest buffered sample -- on a grind within
// 90 deg (dot >= cos 90). A lean swings the held stick up to 90 deg off, onto that edge, so at the flick
// there was often no crank and the other stick's flick matched nothing ("I did the lean and couldn't
// flick the other stick"). For CheckForCrank only, the latest held-stick sample points at the held
// direction the same way, and is put back straight after.
// =====================================================================================================
#include "tweaks_common.h"
#include "grind_lean.h"
#include "ui/menu_ext.h"
#include "grind_pop.h"       // GrindPop_NameOfFName
#include "catch_tweaks.h"    // CatchTweaks_Skater() -- remote players keep vanilla
#include "scoop_speed.h"     // ScoopSpeed_StickRaw
#include "tweaks_mod.h"
#include <cmath>
#include <cstdio>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB)
enum {
    BMC_SKATER    = 0x340,   // USkateboardExMovementComponent::_skater
    SK_ALIGN      = 0x714,   // ASkaterCharacterBase::_grindAlignmentRatio (the "grind alignment" option)
    BMC_MOVE_DIR  = 0x6b0,   // ::_moveDirection -- the line the alignment pulls onto (GetMoveDirection, its vcall)
    BMC_FLAGS_7E9 = 0x7e9,   // bitfield byte; bit 7 = _isBoardReversedOnGround (GetLocalAnimatorBoardQuat negates pitch)
    SK_MOVE       = 0x550,   // ASkaterCharacterBase::_movementComponent (USkaterMovementComponent)
    SK_TGT_GRIND  = 0x6c8, SK_CUR_GRIND = 0x6d0,
    SK_MESH       = 0x280,   MESH_ANIM = 0x6b0,
    AN_IS_SWITCH  = 0x303,   AN_IS_GOOFY = 0x304,
    GD_NAME       = 0x38,    GD_ORIENT = 0x80,
    SMC_SKATER    = 0xb28,   // USkaterMovementComponent's skater
    SMC_CONFIG    = 0xaf8,   // ::SkaterConfig
    SMC_BANK      = 0xc6c,   // lower body, +4 shoulder, +8 upper body (the order AlignBodyToGround reads)
    CFG_SHOULDER  = 0xa0,    // USkaterConfig: max angle, +4 its smoothing
    CFG_UPPER     = 0xa8,
    CFG_LOWER     = 0xb0,
    SK_BOARD      = 0x568,   BOARD_FLIPPER = 0x4e8,   // the deck
    AN_L_SOCK_LOC = 0x404,   AN_R_SOCK_LOC = 0x41c,   // the feet, mesh space
    COMP_CTW_POS  = 0x1d0,   COMP_CTW_SCALE = 0x1e0,
    BMC_CONFIG    = 0x330,   // USkateboardExMovementComponent::_skateboardConfig (USkateboardConfig*)
    BMC_BOARD     = 0x338,   // ::_skateboard (ASkateboardEx*)
    BRD_TIGHT_F   = 0x4a0,   BRD_TIGHT_B = 0x4a4,     // FSkateboardSettings (board +0x488): TruckTightnessFront/Back
    BRD_FLAGS     = 0x4a8,   // ...its bitfield: bit 2 IsWheelBiteEnabled
    SCFG_MAXROLL  = 0xd0,    SCFG_MAXROLL_NOBITE = 0xd8,   // BankingFlipperMaxAngle(NoBite)Curve
    BRD_TRUCK_BACK = 0x4f0,  BRD_TRUCK_FRONT = 0x500,       // the truck components (positions)
    BRD_CON_BACK   = 0x5d0,  BRD_CON_FRONT   = 0x5d8,       // ASkateboardEx::_truckBack/FrontConstraint
    // UPhysicsConstraintComponent: ConstraintInstance +0x240 -> ProfileInstance +0x8c -> ConeLimit +0x3c
    // (Swing1/2LimitDegrees +0x14/+0x18, motions +0x1c/+0x1d), TwistLimit +0x5c (degrees +0x14, motion
    // +0x18), AngularDrive +0xc4 (TwistDrive: Stiffness, Damping, MaxForce).
    CON_INSTANCE   = 0x240,
    CON_SWING1     = 0x31c,  CON_SWING2 = 0x320, CON_SWING1_M = 0x324, CON_SWING2_M = 0x325,
    CON_TWIST      = 0x33c,  CON_TWIST_M = 0x340,
    CON_STIFF      = 0x390,  CON_DAMP = 0x394, CON_MAXF = 0x398,
    BMC_LAST_GRIND_DEF = 0x528,   // USkateboardMovementComponent::_lastGrindDef
    BMC_GRIND_TYPE     = 0x8a9,   // ::_grindType (EGrindType: GRIND_Ledge, GRIND_Rail)
    BMC_EDGE_VALID     = 0x8e0,   // ::_lastGrindEdgeResult.IsValid
    BMC_SIDE_NORMAL    = 0x9b8,   // ...SideHitResult (+0xa8) .ImpactNormal (+0x30)
    BMC_SIDE_BLOCKING  = 0x9e5,   // ...SideHitResult.bBlockingHit (bit 0)
    GD_FRICTION        = 0x64,    // UGrindOrSlideDefinition::FrictionFactor
};
// ECatchOrientState: 1 FrontFoot, 2 BackFoot -- single-foot; 3 BothFeetFrontFoot, 4 BothFeetBackFoot,
// 5 BothFeet -- two-stick (see TWO-STICK GRINDS).
static bool SingleFoot(int o) { return o == 1 || o == 2; }
static bool TwoStick(int o)   { return o >= 3 && o <= 5; }

// Epic 0x100b490 / Steam 0xfcb2c0: UpdateBodyBanking(this, float dt).
static const char* SIG_BANKING =
    "48 8B C4 48 89 58 08 57 48 81 EC B0 00 00 00 0F 29 70 E8 48 8B D9 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 44 0F 29 50 A8";
// Epic 0xfe9fd0 / Steam 0xfa9e00: AlignBodyToGroundOnGround(this, float, bool) -- counted only.
static const char* SIG_ALIGN_GROUND =
    "48 8B C4 44 88 40 18 F3 0F 11 48 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 F8 FE FF FF 48 81 EC C8 01 00 00 0F 29 70 A8";
// UCurveFloat::GetFloatValue(this, float) -- Epic 0x2ba7d80 / Steam 0x2b6a5c0, sigmake: unique in both.
static const char* SIG_CURVE_EVAL =
    "48 83 C1 30 0F 57 D2 E9 ?? ?? ?? ?? CC CC CC CC 40 56 48 83 EC 50 48 89 5C 24 68 48 8B F2 48 89 6C 24 70";
typedef float (__fastcall* CurveEvalFn)(void* curve, float t);
static CurveEvalFn g_curveEval = nullptr;
// FConstraintInstance::SetAngularDriveParams(this, PositionStrength, VelocityStrength, ForceLimit) --
// Epic 0x2e688a0 / Steam 0x2e2b300, sigmake: unique in both. (UPhysicsConstraintComponent's own is
// `add rcx, 0x240; jmp` to this.)
static const char* SIG_CON_DRIVE =
    "48 83 EC 38 48 8D 44 24 40 F3 0F 11 89 60 01 00 00 48 89 44 24 28 48 8D 54 24 20 48 8D 05 ?? ?? ?? ??";
typedef void (__fastcall* ConDriveFn)(void* instance, float pos, float vel, float maxForce);
static ConDriveFn g_conDrive = nullptr;
// USkateboardExMovementComponent::PhysGrindingMovement(this, float dt, FBodyInstance*, ptr) -- Epic
// 0xfc8230 / Steam 0xf88060; no stack arguments (disassembled). FBodyInstance::AddImpulse(this, const
// FVector&, bool bVelChange) -- 0x2e47bf0 / 0x2e0a650. FBodyInstance::GetUnrealWorldVelocity(this,
// FVector* out) -- 0x2e4f560 / 0x2e11fc0. All sigmade: unique in both.
static const char* SIG_GRIND_MOVE =
    "48 8B C4 F3 0F 11 48 10 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 4C 8B B1 A0 08 00 00";
static const char* SIG_ADD_IMPULSE =
    "4C 8B DC 45 88 43 18 48 83 EC 48 49 8D 43 18 49 89 53 E0 49 89 43 D8 49 8D 53 E8 49 8D 43 D8 48 81 C1 20 01 00 00";
static const char* SIG_BODY_VELOCITY =
    "40 53 48 83 EC 40 F2 0F 10 05 ?? ?? ?? ?? 48 8B DA F2 0F 11 02 8B 05 ?? ?? ?? ?? 89 42 08 48 8D 44 24 20";
typedef void  (__fastcall* GrindMoveFn)(void* self, float dt, void* body, void* p4);
typedef void  (__fastcall* AddImpulseFn)(void* body, const float* impulse, bool velChange);
typedef void* (__fastcall* BodyVelocityFn)(void* body, float* out);
static void* g_moveOrig = nullptr; static uint8_t* g_moveAt = nullptr;
static AddImpulseFn   g_addImpulse = nullptr;
static BodyVelocityFn g_bodyVel = nullptr;
typedef void (__fastcall* BankingFn)(void* self, float dt);
typedef void (__fastcall* AlignFn)(void* self, float a, uint64_t b);
static void* g_bankOrig = nullptr;  static uint8_t* g_bankAt = nullptr;
static void* g_alignOrig = nullptr; static uint8_t* g_alignAt = nullptr;

// ------------------------------------------------------------------ knobs
static int   g_on       = 1;       // GrindLean
static float g_strength = 50.0f;   // GrindLeanStrength -- % of the game's own carving lean
static int   g_flip     = 0;       // GrindLeanFlip (ini only) -- if the lean goes the wrong way
static float g_rollPct  = 220.0f;  // GrindLeanRollPct -- at a full push, the extra roll as % of your trucks' own max roll
static int   g_rollFlip = 0;       // GrindLeanRollFlip (ini only) -- if the deck rolls the wrong way
static float g_truckGive = 100.0f;  // GrindLeanTruckGive -- how much of the grinding truck's spring gives at a full lean (%)
static float g_pressDeg  = 5.0f;    // GrindLeanPressDeg -- 50-50 style: one foot pressing harder tips that end down (deg)
static float g_friction  = 50.0f;  // GrindLeanFriction -- how much the lean digs in / rides light (%)
static float g_commit    = 20.0f;  // GrindLeanCommitPct -- how much the lean locks you on / lets you ride off (%)
static int   g_hideScoop = 1;      // GrindLeanHideScoop -- the game's scoop and crank checks do not see the lean
static int   g_ok       = 1;
static volatile float g_uiLean = 0.0f;
static volatile int   g_uiLive = 0;

static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void GrindLean_ReadConfig(const char* buf) {
    g_on       = TwkIniInt(buf, "GrindLean", 1) ? 1 : 0;
    g_strength = (float)clampI(TwkIniInt(buf, "GrindLeanStrength", 50), 0, 200);
    g_flip     = TwkIniInt(buf, "GrindLeanFlip", 0) ? 1 : 0;
    g_rollPct  = (float)clampI(TwkIniInt(buf, "GrindLeanRollPct", 220), 0, 400);
    g_truckGive = (float)clampI(TwkIniInt(buf, "GrindLeanTruckGive", 100), 0, 100);
    g_pressDeg  = (float)clampI(TwkIniInt(buf, "GrindLeanPressDeg", 5), 0, 15);
    g_friction  = (float)clampI(TwkIniInt(buf, "GrindLeanFriction", 50), 0, 200);
    g_commit    = (float)clampI(TwkIniInt(buf, "GrindLeanCommitPct", 20), 0, 200);
    g_rollFlip = TwkIniInt(buf, "GrindLeanRollFlip", 0) ? 1 : 0;
    g_hideScoop = TwkIniInt(buf, "GrindLeanHideScoop", 1) ? 1 : 0;
}
void GrindLean_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "GrindLean",         g_on);
    TwkIniSetInt(buf, cap, "GrindLeanStrength", (int)g_strength);
    TwkIniSetInt(buf, cap, "GrindLeanFlip",     g_flip);
    TwkIniSetInt(buf, cap, "GrindLeanRollPct",  (int)g_rollPct);
    TwkIniSetInt(buf, cap, "GrindLeanTruckGive", (int)g_truckGive);
    TwkIniSetInt(buf, cap, "GrindLeanPressDeg", (int)g_pressDeg);
    TwkIniSetInt(buf, cap, "GrindLeanFriction",  (int)g_friction);
    TwkIniSetInt(buf, cap, "GrindLeanCommitPct", (int)g_commit);
    TwkIniSetInt(buf, cap, "GrindLeanRollFlip", g_rollFlip);
    TwkIniSetInt(buf, cap, "GrindLeanHideScoop", g_hideScoop);
}
void GrindLean_ResetDefaults() { g_on = 1; g_strength = 50.0f; g_flip = 0; g_rollPct = 220.0f; g_rollFlip = 0; g_truckGive = 100.0f; g_pressDeg = 5.0f; g_friction = 50.0f; g_commit = 20.0f; g_hideScoop = 1; TwkMarkDirty(); }
bool  GrindLean_Enabled() { return g_on != 0; }
void  GrindLean_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
float GrindLean_Strength() { return g_strength; }
void  GrindLean_SetStrength(float p) { g_strength = p < 0.0f ? 0.0f : (p > 200.0f ? 200.0f : floorf(p + 0.5f)); TwkMarkDirty(); }
float GrindLean_RollPct() { return g_rollPct; }
float GrindLean_TruckGive() { return g_truckGive; }
void  GrindLean_SetTruckGive(float p) { g_truckGive = p < 0.0f ? 0.0f : (p > 100.0f ? 100.0f : floorf(p + 0.5f)); TwkMarkDirty(); }
float GrindLean_Friction() { return g_friction; }
void  GrindLean_SetFriction(float p) { g_friction = p < 0.0f ? 0.0f : (p > 200.0f ? 200.0f : floorf(p + 0.5f)); TwkMarkDirty(); }
void  GrindLean_SetRollPct(float p) { g_rollPct = p < 0.0f ? 0.0f : (p > 400.0f ? 400.0f : floorf(p + 0.5f)); TwkMarkDirty(); }

// ------------------------------------------------------------------ the lean
static void*    s_smc = nullptr;       // your USkaterMovementComponent, while we drive its banking
static bool     s_owned = false;
static float    s_bank[3];             // the three angles as we last set them (lower, shoulder, upper)
static uint64_t s_lastMs = 0;
static void*    s_def = nullptr;
static float    s_entryT = 0.0f;
static bool     s_baseOk = false;
static int      s_stick = 0;           // 0 left, 1 right: the stick this grind is held on
static float    s_base[2];             // the held direction: where it was pushed before any lean, as the grind began (533)
static int      s_baseGen = 0;         // bumped at each capture
static bool     s_has[2] = { false, false };   // the sticks held at the capture (a two-stick grind leans off both)
static float    s_bases[2][2];         // ...their held directions
static int      s_front = 0;           // the front foot's stick (0 left, 1 right)
// TRICKS ARE NOT LEANS (527; what the game's scoop check sees is the OTHER stick's call since 533 -- see SHOVES
// NEED THE OTHER STICK). A held stick swinging faster than kLeanSpeedDeg (a shove's scoop runs ~700-1200 deg/s)
// holds the lean still until it is calm again. LETTING
// GO (the game's own idle: each component <= 0.2, UpdateInputRelativeAngle) is the start of a REINPUT: nothing
// of ours touches that stick until it is pressed again and settles, which becomes its new held direction.
// Why (disassembled): with "Crank Relative Input On Grinds" (skater +0x651 bit 4) FlipTricksHandler::
// UpdateInputRelativeAngle -- run INSIDE CheckForCrank -- zeroes the relative angle while both sticks are
// idle and recomputes it (UpdateGrindCrankRelativeAngle: held stick's normalised X x -90) on the idle->active
// edge; otherwise it keeps the one set at OnEnterGrind. The crank snap (463) bent a re-pressed stick back to
// the OLD direction for that very read, so a reinput recomputed to the old direction; and the scoop hiding
// (462) straightened every held sample within 100 deg the whole grind -- any scoop smaller than that (arc 35+,
// eighth 35-70) never reached the game ("shoves often dont shove at all").
static const float kLeanSpeedDeg = 350.0f;  // faster than this (deg/s): a trick gesture
static const float kLeanCalmSec  = 0.15f;   // calm this long and a trick is over
static const float kIdle         = 0.2f;    // the game's own idle stick, per component
static bool     s_rel[2] = { false, false };       // let go since its direction was taken (a reinput pending)
static bool     s_gest[2] = { false, false };      // a trick gesture in progress on this stick
static float    s_calm[2] = { 0.0f, 0.0f };
static float    s_pressT[2] = { 0.0f, 0.0f }, s_stillT[2] = { 0.0f, 0.0f };   // re-press: time since / time still
static float    s_prevAng[2] = { 0.0f, 0.0f }, s_prevV[2][2] = {}, s_spd[2] = { 0.0f, 0.0f }, s_spdAcc[2] = { 0.0f, 0.0f };
static bool     s_angOk[2] = { false, false };
static float    s_heldPush[2] = { 0.0f, 0.0f };    // the lean's push, held through a trick gesture
static bool     s_hideOk[2] = { false, false };    // not let go: the scoop/crank checks may be shown the lean hidden
static int      s_nReinput = 0;                    // this grind, for the window line
static int      s_nShown = 0;                      // scoops shown to the game with a flick (533)
static int      s_nHold = 0;                       // fast swings that held the lean (529)
static float    s_holdSec = 0.0f;                  // ...and how long it was held
static float    s_baseT = -1.0f;                   // input time of the neutral's own sample: the window starts there (533)
static void ResetSticks() {
    for (int k = 0; k < 2; k++) {
        s_rel[k] = s_gest[k] = s_angOk[k] = s_hideOk[k] = false;
        s_calm[k] = s_pressT[k] = s_stillT[k] = s_spd[k] = s_spdAcc[k] = s_heldPush[k] = 0.0f;
    }
}
// Where each stick was pushed before a swing still going on at the landing (533): defined with the input history.
static void EntryNeutral(float dir[2][2], bool pre[2], float* tb);
static int      s_leanOrient = 0;      // this step's grind orient
static int      s_bankCalls = 0, s_alignCalls = 0;   // while we drive it: does the game still run them?
static void*    s_bmc = nullptr;       // the board movement component whose read gets the roll
static float    s_roll = 0.0f;         // the roll it gets (deg)
static float    s_rollLean = 0.0f;     // the roll's own lean, over the WHOLE sideways push (+ = toes), eased
static float    s_wall = 0.0f;         // your trucks' own max roll (the game's wall), deg
static float    s_tight = 0.0f;        // ...at this truck tightness
static const float kRollFullSide = 0.65f;   // the sideways push that gives the full roll

// The grinding truck's constraint while its spring gives, and his own values to put back.
struct TruckGive { void* con; float stiff, damp, maxF, applied; bool on; const char* which; };
static TruckGive s_give = { nullptr, 0.0f, 0.0f, 0.0f, 0.0f, false, "" };    // the grinding truck; per-foot: the BACK foot's
static TruckGive s_give2 = { nullptr, 0.0f, 0.0f, 0.0f, 0.0f, false, "" };   // per-foot: the FRONT foot's
// EACH FOOT WORKS ITS OWN END (516). "If I do a 50-50 and lean right on my back foot, it only leans/rolls the
// back side of the board ... put pressure in different spots of the board with each foot." The deck is one
// rigid static mesh -- it cannot roll at one end only -- so on a two-stick grind with the board ALONG the rail
// (50-50 style; boardslides and lipslides lie across it and are left as they were) each foot's own push works
// the truck under it (that truck's spring gives, the other holds), and the harder-pressed end tips down
// (a 50-50 easing toward a 5-0 or a nosegrind) through the same pitch read the boardslide rock uses.
static bool     s_perFoot = false;     // this grind: per-foot trucks and pressure
static float    s_footLean[2] = { 0.0f, 0.0f };   // each foot's push, 0..1, eased: [0] back, [1] front
static float    s_pitch = 0.0f;        // the pressure tilt the board's read gets (deg, in +0x620's own sense)
static float    s_dig = 0.0f;          // ledge: + leaning into it, - away (-1..1); rail: |lean|
static int      s_digMode = 0;         // 0 none, 1 ledge (into/away), 2 rail (either way)
static uint64_t s_fricMs = 0;
static float    s_natural = 0.0f;      // the grind's own slowdown, measured while not leaning (cm/s^2)
static float    s_prevSpeed = -1.0f;   // the board's speed after our last step (cm/s)
static const float kDigDecel = 250.0f; // cm/s^2 at a full lean into a ledge, x GrindLeanFriction
static const float kLightGive = 0.6f;  // share of the grind's own slowdown given back at a full lean away
static const float kLetGo = 0.95f;     // share of the alignment let go at a full lean away from a ledge
static const float kCommitPush = 110.0f;   // cm/s^2 toward the open edge at a full lean away, x GrindLeanCommitPct
static float    s_toeW[2] = { 0.0f, 0.0f };   // the toes' way across the deck, level (world)
static bool     s_toeOk = false;
static float    s_alignBase = -1.0f;   // the game's alignment, for the one-per-grind commitment line
static bool     s_commitSaid = false;
static int      s_ledge = -1;          // this grind: 1 ledge, 0 rail, -1 unknown
static float    s_into = 0.0f;         // +1: the toes point into the ledge, -1: away, 0: unknown

static void ApplyDriveTo(TruckGive& g, float stiff) {
    g_conDrive((uint8_t*)g.con + CON_INSTANCE, stiff, g.damp, g.maxF);
    g.applied = stiff;
}
static void ApplyDrive(float stiff) { ApplyDriveTo(s_give, stiff); }
static void RestoreOne(TruckGive& g) {
    if (g.on && g.con && g_conDrive) {
        __try { ApplyDriveTo(g, g.stiff); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g.on = false; g.con = nullptr;
}
static void InitGive(TruckGive& g, void* con, const char* which) {
    g.con = con; g.which = which;
    g.stiff = twkF(con, CON_STIFF); g.damp = twkF(con, CON_DAMP); g.maxF = twkF(con, CON_MAXF);
    g.applied = g.stiff; g.on = false;
}
static void GiveBy(TruckGive& g, float lean01) {
    if (!g.con || !(g.stiff > 0.0f)) return;
    const float want = g.stiff * (1.0f - g_truckGive * 0.01f * fminf(1.0f, lean01));
    if (fabsf(want - g.applied) > 0.02f * g.stiff) { ApplyDriveTo(g, want); g.on = true; }
}
static void RestoreTruck() {
    RestoreOne(s_give2);
    if (s_give.on && s_give.con && g_conDrive) {
        __try { ApplyDrive(s_give.stiff); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    s_give.on = false; s_give.con = nullptr;
}

// A component's position along the deck (the flipper's own x).
static float DeckX(void* flp, void* comp) {
    float qf[4], qc[4];
    if (!flp || !comp || !TwkCompQuat(flp, qf) || !TwkCompQuat(comp, qc)) return 0.0f;
    float w[3];
    for (int i = 0; i < 3; i++) w[i] = twkF(comp, COMP_CTW_POS + i * 4) - twkF(flp, COMP_CTW_POS + i * 4);
    float l[3]; TwkQuatInvRotate(qf, w, l);
    return l[0];
}

// The game's own wall for this board, exactly as ApplyForceOnBoard computes it (0 if unavailable).
static float TruckMaxRoll(void* bmc, float* tightOut) {
    void* board = twkP(bmc, BMC_BOARD);
    void* cfg   = twkP(bmc, BMC_CONFIG);
    if (!board || !cfg || !g_curveEval) return 0.0f;
    const float tight = 0.5f * (twkF(board, BRD_TIGHT_F) + twkF(board, BRD_TIGHT_B));
    // Always the wheel-bite curve: with bite off the game's own roll uses the NoBite one, half the range
    // (22.1 vs 11.2 deg at 0.28), and sizing the lean off it halved the lean. The setting itself stays.
    void* curve = twkP(cfg, SCFG_MAXROLL);
    if (!curve) curve = twkP(cfg, SCFG_MAXROLL_NOBITE);
    if (!curve || !(tight > -1e4f && tight < 1e4f)) return 0.0f;
    *tightOut = tight;
    const float v = g_curveEval(curve, tight);
    return (v > 0.0f && v < 90.0f) ? v : 0.0f;
}
static uint64_t s_rollMs = 0;          // when that was last set

// Which side of the deck (its own +Y or -Y) the toes are on: the leading foot says which way along the
// deck he is going, and the toes are to the right of travel for regular riding forward, the left for
// goofy or switch. In the deck's own frame (X along, Y across, Z up; UE's left-handed axes) right of +X
// is +Y. 0 when the feet cannot be read.
static float s_feetX[2] = { 0.0f, 0.0f };   // ToeSideY's reading of the feet along the deck (left, right)
static float ToeSideY(void* skater, void* anim, bool toesRight) {
    void* mesh = twkP(skater, SK_MESH);
    void* bd   = twkP(skater, SK_BOARD);
    void* flp  = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    float qm[4], qf[4];
    if (!mesh || !anim || !flp || !TwkCompQuat(mesh, qm) || !TwkCompQuat(flp, qf)) return 0.0f;
    float sm = twkF(mesh, COMP_CTW_SCALE); if (!(sm > 0.01f && sm < 100.0f)) sm = 1.0f;
    float x[2];
    const int off[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC };
    for (int f = 0; f < 2; f++) {
        float m[3] = { twkF(anim, off[f]) * sm, twkF(anim, off[f] + 4) * sm, twkF(anim, off[f] + 8) * sm };
        float w[3]; TwkQuatRotate(qm, m, w);
        for (int i = 0; i < 3; i++) w[i] += twkF(mesh, COMP_CTW_POS + i * 4) - twkF(flp, COMP_CTW_POS + i * 4);
        float l[3]; TwkQuatInvRotate(qf, w, l);
        x[f] = l[0];
    }
    s_feetX[0] = x[0]; s_feetX[1] = x[1];
    const int front = toesRight ? 0 : 1;               // regular forward: the left foot leads, toes right
    const float along = x[front] - x[1 - front];
    if (fabsf(along) < 5.0f) return 0.0f;
    const float travel = along > 0.0f ? 1.0f : -1.0f;
    return travel * (toesRight ? 1.0f : -1.0f);
}

// UE's FInterpTo.
static float InterpTo(float cur, float target, float dt, float speed) {
    if (speed <= 0.0f) return target;
    const float d = target - cur;
    if (d * d < 1e-8f) return target;
    float a = dt * speed; if (a > 1.0f) a = 1.0f; if (a < 0.0f) a = 0.0f;
    return cur + d * a;
}

static void WriteBank(void* smc) {
    for (int i = 0; i < 3; i++) *(float*)((uint8_t*)smc + SMC_BANK + i * 4) = s_bank[i];
}

static void Lean(void* bmc, float dt) {
    void* skater = twkP(bmc, BMC_SKATER);
    if (!skater) return;
    void* mine = CatchTweaks_Skater();
    if (mine && mine != skater) return;
    void* smc = twkP(skater, SK_MOVE);
    if (!smc) return;
    if (!(dt > 0.0f && dt < 0.25f)) dt = 1.0f / 60.0f;

    void* def = twkP(skater, SK_CUR_GRIND);
    if (!def) def = twkP(skater, SK_TGT_GRIND);
    const uint64_t now = GetTickCount64();
    const bool fresh = now - s_lastMs > 150;
    // A MOMENT WITHOUT A DEF mid-grind carries on with the grind it was (529). On blunts the game empties
    // both defs for ~0.1 s while PhysGrinding keeps running (the lean itself pushing the stick between
    // grinds); letting go there re-measured the held direction from the LEANED stick (-90 on a noseblunt
    // held at 0: logged again and again), so the lean you held became the neutral.
    static uint64_t s_noDefMs = 0;
    if (!def && !fresh && s_owned && s_def) {
        if (!s_noDefMs) s_noDefMs = now;
        if (now - s_noDefMs <= 500) def = s_def;
    } else if (s_noDefMs) {
        static int s_nd = 0;
        if (s_nd < 100 && !fresh) {                     // a new grind after a def-less end is not news
            s_nd++;
            if (def && s_owned) TwkLog("[lean] no grind def for %llu ms mid-grind: carried on through it (held direction kept)", now - s_noDefMs);
            else TwkLog("[lean] no grind def for %llu ms mid-grind: over 0.5 s, the lean let go", now - s_noDefMs);
        }
        s_noDefMs = 0;
    }
    const int orient = def ? *((const uint8_t*)def + GD_ORIENT) : 0;
    const bool active = g_on && def && (SingleFoot(orient) || TwoStick(orient));

    // A NEW GRIND is PhysGrinding starting again after a gap -- not a change of grind on the same rail.
    // A switch mid-grind (a crook turning into a nosegrind: often the lean itself does it, pushing the
    // diagonal toward straight) keeps the direction held at the start, so the lean carries on: measured
    // again after a switch, the stick is already pushed over and the lean would become the new neutral.
    static int s_orient = 0;
    const bool switched = !fresh && def != s_def && s_owned;
    s_lastMs = now;
    if (!active) {                                       // the game takes its banking back and eases it home
        s_owned = false; s_def = def; g_uiLive = 0; g_uiLean = 0.0f;
        s_roll *= expf(-dt / 0.12f); if (fabsf(s_roll) < 0.01f) s_roll = 0.0f;
        s_rollLean *= expf(-dt / 0.12f);
        s_pitch *= expf(-dt / 0.12f); if (fabsf(s_pitch) < 0.01f) s_pitch = 0.0f;
        s_footLean[0] = s_footLean[1] = 0.0f; s_perFoot = false;
        RestoreTruck();
        s_digMode = 0;
        s_bmc = bmc; s_rollMs = now;
        return;
    }
    if (fresh || !s_owned) {                             // ease from wherever the body is now
        for (int i = 0; i < 3; i++) s_bank[i] = twkF(smc, SMC_BANK + i * 4);
        s_entryT = 0.0f; s_baseOk = false;
        if (fresh) {
            static int s_n = 0;
            if (s_n < 200) {
                s_n++;
                char name[96] = "?";
                GrindPop_NameOfFName((const uint8_t*)def + GD_NAME, name, sizeof(name));
                void* cfg = twkP(smc, SMC_CONFIG);
                TwkLog("[lean] %s grind '%s' (orient %d): %s leans the body (the game's own max lean: shoulder %.1f, upper %.1f, lower %.1f)",
                       TwoStick(orient) ? "two-stick" : "single-foot", name, orient,
                       TwoStick(orient) ? "pushing both sticks the same way" : "the held stick",
                       cfg ? twkF(cfg, CFG_SHOULDER) : -1.0f, cfg ? twkF(cfg, CFG_UPPER) : -1.0f, cfg ? twkF(cfg, CFG_LOWER) : -1.0f);
            }
            s_bankCalls = s_alignCalls = 0;
            s_wall = TruckMaxRoll(bmc, &s_tight);
            RestoreTruck();
            s_ledge = -1; s_digMode = 0; s_prevSpeed = -1.0f; s_natural = 0.0f;
            s_commitSaid = false; s_alignBase = -1.0f;
            static int s_nw = 0;
            if (s_nw < 60) {
                s_nw++;
                void* bd = twkP(bmc, BMC_BOARD);
                TwkLog("[lean] your trucks: tightness %.2f -> the game's max roll %.1f deg with wheel bite (the lean's range; wheel bite is %s)%s",
                       s_tight, s_wall, bd ? ((twkB(bd, BRD_FLAGS) & 4) ? "on" : "off") : "?", s_wall > 0.0f ? "" : " (curve unavailable: 6 deg)");
            }
        }
    } else if (switched) {
        static int s_ns = 0;
        if (s_ns < 200) {
            s_ns++;
            char name[96] = "?";
            GrindPop_NameOfFName((const uint8_t*)def + GD_NAME, name, sizeof(name));
            TwkLog("[lean] switched to '%s' mid-grind: the lean carries on (held direction kept)", name);
        }
        if (orient != s_orient) RestoreTruck();          // the other truck grinds now: it is picked again
    }
    s_def = def; s_orient = orient; s_leanOrient = orient; s_smc = smc; s_owned = true;
    s_entryT += dt;

    // Stick right = the rider's right of travel = his TOES for regular riding forward, his heels for
    // goofy or switch (the stance rule board_stance uses).
    void* mesh = twkP(skater, SK_MESH);
    void* anim = mesh ? twkP(mesh, MESH_ANIM) : nullptr;
    const bool toesRight = anim ? ((twkB(anim, AN_IS_GOOFY) != 0) == (twkB(anim, AN_IS_SWITCH) != 0)) : true;

    // The held stick(s), and how far they have gone to the side of the directions held at the start.
    float lx, ly, rx, ry, lean = 0.0f, side = 0.0f, leanFull = 0.0f;
    float footSide[2] = { 0.0f, 0.0f };                 // each foot's own push (two-stick): [0] back, [1] front
    if (ScoopSpeed_StickRaw(false, &lx, &ly) && ScoopSpeed_StickRaw(true, &rx, &ry)) {
        const float lm = sqrtf(lx * lx + ly * ly), rm = sqrtf(rx * rx + ry * ry);
        // THE NEUTRAL IS WHERE THE STICK WAS PUSHED BEFORE THE LEAN, taken the moment the grind begins (533).
        // "if I ollie into noseblunts it usually works okay. But if I do a trick into a noseblunt, like a
        // kickflip, it starts behaving odd when I lean and pop out." Every noseblunt is the same hand: hold right,
        // pull down to lean. After an ollie the stick is still at right when the grind starts; after a kickflip
        // the catch rolls straight on into the pull-down, and the capture 0.12 s in took the stick already at
        // -80..-90 (6 of the last 8 kickflip entries) -- the lean became the neutral. Now at once (the wait was
        // also the "delayed" feel), and back through the input history past any swing still going on.
        if (!s_baseOk) {
            s_stick = rm > lm ? 1 : 0;
            const float m = s_stick ? rm : lm;
            if (m > 0.35f) {
                float dir[2][2] = { { lx / fmaxf(lm, 1e-4f), ly / fmaxf(lm, 1e-4f) }, { rx / fmaxf(rm, 1e-4f), ry / fmaxf(rm, 1e-4f) } };
                bool pre[2] = { false, false }; float tb = -1.0f;
                EntryNeutral(dir, pre, &tb);
                s_base[0] = dir[s_stick][0]; s_base[1] = dir[s_stick][1]; s_baseOk = true; s_baseGen++; ResetSticks();
                s_nShown = s_nReinput = s_nHold = 0; s_holdSec = 0.0f; s_baseT = tb;
                s_has[0] = lm > 0.35f; s_has[1] = rm > 0.35f;
                if (s_has[0]) { s_bases[0][0] = dir[0][0]; s_bases[0][1] = dir[0][1]; }
                if (s_has[1]) { s_bases[1][0] = dir[1][0]; s_bases[1][1] = dir[1][1]; }
                const float x = s_stick ? rx : lx, y = s_stick ? ry : ly;
                void* lih = CatchTweaks_LocalInputHandler();
                s_front = (toesRight || (lih && twkB(lih, 0x20) == 2)) ? 0 : 1;   // InputHandler _inputModeType +0x20
                static int s_nb = 0;
                if (s_nb < 200) {
                    s_nb++;
                    if (TwoStick(orient))
                        TwkLog("[lean] held directions: L %s%.0f deg, R %s%.0f deg%s; front foot on %s (%.2f s in)",
                               s_has[0] ? "" : "(not held) ", atan2f(s_bases[0][1], s_bases[0][0]) * 57.29578f,
                               s_has[1] ? "" : "(not held) ", atan2f(s_bases[1][1], s_bases[1][0]) * 57.29578f,
                               (pre[0] || pre[1]) ? " (from before a swing at the landing)" : "", s_front ? "R" : "L", s_entryT);
                    else if (pre[s_stick])
                        TwkLog("[lean] held direction: %s stick at %.0f deg -- pushed there before the lean began (at %.0f deg as the grind started, %.2f s in)",
                               s_stick ? "R" : "L", atan2f(s_base[1], s_base[0]) * 57.29578f, atan2f(y, x) * 57.29578f, s_entryT);
                    else
                        TwkLog("[lean] held direction: %s stick at %.0f deg (%.2f s in)", s_stick ? "R" : "L", atan2f(y, x) * 57.29578f, s_entryT);
                }
            }
        }
        if (s_baseOk) {
            // Each held stick's push across its own held direction -- both sticks' mean on a two-stick grind (see
            // TWO-STICK GRINDS). Sideways + = turned toward the toes (regular forward): clockwise off the nose on
            // a front-foot grind, counter-clockwise off the tail on a back-foot one (a fixed "right is +" read FS
            // noseslides backwards). Only a LEAN pushes; a trick holds the lean still; a released stick eases it
            // out and, pressed again, becomes the new neutral (see TRICKS ARE NOT LEANS).
            const bool two = TwoStick(orient) && (s_has[0] || s_has[1]);
            const float sv[2][2] = { { lx, ly }, { rx, ry } };
            float sum = 0.0f; int cnt = 0;
            for (int k = 0; k < 2; k++) {
                const bool held = two ? s_has[k] : (k == s_stick);
                if (!held) { s_hideOk[k] = false; continue; }
                float* b = two ? s_bases[k] : s_base;
                const float x = sv[k][0], y = sv[k][1], m = sqrtf(x * x + y * y);
                // Angular speed, per change of the input sample (PhysGrinding steps several times per sample).
                const float ang = atan2f(y, x);
                s_spdAcc[k] += dt;
                if (x != s_prevV[k][0] || y != s_prevV[k][1]) {
                    if (s_angOk[k] && m > kIdle) {
                        float da = (ang - s_prevAng[k]) * 57.29578f;
                        if (da > 180.0f) da -= 360.0f; else if (da < -180.0f) da += 360.0f;
                        s_spd[k] = fabsf(da) / fmaxf(s_spdAcc[k], 0.004f);
                    } else s_spd[k] = 0.0f;
                    s_spdAcc[k] = 0.0f; s_prevV[k][0] = x; s_prevV[k][1] = y;
                } else if (s_spdAcc[k] > 0.05f) s_spd[k] = 0.0f;                  // no new sample: still
                s_prevAng[k] = ang; s_angOk[k] = m > kIdle;
                float push = 0.0f;
                if (fabsf(x) <= kIdle && fabsf(y) <= kIdle) {                    // let go
                    s_rel[k] = true; s_gest[k] = false; s_pressT[k] = s_stillT[k] = 0.0f;
                } else if (s_rel[k]) {                                           // pressed again: a reinput
                    s_pressT[k] += dt;
                    if (m > 0.35f && s_spd[k] < 150.0f) s_stillT[k] += dt; else s_stillT[k] = 0.0f;
                    if (m > 0.35f && ((s_stillT[k] >= 0.06f && s_pressT[k] >= 0.06f) || s_pressT[k] >= 0.3f)) {
                        b[0] = x / m; b[1] = y / m;
                        s_rel[k] = false; s_gest[k] = false; s_heldPush[k] = 0.0f; s_baseGen++; s_nReinput++;
                        s_baseT = -1.0f;                         // the new window starts now
                        static int s_nr = 0;
                        if (s_nr < 200) { s_nr++; TwkLog("[lean] reinput on %s: new held direction %.0f deg (%.2f s after pressing) -- the lean is measured from there", k ? "R" : "L", ang * 57.29578f, s_pressT[k]); }
                    }
                } else {
                    // Only a FAST swing holds the lean (a scoop in flight), and it lets go once the stick is still,
                    // wherever it rests (529). Past the zone no longer holds it on its own: a blunt's lean is up/down
                    // off a sideways hold, 90-100 deg (measured), and 528 froze any stick past 60 deg for the rest of
                    // the grind. What the game's scoop check sees is decided by the other stick (533).
                    if (s_spd[k] > kLeanSpeedDeg) {
                        if (!s_gest[k]) s_nHold++;
                        s_gest[k] = true; s_calm[k] = 0.0f;
                    } else if (s_gest[k] && (s_calm[k] += dt) > kLeanCalmSec) s_gest[k] = false;
                    if (s_gest[k]) s_holdSec += dt;
                    float px = b[1], py = -b[0];
                    if (two ? (k != s_front) : (orient == 2)) { px = -px; py = -py; }
                    const float p = x * px + y * py;
                    if (s_gest[k]) push = s_heldPush[k];
                    else { push = p; s_heldPush[k] = p; }
                }
                if (s_rel[k]) { push = 0.0f; s_heldPush[k] = 0.0f; }
                s_hideOk[k] = !s_rel[k];                  // only a release hands the stick back (528)
                sum += push; cnt++;
                if (two) footSide[k == s_front ? 1 : 0] = push;
            }
            side = cnt ? sum / (float)cnt : 0.0f;
        }
        if (s_baseOk) {
            const float dz = 0.05f;
            float t = (fabsf(side) - dz) / 0.45f;        // full lean at ~0.5 of the stick to the side
            if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            lean = powf(t, 1.3f) * (side < 0.0f ? -1.0f : 1.0f);
            // The roll builds over the WHOLE push (the body is fully over at half of it).
            // Full at a 0.65 sideways push: holding a direction, the rim only gives 0.5-0.77 to the side
            // (measured), so a 0..1 range would leave every real push in the bottom half of the curve.
            float tf = (fabsf(side) - dz) / (kRollFullSide - dz);
            if (tf < 0.0f) tf = 0.0f; if (tf > 1.0f) tf = 1.0f;
            leanFull = powf(tf, 1.1f) * (side < 0.0f ? -1.0f : 1.0f);
        }
    }
    const float toward = lean * (toesRight ? 1.0f : -1.0f) * (g_flip ? -1.0f : 1.0f);
    // The BODY banks in the frame of his travel, not his toes: it is the game's carving lean, and a
    // carving lean goes right of where he is going in any stance. So stick right banks him right in
    // every stance -- regular forward is unchanged (toes ARE right of travel there). Steered through the
    // stance's toe flip like the board is, the body leans backwards after a 180 into the grind: the board
    // follows the feet, the body the travel.
    const float bodyLean = lean * (g_flip ? -1.0f : 1.0f);

    void* cfg = twkP(smc, SMC_CONFIG);
    const int cfgOff[3] = { CFG_LOWER, CFG_SHOULDER, CFG_UPPER };  // in s_bank order
    const float k = g_strength * 0.01f;
    for (int i = 0; i < 3; i++) {
        const float maxA = cfg ? twkF(cfg, cfgOff[i]) : 10.0f, sm = cfg ? twkF(cfg, cfgOff[i] + 4) : 5.0f;
        const float target = bodyLean * (maxA > -90.0f && maxA < 90.0f ? maxA : 10.0f) * k;
        s_bank[i] = InterpTo(s_bank[i], target, dt, (sm > 0.0f && sm < 100.0f) ? sm : 5.0f);
    }
    WriteBank(smc);
    g_uiLean = toward; g_uiLive = 1;

    // The deck rolls onto the edge he leans toward: over the whole push, on the lower body's smoothing
    // so it moves with him, sized off YOUR trucks' own max roll (see PAST THE TRUCK-TIGHTNESS WALL).
    // + roll dips the deck's +Y edge (confirmed in game).
    const float towardFull = leanFull * (toesRight ? 1.0f : -1.0f) * (g_flip ? -1.0f : 1.0f);
    const float lowSm = cfg ? twkF(cfg, CFG_LOWER + 4) : 5.0f;
    s_rollLean = InterpTo(s_rollLean, towardFull, dt, (lowSm > 0.0f && lowSm < 100.0f) ? lowSm : 5.0f);
    const float wall = s_wall > 0.0f ? s_wall : 6.0f;
    const float toeSide = ToeSideY(skater, anim, toesRight);
    s_roll = s_rollLean * wall * (g_rollPct * 0.01f) * toeSide * (g_rollFlip ? -1.0f : 1.0f);
    s_bmc = bmc; s_rollMs = now;

    // PER FOOT (516): a two-stick grind with the deck along the rail -- its long axis within ~45 deg of the
    // line the alignment pulls onto. Decided once per grind (and again after a switch), from the frame.
    {
        static int s_pfGen = -1, s_pfOrient = -1;
        if (s_pfGen != s_baseGen || s_pfOrient != orient) s_pfGen = -2;   // a new grind or a switch: re-decide
        if (s_pfGen == -2 && s_baseOk && toeSide != 0.0f) {
            s_pfGen = s_baseGen; s_pfOrient = orient;
            bool along = false;
            void* board = twkP(skater, SK_BOARD);
            void* flp   = board ? twkP(board, BOARD_FLIPPER) : nullptr;
            float qf[4];
            const float lxd = twkF(bmc, BMC_MOVE_DIR), lyd = twkF(bmc, BMC_MOVE_DIR + 4);
            if (flp && TwkCompQuat(flp, qf)) {
                const float x1[3] = { 1.0f, 0.0f, 0.0f };
                float xw[3]; TwkQuatRotate(qf, x1, xw);
                const float nx = sqrtf(xw[0] * xw[0] + xw[1] * xw[1]), nl = sqrtf(lxd * lxd + lyd * lyd);
                if (nx > 0.1f && nl > 0.1f) along = fabsf(xw[0] * lxd + xw[1] * lyd) / (nx * nl) > 0.7f;
            }
            const bool pf = TwoStick(orient) && along;
            if (pf != s_perFoot) RestoreTruck();
            s_perFoot = pf;
            static int s_npf = 0;
            if (pf && s_npf < 60) { s_npf++; TwkLog("[lean] along the rail on both sticks: each foot works its own truck and presses its own end"); }
        }
    }
    {
        const float lowSm2 = (lowSm > 0.0f && lowSm < 100.0f) ? lowSm : 5.0f;
        for (int r = 0; r < 2; r++) {
            float tf = 0.0f;
            if (s_perFoot) { tf = (fabsf(footSide[r]) - 0.05f) / (kRollFullSide - 0.05f); tf = tf < 0.0f ? 0.0f : (tf > 1.0f ? 1.0f : powf(tf, 1.1f)); }
            s_footLean[r] = InterpTo(s_footLean[r], tf, dt, lowSm2);
        }
        // The harder-pressed end tips down: + pitch raises the deck's own +X end (grind_rock, measured), and
        // the board's read negates the pitch while it is reversed on the ground (a 180 shove).
        float pitch = 0.0f;
        if (s_perFoot && g_pressDeg > 0.0f) {
            const int lead = toesRight ? 0 : 1;
            const float backX = s_feetX[1 - lead];
            const float press = g_pressDeg * (s_footLean[0] - s_footLean[1]);    // + = the back foot presses harder
            const bool reversed = (twkB(bmc, BMC_FLAGS_7E9) & 0x80) != 0;
            pitch = -press * (backX >= 0.0f ? 1.0f : -1.0f) * (reversed ? -1.0f : 1.0f);
        }
        s_pitch = pitch;
    }
    if (s_perFoot) {
        if (!s_give.con && !s_give2.con && g_conDrive) {
            void* board = twkP(skater, SK_BOARD);
            void* flp   = board ? twkP(board, BOARD_FLIPPER) : nullptr;
            const int lead = toesRight ? 0 : 1;
            const float xB = DeckX(flp, board ? twkP(board, BRD_TRUCK_BACK) : nullptr);
            const float xF = DeckX(flp, board ? twkP(board, BRD_TRUCK_FRONT) : nullptr);
            const bool frontIsF = fabsf(s_feetX[lead] - xF) < fabsf(s_feetX[lead] - xB);   // the front foot over the "front" truck?
            void* conBack  = board ? twkP(board, frontIsF ? BRD_CON_BACK : BRD_CON_FRONT) : nullptr;
            void* conFront = board ? twkP(board, frontIsF ? BRD_CON_FRONT : BRD_CON_BACK) : nullptr;
            if (conBack)  InitGive(s_give, conBack, "back foot's");
            if (conFront) InitGive(s_give2, conFront, "front foot's");
        }
        GiveBy(s_give, s_footLean[0]);
        GiveBy(s_give2, s_footLean[1]);
    }
    // The grinding truck's spring gives with the lean. Picked once per grind: the truck under the leading
    // foot on a front-foot grind (nosegrind, crook), under the trailing foot on a back-foot one (5-0,
    // tailslide) -- by position, so a board the wrong way round picks right too. None on BothFeet.
    if (!s_perFoot && !s_give.con && g_conDrive && toeSide != 0.0f && orient != 5) {
        void* board = twkP(skater, SK_BOARD);
        void* flp   = board ? twkP(board, BOARD_FLIPPER) : nullptr;
        const int lead = toesRight ? 0 : 1;
        const float footX = s_feetX[(orient == 1 || orient == 3) ? lead : 1 - lead];
        const float xB = DeckX(flp, board ? twkP(board, BRD_TRUCK_BACK) : nullptr);
        const float xF = DeckX(flp, board ? twkP(board, BRD_TRUCK_FRONT) : nullptr);
        const bool front = fabsf(footX - xF) < fabsf(footX - xB);
        void* con = board ? twkP(board, front ? BRD_CON_FRONT : BRD_CON_BACK) : nullptr;
        if (con) {
            s_give.con = con; s_give.which = front ? "front" : "back";
            s_give.stiff = twkF(con, CON_STIFF); s_give.damp = twkF(con, CON_DAMP); s_give.maxF = twkF(con, CON_MAXF);
            s_give.applied = s_give.stiff; s_give.on = false;
            static int s_nt = 0;
            if (s_nt < 60) {
                s_nt++;
                TwkLog("[lean] grinding truck: %s (deck x %.1f, foot %.1f) | its spring %.0f, damping %.0f, max force %.0f | "
                       "angle limits: swing1 %.0f (mode %d), swing2 %.0f (mode %d), twist %.0f (mode %d)",
                       s_give.which, front ? xF : xB, footX, s_give.stiff, s_give.damp, s_give.maxF,
                       twkF(con, CON_SWING1), twkB(con, CON_SWING1_M), twkF(con, CON_SWING2), twkB(con, CON_SWING2_M),
                       twkF(con, CON_TWIST), twkB(con, CON_TWIST_M));
            }
        }
    }
    if (!s_perFoot && s_give.con && s_give.stiff > 0.0f) {
        const float want = s_give.stiff * (1.0f - g_truckGive * 0.01f * fminf(1.0f, fabsf(s_rollLean)));
        if (fabsf(want - s_give.applied) > 0.02f * s_give.stiff) {
            ApplyDrive(want);
            s_give.on = true;
        }
    }

    // Digging in (see THE LEAN DIGS IN). Which way the ledge is, from its side face, once per grind.
    if (s_ledge < 0 && toeSide != 0.0f) {
        const int type = twkB(bmc, BMC_GRIND_TYPE);
        s_ledge = (type == 0) ? 1 : 0;
        s_into = 0.0f;
        void* board = twkP(skater, SK_BOARD);
        void* flp   = board ? twkP(board, BOARD_FLIPPER) : nullptr;
        float qf[4];
        if (s_ledge && twkB(bmc, BMC_EDGE_VALID) && (twkB(bmc, BMC_SIDE_BLOCKING) & 1) && flp && TwkCompQuat(flp, qf)) {
            float in[3] = { -twkF(bmc, BMC_SIDE_NORMAL), -twkF(bmc, BMC_SIDE_NORMAL + 4), 0.0f };
            const float n = sqrtf(in[0] * in[0] + in[1] * in[1]);
            const float y1[3] = { 0.0f, 1.0f, 0.0f };
            float yw[3]; TwkQuatRotate(qf, y1, yw);
            if (n > 0.3f) {
                const float d = (yw[0] * in[0] + yw[1] * in[1]) / n * toeSide;   // toes . into-the-ledge
                s_into = d > 0.3f ? 1.0f : (d < -0.3f ? -1.0f : 0.0f);
            }
        }
        static int s_nl = 0;
        if (s_nl < 100) {
            s_nl++;
            TwkLog("[lean] grind is a %s (type %d): %s", s_ledge ? "LEDGE" : "RAIL", type,
                   s_ledge ? (s_into > 0.0f ? "the toes point into it -- leaning toes digs in, heels rides light"
                           : s_into < 0.0f ? "the heels are on its side -- leaning heels digs in, toes rides light"
                                           : "its side could not be read -- either way leans on it")
                           : "either way leans on it");
        }
    }
    // The toes' way across, level: the commitment pushes along it, with the lean's sign.
    s_toeOk = false;
    if (toeSide != 0.0f) {
        void* board = twkP(skater, SK_BOARD);
        void* flp   = board ? twkP(board, BOARD_FLIPPER) : nullptr;
        float qf[4];
        if (flp && TwkCompQuat(flp, qf)) {
            const float y1[3] = { 0.0f, 1.0f, 0.0f };
            float yw[3]; TwkQuatRotate(qf, y1, yw);
            const float n = sqrtf(yw[0] * yw[0] + yw[1] * yw[1]);
            if (n > 0.3f) { s_toeW[0] = yw[0] / n * toeSide; s_toeW[1] = yw[1] / n * toeSide; s_toeOk = true; }
        }
    }
    // What the grind step does with it (hkGrindMove): into/away on a ledge, either way on a rail.
    if (s_ledge == 1 && s_into != 0.0f) { s_digMode = 1; s_dig = fmaxf(-1.0f, fminf(1.0f, s_rollLean * s_into)); }
    else                                { s_digMode = 2; s_dig = fminf(1.0f, fabsf(s_rollLean)); }
    s_fricMs = now;
}

void GrindLean_OnPhysGrinding(void* bmc, float dt) {
    if (!g_ok || !bmc) return;
    __try { Lean(bmc, dt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_ok = 0; s_owned = false; RestoreTruck(); TwkLog("[lean] caught fatal -- grind lean off for this run"); }
}

// The trucks' own max lean for this board (deg; 0 if unavailable) -- for the landing lean (board_stance).
float GrindLean_TruckWall(void* bmc) {
    float tight = 0.0f;
    __try { return bmc ? TruckMaxRoll(bmc, &tight) : 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

float GrindLean_BoardPitch(void* bmc) {
    if (!g_ok || bmc != s_bmc || s_pitch == 0.0f) return 0.0f;
    const uint64_t gap = GetTickCount64() - s_rollMs;
    if (gap < 80) return s_pitch;
    if (gap > 1000) return 0.0f;
    return s_pitch * expf(-(float)(gap - 80) / 150.0f);
}

float GrindLean_BoardRoll(void* bmc) {
    if (!g_ok || bmc != s_bmc || s_roll == 0.0f) return 0.0f;
    const uint64_t gap = GetTickCount64() - s_rollMs;
    if (gap < 80) return s_roll;
    if (gap > 1000) return 0.0f;
    return s_roll * expf(-(float)(gap - 80) / 150.0f);
}

// ------------------------------------------------------------------ the scoop check (see THE LEAN IS NOT A SCOOP)
// InputHandler::CheckForLeftStickCircular / CheckForRightStickCircular(this, FVector2D l, FVector2D r) --
// Epic 0x10482d0 / 0x10490b0, Steam 0x1008650 / 0x1009430, sigmade: unique in both. Called from
// InputHandler::Tick after the tick's sample is buffered; they read the buffer, not l/r.
static const char* SIG_CIRC[2] = {
    "48 8B C4 53 48 81 EC 30 01 00 00 48 89 54 24 40 48 8B D9 48 8B 91 88 00 00 00 4C 89 44 24 38 83 7A 08 02 0F 8C ?? ?? ?? ?? 80 79 20 03 0F 57 C9",
    "48 8B C4 53 48 81 EC 30 01 00 00 48 89 54 24 38 48 8B D9 48 8B 91 88 00 00 00 4C 89 44 24 30 83 7A 08 02 0F 8C ?? ?? ?? ?? 80 79 20 03 48 89 68 10",
};
typedef void (__fastcall* CircularFn)(void* ih, uint64_t l, uint64_t r);
static void*    g_circOrig[2] = { nullptr, nullptr };
static uint8_t* g_circAt[2]   = { nullptr, nullptr };
enum {
    IH_HISTORY_TIME = 0x00, IH_INPUTS_DB = 0x10, IH_MODE = 0x20, IH_TIME = 0x3c, IH_SMALL_SCOOP = 0x77,
    IH_BUFFERED = 0x88,
    FID_STRIDE = 0x24, FID_LSTICK = 0x08, FID_RSTICK = 0x10, FID_TIME = 0x1c,   // FInputData
    // FInputModeInputSettings (UInputsDatabase +0x30 default, +0x6c small-scoop mode, +0xa8 mode 3): min/max pairs
    IS_CIRC_MIN = 0x18, IS_ARC = 0x1c, IS_EIGHTH = 0x24, IS_QUARTER = 0x2c, IS_HALF = 0x34,
};
static int   s_winGen = -1;            // the held-direction capture the window belongs to
static bool  s_winLive = false;
static float s_winT0 = 0.0f, s_winT1 = -1.0f;   // handler time the lean owned the stick (= sample InputTime)
static bool  s_winHas[2] = { false, false };   // the sticks hidden: the held one, or both on a two-stick grind
static float s_winBases[2][2];
static bool  s_winTwo = false;                 // two-stick: a stick let go (< 0.2) is the game's own from then
static float s_winLetT[2] = { 1e30f, 1e30f };  // ...the input time each was let go
static int   s_scRaw = 0, s_scHid = 0; // ticks the check would file a scoop: from the stick as it was / as it sees it
static float s_scRawMax = 0.0f, s_scHidMax = 0.0f;
static int   s_ckReads = 0, s_ckNone = 0; // crank reads at the held direction / of them with no crank found
static int   s_ckHit[3] = { 0, 0, 0 };    // cranks found shown held / mirrored / as is (532)
static float s_flickT[2] = { -1.0f, -1.0f };   // input time the OTHER stick last flicked, per held stick (533)
static float s_shownFlickT[2] = { -1.0f, -1.0f }; // the flick a shown scoop was last counted for
static float s_ckMaxDeg = 0.0f;           // the furthest lean they covered
struct SnapSave { float* v; float x, y; };
static SnapSave s_save[1024];
static int      s_nsave = 0;

static const float* ScoopSettings(void* ih) {
    uint8_t* db = (uint8_t*)twkP(ih, IH_INPUTS_DB);
    if (!db) return nullptr;
    return (const float*)(db + (twkB(ih, IH_MODE) == 3 ? 0xa8 : (twkB(ih, IH_SMALL_SCOOP) ? 0x6c : 0x30)));
}

// The recogniser's own sum (disassembled): each pair of samples both past CircularMinInput adds its angle
// change; a reversal restarts the sum and its best; dropping inside the radius restarts the sum. The best
// |sum| picks the input, filed only while the latest sum is non-zero.
static float CircularBest(const uint8_t* data, int n, int off, float minR2, bool* live) {
    float acc = 0.0f, best = 0.0f, last = 0.0f;
    for (int i = 1; i < n; i++) {
        const float* a = (const float*)(data + (i - 1) * FID_STRIDE + off);
        const float* c = (const float*)(data + i * FID_STRIDE + off);
        if (c[0] * c[0] + c[1] * c[1] < minR2 || a[0] * a[0] + a[1] * a[1] < minR2) { acc = 0.0f; continue; }
        float d = atan2f(c[1], c[0]) - atan2f(a[1], a[0]);
        if (d > 3.14159265f) d -= 6.2831853f; else if (d < -3.14159265f) d += 6.2831853f;
        if ((d > 0.0f && last < 0.0f) || (d < 0.0f && last > 0.0f)) { acc = 0.0f; best = 0.0f; }
        acc += d;
        if (fabsf(acc) > fabsf(best)) best = acc;
        last = d;
    }
    *live = acc != 0.0f;
    return fabsf(best) * 57.29578f;
}
static bool Files(const float* set, float deg, bool live) {
    if (!live) return false;
    const int o[4] = { IS_ARC / 4, IS_EIGHTH / 4, IS_QUARTER / 4, IS_HALF / 4 };
    for (int k = 0; k < 4; k++) if (deg >= set[o[k]] && deg <= set[o[k] + 1]) return true;
    return false;
}

static void LogWindow() {
    static int s_n = 0;
    if (s_n >= 200) return;
    s_n++;
    TwkLog("[lean] the game's scoop check during that lean: read a scoop on %d ticks (up to %.0f deg) as the stick was -> %d (up to %.0f deg) with the lean hidden"
           " | crank read at the held direction %d times (lean up to %.0f deg), no crank on %d | scoops shown with a flick %d, reinputs %d"
           " | lean held still by %d fast swings for %.2f s | cranks found: held %d, mirrored %d, as is %d",
           s_scRaw, s_scRawMax, s_scHid, s_scHidMax, s_ckReads, s_ckMaxDeg, s_ckNone, s_nShown, s_nReinput, s_nHold, s_holdSec,
           s_ckHit[0], s_ckHit[1], s_ckHit[2]);
}

static void FlushFlick(int stick);
// The window of input time the lean owned the stick. Game thread (inside InputHandler::Tick).
static void UpdateWindow(void* ih) {
    const bool live = g_on && g_hideScoop && s_owned && s_baseOk && GetTickCount64() - s_lastMs <= 80;
    const float now = *(const float*)((const uint8_t*)ih + IH_TIME);
    static void* s_winIh = nullptr;
    if (ih != s_winIh || now < s_winT1) { s_winIh = ih; s_winT0 = 0.0f; s_winT1 = -1.0f; s_winLive = false; }  // new handler / its clock reset
    if (live) {
        if (!s_winLive || s_winGen != s_baseGen) {
            if (s_winLive) LogWindow();
            // From the neutral's own sample (533): a lean begun before the grind (a kickflip's catch rolling into
            // the pull-down) is hidden too, continuous with the landing before it.
            s_winGen = s_baseGen; s_winT0 = (s_baseT > 0.0f && s_baseT <= now && now - s_baseT < 1.0f) ? s_baseT : now;
            s_flickT[0] = s_flickT[1] = s_shownFlickT[0] = s_shownFlickT[1] = -1.0f;
            s_winLetT[0] = s_winLetT[1] = 1e30f;
            s_scRaw = s_scHid = 0; s_scRawMax = s_scHidMax = 0.0f;
            s_ckReads = s_ckNone = 0; s_ckMaxDeg = 0.0f; s_ckHit[0] = s_ckHit[1] = s_ckHit[2] = 0;
        }
        s_winT1 = now; s_winLive = true;
        s_winTwo = TwoStick(s_leanOrient);             // a mid-grind switch can change it
        for (int k = 0; k < 2; k++) {
            s_winHas[k] = (s_winTwo ? s_has[k] : (k == s_stick)) && s_hideOk[k];   // let go: the game's own (527)
            const float* b = s_winTwo ? s_bases[k] : s_base;
            s_winBases[k][0] = b[0]; s_winBases[k][1] = b[1];
        }
    } else if (s_winLive) {
        s_winLive = false;
        FlushFlick(0); FlushFlick(1);
        LogWindow();
    }
}

// ---- the input history (533) --------------------------------------------------------------------------------
static const float kMoveDegPerSec = 150.0f;  // a stick turning faster than this is swinging
static const float kMoveSpan      = 0.03f;   // ...measured over this much input time (samples repeat)
static const float kEntryRecent   = 0.10f;   // a swing still going on (or just ended) at the landing...
static const float kEntryBack     = 0.45f;   // ...is walked back at most this far
static const float kFlickDist     = 0.15f;   // the other stick moving this far... (534: 0.30 was late for the pop)
static const float kFlickSpan     = 0.06f;   // ...within this is a flick
static const float kFlickShowSec  = 0.15f;   // a flick shows the held stick's swing for this long
static const float kScoopLead     = 0.25f;   // the swing must still be moving this close to the flick (534: 0.15)
static const float kScoopBack     = 0.60f;   // and is shown from at most this far back
static const float kScoopPause    = 0.05f;   // a hitch shorter than this does not split a swing (534)
static const float kScoopFastDeg  = 400.0f;  // a swing never this fast is a lean, flick or not (534)
static inline float HT(const uint8_t* data, int i) { return *(const float*)(data + i * FID_STRIDE + FID_TIME); }
static inline const float* HV(const uint8_t* data, int i, int off) { return (const float*)(data + i * FID_STRIDE + off); }
// The stick's turn rate over the kMoveSpan before sample i (deg/s); -1 when it is not pushed at either end.
static float SpanSpeed(const uint8_t* data, int i, int off) {
    const float* v = HV(data, i, off);
    if (v[0] * v[0] + v[1] * v[1] < 0.1225f) return -1.0f;
    const float t = HT(data, i);
    for (int j = i - 1; j >= 0; j--) {
        const float tp = HT(data, j);
        if (t - tp < kMoveSpan) continue;
        const float* w = HV(data, j, off);
        if (w[0] * w[0] + w[1] * w[1] < 0.1225f) return -1.0f;
        float d = (atan2f(v[1], v[0]) - atan2f(w[1], w[0])) * 57.29578f;
        if (d > 180.0f) d -= 360.0f; else if (d < -180.0f) d += 360.0f;
        return fabsf(d) / (t - tp);
    }
    return -1.0f;
}
// Where the stick was pushed before a swing still going on (or just ended) at the newest sample: that swing is
// walked back to where it was still, or to where the push itself began. False when it is not pushed now.
static bool PreMotionDir(const uint8_t* data, int n, int off, float out[2], float* outT, bool* moved) {
    const float* v = HV(data, n - 1, off);
    if (v[0] * v[0] + v[1] * v[1] < 0.1225f) return false;
    const float tNow = HT(data, n - 1);
    int j = -1;
    for (int i = n - 1; i >= 1 && tNow - HT(data, i) <= kEntryRecent; i--)
        if (SpanSpeed(data, i, off) > kMoveDegPerSec) { j = i; break; }
    int k = n - 1;
    if (j >= 0) {
        k = j;
        while (k > 0 && tNow - HT(data, k) < kEntryBack && SpanSpeed(data, k, off) > kMoveDegPerSec) k--;
        while (k < n - 1) { const float* w = HV(data, k, off); if (w[0] * w[0] + w[1] * w[1] >= 0.1225f) break; k++; }
    }
    const float* b = HV(data, k, off);
    const float bm = sqrtf(b[0] * b[0] + b[1] * b[1]);
    out[0] = b[0] / bm; out[1] = b[1] / bm; *outT = HT(data, k); *moved = j >= 0;
    return true;
}
// The other stick flicking now: it moved kFlickDist within the last kFlickSpan.
static bool OtherFlicking(const uint8_t* data, int n, int off) {
    const float* v = HV(data, n - 1, off);
    const float t = HT(data, n - 1);
    for (int j = n - 2; j >= 0 && t - HT(data, j) <= kFlickSpan; j--) {
        const float* w = HV(data, j, off);
        const float dx = v[0] - w[0], dy = v[1] - w[1];
        if (dx * dx + dy * dy > kFlickDist * kFlickDist) return true;
    }
    return false;
}
// The start of the swing the held stick was making around a flick at flickT (the input time just after the
// sample it left from), or -1 if it was not swinging within kScoopLead of the flick. 534: "check the last 4
// nose grinds ... Why did the first 3 not detect a shove but the last one did?" -- same 88-108 deg scoops,
// flick seen in all four, but the game got 16 deg / nothing / nothing / 110 deg: the swing had to be moving
// within 0.15 s of a flick detected at 0.3 of travel, and a hitch cut it at the last pause. Now a hitch under
// kScoopPause joins the swing, it may end kScoopLead before the flick, and it must reach kScoopFastDeg.
static float s_scoopPeak = 0.0f;                 // the last swing's peak turn rate, for the flick line
static float ScoopStart(const uint8_t* data, int n, int off, float flickT) {
    s_scoopPeak = 0.0f;
    int j = -1;
    for (int i = n - 1; i >= 1 && HT(data, i) >= flickT - kScoopLead; i--)
        if (SpanSpeed(data, i, off) > kMoveDegPerSec) { j = i; break; }
    if (j < 0) return -1.0f;
    const float tNow = HT(data, n - 1);
    int k = j; float lastMove = HT(data, j), peak = 0.0f;
    for (int i = j; i > 0 && tNow - HT(data, i) < kScoopBack; i--) {
        const float sp = SpanSpeed(data, i, off);
        if (sp > kMoveDegPerSec) { k = i; lastMove = HT(data, i); if (sp > peak) peak = sp; }
        else if (sp < 0.0f || lastMove - HT(data, i) > kScoopPause) break;
    }
    s_scoopPeak = peak;
    if (peak < kScoopFastDeg) return -1.0f;
    int a = k;                                   // the sample it left from: kMoveSpan before its first moving one
    while (a > 0 && HT(data, k) - HT(data, a) < kMoveSpan) a--;
    return HT(data, a) + 0.0005f;
}
// ONE LINE PER FLICK (534): the held stick's trace before it, and what the game's check made of the shown swing.
static bool  s_flOpen[2] = { false, false };
static float s_flArc[2] = { 0.0f, 0.0f };
static int   s_flFiled[2] = { 0, 0 }, s_flTicks[2] = { 0, 0 };
static void FlushFlick(int stick) {
    if (!s_flOpen[stick]) return;
    s_flOpen[stick] = false;
    static int s_nf = 0;
    if (s_nf < 300) {
        s_nf++;
        TwkLog("[lean]   ... shown for %d ticks: the game's check read up to %.0f deg, a scoop on %d ticks -> %s",
               s_flTicks[stick], s_flArc[stick], s_flFiled[stick], s_flFiled[stick] ? "SHOVE" : "no shove");
    }
}
static void TraceFlick(const uint8_t* data, int n, int off, int offOther, int stick, float bound, float nowT) {
    static int s_nt = 0;
    if (s_nt >= 300) return;
    s_nt++;
    char buf[640]; int w = 0;
    float nextT = nowT - 0.35f;
    for (int i = 0; i < n && w < 560; i++) {
        const float t = HT(data, i);
        if (t < nextT) continue;
        nextT = t + 0.024f;
        const float* v = HV(data, i, off); const float* o = HV(data, i, offOther);
        const float m = sqrtf(v[0] * v[0] + v[1] * v[1]), mo = sqrtf(o[0] * o[0] + o[1] * o[1]);
        w += snprintf(buf + w, sizeof(buf) - w, " %d:%s%.0f/%.1f", (int)((nowT - t) * 1000.0f + 0.5f),
                      m < 0.35f ? "~" : "", atan2f(v[1], v[0]) * 57.29578f, mo);
    }
    TwkLog("[lean] flick on %s: held %s (ms ago:deg/other mag)%s | swing shown from %s, peak %.0f deg/s",
           stick ? "L" : "R", stick ? "R" : "L", buf,
           bound > 0.0f ? "" : "(none -- no fast swing within 0.25 s)", s_scoopPeak);
    if (bound > 0.0f) TwkLog("[lean]   ... the swing starts %.0f ms before the flick", (nowT - bound) * 1000.0f);
}
// The neutral at a grind's start, per stick (the relative-angle hook fills it first; Lean takes it from there).
static float    s_entryDir[2][2];
static bool     s_entryOk[2] = { false, false }, s_entryPre[2] = { false, false };
static float    s_entryTb = -1.0f;
static uint64_t s_entryMs = 0;
static void ComputeEntry(void* ih) {
    s_entryOk[0] = s_entryOk[1] = false; s_entryTb = -1.0f; s_entryMs = GetTickCount64();
    uint8_t* arr = ih ? (uint8_t*)twkP(ih, IH_BUFFERED) : nullptr;
    uint8_t* data = arr ? *(uint8_t**)arr : nullptr;
    const int n = arr ? *(const int*)(arr + 8) : 0;
    if (!data || n < 2 || n > 4096) return;
    for (int k = 0; k < 2; k++) {
        float t = -1.0f;
        s_entryOk[k] = PreMotionDir(data, n, k ? FID_RSTICK : FID_LSTICK, s_entryDir[k], &t, &s_entryPre[k]);
        if (s_entryOk[k] && (s_entryTb < 0.0f || t < s_entryTb)) s_entryTb = t;
    }
}
static void EntryNeutral(float dir[2][2], bool pre[2], float* tb) {
    if (GetTickCount64() - s_entryMs > 300) {            // the hook did not run for this grind: take it now
        __try { ComputeEntry(CatchTweaks_LocalInputHandler()); } __except (EXCEPTION_EXECUTE_HANDLER) { s_entryOk[0] = s_entryOk[1] = false; }
    }
    for (int k = 0; k < 2; k++) {
        if (!s_entryOk[k]) continue;
        dir[k][0] = s_entryDir[k][0]; dir[k][1] = s_entryDir[k][1]; pre[k] = s_entryPre[k];
    }
    *tb = s_entryTb;
    s_entryMs = 0;                                       // used: the next grind takes its own
}

static void SnapHistory(int stick, void* ih) {
    s_nsave = 0;
    UpdateWindow(ih);
    if (!s_winHas[stick] || s_winT1 < s_winT0) return;
    const float* set = ScoopSettings(ih);
    static bool s_said = false;
    if (set && !s_said) {
        s_said = true;
        TwkLog("[lean] the game's scoop check: stick past %.2f; arc %.0f-%.0f, eighth %.0f-%.0f, quarter %.0f-%.0f, half %.0f-%.0f deg; history %.2f s",
               set[IS_CIRC_MIN / 4], set[IS_ARC / 4], set[IS_ARC / 4 + 1], set[IS_EIGHTH / 4], set[IS_EIGHTH / 4 + 1],
               set[IS_QUARTER / 4], set[IS_QUARTER / 4 + 1], set[IS_HALF / 4], set[IS_HALF / 4 + 1], twkF(ih, IH_HISTORY_TIME));
    }
    uint8_t* arr = (uint8_t*)twkP(ih, IH_BUFFERED);
    uint8_t* data = arr ? *(uint8_t**)arr : nullptr;
    const int n = arr ? *(const int*)(arr + 8) : 0;
    if (!data || n < 2 || n > 4096) return;
    const int off = stick ? FID_RSTICK : FID_LSTICK;
    if (s_winTwo) {                                      // let go since the window opened: the game's own from then
        const float* lv = (const float*)(data + (n - 1) * FID_STRIDE + off);
        const float lt = *(const float*)(data + (n - 1) * FID_STRIDE + FID_TIME);
        if (lv[0] * lv[0] + lv[1] * lv[1] < 0.04f && lt >= s_winT0 && lt < s_winLetT[stick]) s_winLetT[stick] = lt;
    }
    const float minR2 = set ? set[IS_CIRC_MIN / 4] * set[IS_CIRC_MIN / 4] : 0.0f;
    // SHOVES NEED THE OTHER STICK (533). "A shove should only be detected if you are flicking the other stick mid
    // scoop. That should make a lean never be detected as a scoop." The game FILES a scoop on any tick its 1 s
    // history reads an arc, and a pop within its window turns into a shove (the pop shuvs out of noseblunts read
    // NO arc at the pop itself -- 23 ticks of a pull-down lean had been filed before it). No extent or speed
    // rule can split a 90 deg blunt lean from a quarter scoop. So the held stick's history is ALWAYS shown as
    // the neutral, except while the OTHER stick is flicking: then the swing the held stick was making around
    // that flick is shown as it was -- from where it started, rotated onto the neutral (531's continuity).
    const float nowT = *(const float*)((const uint8_t*)ih + IH_TIME);
    const int offOther = stick ? FID_LSTICK : FID_RSTICK;
    bool newFlick = false;
    if (OtherFlicking(data, n, offOther)) {
        newFlick = s_flickT[stick] < 0.0f || nowT - s_flickT[stick] > kFlickShowSec;
        s_flickT[stick] = nowT;
    }
    float bound = -1.0f;
    if (s_flickT[stick] >= 0.0f && nowT >= s_flickT[stick] && nowT - s_flickT[stick] <= kFlickShowSec)
        bound = ScoopStart(data, n, off, s_flickT[stick]);
    if (newFlick) {
        FlushFlick(stick);
        s_flOpen[stick] = true; s_flArc[stick] = 0.0f; s_flFiled[stick] = s_flTicks[stick] = 0;
        TraceFlick(data, n, off, offOther, stick, bound, nowT);
    } else if (s_flOpen[stick] && nowT - s_flickT[stick] > kFlickShowSec) FlushFlick(stick);
    const bool haveBound = bound >= s_winT0 && bound <= s_winT1 + 0.01f;
    if (haveBound && s_shownFlickT[stick] != s_flickT[stick]) { s_shownFlickT[stick] = s_flickT[stick]; s_nShown++; }
    auto inWin = [&](float t) { return !(t < s_winT0 || t > s_winT1 || (s_winTwo && t >= s_winLetT[stick])); };
    int anchor = -1;
    for (int i = n - 1; i >= 0; i--) {
        const uint8_t* e = data + i * FID_STRIDE;
        const float t = *(const float*)(e + FID_TIME);
        if (!inWin(t) || (haveBound && t >= bound)) continue;
        const float* v = (const float*)(e + off);
        if (v[0] * v[0] + v[1] * v[1] < 0.04f) continue;
        anchor = i; break;
    }
    if (anchor < 0) return;
    const float* av = (const float*)(data + anchor * FID_STRIDE + off);
    const float am = sqrtf(av[0] * av[0] + av[1] * av[1]);
    const float ax = av[0] / am, ay = av[1] / am;
    // SHOWN FROM THE HELD DIRECTION (531). 528 pointed the window at the ANCHOR -- the leaned stick -- but the
    // samples before the window (landing into the grind at the held direction) stay in the 1 s history, and
    // the recogniser sums the angle between neighbours: the whole lean arrived as ONE jump at the window's
    // start and was filed as a scoop ("detecting a shove if I get in a grind and try to lean quickly"; the
    // pop shuvs read -83 deg over 0.99 s). Now everything before the anchor reads as the held direction, and
    // the anchor on (a scoop in flight, or just the stick now) is ROTATED onto it: continuous with the
    // landing, and a scoop keeps its own size and direction.
    const float* wb = s_winBases[stick];
    const float rc = ax * wb[0] + ay * wb[1], rs = ax * wb[1] - ay * wb[0];   // anchor -> held direction
    bool rawLive = false; float rawDeg = 0.0f;
    for (int i = 0; i < n && s_nsave < 1024; i++) {
        uint8_t* e = data + i * FID_STRIDE;
        const float t = *(const float*)(e + FID_TIME);
        if (!inWin(t)) continue;
        float* v = (float*)(e + off);
        const float m = sqrtf(v[0] * v[0] + v[1] * v[1]);
        if (m < 0.2f) continue;
        if (s_nsave == 0 && set) rawDeg = CircularBest(data, n, off, minR2, &rawLive);   // before the first change
        s_save[s_nsave].v = v; s_save[s_nsave].x = v[0]; s_save[s_nsave].y = v[1]; s_nsave++;
        if (i < anchor) { v[0] = wb[0] * m; v[1] = wb[1] * m; }
        else { const float x = v[0], y = v[1]; v[0] = x * rc - y * rs; v[1] = x * rs + y * rc; }
    }
    if (s_nsave && set) {
        bool hidLive = false;
        const float hidDeg = CircularBest(data, n, off, minR2, &hidLive);
        if (Files(set, rawDeg, rawLive)) { s_scRaw++; s_scRawMax = fmaxf(s_scRawMax, rawDeg); }
        if (Files(set, hidDeg, hidLive)) { s_scHid++; s_scHidMax = fmaxf(s_scHidMax, hidDeg); }
        if (s_flOpen[stick]) {
            s_flTicks[stick]++;
            if (hidLive) s_flArc[stick] = fmaxf(s_flArc[stick], hidDeg);
            if (Files(set, hidDeg, hidLive)) s_flFiled[stick]++;
        }
    }
}
static void RestoreHistory() {
    while (s_nsave > 0) { s_nsave--; s_save[s_nsave].v[0] = s_save[s_nsave].x; s_save[s_nsave].v[1] = s_save[s_nsave].y; }
}
static void Circular(int stick, void* ih, uint64_t l, uint64_t r) {
    void* localIh = CatchTweaks_LocalInputHandler();
    const bool mine = g_ok && ih && (!localIh || ih == localIh);
    if (mine) { __try { SnapHistory(stick, ih); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    ((CircularFn)g_circOrig[stick])(ih, l, r);
    if (mine) { __try { RestoreHistory(); } __except (EXCEPTION_EXECUTE_HANDLER) { s_nsave = 0; } }
}
static void __fastcall hkCircLeft(void* ih, uint64_t l, uint64_t r)  { Circular(0, ih, l, r); }
static void __fastcall hkCircRight(void* ih, uint64_t l, uint64_t r) { Circular(1, ih, l, r); }

// FlipTricksHandler::CheckForCrank(this, float dt) -- Epic 0x101d230 / Steam 0xfdd0e0, sigmade: unique in
// both. Returns nothing. this +0x18 = its InputHandler, +0x79 = the crank input found this call (0 = none).
static const char* SIG_CRANK =
    "48 8B C4 53 48 81 EC E0 00 00 00 48 83 B9 B8 00 00 00 00 48 8B D9 44 0F 29 54 24 60 44 0F 28 D1 0F 85 ?? ?? ?? ?? 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88";
typedef void (__fastcall* CrankFn)(void* fth, float dt);
static void*    g_crankOrig = nullptr;
static uint8_t* g_crankAt = nullptr;
enum { FTH_INPUT = 0x18, FTH_CRANK_INPUT = 0x79 };
static float*   s_ckV[2] = { nullptr, nullptr };   // the samples pointed at the held direction for this call
static float    s_ckOld[2][2];
// THE POP CHECK SEES A STICK IT ACCEPTS (532). "sometimes it doesnt even let me pop out, it just doesnt read my
// second stick input". The crank test (InputHandler::HasInputDirection on the LAST buffered sample -- the one
// this snaps) wants the held stick within 90 deg of the crank input rotated by _grindCrankRelativeAngle, and
// UpdateGrindCrankRelativeAngle builds that from the held stick's X ONLY (orient 1: -90 x cos) -- it cannot
// tell down-right from up-right. A noseblunt held diagonally DOWN (-56..-74 deg) gets a crank direction
// mirrored into the upper half, 96-139 deg off: 0-15 cranks in 60-100 checks, never popped (every such grind
// logged); held 0..-45 popped every time. Pinning the stick at the held direction made that permanent. Now
// the game's own verdict picks what it sees: the held direction; after 2 misses its MIRROR across the X
// axis (exactly the formula's blind spot); then the stick as it is; round again, staying on whatever cranks.
static int      s_ckMode = 0;                      // 0 held direction, 1 its mirror, 2 the stick as it is
static int      s_ckMiss = 0, s_ckGen = -1;
static bool     s_ckElig = false;                  // the lean owns a held stick in this call
static bool     s_ckSaid = false;

// Uses the scoop window's state (updated each InputHandler::Tick): the sticks the lean owns now.
static void CrankSnap(void* fth) {
    s_ckV[0] = s_ckV[1] = nullptr; s_ckElig = false;
    if (!g_ok || !g_on || !g_hideScoop || !s_winLive) return;
    if (s_ckGen != s_winGen) {                       // a new held direction: start from it again
        s_ckGen = s_winGen; s_ckMode = 0; s_ckMiss = 0; s_ckSaid = false;
    }
    void* ih = twkP(fth, FTH_INPUT);
    void* localIh = CatchTweaks_LocalInputHandler();
    if (!ih || (localIh && ih != localIh)) return;
    uint8_t* arr = (uint8_t*)twkP(ih, IH_BUFFERED);
    uint8_t* data = arr ? *(uint8_t**)arr : nullptr;
    const int n = arr ? *(const int*)(arr + 8) : 0;
    if (!data || n < 1 || n > 4096) return;
    uint8_t* e = data + (n - 1) * FID_STRIDE;
    const float lt = *(const float*)(e + FID_TIME);
    bool any = false; float farDeg = 0.0f;
    for (int k = 0; k < 2; k++) {
        if (!s_winHas[k] || !s_hideOk[k] || (s_winTwo && lt >= s_winLetT[k])) continue;   // let go: raw (527)
        float* v = (float*)(e + (k ? FID_RSTICK : FID_LSTICK));
        const float m = sqrtf(v[0] * v[0] + v[1] * v[1]);
        if (m < 0.2f) continue;            // released: the game's own
        const float* b = s_winBases[k];
        float d = (atan2f(v[1], v[0]) - atan2f(b[1], b[0])) * 57.29578f;
        if (d > 180.0f) d -= 360.0f; else if (d < -180.0f) d += 360.0f;
        if (fabsf(d) > 100.0f) continue;       // turned past the lean: a deliberate turn, the game's own (463's reach)
        s_ckElig = true;
        if (s_ckMode == 2) continue;           // the stick as it is (532)
        s_ckOld[k][0] = v[0]; s_ckOld[k][1] = v[1];
        v[0] = b[0] * m; v[1] = (s_ckMode == 1 ? -b[1] : b[1]) * m;
        s_ckV[k] = v; any = true; farDeg = fmaxf(farDeg, fabsf(d));
    }
    if (s_ckElig) { s_ckReads++; if (any) s_ckMaxDeg = fmaxf(s_ckMaxDeg, farDeg); }
}
static void __fastcall hkCrank(void* fth, float dt) {
    __try { CrankSnap(fth); } __except (EXCEPTION_EXECUTE_HANDLER) { s_ckV[0] = s_ckV[1] = nullptr; }
    ((CrankFn)g_crankOrig)(fth, dt);
    if (s_ckV[0] || s_ckV[1]) {
        __try {
            for (int k = 0; k < 2; k++) if (s_ckV[k]) { s_ckV[k][0] = s_ckOld[k][0]; s_ckV[k][1] = s_ckOld[k][1]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s_ckV[0] = s_ckV[1] = nullptr;
    }
    if (s_ckElig) {                                  // the game's verdict picks what it sees next (532)
        __try {
            if (twkB(fth, FTH_CRANK_INPUT) == 0) {
                s_ckNone++;
                if (++s_ckMiss >= 2) { s_ckMode = (s_ckMode + 1) % 3; s_ckMiss = 0; }
            } else {
                s_ckMiss = 0; s_ckHit[s_ckMode]++;
                static int s_nm = 0;
                if (s_ckMode != 0 && !s_ckSaid && s_nm < 100) {
                    s_ckSaid = true; s_nm++;
                    const float* b = s_winBases[s_stick];
                    TwkLog("[lean] pop check: no crank at the held direction (%.0f deg, relative angle %.0f) -- %s -> crank found",
                           atan2f(b[1], b[0]) * 57.29578f, twkF(fth, 0x4c),
                           s_ckMode == 1 ? "shown mirrored across the X axis" : "shown the stick as it is");
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s_ckElig = false;
    }
}

// THE RELATIVE ANGLE FROM THE SAME NEUTRAL (533). UpdateGrindCrankRelativeAngle(this) -- Epic 0x1038d50 /
// Steam 0xff90d0, sigmade: unique in both -- sets _grindCrankRelativeAngle (+0x4c) from the held stick's X as
// the grind begins (HandleOnEnterGrind, FlipTricksHandler::OnEnterGrind; mid-grind only on the both-idle ->
// pressed reinput edge inside CheckForCrank). The game rotates every input direction by it, the other stick's
// pop flick included. After a kickflip the stick is already pulled down there (-88: angle ~0 instead of ~-90),
// and the SAME flick read as a kickflip instead of a nollie (every kickflip entry popped NLS_Kickflip). The call
// now reads each pushed stick at the neutral (GetLeft/RightStickInput read the last buffered sample, pointed
// there for the call); a released stick (the game's own reinput) is left as it is.
static const char* SIG_RELANGLE =
    "48 8B C4 57 48 83 EC 70 0F 29 70 E8 48 8B F9 44 0F 29 48 B8 44 0F 29 50 A8 48 89 58 18 48 89 70 20";
typedef void (__fastcall* RelAngleFn)(void* fth);
static void*    g_relOrig = nullptr;
static uint8_t* g_relAt = nullptr;
static void __fastcall hkRelAngle(void* fth) {
    float* pv[2] = { nullptr, nullptr }; float old[2][2] = {}; float shown[2] = { 0.0f, 0.0f };
    bool entry = false;
    __try {
        void* ih = twkP(fth, FTH_INPUT);
        void* localIh = CatchTweaks_LocalInputHandler();
        uint8_t* arr = ih ? (uint8_t*)twkP(ih, IH_BUFFERED) : nullptr;
        uint8_t* data = arr ? *(uint8_t**)arr : nullptr;
        const int n = arr ? *(const int*)(arr + 8) : 0;
        if (g_ok && g_on && ih && (!localIh || ih == localIh) && data && n >= 2 && n <= 4096) {
            entry = GetTickCount64() - s_lastMs > 150 || !s_owned || !s_baseOk;
            if (entry) ComputeEntry(ih);
            for (int k = 0; k < 2; k++) {
                const float* d = nullptr;
                if (entry) { if (s_entryOk[k]) d = s_entryDir[k]; }
                else if ((TwoStick(s_leanOrient) ? s_has[k] : k == s_stick) && !s_rel[k]) d = TwoStick(s_leanOrient) ? s_bases[k] : s_base;
                if (!d) continue;
                float* v = (float*)(data + (n - 1) * FID_STRIDE + (k ? FID_RSTICK : FID_LSTICK));
                const float m = sqrtf(v[0] * v[0] + v[1] * v[1]);
                if (m < 0.35f) continue;
                old[k][0] = v[0]; old[k][1] = v[1]; pv[k] = v;
                v[0] = d[0] * m; v[1] = d[1] * m;
                shown[k] = atan2f(d[1], d[0]) * 57.29578f;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { pv[0] = pv[1] = nullptr; }
    ((RelAngleFn)g_relOrig)(fth);
    if (pv[0] || pv[1]) {
        __try {
            for (int k = 0; k < 2; k++) if (pv[k]) {
                static int s_nl = 0;
                if (s_nl < 200 && fabsf(atan2f(old[k][1], old[k][0]) * 57.29578f - shown[k]) > 5.0f) {
                    s_nl++;
                    TwkLog("[lean] relative angle %.0f from the %s neutral: %s stick read at %.0f deg (it was at %.0f)",
                           twkF(fth, 0x4c), entry ? "grind's" : "held", k ? "R" : "L", shown[k], atan2f(old[k][1], old[k][0]) * 57.29578f);
                }
                pv[k][0] = old[k][0]; pv[k][1] = old[k][1];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// After the game's own grind step, on the same body: dig in or ride light (see THE LEAN DIGS IN).
static void Drag(void* self, float dt, void* body) {
    if (!g_addImpulse || !g_bodyVel || !body || self != s_bmc) return;
    if (!(dt > 0.0f && dt < 0.1f)) return;
    const bool live = s_digMode != 0 && GetTickCount64() - s_fricMs < 80;
    float v[3] = { 0.0f, 0.0f, 0.0f };
    g_bodyVel(body, v);
    const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(speed > 50.0f && speed < 1e5f)) { s_prevSpeed = -1.0f; return; }
    // The grind's own slowdown, from the speed lost across the game's step while not leaning.
    if (s_prevSpeed > 0.0f && live && fabsf(s_dig) < 0.05f) {
        const float loss = (s_prevSpeed - speed) / dt;
        if (loss > -2000.0f && loss < 2000.0f) s_natural += (fmaxf(0.0f, loss) - s_natural) * fminf(1.0f, dt / 0.4f);
    }
    float decel = 0.0f;
    if (live) {
        const float pk = g_friction * 0.01f;
        if (s_digMode == 1) decel = s_dig >= 0.0f ? kDigDecel * pk * s_dig
                                                  : -kLightGive * fminf(s_natural, 300.0f) * pk * -s_dig;
        else                decel = 0.5f * kDigDecel * pk * s_dig;
    }
    float dv = -decel * dt;
    if (dv < -0.5f * speed) dv = -0.5f * speed;           // never stops or reverses it in one step
    if (dv != 0.0f) {
        const float imp[3] = { v[0] / speed * dv, v[1] / speed * dv, v[2] / speed * dv };
        g_addImpulse(body, imp, true);
    }
    s_prevSpeed = speed + dv;

    // THE LEAN COMMITS YOU: the off-line speed the game's step left (+ toes), and the push off the open edge.
    float sx = 0.0f, sy = 0.0f;
    if (s_toeOk) {
        sx = s_toeW[0]; sy = s_toeW[1];
        const float hn = sqrtf(v[0] * v[0] + v[1] * v[1]);
        if (hn > 1.0f) { const float d = (sx * v[0] + sy * v[1]) / hn; sx -= d * v[0] / hn; sy -= d * v[1] / hn; }
        const float sn = sqrtf(sx * sx + sy * sy);
        if (sn > 0.3f) { sx /= sn; sy /= sn; } else sx = sy = 0.0f;   // toes along the rail (a slide): no across
    }
    if (live && g_commit > 0.0f && (sx != 0.0f || sy != 0.0f)) {
        const float c = g_commit * 0.01f;
        float push = 0.0f;
        if (s_digMode == 1 && s_dig < 0.0f) push = kCommitPush * c * -s_dig;
        else if (s_digMode == 2)            push = 0.5f * kCommitPush * c * s_dig;
        if (push > 0.0f) {
            const float way = s_rollLean >= 0.0f ? 1.0f : -1.0f;       // the lean's side: off a ledge, the open edge
            const float imp[3] = { sx * way * push * dt, sy * way * push * dt, 0.0f };
            g_addImpulse(body, imp, true);
        }
    }
    if (live && !s_commitSaid && s_ledge >= 0 && s_alignBase >= 0.0f) {
        s_commitSaid = true;
        static int s_nc = 0;
        if (s_nc < 100) {
            s_nc++;
            TwkLog("[lean] commitment: the game's grind alignment is %.2f (your option) -- %s; grind step %.1f ms",
                   s_alignBase, s_digMode == 1 ? "into the ledge locks it toward 1.00, away lets it go and pushes you off the open edge"
                                               : "leaning either way lets half of it go and pushes you toward the lean", dt * 1000.0f);
        }
    }
}

// THE LEAN COMMITS YOU: the alignment its grind step reads, this step (see the note up top). Returns the
// value to put back after the step, or null when it is left alone.
static float* CommitAlign(void* self, float* was, float* want) {
    if (self != s_bmc || !(s_digMode != 0 && GetTickCount64() - s_fricMs < 80)) return nullptr;
    void* sk = twkP(self, BMC_SKATER);
    if (!sk) return nullptr;
    float* ar = (float*)((uint8_t*)sk + SK_ALIGN);
    const float base = *ar;
    if (!(base >= 0.0f && base <= 2.0f)) return nullptr;        // not a value we know
    float r = base;
    const float c = g_commit * 0.01f;
    if (c > 0.0f) {
        if (s_digMode == 1 && s_dig > 0.0f) { if (base < 1.0f) r = base + (1.0f - base) * fminf(1.0f, c * s_dig); }
        else if (s_digMode == 1)            r = base * fmaxf(0.0f, 1.0f - kLetGo * c * -s_dig);
        else                                r = base * fmaxf(0.0f, 1.0f - 0.5f * kLetGo * c * s_dig);
    }
    s_alignBase = base;
    if (r == base) return nullptr;
    *ar = r; *was = base; *want = r;
    return ar;
}
static void __fastcall hkGrindMove(void* self, float dt, void* body, void* p4) {
    float* ar = nullptr; float was = 0.0f, want = 0.0f;
    if (g_ok) { __try { ar = CommitAlign(self, &was, &want); } __except (EXCEPTION_EXECUTE_HANDLER) { ar = nullptr; } }
    ((GrindMoveFn)g_moveOrig)(self, dt, body, p4);
    if (ar) { __try { if (*ar == want) *ar = was; } __except (EXCEPTION_EXECUTE_HANDLER) {} }   // unless the option moved it
    if (!g_ok) return;
    __try { Drag(self, dt, body); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_addImpulse = nullptr; TwkLog("[lean] caught fatal in the grind drag -- grind friction off for this run"); }
}

// The game's own banking step, if it runs mid-grind, eases toward its grind target (nothing); ours
// goes back on after it while we drive the lean.
static void __fastcall hkBanking(void* self, float dt) {
    ((BankingFn)g_bankOrig)(self, dt);
    if (s_give.con && self == s_smc && GetTickCount64() - s_lastMs > 150) RestoreTruck();   // the grind is over
    if (!g_ok || !s_owned || self != s_smc) return;
    __try {
        if (GetTickCount64() - s_lastMs > 150) { s_owned = false; return; }   // the grind is over
        s_bankCalls++;
        WriteBank(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_owned = false; }
}
static void __fastcall hkAlign(void* self, float a, uint64_t b) {
    if (s_owned && self == s_smc) s_alignCalls++;
    ((AlignFn)g_alignOrig)(self, a, b);
}

// ------------------------------------------------------------------ install + menu
void GrindLean_Install() {
    g_bankAt = TwkScanExe(SIG_BANKING);
    if (g_bankAt && (MH_CreateHook(g_bankAt, (void*)&hkBanking, &g_bankOrig) != MH_OK || MH_EnableHook(g_bankAt) != MH_OK)) g_bankAt = nullptr;
    g_alignAt = TwkScanExe(SIG_ALIGN_GROUND);
    if (g_alignAt && (MH_CreateHook(g_alignAt, (void*)&hkAlign, &g_alignOrig) != MH_OK || MH_EnableHook(g_alignAt) != MH_OK)) g_alignAt = nullptr;
    g_curveEval = (CurveEvalFn)TwkScanExe(SIG_CURVE_EVAL);
    g_conDrive  = (ConDriveFn)TwkScanExe(SIG_CON_DRIVE);
    g_addImpulse = (AddImpulseFn)TwkScanExe(SIG_ADD_IMPULSE);
    g_bodyVel    = (BodyVelocityFn)TwkScanExe(SIG_BODY_VELOCITY);
    g_moveAt = TwkScanExe(SIG_GRIND_MOVE);
    if (!g_moveAt || !g_addImpulse || !g_bodyVel
        || MH_CreateHook(g_moveAt, (void*)&hkGrindMove, &g_moveOrig) != MH_OK || MH_EnableHook(g_moveAt) != MH_OK) {
        TwkLog("[lean] grind step %s -- the lean does not change grind friction",
               !g_moveAt ? "NOT FOUND" : (!g_addImpulse || !g_bodyVel) ? "found, body functions NOT FOUND" : "hook failed");
        g_moveAt = nullptr;
    }
    if (!g_conDrive) TwkLog("[lean] constraint drive setter NOT FOUND -- the trucks keep their stiffness");
    void* circHk[2] = { (void*)&hkCircLeft, (void*)&hkCircRight };
    for (int k = 0; k < 2; k++) {
        g_circAt[k] = TwkScanExe(SIG_CIRC[k]);
        if (g_circAt[k] && (MH_CreateHook(g_circAt[k], circHk[k], &g_circOrig[k]) != MH_OK || MH_EnableHook(g_circAt[k]) != MH_OK)) g_circAt[k] = nullptr;
    }
    if (!g_circAt[0] || !g_circAt[1])
        TwkLog("[lean] scoop check %s NOT FOUND or hook failed -- the game can read a lean as a scoop", !g_circAt[0] ? "(left)" : "(right)");
    g_crankAt = TwkScanExe(SIG_CRANK);
    if (g_crankAt && (MH_CreateHook(g_crankAt, (void*)&hkCrank, &g_crankOrig) != MH_OK || MH_EnableHook(g_crankAt) != MH_OK)) g_crankAt = nullptr;
    if (!g_crankAt) TwkLog("[lean] crank check NOT FOUND or hook failed -- a hard lean can lose the crank at the pop");
    g_relAt = TwkScanExe(SIG_RELANGLE);
    if (g_relAt && (MH_CreateHook(g_relAt, (void*)&hkRelAngle, &g_relOrig) != MH_OK || MH_EnableHook(g_relAt) != MH_OK)) g_relAt = nullptr;
    if (!g_relAt) TwkLog("[lean] relative-angle update NOT FOUND or hook failed -- a lean begun at the landing sets the pop's relative angle");
    TwkLog("[lean] installed: banking step %s, body align %s, curve eval %s (%s, strength %.0f%%, board roll %.0f%%)",
           g_bankAt ? "hooked" : "NOT FOUND", g_alignAt ? "counted" : "NOT FOUND", g_curveEval ? "found" : "NOT FOUND (6 deg)",
           g_on ? "ON" : "off", g_strength, g_rollPct);
}

void GrindLean_DrawMenu(const OmpMenuApi* api) {
    bool on = g_on != 0;
    if (api->Checkbox("Grind lean", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(push the held stick a little to the side to lean; smith, boardslide...: push both sticks the same way)");
    if (!on) return;
    api->Indent();
    float s = g_strength;
    if (api->SliderFloat("Lean strength (%)", &s, 0.0f, 200.0f, "%.0f")) GrindLean_SetStrength(s);
    api->SameLine(); api->TextDisabled("(100 = as far as the game leans you when carving)");
    float rp = g_rollPct;
    if (api->SliderFloat("Board roll (%)", &rp, 0.0f, 400.0f, "%.0f")) GrindLean_SetRollPct(rp);
    api->SameLine(); api->TextDisabled("(extra roll at a full push, past your trucks' own max; looser trucks roll further)");
    float fr = g_friction;
    if (api->SliderFloat("Grind friction (%)", &fr, 0.0f, 200.0f, "%.0f")) GrindLean_SetFriction(fr);
    api->SameLine(); api->TextDisabled("(ledge: lean into it to dig in and slow, away to ride light; rail: either way slows)");
    float cm = g_commit;
    if (api->SliderFloat("Grind commitment (%)", &cm, 0.0f, 200.0f, "%.0f")) { g_commit = fmaxf(0.0f, fminf(200.0f, floorf(cm + 0.5f))); TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(ledge: lean into it to lock on, away to let go and ride off the open edge; rail: half, either way)");
    float tg = g_truckGive;
    if (api->SliderFloat("Truck give (%)", &tg, 0.0f, 100.0f, "%.0f")) GrindLean_SetTruckGive(tg);
    api->SameLine(); api->TextDisabled("(the grinding truck loosens as you lean: wheel bite; your tightness returns after)");
    float pd = g_pressDeg;
    if (api->SliderFloat("Foot pressure tilt (deg)", &pd, 0.0f, 15.0f, "%.0f")) { g_pressDeg = fmaxf(0.0f, fminf(15.0f, floorf(pd + 0.5f))); TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(50-50 style, both sticks: the foot you lean harder with sinks its end, and each foot works its own truck)");
    bool hs = g_hideScoop != 0;
    if (api->Checkbox("Tricks ignore the lean", &hs)) { g_hideScoop = hs ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the game's scoop and crank checks read the held stick at its held direction: pop out of a lean like without one)");
    if (s_wall > 0.0f) {
        char w[96]; snprintf(w, sizeof(w), "your trucks: max roll %.1f deg (tightness %.2f)", (float)s_wall, (float)s_tight);
        api->TextDisabled(w);
    }
    char b[96];
    snprintf(b, sizeof(b), g_uiLive ? "now: leaning %+.2f (+ = toes)" : "now: not on a grind", (float)g_uiLean);
    api->TextDisabled(b);
    api->Unindent();
}
