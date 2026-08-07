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
// SessionTweaks -- gameplay fixes & improvements for Session, one module per concern:
//   scoop_speed   scoop (shove-it) speed from how fast the stick was actually swept
//   catch_tweaks  wider MANUAL catch window + darkslide-aware eaten-catch fix
//   run_out       low-air missed-trick bails become the native run-out
//   catch_level   board levels out when it hits your foot (restores a removed feature)
//   grind_pop     read-only probe on the grind-exit pop pipeline (measurement, changes nothing)
//   cloth_physics read-only cloth/wind recon dump (merged mesh, props, material params; no writes)
//
// A SEPARATE UE4SS C++ mod (Mods\SessionTweaks\dlls\main.dll), deliberately not part of the
// multiplayer mod: gameplay feel and co-op ship together by the user's distribution choice, but they
// enable/disable independently. Its menu section rides SessionOpenMP's F1 window through the
// menu_ext.h seam; WITHOUT the MP mod it still works fully, configured by SessionTweaks.ini alone.
//
// This TU owns only the shell: the log file, the ini read (one file, each module parses its own
// keys), the shared MinHook instance, the F1 host probe, and the UE4SS mod class. All game
// knowledge -- offsets, sigs, hooks, knobs -- lives in the module that owns it.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#define PSAPI_VERSION 2          // K32EnumProcessModules from kernel32 -- no psapi.lib dependency
#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include "tweaks_common.h"
#include "tweaks_mod.h"
#include "scoop_speed.h"
#include "catch_tweaks.h"
#include "run_out.h"
#include "catch_level.h"
#include "catch_sound.h"
#include "grind_pop.h"
#include "cloth_physics.h"
#include "MinHook.h"
#include "ue4ss_abi.h"
#include "ui/menu_ext.h"

// ------------------------------------------------------------------ log (own file, fresh per launch)
static FILE* g_log = nullptr;
void TwkLog(const char* fmt, ...) {
    if (!g_log) return;
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, b);
    fflush(g_log);
}

// ------------------------------------------------------------------ ini (one file, modules own keys)
static char g_iniPath[MAX_PATH] = {0};
// Debounced auto-save: F1 changes mark dirty (render thread); once quiet for 2 s the game thread
// rewrites JUST the values in the ini (comments preserved by TwkIniSetInt's line splicing).
static volatile LONG     g_dirty = 0;
static volatile LONGLONG g_dirtyMs = 0;
void TwkMarkDirty() {
    InterlockedExchange64(&g_dirtyMs, (LONGLONG)GetTickCount64());
    InterlockedExchange(&g_dirty, 1);
}
static void saveSettings() {
    if (!g_iniPath[0]) return;
    static char buf[16384]; buf[0] = 0;
    FILE* f = fopen(g_iniPath, "r");
    if (f) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
    ScoopSpeed_SaveConfig(buf, sizeof(buf));
    CatchTweaks_SaveConfig(buf, sizeof(buf));
    RunOut_SaveConfig(buf, sizeof(buf));
    CatchLevel_SaveConfig(buf, sizeof(buf));
    CatchSound_SaveConfig(buf, sizeof(buf));
    GrindPop_SaveConfig(buf, sizeof(buf));
    ClothPhys_SaveConfig(buf, sizeof(buf));
    f = fopen(g_iniPath, "w");
    if (!f) { TwkLog("[tweaks] settings save FAILED (cannot write %s)", g_iniPath); return; }
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
    TwkLog("[tweaks] settings saved to SessionTweaks.ini");
}

static void readConfig(const char* dir) {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%sSessionTweaks.ini", dir);
    snprintf(g_iniPath, sizeof(g_iniPath), "%s", p);
    // Generous buffer, and truncation is REPORTED: a config reader that silently drops settings is
    // worse than one that refuses.
    static char buf[16384]; buf[0] = 0;
    FILE* f = fopen(p, "r");
    const bool existed = (f != nullptr);
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
        fclose(f);
        if (n == sizeof(buf) - 1) TwkLog("[tweaks] WARNING: SessionTweaks.ini exceeds 16 KB and was truncated -- settings past the cut are IGNORED");
    }
    // Each module parses its own keys and echoes every one, so a value that failed to parse is
    // visible at load time.
    ScoopSpeed_ReadConfig(buf);
    CatchTweaks_ReadConfig(buf);
    RunOut_ReadConfig(buf);
    CatchLevel_ReadConfig(buf);
    CatchSound_ReadConfig(buf);
    GrindPop_ReadConfig(buf);
    ClothPhys_ReadConfig(buf);
    // No ini yet: write one holding the defaults just loaded. Without the multiplayer mod there is
    // no menu to change a setting through, so the file IS the interface -- and a file that lists
    // every key at its current value is the only way to discover what can be configured. Writing it
    // from the live values also means it can never drift from the code the way a hand-kept sample
    // would. (TwkIniSetInt appends any key it does not find, so an empty buffer yields a full file.)
    if (!existed) {
        TwkLog("[tweaks] no SessionTweaks.ini -- writing one with the built-in defaults");
        saveSettings();
    }
}

// ------------------------------------------------------------------ F1 menu (host seam)
// Every module's shipped defaults. For the scoop those are the measured calibration (VelMin 350 /
// VelMax 1600), so this restores tuned values rather than blanking them.
static void resetAllDefaults() {
    ScoopSpeed_ResetDefaults();
    CatchTweaks_ResetDefaults();
    RunOut_ResetDefaults();
    CatchLevel_ResetDefaults();
    CatchSound_ResetDefaults();
    GrindPop_ResetDefaults();
    ClothPhys_ResetDefaults();
    TwkLog("[tweaks] settings reset to defaults");
}

static void drawSection(const OmpMenuApi* api, void*) {
    if (!api || api->version < 1) return;
    ScoopSpeed_DrawMenu(api);
    api->Separator();
    CatchTweaks_DrawMenu(api);
    api->Separator();
    RunOut_DrawMenu(api);
    api->Separator();
    CatchLevel_DrawMenu(api);
    api->Separator();
    CatchSound_DrawMenu(api);
    api->Separator();
    GrindPop_DrawMenu(api);
    api->Separator();
    ClothPhys_DrawMenu(api);
    // The same reset the pause menu offers, so neither surface is the only way to get back.
    if (api->version >= 2 && api->Button) {
        api->Separator();
        if (api->Button("Reset to defaults")) resetAllDefaults();
    }
}
// ------------------------------------------------------------------ pause-menu page (host seam #2)
// The same master switches, reachable from the game's own pause menu. Both callbacks below run on
// the GAME THREAD, inside the engine's menu code -- the opposite contract from drawSection above,
// which is a render-thread ImGui callback. The module accessors they call are plain int
// reads/writes, safe from either.
static const char* const kTwkScoop = "TwkScoop";
static const char* const kTwkCatch = "TwkCatch";
static const char* const kTwkRunOut = "TwkRunOut";

static const char* const kTwkDrop     = "TwkRunOutDrop";
static const char* const kTwkVelMin   = "TwkScoopVelMin";
static const char* const kTwkVelMax   = "TwkScoopVelMax";
static const char* const kTwkCatchX   = "TwkCatchWindowPct";
static const char* const kTwkDsZone   = "TwkDarkslideZone";
static const char* const kTwkCatchSnd = "TwkCatchSound";
static const char* const kTwkSndVol   = "TwkCatchSoundVol";
static const char* const kTwkLevel    = "TwkCatchLevel";
static const char* const kTwkGPitch   = "TwkGrindPitch";
static const char* const kTwkGPitchAmt = "TwkGrindPitchAmt";
static const char* const kTwkGSwing   = "TwkGrindSwing";
static const char* const kTwkGSwingAmt = "TwkGrindSwingAmt";
static const char* const kTwkReset    = "TwkResetDefaults";

// An ACTION row on the page: pressed, not adjusted.
static void pageSelect(const char* key, void*) {
    if (key && !strcmp(key, kTwkReset)) resetAllDefaults();
}

// A toggle or slider CHANGED. `iv` is the option index, `fv` the slider's value in the units the
// page registered.
static void pageValue(const char* key, int iv, float fv, void*) {
    if (!key) return;
    if      (!strcmp(key, kTwkScoop))  ScoopSpeed_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkCatch))  CatchTweaks_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkRunOut)) RunOut_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkDrop))   RunOut_SetMaxDropCm(fv);
    else if (!strcmp(key, kTwkVelMin)) ScoopSpeed_SetVelMin(fv);
    else if (!strcmp(key, kTwkVelMax)) ScoopSpeed_SetVelMax(fv);
    else if (!strcmp(key, kTwkCatchX)) CatchTweaks_SetWindowMultPct(fv);
    else if (!strcmp(key, kTwkDsZone)) CatchTweaks_SetDarkslideZoneDeg(fv);
    else if (!strcmp(key, kTwkCatchSnd)) CatchSound_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkSndVol))   CatchSound_SetVolumePct(fv);
    else if (!strcmp(key, kTwkLevel))    CatchLevel_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkGPitch))    GrindPop_SetPitchEnabled(iv != 0);
    else if (!strcmp(key, kTwkGPitchAmt)) GrindPop_SetPitchScale(fv);
    else if (!strcmp(key, kTwkGSwing))    GrindPop_SetSwingEnabled(iv != 0);
    else if (!strcmp(key, kTwkGSwingAmt)) GrindPop_SetSwingBlend(fv);
}
// ...and what it is set to RIGHT NOW, so a row opens showing the truth instead of a default.
static int pageGet(const char* key, int* oi, float* of, void*) {
    if (!key) return 0;
    if      (!strcmp(key, kTwkScoop))  { *oi = ScoopSpeed_Enabled()  ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkCatch))  { *oi = CatchTweaks_Enabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkRunOut)) { *oi = RunOut_Enabled()      ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkDrop))   { *of = RunOut_MaxDropCm();           return 1; }
    else if (!strcmp(key, kTwkVelMin)) { *of = ScoopSpeed_VelMin();          return 1; }
    else if (!strcmp(key, kTwkVelMax)) { *of = ScoopSpeed_VelMax();          return 1; }
    else if (!strcmp(key, kTwkCatchX)) { *of = CatchTweaks_WindowMultPct();  return 1; }
    else if (!strcmp(key, kTwkDsZone)) { *of = CatchTweaks_DarkslideZoneDeg(); return 1; }
    else if (!strcmp(key, kTwkCatchSnd)) { *oi = CatchSound_Enabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkSndVol))   { *of = CatchSound_VolumePct();       return 1; }
    else if (!strcmp(key, kTwkLevel))    { *oi = CatchLevel_Enabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGPitch))    { *oi = GrindPop_PitchEnabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGPitchAmt)) { *of = GrindPop_PitchScale();           return 1; }
    else if (!strcmp(key, kTwkGSwing))    { *oi = GrindPop_SwingEnabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGSwingAmt)) { *of = GrindPop_SwingBlend();           return 1; }
    return 0;
}
// Every slider's units are chosen so its INTEGER readout is meaningful -- the pause menu's progress
// row prints "%d" (cm, deg/s, deg, percent), unlike the F1 sliders which can format floats.
static const OmpPageItem2 kTwkPageItems[] = {
    { OMP_ITEM_TOGGLE, kTwkScoop,  "Scoop speed fix",         "Stick speed drives how fast the board scoops" },
    { OMP_ITEM_SLIDER, kTwkVelMin, "  Slowest at (deg/s)",    "Stick speed that gives the slowest scoop",
      nullptr, nullptr, 50.0f, 1500.0f, 50.0f },
    { OMP_ITEM_SLIDER, kTwkVelMax, "  Fastest at (deg/s)",    "Stick speed that gives the fastest scoop",
      nullptr, nullptr, 200.0f, 3000.0f, 100.0f },
    { OMP_ITEM_TOGGLE, kTwkCatch,  "Wider manual catch",      "Widens the catch window while Catch Mode is manual" },
    { OMP_ITEM_SLIDER, kTwkCatchX, "  Catch window (%)",      "200 = twice the stock window",
      nullptr, nullptr, 100.0f, 400.0f, 25.0f },
    { OMP_ITEM_SLIDER, kTwkDsZone, "  Dark slide zone (deg)", "How far from grip-down a dark slide is still reserved",
      nullptr, nullptr, 10.0f, 170.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkRunOut, "Run out instead of bail", "Low missed tricks run out on foot (needs manual Catch Mode)" },
    { OMP_ITEM_SLIDER, kTwkDrop,   "  Real bail if drop over (cm)", "Above this drop a missed trick still bails",
      nullptr, nullptr, 50.0f, 600.0f, 25.0f },
    { OMP_ITEM_TOGGLE, kTwkLevel,   "Level board on catch",   "Eases the board flat when it hits your foot" },
    { OMP_ITEM_TOGGLE, kTwkCatchSnd, "Catch sound fix",       "A catch sound on every catch, and never too quiet" },
    { OMP_ITEM_SLIDER, kTwkSndVol,  "  Catch sound (%)",      "100 = the game's own volume",
      nullptr, nullptr, 50.0f, 300.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkGPitch,    "Pitch control out of grinds", "Board Control pitch works coming off a grind (set Board Control Mode to manual)" },
    { OMP_ITEM_SLIDER, kTwkGPitchAmt, "  Pitch amount (%)",   "How much of your input reaches the board out of a grind",
      nullptr, nullptr, 0.0f, 200.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkGSwing,    "Pop swing out of grinds",   "The board drops its tail before popping out of a grind" },
    { OMP_ITEM_SLIDER, kTwkGSwingAmt, "  Swing amount (%)",   "100 = the trick's full pop swing",
      nullptr, nullptr, 0.0f, 100.0f, 10.0f },
    // "Reset to defaults" MUST stay last only as a matter of taste -- but note the host clamps a
    // guest's count to OMP_PAGEITEM_MAX by truncating the TAIL, so if this list ever exceeds the cap
    // it is this row that vanishes, not the newest one. The static_assert below is what makes that
    // impossible to ship by accident.
    { OMP_ITEM_ACTION, kTwkReset,  "Reset to defaults",       "Restore every Session Tweaks setting to its shipped value" },
};
// Enforced at COMPILE time, so the next row added fails the build instead of quietly deleting the
// reset row in someone's pause menu.
static_assert(sizeof(kTwkPageItems) / sizeof(kTwkPageItems[0]) <= OMP_PAGEITEM_MAX,
              "SessionTweaks pause page exceeds OMP_PAGEITEM_MAX -- the host truncates the TAIL "
              "silently, so the row that vanishes is the LAST one, not the one just added. Raise the "
              "cap in menu_ext.h (host-side only) and ship both DLLs, or move the control to F1.");
typedef int (*OmpMenuRegisterPage2Fn)(const char*, const OmpPageItem2*, int, OmpPageSelectFn,
                                      OmpPageValueFn, OmpPageGetFn, OmpPageStatusFn, void*);
static bool g_pageRegistered = false;

static bool g_menuRegistered = false;
typedef int (*OmpMenuRegisterFn)(const char*, OmpMenuDrawFn, void*);
// Every UE4SS mod DLL is named main.dll, so the host is found by EXPORT, not by name: enumerate
// loaded modules and probe each for OmpMenu_Register. Retried from the frame pump -- mods.txt order
// decides who loads first, and the seam must work either way.
static bool tryRegisterMenu() {
    if (g_menuRegistered && g_pageRegistered) return true;
    HMODULE mods[512]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    int n = (int)(needed / sizeof(HMODULE)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        if (!g_menuRegistered) {
            auto reg = (OmpMenuRegisterFn)GetProcAddress(mods[i], "OmpMenu_Register");
            if (reg && reg("Session Tweaks", &drawSection, nullptr)) {
                g_menuRegistered = true;
                TwkLog("[tweaks] registered in the F1 menu (host found: SessionOpenMP)");
            }
        }
        // The pause-menu seam is probed SEPARATELY: an older host DLL exports the F1 one and not
        // this, and losing the in-game page must never cost the F1 section (or vice versa).
        if (!g_pageRegistered) {
            auto regp = (OmpMenuRegisterPage2Fn)GetProcAddress(mods[i], "OmpMenu_RegisterPage2");
            if (regp && regp("Session Tweaks", kTwkPageItems,
                             (int)(sizeof(kTwkPageItems) / sizeof(kTwkPageItems[0])),
                             &pageSelect, &pageValue, &pageGet, nullptr, nullptr)) {
                g_pageRegistered = true;
                TwkLog("[tweaks] registered a page in the game's pause menu (native toggles + slider)");
            }
        }
        if (g_menuRegistered && g_pageRegistered) return true;
    }
    return false;
}
void Tweaks_PumpFrame() {
    RunOut_PumpFrame();              // the armed post-conversion fall watch + facing
    CatchLevel_PumpFrame();          // catch-triggered board leveling
    CatchSound_PumpFrame();          // catch-sound telemetry (the fixes live in the hooks, not here)
    GrindPop_PumpFrame();            // grind-exit pop records: names resolved and logged out here
    ClothPhys_PumpFrame();           // runs a cloth/material dump when the F1 button requested one
    if (g_dirty && (LONGLONG)GetTickCount64() - g_dirtyMs > 2000) {
        InterlockedExchange(&g_dirty, 0);
        saveSettings();
    }
    if (g_menuRegistered && g_pageRegistered) return;
    static uint64_t lastTry = 0;
    const uint64_t ms = GetTickCount64();
    if (ms - lastTry > 2000) { lastTry = ms; tryRegisterMenu(); }
}

// ------------------------------------------------------------------ UE4SS shell
class SessionTweaks : public RC::CppUserModBase
{
public:
    SessionTweaks() {
        ModName = STR("SessionTweaks");
        // Version history is kept with the releases, not in the source.
        ModVersion = STR("2.13.1");
        ModDescription = STR("Gameplay fixes: real-stick-sweep scoop speed, wider manual catch window, darkslide-aware catch fix, run out on missed tricks");
        ModAuthors = STR("matsix");
        char dir[MAX_PATH]{};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *(slash + 1) = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sSessionTweaks.log", dir);
        g_log = fopen(path, "w");
        TwkLog("=== SessionTweaks 2.13.1 loading (UE4SS C++ mod) ===");
        readConfig(dir);
    }
    ~SessionTweaks() override {
        TwkLog("=== SessionTweaks unloaded ===");
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    // The exe is fully mapped by unreal-init, so the sig scans run here. This mod uses its own
    // statically linked MinHook instance; SessionOpenMP's is a separate instance hooking DIFFERENT
    // functions, so there is no conflict. Each module's install is per-symbol non-fatal: a game
    // update degrades to "feature off", never to a hook on the wrong function.
    auto on_unreal_init() -> void override {
        if (MH_Initialize() != MH_OK) { TwkLog("[tweaks] *** MinHook init failed -- mod inactive"); return; }
        ScoopSpeed_Install();
        CatchTweaks_Install();
        CatchLevel_Install();
    CatchSound_Install();
        GrindPop_Install();
        ClothPhys_Install();
        RunOut_Install();
        // Registration is attempted once now (host usually loaded already; mods.txt order) and
        // re-offered from the frame pump until the host appears, or forever if it never does:
        // without SessionOpenMP this mod still works fully, configured by the ini alone.
        if (!tryRegisterMenu())
            TwkLog("[tweaks] F1 menu host not found yet -- will keep offering (ini config active meanwhile)");
    }
    // UE4SS event-loop thread: nothing that touches the game may live here.
    auto on_update() -> void override {}
};

#define TWEAKS_MOD_API __declspec(dllexport)
extern "C" {
    TWEAKS_MOD_API RC::CppUserModBase* start_mod()           { return new SessionTweaks(); }
    TWEAKS_MOD_API void uninstall_mod(RC::CppUserModBase* m) { delete m; }
}
