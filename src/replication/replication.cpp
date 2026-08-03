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
// SessionOpenMP replication core -- implementation. Where a constant or a rule looks arbitrary, it was
// measured against live traffic rather than chosen.
#include "replication.h"
#include "anim_fields.h"
#include <cstring>
#include <cmath>

namespace omp { namespace repl {

// ------------------------------------------------------------------ wire format
// QUANTIZED, variable-length, but still a SELF-CONTAINED SNAPSHOT per packet -- the transport is
// unreliable, so nothing may reference a previous packet. The classic cross-packet changed-mask is
// deliberately NOT used; the compression here is zero-suppression + smaller encodings, both loss-safe:
//   * quats travel as SMALLEST-THREE (10 bits x 3 + 2-bit index = 4 B, ~0.15 deg worst case)
//   * the DECK position travels as float16 relative to bodyPos (the one float32 anchor) -- valid
//     because deck and body are read from the SAME source (component world positions, kCompPos), so
//     they are provably in one frame. relPos is mesh-frame BY CONSTRUCTION (gather computes the delta
//     itself), so its f16 stays. Foot and hand positions are ABSOLUTE f32 -- see the space rule below
//   * anim-blob floats travel as float16 with a per-field float32 ESCAPE bit, decided by a round-trip
//     error test at pack time -- no per-field range annotations, so an unusual field (seconds counter,
//     big magnitude) keeps full precision instead of clamping wrong (92 fields of unknowable
//     semantics -- correctness by measurement, not by table)
//   * zero-valued anim fields don't travel at all (presence mask); zero IS the decoded default
//   * the trick name is length-prefixed and only present when set
//   * HAND IK targets are on the wire because the blob carries the hand IK ALPHAS, and an alpha
//     without its target blends the arm toward whatever the observer invented. Trucks/wheels are
//     absent (never filled); adding them means extending the format AND bumping the magic
// The euler debug triple (bodyYaw/Pitch/Roll) is not on the wire: nothing applies it.
//
// SPACE RULE: a field whose coordinate SPACE is not proven must travel value-preserving (absolute
// f32), never relative to bodyPos. Foot sockets do not hold world coordinates -- encoded rel-to-body
// they come out ~2500 cm, hit the +/-1000 clamp, and decode to IK targets ~10 m away at hip height,
// which renders as both legs fully extended. A codec gate for such a field must feed it data that
// VIOLATES the codec's assumptions: a gate whose test feet are invented to sit near the body shares
// the assumption it exists to check.
//
// WIRE MAGIC LINEAGE. Three message types share one "OMP?" namespace -- snapshot, cosmetics
// (kCosMagic) and chat (kChatMagic) -- so a letter is claimed once and NEVER reused, retired values
// included. A stale build must REJECT a packet, not misparse it; that is the whole job of the magic.
//   OMP9 -> feet relative to body
//   OMPA -> feet absolute f32
//   OMPB -> grind def + board mode
//   OMPC -> audio records
//   OMPD -> cosmetics: inventory-instance attributes
//   OMPE -> `replaying` in f2 bit 4 (costs no bytes, but a peer that cannot read it never sends a
//           pose, and the receiver sits looking at a frozen skater with no error)
//   OMPF -> hand IK targets
//   OMPG -> cosmetics: sender map name
//   OMPH -> chat
//   OMPI -> PushSpeedMultiplier (a peer on an older build animates every push at 1.0x, silently)
static const uint32_t kMagic = 0x49504D4Fu; // "OMPI"

static bool finite3(const float* v, float lim) {
    for (int i = 0; i < 3; i++) if (!(v[i] > -lim && v[i] < lim)) return false;   // rejects NaN/Inf too
    return true;
}
static bool unitQuat(const float* q) {
    float n = 0; for (int i = 0; i < 4; i++) { if (!(q[i] > -2.f && q[i] < 2.f)) return false; n += q[i]*q[i]; }
    return n > 0.5f && n < 1.5f;
}

// ---- float32 <-> float16 (IEEE half), scalar bit math -- no intrinsics, so every build agrees.
static uint16_t f32to16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t  e    = (int32_t)((x >> 23) & 0xffu) - 127 + 15;
    uint32_t       man  = x & 0x7fffffu;
    if (e >= 31) return (uint16_t)(sign | 0x7c00u | (((x & 0x7f800000u) == 0x7f800000u && man) ? 1 : 0));
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;                      // underflow -> signed zero
        man |= 0x800000u;
        const int shift = 14 - e;
        uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1u) h++;                      // round to nearest
        return (uint16_t)(sign | h);
    }
    uint16_t h = (uint16_t)(sign | ((uint32_t)e << 10) | (man >> 13));
    if (man & 0x1000u) h++;                                      // round; carry into exponent is correct
    return h;
}
static float f16to32(uint16_t h) {
    const uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1fu, man = h & 0x3ffu, x;
    if (e == 0) {
        if (!man) x = sign;
        else {                                                   // subnormal half -> normal float
            e = 127 - 15 + 1;
            while (!(man & 0x400u)) { man <<= 1; e--; }
            x = sign | (e << 23) | ((man & 0x3ffu) << 13);
        }
    } else if (e == 31) x = sign | 0x7f800000u | (man << 13);
    else x = sign | ((e - 15 + 127) << 23) | (man << 13);
    float f; memcpy(&f, &x, 4); return f;
}

// ---- smallest-three quaternion: drop the largest-magnitude component (sign-normalized away, q == -q),
// store the other three at 10 bits over [-1/sqrt2, 1/sqrt2]. Reconstruction is unit BY CONSTRUCTION,
// which is why Unpack needs no unitQuat check on these.
static uint32_t qPack(const float* q) {
    int big = 0; float mx = fabsf(q[0]);
    for (int i = 1; i < 4; i++) { const float a = fabsf(q[i]); if (a > mx) { mx = a; big = i; } }
    const float sgn = (q[big] < 0.f) ? -1.f : 1.f;
    uint32_t v = (uint32_t)big << 30;
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        float c = q[i] * sgn * 1.41421356f;                      // -> [-1, 1]
        if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
        uint32_t u = (uint32_t)((c * 0.5f + 0.5f) * 1023.f + 0.5f);
        if (u > 1023u) u = 1023u;
        v |= u << (slot * 10); slot++;
    }
    return v;
}
static void qUnpack(uint32_t v, float* q) {
    const int big = (int)(v >> 30);
    float sum = 0; int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (i == big) { q[i] = 0; continue; }
        const uint32_t u = (v >> (slot * 10)) & 0x3ffu; slot++;
        q[i] = ((float)u / 1023.f * 2.f - 1.f) * 0.70710678f;
        sum += q[i] * q[i];
    }
    q[big] = (sum < 1.f) ? sqrtf(1.f - sum) : 0.f;
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (n > 1e-6f) { n = 1.f / sqrtf(n); for (int i = 0; i < 4; i++) q[i] *= n; }
    else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
}

// ---- bounds-checked cursors: every read/write goes through these, so a truncated or hostile packet
// can only ever produce `ok = false`, never an overread.
struct Wr {
    uint8_t* p; int cap, n; bool ok;
    void b(const void* d, int l) { if (!ok || n + l > cap) { ok = false; return; } memcpy(p + n, d, l); n += l; }
    void u8(uint8_t v)   { b(&v, 1); }
    void u16(uint16_t v) { b(&v, 2); }
    void u32(uint32_t v) { b(&v, 4); }
    void u64(uint64_t v) { b(&v, 8); }
    void f32(float v)    { b(&v, 4); }
    void h16(float v)    { u16(f32to16(v)); }
};
struct Rd {
    const uint8_t* p; int len, n; bool ok;
    bool b(void* d, int l) { if (!ok || n + l > len) { ok = false; return false; } memcpy(d, p + n, l); n += l; return true; }
    uint8_t  u8()  { uint8_t v = 0;  b(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; b(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; b(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; b(&v, 8); return v; }
    float    f32() { float v = 0;    b(&v, 4); return v; }
    float    h16() { return f16to32(u16()); }
};

static float clampf(float v, float lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

// Every string on this wire ends up in a log line, a menu label or a Slate/ImGui text block, and all
// of them come from a peer. Collapse anything that is not printable 7-bit ASCII to '?' at the unpack
// choke point, so no consumer downstream has to remember to.
// It is a NO-OP for legitimate traffic by construction: asset names are FNames, and the local side
// already collapses >=128 to '?' when it reads one (gather/cosmetics/audio all do), while a player's
// display name is restricted to alnum/space/_-. by MpName_Set before it is ever sent. What it stops
// is a crafted name smuggling newlines into the log (forged log lines -- and the logs are what the
// user diagnoses field reports from) or escapes/controls into text layout.
// Deliberately the SAFETY half only: the sender's word-list filter is the manners half and stays
// sender-side, because a filter the receiver enforces is a filter the receiver has to agree with.
static void sanitizeAscii(char* s, int cap) {
    if (!s || cap <= 0) return;
    s[cap - 1] = 0;                                  // every caller terminates already; belt and braces
    for (int i = 0; i < cap && s[i]; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) s[i] = '?';
    }
}

// ---- audio records ----------------------------------------------------------------------------------
// Sizing is a separate pass from writing so Pack can decide how many WHOLE records fit before it
// commits a count byte. Strings use the length-prefixed idiom the trick/grind names already use:
// length from strnlen(buf, sizeof-1), and on the way back a length that does not fit REJECTS THE
// PACKET rather than truncating -- a malformed name is a malformed packet.
static int audioStrSize(const char* s, int cap) { return 1 + (int)strnlen(s, (size_t)cap - 1); }
static void audioWriteStr(Wr& w, const char* s, int cap) {
    const uint8_t l = (uint8_t)strnlen(s, (size_t)cap - 1);
    w.u8(l); w.b(s, l);
}
static bool audioReadStr(Rd& r, char* out, int cap) {
    const uint8_t l = r.u8();
    if (l >= cap) return false;
    if (!r.b(out, l)) return false;
    out[l] = 0;                                              // untrusted input: force-terminate
    sanitizeAscii(out, cap);                                 // ...and printable: cue/param names get logged
    return true;
}
static int audioLoopSize(const AudioLoop& l) {
    int n = 3 + audioStrSize(l.cue, kAudioCueLen) + 3 * 2 + 2 + 2;   // slot, attach, nParam, cue, rel, vol, pitch
    for (int i = 0; i < l.nParam && i < kAudioMaxParams; i++)
        n += audioStrSize(l.param[i].name, kAudioParamLen) + 2;
    return n;
}
static int audioEventSize(const AudioEvent& e) {
    return 2 + 1 + audioStrSize(e.cue, kAudioCueLen) + 3 * 2 + 2 + 2 + 2 + 2;
}
static void audioWriteLoop(Wr& w, const AudioLoop& l) {
    const uint8_t np = (uint8_t)(l.nParam < kAudioMaxParams ? l.nParam : kAudioMaxParams);
    w.u8(l.slot); w.u8(l.attach); w.u8(np);
    audioWriteStr(w, l.cue, kAudioCueLen);
    for (int i = 0; i < 3; i++) w.h16(clampf(l.rel[i], 2000.f));
    w.h16(clampf(l.vol, 100.f)); w.h16(clampf(l.pitch, 100.f));
    for (int i = 0; i < np; i++) { audioWriteStr(w, l.param[i].name, kAudioParamLen); w.u16((uint16_t)l.param[i].value); }
}
static void audioWriteEvent(Wr& w, const AudioEvent& e) {
    w.u16(e.id); w.u8(e.attach);
    audioWriteStr(w, e.cue, kAudioCueLen);
    for (int i = 0; i < 3; i++) w.h16(clampf(e.rel[i], 2000.f));
    w.h16(clampf(e.vol, 100.f)); w.h16(clampf(e.pitch, 100.f)); w.h16(clampf(e.start, 1000.f));
    w.u16(e.ageMs);
}

int Pack(const State& s, uint64_t senderUs, uint8_t* out, int cap) {
    // A packet without a valid board pose never ships; Unpack enforces the same gate on arrival.
    if (!out || !unitQuat(s.deckQuat) || !finite3(s.deckPos, 1e7f) || !finite3(s.bodyPos, 1e7f)) return 0;
    if (s.animLen > sizeof(s.anim)) return 0;
    Wr w{out, cap, 0, true};
    w.u32(kMagic); w.u64(senderUs);
    const uint8_t trick = s.trickName[0] ? 1 : 0;
    const uint8_t grind = s.grindName[0] ? 1 : 0;
    const uint8_t f1 = (uint8_t)((s.bodyPosOk ? 1 : 0) | (s.bodyRotOk ? 2 : 0) | (s.meshOk ? 4 : 0)
                     | (s.relOk ? 8 : 0) | (s.feetOk ? 16 : 0) | (s.feetWorld ? 32 : 0)
                     | (s.onBoard ? 64 : 0) | (s.bailing ? 128 : 0));
    const uint8_t f2 = (uint8_t)((s.grounded ? 1 : 0) | (s.crankOn ? 2 : 0) | (trick ? 4 : 0)
                     | (grind ? 8 : 0) | (s.replaying ? 16 : 0)
                     | (s.handOk ? 32 : 0) | (s.handWorld ? 64 : 0));
    w.u8(f1); w.u8(f2);
    w.u8(s.pushFlags); w.u8(s.pushState); w.u8(s.brakeState); w.u8(s.boardMode);
    // PushSpeedMultiplier. h16 is plenty: it is a small animation-rate multiplier around 1.0, and the
    // clamp is what keeps a garbage read from driving the proxy's push animation to an absurd rate.
    w.h16(clampf(s.pushSpeed, 8.f));
    w.u16(s.crankDefOff); w.h16(s.crankPocket);
    w.h16(clampf(s.grindPitch, 1000.f)); w.h16(clampf(s.grindYaw, 1000.f));
    for (int i = 0; i < 3; i++) w.f32(s.bodyPos[i]);             // the one absolute anchor
    w.u32(qPack(s.deckQuat));
    for (int i = 0; i < 3; i++) w.h16(clampf(s.deckPos[i] - s.bodyPos[i], 2000.f));   // board >20 m from
                                                                 // the rider is garbage anyway (snap land)
    if (s.bodyRotOk) w.u32(qPack(s.bodyQuat));
    if (s.meshOk)    w.u32(qPack(s.meshQuat));
    if (s.relOk)     for (int i = 0; i < 3; i++) w.h16(clampf(s.relPos[i], 1000.f));
    if (s.feetOk) {
        // positions ABSOLUTE f32 (see the space rule at the top); rotators are degrees, which are
        // space-free, so f16 stays.
        for (int i = 0; i < 3; i++) w.f32(s.lFootPos[i]);
        for (int i = 0; i < 3; i++) w.h16(s.lFootRot[i]);
        for (int i = 0; i < 3; i++) w.f32(s.rFootPos[i]);
        for (int i = 0; i < 3; i++) w.h16(s.rFootRot[i]);
    }
    if (s.handOk) {
        // Identical encoding to the feet, for the identical reason: a position whose coordinate SPACE
        // is not proven travels value-preserving/absolute, never relative to bodyPos. Rotators are
        // degrees -- space-free -- so f16 is safe there.
        for (int i = 0; i < 3; i++) w.f32(s.lHandPos[i]);
        for (int i = 0; i < 3; i++) w.h16(s.lHandRot[i]);
        for (int i = 0; i < 3; i++) w.f32(s.rHandPos[i]);
        for (int i = 0; i < 3; i++) w.h16(s.rHandRot[i]);
    }
    if (trick) {
        const uint8_t l = (uint8_t)strnlen(s.trickName, sizeof(s.trickName) - 1);
        w.u8(l); w.b(s.trickName, l);
    }
    if (grind) {
        const uint8_t l = (uint8_t)strnlen(s.grindName, sizeof(s.grindName) - 1);
        w.u8(l); w.b(s.grindName, l);
    }
    // ---- the anim blob: walked BY THE FIELD TABLE (never by struct offsets). Only fields that fit
    // completely inside animLen travel; the count pins both ends to the same walk.
    int nf = 0;
    { int off = 0;
      for (int i = 0; i < AnimFieldCount(); i++) {
          const AnimField& f = AnimFieldAt(i);
          if (off + f.size > (int)s.animLen) break;
          off += f.size; nf++;
      } }
    w.u8((uint8_t)nf);
    if (nf) {
        uint8_t present[16] = {}, esc[16] = {}, payload[512]; int pn = 0;
        const int mb = (nf + 7) / 8;
        if (mb > (int)sizeof(present)) return 0;
        int off = 0;
        for (int i = 0; i < nf; i++) {
            const AnimField& f = AnimFieldAt(i);
            if (f.size == 1) {
                const uint8_t v = s.anim[off];
                if (v) { present[i >> 3] |= (uint8_t)(1 << (i & 7)); payload[pn++] = v; }
            } else {
                // float vector of size/4 components (1 or 3). Escape to f32 if ANY component fails the
                // round-trip tolerance -- unusual magnitudes keep full precision automatically.
                const int nc = f.size / 4;
                float v[3]; bool nz = false, wide = false;
                for (int c = 0; c < nc; c++) {
                    memcpy(&v[c], s.anim + off + c * 4, 4);
                    if (!(v[c] > -3.4e38f && v[c] < 3.4e38f)) v[c] = 0;      // NaN/Inf never travels
                    if (v[c] != 0.f) nz = true;
                }
                if (nz) {
                    for (int c = 0; c < nc && !wide; c++) {
                        const float back = f16to32(f32to16(v[c]));
                        if (fabsf(back - v[c]) > 0.0015f + 0.0015f * fabsf(v[c])) wide = true;
                    }
                    present[i >> 3] |= (uint8_t)(1 << (i & 7));
                    if (wide) esc[i >> 3] |= (uint8_t)(1 << (i & 7));
                    for (int c = 0; c < nc; c++) {
                        if (pn + 4 > (int)sizeof(payload)) return 0;
                        if (wide) { memcpy(payload + pn, &v[c], 4); pn += 4; }
                        else { const uint16_t h = f32to16(v[c]); memcpy(payload + pn, &h, 2); pn += 2; }
                    }
                }
            }
            off += f.size;
        }
        w.b(present, mb); w.b(esc, mb); w.b(payload, pn);
    }
    // ---- THE POSE LANE. Written before audio because a peer's SKELETON matters more than their
    // sounds when both compete for the same packet -- and ALL OR NOTHING: half a skeleton is not a
    // degraded pose, it is a scrambled one.
    {
        const int n = (s.poseN > kPoseMaxBones) ? 0 : s.poseN;
        const int need = 1 + n * (4 + 3 * 2);
        if (n > 0 && w.ok && w.n + need <= cap) {
            w.u8((uint8_t)n);
            for (int b = 0; b < n; b++) {
                w.u32(qPack(s.poseRot[b]));
                for (int c = 0; c < 3; c++) w.h16(clampf(s.posePos[b][c], 1000.f));
            }
        } else {
            w.u8(0);
        }
    }
    // ---- AUDIO, written LAST and only as far as it fits --------------------------------------------
    // The promise: audio can never starve the pose. A sizing pass decides how many whole records fit
    // in what is left, so an overfull frame drops trailing SOUNDS and the skater still moves. Without
    // an explicit budget one section silently eats the other's, and the loser vanishes with no error.
    {
        const int room = w.ok ? (cap - w.n - 2) : 0;      // -2 for the two count bytes
        int used = 0, nl = 0, ne = 0;
        for (int i = 0; i < s.nLoops && i < kAudioMaxLoops; i++) {
            const int sz = audioLoopSize(s.loops[i]);
            if (used + sz > room) break;
            used += sz; nl++;
        }
        for (int i = 0; i < s.nEvents && i < kAudioMaxEvents; i++) {
            const int sz = audioEventSize(s.events[i]);
            if (used + sz > room) break;
            used += sz; ne++;
        }
        w.u8((uint8_t)nl);
        for (int i = 0; i < nl; i++) audioWriteLoop(w, s.loops[i]);
        w.u8((uint8_t)ne);
        for (int i = 0; i < ne; i++) audioWriteEvent(w, s.events[i]);
    }
    return w.ok ? w.n : 0;
}

bool Unpack(const uint8_t* d, int len, State& out, uint64_t* senderUs) {
    if (!d || len < 4) return false;
    Rd r{d, len, 0, true};
    if (r.u32() != kMagic) return false;
    const uint64_t su = r.u64();
    const uint8_t f1 = r.u8(), f2 = r.u8();
    out = State{};
    out.bodyPosOk = (f1 & 1) ? 1 : 0;  out.bodyRotOk = (f1 & 2) ? 1 : 0;
    out.meshOk    = (f1 & 4) ? 1 : 0;  out.relOk     = (f1 & 8) ? 1 : 0;
    out.feetOk    = (f1 & 16) ? 1 : 0; out.feetWorld = (f1 & 32) ? 1 : 0;
    out.onBoard   = (f1 & 64) ? 1 : 0; out.bailing   = (f1 & 128) ? 1 : 0;
    out.grounded  = (f2 & 1) ? 1 : 0;  out.crankOn   = (f2 & 2) ? 1 : 0;
    out.handOk    = (f2 & 32) ? 1 : 0; out.handWorld = (f2 & 64) ? 1 : 0;
    const bool trick = (f2 & 4) != 0;
    const bool grind = (f2 & 8) != 0;
    out.replaying = (f2 & 16) != 0;    // "I am scrubbing" -- see the pose lane in the header
    out.pushFlags = r.u8(); out.pushState = r.u8(); out.brakeState = r.u8(); out.boardMode = r.u8();
    out.pushSpeed = r.h16();
    // A push rate of 0 would FREEZE the animation and a negative would run it backwards -- neither is
    // a thing the sender can be in, so a decode that produces one is corruption, not a slow push.
    if (!(out.pushSpeed > 0.01f && out.pushSpeed < 8.f)) out.pushSpeed = 1.0f;
    out.crankDefOff = r.u16();
    { const float cp = r.h16(); out.crankPocket = (cp > -10.f && cp < 10.f) ? cp : 0; }
    { const float gp = r.h16(); out.grindPitch  = (gp > -1001.f && gp < 1001.f) ? gp : 0;
      const float gy = r.h16(); out.grindYaw    = (gy > -1001.f && gy < 1001.f) ? gy : 0; }
    for (int i = 0; i < 3; i++) out.bodyPos[i] = r.f32();
    qUnpack(r.u32(), out.deckQuat);
    for (int i = 0; i < 3; i++) out.deckPos[i] = out.bodyPos[i] + r.h16();
    if (out.bodyRotOk) qUnpack(r.u32(), out.bodyQuat);
    if (out.meshOk)    qUnpack(r.u32(), out.meshQuat);
    if (out.relOk)     for (int i = 0; i < 3; i++) out.relPos[i] = r.h16();
    if (out.feetOk) {
        for (int i = 0; i < 3; i++) out.lFootPos[i] = r.f32();
        for (int i = 0; i < 3; i++) out.lFootRot[i] = r.h16();
        for (int i = 0; i < 3; i++) out.rFootPos[i] = r.f32();
        for (int i = 0; i < 3; i++) out.rFootRot[i] = r.h16();
    }
    if (out.handOk) {
        for (int i = 0; i < 3; i++) out.lHandPos[i] = r.f32();
        for (int i = 0; i < 3; i++) out.lHandRot[i] = r.h16();
        for (int i = 0; i < 3; i++) out.rHandPos[i] = r.f32();
        for (int i = 0; i < 3; i++) out.rHandRot[i] = r.h16();
    }
    if (trick) {
        const uint8_t l = r.u8();
        if (l >= sizeof(out.trickName)) return false;
        if (!r.b(out.trickName, l)) return false;
        out.trickName[l] = 0;                                    // untrusted input: force-terminate
        sanitizeAscii(out.trickName, sizeof(out.trickName));     // ...and printable: both get logged
    }
    if (grind) {
        const uint8_t l = r.u8();
        if (l >= sizeof(out.grindName)) return false;
        if (!r.b(out.grindName, l)) return false;
        out.grindName[l] = 0;
        sanitizeAscii(out.grindName, sizeof(out.grindName));
    }
    const int nf = r.u8();
    if (nf > AnimFieldCount()) return false;                     // a different build slipped past magic
    if (nf > 0) {
        uint8_t present[16] = {}, esc[16] = {};
        const int mb = (nf + 7) / 8;
        if (mb > (int)sizeof(present) || !r.b(present, mb) || !r.b(esc, mb)) return false;
        int off = 0;
        for (int i = 0; i < nf; i++) {
            const AnimField& f = AnimFieldAt(i);
            if (present[i >> 3] & (1 << (i & 7))) {
                if (f.size == 1) out.anim[off] = r.u8();
                else {
                    const bool wide = (esc[i >> 3] & (1 << (i & 7))) != 0;
                    for (int c = 0; c < f.size / 4; c++) {
                        float v = wide ? r.f32() : r.h16();
                        if (!(v > -3.4e38f && v < 3.4e38f)) v = 0;
                        memcpy(out.anim + off + c * 4, &v, 4);
                    }
                }
            }                                                    // absent = zero (State{} zeroed it)
            off += f.size;
        }
        out.animLen = (uint16_t)off;
    }
    // ---- THE POSE LANE.
    {
        const int pn = r.u8();
        if (pn > kPoseMaxBones) return false;             // hostile/other-build packet
        for (int b = 0; b < pn; b++) {
            qUnpack(r.u32(), out.poseRot[b]);
            for (int c = 0; c < 3; c++) out.posePos[b][c] = r.h16();
            if (!finite3(out.posePos[b], 1e5f)) { out.posePos[b][0] = out.posePos[b][1] = out.posePos[b][2] = 0; }
        }
        out.poseN = (uint8_t)pn;
    }
    // ---- AUDIO. Absent entirely on a silent frame: two zero bytes.
    {
        const int nl = r.u8();
        if (nl > kAudioMaxLoops) return false;               // a different build slipped past magic
        for (int i = 0; i < nl; i++) {
            AudioLoop& l = out.loops[i];
            l.slot = r.u8(); l.attach = r.u8();
            const uint8_t np = r.u8();
            if (np > kAudioMaxParams) return false;
            if (!audioReadStr(r, l.cue, kAudioCueLen)) return false;
            for (int c = 0; c < 3; c++) l.rel[c] = r.h16();
            l.vol = r.h16(); l.pitch = r.h16();
            l.nParam = np;
            for (int p = 0; p < np; p++) {
                if (!audioReadStr(r, l.param[p].name, kAudioParamLen)) return false;
                l.param[p].value = (int16_t)r.u16();
            }
            if (l.attach > kAudRoot) l.attach = kAudWorld;    // unknown placement degrades to a point
            if (!finite3(l.rel, 1e5f)) { l.rel[0] = l.rel[1] = l.rel[2] = 0; }
            if (!(l.vol > -1e4f && l.vol < 1e4f)) l.vol = 1.f;
            if (!(l.pitch > 0.f && l.pitch < 1e4f)) l.pitch = 1.f;
        }
        out.nLoops = (uint8_t)nl;
        const int ne = r.u8();
        if (ne > kAudioMaxEvents) return false;
        for (int i = 0; i < ne; i++) {
            AudioEvent& e = out.events[i];
            e.id = r.u16(); e.attach = r.u8();
            if (!audioReadStr(r, e.cue, kAudioCueLen)) return false;
            for (int c = 0; c < 3; c++) e.rel[c] = r.h16();
            e.vol = r.h16(); e.pitch = r.h16(); e.start = r.h16(); e.ageMs = r.u16();
            if (e.attach > kAudRoot) e.attach = kAudWorld;
            if (!finite3(e.rel, 1e5f)) { e.rel[0] = e.rel[1] = e.rel[2] = 0; }
            if (!(e.vol > -1e4f && e.vol < 1e4f)) e.vol = 1.f;
            if (!(e.pitch > 0.f && e.pitch < 1e4f)) e.pitch = 1.f;
            if (!(e.start >= 0.f && e.start < 1e4f)) e.start = 0.f;
        }
        out.nEvents = (uint8_t)ne;
    }
    if (!r.ok) return false;
    // Post-decode discipline: no packet field is an index or a pointer, and poses are range-checked,
    // so a hostile or corrupt packet can at worst move a proxy, never corrupt local state.
    // (Quats are unit by construction of qUnpack; f16 fields can still smuggle inf -- check.)
    if (!finite3(out.bodyPos, 1e7f) || !finite3(out.deckPos, 1e7f)) return false;
    if (out.feetOk && (!finite3(out.lFootPos, 1e7f) || !finite3(out.rFootPos, 1e7f)
                    || !finite3(out.lFootRot, 1e6f) || !finite3(out.rFootRot, 1e6f))) out.feetOk = 0;
    if (out.handOk && (!finite3(out.lHandPos, 1e7f) || !finite3(out.rHandPos, 1e7f)
                    || !finite3(out.lHandRot, 1e6f) || !finite3(out.rHandRot, 1e6f))) out.handOk = 0;
    if (out.relOk && !finite3(out.relPos, 1e6f)) out.relOk = 0;
    if (senderUs) *senderUs = su;
    return true;
}

// ============================ COSMETICS CODEC ========================================================
// Its own magic, because it is its own message type on the same transport. Bumping the SNAPSHOT magic
// does not need to bump this one and vice versa -- they are independently versioned by construction.
// All three series share ONE "OMP?" namespace and must never collide, retired values included: a
// letter reused across series makes a peer on an older build parse pose packets as cosmetics instead
// of rejecting them. See the magic lineage at the top of the file for the claimed letters.
static const uint32_t kCosMagic = 0x47504D4Fu;   // "OMPG"

bool IsCosmeticsPacket(const uint8_t* d, int len) {
    if (!d || len < 4) return false;
    uint32_t m = 0; memcpy(&m, d, 4);
    return m == kCosMagic;
}

// One item: category, variant, instance, then a length-prefixed name. Names are the only variable part,
// which is why the packer measures before it writes.
static int cosColorSize(const CosmeticColor& c) {
    return 1 + (int)strnlen(c.key, sizeof(c.key) - 1) + 1 + 8;   // f16 x4: colours are small magnitudes
}
static int cosItemSize(const CosmeticItem& it) {
    int n = 4 + 4 + 1 + 1 + (int)strnlen(it.name, sizeof(it.name) - 1)
          + 4 + 1 + 1;                                   // instVariant, sockHeight, nColors
    for (int i = 0; i < it.nColors && i < (int)(sizeof(it.colors)/sizeof(it.colors[0])); i++)
        n += cosColorSize(it.colors[i]);
    return n;
}
static void cosWriteItem(Wr& w, const CosmeticItem& it) {
    const uint8_t l = (uint8_t)strnlen(it.name, sizeof(it.name) - 1);
    w.u32((uint32_t)it.cat); w.u32((uint32_t)it.variant); w.u8(it.inst); w.u8(l); w.b(it.name, l);
    w.u32((uint32_t)it.instVariant); w.u8(it.sockHeight);
    const int maxC = (int)(sizeof(it.colors)/sizeof(it.colors[0]));
    const uint8_t nc = (uint8_t)(it.nColors < maxC ? it.nColors : maxC);
    w.u8(nc);
    for (int i = 0; i < nc; i++) {
        const CosmeticColor& c = it.colors[i];
        const uint8_t kl = (uint8_t)strnlen(c.key, sizeof(c.key) - 1);
        w.u8(kl); w.b(c.key, kl); w.u8(c.enabled);
        for (int k = 0; k < 4; k++) w.h16(c.rgba[k]);
    }
}
static bool cosReadItem(Rd& r, CosmeticItem& it) {
    it.cat = (int32_t)r.u32(); it.variant = (int32_t)r.u32(); it.inst = r.u8();
    const uint8_t l = r.u8();
    if (l >= sizeof(it.name)) return false;
    if (!r.b(it.name, l)) return false;
    it.name[l] = 0;                                  // untrusted input: force-terminate
    sanitizeAscii(it.name, sizeof(it.name));         // every unresolved item gets logged BY NAME
    it.instVariant = (int32_t)r.u32(); it.sockHeight = r.u8();
    const uint8_t nc = r.u8();
    if (nc > (int)(sizeof(it.colors)/sizeof(it.colors[0]))) return false;
    for (int i = 0; i < nc; i++) {
        CosmeticColor& c = it.colors[i];
        const uint8_t kl = r.u8();
        if (kl >= sizeof(c.key)) return false;
        if (!r.b(c.key, kl)) return false;
        c.key[kl] = 0;
        sanitizeAscii(c.key, sizeof(c.key));
        c.enabled = r.u8();
        for (int k = 0; k < 4; k++) {
            const float v = r.h16();
            c.rgba[k] = (v > -1e4f && v < 1e4f) ? v : 0.f;   // untrusted: no NaN/Inf into a material
        }
    }
    it.nColors = nc;
    return true;
}

int PackCosmetics(const CosmeticSet& c, uint8_t section, uint8_t* out, int cap) {
    if (!out || cap < 32 || section > kCosBoard) return 0;
    const bool board = (section == kCosBoard);
    const CosmeticItem* src = board ? c.brd : c.chr;
    const int srcN = board ? (int)c.nBoard : (int)c.nChar;
    const int srcCap = board ? (int)(sizeof(c.brd)/sizeof(c.brd[0])) : (int)(sizeof(c.chr)/sizeof(c.chr[0]));
    const int nameLen = (int)strnlen(c.skaterName, sizeof(c.skaterName) - 1);
    const int vdefLen = (int)strnlen(c.visualDef,  sizeof(c.visualDef)  - 1);
    const int mapLen  = (int)strnlen(c.mapName,    sizeof(c.mapName)    - 1);
    // header = magic + section + stance + digest + 3 length-prefixed names + count
    int used = 4 + 1 + 1 + 8 + 1 + nameLen + 1 + vdefLen + 1 + mapLen + 1;
    // Decide how many slots FIT before writing any, so the count in the header is always truthful.
    int n = 0;
    for (int i = 0; i < srcN && i < srcCap; i++) {
        const int sz = cosItemSize(src[i]); if (used + sz > cap) break; used += sz; n++;
    }
    Wr w{out, cap, 0, true};
    w.u32(kCosMagic);
    w.u8(section);
    w.u8(c.stance);
    w.u64(c.modDigest);
    w.u8((uint8_t)nameLen); w.b(c.skaterName, nameLen);
    w.u8((uint8_t)vdefLen); w.b(c.visualDef,  vdefLen);
    w.u8((uint8_t)mapLen);  w.b(c.mapName,    mapLen);
    w.u8((uint8_t)n);
    for (int i = 0; i < n; i++) cosWriteItem(w, src[i]);
    return w.ok ? w.n : 0;
}

bool UnpackCosmetics(const uint8_t* d, int len, CosmeticSet& out, uint8_t* sectionOut) {
    if (!d || len < 4) return false;
    Rd r{d, len, 0, true};
    if (r.u32() != kCosMagic) return false;
    const uint8_t section = r.u8();
    if (section > kCosBoard) return false;
    memset(&out, 0, sizeof(out));      // padding included: the session memcmp's these to detect changes
    out.stance = r.u8();
    out.modDigest = r.u64();
    // These three are the most exposed strings on the wire: the peer's NAME is drawn in the world and
    // written to the log, and the MAP name reaches the lobby browser's rows. Terminate AND sanitise --
    // a length-bounded string is still an arbitrary byte string.
    { const uint8_t l = r.u8();
      if (l >= sizeof(out.skaterName) || !r.b(out.skaterName, l)) return false;
      out.skaterName[l] = 0; sanitizeAscii(out.skaterName, sizeof(out.skaterName)); }
    { const uint8_t l = r.u8();
      if (l >= sizeof(out.visualDef) || !r.b(out.visualDef, l)) return false;
      out.visualDef[l] = 0; sanitizeAscii(out.visualDef, sizeof(out.visualDef)); }
    { const uint8_t l = r.u8();
      if (l >= sizeof(out.mapName) || !r.b(out.mapName, l)) return false;
      out.mapName[l] = 0; sanitizeAscii(out.mapName, sizeof(out.mapName)); }
    const uint8_t n = r.u8();
    CosmeticItem* dst = (section == kCosBoard) ? out.brd : out.chr;
    const int dstCap = (section == kCosBoard) ? (int)(sizeof(out.brd)/sizeof(out.brd[0]))
                                              : (int)(sizeof(out.chr)/sizeof(out.chr[0]));
    if (n > dstCap) return false;
    for (int i = 0; i < n; i++) if (!cosReadItem(r, dst[i])) return false;
    if (!r.ok) return false;
    if (section == kCosBoard) out.nBoard = n; else out.nChar = n;
    if (sectionOut) *sectionOut = section;
    return true;
}

// ============================ CHAT CODEC =============================================================
// Same "OMP?" namespace as the other two, retired values included -- see the magic lineage at the top
// of the file before claiming another letter.
static const uint32_t kChatMagic = 0x48504D4Fu;   // "OMPH"

bool IsChatPacket(const uint8_t* d, int len) {
    if (!d || len < 4) return false;
    uint32_t m = 0; memcpy(&m, d, 4);
    return m == kChatMagic;
}

int PackChat(const ChatMsg& m, uint8_t* out, int cap) {
    const int nameLen = (int)strnlen(m.name, sizeof(m.name) - 1);
    const int textLen = (int)strnlen(m.text, sizeof(m.text) - 1);
    if (!out || !textLen || cap < 4 + 4 + 1 + nameLen + 1 + textLen) return 0;
    Wr w{out, cap, 0, true};
    w.u32(kChatMagic);
    w.u32(m.id);
    w.u8((uint8_t)nameLen); w.b(m.name, nameLen);
    w.u8((uint8_t)textLen); w.b(m.text, textLen);
    return w.ok ? w.n : 0;
}

bool UnpackChat(const uint8_t* d, int len, ChatMsg& out) {
    if (!d || len < 4) return false;
    Rd r{d, len, 0, true};
    if (r.u32() != kChatMagic) return false;
    memset(&out, 0, sizeof(out));
    out.id = r.u32();
    // This is the most exposed string the mod has: it is typed by a stranger and DRAWN. Both fields
    // are length-bounded AND sanitised -- a bounded string is still an arbitrary byte string, and one
    // that reaches a text renderer can carry control characters that a name field never would.
    { const uint8_t l = r.u8();
      if (l >= sizeof(out.name) || !r.b(out.name, l)) return false;
      out.name[l] = 0; sanitizeAscii(out.name, sizeof(out.name)); }
    { const uint8_t l = r.u8();
      if (l >= sizeof(out.text) || !r.b(out.text, l)) return false;
      out.text[l] = 0; sanitizeAscii(out.text, sizeof(out.text)); }
    return r.ok && out.text[0] != 0;
}

// ------------------------------------------------------------------ math
static void lerp3(const float* a, const float* b, float t, float* o) {
    for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}
// nlerp with hemisphere fix -- q and -q are one rotation; without the sign flip an interpolation across
// hemispheres takes the long way round. Kept in one place so no caller has to remember it.
static void qLerp(const float* a, const float* b, float t, float* o) {
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float s = (d < 0.f) ? -1.f : 1.f;
    float n = 0;
    for (int i = 0; i < 4; i++) { o[i] = a[i] + (s*b[i] - a[i]) * t; n += o[i]*o[i]; }
    if (n > 1e-8f) { n = 1.f / sqrtf(n); for (int i = 0; i < 4; i++) o[i] *= n; }
    else memcpy(o, a, 16);
}
// Rotators (UE pitch/yaw/roll degrees) interpolate THROUGH quaternions -- a foot socket swings tens of
// degrees in a flip and Euler components neither wrap nor blend over an arc like that.
static void rotToQuat(const float* r, float* q) {
    const float d2r = 0.0174532925f * 0.5f;
    const float cp = cosf(r[0]*d2r), sp = sinf(r[0]*d2r);
    const float cy = cosf(r[1]*d2r), sy = sinf(r[1]*d2r);
    const float cr = cosf(r[2]*d2r), sr = sinf(r[2]*d2r);
    q[0] =  cr*sp*sy - sr*cp*cy;  q[1] = -cr*sp*cy - sr*cp*sy;
    q[2] =  cr*cp*sy - sr*sp*cy;  q[3] =  cr*cp*cy + sr*sp*sy;
}
static void quatToRot(const float* q, float* r) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float sing = w*y - x*z;
    const float r2d = 57.2957795f;
    if (fabsf(sing) > 0.49999f) {                    // gimbal poles
        r[0] = (sing > 0 ? 90.f : -90.f);
        r[1] = 2.f * atan2f(z, w) * r2d * (sing > 0 ? 1.f : -1.f);
        r[2] = 0;
        return;
    }
    r[0] = asinf(2.f*sing) * r2d;
    r[1] = atan2f(2.f*(w*z + x*y), 1.f - 2.f*(y*y + z*z)) * r2d;
    r[2] = atan2f(2.f*(w*x + y*z), 1.f - 2.f*(x*x + z*z)) * r2d;
}
static void rotLerp(const float* a, const float* b, float t, float* o) {
    float qa[4], qb[4], qo[4];
    rotToQuat(a, qa); rotToQuat(b, qb); qLerp(qa, qb, t, qo); quatToRot(qo, o);
}
static float wrapLerpDeg(float a, float b, float t) {          // shortest arc: 179 -> -179 is 2 degrees
    float d = b - a;
    while (d >  180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return a + d * t;
}
static float expT(float ratePerSec, float dtSec) { return 1.f - expf(-ratePerSec * dtSec); }

// ------------------------------------------------------------------ the stream
void Stream::Push(const State& s, uint64_t senderUs, uint64_t localUs) {
    // Audio one-shots are filed BEFORE the staleness gate on purpose: they are deduped by id and
    // stamped with the SENDER's own time, so a reordered packet carrying a repeat we have not seen
    // yet is free extra redundancy rather than something to discard with the pose.
    audioFile(s, senderUs);
    // Push transitions likewise: filed from EVERY arriving packet, before the staleness gate, because
    // an edge that falls between two sampled snapshots is an edge lost forever.
    pushFile(s, senderUs);
    // Monotonic by construction on the sender; anything not newer is a duplicate/reorder off the wire.
    if (count_ > 0 && (int64_t)(senderUs - ring_[head_].t) <= 0) { st_.stale++; return; }
    if (count_ > 0) {
        const float gapMs = (float)((double)(int64_t)(senderUs - ring_[head_].t) / 1000.0);
        if (gapMs > 0 && gapMs < 500) {              // cadence: rise fast, relax slow
            const float k = (gapMs > gapAvgMs_) ? 0.35f : 0.03f;
            gapAvgMs_ += (gapMs - gapAvgMs_) * k;
        }
        // Network jitter: |arrival delta - sender delta|, DEADBANDED by the poll quantum (a 60 Hz
        // sender polled at 60 Hz otherwise shows a full frame of phantom variance), then the
        // asymmetric EMA -- expand fast on a spike, contract slowly through recovery.
        if (lastArriveUs_) {
            double var = fabs((double)(int64_t)(localUs - lastArriveUs_) / 1000.0 - (double)gapMs);
            var -= tun_.pollQuantumMs; if (var < 0) var = 0;
            if (var < 1000) {                        // an alt-tab is not jitter
                const double a = (var > netJitMs_) ? 0.2 : 0.01;
                netJitMs_ = (float)(a * var + (1.0 - a) * netJitMs_);
                // Peak with slow linear decay: this is what the FLOOR rides. A burst spikes it instantly
                // to the burst's real size; it bleeds out over seconds instead of averaging into noise.
                const float dtS = (float)((double)(int64_t)(localUs - lastArriveUs_) * 1e-6);
                jitPeakMs_ -= tun_.jitterPeakDecay * (dtS > 0 ? dtS : 0);
                if (jitPeakMs_ < 0) jitPeakMs_ = 0;
                if ((float)var > jitPeakMs_) jitPeakMs_ = (float)var;
            }
        }
    }
    lastArriveUs_ = localUs;
    head_ = (head_ + 1) % kRing;
    ring_[head_].s = s; ring_[head_].t = senderUs;
    if (count_ < kRing) count_++;
    st_.pushed++; st_.netJitMs = jitPeakMs_; st_.gapAvgMs = gapAvgMs_;   // report what steers the floor
}

bool Stream::Sample(uint64_t localUs, State& out) {
    if (count_ < 2) return false;
    int64_t dt = lastLocalUs_ ? (int64_t)(localUs - lastLocalUs_) : 0;
    if (dt < 0) dt = 0;
    if (dt > 100000) dt = 100000;                    // a hitch must not fast-forward past the ring
    lastLocalUs_ = localUs;
    const Snap& newest = ring_[head_];

    // Delay = base, floored by cadence AND measured jitter.
    float jitAllow = jitPeakMs_ * tun_.jitterAllow;
    if (jitAllow > tun_.jitterCapMs) jitAllow = tun_.jitterCapMs;
    const float wantFloor = tun_.gapFloorMult * gapAvgMs_ + jitAllow;
    if (wantFloor > floorMs_ + 4.f || wantFloor < floorMs_ - 8.f) floorMs_ = wantFloor;  // hysteresis
    float wantDelay = tun_.baseDelayMs;
    if (wantDelay < floorMs_) wantDelay = floorMs_;
    const float slew = (wantDelay > delayMs_ ? tun_.delaySlewUpMsS : tun_.delaySlewDownMsS) * ((float)dt * 1e-6f);
    const float dd = wantDelay - delayMs_;
    delayMs_ += (dd >  slew) ?  slew : (dd < -slew ? -slew : dd);
    const float delayMs = delayMs_;
    st_.delayMs = delayMs;

    const int64_t target = (int64_t)newest.t - (int64_t)(delayMs * 1000.f);
    const float dtSec = (float)dt * 1e-6f;
    if (!playUs_) { playUs_ = (uint64_t)target; errAvgUs_ = 0; st_.resyncs++; }
    else if ((int64_t)playUs_ >= (int64_t)newest.t) {
        // STARVING: the sender is silent, so the error against a stale target is MEANINGLESS -- acting
        // on it resync-snaps the clock BACKWARDS once per burst, worth ~140 cm of output jump. Never
        // act on a signal whose absence is indistinguishable from "never happened". Advance in real
        // time, clamp at the extrapolation bound, and clear the trend so stale error cannot kick when
        // data resumes.
        playUs_ += (uint64_t)dt;
        const int64_t clampUs = (int64_t)newest.t + (int64_t)(tun_.extrapMaxMs * 1000.f);
        if ((int64_t)playUs_ > clampUs) playUs_ = (uint64_t)clampUs;
        errAvgUs_ = 0;
    }
    else {
        const int64_t err = target - (int64_t)playUs_;
        if (llabs(err) > (int64_t)(tun_.resyncMs * 1000.f)) { playUs_ = (uint64_t)target; errAvgUs_ = 0; st_.resyncs++; }
        else {
            // The target is a STAIRCASE (it moves only when a packet lands), so the raw error is a
            // sawtooth a full packet interval tall. Track its TREND and slew onto that, per second.
            errAvgUs_ += ((double)err - errAvgUs_) * (double)expT(tun_.errFilterRate, dtSec);
            int64_t corr = (int64_t)(errAvgUs_ * (double)expT(tun_.catchupRate, dtSec));
#ifdef OMP_CLOCK_TRACE
            { static int n = 0; if (++n % 30 == 0)
                fprintf(stderr, "[clk] err=%+7lld errAvg=%+8.0f corr=%+6lld dt=%lld newestMinusPlay=%+lld\n",
                        (long long)err, errAvgUs_, (long long)corr, (long long)dt,
                        (long long)((int64_t)newest.t - (int64_t)playUs_)); }
#endif
            const int64_t cap = (int64_t)(dt * tun_.rateCap);
            if (corr >  cap) corr =  cap;
            if (corr < -cap) corr = -cap;
            playUs_ += (uint64_t)(dt + corr);
            if (dt > 1000) {
                const float rate = 1.f + (float)corr / (float)dt;
                if (rate < st_.rateMin) st_.rateMin = rate;
                if (rate > st_.rateMax) st_.rateMax = rate;
            }
        }
    }

    // Bracket the playhead. It must never fall BEHIND the ring (a delay larger than the buffered span
    // would otherwise pin it on the hold-oldest branch, stepping at packet cadence forever).
    const int oldest = (head_ - (count_ - 1) + kRing * 2) % kRing;
    if ((int64_t)playUs_ < (int64_t)ring_[oldest].t) playUs_ = ring_[oldest].t;
    const Snap* A = nullptr; const Snap* B = nullptr;
    for (int k = 0; k < count_; k++) {
        const Snap* s = &ring_[(oldest + k) % kRing];
        if ((int64_t)(s->t - playUs_) <= 0) A = s; else { B = s; break; }
    }
    bool starvedNow = false;
    if (!A) { A = &ring_[oldest]; B = nullptr; }                 // buffer still filling: hold oldest
    else if (!B) { st_.starved++; starvedNow = true; }           // ran off the data: hold newest
    float t = 0;
    if (B) {
        const double span = (double)(int64_t)(B->t - A->t);
        if (span > 0) t = (float)((double)(int64_t)(playUs_ - A->t) / span);
        if (t < 0) t = 0; else if (t > 1) t = 1;
        const float dx = B->s.deckPos[0]-A->s.deckPos[0], dy = B->s.deckPos[1]-A->s.deckPos[1],
                    dz = B->s.deckPos[2]-A->s.deckPos[2];
        if (dx*dx + dy*dy + dz*dz > tun_.snapDistCm * tun_.snapDistCm) { A = B; t = 0; }   // teleport
    }
    st_.alpha = t;
    st_.leadMs = (float)((double)((int64_t)newest.t - (int64_t)playUs_) / 1000.0);

    // Interpolate. `out` starts as *A, so every field that is not blended is the at-render-time value
    // -- exactly the stepped-not-blended rule for bools, enums and the anim blob.
    out = A->s;
    if (B && t > 0) {
        lerp3(A->s.deckPos, B->s.deckPos, t, out.deckPos);
        qLerp(A->s.deckQuat, B->s.deckQuat, t, out.deckQuat);
        if (A->s.meshOk && B->s.meshOk) qLerp(A->s.meshQuat, B->s.meshQuat, t, out.meshQuat);
        if (A->s.bodyRotOk && B->s.bodyRotOk) {
            qLerp(A->s.bodyQuat, B->s.bodyQuat, t, out.bodyQuat);   // hemisphere-safe; the euler fields
            out.bodyYaw   = wrapLerpDeg(A->s.bodyYaw, B->s.bodyYaw, t);        // below are debug-only
            out.bodyPitch = A->s.bodyPitch + (B->s.bodyPitch - A->s.bodyPitch) * t;
            out.bodyRoll  = A->s.bodyRoll  + (B->s.bodyRoll  - A->s.bodyRoll)  * t;
        }
        if (A->s.bodyPosOk && B->s.bodyPosOk) lerp3(A->s.bodyPos, B->s.bodyPos, t, out.bodyPos);
        if (A->s.relOk && B->s.relOk)         lerp3(A->s.relPos,  B->s.relPos,  t, out.relPos);
        if (A->s.feetOk && B->s.feetOk && A->s.feetWorld == B->s.feetWorld) {
            lerp3(A->s.lFootPos, B->s.lFootPos, t, out.lFootPos);
            lerp3(A->s.rFootPos, B->s.rFootPos, t, out.rFootPos);
            // rotators STEP (out = A already): rotLerp routes through the quatToRot/rotToQuat pair,
            // which is NOT self-inverse. A stepped 60 Hz rotator is correct by construction.
        }
        if (A->s.handOk && B->s.handOk && A->s.handWorld == B->s.handWorld) {
            lerp3(A->s.lHandPos, B->s.lHandPos, t, out.lHandPos);
            lerp3(A->s.rHandPos, B->s.rHandPos, t, out.rHandPos);
            // rotators STEP (out = A already), for the same reason as the feet above: rotLerp routes
            // through the quatToRot/rotToQuat pair, which is NOT self-inverse and flips the pose at
            // the poles. No euler round-trip on an interpolation path, ever.
        }
        if (A->s.artOk && B->s.artOk) {
            qLerp(A->s.truckB, B->s.truckB, t, out.truckB);
            qLerp(A->s.truckF, B->s.truckF, t, out.truckF);
            if (A->s.artOk >= 2 && B->s.artOk >= 2) qLerp(A->s.wheelBL, B->s.wheelBL, t, out.wheelBL);
        }
        // The pose blob: whitelisted continuous floats blend between the bracketing snapshots; every
        // bool/enum/state byte comes from A unblended, which copying A already did.
        // Unequal lengths = the sender changed mid-stream (build boundary); step, never mis-align.
        // Unaligned float access is fine on x64, which is what the 1/4/12-byte field packing implies.
        if (A->s.animLen && A->s.animLen == B->s.animLen) {
            int n = 0;
            for (int i = 0; i < AnimFieldCount(); i++) {
                const AnimField& f = AnimFieldAt(i);
                if (n + f.size > (int)A->s.animLen) break;
                if (AnimFieldSmoothed(f.off, f.size)) {
                    const float* fa = (const float*)(A->s.anim + n);
                    const float* fb = (const float*)(B->s.anim + n);
                    float* fo = (float*)(out.anim + n);
                    for (int c = 0; c < f.size / 4; c++) fo[c] = fa[c] + (fb[c] - fa[c]) * t;
                }
                n += f.size;
            }
        }
    }
    // Starved -> project POSITION ONLY along the velocity implied by the two newest samples, bounded in
    // time and distance. Pose is never guessed, and the deck and the body get the SAME displacement or
    // the board slides out of the rider's hands.
    if (starvedNow && tun_.extrapMaxMs > 0 && count_ >= 2) {
        const Snap& p1 = ring_[head_];
        const Snap& p0 = ring_[(head_ - 1 + kRing) % kRing];
        const double spanS  = (double)(int64_t)(p1.t - p0.t) * 1e-6;
        const double aheadS = (double)((int64_t)playUs_ - (int64_t)p1.t) * 1e-6;
        if (spanS > 0.001 && spanS < 0.25 && aheadS > 0) {
            double dtS = aheadS;
            const double capS = tun_.extrapMaxMs * 1e-3;
            if (dtS > capS) dtS = capS;
            float d[3]; double mag2 = 0; bool sane = true;
            for (int i = 0; i < 3; i++) {
                d[i] = (float)(((double)p1.s.deckPos[i] - (double)p0.s.deckPos[i]) / spanS * dtS);
                mag2 += (double)d[i] * d[i];
                if (!(d[i] > -1e6f && d[i] < 1e6f)) sane = false;
            }
            if (sane && mag2 <= (double)tun_.extrapMaxCm * tun_.extrapMaxCm) {
                for (int i = 0; i < 3; i++) out.deckPos[i] += d[i];
                if (out.bodyPosOk) for (int i = 0; i < 3; i++) out.bodyPos[i] += d[i];
                st_.extrap++;
            }
        }
    }
    st_.sampled++;
    return true;
}


void Stream::Reset() {
    // A slot handed to a NEW peer must carry NOTHING across: a stale playback clock or ring would render
    // the newcomer with the previous player's timing.
    st_ = Stats{};
    head_ = -1; count_ = 0;
    playUs_ = lastLocalUs_ = lastArriveUs_ = 0;
    errAvgUs_ = 0;
    gapAvgMs_ = 16.7f; floorMs_ = 21.0f; netJitMs_ = 0; jitPeakMs_ = 0; delayMs_ = 21.0f;
    audioN_ = 0; seenHead_ = 0;
    for (int i = 0; i < kAudioSeen; i++) { seenId_[i] = 0; seenOk_[i] = 0; }
    // lastPushedState_ goes back to -1, not 0: a new peer's FIRST packet must not read as a transition
    // into whatever state they happen to be in, or a joiner mid-push fires a phantom push locally.
    pushN_ = 0; lastPushedState_ = -1;
}

// ---- the audio one-shot queue -----------------------------------------------------------------------
// Each event is repeated in several consecutive packets so a drop cannot silence it; the id ring turns
// that redundancy back into exactly one playback. Ids are u16 and WRAP, so a parallel occupied flag is
// what distinguishes "id 0 was seen" from "this slot was never used".
bool Stream::audioSeen(uint16_t id) {
    for (int i = 0; i < kAudioSeen; i++) if (seenOk_[i] && seenId_[i] == id) return true;
    return false;
}
void Stream::audioRemember(uint16_t id) {
    seenId_[seenHead_] = id; seenOk_[seenHead_] = 1;
    seenHead_ = (seenHead_ + 1) % kAudioSeen;
}
void Stream::audioFile(const State& s, uint64_t senderUs) {
    for (int i = 0; i < s.nEvents && i < kAudioMaxEvents; i++) {
        const AudioEvent& e = s.events[i];
        if (!e.cue[0] || audioSeen(e.id)) continue;
        audioRemember(e.id);
        if (audioN_ >= kAudioQueue) continue;                 // a full queue drops the newest: the ones
                                                              // already filed are older and closer to due
        const uint64_t age = (uint64_t)e.ageMs * 1000ull;
        audioQ_[audioN_].e = e;
        audioQ_[audioN_].t = (senderUs > age) ? (senderUs - age) : senderUs;
        audioN_++;
    }
}
int Stream::DrainAudio(AudioEvent* out, int cap) {
    if (!out || cap <= 0 || !playUs_) return 0;               // no playback clock yet = nothing is "due"
    const uint64_t stale = (uint64_t)kAudioStaleMs * 1000ull;
    int n = 0;
    for (int i = 0; i < audioN_; ) {
        if ((int64_t)(audioQ_[i].t - playUs_) > 0) { i++; continue; }   // not due yet
        // A FULL output buffer must LEAVE the event queued, not drop it. Removing it here would
        // silently discard sounds exactly when several land in one frame -- which is a landing, i.e.
        // the case this whole path exists for.
        if (n >= cap) { i++; continue; }
        // Due. Play it unless the playhead has already run far past it -- after a stall the clock
        // resyncs FORWARD, and firing the whole backlog at once is a burst of machine-gun pops.
        if (playUs_ - audioQ_[i].t < stale) out[n++] = audioQ_[i].e;
        audioQ_[i] = audioQ_[--audioN_];                      // swap-remove; order among simultaneous
    }                                                         // events does not matter, they are sounds
    return n;
}

// ---- the push-transition queue ----------------------------------------------------------------------
void Stream::pushFile(const State& s, uint64_t senderUs) {
    if ((int)s.pushState == lastPushedState_) return;          // no edge in this packet
    lastPushedState_ = (int)s.pushState;
    if (pushN_ >= kPushQueue) return;                          // a full queue drops the newest, as audio does
    // Insert in SENDER-TIME order. Packets can arrive reordered, and a push machine driven through a
    // sequence that never happened is worse than a dropped edge.
    int i = pushN_;
    while (i > 0 && (int64_t)(pushQ_[i - 1].t - senderUs) > 0) { pushQ_[i] = pushQ_[i - 1]; i--; }
    pushQ_[i].state = s.pushState;
    pushQ_[i].t     = senderUs;
    pushN_++;
}

int Stream::DrainPushStates(uint8_t* out, int cap) {
    if (!out || cap <= 0 || !playUs_) return 0;                // no playback clock yet = nothing is "due"
    const uint64_t stale = (uint64_t)kPushStaleMs * 1000ull;
    int n = 0, consumed = 0;
    for (; consumed < pushN_; consumed++) {
        // Sorted by time, so the first not-yet-due entry ends the pass -- nothing behind it can be due.
        if ((int64_t)(pushQ_[consumed].t - playUs_) > 0) break;
        if (n >= cap) break;                                   // leave the rest QUEUED and in order
        // Past-due transitions are consumed but not applied: after a stall the clock resyncs forward,
        // and replaying the backlog would be a burst of pushes at a moment they never happened. The
        // state they carried is superseded by the ones behind them anyway.
        if (playUs_ - pushQ_[consumed].t < stale) out[n++] = pushQ_[consumed].state;
    }
    if (consumed > 0) {
        for (int k = consumed; k < pushN_; k++) pushQ_[k - consumed] = pushQ_[k];
        pushN_ -= consumed;
    }
    return n;
}

}} // namespace omp::repl
