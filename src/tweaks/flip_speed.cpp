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
// ⚠️ x10, not x100: the pause menu's value field CLIPS at four digits, so a 1500..5000 range was
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
static volatile LONG g_trickSerial = 0;
LONG FlipSpeed_TrickSerial() { return g_trickSerial; }

enum {
    FTH_SKATER     = 0x08,    // FlipTricksHandler -> skater  (the base the stock code uses)
    FTH_TRICKS_DB  = 0x28,    // ... -> UTricksDatabase
    DB_FLIP_MIN    = 0x170,   // UTricksDatabase::FlipSpeedMinMultiplier
    DB_FLIP_MAX    = 0x174,   // ... MaxMultiplier
    SK_ADV_FLIP    = 0x9d0,   // ASkaterCharacterBase::_advancedSettings.FlipSpeedMultiplier
    SK_FLIP_MODE   = 0x64f,   // _boardFlipSpeedMode (2 = magnitude, 3 = angle; else no mapping)
    SK_SYNC_FLAGS  = 0x711,   // bit 0x20 = _isSyncFlipsScoopsEnabled
};

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
        if (def) CatchSound_ObjName(def, g_lastTrick, sizeof(g_lastTrick));
        InterlockedIncrement(&g_trickSerial);          // a trick just started; the pump watches it
        void* skater = twkP(self, FTH_SKATER);
        void* db     = twkP(self, FTH_TRICKS_DB);
        if (!skater || !db) return stock;
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

        // EInputType mirrors left/right at +50 (measured across every pairing the scoop logs ever
        // produced), so the expected type names its own stick -- no stance logic needed.
        const bool rightStick = (inputType >= 50);
        float speed = 0.0f, peak = 0.0f, frameMs = 0.0f;
        if (!ScoopSpeed_FlickMeasure(rightStick, (float)g_windowMs / 1000.0f, &speed, &peak, &frameMs)) {
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
        const float measure = g_usePeak ? (peak * 1000.0f) : speed;   // peak is 0..1, scale to match
        const float lo = (float)g_velMin, hi = (float)g_velMax;
        float ratio = (measure * 10.0f - lo) / (hi - lo);
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
            TwkLog("[flip] #%ld '%s' type=%u stick=%c mode=%u | flick %.2f u/s peak %.2f "
                   "(frame %.1f ms) | stock %.3f -> ours %.3f (ratio %.3f of %.3f..%.3f)",
                   n, trick, (unsigned)inputType, rightStick ? 'R' : 'L', (unsigned)mode,
                   speed, peak, frameMs, stock, ours, ratio, minM, maxM);
        }
        return ours;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ok = 0;
        TwkLog("[flip] caught fatal mapping flip speed -> paused (your setting is untouched)");
        return stock;
    }
}

void FlipSpeed_Install() {
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
