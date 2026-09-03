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
//   catch_sound   a catch sound on every catch (flip-trick anims carry no sound notify at all)
//   pitch_range   spreads board pitch over the whole flick range, not just its first third
//   foot_place    why the shoe sits off the deck (probe + one default-off clamp override)
//   foot_steer    mid-trick foot control: the thumbsticks move the feet while you are in the air
//   grind_pop     read-only probe on the grind-exit pop pipeline (measurement, changes nothing)
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
#include "flip_speed.h"
#include "catch_tweaks.h"
#include "run_out.h"
#include "catch_level.h"
#include "catch_sound.h"
#include "cloth_merge.h"
#include "cloth_sim.h"
#include "pitch_range.h"
#include "foot_place.h"
#include "foot_steer.h"
#include "grind_pop.h"
#include "camera_height.h"
#include "pop_probe.h"
#include "body_feel.h"
#include "MinHook.h"
#include "ue4ss_abi.h"
#include "ui/menu_ext.h"

#define TWEAKS_VERSION "3.19.222"
#define TWK_WIDEN(x) STR(x)   // STR() prepends L before the macro expands; expand first

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
    static char buf[65536]; buf[0] = 0;
    FILE* f = fopen(g_iniPath, "r");
    if (f) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
    ScoopSpeed_SaveConfig(buf, sizeof(buf));
    FlipSpeed_SaveConfig(buf, sizeof(buf));
    CatchTweaks_SaveConfig(buf, sizeof(buf));
    RunOut_SaveConfig(buf, sizeof(buf));
    CatchLevel_SaveConfig(buf, sizeof(buf));
    CatchSound_SaveConfig(buf, sizeof(buf));
    ClothMerge_SaveConfig(buf, sizeof(buf));
    ClothSim_SaveConfig(buf, sizeof(buf));
    PitchRange_SaveConfig(buf, sizeof(buf));
    FootPlace_SaveConfig(buf, sizeof(buf));
    FootSteer_SaveConfig(buf, sizeof(buf));
    GrindPop_SaveConfig(buf, sizeof(buf));
    PopProbe_SaveConfig(buf, sizeof(buf));
    BodyFeel_SaveConfig(buf, sizeof(buf));
    CameraHeight_SaveConfig(buf, sizeof(buf));
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
    static char buf[65536]; buf[0] = 0;
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
    FlipSpeed_ReadConfig(buf);
    CatchTweaks_ReadConfig(buf);
    RunOut_ReadConfig(buf);
    CatchLevel_ReadConfig(buf);
    CatchSound_ReadConfig(buf);
    ClothMerge_ReadConfig(buf);
    ClothSim_ReadConfig(buf);
    PitchRange_ReadConfig(buf);
    FootPlace_ReadConfig(buf);
    FootSteer_ReadConfig(buf);
    GrindPop_ReadConfig(buf);
    PopProbe_ReadConfig(buf);
    BodyFeel_ReadConfig(buf);
    CameraHeight_ReadConfig(buf);
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
    FlipSpeed_ResetDefaults();
    CatchTweaks_ResetDefaults();
    RunOut_ResetDefaults();
    CatchLevel_ResetDefaults();
    CatchSound_ResetDefaults();
    ClothMerge_ResetDefaults();
    ClothSim_ResetDefaults();
    PitchRange_ResetDefaults();
    FootPlace_ResetDefaults();
    FootSteer_ResetDefaults();
    GrindPop_ResetDefaults();
    PopProbe_ResetDefaults();
    BodyFeel_ResetDefaults();
    CameraHeight_ResetDefaults();
    TwkLog("[tweaks] settings reset to defaults");
}

// The F1 section, in the SAME categories as the pause-menu pages. It used to be all fourteen modules
// one after another with separators between -- every module reasonable on its own, the whole thing a
// screenful you had to read to navigate. The pause menu already had the right taxonomy; this is only
// the F1 surface catching up to it, so a setting is in the same place whichever surface you opened.
//
// The groups need host API v3. An older host has no BeginGroup, so the flat layout stays as the
// fallback rather than the section going missing: the seam's whole promise is that either side can be
// older than the other.
static void drawSection(const OmpMenuApi* api, void*) {
    if (!api || api->version < 1) return;

    const bool grouped = (api->version >= 3 && api->BeginGroup && api->EndGroup);
    // Open a category, draw its modules, close it. Written as a lambda over a small table rather than
    // repeated eight times, so adding a module is one line in one place.
    const auto group = [&](const char* title, void (*const* fns)(const OmpMenuApi*), int n) {
        if (grouped) {
            if (api->BeginGroup(title)) {
                for (int i = 0; i < n; i++) { if (i) api->Separator(); fns[i](api); }
            }
            api->EndGroup();                       // once per BeginGroup, whatever it returned
        } else {
            api->Separator();
            api->Text(title);
            for (int i = 0; i < n; i++) fns[i](api);
        }
    };

    typedef void (*DrawFn)(const OmpMenuApi*);
    static DrawFn const kPop[]    = { PopProbe_DrawMenu };
    static DrawFn const kBoard[]  = { FlipSpeed_DrawMenu, ScoopSpeed_DrawMenu, PitchRange_DrawMenu };
    static DrawFn const kCatch[]  = { CatchTweaks_DrawMenu, CatchLevel_DrawMenu, CatchSound_DrawMenu,
                                      RunOut_DrawMenu };
    static DrawFn const kGrind[]  = { GrindPop_DrawMenu };
    static DrawFn const kFeet[]   = { FootPlace_DrawMenu, FootSteer_DrawMenu };
    static DrawFn const kCamera[] = { CameraHeight_DrawMenu };
    static DrawFn const kCloth[]  = { ClothMerge_DrawMenu, ClothSim_DrawMenu };
    #define TWK_GROUP(title, arr) group(title, arr, (int)(sizeof(arr) / sizeof(arr[0])))
    TWK_GROUP("Pop control",    kPop);
    TWK_GROUP("Board & tricks", kBoard);
    TWK_GROUP("Catch & bail",   kCatch);
    TWK_GROUP("Grinds",         kGrind);
    TWK_GROUP("Feet",           kFeet);
    TWK_GROUP("Camera",         kCamera);
    TWK_GROUP("Clothing",       kCloth);
    #undef TWK_GROUP

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
static const char* const kTwkSndVol   = "TwkCatchSoundVol";
static const char* const kTwkLevel    = "TwkCatchLevel";
static const char* const kTwkGPitch   = "TwkGrindPitch";
static const char* const kTwkGPitchAmt = "TwkGrindPitchAmt";
static const char* const kTwkGSwing   = "TwkGrindSwing";
static const char* const kTwkGSwingAmt = "TwkGrindSwingAmt";
static const char* const kTwkPitch    = "TwkPitchRange";
static const char* const kTwkPitchAmt = "TwkPitchSpread";
static const char* const kTwkStopFlip = "TwkCatchStopsFlip";
static const char* const kTwkClickCat = "TwkCatchClickToCatch";
static const char* const kTwkAnyRev   = "TwkCatchAnyRevolution";
static const char* const kTwkCloth     = "TwkCloth";
static const char* const kTwkClothMove = "TwkClothMove";
static const char* const kTwkClothHem  = "TwkClothHemLift";
static const char* const kTwkClothHemUp = "TwkClothHemLiftReach";
static const char* const kTwkClothCuff = "TwkClothCuffGrip";
static const char* const kTwkCamFollow    = "TwkCamFollow";
static const char* const kTwkCamPitchDrop = "TwkCamPitchDrop";
static const char* const kTwkCamPitch  = "TwkCamPitch";
static const char* const kTwkFootLvl  = "TwkCatchFootLevelsBoard";
static const char* const kTwkFlip     = "TwkFlipSpeed";
static const char* const kTwkFlipMin  = "TwkFlipVelMin";
static const char* const kTwkFlipMax  = "TwkFlipVelMax";
static const char* const kTwkSteer    = "TwkFootSteer";
static const char* const kTwkSteerCm  = "TwkFootSteerReach";
static const char* const kTwkSteerMs  = "TwkFootSteerResponse";
static const char* const kTwkSteerAxX = "TwkFootSteerAxisX";
static const char* const kTwkSteerAxY = "TwkFootSteerAxisY";
static const char* const kTwkSteerTw  = "TwkFootSteerTwistDeg";
static const char* const kTwkSteerTwA = "TwkFootSteerTwistAxis";
static const char* const kTwkBone     = "TwkBoneScalePct";
static const char* const kTwkBoneX    = "TwkBoneAddX";
static const char* const kTwkBoneY    = "TwkBoneAddY";
static const char* const kTwkBoneZ    = "TwkBoneAddZ";
static const char* const kTwkBodyFeel  = "TwkBodyFeel";
static const char* const kTwkBodyAmt   = "TwkBodyFeelAmount";
static const char* const kTwkBrace     = "TwkBodyBrace";
static const char* const kTwkArmPow    = "TwkBodyArmPct";
static const char* const kTwkFallPow   = "TwkBodyFallPct";
static const char* const kTwkReachPow  = "TwkBodyReachPct";
static const char* const kTwkCarryPow  = "TwkBodyCarryPct";
static const char* const kTwkFlailLen  = "TwkBodyFlailMs";
static const char* const kTwkFlailDel  = "TwkBodyFlailDelayMs";
static const char* const kTwkFlailPow  = "TwkBodyFlailPct";
static const char* const kTwkGrabLen   = "TwkBodyGrabMs";
static const char* const kTwkGrabDel   = "TwkBodyGrabDelayMs";
static const char* const kTwkGrabPow   = "TwkBodyGrabPct";
static const char* const kTwkPop       = "TwkPopControl";
static const char* const kTwkPopWin    = "TwkPopWindow";
static const char* const kTwkPopGate   = "TwkPopGate";
static const char* const kTwkPopVisLo  = "TwkPopVisStart";
static const char* const kTwkPopVisHi  = "TwkPopVisBottom";
static const char* const kTwkPopVisSm  = "TwkPopVisSmooth";
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
    else if (!strcmp(key, kTwkVelMin)) ScoopSpeed_SetVelMin(fv * 10.0f);   // shown in tens
    else if (!strcmp(key, kTwkVelMax)) ScoopSpeed_SetVelMax(fv * 10.0f);
    else if (!strcmp(key, kTwkCatchX)) CatchTweaks_SetWindowMultPct(fv);
    else if (!strcmp(key, kTwkDsZone)) CatchTweaks_SetDarkslideZoneDeg(fv);
    else if (!strcmp(key, kTwkSndVol))   CatchSound_SetVolumePct(fv);
    else if (!strcmp(key, kTwkLevel))    CatchLevel_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkStopFlip)) CatchTweaks_SetStopsFlip(iv != 0);
    else if (!strcmp(key, kTwkClickCat)) CatchTweaks_SetClickToCatch(iv != 0);
    else if (!strcmp(key, kTwkAnyRev))   CatchTweaks_SetAnyRevolution(iv != 0);
    else if (!strcmp(key, kTwkFootLvl))  CatchTweaks_SetFootLevelsBoard(iv != 0);
    else if (!strcmp(key, kTwkGPitch))    GrindPop_SetPitchEnabled(iv != 0);
    else if (!strcmp(key, kTwkGPitchAmt)) GrindPop_SetPitchScale(fv);
    else if (!strcmp(key, kTwkGSwing))    GrindPop_SetSwingEnabled(iv != 0);
    else if (!strcmp(key, kTwkGSwingAmt)) GrindPop_SetSwingBlend(fv);
    else if (!strcmp(key, kTwkPitch))     PitchRange_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkPitchAmt))  PitchRange_SetMaxAngleDeg(fv);
    else if (!strcmp(key, kTwkFlip))      FlipSpeed_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkFlipMin))   FlipSpeed_SetVelMin(fv);
    else if (!strcmp(key, kTwkFlipMax))   FlipSpeed_SetVelMax(fv);
    else if (!strcmp(key, kTwkSteer))     FootSteer_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkSteerCm))   FootSteer_SetReachCm(fv);
    else if (!strcmp(key, kTwkSteerMs))   FootSteer_SetResponseMs(fv);
    else if (!strcmp(key, kTwkSteerAxX))  FootSteer_SetAxisX(fv);
    else if (!strcmp(key, kTwkSteerAxY))  FootSteer_SetAxisY(fv);
    else if (!strcmp(key, kTwkSteerTw))   FootSteer_SetTwistDeg(fv);
    else if (!strcmp(key, kTwkSteerTwA))  FootSteer_SetTwistAxis(fv);
    else if (!strcmp(key, kTwkBone))      CatchTweaks_SetBoneScalePct(fv);
    else if (!strcmp(key, kTwkBoneX))     CatchTweaks_SetBoneAdd(0, fv);
    else if (!strcmp(key, kTwkBoneY))     CatchTweaks_SetBoneAdd(1, fv);
    else if (!strcmp(key, kTwkBoneZ))     CatchTweaks_SetBoneAdd(2, fv);
    else if (!strcmp(key, kTwkCloth))     ClothSim_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkClothMove)) ClothSim_SetTravelCm(fv);
    else if (!strcmp(key, kTwkClothHem))  ClothSim_SetHemPushMm(fv);
    else if (!strcmp(key, kTwkClothHemUp)) ClothSim_SetHemPushBandPct(fv);
    else if (!strcmp(key, kTwkClothCuff)) ClothSim_SetCuffGripPct(fv);
    else if (!strcmp(key, kTwkCamFollow))    CameraHeight_SetFollowEnabled(iv != 0);
    else if (!strcmp(key, kTwkCamPitchDrop)) CameraHeight_SetPitchOnDropEnabled(iv != 0);
    else if (!strcmp(key, kTwkCamPitch))  CameraHeight_SetPitchDeg(fv);
    else if (!strcmp(key, kTwkBodyFeel))  BodyFeel_SetEnabled(iv != 0);
    else if (!strcmp(key, kTwkBodyAmt))   BodyFeel_SetAmountPct(fv);
    else if (!strcmp(key, kTwkBrace))     BodyFeel_SetBraceEnabled(iv != 0);
    else if (!strcmp(key, kTwkArmPow))    BodyFeel_SetArmPct(fv);
    else if (!strcmp(key, kTwkFallPow))   BodyFeel_SetFallPct(fv);
    else if (!strcmp(key, kTwkReachPow))  BodyFeel_SetReachPct(fv);
    else if (!strcmp(key, kTwkCarryPow))  BodyFeel_SetCarryPct(fv);
    else if (!strcmp(key, kTwkFlailLen))  BodyFeel_SetFlailMs(fv);
    else if (!strcmp(key, kTwkFlailDel))  BodyFeel_SetFlailDelayMs(fv);
    else if (!strcmp(key, kTwkFlailPow))  BodyFeel_SetFlailPct(fv);
    else if (!strcmp(key, kTwkGrabLen))   BodyFeel_SetGrabMs(fv);
    else if (!strcmp(key, kTwkGrabDel))   BodyFeel_SetGrabDelayMs(fv);
    else if (!strcmp(key, kTwkGrabPow))   BodyFeel_SetGrabPct(fv);
    else if (!strcmp(key, kTwkPop))       PopProbe_SetSchemeEnabled(iv != 0);
    else if (!strcmp(key, kTwkPopWin))    PopProbe_SetTrickWindowMs(fv);
    else if (!strcmp(key, kTwkPopGate))   PopProbe_SetCrouchGatePct(fv);
    else if (!strcmp(key, kTwkPopVisLo))  PopProbe_SetCrankVisMinMs(fv);
    else if (!strcmp(key, kTwkPopVisHi))  PopProbe_SetCrankVisTimeMs(fv);
    else if (!strcmp(key, kTwkPopVisSm))  PopProbe_SetCrankVisSmoothMs(fv);
}
// ...and what it is set to RIGHT NOW, so a row opens showing the truth instead of a default.
static int pageGet(const char* key, int* oi, float* of, void*) {
    if (!key) return 0;
    if      (!strcmp(key, kTwkScoop))  { *oi = ScoopSpeed_Enabled()  ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkCatch))  { *oi = CatchTweaks_Enabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkRunOut)) { *oi = RunOut_Enabled()      ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkDrop))   { *of = RunOut_MaxDropCm();           return 1; }
    else if (!strcmp(key, kTwkVelMin)) { *of = ScoopSpeed_VelMin() / 10.0f;  return 1; }
    else if (!strcmp(key, kTwkVelMax)) { *of = ScoopSpeed_VelMax() / 10.0f;  return 1; }
    else if (!strcmp(key, kTwkCatchX)) { *of = CatchTweaks_WindowMultPct();  return 1; }
    else if (!strcmp(key, kTwkDsZone)) { *of = CatchTweaks_DarkslideZoneDeg(); return 1; }
    else if (!strcmp(key, kTwkSndVol))   { *of = CatchSound_VolumePct();       return 1; }
    else if (!strcmp(key, kTwkLevel))    { *oi = CatchLevel_Enabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkStopFlip)) { *oi = CatchTweaks_StopsFlip() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkClickCat)) { *oi = CatchTweaks_ClickToCatch() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkAnyRev))   { *oi = CatchTweaks_AnyRevolution() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkFootLvl))  { *oi = CatchTweaks_FootLevelsBoard() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGPitch))    { *oi = GrindPop_PitchEnabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGPitchAmt)) { *of = GrindPop_PitchScale();           return 1; }
    else if (!strcmp(key, kTwkGSwing))    { *oi = GrindPop_SwingEnabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkGSwingAmt)) { *of = GrindPop_SwingBlend();           return 1; }
    else if (!strcmp(key, kTwkPitch))     { *oi = PitchRange_Enabled() ? 1 : 0;    return 1; }
    else if (!strcmp(key, kTwkPitchAmt))  { *of = PitchRange_MaxAngleDeg();        return 1; }
    else if (!strcmp(key, kTwkFlip))      { *oi = FlipSpeed_Enabled() ? 1 : 0;     return 1; }
    else if (!strcmp(key, kTwkFlipMin))   { *of = FlipSpeed_VelMin();              return 1; }
    else if (!strcmp(key, kTwkFlipMax))   { *of = FlipSpeed_VelMax();              return 1; }
    else if (!strcmp(key, kTwkSteer))     { *oi = FootSteer_Enabled() ? 1 : 0;     return 1; }
    else if (!strcmp(key, kTwkSteerCm))   { *of = FootSteer_ReachCm();             return 1; }
    else if (!strcmp(key, kTwkSteerMs))   { *of = FootSteer_ResponseMs();          return 1; }
    else if (!strcmp(key, kTwkSteerAxX))  { *of = FootSteer_AxisX();              return 1; }
    else if (!strcmp(key, kTwkSteerAxY))  { *of = FootSteer_AxisY();              return 1; }
    else if (!strcmp(key, kTwkSteerTw))   { *of = FootSteer_TwistDeg();           return 1; }
    else if (!strcmp(key, kTwkSteerTwA))  { *of = FootSteer_TwistAxis();          return 1; }
    else if (!strcmp(key, kTwkCloth))     { *oi = ClothSim_Enabled() ? 1 : 0;    return 1; }
    else if (!strcmp(key, kTwkClothMove)) { *of = ClothSim_TravelCm();            return 1; }
    else if (!strcmp(key, kTwkClothHem))  { *of = ClothSim_HemPushMm();           return 1; }
    else if (!strcmp(key, kTwkClothHemUp)) { *of = ClothSim_HemPushBandPct();     return 1; }
    else if (!strcmp(key, kTwkClothCuff)) { *of = ClothSim_CuffGripPct();         return 1; }
    else if (!strcmp(key, kTwkCamFollow))    { *oi = CameraHeight_FollowEnabled()      ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkCamPitchDrop)) { *oi = CameraHeight_PitchOnDropEnabled() ? 1 : 0; return 1; }
    else if (!strcmp(key, kTwkCamPitch))  { *of = CameraHeight_PitchDeg();            return 1; }
    else if (!strcmp(key, kTwkBone))      { *of = CatchTweaks_BoneScalePct();          return 1; }
    else if (!strcmp(key, kTwkBoneX))     { *of = CatchTweaks_BoneAdd(0);              return 1; }
    else if (!strcmp(key, kTwkBoneY))     { *of = CatchTweaks_BoneAdd(1);              return 1; }
    else if (!strcmp(key, kTwkBoneZ))     { *of = CatchTweaks_BoneAdd(2);              return 1; }
    else if (!strcmp(key, kTwkBodyFeel))  { *oi = BodyFeel_Enabled() ? 1 : 0;          return 1; }
    else if (!strcmp(key, kTwkBodyAmt))   { *of = BodyFeel_AmountPct();                return 1; }
    else if (!strcmp(key, kTwkBrace))     { *oi = BodyFeel_BraceEnabled() ? 1 : 0;     return 1; }
    else if (!strcmp(key, kTwkArmPow))    { *of = BodyFeel_ArmPct();                   return 1; }
    else if (!strcmp(key, kTwkFallPow))   { *of = BodyFeel_FallPct();                  return 1; }
    else if (!strcmp(key, kTwkReachPow))  { *of = BodyFeel_ReachPct();                 return 1; }
    else if (!strcmp(key, kTwkCarryPow))  { *of = BodyFeel_CarryPct();                 return 1; }
    else if (!strcmp(key, kTwkFlailLen))  { *of = BodyFeel_FlailMs();                  return 1; }
    else if (!strcmp(key, kTwkFlailDel))  { *of = BodyFeel_FlailDelayMs();             return 1; }
    else if (!strcmp(key, kTwkFlailPow))  { *of = BodyFeel_FlailPct();                 return 1; }
    else if (!strcmp(key, kTwkGrabLen))   { *of = BodyFeel_GrabMs();                   return 1; }
    else if (!strcmp(key, kTwkGrabDel))   { *of = BodyFeel_GrabDelayMs();              return 1; }
    else if (!strcmp(key, kTwkGrabPow))   { *of = BodyFeel_GrabPct();                  return 1; }
    else if (!strcmp(key, kTwkPop))       { *oi = PopProbe_SchemeEnabled()  ? 1 : 0;   return 1; }
    else if (!strcmp(key, kTwkPopWin))    { *of = PopProbe_TrickWindowMs();            return 1; }
    else if (!strcmp(key, kTwkPopGate))   { *of = PopProbe_CrouchGatePct();            return 1; }
    else if (!strcmp(key, kTwkPopVisLo))  { *of = PopProbe_CrankVisMinMs();            return 1; }
    else if (!strcmp(key, kTwkPopVisHi))  { *of = PopProbe_CrankVisTimeMs();           return 1; }
    else if (!strcmp(key, kTwkPopVisSm))  { *of = PopProbe_CrankVisSmoothMs();         return 1; }
    return 0;
}
// Every slider's units are chosen so its INTEGER readout is meaningful -- the pause menu's progress
// row prints "%d" (cm, deg/s, deg, percent), unlike the F1 sliders which can format floats.
// The page is SPLIT into categories rather than run as one long list. A guest page does not scroll:
// the engine's scroll math reads the real page's own definitions array, not the one we hand the row
// builder, so a list longer than its ~14-row window is simply cut off at the bottom. Categories cost
// one registration each, keep every page readable at a glance, and grow indefinitely.
static const OmpPageItem2 kTwkRootItems[] = {
    { OMP_ITEM_PAGE, "Pop control",    "Pop control",     "Crouch to any depth and pop from it -- deeper means higher" },
    { OMP_ITEM_PAGE, "Board & tricks", "Board & tricks",  "Scoop and flip speed, board pitch control" },
    { OMP_ITEM_PAGE, "Catch & bail",   "Catch & bail",    "Catch window, catch sound, running out of a bail" },
    { OMP_ITEM_PAGE, "Grinds",         "Grinds",          "Pitch control and pop swing coming out of a grind" },
    { OMP_ITEM_PAGE, "Feet",           "Feet",            "Move your feet with the sticks while you are in the air" },
    { OMP_ITEM_PAGE, "Camera",         "Camera",          "Make the camera's height follow your skater everywhere" },
    { OMP_ITEM_PAGE, "Clothing",       "Clothing",        "Cloth physics on your shirt and trousers" },
    { OMP_ITEM_PAGE, "Physical animation", "Physical animation", "The reactive body and ragdoll bails: bracing, grabbing what hurt, the landing flail" },
    // Kept on the front page deliberately: it resets EVERY Session Tweaks setting, not one category.
    { OMP_ITEM_ACTION, kTwkReset,  "Reset to defaults",   "Restore every Session Tweaks setting to its shipped value" },
};
static const OmpPageItem2 kTwkBoardItems[] = {
    { OMP_ITEM_TOGGLE, kTwkScoop,  "Scoop speed fix",         "Stick speed drives how fast the board scoops" },
    // Shown in TENS of deg/s only here: the pause menu's value field clips at four digits, and this
    // range runs to 3000. The F1 menu and the ini keep real deg/s -- only the display is scaled.
    { OMP_ITEM_SLIDER, kTwkVelMin, "  Slowest at (x10 deg/s)", "Stick speed that gives the slowest scoop",
      nullptr, nullptr, 5.0f, 150.0f, 5.0f },
    { OMP_ITEM_SLIDER, kTwkVelMax, "  Fastest at (x10 deg/s)", "Stick speed that gives the fastest scoop",
      nullptr, nullptr, 20.0f, 300.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkFlip,    "Flip speed from the flick", "How fast you flick sets how fast the board flips" },
    // These bounds MUST cover the shipped defaults (150 / 500), and every value here has to stay
    // UNDER 1000 -- the pause menu's value field clips at four digits.
    { OMP_ITEM_SLIDER, kTwkFlipMin, "  Slowest flip at",      "Flick speed that gives the slowest flip",
      nullptr, nullptr, 20.0f, 400.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkFlipMax, "  Fastest flip at",      "Flick speed that gives the fastest flip",
      nullptr, nullptr, 50.0f, 800.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkPitch,    "Wider board pitch control", "Spreads board pitch over the whole flick instead of its first third" },
    { OMP_ITEM_SLIDER, kTwkPitchAmt, "  Pitch spread (deg)",  "65 = stock; higher means full pitch needs a bigger flick",
      nullptr, nullptr, 65.0f, 89.0f, 1.0f },
};
static const OmpPageItem2 kTwkCatchItems[] = {
    { OMP_ITEM_TOGGLE, kTwkCatch,  "Wider manual catch",      "Widens the catch window while Catch Mode is manual" },
    { OMP_ITEM_SLIDER, kTwkCatchX, "  Catch window (%)",      "200 = twice the stock window",
      nullptr, nullptr, 100.0f, 400.0f, 25.0f },
    { OMP_ITEM_SLIDER, kTwkDsZone, "  Dark slide zone (deg)", "How far from grip-down a dark slide is still reserved",
      nullptr, nullptr, 10.0f, 170.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkClickCat, "Click a stick to catch", "Press a stick instead of flicking; that foot catches. Only while you are on the board" },
    { OMP_ITEM_TOGGLE, kTwkStopFlip, "Catch ends the flip",   "A caught board stops at griptape-up instead of spinning another full flip" },
    { OMP_ITEM_TOGGLE, kTwkAnyRev,  "Foot always attaches",    "A caught board ends its flip flat under your foot, whatever revolution it was on" },
    { OMP_ITEM_TOGGLE, kTwkFootLvl, "  Foot levels the board", "The deck rolls flat in step with the foot coming down on it" },
    { OMP_ITEM_TOGGLE, kTwkLevel,   "Level board on catch",   "Eases the board flat when it hits your foot" },
    { OMP_ITEM_SLIDER, kTwkSndVol,  "Catch sound (%)",        "Our replay-recorded catch sound; 100 = the cue's authored level",
      nullptr, nullptr, 50.0f, 300.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkRunOut, "Run out instead of bail", "Low missed tricks run out on foot (needs manual Catch Mode)" },
    { OMP_ITEM_SLIDER, kTwkDrop,   "  Real bail if drop over (cm)", "Above this drop a missed trick still bails",
      nullptr, nullptr, 50.0f, 600.0f, 25.0f },
};
static const OmpPageItem2 kTwkGrindItems[] = {
    { OMP_ITEM_TOGGLE, kTwkGPitch,    "Pitch control out of grinds", "Board Control pitch works coming off a grind (set Board Control Mode to manual)" },
    { OMP_ITEM_SLIDER, kTwkGPitchAmt, "  Pitch amount (%)",   "How much of your input reaches the board out of a grind",
      nullptr, nullptr, 0.0f, 200.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkGSwing,    "Pop swing out of grinds",   "The board drops its tail before popping out of a grind" },
    { OMP_ITEM_SLIDER, kTwkGSwingAmt, "  Swing amount (%)",   "100 = the trick's full pop swing",
      nullptr, nullptr, 0.0f, 100.0f, 10.0f },
};
static const OmpPageItem2 kTwkFeetItems[] = {
    { OMP_ITEM_TOGGLE, kTwkSteer,    "Mid-trick foot control", "The sticks move your feet in the air; fast flicks are still the catch" },
    { OMP_ITEM_SLIDER, kTwkSteerCm,  "  Reach (cm)",          "How far a fully pushed stick moves that foot. Past the leg's reach the foot stops travelling and buzzes",
      nullptr, nullptr, 2.0f, 80.0f, 2.0f },
    // Lower is quicker AND closer to a flick, so this slider is the discriminator: it is what decides
    // whether a catch flick can drag the foot before the veto catches it.
    { OMP_ITEM_SLIDER, kTwkSteerMs,  "  Response (ms)",       "Stick to full reach; lower is quicker but closer to a flick",
      nullptr, nullptr, 100.0f, 800.0f, 25.0f },
    // The three axis-mapping sliders stay in F1 and the ini only: they are dialled in once and then
    // never touched, and the page has no room for rows nobody adjusts.
    { OMP_ITEM_SLIDER, kTwkSteerTw,  "  Foot twist (deg)",    "Push up and the toe swings forward, pull back and it swings back; 0 = move only",
      nullptr, nullptr, 0.0f, 20.0f, 1.0f },
    { OMP_ITEM_SLIDER, kTwkBone,     "Boned ollie (%)",      "The game's own bone: 100 is stock, 0 removes it, higher shoves the board further",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkBoneX,    "  Bone add X (cm)",    "Added on top of the scaled bone",
      nullptr, nullptr, -100.0f, 100.0f, 2.0f },
    { OMP_ITEM_SLIDER, kTwkBoneY,    "  Bone add Y (cm)",    "Added on top of the scaled bone",
      nullptr, nullptr, -100.0f, 100.0f, 2.0f },
    { OMP_ITEM_SLIDER, kTwkBoneZ,    "  Bone add Z (cm)",    "Added on top of the scaled bone -- raise whichever axis lifts the deck",
      nullptr, nullptr, -100.0f, 100.0f, 2.0f },
};
static const OmpPageItem2 kTwkClothItems[] = {
    { OMP_ITEM_TOGGLE, kTwkCloth,     "Cloth physics",        "Your shirt and trousers move instead of being painted on" },
    { OMP_ITEM_SLIDER, kTwkClothMove, "  Movement (cm)",      "How far the loose part of a garment may swing from the body",
      nullptr, nullptr, 1.0f, 15.0f, 1.0f },
    { OMP_ITEM_SLIDER, kTwkClothCuff, "  Cuff tightness (%)", "How firmly trouser cuffs are held to the ankle; higher shows less gap",
      nullptr, nullptr, 50.0f, 100.0f, 5.0f },
    { OMP_ITEM_SLIDER, kTwkClothHem,  "  Shirt hem lift (mm)","Holds a shirt's bottom edge clear of your trousers instead of clipping through",
      nullptr, nullptr, 0.0f, 30.0f, 1.0f },
    { OMP_ITEM_SLIDER, kTwkClothHemUp,"  Hem lift reach (%)", "How far up the shirt the lift reaches. Low only lifts the bottom edge; higher flares more of the garment",
      nullptr, nullptr, 10.0f, 80.0f, 5.0f },
};
static const OmpPageItem2 kTwkPhysItems[] = {
    { OMP_ITEM_TOGGLE, kTwkBodyFeel, "Reactive body",
      "The body braces at speed, loosens in the air, coils into the crouch and absorbs landings" },
    { OMP_ITEM_SLIDER, kTwkBodyAmt,  "  Body reactivity (%)",
      "Master strength for everything on this page; 0 = stock stiffness everywhere",
      nullptr, nullptr, 0.0f, 200.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkArmPow,   "  Arm reactivity (%)",
      "Balance arms while riding: swing wide in carves, spread in the air and on rails, absorb landings",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
    { OMP_ITEM_TOGGLE, kTwkBrace,    "Bail reactions",
      "Ragdolls brace the fall, grab what hurt and flail on landing instead of going limp" },
    { OMP_ITEM_SLIDER, kTwkFallPow,  "  Fall weight (%)",
      "Extra downward pull on the falling ragdoll; 0 = the game's own gravity only",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkReachPow, "  Mid-air reach (%)",
      "Arms thrown toward the landing while still in the air",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkCarryPow, "  Momentum keep (%)",
      "How much pre-bail speed the ragdoll holds through the fall; 0 leaves whatever the game gives",
      nullptr, nullptr, 0.0f, 150.0f, 5.0f },
    { OMP_ITEM_SLIDER, kTwkFlailDel, "  Flail delay (ms)",
      "How long after the bail starts the legs begin kicking -- in the air, before landing",
      nullptr, nullptr, 0.0f, 2000.0f, 50.0f },
    { OMP_ITEM_SLIDER, kTwkFlailLen, "  Flail time (ms)",
      "How long the legs pedal after touching down before settling into the curl",
      nullptr, nullptr, 0.0f, 4000.0f, 100.0f },
    { OMP_ITEM_SLIDER, kTwkFlailPow, "  Flail strength (%)",
      "How hard the legs kick during the flail",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkGrabDel,  "  Reaction delay (ms)",
      "Beat after touching down before the grab, curl and flail start -- room for the natural scorpion flop",
      nullptr, nullptr, 0.0f, 2000.0f, 50.0f },
    { OMP_ITEM_SLIDER, kTwkGrabLen,  "  Grab time (ms)",
      "How long the hands keep clutching the part that hit the ground",
      nullptr, nullptr, 500.0f, 9900.0f, 250.0f },
    { OMP_ITEM_SLIDER, kTwkGrabPow,  "  Grab strength (%)",
      "How hard the hands reach for the hurt",
      nullptr, nullptr, 0.0f, 300.0f, 10.0f },
};
static const OmpPageItem2 kTwkPopItems[] = {
    { OMP_ITEM_TOGGLE, kTwkPop,       "Pop control scheme",
      "Crouch on one stick and pop with the other: how deep you "
      "go is how high you pop. Mirrored for nollie and switch." },
    { OMP_ITEM_SLIDER, kTwkPopWin,    "  Trick window (ms)",
      "How long after the pop your trick flick may arrive. Longer is more forgiving; running out "
      "pops a plain ollie",
      nullptr, nullptr, 60.0f, 600.0f, 10.0f },
    { OMP_ITEM_SLIDER, kTwkPopGate,   "  Crouch starts at (%)",
      "How far the stick travels before you start crouching. Everything past this point is pop height",
      nullptr, nullptr, 20.0f, 80.0f, 1.0f },
    { OMP_ITEM_SLIDER, kTwkPopVisLo,  "  Shallowest crouch",
      "How low the character sits the moment the crouch takes hold. Raise it if a light pull only "
      "shuffles the back foot",
      nullptr, nullptr, 0.0f, 800.0f, 25.0f },
    { OMP_ITEM_SLIDER, kTwkPopVisHi,  "  Deepest crouch",
      "How low a fully pulled stick sits. Lower it if you bottom out early, raise it if full stick "
      "never reaches a full crouch",
      nullptr, nullptr, 300.0f, 1500.0f, 25.0f },
    { OMP_ITEM_SLIDER, kTwkPopVisSm,  "  Crouch smoothing (ms)",
      "How heavily the body follows the stick. Higher is lazier and smoother, 0 tracks your thumb "
      "exactly",
      nullptr, nullptr, 0.0f, 400.0f, 10.0f },
};
static const OmpPageItem2 kTwkCameraItems[] = {
    { OMP_ITEM_TOGGLE, kTwkCamFollow, "Camera always follows height",
      "The camera tracks your height on every air, not just onto obstacles higher than you" },
    // Named for what the GAME does, so ON is stock -- the module disables drop detection when this
    // is off. Do not "correct" the apparent inversion in camera_height.cpp; the label is the contract.
    { OMP_ITEM_TOGGLE, kTwkCamPitchDrop, "Pitch camera before drop",
      "Stock behaviour: the camera tilts down at the edge of a drop instead of descending with you" },
    { OMP_ITEM_SLIDER, kTwkCamPitch, "Pitch (deg)",
      "Tilts the camera: positive looks up, negative looks down. 0 is the stock camera",
      nullptr, nullptr, -30.0f, 30.0f, 1.0f },
};
// Every page must stay inside the host's cap AND inside the engine's visible window -- the host
// truncates the TAIL, so an over-long page loses its Back row, not the row just added.
static_assert(sizeof(kTwkRootItems)  / sizeof(kTwkRootItems[0])  <= 13 &&
              sizeof(kTwkBoardItems) / sizeof(kTwkBoardItems[0]) <= 13 &&
              sizeof(kTwkCatchItems) / sizeof(kTwkCatchItems[0]) <= 13 &&
              sizeof(kTwkGrindItems) / sizeof(kTwkGrindItems[0]) <= 13 &&
              sizeof(kTwkFeetItems)  / sizeof(kTwkFeetItems[0])  <= 13 &&
              sizeof(kTwkCameraItems) / sizeof(kTwkCameraItems[0]) <= 13 &&
              sizeof(kTwkClothItems)  / sizeof(kTwkClothItems[0])  <= 13 &&
              sizeof(kTwkPopItems)    / sizeof(kTwkPopItems[0])    <= 13 &&
              sizeof(kTwkPhysItems)   / sizeof(kTwkPhysItems[0])   <= 13,
              "A Session Tweaks page exceeds the engine's visible-row window (14 incl. the Back row "
              "the host appends). Split it into another category page rather than raising this.");

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
            // The FRONT page registers FIRST. Child pages are claimed by TITLE at draw time,
            // so order never affects the hierarchy -- but the host's page table has a cap, and
            // if anything is refused it must be a category (one inert row), never the front
            // page: an unclaimed category spills into the pause-menu root (field-hit when the
            // 8th category pushed the front page past the old cap of 8).
            if (regp) {
                if (regp("Session Tweaks", kTwkRootItems,
                         (int)(sizeof(kTwkRootItems) / sizeof(kTwkRootItems[0])),
                         &pageSelect, &pageValue, &pageGet, nullptr, nullptr)) {
                    g_pageRegistered = true;
                    TwkLog("[tweaks] registered the pause-menu page (the front page + 8 category pages)");
                }
                #define TWK_SUBPAGE(title, arr)                     regp(title, arr, (int)(sizeof(arr) / sizeof(arr[0])),                          &pageSelect, &pageValue, &pageGet, nullptr, nullptr)
                TWK_SUBPAGE("Pop control",    kTwkPopItems);
                TWK_SUBPAGE("Board & tricks", kTwkBoardItems);
                TWK_SUBPAGE("Catch & bail",   kTwkCatchItems);
                TWK_SUBPAGE("Grinds",         kTwkGrindItems);
                TWK_SUBPAGE("Feet",           kTwkFeetItems);
                TWK_SUBPAGE("Camera",         kTwkCameraItems);
                TWK_SUBPAGE("Clothing",       kTwkClothItems);
                TWK_SUBPAGE("Physical animation", kTwkPhysItems);
                #undef TWK_SUBPAGE
            }
        }
        if (g_menuRegistered && g_pageRegistered) return true;
    }
    return false;
}
void Tweaks_PumpFrame() {
    RunOut_PumpFrame();              // the armed post-conversion fall watch + facing
    CatchTweaks_PumpFrame();         // per-trick board-rotation watch
    CatchLevel_PumpFrame();          // catch-triggered board leveling
    CatchSound_PumpFrame();
    ClothMerge_PumpFrame();
    ClothSim_PumpFrame();
    FootPlace_PumpFrame();
    FootSteer_PumpFrame();           // liveness only: it applies from inside foot_place's hook
    GrindPop_PumpFrame();            // grind-exit pop records: names resolved and logged out here
    PopProbe_PumpFrame();            // AFTER grind_pop: its drain feeds PopProbe_OnJump first
    BodyFeel_PumpFrame();            // breathes the physical-animation stiffness (after pop_probe: reads its crouch depth)
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
        // Version history is kept with the releases, not in the source. ONE definition -- the log
        // banner used to carry its own copy and silently drifted five versions behind.
        ModVersion = TWK_WIDEN(TWEAKS_VERSION);
        ModDescription = STR("Gameplay fixes: real-stick-sweep scoop speed, wider manual catch window, darkslide-aware catch fix, run out on missed tricks");
        ModAuthors = STR("matsix");
        char dir[MAX_PATH]{};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *(slash + 1) = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sSessionTweaks.log", dir);
        g_log = fopen(path, "w");
        TwkLog("=== SessionTweaks " TWEAKS_VERSION " loading (UE4SS C++ mod) ===");
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
    FlipSpeed_Install();
        CatchTweaks_Install();
        CatchLevel_Install();
        CatchSound_Install();
    ClothMerge_Install();
    ClothSim_Install();
        PitchRange_Install();
        FootPlace_Install();
        FootSteer_Install();
        GrindPop_Install();
        CameraHeight_Install();
        RunOut_Install();
        PopProbe_Install();
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

