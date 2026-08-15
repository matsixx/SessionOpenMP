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

// Floating names: on while you are off your board, which is when you are looking around rather than
// skating. Bubble range is far shorter than name range because a sentence needs much more screen than
// a name does, and one you cannot read is just clutter.
static int      g_nameMode    = MPNAME_OFFBOARD;
static int      g_nameDistM   = 120;
static int      g_bubbleDistM = 35;
// Dropped objects: SHARED by default. One canonical set is the only arrangement in which everyone is
// looking at the same spot, and it takes nothing away permanently -- your own props are hidden for
// the session and come straight back.
static int      g_dropMode    = MPDROP_SHARED;
// OFF by default: sharing the level's own furniture is unfinished, and the setting exists so it
// cannot take the working half down with it.

static void say(const char* s) { if (g_log) g_log(s); }

static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void saveAll() {
    if (!g_path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "wb") != 0 || !f) {
        say("[prefs] *** could not write the preferences file -- this change lasts for this run only");
        return;
    }
    fprintf(f, "# SessionOpenMP preferences. Delete a line to return it to its default.\n");
    fprintf(f, "HideAddress=%d\n", g_hideAddress ? 1 : 0);
    fprintf(f, "# Player names above heads: 0 off, 1 only while off your board, 2 always.\n");
    fprintf(f, "NameMode=%d\n", g_nameMode);
    fprintf(f, "NameDistM=%d\n", g_nameDistM);
    fprintf(f, "BubbleDistM=%d\n", g_bubbleDistM);
    fprintf(f, "# Dropped objects: 0 off, 1 only what is placed during the session, 2 share one set.\n");
    fprintf(f, "DropMode=%d\n", g_dropMode);
    fprintf(f, "# The level's own props (benches, barriers): 0 leave them alone, 1 share them.\n");
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

// The nameplate settings. No generation bump: nothing about them reaches the transport, and the
// game-thread publish reads them straight out of here every frame. Each one clamps, so a hand-edited
// or corrupt file can never produce a slider position the menu could not have produced.
int  MpPrefs_NameMode()    { return g_nameMode; }
int  MpPrefs_DropMode()    { return g_dropMode; }
void MpPrefs_SetDropMode(int mode) {
    mode = clampI(mode, MPDROP_OFF, MPDROP_SHARED);
    if (mode == g_dropMode) return;
    g_dropMode = mode;
    saveAll();
    char m[140];
    snprintf(m, sizeof(m), "[prefs] dropped objects: %s",
             (mode == MPDROP_OFF) ? "off" : (mode == MPDROP_LIVE) ? "live edits only"
                                                                  : "share one set");
    say(m);
}
int  MpPrefs_NameDistM()   { return g_nameDistM; }
int  MpPrefs_BubbleDistM() { return g_bubbleDistM; }

void MpPrefs_SetNameMode(int mode) {
    mode = clampI(mode, MPNAME_OFF, MPNAME_ALWAYS);
    if (mode == g_nameMode) return;                 // a no-op write must not churn the file
    g_nameMode = mode;
    saveAll();
    char m[120];
    snprintf(m, sizeof(m), "[prefs] player names: %s",
             (mode == MPNAME_OFF) ? "off" : (mode == MPNAME_ALWAYS) ? "always" : "off board only");
    say(m);
}
void MpPrefs_SetNameDistM(int metres) {
    metres = clampI(metres, MPNAME_DIST_MIN, MPNAME_DIST_MAX);
    if (metres == g_nameDistM) return;
    g_nameDistM = metres;
    saveAll();
    char m[96]; snprintf(m, sizeof(m), "[prefs] name distance: %d m", metres); say(m);
}
void MpPrefs_SetBubbleDistM(int metres) {
    metres = clampI(metres, MPBUBBLE_DIST_MIN, MPBUBBLE_DIST_MAX);
    if (metres == g_bubbleDistM) return;
    g_bubbleDistM = metres;
    saveAll();
    char m[96]; snprintf(m, sizeof(m), "[prefs] bubble distance: %d m", metres); say(m);
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
            else if (!_stricmp(key, "NameMode"))    g_nameMode    = clampI(atoi(val), MPNAME_OFF, MPNAME_ALWAYS);
            else if (!_stricmp(key, "NameDistM"))   g_nameDistM   = clampI(atoi(val), MPNAME_DIST_MIN, MPNAME_DIST_MAX);
            else if (!_stricmp(key, "BubbleDistM")) g_bubbleDistM = clampI(atoi(val), MPBUBBLE_DIST_MIN, MPBUBBLE_DIST_MAX);
            else if (!_stricmp(key, "DropMode"))    g_dropMode    = clampI(atoi(val), MPDROP_OFF, MPDROP_SHARED);
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
