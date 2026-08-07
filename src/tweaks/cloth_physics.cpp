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
// SessionTweaks -- CLOTH/WIND recon probe (cloth_physics.h has the charter).
//
// Why a probe and not a feature: the engine ships the full cloth pipeline (both the NvCloth and
// Chaos factories are in the exe), but the skater's outfit is a runtime FSkeletalMeshMerge
// (RefreshVisuals calls DoMerge directly) and a merge discards clothing-simulation data -- so
// whether anything can be forced depends on the cooked ASSETS: do any garment/prop meshes carry
// MeshClothingAssets, do props carry physics bodies, and is the wind wobble some shirts show a
// material PARAMETER (forceable) or a different parent material (re-parent via MID) or a static
// switch (cooked, per-instance)? None of that is knowable from code; this dump reads it out of the
// live objects. The follow-up feature builds on whichever channel the log proves real.
//
// Everything here is a READ. The only game function called is FName::ToString (via the cached
// resolver), plus UE4SS's FindAllOf for the global census -- both from the game thread, one shot
// per button press.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <vector>
#include "tweaks_common.h"
#include "catch_tweaks.h"          // CatchTweaks_Skater(): the live skater, published for pump use
#include "cloth_physics.h"
#include "ue4ss_abi.h"             // RC::Unreal::UObjectGlobals::FindAllOf (global census)
#include "ui/menu_ext.h"

// ------------------------------------------------------------------ offsets (all PDB-read)
enum : int {
    // UObjectBase
    OBJ_CLASS               = 0x10,   // UClass*
    OBJ_NAME                = 0x18,   // FName
    // ACharacter / ASkaterCharacterBase
    CH_MESH                 = 0x280,  // USkeletalMeshComponent* (the merged body)
    SK_CHARACTER_PROPS      = 0x928,  // TArray<FSkaterCharacterProp*> (count +0x930)
    // FSkaterCharacterProp (size 32)
    PROP_SIMULATE           = 0x00,   // bool -- the game's own accessory-physics switch
    PROP_SIM_BELOW_BONE     = 0x04,   // FName
    PROP_SKEL_COMP          = 0x10,   // USkeletalMeshComponent*
    PROP_STATIC_COMP        = 0x18,   // UStaticMeshComponent*
    // USkinnedMeshComponent / USkeletalMeshComponent
    CMP_SKELETAL_MESH       = 0x480,  // USkeletalMesh*
    CMP_OVERRIDE_MATS       = 0x450,  // TArray<UMaterialInterface*> (UMeshComponent)
    CMP_DISABLE_CLOTH_SIM   = 0x8ba,  // bDisableClothSimulation
    CMP_HAS_VALID_BODIES    = 0x8b9,  // bHasValidBodies
    CMP_CLOTH_FACTORY       = 0x900,  // TSubclassOf<UClothingSimulationFactory>
    CMP_BODIES              = 0x980,  // TArray<FBodyInstance*>
    CMP_CLOTH_SIM           = 0xa30,  // IClothingSimulation* (non-null = cloth actually running)
    // UStaticMeshComponent
    CMP_STATIC_MESH         = 0x488,  // UStaticMesh*
    // USkeletalMesh
    MESH_HAS_ACTIVE_CLOTH   = 0x15f,  // bHasActiveClothingAssets
    MESH_PHYSICS_ASSET      = 0x168,  // UPhysicsAsset*
    MESH_MATERIALS          = 0xd8,   // TArray<FSkeletalMaterial> (stride 40, interface at +0x00)
    MESH_MATERIAL_STRIDE    = 40,
    MESH_CLOTHING_ASSETS    = 0x320,  // TArray<UClothingAssetBase*>
    // UMaterialInstance
    MI_PARENT               = 0xd0,   // UMaterialInterface*
    MI_SCALAR_PARAMS        = 0xe0,   // TArray<FScalarParameterValue>  (stride 36, value +0x10)
    MI_VECTOR_PARAMS        = 0xf0,   // TArray<FVectorParameterValue>  (stride 48, color +0x10)
    MI_TEXTURE_PARAMS       = 0x100,  // TArray<FTextureParameterValue> (stride 40, tex   +0x10)
    MI_STATIC_SWITCHES      = 0x148,  // FStaticParameterSet.StaticSwitchParameters
                                      //   (stride 40, name +0x00, bool value +0x24)
    // every parameter struct starts with FMaterialParameterInfo whose FName Name is at +0x00
};

// ------------------------------------------------------------------ FName -> text (cached)
// FName::ToString(FString&) -- Epic 0x133a4f0 / Steam 0x12fb7b0. CALLED, never hooked, game thread
// only. Sig proven on both exes in src/game/game_syms.cpp; grind_pop carries the same copy.
static const char* SIG_FNAME_TOSTR =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00";

typedef void (*FNameToStrFn)(const void*, void*);
static FNameToStrFn g_fnameToStr = nullptr;

// An FName's text never changes, so caching by the FName value bounds the FString churn. The cache
// is larger than grind_pop's: one dump touches every material parameter name on the outfit.
static bool NameOfFName(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    if (!fnamePtr || !g_fnameToStr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fnamePtr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;                                    // NAME_None
    struct Entry { uint64_t key; char name[64]; };
    static Entry cache[256];
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
static bool NameOfObject(const void* obj, char* out, int cap) {
    out[0] = 0;
    if (!obj) return false;
    return NameOfFName((const uint8_t*)obj + OBJ_NAME, out, cap);
}
// The object's CLASS name, read through ClassPrivate. This is the honest discriminator between a
// base UMaterial and a MaterialInstance* -- reading instance fields off a base material would
// dereference plausible garbage.
static bool ClassNameOf(const void* obj, char* out, int cap) {
    out[0] = 0;
    return obj && NameOfObject(twkP(obj, OBJ_CLASS), out, cap);
}

// ------------------------------------------------------------------ state
static volatile LONG g_dumpReq = 0;      // set by the F1 button (render thread), drained by the pump
static bool g_probeOK = true;            // one-shot kill: a faulting dump disables ONLY this probe
static char g_status[96] = "no dump yet";

// ------------------------------------------------------------------ material dump
static void dumpMaterial(int slot, const void* mat) {
    char name[64], cls[64];
    if (!mat) { TwkLog("[cloth]   slot %d: <null material>", slot); return; }
    NameOfObject(mat, name, sizeof(name));
    ClassNameOf(mat, cls, sizeof(cls));
    TwkLog("[cloth]   slot %d: %s (%s)", slot, name[0] ? name : "?", cls[0] ? cls : "?");
    if (!strstr(cls, "MaterialInstance")) return;   // a base UMaterial has no override arrays

    // Parent chain up to the master -- route B (re-parent onto a wind master) needs these names.
    const void* cur = mat;
    for (int d = 0; d < 4; d++) {
        const void* parent = twkP(cur, MI_PARENT);
        if (!parent) break;
        char pn[64], pc[64];
        NameOfObject(parent, pn, sizeof(pn));
        ClassNameOf(parent, pc, sizeof(pc));
        TwkLog("[cloth]     parent[%d]: %s (%s)", d, pn[0] ? pn : "?", pc[0] ? pc : "?");
        if (!strstr(pc, "MaterialInstance")) break;  // reached the base material
        cur = parent;
    }
    // Overridden parameters. Only OVERRIDES live here -- a parameter left at the parent's default
    // does not appear, which is exactly what makes the windy-vs-plain diff meaningful.
    const uint8_t* sd = (const uint8_t*)twkP(mat, MI_SCALAR_PARAMS);
    int sn = twkI(mat, MI_SCALAR_PARAMS + 8); if (sn < 0 || sn > 128) sn = 0;
    for (int i = 0; i < sn && sd; i++) {
        char pn[64]; NameOfFName(sd + i * 36, pn, sizeof(pn));
        TwkLog("[cloth]     scalar '%s' = %.4f", pn[0] ? pn : "?", twkF(sd, i * 36 + 0x10));
    }
    const uint8_t* vd = (const uint8_t*)twkP(mat, MI_VECTOR_PARAMS);
    int vn = twkI(mat, MI_VECTOR_PARAMS + 8); if (vn < 0 || vn > 128) vn = 0;
    for (int i = 0; i < vn && vd; i++) {
        char pn[64]; NameOfFName(vd + i * 48, pn, sizeof(pn));
        TwkLog("[cloth]     vector '%s' = (%.3f %.3f %.3f %.3f)", pn[0] ? pn : "?",
               twkF(vd, i * 48 + 0x10), twkF(vd, i * 48 + 0x14),
               twkF(vd, i * 48 + 0x18), twkF(vd, i * 48 + 0x1c));
    }
    const uint8_t* td = (const uint8_t*)twkP(mat, MI_TEXTURE_PARAMS);
    int tn = twkI(mat, MI_TEXTURE_PARAMS + 8); if (tn < 0 || tn > 128) tn = 0;
    for (int i = 0; i < tn && td; i++) {
        char pn[64], tex[64];
        NameOfFName(td + i * 40, pn, sizeof(pn));
        NameOfObject(twkP(td, i * 40 + 0x10), tex, sizeof(tex));
        TwkLog("[cloth]     texture '%s' = %s", pn[0] ? pn : "?", tex[0] ? tex : "<null>");
    }
    const uint8_t* wd = (const uint8_t*)twkP(mat, MI_STATIC_SWITCHES);
    int wn = twkI(mat, MI_STATIC_SWITCHES + 8); if (wn < 0 || wn > 64) wn = 0;
    for (int i = 0; i < wn && wd; i++) {
        char pn[64]; NameOfFName(wd + i * 40, pn, sizeof(pn));
        TwkLog("[cloth]     switch '%s' = %d", pn[0] ? pn : "?", twkB(wd, i * 40 + 0x24));
    }
}

// ------------------------------------------------------------------ skeletal component dump
static void dumpSkelComp(const char* label, const void* comp) {
    if (!comp) return;
    const void* mesh = twkP(comp, CMP_SKELETAL_MESH);
    char mn[64];
    NameOfObject(mesh, mn, sizeof(mn));
    const int clothN   = mesh ? twkI(mesh, MESH_CLOTHING_ASSETS + 8) : -1;
    const int hasCloth = mesh ? twkB(mesh, MESH_HAS_ACTIVE_CLOTH) : -1;
    char pa[64];
    NameOfObject(mesh ? twkP(mesh, MESH_PHYSICS_ASSET) : nullptr, pa, sizeof(pa));
    TwkLog("[cloth] %s: mesh=%s clothAssets=%d hasActiveCloth=%d physAsset=%s",
           label, mn[0] ? mn : "<none>", clothN, hasCloth, pa[0] ? pa : "<none>");
    char fac[64];
    NameOfObject(twkP(comp, CMP_CLOTH_FACTORY), fac, sizeof(fac));
    TwkLog("[cloth]   comp: disableClothSim=%d clothSimLive=%d validBodies=%d bodies=%d factory=%s",
           twkB(comp, CMP_DISABLE_CLOTH_SIM), twkP(comp, CMP_CLOTH_SIM) ? 1 : 0,
           twkB(comp, CMP_HAS_VALID_BODIES), twkI(comp, CMP_BODIES + 8), fac[0] ? fac : "<none>");
    // Any clothing assets on the mesh get named -- this line existing at all IS the headline.
    if (mesh && clothN > 0 && clothN <= 16) {
        const uint8_t* ca = (const uint8_t*)twkP(mesh, MESH_CLOTHING_ASSETS);
        for (int i = 0; i < clothN && ca; i++) {
            char cn[64]; NameOfObject(twkP(ca, i * 8), cn, sizeof(cn));
            TwkLog("[cloth]   CLOTHING ASSET[%d]: %s", i, cn[0] ? cn : "?");
        }
    }
    // Material slots: the component override wins where present, else the mesh's own slot -- the
    // same precedence the renderer uses.
    if (!mesh) return;
    const uint8_t* md = (const uint8_t*)twkP(mesh, MESH_MATERIALS);
    int mnum = twkI(mesh, MESH_MATERIALS + 8); if (mnum < 0 || mnum > 64) mnum = 0;
    const uint8_t* od = (const uint8_t*)twkP(comp, CMP_OVERRIDE_MATS);
    int onum = twkI(comp, CMP_OVERRIDE_MATS + 8); if (onum < 0 || onum > 64) onum = 0;
    const int slots = mnum > onum ? mnum : onum;
    for (int i = 0; i < slots; i++) {
        const void* mat = (i < onum && od) ? twkP(od, i * 8) : nullptr;
        if (!mat && i < mnum && md) mat = twkP(md, i * MESH_MATERIAL_STRIDE);
        dumpMaterial(i, mat);
    }
}

// ------------------------------------------------------------------ global census
// SEH lives in a frame with nothing to unwind (the C2712 rule); FindAllOf walks GUObjectArray, which
// is why this runs once per button press and never per frame.
static bool findAllSeh(const wchar_t* cls, std::vector<RC::Unreal::UObject*>& out) {
    __try { RC::Unreal::UObjectGlobals::FindAllOf(cls, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void dumpCensus(const wchar_t* cls, const char* what, bool listAll) {
    std::vector<RC::Unreal::UObject*> found;
    if (!findAllSeh(cls, found)) { TwkLog("[cloth] census %s: FindAllOf faulted", what); return; }
    TwkLog("[cloth] census: %d %s loaded", (int)found.size(), what);
    const int cap = listAll ? (int)found.size() : 8;
    for (int i = 0; i < (int)found.size() && i < cap; i++) {
        char n[64]; NameOfObject(found[i], n, sizeof(n));
        TwkLog("[cloth]   %s[%d]: %s", what, i, n[0] ? n : "?");
    }
}

// ------------------------------------------------------------------ the dump
static void runDump() {
    void* skater = CatchTweaks_Skater();
    if (!skater) {
        TwkLog("[cloth] dump: no skater seen yet -- ride around for a moment first");
        strncpy_s(g_status, sizeof(g_status), "no skater yet -- ride a moment, retry", _TRUNCATE);
        return;
    }
    TwkLog("[cloth] ================ cloth/material recon dump ================");
    dumpSkelComp("body(merged)", twkP(skater, CH_MESH));

    const uint8_t* pd = (const uint8_t*)twkP(skater, SK_CHARACTER_PROPS);
    int pn = twkI(skater, SK_CHARACTER_PROPS + 8); if (pn < 0 || pn > 32) pn = 0;
    TwkLog("[cloth] props: %d", pn);
    for (int i = 0; i < pn && pd; i++) {
        const void* prop = twkP(pd, i * 8);
        if (!prop) continue;
        char bone[64];
        NameOfFName((const uint8_t*)prop + PROP_SIM_BELOW_BONE, bone, sizeof(bone));
        TwkLog("[cloth] prop %d: simulatePhysics=%d belowBone=%s",
               i, twkB(prop, PROP_SIMULATE), bone[0] ? bone : "<none>");
        char label[32];
        snprintf(label, sizeof(label), "prop %d skel", i);
        dumpSkelComp(label, twkP(prop, PROP_SKEL_COMP));
        const void* sc = twkP(prop, PROP_STATIC_COMP);
        if (sc) {
            char sn[64];
            NameOfObject(twkP(sc, CMP_STATIC_MESH), sn, sizeof(sn));
            TwkLog("[cloth] prop %d static: mesh=%s", i, sn[0] ? sn : "<none>");
            const uint8_t* od = (const uint8_t*)twkP(sc, CMP_OVERRIDE_MATS);
            int onum = twkI(sc, CMP_OVERRIDE_MATS + 8); if (onum < 0 || onum > 32) onum = 0;
            for (int m = 0; m < onum && od; m++) dumpMaterial(m, twkP(od, m * 8));
        }
    }
    // The census answers the questions no component walk can: are there ANY clothing assets in the
    // process (zero across varied outfits = real cloth has no data to run on), is there a wind/
    // weather material parameter collection (a global knob the wobble might ride), any wind sources.
    dumpCensus(L"ClothingAssetCommon",            "ClothingAssetCommon", true);
    dumpCensus(L"MaterialParameterCollection",    "MaterialParameterCollection", true);
    dumpCensus(L"WindDirectionalSourceComponent", "WindDirectionalSourceComponent", false);
    TwkLog("[cloth] ================ dump complete ================");
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(g_status, sizeof(g_status), "dumped %02d:%02d:%02d -- see SessionTweaks.log",
             t.wHour, t.wMinute, t.wSecond);
}

// ------------------------------------------------------------------ module surface
// No persisted settings: the probe is a button, not a behavior. The config hooks exist so the shell
// contract stays uniform and future knobs have a home.
void ClothPhys_ReadConfig(const char*) {}
void ClothPhys_SaveConfig(char*, size_t) {}
void ClothPhys_ResetDefaults() {}

void ClothPhys_Install() {
    g_fnameToStr = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR);
    if (!g_fnameToStr)
        TwkLog("[cloth] FName::ToString not found -- dump will show pointers without names");
    TwkLog("[cloth] recon probe ready (F1 -> Session Tweaks -> Dump cloth/material recon)");
}

void ClothPhys_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    if (api->version >= 2 && api->Button && api->Button("Dump cloth/material recon"))
        InterlockedExchange(&g_dumpReq, 1);
    api->TextDisabled(g_status);
}

void ClothPhys_PumpFrame() {
    if (!g_dumpReq) return;
    InterlockedExchange(&g_dumpReq, 0);
    if (!g_probeOK) return;
    // The reads inside are individually fault-tolerant; this guard exists so an unforeseen fault
    // disables the probe alone, never the module set it ships with.
    __try { runDump(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_probeOK = false;
        TwkLog("[cloth] dump FAULTED -- probe disabled for this run");
        strncpy_s(g_status, sizeof(g_status), "dump faulted -- probe disabled", _TRUNCATE);
    }
}
