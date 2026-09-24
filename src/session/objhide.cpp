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
// SessionOpenMP -- the hidden-objects list. The mute list's shape exactly; see objhide.h.
#include "objhide.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
struct Entry { char id[80]; char name[40]; };
Entry g_hidden[OMP_OBJHIDE_MAX];
int   g_n = 0;
char  g_path[512] = {0};
void (*g_log)(const char*) = nullptr;
void say(const char* s) { if (g_log) g_log(s); }

void save() {
    if (!g_path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "wb") != 0 || !f) return;
    fputs("; SessionOpenMP -- players whose dropped objects you do not see. One per line: <productUserId> <name>\n", f);
    for (int i = 0; i < g_n; i++) fprintf(f, "%s %s\n", g_hidden[i].id, g_hidden[i].name);
    fclose(f);
}
int find(const char* id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < g_n; i++) if (_stricmp(g_hidden[i].id, id) == 0) return i;
    return -1;
}
} // namespace

bool ObjHide_Is(const char* peerId) { return find(peerId) >= 0; }
int  ObjHide_Count() { return g_n; }

bool ObjHide_Add(const char* peerId, const char* name) {
    if (!peerId || !*peerId) return false;
    if (find(peerId) >= 0) return true;
    if (g_n >= OMP_OBJHIDE_MAX) { say("[objects] hidden list full"); return false; }
    strncpy_s(g_hidden[g_n].id,   peerId,                       _TRUNCATE);
    strncpy_s(g_hidden[g_n].name, (name && *name) ? name : "?", _TRUNCATE);
    g_n++;
    save();
    char m[160];
    snprintf(m, sizeof(m), "[objects] hiding what %s (%s) drops -- %d hidden", name ? name : "?", peerId, g_n);
    say(m);
    return true;
}
bool ObjHide_Remove(const char* peerId) {
    const int i = find(peerId);
    if (i < 0) return false;
    for (int k = i; k + 1 < g_n; k++) g_hidden[k] = g_hidden[k + 1];
    g_n--;
    save();
    say("[objects] showing their objects again");
    return true;
}

void ObjHide_Init(const char* dir, void (*logf)(const char*)) {
    g_log = logf;
    g_n = 0;
    if (!dir || !*dir) return;
    snprintf(g_path, sizeof(g_path), "%sSessionOpenMP_hidden_objects.txt", dir);
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "rb") == 0 && f) {
        char line[160];
        while (g_n < OMP_OBJHIDE_MAX && fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            if (!line[0] || line[0] == ';' || line[0] == '#') continue;
            char* sp = strchr(line, ' ');
            if (sp) *sp = 0;
            if (!line[0]) continue;
            strncpy_s(g_hidden[g_n].id,   line,              _TRUNCATE);
            strncpy_s(g_hidden[g_n].name, sp ? sp + 1 : "?", _TRUNCATE);
            g_n++;
        }
        fclose(f);
    }
    if (g_n) {
        char m[120];
        snprintf(m, sizeof(m), "[objects] %d player(s) whose objects you have hidden", g_n);
        say(m);
    }
}
