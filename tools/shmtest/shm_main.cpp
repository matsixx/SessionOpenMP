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
// omp_shmtest -- proves the SHARED-MEMORY backend across two REAL processes, with no game involved.
// Run it with no arguments: it relaunches itself as a child, both halves claim a mailbox slot, and each
// streams packed States at the other while checking what arrives.
//
// The same-PC rig is the only configuration one person can test alone, so it has to be verifiable
// alone too. This checks the properties that matter for a transform stream:
//   * both processes claim DIFFERENT slots (claiming the same slot is a real failure mode)
//   * each side DISCOVERS the other with no ids exchanged (identity is the slot)
//   * payloads survive the seqlock intact (magic + a checksum field, every packet)
//   * LATEST-WINS: a fast writer against a slow reader must never produce a torn or stale-mixed frame
//   * a departed process is noticed (state 5), which is what stops a proxy being driven by a corpse
#include "transport/transport.h"
#include "replication/replication.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>

using namespace omp;

static int  g_fails = 0;
static void check(bool ok, const char* what) {
    printf("  %-56s %s\n", what, ok ? "PASS" : "*** FAIL");
    if (!ok) g_fails++;
}

// A recognisable State: deckPos.x carries a counter so the receiver can prove ordering/freshness.
static void makeState(repl::State& s, float tag, uint32_t seq) {
    s = repl::State{};
    s.bodyPosOk = 1; s.bodyPos[0] = tag; s.bodyPos[1] = (float)seq;
    s.deckPos[0] = tag; s.deckPos[1] = (float)seq; s.deckPos[2] = -60.f;
    s.onBoard = 1; s.grounded = 1;
    s.animLen = 0;
}

static int runSide(const char* label, float myTag, float theirTag) {
    char log[64]; snprintf(log, sizeof(log), "omp_shmtest_%s.log", label);
    if (!Init(BK_SHM, log, false)) { printf("[%s] shm init FAILED\n", label); return 1; }
    printf("[%s] id=%s\n", label, MyId());
    if (!LobbyHost()) { printf("[%s] session open FAILED\n", label); return 1; }

    uint32_t sent = 0, recvd = 0, bad = 0, regressions = 0;
    uint32_t lastSeq = 0;
    bool     sawPeer = false;

    // The RELIABLE channel. Cosmetics send a pair of packets in one frame, and on a latest-wins
    // mailbox the second silently destroys the first while snapshots still look perfect. This half of
    // the test sends reliable messages in same-frame BURSTS -- the shape that breaks -- and demands
    // that every one arrives, in order.
    uint32_t relSent = 0, relRecvd = 0, relBad = 0, relOutOfOrder = 0, relLastSeq = 0;
    struct Ctx { uint32_t *recvd, *bad, *regressions, *lastSeq; float theirTag;
                 uint32_t *relRecvd, *relBad, *relOutOfOrder, *relLastSeq; };
    Ctx ctx{ &recvd, &bad, &regressions, &lastSeq, theirTag,
             &relRecvd, &relBad, &relOutOfOrder, &relLastSeq };

    auto onRecv = [](int peer, const uint8_t* d, int len, void* user) {
        Ctx* c = (Ctx*)user;
        // Reliable messages are tagged 'RELI' + a sequence; anything else is a snapshot.
        if (len >= 8 && !memcmp(d, "RELI", 4)) {
            uint32_t seq = 0; memcpy(&seq, d + 4, 4);
            if (seq != *c->relLastSeq + 1) (*c->relOutOfOrder)++;   // gaps AND reordering both fail
            *c->relLastSeq = seq;
            (*c->relRecvd)++;
            return;
        }
        repl::State s; uint64_t su = 0;
        if (!repl::Unpack(d, len, s, &su)) { (*c->bad)++; return; }
        if (s.bodyPos[0] != c->theirTag) { (*c->bad)++; return; }      // wrong sender's payload
        const uint32_t seq = (uint32_t)s.bodyPos[1];
        if (seq < *c->lastSeq) (*c->regressions)++;                    // latest-wins: never go backwards
        *c->lastSeq = seq;
        (*c->recvd)++;
        (void)peer;
    };

    // ~3 s at 60 Hz. The child is deliberately SLOWER so the parent overruns it: latest-wins must hold.
    const int frames = 180;
    const bool slow = (myTag > 1.5f);
    for (int i = 0; i < frames; i++) {
        const int n = PeerCount();
        if (n > 0) sawPeer = true;
        repl::State s; makeState(s, myTag, ++sent);
        uint8_t pkt[900];
        const int len = repl::Pack(s, (uint64_t)i * 16667, pkt, sizeof(pkt));
        for (int p = 0; p < n; p++) Send(p, pkt, len, false);
        // Every 20th frame, a BURST of 3 reliable messages in the SAME frame, interleaved with the
        // 60 Hz snapshot above -- what cosmetics do, only harder.
        if (n > 0 && (i % 20) == 0) {
            for (int k = 0; k < 3; k++) {
                uint8_t rel[64];
                memcpy(rel, "RELI", 4);
                const uint32_t seq = ++relSent;
                memcpy(rel + 4, &seq, 4);
                memset(rel + 8, (int)(seq & 0xff), sizeof(rel) - 8);
                for (int p = 0; p < n; p++) Send(p, rel, (int)sizeof(rel), true);
            }
        }
        Tick(onRecv, &ctx);
        Sleep(slow ? 33 : 16);
    }
    // Drain until the peer's bursts have all landed. The two sides deliberately run at DIFFERENT frame
    // rates (that is how latest-wins is stress-tested), so the fast side finishes its loop while the
    // slow one is still sending -- without this wait the fast side would "miss" messages that were
    // never sent yet, and the assertion below would be measuring the test's own timing, not the wire.
    for (int i = 0; i < 500 && relRecvd < relSent; i++) { Tick(onRecv, &ctx); Sleep(16); }

    printf("\n[%s] results\n", label);
    check(sawPeer, "discovered the other process (no ids exchanged)");
    check(recvd > 40, "received a healthy stream");
    check(bad == 0, "every packet unpacked and came from the expected sender");
    check(regressions == 0, "no stale/torn frame (latest-wins held)");
    printf("      sent=%u recvd=%u bad=%u regressions=%u lastSeq=%u\n", sent, recvd, bad, regressions, lastSeq);
    // The peer sent the same burst pattern, so the received count must equal the sent count exactly.
    check(relRecvd == relSent, "EVERY reliable message arrived (same-frame bursts, none overwritten)");
    check(relOutOfOrder == 0, "reliable messages arrived in order, no gaps");
    printf("      reliable: sent=%u recvd=%u outOfOrder=%u\n", relSent, relRecvd, relOutOfOrder);

    // ---- departure: the parent outlives the child, so only the parent can observe it.
    if (myTag < 1.5f) {
        printf("\n[%s] waiting for the child to exit, then checking departure\n", label);
        bool departed = false;
        for (int i = 0; i < 120 && !departed; i++) {
            Tick(onRecv, &ctx);
            PeerStats ps;
            for (int p = 0; p < PeerCount(); p++) if (GetStats(p, &ps) && ps.state == 5) departed = true;
            Sleep(50);
        }
        check(departed, "noticed the other process EXIT (peer marked departed)");
    }

    Shutdown();
    return g_fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return runSide("child", 2.0f, 1.0f);

    printf("omp_shmtest -- shared-memory backend across two processes\n\n");
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char cmd[MAX_PATH + 16]; snprintf(cmd, sizeof(cmd), "\"%s\" child", exe);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("could not launch the child process\n"); return 1;
    }
    const int rc = runSide("parent", 1.0f, 2.0f);
    DWORD childRc = 1;
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, &childRc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    check(childRc == 0, "the child process also passed");
    printf("\n%s\n", (rc == 0 && childRc == 0 && !g_fails) ? "SHM TEST PASS" : "*** SHM TEST FAILURES ***");
    return (rc == 0 && childRc == 0 && !g_fails) ? 0 : 1;
}
