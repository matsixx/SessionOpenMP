// omp_scantest -- SessionTweaks' signature search (src/tweaks/tweaks_common.cpp TwkScanExe / TwkScanExeNth) finds a
// function that somebody has HOOKED.
//
// Hooking (MinHook, as OpenMP's audio funnel does) rewrites a function's first bytes with a jump, and a signature IS
// those first bytes: in memory the function is gone. Field 2026-09-19: the radio, first used in a co-op session, was
// refused -- "not available" -- because OpenMP had hooked the sound spawner it calls. Here a function of this very
// program is hooked the same way (a 5-byte relative jump over its entry), and the search must still return it: from
// the exe on disk, at the address it has in memory.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "tweaks_common.h"

void TwkLog(const char* fmt, ...) { va_list a; va_start(a, fmt); printf("    log: "); vprintf(fmt, a); va_end(a); printf("\n"); }

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail++; }

// Two functions with bytes nothing else in this program has (the constants), and no absolute addresses in them --
// so the bytes on disk are the bytes in memory.
__declspec(noinline) int Target(volatile int x) { int r = x * 0x5A17C3D1 + 0x1234ABCD; r ^= (r >> 7) * 0x02F6E2B1; return r + 0x77AA55CC; }
__declspec(noinline) int Elsewhere(volatile int x) { return x * 0x3C6EF372 + 0x0BADF00D; }

static void SigOf(const uint8_t* p, int n, char* out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 3, "%02X ", p[i]);
    out[n * 3 - 1] = 0;
}

int main() {
    uint8_t* f = (uint8_t*)&Target;
    if (f[0] == 0xE9) f = f + 5 + *(int32_t*)(f + 1);          // an incremental-link thunk: the function is where it jumps
    char sig[40 * 3 + 1]; SigOf(f, 40, sig);
    printf("Target at exe+%llx\n", (unsigned long long)(f - (uint8_t*)GetModuleHandleA(nullptr)));

    Check(TwkScanExe(sig) == f, "unhooked: found in memory, at the function");
    Check(TwkScanExeNth(sig, 0) == f, "...and as the first of its kind (the disk is asked first there)");
    Check(TwkScanExeNth(sig, 1) == nullptr, "...and it has no twin");

    // HOOK IT, as MinHook does: a relative jump over its first five bytes
    DWORD old = 0;
    if (!VirtualProtect(f, 16, PAGE_EXECUTE_READWRITE, &old)) { printf("could not unprotect\n"); return 2; }
    uint8_t save[5]; memcpy(save, f, 5);
    const int32_t rel = (int32_t)((uint8_t*)&Elsewhere - (f + 5));
    f[0] = 0xE9; memcpy(f + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
    char memSig[40 * 3 + 1]; SigOf(f, 40, memSig);
    Check(strcmp(memSig, sig) != 0, "hooked: its first bytes in memory are a jump now");
    Check(Target(3) == Elsewhere(3), "...and calling it goes where the hook sends it");

    Check(TwkScanExe(sig) == f, "HOOKED: still found -- from the exe on disk, at its address in memory");
    Check(TwkScanExeNth(sig, 0) == f, "...by the Nth search too");
    Check(TwkScanExe("DE AD BE EF 13 37 C0 DE 99 88 77 66 55 44 33 22 11 00 FF EE") == nullptr, "a signature that exists nowhere is still not found");

    memcpy(f, save, 5); FlushInstructionCache(GetCurrentProcess(), f, 5); VirtualProtect(f, 16, old, &old);
    Check(TwkScanExe(sig) == f, "unhooked again: found in memory");
    printf(g_fail ? "SCAN TEST FAIL (%d)\n" : "SCAN TEST PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
