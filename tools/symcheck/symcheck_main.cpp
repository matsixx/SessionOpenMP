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
// omp_symcheck -- verify the whole game-symbol table against BOTH exes, from disk, with no game
// running. Resolving sigs only at runtime turns an ambiguous or build-specific sig into a silently
// dead feature; this makes the table a testable artifact instead. It passes only when every required
// signature matches exactly once per exe, and reports ambiguous matches separately from misses.
// Usage: omp_symcheck [exe ...]   (defaults to the known Epic + Steam install paths)
#include "../../src/game/game_syms.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <windows.h>

using namespace omp::game;

struct Pat { uint8_t b[64]; bool wild[64]; int n = 0; };
static bool parsePat(const char* sig, Pat& p) {
    p.n = 0;
    for (const char* c = sig; *c && p.n < 64; ) {
        if (*c == ' ') { c++; continue; }
        if (c[0] == '?') { p.wild[p.n] = true; p.b[p.n++] = 0; c += (c[1] == '?') ? 2 : 1; continue; }
        unsigned v = 0; int k = 0;
        while (k < 2 && isxdigit((unsigned char)c[k])) {
            const char h = c[k];
            v = v * 16 + (unsigned)((h <= '9') ? h - '0' : ((h | 32) - 'a' + 10)); k++;
        }
        if (!k) return false;
        p.wild[p.n] = false; p.b[p.n++] = (uint8_t)v; c += k;
    }
    return p.n > 0;
}

int main(int argc, char** argv) {
    std::vector<const char*> exes;
    if (argc > 1) for (int i = 1; i < argc; i++) exes.push_back(argv[i]);
    else {
        exes.push_back("C:\\Program Files\\Epic Games\\SessionSkateSim\\SessionGame\\Binaries\\Win64\\SessionGame-Win64-Shipping.exe");
        exes.push_back("F:\\Steam\\steamapps\\common\\Session\\SessionGame\\Binaries\\Win64\\SessionGame-Win64-Shipping.exe");
    }
    printf("omp_symcheck -- %d sigs x %zu exe(s)\n\n", SigCount(), exes.size());
    bool allOk = true;

    for (const char* path : exes) {
        FILE* f = fopen(path, "rb");
        if (!f) { printf("  SKIP (not found): %s\n\n", path); continue; }
        fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> img((size_t)sz);
        fread(img.data(), 1, (size_t)sz, f); fclose(f);
        // executable sections only (a sig can otherwise "match" inside rdata and mislead)
        auto* dos = (IMAGE_DOS_HEADER*)img.data();
        auto* nt  = (IMAGE_NT_HEADERS64*)(img.data() + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        printf("  %s\n", path);
        int bad = 0;
        for (int i = 0; i < SigCount(); i++) {
            const SigEntry& e = SigAt(i);
            Pat p;
            if (!parsePat(e.sig, p)) { printf("    %-22s BAD PATTERN\n", e.name); bad++; continue; }
            int hits = 0; uint32_t firstRva = 0;
            for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
                if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                const uint8_t* base = img.data() + sec[s].PointerToRawData;
                const size_t len = sec[s].SizeOfRawData;
                if (sec[s].PointerToRawData + len > (size_t)sz) continue;
                for (size_t o = 0; o + p.n <= len; o++) {
                    int k = 0;
                    for (; k < p.n; k++) if (!p.wild[k] && base[o + k] != p.b[k]) break;
                    if (k == p.n) { if (!hits) firstRva = sec[s].VirtualAddress + (uint32_t)o; hits++; }
                }
            }
            const bool ok = (hits == 1);
            if (!ok && e.required) bad++;
            printf("    %-22s %s  hits=%d%s%s\n", e.name,
                   hits ? "OK " : "MISS", hits,
                   hits ? "" : (e.required ? "   *** REQUIRED" : "   (optional)"),
                   hits > 1 ? "   *** AMBIGUOUS -- lengthen the sig" : "");
            if (hits == 1) printf("%*sexe+0x%x\n", 30, "", firstRva);
        }
        printf("    => %s\n\n", bad ? "*** FAIL" : "all required sigs unique");
        allOk = allOk && !bad;
    }
    printf(allOk ? "SYMCHECK PASS\n" : "*** SYMCHECK FAIL ***\n");
    return allOk ? 0 : 1;
}
