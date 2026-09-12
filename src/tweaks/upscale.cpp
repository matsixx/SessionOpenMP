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
// SessionTweaks -- UPSCALING: the render scale, and the seam FSR is delivered through.
//
// Two things. (1) THE RENDER SCALE: r.ScreenPercentage and the engine's own temporal upsampling
// (r.TemporalAA.Upsampling, "TAAU": render at the scale, let the TAA pass rebuild full resolution),
// applied through UKismetSystemLibrary::ExecuteConsoleCommand with the skater as the world context and
// re-asserted every few seconds, because the game's own settings apply re-stamps r.ScreenPercentage
// from its resolution-quality setting whenever the pause menu closes.
//
// (2) THE SEAM. UE 4.27 gives DLSS/FSR2-class plugins one seam: ITemporalUpscaler, an interface
// the post-process chain calls in place of its TAA pass. In this build the chain (AddPostProcessingPasses)
// picks the VIEW FAMILY's interface (FSceneViewFamily +0xb8) when r.TemporalAA.Upscaler != 0 and the AA
// method is TAA; the GTemporalUpscaler global has no readers. The family never frees that pointer (its
// destructor deletes +0xb0 / +0xc0 / +0xc8 only), so one static object of ours is set on every family at
// the top of AddPostProcessingPasses. Our object runs the engine's default upscaler first
// (FDefaultTemporalUpscaler::AddPasses, stateless: its half-resolution output is still wanted
// downstream and its full output is our fallback) and then offers the frame to FSR (upscale_fsr.cpp),
// which replaces the output when it is on and we are in a level. Kill switch: UpscalerProbe=0 leaves
// the engine's own upscaler alone. UpscalerProbeLog=1 brings back the bring-up telemetry: a per-second
// line of everything the FSR dispatch consumes (view rects, display fraction, jitter, the three input
// textures' formats and sizes) plus a health count of calls, installs and faults.
//
// Layouts (Epic PDB, verified against the caller's disassembly): FSceneView +0x0 Family, +0x258
// UnscaledViewRect, +0xd90 bCameraCut byte, +0xd9e bIsSceneCapture, +0x15f0 AntiAliasingMethod, +0x15f4
// PrimaryScreenPercentageMethod; FViewInfo +0x1670 ViewRect, +0x4f9c/+0x4fa0/+0x4fa4 jitter length/index/
// pixels; FSceneViewFamily +0xa4 SecondaryViewFraction, +0xb8 TemporalUpscalerInterface; FRDGTexture +0x28
// Desc { +0x18 flags, +0x1c EPixelFormat, +0x20 FIntPoint extent }; ITemporalUpscaler::FPassInputs { +0 allow
// downsample, +4 format, +8 color, +0x10 depth, +0x18 velocity }; vtable [0] deleting dtor, [1] GetDebugName,
// [2] AddPasses(GraphBuilder&, const FViewInfo&, const FPassInputs&, FRDGTexture** outColor, FIntRect* outRect,
// FRDGTexture** outHalfRes, FIntRect* outHalfRect), [3] GetMinUpsampleResolutionFraction, [4] GetMax...
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tweaks_common.h"
#include "upscale.h"
#include "upscale_fsr.h"
#include "catch_tweaks.h"      // CatchTweaks_Skater(): the world-context object for the console exec
#include "MinHook.h"

// ------------------------------------------------------------------ knobs
static int   g_renderScale = 100;   // RenderScalePct: r.ScreenPercentage (100 = native)
static int   g_probe       = 1;     // UpscalerProbe: install the pass-through interface
static int   g_probeLog    = 0;     // UpscalerProbeLog: bring-up telemetry, off unless something needs diagnosing
static int   g_applyMs     = 5000;  // UpscalerApplyMs: re-assert cadence for the console variables

void Upscale_ReadConfig(const char* buf) {
    g_renderScale = TwkIniInt(buf, "RenderScalePct", 100);
    if (g_renderScale < 50) g_renderScale = 50; if (g_renderScale > 100) g_renderScale = 100;
    g_probe    = TwkIniInt(buf, "UpscalerProbe", 1) ? 1 : 0;
    g_probeLog = TwkIniInt(buf, "UpscalerProbeLog", 0) ? 1 : 0;
    g_applyMs  = TwkIniInt(buf, "UpscalerApplyMs", 5000);
    if (g_applyMs < 500) g_applyMs = 500; if (g_applyMs > 60000) g_applyMs = 60000;
    UpscaleFsr_ReadConfig(buf);
}
void Upscale_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "RenderScalePct",     g_renderScale);
    TwkIniSetInt(buf, cap, "UpscalerProbe",      g_probe);
    TwkIniSetInt(buf, cap, "UpscalerProbeLog",   g_probeLog);
    TwkIniSetInt(buf, cap, "UpscalerApplyMs",    g_applyMs);
    UpscaleFsr_SaveConfig(buf, cap);
}
void Upscale_ResetDefaults() { g_renderScale = 100; UpscaleFsr_ResetDefaults(); }

static bool g_dirtyApply = true;     // apply on the next pump (load, or a menu change)
float Upscale_RenderScalePct()       { return (float)g_renderScale; }
void  Upscale_SetRenderScalePct(float v) {
    int p = (int)(v + 0.5f); if (p < 50) p = 50; if (p > 100) p = 100;
    g_renderScale = p; g_dirtyApply = true; TwkMarkDirty();
}

// ------------------------------------------------------------------ the console
// UKismetSystemLibrary::ExecuteConsoleCommand(const UObject* WorldContext, const FString& Command,
// APlayerController* SpecificPlayer): a null player means the first local controller, failing that
// the engine's Exec. Unique in both exes (Epic 0x2ce2630 / Steam 0x2ca4ea0). GAME THREAD.
typedef void (*ExecConsoleFn)(const void* worldContext, const void* fstring, void* player);
static ExecConsoleFn g_exec = nullptr;
static const char* SIG_EXEC_CONSOLE =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B FA 49 8B D8 48 8B D1 45 33 C0 48 8B 0D ?? ?? ?? ??";
struct FStr { wchar_t* data; int32_t num; int32_t max; };
static bool Exec(void* worldCtx, const char* cmd) {
    if (!g_exec || !worldCtx || !cmd) return false;
    wchar_t w[160]; int n = 0;
    for (; cmd[n] && n < 159; n++) w[n] = (wchar_t)(unsigned char)cmd[n];
    w[n] = 0;
    FStr s = { w, n + 1, n + 1 };
    __try { g_exec(worldCtx, &s, nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_exec = nullptr; TwkLog("[upscale] console exec faulted -- disabled"); return false; }
}

// ------------------------------------------------------------------ the seam probe
// FDefaultTemporalUpscaler::AddPasses -- the engine's own TAA/TAAU, stateless (the class is one vtable
// pointer), so any object may be its `this`. Unique both (Epic 0x1bac430 / Steam 0x1b6dc40).
typedef void (*AddPassesFn)(void* self, void* gb, const uint8_t* view, const uint8_t* inputs,
                            void** outColor, void* outRect, void** outHalf, void* outHalfRect);
static AddPassesFn g_defaultAddPasses = nullptr;
static const char* SIG_DEFAULT_ADDPASSES =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 49 8B F9 49 8B D8 48 8B F2 83 78 04 00";
// ITemporalUpscaler::GetTemporalUpscalerMode -- r.TemporalAA.Upscaler (Epic 0x1bb8d20 / Steam 0x1b7a530)
typedef int (*UpscalerModeFn)();
static UpscalerModeFn g_modeFn = nullptr;
static const char* SIG_UPSCALER_MODE =
    "48 8B 05 ?? ?? ?? ?? 8B 40 04 C3 CC CC CC CC CC 40 53 48 83 EC 20 0F B6 05 ?? ?? ?? ?? A8 02 0F 85 ?? ?? ?? ??";
// AddPostProcessingPasses(FRDGBuilder&, const FViewInfo&, int32 ViewIndex, const FPostProcessingInputs&)
// -- RENDER THREAD. FOUR arguments in this build (the prologue: rdx -> View, r8d spilled as a 32-bit
// view index, r9 -> Inputs, whose first field is read straight into GetSceneTextureParameters). The
// public 4.27 source has three; a three-argument wrapper here dropped r9 and the original crashed
// dereferencing it (field, 3.19.289: fatal in GetSceneTextureParameters, reading 0x41ff). ARITY AND
// SIZE OF EVERY HOOKED FUNCTION COME FROM ITS PROLOGUE, NEVER FROM THE PUBLIC HEADER.
// Unique both (Epic 0x1b2a040 / Steam 0x1aeb850).
typedef void (*AddPostProcessFn)(void* gb, void* view, int viewIndex, void* inputs);
static AddPostProcessFn g_origAddPP = nullptr;
static const char* SIG_ADD_POSTPROCESS =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 58 F9 FF FF 48 81 EC 68 07 00 00 0F 29 70 A8";

static volatile LONG g_calls = 0, g_installs = 0, g_foreign = 0, g_faults = 0;
static char  g_lastLine[400] = {0};      // the render thread's last input line, printed by the pump
static volatile LONG g_lineReady = 0;
static double NowS() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart;
}

static void __fastcall Up_Dtor(void* self, unsigned flags) { (void)self; (void)flags; }   // static object: never freed
static const wchar_t* __fastcall Up_DebugName(void* self) { (void)self; return L"SessionTweaks"; }
static float __fastcall Up_MinFrac(void* self) { (void)self; return 0.5f; }
static float __fastcall Up_MaxFrac(void* self) { (void)self; return 1.0f; }
static void __fastcall Up_AddPasses(void* self, void* gb, const uint8_t* view, const uint8_t* inputs,
                                    void** outColor, void* outRect, void** outHalf, void* outHalfRect) {
    InterlockedIncrement(&g_calls);
    if (g_probeLog && view && inputs) {
        static double lastT = 0.0; static long lastCalls = 0;
        const double t = NowS();
        if (t - lastT >= 1.0) {
            __try {
                const uint8_t* fam = *(const uint8_t* const*)view;
                const int32_t* vr = (const int32_t*)(view + 0x1670);
                const int32_t* ur = (const int32_t*)(view + 0x258);
                const float*   jit = (const float*)(view + 0x4fa4);
                const uint8_t* tex[3] = { *(const uint8_t* const*)(inputs + 0x08),
                                          *(const uint8_t* const*)(inputs + 0x10),
                                          *(const uint8_t* const*)(inputs + 0x18) };
                int fmt[3] = {-1,-1,-1}, ex[3] = {0,0,0}, ey[3] = {0,0,0};
                for (int i = 0; i < 3; i++) if (tex[i]) {
                    fmt[i] = *(const int32_t*)(tex[i] + 0x28 + 0x1c);
                    ex[i]  = *(const int32_t*)(tex[i] + 0x28 + 0x20);
                    ey[i]  = *(const int32_t*)(tex[i] + 0x28 + 0x24);
                }
                snprintf(g_lastLine, sizeof(g_lastLine),
                    "[upscale] probe: %ld calls/s | view %dx%d of %dx%d unscaled, secondary x%.3f | "
                    "jitter (%.3f, %.3f) %d/%d | aa %d spm %d capture %d cut 0x%02x | color fmt %d %dx%d, "
                    "depth fmt %d %dx%d, velocity fmt %d %dx%d | allowDown %d mode %d",
                    g_calls - lastCalls, vr[2]-vr[0], vr[3]-vr[1], ur[2]-ur[0], ur[3]-ur[1],
                    fam ? *(const float*)(fam + 0xa4) : -1.0f, jit[0], jit[1],
                    *(const int32_t*)(view + 0x4fa0), *(const int32_t*)(view + 0x4f9c),
                    *(const int32_t*)(view + 0x15f0), *(const int32_t*)(view + 0x15f4),
                    (int)*(view + 0xd9e), (unsigned)*(view + 0xd90),
                    fmt[0], ex[0], ey[0], fmt[1], ex[1], ey[1], fmt[2], ex[2], ey[2],
                    (int)*inputs, g_modeFn ? g_modeFn() : -1);
                InterlockedExchange(&g_lineReady, 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); g_probeLog = 0; }
            lastT = t; lastCalls = g_calls;
        }
    }
    g_defaultAddPasses(self, gb, view, inputs, outColor, outRect, outHalf, outHalfRect);
    // FSR in place of the engine's result: the engine's half-resolution output stays (downstream
    // wants it), its full-resolution output becomes the fallback and the template for ours.
    UpscaleFsr_AddPasses(gb, view, inputs, outColor, (int32_t*)outRect);
}
static void* g_vtable[5] = { (void*)&Up_Dtor, (void*)&Up_DebugName, (void*)&Up_AddPasses,
                             (void*)&Up_MinFrac, (void*)&Up_MaxFrac };
static struct { void** vt; } g_upscaler = { g_vtable };

// The hook: hand every family our interface before the chain picks one. A family that already
// carries an interface (another plugin's) is left alone and counted.
static void hkAddPostProcessing(void* gb, void* view, int viewIndex, void* inputs) {
    if (g_probe && view) {
        __try {
            uint8_t* fam = *(uint8_t**)view;
            if (fam) {
                void** slot = (void**)(fam + 0xb8);
                if (!*slot) { *slot = &g_upscaler; InterlockedIncrement(&g_installs); }
                else if (*slot != &g_upscaler) InterlockedIncrement(&g_foreign);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_probe = 0; InterlockedIncrement(&g_faults); }
    }
    g_origAddPP(gb, view, viewIndex, inputs);
}

void Upscale_Install() {
    g_exec = (ExecConsoleFn)TwkScanExe(SIG_EXEC_CONSOLE);
    TwkLog("[upscale] console exec %s", g_exec ? "resolved" : "SIG NOT FOUND -- render scale cannot be applied");
    if (!g_probe) { TwkLog("[upscale] upscaler seam off (UpscalerProbe=0) -- the engine keeps its own"); return; }
    g_defaultAddPasses = (AddPassesFn)TwkScanExe(SIG_DEFAULT_ADDPASSES);
    g_modeFn = (UpscalerModeFn)TwkScanExe(SIG_UPSCALER_MODE);
    uint8_t* target = TwkScanExe(SIG_ADD_POSTPROCESS);
    if (!g_defaultAddPasses || !target) {
        TwkLog("[upscale] upscaler seam: %s%s -- off (game updated?)",
               g_defaultAddPasses ? "" : "default upscaler sig NOT FOUND ",
               target ? "" : "AddPostProcessingPasses sig NOT FOUND");
        g_probe = 0; return;
    }
    if (MH_CreateHook(target, (void*)&hkAddPostProcessing, (void**)&g_origAddPP) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        TwkLog("[upscale] upscaler seam: hook failed -- off"); g_probe = 0; return;
    }
    TwkLog("[upscale] upscaler seam armed: AddPostProcessingPasses @ %p, default upscaler @ %p, "
           "r.TemporalAA.Upscaler reader %s", target, (void*)g_defaultAddPasses, g_modeFn ? "resolved" : "missing");
    UpscaleFsr_Install();
}

// Have these console variables been written at all? Once they have, going back to "off" has to
// write ONE more time to undo the override -- stopping quietly would leave the game running at
// whatever was last pushed.
static bool g_scaleOwned = false;

static void Apply(void* sk) {
    char c[64];
    // NOTHING IS TOUCHED WHILE THE FEATURE IS OFF. These are the game's own console variables and
    // the video settings screen writes them too: a player who set their resolution scale there had
    // it silently overwritten every few seconds by this module's default of 100, whether or not they
    // ever opened the Graphics page. So the scale is only pushed while it IS the one in charge --
    // the slider moved off native, or the upscaler running (which is what renders below native in
    // the first place). Turning both back off writes the engine default once and then lets go.
    const bool wantScale = (g_renderScale != 100) || UpscaleFsr_Enabled();
    if (!wantScale && !g_scaleOwned) return;

    // Temporal upsampling is not a choice any more: it is what makes rendering below native produce a
    // full-resolution image, so it follows the thing that needs it rather than being a switch of its
    // own that could be left in the wrong position.
    // IT FOLLOWS THE RENDER SCALE, not just FSR. 3.19.361 tied it to FSR alone, which quietly took
    // temporal upsampling away from anyone running a reduced render scale WITHOUT FSR -- their scale
    // became a plain downscale. That matters doubly now: on DirectX 11 FSR is forced off, and the
    // render-scale slider is the only upscaling left, so it must keep the engine's own upsampler.
    const int g_taau = (UpscaleFsr_Enabled() || g_renderScale != 100) ? 1 : 0;
    const int scale  = wantScale ? g_renderScale : 100;
    snprintf(c, sizeof(c), "r.TemporalAA.Upsampling %d", g_taau);
    const bool a = Exec(sk, c);
    snprintf(c, sizeof(c), "r.ScreenPercentage %d", scale);
    const bool b = Exec(sk, c);
    // FSR wants 8 jitter phases x (display/render)^2; the engine scales r.TemporalAASamples by that
    // ratio itself under temporal upscaling, so the base value is what changes with FSR on/off.
    const int samples = UpscaleFsr_WantedTaaSamples();
    snprintf(c, sizeof(c), "r.TemporalAASamples %d", samples);
    Exec(sk, c);
    // Only claimed once a write actually landed, so a failed exec does not leave a release owed.
    if (a || b) g_scaleOwned = wantScale;

    static int lastScale = -1, lastTaau = -1, lastSamples = -1;
    if (lastScale != scale || lastTaau != g_taau || lastSamples != samples) {
        lastScale = scale; lastTaau = g_taau; lastSamples = samples;
        TwkLog("[upscale] applied: r.TemporalAA.Upsampling %d, r.ScreenPercentage %d, r.TemporalAASamples %d%s%s",
               g_taau, scale, samples, wantScale ? "" : " (released -- the game's own setting stands)",
               (a && b) ? "" : " (exec unavailable)");
    }
}

bool Upscale_Console(const char* cmd) {
    void* sk = CatchTweaks_Skater();
    return sk ? Exec(sk, cmd) : false;
}

void Upscale_PumpFrame() {
    UpscaleFsr_PumpFrame();
    if (g_lineReady && InterlockedExchange(&g_lineReady, 0)) TwkLog("%s", g_lastLine);
    {   // 10 s health line, with the rest of the bring-up telemetry
        static uint64_t lastH = 0; const uint64_t ms = GetTickCount64();
        if (g_probe && g_probeLog && ms - lastH > 10000) {
            lastH = ms;
            TwkLog("[upscale] probe health: %ld calls, %ld family installs, %ld foreign interfaces seen, %ld faults",
                   g_calls, g_installs, g_foreign, g_faults);
        }
    }
    void* sk = CatchTweaks_Skater();
    UpscaleFsr_SetGameplay(sk != nullptr);      // no skater = menu/loading: FSR stays out of it
    if (!sk || !g_exec) return;
    static uint64_t lastApply = 0;
    const uint64_t ms = GetTickCount64();
    if (g_dirtyApply || ms - lastApply > (uint64_t)g_applyMs) { lastApply = ms; g_dirtyApply = false; Apply(sk); }
}
