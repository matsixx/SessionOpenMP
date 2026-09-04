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
#include "cosmetics.h"
#include "../ui/mp_name.h"
#include "../debug.h"
#include "game_syms.h"
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game {

// On a faulting rebuild, probe an empty profile and then each item alone, so the log names the item
// that broke it. Costs several RefreshVisuals calls, but only on a path that is already failing --
// which is why it stays on permanently: an item this install cannot wear reports itself.
bool g_bisectOnFault = true;

#ifdef _WIN32

static void* rdPtr(void* b, int o) {
    if (!b) return nullptr;
    __try { return *(void**)((uint8_t*)b + o); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- the profile TMaps ------------------------------------------------------------------------------
// EVERY offset below was read out of `TMapBase<int,FCustomizationProfileItem>::Emplace`'s own
// instructions (Epic 0x7dbb60), not assumed: it indexes elements with `imul rcx, rdi, 0x1c`, reads
// FirstFreeIndex/NumFreeIndices at [rcx+0x30]/[rcx+0x34], takes the bit array at rcx+0x10, and stores
// `mov [r8],eax` (key) / `movups [r8+4],xmm0` (the 16-byte value) / `mov [r8+0x14],-1` (HashNextId).
//   element: +0x00 int32 Key | +0x04 FCustomizationProfileItem(16) | +0x14 HashNextId | +0x18 HashIndex
//   TSparseArray: +0x00 Data | +0x08 Num | +0x0c Max | +0x10 AllocFlags(TBitArray) | +0x30 FirstFree
//   TBitArray: inline words at +0x10, secondary ptr at +0x20 (used once it outgrows 128 bits), NumBits +0x28
// This layout is only ever READ. Writes go through the element's 16-byte VALUE, whose position is fixed
// by the same instruction that proves the stride -- no key, no hash link, no allocation is ever touched,
// so a mistake here cannot corrupt the map.
struct MapWalk {
    uint8_t* data = nullptr;
    int      num = 0;
    const uint32_t* bits = nullptr;
    int      numBits = 0;
};
static const int kElemStride = 0x1c;
static const int kElemValue  = 0x04;     // FCustomizationProfileItem inside the element
static const int kItemSize   = 16;

static bool mapOpen(void* map, MapWalk& w) {
    if (!map) return false;
    __try {
        w.data    = *(uint8_t**)((uint8_t*)map + 0x00);
        w.num     = *(int32_t*)((uint8_t*)map + 0x08);
        w.numBits = *(int32_t*)((uint8_t*)map + 0x28);
        void* sec = *(void**)((uint8_t*)map + 0x20);
        w.bits    = (const uint32_t*)(sec ? sec : ((uint8_t*)map + 0x10));
        // An EMPTY map is VALID, and its data pointer is legitimately null -- a PRO skater's clothing
        // map is exactly this, and rejecting it as unreadable is what kept a pro user's client from
        // ever reaching the insert pass (every peer in underwear). Zero elements walk as a no-op.
        if (w.num == 0) { w.numBits = 0; return true; }
        // Sanity: a plausible map or nothing. Bad numbers mean the layout belief is wrong, and the
        // caller logs that rather than walking into the weeds.
        if (!w.data || w.num < 0 || w.num > 4096 || w.numBits < w.num) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool mapAlive(const MapWalk& w, int i) {
    __try { return (w.bits[i >> 5] >> (i & 31)) & 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static uint8_t* mapElem(const MapWalk& w, int i) { return w.data + (size_t)i * kElemStride; }

// ---- FName <-> string -------------------------------------------------------------------------------
// FName indices are per-process, so only the STRING travels. Turning a peer's string back into a LOCAL
// FName needs no FName constructor: find the item definition asset by that name and copy its own
// NamePrivate. A null result IS "this item is not installed here".
// THE CACHE IS NOT AN OPTIMISATION, IT IS A LEAK FIX. `FName::ToString` hands back a GAME-ALLOCATED
// FString that cannot be freed here (freeing needs the game's allocator, which the mod deliberately
// does not couple to), and this runs for EVERY item on EVERY sample -- roughly 8 allocations a second,
// leaked for the life of the process. An FName's text never changes, so one conversion per distinct
// name is both correct and permanent, and it makes sampling cheap enough to notice a wardrobe change
// quickly without spending a byte on the wire.
// Set when the last conversion did not fit. A CUT NAME IS A SILENT FAILURE otherwise: it looks
// like a perfectly good name, travels, and then simply never resolves anywhere -- the item
// vanishes off the peer with nothing in the log to follow. This function has no logger; its
// caller does, so the fact is left here to be picked up.
static bool g_nameTruncated = false;
static bool fnameToString(const void* fnamePtr, char* out, int cap) {
    g_nameTruncated = false;
    out[0] = 0;
    const Syms& S = Get();
    if (!S.FNameToString || !fnamePtr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fnamePtr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;                                  // NAME_None: nothing to convert

    struct Entry { uint64_t key; char name[64]; };   // sized WITH CosmeticItem::name -- a shorter
                                                     // cache would re-introduce the truncation
    static Entry cache[128];
    static int   nCached = 0;
    for (int i = 0; i < nCached; i++) {
        if (cache[i].key != key) continue;
        strncpy_s(out, (size_t)cap, cache[i].name, _TRUNCATE);
        return out[0] != 0;
    }
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fnamePtr, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;                       // the FString itself is still leaked -- ONCE per distinct name
        if (k >= cap - 1 && fs.n > k) g_nameTruncated = true;
        if (k > 0 && nCached < (int)(sizeof(cache)/sizeof(cache[0]))) {
            cache[nCached].key = key;
            strncpy_s(cache[nCached].name, sizeof(cache[nCached].name), out, _TRUNCATE);
            nCached++;
        }
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
static void* findByName(const char* name) {
    const Syms& S = Get();
    if (!S.StaticFindObject || !name || !name[0]) return nullptr;
    wchar_t wide[80];
    int i = 0;
    for (; name[i] && i < (int)(sizeof(wide)/sizeof(wide[0])) - 1; i++) wide[i] = (wchar_t)(uint8_t)name[i];
    wide[i] = 0;
    __try { return S.StaticFindObject(nullptr, (void*)(intptr_t)-1, wide, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// The 8-byte FName of an object, usable directly as a profile item's ItemName.
static bool fnameOf(const char* name, void* out8) {
    void* obj = findByName(name);
    if (!obj) return false;
    __try { memcpy(out8, (uint8_t*)obj + off::kObjNamePrivate, 8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the BASE BODY definition -----------------------------------------------------------------------
// The ASSET-NAME TAIL of a soft path's FName ("/Game/Path/Package.AssetName" -> "AssetName"), cached by
// FName value like fnameToString -- but with its OWN cache: fnameToString's 48-char entries would
// truncate a long asset PATH, and the tail lives at the truncated end, so a cache hit there would
// return a path whose tail is gone. Tails themselves are short, so caching only the tail is exact.
static bool softPathAssetTail(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    const Syms& S = Get();
    if (!S.FNameToString || !fnamePtr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fnamePtr; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;
    struct Entry { uint64_t key; char tail[48]; };
    static Entry cache[32];
    static int   nCached = 0;
    for (int i = 0; i < nCached; i++) {
        if (cache[i].key != key) continue;
        strncpy_s(out, (size_t)cap, cache[i].tail, _TRUNCATE);
        return out[0] != 0;
    }
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fnamePtr, &fs);                  // FString leaked ONCE per distinct path name
        if (!fs.d || fs.n <= 0) return false;
        int tail = 0;
        for (int k = 0; k < fs.n && fs.d[k]; k++) if (fs.d[k] == L'.' || fs.d[k] == L'/') tail = k + 1;
        int k = 0;
        for (; tail + k < fs.n && k < cap - 1 && fs.d[tail + k]; k++)
            out[k] = (char)(fs.d[tail + k] < 128 ? fs.d[tail + k] : '?');
        out[k] = 0;
        if (k > 0 && nCached < (int)(sizeof(cache)/sizeof(cache[0]))) {
            cache[nCached].key = key;
            strncpy_s(cache[nCached].tail, sizeof(cache[nCached].tail), out, _TRUNCATE);
            nCached++;
        }
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
static bool nameEqNoCase(const char* a, const char* b) {
    for (;; a++, b++) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}
// Resolve a peer's body-definition NAME to a local USkaterVisualsDefinition. NOT findByName:
// StaticFindObject(ANY_PACKAGE) returns the first object of ANY class with that name, and a
// wrong-typed pointer where the rebuild expects exactly this type is an AV -- which is why the body
// stayed local until now. The game instance's own `_skaterDefinitions` array IS the type gate (its
// declared element type is TSoftObjectPtr<USkaterVisualsDefinition>), and TryLoad on the MATCHING
// path both loads a never-resident body asset (a pro/female model this player has never equipped)
// and proves it loaded -- residency is never inferred (the garment-mesh lesson). Names are compared
// FIRST so only the one matching asset is ever loaded, not the whole roster.
static void* resolveVisualDef(void* gi, const char* name) {
    const Syms& S = Get();
    if (!gi || !name || !name[0]) return nullptr;
    __try {
        uint8_t* arr = (uint8_t*)gi + off::kGiSkaterDefs;
        uint8_t* d = *(uint8_t**)arr;
        const int32_t n = *(int32_t*)(arr + 8);
        if (d && n > 0 && n <= 64 && S.SoftPathTryLoad) {
            for (int i = 0; i < n; i++) {
                uint8_t* path = d + (size_t)i * off::kSoftPtrSize + off::kSoftPathInPtr;
                if (*(uint64_t*)path == 0) continue;             // empty slot
                char tail[48];
                if (!softPathAssetTail(path, tail, sizeof(tail)) || !nameEqNoCase(tail, name)) continue;
                void* obj = S.SoftPathTryLoad(path, nullptr);
                if (obj) return obj;                             // typed by the array's declaration
            }
        }
        // Fallbacks: the two definitions already resident on the instance, matched by OBJECT name --
        // covers a name the roster array does not carry but the local player happens to be using.
        void* cand[2] = { *(void**)((uint8_t*)gi + off::kGiDefaultVisualDef),
                          *(void**)((uint8_t*)gi + off::kGiSkaterInstance + off::kSkInstVisualDef) };
        for (int i = 0; i < 2; i++) {
            char nm[64];
            if (cand[i] && fnameToString((uint8_t*)cand[i] + off::kObjNamePrivate, nm, sizeof(nm))
                && nameEqNoCase(nm, name))
                return cand[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// THE ASSET GATE. Two independent nulls have to be closed here, or the rebuild crashes.
// (1) Having an FName is NOT the same as this install HAVING the item: the game resolves a profile
// item through its own catalog (`GetCustomizationItem`), which returns NULL for anything this copy
// does not have -- a DLC or mod item the peer owns and we do not. Nothing downstream checks that null;
// the rebuild walks it into `MapItemMaterials` and dereferences it. Vanilla can never hit it, because
// the menu only offers items you own. An item that does not resolve here is never written, exactly
// like a null-asset anim gate.
// (2) The catalog check ALONE is not enough -- the second null is the item's SKELETAL MESH. The
// rebuild reaches it through FSoftObjectPath::ResolveObject, which returns null for anything not
// already in memory: the local player's garments were streamed in when their own profile was applied,
// a PEER's never were, and nothing loads them, so MapItemMaterials walks USkeletalMesh::Materials
// (+0xd8) through a null. So the item's meshes are LOADED here (TryLoad, which returns the object)
// before the profile is handed to the game, and a non-empty path that will not load means the item is
// not worn. Nothing is written unless its mesh is proven resident.
static bool itemUsableHere(const void* fname8, const char** whyOut, void (*logf)(const char*)) {
    const Syms& S = Get();
    // FAILS CLOSED. The catalog IS the type check on this path -- a typed lookup that returns null for
    // anything that is not a customization item -- so without it there is no check at all and every
    // name a peer sends would be accepted whole. The safe answer to "I cannot verify this" is no. Said
    // once, loudly, because it disables peer cosmetics entirely.
    if (!S.GetCustomizationItem) {
        static bool said = false;
        if (!said) {
            said = true;
            if (logf) logf("[cosmetics] *** GetCustomizationItem unresolved -- peer items cannot be "
                           "verified, so none will be worn (a sig needs fixing)");
        }
        if (whyOut) *whyOut = "item catalog unavailable -- cannot verify";
        return false;
    }
    void* def = nullptr;
    __try { def = S.GetCustomizationItem(fname8); } __except (EXCEPTION_EXECUTE_HANDLER) { def = nullptr; }
    if (!def) { if (whyOut) *whyOut = "not in this install's item catalog"; return false; }
    if (!S.SoftPathTryLoad) { if (whyOut) *whyOut = ""; return true; }        // no loader: catalog only
    int entries = 0, paths = 0, loaded = 0, failed = 0, matFailed = 0;
    __try {
        uint8_t* data = *(uint8_t**)((uint8_t*)def + off::kItemDefMeshes);
        const int32_t num = *(int32_t*)((uint8_t*)def + off::kItemDefMeshes + 8);
        if (!data || num <= 0 || num > 64) { if (whyOut) *whyOut = ""; return true; }  // nothing to load
        entries = num;
        // The MATERIALS (and alpha masks) are soft pointers as well, and a garment whose material never
        // resolved renders as the engine's fallback (a branded shirt shows up plain). Same rule as the
        // mesh: the rebuild only RESOLVES, so anything it will need must already be resident. A
        // material that will not load is NOT fatal -- the mesh still renders, just plainly -- so these
        // are loaded but never gate the item.
        auto loadSoft = [&](uint8_t* soft) {
            uint8_t* path = soft + off::kSoftPathInPtr;
            if (*(uint64_t*)path == 0) return;                    // empty slot
            paths++;
            if (S.SoftPathTryLoad(path, nullptr)) loaded++; else matFailed++;
        };
        auto loadSoftArray = [&](uint8_t* arr, int stride, int softOff) {
            uint8_t* d = *(uint8_t**)arr;
            const int32_t n = *(int32_t*)(arr + 8);
            if (!d || n <= 0 || n > 64) return;
            for (int j = 0; j < n; j++) loadSoft(d + (size_t)j * stride + softOff);
        };
        for (int i = 0; i < num; i++) {
            uint8_t* entry = data + (size_t)i * off::kItemMeshStride;
            // materials for this mesh entry: the base, its variants, and the female equivalents
            { uint8_t* mats = entry + off::kItemMeshMaterials;
              uint8_t* md = *(uint8_t**)mats;
              const int32_t mn = *(int32_t*)(mats + 8);
              if (md && mn > 0 && mn <= 64) {
                  for (int m = 0; m < mn; m++) {
                      uint8_t* mat = md + (size_t)m * off::kItemMatStride;
                      loadSoft(mat + off::kItemMatSoft);
                      loadSoft(mat + off::kItemMatSoftAFXX);
                      loadSoftArray(mat + off::kItemMatVariants,   off::kItemMatVarStride, off::kItemMatVarSoft);
                      loadSoftArray(mat + off::kItemMatVariantsAF, off::kItemMatVarStride, off::kItemMatVarSoft);
                  }
              } }
            for (int k = 0; k < (int)(sizeof(off::kItemMeshSoftOffs)/sizeof(int)); k++) {
                uint8_t* soft = entry + off::kItemMeshSoftOffs[k];
                uint8_t* path = soft + off::kSoftPathInPtr;
                if (*(uint64_t*)path == 0) continue;              // empty slot (e.g. the other gender)
                paths++;
                // NEVER INFER RESIDENCY FROM THE CACHED FWeakObjectPtr. A weak pointer whose object
                // has been GARBAGE COLLECTED keeps its non-zero ObjectIndex while Get() returns null,
                // which is exactly the state of an item loaded once while browsing the menu and
                // collected since -- i.e. every item the local player is not currently wearing. Just
                // load: TryLoad on an already-loaded asset resolves and returns it cheaply.
                if (S.SoftPathTryLoad(path, nullptr)) loaded++;
                else                                  failed++;
            }
        }
        // the definition's own alpha masks (they carve the body under clothing)
        loadSoftArray((uint8_t*)def + off::kItemDefAlphaMasks,  off::kSoftPtrSize, 0);
        loadSoftArray((uint8_t*)def + off::kItemDefAlphaMaskAF, off::kSoftPtrSize, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (whyOut) *whyOut = "faulted while loading its meshes";
        return false;
    }
    // Report only what is worth knowing: a failure, or everything while diagnosing.
    if (logf && (failed || matFailed || debug::Get().cosmeticsLoad)) {
        char m[160];
        snprintf(m, sizeof(m), "[cosmetics]   load: meshEntries=%d paths=%d loaded=%d meshFailed=%d"
                               " matFailed=%d", entries, paths, loaded, failed, matFailed);
        logf(m);
    }
    if (failed) { if (whyOut) *whyOut = "one of its mesh assets would not load"; return false; }
    if (whyOut) *whyOut = "";
    return true;
}

static void* gameInstanceOf(void* actor) {
    const Syms& S = Get();
    if (!S.GetGameInstance || !actor) return nullptr;
    __try { return S.GetGameInstance(actor); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- the INVENTORY INSTANCE ---------------------------------------------------------------------
// Colour, pattern, variant and sock height are NOT in the profile -- they hang off the inventory
// instance that `UsedInstanceId` selects, in whoever's inventory is being read, which on a receiver is
// the LOCAL wardrobe. A transported index alone therefore renders a peer's colours as the local
// player's. Both ends reach the instance the same way the game does, and the VALUES are transported.
// Layout proven by `FCustomizationInventoryPersistentData::GetInventoryItem`'s own instructions -- see
// game_syms.h. The colour map's stride is the one thing NOT read out of an instruction, so its walker
// validates every key before trusting it.
static void* instanceFor(void* ownerActor, const void* fname8, uint8_t instIdx) {
    void* gi = gameInstanceOf(ownerActor);
    if (!gi) return nullptr;
    __try {
        void* profile = *(void**)((uint8_t*)gi + off::kGiPlayerProfile);
        if (!profile) return nullptr;
        uint8_t* map = (uint8_t*)profile + off::kProfileInventory + off::kInvItemsMap;
        MapWalk w;
        if (!mapOpen(map, w)) return nullptr;
        const uint64_t want = *(const uint64_t*)fname8;
        for (int i = 0; i < w.num; i++) {
            if (!mapAlive(w, i)) continue;
            uint8_t* e = w.data + (size_t)i * off::kInvElemStride;
            if (*(uint64_t*)e != want) continue;                       // FName key, 8 bytes
            uint8_t* list = e + off::kInvElemInstances;                // InstanceList TArray
            uint8_t* data = *(uint8_t**)list;
            const int32_t num = *(int32_t*)(list + 8);
            if (!data || instIdx >= num) return nullptr;
            return data + (size_t)instIdx * off::kInvInstanceStride;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
// A colour-map element is {FString key, {bool, FLinearColor} value}. The FString is validated (sane
// Num/Max, printable first char) before the element is believed -- if the stride assumption is wrong
// this fails on the first entry and reports it rather than scribbling on the player's wardrobe.
static bool colorKeyOf(uint8_t* elem, char* out, int cap) {
    __try {
        wchar_t* d   = *(wchar_t**)(elem + 0x00);
        const int32_t n = *(int32_t*)(elem + 0x08);
        const int32_t m = *(int32_t*)(elem + 0x0c);
        if (!d || n <= 1 || n > 128 || m < n) return false;
        if (d[0] < 32 || d[0] > 126) return false;
        int k = 0;
        for (; k < n - 1 && k < cap - 1 && d[k]; k++) out[k] = (char)(d[k] < 128 ? d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void readInstanceAttribs(void* inst, repl::CosmeticItem& out, void (*logf)(const char*)) {
    if (!inst) return;
    __try {
        out.instVariant = *(int32_t*)((uint8_t*)inst + off::kInstVariantId);
        out.sockHeight  = *(uint8_t*)((uint8_t*)inst + off::kInstSockHeight);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    MapWalk w;
    if (!mapOpen((uint8_t*)inst + off::kInstColorOverrides, w)) return;
    const int cap = (int)(sizeof(out.colors)/sizeof(out.colors[0]));
    for (int i = 0; i < w.num && out.nColors < cap; i++) {
        if (!mapAlive(w, i)) continue;
        uint8_t* e = w.data + (size_t)i * off::kColorElemStride;
        repl::CosmeticColor c{};
        if (!colorKeyOf(e, c.key, sizeof(c.key))) {
            static bool said = false;
            if (!said && logf) { said = true;
                logf("[cosmetics] colour map layout not recognised -- colours will not sync (harmless)"); }
            return;                                   // never write through an unvalidated layout
        }
        __try {
            uint8_t* v = e + off::kColorValueOff;
            c.enabled = *(uint8_t*)(v + off::kColorEnabledOff);
            memcpy(c.rgba, v + off::kColorRgbaOff, 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        out.colors[out.nColors++] = c;
    }
}
// Overwrite an instance's attributes with a peer's, remembering the originals. Colours are matched by
// KEY: an existing slot may be overwritten but none is ever inserted, so a colour on a slot this
// player has never customised is reported rather than silently dropped.
struct SavedInstance {
    void*   inst = nullptr;
    int32_t variantId = 0;
    uint8_t sockHeight = 0;
    int     nColors = 0;
    uint8_t* elems[8] = {};
    uint8_t  values[8][20] = {};
};
static void wearInstanceAttribs(void* inst, const repl::CosmeticItem& it, SavedInstance& sv,
                                int* unmatched) {
    if (!inst) return;
    sv.inst = inst;
    __try {
        sv.variantId  = *(int32_t*)((uint8_t*)inst + off::kInstVariantId);
        sv.sockHeight = *(uint8_t*)((uint8_t*)inst + off::kInstSockHeight);
        *(int32_t*)((uint8_t*)inst + off::kInstVariantId) = it.instVariant;
        *(uint8_t*)((uint8_t*)inst + off::kInstSockHeight) = it.sockHeight;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!it.nColors) return;
    MapWalk w;
    if (!mapOpen((uint8_t*)inst + off::kInstColorOverrides, w)) return;
    for (int i = 0; i < it.nColors; i++) {
        const repl::CosmeticColor& c = it.colors[i];
        bool matched = false;
        for (int k = 0; k < w.num && !matched; k++) {
            if (!mapAlive(w, k)) continue;
            uint8_t* e = w.data + (size_t)k * off::kColorElemStride;
            char key[24];
            if (!colorKeyOf(e, key, sizeof(key))) return;      // unvalidated layout: stop, change nothing
            if (strcmp(key, c.key) != 0) continue;
            matched = true;
            if (sv.nColors < (int)(sizeof(sv.elems)/sizeof(sv.elems[0]))) {
                uint8_t* v = e + off::kColorValueOff;
                __try {
                    memcpy(sv.values[sv.nColors], v, 20);
                    sv.elems[sv.nColors] = v;
                    sv.nColors++;
                    *(uint8_t*)(v + off::kColorEnabledOff) = c.enabled;
                    memcpy(v + off::kColorRgbaOff, c.rgba, 16);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        if (!matched && unmatched) (*unmatched)++;
    }
}
static void restoreInstance(const SavedInstance& sv) {
    if (!sv.inst) return;
    __try {
        *(int32_t*)((uint8_t*)sv.inst + off::kInstVariantId) = sv.variantId;
        *(uint8_t*)((uint8_t*)sv.inst + off::kInstSockHeight) = sv.sockHeight;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < sv.nColors; i++) {
        __try { memcpy(sv.elems[i], sv.values[i], 20); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- gather ------------------------------------------------------------------------------------------
static bool readMapInto(void* map, repl::CosmeticItem* dst, int cap, uint8_t& count,
                        void* ownPawn, void (*logf)(const char*)) {
    count = 0;
    MapWalk w;
    if (!mapOpen(map, w)) return false;
    for (int i = 0; i < w.num && count < cap; i++) {
        if (!mapAlive(w, i)) continue;
        uint8_t* e = mapElem(w, i);
        repl::CosmeticItem it{};
        __try {
            it.cat     = *(int32_t*)(e + 0x00);
            it.inst    = *(uint8_t*)(e + kElemValue + 0x08);
            it.variant = *(int32_t*)(e + kElemValue + 0x0c);
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        fnameToString(e + kElemValue, it.name, sizeof(it.name));   // ItemName is the value's first field
        if (g_nameTruncated && logf) {
            char m[220];
            snprintf(m, sizeof(m), "[cosmetics] item name TRUNCATED ('%s') -- it cannot resolve "
                                   "on any client, including this one; the name buffer is too "
                                   "small for it", it.name);
            logf(m);
        }
        // A cleared slot (empty ItemName) is not a worn item -- receivers already treat it as
        // absent, and a dress restore leaves cleared keys behind, which would otherwise flap the
        // own-look change detector forever (an extra "item" appearing after every dress).
        if (!it.name[0]) continue;
        // the attributes that decide how it LOOKS live on the inventory instance, not the profile
        readInstanceAttribs(instanceFor(ownPawn, e + kElemValue, it.inst), it, logf);
        dst[count++] = it;
    }
    return true;
}

bool GatherOwnCosmetics(void* ownPawn, repl::CosmeticSet& out, void (*logf)(const char*)) {
    // memset, not `= CosmeticSet{}`: the caller decides "did my look change?" with memcmp, and value
    // initialisation says nothing about PADDING bytes, so stack garbage there makes every check report
    // a change and re-publish forever. Zeroing the whole object makes the comparison mean what it says.
    memset(&out, 0, sizeof(out));
    void* gi = gameInstanceOf(ownPawn);
    if (!gi) return false;
    uint8_t* inst = (uint8_t*)gi + off::kGiSkaterInstance;
    __try {
        out.stance = *(uint8_t*)(inst + off::kSkInstProfile + off::kProfStance);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // The name BROADCAST here is the multiplayer name, NOT the skater name off the game instance. Two
    // reasons: the in-game skater name is a private profile detail that should take an explicit choice
    // to publish, and the multiplayer one has been through the word filter (mp_name.h).
    strncpy_s(out.skaterName, MpName_Get(), _TRUNCATE);
    // Which map this player is on, so the roster can show it. The INTERNAL level name -- every receiver
    // resolves it to its own pretty label, as with every other transported name.
    LocalMapName(ownPawn, out.mapName, sizeof(out.mapName));
    { void* vd = rdPtr(inst, off::kSkInstVisualDef);
      if (vd) fnameToString((uint8_t*)vd + off::kObjNamePrivate, out.visualDef, sizeof(out.visualDef)); }

    uint8_t* prof = inst + off::kSkInstProfile;
    const bool okC = readMapInto(prof + off::kProfCharItems,  out.chr, 24, out.nChar, ownPawn, logf);
    const bool okB = readMapInto(prof + off::kProfBoardItems, out.brd, 16, out.nBoard, ownPawn, logf);
    out.modDigest = LocalModDigest();

    // SELF-CHECK, once: the walk is the one part of this feature built on a memory layout, so it says
    // out loud what it found. Zero items, or nonsense, means the layout belief is wrong -- and since
    // gather is read-only, the cost of that is a wrong LOG LINE, not a wrecked profile.
    if (logf) {
        static bool said = false;
        if (!said) {
            said = true;
            char m[240];
            snprintf(m, sizeof(m), "[cosmetics] own look: skater='%s' visuals='%s' stance=%d "
                                   "clothing=%d board=%d (maps %s/%s) modDigest=%016llX",
                     out.skaterName, out.visualDef, (int)out.stance, (int)out.nChar, (int)out.nBoard,
                     okC ? "ok" : "UNREADABLE", okB ? "ok" : "UNREADABLE",
                     (unsigned long long)out.modDigest);
            logf(m);
            for (int i = 0; i < out.nChar; i++) {
                char l[120];
                snprintf(l, sizeof(l), "[cosmetics]   clothing cat=%d '%s' inst=%d var=%d instVar=%d colours=%d",
                         out.chr[i].cat, out.chr[i].name, (int)out.chr[i].inst, out.chr[i].variant,
                         out.chr[i].instVariant, (int)out.chr[i].nColors);
                logf(l);
            }
            for (int i = 0; i < out.nBoard; i++) {
                char l[120];
                snprintf(l, sizeof(l), "[cosmetics]   board    cat=%d '%s' inst=%d var=%d instVar=%d colours=%d",
                         out.brd[i].cat, out.brd[i].name, (int)out.brd[i].inst, out.brd[i].variant,
                         out.brd[i].instVariant, (int)out.brd[i].nColors);
                logf(l);
            }
        }
    }
    return okC || okB;
}

// ---- dress -------------------------------------------------------------------------------------------
// Borrow the global, refresh, put it back. Everything saved is restored on every exit path.
// The category KEY is saved beside the value so the restore can go BY KEY (see restoreMap).
struct SavedItem { int32_t cat = 0; uint8_t bytes[kItemSize]; };

static const repl::CosmeticItem* findCat(const repl::CosmeticItem* arr, int n, int32_t cat) {
    for (int i = 0; i < n; i++) if (arr[i].cat == cat) return &arr[i];
    return nullptr;
}

// Overwrite every live element's 16-byte VALUE from the peer's set; remember the originals.
// A category the peer did not send is CLEARED (an empty FName), never left showing the local item --
// otherwise a proxy silently wears the local player's shirt and it looks like sync working.
static int wearMap(void* map, const repl::CosmeticItem* items, int n,
                   SavedItem* saved, int savedCap, int* savedCount, int* unresolved,
                   void (*logf)(const char*), void* proxyActor = nullptr,
                   SavedInstance* savedInst = nullptr, int* savedInstCount = nullptr,
                   int savedInstCap = 0, int* unmatchedColors = nullptr,
                   int32_t* insertedCats = nullptr, int insertedCap = 0, int* insertedCount = nullptr) {
    MapWalk w;
    if (!mapOpen(map, w)) return 0;
    int worn = 0;
    int32_t seenCats[64]; int nSeen = 0; bool seenOverflow = false;
    for (int i = 0; i < w.num; i++) {
        if (!mapAlive(w, i)) continue;
        uint8_t* e = mapElem(w, i);
        int32_t cat = 0;
        __try { cat = *(int32_t*)(e + 0x00); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (nSeen < (int)(sizeof(seenCats)/sizeof(seenCats[0]))) seenCats[nSeen++] = cat;
        else seenOverflow = true;                           // cannot prove a cat unseen: no inserts
        if (*savedCount >= savedCap) break;                 // out of restore room: stop, never overwrite
        __try { saved[*savedCount].cat = cat;
                memcpy(saved[*savedCount].bytes, e + kElemValue, kItemSize); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        (*savedCount)++;
        uint8_t nv[kItemSize];
        memset(nv, 0, sizeof(nv));                          // default = cleared slot (FName None)
        const repl::CosmeticItem* it = findCat(items, n, cat);
        if (it && it->name[0]) {
            const char* why = "name did not resolve";
            if (fnameOf(it->name, nv) && itemUsableHere(nv, &why, logf)) {   // resolved, catalogued, LOADED
                nv[0x08] = it->inst;
                memcpy(nv + 0x0c, &it->variant, 4);
                worn++;
                // and their colour/variant/sock height, which live on the LOCAL instance of the item
                if (savedInst && savedInstCount && *savedInstCount < savedInstCap) {
                    void* inst = instanceFor(proxyActor, nv, it->inst);
                    if (inst) wearInstanceAttribs(inst, *it, savedInst[(*savedInstCount)++],
                                                  unmatchedColors);
                }
            } else {
                memset(nv, 0, sizeof(nv));                  // half-built name must not reach the game
                if (unresolved) (*unresolved)++;            // leave the slot empty rather than crash
                // Name the item AND the reason: "N items missing" does not say which ones, and "not
                // installed" is a different fault from "installed but not loaded".
                if (logf) { char m[180];
                    snprintf(m, sizeof(m), "[cosmetics]   '%s' (cat=%d) skipped -- %s",
                             it->name, (int)cat, (why && *why) ? why : "unusable here");
                    logf(m); }
            }
        }
        __try { memcpy(e + kElemValue, nv, kItemSize); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // ---- categories the LOCAL profile has no ELEMENT for. The walk above can only OVERWRITE live
    // elements, and a profile only carries elements for what its OWN player wears -- so a PRO
    // skater's EMPTY clothing map could dress nobody (every peer in underwear, the field symptom),
    // and even a normal profile silently dropped a peer category the local player wears nothing in.
    // The game's own Emplace inserts the missing slot (allocation + hashing are its logic, never
    // ours); each inserted KEY is recorded so the restore can clear it -- there is no sanctioned
    // "remove key", and a cleared item is the exact state every dress already writes into categories
    // a peer did not send.
    {
        const Syms& S = Get();
        if (S.ProfileEmplace && insertedCats && insertedCount && !seenOverflow) {
            for (int i = 0; i < n; i++) {
                const repl::CosmeticItem& it = items[i];
                if (!it.name[0]) continue;
                bool seen = false;
                for (int k = 0; k < nSeen && !seen; k++) seen = (seenCats[k] == it.cat);
                if (seen) continue;
                if (*insertedCount >= insertedCap) break;
                uint8_t nv[kItemSize];
                memset(nv, 0, sizeof(nv));
                const char* why = "name did not resolve";
                if (fnameOf(it.name, nv) && itemUsableHere(nv, &why, logf)) {
                    nv[0x08] = it.inst;
                    memcpy(nv + 0x0c, &it.variant, 4);
                    bool ins = false;
                    __try { S.ProfileEmplace(map, &it.cat, nv); ins = true; }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                    if (ins) {
                        insertedCats[(*insertedCount)++] = it.cat;
                        worn++;
                        if (savedInst && savedInstCount && *savedInstCount < savedInstCap) {
                            void* instP = instanceFor(proxyActor, nv, it.inst);
                            if (instP) wearInstanceAttribs(instP, it, savedInst[(*savedInstCount)++],
                                                           unmatchedColors);
                        }
                    }
                } else {
                    if (unresolved) (*unresolved)++;
                    if (logf) { char m[180];
                        snprintf(m, sizeof(m), "[cosmetics]   '%s' (cat=%d) skipped -- %s",
                                 it.name, (int)it.cat, (why && *why) ? why : "unusable here");
                        logf(m); }
                }
            }
        }
    }
    return worn;
}
static void restoreMap(void* map, const SavedItem* saved, int savedCount, int* cursor) {
    // BY KEY when the game's Emplace is available: an insert during the wear phase can fill a
    // free-list HOLE below the originals, and an order-walk would then pair saved values with the
    // wrong elements -- scrambling the local player's own wardrobe. Emplace by key is immune to
    // element order and to the array reallocating under an insert.
    const Syms& S = Get();
    if (S.ProfileEmplace) {
        for (int i = 0; i < savedCount; i++) {
            __try { S.ProfileEmplace(map, &saved[i].cat, saved[i].bytes); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (cursor) *cursor = savedCount;
        return;
    }
    // No Emplace resolved = no inserts happened either, so the order-walk is exact (today's shape).
    MapWalk w;
    if (!mapOpen(map, w)) return;
    for (int i = 0; i < w.num; i++) {
        if (!mapAlive(w, i)) continue;
        if (*cursor >= savedCount) return;
        uint8_t* e = mapElem(w, i);
        __try { memcpy(e + kElemValue, saved[*cursor].bytes, kItemSize); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        (*cursor)++;
    }
}

// ---- the BISECT --------------------------------------------------------------------------------
// Finds the item the game refuses, instead of leaving it to be guessed. Runs only on the fault path,
// once per look, and answers two questions in order:
//   1. does an EMPTY profile rebuild cleanly? (if not, the problem is clearing slots, not any item)
//   2. which single item, applied alone, reproduces the fault?
// SEH makes each probe survivable, so one dress yields a complete answer rather than one bit.
static bool refreshFaults(void* proxy) {
    const Syms& S = Get();
#ifdef _WIN32
    __try { S.RefreshVisuals(proxy); return false; } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
#else
    (void)proxy; return false;
#endif
}
// Write one item (or none) into `map`, clearing every other live slot. OVERWRITE-ONLY on purpose:
// the bisect must not insert keys it has no restore bookkeeping for, so an item whose category has
// no local element simply cannot be probed (it also cannot have been the fault -- the wear phase's
// insert is what would have applied it, and its own SEH already attributes an Emplace fault).
static void wearOnly(void* map, const repl::CosmeticItem* only) {
    MapWalk w;
    if (!mapOpen(map, w)) return;
    for (int i = 0; i < w.num; i++) {
        if (!mapAlive(w, i)) continue;
        uint8_t* e = mapElem(w, i);
        uint8_t nv[kItemSize]; memset(nv, 0, sizeof(nv));
        __try {
            const int32_t cat = *(int32_t*)(e + 0x00);
            // `itemUsableHere` applies here too. This write runs AFTER something already faulted, so
            // skipping it would make it the least validated write in the mod -- a peer's name going
            // straight into the profile and on to RefreshVisuals. An unverifiable name is exactly the
            // kind of item the bisect is looking for, so gating it loses nothing.
            const char* why = nullptr;
            if (only && only->cat == cat && only->name[0] && fnameOf(only->name, nv) &&
                itemUsableHere(nv, &why, nullptr)) {
                nv[0x08] = only->inst;
                memcpy(nv + 0x0c, &only->variant, 4);
            } else {
                memset(nv, 0, sizeof(nv));      // a half-built name must not reach the game
            }
            memcpy(e + kElemValue, nv, kItemSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static void bisectCulprit(void* proxyActor, void* charMap, void* brdMap,
                          const repl::CosmeticSet& c, void (*logf)(const char*)) {
    if (!logf) return;
    logf("[cosmetics] BISECT: the full look faulted -- probing what the game refuses");
    wearOnly(charMap, nullptr); wearOnly(brdMap, nullptr);
    logf(refreshFaults(proxyActor) ? "[cosmetics]   EMPTY profile -> FAULT (clearing slots is the problem,"
                                     " not any single item)"
                                   : "[cosmetics]   EMPTY profile -> ok");
    for (int pass = 0; pass < 2; pass++) {
        const repl::CosmeticItem* arr = pass ? c.brd : c.chr;
        const int n = pass ? (int)c.nBoard : (int)c.nChar;
        void* target = pass ? brdMap : charMap;
        void* other  = pass ? charMap : brdMap;
        for (int i = 0; i < n; i++) {
            wearOnly(other, nullptr);
            wearOnly(target, &arr[i]);
            const bool bad = refreshFaults(proxyActor);
            char m[180];
            snprintf(m, sizeof(m), "[cosmetics]   only %s '%s' (cat=%d inst=%d var=%d) -> %s",
                     pass ? "board" : "clothing", arr[i].name, arr[i].cat, (int)arr[i].inst,
                     arr[i].variant, bad ? "FAULT" : "ok");
            logf(m);
        }
    }
    logf("[cosmetics] BISECT complete");
}

// Kill-switch for the replay bone-cache re-sync (see the end of DressProxy). Off restores the crash.
static bool g_replayBoneRefresh = true;

bool DressProxy(void* proxyActor, const repl::CosmeticSet& c, int* unresolved, void (*logf)(const char*)) {
    const Syms& S = Get();
    if (!proxyActor || !S.RefreshVisuals) return false;
    void* gi = gameInstanceOf(proxyActor);
    if (!gi) return false;
    if (unresolved) *unresolved = 0;

    uint8_t* inst = (uint8_t*)gi + off::kGiSkaterInstance;
    uint8_t* prof = inst + off::kSkInstProfile;
    void* charMap = prof + off::kProfCharItems;
    void* brdMap  = prof + off::kProfBoardItems;

    // ---- save the scalars
    uint8_t  savedName[8], savedStance = 0;
    void*    savedVisualDef = nullptr;
    __try {
        memcpy(savedName, inst + off::kSkInstName, 8);
        savedVisualDef = *(void**)(inst + off::kSkInstVisualDef);
        savedStance    = *(uint8_t*)(prof + off::kProfStance);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // ---- wear theirs
    SavedItem savedC[48], savedB[48];
    int nSavedC = 0, nSavedB = 0;
    // the inventory instances borrowed alongside the profile (colours/variant/sock height)
    SavedInstance savedInst[40];
    int nSavedInst = 0, unmatchedColors = 0;
    // categories INSERTED into the borrowed maps (no local element existed) -- cleared on restore
    int32_t insC[24], insB[16];
    int nInsC = 0, nInsB = 0;
    const int wornC = wearMap(charMap, c.chr, c.nChar, savedC, 48, &nSavedC, unresolved, logf,
                              proxyActor, savedInst, &nSavedInst, 40, &unmatchedColors,
                              insC, 24, &nInsC);
    const int wornB = wearMap(brdMap,  c.brd, c.nBoard, savedB, 48, &nSavedB, unresolved, logf,
                              proxyActor, savedInst, &nSavedInst, 40, &unmatchedColors,
                              insB, 16, &nInsB);
    // ---- the BASE BODY. The rebuild takes the definition from the ACTOR (RefreshVisuals passes
    // skater+0x570 into BuildCharacterMesh), so the peer's body is written THERE -- their proxy's own
    // field, semantically correct, no borrow needed -- plus the instance pointer inside the borrow
    // window, for any callee that reads the instance instead. resolveVisualDef goes through the game
    // instance's _skaterDefinitions soft array, whose declared element type is the type gate a bare
    // name-first-match could never provide (the reason this stayed local until now). SkaterName still
    // stays local. Both writes land BEFORE the one RefreshVisuals call so a single rebuild carries
    // body and clothes together. TryLoad on a first-seen pro/female body is a synchronous load -- a
    // one-time hitch on that dress is expected, not a defect.
    void* peerBodyDef  = nullptr;
    void* savedActorDef = nullptr;
    bool  actorDefWritten = false;
    if (c.visualDef[0]) {
        peerBodyDef = resolveVisualDef(gi, c.visualDef);
        if (peerBodyDef) {
            __try {
                // Compared against the ACTOR's current definition, not the instance's: the actor
                // KEEPS a previously-applied peer body across dresses, so "peer switched back to the
                // body I also use" must still write -- an instance-compare would skip it and leave
                // the proxy stuck in the old body.
                savedActorDef = *(void**)((uint8_t*)proxyActor + off::kSkaterVisualDef);
                if (peerBodyDef != savedActorDef) {
                    *(void**)((uint8_t*)proxyActor + off::kSkaterVisualDef) = peerBodyDef;
                    actorDefWritten = true;
                    if (logf) { char m[140];
                        snprintf(m, sizeof(m), "[cosmetics] body definition '%s' applied", c.visualDef);
                        logf(m); }
                }
                *(void**)(inst + off::kSkInstVisualDef) = peerBodyDef;   // restored with the borrow
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        } else if (logf) {
            char m[140];
            snprintf(m, sizeof(m), "[cosmetics] body definition '%s' not available here (kept local)",
                     c.visualDef);
            logf(m);
        }
    }
    __try { *(uint8_t*)(prof + off::kProfStance) = c.stance; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // ---- the one call that reads all of it
    // A caught fault here is NOT recovery -- the rebuild died partway and the proxy keeps whatever it
    // was constructed with. So the handler NAMES the fault: code, address, and the exe-relative
    // offset, which `pdbsym.py addr <offset>` turns straight into a function name.
    bool ok = true;
    unsigned long xcode = 0; void* xaddr = nullptr;
    __try { S.RefreshVisuals(proxyActor); }
    __except (xcode = GetExceptionCode(),
              xaddr = (GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
              EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        if (logf) {
            const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
            char m[220];
            snprintf(m, sizeof(m), "[cosmetics] RefreshVisuals FAULTED code=0x%08lX at=%p (exe+0x%llX)"
                                   " -- proxy keeps its constructed look",
                     xcode, xaddr, (unsigned long long)((const uint8_t*)xaddr - base));
            logf(m);
        }
    }
    // A faulting rebuild with a peer body def in play needs ONE discriminating probe before the item
    // bisect: put the LOCAL body back and refresh again with the peer's items still worn. Clean now =>
    // the BODY DEFINITION is the fault (named, item bisect skipped); still faulting => the body is
    // cleared of suspicion and the item bisect runs against the local body, cleanly isolated. Either
    // way the peer's def stays on the actor only after a SUCCESSFUL rebuild -- a def the game refuses
    // must not be left where every later game-side refresh would re-fault on it.
    bool bodyDefWasFault = false;
    if (!ok && actorDefWritten) {
        __try {
            *(void**)((uint8_t*)proxyActor + off::kSkaterVisualDef) = savedActorDef;
            *(void**)(inst + off::kSkInstVisualDef) = savedVisualDef;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        actorDefWritten = false;                       // restored; the success path below keeps it only when ok
        if (g_bisectOnFault) {
            bodyDefWasFault = !refreshFaults(proxyActor);
            if (logf) logf(bodyDefWasFault
                ? "[cosmetics]   probe: LOCAL body def -> ok => the peer's BODY DEFINITION is the fault"
                : "[cosmetics]   probe: LOCAL body def -> still faults => body def cleared, bisecting items");
        }
    }
    // One dress, one complete answer -- see bisectCulprit's comment.
    if (!ok && g_bisectOnFault && !bodyDefWasFault) bisectCulprit(proxyActor, charMap, brdMap, c, logf);

    // ---- give the local player their look back, always
    __try {
        memcpy(inst + off::kSkInstName, savedName, 8);
        *(void**)(inst + off::kSkInstVisualDef) = savedVisualDef;
        *(uint8_t*)(prof + off::kProfStance) = savedStance;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    { int cur = 0; restoreMap(charMap, savedC, nSavedC, &cur); }
    { int cur = 0; restoreMap(brdMap,  savedB, nSavedB, &cur); }
    // Keys the wear phase INSERTED had no original to restore: overwrite each back to a cleared
    // item. A lingering cleared key is inert (it is the state every dress writes into unsent
    // categories, proven through RefreshVisuals since the first cosmetics round).
    if ((nInsC || nInsB) && S.ProfileEmplace) {
        const uint8_t cleared[kItemSize] = {};
        for (int i = 0; i < nInsC; i++) {
            __try { S.ProfileEmplace(charMap, &insC[i], cleared); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        for (int i = 0; i < nInsB; i++) {
            __try { S.ProfileEmplace(brdMap, &insB[i], cleared); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    for (int i = 0; i < nSavedInst; i++) restoreInstance(savedInst[i]);   // the wardrobe, exactly as found

    if (logf) {
        char m[200];
        snprintf(m, sizeof(m), "[cosmetics] dressed proxy: %d clothing + %d board item(s) applied "
                               "(%d into empty categories), %d not installed here | attribs: %d "
                               "instance(s), %d colour(s) with no local slot%s",
                 wornC, wornB, nInsC + nInsB, unresolved ? *unresolved : 0, nSavedInst,
                 unmatchedColors, ok ? "" : " (refresh faulted)");
        logf(m);
    }
    // THE REPLAY-EDITOR CRASH FIX. The game's own wardrobe rebuild re-syncs the skater's replay bone
    // cache after the merge; this dress path is not that rebuild, so the proxy's cache kept the indices
    // of whatever skeleton it was spawned with and the replay editor wrote through them into a pose of
    // a different size (game_syms.h, kAirReplay*). Doing the same re-sync here, after every dress,
    // makes the cache and the pose agree by construction -- for any body, any garment, any count.
    if (ok && g_replayBoneRefresh) RefreshProxyReplayBones(proxyActor, logf);
    return ok;
}

// ---- installed-content digest -------------------------------------------------------------------------
// Names cannot answer "do we see the same thing" for REPLACEMENT mods, which keep the name and change
// the art. Hashing the pak set can: equal digests => every shared item resolves to identical content.
uint64_t LocalModDigest() {
    static uint64_t cached = 0;
    static bool done = false;
    if (done) return cached;
    done = true;
    uint64_t h = 1469598103934665603ull;                    // FNV-1a
    auto mix = [&h](const void* p, int n) {
        const uint8_t* b = (const uint8_t*)p;
        for (int i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    };
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return cached;
    // <...>\SessionGame\Binaries\Win64\x.exe -> strip THREE components (file, Win64, Binaries) to land
    // on <...>\SessionGame, then + \Content\Paks.
    // THREE, not four: stripping one level too many makes the scan find nothing, every digest come out
    // 0, and the whole divergence report silently read as "everyone's content matches".
    for (int lvl = 0; lvl < 3; lvl++) {
        wchar_t* slash = nullptr;
        for (wchar_t* p = path; *p; p++) if (*p == L'\\') slash = p;
        if (!slash) return cached;
        *slash = 0;
    }
    const size_t used = wcslen(path);
    if (used + 20 >= MAX_PATH) return cached;
    wcscat_s(path, MAX_PATH, L"\\Content\\Paks\\*");
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(path, &fd);
    if (hf == INVALID_HANDLE_VALUE) return cached;
    int files = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t lower[MAX_PATH];
        int i = 0;
        for (; fd.cFileName[i] && i < MAX_PATH - 1; i++)
            lower[i] = (fd.cFileName[i] >= L'A' && fd.cFileName[i] <= L'Z') ? (wchar_t)(fd.cFileName[i] + 32)
                                                                            : fd.cFileName[i];
        lower[i] = 0;
        mix(lower, i * 2);
        const uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        mix(&sz, 8);
        files++;
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    cached = files ? h : 0;
    return cached;
}

#else
bool GatherOwnCosmetics(void*, repl::CosmeticSet&, void (*)(const char*)) { return false; }
bool DressProxy(void*, const repl::CosmeticSet&, int*, void (*)(const char*)) { return false; }
uint64_t LocalModDigest() { return 0; }
#endif

}} // namespace omp::game
