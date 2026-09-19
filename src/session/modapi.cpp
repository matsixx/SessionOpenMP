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
// The mod channel -- see modapi.h for the wire and the rules.
#include "modapi.h"
#include "session.h"
#include "../transport/transport.h"
#include "../ui/mp_name.h"
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace omp { namespace modapi {

// "OMPm". Every uppercase OMP? letter is claimed (see the ledger in replication.cpp); lowercase starts here.
// Lowercase claimed so far: "OMPm" (this lane), "OMPr" (a player's radio stream, replication.cpp PackRadio).
static const uint32_t kMagic       = 0x6D504D4Fu;
static const uint8_t  kLaneVersion = 1;
static const uint8_t  kKindList    = 1;
static const uint8_t  kKindData    = 2;
static const int      kHdr         = 6;             // magic, lane version, kind
static const int      kDataHdr     = kHdr + 4;      // + channel hash
static const int      kPacketMax   = kDataHdr + kMaxPayload;
static const int      kMaxPeers    = 32;            // transport indices tracked (EOS holds 16)
static const int      kMaxParts    = 8;
static const int      kIdMax       = 63;

static const uint64_t kListEveryUs  = 2000000;      // our list, while we have channels
static const uint64_t kListExpireUs = 7000000;      // their view of a peer dies after this silence
static const uint64_t kActiveMs     = 2000;         // InSession: a session frame this recently

// Outgoing, per channel (token buckets).
static const double   kOutMsgsPerSec  = 30.0,  kOutMsgsBurst  = 10.0;
static const double   kOutBytesPerSec = 8000., kOutBytesBurst = 16000.;
// Incoming, per peer across all channels.
static const double   kInMsgsPerSec   = 60.0,  kInMsgsBurst   = 120.0;
static const int      kOutboxMax      = 256;

static uint64_t steadyMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static bool validName(const char* s, int len) {
    if (len < 1 || len > kNameMax) return false;
    for (int i = 0; i < len; i++) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static void copyOut(char* out, int cap, const char* s) {
    if (!out || cap <= 0) return;
    size_t n = s ? strlen(s) : 0;
    if (n > (size_t)cap - 1) n = (size_t)cap - 1;
    if (n) memcpy(out, s, n);
    out[n] = 0;
}

struct Chan {
    bool        used = false;
    uint32_t    serial = 0;             // part of the handle: a stale handle never reaches a reused slot
    char        name[kNameMax + 1] = {};
    uint32_t    hash = 0;
    OnMessageFn onMessage = nullptr;
    OnPlayerFn  onPlayer = nullptr;
    void*       user = nullptr;
    bool        announce = false;       // fire joined for players who already have it, next frame
    double      tokMsgs = kOutMsgsBurst, tokBytes = kOutBytesBurst;
    uint64_t    refillMs = 0;
    int         refusedSaid = 0;
    uint64_t    sent = 0, received = 0;
};

struct NameSet {
    int      n = 0;
    uint32_t hash[kMaxChannels] = {};
    char     name[kMaxChannels][kNameMax + 1] = {};
    bool has(const char* nm, uint32_t h) const {
        for (int i = 0; i < n; i++) if (hash[i] == h && !strcmp(name[i], nm)) return true;
        return false;
    }
    void add(const char* nm, int len) {
        if (n >= kMaxChannels) return;
        char tmp[kNameMax + 1]; memcpy(tmp, nm, (size_t)len); tmp[len] = 0;
        const uint32_t h = fnv1a(tmp);
        if (has(tmp, h)) return;
        memcpy(name[n], tmp, (size_t)len + 1); hash[n] = h; n++;
    }
};

struct Peer {
    bool     used = false;
    char     id[kIdMax + 1] = {};       // the transport's identity when claimed ("" on shm/UDP)
    char     saysId[kIdMax + 1] = {};   // the id their lists carry (their MyId)
    bool     capable = false;           // may be sent this lane
    bool     departed = false;
    bool     listSentOnce = false;
    bool     haveList = false;
    uint64_t lastListUs = 0;
    NameSet  chans;                     // committed
    uint16_t stGen = 0;
    uint8_t  stParts = 0, stSeen = 0;
    NameSet  staging;
    // cached from the session each frame, so the queries are safe off the game thread
    bool     inRoster = false;
    char     display[40] = {};
    void*    actor = nullptr;
    double   inTok = kInMsgsBurst;
    uint64_t inRefillUs = 0;
    uint32_t dropped = 0;
};

struct OutMsg {
    int      chanSlot;
    uint32_t serial;
    int      player;
    bool     reliable;
    std::vector<uint8_t> bytes;
};

// One event queued under the lock and fired after it is released.
struct Event {
    OnMessageFn    onMessage = nullptr;
    OnPlayerFn     onPlayer = nullptr;
    void*          user = nullptr;
    int            player = -1;
    int            joined = 0;
    const uint8_t* data = nullptr;
    int            len = 0;
};

static std::mutex          g_lock;
static Chan                g_chans[kMaxChannels];
static uint32_t            g_serial = 0;
static Peer                g_peers[kMaxPeers];
static std::vector<OutMsg> g_outbox;
static bool                g_listDirty = false;
static uint16_t            g_listGen = 0;
static uint64_t            g_lastListUs = 0;
static bool                g_active = false;
static uint64_t            g_activeMs = 0;
static char                g_localName[40] = {};
static char                g_localId[kIdMax + 1] = {};
static uint64_t            g_droppedLoggedUs = 0;
static std::thread::id     g_gameThread{};
// Who a player number last belonged to, kept after they leave so a leave callback can still name them.
// Live data wins whenever the number is in use again.
struct Gone { bool set = false; char id[kIdMax + 1] = {}; char name[40] = {}; };
static Gone                g_gone[kMaxPeers];
static void (*g_logf)(const char*) = nullptr;

static void say(const char* fmt, ...) {
    if (!g_logf) return;
    char m[300];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    g_logf(m);
}

static void fire(const std::vector<Event>& ev) {
    for (const Event& e : ev) {
        if (e.onPlayer)  e.onPlayer(e.player, e.joined, e.user);
        if (e.onMessage) e.onMessage(e.player, e.data, e.len, e.user);
    }
}

static int handleOf(int slot) { return (int)(((g_chans[slot].serial & 0xFFFFFFu) << 6) | (uint32_t)(slot + 1)); }
static Chan* chanOf(int handle, int* slotOut = nullptr) {
    if (handle <= 0) return nullptr;
    const int slot = (handle & 0x3F) - 1;
    if (slot < 0 || slot >= kMaxChannels) return nullptr;
    Chan& c = g_chans[slot];
    if (!c.used || handleOf(slot) != handle) return nullptr;
    if (slotOut) *slotOut = slot;
    return &c;
}

static bool activeNow() { return g_active && steadyMs() - g_activeMs < kActiveMs; }

// A peer counts as "having" a channel only while it can actually be reached with it.
static bool peerHas(const Peer& p, const Chan& c) {
    return p.used && p.capable && !p.departed && p.haveList && p.chans.has(c.name, c.hash);
}

static const char* authorityId(const Peer& p) {
    // On EOS the transport's id is authenticated; a list cannot claim a different one there.
    return p.id[0] ? p.id : p.saysId;
}

static void peerLeaveAll(int idx, std::vector<Event>& ev) {
    Peer& p = g_peers[idx];
    if (p.used) {
        const char* id = authorityId(p);
        Gone& g = g_gone[idx];
        if (id[0] || p.display[0]) {
            if (p.display[0] || strcmp(g.id, id) != 0) copyOut(g.name, sizeof(g.name), p.display);
            copyOut(g.id, sizeof(g.id), id);
            g.set = true;
        }
    }
    if (p.haveList && p.capable && !p.departed) {
        for (auto& c : g_chans) {
            if (!c.used || c.announce || !p.chans.has(c.name, c.hash) || !c.onPlayer) continue;
            Event e; e.onPlayer = c.onPlayer; e.user = c.user; e.player = idx; e.joined = 0;
            ev.push_back(e);
            say("[modapi] player %d left '%s'", idx, c.name);
        }
    }
    p.chans = NameSet{}; p.haveList = false;
    p.staging = NameSet{}; p.stSeen = 0; p.stParts = 0;
}

static void peerReset(int idx, const char* id, std::vector<Event>& ev) {
    peerLeaveAll(idx, ev);
    g_peers[idx] = Peer{};
    g_peers[idx].used = true;
    copyOut(g_peers[idx].id, sizeof(g_peers[idx].id), id);
}

// The transport may hand an index to a different player; nothing of the old one may reach them.
static Peer* peerFor(int idx, std::vector<Event>& ev) {
    if (idx < 0 || idx >= kMaxPeers) return nullptr;
    Peer& p = g_peers[idx];
    const char* id = PeerIdStr(idx);
    if (!p.used) peerReset(idx, id, ev);
    else if (id && id[0] && p.id[0] && _stricmp(id, p.id) != 0) {
        say("[modapi] player %d is a different player now -- their channels are dropped", idx);
        peerReset(idx, id, ev);
    } else if (id && id[0] && !p.id[0]) copyOut(p.id, sizeof(p.id), id);
    return &p;
}

static void commitList(int idx, std::vector<Event>& ev) {
    Peer& p = g_peers[idx];
    const bool wasReachable = p.haveList && !p.departed;
    for (auto& c : g_chans) {
        if (!c.used || !c.onPlayer || c.announce) continue;   // a pending announce reports the final set
        const bool had = wasReachable && p.chans.has(c.name, c.hash);
        const bool has = p.staging.has(c.name, c.hash);
        if (had == has) continue;
        Event e; e.onPlayer = c.onPlayer; e.user = c.user; e.player = idx; e.joined = has ? 1 : 0;
        ev.push_back(e);
        say("[modapi] player %d %s '%s'", idx, has ? "joined" : "left", c.name);
    }
    p.chans = p.staging;
    p.haveList = true;
}

// ---- LIST encode: one or more parts, each within the transport's message size.
static void sendList(int onlyPeer, int* relCount) {
    uint8_t names[kMaxChannels][kNameMax + 1];
    int     lens[kMaxChannels];
    int     n = 0;
    for (auto& c : g_chans) if (c.used) { lens[n] = (int)strlen(c.name); memcpy(names[n], c.name, (size_t)lens[n]); n++; }
    int idLen = (int)strlen(g_localId); if (idLen > kIdMax) idLen = kIdMax;
    const int fixed = kHdr + 2 + 1 + 1 + 1 + idLen + 1;

    // split into parts
    int partStart[kMaxParts + 1]; int parts = 0; int i = 0;
    partStart[0] = 0;
    do {
        int size = fixed, j = i;
        while (j < n && size + 1 + lens[j] <= kPacketMax) { size += 1 + lens[j]; j++; }
        partStart[parts++] = i;
        i = j;
    } while (i < n && parts < kMaxParts);
    partStart[parts] = i;

    const uint16_t gen = ++g_listGen;
    const int nPeers = PeerCount();
    for (int part = 0; part < parts; part++) {
        uint8_t pkt[kPacketMax]; int w = 0;
        memcpy(pkt, &kMagic, 4); w = 4;
        pkt[w++] = kLaneVersion; pkt[w++] = kKindList;
        memcpy(pkt + w, &gen, 2); w += 2;
        pkt[w++] = (uint8_t)part; pkt[w++] = (uint8_t)parts;
        pkt[w++] = (uint8_t)idLen; memcpy(pkt + w, g_localId, (size_t)idLen); w += idLen;
        const int from = partStart[part], to = partStart[part + 1];
        pkt[w++] = (uint8_t)(to - from);
        for (int k = from; k < to; k++) { pkt[w++] = (uint8_t)lens[k]; memcpy(pkt + w, names[k], (size_t)lens[k]); w += lens[k]; }
        for (int p = 0; p < nPeers && p < kMaxPeers; p++) {
            if (onlyPeer >= 0 && p != onlyPeer) continue;
            const Peer& pe = g_peers[p];
            if (!pe.used || !pe.capable || pe.departed) continue;
            omp::Send(p, pkt, w, true);
            if (relCount) relCount[p]++;
        }
    }
    for (int p = 0; p < kMaxPeers; p++)
        if ((onlyPeer < 0 || p == onlyPeer) && g_peers[p].used && g_peers[p].capable && !g_peers[p].departed)
            g_peers[p].listSentOnce = true;
}

// ==================================== public ====================================================

int Register(const char* channel, OnMessageFn onMessage, OnPlayerFn onPlayer, void* user) {
    if (!channel) return 0;
    const int len = (int)strnlen(channel, kNameMax + 2);
    std::lock_guard<std::mutex> lk(g_lock);
    if (!validName(channel, len)) { say("[modapi] refused to register '%.*s': invalid channel name", 64, channel); return 0; }
    const uint32_t h = fnv1a(channel);
    int freeSlot = -1;
    for (int i = 0; i < kMaxChannels; i++) {
        if (!g_chans[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
        if (!strcmp(g_chans[i].name, channel)) { say("[modapi] refused to register '%s': already registered", channel); return 0; }
        if (g_chans[i].hash == h) { say("[modapi] refused to register '%s': collides with '%s'", channel, g_chans[i].name); return 0; }
    }
    if (freeSlot < 0) { say("[modapi] refused to register '%s': %d channels already registered", channel, kMaxChannels); return 0; }
    Chan& c = g_chans[freeSlot];
    c = Chan{};
    c.used = true; c.serial = ++g_serial;
    memcpy(c.name, channel, (size_t)len + 1);
    c.hash = h; c.onMessage = onMessage; c.onPlayer = onPlayer; c.user = user;
    c.announce = true; c.refillMs = steadyMs();
    g_listDirty = true;
    say("[modapi] registered '%s'", channel);
    return handleOf(freeSlot);
}

void Unregister(int handle) {
    std::lock_guard<std::mutex> lk(g_lock);
    int slot = -1;
    Chan* c = chanOf(handle, &slot);
    if (!c) return;
    say("[modapi] unregistered '%s' (sent %llu, received %llu)", c->name,
         (unsigned long long)c->sent, (unsigned long long)c->received);
    *c = Chan{};
    g_listDirty = true;
}

int Send(int handle, int player, const uint8_t* data, int len, int reliable) {
    std::lock_guard<std::mutex> lk(g_lock);
    int slot = -1;
    Chan* c = chanOf(handle, &slot);
    if (!c) return 0;
    const char* why = nullptr;
    if (!activeNow())                                      why = "not in a session";
    else if (len < 0 || len > kMaxPayload || (len && !data)) why = "payload over 1000 bytes";
    else if (player < -1 || player >= kMaxPeers)           why = "no such player";
    else if ((int)g_outbox.size() >= kOutboxMax)           why = "outbox full";
    if (!why) {
        const uint64_t now = steadyMs();
        const double dt = (double)(now - c->refillMs) / 1000.0;
        c->refillMs = now;
        c->tokMsgs  = c->tokMsgs  + dt * kOutMsgsPerSec;  if (c->tokMsgs  > kOutMsgsBurst)  c->tokMsgs  = kOutMsgsBurst;
        c->tokBytes = c->tokBytes + dt * kOutBytesPerSec; if (c->tokBytes > kOutBytesBurst) c->tokBytes = kOutBytesBurst;
        if (c->tokMsgs < 1.0 || c->tokBytes < (double)len) why = "rate limit";
        else { c->tokMsgs -= 1.0; c->tokBytes -= (double)len; }
    }
    if (why) {
        const bool normal = !activeNow();                                // outside a session is not news
        if (!normal && c->refusedSaid < 5) { c->refusedSaid++; say("[modapi] '%s': send refused (%s)", c->name, why); }
        return 0;
    }
    OutMsg m;
    m.chanSlot = slot; m.serial = c->serial; m.player = player; m.reliable = reliable != 0;
    m.bytes.resize((size_t)(kDataHdr + len));
    memcpy(m.bytes.data(), &kMagic, 4);
    m.bytes[4] = kLaneVersion; m.bytes[5] = kKindData;
    memcpy(m.bytes.data() + kHdr, &c->hash, 4);
    if (len) memcpy(m.bytes.data() + kDataHdr, data, (size_t)len);
    g_outbox.push_back(std::move(m));
    return 1;
}

int InSession() {
    std::lock_guard<std::mutex> lk(g_lock);
    return activeNow() ? 1 : 0;
}

int Players(int handle, int* out, int cap) {
    std::lock_guard<std::mutex> lk(g_lock);
    Chan* c = chanOf(handle);
    if (!c || !activeNow()) return 0;
    int n = 0;
    for (int i = 0; i < kMaxPeers; i++) {
        if (!peerHas(g_peers[i], *c)) continue;
        if (out && n < cap) out[n] = i;
        n++;
    }
    return n;
}

static int authorityLocked(const Chan& c) {
    int best = -1;
    const char* bestId = g_localId;
    if (!activeNow()) return -1;
    for (int i = 0; i < kMaxPeers; i++) {
        const Peer& p = g_peers[i];
        if (!peerHas(p, c)) continue;
        const char* id = authorityId(p);
        if (_stricmp(id, bestId) < 0) { best = i; bestId = id; }     // a tie keeps the earlier: this game
    }
    return best;
}

int Authority(int handle) {
    std::lock_guard<std::mutex> lk(g_lock);
    Chan* c = chanOf(handle);
    return c ? authorityLocked(*c) : -1;
}

int IsAuthority(int handle) {
    std::lock_guard<std::mutex> lk(g_lock);
    Chan* c = chanOf(handle);
    return (c && authorityLocked(*c) == -1) ? 1 : 0;
}

int PlayerName(int player, char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_lock);
    copyOut(out, cap, "");
    if (player < 0 || player >= kMaxPeers) return 0;
    const Peer& p = g_peers[player];
    if (p.used && !p.departed && p.inRoster) { copyOut(out, cap, p.display); return 1; }
    if (g_gone[player].set) { copyOut(out, cap, g_gone[player].name); return 1; }
    return 0;
}

int PlayerId(int player, char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_lock);
    copyOut(out, cap, "");
    if (player < 0 || player >= kMaxPeers) return 0;
    const Peer& p = g_peers[player];
    if (p.used && !p.departed) {
        const char* id = authorityId(p);
        if (id[0]) { copyOut(out, cap, id); return 1; }
    }
    if (g_gone[player].set && g_gone[player].id[0]) { copyOut(out, cap, g_gone[player].id); return 1; }
    return 0;
}

int LocalName(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_lock);
    copyOut(out, cap, g_localName[0] ? g_localName : MpName_Get());
    return 1;
}

int LocalId(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_lock);
    copyOut(out, cap, g_localId);
    return g_localId[0] ? 1 : 0;
}

bool OnGameThread() { return std::this_thread::get_id() == g_gameThread; }

void* PlayerActor(int player) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (player < 0 || player >= kMaxPeers) return nullptr;
    const Peer& p = g_peers[player];
    if (!p.used || p.departed) return nullptr;
    // On the game thread, ask the session: a proxy destroyed since the last frame must not come back.
    return OnGameThread() ? session::PeerActorById(player) : p.actor;
}

int ActorPlayer(void* actor) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!actor) return -1;
    for (int i = 0; i < kMaxPeers; i++)
        if (g_peers[i].used && !g_peers[i].departed && g_peers[i].actor == actor) return i;
    return -1;
}

// ==================================== session ===================================================

void SetLogger(void (*f)(const char*)) { std::lock_guard<std::mutex> lk(g_lock); g_logf = f; }

bool IsPacket(const uint8_t* d, int len) {
    if (!d || len < kHdr) return false;
    uint32_t m = 0; memcpy(&m, d, 4);
    return m == kMagic;
}

void NoteSnapshot(int peerIdx, uint8_t wireMinor) {
    if (wireMinor < kCapableMinor || peerIdx < 0 || peerIdx >= kMaxPeers) return;
    std::vector<Event> ev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        Peer* p = peerFor(peerIdx, ev);
        if (p) p->capable = true;
    }
    fire(ev);
}

void OnPacket(int peerIdx, const uint8_t* d, int len, uint64_t nowUs) {
    if (!IsPacket(d, len) || peerIdx < 0 || peerIdx >= kMaxPeers) return;
    if (d[4] != kLaneVersion) return;                 // a newer lane: nothing here can read it
    std::vector<Event> ev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        Peer* p = peerFor(peerIdx, ev);
        if (!p) return;
        p->capable = true;                           // only a build that knows the lane sends it

        // incoming rate, per peer
        if (!p->inRefillUs) p->inRefillUs = nowUs;
        const double dt = nowUs > p->inRefillUs ? (double)(nowUs - p->inRefillUs) / 1e6 : 0.0;
        p->inRefillUs = nowUs;
        p->inTok += dt * kInMsgsPerSec; if (p->inTok > kInMsgsBurst) p->inTok = kInMsgsBurst;
        if (p->inTok < 1.0) { p->dropped++; return; }
        p->inTok -= 1.0;

        const uint8_t kind = d[5];
        if (kind == kKindList) {
            int r = kHdr;
            if (len < r + 5) return;
            uint16_t gen; memcpy(&gen, d + r, 2); r += 2;
            const int part = d[r++], parts = d[r++];
            const int idLen = d[r++];
            if (parts < 1 || parts > kMaxParts || part >= parts || idLen > kIdMax || len < r + idLen + 1) return;
            char id[kIdMax + 1]; memcpy(id, d + r, (size_t)idLen); id[idLen] = 0; r += idLen;
            const int count = d[r++];
            if (count > kMaxChannels) return;
            NameSet partNames;
            for (int i = 0; i < count; i++) {
                if (len < r + 1) return;
                const int nl = d[r++];
                if (len < r + nl || !validName((const char*)d + r, nl)) return;
                partNames.add((const char*)d + r, nl);
                r += nl;
            }
            if (r != len) return;
            if (!p->stSeen || gen != p->stGen || parts != p->stParts) {
                p->staging = NameSet{}; p->stSeen = 0; p->stGen = gen; p->stParts = (uint8_t)parts;
            }
            if (p->stSeen & (1u << part)) return;
            for (int i = 0; i < partNames.n; i++)
                p->staging.add(partNames.name[i], (int)strlen(partNames.name[i]));
            p->stSeen |= (uint8_t)(1u << part);
            copyOut(p->saysId, sizeof(p->saysId), id);
            if (p->stSeen == (uint8_t)((1u << parts) - 1)) {
                commitList(peerIdx, ev);
                p->lastListUs = nowUs;
                p->stSeen = 0;
            }
        } else if (kind == kKindData) {
            if (len < kDataHdr || len > kPacketMax) return;
            uint32_t h; memcpy(&h, d + kHdr, 4);
            for (auto& c : g_chans) {
                if (!c.used || c.hash != h) continue;
                if (!peerHas(*p, c)) return;           // not announced, or announced under another name
                c.received++;
                if (c.onMessage) {
                    Event e; e.onMessage = c.onMessage; e.user = c.user; e.player = peerIdx;
                    e.data = d + kDataHdr; e.len = len - kDataHdr;
                    ev.push_back(e);
                }
                break;
            }
        }
    }
    fire(ev);                                        // `d` is valid for the duration of this call
}

void Frame(uint64_t nowUs) {
    std::vector<Event> ev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_active = true; g_activeMs = steadyMs();
        g_gameThread = std::this_thread::get_id();
        copyOut(g_localName, sizeof(g_localName), MpName_Get());
        copyOut(g_localId, sizeof(g_localId), MyId());

        // ---- roster: identity, departures
        const int nPeers = PeerCount();
        PeerStats ps;
        for (int i = 0; i < nPeers && i < kMaxPeers; i++) {
            Peer* p = peerFor(i, ev);
            if (!p) continue;
            const bool departed = GetStats(i, &ps) && ps.state == 5;
            if (departed && !p->departed) { peerLeaveAll(i, ev); p->departed = true; }
            else if (!departed && p->departed) { p->departed = false; p->listSentOnce = false; }
        }
        // ---- names and actors from the session, for the thread-safe queries
        for (auto& p : g_peers) { p.inRoster = false; p.actor = nullptr; p.display[0] = 0; }
        for (int s = 0; s < session::PeerSlots(); s++) {
            char nm[40]; void* actor = nullptr; int pid = -1;
            if (!session::PeerAt(s, nm, sizeof(nm), &actor, &pid) || pid < 0 || pid >= kMaxPeers) continue;
            Peer& p = g_peers[pid];
            if (!p.used) continue;
            p.inRoster = true; p.actor = actor; copyOut(p.display, sizeof(p.display), nm);
        }
        // ---- expiry: a peer who stopped announcing has left every channel
        for (int i = 0; i < kMaxPeers; i++) {
            Peer& p = g_peers[i];
            if (p.used && p.haveList && nowUs > p.lastListUs && nowUs - p.lastListUs > kListExpireUs)
                peerLeaveAll(i, ev);
        }
        // ---- newly registered channels: tell them who already has them
        for (auto& c : g_chans) {
            if (!c.used || !c.announce) continue;
            c.announce = false;
            if (!c.onPlayer) continue;
            for (int i = 0; i < kMaxPeers; i++) {
                if (!peerHas(g_peers[i], c)) continue;
                Event e; e.onPlayer = c.onPlayer; e.user = c.user; e.player = i; e.joined = 1;
                ev.push_back(e);
                say("[modapi] player %d joined '%s'", i, c.name);
            }
        }

        int relCount[kMaxPeers] = {};
        int nChans = 0; for (auto& c : g_chans) if (c.used) nChans++;
        // ---- our list: on change, to anyone new, and every 2 s while we have channels
        if (g_listDirty || (nChans && nowUs - g_lastListUs > kListEveryUs)) {
            sendList(-1, relCount);
            g_listDirty = false; g_lastListUs = nowUs;
        } else if (nChans) {
            for (int i = 0; i < nPeers && i < kMaxPeers; i++)
                if (g_peers[i].used && g_peers[i].capable && !g_peers[i].departed && !g_peers[i].listSentOnce)
                    sendList(i, relCount);
        }

        // ---- outbox. Reliable sends yield to the wire's budget; order is kept by stopping, not skipping.
        const int budget = SendBudget();
        const int relCap = budget / 4 > 4 ? budget / 4 : 4;
        size_t done = 0;
        for (; done < g_outbox.size(); done++) {
            OutMsg& m = g_outbox[done];
            Chan& c = g_chans[m.chanSlot];
            if (!c.used || c.serial != m.serial) continue;        // unregistered while queued
            const bool rel = m.reliable || Current() == BK_SHM;   // shm unreliable is latest-wins
            int to[kMaxPeers]; int nTo = 0;
            for (int i = 0; i < nPeers && i < kMaxPeers; i++) {
                if (m.player >= 0 && i != m.player) continue;
                if (peerHas(g_peers[i], c)) to[nTo++] = i;
            }
            bool blocked = false;
            if (rel) for (int k = 0; k < nTo; k++) if (relCount[to[k]] >= relCap) { blocked = true; break; }
            if (blocked) break;
            for (int k = 0; k < nTo; k++) {
                omp::Send(to[k], m.bytes.data(), (int)m.bytes.size(), rel);
                if (rel) relCount[to[k]]++;
            }
            if (nTo) c.sent++;
        }
        g_outbox.erase(g_outbox.begin(), g_outbox.begin() + (std::ptrdiff_t)done);

        // ---- dropped inbound, summarised
        if (nowUs - g_droppedLoggedUs > 30000000ull) {
            g_droppedLoggedUs = nowUs;
            for (int i = 0; i < kMaxPeers; i++)
                if (g_peers[i].dropped) {
                    say("[modapi] player %d: %u mod message(s) dropped over the rate limit", i, g_peers[i].dropped);
                    g_peers[i].dropped = 0;
                }
        }
    }
    fire(ev);
}

void Reset() {
    std::vector<Event> ev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (int i = 0; i < kMaxPeers; i++) { if (g_peers[i].used) peerLeaveAll(i, ev); g_peers[i] = Peer{}; }
        g_outbox.clear();
        g_active = false;
        g_listDirty = true;
        for (auto& c : g_chans) if (c.used) c.announce = false;
    }
    fire(ev);
}

}} // namespace omp::modapi
