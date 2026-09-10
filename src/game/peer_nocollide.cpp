// A proxy skater that ignores other skaters -- see peer_nocollide.h for why this replaces
// SetActorEnableCollision(false), and for WHICH container the ignore has to be written to.
#include "peer_nocollide.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp::game {

#ifdef _WIN32
namespace {

constexpr int kMaxMeshes = 16;
constexpr int kMaxBodies = 48;
constexpr int kMaxChans  = 5;
// ECollisionChannel: the two every skater is built from. The rest are read live off the components.
constexpr int32_t kChanPawn        = 2;
constexpr int32_t kChanPhysicsBody = 5;
constexpr int32_t kRespIgnore      = 0;

struct TArr { const uint8_t* data; int32_t num; int32_t max; };

// One response container: what each of the ignored channels answered before it was written.
struct Saved { uint8_t done; uint8_t was[kMaxChans]; };

struct Rec {
    void*    mesh;
    void*    capsule;           // the root capsule's body instance, or null
    int32_t  chan[kMaxChans];   // the channels other skaters arrive on, deduplicated
    int      nchan;
    int32_t  num;
    bool     active;
    int      relogs;            // re-apply lines printed for this skater (capped)
    Saved    cap;               // the capsule's own container -- effective, it is a plain component
    Saved    comp;              // the MESH COMPONENT's container -- what its bodies are filtered by
    void*    bi[kMaxBodies];    // the body instance each slot held last pass (a re-dress swaps them)
};
Rec g_rec[kMaxMeshes];

Rec* SlotFor(void* mesh, bool claim) {
    for (auto& r : g_rec) if (r.active && r.mesh == mesh) return &r;
    if (!claim) return nullptr;
    for (auto& r : g_rec) if (!r.active) return &r;
    return nullptr;
}

int32_t ObjTypeOf(const void* bodyInst) {
    return bodyInst ? (int32_t)*((const uint8_t*)bodyInst + off::kBodyObjectType) : -1;
}

// A channel a skater can arrive on. WorldStatic (0) and WorldDynamic (1) are refused outright: a
// misread byte that landed on one of those would have this skater's bodies ignore the level and
// fall through the floor, and no skater component is ever typed as level geometry.
void AddChan(Rec& r, int32_t c) {
    if (c < 2 || c >= 32) return;
    for (int i = 0; i < r.nchan; i++) if (r.chan[i] == c) return;
    if (r.nchan < kMaxChans) r.chan[r.nchan++] = c;
}

// One container, decided PER CHANNEL by what it answers right now: an answer that is not Ignore is
// the game's own -- the profile, or something written back -- so it is remembered and replaced; one
// already at Ignore is ours and left alone. Nothing depends on this being the same object as last
// pass. FBodyInstance::SetResponseToChannel rebuilds the filter itself where the instance has a
// physics actor (the capsule); the mesh component's own instance has none, so its bodies are
// refiltered by the caller. Returns how many channels were written.
int ignoreOn(void* bi, const Rec& r, Saved& s) {
    const Syms& S = Get();
    if (!bi || !S.BodySetResponse) return 0;
    const uint8_t* rc = (const uint8_t*)bi + off::kBodyResponses;
    int n = 0;
    for (int i = 0; i < r.nchan; i++) {
        const int32_t c = r.chan[i];
        if (rc[c] != kRespIgnore) { s.was[i] = rc[c]; S.BodySetResponse(bi, c, kRespIgnore); n++; }
        else if (!s.done)          s.was[i] = kRespIgnore;
    }
    s.done = 1;
    return n;
}

void restoreOn(void* bi, const Rec& r, Saved& s) {
    const Syms& S = Get();
    if (!s.done || !bi || !S.BodySetResponse) return;
    for (int i = 0; i < r.nchan; i++) S.BodySetResponse(bi, r.chan[i], s.was[i]);
    s.done = 0;
}

// Rebuild every body's shape filter from the component's container -- the loop the engine's own
// USkeletalMeshComponent::OnComponentCollisionSettingsChanged runs.
int refilterAll(const TArr& a) {
    const Syms& S = Get();
    if (!S.BodyUpdateFilter) return 0;
    int n = 0;
    for (int i = 0; i < a.num; i++) {
        void* bi = ((void* const*)a.data)[i];
        if (bi) { S.BodyUpdateFilter(bi); n++; }
    }
    return n;
}

void FormatChans(const Rec& r, char* out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < r.nchan && at < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, "%s%d", i ? " " : "", r.chan[i]);
}

}  // namespace

void PeerNoCollideApply(void* meshComp, void* capsuleBody, void* boardBody, void (*logf)(const char*)) {
    if (!meshComp) return;
    __try {
        TArr a{};
        memcpy(&a, (const uint8_t*)meshComp + off::kMeshBodies, sizeof(a));
        if (!a.data || a.num <= 0 || a.num > kMaxBodies) return;
        Rec* r = SlotFor(meshComp, true);
        if (!r) return;
        void* compBody = (uint8_t*)meshComp + off::kCompBodyInstance;
        const bool fresh = !r->active;
        if (fresh) {
            *r = Rec{};
            r->mesh = meshComp; r->capsule = capsuleBody;
            // THE CHANNELS ANOTHER SKATER ARRIVES ON, read off this one's own components -- the two
            // skaters are the same class. The engine's two first, then whatever the capsule, the
            // mesh and the board are typed as.
            AddChan(*r, kChanPawn);
            AddChan(*r, kChanPhysicsBody);
            AddChan(*r, ObjTypeOf(capsuleBody));
            AddChan(*r, ObjTypeOf(compBody));
            AddChan(*r, ObjTypeOf(boardBody));
            r->active = true;                        // claimed BEFORE any write -- a fault mid-pass
        }                                            // must leave a record that can put things back
        r->num = a.num;

        // 1. the capsule: its own container.
        const bool capHad = r->cap.done != 0;
        const int  capW   = ignoreOn(r->capsule, *r, r->cap);
        // 2. the bodies: the MESH COMPONENT's container (peer_nocollide.h), then their filters.
        const bool compHad = r->comp.done != 0;
        const int  compW   = ignoreOn(compBody, *r, r->comp);
        int refiltered = 0;
        if (compW) {
            refiltered = refilterAll(a);
            for (int i = 0; i < a.num; i++) r->bi[i] = ((void* const*)a.data)[i];
        } else {
            // A body swapped since the last pass (a re-dress builds new ones) reads the container
            // at its init and should already be right; refiltering it once is the cheap insurance.
            const Syms& S = Get();
            for (int i = 0; i < a.num; i++) {
                void* bi = ((void* const*)a.data)[i];
                if (bi == r->bi[i]) continue;
                r->bi[i] = bi;
                if (bi && S.BodyUpdateFilter) { S.BodyUpdateFilter(bi); refiltered++; }
            }
        }

        if (fresh && logf) {
            char cl[48], m[240];
            FormatChans(*r, cl, sizeof(cl));
            snprintf(m, sizeof(m), "[proxy] their skater ignores other skaters: channels [%s] on %s%s, "
                                   "%d of %d bodies refiltered -- collision stays on, body physics keeps running",
                     cl, r->comp.done ? "the mesh" : "nothing", r->cap.done ? " + capsule" : "",
                     refiltered, a.num);
            logf(m);
        } else if (((capW && capHad) || (compW && compHad)) && logf && r->relogs < 4) {
            char m[220];
            snprintf(m, sizeof(m), "[proxy] their %s collision responses had been reset (re-dress, or a profile"
                                   " change) -- skater-ignore re-applied, %d bodies refiltered%s",
                     (capW && compW) ? "capsule and mesh" : capW ? "capsule" : "mesh", refiltered,
                     ++r->relogs == 4 ? " (further ones on this skater not logged)" : "");
            logf(m);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (logf) logf("[proxy] skater-ignore FAULTED -- what was changed stays restorable");
    }
}

void PeerNoCollideRestore(void* meshComp, void* capsuleBody, void (*logf)(const char*)) {
    if (!meshComp) return;
    Rec* r = SlotFor(meshComp, false);
    if (!r) return;
    __try {
        if (r->capsule && r->capsule == capsuleBody) restoreOn(r->capsule, *r, r->cap);
        if (r->comp.done) {
            restoreOn((uint8_t*)meshComp + off::kCompBodyInstance, *r, r->comp);
            TArr a{};
            memcpy(&a, (const uint8_t*)meshComp + off::kMeshBodies, sizeof(a));
            if (a.data && a.num > 0 && a.num <= kMaxBodies) refilterAll(a);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (logf) logf("[proxy] skater-ignore RESTORE faulted");
    }
    if (logf) logf("[proxy] their skater collides with skaters again");
    *r = Rec{};
}

void PeerNoCollideForget(void* meshComp) {
    Rec* r = SlotFor(meshComp, false);
    if (r) *r = Rec{};
}

#else
void PeerNoCollideApply(void*, void*, void*, void (*)(const char*)) {}
void PeerNoCollideRestore(void*, void*, void (*)(const char*))     {}
void PeerNoCollideForget(void*)                                     {}
#endif

} // namespace omp::game
