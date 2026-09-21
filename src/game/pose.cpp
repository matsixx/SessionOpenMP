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
#include "omp_peers.h"
#include "pose.h"
#include "pose_blend.h"
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
// ONE PER PEER THE SESSION CAN HOLD (session.cpp kMaxPeers = 32; the anim post-pass registry beside this one,
// proxy.cpp kAnimSlots, is 32 too). IT WAS 8, and a slot is never given back while its proxy lives -- so in a
// lobby of ten, the ninth proxy on every machine had NO transported pose at all: not their sit, not their bail's
// rag-doll, not an emote. Worse for a pose that MOVES (an emote, a carried prop): its packets carry the skeleton
// INSTEAD of the drivers, so that viewer got neither -- a frozen, broken-looking skater (field, 2026-09-19:
// peers=9). Every refusal is COUNTED now (Stats::noSlot): it was silent.
static const int kSlots = 32;
static_assert(kSlots == OMP_MAX_PEERS, "per-peer table out of step with omp_peers.h");
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
    // The SENDER's skeleton, and the map from our bone index to theirs. Built lazily at stamp time
    // and only when the fingerprint or the mesh changes; -1 means "they have no bone of this name",
    // which is exactly what a garment of ours they are not wearing should get.
    uint8_t  peerN = 0;
    uint32_t peerHash[kPoseMaxBones] = {};
    int16_t  map[kPoseMaxBones];
    bool     mapReady = false;
    void*    mapBuiltFor = nullptr;
    // ...and the SKELETON the map was read from. The component survives a re-dress (the merged
    // asset on it is what changes), so the component pointer alone said "still valid" over a map
    // built for the previous outfit -- field: a peer bailing at the moment their proxy spawned had
    // the map built on the undressed 70-bone body, then the dress swapped in 95 bones under it.
    void*    mapBuiltForAsset = nullptr;
    int      mapBuiltForBones = 0;
    uint8_t  mapPeerN = 0;
    uint8_t  holdN = 0;
    float    holdRot[kPoseMaxBones][4];
    float    holdPos[kPoseMaxBones][3];
    // The sender's HOLD (OMPS): a pose-less snapshot keeps the transported pose while this is
    // fresh. The heartbeat has no clock in common with Note's nowMs, so it only arms `holdPing`;
    // the next snapshot converts that into a deadline in its own clock.
    bool     holdPing = false;
    uint32_t holdTtlMs = 0;
    uint64_t holdUntilMs = 0;
    // ...and their RELEASE, which must not be obeyed on arrival: see Note.
    bool     holdRelease = false;
    uint32_t holdReleaseMs = 0;
    // THE RELEASE FADE. The stamp is a hard overwrite, so letting go of a pose was a step function:
    // the transported skeleton one frame, the graph's own evaluation at full strength the next. The
    // graph has been running underneath the whole time and is nowhere near the posed skeleton, so
    // that step is a visible snap -- measured 2026-09-19 at 48-137 degrees on a single bone in a
    // single frame, against 2 degrees on the sender at the same instant. So the last stamped pose is
    // kept and faded out over `releaseFadeMs` instead. Local bone order (it is copied back out of the
    // component-space buffer after stamping), so the fade needs no bone map of its own.
    uint8_t  outN = 0;
    float    outRot[kPoseMaxBones][4];
    float    outPos[kPoseMaxBones][3];
    bool     outArm = false;      // Note saw the release; Apply starts the clock in ITS own timebase
    uint64_t outStartMs = 0;
    // ---- BETWEEN WHOLE SWEEPS. The snapshot stream ALREADY knows how to blend a pose (InterpStates'
    // pose branch), and on a 70-bone skater it does. It cannot on a 95-bone one: that skeleton needs two
    // packets, so consecutive snapshots carry DIFFERENT SLICES, the guard
    // (poseFirst == poseFirst && poseCount == poseCount) fails, and the pose steps. That guard is right --
    // lerping one slice against another mixes unrelated bones -- so the blend has to happen where whole
    // poses exist, which is HERE, after reassembly. Measured on a viewer: 33% of frames during a pose
    // were an exact repeat of the previous one, against 0% on the sender.
    // The last attempt at smoothing (pose.h Tuning, "TESTED AND REVERTED") failed because it read the
    // COMPONENT-SPACE ARRAY as "last frame's pose" and the animator re-stomps that array every frame.
    // These are OUR OWN copies of the last two finished sweeps; nothing is ever read back off the mesh.
    uint8_t  interpN = 0;                   // bones in the pair below (0 = nothing to blend yet)
    float    prevRot[kPoseMaxBones][4], prevPos[kPoseMaxBones][3];
    float    curRot[kPoseMaxBones][4],  curPos[kPoseMaxBones][3];
    float    blendT = 1.0f;                 // 0 = showing prev, 1 = showing cur
    float    sweepMs = 0.0f;                // measured interval between sweeps, smoothed
    uint64_t lastSweepMs = 0;
    uint64_t lastStepMs = 0;                // Apply's clock, for advancing blendT
    bool     saidNotSent = false;           // the un-stamped bones have been named once
    int16_t  headIdx = -2;                  // -2 = not looked for yet, -1 = this mesh has no head bone
};
static Slot g_slots[kSlots];
// A slot is claimed by ADDRESS, and UE hands addresses back: a proxy dies with its world, a new
// proxy's mesh lands on the same bytes. Every claim starts from nothing -- no previous occupant's
// fingerprint, map, coverage bits or hold may leak into the next mesh (field: legs broken on every
// pose-lane peer after a map change, healed only by leaving the session, which recreates the slots).
static void claimSlot(Slot* sl, void* mesh) { *sl = Slot{}; sl->mesh = mesh; }
static void (*g_logf)(const char*) = nullptr;   // optional; set via SetLogger
static bool g_localHold = false;                 // SetLocalHold: our pose is a held one (sitting)

void SetLocalHold(bool on) { g_localHold = on; }
bool CapturedForHoldOnly(const repl::State& s) {
    return g_localHold && !(g_tun.captureBails && s.bailing) && LocalReplayMode() != g_tun.captureMode;
}

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
// =====================================================================================================
// A MEASUREMENT (2026-09-19): THE END-OF-POSE TWITCH.
// Two fixes -- the release waiting for the playback, and drivers interleaved into a moving pose -- were both real
// bugs and NEITHER stopped it, and the receiver's own counters say the pose lane never drops, goes stale or
// re-stamps during a pose. So both ends now log THE SAME QUANTITY, every frame, from the moment a pose starts
// until 2 s after it ends: the rendered skeleton's first few bones (yaw of each, and how far each MOVED since the
// last frame). The sender's line is what the player sees; the receiver's is what the watcher sees. Lay the two
// side by side and the frame where they part company IS the twitch -- and which bone and how far.
//   [posedbg] me  t=... pose=1 yaw0=.. yaw1=.. yaw2=.. d0=.. d1=.. d2=.. move=..
//   [posedbg] rx0 t=... pose=1 stamped=1 ...
// Off by default: `omp::debug::Get().poseTwitch` (debug.h) turns it on, in BOTH games.
namespace {
struct DbgPrev { void* mesh = nullptr; float yaw[6] = {}; float pos[6][3] = {}; bool have = false; uint64_t lastMs = 0; uint64_t untilMs = 0; };
DbgPrev g_dbgMine, g_dbgPeer[kSlots];
// ---- THE RELEASE FADE (see Slot::outN).
// Keep what was last STAMPED, in this mesh's own bone order, straight out of the component-space
// buffer we just wrote: whatever mapping produced it is already baked in, so the fade never needs a
// bone map and can never disagree with the stamp about which bone is which.
void keepStamped(Slot* sl, const uint8_t* cs, int num) {
    const int n = num < kPoseMaxBones ? num : kPoseMaxBones;
    __try {
        for (int b = 0; b < n; b++) {
            const uint8_t* t = cs + (size_t)b * off::kTransformStride;
            memcpy(sl->outRot[b], t + off::kTransformRotOff, 16);
            memcpy(sl->outPos[b], t + off::kTransformPosOff, 12);
        }
        sl->outN = (uint8_t)n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { sl->outN = 0; }
}
// Blend the kept pose back OVER the graph's own evaluation at weight w (1 = the pose, 0 = the graph).
// Per-bone on component-space transforms -- exactly what the hard stamp does at w = 1, so w sliding
// 1 -> 0 is continuous with it by construction.
void blendKept(Slot* sl, uint8_t* cs, int num, float w) {
    const int n = (num < (int)sl->outN ? num : (int)sl->outN);
    __try {
        for (int b = 0; b < n; b++) {
            uint8_t* t = cs + (size_t)b * off::kTransformStride;
            float* q = (float*)(t + off::kTransformRotOff);
            float* p = (float*)(t + off::kTransformPosOff);
            blendQuat(q, sl->outRot[b], w);         // shortest-arc; gated in omp_posetest
            blendVec3(p, sl->outPos[b], w);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { sl->outN = 0; }
}

// Advance the sweep-to-sweep blend on Apply's own clock, and say whether there is a pair to blend.
bool stepInterp(Slot* sl, uint64_t nowMs, int num) {
    if (!g_tun.poseInterp || sl->interpN == 0 || (int)sl->interpN != (int)sl->n) return false;
    if (num < (int)sl->interpN) return false;
    // DO NOTHING WHEN THERE IS NOTHING TO FIX. When the sender's skeleton fits ONE packet -- which is
    // what dropping the feet and hands during a held pose buys -- every snapshot carries the whole
    // slice, the stream's own pose blend already applies (InterpStates' guard passes), and a sweep
    // lands every frame. Blending on top of that would only hold the skeleton a frame behind for no
    // gain. This engages for a SLICED skeleton, where sweeps are frames apart and the stream cannot.
    if (sl->sweepMs > 0.0f && sl->sweepMs < g_tun.interpMinGapMs) return false;
    const uint64_t last = sl->lastStepMs;
    sl->lastStepMs = nowMs;
    if (!last || nowMs <= last) return sl->blendT < 1.0f;
    float dt = (float)(nowMs - last);
    if (dt > 100.0f) dt = 100.0f;                        // a hitch must not jump the blend
    const float span = sl->sweepMs > 0.0f ? sl->sweepMs : 33.0f;
    g_st.interpSpanMs = span;                            // what the blend is actually spread over
    sl->blendT += dt / span;
    if (sl->blendT > 1.0f) sl->blendT = 1.0f;            // arrived: hold there until the next sweep
    return true;
}
// The pose to stamp for the SENDER's bone `r`: the blend between the last two finished sweeps.
void poseFor(const Slot* sl, int r, bool interp, float* q, float* p) {
    if (!interp) { memcpy(q, sl->rot[r], 16); memcpy(p, sl->pos[r], 12); return; }
    memcpy(q, sl->prevRot[r], 16); memcpy(p, sl->prevPos[r], 12);
    blendQuat(q, sl->curRot[r], sl->blendT);
    blendVec3(p, sl->curPos[r], sl->blendT);
}

float dbgYaw(const float* q) {
    return atan2f(2.0f * (q[3] * q[2] + q[0] * q[1]), 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2])) * 57.2957795f;
}
// One line for one skeleton, from a component-space buffer. `tag` names the end; `flags` is whatever that end
// wants to say about this frame (a pose live, a stamp written).
void dbgLine(DbgPrev& p, const char* tag, void* mesh, const uint8_t* cs, int num, bool live, const char* flags, uint64_t nowMs) {
    if (!debug::Get().poseTwitch || !cs || num <= 0 || !g_logf) return;
    if (live) p.untilMs = nowMs + 2000;                       // ...and for two seconds after it ends
    if (!live && (!p.untilMs || nowMs > p.untilMs)) { p.have = false; return; }
    if (p.mesh != mesh) { p.mesh = mesh; p.have = false; }
    if (nowMs == p.lastMs) return;                            // the seam can fire twice in a frame
    const int n = num < 6 ? num : 6;
    float yaw[6] = {}, mv[6] = {}; float worst = 0.0f; int worstB = 0;
    __try {
        for (int b = 0; b < n; b++) {
            const float* q = (const float*)(cs + (size_t)b * off::kTransformStride + off::kTransformRotOff);
            const float* v = (const float*)(cs + (size_t)b * off::kTransformStride + off::kTransformPosOff);
            yaw[b] = dbgYaw(q);
            if (p.have) {
                float dy = yaw[b] - p.yaw[b];
                while (dy > 180.0f) dy -= 360.0f;
                while (dy < -180.0f) dy += 360.0f;
                mv[b] = dy;
                const float dp = sqrtf((v[0]-p.pos[b][0])*(v[0]-p.pos[b][0]) + (v[1]-p.pos[b][1])*(v[1]-p.pos[b][1]) + (v[2]-p.pos[b][2])*(v[2]-p.pos[b][2]));
                if (fabsf(dy) > fabsf(worst)) { worst = dy; worstB = b; }
                if (dp > fabsf(worst)) { worst = dp; worstB = b; }
            }
            p.yaw[b] = yaw[b];
            for (int k = 0; k < 3; k++) p.pos[b][k] = v[k];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    const bool had = p.have;
    p.have = true; p.lastMs = nowMs;
    if (!had) return;
    char m[300];
    snprintf(m, sizeof(m), "[posedbg] %s t=%llu %s yaw=%.1f/%.1f/%.1f d=%.2f/%.2f/%.2f worst=%.2f@%d",
             tag, (unsigned long long)nowMs, flags, yaw[0], yaw[1], yaw[2], mv[0], mv[1], mv[2], worst, worstB);
    g_logf(m);
}
} // namespace

static bool captureInto(void* mesh, State& s) {
#ifdef _WIN32
    int num = 0;
    // READ buffer here: the sender samples from outside the animation pipeline (the engine-tick
    // anchor), so the published pose is the correct one to copy.
    uint8_t* cs = compSpace(mesh, /*editable*/ false, &num);
    if (!cs) return false;
    int n = num < kPoseMaxBones ? num : kPoseMaxBones;
    // Drop the trailing terminator bones. Cached per mesh because resolving names allocates; the
    // answer only changes when the character is re-dressed, which also changes the bone count.
    if (g_tun.trimLeafBones) {
        static void* cachedMesh = nullptr; static int cachedFor = 0, cachedTo = 0;
        if (mesh != cachedMesh || cachedFor != num) {
            cachedMesh = mesh; cachedFor = num;
            cachedTo = SkeletonTransportBoneCount(mesh, g_tun.trimBoardBones);
            if (cachedTo > 0 && cachedTo < num && g_logf) {
                char m[180];
                snprintf(m, sizeof(m), "[pose] transporting %d of %d bone(s) -- %d trailing bone(s)"
                                       " carry no motion a receiver needs (terminators%s)",
                         cachedTo, num, num - cachedTo,
                         g_tun.trimBoardBones ? " + the board rig, which travels as its own transform"
                                              : "");
                g_logf(m);
            }
        }
        if (cachedTo > 0 && cachedTo < n) n = cachedTo;
    }
    __try {
        for (int b = 0; b < n; b++) {
            const uint8_t* t = cs + (size_t)b * off::kTransformStride;
            memcpy(s.poseRot[b], t + off::kTransformRotOff, 16);
            memcpy(s.posePos[b], t + off::kTransformPosOff, 12);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; s.poseN = 0; return false; }
    dbgLine(g_dbgMine, "me ", mesh, cs, num, g_localHold, g_localHold ? "pose=1" : "pose=0", GetTickCount64());
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

// One dump per distinct skeleton size, ever. Names allocate, so this can never become periodic.
static void maybeDumpBones(void* mesh) {
    if (!g_tun.dumpBones || !mesh) return;
    static int dumped[4] = {0, 0, 0, 0};
    const int n = SkeletonBoneCount(mesh);
    if (n <= 0) return;
    for (int i = 0; i < 4; i++) if (dumped[i] == n) return;
    for (int i = 0; i < 4; i++) if (!dumped[i]) { dumped[i] = n; break; }
    DumpSkeletonBones(mesh, g_logf);
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
    // ...or while RAGDOLLING, which has the same shape: no drivers, so nothing to derive from. The
    // caller has already filled s.bailing this frame (gather sets it long before it gets here).
    // BEFORE the gate on purpose: this is a fact about the SKELETON, not about posing, so it must not
    // wait for a bail or a scrub to be discoverable. Capture runs every gather, the dump is one-shot
    // per distinct bone count, so it costs one integer compare per frame after it has fired.
    maybeDumpBones(mesh);
    if (LocalReplayMode() != g_tun.captureMode && !(g_tun.captureBails && s.bailing) && !g_localHold) return false;
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
void SetLogger(void (*logf)(const char*)) { g_logf = logf; }

bool SetPeerSkeleton(void* mesh, const uint32_t* hashes, int n) {
    if (!g_tun.enabled || !g_tun.skeletonSync || !mesh) return false;
    Slot* sl = slotFor(mesh);
    if (!sl) {
        // CLAIM one, exactly as Note and the hook do. A fingerprint routinely arrives before this
        // mesh has ever carried a pose, and refusing it here would throw it away for good because
        // the caller has already marked the peer fed. A slot is inert until a pose is written.
        if (!hashes || n <= 0) return false;                  // nothing to remember: do not claim
        for (auto& c : g_slots) if (!c.mesh) { sl = &c; break; }
        if (!sl) { g_st.noSlot++; return false; }             // all busy: the caller retries
        claimSlot(sl, mesh);
    }
    if (!hashes || n <= 0) { sl->peerN = 0; sl->mapReady = false; sl->mapBuiltFor = nullptr; return true; }
    if (n > kPoseMaxBones) n = kPoseMaxBones;
    // Only a CHANGED fingerprint invalidates the map -- the same one arriving again (a new peer
    // joining makes everyone re-send) must not throw away a map that is already correct.
    if (sl->peerN == (uint8_t)n && memcmp(sl->peerHash, hashes, sizeof(uint32_t) * (size_t)n) == 0) return true;
    sl->peerN = (uint8_t)n;
    memcpy(sl->peerHash, hashes, sizeof(uint32_t) * (size_t)n);
    sl->mapReady = false; sl->mapBuiltFor = nullptr;
    return true;
}

void Note(void* mesh, const State& s, uint64_t nowMs) {
    if (!g_tun.enabled || !mesh) return;
    if (!s.poseN) {
        // Back to live skating: release the TRANSPORTED pose so the proxy's own graph drives again.
        // It must NOT release the SLOT: the slot also carries the held pose, and dropping it here
        // would throw away the snapshot on the very frame the peer stops sending -- i.e. always,
        // since a live peer never sends one.
        // UNLESS the sender is HOLDING (sitting): their skeleton is static, they send it only when
        // it moves, and the OMPS heartbeat says "keep what you have".
        Slot* sl = slotFor(mesh);
        if (sl) {
            // THE RELEASE WAITS FOR THE PLAYBACK. Poses and drivers are applied from the INTERPOLATED stream,
            // which runs a jitter buffer behind arrival; the hold packets are not in that stream and land at
            // once. Obeyed on arrival, "I am done holding" dropped the pose while the frames still being played
            // out were from the middle of the sender's blend-out -- the last 50-250 ms of it never shown, so the
            // skeleton jumped to whatever the proxy's own graph had. Field 2026-09-19, every pose, sitting
            // included: "right before their pose ends, their body turns/twitches"; invisible to the sender, who
            // has no buffer. So a release is filed as "hold it this much longer" (session.cpp passes the
            // stream's own delay) and the pose is let go when the playback has caught up with it.
            if (sl->holdRelease) { sl->holdRelease = false; sl->holdPing = false; sl->holdUntilMs = sl->n ? nowMs + sl->holdReleaseMs : 0; }
            else if (sl->holdPing) { sl->holdUntilMs = nowMs + sl->holdTtlMs; sl->holdPing = false; }
            if (sl->n && sl->holdUntilMs && nowMs < sl->holdUntilMs) { sl->freshMs = nowMs; return; }
            if (sl->n) g_st.wiped++;      // a completed pose thrown away by a pose-less frame
            // Hand the skeleton back over `releaseFadeMs` rather than in one frame. Armed here and
            // clocked in Apply: this runs on the network path, and only Apply's timebase is the one
            // the fade is measured in.
            if (sl->n && sl->outN) sl->outArm = true;
            sl->n = 0; sl->freshMs = 0;
        }
        return;
    }
    Slot* sl = slotFor(mesh);
    if (!sl) {
        for (auto& c : g_slots) if (!c.mesh) { sl = &c; break; }
        if (!sl) { g_st.noSlot++; return; }
        claimSlot(sl, mesh);
    }
    const int total = s.poseN < kPoseMaxBones ? s.poseN : kPoseMaxBones;
    if (sl->covTotal != (uint8_t)total) {     // re-dressed: what we have describes another skeleton
        sl->cov[0] = sl->cov[1] = sl->cov[2] = 0;
        sl->covTotal = (uint8_t)total;
        sl->n = 0;
    }
    const int first = s.poseFirst;
    int cnt = s.poseCount;
    if (first >= total) { g_st.noSlice++; return; }
    // A NEW SWEEP IS ABOUT TO OVERWRITE THE FINISHED ONE. Slices refresh `rot`/`pos` IN PLACE, so the
    // instant before the first slice of the next sweep lands is the only moment this buffer holds one
    // coherent skeleton: after it, bones 0..47 are the new sweep and the rest are still the old one.
    // That is what gets kept as a blend endpoint -- never a half-updated mix.
    if (g_tun.poseInterp && first == 0 && sl->n == (uint8_t)total) {
        const int nb = total < kPoseMaxBones ? total : kPoseMaxBones;
        if (sl->interpN == (uint8_t)nb) {
            // PREV := WHAT IS ON SCREEN, not the last sweep. Poses land every ~25 ms and a frame is
            // ~16, so the blend is typically two thirds finished when the next one arrives. Taking
            // `cur` as the new start throws that last third away and jumps the skeleton forward by it
            // -- once per pose, 40 times a second, which IS the chop this was built to remove. Field
            // 2026-09-20: "still choppy", with interp/s == noted/s and span 25 ms proving the blend
            // was running and therefore that the fault was in the handover, not in whether it ran.
            for (int b = 0; b < nb; b++) {
                blendQuat(sl->prevRot[b], sl->curRot[b], sl->blendT);
                blendVec3(sl->prevPos[b], sl->curPos[b], sl->blendT);
            }
        } else {                                   // first pair: start ON the new pose, so nothing jumps
            memcpy(sl->prevRot, sl->rot, sizeof(float) * 4 * (size_t)nb);
            memcpy(sl->prevPos, sl->pos, sizeof(float) * 3 * (size_t)nb);
        }
        memcpy(sl->curRot, sl->rot, sizeof(float) * 4 * (size_t)nb);
        memcpy(sl->curPos, sl->pos, sizeof(float) * 3 * (size_t)nb);
        sl->interpN = (uint8_t)nb;
        if (sl->lastSweepMs && nowMs > sl->lastSweepMs) {
            const float iv = (float)(nowMs - sl->lastSweepMs);
            // Smoothed, and bounded: a stall must not stretch the blend into a slow-motion crawl.
            sl->sweepMs = sl->sweepMs > 0.0f ? sl->sweepMs * 0.7f + iv * 0.3f : iv;
            if (sl->sweepMs > 250.0f) sl->sweepMs = 250.0f;
            if (sl->sweepMs < 8.0f)   sl->sweepMs = 8.0f;
        }
        sl->lastSweepMs = nowMs;
        sl->blendT = 0.0f;                         // ...and glide from the old one to this
        g_st.interpSweeps++;
    }
    if (first + cnt > total) cnt = total - first;
    if (cnt <= 0) { g_st.noSlice++; return; }
    memcpy(&sl->rot[first], &s.poseRot[first], sizeof(float) * 4 * (size_t)cnt);
    memcpy(&sl->pos[first], &s.posePos[first], sizeof(float) * 3 * (size_t)cnt);
    for (int b = first; b < first + cnt; b++) sl->cov[b >> 5] |= (1u << (b & 31));
    // Usable only once every bone has arrived at least once. After that the pose STAYS usable and
    // later slices refresh it in place: a scrubbing skeleton barely moves between frames, so a
    // partially-refreshed pose is imperceptible, where withholding it entirely is a frozen skater.
    bool whole = true;
    for (int b = 0; b < total && whole; b++) whole = (sl->cov[b >> 5] & (1u << (b & 31))) != 0;
    // The LARGEST slice seen, not the last: the final slice of a sweep is whatever remainder is
    // left (five bones of a 95-bone skeleton, say), so reporting the last one makes a healthy
    // stream look starved. The max is the sender's actual per-packet budget.
    if ((uint8_t)cnt > g_st.sliceBones) g_st.sliceBones = (uint8_t)cnt;
    if (whole && !sl->n) g_st.sweeps++;
    if (whole) sl->n = (uint8_t)total;
    g_st.liveN = sl->n;
    sl->freshMs = nowMs;                      // slices keep the stream alive even mid-sweep
    g_st.noted++;
}
bool PoseDrivingActorHead(void* actor) {
    if (!g_tun.enabled || !actor) return false;
    void* mesh = nullptr;
#ifdef _WIN32
    __try { mesh = *(void**)((uint8_t*)actor + off::kSkaterMesh); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
    if (!mesh) return false;
    const Slot* sl = slotFor(mesh);
    if (!sl || !sl->n) return false;
    // Fresh only: a pose that has gone stale hands the skeleton back to the graph, and the head-look
    // should come back with it rather than leaving the head frozen.
    const uint64_t now = GetTickCount64();
    return !(sl->freshMs && now > sl->freshMs && now - sl->freshMs > g_tun.freshMs);
}
void Forget(void* mesh) {
    Slot* sl = slotFor(mesh);
    if (sl) *sl = Slot{};                  // the whole slot: a released mesh's address comes back
}
void ForgetAll() {
    for (auto& sl : g_slots) sl = Slot{};
}
void NoteHold(void* mesh, bool hold, uint32_t ttlMs) {
    if (!g_tun.enabled || !mesh) return;
    Slot* sl = slotFor(mesh);
    if (!sl) {
        if (!hold) return;
        for (auto& c : g_slots) if (!c.mesh) { sl = &c; break; }
        if (!sl) { g_st.noSlot++; return; }
        claimSlot(sl, mesh);
    }
    if (hold) { sl->holdPing = true; sl->holdTtlMs = ttlMs; sl->holdRelease = false; }
    else      { sl->holdRelease = true; sl->holdReleaseMs = ttlMs; }   // ...once the playback has caught up (Note)
}

// Our bone index -> the SENDER's, by NAME. Two players' merged skeletons agree on names and on
// nothing else, so this is the only mapping that survives either of them wearing something the other
// is not. Rebuilt when the mesh or the peer's fingerprint changes; -1 means "they have no bone of
// this name", which is exactly what a garment of ours they are not wearing should get.
// Extracted from the pose-stamp branch it used to live inside. It stays a function rather than going
// back inline because the map is a property of the SLOT, not of the stamp: anything that needs to
// name a peer's bones needs it built, and an ordinary riding frame never reaches the stamp.
static void EnsureBoneMap(Slot* sl, void* mesh) {
    if (!g_tun.skeletonSync || !g_tun.nameKeyedBones || !sl->peerN) return;
    // The key is the SKELETON, not the component: the asset on the component and its bone count.
    void* asset = nullptr;
    __try { asset = *(void**)((uint8_t*)mesh + off::kMeshSkeletalMesh); }
    __except (EXCEPTION_EXECUTE_HANDLER) { asset = nullptr; }
    const int bones = game::SkeletonBoneCount(mesh);
    if (sl->mapReady && sl->mapBuiltFor == mesh && sl->mapPeerN == sl->peerN &&
        sl->mapBuiltForAsset == asset && sl->mapBuiltForBones == bones) return;
    uint32_t localHash[kPoseMaxBones];
    const int ln = game::SkeletonBoneHashes(mesh, localHash, kPoseMaxBones);
    if (ln <= 0) return;
    int mapped = 0;
    for (int b = 0; b < kPoseMaxBones; b++) sl->map[b] = -1;
    for (int b = 0; b < ln && b < kPoseMaxBones; b++) {
        for (int r = 0; r < (int)sl->peerN; r++)
            if (sl->peerHash[r] == localHash[b]) { sl->map[b] = (int16_t)r; mapped++; break; }
    }
    sl->mapReady = true; sl->mapBuiltFor = mesh; sl->mapPeerN = sl->peerN;
    sl->mapBuiltForAsset = asset; sl->mapBuiltForBones = bones;
    g_st.mappedBones = (uint8_t)(mapped > 255 ? 255 : mapped);
    g_st.unmappedBones = (uint8_t)((ln - mapped) > 255 ? 255 : (ln - mapped));
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
        claimSlot(sl, mesh);
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
    if (sl->n && !haveFresh) {
        g_st.stale++;
        // A pose going stale hands the skeleton back exactly as abruptly as a release does -- same
        // snap, different cause -- so it fades out too. Armed only on the way in: `outStartMs` is set
        // once the fade begins and `outN` is cleared once it ends, so neither can re-arm it and hold
        // the fade at full weight forever.
        if (sl->outN && !sl->outStartMs) sl->outArm = true;
        // Field logs showed stale climbing into the thousands while observing a scrubbing peer;
        // believed benign (a PAUSED scrubber publishes no fresh pose), but believed is not known --
        // one throttled line makes the cause readable instead of inferred.
        static uint64_t lastSaidMs = 0;
        if (nowMs > lastSaidMs + 5000) {
            lastSaidMs = nowMs;
            if (g_logf) { char m[160];
                snprintf(m, sizeof(m), "[pose] transported pose is STALE (age %u ms) -- the sender has"
                         " stopped publishing poses (paused scrub, or their stream stalled)",
                         (unsigned)(nowMs - sl->freshMs));
                g_logf(m); }
        }
    }
    // Blend between the last two finished sweeps rather than stepping onto each one (see Slot::interpN).
    const bool interp = haveFresh && stepInterp(sl, nowMs, num);
    if (haveFresh) {
        // Count mismatch = the two ends MERGED different meshes for this player (garments carry rig
        // bones, and an item not installed here changes the merged mesh). The base skeleton is the
        // index prefix of every merge, so the prefix IS the body: stamp what both sides have. Any
        // local bones past their count are garment extras left un-stamped -- and in the common
        // direction (their outfit is richer than our stripped-down proxy) there are none.
        // ---- BY NAME, when the sender has told us what their bones are called. Built once per
        // (mesh, fingerprint): read THIS mesh's names, then for each local bone find the sender's
        // bone of the same name. Two players' merged skeletons agree on names and on nothing else,
        // so this is the only mapping that stays correct when either side is wearing something the
        // other is not -- including a garment the bone floor has substituted.
        EnsureBoneMap(sl, mesh);
        if (g_tun.skeletonSync && g_tun.nameKeyedBones && sl->mapReady && sl->mapBuiltFor == mesh &&
            g_st.mappedBones > 0) {
            int untransported = 0;
            // The head as the GRAPH left it this frame, before we overwrite it. Three values make the
            // buffers unambiguous: if `published` matches THIS, the read buffer is the graph's output
            // and the probe was measuring nothing; if it matches what we STAMP, ours is what renders.
            float headPre[4] = { 0, 0, 0, 0 };
            if (debug::Get().headProbe && sl->headIdx >= 0 && sl->headIdx < num)
                memcpy(headPre, cs + (size_t)sl->headIdx * off::kTransformStride + off::kTransformRotOff, 16);
            __try {
                const int nLocal = num < kPoseMaxBones ? num : kPoseMaxBones;
                for (int b = 0; b < nLocal; b++) {
                    const int r = sl->map[b];
                    // A bone the sender does not have keeps whatever the local graph put there --
                    // a garment bone of ours they cannot speak for, which is exactly right.
                    // A bone the sender HAS a name for but did NOT transport (their pose was
                    // trimmed shorter than their fingerprint) is left to the proxy's own graph. That
                    // is invisible and it is not always harmless: during a moving pose the graph has
                    // no fresh drivers, so such a bone drifts on stale input while the rest of the
                    // skeleton is exact. Counted so "some bones are not ours" can never be silent.
                    if (r >= (int)sl->n) {
                        // NAME them, once per slot. "37 bones are not ours" is only actionable if it
                        // says WHICH: inert leaf terminators and the board rig are trimmed on purpose
                        // and skipping them is correct, but a real bone here is a hole in the pose that
                        // the proxy's own graph fills -- and during a moving pose that graph has no
                        // fresh drivers. Field 2026-09-20: a peer's head twisting during emotes only.
                        if (!sl->saidNotSent && g_logf && untransported < 24) {
                            char bn[64] = "?";
                            game::SkeletonBoneName(mesh, b, bn, sizeof(bn));
                            char m[160];
                            snprintf(m, sizeof(m), "[pose] not sent by them: local bone %d '%s' -> their %d (they sent %d)",
                                     b, bn, r, (int)sl->n);
                            g_logf(m);
                        }
                        untransported++; continue;
                    }
                    if (r < 0) continue;
                    uint8_t* t = cs + (size_t)b * off::kTransformStride;
                    float q[4], pv[3];
                    poseFor(sl, r, interp, q, pv);
                    memcpy(t + off::kTransformRotOff, q, 16);
                    memcpy(t + off::kTransformPosOff, pv, 12);
                }
                g_st.applied++; g_st.mappedStamps++;
                if (untransported > g_st.untransported) g_st.untransported = (uint8_t)(untransported > 255 ? 255 : untransported);
                if (untransported) sl->saidNotSent = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
            // ---- WHAT HAPPENS TO THE HEAD AFTER WE WRITE IT (debug::poseTwitch).
            // We stamp the EDITABLE buffer and the engine publishes it; the READ buffer therefore holds
            // what was actually RENDERED last frame. If they disagree on the head, something is writing
            // it after us -- and the ANGLE BETWEEN THEM is how much. Field 2026-09-20: a watched head
            // pitches down much further than the sender's, i.e. the same turn applied about twice.
            if (debug::Get().headProbe) {
                if (sl->headIdx == -2) {
                    sl->headIdx = -1;
                    for (int b2 = 0; b2 < num && b2 < kPoseMaxBones; b2++) {
                        char bn[64] = "";
                        if (!game::SkeletonBoneName(mesh, b2, bn, sizeof(bn))) continue;
                        size_t L = strlen(bn);
                        if (L >= 4 && _stricmp(bn + L - 4, "head") == 0) { sl->headIdx = (int16_t)b2; break; }
                    }
                    if (g_logf) { char m[120];
                        snprintf(m, sizeof(m), "[posedbg] head bone = local %d (of %d)", (int)sl->headIdx, num);
                        g_logf(m); }
                }
                static uint64_t saidMs = 0;
                if (sl->headIdx >= 0 && g_logf && nowMs > saidMs + 1000) {
                    saidMs = nowMs;
                    int rn = 0; const uint8_t* rd = compSpace(mesh, /*editable*/ false, &rn);
                    if (rd && sl->headIdx < rn) {
                        const float* mine2 = (const float*)(cs + (size_t)sl->headIdx * off::kTransformStride + off::kTransformRotOff);
                        const float* pub   = (const float*)(rd + (size_t)sl->headIdx * off::kTransformStride + off::kTransformRotOff);
                        float d = mine2[0]*pub[0] + mine2[1]*pub[1] + mine2[2]*pub[2] + mine2[3]*pub[3];
                        if (d < 0.0f) d = -d; if (d > 1.0f) d = 1.0f;
                        const float deg = 2.0f * acosf(d) * 57.2957795f;
                        char m[200];
                        float dg = headPre[0]*pub[0] + headPre[1]*pub[1] + headPre[2]*pub[2] + headPre[3]*pub[3];
                        if (dg < 0.0f) dg = -dg; if (dg > 1.0f) dg = 1.0f;
                        const float degGraph = 2.0f * acosf(dg) * 57.2957795f;
                        snprintf(m, sizeof(m), "[posedbg] head: graph(%.3f %.3f %.3f %.3f) ours(%.3f %.3f %.3f %.3f) "
                                 "published(%.3f %.3f %.3f %.3f) | published vs ours %.1f, vs graph %.1f -- %s",
                                 headPre[0], headPre[1], headPre[2], headPre[3],
                                 mine2[0], mine2[1], mine2[2], mine2[3], pub[0], pub[1], pub[2], pub[3],
                                 deg, degGraph,
                                 degGraph < 5.0f ? "the READ buffer IS the graph: probe was measuring nothing"
                                                 : (deg > 5.0f ? "SOMETHING MOVES IT AFTER US" : "ours is what renders"));
                        g_logf(m);
                    }
                }
            }
            keepStamped(sl, cs, num);       // ...so the release has something to fade out of
            sl->outArm = false; sl->outStartMs = 0;
            { char f[40]; snprintf(f, sizeof(f), "pose=1 stamped=1"); dbgLine(g_dbgPeer[(int)(sl - g_slots)], "rx ", mesh, cs, num, true, f, nowMs); }
            return;
        }
        // ---- BY INDEX. No fingerprint (a peer on an older build), or their names matched nothing.
        int nStamp = (int)sl->n;
        if (num != (int)sl->n) {
            if (!g_tun.prefixOnMismatch) { g_st.skippedCount++; return; }
            nStamp = num < (int)sl->n ? num : (int)sl->n;
            g_st.prefixStamps++;             // visible as pfx= in the 1 Hz [pose] line
        }
        __try {
            for (int b = 0; b < nStamp; b++) {
                uint8_t* t = cs + (size_t)b * off::kTransformStride;
                float q[4], pv[3];
                poseFor(sl, b, interp, q, pv);
                memcpy(t + off::kTransformRotOff, q, 16);
                memcpy(t + off::kTransformPosOff, pv, 12);
            }
            g_st.applied++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
        keepStamped(sl, cs, num);           // ...so the release has something to fade out of
        sl->outArm = false; sl->outStartMs = 0;
        dbgLine(g_dbgPeer[(int)(sl - g_slots)], "rx ", mesh, cs, num, true, "pose=1 stamped=1", nowMs);
        return;
    }
    // NOT stamped this frame: the graph's own pose is what this proxy wears -- but if a pose was just
    // released, it is faded out from underneath rather than dropped (see Slot::outN). The first fade
    // frame is what was already on screen, so there is nothing to step over; by the last one the
    // graph owns the skeleton outright.
    bool faded = false;
    if (sl->outN && g_tun.releaseFadeMs) {
        if (sl->outArm) { sl->outArm = false; sl->outStartMs = nowMs; }
        if (sl->outStartMs) {
            const uint64_t age = nowMs > sl->outStartMs ? nowMs - sl->outStartMs : 0;
            if (age >= g_tun.releaseFadeMs) { sl->outStartMs = 0; sl->outN = 0; }
            else {
                const float u = (float)age / (float)g_tun.releaseFadeMs;
                blendKept(sl, cs, num, fadeWeight(u));
                g_st.fadeFrames++;
                faded = true;
            }
        }
    }
    // The frames after a release are exactly the ones the twitch was in, so they are logged too
    // (for two seconds -- see dbgLine).
    dbgLine(g_dbgPeer[(int)(sl - g_slots)], "rx ", mesh, cs, num, false,
            sl->n ? "pose=1 stamped=0" : (faded ? "pose=0 fading=1" : "pose=0 stamped=0"), nowMs);

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
