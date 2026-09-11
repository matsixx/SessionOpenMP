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
// ASkaterCharacterBase::SetTrick(def, float, float, float, uint8, ...): the body reads a fifth
// argument at [rsp+0xd0] and a sixth at [rsp+0xd8], i.e. STACK arguments past the four register
// ones. A hook that forwards only four hands the callee its own frame there -- garbage, which on a
// grind exit (where those arguments carry) sent the skater flying. Eight are forwarded, the same
// prototype the tweaks DLL's pop_probe.cpp uses on this function. Only the fact of the call is taken.
using SetTrickFn = void (*)(void* self, void* def, float a, float b,
                            uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8);
SetTrickFn o_SetTrick = nullptr;

void hkSetTrick(void* self, void* def, float a, float b,
                uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    o_SetTrick(self, def, a, b, a5, a6, a7, a8);
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
