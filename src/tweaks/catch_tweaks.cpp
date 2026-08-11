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
//  * THE WINDOW. The feet can only take the board within `BoardFlipPreCatchAngle` /
//    `BoardRotationPreCatchAngle` of its target (USkateboardMovementComponent::UpdateFeetCatchInfo).
//    Both ship 60 deg on every def -- at a measured ~1960 deg/s flip that is a ~31 ms window, about
//    two frames. Widened by CatchWindowMult while in manual, restored the moment it is not.
//
//  * THE EATEN INPUT. While `_shouldCancelSingleStickInputForDarkSlide` is nonzero,
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
    MC_BOARD_FLIP_TARGET    = 0x778,   // _boardFlipTargetAngle
    MC_BOARD_FLIP_CUR       = 0x77c,   // _boardFlipCurrentAngle
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
    DEF_ROT_PRECATCH_ANGLE  = 0x25c,   // UFlipTrickDefinition::BoardRotationPreCatchAngle (default 60)
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
    TwkIniSetInt(buf, cap, "CatchStopsFlip",      g_stopFlip);
    TwkIniSetInt(buf, cap, "CatchStopFlipDeg",    g_stopFlipDeg);
    TwkIniSetInt(buf, cap, "CatchSnapMs",         g_snapMs);
    TwkIniSetInt(buf, cap, "CatchSnapMaxDeg",     g_snapMaxDeg);
    TwkIniSetInt(buf, cap, "CatchSnapMaxBoost",   g_snapMaxBoost);
    TwkIniSetInt(buf, cap, "CatchFlipAxis",       g_flipAxis);
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
    if (g_nOrientSave > 0)
        TwkLog("[catch] catch-orient board offsets captured on %d settings blocks (%d definitions) "
               "-- originals held for exact restore", g_nOrientSave, n);
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
// ⚠️ The previous version used the deck's up-vector against world up (`1 - 2(qx^2+qy^2)`), which is a
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
    void* r = nullptr;
    __try { r = ((CatchDefaultFn)g_origCatchDef)(self, dt, frontStick, backStick, a5, a6, a7); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[catch] caught fatal in CheckForCatchOrient_Default -> recovered");
        return nullptr;
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
    if (!g_catchBeatsDS && !g_catchDiag) { TwkLog("[catch] CatchBeatsDarkslide=0 + CatchDiag=0 -- CheckForCatchOrient_Default NOT hooked"); return; }
    g_startCatchDef = TwkScanExe(SIG_CATCH_DEFAULT);
    if (!g_startCatchDef) { TwkLog("[catch] CheckForCatchOrient_Default sig NOT FOUND -- dark-slide fix off (game updated?)"); g_catchBeatsDS = 0; }
    else if (MH_CreateHook(g_startCatchDef, (void*)&hkCatchDefault, &g_origCatchDef) != MH_OK ||
             MH_EnableHook(g_startCatchDef) != MH_OK) {
        TwkLog("[catch] CheckForCatchOrient_Default hook failed -- dark-slide fix off");
        g_startCatchDef = nullptr; g_catchBeatsDS = 0;
    } else TwkLog("[catch] hooked CheckForCatchOrient_Default @ %p -- catch input beats dark slides", g_startCatchDef);
}

bool CatchTweaks_Enabled() { return g_catchFix != 0; }
void CatchTweaks_SetEnabled(bool on) { g_catchFix = on ? 1 : 0; TwkMarkDirty(); }
// `g_manualMode` is deliberately NOT reset. It is not a preference but the ECatchMode value measured
// for this install off the "[catch] ECatchMode=" log line, and its default -1 means "unknown, widen
// nothing" -- resetting it would silently switch the whole catch feature off.
void CatchTweaks_ResetDefaults() {
    g_catchFix = 1; g_catchMult = 2.0f; g_catchBeatsDS = 1; g_dsAngleDeg = 60; g_catchDiag = 0;
    g_anyRev = 1; g_anyRevDeg = 60;
    g_stopFlip = 1; g_stopFlipDeg = 168; g_snapMs = 90;
    g_snapMaxDeg = 200; g_snapMaxBoost = 3; g_flipAxis = 0;
    g_boneScale = 100; g_boneAdd[0] = g_boneAdd[1] = g_boneAdd[2] = 0;
    TwkMarkDirty();
}
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

    if (state != 0 && !inCatch) {
        inCatch = true; sawL = sawR = false; peakL = peakR = 0.0f; frames = 0; quiet = 0;
        angAt = ang;
        curAt = twkF(comp, MC_BOARD_FLIP_CUR);
        tgtAt = twkF(comp, MC_BOARD_FLIP_TARGET);
    }
    if (!inCatch) return;

    ++frames;
    const float rl = twkF(comp, MC_LFOOT_CATCH + FCFI_RATIO);
    const float rr = twkF(comp, MC_RFOOT_CATCH + FCFI_RATIO);
    if (rl > peakL) peakL = rl;
    if (rr > peakR) peakR = rr;
    if (twkB(anim, AN_HAS_LFOOT_CATCH) == 1) sawL = true;
    if (twkB(anim, AN_HAS_RFOOT_CATCH) == 1) sawR = true;

    // The catch state drops back to 0 the moment it ends; a few frames of grace keep a one-frame
    // flicker from splitting one catch into two log lines.
    if (state == 0) ++quiet; else quiet = 0;
    if (quiet < 4 && frames < 600) return;

    inCatch = false;
    TwkLog("[feet] catch over %d frames: L orient=%s peak ratio %.2f | R orient=%s peak ratio %.2f "
           "| deck %.0f -> %.0f deg | flip angle %.0f -> %.0f (target %.0f)%s",
           frames, sawL ? "YES" : "no ", peakL, sawR ? "YES" : "no ", peakR,
           angAt, ang, curAt, twkF(comp, MC_BOARD_FLIP_CUR), tgtAt,
           (!sawL && !sawR) ? "   <-- NO FOOT CAUGHT" : "");
}

void CatchTweaks_PumpFrame() {
    // ⚠️ The FIX (g_stopFlip) lives in here alongside the diagnostic trace, so this must not early-out
    // on the trace flag -- doing so silently disabled the fix the moment logging was turned off for
    // release. Each part checks its own switch below.
    if (!g_flipTrace && !g_stopFlip) return;
    __try {
        void* skater = g_lastSkater;
        if (!skater) return;
        const float ang = BoardGrip(skater);
        if (ang < 0.0f) return;
        static long  lastSerial = -1;
        static bool  watching = false;
        static float prev = 0.0f, travel = 0.0f;
        static int   still = 0, frames = 0;
        // ---- the fix: a registered catch ends the flip at the first grip-up ----------------------
        // `_boardFlipRate` is zeroed rather than the angle being written: the deck is already AT the
        // orientation a completed flip ends on, so removing the rate leaves it exactly there and lets
        // every other system (landing, pitch, the catch's own alignment) carry on untouched.
        if (g_stopFlip) {
            void* comp = CatchLevel_MovementComponent();
            const int catchState = twkB(skater, SK_CATCH_ORIENT_STATE);
            static bool  armed = false;
            static float prevAng = -1.0f;
            const float delta = (prevAng >= 0.0f) ? (ang - prevAng) : 0.0f;
            if (catchState != 0 && !armed) {
                armed = true;
                // ⚠️ If the deck is ALREADY flat when the catch registers, do nothing at all. There is
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
                    // ⚠️ TWO LIMITS, both learned from a trick that visibly bugged out. A catch can
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
            static bool endedThisCatch = false;
            if (catchState == 0) endedThisCatch = false;
            if (g_anyRev && comp && catchState != 0 && !endedThisCatch &&
                ang >= (180.0f - (float)g_anyRevDeg)) {
                const float tgt = twkF(comp, MC_BOARD_FLIP_TARGET);
                const float cur = twkF(comp, MC_BOARD_FLIP_CUR);
                const float toFlat = 180.0f - ang;          // deck degrees still to roll to grip-up
                const float dir = (tgt < cur) ? -1.0f : 1.0f;   // the way the flip is already going
                const float aim = cur + dir * toFlat;
                if (fabsf(aim - tgt) > 1.0f) {
                    *(float*)((uint8_t*)comp + MC_BOARD_FLIP_TARGET) = aim;
                    endedThisCatch = true;
                    TwkLog("[catch] caught at %.0f deg -- aiming the flip at flat, %.0f deg away "
                           "(target %.0f -> %.0f, game wanted %.0f more)",
                           ang, toFlat, tgt, aim, fabsf(tgt) - fabsf(cur));
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
