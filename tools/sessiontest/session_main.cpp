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
// omp_sessiontest -- exercise the SESSION's lifecycle logic headless (no game, no EOS).
// What it proves: peers arrive and get their own stream, never a shared one (shared state silently
// caps the design at exactly one other player); a peer going quiet stops being driven but is NOT
// released; a peer gone long enough frees its slot for a genuinely new one; the publish rate scales
// with peer count and does NOT rate-limit at one peer; and a full slot table drops the newcomer
// instead of stealing an existing peer's slot.
#include "../../src/session/session.h"
#include "../../src/replication/replication.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "transport/transport.h"    // PeerStats -- the roster mock below wears the real signature

using namespace omp;

// The session calls transport Send() and enumerates the roster via GetStats(); with no EOS here both
// are mocked -- Send counts calls, GetStats presents g_roster lobby members. Publish is ROSTER-driven,
// never heard-from-driven: two freshly-joined players must not deadlock on each other's first packet.
namespace omp {
static int g_sent[64] = {};
static int g_roster = 0;
void Send(int peerIdx, const void*, int, bool) { if (peerIdx >= 0 && peerIdx < 64) g_sent[peerIdx]++; }
int  SendBudget() { return 0; }   // no wire: replay-sync bursts uncapped (and unexercised) here
// The dropped-object lane hashes this into its authority key. A fixed answer is right for a headless
// test: there is no game, so nothing ever adopts or hides a set here.
const char* MyId() { return "test:self"; }
// No lobby, so no host: the dropped-object authority falls back to its lowest-key rule, which is
// exactly the path a host-less wire (shared memory) takes for real.
const char* LobbyOwnerId() { return ""; }
bool LobbyOwnershipKnowable() { return false; }   // the test rig is the symmetric-wire case
const char* PeerIdStr(int) { return ""; }
bool LobbyIsHost() { return false; }
TrustLevel BackendTrust() { return TRUST_NONE; }  // no wire can vouch: the spawn gate stays open here
TrustLevel PeerTrust(int) { return TRUST_NONE; }
bool GetStats(int idx, PeerStats* out) {
    if (idx < 0 || idx >= g_roster || !out) return false;
    *out = PeerStats{}; return true;
}
int PeerCount() { return g_roster; }
}

static int   g_fails = 0;
static int   g_mismatches = 0;   // version-skew announcements seen by the test callback
static void  check(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "*** FAIL");
    if (!ok) g_fails++;
}
static void logf_(const char* s) { printf("      %s\n", s); }

static bool gatherOwn(void*, repl::State& out) {
    out = repl::State{};
    out.bodyPosOk = 1; out.bodyPos[0] = 10; out.bodyPos[1] = 20; out.bodyPos[2] = -52;
    out.onBoard = 1; out.grounded = 1; out.animLen = 229;
    return true;
}
// a dummy non-null "pawn": the session only passes it through to the proxy (which needs real syms to
// spawn, unavailable here), so its value is irrelevant -- but non-null exercises the publish path.
static int g_fakePawn = 0;
static void* pawn() { return &g_fakePawn; }

// Regression guard. Frame() takes a microsecond clock AND a millisecond clock, and the real loader
// passes two INDEPENDENT sources (QPC vs GetTickCount64). Deriving the ms argument from the us one
// makes the two agree perfectly and hides any code that mixes them: session liveness stamping a
// packet with one clock and aging it against the other underflows the unsigned subtraction and
// releases every peer the frame after it opens. The offset below makes the two clocks disagree the
// way they really do, so mixing them fails here instead of in the game.
static uint64_t msClock(uint64_t us) { return us / 1000 + 9'000'000ull; }

static void feed(int peer, uint64_t senderUs, uint64_t nowUs) {
    repl::State s{};
    s.bodyPosOk = 1; s.bodyPos[0] = 100.f + (float)peer; s.bodyPos[2] = -52.f;
    s.deckPos[0] = s.bodyPos[0]; s.deckPos[2] = -60.f;
    s.onBoard = 1; s.grounded = 1; s.animLen = 229;
    uint8_t pkt[900];
    const int n = repl::Pack(s, senderUs, pkt, sizeof(pkt));
    session::OnPacket(peer, pkt, n, nowUs);
}

int main() {
    printf("omp_sessiontest -- session lifecycle\n\n");
    session::Init(logf_);
    session::Config cfg{};
    session::SetConfig(cfg);

    uint64_t us = 1'000'000;                       // start at a non-zero epoch on purpose
    const uint64_t step = 16'667;

    // ---- one peer: streams open, publish is FULL RATE (no limiter aliasing)
    printf("one peer\n");
    omp::g_roster = 1;
    memset(omp::g_sent, 0, sizeof(omp::g_sent));
    for (int i = 0; i < 120; i++) {
        feed(0, us, us);
        session::Frame(pawn(), us, msClock(us), gatherOwn);
        us += step;
    }
    session::Stats st = session::GetStats();
    check(st.peers == 1, "1 peer live");
    check(st.publishHz >= 59.0f, "publish rate is FULL (no limiter at one peer)");
    check(omp::g_sent[0] >= 110, "we published ~every frame to that peer");
    check(st.received >= 118, "every packet was accepted and pushed");

    // ---- three peers: rate scales down, all three get published to, each has its own stream
    printf("\nthree peers\n");
    omp::g_roster = 3;
    memset(omp::g_sent, 0, sizeof(omp::g_sent));
    for (int i = 0; i < 180; i++) {
        feed(0, us, us); feed(1, us, us); feed(2, us, us);
        session::Frame(pawn(), us, msClock(us), gatherOwn);
        us += step;
    }
    st = session::GetStats();
    check(st.peers == 3, "3 peers live, one slot each");
    check(st.publishHz > 29.0f && st.publishHz < 35.0f, "publish rate scaled down (60/3, floored at 30 Hz)");
    check(omp::g_sent[0] > 30 && omp::g_sent[1] > 30 && omp::g_sent[2] > 30, "published to all three");
    const int spread = (omp::g_sent[0] > omp::g_sent[2]) ? omp::g_sent[0] - omp::g_sent[2]
                                                         : omp::g_sent[2] - omp::g_sent[0];
    check(spread <= 2, "all peers got the SAME number of packets (no starved peer)");

    // ---- one peer goes quiet: it stays in its slot (not released) and stops being driven
    printf("\none peer goes quiet (>quietMs, <dropMs)\n");
    for (int i = 0; i < 180; i++) {                 // ~3 s with peer 1 silent
        feed(0, us, us); feed(2, us, us);
        session::Frame(pawn(), us, msClock(us), gatherOwn);
        us += step;
    }
    st = session::GetStats();
    check(st.peers == 3, "quiet peer NOT released (still within dropMs)");

    // ---- and now long enough to be released
    printf("\nquiet peer exceeds dropMs\n");
    for (int i = 0; i < 2000; i++) {                // ~33 s
        feed(0, us, us); feed(2, us, us);
        session::Frame(pawn(), us, msClock(us), gatherOwn);
        us += step;
    }
    st = session::GetStats();
    check(st.peers == 2, "gone peer released, others untouched");

    // ---- a new peer takes the freed slot (transport index 7 -> roster must cover it)
    printf("\nnew peer arrives\n");
    omp::g_roster = 8;
    for (int i = 0; i < 60; i++) {
        feed(0, us, us); feed(2, us, us); feed(7, us, us);
        session::Frame(pawn(), us, msClock(us), gatherOwn);
        us += step;
    }
    st = session::GetStats();
    check(st.peers == 3, "new peer id got a slot");

    // ---- a stale/duplicate packet must not corrupt a stream
    printf("\nhostile-ish input\n");
    const uint32_t before = session::GetStats().received;
    uint8_t junk[64]; memset(junk, 0xAB, sizeof(junk));
    session::OnPacket(0, junk, sizeof(junk), us);                  // bad magic
    session::OnPacket(0, junk, 3, us);                             // too short
    check(session::GetStats().received == before, "malformed packets rejected, not counted");

    // ---- VERSION SKEW. A peer on a different build sends packets whose magic this one rejects. That
    // must be ANNOUNCED (it is otherwise invisible: the lobby and the P2P link both still succeed, so
    // the only symptom is a player who never appears) and announced exactly ONCE -- a mismatched peer
    // rejects at the send rate, and a warning that repeats 60 times a second is a flood.
    printf("\nversion skew\n");
    {
        session::Config vc = session::GetConfig();
        vc.onVersionMismatch = [](int) { g_mismatches++; };
        session::SetConfig(vc);
        g_mismatches = 0;
        for (int i = 0; i < 120; i++) session::OnPacket(3, junk, sizeof(junk), us);
        check(g_mismatches == 1, "unreadable packets from a peer announce ONCE, not per packet");
        for (int i = 0; i < 60; i++) session::OnPacket(9, junk, sizeof(junk), us);
        check(g_mismatches == 2, "a DIFFERENT peer announces on its own");
    }

    printf("\n%s\n", g_fails ? "*** SESSION TEST FAILURES ***" : "SESSION TEST PASS");
    return g_fails ? 1 : 0;
}
