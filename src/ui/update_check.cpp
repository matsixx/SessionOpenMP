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
