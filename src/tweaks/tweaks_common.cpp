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
// SessionTweaks -- shared plumbing implementation. See tweaks_common.h for the rules.
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include <cctype>

static bool sigByte(const char* tok, uint8_t* out) {
    if (tok[0] == '?') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int hi = hex(tok[0]), lo = hex(tok[1]);
    if (hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}
static uint8_t* scanRange(uint8_t* base, size_t len, const char* sig) {
    uint8_t bytes[64]; bool wild[64]; int n = 0;
    for (const char* p = sig; *p && n < 64; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        wild[n] = !sigByte(p, &bytes[n]); n++;
        while (*p && *p != ' ') p++;
    }
    if (!n) return nullptr;
    for (size_t i = 0; i + n <= len; i++) {
        int j = 0;
        for (; j < n; j++) if (!wild[j] && base[i + j] != bytes[j]) break;
        if (j == n) return base + i;
    }
    return nullptr;
}
uint8_t* TwkScanExe(const char* sig) {
    HMODULE hExe = GetModuleHandleA(nullptr);
    if (!hExe) return nullptr;
    uint8_t* base = (uint8_t*)hExe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (uint8_t* hit = scanRange(base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, sig)) return hit;
    }
    return nullptr;
}

int TwkIniSetInt(char* text, size_t cap, const char* key, int value) {
    char val[32];
    snprintf(val, sizeof(val), "%d", value);
    return TwkIniSetStr(text, cap, key, val);
}

int TwkIniSetStr(char* text, size_t cap, const char* key, const char* val) {
    const size_t klen = strlen(key), vlen = strlen(val);
    for (char* line = text; *line; ) {
        char* eol = strpbrk(line, "\r\n");
        char* end = eol ? eol : line + strlen(line);
        char* p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p != ';' && *p != '#' && *p != '[' &&
            (size_t)(end - p) > klen && _strnicmp(p, key, klen) == 0) {
            char* q = p + klen;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;                                   // value spans q..end; splice the new one in
                const size_t tail = strlen(end) + 1;   // rest of file incl. terminator
                if (strlen(text) - (size_t)(end - q) + vlen + 1 > cap) return 0;
                memmove(q + vlen, end, tail);
                memcpy(q, val, vlen);
                return 1;
            }
        }
        line = eol ? eol + 1 : end;
    }
    const size_t len = strlen(text);
    if (len + klen + vlen + 4 > cap) return 0;         // key not present: append a fresh line
    snprintf(text + len, cap - len, "%s%s=%s\n", (len && text[len - 1] != '\n') ? "\n" : "", key, val);
    return 1;
}

void TwkIniStr(const char* text, const char* key, char* out, size_t cap, const char* def) {
    snprintf(out, cap, "%s", def ? def : "");
    const size_t klen = strlen(key);
    for (const char* line = text; *line; ) {
        const char* eol = strpbrk(line, "\r\n");
        const char* end = eol ? eol : line + strlen(line);
        const char* p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p != ';' && *p != '#' && *p != '[' &&
            (size_t)(end - p) > klen && _strnicmp(p, key, klen) == 0) {
            const char* q = p + klen;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                while (end > q && (end[-1] == ' ' || end[-1] == '\t')) end--;
                size_t n = (size_t)(end - q);
                if (n >= cap) n = cap - 1;
                memcpy(out, q, n); out[n] = 0;
                return;
            }
        }
        line = eol ? eol + 1 : end;
    }
}

int TwkIniInt(const char* text, const char* key, int def) {
    const size_t klen = strlen(key);
    for (const char* line = text; *line; ) {
        const char* eol = strpbrk(line, "\r\n");
        const char* end = eol ? eol : line + strlen(line);
        const char* p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p != ';' && *p != '#' && *p != '[' &&
            (size_t)(end - p) > klen && _strnicmp(p, key, klen) == 0) {
            const char* q = p + klen;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                return atoi(q);
            }
        }
        line = eol ? eol + 1 : end;
    }
    return def;
}
