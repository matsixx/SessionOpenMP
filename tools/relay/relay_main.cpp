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
// omp_relay -- the SessionOpenMP relay server.
//
// Run it on anything with a public address:   omp_relay [port]
// Players type that address into the mod's "Direct connect" box and pick a room.
//
// WHAT IT DOES: forwards opaque payloads between the clients in a room, and tells each client who
// else is in it. That is the whole job. It does not run the game, does not parse a game packet, and
// keeps no state that outlives the players -- a room exists exactly as long as somebody is in it.
//
// WHY IT EXISTS: on direct UDP a joiner can only reach the host, so two joiners behind different
// routers never see each other, and the host has to forward a port. Here EVERY client dials out,
// which is what opens a return path through a home router, and the relay is reachable by all of
// them by construction.
//
// THE THINGS THAT MATTER FOR LEAVING IT RUNNING UNATTENDED:
//   * NO AMPLIFICATION. The one reply bigger than its request (the room list) requires the request
//     to be padded to the reply's maximum size. An attacker spoofing a victim's address therefore
//     gains nothing -- the factor is <= 1. See relay_proto.h.
//   * NOTHING GROWS. Rooms, clients per room and bytes per second are all capped, and a room is
//     freed the moment its last client leaves or times out. There is no disk state and no cleanup.
//   * A CLIENT IS ITS ENDPOINT UNTIL IT PROVES OTHERWISE. Every packet after the handshake carries
//     the token the relay issued, so a stray or spoofed datagram is one comparison away from being
//     dropped, and a NAT rebind MOVES a client rather than creating a second one.
// PORTABLE ON PURPOSE. The mod is Windows-only because the game is; the relay is not, because the
// natural home for a service that runs all day is a cheap Linux VPS. Everything platform-specific
// lives in the compat block below and nothing below it knows which OS it is on.
//
// Linux build, with no project and no dependencies:   g++ -O2 -o omp_relay relay_main.cpp
// See README.md beside this file for a systemd unit.
#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #define _CRT_RAND_S               // must precede <cstdlib>: it is what declares rand_s
#endif
#include "../../src/transport/relay_proto.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <ctime>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <mstcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
  typedef int socklen_t;
  #define OMP_POLL WSAPoll
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <errno.h>
  #include <strings.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket(s) ::close(s)
  #define _strnicmp      strncasecmp
  #define OMP_POLL       poll
#endif

using namespace omp::relay;

// ---- the compat block. Four things differ; nothing else does. ----------------------------------
// A MONOTONIC millisecond clock. Not wall time: a VPS runs for months and its clock gets stepped by
// NTP, and every timeout here would jump with it.
static uint64_t nowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;                    // spelled `struct` so it resolves whether or not the
    ts.tv_sec = 0; ts.tv_nsec = 0;         // implementation also hoists it into namespace std
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}
// Connection tokens. They are what stops a stray or spoofed datagram being mistaken for a player, so
// they want a real random source rather than rand() seeded off the clock.
static uint32_t randU32() {
    unsigned v = 0;
#ifdef _WIN32
    if (rand_s(&v) != 0) v = ((unsigned)rand() << 16) ^ (unsigned)rand();
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, 1, sizeof(v), f) != sizeof(v)) v = ((unsigned)rand() << 16) ^ (unsigned)rand();
    if (f) fclose(f);
#endif
    return v ? v : 1u;
}
static bool netStart() {
#ifdef _WIN32
    WSADATA w{};
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return true;
#endif
}
static void setNonBlocking(SOCKET s) {
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    // Windows only: without this one ICMP port-unreachable from a client that went away makes the
    // NEXT recvfrom fail, and on a shared socket that is every other client's packet lost too.
    BOOL off = FALSE; DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
static int lastNetError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
static bool wouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}
// Connection-reset noise from an unconnected UDP socket. Recoverable on both, and not worth a line.
static bool transientRecvError(int e) {
#ifdef _WIN32
    return e == WSAECONNRESET;
#else
    return e == ECONNREFUSED || e == EINTR;
#endif
}

// ---- limits. Every one of these is a refusal, not a resize. ------------------------------------
static const int kMaxRooms       = 64;
// THE RATE CAP, and how the number is arrived at -- because the first guess was wrong in the field.
// It was 512 KB/s, chosen as "far above a 30 Hz game stream", which it is and which is beside the
// point: the game stream is not the peak. A REPLAY SYNC is a bulk transfer, paced by the transport's
// SendBudget of 32 chunks per tick at ~931 bytes a chunk, 60 ticks a second -- about 1.8 MB/s, three
// and a half times the old cap. It throttled the mod's own transfer, the reliable lane answered by
// retransmitting (making it worse), and the sync gave up at "stalled". Short syncs squeaked through
// and long ones never did, which is exactly what the field reported.
//
//   replay sync burst   32 x 931 B x 60/s        ~1.8 MB/s
//   snapshot streams    ~1 KB x 60/s x 15 peers  ~0.9 MB/s
//   peak legitimate                              ~2.7 MB/s
//
// 4 MB/s leaves headroom over that. The cap is still a real limit -- it exists so one bad client
// cannot burn the host's bandwidth -- but it must never be what shapes honest traffic, and now that
// it cannot be, tripping it genuinely means something is wrong.
static const int kRateBytesPerS  = 4 * 1024 * 1024;
static const int kRateWindowMs   = 1000;

struct Client {
    sockaddr_in addr{};
    uint32_t    token = 0;
    char        id[kNameLen + 1] = {0};
    uint64_t    lastRecvMs = 0;
    uint32_t    bytesInWindow = 0;
    uint64_t    windowStartMs = 0;
    bool        used = false;
    bool        warnedRate = false;
};
struct Room {
    char   code[kRoomCodeLen + 1] = {0};
    char   host[kNameLen + 1] = {0};
    char   map[kMapLen + 1] = {0};
    Client slot[kMaxSlots];
    bool   used = false;
    uint64_t createdMs = 0;
};
static Room   g_rooms[kMaxRooms];
static SOCKET g_sock = INVALID_SOCKET;
static uint64_t g_fwd = 0, g_dropped = 0;

static void logf(const char* fmt, ...) {
    char t[32];
    const time_t now = time(nullptr);
    tm bd{};
#ifdef _WIN32
    localtime_s(&bd, &now);
#else
    localtime_r(&now, &bd);
#endif
    strftime(t, sizeof(t), "%H:%M:%S", &bd);
    char b[400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    printf("[%s] %s\n", t, b);
    fflush(stdout);
}
static void addrStr(const sockaddr_in& a, char* out, int cap) {
    char ip[64] = {0};
    inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
    snprintf(out, (size_t)cap, "%s:%d", ip, (int)ntohs(a.sin_port));
}
static bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
// Room codes are compared case-insensitively and stored as given: players type them by hand.
static bool sameCode(const char* a, const char* b) { return _strnicmp(a, b, kRoomCodeLen) == 0; }

static void sendTo(const sockaddr_in& to, uint8_t type, uint32_t token,
                   const void* body = nullptr, int bodyLen = 0) {
    uint8_t buf[kHdrLen + kMaxPayload + 64];
    if (bodyLen < 0 || bodyLen > (int)sizeof(buf) - kHdrLen) return;
    Hdr h{};
    h.magic = kMagic; h.ver = kVer; h.type = type; h.slot = kNoSlot; h.token = token;
    memcpy(buf, &h, kHdrLen);
    if (body && bodyLen) memcpy(buf + kHdrLen, body, (size_t)bodyLen);
    sendto(g_sock, (const char*)buf, kHdrLen + bodyLen, 0, (const sockaddr*)&to, sizeof(to));
}

// The roster, sent to everyone in a room whenever it changes. A client turns these slots into peers;
// a slot missing from the roster is a player who left.
static void sendRoster(Room& r) {
    uint8_t body[1 + kMaxSlots * (1 + kNameLen)];
    int w = 1, n = 0;
    for (int i = 0; i < kMaxSlots; i++) {
        if (!r.slot[i].used) continue;
        body[w++] = (uint8_t)i;
        memcpy(body + w, r.slot[i].id, kNameLen); w += kNameLen;
        n++;
    }
    body[0] = (uint8_t)n;
    for (int i = 0; i < kMaxSlots; i++)
        if (r.slot[i].used) sendTo(r.slot[i].addr, RT_ROSTER, r.slot[i].token, body, w);
}

static void dropClient(Room& r, int slot, const char* why) {
    if (!r.slot[slot].used) return;
    char a[64]; addrStr(r.slot[slot].addr, a, sizeof(a));
    logf("room %s: slot %d (%s at %s) left -- %s", r.code, slot, r.slot[slot].id, a, why);
    r.slot[slot] = Client{};
    int live = 0;
    for (int i = 0; i < kMaxSlots; i++) if (r.slot[i].used) live++;
    if (!live) {
        logf("room %s: empty, closed", r.code);
        r = Room{};                       // a room lives exactly as long as its players
        return;
    }
    sendRoster(r);
}

static Room* roomByCode(const char* code) {
    for (auto& r : g_rooms) if (r.used && sameCode(r.code, code)) return &r;
    return nullptr;
}
// Find the client this packet claims to be, and prove it. The token is the proof; the address is
// only a hint, so a NAT rebind moves a client instead of orphaning them.
static Client* findClient(const Hdr& h, const sockaddr_in& from, Room** roomOut, int* slotOut) {
    if (h.slot >= kMaxSlots || !h.token) return nullptr;
    for (auto& r : g_rooms) {
        if (!r.used) continue;
        Client& c = r.slot[h.slot];
        if (!c.used || c.token != h.token) continue;
        if (!sameAddr(c.addr, from)) {
            char a[64], b[64]; addrStr(c.addr, a, sizeof(a)); addrStr(from, b, sizeof(b));
            logf("room %s: slot %d moved %s -> %s", r.code, h.slot, a, b);
            c.addr = from;
        }
        if (roomOut) *roomOut = &r;
        if (slotOut) *slotOut = h.slot;
        return &c;
    }
    return nullptr;
}

static void onHello(const sockaddr_in& from, const uint8_t* body, int len) {
    const int need = kRoomCodeLen + kNameLen + kMapLen + kNameLen;
    if (len < need) return;
    char code[kRoomCodeLen + 1] = {0}, name[kNameLen + 1] = {0};
    char map[kMapLen + 1] = {0},       id[kNameLen + 1] = {0};
    memcpy(code, body, kRoomCodeLen);
    memcpy(name, body + kRoomCodeLen, kNameLen);
    memcpy(map,  body + kRoomCodeLen + kNameLen, kMapLen);
    memcpy(id,   body + kRoomCodeLen + kNameLen + kMapLen, kNameLen);
    // Every one of these is shown in a log line and in other players' menus. Untrusted input, so
    // force it printable rather than trusting a client to have done it.
    char* const fields[] = { code, name, map, id };
    for (char* f : fields)
        for (int i = 0; f[i]; i++) if (f[i] < 0x20 || f[i] > 0x7e) f[i] = '?';
    if (!code[0]) return;

    Room* r = roomByCode(code);
    if (!r) {
        for (auto& room : g_rooms) if (!room.used) { r = &room; break; }
        if (!r) { sendTo(from, RT_FULL, 0); logf("REFUSED %s -- all %d rooms in use", code, kMaxRooms); return; }
        *r = Room{};
        r->used = true;
        strncpy(r->code, code, kRoomCodeLen);
        strncpy(r->host, name, kNameLen);
        strncpy(r->map,  map,  kMapLen);
        r->createdMs = nowMs();
        logf("room %s: opened by %s (%s)", r->code, r->host, r->map[0] ? r->map : "no map");
    }

    // Already in? A repeated HELLO is a retry whose WELCOME was lost -- answer it again with the
    // SAME slot and token rather than consuming a second slot for one player.
    for (int i = 0; i < kMaxSlots; i++) {
        Client& c = r->slot[i];
        if (c.used && sameAddr(c.addr, from)) {
            c.lastRecvMs = nowMs();
            uint8_t w[5]; w[0] = (uint8_t)i; memcpy(w + 1, &c.token, 4);
            sendTo(from, RT_WELCOME, c.token, w, sizeof(w));
            return;
        }
    }
    int slot = -1;
    for (int i = 0; i < kMaxSlots; i++) if (!r->slot[i].used) { slot = i; break; }
    if (slot < 0) {
        sendTo(from, RT_FULL, 0);
        char a[64]; addrStr(from, a, sizeof(a));
        logf("room %s: REFUSED %s -- room full (%d slots)", r->code, a, kMaxSlots);
        return;
    }
    Client& c = r->slot[slot];
    c = Client{};
    c.used = true; c.addr = from; c.token = randU32();
    strncpy(c.id, id[0] ? id : name, kNameLen);
    c.lastRecvMs = c.windowStartMs = nowMs();
    char a[64]; addrStr(from, a, sizeof(a));
    logf("room %s: slot %d = %s at %s", r->code, slot, c.id, a);
    uint8_t w[5]; w[0] = (uint8_t)slot; memcpy(w + 1, &c.token, 4);
    sendTo(from, RT_WELCOME, c.token, w, sizeof(w));
    sendRoster(*r);
}

static void onListReq(const sockaddr_in& from, int datagramLen) {
    // THE PADDING RULE (relay_proto.h). A short request is dropped, silently and always: answering
    // it is what would turn this server into a reflector for somebody else's attack.
    if (datagramLen < kHdrLen + kListReqMin) {
        g_dropped++;
        return;
    }
    uint8_t body[1 + kListRooms * (int)sizeof(RoomInfo)];
    int w = 1, n = 0;
    for (auto& r : g_rooms) {
        if (!r.used || n >= kListRooms) continue;
        RoomInfo ri{};
        memcpy(ri.code, r.code, kRoomCodeLen);
        memcpy(ri.host, r.host, kNameLen);
        memcpy(ri.map,  r.map,  kMapLen);
        int live = 0;
        for (int i = 0; i < kMaxSlots; i++) if (r.slot[i].used) live++;
        ri.players = (uint8_t)live;
        memcpy(body + w, &ri, sizeof(ri)); w += (int)sizeof(ri);
        n++;
    }
    body[0] = (uint8_t)n;
    sendTo(from, RT_LIST, 0, body, w);
}

int main(int argc, char** argv) {
    int port = kDefaultPort;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) { printf("usage: omp_relay [port]\n"); return 1; }
    }
    if (!netStart()) { printf("network startup failed\n"); return 1; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { printf("socket failed\n"); return 1; }
    setNonBlocking(g_sock);
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = htons((unsigned short)port);
    if (bind(g_sock, (sockaddr*)&me, sizeof(me)) == SOCKET_ERROR) {
        printf("cannot bind UDP port %d (error %d) -- is something already on it?\n", port, lastNetError());
        return 1;
    }

    logf("SessionOpenMP relay listening on UDP %d", port);
    logf("players enter this machine's address and port in the mod's Direct connect box");
    logf("limits: %d rooms, %d players per room -- a room closes when its last player leaves",
         kMaxRooms, kMaxSlots);

    uint64_t lastSweep = 0, lastStat = 0;
    for (;;) {
        // WAIT, do not spin. A 1 ms poll loop is 1000 wakeups a second of nothing, which on a small
        // VPS is real CPU and real power for a server that is idle most of the time. Blocking here
        // until a packet arrives (or the sweep is due) costs nothing while nobody is playing.
        pollfd pfd{};
        pfd.fd = g_sock; pfd.events = POLLIN;
        OMP_POLL(&pfd, 1, 100);

        const uint64_t now = nowMs();
        uint8_t buf[kHdrLen + kMaxPayload + 64];
        sockaddr_in from{}; socklen_t fl = (socklen_t)sizeof(from);
        const int n = (int)recvfrom(g_sock, (char*)buf, (int)sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) {
            const int e = lastNetError();
            if (!wouldBlock(e) && !transientRecvError(e)) logf("recvfrom error %d", e);
        } else if (n >= kHdrLen) {
            Hdr h{};
            memcpy(&h, buf, kHdrLen);
            if (h.magic == kMagic && h.ver == kVer) {
                const uint8_t* body = buf + kHdrLen;
                const int bodyLen = n - kHdrLen;
                switch (h.type) {
                case RT_HELLO:   onHello(from, body, bodyLen); break;
                case RT_LISTREQ: onListReq(from, n); break;
                default: {
                    Room* r = nullptr; int slot = -1;
                    Client* c = findClient(h, from, &r, &slot);
                    if (!c) { g_dropped++; break; }
                    c->lastRecvMs = now;
                    // Per-client rate cap. A game stream is nowhere near it, so tripping this means
                    // something is wrong -- say so once rather than every packet.
                    if (now - c->windowStartMs >= (uint64_t)kRateWindowMs) {
                        c->windowStartMs = now; c->bytesInWindow = 0; c->warnedRate = false;
                    }
                    c->bytesInWindow += (uint32_t)n;
                    if (c->bytesInWindow > (uint32_t)kRateBytesPerS) {
                        if (!c->warnedRate) {
                            c->warnedRate = true;
                            logf("room %s: slot %d is over the rate cap (%d KB/s) -- dropping its "
                                 "packets this second. Legitimate play does not reach this.",
                                 r->code, slot, kRateBytesPerS / 1024);
                        }
                        g_dropped++;
                        break;
                    }
                    if (h.type == RT_BYE) { dropClient(*r, slot, "said goodbye"); break; }
                    if (h.type == RT_PING) break;                 // liveness only; lastRecvMs did it
                    if (h.type == RT_FWD) {
                        if (bodyLen < 1) break;
                        const int dst = body[0];
                        if (dst < 0 || dst >= kMaxSlots || dst == slot) break;
                        Client& d = r->slot[dst];
                        if (!d.used) break;
                        // Rewrite the tag: to us it was the DESTINATION, to them it is the SOURCE.
                        uint8_t out[1 + kMaxPayload];
                        const int plen = bodyLen - 1;
                        if (plen < 0 || plen > kMaxPayload) break;
                        out[0] = (uint8_t)slot;
                        memcpy(out + 1, body + 1, (size_t)plen);
                        sendTo(d.addr, RT_FWD, d.token, out, 1 + plen);
                        g_fwd++;
                    }
                    break;
                }
                }
            }
        }

        if (now - lastSweep >= 1000) {
            lastSweep = now;
            for (auto& r : g_rooms) {
                if (!r.used) continue;
                for (int i = 0; i < kMaxSlots; i++) {
                    Client& c = r.slot[i];
                    if (!c.used) continue;
                    if (now - c.lastRecvMs >= (uint64_t)kClientTimeout) {
                        dropClient(r, i, "silent too long");
                        if (!r.used) break;                       // the room closed under us
                    }
                }
            }
        }
        if (now - lastStat >= 60000) {
            lastStat = now;
            int rooms = 0, players = 0;
            for (auto& r : g_rooms) {
                if (!r.used) continue;
                rooms++;
                for (int i = 0; i < kMaxSlots; i++) if (r.slot[i].used) players++;
            }
            logf("%d room(s), %d player(s), %llu forwarded, %llu dropped",
                 rooms, players, (unsigned long long)g_fwd, (unsigned long long)g_dropped);
        }
    }
}
