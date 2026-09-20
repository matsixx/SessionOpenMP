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
// SessionOpenMP -- "there is a newer version", asked once, at startup.
//
// WHY THIS EXISTS: a version mismatch between two players is the worst failure this project has.
// The lobby joins, EOS reports the connection ESTABLISHED, and the other player is simply INVISIBLE
// -- because a packet whose magic does not match is dropped without a word. Both people conclude
// the mod is broken. Publishing releases is what makes that common, so the fix is to tell somebody
// their copy is old BEFORE they try to play with it.
//
// THREADING, which is the whole design:
//   * ONE SHOT, DETACHED. There is no transport thread in this project and there must never be one
//     (standing rule). This starts a single thread, does one request, publishes an answer into an
//     atomic and exits. It does not loop, does not poll and does not outlive the question.
//   * IT TOUCHES NO GAME API. Not the engine, not UE4SS, not a single symbol from the table. It
//     talks to WinHTTP and writes plain scalars. The game thread reads the result and does all the
//     showing. Calling a game function from here is the classic way to take the process down.
//   * IT FAILS SILENTLY. Offline, rate-limited, DNS down, GitHub having a bad day -- all of them
//     mean "say nothing". A check that nags when it cannot reach the internet is worse than no
//     check, because it trains people to dismiss it.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "update_check.h"
#include "version_tag.h"

#pragma comment(lib, "winhttp.lib")

namespace omp { namespace ui {

// 0 = still asking or never asked, 1 = we are current (or could not tell), 2 = there is a newer one.
static volatile LONG g_state = 0;
static char          g_latest[32] = {0};        // written by the worker BEFORE g_state goes to 2

// ---- the published release notes ----------------------------------------------------------------
// Filled by the same worker, from the release LIST. Sized for what the panel can hold rather than for
// what GitHub sends: the text goes into the widget through an FName, whose limit is 1024 characters
// for the WHOLE page, so keeping more than this would only be thrown away later.
// kNoteBody is per release and generous on purpose: the panel used to be capped by FName's 1024
// characters for the WHOLE page, so storing more than a few hundred was pointless. FText::FromString
// removed that ceiling, and what governs now is how many lines the box draws -- so a release is kept
// whole and the panel decides how much of it to use.
enum { kMaxNotes = 6, kNoteBody = 2048, kNoteVer = 32, kListBytes = 262144 };
static char          g_noteVer[kMaxNotes][kNoteVer] = {};
static char          g_noteBody[kMaxNotes][kNoteBody] = {};
static volatile LONG g_noteCount = 0;           // written LAST: a reader seeing n can read n entries
static volatile LONG g_noteGen   = 0;

int UpdateCheck_NotesCount() { return (int)InterlockedCompareExchange(&g_noteCount, 0, 0); }
unsigned UpdateCheck_NotesGeneration() { return (unsigned)InterlockedCompareExchange(&g_noteGen, 0, 0); }
bool UpdateCheck_NoteAt(int i, char* verOut, int verCap, char* bodyOut, int bodyCap) {
    if (i < 0 || i >= UpdateCheck_NotesCount()) return false;
    if (verOut  && verCap  > 0) { strncpy(verOut,  g_noteVer[i],  (size_t)verCap  - 1); verOut[verCap - 1]   = 0; }
    if (bodyOut && bodyCap > 0) { strncpy(bodyOut, g_noteBody[i], (size_t)bodyCap - 1); bodyOut[bodyCap - 1] = 0; }
    return true;
}
static volatile LONG g_started = 0;

// ---- version comparison ------------------------------------------------------------------------
// Written against the tags this project ACTUALLY publishes, which is not what a first guess assumes:
// they are "v1.0.0rc3", "v0.9.5b", "v0.8.2b" -- a dotted number followed by letters, with NO
// separator. The first cut here treated a pre-release as "-something", so "1.0.0-rc4" versus
// "1.0.0rc3" compared the local build's "-rc4" against a suffix it did not recognise as one, and
// concluded rc4 was OLDER than rc3. That is a false "you are out of date" shown to somebody who is
// ahead -- the exact opposite of the job.
//
// So: split into NUMBERS and a SUFFIX, and let the suffix start with a hyphen, an underscore or a
// letter. The rules, in order:
//   1. Compare the dotted numbers, numerically ("1.10.0" is newer than "1.2.0", which a string
//      compare gets backwards). A missing component is 0, so "1.0" == "1.0.0".
//   2. Equal numbers, and one has NO suffix: that one is the finished release and is NEWER. This is
//      what makes 1.0.0 beat 1.0.0rc3 and rc4 alike.
//   3. Both have suffixes: letters first, lexically ("b" before "rc"), then any trailing number
//      NUMERICALLY, so rc10 comes after rc9 rather than before it.
// Anything unparseable falls out as EQUAL, and equal means silence -- the safe answer for a check
// nobody asked to be nagged by.
static int suffixCmp(const char* a, const char* b) {
    while (*a == '-' || *a == '_') a++;
    while (*b == '-' || *b == '_') b++;
    const bool ea = (*a == 0), eb = (*b == 0);
    if (ea && eb) return 0;
    if (ea) return 1;                      // no suffix = the release itself = newer
    if (eb) return -1;
    // letters, then the number that may follow them
    const char* pa = a; while (*pa && !(*pa >= '0' && *pa <= '9')) pa++;
    const char* pb = b; while (*pb && !(*pb >= '0' && *pb <= '9')) pb++;
    const size_t la = (size_t)(pa - a), lb = (size_t)(pb - b);
    const int alpha = strncmp(a, b, la < lb ? la : lb);
    if (alpha) return alpha < 0 ? -1 : 1;
    if (la != lb) return la < lb ? -1 : 1;
    const long na = *pa ? strtol(pa, nullptr, 10) : -1;
    const long nb = *pb ? strtol(pb, nullptr, 10) : -1;
    if (na != nb) return na < nb ? -1 : 1;
    return 0;
}

int UpdateCheck_CompareVersions(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a == 'v' || *a == 'V') a++;
    while (*b == 'v' || *b == 'V') b++;
    for (int part = 0; part < 8; part++) {
        const bool da = (*a >= '0' && *a <= '9');
        const bool db = (*b >= '0' && *b <= '9');
        if (!da && !db) break;                       // both out of numbers: the suffixes decide
        const long na = da ? strtol(a, (char**)&a, 10) : 0;   // a missing component is 0
        const long nb = db ? strtol(b, (char**)&b, 10) : 0;
        if (na != nb) return na < nb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return suffixCmp(a, b);
}

// ---- the request -------------------------------------------------------------------------------
// Deliberately tiny: no JSON library, one field. "tag_name":"v1.0.0-rc4" is the only thing wanted,
// and a scan for that key is less code and less risk than parsing a document we do not control.
static bool extractTag(const char* body, char* out, int cap) {
    const char* k = strstr(body, "\"tag_name\"");
    if (!k) return false;
    k = strchr(k + 10, '"');
    if (!k) return false;
    k++;
    int n = 0;
    while (*k && *k != '"' && n < cap - 1) out[n++] = *k++;
    out[n] = 0;
    return n > 0;
}

// ---- the release list --------------------------------------------------------------------------
// Still no JSON library, for the reason above: this reads two keys out of a document we do not
// control, and a scan for them is less code and less risk than parsing the whole thing. The array
// arrives newest-first, so the order it is stored in is the order it is shown in.

// A JSON string value, unescaped, from just after its opening quote. Returns where it ended.
static const char* jsonString(const char* p, char* out, int cap) {
    int n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            const char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': continue;                     // CRLF from GitHub: keep the \n, drop the \r
                case 't': c = ' ';  break;
                case 'u': for (int k = 0; k < 4 && *p; k++) p++; c = ' '; break;   // not worth decoding
                default:  c = e;    break;              // \" \\ \/ are themselves
            }
        }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return p;
}

// GitHub markdown -> what a single text block can render. Headings and emphasis markers are dropped,
// `code` loses its backticks, [text](url) keeps the text, and runs of blank lines collapse. Lines are
// kept whole: the panel wraps them itself.
static void plainify(const char* in, char* out, int cap) {
    int n = 0;
    bool atLineStart = true, lastWasBlank = true;
    for (const char* p = in; *p && n < cap - 1; ) {
        if (*p == '\n') {
            p++;
            if (lastWasBlank) continue;                 // never two blank lines in a row
            out[n++] = '\n';
            atLineStart = true; lastWasBlank = true;
            continue;
        }
        if (atLineStart) {
            while (*p == ' ' || *p == '\t') p++;
            while (*p == '#' || *p == '>') p++;         // heading / quote markers
            while (*p == ' ') p++;
            if (*p == '*' || *p == '+') { const char* q = p + 1; if (*q == ' ') { p++; out[n++] = '-'; } }
            atLineStart = false;
            if (!*p) break;
        }
        if (*p == '*' || *p == '`' || *p == '_') { p++; continue; }      // emphasis / code markers
        if (*p == '[') {                                                 // [text](url) -> text
            const char* close = strchr(p, ']');
            if (close && close[1] == '(') {
                for (const char* q = p + 1; q < close && n < cap - 1; q++) out[n++] = *q;
                const char* end = strchr(close, ')');
                p = end ? end + 1 : close + 1;
                lastWasBlank = false;
                continue;
            }
        }
        out[n++] = *p++;
        lastWasBlank = false;
    }
    // If the cap cut it, cut back to the last COMPLETE line: a stored body ending mid-word would be
    // rendered as though it were a whole bullet.
    if (n >= cap - 1) { while (n > 0 && out[n - 1] != '\n') n--; }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) n--;      // no trailing blank line
    out[n] = 0;
}

static void parseReleases(const char* json) {
    int found = 0;
    const char* p = json;
    while (found < kMaxNotes) {
        const char* tagKey = strstr(p, "\"tag_name\"");
        if (!tagKey) break;
        // This release's slice of the document: up to the next release's tag, so a key found below
        // cannot belong to a different entry.
        const char* next = strstr(tagKey + 10, "\"tag_name\"");
        char ver[kNoteVer] = {0};
        const char* q = strchr(tagKey + 10, '"');
        if (q) jsonString(q + 1, ver, sizeof(ver));
        p = tagKey + 10;
        if (!ver[0]) continue;
        // Drafts and pre-releases are not what anybody means by "what's new".
        const char* draft = strstr(tagKey, "\"draft\":true");
        const char* pre   = strstr(tagKey, "\"prerelease\":true");
        if ((draft && (!next || draft < next)) || (pre && (!next || pre < next))) continue;
        const char* bodyKey = strstr(tagKey, "\"body\"");
        if (!bodyKey || (next && bodyKey > next)) continue;
        const char* b = strchr(bodyKey + 6, '"');
        if (!b) continue;
        static char raw[8192];
        jsonString(b + 1, raw, sizeof(raw));
        if (!raw[0]) continue;
        // A leading "v" is the tag's, not the version's: the panel says 1.2.1, like everything else.
        const char* vshow = (ver[0] == 'v' || ver[0] == 'V') ? ver + 1 : ver;
        strncpy(g_noteVer[found], vshow, kNoteVer - 1);
        plainify(raw, g_noteBody[found], kNoteBody);
        if (g_noteBody[found][0]) found++;
    }
    if (found > 0) {
        InterlockedExchange(&g_noteCount, found);       // count LAST: the entries are written first
        InterlockedIncrement(&g_noteGen);
    }
}

static DWORD WINAPI worker(LPVOID) {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    char body[16384];
    DWORD total = 0;
    bool got = false;
    // GitHub requires a User-Agent and will refuse without one. Naming the mod also means a glance
    // at their logs says who is asking.
    ses = WinHttpOpen(L"SessionOpenMP", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        // Short timeouts. This runs while the game is booting and nobody is waiting on it; a slow
        // network must never turn into a thread hanging around for a minute.
        WinHttpSetTimeouts(ses, 4000, 4000, 4000, 4000);
        con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (con) {
        req = WinHttpOpenRequest(con, L"GET",
                                 L"/repos/matsixx/SessionOpenMP/releases/latest",
                                 nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && total < sizeof(body) - 1) {
            DWORD want = avail;
            if (want > sizeof(body) - 1 - total) want = (DWORD)(sizeof(body) - 1 - total);
            DWORD read = 0;
            if (!WinHttpReadData(req, body + total, want, &read) || !read) break;
            total += read;
        }
        body[total] = 0;
        got = total > 0;
    }
    if (req) WinHttpCloseHandle(req);
    req = nullptr;

    // ---- SECOND REQUEST: the release LIST, for the notes the What's New panel shows.
    // A separate request rather than reading the notes out of the one above, because the two want
    // different things: "am I out of date" must use /releases/latest, which EXCLUDES drafts and
    // pre-releases, and swapping it for the list would start announcing a pre-release as an update.
    // Bodies are prose and there are several of them, so the buffer is heap rather than stack.
    if (con) {
        char* list = (char*)malloc(kListBytes);
        DWORD n = 0;
        if (list) {
            req = WinHttpOpenRequest(con, L"GET",
                                     L"/repos/matsixx/SessionOpenMP/releases?per_page=6",
                                     nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && n < kListBytes - 1) {
                    DWORD want = avail;
                    if (want > kListBytes - 1 - n) want = kListBytes - 1 - n;
                    DWORD read = 0;
                    if (!WinHttpReadData(req, list + n, want, &read) || !read) break;
                    n += read;
                }
                list[n] = 0;
                if (n > 0) parseReleases(list);
            }
            free(list);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);

    if (got) {
        char tag[32] = {0};
        if (extractTag(body, tag, sizeof(tag)) && UpdateCheck_CompareVersions(OMP_VERSION_STRING, tag) < 0) {
            // The string lands BEFORE the flag: the game thread reads the flag first and would
            // otherwise be able to see "newer version" with nothing to name.
            strncpy(g_latest, tag, sizeof(g_latest) - 1);
            InterlockedExchange(&g_state, 2);
            return 0;
        }
    }
    InterlockedExchange(&g_state, 1);        // current, or unknowable -- both mean say nothing
    return 0;
}

void UpdateCheck_Start() {
    if (InterlockedExchange(&g_started, 1)) return;          // once per process, ever
    HANDLE h = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);                                   // detached: nothing waits on it
    else InterlockedExchange(&g_state, 1);
}

bool UpdateCheck_NewerAvailable(char* latestOut, int cap) {
    if (InterlockedCompareExchange(&g_state, 2, 2) != 2) return false;
    if (latestOut && cap > 0) { strncpy(latestOut, g_latest, (size_t)cap - 1); latestOut[cap - 1] = 0; }
    return true;
}

} }  // namespace omp::ui
