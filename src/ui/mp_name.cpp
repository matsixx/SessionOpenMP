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
// SessionOpenMP -- the multiplayer name and its word filter. Design: mp_name.h.
//
// The list is COMPILED IN: a word list shipped as a file beside the mod is a filter anyone can
// delete, so the check has to live somewhere the player cannot edit. It is deliberately compact --
// the goal is to stop the obvious cases, not to be exhaustive. Anything that gets through is a name
// a human can still report.
#include "mp_name.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#endif

static char  g_name[OMP_NAME_MAX + 1] = "Skater";
static char  g_namePath[512]  = {0};
static void (*g_log)(const char*) = nullptr;
static void  say(const char* s) { if (g_log) g_log(s); }

// =====================================================================================================
// THE LIST. Split in two, and the split is the whole reason this works.
//
// `kAnywhere` -- terms with no innocent containment in ordinary English or in names. Matched as a
// SUBSTRING, so padding ("xxfuckxx") does not get past.
//
// `kWholeWord` -- terms that ARE contained in perfectly normal words, so they only count as the whole
// name or a whole word inside it. "ass" is in bass/class/pass/grass, "cock" in peacock/Hancock,
// "coon" in raccoon/tycoon, "spic" in suspicion, "cum" in cumbersome, "hell" in shell/Michelle,
// "arse" in sparse/coarse, "rape" in grape/drape, "dick" in Dickinson, "tit" in title/Titan.
// Putting any of those in the substring list would reject real names -- which is how a filter loses
// the room's trust and gets switched off.
// =====================================================================================================
static const char* const kAnywhere[] = {
    "nigger", "nigga", "niger",  "faggot", "fagot", "kike", "chink", "gook", "wetback",
    "jigaboo", "shitskin", "towelhead", "raghead", "beaner", "tranny", "shemale",
    "retard", "spastic", "mongoloid",
    "fuck", "fuk", "phuck", "shit", "bitch", "cunt", "whore", "slut", "wanker", "wank",
    "twat", "bollock", "bastard", "arsehole", "asshole", "assh0le", "dickhead", "dumbass",
    "penis", "vagina", "clitoris", "jizz", "cumshot", "blowjob", "handjob", "rimjob",
    "porn", "hentai", "incest", "bestiality", "molest", "rapist", "pedophile", "paedophile",
    "childporn", "cp0rn", "nazi", "hitler", "kkk", "klux", "genocide", "lynch",
    "suicide", "killyourself", "kys",
};
// Whole-name or whole-word only.
static const char* const kWholeWord[] = {
    "ass", "arse", "anus", "cock", "dick", "coon", "spic", "cum", "tit", "tits", "boob", "boobs",
    "hell", "damn", "piss", "crap", "prick", "knob", "semen", "rape", "pedo", "fag", "homo",
    "queer", "dyke", "hoe", "skank", "milf", "sex", "sexy", "nude", "nudes", "naked",
};
static const int kNAnywhere  = (int)(sizeof(kAnywhere)  / sizeof(kAnywhere[0]));
static const int kNWholeWord = (int)(sizeof(kWholeWord) / sizeof(kWholeWord[0]));

// Fold the tricks people use to smuggle a word past a list: case, leet digits, and any punctuation or
// spacing at all. "@$$h0le", "A55HOLE" and "a s s h o l e" all normalise to the same thing.
static void normalise(const char* in, char* out, int cap) {
    int n = 0;
    for (const char* p = in; *p && n < cap - 1; p++) {
        char c = (char)tolower((unsigned char)*p);
        switch (c) {
            case '0': c = 'o'; break;
            case '1': case '|': c = 'i'; break;
            case '3': c = 'e'; break;
            case '4': c = 'a'; break;
            case '5': c = 's'; break;
            case '7': c = 't'; break;
            case '8': c = 'b'; break;
            case '@': c = 'a'; break;
            case '$': c = 's'; break;
            case '!': c = 'i'; break;
            case '+': c = 't'; break;
            default: break;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = c;
    }
    out[n] = 0;
}
// Squash runs of the same letter: "fuuuck" -> "fuck", "niiigger" -> "niger". Used ONLY against the
// anywhere-list, and only for entries still >= 4 characters once squashed -- squashing shortens
// everything, and a 3-letter needle in a haystack of real names is how false positives start.
static void collapse(const char* in, char* out, int cap) {
    int n = 0;
    char last = 0;
    for (const char* p = in; *p && n < cap - 1; p++) {
        if (*p == last) continue;
        last = *p;
        out[n++] = *p;
    }
    out[n] = 0;
}

// Whole-word test: split the RAW name on non-alphanumerics, normalise each token, compare.
static bool wholeWordHit(const char* raw, const char* norm, const char* needle) {
    if (strcmp(norm, needle) == 0) return true;
    char tok[OMP_NAME_MAX + 1];
    int t = 0;
    for (const char* p = raw; ; p++) {
        const bool end = (*p == 0);
        if (end || !isalnum((unsigned char)*p)) {
            if (t) {
                tok[t] = 0;
                char ntok[OMP_NAME_MAX + 1];
                normalise(tok, ntok, sizeof(ntok));
                if (strcmp(ntok, needle) == 0) return true;
                t = 0;
            }
            if (end) break;
        } else if (t < OMP_NAME_MAX) {
            tok[t++] = *p;
        }
    }
    return false;
}

bool MpName_FilterLoaded() { return true; }          // compiled in -- always present
const char* MpName_Get() { return g_name[0] ? g_name : "Skater"; }

static void saveName() {
    if (!g_namePath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_namePath, "wb") == 0 && f) { fputs(g_name, f); fclose(f); }
}

bool MpName_Set(const char* candidate, char* reason, int reasonCap) {
    auto fail = [&](const char* why) {
        if (reason && reasonCap > 0) strncpy_s(reason, (size_t)reasonCap, why, _TRUNCATE);
        return false;
    };
    if (reason && reasonCap > 0) reason[0] = 0;
    if (!candidate) return fail("No name given.");

    // Trim the ends; interior spaces are fine.
    char buf[OMP_NAME_MAX + 1] = {0};
    const char* s = candidate;
    while (*s == ' ' || *s == '\t') s++;
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n > OMP_NAME_MAX) return fail("Too long -- 20 characters maximum.");
    if (n < 2)            return fail("Too short -- at least 2 characters.");
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;

    // ASCII letters/digits/space and a little punctuation. Keeps the name renderable in the game's
    // font and keeps it from smuggling markup into the RichTextBlock the roster panel draws into.
    for (int i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)buf[i];
        const bool ok = isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.';
        if (!ok) return fail("Letters, numbers, spaces, - _ . only.");
    }

    char norm[OMP_NAME_MAX + 1];
    normalise(buf, norm, sizeof(norm));
    if (!norm[0]) return fail("Needs at least a couple of letters or numbers.");
    char squashed[OMP_NAME_MAX + 1];
    collapse(norm, squashed, sizeof(squashed));

    char why[96];
    for (int i = 0; i < kNAnywhere; i++) {
        const char* w = kAnywhere[i];
        bool hit = (strstr(norm, w) != nullptr);
        if (!hit) {
            char cw[64];
            collapse(w, cw, sizeof(cw));
            if ((int)strlen(cw) >= 4) hit = (strstr(squashed, cw) != nullptr);
        }
        if (hit) { snprintf(why, sizeof(why), "Not allowed (matched \"%s\").", w); return fail(why); }
    }
    for (int i = 0; i < kNWholeWord; i++) {
        if (!wholeWordHit(buf, norm, kWholeWord[i])) continue;
        snprintf(why, sizeof(why), "Not allowed (matched \"%s\").", kWholeWord[i]);
        return fail(why);
    }

    strncpy_s(g_name, buf, _TRUNCATE);
    saveName();
    char m[128];
    snprintf(m, sizeof(m), "[name] multiplayer name set to \"%s\"", g_name);
    say(m);
    return true;
}

void MpName_Init(const char* dir, void (*logf)(const char*)) {
    g_log = logf;
    if (!dir || !*dir) return;
    snprintf(g_namePath, sizeof(g_namePath), "%sSessionOpenMP_name.txt", dir);

    // A previously saved name is re-validated on load, so tightening the list later retires a name
    // that previously passed instead of grandfathering it.
    FILE* f = nullptr;
    if (fopen_s(&f, g_namePath, "rb") == 0 && f) {
        char saved[128] = {0};
        if (fgets(saved, sizeof(saved), f)) {
            char* nl = strpbrk(saved, "\r\n");
            if (nl) *nl = 0;
            char why[128], m[220];
            if (saved[0] && !MpName_Set(saved, why, sizeof(why))) {
                snprintf(m, sizeof(m), "[name] saved name rejected (%s) -- using \"%s\"", why, MpName_Get());
                say(m);
            }
        }
        fclose(f);
    }
    char m[160];
    snprintf(m, sizeof(m), "[name] multiplayer name: \"%s\" (filter: %d blocked / %d word-only)",
             MpName_Get(), kNAnywhere, kNWholeWord);
    say(m);
}
