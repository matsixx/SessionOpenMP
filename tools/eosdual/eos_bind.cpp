// omp_eosbind -- does main.dll's way of reaching EOS really reach OUR SDK, with the game's in the process?
//
// omp_eosdual proves two SDKs can live together. This proves the MECHANISM the mod uses to reach its own:
// built from the SAME src/transport/eos_sideload.cpp, linked the SAME way (the import library, plus
// /DELAYLOAD:EOSSDK-Win64-Shipping.dll), it first does what the game does -- loads the GAME'S SDK under its
// real name and initialises it -- and then makes ORDINARY EOS_* calls, exactly as eos_transport.cpp does.
// Those must land in OMP_EOSSDK-Win64-Shipping.dll beside this exe: our version, a state of our own
// (EOS_Success, not EOS_AlreadyConfigured), in a module that is not the game's.
//
//   omp_eosbind <game's EOS dll>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <eos_sdk.h>
#include <eos_init.h>
#include <eos_version.h>
#include "transport/eos_sideload.h"

static int g_fail = 0;
static void Check(bool ok, const char* what, const char* detail = "") {
    printf("  %s  %s %s\n", ok ? "ok  " : "FAIL", what, detail);
    if (!ok) g_fail++;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: omp_eosbind <game's EOS dll>\n"); return 2; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"omp_eosbind\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring gamePath = dir + L"EOSSDK-Win64-Shipping.dll";
    if (!CopyFileW(argv[1], gamePath.c_str(), FALSE)) { printf("could not stage the game's DLL (%lu)\n", GetLastError()); return 2; }

    printf("the game's SDK first, under its real name, as in the game:\n");
    HMODULE game = LoadLibraryExW(gamePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(game != nullptr, "loaded");
    if (!game) return 1;
    typedef EOS_EResult (EOS_CALL *InitFn)(const EOS_InitializeOptions*);
    typedef const char* (EOS_CALL *VerFn)();
    EOS_InitializeOptions gi{}; gi.ApiVersion = EOS_INITIALIZE_API_LATEST; gi.ProductName = "Session"; gi.ProductVersion = "1.0";
    const EOS_EResult gr = ((InitFn)GetProcAddress(game, "EOS_Initialize"))(&gi);
    const char* gameVer = ((VerFn)GetProcAddress(game, "EOS_GetVersion"))();
    char buf[200]; snprintf(buf, sizeof(buf), "(version %s)", gameVer);
    Check(gr == EOS_EResult::EOS_Success, "initialised", buf);
    Check(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll") == game, "the name EOSSDK-Win64-Shipping.dll is the game's module");

    printf("now ORDINARY EOS_* calls, as the mod makes them:\n");
    char why[320];
    const bool ready = omp::eosb::SideloadReady(why, sizeof(why));
    snprintf(buf, sizeof(buf), "(%s)", ready ? omp::eosb::SideloadPath() : why);
    Check(ready, "our SDK is found beside this module", buf);
    if (!ready) { printf("EOS BIND FAIL\n"); return 1; }
    const char* ours = EOS_GetVersion();                                      // <- a delay-loaded import: the first one binds
    snprintf(buf, sizeof(buf), "(EOS_GetVersion says %s; the game's says %s)", ours, gameVer);
    // THE OLD-INSTALL STATE: handed OUR SDK as "the game's" (an older install overwrote the game's file), the two
    // versions are the same string -- so which SDK a call landed in is judged by the MODULE below, not by this.
    const bool oldInstall = !strncmp(gameVer, EOS_VERSION_STRING_BASE, strlen(EOS_VERSION_STRING_BASE));
    if (oldInstall) printf("  (the \"game's\" SDK here is OUR version: this is the state an older install leaves behind)\n");
    Check(ours && !strncmp(ours, EOS_VERSION_STRING_BASE, strlen(EOS_VERSION_STRING_BASE)) && (oldInstall || strcmp(ours, gameVer) != 0), "the call landed in an SDK of the version this was built against", buf);
    HMODULE landed = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ours, &landed);
    wchar_t where[1024] = L""; GetModuleFileNameW(landed, where, 1024);
    snprintf(buf, sizeof(buf), "(%ls)", where);
    Check(landed && landed != game && wcsstr(where, L"OMP_EOSSDK-Win64-Shipping.dll") != nullptr, "...in the module OMP_EOSSDK-Win64-Shipping.dll, not the game's", buf);
    EOS_InitializeOptions oi{}; oi.ApiVersion = EOS_INITIALIZE_API_LATEST; oi.ProductName = "SessionOpenMP"; oi.ProductVersion = "0.1";
    const EOS_EResult r = EOS_Initialize(&oi);
    snprintf(buf, sizeof(buf), "(%s)", EOS_EResult_ToString(r));
    Check(r == EOS_EResult::EOS_Success, "EOS_Initialize answers SUCCESS: our own state, though the game's SDK is already initialised", buf);
    char gv[64];
    const bool saysReplaced = omp::eosb::GameSdkWasReplaced(gv, sizeof(gv));
    if (oldInstall) Check(saysReplaced && !strcmp(gv, gameVer), "the overwritten game SDK is DETECTED (the player is told to verify files) -- and the mod works regardless", gv);
    else            Check(!saysReplaced && !strcmp(gv, gameVer), "the game's SDK is seen for what it is: its own version, not replaced", gv);
    Check(EOS_Shutdown() == EOS_EResult::EOS_Success, "ours shuts down");
    typedef EOS_EResult (EOS_CALL *ShutFn)();
    Check(((ShutFn)GetProcAddress(game, "EOS_Shutdown"))() == EOS_EResult::EOS_Success, "...and the game's was never touched by any of it");
    printf(g_fail ? "EOS BIND FAIL (%d)\n" : "EOS BIND PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
