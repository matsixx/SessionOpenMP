// The trick serial -- see trick_pulse.h.
#include "trick_pulse.h"
#include "game_syms.h"
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif

namespace omp::game::trick {

namespace {
void*    g_ownPawn = nullptr;
uint8_t  g_serial  = 0;
int      g_logged  = 0;
void   (*g_logf)(const char*) = nullptr;
}  // namespace

void    NoteOwnPawn(void* pawn) { g_ownPawn = pawn; }
uint8_t OwnSerial()             { return g_serial; }

#ifdef OMP_USE_MINHOOK
namespace {
// ASkaterCharacterBase::SetTrick(UFlipTrickDefinition* def, float, float). Whatever it returns is
// passed through untouched; only the fact of the call is taken.
using SetTrickFn = uint64_t (*)(void* self, void* def, float a, float b);
SetTrickFn o_SetTrick = nullptr;

uint64_t hkSetTrick(void* self, void* def, float a, float b) {
    const uint64_t r = o_SetTrick(self, def, a, b);
    if (self && self == g_ownPawn) {
        g_serial++;
        if (g_logf && g_logged < 8) {
            g_logged++;
            char m[120];
            snprintf(m, sizeof(m), "[trick] own trick set -> serial %u%s", (unsigned)g_serial,
                     def ? "" : " (def null)");
            g_logf(m);
        }
    }
    return r;
}
}  // namespace

void Install(void (*logf)(const char*)) {
    static bool done = false;
    if (done) return;
    done = true;
    g_logf = logf;
    const Syms& S = Get();
    if (!S.SetTrick) {
        if (logf) logf("[trick] SetTrick NOT RESOLVED -- peers will not see your tricks start on time");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { if (logf) logf("[trick] MH_Initialize failed"); return; }
    if (MH_CreateHook(S.SetTrick, (void*)&hkSetTrick, (void**)&o_SetTrick) == MH_OK &&
        MH_EnableHook(S.SetTrick) == MH_OK) {
        if (logf) logf("[trick] flick seam hooked (SetTrick) -- each of your tricks carries a serial, "
                       "so a peer's skater starts the trick animation when yours does");
    } else if (logf) logf("[trick] hook FAILED on SetTrick -- peers will not see your tricks start on time");
}
#else
void Install(void (*)(const char*)) {}
#endif

} // namespace omp::game::trick
