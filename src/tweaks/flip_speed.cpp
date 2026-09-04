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
// Flip speed from the flick, not from a single sampled frame.
//
// THE STOCK PIPELINE (read out of FlipTricksHandler::GetBoardFlipSpeedMultiplier, Epic 0x1024670 /
// Steam 0xfe47e0, `float (this, UFlipTrickDefinition*, EInputType, TArray<FInputData>*)`):
//
//     minM = tricksDb->FlipSpeedMinMultiplier            (db + 0x170)
//     maxM = skater->_advancedSettings.FlipSpeedMult     (skater + 0x9d0)  * db + 0x174
//     walk the recorded inputs for one whose EInputType == the trick's FlipSpeedModifierInput;
//     no match at all  ->  return 1.0 flat
//     mode (skater + 0x64f) == 2 -> ratio from stick MAGNITUDE, curve at db + 0x188
//     mode           == 3 -> ratio from stick ANGLE,    curve at db + 0x190
//     return minM + Curve->GetFloatValue(ratio) * (maxM - minM)
//
// TWO DEFECTS, and only the second is visible from the endpoints:
//
//   1. THE CURVE IS A STAIRCASE, the same defect the scoop's RotationSpeedCurve has. A measurement
//      round over ~30 flips found the multiplier spanning its full 0.606-1.750 range -- which is why
//      it was first written off as healthy -- but landing on EXACTLY 1.175 on about two thirds of
//      them. 1.175 is minM + 0.5*(maxM-minM) precisely, i.e. the curve returning a dead-flat 0.5
//      across its whole middle. Most of the range you can actually flick is mapped to one speed.
//      Checking that the endpoints move says nothing about the shape in between.
//
//   2. THE INPUT IS SAMPLED PER FRAME. Both modes read the recorded FInputData -- a stick position
//      captured on some frame boundary -- so what the game sees depends on where the frame landed
//      within a flick that lasts 60-120 ms. At a low or unstable frame rate the same physical flick
//      reads differently run to run, which is the frame-rate sensitivity that the scoop fix
//      noticeably improved for shove-its and that flips never got.
//
// THE FIX mirrors the scoop's: measure the gesture over TIME rather than per frame, and map it with
// a straight line between two thresholds you control. Peak radial speed comes from
// `ScoopSpeed_FlickMeasure`, which rides the InputHandler::Tick samples scoop_speed already owns --
// only one MinHook detour may exist on that address, so this module is FED rather than hooking it.
// The stock multiplier is computed first and logged beside ours, so the two are always comparable.
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "flip_speed.h"
#include "scoop_speed.h"
#include "catch_sound.h"
#include "ui/menu_ext.h"
#include "tweaks_mod.h"
#include "MinHook.h"
#include <math.h>

// ------------------------------------------------------------------ settings
static int   g_on        = 1;      // FlipSpeed
static int   g_log       = 0;      // FlipSpeedLog -- per-trick detail; diagnostic, off by default
// Stick-units/s x10, MEASURED not estimated: real flicks land between ~180 and ~520, so this range
// spans a lazy flick to about as fast as a thumb goes. Frame-rate independent, so the numbers mean
// the same thing on any machine. (The first two shipped ranges were guesses and both saturated at
// the top, pinning every trick to the maximum multiplier instead of tracking the flick.)
// x10, not x100: the pause menu's value field CLIPS at four digits, so a 1500..5000 range was
// displayed cut off. Keep any menu-facing number in this mod under 1000.
static int   g_velMin    = 150;    // FlipVelMin -- maps to the SLOWEST flip
static int   g_velMax    = 500;    // FlipVelMax -- ... and to the FASTEST
static int   g_windowMs  = 250;    // FlipWindowMs -- how far back a flick may have started
static int   g_usePeak   = 0;      // FlipUsePeakPush -- map displacement instead of speed
// With "Sync Flips & Scoops" on, UpdateBoardTargetFlipAndScoop COUPLES the two rotations: it reads
// both remainders and both rates together and drives them to finish as one motion. Overriding only
// the flip multiplier breaks that relationship, and the coupling keeps the flip turning while the
// shove catches up -- which is a second flip on a trick that has both, i.e. a treflip. So when the
// game is syncing, its own multiplier stands.
static int   g_respectSync = 0;    // FlipSpeedRespectSync -- see the note above. Tried against
                                   // the double-flip and did NOT fix it, so it is off by default
                                   // rather than disabling flip speed for no benefit.

void FlipSpeed_ReadConfig(const char* buf) {
    g_on       = TwkIniInt(buf, "FlipSpeed", 1);
    g_log      = TwkIniInt(buf, "FlipSpeedLog", 0);
    g_velMin   = TwkIniInt(buf, "FlipVelMin", 150);
    g_velMax   = TwkIniInt(buf, "FlipVelMax", 500);
    // Migrate a value saved under the old x100 scale rather than leaving it a hundred times too
    // large, which would map every flick to the slowest flip.
    if (g_velMax > 900) { g_velMin /= 10; g_velMax /= 10;
        TwkLog("[flip] flick range rescaled to x10 units -> [%d..%d]", g_velMin, g_velMax); }
    g_windowMs = TwkIniInt(buf, "FlipWindowMs", 250);
    g_usePeak  = TwkIniInt(buf, "FlipUsePeakPush", 0);
    g_respectSync = TwkIniInt(buf, "FlipSpeedRespectSync", 0);
    if (g_velMax <= g_velMin) g_velMax = g_velMin + 10;      // a zero-width range maps everything
    TwkLog("[flip] config: FlipSpeed=%d Vel=[%d..%d]/100 WindowMs=%d UsePeakPush=%d RespectSync=%d Log=%d",
           g_on, g_velMin, g_velMax, g_windowMs, g_usePeak, g_respectSync, g_log);
}
void FlipSpeed_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "FlipSpeed",        g_on);
    TwkIniSetInt(buf, cap, "FlipSpeedLog",     g_log);
    TwkIniSetInt(buf, cap, "FlipVelMin",       g_velMin);
    TwkIniSetInt(buf, cap, "FlipVelMax",       g_velMax);
    TwkIniSetInt(buf, cap, "FlipWindowMs",     g_windowMs);
    TwkIniSetInt(buf, cap, "FlipUsePeakPush",  g_usePeak);
    TwkIniSetInt(buf, cap, "FlipSpeedRespectSync", g_respectSync);
}
void FlipSpeed_ResetDefaults() {
    g_on = 1; g_log = 0; g_velMin = 150; g_velMax = 500; g_windowMs = 250; g_usePeak = 0;
    g_respectSync = 0;
}
bool FlipSpeed_Enabled() { return g_on != 0; }
void FlipSpeed_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
float FlipSpeed_VelMin() { return (float)g_velMin; }
float FlipSpeed_VelMax() { return (float)g_velMax; }
void  FlipSpeed_SetVelMin(float v) {
    g_velMin = (int)v; if (g_velMax <= g_velMin) g_velMax = g_velMin + 10;
    TwkMarkDirty();
}
void  FlipSpeed_SetVelMax(float v) {
    g_velMax = (int)v; if (g_velMax <= g_velMin) g_velMin = g_velMax - 10;
    TwkMarkDirty();
}

// ------------------------------------------------------------------ the hook
// Arity verified at the call site (CheckForTrick, 0x101e472): `(this, def, uint8, TArray*)`. NOTE
// this is NOT the scoop's argument list -- that one takes `(this, bool, TArray*)`. No float args in,
// so there is no XMM hazard on entry; the return is a float.
static const char* SIG_FLIP_MULT =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 EC F0 00 00 00 0F 29 70 D8 "
    "4D 8B F9";
typedef float (*FlipMultFn)(void*, void*, unsigned char, void*);

enum {
    FTH_SKATER     = 0x08,    // FlipTricksHandler -> skater  (the base the stock code uses)
    FTH_INPUT_HANDLER = 0x18,  // FlipTricksHandler -> InputHandler (the base the stock loop uses)
    FTH_TRICKS_DB  = 0x28,    // ... -> UTricksDatabase
    DB_FLIP_MIN    = 0x170,   // UTricksDatabase::FlipSpeedMinMultiplier
    DB_FLIP_MAX    = 0x174,   // ... MaxMultiplier
    SK_ADV_FLIP    = 0x9d0,   // ASkaterCharacterBase::_advancedSettings.FlipSpeedMultiplier
    SK_FLIP_MODE   = 0x64f,   // _boardFlipSpeedMode (2 = magnitude, 3 = angle; else no mapping)
    SK_SYNC_FLAGS  = 0x711,   // bit 0x20 = _isSyncFlipsScoopsEnabled
    // FInputData (36 B) -- the game's OWN record of the gesture, handed to us as arg 4. This is the
    // data the game itself matched to recognise the trick, so reading the flick from here works
    // wherever tricks work: no separate sampling of the stick, nothing to differ between machines.
    FID_INPUT = 0x00, FID_LSTICK = 0x08, FID_RSTICK = 0x10,
    FID_ANGLE = 0x18, FID_TOTAL_TIME = 0x20, FID_STRIDE = 0x24,
};

// ---- the game's own entry match ------------------------------------------------------------------
// The recorded EInputType is NOT comparable raw. GetBoardFlipSpeedMultiplier's own loop normalises
// each entry by stance first and compares THAT:
//     IsSkatingGoofy(skater) -> goofy ; IsSkatingSwitch(skater) -> sw
//     ConvertToCurrentInputModeInput(inputHandler, rawType, sw, goofy) ; cmp result, expectedType
// Comparing raw only agrees in regular stance. On goofy nothing matched, the lookup fell through to
// a guess, and it picked a different row of the array entirely -- one that had been active 300+ ms
// instead of the flick's own 17-25 ms. Same controller, same gesture, different entry, so every
// derived number was incomparable between two players. Call the game's functions and the entry is
// the one the game itself chose.
static const char* SIG_IS_GOOFY =
    "48 83 EC 28 48 8B 01 FF 90 ?? ?? ?? ?? 3C 01 0F 94 C0 48 83 C4 28 C3";
static const char* SIG_IS_SWITCH =
    "0F B6 81 98 05 00 00 2C 02 A8 FD 0F 94 C0 C3";
static const char* SIG_CONVERT_INPUT =
    "48 83 EC 28 0F B6 41 20 45 84 C9 ?? ?? 3C 02 ?? ?? 45 84 C0 ?? ?? 0F B6 CA E8 ?? ?? ?? ?? "
    "0F B6 D0";
typedef bool          (*IsStanceFn)(void*);
typedef unsigned char (*ConvertInputFn)(void*, unsigned int, unsigned int, unsigned int);
static IsStanceFn     g_isGoofy = nullptr, g_isSwitch = nullptr;
static ConvertInputFn g_convertInput = nullptr;
// Normalise a recorded type the way the game does. Falls back to the raw value when the functions
// are unavailable, which is the old behaviour rather than no behaviour.
static unsigned char NormalisedType(void* fth, unsigned char raw) {
    if (!g_convertInput || !g_isGoofy || !g_isSwitch) return raw;
    unsigned char out = raw;
    __try {
        void* skater = twkP(fth, FTH_SKATER);
        void* ih     = twkP(fth, FTH_INPUT_HANDLER);
        if (!skater || !ih) return raw;
        const unsigned int goofy = g_isGoofy(skater) ? 1u : 0u;
        const unsigned int sw    = g_isSwitch(skater) ? 1u : 0u;
        out = g_convertInput(ih, raw, sw, goofy);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return raw; }
    return out;
}
static void* g_orig = nullptr, *g_start = nullptr;
static bool  g_installed = false;
static int   g_ok = 1;                       // runtime health, NEVER persisted
static volatile LONG g_calls = 0;
// Last shot, for the F1 panel.
static volatile float g_uiSpeed = 0.0f, g_uiPeak = 0.0f, g_uiStock = 0.0f, g_uiOurs = 0.0f;
// The trick most recently selected, for the catch trace: a catch is only interesting alongside what
// was being caught, and the catch path has no trick name of its own.
static char g_lastTrick[80] = "?";
const char* FlipSpeed_LastTrickName() { return g_lastTrick; }
static volatile long long g_trickQpc = 0;   // QPC at the last trick selection
// Milliseconds since the last trick SELECTION, or -1 with no trick yet. For the catch-engage
// latency measurement: a catch engaging ~0 ms after the pop is the held-stick pop-time engage.
int FlipSpeed_MsSinceTrick() {
    const long long t0 = g_trickQpc;
    if (!t0) return -1;
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    return (int)((now.QuadPart - t0) * 1000 / fq.QuadPart);
}
static volatile LONG g_trickSerial = 0;
LONG FlipSpeed_TrickSerial() { return g_trickSerial; }


// ---------------------------------------------------------------------------------------------
// One-shot trick-definition dump, once per unique def per session. RECON for the base-game nollie
// heelflip bug (bottom-right flick dips the NOSE instead of the tail, then the trick animation
// gives way to a catch pose): trick-specific misbehaviour points at per-def DATA, and these are
// the def's direction-sensitive fields -- BoardControlInversePitch and the pitch/roll board
// control extras, the authored start/end pitch blocks, the flip-speed input type and its Inputs
// list, and the catch targets. A session covering the bad trick and its well-behaved siblings
// (nollie kickflip, regular heelflip) turns the diff into the diagnosis.
static void DumpTrickDefOnce(void* def) {
    if (!def) return;
    static void* seen[32]; static int nSeen = 0;
    for (int i = 0; i < nSeen; ++i) if (seen[i] == def) return;
    if (nSeen < 32) seen[nSeen++] = def;
    char nm[96]; CatchSound_ObjName(def, nm, sizeof(nm));
    char ins[64]; int off = 0;
    const int nIn = twkI(def, 0xe0);
    void* inArr = twkP(def, 0xd8);
    for (int i = 0; i < nIn && i < 8 && inArr && off < (int)sizeof(ins) - 5; ++i)
        off += snprintf(ins + off, sizeof(ins) - off, "%s%d", i ? "," : "", (int)twkB(inArr, i));
    TwkLog("[trickdef] '%s' | flipSpdInput=%d catchFoot=%d | invPitch=%d exPitch dn=%.1f up=%.1f "
           "| exRollPitch dn=%.1f up=%.1f exRollScoop bs=%.1f fs=%.1f "
           "| startPitch ovr=%d (%.1f..%.1f) endPitch (%.1f..%.1f) "
           "| precatch %.0f/%.0f pitchPre %.1f..%.1f otherFoot=%.2f | catchTgt P=%.1f R=%.1f "
           "| inputs[%d]=%s | manualCatchThr flip=%.0f rot=%.0f forceAuto=%d",
           nm, (int)twkB(def, 0x1bc), (int)twkB(def, 0x50),
           (int)twkB(def, 0x228), twkF(def, 0x22c), twkF(def, 0x230),
           twkF(def, 0x234), twkF(def, 0x238), twkF(def, 0x244), twkF(def, 0x248),
           (int)twkB(def, 0x1e0), twkF(def, 0x1e8 + 0xc), twkF(def, 0x1e8 + 0x10),
           twkF(def, 0x208 + 0xc), twkF(def, 0x208 + 0x10),
           twkF(def, 0x258), twkF(def, 0x25c), twkF(def, 0x260), twkF(def, 0x264),
           twkF(def, 0x268), twkF(def, 0x26c), twkF(def, 0x270),
           nIn, ins, twkF(def, 0x290), twkF(def, 0x294), (int)twkB(def, 0x254));
}

static float hkFlipMult(void* self, void* def, unsigned char inputType, void* inputs) {
    float stock = 1.0f;
    __try { stock = ((FlipMultFn)g_orig)(self, def, inputType, inputs); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_calls) == 1)
            TwkLog("[flip] caught fatal in GetBoardFlipSpeedMultiplier -> passing 1.0");
        return 1.0f;
    }
    if (!g_on || !g_ok) return stock;
    __try {
        // A trick with no flip-speed input (`FlipSpeedModifierInput` == None) is not an analog flip
        // at all -- the stock function returns 1.0 immediately for it. Substituting there would
        // put a flip speed on tricks the game deliberately gives none.
        if (inputType == 0) return stock;
        void* skater = twkP(self, FTH_SKATER);
        void* db     = twkP(self, FTH_TRICKS_DB);
        if (!skater || !db) return stock;
        // A remote skater's trick is not the player's: it gets the stock multiplier, and it must
        // not bump the trick serial the catch logic keys its timing off.
        if (Twk_IsProxy(skater)) return stock;
        if (def) CatchSound_ObjName(def, g_lastTrick, sizeof(g_lastTrick));
        DumpTrickDefOnce(def);
        InterlockedIncrement(&g_trickSerial);          // a trick just started; the pump watches it
        { LARGE_INTEGER t; QueryPerformanceCounter(&t); g_trickQpc = t.QuadPart; }
        const bool syncing = (twkB(skater, SK_SYNC_FLAGS) & 0x20) != 0;
        if (syncing && g_respectSync) {
            static bool said = false;
            if (!said) {
                said = true;
                TwkLog("[flip] 'Sync Flips & Scoops' is ON -- the game couples the flip and shove "
                       "rates, so its own flip multiplier stands (ours would desync them and the "
                       "flip keeps turning while the shove catches up). FlipSpeedRespectSync=0 to "
                       "override anyway.");
            }
            return stock;
        }
        const unsigned char mode = (unsigned char)twkI(skater, SK_FLIP_MODE);
        if (mode != 2 && mode != 3) return stock;      // no analog mapping is running; leave it be
        const float minM = twkF(db, DB_FLIP_MIN);
        const float maxM = twkF(db, DB_FLIP_MAX) * twkF(skater, SK_ADV_FLIP);
        if (!(maxM > minM) || !(minM > 0.0f) || minM > 10.0f || maxM > 10.0f) return stock;

        // WHICH STICK IS DECIDED BY THE MATCHED ENTRY'S OWN DATA, further down -- NOT by the
        // trick definition's expected type. The EInputType left/right mirror at +50 describes the
        // RAW type, and the game normalises by stance before it means anything: in goofy the flick is
        // performed with the other stick, so deriving it from the expected type sampled the wrong
        // tracker and produced flip speeds that felt nothing like the regular-stance ones. This is
        // only the starting guess for the case where the entry carries no usable stick data.
        bool rightStick = (inputType >= 50);

        // ---- the flick, from the GAME'S OWN INPUT RECORD -------------------------------------------
        // Read this, not our own stick sampling. Sampling InputHandler's raw stick fields ourselves
        // works on some machines and not others: one field log had the left stick's Y pinned at
        // exactly -1.00 for a whole session while the game recognised every trick perfectly, so the
        // player's gesture simply was not in the field we were reading there. The array the game hands
        // us is the record it MATCHED to pick the trick, and it already carries both the stick
        // position and TotalTime -- the duration of the gesture as the game measured it.
        float gameSpeed = 0.0f, gameMag = 0.0f, gameTime = 0.0f;
        float entryLm = 0.0f, entryRm = 0.0f;
        bool  gameExact = false;
        if (inputs) {
            void* data = twkP(inputs, 0);
            const int n = twkI(inputs, 8);
            if (data && n > 0 && n < 64) {
                // DO NOT REQUIRE THE RAW TYPE TO MATCH. The game's own loop converts each recorded
                // type through IsSkatingGoofy / IsSkatingSwitch / ConvertToCurrentInputModeInput and
                // compares the CONVERTED value, so the raw byte only equals the expected type in
                // regular stance on the default input mode. Matching it raw meant every trick on any
                // other stance or mode found nothing, fell through to "no flick measurement", and
                // returned the game's own multiplier -- which looks like the feature working (the
                // game's analog still varies) while none of these sliders do anything at all.
                //
                // Rather than reproduce that conversion, pick by what a flick IS: the FASTEST
                // gesture in the array, displacement over its own duration.
                // NOT the largest displacement. A stick being HELD reads at full magnitude and
                // beats a quick flick at half -- measured, that selected a 333-483 ms hold at
                // magnitude 1.0 over the actual flick, and handed back a multiplier below the game's
                // own on every trick. Biggest is not fastest, and the flick is the fast one.
                int best = -1; float bestMag = 0.0f, bestSpeed = 0.0f;
                for (int i = 0; i < n; i++) {
                    const uint8_t* e = (const uint8_t*)data + (size_t)i * FID_STRIDE;
                    const float tt = twkF(e, FID_TOTAL_TIME);
                    if (!(tt > 0.001f)) continue;
                    const float lx = twkF(e, FID_LSTICK),  ly = twkF(e, FID_LSTICK + 4);
                    const float rx = twkF(e, FID_RSTICK),  ry = twkF(e, FID_RSTICK + 4);
                    const float lm = sqrtf(lx * lx + ly * ly), rm = sqrtf(rx * rx + ry * ry);
                    const float m  = (lm > rm) ? lm : rm;
                    const float sp = m / tt;
                    const bool exact = (NormalisedType(self, twkB(e, FID_INPUT)) == inputType);
                    // An exact match still wins if there is one; otherwise the fastest gesture does.
                    if ((exact && !gameExact) || ((exact == gameExact) && sp > bestSpeed)) {
                        best = i; bestMag = m; bestSpeed = sp; gameExact = exact;
                    }
                }
                if (best >= 0) {
                    const uint8_t* e = (const uint8_t*)data + (size_t)best * FID_STRIDE;
                    gameMag  = bestMag;
                    gameTime = twkF(e, FID_TOTAL_TIME);
                    if (gameMag > 0.05f) gameSpeed = (gameMag / gameTime) * 10.0f;
                    // The hand that actually did it: whichever of this entry's two sticks holds the
                    // displacement. True in any stance, so nothing here has to know about goofy.
                    const float lx = twkF(e, FID_LSTICK), ly = twkF(e, FID_LSTICK + 4);
                    const float rx = twkF(e, FID_RSTICK), ry = twkF(e, FID_RSTICK + 4);
                    const float lm = sqrtf(lx * lx + ly * ly), rm = sqrtf(rx * rx + ry * ry);
                    if (lm > rm * 1.25f)      rightStick = false;
                    else if (rm > lm * 1.25f) rightStick = true;
                    entryLm = lm; entryRm = rm;
                }
            }
        }

        // SIZE THE LOOKBACK FROM THE GAME'S OWN GESTURE AGE. A fixed 250 ms window assumes the
        // flick just happened -- true if you flick and release, false if you flick and HOLD, where
        // the movement is already off the left edge of the window and all we can see is the stick
        // parked at full deflection. Measured: one player's window opened with the stick already at
        // (0.00,-1.00) and the game reporting a 333-483 ms gesture, so their flick predated
        // everything we looked at. TotalTime is that age, so it is exactly the right lookback.
        float lookback = (float)g_windowMs / 1000.0f;
        if (gameTime > lookback) lookback = gameTime * 1.3f;      // a margin for the run-up
        if (lookback > 1.5f) lookback = 1.5f;                     // never unbounded
        // DO NOT CHOOSE A STICK. Every rule tried for that was a stance assumption in disguise --
        // the trick definition's expected type, then the matched entry's own stick fields -- and each
        // one broke a different combination of goofy / switch / nollie / fakie, because which thumb
        // performs a flick changes with all of them. There is nothing to decide: measure BOTH and
        // take whichever actually moved. That is the flick by definition, in every stance, and it is
        // how the raw-sampling version behaved before any of this selection logic existed.
        float speed = 0.0f, peak = 0.0f, frameMs = 0.0f;
        float lSpeed = 0.0f, lPeak = 0.0f, rSpeed = 0.0f, rPeak = 0.0f, fm = 0.0f;
        const bool okL = ScoopSpeed_FlickMeasure(false, lookback, &lSpeed, &lPeak, &frameMs);
        const bool okR = ScoopSpeed_FlickMeasure(true,  lookback, &rSpeed, &rPeak, &fm);
        const bool sampled = okL || okR;
        rightStick = (rSpeed > lSpeed);
        speed = rightStick ? rSpeed : lSpeed;
        peak  = rightStick ? rPeak  : lPeak;
        // With the window sized correctly, our own sampling is the better measure: it finds the
        // MOVEMENT BURST inside the gesture, so a flick scores the same whether it was released or
        // held afterwards. The game's record stays as the fallback for when we have no samples --
        // it cannot separate the flick from the hold, because TotalTime covers both.
        const bool fromGame = (!sampled && gameSpeed > 0.0f);
        if (fromGame) speed = gameSpeed;
        if (!fromGame && !sampled) {
            // No usable gesture: the game's own value stands. Mapping an unknown onto our range would
            // quietly ask for the slowest flip on every trick we failed to measure.
            if (g_log) {
                TwkLog("[flip] #%ld no flick measurement -- keeping stock %.3f (peak %.2f)",
                       InterlockedIncrement(&g_calls), stock, peak);
                ScoopSpeed_DumpFlick(rightStick, (float)g_windowMs / 1000.0f);
            }
            return stock;
        }
        if (g_log) ScoopSpeed_DumpFlick(rightStick, (float)g_windowMs / 1000.0f);
        const float measure = g_usePeak ? (peak * 1000.0f) : speed;
        const float lo = (float)g_velMin, hi = (float)g_velMax;
        float ratio = ((fromGame ? measure : measure * 10.0f) - lo) / (hi - lo);
        if (ratio < 0.0f) ratio = 0.0f; else if (ratio > 1.0f) ratio = 1.0f;
        const float ours = minM + ratio * (maxM - minM);

        g_uiSpeed = speed; g_uiPeak = peak; g_uiStock = stock; g_uiOurs = ours;
        const LONG n = InterlockedIncrement(&g_calls);
        if (g_log) {
            // The trick's NAME is logged because the multiplier alone cannot tell a single kickflip
            // from a double -- and which of the two the game picked is a question the numbers here
            // simply do not answer.
            char trick[80];
            if (!def || !CatchSound_ObjName(def, trick, sizeof(trick))) strcpy(trick, "?");
            // The frame time is logged BESIDE the flick so frame-rate dependence is a comparison
            // anyone can make from one log rather than a claim: the same flick at 8 ms and at 16 ms
            // per frame must read the same number.
            TwkLog("[flip] #%ld '%s' type=%u stick=%c mode=%u | %s: %.0f "
                   "(game mag %.2f / %.0f ms, lookback %.0f ms, entry L%.2f R%.2f%s; "
                   "flick L%.0f R%.0f -> %c, peak %.2f) | "
                   "stock %.3f -> ours %.3f "
                   "(ratio %.3f of %.3f..%.3f)",
                   n, trick, (unsigned)inputType, rightStick ? 'R' : 'L', (unsigned)mode,
                   fromGame ? (gameExact ? "GAME RECORD" : "GAME RECORD~") : (gameExact ? "sampled" : "sampled~"),
                   fromGame ? speed : speed * 10.0f,
                   gameMag, gameTime * 1000.0f, lookback * 1000.0f, entryLm, entryRm,
                   gameExact ? "" : " NO EXACT TYPE MATCH", lSpeed * 10.0f, rSpeed * 10.0f,
                   rightStick ? 'R' : 'L', peak,
                   stock, ours, ratio, minM, maxM);
        }
        return ours;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[flip] caught fatal mapping flip speed -> paused (your setting is untouched)");
        return stock;
    }
}

void FlipSpeed_Install() {
    g_isGoofy      = (IsStanceFn)TwkScanExe(SIG_IS_GOOFY);
    g_isSwitch     = (IsStanceFn)TwkScanExe(SIG_IS_SWITCH);
    g_convertInput = (ConvertInputFn)TwkScanExe(SIG_CONVERT_INPUT);
    if (!g_isGoofy || !g_isSwitch || !g_convertInput)
        TwkLog("[flip] stance-conversion sigs NOT FOUND (goofy=%p switch=%p convert=%p) -- input "
               "types compared RAW, which only agrees in regular stance",
               (void*)g_isGoofy, (void*)g_isSwitch, (void*)g_convertInput);
    else
        TwkLog("[flip] input types normalised by stance, as the game does (goofy/switch/convert resolved)");
    g_installed = true;
    g_start = TwkScanExe(SIG_FLIP_MULT);
    if (!g_start) {
        TwkLog("[flip] GetBoardFlipSpeedMultiplier sig NOT FOUND -- flip speed stays stock (game updated?)");
        return;
    }
    if (MH_CreateHook(g_start, (void*)&hkFlipMult, &g_orig) != MH_OK ||
        MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[flip] hook failed on GetBoardFlipSpeedMultiplier -- flip speed stays stock");
        g_start = nullptr;
        return;
    }
    TwkLog("[flip] installed @ %p -- flip speed from the flick over %d ms, %s (%s)",
           g_start, g_windowMs, g_usePeak ? "peak push" : "peak speed", g_on ? "ON" : "off");
}

void FlipSpeed_DrawMenu(const OmpMenuApi* api) {
    char b[224];
    if (!g_installed) { api->TextDisabled("Flip speed: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Flip speed from the flick", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(how fast you flick sets how fast the board flips)");
    api->Indent();
    if (!g_start) api->TextDisabled("hook missing -- the sliders below cannot work");
    if (!g_ok)    api->TextDisabled("PAUSED by a fault this session -- untick and retick to retry");
    float lo = (float)g_velMin, hi = (float)g_velMax;
    if (api->SliderFloat("Slowest at (stick/s x10)", &lo, 20.0f, 400.0f, "%.0f")) {
        g_velMin = (int)lo; if (g_velMax <= g_velMin) g_velMax = g_velMin + 10; TwkMarkDirty();
    }
    if (api->SliderFloat("Fastest at (stick/s x10)", &hi, 50.0f, 800.0f, "%.0f")) {
        g_velMax = (int)hi; if (g_velMax <= g_velMin) g_velMin = g_velMax - 10; TwkMarkDirty();
    }
    snprintf(b, sizeof(b), "last flick: %.2f u/s (peak push %.2f)   stock %.3f -> ours %.3f",
             g_uiSpeed, g_uiPeak, g_uiStock, g_uiOurs);
    api->TextDisabled(b);
    api->TextDisabled("Stock maps most flicks to one speed (a flat spot in the game's curve) and "
                      "reads the stick on a single frame, so frame rate changes the result.");
    bool pk = g_usePeak != 0;
    if (api->Checkbox("Use how FAR you flick instead of how fast", &pk)) { g_usePeak = pk ? 1 : 0; TwkMarkDirty(); }
    bool lg = g_log != 0;
    if (api->Checkbox("Log every flip", &lg)) { g_log = lg ? 1 : 0; TwkMarkDirty(); }
    api->Unindent();
}
// See the header note. SEH-guarded: the resolvers are game code handed a game pointer.
bool FlipSpeed_Stance(void* skater, bool* goofy, bool* switchStance) {
    if (!skater || !g_isGoofy || !g_isSwitch) return false;
    __try {
        if (goofy)        *goofy        = g_isGoofy(skater);
        if (switchStance) *switchStance = g_isSwitch(skater);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
