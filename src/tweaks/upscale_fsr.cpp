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
// SessionTweaks -- FSR UPSCALING: AMD FidelityFX Super Resolution (FSR 4 on
// RDNA4 through the driver's provider, FSR 3.1.x elsewhere) in place of the engine's TAA(U), through
// the temporal-upscaler seam upscale.cpp owns.
//
// HOW IT SITS IN THE FRAME. The seam hands us the render-resolution scene colour, depth and velocity
// as render-graph (RDG) textures and wants a display-resolution colour back. The engine's own
// upscaler is run FIRST (its half-resolution output is still needed downstream, and its full output
// is our fallback and the template for ours), then three passes of ours go on the graph:
//   copy   TAA output -> our output   (the fallback image; and the pass whose PARAMETER LAYOUT --
//          two texture-access members -- we borrow for the passes below: this build's RDG member-type
//          ids differ from the public 4.27 headers, so nothing here hand-builds a layout)
//   pass 1 declares velocity + depth (compute read): a compute shader compiled at load writes our
//          own motion-vector and depth textures. UE's velocity buffer only carries objects that moved;
//          camera motion is reconstructed from depth with clip-to-previous-clip, and the depth copy
//          means FSR never reads an RDG texture pass 2 does not declare (split barriers).
//   pass 2 declares colour (compute read) + our output (compute UAV): the FSR dispatch.
// A pass's Execute runs on the render thread; the native D3D12 work is queued as an RHI command
// (or run inline in bypass mode) so it records into the RHI thread's live command list. After it,
// FD3D12StateCacheBase::DirtyStateForNewCommandList makes the engine re-bind everything.
//
// SAFETY. Every ffx call is behind an SEH shim (a driver access-violation would otherwise take the
// game down -- field lesson from the SPT bridge); any failure at any stage sets a permanent fallback
// for the session (TAA stays, the stage is named once in the log); the FSR 4 provider is preferred
// on RDNA4 and a crash in it drops to the 3.1.x provider automatically.
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "tweaks_common.h"
#include "upscale_fsr.h"
#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

// ------------------------------------------------------------------ knobs
static int   g_on          = 0;      // Upscaler: 0 = the engine's TAA/TAAU, 1 = FSR
static int   g_sharpPct    = 50;     // FsrSharpnessPct: RCAS after the upscale (FSR 4 is soft by design)
static int   g_preferFsr4  = 1;      // FsrPreferFsr4: take the 4.x provider when the GPU/driver offer it
static int   g_flipJitX    = 0, g_flipJitY = 0;    // FsrJitterFlipX/Y: A/B knobs for the jitter convention
static int   g_flipMvX     = 0, g_flipMvY  = 0;    // FsrMvFlipX/Y: the same for the motion vectors
static int   g_dynSign     = 1;      // FsrDynamicMvSign: 1 or -1 on decoded object velocities
static int   g_debug       = 0;      // FsrDebug: FSR's own debug view
static int   g_taaSamplesFsr = 8;    // FsrTaaSamples: r.TemporalAASamples while FSR is on (x ratio^2 by the engine)
static int   g_taaSamplesOff = 4;    // FsrTaaSamplesRestore: what to put back when it is off
static int   g_log         = 1;      // FsrLog: the 1/s status line

// FSR needs DirectX 12: its whole native path is D3D12 command lists. On a -dx11 launch it would read
// a D3D11 context through those layouts, which is a crash -- so it is not merely hidden, it is refused
// at every door: the ini cannot turn it on, the menu cannot turn it on, and the pass never runs.
bool UpscaleFsr_Available() { return !Twk_GraphicsIsD3D11(); }
static void SayUnavailable() {
    static bool said = false;
    if (said) return;
    said = true;
    TwkLog("[fsr] the game is running DirectX 11 -- FSR is switched OFF and cannot be turned on "
           "(it builds DirectX 12 commands directly, and enabling it here crashes the game). "
           "Remove -dx11 from the launch options to use it.");
}

void UpscaleFsr_ReadConfig(const char* buf) {
    g_on         = TwkIniInt(buf, "Upscaler", 0) ? 1 : 0;
    if (g_on && !UpscaleFsr_Available()) { g_on = 0; SayUnavailable(); }
    g_sharpPct   = TwkIniInt(buf, "FsrSharpnessPct", 50);
    if (g_sharpPct < 0) g_sharpPct = 0; if (g_sharpPct > 100) g_sharpPct = 100;
    g_preferFsr4 = TwkIniInt(buf, "FsrPreferFsr4", 1) ? 1 : 0;
    g_flipJitX   = TwkIniInt(buf, "FsrJitterFlipX", 0) ? 1 : 0;
    g_flipJitY   = TwkIniInt(buf, "FsrJitterFlipY", 0) ? 1 : 0;
    g_flipMvX    = TwkIniInt(buf, "FsrMvFlipX", 0) ? 1 : 0;
    g_flipMvY    = TwkIniInt(buf, "FsrMvFlipY", 0) ? 1 : 0;
    g_dynSign    = TwkIniInt(buf, "FsrDynamicMvSign", 1) < 0 ? -1 : 1;
    g_debug      = TwkIniInt(buf, "FsrDebug", 0) ? 1 : 0;
    g_taaSamplesFsr = TwkIniInt(buf, "FsrTaaSamples", 8);
    if (g_taaSamplesFsr < 1) g_taaSamplesFsr = 1; if (g_taaSamplesFsr > 64) g_taaSamplesFsr = 64;
    g_taaSamplesOff = TwkIniInt(buf, "FsrTaaSamplesRestore", 4);
    if (g_taaSamplesOff < 1) g_taaSamplesOff = 1; if (g_taaSamplesOff > 64) g_taaSamplesOff = 64;
    g_log        = TwkIniInt(buf, "FsrLog", 1) ? 1 : 0;
}
void UpscaleFsr_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "Upscaler",            g_on);
    TwkIniSetInt(buf, cap, "FsrSharpnessPct",     g_sharpPct);
    TwkIniSetInt(buf, cap, "FsrPreferFsr4",       g_preferFsr4);
    TwkIniSetInt(buf, cap, "FsrJitterFlipX",      g_flipJitX);
    TwkIniSetInt(buf, cap, "FsrJitterFlipY",      g_flipJitY);
    TwkIniSetInt(buf, cap, "FsrMvFlipX",          g_flipMvX);
    TwkIniSetInt(buf, cap, "FsrMvFlipY",          g_flipMvY);
    TwkIniSetInt(buf, cap, "FsrDynamicMvSign",    g_dynSign);
    TwkIniSetInt(buf, cap, "FsrDebug",            g_debug);
    TwkIniSetInt(buf, cap, "FsrTaaSamples",       g_taaSamplesFsr);
    TwkIniSetInt(buf, cap, "FsrTaaSamplesRestore", g_taaSamplesOff);
    TwkIniSetInt(buf, cap, "FsrLog",              g_log);
}
void UpscaleFsr_ResetDefaults() { g_on = 0; g_sharpPct = 50; g_preferFsr4 = 1; }
bool  UpscaleFsr_Enabled()            { return g_on != 0; }
void  UpscaleFsr_SetEnabled(bool o)   {
    if (o && !UpscaleFsr_Available()) { SayUnavailable(); g_on = 0; TwkMarkDirty(); return; }
    g_on = o ? 1 : 0; TwkMarkDirty();
}
float UpscaleFsr_SharpnessPct()       { return (float)g_sharpPct; }
void  UpscaleFsr_SetSharpnessPct(float v) { int p = (int)(v + 0.5f); if (p < 0) p = 0; if (p > 100) p = 100; g_sharpPct = p; TwkMarkDirty(); }
bool  UpscaleFsr_PreferFsr4()         { return g_preferFsr4 != 0; }
void  UpscaleFsr_SetPreferFsr4(bool o){ g_preferFsr4 = o ? 1 : 0; TwkMarkDirty(); }

// ------------------------------------------------------------------ the engine (Epic PDB; sigs unique both)
enum {
    // FSceneView / FViewInfo
    SV_FAMILY = 0x0, SV_UNSCALED_RECT = 0x258, SV_VIEW_MATRICES = 0x280, SV_CAMERA_CUT = 0xd90,
    SV_SCENE_CAPTURE = 0xd9e, VI_VIEW_RECT = 0x1670, VI_JITTER_LEN = 0x4f9c, VI_JITTER_IDX = 0x4fa0,
    VI_JITTER = 0x4fa4, VI_PREV_VIEW = 0x4fc0, PV_VIEW_MATRICES = 0x10, VI_PRE_EXPOSURE = 0x56a4,
    VM_PROJ = 0x0, VM_PROJ_NOAA = 0x40, VM_VIEW = 0xc0, VM_INV_VIEW = 0x100,
    FAM_DELTA_TIME = 0x54,
    // RDG
    RT_RHI = 0x10, RT_DESC = 0x28, RTD_FLAGS = 0x18, RTD_FORMAT = 0x1c, RTD_EXTENT = 0x20, RTD_SIZE = 48,
    GB_RHICMDLIST = 0x0, GB_PASSES = 0x68,
    PASS_SIZE = 240, PASS_PARAMS = 0x10, PASS_LAYOUT = 0x18, PASS_FLAGS = 0x20,
    LAYOUT_GRAPH_TEXTURES = 0x30,
    ACCESS_SRV_COMPUTE = 0x10, ACCESS_UAV_COMPUTE = 0x200,   // ERHIAccess (CopySrc 0x40 / CopyDest 0x1000 calibrated)
    PASSFLAG_COMPUTE = 2, PASSFLAG_NEVER_CULL = 16,   // NeverCull: pass 1 declares only reads and would be culled
    // RHI command list + D3D12
    CL_ROOT = 0x0, CL_LINK = 0x8, CL_NUM = 0x14, CL_CONTEXT = 0x20,
    CTX_CMDLIST_HANDLE = 0x1c8, CLH_DATA = 0x8, CLD_LIST = 0x30, CTX_STATE_CACHE = 0x250,
    // FRHITexture vtable slot 7 = GetNativeResource() (D3D12Texture.h:313 in the 2D class; the same
    // slot in every texture class). NOT a fixed field: the "+0x98" 3.19.291-293 read was the D3D11
    // texture's getter, and the D3D12 getters walk a resource-location chain that differs between the
    // 2D and array/cube instantiations.
    RHITEX_VT_GET_NATIVE = 7,
};
struct FIntPointLocal { int32_t x, y; };
struct TextureAccess { void* texture; uint32_t access; uint32_t pad; };
struct CopyInfo { int32_t size[3], srcPos[3], dstPos[3], srcSlice, dstSlice, numSlices, srcMip, dstMip, numMips; };

typedef void* (*RdgCreateTextureFn)(void* gb, const void* desc, const wchar_t* name, uint8_t flags);
typedef void* (*RdgSetupPassFn)(void* gb, void* pass);
typedef void* (*RdgPassCtorFn)(void* pass, const void* name, const void* paramStruct, uint8_t flags);
typedef void  (*RdgCopyPassFn)(void* gb, void* src, void* dst, const CopyInfo* info);
typedef void  (*D3D12DirtyStateFn)(void* stateCache);
// TRDGHandleRegistry<FRDGPass>::Insert(registry, pass): Array.Add(pass); pass->Handle = Num - 1. The
// engine's AddPass calls it BEFORE SetupPass, which reads the handle at entry and stores it into the
// pass's barrier/fork fields and indexes the registry with it -- without it the pass ran through with
// the null handle 0xffff and faulted (field, 3.19.291). The four registries share the same code but
// for the handle offset (0x22 for a pass), so the signature runs to that byte.
typedef void  (*RdgInsertPassFn)(void* registry, void* pass);
static RdgCreateTextureFn g_rdgCreateTexture = nullptr;
static RdgSetupPassFn     g_rdgSetupPass = nullptr;
static RdgPassCtorFn      g_rdgPassCtor = nullptr;
static RdgCopyPassFn      g_rdgCopyPass = nullptr;
static D3D12DirtyStateFn  g_d3dDirtyState = nullptr;
static RdgInsertPassFn    g_rdgInsertPass = nullptr;
static const uint8_t*     g_bypassFlag = nullptr;    // GRHICommandList.bLatchedBypass
static const char* SIG_RDG_CREATE_TEXTURE =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 0F 10 02 48 8B C2";
static const char* SIG_RDG_SETUP_PASS =
    "48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC 98 00 00 00 0F B6 42 21";
static const char* SIG_RDG_PASS_CTOR =
    "41 0F 10 00 48 8D 05 ?? ?? ?? ?? 44 88 49 20 48 89 01 41 F6 C1 04 0F 11 41 10 0F 95 C0 48 C7 41 28 FF FF FF FF";
static const char* SIG_RDG_COPY_PASS =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00 4D 8B F9";
static const char* SIG_RDG_INSERT_PASS =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 79 08 48 8B F2 48 8B D9 8D 47 01 89 41 08 3B 41 0C 7E 07 8B D7 E8 ?? ?? ?? ?? 48 8B 03 48 89 34 F8 0F B7 43 08 48 8B 5C 24 30 66 FF C8 66 89 46 22 48 8B 74 24 38 48 83 C4 20 5F C3";
static const char* SIG_D3D12_DIRTY_STATE =
    "33 D2 C6 81 B8 2F 00 00 01 C6 81 E8 07 00 00 01 C6 41 48 01 C6 41 36 01 48 89 91 50 06 00 00 39 91 EC 05 00 00";
// `dynamic initializer for GRHICommandList`: its first `lea rcx, [rip+..]` targets the executor's
// CommandListImmediate member (+0x10); the executor's first byte is bLatchedBypass.
static const char* SIG_RHI_EXECUTOR_INIT =
    "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? BB 01 00 00 00 8B D3 D3 E2 48 8D 0D ?? ?? ?? ?? FF CA E8 ?? ?? ?? ??";

static bool g_engineOk = false;
static char g_failStage[96] = {0};     // the first stage that failed this session (perma-fallback)
static void Fail(const char* stage) {
    if (g_failStage[0]) return;
    snprintf(g_failStage, sizeof(g_failStage), "%s", stage);
    TwkLog("[fsr] DISABLED for this session: %s -- the engine's upscaler carries on", stage);
}
static bool ResolveEngine() {
    g_rdgCreateTexture = (RdgCreateTextureFn)TwkScanExe(SIG_RDG_CREATE_TEXTURE);
    g_rdgSetupPass     = (RdgSetupPassFn)TwkScanExe(SIG_RDG_SETUP_PASS);
    g_rdgPassCtor      = (RdgPassCtorFn)TwkScanExe(SIG_RDG_PASS_CTOR);
    g_rdgCopyPass      = (RdgCopyPassFn)TwkScanExe(SIG_RDG_COPY_PASS);
    g_d3dDirtyState    = (D3D12DirtyStateFn)TwkScanExe(SIG_D3D12_DIRTY_STATE);
    g_rdgInsertPass    = (RdgInsertPassFn)TwkScanExe(SIG_RDG_INSERT_PASS);
    const uint8_t* init = TwkScanExe(SIG_RHI_EXECUTOR_INIT);
    if (init) {
        for (int i = 0; i < 0x30; i++) {
            if (init[i] == 0x48 && init[i+1] == 0x8D && init[i+2] == 0x0D) {   // lea rcx, [rip+disp32]
                const int32_t disp = *(const int32_t*)(init + i + 3);
                g_bypassFlag = init + i + 7 + disp - 0x10;
                break;
            }
        }
    }
    TwkLog("[fsr] engine: CreateTexture %d SetupPass %d PassCtor %d InsertPass %d CopyPass %d DirtyState %d bypass-flag %d",
           g_rdgCreateTexture != nullptr, g_rdgSetupPass != nullptr, g_rdgPassCtor != nullptr, g_rdgInsertPass != nullptr,
           g_rdgCopyPass != nullptr, g_d3dDirtyState != nullptr, g_bypassFlag != nullptr);
    return g_rdgCreateTexture && g_rdgSetupPass && g_rdgPassCtor && g_rdgInsertPass && g_rdgCopyPass &&
           g_d3dDirtyState && g_bypassFlag;
}

// ------------------------------------------------------------------ the ffx runtime (RHI thread)
static PfnFfxCreateContext  g_ffxCreate = nullptr;
static PfnFfxDestroyContext g_ffxDestroy = nullptr;
static PfnFfxQuery          g_ffxQuery = nullptr;
static PfnFfxDispatch       g_ffxDispatch = nullptr;
static HMODULE g_ffxLoader = nullptr, g_ffxUpscaler = nullptr, g_dxil = nullptr;
static int  g_ffxLoadState = 0;              // 0 untried, 1 ok, -1 failed
static const ffxReturnCode_t FFX_SEH_CRASH = 0x0BADC0DE;
static ffxReturnCode_t SafeCreate(ffxContext* c, ffxCreateContextDescHeader* d) {
    __try { return g_ffxCreate(c, d, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static ffxReturnCode_t SafeQuery(ffxContext* c, ffxQueryDescHeader* d) {
    __try { return g_ffxQuery(c, d); } __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static ffxReturnCode_t SafeDispatch(ffxContext* c, ffxDispatchDescHeader* d) {
    __try { return g_ffxDispatch(c, d); } __except (EXCEPTION_EXECUTE_HANDLER) { return FFX_SEH_CRASH; }
}
static void SafeDestroy(ffxContext* c) {
    if (!*c) return;
    __try { g_ffxDestroy(c, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    *c = nullptr;
}
static void FfxMessage(uint32_t type, const wchar_t* msg) {
    char b[240]; int k = 0;
    for (; msg && msg[k] && k < 239; k++) b[k] = (char)(msg[k] < 128 ? msg[k] : '?');
    b[k] = 0;
    TwkLog("[fsr] ffx %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", b);
}
// The AMD runtime lives beside this DLL in fsr\: the upscaler provider is preloaded by full path so
// the loader's by-name LoadLibrary resolves to it wherever the mod folder is; dxil.dll likewise for
// the FSR 4 shaders' validation.
static bool LoadFfx() {
    if (g_ffxLoadState) return g_ffxLoadState > 0;
    g_ffxLoadState = -1;
    wchar_t dir[MAX_PATH]; HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&LoadFfx, &self) || !GetModuleFileNameW(self, dir, MAX_PATH)) {
        TwkLog("[fsr] cannot locate the mod folder"); return false;
    }
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) slash[1] = 0;
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%sfsr\\dxil.dll", dir);                          g_dxil = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    swprintf_s(p, L"%sfsr\\amd_fidelityfx_upscaler_dx12.dll", dir);  g_ffxUpscaler = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    swprintf_s(p, L"%sfsr\\amd_fidelityfx_loader_dx12.dll", dir);    g_ffxLoader = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_ffxLoader) { TwkLog("[fsr] amd_fidelityfx_loader_dx12.dll not found in the mod's fsr\\ folder (err %lu)", GetLastError()); return false; }
    HMODULE from = g_ffxLoader;
    for (int pass = 0; pass < 2; pass++) {
        g_ffxCreate  = (PfnFfxCreateContext)GetProcAddress(from, "ffxCreateContext");
        g_ffxDestroy = (PfnFfxDestroyContext)GetProcAddress(from, "ffxDestroyContext");
        g_ffxQuery   = (PfnFfxQuery)GetProcAddress(from, "ffxQuery");
        g_ffxDispatch= (PfnFfxDispatch)GetProcAddress(from, "ffxDispatch");
        if (g_ffxCreate && g_ffxDestroy && g_ffxQuery && g_ffxDispatch) break;
        if (!g_ffxUpscaler) break;
        from = g_ffxUpscaler;
    }
    if (!(g_ffxCreate && g_ffxDestroy && g_ffxQuery && g_ffxDispatch)) { TwkLog("[fsr] ffx-api entry points missing"); return false; }
    TwkLog("[fsr] ffx-api loaded (upscaler dll %s, dxil %s)", g_ffxUpscaler ? "preloaded" : "NOT preloaded", g_dxil ? "ok" : "absent");
    g_ffxLoadState = 1;
    return true;
}

// Provider selection (the SPT bridge's rule): enumerate once, keep the highest 4.x and the highest
// 3.x; a context is ALWAYS created with an explicit id -- the ffx default is the highest, i.e. FSR 4,
// which is exactly what a "prefer 3.1" setting must not get.
static uint64_t g_verFsr4 = 0, g_ver3x = 0;
static char g_verName4[48] = {0}, g_verName3[48] = {0};
static bool g_versionsQueried = false, g_force3x = false, g_chosenIsFsr4 = false;
static void QueryVersions(ID3D12Device* dev) {
    if (g_versionsQueried) return;
    g_versionsQueried = true;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = dev;
    uint64_t count = 0; q.outputCount = &count;
    ffxReturnCode_t rc = SafeQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK || count == 0 || count > 16) { TwkLog("[fsr] version query failed (rc 0x%X, %llu)", rc, (unsigned long long)count); return; }
    uint64_t ids[16] = {}; const char* names[16] = {};
    q.versionIds = ids; q.versionNames = names;
    rc = SafeQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) { TwkLog("[fsr] version name query failed (rc 0x%X)", rc); return; }
    for (uint64_t i = 0; i < count; i++) {
        const char* nm = names[i] ? names[i] : "?";
        TwkLog("[fsr] provider available: %s (id 0x%016llX)", nm, (unsigned long long)ids[i]);
        const char* d = nm; while (*d && (*d < '0' || *d > '9')) d++;
        if (d[0] == '4' && d[1] == '.' && (!g_verFsr4 || strcmp(nm, g_verName4) > 0)) { g_verFsr4 = ids[i]; snprintf(g_verName4, sizeof(g_verName4), "%s", nm); }
        if (d[0] == '3' && d[1] == '.' && (!g_ver3x  || strcmp(nm, g_verName3) > 0)) { g_ver3x  = ids[i]; snprintf(g_verName3, sizeof(g_verName3), "%s", nm); }
    }
}

// ------------------------------------------------------------------ our D3D12 objects (RHI thread)
// The motion-vector pass: a compute shader compiled at first use (d3dcompiler_47, cs_5_0). UE's
// velocity texel encodes (current - previous) in NDC as 16-bit unorm about 32767/65535, and a texel
// that was never written stays 0 -- that is the engine's own "static, reproject from depth" test.
// Static pixels get clip -> previous clip through the matrix built on the render thread. Output is
// the FSR convention: pixels, previous minus current. The depth copy exists so FSR reads nothing
// pass 2 does not declare.
static const char* kMvShader =
"cbuffer CB : register(b0) { row_major float4x4 ClipToPrevClip; float2 RenderSize; float2 InvRenderSize; float4 Flags; };\n"
"Texture2D<float4> Velocity : register(t0);\n"
"Texture2D<float>  Depth    : register(t1);\n"
"RWTexture2D<float2> OutMv   : register(u0);\n"
"RWTexture2D<float>  OutDepth: register(u1);\n"
"[numthreads(8,8,1)]\n"
"void main(uint3 id : SV_DispatchThreadID) {\n"
"  if (id.x >= (uint)RenderSize.x || id.y >= (uint)RenderSize.y) return;\n"
"  float z = Depth[id.xy];\n"
"  float4 enc = Velocity[id.xy];\n"
"  float2 mv;\n"
"  if (enc.x > 0.0) {\n"
"    float2 v = (enc.xy - 32767.0/65535.0) / (0.499*0.5);\n"
"    mv = float2(-v.x * 0.5 * RenderSize.x, v.y * 0.5 * RenderSize.y) * Flags.x;\n"
"  } else {\n"
"    float2 uv = (float2(id.xy) + 0.5) * InvRenderSize;\n"
"    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
"    float4 prev = mul(float4(ndc, z, 1.0), ClipToPrevClip);\n"
"    float2 pndc = (abs(prev.w) > 1e-6) ? prev.xy / prev.w : ndc;\n"
"    float2 d = pndc - ndc;\n"
"    mv = float2(d.x * 0.5 * RenderSize.x, -d.y * 0.5 * RenderSize.y);\n"
"  }\n"
"  OutMv[id.xy] = mv * Flags.yz;\n"
"  OutDepth[id.xy] = z;\n"
"}\n";
struct MvConstants { float clipToPrev[16]; float renderW, renderH, invW, invH; float flags[4]; };

static ID3D12Device*         g_dev = nullptr;
static ID3D12RootSignature*  g_rootSig = nullptr;
static ID3D12PipelineState*  g_pso = nullptr;
static ID3D12DescriptorHeap* g_heap = nullptr;        // shader-visible: 4 slots x 4 descriptors
static UINT                  g_descSize = 0;
static ID3D12Resource*       g_cbuf = nullptr;        // upload ring, 4 x 256 B, persistently mapped
static uint8_t*              g_cbufMap = nullptr;
static ID3D12Resource*       g_mvTex = nullptr;       // R16G16_FLOAT
static ID3D12Resource*       g_depthTex = nullptr;    // R32_FLOAT
static UINT   g_ownW = 0, g_ownH = 0;
static bool   g_ownReadable = false;                  // false: UNORDERED_ACCESS, true: NON_PIXEL_SHADER_RESOURCE
static int    g_pipelineState = 0;                    // 0 untried, 1 ok, -1 failed
static UINT   g_slot = 0;

typedef HRESULT (WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI* SerializeRootSigFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

static bool EnsurePipeline() {
    if (g_pipelineState) return g_pipelineState > 0;
    g_pipelineState = -1;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    D3DCompileFn compile = dc ? (D3DCompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    SerializeRootSigFn serialize = d3d12 ? (SerializeRootSigFn)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
    if (!compile || !serialize) { Fail("d3dcompiler_47 / D3D12SerializeRootSignature unavailable"); return false; }
    ID3DBlob* cs = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = compile(kMvShader, strlen(kMvShader), "mv.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr) || !cs) {
        TwkLog("[fsr] motion-vector shader compile failed 0x%08lX: %s", hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        Fail("shader compile"); return false;
    }
    if (err) err->Release();
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 0; ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 2; ranges[1].BaseShaderRegister = 0; ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor.ShaderRegister = 0; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable.NumDescriptorRanges = 2; params[1].DescriptorTable.pDescriptorRanges = ranges; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 2; rs.pParameters = params;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    hr = serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (sigErr) sigErr->Release();
    if (FAILED(hr) || !sig) { cs->Release(); Fail("root signature serialize"); return false; }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
    sig->Release();
    if (FAILED(hr)) { cs->Release(); Fail("root signature create"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rootSig; pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
    hr = g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    cs->Release();
    if (FAILED(hr)) { Fail("compute PSO create"); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 16; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { Fail("descriptor heap create"); return false; }
    g_descSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 4 * 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cbuf)))) { Fail("constant buffer create"); return false; }
    D3D12_RANGE none = {0, 0};
    if (FAILED(g_cbuf->Map(0, &none, (void**)&g_cbufMap))) { Fail("constant buffer map"); return false; }
    g_pipelineState = 1;
    TwkLog("[fsr] motion-vector pipeline ready");
    return true;
}
static bool EnsureOwnTextures(UINT w, UINT h) {
    if (g_mvTex && g_ownW == w && g_ownH == h) return true;
    if (g_mvTex) { g_mvTex->Release(); g_mvTex = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_mvTex)))) { Fail("motion-vector texture create"); return false; }
    td.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_depthTex)))) { Fail("depth copy texture create"); return false; }
    g_ownW = w; g_ownH = h; g_ownReadable = false;
    TwkLog("[fsr] motion-vector + depth textures %ux%u", w, h);
    return true;
}
static void OwnBarrier(ID3D12GraphicsCommandList* cl, bool toReadable) {
    if (g_ownReadable == toReadable) return;
    D3D12_RESOURCE_BARRIER b[2] = {};
    for (int i = 0; i < 2; i++) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = i ? g_depthTex : g_mvTex;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = toReadable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[i].Transition.StateAfter  = toReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    cl->ResourceBarrier(2, b);
    g_ownReadable = toReadable;
}

// ------------------------------------------------------------------ the frame record + the graph (render thread)
struct Frame {
    void* rdgColor; void* rdgDepth; void* rdgVelocity; void* rdgOut;      // set in AddPasses
    ID3D12Resource* color; ID3D12Resource* depth; ID3D12Resource* velocity; ID3D12Resource* out;  // resolved in Execute
    uint32_t renderW, renderH, outW, outH;
    float jitterX, jitterY, preExposure, dtMs, fovY, camNear;
    float clipToPrev[16];
    bool reset;
};
// A record is filled on the render thread and consumed later on the RHI thread, so a slot must never be
// reused while the RHI thread still owns it: reuse would overwrite the AddRef'd native textures (the RHI
// thread then releases the replacements twice, freeing a resource under the GPU) and stomp the frame the
// dispatch reads. Hence 16 slots, each with a busy flag; a frame that finds its slot still in flight
// skips FSR entirely, which is free because the engine's own upscaled output is already sitting in
// outColor. This was written to explain the main-menu crash and did NOT fix it (3.19.299, identical
// stack afterwards) -- keep it as the correctness it is, but the menu crash has another cause.
enum { FRAME_COUNT = 16, FRAME_MASK = FRAME_COUNT - 1 };
static Frame    g_frames[FRAME_COUNT];
static volatile LONG g_frameBusy[FRAME_COUNT];
static uint32_t g_frameSerial = 0;
static volatile LONG g_inGame = 0;     // set from the game thread: a skater exists, i.e. we are in a level
static volatile LONGLONG g_beat = 0;   // ...and WHEN it last said so: a latch gets stuck, a heartbeat cannot
static const uint64_t kBeatStaleMs = 300;
static volatile LONG g_outstanding = 0;// native texture references we hold (AddRef'd, not yet released)
static volatile LONG g_skips = 0;      // frames that found their slot still in flight
static LONG     g_skipRun = 0;         // consecutive skips (render thread only)
// The parameter block MUST be 16-byte aligned: the render graph's per-texture setup reads a
// texture-access member with `movaps` (RenderGraphBuilder.cpp:138, case UBMT_RDG_TEXTURE_ACCESS),
// which faults on an unaligned address -- the engine allocates its parameter structs aligned; a
// block at +248 of an 8-aligned object was the 3.19.292 "pass 1" exception.
struct alignas(16) OurPass { uint8_t engine[PASS_SIZE]; alignas(16) TextureAccess params[2]; int kind; uint32_t frame; uint8_t pad[8]; };
static_assert(sizeof(OurPass) % 16 == 0, "pass ring elements must keep the parameter block aligned");
static const char* g_passStage = "";
static OurPass  g_passes[8];
static uint32_t g_passSerial = 0;
static void*    g_layout = nullptr;         // the copy pass's FRHIUniformBufferLayout (2 texture-access members)
static void*    g_passVtable[8] = {};       // [0] no-op dtor, [1] Execute, the rest the engine's own
static bool     g_vtableReady = false;
static volatile LONG g_dispatches = 0;

static void __fastcall Pass_Dtor(void* self, unsigned flags) { (void)self; (void)flags; }   // ring storage, never freed
static void __fastcall Pass_Execute(OurPass* self, void* rhiCmdList);

// row-vector UE matrices: r = a * b
static void MatMul(const float* a, const float* b, float* r) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0.0f; for (int k = 0; k < 4; k++) s += a[i*4+k] * b[k*4+j]; r[i*4+j] = s;
    }
}
static bool MatInv(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return true;
}

static OurPass* NewPass(void* gb, int kind, uint32_t frame, void* texA, uint32_t accA, void* texB, uint32_t accB) {
    OurPass* p = &g_passes[g_passSerial++ & 7];
    memset(p, 0, sizeof(*p));
    p->params[0].texture = texA; p->params[0].access = accA;
    p->params[1].texture = texB; p->params[1].access = accB;
    struct { void* contents; void* layout; } ps = { p->params, g_layout };
    uint8_t name[16] = {};
    g_passStage = "constructor";
    g_rdgPassCtor(p, name, &ps, PASSFLAG_COMPUTE | PASSFLAG_NEVER_CULL);
    if (!g_vtableReady) {
        void** ev = *(void***)p;
        for (int i = 0; i < 8; i++) g_passVtable[i] = ev[i];
        g_passVtable[0] = (void*)&Pass_Dtor;
        g_passVtable[1] = (void*)&Pass_Execute;
        g_vtableReady = true;
    }
    *(void***)p = g_passVtable;
    p->kind = kind; p->frame = frame;
    g_passStage = "registry insert";
    g_rdgInsertPass((uint8_t*)gb + GB_PASSES, p);      // handle first, exactly as AddPass does
    g_passStage = "SetupPass";
    void* r = g_rdgSetupPass(gb, p);
    g_passStage = "";
    return r ? p : nullptr;
}

void UpscaleFsr_SetGameplay(bool in) {
    const LONG v = in ? 1 : 0;
    const uint64_t now = GetTickCount64();
    // Report our own absence. This runs once a frame, so a long gap between two calls means the game
    // thread was somewhere else -- a level load -- and the render thread was drawing without us.
    const uint64_t was = (uint64_t)InterlockedExchange64(&g_beat, (LONGLONG)now);
    if (v && was && now - was > kBeatStaleMs)
        TwkLog("[fsr] the game thread was away %llu ms (a level load?) -- FSR idled through it", (unsigned long long)(now - was));
    if (InterlockedExchange(&g_inGame, v) != v) TwkLog("[fsr] %s", v ? "in a level -- FSR active" : "left the level -- FSR idle");
}

bool UpscaleFsr_AddPasses(void* gb, const uint8_t* view, const uint8_t* inputs, void** outColor, int32_t* outRect) {
    if (!g_on || !g_engineOk || g_failStage[0] || !view || !inputs || !outColor || !outRect) return false;
    if (!UpscaleFsr_Available()) return false;   // belt and braces: the pass never runs on DirectX 11
    // Only upscale actual gameplay, and only while the game thread is still SAYING so. With FSR on,
    // sitting in the main menu killed the RHI thread -- a NULL compute pipeline state reaching
    // RHISetComputePipelineState (it reads +0x18 of it unchecked), and once a device removal at a Map.
    // Root cause UNKNOWN (a starved pipeline-state create returns exactly that null, which is why the
    // status line carries video memory and our held references). Upscaling a menu or a loading screen
    // buys nothing, so we stay out of both.
    //
    // The flag alone was not enough, because it is a LATCH. Changing maps, the game thread goes into
    // the level load and stops pumping entirely, so it stayed stuck at "in a level" from the map just
    // left while the render thread carried on drawing the loading screen -- and we built passes
    // straight through the teardown, which is the same crash (reported from the field, 3.19.341).
    // A heartbeat cannot get stuck: no word from the game thread for kBeatStaleMs and we are not in a
    // level as far as THIS thread is concerned. A load is seconds; an ordinary frame hitch is nowhere
    // near it, and a hitch that long is already a visible stutter. Silent here by this module's rule --
    // the render thread sets state, the game-thread pump does the printing.
    if (!g_inGame) return false;
    if (GetTickCount64() - (uint64_t)g_beat > kBeatStaleMs) return false;
    const char* stage = "reading the view";
    __try {
        if (*(view + SV_SCENE_CAPTURE)) return false;
        const int32_t* vr = (const int32_t*)(view + VI_VIEW_RECT);
        if (vr[0] != 0 || vr[1] != 0) return false;
        void* taaOut = *outColor;
        uint8_t* color = *(uint8_t**)(inputs + 0x08);
        uint8_t* depth = *(uint8_t**)(inputs + 0x10);
        uint8_t* vel   = *(uint8_t**)(inputs + 0x18);
        if (!taaOut || !color || !depth || !vel) return false;
        const uint32_t rw = (uint32_t)(vr[2] - vr[0]), rh = (uint32_t)(vr[3] - vr[1]);
        const uint32_t ow = (uint32_t)(outRect[2] - outRect[0]), oh = (uint32_t)(outRect[3] - outRect[1]);
        if (!rw || !rh || !ow || !oh || rw > ow || rh > oh || ow > 8192 || oh > 8192) return false;

        uint8_t desc[RTD_SIZE]; memcpy(desc, (const uint8_t*)taaOut + RT_DESC, RTD_SIZE);
        stage = "creating the output texture";
        void* ourOut = g_rdgCreateTexture(gb, desc, L"SessionTweaks.FSR", 0);
        if (!ourOut) { Fail("CreateTexture returned null"); return false; }
        CopyInfo ci = {}; ci.numSlices = 1; ci.numMips = 1;
        stage = "the copy pass";
        g_rdgCopyPass(gb, taaOut, ourOut, &ci);
        stage = "borrowing the layout";
        if (!g_layout) {
            uint8_t** passes = *(uint8_t***)((uint8_t*)gb + GB_PASSES);
            const int n = *(const int*)((uint8_t*)gb + GB_PASSES + 8);
            if (!passes || n <= 0) { Fail("no pass after the copy"); return false; }
            void* layout = *(void**)(passes[n - 1] + PASS_LAYOUT);
            const int nt = layout ? *(const int*)((uint8_t*)layout + LAYOUT_GRAPH_TEXTURES + 8) : 0;
            if (nt != 2) { Fail("copy pass layout does not carry 2 textures"); return false; }
            g_layout = layout;
            TwkLog("[fsr] borrowed the engine's copy-pass parameter layout (%p, 2 texture accesses)", layout);
        }
        stage = "the frame record";
        const uint32_t fi = g_frameSerial;
        if (InterlockedCompareExchange(&g_frameBusy[fi & FRAME_MASK], 1, 0) != 0) {
            InterlockedIncrement(&g_skips);
            if (++g_skipRun == 1 || g_skipRun % 300 == 0)
                TwkLog("[fsr] the RHI thread is behind -- skipping FSR this frame (%ld in a row, %ld total)", g_skipRun, g_skips);
            if (g_skipRun >= 600) {   // a slot that never came back: reclaim rather than stay off forever
                for (int i = 0; i < FRAME_COUNT; i++) InterlockedExchange(&g_frameBusy[i], 0);
                g_skipRun = 0;
                TwkLog("[fsr] no slot came back for 600 frames -- reclaiming the ring");
            }
            return false;
        }
        g_skipRun = 0;
        g_frameSerial++;
        Frame* f = &g_frames[fi & FRAME_MASK];
        memset(f, 0, sizeof(*f));
        f->rdgColor = color; f->rdgDepth = depth; f->rdgVelocity = vel; f->rdgOut = ourOut;
        f->renderW = rw; f->renderH = rh; f->outW = ow; f->outH = oh;
        const float* jit = (const float*)(view + VI_JITTER);
        f->jitterX = g_flipJitX ? -jit[0] : jit[0];
        f->jitterY = g_flipJitY ? -jit[1] : jit[1];
        f->preExposure = *(const float*)(view + VI_PRE_EXPOSURE);
        if (!(f->preExposure > 0.0f)) f->preExposure = 1.0f;
        const uint8_t* fam = *(const uint8_t* const*)(view + SV_FAMILY);
        float dt = fam ? *(const float*)(fam + FAM_DELTA_TIME) : 0.0f;
        if (!(dt > 0.0f) || dt > 1.0f) dt = 1.0f / 60.0f;
        f->dtMs = dt * 1000.0f;
        const float* proj = (const float*)(view + SV_VIEW_MATRICES + VM_PROJ);
        const float m11 = proj[5];
        f->fovY = (m11 > 1e-6f) ? 2.0f * atanf(1.0f / m11) : 1.2f;
        f->camNear = proj[14] > 0.0f ? proj[14] : 10.0f;          // reversed-Z infinite: M[3][2] = near
        f->reset = *(view + SV_CAMERA_CUT) != 0 || fi == 0;
        {   // ClipToPrevClip = Inv(View_c * ProjNoAA_c) * (View_p * ProjNoAA_p)   (row vectors)
            const uint8_t* vm = view + SV_VIEW_MATRICES;
            const uint8_t* pm = view + VI_PREV_VIEW + PV_VIEW_MATRICES;
            float vpC[16], vpP[16], inv[16];
            MatMul((const float*)(vm + VM_VIEW), (const float*)(vm + VM_PROJ_NOAA), vpC);
            MatMul((const float*)(pm + VM_VIEW), (const float*)(pm + VM_PROJ_NOAA), vpP);
            if (MatInv(vpC, inv)) MatMul(inv, vpP, f->clipToPrev);
            else { for (int i = 0; i < 16; i++) f->clipToPrev[i] = (i % 5 == 0) ? 1.0f : 0.0f; f->reset = true; }
        }
        stage = "pass 1 (motion vectors)";
        OurPass* p1 = NewPass(gb, 1, fi, vel, ACCESS_SRV_COMPUTE, depth, ACCESS_SRV_COMPUTE);
        stage = "pass 2 (FSR)";
        OurPass* p2 = p1 ? NewPass(gb, 2, fi, color, ACCESS_SRV_COMPUTE, ourOut, ACCESS_UAV_COMPUTE) : nullptr;
        if (!p1 || !p2) { InterlockedExchange(&g_frameBusy[fi & FRAME_MASK], 0); Fail("SetupPass returned null"); return false; }
        *outColor = ourOut;
        outRect[0] = 0; outRect[1] = 0; outRect[2] = (int32_t)ow; outRect[3] = (int32_t)oh;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char why[160]; snprintf(why, sizeof(why), "exception while adding the passes: %s%s%s", stage,
                                g_passStage[0] ? " / " : "", g_passStage);
        Fail(why); return false;
    }
}

// ------------------------------------------------------------------ execution (render thread -> RHI thread)
static ffxContext g_ctx = nullptr;
static UINT g_ctxW = 0, g_ctxH = 0;
static char g_provider[48] = {0};

static bool EnsureContext(UINT w, UINT h) {
    if (g_ctx && g_ctxW == w && g_ctxH == h) return true;
    if (!LoadFfx()) { Fail("ffx runtime load"); return false; }
    SafeDestroy(&g_ctx);
    QueryVersions(g_dev);
    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (g_debug ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0);
    desc.maxRenderSize = { w, h }; desc.maxUpscaleSize = { w, h };
    desc.fpMessage = &FfxMessage;
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = g_dev;
    desc.header.pNext = &backend.header;
    ffxCreateContextDescUpscaleVersion apiVer = {};
    apiVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION; apiVer.version = FFX_UPSCALER_VERSION;
    backend.header.pNext = &apiVer.header;
    ffxOverrideVersion over = {};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    const bool want4 = g_preferFsr4 && !g_force3x && g_verFsr4 != 0;
    over.versionId = want4 ? g_verFsr4 : g_ver3x;
    g_chosenIsFsr4 = want4;
    if (over.versionId) apiVer.header.pNext = &over.header;
    ffxReturnCode_t rc = SafeCreate(&g_ctx, &desc.header);
    if (rc == FFX_SEH_CRASH && g_chosenIsFsr4) {
        TwkLog("[fsr] the FSR 4 provider crashed creating its context -- using the 3.1.x provider");
        g_force3x = true; g_chosenIsFsr4 = false; g_ctx = nullptr;
        over.versionId = g_ver3x; apiVer.header.pNext = g_ver3x ? &over.header : nullptr;
        rc = SafeCreate(&g_ctx, &desc.header);
    } else if (rc != FFX_API_RETURN_OK && rc != FFX_SEH_CRASH && over.versionId) {
        TwkLog("[fsr] context create with the version override failed (rc 0x%X) -- retrying the default provider", rc);
        apiVer.header.pNext = nullptr; g_ctx = nullptr; g_chosenIsFsr4 = false;
        rc = SafeCreate(&g_ctx, &desc.header);
    }
    if (rc != FFX_API_RETURN_OK) { g_ctx = nullptr; TwkLog("[fsr] ffxCreateContext failed (rc 0x%X)", rc); Fail("context create"); return false; }
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (SafeQuery(&g_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) snprintf(g_provider, sizeof(g_provider), "%s", pv.versionName);
    else snprintf(g_provider, sizeof(g_provider), "%s", want4 ? g_verName4 : g_verName3);
    g_ctxW = w; g_ctxH = h;
    TwkLog("[fsr] upscale context: provider %s, display %ux%u, flags 0x%X", g_provider, w, h, desc.flags);
    return true;
}

static DXGI_FORMAT SrvFormatFor(ID3D12Resource* r) {
    D3D12_RESOURCE_DESC d = r->GetDesc();
    switch (d.Format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default: return d.Format;
    }
}

static void DispatchMv(ID3D12GraphicsCommandList* cl, Frame* f) {
    OwnBarrier(cl, false);
    const UINT slot = g_slot++ & 3;
    MvConstants* c = (MvConstants*)(g_cbufMap + slot * 256);
    memcpy(c->clipToPrev, f->clipToPrev, sizeof(c->clipToPrev));
    c->renderW = (float)f->renderW; c->renderH = (float)f->renderH;
    c->invW = 1.0f / (float)f->renderW; c->invH = 1.0f / (float)f->renderH;
    c->flags[0] = (float)g_dynSign; c->flags[1] = g_flipMvX ? -1.0f : 1.0f; c->flags[2] = g_flipMvY ? -1.0f : 1.0f; c->flags[3] = 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * 4 * g_descSize; gpu.ptr += (UINT64)slot * 4 * g_descSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = SrvFormatFor(f->velocity); g_dev->CreateShaderResourceView(f->velocity, &sv, cpu); cpu.ptr += g_descSize;
    sv.Format = SrvFormatFor(f->depth);    g_dev->CreateShaderResourceView(f->depth, &sv, cpu);    cpu.ptr += g_descSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16_FLOAT; g_dev->CreateUnorderedAccessView(g_mvTex, nullptr, &uv, cpu); cpu.ptr += g_descSize;
    uv.Format = DXGI_FORMAT_R32_FLOAT;    g_dev->CreateUnorderedAccessView(g_depthTex, nullptr, &uv, cpu);
    ID3D12DescriptorHeap* heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rootSig);
    cl->SetPipelineState(g_pso);
    cl->SetComputeRootConstantBufferView(0, g_cbuf->GetGPUVirtualAddress() + slot * 256);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->Dispatch((f->renderW + 7) / 8, (f->renderH + 7) / 8, 1);
    OwnBarrier(cl, true);
}
static void DispatchFsr(ID3D12GraphicsCommandList* cl, Frame* f) {
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color         = ffxApiGetResourceDX12(f->color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth         = ffxApiGetResourceDX12(g_depthTex, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(g_mvTex, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output        = ffxApiGetResourceDX12(f->out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset      = { f->jitterX, f->jitterY };
    d.motionVectorScale = { 1.0f, 1.0f };
    d.renderSize        = { f->renderW, f->renderH };
    d.upscaleSize       = { f->outW, f->outH };
    d.enableSharpening  = g_sharpPct > 0;
    d.sharpness         = (float)g_sharpPct / 100.0f;
    d.frameTimeDelta    = f->dtMs;
    d.preExposure       = f->preExposure;
    d.reset             = f->reset;
    d.cameraNear        = f->camNear;
    d.cameraFar         = 1.0e7f;
    d.cameraFovAngleVertical = f->fovY;
    d.viewSpaceToMetersFactor = 0.01f;
    d.flags             = g_debug ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;
    ffxReturnCode_t rc = SafeDispatch(&g_ctx, &d.header);
    if (rc == FFX_API_RETURN_OK) { InterlockedIncrement(&g_dispatches); return; }
    if (rc == FFX_SEH_CRASH && g_chosenIsFsr4 && !g_force3x) {
        TwkLog("[fsr] the FSR 4 provider crashed in dispatch -- switching to the 3.1.x provider next frame");
        g_force3x = true; SafeDestroy(&g_ctx); g_ctxW = g_ctxH = 0;
        return;
    }
    TwkLog("[fsr] ffxDispatch failed (rc 0x%X%s)", rc, rc == FFX_SEH_CRASH ? " CRASHED" : "");
    Fail("dispatch");
}

static void RunNative(int kind, uint32_t fi, void* ctx) {
    Frame* f = &g_frames[fi & FRAME_MASK];
    __try {
        uint8_t* data = ctx ? *(uint8_t**)((uint8_t*)ctx + CTX_CMDLIST_HANDLE + CLH_DATA) : nullptr;
        ID3D12GraphicsCommandList* cl = data ? *(ID3D12GraphicsCommandList**)(data + CLD_LIST) : nullptr;
        if (!cl) { Fail("no native command list on the RHI context"); return; }
        if (!g_dev && FAILED(cl->GetDevice(IID_PPV_ARGS(&g_dev)))) { Fail("GetDevice"); return; }
        if (g_failStage[0]) return;
        if (kind == 1) {
            if (f->velocity && f->depth && EnsurePipeline() && EnsureOwnTextures(f->outW, f->outH)) DispatchMv(cl, f);
        } else {
            if (f->color && f->out && g_mvTex && EnsureContext(f->outW, f->outH)) DispatchFsr(cl, f);
        }
        g_d3dDirtyState((uint8_t*)ctx + CTX_STATE_CACHE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { Fail("exception in the native work"); }
    if (kind == 1) { if (f->velocity) { f->velocity->Release(); InterlockedDecrement(&g_outstanding); }
                     if (f->depth)    { f->depth->Release();    InterlockedDecrement(&g_outstanding); } f->velocity = f->depth = nullptr; }
    else           { if (f->color)    { f->color->Release();    InterlockedDecrement(&g_outstanding); }
                     if (f->out)      { f->out->Release();      InterlockedDecrement(&g_outstanding); } f->color = f->out = nullptr; }
    if (kind != 1) InterlockedExchange(&g_frameBusy[fi & FRAME_MASK], 0);   // the slot is the render thread's again
}
struct CmdNode { void** vt; CmdNode* next; int kind; uint32_t frame; };
// queued for the RHI thread the same way: reusing a node still in the chain cuts the command list
enum { NODE_COUNT = 32, NODE_MASK = NODE_COUNT - 1 };
static CmdNode  g_nodes[NODE_COUNT];
static uint32_t g_nodeSerial = 0;
static void __fastcall Node_Execute(CmdNode* self, void* cmdList, void* dbg) {
    (void)dbg;
    RunNative(self->kind, self->frame, *(void**)((uint8_t*)cmdList + CL_CONTEXT));
}
static void* g_nodeVtable[2] = { (void*)&Node_Execute, nullptr };
typedef void* (*GetNativeFn)(void* rhiTexture);
static ID3D12Resource* NativeOf(void* rdgTex) {
    uint8_t* rhi = rdgTex ? *(uint8_t**)((uint8_t*)rdgTex + RT_RHI) : nullptr;
    if (!rhi) return nullptr;
    void** vt = *(void***)rhi;
    ID3D12Resource* r = (ID3D12Resource*)((GetNativeFn)vt[RHITEX_VT_GET_NATIVE])(rhi);
    if (r) { r->AddRef(); InterlockedIncrement(&g_outstanding); }
    return r;
}
static void __fastcall Pass_Execute(OurPass* self, void* rhiCmdList) {
    Frame* f = &g_frames[self->frame & FRAME_MASK];
    const char* step = "native textures";
    __try {
        if (self->kind == 1) { f->velocity = NativeOf(f->rdgVelocity); f->depth = NativeOf(f->rdgDepth); }
        else                 { f->color = NativeOf(f->rdgColor);       f->out = NativeOf(f->rdgOut); }
        step = "command link";
        CmdNode* n = &g_nodes[g_nodeSerial++ & NODE_MASK];
        n->vt = g_nodeVtable; n->next = nullptr; n->kind = self->kind; n->frame = self->frame;
        if (*g_bypassFlag) {
            step = "bypass execute";
            Node_Execute(n, rhiCmdList, nullptr);
        } else {
            uint8_t* cl = (uint8_t*)rhiCmdList;
            void** link = *(void***)(cl + CL_LINK);
            *link = n;
            *(void**)(cl + CL_LINK) = &n->next;
            (*(int32_t*)(cl + CL_NUM))++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char why[96]; snprintf(why, sizeof(why), "exception in the pass Execute: %s", step);
        Fail(why);
    }
}

// ------------------------------------------------------------------ install / pump
void UpscaleFsr_Install() {
    if (!UpscaleFsr_Available()) {
        // Nothing is resolved and nothing is hooked: on DirectX 11 every engine function this would
        // reach for belongs to a renderer that is not running.
        SayUnavailable();
        g_engineOk = false;
        return;
    }
    g_engineOk = ResolveEngine();
    if (!g_engineOk) TwkLog("[fsr] engine functions incomplete -- FSR unavailable (game updated?)");
}
bool UpscaleFsr_Active() { return g_on && g_engineOk && !g_failStage[0]; }
int  UpscaleFsr_WantedTaaSamples() { return UpscaleFsr_Active() ? g_taaSamplesFsr : g_taaSamplesOff; }
const char* UpscaleFsr_Status() {
    static char s[200];
    if (!UpscaleFsr_Available()) return "unavailable -- the game is running DirectX 11";
    if (!g_on) return "off";
    if (g_failStage[0]) { snprintf(s, sizeof(s), "FAILED: %s", g_failStage); return s; }
    snprintf(s, sizeof(s), "%s, %ld dispatches, %ld skipped, provider %s, %s", g_ctx ? "running" : "starting",
             g_dispatches, g_skips, g_provider[0] ? g_provider : "?", g_bypassFlag && *g_bypassFlag ? "bypass" : "RHI thread");
    return s;
}
// A per-frame resource leak would show as video memory climbing long before it starves a pipeline-state
// create (a failed create hands the engine a null state -- exactly the menu access violation), so the
// health line carries the adapter's memory use and the references we are holding.
typedef HRESULT (WINAPI *CreateFactory1Fn)(REFIID, void**);
static IDXGIAdapter3* g_adapter = nullptr;
static bool g_adapterTried = false;
static void EnsureAdapter() {
    if (g_adapterTried || !g_dev) return;
    g_adapterTried = true;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    CreateFactory1Fn create = dxgi ? (CreateFactory1Fn)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!create) return;
    IDXGIFactory4* f = nullptr;
    if (FAILED(create(IID_PPV_ARGS(&f))) || !f) return;
    IDXGIAdapter3* a = nullptr;
    if (SUCCEEDED(f->EnumAdapterByLuid(g_dev->GetAdapterLuid(), IID_PPV_ARGS(&a)))) g_adapter = a;
    f->Release();
}
void UpscaleFsr_PumpFrame() {
    if (!g_log || !g_on) return;
    static uint64_t last = 0; const uint64_t ms = GetTickCount64();
    if (ms - last < 1000) return; last = ms;
    static long lastD = 0;
    EnsureAdapter();
    char vram[64] = "";
    if (g_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
            snprintf(vram, sizeof(vram), " | VRAM %llu/%llu MB", (unsigned long long)(vm.CurrentUsage >> 20),
                     (unsigned long long)(vm.Budget >> 20));
    }
    TwkLog("[fsr] %s | %ld/s%s | refs held %ld", UpscaleFsr_Status(), g_dispatches - lastD, vram, g_outstanding);
    lastD = g_dispatches;
}
