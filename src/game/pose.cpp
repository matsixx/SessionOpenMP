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
#include <cmath>
#include "pose.h"
#include "game_syms.h"
#include "proxy.h"
#include "../session/session.h"
#include "../debug.h"
#include <cstring>
#include <cstdio>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace omp { namespace game { namespace pose {

using namespace omp::repl;

static Tuning g_tun;
static Stats  g_st;
Tuning& Tune() { return g_tun; }
Stats   GetStats() { return g_st; }

// The transported pose, keyed by the MESH that has to wear it -- the same shape as the anim post-pass
// registry, and for the same reason: the hook sees a component, not a proxy.
static const int kSlots = 8;
struct Slot {
    void*    mesh = nullptr;
    uint8_t  n = 0;                       // TRANSPORTED pose (0 = none in hand)
    // Slice reassembly. A skeleton too big for one packet arrives over consecutive frames, so bones
    // land here as they come and `n` stays 0 -- the pose is not usable, and must not be stamped --
    // until a whole sweep has been seen. `covTotal` is the total the slices claim; a change means
    // the peer was re-dressed and everything collected so far describes a different skeleton.
    uint32_t cov[3] = {0, 0, 0};
    uint8_t  covTotal = 0;
    uint64_t freshMs = 0;
    float    rot[kPoseMaxBones][4];
    float    pos[kPoseMaxBones][3];
    // The LAST POSE THIS PROXY'S OWN GRAPH PRODUCED, snapshotted every frame of normal play. During a
    // LOCAL replay nothing produces another one (see the header), so this is what the skeleton wears
    // instead of collapsing. A slot exists for every proxy mesh, not only for one with a transported
    // pose, so `n == 0` does not mean "no slot".
    uint8_t  holdN = 0;
    float    holdRot[kPoseMaxBones][4];
    float    holdPos[kPoseMaxBones][3];
};
static Slot g_slots[kSlots];

#ifdef _WIN32
// ---- the component-space buffer that is CURRENTLY BEING BUILT (the editable one). Before the flip
// this holds the finished pose; after it, it is the previous frame's. Which is why the seam matters.
static uint8_t* compSpace(void* mesh, bool editable, int* numOut) {
    __try {
        const int idxOff = editable ? off::kMeshCompEditIdx : off::kMeshCompReadIdx;
        const int idx = *(const int*)((const uint8_t*)mesh + idxOff);
        if (idx != 0 && idx != 1) return nullptr;
        uint8_t* base = (uint8_t*)mesh + off::kMeshCompSpaceArr + idx * off::kMeshCompSpaceStride;
        uint8_t* data = *(uint8_t**)base;
        const int num = *(const int*)(base + 8);
        if (!data || num <= 0 || num > 512) return nullptr;
        if (numOut) *numOut = num;
        return data;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
#else
static uint8_t* compSpace(void*, bool, int*) { return nullptr; }
#endif

// =====================================================================================================
// SENDER
// =====================================================================================================
// The copy itself, no gate: both public entry points funnel here.
static bool captureInto(void* mesh, State& s) {
#ifdef _WIN32
    int num = 0;
    // READ buffer here: the sender samples from outside the animation pipeline (the engine-tick
    // anchor), so the published pose is the correct one to copy.
    uint8_t* cs = compSpace(mesh, /*editable*/ false, &num);
    if (!cs) return false;
    const int n = num < kPoseMaxBones ? num : kPoseMaxBones;
    __try {
        for (int b = 0; b < n; b++) {
            const uint8_t* t = cs + (size_t)b * off::kTransformStride;
            memcpy(s.poseRot[b], t + off::kTransformRotOff, 16);
            memcpy(s.posePos[b], t + off::kTransformPosOff, 12);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; s.poseN = 0; return false; }
    s.poseN = (uint8_t)n;
    g_st.captured++; g_st.bones = (uint8_t)n;
    // The blob and the feet must be dropped: 700 B of bones + 273 B of drivers + feet + header
    // overruns the 1024 B mailbox, so a packet carrying both would not ship at all. While the local
    // player scrubs they are inert anyway (0/97 fields moving), so dropping them is free. While a PEER
    // scrubs they are live, and this chooses results over drivers for everyone -- visually exact, but
    // no extrapolation on loss. That trade is only affordable because audio comes off the transported
    // funnel rather than receiver-side anim notifies.
    s.animLen = 0;
    s.feetOk = 0;
    return true;
#else
    return false;
#endif
}

bool Capture(void* mesh, State& s) {
    s.poseN = 0;
    if (!g_tun.enabled || !mesh) return false;
    // ONLY while the LOCAL player is scrubbing. Their drivers are inert (0/97 fields moving), so the
    // pose is all there is, and every receiver must get it.
    //
    // The mirror case -- a PEER is scrubbing, and their machine cannot evaluate our drivers -- is NOT
    // handled here any more, because this state is the one broadcast to everyone: pose-ifying it
    // meant one player opening the replay editor put every OTHER player on stepped 30 Hz skeletons
    // with no foot IK and no extrapolation, and any packet hiccup rendered as a driverless anim
    // graph instead of a clean freeze. The session now builds a SEPARATE results packet and unicasts
    // it to the scrubbing peers alone (CaptureFromPawn below); everyone else keeps drivers.
    if (LocalReplayMode() != g_tun.captureMode) return false;
    return captureInto(mesh, s);
}

bool CaptureFromPawn(void* pawn, State& s) {
    s.poseN = 0;
    if (!g_tun.enabled || !pawn) return false;
    void* mesh = nullptr;
#ifdef _WIN32
    __try { mesh = *(void**)((uint8_t*)pawn + off::kSkaterMesh); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    if (!mesh) return false;
    return captureInto(mesh, s);
}

// =====================================================================================================
// RECEIVER
// =====================================================================================================
static Slot* slotFor(void* mesh) {
    for (auto& sl : g_slots) if (sl.mesh == mesh) return &sl;
    return nullptr;
}
// The first mesh any slot tracks, for diagnostics that need "a live proxy mesh" and nothing more.
void* FirstProxyMesh() {
    for (auto& sl : g_slots) if (sl.mesh) return sl.mesh;
    return nullptr;
}
void Note(void* mesh, const State& s, uint64_t nowMs) {
    if (!g_tun.enabled || !mesh) return;
    if (!s.poseN) {
        // Back to live skating: release the TRANSPORTED pose so the proxy's own graph drives again.
        // It must NOT release the SLOT: the slot also carries the held pose, and dropping it here
        // would throw away the snapshot on the very frame the peer stops sending -- i.e. always,
        // since a live peer never sends one.
        Slot* sl = slotFor(mesh);
        if (sl) { sl->n = 0; sl->freshMs = 0; }
        return;
    }
    Slot* sl = slotFor(mesh);
    if (!sl) {
        for (auto& c : g_slots) if (!c.mesh) { sl = &c; break; }
        if (!sl) return;
        sl->mesh = mesh;
    }
    const int total = s.poseN < kPoseMaxBones ? s.poseN : kPoseMaxBones;
    if (sl->covTotal != (uint8_t)total) {     // re-dressed: what we have describes another skeleton
        sl->cov[0] = sl->cov[1] = sl->cov[2] = 0;
        sl->covTotal = (uint8_t)total;
        sl->n = 0;
    }
    const int first = s.poseFirst;
    int cnt = s.poseCount;
    if (first >= total) return;
    if (first + cnt > total) cnt = total - first;
    if (cnt <= 0) return;
    memcpy(&sl->rot[first], &s.poseRot[first], sizeof(float) * 4 * (size_t)cnt);
    memcpy(&sl->pos[first], &s.posePos[first], sizeof(float) * 3 * (size_t)cnt);
    for (int b = first; b < first + cnt; b++) sl->cov[b >> 5] |= (1u << (b & 31));
    // Usable only once every bone has arrived at least once. After that the pose STAYS usable and
    // later slices refresh it in place: a scrubbing skeleton barely moves between frames, so a
    // partially-refreshed pose is imperceptible, where withholding it entirely is a frozen skater.
    bool whole = true;
    for (int b = 0; b < total && whole; b++) whole = (sl->cov[b >> 5] & (1u << (b & 31))) != 0;
    if (whole) sl->n = (uint8_t)total;
    sl->freshMs = nowMs;                      // slices keep the stream alive even mid-sweep
    g_st.noted++;
}
void Forget(void* mesh) {
    Slot* sl = slotFor(mesh);
    if (sl) { sl->mesh = nullptr; sl->n = 0; sl->freshMs = 0; sl->holdN = 0; }
}

void OnFinalizeBones(void* mesh, uint64_t nowMs) {
    if (!g_tun.enabled || !mesh) return;
    Slot* sl = slotFor(mesh);
    // Fast path for the thousands of meshes that are not ours. With the hold OFF it is one pointer
    // scan and out; with it ON, one guarded deref per skeletal mesh per frame finds proxies with no
    // slot yet, which is how a proxy's mesh is adopted at all given `Note` does not create a slot for
    // a live peer.
    if (!sl && !g_tun.holdInLocalReplay) return;
    // A slot key is a POINTER, and UE recycles addresses. Before writing 70 transforms into something,
    // confirm the mesh belongs to a LIVE proxy -- otherwise a released proxy whose address was reused
    // means stamping a pose into an unrelated actor's skeleton. Never trust a cached handle; ask the
    // resolver.
    void* owner = nullptr;
    {
        __try { owner = *(void**)((uint8_t*)mesh + off::kCompOwner); }
        __except (EXCEPTION_EXECUTE_HANDLER) { owner = nullptr; }
        if (!owner || !IsProxyActor(owner)) { if (sl) Forget(mesh); return; }
    }
    if (!sl) {
        for (auto& c : g_slots) if (!c.mesh) { sl = &c; break; }
        if (!sl) return;
        sl->mesh = mesh; sl->n = 0; sl->freshMs = 0; sl->holdN = 0;
    }
    g_st.hookCalls++;
    // Measurement round: adopt the slot (the probe reads flags off its mesh) but write NOTHING --
    // no stamp, no hold. What renders is the graph's own evaluation, which is the question.
    if (debug::Get().replayDriverTest) return;
    // The replay editor shows recordings only: during playback the replay system poses every proxy
    // skeleton, and both the transported-pose stamp and the hold would overwrite it -- the
    // two-writer fight, one layer down.
    if (game::Proxy::Tuning().recordPeers && LocalReplayMode() == 2) return;
#ifdef _WIN32
    int num = 0;
    uint8_t* cs = compSpace(mesh, /*editable*/ true, &num);
    if (!cs) { g_st.faults++; return; }
    // Bone COUNT must agree exactly. Both ends run the same skeleton asset, so a mismatch means this
    // is a different mesh than expected -- write nothing rather than a scrambled pose.
    g_st.meshBones = (uint8_t)(num > 255 ? 255 : num);

    // ---- 1. A fresh TRANSPORTED pose always wins: it is the peer's actual skeleton, this instant.
    // A stale one must NOT be re-stamped forever -- hand the skeleton back rather than freezing it
    // mid-motion (the anim post-pass's freshness rule, same reasoning).
    const bool haveFresh = sl->n && !(nowMs > sl->freshMs && nowMs - sl->freshMs > g_tun.freshMs);
    if (sl->n && !haveFresh) g_st.stale++;
    if (haveFresh) {
        // Count mismatch = the two ends MERGED different meshes for this player (garments carry rig
        // bones, and an item not installed here changes the merged mesh). The base skeleton is the
        // index prefix of every merge, so the prefix IS the body: stamp what both sides have. Any
        // local bones past their count are garment extras left un-stamped -- and in the common
        // direction (their outfit is richer than our stripped-down proxy) there are none.
        int nStamp = (int)sl->n;
        if (num != (int)sl->n) {
            if (!g_tun.prefixOnMismatch) { g_st.skippedCount++; return; }
            nStamp = num < (int)sl->n ? num : (int)sl->n;
            g_st.prefixStamps++;             // visible as pfx= in the 1 Hz [pose] line
        }
        __try {
            for (int b = 0; b < nStamp; b++) {
                uint8_t* t = cs + (size_t)b * off::kTransformStride;
                memcpy(t + off::kTransformRotOff, sl->rot[b], 16);
                memcpy(t + off::kTransformPosOff, sl->pos[b], 12);
            }
            g_st.applied++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
        return;
    }
    if (!g_tun.holdInLocalReplay) return;

    // ---- 2. Normal play: the proxy's own graph just finished this pose. Remember it.
    if (LocalReplayMode() != 2) {
        const int n = num < kPoseMaxBones ? num : kPoseMaxBones;
        __try {
            for (int b = 0; b < n; b++) {
                const uint8_t* t = cs + (size_t)b * off::kTransformStride;
                memcpy(sl->holdRot[b], t + off::kTransformRotOff, 16);
                memcpy(sl->holdPos[b], t + off::kTransformPosOff, 12);
            }
            sl->holdN = (uint8_t)n;
            g_st.held++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
        return;
    }

    // ---- 3. A LOCAL replay, and no transported pose to wear. Nothing else will pose this skeleton
    // this frame: unregistering the proxy's replay component removed the only writer the replay path
    // had, and the graph's own output is unusable here -- FIELD-MEASURED, not assumed: during a local
    // replay the anim UPDATE still fires at full frame rate for proxies (the [rprobe] line), the
    // standard suppressors are clean (bPauseAnims/bNoSkeletonUpdate unset, GlobalAnimRateScale 1.0),
    // and the EVALUATION still publishes degenerate transforms -- a peer rendered from it collapses
    // into a heap of clothes. The suppression lives inside evaluation state the replay system owns;
    // do not re-chase it with the component knobs.
    // So the transported pose is the only usable writer during a replay, and this hold bridges the
    // gap before it arrives (about a second of unicast round-trip when the editor opens) and any
    // stall while it flows: re-stamp the last live pose so they stand as they last stood.
    if (!sl->holdN || (int)sl->holdN != num) { g_st.skippedCount++; return; }
    __try {
        for (int b = 0; b < (int)sl->holdN; b++) {
            uint8_t* t = cs + (size_t)b * off::kTransformStride;
            memcpy(t + off::kTransformRotOff, sl->holdRot[b], 16);
            memcpy(t + off::kTransformPosOff, sl->holdPos[b], 12);
        }
        g_st.holdApplied++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
#endif
}

}}} // namespace omp::game::pose
