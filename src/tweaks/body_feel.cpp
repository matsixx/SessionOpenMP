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
// SessionTweaks -- BODY FEEL v2. See body_feel.h for what this is; mechanics below.
//
// V1 WAS A DEAD LETTERBOX (field, 2026-08-29): the one native knob the PDB offered --
// ASkaterCharacterBase::PhysicalAnimationBlendWeightMultiplier (+0x548) -- read 0.000 authored,
// and writing it up to 2.0 changed nothing on screen. If the Blueprint consumed it as a factor,
// 0 would mean no body physics at all, yet the body visibly reacts: the field is an unused
// property. (Same field run also proved skater+0x710 is a shared BITFIELD byte -- the whole-byte
// "_isRagDoll" test read 1 during normal riding and starved v1's writes. No bit test needed now:
// Bail broadcasts DisablePhysicalAnimation, so the physOn flag alone covers ragdolls.)
//
// V2 WRITES WHERE THE ENGINE ACTUALLY READS: every physics body carries
// FBodyInstance::PhysicsBlendWeight (+0x11c), the per-body fraction of PHYSICS in the final pose,
// consumed by the skeletal mesh's own anim blend each frame. The chain is plain memory:
// skater+0x280 Mesh -> component+0x980 Bodies (TArray<FBodyInstance*>) -> body+0x11c. The
// Blueprint authors these once at its enable event; we capture each body's authored weight the
// first time we see it, then write authored * feel every tick. Self-healing like the crankvis
// scrub: a body whose current value is not our last write was re-authored by the game -> that
// body is re-captured, so enable events and profile switches pass through untouched. Bodies the
// game left at 0 STAY at 0 -- never activate physics the game turned off.
//
// THE MODEL -- blend weight means "how much physics shows", so the instincts read as:
//   loose in the air     more physics: limbs trail through rotations     (airborne +25%)
//   give on impact       a SURGE of physics on landings, scaled by drop, breathed back in
//   braced at speed      less physics: a body at speed is set            (SpeedRatio -> -15%)
//   coiled on the load   less physics while crouching a pop              (depth -> -20%)
//   set on the rail      less physics while grinding                    (-15%)
// All eased (tau ~130 ms) so nothing steps. BodyFeelAmount scales the deviation from authored
// (0 = stock, 100 = tuned, 200 = exaggerated). Per-body result clamped to [0, 1].
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "body_feel.h"
#include "foot_place.h"      // FootPlace_AnimInstance -- the per-tick anim state
#include "catch_tweaks.h"    // CatchTweaks_Skater -- the own player's skater
#include "pop_probe.h"       // PopProbe_CrouchDepth01 -- the pop scheme's live crouch depth
#include "grind_pop.h"       // GrindPop_FNameToString -- bone names for the brace classifier
#include <windows.h>
#include <cmath>
#include <cstdio>

// ------------------------------------------------------------------ offsets (PDB, all named)
enum {
    SK_MESH            = 0x280,   // ACharacter::Mesh (USkeletalMeshComponent*)
    SK_PHYSANIM_ON     = 0x711,   // _isPhysicalAnimationEnabled -- only modulate a live system
    SK_STATE_BITS      = 0x710,   // shared bitfield byte; _isRagDoll = BIT 1 (0x02), read off
                                  // Bail's own `test al,2 / or al,2` at 0xfeb016 -- the whole-
                                  // byte test that sank v1 must never come back
    CMP_BODIES         = 0x980,   // USkeletalMeshComponent::Bodies (TArray<FBodyInstance*>)
    BI_BLEND_WEIGHT    = 0x11c,   // FBodyInstance::PhysicsBlendWeight (0..1, physics fraction)
    AN_SPEED_RATIO     = 0x2f4,   // USkaterAnimInstance::SpeedRatio (0..1)
    AN_GROUNDED_BF     = 0x5fa,   // same grounded byte foot_place/pop_probe read
    AN_IS_GRINDING_BF  = 0x33c,   // IsGrinding (+0x33d IsGrindingInLiptrick right behind)
    AN_LAND_DROP       = 0x604,   // LandDropHeightRatio (0..1) -- how big the landing was
    // The bail brace's chain (all PDB-named): each body knows its bone, the mesh knows the
    // bone's name, and FBodyInstance::AddForce (sig below) is the engine's own game-thread
    // safe force entry (it routes through the physics command path).
    BI_BONE_INDEX      = 0x01c,   // FBodyInstance::InstanceBoneIndex (int16)
    CMP_SKELMESH       = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SKM_REFSKEL_INFO   = 0x1b0 + 0x20, // USkeletalMesh::RefSkeleton.FinalRefBoneInfo (TArray)
    BONEINFO_STRIDE    = 12,      // FMeshBoneInfo { FName Name; int32 ParentIndex; }
    SK_ROOT            = 0x130,   // AActor::RootComponent
    CTW_TRANSLATION    = 0x1c0 + 0x10, // USceneComponent ComponentToWorld: quat 16B, then pos
    CTW_QUAT           = 0x1c0,   // ComponentToWorld start (FTransform: quat, pos, scale)
    CMP_CST_ARR        = 0x4b0,   // USkinnedMeshComponent::ComponentSpaceTransformsArray[2]
    CMP_CST_READIDX    = 0x4f4,   // CurrentReadComponentTransforms -- which buffer to read
    BI_LIN_DAMP        = 0x0b8,   // FBodyInstance::LinearDamping (+0xbc angular; log only)
};
enum { kMaxBodies = 64 };

// ------------------------------------------------------------------ config
static int g_on     = 1;      // BodyFeel -- live toggle; off restores every authored weight
// A FAULT IS NOT THE USER'S SETTING. The two fault handlers used to clear g_on, which IS the saved
// BodyFeel toggle -- so a fault switched the feature off in the menu and, on the next settings
// write, in the ini as well. A map change tears the skater and its meshes down while our tick is
// still reading them, which is exactly when a fault is likeliest: the reported symptom was
// "reactive body resets to off whenever I change maps", and it was this.
// Health lives in its own flag, is never read from or written to the ini, and CLEARS when a new
// mesh is picked up -- a fault taken during a teardown says nothing about the next map.
static int g_fault  = 0;
static int g_amount = 100;    // BodyFeelAmount -- percent of the tuned character (0 = stock)
static int g_log    = 0;      // BodyFeelLog -- 1 Hz state line

// THE PER-BAIL CHATTER lives behind BodyFeelLog. Every line routed through this fires on an
// ordinary fall -- five or six of them each time -- which is diagnostics, not news, and it
// filled the log of anyone who never asked for it. The one-time lines are deliberately NOT
// routed here: the config line, the resolved signatures and the fault lines are what make
// somebody's bug report readable, and they cost four lines a session.
#define TwkLogBail(...) do { if (g_log) TwkLog(__VA_ARGS__); } while (0)
static int g_bail   = 1;      // BodyFeelBrace -- the skater reaches to protect the fall
// MUSCLE TONE. This is the piece the research says everyone else has and we did not: an
// active ragdoll drives EVERY joint toward a pose all the time, and behaviours bias a few
// joints on top of that baseline. Only three joint groups were being driven here and every
// other joint was completely limp -- and a limp joint has no reason to leave a bad position,
// which is why limbs tangle and the body gets stuck instead of rolling. Everything else is
// now driven gently toward the pose its constraint was authored in.
static float g_toneStiff = 200.0f;
static float g_toneDamp  = 45.0f;
// ARMS GET ALMOST NONE. A joint drive is a constraint-level servo and it beats any force we
// can apply, so putting tone on the shoulders and elbows quietly locked the arms at their
// rest pose and the grab stopped being able to move them anywhere (field: "the hands do not
// go toward the bone like they used to"). The arms stay loose enough to be steered.
static float g_toneArmStiff = 45.0f;
static int   g_rollMaxTries = 2;     // BodyFeelRollTries: then he stays where he landed
static int   g_jointTone = 1;        // BodyFeelJointTone
static float g_curlStiff = 900.0f;   // how hard the spine holds its curl
static float g_curlDamp  = 90.0f;
// Spine and hip both ask for far more than any joint will give and are clamped back to the
// rig's own room -- that is deliberate. The number is not an angle so much as "fold as far
// as this joint allows, this way round", which is what the body reads as a real tuck.
static float g_curlDeg   = 280.0f;   // spine/neck: how far each joint bends
static int   g_curlAxis  = 2;        // 0 = X, 1 = Y, 2 = Z (sign of the angle flips it)
static float g_hipDeg    = -50.0f;   // hips: the knees come up toward the chest. NEGATIVE is
                                     // field-settled -- it flexes the thigh the way the knee
                                     // folds the heel, and the positive value it replaced was
                                     // only ever "as far as the joint allows, the other way"
static int   g_hipAxis   = 2;
static float g_kneeDeg   = 70.0f;    // knees: the heels fold in toward the butt
static int   g_kneeAxis  = 0;
static int   g_driveCurl = 1;        // BodyFeelDriveCurl
static int g_freeJoints = 0;  // BodyFeelFreeJoints -- OFF: the field tried freeing the
                              // game's joint damping during ragdolls and it did not help,
                              // so the game's own drives are left exactly as they were

static void ReadBraceTuning(const char* buf);
static void SaveBraceTuning(char* buf, size_t cap);
static void ResetBraceTuning();

void BodyFeel_ReadConfig(const char* buf) {
    g_on     = TwkIniInt(buf, "BodyFeel", 1);
    g_amount = TwkIniInt(buf, "BodyFeelAmount", 100);
    if (g_amount < 0) g_amount = 0; else if (g_amount > 200) g_amount = 200;
    g_log    = TwkIniInt(buf, "BodyFeelLog", 0);
    g_bail   = TwkIniInt(buf, "BodyFeelBrace", 1);
    g_freeJoints = TwkIniInt(buf, "BodyFeelFreeJoints", 1);
    g_driveCurl  = TwkIniInt(buf, "BodyFeelDriveCurl", 1);
    g_curlDeg    = (float)TwkIniInt(buf, "BodyFeelCurlDeg", 280);
    g_curlAxis   = TwkIniInt(buf, "BodyFeelCurlAxis", 2);
    g_hipDeg     = (float)TwkIniInt(buf, "BodyFeelHipDeg", -50);
    g_hipAxis    = TwkIniInt(buf, "BodyFeelHipAxis", 2);
    g_kneeDeg    = (float)TwkIniInt(buf, "BodyFeelKneeDeg", 70);
    g_kneeAxis   = TwkIniInt(buf, "BodyFeelKneeAxis", 0);
    g_curlStiff  = (float)TwkIniInt(buf, "BodyFeelCurlStiff", 900);
    g_jointTone  = TwkIniInt(buf, "BodyFeelJointTone", 1);
    g_rollMaxTries = TwkIniInt(buf, "BodyFeelRollTries", 2);
    g_toneStiff  = (float)TwkIniInt(buf, "BodyFeelToneStiff", 200);
    g_toneArmStiff = (float)TwkIniInt(buf, "BodyFeelToneArmStiff", 45);
    ReadBraceTuning(buf);
    TwkLog("[body] config: BodyFeel=%d Amount=%d%% -- per-body physics blend breathes with the "
           "skating (loose in air, gives on landings, set at speed/on rails)", g_on, g_amount);
}
void BodyFeel_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "BodyFeel", g_on);
    TwkIniSetInt(buf, cap, "BodyFeelAmount", g_amount);
    TwkIniSetInt(buf, cap, "BodyFeelLog", g_log);
    TwkIniSetInt(buf, cap, "BodyFeelBrace", g_bail);
    TwkIniSetInt(buf, cap, "BodyFeelFreeJoints", g_freeJoints);
    TwkIniSetInt(buf, cap, "BodyFeelDriveCurl", g_driveCurl);
    TwkIniSetInt(buf, cap, "BodyFeelCurlDeg", (int)g_curlDeg);
    TwkIniSetInt(buf, cap, "BodyFeelCurlAxis", g_curlAxis);
    TwkIniSetInt(buf, cap, "BodyFeelHipDeg", (int)g_hipDeg);
    TwkIniSetInt(buf, cap, "BodyFeelHipAxis", g_hipAxis);
    TwkIniSetInt(buf, cap, "BodyFeelKneeDeg", (int)g_kneeDeg);
    TwkIniSetInt(buf, cap, "BodyFeelKneeAxis", g_kneeAxis);
    TwkIniSetInt(buf, cap, "BodyFeelCurlStiff", (int)g_curlStiff);
    TwkIniSetInt(buf, cap, "BodyFeelJointTone", g_jointTone);
    TwkIniSetInt(buf, cap, "BodyFeelRollTries", g_rollMaxTries);
    TwkIniSetInt(buf, cap, "BodyFeelToneStiff", (int)g_toneStiff);
    TwkIniSetInt(buf, cap, "BodyFeelToneArmStiff", (int)g_toneArmStiff);
    SaveBraceTuning(buf, cap);
}
void BodyFeel_ResetDefaults() { g_on = 1; g_amount = 100; g_bail = 1; ResetBraceTuning(); }

bool  BodyFeel_Enabled()          { return g_on != 0; }
void  BodyFeel_SetEnabled(bool o) { g_on = o ? 1 : 0; TwkMarkDirty(); }
float BodyFeel_AmountPct()        { return (float)g_amount; }
void  BodyFeel_SetAmountPct(float v) {
    g_amount = (int)(v + 0.5f);
    if (g_amount < 0) g_amount = 0; else if (g_amount > 200) g_amount = 200;
    TwkMarkDirty();
}

// ------------------------------------------------------------------ the instincts (live statics)
static float g_airMore   = 0.25f;  // more physics while airborne
static float g_brace     = 0.15f;  // less at full speed
static float g_coil      = 0.20f;  // less at full crouch depth
static float g_railSet   = 0.15f;  // less while grinding
static float g_landGive  = 0.60f;  // the landing surge at a max-height drop
static float g_landMinGive = 0.15f;// even a curb hop shows a little give
static int   g_tauMs     = 130;    // ease time constant
static int   g_recoverMs = 340;    // how long a landing surge takes to breathe back in
// the bail brace (live statics)
static float g_bailAccel  = 1560.0f; // cm/s^2 pulled through the reaching bodies (60% of base)
static float g_tuckAccel  = 1100.0f; // FLIGHT: the head held off the ground (world-up)
static int   g_bailMs     = 900;     // FLIGHT: ms over which the reach blends throw->down
// the CLUTCH: from the instant of touchdown (detected off live body positions) the body
// curls up and holds the hurt -- hands seek the part that hit the ground, calves/feet draw
// toward the pelvis, the head tucks in. Constant target-seeking (not pulses), breathing at
// the bail's own rock pace, decaying to genuine rest.
static int   g_clutchMs    = 8250;    // how long the pain hold lasts
// A BEAT before the body reacts: for this long after touchdown NOTHING drives -- pure
// physics and momentum, which is where the natural "scorpion" flop lives (field-blessed:
// legs whipping over at first contact is real; the same pose HELD by our forces is not).
// Also simply human: nobody reacts the frame they land.
static int   g_grabDelayMs = 0;
// Landing flail: for the first moment after touchdown the legs PEDAL -- one thigh flexes
// toward the chest while the other extends, alternating. Motion breaks ground friction
// (a static both-thigh pull barely moved the knees) and reads alive; then the fetal hold.
static int   g_flailDelayMs = 0;      // the flail starts THIS long after the bail edge
                                      // (in the air), not at collision -- field ask
static int   g_flailMs     = 1800;
static float g_flailHz     = 1.4f;    // slower alternation: each kick gets time to move the leg
static float g_flailTorque = 120.0f;   // landing flail: alternating hip torque. Raised
                                       // hard twice: field reports the torque era reads
                                       // invisible -- if it STILL does at this level,
                                       // suspect the torque pathway, not the magnitude
static float g_flailAccel  = 7280.0f;   // 140%
static float g_clutchAccel = 2800.0f; // hands toward the impact part
static float g_headHitMin  = 120.0f;  // ANY real knock to the head or neck counts -- you
                                      // clutch your head whether you cracked it or just
                                      // banged it (field rule)
static float g_headTopCm   = 13.0f;   // and the hands go to the TOP of the skull: the head
                                      // body's own centre sits low, right by the neck,
                                      // which is where they kept ending up
// ARMS HAVE A LENGTH. A person reaches toward what hurts and stops when the arm runs out --
// they do not contort to get there. Past this distance from the shoulder the reach fades,
// which is what stops both arms hauling down toward one low knee (field).
// ARMS SETTLE IN. Face-down the in-front keeper stands down (pushing arms "toward the
// belly" would drive them into the pavement), so nothing tidied them and they were left
// splayed out at waist level wherever the impact dropped them. A person who has face-planted
// ends up with their hands near their shoulders, so a splayed arm is drawn gently back in --
// weak, distance-gated, and internal against its own shoulder so it cannot shift the body.
static bool  g_grabbing[kMaxBodies];  // this arm is holding the hurt part right now
static float g_settleAccel = 900.0f;
static float g_settleFar   = 34.0f;   // only an arm further out than this is drawn in
static float g_settleOut   = 30.0f;   // the rest spot sits well OUT from the shoulder --
                                      // clear of the body, or the arm is simply dragged
                                      // in under the chest, which is where they kept
                                      // ending up (field)
static float g_settleDown  = 20.0f;   // ...a little down toward the hip
static float g_settleGround = 8.0f;   // ...and ON the floor, not inside the torso
static float g_reachEasy   = 55.0f;   // comfortable reach from the shoulder, cm
static float g_reachMax    = 100.0f;  // ...beyond this the hand simply does not go. A roll
                                      // leaves the hurt part further away than it was on
                                      // landing, and cutting off at arm's length made the
                                      // skater simply stop holding it (field).
// EVERY steering force is an internal COUPLE (equal-and-opposite on a partner body), like a
// muscle: one-sided world-space pulls drag the whole ragdoll toward the target ("slides on
// the ground like they're being pulled" -- the .91 field verdict), and any net along-throw
// push flings the body. Momentum-neutral couples also give tumble for free: the flight
// reach + leg counter-push IS the pitch-over rotation of a fast bail.
// MOMENTUM CARRY v3: the engine dumps speed at the ragdoll edge (proven when removing the
// carry lost the forward momentum), but full pre-bail speed reads too strong (a real fall
// bleeds energy into the tumble). v3 matches each body ALONG THE THROW AXIS only, up to
// g_carryFrac of the pre-bail speed, and only ever ACCELERATES toward that target (never
// brakes) -- it cannot overshoot, sidetrack, or fight a body the engine already threw.
// (v1 flung: its velocity sensor smoothed in from zero = fake deficit. The sensor stays
// seeded with the pre-bail velocity at first sight.)
static float g_carryFrac   = 0.55f;   // fraction of pre-bail speed the ragdoll keeps
static int   g_carryMs     = 250;
static float g_carryGain   = 8.0f;    // accel = along-throw deficit * gain (1/s)
static float g_carryMax    = 6000.0f; // cm/s^2 clamp per body
// RIDING ARM REFLEXES: the arms as balance, not luggage (field: "arms just keep the
// animation's momentum... make them more reactive"). The game zeroes most body weights at
// speed, so first the arms get a physics-weight FLOOR (the motors+physics stay alive),
// then three reflex forces lean them against those motors: carve sway (centripetal), air/
// grind spread (up-and-out balance), landing absorb (downward accent on the pulse).
static int   g_armAmt      = 170;     // Arm reactivity (%) -- 0 = stock luggage arms
static float g_armFloor    = 0.35f;   // weight floor on arm bodies while riding
static float g_armFloorNow = 0.0f;    // per-frame resolved floor (0 = layer off)
static float g_swayGain    = 1.0f;    // accel = gain * |yawRate| * speed
static float g_swayMax     = 2200.0f; // cm/s^2 cap on the sway
static float g_swaySign    = 1.0f;    // flip if the arms lean INTO the turn
static float g_spreadAccel = 900.0f;  // air/grind balance spread (55% up, 45% out)
static float g_landAccent  = 1200.0f; // downward absorb accent, scaled by the landing pulse
// TORQUE-SPACE (field direction: "rotate joints, don't pull"): the flail, the roll and
// the arms-in-front keeper act via AddTorqueInRadians in accel mode (rad/s^2).
// (THE STUMBLE lived here 3.19.113-120 and was removed on field verdict.)
// FORCES, RESTORED (results verdict): three rounds of torque behaviors read as invisible
// in the field while every force behavior always showed -- the AddTorqueInRadians pathway
// is unproven-ineffective here (kept resolved for a future isolated test). The roll and
// flail return to their proven force mechanics; alternation and ramping are what keep the
// old dragging/folding artifacts away, and the corrected facing compass (.120) now feeds
// them the right directions for the first time.
// THE FETAL CURL, retried now that the facing is right. Every earlier attempt failed on a
// bad compass: the knee target has to sit IN FRONT OF THE CHEST, and without a trustworthy
// belly axis that point landed behind the body, where the only way to reach it was to fold
// the legs backward over the back. With the body frame it is well defined, and the pull
// lands on the THIGHS (hip flexion -- a femur pulled toward the chest can only flex the
// hip) while the calves trail. Momentum-neutralised: this is a free internal rearrangement,
// nothing braced against the floor, so it must not drag the body anywhere.
static float g_curlAccel     = 4800.0f; // thighs toward the point in front of the chest
static float g_curlFront     = 30.0f;   // how far in front of the chest that point sits
static float g_kneeRoomCm    = 18.0f;   // the knee must be THIS far off the floor before
                                        // the shin is asked to fold: lying on your back
                                        // with the leg flat there is no room under the
                                        // knee, so the only way the shin can move is UP,
                                        // which is hyperextension. Lift the thigh first,
                                        // fold second -- the order a person uses.
static float g_curlShinFront = 16.0f;   // and how far in front of the PELVIS the shin
                                        // folds to -- never at the pelvis itself, which
                                        // is lying on the floor
static float g_curlCalfK     = 0.85f;   // the SHIN coming up over the chest is what
                                        // reads as a tuck -- a thigh pulled alone just
                                        // slides along the ground and the knee folds
                                        // whichever way physics finds cheapest
static float g_curlArmK      = 0.55f;   // arms fold in
static float g_curlHeadK     = 0.5f;    // chin toward the chest
// BOUNCE: a little life in the impacts. A body that slams and stops gets a brief upward
// kick scaled by how hard it hit -- restitution, basically, which the ragdoll has none of
// (its damping reads 0/0 and it just dead-drops). Deliberately small.
// THE KNEE GUARD, as a FALLBACK rather than a fight (the field's own idea, and a better
// one than mine): the rig's knee constraints are SYMMETRIC (+/-60 degrees swing, read out
// of the ragdoll itself), so physics may fold a knee 60 degrees BACKWARD whenever that is
// the cheapest path. Rather than wrestle a leg that has started folding the wrong way,
// give up on tucking THAT leg and let it lie STRAIGHT instead -- a straight leg always
// looks fine, a backward-folded one never does. The leg is extended back along the femur
// line, the thigh takes the opposite share so nothing is pushed anywhere, and after a
// short hold it is free to try tucking again.
static float g_kneeGuard     = 2600.0f; // how hard a wrong-folding leg straightens out
static int   g_straightMs    = 700;     // ...and how long it stays out of the tuck
static float g_bounceGain    = 5.0f;    // upward accel per cm/s of arrested impact speed
static float g_bounceCap     = 3000.0f; // ...clamped here
static int   g_bounceMs      = 70;      // how long each kick lasts
static float g_rollNetUp     = 220.0f;  // hard cap on the body's NET upward acceleration
                                        // (cm/s^2, well under gravity): the roll may
                                        // rotate the body, never levitate it
static float g_rollLiftSpan  = 32.0f;   // the lift fades out over this height, so a
                                        // side that is already up stops being pushed
                                        // (that, not neutralisation, is what keeps the
                                        // roll from launching the body)
static float g_rollAccel     = 5200.0f; // the roll couple: trailing side up, leading
                                        // side down (a uniform lateral swing used to
                                        // ride along with it -- pure net force, and the
                                        // reason the body went flying; it is gone)
static float g_rollLiftAccel = 1000.0f; // the push-up: chest off the ground mid-roll
static float g_rollPushAccel = 1600.0f; // the LEADING side pressed into the ground
static float g_headCurlAcc   = 1600.0f; // head and upper spine arc in toward the stomach
static float g_legSwingAcc   = 1900.0f; // the far leg swings over like the far arm...
static float g_legSwingLift  = 1500.0f; // ...lifted so it clears instead of dragging
static float g_armPinnedPush = 3400.0f; // pinned hands PRESS THE FLOOR for leverage
static float g_armPinnedCm   = 20.0f;   // ...judged by how close it lies to the spine line
static float g_armUnderAcc   = 2000.0f; // the posting arm drives UNDER the chest...
static float g_armSwingAcc   = 2600.0f; // ...while the far arm swings OVER the top
static float g_armFreeLift   = 2600.0f; // the leading arm is LIFTED clear, not pressed
static float g_armFreeSlide  = 1600.0f; // ...and slid out from under toward the head
static float g_rollTuckAccel = 900.0f;  // the trapped arm tucked toward the body centre
static int   g_rollMs        = 2000;    // max roll effort; ends early once off the belly
static float g_rollRampS     = 0.4f;    // effort ramps in -- a struggle, not a flip
static int   g_rollCommitMs  = 700;     // once started, the roll COMMITS for this long:
                                        // ending it the instant bellyZ crosses the
                                        // success line let a single rock cancel the
                                        // effort, the body flopped back, and the retry
                                        // fired again -- a start/stop loop that reads
                                        // as twitching, not rolling
static float g_armKeepAccel  = 1400.0f; // arms held to the FRONT (force toward the belly
                                        // plane, the same mechanism class as the grab)
// The body frame is a TRUE unit belly axis: prone reads ~ -1, supine ~ +1, on the side
// ~ 0. (The old calibrated axis only ever reached +/-0.4, which its thresholds matched.)
// FALL WEIGHT (field: "the body needs more weight... no proper gravity when falling"):
// extra downward pull on every body while airborne in the ragdoll; ends at touchdown so
// it never grinds the body into the floor. 100% = g_extraGrav on top of world gravity.
static float g_extraGrav     = 750.0f;  // cm/s^2 at Fall weight 100%
static int   g_fallAmt       = 100;     // Fall weight (%)
static float g_faceDownZ     = -0.45f;  // bellyZ below this = face-down
static float g_rollDoneZ     = -0.15f;  // bellyZ above this = rolled off the belly
// THE BREAK is OFF by default: two rounds of thresholds could not separate a bad slam
// from speeds OUR OWN mid-air reach gives the hands on ordinary bails (11 limp lines in
// 7 bails even with arrest conditions) -- and a limp arm kills the grab and windmill.
// The wiring stays for a future impulse-based detector; flip g_breakOn to experiment.
static int   g_breakOn       = 0;
static float g_breakVz       = -750.0f; // an arm slammed harder than this hangs limp
static float g_breakCliff    = 600.0f;
// The shipped tuning the menu's percent sliders are relative to (100% = these).
static const float kReachBase  = 2600.0f;  // g_bailAccel
static const float kClutchBase = 2800.0f;  // g_clutchAccel
static const float kFlailBase  = 5200.0f;  // g_flailAccel

static float BraceClampPct(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 400.0f) v = 400.0f; return v;
}
static void ReadBraceTuning(const char* buf) {
    g_flailMs     = TwkIniInt(buf, "BodyFeelFlailMs", 1800);
    if (g_flailMs < 0) g_flailMs = 0; if (g_flailMs > 10000) g_flailMs = 10000;
    g_flailDelayMs = TwkIniInt(buf, "BodyFeelFlailDelayMs", 0);
    if (g_flailDelayMs < 0) g_flailDelayMs = 0;
    if (g_flailDelayMs > 3000) g_flailDelayMs = 3000;
    g_clutchMs    = TwkIniInt(buf, "BodyFeelGrabMs", 8250);
    if (g_clutchMs < 500) g_clutchMs = 500; if (g_clutchMs > 15000) g_clutchMs = 15000;
    g_grabDelayMs = TwkIniInt(buf, "BodyFeelGrabDelayMs", 0);
    if (g_grabDelayMs < 0) g_grabDelayMs = 0; if (g_grabDelayMs > 3000) g_grabDelayMs = 3000;
    g_flailAccel  = kFlailBase  * BraceClampPct((float)TwkIniInt(buf, "BodyFeelFlailPct", 140)) / 100.0f;
    g_clutchAccel = kClutchBase * BraceClampPct((float)TwkIniInt(buf, "BodyFeelGrabPct",  300)) / 100.0f;
    g_bailAccel   = kReachBase  * BraceClampPct((float)TwkIniInt(buf, "BodyFeelReachPct", 60)) / 100.0f;
    g_armAmt = TwkIniInt(buf, "BodyFeelArmPct", 170);
    if (g_armAmt < 0) g_armAmt = 0; if (g_armAmt > 300) g_armAmt = 300;
    g_fallAmt = TwkIniInt(buf, "BodyFeelFallPct", 100);
    if (g_fallAmt < 0) g_fallAmt = 0; if (g_fallAmt > 300) g_fallAmt = 300;
    int cp = TwkIniInt(buf, "BodyFeelCarryPct", 55);
    if (cp < 0) cp = 0; if (cp > 150) cp = 150;
    g_carryFrac   = (float)cp / 100.0f;
}
static void SaveBraceTuning(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "BodyFeelFlailMs",  g_flailMs);
    TwkIniSetInt(buf, cap, "BodyFeelFlailDelayMs", g_flailDelayMs);
    TwkIniSetInt(buf, cap, "BodyFeelGrabMs",   g_clutchMs);
    TwkIniSetInt(buf, cap, "BodyFeelGrabDelayMs", g_grabDelayMs);
    TwkIniSetInt(buf, cap, "BodyFeelFlailPct", (int)(g_flailAccel  / kFlailBase  * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "BodyFeelGrabPct",  (int)(g_clutchAccel / kClutchBase * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "BodyFeelReachPct", (int)(g_bailAccel   / kReachBase  * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "BodyFeelCarryPct", (int)(g_carryFrac * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "BodyFeelArmPct", g_armAmt);
    TwkIniSetInt(buf, cap, "BodyFeelFallPct", g_fallAmt);
}
static void ResetBraceTuning() {
    g_flailMs = 1800; g_flailDelayMs = 0; g_clutchMs = 8250; g_grabDelayMs = 0;
    g_flailAccel = kFlailBase * 1.4f; g_clutchAccel = kClutchBase * 3.0f;
    g_bailAccel = kReachBase * 0.6f;
    g_carryFrac = 0.55f; g_armAmt = 170; g_fallAmt = 100;
}
bool  BodyFeel_BraceEnabled()          { return g_bail != 0; }
void  BodyFeel_SetBraceEnabled(bool o) { g_bail = o ? 1 : 0; TwkMarkDirty(); }
float BodyFeel_ReachPct()              { return g_bailAccel / kReachBase * 100.0f; }
void  BodyFeel_SetReachPct(float v)    { g_bailAccel = kReachBase * BraceClampPct(v) / 100.0f; TwkMarkDirty(); }
float BodyFeel_FallPct()               { return (float)g_fallAmt; }
void  BodyFeel_SetFallPct(float v)     {
    g_fallAmt = (int)(v + 0.5f);
    if (g_fallAmt < 0) g_fallAmt = 0; if (g_fallAmt > 300) g_fallAmt = 300;
    TwkMarkDirty();
}
float BodyFeel_ArmPct()                { return (float)g_armAmt; }
void  BodyFeel_SetArmPct(float v)      {
    g_armAmt = (int)(v + 0.5f);
    if (g_armAmt < 0) g_armAmt = 0; if (g_armAmt > 300) g_armAmt = 300;
    TwkMarkDirty();
}
float BodyFeel_CarryPct()              { return g_carryFrac * 100.0f; }
void  BodyFeel_SetCarryPct(float v)    {
    if (v < 0.0f) v = 0.0f; if (v > 150.0f) v = 150.0f;
    g_carryFrac = v / 100.0f; TwkMarkDirty();
}
float BodyFeel_FlailDelayMs()          { return (float)g_flailDelayMs; }
void  BodyFeel_SetFlailDelayMs(float v) {
    g_flailDelayMs = (int)(v + 0.5f);
    if (g_flailDelayMs < 0) g_flailDelayMs = 0;
    if (g_flailDelayMs > 3000) g_flailDelayMs = 3000;
    TwkMarkDirty();
}
float BodyFeel_FlailMs()               { return (float)g_flailMs; }
void  BodyFeel_SetFlailMs(float v)     {
    g_flailMs = (int)(v + 0.5f);
    if (g_flailMs < 0) g_flailMs = 0; if (g_flailMs > 10000) g_flailMs = 10000;
    TwkMarkDirty();
}
float BodyFeel_FlailPct()              { return g_flailAccel / kFlailBase * 100.0f; }
void  BodyFeel_SetFlailPct(float v)    { g_flailAccel = kFlailBase * BraceClampPct(v) / 100.0f; TwkMarkDirty(); }
float BodyFeel_GrabMs()                { return (float)g_clutchMs; }
void  BodyFeel_SetGrabMs(float v)      {
    g_clutchMs = (int)(v + 0.5f);
    if (g_clutchMs < 500) g_clutchMs = 500; if (g_clutchMs > 15000) g_clutchMs = 15000;
    TwkMarkDirty();
}
float BodyFeel_GrabDelayMs()           { return (float)g_grabDelayMs; }
void  BodyFeel_SetGrabDelayMs(float v)  {
    g_grabDelayMs = (int)(v + 0.5f);
    if (g_grabDelayMs < 0) g_grabDelayMs = 0; if (g_grabDelayMs > 3000) g_grabDelayMs = 3000;
    TwkMarkDirty();
}
float BodyFeel_GrabPct()               { return g_clutchAccel / kClutchBase * 100.0f; }
void  BodyFeel_SetGrabPct(float v)     { g_clutchAccel = kClutchBase * BraceClampPct(v) / 100.0f; TwkMarkDirty(); }

// ------------------------------------------------------------------ state
static void*  g_mesh       = nullptr;             // the mesh the captures belong to
static int    g_nBodies    = 0;
static float  g_authored[kMaxBodies];             // the game's per-body weights
static float  g_written[kMaxBodies];              // our last write, for external-change detection
static bool   g_owned[kMaxBodies];                // we are currently steering this body
static float  g_feel       = 1.0f;                // eased physics-visibility factor
static float  g_pulse      = 0.0f;
static bool   g_wasAir     = false;
static double g_airSince   = 0.0;
static double g_lastT      = 0.0;
static bool   g_apply      = false;  // pump verdict: the post-phys writer may run this frame
// the bail brace's state
static bool   g_reacher[kMaxBodies]; // bodies that reach to protect (hands/forearms)
static bool   g_headBody[kMaxBodies];// head/neck -- tucked AWAY from the fall
static bool   g_legBody[kMaxBodies]; // thigh/calf/foot
static bool   g_kneeBody[kMaxBodies];// THIGHS only -- the fetal draw (hip flexion);
                                     // calf and foot trail the knee passively
static bool   g_footBody[kMaxBodies];// feet -- passive in every ground phase
static bool   g_handBody[kMaxBodies];// hands -- never "the part that hit" (the brace makes
                                     // them the first contact on nearly every fall)
static bool   g_armBody[kMaxBodies]; // whole arm chain incl. upper arm -- a hurt arm is
                                     // grabbed by the OPPOSITE hand only
static bool   g_leftSide[kMaxBodies];// "_l_" bones -- picks pedal sides + leg indices
static int    g_thighIdx[2] = {-1,-1};   // [0]=left [1]=right, for the belly compass
static int    g_calfIdx[2]  = {-1,-1};
static int    g_footIdx[2]  = {-1,-1};
static int    g_nReachers  = 0;
static int    g_nHead      = 0;
static int    g_nLegs      = 0;
static int    g_clavIdx[2] = {-1,-1};// clavicles: the shoulder line, rigid to the torso
static int    g_pelvisBody = -1;     // the fetal curl's target body
static int    g_headIdx    = -1;     // the HEAD itself (neck hits route here)
static int    g_spineFallback = -1;
static bool   g_noImpact[kMaxBodies];// root/ik helper bodies -- never "the part that hit"
static bool   g_torsoBody[kMaxBodies];// spine/clavicle/pelvis -- the roll's rotation mass
static bool   g_armBroken[2];        // [0]=left: a slammed arm hangs limp (the break)
static bool   g_classified = false;  // classification found reachers; retry until it does
static double g_classifyAt = 0.0;    // next retry time (bone names resolve late at level load)
static float  g_pos[3]     = {0,0,0}; // root position, tracked every tick for the throw vector
static float  g_vel[3]     = {0,0,0}; // smoothed velocity (cm/s)
static bool   g_posValid   = false;
static bool   g_wasRagdoll = false;
static double g_braceStart = 0.0;
static float  g_throw[3]   = {0,0,-1}; // fall direction captured at the ragdoll edge
// the clutch's state (reseeded at the ragdoll edge)
static uint32_t g_rng       = 0x9e3779b9;
static float  g_throwVel[3] = {0,0,0}; // full pre-bail velocity (the carry target)
static float  g_throwSpeed  = 0.0f;
static float  g_bPos[kMaxBodies][3];   // live per-body world positions during the ragdoll
static float  g_bVel[kMaxBodies][3];
static bool   g_bOk[kMaxBodies];
static bool   g_bPosValid   = false;
static bool   g_sawFall     = false;
// WHAT HE HIT WHILE STILL ON THE BOARD. Everything below waits for TOUCHDOWN -- the fall
// arresting -- so the crash that caused the bail happens before any of it is watching, and
// the first thing to reach the ground afterwards gets clutched instead (field: "my knee
// hits something really hard and it picks the next bone"). A bone stopped by an obstacle at
// speed has its own signature: it is held back while the rest of him carries on. So the
// first moments of the ragdoll are scanned for the bone falling behind its peers along the
// direction he was travelling, and that bone is the one he holds.
static int    g_crashHit    = 1;       // off restores the old touchdown-only behaviour
static double g_crashWindow = 0.30;    // how long after the bail to watch for it
static float  g_crashMinSpeed = 350.0f;// below this he was not going fast enough to crash
static float  g_crashRelDrop  = 220.0f;// cm/s a bone must trail the others by to count
static int    g_crashBody   = -1;      // best candidate so far
static float  g_crashScore  = 0.0f;
static int    g_impactBody  = -1;      // the part that hit the ground first
static double g_landT       = 0.0;
static float  g_oscHz       = 0.9f;    // per-bail squeeze/rock pace
static float  g_lastYaw     = 0.0f;    // root yaw, for the carve-sway reflex
static float  g_yawRate     = 0.0f;    // rad/s, smoothed
static float  g_belly[3]    = {0,0,0}; // the body's FRONT (knee-hinge compass, re-added)
static bool   g_bellyOk     = false;
static double g_faceDownSince = 0.0;
static double g_rollUntil   = 0.0;     // roll effort window (0 = not rolling)
static float  g_rollSign    = 1.0f;    // roll toward +L * sign
static bool   g_rollDone    = false;   // one roll per bail
static int    g_rollTries   = 0;       // ...and he only tries so many times
// ONCE HE HAS GIVEN UP, THE LEGS LET GO. A tuck is a thing you do on your back or your
// side; held on a man lying face down who has stopped trying to turn over, it just looks
// wrong. When the attempts run out and he finally goes still, the leg joints hand their
// stiffness back to the rig and straighten out under their own weight (field).
static int    g_limpLegs    = 1;       // off keeps the legs driven to the end
static bool   g_legsLimp    = false;   // are they slack right now
// THE PEDAL COMES BEFORE THE TUCK. The flail skipped every leg whenever the joint drives were on,
// reasoning that a pedal underneath a commanded pose is just noise -- but the curl is driven from
// the bail edge, so the legs were commanded for the whole ragdoll and the pedal never ran at all.
// No slider could reach it. The legs are now left undriven while the flail is running and take up
// the tuck when it finishes, which is the order a person does them in anyway.
static bool   g_legsFlail   = false;
static bool   g_rollLogged  = false;   // one force-report line per roll attempt
static bool   g_legLogged   = false;   // one leg-geometry line per bail
static double g_detectStart   = 0.0;   // touchdown gates count from here
static bool   g_carryLogged = false;   // one measured-deficit line per bail
static double g_restNext    = 3.0;     // repeating settled-pose telemetry (invisible-surface hunt)
static bool   g_blendLogged = false;   // one blend-raise line per bail
static float  g_bMinVz[kMaxBodies];    // deepest recent downward vel (decays): slam detector
static float  g_bPkH[kMaxBodies];      // decaying peak horizontal speed: wall-slam detector
static int    g_grabSide   = -1;        // -1 both hands reach; 0/1 only that side does
static double g_legStraight[2] = {0,0}; // per-leg: straightening out until this time
static double g_bounceUntil[kMaxBodies];// per-body bounce kick timer
static float  g_bounceAcc[kMaxBodies];  // ...and its strength
static double g_retargetAt  = 0.0;     // post-touchdown: when to re-scan for a newer hit
static double g_retargetUntil = 0.0;   // retargeting closes here -- see the window comment
static float  g_impactScore = 0.0f;    // how hard the part he is holding was actually hit
static float  g_headBias    = 1.15f;   // a knock to the head counts for a little more
static float  g_headSteal   = 380.0f;  // ...but STEALING the grab from a part he is already
                                       // holding takes a genuinely hard knock. The head
                                       // whips about and stops constantly during a tumble,
                                       // so a generous bias had it winning most bails
                                       // regardless of what actually hit (field: 3 of 5).
static bool   g_dampLogged  = false;

static double NowS();

static float Frand() { // xorshift32 -> [0,1)
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xffffff) / 16777216.0f;
}
static void QuatRotate(const float* q, float* v) { // v' = q * v (q = xyzw)
    const float cx = q[1]*v[2] - q[2]*v[1], cy = q[2]*v[0] - q[0]*v[2],
                cz = q[0]*v[1] - q[1]*v[0];
    const float dx = q[1]*cz - q[2]*cy, dy = q[2]*cx - q[0]*cz, dz = q[0]*cy - q[1]*cx;
    v[0] += 2.0f * (q[3]*cx + dx); v[1] += 2.0f * (q[3]*cy + dy);
    v[2] += 2.0f * (q[3]*cz + dz);
}
// How hard this body HIT something, in cm/s: deep vertical velocity that has ARRESTED,
// or a horizontal cliff (decayed peak minus current) while now slow. Zero for a body
// that is merely moving fast -- a hit is speed that STOPPED.
// THE BODY FRAME: belly = cross(lateral_right, spine_headward), built LIVE from two
// TORSO-RIGID lines -- the shoulder line and pelvis->chest. Standing, prone, supine or
// mid-tumble it is exact by construction, with no calibration and no session state.
// (Four earlier compasses derived facing from LEG geometry -- knee hinge, toe direction,
// calibrated pelvis axis -- and all four were wrong: a skate stance is asymmetric so
// nothing cancels, and the field measured axes ~70 degrees off, differing per session.)
static void UpdateBellyCompass(int pb) {
    if (pb < 0 || !g_bOk[pb]) return;
    const bool clav = g_clavIdx[0] >= 0 && g_clavIdx[1] >= 0 &&
                      g_bOk[g_clavIdx[0]] && g_bOk[g_clavIdx[1]];
    const int lIdx = clav ? g_clavIdx[0] : g_thighIdx[0];
    const int rIdx = clav ? g_clavIdx[1] : g_thighIdx[1];
    if (lIdx < 0 || rIdx < 0 || !g_bOk[lIdx] || !g_bOk[rIdx]) return;
    float lat[3] = { g_bPos[rIdx][0]-g_bPos[lIdx][0],
                     g_bPos[rIdx][1]-g_bPos[lIdx][1],
                     g_bPos[rIdx][2]-g_bPos[lIdx][2] };
    const float lm = sqrtf(lat[0]*lat[0] + lat[1]*lat[1] + lat[2]*lat[2]);
    if (lm < 5.0f) return;
    lat[0]/=lm; lat[1]/=lm; lat[2]/=lm;
    float top[3];
    if (clav) {
        for (int a = 0; a < 3; a++)
            top[a] = (g_bPos[g_clavIdx[0]][a] + g_bPos[g_clavIdx[1]][a]) * 0.5f;
    } else if (g_headIdx >= 0 && g_bOk[g_headIdx]) {
        for (int a = 0; a < 3; a++) top[a] = g_bPos[g_headIdx][a];
    } else return;
    float sp[3] = { top[0]-g_bPos[pb][0], top[1]-g_bPos[pb][1], top[2]-g_bPos[pb][2] };
    const float d = sp[0]*lat[0] + sp[1]*lat[1] + sp[2]*lat[2];
    sp[0] -= lat[0]*d; sp[1] -= lat[1]*d; sp[2] -= lat[2]*d;   // keep it square
    const float sm = sqrtf(sp[0]*sp[0] + sp[1]*sp[1] + sp[2]*sp[2]);
    if (sm < 8.0f) return;
    sp[0]/=sm; sp[1]/=sm; sp[2]/=sm;
    // right x headward = forward: prone gives belly straight down (bellyZ ~ -1),
    // supine straight up (~ +1).
    const float b[3] = { lat[1]*sp[2] - lat[2]*sp[1],
                         lat[2]*sp[0] - lat[0]*sp[2],
                         lat[0]*sp[1] - lat[1]*sp[0] };
    if (!g_bellyOk) { g_belly[0]=b[0]; g_belly[1]=b[1]; g_belly[2]=b[2]; }
    else {
        for (int a = 0; a < 3; a++) g_belly[a] += (b[a]-g_belly[a]) * 0.35f;
        const float gm = sqrtf(g_belly[0]*g_belly[0] + g_belly[1]*g_belly[1] +
                               g_belly[2]*g_belly[2]);
        if (gm > 0.1f) { g_belly[0]/=gm; g_belly[1]/=gm; g_belly[2]/=gm; }
    }
    g_bellyOk = true;
}

// A rough per-body mass, for making a set of accelerations momentum-neutral. Accel-mode
// forces are mass-independent, so zeroing the MASS-WEIGHTED mean is what actually leaves
// the centre of mass alone: sum(m*(a - mean)) == 0 exactly.
static float BodyMassProxy(int i) {
    if (g_handBody[i])  return 0.5f;
    if (g_footBody[i])  return 1.0f;
    if (g_reacher[i])   return 1.5f;   // forearm
    if (g_armBody[i])   return 2.0f;   // upper arm
    if (g_headBody[i])  return 5.0f;
    if (g_kneeBody[i])  return 8.0f;   // thigh
    if (g_legBody[i])   return 3.5f;   // calf
    if (g_torsoBody[i]) return 8.0f;
    return 2.0f;
}

// Does the rig itself stop a knee bending the wrong way? Read the ragdoll's own joint
// limits once and say so, instead of guessing. The constraints live on the component;
// EAngularConstraintMotion is 0 free, 1 limited, 2 locked -- a knee that reads "free", or
// one with a big symmetric swing limit, can hinge both ways and no force we apply will
// stop it looking broken.
// FBodyInstance::SetLinearVelocity(this, FVector*, bAddToCurrent) -- AddForce + 0x10460 on
// BOTH exes (its own signature has a twin in SetAngularVelocityInRadians, so the delta plus
// a prologue check is the reliable way in). A FORCE can be overpowered by an impact; setting
// the velocity cannot. That is what finally stops a knee crossing the wrong way.
// DRIVING THE JOINTS, which is how every other game does this. A force on a body centre is
// forever negotiating with gravity, friction and the floor -- which is why the curl keeps
// losing to ground collision. A JOINT DRIVE is not a negotiation: the solver is told the
// angle the joint should hold and it holds it. This rig already has the machinery (drives
// present with stiffness 500 / damping 60), it just ships with the POSITION drives switched
// off, so nothing was ever commanding an angle. Turned on for the spine and neck while the
// body is down, and off again the moment it gets up.
typedef void (*SetDriveParamsFn)(void*, float, float, float);
typedef void (*SetOrientTargetFn)(void*, const float*);
typedef void (*SetOrientDriveFn)(void*, bool);
static SetDriveParamsFn  g_setDriveParams = nullptr;
static SetOrientTargetFn g_setOrientTarget = nullptr;
static SetOrientDriveFn  g_setOrientDrive = nullptr;
typedef void (*SetLinVelFn)(void*, const float*, bool);
static SetLinVelFn g_setLinVel = nullptr;
typedef void (*SetSlerpDriveFn)(void*, bool);
static SetSlerpDriveFn g_setSlerp = nullptr;
static bool g_slerpTried = false;
static bool g_drivesFreed = false;
static const char* SIG_SET_SLERP_DRIVE =
    "48 83 EC 38 0F B6 81 7C 01 00 00 02 D2 24 FD 48 89 4C 24 40 0A C2 48 8D 54 24 20 "
    "88 81 7C 01 00 00 48 8D 44 24 40 48 89 44 24 28 48 83 C1 08";

// Command the spine and neck joints to hold a curl. Joints are found by the BONES they
// connect (the constraint names are all "userconstraint_N" and tell us nothing).
static void DriveSpineCurl(uint8_t* mc, bool on) {
    if (!mc || !g_driveCurl) return;
    if (!g_setDriveParams || !g_setOrientTarget || !g_setOrientDrive) return;
    uint8_t** arr = *(uint8_t***)(mc + 0x990);
    const int n = *(int*)(mc + 0x990 + 8);
    if (!arr || n <= 0) return;
    // a quaternion turning g_curlDeg about the chosen local axis
    const float h = g_curlDeg * 0.5f * 3.14159265f / 180.0f;
    const float sn = sinf(h);
    float q[4] = { 0.0f, 0.0f, 0.0f, cosf(h) };
    q[g_curlAxis < 0 || g_curlAxis > 2 ? 0 : g_curlAxis] = sn;
    const float qI[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int hit = 0;
    float lastLim[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // what each group is allowed
    int   lastAx[4]  = { 0, 0, 0, 0 };               // and which axis it ended up using
    for (int i = 0; i < n && i < 40; i++) {
        uint8_t* c = arr[i]; if (!c) continue;
        char b1[64] = "", b2[64] = "";
        GrindPop_FNameToString(c + 0x20, b1, sizeof(b1));
        GrindPop_FNameToString(c + 0x28, b2, sizeof(b2));
        for (char* q2 = b1; *q2; q2++) if (*q2 >= 'A' && *q2 <= 'Z') *q2 += 32;
        for (char* q2 = b2; *q2; q2++) if (*q2 >= 'A' && *q2 <= 'Z') *q2 += 32;
        // EVERY joint the curl needs, not just the spine -- this is the part that makes
        // it an active ragdoll rather than a rag being shoved: the hips bring the knees up
        // and the knees fold the heels in, as commanded ANGLES the solver enforces, so
        // ground contact cannot argue with them and the curl levers the body up instead of
        // being flattened by the floor.
        // A JOINT IS NAMED BY BOTH BONES IT JOINS, so the tests have to run from the far
        // end of the limb inwards or each group steals the next one's joint. The knee is
        // the calf-to-THIGH joint -- it has the word thigh in it -- so a thigh test placed
        // first claimed the knee, and the calf test was then left with the only joint that
        // still mentioned a calf: the ANKLE. For several builds "hip" was driving the knee
        // (which is why knee flexion appeared at all) and "knee" was driving the ankle,
        // while the real hip only ever got the hip number second-hand. Ankle first, then
        // knee, then hip: now each group drives exactly the joint it is named for.
        const bool ankleJ = strstr(b1, "foot") || strstr(b2, "foot") ||
                            strstr(b1, "ball") || strstr(b2, "ball");
        const bool calfJ  = strstr(b1, "calf")  || strstr(b2, "calf");
        const bool thighJ = strstr(b1, "thigh") || strstr(b2, "thigh");
        float deg = 0.0f; int ax = 0;
        if (strstr(b1, "spine") || strstr(b2, "spine") ||
            strstr(b1, "neck")  || strstr(b2, "neck")  ||
            strstr(b1, "head")  || strstr(b2, "head")) { deg = g_curlDeg; ax = g_curlAxis; }
        else if (ankleJ) { deg = 0.0f; ax = 0; }   // the ankle only holds tone
        else if (calfJ)  { deg = g_kneeDeg; ax = g_kneeAxis; }
        else if (thighJ) { deg = g_hipDeg;  ax = g_hipAxis;  }
        // he has given up turning over: the whole leg goes slack, tone included, so it can
        // straighten out instead of staying tucked underneath him
        if (on && (g_legsLimp || g_legsFlail) && (ankleJ || calfJ || thighJ)) {
            g_setOrientTarget(c, qI);
            g_setDriveParams(c, *(float*)(c + 0x8c + 0xc4 + 0x20),
                                *(float*)(c + 0x8c + 0xc4 + 0x24), 0.0f);
            g_setOrientDrive(c, false);
            continue;
        }
        else if (g_jointTone) { deg = 0.0f; ax = 0; }   // everything else: tone, no bias
        else continue;
        const bool armJoint = strstr(b1, "arm")  || strstr(b2, "arm")  ||
                              strstr(b1, "hand") || strstr(b2, "hand") ||
                              strstr(b1, "clavicle") || strstr(b2, "clavicle");
        // THE REACHING JOINTS ARE NEVER DRIVEN. Elbows and wrists are what actually carry a
        // hand to the hurt part, and a driven joint is a servo the grab's forces cannot
        // argue with -- driving them is why the hands stopped visibly going to the bone.
        if (deg == 0.0f && (strstr(b1, "hand") || strstr(b2, "hand") ||
                            strstr(b1, "lowerarm") || strstr(b2, "lowerarm") ||
                            strstr(b1, "forearm")  || strstr(b2, "forearm"))) continue;
        if (on) {
            // NEVER COMMAND AN ANGLE THE JOINT CANNOT REACH. The knee drive was asking for
            // 70 degrees from a joint whose own limit is 60: the drive pushes, the limit
            // pushes back, and the joint buzzes -- which is what the flailing feet were
            // (field, with the flail already turned down). Keep the target inside the
            // rig's own swing limit.
            // CLAMP AGAINST THE RIGHT AXIS. Every target was being measured against the
            // swing1 limit whichever axis it turned about -- and axis 0 is the TWIST axis,
            // which on these joints is limited to about 20 degrees. So a big angle on the
            // default axis was asking for rotation about the one axis that is nearly
            // locked, and nothing visible happened however large the number (field: -280
            // looked identical to 28). X is twist, Y is swing2, Z is swing1.
            const float limT = *(float*)(c + 0x8c + 0x5c + 0x14);   // twist  (X)
            const float lim2 = *(float*)(c + 0x8c + 0x3c + 0x18);   // swing2 (Y)
            const float lim1 = *(float*)(c + 0x8c + 0x3c + 0x14);   // swing1 (Z)
            float lim = (ax == 0) ? limT : (ax == 1) ? lim2 : lim1;
            // AN AXIS THAT CANNOT BEND IS NOT A CHOICE. These hips and knees allow only
            // 5 and 10 degrees of twist, so asking for a curl about the twist axis moves
            // nothing however large the angle -- and the joint that has to bend for a tuck
            // is exactly the one with the big range. If the chosen axis has no room, the
            // one that does is used instead.
            if (lim < 15.0f) {
                if (lim1 >= lim2 && lim1 >= limT)      { ax = 2; lim = lim1; }
                else if (lim2 >= limT)                 { ax = 1; lim = lim2; }
                else                                   { ax = 0; lim = limT; }
            }
            if (lim > 1.0f) {
                const float room = lim * 0.8f;
                if (deg >  room) deg =  room;
                if (deg < -room) deg = -room;
                const int grp = ankleJ ? 3 : calfJ ? 2 : thighJ ? 1 : 0;
                lastLim[grp] = room; lastAx[grp] = ax;
            }
            const float h2 = deg * 0.5f * 3.14159265f / 180.0f;
            float qq[4] = { 0.0f, 0.0f, 0.0f, cosf(h2) };
            qq[(ax < 0 || ax > 2) ? 0 : ax] = sinf(h2);
            const bool toned = (deg == 0.0f);
            const float st = toned ? (armJoint ? g_toneArmStiff : g_toneStiff)
                                   : g_curlStiff;
            g_setDriveParams(c, st, toned ? g_toneDamp : g_curlDamp, 0.0f);
            g_setOrientTarget(c, qq);
        } else {
            // hand the joint back exactly as the rig had it: our stiffness and damping
            // were overwriting the game's own values and never being put back
            g_setOrientTarget(c, qI);
            g_setDriveParams(c, *(float*)(c + 0x8c + 0xc4 + 0x20),
                                *(float*)(c + 0x8c + 0xc4 + 0x24), 0.0f);
        }
        g_setOrientDrive(c, on);
        hit++;
    }
    if (on) TwkLogBail("[body] curl driven on %d joint(s): spine %.0f on ax%d (max %.0f), "
                   "hip %.0f on ax%d (max %.0f), knee %.0f on ax%d (max %.0f) -- ax is the "
                   "axis actually used and max is the joint's own room",
                   hit, g_curlDeg, lastAx[0], lastLim[0], g_hipDeg, lastAx[1], lastLim[1],
                   g_kneeDeg, lastAx[2], lastLim[2]);
}

static void SetJointDrives(uint8_t* mc, bool on) {
    if (!mc || !g_freeJoints) return;
    if (!g_slerpTried) {
        g_slerpTried = true;
        g_setSlerp = (SetSlerpDriveFn)TwkScanExe(SIG_SET_SLERP_DRIVE);
        if (g_setSlerp) {
            // all three live beside the velocity switch, at fixed offsets on both exes;
            // each prologue is checked before it is trusted
            uint8_t* base = (uint8_t*)g_setSlerp;
            static const uint8_t kDp[] = { 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8D, 0x44, 0x24 };
            static const uint8_t kOt[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };
            static const uint8_t kOd[] = { 0x48, 0x83, 0xEC, 0x38, 0x0F, 0xB6, 0x81, 0x7C };
            uint8_t* dp = base - 0x100; uint8_t* ot = base - 0x80; uint8_t* od2 = base + 0x3a0;
            bool okd = true, oko = true, okv = true;
            for (int i = 0; i < 8; i++) {
                if (dp[i]  != kDp[i]) okd = false;
                if (ot[i]  != kOt[i]) oko = false;
                if (od2[i] != kOd[i]) okv = false;
            }
            g_setDriveParams  = okd ? (SetDriveParamsFn)dp   : nullptr;
            g_setOrientTarget = oko ? (SetOrientTargetFn)ot  : nullptr;
            g_setOrientDrive  = okv ? (SetOrientDriveFn)od2  : nullptr;
            TwkLog("[body] joint drive API: params %d target %d enable %d",
                   okd ? 1 : 0, oko ? 1 : 0, okv ? 1 : 0);
        }
        TwkLog("[body] joint drive switch %s",
               g_setSlerp ? "resolved -- ragdolls get free joints"
                          : "SIG NOT FOUND -- the game's joint damping stays on");
    }
    if (!g_setSlerp) return;
    uint8_t** arr = *(uint8_t***)(mc + 0x990);
    const int n = *(int*)(mc + 0x990 + 8);
    if (!arr || n <= 0) return;
    for (int i = 0; i < n && i < 40; i++)
        if (arr[i]) g_setSlerp(arr[i], on);
}

static bool g_jointsLogged = false;
static void LogJointLimits(uint8_t* mc) {
    if (g_jointsLogged || !mc || !g_log) return;
    g_jointsLogged = true;
    uint8_t** arr = *(uint8_t***)(mc + 0x990);
    const int n = *(int*)(mc + 0x990 + 8);
    TwkLog("[body] ragdoll joints: %d constraint(s)", n);
    if (!arr || n <= 0) return;
    int shown = 0;
    for (int i = 0; i < n && i < 40; i++) {
        uint8_t* c = arr[i]; if (!c) continue;
        char nm[64] = "?", n1[64] = "?", n2[64] = "?";
        GrindPop_FNameToString(c + 0x18, nm, sizeof(nm));
        // the joint names are all userconstraint_N and say nothing -- the BONES it joins
        // are the only way to tell a knee from a hip
        GrindPop_FNameToString(c + 0x20, n1, sizeof(n1));
        GrindPop_FNameToString(c + 0x28, n2, sizeof(n2));
        for (char* q = nm; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        uint8_t* pr = c + 0x8c;
        // ...and whether the joint is MOTORISED. An angular drive with stiffness and
        // position-drive enabled is the game actively holding the bone at a target
        // orientation -- which would fight every force we apply.
        uint8_t* ad = pr + 0xc4;
        TwkLog("[body] joint %s [%s <- %s]: swing1 %.0f (%d), swing2 %.0f (%d), "
               "twist %.0f (%d) | drive twist %.0f/%.0f swing %.0f/%.0f slerp %.0f/%.0f "
               "flags %d/%d/%d",
               nm, n1, n2, *(float*)(pr + 0x3c + 0x14), (int)*(pr + 0x3c + 0x1c),
               *(float*)(pr + 0x3c + 0x18), (int)*(pr + 0x3c + 0x1d),
               *(float*)(pr + 0x5c + 0x14), (int)*(pr + 0x5c + 0x18),
               *(float*)(ad + 0x00), *(float*)(ad + 0x04),
               *(float*)(ad + 0x10), *(float*)(ad + 0x14),
               *(float*)(ad + 0x20), *(float*)(ad + 0x24),
               (int)*(ad + 0x0c), (int)*(ad + 0x1c), (int)*(ad + 0x2c));
        if (++shown >= 20) break;
    }
}

static float g_groundZnow = 0.0f;     // lowest body this frame -- our stand-in for the floor
static float g_hitGroundCm = 34.0f;   // an impact has to happen NEAR it

static float SlamScore(int i) {
    // IT ONLY COUNTS IF IT HIT THE GROUND. This measures a body whose velocity arrested,
    // and a limb stopped by its OWN JOINT decelerates exactly like one stopped by the
    // floor -- a head whipping to the end of its neck reads as a head slam with the ground
    // nowhere near it (field: "he never hit the ground and still grabbed his head").
    if (g_bPos[i][2] - g_groundZnow > g_hitGroundCm) return 0.0f;
    float sc = 0.0f;
    if (g_bVel[i][2] > -80.0f && -g_bMinVz[i] > sc) sc = -g_bMinVz[i];
    const float hs = sqrtf(g_bVel[i][0]*g_bVel[i][0] + g_bVel[i][1]*g_bVel[i][1]);
    const float cliff = g_bPkH[i] - hs;
    if (hs < 150.0f && cliff > sc) sc = cliff;
    return sc;
}

static void BodyBoneName(uint8_t* mc, uint8_t* arr2, int i, char* out, int cap) {
    out[0] = '?'; if (cap > 1) out[1] = 0;
    void* skm = *(void**)(mc + CMP_SKELMESH);
    uint8_t* info = skm ? *(uint8_t**)((uint8_t*)skm + SKM_REFSKEL_INFO) : nullptr;
    const int nb = skm ? *(int*)((uint8_t*)skm + SKM_REFSKEL_INFO + 8) : 0;
    uint8_t* bi = ((uint8_t**)arr2)[i];
    const int bx = bi ? *(short*)(bi + BI_BONE_INDEX) : -1;
    if (info && bx >= 0 && bx < nb)
        GrindPop_FNameToString(info + (size_t)bx * BONEINFO_STRIDE, out, cap);
}
typedef void (*AddForceFn)(void*, const float*, bool, bool);
static AddForceFn g_addForce = nullptr;
static AddForceFn g_addTorque = nullptr;   // FBodyInstance::AddTorqueInRadians -- same
                                           // shape; accelChange = angular accel rad/s^2
static bool   g_addForceTried = false;
// FBodyInstance::AddForce(this, FVector*, bAllowSubstepping, bAccelChange). Two prologue
// twins exist; the FIRST match is AddForce on both exes (verified against the Epic PDB).
// FConstraintInstance::SetAngularVelocityDriveSLERP(bool). The ragdoll's joints ship with
// a SLERP VELOCITY DRIVE enabled (stiffness 500, damping 60, read out of the rig itself) --
// a servo damping every joint's rotation. That is the game "constraining the bones", and it
// is why our forces struggle to fold a joint the right way while a hard impact still
// overpowers it and bends the knee backwards. Freed for the duration of the ragdoll and
// restored the moment it ends.

static const char* SIG_ADDFORCE =
    "4C 8B DC 45 88 4B 20 45 88 43 18 48 83 EC 58 49 8D 43 18 49 89 4B D8 49 89 43 E8 "
    "48 81 C1 ?? ?? 00 00 49 8D 43 20 49 89 53 E0";

static double NowS() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static void ResolveAddForce() {
    if (g_addForceTried) return;
    g_addForceTried = true;
    g_addForce = (AddForceFn)TwkScanExe(SIG_ADDFORCE);
    TwkLog("[body] FBodyInstance::AddForce %s",
           g_addForce ? "resolved" : "SIG NOT FOUND -- brace + arm reflexes inert");
    if (g_addForce) {
        static const uint8_t kLv[] = { 0x4C, 0x8B, 0xDC, 0x45, 0x88, 0x43, 0x18,
                                       0x48, 0x83, 0xEC, 0x48 };
        uint8_t* lv = (uint8_t*)g_addForce + 0x10460;
        bool lok = true;
        for (int i = 0; i < (int)sizeof(kLv); i++)
            if (lv[i] != kLv[i]) { lok = false; break; }
        g_setLinVel = lok ? (SetLinVelFn)lv : nullptr;
        TwkLog("[body] FBodyInstance::SetLinearVelocity %s", lok ?
               "resolved -- knees are held on the right side of straight" :
               "MISMATCH -- the knee clamp is off");
    }
    // AddTorqueInRadians sits at AddForce + 0x2f0 on BOTH exes (PDB-verified on Epic,
    // same delta on Steam; the two share their whole thunk prologue). The prologue is
    // byte-verified before trust -- a build that moves it just loses torques, safely.
    if (g_addForce) {
        static const uint8_t kPro[] = { 0x4C, 0x8B, 0xDC, 0x45, 0x88, 0x4B, 0x20,
                                        0x45, 0x88, 0x43, 0x18, 0x48, 0x83, 0xEC, 0x58 };
        uint8_t* cand = (uint8_t*)g_addForce + 0x2f0;
        bool ok = true;
        for (int i = 0; i < (int)sizeof(kPro); i++)
            if (cand[i] != kPro[i]) { ok = false; break; }
        g_addTorque = ok ? (AddForceFn)cand : nullptr;
        TwkLog("[body] FBodyInstance::AddTorqueInRadians %s",
               ok ? "resolved (AddForce + 0x2f0, prologue verified)"
                  : "MISMATCH -- joint torques off, force behaviors only");
    }
}

static void ForgetMesh() {
    g_mesh = nullptr; g_nBodies = 0; g_feel = 1.0f; g_pulse = 0.0f;
    // A new character on a new map deserves the benefit of the doubt.
    if (g_fault) { g_fault = 0; TwkLog("[body] new character -- reactive body armed again"); }
}

// Restore every body we steered to its authored weight. Guarded; shared by every stand-down.
static void RestoreAll() {
    if (!g_mesh || !g_nBodies) { ForgetMesh(); return; }
    __try {
        uint8_t* arr = *(uint8_t**)((uint8_t*)g_mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)g_mesh + CMP_BODIES + 8);
        if (arr && n == g_nBodies) {
            for (int i = 0; i < n && i < kMaxBodies; i++) {
                if (!g_owned[i]) continue;
                uint8_t* bi = ((uint8_t**)arr)[i];
                if (bi) *(float*)(bi + BI_BLEND_WEIGHT) = g_authored[i];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ForgetMesh();
}

void BodyFeel_PumpFrame() {
    void* sk = CatchTweaks_Skater();
    void* an = FootPlace_AnimInstance();
    if (!sk || !an) { if (g_mesh) RestoreAll(); return; }
    const double t = NowS();
    float dt = (g_lastT > 0.0) ? (float)(t - g_lastT) : 0.0f;
    if (dt < 0.0f) dt = 0.0f; else if (dt > 0.1f) dt = 0.1f;
    g_lastT = t;

    __try {
        // Track the skater's velocity every tick regardless of state: the brace needs the THROW
        // vector from the instant the ragdoll begins, and mid-ragdoll positions are not trusted
        // (the capsule and the simulated mesh part ways). Smoothed lightly.
        {
            void* root = *(void**)((uint8_t*)sk + SK_ROOT);
            if (root) {
                const float* pos = (const float*)((uint8_t*)root + CTW_TRANSLATION);
                if (g_posValid && dt > 0.001f) {
                    for (int a = 0; a < 3; a++) {
                        const float v = (pos[a] - g_pos[a]) / dt;
                        g_vel[a] += (v - g_vel[a]) * 0.35f;
                    }
                }
                const float* rq = (const float*)((uint8_t*)root + CTW_QUAT);
                const float yaw = atan2f(2.0f * (rq[3]*rq[2] + rq[0]*rq[1]),
                                         1.0f - 2.0f * (rq[1]*rq[1] + rq[2]*rq[2]));
                if (g_posValid && dt > 0.001f) {
                    float dy = yaw - g_lastYaw;
                    while (dy >  3.14159f) dy -= 6.28318f;
                    while (dy < -3.14159f) dy += 6.28318f;
                    g_yawRate += (dy / dt - g_yawRate) * 0.3f;
                }
                g_lastYaw = yaw;
                g_pos[0] = pos[0]; g_pos[1] = pos[1]; g_pos[2] = pos[2];
                g_posValid = true;
            } else g_posValid = false;
        }
        const bool physOn  = *((uint8_t*)sk + SK_PHYSANIM_ON) != 0;
        const bool ragdoll = (*((uint8_t*)sk + SK_STATE_BITS) & 0x02) != 0;
        if (ragdoll) {
            // ---- THE BAIL BRACE: the skater reaches to protect the fall --------------------
            // At the ragdoll edge the throw vector is captured from the tracked velocity; for
            // g_bailMs the hand/forearm bodies are pulled along it (blending toward straight
            // down as gravity takes over), so the arms lead the fall instead of trailing limp.
            // Acceleration-change forces (mass-independent), through the engine's own physics
            // command path. Everything else in the module stays hands-off during the ragdoll.
            if (!g_wasRagdoll) {
                g_wasRagdoll = true; g_braceStart = t;
                float m = sqrtf(g_vel[0]*g_vel[0] + g_vel[1]*g_vel[1] + g_vel[2]*g_vel[2]);
                if (m > 100.0f) { for (int a = 0; a < 3; a++) g_throw[a] = g_vel[a] / m; }
                else { g_throw[0] = 0; g_throw[1] = 0; g_throw[2] = -1.0f; }
                ResolveAddForce();
                if (g_bail && g_on && !g_fault && g_nReachers && g_addForce)
                    TwkLogBail("[body] brace: reaching along (%.2f, %.2f, %.2f) with %d bodies",
                           g_throw[0], g_throw[1], g_throw[2], g_nReachers);
                g_throwVel[0] = g_vel[0]; g_throwVel[1] = g_vel[1];
                g_throwVel[2] = g_vel[2]; g_throwSpeed = m;
                g_rng = (uint32_t)(t * 1000.0) | 1u;
                g_oscHz = 0.55f + Frand() * 0.30f; // each bail rocks at its own pace
                g_legStraight[0] = g_legStraight[1] = 0.0;
                for (int i = 0; i < kMaxBodies; i++) g_bounceUntil[i] = 0.0;
                g_impactBody = -1; g_sawFall = false; g_bPosValid = false; g_landT = 0.0;
                g_crashBody = -1; g_crashScore = 0.0f;
                g_carryLogged = false; g_retargetAt = 0.0; g_restNext = 3.0;
                g_legLogged = false;
                g_blendLogged = false;
                g_bellyOk = false;
                g_faceDownSince = 0.0; g_rollUntil = 0.0;
                LogJointLimits((uint8_t*)g_mesh);
                TwkLogBail("[body] at bail: physical animation flag = %d (1 means the game is "
                       "still driving bones toward the animated pose)",
                       (int)*((uint8_t*)sk + SK_PHYSANIM_ON));
                if (!g_drivesFreed) {
                    SetJointDrives((uint8_t*)g_mesh, false);
                    g_drivesFreed = true;
                }
                DriveSpineCurl((uint8_t*)g_mesh, true);
                g_rollDone = false; g_rollTries = 0; g_legsLimp = false;
                // the legs pedal first; the curl takes them over when the flail is spent
                g_legsFlail = (g_bail && g_amount > 0 && g_flailMs > 0 &&
                               g_flailAccel > 0.0f);
                g_detectStart = t;
                g_armBroken[0] = g_armBroken[1] = false; g_retargetUntil = 0.0;
                for (int i = 0; i < kMaxBodies; i++) g_bOk[i] = false;
            }
            const double el = t - g_braceStart;
            if (g_bail && g_on && !g_fault && g_amount > 0 && g_addForce && g_nReachers &&
                g_mesh && el < 30.0) {
                uint8_t* arr2 = *(uint8_t**)((uint8_t*)g_mesh + CMP_BODIES);
                const int n2 = *(int*)((uint8_t*)g_mesh + CMP_BODIES + 8);
                if (arr2 && n2 == g_nBodies) {
                    const float amt = (float)g_amount / 100.0f;
                    if (!g_dampLogged) {
                        g_dampLogged = true;
                        uint8_t* b0 = ((uint8_t**)arr2)[0];
                        if (b0) TwkLog("[body] ragdoll damping: linear %.2f angular %.2f",
                                       *(float*)(b0 + BI_LIN_DAMP),
                                       *(float*)(b0 + BI_LIN_DAMP + 4));
                    }
                    // ---- live body world positions: component-space bone transforms (the
                    // physics-driven pose the mesh renders) through the mesh's own
                    // ComponentToWorld. This is what the old timers could never give: the
                    // landing moment, the part that hit, and where each hand is relative
                    // to it.
                    uint8_t* mc = (uint8_t*)g_mesh;
                    int ri = *(int*)(mc + CMP_CST_READIDX); if (ri != 1) ri = 0;
                    uint8_t* cst = *(uint8_t**)(mc + CMP_CST_ARR + (size_t)ri * 0x10);
                    const int cstN = *(int*)(mc + CMP_CST_ARR + (size_t)ri * 0x10 + 8);
                    const float* mq  = (const float*)(mc + CTW_QUAT);
                    const float* mp  = mq + 4;  // FTransform: quat, translation, scale
                    const float* msc = mq + 8;
                    float meanVz = 0.0f; int nV = 0;
                    if (cst && cstN > 0) {
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            uint8_t* bi = ((uint8_t**)arr2)[i];
                            const bool was = g_bOk[i]; g_bOk[i] = false;
                            if (!bi) continue;
                            const int bx = *(short*)(bi + BI_BONE_INDEX);
                            if (bx < 0 || bx >= cstN) continue;
                            const float* bp = (const float*)(cst + (size_t)bx * 48 + 0x10);
                            float w[3] = { bp[0]*msc[0], bp[1]*msc[1], bp[2]*msc[2] };
                            QuatRotate(mq, w);
                            w[0] += mp[0]; w[1] += mp[1]; w[2] += mp[2];
                            if (was && g_bPosValid && dt > 0.001f) {
                                for (int a = 0; a < 3; a++) {
                                    const float bv = (w[a] - g_bPos[i][a]) / dt;
                                    g_bVel[i][a] += (bv - g_bVel[i][a]) * 0.45f;
                                }
                                meanVz += g_bVel[i][2]; nV++;
                                float mz = g_bMinVz[i] + 500.0f * dt;
                                if (mz > 0.0f) mz = 0.0f;
                                g_bMinVz[i] = (g_bVel[i][2] < mz) ? g_bVel[i][2] : mz;
                                // Peak decays slower than sliding friction bleeds speed,
                                // so only an abrupt arrest opens a gap under it.
                                const float hs = sqrtf(g_bVel[i][0]*g_bVel[i][0] +
                                                       g_bVel[i][1]*g_bVel[i][1]);
                                const float pk = g_bPkH[i] - 400.0f * dt;
                                g_bPkH[i] = (hs > pk) ? hs : pk;
                                // BOUNCE: this body was coming down hard and has just
                                // stopped -- kick it back up a little. Fires once per
                                // slam (the record is cleared), so a body resting on the
                                // floor never buzzes.
                                if (g_bMinVz[i] < -250.0f && g_bVel[i][2] > -60.0f &&
                                    g_bounceGain > 0.0f) {
                                    float bk = -g_bMinVz[i] * g_bounceGain;
                                    if (bk > g_bounceCap) bk = g_bounceCap;
                                    g_bounceAcc[i] = bk;
                                    g_bounceUntil[i] = t + (double)g_bounceMs / 1000.0;
                                    g_bMinVz[i] = 0.0f;
                                }
                                // THE BREAK: an arm slammed hard enough goes limp for
                                // this ragdoll -- no reaching, no keeper, it just hangs
                                // (field ask: a joint "totally unhinged if it lands at a
                                // bad angle at a certain force").
                                // A break is a SLAM, not motion: the deep velocity must
                                // have ARRESTED (first cut had no arrest condition --
                                // free fall and ordinary deceleration broke both arms
                                // 0.2s into every bail, killing the grab and windmill).
                                if (g_breakOn && g_armBody[i] && el > 0.2 &&
                                    ((g_bMinVz[i] < g_breakVz &&
                                      g_bVel[i][2] > -80.0f) ||
                                     (g_bPkH[i] - hs > g_breakCliff &&
                                      hs < 150.0f))) {
                                    const int bsd = g_leftSide[i] ? 0 : 1;
                                    if (!g_armBroken[bsd]) {
                                        g_armBroken[bsd] = true;
                                        TwkLogBail("[body] %s arm took the hit -- hanging "
                                               "limp", bsd == 0 ? "left" : "right");
                                    }
                                }
                            } else {
                                // First sight this ragdoll: assume the body carries the
                                // pre-bail velocity. Seeding from zero was carry v1's
                                // fling -- a fake deficit until the smoothing caught up.
                                g_bVel[i][0] = g_throwVel[0]; g_bVel[i][1] = g_throwVel[1];
                                g_bVel[i][2] = g_throwVel[2];
                                g_bMinVz[i] = 0.0f; g_bPkH[i] = 0.0f;
                            }
                            g_bPos[i][0] = w[0]; g_bPos[i][1] = w[1]; g_bPos[i][2] = w[2];
                            g_bOk[i] = true;
                        }
                        g_bPosValid = true;
                        g_groundZnow = 1e30f;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++)
                            if (g_bOk[i] && g_bPos[i][2] < g_groundZnow)
                                g_groundZnow = g_bPos[i][2];
                    }
                    const bool velOk = nV > 0;
                    if (velOk) meanVz /= (float)nV;
                    // ---- MOMENTUM CARRY v3 (along-throw, accelerate-only) --------------
                    if (velOk && el < (double)g_carryMs / 1000.0 && g_throwSpeed > 250.0f) {
                        const float tproj = g_throwSpeed * g_carryFrac;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_bOk[i]) continue;
                            uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                            const float proj = g_bVel[i][0]*g_throw[0] +
                                               g_bVel[i][1]*g_throw[1] +
                                               g_bVel[i][2]*g_throw[2];
                            if (proj >= tproj) continue;
                            float a2 = (tproj - proj) * g_carryGain;
                            if (a2 > g_carryMax) a2 = g_carryMax;
                            const float d[3] = { g_throw[0]*a2, g_throw[1]*a2,
                                                 g_throw[2]*a2 };
                            g_addForce(bi, d, true, true);
                        }
                    }
                    if (!g_carryLogged && velOk && el > 0.1) {
                        g_carryLogged = true;
                        float ms2 = 0.0f; int nm2 = 0;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_bOk[i]) continue;
                            ms2 += sqrtf(g_bVel[i][0]*g_bVel[i][0] +
                                         g_bVel[i][1]*g_bVel[i][1] +
                                         g_bVel[i][2]*g_bVel[i][2]);
                            nm2++;
                        }
                        if (nm2) TwkLogBail("[body] carry: bodies at %.0f cm/s vs pre-bail %.0f",
                                        ms2 / (float)nm2, g_throwSpeed);
                    }
                    // THE FLOAT, SOLVED (.107 telemetry): during long ragdolls the
                    // per-body PhysicsBlendWeight sits STUCK below 1 (0.75 for 24 straight
                    // seconds on the field's floating bail; nobody re-authors weights
                    // mid-ragdoll), so the rendered mesh is a blend of the bodies lying ON
                    // the real ground and the animation pose anchored at the ACTOR, back
                    // at deck height -- over low terrain the blend lifts the render into
                    // mid-air. There is no surface; the float IS the blend. Ramp to full
                    // physics once the ragdoll is committed; ragdoll exit already forgets
                    // the mesh so the game re-authors weights for the get-up untouched.
                    // 0.25s grace clears the game's own blend-IN to ragdoll, nothing more
                    // -- the 1s wait of the first cut was most of the residual float the
                    // field still saw (the stuck weight exists from the first frames).
                    if (el > 0.25) {
                        const float wTgt = 1.0f;
                        float before = 0.0f, raised = 0.0f; int nrw = 0;
                        const float step = 4.0f * dt;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            uint8_t* bw = ((uint8_t**)arr2)[i]; if (!bw) continue;
                            float* w = (float*)(bw + BI_BLEND_WEIGHT);
                            before += *w; nrw++;
                            if (*w < wTgt) {
                                float w2 = *w + step; if (w2 > wTgt) w2 = wTgt;
                                raised += w2 - *w; *w = w2;
                            } else if (*w > wTgt) {
                                float w2 = *w - step; if (w2 < wTgt) w2 = wTgt;
                                *w = w2;
                            }
                        }
                        if (!g_blendLogged && nrw && raised > 0.001f &&
                            before / (float)nrw < 0.9f) {
                            g_blendLogged = true;
                            TwkLogBail("[body] ragdoll blend was %.2f -- raising to full "
                                   "physics (the float fix)", before / (float)nrw);
                        }
                    }
                    // ---- THE FLAIL, air-started (field ask: "start it in the air
                    // after a certain amount of time, not at collision"): the legs
                    // pedal from g_flailDelayMs after the bail EDGE -- panic-kicking on
                    // the way down -- riding through the landing and fading out over
                    // g_flailMs. Same proven pedal; only the clock moved.
                    {
                        // Full-strength kicking from the delay until TOUCHDOWN; the fade
                        // timer only starts at the landing, so there is always Flail time
                        // worth of kicking after the slam (field: a long fall spent the
                        // whole fixed window in the air and landed with dead legs).
                        const double elF = el - (double)g_flailDelayMs / 1000.0;
                        double fade = 0.0;
                        if (g_impactBody >= 0)
                            fade = (t - g_landT) / ((double)g_flailMs / 1000.0);
                        if (elF > 0.0 && fade < 1.0 &&
                            g_pelvisBody >= 0 && g_bOk[g_pelvisBody] &&
                            g_headIdx >= 0 && g_bOk[g_headIdx]) {
                            const float envF = 1.0f - (float)fade;
                            const float* pp2 = g_bPos[g_pelvisBody];
                            const float ch2[3] = {
                                pp2[0] + (g_bPos[g_headIdx][0]-pp2[0]) * 0.65f,
                                pp2[1] + (g_bPos[g_headIdx][1]-pp2[1]) * 0.65f,
                                pp2[2] + (g_bPos[g_headIdx][2]-pp2[2]) * 0.65f };
                            const float ph2 = sinf((float)(elF * 6.28318 *
                                                    (double)g_flailHz));
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_legBody[i] || !g_bOk[i]) continue;
                                // The FEET never seek. They were being pulled toward the
                                // chest at full strength like the rest of the leg, which
                                // during the grab window reads as the feet trying to reach
                                // the sore spot as well (field). A foot follows its shin.
                                if (g_footBody[i]) continue;
                                uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                const float gsel = g_leftSide[i] ? ph2 : -ph2;
                                const float oscF2 = (gsel >= 0.0f) ? gsel : 0.8f * gsel;
                                const float acc2 = (g_kneeBody[i] ? g_flailAccel * 0.65f
                                                                  : g_flailAccel) *
                                                   amt * envF * oscF2;
                                float d2[3] = { ch2[0]-g_bPos[i][0],
                                                ch2[1]-g_bPos[i][1],
                                                ch2[2]-g_bPos[i][2] };
                                const float dl2 = sqrtf(d2[0]*d2[0] + d2[1]*d2[1] +
                                                        d2[2]*d2[2]);
                                if (dl2 < 2.0f) continue;
                                const float sc2 = acc2 / dl2;
                                float f4[3] = { d2[0]*sc2, d2[1]*sc2, d2[2]*sc2 };
                                if (f4[2] < 0.0f) f4[2] *= 0.35f;  // never into the ground
                                g_addForce(bi, f4, true, true);
                            }
                        }
                    }
                    // The pedal is spent: the legs stop being free and take up the tuck.
                    if (g_legsFlail && g_mesh && g_impactBody >= 0 &&
                        (t - g_landT) >= (double)g_flailMs / 1000.0) {
                        g_legsFlail = false;
                        DriveSpineCurl((uint8_t*)g_mesh, true);
                        TwkLogBail("[body] flail spent -- the legs take up the tuck");
                    }
                    // Invisible-surface telemetry, every 3s of ragdoll. The discriminator
                    // is motion: a body resting on real (even invisible) geometry keeps
                    // simulating -- tiny contact jitter, nonzero speed -- while a body the
                    // game is FREEZING or DRIVING for bail-recovery reads exactly zero,
                    // and if the ragdoll bit drops entirely these lines simply stop while
                    // the float is still visible. Blend weight shows a recovery blend-out.
                    if (el > g_restNext && g_posValid) {
                        g_restNext = el + 3.0;
                        float lowZ2 = 1e30f; float mx = 0, my = 0; int nm3 = 0;
                        float spd = 0.0f, wsum = 0.0f;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_bOk[i]) continue;
                            if (g_bPos[i][2] < lowZ2) lowZ2 = g_bPos[i][2];
                            mx += g_bPos[i][0]; my += g_bPos[i][1];
                            spd += sqrtf(g_bVel[i][0]*g_bVel[i][0] +
                                         g_bVel[i][1]*g_bVel[i][1] +
                                         g_bVel[i][2]*g_bVel[i][2]);
                            uint8_t* bw = ((uint8_t**)arr2)[i];
                            if (bw) wsum += *(float*)(bw + BI_BLEND_WEIGHT);
                            nm3++;
                        }
                        if (nm3) {
                            mx /= (float)nm3; my /= (float)nm3;
                            const float hd = sqrtf((mx - g_pos[0]) * (mx - g_pos[0]) +
                                                   (my - g_pos[1]) * (my - g_pos[1]));
                            TwkLogBail("[body] rest t=%.1fs: lowest %+.0f vs rootZ, %.0f cm "
                                   "from capsule, mean speed %.0f cm/s, blendW %.2f, "
                                   "bellyZ %+.2f%s",
                                   el, lowZ2 - g_pos[2], hd, spd / (float)nm3,
                                   wsum / (float)nm3,
                                   g_bellyOk ? g_belly[2] : 0.0f,
                                   g_bellyOk ? "" : " (blind)");
                        }
                    }
                    // apply any live bounce kicks
                    if (g_bounceGain > 0.0f) {
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_bOk[i] || g_bounceUntil[i] <= t) continue;
                            uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                            const float bf[3] = { 0.0f, 0.0f, g_bounceAcc[i] * amt };
                            g_addForce(bi, bf, true, true);
                        }
                    }
                    // ---- TOUCHDOWN: the fall arrests (mean body z-velocity was fast-down,
                    // now near zero) -> the lowest non-hand body is what hit. Fallbacks
                    // cover a bail that starts already on the ground.
                    const double elD = t - g_detectStart;
                    // ---- THE CRASH ITSELF: what he hit while still riding -------------
                    // Measured against the speed he carried into the bail: the bone jammed
                    // against whatever he hit stops dead while the rest of him keeps going,
                    // so it is the one trailing the others along his direction of travel.
                    // Comparing each bone to its PEERS rather than to a fixed number is
                    // what keeps an ordinary fall (where everything slows together) from
                    // reading as a crash.
                    if (g_impactBody < 0 && g_crashHit && velOk &&
                        g_throwSpeed > g_crashMinSpeed && elD >= 0.03) {
                        if (elD <= g_crashWindow) {
                            float mean = 0.0f; int nA = 0;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_bOk[i] || g_noImpact[i]) continue;
                                mean += g_bVel[i][0]*g_throw[0] + g_bVel[i][1]*g_throw[1] +
                                        g_bVel[i][2]*g_throw[2];
                                nA++;
                            }
                            if (nA >= 4) {
                                mean /= (float)nA;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    if (!g_bOk[i] || g_handBody[i] || g_footBody[i] ||
                                        g_noImpact[i]) continue;
                                    if (!(g_armBody[i] || g_legBody[i] || g_headBody[i]))
                                        continue;
                                    const float al = g_bVel[i][0]*g_throw[0] +
                                                     g_bVel[i][1]*g_throw[1] +
                                                     g_bVel[i][2]*g_throw[2];
                                    const float drop = mean - al;
                                    if (drop > g_crashRelDrop && drop > g_crashScore) {
                                        g_crashScore = drop; g_crashBody = i;
                                    }
                                }
                            }
                        } else if (g_crashBody >= 0) {
                            int cb = g_crashBody;
                            if (g_headBody[cb] && g_headIdx >= 0 && g_bOk[g_headIdx])
                                cb = g_headIdx;
                            g_impactBody = cb; g_landT = t; g_grabSide = -1;
                            g_impactScore = g_crashScore;
                            g_retargetUntil = t + 30.0;
                            g_retargetAt = t + 0.35; g_bMinVz[cb] = 0.0f;
                            char cnm[64];
                            BodyBoneName(mc, arr2, cb, cnm, sizeof(cnm));
                            TwkLogBail("[body] crash: %s took the hit while he was still "
                                   "riding, trailing the rest of him by %.0f cm/s at "
                                   "%.0f -- clutching it", cnm, g_crashScore, g_throwSpeed);
                        }
                    }
                    if (g_impactBody < 0) {
                        if (velOk && meanVz < -140.0f) g_sawFall = true;
                        // Low flat bails barely register as a fall, and the old gates
                        // could take until the 2s fallback to call touchdown -- with the
                        // reaction delay on top, a quick game recovery ended the ragdoll
                        // before the hands ever moved (field: "sometimes nothing grabs";
                        // 7 of 93 logged bails never detected touchdown at all).
                        // Shallower fall threshold, looser settle, and a 1s fallback for
                        // bails that never read as falling; a body still dropping fast
                        // is left to the real arrest.
                        const bool down = (g_sawFall && velOk && meanVz > -60.0f) ||
                                          (elD > 0.5 && velOk && fabsf(meanVz) < 100.0f) ||
                                          (elD > 1.0 && !g_sawFall) || elD > 2.5;
                        if (down) {
                            // GRAB-TARGET POLICY (field spec): a REAL hit to the head or
                            // neck (slam above g_headHitMin) -> the hands go for the HEAD
                            // itself, always. Otherwise the hands grab a LIMB -- the arm
                            // or leg that took the biggest hit -- never a torso bone; a
                            // soft landing falls back to the lowest limb. SlamScore
                            // covers walls too (the horizontal cliff).
                            int low = -1;
                            float headHit = 0.0f;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_headBody[i] || !g_bOk[i]) continue;
                                const float sc = SlamScore(i);
                                if (sc > headHit) headHit = sc;
                            }
                            if (headHit > g_headHitMin && g_headIdx >= 0 &&
                                g_bOk[g_headIdx]) {
                                low = g_headIdx;
                            } else {
                                float best = 150.0f, lowZ = 1e30f; int lowest = -1;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    if (!g_bOk[i] || g_handBody[i] || g_footBody[i] ||
                                        g_noImpact[i] || g_headBody[i] ||
                                        !(g_armBody[i] || g_legBody[i])) continue;
                                    const float sc = SlamScore(i);
                                    if (sc > best) { best = sc; low = i; }
                                    if (g_bPos[i][2] < lowZ) {
                                        lowZ = g_bPos[i][2]; lowest = i;
                                    }
                                }
                                if (low < 0) low = lowest;
                            }
                            if (low >= 0) {
                                // A HIT TO THE BACK IS REACHED WITH ONE HAND -- both arms
                                // wrapping behind you is a shape a person cannot make. The
                                // nearer hand takes it, chosen once so it cannot dither.
                                g_grabSide = -1;
                                if (g_torsoBody[low]) {
                                    float bd = 1e30f;
                                    for (int h = 0; h < n2 && h < kMaxBodies; h++) {
                                        if (!g_handBody[h] || !g_bOk[h]) continue;
                                        const float dx2 = g_bPos[h][0]-g_bPos[low][0];
                                        const float dy2 = g_bPos[h][1]-g_bPos[low][1];
                                        const float dz2 = g_bPos[h][2]-g_bPos[low][2];
                                        const float dd = dx2*dx2 + dy2*dy2 + dz2*dz2;
                                        if (dd < bd) { bd = dd;
                                                       g_grabSide = g_leftSide[h] ? 0 : 1; }
                                    }
                                }
                                g_impactBody = low; g_landT = t;
                                g_impactScore = SlamScore(low);
                                // Retargeting only stays open for a beat: a real tumble
                                // slams new parts EARLY. Left open, the clutch's own rock
                                // cycle registers as endless fresh slams (field log:
                                // Head->Neck->Head every 0.35s for the whole ragdoll) and
                                // the hold-refresh below makes the clutch IMMORTAL -- the
                                // body ends up levitating on its own grab forces (the
                                // "colliding with something invisible" float).
                                // A HARDER HIT LATER TAKES OVER, for as long as he is
                                // down. This used to shut two seconds after landing and
                                // judge each new impact against a fixed threshold rather
                                // than against how hard the part he is already holding was
                                // hit -- so a leg that landed first kept the grab even
                                // when the head slammed far harder afterwards (field).
                                g_retargetUntil = t + 30.0;
                                g_retargetAt = t + 0.35; g_bMinVz[low] = 0.0f;
                                char inm[64];
                                BodyBoneName(mc, arr2, low, inm, sizeof(inm));
                                TwkLogBail("[body] impact: %s hit at %.0f cm/s -- clutching it",
                                       inm, g_throwSpeed);
                            }
                        }
                    } else if (t >= g_retargetAt && t < g_retargetUntil) {
                        // Keep listening after touchdown: a tumble slams new parts down.
                        // The hands move to the LAST part that hit (a slam = deep recent
                        // downward velocity that just arrested), and the hold refreshes
                        // so the grab stays continuous.
                        int hit2 = -1; float best2 = g_impactScore * 1.15f;
                        if (best2 < 150.0f) best2 = 150.0f;
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_bOk[i] || g_handBody[i] || g_footBody[i] ||
                                g_noImpact[i] || i == g_impactBody) continue;
                            if (!(g_armBody[i] || g_legBody[i] || g_headBody[i])) continue;
                            // how hard did THIS part just hit, measured against how hard
                            // the part he is holding was hit -- and a head knock counts
                            // for more, the way a person drops everything to hold their
                            // head
                            float sc7 = SlamScore(i);
                            if (g_headBody[i]) {
                                if (sc7 < g_headSteal) continue;
                                sc7 *= g_headBias;
                            }
                            if (sc7 > best2) { best2 = sc7; hit2 = i; }
                        }
                        if (hit2 >= 0 && g_headBody[hit2] && g_headIdx >= 0 &&
                            g_bOk[g_headIdx]) hit2 = g_headIdx;
                        if (hit2 == g_impactBody) hit2 = -1;
                        if (hit2 >= 0) {
                            g_grabSide = -1;
                            if (g_torsoBody[hit2]) {
                                float bd2 = 1e30f;
                                for (int h = 0; h < n2 && h < kMaxBodies; h++) {
                                    if (!g_handBody[h] || !g_bOk[h]) continue;
                                    const float dx3 = g_bPos[h][0]-g_bPos[hit2][0];
                                    const float dy3 = g_bPos[h][1]-g_bPos[hit2][1];
                                    const float dz3 = g_bPos[h][2]-g_bPos[hit2][2];
                                    const float dd2 = dx3*dx3 + dy3*dy3 + dz3*dz3;
                                    if (dd2 < bd2) { bd2 = dd2;
                                                     g_grabSide = g_leftSide[h] ? 0 : 1; }
                                }
                            }
                            g_impactBody = hit2; g_retargetAt = t + 0.35;
                            g_impactScore = best2;
                            g_bMinVz[hit2] = 0.0f; g_bPkH[hit2] = 0.0f;
                            if (t - g_landT > 1.0) g_landT = t - 1.0;
                            char inm2[64];
                            BodyBoneName(mc, arr2, hit2, inm2, sizeof(inm2));
                            TwkLogBail("[body] impact: now clutching %s", inm2);
                        }
                    }
                    if (g_impactBody < 0) {
                        // ---- FLIGHT: reach along throw->down, head held off the ground
                        // (the world-up rule; field-confirmed). Runs until touchdown.
                        float t01 = (float)(el / ((double)g_bailMs / 1000.0));
                        if (t01 > 1.0f) t01 = 1.0f;
                        // EXTRA WEIGHT while falling (see the knob comment)
                        if (g_fallAmt > 0) {
                            const float gw[3] = { 0.0f, 0.0f,
                                -g_extraGrav * ((float)g_fallAmt / 100.0f) };
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_bOk[i]) continue;
                                uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                g_addForce(bi, gw, true, true);
                            }
                        }
                        float dir[3] = { g_throw[0] * (1.0f - t01), g_throw[1] * (1.0f - t01),
                                         g_throw[2] * (1.0f - t01) + (-1.0f) * t01 };
                        const float dm = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
                        if (dm > 0.01f) {
                            const float acc = g_bailAccel * amt / dm;
                            const float f[3] = { dir[0] * acc, dir[1] * acc, dir[2] * acc };
                            // The leg counter-push closes the couple: no net shove along
                            // the throw (a one-sided reach flung the whole body), and the
                            // couple IS the pitch-over. It FADES with the down-blend --
                            // countering a straight-down reach is a straight-UP push on
                            // the thighs, ~3x gravity of pure floatiness (part of the
                            // "no proper gravity" feel).
                            const float cf = 0.8f * (1.0f - t01);
                            const float fc[3] = { -f[0] * cf, -f[1] * cf, -f[2] * cf };
                            const float h[3] = { 0.0f, 0.0f, g_tuckAccel * amt };
                            // WRIST BRACE: the PALM leads the reach. Hand and forearm
                            // getting identical force adds zero torque about the wrist,
                            // and the joint flops into its loose inward curl (field:
                            // "wrist bends inwards when falling backwards"). The
                            // differential -- hands 1.3x, forearms 0.7x of the same
                            // reach -- levers the wrist into extension, palm presented
                            // to the ground, whatever the fall direction.
                            const float fh2[3] = { f[0]*1.3f, f[1]*1.3f, f[2]*1.3f };
                            const float ff2[3] = { f[0]*0.7f, f[1]*0.7f, f[2]*0.7f };
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                uint8_t* bi = ((uint8_t**)arr2)[i];
                                if (!bi) continue;
                                if (g_reacher[i]) {
                                    if (g_armBroken[g_leftSide[i] ? 0 : 1]) continue;
                                    g_addForce(bi, g_handBody[i] ? fh2 : ff2, true, true);
                                }
                                else if (g_kneeBody[i]) g_addForce(bi, fc, true, true);
                                else if (g_headBody[i]) g_addForce(bi, h, true, true);
                            }
                        }
                    } else {
                        // ---- THE CLUTCH: constant from the instant of touchdown. Hands
                        // seek the impact part's live position (and come to REST on it via
                        // the near-target ease), calves/feet draw to the pelvis, the head
                        // tucks in. The slow oscillation is the squeeze/rock that keeps it
                        // alive without reading as twitching.
                        const double elC = t - g_landT - (double)g_grabDelayMs / 1000.0;
                        float env = (elC < 0.0)
                            ? 0.0f
                            : 1.0f - (float)(elC / ((double)g_clutchMs / 1000.0));
                        // Targets and facing live OUTSIDE the grab envelope: the roll and
                        // the arms-in-front hold keep working however long the body lies
                        // there (field: "should ALWAYS try and flip themselves over").
                        const int pv = (g_pelvisBody >= 0 && g_bOk[g_pelvisBody])
                                       ? g_pelvisBody : g_impactBody;
                        const float* impP = g_bPos[g_impactBody];
                        const float* pelP = g_bPos[pv];
                        float chest[3] = { pelP[0], pelP[1], pelP[2] };
                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                            if (!g_headBody[i] || !g_bOk[i]) continue;
                            chest[0] += (g_bPos[i][0] - pelP[0]) * 0.65f;
                            chest[1] += (g_bPos[i][1] - pelP[1]) * 0.65f;
                            chest[2] += (g_bPos[i][2] - pelP[2]) * 0.65f;
                            break;
                        }
                        UpdateBellyCompass(pv);
                        // one line per bail describing what the legs actually did, so a
                        // report of "it curled the wrong way" comes with numbers: bellyZ
                        // says which way up the body is, kneeZ how much room the knee had,
                        // and bend is the shin's deviation (positive = the wrong way).
                        if (!g_legLogged && g_bellyOk && g_impactBody >= 0 &&
                            t - g_landT > 1.2) {
                            g_legLogged = true;
                            float gz3 = 1e30f;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++)
                                if (g_bOk[i] && g_bPos[i][2] < gz3) gz3 = g_bPos[i][2];
                            float bend[2] = {0,0}, kneeZ[2] = {0,0};
                            for (int sd = 0; sd < 2; sd++) {
                                const int ti2 = g_thighIdx[sd], ci2 = g_calfIdx[sd],
                                          fi2 = g_footIdx[sd];
                                if (ti2 < 0 || ci2 < 0 || fi2 < 0) continue;
                                if (!g_bOk[ti2] || !g_bOk[ci2] || !g_bOk[fi2]) continue;
                                float fm2[3] = { g_bPos[ci2][0]-g_bPos[ti2][0],
                                                 g_bPos[ci2][1]-g_bPos[ti2][1],
                                                 g_bPos[ci2][2]-g_bPos[ti2][2] };
                                const float fl2 = sqrtf(fm2[0]*fm2[0] + fm2[1]*fm2[1] +
                                                        fm2[2]*fm2[2]);
                                if (fl2 < 5.0f) continue;
                                fm2[0]/=fl2; fm2[1]/=fl2; fm2[2]/=fl2;
                                const float sh3[3] = { g_bPos[fi2][0]-g_bPos[ci2][0],
                                                       g_bPos[fi2][1]-g_bPos[ci2][1],
                                                       g_bPos[fi2][2]-g_bPos[ci2][2] };
                                const float dp3 = sh3[0]*fm2[0] + sh3[1]*fm2[1] +
                                                  sh3[2]*fm2[2];
                                bend[sd] = (sh3[0]-fm2[0]*dp3)*g_belly[0] +
                                           (sh3[1]-fm2[1]*dp3)*g_belly[1] +
                                           (sh3[2]-fm2[2]*dp3)*g_belly[2];
                                kneeZ[sd] = g_bPos[ci2][2] - gz3;
                            }
                            TwkLogBail("[body] legs: bellyZ %+.2f | left kneeZ %.0f bend %+.0f "
                                   "| right kneeZ %.0f bend %+.0f (bend + is the wrong way)",
                                   g_belly[2], kneeZ[0], bend[0], kneeZ[1], bend[1]);
                        }
                        // ---- ARMS SETTLE IN (see the knob comment) ---------------------
                        if (g_settleAccel > 0.0f) {
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                // hands AND forearms: an arm left hanging behind the back
                                // after the roll is the elbow's doing, not the hand's
                                if ((!g_handBody[i] && !g_reacher[i]) || !g_bOk[i]) continue;
                                // ...but an arm that is holding onto the hurt part is BUSY.
                                // The settle firms up as the body goes still, which is
                                // exactly when it was overpowering the grab and dragging
                                // the hand away from what it was holding.
                                if (g_grabbing[i]) continue;
                                const int asd2 = g_leftSide[i] ? 0 : 1;
                                if (g_armBroken[asd2]) continue;
                                const int sh2 = g_clavIdx[asd2];
                                if (sh2 < 0 || !g_bOk[sh2]) continue;
                                // WHERE AN ARM RESTS: beside the ribs, not at the throat.
                                // Aiming the hand at its own clavicle parked it against
                                // the neck, so every fall ended with a hand at the face
                                // (field). Out from the shoulder and down toward the hip
                                // is where a resting arm actually lies.
                                const int oth = g_clavIdx[asd2 ? 0 : 1];
                                float rest[3] = { g_bPos[sh2][0], g_bPos[sh2][1],
                                                  g_bPos[sh2][2] };
                                // OUTWARD MUST BE HORIZONTAL. This used the shoulder-to-
                                // shoulder line, which is nearly VERTICAL when the body is
                                // on its side -- so it moved the spot up rather than out,
                                // and once the height was overwritten with ground level
                                // the rest spot collapsed back onto the base of the neck.
                                // That is why the hands kept ending up at his face.
                                float ox2 = 0.0f, oy2 = 0.0f;
                                if (oth >= 0 && g_bOk[oth]) {
                                    ox2 = g_bPos[sh2][0]-g_bPos[oth][0];
                                    oy2 = g_bPos[sh2][1]-g_bPos[oth][1];
                                }
                                if (sqrtf(ox2*ox2 + oy2*oy2) < 8.0f) {
                                    // shoulders stacked: go out square to the spine instead
                                    const float sxh = chest[0]-pelP[0],
                                                syh = chest[1]-pelP[1];
                                    ox2 = syh; oy2 = -sxh;
                                    if (asd2 == 0) { ox2 = -ox2; oy2 = -oy2; }
                                }
                                {
                                    const float om2 = sqrtf(ox2*ox2 + oy2*oy2);
                                    if (om2 > 1.0f) {
                                        rest[0] += ox2/om2 * g_settleOut;
                                        rest[1] += oy2/om2 * g_settleOut;
                                    }
                                }
                                {
                                    float hx = chest[0]-pelP[0], hy = chest[1]-pelP[1],
                                          hz = chest[2]-pelP[2];
                                    const float hm2 = sqrtf(hx*hx + hy*hy + hz*hz);
                                    if (hm2 > 5.0f) {
                                        rest[0] -= hx/hm2 * g_settleDown;
                                        rest[1] -= hy/hm2 * g_settleDown;
                                        rest[2] -= hz/hm2 * g_settleDown;
                                    }
                                    // and put it ON the ground. A rest spot floating at
                                    // shoulder height is inside the torso when the body is
                                    // lying down, so the arm was pulled in under the chest
                                    // rather than out onto the floor beside it.
                                    float gz6 = 1e30f;
                                    for (int q6 = 0; q6 < n2 && q6 < kMaxBodies; q6++)
                                        if (g_bOk[q6] && g_bPos[q6][2] < gz6)
                                            gz6 = g_bPos[q6][2];
                                    if (gz6 < 1e29f) rest[2] = gz6 + g_settleGround;
                                }
                                // AND NEVER BESIDE THE HEAD. Whatever the geometry does,
                                // a resting hand does not belong at the face -- if the
                                // spot lands near the head it is pushed away from it.
                                if (g_headIdx >= 0 && g_bOk[g_headIdx]) {
                                    float hx5 = rest[0]-g_bPos[g_headIdx][0];
                                    float hy5 = rest[1]-g_bPos[g_headIdx][1];
                                    const float hd5 = sqrtf(hx5*hx5 + hy5*hy5);
                                    if (hd5 < 35.0f) {
                                        if (hd5 > 1.0f) {
                                            rest[0] += hx5/hd5 * (35.0f - hd5);
                                            rest[1] += hy5/hd5 * (35.0f - hd5);
                                        } else {
                                            const float sxh2 = chest[0]-pelP[0],
                                                        syh2 = chest[1]-pelP[1];
                                            const float sm7 = sqrtf(sxh2*sxh2 + syh2*syh2);
                                            if (sm7 > 1.0f) {
                                                rest[0] -= sxh2/sm7 * 35.0f;
                                                rest[1] -= syh2/sm7 * 35.0f;
                                            }
                                        }
                                    }
                                }
                                float d5[3] = { rest[0]-g_bPos[i][0],
                                                rest[1]-g_bPos[i][1],
                                                rest[2]-g_bPos[i][2] };
                                const float dl5 = sqrtf(d5[0]*d5[0] + d5[1]*d5[1] +
                                                        d5[2]*d5[2]);
                                if (dl5 < g_settleFar) continue;
                                // and it firms up once the body has stopped tumbling, so
                                // the last thing that happens is the arms coming to rest
                                float calm = 1.0f;
                                if (g_pelvisBody >= 0 && g_bOk[g_pelvisBody]) {
                                    const float ps = sqrtf(
                                        g_bVel[g_pelvisBody][0]*g_bVel[g_pelvisBody][0] +
                                        g_bVel[g_pelvisBody][1]*g_bVel[g_pelvisBody][1] +
                                        g_bVel[g_pelvisBody][2]*g_bVel[g_pelvisBody][2]);
                                    calm = (ps > 40.0f) ? 0.5f : 1.6f;
                                }
                                const float sc5 = g_settleAccel * amt * calm / dl5;
                                float f7[3] = { d5[0]*sc5, d5[1]*sc5, d5[2]*sc5 };
                                if (f7[2] < 0.0f) f7[2] *= 0.35f;   // never press downward
                                uint8_t* bh = ((uint8_t**)arr2)[i];
                                uint8_t* bs = ((uint8_t**)arr2)[sh2];
                                if (bh) g_addForce(bh, f7, true, true);
                                if (bs) {
                                    // the shoulder takes the opposite share: the arm comes
                                    // in, the body does not go out to meet it
                                    const float r5 = BodyMassProxy(i) / BodyMassProxy(sh2);
                                    const float f8[3] = { -f7[0]*r5, -f7[1]*r5, -f7[2]*r5 };
                                    g_addForce(bs, f8, true, true);
                                }
                            }
                        }
                        // ---- THE KNEE GUARD (see the knob comment) ---------------------
                        if (g_bellyOk && g_kneeGuard > 0.0f) {
                            for (int sd = 0; sd < 2; sd++) {
                                const int ti = g_thighIdx[sd], ci = g_calfIdx[sd],
                                          fi = g_footIdx[sd];
                                if (ti < 0 || ci < 0 || fi < 0) continue;
                                if (!g_bOk[ti] || !g_bOk[ci] || !g_bOk[fi]) continue;
                                float fem[3] = { g_bPos[ci][0]-g_bPos[ti][0],
                                                 g_bPos[ci][1]-g_bPos[ti][1],
                                                 g_bPos[ci][2]-g_bPos[ti][2] };
                                const float fl = sqrtf(fem[0]*fem[0] + fem[1]*fem[1] +
                                                       fem[2]*fem[2]);
                                if (fl < 5.0f) continue;
                                fem[0]/=fl; fem[1]/=fl; fem[2]/=fl;
                                const float shin[3] = { g_bPos[fi][0]-g_bPos[ci][0],
                                                        g_bPos[fi][1]-g_bPos[ci][1],
                                                        g_bPos[fi][2]-g_bPos[ci][2] };
                                const float dp2 = shin[0]*fem[0] + shin[1]*fem[1] +
                                                  shin[2]*fem[2];
                                const float pp2[3] = { shin[0]-fem[0]*dp2,
                                                       shin[1]-fem[1]*dp2,
                                                       shin[2]-fem[2]*dp2 };
                                // deviation toward the BELLY side = bending the wrong way
                                const float bad = pp2[0]*g_belly[0] + pp2[1]*g_belly[1] +
                                                  pp2[2]*g_belly[2];
                                if (bad > 0.0f && g_setLinVel) {
                                    // THE HARD STOP. The shin has crossed to the wrong
                                    // side of straight, so remove whatever velocity is
                                    // still carrying it that way -- a knee cannot be
                                    // argued out of hyperextending with forces alone,
                                    // because the impact that drives it there is far
                                    // bigger than anything we apply.
                                    const int lb[2] = { ci, fi };
                                    for (int q = 0; q < 2; q++) {
                                        const int b2 = lb[q];
                                        uint8_t* bv = ((uint8_t**)arr2)[b2];
                                        if (!bv) continue;
                                        const float vd = g_bVel[b2][0]*g_belly[0] +
                                                         g_bVel[b2][1]*g_belly[1] +
                                                         g_bVel[b2][2]*g_belly[2];
                                        if (vd <= 0.0f) continue;
                                        const float nv[3] = {
                                            g_bVel[b2][0] - g_belly[0]*vd,
                                            g_bVel[b2][1] - g_belly[1]*vd,
                                            g_bVel[b2][2] - g_belly[2]*vd };
                                        g_setLinVel(bv, nv, false);
                                        g_bVel[b2][0] = nv[0]; g_bVel[b2][1] = nv[1];
                                        g_bVel[b2][2] = nv[2];
                                    }
                                }
                                if (bad > 2.0f)
                                    g_legStraight[sd] = t + (double)g_straightMs / 1000.0;
                                if (t >= g_legStraight[sd]) continue;
                                // STRAIGHTEN OUT: extend the shin back along the femur
                                // line until the leg is straight, instead of wrestling a
                                // joint the rig is happy to bend the wrong way
                                const float kk2 = g_kneeGuard * amt;
                                const float fb[3] = { fem[0]*kk2, fem[1]*kk2,
                                                      fem[2]*kk2 };
                                uint8_t* bc = ((uint8_t**)arr2)[ci];
                                uint8_t* bf = ((uint8_t**)arr2)[fi];
                                uint8_t* bt = ((uint8_t**)arr2)[ti];
                                if (bc) g_addForce(bc, fb, true, true);
                                if (bf) g_addForce(bf, fb, true, true);
                                // ...and the thigh takes the opposite share, so the leg
                                // folds instead of the body being pushed
                                if (bt) {
                                    const float sh2 = (BodyMassProxy(ci) +
                                                       BodyMassProxy(fi)) /
                                                      BodyMassProxy(ti);
                                    const float ft[3] = { -fem[0]*kk2*sh2,
                                                          -fem[1]*kk2*sh2,
                                                          -fem[2]*kk2*sh2 };
                                    g_addForce(bt, ft, true, true);
                                }
                            }
                        }
                        // ---- THE FETAL CURL (see the knob comment). Tucking also makes
                        // the roll work: a curled body has a smaller footprint, less
                        // friction and less inertia to turn, which is exactly why people
                        // tuck up before they roll over.
                        // THE TUCK. A face-down body tucks its knees UNDER itself --
                        // that is how a person gets off the floor, and the knees pressing
                        // the ground is exactly what RAISES the hips. So this is a
                        // GROUND-BRACED behaviour, like the roll: momentum-neutralising it
                        // cancels the very lift it exists to produce (the same mistake
                        // that once made the roll useless). No neutralisation here. The
                        // knee target is clamped to floor level instead of being aimed
                        // into the floor, the legs are UNWEIGHTED while they tuck -- they
                        // never had the force to move themselves against gravity -- and
                        // only the net upward acceleration is capped, so the body can be
                        // pushed up onto its knees but never launched.
                        // ONLY WHILE ACTUALLY LYING DOWN. Landing on his knees leaves
                        // the torso UPRIGHT, and tucking weightless legs toward the chest
                        // of an upright body pitches it over backwards -- the backflip off
                        // a knee landing (field). A tuck is something you do lying down,
                        // so it fades out as the torso stands up.
                        float lieK = 0.0f;
                        {
                            float spz = chest[2] - pelP[2];
                            const float spl = sqrtf((chest[0]-pelP[0])*(chest[0]-pelP[0]) +
                                                    (chest[1]-pelP[1])*(chest[1]-pelP[1]) +
                                                    spz*spz);
                            if (spl > 5.0f) {
                                const float up2 = fabsf(spz) / spl;   // 0 lying, 1 upright
                                lieK = (0.70f - up2) / 0.20f;
                                if (lieK > 1.0f) lieK = 1.0f;
                                if (lieK < 0.0f) lieK = 0.0f;
                            }
                        }
                        // While the JOINTS are being driven into the curl, the old
                        // force-based tuck only fights them -- two systems pulling the same
                        // limbs toward different ideas of the same pose is what made the
                        // body spin and stall. The drive owns the curl now.
                        if (g_bellyOk && env > 0.0f && g_curlAccel > 0.0f && lieK > 0.0f &&
                            !g_driveCurl) {
                            const float cs = sqrtf(env) * amt * lieK;
                            float gzc = 1e30f;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++)
                                if (g_bOk[i] && g_bPos[i][2] < gzc) gzc = g_bPos[i][2];
                            float knee[3] = {
                                chest[0] + g_belly[0] * g_curlFront,
                                chest[1] + g_belly[1] * g_curlFront,
                                chest[2] + g_belly[2] * g_curlFront };
                            if (knee[2] < gzc + 12.0f) knee[2] = gzc + 12.0f;
                            // WHERE THE HEEL FOLDS TO. Aiming the shin at the pelvis
                            // itself works face-down, where the heel just travels along
                            // the floor -- but the pelvis is ON the floor, so lying on
                            // your BACK that same target drives the shin down into the
                            // ground and the only path left is folding the knee the wrong
                            // way (field: forward falls curl correctly, backward ones do
                            // not). A real tuck brings the heels toward the butt on the
                            // BELLY side, clear of the floor.
                            float shin[3] = {
                                pelP[0] + g_belly[0] * g_curlShinFront,
                                pelP[1] + g_belly[1] * g_curlShinFront,
                                pelP[2] + g_belly[2] * g_curlShinFront };
                            if (shin[2] < gzc + 10.0f) shin[2] = gzc + 10.0f;
                            float cAcc[kMaxBodies][3]; bool cHas[kMaxBodies];
                            float cW = 0.0f, cUp = 0.0f, cPos = 0.0f;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                cHas[i] = false;
                                if (!g_bOk[i] || i == g_pelvisBody) continue;
                                const float* tgt = nullptr; float k = 0.0f;
                                bool leg = false;
                                // A FETAL TUCK IS TWO DIFFERENT JOINTS. The thigh goes
                                // to the chest (hip flexion) and the shin goes to the
                                // PELVIS (knee flexion -- heel toward the butt). Sending
                                // the shin at the CHEST instead is the motion that
                                // HYPEREXTENDS a knee: its shortest path there is
                                // straight through, so physics straightens the joint past
                                // flat -- the backward bend, over and over.
                                // a leg that started folding the wrong way is out of the
                                // tuck while it straightens itself out
                                if (g_legBody[i] &&
                                    t < g_legStraight[g_leftSide[i] ? 0 : 1]) continue;
                                if (g_kneeBody[i])       { tgt = knee;  k = 1.0f;
                                                           leg = true; }
                                else if (g_footBody[i])  { continue; }
                                else if (g_legBody[i])   {
                                    // no room under the knee yet -> hip flexion only
                                    const int ks = g_leftSide[i] ? 0 : 1;
                                    const int kn = g_calfIdx[ks];
                                    if (kn >= 0 && g_bOk[kn] &&
                                        g_bPos[kn][2] - gzc < g_kneeRoomCm) continue;
                                    tgt = shin;  k = g_curlCalfK; leg = true;
                                }
                                else if (g_headBody[i])  { tgt = chest; k = g_curlHeadK; }
                                else if (g_armBody[i] && !g_handBody[i])
                                                         { tgt = chest; k = g_curlArmK; }
                                if (!tgt) continue;
                                float d[3] = { tgt[0]-g_bPos[i][0], tgt[1]-g_bPos[i][1],
                                               tgt[2]-g_bPos[i][2] };
                                const float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                                if (dl < 8.0f) continue;      // already tucked
                                float sc = g_curlAccel * k * cs / dl;
                                if (dl < 25.0f) sc *= dl / 25.0f;
                                cAcc[i][0] = d[0]*sc; cAcc[i][1] = d[1]*sc;
                                cAcc[i][2] = d[2]*sc;
                                // unweight the leg so the tuck can actually move it (a
                                // pull that nets out below gravity moves nothing) -- but
                                // only while it is still down near the floor, or a leg
                                // already in the air keeps being carried upward
                                if (leg) {
                                    float lu = 1.0f - (g_bPos[i][2] - gzc) / 40.0f;
                                    if (lu > 1.0f) lu = 1.0f;
                                    if (lu > 0.0f) cAcc[i][2] += 980.0f * cs * k * lu;
                                }
                                cHas[i] = true;
                                const float w = BodyMassProxy(i);
                                cW += w; cUp += w * cAcc[i][2];
                                if (cAcc[i][2] > 0.0f) cPos += w * cAcc[i][2];
                            }
                            // THE RULE THAT KEEPS HIM ON THE FLOOR AND STILL LETS HIM
                            // MOVE: vertical forces are safe -- gravity and the floor
                            // bound them, and they are what lifts a hip or a shoulder.
                            // HORIZONTAL forces just slide the whole body across the
                            // ground, so they must be INTERNAL: the mass-weighted
                            // horizontal mean is subtracted, which leaves the knees free
                            // to come under the body while the body itself stays put.
                            if (cW > 0.0f) {
                                float mx = 0.0f, my = 0.0f;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    if (!cHas[i]) continue;
                                    const float w = BodyMassProxy(i);
                                    mx += cAcc[i][0]*w; my += cAcc[i][1]*w;
                                }
                                mx /= cW; my /= cW;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    if (!cHas[i]) continue;
                                    cAcc[i][0] -= mx; cAcc[i][1] -= my;
                                }
                            }
                            // NO VERTICAL CAP HERE. The tuck only ever touches LIMBS and
                            // the head -- never the torso or pelvis -- so it cannot lift
                            // the body no matter how hard it pulls, and on a body lying on
                            // its back the tuck is ENTIRELY an upward motion. Capping it
                            // (against the small mass of just those limbs, at that) scaled
                            // the lift away to nothing and left only the horizontal part:
                            // legs dragged along the ground with the knee folding whichever
                            // way was cheapest, and the torso shoved the other way by the
                            // neutralisation -- the legs bending back AND the back arching,
                            // both from one cap. The horizontal above stays internal; the
                            // vertical is free.
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!cHas[i]) continue;
                                uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                g_addForce(bi, cAcc[i], true, true);
                            }
                        }
                        // ROLL-OVER, RETRYING (field: "a lot of times the skater just
                        // stays face down"): every attempt gets g_rollMs of effort; if
                        // the body is still -- or again -- face-down after a cooldown,
                        // it tries again, for as long as the ragdoll lasts.
                        const float pvSpd2 = sqrtf(g_bVel[pv][0]*g_bVel[pv][0] +
                                                   g_bVel[pv][1]*g_bVel[pv][1] +
                                                   g_bVel[pv][2]*g_bVel[pv][2]);
                        // A PERSON GIVES UP. After a few honest attempts at turning over
                        // he stays where he landed rather than squirming at the floor
                        // forever.
                        if (g_bellyOk && t > g_rollUntil + 0.6 && pvSpd2 < 100.0f &&
                            g_rollTries < g_rollMaxTries) {
                            if (g_belly[2] > g_faceDownZ) g_faceDownSince = 0.0;
                            else if (g_faceDownSince == 0.0) g_faceDownSince = t;
                            else if (t - g_faceDownSince > 0.3) {
                                g_faceDownSince = 0.0;
                                g_rollUntil = t + (double)g_rollMs / 1000.0;
                                // L = spine x up, horizontal side axis; map which
                                // classified body side lies along +L via a left leg.
                                float sx = chest[0]-pelP[0], sy = chest[1]-pelP[1];
                                float lx = sy, ly = -sx;
                                const float lm = sqrtf(lx*lx + ly*ly);
                                float leftDot = 0.0f;
                                if (lm > 1.0f) {
                                    lx /= lm; ly /= lm;
                                    const int lt = g_thighIdx[0];
                                    if (lt >= 0 && g_bOk[lt])
                                        leftDot = (g_bPos[lt][0]-pelP[0])*lx +
                                                  (g_bPos[lt][1]-pelP[1])*ly;
                                }
                                float away = (Frand() < 0.5f) ? 1.0f : -1.0f;
                                if (g_impactBody >= 0 &&
                                    (g_armBody[g_impactBody] || g_legBody[g_impactBody]) &&
                                    leftDot != 0.0f) {
                                    const float hurtSide = g_leftSide[g_impactBody]
                                        ? (leftDot > 0 ? 1.0f : -1.0f)
                                        : (leftDot > 0 ? -1.0f : 1.0f);
                                    away = -hurtSide;
                                }
                                // GO THE WAY HE IS ALREADY GOING. The direction used to be
                                // decided without ever looking at how the body was lying,
                                // so a retry could pick the opposite side to the one he was
                                // half rolled onto and fight uphill against his own weight
                                // (field: "it will be partly rolled and then try rolling
                                // the other way"). If the body is already tipped, that
                                // decides it; the hurt side only matters while he is flat.
                                if (lm > 1.0f) {
                                    const float tip = g_belly[0]*(lx/lm) +
                                                      g_belly[1]*(ly/lm);
                                    if (tip > 0.15f)       away =  1.0f;
                                    else if (tip < -0.15f) away = -1.0f;
                                    else {
                                        // FLAT ON HIS FACE, so nothing is tipped and the
                                        // choice used to fall through to a coin flip --
                                        // half the time into the harder direction (field:
                                        // four failed attempts at bellyZ -0.73). The arms
                                        // decide it: whichever is already tucked in near
                                        // the chest becomes the side he rolls TOWARD, so
                                        // that arm posts where it already is and the arm
                                        // out to the side is the one free to swing over.
                                        float innerD = 0.0f, innerAbs = 1e30f;
                                        for (int q7 = 0; q7 < n2 && q7 < kMaxBodies; q7++) {
                                            if (!g_armBody[q7] || g_handBody[q7] ||
                                                !g_bOk[q7]) continue;
                                            const float dd7 =
                                                (g_bPos[q7][0]-pelP[0])*(lx/lm) +
                                                (g_bPos[q7][1]-pelP[1])*(ly/lm);
                                            if (fabsf(dd7) < innerAbs) {
                                                innerAbs = fabsf(dd7); innerD = dd7;
                                            }
                                        }
                                        if (innerAbs < 1e29f && fabsf(innerD) > 1.0f)
                                            away = (innerD > 0.0f) ? 1.0f : -1.0f;
                                    }
                                }
                                g_rollSign = away; g_rollLogged = false;
                                g_rollTries++;
                                TwkLogBail("[body] face-down -- rolling over (try %d of %d)",
                                       g_rollTries, g_rollMaxTries);
                                g_rollDone = true;
                            }
                        }
                        if (g_rollUntil > t && g_bellyOk && g_belly[2] > g_rollDoneZ &&
                            t - (g_rollUntil - (double)g_rollMs / 1000.0) >
                                (double)g_rollCommitMs / 1000.0)
                            g_rollUntil = 0.0;   // off the belly, and past the commit
                        const bool rolling = g_rollUntil > t;
                        // OUT OF ATTEMPTS, STILL FACE DOWN, AND FINALLY STILL -- he stops
                        // holding the tuck. It comes back if he somehow ends up off his
                        // belly later, and only then, so it cannot flicker on and off
                        // while he is settling.
                        if (g_limpLegs && g_bellyOk && !rolling && g_mesh) {
                            const bool giveUp = g_rollTries >= g_rollMaxTries &&
                                                g_belly[2] <= g_faceDownZ &&
                                                pvSpd2 < 100.0f;
                            if (!g_legsLimp && giveUp) {
                                g_legsLimp = true;
                                DriveSpineCurl((uint8_t*)g_mesh, true);
                                TwkLogBail("[body] gave up turning over -- the legs let go so "
                                       "they can straighten out");
                            } else if (g_legsLimp && g_belly[2] > g_rollDoneZ) {
                                g_legsLimp = false;
                                DriveSpineCurl((uint8_t*)g_mesh, true);
                            }
                        }
                        if (rolling) {
                            // THE ROLL, ground-aware. A body lying on the floor CANNOT be
                            // rolled by a balanced couple: the floor absorbs the down
                            // half, and the mass-weighted neutralisation that stopped the
                            // flying then cancelled the up half too -- the whole effort
                            // netted out to a twitch. A real person rolls by LIFTING one
                            // side while the other presses the floor, and the FLOOR
                            // supplies the balancing reaction, so no neutralisation is
                            // wanted here. The lift instead fades with height: once a
                            // side is up it stops being pushed, which bounds the whole
                            // thing without gutting it. The HIPS roll too -- thighs carry
                            // full weight, because they are the pelvis's only levers and
                            // a pelvis left behind just drags the torso back down.
                            float sx = chest[0]-pelP[0], sy = chest[1]-pelP[1];
                            float lx = sy, ly = -sx;
                            const float lm = sqrtf(lx*lx + ly*ly);
                            float rr = (float)((t - (g_rollUntil -
                                (double)g_rollMs / 1000.0)) / (double)g_rollRampS);
                            if (rr > 1.0f) rr = 1.0f; if (rr < 0.0f) rr = 0.0f;
                            const float rAmt = amt * rr;
                            if (lm > 1.0f && rr > 0.0f) {
                                lx = lx / lm * g_rollSign; ly = ly / lm * g_rollSign;
                                float gz = 1e30f;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++)
                                    if (g_bOk[i] && g_bPos[i][2] < gz) gz = g_bPos[i][2];
                                float liftPeak = 0.0f; int nLift = 0;
                                float acc[kMaxBodies][3]; bool hasA[kMaxBodies];
                                float pSum = 0.0f, mAll = 0.0f, mLead = 0.0f;
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    hasA[i] = false;
                                    if (!g_bOk[i]) continue;
                                    float role = 0.0f;
                                    // The legs are tucked in by the curl now, so the
                                    // roll does not need to heave them -- and a lifted
                                    // leg was the ugliest part of it (field). The chest
                                    // and arms lead; the hips get enough not to be left
                                    // behind, the lower legs almost nothing.
                                    if (g_torsoBody[i])                       role = 1.0f;
                                    else if (g_armBody[i] && !g_handBody[i])  role = 1.0f;
                                    else if (g_headBody[i])                   role = 0.6f;
                                    else if (g_kneeBody[i])                   role = 0.6f;
                                    else if (g_footBody[i])                   role = 0.1f;
                                    else if (g_legBody[i])                    role = 0.25f;
                                    else if (g_handBody[i])                   role = 0.5f;
                                    if (role <= 0.0f) continue;
                                    const float od = (g_bPos[i][0]-pelP[0])*lx +
                                                     (g_bPos[i][1]-pelP[1])*ly;
                                    // The lever still favours the outer bodies, but it
                                    // no longer GUTS the lift: lever x role x fade x ramp
                                    // used to multiply out below gravity, so the trailing
                                    // side went weightless and never rose.
                                    float lev = fabsf(od) / 15.0f;
                                    if (lev > 1.0f) lev = 1.0f;
                                    if (lev < 0.6f) lev = 0.6f;
                                    float fr[3] = { 0.0f, 0.0f, 0.0f };
                                    if (g_armBody[i] &&
                                        fabsf(od) < g_armPinnedCm &&
                                        g_bPos[i][2] - gz < 25.0f) {
                                        // PINNED UNDER THE BODY, so the HANDS PRESS THE
                                        // FLOOR -- that is where the leverage comes from,
                                        // and the room for the arm to work appears as the
                                        // chest comes up off it. Shoving the arm sideways
                                        // instead was both futile under his own weight and
                                        // the cause of the sliding and spinning: the roll
                                        // makes horizontal forces internal, so a one-sided
                                        // sideways push becomes a couple, which is a spin.
                                        // Vertical is the only safe axis here -- the floor
                                        // takes the push and gravity bounds the lift.
                                        if (g_handBody[i]) {
                                            fr[2] = -g_armPinnedPush * rAmt;
                                        } else {
                                            fr[2] = 980.0f + g_armFreeLift * 0.4f * rAmt;
                                        }
                                    } else if (g_armBody[i]) {
                                        // THE TWO ARMS DO DIFFERENT JOBS -- this is how a
                                        // person actually rolls off their front. The near
                                        // arm POSTS: it drives in under the chest and
                                        // presses the floor, giving the torso something to
                                        // lever against. The far arm SWINGS OVER the top,
                                        // carrying the body across with it. Treating both
                                        // as "lift them clear" gave neither a purpose.
                                        // WHICH ARM DOES WHICH JOB (field: they were the
                                        // wrong way round). The arm on the side the body is
                                        // rolling TOWARD is the one that posts under the
                                        // chest and pushes; the arm on the side that is
                                        // lifting swings over the top.
                                        if (od > 0.0f) {
                                            float ux = chest[0]-g_bPos[i][0],
                                                  uy = chest[1]-g_bPos[i][1];
                                            const float um3 = sqrtf(ux*ux + uy*uy);
                                            if (um3 > 5.0f) {
                                                fr[0] += ux/um3 * g_armUnderAcc * rAmt;
                                                fr[1] += uy/um3 * g_armUnderAcc * rAmt;
                                            }
                                            // UNWEIGHT IT WHILE IT TRAVELS IN. A hand
                                            // pressed flat on the ground and dragged
                                            // sideways just catches, and then it IS the
                                            // thing the body cannot roll over (field,
                                            // with a picture). It floats in under the
                                            // chest first and only presses once there.
                                            const float near2 = (um3 < 30.0f) ? 1.0f
                                                                              : 30.0f/um3;
                                            fr[2] = 980.0f * (1.0f - near2)
                                                    - g_rollPushAccel * rAmt * near2;
                                        } else {
                                            fr[0] += lx * g_armSwingAcc * rAmt;
                                            fr[1] += ly * g_armSwingAcc * rAmt;
                                            fr[2] = g_armFreeLift * rAmt;      // swing
                                        }
                                    } else if (od < 0.0f) {
                                        float up = 1.0f - (g_bPos[i][2] - gz) /
                                                          g_rollLiftSpan;
                                        if (up > 1.0f) up = 1.0f;
                                        if (up > 0.0f) {
                                            // UNWEIGHT FIRST, then lift: cancel this
                                            // body's gravity, and only what is added on
                                            // top of that actually raises the side.
                                            fr[2] = (980.0f +
                                                     g_rollAccel * lev * role * rAmt) * up;
                                            if (fr[2] > liftPeak) liftPeak = fr[2];
                                            nLift++;
                                        }
                                    } else {
                                        // leading side presses the floor: purchase, and
                                        // the floor absorbs it so nothing is flung
                                        fr[2] = -g_rollPushAccel * lev * role * rAmt;
                                    }
                                    // THE HEAD AND UPPER SPINE ARC IN toward the stomach.
                                    // Curling the top of the body brings its mass to the
                                    // inside of the roll, which is half of how a person
                                    // turns over -- an outstretched head and flat spine
                                    // fight the whole movement.
                                    if (g_headBody[i]) {
                                        const float ht[3] = {
                                            chest[0] + g_belly[0]*15.0f - g_bPos[i][0],
                                            chest[1] + g_belly[1]*15.0f - g_bPos[i][1],
                                            chest[2] + g_belly[2]*15.0f - g_bPos[i][2] };
                                        const float hm4 = sqrtf(ht[0]*ht[0] + ht[1]*ht[1] +
                                                                ht[2]*ht[2]);
                                        if (hm4 > 6.0f) {
                                            const float hs4 = g_headCurlAcc * rAmt / hm4;
                                            fr[0] += ht[0]*hs4; fr[1] += ht[1]*hs4;
                                            fr[2] += ht[2]*hs4;
                                        }
                                    }
                                    // ...and the FAR LEG swings over, exactly like the far
                                    // arm: lifted so it clears the ground, carried in the
                                    // roll direction so it takes the hips with it.
                                    if (g_legBody[i] && !g_footBody[i] && od > 8.0f) {
                                        // feet excluded: a driven foot just paddles, and
                                        // the shin carries it anyway
                                        fr[0] += lx * g_legSwingAcc * rAmt;
                                        fr[1] += ly * g_legSwingAcc * rAmt;
                                        fr[2] += g_legSwingLift * rAmt;
                                    }
                                    acc[i][0] = fr[0]; acc[i][1] = fr[1]; acc[i][2] = fr[2];
                                    hasA[i] = true;
                                    const float w = BodyMassProxy(i);
                                    mAll += w; pSum += w * fr[2];
                                    if (od >= 0.0f) mLead += w;
                                }
                                // ANCHOR THE PLANTED SIDE. The lift is a real net upward
                                // force -- that is what makes the roll work at all, but
                                // left alone it sometimes hoists the whole body off the
                                // floor (field). So the body's NET upward acceleration is
                                // capped well under gravity, and the excess is pushed
                                // into the LEADING side as extra downward force. The
                                // floor absorbs that, the centre of mass stays pinned,
                                // and the couple driving the roll gets STRONGER rather
                                // than weaker -- exactly what a person does when they
                                // press down on one arm to lever the other side up.
                                // horizontal is internal here too -- the leading arm
                                // pulling in must not drag the body with it
                                if (mAll > 0.0f) {
                                    float rx = 0.0f, ry = 0.0f;
                                    for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                        if (!hasA[i]) continue;
                                        const float w = BodyMassProxy(i);
                                        rx += acc[i][0]*w; ry += acc[i][1]*w;
                                    }
                                    rx /= mAll; ry /= mAll;
                                    for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                        if (!hasA[i]) continue;
                                        acc[i][0] -= rx; acc[i][1] -= ry;
                                    }
                                }
                                const float pCap = g_rollNetUp * mAll;
                                if (pSum > pCap) {
                                    float extra = pSum - pCap;
                                    if (mLead > 0.5f) {
                                        float perLead = extra / mLead;
                                        if (perLead > 6000.0f) perLead = 6000.0f;
                                        for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                            if (!hasA[i]) continue;
                                            const float od2 =
                                                (g_bPos[i][0]-pelP[0])*lx +
                                                (g_bPos[i][1]-pelP[1])*ly;
                                            if (od2 < 0.0f) continue;
                                            acc[i][2] -= perLead;
                                            extra -= perLead * BodyMassProxy(i);
                                        }
                                    }
                                    // nothing planted to push against (already on its
                                    // side, or airborne): scale the lift back instead
                                    if (extra > 1.0f && pSum > 1.0f) {
                                        const float sc = 1.0f - extra / pSum;
                                        for (int i = 0; i < n2 && i < kMaxBodies; i++)
                                            if (hasA[i] && acc[i][2] > 0.0f)
                                                acc[i][2] *= (sc > 0.0f ? sc : 0.0f);
                                    }
                                }
                                for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                    if (!hasA[i]) continue;
                                    uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                    g_addForce(bi, acc[i], true, true);
                                }
                                if (!g_rollLogged && nLift > 0) {
                                    g_rollLogged = true;
                                    float pAfter = 0.0f;
                                    for (int i = 0; i < n2 && i < kMaxBodies; i++)
                                        if (hasA[i]) pAfter += BodyMassProxy(i)*acc[i][2];
                                    TwkLogBail("[body] roll: lifting %d bodies, peak %.0f, "
                                           "net %.0f cm/s2 after the cap (gravity is 980)",
                                           nLift, liftPeak,
                                           mAll > 0.0f ? pAfter / mAll : 0.0f);
                                }
                            }
                        }
                        // ARMS STAY IN FRONT: force toward the belly plane whenever an
                        // arm drifts behind the back plane -- runs for the whole ragdoll.
                        // The keeper stands down while the front faces the ground:
                        // "toward the belly" would press the arms INTO the pavement
                        // under the body -- itself an arm-trapper (field); face-down
                        // situations belong to the roll.
                        if (g_bellyOk && g_belly[2] > -0.2f) {
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_armBody[i] || g_handBody[i] || !g_bOk[i]) continue;
                                const int asd = g_leftSide[i] ? 0 : 1;
                                if (g_armBroken[asd]) continue;
                                uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                float ox = g_bPos[i][0]-chest[0],
                                      oy = g_bPos[i][1]-chest[1],
                                      oz = g_bPos[i][2]-chest[2];
                                const float behind = -(ox*g_belly[0] + oy*g_belly[1] +
                                                       oz*g_belly[2]);
                                if (behind < 6.0f) continue;
                                float kk = behind / 25.0f; if (kk > 1.0f) kk = 1.0f;
                                kk *= g_armKeepAccel * amt *
                                      (g_reacher[i] ? 0.7f : 1.0f);
                                const float tk[3] = { g_belly[0]*kk, g_belly[1]*kk,
                                                      g_belly[2]*kk };
                                g_addForce(bi, tk, true, true);
                            }
                        }
                        if (env > 0.0f) {
                            env = sqrtf(env); // strong immediately, long gentle tail
                            // PULL-AND-RELEASE, not amplitude wobble: full clutch at the
                            // top of the cycle, complete release in the trough; the
                            // slump under gravity is the visible half of the rock.
                            float osc = 0.25f + 0.75f *
                                sinf((float)(elC * 6.28318 * (double)g_oscHz));
                            if (osc < 0.0f) osc = 0.0f;
                            // THE CURL WAS REMOVED (field verdict after five rounds of
                            // wrong-way folds and sliding: "lets just remove the curl").
                            // What lands here is only what field-tested well: the hands
                            // continuously grabbing the part that hit, and the leg flail.
                            // After the flail the legs and head are passive physics. No
                            // counter-forces on the ground -- they shoved the pelvis
                            // around; the airborne flight couple keeps its counter-push.
                            // HURT A LEG ANYWHERE -- shin, foot, thigh -- and you reach
                            // for the FRONT OF THE KNEE. Chasing the part that actually
                            // hit meant grabbing at a foot mid-fold and dragging the leg
                            // into exactly the broken shapes we have been fighting; the
                            // knee is where a person grabs, and it is also where the tuck
                            // is bringing the leg anyway, so the two now cooperate.
                            float grabPt[3] = { impP[0], impP[1], impP[2] };
                            if (g_headBody[g_impactBody] && g_headIdx >= 0 &&
                                g_bOk[g_headIdx]) {
                                // hands to the TOP of the head, not its centre
                                float sxh3 = chest[0]-pelP[0], syh3 = chest[1]-pelP[1],
                                      szh3 = chest[2]-pelP[2];
                                const float sm8 = sqrtf(sxh3*sxh3 + syh3*syh3 + szh3*szh3);
                                grabPt[0] = g_bPos[g_headIdx][0];
                                grabPt[1] = g_bPos[g_headIdx][1];
                                grabPt[2] = g_bPos[g_headIdx][2];
                                if (sm8 > 5.0f) {
                                    grabPt[0] += sxh3/sm8 * g_headTopCm;
                                    grabPt[1] += syh3/sm8 * g_headTopCm;
                                    grabPt[2] += szh3/sm8 * g_headTopCm;
                                }
                            } else if (g_legBody[g_impactBody]) {
                                const int gs = g_leftSide[g_impactBody] ? 0 : 1;
                                const int gt = g_thighIdx[gs], gc = g_calfIdx[gs];
                                if (gt >= 0 && gc >= 0 && g_bOk[gt] && g_bOk[gc]) {
                                    for (int a = 0; a < 3; a++)
                                        grabPt[a] = (g_bPos[gt][a] + g_bPos[gc][a]) * 0.5f
                                                    + g_belly[a] * 8.0f;
                                }
                            }
                            for (int i = 0; i < kMaxBodies; i++) g_grabbing[i] = false;
                            for (int i = 0; i < n2 && i < kMaxBodies; i++) {
                                if (!g_bOk[i] || i == g_impactBody) continue;
                                uint8_t* bi = ((uint8_t**)arr2)[i]; if (!bi) continue;
                                const float* tgt = nullptr; float acc = 0.0f;
                                float oscF = osc;
                                if (g_reacher[i]) {
                                    if (g_grabSide >= 0 &&
                                        (g_leftSide[i] ? 0 : 1) != g_grabSide) continue;
                                    // can this arm actually get there?
                                    {
                                        const int sh = g_clavIdx[g_leftSide[i] ? 0 : 1];
                                        const float* sp2 = (sh >= 0 && g_bOk[sh])
                                                           ? g_bPos[sh] : chest;
                                        const float rx = grabPt[0]-sp2[0];
                                        const float ry = grabPt[1]-sp2[1];
                                        const float rz = grabPt[2]-sp2[2];
                                        const float rd = sqrtf(rx*rx + ry*ry + rz*rz);
                                        if (rd > g_reachMax) continue;
                                        if (rd > g_reachEasy)
                                            oscF *= 1.0f - (rd - g_reachEasy) /
                                                           (g_reachMax - g_reachEasy);
                                    }
                                    // Hands pinned under the body cannot roll: the grab
                                    // pauses for the roll window, resumes on the side.
                                    if (rolling) continue;
                                    if (g_armBroken[g_leftSide[i] ? 0 : 1]) continue;
                                    // A hurt ARM does not grab itself: when the impact
                                    // part is on an arm, only the OPPOSITE hand reaches
                                    // for it and the hit arm stays passive (field rule).
                                    if (g_armBody[g_impactBody] &&
                                        g_leftSide[i] == g_leftSide[g_impactBody]) continue;
                                    // The hands never let go: a floored gate keeps the
                                    // grab continuous with a breathing squeeze on top.
                                    tgt = grabPt; acc = g_clutchAccel;
                                    oscF = 0.6f + 0.4f * osc;
                                    g_grabbing[i] = true;
                                }
                                // (the flail moved to its own bail-timed block above --
                                // legs are passive in the clutch)
                                if (!tgt) continue;
                                float d[3] = { tgt[0] - g_bPos[i][0], tgt[1] - g_bPos[i][1],
                                               tgt[2] - g_bPos[i][2] };
                                const float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                                if (dl < 2.0f) continue;
                                float sc = acc * amt * env * oscF / dl;
                                if (dl < 15.0f) sc *= dl / 15.0f;
                                float f[3] = { d[0]*sc, d[1]*sc, d[2]*sc };
                                // Downward components buy ground friction ("sticky"):
                                // curl along the ground and upward, never into it.
                                if (f[2] < 0.0f) f[2] *= 0.35f;
                                g_addForce(bi, f, true, true);
                            }
                        }
                    }
                }
            }
            // A ragdoll owns every blend weight: no scaling, no restoring (stale riding
            // weights over a ragdoll are as wrong as scaled ones -- the v3 lesson). The mesh
            // pointer is KEPT for the brace above; weights are re-captured on ragdoll exit.
            for (int i = 0; i < kMaxBodies; i++) g_owned[i] = false;
            g_apply = false;
            return;
        }
        if (g_wasRagdoll) {          // ragdoll just ended: forget, so everything re-captures
            g_wasRagdoll = false;
            DriveSpineCurl((uint8_t*)g_mesh, false);
            if (g_drivesFreed) {
                SetJointDrives((uint8_t*)g_mesh, true);
                g_drivesFreed = false;
            }
            ForgetMesh();
            return;
        }
        if (!g_on || g_fault || g_amount == 0 || !physOn) {
            if (g_mesh) RestoreAll();  // pickups, replays, the toggle -- restore and hand back
            return;
        }
        void* mesh = *(void**)((uint8_t*)sk + SK_MESH);
        if (!mesh) { if (g_mesh) RestoreAll(); return; }
        uint8_t* arr = *(uint8_t**)((uint8_t*)mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)mesh + CMP_BODIES + 8);
        if (!arr || n <= 0 || n > kMaxBodies) { if (g_mesh) RestoreAll(); return; }
        if (mesh != g_mesh || n != g_nBodies) {
            // New mesh (respawn, re-dress) -- fresh captures; nothing owned yet.
            g_mesh = mesh; g_nBodies = n;
            for (int i = 0; i < n; i++) { g_owned[i] = false; g_written[i] = -1.0f; }
            TwkLog("[body] mesh %p: %d physics bodies -- scaling their live blend weights "
                   "post-physics (the game re-authors them per frame; our write rides on top)",
                   mesh, n);
            g_classified = false; g_classifyAt = 0.0;
        }
        // ---- classify the REACHERS for the bail brace: hands + forearms, by bone name.
        // At level load the first attempt can find NOTHING (the SkeletalMesh pointer or the
        // name resolver is not ready yet -- field-logged "0 of 21" on the session's first
        // latch, fine 24s later), so a zero-reacher result is retried, not cached.
        if (!g_classified && t >= g_classifyAt) {
            g_classifyAt = t + 1.0;
            g_nReachers = 0; g_nHead = 0; g_nLegs = 0;
            g_pelvisBody = -1; g_spineFallback = -1; g_headIdx = -1;
            g_clavIdx[0] = g_clavIdx[1] = -1;
            g_thighIdx[0] = g_thighIdx[1] = -1;
            g_calfIdx[0]  = g_calfIdx[1]  = -1;
            g_footIdx[0]  = g_footIdx[1]  = -1;
            char names[192]; int used = 0; names[0] = 0;
            void* skm = *(void**)((uint8_t*)mesh + CMP_SKELMESH);
            uint8_t* info = skm ? *(uint8_t**)((uint8_t*)skm + SKM_REFSKEL_INFO) : nullptr;
            const int nBones = skm ? *(int*)((uint8_t*)skm + SKM_REFSKEL_INFO + 8) : 0;
            for (int i = 0; i < n; i++) {
                g_reacher[i] = false;
                uint8_t* bi = ((uint8_t**)arr)[i];
                if (!bi || !info) continue;
                const int boneIdx = *(short*)(bi + BI_BONE_INDEX);
                if (boneIdx < 0 || boneIdx >= nBones) continue;
                char nm[64];
                if (!GrindPop_FNameToString(info + (size_t)boneIdx * BONEINFO_STRIDE, nm, sizeof(nm)))
                    continue;
                for (char* c = nm; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                g_headBody[i] = false; g_legBody[i] = false; g_kneeBody[i] = false;
                g_footBody[i] = false;
                g_handBody[i] = strstr(nm, "hand") != nullptr;
                g_armBody[i]  = strstr(nm, "upperarm") != nullptr;
                // Excluded from "the part that hit": root/ik helpers, FEET (they arrive
                // last in every tumble and stole the grab -- log showed retarget-to-foot
                // on 6 of 8 bails), and HANDS (via g_handBody in the scans -- the brace
                // makes them the first contact on nearly every fall, so a hand hit
                // carries no information). Upper arms and forearms ARE valid hits: the
                // opposite hand grabs them, the hit arm goes passive (field rule).
                g_noImpact[i] = strstr(nm, "root") != nullptr || strstr(nm, "ik_") != nullptr ||
                                strstr(nm, "foot") != nullptr;
                g_leftSide[i] = strstr(nm, "_l_") != nullptr;
                if (strstr(nm, "hand") || strstr(nm, "forearm") || strstr(nm, "lowerarm")) {
                    g_reacher[i] = true; g_armBody[i] = true; g_nReachers++;
                    const int w = snprintf(names + used, sizeof(names) - used, "%s%s",
                                           used ? ", " : "", nm);
                    if (w > 0 && used + w < (int)sizeof(names) - 1) used += w;
                } else if (strstr(nm, "head") || strstr(nm, "neck")) {
                    g_headBody[i] = true; g_nHead++;
                    if (strstr(nm, "head")) g_headIdx = i;
                } else if (strstr(nm, "thigh") || strstr(nm, "calf") || strstr(nm, "foot") ||
                           strstr(nm, "leg")) {
                    g_legBody[i] = true; g_nLegs++;
                    g_kneeBody[i] = strstr(nm, "thigh") != nullptr;
                    g_footBody[i] = strstr(nm, "foot")  != nullptr;
                    const int sd = g_leftSide[i] ? 0 : 1;
                    if (g_kneeBody[i])      g_thighIdx[sd] = i;
                    else if (g_footBody[i]) g_footIdx[sd]  = i;
                    else                    g_calfIdx[sd]  = i;
                } else if (strstr(nm, "clavicle")) {
                    g_clavIdx[g_leftSide[i] ? 0 : 1] = i;
                } else if (g_pelvisBody < 0 && strstr(nm, "pelvis")) {
                    g_pelvisBody = i;
                } else if (g_spineFallback < 0 && strstr(nm, "spine")) {
                    g_spineFallback = i;
                }
                // AFTER the chain: it reads headBody/legBody, which the chain is what
                // sets. Computed before them it saw the freshly-zeroed values and
                // flagged every head, neck and LEG as "torso" -- the roll's push-up
                // was heaving the whole body straight up instead of the chest.
                g_torsoBody[i] = !g_armBody[i] && !g_headBody[i] && !g_legBody[i] &&
                                 strstr(nm, "root") == nullptr && strstr(nm, "ik_") == nullptr;
            }
            if (g_pelvisBody < 0) g_pelvisBody = g_spineFallback;
            g_classified = g_nReachers > 0;
            TwkLog("[body] brace reachers: %d of %d bodies (%s), %d head/neck, %d legs, "
                   "pelvis %s", g_nReachers, n,
                   g_nReachers ? names : "none found -- retrying every 1s until bone names resolve",
                   g_nHead, g_nLegs, g_pelvisBody >= 0 ? "found" : "MISSING");
        }
        g_apply = true;                 // the post-phys writer (foot_place detour) takes it from here

        const float speed = *(float*)((uint8_t*)an + AN_SPEED_RATIO);
        const bool grounded = *((uint8_t*)an + AN_GROUNDED_BF) != 0;
        // ---- RIDING ARM REFLEXES (see the knob comment). The physical-anim motors act
        // as the return spring; the forces are a lean, not a pose.
        const float armK = (float)g_armAmt / 100.0f;
        g_armFloorNow = 0.0f;
        if (armK > 0.0f && g_amount > 0) {
            g_armFloorNow = g_armFloor * (armK < 1.0f ? armK : 1.0f);
            ResolveAddForce();
            if (g_addForce) {
                const bool grinding = *((uint8_t*)an + AN_IS_GRINDING_BF) != 0;
                uint8_t* mc2 = (uint8_t*)mesh;
                int ri2 = *(int*)(mc2 + CMP_CST_READIDX); if (ri2 != 1) ri2 = 0;
                uint8_t* cst2 = *(uint8_t**)(mc2 + CMP_CST_ARR + (size_t)ri2 * 0x10);
                const int cstN2 = *(int*)(mc2 + CMP_CST_ARR + (size_t)ri2 * 0x10 + 8);
                const float* mq2 = (const float*)(mc2 + CTW_QUAT);
                const float* mp2 = mq2 + 4; const float* ms2 = mq2 + 8;
                float rp[8][3]; int rIdx[8]; int nR = 0;
                float pel[3] = { g_pos[0], g_pos[1], g_pos[2] };
                if (cst2 && cstN2 > 0) {
                    for (int i = 0; i < n && i < kMaxBodies; i++) {
                        if (!g_reacher[i] && i != g_pelvisBody) continue;
                        uint8_t* bi = ((uint8_t**)arr)[i]; if (!bi) continue;
                        const int bx = *(short*)(bi + BI_BONE_INDEX);
                        if (bx < 0 || bx >= cstN2) continue;
                        const float* bp = (const float*)(cst2 + (size_t)bx * 48 + 0x10);
                        float w2[3] = { bp[0]*ms2[0], bp[1]*ms2[1], bp[2]*ms2[2] };
                        QuatRotate(mq2, w2);
                        w2[0] += mp2[0]; w2[1] += mp2[1]; w2[2] += mp2[2];
                        if (i == g_pelvisBody) {
                            pel[0] = w2[0]; pel[1] = w2[1]; pel[2] = w2[2];
                        }
                        if (g_reacher[i] && nR < 8) {
                            rIdx[nR] = i;
                            rp[nR][0] = w2[0]; rp[nR][1] = w2[1]; rp[nR][2] = w2[2];
                            nR++;
                        }
                    }
                }
                // CARVE SWAY: centripetal reaction, sideways off the travel direction.
                const float hvx = g_vel[0], hvy = g_vel[1];
                const float hsp = sqrtf(hvx*hvx + hvy*hvy);
                float sway[3] = {0, 0, 0};
                if (hsp > 60.0f && fabsf(g_yawRate) > 0.3f) {
                    const float ax = hvy / hsp, ay = -hvx / hsp;   // right of travel
                    const float sgn = (g_yawRate > 0.0f ? 1.0f : -1.0f) * g_swaySign;
                    float mag = g_swayGain * fabsf(g_yawRate) * hsp;
                    if (mag > g_swayMax) mag = g_swayMax;
                    sway[0] = ax * sgn * mag; sway[1] = ay * sgn * mag;
                }
                const bool spreadOn = !grounded || grinding;
                const float spreadMag = g_spreadAccel * (grinding && grounded ? 0.6f : 1.0f);
                const float landMag = (grounded && g_pulse > 0.05f)
                                      ? g_pulse * g_landAccent : 0.0f;
                for (int k = 0; k < nR; k++) {
                    const int i = rIdx[k];
                    uint8_t* bi = ((uint8_t**)arr)[i]; if (!bi) continue;
                    const float bs = g_handBody[i] ? 1.0f : 0.55f;
                    float f2[3] = { sway[0], sway[1], 0.0f };
                    if (spreadOn) {
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
        const bool grinding = (*((uint8_t*)an + AN_IS_GRINDING_BF) |
                               *((uint8_t*)an + AN_IS_GRINDING_BF + 1)) != 0;
        // ---- the landing surge ----------------------------------------------------------------
        if (!grounded) { if (!g_wasAir) { g_wasAir = true; g_airSince = t; } }
        else {
            if (g_wasAir && t - g_airSince > 0.15) {
                float drop = *(float*)((uint8_t*)an + AN_LAND_DROP);
                if (!(drop >= 0.0f)) drop = 0.0f; else if (drop > 1.0f) drop = 1.0f;
                const float give = g_landMinGive + (g_landGive - g_landMinGive) * drop;
                if (give > g_pulse) g_pulse = give;
            }
            g_wasAir = false;
        }
        if (g_pulse > 0.0f) {
            g_pulse -= dt * 1000.0f / (float)g_recoverMs;
            if (g_pulse < 0.0f) g_pulse = 0.0f;
        }
        // ---- physics-visibility target, then the ease -----------------------------------------
        float target = 1.0f;
        if (!grounded) target += g_airMore;
        target -= g_brace * (speed < 0.0f ? 0.0f : (speed > 1.0f ? 1.0f : speed));
        target -= g_coil * PopProbe_CrouchDepth01();
        if (grinding) target -= g_railSet;
        target *= (1.0f + g_pulse);                         // impact: physics surges through
        target = 1.0f + (target - 1.0f) * ((float)g_amount / 100.0f);
        if (target < 0.1f) target = 0.1f; else if (target > 3.0f) target = 3.0f;
        const float tau = (float)g_tauMs / 1000.0f;
        g_feel += (target - g_feel) * (tau > 0.0f ? (1.0f - expf(-dt / tau)) : 1.0f);

        if (g_log) {
            static double said = 0.0;
            if (t - said > 1.0) { said = t;
                TwkLog("[body] feel %.2f (target %.2f) | speed %.2f air %d grind %d crouch %.2f "
                       "pulse %.2f (writes land post-physics; see the apply line)",
                       g_feel, target, speed, grounded ? 0 : 1, grinding ? 1 : 0,
                       PopProbe_CrouchDepth01(), g_pulse);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault = 1;
        TwkLog("[body] faulted -- reactive body paused until the next character loads "
               "(your setting is untouched)");
    }
}

// The write that survives. Runs inside the animation update (foot_place's detour), which is
// AFTER the Blueprint's tick re-authored the per-body weights this frame and BEFORE the pose
// blend consumes them -- the same frame-order lesson as catch_level's post-phys assert. The
// value read here IS the game's live authored weight for this frame (its own state machine at
// work: ~0.52 avg idle, most bodies zeroed at speed), so our feel multiplies their modulation
// instead of fighting it; bodies the game holds at zero stay untouched.
void BodyFeel_PostPhysApply() {
    if (!g_apply) return;
    g_apply = false;                    // one apply per pump verdict; re-armed every pump tick
    if (!g_mesh || !g_nBodies) return;
    __try {
        uint8_t* arr = *(uint8_t**)((uint8_t*)g_mesh + CMP_BODIES);
        const int n = *(int*)((uint8_t*)g_mesh + CMP_BODIES + 8);
        if (!arr || n != g_nBodies) return;
        int owned = 0; float authoredSum = 0.0f;
        for (int i = 0; i < n && i < kMaxBodies; i++) {
            uint8_t* bi = ((uint8_t**)arr)[i];
            if (!bi) continue;
            float* w = (float*)(bi + BI_BLEND_WEIGHT);
            const float cur = *w;
            if (!(cur >= 0.0f && cur <= 1.0f)) continue;
            // Capture ONLY when the game re-authored the weight (cur differs from OUR last
            // write). v3 captured unconditionally on the "the BP writes every frame" assumption,
            // and the moment that writer paused (ragdolls) our own output became the next
            // "authored": weight x feel x feel x ... -> zero in half a second -> blend weight 0 =
            // pure animation = the bail frozen in the skating stance (field report). With the
            // self-heal the write is idempotent while the game's writer is quiet.
            if (!g_owned[i] || fabsf(cur - g_written[i]) > 0.0001f) {
                g_authored[i] = cur;
                g_owned[i] = cur > 0.0f;
            }
            float out;
            if (!g_owned[i] || g_authored[i] <= 0.0f) {
                // "off stays off" -- EXCEPT the arms: the game zeroes most weights at
                // speed, which is exactly the luggage look. The floor keeps the arm
                // motors+physics alive so the riding reflexes have a body to move.
                if (!(g_armBody[i] && g_armFloorNow > 0.0f)) continue;
                out = g_armFloorNow;
            } else {
                out = g_authored[i] * g_feel;
                if (g_armBody[i] && out < g_armFloorNow) out = g_armFloorNow;
            }
            if (out < 0.0f) out = 0.0f; else if (out > 1.0f) out = 1.0f;
            *w = out; g_written[i] = out;
            owned++; authoredSum += cur;
        }
        if (g_log) {
            static double said2 = 0.0;
            const double t = NowS();
            if (t - said2 > 1.0) { said2 = t;
                TwkLog("[body] apply: feel %.2f on %d/%d bodies (game-authored avg %.2f this frame)",
                       g_feel, owned, g_nBodies, owned ? authoredSum / owned : 0.0f);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fault = 1;
        TwkLog("[body] post-phys apply faulted -- reactive body paused until the next character "
               "loads (your setting is untouched)");
    }
}
