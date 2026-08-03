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
#include "mp_prefs.h"

#define _CRT_RAND_S                      // rand_s: seeded from the OS, not from the clock
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <intrin.h>                      // __rdtsc, only as the never-return-zero fallback

static void (*g_log)(const char*) = nullptr;
static char  g_path[512] = {0};

// The default is the SAFE one. A player who has never opened the menu gets their address hidden;
// turning it off is a deliberate act by someone who wants the latency back.
static bool     g_hideAddress = true;
static unsigned g_gen = 0;
static char     g_peerId[33] = {0};      // 32 hex chars + terminator; empty until Init

static void say(const char* s) { if (g_log) g_log(s); }

static void saveAll() {
    if (!g_path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "wb") != 0 || !f) {
        say("[prefs] *** could not write the preferences file -- this change lasts for this run only");
        return;
    }
    fprintf(f, "# SessionOpenMP preferences. Delete a line to return it to its default.\n");
    fprintf(f, "HideAddress=%d\n", g_hideAddress ? 1 : 0);
    // PeerId is an IDENTITY, not a preference: deleting the line makes this install a different
    // person to everyone who has played with it. Written last, with a warning above it.
    fprintf(f, "# PeerId identifies this install to peers on non-EOS transports. Deleting it is\n"
               "# harmless but you will appear as a new player.\n");
    fprintf(f, "PeerId=%s\n", g_peerId);
    fclose(f);
}

// 128 random bits as lowercase hex. `rand_s` is seeded by the OS (RtlGenRandom underneath), not by
// the clock -- two copies of the game launched in the same second must not agree on an identity.
static void makePeerId(char out[33]) {
    static const char* kHex = "0123456789abcdef";
    int k = 0;
    for (int w = 0; w < 4; w++) {
        unsigned v = 0;
        if (rand_s(&v) != 0) v = (unsigned)(uintptr_t)&out[w] ^ (unsigned)__rdtsc();  // never leave it 0
        for (int n = 0; n < 8; n++) out[k++] = kHex[(v >> (28 - n * 4)) & 0xf];
    }
    out[32] = 0;
}

const char* MpPrefs_PeerId() { return g_peerId; }

bool     MpPrefs_HideAddress() { return g_hideAddress; }
unsigned MpPrefs_Generation()  { return g_gen; }

void MpPrefs_SetHideAddress(bool on) {
    if (g_hideAddress == on) return;              // a no-op write must not churn the file or the gen
    g_hideAddress = on;
    g_gen++;
    saveAll();
    char m[160];
    snprintf(m, sizeof(m), "[prefs] hide my address: %s%s", on ? "ON" : "OFF",
             on ? "" : " -- peers may connect directly and see your IP");
    say(m);
}

void MpPrefs_Init(const char* dir, void (*logf)(const char*)) {
    g_log = logf;
    if (!dir || !*dir) return;
    snprintf(g_path, sizeof(g_path), "%sSessionOpenMP_prefs.txt", dir);

    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "rb") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (line[0] == '#' || !line[0]) continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* key = line;
            const char* val = eq + 1;
            // A missing or unparseable key keeps the compiled default rather than falling to zero --
            // for this preference zero is the LESS safe value, so "corrupt file" must not silently
            // mean "stop hiding my address".
            if (!_stricmp(key, "HideAddress")) g_hideAddress = (val[0] != '0');
            else if (!_stricmp(key, "PeerId")) {
                // Only accept a well-formed one. A truncated or hand-edited id would still "work"
                // right up until it collided with somebody, which is the worst time to find out.
                const size_t n = strlen(val);
                bool ok = (n == 32);
                for (size_t i = 0; ok && i < n; i++)
                    ok = (val[i] >= '0' && val[i] <= '9') || (val[i] >= 'a' && val[i] <= 'f');
                if (ok) strncpy_s(g_peerId, val, _TRUNCATE);
            }
        }
        fclose(f);
    }
    if (!g_peerId[0]) {                            // first run, or a stored identity that failed the check
        makePeerId(g_peerId);
        saveAll();
    }
    char m[192];
    snprintf(m, sizeof(m), "[prefs] hide my address: %s | peer id %s",
             g_hideAddress ? "ON" : "OFF", g_peerId);
    say(m);
}
