// SessionOpenMP -- our EOS SDK, loaded beside the game's. See eos_sideload.h for the why.
//
// HOW main.dll IS BOUND TO IT. The mod still links EOSSDK-Win64-Shipping.lib, so every EOS_* call site is
// an ordinary import -- but the linker is told /DELAYLOAD:EOSSDK-Win64-Shipping.dll, which turns each import
// into a thunk resolved on FIRST USE by the delay-load helper. The helper asks __pfnDliNotifyHook2 which
// module to use before it would LoadLibrary the name itself; this file answers with OUR copy, loaded by full
// path from the folder main.dll is in. The name in the import table is only ever a label after that.
//
// THE HELPER'S OWN FALLBACK IS THE DANGER: given no module it loads the DLL BY NAME -- and in the game that name
// is the GAME'S SDK, already in the process. Some of our imports exist in it; binding to those would put the
// mod back inside the game's SDK, silently. So with ours unavailable the hook answers with a module that has NO
// EOS exports (this one): every lookup then fails loudly instead. It should never come to that -- the
// transport asks SideloadReady() before its first EOS call and refuses the backend if the answer is no.
#include <windows.h>
#include <delayimp.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <eos_version.h>        // EOS_VERSION_STRING_BASE: the SDK this mod is built against ("1.17.0")
#include "eos_sideload.h"

namespace omp { namespace eosb {

static const wchar_t* const kOurFile    = L"OMP_EOSSDK-Win64-Shipping.dll";
static const char*    const kImportName = "EOSSDK-Win64-Shipping.dll";        // what the import table calls it
static const wchar_t* const kGameName   = L"EOSSDK-Win64-Shipping.dll";       // ...and what the GAME'S module is called

static HMODULE g_mod = nullptr;
static bool    g_tried = false;
static char    g_path[1024] = "";
static char    g_why[320] = "";

static HMODULE Self() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Self, &self);
    return self;
}
static HMODULE LoadOurs() {
    if (g_tried) return g_mod;
    g_tried = true;
    wchar_t dir[1024];
    const DWORD n = GetModuleFileNameW(Self(), dir, (DWORD)(sizeof(dir) / sizeof(dir[0])));
    wchar_t* slash = (n && n < sizeof(dir) / sizeof(dir[0])) ? wcsrchr(dir, L'\\') : nullptr;
    if (!slash) { snprintf(g_why, sizeof(g_why), "this mod could not find its own folder"); return nullptr; }
    slash[1] = 0;
    const std::wstring full = std::wstring(dir) + kOurFile;
    WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, g_path, (int)sizeof(g_path), nullptr, nullptr);
    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
        snprintf(g_why, sizeof(g_why), "OMP_EOSSDK-Win64-Shipping.dll is missing from Mods\\SessionOpenMP\\dlls -- reinstall SessionOpenMP (every file in the zip)");
        return nullptr;
    }
    HMODULE m = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { snprintf(g_why, sizeof(g_why), "Windows would not load OMP_EOSSDK-Win64-Shipping.dll (error %lu)", GetLastError()); return nullptr; }
    // It must be an EOS SDK, and it must NOT be the game's module under another path.
    if (!GetProcAddress(m, "EOS_Initialize") || !GetProcAddress(m, "EOS_Platform_Create") || !GetProcAddress(m, "EOS_GetVersion")) {
        snprintf(g_why, sizeof(g_why), "OMP_EOSSDK-Win64-Shipping.dll is not an EOS SDK -- reinstall SessionOpenMP");
        FreeLibrary(m);
        return nullptr;
    }
    if (m == GetModuleHandleW(kGameName)) {
        snprintf(g_why, sizeof(g_why), "OMP_EOSSDK-Win64-Shipping.dll resolved to the GAME'S EOS module -- refusing to share it");
        return nullptr;
    }
    g_mod = m;
    return m;
}

bool SideloadReady(char* why, size_t cap) {
    const bool ok = LoadOurs() != nullptr;
    if (why && cap) snprintf(why, cap, "%s", ok ? "" : g_why);
    return ok;
}
const char* SideloadPath() { return g_path; }

bool GameSdkWasReplaced(char* gameVersion, size_t cap) {
    if (gameVersion && cap) gameVersion[0] = 0;
    HMODULE game = GetModuleHandleW(kGameName);
    if (!game || game == g_mod) return false;                  // the game has no SDK loaded (yet): nothing to say
    typedef const char* (__cdecl *VerFn)();
    VerFn gv = (VerFn)GetProcAddress(game, "EOS_GetVersion");
    if (!gv) return false;
    const char* g = nullptr;
    __try { g = gv(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }      // a getter of a constant string: any thread
    if (!g) return false;
    if (gameVersion && cap) snprintf(gameVersion, cap, "%s", g);
    // The game ships 1.13. The version THIS MOD IS BUILT AGAINST, sitting in the game's slot, is the old
    // install's overwrite and nothing else is. Compared with what we were compiled against, so it can be asked
    // before our own copy has ever been loaded (the menu asks).
    return !strncmp(g, EOS_VERSION_STRING_BASE, strlen(EOS_VERSION_STRING_BASE));
}

}} // namespace omp::eosb

// ------------------------------------------------------------------ the delay-load hooks (one pair per module)
static FARPROC WINAPI OmpDelayNotify(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify != dliNotePreLoadLibrary || !pdli || !pdli->szDll) return nullptr;
    if (_stricmp(pdli->szDll, omp::eosb::kImportName) != 0) return nullptr;       // some other delay-loaded DLL: not ours to answer
    HMODULE m = omp::eosb::LoadOurs();
    // NEVER null here: null tells the helper to LoadLibrary the NAME, which in the game is the game's SDK.
    return (FARPROC)(m ? m : omp::eosb::Self());
}
static FARPROC WINAPI OmpDelayFail(unsigned, PDelayLoadInfo) { return nullptr; }   // let it raise: an EOS call with no SDK is a bug upstream
extern "C" const PfnDliHook __pfnDliNotifyHook2  = OmpDelayNotify;
extern "C" const PfnDliHook __pfnDliFailureHook2 = OmpDelayFail;
