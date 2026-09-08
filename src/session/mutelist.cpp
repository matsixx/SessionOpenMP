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
// SessionOpenMP -- the voice mute list. The ban list's shape exactly; see mutelist.h.
#include "mutelist.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
struct Entry { char id[80]; char name[40]; };
Entry g_mutes[OMP_MUTE_MAX];
int   g_n = 0;
char  g_path[512] = {0};
void (*g_log)(const char*) = nullptr;
void say(const char* s) { if (g_log) g_log(s); }

void save() {
    if (!g_path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "wb") != 0 || !f) return;
    fputs("; SessionOpenMP -- players whose voice you do not hear. One per line: <productUserId> <name>\n", f);
    for (int i = 0; i < g_n; i++) fprintf(f, "%s %s\n", g_mutes[i].id, g_mutes[i].name);
    fclose(f);
}
int find(const char* id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < g_n; i++) if (_stricmp(g_mutes[i].id, id) == 0) return i;
    return -1;
}
} // namespace

bool Mute_Is(const char* peerId) { return find(peerId) >= 0; }
int  Mute_Count() { return g_n; }
bool Mute_At(int i, char* idOut, int idCap, char* nameOut, int nameCap) {
    if (i < 0 || i >= g_n) return false;
    if (idOut   && idCap   > 0) strncpy_s(idOut,   (size_t)idCap,   g_mutes[i].id,   _TRUNCATE);
    if (nameOut && nameCap > 0) strncpy_s(nameOut, (size_t)nameCap, g_mutes[i].name, _TRUNCATE);
    return true;
}

bool Mute_Add(const char* peerId, const char* name) {
    if (!peerId || !*peerId) return false;
    if (find(peerId) >= 0) return true;
    if (g_n >= OMP_MUTE_MAX) { say("[mute] list full"); return false; }
    strncpy_s(g_mutes[g_n].id,   peerId,                 _TRUNCATE);
    strncpy_s(g_mutes[g_n].name, (name && *name) ? name : "?", _TRUNCATE);
    g_n++;
    save();
    char m[160];
    snprintf(m, sizeof(m), "[mute] muted %s (%s) -- %d muted", peerId, name ? name : "?", g_n);
    say(m);
    return true;
}
bool Mute_Remove(const char* peerId) {
    const int i = find(peerId);
    if (i < 0) return false;
    for (int k = i; k + 1 < g_n; k++) g_mutes[k] = g_mutes[k + 1];
    g_n--;
    save();
    say("[mute] unmuted");
    return true;
}

void Mute_Init(const char* dir, void (*logf)(const char*)) {
    g_log = logf;
    g_n = 0;
    if (!dir || !*dir) return;
    snprintf(g_path, sizeof(g_path), "%sSessionOpenMP_muted.txt", dir);
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "rb") == 0 && f) {
        char line[160];
        while (g_n < OMP_MUTE_MAX && fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            if (!line[0] || line[0] == ';' || line[0] == '#') continue;
            char* sp = strchr(line, ' ');
            if (sp) *sp = 0;
            if (!line[0]) continue;
            strncpy_s(g_mutes[g_n].id,   line, _TRUNCATE);
            strncpy_s(g_mutes[g_n].name, sp ? sp + 1 : "?", _TRUNCATE);
            g_n++;
        }
        fclose(f);
    }
    char m[120];
    snprintf(m, sizeof(m), "[mute] %d player(s) on your mute list", g_n);
    say(m);
}
