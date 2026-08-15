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
#include "dropper.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace omp { namespace game { namespace dropper {

Tuning g_tun;
static Stats g_st;
const Stats& St() { return g_st; }

// Objects we spawned for peers, and objects of OURS that adoption has hidden. Fixed arrays, the
// CompStash shape: entries live for an actor's lifetime, and both lists are cleared wholesale on a
// world change rather than pruned.
static void* g_remote[kMaxObjects];  static int g_remoteN = 0;
static void* g_hidden[kMaxObjects];  static int g_hiddenN = 0;
// The pre-session baseline (see the header). Pointers only -- these are the player's own live
// actors, and the list dies with the world like every other actor pointer here.
static void* g_pre[kMaxObjects];     static int g_preN = 0;

#ifdef _WIN32

// ---- small guarded reads ---------------------------------------------------------------------
static bool rd(const void* src, void* dst, int n) {
    __try { memcpy(dst, src, (size_t)n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; return false; }
}
static void* rdPtr(void* base, int off) {
    if (!base) return nullptr;
    void* v = nullptr;
    return rd((uint8_t*)base + off, &v, sizeof(v)) ? v : nullptr;
}

// A UE TArray header, which is all we ever touch: never resized, never reallocated, only READ and --
// for the purge below -- shortened in place.
struct TArrayHdr { void** data; int32_t num; int32_t max; };
static bool readArray(void* owner, int off, TArrayHdr* out) {
    if (!owner) return false;
    if (!rd((uint8_t*)owner + off, out, sizeof(*out))) return false;
    // A plausibility gate, not a security one: the offsets are PDB-derived and corroborated, so a
    // wild Num here means we are looking at the wrong object entirely and must touch nothing.
    if (!out->data || out->num < 0 || out->num > 4096 || out->max < out->num) { out->data = nullptr; out->num = 0; return false; }
    return true;
}

// ---- class names, cached ----------------------------------------------------------------------
// ObjectName goes through FName::ToString, which allocates an FString we deliberately leak (the
// game_syms comment: one-shot uses only). This runs once per object per publish, so it is cached by
// CLASS pointer -- a set of 200 rails is a handful of distinct classes, and a class pointer is stable
// for the life of the world.
static struct { void* cls; char name[64]; } g_clsCache[64];
static int g_clsCacheN = 0;
static const char* classNameOf(void* actor) {
    void* cls = rdPtr(actor, off::kObjClassPrivate);
    if (!cls) return nullptr;
    for (int i = 0; i < g_clsCacheN; i++) if (g_clsCache[i].cls == cls) return g_clsCache[i].name;
    char nm[64];
    if (!ObjectName(cls, nm, sizeof(nm)) || !nm[0]) return nullptr;
    if (g_clsCacheN < (int)(sizeof(g_clsCache) / sizeof(g_clsCache[0]))) {
        g_clsCache[g_clsCacheN].cls = cls;
        strncpy_s(g_clsCache[g_clsCacheN].name, sizeof(g_clsCache[0].name), nm, _TRUNCATE);
        return g_clsCache[g_clsCacheN++].name;
    }
    static char overflow[64];                       // cache full: correct, just no longer free
    strncpy_s(overflow, sizeof(overflow), nm, _TRUNCATE);
    return overflow;
}

#endif // _WIN32

// ---- availability -----------------------------------------------------------------------------
bool Available() {
    const Syms& S = Get();
    return g_tun.enabled && S.DropperInstance && S.DropperObjInfoById && S.SpawnActor && S.GetWorld
        && S.SetActorLocRot && S.SoftPathTryLoad && S.FNameCtor;
}

void* Manager() {
#ifdef _WIN32
    const Syms& S = Get();
    if (!g_tun.enabled || !S.DropperInstance) return nullptr;
    void* m = nullptr;
    if (!rd(S.DropperInstance, &m, sizeof(m))) return nullptr;
    return m;
#else
    return nullptr;
#endif
}

bool LocalActive() {
#ifdef _WIN32
    void* m = Manager();
    if (!m) return false;
    uint8_t mode = 0;
    if (!rd((uint8_t*)m + off::kDropMgrMode, &mode, 1)) return false;
    return mode != 0;
#else
    return false;
#endif
}

bool IsRemote(void* actor) {
    if (!actor) return false;
    for (int i = 0; i < g_remoteN; i++) if (g_remote[i] == actor) return true;
    return false;
}
int RemoteCount() { return g_remoteN; }
bool OwnSetHidden() { return g_hiddenN > 0; }

static bool isHidden(void* actor) {
    for (int i = 0; i < g_hiddenN; i++) if (g_hidden[i] == actor) return true;
    return false;
}

#ifdef _WIN32

// ---- THE SAVE GUARD ---------------------------------------------------------------------------
// A prop we spawn for a peer is a REAL dropped object as far as the game is concerned: it carries the
// real UObjectDropperPickableObject, whose BeginPlay registers it with the manager. Left alone, the
// local player could highlight it, pick it up and call it back -- into their own inventory, and from
// there into their own save file. Two independent cuts, because this is the one failure in the
// feature that damages something the player cannot get back:
//   (1) out of `_allObjects`, so no enumeration -- ours or the game's -- can reach it, and
//   (2) _isCurrentlyPickable cleared, so the camera trace that highlights objects refuses it even
//       though the component is still there.
// (1) is re-asserted every enumeration rather than trusted once: registration order at BeginPlay is
// the game's business and may change.
// Is this an INVENTORY object? The dropper component the game attaches answers it: UObjectDropperS
// torableObject (the derived one) means it can be stored back in an inventory, which only a prop you
// own can. A level's own prop gets the plain UObjectDropperPickableObject. Anything unreadable is
// treated as a WORLD object, which is the safe direction: the worst case is that we try to move an
// existing actor and fail to find it, rather than spawning a duplicate of a bench.
static bool isStorable(void* actor) {
    const Syms& S = Get();
    if (!S.DropperPickableOf) return false;
    void* comp = nullptr;
    __try { comp = S.DropperPickableOf(actor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; return false; }
    if (!comp) return false;
    const char* cn = classNameOf(comp);              // cached by class pointer: two entries, ever
    return cn && strstr(cn, "Storable") != nullptr;
}

static void clearPickable(void* actor) {
    const Syms& S = Get();
    if (!S.DropperPickableOf) return;
    __try {
        void* comp = S.DropperPickableOf(actor);
        if (comp) *((uint8_t*)comp + off::kPickableIsPickable) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
}

// RemoveAtSwap by hand: move the last entry into the hole and shorten. No allocation and no free --
// the array keeps owning its buffer, it just reports one fewer element, exactly as the proxy spawn
// zeroes an actor's tag count.
static bool removeFromArray(TArrayHdr& a, void* owner, int off, int idx) {
    __try {
        a.data[idx] = a.data[a.num - 1];
        a.num--;
        *(int32_t*)((uint8_t*)owner + off + 8) = a.num;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; return false; }
}

// MAKE IT MOVABLE, and keep it that way. Every prop we spawn for a peer is wire-driven, so it has to
// accept a transform write at any moment -- not only while the local player happens to have the
// dropper open. The game's own activate handler does exactly this call; going through the virtual
// rather than writing the byte is what re-registers the component, and without the re-registration
// the render and physics proxies keep the old, immovable transform.
static void makeMovable(void* actor) {
    void* root = rdPtr(actor, off::kActorRootComp);
    if (!root) return;
    __try {
        uint8_t* mob = (uint8_t*)root + off::kCompMobility;
        if (*mob == (uint8_t)off::kMobilityMovable) return;
        void** vtbl = *(void***)root;
        using SetMobilityFn = void (*)(void*, uint8_t);
        auto fn = (SetMobilityFn)vtbl[off::kVtblSetMobility / sizeof(void*)];
        fn(root, (uint8_t)off::kMobilityMovable);
        g_st.madeMovable++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
}

// Pull ONE actor out of `_allObjects` immediately. The per-enumeration sweep would catch it within a
// poll interval, but "within 250 ms" is not good enough for the save guard: the local player could
// leave the dropper -- which is when the game saves -- inside that window.
static void purgeFromAllObjects(void* actor) {
    void* m = Manager();
    TArrayHdr a;
    if (!m || !readArray(m, off::kDropMgrAllObjects, &a)) return;
    for (int i = 0; i < a.num; i++) {
        void* p = nullptr;
        if (!rd(&a.data[i], &p, sizeof(p)) || p != actor) continue;
        if (removeFromArray(a, m, off::kDropMgrAllObjects, i)) g_st.purgedFromAll++;
        return;
    }
}

#endif // _WIN32

// ---- our own set ------------------------------------------------------------------------------
int EnumerateOwn(ObjRec* out, void** actorsOut, int cap) {
#ifdef _WIN32
    g_st.own = 0; g_st.remote = g_remoteN;
    g_st.arrayNum = 0; g_st.skipWorld = 0;
    g_st.skipRemote = g_st.skipHidden = g_st.skipNoClass = g_st.skipNoRoot = 0;
    void* m = Manager();
    if (!m || cap <= 0) return 0;
    TArrayHdr a;
    if (!readArray(m, off::kDropMgrAllObjects, &a)) return 0;
    g_st.arrayNum = a.num;

    int n = 0;
    for (int i = 0; i < a.num; ) {
        void* actor = nullptr;
        if (!rd(&a.data[i], &actor, sizeof(actor)) || !actor) { i++; continue; }
        if (IsRemote(actor)) {                       // see the save guard above
            g_st.skipRemote++;
            if (removeFromArray(a, m, off::kDropMgrAllObjects, i)) { g_st.purgedFromAll++; continue; }
            i++; continue;                           // could not shorten: leave it, never spin
        }
        i++;
        if (isHidden(actor)) { g_st.skipHidden++; continue; }   // adopted away: not in the shared world
        if (n >= cap) continue;                      // count what we have, publish what fits
        // THE LEVEL'S OWN PROPS ARE LEFT ALONE. Only an object that can be stored back into an
        // inventory -- one the player actually owns -- is ours to publish. A bench belongs to the map:
        // every player already has it, syncing it was tried and withdrawn, and touching it here is
        // what once made one vanish off a joiner's screen.
        if (!isStorable(actor)) { g_st.skipWorld++; NoteMapDefault(actor); continue; }
        const char* cls = classNameOf(actor);
        if (!cls || !cls[0]) { g_st.skipNoClass++; continue; }
        void* root = rdPtr(actor, off::kActorRootComp);
        if (!root) { g_st.skipNoRoot++; continue; }
        ObjRec& r = out[n];
        if (!rd((uint8_t*)root + off::kCompPos,  r.loc,  12)) continue;
        if (!rd((uint8_t*)root + off::kCompQuat, r.quat, 16)) continue;
        strncpy_s(r.id, sizeof(r.id), cls, _TRUNCATE);
        if (actorsOut) actorsOut[n] = actor;
        n++;
    }
    g_st.own = n;
    return n;
#else
    (void)out; (void)actorsOut; (void)cap; return 0;
#endif
}

// ---- the pre-session baseline -----------------------------------------------------------------
void SnapshotPreSession() {
#ifdef _WIN32
    if (g_preN) return;                              // taken ONCE per session
    void* m = Manager();
    TArrayHdr a;
    if (!m || !readArray(m, off::kDropMgrAllObjects, &a)) return;
    for (int i = 0; i < a.num && g_preN < kMaxObjects; i++) {
        void* actor = nullptr;
        if (!rd(&a.data[i], &actor, sizeof(actor)) || !actor || IsRemote(actor)) continue;
        // Inventory objects only. The level's own furniture belongs to the map, travels on the world
        // lane, and must never be hidden -- hiding one deleted a piece of the level off a screen.
        if (!isStorable(actor)) continue;
        g_pre[g_preN++] = actor;
    }
#endif
}
bool IsPreSession(void* actor) {
    for (int i = 0; i < g_preN; i++) if (g_pre[i] == actor) return true;
    return false;
}
void ClearPreSession() { g_preN = 0; }
int  PreSessionCount() { return g_preN; }

// ---- adoption ---------------------------------------------------------------------------------
void HideOwnSet(void (*logf)(const char*)) {
#ifdef _WIN32
    if (g_hiddenN) return;                           // already adopted
    const Syms& S = Get();
    if (!S.SetActorHidden) return;
    // THE BASELINE IS THE LIST, not a fresh snapshot: by adoption time the player may already have
    // placed post-join objects, and hiding those would also silently unpublish them (hidden objects
    // are excluded from enumeration). Taken here only if the session flow somehow never took it.
    SnapshotPreSession();
    int noCollide = 0;
    for (int i = 0; i < g_preN && g_hiddenN < kMaxObjects; i++) {
        void* actor = g_pre[i];
        __try {
            S.SetActorHidden(actor, true);
            // Hiding is PURELY VISUAL -- the components keep colliding, and an invisible rail you can
            // still grind is worse than a visible one. A missing SetActorCollision is a degraded mode,
            // reported once, not a failure.
            if (S.SetActorCollision) { S.SetActorCollision(actor, false); noCollide++; }
            g_hidden[g_hiddenN++] = actor;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
    if (logf) { char msg[240]; snprintf(msg, sizeof(msg),
        "[drop] adopted the shared set: %d of my own objects hidden (%d decollided)%s",
        g_hiddenN, noCollide,
        S.SetActorCollision ? "" : " -- NO SetActorCollision: they still block!");
        logf(msg); }
#else
    (void)logf;
#endif
}

// Put the adoption hide BACK. The hidden actors are the player's own real actors, and the replay
// editor's exit pass restores the visibility of everything it registered -- which un-hides the whole
// adopted-away set and leaves the player looking at their own park standing inside the host's
// (field-logged, and the same enemy the withdrawn world model fought). Cheap to re-assert on a slow
// beat; collision comes along so an invisible rail can never be grindable.
void ReassertOwnSetHidden() {
#ifdef _WIN32
    const Syms& S = Get();
    if (!g_hiddenN || !S.SetActorHidden) return;
    for (int i = 0; i < g_hiddenN; i++) {
        __try {
            S.SetActorHidden(g_hidden[i], true);
            if (S.SetActorCollision) S.SetActorCollision(g_hidden[i], false);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
#endif
}

void RestoreOwnSet(void (*logf)(const char*)) {
#ifdef _WIN32
    if (!g_hiddenN) return;
    const Syms& S = Get();
    int n = g_hiddenN;
    for (int i = 0; i < g_hiddenN; i++) {
        __try {
            if (S.SetActorCollision) S.SetActorCollision(g_hidden[i], true);
            if (S.SetActorHidden)    S.SetActorHidden(g_hidden[i], false);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
    g_hiddenN = 0;
    if (logf) { char msg[120]; snprintf(msg, sizeof(msg), "[drop] my own %d objects are back", n); logf(msg); }
#else
    (void)logf;
#endif
}

// ---- remote objects ---------------------------------------------------------------------------
#ifdef _WIN32
// The wire name is a peer's string. It only ever reaches FName::FName(..., FNAME_Add), which INTERNS
// it into the process-wide table permanently -- the audio funnel's parameter-name lesson, verbatim:
// not type confusion, just sender-driven allocation that never comes back. So the charset is checked
// first (a UE object name is [A-Za-z0-9_]), and distinct names are budgeted. The whole dropper
// catalogue is a few dozen entries, so the budget cannot be reached by anything legitimate.
static const int kNameBudget = 128;
static int g_namesInterned = 0;
static char g_names[kNameBudget][64];

static bool validName(const char* s) {
    if (!s || !s[0]) return false;
    int i = 0;
    for (; s[i]; i++) {
        if (i >= 63) return false;
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return i > 0;
}
static bool budgetName(const char* s) {
    for (int i = 0; i < g_namesInterned; i++) if (strcmp(g_names[i], s) == 0) return true;
    if (g_namesInterned >= kNameBudget) return false;
    strncpy_s(g_names[g_namesInterned], sizeof(g_names[0]), s, _TRUNCATE);
    g_namesInterned++;
    return true;
}

// ---- THE CATALOGUE, BY NAME. GetObjectInformationByID matches an FName against
// FSoftObjectPath::GetAssetName() of each entry's class path, which assumes the peer's id is spelled
// EXACTLY as this install spells that asset. When it is not, the honest-looking answer is "you do not
// have this object" -- indistinguishable from a DLC we really lack, and that is what both ends
// reported for each other's ordinary park props.
// So a miss falls back to walking the catalogue and comparing the asset name as a STRING: exact
// first, then tolerating a trailing "_C" on either side (a class is `BP_Foo_C`, its blueprint asset
// is `BP_Foo`, and which one an entry's soft path names is per-entry). The path's FName is read with
// FNameToString -- already proven machinery -- rather than calling GetAssetName, whose return
// convention at the one call site does not match the usual struct-return ABI and is not worth
// guessing at when the same answer is one substring away.
static bool sameAsset(const char* a, const char* b) {
    if (!a || !b) return false;
    if (_stricmp(a, b) == 0) return true;
    const size_t la = strlen(a), lb = strlen(b);
    if (la > 2 && lb == la - 2 && _strnicmp(a, b, lb) == 0 && _stricmp(a + lb, "_C") == 0) return true;
    if (lb > 2 && la == lb - 2 && _strnicmp(b, a, la) == 0 && _stricmp(b + la, "_C") == 0) return true;
    return false;
}
// An FName at a known address -> ASCII. The FString is deliberately leaked, exactly as game_syms and
// audio.cpp do: this runs on a catalogue miss, once per distinct name, not per frame.
static bool fnameAscii(const void* fname, char* out, int cap) {
    out[0] = 0;
    const Syms& S = Get();
    if (!S.FNameToString || !fname || cap <= 1) return false;
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fname, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; out[0] = 0; return false; }
}
// The asset name at the tail of a soft path's package string: "/Game/A/BP_Foo.BP_Foo_C" -> "BP_Foo_C".
// The FSoftObjectPath's AssetPathName FName is its first member.
static bool softPathAssetName(void* path, char* out, int cap) {
    out[0] = 0;
    char full[220];
    if (!fnameAscii(path, full, sizeof(full)) || !full[0]) return false;
    const char* dot = strrchr(full, '.');
    const char* tail = dot ? dot + 1 : full;
    strncpy_s(out, (size_t)cap, tail, _TRUNCATE);
    return out[0] != 0;
}
// Walk every category and entry looking for `want`. Returns the entry, or null.
static void* findInfoByName(void* db, const char* want, char* sampleOut, int sampleCap) {
    if (sampleOut && sampleCap) sampleOut[0] = 0;
    TArrayHdr cats;
    if (!readArray(db, off::kDropDbCategories, &cats)) return nullptr;
    int sampled = 0;
    for (int c = 0; c < cats.num; c++) {
        uint8_t* cat = (uint8_t*)cats.data + (size_t)c * off::kDropCatStride;
        TArrayHdr objs;
        if (!readArray(cat, off::kDropCatObjects, &objs)) continue;
        for (int i = 0; i < objs.num; i++) {
            uint8_t* info = (uint8_t*)objs.data + (size_t)i * off::kDropInfoStride;
            char nm[96];
            if (!softPathAssetName(info + off::kDropInfoClassPath, nm, sizeof(nm))) continue;
            if (sameAsset(nm, want)) return info;
            // A few real names, so a failure can say what this install DOES call its objects instead
            // of only asserting that it does not have one.
            if (sampleOut && sampled < 4 && (int)strlen(sampleOut) + (int)strlen(nm) + 2 < sampleCap) {
                if (sampleOut[0]) strncat_s(sampleOut, (size_t)sampleCap, ", ", _TRUNCATE);
                strncat_s(sampleOut, (size_t)sampleCap, nm, _TRUNCATE);
                sampled++;
            }
        }
    }
    return nullptr;
}

// Names we have already refused, so a set full of one missing DLC prop says so once and not 40 times.
static char g_unknown[16][64]; static int g_unknownN = 0;
static bool sayUnknownOnce(const char* s) {
    for (int i = 0; i < g_unknownN; i++) if (strcmp(g_unknown[i], s) == 0) return false;
    if (g_unknownN < 16) { strncpy_s(g_unknown[g_unknownN], sizeof(g_unknown[0]), s, _TRUNCATE); g_unknownN++; }
    return true;
}
#endif

// Name -> UClass, cached per distinct name. The cache matters: the catalogue walk allocates an FString
// per entry it inspects, and a set arrives every few seconds, so an un-cached miss would leak on a
// heartbeat. One decision per name, kept.
// NO CLASS CACHE, deliberately (one was here and got a field round spent on it): a UClass returned
// by TryLoad stays alive only while something references it, and OUR references are the spawned
// actors themselves -- the moment a session reset destroys the last one, the soft-loaded class is
// garbage-collectable IN THE SAME WORLD, and a cached pointer starts dangling. Field-logged as the
// same three classes spawning at 23:26 and returning null / FAULTING from 23:33, with zero level
// changes between. TryLoad on a live class is a cheap resolve, spawns are rare, and a fresh answer
// is valid by construction at the moment it is used.
static void* resolveClass(void* db, const char* name, void (*logf)(const char*)) {
    const Syms& S = Get();
    void* cls = nullptr;
    char sample[200] = {0};
    __try {
        // First the game's own id lookup: allocation-free, and it is the type gate too -- what it
        // returns is a FObjectDropperObjectInformation whose class path is declared
        // TSoftClassPtr<AActor>, so TryLoad on it cannot hand back a wrong-typed object the way
        // StaticFindObject(ANY_PACKAGE) on a peer's raw name could.
        void* info = nullptr;
        if (S.DropperObjInfoById && S.FNameCtor && budgetName(name)) {
            uint64_t fn = 0;
            S.FNameCtor(&fn, name, 1);                           // FNAME_Add, budgeted
            info = S.DropperObjInfoById(db, fn);
        }
        // Then by name. Same catalogue, same entries -- only the comparison differs, so this can
        // never reach anything the id lookup was protecting.
        if (!info) {
            info = findInfoByName(db, name, sample, sizeof(sample));
            if (info) {
                g_st.resolvedByName++;
                if (logf) { static bool said = false; if (!said) { said = true; char msg[220];
                    snprintf(msg, sizeof(msg), "[drop] '%s' resolved by name, not by id -- this"
                             " install spells its catalogue entries differently", name); logf(msg); } }
            }
        }
        // TryLoad, not ResolveObject: a prop this player has never placed was never streamed in, and
        // residency is never inferred (the garment-mesh lesson). The return doubles as the proof.
        if (info) cls = S.SoftPathTryLoad((uint8_t*)info + off::kDropInfoClassPath, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; cls = nullptr; }

    if (!cls) {
        g_st.unknownIds++;
        if (logf && sayUnknownOnce(name)) { char msg[320]; snprintf(msg, sizeof(msg),
            "[drop] object '%s' is not in this install's dropper catalogue -- skipping it%s%s",
            name, sample[0] ? ". Mine are named like: " : "", sample); logf(msg); }
    }
    return cls;
}

void* SpawnRemote(void* ownPawn, const ObjRec& r, void (*logf)(const char*)) {
#ifdef _WIN32
    const Syms& S = Get();
    if (!Available() || !ownPawn) return nullptr;
    if (g_remoteN >= kMaxObjects) return nullptr;
    if (!validName(r.id) || !budgetName(r.id)) { g_st.spawnFails++; return nullptr; }
    void* m = Manager();
    void* db = rdPtr(m, off::kDropMgrDatabase);
    if (!db) return nullptr;

    void* cls = resolveClass(db, r.id, logf);
    if (!cls) { g_st.spawnFails++; return nullptr; }

    void* world = S.GetWorld(ownPawn);
    if (!world) return nullptr;

    void* p = nullptr;
    unsigned long xcode = 0;
    // Spawned at the final position with identity rotation, then rotated by quat: SpawnActor's
    // rotation argument is a 3-float FRotator, and a rotator round-trip is exactly what standing
    // rule 1 forbids. SetActorLocRot takes the FQuat overload, so the wire quat is written unconverted.
    float loc[3] = { r.loc[0], r.loc[1], r.loc[2] };
    float rot0[3] = { 0, 0, 0 };
    uint8_t params[0x80]; memset(params, 0, sizeof(params));
    // Same discipline as the proxy spawn: a caught fault is NOT a recovery (the engine unwound
    // without its cleanup and a half-built actor is likely abandoned), so it is loud and we do not
    // retry this record.
    __try { p = S.SpawnActor(world, cls, loc, rot0, params); }
    __except (xcode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
    if (!p) {
        g_st.spawnFails++;
        if (logf) { char msg[200]; snprintf(msg, sizeof(msg),
            "[drop] spawn of '%s' %s", r.id, xcode ? "FAULTED" : "returned null"); logf(msg); }
        return nullptr;
    }

    // Registered BEFORE anything else touches it: every "is this one of ours?" answer, including the
    // save guard's, must be right from the actor's first frame.
    g_remote[g_remoteN++] = p;
    __try {
        // The host is a server as far as the engine is concerned, so its spawned actors would
        // replicate. Same frame as the spawn, for the same reason as a proxy: the net driver
        // replicates on its NEXT tick.
        if (S.SetReplicates) S.SetReplicates(p, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    clearPickable(p);
    purgeFromAllObjects(p);            // the save guard, closed the same frame the actor appears
    makeMovable(p);                    // BEFORE the first placement, or even that one is ignored
    __try { S.SetActorLocRot(p, r.loc, r.quat, false, nullptr, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    g_st.spawned++;
    return p;
#else
    (void)ownPawn; (void)r; (void)logf; return nullptr;
#endif
}

void DriveRemote(void* actor, const float loc[3], const float quat[4],
                 float curLoc[3], float curQuat[4], float dt) {
#ifdef _WIN32
    const Syms& S = Get();
    if (!actor || !S.SetActorLocRot) return;
    const float dx = loc[0]-curLoc[0], dy = loc[1]-curLoc[1], dz = loc[2]-curLoc[2];
    const float d2 = dx*dx + dy*dy + dz*dz;
    // ALREADY THERE: write nothing. A dropped object is static almost all the time, and a session can
    // hold hundreds of them -- moving an actor is not free (transform propagation, physics and
    // overlap updates), so a per-frame write per object would be a standing cost for a world that is
    // not changing.
    // The comparison is against THE ACTOR, read back, not against our own last write. Trusting the
    // belief is a bug with a name: anything that moves the prop after we place it -- its own tick, a
    // physics settle, a ground snap -- is then never corrected, because our belief still matches the
    // target and we never write again. It renders as an object that lands somewhere its owner did not
    // put it and stays there.
    {
        float qd = 0;
        for (int i = 0; i < 4; i++) { const float d = quat[i] - curQuat[i]; qd += d*d; }
        const bool driveDone = d2 <= g_tun.moveEpsCm * g_tun.moveEpsCm * 0.01f &&
                               qd <= g_tun.moveEpsQuat * g_tun.moveEpsQuat * 0.01f;
        if (driveDone) {
            void* root = rdPtr(actor, off::kActorRootComp);
            float actual[3];
            if (root && rd((uint8_t*)root + off::kCompPos, actual, 12)) {
                const float ex = loc[0]-actual[0], ey = loc[1]-actual[1], ez = loc[2]-actual[2];
                if (ex*ex + ey*ey + ez*ez <= g_tun.driftEpsCm * g_tun.driftEpsCm) return;
                g_st.driftFixes++;                       // it moved on its own: put it back
                memcpy(curLoc, actual, 12);
            } else return;                               // cannot read it back: leave it alone
        }
    }
    float t = 1.0f;                                   // stamp
    if (g_tun.followRate > 0.f && dt > 0.f && d2 < g_tun.followSnapCm * g_tun.followSnapCm) {
        t = g_tun.followRate * dt;
        if (t > 1.0f) t = 1.0f;
    }
    curLoc[0] += dx*t; curLoc[1] += dy*t; curLoc[2] += dz*t;
    // Shortest-arc nlerp. Objects rotate by hand at human speed, so there is no 180-degree wrap race
    // here of the kind that broke the board's angular chase -- but the sign fix is free and a
    // duplicate placed mirrored would otherwise take the long way round.
    float dot = curQuat[0]*quat[0] + curQuat[1]*quat[1] + curQuat[2]*quat[2] + curQuat[3]*quat[3];
    const float s = (dot < 0.f) ? -1.f : 1.f;
    for (int i = 0; i < 4; i++) curQuat[i] += (s*quat[i] - curQuat[i]) * t;
    float n = sqrtf(curQuat[0]*curQuat[0] + curQuat[1]*curQuat[1] + curQuat[2]*curQuat[2] + curQuat[3]*curQuat[3]);
    if (n > 1e-6f) { for (int i = 0; i < 4; i++) curQuat[i] /= n; }
    else           { curQuat[0]=curQuat[1]=curQuat[2]=0.f; curQuat[3]=1.f; }
    // Re-asserted on every write, not just at spawn: the game restores each prop's REMEMBERED
    // mobility when the local player closes the dropper, and if it ever remembered Static for one of
    // ours, that object would go quietly immovable again. One byte compare when it is already right.
    makeMovable(actor);
    __try { S.SetActorLocRot(actor, curLoc, curQuat, false, nullptr, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
#else
    (void)actor; (void)loc; (void)quat; (void)curLoc; (void)curQuat; (void)dt;
#endif
}

void DestroyRemote(void* actor, void (*logf)(const char*)) {
#ifdef _WIN32
    if (!actor) return;
    int at = -1;
    for (int i = 0; i < g_remoteN; i++) if (g_remote[i] == actor) { at = i; break; }
    if (at < 0) return;                               // never destroy an actor that is not ours
    g_remote[at] = g_remote[--g_remoteN];
    const Syms& S = Get();
    if (S.ActorDestroy) {
        __try { S.ActorDestroy(actor, false, true); g_st.destroyed++; return; }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
    // No Destroy, or it faulted: fall back to the retirement a proxy gets -- collision off FIRST, then
    // hidden, so nothing is ever an invisible obstacle. The prop stays in the world until the next
    // level load, which is the honest cost of a missing symbol.
    __try {
        if (S.SetActorCollision) S.SetActorCollision(actor, false);
        if (S.SetActorHidden)    S.SetActorHidden(actor, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    if (logf) { static bool said = false; if (!said) { said = true;
        logf("[drop] no ActorDestroy -- peers' objects are hidden instead of removed until the next level"); } }
#else
    (void)actor; (void)logf;
#endif
}

bool ActorPose(void* actor, float loc[3], float quat[4]) {
#ifdef _WIN32
    void* root = rdPtr(actor, off::kActorRootComp);
    return root && rd((uint8_t*)root + off::kCompPos, loc, 12)
                && rd((uint8_t*)root + off::kCompQuat, quat, 16);
#else
    (void)actor; (void)loc; (void)quat; return false;
#endif
}
static bool ActorPoseOf(void* a, float l[3], float q[4]) { return ActorPose(a, l, q); }

bool IsSelectedLocally(void* actor) {
#ifdef _WIN32
    if (!actor) return false;
    void* m = Manager();
    TArrayHdr a;
    if (!m || !readArray(m, off::kDropMgrSelected, &a)) return false;
    for (int i = 0; i < a.num; i++) {
        void* p = nullptr;
        if (rd(&a.data[i], &p, sizeof(p)) && p == actor) return true;
    }
    return false;
#else
    (void)actor; return false;
#endif
}

// ---- THE LEVEL'S OWN PROPS, moved for the session (see the header) -----------------------------
struct WorldTouched {
    char  name[64];
    void* actor;                       // the level's own actor, standing on a session pose
    float origLoc[3], origQuat[4];     // where THIS player's world had it before the session
    float stashLoc[3], stashQuat[4];   // the session pose, parked here across a bracketed save
    bool  stashed;
};
static WorldTouched g_wt[128];
static int          g_wtN = 0;
int WorldTouchedCount() { return g_wtN; }
static bool g_saveGuard = false;
void SetSaveGuardArmed(bool armed) { g_saveGuard = armed; }
bool SaveGuardArmed() { return g_saveGuard; }

static WorldTouched* wtFind(const char* name) {
    for (int i = 0; i < g_wtN; i++) if (_stricmp(g_wt[i].name, name) == 0) return &g_wt[i];
    return nullptr;
}

#ifdef _WIN32
// The level's own actor with this name, proven to be a dropper prop and not one of ours. The name
// came off the wire and ANY_PACKAGE matches any class, so it is a peer's choice until checked.
static void* levelActorByName(const char* name) {
    const Syms& S = Get();
    if (!name || !name[0] || !S.StaticFindObject) return nullptr;
    wchar_t wide[64];
    int k = 0;
    for (; k < 63 && name[k]; k++) wide[k] = (wchar_t)(uint8_t)name[k];
    wide[k] = 0;
    void* obj = nullptr;
    __try { obj = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, wide, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; return nullptr; }
    if (!obj || IsRemote(obj) || !IsObjectOfClass(obj, "Actor")) return nullptr;
    void* comp = nullptr;
    if (S.DropperPickableOf) {
        __try { comp = S.DropperPickableOf(obj); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; return nullptr; }
    }
    return comp ? obj : nullptr;
}
#endif

void* WorldTouch(const char* actorName) {
#ifdef _WIN32
    if (!actorName || !actorName[0]) return nullptr;
    if (WorldTouched* w = wtFind(actorName)) return w->actor;
    if (g_wtN >= (int)(sizeof(g_wt) / sizeof(g_wt[0]))) return nullptr;
    void* actor = levelActorByName(actorName);
    if (!actor) { g_st.worldMissing++; return nullptr; }
    // The pose it stands on RIGHT NOW is this player's own arrangement -- their save applied it at
    // level load. Remembered before anything writes, because it is what every restore returns to.
    float loc[3], quat[4];
    if (!ActorPoseOf(actor, loc, quat)) return nullptr;
    makeMovable(actor);
    WorldTouched& w = g_wt[g_wtN++];
    g_st.worldTouched = g_wtN;
    strncpy_s(w.name, sizeof(w.name), actorName, _TRUNCATE);
    w.actor = actor;
    memcpy(w.origLoc, loc, sizeof(w.origLoc)); memcpy(w.origQuat, quat, sizeof(w.origQuat));
    w.stashed = false;
    return actor;
#else
    (void)actorName; return nullptr;
#endif
}

bool WorldOriginalOf(const char* actorName, float loc[3], float quat[4]) {
    WorldTouched* w = wtFind(actorName);
    if (!w) return false;
    memcpy(loc, w->origLoc, 12); memcpy(quat, w->origQuat, 16);
    return true;
}

void RestoreWorldAll(void (*logf)(const char*)) {
#ifdef _WIN32
    if (!g_wtN) return;
    const Syms& S = Get();
    const int n = g_wtN;
    for (int i = 0; i < g_wtN; i++) {
        makeMovable(g_wt[i].actor);                 // the game restores remembered mobility on dropper close
        __try { S.SetActorLocRot(g_wt[i].actor, g_wt[i].origLoc, g_wt[i].origQuat, false, nullptr, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
    g_wtN = 0; g_st.worldTouched = 0;
    if (logf) { char m[160]; snprintf(m, sizeof(m),
        "[drop/world] session over -- %d level prop(s) back to your own arrangement", n); logf(m); }
#else
    (void)logf;
#endif
}

// The save bracket (see the header). Begin parks each prop's SESSION pose and stands it on its
// original; End puts the session pose back. Between the two the game reads the world and writes the
// profile, so what it records is always the player's own arrangement. Both are no-ops for a prop
// whose pose was never changed (origin == current), which keeps the bracket harmless when idle.
void WorldSaveRestoreBegin() {
#ifdef _WIN32
    const Syms& S = Get();
    for (int i = 0; i < g_wtN; i++) {
        WorldTouched& w = g_wt[i];
        w.stashed = false;
        float loc[3], quat[4];
        if (!ActorPoseOf(w.actor, loc, quat)) continue;
        const float dx = loc[0]-w.origLoc[0], dy = loc[1]-w.origLoc[1], dz = loc[2]-w.origLoc[2];
        if (dx*dx + dy*dy + dz*dz < 0.25f) {
            float qd = 0; for (int k = 0; k < 4; k++) { const float d = quat[k]-w.origQuat[k]; qd += d*d; }
            if (qd < 1e-6f) continue;                // standing on its original: nothing to bracket
        }
        memcpy(w.stashLoc, loc, sizeof(w.stashLoc)); memcpy(w.stashQuat, quat, sizeof(w.stashQuat));
        w.stashed = true;
        __try { S.SetActorLocRot(w.actor, w.origLoc, w.origQuat, false, nullptr, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
#endif
}
void WorldSaveRestoreEnd() {
#ifdef _WIN32
    const Syms& S = Get();
    for (int i = 0; i < g_wtN; i++) {
        WorldTouched& w = g_wt[i];
        if (!w.stashed) continue;
        w.stashed = false;
        __try { S.SetActorLocRot(w.actor, w.stashLoc, w.stashQuat, false, nullptr, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    }
#endif
}

int PurgeOursFromSaveList() {
#ifdef _WIN32
    void* m = Manager();
    TArrayHdr a;
    if (!m || !readArray(m, off::kDropMgrAllObjects, &a)) return 0;
    int removed = 0;
    for (int i = 0; i < a.num; ) {
        void* actor = nullptr;
        if (!rd(&a.data[i], &actor, sizeof(actor))) { i++; continue; }
        if (!actor || !IsRemote(actor)) { i++; continue; }
        if (removeFromArray(a, m, off::kDropMgrAllObjects, i)) { removed++; g_st.purgedFromAll++; continue; }
        i++;
    }
    return removed;
#else
    return 0;
#endif
}

struct MapDefault { void* actor; float loc[3]; float quat[4]; };
static MapDefault g_mapDef[512];
static int        g_mapDefN = 0;
static bool       g_inLoad = false;

int MovedWorldNames(char out[][64], int cap) {
#ifdef _WIN32
    // MOVED means "not where the map put it": the map-default table (fed by the Load seam, topped up
    // by first-sight capture during enumeration) holds where each prop started, so the answer is a
    // pose diff against it -- NOT a walk of `_allObjects`, whose membership is the manager's business
    // and includes props nobody ever touched. A prop dragged back onto its own default correctly
    // stops counting as moved.
    if (cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_mapDefN && n < cap; i++) {
        void* actor = g_mapDef[i].actor;
        if (!actor || IsRemote(actor)) continue;
        float loc[3], quat[4];
        if (!ActorPose(actor, loc, quat)) continue;
        const float dx = loc[0]-g_mapDef[i].loc[0], dy = loc[1]-g_mapDef[i].loc[1], dz = loc[2]-g_mapDef[i].loc[2];
        float qd = 0; for (int k = 0; k < 4; k++) { const float d = quat[k]-g_mapDef[i].quat[k]; qd += d*d; }
        if (dx*dx + dy*dy + dz*dz < 1.0f && qd < 1e-5f) continue;    // standing on its default
        if (!ObjectName(actor, out[n], 64) || !out[n][0]) continue;
        n++;
    }
    return n;
#else
    (void)out; (void)cap; return 0;
#endif
}

// ---- THE MAP DEFAULT (see the header) ----------------------------------------------------------
// Actor-keyed, so it dies with the world like every other actor pointer we hold. Sized for the whole
// level rather than for a session's worth of props: this records what the MAP has, which is a
// property of the level and not of who is playing.

void SetInPersistentLoad(bool inside) { g_inLoad = inside; }
bool InPersistentLoad() { return g_inLoad; }
int  MapDefaultCount() { return g_mapDefN; }

void NoteMapDefault(void* actor) {
#ifdef _WIN32
    if (!actor || g_mapDefN >= (int)(sizeof(g_mapDef) / sizeof(g_mapDef[0]))) return;
    // Never capture a prop that is standing on a SESSION pose -- reading it now would record the
    // host's arrangement as this map's default. (First sight during a session happens: the world
    // table can touch a prop before the enumeration ever saw it.)
    for (int i = 0; i < g_wtN; i++) if (g_wt[i].actor == actor) return;
    // FIRST WRITE WINS. Load may move a prop more than once, and only the pose before the FIRST move
    // is the map's -- every later one is already the save talking.
    for (int i = 0; i < g_mapDefN; i++) if (g_mapDef[i].actor == actor) return;
    void* root = rdPtr(actor, off::kActorRootComp);
    MapDefault& m = g_mapDef[g_mapDefN];
    if (!root || !rd((uint8_t*)root + off::kCompPos, m.loc, 12) ||
        !rd((uint8_t*)root + off::kCompQuat, m.quat, 16)) { g_st.mapDefaultMissed++; return; }
    m.actor = actor;
    g_mapDefN++;
    g_st.mapDefaults = g_mapDefN;
#else
    (void)actor;
#endif
}

bool MapDefaultOf(void* actor, float loc[3], float quat[4]) {
    for (int i = 0; i < g_mapDefN; i++) {
        if (g_mapDef[i].actor != actor) continue;
        memcpy(loc, g_mapDef[i].loc, 12);
        memcpy(quat, g_mapDef[i].quat, 16);
        return true;
    }
    return false;
}

void Forget() {
    g_mapDefN = 0; g_st.mapDefaults = 0;   // actor-keyed: they died with the level
    g_wtN = 0; g_st.worldTouched = 0;      // ...and so did the touched props and their originals
    g_remoteN = 0; g_hiddenN = 0; g_preN = 0; g_clsCacheN = 0;
    g_st.remote = 0; g_st.own = 0;
}

void ResetAll(void (*logf)(const char*)) {
    RestoreWorldAll(logf);          // every level prop back on this player's own arrangement
    while (g_remoteN > 0) DestroyRemote(g_remote[g_remoteN - 1], logf);
    RestoreOwnSet(logf);
    // NOT Forget(): the world is still standing, and so is everything the map-default table knows
    // about it. Only a level change may clear that table -- see the header note.
    g_preN = 0;
}

}}} // namespace omp::game::dropper
