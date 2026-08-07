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
// The dump is a READ. The only game function it calls is FName::ToString (via the cached
// resolver), plus UE4SS's FindAllOf for the global census -- both from the game thread, one shot
// per button press.
//
// CLOTH WIND BOOST (the shipped feature the dumps led to). The field dumps decoded the wobble:
// wind-capable garments sit on the MAT_ClothMaster master material (baked BC/MG/NM + a per-item
// WindMask texture); the newer procedural masters (MAT_Cloth_UB_Master, M_Apparel_LowerBody_Master,
// MAT_Cap_Master, MAT_Shoe_Master) have no wind code at all, so shader wind cannot be given to
// those garments -- their look is composed in-shader with different parameters, and re-parenting
// them onto MAT_ClothMaster would replace the garment's appearance with the master's defaults.
// The wind amplitude rides ONE global: the 'PlayerSpeed' scalar of the MPC_ClothWind material
// parameter collection, written by the game through UMaterialParameterCollectionInstance::
// SetScalarParameterValue. The boost hooks that setter and, when the write is ClothWind's
// PlayerSpeed, scales it and floors it -- flap harder while riding, keep a breeze at a standstill.
// The hook itself never calls a game API and never logs; naming the (instance, FName) pair happens
// on the pump from a small candidate table, and only the named pair is ever modified.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <vector>
#include "tweaks_common.h"
#include "catch_tweaks.h"          // CatchTweaks_Skater(): the live skater, published for pump use
#include "cloth_physics.h"
#include "ue4ss_abi.h"             // RC::Unreal::UObjectGlobals::FindAllOf (global census)
#include "MinHook.h"
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
    // UMaterialParameterCollection (the MPC_ClothWind lead)
    MPC_SCALAR_PARAMS       = 0x38,   // TArray<FCollectionScalarParameter> (stride 28)
    MPC_VECTOR_PARAMS       = 0x48,   // TArray<FCollectionVectorParameter> (stride 40)
                                      //   both: FName name +0x00, DefaultValue +0x18
    // UMaterialParameterCollectionInstance (the world's LIVE values; only runtime-set keys appear)
    MPCI_COLLECTION         = 0x30,   // UMaterialParameterCollection*
    MPCI_SCALAR_MAP         = 0x40,   // TMap<FName,float>        (element stride 20, value +0x08)
    MPCI_VECTOR_MAP         = 0x90,   // TMap<FName,FLinearColor> (element stride 32, value +0x08)
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

// ------------------------------------------------------------------ wind boost
// UMaterialParameterCollectionInstance::SetScalarParameterValue -- Epic 0x2d85a80 / Steam 0x2d482f0,
// sig unique in both. ABI read out of the callee: rcx=this, rdx=FName BY VALUE, xmm2=float (the
// entry spills it with movss, so it is provably a single, not a double); returns bool (false when
// the name is not in the instance's collection).
static const char* SIG_MPCI_SET_SCALAR =
    "F3 0F 11 54 24 18 48 89 54 24 10 53 55 41 56 48 83 EC 50 48 8B 41 30 48 8B E9 48 8B DA 48 63 48 40";
typedef bool (*MpciSetScalarFn)(void*, uint64_t, float);
static MpciSetScalarFn g_origSetScalar = nullptr;

// Knobs (ini + F1). The boost only ever changes the ONE value the dumps proved is the wind
// amplitude, so a wrong theory shows as "nothing happens", never as damage.
static int   g_windBoost = 1;        // master toggle
static float g_windBase  = 400.0f;   // breeze floor, in the game's own PlayerSpeed units (~cm/s)
static float g_windMult  = 200.0f;   // percent applied to the game's written speed

static bool g_windOK = true;         // one-shot kill for the floor writer alone

// Identification: the hook records (instance, FName) pairs it sees; the pump names them (FName::
// ToString is a game API and may not be called from inside the hooked callstack) and latches the
// ClothWind/PlayerSpeed pair. FNames are stable for the process, so after that the hook matches by
// FName value alone -- the instance pointer is refreshed from each matching write because the
// per-world instance dies on a map change.
struct WindCand { void* inst; uint64_t fname; };
static WindCand      g_windCands[8];
static volatile LONG g_windCandN = 0;
static volatile LONG g_windIdDone = 0;
static uint64_t          g_cwFName = 0;
static void* volatile    g_cwInst = nullptr;
static volatile LONGLONG g_cwLastWriteMs = 0;   // GAME writes only (our floor writes bypass the hook)

static bool hkSetScalar(void* inst, uint64_t fname, float v) {
    if (g_windIdDone) {
        if (fname == g_cwFName) {
            g_cwInst = inst;
            InterlockedExchange64(&g_cwLastWriteMs, (LONGLONG)GetTickCount64());
            if (g_windBoost) {
                float boosted = v * (g_windMult * 0.01f);
                if (boosted < g_windBase) boosted = g_windBase;
                v = boosted;
            }
        }
    } else if (g_windCandN < 8) {
        const LONG n = g_windCandN;
        bool seen = false;
        for (LONG i = 0; i < n && i < 8; i++)
            if (g_windCands[i].inst == inst && g_windCands[i].fname == fname) { seen = true; break; }
        if (!seen) {
            const LONG at = InterlockedIncrement(&g_windCandN) - 1;
            if (at < 8) { g_windCands[at].inst = inst; g_windCands[at].fname = fname; }
        }
    }
    return g_origSetScalar ? g_origSetScalar(inst, fname, v) : false;
}

// GAME THREAD (from the pump). Names candidates until the pair is found, then keeps the breeze
// alive when the game's own writer goes quiet (its write already carries the floor otherwise).
static void windPump() {
    if (!g_origSetScalar) return;
    if (!g_windIdDone) {
        const LONG n = g_windCandN;
        for (LONG i = 0; i < n && i < 8; i++) {
            char cn[64], pn[64];
            NameOfObject(twkP(g_windCands[i].inst, MPCI_COLLECTION), cn, sizeof(cn));
            if (strcmp(cn, "MPC_ClothWind") != 0) continue;
            NameOfFName(&g_windCands[i].fname, pn, sizeof(pn));
            if (strcmp(pn, "PlayerSpeed") != 0) continue;
            g_cwFName = g_windCands[i].fname;
            g_cwInst  = g_windCands[i].inst;
            InterlockedExchange64(&g_cwLastWriteMs, (LONGLONG)GetTickCount64());
            InterlockedExchange(&g_windIdDone, 1);
            TwkLog("[cloth] wind writer identified: MPC_ClothWind 'PlayerSpeed' -- boost %s (base %.0f, x%.0f%%)",
                   g_windBoost ? "ON" : "off", g_windBase, g_windMult);
            break;
        }
        return;
    }
    if (!g_windBoost || !g_windOK) return;
    void* inst = g_cwInst;
    if (!inst) return;
    const LONGLONG now = (LONGLONG)GetTickCount64();
    const LONGLONG last = g_cwLastWriteMs;
    // No game write for a long time = the instance may be a dead world's; drop it and wait for the
    // next real write to re-point us. A missed breeze is recoverable, a call into a freed object is not.
    if (now - last > 5000) { g_cwInst = nullptr; return; }
    if (now - last > 250) {
        static LONGLONG lastFloor = 0;
        if (now - lastFloor > 100) {
            lastFloor = now;
            __try { g_origSetScalar(inst, g_cwFName, g_windBase); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_windOK = false;
                TwkLog("[cloth] wind floor write FAULTED -- breeze floor disabled (hook boost still active)");
            }
        }
    }
}

// ------------------------------------------------------------------ material dump
// One instance level's OVERRIDES. A parameter left at the parent's default does not appear here,
// which is exactly what makes a windy-vs-plain diff meaningful.
static void dumpMatParams(const void* mat, const char* ind) {
    const uint8_t* sd = (const uint8_t*)twkP(mat, MI_SCALAR_PARAMS);
    int sn = twkI(mat, MI_SCALAR_PARAMS + 8); if (sn < 0 || sn > 128) sn = 0;
    for (int i = 0; i < sn && sd; i++) {
        char pn[64]; NameOfFName(sd + i * 36, pn, sizeof(pn));
        TwkLog("[cloth]   %sscalar '%s' = %.4f", ind, pn[0] ? pn : "?", twkF(sd, i * 36 + 0x10));
    }
    const uint8_t* vd = (const uint8_t*)twkP(mat, MI_VECTOR_PARAMS);
    int vn = twkI(mat, MI_VECTOR_PARAMS + 8); if (vn < 0 || vn > 128) vn = 0;
    for (int i = 0; i < vn && vd; i++) {
        char pn[64]; NameOfFName(vd + i * 48, pn, sizeof(pn));
        TwkLog("[cloth]   %svector '%s' = (%.3f %.3f %.3f %.3f)", ind, pn[0] ? pn : "?",
               twkF(vd, i * 48 + 0x10), twkF(vd, i * 48 + 0x14),
               twkF(vd, i * 48 + 0x18), twkF(vd, i * 48 + 0x1c));
    }
    const uint8_t* td = (const uint8_t*)twkP(mat, MI_TEXTURE_PARAMS);
    int tn = twkI(mat, MI_TEXTURE_PARAMS + 8); if (tn < 0 || tn > 128) tn = 0;
    for (int i = 0; i < tn && td; i++) {
        char pn[64], tex[64];
        NameOfFName(td + i * 40, pn, sizeof(pn));
        NameOfObject(twkP(td, i * 40 + 0x10), tex, sizeof(tex));
        TwkLog("[cloth]   %stexture '%s' = %s", ind, pn[0] ? pn : "?", tex[0] ? tex : "<null>");
    }
    const uint8_t* wd = (const uint8_t*)twkP(mat, MI_STATIC_SWITCHES);
    int wn = twkI(mat, MI_STATIC_SWITCHES + 8); if (wn < 0 || wn > 64) wn = 0;
    for (int i = 0; i < wn && wd; i++) {
        char pn[64]; NameOfFName(wd + i * 40, pn, sizeof(pn));
        TwkLog("[cloth]   %sswitch '%s' = %d", ind, pn[0] ? pn : "?", twkB(wd, i * 40 + 0x24));
    }
}
static void dumpMaterial(int slot, const void* mat) {
    char name[64], cls[64];
    if (!mat) { TwkLog("[cloth]   slot %d: <null material>", slot); return; }
    NameOfObject(mat, name, sizeof(name));
    ClassNameOf(mat, cls, sizeof(cls));
    TwkLog("[cloth]   slot %d: %s (%s)", slot, name[0] ? name : "?", cls[0] ? cls : "?");
    if (!strstr(cls, "MaterialInstance")) return;   // a base UMaterial has no override arrays
    dumpMatParams(mat, "  ");
    // Parent chain up to the master, dumping EVERY instance level's overrides: a per-item wind gate
    // set on the item's MaterialInstanceConstant (under a shared per-category MID) would be
    // invisible in a top-level-only dump -- the first field round proved the garment MIDs carry
    // nothing but 'Dirt'.
    const void* cur = mat;
    for (int d = 0; d < 4; d++) {
        const void* parent = twkP(cur, MI_PARENT);
        if (!parent) break;
        char pn[64], pc[64];
        NameOfObject(parent, pn, sizeof(pn));
        ClassNameOf(parent, pc, sizeof(pc));
        TwkLog("[cloth]     parent[%d]: %s (%s)", d, pn[0] ? pn : "?", pc[0] ? pc : "?");
        if (!strstr(pc, "MaterialInstance")) break;  // reached the base material
        dumpMatParams(parent, "    ");
        cur = parent;
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

// ------------------------------------------------------------------ parameter collections
// Each collection ASSET carries the parameter names + authored defaults; the per-world INSTANCE
// carries only the values something set at runtime. Together they name every global knob the cloth
// wind could ride and what it currently reads as.
static void dumpOneCollection(const void* col) {
    char cn[64]; NameOfObject(col, cn, sizeof(cn));
    const uint8_t* sd = (const uint8_t*)twkP(col, MPC_SCALAR_PARAMS);
    int sn = twkI(col, MPC_SCALAR_PARAMS + 8); if (sn < 0 || sn > 64) sn = 0;
    const uint8_t* vd = (const uint8_t*)twkP(col, MPC_VECTOR_PARAMS);
    int vn = twkI(col, MPC_VECTOR_PARAMS + 8); if (vn < 0 || vn > 64) vn = 0;
    TwkLog("[cloth] collection %s: %d scalar, %d vector", cn[0] ? cn : "?", sn, vn);
    for (int i = 0; i < sn && sd; i++) {
        char pn[64]; NameOfFName(sd + i * 28, pn, sizeof(pn));
        TwkLog("[cloth]     scalar '%s' default=%.4f", pn[0] ? pn : "?", twkF(sd, i * 28 + 0x18));
    }
    for (int i = 0; i < vn && vd; i++) {
        char pn[64]; NameOfFName(vd + i * 40, pn, sizeof(pn));
        TwkLog("[cloth]     vector '%s' default=(%.3f %.3f %.3f %.3f)", pn[0] ? pn : "?",
               twkF(vd, i * 40 + 0x18), twkF(vd, i * 40 + 0x1c),
               twkF(vd, i * 40 + 0x20), twkF(vd, i * 40 + 0x24));
    }
}
// A TMap's sparse array: {Data, Num, Max, ...}; element = TPair + 8 bytes of hash bookkeeping.
// Freed holes read as garbage names and are skipped -- read-only, so a wrong assumption can only
// mis-read (the same argument as the cosmetics map walker).
static void dumpInstanceMap(const void* inst, int mapOff, int stride, bool vec, const char* kind) {
    const uint8_t* data = (const uint8_t*)twkP(inst, mapOff);
    int num = twkI(inst, mapOff + 8); if (num < 0 || num > 64) num = 0;
    for (int i = 0; i < num && data; i++) {
        char pn[64];
        if (!NameOfFName(data + i * stride, pn, sizeof(pn)) || !pn[0]) continue;
        if (vec)
            TwkLog("[cloth]     live %s '%s' = (%.3f %.3f %.3f %.3f)", kind, pn,
                   twkF(data, i * stride + 0x8), twkF(data, i * stride + 0xc),
                   twkF(data, i * stride + 0x10), twkF(data, i * stride + 0x14));
        else
            TwkLog("[cloth]     live %s '%s' = %.4f", kind, pn, twkF(data, i * stride + 0x8));
    }
}
static void dumpCollections() {
    std::vector<RC::Unreal::UObject*> cols;
    if (findAllSeh(L"MaterialParameterCollection", cols)) {
        for (int i = 0; i < (int)cols.size() && i < 16; i++) dumpOneCollection(cols[i]);
    }
    std::vector<RC::Unreal::UObject*> insts;
    if (findAllSeh(L"MaterialParameterCollectionInstance", insts)) {
        TwkLog("[cloth] %d collection instance(s) (runtime-set values only):", (int)insts.size());
        for (int i = 0; i < (int)insts.size() && i < 16; i++) {
            char cn[64]; NameOfObject(twkP(insts[i], MPCI_COLLECTION), cn, sizeof(cn));
            TwkLog("[cloth]   instance[%d] of %s", i, cn[0] ? cn : "?");
            dumpInstanceMap(insts[i], MPCI_SCALAR_MAP, 20, false, "scalar");
            dumpInstanceMap(insts[i], MPCI_VECTOR_MAP, 32, true,  "vector");
        }
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
    dumpCensus(L"WindDirectionalSourceComponent", "WindDirectionalSourceComponent", false);
    dumpCollections();
    TwkLog("[cloth] ================ dump complete ================");
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(g_status, sizeof(g_status), "dumped %02d:%02d:%02d -- see SessionTweaks.log",
             t.wHour, t.wMinute, t.wSecond);
}

// ------------------------------------------------------------------ module surface
void ClothPhys_ReadConfig(const char* ini) {
    g_windBoost = TwkIniInt(ini, "ClothWindBoost", 1);
    g_windBase  = (float)TwkIniInt(ini, "ClothWindBase", 400);
    g_windMult  = (float)TwkIniInt(ini, "ClothWindMult", 200);
    TwkLog("[cloth] config: ClothWindBoost=%d ClothWindBase=%.0f ClothWindMult=%.0f%%",
           g_windBoost, g_windBase, g_windMult);
}
void ClothPhys_SaveConfig(char* ini, size_t cap) {
    TwkIniSetInt(ini, cap, "ClothWindBoost", g_windBoost ? 1 : 0);
    TwkIniSetInt(ini, cap, "ClothWindBase", (int)g_windBase);
    TwkIniSetInt(ini, cap, "ClothWindMult", (int)g_windMult);
}
void ClothPhys_ResetDefaults() {
    g_windBoost = 1;
    g_windBase  = 400.0f;
    g_windMult  = 200.0f;
}

void ClothPhys_Install() {
    g_fnameToStr = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR);
    if (!g_fnameToStr)
        TwkLog("[cloth] FName::ToString not found -- dump will show pointers without names");
    uint8_t* t = TwkScanExe(SIG_MPCI_SET_SCALAR);
    if (t && MH_CreateHook(t, (void*)&hkSetScalar, (void**)&g_origSetScalar) == MH_OK &&
        MH_EnableHook(t) == MH_OK) {
        TwkLog("[cloth] wind boost hooked SetScalarParameterValue @ %p (boost %s)", t,
               g_windBoost ? "ON" : "off");
    } else {
        g_origSetScalar = nullptr;
        TwkLog("[cloth] SetScalarParameterValue not hooked -- wind boost off (dump still works)");
    }
    TwkLog("[cloth] recon probe ready (F1 -> Session Tweaks -> Dump cloth/material recon)");
}

void ClothPhys_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    bool b = g_windBoost != 0;
    if (api->Checkbox && api->Checkbox("Cloth wind boost", &b)) { g_windBoost = b ? 1 : 0; TwkMarkDirty(); }
    float v = g_windBase;
    if (api->SliderFloat && api->SliderFloat("Breeze at standstill", &v, 0.0f, 2000.0f, "%.0f")) {
        g_windBase = v; TwkMarkDirty();
    }
    v = g_windMult;
    if (api->SliderFloat && api->SliderFloat("Wind vs speed (%)", &v, 0.0f, 1000.0f, "%.0f")) {
        g_windMult = v; TwkMarkDirty();
    }
    if (g_origSetScalar)
        api->TextDisabled(g_windIdDone ? "wind writer: found (MPC_ClothWind)"
                                       : "wind writer: waiting for the game's first wind write");
    if (api->version >= 2 && api->Button && api->Button("Dump cloth/material recon"))
        InterlockedExchange(&g_dumpReq, 1);
    api->TextDisabled(g_status);
}

void ClothPhys_PumpFrame() {
    windPump();
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
