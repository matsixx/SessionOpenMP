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
// SessionOpenMP -- SHARED-MEMORY transport backend: the SAME-PC development rig.
//
// WHY THIS EXISTS AT ALL: EOS cannot test two instances on one machine. Both copies log in with the
// same device id, get the SAME ProductUserId, and there is no second player to address. A named
// mapping needs no login, no sockets, no ports, no firewall and no internet, and it is instant.
//
// THE DESIGN:
//   * N SLOTS, one per process. Each process WRITES only its own slot and READS every other.
//   * LATEST-WINS per slot, not a queue: a transform stream has no backlog worth keeping, only a
//     newest value. (The Stream's snapshot ring does the buffering, on the receiving side.)
//   * SEQLOCK against torn reads: the writer bumps seq ODD, writes, bumps EVEN; a reader retries
//     while seq is odd or changed under it. No locks, no blocking, safe across processes.
//   * SLOTS ARE CLAIMED BY PID, never assigned by role. An atomic compare-exchange cannot disagree
//     between two processes; a role value owned by another subsystem can, and when it does both
//     processes pick the same slot and neither ever receives anything.
//   * DEAD OWNERS ARE RECLAIMED (pidAlive): without it every crash or restart burns a slot forever.
//   * IDENTITY IS THE SLOT -- established by the transport, never claimed inside a payload. Same rule
//     as the EOS backend's peer index, for the same reason.
//   * THE MAPPING NAME CARRIES A VERSION: a mapping's SIZE is fixed by whoever creates it FIRST, so a
//     stale mapping from an older layout, held open by a surviving process, would silently cap this
//     one. Bump the name on any layout change.
#include "transport.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace omp { namespace shmb {

static const int      kSlots   = 16;
static const int      kMaxMsg  = 1024;               // > our packet (State ~= 470 B) with room to grow
static const int      kRelRing = 64;                 // reliable messages in flight per sender. Sized
                                                     // for the replay-sync bulk transfer: its chunk
                                                     // burst is capped at kRelRing/2 per tick (see
                                                     // SendBudget), so one frame's burst can never
                                                     // lap a reader that drains every poll. The old
                                                     // 8 lost the transfer's header EVERY frame.
static const char*    kMapName = "Local\\SessionOpenMP_Mailbox_v3";   // v3: kRelRing 8 -> 64

// WHY THERE ARE TWO CHANNELS.
// The mailbox is LATEST-WINS by design: for 60 Hz pose snapshots you want the newest one and nothing
// else, and a backlog would be worse than useless. Whole MESSAGES are the opposite case -- cosmetics
// send a pair of packets in one frame, and on a latest-wins slot the second overwrites the first
// before the reader's next poll, so one section is lost every time. So the unreliable path below is
// latest-wins on one slot, and reliable messages get their own small ring that the reader drains
// COMPLETELY every poll.
struct RelMsg { volatile uint32_t ticket; uint32_t len; uint8_t data[kMaxMsg]; };
struct Slot {
    volatile uint32_t seq; uint32_t len; uint8_t data[kMaxMsg];   // unreliable: latest-wins
    volatile uint32_t relHead;                                     // reliable messages ever written
    RelMsg   rel[kRelRing];
};
struct Shared { volatile LONG owner[kSlots]; Slot slot[kSlots]; };

static HANDLE   g_map = nullptr;
static Shared*  g_mem = nullptr;
static int      g_mine = -1;
static char     g_myId[32] = {0};
static bool     g_inSession = false;                 // host/join pressed: gates publishing AND receiving

// Peers are DISCOVERED here, not added. A dense peer list keeps indices contiguous for the session
// (which enumerates [0, PeerCount)) while the SLOT remains the real identity -- and an index, once
// handed out, is never reused or renumbered, so a proxy can never inherit another player's stream.
struct SPeer {
    int       slot = -1;
    PeerStats st{};
    uint32_t  lastSeq = 0;
    uint32_t  lastRel = 0;                           // reliable tickets consumed from this peer
    bool      relPrimed = false;                     // ignore whatever predates our arrival
    uint64_t  lastRecvMs = 0;
};
static SPeer g_peers[kSlots];
static int   g_nPeers = 0;
static int   g_nextRead = 0;                         // round-robin: a fast sender must not starve others

// A recycled PID can read as alive, which is harmless -- worst case we skip a slot we could have taken.
static bool pidAlive(LONG pid) {
    if (pid <= 0) return false;
    if (pid == (LONG)GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // exists; we just cannot query it
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) ? (code == STILL_ACTIVE) : true;
    CloseHandle(h);
    return alive;
}

static int peerForSlot(int slot) {
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].slot == slot) return i;
    if (g_nPeers >= kSlots) return -1;
    SPeer& p = g_peers[g_nPeers];
    p = SPeer{}; p.slot = slot;
    p.st.state = 1;                                   // shared memory is as DIRECT as a link gets
    Log("[shm] peer #%d = slot %d", g_nPeers, slot);
    return g_nPeers++;
}

bool Init(bool /*forceRelays*/) {
    if (g_mem) return true;
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kMapName);
    if (!g_map) { Log("[shm] CreateFileMapping failed (%lu)", GetLastError()); return false; }
    g_mem = (Shared*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!g_mem) { Log("[shm] MapViewOfFile failed (%lu)", GetLastError()); return false; }

    const LONG pid = (LONG)GetCurrentProcessId();
    for (int i = 0; i < kSlots && g_mine < 0; i++) {
        const LONG prev = InterlockedCompareExchange(&g_mem->owner[i], pid, 0);
        if (prev == 0 || prev == pid) g_mine = i;      // claimed, or it was already ours
    }
    if (g_mine < 0) {                                 // all held: reclaim one whose owner is gone
        for (int i = 0; i < kSlots && g_mine < 0; i++) {
            const LONG prev = g_mem->owner[i];
            if (pidAlive(prev)) continue;
            if (InterlockedCompareExchange(&g_mem->owner[i], pid, prev) == prev) {
                g_mine = i;
                Log("[shm] reclaimed slot %d from dead pid %ld", i, (long)prev);
            }
        }
    }
    if (g_mine < 0) { Log("[shm] *** all %d slots held by LIVE processes -- cannot publish", kSlots); return false; }

    // Our slot may hold a previous process's bytes; start it clean so nobody reads a stale frame.
    g_mem->slot[g_mine].seq = 0; g_mem->slot[g_mine].len = 0;
    snprintf(g_myId, sizeof(g_myId), "shm:%d", g_mine);
    Log("[shm] mailbox ready: slot %d of %d is ours (pid %ld) -- no login, no network", g_mine, kSlots, (long)pid);
    return true;
}

void Shutdown() {
    if (g_mem) {
        if (g_mine >= 0) {
            g_mem->slot[g_mine].seq = 0; g_mem->slot[g_mine].len = 0;
            InterlockedCompareExchange(&g_mem->owner[g_mine], 0, (LONG)GetCurrentProcessId());
        }
        UnmapViewOfFile(g_mem); g_mem = nullptr;
    }
    if (g_map) { CloseHandle(g_map); g_map = nullptr; }
    g_mine = -1; g_nPeers = 0; g_inSession = false;
}

const char* MyId() { return g_myId; }
int  AddPeer(const char*) { return -1; }              // discovered, never added
int  PeerCount() { return g_nPeers; }

// Every peer gets the same bytes, so a "send" is ONE write to our own slot regardless of peer count.
// (The session still calls Send per peer; the redundant writes are harmless and keep the seam honest.
// Guarding on peer 0 would tie the wire format to the session's iteration order.)
void Send(int peerIdx, const void* data, int len, bool reliable) {
    if (!g_mem || g_mine < 0 || !g_inSession) return;
    if (len <= 0 || len > kMaxMsg) return;
    if (peerIdx < 0 || peerIdx >= g_nPeers || g_peers[peerIdx].st.state == 5) return;
    if (peerIdx != 0) { g_peers[peerIdx].st.sent++; return; }   // already published this frame

    if (reliable) {
        // Append to the ring. Each entry carries its own TICKET (the write ordinal, never 0), which is
        // both the reader's sequencing key and its torn-write guard: cleared before the payload lands,
        // stamped after, so a reader that sees the ticket has seen the whole message.
        Slot& rs = g_mem->slot[g_mine];
        const uint32_t ticket = rs.relHead + 1;
        RelMsg& m = rs.rel[(ticket - 1) % kRelRing];
        m.ticket = 0;
        MemoryBarrier();
        m.len = (uint32_t)len;
        memcpy(m.data, data, (size_t)len);
        MemoryBarrier();
        m.ticket = ticket;
        MemoryBarrier();
        rs.relHead = ticket;
        g_peers[peerIdx].st.sent++;
        return;
    }

    Slot& s = g_mem->slot[g_mine];
    const uint32_t q = s.seq + 1;
    s.seq = q;                                        // odd => write in progress
    MemoryBarrier();
    s.len = (uint32_t)len;
    memcpy(s.data, data, (size_t)len);
    MemoryBarrier();
    s.seq = q + 1;                                    // even => complete
    g_peers[peerIdx].st.sent++;
}

void Tick(RecvFn onRecv, void* user) {
    if (!g_mem) return;
    // The session gate has to hold in BOTH directions. `Send` checks g_inSession so a solo instance
    // cannot publish; receiving is gated here for the same reason -- an instance that never pressed
    // Host or Join must not discover the mailbox's other occupants, drain their frames and hand them
    // to the session, which would spawn a proxy of a player who never agreed to a session.
    // Leaving is unaffected: LobbyLeave already retires every peer to state 5 before this stops running.
    if (!g_inSession) return;
    const uint64_t now = GetTickCount64();

    // ---- discover joiners and retire leavers by watching slot ownership.
    for (int i = 0; i < kSlots; i++) {
        if (i == g_mine) continue;
        const LONG owner = g_mem->owner[i];
        if (owner != 0 && pidAlive(owner)) { peerForSlot(i); continue; }
        for (int p = 0; p < g_nPeers; p++)
            if (g_peers[p].slot == i && g_peers[p].st.state != 5) {
                g_peers[p].st.state = 5;              // DEPARTED: index stays valid, sends stop
                Log("[shm] peer #%d (slot %d) left -- owner pid %ld gone", p, i, (long)owner);
            }
    }

    // ---- drain one fresh frame per peer, round-robin so a fast writer cannot starve a slow one.
    for (int k = 0; k < g_nPeers; k++) {
        const int pi = (g_nextRead + k) % (g_nPeers > 0 ? g_nPeers : 1);
        SPeer& p = g_peers[pi];
        if (p.slot < 0 || p.st.state == 5) continue;
        Slot& s = g_mem->slot[p.slot];
        // ---- RELIABLE first, and drain it COMPLETELY: these are whole messages, not successive
        // views of one value, so "take the newest" is data loss.
        {
            const uint32_t head = s.relHead;
            if (!p.relPrimed) {                       // whatever predates us is not ours to replay
                p.lastRel = head; p.relPrimed = true;
            } else if (head > p.lastRel) {
                // Lapped: the writer outran the ring. Skip to the oldest entry still present and log
                // it -- a gap in a reliable channel must never be silent.
                if (head - p.lastRel > (uint32_t)kRelRing) {
                    Log("[shm] peer #%d: reliable ring lapped, %u message(s) dropped",
                        pi, (unsigned)(head - p.lastRel - kRelRing));
                    p.lastRel = head - kRelRing;
                }
                while (p.lastRel < head) {
                    const uint32_t ticket = p.lastRel + 1;
                    RelMsg& m = s.rel[(ticket - 1) % kRelRing];
                    const uint32_t t0 = m.ticket;
                    if (t0 != ticket) { p.lastRel = ticket; continue; }   // overwritten under us: skip
                    uint32_t n = m.len; if (n > kMaxMsg) n = kMaxMsg;
                    uint8_t tmp[kMaxMsg];
                    memcpy(tmp, m.data, n);
                    MemoryBarrier();
                    if (m.ticket != ticket) { p.lastRel = ticket; continue; }   // torn: drop, not retry
                    p.lastRel = ticket; p.st.recv++; p.lastRecvMs = now;
                    if (onRecv) onRecv(pi, tmp, (int)n, user);
                }
            }
        }
        for (int tries = 0; tries < 4; tries++) {
            const uint32_t a = s.seq;
            if (a & 1) continue;                       // writer mid-update: retry
            if (a == 0 || a == p.lastSeq) break;       // nothing new in this slot
            uint32_t n = s.len; if (n > kMaxMsg) n = kMaxMsg;
            uint8_t tmp[kMaxMsg];
            memcpy(tmp, s.data, n);
            MemoryBarrier();
            if (s.seq != a) continue;                  // torn under us: retry
            p.lastSeq = a; p.st.recv++; p.lastRecvMs = now;
            g_nextRead = (pi + 1) % (g_nPeers > 0 ? g_nPeers : 1);
            if (onRecv) onRecv(pi, tmp, (int)n, user);
            break;
        }
    }
}

bool GetStats(int idx, PeerStats* out) {
    if (idx < 0 || idx >= g_nPeers || !out) return false;
    SPeer& p = g_peers[idx];
    p.st.lastRecvAgoMs = p.lastRecvMs ? (double)(GetTickCount64() - p.lastRecvMs) : -1.0;
    *out = p.st;
    return true;
}

// ---- "lobby": claiming a slot IS joining, so host and join are the same act. The gate exists because
// two INDEPENDENT solo instances on one PC would otherwise cross-publish over the mailbox and each
// would spawn a proxy of a player who never agreed to a session.
bool LobbyHost() { if (!g_mem || g_mine < 0) return false; g_inSession = true; Log("[shm] session OPEN (slot %d)", g_mine); return true; }
bool LobbyJoin() { return LobbyHost(); }
void LobbyLeave() {
    g_inSession = false;
    if (g_mem && g_mine >= 0) { g_mem->slot[g_mine].seq = 0; g_mem->slot[g_mine].len = 0; }
    for (int i = 0; i < g_nPeers; i++) g_peers[i].st.state = 5;
    Log("[shm] session CLOSED");
}
int LobbyStatus() { return (g_mem && g_mine >= 0 && g_inSession) ? 2 : 0; }

// ---- trust. This wire can prove exactly one thing, and it proves it well: the sender
// is a LIVE process on this machine holding a claimed mailbox slot. It cannot prove WHO -- there is no
// login and no identity beyond the slot -- so TRUST_LOCAL is the ceiling and it is the honest answer.
// Tick() already retires a peer to state 5 the moment its slot owner's PID is gone, so the state byte
// IS the liveness answer; re-deriving it here would just be a second, disagreeing source.
TrustLevel PeerTrust(int idx) {
    if (idx < 0 || idx >= g_nPeers) return TRUST_NONE;
    return (g_peers[idx].st.state == 5) ? TRUST_NONE : TRUST_LOCAL;
}
bool SessionOpen() { return g_inSession; }

}} // namespace omp::shmb
