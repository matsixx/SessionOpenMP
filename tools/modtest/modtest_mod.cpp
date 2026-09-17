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
// =====================================================================================================
// OmpModTest -- an in-game test of the mod channel, written the way a third-party mod would be: it
// includes ONLY sdk/omp_mod_api.h and knows nothing else about OpenMP. Not shipped.
//
// Install: Mods\OmpModTest\dlls\main.dll plus "OmpModTest : 1" in Mods\mods.txt, in every game.
// Output:  OmpModTest.log beside the game exe.
//
// What it exercises, on channel "omp.modtest":
//   join/leave     every OnPlayer, with the player's name, id, actor and the authority at that moment
//   counter        the authority broadcasts +1 every 3 s (reliable); others check it came from the
//                  authority and that no value was skipped
//   big            every 10 s, a full 1000-byte message with a checksum (reliable), verified on arrival
//   ping           Ctrl+Shift+P in the focused game: ping everyone, each replies, round trip logged
//   rate limit     Ctrl+Shift+L: 40 sends at once, logs how many were accepted (expect about 10)
//   status         a summary line every 10 s while in a session
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "omp_mod_api.h"
#include "ue4ss_abi.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

static FILE*      g_log = nullptr;
static std::mutex g_logLock;
static std::mutex g_state;           // callbacks run on the game thread, the pump on UE4SS's

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static OmpModApi omp;
static int       g_channel = 0;

enum : uint8_t { kFormat = 1 };
enum : uint8_t { MSG_COUNTER = 1, MSG_BIG = 2, MSG_PING = 3, MSG_PONG = 4 };

// counters (under g_state)
static uint32_t g_counterSent = 0, g_counterLast = 0, g_counterGaps = 0, g_counterSeen = 0;
static uint32_t g_bigOk = 0, g_bigBad = 0, g_bigSent = 0;
static uint32_t g_pingSeq = 0, g_pingsSent = 0, g_pongs = 0, g_pingsAnswered = 0;
static uint32_t g_refused = 0, g_rejected = 0;

static uint32_t fnv1a(const uint8_t* d, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

static const char* nameOf(int player, char* buf, int cap) {
    if (player < 0) { omp.LocalName(buf, cap); return buf; }
    if (!omp.PlayerName(player, buf, cap) || !buf[0]) snprintf(buf, (size_t)cap, "player %d", player);
    return buf;
}

static bool sendMsg(int player, const uint8_t* d, int len) {
    const bool ok = omp.Send(g_channel, player, d, len, 1) == 1;
    if (!ok) { std::lock_guard<std::mutex> lk(g_state); g_refused++; }
    return ok;
}

// ------------------------------------------------------------------ callbacks (game thread)
static void OnPlayer(int player, int joined, void*) {
    char nm[64], id[64] = "?", auth[64];
    nameOf(player, nm, sizeof(nm));
    omp.PlayerId(player, id, sizeof(id));
    const int a = omp.Authority(g_channel);
    Log("%s %s the channel (id %s, skater actor %s). Authority now: %s%s", nm, joined ? "JOINED" : "LEFT", id,
        omp.PlayerActor(player) ? "present" : "none", nameOf(a, auth, sizeof(auth)), a < 0 ? " (this game)" : "");
}

static void OnMessage(int player, const uint8_t* d, int len, void*) {
    char nm[64];
    if (len < 2 || d[0] != kFormat) { std::lock_guard<std::mutex> lk(g_state); g_rejected++; return; }
    switch (d[1]) {
    case MSG_COUNTER: {
        if (len != 6) break;
        if (player != omp.Authority(g_channel)) {
            Log("counter from %s IGNORED: they are not the authority", nameOf(player, nm, sizeof(nm)));
            return;
        }
        uint32_t v; memcpy(&v, d + 2, 4);
        std::lock_guard<std::mutex> lk(g_state);
        if (g_counterSeen && v != g_counterLast + 1) {
            g_counterGaps++;
            Log("counter jumped %u -> %u (a gap is expected only when the authority changed)", g_counterLast, v);
        }
        g_counterLast = v; g_counterSeen++;
        return;
    }
    case MSG_BIG: {
        if (len != OMPMOD_MAX_PAYLOAD) break;
        uint32_t want; memcpy(&want, d + len - 4, 4);
        const bool ok = fnv1a(d, len - 4) == want;
        { std::lock_guard<std::mutex> lk(g_state); if (ok) g_bigOk++; else g_bigBad++; }
        Log("1000-byte message from %s: %s", nameOf(player, nm, sizeof(nm)), ok ? "checksum OK" : "*** CHECKSUM BAD");
        return;
    }
    case MSG_PING: {
        if (len != 14) break;
        uint8_t reply[14]; memcpy(reply, d, 14); reply[1] = MSG_PONG;
        sendMsg(player, reply, sizeof(reply));
        { std::lock_guard<std::mutex> lk(g_state); g_pingsAnswered++; }
        Log("ping from %s, answered", nameOf(player, nm, sizeof(nm)));
        return;
    }
    case MSG_PONG: {
        if (len != 14) break;
        uint32_t seq; uint64_t t0; memcpy(&seq, d + 2, 4); memcpy(&t0, d + 6, 8);
        const uint64_t now = GetTickCount64();
        { std::lock_guard<std::mutex> lk(g_state); g_pongs++; }
        Log("pong #%u from %s: %llu ms round trip", seq, nameOf(player, nm, sizeof(nm)),
            (unsigned long long)(now >= t0 ? now - t0 : 0));
        return;
    }
    default: break;
    }
    std::lock_guard<std::mutex> lk(g_state);
    g_rejected++;
}

// ------------------------------------------------------------------ the pump (UE4SS thread; the API is thread-safe)
static bool keyEdge(bool down, bool* held) {
    const bool edge = down && !*held;
    *held = down;
    return edge;
}

static bool ourWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void Pump() {
    static uint64_t lastBind = 0, lastCounter = 0, lastBig = 0, lastStatus = 0;
    static bool wasInSession = false, pHeld = false, lHeld = false;
    const uint64_t now = GetTickCount64();

    if (!g_channel) {
        if (now - lastBind < 1000) return;
        lastBind = now;
        if (!omp.version && !OmpMod_Bind(&omp)) return;
        g_channel = omp.Register("omp.modtest", OnMessage, OnPlayer, nullptr);
        Log(g_channel ? "bound to SessionOpenMP (API version %d), channel omp.modtest registered"
                      : "bound to SessionOpenMP (API version %d), but Register REFUSED -- see SessionOpenMP.log", omp.version);
        if (!g_channel) { omp.version = 0; return; }
    }

    const bool inSession = omp.InSession() == 1;
    if (inSession != wasInSession) {
        char me[64], id[64] = "";
        omp.LocalName(me, sizeof(me)); omp.LocalId(id, sizeof(id));
        Log(inSession ? "session started -- you are %s (id %s)" : "session ended (%s)", me, id);
        wasInSession = inSession;
    }
    if (!inSession) return;

    const bool ctrlShift = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    const bool focused = ourWindowFocused();
    if (keyEdge(focused && ctrlShift && (GetAsyncKeyState('P') & 0x8000), &pHeld)) {
        uint8_t ping[14]; ping[0] = kFormat; ping[1] = MSG_PING;
        uint32_t seq; { std::lock_guard<std::mutex> lk(g_state); seq = ++g_pingSeq; g_pingsSent++; }
        memcpy(ping + 2, &seq, 4); memcpy(ping + 6, &now, 8);
        Log("Ctrl+Shift+P: ping #%u to %d player(s) %s", seq, omp.Players(g_channel, nullptr, 0),
            sendMsg(OMPMOD_EVERYONE, ping, sizeof(ping)) ? "queued" : "REFUSED");
        int players[16];
        const int n = omp.Players(g_channel, players, 16);
        for (int i = 0; i < n && i < 16; i++) {
            char nm[64];
            void* actor = omp.PlayerActor(players[i]);
            Log("  %s: skater actor %s, ActorPlayer -> %d (expect %d)", nameOf(players[i], nm, sizeof(nm)),
                actor ? "present" : "none", actor ? omp.ActorPlayer(actor) : -1, actor ? players[i] : -1);
        }
    }
    if (keyEdge(focused && ctrlShift && (GetAsyncKeyState('L') & 0x8000), &lHeld)) {
        int accepted = 0;
        const uint8_t junk[2] = { kFormat, 0xEE };             // an unknown type: receivers count and ignore it
        for (int i = 0; i < 40; i++) accepted += omp.Send(g_channel, OMPMOD_EVERYONE, junk, sizeof(junk), 0);
        Log("Ctrl+Shift+L: rate limit test -- %d of 40 sends accepted (expect about 10)", accepted);
    }

    if (omp.IsAuthority(g_channel) && now - lastCounter >= 3000) {
        lastCounter = now;
        uint8_t m[6]; m[0] = kFormat; m[1] = MSG_COUNTER;
        uint32_t v; { std::lock_guard<std::mutex> lk(g_state); v = ++g_counterSent; g_counterLast = v; }
        memcpy(m + 2, &v, 4);
        sendMsg(OMPMOD_EVERYONE, m, sizeof(m));
    }

    if (now - lastBig >= 10000 && omp.Players(g_channel, nullptr, 0) > 0) {
        lastBig = now;
        uint8_t big[OMPMOD_MAX_PAYLOAD];
        big[0] = kFormat; big[1] = MSG_BIG;
        for (int i = 2; i < OMPMOD_MAX_PAYLOAD - 4; i++) big[i] = (uint8_t)(i * 31 + (int)(now & 0xFF));
        const uint32_t sum = fnv1a(big, OMPMOD_MAX_PAYLOAD - 4);
        memcpy(big + OMPMOD_MAX_PAYLOAD - 4, &sum, 4);
        if (sendMsg(OMPMOD_EVERYONE, big, sizeof(big))) { std::lock_guard<std::mutex> lk(g_state); g_bigSent++; }
    }

    if (now - lastStatus >= 10000) {
        lastStatus = now;
        int players[16];
        const int n = omp.Players(g_channel, players, 16);
        char list[512] = ""; size_t w = 0;
        for (int i = 0; i < n && i < 16; i++) {
            char nm[64];
            w += (size_t)snprintf(list + w, sizeof(list) - w, "%s%s", i ? ", " : "", nameOf(players[i], nm, sizeof(nm)));
            if (w >= sizeof(list)) break;
        }
        const int a = omp.Authority(g_channel);
        char auth[64];
        std::lock_guard<std::mutex> lk(g_state);
        Log("STATUS players with the channel: %d [%s] | authority: %s | counter %u (gaps %u) | "
            "1000-byte sent %u, ok %u, bad %u | pings sent %u, pongs %u, answered %u | refused %u, ignored %u",
            n, list, a < 0 ? "this game" : nameOf(a, auth, sizeof(auth)), g_counterLast, g_counterGaps,
            g_bigSent, g_bigOk, g_bigBad, g_pingsSent, g_pongs, g_pingsAnswered, g_refused, g_rejected);
    }
}

// ------------------------------------------------------------------ UE4SS shell
class OmpModTest : public RC::CppUserModBase
{
public:
    OmpModTest() {
        ModName = STR("OmpModTest");
        ModVersion = STR("1");
        ModDescription = STR("In-game test of the SessionOpenMP mod channel");
        ModAuthors = STR("matsix");
        char dir[MAX_PATH]{};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *(slash + 1) = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sOmpModTest.log", dir);
        g_log = fopen(path, "w");
        Log("=== OmpModTest loaded. Waiting for SessionOpenMP. Ctrl+Shift+P = ping, Ctrl+Shift+L = rate limit test ===");
    }
    // No Unregister here: at game exit UE4SS may already have unloaded SessionOpenMP.
    ~OmpModTest() override {
        Log("=== OmpModTest unloaded ===");
        std::lock_guard<std::mutex> lk(g_logLock);
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    auto on_update() -> void override { Pump(); }
};

extern "C" {
    __declspec(dllexport) RC::CppUserModBase* start_mod()           { return new OmpModTest(); }
    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* m) { delete m; }
}
