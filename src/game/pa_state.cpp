// The owner's physical-animation lifecycle -- see pa_state.h.
#include "pa_state.h"
#include "game_syms.h"
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif

namespace omp::game::pa {

namespace {
void*    g_ownPawn = nullptr;
bool     g_enabled = false;
uint8_t  g_changes = 0;
int      g_logged  = 0;
void   (*g_logf)(const char*) = nullptr;
}  // namespace

void NoteOwnPawn(void* pawn) {
    if (pawn == g_ownPawn) return;
    g_ownPawn = pawn;
    // Seed from the enable bit: steady state at join is what it says; from here the broadcasts lead.
    g_enabled = false;
#ifdef _WIN32
    if (pawn) { __try { g_enabled = ((*((const uint8_t*)pawn + off::kSkaterPhysAnimOn) >> 4) & 1) != 0; }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_enabled = false; } }
#endif
}

uint8_t OwnPaSerial() {
    return (uint8_t)(0x01 | (g_enabled ? 0x02 : 0x00) | ((g_changes & 0x3f) << 2));
}

#ifdef OMP_USE_MINHOOK
namespace {
using BcastFn = uint64_t (*)(void* self);
BcastFn o_Disable = nullptr, o_Enable = nullptr;

void note(void* self, bool on) {
    if (!self || self != g_ownPawn) return;             // proxies get these too, from proxy.cpp
    g_enabled = on; g_changes++;
    if (g_logf && g_logged < 8) {
        g_logged++;
        char m[100];
        snprintf(m, sizeof(m), "[pa] own body physics %s (broadcast #%u)", on ? "ON" : "OFF", (unsigned)g_changes);
        g_logf(m);
    }
}
uint64_t hkDisable(void* self) { const uint64_t r = o_Disable(self); note(self, false); return r; }
uint64_t hkEnable (void* self) { const uint64_t r = o_Enable(self);  note(self, true);  return r; }
}  // namespace

void Install(void (*logf)(const char*)) {
    static bool done = false;
    if (done) return;
    done = true;
    g_logf = logf;
    const Syms& S = Get();
    if (!S.BcastPaDisable || !S.BcastPaEnable) {
        if (logf) logf("[pa] physical-animation broadcasts NOT RESOLVED -- peers keep their own guess at your body physics");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { if (logf) logf("[pa] MH_Initialize failed"); return; }
    const bool ok = MH_CreateHook(S.BcastPaDisable, (void*)&hkDisable, (void**)&o_Disable) == MH_OK &&
                    MH_CreateHook(S.BcastPaEnable,  (void*)&hkEnable,  (void**)&o_Enable)  == MH_OK &&
                    MH_EnableHook(S.BcastPaDisable) == MH_OK && MH_EnableHook(S.BcastPaEnable) == MH_OK;
    if (logf) logf(ok ? "[pa] physical-animation broadcasts hooked -- a peer's skater switches body physics off and on when yours does"
                      : "[pa] hook FAILED on the physical-animation broadcasts");
}
#else
void Install(void (*)(const char*)) {}
#endif

} // namespace omp::game::pa
