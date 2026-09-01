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
// omp_relaytest -- the relay backend, end to end, with no game and no network.
//
// It launches the REAL relay server on loopback and THREE real clients against it, because the
// thing worth proving is the property direct UDP cannot have: every player sees every other player
// without any of them being reachable. Two processes cannot show that -- a star and a mesh look
// identical at two -- so the gate is three.
//
// It also proves the amplification defence, which is the one thing here that protects somebody
// else's machine rather than ours: a short room-list request must be IGNORED.
#define _CRT_SECURE_NO_WARNINGS
#include "../../src/transport/transport.h"
#include "../../src/transport/relay_proto.h"
#include "../../src/replication/replication.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

using namespace omp;
namespace R = omp::relay;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "*** FAIL");
    if (!ok) g_fails++;
}
static const int  kPort = 47820;
static const char kRoom[] = "GATE01";
// The burst: 600 chunks of 900 B at 20 a tick is ~1.1 MB/s sustained, which is comfortably past
// the 512 KB/s cap this test was written to catch and in the same range as a real replay sync.
// One sender, like a real transfer; the other two must receive every chunk.
static const int kBurstRole    = 2;
static const int kBurstCount   = 600;
static const int kBurstPerTick = 20;

static void makeState(repl::State& s, float tag, uint32_t n) {
    s = repl::State{};
    s.bodyPosOk = 1;
    s.bodyPos[0] = tag; s.bodyPos[1] = 200.f + (float)(n % 50); s.bodyPos[2] = 50.f;
    s.deckPos[0] = 110.f; s.deckPos[1] = 195.f; s.deckPos[2] = 40.f;
    const float q[4] = { 0.f, 0.f, 0.f, 1.f };
    memcpy(s.deckQuat, q, sizeof(q));
    s.onBoard = 1; s.grounded = 1; s.pushSpeed = 1.f;
}

struct Ctx { int from[4] = {0}; int bad = 0; int rel = 0; int bulk = 0; };
static void onRecv(int peer, const uint8_t* d, int len, void* user) {
    Ctx& c = *(Ctx*)user;
    if (len >= 8 && !memcmp(d, "RELI", 4)) { c.rel++; return; }
    if (len >= 8 && !memcmp(d, "BULK", 4)) { c.bulk++; return; }
    repl::State s; uint64_t su = 0;
    if (!repl::Unpack(d, len, s, &su)) { c.bad++; return; }
    const int tag = (int)(s.bodyPos[0] + 0.5f);
    if (tag >= 1 && tag <= 3) c.from[tag]++; else c.bad++;
    (void)peer;
}

// One client. `role` is also its tag, so a received packet names its own sender.
static int runClient(int role) {
    char log[64]; snprintf(log, sizeof(log), "omp_relaytest_%d.log", role);
    char id[40]; snprintf(id, sizeof(id), "player%d", role);
    SetLocalIdentity(id);
    SetRelayServer("127.0.0.1", kPort);
    SetLobbyCode(kRoom);
    SetLobbyAd(id, "gate-map");
    if (!Init(BK_RELAY, log, false)) { printf("[%d] init FAILED\n", role); return 1; }
    // Host and Join are the same act on a relay -- the first to name a room opens it. Role 1 uses
    // Host so the path a player takes is the path under test.
    const bool ok = (role == 1) ? LobbyHost() : LobbyJoin();
    if (!ok) { printf("[%d] enter room FAILED\n", role); return 1; }

    Ctx ctx;
    int peak = 0, relSent = 0;
    for (int i = 0; i < 800; i++) {                    // ~13 s
        const int n = PeerCount();
        if (n > peak) peak = n;
        repl::State s; makeState(s, (float)role, (uint32_t)i);
        uint8_t pkt[900];
        const int len = repl::Pack(s, (uint64_t)i * 16667, pkt, sizeof(pkt));
        for (int p = 0; p < n; p++) Send(p, pkt, len, false);
        if (n > 0 && (i % 40) == 0 && relSent < 8) {   // the reliable lane, through the relay
            uint8_t rel[64];
            memcpy(rel, "RELI", 4);
            memcpy(rel + 4, &relSent, 4);
            memset(rel + 8, role, sizeof(rel) - 8);
            for (int p = 0; p < n; p++) Send(p, rel, (int)sizeof(rel), true);
            relSent++;
        }
        Tick(onRecv, &ctx);
        Sleep(16);
    }
    // ---- A BULK TRANSFER, which is what a replay sync is. This is here because its absence let a
    // real bug ship: the relay had a 512 KB/s per-client rate cap, chosen against the 30 Hz game
    // stream, and a replay sync is nowhere near that shape -- it is ~1.8 MB/s of RELIABLE chunks
    // paced by SendBudget. The relay silently dropped them, the reliable lane retransmitted (making
    // it worse), and the sync gave up at "stalled" in the field. Eight small messages could never
    // have caught that; a sustained burst does, and it must arrive INTACT.
    if (role == kBurstRole) {
        int sent = 0;
        while (sent < kBurstCount) {
            const int budget = SendBudget() < kBurstPerTick ? SendBudget() : kBurstPerTick;
            for (int k = 0; k < budget && sent < kBurstCount; k++) {
                uint8_t b[900];
                memcpy(b, "BULK", 4);
                memcpy(b + 4, &sent, 4);
                memset(b + 8, (int)(sent & 0xff), sizeof(b) - 8);
                for (int p = 0; p < PeerCount(); p++) Send(p, b, (int)sizeof(b), true);
                sent++;
            }
            Tick(onRecv, &ctx);
            Sleep(16);
        }
    }
    // Long enough for the reliable lane to finish retransmitting anything that was actually lost.
    for (int i = 0; i < 240; i++) { Tick(onRecv, &ctx); Sleep(16); }

    printf("\n[player %d] results\n", role);
    char m[160];
    snprintf(m, sizeof(m), "sees BOTH other players (peers=%d)", peak);
    check(peak >= 2, m);
    int heard = 0;
    for (int t = 1; t <= 3; t++) if (t != role && ctx.from[t] > 50) heard++;
    snprintf(m, sizeof(m), "a stream from BOTH (p1=%d p2=%d p3=%d)", ctx.from[1], ctx.from[2], ctx.from[3]);
    check(heard == 2, m);
    snprintf(m, sizeof(m), "reliable messages arrived through the relay (%d of 16)", ctx.rel);
    check(ctx.rel == 16, m);                            // 8 from each of the other two
    if (role != kBurstRole) {
        snprintf(m, sizeof(m), "a %d KB bulk transfer arrived WHOLE (%d of %d chunks)",
                 kBurstCount * 900 / 1024, ctx.bulk, kBurstCount);
        check(ctx.bulk == kBurstCount, m);
    }
    check(ctx.bad == 0, "every packet unpacked cleanly");
    LobbyLeave();
    Shutdown();
    return g_fails ? 1 : 0;
}

// THE AMPLIFICATION DEFENCE. A room-list request shorter than the reply must be dropped, or this
// server is a reflector: an attacker spoofs a victim's address, sends a few bytes, and the relay
// sprays a much larger reply at them. Proven by asking BOTH ways from a raw socket.
static void checkNoAmplification() {
    // The client above ends with Shutdown(), which calls WSACleanup for the whole process -- so
    // Winsock has to be brought back up for this probe rather than assumed still running.
    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { check(false, "WSAStartup for the probe"); return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { check(false, "could not open a probe socket"); return; }
    DWORD to = 1200; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)kPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);

    uint8_t buf[2048];
    R::Hdr h{};
    h.magic = R::kMagic; h.ver = R::kVer; h.type = R::RT_LISTREQ; h.slot = R::kNoSlot; h.token = 0;

    // 1) a SHORT request -- the attack shape. Must get nothing back.
    memcpy(buf, &h, R::kHdrLen);
    sendto(s, (const char*)buf, R::kHdrLen, 0, (const sockaddr*)&a, sizeof(a));
    int n = recv(s, (char*)buf, (int)sizeof(buf), 0);
    check(n <= 0, "a SHORT room-list request is ignored (no amplification)");

    // 2) a properly padded one -- the feature still works.
    memcpy(buf, &h, R::kHdrLen);
    memset(buf + R::kHdrLen, 0, R::kListReqMin);
    const int reqLen = R::kHdrLen + R::kListReqMin;
    sendto(s, (const char*)buf, reqLen, 0, (const sockaddr*)&a, sizeof(a));
    n = recv(s, (char*)buf, (int)sizeof(buf), 0);
    check(n > 0, "a PADDED room-list request is answered");
    if (n > 0) {
        char m[128];
        snprintf(m, sizeof(m), "the reply is no bigger than the request (%d <= %d)", n, reqLen);
        check(n <= reqLen, m);
    }
    closesocket(s);
    WSACleanup();
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "client")) {
        Sleep(600);                                     // let the relay bind first
        return runClient(atoi(argv[2]));
    }

    printf("omp_relaytest -- the relay backend, 1 server + 3 clients on loopback :%d\n\n", kPort);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // The relay lives beside this executable: both are build outputs of the same project.
    char dir[MAX_PATH]; strncpy(dir, exe, sizeof(dir) - 1); dir[sizeof(dir) - 1] = 0;
    char* slash = strrchr(dir, '\\'); if (slash) *slash = 0;
    char relayExe[MAX_PATH]; snprintf(relayExe, sizeof(relayExe), "%s\\omp_relay.exe", dir);
    if (GetFileAttributesA(relayExe) == INVALID_FILE_ATTRIBUTES) {
        printf("omp_relay.exe not found beside this test (%s) -- build it first\n", relayExe);
        return 1;
    }
    char relayCmd[MAX_PATH + 32]; snprintf(relayCmd, sizeof(relayCmd), "\"%s\" %d", relayExe, kPort);
    STARTUPINFOA rsi{}; rsi.cb = sizeof(rsi);
    PROCESS_INFORMATION rpi{};
    if (!CreateProcessA(nullptr, relayCmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &rsi, &rpi)) {
        printf("could not launch the relay\n"); return 1;
    }
    Sleep(400);

    PROCESS_INFORMATION cp[2]{};
    bool spawned = true;
    for (int k = 0; k < 2; k++) {
        char c[MAX_PATH + 32]; snprintf(c, sizeof(c), "\"%s\" client %d", exe, k + 2);
        STARTUPINFOA si{}; si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, c, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &cp[k])) {
            printf("could not launch client %d\n", k + 2); spawned = false; break;
        }
    }
    int rc = 1; DWORD c1 = 1, c2 = 1;
    if (spawned) {
        rc = runClient(1);
        WaitForSingleObject(cp[0].hProcess, 60000); GetExitCodeProcess(cp[0].hProcess, &c1);
        WaitForSingleObject(cp[1].hProcess, 60000); GetExitCodeProcess(cp[1].hProcess, &c2);
        for (int k = 0; k < 2; k++) { CloseHandle(cp[k].hThread); CloseHandle(cp[k].hProcess); }
        check(c1 == 0 && c2 == 0, "both other players also passed");
        printf("\n[amplification]\n");
        checkNoAmplification();
    }

    TerminateProcess(rpi.hProcess, 0);
    CloseHandle(rpi.hThread); CloseHandle(rpi.hProcess);

    const bool pass = (spawned && rc == 0 && c1 == 0 && c2 == 0 && !g_fails);
    printf("\n%s\n", pass ? "RELAY TEST PASS" : "*** RELAY TEST FAILURES ***");
    return pass ? 0 : 1;
}
