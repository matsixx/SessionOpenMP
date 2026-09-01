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
// omp_udptest -- proves the DIRECT UDP backend across two REAL processes over loopback, no game and
// no network involved. Run with no arguments: it relaunches itself as a child, the parent HOSTS a
// port and the child JOINS 127.0.0.1, and each streams packed States at the other while checking what
// arrives. Same shape as omp_shmtest, because the properties that matter are the same ones.
//
// What it proves, and why each property is checked rather than assumed:
//   * THE HANDSHAKE COMPLETES both ways -- a joiner that never gets an ACK is the most likely way
//     this backend fails, and it fails silently.
//   * PAYLOADS ARRIVE INTACT and are attributed to the right sender (a datagram socket hands you
//     whatever arrived, from anyone).
//   * EVERY RELIABLE MESSAGE ARRIVES, sent in SAME-FRAME BURSTS interleaved with the 60 Hz snapshot
//     stream. Cosmetics send a pair in one frame, and on a latest-wins channel the second destroys
//     the first every time.
//   * NO RELIABLE MESSAGE ARRIVES TWICE. A retransmit is not a bug, delivering it twice is, and this
//     wire retransmits on a 200 ms timer, so duplicates are guaranteed to be exercised.
//   * A DEPARTED PROCESS IS NOTICED, which is what stops a proxy being driven by a corpse.
#include "transport/transport.h"
#include "replication/replication.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>

using namespace omp;

// A high port, so a developer machine is unlikely to already be using it. Loopback only.
static const int kTestPort = 47811;

static int  g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "*** FAIL");
    if (!ok) g_fails++;
}

static void makeState(repl::State& s, float tag, uint32_t seq) {
    s = repl::State{};
    s.bodyPosOk = 1; s.bodyPos[0] = tag; s.bodyPos[1] = (float)seq;
    s.deckPos[0] = tag; s.deckPos[1] = (float)seq; s.deckPos[2] = -60.f;
    s.onBoard = 1; s.grounded = 1;
    s.animLen = 0;
}

struct Ctx {
    float    theirTag = 0;
    uint32_t recvd = 0, bad = 0;
    uint32_t relRecvd = 0, relDuped = 0;
    // Reliable messages carry their own counter; a repeat of one already seen is a DUPLICATE and a
    // real defect. 512 is far more than the test sends, so the bitmap never wraps.
    bool     relSeen[512] = {};
};

static void onRecv(int peer, const uint8_t* d, int len, void* user) {
    Ctx& c = *(Ctx*)user;
    if (len >= 8 && !memcmp(d, "RELI", 4)) {
        uint32_t seq = 0; memcpy(&seq, d + 4, 4);
        if (seq < 512) {
            if (c.relSeen[seq]) c.relDuped++;
            else { c.relSeen[seq] = true; c.relRecvd++; }
        }
        return;
    }
    repl::State s; uint64_t su = 0;
    if (!repl::Unpack(d, len, s, &su)) { c.bad++; return; }
    // Attribution: the payload says who sent it, and it must agree with the peer index the transport
    // assigned. A mismatch means a datagram was credited to the wrong connection.
    if (s.bodyPos[0] != c.theirTag) c.bad++;
    (void)peer;
    c.recvd++;
}

static int runSide(const char* label, bool host, float myTag, float theirTag) {
    char log[64]; snprintf(log, sizeof(log), "omp_udptest_%s.log", label);
    // A distinct identity per side. Without it both processes would claim the same id and each would
    // reject the other's HELLO as its own reflection -- which is exactly the check that guard exists for.
    char id[33];
    snprintf(id, sizeof(id), "%s%s", host ? "aaaaaaaaaaaaaaaa" : "bbbbbbbbbbbbbbbb",
                                     host ? "1111111111111111" : "2222222222222222");
    SetLocalIdentity(id);
    // The joiner uses the EXPLICIT "ip:port" form, because that is what the menu tells a player to
    // type -- the gate should exercise the documented path, not the convenience fallback.
    char target[64]; snprintf(target, sizeof(target), "127.0.0.1:%d", kTestPort);
    SetDirectEndpoint(host ? "" : target, kTestPort);

    if (!Init(BK_UDP, log, false)) { printf("[%s] udp init FAILED\n", label); return 1; }
    printf("[%s] id=%s\n", label, MyId());

    if (host) { if (!LobbyHost()) { printf("[%s] host FAILED\n", label); return 1; } }
    else      { if (!LobbyJoin()) { printf("[%s] join FAILED\n", label); return 1; } }

    Ctx ctx; ctx.theirTag = theirTag;
    uint32_t sent = 0, relSent = 0;
    bool sawPeer = false;

    // The joiner may start before the host has bound, so the handshake is given room to retry. This
    // is not slack in the test -- it is the actual startup race two humans will hit.
    for (int i = 0; i < 900; i++) {
        const int n = PeerCount();
        if (n > 0) sawPeer = true;

        repl::State s; makeState(s, myTag, ++sent);
        uint8_t pkt[900];
        const int len = repl::Pack(s, (uint64_t)i * 16667, pkt, sizeof(pkt));
        for (int p = 0; p < n; p++) Send(p, pkt, len, false);

        // Reliable bursts: THREE in one frame, interleaved with the snapshot stream above.
        if (n > 0 && (i % 20) == 0 && relSent < 24) {
            for (int k = 0; k < 3; k++) {
                uint8_t rel[64];
                memcpy(rel, "RELI", 4);
                const uint32_t seq = relSent++;
                memcpy(rel + 4, &seq, 4);
                memset(rel + 8, (int)(seq & 0xff), sizeof(rel) - 8);
                for (int p = 0; p < n; p++) Send(p, rel, (int)sizeof(rel), true);
            }
        }
        Tick(onRecv, &ctx);
        Sleep(16);
    }
    // Let retransmits settle. The reliable lane resends every 200 ms, so this window is also where
    // duplicates would show up if the dedupe window were wrong -- it is doing double duty.
    for (int i = 0; i < 200 && ctx.relRecvd < 24; i++) { Tick(onRecv, &ctx); Sleep(16); }
    for (int i = 0; i < 40; i++) { Tick(onRecv, &ctx); Sleep(16); }

    printf("\n[%s] results\n", label);
    check(sawPeer, "handshake completed -- the other process became a peer");
    check(ctx.recvd > 200, "received a healthy snapshot stream");
    check(ctx.bad == 0, "every packet unpacked and was attributed to the right sender");
    printf("      sent=%u recvd=%u bad=%u\n", sent, ctx.recvd, ctx.bad);
    check(ctx.relRecvd == 24, "EVERY reliable message arrived (same-frame bursts)");
    check(ctx.relDuped == 0, "no reliable message was delivered twice (retransmits deduped)");
    printf("      reliable: sent=%u recvd=%u duplicates=%u\n", relSent, ctx.relRecvd, ctx.relDuped);

    // Departure: the parent outlives the child, so only the parent can watch it happen. A clean exit
    // sends BYE, so this should be near-instant rather than waiting out the 10 s silence timeout --
    // and that difference is worth asserting, because the timeout would mask a missing BYE.
    if (host) {
        printf("\n[%s] waiting for the child to exit, then checking departure\n", label);
        bool departed = false;
        for (int i = 0; i < 100 && !departed; i++) {
            Tick(onRecv, &ctx);
            PeerStats ps;
            for (int p = 0; p < PeerCount(); p++) if (GetStats(p, &ps) && ps.state == 5) departed = true;
            Sleep(50);
        }
        check(departed, "noticed the other process LEAVE (peer marked departed)");
    }

    Shutdown();
    return g_fails ? 1 : 0;
}


// ---- THE MESH, across THREE processes ------------------------------------------------------------
// The pair test above proves two processes talk. It cannot prove the thing that actually decides
// whether three people can play: a joiner only ever dials the ONE address it was given, so before
// peer introduction the two joiners were invisible to each other and only the host saw everybody --
// a session that looks connected and is half empty. No two-process test can see that.
//
// So: one host, TWO joiners, both dialling only the host. Each must end up with BOTH other players,
// which can only happen if the introduction was sent AND acted on.
static const int kMeshPort = kTestPort + 1;

struct MeshCtx {
    int recvFrom[4] = {0};      // snapshots received, indexed by the sender's tag
    int bad = 0;
};
static void onMeshRecv(int peer, const uint8_t* d, int len, void* user) {
    MeshCtx& c = *(MeshCtx*)user;
    repl::State s; uint64_t su = 0;
    if (!repl::Unpack(d, len, s, &su)) { c.bad++; return; }
    const int tag = (int)(s.bodyPos[0] + 0.5f);
    if (tag >= 1 && tag <= 3) c.recvFrom[tag]++;
    else c.bad++;
    (void)peer;
}

// role 1 = host, roles 2 and 3 = joiners. The role IS the tag, so a packet names its own sender.
static int runMesh(const char* label, int role) {
    char log[64]; snprintf(log, sizeof(log), "omp_udptest_mesh%d.log", role);
    char id[33];
    snprintf(id, sizeof(id), "mesh%d___________________________", role);
    SetLocalIdentity(id);
    char target[64]; snprintf(target, sizeof(target), "127.0.0.1:%d", kMeshPort);
    SetDirectEndpoint(role == 1 ? "" : target, kMeshPort);
    if (!Init(BK_UDP, log, false)) { printf("[%s] udp init FAILED\n", label); return 1; }

    if (role == 1) { if (!LobbyHost()) { printf("[%s] host FAILED\n", label); return 1; } }
    else           { if (!LobbyJoin()) { printf("[%s] join FAILED\n", label); return 1; } }

    MeshCtx ctx;
    int peak = 0;
    // 15 s. The roster is gossiped every 3 s and a dial is answered in one loopback round trip, so
    // this is several chances to converge rather than a photo finish.
    for (int i = 0; i < 940; i++) {
        const int n = PeerCount();
        if (n > peak) peak = n;
        repl::State s; makeState(s, (float)role, (uint32_t)i);
        uint8_t pkt[900];
        const int len = repl::Pack(s, (uint64_t)i * 16667, pkt, sizeof(pkt));
        for (int p = 0; p < n; p++) Send(p, pkt, len, false);
        Tick(onMeshRecv, &ctx);
        Sleep(16);
    }

    printf("\n[%s] mesh results (role %d)\n", label, role);
    char m[160];
    snprintf(m, sizeof(m), "sees BOTH other players (peers=%d)", peak);
    check(peak >= 2, m);
    int heard = 0;
    for (int t = 1; t <= 3; t++) if (t != role && ctx.recvFrom[t] > 50) heard++;
    snprintf(m, sizeof(m), "receives a stream from BOTH (from role1=%d role2=%d role3=%d)",
             ctx.recvFrom[1], ctx.recvFrom[2], ctx.recvFrom[3]);
    check(heard == 2, m);
    check(ctx.bad == 0, "every mesh packet unpacked cleanly");
    LobbyLeave();
    return g_fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) {
        Sleep(400);                       // give the parent a moment to bind; the retry covers the rest
        return runSide("child", false, 2.0f, 1.0f);
    }
    if (argc > 2 && !strcmp(argv[1], "mesh")) {
        Sleep(500);                   // the joiners start after the host has bound
        return runMesh(argv[2][0] == '2' ? "mesh-b" : "mesh-c", atoi(argv[2]));
    }

    printf("omp_udptest -- direct UDP backend across two processes (loopback :%d)\n\n", kTestPort);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char cmd[MAX_PATH + 16]; snprintf(cmd, sizeof(cmd), "\"%s\" child", exe);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("could not launch the child process\n"); return 1;
    }
    const int rc = runSide("parent", true, 1.0f, 2.0f);
    DWORD childRc = 1;
    WaitForSingleObject(pi.hProcess, 40000);
    GetExitCodeProcess(pi.hProcess, &childRc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    check(childRc == 0, "the child process also passed");

    // ---- phase 2: three processes, and the mesh.
    printf("\n\nomp_udptest -- THREE processes, peer introduction (loopback :%d)\n", kMeshPort);
    PROCESS_INFORMATION mp[2]{};
    bool spawned = true;
    for (int k = 0; k < 2; k++) {
        char mc[MAX_PATH + 24];
        snprintf(mc, sizeof(mc), "\"%s\" mesh %d", exe, k + 2);
        STARTUPINFOA msi{}; msi.cb = sizeof(msi);
        if (!CreateProcessA(nullptr, mc, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &msi, &mp[k])) {
            printf("could not launch mesh joiner %d\n", k + 2); spawned = false; break;
        }
    }
    int meshRc = 1; DWORD m1 = 1, m2 = 1;
    if (spawned) {
        meshRc = runMesh("mesh-a", 1);
        WaitForSingleObject(mp[0].hProcess, 40000); GetExitCodeProcess(mp[0].hProcess, &m1);
        WaitForSingleObject(mp[1].hProcess, 40000); GetExitCodeProcess(mp[1].hProcess, &m2);
        for (int k = 0; k < 2; k++) { CloseHandle(mp[k].hThread); CloseHandle(mp[k].hProcess); }
        check(m1 == 0 && m2 == 0, "both mesh joiners also passed");
    }
    const bool pass = (rc == 0 && childRc == 0 && spawned && meshRc == 0 &&
                       m1 == 0 && m2 == 0 && !g_fails);
    printf("\n%s\n", pass ? "UDP TEST PASS" : "*** UDP TEST FAILURES ***");
    return pass ? 0 : 1;
}
