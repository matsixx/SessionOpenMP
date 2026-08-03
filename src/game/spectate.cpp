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
#include "spectate.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace omp { namespace game { namespace spectate {

static void* g_target = nullptr;
void* Target() { return g_target; }

// The local skater, republished every frame by the loader (the same pattern as
// audio::SetLocalParts). "Me" has to be an actual actor -- see SetLookTarget.
static void* g_localSkater = nullptr;
void SetLocalSkater(void* pawn) { g_localSkater = pawn; }

#ifdef _WIN32
// An object's CLASS name. The registered recorders are a mix of types, and reading one type's fields
// out of another faults, so nothing is read from a recorder until it has identified itself.
// UObject::ClassPrivate is +0x10, and a UObject's own FName is +0x18.
static bool classNameOf(void* obj, char* out, int cap) {
    out[0] = 0;
    const Syms& S = Get();
    if (!obj || !S.FNameToString) return false;
    __try {
        void* cls = *(void**)((uint8_t*)obj + 0x10);
        if (!cls) return false;
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString((uint8_t*)cls + off::kObjNamePrivate, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
// instance -> _replayInputController -> _replayCamera. Null anywhere means the replay editor is not
// up, which is not an error -- the row can be changed before entering it.
static void* replayCamera() {
    const Syms& S = Get();
    if (!S.ReplayMgrInstance) return nullptr;
    __try {
        void* mgr = S.ReplayMgrInstance();
        if (!mgr) return nullptr;
        void* ctl = *(void**)((uint8_t*)mgr + off::kReplayMgrInputCtl);
        if (!ctl) return nullptr;
        return *(void**)((uint8_t*)ctl + off::kInputCtlCamera);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
#else
static void* replayCamera() { return nullptr; }
#endif

// =====================================================================================================
// WHY A PEER LANDS IN YOUR REPLAY -- read out of UReplayComponentBase::BeginPlay:
//
//     if (this->_manager) return;                          // +0xb8 -- registration is idempotent
//     if (!GReplayManager || !(GReplayManager->flags & 2)) return;
//     AddUnique(GReplayManager->_components /*+0x298*/, (IReplayComponent*)this + 0xb0);
//     this->_manager = GReplayManager;
//
// Replay components REGISTER THEMSELVES at BeginPlay. A proxy is a real game-class skater -- the
// founding architectural choice, and what buys player collision, native audio and the anim graph -- so
// it owns the very same UDebugSkaterReplayComponent every skater owns, and its board a
// UDebugSkateboardReplayComponent. Each one signs itself up. Nothing singles the peer out; it is
// recorded for exactly the reason the local player is, and it scales the wrong way: every peer and
// every peer's board, written into your replay buffer, for as many players as join.
//
// THE ARRAY DOES NOT HOLD UObject*. It holds the INTERFACE sub-object, `component + 0xb0`
// (`lea rbx,[rsi+0xb0]` in BOTH BeginPlay and EndPlay). Reading an entry's `+0x10` as a UClass* really
// reads `component + 0xc0` and yields nonsense class names. Decode the frame before naming what a
// pointer holds -- array elements included, not just registers.
//
// Removal mirrors UReplayComponentBase::EndPlay, which is a run-length TArray::Remove: an
// order-preserving compaction of that same array. Only the container's STATE has to match; the
// instructions need not. The `_manager` back-pointer at +0xb8 is deliberately LEFT SET -- BeginPlay
// bails when it is non-null, so it doubles as a permanent "never re-register" latch, and a later
// EndPlay simply scans and finds nothing.
//
// With no replay component registered, a peer is neither RECORDED nor PLAYED BACK. During your replay
// their skater is not posed by the replay system at all, so the live drive is the only writer and you
// watch them skating live while you scrub. That also ends the two-writers fight that makes a recorded
// peer's board thrash, their feet lift off the deck, and their live bails play back as phantom falls.
// =====================================================================================================
int PruneProxyComponents(void (*logf)(const char*)) {
#ifdef _WIN32
    const Syms& S = Get();
    if (!S.ReplayMgrInstance) return 0;
    void* mgr = nullptr;
    __try { mgr = S.ReplayMgrInstance(); } __except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
    if (!mgr) return 0;

    // Gathered under the guard, reported outside it -- naming a class calls back into the game, which
    // has no business running inside the handler that is protecting the array walk.
    void* removedComp[8] = {};
    int   removed = 0, scanned = 0;

    __try {
        uint8_t** data = *(uint8_t***)((uint8_t*)mgr + off::kReplayMgrCompData);
        int*      num  =  (int*)      ((uint8_t*)mgr + off::kReplayMgrCompNum);
        if (!data || *num <= 0 || *num > 4096) return 0;
        scanned = *num;
        int w = 0;
        for (int r = 0; r < *num; r++) {
            uint8_t* entry = data[r];
            bool drop = false;
            if (entry) {
                // ONE read off the candidate, and its result is used purely as a pointer COMPARISON
                // against actors this mod spawned. So even if some future registrant sits at a
                // different interface offset and this lands on an unrelated field, garbage cannot
                // equal one of our proxies -- the pass degrades to "leave it alone", never to a
                // wrong removal.
                uint8_t* comp  = entry - off::kReplayIfaceToComp;
                void*    owner = *(void**)(comp + off::kCompOwner);
                if (owner && IsProxyActor(owner)) {                 // covers the skater AND its board
                    drop = true;
                    if (removed < 8) removedComp[removed] = comp;
                    removed++;
                }
            }
            if (!drop) data[w++] = data[r];                         // order-preserving, like Remove()
        }
        *num = w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool faulted = false;
        if (!faulted && logf) { faulted = true;
            logf("[replay] prune: FAULTED walking the registered-component list -- left untouched"); }
        return 0;
    }

    if (removed && logf) {
        // Named once: this is the proof of the mechanism, and it tells the next reader exactly which
        // components a skater signs up.
        static bool named = false;
        if (!named) {
            named = true;
            for (int i = 0; i < removed && i < 8; i++) {
                char cls[96], m[200];
                if (!classNameOf(removedComp[i], cls, sizeof(cls))) continue;
                snprintf(m, sizeof(m), "[replay]   unregistered %s (a peer's)", cls);
                logf(m);
            }
        }
        char m[210];
        snprintf(m, sizeof(m), "[replay] kept %d peer component(s) out of your recording (%d were"
                               " registered) -- a peer's skating belongs in THEIR replay, and during"
                               " yours you now see them live", removed, scanned);
        logf(m);
    }
    return removed;
#else
    (void)logf; return 0;
#endif
}

// ---- what the camera was before the mod touched it, so "Me" can put it back.
// The replay editor's camera is the USER'S -- they positioned it, they chose its mode. Watching a peer
// is an imposed override, so going back to "Me" must be an UNDO of that override, never a second
// override that happens to point at the local skater. Forcing an Orbit onto the local skater instead
// throws the camera somewhere else the moment a solo replay is paused.
static bool    g_camSaved     = false;
static void*   g_savedLookAt  = nullptr;
static uint8_t g_savedCamType = 0;

bool SetLookTarget(void* actor, void (*logf)(const char*)) {
    // The LAST line of defence against aiming the engine at a freed actor. Callers are supposed to
    // resolve an identity to a live actor at the moment of use, but this field is read by the game
    // every frame of playback and a stale pointer here faults inside the camera with this DLL nowhere
    // in the stack. So refuse anything that is not a live proxy rather than trusting the caller.
    // Cheap: a scan of a 16-entry table. (null = "Me", handled below, never refused.)
    if (actor && actor != g_localSkater && !IsProxyActor(actor)) {
        if (logf) logf("[spectate] refused a look target that is not a live proxy (that peer left) --"
                       " camera handed back to your own skater");
        actor = nullptr;
    }
    const bool toPeer = (actor != nullptr && actor != g_localSkater);
    g_target = toPeer ? actor : nullptr;               // remembered even if the editor is not up yet
#ifdef _WIN32
    const Syms& S = Get();
    void* cam = replayCamera();
    if (!cam) return false;
    __try {
        if (toPeer) {
            // Save the user's camera ONCE, on the first override, so a peer->peer switch does not
            // overwrite the original with the previous override.
            if (!g_camSaved) {
                g_camSaved     = true;
                g_savedLookAt  = *(void**)((uint8_t*)cam + off::kReplayCamLookAt);
                g_savedCamType = *(uint8_t*)((uint8_t*)cam + off::kReplayCamActiveType);
            }
            *(void**)((uint8_t*)cam + off::kReplayCamLookAt) = actor;
            // SetCameraType early-outs when the requested type is already active, and Orbit is the
            // target state every time -- so drop the active type first and let it run its full path.
            // That is what recomputes the orbit distance and rotation from the NEW target.
            if (S.ReplayCamSetType) {
                *(uint8_t*)((uint8_t*)cam + off::kReplayCamActiveType) = 0;   // RCT_None
                S.ReplayCamSetType(cam, (uint8_t)off::kReplayCamOrbit);
            }
        } else {
            // "Me". With no override in place there is NOTHING to do. Otherwise put back exactly what
            // was found.
            if (!g_camSaved) return true;
            g_camSaved = false;
            *(void**)((uint8_t*)cam + off::kReplayCamLookAt) = g_savedLookAt;
            if (S.ReplayCamSetType) {
                *(uint8_t*)((uint8_t*)cam + off::kReplayCamActiveType) = 0;   // force the full path
                S.ReplayCamSetType(cam, g_savedCamType);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (logf) logf("[spectate] faulted aiming the replay camera -- target not changed");
        return false;
    }
    if (logf) logf(toPeer ? "[spectate] replay camera now orbiting a peer"
                          : "[spectate] replay camera restored to how you had it");
    return true;
#else
    (void)logf; return false;
#endif
}

void OnActorGone(void* actor, void (*logf)(const char*)) {
    if (!actor || g_target != actor) return;
    // They were the camera's subject and their actor is about to die. Clearing the local pointer is
    // not enough -- the game holds its own copy in AReplayCamera::_lookAtTarget and dereferences it
    // every frame of playback. Hand the camera back before the actor goes, not after.
    // Do NOT just null the field: a null _lookAtTarget is not "back to the player", it is "no subject
    // at all", which is its own wrong-spot camera. Routed through SetLookTarget so the saved camera is
    // RESTORED and there is exactly one place that decides what handing back means.
    g_target = nullptr;
    SetLookTarget(nullptr, nullptr);
    if (logf) logf("[spectate] the player you were watching left -- camera restored to how you had it");
}

}}} // namespace omp::game::spectate
