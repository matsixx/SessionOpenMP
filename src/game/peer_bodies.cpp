// Peer body trim -- see peer_bodies.h for what this is for and why the restore is not optional.
#include "peer_bodies.h"

#include "game_syms.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace omp::game {

bool trimPeerLegs      = true;
bool trimPeerWorldHits = true;
int  trimPeerIterDiv   = 2;

#ifdef _WIN32

namespace {

constexpr int kMaxMeshes = 16;   // one per proxy this session can hold
constexpr int kMaxBodies = 48;   // a skater PhysicsAsset is about 21; this is the sanity bound

// The two channels the level is built from. A peer's arm passing through a rail is invisible at the
// distance another skater is ever watched from; the narrowphase against the level is not.
constexpr uint8_t kChanWorldStatic  = 0;
constexpr uint8_t kChanWorldDynamic = 1;
constexpr uint8_t kRespIgnore       = 0;

struct TArr { const uint8_t* data; int32_t num; int32_t max; };

struct BodyState {
    int8_t  kind;         // -1 not classified yet, 0 upper body, 1 hips and below
    uint8_t madeKin;      // this one was made kinematic here
    uint8_t didResp;      // ...had its level responses changed
    uint8_t didIters;     // ...had its solver counts divided down
    uint8_t respStatic;   // and what all of those were before
    uint8_t respDynamic;
    uint8_t posIters;
    uint8_t velIters;
};

struct MeshTrim {
    void*       mesh;
    const void* bodiesData;   // the Bodies array as it was when this was filled in
    int32_t     num;
    uint64_t    recheckMs;
    int         kin, hits, iters;
    int         reasserts;    // times a leg had to be put back -- see the "keeps putting them back" log
    bool        active;
    bool        saidStubborn;
};

MeshTrim g_trim[kMaxMeshes];
BodyState g_body[kMaxMeshes][kMaxBodies];

MeshTrim* SlotFor(void* mesh, bool claim) {
    for (int i = 0; i < kMaxMeshes; i++)
        if (g_trim[i].active && g_trim[i].mesh == mesh) return &g_trim[i];
    if (!claim) return nullptr;
    for (int i = 0; i < kMaxMeshes; i++)
        if (!g_trim[i].active) return &g_trim[i];
    return nullptr;   // more proxies than slots: the extras keep the game's own behaviour
}

int SlotIndex(const MeshTrim* t) { return (int)(t - &g_trim[0]); }

// ---- what this actually costs, measured -----------------------------------------------------------
// A trim or a restore is CHEAP while it holds -- two reads and out -- but the first pass on a mesh
// writes every body, and UpdatePhysicsFilterData rebuilds shape filter data in the scene. That pass
// runs again on every physical-animation edge, which in a busy lobby means every time anyone steps on
// or off a board or bails. Several peers doing that at once is exactly the shape of a frame spike, so
// the cost is timed rather than argued about: anything over the threshold says so, with what it was
// doing and how many bodies it wrote.
double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
float slowPassMs = 1.5f;      // report a pass that took longer than this
long  g_slowPasses = 0;       // ...and how many there have been, for the line itself

// ---- the solver counts -------------------------------------------------------------------------
// PositionSolverIterationCount lives on the FBodyInstance, but the number the simulation actually
// uses lives on the PhysX actor, and pushing it there is an "AssumesLocked" call. The engine's own
// FPhysicsCommand_PhysX::ExecuteWrite is what takes that lock: it dereferences the actor handle,
// takes the scene's write lock, calls back through a TFunctionRef and unlocks. The TFunctionRef is
// two pointers -- the thunk first, the callable second -- and the thunk is invoked as
// thunk(callable, actorHandleRef), which is exactly the shape built here.
struct IterJob { int pos, vel; };
struct FnRef   { void* thunk; void* self; };

void IterThunk(void* self, const void* handleRef) {
    const IterJob* j = (const IterJob*)self;
    const Syms& S = Get();
    if (S.SetSolverPosIters) S.SetSolverPosIters(handleRef, j->pos);
    if (S.SetSolverVelIters) S.SetSolverVelIters(handleRef, j->vel);
}

void PushIters(const void* bi, int pos, int vel) {
    const Syms& S = Get();
    if (!S.PhysExecuteWrite) return;
    IterJob job{ pos, vel };
    FnRef   fr{ (void*)&IterThunk, &job };
    // A body with no PhysX actor yet reads a null handle, which ExecuteWrite returns false on.
    S.PhysExecuteWrite((const uint8_t*)bi + off::kBodyActorHandle, &fr);
}

// ---- which bodies are legs ---------------------------------------------------------------------
// By bone name, so a rig that names things differently is not silently mis-trimmed: a body whose
// name cannot be read stays classified as upper body, which is the behaviour that changes nothing.
bool NameIsLeg(const char* n) {
    return strstr(n, "pelvis") || strstr(n, "hips") || strstr(n, "thigh") || strstr(n, "calf") ||
           strstr(n, "shin")   || strstr(n, "knee") || strstr(n, "foot")  || strstr(n, "toe")  ||
           strstr(n, "ball")   || strstr(n, "leg");
}

int8_t ClassifyBody(void* meshComp, const void* bi) {
    char nm[96];
    nm[0] = 0;
    const int16_t bone = *(const int16_t*)((const uint8_t*)bi + off::kBodyBoneIndex);
    if (bone < 0) return 0;
    const void* skelMesh = *(void* const*)((const uint8_t*)meshComp + off::kMeshSkeletalMesh);
    if (!skelMesh) return 0;
    TArr a{};
    memcpy(&a, (const uint8_t*)skelMesh + off::kSkelMeshRefSkeleton + off::kRefSkelFinalBoneInfo, sizeof(a));
    if (!a.data || bone >= a.num || a.num <= 0 || a.num > 4096) return 0;
    if (!FNameAscii(a.data + (size_t)bone * off::kMeshBoneInfoStride, nm, sizeof(nm))) return 0;
    for (char* c = nm; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    return NameIsLeg(nm) ? (int8_t)1 : (int8_t)0;
}

}  // namespace

void PeerBodiesTrim(void* meshComp, uint64_t nowMs, TrimLogFn logf) {
    if (!meshComp) return;
    const Syms& S = Get();
    const bool canKin  = trimPeerLegs      && S.BodySetSimulate;
    const bool canResp = trimPeerWorldHits && S.BodySetResponse && S.BodyUpdateFilter;
    const bool canIter = trimPeerIterDiv > 1 && S.PhysExecuteWrite && S.SetSolverPosIters;
    if (!canKin && !canResp && !canIter) return;

    MeshTrim* t = SlotFor(meshComp, true);
    if (!t) return;

    bool fresh = false, stubborn = false;
    const double t0 = NowMs();
    __try {
        TArr a{};
        memcpy(&a, (const uint8_t*)meshComp + off::kMeshBodies, sizeof(a));
        if (!a.data || a.num <= 0 || a.num > kMaxBodies) return;

        // A re-dress builds a new merged mesh and a new body array. What is remembered then belongs
        // to bodies that no longer exist, so it is DROPPED rather than restored -- writing through
        // those pointers is exactly the crash this check exists to avoid.
        if (t->active && (t->bodiesData != a.data || t->num != a.num)) { *t = MeshTrim{}; }
        // Stepping on or off the board changes WHICH bodies should simulate, so it must not wait for
        // the timer -- that would leave up to half a second of flop on every dismount.
        if (t->active && nowMs < t->recheckMs) return;   // the cheap path: two reads and out

        BodyState* bs = g_body[SlotIndex(t)];
        if (!t->active) {
            fresh = true;
            memset(bs, 0, sizeof(BodyState) * kMaxBodies);
            for (int i = 0; i < kMaxBodies; i++) bs[i].kind = -1;
            // Claimed BEFORE a single body is written. A fault partway through the loop leaves
            // bodies already changed, and without a record nothing can put them back -- that peer
            // would keep a kinematic leg and no level collision for the rest of the session.
            t->mesh = meshComp; t->bodiesData = a.data; t->num = a.num; t->active = true;
        }
        t->recheckMs = nowMs + 500;

        int kin = 0, hits = 0, iters = 0;
        for (int i = 0; i < a.num; i++) {
            void* bi = ((void* const*)a.data)[i];
            if (!bi) continue;
            BodyState& st = bs[i];
            if (st.kind < 0) st.kind = ClassifyBody(meshComp, bi);

            // 1. the legs ride the animation. Re-asserted on the timer because the blueprint that
            //    drives this skater's body physics is free to put a body back to simulating.
            if (canKin && st.kind == 1) {
                const uint8_t simByte = *((const uint8_t*)bi + off::kBodySimByte);
                if ((simByte >> off::kBodySimBit) & 1) {
                    S.BodySetSimulate(bi, false, false, false);
                    if (st.madeKin) t->reasserts++;   // it had been put back since the last pass
                    st.madeKin = 1;
                }
                if (st.madeKin) kin++;
            }

            // 2. the level. Only a body that answers to it at all is changed, and what it answered
            //    with is kept for the restore.
            if (canResp && !st.didResp) {
                const uint8_t* rc = (const uint8_t*)bi + off::kBodyResponses;
                st.respStatic  = rc[kChanWorldStatic];
                st.respDynamic = rc[kChanWorldDynamic];
                if (st.respStatic != kRespIgnore || st.respDynamic != kRespIgnore) {
                    S.BodySetResponse(bi, kChanWorldStatic,  kRespIgnore);
                    S.BodySetResponse(bi, kChanWorldDynamic, kRespIgnore);
                    S.BodyUpdateFilter(bi);
                    st.didResp = 1;
                }
            }
            if (st.didResp) hits++;

            // 3. the solver. Divided down rather than set to a number, so a body the asset author
            //    deliberately made cheap or expensive keeps its relative weight, with a floor of 2.
            if (canIter && !st.didIters) {
                uint8_t* pi = (uint8_t*)bi + off::kBodyPosIters;
                st.posIters = *pi;
                st.velIters = *((const uint8_t*)bi + off::kBodyVelIters);
                int np = (int)st.posIters / trimPeerIterDiv;
                if (np < 2) np = 2;
                if (np < (int)st.posIters) {
                    *pi = (uint8_t)np;
                    PushIters(bi, np, st.velIters);
                    st.didIters = 1;
                }
            }
            if (st.didIters) iters++;
        }
        t->kin = kin; t->hits = hits; t->iters = iters;
        stubborn = t->reasserts > 40 && !t->saidStubborn;
        if (stubborn) t->saidStubborn = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The record is KEPT so the restore can still undo whatever was written before the fault.
        // Backing the re-check well off stops a mesh that faults every pass from doing it at 2 Hz.
        t->recheckMs = nowMs + 5000;
        if (logf) logf("[proxy] body trim FAULTED -- what was changed stays restorable");
        return;
    }

    const double tookMs = NowMs() - t0;
    if (fresh && logf) {
        char m[200];
        snprintf(m, sizeof(m), "[proxy] body trim: %d of %d bodies ride the animation, "
                               "%d stop hitting the level, %d solve at 1/%d (%.2f ms)",
                 t->kin, t->num, t->hits, t->iters, trimPeerIterDiv, tookMs);
        logf(m);
    }
    if (tookMs > slowPassMs && logf) {
        char m[160];
        snprintf(m, sizeof(m), "[proxy] body trim pass took %.2f ms (%d bodies, %s) -- slow pass #%ld",
                 tookMs, t->num, fresh ? "first pass on this mesh" : "re-assert",
                 ++g_slowPasses);
        logf(m);
    }
    // Said once per peer. The trim is not holding: something on the game side is switching those
    // bodies back on, so the saving is not being made and the cause is worth knowing about.
    if (stubborn && logf) logf("[proxy] body trim: the game keeps putting the legs back -- not holding");
}

void PeerBodiesRestore(void* meshComp, TrimLogFn logf) {
    if (!meshComp) return;
    MeshTrim* t = SlotFor(meshComp, false);
    if (!t) return;
    const Syms& S = Get();
    const double t0 = NowMs();
    __try {
        TArr a{};
        memcpy(&a, (const uint8_t*)meshComp + off::kMeshBodies, sizeof(a));
        // Only the array this was filled in from. A rebuilt character's bodies are not ours to write.
        if (a.data == t->bodiesData && a.num == t->num && a.data) {
            BodyState* bs = g_body[SlotIndex(t)];
            for (int i = 0; i < t->num && i < kMaxBodies; i++) {
                void* bi = ((void* const*)a.data)[i];
                if (!bi) continue;
                BodyState& st = bs[i];
                if (st.didIters) {
                    *((uint8_t*)bi + off::kBodyPosIters) = st.posIters;
                    *((uint8_t*)bi + off::kBodyVelIters) = st.velIters;
                    PushIters(bi, st.posIters, st.velIters);
                }
                if (st.didResp && S.BodySetResponse && S.BodyUpdateFilter) {
                    S.BodySetResponse(bi, kChanWorldStatic,  st.respStatic);
                    S.BodySetResponse(bi, kChanWorldDynamic, st.respDynamic);
                    S.BodyUpdateFilter(bi);
                }
                // Simulation goes back LAST, so a body is never dynamic while it still ignores the
                // level -- that one-frame combination is a ragdoll falling through the floor.
                if (st.madeKin && S.BodySetSimulate) S.BodySetSimulate(bi, true, false, false);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const double tookMs = NowMs() - t0;
    if (logf) {
        char m[160];
        snprintf(m, sizeof(m), "[proxy] body trim lifted -- the whole asset is simulated again (%.2f ms)", tookMs);
        logf(m);
        if (tookMs > slowPassMs) {
            snprintf(m, sizeof(m), "[proxy] body trim RESTORE took %.2f ms (%d bodies) -- slow pass #%ld",
                     tookMs, t->num, ++g_slowPasses);
            logf(m);
        }
    }
    *t = MeshTrim{};
}

void PeerBodiesForget(void* meshComp) {
    MeshTrim* t = SlotFor(meshComp, false);
    if (t) *t = MeshTrim{};
}

int PeerBodiesTrimmedCount() {
    int n = 0;
    for (const auto& t : g_trim) if (t.active) n++;
    return n;
}

#else   // !_WIN32

void PeerBodiesTrim(void*, uint64_t, TrimLogFn)  {}
void PeerBodiesRestore(void*, TrimLogFn)         {}
void PeerBodiesForget(void*)                     {}
int  PeerBodiesTrimmedCount()                    { return 0; }

#endif

}  // namespace omp::game
