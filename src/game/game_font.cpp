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
// SessionOpenMP -- the game's own typeface, read out of a UFontFace. Rationale: game_font.h.
//
// This translation unit is built ONLY into the loader target, not into the omp_game static lib:
// it needs UE4SS's exported FindAllOf, and the static libs deliberately link nothing.
#include "game_font.h"
#include "game_syms.h"
#include "../../third_party/ue4ss_abi/ue4ss_abi.h"
#include "../ui/theme.h"
#include <windows.h>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {

bool  g_done   = false;     // a font was handed over; stop looking
bool  g_logged = false;     // the candidate dump has been written once

// UFontFace layout, from the shipped PDB (pdbmembers.py UFontFace / FFontFaceData):
//   UFontFace     +0x30 SourceFilename FString
//                 +0x41 LoadingPolicy  EFontLoadingPolicy   (0 = Inline: the bytes are in memory)
//                 +0x48 FontFaceData   TSharedRef<FFontFaceData>  { FFontFaceData*; FSharedReferencer; }
//   FFontFaceData +0x00 Data           TArray<uint8>        { uint8* ; int32 Num; int32 Max }
constexpr int kFaceLoadingPolicy = 0x41;
constexpr int kFaceDataRef       = 0x48;

struct FaceBytes { const uint8_t* data; int size; };

// Both of these exist ONLY so the SEH lives in a frame with nothing to unwind: MSVC refuses __try in a
// function holding an object with a destructor (C2712), and GameFont_Grab holds the vector.
bool findFacesSeh(std::vector<RC::Unreal::UObject*>& out) {
    __try { RC::Unreal::UObjectGlobals::FindAllOf(L"FontFace", out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool copySeh(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Diagnostics from the last readFace, so a refusal says WHICH check refused it: "no usable bytes" on
// its own cannot tell an empty array from a wrong offset. (Every face in the shipped build reports
// LoadingPolicy=Inline and still carries an empty array.)
struct FaceProbe { const void* shared; int32_t num; int32_t max; uint32_t tag; };
FaceProbe g_probe{};

bool readFace(const void* face, FaceBytes* out) {
    out->data = nullptr; out->size = 0;
    g_probe = FaceProbe{};
    if (!face) return false;
    __try {
        void* shared = *(void**)((const uint8_t*)face + kFaceDataRef);
        g_probe.shared = shared;
        if (!shared) return false;
        const uint8_t* arr = (const uint8_t*)shared;              // FFontFaceData::Data at +0
        const uint8_t* ptr = *(const uint8_t**)arr;
        const int32_t  num = *(const int32_t*)(arr + 8);
        g_probe.num = num;
        g_probe.max = *(const int32_t*)(arr + 12);
        if (ptr && num >= 4) g_probe.tag = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16)
                                         | ((uint32_t)ptr[2] << 8) | (uint32_t)ptr[3];
        // A font file is tens of KB to a few MB. Anything outside that is a layout mismatch, not a
        // small font -- refuse it rather than hand a wild pointer to the atlas builder.
        if (!ptr || num < 4096 || num > 32 * 1024 * 1024) return false;
        // Sniff the container so a wrong offset cannot silently reach ImGui. TTF is 0x00010000 or
        // "true"/"ttcf", OTF is "OTTO", WOFF would be "wOFF" (which ImGui cannot read anyway).
        const uint32_t tag = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16)
                           | ((uint32_t)ptr[2] << 8) | (uint32_t)ptr[3];
        if (tag != 0x00010000u && tag != 0x74727565u /*true*/ &&
            tag != 0x4F54544Fu /*OTTO*/ && tag != 0x74746366u /*ttcf*/) return false;
        out->data = ptr; out->size = num;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Which face to draw chat in, when the build has several. Ranked by name: the menu font is the one
// worth matching, and Session's UI faces are the ones carrying these words. A face that scores
// nothing is still usable -- it is just the last resort.
int score(const char* name) {
    if (!name || !*name) return 0;
    char lower[96]; strncpy_s(lower, name, _TRUNCATE);
    for (char* p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    struct Pref { const char* needle; int pts; };
    static const Pref kPrefs[] = {
        { "menu",     50 }, { "ui",        40 }, { "hud",     35 },
        { "session",  30 }, { "regular",   20 }, { "medium",  18 },
        { "book",     14 }, { "roboto",    10 }, { "bold",     4 },
    };
    int s = 1;
    for (const Pref& p : kPrefs) if (strstr(lower, p.needle)) s += p.pts;
    // Italics and very heavy weights read badly at chat size; prefer anything else.
    if (strstr(lower, "italic") || strstr(lower, "oblique")) s -= 25;
    if (strstr(lower, "black")  || strstr(lower, "heavy"))   s -= 10;
    return s;
}

// ---- the SHIPPED font ---------------------------------------------------------------------------
// Preferred over anything read out of the game, for two reasons: the game's own faces do not hand
// their bytes over (see the probe line in the log), and a file the player can SEE is a file they can
// swap. Ships in Mods/SessionOpenMP/fonts/ next to its OFL licence.
bool fontsDir(char* out, int cap) {
    // Locate this DLL rather than the game: main.dll lives in Mods/SessionOpenMP/dlls, so the fonts
    // folder is one level up. Using the exe's path instead breaks the moment a user installs the mod
    // somewhere unusual.
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&fontsDir, &self) || !self) return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(self, path, sizeof(path))) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *slash = 0;                                   // ...\Mods\SessionOpenMP\dlls
    slash = strrchr(path, '\\');
    if (!slash) return false;
    *slash = 0;                                   // ...\Mods\SessionOpenMP
    snprintf(out, (size_t)cap, "%s\\fonts", path);
    return true;
}

// Read a whole file into a malloc'd buffer. Bounded: a "font" outside this range is not one, and the
// bytes are about to be handed to a parser.
uint8_t* readWholeFile(const char* path, int* sizeOut) {
    *sizeOut = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 4096 || n > 32 * 1024 * 1024) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return nullptr; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return nullptr; }
    *sizeOut = (int)n;
    return buf;
}

// The shipped default; a player who drops their own .ttf in the folder gets it picked up by the
// wildcard below, so there is no setting to get wrong.
const char* const kFontFileDefault = "Almarai-Regular.ttf";

bool tryFontFile(void (*logf)(const char*)) {
    char dir[MAX_PATH] = {};
    if (!fontsDir(dir, sizeof(dir))) return false;

    char path[MAX_PATH] = {};
    char chosen[64] = {};
    // The default name first; then ANY .ttf in the folder, so dropping your own font in works without
    // touching a setting.
    snprintf(path, sizeof(path), "%s\\%s", dir, kFontFileDefault);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        char glob[MAX_PATH];
        snprintf(glob, sizeof(glob), "%s\\*.ttf", dir);
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        strncpy_s(chosen, fd.cFileName, _TRUNCATE);
        FindClose(h);
    } else {
        strncpy_s(chosen, kFontFileDefault, _TRUNCATE);
    }

    int size = 0;
    uint8_t* bytes = readWholeFile(path, &size);
    if (!bytes) return false;
    Theme_OfferFont(bytes, size, chosen);          // takes ownership
    if (logf) {
        char m[MAX_PATH + 64];
        snprintf(m, sizeof(m), "[font] chat font '%s' (%d bytes) from %s", chosen, size, dir);
        logf(m);
    }
    return true;
}

} // namespace

bool GameFont_Grab(void (*logf)(const char*)) {
    if (g_done) return true;

    // The shipped file wins. It needs nothing from the game, so it settles on the very first call.
    {
        static bool fileTried = false;
        if (!fileTried) {
            fileTried = true;
            if (tryFontFile(logf)) { g_done = true; return true; }
            if (logf) logf("[font] no font file in Mods\\SessionOpenMP\\fonts -- trying the game's own faces");
        }
    }

    std::vector<RC::Unreal::UObject*> faces;
    if (!findFacesSeh(faces) || faces.empty()) return false;   // fonts not loaded yet: try again later

    const void* best = nullptr;
    char        bestName[96] = {};
    FaceBytes   bestBytes{};
    int         bestScore = -1;

    for (RC::Unreal::UObject* o : faces) {
        if (!o) continue;
        char name[96] = {};
        omp::game::ObjectName(o, name, sizeof(name));
        FaceBytes fb{};
        const bool ok = readFace(o, &fb);
        if (!g_logged && logf) {
            uint8_t policy = 0xff;
            copySeh(&policy, (const uint8_t*)o + kFaceLoadingPolicy, 1);
            char m[240];
            snprintf(m, sizeof(m), "[font] face '%s' policy=%u data=%p num=%d max=%d tag=%08X -> %s",
                     name[0] ? name : "?", policy, g_probe.shared, g_probe.num, g_probe.max,
                     g_probe.tag, ok ? "USABLE" : "no bytes here");
            logf(m);
        }
        if (!ok) continue;
        const int s = score(name);
        if (s > bestScore) { bestScore = s; best = o; bestBytes = fb; strncpy_s(bestName, name, _TRUNCATE); }
    }
    g_logged = true;

    if (!best) {
        if (logf) logf("[font] no font face carries its bytes in memory -- chat keeps the built-in face");
        g_done = true;                                  // do not re-walk GUObjectArray every tick
        return false;
    }

    // COPY the bytes out before handing them across the thread boundary. The atlas is built on the
    // render thread whenever it gets there, and by then the asset could have been unloaded; game
    // memory must never be read from that thread at that time.
    void* copy = malloc((size_t)bestBytes.size);
    if (!copy) { g_done = true; return false; }
    if (!copySeh(copy, bestBytes.data, (size_t)bestBytes.size)) { free(copy); g_done = true; return false; }

    Theme_OfferFont(copy, bestBytes.size, bestName);    // takes ownership
    if (logf) {
        char m[200];
        snprintf(m, sizeof(m), "[font] chat will use '%s' (%d bytes, score %d of %d face(s))",
                 bestName, bestBytes.size, bestScore, (int)faces.size());
        logf(m);
    }
    g_done = true;
    return true;
}
