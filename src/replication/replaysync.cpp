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
// SessionOpenMP -- replay sync (see the header for the design).
#include "replaysync.h"
#include <cstring>
#include <cstdio>

namespace omp { namespace replaysync {

// Same "OMP?" namespace as the snapshot (OMPI), cosmetics (OMPG) and chat (OMPH) magics -- see the
// lineage note at the top of replication.cpp. Every sync packet is magic + a subtype byte.
static const uint32_t kMagic = 0x4A504D4Fu;   // "OMPJ"
// kAck is a later addition (flow control); an older build's `default: break` ignores it, and an
// older requester that never ACKs still completes against a new sender through NAK resends, which
// bypass the window. Subtype extension only -- the OMPJ magic is unchanged.
enum : uint8_t { kReq = 1, kHdr = 2, kChunk = 3, kNak = 4, kDone = 5, kCancel = 6, kAck = 7 };

static Tuning g_tun;
Tuning& Tune() { return g_tun; }

static SendFn g_send = nullptr;
void SetSendFn(SendFn fn) { g_send = fn; }
static int g_budget = 0;
void SetChunkBudget(int budget) { g_budget = budget; }

bool IsSyncPacket(const uint8_t* d, int len) {
    if (!d || len < 5) return false;
    uint32_t m; memcpy(&m, d, 4);
    return m == kMagic;
}

// ---- little-endian scribblers. The wire is x64-only on both ends today, but going through memcpy
// keeps every access alignment-safe on the receive path, where offsets are packet-controlled.
static void wU16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void wU32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void wU64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t rU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rU64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

// ==== THE OWN RING ===================================================================================
// A circular byte arena of packed snapshots plus an entry index. Entries are addressed by a
// monotonically increasing VIRTUAL byte offset (arena index = off % kArena); a packet is never
// split across the wrap -- if it will not fit contiguously the writer skips to the next lap and the
// tail entries covering the skipped gap are dropped with the overwritten ones.
// Entries are FAT snapshots (drivers + anim blob + the CAPTURED 70-bone pose, ~1.3 KB at cap 2048):
// during the requester's local replay playback the anim graph cannot evaluate a skater (thrice-
// measured -- the heap-of-clothes), so the transferred history must carry the finished skeleton and
// the receiver stamps bones at FinalizeBones, the one path proven to work in playback. ~120 s at
// 60 Hz of ~1.3 KB is ~9.4 MB; the 10 MB arena is the limiting window in practice.
static const int      kArena       = 10 << 20;
static const int      kIdxCap      = 16384;         // > 120 s x 60 Hz, power of two for cheap mod
static const uint64_t kRingSpanUs  = 120ull * 1000 * 1000;
static uint8_t  g_arena[kArena];
struct OwnEntry { uint64_t vOff = 0; uint64_t us = 0; uint16_t len = 0; };   // len <= 2048
static OwnEntry g_own[kIdxCap];
static int      g_ownHead = 0, g_ownCount = 0;      // ring: head = next write slot
static uint64_t g_vHead = 0;                        // next virtual byte offset

static void ownPopTail() { if (g_ownCount > 0) g_ownCount--; }
static OwnEntry& ownAt(int fromTail) {              // 0 = oldest
    return g_own[(g_ownHead - g_ownCount + fromTail + 2 * kIdxCap) % kIdxCap];
}

void RecordOwn(const uint8_t* pkt, int len, uint64_t senderUs) {
    if (!pkt || len <= 0 || len > 2048) return;
    // never split across the wrap: skip the tail gap if the packet will not fit contiguously
    uint64_t v = g_vHead;
    if ((int)(v % kArena) + len > kArena) v += kArena - (v % kArena);
    // drop tail entries the new write (or the skipped gap) overwrites, plus anything out of the
    // time window or an over-full index
    while (g_ownCount > 0 &&
           (ownAt(0).vOff + kArena < v + (uint64_t)len ||          // lapped: bytes being overwritten
            senderUs - ownAt(0).us > kRingSpanUs ||                // aged out of the window
            g_ownCount >= kIdxCap - 1))                            // index full
        ownPopTail();
    memcpy(g_arena + (v % kArena), pkt, (size_t)len);
    OwnEntry& e = g_own[g_ownHead];
    e.vOff = v; e.us = senderUs; e.len = (uint16_t)len;
    g_ownHead = (g_ownHead + 1) % kIdxCap;
    if (g_ownCount < kIdxCap) g_ownCount++;
    g_vHead = v + (uint64_t)len;
}

// ==== OUTGOING TRANSFERS (we are the owner) ==========================================================
// The serialized blob is [u16 len][u64 us][bytes] per entry, oldest first, built once per REQ so the
// ring can keep rolling underneath. Chunks are paced from Tick; a NAK queues resends.
static const int kChunkData  = 900;                  // + 13 B header stays under the ~1 KB transport cap
static const int kOutMax     = 4;                    // concurrent requesters (same-session peers)
static const int kResendCap  = 512;
struct OutXfer {
    bool     active = false;
    int      peerIdx = -1;
    uint32_t reqId = 0;
    uint8_t* buf = nullptr;
    uint32_t total = 0, entryCount = 0;
    uint16_t chunkCount = 0;
    uint64_t newestUs = 0, oldestUs = 0;
    uint32_t sweep = 0;                              // first pass cursor
    uint32_t swept = 0;                              // sweep chunks SENT (resends excluded -- see Tick)
    uint32_t ackedGot = 0;                           // requester's reported received count (monotonic)
    uint16_t resendQ[kResendCap]; int rHead = 0, rCount = 0;
    uint64_t lastActUs = 0;
    uint64_t lastHdrUs = 0;                          // periodic HDR re-sends until the requester speaks
    bool     heardBack = false;                      // a NAK arrived: the HDR landed, stop repeating it
};
static OutXfer g_out[kOutMax];

static void outFree(OutXfer& x) { delete[] x.buf; x = OutXfer{}; }

static void sendHdr(const OutXfer& x) {
    uint8_t p[5 + 4 + 4 + 2 + 2 + 4 + 8 + 8];
    wU32(p, kMagic); p[4] = kHdr;
    wU32(p + 5, x.reqId); wU32(p + 9, x.total);
    wU16(p + 13, x.chunkCount); wU16(p + 15, (uint16_t)kChunkData);
    wU32(p + 17, x.entryCount); wU64(p + 21, x.newestUs); wU64(p + 29, x.oldestUs);
    if (g_send) g_send(x.peerIdx, p, (int)sizeof(p), true);
}

static void sendChunk(OutXfer& x, uint16_t idx) {
    if (idx >= x.chunkCount) return;
    const uint32_t off = (uint32_t)idx * kChunkData;
    uint16_t dlen = (uint16_t)((x.total - off) < kChunkData ? (x.total - off) : kChunkData);
    uint8_t p[13 + kChunkData];
    wU32(p, kMagic); p[4] = kChunk;
    wU32(p + 5, x.reqId); wU16(p + 9, idx); wU16(p + 11, dlen);
    memcpy(p + 13, x.buf + off, dlen);
    if (g_send) g_send(x.peerIdx, p, 13 + (int)dlen, true);
}

static void beginOutgoing(int peerIdx, uint32_t reqId, uint64_t nowUs, void (*logf)(const char*)) {
    // A repeat REQ with the SAME id is the requester saying "I never saw your header" -- re-send
    // the header, keep the transfer exactly where it is. Only a NEW id (a fresh toggle) rebuilds.
    // The old restart-on-every-REQ turned the requester's 3 s retry into a permanent reset loop
    // whenever the header kept getting lost.
    for (auto& o : g_out) if (o.active && o.peerIdx == peerIdx) {
        if (o.reqId == reqId) { sendHdr(o); o.lastHdrUs = nowUs; o.lastActUs = nowUs; return; }
        outFree(o);
    }
    OutXfer* x = nullptr;
    for (auto& o : g_out) if (!o.active) { x = &o; break; }
    if (!x || g_ownCount < 2) return;                // nothing worth sending
    // DOWNSAMPLED to ~30 Hz (historyMinGapMs): the cadence the live pose lane already serves a
    // scrubbing peer at, and the scrub-side interp blends between entries -- half the bytes, same
    // scrub fidelity. The newest entry is always kept: it anchors the timeline mapping.
    const uint64_t minGapUs = (uint64_t)(g_tun.historyMinGapMs * 1000.f);
    uint32_t total = 0, kept = 0;
    uint64_t lastKeptUs = 0;
    for (int i = 0; i < g_ownCount; i++) {
        OwnEntry& e = ownAt(i);
        if (i > 0 && i != g_ownCount - 1 && e.us - lastKeptUs < minGapUs) continue;
        total += 10u + e.len; kept++; lastKeptUs = e.us;
    }
    if (kept < 2) return;
    x->buf = new uint8_t[total];
    uint32_t w = 0; uint64_t firstUs = 0, lastUs = 0;
    bool haveFirst = false;
    lastKeptUs = 0;
    for (int i = 0; i < g_ownCount; i++) {
        OwnEntry& e = ownAt(i);
        if (i > 0 && i != g_ownCount - 1 && e.us - lastKeptUs < minGapUs) continue;
        wU16(x->buf + w, e.len); wU64(x->buf + w + 2, e.us);
        memcpy(x->buf + w + 10, g_arena + (e.vOff % kArena), e.len);
        w += 10u + e.len; lastKeptUs = e.us;
        if (!haveFirst) { firstUs = e.us; haveFirst = true; }
        lastUs = e.us;
    }
    x->active = true; x->peerIdx = peerIdx; x->reqId = reqId;
    x->total = w; x->entryCount = kept;
    x->chunkCount = (uint16_t)((w + kChunkData - 1) / kChunkData);
    x->oldestUs = firstUs; x->newestUs = lastUs;
    x->sweep = 0; x->swept = 0; x->ackedGot = 0; x->rHead = x->rCount = 0; x->lastActUs = nowUs;
    x->lastHdrUs = nowUs; x->heardBack = false;
    sendHdr(*x);
    if (logf) { char m[160];
        snprintf(m, sizeof(m), "[sync] peer %d requested our replay history -- sending %u KB"
                 " (%u snapshots, %.0f s)", peerIdx, x->total / 1024, x->entryCount,
                 (double)(x->newestUs - x->oldestUs) / 1e6);
        logf(m); }
}

// ==== THE INCOMING TRANSFER (we are the requester) ===================================================
// One in flight at a time (the menu syncs one player per toggle); completed buffers are kept
// per-peer until DropAll. All fields of every packet are validated -- P2P input is untrusted.
static const uint32_t kMaxTotal = 12u << 20;
struct ReadyBuf {
    bool     used = false;
    int      peerIdx = -1;
    uint8_t* blob = nullptr;                          // the raw serialized entries
    struct Entry { uint32_t off; uint16_t len; uint64_t us; };
    Entry*   idx = nullptr;
    uint32_t count = 0;
    uint64_t newestUs = 0, oldestUs = 0;
};
static const int kReadyMax = 8;
static ReadyBuf g_ready[kReadyMax];

struct InXfer {
    bool     active = false, haveHdr = false;
    int      peerIdx = -1;
    uint32_t reqId = 0;
    uint8_t* buf = nullptr;
    uint8_t* have = nullptr;                          // one byte per chunk (bitmap without the bit math)
    uint32_t total = 0, entryCount = 0, got = 0;
    uint16_t chunkCount = 0, chunkSize = 0;
    uint64_t newestUs = 0, oldestUs = 0;
    uint64_t startUs = 0, lastActUs = 0, lastNakUs = 0, lastAckUs = 0;
    int      reqRetries = 0;
    bool     failed = false;
};
static InXfer g_in;

static void inFree() { delete[] g_in.buf; delete[] g_in.have; g_in = InXfer{}; }

static ReadyBuf* readyFor(int peerIdx) {
    for (auto& r : g_ready) if (r.used && r.peerIdx == peerIdx) return &r;
    return nullptr;
}
static void readyFree(ReadyBuf& r) { delete[] r.blob; delete[] r.idx; r = ReadyBuf{}; }

// Parse the completed blob into a timestamp index. Every offset is re-validated against the blob we
// actually assembled; timestamps must be non-decreasing (the sender wrote them oldest-first) or the
// buffer is rejected outright.
static bool finishIncoming(void (*logf)(const char*)) {
    ReadyBuf* slot = readyFor(g_in.peerIdx);
    if (!slot) for (auto& r : g_ready) if (!r.used) { slot = &r; break; }
    if (!slot) return false;
    readyFree(*slot);
    ReadyBuf::Entry* idx = new ReadyBuf::Entry[g_in.entryCount];
    uint32_t w = 0, n = 0;
    uint64_t prevUs = 0;
    while (w + 10 <= g_in.total && n < g_in.entryCount) {
        const uint16_t len = rU16(g_in.buf + w);
        const uint64_t us  = rU64(g_in.buf + w + 2);
        if (len == 0 || len > 2048 || w + 10u + len > g_in.total || us < prevUs) break;
        idx[n].off = w + 10; idx[n].len = len; idx[n].us = us;
        prevUs = us; n++; w += 10u + len;
    }
    if (n != g_in.entryCount || w != g_in.total || n < 2) { delete[] idx; return false; }
    slot->used = true; slot->peerIdx = g_in.peerIdx;
    slot->blob = g_in.buf; g_in.buf = nullptr;        // ownership moves
    slot->idx = idx; slot->count = n;
    slot->oldestUs = idx[0].us; slot->newestUs = idx[n - 1].us;
    if (logf) { char m[160];
        snprintf(m, sizeof(m), "[sync] peer %d replay history complete: %u snapshots covering %.0f s",
                 g_in.peerIdx, n, (double)(slot->newestUs - slot->oldestUs) / 1e6);
        logf(m); }
    return true;
}

static void sendSimple(int peerIdx, uint8_t type, uint32_t reqId) {
    uint8_t p[9];
    wU32(p, kMagic); p[4] = type; wU32(p + 5, reqId);
    if (g_send) g_send(peerIdx, p, 9, true);
}

bool RequestSync(int peerIdx, uint64_t nowUs) {
    if (g_in.active) return false;
    if (ReadyBuf* r = readyFor(peerIdx)) readyFree(*r);   // a re-sync replaces the old window
    g_in = InXfer{};
    g_in.active = true; g_in.peerIdx = peerIdx;
    g_in.reqId = (uint32_t)(nowUs ^ (nowUs >> 17)) | 1u;
    g_in.startUs = g_in.lastActUs = nowUs;
    sendSimple(peerIdx, kReq, g_in.reqId);
    return true;
}

void CancelSync(int peerIdx) {
    if (g_in.active && g_in.peerIdx == peerIdx) { sendSimple(peerIdx, kCancel, g_in.reqId); inFree(); }
    if (ReadyBuf* r = readyFor(peerIdx)) readyFree(*r);
}

SyncState PeerSyncState(int peerIdx, int* pctOut) {
    if (pctOut) *pctOut = 0;
    if (g_in.active && g_in.peerIdx == peerIdx) {
        if (g_in.failed) return SyncState::Failed;
        if (pctOut && g_in.chunkCount) *pctOut = (int)(100u * g_in.got / g_in.chunkCount);
        return SyncState::Transferring;
    }
    if (readyFor(peerIdx)) return SyncState::Ready;
    return SyncState::None;
}

void DropAll() {
    if (g_in.active) sendSimple(g_in.peerIdx, kCancel, g_in.reqId);
    inFree();
    for (auto& r : g_ready) readyFree(r);
    for (auto& o : g_out) outFree(o);
}

// ==== PACKET PUMP ====================================================================================
void OnPacket(int peerIdx, const uint8_t* d, int len, uint64_t nowUs, void (*logf)(const char*)) {
    if (!IsSyncPacket(d, len)) return;
    const uint8_t type = d[4];
    if (len < 9) return;
    const uint32_t reqId = rU32(d + 5);
    switch (type) {
    case kReq:
        beginOutgoing(peerIdx, reqId, nowUs, logf);
        break;
    case kCancel:
        for (auto& o : g_out) if (o.active && o.peerIdx == peerIdx && o.reqId == reqId) outFree(o);
        if (g_in.active && g_in.peerIdx == peerIdx && g_in.reqId == reqId) inFree();
        break;
    case kDone:
        for (auto& o : g_out) if (o.active && o.peerIdx == peerIdx && o.reqId == reqId) outFree(o);
        break;
    case kNak: {
        if (len < 11) return;
        const int n = rU16(d + 9);
        if (n <= 0 || n > 96 || len < 11 + n * 2) return;
        for (auto& o : g_out) if (o.active && o.peerIdx == peerIdx && o.reqId == reqId) {
            for (int i = 0; i < n && o.rCount < kResendCap; i++) {
                const uint16_t idx = rU16(d + 11 + i * 2);
                if (idx < o.chunkCount) { o.resendQ[(o.rHead + o.rCount) % kResendCap] = idx; o.rCount++; }
            }
            o.lastActUs = nowUs; o.heardBack = true;   // they have the header: NAKs name chunks
        }
        break;
    }
    case kAck: {
        // The requester's progress report: its unique-received chunk count. Monotonic on this side --
        // ACKs ride the reliable channel but a stale count must never re-widen the outstanding figure.
        if (len < 13) return;
        const uint32_t got = rU32(d + 9);
        for (auto& o : g_out) if (o.active && o.peerIdx == peerIdx && o.reqId == reqId) {
            if (got > o.ackedGot && got <= o.chunkCount) o.ackedGot = got;
            o.lastActUs = nowUs; o.heardBack = true;   // an ACK proves the header landed too
        }
        break;
    }
    case kHdr: {
        if (!g_in.active || g_in.peerIdx != peerIdx || g_in.reqId != reqId || g_in.haveHdr) return;
        if (len < 37) return;
        const uint32_t total = rU32(d + 9);
        const uint16_t chunkCount = rU16(d + 13), chunkSize = rU16(d + 15);
        const uint32_t entryCount = rU32(d + 17);
        const uint64_t newest = rU64(d + 21), oldest = rU64(d + 29);
        if (!total || total > kMaxTotal || !chunkCount || chunkSize < 64 || chunkSize > 1024 ||
            (uint32_t)(chunkCount - 1) * chunkSize >= total ||
            (uint32_t)chunkCount * chunkSize < total ||
            !entryCount || entryCount > 65536 || newest < oldest) { g_in.failed = true; return; }
        g_in.haveHdr = true;
        g_in.total = total; g_in.chunkCount = chunkCount; g_in.chunkSize = chunkSize;
        g_in.entryCount = entryCount; g_in.newestUs = newest; g_in.oldestUs = oldest;
        g_in.buf = new uint8_t[total];
        g_in.have = new uint8_t[chunkCount](); // zeroed
        g_in.lastActUs = nowUs;
        break;
    }
    case kChunk: {
        if (!g_in.active || !g_in.haveHdr || g_in.peerIdx != peerIdx || g_in.reqId != reqId) return;
        if (len < 13) return;
        const uint16_t idx = rU16(d + 9), dlen = rU16(d + 11);
        if (idx >= g_in.chunkCount || len < 13 + dlen) return;
        const uint32_t off = (uint32_t)idx * g_in.chunkSize;
        const uint32_t expect = (g_in.total - off) < g_in.chunkSize ? (g_in.total - off) : g_in.chunkSize;
        if (dlen != expect) return;
        if (!g_in.have[idx]) {
            memcpy(g_in.buf + off, d + 13, dlen);
            g_in.have[idx] = 1; g_in.got++;
        }
        g_in.lastActUs = nowUs;
        if (g_in.got == g_in.chunkCount) {
            const int peer = g_in.peerIdx; const uint32_t rid = g_in.reqId;
            const bool ok = finishIncoming(logf);
            sendSimple(peer, ok ? kDone : kCancel, rid);
            if (!ok && logf) logf("[sync] transfer completed but the blob failed validation -- dropped");
            inFree();
        }
        break;
    }
    default: break;
    }
}

void Tick(uint64_t nowUs, void (*logf)(const char*)) {
    // ---- pace outgoing chunks: resends first (they are the requester's actual holes), then the sweep
    for (auto& o : g_out) {
        if (!o.active) continue;
        // Until the requester has spoken (a NAK proves the header landed), repeat the header every
        // ~500 ms: the header shares the reliable channel with the chunk burst, and on a lossy or
        // ring-buffered wire the FIRST copy is the likeliest casualty.
        if (!o.heardBack && nowUs - o.lastHdrUs > 500000ull) { sendHdr(o); o.lastHdrUs = nowUs; }
        int budget = g_tun.chunksPerTick;
        if (g_budget > 0 && budget > g_budget) budget = g_budget;
        if (!o.heardBack && budget > 1) budget -= 1;   // leave room for the repeated header
        // Resends first and OUTSIDE the window: the requester named these holes, so it has room for
        // them, and they are what re-opens a loss-narrowed window (got rises -> ACK -> sweep moves).
        while (budget > 0 && o.rCount > 0) {
            sendChunk(o, o.resendQ[o.rHead]); o.rHead = (o.rHead + 1) % kResendCap; o.rCount--; budget--;
        }
        // The SWEEP is window-gated: at most windowChunks beyond the requester's last reported
        // receive count in flight. `swept` counts sweep sends only -- a resend must not inflate the
        // outstanding figure, since a chunk the requester already had leaves `got` unchanged and the
        // window would ratchet shut on every recovered loss. SIGNED and clamped: a quiet-period NAK
        // names every missing chunk including not-yet-swept ones, so resends can push `got` PAST
        // `swept` -- unsigned math would wrap that into a permanently shut window.
        while (budget > 0 && o.sweep < o.chunkCount) {
            int outstanding = (int)o.swept - (int)o.ackedGot;
            if (outstanding < 0) outstanding = 0;
            if (outstanding >= g_tun.windowChunks) break;
            sendChunk(o, (uint16_t)o.sweep++); o.swept++; budget--;
        }
        if (o.sweep >= o.chunkCount && o.rCount == 0 &&
            nowUs - o.lastActUs > (uint64_t)(g_tun.stallTimeoutMs * 1000.f))
            outFree(o);                              // requester went quiet; DONE/NAK never came
    }
    // ---- requester: REQ retry, NAK holes, stall failure
    if (g_in.active && !g_in.failed) {
        if (!g_in.haveHdr) {
            if (nowUs - g_in.lastActUs > (uint64_t)(g_tun.hdrTimeoutMs * 1000.f)) {
                if (++g_in.reqRetries > 3) {
                    g_in.failed = true;
                    sendSimple(g_in.peerIdx, kCancel, g_in.reqId);   // a giver-up must SAY so, or the
                    if (logf) logf("[sync] no response to the replay-history request -- peer offline"
                                   " or running an older version");
                } else { sendSimple(g_in.peerIdx, kReq, g_in.reqId); g_in.lastActUs = nowUs; }
            }
        } else if (g_in.got < g_in.chunkCount) {
            // Progress report for the sender's flow-control window, on its own cadence -- tiny,
            // reliable, and what the sweep's throughput is made of.
            if (nowUs - g_in.lastAckUs > (uint64_t)(g_tun.ackIntervalMs * 1000.f)) {
                uint8_t p[13];
                wU32(p, kMagic); p[4] = kAck; wU32(p + 5, g_in.reqId); wU32(p + 9, g_in.got);
                if (g_send) g_send(g_in.peerIdx, p, 13, true);
                g_in.lastAckUs = nowUs;
            }
            if (nowUs - g_in.startUs > (uint64_t)(g_tun.stallTimeoutMs * 1000.f) &&
                nowUs - g_in.lastActUs > (uint64_t)(g_tun.stallTimeoutMs * 1000.f)) {
                g_in.failed = true;
                // ...sender keeps feeding a dead transfer into the connection's reliable queue,
                // which is the drowning this protocol's window exists to prevent.
                sendSimple(g_in.peerIdx, kCancel, g_in.reqId);
                if (logf) logf("[sync] replay-history transfer stalled -- giving up");
            } else if (nowUs - g_in.lastNakUs > (uint64_t)(g_tun.nakIntervalMs * 1000.f) &&
                       nowUs - g_in.lastActUs > (uint64_t)(g_tun.nakIntervalMs * 1000.f)) {
                // collect up to 96 missing chunk indices and re-ask
                uint8_t p[11 + 96 * 2];
                int n = 0;
                for (uint32_t i = 0; i < g_in.chunkCount && n < 96; i++)
                    if (!g_in.have[i]) { wU16(p + 11 + n * 2, (uint16_t)i); n++; }
                if (n > 0) {
                    wU32(p, kMagic); p[4] = kNak; wU32(p + 5, g_in.reqId); wU16(p + 9, (uint16_t)n);
                    if (g_send) g_send(g_in.peerIdx, p, 11 + n * 2, true);
                }
                g_in.lastNakUs = nowUs;
            }
        }
    }
}

// ==== PLAYBACK SAMPLING ==============================================================================
bool SampleAt(int peerIdx, uint64_t targetUs, repl::State& out) {
    ReadyBuf* r = readyFor(peerIdx);
    if (!r || r->count < 2) return false;
    if (targetUs <= r->oldestUs) targetUs = r->oldestUs;
    if (targetUs >= r->newestUs) targetUs = r->newestUs;
    // binary search: greatest entry with us <= target
    uint32_t lo = 0, hi = r->count - 1;
    while (lo + 1 < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (r->idx[mid].us <= targetUs) lo = mid; else hi = mid;
    }
    const ReadyBuf::Entry& ea = r->idx[lo];
    const ReadyBuf::Entry& eb = r->idx[lo + 1 < r->count ? lo + 1 : lo];
    repl::State a, b; uint64_t su = 0;
    if (!repl::Unpack(r->blob + ea.off, ea.len, a, &su)) return false;
    if (&eb == &ea || !repl::Unpack(r->blob + eb.off, eb.len, b, &su)) { out = a; return true; }
    float t = 0.f;
    const double span = (double)(int64_t)(eb.us - ea.us);
    if (span > 0) t = (float)((double)(int64_t)(targetUs - ea.us) / span);
    if (t < 0) t = 0; else if (t > 1) t = 1;
    repl::InterpStates(a, b, t, out);
    return true;
}

uint64_t BufferNewestUs(int peerIdx) { ReadyBuf* r = readyFor(peerIdx); return r ? r->newestUs : 0; }
uint64_t BufferOldestUs(int peerIdx) { ReadyBuf* r = readyFor(peerIdx); return r ? r->oldestUs : 0; }

}} // namespace omp::replaysync
