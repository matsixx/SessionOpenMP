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
// HOW THE GAME PLACES THE FEET, which everything below is built on.
// Where a foot sits along the board is AUTHORED ANIMATION. The rolling idle parks the back foot up
// on the tail; the landing clip puts both feet over the bolts and then blends back to the idle; the
// crank, the manuals, the catches and the push each have their own spots. USkaterAnimInstance::
// UpdateFootAnchors takes whatever the animation produced, anchors it to the board, smooths it and
// sweeps it onto the deck -- and consults almost no riding state while doing it (only the
// throwdown, switch, mongo-push avoidance and a reset flag). So a stance cannot be a constant
// offset "while riding": that lands on the landing, the crank and the manuals too.
// What this does instead: it moves the feet ONLY while the animation is in its rolling idle -- read
// off the anim blueprint's own state variables, the same ones its state machine switches on -- and
// only as far as the animated foot has actually arrived home, so a landing blends into the stance
// rather than bouncing through the tail. Everything is a DIFFERENCE from this frame's animation, so
// zero on every slider is the stock stance exactly.
#include "tweaks_common.h"
#include "board_stance.h"
#include "ui/menu_ext.h"
#include "grind_pop.h"       // GrindPop_FNameToString -- the drawn skeleton's bone names
#include "grind_lean.h"      // GrindLean_TruckWall -- the landing lean is sized off your trucks
#include "sit.h"             // Sit_TraceSurface -- the ground under the board, for the heel slap
#include <cmath>
#include <cstring>

// ------------------------------------------------------------------ measured offsets
enum {
    AN_SKATER     = 0x608,
    AN_L_SOCK_LOC = 0x404,   AN_R_SOCK_LOC = 0x41c,   // FVector, mesh space
    AN_L_SOCK_ROT = 0x410,   AN_R_SOCK_ROT = 0x428,   // FRotator, mesh space
    AN_L_ALPHA    = 0x3fc,   AN_R_ALPHA    = 0x400,   // 0 = the socket is ignored
    SK_MESH       = 0x280,
    SK_BOARD      = 0x568,   BOARD_FLIPPER = 0x4e8,   // the deck mesh: its frame IS the deck's
    BOARD_TRUCK_BACK = 0x4f0, BOARD_TRUCK_FRONT = 0x500,   // where the flat between the kicks ends
    BOARD_MOVE    = 0x298,   // ASkateboardEx::_skateboardMovement
    BM_MODE       = 0x534,   // ...its _movementMode (ESkateboardMovementMode, read only; see sit.cpp)
    COMP_CTW_POS   = 0x1d0,                           // USceneComponent ComponentToWorld translation
    COMP_CTW_SCALE = 0x1e0,                           // ...and scale, past the padded translation

    // The anim blueprint's own riding state (USkaterAnimInstance, PDB). Its state machine switches on
    // these, so they say exactly when the feet are somewhere other than the rolling idle.
    AN_POWERSLIDE_STANCE = 0x2d8, AN_POWERSLIDING = 0x2d9, AN_EXIT_POWERSLIDE = 0x2da,
    AN_ON_BOARD     = 0x300, AN_PICKING_UP = 0x301, AN_THROWING_DOWN = 0x302,
    AN_FOOT_POS     = 0x305,   // EFootPositionType: 0 None, 1 Regular, 2 Fakie, 3 Nollie, 4 Switch
    AN_FOOT_POS_TR  = 0x307,   // EFootPositionTransitionType: non-zero mid stance swap
    AN_TRICK_PEND   = 0x310, AN_CATCH_ORIENT = 0x312, AN_AUTO_CATCH = 0x320, AN_GRAB = 0x328,
    AN_PUSHING      = 0x329, AN_MANUAL = 0x32e, AN_NOSE_MANUAL = 0x32f, AN_MANUAL_RATIO = 0x330,
    AN_CASPER       = 0x334, AN_ANTI_CASPER = 0x335, AN_GRINDING = 0x33c, AN_LIPTRICK = 0x33d,
    AN_KICKTURN_L   = 0x3c8, AN_KICKTURN_R = 0x3c9, AN_PRIMO = 0x3cc, AN_WALLRIDE = 0x3cd,
    AN_FLIPPING     = 0x495, AN_ROTATING = 0x496, AN_CRANKING = 0x497, AN_QUICK_SWAP = 0x4c4,
    AN_REVERT       = 0x4d8, AN_GROUNDED = 0x5fa, AN_LANDING = 0x5fd, AN_JUST_LANDED = 0x5fe,
    AN_IS_SWITCH    = 0x303, AN_IS_GOOFY = 0x304,
    AN_LAND_FOOT_POS = 0x306, AN_STANCE_XFER = 0x30c,   // LandingFootPositionType, FlipTrickStanceTransferRatio
    AN_LAND_DROP    = 0x604,                            // LandDropHeightRatio (0..1): the game's own landing size
    BM_CATCH_L      = 0x538, BM_CATCH_R = 0x540,        // USkateboardExMovementComponent::_left/_rightFootCatchInfo
    SKM_MESH        = 0x480,                            // USkinnedMeshComponent::SkeletalMesh
    SKM_CST         = 0x4b0, SKM_READ = 0x4f4,          // ComponentSpaceTransformsArray[2], CurrentReadComponentTransforms
    SKM_EDIT        = 0x4f0,                            // CurrentEditableComponentTransforms (what the flip hands on)
    SM_REFSKEL      = 0x1b0, RS_FINAL_INFO = 0x20,      // USkeletalMesh::RefSkeleton -> FinalRefBoneInfo {FName, int parent}
    RS_FINAL_POSE   = 0x30,                             //   ...FinalRefBonePose (FTransform each, parent-relative)
};

// ------------------------------------------------------------------ knobs
// Millimetres and degrees, per foot, in the deck's frame. Zero everywhere = the game's own stance,
// which is what a fresh install must feel like.
static int g_on          = 1;     // StanceOn
// Two sets: [0] the MAIN stance (EFootPositionType Regular -- your natural stance, goofy or regular)
// and [1] SWITCH. Front and back are the leading and trailing foot, relative to the way you are
// going, so "back foot" means the foot nearest the camera in either one.
enum { S_FA, S_FC, S_FY, S_FP, S_BA, S_BC, S_BY, S_BP, S_N };
static int g_set[2][S_N];
static const char* const kSetKey[2][S_N] = {
    { "StanceFrontAlongMm", "StanceFrontAcrossMm", "StanceFrontAngleDeg", "StanceFrontPitchDeg",
      "StanceBackAlongMm",  "StanceBackAcrossMm",  "StanceBackAngleDeg",  "StanceBackPitchDeg" },
    { "StanceSwFrontAlongMm", "StanceSwFrontAcrossMm", "StanceSwFrontAngleDeg", "StanceSwFrontPitchDeg",
      "StanceSwBackAlongMm",  "StanceSwBackAcrossMm",  "StanceSwBackAngleDeg",  "StanceSwBackPitchDeg" },
};
static const int kLo[S_N] = { -250, -100, -40, -30, -250, -100, -40, -30 };
static const int kHi[S_N] = {  250,  100,  40,  30,  250,  100,  40,  30 };
static int g_follow      = 1;     // StanceFollowDeck -- ride the kick instead of staying on the flat

// THE LANDING STANCE: on touchdown each foot goes over the BOLTS -- the truck under it -- and holds
// there for a moment, then glides on into the idle. The sliders are offsets from the bolts (0 = right
// over them), front/back = leading/trailing foot as above, ONE SET PER KIND OF LANDING (485): what the
// feet touch down in -- regular, switch, nollie or fakie -- picks the set. With a stance left at zero
// too, when "Fixed landing stance" is on (505): the feet then step back into the game's own stance.
enum { L_FA, L_FC, L_FY, L_BA, L_BC, L_BY, L_N };
enum { LT_REG, LT_SW, LT_NOLLIE, LT_FAKIE, LT_N };
static int g_landOn = 1;                                  // StanceLandOn
static int g_land[LT_N][L_N];
static const char* const kLandKey[LT_N][L_N] = {   // regular keeps 436's names (the 435 keys were offsets from the stance)
    { "StanceLandBoltFrontAlongMm", "StanceLandBoltFrontAcrossMm", "StanceLandBoltFrontAngleDeg",
      "StanceLandBoltBackAlongMm",  "StanceLandBoltBackAcrossMm",  "StanceLandBoltBackAngleDeg" },
    { "StanceLandSwBoltFrontAlongMm", "StanceLandSwBoltFrontAcrossMm", "StanceLandSwBoltFrontAngleDeg",
      "StanceLandSwBoltBackAlongMm",  "StanceLandSwBoltBackAcrossMm",  "StanceLandSwBoltBackAngleDeg" },
    { "StanceLandNollieBoltFrontAlongMm", "StanceLandNollieBoltFrontAcrossMm", "StanceLandNollieBoltFrontAngleDeg",
      "StanceLandNollieBoltBackAlongMm",  "StanceLandNollieBoltBackAcrossMm",  "StanceLandNollieBoltBackAngleDeg" },
    { "StanceLandFakieBoltFrontAlongMm", "StanceLandFakieBoltFrontAcrossMm", "StanceLandFakieBoltFrontAngleDeg",
      "StanceLandFakieBoltBackAlongMm",  "StanceLandFakieBoltBackAcrossMm",  "StanceLandFakieBoltBackAngleDeg" } };
static int s_landType = LT_REG;                           // the kind of landing now running (set at touchdown)
static const int kLandDef[L_N] = { 0, 0, 0, 0, 0, 0 };
static const int kLandLo[L_N]  = { -150, -100, -40, -150, -100, -40 };
static const int kLandHi[L_N]  = {  150,  100,  40,  150,  100,  40 };
static int g_landHoldMs   = 250;                          // StanceLandHoldMs -- from touchdown
static int g_landReturnMs = 300;                          // StanceLandReturnMs -- the glide on into the idle
static int g_landVarMm    = 30;                           // StanceLandVariationMm -- each landing its own spot
static int g_landStep     = 1;                            // StanceLandStep -- swivel into the stance (0 = glide)
static int g_landPivotMs  = 140;                          // StanceLandPivotMs -- one pivot of a swivel
static int g_landAnim     = 1;                            // StanceLandAnimFix (ini only) -- see THE ANIMATION LANDS IN THE STANCE YOU ROLL AWAY IN
static int g_airCatch     = 1;                            // StanceLandCatchInAir (ini only) -- see CATCHING INTO THE LANDING
static int g_landFix      = 1;                            // StanceLandFixed -- the landing also for a stance left at zero (505)
static int g_landLeanPct  = 50;                           // StanceLandLeanPct -- the deck's lean on its trucks off a drop (511)
static int g_landImpactPct = 100;                         // StanceLandImpactPct -- the feet take the landing (512); 0 = they stay put

// What the gate is doing right now, for the F1 page (written on the game thread, read on the render
// thread: a pointer to a string literal and a float, so a torn read is impossible).
static const char* volatile g_status = "not riding";
static volatile float       g_shown  = 0.0f;
static volatile int         g_live   = -1;    // the set driving the feet right now: 0 main, 1 switch, -1 none

static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void LoadLearned(const char* buf);       // the learned idle and deck, kept across launches (below)
static void SaveLearned(char* buf, size_t cap);
static void LoadRefs(const char* buf);          // the landing references -- toes, switch tilt, ball -- kept too (498)
static void SaveRefs(char* buf, size_t cap);
static void ForgetRefs();

void Stance_ReadConfig(const char* buf) {
    g_on     = TwkIniInt(buf, "StanceOn", 1) ? 1 : 0;
    g_follow = TwkIniInt(buf, "StanceFollowDeck", 1) ? 1 : 0;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < S_N; i++) g_set[k][i] = clampI(TwkIniInt(buf, kSetKey[k][i], 0), kLo[i], kHi[i]);
    g_landOn = TwkIniInt(buf, "StanceLandOn", 1) ? 1 : 0;
    for (int i = 0; i < L_N; i++) g_land[LT_REG][i] = clampI(TwkIniInt(buf, kLandKey[LT_REG][i], kLandDef[i]), kLandLo[i], kLandHi[i]);
    for (int t = 1; t < LT_N; t++)       // a kind never set yet lands like regular
        for (int i = 0; i < L_N; i++) g_land[t][i] = clampI(TwkIniInt(buf, kLandKey[t][i], g_land[LT_REG][i]), kLandLo[i], kLandHi[i]);
    g_landHoldMs   = clampI(TwkIniInt(buf, "StanceLandHoldMs", 250), 0, 3000);
    g_landReturnMs = clampI(TwkIniInt(buf, "StanceLandReturnMs", 300), 50, 2000);
    g_landVarMm    = clampI(TwkIniInt(buf, "StanceLandVariationMm", 30), 0, 60);
    g_landStep     = TwkIniInt(buf, "StanceLandStep", 1) ? 1 : 0;
    g_landPivotMs  = clampI(TwkIniInt(buf, "StanceLandPivotMs", 140), 80, 300);
    g_landAnim     = TwkIniInt(buf, "StanceLandAnimFix", 1) ? 1 : 0;
    g_airCatch     = TwkIniInt(buf, "StanceLandCatchInAir", 1) ? 1 : 0;
    g_landFix      = TwkIniInt(buf, "StanceLandFixed", 1) ? 1 : 0;
    g_landLeanPct  = clampI(TwkIniInt(buf, "StanceLandLeanPct", 50), 0, 100);
    g_landImpactPct = clampI(TwkIniInt(buf, "StanceLandImpactPct", 100), 0, 200);
    LoadLearned(buf);
    LoadRefs(buf);
}

void Stance_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "StanceOn",         g_on);
    TwkIniSetInt(buf, cap, "StanceFollowDeck", g_follow);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < S_N; i++) TwkIniSetInt(buf, cap, kSetKey[k][i], g_set[k][i]);
    TwkIniSetInt(buf, cap, "StanceLandOn", g_landOn);
    for (int t = 0; t < LT_N; t++)
        for (int i = 0; i < L_N; i++) TwkIniSetInt(buf, cap, kLandKey[t][i], g_land[t][i]);
    TwkIniSetInt(buf, cap, "StanceLandHoldMs",   g_landHoldMs);
    TwkIniSetInt(buf, cap, "StanceLandReturnMs", g_landReturnMs);
    TwkIniSetInt(buf, cap, "StanceLandVariationMm", g_landVarMm);
    TwkIniSetInt(buf, cap, "StanceLandStep",        g_landStep);
    TwkIniSetInt(buf, cap, "StanceLandPivotMs",     g_landPivotMs);
    TwkIniSetInt(buf, cap, "StanceLandAnimFix",     g_landAnim);
    TwkIniSetInt(buf, cap, "StanceLandCatchInAir",  g_airCatch);
    TwkIniSetInt(buf, cap, "StanceLandFixed",       g_landFix);
    TwkIniSetInt(buf, cap, "StanceLandLeanPct",     g_landLeanPct);
    TwkIniSetInt(buf, cap, "StanceLandImpactPct",   g_landImpactPct);
    SaveLearned(buf, cap);
    SaveRefs(buf, cap);
}

static void CentreSet(int k) { for (int i = 0; i < S_N; i++) g_set[k][i] = 0; TwkMarkDirty(); }
static void DefaultLanding() {
    g_landOn = 1; g_landFix = 1; g_landLeanPct = 50; g_landImpactPct = 100; g_landHoldMs = 250; g_landReturnMs = 300;
    g_landVarMm = 30; g_landStep = 1; g_landPivotMs = 140;
    for (int t = 0; t < LT_N; t++) for (int i = 0; i < L_N; i++) g_land[t][i] = kLandDef[i];
    TwkMarkDirty();
}

void Stance_ResetDefaults() {
    g_on = 1;
    g_follow = 1;
    CentreSet(0); CentreSet(1);
    DefaultLanding();
}

// ------------------------------------------------------------------ stepping off the landing
// How a foot moves on griptape without leaving it: a SWIVEL -- pivot on the ball of the foot so the
// heel swings over, then on the heel so the toes come round -- with the foot sliding the rest of the
// way as it goes (a scoot). It never lifts; only the heel (pivoting on the ball) or the toes (pivoting
// on the heel) come up a little. The socket is taken as the ankle, the ball kBallCm ahead of it.
// 445, from the 444 log and "too many swivels ... a bit odd looking": the feet travel only 7-10 cm
// after a landing (front 8.6 avg, back 7.0), but one swivel covered at most 6.6 cm, so EVERY step was
// two full swivels, and both feet moved nearly together (front 0.16 s behind). Now: ONE swivel up to
// 12 cm, its angle growing with the distance (kDegPerCm, capped kMaxSwivelDeg) and the remainder slid;
// the heel pivot starts before the ball pivot ends (one fluid motion, not stop-start); a longer way
// takes a little longer; and the front foot waits until the back foot is 3/4 done -- nobody unweights
// both feet at once. Each pivot on the minimum-jerk profile of human reaching movements.
struct Pose { float x, c, y, p, up; };  // along cm (+ front), across cm (+ toes), yaw deg (+ toes to the front), pitch deg (+ toes up), rise cm
struct Step { bool latched; int n; float phi, dir, T, ex, ey; float t0[4], td[4]; };   // pivot j: start, length
static Step  s_step[2];                 // [0] back foot, [1] front foot
// THE LANDING IS HELD, not only its spot along the board. Along is a spot of our own, but across and
// the foot's angle are added to the ANIMATION -- and the game's landing clip blends back to the idle
// during the hold, so the feet slowly took on the idle's angles before the swivel ("as it holds, it
// slowly transitions my feet to what they would look like at my custom stance"). So the animated
// foot's across and rotation are caught on the first frame the landing shows and held against the
// clip, the hold fading out over the swivel. Per physical foot: [0] left, [1] right.
// The rotation is held against the BOARD (q = the socket's rotation in the board's frame), like the
// across: after a shifty the game turns the board back under the body through the hold, and a turn
// held in the mesh's frame stayed put while the deck turned under it -- the foot ended up angled
// across the board, toes onto the kick ("the landing gets messed up ... my foot through the board").
// The height is set from the ball of the foot (500, below), not held.
struct Freeze { bool ok; float x, y; float q[4]; };
static Freeze s_frz[2];
static bool   s_frzWant = false;
static bool  s_settled = true;          // both feet are home since the last landing
static float s_rand[2][3];              // this landing's own spot, per role: along mm, across mm, angle deg
static const float kBallCm = 14.0f, kMaxSwivelDeg = 24.0f, kDegPerCm = 2.4f, kHeelRaiseDeg = 3.0f, kToeRaiseDeg = 2.0f;
static const float kSlideSec = 0.22f;
static const float kDeg = 0.0174532925f;

static float MinJerk(float u) { u = clampF(u, 0.0f, 1.0f); return u * u * u * (10.0f - 15.0f * u + 6.0f * u * u); }

// The swivels alone, from the landing pose, tau seconds in: the heel's travel (hx along, hy across),
// the yaw they have added, and the heel/toe lift of the pivot in progress.
static void Kin(const Pose& L, const Step& s, float tau, float& hx, float& hy, float& psi, float& pitch, float& rise) {
    hx = hy = psi = pitch = rise = 0.0f;
    for (int j = 0; j < 2 * s.n; j++) {
        const float u0 = (tau - s.t0[j]) / s.td[j];
        if (u0 <= 0.0f) break;                              // pivots start in order
        const float u = u0 > 1.0f ? 1.0f : u0, m = MinJerk(u);
        const bool ball = (j % 2) == 0;                     // ball first: the heel swings toward the target
        const float dpsi = (ball ? -s.dir : s.dir) * s.phi;
        if (ball) {                                         // the ball stays put, the heel goes round it
            const float b0 = (L.y + psi) * kDeg, b1 = (L.y + psi + dpsi * m) * kDeg;
            const float bx = hx + kBallCm * sinf(b0), by = hy + kBallCm * cosf(b0);
            hx = bx - kBallCm * sinf(b1); hy = by - kBallCm * cosf(b1);
        }
        psi += dpsi * m;
        if (u0 < 1.0f) {                                    // overlapping pivots: the heel's and the toes' lifts add
            const float bump = sinf(3.14159265f * u);
            if (ball) { pitch -= kHeelRaiseDeg * bump; rise += kBallCm * sinf(kHeelRaiseDeg * bump * kDeg); }
            else        pitch += kToeRaiseDeg * bump;
        }
    }
}

static void LatchStep(Step& s, const Pose& L, const Pose& S, bool front) {
    const float dx = S.x - L.x, d = fabsf(dx);
    s.dir = dx >= 0.0f ? 1.0f : -1.0f;
    s.n   = d < 1.5f ? 0 : (d <= 12.0f ? 1 : 2);
    const float per = s.n ? d / s.n : 0.0f;                 // each swivel's share of the way
    s.phi = s.n ? fminf(fminf(kMaxSwivelDeg, kDegPerCm * per), asinf(clampF(per / kBallCm, 0.0f, 1.0f)) / kDeg) : 0.0f;
    // One swivel: the rider's pivot time for each of its two pivots, overlapped, a little longer for a
    // longer way. The heel pivot starts at 42% (the toes come round before the heel is quite down);
    // a second swivel starts at 90% of the first.
    const float sw = 2.0f * g_landPivotMs * 0.001f * 0.84f * clampF(0.8f + 0.03f * per, 0.85f, 1.2f);
    for (int i = 0; i < s.n; i++) {
        const float base = i * sw * 0.9f;
        s.t0[2 * i]     = base;              s.td[2 * i]     = sw * 0.58f;
        s.t0[2 * i + 1] = base + sw * 0.42f; s.td[2 * i + 1] = sw * 0.58f;
    }
    s.T   = s.n ? (s.n - 1) * sw * 0.9f + sw : kSlideSec;
    float psi, p, up;
    Kin(L, s, s.T + 1.0f, s.ex, s.ey, psi, p, up);          // where the swivels alone end up
    s.latched = true;
    static int s_n = 0;
    if (s_n < 80) {
        s_n++;
        if (s.n) TwkLog("[stance] stepping the %s foot %.1f cm %s: %d swivel%s of %.0f deg (%.1f cm of it slid) over %.2f s",
                        front ? "front" : "back", d, dx < 0.0f ? "back" : "forward", s.n, s.n > 1 ? "s" : "", s.phi,
                        d - s.n * kBallCm * sinf(s.phi * kDeg), s.T);
        else     TwkLog("[stance] the %s foot is %.1f cm from your stance: a small settle, no swivel", front ? "front" : "back", fabsf(dx));
    }
}

// The swivels, plus whatever they leave (across, angle, pitch, and the along they cannot hit exactly)
// blended in over the whole step, so the foot ends exactly on the stance.
// The front foot waits until the back foot is 3/4 through its step: the weight is on the front foot
// while the back foot moves, then comes back. Never before the back foot has started.
static float FrontDelay() { return s_step[0].latched ? fmaxf(0.12f, 0.75f * s_step[0].T) : 1e9f; }

static Pose StepPose(const Pose& L, const Pose& S, const Step& s, float tau) {
    if (tau >= s.T) return S;
    float hx, hy, psi, pitch, rise;
    Kin(L, s, tau, hx, hy, psi, pitch, rise);
    const float m = MinJerk(tau / s.T);
    Pose P;
    P.x  = L.x + hx + (S.x - L.x - s.ex) * m;
    P.c  = L.c + hy + (S.c - L.c - s.ey) * m;
    P.y  = L.y + psi + (S.y - L.y) * m;
    P.p  = L.p + pitch + (S.p - L.p) * m;
    P.up = rise;
    return P;
}

static uint32_t s_rng = 0;
static float RandSigned() {                                 // -1..1, xorshift
    if (!s_rng) s_rng = (uint32_t)GetTickCount64() | 1u;
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / 8388608.0f - 1.0f;
}
static void RollLanding() {
    static float s_prevAlong[2] = { 0.0f, 0.0f };
    const float v = (float)g_landVarMm;
    for (int r = 0; r < 2; r++) {
        // Along, only ever TOWARD THE MIDDLE of the board (485): past the bolts is the start of the kick,
        // and a spot out there put the shoe into the kick (near-tail landings "clip through the tail").
        // Travel-signed x: the back foot's middle is +, the front foot's is -.
        const float in = r == 0 ? 1.0f : -1.0f;
        float al = fabsf(RandSigned()) * v * in;
        // Never nearly where the last landing put this foot, so two landings in a row visibly differ.
        for (int i = 0; i < 6 && fabsf(al - s_prevAlong[r]) < v * 0.35f; i++) al = fabsf(RandSigned()) * v * in;
        s_prevAlong[r] = al;
        s_rand[r][0] = al;
        s_rand[r][1] = RandSigned() * v * 0.5f;
        s_rand[r][2] = 0.0f;                    // along and across only: the rider's rule
    }
}

// THE FEET TAKE THE LANDING (512). "It just catches then keeps a perfect foot position all the way through
// and on landing they don't move even slightly." Two reactions, from touchdown, sized by StanceLandImpactPct:
//  - the SLAP: caught in the air the heels ride a touch up (the weight is not on yet; the balls hold the
//    board); on touchdown they come down flat, dip a little past it as the sole takes the weight, and
//    settle. A pitch about the board's length, so the ball height (500) keeps the ball planted: the heel moves;
//  - the KNOCK: the impact shoves each foot a little -- along (toward the middle only, as the variation:
//    past the bolts is the kick), across and a small twist, its own way per foot -- in ~70 ms, and the landing
//    holds it there until the swivel takes it home. A bigger fall knocks harder; a flat trick still gets some.
static const float kSlapDeg = 5.0f;
// THE HEEL IS DOWN WHEN THE BOARD HITS (514). 512 dropped it in ~50 ms after touchdown -- inside the game's
// own landing jolt, invisible; 513 hung it up after touchdown and then let it fall -- heels lifted on a board
// already on the ground ("These are my heels on landing"). So the heel comes down on the WAY IN: the ground
// under the board (a trace) and the board's fall speed give the time to touchdown, and the heel falls --
// faster and faster -- over the last kSlapFall of it, flat at the moment of impact. From touchdown the toes
// flick up and settle (the rebound), bigger off a bigger fall. Ground not seen (a gap, a missed trace):
// whatever lift is left comes down in kSlapCatchUp after touchdown.
static const float kSlapFall = 0.11f, kSlapCatchUp = 0.05f, kSlapBounce = 0.08f, kSlapBounceTau = 0.05f;
static const float kGravity = 980.0f;
static float s_zPrev = 0.0f, s_vz = 0.0f, s_clear = 9.0f;  // the board's fall speed (cm/s); its height above the ground when down
static bool  s_zPrevOk = false;
static float s_tImpact = 1e9f;                             // seconds to touchdown, from the trace (1e9 = not seen)
static float s_slapLast = 0.0f, s_slapAtTouch = 0.0f;      // the heel's pitch on the last air frame / at touchdown
static float AirSlapPitch(float h) {                        // deg, + toes up
    if (s_tImpact >= kSlapFall) return -h;
    const float u = 1.0f - clampF(s_tImpact / kSlapFall, 0.0f, 1.0f);
    return -h * (1.0f - u * u);
}
static float LandSlapPitch(float t, float h, float impact) {   // t from touchdown
    float t0 = 0.0f;
    if (s_slapAtTouch < -0.01f) {                            // not down yet: the rest of it, fast
        if (t < kSlapCatchUp) { const float u = t / kSlapCatchUp; return s_slapAtTouch * (1.0f - u * u); }
        t0 = kSlapCatchUp;
    }
    const float v = t - t0;
    if (v > 2.0f * kSlapBounce) return 0.0f;
    const float peak = h * (0.2f + 0.2f * impact);             // the rebound's top: 1 deg flat, 2 at a big fall
    return peak / 0.5f * expf(-v / kSlapBounceTau) * sinf(3.14159265f * v / kSlapBounce);
}
static const float kKnockCm = 1.5f, kKnockAcrossCm = 0.8f, kKnockDeg = 3.0f, kKnockSec = 0.07f;
static const float kFallFlat = 10.0f, kFallFull = 160.0f;   // cm from the air's peak to touchdown
static float s_knock[2][3] = {};                             // per role [0] back [1] front: along cm, across cm, yaw deg
static float s_fallImpact = 0.0f;                            // 0..1: this landing's fall
static float s_fallCm = 0.0f;
static void RollKnock(float scale) {
    auto mag = [] { return 0.4f + 0.6f * fabsf(RandSigned()); };
    auto sgn = [] { return RandSigned() >= 0.0f ? 1.0f : -1.0f; };
    for (int r = 0; r < 2; r++) {
        s_knock[r][0] = mag() * kKnockCm * scale * (r == 0 ? 1.0f : -1.0f);   // toward the middle
        s_knock[r][1] = sgn() * mag() * kKnockAcrossCm * scale;
        s_knock[r][2] = sgn() * mag() * kKnockDeg * scale;
    }
}

// ------------------------------------------------------------------ is the animation in its rolling idle?
// The first state that says otherwise, by name -- the name is what the log and the F1 page show, so
// "why isn't my stance applying" is answered by the game's own variable rather than a guess.
static const char* IdleBlocker(void* a, bool landingCovers, bool stanceCovered) {
    if (twkB(a, AN_ON_BOARD) != 1) return "off the board";
    if (twkB(a, AN_GROUNDED) != 1) return "in the air";
    struct Flag { int off; const char* name; };
    static const Flag kSet[] = {
        { AN_PICKING_UP, "picking up" },        { AN_THROWING_DOWN, "throwing down" },
        { AN_TRICK_PEND, "trick pending" },
        { AN_CRANKING, "setting up a trick" },  { AN_QUICK_SWAP, "setting up a trick" },
        { AN_CATCH_ORIENT, "catching" },
        { AN_GRAB, "grabbing" },                { AN_PUSHING, "pushing" },
        { AN_CASPER, "casper" },                { AN_ANTI_CASPER, "anti-casper" },
        { AN_PRIMO, "primo" },                  { AN_GRINDING, "grinding" },
        { AN_LIPTRICK, "liptrick" },            { AN_KICKTURN_L, "kick turn" },
        { AN_KICKTURN_R, "kick turn" },         { AN_WALLRIDE, "wallride" },
        { AN_POWERSLIDING, "powerslide" },      { AN_POWERSLIDE_STANCE, "powerslide" },
        { AN_EXIT_POWERSLIDE, "powerslide" },
        { AN_FLIPPING, "board flipping" },      { AN_ROTATING, "board rotating" },
        { AN_JUST_LANDED, "landing" },
    };
    // The rule is the rider's: the stance is the IDLE stance, and anything that is not rolling idle --
    // kick turns included -- keeps the game's own feet.
    // The landing is the landing stance's, when it is in use (see THE LANDING in Stance_AddOffset).
    for (const Flag& f : kSet)
        if (twkB(a, f.off) != 0 && !(landingCovers && f.off == AN_JUST_LANDED)) return f.name;
    // NOT IsAutoCatching (+0x320): it records how the last catch was made and stays set after the
    // landing -- the 423 log has it holding the stance off for 76 s of plain rolling, until the next
    // trick (or getting off and on) cleared it: "it keeps losing foot position". A catch in progress is
    // CatchOrientState (above, and foot_place's riding test), and the air is "in the air".
    // A MANUAL IS WHAT SHOWS: the board tipped up, which ManualRatio carries. The manual BITS
    // (IsInManual / IsInNoseManual -- copied off the skater's +0x918, next to _landedInManual) stay set
    // for seconds after some landings -- 360 flips in the field -- while he rolls away with the board
    // flat: the 428 log has "manual" holding 5 s ("held off for 5 s on the ground by: manual") and the
    // rider seeing his normal stance throughout. So the bits alone do not hold the stance off; a ratio
    // that says the board is actually up does. In above 0.2, out below 0.1, so it cannot flicker.
    {
        static bool s_tipped = false;
        const bool bits  = twkB(a, AN_MANUAL) != 0 || twkB(a, AN_NOSE_MANUAL) != 0;
        const float r    = fabsf(twkF(a, AN_MANUAL_RATIO));
        if (!bits || r < 0.1f) s_tipped = false;
        else if (r > 0.2f)     s_tipped = true;
        // While the bits are set, say what they and the ratio read, once a second: if a manual ever
        // holds the stance off when it should not, the next log shows the numbers.
        static float s_logT = 0.0f; static int s_n = 0;
        s_logT += 1.0f / 60.0f;
        if (bits && s_logT > 1.0f && s_n < 60) {
            s_logT = 0.0f; s_n++;
            TwkLog("[stance] manual bits set (manual %d, nose %d), ratio %.2f -> %s", twkB(a, AN_MANUAL),
                   twkB(a, AN_NOSE_MANUAL), twkF(a, AN_MANUAL_RATIO), s_tipped ? "holding the stance off" : "stance stays on");
        }
        if (s_tipped) return twkB(a, AN_NOSE_MANUAL) ? "nose manual" : "manual";
    }
    const int st = twkB(a, AN_FOOT_POS);
    // Regular and switch are the stances you roll around in; fakie and nollie park the feet
    // elsewhere on purpose, and are left exactly as the game has them.
    // ...except a landing in them, which the landing stance takes (see A NOLLIE OR FAKIE LANDING).
    if (st == 2 && !stanceCovered) return "riding fakie";
    if (st == 3 && !stanceCovered) return "riding nollie";
    if (st != 1 && st != 4 && !((st == 2 || st == 3) && stanceCovered)) return "no stance yet";
    float wl = twkF(a, AN_L_ALPHA), wr = twkF(a, AN_R_ALPHA);
    if (!(wl > 0.5f) || !(wr > 0.5f)) return "foot IK off";
    return nullptr;
}

// ------------------------------------------------------------------ frames
static bool CompWorld(void* c, float q[4], float t[3]) {
    if (!c || !TwkCompQuat(c, q)) return false;
    for (int i = 0; i < 3; i++) {
        t[i] = twkF(c, COMP_CTW_POS + i * 4);
        if (!(t[i] > -1e7f && t[i] < 1e7f)) return false;
    }
    return true;
}

// Everything a foot needs to be moved between the mesh (where the sockets live) and the board.
struct Ride {
    float along[3], across[3], up[3];   // the deck's axes in MESH space, along = the way he is going
    float qm[4], tm[3], sm;             // mesh: world rotation, translation, scale
    float qf[4], tf[3];                 // flipper (the deck): world rotation and translation
    float travel;                       // +1 when the flipper's own +X is the way he is going
    float zs;                           // +1 when the flipper's own +Z is skyward
    float truck[2];                     // |x| of the trucks on the flipper's -x / +x side
    void* board;
};

// WHICH END IS FRONT comes from the stance and the feet, never from a heading or a velocity: the game
// says which stance you are in and whether you are goofy, which says which foot leads (regular riding
// his main stance leads with the left; goofy, or switch, with the right -- the same rule foot_steer
// and the catch code use), and that foot's own position says which end of the board is the front.
// The feet sit 40 cm apart, so that cannot flicker. RideFrame hands back the flipper's own +x; the
// caller signs it once the feet are read. (A capsule heading -- wrong in switch -- and a position-
// delta velocity -- wrong outright -- were both tried and are gone.)
static bool RideFrame(void* a, Ride& rd) {
    void* sk   = twkP(a, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    void* bd   = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* flp  = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    if (!mesh || !flp) return false;
    if (!CompWorld(mesh, rd.qm, rd.tm) || !CompWorld(flp, rd.qf, rd.tf)) return false;
    rd.sm = twkF(mesh, COMP_CTW_SCALE);
    if (!(rd.sm > 0.01f && rd.sm < 100.0f)) rd.sm = 1.0f;
    rd.board = bd;

    const float x[3] = { 1.0f, 0.0f, 0.0f }, z[3] = { 0.0f, 0.0f, 1.0f };
    float aw[3], nw[3];
    TwkQuatRotate(rd.qf, x, aw);                    // the deck's long axis, in the world
    TwkQuatRotate(rd.qf, z, nw);                    // its normal
    rd.zs = (nw[2] < 0.0f) ? -1.0f : 1.0f;
    for (int i = 0; i < 3; i++) nw[i] *= rd.zs;
    rd.travel = 1.0f;                               // signed by the caller (see WHICH END IS FRONT)
    for (int i = 0; i < 3; i++) aw[i] *= rd.travel;

    TwkQuatInvRotate(rd.qm, aw, rd.along);
    TwkQuatInvRotate(rd.qm, nw, rd.up);
    rd.across[0] = rd.up[1]*rd.along[2] - rd.up[2]*rd.along[1];
    rd.across[1] = rd.up[2]*rd.along[0] - rd.up[0]*rd.along[2];
    rd.across[2] = rd.up[0]*rd.along[1] - rd.up[1]*rd.along[0];
    const float l = sqrtf(rd.across[0]*rd.across[0] + rd.across[1]*rd.across[1] + rd.across[2]*rd.across[2]);
    if (!(l > 1e-3f)) return false;
    for (int i = 0; i < 3; i++) rd.across[i] /= l;

    // The trucks, in the deck's own frame: the only thing read off the board, and only positions.
    rd.truck[0] = rd.truck[1] = 0.0f;
    const int tOff[2] = { BOARD_TRUCK_BACK, BOARD_TRUCK_FRONT };
    for (int k = 0; k < 2; k++) {
        float q[4], t[3];
        if (!CompWorld(twkP(bd, tOff[k]), q, t)) return false;
        const float d[3] = { t[0] - rd.tf[0], t[1] - rd.tf[1], t[2] - rd.tf[2] };
        float lp[3]; TwkQuatInvRotate(rd.qf, d, lp);
        rd.truck[lp[0] >= 0.0f ? 1 : 0] = fabsf(lp[0]);
    }
    return rd.truck[0] > 1.0f && rd.truck[1] > 1.0f;
}

// A mesh-space socket, in the deck's frame: x along the flipper's own axis, h above it (skyward).
// A mesh-space socket's coordinate ACROSS the deck (the flipper's own y).
static float DeckY(const Ride& rd, const float m[3]) {
    float sc[3] = { m[0] * rd.sm, m[1] * rd.sm, m[2] * rd.sm };
    float w[3]; TwkQuatRotate(rd.qm, sc, w);
    for (int i = 0; i < 3; i++) w[i] += rd.tm[i] - rd.tf[i];
    float l[3]; TwkQuatInvRotate(rd.qf, w, l);
    return l[1];
}

// Identity -> q, a fraction t of the way (normalised lerp: these corrections are small).
static void QuatPart(const float q[4], float t, float out[4]) {
    const float sg = q[3] < 0.0f ? -1.0f : 1.0f;
    float r[4] = { q[0] * sg * t, q[1] * sg * t, q[2] * sg * t, 1.0f + (q[3] * sg - 1.0f) * t };
    const float n = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2] + r[3]*r[3]);
    for (int i = 0; i < 4; i++) out[i] = n > 1e-6f ? r[i] / n : (i == 3 ? 1.0f : 0.0f);
}

static void ToDeck(const Ride& rd, const float m[3], float& x, float& h) {
    float s[3] = { m[0] * rd.sm, m[1] * rd.sm, m[2] * rd.sm };
    float w[3]; TwkQuatRotate(rd.qm, s, w);
    for (int i = 0; i < 3; i++) w[i] += rd.tm[i] - rd.tf[i];
    float l[3]; TwkQuatInvRotate(rd.qf, w, l);
    x = l[0];
    h = l[2] * rd.zs;
}

// Which way the toes point, from the foot's own socket rotation (the foot bone runs along its X).
// Used only to decide which side "+ across" and the angle signs land on.
// The smallest rotation taking one direction onto another.
static void RotBetween(const float from[3], const float to[3], float q[4]) {
    const float c[3] = { from[1]*to[2] - from[2]*to[1], from[2]*to[0] - from[0]*to[2],
                         from[0]*to[1] - from[1]*to[0] };
    const float d = from[0]*to[0] + from[1]*to[1] + from[2]*to[2];
    float r[4] = { c[0], c[1], c[2], 1.0f + d };
    float n = r[0]*r[0] + r[1]*r[1] + r[2]*r[2] + r[3]*r[3];
    if (n < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    n = 1.0f / sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] = r[i] * n;
}

// ------------------------------------------------------------------ where the idle puts each foot
// Learned, per stance and per foot, only once the idle has held long enough that a landing or a
// push can no longer be blending out of it. The deck model and the landing blend both hang off it.
struct Home { bool ok; float x, h; };
static Home  s_home[5][2];               // [EFootPositionType][0 = left foot, 1 = right foot]
static float s_idleFor = 0.0f;           // seconds the gate has been open, continuously

// WHICH SIDE OF THE BOARD THE TOES ARE ON comes from the stance, never from the feet. Read off the
// foot sockets it went wrong twice: per frame, the socket's X axis is only loosely the toe direction,
// so the test flickered and "+ across" jumped to the heel side now and then ("sometimes I see it
// twitch to -50"); voted on, both feet summed, the right foot's mirrored bone axes cancelled the
// left's to ~0.03 a frame and the vote latched on noise, reversing every slider. A skater's own rule
// has no bones in it: regular going forward has the toes to the RIGHT of travel; goofy, or riding
// switch, to the left.

// ------------------------------------------------------------------ the deck, from the feet
// The idle stands the back foot ON the tail and the front foot on the flat, so between them they
// say how high the kick rises above the flat and how far out that is; the truck says where the kick
// starts. The kick is then a ramp from the truck through the idle foot, and the nose -- which no
// idle foot stands on -- is its mirror image. Nothing about the deck is read off the board except
// where its trucks are, so no board socket or mesh can hand the feet a wrong shape.
struct Kick { bool ok; float hFlat, rise, run, tilt; };   // rise over run beyond the truck; tilt in rad

// THE DECK'S SHAPE IS MEASURED ONCE AND HELD, per stance. It was re-derived every frame from the two
// homes, and a kick only counted past 1.0 cm of rise: this deck measures 1.2-1.7, and the FRONT foot
// bobs through the idle (its home height drifts 13.3 -> 14.5), so the rise wandered across that line
// and the kick came and went. Each time it went, the follow's drop switched off and the back foot
// popped up to exactly where it sits with "Follow the deck" unticked -- the lift that came and went
// while rolling straight. The deck does not change shape while you ride: average it over the first
// two seconds of settled idle, decide once, and keep it.
struct KickLatch { bool decided; Kick k; float sumRise, sumRun; int n; };
static KickLatch s_kick[5];

// ------------------------------------------------------------------ the learned idle, kept across launches
// The homes and the deck come out the same every ride (the animation's own idle), so once learned
// they are saved and loaded at startup: the stance applies from the first push instead of after a
// learn. In hundredths of a cm. Forgotten only on a goofy/regular change or F1's "Re-learn my idle".
static int          g_homeGoofy = -1;           // goofy when the homes were learned; -1 unknown
static volatile LONG g_relearn = 0;             // F1 asked for a fresh learn (done on the game thread)
static float        s_savedHome[5][2][2];       // what the ini holds, so a save happens only on a real change
static const int    kLearnSt[2] = { 1, 4 };     // the stances you roll in: regular, switch
static const int    kNone = -1000000;

static int Hund(float v) { return (int)floorf(v * 100.0f + 0.5f); }

// ESkateboardMovementMode, from the reflected names in order: 0 None, 1 Skateboarding, 2 Grinding,
// 3 Falling, 4 ToOnFoot, 5 OnFoot, 6 Throwdown, 7 ToSkateboarding, 8 Bailing, 9 Replay, 10 Intro.
enum { BM_SKATEBOARDING = 1, BM_FALLING = 3, BM_THROWDOWN = 6, BM_TO_SKATEBOARDING = 7 };
static int BoardMode(void* a) {
    void* sk = a ? twkP(a, AN_SKATER) : nullptr;
    void* bd = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* mv = bd ? twkP(bd, BOARD_MOVE) : nullptr;
    return mv ? twkB(mv, BM_MODE) : -1;
}

static void LoadLearned(const char* buf) {
    g_homeGoofy = TwkIniInt(buf, "StanceHomeGoofy", -1);
    int feet = 0, decks = 0;
    for (int st : kLearnSt) {
        for (int f = 0; f < 2; f++) {
            char kx[40], kh[40];
            snprintf(kx, sizeof(kx), "StanceHome%d%cX", st, f ? 'R' : 'L');
            snprintf(kh, sizeof(kh), "StanceHome%d%cH", st, f ? 'R' : 'L');
            const int x = TwkIniInt(buf, kx, kNone), h = TwkIniInt(buf, kh, kNone);
            const bool ok = x > -10000 && x < 10000 && h > -10000 && h < 10000;
            s_home[st][f] = Home{ ok, x * 0.01f, h * 0.01f };
            s_savedHome[st][f][0] = s_home[st][f].x; s_savedHome[st][f][1] = s_home[st][f].h;
            feet += ok;
        }
        char kk[40], kr[40], kn[40], kf[40];
        snprintf(kk, sizeof(kk), "StanceKick%d", st);
        snprintf(kr, sizeof(kr), "StanceKick%dRise", st);
        snprintf(kn, sizeof(kn), "StanceKick%dRun", st);
        snprintf(kf, sizeof(kf), "StanceKick%dFlat", st);
        const int kok = TwkIniInt(buf, kk, -1);
        s_kick[st] = KickLatch{};
        if (kok >= 0) {
            KickLatch& kl = s_kick[st];
            kl.decided = true;
            kl.k = Kick{ false, 0.0f, 0.0f, 1.0f, 0.0f };
            const float rise = TwkIniInt(buf, kr, 0) * 0.01f, run = TwkIniInt(buf, kn, 0) * 0.01f;
            if (kok == 1 && rise >= 0.5f && run >= 2.0f)
                kl.k = Kick{ true, TwkIniInt(buf, kf, 0) * 0.01f, rise, run, atan2f(rise, run) };
            decks++;
        }
    }
    // 491: the switch home crawled (see LEARNED ONCE) and was saved; forget it once, it re-learns from the
    // next still switch idle.
    if (!TwkIniInt(buf, "StanceSwitchHomeReset491", 0) && (s_home[4][0].ok || s_home[4][1].ok)) {
        s_home[4][0].ok = s_home[4][1].ok = false;
        TwkLog("[stance] your switch idle is re-learned once (the saved one had crept away from the game's): roll in switch for a second");
        TwkMarkDirty();
    }
    if (feet || decks)
        TwkLog("[stance] loaded your learned idle: main stance %s (x %.1f / %.1f), switch %s, deck %s -- no learn needed",
               s_home[1][0].ok && s_home[1][1].ok ? "known" : "not yet", s_home[1][0].x, s_home[1][1].x,
               s_home[4][0].ok && s_home[4][1].ok ? "known" : "not yet", decks ? "known" : "not yet");
}

static void SaveLearned(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "StanceHomeGoofy", g_homeGoofy);
    TwkIniSetInt(buf, cap, "StanceSwitchHomeReset491", 1);
    for (int st : kLearnSt) {
        for (int f = 0; f < 2; f++) {
            char kx[40], kh[40];
            snprintf(kx, sizeof(kx), "StanceHome%d%cX", st, f ? 'R' : 'L');
            snprintf(kh, sizeof(kh), "StanceHome%d%cH", st, f ? 'R' : 'L');
            const Home hm = s_home[st][f];
            TwkIniSetInt(buf, cap, kx, hm.ok ? Hund(hm.x) : kNone);
            TwkIniSetInt(buf, cap, kh, hm.ok ? Hund(hm.h) : kNone);
            s_savedHome[st][f][0] = hm.x; s_savedHome[st][f][1] = hm.h;
        }
        char kk[40], kr[40], kn[40], kf[40];
        snprintf(kk, sizeof(kk), "StanceKick%d", st);
        snprintf(kr, sizeof(kr), "StanceKick%dRise", st);
        snprintf(kn, sizeof(kn), "StanceKick%dRun", st);
        snprintf(kf, sizeof(kf), "StanceKick%dFlat", st);
        const KickLatch kl = s_kick[st];
        TwkIniSetInt(buf, cap, kk, kl.decided ? (kl.k.ok ? 1 : 0) : -1);
        TwkIniSetInt(buf, cap, kr, Hund(kl.k.rise));
        TwkIniSetInt(buf, cap, kn, Hund(kl.k.run));
        TwkIniSetInt(buf, cap, kf, Hund(kl.k.hFlat));
    }
}

static void ForgetLearned(bool deckToo) {
    for (int k = 0; k < 5; k++) {
        s_home[k][0].ok = s_home[k][1].ok = false;
        if (deckToo) s_kick[k] = KickLatch{};
    }
    ForgetRefs();                                   // they are per physical foot and stance, like the homes
    TwkMarkDirty();
}

// The deck's height and surface tilt at a point along it. Flat inside the trucks; beyond them the
// ramp, carried on past the idle foot (a foot pushed further out keeps climbing, to a limit).
static void DeckAt(const Ride& rd, const Kick& k, float x, float& h, float& tilt) {
    h = k.hFlat; tilt = 0.0f;
    if (!k.ok) return;
    const float over = fabsf(x) - rd.truck[x >= 0.0f ? 1 : 0];
    if (over <= 0.0f) return;
    const float u = clampF(over / k.run, 0.0f, 1.8f);
    h = k.hFlat + k.rise * u;
    tilt = k.tilt * (u < 1.0f ? u : 1.0f);            // eases in from flat at the truck
}

// That surface's normal, in MESH space: tipped back toward the middle of the board by the tilt.
static void DeckNormal(const Ride& rd, float x, float tilt, float out[3]) {
    const float s = (x >= 0.0f) ? 1.0f : -1.0f;
    const float nl[3] = { -s * sinf(tilt), 0.0f, rd.zs * cosf(tilt) };
    float w[3]; TwkQuatRotate(rd.qf, nl, w);
    TwkQuatInvRotate(rd.qm, w, out);
}

// ------------------------------------------------------------------ our own rotation, added and removed
// Composed as a quaternion in the socket's parent frame, and unwound before the next one goes on when
// the animation did not recompute the socket -- otherwise the foot winds a little further every
// frame. (foot_steer owns the air and this module the ride, so they never both hold one.)
static float g_wrote[2][3] = {}, g_twist[2][4] = {};
static bool  g_held[2] = { false, false };

static void ApplyExtraRot(void* a, int off, int idx, const float tw[4]) {
    const bool wantZero = (tw[3] > 0.99999f) || (tw[3] < -0.99999f);   // identity: nothing to add
    if (wantZero && !g_held[idx]) return;
    float cur[3] = { twkF(a, off), twkF(a, off + 4), twkF(a, off + 8) };
    if (cur[0] < -1e5f || cur[1] < -1e5f || cur[2] < -1e5f) return;
    float q[4]; TwkRotatorToQuat(cur, q);
    if (g_held[idx] && fabsf(cur[0] - g_wrote[idx][0]) < 1e-3f
                    && fabsf(cur[1] - g_wrote[idx][1]) < 1e-3f
                    && fabsf(cur[2] - g_wrote[idx][2]) < 1e-3f) {
        const float inv[4] = { -g_twist[idx][0], -g_twist[idx][1], -g_twist[idx][2], g_twist[idx][3] };
        float undone[4]; TwkQuatMul(inv, q, undone);
        for (int i = 0; i < 4; i++) q[i] = undone[i];
    }
    if (wantZero) {
        float r[3]; TwkQuatToRotator(q, r);
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)a + off + i * 4) = r[i];
        g_held[idx] = false;
        return;
    }
    float out[4]; TwkQuatMul(tw, q, out);
    float r[3]; TwkQuatToRotator(out, r);
    for (int i = 0; i < 3; i++) { *(float*)((uint8_t*)a + off + i * 4) = r[i]; g_wrote[idx][i] = r[i]; }
    for (int i = 0; i < 4; i++) g_twist[idx][i] = tw[i];
    g_held[idx] = true;
}

// BEFORE the game's own update runs. UpdateFootAnchors does not recompute the socket rotation from
// scratch: it slerps FROM last frame's socket value toward its new target,
//     q = Slerp(LeftFootSocketRotation.Quaternion(), target, _footLeftIKSmoothing)
// so a rotation of ours still sitting in the socket is fed back into that slerp, and composing ours
// again on top of the result settles near our angle DIVIDED by the smoothing alpha -- a few degrees
// of deck follow grew into the foot rolling onto its side. Taking ours back out here hands the game
// its own previous value, exactly as if we had never been there.
void Stance_PreAnchors(void* a) {
    if (!a) return;
    const int off[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
    for (int f = 0; f < 2; f++) {
        if (!g_held[f]) continue;
        const float cur[3] = { twkF(a, off[f]), twkF(a, off[f] + 4), twkF(a, off[f] + 8) };
        g_held[f] = false;                          // either way, nothing of ours is in it after this
        if (fabsf(cur[0] - g_wrote[f][0]) >= 1e-3f || fabsf(cur[1] - g_wrote[f][1]) >= 1e-3f
                                                   || fabsf(cur[2] - g_wrote[f][2]) >= 1e-3f) continue;
        float q[4]; TwkRotatorToQuat(cur, q);
        const float inv[4] = { -g_twist[f][0], -g_twist[f][1], -g_twist[f][2], g_twist[f][3] };
        float own[4]; TwkQuatMul(inv, q, own);
        float r[3]; TwkQuatToRotator(own, r);
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)a + off[f] + i * 4) = r[i];
    }
}

static void ReleaseRotations(void* a) {
    if (!a || (!g_held[0] && !g_held[1])) return;
    const float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ApplyExtraRot(a, AN_L_SOCK_ROT, 0, none);
    ApplyExtraRot(a, AN_R_SOCK_ROT, 1, none);
}

// ------------------------------------------------------------------ the drawn skeleton
// The skeleton's last FINISHED pose (the read buffer: last frame's, every anim node applied): the foot,
// ball (toe joint) and toe-tip bones per foot, for the toe fix and the landing reference.
struct DrawnFeet { void* skel; int foot[2], ball[2], tip[2]; bool ok; float qRef[2][4]; };
static DrawnFeet s_drawn = {};
static bool DrawnResolve(void* mesh) {
    void* skel = twkP(mesh, SKM_MESH);
    if (!skel) return false;
    if (skel == s_drawn.skel) return s_drawn.ok;
    // A FAILED resolve is NOT kept (497): at spawn the bone names can still be unreadable (the 496 session's
    // first resolve, at the spawn "landing", found 95 bones and no names), and caching that turned off every
    // drawn-pose feature -- the toe fixes, the switch tilt, the ball reference -- for the whole session
    // ("you just ruined switch landing"). Retried once a second until the names read.
    static ULONGLONG s_retryAt = 0;
    const ULONGLONG nowMs = GetTickCount64();
    if (nowMs < s_retryAt) return false;
    s_drawn = DrawnFeet{ skel, { -1, -1 }, { -1, -1 }, { -1, -1 }, false, {} };
    const uint8_t* rs = (const uint8_t*)skel + SM_REFSKEL;
    const uint8_t* info = *(const uint8_t* const*)(rs + RS_FINAL_INFO);
    const int n = *(const int*)(rs + RS_FINAL_INFO + 8);
    if (!info || n <= 0 || n > 400) return false;
    static char names[400][48]; static int parent[400];
    for (int i = 0; i < n; i++) {
        char nb[96];
        if (!GrindPop_FNameToString(info + i * 12, nb, sizeof(nb))) nb[0] = 0;
        for (char* c = nb; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        snprintf(names[i], sizeof(names[i]), "%s", nb);
        parent[i] = *(const int*)(info + i * 12 + 8);
    }
    for (int sd = 0; sd < 2; sd++) {
        // The skater's are amxx_l_foot > amxx_l_toe0 > amxx_l_toe0_end (477's log); the board rig's
        // skateskel_foot_anchor_* are not feet.
        for (int i = 0; i < n; i++) {
            const char* nm = names[i];
            const size_t l = strlen(nm);
            const bool side = sd ? (strstr(nm, "_r_") || (l > 2 && !strcmp(nm + l - 2, "_r")))
                                 : (strstr(nm, "_l_") || (l > 2 && !strcmp(nm + l - 2, "_l")));
            if (side && strstr(nm, "foot") && !strstr(nm, "anchor") && !strstr(nm, "_end")) { s_drawn.foot[sd] = i; break; }
        }
        for (int i = 0; i < n && s_drawn.foot[sd] >= 0; i++)
            if (parent[i] == s_drawn.foot[sd] && (strstr(names[i], "ball") || strstr(names[i], "toe"))) { s_drawn.ball[sd] = i; break; }
        for (int i = 0; i < n && s_drawn.ball[sd] >= 0; i++)
            if (parent[i] == s_drawn.ball[sd]) { s_drawn.tip[sd] = i; break; }
    }
    s_drawn.ok = s_drawn.foot[0] >= 0 && s_drawn.foot[1] >= 0 && s_drawn.ball[0] >= 0 && s_drawn.ball[1] >= 0;
    // The toe joint at rest (its parent-relative rotation in the reference pose): the bend is measured off it.
    const float* ref = *(const float* const*)(rs + RS_FINAL_POSE);
    const int nRef = *(const int*)(rs + RS_FINAL_POSE + 8);
    for (int sd = 0; sd < 2; sd++) {
        s_drawn.qRef[sd][3] = 1.0f;
        if (ref && s_drawn.ball[sd] >= 0 && s_drawn.ball[sd] < nRef)
            for (int i = 0; i < 4; i++) s_drawn.qRef[sd][i] = ref[s_drawn.ball[sd] * 12 + i];
    }
    char kids[512] = ""; size_t k = 0;
    for (int i = 0; i < n && k < sizeof(kids) - 60; i++)
        if (strstr(names[i], "foot") || strstr(names[i], "ball") || strstr(names[i], "toe"))
            k += snprintf(kids + k, sizeof(kids) - k, "%s%s(%d<-%d)", k ? " " : "", names[i], i, parent[i]);
    static int s_saidFail = 0;
    if (s_drawn.ok || s_saidFail < 5) {
        if (!s_drawn.ok) s_saidFail++;
        TwkLog("[toes] skeleton %d bones: foot %d/%d, ball %d/%d, tip %d/%d%s | %s", n, s_drawn.foot[0], s_drawn.foot[1],
               s_drawn.ball[0], s_drawn.ball[1], s_drawn.tip[0], s_drawn.tip[1], s_drawn.ok ? "" : " -- NOT RESOLVED yet, retrying", kids);
    }
    if (!s_drawn.ok) { s_drawn.skel = nullptr; s_retryAt = nowMs + 1000; }
    return s_drawn.ok;
}
// THE BALL OF THE FOOT (495) is what stands on the griptape, not the ankle: how far below the ankle it sits
// depends on the foot's tilt. Every landing sets its balls to ONE reference (506): the REGULAR riding front
// ball as the game draws it -- a ball planted on the flat -- with our stance's share taken out, so no slider
// moves it. 495-505 kept two, off the drawn pose WITH the stance: switch/fakie landings used the switch
// riding ball, which in the game's own pose sits ~1 cm high (the user's -5 deg switch front pitch had been
// hiding it): a stance at zero landed switch "slightly floating". And a slider changed after the learn left
// the reference stale.
static float s_regBall = 0.0f, s_regBallSum = 0.0f, s_regBallOurs = 0.0f;
static int   s_regBallN = 0;
static bool  s_regBallOk = false;
static float s_ourBallDh[2] = { 0.0f, 0.0f };   // our stance's share of each foot's drawn ball height, last frame
// ...but the share does not cover it (507): the user's stance reads 3.44 cm, the same feet at zero 4.14, with
// nothing of ours to take out -- something past the socket moves the drawn foot with where the stance puts
// it. So the reference is re-learned whenever the main stance's sliders change (the old one stays in use
// until the new one is in).
static int   s_regBallKey[S_N + 1];
static bool  s_regBallKeySet = false, s_regBallPending = false;
static bool DrawnBallH(void* a, const Ride& rd, int f, float& out) {
    void* sk = twkP(a, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    if (!mesh || !DrawnResolve(mesh)) return false;
    const int idx = *(const int*)((const uint8_t*)mesh + SKM_READ);
    if (idx < 0 || idx > 1) return false;
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + idx * 0x10;
    const float* data = *(const float* const*)arr;
    const int n = *(const int*)(arr + 8);
    if (!data || s_drawn.ball[f] < 0 || s_drawn.ball[f] >= n) return false;
    const float* P = data + s_drawn.ball[f] * 12 + 4;
    const float pm[3] = { P[0], P[1], P[2] };
    float x; ToDeck(rd, pm, x, out);
    return out > -100.0f && out < 100.0f;
}

// ------------------------------------------------------------------ the toes in nollie and fakie
// The front of the shoe bends at amxx_*_toe0, a joint the foot socket does not drive: the animation owns
// it. The game's nollie riding pose bends the BACK foot's toes 13-18 deg off rest where the regular idle
// has 3-6 (480 log; the front ~9 in both) -- "the front of the shoe/toes bending up", worst once our
// landing has that foot on the flat over the bolts. So while rolling in nollie or fakie (the landing
// included), each toe joint is set to the bend the rider's own REGULAR idle has, learned off the drawn
// pose, in the finished pose (the FlipEditableSpaceBases seam sit and emote write, called from sit.cpp).
// Setups, flips, catches and grinds keep the game's toes.
// Two references (493): [0] the REGULAR idle's (nollie rolling), [1] the SWITCH idle's (switch landings).
static float           s_toeIdle[2][2][4];               // [set][physical foot]: the toe joint, parent-relative
static bool            s_toeIdleOk[2][2] = {};
static float           s_toeSum[2][2][4] = {};           // ...averaged over their first 30 clean samples, then fixed (498)
static int             s_toeN[2] = { 0, 0 };
static volatile int    s_toeSet = 0;                     // which reference ToeApply uses
// THE FOOT'S OWN ANKLE->BALL LINE (493), in the foot bone's frame -- the foot is rigid, and the drawn foot
// IS the socket rotation (478), so the socket's rotation of this vector is where the toes point. Its rise
// above the deck is the foot's toe-down angle: the switch riding pose -43 / -37 deg (front / back), the
// switch LANDING -34 / -34 at +0.1 s (492 log) -- toes up, what floats.
static float           s_abLocal[2][3];
static bool            s_abLocalOk[2] = { false, false };
static volatile float  s_toeW = 0.0f;                    // how far the toes are ours (eased)
static void* volatile  s_toeMesh = nullptr;
static volatile ULONGLONG s_toeMs = 0;

static bool ToeLocal(const float* data, int f, float out[4]) {     // parent-relative toe joint, drawn
    const float* qf = data + s_drawn.foot[f] * 12;
    const float* qt = data + s_drawn.ball[f] * 12;
    const float inv[4] = { -qf[0], -qf[1], -qf[2], qf[3] };
    TwkQuatMul(inv, qt, out);
    const float n = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (!(n > 0.5f && n < 1.5f)) return false;
    for (int i = 0; i < 4; i++) out[i] /= n;
    return true;
}

// From Stance_AddOffset, every frame: learn the idles' toes, decide whether the toes are ours.
// landToes: which reference a landing's toes go to -- 1 switch (switch/fakie landings, 493), 0 regular
// (a regular landing out of a 180, 502), -1 none.
static void ToeUpdate(void* a, void* mesh, int stRaw, bool idleClean, bool riding, float dt, bool animOv, int landToes) {
    static int s_said = 0;
    if (!mesh || !DrawnResolve(mesh)) { s_toeW = 0.0f; return; }
    const int idx = *(const int*)((const uint8_t*)mesh + SKM_READ);
    if (idx < 0 || idx > 1) return;
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + idx * 0x10;
    const float* data = *(const float* const*)arr;
    const int n = *(const int*)(arr + 8);
    if (!data || s_drawn.ball[0] >= n || s_drawn.ball[1] >= n) return;
    // The foot's ankle->ball line in its own frame, once (rigid).
    for (int f = 0; f < 2; f++) {
        if (s_abLocalOk[f]) continue;
        const float* qf = data + s_drawn.foot[f] * 12;
        const float* pf = qf + 4;
        const float* pb = data + s_drawn.ball[f] * 12 + 4;
        const float v[3] = { pb[0] - pf[0], pb[1] - pf[1], pb[2] - pf[2] };
        const float inv[4] = { -qf[0], -qf[1], -qf[2], qf[3] };
        TwkQuatRotate(inv, v, s_abLocal[f]);
        const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        s_abLocalOk[f] = l > 2.0f && l < 40.0f;
    }
    // Learned in the regular (set 0) and switch (set 1) rolling idles, never off a pose of ours. LEARNED ONCE
    // (498): averaged over the first 30 clean samples, then fixed and saved -- never tracking the live
    // animation (the rule from 491's crawl).
    const int learnSet = stRaw == 1 ? 0 : stRaw == 4 ? 1 : -1;
    if (learnSet >= 0 && idleClean && s_toeW < 0.001f && !(s_toeIdleOk[learnSet][0] && s_toeIdleOk[learnSet][1])) {
        float q0[4], q1[4];
        if (ToeLocal(data, 0, q0) && ToeLocal(data, 1, q1)) {
            const float* qs[2] = { q0, q1 };
            for (int f = 0; f < 2; f++) {
                float* sm = s_toeSum[learnSet][f];
                const float d = qs[f][0]*sm[0] + qs[f][1]*sm[1] + qs[f][2]*sm[2] + qs[f][3]*sm[3];
                const float sg = (s_toeN[learnSet] && d < 0.0f) ? -1.0f : 1.0f;
                for (int i = 0; i < 4; i++) sm[i] += qs[f][i] * sg;
            }
            if (++s_toeN[learnSet] >= 30) {
                for (int f = 0; f < 2; f++) {
                    float* sm = s_toeSum[learnSet][f];
                    const float m = sqrtf(sm[0]*sm[0] + sm[1]*sm[1] + sm[2]*sm[2] + sm[3]*sm[3]);
                    for (int i = 0; i < 4; i++) { s_toeIdle[learnSet][f][i] = m > 1e-6f ? sm[i] / m : (i == 3 ? 1.0f : 0.0f); sm[i] = 0.0f; }
                    s_toeIdleOk[learnSet][f] = true;
                }
                s_toeN[learnSet] = 0;
                TwkMarkDirty();                           // kept for the next launch
                if (s_said < 4) { s_said++; TwkLog("[toes] learned your %s idle's toe joints (kept between launches)", learnSet ? "switch" : "regular"); }
            }
        }
    }
    const bool trick = twkB(a, AN_TRICK_PEND) || twkB(a, AN_CRANKING) || twkB(a, AN_QUICK_SWAP) || twkB(a, AN_FLIPPING) ||
                       twkB(a, AN_ROTATING) || twkB(a, AN_CATCH_ORIENT) || twkB(a, AN_GRINDING) || twkB(a, AN_LIPTRICK);
    // Nollie rolling (491) to the regular idle's; a SWITCH LANDING (493) to the switch idle's -- its clip bends
    // the toes up 17-21 deg where switch riding has 11-12 (492 log, +0.1 s).
    // ...and a REGULAR landing out of a 180 (502) to the regular idle's: a fakie 180 touches down regular
    // through the game's transfer animation, which bends the back foot's toes 15-17 deg where regular riding
    // has 3-6 (501 log) -- "that same old toe curl issue that nollie's used to have".
    const bool nollie = stRaw == 3 && !animOv && s_toeIdleOk[0][0] && s_toeIdleOk[0][1];
    const bool landT  = landToes >= 0 && s_toeIdleOk[landToes][0] && s_toeIdleOk[landToes][1];
    const bool want = g_on && riding && (nollie || landT) && !trick && twkB(a, AN_ON_BOARD) == 1 && twkB(a, AN_GROUNDED) == 1;
    if (want) s_toeSet = landT ? landToes : 0;
    float w = s_toeW;
    w += ((want ? 1.0f : 0.0f) - w) * (1.0f - expf(-dt / 0.12f));
    if (w < 0.001f) w = 0.0f;
    static bool s_was = false;
    if (want != s_was) {
        s_was = want;
        static int s_n = 0;
        if (s_n < 60) {
            s_n++;
            float b[2] = { 0.0f, 0.0f };
            for (int f = 0; f < 2; f++) {
                float q[4];
                if (!ToeLocal(data, f, q)) continue;
                const float* t = s_toeIdle[s_toeSet][f];
                b[f] = 2.0f * acosf(fminf(1.0f, fabsf(q[0]*t[0] + q[1]*t[1] + q[2]*t[2] + q[3]*t[3]))) * 57.2957795f;
            }
            TwkLog(want ? "[toes] %s: the toe joints go to your %s idle's (left %.1f, right %.1f deg off it now)"
                        : "[toes] %s over: the toes are the game's again%s (left %.1f, right %.1f deg off your idle's)",
                   landT ? (s_toeSet ? "switch landing" : "180 landing") : "rolling nollie", want ? (s_toeSet ? "switch" : "regular") : "", b[0], b[1]);
        }
    }
    s_toeW = w; s_toeMesh = mesh; s_toeMs = GetTickCount64();
}

// The pose seam: the toe joint to the learned bend, by the weight; its tip bone carried with it.
static void ToeApply(void* mesh, float w) {
    if (twkP(mesh, SKM_MESH) != s_drawn.skel || !s_drawn.ok) return;
    const int idx = *(const int*)((const uint8_t*)mesh + SKM_EDIT);
    if (idx < 0 || idx > 1) return;
    const uint8_t* arr = (const uint8_t*)mesh + SKM_CST + idx * 0x10;
    float* data = *(float* const*)arr;
    const int n = *(const int*)(arr + 8);
    if (!data) return;
    for (int f = 0; f < 2; f++) {
        const int b = s_drawn.foot[f], t = s_drawn.ball[f], e = s_drawn.tip[f];
        if (b < 0 || t < 0 || b >= n || t >= n || e >= n) continue;
        float loc[4];
        if (!ToeLocal(data, f, loc)) continue;
        const float* ti = s_toeIdle[s_toeSet][f];
        float tg[4] = { ti[0], ti[1], ti[2], ti[3] };
        if (loc[0]*tg[0] + loc[1]*tg[1] + loc[2]*tg[2] + loc[3]*tg[3] < 0.0f) for (int i = 0; i < 4; i++) tg[i] = -tg[i];
        float m[4], mn = 0.0f;
        for (int i = 0; i < 4; i++) { m[i] = loc[i] + (tg[i] - loc[i]) * w; mn += m[i] * m[i]; }
        mn = sqrtf(mn);
        if (!(mn > 1e-6f)) continue;
        for (int i = 0; i < 4; i++) m[i] /= mn;
        float* qT = data + t * 12;
        const float qOld[4] = { qT[0], qT[1], qT[2], qT[3] };
        float qNew[4]; TwkQuatMul(data + b * 12, m, qNew);
        // The tip rides the toe: its offset and turn in the toe's old frame, put back in the new one.
        if (e >= 0) {
            float* E = data + e * 12;
            const float inv[4] = { -qOld[0], -qOld[1], -qOld[2], qOld[3] };
            const float d[3] = { E[4] - qT[4], E[5] - qT[5], E[6] - qT[6] };
            float lp[3]; TwkQuatRotate(inv, d, lp);
            float lq[4]; TwkQuatMul(inv, E, lq);
            float np[3]; TwkQuatRotate(qNew, lp, np);
            float nq[4]; TwkQuatMul(qNew, lq, nq);
            for (int i = 0; i < 3; i++) E[4 + i] = qT[4 + i] + np[i];
            for (int i = 0; i < 4; i++) E[i] = nq[i];
        }
        for (int i = 0; i < 4; i++) qT[i] = qNew[i];
    }
}

void Stance_OnFlip(void* mesh) {
    const float w = s_toeW;
    if (!mesh || mesh != s_toeMesh || w <= 0.0f) return;
    if (GetTickCount64() - s_toeMs > 200) return;             // nobody is driving it (replays, editors)
    __try { ToeApply(mesh, w > 1.0f ? 1.0f : w); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s_toeMesh = nullptr; s_toeW = 0.0f; TwkLog("[toes] fault in the pose -- the toes stay the game's"); }
}

// The foot's toe-down angle above the deck (deg; the ankle->ball line's rise) from the SOCKET rotation --
// the game's, ours having been taken out in Stance_PreAnchors.
static bool AbRise(void* a, const Ride& rd, int f, float& out) {
    if (!s_abLocalOk[f]) return false;
    const int off = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
    const float r[3] = { twkF(a, off), twkF(a, off + 4), twkF(a, off + 8) };
    if (!(r[0] > -1e5f && r[1] > -1e5f && r[2] > -1e5f)) return false;
    float q[4]; TwkRotatorToQuat(r, q);
    float v[3]; TwkQuatRotate(q, s_abLocal[f], v);
    const float up = v[0]*rd.up[0] + v[1]*rd.up[1] + v[2]*rd.up[2];
    const float al = v[0]*rd.along[0] + v[1]*rd.along[1] + v[2]*rd.along[2];
    const float ac = v[0]*rd.across[0] + v[1]*rd.across[1] + v[2]*rd.across[2];
    out = atan2f(up, sqrtf(al * al + ac * ac)) * 57.2957795f;
    return true;
}
static float s_abIdle[2] = { 0.0f, 0.0f };   // the switch riding pose's toe-down angle, per physical foot
static float s_abSum[2] = { 0.0f, 0.0f };
static int   s_abN = 0;
static bool  s_abIdleOk = false;

// ------------------------------------------------------------------ the landing references, kept (498)
// The toe joints (regular / switch idle) and the switch landing's tilt + ball: learned once, then saved
// like the homes, so switch need not be ridden each launch before a switch landing is right. Forgotten
// with them (goofy/regular changed, or "Re-learn my idle").
static void ForgetRefs() {
    for (int t = 0; t < 2; t++) { s_toeIdleOk[t][0] = s_toeIdleOk[t][1] = false; s_toeN[t] = 0; }
    for (int t = 0; t < 2; t++) for (int f = 0; f < 2; f++) for (int i = 0; i < 4; i++) s_toeSum[t][f][i] = 0.0f;
    s_abIdleOk = false; s_abN = 0; s_abSum[0] = s_abSum[1] = 0.0f;
    s_regBallOk = false; s_regBallN = 0; s_regBallSum = 0.0f; s_regBallOurs = 0.0f; s_regBallPending = false;
}
static void LoadRefs(const char* buf) {
    int toes = 0;
    for (int t = 0; t < 2; t++)
        for (int f = 0; f < 2; f++) {
            int v[4]; bool ok = true;
            for (int i = 0; i < 4; i++) {
                char k[40]; snprintf(k, sizeof(k), "StanceToe%d%c%c", t, f ? 'R' : 'L', "XYZW"[i]);
                v[i] = TwkIniInt(buf, k, kNone);
                if (v[i] == kNone) ok = false;
            }
            float q[4], m = 0.0f;
            for (int i = 0; i < 4; i++) { q[i] = v[i] * 0.0001f; m += q[i] * q[i]; }
            ok = ok && m > 0.8f && m < 1.2f;
            if (ok) { m = sqrtf(m); for (int i = 0; i < 4; i++) s_toeIdle[t][f][i] = q[i] / m; }
            s_toeIdleOk[t][f] = ok;
            toes += ok;
        }
    const int ab0 = TwkIniInt(buf, "StanceSwToeDownL", kNone), ab1 = TwkIniInt(buf, "StanceSwToeDownR", kNone);
    s_abIdleOk = ab0 != kNone && ab1 != kNone;
    if (s_abIdleOk) { s_abIdle[0] = ab0 * 0.01f; s_abIdle[1] = ab1 * 0.01f; }
    // A new key (506): StanceRegBall / StanceSwBall were read off the drawn pose WITH the stance -- not reused.
    const int rb = TwkIniInt(buf, "StanceLandBall", kNone);
    s_regBallOk = rb != kNone;
    if (s_regBallOk) s_regBall = rb * 0.01f;
    if (toes || s_abIdleOk)
        TwkLog("[stance] loaded your landing references: toe joints regular %s, switch %s; switch landing %s -- no riding needed",
               s_toeIdleOk[0][0] && s_toeIdleOk[0][1] ? "known" : "not yet", s_toeIdleOk[1][0] && s_toeIdleOk[1][1] ? "known" : "not yet",
               s_abIdleOk ? "known" : "not yet (ride switch once)");
    if (s_regBallOk) TwkLog("[stance] landing reference: the game's regular riding front ball at %.2f cm", s_regBall);
}
static void SaveRefs(char* buf, size_t cap) {
    for (int t = 0; t < 2; t++)
        for (int f = 0; f < 2; f++)
            for (int i = 0; i < 4; i++) {
                char k[40]; snprintf(k, sizeof(k), "StanceToe%d%c%c", t, f ? 'R' : 'L', "XYZW"[i]);
                TwkIniSetInt(buf, cap, k, s_toeIdleOk[t][f] ? (int)floorf(s_toeIdle[t][f][i] * 10000.0f + 0.5f) : kNone);
            }
    TwkIniSetInt(buf, cap, "StanceSwToeDownL", s_abIdleOk ? Hund(s_abIdle[0]) : kNone);
    TwkIniSetInt(buf, cap, "StanceSwToeDownR", s_abIdleOk ? Hund(s_abIdle[1]) : kNone);
    TwkIniSetInt(buf, cap, "StanceLandBall",   s_regBallOk ? Hund(s_regBall) : kNone);
}

// ------------------------------------------------------------------ the deck leans on its trucks (511)
// "Landing feels very flat ... when doing something off something higher shouldn't we get some lean on the
// board/trucks?" Off a DROP -- the board touching down lower than it took off: a ledge, stairs, a rail, rolling
// off something -- the deck leans on its trucks for a moment and rebounds. It is a roll added to the game's
// own truck-lean read (the carve's: ApplyForceOnBoard -> GetLocalAnimatorBoardQuat, hooked in grind_rock), so
// the trucks' tightness and the game's own follow shape it. Toward the side the feet came down on (the landing
// spot's across: toes or heels; either at random when it has none), sized by the drop and the trucks' own max
// lean. Flat ground stays flat: nothing under kLeanDropMin.
static const float kLeanDropMin = 15.0f, kLeanDropFull = 150.0f;   // cm the board lands below its takeoff
static const float kLeanPeriod = 0.36f, kLeanDecay = 0.11f, kLeanEnd = 0.6f, kLeanPeak = 0.5025f;
static float s_takeoffZ = 0.0f, s_apexZ = 0.0f;
static bool  s_takeoffOk = false;
static bool  s_impPending = false;
static float s_impAmount = 0.0f, s_impDrop = 0.0f, s_impGame = 0.0f, s_impWait = 0.0f;
static float s_impT = 1e9f, s_impAmp = 0.0f, s_impWall = 0.0f;
static volatile float s_impRoll = 0.0f;        // read by the board's rotation read (grind_rock)
static void* volatile s_impBmc = nullptr;
static float s_impToeY = 0.0f, s_impEdge0 = 0.0f, s_impEdgeMax = 0.0f;   // the deck's toe edge, for the log
// Peak 1 at 62 ms, a 19% rebound at 0.24 s, gone by kLeanEnd.
static float LeanShape(float t) {
    if (t < 0.0f || t >= kLeanEnd) return 0.0f;
    return expf(-t / kLeanDecay) * sinf(6.2831853f / kLeanPeriod * t) / kLeanPeak;
}
float Stance_LandingRoll(void* bmc) { return (bmc && bmc == s_impBmc) ? s_impRoll : 0.0f; }
// The deck's toe edge above its middle (deg): the flipper's +Y rise, signed to the toes.
static float ToeEdgeDeg(const float qf[4], float toeY) {
    const float y1[3] = { 0.0f, 1.0f, 0.0f };
    float yw[3]; TwkQuatRotate(qf, y1, yw);
    return asinf(clampF(yw[2] * toeY, -1.0f, 1.0f)) * 57.2957795f;
}

// ------------------------------------------------------------------ the stance
void Stance_AddOffset(void* a, bool riding, float dt, float dL[3], float dR[3]) {
    static float s_gate = 0.0f;                    // the state gate, eased
    // The drawn pose read this frame was made from last frame's sockets: last frame's share (506). Every early
    // return below leaves this frame's at zero -- nothing of ours on the feet.
    const float ourBallPrev[2] = { s_ourBallDh[0], s_ourBallDh[1] };
    s_ourBallDh[0] = s_ourBallDh[1] = 0.0f;
    static const char* s_lastBlock = "";
    // Separate budgets: gate edges and dips are what a field report is about, and they must not be
    // crowded out by the twice-a-second samples (they were: one shared budget of 60 was gone in 30 s,
    // and everything after it went unlogged).
    static int s_said = 0, s_saidEdge = 0, s_saidDip = 0;
    if (!(dt > 0.0f && dt < 0.25f)) dt = 1.0f / 60.0f;

    const int stRaw = a ? twkB(a, AN_FOOT_POS) : 0;
    const int swRaw = a ? twkB(a, AN_IS_SWITCH) : 0;     // the game's own, before 488's write
    // The touchdown (see THE LANDING, below), read first: it decides which stance this landing is in.
    const int mode = BoardMode(a);                   // ESkateboardMovementMode, -1 unreadable
    static int s_prevMode = -1;
    const bool touchdown = s_prevMode == BM_FALLING && mode == BM_SKATEBOARDING;
    const bool gotOn = mode == BM_SKATEBOARDING && (s_prevMode == BM_THROWDOWN || s_prevMode == BM_TO_SKATEBOARDING);
    const bool takeoff = s_prevMode >= 0 && s_prevMode != BM_FALLING && mode == BM_FALLING;
    if (mode >= 0) s_prevMode = mode;
    // THE DECK LEANS ON ITS TRUCKS (511): the drop is the board's height at takeoff less its height at
    // touchdown; the lean's side is picked later this frame, where the deck's frame is known.
    {
        void* skI = a ? twkP(a, AN_SKATER) : nullptr;
        void* bdI = skI ? twkP(skI, SK_BOARD) : nullptr;
        void* flI = bdI ? twkP(bdI, BOARD_FLIPPER) : nullptr;
        float qI[4], tI[3];
        const bool zOk = flI && CompWorld(flI, qI, tI);
        if (zOk) {
            if (s_zPrevOk) s_vz += ((tI[2] - s_zPrev) / dt - s_vz) * 0.5f;
            s_zPrev = tI[2]; s_zPrevOk = true;
        } else s_zPrevOk = false;
        s_tImpact = 1e9f;
        if (zOk && skI && (mode == BM_FALLING || touchdown)) {   // the ground under the board (514)
            const float b[3] = { tI[0], tI[1], tI[2] - 800.0f };
            float hit[3];
            if (Sit_TraceSurface(skI, tI, b, hit, nullptr)) {
                const float above = tI[2] - hit[2];
                if (touchdown) { if (above > 2.0f && above < 30.0f) s_clear += (above - s_clear) * 0.3f; }
                else {
                    const float hAb = fmaxf(0.0f, above - s_clear);    // h + vz t - g t^2 / 2 = 0
                    s_tImpact = (s_vz + sqrtf(s_vz * s_vz + 2.0f * kGravity * hAb)) / kGravity;
                }
            }
        }
        if (takeoff) { s_takeoffOk = zOk; if (zOk) s_takeoffZ = s_apexZ = tI[2]; }
        if (mode == BM_FALLING && zOk && tI[2] > s_apexZ) s_apexZ = tI[2];
        if (touchdown) {
            s_fallCm = (zOk && s_takeoffOk) ? s_apexZ - tI[2] : 0.0f;          // the feet's impact (512)
            s_fallImpact = clampF((s_fallCm - kFallFlat) / (kFallFull - kFallFlat), 0.0f, 1.0f);
            s_impPending = false;
            if (zOk && s_takeoffOk && g_on && g_landLeanPct > 0) {
                const float drop = s_takeoffZ - tI[2];
                const float amt = clampF((drop - kLeanDropMin) / (kLeanDropFull - kLeanDropMin), 0.0f, 1.0f);
                if (amt > 0.0f) {
                    s_impPending = true; s_impWait = 0.0f; s_impAmount = amt; s_impDrop = drop;
                    s_impGame = a ? twkF(a, AN_LAND_DROP) : -1.0f;
                }
            }
            s_takeoffOk = false;
        }
        if (s_impPending && (s_impWait += dt) > 0.1f) s_impPending = false;   // the feet never read: no lean
        if (s_impT < kLeanEnd) {
            s_impT += dt;
            if (zOk && s_impToeY != 0.0f) {
                const float d = ToeEdgeDeg(qI, s_impToeY) - s_impEdge0;
                if (fabsf(d) > fabsf(s_impEdgeMax)) s_impEdgeMax = d;
            }
            if (mode != BM_SKATEBOARDING || s_impT >= kLeanEnd) {
                s_impT = 1e9f; s_impRoll = 0.0f;
                static int s_n = 0;
                if (s_n < 100) {
                    s_n++;
                    TwkLog("[stance] landing lean: a %.0f cm drop (the game's landing size %.2f) -> asked %+.1f deg toward the %s "
                           "(%.0f%% of your trucks' %.1f deg); the deck's toe edge moved up to %+.1f deg (+ = up)%s",
                           s_impDrop, s_impGame, s_impAmp, (s_impAmp * s_impToeY) < 0.0f ? "heels" : "toes",
                           s_impAmount * g_landLeanPct, s_impWall, s_impEdgeMax, mode != BM_SKATEBOARDING ? " -- cut short, off the ground" : "");
                }
            } else s_impRoll = s_impAmp * LeanShape(s_impT);
        }
    }
    // A NOLLIE OR FAKIE LANDING is a landing into the stance it rolls away in: the game carries a nollie
    // on to regular and a fakie on to switch within a second or two (every one in the 475-477 logs),
    // animating the feet there itself -- the back foot walked back onto the tail -- while the stance held
    // off for "riding nollie", then came in mid-step once the stance flipped ("it then does an animation
    // to move the foot back onto the tail (not our swivel) then after that it then does our swivel").
    // So from the touchdown the landing stance takes it exactly as a regular landing: over the bolts, the
    // hold, the swivel into that stance's own. Riding or setting up in nollie/fakie stays the game's: the
    // cover is only for the landing, and ends when the game gets to the stance or anything else holds off.
    static int s_landSt = 0;                         // 1 or 4 while a nollie / fakie landing is covered
    // A STANCE-CHANGING LANDING -- A 180 (501). It reads the anim's FlipTrickStanceTransferRatio (+0x30c;
    // BP-driven, reset by SetFlipTrick at every trick) at 0.90-1.00 at touchdown, a plain landing 0.00-0.01,
    // and it touches down in the stance you roll away in -- a regular 180 in FAKIE (-> switch), a switch or
    // fakie 180 in regular -- so the ordinary mapping already gives the landing the user asked for ("regular
    // and do a 180 ... the switch landing stance ... switch/fakie 180 ... regular"). 490 had given 180s no
    // landing stance at all, because everything laid over the game's TRANSFER animation fought it: 488's
    // switch override, 489's catch-at-impact height hold ("up to 12.2 cm") and the held turn ("up to 172
    // deg": the transfer keeps turning the body after touchdown). Now a 180 gets the landing, minus exactly
    // those: the animation is NOT overridden (the transfer plays), the turn is NOT held (the feet turn with
    // the body), and the transfer's own foot lift and stance-swap flag do not hold the landing off; the
    // height is 500's ball from the final foot, which does not depend on the catch.
    const bool xferLanding = touchdown && a && twkF(a, AN_STANCE_XFER) > 0.5f;
    static bool s_landXfer = false;                  // the landing now running is a 180
    if (touchdown) s_landXfer = xferLanding;
    // CATCHING INTO THE LANDING (504). "Right now it does base game catch then lands, then feet move into
    // place quickly." The 503 air log: in a flip trick the game's catch STARTS ~0.30 s after takeoff
    // (CatchOrientState set, each foot's catch info assigned at ratio 0), the flip ends ~0.38 s with one foot
    // caught (ratio 1.00) and the other at 0.07, and touchdown follows ~0.50 s with that one still blending
    // (0.15-1.00). The mid-air stance is already the landing's (nollie 3, fakie 2). So the landing starts at
    // the catch: its spot rolled then and kept through touchdown, each foot pulled onto it by the game's own
    // catch ratio -- they catch INTO the landing -- the hold still counted from touchdown. Not for a 180
    // (the transfer ratio is already high at the catch: its stance flips in the air -- that keeps 501's
    // touchdown landing) nor an ollie (no catch). The nollie/fakie animation override still waits for
    // touchdown (it switches which clip plays, not something to do mid-trick). A bail or grind ends it.
    // A GRIND OR MANUAL ORIENT IS NOT A CATCH (507). "During board orients for tailslides or bluntslides, the
    // foot is staying in the catch/land foot pose". The 506 air log: every plain catch set each foot's catch
    // type (bm +0x538 / +0x540) to Left, Right or Both (1-3); a held nosegrind orient put the front foot on the
    // NOSE (4), the tail ones the back foot on the TAIL (5). So with either foot on anything past Both, the
    // feet are the game's: no catch into the landing (and one already caught lets go), and a touchdown still
    // holding it gets no landing stance (a missed ledge landing into the orient).
    static bool s_airLand = false;
    static bool s_landSkip = false;                  // this landing is the game's: an orient was held at touchdown
    bool airStart = false, orientHeld = false;
    {
        void* skA = a ? twkP(a, AN_SKATER) : nullptr;
        void* bdA = skA ? twkP(skA, SK_BOARD) : nullptr;
        void* bmA = bdA ? twkP(bdA, BOARD_MOVE) : nullptr;
        const int tL = bmA ? twkB(bmA, BM_CATCH_L) : 0, tR = bmA ? twkB(bmA, BM_CATCH_R) : 0;
        orientHeld = tL > 3 || tR > 3;
        const bool catching = a && (twkB(a, AN_CATCH_ORIENT) != 0 || tL != 0 || tR != 0);
        if (g_airCatch && g_on && g_landOn && a && mode == BM_FALLING && !s_airLand && catching && !orientHeld &&
            (twkB(a, AN_FLIPPING) || twkB(a, AN_ROTATING)) && twkF(a, AN_STANCE_XFER) < 0.5f) {
            s_airLand = true; airStart = true;
        }
        if (s_airLand && orientHeld) {
            s_airLand = false; s_settled = true; s_landSt = 0;
            static int s_n = 0;
            if (s_n < 60) { s_n++; TwkLog("[stance] a grind/manual orient in the air (catch L %d R %d): the feet stay the game's", tL, tR); }
        }
        if (s_airLand && mode >= 0 && mode != BM_FALLING && mode != BM_SKATEBOARDING) {   // bailed, grinding...
            s_airLand = false; s_settled = true; s_landSt = 0;
        }
        if (touchdown) {
            s_landSkip = orientHeld;
            static int s_n = 0;
            if (orientHeld && s_n < 60) { s_n++; TwkLog("[stance] touchdown holding a grind/manual orient (catch L %d R %d): the game's own landing", tL, tR); }
        } else if (airStart) s_landSkip = false;
    }
    if (touchdown || airStart) {
        // Nollie into main; FAKIE into switch again (496), now that a switch landing is right (495, "working
        // perfectly"): a fakie landing gets the switch animation (488's override) and every switch landing
        // correction -- the balls at the switch riding ball's height, the switch tilt and pitch, the switch
        // toe joints. (491 had taken fakie back to the game's own while switch landings were still wrong.)
        const int to = (stRaw == 3) ? 1 : (stRaw == 2) ? 4 : 0;
        bool any = false;
        if (to) for (int i = 0; i < S_N; i++) if (g_set[to == 4 ? 1 : 0][i]) any = true;
        s_landSt = (to && g_on && g_landOn && (any || g_landFix) && !s_landSkip && s_home[to][0].ok && s_home[to][1].ok) ? to : 0;
    } else if (s_landSt && stRaw != 2 && stRaw != 3) {                  // the game got there itself
        static int s_n = 0;
        if (s_n < 60) { s_n++; TwkLog("[stance] the game reached stance %d itself (its switch flag %d)", stRaw, swRaw); }
        s_landSt = 0;
    }
    const int st = s_landSt ? s_landSt : stRaw;
    const int setIx = (st == 4) ? 1 : 0;             // switch has its own stance
    const int* S = g_set[setIx];
    bool zero = true;
    for (int i = 0; i < S_N; i++) if (S[i]) zero = false;

    // THE LANDING is the TOUCHDOWN: the board's own movement mode going Falling -> Skateboarding
    // (3 -> 1). NOT IsJustLanded's rising edge, which this used until 441: that flag rises at the POP
    // and clears ~0.08 s after touchdown (every trick in the 440 log: kickflip flag up 46.02, board
    // airborne 46.03, down 46.55, flag clear 46.63), so the landing's clock ran through the air -- on a
    // drop the hold and the whole step were over before the feet came down ("tricks off higher things
    // ... going straight into my custom stance"). Getting on (Throwdown / ToSkateboarding -> 1) is
    // not a landing: no landing stance. The feet go over the bolts (each landing its own spot around
    // them) for StanceLandHoldMs, then SWIVEL into the stance (see "stepping off the landing";
    // StanceLandStep 0 = the old glide) -- only when there is a custom stance to come back to, so a
    // stock install keeps the game's own landing.
    // LANDING TELEMETRY, one line per landing: how far the board turns against the body through it
    // (a shifty lands with them turned apart), how far the held turn went, what held the stance off.
    static struct LandMeas { bool on, tdSet, caught; float yTd, yCatch, dMin, dMax, turnMax, blocked, hMax, pMax, kMax; const char* why; } s_lm = {};
    float landW = 0.0f;                              // the glide: 1 over the bolts, easing to 0
    bool  landActive = false, landFlag = false, landUse = false;
    const float hold = g_landHoldMs * 0.001f;
    static float s_since = 1e9f;                     // seconds since touchdown
    {
        static int s_saidL = 0;
        landFlag = a && twkB(a, AN_JUST_LANDED) != 0;
        // Only when the stance can actually apply -- its idle known -- or a landing would hold the
        // first learn of the session off for nothing.
        const bool homesKnown = st >= 0 && st < 5 && s_home[st][0].ok && s_home[st][1].ok;
        // FIXED LANDING STANCE (505): a stance left at zero gets the landing too -- the game's own landing,
        // fixed (nollie on the regular clip, switch/fakie height and tilt, caught in the air, each landing its
        // own spot) -- stepping back into the game's own stance (see THE GAME'S OWN SPOT, below).
        landUse = g_on && g_landOn && (!zero || g_landFix) && homesKnown && !s_landSkip;
        if (touchdown || airStart) {
            const bool caughtInAir = touchdown && s_airLand;     // this landing began at the catch (504)
            s_since = 0.0f;
            s_settled = false; s_step[0].latched = s_step[1].latched = false;
            s_frzWant = touchdown; s_frz[0].ok = s_frz[1].ok = false;   // the turn/across hold is caught on the ground
            if (!caughtInAir) RollLanding();                            // ...but the spot is the one it caught into
            if (touchdown) {
                s_slapAtTouch = caughtInAir ? s_slapLast : 0.0f;     // the heel's lift the board came down with
                s_slapLast = 0.0f;
                const float ks = g_landImpactPct * 0.01f * (0.4f + 0.6f * s_fallImpact);
                RollKnock(ks);
                static int s_nI = 0;
                if (landUse && g_landImpactPct > 0 && s_nI < 100) {
                    s_nI++;
                    TwkLog("[stance] the feet take the landing: fell %.0f cm (impact %.2f) -> heels lifted %.1f deg in the air, %.1f still up at touchdown%s, knocked back %+.1f/%+.1f cm %+.1f deg, front %+.1f/%+.1f cm %+.1f deg (along/across/twist)",
                           s_fallCm, s_fallImpact, caughtInAir ? kSlapDeg * g_landImpactPct * 0.01f : 0.0f, -s_slapAtTouch,
                           caughtInAir ? "" : " (no catch in the air)", s_knock[0][0], s_knock[0][1], s_knock[0][2],
                           s_knock[1][0], s_knock[1][1], s_knock[1][2]);
                }
            }
            s_landType = stRaw == 4 ? LT_SW : stRaw == 3 ? LT_NOLLIE : stRaw == 2 ? LT_FAKIE : LT_REG;
            if (touchdown) s_airLand = false;
            if (airStart) {
                static int s_nA = 0;
                if (s_nA < 100) { s_nA++; TwkLog("[stance] catch in the air (stance %d): the feet catch into your landing", stRaw); }
            }
            if (touchdown) { s_lm = {}; s_lm.on = landUse; }
            if (!touchdown) {}
            else if (xferLanding && s_saidL < 100) {
                s_saidL++;
                TwkLog("[stance] touchdown from a 180 (transfer %.2f, stance %d): your %s landing over the game's transfer (turn not held, no animation override)",
                       twkF(a, AN_STANCE_XFER), stRaw, (stRaw == 2 || stRaw == 4) ? "switch" : "regular");
            } else if (landUse && s_saidL < 100) {
                s_saidL++;
                TwkLog("[stance] touchdown%s: the feet go over the bolts for %d ms (this landing: back %+.0f mm, front %+.0f mm), then %s into your %s stance",
                       stRaw == 3 ? " in NOLLIE" : stRaw == 2 ? (swRaw ? " in FAKIE (switch flag 1)" : " in FAKIE (switch flag 0)") : "",
                       g_landHoldMs, s_rand[0][0], s_rand[1][0],
                       g_landStep ? "step" : "glide", st == 4 ? "switch" : "main");
            }
        } else if (s_airLand) s_since = 0.0f;                   // caught in the air: the hold waits for touchdown
        else s_since += dt;
        if (gotOn && landUse && s_saidL < 100) { s_saidL++; TwkLog("[stance] got on the board: not a landing, no landing stance"); }
        if (landUse) {
            // The cap only guards against a step that never gets to run; it fits the longest one.
            if (g_landStep) landActive = !s_settled && s_since < hold + 4.0f;
            else {
                landW = (s_since <= hold) ? 1.0f : expf(-(s_since - hold) / (g_landReturnMs * 0.001f));
                if (landW < 0.01f) landW = 0.0f;
                landActive = landW > 0.0f;
            }
        }
    }
    // Each foot's share of the landing while it is caught in the air: the game's own catch ratio; after
    // touchdown whatever is left comes in over ~0.1 s (504).
    static float s_airW[2] = { 1.0f, 1.0f };
    {
        void* skA = a ? twkP(a, AN_SKATER) : nullptr;
        void* bdA = skA ? twkP(skA, SK_BOARD) : nullptr;
        void* bmA = bdA ? twkP(bdA, BOARD_MOVE) : nullptr;
        for (int f = 0; f < 2; f++) {
            if (s_airLand && bmA) s_airW[f] = clampF(twkF(bmA, (f ? BM_CATCH_R : BM_CATCH_L) + 4), 0.0f, 1.0f);
            else s_airW[f] = fminf(1.0f, s_airW[f] + dt / 0.12f);
        }
    }
    // The landing flag spans the air (where "in the air" holds the stance off anyway) and ~0.08 s past
    // touchdown; with the landing stance in use that moment is the landing stance's.
    const char* block = !g_on ? "off" : (!riding ? "not riding" : (a ? IdleBlocker(a, landUse, s_landSt != 0) : "no skater"));
    // The cover ends once its landing is over and anything at all holds the stance off: a nollie set up
    // again, or ridden on, is the game's.
    if (s_landSt && !landActive && block) s_landSt = 0;
    // THE ANIMATION LANDS IN THE STANCE YOU ROLL AWAY IN (487 nollie -> regular, CONFIRMED "works perfect";
    // 488 fakie -> switch, which the animation also keys on its IsSkatingSwitch copy +0x303). Our landing owns along, across, the turn (and for a
    // while the height) but the tilt and the rest of the leg are the animation's -- and under a nollie
    // landing that is the game's NOLLIE landing (front foot out on the nose) and then its step on to
    // regular, which every fix layered on top kept showing through ("still clipping, front foot floats
    // a bit"; "why cant we get the same exact landing behavior for regular landing"). So for exactly
    // the covered landing the ANIMATION is told it is regular: USkaterAnimInstance's own copies of the
    // stance, FootPositionType (+0x305, copied from the skater in NativeUpdateAnimation BEFORE this
    // hook -- its only writer) and LandingFootPositionType (+0x306, SetFlipTrick's). The game's own
    // stance (the skater's) is untouched: tricks, switching, everything else reads that. The cover ends
    // the moment the game gets to regular itself (no change for the animation to see, so no step).
    bool animOv = false;
    if (g_landAnim && s_landSt && (stRaw == 3 || stRaw == 2) && !s_landXfer && mode != BM_FALLING) {   // a 180: the transfer plays (501); not mid-air (504)
        *((uint8_t*)a + AN_FOOT_POS) = (uint8_t)s_landSt;
        *((uint8_t*)a + AN_LAND_FOOT_POS) = (uint8_t)s_landSt;
        if (s_landSt == 4) *((uint8_t*)a + AN_IS_SWITCH) = 1;
        animOv = true;
    }
    // STANCE SWAP AND REVERT hold for 0.8 s from when they come on, and no longer. The game leaves
    // FootPositionTransitionType set ~5 s after a revert while you roll away in the new stance (the
    // 426 log: "held off for 5 s on the ground by: swapping stance"), which put the feet back at
    // default after every revert. The spin itself is over well inside 0.8 s.
    {
        static float s_swapFor = 0.0f, s_revFor = 0.0f;
        const bool swapOn = a && twkB(a, AN_FOOT_POS_TR) != 0, revOn = a && twkB(a, AN_REVERT) != 0;
        s_swapFor = swapOn ? s_swapFor + dt : 0.0f;
        s_revFor  = revOn  ? s_revFor  + dt : 0.0f;
        const bool xferCover = landActive && s_landXfer;      // a 180's own transfer signals are the landing's (501)
        if (!block && revOn  && s_revFor  < 0.8f && !xferCover) block = "revert";
        if (!block && swapOn && s_swapFor < 0.8f && !xferCover) block = "swapping stance";
    }

    // Where the animation has each foot THIS frame, before anything of ours goes on, in the deck's frame.
    Ride rd;
    // THE IDLE IS KEPT FOR THE SESSION. It was forgotten after 3 s off the board and learned afresh
    // on the next ride -- which needs 1.2 s of clean rolling, and with tricks and landings every few
    // seconds that took 27-28 s in the 437 log, feet at default the whole time ("I get caught in the
    // default stance ... if I wait long enough it switches"). It re-learned the same feet every time
    // (x 10.1 / -29.9, 9.9 / -30.2, 9.9 / -30.2): it is the animation's idle, the same every ride.
    // The one thing that changes it is goofy <-> regular in the settings, which swaps which foot leads.
    // Saved across launches (see "the learned idle"), so the goofy it was learned with is saved too.
    {
        const int goofy = a ? (twkB(a, AN_IS_GOOFY) != 0) : -1;
        if (goofy >= 0 && g_homeGoofy >= 0 && goofy != g_homeGoofy && (s_home[1][0].ok || s_home[4][0].ok)) {
            ForgetLearned(false);                   // the deck is the same deck
            if (s_saidEdge < 300) { s_saidEdge++; TwkLog("[stance] goofy/regular changed: learning your idle afresh"); }
        }
        if (goofy >= 0 && goofy != g_homeGoofy) { g_homeGoofy = goofy; TwkMarkDirty(); }
        if (InterlockedExchange(&g_relearn, 0)) {
            ForgetLearned(true);
            if (s_saidEdge < 300) { s_saidEdge++; TwkLog("[stance] re-learning your idle and the deck (asked from F1)"); }
        }
    }
    bool feet = a && RideFrame(a, rd);
    // Which foot leads: the stance says so directly.
    const bool frontIsRight = a && ((twkB(a, AN_IS_GOOFY) != 0) != (st == 4));
    Home* home = (st >= 0 && st < 5) ? s_home[st] : s_home[0];
    float fm[2][3], fx[2] = { 0.0f, 0.0f }, fh[2] = { 0.0f, 0.0f }, ax[2] = { 0.0f, 0.0f };
    for (int f = 0; feet && f < 2; f++) {
        const int off = f ? AN_R_SOCK_LOC : AN_L_SOCK_LOC;
        for (int i = 0; i < 3; i++) fm[f][i] = twkF(a, off + i * 4);
        if (!(fm[f][0] > -1e5f && fm[f][1] > -1e5f && fm[f][2] > -1e5f)) { feet = false; break; }
        ToDeck(rd, fm[f], fx[f], fh[f]);
    }
    // The front of the board is the end the LEADING foot stands toward. Positions are then measured
    // along it, so a board that comes back reversed from a shuvit changes nothing, and "back foot" is
    // the trailing foot whichever end of the board it is on.
    if (feet) {
        const int fF = frontIsRight ? 1 : 0, fB = 1 - fF;
        if (fx[fF] - fx[fB] < 0.0f) {
            rd.travel = -1.0f;
            for (int i = 0; i < 3; i++) { rd.along[i] = -rd.along[i]; rd.across[i] = -rd.across[i]; }
        }
        for (int f = 0; f < 2; f++) ax[f] = fx[f] * rd.travel;
    }

    // BOTH FEET ON THE DECK. None of the anim variables says "a foot is down on the ground" -- a
    // brake, standing still -- but the geometry does: a foot on the tail sits a centimetre or two
    // ABOVE the other, and a foot on the ground sits the better part of ten below it. Such a pose is
    // neither a stance to move nor an idle to learn from (one was learned once, and every home after
    // it was wrong).
    // With hysteresis: a pushing foot coming back hovers right at the line for a moment, and a bare
    // threshold toggled the gate several times in a second as it landed.
    static bool s_footOff = false;
    if (feet) {
        const float dh = fabsf(fh[0] - fh[1]);
        if (!(fabsf(fx[0]) < 45.0f && fabsf(fx[1]) < 45.0f) || dh > 4.0f) s_footOff = true;
        else if (dh < 2.5f) s_footOff = false;
    }
    if (!block && feet && s_footOff && !(landActive && s_landXfer)) block = "a foot is off the board";
    // Caught in the air (504): "in the air", "catching", "board flipping", "not riding" and the rest are the
    // trick this landing is catching into -- none of them hold it off.
    if (s_airLand && g_on && block) block = nullptr;

    // The board's rotation in the mesh's frame (the landing's held turn is kept against it), and its
    // heading there: the board turned against the body.
    float qB[4] = { 0.0f, 0.0f, 0.0f, 1.0f }, yawB = 0.0f;
    if (feet) {
        const float cm[4] = { -rd.qm[0], -rd.qm[1], -rd.qm[2], rd.qm[3] };
        TwkQuatMul(cm, rd.qf, qB);
        const float x1[3] = { 1.0f, 0.0f, 0.0f };
        float bx[3]; TwkQuatRotate(qB, x1, bx);
        yawB = atan2f(bx[1], bx[0]) * 57.2957795f;
    }
    auto wrap = [](float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; };
    if (s_lm.on) {
        if (feet && !s_lm.tdSet) { s_lm.tdSet = true; s_lm.yTd = yawB; }
        if (feet && s_lm.caught) {
            const float d = wrap(yawB - s_lm.yCatch);
            s_lm.dMin = fminf(s_lm.dMin, d); s_lm.dMax = fmaxf(s_lm.dMax, d);
        }
        if (block) { s_lm.blocked += dt; if (!s_lm.why) s_lm.why = block; }
        if (!landActive) {
            s_lm.on = false;
            static int s_n = 0;
            if (s_n < 100) {
                s_n++;
                if (s_lm.caught)
                    TwkLog("[stance] landing over: the board turned %+.1f deg against the body from touchdown to the catch, "
                           "then %+.1f..%+.1f more while held | held turn up to %.1f deg, height held by up to %.1f cm, tilt set by up to %.1f deg, "
                           "flattened along the board by up to %.1f deg | stance held off %.2f s of it%s%s%s",
                           wrap(s_lm.yCatch - s_lm.yTd), s_lm.dMin, s_lm.dMax, s_lm.turnMax, s_lm.hMax, s_lm.pMax, s_lm.kMax, s_lm.blocked,
                           s_lm.why ? " (first: " : "", s_lm.why ? s_lm.why : "", s_lm.why ? ")" : "");
                else
                    TwkLog("[stance] landing over: nothing caught (stance held off %.2f s of it%s%s%s)", s_lm.blocked,
                           s_lm.why ? ", first: " : "", s_lm.why ? s_lm.why : "", s_lm.why ? "" : "");
            }
        }
    }

    // Out fast, so a crank or a manual never starts with the stance still on it; in more slowly, so
    // coming back to the idle eases rather than snaps -- except onto a landing, which the feet should
    // land INTO rather than drift into.
    const float want = block ? 0.0f : 1.0f;
    s_gate += (want - s_gate) * (1.0f - expf(-dt / (block ? 0.06f : (landActive && s_since < hold ? 0.1f : 0.25f))));
    // The homes and the deck are learned off the animated feet, which the landing clip has over the
    // bolts: its window is never idle to learn from, even with the stance applying through it.
    s_idleFor = (block || landFlag || landActive || s_landSt) ? 0.0f : s_idleFor + dt;   // never learn from a nollie's feet
    {
        void* sk = a ? twkP(a, AN_SKATER) : nullptr;
        void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
        const int landToes = !landActive ? -1 : (s_landType == LT_SW || s_landType == LT_FAKIE) ? 1 : s_landXfer ? 0 : -1;
        __try { ToeUpdate(a, mesh, stRaw, s_idleFor > 1.2f, riding, dt, animOv, landToes); }
        __except (EXCEPTION_EXECUTE_HANDLER) { s_toeW = 0.0f; }
    }
    // A blocker that HOLDS while he is on the ground is either a long manual or a flag that has stuck
    // -- say which after five seconds, so a stuck one is named instead of looking like a lost stance.
    {
        static float s_heldFor = 0.0f; static const char* s_held = nullptr; static int s_saidHeld = 0;
        const bool grounded = a && twkB(a, AN_GROUNDED) == 1 && twkB(a, AN_ON_BOARD) == 1;
        if (block && grounded && block == s_held) s_heldFor += dt; else { s_heldFor = 0.0f; s_held = block; }
        if (s_heldFor > 5.0f && s_saidHeld < 40) {
            s_saidHeld++; s_heldFor = -1e9f;                // once per hold
            TwkLog("[stance] held off for 5 s on the ground by: %s", block);
        }
    }
    if (block != s_lastBlock) {                    // edges only: what the gate is waiting on
        s_lastBlock = block;
        if (s_saidEdge < 300) { s_saidEdge++; TwkLog("[stance] %s", block ? block : "rolling idle: the stance applies"); }
    }
    g_status = block ? block : (st == 4 ? "rolling idle (switch)" : "rolling idle (main stance)");
    // The game puts you in switch by itself after a 180, and from then the SWITCH set drives the feet
    // until you are back in your main stance -- which from the main set's sliders looks like they
    // stopped working. The F1 page marks the live set so that is never a mystery.
    g_live = (st == 4) ? 1 : (st == 1 ? 0 : -1);
    if (!feet) { g_shown = 0.0f; ReleaseRotations(a); return; }

    // THE HOME is where the NORMAL rolling idle puts each foot: learned once per ride, after the idle has
    // held 1.2 s, then only refined inside the idle's own sway (2 cm). It is never re-learned from
    // where the idle DRIFTS to: after a 360 flip the game parks the front foot 5.5 cm further forward
    // (15.5 instead of 10.0, every time in the 424 log), and a home that followed it -- or a stance
    // that was a difference added to it -- landed a -58 mm front foot right back on the default spot.
    // Forgotten only after 3 s off the board (see above), so each ride starts from its own normal idle.
    // LEARNED ONCE, NEVER NUDGED (491). It used to be refined toward the animated feet whenever they sat
    // within 2 cm -- which a pose DRIFTING slowly (the switch idle after fakie tricks slid the back foot
    // -26 -> -36 cm over 13 s in the 490 log) walked step by step, and the crawled home was saved: "you
    // broke switch riding, looks like regular stance now".
    // The switch riding pose's toe-down angle (493): averaged over its first 30 clean samples this session,
    // then fixed -- a reference, never a tracker (see LEARNED ONCE).
    if (!block && s_idleFor > 1.2f && stRaw == 4 && !landActive && !s_abIdleOk) {
        float ab0, ab1;
        if (AbRise(a, rd, 0, ab0) && AbRise(a, rd, 1, ab1)) {
            s_abSum[0] += ab0; s_abSum[1] += ab1;
            if (++s_abN >= 30) {
                s_abIdle[0] = s_abSum[0] / s_abN; s_abIdle[1] = s_abSum[1] / s_abN; s_abIdleOk = true;
                TwkMarkDirty();                           // kept for the next launch (498)
                if (s_saidEdge < 300) { s_saidEdge++; TwkLog("[stance] switch riding: toe-down angles left %.1f / right %.1f deg (the switch landing's tilt)", s_abIdle[0], s_abIdle[1]); }
            }
        }
    }
    // The landing reference (499, one for every landing since 506): the regular riding front ball, drawn, less
    // our share of it -- the game's own ball, whatever the sliders.
    {   // a main-stance slider moved: learn it again (507)
        int key[S_N + 1];
        for (int i = 0; i < S_N; i++) key[i] = g_set[0][i];
        key[S_N] = g_follow;
        bool same = s_regBallKeySet;
        for (int i = 0; same && i <= S_N; i++) same = key[i] == s_regBallKey[i];
        if (!same) {
            if (s_regBallKeySet && s_regBallOk) s_regBallPending = true;   // the first read is the loaded stance's
            for (int i = 0; i <= S_N; i++) s_regBallKey[i] = key[i];
            s_regBallKeySet = true;
            s_regBallN = 0; s_regBallSum = 0.0f; s_regBallOurs = 0.0f;
        }
    }
    if (!block && s_idleFor > 1.2f && stRaw == 1 && !landActive && (!s_regBallOk || s_regBallPending)) {
        float bh;
        const int fF = frontIsRight ? 1 : 0;
        if (DrawnBallH(a, rd, fF, bh)) {
            s_regBallSum += bh - ourBallPrev[fF]; s_regBallOurs += ourBallPrev[fF];
            if (++s_regBallN >= 30) {
                const float was = s_regBall; const bool again = s_regBallPending;
                s_regBall = s_regBallSum / s_regBallN; s_regBallOk = true; s_regBallPending = false;
                TwkMarkDirty();
                if (s_saidEdge < 300) {
                    s_saidEdge++;
                    if (again) TwkLog("[stance] landing reference re-learned after a stance change: %.2f cm (was %.2f; your stance's %+.2f cm taken out)", s_regBall, was, s_regBallOurs / s_regBallN);
                    else TwkLog("[stance] landing reference: the game's regular riding front ball at %.2f cm (your stance's %+.2f cm taken out) -- every landing's", s_regBall, s_regBallOurs / s_regBallN);
                }
            }
        }
    }
    if (!block && s_idleFor > 1.2f) {
        bool learned = false;
        for (int f = 0; f < 2; f++)
            if (!home[f].ok) { home[f].x = ax[f]; home[f].h = fh[f]; home[f].ok = true; learned = true; }
        if (learned) TwkMarkDirty();                 // kept for the next launch
        if (learned && s_saidEdge < 300) {
            s_saidEdge++;
            TwkLog("[stance] learned the normal idle for stance %d: feet at x %.1f / %.1f cm", st, home[0].x, home[1].x);
        }
    }

    // The deck's long axis as the flipper has it (not signed by travel), and the side across it the
    // toes are on: right of travel for regular, left for goofy or switch (see above). Right of travel
    // is up x along, so against the deck's own axis that is `travel` times up x lenX.
    float lenX[3], side0[3];
    for (int i = 0; i < 3; i++) lenX[i] = rd.along[i] * rd.travel;
    side0[0] = rd.up[1]*lenX[2] - rd.up[2]*lenX[1];
    side0[1] = rd.up[2]*lenX[0] - rd.up[0]*lenX[2];
    side0[2] = rd.up[0]*lenX[1] - rd.up[1]*lenX[0];
    // Toes to the right of "toward the front" when the left foot leads, to the left when the right does.
    const float sideF = (frontIsRight ? -1.0f : 1.0f) * rd.travel;
    if (s_impPending) {
        s_impPending = false;
        // Which of the deck's own sides (+Y / -Y) the toes are on, and the landing spot's across (mm, + toes).
        float toesM[3], toesW[3], yW[3];
        const float y1[3] = { 0.0f, 1.0f, 0.0f };
        for (int i = 0; i < 3; i++) toesM[i] = side0[i] * sideF;
        TwkQuatRotate(rd.qm, toesM, toesW); TwkQuatRotate(rd.qf, y1, yW);
        const float toeY = (toesW[0]*yW[0] + toesW[1]*yW[1] + toesW[2]*yW[2]) >= 0.0f ? 1.0f : -1.0f;
        const int* LS = g_land[s_landType];
        const float acr = 0.5f * ((float)LS[L_FC] + s_rand[1][1] + (float)LS[L_BC] + s_rand[0][1]);
        const float side = acr > 1.0f ? 1.0f : (acr < -1.0f ? -1.0f : (RandSigned() >= 0.0f ? 1.0f : -1.0f));
        void* bmI = twkP(rd.board, BOARD_MOVE);
        float wall = GrindLean_TruckWall(bmI);
        if (!(wall > 0.0f)) wall = 20.0f;
        // + roll dips the deck's +Y edge (grind_lean 451, confirmed): toward the toes is +toeY.
        s_impAmp = s_impAmount * (float)g_landLeanPct * 0.01f * wall * side * toeY;
        s_impWall = wall; s_impToeY = toeY; s_impEdge0 = ToeEdgeDeg(rd.qf, toeY); s_impEdgeMax = 0.0f;
        s_impBmc = bmI; s_impT = 0.0f; s_impRoll = 0.0f;
    }

    // Measured with the stance at zero too (505): it is the game's own idle, and a landing needs it.
    KickLatch& kl = s_kick[(st >= 0 && st < 5) ? st : 0];
    if (!kl.decided && !block && s_idleFor > 1.5f && home[0].ok && home[1].ok) {
        // The raw rise and run from this frame's homes, averaged; the flat's height cancels out of
        // every correction (only the rise over the run is ever used), so it need not be held.
        const int lo = (home[0].h <= home[1].h) ? 0 : 1, hi = 1 - lo;
        kl.sumRise += home[hi].h - home[lo].h;
        kl.sumRun  += fabsf(home[hi].x) - rd.truck[home[hi].x * rd.travel >= 0.0f ? 1 : 0];
        if (++kl.n >= 120) {
            const float rise = kl.sumRise / kl.n, run = kl.sumRun / kl.n;
            kl.decided = true;
            TwkMarkDirty();                             // kept for the next launch
            kl.k = Kick{ false, 0.0f, 0.0f, 1.0f, 0.0f };
            if (rise >= 0.5f && run >= 2.0f) {
                kl.k.ok = true; kl.k.hFlat = home[lo].h; kl.k.rise = rise; kl.k.run = run;
                kl.k.tilt = atan2f(rise, run);
            }
            if (s_saidEdge < 300) {
                s_saidEdge++;
                if (kl.k.ok)
                    TwkLog("[stance] deck measured for stance %d and held: kick starts at the trucks (%.1f / %.1f cm), "
                           "rises %.1f cm over %.1f cm = %.0f deg (from %d samples; feet at x %.1f / %.1f, h %.1f / %.1f)",
                           st, rd.truck[0], rd.truck[1], rise, run, kl.k.tilt * 57.2957795f, kl.n,
                           home[0].x, home[1].x, home[0].h, home[1].h);
                else
                    TwkLog("[stance] deck measured for stance %d: no kick under either foot (rise %.1f cm; feet at "
                           "x %.1f / %.1f, h %.1f / %.1f) -- another stance's measurement is used if it has one",
                           st, rise, home[0].x, home[1].x, home[0].h, home[1].h);
            }
        }
    }
    if ((zero && !landActive) || s_gate < 0.001f || !home[0].ok || !home[1].ok) {
        if (!block) g_status = zero ? "rolling idle -- every slider for this stance is at zero"
                                    : "rolling idle -- learning where your feet normally sit";
        // Whatever kept it off, the stance glides in from nothing once it can apply: the gate's ease
        // kept counting while it could not, and the feet jumped straight to the stance when it could.
        s_gate = 0.0f;
        g_shown = 0.0f; ReleaseRotations(a); return;
    }

    // Front and back by where the idle puts them: further along the way he is going is the front.
    // [0] the left foot, [1] the right. Each foot has two poses: the STANCE (the normal idle's home plus
    // the sliders) and the LANDING (over the BOLTS -- the truck under that foot, in the same travel-
    // signed x as the feet, front = +x -- plus the landing sliders and this landing's own spot). After
    // touchdown the foot holds the landing, then swivels (or glides) to the stance.
    const bool rightFront = frontIsRight;
    const float boltX[2] = { -(rd.travel > 0.0f ? rd.truck[0] : rd.truck[1]),     // [0] back, [1] front
                              (rd.travel > 0.0f ? rd.truck[1] : rd.truck[0]) };
    float targetX[2], acrossCm[2], yawDeg[2], pitchDeg[2], riseCm[2] = { 0.0f, 0.0f }, freezeK[2] = { 0.0f, 0.0f };
    bool  idleSet[2];
    bool  bothHome = true;
    for (int f = 0; f < 2; f++) {
        const bool fr = (f == 1) == rightFront;
        const int  r  = fr ? 1 : 0;                           // role: [0] back, [1] front
        // THE GAME'S OWN SPOT (505): a foot whose sliders are all at zero is the game's once the landing is
        // over, so it steps back to where the animation has it NOW, not to the learned home -- which it can be
        // off by the idle's drift (5.5 cm after a 360 flip): the landing ends with nothing added, no jump.
        idleSet[f] = S[fr ? S_FA : S_BA] || S[fr ? S_FC : S_BC] || S[fr ? S_FY : S_BY] || S[fr ? S_FP : S_BP];
        const Pose Sp = { idleSet[f] ? home[f].x + (float)S[fr ? S_FA : S_BA] * 0.1f : ax[f], (float)S[fr ? S_FC : S_BC] * 0.1f,
                          (float)S[fr ? S_FY : S_BY], (float)S[fr ? S_FP : S_BP], 0.0f };
        const int* LS = g_land[s_landType];
        Pose Lp = { boltX[r] + ((float)LS[fr ? L_FA : L_BA] + s_rand[r][0]) * 0.1f,
                    ((float)LS[fr ? L_FC : L_BC] + s_rand[r][1]) * 0.1f,
                    (float)LS[fr ? L_FY : L_BY] + s_rand[r][2],
                    (s_landType == LT_SW || s_landType == LT_FAKIE) ? Sp.p : 0.0f, 0.0f };   // switch/fakie: your pitch, as when riding (495)
        if (landActive && g_landImpactPct > 0) {                  // the feet take the landing (512)
            const float h = kSlapDeg * g_landImpactPct * 0.01f;
            if (s_airLand) {                                      // caught, not yet weighted: heels up, down by the impact
                const float ap = AirSlapPitch(h);
                Lp.p += ap; s_slapLast = ap;
            } else {
                const float k = MinJerk(s_since / kKnockSec);
                Lp.x += s_knock[r][0] * k; Lp.c += s_knock[r][1] * k; Lp.y += s_knock[r][2] * k;
                Lp.p += LandSlapPitch(s_since, h, s_fallImpact);
            }
        }
        Pose P = Sp;
        freezeK[f] = 0.0f;
        if (landActive && g_landStep) {
            const float tau = s_since - hold - (fr ? FrontDelay() : 0.0f);
            if (tau < 0.0f) { P = Lp; bothHome = false; freezeK[f] = 1.0f; }
            else {
                if (!s_step[r].latched) LatchStep(s_step[r], Lp, Sp, fr);
                P = StepPose(Lp, Sp, s_step[r], tau);
                freezeK[f] = 1.0f - MinJerk(tau / s_step[r].T);
                if (tau < s_step[r].T) bothHome = false;
            }
        } else if (landActive) {                              // the glide
            P.x = Sp.x + (Lp.x - Sp.x) * landW;
            P.c = Sp.c + (Lp.c - Sp.c) * landW;
            P.y = Sp.y + (Lp.y - Sp.y) * landW;
            P.p = Sp.p * (1.0f - landW);
            freezeK[f] = landW;
        }
        targetX[f] = P.x; acrossCm[f] = P.c; yawDeg[f] = P.y; pitchDeg[f] = P.p; riseCm[f] = P.up;
    }
    if (landActive && g_landStep && bothHome) s_settled = true;
    // Where the game's own landing clip has the feet against the bolts, once per landing: shows how
    // closely "over the truck" matches the game's own "over the bolts".
    {
        static bool s_logged = true; static int s_n = 0;
        if (s_since < 0.05f) s_logged = false;
        if (!s_logged && s_since > 0.3f && landActive && s_n < 60) {
            s_logged = true; s_n++;
            const int fF = rightFront ? 1 : 0, fB = 1 - fF;
            TwkLog("[stance] landing: the game's clip has front %.1f / back %.1f cm, the bolts are at %.1f / %.1f",
                   ax[fF], ax[fB], boltX[1], boltX[0]);
        }
    }

    // "+ across" is toward the toes, from the stance. The two angle signs follow from geometry rather
    // than from their own tests (tools/stance_check.py proves them): with the toes along `across`,
    // turning about the deck's normal by -side*travel swings them toward the nose, and turning about
    // the board's length by +side*travel lifts them.
    float across[3];
    for (int i = 0; i < 3; i++) across[i] = side0[i] * sideF;
    const float yawSign   = -sideF * rd.travel;
    const float pitchSign =  sideF * rd.travel;

    // THE DECK IS THE SAME DECK IN EVERY STANCE, and both ends are kicks: whichever stance measured one
    // lends it to the others, so a stance whose idle happens to read flat still rides the kick when a
    // foot is moved onto it.
    const Kick* shared = (kl.decided && kl.k.ok) ? &kl.k : nullptr;
    for (int i = 0; !shared && i < 5; i++) if (s_kick[i].decided && s_kick[i].k.ok) shared = &s_kick[i].k;
    const Kick none = { false, 0.0f, 0.0f, 1.0f, 0.0f };
    const Kick kick = !g_follow ? none : (shared ? *shared : none);
    // Nothing goes on until the deck is known: otherwise the stance would arrive without the follow
    // and then drop onto the deck a couple of seconds later.
    if (g_follow && !kl.decided && !shared) {
        g_status = "measuring the deck";
        s_gate = 0.0f;
        g_shown = 0.0f; ReleaseRotations(a); return;
    }
    float* const out[2] = { dL, dR };
    float shown = 0.0f;
    for (int f = 0; f < 2; f++) {
        // A foot whose sliders are all at zero is left exactly as the game has it -- unless a landing
        // is taking it to the bolts.
        const bool untouched = !idleSet[f] && !landActive;
        const float w = untouched ? 0.0f : s_gate * s_airW[f];      // caught in the air: by the catch (504)
        if (w > shown) shown = w;
        if (w < 0.001f) {
            const float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            ApplyExtraRot(a, f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT, f, none);
            continue;
        }

        // ALONG THE BOARD THE FOOT GOES TO A SPOT, not by an amount: the normal idle's home plus the
        // rider's offset, wherever the animation has the foot this frame. An amount added to the
        // animation inherits every drift the idle has (see THE HOME above); a spot does not. Fully on
        // whenever the gate is open -- the gate's own ease is the glide in after a landing or a push.
        const float A = (targetX[f] - ax[f]) * w, C = acrossCm[f] * w;
        for (int i = 0; i < 3; i++) out[f][i] += (rd.along[i] * A + across[i] * C + rd.up[i] * riseCm[f] * w) / rd.sm;

        // The landing held against the clip (see THE LANDING IS HELD): caught once the landing shows
        // (the gate half open, a few hundredths after touchdown -- not the touchdown frame itself,
        // which can still carry the air pose) -- the game's own socket values, ours having been taken
        // out in Stance_PreAnchors.
        const int rotOff = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
        float qHold[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (freezeK[f] > 0.0f) {
            const float cur[3] = { twkF(a, rotOff), twkF(a, rotOff + 4), twkF(a, rotOff + 8) };
            float qCur[4]; TwkRotatorToQuat(cur, qCur);
            const float yNow = DeckY(rd, fm[f]);
            if (s_frzWant && !s_frz[f].ok && !g_held[f] && w >= 0.5f) {
                s_frz[f].ok = true; s_frz[f].x = fx[f]; s_frz[f].y = yNow;
                const float qBi[4] = { -qB[0], -qB[1], -qB[2], qB[3] };
                TwkQuatMul(qBi, qCur, s_frz[f].q);          // in the board's frame
                if (s_lm.on && !s_lm.caught) { s_lm.caught = true; s_lm.yCatch = yawB; s_lm.dMin = s_lm.dMax = 0.0f; }
            }
            if (s_frz[f].ok) {
                const float k = freezeK[f] * w;
                const float kTurn = s_landXfer ? 0.0f : k;           // a 180: the feet turn with the body (501)
                const float y1[3] = { 0.0f, 1.0f, 0.0f };
                float yw[3], ym[3]; TwkQuatRotate(rd.qf, y1, yw); TwkQuatInvRotate(rd.qm, yw, ym);
                for (int i = 0; i < 3; i++) out[f][i] += ym[i] * (s_frz[f].y - yNow) * k / rd.sm;
                const float inv[4] = { -qCur[0], -qCur[1], -qCur[2], qCur[3] };
                float held[4]; TwkQuatMul(qB, s_frz[f].q, held);    // the caught turn, where the board is now
                float full[4]; TwkQuatMul(held, inv, full);
                // Only the TURN about the deck's normal is held (twist of a swing-twist split): the
                // clip's first landing frame can have a heel or toe dipped from the impact, and holding
                // that tilt left the foot in the deck. The tilt stays the animation's and the follow's.
                const float d = full[0] * rd.up[0] + full[1] * rd.up[1] + full[2] * rd.up[2];
                float turn[4] = { rd.up[0] * d, rd.up[1] * d, rd.up[2] * d, full[3] };
                const float tn = sqrtf(turn[0]*turn[0] + turn[1]*turn[1] + turn[2]*turn[2] + turn[3]*turn[3]);
                if (tn > 1e-6f) { for (int i = 0; i < 4; i++) turn[i] /= tn; }
                else { turn[0] = turn[1] = turn[2] = 0.0f; turn[3] = 1.0f; }
                if (s_lm.on) s_lm.turnMax = fmaxf(s_lm.turnMax, 2.0f * acosf(fminf(1.0f, fabsf(turn[3]))) * 57.2957795f);
                QuatPart(turn, kTurn, qHold);
            }
        }

        // Onto the deck at the new spot, and lying on it -- both as differences from where the model
        // puts the animated foot, so a foot that has not moved gets nothing.
        float follow[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float hFol = 0.0f;                                   // what the follow adds to the height
        if (kick.ok) {
            const float x0 = fx[f], x1 = fx[f] + A * rd.travel;
            float h0, t0, h1, t1;
            DeckAt(rd, kick, x0, h0, t0);
            DeckAt(rd, kick, x1, h1, t1);
            for (int i = 0; i < 3; i++) out[f][i] += rd.up[i] * (h1 - h0) / rd.sm;
            hFol = h1 - h0;
            // Laid from where the ANIMATION has the foot this frame, onto the new spot. Always the
            // animated spot: the held turn is about the deck's normal only, so the foot's tilt is
            // still the animation's -- after a landing the clip walks the back foot onto the tail and
            // tilts it with the kick, and measuring from the caught spot on the flat (443) left that
            // tilt on a foot held over the bolts ("the foot sticks" angled through the hold).
            float n0[3], n1[3];
            DeckNormal(rd, x0, t0, n0);
            DeckNormal(rd, x1, t1, n1);
            RotBetween(n0, n1, follow);
        }
        // A SWITCH LANDING LANDS AT SWITCH RIDING HEIGHT (492). The game's switch landing lifts the feet
        // (the front 14.0 -> 17.0 cm over the first 0.6 s, every one in the 488/489 logs: "switch landings
        // float too"), while its switch riding pose looks right. So through a switch landing -- the hold,
        // fading over the swivel -- both feet stand exactly as high as the FRONT foot does when you ride
        // switch: the learned switch home, a known-good value (learned once since 491, it cannot creep).
        // The user's own spec: "get both feet landing at the same height as the front foot during switch
        // riding". The landing spots are on the flat (one-sided variation, 485), where that height holds;
        // the swivel's own heel lift stays on top.
        // 495: not the ANKLE at the front ankle's riding height (492: the balls ended ~1 cm high in front, more
        // at the back -- "both floating"), but each foot's BALL at the front ball's riding height: the ankle
        // above it by the rigid ankle->ball line at the tilt this landing gives the foot (the riding tilt, 493,
        // plus your pitch slider, above).
        // 499: EVERY landing, not only switch/fakie -- regular and nollie ones against the regular riding front
        // ball, at the landing's own toe-down (the regular clip's tilt was never the problem; only its height
        // is, now that 498 lays the foot flat).
        // 500: the ankle is set AFTER the foot's rotation is final (see THE BALL, FROM THE FINAL FOOT, below).
        const bool swKind = s_landType == LT_SW || s_landType == LT_FAKIE;       // fakie lands into switch (496)
        // A SWITCH LANDING'S FEET TILT LIKE SWITCH RIDING (493): the switch landing clip starts toes-up (-34 deg
        // where riding has -43 / -37: "front foot moves up and down slightly a few times", "the back foot seems
        // to stay a bit higher"). Through the landing each foot gets the pitch that brings its toe-down angle to
        // the switch riding pose's -- the same "toes up" turn the pitch sliders use -- fading over the swivel.
        if (landActive && (s_landType == LT_SW || s_landType == LT_FAKIE) && freezeK[f] > 0.0f && s_abIdleOk) {
            float ab;
            if (AbRise(a, rd, f, ab)) {
                const float c = (s_abIdle[f] - ab) * freezeK[f];
                pitchDeg[f] += c;
                if (s_lm.on) s_lm.pMax = fmaxf(s_lm.pMax, fabsf(c));
            }
        }
        float qy[4], qp[4], q1[4], q1h[4], q2[4];
        TwkQuatAxisAngle(rd.up,  yawDeg[f]   * yawSign   * w, qy);
        // The feet point across the board, so "toes up" turns about the board's LENGTH; turning about
        // the across axis rolls the foot along its own length instead.
        TwkQuatAxisAngle(rd.along, pitchDeg[f] * pitchSign * w, qp);
        TwkQuatMul(qy, qp, q1);                    // the rider's own angles...
        TwkQuatMul(q1, qHold, q1h);                // ...on the held landing pose (identity once it is released)...
        TwkQuatMul(follow, q1h, q2);               // ...on top of lying on the deck
        // FLAT ALONG THE BOARD ON A LANDING (498). Through the hold the game's clip walks the back foot onto the
        // tail and tilts it with the kick (about -14 deg along the board by +0.6 s, regular), while ours keeps it
        // on the flat by the bolts -- and the deck model under the follow believes a gentle kick (this deck:
        // 1.34 cm over 8.99 = 8.5 deg; the foot on the tail really stands ~24 deg), so most of that tilt
        // stayed: the foot's middle-side edge dipped into the deck, more the further in it lands ("the back
        // foot is often still slightly tilted forward ... dipping and clipping into the board"; every stance).
        // So each foot's tilt ALONG the board -- its socket X's lean toward nose/tail; a foot standing on the
        // flat reads 0..2 deg -- is set to 0 over the landing, fading with the hold like the rest. Only that one
        // axis: the toes-up angle (across) is kept exactly as it is.
        if (landActive && freezeK[f] > 0.0f) {
            const int rOff = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
            const float cr[3] = { twkF(a, rOff), twkF(a, rOff + 4), twkF(a, rOff + 8) };
            if (cr[0] > -1e5f && cr[1] > -1e5f && cr[2] > -1e5f) {
                float qg[4]; TwkRotatorToQuat(cr, qg);
                float pre[4]; TwkQuatMul(q2, qg, pre);
                const float x1[3] = { 1.0f, 0.0f, 0.0f };
                float X[3]; TwkQuatRotate(pre, x1, X);
                float u = X[0]*rd.up[0] + X[1]*rd.up[1] + X[2]*rd.up[2];
                if (u < 0.0f) { for (int i = 0; i < 3; i++) X[i] = -X[i]; u = -u; }
                const float c = X[0]*across[0] + X[1]*across[1] + X[2]*across[2];
                const float u2 = sqrtf(fmaxf(0.0f, 1.0f - c * c));
                float Xd[3];
                for (int i = 0; i < 3; i++) Xd[i] = across[i] * c + rd.up[i] * u2;
                float qk[4]; RotBetween(X, Xd, qk);
                if (s_lm.on) s_lm.kMax = fmaxf(s_lm.kMax, 2.0f * acosf(fminf(1.0f, fabsf(qk[3]))) * 57.2957795f * freezeK[f]);
                float qks[4]; QuatPart(qk, freezeK[f] * w, qks);
                float t[4]; TwkQuatMul(qks, q2, t);
                for (int i = 0; i < 4; i++) q2[i] = t[i];
            }
        }
        // THE BALL, FROM THE FINAL FOOT (500). 499 worked the ankle out from the toe-down angle BEFORE 498's
        // flatten turned the foot -- and as the clip walks the back foot onto the tail through the hold, that
        // flatten grows (to ~14 deg); on a foot angled across the board it also lowers the ball, which then
        // sank through the deck partway through the hold ("shortly after landing, the foot slides down a bit
        // through the board"). Now the ankle comes from the foot as it will actually be drawn -- the rigid
        // ankle->ball line under this frame's final rotation -- so the ball sits exactly at the riding
        // reference whatever the clip does -- one reference for every kind (506). Fading with the hold like the rest.
        {
            float swAnkle = -1e9f;
            if (landActive && freezeK[f] > 0.0f) {
                const int fF = rightFront ? 1 : 0;
                const bool haveRef = s_regBallOk;
                const int rOff = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
                const float cr[3] = { twkF(a, rOff), twkF(a, rOff + 4), twkF(a, rOff + 8) };
                if (haveRef && s_abLocalOk[f] && cr[0] > -1e5f && cr[1] > -1e5f && cr[2] > -1e5f) {
                    float qg[4]; TwkRotatorToQuat(cr, qg);
                    float qfin[4]; TwkQuatMul(q2, qg, qfin);
                    float v[3]; TwkQuatRotate(qfin, s_abLocal[f], v);      // ankle -> ball, mesh space
                    const float drop = (v[0]*rd.up[0] + v[1]*rd.up[1] + v[2]*rd.up[2]) * rd.sm;   // ball below ankle: < 0
                    swAnkle = s_regBall - drop;
                } else if (swKind && home[fF].ok) swAnkle = home[fF].h;   // 492's, until the references are learned
            }
            float swH = 0.0f;
            if (swAnkle > -1e8f) {
                swH = (swAnkle - (fh[f] + hFol)) * freezeK[f] * w;
                for (int i = 0; i < 3; i++) out[f][i] += rd.up[i] * swH / rd.sm;
                if (s_lm.on) s_lm.hMax = fmaxf(s_lm.hMax, fabsf(swH));
            }
        }
        {
            // Our share of this foot's ball height (506): what we add to the ankle, plus what our rotation does to
            // the ankle->ball line. Next frame's reference learn takes it out of the drawn ball.
            const int rOff = f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT;
            const float cr[3] = { twkF(a, rOff), twkF(a, rOff + 4), twkF(a, rOff + 8) };
            const bool crOk = cr[0] > -1e5f && cr[1] > -1e5f && cr[2] > -1e5f;
            float dh = (out[f][0]*rd.up[0] + out[f][1]*rd.up[1] + out[f][2]*rd.up[2]) * rd.sm;
            if (crOk && s_abLocalOk[f]) {
                float qg[4]; TwkRotatorToQuat(cr, qg);
                float qfin[4]; TwkQuatMul(q2, qg, qfin);
                float v1[3], v0[3]; TwkQuatRotate(qfin, s_abLocal[f], v1); TwkQuatRotate(qg, s_abLocal[f], v0);
                dh += ((v1[0]-v0[0])*rd.up[0] + (v1[1]-v0[1])*rd.up[1] + (v1[2]-v0[2])*rd.up[2]) * rd.sm;
            }
            s_ourBallDh[f] = dh;
        }
        ApplyExtraRot(a, f ? AN_R_SOCK_ROT : AN_L_SOCK_ROT, f, q2);
    }
    if (s_frz[0].ok && s_frz[1].ok) s_frzWant = false;
    g_shown = shown;
    if (shown > 0.0f) g_status = !landActive ? (st == 4 ? "rolling idle (switch)" : "rolling idle (main stance)")
                               : s_since < hold ? (st == 4 ? "landing stance (switch)" : "landing stance (main stance)")
                                                : (st == 4 ? "stepping into your stance (switch)" : "stepping into your stance (main stance)");

    // Twice a second while it applies: what the animation did, where home is, what went on.
    static float s_logT = 0.0f;
    s_logT += dt;
    if (s_logT > 0.5f && shown > 0.0f && s_said < 40) {
        s_logT = 0.0f; s_said++;
        TwkLog("[stance] applying %.0f%% | L x %.1f (home %.1f) h %.1f | R x %.1f (home %.1f) h %.1f | "
               "front=%s stance=%d toes=%s of the front",
               shown * 100.0f, ax[0], home[0].x, fh[0], ax[1], home[1].x, fh[1],
               rightFront ? "R" : "L", st, frontIsRight ? "left" : "right");
    }
}

// ------------------------------------------------------------------ F1 page
void Stance_DrawMenu(const OmpMenuApi* api) {
    bool on = g_on != 0;
    if (api->Checkbox("Board stance", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(where your feet sit while you roll around)");
    if (!on) return;
    api->Indent();
    api->TextDisabled("only the rolling idle: setups, manuals, catches and pushes stay the game's own (landings: below)");
    api->TextDisabled("front = the leading foot, back = the one nearest the camera, in either stance");
    {
        char b[160];
        const float s = g_shown;
        const char* st = g_status;
        if (s > 0.01f) snprintf(b, sizeof(b), "now: %s, applied %.0f%%", st, s * 100.0f);
        else           snprintf(b, sizeof(b), "now: %s", st);
        api->TextDisabled(b);
    }

    // Distinct hidden IDs (##main / ##sw): ImGui keys a widget by its label, and two sliders with the
    // same label would both drive whichever it met first.
    static const char* const kLabel[S_N] = {
        "Front foot along (mm)", "Front foot across (mm)", "Front foot angle (deg)", "Front foot pitch (deg)",
        "Back foot along (mm)",  "Back foot across (mm)",  "Back foot angle (deg)",  "Back foot pitch (deg)" };
    static const char* const kHint[S_N] = {
        "(+ toward the nose)", "(+ toward your toes)", "(+ toes toward the nose)", "(+ toes up)",
        nullptr, nullptr, nullptr, nullptr };
    static const char* const kTitle[2] = { "Main stance", "Switch" };
    static const char* const kTag[2]   = { "##main", "##sw" };
    for (int k = 0; k < 2; k++) {
        api->Separator();
        api->Text(kTitle[k]);
        if (g_live == k) { api->SameLine(); api->Text("<- riding this now"); }
        if (k == 1) { api->SameLine(); api->TextDisabled("(the game puts you here after a 180)"); }
        for (int i = 0; i < S_N; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s%s", kLabel[i], kTag[k]);
            float v = (float)g_set[k][i];
            if (api->SliderFloat(label, &v, (float)kLo[i], (float)kHi[i], "%.0f")) {
                g_set[k][i] = clampI((int)(v < 0.0f ? v - 0.5f : v + 0.5f), kLo[i], kHi[i]);
                TwkMarkDirty();
            }
            if (kHint[i]) { api->SameLine(); api->TextDisabled(kHint[i]); }
        }
        if (api->version >= 2 && api->Button) {
            char b[48];
            snprintf(b, sizeof(b), "Centre %s%s", k ? "switch" : "the main stance", kTag[k]);
            if (api->Button(b)) CentreSet(k);
        }
    }
    api->Separator();
    api->Text("Landing");
    api->SameLine(); api->TextDisabled("(on touchdown the feet go over the bolts, hold, then glide into your stance)");
    bool lo = g_landOn != 0;
    if (api->Checkbox("Landing stance", &lo)) { g_landOn = lo ? 1 : 0; TwkMarkDirty(); }
    if (lo) {
        bool fx = g_landFix != 0;
        if (api->Checkbox("Fixed landing stance##land", &fx)) { g_landFix = fx ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(also with your stance sliders at zero: the game's landings fixed; off = only with a custom stance)");
        // One set per kind of landing; the buttons pick which one the sliders below edit.
        static int s_edit = LT_REG;
        static const char* const kKind[LT_N]  = { "Regular", "Switch", "Nollie", "Fakie" };
        static const char* const kKindHint[LT_N] = {
            "landing in your main stance", "landing in switch", "landing a nollie trick (then into your main stance)",
            "landing a fakie trick (then into switch; lands like a switch landing)" };
        static const char* const kKindTag[LT_N] = { "##landreg", "##landsw", "##landnollie", "##landfakie" };
        int t = s_edit;
        if (api->version >= 2 && api->Button) {
            api->TextDisabled("landing positions for:");
            for (int k = 0; k < LT_N; k++) {
                api->SameLine();
                char b[48]; snprintf(b, sizeof(b), "%s%s%s", k == s_edit ? "> " : "", kKind[k], kKindTag[k]);
                if (api->Button(b)) s_edit = k;
            }
            t = s_edit;
            char h[128]; snprintf(h, sizeof(h), "editing %s: %s", kKind[t], kKindHint[t]);
            api->TextDisabled(h);
        } else t = LT_REG;
        static const char* const kLandLabel[L_N] = {
            "Front foot along (mm)", "Front foot across (mm)", "Front foot angle (deg)",
            "Back foot along (mm)",  "Back foot across (mm)",  "Back foot angle (deg)" };
        for (int i = 0; i < L_N; i++) {
            char label[80]; snprintf(label, sizeof(label), "%s%s", kLandLabel[i], kKindTag[t]);
            float v = (float)g_land[t][i];
            if (api->SliderFloat(label, &v, (float)kLandLo[i], (float)kLandHi[i], "%.0f")) {
                g_land[t][i] = clampI((int)(v < 0.0f ? v - 0.5f : v + 0.5f), kLandLo[i], kLandHi[i]);
                TwkMarkDirty();
            }
            if (i == L_FA) { api->SameLine(); api->TextDisabled("(from the bolts, + toward the nose)"); }
        }
        if (t != LT_REG && api->version >= 2 && api->Button) {
            char b[64]; snprintf(b, sizeof(b), "Same as regular%s", kKindTag[t]);
            if (api->Button(b)) { for (int i = 0; i < L_N; i++) g_land[t][i] = g_land[LT_REG][i]; TwkMarkDirty(); }
        }
        float h = (float)g_landHoldMs, r = (float)g_landReturnMs;
        if (api->SliderFloat("Hold after landing (ms)##land", &h, 0.0f, 3000.0f, "%.0f")) { g_landHoldMs = clampI((int)(h + 0.5f), 0, 3000); TwkMarkDirty(); }
        float vv = (float)g_landVarMm;
        if (api->SliderFloat("Landing variation (mm)##land", &vv, 0.0f, 60.0f, "%.0f")) { g_landVarMm = clampI((int)(vv + 0.5f), 0, 60); TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(each landing its own spot, only ever toward the middle; 0 = right on the bolts every time)");
        float im = (float)g_landImpactPct;
        if (api->SliderFloat("Landing impact (%)##land", &im, 0.0f, 200.0f, "%.0f")) { g_landImpactPct = clampI((int)(im + 0.5f), 0, 200); TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(the feet take the landing: heels slap down, each foot knocked a touch; more off a bigger fall; 0 = they stay put)");
        bool stp = g_landStep != 0;
        if (api->Checkbox("Step into your stance##land", &stp)) { g_landStep = stp ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(swivel on the ball and heel, back foot first; off = glide)");
        if (stp) {
            float pv = (float)g_landPivotMs;
            if (api->SliderFloat("Swivel speed (ms per pivot)##land", &pv, 80.0f, 300.0f, "%.0f")) { g_landPivotMs = clampI((int)(pv + 0.5f), 80, 300); TwkMarkDirty(); }
        } else if (api->SliderFloat("Glide into your stance (ms)##land", &r, 50.0f, 2000.0f, "%.0f")) { g_landReturnMs = clampI((int)(r + 0.5f), 50, 2000); TwkMarkDirty(); }
        if (api->version >= 2 && api->Button && api->Button("Default landing##land")) DefaultLanding();
    }
    float ll = (float)g_landLeanPct;
    if (api->SliderFloat("Landing lean on the trucks (%)", &ll, 0.0f, 100.0f, "%.0f")) { g_landLeanPct = clampI((int)(ll + 0.5f), 0, 100); TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(off something higher the deck leans on its trucks as you land, toward where your feet came down; % of your trucks' max lean at a big drop; flat stays flat)");
    api->Separator();
    bool fol = g_follow != 0;
    if (api->Checkbox("Follow the deck", &fol)) { g_follow = fol ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(a foot moved on or off the kick rides up or down it)");
    if (api->version >= 2 && api->Button && api->Button("Re-learn my idle")) InterlockedExchange(&g_relearn, 1);
    if (api->version >= 2 && api->Button) {
        api->SameLine(); api->TextDisabled("(where the game's own idle puts your feet: learned once, kept between launches)");
    }
    api->Unindent();
}
