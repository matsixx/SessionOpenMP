// omp_eosdual -- can TWO EOS SDKs live in one process?
//
// SessionOpenMP used to be installed by overwriting the game's own EOSSDK-Win64-Shipping.dll (1.13) with
// the 1.17 it is built against. Session has its own EOS integration, so that put the GAME on an SDK it
// was not built for -- and on the Epic client its DLC stopped working. The fix is to leave the game's
// SDK alone and load ours beside it, under another name. Epic documents nothing about two copies of the
// SDK in one process, so before the mod is trusted to it this proves it, with the real binaries:
//
//   1. the game's SDK is loaded the way the game loads it -- by full path, under its own name -- and
//      initialised;
//   2. ours is loaded under ANOTHER name from ANOTHER folder and initialised: it must answer
//      EOS_Success, not EOS_AlreadyConfigured (which is what ONE shared SDK answers -- the old world);
//   3. each reports its own version, the name "EOSSDK-Win64-Shipping.dll" still means the GAME's module,
//      and a platform is created on ours (overlay off, as the mod does) and ticked next to the other.
//
//   omp_eosdual <game's 1.13 dll> <our 1.17 dll>        (copies them to a temp folder; touches nothing else)
//
// No network is needed and nobody is signed in: creating a platform is local.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "eos_sdk.h"
#include "eos_init.h"
#include "transport/eos_creds.h"

static int g_fail = 0;
static void Check(bool ok, const char* what, const char* detail = "") {
    printf("  %s  %s %s\n", ok ? "ok  " : "FAIL", what, detail);
    if (!ok) g_fail++;
}
typedef EOS_EResult (EOS_CALL *InitFn)(const EOS_InitializeOptions*);
typedef const char* (EOS_CALL *VerFn)();
typedef EOS_HPlatform (EOS_CALL *CreateFn)(const EOS_Platform_Options*);
typedef void (EOS_CALL *TickFn)(EOS_HPlatform);
typedef void (EOS_CALL *ReleaseFn)(EOS_HPlatform);
typedef EOS_EResult (EOS_CALL *ShutdownFn)();

struct Sdk { HMODULE mod; InitFn init; VerFn ver; CreateFn create; TickFn tick; ReleaseFn release; ShutdownFn shutdown; };
static bool Bind(Sdk& s, const std::wstring& path) {
    s.mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s.mod) { printf("  LoadLibrary failed (%lu) for %ls\n", GetLastError(), path.c_str()); return false; }
    s.init = (InitFn)GetProcAddress(s.mod, "EOS_Initialize");
    s.ver = (VerFn)GetProcAddress(s.mod, "EOS_GetVersion");
    s.create = (CreateFn)GetProcAddress(s.mod, "EOS_Platform_Create");
    s.tick = (TickFn)GetProcAddress(s.mod, "EOS_Platform_Tick");
    s.release = (ReleaseFn)GetProcAddress(s.mod, "EOS_Platform_Release");
    s.shutdown = (ShutdownFn)GetProcAddress(s.mod, "EOS_Shutdown");
    return s.init && s.ver && s.create && s.tick && s.release && s.shutdown;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { printf("usage: omp_eosdual <game's EOS dll> <our EOS dll>\n"); return 2; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"omp_eosdual\\";
    const std::wstring gameDir = root + L"Win64\\", oursDir = gameDir + L"Mods\\SessionOpenMP\\dlls\\";
    CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW(gameDir.c_str(), nullptr);
    CreateDirectoryW((gameDir + L"Mods\\").c_str(), nullptr); CreateDirectoryW((gameDir + L"Mods\\SessionOpenMP\\").c_str(), nullptr);
    CreateDirectoryW(oursDir.c_str(), nullptr);
    const std::wstring gamePath = gameDir + L"EOSSDK-Win64-Shipping.dll", oursPath = oursDir + L"OMP_EOSSDK-Win64-Shipping.dll";
    if (!CopyFileW(argv[1], gamePath.c_str(), FALSE) || !CopyFileW(argv[2], oursPath.c_str(), FALSE)) { printf("could not stage the two DLLs (%lu)\n", GetLastError()); return 2; }

    printf("the GAME's SDK, loaded as the game loads it:\n");
    Sdk game{}, ours{};
    Check(Bind(game, gamePath), "loads, and has the calls");
    if (!game.mod) return 1;
    EOS_InitializeOptions gi{}; gi.ApiVersion = EOS_INITIALIZE_API_LATEST; gi.ProductName = "Session"; gi.ProductVersion = "1.0";
    EOS_EResult r = game.init(&gi);
    char buf[160]; snprintf(buf, sizeof(buf), "(result %d, version %s)", (int)r, game.ver());
    Check(r == EOS_EResult::EOS_Success, "EOS_Initialize succeeds", buf);

    printf("OURS, under another name, beside it:\n");
    Check(Bind(ours, oursPath), "loads, and has the calls");
    if (!ours.mod) return 1;
    Check(ours.mod != game.mod, "it is a DIFFERENT module");
    EOS_InitializeOptions oi{}; oi.ApiVersion = EOS_INITIALIZE_API_LATEST; oi.ProductName = "SessionOpenMP"; oi.ProductVersion = "0.1";
    r = ours.init(&oi);
    snprintf(buf, sizeof(buf), "(result %d = %s; version %s)", (int)r, r == EOS_EResult::EOS_AlreadyConfigured ? "ALREADY CONFIGURED: one SDK, shared" : "its own", ours.ver());
    Check(r == EOS_EResult::EOS_Success, "EOS_Initialize answers SUCCESS: a state of its own, not the game's", buf);
    Check(strcmp(game.ver(), ours.ver()) != 0, "each reports its own version");
    Check(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll") == game.mod, "the name EOSSDK-Win64-Shipping.dll still means the GAME's module");

    printf("a platform on ours, as the mod makes it:\n");
    EOS_Platform_Options po{};
    po.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    po.ProductId = OMP_PRODUCT_ID; po.SandboxId = OMP_SANDBOX_ID; po.DeploymentId = OMP_DEPLOYMENT_ID;
    po.ClientCredentials.ClientId = OMP_CLIENT_ID; po.ClientCredentials.ClientSecret = OMP_CLIENT_SECRET;
    po.Flags = EOS_PF_DISABLE_OVERLAY;
    const bool haveCreds = strstr(OMP_PRODUCT_ID, "PUT_YOUR_") == nullptr;
    EOS_HPlatform plat = haveCreds ? ours.create(&po) : nullptr;
    if (!haveCreds) printf("  (no credentials in this tree: platform creation skipped)\n");
    else Check(plat != nullptr, "EOS_Platform_Create gives a platform");
    // ...and one on the game's, on options as old as the game's own SDK understands (the structs only ever
    // grow at the end, so an older ApiVersion reads a prefix of this one). Dummy ids: nothing is contacted.
    EOS_HPlatform gplat = nullptr;
    for (int api = EOS_PLATFORM_OPTIONS_API_LATEST; api >= 8 && !gplat; api--) {
        EOS_Platform_Options go{};
        go.ApiVersion = api;
        go.ProductId = "00000000000000000000000000000000"; go.SandboxId = "00000000000000000000000000000000"; go.DeploymentId = "00000000000000000000000000000000";
        go.ClientCredentials.ClientId = "xyza0000000000000000000000000000"; go.ClientCredentials.ClientSecret = "0000000000000000000000000000000000000000000";
        go.Flags = EOS_PF_DISABLE_OVERLAY;
        gplat = game.create(&go);
        if (gplat) printf("  (the game's SDK took platform options of ApiVersion %d)\n", api);
    }
    Check(gplat != nullptr, "the GAME's SDK still gives a platform with ours alive next to it");
    for (int i = 0; i < 60; i++) { if (plat) ours.tick(plat); if (gplat) game.tick(gplat); Sleep(5); }
    Check(true, "both ticked, side by side, for a third of a second");
    if (plat) ours.release(plat);
    if (gplat) game.release(gplat);
    Check(ours.shutdown() == EOS_EResult::EOS_Success, "ours shuts down");
    Check(game.shutdown() == EOS_EResult::EOS_Success, "...and the game's, untouched by that, shuts down too");

    printf(g_fail ? "EOS DUAL FAIL (%d)\n" : "EOS DUAL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
