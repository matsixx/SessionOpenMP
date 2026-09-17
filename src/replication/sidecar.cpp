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
// The replay sidecar codec -- see sidecar.h for the format.
#include "sidecar.h"
#include <cstdio>
#include <cstring>

namespace omp { namespace sidecar {

namespace {
struct W {
    uint8_t* out; uint32_t cap, n; bool ok;
    void bytes(const void* p, uint32_t len) {
        if (!ok) return;
        if (out) { if (n + len > cap) { ok = false; return; } memcpy(out + n, p, len); }
        n += len;
    }
    void u8(uint8_t v)   { bytes(&v, 1); }
    void u16(uint16_t v) { bytes(&v, 2); }
    void u32(uint32_t v) { bytes(&v, 4); }
    void u64(uint64_t v) { bytes(&v, 8); }
};
struct R {
    const uint8_t* d; uint32_t len, n; bool ok;
    bool take(void* p, uint32_t l) {
        if (!ok || n + l > len) { ok = false; return false; }
        if (p) memcpy(p, d + n, l);
        n += l; return true;
    }
    uint8_t  u8()  { uint8_t v = 0;  take(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; take(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; take(&v, 8); return v; }
};
void say(char* why, int cap, const char* m) { if (why && cap > 0) { strncpy_s(why, (size_t)cap, m, _TRUNCATE); } }
}

uint32_t Pack(const Peer* peers, int n, uint8_t* out, uint32_t cap) {
    if (!peers || n <= 0) return 0;
    if (n > kMaxPeers) n = kMaxPeers;
    W w{out, cap, 0, true};
    w.u32(kMagic); w.u16(kVersion); w.u8(repl::kWireMajor); w.u8(repl::kWireMinor);
    w.u32((uint32_t)n);
    for (int i = 0; i < n; i++) {
        const Peer& p = peers[i];
        if (!p.entries || p.entriesLen == 0 || p.entryCount < 2) return 0;
        const uint32_t block = (uint32_t)kNameMax + 4 + (uint32_t)sizeof(p.cosmetics) + 1 + 4 + (uint32_t)sizeof(p.wear)
                             + 1 + 4 + (uint32_t)sizeof(p.skel) + 8 + 4 + 4 + p.entriesLen;
        w.u32(block);
        w.bytes(p.name, kNameMax);
        w.u32((uint32_t)sizeof(p.cosmetics)); w.bytes(&p.cosmetics, (uint32_t)sizeof(p.cosmetics));
        w.u8(p.haveWear ? 1 : 0);
        w.u32((uint32_t)sizeof(p.wear));      w.bytes(&p.wear, (uint32_t)sizeof(p.wear));
        w.u8(p.haveSkel ? 1 : 0);
        w.u32((uint32_t)sizeof(p.skel));      w.bytes(&p.skel, (uint32_t)sizeof(p.skel));
        w.u64(p.anchorEndUs);
        w.u32(p.entryCount);
        w.u32(p.entriesLen);
        w.bytes(p.entries, p.entriesLen);
    }
    return w.ok ? w.n : 0;
}

bool Unpack(const uint8_t* d, uint32_t len, Peer* out, int cap, int* nOut, char* why, int whyCap) {
    if (nOut) *nOut = 0;
    if (!d || len < 12 || !out || cap <= 0) { say(why, whyCap, "empty"); return false; }
    R r{d, len, 0, true};
    if (r.u32() != kMagic) { say(why, whyCap, "not a SessionOpenMP replay sidecar"); return false; }
    const uint16_t ver = r.u16();
    if (ver != kVersion) { say(why, whyCap, "written by a different SessionOpenMP (sidecar version)"); return false; }
    const uint8_t major = r.u8(); (void)r.u8();
    if (major != repl::kWireMajor) { say(why, whyCap, "written by a different SessionOpenMP (snapshot wire version)"); return false; }
    const uint32_t count = r.u32();
    if (!r.ok || count == 0 || count > (uint32_t)kMaxPeers) { say(why, whyCap, "bad peer count"); return false; }
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t block = r.u32();
        if (!r.ok || block > len - r.n) { say(why, whyCap, "truncated"); return false; }
        const uint32_t end = r.n + block;
        Peer tmp;
        r.take(tmp.name, kNameMax); tmp.name[kNameMax - 1] = 0;
        if (r.u32() != (uint32_t)sizeof(tmp.cosmetics)) { say(why, whyCap, "cosmetics layout differs from this build"); return false; }
        r.take(&tmp.cosmetics, (uint32_t)sizeof(tmp.cosmetics));
        tmp.haveWear = r.u8() != 0;
        if (r.u32() != (uint32_t)sizeof(tmp.wear)) { say(why, whyCap, "wear layout differs from this build"); return false; }
        r.take(&tmp.wear, (uint32_t)sizeof(tmp.wear));
        tmp.haveSkel = r.u8() != 0;
        if (r.u32() != (uint32_t)sizeof(tmp.skel)) { say(why, whyCap, "skeleton layout differs from this build"); return false; }
        r.take(&tmp.skel, (uint32_t)sizeof(tmp.skel));
        tmp.anchorEndUs = r.u64();
        tmp.entryCount  = r.u32();
        tmp.entriesLen  = r.u32();
        if (!r.ok || tmp.entriesLen > len - r.n || r.n + tmp.entriesLen != end) { say(why, whyCap, "truncated"); return false; }
        tmp.entries = d + r.n;
        r.n = end;
        // Every bounded field re-checked before it is trusted: the file is the same untrusted input a
        // packet is, just slower to arrive.
        tmp.cosmetics.skaterName[sizeof(tmp.cosmetics.skaterName) - 1] = 0;
        tmp.cosmetics.mapName[sizeof(tmp.cosmetics.mapName) - 1] = 0;
        tmp.cosmetics.visualDef[sizeof(tmp.cosmetics.visualDef) - 1] = 0;
        if (tmp.cosmetics.nChar > 24) tmp.cosmetics.nChar = 24;
        if (tmp.cosmetics.nBoard > 16) tmp.cosmetics.nBoard = 16;
        if (tmp.wear.n > 24) tmp.wear.n = 24;
        if (tmp.skel.n > repl::kPoseMaxBones) tmp.skel.n = repl::kPoseMaxBones;
        if (n < cap) out[n++] = tmp;
    }
    if (nOut) *nOut = n;
    return n > 0;
}

}} // namespace omp::sidecar
