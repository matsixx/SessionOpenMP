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
// SessionTweaks -- MANUAL CATCH. Two fixes, both manual-only by construction (auto catch is tuned to
// look a specific way and must not change):
//
// * THE WINDOW. The feet can only take the board within `BoardFlipPreCatchAngle` /
//    `BoardRotationPreCatchAngle` of its target (USkateboardMovementComponent::UpdateFeetCatchInfo).
//    Both ship 60 deg on every def -- at a measured ~1960 deg/s flip that is a ~31 ms window, about
//    two frames. Widened by CatchWindowMult while in manual, restored the moment it is not.
//
// * THE EATEN INPUT. While `_shouldCancelSingleStickInputForDarkSlide` is nonzero,
//    CheckForCatchOrient_Default SKIPS the single-stick verdict wholesale (0x10469d1) -- no catch,
//    no bail, no feedback. The dark-slide input window overlaps the board-level moment, so it eats
//    precisely the well-timed griptape-up flicks. The flag is not a bug, it is a RESERVATION: a
//    darkslide is a two-stick input and human thumbs never land on the same frame, so the game holds
//    the first stick back to give the second one time to arrive. A blanket mask would therefore make
//    darkslides require frame-perfect sticks.
//    The discriminator is the BOARD'S FLIP ANGLE: a darkslide catch only ever happens with the board
//    near GRIP-DOWN (180 deg mid-flip), while a griptape-up catch press happens with the board far
//    from it. So while the reservation is active, the flag is masked (save/zero/call/restore around
//    the one call) whenever the board is farther than `DarkslideZoneDeg` from grip-down -- the press
//    then resolves NATIVELY on its edge frame with original timing, identical to playing with Dark
//    Slides off. Near grip-down the reservation is honoured completely stock, so darkslides are
//    untouched.
//    Timing-based models were field-tested and do NOT work; do not resurrect them: a blanket mask
//    breaks darkslides (it demands same-frame sticks); a grace window requires the stick to be STILL
//    HELD at expiry, which real flicks are not; and a flick-vs-hold model that writes the verdict
//    through out5 on the release frame evaluates at a later board angle, so outcomes scatter between
//    an instant bad catch, a catch toward the NEXT griptape alignment, and nothing at all.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "catch_tweaks.h"
#include "flip_speed.h"
#include "catch_level.h"
#include "foot_place.h"
#include "foot_steer.h"      // the filtered steer, so a driven board agrees with the feet
#include <cmath>

static bool CatchIsGoofy();   // defined with the catch-orient hook, used by the flick log above it
static bool CatchIsSwitch();
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    IAH_SKATER              = 0x08,    // InAirHandler -> _skater
    IAH_CATCH_ORIENTS_DB    = 0x10,    // InAirHandler -> _catchOrientsDb
    IAH_TRICKS_DB           = 0x28,    // InAirHandler -> _tricksDb (UTricksDatabase*)
    // `_catchOrientInputMode` (+0x30) is NOT the menu's Catch Mode. The only catch menu item
    // (FMenuKey::PAGEITEM_CatchMode) drives ASkaterCharacterBase::SetCatchMode, whose whole body is
    // `mov [rcx+0x63d], dl`, i.e. _catchMode on the SKATER. Gate on that; +0x30 does not move when
    // the setting changes.
    IAH_CATCH_ORIENT_INPUT  = 0x30,    // ECatchOrientInputMode (NOT the setting; context/scheme)
    IAH_INPUT_HANDLER       = 0x20,    // InAirHandler -> _inputHandler (PDB)
    IH_RAW_LEFT             = 0x24,    // InputHandler -> _frameRawLeftInput  (scoop_speed's offsets)
    IH_RAW_RIGHT            = 0x2c,    //   "             _frameRawRightInput
    IAH_LAST_ORIENT_STATE   = 0x31,
    // +0x33/+0x34 are written in CheckForCatchOrient's EPILOGUE from that frame's inputs -- "stick
    // was pushed LAST frame" trackers, making the manual stick branch an EDGE DETECTOR: catch
    // eligibility exists only on the single frame the stick first crosses the deadzone.
    IAH_HAD_FRONT_INPUT     = 0x33,
    IAH_HAD_BACK_INPUT      = 0x34,
    IAH_DS_CANCEL           = 0x60,    // _shouldCancelSingleStickInputForDarkSlide (the input-eater)
    IAH_DS_WINDOW           = 0x61,    // _isInDarkSlideWindow
    CODB_INPUT_DEADZONE     = 0x30,    // UCatchOrientsDatabase::CatchOrientInputDeadZone (ships 0.2)
    // ---- the game's built-in boned ollie lives HERE, in authored catch-orient data.
    // Measured before it was found: holding both sticks in the air pushes the BOARD up to ~36 cm
    // forward of the skater while the character mesh does not move at all, decaying back as the
    // sticks centre; the legs follow only because the feet are IK'd to the deck. That is why it
    // looks like a leg pose and why no foot/bone/stance symbol exists to find.
    CODB_ORIENTS            = 0x70,    // TArray<FCatchOrientDefinition> (data +0x0, Num +0x8)
    CO_DEF_STRIDE           = 180,     // sizeof(FCatchOrientDefinition)
    CO_BACK_SETTINGS        = 0x4,     // FCatchOrientDefinition::BackSideSettings  (88 B)
    CO_FRONT_SETTINGS       = 0x5c,    // FCatchOrientDefinition::FrontSideSettings (88 B)
    // FCatchOrientSettings::{Left,Right}BoardRelativeOffset_{RGS,SWS} -- FVectors, the displacement
    COS_OFF_L_RGS = 0x28, COS_OFF_R_RGS = 0x34, COS_OFF_L_SWS = 0x40, COS_OFF_R_SWS = 0x4c,
    // USkaterAnimInstance, reached through FootPlace_AnimInstance(). IsBoardFlipping /
    // IsBoardRotating: neither set for a whole air = an OLLIE (incl. switch/fakie/nollie).
    AN_FLIPPING             = 0x495,   AN_ROTATING = 0x496,
    AN_GROUNDED             = 0x5fa,
    // IsSkatingGoofy. In goofy the front foot is the RIGHT one, so the physical stick -> catching
    // foot mapping inverts. MEASURED across all four combinations: the catch takes the wrong foot in
    // goofy AND goofy-switch, and the right one in both regular variants -- so goofy flips it and
    // switch does NOT. (Switch reverses which END of the board leads; it does not swap your feet.)
    AN_IS_GOOFY             = 0x304,
    AN_SKATER               = 0x608,   // USkaterAnimInstance::_skater -- to reach the stance options
    // bit 0 = _isLeftRightFootSkater, the CONTROL-SCHEME option "sticks bind to left/right foot"
    // rather than front/back. CheckForCatchOrient swaps its two stick vectors on
    //     swap = (IsSkatingGoofy != IsSkatingSwitch) && (skater[0x651] & 1)
    // so the front/back args handed to _Default are ALREADY stance- AND setting-resolved. Any
    // hand-rolled stance rule that ignores this bit is only correct for one value of a user setting.
    SK_STANCE_OPTS          = 0x651,
    SK_CATCH_MODE           = 0x63d,   // ECatchMode -- THIS is the menu's Catch Mode
    SK_CATCH_ORIENT_STATE   = 0x63e,   // ECatchOrientState -- nonzero = a catch actually ENGAGED
    SK_BOARD                = 0x568,   // ASkaterCharacterBase -> _skateboard (ASkateboardEx*)
    // The game's OWN foot-catch bookkeeping, straight out of the PDB. This is what "my foot
    // actually attached to the board" means to the game, so it is measurable rather than inferred:
    // the movement component carries a per-foot FCatchFootInfo { ECatchFootType type; float ratio; }
    // and the anim instance carries the two booleans the pose reads.
    MC_LFOOT_CATCH          = 0x538,   // USkateboardExMovementComponent::_leftFootCatchInfo
    MC_RFOOT_CATCH          = 0x540,   //   "                            _rightFootCatchInfo
    FCFI_RATIO              = 0x004,   // FCatchFootInfo::CatchRatio (type byte sits at +0)
    // The catch's TIMING, on the same component. These read as garbage (-1.6e25) through
    // `skater+0x550`, which is a different class -- reach them through CatchLevel's learned
    // component, whose owner back-pointer at +0x340 proves it is ours.
    MC_CATCH_TOTAL_TIME     = 0x54c,   // _catchTotalTime
    MC_CATCH_FORCE_TIME     = 0x550,   // _catchForceFeetCatchTime -- when the feet are FORCED down
    MC_CATCH_TO_BOARD_DELAY = 0x554,   // _catchFeetToOnSkateboardDelay
    MC_BOARD_FLIP_TARGET    = 0x778,   // _boardFlipTargetAngle
    MC_BOARD_FLIP_CUR       = 0x77c,   // _boardFlipCurrentAngle
    AN_IS_SWITCH            = 0x303,   // USkaterAnimInstance::IsSkatingSwitch -- front foot swaps
    AN_IS_LANDING           = 0x5fd,   // USkaterAnimInstance::IsLanding -- "the board reaches the
                                       // ground", which is the release the second-foot hold wants
    AN_HAS_LFOOT_CATCH      = 0x313,   // USkaterAnimInstance::HasLeftFootCatchOrient
    AN_HAS_RFOOT_CATCH      = 0x314,   //   "                  HasRightFootCatchOrient
    MC_BOARD_FLIP_RATE      = 0x774,   // USkateboardExMovementComponent::_boardFlipRate. Reached
                                       // through CatchLevel's learned component -- skater+0x550 is a
                                       // different object and every Ex field reads zero through it.
    BOARD_FLIPPER           = 0x4e8,   // ASkateboardEx -> _flipper (UStaticMeshComponent, the DECK
                                       // mesh that visually flips -- its transform is the truth)
    COMP_CTW_QUAT           = 0x1c0,   // USceneComponent -> ComponentToWorld rotation quat (x,y,z,w)
    // mc+0x798 _inAirTime and +0x79c _inAirPopTime both stay 0 through popped-trick airs, and
    // mc+0x77c _boardFlipCurrentAngle always reads 0. Do not gate on these movement-component state
    // fields; measure the rendered geometry instead (the flipper component's quat, TwkActorZ).
    TDB_FLIP_TRICKS         = 0x2c0,   // UTricksDatabase -> _flipTricks TArray<UFlipTrickDefinition*>
    DEF_FLIP_PRECATCH_ANGLE = 0x258,   // UFlipTrickDefinition::BoardFlipPreCatchAngle     (default 60)
    DEF_ROT_PRECATCH_ANGLE  = 0x25c,
    // ALIVE, despite an earlier note in this project calling it dead data. It is the divisor for
    // the SECOND foot's catch ratio in USkateboardExMovementComponent::UpdateFeetCatchInfo
    // (0xfdee40): `[rdi+0x54c] _catchTotalTime / [rsi+0x268]`, clamped to 1, written to that foot's
    // CatchRatio. The "no references" reading came from a linear disassembler that had stopped after
    // ~20 lines of a 0xb66-byte function.
    // NOT WRITTEN. OtherFootCatchTime -- kept only as the documented offset; see the note above
    // the catch-window walk for why scaling it was removed.
    DEF_OTHER_FOOT_TIME     = 0x268,   // UFlipTrickDefinition::OtherFootCatchTime (ships 0.15)
    // The deck's rendered flip roll, decoded from UpdateBoardTargetFlipAndScoop (0xfddf40): the
    // Roll of the FRotator at comp+0x620 (Pitch there is catch_level's setpoint field, Yaw +0x624).
    // The flip integrator writes it while a flip runs; the game's catch roll-align slerps it toward
    // CatchTargetRollAngle -- but only once a foot's CatchRatio passes 0.9 (an rdata constant,
    // shared, not patchable). So during the attach ramp NOTHING drives the roll, which is the
    // "foot lands, THEN the board levels" look the foot-level drive below removes. Roll accumulates
    // degrees across the flip (it is not normalised), so any target must be wrap-aware.
    MC_ANIM_ROLL            = 0x628,   // _localAnimationRotator.Roll -- the rendered deck roll
    SK_CURRENT_TRICK_DEF    = 0x590,   // ASkaterCharacterBase -> the trick def the align reads live
    DEF_CATCH_TGT_ROLL      = 0x270,   // UFlipTrickDefinition::CatchTargetRollAngle (ships 0)
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int   g_catchFix    = 1;       // widen the manual-catch window at all
static float g_catchMult   = 2.0f;    // multiplier on the per-trick pre-catch angles
// Which ECatchMode value means MANUAL. The default -1 means "unknown, never widen": the enumerator
// names are not in the build, and a wrong guess would silently widen AUTO catch, which this module
// promises never to touch. Read the value off the mode-change log line and set it in the ini
// (manual is 2 on the current build).
static int   g_manualMode  = -1;
static int   g_catchBeatsDS = 1;      // press-time catches during the darkslide reservation
static int   g_dsAngleDeg  = 60;      // DarkslideZoneDeg: the reservation is honoured only while
                                      // the board is within this many degrees of grip-down (180);
                                      // elsewhere it is masked = native press-time catching
static int   g_catchDiag   = 0;       // log every catch decision (verbose; for future catch work)
// The game's own boned ollie, ADJUSTED rather than replaced. It is authored catch-orient data --
// board-offset vectors that shove the deck forward while both sticks are held, the legs following
// because the feet are IK'd to the deck. Driving those vectors from the sticks instead was built and
// rejected; scaling what the game already does keeps its timing, its smoothing and its gating and
// changes only how far and in which direction. Data-only, fully restored when back at stock.
// 0 removes the bone entirely, 100 is stock. The add is in cm on the offset's raw components --
// which one reads as "up" is a question for the headset, exactly like the foot axes were.
// OtherFootCatchTime (+0x268) is left STOCK. Scaling it was tried and removed at the
// user's request -- "it doesn't seem to help much". It gates only when the second foot
// ATTACHES, so it cannot stop that foot travelling, and the large values needed to look
// like a hold left it still descending at touchdown and clipped it through the deck.
// `_catchTotalTime` counts UP from zero -- it is an elapsed clock, not a budget.
// What the same measurement DID show: the catch ORIENT is one-footed (`L orient=YES R orient=no`
// every trick) but BOTH feet are handed a catch TYPE immediately (`footType L=1 R=2`). So the other
// foot going down is a per-foot TYPE assignment, not a timer -- clear it and it has nothing to do.
// Held until `IsLanding`, which is the user's own release condition: "until the board actually
// reaches the ground".
// CatchWithStickHeld -- let a catch flick register while the OTHER stick is held (mid-scoop, or any
// hold). A catch orient is a single-stick gesture, so the game refuses it outright when both sticks
// are deflected; this presents the held one as centred for the duration of the decision only.
static int   g_catchTwoStick = 1;
// How many further calls the held stick stays masked once a catch verdict has been produced.
// MEASURED WHY: with the mask on, the decision returns the RIGHT foot (out5=1, front) -- but
// _Default only writes a verdict, and the dispatcher engages the catch after it returns. Restoring
// the scoop immediately let the dispatcher re-derive from the live sticks and take the back foot.
// 0 restores immediately (the old behaviour, for A/B).
static int   g_heldMaskFrames = 2;
// CatchWithFlickedFoot -- catch with the foot whose stick was flicked, instead of whichever one the
// game picks. See the SetCatchOrient hook below for the decode; this is the on/off switch.
static int   g_flickFoot = 1;
// WHICH PHYSICAL STICK made the fresh flick: 0 = none/ambiguous, 1 = LEFT, 2 = RIGHT, plus a
// freshness countdown in frames. Physical, not front/back: front/back is stance-relative naming and
// the dispatcher can hand the two vectors over either way round, so identity is taken by POINTER
// against the InputHandler's own left/right fields.
static int   g_flickPhys = 0, g_flickFresh = 0;
// SetCatchOrient is called EVERY FRAME for the whole duration of a catch, not once at the start
// (measured: ~30 consecutive calls per catch). So the corrected foot must be LATCHED for the whole
// catch -- the first cut forced it only while a fixed freshness window lasted, the window expired
// mid-catch, and the orient reverted to the game's foot with the catch still running. That reads in
// the headset as BOTH feet catching, one after the other.
// 0 = not latched, else the ECatchOrientState being held (1 = left, 2 = right).
static int   g_flickLatch = 0;
static int   g_latchIdle  = 0;       // frames since the last non-zero orient, to release the latch
static int   g_flickInvert = 0;      // CatchFlickFootInvert, if the stance parity reads backwards
static LONG  g_uiFlickFixes = 0;     // how many orients this has corrected, for the menu
// CatchHoldPose: an engaged one-foot catch cannot be broken by the second stick (base-game bug --
// the user's repro: catch the back foot, then move the front stick and BOTH feet detach and float
// above the deck mid-air until landing). Two candidate mechanisms, both closed at this setter (the
// ONLY writer of the skater's orient state and pitch/yaw ratio fields, verified by field xref):
//   * the second stick flips the orient into a two-stick state (5/10/11) -- the boned ollie's
//     authored pose, which shoves the board away from the feet by design;
//   * the second stick modulates the pitch/yaw RATIOS on the same state, blending toward an
//     authored pose variant with the feet lifted.
// While held, incoming two-stick states are narrowed back to the engaged foot. The ratios are left
// ALONE -- they are how the catch is steered, see the note at the narrowing. Gated to real trick airs
// (CatchAirIsTrick) so the boned
// ollie's own two-stick pose is never touched, and darkslide states pass through and drop the
// hold. Each rewrite kind edge-logs once per catch, so which mechanism actually fired is readable
// straight from the log.
// MEASURED (the round after CatchHoldPose shipped): the second stick does not MODIFY the engaged
// catch at all -- the steer samples show state 2 -> 0 -> 1: the first catch ENDS and the second
// input starts a FRESH catch on the other foot. A fresh catch restarts the whole approach cycle
// (feet lift to the coming-down pose, ratios reset), which is exactly the reported float. So the
// state-narrowing and ratio-pinning below never fire on this bug; the working half is ONE CATCH
// PER AIR: once a catch has been taken on a trick air and has ended, any further catch input that
// air is swallowed (the board is already caught -- nothing good can follow). Darkslide conversions
// pass; ollies are exempt (CatchAirIsTrick); the record clears on grounding.
static int   g_holdPose   = 1;       // CatchHoldPose
static int   g_holdState  = 0;       // the engaged one-foot state being held (0 = not holding)
static int   g_holdLoggedState = 0;  // one edge log per catch for the state-narrowing
static LONG  g_uiHoldFixes = 0;
static int   g_airCaughtFoot = 0;    // nonzero = a catch was taken THIS AIR (one catch per air)
static int   g_airCatchZeros = 0;    // consecutive state-0 setter calls since it -- >= 2 = it ended
                                     // for real (a single-frame 0 blip inside a catch must not arm
                                     // the swallow; the setter can also simply STOP being called,
                                     // which the latch idle release covers)
static int   g_airSwallowLogged = 0;
// "Was THIS AIR a flip or rotation" -- maintained EVERY FRAME in CatchTweaks_PumpFrame, not
// from the hook (which only runs on a non-zero orient and so could leave it stale).
static int   g_airWasTrick = 0;
static int   g_secondFootHold = 0;   // SecondFootHold
static int   g_boneScale = 100;             // BoneScalePct
static int   g_boneAdd[3] = { 0, 0, 0 };    // BoneAddX / BoneAddY / BoneAddZ
// FlipCatchTrace -- read-only: measures how far the BOARD turns across a catch, so "it finished a
// second flip" is a number instead of an impression. Costs nothing when no catch is pressed.
static int   g_flipTrace   = 0;   // FlipCatchTrace -- diagnostic, off by default
static int   g_flipTraceVerbose = 0;   // FlipCatchTraceVerbose -- per-frame lines as well
// CatchStopsFlip: once a catch registers, the board stops at the first grip-up instead of carrying
// on for another whole revolution. Measured behaviour this restores: on a good catch the deck runs
// out to grip-up and stops (press + travelled ~= 185 every time); on a bad one it does that AND a
// further ~360. Nothing here predicts WHICH reps go wrong -- it makes the wrong ones end where the
// right ones already do.
// CatchAnyRevolution: a caught board ends its flip FLAT under your foot.
//
// MEASURED, not theorised. The game sets _boardFlipTargetAngle to 360 at pop and EXTENDS it to 720
// the moment the deck runs past 360 without a finished catch -- every 720 in the logs arrived that
// way, never from the input. That extension is what breaks the catch: each foot's CatchRatio is
// computed against the target, so raising it to 720 mid-catch drops the ratio to 0, no foot is ever
// planted, and the board keeps spinning -- which guarantees the overshoot again. Losing that race
// once locks you out of the catch entirely, which is a large part of why manual catch feels bad.
//
// So while a catch is engaged and the deck is near enough to grip-up, the flip target is re-aimed at
// the FLAT orientation: target = current + (however far the deck still has to roll). Two things fall
// out of that one write. The goalpost move to 720 is cancelled, so the ratio completes and the foot
// attaches. And the deck finishes LEVEL rather than frozen at whatever roll it happened to be at --
// the board levelling out as the foot comes down is just the tail of the flip, so it has to be aimed
// at flat rather than cut off. Aiming rather than clamping to 360 is deliberate: the rotation step is
// (target - current), so a target behind the deck would spin it BACKWARDS onto the mark, which is
// very likely why the game extends to 720 in the first place.
//
// Nothing is written while not catching: an uncaught board still runs on and still becomes a genuine
// double flip. This only refuses to let the game move the goalposts during a catch.
static int   g_anyRev       = 1;      // CatchAnyRevolution
static int   g_anyRevDeg    = 60;     // CatchAnyRevDeg -- how sideways a catch still gets levelled.
                                      // Bigger = more of a sideways catch is rolled flat under the
                                      // foot; smaller = only near-flat catches are touched.
// CatchFootLevelsBoard: on a parked over-rotation catch, the deck's roll is driven toward level in
// LOCKSTEP with the foot's own CatchRatio -- board angle becomes a function of foot descent, so the
// level-out reads as the foot pressing the board flat instead of the board levelling by itself
// afterwards. The game's own roll-align cannot do this: it only starts once a ratio passes 0.9,
// i.e. after the foot has already landed on the tilted deck. Reads the ratios, never writes them
// (forcing a CatchRatio kills the catch outright -- measured). Hands over to the game's align at
// 0.9 with ~nothing left to do.
static int   g_footLevel    = 1;      // CatchFootLevelsBoard
// CatchUnstick: base-game bug repair. Catching with one foot and then flicking the OTHER stick
// (and sometimes an ordinary catch) wedges the feet floating above the deck in the catch pose,
// ride after ride, until a bail. Decoded: the only code that force-reseats the feet is a 0.2 s
// settle window (comp+0x554) armed EXCLUSIVELY on entering movement mode 7 in
// OnMovementModeChanged -- a bail's remount passes through it, an ordinary landing never leaves
// the mode, so a wedged catch has no path back. The repair runs the game's own two cleaners from
// the pump once the pose has been stuck on the ground past the grace: end the orient through the
// real setter (state 0, the call every normal catch ends with) and re-arm the settle window --
// exactly what the bail did, without the bail. Darkslides are a legitimate sustained grounded
// catch state and are exempt.
// CatchNeedsFreshFlick: base-game bug repair, ini kill-switch only. A stick HELD since before the
// pop resolves as a catch on the pop frame itself off ramps (measured: three ramp nollie heels all
// engaged at +233 ms after trick selection -- the selection-to-pop lag exactly -- while the same
// held-flick style on flat engages at ~590 ms from a fresh, deliberate flick; releasing the held
// stick mid-freeze resumed the flip, closing the loop). A held-over deflection is not catch
// intent: every working catch in the field logs arrives as a fresh flick. So while a flip trick is
// running, a catch orient may only ENGAGE if some stick made a fresh EDGE after the pop -- the
// trick's own flick (which lands at selection time) does not count. Once admitted, the catch is
// latched admitted until it ends, so a brief flick whose deflection has already relaxed cannot be
// vetoed mid-catch. Darkslide states are exempt as always; ollies never stamp the trick clock, so
// their board-control orients are untouched.
static int   g_needFlick    = 1;      // CatchNeedsFreshFlick
static long long g_lastEdgeQpc = 0;   // QPC of the last raw stick edge (either stick)
static long long g_deflStartL = 0, g_deflStartR = 0;   // QPC when each stick's CURRENT deflection
                                                       // began; 0 = centred right now
static int   g_vetoAdmitted = 0;      // this catch passed the veto once -- stop checking
static long  g_vetoLogged   = -1;     // trick serial the veto last logged for
// CatchMinSpinDeg: a catch may not ENGAGE until the board has actually turned this far into the
// trick. The fresh-flick veto covers the manual case -- it asks "has a new stick edge arrived?" -- but
// AUTO catch has no flick to time, so nothing stopped a very slow flip being caught before the deck
// had begun to spin. Degrees, not milliseconds, so it holds however slowly the board turns.
//
// This is NOT the reverted 2.62.2 guard. That one un-ended the flip AFTER the catch had registered, by
// refusing to park _boardFlipTargetAngle; it treated the symptom and was pulled. This refuses the
// engage itself, in the same place and the same way as the flick veto -- the mechanism that worked.
// AUTO CATCH ONLY (AutoCatchActive) -- manual has the flick veto.
static int   g_minSpinDeg   = 45;     // 0 = off
// CatchMaxCutDeg: how much unfinished rotation "foot always attaches" may throw away. Past this the
// flip is left alone to finish on its own. 0 = no limit (the old behaviour).
// AUTO CATCH ONLY (AutoCatchActive) -- on manual an over-rotation must still level out under the foot.
static int   g_maxCutDeg    = 180;
static float g_trickTravelDeg = 0.0f; // degrees the board has turned since the current trick started
static int   g_spinLogged   = -1;     // trick serial the spin gate last logged for
static int   g_unstick      = 1;      // CatchUnstick -- ini kill-switch only, no menu row: it is
                                      // a bug fix, always on (like CatchHoldPose and FootFixShoeHeight)
static int   g_unstickMs    = 1000;   // CatchUnstickMs -- how long the pose must be stuck on the
                                      // ground before the repair fires. Generous on purpose: a
                                      // normal catch resolves within a moment of landing.
static int   g_stopFlip     = 1;      // CatchStopsFlip
static int   g_stopFlipDeg  = 168;    // CatchStopFlipDeg -- how close to grip-up counts as arrived
static int   g_snapMs       = 90;     // CatchSnapMs -- how long a caught board gets to finish its
                                      // remaining travel. Lower = snappier catch, higher = softer.
static int   g_snapMaxDeg   = 200;    // CatchSnapMaxDeg -- past this the catch is not near home and
                                      // the board finishes on its own; no snap at all.
static int   g_snapMaxBoost = 3;      // CatchSnapMaxBoost -- never speed the deck up by more than
                                      // this multiple of the rate it already had.
// CatchFlipAxis: which LOCAL axis of the flipper component is the board's long axis -- the one a
// flip rotates about. 0 = X (default), 1 = Y, 2 = Z. If flips ever read wrong, the stop log prints
// all three so the right one is a single setting away rather than a rebuild.
static int   g_flipAxis     = 0;
static volatile LONG g_uiFlipStops = 0;
static volatile LONG g_faults = 0;
static volatile LONG g_uiCatchMode = -1, g_uiOrientMode = -1;   // last observed mode pair
static volatile LONG g_uiCatchWide = 0, g_uiCatchDefs = 0;      // widened? on how many defs?
static volatile LONG g_uiDsMasked = 0;                          // masked stretches (press-time zones)
static volatile LONG g_uiDsHonored = 0;                         // grip-down stretches left stock

int CatchTweaks_ManualMode() { return g_manualMode; }

// Both 3.18 guards below exist for AUTO catch only and are locked to it. Manual catch was already
// right: the fresh-flick veto covers its early-engage case, and on manual an over-rotation is caught
// deliberately -- the foot is supposed to attach and level the deck, which is exactly what
// CatchMaxCutDeg was refusing to do (field: kickflip over-rotations stopped levelling out).
// `g_uiCatchMode` is the live menu setting, so switching modes in-game switches the guards with it.
// Manual is only KNOWN once it has been learned (or set in the ini); until then assume auto, so a
// player who never touches manual still gets the fix. The learn fires on the first manual-mode frame.
static bool AutoCatchActive()
{
    const int manual = g_manualMode;
    if (manual < 0) return true;                       // not yet learned -- assume auto
    return (int)g_uiCatchMode != manual;
}

// The live skater, published for other modules' pump-side polling. Updated OUTSIDE both guarded
// blocks in hkCanCatchOrient: the sampler's g_samplerOK and the widen gate's handler each disable
// their own block on a fault, and a consumer that lost the skater along with them would present as
// a broken feature rather than a disabled probe.
static void* g_lastSkater = nullptr;
void* CatchTweaks_Skater() { return g_lastSkater; }

static double DsNow() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
// Skater world-POSITION ring, pushed (throttled to ~125 Hz) from the CanCatchOrient hook. The max Z
// over the last 1.5 s is the apex of the air that is ending; position deltas give run_out the true
// travel velocity, because the component's Velocity field is frozen while skating (see the header).
// Game thread only.
static const int kZRing = 256;
static float  g_zRing[kZRing];
static float  g_pxRing[kZRing], g_pyRing[kZRing];
static double g_zRingT[kZRing];
static int    g_zHead = 0;
static double g_zLastPush = -1000.0;
float CatchTweaks_RecentMaxZ() {
    const double cutoff = DsNow() - 1.5;
    float best = -999999.0f;
    for (int i = 0; i < kZRing; i++)
        if (g_zRingT[i] > cutoff && g_zRing[i] > best) best = g_zRing[i];
    return best;
}
bool CatchTweaks_TravelVel(float* vx, float* vy, float* vz) {
    const double now = DsNow();
    int iNew = -1, iOld = -1;
    double tNew = -1.0, bestErr = 1e9;
    for (int i = 0; i < kZRing; i++)
        if (g_zRingT[i] > tNew) { tNew = g_zRingT[i]; iNew = i; }
    if (iNew < 0 || now - tNew > 0.15) return false;             // nothing fresh
    for (int i = 0; i < kZRing; i++) {
        const double age = tNew - g_zRingT[i];
        if (g_zRingT[i] <= 0.0 || age < 0.10 || age > 0.35) continue;
        const double err = fabs(age - 0.2);
        if (err < bestErr) { bestErr = err; iOld = i; }
    }
    if (iOld < 0) return false;                                  // no usable older pair
    const float dt = (float)(tNew - g_zRingT[iOld]);
    *vx = (g_pxRing[iNew] - g_pxRing[iOld]) / dt;
    *vy = (g_pyRing[iNew] - g_pyRing[iOld]) / dt;
    *vz = (g_zRing[iNew]  - g_zRing[iOld])  / dt;
    return true;
}

void CatchTweaks_ReadConfig(const char* buf) {
    g_catchFix     = TwkIniInt(buf, "CatchWindow", 1);
    g_catchMult    = (float)TwkIniInt(buf, "CatchWindowMult", 200) / 100.0f;
    g_manualMode   = TwkIniInt(buf, "ManualCatchMode", -1);
    g_catchBeatsDS = TwkIniInt(buf, "CatchBeatsDarkslide", 1);
    g_dsAngleDeg   = TwkIniInt(buf, "DarkslideZoneDeg", 60);
    g_catchDiag    = TwkIniInt(buf, "CatchDiag", 0);
    g_flickFoot      = TwkIniInt(buf, "CatchWithFlickedFoot", 1);
    g_flickInvert    = TwkIniInt(buf, "CatchFlickFootInvert", 0);
    // 150 = the other foot takes 1.5x the shipped 0.15 s to ATTACH. Deliberately mild: this gates
    // attachment only, and pushed far enough to look like a hold (300 was tried) it left the foot
    // still descending at touchdown and clipped it through the deck. Holding the foot OFF the board
    // is foot_place's catch pin, which counters the pose directly and releases on landing.
    g_catchTwoStick  = TwkIniInt(buf, "CatchWithStickHeld", 1);
    g_heldMaskFrames = TwkIniInt(buf, "CatchHeldMaskFrames", 2);
    if (g_heldMaskFrames < 0) g_heldMaskFrames = 0; else if (g_heldMaskFrames > 10) g_heldMaskFrames = 10;
    g_secondFootHold = TwkIniInt(buf, "SecondFootHold", 0);
    g_boneScale     = TwkIniInt(buf, "BoneScalePct", 100);
    g_boneAdd[0]    = TwkIniInt(buf, "BoneAddX", 0);
    g_boneAdd[1]    = TwkIniInt(buf, "BoneAddY", 0);
    g_boneAdd[2]    = TwkIniInt(buf, "BoneAddZ", 0);
    if (g_boneScale < 0) g_boneScale = 0; else if (g_boneScale > 300) g_boneScale = 300;
    for (int i = 0; i < 3; i++) {
        if (g_boneAdd[i] < -100) g_boneAdd[i] = -100;
        else if (g_boneAdd[i] > 100) g_boneAdd[i] = 100;
    }
    g_flipTrace    = TwkIniInt(buf, "FlipCatchTrace", 0);
    g_flipTraceVerbose = TwkIniInt(buf, "FlipCatchTraceVerbose", 0);
    g_anyRev       = TwkIniInt(buf, "CatchAnyRevolution", 1);
    g_anyRevDeg    = TwkIniInt(buf, "CatchAnyRevDeg", 60);
    g_footLevel    = TwkIniInt(buf, "CatchFootLevelsBoard", 1);
    g_holdPose     = TwkIniInt(buf, "CatchHoldPose", 1);
    g_needFlick    = TwkIniInt(buf, "CatchNeedsFreshFlick", 1);
    g_unstick      = TwkIniInt(buf, "CatchUnstick", 1);
    g_minSpinDeg   = TwkIniInt(buf, "CatchMinSpinDeg", 45);
    g_maxCutDeg    = TwkIniInt(buf, "CatchMaxCutDeg", 180);
    if (g_maxCutDeg < 0) g_maxCutDeg = 0; else if (g_maxCutDeg > 1080) g_maxCutDeg = 1080;
    if (g_minSpinDeg < 0) g_minSpinDeg = 0; else if (g_minSpinDeg > 360) g_minSpinDeg = 360;
    g_unstickMs    = TwkIniInt(buf, "CatchUnstickMs", 1000);
    if (g_unstickMs < 300)  g_unstickMs = 300;
    if (g_unstickMs > 5000) g_unstickMs = 5000;
    g_stopFlip     = TwkIniInt(buf, "CatchStopsFlip", 1);
    g_stopFlipDeg  = TwkIniInt(buf, "CatchStopFlipDeg", 168);
    g_snapMs       = TwkIniInt(buf, "CatchSnapMs", 90);
    g_snapMaxDeg   = TwkIniInt(buf, "CatchSnapMaxDeg", 200);
    g_snapMaxBoost = TwkIniInt(buf, "CatchSnapMaxBoost", 3);
    g_flipAxis     = TwkIniInt(buf, "CatchFlipAxis", 0);
    if (g_flipAxis < 0 || g_flipAxis > 2) g_flipAxis = 0;
    if (g_snapMaxDeg   < 60) g_snapMaxDeg   = 60;   if (g_snapMaxDeg   > 360) g_snapMaxDeg = 360;
    if (g_snapMaxBoost < 1)  g_snapMaxBoost = 1;    if (g_snapMaxBoost > 10)  g_snapMaxBoost = 10;
    if (g_anyRevDeg < 5)   g_anyRevDeg = 5;
    if (g_anyRevDeg > 120) g_anyRevDeg = 120;
    if (g_snapMs < 30)  g_snapMs = 30;
    if (g_snapMs > 400) g_snapMs = 400;
    if (g_stopFlipDeg < 120) g_stopFlipDeg = 120;
    if (g_stopFlipDeg > 179) g_stopFlipDeg = 179;
    if (g_catchMult < 1.0f) g_catchMult = 1.0f;                // never NARROW the window
    if (g_catchMult > 6.0f) g_catchMult = 6.0f;
    if (g_dsAngleDeg < 10)  g_dsAngleDeg = 10;
    if (g_dsAngleDeg > 170) g_dsAngleDeg = 170;
    TwkLog("[catch] config: CatchWindow=%d CatchWindowMult=%.2f ManualCatchMode=%d "
           "CatchBeatsDarkslide=%d DarkslideZoneDeg=%d CatchDiag=%d | bone scale=%d%% "
           "add=%d/%d/%d cm",
           g_catchFix, g_catchMult, g_manualMode, g_catchBeatsDS, g_dsAngleDeg, g_catchDiag,
           g_boneScale, g_boneAdd[0], g_boneAdd[1], g_boneAdd[2]);
}

void CatchTweaks_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "CatchWindow",         g_catchFix);
    TwkIniSetInt(buf, cap, "CatchWindowMult",     (int)(g_catchMult * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "CatchBeatsDarkslide", g_catchBeatsDS);
    TwkIniSetInt(buf, cap, "DarkslideZoneDeg",    g_dsAngleDeg);
    TwkIniSetInt(buf, cap, "CatchAnyRevolution",  g_anyRev);
    TwkIniSetInt(buf, cap, "CatchAnyRevDeg",      g_anyRevDeg);
    TwkIniSetInt(buf, cap, "CatchFootLevelsBoard", g_footLevel);
    TwkIniSetInt(buf, cap, "CatchHoldPose",       g_holdPose);
    TwkIniSetInt(buf, cap, "CatchNeedsFreshFlick", g_needFlick);
    TwkIniSetInt(buf, cap, "CatchUnstick",        g_unstick);
    TwkIniSetInt(buf, cap, "CatchMinSpinDeg",     g_minSpinDeg);
    TwkIniSetInt(buf, cap, "CatchMaxCutDeg",      g_maxCutDeg);
    TwkIniSetInt(buf, cap, "CatchUnstickMs",      g_unstickMs);
    TwkIniSetInt(buf, cap, "CatchStopsFlip",      g_stopFlip);
    TwkIniSetInt(buf, cap, "CatchStopFlipDeg",    g_stopFlipDeg);
    TwkIniSetInt(buf, cap, "CatchSnapMs",         g_snapMs);
    TwkIniSetInt(buf, cap, "CatchSnapMaxDeg",     g_snapMaxDeg);
    TwkIniSetInt(buf, cap, "CatchSnapMaxBoost",   g_snapMaxBoost);
    TwkIniSetInt(buf, cap, "CatchFlipAxis",       g_flipAxis);
    TwkIniSetInt(buf, cap, "CatchWithFlickedFoot", g_flickFoot);
    TwkIniSetInt(buf, cap, "CatchFlickFootInvert", g_flickInvert);
    TwkIniSetInt(buf, cap, "CatchWithStickHeld",  g_catchTwoStick);
    TwkIniSetInt(buf, cap, "CatchHeldMaskFrames", g_heldMaskFrames);
    TwkIniSetInt(buf, cap, "SecondFootHold",      g_secondFootHold);
    // Written back so it EXISTS in the ini to be found. A read-only key defaults silently and is
    // indistinguishable from a key you set wrongly -- which is exactly how a dead `FootCatchDiag`
    // left over from the foot-placement cleanup got mistaken for this one.
    TwkIniSetInt(buf, cap, "CatchDiag",           g_catchDiag);
    TwkIniSetInt(buf, cap, "BoneScalePct",        g_boneScale);
    TwkIniSetInt(buf, cap, "BoneAddX",            g_boneAdd[0]);
    TwkIniSetInt(buf, cap, "BoneAddY",            g_boneAdd[1]);
    TwkIniSetInt(buf, cap, "BoneAddZ",            g_boneAdd[2]);
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
static const char* SIG_CAN_CATCH =      // InAirHandler::CanCatchOrient
    "48 89 74 24 18 41 56 48 83 EC 70 0F 29 74 24 60 48 8B F1 48 8B 09 0F 29 7C 24 50 44 0F 29 44 24 40"; // Epic 0x1044980 / Steam 0x1004d00
static const char* SIG_CATCH_DEFAULT =  // InAirHandler::CheckForCatchOrient_Default -- SEVEN args!
    "40 56 57 41 56 48 83 EC 50 49 8B F1 4D 8B F0 48 8B F9 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ??";      // Epic 0x10466c0 / Steam 0x1006a40
// ASkaterCharacterBase::SetCatchOrient(ECatchOrientState state, float pitchRatio, float yawRatio).
// FOUR args, all in registers -- verified against the disassembly rather than assumed: the body
// reads nothing above shadow space, `movzx edi,dl` takes the state, `movaps xmm10,xmm2` /
// `movaps xmm9,xmm3` take the two floats, and the epilogue is
//     movss [rbx+0x640], xmm10 / movss [rbx+0x644], xmm9 / mov [rbx+0x63e], dil / ret
// with no eax written, so it returns void.
static const char* SIG_SET_CATCH_ORIENT =
    "40 55 53 57 48 8D 6C 24 B9 48 81 EC C0 00 00 00 48 8B 81 90 05 00 00 0F B6 FA 44 0F 29 8C 24 80 00 00 00"; // Epic 0x1004120 / Steam 0xfc3f50

// ------------------------------------------------------------------ the manual-catch window
// The defs are static config (164 of them, all shipping 60/60), so this is a one-time write per
// mode transition -- no per-frame cost, nothing written while a trick is in flight.
struct DefSave { void* def; float flip, rot; };
static DefSave g_defSave[512];
static int     g_nDefSave = 0;
static bool    g_widened  = false;
// The multiplier actually WRITTEN into the trick defs. The widen only runs on a catch-mode
// transition, so without this a slider change sits inert until the mode is toggled -- which made an
// A/B of the window silently test nothing at all.
static float   g_appliedMult = 0.0f;

// ---- the built-in boned ollie: zero the authored board offsets, restore them exactly on release.
// Same shape as the pre-catch-angle walk below -- save originals, write, put them back -- because
// the failure mode of a half-restored asset is a game that stays modified after the toggle is off.
static const int kBoardOffsets[4] = { COS_OFF_L_RGS, COS_OFF_R_RGS, COS_OFF_L_SWS, COS_OFF_R_SWS };
struct OrientSave { uint8_t* settings; float off[4][3]; };
static OrientSave g_orientSave[256];
static int  g_nOrientSave = 0;
static bool g_boneAdjusted = false;

// Originals are captured ONCE, before anything is written, so the asset can always be put back
// exactly however many times the value has been driven since.
static bool captureOrients(void* codb) {
    if (g_nOrientSave > 0) return true;
    if (!codb) return false;
    uint8_t* arr = (uint8_t*)twkP(codb, CODB_ORIENTS);
    const int n = twkI(codb, CODB_ORIENTS + 8);
    if (!arr || n <= 0 || n > 128) return false;        // an implausible count is a wrong pointer
    for (int i = 0; i < n; i++) {
        uint8_t* def = arr + (size_t)i * CO_DEF_STRIDE;
        for (int h = 0; h < 2; h++) {
            if (g_nOrientSave >= 256) break;
            uint8_t* st = def + (h == 0 ? CO_BACK_SETTINGS : CO_FRONT_SETTINGS);
            OrientSave& s = g_orientSave[g_nOrientSave];
            bool ok = true;
            for (int v = 0; v < 4 && ok; v++)
                for (int c = 0; c < 3; c++) {
                    const float f = twkF(st, kBoardOffsets[v] + c * 4);
                    if (!(f > -1000.0f && f < 1000.0f)) { ok = false; break; }
                    s.off[v][c] = f;
                }
            if (!ok) continue;                          // never write through a read we distrust
            s.settings = st;
            g_nOrientSave++;
        }
    }
    if (g_nOrientSave > 0) {
        TwkLog("[catch] catch-orient board offsets captured on %d settings blocks (%d definitions) "
               "-- originals held for exact restore", g_nOrientSave, n);
        // Print the authored vectors for the first few blocks. The regular/switch MIRROR is the
        // thing the add has to respect, so it should be measured rather than assumed -- if SWS does
        // not come back as the sign-flip of RGS, the signing above is the wrong model.
        for (int i = 0; i < g_nOrientSave && i < 3; i++) {
            const OrientSave& s = g_orientSave[i];
            TwkLog("[catch]   block %d: L_RGS(%.2f,%.2f,%.2f) R_RGS(%.2f,%.2f,%.2f) "
                   "L_SWS(%.2f,%.2f,%.2f) R_SWS(%.2f,%.2f,%.2f)", i,
                   s.off[0][0], s.off[0][1], s.off[0][2], s.off[1][0], s.off[1][1], s.off[1][2],
                   s.off[2][0], s.off[2][1], s.off[2][2], s.off[3][0], s.off[3][1], s.off[3][2]);
        }
    }
    return g_nOrientSave > 0;
}
// Always FROM THE SAVED ORIGINAL, never from the live value: that is what makes a per-frame write
// idempotent instead of compounding. Per-slot too, so the authored differences between sides and
// stances survive the scaling instead of being flattened to one number.
static void writeOrientsScaled(int scalePct, const int add[3]) {
    const float k = (float)scalePct * 0.01f;
    for (int i = 0; i < g_nOrientSave; i++) {
        OrientSave& s = g_orientSave[i];
        if (!s.settings) continue;
        for (int q = 0; q < 4; q++)
            for (int c = 0; c < 3; c++)
                // A flat add, deliberately. MEASURED authored data (logged at capture):
                //   block 0: L_RGS(0,3,0) R_RGS(0,-7,0)  L_SWS(-15,7,0) R_SWS(-15,-3,0)
                // The switch pair is not a sign-flip of the regular pair -- it is the feet SWAPPED
                // and negated, and the tilt is the L-R DIFFERENCE, which is 10 in both stances. A
                // flat add shifts both feet together and leaves that difference untouched, so it
                // cannot tilt the board either way. Signing the add per slot was tried and reverted:
                // it widened the spread (7 -> 9) in BOTH stances, which is a different change, not a
                // switch fix.
                *(float*)(s.settings + kBoardOffsets[q] + c * 4) = s.off[q][c] * k + (float)add[c];
    }
}
static void restoreOrients() {
    for (int i = 0; i < g_nOrientSave; i++) {
        OrientSave& s = g_orientSave[i];
        if (!s.settings) continue;
        for (int q = 0; q < 4; q++)
            for (int c = 0; c < 3; c++)
                *(float*)(s.settings + kBoardOffsets[q] + c * 4) = s.off[q][c];
    }
}

// At stock (100% and no add) the game's own values are left completely alone, and anything written
// earlier is put back. Otherwise the adjusted values are re-derived from the originals each frame,
// so moving a slider takes effect immediately and repeated writes cannot accumulate.
static void applyBoardOffsets(void* codb) {
    const bool adjust = (g_boneScale != 100) ||
                        g_boneAdd[0] || g_boneAdd[1] || g_boneAdd[2];
    if (!adjust && !g_boneAdjusted) return;
    __try {
        if (!adjust) {
            restoreOrients();
            TwkLog("[catch] boned ollie back to stock on %d catch-orient settings blocks", g_nOrientSave);
            g_nOrientSave = 0; g_boneAdjusted = false;
            return;
        }
        if (!captureOrients(codb)) return;
        writeOrientsScaled(g_boneScale, g_boneAdd);
        if (!g_boneAdjusted) {
            g_boneAdjusted = true;
            TwkLog("[catch] boned ollie adjusted: scale %d%%, add %d/%d/%d cm", g_boneScale,
                   g_boneAdd[0], g_boneAdd[1], g_boneAdd[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TwkLog("[catch] caught fatal touching the catch-orient board offsets -> bone left at stock");
        g_boneScale = 100; g_boneAdd[0] = g_boneAdd[1] = g_boneAdd[2] = 0;
    }
}

static void applyCatchWindow(void* tricksDb, bool widen) {
    if (!tricksDb) return;
    __try {
        if (widen) {
            void*   arr = twkP(tricksDb, TDB_FLIP_TRICKS);
            const int n = twkI(tricksDb, TDB_FLIP_TRICKS + 8);
            if (!arr || n <= 0 || n > 512) return;
            g_nDefSave = 0;
            for (int i = 0; i < n; i++) {
                void* def = nullptr;
                __try { def = ((void**)arr)[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!def) continue;
                const float f = twkF(def, DEF_FLIP_PRECATCH_ANGLE);
                const float r = twkF(def, DEF_ROT_PRECATCH_ANGLE);
                if (f < 0.0f || f > 360.0f || r < 0.0f || r > 360.0f) continue;   // never seen; refuse anyway
                DefSave& s = g_defSave[g_nDefSave++];
                s.def = def; s.flip = f; s.rot = r;
                *(float*)((uint8_t*)def + DEF_FLIP_PRECATCH_ANGLE) = f * g_catchMult;
                *(float*)((uint8_t*)def + DEF_ROT_PRECATCH_ANGLE)  = r * g_catchMult;
            }
            g_widened = true;
            g_appliedMult = g_catchMult;
            TwkLog("[catch] MANUAL catch: pre-catch angle x%.2f on %d trick defs (e.g. %.0f -> %.0f deg)",
                   g_catchMult, g_nDefSave,
                   g_nDefSave ? g_defSave[0].flip : 0.0f,
                   g_nDefSave ? g_defSave[0].flip * g_catchMult : 0.0f);
        } else {
            for (int i = 0; i < g_nDefSave; i++) {
                DefSave& s = g_defSave[i];
                if (!s.def) continue;
                *(float*)((uint8_t*)s.def + DEF_FLIP_PRECATCH_ANGLE) = s.flip;
                *(float*)((uint8_t*)s.def + DEF_ROT_PRECATCH_ANGLE)  = s.rot;
            }
            // Always state the reason: this also fires when the F1 checkbox is unticked or the
            // feature has faulted off, so a fixed "not in manual catch" message would misattribute
            // most restores to the catch mode.
            if (g_widened)
                TwkLog("[catch] window RESTORED to stock on %d trick defs (%s)", g_nDefSave,
                       (g_catchFix == 0) ? "the widen setting is OFF"
                                         : (g_manualMode < 0) ? "ManualCatchMode is unset"
                                                              : "not in manual catch");
            g_nDefSave = 0; g_widened = false;
        }
        InterlockedExchange(&g_uiCatchWide, g_widened ? 1 : 0);
        InterlockedExchange(&g_uiCatchDefs, g_nDefSave);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[catch] caught fatal adjusting the catch window -> feature off");
        g_catchFix = 0;
    }
}

// InAirHandler::CanCatchOrient `bool (this)` -- the anchor: `this` carries the skater (mode) and the
// tricks DB. Pure observation: the original's verdict is returned untouched, and this hook only
// watches the mode pair and swaps the window on transitions.
typedef bool (*CanCatchFn)(void*, void*, void*, void*);
static void* g_origCanCatch  = nullptr;
static void* g_startCanCatch = nullptr;
static int g_samplerOK = 1;   // one-shot: a sampler fault may not cost anything but the sampler

static bool hkCanCatchOrient(void* self, void* b, void* c, void* d) {
    if (self) { void* s = twkP(self, IAH_SKATER); if (s) g_lastSkater = s; }   // twkP is SEH-safe
    // Position sampling for run_out, under its OWN guard. Sharing the widen gate's __try would let
    // a sampler fault run that handler, which switches the whole window fix off.
    if (self && g_samplerOK) {
        __try {
            void* skaterS = twkP(self, IAH_SKATER);
            if (skaterS) {
                const double now = DsNow();
                if (now - g_zLastPush >= 0.008) {
                    void* root = twkP(skaterS, 0x130);             // AActor::RootComponent
                    const float px = root ? twkF(root, 0x1d0) : -999999.0f;
                    const float py = root ? twkF(root, 0x1d4) : 0.0f;
                    const float z  = root ? twkF(root, 0x1d8) : 0.0f;
                    if (px > -100000.0f) {
                        static bool s_live = false;
                        if (!s_live) { s_live = true; TwkLog("[catch] position sampler live (z=%.0f cm)", z); }
                        g_zRing[g_zHead]  = z;
                        g_pxRing[g_zHead] = px;
                        g_pyRing[g_zHead] = py;
                        g_zRingT[g_zHead] = now;
                        g_zHead = (g_zHead + 1) % kZRing;
                        g_zLastPush = now;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_samplerOK = 0;
            TwkLog("[catch] position sampler faulted -> sampler off (window/darkslide fixes unaffected)");
        }
    }
    if (self) {
        __try {
            void* skater = twkP(self, IAH_SKATER);
            const int catchMode  = skater ? twkB(skater, SK_CATCH_MODE) : -1;   // the MENU setting
            const int orientMode = twkB(self, IAH_CATCH_ORIENT_INPUT);          // a different enum
            // ---- LEARN which ECatchMode value is MANUAL, instead of demanding it in the ini.
            // The menu's Manual is the one that runs CanCatchOrient's TIMER branch, and that branch
            // is selected by ECatchOrientInputMode == 3 (measured when the mode gates were mapped;
            // the pairing catchMode=2 <-> orientMode=3 shows in every log since, on BOTH installs).
            // So the first time we see the timer branch active, the current catchMode IS manual.
            // Field-proven need: a friend's install ran with ManualCatchMode=-1 -- run-out silently
            // did nothing ("manual=0" on every real-bail line) because the value only lived in one
            // user's ini. An explicit ini value still wins; the learned one is session-only.
            if (g_manualMode < 0 && orientMode == 3 && catchMode >= 0) {
                g_manualMode = catchMode;
                TwkLog("[catch] learned: ECatchMode=%d is MANUAL (the orient timer branch is active) "
                       "-- window widening and run-out armed", catchMode);
            }
            if (catchMode != (int)g_uiCatchMode || orientMode != (int)g_uiOrientMode) {
                InterlockedExchange(&g_uiCatchMode,  (LONG)catchMode);
                InterlockedExchange(&g_uiOrientMode, (LONG)orientMode);
                TwkLog("[catch] ECatchMode=%d (the Catch Mode setting)  ECatchOrientInputMode=%d  -- %s",
                       catchMode, orientMode,
                       (g_manualMode < 0)
                           ? "ManualCatchMode unknown (learned on first manual-mode play; ini overrides)"
                           : (catchMode == g_manualMode ? "MANUAL -- widened window applies"
                                                        : "not manual -- stock window"));
            }
            const bool want = (g_catchFix != 0) && (g_manualMode >= 0) && (catchMode == g_manualMode);
            if (want != g_widened) {
                applyCatchWindow(twkP(self, IAH_TRICKS_DB), want);
            } else if (want && g_appliedMult != g_catchMult) {
                // The setting moved while it was already applied: put the originals back and
                // re-widen with the new value, so the slider means something the moment it moves.
                applyCatchWindow(twkP(self, IAH_TRICKS_DB), false);
                applyCatchWindow(twkP(self, IAH_TRICKS_DB), true);
            }
            // The bone adjustment is independent of catch MODE -- it is authored data, not a manual
            // catch behaviour -- so it follows its own knobs only.
            applyBoardOffsets(twkP(self, IAH_CATCH_ORIENTS_DB));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_catchFix = 0;
            TwkLog("[catch] caught fatal in the widen gate -> window fix off");
        }
    }
    __try { return ((CanCatchFn)g_origCanCatch)(self, b, c, d); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[catch] caught fatal in CanCatchOrient -> recovered");
        return false;
    }
}

// CheckForCatchOrient_Default -- where the whole manual catch is decided, in the ONE frame the stick
// edge exists (the +0x33/+0x34 note). The dark-slide mask lives here.
// The signature below is read from the CALL SITE (0x10464e0..0x1046511), never assumed:
//   (InAirHandler* this, float dt in XMM1, FVector2D* front, FVector2D* back,
//    void* a5, void* a6, void* a7)                 <- args 5-7 on the STACK, 5/6 are out-pointers
// Both the arity and the parameter sizes must match exactly: an under-declared thunk hands the
// original a clobbered dt and garbage args 5-7, and it faults on an ordinary frame. `dt` is declared
// `double` and forwarded UNCONVERTED.
typedef void* (*CatchDefaultFn)(void*, double, void*, void*, void*, void*, void*);
static void* g_origCatchDef  = nullptr;
static void* g_startCatchDef = nullptr;
// How far the deck is from FLAT, measured as ROLL ABOUT ITS OWN LONG AXIS. Returns 180 when flat
// (griptape up) and 0 when fully inverted, matching what the rest of this file expects.
//
// The previous version used the deck's up-vector against world up (`1 - 2(qx^2+qy^2)`), which is a
// single number with roll AND pitch folded into it. Two consequences, both observed: the flip was
// stopped while the board was still visibly rolled, because 12 deg "off vertical" can be entirely
// roll; and it sometimes did not fire at all, because "Level board on catch" and the game's own
// pitch/roll alignment are moving that same up-vector during the same window, so the angle stops
// rising monotonically and the increasing-angle test misses the frame that mattered.
//
// Roll about the long axis is the rotation a flip actually IS. Pitch tilts the long axis itself, and
// building the reference in the plane perpendicular to it removes pitch by construction -- so
// levelling can no longer perturb this measurement.
static float BoardGripAxis(void* skater, int axis) {
    void* board   = skater ? twkP(skater, SK_BOARD) : nullptr;
    void* flipper = board ? twkP(board, BOARD_FLIPPER) : nullptr;
    if (!flipper) return -1.0f;
    const float qx = twkF(flipper, COMP_CTW_QUAT),      qy = twkF(flipper, COMP_CTW_QUAT + 4);
    const float qz = twkF(flipper, COMP_CTW_QUAT + 8),  qw = twkF(flipper, COMP_CTW_QUAT + 12);
    if (!(qx > -100000.0f) || !(qy > -100000.0f) || !(qz > -100000.0f) || !(qw > -100000.0f))
        return -1.0f;

    // The deck's own axes in world, straight from the quaternion.
    const float upX = 2.0f * (qx * qz + qw * qy);
    const float upY = 2.0f * (qy * qz - qw * qx);
    const float upZ = 1.0f - 2.0f * (qx * qx + qy * qy);
    float fx, fy, fz;                       // the LONG axis; which local axis that is, is a knob
    if (axis == 1) {                        // local Y
        fx = 2.0f * (qx * qy - qw * qz); fy = 1.0f - 2.0f * (qx * qx + qz * qz); fz = 2.0f * (qy * qz + qw * qx);
    } else if (axis == 2) {                 // local Z
        fx = upX; fy = upY; fz = upZ;
    } else {                                // local X (default)
        fx = 1.0f - 2.0f * (qy * qy + qz * qz); fy = 2.0f * (qx * qy + qw * qz); fz = 2.0f * (qx * qz - qw * qy);
    }
    // World up with the long-axis component removed = "which way is up, ignoring pitch".
    const float d = fz;                                     // dot(worldUp, fwd)
    float rx = -fx * d, ry = -fy * d, rz = 1.0f - fz * d;
    const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 0.05f) {                                       // deck pointing straight up/down: no
        float u = upZ;                                      // roll reference exists, use the old
        if (u < -1.0f) u = -1.0f; else if (u > 1.0f) u = 1.0f;   // up-vector measure rather than
        return acosf(-u) * 57.2957795f;                     // returning nothing
    }
    rx /= rl; ry /= rl; rz /= rl;
    float c = upX * rx + upY * ry + upZ * rz;               // cos of the roll away from flat
    if (c < -1.0f) c = -1.0f; else if (c > 1.0f) c = 1.0f;
    const float roll = acosf(c) * 57.2957795f;              // 0 = flat, 180 = inverted
    return 180.0f - roll;                                   // 180 = flat, to match the old semantics
}
static float BoardGrip(void* skater) { return BoardGripAxis(skater, g_flipAxis); }

static void* hkCatchDefault(void* self, double dt, void* frontStick, void* backStick,
                            void* a5, void* a6, void* a7) {
    void* skater = self ? twkP(self, IAH_SKATER) : nullptr;
    // ---- catch input beats dark slides, via the BOARD ANGLE (see the header comment): mask the
    // reservation for this ONE call whenever the board is outside the grip-down zone, restore
    // after -- everything else reading the flag this frame sees the real value. Press-time native
    // catching everywhere a darkslide is physically impossible; stock inside the zone.
    int dsSaved = -1;
    if (g_catchBeatsDS && self && skater) {
        __try {
            if (g_manualMode >= 0 && twkB(skater, SK_CATCH_MODE) == g_manualMode) {
                const int dsCancel = twkB(self, IAH_DS_CANCEL);
                // 3-state edge log (-1 idle) so each reservation window logs its zones once
                static int wasMasking = -1;
                if (dsCancel > 0) {
                    // Grip orientation from the RENDERED deck: the flipper component's world quat.
                    // Board-up Z = 1 - 2(qx^2 + qy^2) (the rotated +Z axis' z-component); grip-up
                    // = +1, grip-down = -1. Unreadable -> honour the reservation (stock, safe).
                    float fromGripDown = -1.0f;
                    void* board   = twkP(skater, SK_BOARD);
                    void* flipper = board ? twkP(board, BOARD_FLIPPER) : nullptr;
                    if (flipper) {
                        const float qx = twkF(flipper, COMP_CTW_QUAT);
                        const float qy = twkF(flipper, COMP_CTW_QUAT + 4);
                        if (qx > -100000.0f && qy > -100000.0f) {
                            float upZ = 1.0f - 2.0f * (qx * qx + qy * qy);
                            if (upZ < -1.0f) upZ = -1.0f; else if (upZ > 1.0f) upZ = 1.0f;
                            fromGripDown = acosf(-upZ) * 57.2957795f;
                        }
                    }
                    if (fromGripDown >= 0.0f) {
                        const int masking = (fromGripDown > (float)g_dsAngleDeg) ? 1 : 0;
                        if (masking) {
                            *(uint8_t*)((uint8_t*)self + IAH_DS_CANCEL) = 0;
                            dsSaved = dsCancel;
                        }
                        if (masking != wasMasking) {
                            wasMasking = masking;
                            if (masking) {
                                InterlockedIncrement(&g_uiDsMasked);
                                TwkLog("[catch] reservation masked -- press-time catches (%.0f deg from grip-down)",
                                       fromGripDown);
                            } else {
                                InterlockedIncrement(&g_uiDsHonored);
                                TwkLog("[catch] darkslide zone honoured (%.0f deg from grip-down)",
                                       fromGripDown);
                            }
                        }
                    }
                } else wasMasking = -1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_catchBeatsDS = 0; }
    }
    // ---- CATCH WHILE THE OTHER STICK IS HELD (a scoop, a hold, anything).
    // Measured: with the back stick held at full deflection, a clean front-stick EDGE past the
    // deadzone (hadF 0->1 at magnitude 0.31, deadzone 0.2) is refused -- `st` never leaves 0 -- and
    // the dark-slide reservation is provably NOT involved (dsCancel=0 dsWin=0 on every frame). The
    // only difference from a catch that works is that the other stick is not neutral: a catch orient
    // is a SINGLE-STICK gesture, so two sticks deflected is not a catch to the game at all.
    // Fix: for the duration of this ONE call, present the held stick as centred so the flick reads
    // as single-stick, then put it straight back. Same save/zero/call/restore shape as the
    // dark-slide mask above, and on pointers we were already handed.
    // A mask deliberately left in place across the dispatcher, unwound on a later call.
    // ONLY the InAirHandler's own tracker byte is held across calls -- never a stick pointer.
    // The stick args are the caller's temporaries (see the restore below), so a pointer to one is
    // dangling by the next call.
    static int   pendHadOff = -1, pendHadVal = 0, pendFrames = 0; static void* pendSelf = nullptr;
    if (pendSelf && pendHadOff >= 0) {
        if (--pendFrames <= 0) {
            __try { *((uint8_t*)pendSelf + pendHadOff) = (uint8_t)pendHadVal; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            pendSelf = nullptr; pendHadOff = -1;
        }
    }

    // ---- remember WHICH PHYSICAL STICK made the fresh flick, for the flicked-foot fix.
    //
    // MEASURED: the `frontStick`/`backStick` arguments are COPIES, not pointers into the
    // InputHandler -- the stick-held log has been printing `aliases InputHandler: no` all along. So
    // neither pointer identity nor the game's own front/back trackers can name a PHYSICAL stick
    // here. The edge is therefore detected on the InputHandler's own raw left/right vectors with our
    // own one-frame history, which owes nothing to argument order or stance.
    //
    // Must still run BEFORE the mask block: masking zeroes stick state the game reads afterwards.
    // Exactly one fresh edge names a foot; two together are a deliberate two-stick gesture and name
    // nothing, so the game's own choice stands.
    if ((g_flickFoot || g_needFlick) && self && skater) {
        __try {
            void* mine = CatchTweaks_Skater();
            void* ih   = twkP(self, IAH_INPUT_HANDLER);
            if (ih && (!mine || skater == mine)) {
                const float dzRead = twkF(twkP(self, IAH_CATCH_ORIENTS_DB), CODB_INPUT_DEADZONE);
                const float dz = (dzRead > 0.0f && dzRead < 1.0f) ? dzRead : 0.2f;
                const float lx = twkF(ih, IH_RAW_LEFT),  ly = twkF(ih, IH_RAW_LEFT + 4);
                const float rx = twkF(ih, IH_RAW_RIGHT), ry = twkF(ih, IH_RAW_RIGHT + 4);
                const int outL = (lx * lx + ly * ly) > dz * dz;
                const int outR = (rx * rx + ry * ry) > dz * dz;
                static int prevOutL = 0, prevOutR = 0;   // our own edge history on the RAW sticks
                const int edgeL = outL && !prevOutL;
                const int edgeR = outR && !prevOutR;
                prevOutL = outL; prevOutR = outR;
                // Any fresh deflection stamps the veto clock -- catch INTENT, whichever stick.
                // Per-stick deflection START stamps feed the held-over test: a stick's deflection
                // is only intent if it BEGAN after the pop, and that is a per-stick fact -- the
                // catch flick being fresh says nothing about the scoop still held from before.
                if (edgeL || edgeR) {
                    LARGE_INTEGER t; QueryPerformanceCounter(&t); g_lastEdgeQpc = t.QuadPart;
                    if (edgeL) g_deflStartL = t.QuadPart;
                    if (edgeR) g_deflStartR = t.QuadPart;
                }
                if (!outL) g_deflStartL = 0;
                if (!outR) g_deflStartR = 0;
                if (g_flickFoot && edgeL != edgeR) {                    // exactly one of them edged
                    // A fresh flick re-decides: drop any latch so the next one-footed orient latches
                    // onto the foot just flicked rather than the previous catch's.
                    // 60 frames only has to span flick -> catch-engage (measured ~15); the latch
                    // covers the catch itself, so this window no longer bounds the correction.
                    g_flickPhys = edgeL ? 1 : 2; g_flickFresh = 60; g_flickLatch = 0;
                    // ---- MAPPING PROBE (no behaviour change). Closes the model that the hand-rolled
                    // stance table stands in for: which ARG the flicked stick landed on, plus every
                    // term of the game's own swap predicate. Four catches, one per stance, and the
                    // correct rule is readable straight off these lines.
                    if (g_catchDiag) {
                        const float fx2 = twkF(frontStick, 0), fy2 = twkF(frontStick, 4);
                        const bool fIsL = (fabsf(fx2 - lx) < 0.01f && fabsf(fy2 - ly) < 0.01f);
                        const bool fIsR = (fabsf(fx2 - rx) < 0.01f && fabsf(fy2 - ry) < 0.01f);
                        void* sk2 = twkP(self, IAH_SKATER);
                        TwkLog("[catch] MAP: flicked=%s frontArg=%s goofy=%d switch=%d lrFoot=%d",
                               edgeL ? "LEFT" : "RIGHT",
                               fIsL ? "rawLEFT" : fIsR ? "rawRIGHT" : "NEITHER",
                               CatchIsGoofy() ? 1 : 0, CatchIsSwitch() ? 1 : 0,
                               sk2 ? (twkB(sk2, SK_STANCE_OPTS) & 1) : -1);
                    }
                    if (g_catchDiag)
                        TwkLog("[catch] flick: %s stick (L %.2f,%.2f  R %.2f,%.2f)",
                               edgeL ? "LEFT" : "RIGHT", lx, ly, rx, ry);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_flickFoot = 0; }
    }

    float savedMask[2] = { 0.0f, 0.0f };
    void* maskedStick = nullptr;
    int   maskedHad = -1, savedHad = 0;
    if (g_catchTwoStick && self) {
        __try {
            const float fx = twkF(frontStick, 0), fy = twkF(frontStick, 4);
            const float bx = twkF(backStick, 0),  by = twkF(backStick, 4);
            const float dzRead = twkF(twkP(self, IAH_CATCH_ORIENTS_DB), CODB_INPUT_DEADZONE);
            const float dz = (dzRead > 0.0f && dzRead < 1.0f) ? dzRead : 0.2f;
            const bool fOut = (fx * fx + fy * fy) > dz * dz;
            const bool bOut = (bx * bx + by * by) > dz * dz;
            const bool fEdge = fOut && twkB(self, IAH_HAD_FRONT_INPUT) == 0;
            const bool bEdge = bOut && twkB(self, IAH_HAD_BACK_INPUT) == 0;
            // Mask the HELD stick only, and only while the OTHER one is making a fresh edge. With
            // both edging (a real two-stick gesture) nothing is touched.
            if (fEdge && bOut && !bEdge)      maskedStick = backStick;
            else if (bEdge && fOut && !fEdge) maskedStick = frontStick;
            if (maskedStick) {
                savedMask[0] = twkF(maskedStick, 0); savedMask[1] = twkF(maskedStick, 4);
                *(float*)maskedStick = 0.0f;
                *((float*)maskedStick + 1) = 0.0f;
                // AND THE TRACKER, which is the field that actually gates this. A catch needs
                // BOTH sticks released and then re-input; the "was it pushed" flags are written in
                // this function's own epilogue from LAST frame's sticks, so zeroing only the live
                // vector leaves a stored 1 saying the stick is still held and the edge is refused
                // anyway. Masking both is what makes the held stick look genuinely let go.
                maskedHad = (maskedStick == backStick) ? IAH_HAD_BACK_INPUT : IAH_HAD_FRONT_INPUT;
                savedHad = twkB(self, maskedHad);
                *((uint8_t*)self + maskedHad) = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { maskedStick = nullptr; g_catchTwoStick = 0; }
    }

    void* r = nullptr;
    __try { r = ((CatchDefaultFn)g_origCatchDef)(self, dt, frontStick, backStick, a5, a6, a7); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[catch] caught fatal in CheckForCatchOrient_Default -> recovered");
        // Restore on the FAULT path too. Bailing out with the stick still zeroed would leave the
        // player's held input centred for the rest of the frame, which reads as the stick dying.
        if (maskedStick) {
            __try {
                *(float*)maskedStick = savedMask[0];
                *((float*)maskedStick + 1) = savedMask[1];
                if (maskedHad >= 0) *((uint8_t*)self + maskedHad) = (uint8_t)savedHad;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return nullptr;
    }
    if (maskedStick) {                      // put the held stick and its tracker straight back
        __try {
            // The verdict is written through the out-pointers BEFORE the restore, so this is the
            // decision the mask actually produced -- and out5 is the catch state the dispatcher then
            // engages (a back-stick catch logs out5=2 and st becomes 2). If the wrong foot is being
            // caught, either this value already names the wrong one (the decision is at fault) or it
            // names the right one and the dispatcher re-derives from the restored sticks (the
            // restore is too early). One line separates those.
            const int verdict = a5 ? twkB(a5, 0) : 0;
            // Do these stick pointers ALIAS the InputHandler's own fields? If they do, holding the
            // mask blanks the scoop for everything else reading it, and the window must stay tiny.
            void* ih = twkP(self, IAH_INPUT_HANDLER);
            const bool alias = ih && (maskedStick == (void*)((uint8_t*)ih + IH_RAW_LEFT) ||
                                      maskedStick == (void*)((uint8_t*)ih + IH_RAW_RIGHT));
            TwkLog("[catch] stick-held catch: masked %s | out5=%d out6=%d | st now %d "
                   "(front %.2f,%.2f  held %.2f,%.2f) | aliases InputHandler: %s%s",
                   (maskedStick == backStick) ? "BACK (scoop held)" : "FRONT",
                   verdict, a6 ? twkB(a6, 0) : -1,
                   skater ? twkB(skater, SK_CATCH_ORIENT_STATE) : -1,
                   twkF(frontStick, 0), twkF(frontStick, 4),
                   savedMask[0], savedMask[1], alias ? "YES" : "no",
                   (verdict != 0 && g_heldMaskFrames > 0) ? "   [holding across the dispatcher]" : "");
            // A verdict was produced: keep the mask up so the DISPATCHER also sees the held stick as
            // released, instead of re-deriving the foot from it. No verdict = restore immediately.
            // The flicked stick is recorded at the TOP of this function (before the mask), so there
            // is nothing to record here -- just keep it fresh across the verdict.
            if (verdict != 0 && g_flickPhys != 0) g_flickFresh = 30;
            // The STICK VECTOR is ALWAYS restored before returning, and its pointer is NEVER kept.
            // `aliases InputHandler: no` above is the proof of why: these are the caller's own
            // FVector2D TEMPORARIES, not fields on a persistent object. Holding the pointer and
            // writing two floats through it on a LATER call wrote into a stack frame that had long
            // since been reused -- a stray write into whatever locals were live at that moment.
            // (Reported as the board tilting the wrong way in switch/fakie, which arrived with this
            // feature.) Masking it across calls was pointless as well as unsafe: the next call gets
            // FRESH copies, so a stale mask cannot influence it.
            *(float*)maskedStick = savedMask[0];
            *((float*)maskedStick + 1) = savedMask[1];
            // Only the TRACKER is held across the dispatcher -- it lives on the InAirHandler, a real
            // object with a stable address, which is what makes it safe to unwind on a later call.
            // That is also the field that actually gates the catch, so the feature is unaffected.
            if (verdict != 0 && g_heldMaskFrames > 0 && maskedHad >= 0) {
                pendHadOff = maskedHad; pendHadVal = savedHad; pendSelf = self;
                pendFrames = g_heldMaskFrames;
            } else if (maskedHad >= 0) {
                *((uint8_t*)self + maskedHad) = (uint8_t)savedHad;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (dsSaved > 0) {
        __try { *(uint8_t*)((uint8_t*)self + IAH_DS_CANCEL) = (uint8_t)dsSaved; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // ---- flip-travel trace: what the BOARD does across a catch ------------------------------------
    // ONE LINE PER CATCH by default. The first cut logged 20 lines a rep and stopped after a fixed 60
    // frames whether or not the board had settled -- at 120 fps that is half a second, so a second
    // flip that finished later was recorded as a clean catch. It now runs until the board is actually
    // still, and prints a summary you can scan across dozens of reps.
    if (g_flipTrace && self && skater) {
        __try {
            const float ang = BoardGrip(skater);
            const float fx = twkF(frontStick, 0), fy = twkF(frontStick, 4);
            const float bx = twkF(backStick, 0),  by = twkF(backStick, 4);
            const float dzRead = twkF(twkP(self, IAH_CATCH_ORIENTS_DB), CODB_INPUT_DEADZONE);
            const float dz = (dzRead > 0.0f && dzRead < 1.0f) ? dzRead : 0.2f;
            const bool pressed = (fx*fx + fy*fy > dz*dz) || (bx*bx + by*by > dz*dz);
            static bool  armed = false, wasPressed = false;
            static float prev = -1.0f, travel = 0.0f, pressAng = 0.0f;
            static int   frames = 0, still = 0, verdict = -1;
            if (pressed && !wasPressed && ang >= 0.0f) {
                armed = true; frames = 0; still = 0; travel = 0.0f; prev = ang;
                pressAng = ang; verdict = a5 ? twkB(a5, 0) : -1;
            }
            wasPressed = pressed;
            if (armed && ang >= 0.0f) {
                if (verdict <= 0 && a5 && twkB(a5, 0) > 0) verdict = twkB(a5, 0);   // may land a frame late
                const float d = fabsf(ang - prev);
                if (d < 170.0f) travel += d;
                still = (d < 1.0f) ? still + 1 : 0;        // settled = the deck has stopped moving
                prev = ang;
                if (g_flipTraceVerbose)
                    TwkLog("[trace]   t+%2d  board %3.0f deg  travelled %4.0f deg  out5=%d state=%d",
                           frames, ang, travel, a5 ? twkB(a5, 0) : -1,
                           twkB(skater, SK_CATCH_ORIENT_STATE));
                if (++frames > 400 || still > 12) {
                    armed = false;
                    // "Extra" is what the board did BEYOND running out to grip-up, which is all a
                    // clean catch should ever do -- so a double reads as ~360 rather than needing
                    // the press angle subtracted by eye.
                    const float expected = 180.0f - pressAng;
                    const float extra = travel - (expected > 0.0f ? expected : 0.0f);
                    TwkLog("[catchtrace] %-22s press=%3.0f out5=%d | travelled=%3.0f expected=%3.0f "
                           "extra=%3.0f%s", FlipSpeed_LastTrickName(), pressAng, verdict,
                           travel, expected, extra, (extra > 120.0f) ? "   <-- EXTRA FLIP" : "");
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_flipTrace = 0; }
    }

    if (g_catchDiag && self && skater) {
        __try {
            // _Default does not engage the catch: it writes its verdict through the a5/a6
            // OUT-POINTERS and the dispatcher engages after it returns. The only honest observables
            // here are those out-bytes plus a burst of the following frames, where the dispatcher's
            // effect becomes visible.
            const float fx = twkF(frontStick, 0), fy = twkF(frontStick, 4);
            const float bx = twkF(backStick, 0),  by = twkF(backStick, 4);
            const float dzRead = twkF(twkP(self, IAH_CATCH_ORIENTS_DB), CODB_INPUT_DEADZONE);
            const float dz = (dzRead > 0.0f && dzRead < 1.0f) ? dzRead : 0.2f;
            const bool pressed = (fx*fx + fy*fy > dz*dz) || (bx*bx + by*by > dz*dz);
            static int g_burst = 0;
            if (pressed) g_burst = 6;                    // arm on the EVENT, never a global cap
            if (g_burst > 0) {
                g_burst--;
                TwkLog("[catch] decide: front=(%.2f,%.2f) back=(%.2f,%.2f) hadF=%d hadB=%d "
                       "lastSt=%d st=%d | out5=%d out6=%d dsCancel=%d dsWin=%d%s",
                       fx, fy, bx, by,
                       twkB(self, IAH_HAD_FRONT_INPUT), twkB(self, IAH_HAD_BACK_INPUT),
                       twkB(self, IAH_LAST_ORIENT_STATE), twkB(skater, SK_CATCH_ORIENT_STATE),
                       a5 ? twkB(a5, 0) : -1, a6 ? twkB(a6, 0) : -1,
                       twkB(self, IAH_DS_CANCEL), twkB(self, IAH_DS_WINDOW),
                       pressed ? "   <-- press frame" : "");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_catchDiag = 0; }
    }
    return r;
}

// ------------------------------------------------------------------ catch with the FLICKED foot
// DECODED from USkaterAnimInstance::SetCatchOrient (Epic 0xf651d0), not inferred. It reads the
// skater's ECatchOrientState byte (+0x63e) and derives BOTH foot flags from that byte and nothing
// else:
//     movzx eax, [rdx+0x63e]                 ; the orient state
//     cmp al,1 / cmp al,5 / cmp al,0xa / cmp al,0xb  -> anim+0x313 = LEFT foot orient
//     mov eax, 0xc24 ; bt eax, ecx                   -> anim+0x314 = RIGHT foot orient
//   left  foot = state in { 1, 5, 10, 11 }
//   right foot = state in { 2, 5, 10, 11 }        (0xc24 = bits 2, 5, 10, 11)
//
// So the catching foot is ONE BYTE, chosen upstream of everything this module used to write --
// which is exactly why masking the stick vector, masking the input trackers, holding the mask
// across the dispatcher and forcing the per-foot CatchRatio all failed to move it. The ratio is an
// output of a ramp that raises both feet from a shared divisor (UpdateFeetCatchInfo, `maxss` only);
// it cannot select a foot and writing it only destroyed the catch.
//
// ASkaterCharacterBase::SetCatchOrient takes that byte as its 2nd ARGUMENT and stores it, so
// correcting it here fixes it at the source: the anim flags, the authored FCatchOrientDefinition
// that gets looked up, and the per-foot catch info all derive from the corrected value.
//
// Only a ONE-FOOTED orient that named the wrong foot is swapped:
//   * 7/8/9 are DARK SLIDES (ASkaterCharacterBase::IsInDarkSlideCatchOrientState is exactly
//     `(state-7) <= 1 || state == 9`) -- never touched.
//   * 5/10/11 are genuine TWO-foot orients -- never narrowed to one foot.
//   * 0 is "no catch".
typedef void (*SetCatchOrientFn)(void*, uint8_t, float, float);
static void* g_origSetOrient  = nullptr;
static void* g_startSetOrient = nullptr;

// REAL TRICKS ONLY -- latched over the air.
// The BONED OLLIE IS A CATCH ORIENT: holding the sticks in the air sets an orient state, and that
// state selects the FCatchOrientDefinition whose BoardRelativeOffset vectors shove the board. So a
// hook that rewrites 1<->2 unconditionally also overrides which orient an ORDINARY OLLIE uses -- and
// since the regular/switch orients are mirrored, the forced one is wrong in switch and the board
// tilts the opposite way. (Reported: switch ollie, holding a stick, the wrong end drops. The bone
// data itself was stock; what was wrong was which orient got applied.)
// Latched because the flip has already stopped by the time the catch registers, so an instantaneous
// test reads false exactly when it matters. An ollie sets neither flag for the whole air, which is
// what makes the latch a valid ollie test.
// Stance, read fresh (you can change it between runs, and a cached value would be stale).
// Failure to read counts as NOT goofy: that is the mapping that was already correct, so a bad read
// degrades to the previously-working behaviour instead of inverting it.
static bool CatchIsGoofy() {
    __try {
        void* a = FootPlace_AnimInstance();
        return a && twkB(a, AN_IS_GOOFY) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool CatchIsSwitch() {
    __try {
        void* a = FootPlace_AnimInstance();
        return a && twkB(a, AN_IS_SWITCH) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The stance correction, expressed through THE GAME'S OWN RULE rather than as a bare stance pair.
//
// CheckForCatchOrient (0x1045d80) fetches the two sticks and then swaps which one it hands to
// _Default as `front` / `back`:
//     swap = (IsSkatingGoofy != IsSkatingSwitch) && (skater[0x651] & 1)      <- disassembled
// and that predicate was then confirmed live, all four stances, by the MAP probe:
//     goofy=0 switch=0 -> front = raw LEFT      goofy=0 switch=1 -> front = raw RIGHT
//     goofy=1 switch=0 -> front = raw RIGHT     goofy=1 switch=1 -> front = raw LEFT
//
// We key on the PHYSICAL stick, so we simply UNDO THAT SWAP -- the predicate is the game's, exactly:
//     invert = swap = (goofy != switch) && _isLeftRightFootSkater
// Headset-verified in all four stances. An earlier cut inverted only the goofy half, because
// goofy-switch had read as correct while the both-feet bug was still masking which foot caught; once
// that was fixed, regular-switch showed up wrong too and the rule closed to the plain XOR.
//
// THE SETTING TERM MATTERS. `_isLeftRightFootSkater` is the control-scheme option binding the
// sticks to LEFT/RIGHT feet instead of FRONT/BACK. With it OFF the game never swaps, so inverting
// would be wrong -- the earlier `goofy && !switch` rule was correct only for the one value of a
// setting the player can change in the options menu.
//
// The SETTING term is load-bearing: with _isLeftRightFootSkater off the game never swaps, so
// inverting would be wrong. Do not reduce this to a bare stance test.
static bool CatchStanceInverts() {
    if (CatchIsGoofy() == CatchIsSwitch()) return false;   // goofy XOR switch -- the game's own swap
    __try {
        void* a  = FootPlace_AnimInstance();
        void* sk = a ? twkP(a, AN_SKATER) : nullptr;
        // Unreadable setting: fall back to the measured-true behaviour rather than to "no invert",
        // so a bad read keeps goofy working instead of silently regressing it.
        return !sk || (twkB(sk, SK_STANCE_OPTS) & 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// READ-ONLY. The latch is maintained in CatchTweaks_PumpFrame, every frame. It used to be updated
// HERE, but this runs only from the hook -- i.e. only on a non-zero orient state -- so after a flip
// trick it could stay set through the landing and into the next air, overriding an ordinary ollie's
// orient and tilting the board the wrong way exactly once before self-correcting.
// A rising edge is still taken here so a trick that starts between pumps is not missed for a frame.
static bool CatchAirIsTrick() {
    __try {
        void* a = FootPlace_AnimInstance();
        if (a && (twkB(a, AN_FLIPPING) > 0 || twkB(a, AN_ROTATING) > 0)) g_airWasTrick = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return g_airWasTrick != 0;
}


// "Is every current stick deflection held over from before the pop?" -- the CatchNeedsFreshFlick
// rule, reused for the orient RATIOS. PhysFalling's in-air orient chase (0xfc6ae0) reads the
// skater's catch-orient ratio fields, whose only writer is the setter we hook -- and the game
// keeps calling it after the catch completes with LIVE stick-derived ratios (the post-catch
// "hold to orient for a manual/grind" mechanic). A stick still deflected from the scoop or the
// trick flick is not orient intent; a deflection that BEGAN after the pop is, and passes.
// PER STICK, not per time: the catch flick is itself a fresh edge, so "was there a recent edge"
// reads fresh for the whole rest of the air while the RATIOS are computed from the OTHER stick --
// the scoop still held from before the pop (field round: zero fires, behaviour unchanged). A
// deflected stick is orient intent only if ITS OWN deflection began after the pop.
static bool CatchHeldOverStale() {
    const int ms = FlipSpeed_MsSinceTrick();
    if (ms < 0 || ms > 2500) return false;      // no flip trick running -- ollies/bone untouched
    const long long dl = g_deflStartL, dr = g_deflStartR;
    if (!dl && !dr) return false;               // nothing deflected -- nothing to zero
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    for (int i = 0; i < 2; ++i) {
        const long long st = i ? dr : dl;
        if (!st) continue;
        const int age = (int)((now.QuadPart - st) * 1000 / fq.QuadPart);
        if (ms - age >= 150) return false;      // this deflection began after the pop = real intent
    }
    return true;                                // only held-over deflections are live
}

static void hkSetCatchOrient(void* self, uint8_t state, float pitchRatio, float yawRatio) {
    uint8_t use = state;
    float usePitch = pitchRatio, useYaw = yawRatio;
    if (state == 0) {   // catch ended -- release the latch, the pose hold and the veto admission
        g_flickLatch = 0; g_latchIdle = 0;
        g_holdState = 0; g_holdLoggedState = 0;
        g_vetoAdmitted = 0;
        if (g_airCaughtFoot != 0) ++g_airCatchZeros;
        // Post-catch orient calls flow through HERE with live ratios; a held-over stick's are
        // zeroed (field: landing pitch tracked the held stick like a dial -- scoop-down -36,
        // flick-up +28, clean ~0 -- while every downstream pitch funnel measured untouched).
        __try {
            void* mine0 = CatchTweaks_Skater();
            if (g_needFlick && (!mine0 || self == mine0) && CatchHeldOverStale() &&
                (fabsf(usePitch) > 0.02f || fabsf(useYaw) > 0.02f)) {
                static long ratioLogged = -1;
                const long serial0 = FlipSpeed_TrickSerial();
                if (ratioLogged != serial0) {
                    ratioLogged = serial0;
                    TwkLog("[catch] held-over stick's orient ratios zeroed (pitch %.2f yaw %.2f) "
                           "at +%d ms", usePitch, useYaw, FlipSpeed_MsSinceTrick());
                }
                usePitch = 0.0f; useYaw = 0.0f;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else {
        __try {
            g_latchIdle = 0;
            // Co-op: this fires per skater and the flick record is the LOCAL player's sticks, so a
            // proxy must keep the vanilla selection rather than inherit our foot.
            void* mine = CatchTweaks_Skater();
            const bool isMine = (!mine || self == mine);
            // Only ever touch a real flip/rotation catch. On an ollie the orient is the game's own
            // board control (the bone) and must be left exactly as authored.
            // TWO-FOOT ORIENTS MUST BE NARROWED TOO, not just 1<->2 swapped. MEASURED: a catch
            // taken while the other stick is held (the mid-scoop catch) emits state 5 or 10 -- BOTH
            // feet -- and the first cut only rewrote 1<->2, so those passed through untouched and
            // both feet caught at once, in every stance. That is what "it catches with the wrong
            // foot" actually was; it never responded to a stance flip because it was never a stance
            // bug. Dark slides (7/8/9) stay untouched, and an OLLIE is excluded by CatchAirIsTrick()
            // so the boned ollie's own two-stick orient (11) is never narrowed.
            const bool oneFoot = (state == 1 || state == 2);
            const bool twoFoot = (state == 5 || state == 10 || state == 11);
            // ---- a catch needs a fresh flick (see the CatchNeedsFreshFlick comment) ---------
            bool vetoed = false;
            if (g_needFlick && isMine && !g_vetoAdmitted && (oneFoot || twoFoot)) {
                const int msTrick = FlipSpeed_MsSinceTrick();
                if (msTrick >= 0 && msTrick <= 2000) {      // a flip trick is running (ollies never stamp)
                    long long edge = g_lastEdgeQpc;
                    int edgeAfterSel = -1;
                    if (edge) {
                        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
                        const int edgeAgeMs = (int)((now.QuadPart - edge) * 1000 / fq.QuadPart);
                        edgeAfterSel = msTrick - edgeAgeMs;  // how long AFTER selection the edge landed
                    }
                    // The trick's own flick edges at ~selection time; the pop lands ~230 ms later.
                    // 150 ms splits them cleanly (field: real catch flicks landed 300+ ms in).
                    if (edgeAfterSel < 150) {
                        use = 0; vetoed = true;
                        const long serial = FlipSpeed_TrickSerial();
                        if (g_vetoLogged != serial) {
                            g_vetoLogged = serial;
                            TwkLog("[catch] catch engage vetoed +%d ms into '%s' -- no fresh flick "
                                   "since the pop (last edge %+d ms from selection); release and "
                                   "flick to catch", msTrick, FlipSpeed_LastTrickName(), edgeAfterSel);
                        }
                    } else {
                        g_vetoAdmitted = 1;                 // a real flick -- admit the whole catch
                    }
                }
            }
            // ---- and a catch needs the board to have actually SPUN (see CatchMinSpinDeg) -----
            // Deliberately after the flick veto and before everything else: like the veto this only
            // refuses the engage, so the game keeps asking and the catch lands the moment the deck
            // has turned far enough. Ollies never stamp a trick, darkslides are exempt.
            // NOT gated on g_vetoAdmitted: that flag means "a fresh flick was seen", which says
            // nothing about whether the board has turned. Copying it from the veto meant that on any
            // trick the flick check admitted, the spin check never ran at all.
            if (!vetoed && g_minSpinDeg > 0 && AutoCatchActive() && isMine &&
                (oneFoot || twoFoot) && !(state >= 7 && state <= 9)) {
                const int msTrick = FlipSpeed_MsSinceTrick();
                if (msTrick >= 0 && msTrick <= 2000 && g_trickTravelDeg < (float)g_minSpinDeg) {
                    use = 0; vetoed = true;
                    const long serial = FlipSpeed_TrickSerial();
                    if (g_spinLogged != serial) {
                        g_spinLogged = (int)serial;
                        TwkLog("[catch] catch held off +%d ms into '%s' -- the board has only turned "
                               "%.0f of %d deg", msTrick, FlipSpeed_LastTrickName(),
                               g_trickTravelDeg, g_minSpinDeg);
                    }
                }
            }
            // Same held-over rule for an ENGAGED catch's ratios: the pose hold pins whatever it
            // captures at engage, and for a held stick the engage snapshot IS the stale value.
            // Zeroing before the hold arms means it pins zeros. Darkslides keep theirs.
            if (g_needFlick && isMine && !(state >= 7 && state <= 9) && CatchHeldOverStale()) {
                usePitch = 0.0f; useYaw = 0.0f;
            }
            // ---- one catch per air (see the CatchHoldPose comment) --------------------------
            bool swallowed = false;
            if (!vetoed && g_holdPose && isMine && CatchAirIsTrick() && g_airCaughtFoot != 0 &&
                g_airCatchZeros >= 2 && !(state >= 7 && state <= 9)) {
                use = 0; swallowed = true;
                InterlockedIncrement(&g_uiHoldFixes);
                if (!g_airSwallowLogged) {
                    g_airSwallowLogged = 1;
                    TwkLog("[catch] second catch input this air (state %d after foot %d already "
                           "caught) -- swallowed, one catch per air", (int)state, g_airCaughtFoot);
                }
            }
            if (!swallowed && !vetoed) {
                g_airCatchZeros = 0;
                if (g_airCaughtFoot == 0 && (oneFoot || twoFoot) && CatchAirIsTrick())
                    g_airCaughtFoot = (int)state;
            }
            if (!swallowed && !vetoed && g_flickFoot && isMine && (oneFoot || twoFoot) && CatchAirIsTrick()) {
                // Latch the flicked foot on the FIRST narrowable orient of this catch, then hold it.
                if (g_flickLatch == 0 && g_flickFresh > 0 && g_flickPhys != 0) {
                    int want = (g_flickPhys == 1) ? 1 : 2;
                    if (CatchStanceInverts()) want = 3 - want;   // goofy-and-not-switch; see the table
                    if (g_flickInvert)        want = 3 - want;   // manual override, if parity reads backwards
                    g_flickLatch = want;
                }
                if (g_flickLatch != 0 && (uint8_t)g_flickLatch != state) {
                    use = (uint8_t)g_flickLatch; InterlockedIncrement(&g_uiFlickFixes);
                    // The pitch/yaw pose ratios were computed by the caller FOR the state it
                    // decided on. Narrowing a TWO-STICK orient (the mid-scoop catch) to one foot
                    // while forwarding its two-stick ratios blends the one-foot pose with numbers
                    // that encode the HELD scoop stick's deflection -- field-reported as the board
                    // pitching nose-down on an otherwise perfect front-foot catch. The discarded
                    // state's ratios go with it; a native one-foot catch computes its own from the
                    // single flick and is untouched.
                    if (twoFoot) {
                        static long loggedSerial = -1;
                        const long serial = FlipSpeed_TrickSerial();
                        if (loggedSerial != serial && (fabsf(usePitch) > 0.05f || fabsf(useYaw) > 0.05f)) {
                            loggedSerial = serial;
                            TwkLog("[catch] narrowed two-stick orient %d -> %d: dropped its pose "
                                   "ratios (pitch %.2f yaw %.2f -> 0)", (int)state, (int)use,
                                   usePitch, useYaw);
                        }
                        usePitch = 0.0f; useYaw = 0.0f;
                    }
                }
            }
            // ---- hold an engaged one-foot catch against the second stick (see the knob) ------
            // Runs on `use` (after the flick narrowing) so the two features agree on the foot.
            // Darkslide states (7/8/9) pass untouched AND drop the hold -- converting into a
            // darkslide mid-air is deliberate play, not the bug.
            if (!swallowed && !vetoed && g_holdPose && isMine && CatchAirIsTrick()) {
                if (use >= 7 && use <= 9) {
                    g_holdState = 0;
                } else if (g_holdState == 0 && (use == 1 || use == 2)) {
                    g_holdState = use;              // the catch engaged one-footed: hold this pose
                } else if (g_holdState != 0) {
                    if (use == 5 || use == 10 || use == 11) {
                        if (!g_holdLoggedState) {
                            g_holdLoggedState = 1;
                            TwkLog("[catch] second stick tried to flip the catch into two-stick "
                                   "state %d -- held on foot %d", (int)use, g_holdState);
                        }
                        use = (uint8_t)g_holdState;
                        InterlockedIncrement(&g_uiHoldFixes);
                    }
                    // The pitch/yaw ratios are NOT pinned here, and must not be. Pinning them was
                    // added speculatively alongside the state-narrowing, and the round after measured
                    // that neither ever fires on the bug they were written for -- the working half is
                    // ONE CATCH PER AIR. Kept as "harmless", it was not: STEERING THE CATCH IS DONE
                    // THROUGH THESE RATIOS. Holding a stick over to rotate the legs round into a
                    // tailslide writes a new yaw ratio every call, and pinning overwrote every one of
                    // them, so post-catch orient control was dead on any flip trick while ollies --
                    // exempt via CatchAirIsTrick -- still steered fine. Field-reported and confirmed
                    // from a log where the catch engaged normally (state 2, +449 ms) every time.
                }
            }
            // Behind CatchDiag now that the foot selection is settled. It stays available because a
            // silent log distinguishes "never routed through this setter" from "ran and declined",
            // which is not inferable from the game's behaviour and cost a round to learn.
            if (g_catchDiag)
                TwkLog("[catch] SetCatchOrient(state=%d)%s flick=%s latch=%d %s trick=%d%s",
                       (int)state, isMine ? "" : " [not mine]",
                       (g_flickPhys == 1) ? "LEFT" : (g_flickPhys == 2) ? "RIGHT" : "none",
                       g_flickLatch,
                       CatchIsGoofy() ? (CatchIsSwitch() ? "goofy-switch" : "goofy")
                                      : (CatchIsSwitch() ? "reg-switch"   : "regular"),
                       CatchAirIsTrick() ? 1 : 0,
                       (use != state) ? ((use == 1) ? "  -> FORCED left(1)" : "  -> FORCED right(2)") : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_flickFoot = 0; }
    }
    __try { ((SetCatchOrientFn)g_origSetOrient)(self, use, usePitch, useYaw); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[catch] caught fatal in SetCatchOrient -> recovered");
    }
}

// ------------------------------------------------------------------ install + menu
void CatchTweaks_Install() {
    g_startCanCatch = TwkScanExe(SIG_CAN_CATCH);
    if (!g_startCanCatch) { TwkLog("[catch] CanCatchOrient sig NOT FOUND -- catch window untouched (game updated?)"); g_catchFix = 0; }
    else if (MH_CreateHook(g_startCanCatch, (void*)&hkCanCatchOrient, &g_origCanCatch) != MH_OK ||
             MH_EnableHook(g_startCanCatch) != MH_OK) {
        TwkLog("[catch] CanCatchOrient hook failed -- catch window untouched");
        g_startCanCatch = nullptr; g_catchFix = 0;
    } else TwkLog("[catch] hooked CanCatchOrient @ %p -- window x%.2f when in manual", g_startCanCatch, g_catchMult);

    // The dark-slide mask and the decide log both live on the _Default hook, so when neither is
    // wanted the hook is not installed at all rather than relying on a pass-through being harmless.
    if (!g_catchBeatsDS && !g_catchDiag && !g_catchTwoStick) { TwkLog("[catch] CatchBeatsDarkslide=0 + CatchDiag=0 + CatchWithStickHeld=0 -- CheckForCatchOrient_Default NOT hooked"); return; }
    g_startCatchDef = TwkScanExe(SIG_CATCH_DEFAULT);
    if (!g_startCatchDef) { TwkLog("[catch] CheckForCatchOrient_Default sig NOT FOUND -- dark-slide fix off (game updated?)"); g_catchBeatsDS = 0; }
    else if (MH_CreateHook(g_startCatchDef, (void*)&hkCatchDefault, &g_origCatchDef) != MH_OK ||
             MH_EnableHook(g_startCatchDef) != MH_OK) {
        TwkLog("[catch] CheckForCatchOrient_Default hook failed -- dark-slide fix off");
        g_startCatchDef = nullptr; g_catchBeatsDS = 0;
    } else TwkLog("[catch] hooked CheckForCatchOrient_Default @ %p -- catch input beats dark slides", g_startCatchDef);

    // The flicked-foot fix needs the decide hook above for the flick record, so it is only useful
    // once that one is live.
    if (g_flickFoot && g_startCatchDef) {
        g_startSetOrient = TwkScanExe(SIG_SET_CATCH_ORIENT);
        if (!g_startSetOrient) { TwkLog("[catch] SetCatchOrient sig NOT FOUND -- flicked-foot fix off (game updated?)"); g_flickFoot = 0; }
        else if (MH_CreateHook(g_startSetOrient, (void*)&hkSetCatchOrient, &g_origSetOrient) != MH_OK ||
                 MH_EnableHook(g_startSetOrient) != MH_OK) {
            TwkLog("[catch] SetCatchOrient hook failed -- flicked-foot fix off");
            g_startSetOrient = nullptr; g_flickFoot = 0;
        } else TwkLog("[catch] hooked ASkaterCharacterBase::SetCatchOrient @ %p -- catch uses the flicked foot", g_startSetOrient);
    } else if (g_flickFoot) {
        TwkLog("[catch] flicked-foot fix needs CheckForCatchOrient_Default -- not installed");
        g_flickFoot = 0;
    }
}

bool CatchTweaks_Enabled() { return g_catchFix != 0; }
void CatchTweaks_SetEnabled(bool on) { g_catchFix = on ? 1 : 0; TwkMarkDirty(); }
// `g_manualMode` is deliberately NOT reset. It is not a preference but the ECatchMode value measured
// for this install off the "[catch] ECatchMode=" log line, and its default -1 means "unknown, widen
// nothing" -- resetting it would silently switch the whole catch feature off.
void CatchTweaks_ResetDefaults() {
    g_catchFix = 1; g_catchMult = 2.0f; g_catchBeatsDS = 1; g_dsAngleDeg = 60; g_catchDiag = 0;
    g_anyRev = 1; g_anyRevDeg = 60; g_footLevel = 1; g_unstick = 1; g_minSpinDeg = 45; g_maxCutDeg = 180; g_unstickMs = 1000; g_holdPose = 1; g_needFlick = 1;
    g_stopFlip = 1; g_stopFlipDeg = 168; g_snapMs = 90;
    g_snapMaxDeg = 200; g_snapMaxBoost = 3; g_flipAxis = 0;
    g_boneScale = 100; g_boneAdd[0] = g_boneAdd[1] = g_boneAdd[2] = 0;
    g_secondFootHold = 0; g_catchTwoStick = 1; g_heldMaskFrames = 2;
    // `g_flickFoot` is NOT reset to 1 here if its hook never installed -- Install() clears it on a
    // failed scan and turning it back on would advertise a fix that cannot run.
    g_flickFoot = g_startSetOrient ? 1 : 0; g_flickInvert = 0;
    TwkMarkDirty();
}
bool  CatchTweaks_SecondFootHold() { return g_secondFootHold != 0; }
void  CatchTweaks_SetSecondFootHold(bool on) { g_secondFootHold = on ? 1 : 0; TwkMarkDirty(); }
bool  CatchTweaks_HeldOverStale() { return CatchHeldOverStale(); }
bool  CatchTweaks_FlickFoot() { return g_flickFoot != 0; }
void  CatchTweaks_SetFlickFoot(bool on) { g_flickFoot = (on && g_startSetOrient) ? 1 : 0; TwkMarkDirty(); }
bool  CatchTweaks_FlickInvert() { return g_flickInvert != 0; }
void  CatchTweaks_SetFlickInvert(bool on) { g_flickInvert = on ? 1 : 0; TwkMarkDirty(); }
void* CatchTweaks_SetCatchOrientAddr() { return g_startSetOrient; }
float CatchTweaks_BoneScalePct() { return (float)g_boneScale; }
void  CatchTweaks_SetBoneScalePct(float v) {
    int s = (int)v; if (s < 0) s = 0; else if (s > 300) s = 300;
    g_boneScale = s; TwkMarkDirty();
}
float CatchTweaks_BoneAdd(int axis) {
    return (axis >= 0 && axis < 3) ? (float)g_boneAdd[axis] : 0.0f;
}
void  CatchTweaks_SetBoneAdd(int axis, float v) {
    if (axis < 0 || axis > 2) return;
    int a = (int)v; if (a < -100) a = -100; else if (a > 100) a = 100;
    g_boneAdd[axis] = a; TwkMarkDirty();
}
float CatchTweaks_WindowMultPct() { return g_catchMult * 100.0f; }
bool  CatchTweaks_StopsFlip() { return g_stopFlip != 0; }
void  CatchTweaks_SetStopsFlip(bool on) { g_stopFlip = on ? 1 : 0; TwkMarkDirty(); }
bool  CatchTweaks_AnyRevolution() { return g_anyRev != 0; }
void  CatchTweaks_SetAnyRevolution(bool on) { g_anyRev = on ? 1 : 0; TwkMarkDirty(); }
bool  CatchTweaks_FootLevelsBoard() { return g_footLevel != 0; }
void  CatchTweaks_SetFootLevelsBoard(bool on) { g_footLevel = on ? 1 : 0; TwkMarkDirty(); }
void  CatchTweaks_SetWindowMultPct(float pct) { g_catchMult = pct / 100.0f; TwkMarkDirty(); }
float CatchTweaks_DarkslideZoneDeg() { return (float)g_dsAngleDeg; }
void  CatchTweaks_SetDarkslideZoneDeg(float deg) { g_dsAngleDeg = (int)deg; TwkMarkDirty(); }

void CatchTweaks_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    if (!g_startCanCatch) { api->TextDisabled("Manual catch tweaks: not installed"); return; }
    bool cw = g_catchFix != 0;
    if (api->Checkbox("Wider manual catch window", &cw)) { g_catchFix = cw ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(manual catch only; auto stays stock)");
    if (cw) {
        api->Indent();
        float m = g_catchMult;
        if (api->SliderFloat("Window x", &m, 1.0f, 4.0f, "%.2f")) { g_catchMult = m; TwkMarkDirty(); }
        snprintf(b, sizeof(b), "60 deg -> %.0f deg  (~%.0f ms at 1960 deg/s)",
                 60.0f * g_catchMult, 60.0f * g_catchMult / 1960.0f * 1000.0f);
        api->TextDisabled(b);
        api->Unindent();
    }
    if (g_startCatchDef) {
    float bs = (float)g_boneScale;
    bool cts = g_catchTwoStick != 0;
    if (api->Checkbox("Catch while the other stick is held", &cts)) { g_catchTwoStick = cts ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(catch mid-scoop; the game refuses a two-stick catch)");
    if (g_startSetOrient) {
        bool cff = g_flickFoot != 0;
        if (api->Checkbox("Catch with the foot you flicked", &cff)) { g_flickFoot = cff ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(the game otherwise picks the foot itself)");
        if (cff) {
            api->Indent();
            bool inv = g_flickInvert != 0;
            if (api->Checkbox("Swap left/right", &inv)) { g_flickInvert = inv ? 1 : 0; TwkMarkDirty(); }
            api->SameLine(); api->TextDisabled("(tick this if it picks the opposite foot)");
            snprintf(b, sizeof(b), "orients corrected: %d", (int)g_uiFlickFixes);
            api->TextDisabled(b);
            api->Unindent();
        }
    }
    bool sfh = g_secondFootHold != 0;
    if (api->Checkbox("Hold the second foot until landing", &sfh)) { g_secondFootHold = sfh ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the foot you did NOT catch with stays off the board)");
    bool fl = g_footLevel != 0;
    if (api->Checkbox("Foot levels the board", &fl)) { g_footLevel = fl ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the deck rolls flat in step with the foot coming down)");
    if (api->SliderFloat("Boned ollie (%)", &bs, 0.0f, 300.0f, "%.0f")) CatchTweaks_SetBoneScalePct(bs);
    api->SameLine(); api->TextDisabled("(100 = stock, 0 = no bone at all)");
    {
        api->Indent();
        float ax = (float)g_boneAdd[0], ay = (float)g_boneAdd[1], az = (float)g_boneAdd[2];
        if (api->SliderFloat("Bone add X (cm)", &ax, -100.0f, 100.0f, "%.0f")) CatchTweaks_SetBoneAdd(0, ax);
        if (api->SliderFloat("Bone add Y (cm)", &ay, -100.0f, 100.0f, "%.0f")) CatchTweaks_SetBoneAdd(1, ay);
        if (api->SliderFloat("Bone add Z (cm)", &az, -100.0f, 100.0f, "%.0f")) CatchTweaks_SetBoneAdd(2, az);
        api->TextDisabled("added on top of the scaled bone -- raise whichever axis lifts the deck");
        api->Unindent();
    }
    bool ds = g_catchBeatsDS != 0;
        if (api->Checkbox("Catch input beats dark slides", &ds)) { g_catchBeatsDS = ds ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(fixes eaten catch inputs; manual only)");
        if (ds) {
            api->Indent();
            float g = (float)g_dsAngleDeg;
            if (api->SliderFloat("Darkslide zone (+- deg from grip-down)", &g, 10.0f, 170.0f, "%.0f")) { g_dsAngleDeg = (int)g; TwkMarkDirty(); }
            api->TextDisabled("Board inside the zone = darkslides work as stock; outside = catches");
            api->TextDisabled("resolve instantly at press, like playing with Dark Slides off.");
            snprintf(b, sizeof(b), "press-time zones: %d    darkslide zones honoured: %d",
                     (int)g_uiDsMasked, (int)g_uiDsHonored);
            api->TextDisabled(b);
            api->Unindent();
        }
    }
    const LONG md = g_uiCatchMode;
    if (g_manualMode < 0)
        snprintf(b, sizeof(b), "Catch Mode = %d -- ManualCatchMode UNSET in the ini, widening nothing", (int)md);
    else
        snprintf(b, sizeof(b), "Catch Mode = %d (%s)   widened: %s   defs: %d",
                 (int)md, (md == g_manualMode) ? "MANUAL" : "auto",
                 g_uiCatchWide ? "yes" : "no", (int)g_uiCatchDefs);
    api->TextDisabled(b);
}

// ---- per-trick board-rotation watch --------------------------------------------------------------
// Measures how far the DECK actually turns, from the moment a trick is selected until it stops
// moving. Deliberately independent of the catch path: keying this on the catch press missed the very
// reps worth seeing -- a bail ends the catch path, and a press that is held rather than tapped never
// produces an edge, so the failures hid while the clean catches all recorded.
//
// Summing |delta| per frame recovers the true rotation despite the angle being unsigned: it folds at
// grip-down and grip-up, and a fold reverses the sign of the step, not its size. At ~12 deg a frame
// there is no risk of stepping over a fold.

// ---------------------------------------------------------------------------------------------
// MEASUREMENT: did the game's own foot-catch actually plant a foot?
//
// "The board stops spinning and catches, but my foot doesn't attach to it" is two separate things,
// and until now the log only showed one of them (what WE did to the board). These are the game's
// own fields: each foot carries an FCatchFootInfo whose CatchRatio drives the catch pose, and the
// anim instance carries the HasLeft/RightFootCatchOrient booleans the pose actually reads. One
// summary line per catch, so a rep where the deck arrived but no foot did is visible as such
// instead of being inferred from what is missing.
// A 720 target means the game has committed to a DOUBLE flip, and a foot can then never catch at
// one revolution -- the ratio is measured against the full 720. Logging every CHANGE of the target
// says which of two things happens: 360 appearing once at trick start and later becoming 720 is the
// game EXTENDING the flip because the deck overshot; 720 appearing straight away is the game
// choosing a double from the input. The fix is completely different in each case, so this is the
// one number worth having before touching anything.
static void TraceFlipTarget(void* comp, float ang) {
    if (!comp) return;
    static float lastTgt = -99999.0f;
    const float tgt = twkF(comp, MC_BOARD_FLIP_TARGET);
    if (fabsf(tgt - lastTgt) < 0.5f) return;
    const float prev = lastTgt;
    lastTgt = tgt;
    if (prev < -99998.0f) return;                      // first sample: nothing to compare against
    TwkLog("[flip] target angle %.0f -> %.0f  (flip now %.0f, rate %.0f deg/s, deck %.0f deg)%s",
           prev, tgt, twkF(comp, MC_BOARD_FLIP_CUR), twkF(comp, MC_BOARD_FLIP_RATE), ang,
           (fabsf(tgt) > 540.0f) ? "   <-- DOUBLE FLIP" : "");
}

static void TraceFeetCatch(void* skater, void* comp, float ang) {
    void* anim = FootPlace_AnimInstance();
    if (!comp || !anim) return;
    const int state = twkB(skater, SK_CATCH_ORIENT_STATE);
    static bool  inCatch = false;
    static bool  sawL = false, sawR = false;
    static float peakL = 0.0f, peakR = 0.0f;
    static float angAt = 0.0f, curAt = 0.0f, tgtAt = 0.0f;
    static int   frames = 0, quiet = 0;

    // The forced-catch timers, captured fresh at the START of each catch. Fresh per catch because
    // the game may set them per trick -- scaling from a baseline taken this catch is correct either
    // way, and re-deriving from it every frame can never compound.
    static float baseTotal = 0.0f, baseForce = 0.0f, baseDelay = 0.0f;
    static int   typeLat = 0, typeRat = 0;
    // heldRejected is the honest test: if the write does not stick, this lever is the wrong one too.
    static int   heldFrames = 0, heldRejected = 0, heldAlready = 0;

    if (state != 0 && !inCatch) {
        inCatch = true; sawL = sawR = false; peakL = peakR = 0.0f; frames = 0; quiet = 0;
        // The engage LATENCY names the pop-time-catch bug directly: ~230 ms is the pop itself
        // (selection -> pop lag), anything close to that = the catch engaged AT the pop (the
        // held-stick ramp bug); a deliberate mid-flip catch reads hundreds of ms later.
        TwkLog("[feet] catch engaged (state %d) +%d ms after trick '%s'",
               state, FlipSpeed_MsSinceTrick(), FlipSpeed_LastTrickName());
        angAt = ang;
        curAt = twkF(comp, MC_BOARD_FLIP_CUR);
        tgtAt = twkF(comp, MC_BOARD_FLIP_TARGET);
        baseTotal = twkF(comp, MC_CATCH_TOTAL_TIME);
        baseForce = twkF(comp, MC_CATCH_FORCE_TIME);
        baseDelay = twkF(comp, MC_CATCH_TO_BOARD_DELAY);
        typeLat = twkB(comp, MC_LFOOT_CATCH); typeRat = twkB(comp, MC_RFOOT_CATCH);
        heldFrames = heldRejected = heldAlready = 0;
    }
    if (!inCatch) return;

    ++frames;
    // ---- HOLD THE SECOND FOOT. The foot WITHOUT the catch orient is the one you did not catch
    // with; clearing its catch TYPE to CF_None leaves it nothing to do until the board lands.
    // Written every frame because the movement tick recomputes it (UpdateFeetCatchInfo runs in the
    // physics pass, ahead of this animation-phase hook), so this follows rather than fights it.
    // Released the moment IsLanding goes true.
    // REMOVED: forcing the per-foot CatchRatio (lead=1, other=0) to pick the catching foot.
    // It did NOT change which foot catches AND it destroyed the catch outright -- measured
    // `L orient=no peak ratio 0.00 | R orient=no peak ratio 0.00  <-- NO FOOT CAUGHT` on every
    // attempt, and the un-caught foot then passed through the deck on landing. The ratio is an
    // output of the ramp, not the selector; writing it only removes a catch that was working.
    // The foot is chosen by the catch ORIENT, upstream of this component entirely.

    // The catch state drops back to 0 the moment it ends; a few frames of grace keep a one-frame
    // flicker from splitting one catch into two log lines.
    if (state == 0) ++quiet; else quiet = 0;
    if (quiet < 4 && frames < 600) return;

    inCatch = false;
    // A catch state that simply sits on while rolling is not a catch worth a line -- without this
    // the log filled with 600-frame summaries every 5 seconds, flip angle 0 throughout.
    if (fabsf(twkF(comp, MC_BOARD_FLIP_CUR) - curAt) < 1.0f && peakL <= 0.0f && peakR <= 0.0f) return;
    // The timers are printed as CAPTURED (start of catch) and as they read NOW. If they are live
    // state the pair differs; if they never move and scaling them changes nothing on screen, the
    // fields are not consulted and holding the second foot needs a different lever entirely.
    TwkLog("[feet] catch over %d frames: L orient=%s peak ratio %.2f | R orient=%s peak ratio %.2f "
           "| deck %.0f -> %.0f deg | flip angle %.0f -> %.0f (target %.0f)%s"
           " | footType L=%d R=%d -> %d/%d | timers total %.3f force %.3f delay %.3f"
           " -> %.3f/%.3f/%.3f%s",
           frames, sawL ? "YES" : "no ", peakL, sawR ? "YES" : "no ", peakR,
           angAt, ang, curAt, twkF(comp, MC_BOARD_FLIP_CUR), tgtAt,
           (!sawL && !sawR) ? "   <-- NO FOOT CAUGHT" : "",
           typeLat, typeRat, twkB(comp, MC_LFOOT_CATCH), twkB(comp, MC_RFOOT_CATCH),
           baseTotal, baseForce, baseDelay,
           twkF(comp, MC_CATCH_TOTAL_TIME), twkF(comp, MC_CATCH_FORCE_TIME),
           twkF(comp, MC_CATCH_TO_BOARD_DELAY),
           (g_secondFootHold && heldFrames)
               ? (heldRejected ? "   [2nd foot: WRITE REJECTED]" : "   [2nd foot held]")
               : (g_secondFootHold ? "   [2nd foot: nothing to hold]" : ""));
    if (g_secondFootHold)
        TwkLog("[feet]   second-foot hold: cleared on %d frames, rejected %d, already clear %d",
               heldFrames, heldRejected, heldAlready);
}

void CatchTweaks_PumpFrame() {
    // The flicked-foot record ages out HERE, above every early-out -- it must decay on ordinary
    // frames or a stale flick would still be naming a foot minutes later.
    if (g_flickFresh > 0 && --g_flickFresh == 0) g_flickPhys = 0;
    // Belt-and-braces latch release: the state==0 call is the normal end of a catch, but if the
    // setter simply STOPS being called instead, the latch would otherwise persist into the next one.
    if ((g_flickLatch != 0 || g_holdState != 0) && ++g_latchIdle > 10) {
        g_flickLatch = 0; g_latchIdle = 0;
        g_holdState = 0; g_holdLoggedState = 0;
        if (g_airCaughtFoot != 0) g_airCatchZeros = 99;   // setter went quiet = the catch ended
    }
    // THE "was this air a trick" LATCH IS MAINTAINED HERE, EVERY FRAME -- not only when
    // SetCatchOrient happens to be called. It used to be updated inside CatchAirIsTrick(), which the
    // hook calls only for a NON-ZERO orient state, so after a flip trick the latch could stay set
    // through the landing and into the NEXT air: that ollie was then treated as a trick, its orient
    // was overridden, and the board tilted the wrong way. It self-corrected because the following
    // air did see `grounded` -- which is exactly the reported "randomly once, then fine after".
    __try {
        void* an = FootPlace_AnimInstance();
        if (an) {
            if (twkB(an, AN_FLIPPING) > 0 || twkB(an, AN_ROTATING) > 0) g_airWasTrick = 1;
            else if (twkB(an, AN_GROUNDED) > 0) {
                g_airWasTrick = 0;
                g_airCaughtFoot = 0; g_airCatchZeros = 0; g_airSwallowLogged = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Several FIXES live in here alongside the diagnostic trace, so this must not early-out on the
    // trace flag -- doing so silently disabled a fix the moment logging was turned off for release.
    // Each part checks its own switch below, and every switch must appear here: leaving one out
    // quietly ties that feature to whichever OTHER switch happens to be on.
    if (!g_flipTrace && !g_stopFlip && !g_anyRev && !g_footLevel && !g_unstick && !g_minSpinDeg) return;
    __try {
        void* skater = g_lastSkater;
        if (!skater) return;
        const float ang = BoardGrip(skater);
        if (ang < 0.0f) return;
        static long  lastSerial = -1;
        static bool  watching = false;
        static float prev = 0.0f, travel = 0.0f;
        static int   still = 0, frames = 0;

        // ---- how far the board has actually turned since this trick started -----------------------
        // Maintained EVERY frame and independently of the trace, because the catch setter reads it.
        // The fresh-flick veto cannot cover AUTO catch -- there is no flick to time -- so a very slow
        // flip could be caught before the deck had turned at all. This is the measurement that rule
        // needs, and it is degrees rather than milliseconds so it holds however slowly the deck spins.
        {
            static long  spinSerial = -1;
            static float spinPrev = -1.0f;
            const long ser = FlipSpeed_TrickSerial();
            if (ser != spinSerial) { spinSerial = ser; g_trickTravelDeg = 0.0f; spinPrev = ang; }
            else if (spinPrev >= 0.0f) {
                const float d = fabsf(ang - spinPrev);
                if (d < 170.0f) g_trickTravelDeg += d;   // ignore the wrap at 360
                spinPrev = ang;
            }
        }
        // ---- the fix: a registered catch ends the flip at the first grip-up ----------------------
        // `_boardFlipRate` is zeroed rather than the angle being written: the deck is already AT the
        // orientation a completed flip ends on, so removing the rate leaves it exactly there and lets
        // every other system (landing, pitch, the catch's own alignment) carry on untouched.
        // The whole block is shared plumbing (comp/catchState/delta) for the stop, the flip-ending
        // AND the foot-level drive -- it must open on the whole family, or turning one off silently
        // kills the others (it did: the stop's toggle used to gate all three).
        if (g_stopFlip || g_anyRev || g_footLevel) {
            void* comp = CatchLevel_MovementComponent();
            const int catchState = twkB(skater, SK_CATCH_ORIENT_STATE);
            static bool  armed = false;
            static float prevAng = -1.0f;
            const float delta = (prevAng >= 0.0f) ? (ang - prevAng) : 0.0f;
            if (g_stopFlip && catchState != 0 && !armed) {
                armed = true;
                // If the deck is ALREADY flat when the catch registers, do nothing at all. There is
                // no second revolution to prevent -- the board has arrived -- and zeroing the flip
                // rate on the very frame the catch appears starves the game's own foot-catch, which
                // advances off the board's remaining travel toward its target. Observed exactly once
                // that ordering happened: deck stopped dead, feet never connected. A catch that lands
                // with the board already home is the game's business, not ours.
                if (ang >= (float)g_stopFlipDeg) {
                    armed = false;
                    prevAng = ang;
                    goto stopFlipDone;
                }
                // A caught board should come to the feet, not coast to griptape-up at full flip
                // speed. Measured: a catch registering ~245 deg short of grip-up spent 200 ms
                // getting there, which IS the extra flip the player sees. So the remaining travel
                // is compressed into `snapMs` -- the board finishes fast and stops, rather than
                // taking another revolution's worth of time to arrive.
                if (comp && fabsf(delta) > 0.01f) {
                    // Unsigned angle: rising means heading for grip-up, falling means it must pass
                    // grip-down first. Either way this is the distance still to travel.
                    const float remaining = (delta > 0.0f) ? (180.0f - ang) : (ang + 180.0f);
                    const float rate = twkF(comp, MC_BOARD_FLIP_RATE);
                    const float want = remaining / ((float)g_snapMs / 1000.0f);
                    // TWO LIMITS, both learned from a trick that visibly bugged out. A catch can
                    // register a long way from grip-up -- 326 deg was logged -- and "finish it in
                    // snapMs" then demanded 3623 deg/s from a board turning at 829. The deck whips
                    // round at four times its own speed and is stopped dead: that is the glitch, not
                    // a catch. Compressing the last stretch of a flip is reasonable; teleporting
                    // through most of a revolution is not.
                    //   * skip entirely past `snapMaxDeg` -- that is not a catch landing near home,
                    //     and the board is better left to arrive under its own power
                    //   * never boost by more than `snapMaxBoost` of the speed it already had, so
                    //     the acceleration always stays in proportion to the trick
                    const float maxByBoost = fabsf(rate) * (float)g_snapMaxBoost;
                    if (remaining > 40.0f && remaining <= (float)g_snapMaxDeg &&
                        fabsf(rate) > 1.0f && want > fabsf(rate)) {
                        float capped = want;
                        if (capped > maxByBoost) capped = maxByBoost;
                        if (capped > 2500.0f)    capped = 2500.0f;   // absolute ceiling
                        *(float*)((uint8_t*)comp + MC_BOARD_FLIP_RATE) = (rate < 0.0f) ? -capped : capped;
                        TwkLog("[catch] caught %.0f deg short -- finishing the flip in %d ms "
                               "(%.0f -> %.0f deg/s)", remaining, g_snapMs, fabsf(rate), capped);
                    }
                }
            }
            // ---- a caught board finishes its flip flat under the foot -----------------------
            // _boardFlipTargetAngle is a MAGNITUDE and does NOT share the sign of
            // _boardFlipCurrentAngle -- the logs show target +357 against current -356, and the
            // game's own ratio takes |target| and |current|. Comparing the two raw (to work out
            // which way the flip was going) is therefore meaningless, and doing it wrote garbage
            // targets: +356 became -332, and with no flip running at all a target of 0 became 27.
            // The aim is computed on magnitudes and put back on the TARGET's sign.
            static bool endedThisCatch = false;
            if (catchState == 0) endedThisCatch = false;
            if (g_anyRev && comp && catchState != 0 && !endedThisCatch &&
                ang >= (180.0f - (float)g_anyRevDeg)) {
                const float tgt  = twkF(comp, MC_BOARD_FLIP_TARGET);
                const float cur  = twkF(comp, MC_BOARD_FLIP_CUR);
                const float rate = twkF(comp, MC_BOARD_FLIP_RATE);
                const float owed = fabsf(tgt) - fabsf(cur);
                // Only ever touch a flip that is genuinely still running and still owes rotation.
                // Without this the write fired while simply rolling along -- the catch state sits
                // non-zero for long stretches -- and invented a flip target out of nothing.
                // ...and on AUTO catch, never cut a flip that has barely begun. Aiming at the nearest
                // flat is right for a board coming home; on a SLOW flip caught early it throws away
                // most of the trick -- field: a tre flip caught 52 deg in, "game wanted 303 more", and
                // the flip stopped as it started. If the board still owes more than this, let it finish
                // and let CatchStopsFlip end it at grip-up instead.
                // MANUAL is exempt (AutoCatchActive): there the catch is a deliberate press, and on an
                // over-rotation the foot is meant to attach and level the deck. Applying the limit
                // there stopped that -- the game just extended the target to a second revolution.
                if (owed > (float)g_maxCutDeg && AutoCatchActive()) {
                    static long cutLogged = -1;
                    const long ser = FlipSpeed_TrickSerial();
                    if (cutLogged != ser) {
                        cutLogged = ser;
                        TwkLog("[catch] caught at %.0f deg but the flip still owes %.0f -- letting it "
                               "finish rather than aiming it at flat", ang, owed);
                    }
                } else if (owed > 1.0f && fabsf(rate) > 1.0f) {
                    const float toFlat = 180.0f - ang;      // deck degrees still to roll to grip-up
                    const float aimMag = fabsf(cur) + toFlat;
                    const float aim = (tgt < 0.0f) ? -aimMag : aimMag;
                    if (fabsf(aim - tgt) > 1.0f) {
                        *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = aim;
                        endedThisCatch = true;
                        TwkLog("[catch] caught at %.0f deg -- aiming the flip at flat, %.0f deg away "
                               "(target %.0f -> %.0f, game wanted %.0f more)",
                               ang, toFlat, tgt, aim, owed);
                    }
                }
            }
            // ---- the foot levels the board -------------------------------------------------
            // On a parked over-rotation (endedThisCatch) the flip is frozen and the game's
            // roll-align has not started -- it only engages once a foot's CatchRatio passes 0.9,
            // AFTER the foot has landed on the tilted deck. Until then nothing drives the roll, so
            // the deck levels as a separate second motion. This drive fills that gap: the rendered
            // roll (comp+0x628) is slaved to the foot's own CatchRatio, smoothstepped, so the deck
            // rolls flat exactly in step with the foot coming down on it. Ratios are only READ --
            // writing one kills the catch outright. At 0.9 the game's align takes over with
            // ~nothing left to do; grounding or the catch ending releases the drive.
            {
                static bool  flArmed = false, flDone = false;
                static float flStart = 0.0f, flTarget = 0.0f, flLastWrite = 0.0f;
                static int   flFrames = 0, flStomped = 0;
                if (catchState == 0) {
                    if (flFrames > 0) {
                        TwkLog("[catch] foot-level drive: %d frames, roll %.1f -> %.1f (target %.1f)"
                               ", stomped %d frames%s", flFrames, flStart, flLastWrite, flTarget,
                               flStomped, (flStomped > flFrames / 2)
                                   ? "   <-- WRITE NOT STICKING, lever is wrong" : "");
                        flFrames = 0;
                    }
                    flArmed = false; flDone = false; flStomped = 0;
                }
                if (g_footLevel && comp && catchState != 0 && endedThisCatch && !flDone &&
                    !FootPlace_Grounded()) {
                    const float rl = twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO);
                    const float rr = twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO);
                    const float ratio = (rl > rr) ? rl : rr;
                    if (ratio >= 0.9f) {
                        flDone = true;                      // the game's own align owns it from here
                    } else if (!flArmed) {
                        // First driven frame: capture where the roll is and aim at the NEAREST
                        // level wrap -- the rotator roll accumulates degrees across the flip, so
                        // level is "the closest multiple of 360 (+ the authored target roll)", the
                        // short way round, same as the game's own quat slerp resolves it.
                        flStart = twkF(comp, MC_ANIM_ROLL);
                        float tgtRoll = 0.0f;
                        void* def = twkP(skater, SK_CURRENT_TRICK_DEF);
                        if (def) {
                            const float t = twkF(def, DEF_CATCH_TGT_ROLL);
                            if (t > -180.0f && t < 180.0f) tgtRoll = t;
                        }
                        flTarget = floorf(flStart / 360.0f + 0.5f) * 360.0f + tgtRoll;
                        flLastWrite = flStart;
                        flArmed = true; flFrames = 0; flStomped = 0;
                    } else {
                        // Stomp probe: if the value moved off what we wrote last frame (and the
                        // align is not active below 0.9), something else re-writes the roll after
                        // us and this lever does not stick -- the summary line says so.
                        const float now = twkF(comp, MC_ANIM_ROLL);
                        if (fabsf(now - flLastWrite) > 0.5f) ++flStomped;
                        float r = ratio / 0.9f;
                        if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
                        const float eased = r * r * (3.0f - 2.0f * r);   // a press: fast mid, soft end
                        const float roll = flStart + (flTarget - flStart) * eased;
                        *(float*)((uint8_t*)comp + MC_ANIM_ROLL) = roll;
                        flLastWrite = roll;
                        ++flFrames;
                    }
                }
            }
            if (catchState == 0 && ang > 170.0f) armed = false;
            // Once the flip is aimed at flat the target ends it there by itself; zeroing the rate
            // here would freeze the deck up to (180 - stopFlipDeg) short of level, which is exactly
            // the bent-ankle catch this pass exists to remove.
            if (armed && comp && !endedThisCatch && ang >= (float)g_stopFlipDeg && delta > 0.0f) {
                const float rate = twkF(comp, MC_BOARD_FLIP_RATE);
                if (rate > 1.0f || rate < -1.0f) {
                    *(float*)((uint8_t*)comp + MC_BOARD_FLIP_RATE) = 0.0f;
                    InterlockedIncrement(&g_uiFlipStops);
                    // All three axis readings, so "the deck was still sideways" is checkable: the
                    // correct long axis is the one that reads ~180 when the board is genuinely flat.
                    TwkLog("[catch] flip stopped at grip-up (%.0f deg, was %.0f deg/s) -- "
                           "no second revolution [axis %d; X%.0f Y%.0f Z%.0f]", ang, rate, g_flipAxis,
                           BoardGripAxis(skater, 0), BoardGripAxis(skater, 1), BoardGripAxis(skater, 2));
                }
                armed = false;
            }
            prevAng = ang;
        stopFlipDone: ;
        }

        // ---- unstick a wedged catch pose (base-game bug; see the knob comment) ------------------
        if (g_unstick) {
            static double susSince = 0.0, lastFix = 0.0;
            void* comp = CatchLevel_MovementComponent();
            const int st  = twkB(skater, SK_CATCH_ORIENT_STATE);
            const bool darkslide = (st >= 7 && st <= 9);
            const int ftL = comp ? twkB(comp, MC_LFOOT_CATCH) : 0;
            const int ftR = comp ? twkB(comp, MC_RFOOT_CATCH) : 0;
            // Catch VALUES are 1..253 -- 0 is CF_None, 254/255 the throwdown/seated sentinels. A
            // foot still holding one while riding, or an orient state still engaged, is the wedge.
            const bool catchTypes = (ftL >= 1 && ftL <= 253) || (ftR >= 1 && ftR <= 253);
            // The TYPES are the wedge. A nonzero orient STATE on the ground is a held stick --
            // grinds hold one deliberately (field round: firing on state alone re-seated the feet
            // mid-grind, the reported "feet move for a second in a grind"). Clean catches end with
            // both types back at 0, so types stuck in catch values are the broken pose.
            const bool suspicious = FootPlace_Grounded() && !darkslide && catchTypes;
            const double now = DsNow();
            if (!suspicious) susSince = 0.0;
            else if (susSince <= 0.0) susSince = now;
            else if (now - susSince > (double)g_unstickMs / 1000.0 && now - lastFix > 2.0) {
                lastFix = now;
                TwkLog("[feet] catch pose stuck %.1f s after grounding (state=%d types L=%d R=%d "
                       "ratios L=%.2f R=%.2f) -- ending the orient + re-seating the feet",
                       now - susSince, st, ftL, ftR,
                       comp ? twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO) : 0.0f,
                       comp ? twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO) : 0.0f);
                if (st != 0 && g_origSetOrient) hkSetCatchOrient(skater, 0, 0.0f, 0.0f);
                if (comp) *(float*)((uint8_t*)comp + MC_CATCH_TO_BOARD_DELAY) = 0.2f;
                susSince = 0.0;
            }
        }

        TraceFlipTarget(CatchLevel_MovementComponent(), ang);
        TraceFeetCatch(skater, CatchLevel_MovementComponent(), ang);

        if (!g_flipTrace) return;                 // the rest of this is the diagnostic trace only
        const long serial = FlipSpeed_TrickSerial();
        if (serial != lastSerial) {                    // a new trick was just selected
            lastSerial = serial;
            watching = true; travel = 0.0f; still = 0; frames = 0; prev = ang;
            return;
        }
        if (!watching) return;
        const float d = fabsf(ang - prev);
        if (d < 170.0f) travel += d;
        prev = ang;
        still = (d < 0.8f) ? still + 1 : 0;
        if (++frames > 900) still = 99;                // never watch forever
        if (still > 15) {
            watching = false;
            // A flip trick turns the deck 360. Anything near 720 did it twice -- reported as a count
            // rather than a raw number so a double is unmistakable without arithmetic.
            const float flips = travel / 360.0f;
            TwkLog("[fliptrace] %-22s deck turned %4.0f deg = %.2f flip(s)%s",
                   FlipSpeed_LastTrickName(), travel, flips,
                   (flips > 1.4f) ? "   <-- SECOND FLIP" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_flipTrace = 0; }
}
