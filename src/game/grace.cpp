// Spawn grace -- see grace.h for what it is and why each trigger is read where it is.
#include "grace.h"
#include "game_syms.h"
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif
#endif

namespace omp { namespace game { namespace grace {

int releaseMs = 1000;

#ifdef _WIN32
namespace {

void*          g_pawn        = nullptr;   // the pawn the state below is about
bool           g_active      = false;
uint64_t       g_releaseAtMs = 0;         // 0 = no push seen yet
void*          g_spawnArmedFor = nullptr; // the pawn whose spawn flag has already armed us once
volatile LONG  g_markerHit   = 0;         // set by the hook, consumed by Tick

using GotoFn = void (*)(void* self);
GotoFn o_Goto = nullptr;

void arm(uint64_t nowMs, const char* why, void (*logf)(const char*)) {
    // Re-arming resets the release: a marker return DURING the release delay is a new arrival.
    g_active = true; g_releaseAtMs = 0;
    if (logf) { char m[160];
        snprintf(m, sizeof(m), "[grace] %s -- your collision is off until you push (+%d ms)", why, releaseMs);
        logf(m); }
}

// PRE-hook: the flag is read before the original consumes it. Proxies run this too; only ours arms.
void hkGoto(void* self) {
    if (self && self == g_pawn) {
        __try {
            if (*((const uint8_t*)self + off::kSkaterGotoMarkerFlag) & 1) InterlockedExchange(&g_markerHit, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    o_Goto(self);
}

}  // namespace

void Install(void (*logf)(const char*)) {
#ifdef OMP_USE_MINHOOK
    static bool done = false;
    if (done) return;
    done = true;
    const Syms& S = Get();
    if (!S.GotoMarkerUpdate) {
        if (logf) logf("[grace] marker-return symbol NOT RESOLVED -- spawn grace works, marker returns will not arm it");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { if (logf) logf("[grace] MH_Initialize failed"); return; }
    if (MH_CreateHook(S.GotoMarkerUpdate, (void*)&hkGoto, (void**)&o_Goto) == MH_OK &&
        MH_EnableHook(S.GotoMarkerUpdate) == MH_OK) {
        if (logf) logf("[grace] marker-return seam hooked (UpdatePendingGotoMarker, read on entry)");
    } else if (logf) logf("[grace] hook FAILED on UpdatePendingGotoMarker -- marker returns will not arm grace");
#else
    (void)logf;
#endif
}

void Tick(void* ownPawn, uint64_t nowMs, uint8_t pushState, void (*logf)(const char*)) {
    if (!ownPawn) { g_pawn = nullptr; g_active = false; return; }
    // A different pawn IS a spawn: respawn, world change, anything that built a new skater.
    if (ownPawn != g_pawn) { g_pawn = ownPawn; g_spawnArmedFor = nullptr; arm(nowMs, "spawned", logf); }
    // The game's own spawn flag, once per pawn. Long-lived (cleared by the first mount), so no race.
    if (g_spawnArmedFor != ownPawn) {
        int just = -1;
        __try { just = *((const uint8_t*)ownPawn + off::kSkaterWasJustSpawned); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (just == 1) { g_spawnArmedFor = ownPawn; if (!g_active) arm(nowMs, "spawned (game flag)", logf); }
    }
    if (InterlockedExchange(&g_markerHit, 0)) arm(nowMs, "returned to a marker", logf);
    if (!g_active) return;
    if (pushState != 0 && g_releaseAtMs == 0) {
        g_releaseAtMs = nowMs + (uint64_t)(releaseMs < 0 ? 0 : releaseMs);
        if (logf) { char m[120];
            snprintf(m, sizeof(m), "[grace] push seen (state %u) -- collision back in %d ms", pushState, releaseMs);
            logf(m); }
    }
    if (g_releaseAtMs && nowMs >= g_releaseAtMs) {
        g_active = false; g_releaseAtMs = 0;
        if (logf) logf("[grace] collision is back");
    }
}

bool Active() { return g_active; }

#else
void Install(void (*)(const char*)) {}
void Tick(void*, uint64_t, uint8_t, void (*)(const char*)) {}
bool Active() { return false; }
#endif

}}} // namespace omp::game::grace
