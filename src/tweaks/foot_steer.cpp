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
// FOOT STEERING -- the thumbsticks offset the foot IK targets, so the feet can be tweaked and boned
// through an air instead of only played back.
//
// AIR ONLY, by field verdict. Grinds and manuals were armed in the first cut and tried: steering
// there is not worth having, so the states are gone rather than left behind a switch.
//
// THE FOOT TWIST, and why the first attempt at it failed. The socket ROTATION is taken from the
// deck's own foot-anchor sockets, so it follows the board: through a kickflip it sweeps through a
// full revolution. Adding a fixed euler delta to it therefore meant a twist whose DIRECTION rotated
// with the deck -- the same defect the position offset had when it was built on the flipping deck's
// basis, and it read as the foot buzzing rather than tilting. The twist is now a rotation about a
// STABLE axis (the same frame the position offset uses), composed with the socket's orientation as
// quaternions, so "toe forward" means the same thing at every point in the flip.
// Adding degrees to a rotator whose own frame is moving is never a rotation you can reason about.
//
// ---- WHAT THIS IS AND IS NOT ----------------------------------------------------------------
// The feet do not drive the board. Session's deck is a state machine (_boardFlipRate,
// _boardRotationRate, _boardFlipTargetAngle, all driven from the trick definition chosen at pop
// time); there is no shoe-to-deck contact for a foot to push against, so moving the foot mesh moves
// nothing else. This is expression, not simulation.
//
// ---- THE SEAM ---------------------------------------------------------------------------------
// LeftFootSocketLocation (+0x404) / RightFootSocketLocation (+0x41c) on USkaterAnimInstance, written
// from foot_place's UpdateFootAnchors POST hook. That is the only phase where a write to these
// survives into the rendered pose -- the same fields written from the input tick move the feet by
// exactly nothing, and *FootBoneLocationAdjust (+0x434/+0x440) is a measured dead end for the same
// reason. This module therefore hooks NOTHING: foot_place owns that detour and calls in here, and
// scoop_speed owns InputHandler::Tick and feeds us the stick. Only one MinHook detour may exist per
// address, and a second scan for an already-hooked function silently fails.
//
// ---- OFFSET, NEVER REPLACE --------------------------------------------------------------------
// The value written is a DELTA on whatever the animation just computed. Holding the feet at an
// absolute target was built, shipped and rejected in foot placement: the animation's own micro-motion
// IS the desired motion, so replacing a position fights it every frame while offsetting preserves it.
// A stick-driven absolute target would repeat that exactly.
//
// ---- SLOW STEER vs FAST FLICK -----------------------------------------------------------------
// Both sticks are already load-bearing in the air: a flick is the catch, and its window is about two
// frames wide. Two layers keep the two apart. The RATE LIMITER is the discriminator -- the steer
// chases the stick at a bounded speed, so a 35-120 ms catch flick can only move it a fraction while a
// deliberate push reaches full travel, and a borderline gesture degrades to a partial tweak rather
// than picking the wrong branch. The FLICK VETO is the guarantee -- a measured flick zeroes that
// stick's steer and holds it there through a refractory window.
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "foot_steer.h"
#include "scoop_speed.h"
#include "ui/menu_ext.h"
#include "tweaks_mod.h"
#include "grind_pop.h"   // GrindPop_NameOfFName -- it owns the FName::ToString address
#include <cstring>
#include <cmath>

// ------------------------------------------------------------------ measured offsets
// USkaterAnimInstance, all PDB-exact. The ones foot_place also carries are named identically there.
enum {
    AN_ON_BOARD      = 0x300,   // IsOnBoard -- hard gate, never steer while walking
    AN_IS_SWITCH     = 0x303,   // IsSkatingSwitch (plain bool, no asset gate)
    AN_IS_GOOFY      = 0x304,   // IsSkatingGoofy -- see the per-foot veto for why it matters
    // The game's built-in boned ollie is NOT the stance system (EFootPositionType +0x305 reads a
    // constant through one) and NOT HipOffset (+0x3dc reads (0,0,0) throughout). Both were measured
    // and both are dead ends; the probes for them are gone. What a bone actually does is push the
    // BOARD forward relative to the skater -- up to ~36 cm, decaying back to 0 on release, against
    // +/-2 cm on a plain ollie -- while the character mesh does not move at all. The writer of that
    // displacement is not yet named.
    AN_TRICK_PENDING = 0x310,
    AN_CATCH_ST      = 0x312,   // CatchOrientState
    AN_CATCH_L       = 0x313,   // HasLeftFootCatchOrient
    AN_CATCH_R       = 0x314,   // HasRightFootCatchOrient
    AN_L_ALPHA       = 0x3fc,   AN_R_ALPHA  = 0x400,   // Left/RightFootIKAlpha
    AN_L_SOCK_LOC    = 0x404,   AN_R_SOCK_LOC = 0x41c, // the fields foot_place writes for us
    // The matching FRotators (Pitch +0, Yaw +4, Roll +8). Nothing else writes these on the local
    // player, so this module owns them and writes them itself.
    AN_L_SOCK_ROT    = 0x410,   AN_R_SOCK_ROT = 0x428,
    AN_FLIPPING      = 0x495,   AN_ROTATING = 0x496,
    // Asset-gated exactly as in foot placement: the crank fields are only meaningful while
    // CrankLoopBlendSpace is non-null, and read stale otherwise.
    AN_IS_CRANKING   = 0x497,   AN_CRANK_BS = 0x4a8,
    AN_GROUNDED      = 0x5fa,   AN_FALLING  = 0x5fc,
    // The game's foot-to-deck auto-adjust, read for the log only. Switching it off while steering was
    // tried and made foot placement worse -- it is the thing that sits the shoe on the griptape.
    AN_L_AUTO_OFF    = 0x718,   AN_R_AUTO_OFF = 0x724,   // _last{Left,Right}FootAutoOffset (FVector)
    AN_L_TGT         = 0x700,   AN_R_TGT      = 0x70c,   // _last{Left,Right}FootTargetLocation
    AN_SKATER        = 0x608,   // _skater -- the back-pointer, so no mesh chain has to be re-walked
    // ASkaterCharacterBase, then the board
    SK_MESH          = 0x280,   SK_BOARD    = 0x568,
    // The flipper is the component that VISUALLY FLIPS. Building the steer basis on it makes a
    // held stick trace a circle through a kickflip, because the basis rolls with the deck. It is
    // kept only as frame 1, for comparison.
    BOARD_FLIPPER    = 0x4e8,
    ACTOR_ROOT       = 0x130,   // AActor::RootComponent -- its ComponentToWorld carries no flip
    SK_CUR_TRICK     = 0x590,   // _currentFlipTrickDef -- what the pop actually selected
    TRICKDEF_NAME    = 0x30,    // UFlipTrickDefinition::Name (FName) -- "Ollie", "Kickflip", ...
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
// Ships OFF. The steering is only as good as its calibration, and the numbers below are seeds until
// one round of the probe reports what the sticks and the foot IK actually do outside riding.
static int   g_on         = 0;      // FootSteer
static int   g_probe      = 1;      // FootSteerProbe -- sample + dump one block per armed stretch
static int   g_probeAxis  = 0;      // FootSteerProbeAxis -- 0..2 = +X/+Y/+Z, 3..5 = -X/-Y/-Z
static int   g_probeCm    = 0;      // FootSteerProbeCm -- non-zero pushes BOTH feet a fixed distance
                                    // along that axis while armed, ignoring the sticks. This is what
                                    // names the axes and finds where the leg locks; a derivation
                                    // cannot, because the socket's space is not decided anywhere.
// FootSteerReachCm -- how far full stick deflection moves the foot. The real ceiling is physical:
// past the leg's reach the two-bone IK saturates and the foot chatters at the limit rather than
// travelling further. Where that sits was never measured, so the range here is deliberately generous
// and the judgement is left to the eye. The hard clamp only exists to keep a typo out of orbit.
static float g_reachCm    = 40.0f;
static float g_responseMs = 250.0f; // FootSteerResponseMs -- stick to full steer; the discriminator
static float g_returnMs   = 150.0f; // FootSteerReturnMs -- back to neutral, deliberately quicker
static float g_deadzone   = 0.0f;  // FootSteerDeadzone (pct) -- radial, rescaled so 1.0 still reaches
static float g_flickVeto  = 10.0f;  // FootSteerFlickVeto (tenths) -- stick units/s that count as a
                                    // flick. flip_speed maps real flicks over 15..50 on this measure;
                                    // a deliberate steer push runs 1-5. Set from probe data.
static int   g_blankMs    = 250;    // FootSteerBlankMs -- refractory after a flick or a catch
static int   g_catchVeto  = 1;      // FootSteerCatchVeto -- a registered catch is what plants the feet
                                    // on the deck, so steering yields to it. The probe logs how long
                                    // the catch flags stay set, which is what says whether this is
                                    // right for a whole air or only for the catch itself.
// FootSteerFrame -- WHAT the offset directions are measured against. The basis must not carry the
// flip: on the flipping deck a held stick traces a circle through a kickflip, because the direction
// it means rolls with the board.
//   0 = the anim instance's own axes (no transform at all)
//   1 = the flipping deck (this is the circle; kept for comparison)
//   2 = the board's ROOT -- deck heading with the flip stripped, if the flip lives in the flipper
//   3 = the skater's ROOT (the capsule) -- follows only where you face, never flips or shoves
// FIXED AT 0 (the leg rig). The other frames were A/B knobs during development; 0 is the one
// that ships, so the slider is gone. The ini key still reads, for a fallback.
static int   g_frame      = 0;
// Which basis axis each stick component drives: 0..2 = +X/+Y/+Z, 3..5 = -X/-Y/-Z. Defaults follow
// UE's convention for a character root (X forward, Y right), so stick-up pushes the foot forward and
// stick-right pushes it right. Live, because whether that convention holds here is a thing to look
// at rather than to argue about.
static int   g_axisX      = 1;      // FootSteerAxisX -- stick X drives +Y (right)
static int   g_axisY      = 0;      // FootSteerAxisY -- stick Y drives +X (forward)
// The TWIST: push the stick up and the toe swings forward, pull down and it swings back. Driven by
// the stick's Y only, off the same filtered steer as the position, and applied as a rotation about
// a stable axis in the SAME frame the position uses (0..5, defaults to the skater's right, so the
// twist is toe-up / toe-down). 0 degrees = position only.
static float g_twistDeg   = 5.0f;   // FootSteerTwistDeg -- a little goes a long way here
static int   g_twistAxis  = 4;      // FootSteerTwistAxis -- -Y of the chosen frame (measured, the
                                    // +Y guess tipped the foot the wrong way)
// The game switches foot IK OFF during parts of an air (alpha 0, and the sockets zeroed with it) --
// measured, not assumed. An offset written into a socket the graph is ignoring does nothing, and
// comes back the instant the alpha does, which reads as the foot fighting you. Following the alpha
// makes this module fade exactly as the game's own IK fades.
// Riding switch turns you around relative to the board, so the axes the offsets are built on no
// longer match your own sense of forward and the control reads backwards. Bit 1 = flip the stick's
// X contribution, bit 2 = flip its Y (and the twist with it); 3 = both, i.e. the whole control
// rotated 180 like you are. Blended, not switched: stance can change mid-air off a half-cab, and a
// sign that flips on a frame boundary would snap the foot -- the same lesson foot placement learned
// for its own switch values.
static int   g_switchInv  = 3;      // FootSteerSwitchInvert
// "Has this air flipped or rotated" -- latched, cleared on the ground. An ollie of ANY kind
// (regular/switch/fakie/nollie, from any pocket) never sets it, and surrenders both feet.
static int   g_airTrick   = 0;
// The trick definition last seen, and what its NAME said. Cached per definition pointer so
// the FName is resolved once per trick, not per frame.
static void* g_lastTrickDef = nullptr;
static int   g_defIsOllie   = 0;
static int   g_defNamed     = 0;
static float g_swBlend    = 0.0f;   // 0 = regular, 1 = switch
static int   g_followIK   = 1;      // FootSteerFollowIK
static const float kAlphaMin = 0.02f;   // below this the socket is meaningless -- do not write at all

// Own kill switch. A sub-feature must never fault onto the module that calls it -- a fault here
// pauses steering and leaves foot placement running.
static volatile LONG g_ok     = 1;
static volatile LONG g_faults = 0;
// F1 readout (game thread writes, render thread reads)
static volatile float g_uiEase = 0, g_uiLx = 0, g_uiLy = 0, g_uiRx = 0, g_uiRy = 0;
static volatile float g_uiWL = 0, g_uiWR = 0;   // the game's live foot IK weight, per foot
static volatile LONG  g_uiArmed = 0, g_uiBlanked = 0, g_uiCalls = 0;

void FootSteer_ReadConfig(const char* buf) {
    g_on         = TwkIniInt(buf, "FootSteer", 0);
    g_probe      = TwkIniInt(buf, "FootSteerProbe", 1);
    g_probeAxis  = TwkIniInt(buf, "FootSteerProbeAxis", 0);
    g_probeCm    = TwkIniInt(buf, "FootSteerProbeCm", 0);
    g_reachCm    = (float)TwkIniInt(buf, "FootSteerReachCm", 40);
    g_responseMs = (float)TwkIniInt(buf, "FootSteerResponseMs", 250);
    g_returnMs   = (float)TwkIniInt(buf, "FootSteerReturnMs", 150);
    g_deadzone   = (float)TwkIniInt(buf, "FootSteerDeadzone", 0) / 100.0f;
    g_flickVeto  = (float)TwkIniInt(buf, "FootSteerFlickVeto", 100) / 10.0f;
    g_blankMs    = TwkIniInt(buf, "FootSteerBlankMs", 250);
    g_catchVeto  = TwkIniInt(buf, "FootSteerCatchVeto", 1);
    g_frame      = TwkIniInt(buf, "FootSteerFrame", 0);
    g_axisX      = TwkIniInt(buf, "FootSteerAxisX", 1);
    g_axisY      = TwkIniInt(buf, "FootSteerAxisY", 0);
    g_twistDeg   = (float)TwkIniInt(buf, "FootSteerTwistDeg", 5);
    g_twistAxis  = TwkIniInt(buf, "FootSteerTwistAxis", 4);
    g_switchInv  = TwkIniInt(buf, "FootSteerSwitchInvert", 3);
    if (g_switchInv < 0 || g_switchInv > 3) g_switchInv = 3;
    g_followIK   = TwkIniInt(buf, "FootSteerFollowIK", 1);
    // Divide-by-zero and reach-nothing guards, before anything can use the values.
    if (g_deadzone < 0.0f)   g_deadzone = 0.0f;   else if (g_deadzone > 0.90f) g_deadzone = 0.90f;
    if (g_reachCm < 0.0f) g_reachCm = 0.0f; else if (g_reachCm > 200.0f) g_reachCm = 200.0f;
    if (g_responseMs < 20.0f) g_responseMs = 20.0f;
    if (g_returnMs   < 20.0f) g_returnMs   = 20.0f;
    if (g_probeAxis < 0 || g_probeAxis > 5) g_probeAxis = 0;
    if (g_frame < 0 || g_frame > 3) g_frame = 3;
    if (g_axisX < 0 || g_axisX > 5) g_axisX = 1;
    if (g_axisY < 0 || g_axisY > 5) g_axisY = 0;
    if (g_twistAxis < 0 || g_twistAxis > 5) g_twistAxis = 4;
    if (g_twistDeg < 0.0f) g_twistDeg = 0.0f; else if (g_twistDeg > 90.0f) g_twistDeg = 90.0f;
    // Every parsed key echoed: a value disagreeing with the ini has to be visible before any of the
    // behaviour built on it is worth interpreting.
    TwkLog("[steer] config: FootSteer=%d Probe=%d ProbeAxis=%d ProbeCm=%d ReachCm=%.0f "
           "ResponseMs=%.0f ReturnMs=%.0f Deadzone=%.2f FlickVeto=%.1f BlankMs=%d CatchVeto=%d "
           "Frame=%d AxisX=%d AxisY=%d TwistDeg=%.0f TwistAxis=%d SwitchInvert=%d FollowIK=%d "
           "(air only)",
           g_on, g_probe, g_probeAxis, g_probeCm, g_reachCm, g_responseMs, g_returnMs,
           g_deadzone, g_flickVeto, g_blankMs, g_catchVeto, g_frame, g_axisX, g_axisY,
           g_twistDeg, g_twistAxis, g_switchInv, g_followIK);
}

void FootSteer_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "FootSteer",            g_on);
    TwkIniSetInt(buf, cap, "FootSteerProbe",       g_probe);
    TwkIniSetInt(buf, cap, "FootSteerProbeAxis",   g_probeAxis);
    TwkIniSetInt(buf, cap, "FootSteerProbeCm",     g_probeCm);
    TwkIniSetInt(buf, cap, "FootSteerReachCm",     (int)g_reachCm);
    TwkIniSetInt(buf, cap, "FootSteerResponseMs",  (int)g_responseMs);
    TwkIniSetInt(buf, cap, "FootSteerReturnMs",    (int)g_returnMs);
    TwkIniSetInt(buf, cap, "FootSteerDeadzone",    (int)(g_deadzone * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "FootSteerFlickVeto",   (int)(g_flickVeto * 10.0f + 0.5f));
    TwkIniSetInt(buf, cap, "FootSteerBlankMs",     g_blankMs);
    TwkIniSetInt(buf, cap, "FootSteerCatchVeto",   g_catchVeto);
    TwkIniSetInt(buf, cap, "FootSteerFrame",       g_frame);
    TwkIniSetInt(buf, cap, "FootSteerAxisX",       g_axisX);
    TwkIniSetInt(buf, cap, "FootSteerAxisY",       g_axisY);
    TwkIniSetInt(buf, cap, "FootSteerTwistDeg",    (int)g_twistDeg);
    TwkIniSetInt(buf, cap, "FootSteerTwistAxis",   g_twistAxis);
    TwkIniSetInt(buf, cap, "FootSteerSwitchInvert", g_switchInv);
    TwkIniSetInt(buf, cap, "FootSteerFollowIK",    g_followIK);
}

// ------------------------------------------------------------------ helpers (above every user)
static double nowSeconds() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
// 0..2 = +X/+Y/+Z, 3..5 = -X/-Y/-Z. Which of them points along the deck is a question for the probe,
// not for a derivation -- hence the encoding rather than a hard-coded pair.
static void unitAxis(int code, float v[3]) {
    v[0] = v[1] = v[2] = 0.0f;
    const int i = code % 3;
    v[i] = (code < 3) ? 1.0f : -1.0f;
}

// ------------------------------------------------------------------ per-stick filter state
struct Foot {
    float  s[2];        // the filtered steer, -1..1
    double blankUntil;  // flick/catch refractory
};
static Foot   g_footL = {}, g_footR = {};
static float  g_ease = 0.0f;        // armed ramp; also what eases the offset out on a landing
static double g_lastCall = 0.0;     // liveness: proves the anim hook is reaching us
// What was last written into each foot's socket rotator, and the twist that was inside it.
static float  g_rotWrote[2][3] = {}, g_rotTwist[2][4] = {};
static bool   g_rotHeld[2] = { false, false };

// ------------------------------------------------------------------ the probe buffer
// Buffered, never logged per frame: OvLog-style blocking flushes inside a game-thread hook inflate
// the very DeltaTime the samples are timed against.
struct Sample {
    float t;
    int   st;           // bit0 air, bit1 catch(any), bit2 crank, bit3 switch
    // The three catch indications SEPARATELY. The veto tests only the per-FOOT pair; if a stance
    // sets the state byte without them, that is exactly why it does not fire there.
    int   cS, cL, cR;
    float aL, aR;       // foot IK alphas -- 0 means the graph is ignoring the socket entirely
    float sL[3], sR[3];
    float lx, ly, rx, ry;
    float fL, fR;       // measured flick speed, -1 = no measurement
    // The two ways the game can fight an offset, both scalar so they fit on one line:
    float autoL, autoR; // its foot-to-deck auto-adjust, whose sweep starts from where WE put the foot
    float gapL, gapR;   // how far the IK's own target sits from the socket it was given
    float rotL[3];      // the LEFT socket's rotator: if this sweeps through a flip, the foot's
                        // orientation target is riding the deck, which is why a fixed euler delta
                        // on it cannot mean a fixed twist
    // Both measured in the SKATER CAPSULE's frame, which neither the mesh nor the deck can drag.
    // The socket moving in mesh space cannot say whether the DECK went forward or the BODY went
    // back; these two can, because they move independently of each other.
    float deckX;        // the flipping deck component along the capsule's forward axis
    float meshX;        // the character mesh along the same axis
    // What the OLLIE GATE sees. A pocket ollie still steers, so one of these must be reading like a
    // trick -- printed rather than reasoned about.
    unsigned char fl, ro, at, hasDef;
};
static const int kSamples = 256;
static Sample g_buf[kSamples];
static int    g_nBuf = 0;
static bool   g_bufFull = false;
static double g_armT0 = 0.0;

static void dumpSamples() {
    if (g_nBuf <= 0) return;
    // Every statistic here is over AIRBORNE samples only. Including the grounded ease-out tail
    // dragged the alpha minimum to 0 on every single air, so the summary said "0.00..1.00" whether
    // the IK cut out mid-flight or not -- it could not answer the one question it existed for.
    float aLmin = 9e9f, aLmax = -9e9f, aRmin = 9e9f, aRmax = -9e9f;
    float auMin = 9e9f, auMax = -9e9f, gapMax = 0.0f;
    int nAir = 0, nCatch = 0, nCrank = 0, nAlpha0 = 0;
    // Counted SEPARATELY and over airborne samples only: the veto reads the per-foot pair, so a
    // stance where only the state byte is set is a stance where the veto never fires.
    int nSwitch = 0, nCatchS = 0, nCatchL = 0, nCatchR = 0;
    float deckMin = 9e9f, deckMax = -9e9f;   // the board's travel relative to you: a bone shows here
    for (int i = 0; i < g_nBuf; i++) {
        const Sample& s = g_buf[i];
        if (s.st & 2) nCatch++;  if (s.st & 4) nCrank++;
        if (!(s.st & 1)) continue;
        nAir++;
        if (s.st & 8) nSwitch++;
        if (s.cS > 0) nCatchS++;  if (s.cL > 0) nCatchL++;  if (s.cR > 0) nCatchR++;
        if (s.aL < aLmin) aLmin = s.aL;  if (s.aL > aLmax) aLmax = s.aL;
        if (s.aR < aRmin) aRmin = s.aR;  if (s.aR > aRmax) aRmax = s.aR;
        if (s.aL <= 0.0f && s.aR <= 0.0f) nAlpha0++;
        if (s.autoL < auMin) auMin = s.autoL;  if (s.autoL > auMax) auMax = s.autoL;
        if (s.autoR < auMin) auMin = s.autoR;  if (s.autoR > auMax) auMax = s.autoR;
        if (s.gapL > gapMax) gapMax = s.gapL;
        if (s.gapR > gapMax) gapMax = s.gapR;
        if (s.deckX > -9000.0f) {
            if (s.deckX < deckMin) deckMin = s.deckX;
            if (s.deckX > deckMax) deckMax = s.deckX;
        }
    }
    if (deckMin > deckMax) { deckMin = deckMax = 0.0f; }
    if (nAir == 0) { aLmin = aLmax = aRmin = aRmax = auMin = auMax = 0.0f; }
    TwkLog("[steer] air: %d samples over %.2f s%s | AIRBORNE=%d of them | foot IK alpha L %.2f..%.2f "
           "R %.2f..%.2f (%d airborne samples with NO ik) | auto-adjust Z %.2f..%.2f cm | worst "
           "target gap %.1f cm | crank=%d | AIRBORNE catch: state=%d Lfoot=%d Rfoot=%d%s | "
           "switch=%d | board %.1f..%.1f cm from you%s",
           g_nBuf, g_buf[g_nBuf - 1].t, g_bufFull ? " (TRUNCATED)" : "", nAir,
           aLmin, aLmax, aRmin, aRmax, nAlpha0, auMin, auMax, gapMax, nCrank,
           nCatchS, nCatchL, nCatchR,
           // The whole question in one tag: the veto only reads the per-foot pair.
           (nCatchS > 0 && nCatchL == 0 && nCatchR == 0) ? "  <-- STATE ONLY, VETO CANNOT FIRE" : "",
           nSwitch, deckMin, deckMax, (deckMax > 8.0f) ? "  <-- THE GAME'S BONE FIRED" : "");
    // A decimated trace rather than everything: the shape over the stretch is what is being read,
    // and 250 lines per trick would bury it.
    const int want = 12;
    int stride = (g_nBuf + want - 1) / want;
    if (stride < 1) stride = 1;
    char fl[16], fr[16];
    for (int i = 0; i < g_nBuf; i += stride) {
        const Sample& s = g_buf[i];
        if (s.fL < 0.0f) strcpy(fl, "-"); else snprintf(fl, sizeof(fl), "%.1f", s.fL);
        if (s.fR < 0.0f) strcpy(fr, "-"); else snprintf(fr, sizeof(fr), "%.1f", s.fR);
        TwkLog("[steer]   t=%.2f %c%c%c%c a(%.2f,%.2f) sockL(%.1f,%.1f,%.1f) sockR(%.1f,%.1f,%.1f) "
               "stickL(%+.2f,%+.2f) stickR(%+.2f,%+.2f) flick(%s,%s) autoZ(%+.2f,%+.2f) "
               "gap(%.1f,%.1f) catch(s=%d,L=%d,R=%d) deckX(%.1f) meshX(%.1f)",
               s.t, (s.st & 1) ? 'A' : '.', (s.st & 2) ? 'C' : '.', (s.st & 4) ? 'K' : '.',
               (s.st & 8) ? 'W' : '.',
               s.aL, s.aR,
               s.sL[0], s.sL[1], s.sL[2], s.sR[0], s.sR[1], s.sR[2],
               s.lx, s.ly, s.rx, s.ry, fl, fr, s.autoL, s.autoR, s.gapL, s.gapR,
               s.cS, s.cL, s.cR, s.deckX, s.meshX);
        TwkLog("[steer]      gate: flip=%d rot=%d airTrick=%d trickDef=%s",
               s.fl, s.ro, s.at, s.hasDef ? "YES" : "null");
    }
    g_nBuf = 0; g_bufFull = false;
}

// ------------------------------------------------------------------ the offset
// One stick -> one filtered steer vector. Left stick drives the LEFT foot and right the RIGHT: the
// sliders follow the FOOT, not front/back, which is what keeps switch stance correct with no stance
// logic (the same rule foot placement settled).
static bool updateFoot(Foot& F, bool rightStick, bool armed, bool catchNow, float dt, double now) {
    float sx = 0.0f, sy = 0.0f;
    const bool have = ScoopSpeed_StickRaw(rightStick, &sx, &sy);
    float tx = 0.0f, ty = 0.0f;
    if (have && armed) {
        const float m = sqrtf(sx * sx + sy * sy);
        if (m > g_deadzone) {
            // Radial deadzone, rescaled so the usable travel still reaches full deflection.
            float k = (m - g_deadzone) / (1.0f - g_deadzone);
            if (k > 1.0f) k = 1.0f;                       // a square-gated stick can read past 1
            tx = sx / m * k; ty = sy / m * k;
        }
    }
    // A measured flick is the catch, not a steer. Zero the target and hold it through a refractory
    // window so the rest of the flick cannot leak in behind the rate limiter.
    float spd = 0.0f, peak = 0.0f;
    if (ScoopSpeed_FlickMeasure(rightStick, 0.20f, &spd, &peak, nullptr) && spd >= g_flickVeto)
        F.blankUntil = now + (double)g_blankMs / 1000.0;
    if (catchNow) F.blankUntil = now + (double)g_blankMs / 1000.0;
    const bool blanked = now < F.blankUntil;
    if (blanked) { tx = 0.0f; ty = 0.0f; }
    // THE DISCRIMINATOR: a bounded chase, applied to the vector so the direction is preserved.
    // Frame-rate independent by construction -- the step is a rate times real elapsed time, and both
    // are properties of the gesture rather than of the sampling.
    const float dx = tx - F.s[0], dy = ty - F.s[1];
    const float d  = sqrtf(dx * dx + dy * dy);
    if (d > 1e-5f) {
        const float curMag = sqrtf(F.s[0] * F.s[0] + F.s[1] * F.s[1]);
        const float tgtMag = sqrtf(tx * tx + ty * ty);
        const float rate = (tgtMag >= curMag) ? (1000.0f / g_responseMs) : (1000.0f / g_returnMs);
        const float step = rate * dt;
        if (d <= step) { F.s[0] = tx; F.s[1] = ty; }
        else           { F.s[0] += dx * (step / d); F.s[1] += dy * (step / d); }
    }
    return blanked;
}

// A 2D steer -> a delta in the anim instance's own space. Frames 1-3 build the direction on some
// other component's basis and bring it back through the mesh, so what a stick direction MEANS does
// not depend on which way a stick turn has left you facing. A delta is pure rotation between the two
// bases -- the translation half of a ComponentToWorld never enters -- which is why this can cross
// spaces that an absolute position could not.
static void buildDelta(void* a, int codeX, int codeY, float x, float y, float scale, float out[3]) {
    float ax[3], ay[3];
    unitAxis(codeX, ax);
    if (codeY >= 0) unitAxis(codeY, ay); else { ay[0] = ay[1] = ay[2] = 0.0f; }
    float v[3] = { ax[0] * x + ay[0] * y, ax[1] * x + ay[1] * y, ax[2] * x + ay[2] * y };
    if (g_frame != 0) {
        void*  skater = twkP(a, AN_SKATER);
        void*  mesh   = skater ? twkP(skater, SK_MESH)  : nullptr;
        void*  board  = (g_frame == 1 || g_frame == 2) ? (skater ? twkP(skater, SK_BOARD) : nullptr)
                                                       : nullptr;
        void*  src    = nullptr;
        if      (g_frame == 1) src = board  ? twkP(board, BOARD_FLIPPER) : nullptr;
        else if (g_frame == 2) src = board  ? twkP(board, ACTOR_ROOT)    : nullptr;
        else                   src = skater ? twkP(skater, ACTOR_ROOT)   : nullptr;
        float  qs[4], qm[4];
        if (TwkCompQuat(src, qs) && TwkCompQuat(mesh, qm)) {
            float w[3];
            TwkQuatRotate(qs, v, w);        // the chosen basis -> world
            TwkQuatInvRotate(qm, w, v);     // world -> the mesh's own space
        } else {
            // Either component unreadable: fall through with the instance-space vector, which is
            // wrong by a rotation and reads as the feet drifting sideways rather than as a failure.
            // Say so once, or the symptom names nothing.
            static bool said = false;
            if (!said) {
                said = true;
                TwkLog("[steer] frame %d basis unreadable (src %p mesh %p) -- steering falls back to "
                       "the anim instance's own axes, so its directions will not match. "
                       "FootSteerFrame=0 makes that the deliberate mode.", g_frame, src, mesh);
            }
        }
    }
    float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (m > 1.0f) { v[0] /= m; v[1] /= m; v[2] /= m; m = 1.0f; }
    out[0] = v[0] * scale; out[1] = v[1] * scale; out[2] = v[2] * scale;
}

// One axis of the chosen frame, brought into the anim instance's own space -- the same conversion
// buildDelta does for the position, so the twist axis and the movement directions agree.
static bool frameAxis(void* a, int code, float out[3]) {
    unitAxis(code, out);
    if (g_frame == 0) return true;
    void* skater = twkP(a, AN_SKATER);
    void* mesh   = skater ? twkP(skater, SK_MESH) : nullptr;
    void* board  = (g_frame == 1 || g_frame == 2) ? (skater ? twkP(skater, SK_BOARD) : nullptr) : nullptr;
    void* src    = nullptr;
    if      (g_frame == 1) src = board  ? twkP(board, BOARD_FLIPPER) : nullptr;
    else if (g_frame == 2) src = board  ? twkP(board, ACTOR_ROOT)    : nullptr;
    else                   src = skater ? twkP(skater, ACTOR_ROOT)   : nullptr;
    float qs[4], qm[4];
    if (!TwkCompQuat(src, qs) || !TwkCompQuat(mesh, qm)) return false;
    float w[3];
    TwkQuatRotate(qs, out, w);
    TwkQuatInvRotate(qm, w, out);
    return true;
}

// Twist one foot about a stable axis by composing quaternions, rather than adding degrees to a
// rotator whose own frame is sweeping round with the deck.
static void applyTwist(void* a, int off, int idx, const float axis[3], float deg) {
    const bool wantZero = !(deg > 0.05f || deg < -0.05f);
    if (wantZero && !g_rotHeld[idx]) return;       // nothing to add, nothing of ours to unwind
    float cur[3] = { twkF(a, off), twkF(a, off + 4), twkF(a, off + 8) };
    if (cur[0] < -1e5f || cur[1] < -1e5f || cur[2] < -1e5f) return;   // sentinel read
    float q[4];
    TwkRotatorToQuat(cur, q);
    // If the value that came back is exactly what we wrote, the animation did not recompute it and
    // our own twist is still in there -- take it out before the next one goes on, or the foot winds
    // a little further every frame.
    if (g_rotHeld[idx] && fabsf(cur[0] - g_rotWrote[idx][0]) < 1e-3f
                       && fabsf(cur[1] - g_rotWrote[idx][1]) < 1e-3f
                       && fabsf(cur[2] - g_rotWrote[idx][2]) < 1e-3f) {
        const float inv[4] = { -g_rotTwist[idx][0], -g_rotTwist[idx][1], -g_rotTwist[idx][2], g_rotTwist[idx][3] };
        float undone[4]; TwkQuatMul(inv, q, undone);
        q[0] = undone[0]; q[1] = undone[1]; q[2] = undone[2]; q[3] = undone[3];
    }
    if (wantZero) {                                 // unwound back to the animation's own value
        float r[3]; TwkQuatToRotator(q, r);
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)a + off + i * 4) = r[i];
        g_rotHeld[idx] = false;
        return;
    }
    float tw[4]; TwkQuatAxisAngle(axis, deg, tw);
    float out[4]; TwkQuatMul(tw, q, out);           // twist applied in the socket's PARENT frame
    float r[3];  TwkQuatToRotator(out, r);
    for (int i = 0; i < 3; i++) {
        *(float*)((uint8_t*)a + off + i * 4) = r[i];
        g_rotWrote[idx][i] = r[i];
    }
    for (int i = 0; i < 4; i++) g_rotTwist[idx][i] = tw[i];
    g_rotHeld[idx] = true;
}

bool FootSteer_AddOffset(void* a, float dt, float outL[3], float outR[3]) {
    if (!g_ok || !a || !outL || !outR) return false;
    if (!(dt > 0.0f) || dt > 0.25f) dt = 1.0f / 60.0f;
    bool wrote = false;
    __try {
        const double now = nowSeconds();
        g_lastCall = now;
        InterlockedIncrement(&g_uiCalls);

        const int onBoard  = twkB(a, AN_ON_BOARD);
        const bool crankBs = twkP(a, AN_CRANK_BS) != nullptr;
        const int cranking = crankBs ? twkB(a, AN_IS_CRANKING) : 0;
        const int pending  = twkB(a, AN_TRICK_PENDING);
        const int falling  = twkB(a, AN_FALLING);
        const int grounded = twkB(a, AN_GROUNDED);
        const int flipping = twkB(a, AN_FLIPPING);
        const int rotating = twkB(a, AN_ROTATING);
        const int catchSt  = twkB(a, AN_CATCH_ST);
        const int catchL   = twkB(a, AN_CATCH_L);
        const int catchR   = twkB(a, AN_CATCH_R);

        const bool air = (falling > 0) || (flipping > 0) || (rotating > 0);
        // The crank is a HELD stick -- a sustained displacement is exactly what the discriminator is
        // built to accept, so the trick-setup crouch is the one guaranteed false positive and is
        // vetoed outright rather than filtered.
        const bool veto  = (cranking > 0) || (pending > 0);
        // ---- PER FOOT, on the LEVEL. Catching with one foot must not surrender the other, so only
        // the foot that actually caught yields (the global CatchOrientState is deliberately not part
        // of this -- it is true for the whole catch and took both feet at once).
        // LEVEL, not edge, and deliberately so. Holding both sticks in an ollie engages a catch
        // orient and keeps these set for the whole hold, which means the feet stay out of it for the
        // whole hold -- correct, because that gesture belongs to the game's own boned ollie. An
        // edge-triggered version was tried so foot control would work through an ollie too; it was
        // rejected in the headset. Foot control is for flip tricks; the bone owns the ollie.
        // ON AN OLLIE THE BONE OWNS BOTH FEET, and the per-foot flags cannot express that.
        // Measured across the four stances: the game flags only ONE foot for an ollie's catch
        // orient, and in SWITCH and FAKIE it is the opposite foot from the stick being held --
        //   NLS nollie: state=53 Rfoot=53, right stick held  -> the held foot was flagged, blanked
        //   SWS ollie : state=63 Lfoot=60 Rfoot=9, RIGHT held -> unflagged, steered 5 cm
        //   FKS ollie : state=62 Lfoot=0 Rfoot=62, LEFT held  -> unflagged, steered 29.5 cm
        // Regular and nollie only looked correct because the flagged foot happened to be the one
        // under the thumb. So: an ollie (no flip, no rotation) with ANY catch indication surrenders
        // BOTH feet. A flip trick keeps the per-foot split, so catching with one foot still leaves
        // the other one yours -- which is the whole point of that split.
        // "HAS THIS AIR FLIPPED OR ROTATED", LATCHED -- not "is an orient showing right now".
        // The previous test was `ollieLike && anyCatch`, i.e. it only surrendered the feet when an
        // ollie ALSO raised a catch indication. POCKET POPS do not: setting the stick diagonally and
        // popping with the other one produces no catch orient, so `anyCatch` stayed false and both
        // feet remained steerable through the ollie (reported). There is no reason to ask the catch
        // system at all -- an ollie is simply an air that never flips or rotates, in every stance and
        // from every pocket.
        // Latched rather than instantaneous because a flip trick has not started flipping yet at the
        // moment of the pop: sampling live would call the first frames of a kickflip an ollie. The
        // latch starts false on takeoff and never becomes true for any ollie, so an ollie surrenders
        // both feet for the WHOLE air.
        // ASK THE TRICK WHAT IT IS. The board's motion flags cannot answer this:
        //     pocket ollie : flip=0 rot=1     shove-it : flip=0 rot=1
        // A pop out of a diagonal pocket imparts yaw, so the game flags an ollie as ROTATING and it
        // reads identically to a shuv. Gating on `flipping || rotating` let pocket ollies steer;
        // gating on flipping alone stopped shuvs steering. Neither is the question being asked.
        // The question is "which trick did I just do", and UFlipTrickDefinition::Name (+0x30) answers
        // it directly -- Ollie / Nollie / Fakie Ollie / Switch Ollie all carry "ollie", and no flip
        // or shove trick does. Resolved once per definition (the resolver caches by FName value).
        // Latched over the air because the definition is chosen at the pop and cleared on landing.
        {
            void* sk  = twkP(a, AN_SKATER);
            void* def = sk ? twkP(sk, SK_CUR_TRICK) : nullptr;
            if (def && def != g_lastTrickDef) {           // only on a NEW definition
                g_lastTrickDef = def;
                char nm[96];
                if (GrindPop_NameOfFName((const uint8_t*)def + TRICKDEF_NAME, nm, sizeof(nm))) {
                    for (char* c = nm; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
                    g_defIsOllie = (strstr(nm, "ollie") != nullptr) ? 1 : 0;
                    g_defNamed   = 1;
                    if (g_probe) TwkLog("[steer] trick def '%s' -> %s", nm,
                                        g_defIsOllie ? "OLLIE (feet surrendered)" : "trick (feet yours)");
                } else {
                    g_defNamed = 0;                      // resolver unavailable: fall back below
                }
            }
        }
        // FALLBACK when the name could not be resolved (FName::ToString sig missing): flipping only.
        // It costs shuvs their steering, which is the safer of the two failures -- the alternative
        // lets every pocket ollie steer.
        if (g_defNamed) g_airTrick = g_defIsOllie ? 0 : 1;
        else if (flipping > 0) g_airTrick = 1;
        else if (grounded > 0) g_airTrick = 0;   // back on the board: re-arm for the next
        const bool bothFeet = (g_airTrick == 0);
        // THE PER-FOOT FLAGS NEED THE SAME STANCE CORRECTION THE CATCH DOES. HasLeft/
        // RightFootCatchOrient are derived from the orient STATE, and catch_tweaks inverts that state
        // in goofy-and-not-switch so the correct foot catches (its own measured truth table). The
        // flags therefore stop naming the physical foot in exactly that stance -- so reading them raw
        // vetoed the FREE foot and left the catching one steerable. Symptom: in goofy you cannot hold
        // the other foot out, because the foot you wanted to hold is the one being surrendered while
        // the catching foot is already planted by the animation.
        // Same predicate as CatchStanceInverts() in catch_tweaks -- if one changes, change both.
        // Read directly rather than via swNow -- that is declared further down, with the stance blend.
        const bool stanceInv = (twkB(a, AN_IS_GOOFY) > 0) && (twkB(a, AN_IS_SWITCH) == 0);
        const int  useCatchL = stanceInv ? catchR : catchL;
        const int  useCatchR = stanceInv ? catchL : catchR;
        const bool catchNowL = (g_catchVeto != 0) && ((useCatchL > 0) || bothFeet);
        const bool catchNowR = (g_catchVeto != 0) && ((useCatchR > 0) || bothFeet);
        // THE STATE GATE AND THE APPLY GATE ARE SEPARATE. Folding the feature toggle into the
        // state test would mean the probe recorded nothing whenever steering was off -- which is
        // exactly the configuration a measurement round runs in.
        const bool stateArmed = onBoard > 0 && !veto && air;
        const bool armed    = stateArmed && (g_on != 0);
        const bool applying = stateArmed && ((g_on != 0) || (g_probeCm != 0));

        // Eased on the same ~0.12 s ramp foot placement uses, and for the same reason: a landing is
        // already busy, and dropping the offset on a frame boundary would pop the foot there.
        float step = dt * 8.0f;
        if (step > 1.0f) step = 1.0f;
        g_ease += ((applying ? 1.0f : 0.0f) - g_ease) * step;
        if (g_ease < 0.001f) g_ease = 0.0f; else if (g_ease > 0.999f) g_ease = 1.0f;

        // Stance, on the SAME ramp and for the same reason: you can land into switch off a half-cab,
        // and a sign that flips on a frame boundary would snap the foot across. Riding the blend
        // through zero instead just returns the foot to neutral and takes it out the other way.
        const int swNow = twkB(a, AN_IS_SWITCH);
        g_swBlend += (((swNow > 0) ? 1.0f : 0.0f) - g_swBlend) * step;
        if (g_swBlend < 0.001f) g_swBlend = 0.0f; else if (g_swBlend > 0.999f) g_swBlend = 1.0f;
        // +1 regular, -1 fully switch, for whichever components are configured to flip.
        const float invX = (g_switchInv & 1) ? (1.0f - 2.0f * g_swBlend) : 1.0f;
        const float invY = (g_switchInv & 2) ? (1.0f - 2.0f * g_swBlend) : 1.0f;
        // Applied HERE, at the point of use -- never to the stored steer, or the rate limiter would
        // see the flip as a step change and chase its own sign.
        const float lX = g_footL.s[0] * invX, lY = g_footL.s[1] * invY;
        const float rX = g_footR.s[0] * invX, rY = g_footR.s[1] * invY;

        const bool blankL = updateFoot(g_footL, false, armed, catchNowL, dt, now);
        const bool blankR = updateFoot(g_footR, true,  armed, catchNowR, dt, now);
        InterlockedExchange(&g_uiBlanked, (blankL || blankR) ? 1 : 0);
        g_uiEase = g_ease;
        g_uiLx = g_footL.s[0]; g_uiLy = g_footL.s[1];
        g_uiRx = g_footR.s[0]; g_uiRy = g_footR.s[1];
        InterlockedExchange(&g_uiArmed, stateArmed ? 1 : 0);

        // ---- FOLLOW THE GAME'S OWN IK WEIGHT. Measured: the alpha drops to 0 partway through an
        // air (and on a catch), and the sockets are zeroed with it. While that holds, the graph is
        // ignoring the socket, so an offset written there does nothing -- and then arrives all at
        // once when the alpha comes back. That is the foot "fighting" you. Weighting by the alpha
        // fades this module exactly as the game fades its own IK, and below kAlphaMin nothing is
        // written at all: a zeroed socket plus our delta is not an offset, it is an absolute target
        // made of garbage.
        float wL = 1.0f, wR = 1.0f;
        if (g_followIK) {
            wL = twkF(a, AN_L_ALPHA);  wR = twkF(a, AN_R_ALPHA);
            // An unreadable alpha must not silently disable steering, so a bad read counts as full.
            if (!(wL >= 0.0f && wL <= 1.0f)) wL = 1.0f;
            if (!(wR >= 0.0f && wR <= 1.0f)) wR = 1.0f;
            if (wL < kAlphaMin) wL = 0.0f;
            if (wR < kAlphaMin) wR = 0.0f;
        }
        g_uiWL = wL; g_uiWR = wR;

        // ---- the probe: a fixed push along one named axis, ignoring the sticks. This is what says
        // which axis is which and how far the leg reaches before it locks; neither is derivable.
        const bool probing = (g_probeCm != 0) && g_ease > 0.0f;
        float dL[3] = { 0, 0, 0 }, dR[3] = { 0, 0, 0 };
        if (probing) {
            if (wL > 0.0f) buildDelta(a, g_probeAxis, -1, 1.0f, 0.0f, (float)g_probeCm * g_ease * wL, dL);
            if (wR > 0.0f) buildDelta(a, g_probeAxis, -1, 1.0f, 0.0f, (float)g_probeCm * g_ease * wR, dR);
        } else if (g_ease > 0.0f) {
            if (wL > 0.0f) buildDelta(a, g_axisX, g_axisY, lX, lY, g_reachCm * g_ease * wL, dL);
            if (wR > 0.0f) buildDelta(a, g_axisX, g_axisY, rX, rY, g_reachCm * g_ease * wR, dR);
        }
        if (dL[0] != 0.0f || dL[1] != 0.0f || dL[2] != 0.0f ||
            dR[0] != 0.0f || dR[1] != 0.0f || dR[2] != 0.0f) {
            outL[0] += dL[0]; outL[1] += dL[1]; outL[2] += dL[2];
            outR[0] += dR[0]; outR[1] += dR[1]; outR[2] += dR[2];
            wrote = true;
        }

        // ---- the twist. Stick Y only: push up and the toe swings forward, pull down and it swings
        // back. Same filtered steer as the position, so the two can never disagree.
        float axis[3] = { 0, 0, 0 };
        const bool haveAxis = (!probing && g_twistDeg > 0.0f && g_ease > 0.0f)
                              ? frameAxis(a, g_twistAxis, axis) : false;
        // The twist takes the same inverted Y, so "push forward, toe forward" still means forward
        // from where YOU are standing when you are riding switch.
        applyTwist(a, AN_L_SOCK_ROT, 0, axis, haveAxis ? lY * g_twistDeg * g_ease * wL : 0.0f);
        applyTwist(a, AN_R_SOCK_ROT, 1, axis, haveAxis ? rY * g_twistDeg * g_ease * wR : 0.0f);
        if (haveAxis && (lY != 0.0f || rY != 0.0f)) wrote = true;


        // ---- sampling
        if (g_probe) {
            const bool sampling = stateArmed || g_ease > 0.0f;
            if (sampling && g_nBuf == 0) g_armT0 = now;
            if (sampling) {
                if (g_nBuf < kSamples) {
                    Sample& s = g_buf[g_nBuf++];
                    s.t  = (float)(now - g_armT0);
                    s.st = (air ? 1 : 0) |
                           (((catchSt > 0) || (catchL > 0) || (catchR > 0)) ? 2 : 0) |
                           ((cranking > 0) ? 4 : 0) |
                           ((swNow > 0) ? 8 : 0);
                    s.cS = catchSt; s.cL = catchL; s.cR = catchR;
                    s.fl = (unsigned char)flipping; s.ro = (unsigned char)rotating;
                    s.at = (unsigned char)g_airTrick;
                    { void* sk = twkP(a, AN_SKATER);
                      s.hasDef = (unsigned char)((sk && twkP(sk, SK_CUR_TRICK)) ? 1 : 0); }
                    s.aL = twkF(a, AN_L_ALPHA);   s.aR = twkF(a, AN_R_ALPHA);
                    for (int i = 0; i < 3; i++) {
                        s.sL[i] = twkF(a, AN_L_SOCK_LOC + i * 4);
                        s.sR[i] = twkF(a, AN_R_SOCK_LOC + i * 4);
                    }
                    if (!ScoopSpeed_StickRaw(false, &s.lx, &s.ly)) { s.lx = s.ly = 0.0f; }
                    if (!ScoopSpeed_StickRaw(true,  &s.rx, &s.ry)) { s.rx = s.ry = 0.0f; }
                    float sp = 0.0f;
                    s.fL = ScoopSpeed_FlickMeasure(false, 0.20f, &sp, nullptr, nullptr) ? sp : -1.0f;
                    s.fR = ScoopSpeed_FlickMeasure(true,  0.20f, &sp, nullptr, nullptr) ? sp : -1.0f;
                    // The auto-adjust's own contribution, and how far the IK's target sits from the
                    // socket it was handed. A target that will not follow the socket is a clamp or a
                    // rejection; an auto-adjust that swings is the sweep re-measuring our own output.
                    s.autoL = twkF(a, AN_L_AUTO_OFF + 8);
                    s.autoR = twkF(a, AN_R_AUTO_OFF + 8);
                    float tg[3];
                    tg[0] = twkF(a, AN_L_TGT) - s.sL[0]; tg[1] = twkF(a, AN_L_TGT + 4) - s.sL[1];
                    tg[2] = twkF(a, AN_L_TGT + 8) - s.sL[2];
                    s.gapL = sqrtf(tg[0] * tg[0] + tg[1] * tg[1] + tg[2] * tg[2]);
                    tg[0] = twkF(a, AN_R_TGT) - s.sR[0]; tg[1] = twkF(a, AN_R_TGT + 4) - s.sR[1];
                    tg[2] = twkF(a, AN_R_TGT + 8) - s.sR[2];
                    s.gapR = sqrtf(tg[0] * tg[0] + tg[1] * tg[1] + tg[2] * tg[2]);
                    if (!(s.gapL >= 0.0f && s.gapL < 1e5f)) s.gapL = -1.0f;
                    if (!(s.gapR >= 0.0f && s.gapR < 1e5f)) s.gapR = -1.0f;
                    for (int i = 0; i < 3; i++) s.rotL[i] = twkF(a, AN_L_SOCK_ROT + i * 4);
                    // The board ACTOR's root component is not a usable position source here -- it
                    // read static while the skater rolled away from it, so the difference just
                    // counted distance travelled. Both readings below come from components already
                    // proven live elsewhere in this module: the deck's flipper and the character
                    // mesh, each against the capsule.
                    s.deckX = s.meshX = -9999.0f;
                    void* sk   = twkP(a, AN_SKATER);
                    void* skRt = sk ? twkP(sk, ACTOR_ROOT) : nullptr;
                    void* mshC = sk ? twkP(sk, SK_MESH) : nullptr;
                    void* bd   = sk ? twkP(sk, SK_BOARD) : nullptr;
                    void* flpC = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
                    float qsk[4];
                    if (skRt && TwkCompQuat(skRt, qsk)) {
                        const float ox = twkF(skRt, 0x1d0), oy = twkF(skRt, 0x1d4), oz = twkF(skRt, 0x1d8);
                        void* const comps[2] = { flpC, mshC };
                        float* const outs[2] = { &s.deckX, &s.meshX };
                        for (int c = 0; c < 2; c++) {
                            if (!comps[c]) continue;
                            const float d[3] = { twkF(comps[c], 0x1d0) - ox,
                                                 twkF(comps[c], 0x1d4) - oy,
                                                 twkF(comps[c], 0x1d8) - oz };
                            if (!(d[0] > -1e4f && d[0] < 1e4f)) continue;
                            float loc[3]; TwkQuatInvRotate(qsk, d, loc);
                            *outs[c] = loc[0];
                        }
                    }
                } else {
                    g_bufFull = true;
                }
            } else if (g_nBuf > 0) {
                dumpSamples();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_ok, 0);
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[steer] caught fatal building the foot offset -> steering paused "
                   "(foot placement is unaffected)");
        return false;
    }
    return wrote;
}

// ------------------------------------------------------------------ liveness
// A silent log is indistinguishable from "the code never ran", so the one thing this module cannot
// check for itself -- that foot_place's hook is reaching it -- says so out loud, once.
void FootSteer_PumpFrame() {
    if (!g_on && !g_probe) return;
    static double firstPump = 0.0;
    static bool   said = false;
    const double now = nowSeconds();
    if (firstPump == 0.0) firstPump = now;
    if (said || g_lastCall > 0.0 || now - firstPump < 5.0) return;
    said = true;
    TwkLog("[steer] no call from the foot-anchor hook after 5 s -- steering cannot apply. Foot "
           "placement owns that hook; check whether its signature resolved.");
}

// ------------------------------------------------------------------ install + menu
void FootSteer_Install() {
    // Nothing to scan or hook: the stick comes from scoop_speed's InputHandler::Tick detour and the
    // write from foot_place's UpdateFootAnchors detour. Announcing the state anyway, because a
    // module that logs nothing when it is off cannot be told from one that failed to load.
    TwkLog("[steer] installed (no hooks of its own; fed by foot_place + scoop_speed) -- steering %s, "
           "probe %s", g_on ? "ON" : "off", g_probe ? "ON" : "off");
    if (g_probeCm != 0)
        TwkLog("[steer] WARNING: FootSteerProbeCm=%d -- both feet get a fixed %d cm push along axis "
               "%d whenever armed, and the sticks are ignored. Set it to 0 for normal play.",
               g_probeCm, g_probeCm, g_probeAxis);
}

bool FootSteer_Steer(bool rightStick, float* x, float* y) {
    const Foot& F = rightStick ? g_footR : g_footL;
    if (x) *x = F.s[0];
    if (y) *y = F.s[1];
    return (g_on != 0) && g_ease > 0.0f;
}

bool  FootSteer_Enabled()     { return g_on != 0; }
void  FootSteer_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
float FootSteer_ReachCm()     { return g_reachCm; }
void  FootSteer_SetReachCm(float cm) {
    if (cm < 0.0f) cm = 0.0f; else if (cm > 200.0f) cm = 200.0f;
    g_reachCm = cm; TwkMarkDirty();
}
float FootSteer_ResponseMs()  { return g_responseMs; }
void  FootSteer_SetResponseMs(float ms) { g_responseMs = (ms < 20.0f) ? 20.0f : ms; TwkMarkDirty(); }
float FootSteer_DeadzonePct() { return g_deadzone * 100.0f; }
void  FootSteer_SetDeadzonePct(float pct) {
    g_deadzone = pct / 100.0f;
    if (g_deadzone < 0.0f) g_deadzone = 0.0f; else if (g_deadzone > 0.90f) g_deadzone = 0.90f;
    TwkMarkDirty();
}
float FootSteer_Frame()       { return (float)g_frame; }
void  FootSteer_SetFrame(float f) {
    int v = (int)(f + 0.5f);
    if (v < 0) v = 0; else if (v > 3) v = 3;
    g_frame = v; TwkMarkDirty();
}
float FootSteer_AxisX()       { return (float)g_axisX; }
void  FootSteer_SetAxisX(float a) {
    int v = (int)(a + 0.5f); if (v < 0) v = 0; else if (v > 5) v = 5;
    g_axisX = v; TwkMarkDirty();
}
float FootSteer_AxisY()       { return (float)g_axisY; }
void  FootSteer_SetAxisY(float a) {
    int v = (int)(a + 0.5f); if (v < 0) v = 0; else if (v > 5) v = 5;
    g_axisY = v; TwkMarkDirty();
}
float FootSteer_TwistDeg()    { return g_twistDeg; }
void  FootSteer_SetTwistDeg(float deg) {
    if (deg < 0.0f) deg = 0.0f; else if (deg > 90.0f) deg = 90.0f;
    g_twistDeg = deg; TwkMarkDirty();
}
float FootSteer_SwitchInvert() { return (float)g_switchInv; }
void  FootSteer_SetSwitchInvert(float v) {
    int s = (int)(v + 0.5f); if (s < 0) s = 0; else if (s > 3) s = 3;
    g_switchInv = s; TwkMarkDirty();
}
float FootSteer_TwistAxis()   { return (float)g_twistAxis; }
void  FootSteer_SetTwistAxis(float a) {
    int v = (int)(a + 0.5f); if (v < 0) v = 0; else if (v > 5) v = 5;
    g_twistAxis = v; TwkMarkDirty();
}
const char* FootSteer_FrameName() {
    switch (g_frame) {
        case 0:  return "the leg rig itself";
        case 1:  return "the flipping deck (this is what circles)";
        case 2:  return "the board, flip stripped";
        default: return "you (never flips)";
    }
}

void FootSteer_ResetDefaults() {
    g_on = 0; g_probe = 1; g_probeAxis = 0; g_probeCm = 0;
    // These must track the field-tuned values at the top of the file. They drifted once already:
    // the statics were updated when each number was settled in the headset and these were not, so
    // "Reset to defaults" would have quietly restored the pre-tuning feel.
    g_reachCm = 30.0f; g_responseMs = 300.0f; g_returnMs = 150.0f;
    g_deadzone = 0.05f; g_flickVeto = 10.0f; g_blankMs = 250; g_catchVeto = 1;
    g_frame = 3; g_axisX = 1; g_axisY = 0;
    g_twistDeg = 5.0f; g_twistAxis = 4; g_switchInv = 3; g_followIK = 1;
    TwkMarkDirty();
}

void FootSteer_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    bool on = g_on != 0;
    if (api->Checkbox("Mid-trick foot control", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(in the air; flicks stay the catch)");
    if (on) {
        api->Indent();
        float reach = g_reachCm, resp = g_responseMs;
        if (api->SliderFloat("Reach (cm)", &reach, 2.0f, 80.0f, "%.0f")) FootSteer_SetReachCm(reach);
        api->SameLine(); api->TextDisabled("(past the leg's reach the foot stops travelling and buzzes)");
        if (api->SliderFloat("Response (ms)", &resp, 100.0f, 800.0f, "%.0f")) {
            g_responseMs = resp; TwkMarkDirty();
        }
        api->SameLine(); api->TextDisabled("(lower = quicker, and closer to a flick)");
        float veto = g_flickVeto;
        if (api->SliderFloat("Flick veto (units/s)", &veto, 2.0f, 40.0f, "%.1f")) {
            g_flickVeto = veto; TwkMarkDirty();
        }
        api->SameLine(); api->TextDisabled("(above this the gesture is a catch)");
        bool cv = g_catchVeto != 0;
        if (api->Checkbox("Yield to the catch", &cv)) { g_catchVeto = cv ? 1 : 0; TwkMarkDirty(); }
        // The basis must not carry the board's flip. Frame 1 does, and a held stick then traces a
        // circle through a kickflip -- the direction it means rolls with the deck.
        float axx = (float)g_axisX, axy = (float)g_axisY;
        if (api->SliderFloat("Stick X drives axis", &axx, 0.0f, 5.0f, "%.0f")) FootSteer_SetAxisX(axx);
        if (api->SliderFloat("Stick Y drives axis", &axy, 0.0f, 5.0f, "%.0f")) FootSteer_SetAxisY(axy);
        api->TextDisabled("axis 0/1/2 = +X/+Y/+Z, 3/4/5 = the same negated");
        float tw = g_twistDeg, ta = (float)g_twistAxis;
        if (api->SliderFloat("Foot twist (deg)", &tw, 0.0f, 20.0f, "%.0f")) FootSteer_SetTwistDeg(tw);
        api->SameLine(); api->TextDisabled("(push up = toe forward, pull back = toe back)");
        if (api->SliderFloat("Twist about axis", &ta, 0.0f, 5.0f, "%.0f")) FootSteer_SetTwistAxis(ta);
        bool fik = g_followIK != 0;
        if (api->Checkbox("Fade with the game's foot IK", &fik)) { g_followIK = fik ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(off = fight it, for A/B)");
        api->Unindent();
    }
    bool pr = g_probe != 0;
    if (api->Checkbox("Log a sample block per air", &pr)) { g_probe = pr ? 1 : 0; TwkMarkDirty(); }
    if (g_probeCm != 0) {
        snprintf(b, sizeof(b), "PROBE ACTIVE: both feet pushed %d cm along axis %d, sticks ignored",
                 g_probeCm, g_probeAxis);
        api->Text(b);
    }
    if (!g_ok) api->Text("steering PAUSED after a fault (foot placement unaffected)");
    else if ((LONG)g_uiCalls == 0) api->TextDisabled("waiting for the foot-anchor hook");
    else {
        // "in state" is the STATE test alone -- with steering off it still lights up, which is what
        // says the arming works before the feature is ever switched on. Ease is what is applied.
        snprintf(b, sizeof(b), "%s   ease %.2f   L(%+.2f,%+.2f)  R(%+.2f,%+.2f)%s",
                 g_uiArmed ? "in state" : "idle", (float)g_uiEase,
                 (float)g_uiLx, (float)g_uiLy, (float)g_uiRx, (float)g_uiRy,
                 g_uiBlanked ? "   [flick/catch]" : "");
        api->Text(b);
        // The game's own IK weight, live: when it reads 0 the graph is ignoring the foot socket
        // entirely, and no amount of offset will move that foot.
        snprintf(b, sizeof(b), "game foot IK weight:  L %.2f   R %.2f%s",
                 (float)g_uiWL, (float)g_uiWR,
                 ((float)g_uiWL == 0.0f && (float)g_uiWR == 0.0f) ? "   (IK off - foot not steerable)" : "");
        api->Text(b);
    }
}
