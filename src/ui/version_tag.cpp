// SessionOpenMP -- co-op multiplayer for Session, as an overlay on N solo games.
// Copyright (C) 2026 matsix
//
// This program is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version. It is
// distributed WITHOUT ANY WARRANTY; see the GNU GPL (LICENSE) for details.
//
// Additional permission under GNU GPL version 3 section 7: you may link and convey this
// work combined with the Epic Online Services SDK and the proprietary game runtime it
// loads into. See LICENSE-EXCEPTION.txt.
// SessionOpenMP -- the build line reads "<game version>  |  OpenMP <mod version>". Why the hook is
// filtered by caller rather than applied to every call: version_tag.h.
#include "version_tag.h"
#include "overlay.h"
#include "../game/game_syms.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include "MinHook.h"
#endif

using namespace omp::game;

static bool g_dead = false, g_installed = false;
static uint8_t* g_base = nullptr;

// The game's FString: a TArray<TCHAR> whose Num INCLUDES the null terminator.
struct FStr { wchar_t* d; int32_t n; int32_t max; };

static GameVersionFn o_GameVersion = nullptr;

// Is this return address inside one of the two functions that DISPLAY the version? Anything else --
// notably UPlayerProfile::GetNewsSaveData -- gets the untouched string.
static bool callerDisplays(const void* ret) {
    const Syms& S = Get();
    const uint8_t* r = (const uint8_t*)ret;
    if (S.IntroUiRange   && r >= (const uint8_t*)S.IntroUiRange   && r < (const uint8_t*)S.IntroUiRange   + off::kIntroUiLen)   return true;
    if (S.PauseInitRange && r >= (const uint8_t*)S.PauseInitRange && r < (const uint8_t*)S.PauseInitRange + off::kPauseInitLen) return true;
    return false;
}

// Append the tag onto the FString the game just built. Done by hand rather than with FString::Printf
// because Printf's body has a byte-identical twin in the exe and cannot be signatured; building the
// buffer here with the engine's OWN allocator is both simpler and free of that ambiguity.
// The old buffer is freed, so this adds no leak.
static void appendTag(void* outFString) {
    const Syms& S = Get();
    if (!S.MemMalloc || !S.MemFree || !outFString) return;
    __try {
        FStr* s = (FStr*)outFString;
        if (!s->d || s->n <= 1) return;                       // empty: nothing to append to
        static const wchar_t kTag[] = L"  |  OpenMP " OMP_VERSION_WIDE;
        const int tagLen = (int)(sizeof(kTag) / sizeof(wchar_t)) - 1;   // chars, excluding the null
        const int newN   = s->n + tagLen;                     // s->n already counts the null
        wchar_t* buf = (wchar_t*)S.MemMalloc((size_t)newN * sizeof(wchar_t), 0);
        if (!buf) return;
        memcpy(buf, s->d, (size_t)(s->n - 1) * sizeof(wchar_t));
        memcpy(buf + (s->n - 1), kTag, (size_t)(tagLen + 1) * sizeof(wchar_t));
        S.MemFree(s->d);
        s->d = buf; s->n = newN; s->max = newN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_dead) { g_dead = true; OvLog("[ver] *** faulted tagging the version string -- tag OFF"); }
    }
}

// THE GAME INSTANCE, and "we are on a menu", both for free.
//
// Everything else in the mod reaches the game instance through an ACTOR (AActor::GetGameInstance),
// which is fine in a level and useless at the main menu, where there is no pawn. But this hook is a
// METHOD ON THE GAME INSTANCE -- its `this` IS the pointer, handed over on a plate, at a moment the
// menu is on screen. Nothing else has to be resolved, hooked or guessed.
//
// The caller filter does double duty too: a call from the intro UI means the MAIN MENU is drawing
// its version line, which is exactly when a "you are out of date" popup should appear and exactly
// when it is safe to show one.
static void* g_gameInstance = nullptr;
static volatile LONG g_onMenu = 0;
void* VersionTag_GameInstance() { return g_gameInstance; }
bool  VersionTag_SawMenu()      { return InterlockedExchange(&g_onMenu, 0) != 0; }

static void* hkGameVersion(void* gameInstance, void* outFString) {
    void* r = o_GameVersion(gameInstance, outFString);
    if (gameInstance) g_gameInstance = gameInstance;
    // _ReturnAddress() here is the address in the ORIGINAL caller: MinHook's trampoline leaves the
    // real return address in place, which is what makes the caller filter possible at all.
    const bool fromMenu = callerDisplays(_ReturnAddress());
    if (fromMenu) InterlockedExchange(&g_onMenu, 1);
    if (!g_dead && fromMenu) appendTag(outFString);
    return r;
}

void VersionTag_Install() {
    if (g_installed) return;
    g_installed = true;
    const Syms& S = Get();
    g_base = (uint8_t*)GetModuleHandleA(nullptr);
    // Every dependency named, so "no tag" says which symbol is missing. All optional: a missing one
    // costs a cosmetic string, never correctness.
    struct Dep { const char* name; const void* p; };
    const Dep deps[] = {
        { "GameVersion",    (const void*)S.GameVersion },
        { "IntroUiRange",   (const void*)S.IntroUiRange },
        { "PauseInitRange", (const void*)S.PauseInitRange },
        { "MemMalloc",      (const void*)S.MemMalloc },
        { "MemFree",        (const void*)S.MemFree },
    };
    char missing[200] = {0};
    for (const Dep& d : deps) {
        if (d.p) continue;
        if (missing[0]) strncat_s(missing, ", ", _TRUNCATE);
        strncat_s(missing, d.name, _TRUNCATE);
    }
    if (missing[0]) {
        char m[280];
        snprintf(m, sizeof(m), "[ver] version tag OFF -- unresolved: %s", missing);
        OvLog(m);
        g_dead = true;
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { g_dead = true; return; }
    if (MH_CreateHook(S.GameVersion, (void*)&hkGameVersion, (void**)&o_GameVersion) == MH_OK &&
        MH_EnableHook(S.GameVersion) == MH_OK)
        OvLog("[ver] version tag armed -- the game's build line now ends with \"| OpenMP " OMP_VERSION_STRING "\"");
    else {
        g_dead = true;
        OvLog("[ver] *** version tag hook failed");
    }
}
