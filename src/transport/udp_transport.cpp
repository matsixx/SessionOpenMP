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
// SessionOpenMP -- DIRECT UDP transport backend. No Epic, no sign-in, no third party.
//
// WHY: EOS is the convenient route, not the only acceptable one. Some people cannot or will not use
// it, LAN play should not need the internet, and a mod that only works through one company's service
// is one policy change from not working. This backend needs nothing but a socket.
//
// WHAT IT IS AND IS NOT -- read this before assuming a feature exists:
//   * It is DIRECT: the host listens on a port, the joiner is told an address. That covers LAN, a
//     host who can port-forward, and anyone already behind a friendly NAT.
//   * It is NOT hole punching and NOT a relay. Those need a rendezvous server, which is a separate
//     piece of work -- and the same binary that will later be the dedicated server. Nothing here
//     forecloses it: a relay is "send to the relay's address instead", which this already does.
//   * It is NOT encrypted and NOT authenticated. EOS gave both away for free and this cannot. That is
//     a real downgrade, it is the user's informed choice to make, and `Posture()` says so out loud
//     rather than letting the F1 menu imply otherwise.
//
// THE RULES IT FOLLOWS:
//   * IDENTITY IS NOT AN ADDRESS. A NAT rebind changes a peer's address mid-session; if identity were
//     the endpoint, that peer would silently become a stranger and a second proxy would spawn. Every
//     packet therefore carries a 32-bit TOKEN the receiver issued during the handshake, and a known
//     token arriving from a new address MOVES the peer rather than creating one.
//   * PEER INDICES ARE HANDED OUT ONCE AND NEVER REUSED. Session slots are keyed by transport index;
//     reuse aliases two humans onto one proxy.
//   * TWO LANES, and the reliable one is drained COMPLETELY. Snapshots want newest-only; whole
//     messages want every message -- cosmetics send a PAIR of packets in one frame, and a latest-wins
//     channel destroys the first every time.
//   * A DEPARTED PEER IS NOTICED. Silence is the only signal a datagram transport gets, so silence has
//     to count: past `kPeerTimeoutMs` the peer goes to state 5, which is what stops a proxy being
//     driven by a corpse.
#include "transport.h"

#define _CRT_RAND_S
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

// mstcpip.h guards this behind a _WIN32_WINNT level this build does not reach. It is a stable,
// documented control code, so define it rather than moving the whole project's target version for
// one ioctl. (See its use in Init for why a UDP socket needs it at all.)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")

namespace omp { namespace udpb {

// ---- wire framing ------------------------------------------------------------------------------------
// Deliberately tiny and fixed-size. The magic is version-bearing for the same reason the game wire's is:
// two builds that disagree about the layout must REJECT each other, never misparse.
static const uint32_t kMagic  = 0x55504D4Fu;    // "OMPU"
static const uint8_t  kVer    = 1;
static const int      kMaxMsg = 1024;           // matches session.cpp's pack buffer; far under any MTU

enum PktType : uint8_t {
    PT_HELLO = 0,      // + id[32] + token(u32): "here is who I am and how to address me"
    PT_HELLO_ACK = 1,  // the same, in reply
    PT_DATA = 2,       // unreliable payload
    PT_REL = 3,        // + seq(u32) + payload
    PT_ACK = 4,        // + seq(u32)
    PT_BYE = 5,        // a clean goodbye, so the peer does not wait out the timeout
    PT_PING = 6,       // keepalive: holds the NAT mapping open and proves liveness
    PT_PEERS = 7,      // + count(u8) + N x { id[32], ipv4(u32), port(u16) }: "these are the others"
};

#pragma pack(push, 1)
struct Hdr {
    uint32_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint16_t pad;      // keeps the payload 4-byte aligned and leaves room to grow
    uint32_t token;    // the token the RECEIVER issued for this connection. 0 in a HELLO.
};
#pragma pack(pop)
static const int kHdr = (int)sizeof(Hdr);       // 12
static const int kIdLen = 32;

// ---- tuning ------------------------------------------------------------------------------------------
static const int kPeers          = 16;          // matches the EOS backend and the session's slot table
static const int kRelWindow      = 32;          // reliable messages in flight per peer
static const int kRelResendMs    = 200;
static const int kRelGiveUpMs    = 4000;        // past this a message is dropped and SAID
static const int kPingMs         = 1000;
static const int kPeerTimeoutMs  = 10000;
static const int kHelloResendMs  = 300;
static const int kHelloGiveUpMs  = 20000;
static const int kGossipMs       = 3000;        // how often the roster is passed around (see Tick)

// ---- state -------------------------------------------------------------------------------------------
struct RelOut {
    uint32_t seq = 0;
    int      len = 0;
    uint64_t firstMs = 0, lastMs = 0;
    uint8_t  data[kMaxMsg];
    bool     live = false;
};
struct Peer {
    sockaddr_in addr{};
    char        idStr[kIdLen + 1] = {0};
    uint32_t    localToken  = 0;      // WE issued this; their packets carry it so we can find them
    uint32_t    remoteToken = 0;      // THEY issued this; our packets carry it
    PeerStats   st{};
    uint64_t    lastRecvMs = 0, lastSendMs = 0;
    bool        used = false;
    // reliable out
    RelOut      out[kRelWindow];
    uint32_t    nextRelSeq = 1;
    // reliable in: highest delivered + a bitmap of the 64 below it. Enough to dedupe a retransmit
    // storm without keeping a list; a message older than the window is treated as a duplicate, which
    // is the safe direction (a lost message is re-sent, a double-applied one is a bug).
    uint32_t    relHighest = 0;
    uint64_t    relSeen = 0;
};
static Peer   g_peers[kPeers];
static int    g_nPeers = 0;

static SOCKET      g_sock = INVALID_SOCKET;
static bool        g_wsa = false;
static bool        g_inSession = false;
static volatile LONG g_status = 0;              // 0 idle, 1 connecting, 2 hosting, 3 joined, -1 failed
static char        g_myId[kIdLen + 1] = {0};
static char        g_joinAddr[128] = {0};       // what SetDirectEndpoint was told to join
static int         g_listenPort = 0;
// ---- DIALS: every handshake we are currently attempting.
// This used to be one target, because there used to be one: a joiner dialled the host and that was
// the session. The LAN mesh dials everyone, so it has to be a table -- and NOT because one variable
// was untidy. `peerByToken` finds a peer by the token WE issued, so two peers sharing a token would
// route one of them's data to the other. Each dial therefore mints its OWN token, and the ACK adopts
// the token belonging to the dial it answers.
struct Dial {
    sockaddr_in target{};
    char        id[kIdLen + 1] = {0};   // who we EXPECT, when an introduction named them; "" if unknown
    uint32_t    token = 0;              // what we advertised to this target, and nobody else
    uint64_t    startMs = 0, lastMs = 0;
    bool        active = false;
    bool        primary = false;        // the player's own Join. Only its failure is worth a banner:
                                        // an introduced peer that never answers is normal on the
                                        // internet and says nothing about the session you are in.
};
static Dial        g_dials[kPeers];
static bool        g_joining = false;   // is the PRIMARY dial still outstanding (drives LobbyStatus)
static char        g_boundDesc[64] = {0};

static const int kDefaultPort = 7777;

static uint32_t randU32() {
    unsigned v = 0;
    if (rand_s(&v) != 0) v = (unsigned)GetTickCount64() ^ (unsigned)(uintptr_t)&v;
    return v ? (uint32_t)v : 0x9e3779b9u;        // a token of 0 means "none"; never issue it
}
static bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
static void addrStr(const sockaddr_in& a, char* out, int cap) {
    char ip[64] = {0};
    inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
    snprintf(out, (size_t)cap, "%s:%d", ip, (int)ntohs(a.sin_port));
}

static Peer* peerByToken(uint32_t tok) {
    if (!tok) return nullptr;
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].used && g_peers[i].localToken == tok) return &g_peers[i];
    return nullptr;
}
static Peer* peerById(const char* id) {
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].used && !_stricmp(g_peers[i].idStr, id)) return &g_peers[i];
    return nullptr;
}

// A peer is created ONLY by the handshake. There is no learn-on-data path: a datagram from nowhere
// carrying no token is not a player, and treating it as one is how a stray packet becomes a proxy.
// `localToken` = the token WE want them to stamp on packets to us. 0 means "mint a fresh one".
// It is NOT always fresh: a JOINER advertises its token in the opening HELLO, before any Peer exists,
// so when the ACK comes back and the peer is finally created it must ADOPT the token it already
// promised. Minting a new one there leaves the host addressing every packet to a token the joiner no
// longer answers to -- the handshake completes, both sides report a peer, and no data is ever
// delivered.
static int addPeer(const char* id, const sockaddr_in& from, uint32_t localToken) {
    if (g_nPeers >= kPeers) {
        Log("[udp] REFUSED peer %s -- all %d slots used (indices are never reused)", id, kPeers);
        return -1;
    }
    Peer& p = g_peers[g_nPeers];
    p = Peer{};
    p.used = true;
    p.addr = from;
    strncpy_s(p.idStr, id, _TRUNCATE);
    p.localToken = localToken ? localToken : randU32();
    p.lastRecvMs = GetTickCount64();
    p.st.state = 1;                              // a UDP link is direct by definition
    char a[64]; addrStr(from, a, sizeof(a));
    Log("[udp] peer #%d = %s at %s", g_nPeers, p.idStr, a);
    return g_nPeers++;
}

// Start (or refresh) a handshake attempt. Deduped on BOTH id and address: an introduction can name
// somebody we are already talking to, and gossip repeats every few seconds.
static bool startDial(const sockaddr_in& to, const char* id, bool primary) {
    if (id && *id && !_stricmp(id, g_myId)) return false;          // ourselves
    if (id && *id && peerById(id))          return false;          // already connected
    for (int i = 0; i < kPeers; i++) {
        const Dial& d = g_dials[i];
        if (!d.active) continue;
        if (sameAddr(d.target, to)) return false;                  // already dialling that endpoint
        if (id && *id && d.id[0] && !_stricmp(d.id, id)) return false;
    }
    for (int i = 0; i < kPeers; i++) {
        Dial& d = g_dials[i];
        if (d.active) continue;
        d = Dial{};
        d.target = to;
        if (id) strncpy_s(d.id, id, _TRUNCATE);
        d.token = randU32();
        d.startMs = GetTickCount64(); d.lastMs = 0;
        d.active = true; d.primary = primary;
        return true;
    }
    return false;                                                  // table full: nothing to do
}
// The token we promised THIS endpoint. An ACK is answered with the dial it belongs to, never with
// "the token we happen to be using", which is what makes several dials at once safe.
static Dial* dialFor(const sockaddr_in& from) {
    for (int i = 0; i < kPeers; i++)
        if (g_dials[i].active && sameAddr(g_dials[i].target, from)) return &g_dials[i];
    return nullptr;
}
static void clearDials() { for (int i = 0; i < kPeers; i++) g_dials[i] = Dial{}; }

// ---- raw send ----------------------------------------------------------------------------------------
static void sendTo(const sockaddr_in& to, uint8_t type, uint32_t token,
                   const void* body, int bodyLen, const void* body2 = nullptr, int body2Len = 0) {
    if (g_sock == INVALID_SOCKET) return;
    uint8_t buf[kHdr + 8 + kMaxMsg];
    if (bodyLen + body2Len > (int)sizeof(buf) - kHdr) return;
    Hdr h{}; h.magic = kMagic; h.ver = kVer; h.type = type; h.pad = 0; h.token = token;
    memcpy(buf, &h, kHdr);
    int n = kHdr;
    if (body  && bodyLen  > 0) { memcpy(buf + n, body,  (size_t)bodyLen);  n += bodyLen;  }
    if (body2 && body2Len > 0) { memcpy(buf + n, body2, (size_t)body2Len); n += body2Len; }
    sendto(g_sock, (const char*)buf, n, 0, (const sockaddr*)&to, (int)sizeof(to));
}

// HELLO / HELLO_ACK body: our id, then the token WE want them to put in their packets.
static void sendHello(const sockaddr_in& to, uint8_t type, uint32_t ourToken) {
    uint8_t body[kIdLen + 4];
    memcpy(body, g_myId, kIdLen);
    memcpy(body + kIdLen, &ourToken, 4);
    sendTo(to, type, 0, body, sizeof(body));     // token 0: they cannot know ours yet
}

// ---- the seam ----------------------------------------------------------------------------------------
void SetLocalIdentity(const char* id) {
    if (!id || !*id) return;
    strncpy_s(g_myId, id, _TRUNCATE);
}
void SetDirectEndpoint(const char* joinAddress, int listenPort) {
    if (joinAddress) strncpy_s(g_joinAddr, joinAddress, _TRUNCATE);
    g_listenPort = listenPort;
}
const char* DirectEndpoint() { return g_joinAddr; }
const char* BoundDescription() { return g_boundDesc; }

// "1.2.3.4:7777", "host.name:7777", or a bare "1.2.3.4".
// A BARE ADDRESS TAKES THE CONFIGURED PORT, not the default. The F1 panel puts a port box right next
// to the address box, so someone who sets the port to 9000 and types a bare address means 9000;
// silently dialing 7777 instead produces "no answer from an address that is obviously right", which
// is the least debuggable failure this backend can have.
static bool parseAddr(const char* s, sockaddr_in* out) {
    if (!s || !*s) return false;
    char host[128] = {0};
    int  port = (g_listenPort > 0) ? g_listenPort : kDefaultPort;
    const char* colon = strrchr(s, ':');
    if (colon && colon != s) {
        const size_t n = (size_t)(colon - s);
        if (n >= sizeof(host)) return false;
        memcpy(host, s, n); host[n] = 0;
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return false;
    } else {
        strncpy_s(host, s, _TRUNCATE);
    }
    // Trim whitespace a human pasting an address will bring with them.
    char* b = host; while (*b == ' ' || *b == '\t') b++;
    char* e = b + strlen(b); while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    if (!*b) return false;

    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(b, nullptr, &hints, &res) != 0 || !res) return false;
    memset(out, 0, sizeof(*out));
    *out = *(sockaddr_in*)res->ai_addr;
    out->sin_port = htons((u_short)port);
    freeaddrinfo(res);
    return true;
}

bool Init(bool /*forceRelays*/) {
    if (!g_wsa) {
        WSADATA wd{};
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { Log("[udp] WSAStartup failed"); return false; }
        g_wsa = true;
    }
    if (!g_myId[0]) {
        // Better than refusing: a session with a clock-derived id still works, it is just not stable
        // across launches. Said loudly because it means the loader forgot to call SetLocalIdentity.
        snprintf(g_myId, sizeof(g_myId), "%08x%08x%08x%08x", randU32(), randU32(), randU32(), randU32());
        Log("[udp] *** no local identity was set -- using a throwaway one (%s)", g_myId);
    }
    if (g_sock == INVALID_SOCKET) {
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET) { Log("[udp] socket() failed: %d", WSAGetLastError()); return false; }
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
        // Without this a single ICMP "port unreachable" (which is what a host that is not listening
        // replies with) makes every later recvfrom fail with WSAECONNRESET, and the socket looks dead
        // for reasons that have nothing to do with the packet being read.
        BOOL reset = FALSE; DWORD got = 0;
        WSAIoctl(g_sock, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &got, nullptr, nullptr);
    }
    Log("[udp] ready -- id %s", g_myId);
    return true;
}

static void closeSocket() {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    g_boundDesc[0] = 0;
}
void Deactivate() {
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].used) sendTo(g_peers[i].addr, PT_BYE, g_peers[i].remoteToken, nullptr, 0);
    g_inSession = false; g_joining = false;
    InterlockedExchange(&g_status, 0);
    for (int i = 0; i < g_nPeers; i++) g_peers[i].st.state = 5;
    closeSocket();
    Log("[udp] deactivated");
}
void Shutdown() {
    Deactivate();
    g_nPeers = 0;
    for (auto& p : g_peers) p = Peer{};
    if (g_wsa) { WSACleanup(); g_wsa = false; }
}

const char* MyId() { return g_myId; }
int PeerCount() { return g_nPeers; }
// Peers arrive through the handshake, exactly as shm discovers them through slot ownership. There is
// nothing sensible to do with a bare id string here: an id is not an address.
int AddPeer(const char*) { return -1; }

bool GetStats(int idx, PeerStats* out) {
    if (idx < 0 || idx >= g_nPeers || !out) return false;
    Peer& p = g_peers[idx];
    p.st.lastRecvAgoMs = p.lastRecvMs ? (double)(GetTickCount64() - p.lastRecvMs) : -1.0;
    *out = p.st;
    return true;
}

// ---- binding + lobby ---------------------------------------------------------------------------------
static bool bindSocket(int port) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((u_short)port);
    if (bind(g_sock, (sockaddr*)&a, (int)sizeof(a)) != 0) {
        Log("[udp] bind to port %d failed: %d%s", port, WSAGetLastError(),
            port ? " (already in use? pick another port)" : "");
        return false;
    }
    sockaddr_in got{}; int gl = (int)sizeof(got);
    if (getsockname(g_sock, (sockaddr*)&got, &gl) == 0) addrStr(got, g_boundDesc, sizeof(g_boundDesc));
    else snprintf(g_boundDesc, sizeof(g_boundDesc), "port %d", port);
    return true;
}

bool LobbyHost() {
    if (g_sock == INVALID_SOCKET) return false;
    if (g_inSession) return true;
    const int port = g_listenPort > 0 ? g_listenPort : kDefaultPort;
    if (!bindSocket(port)) { InterlockedExchange(&g_status, -1); return false; }
    g_inSession = true; g_joining = false;
    InterlockedExchange(&g_status, 2);
    Log("[udp] HOSTING on %s -- give a joiner your address and this port", g_boundDesc);
    Log("[udp] (they need to be able to REACH it: same LAN, or this port forwarded to this PC)");
    return true;
}

bool LobbyJoin() {
    if (g_sock == INVALID_SOCKET) return false;
    if (g_inSession) return true;
    sockaddr_in target{};
    if (!parseAddr(g_joinAddr, &target)) {
        Log("[udp] cannot join: '%s' is not an address I can resolve (want 1.2.3.4:7777)", g_joinAddr);
        InterlockedExchange(&g_status, -1);
        return false;
    }
    if (!bindSocket(0)) { InterlockedExchange(&g_status, -1); return false; }   // ephemeral: we dial out
    g_inSession = true;
    clearDials();
    startDial(target, nullptr, true);
    g_joining = true;
    InterlockedExchange(&g_status, 1);
    char a[64]; addrStr(target, a, sizeof(a));
    Log("[udp] joining %s from %s ...", a, g_boundDesc);
    return true;
}

void LobbyLeave() {
    for (int i = 0; i < g_nPeers; i++)
        if (g_peers[i].used && g_peers[i].st.state != 5) {
            sendTo(g_peers[i].addr, PT_BYE, g_peers[i].remoteToken, nullptr, 0);
            g_peers[i].st.state = 5;
        }
    g_inSession = false; g_joining = false;
    clearDials();
    InterlockedExchange(&g_status, 0);
    closeSocket();
    Log("[udp] session CLOSED");
}
int LobbyStatus() { return (int)g_status; }

// ---- trust -------------------------------------------------------------------------------------------
// TRUST_NONE, and that is the honest answer rather than a gap: this wire authenticates nobody. Anyone
// who can reach the port and complete a handshake is a peer. The admission rules above the seam
// degrade to no-ops here BY DESIGN -- they are written against what a backend can prove, and this one
// can prove nothing. `Posture()` reports it so the choice is visible.
TrustLevel PeerTrust(int idx) {
    if (idx < 0 || idx >= g_nPeers) return TRUST_NONE;
    return TRUST_NONE;
}
bool SessionOpen() { return g_inSession; }
void SetRelayControl(bool) {}                    // no relay to control: the route is the route
bool RelaysForced() { return false; }

// ---- send --------------------------------------------------------------------------------------------
void Send(int idx, const void* data, int len, bool reliable) {
    if (idx < 0 || idx >= g_nPeers || len <= 0 || len > kMaxMsg) return;
    Peer& p = g_peers[idx];
    if (!p.used || p.st.state == 5 || !p.remoteToken) return;
    p.lastSendMs = GetTickCount64();
    if (!reliable) {
        sendTo(p.addr, PT_DATA, p.remoteToken, data, len);
        p.st.sent++;
        return;
    }
    // Reliable: park a copy, send it, and keep sending until it is acknowledged.
    RelOut* slot = nullptr;
    for (auto& o : p.out) if (!o.live) { slot = &o; break; }
    if (!slot) {
        // The window is full, which on this wire means the peer has stopped acknowledging. Dropping
        // the OLDEST is the right sacrifice: reliable traffic here is cosmetics, and the newest state
        // is the one worth having. Logged, because a drop on the reliable lane must never be silent.
        RelOut* oldest = &p.out[0];
        for (auto& o : p.out) if (o.firstMs < oldest->firstMs) oldest = &o;
        Log("[udp] peer #%d: reliable window full -- dropping seq %u unacknowledged", idx, oldest->seq);
        slot = oldest;
    }
    slot->seq = p.nextRelSeq++;
    slot->len = len;
    memcpy(slot->data, data, (size_t)len);
    slot->firstMs = slot->lastMs = GetTickCount64();
    slot->live = true;
    sendTo(p.addr, PT_REL, p.remoteToken, &slot->seq, 4, data, len);
    p.st.sent++;
}

// ---- receive -----------------------------------------------------------------------------------------
static bool relSeen(Peer& p, uint32_t seq) {
    if (seq == 0) return true;                               // not a sequence we ever issue
    if (seq > p.relHighest) {
        const uint32_t shift = seq - p.relHighest;
        p.relSeen = (shift >= 64) ? 0 : ((p.relSeen << shift) | 1u);
        p.relHighest = seq;
        return false;                                        // brand new
    }
    const uint32_t back = p.relHighest - seq;
    if (back >= 64) return true;                             // older than the window: treat as a dup
    const uint64_t bit = 1ull << back;
    if (p.relSeen & bit) return true;
    p.relSeen |= bit;
    return false;
}

static void onHello(const sockaddr_in& from, const uint8_t* body, int len, bool isAck) {
    if (len < kIdLen + 4) return;
    char id[kIdLen + 1] = {0};
    memcpy(id, body, kIdLen);
    for (char& c : id) if (c && (c < 0x20 || c > 0x7e)) c = '?';   // it is a peer's string; keep it printable
    uint32_t theirToken = 0;
    memcpy(&theirToken, body + kIdLen, 4);
    if (!theirToken) return;
    if (!_stricmp(id, g_myId)) return;                        // ourselves, via a loopback address

    Peer* p = peerById(id);
    if (!p) {
        // An ACK answers OUR hello, so this peer must keep the token we advertised in it. A plain
        // HELLO is someone dialling US, and they have not been told a token yet -- mint one.
        // An ACK answers ONE of our dials, and must adopt the token we promised THAT endpoint --
        // see startDial. A plain HELLO is someone dialling us: mint one.
        uint32_t adopt = 0;
        if (isAck) {
            const Dial* d = dialFor(from);
            if (!d) return;                     // an ACK for a handshake we never started
            adopt = d->token;
        }
        const int i = addPeer(id, from, adopt);
        if (i < 0) return;
        p = &g_peers[i];
    } else if (!sameAddr(p->addr, from)) {
        // The identity is the id, not the endpoint -- so this MOVES them.
        char a[64], b[64]; addrStr(p->addr, a, sizeof(a)); addrStr(from, b, sizeof(b));
        Log("[udp] peer %s moved %s -> %s", p->idStr, a, b);
        p->addr = from;
    }
    p->remoteToken = theirToken;
    p->lastRecvMs = GetTickCount64();
    if (p->st.state == 5) { p->st.state = 1; Log("[udp] peer %s is back", p->idStr); }

    if (!isAck) {
        // Always answer, including a repeat: our previous ACK may be the packet that was lost.
        sendHello(from, PT_HELLO_ACK, p->localToken);
    } else {
        Dial* d = dialFor(from);
        const bool wasPrimary = d && d->primary;
        if (d) *d = Dial{};                     // answered: stop dialling this one
        if (wasPrimary) {
            g_joining = false;
            InterlockedExchange(&g_status, 3);
            Log("[udp] JOINED -- connected to %s", p->idStr);
        } else {
            Log("[udp] mesh: connected to %s (introduced)", p->idStr);
        }
    }
}

void Tick(RecvFn onRecv, void* user) {
    if (g_sock == INVALID_SOCKET) return;
    const uint64_t now = GetTickCount64();

    // ---- handshake retries. UDP has no connect(): "are you there" is a packet you repeat.
    for (int di = 0; di < kPeers; di++) {
        Dial& d = g_dials[di];
        if (!d.active) continue;
        if (now - d.lastMs >= (uint64_t)kHelloResendMs) {
            d.lastMs = now;
            sendHello(d.target, PT_HELLO, d.token);
        }
        if (now - d.startMs >= (uint64_t)kHelloGiveUpMs) {
            char a[64]; addrStr(d.target, a, sizeof(a));
            if (!d.primary) {
                // An introduced peer that never answers is the NORMAL case off a LAN: two joiners
                // behind different routers cannot reach each other without hole punching, which this
                // backend does not do. It costs nothing and must not look like a fault.
                Log("[udp] mesh: %s did not answer -- not reachable from here (normal off a LAN)", a);
                d = Dial{};
                continue;
            }
            d = Dial{};
            g_joining = false;
            InterlockedExchange(&g_status, -1);
            Log("[udp] join FAILED -- no answer from %s after %d s. Wrong address, wrong port, "
                "firewall, or the host is not listening.", a, kHelloGiveUpMs / 1000);
        }
    }

    // ---- drain. A datagram socket hands us whatever arrived, from anyone; everything below treats
    // the buffer as hostile until the header and the token say otherwise.
    for (;;) {
        uint8_t buf[kHdr + 8 + kMaxMsg];
        sockaddr_in from{}; int fl = (int)sizeof(from);
        const int n = recvfrom(g_sock, (char*)buf, (int)sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            if (e == WSAECONNRESET) continue;      // an ICMP unreachable for an earlier send; not fatal
            break;
        }
        if (n < kHdr) continue;
        Hdr h{};
        memcpy(&h, buf, kHdr);
        if (h.magic != kMagic || h.ver != kVer) continue;      // not ours, or a different build
        const uint8_t* body = buf + kHdr;
        const int bodyLen = n - kHdr;

        if (h.type == PT_HELLO || h.type == PT_HELLO_ACK) {
            if (!g_inSession) continue;                        // not hosting or joining: nobody to greet
            onHello(from, body, bodyLen, h.type == PT_HELLO_ACK);
            continue;
        }

        // Everything else must carry a token WE issued. This is what makes a stray or spoofed datagram
        // cheap to reject, and it is also what lets a peer's address change without losing them.
        Peer* p = peerByToken(h.token);
        if (!p || p->st.state == 5) continue;
        if (!sameAddr(p->addr, from)) {
            char a[64], b[64]; addrStr(p->addr, a, sizeof(a)); addrStr(from, b, sizeof(b));
            Log("[udp] peer %s moved %s -> %s (token matched)", p->idStr, a, b);
            p->addr = from;
        }
        p->lastRecvMs = now;

        switch (h.type) {
        case PT_DATA:
            if (bodyLen <= 0 || bodyLen > kMaxMsg) break;
            p->st.recv++;
            if (onRecv) onRecv((int)(p - g_peers), body, bodyLen, user);
            break;
        case PT_REL: {
            if (bodyLen < 4) break;
            uint32_t seq = 0; memcpy(&seq, body, 4);
            const int plen = bodyLen - 4;
            if (plen <= 0 || plen > kMaxMsg) break;
            sendTo(from, PT_ACK, p->remoteToken, &seq, 4);      // ack FIRST, including duplicates
            if (relSeen(*p, seq)) break;                        // already delivered: ack again, drop
            p->st.recv++;
            if (onRecv) onRecv((int)(p - g_peers), body + 4, plen, user);
            break;
        }
        case PT_ACK: {
            if (bodyLen < 4) break;
            uint32_t seq = 0; memcpy(&seq, body, 4);
            for (auto& o : p->out) if (o.live && o.seq == seq) { o.live = false; break; }
            break;
        }
        case PT_BYE:
            p->st.state = 5;
            Log("[udp] peer %s said goodbye", p->idStr);
            break;
        case PT_PEERS: {
            // WHO ELSE IS HERE. Everyone gossips their own peer list, so the mesh converges no matter
            // who joined first and no matter who anybody considers "the host" -- there is no host in
            // the data model, only the peer somebody happened to dial.
            if (bodyLen < 1) break;
            const int cnt = body[0];
            const int need = 1 + cnt * (kIdLen + 6);
            if (cnt <= 0 || cnt > kPeers || bodyLen < need) break;
            for (int e = 0; e < cnt; e++) {
                const uint8_t* rec = body + 1 + e * (kIdLen + 6);
                char eid[kIdLen + 1] = {0};
                memcpy(eid, rec, kIdLen);
                for (char& c : eid) if (c && (c < 0x20 || c > 0x7e)) c = '?';
                uint32_t ip = 0; uint16_t port = 0;
                memcpy(&ip, rec + kIdLen, 4);
                memcpy(&port, rec + kIdLen + 4, 2);
                if (!ip || !port) continue;
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = ip;
                a.sin_port = port;
                if (startDial(a, eid, false)) {
                    char as[64]; addrStr(a, as, sizeof(as));
                    Log("[udp] mesh: %s introduced %s at %s -- dialling", p->idStr, eid, as);
                }
            }
            break;
        }
        case PT_PING:
        default:
            break;
        }
    }

    // ---- INTRODUCTIONS. Build the roster once per sweep and send it to everyone still live.
    // Every few seconds rather than only on change: a datagram can be lost, and an introduction that
    // was lost is a player who is simply never seen. Repeating is cheaper than acknowledging, and the
    // receiver dedupes on identity, so a repeat costs nothing but the bytes.
    //
    // What this cannot do is make anyone REACHABLE. On a LAN every pair can already talk and the mesh
    // completes; across the internet two joiners behind different routers still cannot reach each
    // other, and those dials time out quietly. That limit is NAT, not addressing, and lifting it
    // needs hole punching -- which needs a rendezvous server this backend deliberately does not have.
    static uint64_t s_lastGossipMs = 0;
    if (g_nPeers > 1 && now - s_lastGossipMs >= (uint64_t)kGossipMs) {
        s_lastGossipMs = now;
        uint8_t body[1 + kPeers * (kIdLen + 6)];
        for (int i = 0; i < g_nPeers; i++) {
            Peer& to = g_peers[i];
            if (!to.used || to.st.state == 5 || !to.remoteToken) continue;
            int cnt = 0, w = 1;
            for (int j = 0; j < g_nPeers; j++) {
                const Peer& about = g_peers[j];
                // Never introduce a peer to themselves, and never pass on a corpse -- the receiver
                // would dial a player who has already left and wait out the whole give-up window.
                if (j == i || !about.used || about.st.state == 5) continue;
                memcpy(body + w, about.idStr, kIdLen);              w += kIdLen;
                memcpy(body + w, &about.addr.sin_addr.s_addr, 4);   w += 4;
                memcpy(body + w, &about.addr.sin_port, 2);          w += 2;
                cnt++;
            }
            if (!cnt) continue;
            body[0] = (uint8_t)cnt;
            sendTo(to.addr, PT_PEERS, to.remoteToken, body, w);
            to.lastSendMs = now;
        }
    }

    // ---- retransmit, keepalive, and the only liveness signal a datagram wire has: silence.
    for (int i = 0; i < g_nPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used || p.st.state == 5) continue;

        for (auto& o : p.out) {
            if (!o.live) continue;
            if (now - o.firstMs >= (uint64_t)kRelGiveUpMs) {
                o.live = false;
                Log("[udp] peer #%d: reliable seq %u never acknowledged -- given up after %d s",
                    i, o.seq, kRelGiveUpMs / 1000);
                continue;
            }
            if (now - o.lastMs >= (uint64_t)kRelResendMs) {
                o.lastMs = now;
                sendTo(p.addr, PT_REL, p.remoteToken, &o.seq, 4, o.data, o.len);
            }
        }

        if (p.remoteToken && now - p.lastSendMs >= (uint64_t)kPingMs) {
            p.lastSendMs = now;
            sendTo(p.addr, PT_PING, p.remoteToken, nullptr, 0);
        }
        if (p.lastRecvMs && now - p.lastRecvMs >= (uint64_t)kPeerTimeoutMs) {
            p.st.state = 5;
            Log("[udp] peer %s timed out after %d s of silence -- treating them as gone",
                p.idStr, kPeerTimeoutMs / 1000);
        }
    }
}

}} // namespace omp::udpb
