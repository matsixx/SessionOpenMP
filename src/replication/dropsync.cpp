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
// SessionOpenMP -- the dropped-object lane (see the header for the design).
#include "dropsync.h"
#include <cstring>
#include <cmath>

namespace omp { namespace dropsync {

// Same "OMP?" namespace as the snapshot, cosmetics (OMPG), chat (OMPH) and replay sync (OMPJ) -- see
// the lineage note at the top of replication.cpp, where OMPL is claimed. A letter is claimed once and
// never reused, so a stale build REJECTS these packets instead of misparsing them as a snapshot.
// OMPL -> OMPM -> OMPN. OMPM carried a per-record flag distinguishing the level's own props from
// inventory ones; that feature was withdrawn and the flag with it, so the record layout changed again
// and needs its own letter. OMPL and OMPM are RETIRED and must never be reused -- a stale build has
// to reject these packets, not misparse a record whose length it disagrees about.
static const uint32_t kMagic = 0x4F504D4Fu;   // "OMPO" -- OMPL/OMPM/OMPN retired, never reused
// kWorldSet/kWorldMove are LATER ADDITIONS and need no magic bump: an older build's `default:`
// ignores an unknown subtype, so a mixed pair simply does not share the level's props -- the
// extension path replaysync's kAck established.
enum : uint8_t { kSet = 1, kPlace = 2, kMove = 3, kRemove = 4, kWorldSet = 5, kWorldMove = 6 };

// Under the transport's ~1 KB message convention with room to spare for the name table.
static const int kMaxPacket = 1000;
static const int kPeers     = 16;             // the lobby cap

static SendFn g_send = nullptr;
void SetSendFn(SendFn fn) { g_send = fn; }
static Stats g_st;
const Stats& St() { return g_st; }

bool IsPacket(const uint8_t* d, int len) {
    if (!d || len < 6) return false;
    uint32_t m; memcpy(&m, d, 4);
    return m == kMagic;
}

// ---- bounds-checked cursors. Every read and write goes through these, so a truncated or hostile
// packet can only ever produce `ok = false` -- never an overread. (The snapshot lane's Wr/Rd, local
// to this TU so the two codecs stay independently changeable.)
struct Wr {
    uint8_t* p; int cap, n; bool ok;
    void b(const void* d, int l) { if (!ok || n + l > cap) { ok = false; return; } memcpy(p + n, d, (size_t)l); n += l; }
    void u8(uint8_t v)   { b(&v, 1); }
    void u16(uint16_t v) { b(&v, 2); }
    void u32(uint32_t v) { b(&v, 4); }
    void f32(float v)    { b(&v, 4); }
};
struct Rd {
    const uint8_t* p; int len, n; bool ok;
    bool b(void* d, int l) { if (!ok || n + l > len) { ok = false; return false; } memcpy(d, p + n, (size_t)l); n += l; return true; }
    uint8_t  u8()  { uint8_t v = 0;  b(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; b(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; b(&v, 4); return v; }
    float    f32() { float v = 0;    b(&v, 4); return v; }
};

// ---- validation. Applied to everything that arrives, before it can reach a spawn.
static bool finite3(const float* v, float lim) {
    for (int i = 0; i < 3; i++) if (!(v[i] > -lim && v[i] < lim)) return false;   // rejects NaN/Inf too
    return true;
}
static bool unitQuat(const float* q) {
    float n = 0;
    for (int i = 0; i < 4; i++) { if (!(q[i] > -2.f && q[i] < 2.f)) return false; n += q[i]*q[i]; }
    return n > 0.5f && n < 1.5f;
}
// PRINTABLE ASCII, length-capped. Deliberately NOT the identifier charset [A-Za-z0-9_] it started
// as: a UE asset name is whatever the artist typed, so a legitimate object called "Bench 01" or
// "Rail-Flat" would have been refused -- and because a bad name rejects the WHOLE kSet packet, one
// such object would silently take the entire set with it, authority key and all. The real
// protections are elsewhere and unaffected: the name is length-capped here, distinct names are
// BUDGETED before they can be interned as FNames (the audio funnel's lesson), and the object still
// has to resolve in the game's own catalogue before anything is spawned.
static bool validName(const char* s, int n) {
    if (n <= 0 || n > kMaxNameLen) return false;
    for (int i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) return false;      // control characters and non-ASCII only
    }
    return true;
}
bool NameIsSendable(const char* s) { return s && validName(s, (int)strlen(s)); }
// Objects live in a level; UE's own WORLD_MAX is 21 km. Anything beyond that is not a place a peer
// can have put a rail, and clamping instead of rejecting would spawn it somewhere arbitrary.
static const float kWorldLimit = 2.2e6f;

// ==== SENDING ========================================================================================
static void writeRec(Wr& w, const Rec& r, int nameIdx) {
    w.u16((uint16_t)nameIdx);
    w.u16(r.localId);
    for (int i = 0; i < 3; i++) w.f32(r.loc[i]);
    for (int i = 0; i < 4; i++) w.f32(r.quat[i]);
}

// One kSet part: a private name table (so the part parses alone) followed by its records.
// Returns how many SOURCE records were consumed (the caller advances by that); `*wroteOut` is how
// many actually went in, which is smaller when a record was unsendable. A record the receiver would
// refuse is dropped HERE rather than shipped, because a rejected packet takes the WHOLE part with it
// -- one missing object beats a missing set, and the authority key rides in that packet too.
static int buildSetPart(uint8_t* out, int cap, uint8_t gen, uint64_t authKey,
                        uint8_t partIdx, uint8_t partCount,
                        const Rec* recs, int first, int n, int* lenOut, int* wroteOut) {
    // Names used by this part, in first-use order. Bounded by kRecsPerPacket by construction.
    const char* names[kRecsPerPacket]; int nameN = 0;
    int    srcOf[kRecsPerPacket];                       // written record -> source index
    int    idxOf[kRecsPerPacket];
    int    take = 0, wrote = 0;
    int bytes = 4 + 1 + 1 + 8 + 1 + 1 + 1 + 2;          // magic, sub, gen, authKey, part, count, nameN, recN
    for (int i = first; i < n && wrote < kRecsPerPacket; i++) {
        if (!NameIsSendable(recs[i].id)) { take++; g_st.unsendable++; continue; }   // consumed, not written
        int ni = -1;
        for (int k = 0; k < nameN; k++) if (strcmp(names[k], recs[i].id) == 0) { ni = k; break; }
        const int add = 32 + (ni < 0 ? (1 + (int)strlen(recs[i].id)) : 0);
        if (bytes + add > cap) break;
        if (ni < 0) { names[nameN] = recs[i].id; ni = nameN++; }
        srcOf[wrote] = i; idxOf[wrote] = ni;
        bytes += add;
        wrote++; take++;
    }
    // take == 0 is legitimate for an EMPTY set -- which must still be sent, or a peer who calls back
    // their last object would leave it standing in everyone else's world forever. It is an ERROR only
    // when there was a record to place and it would not fit even alone.
    if (take == 0 && first < n) { *lenOut = 0; if (wroteOut) *wroteOut = 0; return 0; }

    Wr w{ out, cap, 0, true };
    w.u32(kMagic); w.u8(kSet); w.u8(gen);
    w.b(&authKey, 8);
    w.u8(partIdx); w.u8(partCount);
    w.u8((uint8_t)nameN);
    for (int k = 0; k < nameN; k++) {
        const int l = (int)strlen(names[k]);
        w.u8((uint8_t)l);
        w.b(names[k], l);
    }
    w.u16((uint16_t)wrote);
    for (int i = 0; i < wrote; i++) writeRec(w, recs[srcOf[i]], idxOf[i]);
    if (!w.ok) { *lenOut = 0; if (wroteOut) *wroteOut = 0; return 0; }
    *lenOut = w.n;
    if (wroteOut) *wroteOut = wrote;
    return take;
}

int SendSet(int peerIdx, uint8_t gen, uint64_t authKey, const Rec* recs, int n, int budget) {
    if (!g_send || n < 0 || n > kMaxSetRecords) return 0;
    // MEASURED FIRST, SENT SECOND -- the cosmetics packer's discipline (decide what fits BEFORE
    // writing a count), extended to a multi-packet message: a set that arrives half-complete would be
    // applied as "everything else was deleted". So a dry run decides the part boundaries and the part
    // COUNT, and only then is anything sent; if it does not all fit, nothing goes and the caller
    // retries on the next beat.
    // The WORST case, not the typical one: a record costs 32 bytes plus, if its class name is new to
    // this part, up to 64 more -- so a part full of distinct 63-character names carries only ~10
    // records, not kRecsPerPacket. Sizing this by the typical case is how a big set silently refuses
    // to send at all.
    const int kMaxParts = kMaxSetRecords / 8 + 2;
    int start[kMaxParts]; int parts = 0;
    {
        uint8_t scratch[kMaxPacket];
        int at = 0;
        do {
            if (parts >= kMaxParts) return 0;
            int len = 0;
            const int took = buildSetPart(scratch, kMaxPacket, gen, authKey, 0, 0, recs, at, n, &len, nullptr);
            if (len == 0) return 0;                      // a record that cannot fit at all
            start[parts++] = at;
            at += took;
            if (took == 0) break;                        // the empty set: one part, zero records
        } while (at < n);
    }
    if (budget > 0 && parts > budget) return 0;
    for (int i = 0; i < parts; i++) {
        uint8_t pkt[kMaxPacket]; int len = 0;
        int wrote = 0;
        buildSetPart(pkt, kMaxPacket, gen, authKey, (uint8_t)i, (uint8_t)parts, recs, start[i], n, &len, &wrote);
        if (len == 0) return i;                          // cannot happen after the dry run; never send junk
        g_send(peerIdx, pkt, len, true);
        g_st.sent++; g_st.setsSent++; g_st.setRecordsSent += wrote;
    }
    return parts;
}

// One world-set part. Records are self-contained (name inline), so unlike kSet there is no name
// table -- but the ALL-OR-NOTHING rule is the same, for a sharper reason: a joiner reverts its own
// moved props to their map defaults for every prop NOT in the layout, so a half-received layout
// would actively move furniture to the wrong place rather than merely missing some.
static int buildWorldPart(uint8_t* out, int cap, uint8_t gen, const char* map,
                          uint8_t partIdx, uint8_t partCount,
                          const WorldRec* recs, int first, int n, int* lenOut) {
    Wr w{ out, cap, 0, true };
    w.u32(kMagic); w.u8(kWorldSet); w.u8(gen);
    w.u8(partIdx); w.u8(partCount);
    // The map rides EVERY part (parts must parse alone); assembly takes part 0's copy.
    const int ml = map ? (int)strlen(map) : 0;
    w.u8((uint8_t)(ml > 39 ? 39 : ml));
    if (ml) w.b(map, ml > 39 ? 39 : ml);
    const int countAt = w.n;
    w.u8(0);
    int take = 0, wrote = 0;
    for (int i = first; i < n && wrote < kWorldRecsPerPacket; i++) {
        const int l = (int)strlen(recs[i].name);
        if (!validName(recs[i].name, l)) { take++; g_st.unsendable++; continue; }   // consumed, not written
        if (w.n + 1 + l + 28 > cap) break;
        w.u8((uint8_t)l); w.b(recs[i].name, l);
        for (int k = 0; k < 3; k++) w.f32(recs[i].loc[k]);
        for (int k = 0; k < 4; k++) w.f32(recs[i].quat[k]);
        take++; wrote++;
    }
    if (!w.ok || (take == 0 && first < n)) { *lenOut = 0; return 0; }
    out[countAt] = (uint8_t)wrote;
    *lenOut = w.n;
    return take;
}

int SendWorldSet(int peerIdx, uint8_t gen, const char* map, const WorldRec* recs, int n, int budget) {
    if (!g_send || n < 0 || n > kMaxWorldRecords) return 0;
    if (map && !validName(map, (int)strlen(map))) map = "";
    // Dry run first, exactly like SendSet: the part COUNT has to be known before part 0 goes.
    const int kMaxParts = kMaxWorldRecords / 4 + 2;
    int start[kMaxParts]; int parts = 0;
    {
        uint8_t scratch[kMaxPacket];
        int at = 0;
        do {
            if (parts >= kMaxParts) return 0;
            int len = 0;
            const int took = buildWorldPart(scratch, kMaxPacket, gen, map, 0, 0, recs, at, n, &len);
            if (len == 0) return 0;
            start[parts++] = at;
            at += took;
            if (took == 0) break;                        // the empty layout: one part, zero records
        } while (at < n);
    }
    if (budget > 0 && parts > budget) return 0;
    for (int i = 0; i < parts; i++) {
        uint8_t pkt[kMaxPacket]; int len = 0;
        buildWorldPart(pkt, kMaxPacket, gen, map, (uint8_t)i, (uint8_t)parts, recs, start[i], n, &len);
        if (len == 0) return i;
        g_send(peerIdx, pkt, len, true);
        g_st.sent++;
    }
    return parts;
}

int SendWorldMove(int peerIdx, uint8_t gen, const char* name, const float loc[3], const float quat[4],
                  bool claim) {
    if (!g_send || !name) return 0;
    const int l = (int)strlen(name);
    if (!validName(name, l)) return 0;
    uint8_t pkt[kMaxPacket];
    Wr w{ pkt, kMaxPacket, 0, true };
    w.u32(kMagic); w.u8(kWorldMove); w.u8(gen);
    w.u8((uint8_t)l); w.b(name, l);
    for (int i = 0; i < 3; i++) w.f32(loc[i]);
    for (int i = 0; i < 4; i++) w.f32(quat[i]);
    w.u8(claim ? 1 : 0);
    if (!w.ok) return 0;
    // UNRELIABLE while held (a superseded drag pose is worthless), RELIABLE on release: that last one
    // is where the prop actually ends up, and losing it would leave everyone else a step behind.
    g_send(peerIdx, pkt, w.n, !claim);
    g_st.sent++;
    return 1;
}

int SendPlace(int peerIdx, uint8_t gen, const Rec& r) {
    if (!g_send) return 0;
    const int l = (int)strlen(r.id);
    if (!validName(r.id, l)) return 0;
    uint8_t pkt[kMaxPacket];
    Wr w{ pkt, kMaxPacket, 0, true };
    w.u32(kMagic); w.u8(kPlace); w.u8(gen);
    w.u8((uint8_t)l); w.b(r.id, l);
    writeRec(w, r, 0);
    if (!w.ok) return 0;
    g_send(peerIdx, pkt, w.n, true);
    g_st.sent++;
    return 1;
}

int SendMove(int peerIdx, uint8_t gen, const Rec* recs, int n) {
    if (!g_send || n <= 0) return 0;
    int sent = 0;
    for (int at = 0; at < n; ) {
        int take = n - at; if (take > kMoveBatch) take = kMoveBatch;
        uint8_t pkt[kMaxPacket];
        Wr w{ pkt, kMaxPacket, 0, true };
        w.u32(kMagic); w.u8(kMove); w.u8(gen); w.u8((uint8_t)take);
        for (int i = 0; i < take; i++) writeRec(w, recs[at + i], 0);
        if (!w.ok) break;
        // UNRELIABLE on purpose: a dropped pose from a drag is corrected 40 ms later by the next one,
        // and a reliable queue full of superseded poses is exactly what drowned the live lane once.
        g_send(peerIdx, pkt, w.n, false);
        g_st.sent++; sent++;
        at += take;
    }
    return sent;
}

int SendRemove(int peerIdx, uint8_t gen, const uint16_t* ids, int n) {
    if (!g_send || n <= 0) return 0;
    int sent = 0;
    for (int at = 0; at < n; ) {
        int take = n - at; if (take > 200) take = 200;
        uint8_t pkt[kMaxPacket];
        Wr w{ pkt, kMaxPacket, 0, true };
        w.u32(kMagic); w.u8(kRemove); w.u8(gen); w.u8((uint8_t)take);
        for (int i = 0; i < take; i++) w.u16(ids[at + i]);
        if (!w.ok) break;
        g_send(peerIdx, pkt, w.n, true);       // a lost removal leaves a ghost rail: reliable
        g_st.sent++; sent++;
        at += take;
    }
    return sent;
}

// ==== RECEIVING ======================================================================================
// Per-peer assembly. Parts arrive on the RELIABLE, ordered channel, so they are expected strictly in
// order and a gap means something is wrong rather than merely late -- the assembly is abandoned and
// waits for the next part 0. Removals are never applied from an incomplete set.
struct PeerIn {
    bool     have = false;         // a complete set has been assembled at least once
    uint8_t  gen = 0;
    bool     genSeen = false;
    Rec      set[kMaxSetRecords]; int setN = 0;      // the last COMPLETE set
    Rec      asm_[kMaxSetRecords]; int asmN = 0;     // assembly in progress
    uint8_t  asmGen = 0; int nextPart = -1, partCount = 0;
    Rec      scratch[kRecsPerPacket];                // decoded place/move records for this packet
    uint16_t ids[256];
    char     worldBuf[kMaxNameLen + 1];              // the kWorldMove name for this packet
    // The world layout, assembled from parts exactly like the set above.
    bool     whave = false;
    WorldRec wset[kMaxWorldRecords];  int wsetN = 0;
    char     wsetMap[40] = {0};
    WorldRec wasm[kMaxWorldRecords];  int wasmN = 0;
    char     wasmMap[40] = {0};
    uint8_t  wAsmGen = 0; int wNextPart = -1, wPartCount = 0;
};
static PeerIn g_in[kPeers];

void ForgetPeer(int peerIdx) {
    if (peerIdx < 0 || peerIdx >= kPeers) return;
    g_in[peerIdx] = PeerIn();
}
void ResetAll() { for (int i = 0; i < kPeers; i++) g_in[i] = PeerIn(); }

const Rec* SetRecords(int peerIdx, int* nOut) {
    if (peerIdx < 0 || peerIdx >= kPeers || !g_in[peerIdx].have) { if (nOut) *nOut = 0; return nullptr; }
    if (nOut) *nOut = g_in[peerIdx].setN;
    return g_in[peerIdx].set;
}

const WorldRec* WorldSetRecords(int peerIdx, int* nOut, const char** mapOut) {
    if (peerIdx < 0 || peerIdx >= kPeers || !g_in[peerIdx].whave) { if (nOut) *nOut = 0; return nullptr; }
    if (nOut) *nOut = g_in[peerIdx].wsetN;
    if (mapOut) *mapOut = g_in[peerIdx].wsetMap;
    return g_in[peerIdx].wset;
}

static bool readRec(Rd& r, Rec& out, const char* const* names, int nameN) {
    const int ni = (int)r.u16();
    out.localId = r.u16();
    for (int i = 0; i < 3; i++) out.loc[i]  = r.f32();
    for (int i = 0; i < 4; i++) out.quat[i] = r.f32();
    if (!r.ok) return false;
    if (!finite3(out.loc, kWorldLimit) || !unitQuat(out.quat)) return false;
    if (names) {
        if (ni < 0 || ni >= nameN) return false;
        strncpy_s(out.id, sizeof(out.id), names[ni], _TRUNCATE);
    } else {
        out.id[0] = 0;
    }
    return true;
}

bool OnPacket(int peerIdx, const uint8_t* d, int len, Update& out) {
    out = Update();
    if (!IsPacket(d, len) || peerIdx < 0 || peerIdx >= kPeers) return false;
    PeerIn& in = g_in[peerIdx];
    Rd r{ d, len, 0, true };
    r.u32();
    const uint8_t sub = r.u8();
    const uint8_t gen = r.u8();
    if (!r.ok) { g_st.rejected++; return false; }
    out.gen = gen;

    // A generation change means this peer's ids stopped meaning what they meant. Reported to the
    // caller so it can drop what it holds BEFORE anything in this packet is applied.
    if (!in.genSeen || in.gen != gen) {
        in.genSeen = true; in.gen = gen;
        in.have = false; in.setN = 0;
        in.nextPart = -1; in.asmN = 0;
        in.whave = false; in.wsetN = 0;
        in.wNextPart = -1; in.wasmN = 0;
        out.genReset = true;
    }

    switch (sub) {
    case kSet: {
        uint64_t authKey = 0;
        if (!r.b(&authKey, 8)) { g_st.rejected++; return false; }
        out.haveAuth = true; out.authKey = authKey;
        const uint8_t partIdx   = r.u8();
        const uint8_t partCount = r.u8();
        const uint8_t nameN     = r.u8();
        if (!r.ok || partCount == 0 || partIdx >= partCount || nameN > kRecsPerPacket) { g_st.rejected++; return false; }
        char  nameBuf[kRecsPerPacket][kMaxNameLen + 1];
        const char* names[kRecsPerPacket];
        for (int k = 0; k < nameN; k++) {
            const int l = (int)r.u8();
            if (!r.ok || l <= 0 || l > kMaxNameLen) { g_st.rejected++; return false; }
            if (!r.b(nameBuf[k], l)) { g_st.rejected++; return false; }
            nameBuf[k][l] = 0;
            if (!validName(nameBuf[k], l)) { g_st.rejected++; return false; }
            names[k] = nameBuf[k];
        }
        const int recN = (int)r.u16();
        if (!r.ok || recN < 0 || recN > kRecsPerPacket) { g_st.rejected++; return false; }
        // Parsed into a local first: a part that fails halfway must not leave the assembly holding
        // half of it, or the completed set would be a mix of two attempts.
        Rec tmp[kRecsPerPacket];
        for (int i = 0; i < recN; i++) if (!readRec(r, tmp[i], names, nameN)) { g_st.rejected++; return false; }
        if (r.n != len) { g_st.rejected++; return false; }        // trailing bytes: not our packet shape

        if (partIdx == 0) { in.asmN = 0; in.nextPart = 0; in.partCount = partCount; in.asmGen = gen; }
        if (in.nextPart != partIdx || in.partCount != partCount || in.asmGen != gen) {
            in.nextPart = -1; in.asmN = 0;                        // out of order: wait for a fresh part 0
            g_st.partsDropped++;
            g_st.recv++;
            return true;                                          // genReset may still be worth acting on
        }
        if (in.asmN + recN > kMaxSetRecords) { in.nextPart = -1; in.asmN = 0; g_st.rejected++; return false; }
        for (int i = 0; i < recN; i++) in.asm_[in.asmN++] = tmp[i];
        in.nextPart++;
        if (in.nextPart >= partCount) {
            memcpy(in.set, in.asm_, sizeof(Rec) * (size_t)in.asmN);
            in.setN = in.asmN;
            in.have = true;
            in.nextPart = -1; in.asmN = 0;
            out.setReady = true;
            g_st.setsRecv++; g_st.setRecordsRecv += in.setN;
        }
        g_st.recv++;
        return true;
    }
    case kPlace: {
        const int l = (int)r.u8();
        if (!r.ok || l <= 0 || l > kMaxNameLen) { g_st.rejected++; return false; }
        char nm[kMaxNameLen + 1];
        if (!r.b(nm, l)) { g_st.rejected++; return false; }
        nm[l] = 0;
        if (!validName(nm, l)) { g_st.rejected++; return false; }
        const char* names[1] = { nm };
        if (!readRec(r, in.scratch[0], names, 1)) { g_st.rejected++; return false; }
        if (r.n != len) { g_st.rejected++; return false; }
        out.nPlace = 1; out.place = in.scratch;
        g_st.recv++;
        return true;
    }
    case kMove: {
        const int n = (int)r.u8();
        if (!r.ok || n <= 0 || n > kMoveBatch) { g_st.rejected++; return false; }
        for (int i = 0; i < n; i++) if (!readRec(r, in.scratch[i], nullptr, 0)) { g_st.rejected++; return false; }
        if (r.n != len) { g_st.rejected++; return false; }
        out.nMove = n; out.move = in.scratch;
        g_st.recv++;
        return true;
    }
    case kWorldSet: {
        const uint8_t partIdx   = r.u8();
        const uint8_t partCount = r.u8();
        char mapBuf[40] = {0};
        {
            const int ml = (int)r.u8();
            if (!r.ok || ml > 39) { g_st.rejected++; return false; }
            if (ml && !r.b(mapBuf, ml)) { g_st.rejected++; return false; }
            mapBuf[ml] = 0;
            if (ml && !validName(mapBuf, ml)) { g_st.rejected++; return false; }
        }
        const int recN = (int)r.u8();
        if (!r.ok || partCount == 0 || partIdx >= partCount || recN > kWorldRecsPerPacket) {
            g_st.rejected++; return false;
        }
        // Into a local first, like kSet: a part that fails halfway must not pollute the assembly.
        WorldRec tmp[kWorldRecsPerPacket];
        for (int i = 0; i < recN; i++) {
            const int l = (int)r.u8();
            if (!r.ok || l <= 0 || l > kMaxNameLen) { g_st.rejected++; return false; }
            if (!r.b(tmp[i].name, l)) { g_st.rejected++; return false; }
            tmp[i].name[l] = 0;
            if (!validName(tmp[i].name, l)) { g_st.rejected++; return false; }
            for (int k = 0; k < 3; k++) tmp[i].loc[k]  = r.f32();
            for (int k = 0; k < 4; k++) tmp[i].quat[k] = r.f32();
            if (!r.ok || !finite3(tmp[i].loc, kWorldLimit) || !unitQuat(tmp[i].quat)) {
                g_st.rejected++; return false;
            }
        }
        if (r.n != len) { g_st.rejected++; return false; }

        if (partIdx == 0) {
            in.wasmN = 0; in.wNextPart = 0; in.wPartCount = partCount; in.wAsmGen = gen;
            strncpy_s(in.wasmMap, sizeof(in.wasmMap), mapBuf, _TRUNCATE);
        }
        if (in.wNextPart != partIdx || in.wPartCount != partCount || in.wAsmGen != gen) {
            in.wNextPart = -1; in.wasmN = 0;
            g_st.partsDropped++;
            g_st.recv++;
            return true;
        }
        if (in.wasmN + recN > kMaxWorldRecords) { in.wNextPart = -1; in.wasmN = 0; g_st.rejected++; return false; }
        for (int i = 0; i < recN; i++) in.wasm[in.wasmN++] = tmp[i];
        in.wNextPart++;
        if (in.wNextPart >= partCount) {
            memcpy(in.wset, in.wasm, sizeof(WorldRec) * (size_t)in.wasmN);
            in.wsetN = in.wasmN;
            strncpy_s(in.wsetMap, sizeof(in.wsetMap), in.wasmMap, _TRUNCATE);
            in.whave = true;
            in.wNextPart = -1; in.wasmN = 0;
            out.worldSetReady = true;
        }
        g_st.recv++;
        return true;
    }
    case kWorldMove: {
        const int l = (int)r.u8();
        if (!r.ok || l <= 0 || l > kMaxNameLen) { g_st.rejected++; return false; }
        if (!r.b(in.worldBuf, l)) { g_st.rejected++; return false; }
        in.worldBuf[l] = 0;
        if (!validName(in.worldBuf, l)) { g_st.rejected++; return false; }
        float loc[3], quat[4];
        for (int i = 0; i < 3; i++) loc[i]  = r.f32();
        for (int i = 0; i < 4; i++) quat[i] = r.f32();
        const bool claim = (r.u8() & 1) != 0;
        if (!r.ok || r.n != len) { g_st.rejected++; return false; }
        if (!finite3(loc, kWorldLimit) || !unitQuat(quat)) { g_st.rejected++; return false; }
        out.haveWorldMove = true; out.worldMoveName = in.worldBuf;
        memcpy(out.worldMoveLoc, loc, sizeof(loc));
        memcpy(out.worldMoveQuat, quat, sizeof(quat));
        out.worldMoveClaim = claim;
        g_st.recv++;
        return true;
    }
    case kRemove: {
        const int n = (int)r.u8();
        if (!r.ok || n <= 0 || n > (int)(sizeof(in.ids) / sizeof(in.ids[0]))) { g_st.rejected++; return false; }
        for (int i = 0; i < n; i++) in.ids[i] = r.u16();
        if (!r.ok || r.n != len) { g_st.rejected++; return false; }
        out.nRemove = n; out.remove = in.ids;
        g_st.recv++;
        return true;
    }
    default:
        // An unknown subtype is a NEWER build's message, not a broken one -- ignored, exactly as
        // replaysync's kAck is by builds that predate it. Never counted as a rejection.
        g_st.recv++;
        return true;
    }
}

}} // namespace omp::dropsync
