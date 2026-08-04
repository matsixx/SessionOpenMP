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
// SessionTweaks -- GRIND POP probe (read-only; writes nothing to the game).
//
// Popping a flip trick out of a grind runs a DIFFERENT pop path from flat ground and from manuals,
// and this module measures it. The path, read from the shipped code:
//
//   FlipTricksHandler::CheckForTrick
//       picks the crank anim min time three ways -- UTricksDatabase::CrankAnimMinTime (flat),
//       CrankGrindAnimMinTime (grind), CrankManualAnimMinTime (manual) -- and, ON GRINDS ONLY,
//       computes  GrindCrankDurationRatio = clamp01((crankDuration - CrankGrindAnimMinTime)
//                                                 / (CrankAnimMinTime - CrankGrindAnimMinTime)).
//       Off a grind that ratio is forced to 0.
//   FlipTricksHandler::ExecuteTrick
//       on a grind, REPLACES the crank-scaled flat pop height with a static-asset sum
//       (GrindExitPopHeight or GrindBasePopHeight, + the trick's AdditionalPopHeightGrind, + the
//       grind def's AdditionalPopHeight, times an advanced setting), and clamps the ratio above by
//       the trick def's MaxCrankRatioOnGrinds.
//   ASkaterCharacterBase::SetTrick
//       passes the pop ratio as 0 when (IsOnGrind && _isExitingGrind), handing JumpForTrick the
//       GrindCrankDurationRatio instead.
//   USkaterMovementComponent::JumpForTrick   <-- HOOKED HERE
//       ignores the pop height and pop ratio it is handed (both already live on the skater), stores
//       the grind ratio as _grindCrankDurationRatio, and picks the pop pelvis DELAY by board mode:
//         grind  -> lerp(AnimGrindMomentumPopPelvisDelay, AnimGrindPopPelvisDelay, that ratio)
//         manual -> UManualsDatabase::AnimPopPelvisDelay
//         primo  -> UTricksDatabase::PrimoPopPelvisDelay
//         flat   -> 0, i.e. the jump values are set immediately
//
// So a grind exit's pop is delayed by an amount that is a pure function of one ratio, and at ratio 0
// that lerp lands exactly on the MOMENTUM pop -- which is what "the board doesn't angle the tail, it
// just floats you up" looks like. This probe answers which term pins the ratio: a short crank
// against CrankGrindAnimMinTime, or a small per-trick MaxCrankRatioOnGrinds.
//
// Deliberately measurement-only. The correction depends entirely on which term dominates, and a fix
// shipped before the numbers exist is the mistake this investigation has already made repeatedly.
// The hook records POD into a ring and returns; names are resolved and lines written from the pump,
// outside the game's callstack.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "grind_pop.h"
#include "tweaks_mod.h"
#include <cmath>
#include <cctype>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    OBJ_NAME              = 0x18,   // UObjectBase::NamePrivate (FName)
    // USkaterMovementComponent
    MC_SKATER             = 0xb28,  // ASkaterCharacterBase*
    MC_GRINDS_DB          = 0xb40,  // UGrindsDatabase*
    MC_TRICKS_DB          = 0xb50,  // UTricksDatabase*
    MC_JUMP_DELAY         = 0xcc8,  // _setJumpForTrickDelay  -- THE OUTPUT this probe exists for
    MC_GRIND_CRANK_RATIO  = 0xcd0,  // _grindCrankDurationRatio
    // ASkaterCharacterBase
    SK_CUR_FLIP_DEF       = 0x590,  // UFlipTrickDefinition* _currentFlipTrickDef
    SK_TRICK_POP_RATIO    = 0x624,  // _skateboardingAnimParams(+0x610).TrickPopRatio(+0x14)
    SK_CUR_GRIND_DEF      = 0x6d0,  // UGrindOrSlideDefinition* _currentGrindDef
    SK_FLAGS              = 0x710,  // bitfield: bit2 _isPopPending, bit3 _isExitingGrind
    SK_TRICK_POP_HEIGHT   = 0x8f4,  // _trickPopHeight
    // The multiplier ExecuteTrick's grind branch applies to the assembled grind pop height. Read at
    // this offset by that branch; it sits inside _advancedSettings, which the PDB reports as a
    // nested struct rather than a flat member, so the name is not proven -- the value is logged raw.
    SK_ADV_GRIND_POP      = 0x9bc,
    // UFlipTrickDefinition -- both of these apply ON GRINDS ONLY; flat ground uses the
    // AdditionalPopHeightMin/Max pair at +0x54/+0x58 instead.
    TD_MAX_CRANK_ON_GRIND = 0x4c,   // MaxCrankRatioOnGrinds
    TD_ADD_POP_GRIND      = 0x5c,   // AdditionalPopHeightGrind
    // UGrindOrSlideDefinition
    GD_NAME               = 0x38,   // FName Name -- the designer-facing label (50-50, Nosegrind, ...)
    GD_APPROACH           = 0x5c,   // ESkaterApproachType
    GD_ADD_POP            = 0x60,   // AdditionalPopHeight
    // UGrindsDatabase
    GDB_BASE_POP          = 0x44,   // GrindBasePopHeight
    GDB_EXIT_POP          = 0xb0,   // GrindExitPopHeight
    GDB_POP_DELAY         = 0xe0,   // AnimGrindPopPelvisDelay          (ratio 1 end of the lerp)
    GDB_MOM_POP_DELAY     = 0xe4,   // AnimGrindMomentumPopPelvisDelay  (ratio 0 end -- the "float")
    // UTricksDatabase
    TDB_CRANK_MIN         = 0x34,   // CrankAnimMinTime       (flat)
    TDB_CRANK_GRIND_MIN   = 0x38,   // CrankGrindAnimMinTime  (grind)
    // PitchBoard's `this` is NOT the movement component -- it is the pitch sub-object the component
    // embeds at component+0x138. Reading these off the component lands on unrelated fields that
    // dereference cleanly, which is why a previous attempt at this fix wrote to the wrong address
    // and appeared to do nothing. The sub-object is self-verifying: its +0x208 is the skater.
    PB_SKATER             = 0x208,  // back-pointer -- the identity proof for `this`
    PB_STATE              = 0x380,  // the mode's SECOND discriminator; 3 = actually on a grind
    PB_GRINDDEF           = 0x388,  // UGrindOrSlideDefinition*, only meaningful while state == 3
    PB_FOOTPOS            = 0x739,  // EFootPositionType -- selects which authored pitch entry applies
    PB_TARGET             = 0x748,  // _pitchTargetAngle
    PB_EXTRA              = 0x74c,  // _pitchExtraAngle -- what Board Control Mode feeds
    SK_MOVEMENT_COMP      = 0x550,  // ASkaterCharacterBase -> USkaterMovementComponent*
    // UFlipTrickDefinition::TrickStartPitchData -- the POP's tail-down swing, an FAnimPitchData.
    // TargetAngleMin/Max land at +0x1f4/+0x1f8, which is what PitchBoard mode 1 reads.
    TD_START_PITCH        = 0x1e8,
    // UGrindOrSlideDefinition, the authored grind-exit pitch
    GD_OVERRIDE_START     = 0x125,  // OverrideTrickStartPitch -- makes mode 1 skip the pop swing
    GD_OVERRIDE_END       = 0x158,  // OverrideTrickEndPitch -- the per-grind opt-in flag
    GD_END_PITCH          = 0x160,  // FAnimPitchData, the default entry
    GD_END_ADDL           = 0x180,  // TArray<FAnimPitchData> AdditionalTrickGrindEndPitchData: data
    GD_END_ADDL_NUM       = 0x188,  //   ... and count
    GD_POPRATIO_MOD       = 0x190,  // AnimTrickPopRatioModifier
    // FAnimPitchData (32 B). The Min/Max pairs are lerped by TrickPopRatio, so a ratio pinned at 0
    // makes both Max values unreachable -- which is the shape of the bug this logs.
    APD_FOOT = 0x00, APD_TMIN = 0x04, APD_TMAX = 0x08, APD_AMIN = 0x0c, APD_AMAX = 0x10,
    APD_STRIDE = 0x20,
};

// EPitchMode, read from PitchBoard's own switch. Only mode 2 consumes _pitchExtraAngle: a scan of
// the whole function finds exactly one read of +0x74c, inside that branch.
enum {
    // Mode 1 is the POP: target = lerp(trick->TrickStartPitchData.TargetAngleMin/Max, TrickPopRatio),
    // i.e. how far the popping end of the board swings down. It early-outs entirely when the grind
    // def sets OverrideTrickStartPitch, which is why the board pops off a smith without ever
    // dropping its tail toward the rail.
    PITCH_MODE_POP       = 1,
    PITCH_MODE_TRICK     = 2,   // target = _pitchExtraAngle (your input), but it early-outs on grinds
    PITCH_MODE_START     = 3,   // trick START pitch
    PITCH_MODE_END       = 4,   // trick END pitch
    // Mode 4 is NOT grind-specific: it switches again on PB_STATE, and only state 3 takes the grind
    // path (GetTrickGrindEndPitchData). States 4 and 5 read authored data out of +0x1d8 and are what
    // flat-ground tricks use -- adding the player's angle there double-pitches every normal trick.
    PITCH_STATE_GRIND    = 3,
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int  g_on   = 1;    // GrindPopLog: the probe itself
static int  g_flat = 1;    // GrindPopLogFlat: also log non-grind tricks, for the baseline comparison
// Give the grind-exit pitch mode the input term the flat trick mode has. Ships OFF so the first
// session with it is an A/B: the pitch-mode log lines are produced either way.
static int  g_pitchFix   = 0;    // GrindPitchFix
static int  g_pitchScale = 100;  // GrindPitchScale, percent of the player's extra pitch angle
// Restore the pop's tail-down swing on grind exits. Ships OFF; the blend is how far to move from
// whatever the game left (the grind's static pose, or its floored swing) toward the trick's own
// full swing, so a grind whose authored pose points the other way can be dialled back.
static int  g_popSwing      = 0;    // GrindPopSwing
static int  g_popSwingBlend = 100;  // GrindPopSwingBlend, percent
// Which grinds get the swing back. Comma-separated, matched as a case-insensitive SUBSTRING of the
// grind definition's name, so one entry covers the FS_/BS_ pair and any suffix the assets carry.
// Editable because the asset names are the game's, not ours -- an entry that matches nothing is
// visible in the log as a grind that stayed disabled.
static char g_swingGrinds[192] = "smith,feeble,willy,50-50";
static volatile LONG g_faults = 0;
static int  g_ok   = 1;    // this module's OWN kill-switch; a fault here disables nothing else
// F1 readout of the most recent grind exit (game thread writes, render thread reads)
static volatile float g_uiRatio = 0, g_uiCap = 0, g_uiDelay = 0, g_uiMom = 0, g_uiPop = 0;
static volatile LONG  g_uiCount = 0;
static char           g_uiGrind[64] = {0};

void GrindPop_ReadConfig(const char* buf) {
    g_on         = TwkIniInt(buf, "GrindPopLog", 1);
    g_flat       = TwkIniInt(buf, "GrindPopLogFlat", 1);
    g_pitchFix   = TwkIniInt(buf, "GrindPitchFix", 0);
    g_pitchScale = TwkIniInt(buf, "GrindPitchScale", 100);
    g_popSwing      = TwkIniInt(buf, "GrindPopSwing", 0);
    g_popSwingBlend = TwkIniInt(buf, "GrindPopSwingBlend", 100);
    TwkIniStr(buf, "GrindPopSwingGrinds", g_swingGrinds, sizeof(g_swingGrinds), "smith,feeble,willy,50-50");
    TwkLog("[gpop] config: GrindPopLog=%d GrindPopLogFlat=%d GrindPitchFix=%d GrindPitchScale=%d "
           "GrindPopSwing=%d GrindPopSwingBlend=%d GrindPopSwingGrinds='%s'",
           g_on, g_flat, g_pitchFix, g_pitchScale, g_popSwing, g_popSwingBlend, g_swingGrinds);
}

void GrindPop_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "GrindPopLog",     g_on);
    TwkIniSetInt(buf, cap, "GrindPopLogFlat", g_flat);
    TwkIniSetInt(buf, cap, "GrindPitchFix",   g_pitchFix);
    TwkIniSetInt(buf, cap, "GrindPitchScale", g_pitchScale);
    TwkIniSetInt(buf, cap, "GrindPopSwing",      g_popSwing);
    TwkIniSetInt(buf, cap, "GrindPopSwingBlend", g_popSwingBlend);
    TwkIniSetStr(buf, cap, "GrindPopSwingGrinds", g_swingGrinds);
}

// ------------------------------------------------------------------ which grinds get the swing
// Keyed by the definition POINTER, decided once per grind from its name. The decision needs the name
// table, which is only touched from the pump, so the hook reads this and never resolves anything: an
// unseen grind simply stays stock until the pump has named it (the pitch modes fire in the order
// 3,1,4,2 with the pump running between them, so in practice it is known before the swing is due).
struct GrindAllow { const void* def; int allowed; };
static GrindAllow g_allow[32];
static int g_nAllow = 0;

static int AllowedFor(const void* gd) {           // 1 yes, 0 no, -1 not decided yet
    for (int i = 0; i < g_nAllow; i++) if (g_allow[i].def == gd) return g_allow[i].allowed;
    return -1;
}
static void RememberGrind(const void* gd, const char* name) {
    if (!gd || !name || !name[0] || AllowedFor(gd) >= 0 || g_nAllow >= (int)(sizeof(g_allow) / sizeof(g_allow[0])))
        return;
    char lname[80];
    size_t n = 0;
    for (; name[n] && n < sizeof(lname) - 1; n++) lname[n] = (char)tolower((unsigned char)name[n]);
    lname[n] = 0;
    int ok = 0;
    const char* p = g_swingGrinds;
    while (*p && !ok) {
        while (*p == ' ' || *p == ',') p++;
        char tok[48]; size_t t = 0;
        while (*p && *p != ',' && t < sizeof(tok) - 1) tok[t++] = (char)tolower((unsigned char)*p++);
        while (t > 0 && tok[t - 1] == ' ') t--;         // trailing spaces around a comma
        tok[t] = 0;
        if (t && strstr(lname, tok)) ok = 1;
    }
    g_allow[g_nAllow].def = gd; g_allow[g_nAllow].allowed = ok; g_nAllow++;
    TwkLog("[gpop] grind '%s' -> pop swing %s", name,
           ok ? "ENABLED" : "left stock (not in GrindPopSwingGrinds)");
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
// USkaterMovementComponent::JumpForTrick -- Epic 0xff8890 / Steam 0xfb86c0, unique in both.
static const char* SIG_JUMP_FOR_TRICK =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B D9 0F 29 74 24 30 48 8B 89 30 0B 00 00 0F 28 F3";
// USkateboardExMovementComponent::PitchBoard -- Epic 0xfd0c10 / Steam 0xf90a40, unique in both.
// Arity established from the CALLEE, since it is only ever called virtually: no xmm3 read, r9 only
// ever written, and nothing read at rsp+0x80 or above => (this, uint8_t mode, float), three args.
static const char* SIG_PITCH_BOARD =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 0F B6 F2 48 8B D9 48 8B 91 08 02 00 00 8B CE";
// FName::ToString(FString&) -- Epic 0x133a4f0 / Steam 0x12fb7b0. CALLED, never hooked, and only from
// the pump. Sig copied verbatim from src/game/game_syms.cpp, where it is proven on both exes.
static const char* SIG_FNAME_TOSTR =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00";

typedef void (*FNameToStrFn)(const void*, void*);
static FNameToStrFn g_fnameToStr = nullptr;

// An FName's text never changes, so caching by the FName value bounds the FString churn at one
// resolution per distinct name.
static bool NameOfFName(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    if (!fnamePtr || !g_fnameToStr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fnamePtr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;                                    // NAME_None
    struct Entry { uint64_t key; char name[64]; };
    static Entry cache[64];
    static int nCached = 0;
    for (int i = 0; i < nCached; i++)
        if (cache[i].key == key) { strncpy_s(out, (size_t)cap, cache[i].name, _TRUNCATE); return out[0] != 0; }
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        g_fnameToStr(fnamePtr, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        if (k > 0 && nCached < (int)(sizeof(cache) / sizeof(cache[0]))) {
            cache[nCached].key = key;
            strncpy_s(cache[nCached].name, sizeof(cache[nCached].name), out, _TRUNCATE);
            nCached++;
        }
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
// A UObject's own name, for the trick definition asset.
static bool NameOfObject(const void* obj, char* out, int cap) {
    out[0] = 0;
    if (!obj) return false;
    return NameOfFName((const uint8_t*)obj + OBJ_NAME, out, cap);
}

// ------------------------------------------------------------------ the record ring
// The hook fills POD and returns. Everything that allocates, formats or walks the name table happens
// in the pump: calling game APIs from inside a hooked game callstack is how a previous feature
// faulted mid-bail and disabled itself on the first trick of every session.
struct Rec {
    volatile LONG ready;
    float argHeight, argPopRatio, argGrindRatio;   // the three floats JumpForTrick is handed
    int   isExitingGrind, isPopPending;
    float trickPopRatio, trickPopHeight, advGrindPop;
    const void* trickDef;
    const void* grindDef;
    float maxCrankOnGrind, addPopGrind;            // trick def, grind-only fields
    int   approach;
    float grindAddPop;                             // grind def
    float basePop, exitPop, popDelay, momPopDelay; // grinds db
    float crankMin, crankGrindMin;                 // tricks db
    float outDelay, outGrindRatio;                 // read back AFTER the original ran
    int   ret;
};
static const int kRecs = 16;
static Rec  g_recs[kRecs];
static volatile LONG g_wr = 0;

// ------------------------------------------------------------------ hook
// Measured arity: `(this /*rcx*/, float /*xmm1*/, float /*xmm2*/, float /*xmm3*/)` returning bool.
// The callee writes rbx/rsi into the rdx and r8 home slots, so those are NOT arguments, and the call
// site sets only xmm1-3. Per the house rule the floats are declared `double` and forwarded
// UNCONVERTED, so the compiler only copies registers and cannot insert a cvtss2sd; the value is
// recovered from the low 32 bits, which is where a float actually lives in the register.
typedef char (*JumpForTrickFn)(void*, double, double, double);
static void* g_orig = nullptr;
static void* g_start = nullptr;

static inline float argF(double d) {
    uint64_t u; memcpy(&u, &d, sizeof(u));
    uint32_t lo = (uint32_t)u;
    float f; memcpy(&f, &lo, sizeof(f));
    return f;
}

static char hkJumpForTrick(void* mc, double a2, double a3, double a4) {
    Rec* r = nullptr;
    if (g_ok && g_on && mc) {
        __try {
            const LONG i = InterlockedIncrement(&g_wr) - 1;
            r = &g_recs[((i % kRecs) + kRecs) % kRecs];
            InterlockedExchange(&r->ready, 0);
            r->argHeight    = argF(a2);
            r->argPopRatio  = argF(a3);
            r->argGrindRatio = argF(a4);

            const void* sk  = twkP(mc, MC_SKATER);
            const void* gdb = twkP(mc, MC_GRINDS_DB);
            const void* tdb = twkP(mc, MC_TRICKS_DB);
            const int   fl  = sk ? twkB(sk, SK_FLAGS) : -1;
            r->isPopPending    = (fl >= 0) ? ((fl >> 2) & 1) : -1;
            r->isExitingGrind  = (fl >= 0) ? ((fl >> 3) & 1) : -1;
            r->trickPopRatio   = sk ? twkF(sk, SK_TRICK_POP_RATIO)  : 0.0f;
            r->trickPopHeight  = sk ? twkF(sk, SK_TRICK_POP_HEIGHT) : 0.0f;
            r->advGrindPop     = sk ? twkF(sk, SK_ADV_GRIND_POP)    : 0.0f;
            r->trickDef        = sk ? twkP(sk, SK_CUR_FLIP_DEF)     : nullptr;
            r->grindDef        = sk ? twkP(sk, SK_CUR_GRIND_DEF)    : nullptr;
            r->maxCrankOnGrind = r->trickDef ? twkF(r->trickDef, TD_MAX_CRANK_ON_GRIND) : 0.0f;
            r->addPopGrind     = r->trickDef ? twkF(r->trickDef, TD_ADD_POP_GRIND)      : 0.0f;
            r->approach        = r->grindDef ? twkB(r->grindDef, GD_APPROACH)           : -1;
            r->grindAddPop     = r->grindDef ? twkF(r->grindDef, GD_ADD_POP)            : 0.0f;
            r->basePop     = gdb ? twkF(gdb, GDB_BASE_POP)      : 0.0f;
            r->exitPop     = gdb ? twkF(gdb, GDB_EXIT_POP)      : 0.0f;
            r->popDelay    = gdb ? twkF(gdb, GDB_POP_DELAY)     : 0.0f;
            r->momPopDelay = gdb ? twkF(gdb, GDB_MOM_POP_DELAY) : 0.0f;
            r->crankMin      = tdb ? twkF(tdb, TDB_CRANK_MIN)       : 0.0f;
            r->crankGrindMin = tdb ? twkF(tdb, TDB_CRANK_GRIND_MIN) : 0.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r = nullptr;
            if (InterlockedIncrement(&g_faults) == 1) g_ok = 0;   // ours alone
        }
    }

    char ret = 0;
    __try { ret = ((JumpForTrickFn)g_orig)(mc, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[gpop] caught fatal in JumpForTrick -> recovered");
        return 0;
    }

    // The delay is chosen INSIDE the original, so it can only be read afterwards. This is the number
    // the whole probe exists for.
    if (r) {
        __try {
            r->ret           = ret;
            r->outDelay      = twkF(mc, MC_JUMP_DELAY);
            r->outGrindRatio = twkF(mc, MC_GRIND_CRANK_RATIO);
            InterlockedExchange(&r->ready, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return ret;
}

// ------------------------------------------------------------------ PitchBoard
// The pitch state machine. Which MODE runs decides whether the player's board-control input is read
// at all: mode 2 (flat trick) sets _pitchTargetAngle = _pitchExtraAngle, and that is the only read of
// _pitchExtraAngle anywhere in the function. Mode 4 -- the grind EXIT -- builds its target purely
// from the grind definition's authored end-pitch curve lerped by TrickPopRatio, so there is no term
// for player input in it. That is why Board Control Mode has no effect coming out of a grind: not a
// value arriving wrong, but a branch with nowhere to put it.
//
// The fix gives mode 4 the term mode 2 has, added ON TOP of the authored curve so the grind's own
// shape survives. `this` comes from the game, so no base is inferred -- and it is checked against its
// own skater back-pointer before anything is written.
typedef void (*PitchBoardFn)(void*, uint8_t, double);
static void* g_origPitch = nullptr;
static void* g_startPitch = nullptr;

struct PitchRec {
    volatile LONG ready;
    int mode, state;
    float extra, target, added;
    // Filled only for the grind path (mode 4 / state 3): the authored entry actually in play.
    const void* grindDef;
    int   footPos, overrideEnd, overrideStart, fromAdditional;
    float timeMin, timeMax, angleMin, angleMax, popRatioMod, trickPopRatio;
    // the pop-swing restoration (mode 1)
    int   swingApplied;
    float swingFrom, swingTo, swingMin, swingMax, swingRatio;
};

// GetTrickGrindEndPitchData replicated as pure reads (the game's is a plain lookup: scan the
// per-foot-position array, fall back to the default entry). Reading beats calling -- a getter
// invoked from inside a hooked callstack is a risk taken for nothing when the logic is six lines.
static const void* ResolveEndPitch(const void* gd, int footPos, int* fromAdditional) {
    *fromAdditional = 0;
    if (!gd) return nullptr;
    if (footPos > 0) {
        const int n = twkI(gd, GD_END_ADDL_NUM);
        const uint8_t* data = (const uint8_t*)twkP(gd, GD_END_ADDL);
        if (data && n > 0 && n < 64) {
            for (int i = 0; i < n; i++) {
                const uint8_t* e = data + i * APD_STRIDE;
                if (twkB(e, APD_FOOT) == footPos) { *fromAdditional = 1; return e; }
            }
        }
    }
    return (const uint8_t*)gd + GD_END_PITCH;
}
static const int kPitchRecs = 8;
static PitchRec g_pitchRecs[kPitchRecs];
static volatile LONG g_pitchWr = 0;
static volatile LONG g_lastMode = -1;

static void hkPitchBoard(void* self, uint8_t mode, double a3) {
    __try { ((PitchBoardFn)g_origPitch)(self, mode, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[gpop] caught fatal in PitchBoard -> recovered");
        return;
    }
    if (!self || !g_ok) return;
    __try {
        const float extra = twkF(self, PB_EXTRA);
        const int   state = twkB(self, PB_STATE);
        float added = 0.0f;
        // The identity gate: the pitch sub-object's +0x208 is the skater. If that is not a pointer we
        // are not looking at what we think we are, and nothing is written.
        const bool identified = (twkP(self, PB_SKATER) != nullptr);
        // BOTH discriminators, or this fires on flat ground: mode 4 is the trick-END pitch generally,
        // and only state 3 is the grind. The grind def is checked too, exactly as the game's own
        // grind path does before it reads the authored curve.
        const bool onGrind = (state == PITCH_STATE_GRIND) && (twkP(self, PB_GRINDDEF) != nullptr);
        if (g_pitchFix && identified && mode == PITCH_MODE_END && onGrind && extra != 0.0f) {
            added = extra * ((float)g_pitchScale / 100.0f);
            const float cur = twkF(self, PB_TARGET);
            if (cur > -1.0e6f && cur < 1.0e6f)
                *(float*)((uint8_t*)self + PB_TARGET) = cur + added;
            else
                added = 0.0f;
        }

        // ---- the POP's tail-down swing (mode 1). Two different failures, one repair:
        //   * grinds with OverrideTrickStartPitch -- mode 1 returned before applying anything, so the
        //     target is still the grind's static pose and the tail never swings toward the rail;
        //   * grinds without it -- mode 1 ran, but TrickPopRatio is pinned at 0 on grind exits, so it
        //     took TargetAngleMin, the floor of the trick's own range.
        // Recomputing the swing and blending the target toward it covers both, because in each case
        // the fix is "the target should be further along the trick's authored swing than it is".
        float swingFrom = 0.0f, swingTo = 0.0f, swingMin = 0.0f, swingMax = 0.0f, swingRatio = 0.0f;
        int   swingApplied = 0;
        if (g_popSwing && identified && mode == PITCH_MODE_POP && onGrind &&
            AllowedFor(twkP(self, PB_GRINDDEF)) == 1) {
            const void* sk = twkP(self, PB_SKATER);
            const void* td = sk ? twkP(sk, SK_CUR_FLIP_DEF) : nullptr;
            if (td) {
                swingMin = twkF(td, TD_START_PITCH + APD_AMIN);
                swingMax = twkF(td, TD_START_PITCH + APD_AMAX);
                // TrickPopRatio is the game's own scaler here, but it is a constant 0 on grind exits.
                // The grind crank duration ratio is the input-driven quantity that DOES survive the
                // grind path, so it drives the swing; TrickPopRatio is only the fallback.
                const void* mc = twkP(sk, SK_MOVEMENT_COMP);
                float r = mc ? twkF(mc, MC_GRIND_CRANK_RATIO) : -1.0f;
                if (!(r >= 0.0f && r <= 1.0f)) r = twkF(sk, SK_TRICK_POP_RATIO);
                if (!(r >= 0.0f && r <= 1.0f)) r = 0.0f;
                swingRatio = r;
                const float full = swingMin + (swingMax - swingMin) * r;
                const float cur  = twkF(self, PB_TARGET);
                if (cur > -1.0e6f && cur < 1.0e6f && full > -1.0e6f && full < 1.0e6f) {
                    swingFrom = cur;
                    swingTo   = cur + (full - cur) * ((float)g_popSwingBlend / 100.0f);
                    *(float*)((uint8_t*)self + PB_TARGET) = swingTo;
                    swingApplied = 1;
                }
            }
        }
        // Edge-triggered only: this runs per frame, so a line per call would drown the log and
        // inflate the very frame timing the rest of the mod measures. Keyed on mode AND state,
        // because within one mode the state is what separates a grind from flat ground.
        const LONG key = ((LONG)mode << 8) | (LONG)(state & 0xff);
        if (InterlockedExchange(&g_lastMode, key) != key) {
            const LONG i = InterlockedIncrement(&g_pitchWr) - 1;
            PitchRec* p = &g_pitchRecs[((i % kPitchRecs) + kPitchRecs) % kPitchRecs];
            InterlockedExchange(&p->ready, 0);
            p->mode = mode; p->state = state;
            p->extra = extra; p->target = twkF(self, PB_TARGET); p->added = added;
            p->grindDef = nullptr; p->fromAdditional = 0;
            p->swingApplied = swingApplied; p->swingFrom = swingFrom; p->swingTo = swingTo;
            p->swingMin = swingMin; p->swingMax = swingMax; p->swingRatio = swingRatio;
            if (onGrind) {
                const void* gd = twkP(self, PB_GRINDDEF);
                const int   fp = twkB(self, PB_FOOTPOS);
                const void* pd = ResolveEndPitch(gd, fp, &p->fromAdditional);
                const void* sk = twkP(self, PB_SKATER);
                p->grindDef    = gd;
                p->footPos     = fp;
                p->overrideEnd   = gd ? twkB(gd, GD_OVERRIDE_END)   : -1;
                p->overrideStart = gd ? twkB(gd, GD_OVERRIDE_START) : -1;
                p->popRatioMod = gd ? twkF(gd, GD_POPRATIO_MOD) : 0.0f;
                p->trickPopRatio = sk ? twkF(sk, SK_TRICK_POP_RATIO) : 0.0f;
                p->timeMin  = pd ? twkF(pd, APD_TMIN) : 0.0f;
                p->timeMax  = pd ? twkF(pd, APD_TMAX) : 0.0f;
                p->angleMin = pd ? twkF(pd, APD_AMIN) : 0.0f;
                p->angleMax = pd ? twkF(pd, APD_AMAX) : 0.0f;
            }
            InterlockedExchange(&p->ready, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[gpop] caught fatal in the pitch pass -> probe off");
        g_ok = 0;
    }
}

// ------------------------------------------------------------------ pump: resolve names, write lines
void GrindPop_PumpFrame() {
    if (!g_start || !g_ok) return;
    for (int i = 0; i < kPitchRecs; i++) {
        PitchRec* p = &g_pitchRecs[i];
        if (InterlockedCompareExchange(&p->ready, 0, 1) != 1) continue;
        const bool grind = (p->state == PITCH_STATE_GRIND);
        const char* what = (p->mode == PITCH_MODE_TRICK) ? "target = YOUR extra pitch, but skipped while a grind def is live"
                         : (p->mode == PITCH_MODE_START) ? "trick START pitch, authored curve"
                         : (p->mode == PITCH_MODE_END)   ? "trick END pitch, authored curve"
                                                         : "other";
        TwkLog("[gpop] pitch mode %d state %d (%s%s)  extra=%.2f target=%.2f",
               p->mode, p->state, what, grind ? " -- ON A GRIND" : "", p->extra, p->target);
        if (p->added != 0.0f)
            TwkLog("[gpop]    added %.2f deg of your input to the grind-exit target", p->added);
        if (p->swingApplied)
            TwkLog("[gpop]    POP SWING restored: %.2f -> %.2f deg  (trick swing %.2f..%.2f at ratio %.3f)",
                   p->swingFrom, p->swingTo, p->swingMin, p->swingMax, p->swingRatio);
        // The authored entry actually in play, and where the ratio landed inside it. If the ratio is
        // 0 the Max columns are unreachable and the board takes the smallest dip in the shortest
        // time -- which is the "it pops before the tail gets near the rail" complaint.
        if (grind && p->grindDef) {
            char gname[64];
            if (!NameOfFName((const uint8_t*)p->grindDef + GD_NAME, gname, sizeof(gname)))
                strcpy(gname, "?");
            RememberGrind(p->grindDef, gname);
            const float r = p->trickPopRatio;
            TwkLog("[gpop]    authored end pitch for '%s' (foot %d, %s entry): angle %.2f..%.2f  time %.3f..%.3f",
                   gname, p->footPos, p->fromAdditional ? "per-foot" : "default",
                   p->angleMin, p->angleMax, p->timeMin, p->timeMax);
            TwkLog("[gpop]    TrickPopRatio=%.3f%s -> angle %.2f, time %.3f  | OverrideTrickEndPitch=%d "
                   "AnimTrickPopRatioModifier=%.3f",
                   r, (r <= 0.0001f) ? "  <== PINNED AT MIN" : "",
                   p->angleMin + (p->angleMax - p->angleMin) * r,
                   p->timeMin  + (p->timeMax  - p->timeMin)  * r,
                   p->overrideEnd, p->popRatioMod);
            // OverrideTrickStartPitch is the one that skips the pop swing -- read, no longer inferred.
            TwkLog("[gpop]    OverrideTrickStartPitch=%d%s", p->overrideStart,
                   p->overrideStart == 1 ? "  (the game skips the pop swing on this grind)" : "");
        }
    }
    for (int i = 0; i < kRecs; i++) {
        Rec* r = &g_recs[i];
        if (InterlockedCompareExchange(&r->ready, 0, 1) != 1) continue;
        __try {
            char trick[64], grind[64];
            if (!NameOfObject(r->trickDef, trick, sizeof(trick))) strcpy(trick, r->trickDef ? "?" : "none");
            if (!r->grindDef || !NameOfFName((const uint8_t*)r->grindDef + GD_NAME, grind, sizeof(grind)))
                strcpy(grind, r->grindDef ? "?" : "none");
            // Decide the swing allowance here, where naming a grind is safe, so the hook only reads.
            RememberGrind(r->grindDef, grind);

            const bool onGrind = (r->grindDef != nullptr);
            if (!onGrind && !g_flat) continue;

            if (onGrind) {
                // What the ratio WOULD have to be for the delay to land on the real pop, and what it
                // actually is. The span can legitimately be zero (both endpoints authored equal), so
                // the position is only reported when it means something.
                const float span = r->popDelay - r->momPopDelay;
                char where[96];
                if (fabsf(span) > 1e-6f) {
                    const float pos = (r->outDelay - r->momPopDelay) / span;
                    snprintf(where, sizeof(where), "%.0f%% of the way from MOMENTUM pop to REAL pop", pos * 100.0f);
                } else {
                    snprintf(where, sizeof(where), "endpoints are equal (%.3f) -- the ratio cannot matter here", r->popDelay);
                }
                const bool capped = (r->maxCrankOnGrind > 0.0f) &&
                                    (r->outGrindRatio >= r->maxCrankOnGrind - 1e-4f);
                TwkLog("[gpop] GRIND EXIT  trick='%s'  grind='%s' approach=%d  exitingGrind=%d",
                       trick, grind, r->approach, r->isExitingGrind);
                TwkLog("[gpop]    ratio=%.3f  cap MaxCrankRatioOnGrinds=%.3f%s  |  crankMin flat=%.3f grind=%.3f",
                       r->outGrindRatio, r->maxCrankOnGrind, capped ? "  <== AT THE CAP" : "",
                       r->crankMin, r->crankGrindMin);
                TwkLog("[gpop]    delay=%.4fs  (momentum=%.4f  pop=%.4f)  %s",
                       r->outDelay, r->momPopDelay, r->popDelay, where);
                TwkLog("[gpop]    popRatio arg=%.3f%s  TrickPopRatio=%.3f  popHeight=%.1f  "
                       "(basePop=%.1f exitPop=%.1f addGrind=%.1f grindAdd=%.1f adv=%.2f)",
                       r->argPopRatio,
                       (r->isExitingGrind == 1 && r->argPopRatio == 0.0f) ? " (zeroed, as expected)" : "",
                       r->trickPopRatio, r->argHeight,
                       r->basePop, r->exitPop, r->addPopGrind, r->grindAddPop, r->advGrindPop);
                g_uiRatio = r->outGrindRatio; g_uiCap = r->maxCrankOnGrind;
                g_uiDelay = r->outDelay; g_uiMom = r->momPopDelay; g_uiPop = r->popDelay;
                strncpy_s(g_uiGrind, sizeof(g_uiGrind), grind, _TRUNCATE);
                InterlockedIncrement(&g_uiCount);
            } else {
                // The baseline. A flat pop should show delay 0 (the jump values are set immediately)
                // and a live popRatio -- the two things a grind exit does not get.
                TwkLog("[gpop] flat/other  trick='%s'  popRatio=%.3f  TrickPopRatio=%.3f  popHeight=%.1f  "
                       "delay=%.4fs  grindRatio=%.3f",
                       trick, r->argPopRatio, r->trickPopRatio, r->argHeight, r->outDelay, r->outGrindRatio);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedIncrement(&g_faults) == 1) TwkLog("[gpop] caught fatal formatting a record -> probe off");
            g_ok = 0;
            return;
        }
    }
}

// ------------------------------------------------------------------ install + menu
void GrindPop_Install() {
    g_start = TwkScanExe(SIG_JUMP_FOR_TRICK);
    if (!g_start) { TwkLog("[gpop] JumpForTrick sig NOT FOUND -- grind pop probe off (game updated?)"); return; }
    if (MH_CreateHook(g_start, (void*)&hkJumpForTrick, &g_orig) != MH_OK ||
        MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[gpop] hook failed on JumpForTrick -- grind pop probe off");
        g_start = nullptr; return;
    }
    // Independent of the pop probe: losing one must not cost the other.
    g_startPitch = TwkScanExe(SIG_PITCH_BOARD);
    if (!g_startPitch) TwkLog("[gpop] PitchBoard sig NOT FOUND -- pitch mode logging and fix off");
    else if (MH_CreateHook(g_startPitch, (void*)&hkPitchBoard, &g_origPitch) != MH_OK ||
             MH_EnableHook(g_startPitch) != MH_OK) {
        TwkLog("[gpop] hook failed on PitchBoard -- pitch mode logging and fix off");
        g_startPitch = nullptr;
    }
    g_fnameToStr = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR);
    if (!g_fnameToStr) TwkLog("[gpop] FName::ToString not found -- tricks and grinds will be unnamed");
    TwkLog("[gpop] installed (pop probe @ %p %s, PitchBoard @ %p, grind pitch fix %s)",
           g_start, g_on ? "ON" : "off", g_startPitch, g_pitchFix ? "ON" : "off");
}

bool GrindPop_Enabled() { return g_on != 0; }
void GrindPop_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
bool  GrindPop_PitchEnabled() { return g_pitchFix != 0; }
void  GrindPop_SetPitchEnabled(bool on) { g_pitchFix = on ? 1 : 0; TwkMarkDirty(); }
float GrindPop_PitchScale() { return (float)g_pitchScale; }
void  GrindPop_SetPitchScale(float pct) { g_pitchScale = (int)pct; TwkMarkDirty(); }
bool  GrindPop_SwingEnabled() { return g_popSwing != 0; }
void  GrindPop_SetSwingEnabled(bool on) { g_popSwing = on ? 1 : 0; TwkMarkDirty(); }
float GrindPop_SwingBlend() { return (float)g_popSwingBlend; }
void  GrindPop_SetSwingBlend(float pct) { g_popSwingBlend = (int)pct; TwkMarkDirty(); }
void GrindPop_ResetDefaults() {
    g_on = 1; g_flat = 1; g_pitchFix = 0; g_pitchScale = 100;
    g_popSwing = 0; g_popSwingBlend = 100;
    TwkMarkDirty();
}

void GrindPop_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    bool on = g_on != 0;
    if (api->Checkbox("Grind pop probe (log only)", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(writes SessionTweaks.log; changes nothing)");
    if (on) {
        api->Indent();
        bool fl = g_flat != 0;
        if (api->Checkbox("Also log flat-ground tricks", &fl)) { g_flat = fl ? 1 : 0; TwkMarkDirty(); }
        api->Unindent();
    }
    bool pf = g_pitchFix != 0;
    if (api->Checkbox("Pitch control out of grinds", &pf)) { g_pitchFix = pf ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(needs Board Control Mode = manual)");
    if (pf) {
        api->Indent();
        float sc = (float)g_pitchScale;
        if (api->SliderFloat("Amount (%)", &sc, 0.0f, 200.0f, "%.0f")) { g_pitchScale = (int)sc; TwkMarkDirty(); }
        api->Unindent();
    }
    bool ps = g_popSwing != 0;
    if (api->Checkbox("Pop swing out of grinds", &ps)) { g_popSwing = ps ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the board drops its tail before the pop)");
    if (ps) {
        api->Indent();
        float bl = (float)g_popSwingBlend;
        if (api->SliderFloat("Swing amount (%)", &bl, 0.0f, 100.0f, "%.0f")) { g_popSwingBlend = (int)bl; TwkMarkDirty(); }
        char l[224];
        snprintf(l, sizeof(l), "Grinds: %s", g_swingGrinds);
        api->TextDisabled(l);
        api->TextDisabled("(edit GrindPopSwingGrinds in SessionTweaks.ini; matched as substrings)");
        api->Unindent();
    }
    if ((int)g_uiCount == 0) { api->TextDisabled("last grind exit: none yet"); return; }
    snprintf(b, sizeof(b), "last grind exit #%d on '%s'", (int)g_uiCount, g_uiGrind);
    api->Text(b);
    snprintf(b, sizeof(b), "   ratio %.3f (cap %.3f)   delay %.4fs", (float)g_uiRatio, (float)g_uiCap, (float)g_uiDelay);
    api->Text(b);
    snprintf(b, sizeof(b), "   momentum pop %.4f  ..  real pop %.4f", (float)g_uiMom, (float)g_uiPop);
    api->Text(b);
}
