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
// SessionOpenMP -- RELAY transport backend. Everyone connects OUT to one server, which forwards.
//
// WHY THIS EXISTS ALONGSIDE THE DIRECT UDP BACKEND: direct is better when it works -- one hop, no
// third party -- but it only works when the host can forward a port, and it can never connect two
// joiners behind different routers to each other. That caps direct UDP at "everybody sees the host"
// on the internet. Here every client dials OUT, which is what opens a return path through a home
// router, so nobody configures anything and everyone reaches everyone.
//
// THE SHAPE: the relay is a dumb forwarder (tools/relay). It assigns each client a SLOT in a room
// and moves opaque payloads between slots. Everything that makes a datagram wire usable -- the
// reliable lane, sequencing, dedupe -- stays HERE, end to end between clients, and rides INSIDE the
// forwarded payload. The relay never sees it and could not corrupt it if it tried.
//
// PEER INDICES ARE NOT SLOTS. The seam promises indices are handed out once and never reused; a
// relay slot is reused the moment a player leaves and another joins. So slots are MAPPED to indices
// here, append-only. Getting this wrong aliases two humans onto one proxy, which is the exact bug
// the seam's rule exists to prevent.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include "transport.h"
#include "relay_proto.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")

namespace omp { namespace relayb {

using namespace omp::relay;

// ---- the INNER wire, client to client, carried inside RT_FWD. The relay forwards it blind.
enum : uint8_t { IN_DATA = 0, IN_REL = 1, IN_ACK = 2 };

static const int kMaxMsg      = 1024;    // the session's pack buffer, as everywhere else
// The reliable window has to cover BANDWIDTH x DELAY, and a relay adds a hop: a message is only
// freed when its ack has gone sender -> relay -> receiver -> relay -> sender, which is two or three
// ticks on a LAN and considerably more across the internet. At 32 -- the direct wire's number, for a
// single hop -- a bulk transfer outran the window and the backend dropped 332 messages in a 600
// message burst. Doubling it buys the headroom; SendBudget below is what actually keeps it honest.
static const int kRelWindow   = 64;
static const int kRelResendMs = 200;
static const int kRelGiveUpMs = 4000;

struct RelOut {
    uint32_t seq = 0;
    int      len = 0;
    uint64_t firstMs = 0, lastMs = 0;
    uint8_t  data[kMaxMsg];
    bool     live = false;
};
struct Peer {
    int       slot = -1;                  // the relay's slot, or -1 once they have left
    char      id[kNameLen + 1] = {0};
    PeerStats st{};
    bool      used = false;
    RelOut    out[kRelWindow];
    uint32_t  nextRelSeq = 1;
    uint32_t  relHighest = 0;
    uint64_t  relSeen = 0;
};
static Peer g_peers[kMaxSlots];
static int  g_nPeers = 0;                 // append-only: indices are never reused

static SOCKET      g_sock = INVALID_SOCKET;
static bool        g_wsa = false;
static sockaddr_in g_relay{};
static bool        g_haveRelay = false;
static bool        g_inSession = false;
static volatile LONG g_status = 0;        // 0 idle, 1 connecting, 2 hosting, 3 joined, -1 failed
static int         g_mySlot = -1;
static uint32_t    g_myToken = 0;
static char        g_myId[kNameLen + 1] = {0};
static char        g_roomCode[kRoomCodeLen + 1] = {0};
static char        g_adHost[kNameLen + 1] = {0};
static char        g_adMap[kMapLen + 1] = {0};
static uint64_t    g_helloStartMs = 0, g_helloLastMs = 0, g_lastPingMs = 0;
static bool        g_weOpenedRoom = false;

// the browser
static LobbyInfo   g_browse[kListRooms];
static int         g_nBrowse = 0;
static volatile LONG g_browseState = 0;   // 0 idle, 1 searching, 2 ready, -1 failed
static uint64_t    g_browseStartMs = 0;

static uint32_t randU32() {
    unsigned v = 0;
    if (rand_s(&v) != 0) v = ((unsigned)rand() << 16) ^ (unsigned)rand();
    return v ? v : 1u;
}

// ---- peers -------------------------------------------------------------------------------------
static int indexOfSlot(int slot) {
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].used && g_peers[i].slot == slot) return i;
    return -1;
}
static int addPeer(int slot, const char* id) {
    if (g_nPeers >= kMaxSlots) return -1;
    Peer& p = g_peers[g_nPeers];
    p = Peer{};
    p.used = true; p.slot = slot;
    strncpy(p.id, id ? id : "", kNameLen);
    p.st.state = 1;
    Log("[relay] peer #%d = %s (slot %d)", g_nPeers, p.id, slot);
    return g_nPeers++;
}

// ---- raw send ----------------------------------------------------------------------------------
static void sendRelay(uint8_t type, const void* body, int bodyLen) {
    if (g_sock == INVALID_SOCKET || !g_haveRelay) return;
    uint8_t buf[kHdrLen + kMaxPayload + 64];
    if (bodyLen < 0 || bodyLen > (int)sizeof(buf) - kHdrLen) return;
    Hdr h{};
    h.magic = kMagic; h.ver = kVer; h.type = type;
    h.slot  = (g_mySlot < 0) ? kNoSlot : (uint8_t)g_mySlot;
    h.token = g_myToken;
    memcpy(buf, &h, kHdrLen);
    if (body && bodyLen) memcpy(buf + kHdrLen, body, (size_t)bodyLen);
    sendto(g_sock, (const char*)buf, kHdrLen + bodyLen, 0, (const sockaddr*)&g_relay, sizeof(g_relay));
}
// One client-to-client message: [dstSlot][innerType][optional seq][payload].
static void sendInner(int dstSlot, uint8_t inner, const void* a, int aLen,
                      const void* b = nullptr, int bLen = 0) {
    uint8_t body[1 + 1 + 4 + kMaxMsg];
    int w = 0;
    body[w++] = (uint8_t)dstSlot;
    body[w++] = inner;
    if (a && aLen > 0) { if (w + aLen > (int)sizeof(body)) return; memcpy(body + w, a, (size_t)aLen); w += aLen; }
    if (b && bLen > 0) { if (w + bLen > (int)sizeof(body)) return; memcpy(body + w, b, (size_t)bLen); w += bLen; }
    sendRelay(RT_FWD, body, w);
}

static void sendHello() {
    uint8_t body[kRoomCodeLen + kNameLen + kMapLen + kNameLen] = {0};
    memcpy(body, g_roomCode, kRoomCodeLen);
    memcpy(body + kRoomCodeLen, g_adHost, kNameLen);
    memcpy(body + kRoomCodeLen + kNameLen, g_adMap, kMapLen);
    memcpy(body + kRoomCodeLen + kNameLen + kMapLen, g_myId, kNameLen);
    sendRelay(RT_HELLO, body, (int)sizeof(body));
}

// ---- lifecycle ---------------------------------------------------------------------------------
static bool openSocket() {
    if (!g_wsa) { WSADATA w{}; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false; g_wsa = true; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    BOOL off = FALSE; DWORD got = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_addr.s_addr = INADDR_ANY; me.sin_port = 0;
    if (bind(g_sock, (sockaddr*)&me, sizeof(me)) == SOCKET_ERROR) {
        closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
    }
    return true;
}
static void closeSocket() {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
}

bool Init(bool) {
    if (g_sock != INVALID_SOCKET) return true;
    if (!openSocket()) { Log("[relay] could not open a socket"); return false; }
    if (!g_myId[0]) snprintf(g_myId, sizeof(g_myId), "relay-%08x", randU32());
    Log("[relay] ready -- give it a server address and a room");
    return true;
}
void Shutdown() {
    if (g_inSession) sendRelay(RT_BYE, nullptr, 0);
    closeSocket();
    for (auto& p : g_peers) p = Peer{};
    g_nPeers = 0; g_inSession = false; g_mySlot = -1; g_myToken = 0;
    InterlockedExchange(&g_status, 0);
    if (g_wsa) { WSACleanup(); g_wsa = false; }
}
void Deactivate() { Shutdown(); }
const char* MyId() { return g_myId; }
int  AddPeer(const char*) { return -1; }          // peers arrive on the roster; they are not added
int  PeerCount() { return g_nPeers; }
bool SessionOpen() { return g_inSession; }
void SetRelayControl(bool) {}                     // the relay IS the route
bool RelaysForced() { return true; }
void SetLocalIdentity(const char* id) { if (id && *id) { strncpy(g_myId, id, kNameLen); g_myId[kNameLen] = 0; } }
void SetLobbyAd(const char* host, const char* map) {
    if (host) { strncpy(g_adHost, host, kNameLen); g_adHost[kNameLen] = 0; }
    if (map)  { strncpy(g_adMap,  map,  kMapLen);  g_adMap[kMapLen]  = 0; }
}
void SetLobbyCode(const char* code) {
    memset(g_roomCode, 0, sizeof(g_roomCode));
    if (code) strncpy(g_roomCode, code, kRoomCodeLen);
}
const char* LobbyCode() { return g_roomCode; }
const char* MakeLobbyCode(char* out, int cap) {
    static const char kAlpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no I/O/0/1: people type these
    if (!out || cap < 7) return "";
    for (int i = 0; i < 6; i++) out[i] = kAlpha[randU32() % (sizeof(kAlpha) - 1)];
    out[6] = 0;
    return out;
}
// The relay's address is set through the SAME intent setter the direct backend uses, because to the
// player it is the same act: type where to connect. `DirectEndpoint()` is that string.
// STORED, NOT RESOLVED. This is an intent setter: it is called BEFORE the transport starts, which
// is before WSAStartup, and getaddrinfo without Winsock initialised fails every time -- silently
// looking exactly like "you typed a bad address". So the string is kept and resolved at the moment
// the session opens, by which point the socket exists. (The direct backend keeps its endpoint as a
// string for the same reason.)
static char g_serverStr[160] = {0};
static int  g_serverPort = 0;
void SetServer(const char* addr, int port) {
    g_haveRelay = false;
    memset(g_serverStr, 0, sizeof(g_serverStr));
    if (addr) strncpy(g_serverStr, addr, sizeof(g_serverStr) - 1);
    g_serverPort = port;
}
static bool resolveServer() {
    if (g_haveRelay) return true;
    if (!g_serverStr[0]) return false;
    char host[128] = {0};
    int p = g_serverPort > 0 ? g_serverPort : kDefaultPort;
    const char* colon = strrchr(g_serverStr, ':');
    if (colon && colon != g_serverStr) {
        const size_t n = (size_t)(colon - g_serverStr);
        if (n >= sizeof(host)) return false;
        memcpy(host, g_serverStr, n); host[n] = 0;
        const int typed = atoi(colon + 1);
        if (typed > 0 && typed < 65536) p = typed;
    } else {
        strncpy(host, g_serverStr, sizeof(host) - 1);
    }
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    char portStr[16]; snprintf(portStr, sizeof(portStr), "%d", p);
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
        Log("[relay] cannot resolve '%s' -- want an address or hostname, optionally with :port", host);
        return false;
    }
    memcpy(&g_relay, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    g_haveRelay = true;
    return true;
}

static bool startSession(bool opening) {
    if (g_sock == INVALID_SOCKET && !openSocket()) return false;
    if (!resolveServer()) { Log("[relay] no usable server address -- type one in Direct connect");
                            InterlockedExchange(&g_status, -1); return false; }
    if (g_inSession) return true;
    if (!g_roomCode[0]) { Log("[relay] no room -- a room name or code is required");
                          InterlockedExchange(&g_status, -1); return false; }
    g_inSession = true;
    g_weOpenedRoom = opening;
    g_mySlot = -1; g_myToken = 0;
    g_helloStartMs = GetTickCount64(); g_helloLastMs = 0;
    InterlockedExchange(&g_status, 1);
    char a[64] = {0};
    inet_ntop(AF_INET, (void*)&g_relay.sin_addr, a, sizeof(a));
    Log("[relay] joining room '%s' on %s:%d ...", g_roomCode, a, (int)ntohs(g_relay.sin_port));
    sendHello();
    return true;
}
// Host and Join are THE SAME ACT here, and that is not a shortcut. A room is created by the first
// player to name it and closes when the last leaves; there is no owner to be. The distinction is
// kept only so the menu can keep saying Host and Join, and so a host can advertise a map.
bool LobbyHost() { return startSession(true); }
bool LobbyJoin() { return startSession(false); }
bool LobbyJoinByCode(const char* code) { SetLobbyCode(code); return startSession(false); }
void LobbyLeave() {
    if (g_inSession) sendRelay(RT_BYE, nullptr, 0);
    for (auto& p : g_peers) p = Peer{};
    g_nPeers = 0;
    g_inSession = false; g_mySlot = -1; g_myToken = 0;
    InterlockedExchange(&g_status, 0);
    Log("[relay] left the room");
}
int LobbyStatus() { return (int)g_status; }
int InitState() { return g_sock != INVALID_SOCKET ? 2 : 0; }
// Nobody owns a room, so nobody can kick from one. Said honestly rather than faked.
bool LobbyIsHost() { return false; }
const char* LobbyOwnerId() { return ""; }
bool LobbyKick(const char*) { return false; }
const char* PeerIdStr(int i) { return (i >= 0 && i < g_nPeers && g_peers[i].used) ? g_peers[i].id : ""; }
// The relay places a client in a room and tells everyone who is there -- that IS a roster, and it is
// the same thing EOS's vouching means. A peer we were told about is vouched; nothing else can even
// reach us, because the relay only forwards within a room.
TrustLevel PeerTrust(int i) {
    return (i >= 0 && i < g_nPeers && g_peers[i].used) ? TRUST_VOUCHED : TRUST_NONE;
}

// ---- the browser -------------------------------------------------------------------------------
bool LobbyBrowse() {
    if (g_sock == INVALID_SOCKET && !openSocket()) return false;
    if (!resolveServer()) return false;
    g_nBrowse = 0;
    InterlockedExchange(&g_browseState, 1);
    g_browseStartMs = GetTickCount64();
    // THE PADDING RULE (relay_proto.h): the request is padded to the reply's maximum size so this
    // server can never be used to amplify traffic at a spoofed victim. The bytes are wasted on
    // purpose; that waste IS the defence.
    uint8_t pad[kListReqMin] = {0};
    sendRelay(RT_LISTREQ, pad, (int)sizeof(pad));
    return true;
}
int  BrowseStatus() { return (int)g_browseState; }
int  BrowseCount()  { return g_nBrowse; }
bool BrowseAt(int i, LobbyInfo* out) {
    if (i < 0 || i >= g_nBrowse || !out) return false;
    *out = g_browse[i];
    return true;
}
bool LobbyJoinAt(int i) {
    if (i < 0 || i >= g_nBrowse) return false;
    return LobbyJoinByCode(g_browse[i].id);          // `id` is the room code -- see the browse parse
}

// ---- send --------------------------------------------------------------------------------------
void Send(int idx, const void* data, int len, bool reliable) {
    if (idx < 0 || idx >= g_nPeers || len <= 0 || len > kMaxMsg) return;
    Peer& p = g_peers[idx];
    if (!p.used || p.slot < 0 || g_mySlot < 0) return;
    if (!reliable) {
        sendInner(p.slot, IN_DATA, data, len);
        p.st.sent++;
        return;
    }
    RelOut* slot = nullptr;
    for (auto& o : p.out) if (!o.live) { slot = &o; break; }
    if (!slot) { Log("[relay] peer #%d: reliable window full -- message dropped", idx); return; }
    slot->seq = p.nextRelSeq++;
    slot->len = len;
    memcpy(slot->data, data, (size_t)len);
    slot->firstMs = slot->lastMs = GetTickCount64();
    slot->live = true;
    sendInner(p.slot, IN_REL, &slot->seq, 4, data, len);
    p.st.sent++;
}
// THE HONEST ANSWER to "how many reliable messages can you absorb this tick" -- which is what the
// seam asks for, and what a bulk sender paces itself by. A constant here is a lie the caller pays
// for: it kept handing us chunks after the window was full, and every one past the end was dropped
// on the floor. Free slots in the TIGHTEST peer's window self-regulates instead, at any round-trip
// time, without the backend having to guess at one.
//
// The floor of 1 is deliberate. Replay sync treats a budget of 0 as "no limit given" and falls back
// to its own chunks-per-tick, so returning 0 -- the very moment we least want more data -- would
// remove the brake instead of applying it. One chunk a tick is a brake; zero is a lie.
int SendBudget() {
    int worst = kRelWindow;
    bool any = false;
    for (int i = 0; i < g_nPeers; i++) {
        const Peer& p = g_peers[i];
        if (!p.used || p.st.state == 5 || p.slot < 0) continue;
        int freeSlots = 0;
        for (const auto& o : p.out) if (!o.live) freeSlots++;
        if (!any || freeSlots < worst) { worst = freeSlots; any = true; }
    }
    if (!any) return kRelWindow;
    return worst > 0 ? worst : 1;
}
bool GetStats(int idx, PeerStats* out) {
    if (idx < 0 || idx >= g_nPeers || !out) return false;
    *out = g_peers[idx].st;
    return true;
}

// ---- receive -----------------------------------------------------------------------------------
static void onRoster(const uint8_t* body, int len) {
    if (len < 1) return;
    const int n = body[0];
    if (n < 0 || n > kMaxSlots || len < 1 + n * (1 + kNameLen)) return;
    bool present[kMaxSlots] = {};
    for (int e = 0; e < n; e++) {
        const uint8_t* rec = body + 1 + e * (1 + kNameLen);
        const int slot = rec[0];
        if (slot < 0 || slot >= kMaxSlots) continue;
        char id[kNameLen + 1] = {0};
        memcpy(id, rec + 1, kNameLen);
        for (int i = 0; id[i]; i++) if (id[i] < 0x20 || id[i] > 0x7e) id[i] = '?';
        present[slot] = true;
        if (slot == g_mySlot) continue;                       // ourselves
        if (indexOfSlot(slot) < 0) addPeer(slot, id);
    }
    // Anyone we know who is no longer on the roster has left. State 5 is what stops a proxy being
    // driven by somebody who is gone -- the same meaning it has on every other backend.
    for (int i = 0; i < g_nPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used || p.slot < 0) continue;
        if (!present[p.slot] && p.st.state != 5) {
            p.st.state = 5;
            Log("[relay] peer %s left the room", p.id);
            p.slot = -1;
        }
    }
}

static void onForward(const uint8_t* body, int len, RecvFn onRecv, void* user) {
    if (len < 2) return;
    const int srcSlot = body[0];
    const uint8_t inner = body[1];
    const uint8_t* rest = body + 2;
    const int restLen = len - 2;
    const int idx = indexOfSlot(srcSlot);
    if (idx < 0) return;                       // not on our roster yet: the roster is the admission
    Peer& p = g_peers[idx];
    if (p.st.state == 5) return;

    switch (inner) {
    case IN_DATA:
        p.st.recv++;
        if (onRecv && restLen > 0) onRecv(idx, rest, restLen, user);
        break;
    case IN_REL: {
        if (restLen < 4) break;
        uint32_t seq = 0; memcpy(&seq, rest, 4);
        const int plen = restLen - 4;
        // Acknowledge FIRST, including duplicates: the ack is what stops the retransmit, and a
        // duplicate proves the previous ack was the packet that was lost.
        sendInner(p.slot, IN_ACK, &seq, 4);
        bool dupe = false;
        if (seq == p.relHighest) dupe = true;
        else if (seq < p.relHighest) {
            const uint32_t age = p.relHighest - seq;
            dupe = (age > 64) || ((p.relSeen >> (age - 1)) & 1ull) != 0;
            if (!dupe && age <= 64) p.relSeen |= (1ull << (age - 1));
        } else {
            const uint32_t adv = seq - p.relHighest;
            p.relSeen = (adv >= 64) ? 0 : ((p.relSeen << adv) | (1ull << (adv - 1)));
            p.relHighest = seq;
        }
        if (dupe) break;
        p.st.recv++;
        if (onRecv && plen > 0) onRecv(idx, rest + 4, plen, user);
        break;
    }
    case IN_ACK: {
        if (restLen < 4) break;
        uint32_t seq = 0; memcpy(&seq, rest, 4);
        for (auto& o : p.out) if (o.live && o.seq == seq) { o.live = false; break; }
        break;
    }
    default: break;
    }
}

void Tick(RecvFn onRecv, void* user) {
    if (g_sock == INVALID_SOCKET) return;
    const uint64_t now = GetTickCount64();

    // ---- the handshake, repeated until answered. UDP has no connect().
    if (g_inSession && g_mySlot < 0) {
        if (now - g_helloLastMs >= (uint64_t)kHelloResendMs) { g_helloLastMs = now; sendHello(); }
        if (now - g_helloStartMs >= (uint64_t)kHelloGiveUpMs) {
            g_inSession = false;
            InterlockedExchange(&g_status, -1);
            Log("[relay] no answer from the relay after %d s -- wrong address, wrong port, or it is "
                "not running", kHelloGiveUpMs / 1000);
        }
    }
    if (g_browseState == 1 && now - g_browseStartMs >= 5000) {
        InterlockedExchange(&g_browseState, -1);
        Log("[relay] no room list came back -- is the address right?");
    }

    for (;;) {
        uint8_t buf[kHdrLen + kMaxPayload + 64];
        sockaddr_in from{}; int fl = (int)sizeof(from);
        const int n = recvfrom(g_sock, (char*)buf, (int)sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            if (e == WSAECONNRESET) continue;
            break;
        }
        if (n < kHdrLen) continue;
        Hdr h{};
        memcpy(&h, buf, kHdrLen);
        if (h.magic != kMagic || h.ver != kVer) continue;
        const uint8_t* body = buf + kHdrLen;
        const int bodyLen = n - kHdrLen;
        switch (h.type) {
        case RT_WELCOME: {
            if (bodyLen < 5) break;
            const int slot = body[0];
            if (slot < 0 || slot >= kMaxSlots) break;
            if (g_mySlot < 0) {
                g_mySlot = slot;
                memcpy(&g_myToken, body + 1, 4);
                InterlockedExchange(&g_status, g_weOpenedRoom ? 2 : 3);
                Log("[relay] in room '%s' as slot %d", g_roomCode, g_mySlot);
            }
            break;
        }
        case RT_FULL:
            g_inSession = false;
            InterlockedExchange(&g_status, -1);
            Log("[relay] refused: the room or the server is full");
            break;
        case RT_ROSTER:  onRoster(body, bodyLen); break;
        case RT_FWD:     onForward(body, bodyLen, onRecv, user); break;
        case RT_LIST: {
            if (bodyLen < 1) break;
            const int cnt = body[0];
            if (cnt < 0 || cnt > kListRooms || bodyLen < 1 + cnt * (int)sizeof(RoomInfo)) break;
            g_nBrowse = 0;
            for (int i = 0; i < cnt && g_nBrowse < kListRooms; i++) {
                RoomInfo ri{};
                memcpy(&ri, body + 1 + i * (int)sizeof(RoomInfo), sizeof(ri));
                LobbyInfo& L = g_browse[g_nBrowse];
                L = LobbyInfo{};
                // `id` carries the ROOM CODE. On EOS it is a lobby id used for display only, because
                // you join by index; here the index-join needs the code, so this is the field that
                // makes LobbyJoinAt work rather than a decoration.
                memcpy(L.id, ri.code, kRoomCodeLen);
                L.id[kRoomCodeLen] = 0;
                memcpy(L.host, ri.host, sizeof(L.host) - 1 < kNameLen ? sizeof(L.host) - 1 : kNameLen);
                memcpy(L.map,  ri.map,  sizeof(L.map)  - 1 < kMapLen  ? sizeof(L.map)  - 1 : kMapLen);
                L.players = ri.players;
                L.maxPlayers = kMaxSlots;
                g_nBrowse++;
            }
            InterlockedExchange(&g_browseState, 2);
            break;
        }
        default: break;
        }
    }

    // ---- retransmit and keepalive.
    for (int i = 0; i < g_nPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used || p.st.state == 5 || p.slot < 0) continue;
        for (auto& o : p.out) {
            if (!o.live) continue;
            if (now - o.firstMs >= (uint64_t)kRelGiveUpMs) {
                o.live = false;
                Log("[relay] peer #%d: reliable seq %u never acknowledged -- given up", i, o.seq);
                continue;
            }
            if (now - o.lastMs >= (uint64_t)kRelResendMs) {
                o.lastMs = now;
                sendInner(p.slot, IN_REL, &o.seq, 4, o.data, o.len);
            }
        }
    }
    // One keepalive to the RELAY, not one per peer: there is a single flow, and it is the one whose
    // NAT mapping has to stay open.
    if (g_inSession && g_mySlot >= 0 && now - g_lastPingMs >= (uint64_t)kPingMs) {
        g_lastPingMs = now;
        sendRelay(RT_PING, nullptr, 0);
    }
}

void Posture(char* out, int cap) {
    if (!out || cap <= 0) return;
    snprintf(out, (size_t)cap,
             "Relay server: everyone connects out to one address, so nobody forwards a port and "
             "peers never see each other's IP. Not encrypted -- whoever runs the relay can see the "
             "traffic.");
}

} }  // namespace omp::relayb
