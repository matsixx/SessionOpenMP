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
// SessionTweaks -- FOOT PLACEMENT. The shoe hovers above the griptape. Started after a Session
// update; every shoe is affected, some worse than others.
//
// THE MECHANISM (PDB + disassembly):
//     USkaterAnimInstance::UpdateFootAnchors   Epic 0xf6c370, SkaterAnimInstance.cpp:1455
//       reads deck sockets "SKXX_SkateSkel_Foot_Anchor_L"/"_R" (GetSocketTransform, RTS_Component)
//       -> USkaterAnimInstance::AutoAdjustFoot Epic 0xf4f620, SkaterAnimInstance.cpp:1737
//            sweeps the foot-bone collision box down (channel 23), clamps by FootIKAutoAdjustMaxExtent
//
// ---- WHAT FIELD ROUND 1 SETTLED (measured; these are closed, do not re-chase) ----------------------
// ⛔ The auto-adjust CLAMP is not the problem. MaxExtent X/Y/Z read 20 / 40 / 30 cm and the correction
//    used 0-6% of Z (L autoOffset was flat 0.00, R 0.00-1.77). A 30 cm allowance nothing is touching
//    cannot be what stops the foot. The clamp override shipped in 2.17.0 was therefore correctly
//    useless and has been REMOVED rather than left lying around.
// ⛔ IK alpha is not the problem either: 1.000 on both feet in every sample.
// ✅ The Debug* block IS live -- round 1 called it dead through a bug of mine. I validated it by
//    matching against `LeftFootSocketLocation` (+0x404) believing that field was world space
//    (game_syms.h says the MP mod transports it raw-world). It is COMPONENT-LOCAL here: it read
//    ~(10, -10, 21) while the world socket is ~(-17792, -7496, 128). Two different spaces, so nothing
//    matched. ⚠️ Never validate one field against another without proving they share a space.
// ✅ THE ACTUAL FINDING: the foot sits ~1.3 cm ABOVE the deck anchor, consistently, on both feet --
//    L 128.18 - 126.87 = 1.31, R 129.78 - 128.50 = 1.28. That is the hover.
// ✅ The sweep barely participates: it is only 10 cm long, and the LEFT foot gets no hit at all
//    (hit == sweep end), the right grazes 0.57 cm. So the gap is upstream of AutoAdjustFoot.
//
// The debug-block mapping is confirmed, and by a cross-check rather than by declaration order: the
// right foot's hit sits 0.57 cm above its sweep end while `_lastRightFootAutoOffset.Z` reads 0.56.
// An independent field agreeing to a hundredth of a centimetre is what proves 0/1/2 = Start/End/Hit
// and that the L/R split is at index 6.
//
// ---- WHAT THIS ROUND DOES --------------------------------------------------------------------------
// Measurement, corrected: everything now comes from the debug block so every number shares ONE space,
// the settle gate keys on the socket position (round 1 gated on autoOffset, which is pinned near zero
// and therefore always looked settled -- samples fired while the skater was still moving), and the
// headline is `socket.Z - anchor.Z` per foot, which is the hover itself rather than a proxy for it.
//
// ---- WHY THE FIRST FIX ATTEMPT DID NOTHING (field round 2; the lesson, not just the outcome) ------
// It wrote `Left/RightFootBoneLocationAdjust` (+0x434/+0x440) from the frame pump. Full slider travel
// -- 5 cm, unmissable -- moved the feet by exactly nothing.
// ⛔ THE REASON IS TIMING, NOT THE FIELD. The pump is driven from `InputHandler::Tick`, which runs
// BEFORE the animation update; `USkaterAnimInstance::NativeUpdateAnimation` then recomputes the foot
// from scratch and our value is gone before anything renders. This project already learned this once
// -- the MP mod's pose lane applies its blob from NativeUpdateAnimation's POST hook precisely because
// "writing it anywhere earlier in the frame loses to the game's own per-frame recomputation".
// ⇒ RULE: anything that must survive into the rendered pose has to be written INSIDE the animation
// update, not from the input tick. No amount of magnitude fixes a phase error.
//
// ---- THE SEAM THIS USES ----------------------------------------------------------------------------
// A POST hook on `UpdateFootAnchors` itself: it is the function that computes the foot anchors, it
// runs inside NativeUpdateAnimation (call site 0xf5f1b9), and NOTHING else hooks it (exactly one
// xref). Right after it returns, the socket/anchor/target values are fresh and nothing has consumed
// them yet, so a correction applied there is the last word.
// ⛔ ARITY read at that call site, not assumed: `lea r9,[rbp-0x70] · movaps xmm1,xmm11 · lea
// r8,[rbp-0x60] · mov rcx,rdi` => (this, FLOAT dt, void*, void*). The float is declared `double` and
// forwarded UNCONVERTED, per the standing thunk rule -- an under-declared thunk hands the original
// garbage and fails silently.
//
// The correction is AUTOMATIC and therefore per-shoe by construction: the hover is re-measured every
// frame as socket.Z - anchor.Z from the debug block, and that much is subtracted from the foot socket
// the graph is about to use. No per-shoe table, and a shoe with a different sole self-corrects.
//
// ⚠️ Do not re-scan a function another module hooks -- MinHook overwrites the prologue and a later
// scan silently fails (field-caught this session). catch_level owns GetBoardExtraPitchAngle,
// scoop_speed owns InputHandler::Tick, grind_pop owns JumpForTrick/PitchBoard, run_out owns Bail,
// and OpenMP owns NativeUpdateAnimation when a co-op session arms -- which is the other reason to
// hook UpdateFootAnchors rather than its caller.
// 🛑 `_left/_rightFootBodyInstance` (+0x770/+0x778) are read for their POINTER VALUE ONLY, as a
// costume-change signal, and never dereferenced: `FBodyInstance+0x8` is a TWeakObjectPtr (two int32s,
// not an address), so a raw walk returns plausible floats that are lies rather than faulting.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include <climits>
#include "ui/menu_ext.h"
#include "foot_place.h"
#include "catch_tweaks.h"     // CatchTweaks_Skater() -- the live skater, without a hook of our own
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets
enum {
    // ---- corroborated by a second source in this repo (src/game/game_syms.h, anim_fields.h)
    SK_MESH        = 0x280,   MESH_ANIM   = 0x6b0,
    SK_BOARD       = 0x568,   BOARD_FLIPPER = 0x4e8,
    COMP_QUAT      = 0x1c0,   COMP_POS    = 0x1d0,
    AN_ON_BOARD    = 0x300,   AN_TRICK_PENDING = 0x310, AN_CATCH_ST = 0x312,
    AN_IS_SWITCH   = 0x303,   // USkaterAnimInstance::IsSkatingSwitch (PDB-named)
    AN_L_ALPHA     = 0x3fc,   AN_R_ALPHA  = 0x400,
    // FVector foot socket the graph consumes. ⚠️ SPACE VARIES: it reads component-local here (~20 cm)
    // while the MP mod sees world on its senders -- which is why that mod ships a space DETECTOR.
    // We only ever apply a DELTA to it, so the space never has to be decided.
    AN_L_SOCK_LOC  = 0x404,   AN_R_SOCK_LOC = 0x41c,    // <-- the correction seam
    AN_L_BONE_ADJ  = 0x434,   AN_R_BONE_ADJ = 0x440,    // authored bone delta -- DEAD END, see header
    AN_FLIPPING    = 0x495,   AN_ROTATING = 0x496,      AN_GROUNDED = 0x5fa,
    // THE TRICK-SETUP CROUCH IS THE "CRANK". IsTrickPending (which the first cut used) is a different
    // thing entirely and never matched the crouch. CrankPocketRatio is the crouch DEPTH, so the setup
    // offset can be BLENDED in as you crouch rather than snapping on.
    // ⚠️ Both are ASSET-GATED: they are only meaningful while CrankLoopBlendSpace is non-null. That
    // gate is not decoration -- the MP mod carries the same table because the fields read stale
    // otherwise.
    AN_IS_CRANKING = 0x497,   AN_CRANK_POCKET = 0x498,  AN_CRANK_BS = 0x4a8,
    // ⛔ Round 5: the crank pocket read 0.00 through every REAL trick setup -- the crank is the
    // rotation WIND-UP crouch, a different mechanic. The setup crouch the user means (pulling the
    // stick down to charge the pop) is FlipTrickPopRatio, "the ollie crouch/charge" in the MP mod's
    // own field table. Gated on the FlipTrick asset pointer, same table.
    AN_POP_RATIO   = 0x4d0,   AN_FLIP_TRICK = 0x4c8,
    // ---- PDB-only, but now confirmed live by round 1
    AN_AUTOADJUST  = 0x3e8,   AN_SMOOTHING = 0x3ec,
    AN_MAX_EXT_Z   = 0x3f8,
    AN_L_AUTO_OFF  = 0x718,   AN_R_AUTO_OFF = 0x724,
    AN_L_BODY      = 0x770,   AN_R_BODY     = 0x778,    // POINTER VALUE ONLY -- never dereferenced
    // ---- the debug block: 12 x FReplayDataRaw (LocationX/Y/Z at +0/+4/+8, then a packed quat).
    // Mapping cross-checked against _lastRightFootAutoOffset -- see the header.
    AN_DEBUG_BASE  = 0x7ac,   DEBUG_STRIDE = 20,        DEBUG_COUNT = 12,
    DBG_L_START = 0, DBG_L_END = 1, DBG_L_HIT = 2, DBG_L_SOCKET = 3, DBG_L_ANCHOR = 4, DBG_L_TARGET = 5,
    DBG_R_START = 6, DBG_R_END = 7, DBG_R_HIT = 8, DBG_R_SOCKET = 9, DBG_R_ANCHOR =10, DBG_R_TARGET =11,
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int g_on          = 1;    // FootPlaceProbe    -- the measurement half
static int g_autoSample  = 1;    // FootPlaceAutoSample
static int g_settleTicks = 12;   // FootPlaceSettleTicks
static int g_minDeltaMm  = 25;   // FootPlaceMinDeltaMm -- re-sample threshold on the hover
static int g_maxBlocks   = 60;   // FootPlaceMaxBlocks
// ---- THE FIX. Applied from the UpdateFootAnchors POST hook, never from the pump.
// Four nudges in millimetres: per FOOT, and per STATE. Positive lowers the foot.
// ⛔ The auto "sit the foot down" percentage that shipped in 2.18.0 is GONE. It closed the measured
// socket-to-anchor gap automatically and was per-shoe by construction, but it did not LOOK right in
// play -- the anchor is where the BONE goes and the sole hangs below it, so matching them exactly is
// not the same as the shoe sitting on the griptape. A number dialled by eye beats a derived number
// that is geometrically correct and visually wrong.
// ⚠️ LEFT/RIGHT, not front/back, and deliberately: the hover belongs to the SHOE, so it must follow
// the foot rather than which end of the board that foot happens to be on. These stay correct when
// you ride switch, which a front/back pairing would not.
static int g_nudgeL      = 0;    // FootNudgeLMm       -- riding
static int g_nudgeR      = 0;    // FootNudgeRMm
static int g_nudgeSetupL = 0;    // FootNudgeSetupLMm  -- while a trick is being set up
static int g_nudgeSetupR = 0;    // FootNudgeSetupRMm
// The same four again for SWITCH. Not redundant with the left/right split: the sliders follow the
// FOOT (so a shoe's own error stays with that shoe), but the deck pose does not -- your front foot
// becomes your back foot when you ride switch, and the animation seats front and back differently.
static int g_nudgeSwL      = 0;  // FootNudgeSwLMm
static int g_nudgeSwR      = 0;  // FootNudgeSwRMm
static int g_nudgeSwSetupL = 0;  // FootNudgeSwSetupLMm
static int g_nudgeSwSetupR = 0;  // FootNudgeSwSetupRMm
// Index order everywhere (profiles, ini, the blend): regular L,R,setupL,setupR then switch L,R,sL,sR.
static int* const kNudges[8] = { &g_nudgeL, &g_nudgeR, &g_nudgeSetupL, &g_nudgeSetupR,
                                 &g_nudgeSwL, &g_nudgeSwR, &g_nudgeSwSetupL, &g_nudgeSwSetupR };
static const char* const kNudgeKeys[8] = {
    "FootNudgeLMm", "FootNudgeRMm", "FootNudgeSetupLMm", "FootNudgeSetupRMm",
    "FootNudgeSwLMm", "FootNudgeSwRMm", "FootNudgeSwSetupLMm", "FootNudgeSwSetupRMm" };
// ⛔ DECK-PLANE ABSOLUTE PLACEMENT (2.19.x) IS FIELD-REJECTED -- REMOVED, do not rebuild it.
// Holding the foot at an absolute height above the deck plane made the feet "move a bit randomly"
// and clip through the board; the constant nudge "locks the feet and makes them stable". The lesson
// is general: the animation's own micro-motion IS the desired motion -- REPLACE the height and you
// fight the animation every frame; OFFSET it and you preserve it. (The same modify-don't-simulate
// verdict as the VR weapon-weight work.)
//
// THE REMAINING PROBLEM WAS ONLY PER-SHOE VALUES, and that is solved with DATA, not a new placement
// model: each costume ships its own physics asset, and the foot's collision BOX in it encodes the
// sole geometry -- box bottom = Center.Z - halfZ - RestOffset, bone-local. The mod reads the worn
// shoe's box via the game's OWN weak-pointer resolver (FWeakObjectPtr::Get -- CALLED, never a raw
// deref of FBodyInstance+0x8, which is a TWeakObjectPtr and poison to read raw) and shifts the
// user's tuned nudge by the DIFFERENCE in box bottom between the current shoe and the shoe the
// numbers were tuned on. Tune once by eye; other shoes follow automatically.
// ---- per-shoe PROFILES ------------------------------------------------------------------------
// Each pair of shoes remembers its own four nudges. Tune by eye once per shoe; swapping shoes swaps
// the numbers. This REPLACED a measured auto-compensation that could not work: the foot box the
// anim instance carries comes from the character's FULL-BODY physics asset (logged as
// 'AMXX_SkelMesh_Full_PhysicsAsset_V2', box bottom exactly -10.00 cm on BOTH feet), which is
// identical for every costume -- there is no per-shoe number in physics data to read, because the
// difference is the shoe MESH's visual sole thickness. So we remember the eye-tuned value instead.
static int  g_shoeProfiles = 1;   // FootShoeProfiles: 0 = one global set of sliders, as before
static int  g_shoeCatOverride = -1;  // FootShoeCategory: pin the wardrobe category if detection misses
struct ShoeProfile { char name[48]; int n[8]; };   // see kNudges for the index order
// ⚠️ 128, not 24. The first cut used 24 -- the base game alone has 40+ shoes, and once the table
// filled, StoreCurrentProfile RETURNED SILENTLY: 18 of the author's tuned shoes were discarded
// without a word, and every new shoe after that inherited the last one's numbers and looked "saved".
// A table that can fill must say so (see the overflow log below); this one is sized past the problem.
static const int kMaxProfiles = 128;
static ShoeProfile g_profiles[kMaxProfiles];
// ---- shipped per-shoe defaults ------------------------------------------------------------------
// Tuned by eye in the headset, shoe by shoe, by the mod author (2026-08-08). These are DEFAULTS, not
// a lock: they seed the table before the ini is read, so anything you adjust yourself overrides them
// and only your differences are written back. Modded shoes are unknown here by definition and fall
// back to kFallbackNudges, which is the value the majority of stock shoes wanted.
struct ShoeDefault { const char* name; int n[8]; };
static const ShoeDefault kShoeDefaults[] = {
    { "CIT_GEN_FT_Adidas_MatchBreakSuper",      { 0, 5, 0, 15, 15, 5, 2, 15 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A",            { 0, 5, 0, 15, 10, 5, 2, 12 } },
    { "CIT_GEN_FT_Fallen_Bomber",               { 0, 0, 0, 10, 10, 5, 2, 12 } },
    { "CIT_GEN_FT_Fallen_Bomber_RWTF",          { 0, 0, 0, 10,  5, 5, 0,  5 } },
    { "CIT_GEN_FT_MidTopCup",                   { 0, 5, 0, 15,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A_01",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A_02",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A_03",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A_04",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_A_05",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_01",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_02",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_03",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_04",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_05",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_AccelOG_06",  { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_Explorer_01", { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_Sal23_01",    { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_Sal23_02",    { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_Sal23_03",    { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_eSFootwear_Sal23_04",    { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Globe_Mahalo01",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_B_01",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
    { "CIT_AMXX_GEN_FT_Shoes_Vul_B_02",         { 0, 5, 0, 12,  5, 5, 0,  5 } },
};
static const int kNumShoeDefaults = (int)(sizeof(kShoeDefaults) / sizeof(kShoeDefaults[0]));
// What an UNKNOWN shoe starts at -- a modded shoe, or a stock one not in the table. This is the value
// most stock shoes settled on, so a new shoe starts close instead of at a flat zero.
static const int kFallbackNudges[8] = { 0, 5, 0, 12, 5, 5, 0, 5 };
static const int* DefaultsFor(const char* name) {
    for (int i = 0; i < kNumShoeDefaults; i++)
        if (_stricmp(kShoeDefaults[i].name, name) == 0) return kShoeDefaults[i].n;
    return nullptr;
}
static int  g_nProfiles = 0;
static char g_curShoe[48] = { 0 };      // the shoe the live sliders currently belong to
// The baseline = the box bottoms of the shoe the sliders were TUNED ON (0.1 mm units; 30000 = unset).
// Re-anchored automatically whenever a slider changes: whatever you are wearing while tuning IS the
// reference. Persisted so the compensation survives restarts.
static int g_ok = 1;             // runtime health, NEVER persisted (the catch_level lesson)
static volatile LONG g_pumpTicks = 0, g_sampleReq = 0, g_blocks = 0, g_faults = 0;
static volatile float g_uiHovL = -999.0f, g_uiHovR = -999.0f;
static char g_uiVerdict[112] = "no sample yet";

void FootPlace_ReadConfig(const char* buf) {
    g_on          = TwkIniInt(buf, "FootPlaceProbe", 1);
    g_autoSample  = TwkIniInt(buf, "FootPlaceAutoSample", 1);
    g_settleTicks = TwkIniInt(buf, "FootPlaceSettleTicks", 12);
    g_minDeltaMm  = TwkIniInt(buf, "FootPlaceMinDeltaMm", 25);
    g_maxBlocks   = TwkIniInt(buf, "FootPlaceMaxBlocks", 60);
    // Seeded from the old single-value key so an existing tuning is not silently lost.
    const int legacy = TwkIniInt(buf, "FootPlaceLowerMm", 0);
    // Three sources, in priority order: an explicit key, then (for switch) the matching regular value
    // so an upgrade keeps behaving identically, then the shipped fallback so a FRESH install already
    // sits on the deck instead of at a flat zero.
    const bool hadRegular = TwkIniInt(buf, "FootNudgeRMm", INT_MIN) != INT_MIN;
    for (int i = 0; i < 4; i++)
        *kNudges[i] = TwkIniInt(buf, kNudgeKeys[i], legacy ? legacy : kFallbackNudges[i]);
    for (int i = 4; i < 8; i++)
        *kNudges[i] = TwkIniInt(buf, kNudgeKeys[i], hadRegular ? *kNudges[i - 4] : kFallbackNudges[i]);
    g_shoeProfiles    = TwkIniInt(buf, "FootShoeProfiles", 1) ? 1 : 0;
    g_shoeCatOverride = TwkIniInt(buf, "FootShoeCategory", -1);
    // Profiles are stored under INDEXED keys (FootShoe0..N = "name,l,r,sl,sr") rather than keys built
    // from the shoe name: an asset name is not guaranteed to be a legal ini key, and an index needs no
    // sanitising and no round-trip guarantee.
    // Seed the shipped tuning first; the ini is layered on top, so a user's own value always wins and
    // a default we improve later still reaches anyone who never touched that shoe.
    g_nProfiles = 0;
    for (; g_nProfiles < kNumShoeDefaults && g_nProfiles < kMaxProfiles; g_nProfiles++) {
        strncpy_s(g_profiles[g_nProfiles].name, sizeof(g_profiles[0].name),
                  kShoeDefaults[g_nProfiles].name, _TRUNCATE);
        for (int j = 0; j < 8; j++) g_profiles[g_nProfiles].n[j] = kShoeDefaults[g_nProfiles].n[j];
    }
    for (int i = 0; i < kMaxProfiles; i++) {
        char k[32], v[128];
        snprintf(k, sizeof(k), "FootShoe%d", i);
        TwkIniStr(buf, k, v, sizeof(v), "");
        if (!v[0]) continue;   // indices can have gaps: only differences are written back
        char* c = strchr(v, ',');
        if (!c) continue;
        *c = 0;
        int slot = -1;
        for (int j = 0; j < g_nProfiles; j++)
            if (_stricmp(g_profiles[j].name, v) == 0) { slot = j; break; }
        const bool fresh = (slot < 0);
        if (fresh) { if (g_nProfiles >= kMaxProfiles) continue; slot = g_nProfiles; }
        ShoeProfile& p = g_profiles[slot];
        strncpy_s(p.name, sizeof(p.name), v, _TRUNCATE);
        for (int j = 0; j < 8; j++) p.n[j] = 0;
        char* q = c + 1;
        int got = 0;
        for (; got < 8 && q; got++) {
            p.n[got] = atoi(q);
            char* nx = strchr(q, ',');
            q = nx ? nx + 1 : nullptr;
        }
        // A 4-value profile predates the switch sliders: mirror regular into switch so an already
        // tuned shoe looks identical until its switch values are set.
        if (got <= 4) for (int j = 0; j < 4; j++) p.n[4 + j] = p.n[j];
        if (fresh && p.name[0]) g_nProfiles++;
    }
    if (g_settleTicks < 2) g_settleTicks = 2;    if (g_settleTicks > 120) g_settleTicks = 120;
    int* all[4] = { &g_nudgeL, &g_nudgeR, &g_nudgeSetupL, &g_nudgeSetupR };
    for (int i = 0; i < 4; i++) { if (*all[i] < -50) *all[i] = -50; if (*all[i] > 50) *all[i] = 50; }
    TwkLog("[foot] config: FootPlaceProbe=%d AutoSample=%d | nudge riding L=%d R=%d mm, "
           "trick setup L=%d R=%d mm (%s)", g_on, g_autoSample, g_nudgeL, g_nudgeR,
           g_nudgeSetupL, g_nudgeSetupR,
           (g_nudgeL || g_nudgeR || g_nudgeSetupL || g_nudgeSetupR)
               ? "correcting the feet" : "nothing written to the game");
}
void FootPlace_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "FootPlaceProbe",      g_on);
    TwkIniSetInt(buf, cap, "FootPlaceAutoSample", g_autoSample);
    for (int i = 0; i < 8; i++) TwkIniSetInt(buf, cap, kNudgeKeys[i], *kNudges[i]);
    TwkIniSetInt(buf, cap, "FootShoeProfiles",    g_shoeProfiles);
    TwkIniSetInt(buf, cap, "FootShoeCategory",    g_shoeCatOverride);
    for (int i = 0; i < g_nProfiles; i++) {
        // EVERY profile is written, including ones still at their shipped value. Skipping those was
        // tried and is wrong: nothing ever DELETES an ini key, so a skipped write leaves the previous
        // (user) line in place and "Reset to defaults" would silently come back on the next load.
        char k[32], v[128];
        snprintf(k, sizeof(k), "FootShoe%d", i);
        snprintf(v, sizeof(v), "%s,%d,%d,%d,%d,%d,%d,%d,%d", g_profiles[i].name,
                 g_profiles[i].n[0], g_profiles[i].n[1], g_profiles[i].n[2], g_profiles[i].n[3],
                 g_profiles[i].n[4], g_profiles[i].n[5], g_profiles[i].n[6], g_profiles[i].n[7]);
        TwkIniSetStr(buf, cap, k, v);
    }
}
void FootPlace_ResetDefaults() {
    g_on = 1; g_autoSample = 1; g_settleTicks = 12; g_minDeltaMm = 25; g_maxBlocks = 60;
    for (int i = 0; i < 8; i++) *kNudges[i] = kFallbackNudges[i];
    g_shoeProfiles = 1; g_shoeCatOverride = -1; g_curShoe[0] = 0;
    g_nProfiles = 0;                                        // reset = back to the shipped tuning
    for (; g_nProfiles < kNumShoeDefaults && g_nProfiles < kMaxProfiles; g_nProfiles++) {
        strncpy_s(g_profiles[g_nProfiles].name, sizeof(g_profiles[0].name),
                  kShoeDefaults[g_nProfiles].name, _TRUNCATE);
        for (int j = 0; j < 8; j++) g_profiles[g_nProfiles].n[j] = kShoeDefaults[g_nProfiles].n[j];
    }
    g_ok = 1;
}
bool FootPlace_Enabled()           { return g_on != 0; }
void FootPlace_SetEnabled(bool on) { g_on = on ? 1 : 0; if (on) g_ok = 1; TwkMarkDirty(); }

// ------------------------------------------------------------------ helpers
struct V3 { float x, y, z; };
static V3 readV3(const void* base, int off) {
    V3 v; v.x = twkF(base, off); v.y = twkF(base, off + 4); v.z = twkF(base, off + 8); return v;
}
static bool plausible(const V3& v) { return fabsf(v.x) < 1e7f && fabsf(v.y) < 1e7f && fabsf(v.z) < 1e7f; }
static bool isZero(const V3& v) {   // "no data", never a coordinate
    return fabsf(v.x) < 1e-4f && fabsf(v.y) < 1e-4f && fabsf(v.z) < 1e-4f;
}
static V3 dbg(const void* anim, int idx) { return readV3(anim, AN_DEBUG_BASE + idx * DEBUG_STRIDE); }

// ------------------------------------------------------------------ state
static void*  g_skater = nullptr;
static void*  g_anim   = nullptr;
static void*  g_lastBody = nullptr;      // pointer VALUE only
static int    g_settled = 0, g_gateTicks = 0;
static int    g_settleMm = 30;        // 3 mm: 0.5 mm never fired at all (field round 2)
static float  g_jitter = 0.0f, g_lastJitter = 0.0f;
static bool   g_lastSettled = false;
static V3     g_prevSock = { 0, 0, 0 };
static double g_lastBlockT = -1000.0;
static float  g_lastHov = -9999.0f;
static bool   g_dbgChecked = false, g_dbgLive = false, g_dumped = false;
static bool   g_installed = false;
static void*  g_mesh = nullptr;      // cached by the pump for the hook
static void*  g_flipper = nullptr;

static double NowSec() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ------------------------------------------------------------------ which shoe am I wearing?
// AActor::GetGameInstance -- Epic 0x29c01e0 / Steam 0x2982ac0, verbatim from the OpenMP mod's
// game_syms.cpp. CALLED, never hooked (by us or by OpenMP), so scanning stays valid.
static const char* SIG_GET_GAME_INSTANCE =
    "48 83 EC 28 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 80 80 01 00 00 48 83 C4 28 C3";
static const char* SIG_FNAME_TOSTR2 =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00";
typedef void* (*GetGameInstFn)(void*);
typedef void  (*FNameToStrFn)(const void*, void*);
static GetGameInstFn g_getGameInst = nullptr;
static FNameToStrFn g_fnameToStr = nullptr;

// The worn wardrobe lives on the GAME INSTANCE, not the actor -- every skater rebuilds from the
// local player's loadout (the cosmetics-sync finding). Offsets below were read out of the game's own
// `TMapBase<int,FCustomizationProfileItem>::Emplace` by the OpenMP cosmetics work, and are only ever
// READ here: no key, no hash link, no allocation is touched, so a mistaken belief can only mis-read.
enum {
    OBJ_NAME    = 0x18,   // UObjectBase::NamePrivate
    GI_PROFILE  = 0x400,  // USessionGameInstance::_skaterInstance(+0x3f0)::CustomizationProfile(+0x10)
    PROF_ITEMS  = 0x08,   //   FCustomizationProfile::CharacterItems  TMap<int32 cat, ...>
    ELEM_STRIDE = 0x1c,   //   element: +0x00 int32 Key | +0x04 FCustomizationProfileItem(16) | ...
    ELEM_VALUE  = 0x04,   //   FCustomizationProfileItem::ItemName is an FName at the value's +0x00
};

// FName -> ascii, cached by FName VALUE (ToString leaks one game-side FString per distinct name --
// bounded by the cache; the cosmetics.cpp lesson).
static bool FNameStr(const void* fn, char* out, int cap) {
    out[0] = 0;
    if (!fn || !g_fnameToStr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fn; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;
    struct Entry { uint64_t key; char name[64]; };
    static Entry cache[64];
    static int nCached = 0;
    for (int i = 0; i < nCached; i++)
        if (cache[i].key == key) { strncpy_s(out, (size_t)cap, cache[i].name, _TRUNCATE); return out[0] != 0; }
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        g_fnameToStr(fn, &fs);
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

// ---- the TMap walker (read-only) -------------------------------------------------------------
struct MapWalk { uint8_t* data; int num; const uint32_t* bits; int numBits; };
static bool mapOpen(void* map, MapWalk& w) {
    w.data = nullptr; w.num = 0; w.bits = nullptr; w.numBits = 0;
    if (!map) return false;
    __try {
        w.data    = *(uint8_t**)((uint8_t*)map + 0x00);
        w.num     = *(int32_t*)((uint8_t*)map + 0x08);
        w.numBits = *(int32_t*)((uint8_t*)map + 0x28);
        void* sec = *(void**)((uint8_t*)map + 0x20);
        w.bits    = (const uint32_t*)(sec ? sec : ((uint8_t*)map + 0x10));
        if (w.num == 0) { w.numBits = 0; return true; }        // an EMPTY map is valid (null data)
        if (!w.data || w.num < 0 || w.num > 4096 || w.numBits < w.num) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ContainsCI(const char* hay, const char* needle) {
    const size_t n = strlen(needle);
    for (const char* p = hay; *p; p++) if (_strnicmp(p, needle, n) == 0) return true;
    return false;
}
// Wardrobe categories are bare int32 keys -- the PDB has no enum for them -- so this was MEASURED off
// a real loadout instead: they are BIT FLAGS, 1 head / 2 torso / 4 legs / 8 FEET / 8192 eyes /
// 16384 socks (dumped worn set, 2026-08-08: cat=8 'CIT_GEN_FT_Adidas_MatchBreakSuper', FT = feet).
// The category is the primary test because it cannot be fooled by naming style; the name patterns
// stay as a fallback in case a costume ever files shoes elsewhere. `FootShoeCategory` overrides both.
static const int kShoeCategory = 8;
static bool LooksLikeShoe(const char* n) {
    return ContainsCI(n, "shoe") || ContainsCI(n, "sneaker") || ContainsCI(n, "footwear") ||
           ContainsCI(n, "boot") || ContainsCI(n, "_fw_") || ContainsCI(n, "_ft_");
}
static bool g_dumpedWorn = false;
static char g_wornSummary[96] = { 0 };

// Returns the worn shoe's item name. Failure = we keep using whatever the sliders already say.
static bool ReadWornShoe(void* skater, char* out, int cap) {
    out[0] = 0;
    if (!g_getGameInst || !skater) return false;
    void* gi = nullptr;
    __try { gi = g_getGameInst(skater); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!gi) return false;
    MapWalk w;
    if (!mapOpen((uint8_t*)gi + GI_PROFILE + PROF_ITEMS, w)) return false;
    const bool dump = !g_dumpedWorn;
    if (dump) TwkLog("[foot] worn wardrobe (%d item(s)) -- the shoe is picked out of this list:", w.num);
    int seen = 0;
    __try {
        for (int i = 0; i < w.numBits && seen < w.num; i++) {
            if (!((w.bits[i >> 5] >> (i & 31)) & 1)) continue;
            seen++;
            uint8_t* e = w.data + (size_t)i * ELEM_STRIDE;
            const int cat = *(int32_t*)e;
            char nm[64];
            if (!FNameStr(e + ELEM_VALUE, nm, sizeof(nm))) continue;
            const bool isShoe = (g_shoeCatOverride >= 0) ? (cat == g_shoeCatOverride)
                                                         : (cat == kShoeCategory || LooksLikeShoe(nm));
            if (dump) TwkLog("[foot]     cat=%-6d '%s'%s", cat, nm, isShoe ? "   <-- shoe" : "");
            if (isShoe && !out[0]) strncpy_s(out, (size_t)cap, nm, _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_dumpedWorn = true; return false; }
    g_dumpedWorn = true;
    if (!out[0]) {
        snprintf(g_wornSummary, sizeof(g_wornSummary), "no shoe found among %d worn item(s)", seen);
        return false;
    }
    return true;
}

// ---- profile table -----------------------------------------------------------------------------
static ShoeProfile* FindProfile(const char* name) {
    for (int i = 0; i < g_nProfiles; i++)
        if (_stricmp(g_profiles[i].name, name) == 0) return &g_profiles[i];
    return nullptr;
}
// Save the live sliders onto whichever shoe they currently describe. Called on every slider move and
// before switching away, so the values you tuned are never the ones that get lost.
static void StoreCurrentProfile() {
    if (!g_curShoe[0]) return;
    ShoeProfile* p = FindProfile(g_curShoe);
    if (!p) {
        if (g_nProfiles >= kMaxProfiles) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                TwkLog("[foot] ⚠️ profile table FULL at %d shoes -- '%s' cannot be saved and its "
                       "adjustments WILL BE LOST. Raise kMaxProfiles.", kMaxProfiles, g_curShoe);
            }
            return;
        }
        p = &g_profiles[g_nProfiles++];
        strncpy_s(p->name, sizeof(p->name), g_curShoe, _TRUNCATE);
    }
    for (int i = 0; i < 8; i++) p->n[i] = *kNudges[i];
}

// ------------------------------------------------------------------ the correction hook
// USkaterAnimInstance::UpdateFootAnchors -- Epic 0xf6c370 / Steam 0xf2c180. POST only: the original
// always runs first and its result is then nudged. Arity from the call site (see the header):
// (this, FLOAT dt, void*, void*) -- the float is declared `double` and forwarded UNCONVERTED.
static const char* SIG_UPDATE_ANCHORS =
    "40 55 53 56 57 48 8D AC 24 F8 F6 FF FF 48 81 EC 08 0A 00 00 44 0F 29 84 24 C0 09 00 00 48 8B 05 "
    "?? ?? ?? ?? 48 33 C4 48 89 85 40 08 00 00";
typedef void (*UpdateAnchorsFn)(void*, double, void*, void*);
static void* g_origAnchors = nullptr, *g_startAnchors = nullptr;
static volatile LONG g_corrections = 0;
static volatile float g_uiAppliedL = 0.0f;
static bool g_wasSetup = false;
static volatile float g_uiCrank = 0.0f;
static double g_lastCorrLog = -1000.0;
static float g_crankBlend = 0.0f;   // our own ease, since the depth field is dead
static float g_switchBlend = 0.0f;  // 0 = regular, 1 = switch (eased for the same reason)
static volatile float g_uiSwitch = 0.0f;

static void hkUpdateFootAnchors(void* self, double dt, void* a, void* b) {
    __try { ((UpdateAnchorsFn)g_origAnchors)(self, dt, a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[foot] caught fatal in UpdateFootAnchors -> recovered");
        return;
    }
    if (!g_ok || !self) return;
    __try {
        // Blend riding -> setup by how deep the crouch is, so the offset eases in with the crank
        // instead of popping at a threshold. t=0 is exactly the riding value, t=1 exactly the setup
        // value. The asset gate is honoured: with no CrankLoopBlendSpace the crouch fields are stale.
        // ---- Field round 6 settled the signal: `IsCranking` (+0x497, asset-gated) went TRUE for
        // exactly the stretch the user held the setup stance, and the MP notes say it plainly --
        // "the CRANK = the trick-setup CROUCH". What is dead is the DEPTH: CrankPocketRatio read
        // -0.00 the whole time cranking was true, and FlipTrickPopRatio's asset gate is null outside
        // an actual trick (pop=-1.00 always). So: BINARY state, and we ease the blend ourselves --
        // the hook's own dt makes that trivial, and it looks the same as a real depth ramp.
        const bool cranking = twkP(self, AN_CRANK_BS) && twkB(self, AN_IS_CRANKING) != 0;
        const float target = cranking ? 1.0f : 0.0f;
        // ⚠️ `dt` is a FLOAT forwarded through a `double` parameter (the thunk rule keeps the bits
        // unconverted for the original). USING it as a double is therefore garbage -- the float sits
        // in the LOW 32 BITS. Round 6 bug: `(float)dt` gave ~0, so the blend step was 0 and crouch
        // froze at 0.00 even while cranking=1. REINTERPRET, never convert:
        uint64_t dtBits; memcpy(&dtBits, &dt, 8);
        uint32_t dtLo = (uint32_t)dtBits;
        float realDt; memcpy(&realDt, &dtLo, 4);
        if (!(realDt > 0.0f) || realDt > 0.25f) realDt = 1.0f / 60.0f;   // NaN/garbage fallback
        float step = realDt * 8.0f;                      // ~0.12 s to fully blend either way
        if (!(step > 0.0f)) step = 0.0f; else if (step > 1.0f) step = 1.0f;
        g_crankBlend += (target - g_crankBlend) * step;
        if (g_crankBlend < 0.001f) g_crankBlend = 0.0f; else if (g_crankBlend > 0.999f) g_crankBlend = 1.0f;
        const float t = g_crankBlend;
        // Stance, eased on the SAME ramp and for the same reason: you can land into switch mid-run,
        // and swapping the numbers on a frame boundary would pop the foot exactly where the landing
        // is already busy. IsSkatingSwitch is a plain PDB-named bool -- no asset gate on this one.
        const float sTarget = (twkB(self, AN_IS_SWITCH) != 0) ? 1.0f : 0.0f;
        g_switchBlend += (sTarget - g_switchBlend) * step;
        if (g_switchBlend < 0.001f) g_switchBlend = 0.0f; else if (g_switchBlend > 0.999f) g_switchBlend = 1.0f;
        const float sw = g_switchBlend;
        // Bilinear over the four numbers that own this foot: riding<->setup by the crouch, then
        // regular<->switch by the stance.
        const float regL = (float)g_nudgeL   + ((float)g_nudgeSetupL   - (float)g_nudgeL)   * t;
        const float regR = (float)g_nudgeR   + ((float)g_nudgeSetupR   - (float)g_nudgeR)   * t;
        const float swL  = (float)g_nudgeSwL + ((float)g_nudgeSwSetupL - (float)g_nudgeSwL) * t;
        const float swR  = (float)g_nudgeSwR + ((float)g_nudgeSwSetupR - (float)g_nudgeSwR) * t;
        const float mmL = regL + (swL - regL) * sw;
        const float mmR = regR + (swR - regR) * sw;
        g_uiCrank = t; g_uiSwitch = sw;
        if (mmL == 0.0f && mmR == 0.0f) return;
        // The PROVEN write: a constant offset on what the game just computed. Field verdict on the
        // 2.19.x absolute-height mode: "locks the feet and makes them stable" is THIS path; absolute
        // placement jittered and clipped. Offset preserves the animation's micro-motion; replacing
        // the height fought it. Positive slider = lower foot.
        const float dL = mmL / 10.0f, dR = mmR / 10.0f;             // cm
        const float zL0 = twkF(self, AN_L_SOCK_LOC + 8), zR0 = twkF(self, AN_R_SOCK_LOC + 8);
        *(float*)((uint8_t*)self + AN_L_SOCK_LOC + 8) = zL0 - dL;
        *(float*)((uint8_t*)self + AN_R_SOCK_LOC + 8) = zR0 - dR;
        const float appL = -dL, appR = -dR;
        g_uiAppliedL = appL;
        // ⚠️ UNCONDITIONAL first line, then 1 Hz. Round 4 logged only on a riding<->cranking
        // TRANSITION, so with the crouch signal reading 0 the hook printed NOTHING and "it does not
        // work" was indistinguishable from "the hook never ran". A correction that writes to the game
        // must always be able to prove it wrote.
        const LONG nth = InterlockedIncrement(&g_corrections);
        const double now = NowSec();
        if (nth == 1 || now - g_lastCorrLog > 1.0) {
            g_lastCorrLog = now;
            // Raw candidates stay in the line (-1 = its gate is closed): if the crouch is ever
            // wrong again, the SAME log names which field moved instead of costing another round.
            TwkLog("[foot] applying: crouch=%.2f (%s) [cranking=%d] stance=%.2f (%s) | "
                   "total L=%.1f R=%.1f mm | applied L=%+.2f R=%+.2f cm | shoe '%s'",
                   t, (t > 0.05f) ? "setup" : "riding", (int)cranking,
                   sw, (sw > 0.5f) ? "switch" : "regular", mmL, mmR, appL, appR,
                   g_curShoe[0] ? g_curShoe : "?");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[foot] caught fatal applying the foot correction -> paused (game unaffected)");
    }
}

// ---- Validate the debug block WITHOUT comparing across coordinate spaces (round 1's mistake).
// Two structural facts prove it, and the second one is a cross-check against an independent field:
//   1. entries 0/1 (and 6/7) must be a VERTICAL pair -- same X/Y, start above end. That is a
//      downward sweep, which is what start/end mean.
//   2. (rightHit.Z - rightEnd.Z) must equal `_lastRightFootAutoOffset.Z`. Two unrelated fields
//      agreeing to a hundredth of a centimetre cannot be coincidence, and it pins the L/R split.
static void CheckDebugBlock(void* anim) {
    if (g_dbgChecked) return;
    const V3 s0 = dbg(anim, DBG_L_START), e0 = dbg(anim, DBG_L_END);
    const V3 s6 = dbg(anim, DBG_R_START), e6 = dbg(anim, DBG_R_END);
    if (isZero(s0) || isZero(s6) || !plausible(s0) || !plausible(s6)) return;   // not filled yet
    g_dbgChecked = true;

    const bool vertL = fabsf(s0.x - e0.x) < 0.5f && fabsf(s0.y - e0.y) < 0.5f && s0.z > e0.z;
    const bool vertR = fabsf(s6.x - e6.x) < 0.5f && fabsf(s6.y - e6.y) < 0.5f && s6.z > e6.z;
    const float hitR = dbg(anim, DBG_R_HIT).z - e6.z;
    const float offR = twkF(anim, AN_R_AUTO_OFF + 8);
    const bool crossOk = fabsf(hitR - offR) < 0.05f;

    g_dbgLive = vertL && vertR;
    TwkLog("[foot] debug block %s: sweeps vertical L=%d R=%d (L %.2f cm long), "
           "cross-check rightHit-rightEnd=%.2f vs autoOffset.Z=%.2f -> %s",
           g_dbgLive ? "LIVE" : "REJECTED", (int)vertL, (int)vertR, s0.z - e0.z, hitR, offR,
           crossOk ? "AGREE (mapping confirmed)" : "disagree (mapping suspect -- treat with care)");
    if (!g_dumped) {
        g_dumped = true;
        for (int i = 0; i < DEBUG_COUNT; i++) {
            const V3 e = dbg(anim, i);
            TwkLog("[foot]   debug[%2d] @+0x%03x = (%.2f, %.2f, %.2f)",
                   i, AN_DEBUG_BASE + i * DEBUG_STRIDE, e.x, e.y, e.z);
        }
    }
}

static void EmitBlock(void* anim, bool manual) {
    const V3 sockL = dbg(anim, DBG_L_SOCKET), ancL = dbg(anim, DBG_L_ANCHOR), tgtL = dbg(anim, DBG_L_TARGET);
    const V3 sockR = dbg(anim, DBG_R_SOCKET), ancR = dbg(anim, DBG_R_ANCHOR), tgtR = dbg(anim, DBG_R_TARGET);
    const V3 stL = dbg(anim, DBG_L_START), enL = dbg(anim, DBG_L_END), hitL = dbg(anim, DBG_L_HIT);
    const V3 stR = dbg(anim, DBG_R_START), enR = dbg(anim, DBG_R_END), hitR = dbg(anim, DBG_R_HIT);
    const V3 offL = readV3(anim, AN_L_AUTO_OFF), offR = readV3(anim, AN_R_AUTO_OFF);
    const V3 adjL = readV3(anim, AN_L_BONE_ADJ), adjR = readV3(anim, AN_R_BONE_ADJ);
    const float aL = twkF(anim, AN_L_ALPHA), aR = twkF(anim, AN_R_ALPHA);

    // THE HOVER: socket and anchor both come from the debug block, so they share one space.
    const float hovL = sockL.z - ancL.z, hovR = sockR.z - ancR.z;
    const bool missL = fabsf(hitL.z - enL.z) < 0.01f, missR = fabsf(hitR.z - enR.z) < 0.01f;

    const LONG n = InterlockedIncrement(&g_blocks);
    TwkLog("[foot] ===== sample #%d %s  bodyInst L=%p R=%p%s",
           (int)n, manual ? "(manual)" : "(auto)", twkP(anim, AN_L_BODY), twkP(anim, AN_R_BODY),
           g_corrections ? "  [correction active]" : "");
    TwkLog("[foot]  HOVER (socket above deck anchor):  L = %.2f cm   R = %.2f cm    <-- this is the bug",
           hovL, hovR);
    TwkLog("[foot]  L: socket Z=%.2f  anchor Z=%.2f  target Z=%.2f%s",
           sockL.z, ancL.z, tgtL.z, fabsf(tgtL.z - sockL.z) < 0.01f ? "  (target == socket)" : "");
    TwkLog("[foot]  R: socket Z=%.2f  anchor Z=%.2f  target Z=%.2f%s",
           sockR.z, ancR.z, tgtR.z, fabsf(tgtR.z - sockR.z) < 0.01f ? "  (target == socket)" : "");
    TwkLog("[foot]  L sweep: %.2f -> %.2f (%.1f cm), hit %.2f %s | autoOffset Z=%.2f alpha=%.3f",
           stL.z, enL.z, stL.z - enL.z, hitL.z, missL ? "= END, NO HIT" : "", offL.z, aL);
    TwkLog("[foot]  R sweep: %.2f -> %.2f (%.1f cm), hit %.2f %s | autoOffset Z=%.2f alpha=%.3f",
           stR.z, enR.z, stR.z - enR.z, hitR.z, missR ? "= END, NO HIT" : "", offR.z, aR);
    TwkLog("[foot]  boneAdj L=(%.2f,%.2f,%.2f) R=(%.2f,%.2f,%.2f)",
           adjL.x, adjL.y, adjL.z, adjR.x, adjR.y, adjR.z);

    const float avg = 0.5f * (hovL + hovR);
    TwkLog("[foot]  raw hover %.2f cm average (measured BEFORE our nudge, so it is the untouched gap, "
           "not what you see on screen). Nudges: riding L=%d R=%d, setup L=%d R=%d mm.",
           avg, g_nudgeL, g_nudgeR, g_nudgeSetupL, g_nudgeSetupR);
    snprintf(g_uiVerdict, sizeof(g_uiVerdict), "raw hover L %.2f R %.2f cm", hovL, hovR);
    g_uiHovL = hovL; g_uiHovR = hovR;
    g_lastHov = hovL;
}

void FootPlace_PumpFrame() {
    if (!g_installed) return;
    InterlockedIncrement(&g_pumpTicks);
    if (!g_ok) return;
    __try {
        void* skater = CatchTweaks_Skater();
        if (!skater) {
            static bool said = false;
            if (!said) { said = true; TwkLog("[foot] waiting for a skater -- pop one ollie to publish it"); }
            return;
        }
        if (skater != g_skater) {                  // respawn / map switch
            g_skater = skater; g_anim = nullptr; g_lastBody = nullptr;
            g_settled = 0; g_dbgChecked = false; g_dbgLive = false;
        }
        void* mesh = twkP(skater, SK_MESH);
        void* anim = mesh ? twkP(mesh, MESH_ANIM) : nullptr;
        if (!anim) return;
        g_anim = anim;
        g_mesh = mesh;
        {   void* board = twkP(skater, SK_BOARD);
            g_flipper = board ? twkP(board, BOARD_FLIPPER) : nullptr; }

        // ---- per-shoe profile upkeep (game thread; only does work on a change edge)
        if (g_shoeProfiles) {
            static int prevSliders[8] = { -999, 0, 0, 0, 0, 0, 0, 0 };
            int cur[8]; for (int i = 0; i < 8; i++) cur[i] = *kNudges[i];
            bool sliderMoved = false;
            if (prevSliders[0] == -999) memcpy(prevSliders, cur, sizeof(cur));      // startup != a move
            else for (int i = 0; i < 8; i++) if (cur[i] != prevSliders[i]) { sliderMoved = true; break; }
            // Re-read on the costume-rebuild edge (cheap) and every 2 s (catches a change that reuses
            // the same body instance). Reading is a map walk over ~8 items -- nothing to husband.
            static double lastShoeCheck = 0.0;
            void* biL = twkP(anim, AN_L_BODY);
            static void* prevBiL = nullptr;
            const double nowS = NowSec();
            if (biL != prevBiL || nowS - lastShoeCheck > 2.0) {
                prevBiL = biL; lastShoeCheck = nowS;
                char shoe[48];
                if (ReadWornShoe(skater, shoe, sizeof(shoe)) && strcmp(shoe, g_curShoe) != 0) {
                    if (g_curShoe[0]) StoreCurrentProfile();   // bank the outgoing shoe before leaving
                    strncpy_s(g_curShoe, sizeof(g_curShoe), shoe, _TRUNCATE);
                    ShoeProfile* p = FindProfile(g_curShoe);
                    if (p) {
                        for (int i = 0; i < 8; i++) *kNudges[i] = p->n[i];
                        TwkLog("[foot] shoe '%s' -> loaded its saved nudges: regular %d/%d riding, "
                               "%d/%d setup | switch %d/%d riding, %d/%d setup (mm, L/R)", g_curShoe,
                               p->n[0], p->n[1], p->n[2], p->n[3], p->n[4], p->n[5], p->n[6], p->n[7]);
                    } else {
                        // A new shoe INHERITS the current numbers rather than snapping to zero: most
                        // shoes want something similar, and starting from the last good value is a
                        // better place to tune from than a foot suddenly back in the deck.
                        StoreCurrentProfile();
                        TwkLog("[foot] shoe '%s' is new -- starting from the current nudges (%d/%d, %d/%d mm); "
                               "tune them and they stay with this shoe", g_curShoe,
                               g_nudgeL, g_nudgeR, g_nudgeSetupL, g_nudgeSetupR);
                    }
                    for (int i = 0; i < 8; i++) prevSliders[i] = *kNudges[i];
                    TwkMarkDirty();
                }
            }
            if (sliderMoved) {                                  // tuning always belongs to the worn shoe
                memcpy(prevSliders, cur, sizeof(cur));
                StoreCurrentProfile();
                TwkMarkDirty();
            }
        }

        CheckDebugBlock(anim);                      // plane mode needs the validation, probe on or off
        if (!g_on) return;                          // fix can run with the probe's logging off
        if (!g_dbgLive) return;

        // ⚠️ The manual request is consumed FIRST and bypasses every gate. Field round 2: it was read
        // after the settle return, so pressing "Take a sample now" set a flag nothing ever reached --
        // the button silently did nothing. A button labelled NOW must fire now, gates or no gates.
        const bool manual = (InterlockedExchange(&g_sampleReq, 0) != 0);
        if (manual) { EmitBlock(anim, true); g_lastBlockT = NowSec(); return; }

        // ---- gate: on board, grounded, not mid-trick
        if (twkB(anim, AN_ON_BOARD) != 1 || twkB(anim, AN_GROUNDED) != 1) { g_settled = 0; return; }
        if (twkB(anim, AN_TRICK_PENDING) || twkB(anim, AN_CATCH_ST) ||
            twkB(anim, AN_FLIPPING) || twkB(anim, AN_ROTATING)) { g_settled = 0; return; }

        // ---- settle on the SOCKET, not on autoOffset (round 1 gated on autoOffset, which is pinned
        // near zero and so always looked settled -- samples fired mid-motion and the numbers wandered).
        // ⚠️ But a STRICT settle never fires at all: at 0.5 mm over 12 ticks, idle sway alone defeats
        // it, and field round 2 produced ZERO samples for exactly that reason. A rough number now
        // beats a perfect number never, so patience runs out and we sample anyway, reporting the
        // jitter alongside so a shaky sample is visible as such rather than trusted silently.
        const V3 sock = dbg(anim, DBG_L_SOCKET);
        if (isZero(sock) || !plausible(sock)) { g_settled = 0; g_gateTicks = 0; return; }
        g_gateTicks++;
        const float jit = fmaxf(fmaxf(fabsf(sock.x - g_prevSock.x), fabsf(sock.y - g_prevSock.y)),
                                fabsf(sock.z - g_prevSock.z));
        if (jit > g_jitter) g_jitter = jit;                    // worst movement seen while waiting
        if (jit < (float)g_settleMm / 100.0f) g_settled++; else g_settled = 0;
        g_prevSock = sock;
        const bool settledOk  = g_settled >= g_settleTicks;
        const bool outOfPatience = g_gateTicks > 180;          // ~3 s of standing is enough
        if (!settledOk && !outOfPatience) return;
        g_lastJitter = g_jitter; g_lastSettled = settledOk; g_jitter = 0.0f; g_gateTicks = 0;

        // ---- triggers
        void* body = twkP(anim, AN_L_BODY);         // POINTER VALUE ONLY
        const bool costumeChanged = (body != g_lastBody);
        const float hov = sock.z - dbg(anim, DBG_L_ANCHOR).z;
        const bool moved = (g_lastHov < -9000.0f) ||
                           (fabsf(hov - g_lastHov) > (float)g_minDeltaMm / 100.0f);
        const double now = NowSec();
        const bool spaced = (now - g_lastBlockT) > 2.0;
        if (g_blocks >= g_maxBlocks) return;
        if (!(spaced && (costumeChanged || (g_autoSample && moved)))) return;

        if (costumeChanged && g_lastBody)
            TwkLog("[foot] costume rebuilt (foot body %p -> %p) -- new shoe", g_lastBody, body);
        g_lastBody = body;
        g_lastBlockT = now;
        EmitBlock(anim, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[foot] caught fatal in the probe -> paused (your setting is untouched; game unaffected)");
    }
}

bool FootPlace_Grounded() {
    void* anim = g_anim;
    if (!anim) return false;
    __try { return twkB(anim, AN_GROUNDED) == 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FootPlace_SettingUpTrick() {
    void* anim = g_anim;
    if (!anim) return false;
    // Asset-gated exactly like the foot correction reads it: with no CrankLoopBlendSpace the crank
    // fields are stale, and a stale "cranking" would look like a permanent trick setup.
    __try { return twkP(anim, AN_CRANK_BS) && twkB(anim, AN_IS_CRANKING) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void FootPlace_Install() {
    g_installed = true;
    g_getGameInst = (GetGameInstFn)TwkScanExe(SIG_GET_GAME_INSTANCE);
    g_fnameToStr  = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR2);
    if (!g_getGameInst || !g_fnameToStr)
        TwkLog("[foot] wardrobe sigs NOT FOUND -- per-shoe profiles unavailable, sliders act globally");
    g_startAnchors = TwkScanExe(SIG_UPDATE_ANCHORS);
    if (!g_startAnchors) {
        TwkLog("[foot] UpdateFootAnchors sig NOT FOUND -- the correction cannot be applied (game updated?)");
    } else if (MH_CreateHook(g_startAnchors, (void*)&hkUpdateFootAnchors, &g_origAnchors) != MH_OK ||
               MH_EnableHook(g_startAnchors) != MH_OK) {
        TwkLog("[foot] hook failed on UpdateFootAnchors -- the correction cannot be applied");
        g_startAnchors = nullptr;
    } else {
        TwkLog("[foot] correction hook @ %p (inside the animation update, where a write survives)",
               g_startAnchors);
    }
    TwkLog("[foot] installed -- hover is measured as socket-above-anchor, both from the anim debug "
           "block so they share one space. Lowering is %s.",
           (g_nudgeL || g_nudgeR || g_nudgeSetupL || g_nudgeSetupR)
               ? "ON" : "off (use the F1 sliders, or the FootNudge* ini keys)");
}

void FootPlace_DrawMenu(const OmpMenuApi* api) {
    char b[224];
    if (!g_installed) { api->TextDisabled("Foot placement: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Foot placement probe", &on)) { FootPlace_SetEnabled(on); TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(measures how far the shoe floats above the deck)");
    api->Indent();
    if (!g_ok) api->TextDisabled("PAUSED by a fault this session -- untick and retick to retry");
    if (g_pumpTicks == 0)
        api->TextDisabled("no frame pump (scoop_speed's InputHandler::Tick hook missing) -- cannot run");
    snprintf(b, sizeof(b), "hover:  L %.2f cm   R %.2f cm      samples %d", g_uiHovL, g_uiHovR, (int)g_blocks);
    api->TextDisabled(b);
    api->TextDisabled(g_uiVerdict);
    if (api->version >= 2 && api->Button) {
        if (api->Button("Take a sample now")) InterlockedExchange(&g_sampleReq, 1);
    }
    if (!g_startAnchors) api->TextDisabled("correction hook missing -- the sliders below cannot work");
    // Left/right rather than front/back on purpose: the hover belongs to the shoe, so it should
    // follow the foot and stay right when you ride switch.
    // Both stances are always shown rather than following your feet: you need to be able to set the
    // switch numbers while standing in the menu, which is necessarily not while riding switch.
    const bool inSwitch = g_uiSwitch > 0.5f;
    static const char* const kLabels[8] = {
        "Regular, riding: left foot (mm)",   "Regular, riding: right foot (mm)",
        "Regular, setup: left foot (mm)",    "Regular, setup: right foot (mm)",
        "Switch, riding: left foot (mm)",    "Switch, riding: right foot (mm)",
        "Switch, setup: left foot (mm)",     "Switch, setup: right foot (mm)",
    };
    for (int i = 0; i < 8; i++) {
        if (i == 0 || i == 4) {
            if (i) api->Separator();
            const bool live = (i == 4) == inSwitch;
            api->TextDisabled(i == 0 ? (live ? "REGULAR  <- riding this now" : "REGULAR")
                                     : (live ? "SWITCH  <- riding this now"  : "SWITCH"));
        }
        float mm = (float)*kNudges[i];
        if (api->SliderFloat(kLabels[i], &mm, -20.0f, 50.0f, "%.0f")) {
            *kNudges[i] = (int)(mm + (mm < 0 ? -0.5f : 0.5f)); TwkMarkDirty();
        }
    }
    api->Separator();
    bool sp = g_shoeProfiles != 0;
    if (api->Checkbox("Remember these per shoe", &sp)) { g_shoeProfiles = sp ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(tune each shoe once; swapping shoes swaps the numbers)");
    if (!g_shoeProfiles)
        snprintf(b, sizeof(b), "off -- one set of sliders for every shoe");
    else if (g_curShoe[0]) {
        const int* def = DefaultsFor(g_curShoe);
        bool atDefault = def != nullptr;
        if (def) for (int i = 0; i < 8; i++) if (def[i] != *kNudges[i]) { atDefault = false; break; }
        snprintf(b, sizeof(b), "wearing '%s' -- %s  (%d shoe%s saved)", g_curShoe,
                 atDefault ? "shipped tuning" : (def ? "your tuning" : "not a stock shoe, your tuning"),
                 g_nProfiles, g_nProfiles == 1 ? "" : "s");
    }
    else
        snprintf(b, sizeof(b), "shoe not identified yet%s%s -- sliders act globally until it is",
                 g_wornSummary[0] ? ": " : "", g_wornSummary);
    api->TextDisabled(b);
    snprintf(b, sizeof(b), "positive lowers the foot. right now: crouch %.2f (0 riding, 1 setup), "
             "stance %.2f (0 regular, 1 switch)", g_uiCrank, g_uiSwitch);
    api->TextDisabled(b);
    api->Unindent();
}
