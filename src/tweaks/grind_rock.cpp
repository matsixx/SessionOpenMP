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
// SessionTweaks -- BOARDSLIDE ROCKING.
//
// How the game does grind pressure (read from the shipped code):
//   GrindsHandler::GetGrindOrientRatios turns the sticks into {PitchRatio, YawRatio}: PitchRatio is
//   the DEFLECTION of the stick the grind uses, sqrt(x^2 + y^2). Its mode is the skater's catch orient
//   state (+0x63e): FrontFoot / BackFoot / BothFeetFront / BothFeetBack set it, BothFeet (5 -- the
//   boardslide, both sticks) never does, so it stays 0.
//   SetGrindOrient stores it on the skater (_grindPitchRatio +0x6d8).
//   USkateboardExMovementComponent::PhysGrinding, for a grind whose def has IsUsingPitchAngle:
//       pitch = def->GetTargetPitchAngle() * fabs(_grindPitchRatio), eased into
//       _localAnimationRotator.Pitch (+0x620) -- one direction only.
//   GetLocalAnimatorBoardQuat turns that rotator into the board's local rotation.
// So a tailslide's nose-dip is the stick's deflection times an authored angle, and a boardslide has
// no pressure at all -- the ratio is hard-wired to 0 and the formula could not rock both ways anyway.
//
// This adds the missing term: on a BOTH-FEET grind (the grind def's CatchOrientState, else the
// skater's live one at +0x63e -- the same byte CheckForGrindInput hands GetGrindOrientRatios as its
// mode), the difference between the two sticks' deflections tips the board about the rail -- ease
// one stick and that foot's end rises, the other end dips. Written straight after PhysGrinding (the only writer of that pitch mid-grind;
// PostPhysGrinding touches yaw only) and taken back out before the next add, so it can never
// accumulate: the game's own pitch underneath is left exactly as the game computed it.
// =====================================================================================================
#include "tweaks_common.h"
#include "grind_rock.h"
#include "grind_lean.h"       // the single-foot lean rides this module's PhysGrinding hook
#include "board_stance.h"     // the landing lean rides the same board read
#include "ui/menu_ext.h"
#include "grind_pop.h"       // GrindPop_NameOfFName -- it owns the FName::ToString address
#include "catch_tweaks.h"    // CatchTweaks_Skater() -- remote players keep vanilla
#include "scoop_speed.h"     // ScoopSpeed_StickRaw -- the sticks, off the input tick it already owns
#include "tweaks_mod.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB)
enum {
    MC_SKATER      = 0x340,   // USkateboardExMovementComponent::_skater
    MC_LOCAL_PITCH = 0x620,   // ::_localAnimationRotator.Pitch -- the board's local animation pitch
    MC_LOCAL_ROLL  = 0x628,   // ::_localAnimationRotator.Roll -- the single-foot lean's board roll (grind_lean)
    MC_FLAGS_7E9   = 0x7e9,   // bitfield byte; bit 7 = _isBoardReversedOnGround (last in declaration order)
    SK_TGT_GRIND   = 0x6c8,   // ASkaterCharacterBase::_targetGrindDef
    SK_CUR_GRIND   = 0x6d0,   // ::_currentGrindDef
    SK_MESH        = 0x280,   MESH_ANIM = 0x6b0,
    SK_BOARD       = 0x568,   BOARD_FLIPPER = 0x4e8,   // the deck mesh: its frame is the deck's
    AN_L_SOCK_LOC  = 0x404,   AN_R_SOCK_LOC = 0x41c,   // the feet, mesh space
    COMP_CTW_POS   = 0x1d0,   COMP_CTW_SCALE = 0x1e0,
    SK_ORIENT      = 0x63e,   // ASkaterCharacterBase ECatchOrientState -- which feet are engaged
    GD_NAME        = 0x38,    // UGrindOrSlideDefinition::Name (FName)
    GD_ORIENT      = 0x80,    // ::CatchOrientState
    GD_INPUTS      = 0xd0,    // ::Inputs  TArray<EInputType>: data +0, num +8 (logged only)
    MC_GRINDS_DB   = 0x308,   // USkateboardExMovementComponent::_grindsDb
    GDB_ORIENT_SMOOTHING = 0x90,   // UGrindsDatabase::GrindOrientSmoothing
};
// ECatchOrientState (reflected names, in order): 0 None, 1 FrontFoot, 2 BackFoot, 3 BothFeetFrontFoot,
// 4 BothFeetBackFoot, 5 BothFeet, ... CheckForGrindInput hands skater+0x63e to GetGrindOrientRatios
// as its "mode"; 5 is the case that never sets a pitch ratio.
static const uint8_t ORIENT_BOTH_FEET = 5;

// PhysGrinding(this, float deltaTime, <pointer>): self in rcx, deltaTime in xmm1, a pointer in r8;
// r9 is written before it is read and no stack argument is touched (disassembled, not assumed).
// Epic 0xfc77a0 / Steam 0xf875d0 -- sigmake: unique in both.
static const char* SIG_PHYS_GRINDING =
    "48 8B C4 57 41 57 48 81 EC F8 00 00 00 0F 28 05 ?? ?? ?? ?? 48 8D 54 24 50 44 0F 29 40 B8 0F 57 D2";
typedef void (__fastcall* PhysGrindingFn)(void* self, float dt, void* p3);
static uint8_t* g_start = nullptr;
static void*    g_orig  = nullptr;

// ------------------------------------------------------------------ knobs
static int   g_on     = 1;        // GrindRock
static float g_maxDeg = 4.0f;     // GrindRockMaxDeg -- the tilt with one stick fully off
static float g_easeMs = 120.0f;   // GrindRockEaseMs -- how quickly the board follows the sticks
static float g_curve  = 3.0f;     // GrindRockCurve (x100) -- 1 = linear; higher = a slight ease barely tips it
static float g_swayDeg = 4.0f;    // GrindRockSwayDeg -- the natural rock on a both-feet grind (0 = none)
static float g_popDip  = 0.5f;    // GrindRockPopDip (%) -- the share of the lean kept through a pop (0 = none)
static int   g_ok     = 1;        // runtime health, never persisted
static volatile float g_uiTilt = 0.0f;
static volatile int   g_uiLive = 0;

static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void GrindRock_ReadConfig(const char* buf) {
    g_on     = TwkIniInt(buf, "GrindRock", 1) ? 1 : 0;
    g_maxDeg = (float)clampI(TwkIniInt(buf, "GrindRockMaxDeg", 4), 0, 35);
    g_easeMs = (float)clampI(TwkIniInt(buf, "GrindRockEaseMs", 120), 0, 1000);
    g_curve  = (float)clampI(TwkIniInt(buf, "GrindRockCurve", 300), 100, 300) * 0.01f;
    g_swayDeg = (float)clampI(TwkIniInt(buf, "GrindRockSwayDeg", 4), 0, 10);
    g_popDip  = (float)clampI(TwkIniInt(buf, "GrindRockPopDip", 50), 0, 100) * 0.01f;
}
void GrindRock_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "GrindRock",       g_on);
    TwkIniSetInt(buf, cap, "GrindRockMaxDeg", (int)g_maxDeg);
    TwkIniSetInt(buf, cap, "GrindRockEaseMs", (int)g_easeMs);
    TwkIniSetInt(buf, cap, "GrindRockCurve",  (int)(g_curve * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "GrindRockSwayDeg", (int)g_swayDeg);
    TwkIniSetInt(buf, cap, "GrindRockPopDip", (int)(g_popDip * 100.0f + 0.5f));
}
void GrindRock_ResetDefaults() { g_on = 1; g_maxDeg = 4.0f; g_easeMs = 120.0f; g_curve = 3.0f; g_swayDeg = 4.0f; g_popDip = 0.5f; TwkMarkDirty(); }
bool  GrindRock_Enabled() { return g_on != 0; }
void  GrindRock_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
float GrindRock_MaxDeg() { return g_maxDeg; }
void  GrindRock_SetMaxDeg(float d) { g_maxDeg = d < 0.0f ? 0.0f : (d > 35.0f ? 35.0f : d); TwkMarkDirty(); }
float GrindRock_SwayDeg() { return g_swayDeg; }
void  GrindRock_SetSwayDeg(float v) { g_swayDeg = v < 0.0f ? 0.0f : (v > 10.0f ? 10.0f : floorf(v + 0.5f)); TwkMarkDirty(); }
// Shown as "softness" 0..20 (the pause menu prints integers): curve = 1 + softness / 10.
float GrindRock_Softness() { return (g_curve - 1.0f) * 10.0f; }
void  GrindRock_SetSoftness(float v) {
    v = v < 0.0f ? 0.0f : (v > 20.0f ? 20.0f : v);
    g_curve = 1.0f + floorf(v + 0.5f) * 0.1f; TwkMarkDirty();
}

// ------------------------------------------------------------------ helpers
// A both-feet grind (a boardslide): the grind's own orient state, else the skater's live one.
static bool IsBothFeet(void* skater, void* def) {
    const uint8_t d = *((const uint8_t*)def + GD_ORIENT);
    if (d != 0) return d == ORIENT_BOTH_FEET;
    return *((const uint8_t*)skater + SK_ORIENT) == ORIENT_BOTH_FEET;
}

// One line per new grind: what the gate saw.
static void LogGrind(void* comp, void* skater, void* def, bool both, float dt) {
    static int s_n = 0;
    if (s_n >= 300) return;
    s_n++;
    char name[96] = "?";
    GrindPop_NameOfFName((const uint8_t*)def + GD_NAME, name, sizeof(name));
    char ins[64] = ""; int at = 0;
    const uint8_t* data = (const uint8_t*)twkP(def, GD_INPUTS);
    const int n = *(const int*)((const uint8_t*)def + GD_INPUTS + 8);
    for (int i = 0; data && i < n && i < 8 && at < 56; i++) at += snprintf(ins + at, sizeof(ins) - at, "%s%d", i ? "," : "", data[i]);
    // PhysGrinding eases the pitch by dt x GrindOrientSmoothing each step, which our add rides on
    // (the visible tilt settles near tilt / (dt x smoothing)): both logged to make that exact later.
    void* db = twkP(comp, MC_GRINDS_DB);
    const float sm = db ? twkF(db, GDB_ORIENT_SMOOTHING) : -1.0f;
    TwkLog("[rock] grind '%s': def orient %d, skater orient %d, inputs [%s] -> %s (dt %.4f, orient smoothing %.3f)", name,
           *((const uint8_t*)def + GD_ORIENT), *((const uint8_t*)skater + SK_ORIENT), ins,
           both ? "BOTH FEET: the sticks rock the board" : "not both feet, left alone", dt, sm);
}

// How steeply the deck's own +X runs uphill in the world (the z of its X axis). Logged next to the
// tilt so one run shows which way a positive pitch actually tips the deck.
static float DeckSlopeX(void* skater) {
    void* bd  = twkP(skater, SK_BOARD);
    void* flp = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    float qf[4];
    if (!flp || !TwkCompQuat(flp, qf)) return 0.0f;
    const float x[3] = { 1.0f, 0.0f, 0.0f };
    float w[3]; TwkQuatRotate(qf, x, w);
    return w[2];
}

// Which end of the board the LEFT foot stands on: +1 toward the deck's own +X, -1 toward -X. The feet
// sit on either side of the rail, 40 cm apart, so this cannot flicker.
static float LeftFootEnd(void* skater) {
    void* mesh = twkP(skater, SK_MESH);
    void* anim = mesh ? twkP(mesh, MESH_ANIM) : nullptr;
    void* bd   = twkP(skater, SK_BOARD);
    void* flp  = bd ? twkP(bd, BOARD_FLIPPER) : nullptr;
    float qm[4], qf[4];
    if (!anim || !flp || !TwkCompQuat(mesh, qm) || !TwkCompQuat(flp, qf)) return 0.0f;
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
    if (fabsf(x[0] - x[1]) < 5.0f) return 0.0f;           // feet not apart along the deck: say nothing
    return (x[0] > x[1]) ? 1.0f : -1.0f;
}

// ------------------------------------------------------------------ the rock
// EXACT, NOT A FEEDBACK LOOP. PhysGrinding eases +0x620 every step by k = dt x GrindOrientSmoothing
// toward target x |ratio| (0 on a both-feet grind; 0xfc7bdf..0xfc7c01) and then READS it, inside the
// same step, through GetLocalAnimatorBoardQuat (0xfc7d1a) into the flipper's PID target. So the tilt
// cannot live in +0x620 around the step: added after it, the easing breaks any exact-match removal and
// the adds stack -- the board settles at tilt / k, ~36x at a 0.0056 s step (smoothing 5; measured:
// 0.4-0.7 deg commanded showed 17-27), framerate-dependent; taken out before it, it is never there when
// the game reads it (measured: lean +40 deg, deck slope 0.001). It goes in ONLY for that read -- a hook
// on GetLocalAnimatorBoardQuat adds it and puts the game's own value back straight after -- so the
// game's pitch state never carries it, and the visible lean follows kLegacyGain x the stick lean on the
// game's own smoothing: the stacked feel the strength knob was tuned on (0.0056 s), at any framerate.
// The knob's numbers are that loop's, not degrees.
static const float kLegacyGain = 35.7f;          // 1 / (0.0056 s x 5): the loop's gain the rider tuned against
static const float kMaxVisibleDeg = 45.0f;       // a fully released stick asked for ~140 deg; the log never passed 27
static float    s_tilt = 0.0f;       // the stick lean, eased (strength units)
static float    s_lean = 0.0f;       // the visible lean it makes (deg)
static float    s_apply = 0.0f;      // what the board's read gets on top of the game's pitch (deg)
static uint64_t s_lastMs = 0;
static void*    s_lastDef = nullptr;
static void*    s_comp = nullptr;    // whose read gets it: your skater's movement component
// THE POP IS THE GAME'S (509). "When I try to ollie or do any trick out of them, it dips my board too much
// for the pop." The pop (JumpForTrick) comes ~0.2 s before the board leaves grind mode (507 log: GRIND
// EXIT 00.287, mode 2 -> 3 at 00.481), and a pop out of a both-feet grind starts with one stick let go --
// so the lean was growing through the game's own pop pitch, then carried into the air (held 80 ms,
// eased over ~0.3 s) where no rail holds the board. From the pop the lean and the sway ease out fast.
// ...but not all of it (510): "I kinda liked that effect it was just too strong". The pop keeps
// GrindRockPopDip of the lean it found (509 log: 15-35 deg at the pop), stops it growing, and carries that
// share the old way -- through the rest of the grind, held 80 ms after, then eased at the follow speed.
static const float kPopEaseSec = 0.04f;
static float    s_popHold = 0.0f;    // the lean the pop keeps (deg)
static uint64_t s_grindStartMs = 0;
static bool     s_popped = false;    // this grind has popped: the board is the game's
static float    s_popFade = 1.0f;    // the natural rock's share after the pop

// THE NATURAL ROCK: a board balanced across a rail is never still. On the way in it teeters on its
// pivot and settles; after that the rider's balance keeps it swaying -- a few slow waves out of step
// with each other (new ones every grind, so it never loops), quieter the more deliberately he leans.
static float    s_grindT = 0.0f;
static float    s_fq[3], s_ph[3], s_teeter = 1.0f;
static uint32_t s_rng = 0;
static float Rand01() {
    if (!s_rng) s_rng = (uint32_t)GetTickCount64() | 1u;
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / 16777216.0f;
}
static void RollRock() {
    const float lo[3] = { 0.28f, 0.70f, 1.50f }, span[3] = { 0.14f, 0.25f, 0.40f };
    for (int i = 0; i < 3; i++) { s_fq[i] = lo[i] + span[i] * Rand01(); s_ph[i] = 6.2831853f * Rand01(); }
    s_teeter = Rand01() < 0.5f ? -1.0f : 1.0f;
}
static float NaturalRock(float lean01) {
    if (g_swayDeg <= 0.0f) return 0.0f;
    const float T = s_grindT, tau = 6.2831853f;
    const float fade = T < 0.4f ? T / 0.4f : 1.0f;
    const float calm = 1.0f - 0.7f * (lean01 > 1.0f ? 1.0f : lean01);
    const float sway = 0.6f * sinf(tau * s_fq[0] * T + s_ph[0]) + 0.3f * sinf(tau * s_fq[1] * T + s_ph[1])
                     + 0.1f * sinf(tau * s_fq[2] * T + s_ph[2]);
    const float teeter = 2.2f * s_teeter * expf(-T / 0.35f) * sinf(tau * 1.4f * T);
    return g_swayDeg * (sway * fade * calm + teeter);
}

static void Rock(void* comp, float dt) {
    void* skater = twkP(comp, MC_SKATER);
    if (!skater) return;
    void* mine = CatchTweaks_Skater();
    if (mine && mine != skater) return;                   // remote players keep vanilla

    if (!(dt > 0.0f && dt < 0.25f)) dt = 1.0f / 60.0f;
    // A new grind (PhysGrinding went quiet in between) starts its own natural rock, and its lean from
    // whatever the ease-out had left of the last one -- usually nothing.
    const uint64_t now = GetTickCount64();
    // Back on a grind after a pop is a new grind however short the air (hops along a rail: 0.15 s).
    if (now - s_lastMs > 150 || (s_popped && now - s_lastMs > 40)) {
        s_lastDef = nullptr; s_grindT = 0.0f; RollRock();
        s_lean = s_apply; s_tilt = s_apply / kLegacyGain;
        s_grindStartMs = now; s_popped = false; s_popFade = 1.0f;
    }
    s_lastMs = now;
    s_grindT += dt;

    static bool s_ran = false;
    if (!s_ran) { s_ran = true; TwkLog("[rock] PhysGrinding is running on your skater"); }

    void* def = twkP(skater, SK_CUR_GRIND);
    if (!def) def = twkP(skater, SK_TGT_GRIND);
    const bool two = g_on && def && IsBothFeet(skater, def);
    if (def && def != s_lastDef) LogGrind(comp, skater, def, two, dt);
    s_lastDef = def;

    // The difference between the sticks' deflections: ease the left and the left foot's end rises.
    float target = 0.0f, lMag = 0.0f, rMag = 0.0f, side = 0.0f, lean01 = 0.0f;
    bool sticks = false, reversed = false;
    if (two) {
        float lx, ly, rx, ry;
        if (ScoopSpeed_StickRaw(false, &lx, &ly) && ScoopSpeed_StickRaw(true, &rx, &ry)) {
            sticks = true;
            lMag = sqrtf(lx * lx + ly * ly); if (lMag > 1.0f) lMag = 1.0f;
            rMag = sqrtf(rx * rx + ry * ry); if (rMag > 1.0f) rMag = 1.0f;
            // > 0: the left stick is the eased one. Two held sticks never read exactly equal.
            const float d = rMag - lMag, dz = 0.06f;
            float t = (fabsf(d) - dz) / (1.0f - dz);
            if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            lean01 = t;
            // Over the WHOLE stick: the boardslide holds with a stick fully let go (measured: L 0.00
            // still BS_Boardslide, orient 5). The curve keeps a slight ease a slight lean.
            t = powf(t, g_curve) * (d < 0.0f ? -1.0f : 1.0f);
            side = LeftFootEnd(skater);
            // A positive pitch raises the deck's +X end (measured: tilt +6.9 with the left foot on +X
            // took the +X slope positive), so the left foot's end rises when the left foot is on +X --
            // on a board the right way round. GetLocalAnimatorBoardQuat NEGATES the pitch while
            // _isBoardReversedOnGround is set (bit 7 of +0x7e9: `cmp byte [this+0x7e9],0 / jge / xorps`),
            // which a 180 shove leaves set and a 360 clears ("after a pop shove-it the weight
            // distribution becomes opposite ... a 360 shove into a boardslide was fine").
            reversed = (*((const uint8_t*)comp + MC_FLAGS_7E9) & 0x80) != 0;
            target = t * g_maxDeg * side * (reversed ? -1.0f : 1.0f);
        }
    }
    const unsigned long long pop = GrindPop_LastJumpMs();
    if (!s_popped && pop > s_grindStartMs) {
        s_popped = true;
        s_popHold = s_lean * g_popDip;
        static int s_n = 0;
        if (two && s_n < 100) {
            s_n++;
            char name[96] = "?";
            GrindPop_NameOfFName((const uint8_t*)def + GD_NAME, name, sizeof(name));
            TwkLog("[rock] pop out of '%s' %.2f s in: lean %+.1f deg + natural rock %+.1f at the pop (sticks L %.2f R %.2f) -- eased out for the game's pop",
                   name, s_grindT, s_lean, NaturalRock(lean01), lMag, rMag);
            TwkLog("[rock]   the pop keeps %.0f%% of it: %+.1f deg", g_popDip * 100.0f, s_popHold);
        }
    }
    if (s_popped) {
        const float kq = 1.0f - expf(-dt / kPopEaseSec);
        s_tilt = 0.0f;
        s_lean += (s_popHold - s_lean) * kq;
        s_popFade -= s_popFade * kq;
    } else {
        const float k = (g_easeMs <= 0.0f) ? 1.0f : (1.0f - expf(-dt * 1000.0f / g_easeMs));
        s_tilt += (target - s_tilt) * k;
        // The visible lean, on the game's own smoothing (GrindOrientSmoothing, 5 in the logs).
        void* db = twkP(comp, MC_GRINDS_DB);
        float S = db ? twkF(db, GDB_ORIENT_SMOOTHING) : 0.0f;
        if (!(S > 0.1f && S < 100.0f)) S = 5.0f;
        float want = kLegacyGain * s_tilt;
        if (want > kMaxVisibleDeg) want = kMaxVisibleDeg; else if (want < -kMaxVisibleDeg) want = -kMaxVisibleDeg;
        s_lean += (want - s_lean) * (1.0f - expf(-S * dt));
    }

    const float cur = *(const float*)((const uint8_t*)comp + MC_LOCAL_PITCH);   // the game's own, never ours
    const float rock = two ? NaturalRock(lean01) * s_popFade : 0.0f;
    if (!two && fabsf(s_lean) < 0.01f) {
        s_tilt = 0.0f; s_lean = 0.0f; s_apply = 0.0f; g_uiTilt = 0.0f; g_uiLive = 0;
        return;
    }
    const float total = s_lean + rock;
    s_apply = total; s_comp = comp;                       // the next read of the pitch gets it
    g_uiTilt = total; g_uiLive = two ? 1 : 0;

    // Twice a second while on a both-feet grind: the sticks, the lean, and the deck's actual slope.
    static int s_said = 0; static float s_logT = 0.0f;
    s_logT += dt;
    if (two && s_logT > 0.5f && s_said < 400) {
        s_logT = 0.0f; s_said++;
        if (!sticks) TwkLog("[rock] both feet, but the sticks are unreadable this tick -- holding still");
        else TwkLog("[rock] sticks L %.2f R %.2f -> lean %+.1f deg + natural rock %+.1f (game's own pitch %+.1f), left foot on %s, deck +X slope %+.3f%s",
                    lMag, rMag, s_lean, rock, cur, side > 0.0f ? "+X" : (side < 0.0f ? "-X" : "?"), DeckSlopeX(skater),
                    reversed ? ", board reversed" : "");
    }
}

static void __fastcall hkPhysGrinding(void* self, float dt, void* p3) {
    ((PhysGrindingFn)g_orig)(self, dt, p3);
    GrindLean_OnPhysGrinding(self, dt);                   // its own guard and fault handling
    if (!g_ok || !self) return;
    __try { Rock(self, dt); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[rock] caught fatal after PhysGrinding -- boardslide rocking off for this run");
    }
}

// GetLocalAnimatorBoardQuat(this, FQuat* out) -- Epic 0xfbdbc0 / Steam 0xf7d9f0, sigmake: unique in
// both. Builds the board's target rotation from +0x620/4/8. Ours is added for the read and the game's
// own value put back straight after (it only reads the field), so the pitch state never carries ours.
static const char* SIG_LOCAL_QUAT =
    "48 8B C4 48 89 58 08 48 89 70 10 57 48 81 EC A0 00 00 00 80 B9 E9 07 00 00 00 48 8B F2 F3 0F 10 81 20 06 00 00 48 8B D9";
typedef void* (__fastcall* LocalQuatFn)(void* self, void* out);
static uint8_t* g_quatAt   = nullptr;
static void*    g_quatOrig = nullptr;

static void* __fastcall hkLocalQuat(void* self, void* out) {
    float* pitch = nullptr; float own = 0.0f, wrote = 0.0f;
    float* roll = nullptr;  float ownR = 0.0f, wroteR = 0.0f;
    if (g_ok && self) {
        // The rock's pitch, plus grind_lean's foot pressure tilt on a 50-50 style grind (516), same read.
        const float pAdd = ((self == s_comp) ? s_apply : 0.0f) + GrindLean_BoardPitch(self);
        if (pAdd != 0.0f) {
            __try {
                pitch = (float*)((uint8_t*)self + MC_LOCAL_PITCH);
                own = *pitch;
                if (own > -1e4f && own < 1e4f) { wrote = own + pAdd; *pitch = wrote; }
                else pitch = nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) { pitch = nullptr; }
            static bool s_said = false;
            if (pitch && !s_said) { s_said = true; TwkLog("[rock] the lean reaches the board (through GetLocalAnimatorBoardQuat)"); }
        }
        // The single-foot lean's roll rides the same read (grind_lean decides it, 0 when not leaning), and so
        // does the landing lean on the trucks (board_stance, 0 outside a landing off a drop).
        const float land = Stance_LandingRoll(self);
        const float add = GrindLean_BoardRoll(self) + land;
        static bool s_saidLand = false;
        if (land != 0.0f && !s_saidLand) { s_saidLand = true; TwkLog("[stance] the landing lean reaches the board (through GetLocalAnimatorBoardQuat)"); }
        if (add != 0.0f) {
            __try {
                roll = (float*)((uint8_t*)self + MC_LOCAL_ROLL);
                ownR = *roll;
                if (ownR > -1e4f && ownR < 1e4f) { wroteR = ownR + add; *roll = wroteR; }
                else roll = nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) { roll = nullptr; }
        }
    }
    void* r = ((LocalQuatFn)g_quatOrig)(self, out);
    if (pitch) {
        __try { if (*pitch == wrote) *pitch = own; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (roll) {
        __try { if (*roll == wroteR) *roll = ownR; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// The grind ended mid-lean: PhysGrinding stopped updating it, so it eases out here (the board's reads
// after the pop get what is left) instead of vanishing in one step.
static void EaseOut() {
    if (s_apply == 0.0f || !s_comp) return;
    const uint64_t now = GetTickCount64();
    const uint64_t gap = now - s_lastMs;
    if (gap < 80) return;                                 // still grinding
    if (gap > 2000 || twkP(s_comp, MC_SKATER) != CatchTweaks_Skater()) { s_apply = 0.0f; s_comp = nullptr; return; }
    static uint64_t s_prevMs = 0;
    float dt = (s_prevMs && now > s_prevMs) ? (float)(now - s_prevMs) * 0.001f : 1.0f / 60.0f;
    s_prevMs = now;
    if (dt > 0.1f) dt = 0.1f;
    const float k = (g_easeMs <= 0.0f) ? 1.0f : (1.0f - expf(-dt * 1000.0f / g_easeMs));
    float left = s_apply * (1.0f - k);
    if (fabsf(left) < 0.01f) left = 0.0f;
    s_apply = left;
}
void GrindRock_PumpFrame() {
    if (!g_ok) return;
    __try { EaseOut(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s_apply = 0.0f; s_comp = nullptr; }
}

// ------------------------------------------------------------------ install + menu
void GrindRock_Install() {
    g_start = TwkScanExe(SIG_PHYS_GRINDING);
    if (!g_start) { TwkLog("[rock] PhysGrinding sig NOT FOUND -- boardslide rocking off (game updated?)"); return; }
    if (MH_CreateHook(g_start, (void*)&hkPhysGrinding, &g_orig) != MH_OK || MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[rock] hook failed on PhysGrinding -- boardslide rocking off");
        g_start = nullptr; return;
    }
    g_quatAt = TwkScanExe(SIG_LOCAL_QUAT);
    if (!g_quatAt || MH_CreateHook(g_quatAt, (void*)&hkLocalQuat, &g_quatOrig) != MH_OK || MH_EnableHook(g_quatAt) != MH_OK) {
        TwkLog("[rock] GetLocalAnimatorBoardQuat %s -- boardslide rocking off (game updated?)", g_quatAt ? "hook failed" : "sig NOT FOUND");
        g_quatAt = nullptr; g_ok = 0; return;
    }
    TwkLog("[rock] installed @ %p, board read @ %p (%s, tilt strength %.0f, natural rock %.0f deg)", g_start, g_quatAt,
           g_on ? "ON" : "off", g_maxDeg, g_swayDeg);
}

void GrindRock_DrawMenu(const OmpMenuApi* api) {
    bool on = g_on != 0;
    if (api->Checkbox("Boardslide rocking", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(on a boardslide, ease off a stick and that end rises)");
    if (!on) return;
    api->Indent();
    float d = g_maxDeg, e = g_easeMs;
    if (api->SliderFloat("Tilt strength", &d, 0.0f, 35.0f, "%.0f")) GrindRock_SetMaxDeg(d);
    api->SameLine(); api->TextDisabled("(how far easing a stick tips the board; 4 = ~25 deg at half a stick)");
    float c = GrindRock_Softness();
    if (api->SliderFloat("Softness", &c, 0.0f, 20.0f, "%.0f")) GrindRock_SetSoftness(c);
    api->SameLine(); api->TextDisabled("(0 = linear; higher = a slight ease barely tips it)");
    float sw = g_swayDeg;
    if (api->SliderFloat("Natural rock (deg)", &sw, 0.0f, 10.0f, "%.0f")) GrindRock_SetSwayDeg(sw);
    api->SameLine(); api->TextDisabled("(teeter on the way in, then a balancing sway; 0 = none)");
    float pd = g_popDip * 100.0f;
    if (api->SliderFloat("Pop dip (%)", &pd, 0.0f, 100.0f, "%.0f")) { g_popDip = floorf(pd + 0.5f) * 0.01f; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(how much of the lean the board keeps as you pop out; 0 = none, 100 = all of it)");
    if (api->SliderFloat("Follow speed (ms)", &e, 0.0f, 500.0f, "%.0f")) { g_easeMs = e; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(lower = the board follows the sticks faster)");
    char b[96];
    snprintf(b, sizeof(b), g_uiLive ? "now: rocking, %+.1f deg on the deck" : "now: not on a both-feet grind", (float)g_uiTilt);
    api->TextDisabled(b);
    if (!g_start) api->TextDisabled("PhysGrinding not found -- off (see the log)");
    api->Unindent();
}
