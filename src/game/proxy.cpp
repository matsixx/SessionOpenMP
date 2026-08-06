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
// SessionOpenMP -- proxy lifecycle + apply.
#include "proxy.h"
#include "../debug.h"
#include "../session/session.h"
#include "game_syms.h"
#include "audio.h"
#include "pose.h"
#include "spectate.h"          // hand the replay camera back before this actor's pointer dies
#include "../replication/anim_fields.h"

namespace omp { namespace game {
static void StoreAnimForPostPass(Proxy* owner, void* ai, const repl::State& s, uint64_t nowMs);
static void DropAnimSlot(Proxy* owner);
}}
#include <cstring>
#include <cstdio>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game {

static ProxyTuning g_tun;
ProxyTuning& Proxy::Tuning() { return g_tun; }

// ---- how many transported names have been REJECTED by a type gate, process-wide.
// A counter rather than log lines: the per-site logs are `static int n; if (++n <= 8)`, capped
// permanently and process-wide, so a gate that started firing would go invisible after the eighth
// line. This rides the 1 Hz board telemetry as `rej=`, where a rising number is unmissable. In normal
// play it stays 0 forever; anything else is either an attack or a wrong expectation, and the gate
// self-check distinguishes those.
static uint32_t g_typeRejects = 0;
uint32_t TypeRejects() { return g_typeRejects; }

// ---- safe reads: a proxy's fields are read constantly and a torn/freed pointer must never fault
#ifdef _WIN32
static void* safePtr(void* base, int off) {
    if (!base) return nullptr;
    __try { return *(void**)((uint8_t*)base + off); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int safeByte(void* base, int off) {
    if (!base) return -1;
    __try { return *(uint8_t*)((uint8_t*)base + off); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static bool safeRead(void* p, void* dst, int n) {
    if (!p) return false;
    __try { memcpy(dst, p, (size_t)n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
static void* safePtr(void*, int) { return nullptr; }
static int   safeByte(void*, int) { return -1; }
static bool  safeRead(void*, void*, int) { return false; }
#endif

// quaternion helpers (same conventions as the replication core)
static void qConj(const float* q, float* o) { o[0]=-q[0]; o[1]=-q[1]; o[2]=-q[2]; o[3]=q[3]; }
static void qMul(const float* a, const float* b, float* o) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
static void qRotV(const float* q, const float* v, float* o) {
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float tx=2*(y*v[2]-z*v[1]), ty=2*(z*v[0]-x*v[2]), tz=2*(x*v[1]-y*v[0]);
    o[0]=v[0]+w*tx+(y*tz-z*ty); o[1]=v[1]+w*ty+(z*tx-x*tz); o[2]=v[2]+w*tz+(x*ty-y*tx);
}
static void quatToRot3(const float* q, float* r) {   // UE FRotator (pitch,yaw,roll) degrees
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float sing = w*y - x*z, r2d = 57.2957795f;
    if (fabsf(sing) > 0.49999f) { r[0] = sing>0?90.f:-90.f; r[1] = 2.f*atan2f(z,w)*r2d*(sing>0?1.f:-1.f); r[2]=0; return; }
    r[0] = asinf(2.f*sing)*r2d;
    r[1] = atan2f(2.f*(w*z+x*y), 1.f-2.f*(y*y+z*z))*r2d;
    r[2] = atan2f(2.f*(w*x+y*z), 1.f-2.f*(x*x+z*z))*r2d;
}

// =====================================================================================================
// SPAWN
// =====================================================================================================
bool Proxy::EnsureSpawned(void* ownPawn, const repl::State& first, uint64_t nowMs, void (*logf)(const char*)) {
    if (actor_) return true;
    const Syms& S = Get();
    // A permanent, unrecoverable condition must announce itself ONCE -- and only once, since this runs
    // every frame. A silent early-out here hides behind a healthy-looking log indefinitely.
    if (!S.SpawnActor || !S.GetWorld || !ownPawn) {
        if (logf && !S.SpawnActor) { static bool said = false; if (!said) { said = true; logf("[proxy] cannot spawn: SpawnActor unresolved"); } }
        if (logf && !S.GetWorld)   { static bool said = false; if (!said) { said = true; logf("[proxy] cannot spawn: GetWorld unresolved"); } }
        return false;
    }
    // One attempt per second, bounded. A capped log in front of an uncapped action is a lie: retrying
    // every frame behind a 3-line failure log reads as "3 failures" while a SpawnActor storm runs.
    if (nowMs - lastTryMs_ < (uint64_t)g_tun.spawnRetryMs) return false;
    lastTryMs_ = nowMs;
    if (tries_ >= g_tun.spawnMaxTries) return false;
    tries_++; st_.spawnTries++;

    world_ = S.GetWorld(ownPawn);
    void* cls = safePtr(ownPawn, 0x10);          // the local player's skater class: guaranteed live and
                                                 // in THIS world (a pre-travel class spawns null)
    if (!world_ || !cls) {
        if (logf) { char m[160]; snprintf(m, sizeof(m),
            "[proxy] spawn precondition failed (try %d): world=%p cls=%p", tries_, world_, cls); logf(m); }
        return false;
    }

    // Offset, never the exact transported position -- their own actor is standing there.
    const float* src = first.bodyPosOk ? first.bodyPos : first.deckPos;
    float loc[3] = { src[0] + g_tun.spawnOffsetCm, src[1] + g_tun.spawnOffsetCm, src[2] + 50.f };
    float rot[3] = { 0, 0, 0 };
    uint8_t params[0x80]; memset(params, 0, sizeof(params));
    void* p = nullptr;
    unsigned long xcode = 0; void* xaddr = nullptr;
#ifdef _WIN32
    // The guard stops a fault killing the game, but a caught fault is NOT a recovery: the engine's
    // stack unwound without its cleanup, a half-built actor is likely abandoned in the world, and
    // loader/GC state may be poisoned (it surfaces later as an FLinkerLoad crash). So a fault is LOUD
    // and FINAL -- log code+address and never try again on this slot; retrying manufactures more
    // broken actors.
    __try { p = S.SpawnActor(world_, cls, loc, rot, params); }
    __except (xcode = GetExceptionCode(),
              xaddr = (GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
              EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
#endif
    if (!p) {
        st_.spawnFails++;
        if (xcode) {
            const int tryNo = tries_;              // capture BEFORE the give-up overwrite, or the log
            tries_ = g_tun.spawnMaxTries;          // reports the cap instead of the real attempt
            if (logf) { char m[240]; snprintf(m, sizeof(m),
                "[proxy] spawn FAULTED code=0x%08lX at=%p (try %d) -- half-built actor likely leaked; "
                "NOT retrying this peer", xcode, xaddr, tryNo); logf(m); }
        } else if (logf) { char m[200]; snprintf(m, sizeof(m),
            "[proxy] spawn returned null (try %d/%d) world=%p cls=%p", tries_, g_tun.spawnMaxTries, world_, cls);
            logf(m); }
        return false;
    }
    actor_ = p; bornMs_ = nowMs;

    // MANDATORY. ASkaterCharacter's class default is bReplicates=true, and a HOST is the server: its
    // spawned actors replicate to every client. The host's proxy-of-the-joiner then materialises ON THE
    // JOINER as a Role-1 skater glued to their own transported position, capsule and all -- a second
    // skater standing inside them, an endless bail loop, and board-name theft via the replicated board
    // arriving named `Skateboard`. Must run in the SAME FRAME as the spawn: the net driver replicates
    // on its NEXT tick, so no replica is ever created.
    if (S.SetReplicates) {
#ifdef _WIN32
        __try { S.SetReplicates(actor_, false); repOff_ = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    // A spawned skater's own Tick() derefs state it never got (UpdatePendingGotoMarker -> AV). A
    // wire-driven proxy has no use for its own Tick: goto markers, local input and its own board/body
    // logic are exactly what the apply path replaces. SetActorTickEnabled disables the ACTOR tick ONLY
    // -- component ticks (movement component, skeletal mesh/anim) keep running, which the apply needs.
    if (S.SetActorTick) {
#ifdef _WIN32
        __try { S.SetActorTick(actor_, false); tickOff_ = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    // ---- TAKE THE PROXY OUT OF THE REPLAY SYSTEM AT THE SOURCE.
    // Actors opt into the replay editor BY TAG: AReplayManager::AddReplayComponents runs
    // UGameplayStatics::GetAllActorsWithTag for NAME_ACTORTAG_ActorReplayComponent,
    // ...CameraReplayComponent, ...AnimInstanceReplayComponent, ...FilmerReplayComponent and the rest,
    // then registers whatever it finds. A proxy is spawned from the LOCAL PLAYER'S skater class, so it
    // carries those tags, and the manager would register the PROXY's replay components alongside the
    // real player's and tick them across replay sessions they never recorded for -- which is where the
    // AVs inside TickReplaying -> Filmer/CameraReplayComponent::Replaying come from. Clearing the tag
    // list closes the whole class of fault at its source instead of guarding each consumer.
    // Zeroing Num is safe: the array still owns and frees its allocation; it simply reports empty.
    // Being undiscoverable-by-tag is correct for a proxy in general, like replication off, actor tick
    // off and the bail veto.
#ifdef _WIN32
    __try {
        int* tagNum = (int*)((uint8_t*)actor_ + off::kActorTagsNum);
        if (*tagNum > 0 && *tagNum < 64) *tagNum = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    // Registered HERE, at spawn: every guard that asks "is this actor one of ours?" must get the
    // right answer from the actor's first frame to its last, independently of whether packets are
    // flowing.
    NoteProxyActor(actor_, nullptr);
    st_.alive = true;
    if (logf) { char m[260]; snprintf(m, sizeof(m),
        "[proxy] SPAWNED %p at (%.0f,%.0f,%.0f) role=%d | replication OFF=%d actorTick OFF=%d",
        actor_, loc[0], loc[1], loc[2], safeByte(actor_, off::kActorRole), (int)repOff_, (int)tickOff_);
        logf(m); }
    return true;
}

void Proxy::Forget() {
    // The world changed: UE destroyed every actor in it. A dangling pointer here is worse than no proxy
    // -- once UE recycles the address it mis-tags a REAL skater.
    // OnActorGone must run BEFORE DropProxyActor: it asks IsProxyActor whether this is still ours, so
    // clearing the registry first makes it decline to clean up the very actor it exists to clean up.
    spectate::OnActorGone(actor_, nullptr);
    DropProxyActor(actor_);               // no longer one of ours -- drop it before the pointer dies
    DropAnimSlot(this);                   // the anim instance died with the world; never post-apply to it
    { void* pm = actor_ ? safePtr(actor_, off::kSkaterMesh) : nullptr; if (pm) pose::Forget(pm); }
    // The peer's sounds died with their components. Drop the handles WITHOUT calling Stop -- the
    // objects are gone -- and the audio slots must not survive into a replacement actor either, for
    // the same reason the change-edge caches below are cleared.
    for (auto& l : audioLoops_) { l.slot = 0; l.comp = nullptr; }
    actor_ = nullptr; world_ = nullptr; tries_ = 0; lastTryMs_ = 0;
    refreshed_ = repOff_ = boardRepOff_ = tickOff_ = boardHidden_ = simOn_ = boardLogged_ = false;
    nearLocal_ = true; animThrottled_ = false;
    lastBailing_ = 0;
    // change-edge caches too: a REPLACEMENT actor must get every write again even when the wire value
    // never changed across the world change (a matching cache would skip its first def/state write).
    lastPushState_ = lastBrakeState_ = 0; lastCrankOn_ = 0; lastCrankIdx_ = 0xfffe;
    memset(lastTrickName_, 0, sizeof(lastTrickName_)); memset(lastGrindName_, 0, sizeof(lastGrindName_));
    trickDef_ = nullptr;
    st_.alive = false; st_.boardOwned = false;
}

// =====================================================================================================
// BOARD OWNERSHIP
// =====================================================================================================
void* Proxy::OwnBoard() const {
    // `proxy+0x568` can point at the LOCAL PLAYER'S board, and the game can move the back-link onto the
    // proxy, so a one-sided ownership check PASSES on the wrong board -- and everything then done to
    // "the proxy's board" happens to the player's: hidden, and teleported onto the remote pose 60x/s.
    // A board is ours ONLY if BOTH links agree.
    void* bd = safePtr(actor_, off::kSkaterBoard);
    if (!bd) return nullptr;
    if (safePtr(bd, off::kBoardSkater) != actor_) return nullptr;
    return bd;
}

// =====================================================================================================
// THE ANIM POST-PASS REGISTRY. Proxy::Apply stores each frame's blob here; the loader's POST-hook on
// USkaterAnimInstance::NativeUpdateAnimation calls AnimPostApply(instance), and a match applies the
// stored blob as the LAST writer before pose evaluation. Freshness-gated: a quiet stream stops feeding
// the slot, the write stops, and the proxy's own graph takes over (OnQuiet semantics).
// =====================================================================================================
static const int kAnimSlots = 16;
struct AnimSlot {
    Proxy*   owner = nullptr;
    void*    ai = nullptr;
    uint16_t len = 0;
    uint64_t freshMs = 0;
    uint8_t  blob[320];
    // the foot sockets: raw world, written after the blob in the post-pass
    uint8_t  feetOk = 0;
    float    lFootPos[3], lFootRot[3], rFootPos[3], rFootRot[3];
    // the hand IK targets, same treatment -- see the offsets in game_syms.h for why an alpha without
    // its target is not safe to send on its own
    uint8_t  handOk = 0;
    float    lHandPos[3], lHandRot[3], rHandPos[3], rHandRot[3];
    // WIRE crank values for the animgate line: printing the POST-GATE anim fields instead shows 0 even
    // where the wire carried the crank correctly.
    uint8_t  wireCrankOn = 0;
    uint16_t wireCrankIdx = 0xffff;
    float    wireCrankPocket = 0;
    // the wire's onBoard, for the transition-pose save edge (see AnimPostApply).
    uint8_t  onBoard = 0;
    int8_t   lastOnBoardSaved = -1;   // -1 = no edge yet (a fresh slot must not "transition")
};
static AnimSlot g_animSlots[kAnimSlots];

static void StoreAnimForPostPass(Proxy* owner, void* ai, const repl::State& s, uint64_t nowMs) {
    if (!owner || !ai || !s.animLen || s.animLen > sizeof(AnimSlot::blob)) return;
    AnimSlot* mine = nullptr;
    for (auto& s : g_animSlots) if (s.owner == owner) { mine = &s; break; }
    if (!mine) for (auto& s : g_animSlots) if (!s.owner) { mine = &s; break; }
    if (!mine) return;
    mine->owner = owner; mine->ai = ai; mine->len = s.animLen; mine->freshMs = nowMs;
    memcpy(mine->blob, s.anim, s.animLen);
    mine->feetOk = (uint8_t)(s.feetOk && s.feetWorld);
    mine->handOk = (uint8_t)(s.handOk && s.handWorld);
    if (mine->handOk) {
        memcpy(mine->lHandPos, s.lHandPos, 12); memcpy(mine->lHandRot, s.lHandRot, 12);
        memcpy(mine->rHandPos, s.rHandPos, 12); memcpy(mine->rHandRot, s.rHandRot, 12);
    }
    if (mine->feetOk) {
        memcpy(mine->lFootPos, s.lFootPos, 12); memcpy(mine->lFootRot, s.lFootRot, 12);
        memcpy(mine->rFootPos, s.rFootPos, 12); memcpy(mine->rFootRot, s.rFootRot, 12);
    }
    mine->wireCrankOn = s.crankOn; mine->wireCrankIdx = s.crankDefOff; mine->wireCrankPocket = s.crankPocket;
    mine->onBoard = (uint8_t)(s.onBoard ? 1 : 0);
}
static void DropAnimSlot(Proxy* owner) {
    for (auto& s : g_animSlots) if (s.owner == owner) s = AnimSlot{};
}

static void (*g_animLogf)(const char*) = nullptr;   // set by Apply; the post-pass has no logf of its own

// Probe counter, read once per second by ReplayDriverProbe: how often the game's own anim UPDATE
// runs for a proxy-owned instance. The pose lane was built on "the anim graph does not drive a
// skater during playback" -- an observed symptom whose cause was never found. If this keeps ticking
// while the local player scrubs, the graph still UPDATES during replay and the driver lane may be
// recoverable there; if it stops, the suppression is upstream of the anim update and the pose lane
// is the right design. Counts every call for a known proxy instance, fresh blob or not.
static volatile LONG g_animUpdCalls = 0;
uint32_t AnimUpdateCalls() { return (uint32_t)g_animUpdCalls; }

void AnimPostApply(void* ai) {
    if (!ai) return;
    for (auto& s : g_animSlots) {
        if (s.ai != ai || !s.owner) continue;
        InterlockedIncrement(&g_animUpdCalls);
        // The replay editor shows recordings only: during playback the replay system owns every
        // proxy's anim state, and the slot going stale in 500 ms is not fast enough to stop the
        // first half-second of fresh blobs stamping over it.
        if (Proxy::Tuning().recordPeers && LocalReplayMode() == 2) return;
        if (GetTickCount64() - s.freshMs > 500) return;      // stale stream: let the local graph run
#ifdef _WIN32
        __try {
            int n = 0;
            for (int i = 0; i < repl::AnimFieldCount(); i++) {
                const repl::AnimField& f = repl::AnimFieldAt(i);
                if (n + f.size > (int)s.len) break;
                // ASSET GATES: a ratio/bool whose driving asset is null HERE is forced to 0, never
                // copied -- a null asset in a blend node evaluates to the reference pose (a T-pose),
                // and a bool that enables a state is as dangerous as its ratio.
                const uint16_t gateAt = repl::AnimAssetGateFor(f.off);
                if (gateAt && *(void**)((uint8_t*)ai + gateAt) == nullptr)
                    memset((uint8_t*)ai + f.off, 0, f.size);
                else
                    memcpy((uint8_t*)ai + f.off, s.blob + n, f.size);
                n += f.size;
            }
            // ---- the FOOT SOCKETS, after the blob: the observer's own foot IK computes against ITS
            // board -- a pop shove-it spins that board 180 and drags the feet into a pretzel. The
            // sender's sockets are the truth; raw world is valid because the proxy stands at the
            // sender's exact position. Same last-writer-before-evaluation timing as the blob.
            // ---- the HAND IK TARGETS, written with the feet and for the same reason: the blob just
            // wrote the hand IK ALPHAS, and an alpha whose target the proxy invents locally makes the
            // arms lag into a pop. Last writer before evaluation, so the graph's own recomputation
            // cannot stomp them.
            if (s.handOk) {
                memcpy((uint8_t*)ai + off::kAnimLHandLoc, s.lHandPos, 12);
                memcpy((uint8_t*)ai + off::kAnimLHandRot, s.lHandRot, 12);
                memcpy((uint8_t*)ai + off::kAnimRHandLoc, s.rHandPos, 12);
                memcpy((uint8_t*)ai + off::kAnimRHandRot, s.rHandRot, 12);
            }
            if (s.feetOk) {
                memcpy((uint8_t*)ai + off::kAnimLFootLoc, s.lFootPos, 12);
                memcpy((uint8_t*)ai + off::kAnimLFootRot, s.lFootRot, 12);
                memcpy((uint8_t*)ai + off::kAnimRFootLoc, s.rFootPos, 12);
                memcpy((uint8_t*)ai + off::kAnimRFootRot, s.rFootRot, 12);
            }
            // ---- the on/off-board TRANSITION POSE SAVE. The graph's mount/dismount transition blends
            // FROM a pose buffer (anim+0x5c0) that only gameplay code fills (ApplyToggleOnBoard/
            // DoBoardPickup -- input paths a proxy never runs), so without this a proxy's transition
            // blends from an EMPTY buffer and T-POSES at every mount. Call the game's own saver on the
            // wire's onBoard edge, HERE -- after the blob write, so the flags it consults
            // (IsAnimatedThrowdown 0x3d6, IsAnimMirrorOn 0x3d5) hold THIS frame's wire values, and
            // before this frame's evaluation, so the buffer holds the pre-switch pose exactly like the
            // sender's own save-then-SetOnBoardMode ordering. Direction and flag mirror the game's call
            // sites: dir = leaving the board, flag = the +0xa38 predicate read off the proxy itself.
            if (s.lastOnBoardSaved != (int8_t)s.onBoard) {
                const bool first = (s.lastOnBoardSaved < 0);
                s.lastOnBoardSaved = (int8_t)s.onBoard;
                const game::Syms& Sy = Get();
                if (!first && Sy.SaveFootTrans && s.owner) {
                    const int ts = safeByte(s.owner->actor(), off::kSkaterToggleState);
                    const uint8_t flag = (ts == 2 || ts == 4) ? 1 : 0;
                    const uint8_t dir  = s.onBoard ? 0 : 1;          // 0 = foot->board (mount)
                    Sy.SaveFootTrans(ai, dir, flag);
                    if (g_animLogf) { static int n = 0; if (++n <= 8) {
                        char m[120];
                        snprintf(m, sizeof(m), "[proxy] onboard edge -> transition pose saved (dir=%d flag=%d)",
                                 (int)dir, (int)flag);
                        g_animLogf(m); } }
                }
            }
            // ---- ANIMGATE diagnostic: which gate assets exist on THIS instance, what the WIRE
            // delivered (the slot copy, not the post-gate anim fields), and what survived the gate.
            if (g_animLogf && debug::Get().animGates) {
                static uint64_t lastGate = 0;
                const uint64_t now = GetTickCount64();
                if (now - lastGate >= 2000) {
                    lastGate = now;
                    char m[260];
                    snprintf(m, sizeof(m),
                             "[animgate] crankBS=%s flipTrick=%s revertBS=%s | wire: crankOn=%d idx=%d pocket=%.2f"
                             " | applied: isCranking=%d pocket=%.2f popRatio=%.2f",
                             *(void**)((uint8_t*)ai + off::kAnimCrankBS)  ? "set" : "NULL",
                             *(void**)((uint8_t*)ai + off::kAnimFlipTrick)? "set" : "NULL",
                             *(void**)((uint8_t*)ai + off::kAnimRevertBS) ? "set" : "NULL",
                             (int)s.wireCrankOn, (int)(int16_t)s.wireCrankIdx, s.wireCrankPocket,
                             (int)*((uint8_t*)ai + 0x497), *(float*)((uint8_t*)ai + 0x498),
                             *(float*)((uint8_t*)ai + 0x4d0));
                    g_animLogf(m);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        return;
    }
}

// ---- the replay-driver probe --------------------------------------------------------------------
// One line per second, only while the local player is in replay playback. Reports whether the anim
// UPDATE still fires for proxy instances, and the state of the three standard suppression knobs on
// a live proxy mesh (the bPauseAnims/bNoSkeletonUpdate bitfield byte, and GlobalAnimRateScale).
// Together these say WHY proxies go un-driven during a replay -- the fact the pose lane was built on
// but never root-caused. Read-only; writes nothing.
void ReplayDriverProbe(uint64_t nowMs, void (*logf)(const char*)) {
    if (!logf) return;
    // Also runs for a few seconds AFTER playback ends: whether the anim update comes back for
    // proxies at exit is exactly what a stuck-after-replay report needs the log to answer.
    static uint64_t lastScrubMs = 0;
    if (LocalReplayMode() == 2) lastScrubMs = nowMs;
    else if (!lastScrubMs || nowMs - lastScrubMs > 5000) return;
    static uint64_t lastMs = 0;
    static uint32_t lastUpd = 0;
    if (nowMs - lastMs < 1000) return;
    const uint32_t upd = AnimUpdateCalls();
    const uint32_t updPerSec = (lastMs && upd >= lastUpd) ? (upd - lastUpd) : 0;
    lastMs = nowMs; lastUpd = upd;

    int flags = -1; float rate = -1.f;
    // The pose slots track proxy meshes directly and are refreshed every frame, so that pointer is
    // the reliable one; the anim-slot owner's mesh read proved flaky in the field.
    void* mesh = pose::FirstProxyMesh();
    if (mesh) {
#ifdef _WIN32
        __try {
            flags = *(uint8_t*)((uint8_t*)mesh + off::kMeshAnimFlagsByte);
            rate  = *(float*)((uint8_t*)mesh + off::kMeshGlobalAnimRate);
        } __except (EXCEPTION_EXECUTE_HANDLER) { flags = -1; rate = -1.f; }
#endif
    }
    char m[180];
    snprintf(m, sizeof(m),
             "[rprobe] %s: proxy animUpd/s=%u meshFlags=0x%02x animRate=%.2f "
             "(upd>0 = the graph still updates; flags/rate name the suppressor if it stops)",
             LocalReplayMode() == 2 ? "scrubbing" : "post-exit", updPerSec, (unsigned)(flags & 0xff), (double)rate);
    logf(m);
}


// =====================================================================================================
// APPLY -- one frame
// =====================================================================================================
void Proxy::Apply(const repl::State& s, uint64_t nowMs, uint64_t nowUs, void (*logf)(const char*)) {
    if (!actor_) return;
    g_animLogf = logf;
    const Syms& S = Get();

    // ---- 1. visuals: the game's own rebuild, once, after the actor has had a beat to construct.
    if (!refreshed_ && S.RefreshVisuals && nowMs - bornMs_ > kVisualSettleMs) {
        refreshed_ = true;
#ifdef _WIN32
        // If this faults, the proxy is stuck with its constructed look and the cosmetics path looks
        // broken for a reason that has nothing to do with cosmetics -- so it announces itself ONCE,
        // with the offset that names the function.
        unsigned long xcode = 0; void* xaddr = nullptr;
        __try { S.RefreshVisuals(actor_); }
        __except (xcode = GetExceptionCode(),
                  xaddr = (GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
                  EXCEPTION_EXECUTE_HANDLER) {
            static bool said = false;
            if (!said && logf) {
                said = true;
                const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
                char m[200];
                snprintf(m, sizeof(m), "[proxy] the game's own RefreshVisuals FAULTED code=0x%08lX"
                                       " (exe+0x%llX)", xcode,
                         (unsigned long long)((const uint8_t*)xaddr - base));
                logf(m);
            }
        }
#endif
    }
    // ---- 2. its board must not replicate either. The board arrives later, via async streaming, so
    //         this latches the first frame it exists AND provably belongs to this proxy.
    void* bd = OwnBoard();
    st_.boardOwned = (bd != nullptr);
    if (bd && !boardRepOff_ && S.SetReplicates) {
        boardRepOff_ = true;
#ifdef _WIN32
        __try { S.SetReplicates(bd, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // The board actor's OWN tick runs its local board simulation (steering, ground alignment, roll
        // physics assists) -- on a wire-driven board that is a SECOND WRITER fighting the transported
        // pose every frame, and it renders as the board tumbling on the ground. Same treatment as the
        // skater gets at spawn: a proxy's parts have no use for their own decision-making. Component-
        // level physics stays ON -- the deck must remain a real collidable body, since board-vs-board
        // contact is the point of the velocity drive.
        if (S.SetActorTick) { __try { S.SetActorTick(bd, false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
#endif
    }
    // ---- 3. bail veto. Bail's second instruction is `cmp byte [this+0xf0],1 ; jbe ret`, so a Role-1
    //         replicated copy physically cannot bail -- but a proxy is Role 3, so that gate PASSES and
    //         all seven local bail callers fire on a skater whose board is moved every frame. Hold
    //         `_canBail` at 0; the owner transports the real bail.
    if (g_tun.vetoBail) {
#ifdef _WIN32
        __try {
            uint8_t* cb = (uint8_t*)actor_ + off::kSkaterCanBail;
            if (*cb != 0) { *cb = 0; st_.bailVetoes++; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    // ---- 3.5 BAIL SYNC. The owner's ragdoll bit (+0x710 bit 0x02) is on the wire; on its RISING edge
    //          the proxy EXECUTES the game's own Bail -- its local ragdoll sim plays the flop (each
    //          client is authoritative for itself; the exact pose is not worth transporting), while
    //          step 5 keeps stamping the capsule at the sender's transported position so the getup
    //          happens at the TRUE spot. The FALLING edge of the same bit is the recovery: ResetRagDoll
    //          (it gates on the ragdoll bit itself, so it is a no-op if the game already recovered on
    //          its own ResetRagDollDelay timer). The veto in step 3 keeps suppressing the proxy's OWN
    //          bail deciders; _canBail is lifted for exactly the one triggered call. The trigger needs
    //          the board link up -- the small Bail overload's location fallback derefs skater+0x568 --
    //          and a missed edge just means no flop, never a retry storm: the latch advances regardless.
    if (g_tun.bailSync && (s.bailing ? 1 : 0) != lastBailing_) {
        lastBailing_ = s.bailing ? 1 : 0;
        if (s.bailing) {
            if (S.Bail && bd) {
                // FString layout {data,num,max}, num counts the terminator. Bail treats the reason as
                // a const ref (in-game callers hand it a stack Printf string), so a static buffer is fine.
                static wchar_t reasonChars[] = L"OMP transported bail";
                struct { wchar_t* d; int n; int max; } reason =
                    { reasonChars, (int)(sizeof(reasonChars) / 2), (int)(sizeof(reasonChars) / 2) };
#ifdef _WIN32
                __try {
                    *((uint8_t*)actor_ + off::kSkaterCanBail) = 1;
                    S.Bail(actor_, &reason, true, true);          // (reason, 1, 1) = the in-game pattern;
                    *((uint8_t*)actor_ + off::kSkaterCanBail) = 0; // the last 1 is bRagdoll
                    st_.bails++;
                    if (logf) logf("[proxy] BAIL (owner ragdoll edge) -> local ragdoll sim");
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    if (logf) logf("[proxy] BAIL call FAULTED (suppressed; veto re-held)");
                }
#endif
            } else if (logf) logf("[proxy] owner bailed but no Bail sym/board link -- skipped");
        } else {
            if (S.ResetRagDoll) {
#ifdef _WIN32
                __try { S.ResetRagDoll(actor_); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
            }
            if (logf) logf("[proxy] bail END -> ResetRagDoll");
        }
    }
    // There is deliberately no step that drives the proxy's on/off-board MODE: nothing in the overlay
    // reads the proxy's bOnBoard -- the off-board LOOK is the anim blob, and the board is step 7.
    st_.onBoard = (s.onBoard != 0);
    // ---- 4.5 the PUSH. The EDGE is NOT taken from `s` -- see PlayPushStates. `s.pushState` is the
    // SAMPLED value, and sampling cannot see a transition that lived entirely between two snapshots,
    // which is every fast tap at a 30-60 Hz publish rate. The stream files edges as they arrive and
    // releases them on the playback clock, exactly like the audio one-shots.
    // ---- 4.5b PUSH SPEED. A LEVEL, not an edge, so it rides the sampled state and is written every
    // frame: the proxy's own actor tick is disabled, so nothing else maintains this and SetPushState
    // resets it to 1.0 on every state change. Without it a proxy animates every push at 1.0x
    // regardless of how fast the sender is tapping.
#ifdef _WIN32
    {
        const float mul = (s.pushSpeed > 0.01f && s.pushSpeed < 8.f) ? s.pushSpeed : 1.0f;
        __try { *(float*)((uint8_t*)actor_ + off::kSkaterPushSpeedMul) = mul; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#endif
    // ---- 4.6 the BRAKE. Multicast-RPC-replicated in the stock game, so an overlay must transport it.
    //          On change: write the state bytes directly (the RPC impls open with a Role==1 gate
    //          written for replicated proxies; ours is Role 3, and the anim graph reads the BYTES) AND
    //          call the impl -- if the gate lets the body run, the movement component gets the full
    //          treatment too.
    if (s.brakeState != lastBrakeState_) {
#ifdef _WIN32
        __try {
            *((uint8_t*)actor_ + off::kSkaterBrake1) = (s.brakeState & 1) ? 1 : 0;
            *((uint8_t*)actor_ + off::kSkaterBrake2) = (s.brakeState & 2) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (s.brakeState) {
            const uint8_t type = (s.brakeState & 1) ? 1 : 2;
            if (S.StartBraking) { __try { S.StartBraking(actor_, type); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        } else if (S.StopBraking) {
            __try { S.StopBraking(actor_, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
#endif
        lastBrakeState_ = s.brakeState;
    }
    // ---- 5. the BODY: position + rotation, both transported, written every frame.
    //         SetActorLocRot here is the FQuat overload -- FOUR floats. A 3-float rotator normalises to
    //         (0,1,0,0) = upside down on every machine.
    //         ETeleportType MUST be 0 (None): 1 means "do not derive velocity", which leaves
    //         CharacterMovement->Velocity at zero so the locomotion blend plays IDLE while the body slides.
    if (S.SetActorLocRot && s.bodyPosOk) {
        // The TRANSPORTED capsule quat, applied RAW. NO CONVERSION MAY EVER SIT IN THIS PATH: a
        // euler->quat rebuild is not the inverse of the sender's extraction at yaw ~ +/-180 (a yaw-180
        // quat extracts as yaw 180 + roll 180, which rebuilds to (0,1,0,0)), and the skater flips
        // upside down at random.
        static const float kIdent[4] = {0,0,0,1};
        const float* q = s.bodyRotOk ? s.bodyQuat : kIdent;
#ifdef _WIN32
        __try { S.SetActorLocRot(actor_, s.bodyPos, q, false, nullptr, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    // ---- 6. the mesh's rotation RELATIVE to the capsule: a WORLD write drags everything attached and
    //         overrides the facing, whereas the relative rotation composes with it.
    if (S.SetWorldRotQuat && s.meshOk) {
        void* mesh = safePtr(actor_, off::kSkaterMesh);
        void* root = safePtr(actor_, off::kActorRootComp);
        float rq[4];
        if (mesh && root && safeRead((uint8_t*)root + off::kCompQuat, rq, 16)) {
            float world[4]; qMul(rq, s.meshQuat, world);
#ifdef _WIN32
            __try { S.SetWorldRotQuat(mesh, world, false, nullptr, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        }
    }
    // ---- 4.8 the CRANK = the setup CROUCH, applied every frame: plain skater state the anim's
    //          SetCrank (inside NativeUpdateAnimation, which runs on proxies) derives the crouch from,
    //          copying the def's blend spaces into the anim, which is what opens the gate. _isCranking
    //          is BIT 0 of +0x580 (masked, never assigned -- SetCranking's own and-0xfe/or). The def is
    //          rebuilt from its transported _crankList INDEX through the LOCAL controller's tricks DB
    //          -- it lives in that heap TArray, not at an offset inside the trick def -- and written
    //          UNCONDITIONALLY: the game leaves it set between cranks, and an early def means the blend
    //          space is populated before the crank ever starts.
    {
        void* ckDef = (s.crankDefOff != 0xffff) ? CrankPtrFromIndex((int)s.crankDefOff) : nullptr;
#ifdef _WIN32
        __try {
            uint8_t* ck = (uint8_t*)actor_ + off::kSkaterIsCranking;
            *ck = (uint8_t)((*ck & 0xfe) | (s.crankOn ? 1 : 0));
            *(void**)((uint8_t*)actor_ + off::kSkaterCrankDef) = ckDef;
            *(float*)((uint8_t*)actor_ + off::kSkaterCrankPocket) = s.crankPocket;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        // Edge probes, not samples -- a crank window is shorter than the 2 s animgate line's period:
        // one line when the def identity changes, one on each crankOn rising edge.
        if (logf && debug::Get().crankEdges) {
            if (s.crankDefOff != lastCrankIdx_) {
                lastCrankIdx_ = s.crankDefOff;
                char m[120];
                snprintf(m, sizeof(m), "[proxy] crank def idx=%d -> %s",
                         (int)s.crankDefOff, ckDef ? "RESOLVED" : (s.crankDefOff == 0xffff ? "(none)" : "NOT IN OUR LIST"));
                logf(m);
            }
            if (s.crankOn && !lastCrankOn_) {
                char m[120];
                snprintf(m, sizeof(m), "[proxy] crank ON (idx=%d def=%s pocket=%.2f)",
                         (int)s.crankDefOff, ckDef ? "ok" : "NULL", s.crankPocket);
                logf(m);
            }
            lastCrankOn_ = s.crankOn ? 1 : 0;
        }
    }

    // ---- 4.7 the TRICK DEF. The proxy's own anim update derives its trick assets from
    //          _currentFlipTrickDef every frame (SetFlipTrick); with it null, the T-pose gates zero the
    //          crouch ("crank") and every trick ratio. Resolve the transported NAME once per change via
    //          StaticFindObject(ANY_PACKAGE) and write the pointer; unresolved stays null and the gates
    //          keep protecting.
    if (strncmp(s.trickName, lastTrickName_, sizeof(lastTrickName_)) != 0) {
        memcpy(lastTrickName_, s.trickName, sizeof(lastTrickName_));
        void* def = nullptr;
        if (s.trickName[0] && S.StaticFindObject) {
            wchar_t wide[48];
            for (int i = 0; i < 48; i++) { wide[i] = (wchar_t)(uint8_t)s.trickName[i]; if (!s.trickName[i]) break; }
            wide[47] = 0;
#ifdef _WIN32
            __try { def = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, wide, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { def = nullptr; }
#endif
        }
        // TYPE GATE. The name came off the wire and ANY_PACKAGE matches ANY class, so this pointer is
        // a peer's choice until it is checked. The check is MEMBERSHIP in the local
        // UTricksDatabase::_flipTricks -- validate against a local, typed, always-present list instead
        // of trusting a name, the same argument as the crank def's index. Rejection sets def=null,
        // exactly the "this install does not have it" path that already exists here, so the T-pose
        // gates keep protecting and nothing new has to handle a failure.
        bool rejected = false;
        if (def && Tuning().typeGateTrick && GateTrusted("FlipTrickDefinition") &&
            FlipTrickListAvailable() && !IsKnownFlipTrickDef(def)) {
            def = nullptr; rejected = true; g_typeRejects++;
        }
        // The gate is SKIPPED when the list cannot be read at all (no controller/DB yet): unknown is
        // not hostile, and refusing every trick because the database is not visible yet would break a
        // legitimate session to no benefit.
#ifdef _WIN32
        __try { *(void**)((uint8_t*)actor_ + off::kSkaterTrickDef) = def; } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        trickDef_ = def;
        if (logf) { static int n = 0; if (++n <= 8) {
            char m[160]; snprintf(m, sizeof(m), "[proxy] trick def '%s' -> %s",
                                  s.trickName[0] ? s.trickName : "(none)",
                                  rejected ? "REJECTED (not a trick definition on this install)"
                                           : def ? "RESOLVED" : (s.trickName[0] ? "NOT FOUND" : "cleared"));
            logf(m); } }
    }

    // ---- 4.9 the GRIND DEF, the trick-def pattern verbatim. The grind upper-body pose is a blend
    //          space USkaterAnimInstance::GetGrindBlendSpace fetches THROUGH the skater's
    //          _currentGrindDef/_targetGrindDef, and the whole grind anim-set population runs inside
    //          NativeUpdateAnimation, which is live on proxies. So: resolve the transported name once
    //          per change, write both def pointers (mirroring SetCurrentGrindDefinition's 3 field
    //          writes incl. the def+0x38 -> skater+0x708 cache -- the function is trivially small), and
    //          the proxy's own anim update populates the sets. The wire's IsGrinding then renders a
    //          real grind pose instead of gating into nothing. Pitch/yaw ratios are plain skater floats
    //          the update copies into the blend inputs -- written every frame like the crank pocket.
    if (strncmp(s.grindName, lastGrindName_, sizeof(lastGrindName_)) != 0) {
        memcpy(lastGrindName_, s.grindName, sizeof(lastGrindName_));
        void* gdef = nullptr;
        if (s.grindName[0] && S.StaticFindObject) {
            wchar_t wide[48];
            for (int i = 0; i < 48; i++) { wide[i] = (wchar_t)(uint8_t)s.grindName[i]; if (!s.grindName[i]) break; }
            wide[47] = 0;
#ifdef _WIN32
            __try { gdef = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, wide, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { gdef = nullptr; }
#endif
        }
        // TYPE GATE -- the sharpest of the four sites, because this is the one that dereferences the
        // peer's pointer directly (gdef+0x38 below) as well as handing it to the game, after which the
        // anim update reads def+0x1a0/+0x1c8 in engine code no SEH here wraps.
        // No local list exists for grind definitions the way _flipTricks exists for tricks, so this is
        // a class check. Subclasses pass, by construction of the chain walk.
        bool grindRejected = false;
        if (gdef && Tuning().typeGateGrind && GateTrusted("GrindOrSlideDefinition") &&
            !IsObjectOfClass(gdef, "GrindOrSlideDefinition")) {
            gdef = nullptr; grindRejected = true; g_typeRejects++;
        }
#ifdef _WIN32
        __try {
            *(void**)((uint8_t*)actor_ + off::kSkaterGrindDefTgt) = gdef;
            if (*(void**)((uint8_t*)actor_ + off::kSkaterGrindDefCur) != gdef) {
                *(void**)((uint8_t*)actor_ + off::kSkaterGrindDefCur) = gdef;
                if (gdef) *(void**)((uint8_t*)actor_ + off::kSkaterGrindCache) =
                    *(void**)((uint8_t*)gdef + off::kGrindDefCacheSrc);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        if (logf) { static int n = 0; if (++n <= 8) {
            char m[160]; snprintf(m, sizeof(m), "[proxy] grind def '%s' -> %s",
                                  s.grindName[0] ? s.grindName : "(none)",
                                  grindRejected ? "REJECTED (not a grind definition)"
                                                : gdef ? "RESOLVED" : (s.grindName[0] ? "NOT FOUND" : "cleared"));
            logf(m); } }
    }
#ifdef _WIN32
    __try {
        *(float*)((uint8_t*)actor_ + off::kSkaterGrindPitch) = s.grindPitch;
        *(float*)((uint8_t*)actor_ + off::kSkaterGrindYaw)   = s.grindYaw;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif

    // ---- 6.5 the POSE BLOB (field table in anim_fields.h): STORE this frame's blob for the anim
    //          post-pass. Writing the instance HERE (pre-world-tick) does not survive to render: the
    //          game's own NativeUpdateAnimation recomputes every field from the proxy's empty local
    //          state afterwards and before evaluation. The loader's POST-hook on NativeUpdateAnimation
    //          calls AnimPostApply, which is the last writer before the pose is evaluated.
    // The pose lane lives OUTSIDE the `if (s.animLen)` block and must stay there. `pose::Capture`
    // deliberately zeroes animLen (the driver blob is measured dead during replay, and the skeleton
    // needs the room), so gating the pose handoff on the blob means the one packet that carries a pose
    // is the one packet that never delivers it. The two lanes are ALTERNATIVES; never gate one on the
    // other's payload.
    {
        void* pmesh = safePtr(actor_, off::kSkaterMesh);
        if (pmesh) pose::Note(pmesh, s, nowMs);
    }
    if (s.animLen) {
        void* pmesh = safePtr(actor_, off::kSkaterMesh);
        void* ai = pmesh ? safePtr(pmesh, off::kMeshAnimInstance) : nullptr;
        if (ai) StoreAnimForPostPass(this, ai, s, nowMs);
    }
    // ---- 6.6 board-link visibility: distinguishes "never LINKED" from "linked but mis-driven", the
    //          two halves of a board that is not showing up on the observer.
    if (!boardLogged_) {
        if (bd) { boardLogged_ = true; if (logf) logf("[proxy] board LINKED (back-link verified)"); }
        else if (nowMs - bornMs_ > 10000) {
            boardLogged_ = true;
            if (logf) logf("[proxy] board NOT LINKED after 10s -- skater+0x568/board+0x4d8 never agreed");
        }
    }
    // ---- 6.7 the peer's transported sounds. BEFORE the board early-out on purpose: a sound attached
    // to the skater's MESH (footsteps, clothing, the impact grunt) has nothing to do with whether the
    // board link resolved, and audio must not depend on any state a proxy may fail to have. A null
    // board just means board-attached sounds fall back to a world position.
    AudioApply(s, bd);
    // ---- 7. the BOARD.
    if (!bd) return;
    if (!s.onBoard) {
        // ---- 7a. OFF BOARD = the CARRY. The carry needs no local reproduction: on the SENDER the
        // carried board is positioned every frame by USkateboardExMovementComponent::PlaceInHand
        // (hand-socket math on the skater mesh, board movement mode 9), gather samples the deck pose
        // regardless of onBoard, and the deck travels continuously -- so the carried pose is already on
        // the wire. The proxy just keeps stamping it: sim-off + exact quat, the same path airborne
        // tricks use. A carried board must never simulate or be velocity-driven; the stamp's
        // StopBoardSim re-asserts that even if the proxy's own Bail briefly ragdolled its board. Bail
        // tumbles come free, being just more transported pose.
        // The alternative below (park+hide) leaves an INVISIBLE query-collidable obstacle standing at
        // the dismount point, and is kept only as an A/B fallback.
        if (g_tun.carryBoard) {
            if (boardHidden_) {
                boardHidden_ = false;
                if (S.SetActorHidden) {
#ifdef _WIN32
                    __try { S.SetActorHidden(bd, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
                }
            }
            StampBoard(s);
            st_.carryStamps++;
        } else if (!boardHidden_) {              // park+hide, the A/B fallback
            boardHidden_ = true;
            StopBoardSim();
            if (S.SetActorHidden) {
#ifdef _WIN32
                __try { S.SetActorHidden(bd, true); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
            }
        }
    } else {
        if (boardHidden_) {
            boardHidden_ = false;
            if (S.SetActorHidden) {
#ifdef _WIN32
                __try { S.SetActorHidden(bd, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
            }
        }
        // Airborne frames use the STAMP. A flip outruns the clamped angular chase and the shortest-arc
        // correction reverses once the lag passes 180 deg (the flip unwinds backwards); and the
        // whole-board velocity setter writes ONE velocity to every part, which is right in translation
        // and wrong in spin (parts need v + w x r) -- mid-flip that fights the truck/wheel constraints
        // and the solver noise bleeds into the rider's animation. Board-vs-board contact only matters
        // on the ground. The grounded flag is an EXPLICIT transported field (repl::State::grounded) --
        // the SENDER's own IsGrounded, sampled from its anim instance. It must NOT be read out of
        // `anim[]`: that blob is a packed field SEQUENCE, so indexing it by the struct offset reads an
        // unrelated byte.
        // bOnBoard flips at the START of the mount, while the sender's board is still hand-carried for
        // a beat (board movement mode 9); driving a live dynamic body after the swinging hand in that
        // window jitters. Transported truth decides: mode 9 = keep stamping (the carry continuing into
        // the mount), anything else = the normal split.
        const bool handHeld = (s.boardMode == off::kBoardModeInHand);
        const bool airborne = g_tun.driveGroundedOnly && (s.grounded == 0);
        // A far peer's board is stamped, never simulated: collision response only matters at contact
        // range, and the session's distance gate (with hysteresis) turns the sim back on before the
        // local player can reach it. Fewer live rigid bodies, and the whole velocity-drive PhysX
        // call chain skipped for everyone out of reach.
        if (!nearLocal_) { if (simOn_) StopBoardSim(); StampBoard(s); }
        else if (handHeld) { st_.carryStamps++; StampBoard(s); }
        else if (!g_tun.velocityDrive || airborne) { if (airborne) st_.airSkips++; StampBoard(s); }
        else if (!VelocityDrive(s, nowUs)) StampBoard(s);
    }

    // ---- offscreen anim throttle, once per actor -- but NEVER while peers are recorded into the
    // replay (the retired recordPeers mode): the game's recorder captures the EVALUATED pose, so
    // any stretch the local camera was not looking at a peer recorded them frozen standing
    // (field-confirmed, at both tick-option values). With peers OUT of the replay system this is
    // free again, at the strongest setting: OnlyTickPoseWhenRendered (3) skips the whole anim
    // update+evaluation for an unrendered proxy -- the largest per-proxy CPU cost -- and nothing
    // observes the frozen graph: live viewers get the current pose the frame they look back, and
    // replay sync renders from TRANSFERRED wire states, never from this client's anim instance.
    // One byte, engine's own mechanism.
    if (g_tun.offscreenAnimThrottle && !g_tun.recordPeers && !animThrottled_) {
        void* mesh = safePtr(actor_, off::kSkaterMesh);
        if (mesh) {
#ifdef _WIN32
            __try {
                *(uint8_t*)((uint8_t*)mesh + off::kMeshAnimTickOption) = 3;
                animThrottled_ = true;
                static bool said = false;
                if (!said && logf) { said = true;
                    logf("[proxy] offscreen anim throttle ON (OnlyTickPoseWhenRendered)"); }
            } __except (EXCEPTION_EXECUTE_HANDLER) { animThrottled_ = true; }
#endif
        }
    }

    // ---- board telemetry, 1 Hz: which path placed the board this second, the drive's error against
    // the transported target, and the sim state. A board that is LINKED but not appearing correctly
    // can fail at several stages; this line says which one.
    if (logf) {
        static uint64_t lastBd = 0;
        if (nowMs - lastBd >= 1000) {
            lastBd = nowMs;
            char m[220];
            snprintf(m, sizeof(m),
                     "[proxy] board err=%.0fcm driven=%u snaps=%u stamps(air)=%u carry=%u stops=%u sim=%d"
                     " onBoard=%d grounded=%d bail=%d mode=%d rej=%u",
                     st_.driveErrCm, st_.driven, st_.snaps, st_.airSkips, st_.carryStamps, st_.stops,
                     (int)simOn_, (int)(s.onBoard != 0), (int)(s.grounded != 0), (int)(s.bailing != 0),
                     (int)s.boardMode, TypeRejects());
            logf(m);
        }
    }
}

// ---- the velocity drive: keep the body SIMULATING and steer it, so PhysX can actually resolve
// board-vs-board contact (impulses both ways + the game's own contact sounds), while the drive springs
// it back to the transported truth. Stamping instead does the opposite -- TeleportTo internally does
// StopMovementImmediately + SetSimulatePhysics, lobotomising the body 60x/s.
bool Proxy::VelocityDrive(const repl::State& s, uint64_t nowUs) {
    const Syms& S = Get();
    void* bd = OwnBoard();
    if (!bd || !S.BoardSetLinVel) return false;
    void* deck = safePtr(bd, off::kBoardFlipper);            // the deck you can SEE, never the actor root
    if (!deck) deck = safePtr(bd, off::kBoardTruckBack);
    if (!deck) deck = safePtr(bd, off::kActorRootComp);
    if (!deck) return false;
    float cur[3];
    if (!safeRead((uint8_t*)deck + off::kCompPos, cur, 12)) return false;

    float dt = 1.f / 60.f;
    if (lastDriveUs_) {
        const float d = (float)((double)(int64_t)(nowUs - lastDriveUs_) * 1e-6);
        if (d > 0.002f && d < 0.1f) dt = d;
    }
    lastDriveUs_ = nowUs;

    const float err[3] = { s.deckPos[0]-cur[0], s.deckPos[1]-cur[1], s.deckPos[2]-cur[2] };
    const float mag2 = err[0]*err[0] + err[1]*err[1] + err[2]*err[2];
    st_.driveErrCm = sqrtf(mag2);
    if (mag2 > g_tun.driveSnapCm * g_tun.driveSnapCm) { st_.snaps++; return false; }   // join/teleport

    // ---- ORIENTATION SNAP. A deck settled on its back is a physics ENERGY TRAP: the shortest-arc
    // correction has to rotate it THROUGH the ground, contacts block it, and the clamped angular chase
    // loses to gravity forever, so an upside-down board stays upside down on the observer. Past about a
    // quarter turn of error the chase cannot win; STAMP the pose instead (the stamp teleports rotation
    // directly, escaping the trap, and the drive resumes next frame).
    {
        float qc0[4];
        if (safeRead((uint8_t*)deck + off::kCompQuat, qc0, 16)) {
            float d = qc0[0]*s.deckQuat[0] + qc0[1]*s.deckQuat[1] + qc0[2]*s.deckQuat[2] + qc0[3]*s.deckQuat[3];
            if (d < 0) d = -d;                       // q and -q are one rotation
            if (d < 1.f) {
                const float angRad = 2.f * acosf(d < 1.f ? d : 1.f);
                if (angRad > g_tun.driveSnapAngRad) { st_.snaps++; return false; }
            }
        }
    }

    // Re-assert simulation every frame: the setter early-outs when the state already matches, and the
    // game re-disables it on its own events, so a one-shot would silently lapse.
    if (S.SetSimulatePhysics) {
#ifdef _WIN32
        __try { S.SetSimulatePhysics(bd, true, false); simOn_ = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    }
    float vel[3]; const float inv = 1.f / dt;
    for (int i = 0; i < 3; i++) {
        float v = err[i] * inv;
        if (v >  g_tun.driveMaxVel) v =  g_tun.driveMaxVel;
        if (v < -g_tun.driveMaxVel) v = -g_tun.driveMaxVel;
        vel[i] = v;
    }
#ifdef _WIN32
    __try { S.BoardSetLinVel(bd, vel, false, 0ull); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    // angular: the deck primitive toward the TRANSPORTED DECK quat, never the actor rotator -- they
    // differ by the deck-relative rotation, so using the actor's double-applies it.
    __try {
        float qc[4];
        if (safeRead((uint8_t*)deck + off::kCompQuat, qc, 16)) {
            float cj[4], qe[4];
            qConj(qc, cj); qMul(s.deckQuat, cj, qe);
            if (qe[3] < 0) for (int i = 0; i < 4; i++) qe[i] = -qe[i];      // short way round
            const float sn = sqrtf(qe[0]*qe[0] + qe[1]*qe[1] + qe[2]*qe[2]);
            if (sn > 1e-5f) {
                float rate = 2.f * atan2f(sn, qe[3]) * inv;
                if (rate > g_tun.driveMaxAngRad) rate = g_tun.driveMaxAngRad;
                const float k = rate / sn;
                const float w[3] = { qe[0]*k, qe[1]*k, qe[2]*k };
                void** vt = *(void***)deck;
                using AngFn = void (*)(void*, const float*, bool, uint64_t);
                ((AngFn)vt[off::kVtblSetAngularVel / 8])(deck, w, false, 0ull);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
    st_.driven++;
    return true;
}

// =====================================================================================================
// THE PEER'S OWN SOUNDS
// The wire carries what their game actually played, captured at its sound-spawn funnel. Nothing here
// decides WHETHER a sound should happen; that decision was made on the machine that was simulating.
// See audio.h for why this is the only layer at which that is true.
// =====================================================================================================
void Proxy::AudioApply(const repl::State& s, void* bd) {
    if (!actor_) return;
    for (int i = 0; i < 3; i++) lastBodyPos_[i] = s.bodyPos[i];
    // Tell the capture layer this actor is a proxy, so its OWN anim-notify sounds are muted: their
    // audio arrives on the wire, and letting the local graph fire them too would double every catch.
    // Refreshed per frame and EXPIRING, so a released proxy leaves no pointer behind.
    audio::NoteProxy(actor_, bd);

    // Loops are a SET, not a stream of start/stop events -- there is no stop funnel to capture on the
    // sender, so "still being published" is the only liveness signal that exists. It is also loss-
    // proof: a dropped packet is corrected by the next one, and a sound that ends simply stops
    // appearing.
    bool keep[repl::kAudioMaxLoops] = {};
    for (int w = 0; w < s.nLoops && w < repl::kAudioMaxLoops; w++) {
        const repl::AudioLoop& L = s.loops[w];
        if (!L.cue[0]) continue;
        int found = -1;
        for (int i = 0; i < repl::kAudioMaxLoops; i++)
            if (audioLoops_[i].comp && audioLoops_[i].slot == L.slot) { found = i; break; }
        if (found < 0) {
            for (int i = 0; i < repl::kAudioMaxLoops; i++) if (!audioLoops_[i].comp) { found = i; break; }
            if (found < 0) continue;                       // no free slot: the newest loop waits a frame
            void* c = audio::PlayLoop(L, actor_, bd, lastBodyPos_);
            if (!c) continue;                              // cue missing on this install -- counted there
            audioLoops_[found].slot = L.slot;
            audioLoops_[found].comp = c;
        } else {
            audio::UpdateLoop(audioLoops_[found].comp, L);  // volume/pitch/surface, every frame
        }
        keep[found] = true;
    }
    for (int i = 0; i < repl::kAudioMaxLoops; i++) {
        if (audioLoops_[i].comp && !keep[i]) {              // gone from the wire = the sender stopped it
            audio::StopSound(audioLoops_[i].comp);
            audioLoops_[i].comp = nullptr; audioLoops_[i].slot = 0;
        }
    }
}

void Proxy::PlayAudioEvents(const repl::AudioEvent* e, int n) {
    if (!actor_ || !e) return;
    void* bd = OwnBoard();
    for (int i = 0; i < n; i++) audio::PlayOneShot(e[i], actor_, bd, lastBodyPos_);
}

void Proxy::AudioStopAll() {
    for (auto& l : audioLoops_) {
        if (l.comp) audio::StopSound(l.comp);
        l.comp = nullptr; l.slot = 0;
    }
}

void Proxy::StampBoard(const repl::State& s) {
    const Syms& S = Get();
    void* bd = OwnBoard();
    if (!bd || !S.BoardTeleport) return;
    // A board being PLACED must not simulate, or gravity/restitution undoes every write between frames.
    // That pairing is also exactly why the stamp cannot be the normal path: a non-simulating deck has
    // no physics body for another board to hit.
    StopBoardSim();
    // The teleport's ROTATOR is a lossy, yaw-dependent conversion (the same non-inverse euler class as
    // the body quat) and must never be the authority: on its own it flips the board on placement, and
    // the orientation snap above would re-stamp through it and never converge. It is passed only
    // because the teleport requires the argument; the EXACT orientation is then written as the QUAT,
    // conversion-free, to the visible deck component.
    float rot[3]; quatToRot3(s.deckQuat, rot);
#ifdef _WIN32
    __try { S.BoardTeleport(bd, s.deckPos, rot); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (S.SetWorldRotQuat) {
        void* deck = safePtr(bd, off::kBoardFlipper);
        if (!deck) deck = safePtr(bd, off::kBoardTruckBack);
        if (!deck) deck = safePtr(bd, off::kActorRootComp);
        if (deck) { __try { S.SetWorldRotQuat(deck, s.deckQuat, false, nullptr, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }
#endif
}

void Proxy::StopBoardSim() {
    const Syms& S = Get();
    void* bd = safePtr(actor_, off::kSkaterBoard);
    if (!bd || safePtr(bd, off::kBoardSkater) != actor_) return;     // ownership, always
    if (!S.SetSimulatePhysics) return;
#ifdef _WIN32
    __try { S.SetSimulatePhysics(bd, false, false); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
#endif
    if (simOn_) { simOn_ = false; st_.stops++; }
    lastDriveUs_ = 0;                                               // fresh dt when the drive resumes
}

// Each transition the sender actually went through, in order, released at the moment the playback
// clock reaches it. Called right after Apply, beside PlayAudioEvents and for the same reason: a tapped
// push is a one-shot event, and one-shot events must never ride in a SAMPLED State.
// SetPushState is internally edge-triggered (it no-ops on a repeat of the state it already holds), so
// handing it the true sequence is both necessary and sufficient -- no dedup of our own.
void Proxy::PlayPushStates(const uint8_t* states, int n) {
    if (!actor_ || !states || n <= 0) return;
    const Syms& S = Get();
    if (!S.SetPushState) return;
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        __try { S.SetPushState(actor_, states[i]); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        lastPushState_ = states[i];
        st_.pushes++;
    }
}

void Proxy::Retire(void (*logf)(const char*)) {
    if (!actor_) return;
    spectate::OnActorGone(actor_, logf); // if the replay camera was watching them, hand it back first
    OnQuiet(logf);                       // stops the board simulating and silences their loops
    AudioStopAll();
    const Syms& S = Get();
    // HIDING IS PURELY VISUAL -- the components keep their collision. A retired proxy that is only
    // hidden leaves an invisible obstacle exactly where it was standing: you cannot see it, but the
    // camera still collides with the skater's capsule, which reads as the game snagging on nothing.
    // The actor is never destroyed (destroying one mid-session crashes the client), so its collision
    // has to be turned off explicitly. Nothing re-uses a retired actor -- a returning peer spawns a
    // fresh one -- so this is permanent by design.
    auto retire = [&](void* a) {
        if (!a) return;
#ifdef _WIN32
        if (S.SetActorCollision) { __try { S.SetActorCollision(a, false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (S.SetActorHidden)    { __try { S.SetActorHidden(a, true); }     __except (EXCEPTION_EXECUTE_HANDLER) {} }
#endif
    };
    retire(actor_);
    retire(OwnBoard());
    if (logf) logf(S.SetActorCollision
        ? "[proxy] peer left -- their skater and board hidden and decollided"
        : "[proxy] peer left -- their skater and board hidden (no collision symbol: they will still block the camera)");
}

void Proxy::OnQuiet(void (*logf)(const char*)) {
    if (!actor_) return;
    // Silence their loops. Loop liveness is "the sender keeps publishing it", so a sender who stops
    // publishing at all has, by that definition, stopped every sound -- and a peer whose connection
    // drops mid-grind must not leave the loop droning forever.
    AudioStopAll();
    // A mid-bail sender loss must not strand a ragdolled statue -- recover it (a no-op if the game's
    // own delay timer already did).
    if (lastBailing_) {
        lastBailing_ = 0;
        const Syms& S = Get();
        if (S.ResetRagDoll) {
#ifdef _WIN32
            __try { S.ResetRagDoll(actor_); } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
        }
        if (logf) logf("[proxy] stream quiet mid-bail -> ResetRagDoll");
    }
    // No board may keep simulating without a writer: left simulating after the sender quits, it
    // free-spins between stale stamps (a measured 116 deg of residual rotation).
    if (!simOn_) return;
    StopBoardSim();
    if (logf) logf("[proxy] stream quiet -> board simulation stopped (no writer, no zombie board)");
}

}} // namespace omp::game
