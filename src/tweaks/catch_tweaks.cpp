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
#include "grind_pop.h"       // FName -> string, to name the key a press arrived on
#include "pop_probe.h"       // PopProbe_SkaterManualBits -- the skater's manual latch, for the probe
#include "run_out.h"         // RunOut_BailCalls -- did the game's own catch verdict Bail inside SetCatchOrient
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
    MC_BOARD_ROT_TARGET     = 0x788,   // _boardRotationTargetAngle -- the shuv axis; UpdateFeetCatchInfo
    MC_BOARD_ROT_CUR        = 0x78c,   // _boardRotationCurrentAngle   runs the same ratio math on it
    MC_BOARD_ROT_RATE       = 0x780,   // _boardRotationRate -- the shove axis's own rate (PDB layout:
                                       // rate 0x780, target rate 0x784, target 0x788, current 0x78c)
    MC_BOARD_FLAGS          = 0x7e9,   // the _isBoard* bitfield byte. UpdateFeetCatchInfo applies the
                                       // flip window only while bit 0x08 is set and the rotation window
                                       // only while 0x20 is -- the two bits it clears once attached.
    AN_ON_BOARD             = 0x300,   // USkaterAnimInstance::IsOnBoard -- riding, not walking
    AN_IS_SWITCH            = 0x303,   // USkaterAnimInstance::IsSkatingSwitch -- front foot swaps
    AN_IS_LANDING           = 0x5fd,   // USkaterAnimInstance::IsLanding -- the landing PHASE, true
                                       // from the catch onward while still airborne (measured: the
                                       // scoop hold released on it 0 frames after arming, eight
                                       // times in 50 ms). NOT touchdown. Its neighbours are
                                       // IsFalling 0x5fc / IsJustLanded 0x5fe; touchdown is
                                       // IsGrounded 0x5fa. Nothing should release on this.
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
    DEF_MANUAL_FLIP_TOL     = 0x290,   // UFlipTrickDefinition::CatchManualFlipAngleThreshold     (ships 120)
    DEF_MANUAL_ROT_TOL      = 0x294,   // UFlipTrickDefinition::CatchManualRotationAngleThreshold (ships 120)
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
// CatchWindowMult (ini only, default 100 = as shipped): multiplier on the per-trick FEET-ATTACH windows
// (BoardFlipPreCatchAngle / BoardRotationPreCatchAngle). Those gate when the feet take the deck, not
// whether a catch is good -- the snap and the descent make them near-invisible now -- and the menu's
// "catch window" slider drove this for a long time while the real window sat below, which is why it
// never seemed to do anything. Kept as a key for anyone who wants it.
static float g_catchMult   = 1.0f;
// CatchManualFlipTolDeg / CatchManualRotTolDeg: the game's OWN bad-catch verdict, tightened. On manual
// catch, SetCatchOrient judges every new engage -- the deck's up against the trick's expected up --
// and Bails MID-AIR when it is outside the def's CatchManualFlipAngleThreshold (the rotation axis
// likewise against CatchManualRotationAngleThreshold). Every def ships 120/120: a deck 110 deg from
// flat still passes, which is exactly why an early press never bailed. Field, five kickflips with the
// verdict's inputs otherwise identical: the game passed the deck at 61, 68 and 107 deg from flat and
// bailed it at ~120 and 153. So the verdict works as shipped; the shipped tolerance is far looser than
// "catch it upside down and you bail". Written into the defs alongside the window widen (same
// lifetime: manual catch only, put back the moment it is not). 0 = the def's own value.
// This IS the manual catch window, and the menu's "Catch window" slider drives it. The rotation half
// (shove tricks) follows the flip half unless CatchManualRotTolDeg is set in the ini.
static int   g_manualFlipTol = 120;   // the field's value (with the over-rotation bail at 70 and the
                                      // shove sideways band at 10 carrying the "past flat" side)
static int   g_manualRotTol  = 0;     // 0 = same as the flip tolerance
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
// How flat the deck has to be before a held catch pose counts as WEDGED. It is the cosine of the
// deck's tilt: 1.0 is perfectly flat either way up, 0.0 is standing on its edge. Below this the
// board is on its side -- a primo -- where holding a catch pose is the trick, not a fault.
static float g_unstickFlat = 0.60f;
// How close together two sticks have to START deflecting to count as ONE deliberate gesture.
// Thumbs never land on the same frame, so a two-stick input always begins as a one-stick one --
// which is why both of the narrowing features below could not see it.
static int   g_twoStickMs  = 250;   // CatchTwoStickMs
// KEEP THE POSE RATIOS WHEN NARROWING A TWO-STICK ORIENT. Narrowing hands the game state 1 or 2
// with pitch and yaw of ZERO, i.e. a neutral catch pose -- and a mid-scoop catch, which is the case
// that narrows, comes out with the catching foot in what reads as a MANUAL foot position (field).
// Turning the narrowing off proves where the pose comes from: the game's own two-foot orient (11)
// places the feet correctly, so it is the state forcing, and most likely its zeroed ratios, that
// spoils it -- but the narrowing is also what lets ONE foot catch while the scoop stays held out,
// which is the whole feature, so it cannot simply be dropped.
// The zeroing is not gratuitous: forwarding the ratios of a two-stick orient onto a one-foot pose
// once read as the board PITCHING NOSE-DOWN on an otherwise perfect catch, because those numbers
// encode the held scoop stick's deflection. So this is a switch, defaulted to the shipped
// behaviour, and the round that flips it should watch for that pitch as much as for the foot.
static int   g_narrowKeepRatios = 0;   // CatchNarrowKeepRatios
// HOLD THE SCOOP-SIDE FOOT OFF A TWO-FOOT CATCH, instead of narrowing the catch to one foot.
// Catching with one foot while a scoop is held used to NARROW the game's two-foot orient (11) to
// the flicked foot. That gives the one-footed catch the feature is for -- but state 1's orient
// definition places the board differently from state 11's, and the catching foot comes down on the
// NOSE instead of over the bolts (field, screenshots). With the narrowing off, state 11 places both
// feet correctly and the scoop foot comes down too. Neither ratios nor foot choice are the cause:
// keeping the ratios changed nothing, and inverting the foot made the SCOOP foot catch, which then
// had nothing holding the other one out.
// So: keep state 11 -- the game's own placement, proven right -- and hold the scoop-side foot off
// the deck by clearing its catch TYPE each frame until the board lands. That is the write the
// SecondFootHold note described and nobody ever implemented (its counters were never incremented).
// Written in the animation-phase detour, AFTER UpdateFeetCatchInfo has recomputed the type in the
// physics pass, so it follows the game rather than fighting it. Opt-in until the write is proven:
// the release line reports frames, writes and the held foot's peak ratio, and a ratio that stays at
// zero IS the proof.
static int      g_scoopHold       = 1;      // CatchScoopFootHold -- ON: mode 4 below is the fix for
                                            // the nose/tail foot on a mid-scoop catch, a shipped feature
// WHICH FIELD IS THE LEVER for keeping a foot off. CatchScoopHoldMode:
//   0 = write nothing -- state 11 goes through untouched and the probe below records what every
//       candidate field does across the catch (a measurement round);
//   1 = clear the scoop foot's HasXFootCatchOrient flag on the anim instance each frame -- the
//       per-foot flag SetCatchOrient derives from the state, and the last untested candidate;
//   2 = clear the foot's catch TYPE byte on the movement component (MEASURED NOT to hold: 44
//       writes, ratio still ramped to 1.00; kept only as the A/B reference).
static int      g_scoopHoldMode   = 4;
static int      g_scoopHoldZeros  = 0;      // consecutive state-0 reads -- a flicker is not an end
//   3 = BORROW THE BOARD OFFSETS. Both hold levers are measured dead (type byte: 44 writes, ratio
//       ramped to 1.00; anim flag: stayed cleared all catch, ratio ramped anyway), and the probe
//       shows the game itself settling on state 1 for a mid-scoop catch (5 -> 11 -> 3 -> 1 by
//       frame 18) with none of our converters firing. So the one-foot catch is the game's own
//       outcome, and the remaining difference between it and the correctly-placed two-foot catch
//       is WHICH ORIENT DEFINITION the state selects: each FCatchOrientDefinition carries four
//       BoardRelativeOffset vectors that seat the deck under the skater. Mode 3 narrows as before
//       (one foot, proven) and, for the catch, copies the from-state's offsets into every def of
//       the narrowed state -- restored on release. The arm line prints BOTH sets of offsets, so if
//       they turn out identical the hypothesis is dead on the spot, no headset needed.
static int      g_scoopBorrowFrom = 11;     // CatchScoopBorrowFrom -- whose offsets to borrow
// THE HELD SCOOP STICK IS ALSO THE MANUAL INPUT. Every lever on the catch side is measured dead
// -- type byte, anim flag, and now the orient definition's board offsets (they differed, were
// written, and the foot still went to the nose). What the field actually reports is the skater's
// BODY shifting to ready a nose manual, and the probe's one constant signature is the catching
// foot's type going 1 -> 4 at frame 12 of every scoop catch, where a normal catch stays at 1. A
// stick held past the manual threshold IS Session's manual request, and our two-stick mask only
// hides it from the CATCH decision, one call deep. So this gate hides it from the MANUAL decision
// too: while a scoop catch is live, ASessionPlayerController::Skate_CheckForManuals is skipped --
// the same refusal pop_probe's manual gate already makes, in the same hook -- and for a short tail
// after touchdown, because the game latches manuals at the landing.
//   4 = GIVE THE CATCHING FOOT THE PLAIN FOOT TYPE once the orient has settled to one foot.
//       CORRECTED after the DB dump: the one-stick orient states are AUTHORED as pitched catches
//       (state 1 -> CF_Nose, state 2 -> CF_Tail; only 10/11 carry plain Left/RightFoot), and the
//       game downgrades them to a plain foot only when _catchOrientPitchRatio points the other
//       way (Nose kept while pitch >= 0, Tail kept while pitch <= 0). A pitch of exactly ZERO
//       keeps both -- and zero is what the narrowing writes when it drops the two-stick ratios,
//       which is the whole bug. The first cut of this mode recomputed the game's own answer and
//       so wrote the Tail back. It now writes the DOWNGRADED value: the plain foot for the state,
//       stance-remapped. Older reasoning kept below for the record:
//       Disassembled
//       (UpdateFeetCatchInfo, ConvertCatchFootToStance, GetCatchOrientCatchFootType): the foot type
//       is the ORIENT DEFINITION's authored CatchFootType for the CURRENT state, stance-remapped
//       (goofy ^ switch: 1<->2, 4<->5, 6<->7), and once nonzero it is PRESERVED frame to frame
//       (`cmovne` keeps the previous value). A mid-scoop catch passes through transient two-stick
//       states (5 / 11 / 3) that a normal catch never visits; the type is decided there as 4 and
//       then preserved after the state settles to 1 -- which is exactly what the probe shows (1 ->
//       4 at ~frame 12, state 1 thereafter). A normal catch decides 1 and keeps 1. So this mode
//       recomputes the one-foot value the game would have chosen and writes it; the game's own
//       preserve logic keeps it from there.
static int      g_scoopDbDumped   = 0;      // the orient DB's state -> foot-type table, once
static int      g_scoopNoManual   = 0;      // CatchScoopNoManual -- OFF: measured to fix nothing
                                            // (man=0 throughout, type still 1 -> 4, foot still
                                            // on the nose) and to BREAK mid-air foot control.
                                            // Skate_CheckForManuals does more than latch manuals.
static LONGLONG g_scoopLiveUntil  = 0;      // the post-landing tail
static void*    g_codbCached      = nullptr;// the orient DB, cached from the decide hook
static uint8_t* g_borrowSrc       = nullptr;// the def being borrowed from
static uint8_t* g_borrowDst[8];             // the defs being written (one per matching state)
static float    g_borrowSave[8][2][4][3];   // their originals: [def][side][vector][xyz]
static int      g_nBorrow         = 0;
static void     ApplyBorrow();              // defined below; re-applied from an earlier hook
static int      g_scoopHoldFoot   = 0;      // 1 = left, 2 = right -- the foot being held off
static int      g_scoopHoldFrames = 0, g_scoopHoldWrites = 0;
static float    g_scoopHoldPeak   = 0.0f;   // the held foot's CatchRatio, worst case
static LONGLONG g_scoopHoldQpc    = 0;

// ---- CLICK A STICK TO CATCH ----------------------------------------------------------------
// Press a thumbstick to catch instead of flicking for it. The press does NOT reach into the catch
// system: it stands in for the flick the game is already looking for, by presenting the mapped
// stick as freshly deflected for the ONE call that decides. Everything downstream -- which foot
// catches, the fresh-flick veto, the darkslide mask, the pre-catch angle window -- then behaves
// exactly as it does for a real flick, and every other setting on this page still applies.
//
// The synthetic push is deliberately SMALL, just past the game's own deadzone: the orient's pitch
// and yaw ratios come from how far the stick is pushed, so a gentle push reads as a level catch
// rather than a steered one.
//
// The press is swallowed ONLY while on the board, and only when the catch path is actually hooked
// -- blocking somebody's button without being able to deliver the catch it was blocked for is the
// one outcome worth ruling out. Off the board it is handed straight to the game.
static int   g_clickCatch   = 0;      // CatchClickToCatch -- opt-in: it rebinds a button
static float g_clickMargin  = 0.15f;  // how far past the deadzone the synthetic push goes
static float g_clickDirX    = 0.0f;   // the direction of that push, in stick space
static float g_clickDirY    = -1.0f;
static int   g_clickHoldMs  = 250;    // a press that finds no air this soon simply lapses
static volatile long g_clickArmed = 0;   // 1 = left stick pressed, 2 = right
static int   g_clickFrames  = 8;      // decisions a press holds the stick out for -- see below
static int   g_clickHold    = 0;      // how many of those are left
static int   g_clickWhich   = 0;      // and which stick they belong to
// WHICH WAY THE GAME WANTS THE STICK PUSHED is not in the orient database -- those entries carry
// the resulting pose (yaw, pitch, board offsets) and the state, not the input that selects them.
// So the press SWEEPS: each held decision pushes a different compass direction at a near-full
// deflection, with the edge tracker cleared each time so every one gets a fair test, and the
// verdict is logged beside the direction that produced it. One press tries all eight. Once the
// log names a direction that returns non-zero, this goes off and that direction becomes the push.
static int   g_clickSweep   = 0;   // a measurement mode; the ini default is 0 and so is this
static float g_clickSweepMag = 0.9f;  // a real flick is a big movement, not a nudge
static const float kClickDirs[8][2] = {
    {  0.0f, -1.0f }, {  0.0f,  1.0f }, { -1.0f,  0.0f }, {  1.0f,  0.0f },
    { -0.707f, -0.707f }, {  0.707f, -0.707f }, { -0.707f,  0.707f }, {  0.707f,  0.707f },
};
static float g_clickDirNow[2] = { 0.0f, 0.0f };   // what this decision is being handed
static int   g_clickStepNow = -1;
// FORCING THE ORIENT AT ITS SETTER -- the route the stick injection should have been all along.
// Feeding a synthetic deflection into the decision could never be verified: the sweep proved the
// value being read back was not a per-call verdict at all (all eight directions returned the same
// number within one press, 0 or 2 -- it is the CURRENT catch state, not a decision). This module
// already rewrites the catch state at ASkaterCharacterBase::SetCatchOrient for the flicked-foot
// fix, which rides the game's own call and so cannot be lost to ordering. A press turns that call's
// state 0 into a real catch state for a moment, and if the game does not call the setter at all
// while idle, the pump drives it instead.
static int      g_clickForce      = 1;      // CatchClickForceOrient
static int      g_clickInject     = 0;      // CatchClickInject -- the old stick route, off
static int      g_clickForceMs    = 250;    // how long a press keeps forcing
static LONGLONG g_clickForceUntil = 0;
static int      g_clickForceState = 0;      // 1 = left foot, 2 = right
static long     g_clickSetterSeen = 0;      // did the game call the setter this frame
static LONGLONG      g_clickQpc   = 0;
static long          g_uiClickCatches = 0;
// A SELF-LIMITING PROBE, live while the option is on and spent after this many lines. Which half
// of the feature is failing is not guessable from outside: either the presses never reach the hook
// (the game reads the pad somewhere else), or they reach it under names we do not match, or they
// match and the board test rejects them, or the press arms and no air ever spends it. One line per
// key event names all four.
static long g_clickProbe = 0;         // CatchClickProbe -- lines to log, for naming a new pad
// WHICH KEYS COUNT AS A STICK CLICK. Not a constant, because a pad can enumerate TWICE: the field
// log shows every press arriving under BOTH a Gamepad_* name and a GenericUSBController_* one. Only
// swallowing one of them would leave the other to fire the bound action anyway, so each stick gets
// two name slots, and any pad can be named from the probe lines without a code change.
static char g_clickKeyL [64] = "Gamepad_LeftThumbstick";
// The pad's OTHER name for the same click. Correlated from the probe, same millisecond:
// Gamepad_LeftThumbstick fires with GenericUSBController_Button11 and the right one with
// Button12 -- the standard DirectInput ordering (1-4 face, 5-6 shoulders, 7-8 triggers, 9 select,
// 10 start, 11 L3, 12 R3). Both copies have to be swallowed or the bound action still fires off
// the one we let through. Any pad that disagrees can name its own in the ini.
static char g_clickKeyL2[64] = "GenericUSBController_Button11";
static char g_clickKeyR [64] = "Gamepad_RightThumbstick";
static char g_clickKeyR2[64] = "GenericUSBController_Button12";
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
// CatchFootLevelsBoard ("Over rotation leveling"): a board caught PAST flat is rolled BACK to flat
// the short way, the foot coming down with it, so it never attaches to a tilted deck.
//
// THE BUG THIS REPLACES, found on the seventh look (3.19.244 log): BoardGrip is UNSIGNED -- 180 is
// flat and it cannot tell 36 deg before flat from 36 deg past it -- but its direction of change can:
// it RISES toward 180 before flat and FALLS away from it after. The snap reads that (that is what
// its "remaining" is). The aim-at-flat did not: it aimed the flip FORWARD by (180 - angle) whatever
// the direction, so a deck 36 past flat and falling was aimed 36 further on, the foot attached at 72
// past flat, and the game's roll-align twisted the deck 74 deg back under the planted foot. Every
// earlier attempt chased a mapping between the flip integrator and the rendered angle; there was
// nothing to map -- the sign was sitting in `delta` all along.
//
// THE DRIVE, a mirror of the under-rotation snap: on a new engage with the deck heading AWAY from
// flat, up to 180 -- the short way round -- the flip RATE is reversed (only the rate moves the
// deck -- the integrator adds rate*dt and the rendered roll follows the same step) at the speed the
// snap would use for the same distance, and every frame the flip TARGET is placed exactly one
// "remaining" ahead of the current angle with the def's attach window written to the ORIGINAL
// overshoot. Two things fall out of that one placement: the game's step clamp (|step| <= target -
// current, live during a catch) lands the deck exactly on flat with no overshoot, and the game's
// own foot ratio, 1 - owed/window, climbs from 0 at the engage to 1 as the deck comes level -- the
// foot descends WITH the board, by the game's own formula, nothing forced (writing a ratio kills the
// catch). At flat the rate is zeroed and the target brought to the deck (owed 0 = planted), then the
// game's roll-align finishes the last degree. Ratios are only read. If the reversed rate ever moves
// the deck the WRONG way (a stance mirror this cannot see from here), the drive aborts after three
// widening frames, plants where it is and says so in the log.
// Under-rotations keep the snap + descent they already had. Needs "Foot always attaches" on (it is
// the same promise, one direction over). Both catch modes. The old lockstep roll write this knob
// used to gate never stuck (the flip update re-stamped it every frame) and is gone.
static int   g_footLevel    = 1;      // CatchFootLevelsBoard
// (CatchOverMaxDeg is GONE. Past the cap the board was left to "flip on round to the next flat", and
// that path failed every time it ran -- 14 of 14 in the 3.19.254 log: a double kickflip that never
// finishes in the air, landing upside down with no plant. It was also the answer to "why does it
// default to a double kickflip after a certain amount of over-rotation": a saved ini carried the old
// 120 over the shipped 175. Below 180 the short way round is always the roll-back; past 180 the
// nearest flat genuinely is the next revolution and the game's own target stands.)
// CatchOverMs: the roll-back's OWN time budget. An over-rotated catch is late in the air by definition
// -- field (3.19.250): 70-85 ms to touchdown, and at the snap's 90 ms budget the roll-back was cut off
// with 20-70 deg still to go, every time. Short, and capped only by an absolute ceiling: the speed
// here is bounded by the air left, not by how fast the trick was flipping.
static int   g_overMs       = 40;
// CatchAnyRevLegacyAim: the OLD aim-at-flat for under-rotations, which measured the distance to flat
// off the rendered deck angle and so over-aimed by the render lag (30-50 deg past flat on every tre
// flip in the 3.19.249 log; the game's align then twisted the deck level under the planted foot).
// The counter-based aim replaces it: the nearest flat AHEAD of the counter is the target the game
// already has unless it extended it, so for a plain under-rotation nothing is written and the flip
// ends exactly level. Kept for one A/B round in case the old feel is missed. 0 = counter-based.
static int   g_anyRevLegacy = 0;
// CatchShoveSnap: the snap, on the SHOVE axis. The foot ratio is min(flip term, rotation term), and a
// shove caught SHORT keeps turning at the game's own decaying rate with nothing hurrying it for the
// catch -- base-game behaviour. Field (3.19.253, inward heel): caught with the shove 53 deg short at
// 262 deg/s; the flip was flat in four frames, the shove was still 13 short at touchdown, the ratio
// sat at 0.34 and the foot stayed in the trick pose all the way down. So: the rotation still owed is
// finished within CatchSnapMs -- MAGNITUDE only, the game's own sign and target kept (the rotation
// rate's sign does not follow the counter's direction: measured cur 127 -> 167 at rate -262) -- and
// the descent opens the rotation window to the owed amount so the ratio climbs as the shove
// completes. Pure shoves and shove+flips alike. An over-rotated shove is left alone: its term clamps
// to 1 and is never the blocker. Absolute ceiling 1500 deg/s; a shove already fast enough is left at
// its own speed and only gets the window.
// CatchShoveFixes: the master switch for everything this module does on the SHOVE axis of a catch --
// the snap (an under-rotated shove finished in time for the foot), catching a shove where it is
// (under- or over-rotated), the sideways band bail, the shove-axis plant fix, and the sideways socket
// hold that rides on the stop. Off = the game's own shove catching, exactly as shipped. The flip axis
// (roll-back, aim-at-flat, over-rotation bail) is a separate family and is not touched by this.
static int   g_shoveFixes   = 1;
static int   g_shoveSnap    = 1;
// CatchShoveSnapMaxRate: the snap's speed ceiling on the shove axis, deg/s. 720, not the 1500 it
// shipped with: the board MESH trails the game's counter by ~85 ms, so a shove finished at 1500 had
// the foot planting (on the counter) with the visible board still 60-100 deg short, which then spun
// the rest of the way under the attached foot -- field: "the board mechanically connects to the foot
// and magically finishes the rotation". The game's own shoves run 500-900 deg/s; at that speed the
// mesh keeps up and the board arrives WITH the foot. A shove too far short to finish at this speed
// seats on the landing instead, which is what a badly under-rotated shove does anyway.
static int   g_shoveSnapMaxRate = 720;
// CatchShoveFinishMaxDeg: a shove caught short by MORE than this is caught WHERE IT IS -- the rotation
// stops and the target comes to the counter, so the foot lands on an under-rotated board (the
// sketchy catch it is). Short by this much or less, the snap finishes it. Field (varial heel, shove
// 84 short, snap at 720): the counter reached 180 with the foot planting on it, and the board mesh --
// ~85 ms behind -- was still ~60 short and spun the rest of the way under the attached foot. No finish
// speed fixes that: the mesh needs ~3 time constants after the counter stops, more air than a late
// catch has, so any real deficit finished forward reads as "the board magically completes under the
// foot". Catching it where it is puts the foot on the board the player actually sees. 0 = always
// catch where it is, 180 = always finish (the old behaviour).
static int   g_shoveFinishMaxDeg = 30;
// CatchShoveRollBack ("shove stops where it is caught"): the game extends the shove target the moment
// the shove runs past it uncaught, exactly as it extends the flip (field: hardflips authored at 160
// engaging with the target at 320 and the counter at 182-225; tre flips 360 -> 540 at 368/388) --
// that is "hold the shove and it does a 360", and it is why an over-rotated 180 finished the rotation
// (the shove snap then hurried it there). The trick's AUTHORED shove is captured at the pop, before
// the game can extend it, and a catch with the shove past that mark by less than half the extension
// STOPS the rotation where it is and brings the target to the counter: owed 0, the foot takes the
// board at that yaw. Past the halfway point the extended shove is finished forward, as before.
// A ROLL-BACK was built first (3.19.258) and is not possible: the rotation rate's sign is not a
// direction on this axis -- flipping it made the counter climb FASTER (10 deg/frame at -1260), ten of
// ten catches -- so there is no lever to turn a shove back. The user's call, and the realistic one: a
// shove caught past its mark is caught there.
static int   g_shoveRollBack = 1;
// CatchShoveBailBandDeg: a shove caught this close to SIDEWAYS -- 90 deg of yaw on the counter, mod 180
// (270 on a 360 shove is the same place) -- is bailed on the spot. That band is where the game cannot
// decide which end of the board each foot belongs to: the catching foot's socket wanders between the
// two ends, and three rounds of holding or placing it (3.19.265-270) either missed it or broke normal
// catches. The user's call: a catch there is a bail. Through run_out's mid-air policy (run-out where
// enabled, else the game's own Bail), manual catch only like the other bails. 0 = off.
static int   g_shoveBailBand = 10;    // the field's value: 30 bailed every hardflip (its shove sits ~16 from sideways)
// (CatchShoveStopMaxDeg is GONE. It sent a shove caught more than 60 past its mark on to the NEXT
// full mark -- for a 180 that is 360, the board turning back to face the way it started, which the
// field saw as "over-rotations trying to straighten out". It existed for the nose/tail-flip theory,
// which the type-flip counter disproved and the sideways socket hold now covers. Every over-rotated
// shove stops where it is caught and lands like that.)
static bool  g_shoveRbClaimed = false;   // this catch belongs to the stop: the snap stands down
static bool  g_shoveSnapTried = false;   // the snap's one-decision-per-catch latch (reset by an abort)
static int   g_overTrace    = 0;      // CatchOverTrace -- one line per frame of a roll-back / shove snap;
                                      // proven in the field (3.19.252-272), opt-in like every diagnostic
static bool  g_overActive   = false;  // a roll-back is running: the snap, aim-at-flat, plant fix and
                                      // descent stand down for this catch
// CatchOverBailDeg: the OVER-rotation side of the manual catch window, on its own. The game's verdict
// (the "Catch window" slider) is symmetric: tightening it to refuse a deck caught far PAST flat also
// refuses the same distance SHORT of flat, which is the early catch the player wants generous (field:
// "decreasing the catch window sorta helps but makes early catches almost impossible"). Measured on
// the catch's first frame off the RENDERED deck, past side only: the deck heading away from flat by
// more than this is bailed on the spot -- run_out's mid-air bad-catch policy: run-out where enabled,
// else the game's own Bail. 0 = off, the window rules both sides. Manual catch only, like the verdict.
// WHY THE RENDERED DECK and not the counter the roll-back uses (3.19.255 field, two symptoms): the
// counter runs 60-100 deg ahead of the render at flip speed, so a counter threshold fired on catches
// that LOOKED early (counter 40 past, deck 44 short and rising -> bail at slider 5); and a double
// kickflip is a single whose target the game extended to 720, so any "past the trick's flat" reading
// puts a double caught near 720 hundreds of degrees over and bails it at every setting ("doubles
// almost impossible"). The rendered deck is the frame the game's own verdict judges in and the one the
// player's eye reads; a deck still rising toward flat is short, whatever the counter says, and a
// double caught short of its second flat is short too.
static int   g_overBailDeg  = 70;     // the field's value (0 = off)
// CatchPlantFix: bug repair, always on (menu policy: no row, ini kill-switch). A STOPPED board
// must not owe rotation: each foot's CatchRatio is computed against _boardFlipTargetAngle, so
// any path that halts the deck while the target still exceeds the current angle freezes the
// ratio partway -- which IS a foot frozen partway down, hovering above the deck until landing
// wedges it and CatchUnstick repairs it a second later (field: user screenshot, catch pose
// with the foot just off the board for seconds). The AnyRev retarget prevents this INSIDE its
// level-out window; this releases the ratio everywhere else, the moment the deck stops.
static int   g_plantFix     = 1;
// CatchFootDescends: bug repair, always on (ini kill-switch only, like CatchPlantFix). Each foot's
// CatchRatio is `1 - owed / window`: owed = |flip target| - |flip current| (same again on the
// rotation axis), window = the def's pre-catch angle -- 20 deg on a tre flip after the x2 widen. A
// press-time catch engages the POSE with the deck still owing 70-100 deg, so the ratio is pinned at
// 0 until the last 20 deg of the snap: the foot hangs in the catch pose above a spinning board and
// drops in the final frames (field: front foot on a tre flip, ratio 0.00 for 100 ms, planted at
// +145 ms -- "the foot floating on catch"). The aim-at-flat retarget re-opens the gap mid-catch and
// pins it a second time. This drive makes the ratio a monotonic function of the board's PROGRESS:
// at engage the def's window is written to the rotation still owed, so the ratio climbs from 0 as
// the deck comes round and reaches 1 as it arrives -- the foot comes down WITH the board. When a
// retarget raises the owed amount, the window is re-solved to hold the ratio where it is
// (window = owed / (1 - ratio)), so the foot never lifts back up. UpdateFeetCatchInfo reads the def
// live every frame (rsi = skater+0x590), so a per-frame write is enough; the widened value goes
// back the moment the ratio arrives or the catch ends. Ratios are only READ (writing one kills the
// catch -- measured); only the divisor moves. A catch pressed inside the window is left alone.
static int   g_footDescend  = 1;
// CatchDescendDeg: how much spin the descent may span. Field: a kickflip caught 223 deg early opened
// the window to all 223, so the foot started down onto a board that was upside down and rode it round.
// The window now opens to at most this much -- the foot hovers until the deck is within it, then comes
// down over the last stretch. (The retarget hold above the cap stays: it only ever preserves a ratio
// the foot already has.)
static int   g_descendDeg   = 90;
// Set by the snap when it rescues a catch (pressed within CatchSnapMaxDeg of home); cleared when the
// catch ends. The descent serves ONLY those: on a press beyond the snap's range the feet keep the
// rig's own window, so a deck that never comes round is never attached to (the stock failure).
static bool  g_catchRescued = false;
// CatchOverRotationRescue: the snap's mirror. A board caught a little PAST flat used to get no help at
// all -- worse, the aim-at-flat measures the deck with an angle that cannot tell 20 deg before flat
// from 20 deg past it, so it aimed an over-rotated flip FURTHER forward; the foot then took a tilted
// deck and the game's roll-align twisted it back under the foot (field, screenshot). Read once per
// catch at the engage frame off the flip integrator's OWN angle: past the nearest flat by up to
// CatchOverMaxDeg, the flip is stopped where it is, its target parked far ahead (attach ratio 0 --
// the foot is held off), and the angle is walked BACK to flat at CatchOverRollRate deg/s; the
// rendered roll follows the angle, so the deck rolls back level with nothing on it, and the target
// then comes to the deck so the foot takes a flat board. Both catch modes.
// CatchEarlyMiss: on manual catch, a press with the deck farther from grip-up than the snap will
// rescue (CatchSnapMaxDeg) is a MISS. Field, three rounds: with the rescues off the game's own rule
// still let an upside-down press through (the orient ends with the flick or outlives it, the board
// finishes on its own, a flat deck at touchdown lands fine -- stock only fails what is still wrong at
// touchdown); freezing the deck where it was pressed (3.19.235) let the game's roll-align level it
// and the player re-catch. So the press itself is refused at the setter, the whole air is lost to
// catching (a miss is a miss whatever the board does afterwards), and the landing is a bail: the
// pending miss is handed to run_out's pump at touchdown, where its own low-air run-out policy applies.
// Stricter than stock by design -- it is the user's rule for manual catch. Auto catch untouched.
// OFF (3.19.237): the user wants the GAME'S OWN immediate bad-catch verdict (SetCatchOrient Bails a
// bad manual catch mid-air), which does fire for wrong-time catches but not for early ones. This
// landing-time miss is not that; it stays as a switch while the verdict is measured.
static int   g_earlyMiss    = 0;
// CatchVerdictProbe: one line per NEW engage forwarded to the game with the verdict's own inputs --
// decoded from SetCatchOrient (Epic 0x1004120): manual mode only, skipped while IsPopPending(), free
// pass for primo/casper, otherwise BAD when the board is flipping and its alignment is outside the
// def's CatchManualFlipAngleThreshold, or rotating and outside CatchManualRotationAngleThreshold.
// The "is flipping / is rotating / pop pending" are the board interface's own virtuals (slots
// 0x238 / 0x248 / 0x278 on the object its slot 0x108 returns), called exactly as the game calls them.
// Opt-in, like every other diagnostic in the mod.
static int   g_verdictProbe = 0;
static bool  g_airMissed    = false;   // this air had a missed press; every later engage is refused
static bool  g_missPending  = false;   // the missed air has touched down; run_out delivers the bail
static float g_deckRemaining = 0.0f;   // deck degrees to grip-up along its current direction, per frame
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
// BOTH catch modes. It was auto-only until a field report killed that premise: the flick veto
// does not cover manual's early engage, it only covers a stick HELD since before the pop. A
// DELIBERATE fresh input arriving early -- a two-stick grind setup pressed straight after the
// trick flick, e.g. kickflip into a smith -- is a fresh edge, so the veto admits it and the
// board gets caught before it has flipped. "Has the deck actually turned?" is a question
// neither catch mode can answer with a flick.
static int   g_minSpinDeg   = 45;     // 0 = off
// CatchMaxCutDeg: how much unfinished rotation "foot always attaches" may throw away. Past this the
// flip is left alone to finish on its own. 0 = no limit (the old behaviour).
// AUTO CATCH ONLY (AutoCatchActive) -- on manual an over-rotation must still level out under the foot.
static int   g_maxCutDeg    = 180;
static float g_trickTravelDeg = 0.0f; // degrees the board has FLIPPED since the current trick started
// ... and how far it has SHOVED (the rotation axis, the game's own _boardRotationCurrentAngle). The
// flip measure alone refused every fakie backside pop shove: that one shove def carries a flip-speed
// input, so flip_speed stamps it as a running trick and this gate arms -- and a pop shove never flips,
// so "has the board turned 45 deg" was never true and no catch could ever engage (field: 41 of 41,
// every other shove untouched because they are never stamped). Either axis turning far enough now
// opens the gate; a flip trick reads exactly as before.
static float g_trickRotDeg    = 0.0f;
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

// CatchMaxCutDeg is AUTO-ONLY and locked to it: on manual an over-rotation is caught deliberately
// -- the foot is supposed to attach and level the deck, which is exactly what CatchMaxCutDeg was
// refusing to do (field: kickflip over-rotations stopped levelling out). CatchMinSpinDeg used to
// be locked the same way on the premise that the flick veto covered manual; it does not (see the
// knob's comment), so the spin gate now runs in both modes and this helper no longer gates it.
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
static void* g_localIh    = nullptr;    // the local skater's InputHandler, captured beside it
void* CatchTweaks_Skater() { return g_lastSkater; }
void* CatchTweaks_LocalInputHandler() { return g_localIh; }

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
    g_catchMult    = (float)TwkIniInt(buf, "CatchWindowMult", 100) / 100.0f;
    g_manualFlipTol = TwkIniInt(buf, "CatchManualFlipTolDeg", 120);
    g_manualRotTol  = TwkIniInt(buf, "CatchManualRotTolDeg", 0);
    if (g_manualFlipTol < 0) g_manualFlipTol = 0; if (g_manualFlipTol > 180) g_manualFlipTol = 180;
    if (g_manualRotTol  < 0) g_manualRotTol  = 0; if (g_manualRotTol  > 180) g_manualRotTol  = 180;
    g_manualMode   = TwkIniInt(buf, "ManualCatchMode", -1);
    g_catchBeatsDS = TwkIniInt(buf, "CatchBeatsDarkslide", 1);
    g_dsAngleDeg   = TwkIniInt(buf, "DarkslideZoneDeg", 60);
    g_catchDiag    = TwkIniInt(buf, "CatchDiag", 0);
    g_twoStickMs   = TwkIniInt(buf, "CatchTwoStickMs", 250);
    g_narrowKeepRatios = TwkIniInt(buf, "CatchNarrowKeepRatios", 0);
    g_scoopHold    = TwkIniInt(buf, "CatchScoopFootHold", 1);
    g_scoopHoldMode = TwkIniInt(buf, "CatchScoopHoldMode", 4);
    g_scoopBorrowFrom = TwkIniInt(buf, "CatchScoopBorrowFrom", 11);
    g_scoopNoManual = TwkIniInt(buf, "CatchScoopNoManual", 0);
    g_clickCatch   = TwkIniInt(buf, "CatchClickToCatch", 0);
    g_clickFrames  = TwkIniInt(buf, "CatchClickFrames", 8);
    if (g_clickFrames < 1) g_clickFrames = 1; else if (g_clickFrames > 60) g_clickFrames = 60;
    g_clickSweep   = TwkIniInt(buf, "CatchClickSweep", 0);
    g_clickProbe   = TwkIniInt(buf, "CatchClickProbe", 0);
    g_clickForce   = TwkIniInt(buf, "CatchClickForceOrient", 1);
    g_clickInject  = TwkIniInt(buf, "CatchClickInject", 0);
    g_clickForceMs = TwkIniInt(buf, "CatchClickForceMs", 250);
    TwkIniStr(buf, "CatchClickKeyL",  g_clickKeyL,  sizeof(g_clickKeyL),  "Gamepad_LeftThumbstick");
    TwkIniStr(buf, "CatchClickKeyL2", g_clickKeyL2, sizeof(g_clickKeyL2),
              "GenericUSBController_Button11");
    TwkIniStr(buf, "CatchClickKeyR",  g_clickKeyR,  sizeof(g_clickKeyR),  "Gamepad_RightThumbstick");
    TwkIniStr(buf, "CatchClickKeyR2", g_clickKeyR2, sizeof(g_clickKeyR2),
              "GenericUSBController_Button12");
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
    g_overTrace    = TwkIniInt(buf, "CatchOverTrace", 0);
    g_overMs       = TwkIniInt(buf, "CatchOverMs", 40);
    if (g_overMs < 10) g_overMs = 10; if (g_overMs > 300) g_overMs = 300;
    g_anyRevLegacy = TwkIniInt(buf, "CatchAnyRevLegacyAim", 0);
    g_shoveFixes   = TwkIniInt(buf, "CatchShoveFixes", 1) ? 1 : 0;
    g_shoveSnap    = TwkIniInt(buf, "CatchShoveSnap", 1);
    g_shoveSnapMaxRate = TwkIniInt(buf, "CatchShoveSnapMaxRate", 720);
    g_shoveFinishMaxDeg = TwkIniInt(buf, "CatchShoveFinishMaxDeg", 30);
    if (g_shoveFinishMaxDeg < 0) g_shoveFinishMaxDeg = 0; if (g_shoveFinishMaxDeg > 180) g_shoveFinishMaxDeg = 180;
    if (g_shoveSnapMaxRate < 200) g_shoveSnapMaxRate = 200; if (g_shoveSnapMaxRate > 3000) g_shoveSnapMaxRate = 3000;
    g_shoveRollBack = TwkIniInt(buf, "CatchShoveRollBack", 1);
    g_shoveBailBand = TwkIniInt(buf, "CatchShoveBailBandDeg", 10);
    if (g_shoveBailBand < 0) g_shoveBailBand = 0; if (g_shoveBailBand > 89) g_shoveBailBand = 89;
    g_overBailDeg  = TwkIniInt(buf, "CatchOverBailDeg", 70);
    if (g_overBailDeg < 0) g_overBailDeg = 0; if (g_overBailDeg > 180) g_overBailDeg = 180;
    g_plantFix     = TwkIniInt(buf, "CatchPlantFix", 1);
    g_footDescend  = TwkIniInt(buf, "CatchFootDescends", 1);
    g_descendDeg   = TwkIniInt(buf, "CatchDescendDeg", 90);
    if (g_descendDeg < 20) g_descendDeg = 20;
    if (g_descendDeg > 360) g_descendDeg = 360;
    g_earlyMiss    = TwkIniInt(buf, "CatchEarlyMiss", 0);
    g_verdictProbe = TwkIniInt(buf, "CatchVerdictProbe", 0);
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
    TwkIniSetInt(buf, cap, "CatchManualFlipTolDeg", g_manualFlipTol);
    TwkIniSetInt(buf, cap, "CatchManualRotTolDeg",  g_manualRotTol);
    TwkIniSetInt(buf, cap, "CatchBeatsDarkslide", g_catchBeatsDS);
    TwkIniSetInt(buf, cap, "DarkslideZoneDeg",    g_dsAngleDeg);
    TwkIniSetInt(buf, cap, "CatchAnyRevolution",  g_anyRev);
    TwkIniSetInt(buf, cap, "CatchAnyRevDeg",      g_anyRevDeg);
    TwkIniSetInt(buf, cap, "CatchFootLevelsBoard", g_footLevel);
    TwkIniSetInt(buf, cap, "CatchOverTrace",       g_overTrace);
    TwkIniSetInt(buf, cap, "CatchOverBailDeg",     g_overBailDeg);
    TwkIniSetInt(buf, cap, "CatchOverMs",          g_overMs);
    TwkIniSetInt(buf, cap, "CatchAnyRevLegacyAim", g_anyRevLegacy);
    TwkIniSetInt(buf, cap, "CatchShoveFixes",      g_shoveFixes);
    TwkIniSetInt(buf, cap, "CatchShoveSnap",       g_shoveSnap);
    TwkIniSetInt(buf, cap, "CatchShoveSnapMaxRate", g_shoveSnapMaxRate);
    TwkIniSetInt(buf, cap, "CatchShoveFinishMaxDeg", g_shoveFinishMaxDeg);
    TwkIniSetInt(buf, cap, "CatchShoveRollBack",   g_shoveRollBack);
    TwkIniSetInt(buf, cap, "CatchShoveBailBandDeg", g_shoveBailBand);
    TwkIniSetInt(buf, cap, "CatchPlantFix",       g_plantFix);
    TwkIniSetInt(buf, cap, "CatchFootDescends",   g_footDescend);
    TwkIniSetInt(buf, cap, "CatchDescendDeg",     g_descendDeg);
    TwkIniSetInt(buf, cap, "CatchEarlyMiss",      g_earlyMiss);
    TwkIniSetInt(buf, cap, "CatchVerdictProbe",   g_verdictProbe);
    TwkIniSetInt(buf, cap, "CatchHoldPose",       g_holdPose);
    TwkIniSetInt(buf, cap, "CatchClickToCatch",   g_clickCatch);
    TwkIniSetInt(buf, cap, "CatchTwoStickMs",     g_twoStickMs);
    TwkIniSetInt(buf, cap, "CatchNarrowKeepRatios", g_narrowKeepRatios);
    TwkIniSetInt(buf, cap, "CatchScoopFootHold",  g_scoopHold);
    TwkIniSetInt(buf, cap, "CatchScoopHoldMode",  g_scoopHoldMode);
    TwkIniSetInt(buf, cap, "CatchScoopBorrowFrom", g_scoopBorrowFrom);
    TwkIniSetInt(buf, cap, "CatchScoopNoManual",  g_scoopNoManual);
    TwkIniSetInt(buf, cap, "CatchClickFrames",    g_clickFrames);
    TwkIniSetInt(buf, cap, "CatchClickSweep",     g_clickSweep);
    TwkIniSetInt(buf, cap, "CatchClickProbe",     (int)g_clickProbe);
    TwkIniSetInt(buf, cap, "CatchClickForceOrient", g_clickForce);
    TwkIniSetInt(buf, cap, "CatchClickInject",    g_clickInject);
    TwkIniSetInt(buf, cap, "CatchClickForceMs",   g_clickForceMs);
    TwkIniSetStr(buf, cap, "CatchClickKeyL",      g_clickKeyL);
    TwkIniSetStr(buf, cap, "CatchClickKeyL2",     g_clickKeyL2);
    TwkIniSetStr(buf, cap, "CatchClickKeyR",      g_clickKeyR);
    TwkIniSetStr(buf, cap, "CatchClickKeyR2",     g_clickKeyR2);
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
struct DefSave { void* def; float flip, rot, thrF, thrR; };
static DefSave g_defSave[512];
static int     g_nDefSave = 0;
static bool    g_widened  = false;
// The multiplier actually WRITTEN into the trick defs. The widen only runs on a catch-mode
// transition, so without this a slider change sits inert until the mode is toggled -- which made an
// A/B of the window silently test nothing at all.
static float   g_appliedMult = 0.0f;
static int     g_appliedTol = -1, g_appliedRotTol = -1;   // the tolerances actually written

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
                // ...and the bad-catch tolerances (see the knobs): saved, tightened, restored with the rest.
                s.thrF = twkF(def, DEF_MANUAL_FLIP_TOL);
                s.thrR = twkF(def, DEF_MANUAL_ROT_TOL);
                const bool thrOk = s.thrF >= 0.0f && s.thrF <= 360.0f && s.thrR >= 0.0f && s.thrR <= 360.0f;
                if (!thrOk) { s.thrF = -1.0f; s.thrR = -1.0f; }
                const int rotTol = (g_manualRotTol > 0) ? g_manualRotTol : g_manualFlipTol;
                if (thrOk && g_manualFlipTol > 0) *(float*)((uint8_t*)def + DEF_MANUAL_FLIP_TOL) = (float)g_manualFlipTol;
                if (thrOk && rotTol > 0)          *(float*)((uint8_t*)def + DEF_MANUAL_ROT_TOL)  = (float)rotTol;
            }
            g_widened = true;
            g_appliedMult = g_catchMult;
            g_appliedTol = g_manualFlipTol; g_appliedRotTol = g_manualRotTol;
            TwkLog("[catch] MANUAL catch: pre-catch angle x%.2f on %d trick defs (e.g. %.0f -> %.0f deg)",
                   g_catchMult, g_nDefSave,
                   g_nDefSave ? g_defSave[0].flip : 0.0f,
                   g_nDefSave ? g_defSave[0].flip * g_catchMult : 0.0f);
            char tolF[16], tolR[16];
            if (g_manualFlipTol > 0) snprintf(tolF, sizeof(tolF), "%d", g_manualFlipTol); else strcpy(tolF, "as shipped");
            if (g_manualRotTol  > 0) snprintf(tolR, sizeof(tolR), "%d", g_manualRotTol);  else strcpy(tolR, "as shipped");
            TwkLog("[catch] MANUAL catch: bad-catch tolerance flip %s deg, rotation %s deg (the game "
                   "bails a press with the deck outside it; defs ship %.0f/%.0f)", tolF, tolR,
                   g_nDefSave ? g_defSave[0].thrF : 0.0f, g_nDefSave ? g_defSave[0].thrR : 0.0f);
        } else {
            for (int i = 0; i < g_nDefSave; i++) {
                DefSave& s = g_defSave[i];
                if (!s.def) continue;
                *(float*)((uint8_t*)s.def + DEF_FLIP_PRECATCH_ANGLE) = s.flip;
                *(float*)((uint8_t*)s.def + DEF_ROT_PRECATCH_ANGLE)  = s.rot;
                if (s.thrF >= 0.0f) *(float*)((uint8_t*)s.def + DEF_MANUAL_FLIP_TOL) = s.thrF;
                if (s.thrR >= 0.0f) *(float*)((uint8_t*)s.def + DEF_MANUAL_ROT_TOL)  = s.thrR;
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
    // "The last skater seen" was the mod's whole notion of the player. In co-op CanCatchOrient runs
    // on every skater's InAirHandler, so this flipped to whichever remote skater fired last, and every
    // module that trusts CatchTweaks_Skater() -- foot placement, the pop scheme's grounded check, the
    // catch logic itself -- followed it. Only a skater that is NOT a proxy may become "mine". The
    // InputHandler is captured here too: it carries no pointer back to its skater, and this hook is
    // where the pair is seen together, which is what lets InputHandler::Tick tell whose tick it is.
    if (self) {
        void* s = twkP(self, IAH_SKATER);                                          // twkP is SEH-safe
        if (s && !Twk_IsProxy(s)) {
            g_lastSkater = s;
            void* ih = twkP(self, IAH_INPUT_HANDLER);
            if (ih) g_localIh = ih;
        }
    }
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
            } else if (want && (g_appliedMult != g_catchMult || g_appliedTol != g_manualFlipTol ||
                                g_appliedRotTol != g_manualRotTol)) {
                // The setting moved while it was already applied: put the originals back and
                // re-widen with the new value, so the slider means something the moment it moves.
                applyCatchWindow(twkP(self, IAH_TRICKS_DB), false);
                applyCatchWindow(twkP(self, IAH_TRICKS_DB), true);
            }
            // The bone adjustment is independent of catch MODE -- it is authored data, not a manual
            // catch behaviour -- so it follows its own knobs only.
            g_codbCached = twkP(self, IAH_CATCH_ORIENTS_DB);
            applyBoardOffsets(g_codbCached);
            if (g_nBorrow) ApplyBorrow();
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


// ---- the board-offset borrow (CatchScoopHoldMode 3; see the knob comment) -----------------
// Copies the four BoardRelativeOffset vectors of BOTH settings blocks from the def of one catch
// state into every def of another, saving the originals for an exact restore. Pointers into the
// orient DB are stable for the session (it is a data asset), the same assumption the boned-ollie
// writer already rests on.
static void EndBorrow() {
    for (int i = 0; i < g_nBorrow; i++) {
        uint8_t* def = g_borrowDst[i]; if (!def) continue;
        for (int h = 0; h < 2; h++) {
            uint8_t* st = def + (h == 0 ? CO_BACK_SETTINGS : CO_FRONT_SETTINGS);
            for (int q = 0; q < 4; q++)
                for (int c = 0; c < 3; c++)
                    *(float*)(st + kBoardOffsets[q] + c * 4) = g_borrowSave[i][h][q][c];
        }
    }
    if (g_nBorrow) TwkLog("[catch] scoop borrow: %d def(s) restored", g_nBorrow);
    g_nBorrow = 0; g_borrowSrc = nullptr;
}
static void ApplyBorrow() {
    if (!g_borrowSrc) return;
    for (int i = 0; i < g_nBorrow; i++) {
        uint8_t* def = g_borrowDst[i]; if (!def) continue;
        for (int h = 0; h < 2; h++) {
            const uint8_t* ss = g_borrowSrc + (h == 0 ? CO_BACK_SETTINGS : CO_FRONT_SETTINGS);
            uint8_t*       ds = def         + (h == 0 ? CO_BACK_SETTINGS : CO_FRONT_SETTINGS);
            for (int q = 0; q < 4; q++)
                for (int c = 0; c < 3; c++)
                    *(float*)(ds + kBoardOffsets[q] + c * 4) = *(const float*)(ss + kBoardOffsets[q] + c * 4);
        }
    }
}
static void BeginBorrow(int fromState, int toState) {
    EndBorrow();
    if (!g_codbCached) { TwkLog("[catch] scoop borrow: no orient DB cached yet -- skipped"); return; }
    uint8_t* arr = (uint8_t*)twkP(g_codbCached, CODB_ORIENTS);
    const int n = twkI(g_codbCached, CODB_ORIENTS + 8);
    if (!arr || n <= 0 || n > 128) { TwkLog("[catch] scoop borrow: orient DB unreadable (%d defs)", n); return; }
    uint8_t* src = nullptr;
    for (int i = 0; i < n && !src; i++)
        if (arr[(size_t)i * CO_DEF_STRIDE] == (uint8_t)fromState) src = arr + (size_t)i * CO_DEF_STRIDE;
    if (!src) { TwkLog("[catch] scoop borrow: no def carries state %d -- skipped", fromState); return; }
    for (int i = 0; i < n && g_nBorrow < 8; i++) {
        uint8_t* def = arr + (size_t)i * CO_DEF_STRIDE;
        if (def[0] != (uint8_t)toState || def == src) continue;
        for (int h = 0; h < 2; h++) {
            const uint8_t* st = def + (h == 0 ? CO_BACK_SETTINGS : CO_FRONT_SETTINGS);
            for (int q = 0; q < 4; q++)
                for (int c = 0; c < 3; c++)
                    g_borrowSave[g_nBorrow][h][q][c] = *(const float*)(st + kBoardOffsets[q] + c * 4);
        }
        g_borrowDst[g_nBorrow++] = def;
    }
    if (!g_nBorrow) { TwkLog("[catch] scoop borrow: no def carries state %d -- skipped", toState); return; }
    g_borrowSrc = src;
    // THE MEASUREMENT: both sets, front block, the two regular-stance vectors. If these are the
    // same numbers the offsets are not what moves the foot, and this mode is dead without a run.
    const float* sf = (const float*)(src + CO_FRONT_SETTINGS);
    const float* df = (const float*)(g_borrowDst[0] + CO_FRONT_SETTINGS);
    TwkLog("[catch] scoop borrow: state %d's offsets -> %d state-%d def(s). front block, "
           "L_RGS/R_RGS: from (%.1f,%.1f,%.1f)/(%.1f,%.1f,%.1f)  was (%.1f,%.1f,%.1f)/(%.1f,%.1f,%.1f)",
           fromState, g_nBorrow, toState,
           sf[COS_OFF_L_RGS/4], sf[COS_OFF_L_RGS/4+1], sf[COS_OFF_L_RGS/4+2],
           sf[COS_OFF_R_RGS/4], sf[COS_OFF_R_RGS/4+1], sf[COS_OFF_R_RGS/4+2],
           df[COS_OFF_L_RGS/4], df[COS_OFF_L_RGS/4+1], df[COS_OFF_L_RGS/4+2],
           df[COS_OFF_R_RGS/4], df[COS_OFF_R_RGS/4+1], df[COS_OFF_R_RGS/4+2]);
    ApplyBorrow();
}

// UCatchOrientsDatabase::GetCatchOrientCatchFootType, from the disassembly: the authored
// CatchFootType (def+1) of the FIRST definition whose CatchState (def+0) matches; 0 if none.
static int OrientFootTypeFor(int state) {
    if (!g_codbCached) return 0;
    uint8_t* arr = (uint8_t*)twkP(g_codbCached, CODB_ORIENTS);
    const int n = twkI(g_codbCached, CODB_ORIENTS + 8);
    if (!arr || n <= 0 || n > 128) return 0;
    for (int i = 0; i < n; i++) {
        const uint8_t* def = arr + (size_t)i * CO_DEF_STRIDE;
        if (def[0] == (uint8_t)state) return def[1];
    }
    return 0;
}
// USkateboardExMovementComponent::ConvertCatchFootToStance, from its jump table: identity when the
// stance byte is 0; with it set, 1<->2, 4<->5, 6<->7, everything else unchanged. The stance byte
// the game passes is IsSkatingGoofy() ^ IsSkatingSwitch().
static int StanceFootType(int foot, bool stance) {
    if (!stance) return foot;
    switch (foot) { case 1: return 2; case 2: return 1; case 4: return 5; case 5: return 4;
                    case 6: return 7; case 7: return 6; default: return foot; }
}
static bool GoofyXorSwitch() {
    __try {
        void* a = FootPlace_AnimInstance();
        return a && ((twkB(a, AN_IS_GOOFY) > 0) != (twkB(a, AN_IS_SWITCH) > 0));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Once: every orient definition's state -> authored foot type, so the transient state that carries
// the 4 is named in the log rather than inferred.
static void DumpOrientFootTypes() {
    if (g_scoopDbDumped || !g_codbCached) return;
    g_scoopDbDumped = 1;
    uint8_t* arr = (uint8_t*)twkP(g_codbCached, CODB_ORIENTS);
    const int n = twkI(g_codbCached, CODB_ORIENTS + 8);
    if (!arr || n <= 0 || n > 128) return;
    char line[512]; size_t k = 0;
    for (int i = 0; i < n && k < sizeof(line) - 16; i++) {
        const uint8_t* def = arr + (size_t)i * CO_DEF_STRIDE;
        k += snprintf(line + k, sizeof(line) - k, "%s%d->%d", i ? "  " : "", def[0], def[1]);
    }
    TwkLog("[catch] orient DB (%d defs) state->footType: %s", n, line);
}

// Defined below, beside the rest of the stance rule; needed here for click-to-catch.
static bool CatchStanceInverts();

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

    // ---- CLICK TO CATCH: present the mapped stick as flicked, and HOLD it there -------------
    // A press used to be spent on ONE call, and that is not what a flick looks like. This function
    // only writes a VERDICT -- the dispatcher engages the catch after it returns, re-deriving from
    // the sticks as it goes, which is exactly why the two-stick mask has to hold its tracker for
    // several calls. A real flick stays deflected for a hundred milliseconds or more, so a
    // one-frame blip left the decision nothing to hold on to: the press was seen, the deflection
    // was written, and no catch ever came of it. The press now holds the stick out across
    // g_clickFrames consecutive decisions, the way a thumb does.
    float clickSaved[2] = { 0.0f, 0.0f };
    void* clickStick = nullptr; bool clickFirst = false;
    if (g_clickInject && (g_clickArmed || g_clickHold > 0) && self) {
        __try {
            int which = 0;
            if (g_clickHold > 0) which = g_clickWhich;
            else {
                LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
                const double ms = (double)(t.QuadPart - g_clickQpc) * 1000.0 / (double)f.QuadPart;
                if (ms > (double)g_clickHoldMs) {
                    if (g_catchDiag || g_clickProbe > 0)
                        TwkLog("[catch] click lapsed unspent after %.0f ms (no decision ran)", ms);
                    g_clickArmed = 0;      // pressed with no air to spend it on
                } else {
                    which = (int)g_clickArmed; g_clickWhich = which;
                    g_clickHold = g_clickFrames; clickFirst = true; g_clickArmed = 0;
                    InterlockedIncrement(&g_uiClickCatches);
                }
            }
            if (which) {
                // The game hands _Default its two sticks as front/back and swaps them by stance
                // (CatchStanceInverts is that rule, disassembled). We key on the PHYSICAL stick,
                // and the argument values cannot name it here the way the flick detector does --
                // a press leaves both sticks centred, so the two arguments are identical.
                const bool inv = CatchStanceInverts();
                clickStick = ((which == 1) != inv) ? frontStick : backStick;
                const float dzRead = twkF(twkP(self, IAH_CATCH_ORIENTS_DB), CODB_INPUT_DEADZONE);
                const float dz = (dzRead > 0.0f && dzRead < 1.0f) ? dzRead : 0.2f;
                float push = dz + g_clickMargin;
                float dx = g_clickDirX, dy = g_clickDirY;
                if (g_clickSweep) {
                    // step 0 on the first held decision, then one direction per decision
                    const int step = (g_clickFrames - g_clickHold) & 7;
                    dx = kClickDirs[step][0]; dy = kClickDirs[step][1];
                    push = g_clickSweepMag;
                    g_clickStepNow = step;
                    // Eligibility is an EDGE, so each swept direction needs its own: without this
                    // only the first of the eight would ever be judged.
                    const int hadOff2 = (clickStick == frontStick) ? IAH_HAD_FRONT_INPUT
                                                                   : IAH_HAD_BACK_INPUT;
                    *((uint8_t*)self + hadOff2) = 0;
                } else g_clickStepNow = -1;
                g_clickDirNow[0] = dx; g_clickDirNow[1] = dy;
                clickSaved[0] = twkF(clickStick, 0); clickSaved[1] = twkF(clickStick, 4);
                *(float*)clickStick       = dx * push;
                *((float*)clickStick + 1) = dy * push;
                if (clickFirst) {
                    // Eligibility is an EDGE, so the first held decision has to clear the "was
                    // pushed last frame" tracker. It is deliberately NOT restored afterwards: the
                    // game's own epilogue then owns it, and the frames that follow are supposed to
                    // look like a stick that is still being held.
                    const int hadOff = (clickStick == frontStick) ? IAH_HAD_FRONT_INPUT
                                                                  : IAH_HAD_BACK_INPUT;
                    *((uint8_t*)self + hadOff) = 0;
                    if (g_catchDiag || g_clickProbe > 0)
                        TwkLog("[catch] click: %s stick -> %s arg, push %.2f (deadzone %.2f), "
                               "held for %d decisions", which == 1 ? "LEFT" : "RIGHT",
                               clickStick == frontStick ? "front" : "back", push, dz,
                               g_clickFrames);
                }
                g_clickHold--;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { clickStick = nullptr; g_clickCatch = 0; }
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
        if (clickStick) {
            __try {
                *(float*)clickStick = clickSaved[0];
                *((float*)clickStick + 1) = clickSaved[1];
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return nullptr;
    }
    // The synthetic push is put straight back, exactly like the mask above: these are the caller's
    // own temporaries and the next call gets fresh copies, so nothing may be held across it.
    if (clickStick) {
        __try {
            // What the decision made of it. The press and the deflection were already visible in
            // the log; this is the half that was not -- a5 is the catch state the dispatcher then
            // engages, so a run of zeroes here means the deflection itself was refused, and
            // non-zero means the decision was made and something after it dropped the catch.
            if (g_catchDiag || g_clickProbe > 0) {
                const int vd = a5 ? (int)twkB(a5, 0) : -1;
                if (g_clickStepNow >= 0)
                    TwkLog("[catch] click dir %d (%.2f, %.2f) -> verdict %d",
                           g_clickStepNow, g_clickDirNow[0], g_clickDirNow[1], vd);
                else if (clickFirst)
                    TwkLog("[catch] click verdict = %d", vd);
            }
            *(float*)clickStick = clickSaved[0];
            *((float*)clickStick + 1) = clickSaved[1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
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

// BOTH STICKS PUSHED ON PURPOSE, as opposed to a second stick arriving late.
//
// A two-stick orient (5/10/11 -- the boardslide family) is REWRITTEN to one foot by two separate
// features below, and neither could tell a deliberate gesture from an accident: the flicked-foot
// narrowing was built for a catch taken while a scoop is HELD, and the pose hold for a second stick
// moved later in the air. Both are real, and both were blocking the deliberate push -- pushing both
// sticks inward for a boardslide did nothing, while a DARKSLIDE oriented fine because states 7-9
// are exempt from both (field, longstanding).
//
// The tell is WHEN each deflection began, which is already tracked per stick. Thumbs land a few
// frames apart, never on the same one, so a deliberate two-stick gesture has two deflections that
// STARTED together; a held scoop, or a stick moved later in the air, has one that began long before
// the other.
static bool DeliberateTwoStick() {
    const long long dl = g_deflStartL, dr = g_deflStartR;
    if (dl <= 0 || dr <= 0) return false;              // only one stick is out: not a gesture
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    const long long gap = (dl > dr) ? (dl - dr) : (dr - dl);
    return (int)(gap * 1000 / fq.QuadPart) <= g_twoStickMs;
}

static void hkSetCatchOrient(void* self, uint8_t state, float pitchRatio, float yawRatio) {
    // ---- CLICK TO CATCH: turn an idle call into a catch, for as long as the press lasts -------
    InterlockedExchange(&g_clickSetterSeen, 1);
    if (g_clickCatch && g_clickForce && state == 0 && g_clickForceState) {
        __try {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            if (t.QuadPart < g_clickForceUntil) {
                void* mineC = CatchTweaks_Skater();
                if (!mineC || self == mineC) {
                    state = (uint8_t)g_clickForceState;
                    static LONGLONG loggedFor = 0;
                    if ((g_catchDiag || g_clickProbe > 0) && loggedFor != g_clickForceUntil) {
                        loggedFor = g_clickForceUntil;
                        TwkLog("[catch] click forced orient state %d at the setter", (int)state);
                    }
                }
            } else g_clickForceState = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_clickForce = 0; }
    }
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
            // BOTH modes (see g_minSpinDeg): manual's veto admits any fresh edge, including a
            // grind-orient input pressed straight after the trick flick, so only this asks
            // whether the deck has turned. Ollies never stamp a trick; darkslides stay exempt.
            if (!vetoed && g_minSpinDeg > 0 && isMine &&
                (oneFoot || twoFoot) && !(state >= 7 && state <= 9)) {
                const int msTrick = FlipSpeed_MsSinceTrick();
                if (msTrick >= 0 && msTrick <= 2000 && g_trickTravelDeg < (float)g_minSpinDeg &&
                    g_trickRotDeg < (float)g_minSpinDeg) {
                    use = 0; vetoed = true;
                    const long serial = FlipSpeed_TrickSerial();
                    if (g_spinLogged != serial) {
                        g_spinLogged = (int)serial;
                        TwkLog("[catch] catch held off +%d ms into '%s' -- the board has only turned "
                               "%.0f flip / %.0f shove of %d deg", msTrick, FlipSpeed_LastTrickName(),
                               g_trickTravelDeg, g_trickRotDeg, g_minSpinDeg);
                    }
                }
            }
            // ---- AN EARLY PRESS MISSES (see CatchEarlyMiss). After the spin gate, so a press the
            // spin gate is merely holding off is not a miss; after the flick veto, so a held-over
            // stick is not one either. A press with the deck farther from grip-up than the snap
            // would rescue is refused, and the air is lost to catching from then on -- the game
            // keeps asking every frame the stick stays deflected, and every ask is refused. The
            // landing bail is delivered by run_out's pump (CatchTweaks_TakeMissedLanding).
            if (!vetoed && g_earlyMiss && isMine && (oneFoot || twoFoot) &&
                !(state >= 7 && state <= 9) && !AutoCatchActive()) {
                if (g_airMissed) { use = 0; vetoed = true; }
                else {
                    const int msTrick = FlipSpeed_MsSinceTrick();
                    if (msTrick >= 0 && msTrick <= 2000 && g_deckRemaining > (float)g_snapMaxDeg) {
                        use = 0; vetoed = true; g_airMissed = true;
                        TwkLog("[catch] MISSED: catch pressed +%d ms into '%s' with the deck %.0f deg from "
                               "grip-up (past CatchSnapMaxDeg %d) -- no catch this air, bail at touchdown",
                               msTrick, FlipSpeed_LastTrickName(), g_deckRemaining, g_snapMaxDeg);
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
                // The latch was armed by the FIRST of the two sticks -- a two-stick push cannot
                // help but look like a one-stick flick for a frame or two -- so narrowing on it
                // would undo the gesture the player is in the middle of making.
                const bool bothOnPurpose = twoFoot && DeliberateTwoStick();
                if (bothOnPurpose && g_flickLatch != 0) {
                    static long saidSerial = -1;
                    const long serial9 = FlipSpeed_TrickSerial();
                    if (saidSerial != serial9) {
                        saidSerial = serial9;
                        TwkLog("[catch] both sticks pushed together -- leaving the two-stick orient "
                               "%d alone", (int)state);
                    }
                }
                // The scoop hold takes this case away from the narrowing: the state goes through
                // as the game chose it, and the foot on the scoop side is held off afterwards.
                const bool scoopCase = g_scoopHold && twoFoot && g_flickLatch != 0 &&
                                       (uint8_t)g_flickLatch != state && !bothOnPurpose;
                // mode 3 lets the narrowing run -- the one-foot catch is proven, and is what the
                // game settles on by itself anyway -- and fixes where the board sits instead
                const bool scoopHeld = scoopCase && g_scoopHoldMode != 3 && g_scoopHoldMode != 4;
                if (scoopCase && g_scoopHoldMode == 3 && g_scoopHoldFoot == 0) {
                    g_scoopHoldFoot = 3 - g_flickLatch;         // release tracking only
                    g_scoopHoldFrames = g_scoopHoldWrites = 0; g_scoopHoldPeak = 0.0f;
                    LARGE_INTEGER tq; QueryPerformanceCounter(&tq); g_scoopHoldQpc = tq.QuadPart;
                    BeginBorrow(g_scoopBorrowFrom, g_flickLatch);
                }
                if (scoopCase && g_scoopHoldMode == 4 && g_scoopHoldFoot == 0) {
                    g_scoopHoldFoot = 3 - g_flickLatch;         // release tracking only
                    g_scoopHoldFrames = g_scoopHoldWrites = 0; g_scoopHoldPeak = 0.0f;
                    LARGE_INTEGER tq; QueryPerformanceCounter(&tq); g_scoopHoldQpc = tq.QuadPart;
                    DumpOrientFootTypes();
                }
                if (scoopHeld && g_scoopHoldFoot == 0) {
                    g_scoopHoldFoot = 3 - g_flickLatch;         // the foot that did NOT flick
                    g_scoopHoldFrames = g_scoopHoldWrites = 0; g_scoopHoldPeak = 0.0f;
                    LARGE_INTEGER tq; QueryPerformanceCounter(&tq); g_scoopHoldQpc = tq.QuadPart;
                    TwkLog("[catch] scoop catch: keeping two-foot orient %d, holding the %s foot "
                           "off the deck", (int)state, g_scoopHoldFoot == 1 ? "LEFT" : "RIGHT");
                }
                if (g_flickLatch != 0 && (uint8_t)g_flickLatch != state && !bothOnPurpose &&
                    !scoopHeld) {
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
                            TwkLog("[catch] narrowed two-stick orient %d -> %d: %s pose ratios "
                                   "(pitch %.2f yaw %.2f)", (int)state, (int)use,
                                   g_narrowKeepRatios ? "KEPT its" : "dropped its",
                                   usePitch, useYaw);
                        }
                        if (!g_narrowKeepRatios) { usePitch = 0.0f; useYaw = 0.0f; }
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
                        // The scoop hold has claimed this catch: the two-foot state MUST reach the
                        // game, or the hold is working on a pose that never happened. A mid-scoop
                        // catch engages as state 1 and the 11 follows ~20 ms later, so this block
                        // had latched on the 1 and was converting the 11 straight back -- the
                        // round meant to test state 11 never ran it (log: both lines, same ms).
                        if (DeliberateTwoStick() || g_scoopHoldFoot != 0) {
                            // Both thumbs arrived together: the player is ASKING for the two-stick
                            // pose. Treated exactly as a darkslide is -- passed through, and the
                            // hold dropped, because they have deliberately changed the pose.
                            g_holdState = 0; g_holdLoggedState = 0;
                        } else {
                            if (!g_holdLoggedState) {
                                g_holdLoggedState = 1;
                                TwkLog("[catch] second stick tried to flip the catch into two-stick "
                                       "state %d -- held on foot %d", (int)use, g_holdState);
                            }
                            use = (uint8_t)g_holdState;
                            InterlockedIncrement(&g_uiHoldFixes);
                        }
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
    // ---- VERDICT PROBE (see CatchVerdictProbe): the game's manual-catch verdict runs inside the
    // call below on a NEW engage. Its inputs before, its outcome after, one line.
    const bool newEngage = (use != 0) && twkB(self, SK_CATCH_ORIENT_STATE) == 0 && g_verdictProbe;
    long bailsBefore = 0; int vc238 = -1, vc248 = -1, vc278 = -1, flags = -1, popBit = -1;
    float fcur = 0.0f, ftgt = 0.0f, thrF = -1.0f, thrR = -1.0f; int forceAuto = -1;
    if (newEngage) {
        __try {
            bailsBefore = RunOut_BailCalls();
            void* board = twkP(self, SK_BOARD);
            if (board) {
                uint8_t* iface = (uint8_t*)board + 0x280;
                void* obj = ((void* (*)(void*))((*(void***)iface)[0x108 / 8]))(iface);
                if (obj) {
                    vc238 = ((bool (*)(void*))((*(void***)obj)[0x238 / 8]))(obj) ? 1 : 0;
                    vc248 = ((bool (*)(void*))((*(void***)obj)[0x248 / 8]))(obj) ? 1 : 0;
                    vc278 = ((bool (*)(void*))((*(void***)obj)[0x278 / 8]))(obj) ? 1 : 0;
                }
            }
            popBit = (twkB(self, 0x710) >> 2) & 1;
            void* comp = CatchLevel_MovementComponent();
            if (comp) {
                flags = twkB(comp, MC_BOARD_FLAGS);
                fcur = twkF(comp, MC_BOARD_FLIP_CUR); ftgt = twkF(comp, MC_BOARD_FLIP_TARGET);
            }
            void* def = twkP(self, SK_CURRENT_TRICK_DEF);
            if (def) { thrF = twkF(def, 0x290); thrR = twkF(def, 0x294); forceAuto = twkB(def, 0x254); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { vc238 = vc248 = vc278 = -2; }
    }
    __try { ((SetCatchOrientFn)g_origSetOrient)(self, use, usePitch, useYaw); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[catch] caught fatal in SetCatchOrient -> recovered");
    }
    if (newEngage) {
        __try {
            const long bailsAfter = RunOut_BailCalls();
            TwkLog("[catch] verdict: engage %d -> state now %d | mode=%d popPending=%d (vc278=%d bit=%d) | "
                   "vcIsFlipping=%d vcIsRotating=%d flags=0x%02x | flip %.0f of %.0f | primo=%d casper=0x%02x "
                   "forceAuto=%d | thr flip %.0f rot %.0f | +%d ms '%s' | BAIL %s",
                   use, twkB(self, SK_CATCH_ORIENT_STATE), twkB(self, SK_CATCH_MODE),
                   (vc278 == 1 || popBit == 1) ? 1 : 0, vc278, popBit, vc238, vc248, flags,
                   fcur, ftgt, twkB(self, 0xa0c), twkB(self, 0xa0e) & 3, forceAuto, thrF, thrR,
                   FlipSpeed_MsSinceTrick(), FlipSpeed_LastTrickName(),
                   (bailsAfter != bailsBefore) ? "FIRED" : "no");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ------------------------------------------------------------------ install + menu
// Riding, as opposed to walking around off the board. An unreadable state counts as NOT on the
// board, so a bad read hands the press back to the game rather than eating it.
static bool CatchOnBoard() {
    __try {
        void* a = FootPlace_AnimInstance();
        return a && twkB(a, AN_ON_BOARD) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Which thumbstick a key is, or 0. An FKey's first eight bytes are its FName, so both names are
// resolved once and compared as plain words afterwards -- InputKey runs on every input event and
// is no place for a string compare.
static uint64_t g_fnClick[4] = { 0, 0, 0, 0 };        // L, L2, R, R2
// ...and the keys already ruled OUT. The sticks' own axes arrive here every frame under names like
// Gamepad_RightY, and without this every one of them would be resolved to a string forever.
static uint64_t g_fnNot[24] = { 0 }; static int g_fnNotN = 0;
static const char* ClickKeyName(int i) {
    return (i == 0) ? g_clickKeyL : (i == 1) ? g_clickKeyL2
         : (i == 2) ? g_clickKeyR : g_clickKeyR2;
}
static int ThumbClickWhich(const void* key) {
    const uint64_t nm = *(const uint64_t*)key;
    if (!nm) return 0;
    for (int i = 0; i < 4; i++) if (g_fnClick[i] && nm == g_fnClick[i]) return (i < 2) ? 1 : 2;
    for (int i = 0; i < g_fnNotN; i++) if (nm == g_fnNot[i]) return 0;
    char nb[96];
    if (!GrindPop_FNameToString(key, nb, sizeof(nb))) return 0;
    for (int i = 0; i < 4; i++) {
        const char* want = ClickKeyName(i);
        if (want && want[0] && !strcmp(nb, want)) { g_fnClick[i] = nm; return (i < 2) ? 1 : 2; }
    }
    if (g_fnNotN < (int)(sizeof(g_fnNot) / sizeof(g_fnNot[0]))) g_fnNot[g_fnNotN++] = nm;
    return 0;
}

// bool UPlayerInput::InputKey(FKey, EInputEvent, float, bool) -- MEASURED, not assumed: the FKey is
// too big for a register, so it arrives as a POINTER in rdx (`mov rbx, [rdx]` reads its FName).
static const char* SIG_PLAYER_INPUT_KEY =
    "48 8B C4 53 57 41 56 48 83 EC 50 48 8B 1A 48 8B F9 48 89 68 08 8B CB 48 89 70 10 4C 8B F2 4C 89 60 18";
typedef bool (*InputKeyFn)(void*, void*, int, float, bool);
static InputKeyFn g_origInputKey  = nullptr;
static void*      g_startInputKey = nullptr;

static bool hkInputKey(void* self, void* key, int ev, float amt, bool pad) {
    // IE_Pressed = 0, IE_Released = 1. The sticks' own axis events are the hot path here and are
    // never ours, so the cheapest tests come first.
    if (g_clickCatch && key && (ev == 0 || ev == 1)) {
        int swallow = 0;
        __try {
            // PRESSES ONLY, AND NEVER AN AXIS. The first cut logged both edges of every key and
            // the sticks' own axes come through here too -- 60 lines were spent in 25 seconds on
            // face buttons and axis traffic, before a single stick was pressed.
            if (g_clickProbe > 0 && ev == 0) {
                char nb[96];
                if (GrindPop_FNameToString(key, nb, sizeof(nb))) {
                    const size_t ln = strlen(nb);
                    const bool axis = strstr(nb, "Axis") || strstr(nb, "2D") ||
                                      (ln && (nb[ln-1] == 'X' || nb[ln-1] == 'Y'));
                    if (!axis) {
                        InterlockedDecrement(&g_clickProbe);
                        TwkLog("[catch] key '%s' pressed, onBoard=%d", nb,
                               CatchOnBoard() ? 1 : 0);
                    }
                } else {
                    InterlockedDecrement(&g_clickProbe);
                    TwkLog("[catch] key press -- NAME UNREADABLE (FKey is not an FName here)");
                }
            }
            const int which = ThumbClickWhich(key);
            if (which && CatchOnBoard()) {
                swallow = 1;
                if (ev == 0) {
                    LARGE_INTEGER t; QueryPerformanceCounter(&t);
                    g_clickArmed = which; g_clickQpc = t.QuadPart;
                    // The fresh-flick veto admits a catch only after a stick EDGE. This press IS
                    // that edge -- without the stamp the veto would throw the catch away as a
                    // held-over flick for the first two seconds of every trick.
                    g_lastEdgeQpc = t.QuadPart;
                    // and name the foot, the same way a real flick does
                    if (g_flickFoot) { g_flickPhys = which; g_flickFresh = 60; g_flickLatch = 0; }
                    // ...and hold the orient open at the setter for a moment
                    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
                    g_clickForceState = (which == 1) ? 1 : 2;
                    g_clickForceUntil = t.QuadPart +
                                        (LONGLONG)(fq.QuadPart * g_clickForceMs / 1000);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_clickCatch = 0; swallow = 0; }
        // Swallowed whole while riding -- press AND release -- so a bound action cannot fire on
        // half a gesture.
        if (swallow) return true;
    }
    return g_origInputKey(self, key, ev, amt, pad);
}

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

    // Click to catch. Hooked whenever the signature resolves rather than only when the option is
    // on, because the option is a live menu toggle -- until it is ticked this is a pass-through.
    // It needs the decide hook to deliver the catch, and without that the press is NOT swallowed.
    if (g_startCatchDef) {
        g_startInputKey = TwkScanExe(SIG_PLAYER_INPUT_KEY);
        if (!g_startInputKey) {
            TwkLog("[catch] UPlayerInput::InputKey sig NOT FOUND -- click to catch off (game updated?)");
            g_clickCatch = 0;
        } else if (MH_CreateHook(g_startInputKey, (void*)&hkInputKey, (void**)&g_origInputKey) != MH_OK ||
                   MH_EnableHook(g_startInputKey) != MH_OK) {
            TwkLog("[catch] InputKey hook failed -- click to catch off");
            g_startInputKey = nullptr; g_clickCatch = 0;
        } else TwkLog("[catch] hooked UPlayerInput::InputKey @ %p -- a thumbstick press can catch",
                      g_startInputKey);
    } else if (g_clickCatch) {
        TwkLog("[catch] click to catch needs CheckForCatchOrient_Default -- not installed");
        g_clickCatch = 0;
    }
}

bool CatchTweaks_Enabled() { return g_catchFix != 0; }
void CatchTweaks_SetEnabled(bool on) { g_catchFix = on ? 1 : 0; TwkMarkDirty(); }
// `g_manualMode` is deliberately NOT reset. It is not a preference but the ECatchMode value measured
// for this install off the "[catch] ECatchMode=" log line, and its default -1 means "unknown, widen
// nothing" -- resetting it would silently switch the whole catch feature off.
void CatchTweaks_ResetDefaults() {
    g_catchFix = 1; g_catchMult = 1.0f; g_catchBeatsDS = 1; g_dsAngleDeg = 60; g_catchDiag = 0;
    g_manualFlipTol = 120; g_manualRotTol = 0; g_overBailDeg = 70; g_shoveBailBand = 10; g_shoveFixes = 1;
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
float CatchTweaks_ManualTolDeg() { return (float)g_manualFlipTol; }
float CatchTweaks_OverBailDeg()  { return (float)g_overBailDeg; }
// A shove has been stopped where it was caught (over- or under-rotated) and the catch is still live.
// foot_place holds the feet's sockets against per-frame flip-flops for its duration.
bool  CatchTweaks_ShoveStopHold()   { return g_shoveRbClaimed; }
float CatchTweaks_ShoveBailBandDeg() { return (float)g_shoveBailBand; }
bool  CatchTweaks_ShoveFixes()         { return g_shoveFixes != 0; }
void  CatchTweaks_SetShoveFixes(bool on) { g_shoveFixes = on ? 1 : 0; TwkMarkDirty(); }
void  CatchTweaks_SetShoveBailBandDeg(float deg) {
    int d = (int)(deg + 0.5f); if (d < 0) d = 0; if (d > 89) d = 89;
    g_shoveBailBand = d; TwkMarkDirty();
}
void  CatchTweaks_SetOverBailDeg(float deg) {
    int d = (int)(deg + 0.5f); if (d < 0) d = 0; if (d > 180) d = 180;
    g_overBailDeg = d; TwkMarkDirty();
}
// The press-to-catch toggle. Reports FALSE when the input hook is not installed, so the pause-menu
// row cannot offer a switch that would do nothing.
bool  CatchTweaks_ClickToCatch() { return g_clickCatch != 0 && g_startInputKey != nullptr; }
void  CatchTweaks_SetClickToCatch(bool on) {
    g_clickCatch = (on && g_startInputKey) ? 1 : 0; TwkMarkDirty();
}
bool  CatchTweaks_ClickToCatchAvailable() { return g_startInputKey != nullptr; }
// The control-scheme option that binds the sticks to LEFT/RIGHT feet rather than FRONT/BACK.
// Exported because it is load-bearing for ANY physical-stick mapping, not just this module's: with
// FRONT/BACK binding the sticks already follow the feet, so stance must not swap anything. An
// unreadable setting falls back to the measured-true value, as CatchStanceInverts does.
bool  CatchTweaks_LeftRightFootSkater() {
    __try {
        void* a = FootPlace_AnimInstance();
        void* sk = a ? twkP(a, AN_SKATER) : nullptr;
        return !sk || (twkB(sk, SK_STANCE_OPTS) & 1) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
bool  CatchTweaks_TakeMissedLanding() {
    if (!g_missPending) return false;
    g_missPending = false;
    return true;
}
void  CatchTweaks_SetManualTolDeg(float deg) {
    int d = (int)(deg + 0.5f);
    if (d < 30) d = 30; if (d > 180) d = 180;
    g_manualFlipTol = d; TwkMarkDirty();
}
float CatchTweaks_DarkslideZoneDeg() { return (float)g_dsAngleDeg; }
void  CatchTweaks_SetDarkslideZoneDeg(float deg) { g_dsAngleDeg = (int)deg; TwkMarkDirty(); }

void CatchTweaks_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    if (!g_startCanCatch) { api->TextDisabled("Manual catch tweaks: not installed"); return; }
    bool cw = g_catchFix != 0;
    if (api->Checkbox("Manual catch window", &cw)) { g_catchFix = cw ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(manual catch only; auto stays stock)");
    if (cw) {
        api->Indent();
        float d = (float)g_manualFlipTol;
        if (api->SliderFloat("Window (deg from flat)", &d, 30.0f, 180.0f, "%.0f")) CatchTweaks_SetManualTolDeg(d);
        snprintf(b, sizeof(b), "a press with the deck more than %d deg from flat bails on the spot (the game "
                 "ships 120)", g_manualFlipTol);
        api->TextDisabled(b);
        float ob = (float)g_overBailDeg;
        if (api->SliderFloat("Over-rotation bail (deg past flat)", &ob, 0.0f, 180.0f, "%.0f")) CatchTweaks_SetOverBailDeg(ob);
        api->SameLine(); api->TextDisabled("(0 = the window above rules both sides)");
        float sb = (float)g_shoveBailBand;
        if (api->SliderFloat("Shove sideways bail band (deg)", &sb, 0.0f, 89.0f, "%.0f")) CatchTweaks_SetShoveBailBandDeg(sb);
        api->SameLine(); api->TextDisabled("(a shove caught within this of sideways bails -- the feet cannot decide an end there; 0 = off)");
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
    if (g_startInputKey) {
        bool cc = g_clickCatch != 0;
        if (api->Checkbox("Click a stick to catch", &cc)) { g_clickCatch = cc ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(press the stick instead of flicking; that foot catches)");
        if (cc) {
            api->Indent();
            api->TextDisabled("only while you are on the board -- off it, the press does its usual job");
            snprintf(b, sizeof(b), "catches from a press: %d", (int)g_uiClickCatches);
            api->TextDisabled(b);
            api->Unindent();
        }
    }
    bool sfh = g_secondFootHold != 0;
    if (api->Checkbox("Hold the second foot until landing", &sfh)) { g_secondFootHold = sfh ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the foot you did NOT catch with stays off the board)");
    // Catch ends the flip, Foot always attaches and Over rotation leveling are bug fixes: always on,
    // ini kill-switches only (CatchStopsFlip / CatchAnyRevolution / CatchFootLevelsBoard), no row here
    // or in the pause menu.
    api->TextDisabled("Over-rotated catches (always on): the roll-back and the shove handling");
    {
        api->Indent();
        float om = (float)g_overMs;
        if (api->SliderFloat("Roll-back time (ms)", &om, 10.0f, 150.0f, "%.0f")) { g_overMs = (int)(om + 0.5f); TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(an over-rotated catch is late in the air -- keep it short)");
        bool shf = g_shoveFixes != 0;
        if (api->Checkbox("Shove catch fixes", &shf)) CatchTweaks_SetShoveFixes(shf);
        api->SameLine(); api->TextDisabled("(snap, catch-where-it-is, sideways bail, shove plant fix, socket hold -- off = the game's own shove catching)");
        float sf = (float)g_shoveFinishMaxDeg;
        if (api->SliderFloat("Shove finish range (deg short)", &sf, 0.0f, 180.0f, "%.0f")) { g_shoveFinishMaxDeg = (int)(sf + 0.5f); TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(a shove caught short by more than this is caught where it is; less and it is finished)");
        float sr = (float)g_shoveSnapMaxRate;
        if (api->SliderFloat("Shove finish speed (deg/s)", &sr, 200.0f, 3000.0f, "%.0f")) { g_shoveSnapMaxRate = (int)(sr + 0.5f); TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(how fast an under-rotated shove may be finished under the foot; the board mesh keeps up below ~900)");
        api->Unindent();
    }
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
    static int   attL = -1, attR = -1;          // frame each foot's ratio first reached 0.99
    // Our per-air latches as they stood on the LAST FRAME BEFORE the catch engaged. Sampled on the
    // engage frame they read 1/1/1/1 on every catch -- the engaging setter call has already
    // admitted the veto, latched the foot and marked the air by the time this trace sees the state
    // flip -- so the first cut of this stamp said nothing. A latch that was already set while the
    // state was still 0 is a STALE one carried in from a previous air, which is the thing worth
    // seeing; only a pre-engage sample can show it.
    static int   preVeto = 0, preHold = 0, preAir = 0, preFlick = 0;
    if (state == 0) { preVeto = g_vetoAdmitted; preHold = g_holdState;
                      preAir = g_airCaughtFoot; preFlick = g_flickLatch; }
    static float angAt = 0.0f, curAt = 0.0f, tgtAt = 0.0f;
    static float rotCurAt = 0.0f, rotTgtAt = 0.0f, rotRateAt = 0.0f;   // the shove axis at engage
    static int   frames = 0, quiet = 0;

    // The forced-catch timers, captured fresh at the START of each catch. Fresh per catch because
    // the game may set them per trick -- scaling from a baseline taken this catch is correct either
    // way, and re-deriving from it every frame can never compound.
    static float baseTotal = 0.0f, baseForce = 0.0f, baseDelay = 0.0f;
    static int   typeLat = 0, typeRat = 0;
    static int   typeFlipsL = 0, typeFlipsR = 0, prevTypeL = 0, prevTypeR = 0;   // nose/tail re-decisions
    // heldRejected is the honest test: if the write does not stick, this lever is the wrong one too.
    static int   heldFrames = 0, heldRejected = 0, heldAlready = 0;

    if (state != 0 && !inCatch) {
        inCatch = true; sawL = sawR = false; peakL = peakR = 0.0f; frames = 0; quiet = 0;
        attL = attR = -1;
        // The engage LATENCY names the pop-time-catch bug directly: ~230 ms is the pop itself
        // (selection -> pop lag), anything close to that = the catch engaged AT the pop (the
        // held-stick ramp bug); a deliberate mid-flip catch reads hundreds of ms later.
        // A hover catch that engages early with a nonzero latch here names a stale carry-over as
        // the cause; all four should read 0 on a clean air.
        TwkLog("[feet] catch engaged (state %d) +%d ms after trick '%s'   [stale latches before "
               "engage: veto=%d hold=%d air=%d flick=%d]",
               state, FlipSpeed_MsSinceTrick(), FlipSpeed_LastTrickName(),
               preVeto, preHold, preAir, preFlick);
        angAt = ang;
        curAt = twkF(comp, MC_BOARD_FLIP_CUR);
        tgtAt = twkF(comp, MC_BOARD_FLIP_TARGET);
        rotCurAt = twkF(comp, MC_BOARD_ROT_CUR); rotTgtAt = twkF(comp, MC_BOARD_ROT_TARGET);
        rotRateAt = twkF(comp, MC_BOARD_ROT_RATE);
        baseTotal = twkF(comp, MC_CATCH_TOTAL_TIME);
        baseForce = twkF(comp, MC_CATCH_FORCE_TIME);
        baseDelay = twkF(comp, MC_CATCH_TO_BOARD_DELAY);
        typeLat = twkB(comp, MC_LFOOT_CATCH); typeRat = twkB(comp, MC_RFOOT_CATCH);
        prevTypeL = typeLat; prevTypeR = typeRat; typeFlipsL = typeFlipsR = 0;
        heldFrames = heldRejected = heldAlready = 0;
    }
    if (!inCatch) return;

    ++frames;
    // The ratio column was dead too -- peakL/peakR were reset and never assigned, so every catch
    // printed 0.00 whether the foot planted or hovered, and the one question this trace exists to
    // answer was unanswerable from it. Sampled here every frame from the same fields the scoop-hold
    // probe reads, plus the frame at which each foot first arrived (a hover is a ratio that never
    // gets there; a late plant is one that gets there late -- the two want different fixes).
    if (comp) {
        const float rl = twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO);
        const float rr = twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO);
        if (rl > peakL && rl < 10.0f) peakL = rl;
        if (rr > peakR && rr < 10.0f) peakR = rr;
        if (attL < 0 && rl >= 0.99f) attL = frames;
        if (attR < 0 && rr >= 0.99f) attR = frames;
        // A foot whose TYPE keeps changing mid-catch is a foot whose placement keeps changing --
        // the oscillation the user sees on a board caught far past its shove mark.
        const int tl = twkB(comp, MC_LFOOT_CATCH), tr = twkB(comp, MC_RFOOT_CATCH);
        if (tl != prevTypeL) { ++typeFlipsL; prevTypeL = tl; }
        if (tr != prevTypeR) { ++typeFlipsR; prevTypeR = tr; }
    }
    // The orient column was dead -- sawL/sawR were reset and never set, so every catch since this
    // trace was written has printed "L orient=no R orient=no <-- NO FOOT CAUGHT", working ones
    // included. They now read the anim instance's per-foot orient flags, which is what the column
    // always claimed to be.
    {
        void* an2 = FootPlace_AnimInstance();
        if (an2) {
            if (twkB(an2, AN_HAS_LFOOT_CATCH)) sawL = true;
            if (twkB(an2, AN_HAS_RFOOT_CATCH)) sawR = true;
        }
    }
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
    TwkLog("[feet] catch over %d frames: L orient=%s peak ratio %.2f (plant f%d) | R orient=%s peak "
           "ratio %.2f (plant f%d) "
           "| deck %.0f -> %.0f deg | flip angle %.0f -> %.0f (target %.0f)%s"
           " | shove %.0f -> %.0f (target %.0f -> %.0f, rate %.0f -> %.0f)"
           " | footType L=%d R=%d -> %d/%d (type flips L=%d R=%d)%s | timers total %.3f force %.3f delay %.3f"
           " -> %.3f/%.3f/%.3f%s",
           frames, sawL ? "YES" : "no ", peakL, attL, sawR ? "YES" : "no ", peakR, attR,
           angAt, ang, curAt, twkF(comp, MC_BOARD_FLIP_CUR), tgtAt,
           (!sawL && !sawR) ? "   <-- NO FOOT CAUGHT" : "",
           rotCurAt, twkF(comp, MC_BOARD_ROT_CUR), rotTgtAt, twkF(comp, MC_BOARD_ROT_TARGET),
           rotRateAt, twkF(comp, MC_BOARD_ROT_RATE),
           typeLat, typeRat, twkB(comp, MC_LFOOT_CATCH), twkB(comp, MC_RFOOT_CATCH),
           typeFlipsL, typeFlipsR, (typeFlipsL + typeFlipsR > 2) ? "   <-- FEET OSCILLATING" : "",
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

// Is a scoop catch live -- armed and not yet released, or within the post-landing tail? This is
// what pop_probe's manual gate asks before letting Skate_CheckForManuals run.
bool CatchTweaks_ScoopCatchLive() {
    if (!g_scoopHold || !g_scoopNoManual) return false;
    if (g_scoopHoldFoot != 0) return true;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return t.QuadPart < g_scoopLiveUntil;
}

// The hold itself. Runs from foot_place's UpdateFootAnchors detour -- inside the animation update,
// after the physics pass has recomputed the foot's catch info -- which is the one phase where
// clearing the type outlasts the game's own write. Released the moment the board is landing, the
// catch ends, or a safety timeout passes.
void CatchTweaks_PostPhysHold() {
    if (!g_scoopHold || g_scoopHoldFoot == 0) return;
    __try {
        void* comp = CatchLevel_MovementComponent();
        void* an   = FootPlace_AnimInstance();
        void* sk   = CatchTweaks_Skater();
        LARGE_INTEGER t, fq; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&fq);
        const int ms = (int)((t.QuadPart - g_scoopHoldQpc) * 1000 / fq.QuadPart);
        // Release on TOUCHDOWN -- IsGrounded -- not IsLanding. IsLanding is the landing phase and
        // is already true at the catch, so the first cut released before its first write, eight
        // times in 50 ms, and the round measured nothing.
        const bool grounded = an && twkB(an, AN_GROUNDED) != 0;
        const int  st       = sk ? twkB(sk, SK_CATCH_ORIENT_STATE) : 0;
        // the state can read 0 for a frame mid-catch; three in a row is the catch really ending
        g_scoopHoldZeros = (st == 0) ? g_scoopHoldZeros + 1 : 0;
        if (!comp || grounded || g_scoopHoldZeros >= 3 || ms > 2500) {
            // The proof is in this line -- but only once it has actually written. A held foot whose
            // ratio never left zero was held; one whose ratio climbed came down anyway, and the type
            // byte is not the lever. Zero writes proves nothing either way.
            TwkLog("[catch] scoop hold released (%s) after %d frames: %d writes, held foot's peak "
                   "ratio %.2f -- %s", !comp ? "no component" : grounded ? "grounded"
                   : st == 0 ? "catch ended" : "timeout", g_scoopHoldFrames, g_scoopHoldWrites,
                   g_scoopHoldPeak,
                   g_scoopHoldMode == 4 ? "catching foot's type re-decided (judge the foot)"
                   : g_scoopHoldMode == 3 ? "board offsets borrowed (judge the foot, see arm line)"
                   : g_scoopHoldMode == 0 ? "probe only, nothing written"
                   : g_scoopHoldWrites == 0 ? "NEVER RAN (released before the first write)"
                   : g_scoopHoldPeak < 0.05f ? "HELD" : "NOT HELD by this mode");
            if (g_nBorrow) EndBorrow();
            // manuals latch AT the landing: keep the gate up for a beat after touchdown
            g_scoopLiveUntil = t.QuadPart + fq.QuadPart * 300 / 1000;
            g_scoopHoldFoot = 0;
            return;
        }
        uint8_t* info = (uint8_t*)comp + (g_scoopHoldFoot == 1 ? MC_LFOOT_CATCH : MC_RFOOT_CATCH);
        ++g_scoopHoldFrames;
        if (g_scoopHoldMode == 4) {
            // Give the CATCHING foot the plain foot type -- the value the game's own Nose/Tail
            // downgrade would have produced had the pitch ratio pointed the right way. The game
            // does NOT preserve it on this path: UpdateFeetCatchInfo re-decides Nose every frame
            // the orient reads state 1 (field: 16-37 writes per catch), so this is written every
            // frame too, and wins because it lands in the animation phase after the physics-pass
            // decision. Keyed on the NARROWED foot, not the state byte -- the state flickers
            // through 3/5/11 mid-catch and a frame the correction sat out is a frame the anim
            // could sample Nose. Only the wrong SHAPES are touched: BothFeet/Nose/Tail (3/4/5). A
            // scoop catch is one-footed by definition, so none of those can be right for it.
            const int catchFoot = 3 - g_scoopHoldFoot;                  // 1 = left, 2 = right
            uint8_t* catching = (uint8_t*)comp + (catchFoot == 1 ? MC_LFOOT_CATCH : MC_RFOOT_CATCH);
            const int want = StanceFootType(catchFoot, GoofyXorSwitch());
            const int cur  = *catching;
            if (want > 0 && cur != want && (cur == 3 || cur == 4 || cur == 5)) {
                static int saidFrom = -1;
                if (g_scoopHoldWrites == 0 || saidFrom != *catching) {
                    saidFrom = *catching;
                    TwkLog("[catch] scoop catch: %s foot type %d -> %d (state %d, stance %d, "
                           "pitch %.2f)", catchFoot == 1 ? "LEFT" : "RIGHT", cur, want, st,
                           GoofyXorSwitch() ? 1 : 0, sk ? twkF(sk, 0x640) : 0.0f);
                }
                *catching = (uint8_t)want; ++g_scoopHoldWrites;
            }
        } else if (g_scoopHoldMode == 2) {
            if (*info != 0) { *info = 0; ++g_scoopHoldWrites; }     // the type byte (does not hold)
        } else if (g_scoopHoldMode == 1 && an) {
            uint8_t* fl = (uint8_t*)an + (g_scoopHoldFoot == 1 ? AN_HAS_LFOOT_CATCH
                                                                : AN_HAS_RFOOT_CATCH);
            if (*fl != 0) { *fl = 0; ++g_scoopHoldWrites; }          // the per-foot orient flag
        }
        const float r = *(float*)(info + FCFI_RATIO);
        if (r > g_scoopHoldPeak && r < 10.0f) g_scoopHoldPeak = r;
        // THE PROBE: every candidate field, a few times across the catch, so the round says which
        // of them actually differs for a foot that stays off versus one that comes down. Read
        // AFTER the write of the chosen mode, so a field that the game immediately re-asserts
        // shows up as re-asserted.
        if (g_scoopHoldFrames == 1 || (g_scoopHoldFrames % 6) == 0) {
            const uint8_t* li = (const uint8_t*)comp + MC_LFOOT_CATCH;
            const uint8_t* ri = (const uint8_t*)comp + MC_RFOOT_CATCH;
            TwkLog("[catch]   hold f%d mode %d st=%d | hasL=%d hasR=%d | typeL=%d typeR=%d | "
                   "ratioL=%.2f ratioR=%.2f | man=%d", g_scoopHoldFrames, g_scoopHoldMode, st,
                   an ? twkB(an, AN_HAS_LFOOT_CATCH) : -1, an ? twkB(an, AN_HAS_RFOOT_CATCH) : -1,
                   *li, *ri, *(const float*)(li + FCFI_RATIO), *(const float*)(ri + FCFI_RATIO),
                   PopProbe_SkaterManualBits());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_scoopHold = 0; g_scoopHoldFoot = 0; }
}

void CatchTweaks_PumpFrame() {
    // If the game does not call SetCatchOrient at all while there is no catch input, there is no
    // call for the override above to ride -- so it is driven here instead. Which of the two paths
    // did the work is in the log, because it decides where any further work belongs.
    if (g_clickCatch && g_clickForce && g_clickForceState && g_origSetOrient) {
        __try {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            if (t.QuadPart < g_clickForceUntil) {
                if (!InterlockedExchange(&g_clickSetterSeen, 0)) {
                    void* sk = CatchTweaks_Skater();
                    if (sk) {
                        static LONGLONG droveFor = 0;
                        if ((g_catchDiag || g_clickProbe > 0) && droveFor != g_clickForceUntil) {
                            droveFor = g_clickForceUntil;
                            TwkLog("[catch] click drove orient state %d from the pump "
                                   "(the game never called the setter)", g_clickForceState);
                        }
                        hkSetCatchOrient(sk, (uint8_t)g_clickForceState, 0.0f, 0.0f);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_clickForce = 0; }
    }
    // The flicked-foot record ages out HERE, above every early-out -- it must decay on ordinary
    // frames or a stale flick would still be naming a foot minutes later.
    if (g_flickFresh > 0 && --g_flickFresh == 0) g_flickPhys = 0;
    // Belt-and-braces latch release: the state==0 call is the normal end of a catch, but if the
    // setter simply STOPS being called instead, the latch would otherwise persist into the next one.
    if ((g_flickLatch != 0 || g_holdState != 0 || g_vetoAdmitted != 0) && ++g_latchIdle > 10) {
        g_flickLatch = 0; g_latchIdle = 0;
        g_holdState = 0; g_holdLoggedState = 0;
        g_vetoAdmitted = 0;                                 // same lifetime as the other two
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
                if (g_airMissed) { g_airMissed = false; g_missPending = true; }   // run_out delivers it
                // THE VETO ADMISSION TOO. It was cleared only by the setter's state-0 call, and a
                // stick held through the landing keeps the orient state alive (the game's own
                // post-landing hold-to-orient), so the setter never sent a 0 and the admission
                // survived into the NEXT air. With it stale, the fresh-flick veto stood down and a
                // held stick engaged the catch at the pop -- the pose with a full flip still owed,
                // i.e. the catch animation with the foot hovering above the deck (field: user
                // screenshot), persisting trick after trick until some later catch finally sent a
                // 0. That is "stuck for a bit, then randomly fine". An admission is a fact about ONE
                // catch; the ground ends it.
                g_vetoAdmitted = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Several FIXES live in here alongside the diagnostic trace, so this must not early-out on the
    // trace flag -- doing so silently disabled a fix the moment logging was turned off for release.
    // Each part checks its own switch below, and every switch must appear here: leaving one out
    // quietly ties that feature to whichever OTHER switch happens to be on.
    if (!g_flipTrace && !g_stopFlip && !g_anyRev && !g_footLevel && !g_unstick && !g_minSpinDeg &&
        !g_footDescend) return;
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
            static float rotPrev  = 0.0f;
            const long ser = FlipSpeed_TrickSerial();
            // The shove axis, read off the learned movement component; a sentinel read leaves the
            // rotation measure where it was rather than adding garbage to it.
            void* compS = CatchLevel_MovementComponent();
            float rotNow = compS ? twkF(compS, MC_BOARD_ROT_CUR) : -999999.0f;
            const bool rotOk = fabsf(rotNow) < 1e5f;
            if (ser != spinSerial) {
                spinSerial = ser; g_trickTravelDeg = 0.0f; g_trickRotDeg = 0.0f; spinPrev = ang;
                g_deckRemaining = 0.0f;
                rotPrev = rotOk ? rotNow : 0.0f;
            } else if (spinPrev >= 0.0f) {
                const float d = fabsf(ang - spinPrev);
                if (d < 170.0f) g_trickTravelDeg += d;   // ignore the wrap at 360
                spinPrev = ang;
                if (rotOk) {
                    const float dr = fabsf(rotNow - rotPrev);
                    if (dr < 170.0f) g_trickRotDeg += dr;   // a reset to 0 is a jump, not travel
                    rotPrev = rotNow;
                }
            }
        }
        // ---- the fix: a registered catch ends the flip at the first grip-up ----------------------
        // `_boardFlipRate` is zeroed rather than the angle being written: the deck is already AT the
        // orientation a completed flip ends on, so removing the rate leaves it exactly there and lets
        // every other system (landing, pitch, the catch's own alignment) carry on untouched.
        // The whole block is shared plumbing (comp/catchState/delta) for the stop, the flip-ending
        // AND the foot-level drive -- it must open on the whole family, or turning one off silently
        // kills the others (it did: the stop's toggle used to gate all three).
        if (g_stopFlip || g_anyRev || g_footLevel || g_footDescend) {
            void* comp = CatchLevel_MovementComponent();
            const int catchState = twkB(skater, SK_CATCH_ORIENT_STATE);
            static bool  armed = false;
            static float prevAng = -1.0f;
            const float delta = (prevAng >= 0.0f) ? (ang - prevAng) : 0.0f;
            // The deck's distance to grip-up along its current direction -- the snap's own measure,
            // kept every frame so the setter's early-press gate can read it at the press.
            if (fabsf(delta) > 0.01f) g_deckRemaining = (delta > 0.0f) ? (180.0f - ang) : (ang + 180.0f);
            // ---- OVER-ROTATION: roll the deck BACK to flat, the foot coming down with it --------------
            // See the CatchFootLevelsBoard knob. Everything here is measured off the game's FLIP COUNTER
            // (_boardFlipCurrentAngle against the trick's own flat: 360 for a kickflip, 355 for a tre) --
            // that is what the verdict judges and what the foot ratio is computed against. The rendered
            // deck angle is NOT used to classify. Field (3.19.249 log, every catch): the rendered deck
            // lags the counter by a variable 10-50 deg and was still RISING toward flat while the counter
            // had already gone past it, so a classifier on its direction never saw an over-rotation.
            // Ahead of the snap and the aim-at-flat so both stand down on a catch this owns.
            {
                static void* ovDef = nullptr;
                static float ovBaseF = 0.0f, ovE0 = 0.0f, ovRate = 0.0f, ovFlat = 0.0f;
                static float ovTgtSign = 1.0f, ovRateSign = 1.0f, ovPrevRem = 0.0f, ovPeak = 0.0f;
                static int   ovFrames = 0, ovWrong = 0;
                static long  ovSerial = -1;
                static bool  ovTried = false;            // one decision per catch, on its first frame
                void* anOv = FootPlace_AnimInstance();
                const bool ovGrounded = anOv && twkB(anOv, AN_GROUNDED) != 0;
                if (catchState == 0) ovTried = false;
                // One drive write, shared by the per-frame loop and the arm frame (the first cut lost
                // a whole frame of forward flip between the decision and its first write).
                auto ovWrite = [&](float cur, float remaining) {
                    *(float*)((uint8_t*)comp + MC_BOARD_FLIP_RATE)   = ovRateSign * ovRate;
                    *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = ovTgtSign * (fabsf(cur) + remaining);
                    if (remaining > ovE0) ovE0 = remaining;          // the window spans the true start
                    if (ovDef) *(float*)((uint8_t*)ovDef + DEF_FLIP_PRECATCH_ANGLE) = ovE0 + 1.0f;
                };
                if (g_overActive) {
                    const float rL = comp ? twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO) : 0.0f;
                    const float rR = comp ? twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO) : 0.0f;
                    const float ratio = (rL > rR) ? rL : rR;
                    if (ratio > ovPeak && ratio < 10.0f) ovPeak = ratio;
                    const float cur = comp ? twkF(comp, MC_BOARD_FLIP_CUR) : 0.0f;
                    const float remaining = fabsf(cur) - ovFlat;         // counter degrees still past flat
                    const bool  ended = (catchState == 0) || ovGrounded || !comp ||
                                        FlipSpeed_TrickSerial() != ovSerial;
                    if (!ended) ovWrong = (remaining > ovPrevRem + 0.5f) ? ovWrong + 1 : 0;
                    const bool  arrived = !ended && remaining <= 1.0f;
                    const bool  wrong   = !ended && ovWrong >= 3;
                    const bool  timeout = !ended && ovFrames > 90;
                    if (ended || arrived || wrong || timeout) {
                        // WHATEVER ends it, the reversed rate comes off. The first cut skipped this on
                        // "catch ended", and a touchdown mid-roll-back left the board flipping backwards
                        // for good (field: "it just keeps flipping back the opposite direction").
                        if (comp) {
                            *(float*)((uint8_t*)comp + MC_BOARD_FLIP_RATE) = 0.0f;
                            // The target comes to the counter WHEREVER it stopped -- not to the flat it
                            // was aiming at. The last step overshoots flat by 1-5 deg every time (the
                            // game's clamp reads a target one step stale), and with the target left at
                            // flat the foot still owed those degrees: on every small roll-back (16-44
                            // deg) the plant fix's threshold let it through, the ratio peaked at
                            // 0.91-0.97 and the foot never planted (field: "something about the catch
                            // looks odd"). Owed 0 here completes the ratio by construction; the game's
                            // roll-align levels the last degree or two under the planted foot.
                            *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = ovTgtSign * fabsf(cur);
                        }
                        if (ovDef) {
                            float backF = ovBaseF;
                            for (int i = 0; i < g_nDefSave; i++)
                                if (g_defSave[i].def == ovDef) {
                                    backF = g_widened ? g_defSave[i].flip * g_appliedMult : g_defSave[i].flip;
                                    break;
                                }
                            *(float*)((uint8_t*)ovDef + DEF_FLIP_PRECATCH_ANGLE) = backF;
                        }
                        TwkLog("[catch] over-rotation: rolled back %.0f of %.0f deg in %d frames (deck now reads "
                               "%.0f), foot ratio %.2f -- %s", ovE0 - remaining, ovE0, ovFrames, ang, ovPeak,
                               arrived ? "flat, planted" : wrong ? "WRONG WAY (counter moved further past "
                               "flat) -- aborted, planted where it is" : timeout ? "timeout, planted" :
                               ovGrounded ? "TOUCHED DOWN first (flip stopped where it was)" :
                               (catchState == 0) ? "catch ended first (flip stopped where it was)" : "trick changed");
                        g_overActive = false; ovDef = nullptr; ovFrames = 0; ovWrong = 0;
                    } else {
                        ovWrite(cur, remaining);
                        if (g_overTrace)
                            TwkLog("[over] f%02d cur=%.0f rem=%.1f tgt=%.0f rate=%.0f | deck=%.1f roll=%.1f | "
                                   "ratio L=%.2f R=%.2f flags=0x%02x st=%d | shove %.0f/%.0f r%.0f", ovFrames,
                                   cur, remaining, ovTgtSign * (fabsf(cur) + remaining), ovRateSign * ovRate, ang,
                                   twkF(comp, MC_ANIM_ROLL), rL, rR, twkB(comp, MC_BOARD_FLAGS), catchState,
                                   twkF(comp, MC_BOARD_ROT_CUR), twkF(comp, MC_BOARD_ROT_TARGET),
                                   twkF(comp, MC_BOARD_ROT_RATE));
                        ovPrevRem = remaining; ++ovFrames;
                    }
                }
                // The decision, on the catch's FIRST frame, off the counter.
                if (!g_overActive && !ovTried && catchState != 0 && comp) {
                    ovTried = true;
                    const float cur0  = twkF(comp, MC_BOARD_FLIP_CUR);
                    const float tgt0  = twkF(comp, MC_BOARD_FLIP_TARGET);
                    const float rate0 = twkF(comp, MC_BOARD_FLIP_RATE);
                    const int   flags = twkB(comp, MC_BOARD_FLAGS);
                    const bool  sane  = fabsf(cur0) < 3600.0f && fabsf(tgt0) < 3600.0f && fabsf(rate0) < 100000.0f;
                    // The trick's own flat: its target, divided by however many revolutions the game
                    // has extended it to (720 = a 360 flip run past its mark, 710 = a 355 tre).
                    float base = fabsf(tgt0);
                    { const float revs = floorf(base / 360.0f + 0.5f); if (revs >= 1.0f) base /= revs; }
                    if (!(base > 200.0f && base < 400.0f)) base = 360.0f;
                    const float nearest = base * floorf(fabsf(cur0) / base + 0.5f);
                    const float over    = fabsf(cur0) - nearest;         // > 0 past flat, < 0 short of it
                    if (sane && (flags & 0x08)) {
                        TwkLog("[catch] engage: flip %.0f of %.0f = %.0f deg %s the nearest flat (%.0f) | deck "
                               "reads %.0f, %s | shove %.0f of %.0f at %.0f deg/s", cur0, tgt0, fabsf(over),
                               over > 0.0f ? "PAST" : "short of", nearest, ang,
                               delta > 0.0f ? "rising" : delta < 0.0f ? "falling" : "still",
                               twkF(comp, MC_BOARD_ROT_CUR), twkF(comp, MC_BOARD_ROT_TARGET),
                               twkF(comp, MC_BOARD_ROT_RATE));
                        const float pastFlat = fabsf(cur0) - base;    // counter degrees past the trick's flat (log)
                        // The bail judges the RENDERED deck, past side only (see the knob): heading away
                        // from flat = past it; still rising = short, whatever the counter says.
                        const float visPast = (delta < -0.3f) ? (180.0f - ang) : 0.0f;
                        if (g_overBailDeg > 0 && visPast > (float)g_overBailDeg && !AutoCatchActive()) {
                            TwkLog("[catch] over-rotated: the deck reads %.0f deg past flat and heading away (counter "
                                   "%.0f past), more than CatchOverBailDeg %d -- BAIL", visPast, pastFlat, g_overBailDeg);
                            RunOut_BailNow(skater);
                        } else if (g_footLevel && g_anyRev && !ovGrounded && over > 3.0f && fabsf(rate0) > 1.0f) {
                            void* def = twkP(skater, SK_CURRENT_TRICK_DEF);
                            const float curF = def ? twkF(def, DEF_FLIP_PRECATCH_ANGLE) : 0.0f;
                            if (def && curF > 0.0f && curF <= 3600.0f) {
                                ovDef = def; ovBaseF = curF; ovSerial = FlipSpeed_TrickSerial();
                                ovE0 = over; ovFlat = nearest;
                                // its own budget (CatchOverMs): the air left decides, not the trick's speed
                                float want = over / ((float)g_overMs / 1000.0f);
                                if (want > 3500.0f) want = 3500.0f;
                                if (want < 60.0f)   want = 60.0f;
                                ovRate     = want;
                                ovRateSign = (rate0 < 0.0f) ? 1.0f : -1.0f;      // the OPPOSITE of the flip
                                ovTgtSign  = (tgt0 < 0.0f) ? -1.0f : 1.0f;
                                ovPrevRem = over; ovFrames = 0; ovWrong = 0; ovPeak = 0.0f;
                                g_overActive = true;
                                ovWrite(cur0, over);                     // this frame, not the next
                                TwkLog("[catch] over-rotated: caught %.0f deg PAST flat -- rolling the flip back "
                                       "over ~%d ms at %.0f deg/s (flip was %.0f), the foot coming down with it",
                                       over, g_overMs, want, fabsf(rate0));
                            }
                        } else if (over < -3.0f && fabsf(cur0) > base + 3.0f) {
                            // Past 180 on the counter: the nearest flat is the NEXT revolution, the
                            // game's own extended target already points at it, and the board completes
                            // forward -- a double. Named in the log so it is never mistaken for a rescue.
                            TwkLog("[catch] over-rotated %.0f deg past the trick's flat (%.0f short of the next) "
                                   "-- past the halfway mark: completing forward, a double", pastFlat, -over);                        } else if (over > 3.0f) {
                            TwkLog("[catch] over-rotated %.0f deg but not rolled back: %s", over,
                                   !g_footLevel ? "Over rotation leveling is off" :
                                   !g_anyRev ? "Foot always attaches is off" : ovGrounded ? "grounded" : "no flip rate");
                        }
                    }
                }
            }
            // ---- OVER-ROTATED SHOVE: stop it where it is caught (CatchShoveRollBack) --------------------
            // See the knob. Ahead of the snap: a catch this claims is not the snap's to hurry forward.
            {
                // The trick's AUTHORED shove: the LAST rotation target seen while the shove is still
                // getting under way (counter < 60). The target field only ever shows the CURRENT value:
                // it RAMPS UP over the first frames of the flick (a first-nonzero read captured 21 deg
                // on an FS pop shove-it), and the game rewrites it upward the frame the shove runs past
                // it -- which cannot happen before the counter reaches it, so freezing at 60 is safe.
                // A NEW shove is any of: flip_speed's trick serial changing (pure shoves never bump it
                // -- field: that 21-deg mark stayed stale all session and every catch read as 125-219
                // past it, so every one was finished forward to a full 360), the rotation flag rising,
                // or the target reading 0 (it is zeroed at landing).
                static long  shSerial = -1;
                static float shAuth = 0.0f;
                static bool  shRotWas = false;
                {
                    const long  ser    = FlipSpeed_TrickSerial();
                    const bool  rotNow = comp && (twkB(comp, MC_BOARD_FLAGS) & 0x20) != 0;
                    const float t = comp ? fabsf(twkF(comp, MC_BOARD_ROT_TARGET)) : 0.0f;
                    const float c = comp ? fabsf(twkF(comp, MC_BOARD_ROT_CUR))    : 0.0f;
                    if (ser != shSerial || (rotNow && !shRotWas) || t < 1.0f) { shSerial = ser; shAuth = 0.0f; }
                    shRotWas = rotNow;
                    if (rotNow && t > 10.0f && t < 3600.0f && c < 60.0f) shAuth = t;
                }
                static bool srTried = false;
                void* anSr = FootPlace_AnimInstance();
                const bool srGrounded = anSr && twkB(anSr, AN_GROUNDED) != 0;
                if (catchState == 0) { srTried = false; g_shoveRbClaimed = false; }
                if (!srTried && catchState != 0 && comp && g_shoveRollBack && g_shoveFixes) {
                    srTried = true;
                    const int   flags = twkB(comp, MC_BOARD_FLAGS);
                    const float rc0 = twkF(comp, MC_BOARD_ROT_CUR), rt0 = twkF(comp, MC_BOARD_ROT_TARGET);
                    const float rr0 = twkF(comp, MC_BOARD_ROT_RATE);
                    const float c = fabsf(rc0), t = fabsf(rt0);
                    const bool  sane = c < 3600.0f && t < 3600.0f && fabsf(rr0) < 100000.0f;
                    // ---- the sideways band (see CatchShoveBailBandDeg): decided first, before the stop
                    // and before the snap can claim the catch.
                    if (g_shoveBailBand > 0 && sane && (flags & 0x20) && !srGrounded && !AutoCatchActive()) {
                        float m = fmodf(c, 180.0f); if (m < 0.0f) m += 180.0f;
                        const float off90 = fabsf(m - 90.0f);
                        if (off90 <= (float)g_shoveBailBand) {
                            g_shoveRbClaimed = true;             // neither the stop nor the snap touches it
                            TwkLog("[catch] shove caught %.0f deg from sideways (counter %.0f, %.0f mod 180), inside "
                                   "CatchShoveBailBandDeg %d -- BAIL", off90, c, m, g_shoveBailBand);
                            RunOut_BailNow(skater);
                        }
                    }
                    if (!g_shoveRbClaimed && sane && (flags & 0x20) && !srGrounded && shAuth > 10.0f &&
                        t > shAuth + 3.0f && c > shAuth + 3.0f) {
                        const float over = c - shAuth;              // past the authored mark
                        {
                            // Stop it here, however far past: both rate fields (the game's ease has nothing to resume
                            // toward) and the target brought to the counter (owed 0). The shove-axis
                            // plant fix keeps it there if anything nudges the target later.
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE)     = 0.0f;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE + 4) = 0.0f;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_TARGET)   = (rt0 < 0.0f ? -1.0f : 1.0f) * c;
                            g_shoveRbClaimed = true;
                            TwkLog("[catch] shove over-rotated: caught %.0f deg past its %.0f mark (target extended to "
                                   "%.0f, was turning at %.0f deg/s) -- stopped where it is, the foot takes it there",
                                   over, shAuth, t, fabsf(rr0));
                        }
                    }
                }
            }
            // ---- UNDER-ROTATED SHOVE: finish the rotation in time for the foot (CatchShoveSnap) ---------
            // See the knob. Independent of the flip drive above -- a shove+flip caught short on the shove
            // and past on the flip runs both: the flip rolls back while the shove hurries on.
            {
                static bool  ssActive = false;
                static float ssRate = 0.0f, ssOwed0 = 0.0f, ssLastWrite = 0.0f;
                static int   ssFrames = 0, ssStomped = 0;
                static long  ssSerial = -1;
                if (catchState == 0) g_shoveSnapTried = false;
                void* anSs = FootPlace_AnimInstance();
                const bool ssGrounded = anSs && twkB(anSs, AN_GROUNDED) != 0;
                if (ssActive) {
                    const float rc = comp ? twkF(comp, MC_BOARD_ROT_CUR)    : 0.0f;
                    const float rt = comp ? twkF(comp, MC_BOARD_ROT_TARGET) : 0.0f;
                    const float rr = comp ? twkF(comp, MC_BOARD_ROT_RATE)   : 0.0f;
                    const float owed = fabsf(rt) - fabsf(rc);
                    const float rL = comp ? twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO) : 0.0f;
                    const float rR = comp ? twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO) : 0.0f;
                    // stomp probe: the rate read back off what was written last frame
                    if (comp && fabsf(fabsf(rr) - ssLastWrite) > ssLastWrite * 0.1f + 1.0f) ++ssStomped;
                    const bool ended   = (catchState == 0) || ssGrounded || !comp ||
                                         FlipSpeed_TrickSerial() != ssSerial;
                    const bool arrived = !ended && owed <= 1.0f;
                    const bool timeout = !ended && ssFrames > 90;
                    if (ended || arrived || timeout) {
                        if (comp && !ended) {
                            // Stop the shove where it is and bring its target to it: owed 0, the
                            // rotation term completes. Both rate fields, so the game's own ease has
                            // nothing to resume toward.
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE)     = 0.0f;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE + 4) = 0.0f;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_TARGET)   = (rt < 0.0f) ? -fabsf(rc) : fabsf(rc);
                        }
                        TwkLog("[catch] shove snap: finished %.0f of %.0f deg in %d frames (rate read back %.0f, "
                               "stomped %d), foot ratio L=%.2f R=%.2f -- %s", ssOwed0 - owed, ssOwed0, ssFrames,
                               rr, ssStomped, rL, rR,
                               arrived ? "arrived, planted" : timeout ? "timeout" :
                               ssGrounded ? "TOUCHED DOWN first" : (catchState == 0) ? "catch ended first" :
                               "trick changed");
                        ssActive = false; ssFrames = 0; ssStomped = 0;
                    } else {
                        const float sgn = (rr < 0.0f) ? -1.0f : 1.0f;
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE)     = sgn * ssRate;
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE + 4) = sgn * ssRate;
                        ssLastWrite = ssRate;
                        if (g_overTrace)
                            TwkLog("[shove] f%02d cur=%.0f tgt=%.0f owed=%.1f rate=%.0f | ratio L=%.2f R=%.2f "
                                   "flags=0x%02x st=%d", ssFrames, rc, rt, owed, rr, rL, rR,
                                   twkB(comp, MC_BOARD_FLAGS), catchState);
                        ++ssFrames;
                    }
                }
                if (!ssActive && !g_shoveSnapTried && !g_shoveRbClaimed &&
                    catchState != 0 && comp && g_shoveSnap && g_shoveFixes) {
                    g_shoveSnapTried = true;
                    const int   flags = twkB(comp, MC_BOARD_FLAGS);
                    const float rc0 = twkF(comp, MC_BOARD_ROT_CUR), rt0 = twkF(comp, MC_BOARD_ROT_TARGET);
                    const float rr0 = twkF(comp, MC_BOARD_ROT_RATE);
                    const float owed0 = fabsf(rt0) - fabsf(rc0);
                    const bool  sane = fabsf(rc0) < 3600.0f && fabsf(rt0) < 3600.0f && fabsf(rr0) < 100000.0f;
                    if (sane && (flags & 0x20) && !ssGrounded && owed0 > 3.0f && owed0 <= 270.0f && fabsf(rr0) > 1.0f) {
                      if (owed0 > (float)g_shoveFinishMaxDeg) {
                        // Caught where it is (see CatchShoveFinishMaxDeg): the rotation stops and the
                        // target comes to the counter -- owed 0, the foot lands on the board as it is.
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE)     = 0.0f;
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE + 4) = 0.0f;
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_TARGET)   = (rt0 < 0.0f ? -1.0f : 1.0f) * fabsf(rc0);
                        g_shoveRbClaimed = true;
                        TwkLog("[catch] shove caught %.0f deg short (%.0f of %.0f, turning at %.0f deg/s), more than "
                               "CatchShoveFinishMaxDeg %d -- caught where it is: the board stops, the foot takes it there",
                               owed0, fabsf(rc0), fabsf(rt0), fabsf(rr0), g_shoveFinishMaxDeg);
                      } else {
                        float want = owed0 / ((float)g_snapMs / 1000.0f);
                        if (want > (float)g_shoveSnapMaxRate) want = (float)g_shoveSnapMaxRate;
                        g_catchRescued = true;      // the descent opens the rotation window to the owed amount
                        if (want > fabsf(rr0)) {
                            ssActive = true; ssRate = want; ssOwed0 = owed0; ssFrames = 0; ssStomped = 0;
                            ssSerial = FlipSpeed_TrickSerial();
                            const float sgn = (rr0 < 0.0f) ? -1.0f : 1.0f;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE)     = sgn * want;
                            *(float*)((uint8_t*)comp + MC_BOARD_ROT_RATE + 4) = sgn * want;
                            ssLastWrite = want;
                            TwkLog("[catch] shove snap: caught with the shove %.0f deg short (%.0f of %.0f) at %.0f "
                                   "deg/s -- finishing it in %d ms (%.0f deg/s), the foot coming down with it",
                                   owed0, fabsf(rc0), fabsf(rt0), fabsf(rr0), g_snapMs, want);
                        } else {
                            TwkLog("[catch] shove snap: caught with the shove %.0f deg short at %.0f deg/s -- fast "
                                   "enough, opening the foot's window to it", owed0, fabsf(rr0));
                        }
                      }
                    }
                }
            }
            if (g_stopFlip && catchState != 0 && !armed && !g_overActive) {
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
                    if (remaining > 40.0f && fabsf(rate) > 1.0f) {
                        if (remaining <= (float)g_snapMaxDeg) {
                            // In range = the foot comes down WITH the deck (CatchFootDescends),
                            // whether or not the rate needed a boost to arrive in time. It used to
                            // arm only on a boost, so a flip already fast enough kept the rig's own
                            // short window: the foot hung at zero until the last stretch, then
                            // dropped -- the same float, one case over.
                            g_catchRescued = true;
                            if (want > fabsf(rate)) {
                                float capped = want;
                                if (capped > maxByBoost) capped = maxByBoost;
                                if (capped > 2500.0f)    capped = 2500.0f;   // absolute ceiling
                                *(float*)((uint8_t*)comp + MC_BOARD_FLIP_RATE) = (rate < 0.0f) ? -capped : capped;
                                TwkLog("[catch] caught %.0f deg short -- finishing the flip in %d ms "
                                       "(%.0f -> %.0f deg/s)", remaining, g_snapMs, fabsf(rate), capped);
                            }
                        } else {
                            // PAST the snap's range NOTHING rescues this catch -- not the snap, not a
                            // boost (3.19.233 tried one and was wrong: it turned a press that should
                            // fail into a made catch), not the foot descent, not the plant fix. The
                            // pose engages as the game's own manual catch engages it, the feet attach
                            // only within the rig's window, and if the deck has not come round by
                            // touchdown the landing fails: bail, or the run-out where enabled. That is
                            // the stock rule, and it is the point of manual catch. Logged so the
                            // refusal is never silent again (field: two kickflips pressed ~140 deg in
                            // with the deck 260 from grip-up, no line, "caught upside down").
                            // (with CatchEarlyMiss on this branch is unreachable on manual: the
                            // press was refused at the setter and never engaged)
                            TwkLog("[catch] caught %.0f deg short -- past CatchSnapMaxDeg (%d): no rescue, "
                                   "the flip finishes on its own; land it or bail", remaining, g_snapMaxDeg);
                        }
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
            if (g_anyRev && comp && catchState != 0 && !endedThisCatch && !g_overActive &&
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
                    float aimMag;
                    if (g_anyRevLegacy) {
                        aimMag = fabsf(cur) + (180.0f - ang);   // the old rendered-angle aim (A/B only)
                    } else {
                        // COUNTER-BASED (see CatchAnyRevLegacyAim): the nearest flat AHEAD of the counter.
                        // Unless the game has extended the target -- a run past the mark, which is the
                        // over-rotation drive's business -- that is the target it already has, and
                        // nothing is written: the flip ends exactly level, no over-aim to twist back.
                        float base = fabsf(tgt);
                        { const float revs = floorf(base / 360.0f + 0.5f); if (revs >= 1.0f) base /= revs; }
                        if (!(base > 200.0f && base < 400.0f)) base = 360.0f;
                        aimMag = base * ceilf(fabsf(cur) / base - 0.01f);
                        if (aimMag < fabsf(cur)) aimMag = fabsf(cur);
                    }
                    const float aim = (tgt < 0.0f) ? -aimMag : aimMag;
                    if (fabsf(aim - tgt) > 1.0f) {
                        *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = aim;
                        endedThisCatch = true;
                        TwkLog("[catch] caught with the flip at %.0f (deck reads %.0f) -- aiming it at the nearest "
                               "flat ahead, %.0f deg away (target %.0f -> %.0f, game wanted %.0f more)%s",
                               fabsf(cur), ang, aimMag - fabsf(cur), tgt, aim, owed,
                               g_anyRevLegacy ? "  [legacy rendered-angle aim]" : "");
                    }
                }
            }
            // ---- a stopped board must not owe rotation (CatchPlantFix) ----------------------
            // Every catch path that halts the deck outside the AnyRev window -- StopFlip parking
            // at grip-up, the MaxCut "let it finish" that then stalls, the attach window refusing
            // -- leaves _boardFlipTargetAngle above the frozen current angle, and the per-foot
            // CatchRatio (computed against the target) freezes with it: the foot hangs partway
            // down. Rate ~0 with rotation still owed is that exact stuck state and can never
            // resolve itself, so the target is brought to the deck: ratio completes, foot plants.
            if (g_plantFix && comp && catchState != 0 && !g_overActive) {
                const float pfTgt  = twkF(comp, MC_BOARD_FLIP_TARGET);
                const float pfCur  = twkF(comp, MC_BOARD_FLIP_CUR);
                const float pfRate = twkF(comp, MC_BOARD_FLIP_RATE);
                const float pfOwed = fabsf(pfTgt) - fabsf(pfCur);
                // ...and only NEAR home. Far from it a stopped deck is a failed catch, and completing
                // the ratio would plant the foot on an upside-down board and make the catch.
                // > 0.5, not > 2: the over-rotation roll-back stops 1-5 deg off its mark, and the foot's
                // ratio is 1 - owed/window with a 20-40 deg window -- two degrees owed is a ratio of 0.95
                // and a foot hovering a hair off the deck (field, 3.19.251).
                if (fabsf(pfRate) < 1.0f && pfOwed > 0.5f && pfOwed <= 90.0f) {
                    const float pfAim = (pfTgt < 0.0f) ? -fabsf(pfCur) : fabsf(pfCur);
                    *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = pfAim;
                    static long plantLogged = -1;
                    const long pfSer = FlipSpeed_TrickSerial();
                    if (plantLogged != pfSer) {
                        plantLogged = pfSer;
                        TwkLog("[catch] plant fix: deck stopped still owing %.0f deg (target %.0f, "
                               "current %.0f) -- releasing the ratio so the foot lands", pfOwed,
                               pfTgt, pfCur);
                    }
                }
                // ---- the same on the SHOVE axis. The foot ratio is the SMALLER of the flip term and
                // the rotation term (UpdateFeetCatchInfo runs the same owed/window math on
                // _boardRotationTarget/Current while flag 0x20 is set), so a stopped board that still
                // owes ROTATION pins the ratio exactly as owed flip does -- and nothing ever released
                // it. Field (3.19.252 survey): hardflips and inward heels (180 shove + flip) caught at
                // grip-up with the board stopped and the catching foot never attaching, roll-back or
                // not, while every tre flip (360 shove) planted. No distance cap on this axis: the
                // board's YAW does not decide whether the griptape is up, so a stopped board is
                // catchable at any yaw. Gated on the flip being settled too, so a shove that simply
                // has not started yet is never zeroed.
                {
                    const int   pfFlags   = twkB(comp, MC_BOARD_FLAGS);
                    const float pfRotTgt  = twkF(comp, MC_BOARD_ROT_TARGET);
                    const float pfRotCur  = twkF(comp, MC_BOARD_ROT_CUR);
                    const float pfRotRate = twkF(comp, MC_BOARD_ROT_RATE);
                    const float pfRotOwed = fabsf(pfRotTgt) - fabsf(pfRotCur);
                    const bool  pfRotSane = fabsf(pfRotTgt) < 3600.0f && fabsf(pfRotCur) < 3600.0f &&
                                            fabsf(pfRotRate) < 100000.0f;
                    const bool  flipSettled = fabsf(pfRate) < 1.0f || !(pfFlags & 0x08) || pfOwed <= 0.5f;
                    if (g_shoveFixes && pfRotSane && (pfFlags & 0x20) && flipSettled && fabsf(pfRotRate) < 1.0f && pfRotOwed > 0.5f) {
                        const float pfRotAim = (pfRotTgt < 0.0f) ? -fabsf(pfRotCur) : fabsf(pfRotCur);
                        *(float*)((uint8_t*)comp + MC_BOARD_ROT_TARGET) = pfRotAim;
                        static long rotPlantLogged = -1;
                        const long pfSer2 = FlipSpeed_TrickSerial();
                        if (rotPlantLogged != pfSer2) {
                            rotPlantLogged = pfSer2;
                            TwkLog("[catch] plant fix (shove axis): board stopped still owing %.0f deg of "
                                   "rotation (target %.0f, current %.0f, rate %.0f) -- releasing the ratio so "
                                   "the foot lands", pfRotOwed, pfRotTgt, pfRotCur, pfRotRate);
                        }
                    }
                }
            }
            // ---- the foot comes down WITH the board (CatchFootDescends) -----------------------
            // See the knob. Runs AFTER the AnyRev retarget and the plant fix above, so the owed
            // amount it solves against is this pass's final goalpost -- the hold lands in the same
            // frame as the retarget and the ratio never sees the dip.
            {
                static void* fdDef = nullptr;
                static float fdWF = 0.0f, fdWR = 0.0f;         // the windows currently written
                static float fdBaseF = 0.0f, fdBaseR = 0.0f;   // the def's values when we began
                static float fdOwedF0 = 0.0f, fdOwedR0 = 0.0f; // owed at engage, for the log
                static int   fdFrames = 0, fdHolds = 0;
                static long  fdSerial = -1;
                void* anFd = FootPlace_AnimInstance();
                const bool fdGrounded = anFd && twkB(anFd, AN_GROUNDED) != 0;
                const float rL = comp ? twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO) : 0.0f;
                const float rR = comp ? twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO) : 0.0f;
                const float r  = (rL > rR) ? rL : rR;
                if (catchState == 0) g_catchRescued = false;
                bool fdEnd = (catchState == 0) || fdGrounded || !comp || !g_footDescend;
                if (fdDef && !fdEnd && FlipSpeed_TrickSerial() != fdSerial) fdEnd = true;
                if (fdDef && (fdEnd || r >= 0.99f)) {
                    // Put back what the widen system says this def carries -- not the value seen at
                    // begin, which a catch-mode transition mid-catch could have made stale.
                    float backF = fdBaseF, backR = fdBaseR;
                    for (int i = 0; i < g_nDefSave; i++)
                        if (g_defSave[i].def == fdDef) {
                            backF = g_widened ? g_defSave[i].flip * g_appliedMult : g_defSave[i].flip;
                            backR = g_widened ? g_defSave[i].rot  * g_appliedMult : g_defSave[i].rot;
                            break;
                        }
                    *(float*)((uint8_t*)fdDef + DEF_FLIP_PRECATCH_ANGLE) = backF;
                    *(float*)((uint8_t*)fdDef + DEF_ROT_PRECATCH_ANGLE)  = backR;
                    // Ungated like the [feet] engage/over lines it sits between: one line per catch
                    // that needed the drive (a press inside the window prints nothing).
                    TwkLog("[catch] foot descent: window flip %.0f -> %.0f, rot %.0f -> %.0f deg "
                               "(owed %.0f/%.0f at engage) over %d frames, %d retarget holds, ratio %.2f "
                           "-- %s", fdBaseF, fdWF, fdBaseR, fdWR, fdOwedF0, fdOwedR0, fdFrames,
                           fdHolds, r, (r >= 0.99f) ? "planted" : (catchState == 0) ? "catch ended"
                                                    : fdGrounded ? "grounded" : "released");
                    fdDef = nullptr; fdFrames = 0; fdHolds = 0;
                }
                if (!fdEnd && r < 0.99f) {
                    void* def = twkP(skater, SK_CURRENT_TRICK_DEF);
                    if (def && (!fdDef || def == fdDef)) {
                        const int flags = twkB(comp, MC_BOARD_FLAGS);
                        // Each axis counts only while the game is still applying its window.
                        const float owedF = (flags & 0x08)
                            ? fabsf(twkF(comp, MC_BOARD_FLIP_TARGET)) - fabsf(twkF(comp, MC_BOARD_FLIP_CUR)) : 0.0f;
                        const float owedR = (flags & 0x20)
                            ? fabsf(twkF(comp, MC_BOARD_ROT_TARGET))  - fabsf(twkF(comp, MC_BOARD_ROT_CUR))  : 0.0f;
                        const float curF  = twkF(def, DEF_FLIP_PRECATCH_ANGLE);
                        const float curR  = twkF(def, DEF_ROT_PRECATCH_ANGLE);
                        const bool sane = curF > 0.0f && curF <= 3600.0f && curR > 0.0f && curR <= 3600.0f &&
                                          owedF > -3600.0f && owedF < 3600.0f &&
                                          owedR > -3600.0f && owedR < 3600.0f;
                        if (!fdDef) {
                            // Begin only where the game's own window would pin the ratio at 0.
                            if (sane && g_catchRescued && !g_overActive && (owedF > curF || owedR > curR)) {
                                fdDef = def; fdSerial = FlipSpeed_TrickSerial();
                                fdBaseF = curF; fdBaseR = curR;
                                fdOwedF0 = owedF; fdOwedR0 = owedR;
                                const float capW = (float)g_descendDeg;
                                fdWF = (owedF > curF) ? ((owedF < capW) ? owedF : capW) + 1.0f : curF;
                                fdWR = (owedR > curR) ? ((owedR < capW) ? owedR : capW) + 1.0f : curR;
                                fdFrames = 0; fdHolds = 0;
                            }
                        } else if (sane) {
                            // The goalpost moved out (a retarget): the ratio the game would compute,
                            // 1 - owed/window, has fallen below where the foot already is. Re-solve
                            // the window so it comes out exactly there instead, and let it climb on.
                            const float keep = (1.0f - r) < 0.02f ? 0.02f : (1.0f - r);
                            bool held = false;
                            // Only a ratio the foot already HAS is held: with the foot still parked at
                            // zero, a goalpost moving out simply keeps it parked (the cap is the point).
                            if (r > 0.02f && owedF > 0.0f && owedF > fdWF * keep + 0.5f) { fdWF = owedF / keep; held = true; }
                            if (r > 0.02f && owedR > 0.0f && owedR > fdWR * keep + 0.5f) { fdWR = owedR / keep; held = true; }
                            if (held) fdHolds++;
                        }
                        if (fdDef) {
                            if (fdWF > 3600.0f) fdWF = 3600.0f;
                            if (fdWR > 3600.0f) fdWR = 3600.0f;
                            *(float*)((uint8_t*)fdDef + DEF_FLIP_PRECATCH_ANGLE) = fdWF;
                            *(float*)((uint8_t*)fdDef + DEF_ROT_PRECATCH_ANGLE)  = fdWR;
                            fdFrames++;
                        }
                    }
                }
            }
            // (The lockstep write of the rendered roll that used to sit here -- "the foot levels the
            // board" -- is gone: the flip update re-stamps that field every frame, so it measured
            // as WRITE NOT STICKING every time. The over-rotation drive above does the job through
            // the rate, which is the one lever that moves the deck.)
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
            // A DECK ON ITS EDGE IS NOT A WEDGE. In a primo the feet legitimately hold catch types
            // while grounded, and the orient state reads 0 -- so the darkslide exemption, which
            // tests the STATE, does not cover it. The pose looked stuck, and a second in this
            // re-seated both feet onto the griptape and twisted them (field: "the feet look correct
            // on the side of the board, after a few seconds they connect to the grip tape").
            // This is the same mistake the grind round made, one level down: the types are a
            // legitimate pose there too. The discriminator is the BOARD, not the feet -- a catch
            // wedged after a landing has the deck flat, a primo has it on its edge -- and it is
            // measured from the flipper's world quat, exactly as the darkslide zone is.
            bool onEdge = false;
            __try {
                void* board7   = twkP(skater, SK_BOARD);
                void* flipper7 = board7 ? twkP(board7, BOARD_FLIPPER) : nullptr;
                if (flipper7) {
                    const float qx7 = twkF(flipper7, COMP_CTW_QUAT);
                    const float qy7 = twkF(flipper7, COMP_CTW_QUAT + 4);
                    if (qx7 > -100000.0f && qy7 > -100000.0f) {
                        float upZ7 = 1.0f - 2.0f * (qx7 * qx7 + qy7 * qy7);
                        if (upZ7 < -1.0f) upZ7 = -1.0f; else if (upZ7 > 1.0f) upZ7 = 1.0f;
                        // neither grip-up nor grip-down: the deck is standing on its edge
                        onEdge = fabsf(upZ7) < g_unstickFlat;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { onEdge = false; }  // unreadable: as before
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
            const bool suspicious = FootPlace_Grounded() && !darkslide && !onEdge && catchTypes;
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
