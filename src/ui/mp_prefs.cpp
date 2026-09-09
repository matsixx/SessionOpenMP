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
static int   g_syncSeconds = MPSYNC_SEC_DEFAULT;

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
// OFF by default. This is the most expensive thing the mod can ask of a CPU -- every observer
// simulates every peer, so a lobby of N pays N*N -- and it is a look, not a feature anyone needs.
static int      g_peerBody    = MPBODY_OFF;
// A DEFAULT ONLY REACHES A FRESH INSTALL. Everyone already running has PeerBodyPhysics saved as ON
// from when it shipped that way, and would keep paying for it forever. This stamp is written once;
// a prefs file without it gets the setting forced OFF exactly one time, after which the player's own
// choice is theirs again. Turning it back on and restarting keeps it on.
static bool     g_peerBodyOffDone = false;
static int      g_voiceMode   = MPVOICE_PTT;
static int      g_voiceKey    = 0;
static int      g_voiceRange  = 25;
static int      g_voiceVol    = 100;
static int      g_voiceSens   = 50;
static char     g_voiceDevice[200] = {0};
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
    fprintf(f, "SyncSeconds=%d\n", g_syncSeconds);
    fprintf(f, "NameDistM=%d\n", g_nameDistM);
    fprintf(f, "BubbleDistM=%d\n", g_bubbleDistM);
    fprintf(f, "# Dropped objects: 0 off, 1 only what is placed during the session, 2 share one set.\n");
    fprintf(f, "DropMode=%d\n", g_dropMode);
    fprintf(f, "# Other players' body physics on your screen: 0 off, 1 light, 2 full. VERY heavy --\n");
    fprintf(f, "# your machine simulates a whole body for every other player in the session.\n");
    fprintf(f, "PeerBodyPhysics=%d\n", g_peerBody);
    fprintf(f, "PeerBodyDefaultedOff=%d\n", g_peerBodyOffDone ? 1 : 0);
    fprintf(f, "# Voice chat: 0 off, 1 push to talk, 2 open mic. Key: 0 V, 1 B, 2 T, 3 Left Alt, 4 Left Ctrl, 5 Mouse 4, 6 Mouse 5.\n");
    fprintf(f, "VoiceMode=%d\n", g_voiceMode);
    fprintf(f, "VoiceKey=%d\n", g_voiceKey);
    fprintf(f, "VoiceRangeM=%d\n", g_voiceRange);
    fprintf(f, "VoiceVolume=%d\n", g_voiceVol);
    fprintf(f, "VoiceSensitivity=%d\n", g_voiceSens);
    fprintf(f, "# The microphone: a Windows endpoint id from the Voice chat page; empty = the default device.\n");
    fprintf(f, "VoiceDevice=%s\n", g_voiceDevice);
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
int  MpPrefs_PeerBodyPhysics() { return g_peerBody; }
void MpPrefs_SetPeerBodyPhysics(int on) {
    on = clampI(on, MPBODY_OFF, MPBODY_FULL);
    if (on == g_peerBody) return;
    g_peerBody = on;
    saveAll();
    say(on ? "[prefs] peer body physics: on" : "[prefs] peer body physics: off");
}
int  MpPrefs_VoiceMode()        { return g_voiceMode; }
void MpPrefs_SetVoiceMode(int mode) {
    mode = clampI(mode, MPVOICE_OFF, MPVOICE_OPEN);
    if (mode == g_voiceMode) return;
    g_voiceMode = mode;
    saveAll();
    say(mode == MPVOICE_OFF ? "[prefs] voice chat: off" : mode == MPVOICE_PTT ? "[prefs] voice chat: push to talk" : "[prefs] voice chat: open mic");
}
int  MpPrefs_VoiceKey()         { return g_voiceKey; }
void MpPrefs_SetVoiceKey(int idx) {
    idx = clampI(idx, 0, MPVOICE_KEY_COUNT - 1);
    if (idx == g_voiceKey) return;
    g_voiceKey = idx;
    saveAll();
}
int  MpPrefs_VoiceRangeM()      { return g_voiceRange; }
void MpPrefs_SetVoiceRangeM(int metres) {
    metres = clampI(metres, MPVOICE_RANGE_MIN, MPVOICE_RANGE_MAX);
    if (metres == g_voiceRange) return;
    g_voiceRange = metres;
    saveAll();
}
int  MpPrefs_VoiceVolume()      { return g_voiceVol; }
void MpPrefs_SetVoiceVolume(int pct) {
    pct = clampI(pct, MPVOICE_VOL_MIN, MPVOICE_VOL_MAX);
    if (pct == g_voiceVol) return;
    g_voiceVol = pct;
    saveAll();
}
int  MpPrefs_VoiceSensitivity() { return g_voiceSens; }
void MpPrefs_SetVoiceSensitivity(int pct) {
    pct = clampI(pct, 0, 100);
    if (pct == g_voiceSens) return;
    g_voiceSens = pct;
    saveAll();
}
const char* MpPrefs_VoiceDevice() { return g_voiceDevice; }
void MpPrefs_SetVoiceDevice(const char* id) {
    if (!id) id = "";
    if (strcmp(id, g_voiceDevice) == 0) return;
    strncpy_s(g_voiceDevice, id, _TRUNCATE);
    saveAll();
    say(g_voiceDevice[0] ? "[prefs] microphone: chosen device" : "[prefs] microphone: the default device");
}
int  MpPrefs_SyncSeconds() { return g_syncSeconds; }
void MpPrefs_SetSyncSeconds(int seconds) {
    seconds = clampI(seconds, MPSYNC_SEC_MIN, MPSYNC_SEC_MAX);
    if (seconds == g_syncSeconds) return;           // a no-op write must not churn the file
    g_syncSeconds = seconds;
    saveAll();
    char m[120];
    snprintf(m, sizeof(m), "[prefs] synced replay length: %d s", seconds);
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
            else if (!_stricmp(key, "SyncSeconds")) g_syncSeconds = clampI(atoi(val), MPSYNC_SEC_MIN, MPSYNC_SEC_MAX);
            else if (!_stricmp(key, "NameDistM"))   g_nameDistM   = clampI(atoi(val), MPNAME_DIST_MIN, MPNAME_DIST_MAX);
            else if (!_stricmp(key, "BubbleDistM")) g_bubbleDistM = clampI(atoi(val), MPBUBBLE_DIST_MIN, MPBUBBLE_DIST_MAX);
            else if (!_stricmp(key, "DropMode"))    g_dropMode    = clampI(atoi(val), MPDROP_OFF, MPDROP_SHARED);
            else if (!_stricmp(key, "PeerBodyPhysics")) g_peerBody = clampI(atoi(val), MPBODY_OFF, MPBODY_FULL);
            else if (!_stricmp(key, "PeerBodyDefaultedOff")) g_peerBodyOffDone = atoi(val) != 0;
            else if (!_stricmp(key, "VoiceMode"))        g_voiceMode  = clampI(atoi(val), MPVOICE_OFF, MPVOICE_OPEN);
            else if (!_stricmp(key, "VoiceKey"))         g_voiceKey   = clampI(atoi(val), 0, MPVOICE_KEY_COUNT - 1);
            else if (!_stricmp(key, "VoiceRangeM"))      g_voiceRange = clampI(atoi(val), MPVOICE_RANGE_MIN, MPVOICE_RANGE_MAX);
            else if (!_stricmp(key, "VoiceVolume"))      g_voiceVol   = clampI(atoi(val), MPVOICE_VOL_MIN, MPVOICE_VOL_MAX);
            else if (!_stricmp(key, "VoiceSensitivity")) g_voiceSens  = clampI(atoi(val), 0, 100);
            else if (!_stricmp(key, "VoiceDevice"))      strncpy_s(g_voiceDevice, val, _TRUNCATE);
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
    // The one-time forced OFF (see g_peerBodyOffDone). After the whole file is parsed, so no later
    // line can undo it, and saved immediately so it happens exactly once -- the player's own choice
    // is theirs again from here.
    if (!g_peerBodyOffDone) {
        const int was = g_peerBody;
        g_peerBody = MPBODY_OFF;
        g_peerBodyOffDone = true;
        saveAll();
        if (was != MPBODY_OFF)
            say("[prefs] peer body physics turned OFF: it costs a whole body simulation per player. "
                "Multiplayer -> Other options turns it back on.");
    }
    char m[192];
    snprintf(m, sizeof(m), "[prefs] hide my address: %s | peer id %s",
             g_hideAddress ? "ON" : "OFF", g_peerId);
    say(m);
}
