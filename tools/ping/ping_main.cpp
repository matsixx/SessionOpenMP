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
// omp_ping -- prove the transport on its own, on a real link, without the game running.
// Each side runs this exe and swaps the printed id with the other player; the tool then measures EACH
// DIRECTION separately at 60 Hz. A link can be perfect one way and dead the other, which no symmetric
// "ping" number can express, so the two directions are always reported apart. Usage:
//     omp_ping [peerId] [--relay]
// --relay forces Epic relay routing: if direct fails one way but relayed is clean both ways, the NAT
// is at fault and ForceRelays is the right default for that pairing.
#include "../../src/transport/transport.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod

#pragma pack(push, 1)
struct PingPkt {
    uint32_t magic;      // 'OMPP'
    uint8_t  type;       // 0 = ping, 1 = pong
    uint8_t  pad[3];
    uint32_t seq;
    uint64_t sendUs;     // sender clock at send (echoed in the pong)
};
#pragma pack(pop)
static const uint32_t kMagic = 0x50504D4Fu; // "OMPP"

static uint64_t nowUs() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

// ---- inbound (their->me) health: sequence gaps = loss, inter-arrival variance = jitter
static uint32_t g_rxLastSeq = 0, g_rxCount = 0, g_rxLost = 0;
static uint64_t g_rxLastUs = 0; static double g_rxJitEwma = 0;
// ---- outbound (me->them) health, via pongs: rtt + pong loss
static uint32_t g_pongCount = 0; static double g_rttEwma = -1;
static std::atomic<int> g_peerIdx{-1};

static void onRecv(int peer, const uint8_t* d, int len, void*) {
    if (len < (int)sizeof(PingPkt)) return;
    PingPkt p; memcpy(&p, d, sizeof(p));
    if (p.magic != kMagic) return;
    if (g_peerIdx.load() < 0) g_peerIdx = peer;              // first contact locks the peer
    if (p.type == 0) {                                       // their ping -> count + echo
        const uint64_t now = nowUs();
        if (g_rxCount && p.seq > g_rxLastSeq + 1) g_rxLost += p.seq - g_rxLastSeq - 1;
        if (g_rxLastUs) {
            const double gapMs = (double)(now - g_rxLastUs) / 1000.0;
            const double dev = fabs(gapMs - 16.7);
            g_rxJitEwma += (dev - g_rxJitEwma) * 0.05;
        }
        g_rxLastSeq = p.seq; g_rxLastUs = now; g_rxCount++;
        PingPkt pong = p; pong.type = 1;
        omp::Send(peer, &pong, sizeof(pong), false);
    } else {                                                 // pong of OUR ping -> rtt
        const double rtt = (double)(nowUs() - p.sendUs) / 1000.0;
        g_rttEwma = (g_rttEwma < 0) ? rtt : g_rttEwma + (rtt - g_rttEwma) * 0.1;
        g_pongCount++;
    }
}

int main(int argc, char** argv) {
    bool relay = false; const char* peerArg = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--relay")) relay = true; else peerArg = argv[i];
    }
    // Required: without it Sleep() quantizes to the 15.6 ms scheduler quantum, the send loop runs at
    // ~33 Hz, and the far end reads that sender cadence as ~14 ms of network jitter.
    timeBeginPeriod(1);
    printf("SessionOpenMP ping -- Milestone 1 transport proof%s\n", relay ? "  [FORCE RELAYS]" : "");
    if (!omp::Init(omp::BK_EOS, "omp_ping.log", relay)) { printf("EOS init failed -- see omp_ping.log\n"); return 1; }
    // Init only STARTS sign-in: in the game it shares the SDK with Session's own EOS plugin and must
    // never block the game thread. This tool has no game thread to protect, so it drives the tick
    // until sign-in resolves. `MyId()` below is printed for the operator to paste and does not exist
    // until then.
    printf("  signing in...\n");
    while (omp::InitState() == 1) { omp::Tick(nullptr, nullptr); Sleep(10); }
    if (omp::InitState() != 2) { printf("EOS sign-in failed -- see omp_ping.log\n"); return 1; }
    // This tool has no lobby and ticks the wire while the operator is still pasting the other id, so
    // it needs the learn-on-packet path that the mod deliberately gates off (see transport.h).
    // Opting in explicitly, rather than letting the transport guess, keeps the mod's default safe.
    omp::AllowLearnWithoutSession(true);

    printf("\n  ==============================================================\n");
    printf("  YOUR ID (send this to the other player):\n\n      %s\n", omp::MyId());
    printf("  ==============================================================\n\n");

    std::string peerId;
    if (peerArg) peerId = peerArg;
    else {
        printf("  paste THEIR id and press enter: ");
        std::atomic<bool> got{false};
        std::thread reader([&]{ std::string s; std::getline(std::cin, s);
                                while (!s.empty() && (s.back()=='\r'||s.back()==' ')) s.pop_back();
                                peerId = s; got = true; });
        while (!got) { omp::Tick(onRecv, nullptr); Sleep(10); }   // keep ticking while they type
        reader.join();
    }
    int peer = omp::AddPeer(peerId.c_str());
    if (peer < 0) { printf("bad peer id\n"); return 1; }
    g_peerIdx = peer;

    printf("\n  pinging at 60 Hz -- stats each second; Ctrl+C to stop. Send omp_ping.log back.\n\n");
    uint32_t seq = 0; uint64_t lastSend = 0, lastStats = 0; uint32_t lastSent = 0, lastRx = 0, lastPong = 0;
    for (;;) {
        omp::Tick(onRecv, nullptr);
        const uint64_t now = nowUs();
        if (now - lastSend >= 16667) {                        // 60 Hz, the mod's real cadence
            lastSend = now;
            PingPkt p{ kMagic, 0, {0,0,0}, ++seq, now };
            omp::Send(g_peerIdx.load(), &p, sizeof(p), false);
        }
        if (now - lastStats >= 1000000) {
            lastStats = now;
            omp::PeerStats st{}; omp::GetStats(g_peerIdx.load(), &st);
            const uint32_t rx = g_rxCount - lastRx, tx = seq - lastSent, pong = g_pongCount - lastPong;
            lastRx = g_rxCount; lastSent = seq; lastPong = g_pongCount;
            const char* state = st.state == 1 ? "DIRECT" : st.state == 2 ? "RELAY" :
                                st.state == 3 ? "INTERRUPTED" : st.state == 4 ? "CLOSED" : "connecting";
            omp::Log("[ping] link=%s | THEIR->ME %u/s jitter=%.1fms lostTotal=%u | ME->THEM rtt=%.1fms pongs=%u/s (sent %u/s)",
                     state, rx, g_rxJitEwma, g_rxLost, g_rttEwma, pong, tx);
        }
        Sleep(1);
    }
}
