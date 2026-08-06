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
// omp_looptest -- the replication loop rig as a pass/fail binary, with no game and no network.
// It simulates a skater (circle at riding speed, deck spinning through periodic "tricks"), packs at
// 60 Hz, delivers through an impaired wire (latency / jitter / loss / bursts), samples the Stream at
// a receiver clock with a DIFFERENT phase, and judges the OUTPUT by a straight-line prediction
// residual on what the eye would integrate, plus clock-health counters. It also runs the codec,
// audio-event and push-transition gates below. Run it with any argument for per-second clock
// internals on the clean profile.
#include "../../src/replication/replication.h"
#include "../../src/replication/replaysync.h"
#include "../../src/replication/anim_fields.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <random>

using namespace omp::repl;

struct Profile { const char* name; double latMs, jitMs; int lossPct; double burstEveryS, burstHoldMs; };
static const Profile kProfiles[] = {
    { "clean       ",  0,  0, 0, 0,   0 },
    { "internet    ", 30, 10, 0, 0,   0 },
    { "relay65     ", 65,  5, 0, 0,   0 },
    { "lossy5%     ", 65,  5, 5, 0,   0 },
    { "bursty      ", 30,  5, 0, 2.0, 250 },
};

struct Delivery { double dueS; std::vector<uint8_t> bytes; };

static double residual(const float p0[3], double t0, const float p1[3], double t1,
                       const float p2[3], double t2) {
    const double d01 = t1 - t0, d12 = t2 - t1;
    if (d01 < 1e-4 || d12 < 1e-4 || d01 > 0.2 || d12 > 0.2) return -1;
    const double k = d12 / d01;
    double s = 0;
    for (int i = 0; i < 3; i++) {
        const double e = (double)p2[i] - ((double)p1[i] + ((double)p1[i] - (double)p0[i]) * k);
        s += e * e;
    }
    return sqrt(s);
}

// ---- the CODEC gate. The quantized wire (OMP9) round-trips every section with adversarial values;
// the tolerances asserted here are the format's promises. It also fuzzes truncation, proving no
// prefix of a valid packet can ever parse -- the bounds cursor's guarantee against corrupt input.
static bool codecCheck() {
    State s{};
    s.bodyPosOk = 1; s.bodyPos[0] = 12345.67f; s.bodyPos[1] = -9876.5f; s.bodyPos[2] = 42.125f;
    s.deckPos[0] = s.bodyPos[0] + 31.25f; s.deckPos[1] = s.bodyPos[1] - 88.5f; s.deckPos[2] = s.bodyPos[2] - 90.f;
    const float q1[4] = { 0.1830127f, 0.6830127f, -0.1830127f, 0.6830127f };   // arbitrary unit quat
    const float q2[4] = { 0.5f, -0.5f, 0.5f, -0.5f };
    const float q3[4] = { 0.f, 0.99875f, 0.05f, 0.f };
    memcpy(s.deckQuat, q1, 16);
    s.bodyRotOk = 1; memcpy(s.bodyQuat, q2, 16);
    s.meshOk = 1;    memcpy(s.meshQuat, q3, 16);
    s.relOk = 1; s.relPos[0] = -12.5f; s.relPos[1] = 3.75f; s.relPos[2] = -91.0f;
    s.feetOk = 1; s.feetWorld = 1;
    // The feet are deliberately NOWHERE NEAR bodyPos -- small, mesh-local-scale values. Feet placed
    // next to the body would share the codec's world-space assumption, letting a rel-to-body encoding
    // ship that clamps real (local) sockets and extends both legs 10 m. A gate must feed data that
    // VIOLATES the codec's assumptions, not data shaped by them.
    for (int i = 0; i < 3; i++) {
        s.lFootPos[i] = 34.25f - 20.f * i;  s.rFootPos[i] = -12.5f + 15.f * i;
        s.lFootRot[i] = -179.5f + 10.f * i; s.rFootRot[i] = 44.25f * (i + 1);
    }
    // Hand IK targets. Same trap as the feet: deliberately NOT near bodyPos, so a future "encode
    // relative to the body" optimisation is caught by this gate instead of by a player's arms.
    s.handOk = 1; s.handWorld = 1;
    for (int i = 0; i < 3; i++) {
        s.lHandPos[i] = -55.5f + 11.f * i;  s.rHandPos[i] = 27.75f - 9.f * i;
        s.lHandRot[i] = 91.25f - 40.f * i;  s.rHandRot[i] = -133.5f + 22.f * i;
    }
    s.onBoard = 1; s.grounded = 1; s.bailing = 0;
    s.pushFlags = 0x40; s.pushState = 3; s.brakeState = 2;
    s.pushSpeed = 1.734f;                       // deliberately NOT 1.0 -- see the assertion below
    s.crankOn = 1; s.crankDefOff = 3; s.crankPocket = 0.7351f;
    strcpy_s(s.trickName, "TRICK_RGS_KickFlip");
    // Grind fields. Ratios deliberately NOT 0..1 (the codec must not assume a range, only clamp at
    // +/-1000), name at a different length than the trick's, board mode = the in-hand 9.
    strcpy_s(s.grindName, "GRIND_BS_5050_Regular_Long");
    s.grindPitch = -3.875f; s.grindYaw = 271.25f;
    s.boardMode = 9;
    // anim blob: field-table-shaped, adversarial values -- zeros, ratios, a big magnitude that MUST
    // escape to f32, negatives, and byte fields.
    { int off = 0, i = 0;
      for (; i < AnimFieldCount(); i++) {
          const AnimField& f = AnimFieldAt(i);
          if (off + f.size > (int)sizeof(s.anim)) break;
          if (f.size == 1) s.anim[off] = (uint8_t)((i % 3 == 0) ? 0 : (i & 0x7f));
          else for (int c = 0; c < f.size / 4; c++) {
              float v = 0;
              switch (i % 5) { case 0: v = 0.f; break;           case 1: v = 0.73512f; break;
                               case 2: v = -123.456f; break;     case 3: v = 100000.123f; break;
                               case 4: v = 1e-4f; break; }
              memcpy(s.anim + off + c * 4, &v, 4);
          }
          off += f.size;
      }
      s.animLen = (uint16_t)off; }
    // ---- the AUDIO section, deliberately at its caps and full of values the encoding does NOT
    // promise to preserve exactly: names at the length limit, a negative parameter value, a pitch far
    // outside 0..1, a position beyond the f16 relative clamp. The format only promises a clamp, and
    // the assertions below state exactly that and nothing more.
    s.nLoops = kAudioMaxLoops; s.nEvents = kAudioMaxEvents;
    for (int i = 0; i < kAudioMaxLoops; i++) {
        AudioLoop& l = s.loops[i];
        l.slot = (uint8_t)(200 + i);                       // slot ids are the sender's, and they wrap
        l.attach = (uint8_t)(i % 4);
        // Names are filled to the LAST usable byte. A cue field one byte too short silently truncates
        // real names such as `SCU_Onboard_WheelSpinning_Looping` (33 chars) and every lookup for that
        // sound fails. A cap is only tested by a value that reaches it.
        for (int c = 0; c < kAudioCueLen - 1; c++) l.cue[c] = (char)('A' + ((c + i) % 26));
        l.cue[kAudioCueLen - 1] = 0;
        l.rel[0] = -18.5f + i; l.rel[1] = 4.25f; l.rel[2] = 3000.f;              // z beyond the clamp
        l.vol = 0.6875f + i * 0.01f; l.pitch = 3.5f;                             // pitch is NOT 0..1
        l.nParam = kAudioMaxParams;
        // `GrindDropHeightIndex` is exactly 20 chars -- a real name that a 20-byte field mangles.
        strcpy_s(l.param[0].name, "GrindDropHeightIndex"); l.param[0].value = (int16_t)-1234;
        for (int c = 0; c < kAudioParamLen - 1; c++) l.param[1].name[c] = (char)('a' + (c % 26));
        l.param[1].name[kAudioParamLen - 1] = 0;
        l.param[1].value = (int16_t)7;
    }
    for (int i = 0; i < kAudioMaxEvents; i++) {
        AudioEvent& e = s.events[i];
        e.id = (uint16_t)(65530 + i);                      // ids near the u16 wrap
        e.attach = (uint8_t)(i % 4);
        for (int c = 0; c < kAudioCueLen - 1; c++) e.cue[c] = (char)('a' + ((c * 3 + i) % 26));
        e.cue[kAudioCueLen - 1] = 0;
        e.rel[0] = 11.f * i; e.rel[1] = -55.5f; e.rel[2] = 2.25f;
        e.vol = 1.25f; e.pitch = 0.875f; e.start = 0.5f; e.ageMs = (uint16_t)(37 + i * 400);
    }
    uint8_t pkt[1024];
    const int n = Pack(s, 123456789ull, pkt, sizeof(pkt));
    if (n <= 0) { printf("  codec: Pack failed\n"); return false; }
    State o; uint64_t su = 0;
    if (!Unpack(pkt, n, o, &su) || su != 123456789ull) { printf("  codec: Unpack failed\n"); return false; }
    int bad = 0;
    auto near1 = [&](float a, float b, float tol, const char* what) {
        if (fabsf(a - b) > tol) { printf("  codec: %s %.6f != %.6f (tol %.4f)\n", what, a, b, tol); bad++; } };
    auto qNear = [&](const float* a, const float* b, const char* what) {
        float d = 0; for (int i = 0; i < 4; i++) d += a[i] * b[i];
        if (fabsf(d) < 0.9999905f) { printf("  codec: quat %s dot=%.7f\n", what, d); bad++; } };  // <0.25 deg
    for (int i = 0; i < 3; i++) {
        near1(o.bodyPos[i], s.bodyPos[i], 0.0f, "bodyPos");                    // the anchor is exact
        near1(o.deckPos[i], s.deckPos[i], 0.15f, "deckPos");
        near1(o.relPos[i], s.relPos[i], 0.1f, "relPos");
        near1(o.lFootPos[i], s.lFootPos[i], 0.0f, "lFootPos");    // absolute f32: EXACT, any space
        near1(o.rFootPos[i], s.rFootPos[i], 0.0f, "rFootPos");
        near1(o.lFootRot[i], s.lFootRot[i], 0.25f, "lFootRot");
        near1(o.rFootRot[i], s.rFootRot[i], 0.25f, "rFootRot");
        near1(o.lHandPos[i], s.lHandPos[i], 0.0f, "lHandPos");    // absolute f32: EXACT, any space
        near1(o.rHandPos[i], s.rHandPos[i], 0.0f, "rHandPos");
        near1(o.lHandRot[i], s.lHandRot[i], 0.25f, "lHandRot");
        near1(o.rHandRot[i], s.rHandRot[i], 0.25f, "rHandRot");
    }
    qNear(o.deckQuat, s.deckQuat, "deck"); qNear(o.bodyQuat, s.bodyQuat, "body"); qNear(o.meshQuat, s.meshQuat, "mesh");
    if (strcmp(o.trickName, s.trickName)) { printf("  codec: trickName '%s'\n", o.trickName); bad++; }
    if (strcmp(o.grindName, s.grindName)) { printf("  codec: grindName '%s'\n", o.grindName); bad++; }
    near1(o.grindPitch, s.grindPitch, 0.01f, "grindPitch");
    near1(o.grindYaw,   s.grindYaw,   0.25f, "grindYaw");     // f16 at ~271: quantum ~0.25
    if (o.boardMode != 9) { printf("  codec: boardMode %d\n", o.boardMode); bad++; }
    if (o.crankDefOff != 3 || !o.crankOn) { printf("  codec: crank fields\n"); bad++; }
    near1(o.crankPocket, s.crankPocket, 0.001f, "crankPocket");
    if (o.pushFlags != 0x40 || o.pushState != 3 || o.brakeState != 2) { printf("  codec: push/brake\n"); bad++; }
    // PushSpeedMultiplier is asserted with a value that is NOT 1.0, because 1.0 is both the neutral
    // rate and every failure path's fallback -- a gate that tested 1.0 would pass on a field that
    // never made it onto the wire at all.
    near1(o.pushSpeed, s.pushSpeed, 0.005f, "pushSpeed");
    if (o.animLen != s.animLen) { printf("  codec: animLen %d != %d\n", o.animLen, s.animLen); bad++; }
    { int off = 0;
      for (int i = 0; i < AnimFieldCount(); i++) {
          const AnimField& f = AnimFieldAt(i);
          if (off + f.size > (int)o.animLen) break;
          if (f.size == 1) { if (o.anim[off] != s.anim[off]) { printf("  codec: anim byte fld %d\n", i); bad++; } }
          else for (int c = 0; c < f.size / 4; c++) {
              float a, b; memcpy(&a, s.anim + off + c * 4, 4); memcpy(&b, o.anim + off + c * 4, 4);
              if (fabsf(a - b) > 0.0015f + 0.0015f * fabsf(a)) { printf("  codec: anim fld %d %.6f != %.6f\n", i, a, b); bad++; }
          }
          off += f.size;
      } }
    // ---- audio. The promise is: every record that fits survives whole, identity (slot/id/name/
    // parameter) is exact, and magnitudes survive to f16 precision within the stated clamps.
    if (o.nLoops != s.nLoops)   { printf("  codec: nLoops %d != %d\n", o.nLoops, s.nLoops); bad++; }
    if (o.nEvents != s.nEvents) { printf("  codec: nEvents %d != %d\n", o.nEvents, s.nEvents); bad++; }
    for (int i = 0; i < o.nLoops && i < s.nLoops; i++) {
        const AudioLoop& a = s.loops[i]; const AudioLoop& b = o.loops[i];
        if (b.slot != a.slot || b.attach != a.attach) { printf("  codec: loop %d slot/attach\n", i); bad++; }
        if (strcmp(a.cue, b.cue)) { printf("  codec: loop %d cue '%s' != '%s'\n", i, b.cue, a.cue); bad++; }
        // A cue name that reaches the cap must arrive WHOLE. A truncated name is not a shorter name,
        // it is a DIFFERENT identity that can never resolve on the receiver.
        if ((int)strlen(b.cue) != kAudioCueLen - 1) {
            printf("  codec: loop %d cue truncated to %d (cap allows %d)\n", i, (int)strlen(b.cue), kAudioCueLen - 1); bad++; }
        for (int p = 0; p < b.nParam && p < a.nParam; p++)
            if (strlen(b.param[p].name) != strlen(a.param[p].name)) {
                printf("  codec: loop %d param %d name truncated\n", i, p); bad++; }
        near1(b.vol, a.vol, 0.001f, "loop vol");
        near1(b.pitch, a.pitch, 0.01f, "loop pitch");         // 3.5 survives: the clamp is +/-100
        near1(b.rel[0], a.rel[0], 0.05f, "loop rel.x");
        near1(b.rel[2], 2000.f, 1.0f, "loop rel.z clamp");    // the promise is a CLAMP, not the value
        if (b.nParam != a.nParam) { printf("  codec: loop %d nParam\n", i); bad++; }
        for (int p = 0; p < b.nParam && p < a.nParam; p++) {
            if (strcmp(a.param[p].name, b.param[p].name) || a.param[p].value != b.param[p].value) {
                printf("  codec: loop %d param %d '%s'=%d\n", i, p, b.param[p].name, b.param[p].value); bad++; }
        }
    }
    for (int i = 0; i < o.nEvents && i < s.nEvents; i++) {
        const AudioEvent& a = s.events[i]; const AudioEvent& b = o.events[i];
        if (b.id != a.id || b.attach != a.attach) { printf("  codec: event %d id/attach\n", i); bad++; }
        if (strcmp(a.cue, b.cue)) { printf("  codec: event %d cue '%s' != '%s'\n", i, b.cue, a.cue); bad++; }
        if (b.ageMs != a.ageMs) { printf("  codec: event %d ageMs %u != %u\n", i, b.ageMs, a.ageMs); bad++; }
        near1(b.vol, a.vol, 0.001f, "event vol");
        near1(b.start, a.start, 0.001f, "event start");
    }
    // A driver packet must carry NO pose -- the two lanes are alternatives, never both.
    if (o.poseN != 0) { printf("  codec: driver packet carried a pose (%u)\n", o.poseN); bad++; }
    // `replaying` rides a SPARE BIT of f2 alongside four existing flags, which is the shape of bug
    // where a new bit lands on top of an old one. `s.replaying` is false here, so this asserts the
    // default does not leak on while its four neighbours (checked above) are set.
    if (o.replaying) { printf("  codec: replaying set on a packet that never asked for it\n"); bad++; }

    // ---- THE POSE LANE, tested in the shape it actually ships in. When the sender has a pose it
    // drops the driver blob and feet, which are inert during replay playback, so "full drivers AND a
    // full skeleton" is a packet that cannot occur and asserting it would assert a fiction. What is
    // asserted: the skeleton survives WHOLE, and the lane is ALL OR NOTHING (half a skeleton is not a
    // degraded pose, it is a scrambled one).
    {
        State p = s;
        p.animLen = 0; p.feetOk = 0;                       // exactly what pose::Capture does
        p.replaying = true;                                // and the flag that asks for it
        p.poseN = 70;                                      // the measured skater rig
        for (int b = 0; b < p.poseN; b++) {
            const float a = 0.017f * (b + 1);              // deliberately NOT axis-aligned: a
            float q[4] = { sinf(a) * 0.4f, cosf(a) * 0.5f, sinf(a * 2.f) * 0.3f, 0.f };
            float l2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2];
            q[3] = sqrtf(1.f - (l2 > 0.9f ? 0.9f : l2));   // smallest-three flatters axis-aligned quats
            const float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            for (int c = 0; c < 4; c++) p.poseRot[b][c] = q[c] / n;
            p.posePos[b][0] = -37.5f + b * 1.25f;
            p.posePos[b][1] = 12.75f;
            p.posePos[b][2] = (b == 3) ? 5000.f : (float)b * -0.5f;   // one beyond the clamp
        }
        uint8_t ppkt[1024];
        const int pn = Pack(p, 42ull, ppkt, sizeof(ppkt));
        if (pn <= 0) { printf("  codec: pose Pack failed\n"); bad++; }
        else {
            State po; uint64_t psu = 0;
            if (!Unpack(ppkt, pn, po, &psu)) { printf("  codec: pose Unpack failed\n"); bad++; }
            else {
                if (po.poseN != p.poseN) { printf("  codec: poseN %u != %u\n", po.poseN, p.poseN); bad++; }
                for (int b = 0; b < po.poseN && b < p.poseN; b++) {
                    qNear(po.poseRot[b], p.poseRot[b], "poseRot");
                    near1(po.posePos[b][0], p.posePos[b][0], 0.05f, "posePos.x");
                    near1(po.posePos[b][1], p.posePos[b][1], 0.05f, "posePos.y");
                }
                if (po.poseN > 3) near1(po.posePos[3][2], 1000.f, 1.0f, "posePos clamp");
                if (po.animLen != 0) { printf("  codec: pose packet carried a driver blob\n"); bad++; }
                if (!po.replaying) { printf("  codec: replaying lost in transit\n"); bad++; }
                // The four f2 neighbours must survive the new bit -- this catches a bit that lands on
                // one of them.
                if (!po.grounded || !po.crankOn || !po.trickName[0] || !po.grindName[0]) {
                    printf("  codec: replaying clobbered an f2 neighbour\n"); bad++; }
                for (int L = 0; L < pn; L++) { State t; if (Unpack(ppkt, L, t, nullptr)) {
                    printf("  codec: truncated POSE packet parsed at %d/%d\n", L, pn); bad++; break; } }
                printf("codec pose lane: %d bytes for a %u-bone skeleton\n", pn, po.poseN);
            }
        }
    }
    // The same hostile-string rule on the SNAPSHOT lane. These two names are logged verbatim by the
    // proxy ("trick def '%s' -> RESOLVED"), so a newline here forges log lines just as well.
    {
        State hs = s;
        snprintf(hs.trickName, sizeof(hs.trickName), "Trick\n[proxy] forged\x1b");
        snprintf(hs.grindName, sizeof(hs.grindName), "Grind\r\x7f\xc3\xa9");
        uint8_t hb[1024]; State ho;
        const int hn = Pack(hs, 7ull, hb, sizeof(hb));
        if (hn <= 0 || !Unpack(hb, hn, ho, nullptr)) {
            printf("  codec: hostile-name snapshot failed to round-trip\n"); bad++;
        } else {
            const char* names[2] = { ho.trickName, ho.grindName };
            const char* sent[2]  = { hs.trickName, hs.grindName };
            for (int i = 0; i < 2; i++) {
                for (const char* p = names[i]; *p; p++) {
                    const unsigned char u = (unsigned char)*p;
                    if (u < 0x20 || u > 0x7e) {
                        printf("  codec: %s name kept a non-printable byte 0x%02x\n",
                               i ? "grind" : "trick", u); bad++; break;
                    }
                }
                if (strlen(names[i]) != strlen(sent[i])) {
                    printf("  codec: %s name length changed\n", i ? "grind" : "trick"); bad++;
                }
            }
        }
    }

    // truncation fuzz: NO strict prefix of a valid packet may parse.
    for (int L = 0; L < n; L++) { State t; if (Unpack(pkt, L, t, nullptr)) { printf("  codec: truncated packet parsed at len %d/%d\n", L, n); bad++; break; } }

    // ---- the COSMETICS message type (its own magic on the same transport).
    {
        CosmeticSet cs{};
        cs.stance = 1; cs.modDigest = 0xDEADBEEFCAFEF00Dull;
        strcpy_s(cs.skaterName, "SkaterMale01");
        strcpy_s(cs.visualDef, "SVD_Male_Default_Long_Asset_Name_Here");
        cs.nChar = 24; cs.nBoard = 16;                       // deliberately FULL: exercises the cap logic
        for (int i = 0; i < cs.nChar; i++) {
            cs.chr[i].cat = i * 7 - 3;                       // negative + sparse category keys
            cs.chr[i].inst = (uint8_t)(i * 3);
            cs.chr[i].variant = (i % 2) ? -i : i * 1000;
            snprintf(cs.chr[i].name, sizeof(cs.chr[i].name), "Item_Clothing_%02d_LongIshName", i);
        }
        for (int i = 0; i < cs.nBoard; i++) {
            cs.brd[i].cat = i; cs.brd[i].inst = 1; cs.brd[i].variant = i;
            snprintf(cs.brd[i].name, sizeof(cs.brd[i].name), "Deck_Graphic_%02d", i);
        }
        for (int i = 0; i < cs.nChar; i++) {                 // colours on every slot
            cs.chr[i].instVariant = i - 2; cs.chr[i].sockHeight = (uint8_t)(i & 3);
            cs.chr[i].nColors = 3;
            for (int k = 0; k < 3; k++) {
                snprintf(cs.chr[i].colors[k].key, sizeof(cs.chr[i].colors[k].key), "Slot_%02d_%d", i, k);
                cs.chr[i].colors[k].enabled = (uint8_t)(k & 1);
                for (int q = 0; q < 4; q++) cs.chr[i].colors[k].rgba[q] = 0.125f * (float)(k + q);
            }
        }
        cs.chr[5].name[0] = 0;                               // an empty slot must survive the round trip
        // Per-SECTION packets: the whole point is that a full wardrobe cannot let clothing starve the
        // board slots, so the gate asserts BOTH sections survive a full set.
        uint8_t cpkt[2][1000]; int cn[2] = {0,0};
        CosmeticSet merged{};
        bool cosOk = true;
        for (uint8_t sec = kCosClothing; sec <= kCosBoard; sec++) {
            cn[sec] = PackCosmetics(cs, sec, cpkt[sec], sizeof(cpkt[sec]));
            if (cn[sec] <= 0) { printf("  codec: PackCosmetics(section %d) failed\n", sec); bad++; cosOk = false; continue; }
            if (!IsCosmeticsPacket(cpkt[sec], cn[sec]) || IsCosmeticsPacket(pkt, n)) {
                printf("  codec: cosmetics/snapshot magics not distinguishable\n"); bad++; }
            CosmeticSet co; uint8_t got = 0xff;
            if (!UnpackCosmetics(cpkt[sec], cn[sec], co, &got) || got != sec) {
                printf("  codec: UnpackCosmetics(section %d) failed\n", sec); bad++; cosOk = false; continue; }
            if (co.stance != cs.stance || co.modDigest != cs.modDigest) { printf("  codec: cos scalars\n"); bad++; }
            if (strcmp(co.skaterName, cs.skaterName) || strcmp(co.visualDef, cs.visualDef)) {
                printf("  codec: cos names\n"); bad++; }
            if (sec == kCosBoard) { memcpy(merged.brd, co.brd, sizeof(merged.brd)); merged.nBoard = co.nBoard; }
            else                  { memcpy(merged.chr, co.chr, sizeof(merged.chr)); merged.nChar  = co.nChar;  }
            for (int L = 0; L < cn[sec]; L++) {
                CosmeticSet t;
                if (UnpackCosmetics(cpkt[sec], L, t, nullptr)) {
                    printf("  codec: truncated cosmetics parsed at %d/%d\n", L, cn[sec]); bad++; break; }
            }
        }
        if (cosOk) {
            // The regression this gate exists for: a single-packet encoding fits 24 clothing slots
            // and only 2 of 16 board slots, silently starving board graphics. The per-section split
            // prevents that, so the BOARD section must survive whole no matter how heavy clothing
            // gets. WITHIN a section, overflow is allowed and must be GRACEFUL: whole trailing slots
            // are dropped (a prefix) and the count written is reported. Asserting "all 24 fit" would
            // be asserting a packet SIZE, not the format's promise.
            if (merged.nBoard != cs.nBoard) {
                printf("  codec: board section starved by clothing (%d/%d)\n",
                       (int)merged.nBoard, (int)cs.nBoard); bad++; }
            if (merged.nChar > cs.nChar) { printf("  codec: clothing count grew\n"); bad++; }
            for (int i = 0; i < merged.nChar; i++) {
                if (merged.chr[i].cat != cs.chr[i].cat || merged.chr[i].inst != cs.chr[i].inst ||
                    merged.chr[i].variant != cs.chr[i].variant || strcmp(merged.chr[i].name, cs.chr[i].name)) {
                    printf("  codec: cos clothing %d\n", i); bad++; break; }
            }
            for (int i = 0; i < merged.nBoard; i++) {
                if (merged.brd[i].cat != cs.brd[i].cat || merged.brd[i].variant != cs.brd[i].variant ||
                    strcmp(merged.brd[i].name, cs.brd[i].name)) {
                    printf("  codec: cos board %d\n", i); bad++; break; }
            }
            // A snapshot packet must never decode as cosmetics, or vice versa.
            { CosmeticSet t; State t2;
              if (UnpackCosmetics(pkt, n, t, nullptr) || Unpack(cpkt[0], cn[0], t2, nullptr)) {
                  printf("  codec: cross-type decode accepted\n"); bad++; } }
            // ---- HOSTILE STRINGS. A length-bounded string is still an ARBITRARY BYTE STRING, and
            // every one of these is drawn in the world or written to the log. The gate feeds bytes a
            // legitimate sender can never produce -- controls, an ESC, a DEL, and high bytes -- and
            // asserts that nothing outside printable ASCII survives the trip.
            {
                CosmeticSet h{};
                h.stance = 1;
                // "Bad\n[mp] forged" is the attack that matters: a newline lets a peer's NAME write
                // what looks like a genuine mod log line into the log used for diagnosis.
                snprintf(h.skaterName, sizeof(h.skaterName), "Bad\n[mp] forged\r\x1b[31m\x7f\xc3\xa9");
                snprintf(h.mapName,    sizeof(h.mapName),    "Map\nname");
                snprintf(h.visualDef,  sizeof(h.visualDef),  "Vis\tdef");
                h.nChar = 1;
                h.chr[0].cat = 3; h.chr[0].inst = 1; h.chr[0].variant = 0;
                snprintf(h.chr[0].name, sizeof(h.chr[0].name), "Item\nName");
                h.chr[0].nColors = 1;
                snprintf(h.chr[0].colors[0].key, sizeof(h.chr[0].colors[0].key), "Key\x01\x02");
                h.chr[0].colors[0].enabled = 1;

                uint8_t hp[1000]; CosmeticSet ho; uint8_t hsec = 0;
                const int hn = PackCosmetics(h, kCosClothing, hp, sizeof(hp));
                if (hn <= 0 || !UnpackCosmetics(hp, hn, ho, &hsec)) {
                    printf("  codec: hostile-string cosmetics packet failed to round-trip\n"); bad++;
                } else {
                    struct { const char* what; const char* got; const char* sent; } chk[] = {
                        { "skaterName", ho.skaterName, h.skaterName },
                        { "mapName",    ho.mapName,    h.mapName    },
                        { "visualDef",  ho.visualDef,  h.visualDef  },
                        { "item name",  ho.nChar ? ho.chr[0].name : "", h.chr[0].name },
                        { "colour key", ho.nChar ? ho.chr[0].colors[0].key : "", h.chr[0].colors[0].key },
                    };
                    for (const auto& c : chk) {
                        for (const char* p = c.got; *p; p++) {
                            const unsigned char u = (unsigned char)*p;
                            if (u < 0x20 || u > 0x7e) {
                                printf("  codec: %s kept a non-printable byte 0x%02x\n", c.what, u);
                                bad++; break;
                            }
                        }
                        // Sanitising must REPLACE, not drop: a shortened string would mean the filter
                        // is silently editing content rather than neutralising it.
                        if (strlen(c.got) != strlen(c.sent)) {
                            printf("  codec: %s length changed %d -> %d\n", c.what,
                                   (int)strlen(c.sent), (int)strlen(c.got)); bad++;
                        }
                    }
                    // The legitimate leading run must be untouched -- the filter neutralises, it does
                    // not mangle names that were fine.
                    if (strncmp(ho.skaterName, "Bad", 3)) { printf("  codec: sanitiser ate clean ASCII\n"); bad++; }
                }
                // ...and a name that fills the field to the cap still arrives WHOLE; silent
                // truncation of names at the cap is exactly the gap this closes.
                CosmeticSet full{};
                for (int i = 0; i < (int)sizeof(full.skaterName) - 1; i++) full.skaterName[i] = 'A';
                uint8_t fp[1000]; CosmeticSet fo;
                const int fn = PackCosmetics(full, kCosClothing, fp, sizeof(fp));
                if (fn <= 0 || !UnpackCosmetics(fp, fn, fo, nullptr) ||
                    strcmp(fo.skaterName, full.skaterName)) {
                    printf("  codec: a name at the field cap did not survive (%d chars)\n",
                           fn > 0 ? (int)strlen(fo.skaterName) : -1); bad++;
                }
            }
            // A REALISTIC loadout must fit COMPLETELY -- truncation is the safety valve for absurd
            // input, never the shipping path. This is the case that actually protects players.
            {
                CosmeticSet real{};
                strcpy_s(real.skaterName, "SkaterMale01");
                strcpy_s(real.visualDef, "SVD_Male_Default");
                real.nChar = 8; real.nBoard = 4;
                for (int i = 0; i < real.nChar; i++) {
                    real.chr[i].cat = 1 << i; real.chr[i].nColors = 3; real.chr[i].instVariant = i;
                    snprintf(real.chr[i].name, sizeof(real.chr[i].name), "CIT_AMXX_GEN_Item_%02d", i);
                    for (int k = 0; k < 3; k++) {
                        snprintf(real.chr[i].colors[k].key, sizeof(real.chr[i].colors[k].key), "Color_%d", k);
                        for (int q = 0; q < 4; q++) real.chr[i].colors[k].rgba[q] = 0.25f * (float)(k + 1);
                    }
                }
                for (int i = 0; i < real.nBoard; i++) {
                    real.brd[i].cat = 128 << i; real.brd[i].nColors = 2;
                    snprintf(real.brd[i].name, sizeof(real.brd[i].name), "CIT_DG_Deck_%02d", i);
                }
                uint8_t rp[1000]; CosmeticSet ro; uint8_t sec2 = 0;
                for (uint8_t sec = kCosClothing; sec <= kCosBoard; sec++) {
                    const int rn = PackCosmetics(real, sec, rp, sizeof(rp));
                    if (rn <= 0 || !UnpackCosmetics(rp, rn, ro, &sec2)) {
                        printf("  codec: realistic loadout pack/unpack failed\n"); bad++; break; }
                    const int got  = (sec == kCosBoard) ? ro.nBoard   : ro.nChar;
                    const int want = (sec == kCosBoard) ? real.nBoard : real.nChar;
                    if (got != want) {
                        printf("  codec: a REALISTIC loadout did not fit (%d/%d in section %d, %d bytes)\n",
                               got, want, sec, rn); bad++; }
                    if (sec == kCosClothing && (ro.chr[0].nColors != 3 ||
                        fabsf(ro.chr[0].colors[1].rgba[0] - 0.5f) > 0.002f ||
                        ro.chr[3].instVariant != 3)) {
                        printf("  codec: colour/instance round-trip\n"); bad++; }
                }
            }
            if (!bad) printf("codec cosmetics: PASS (%d + %d bytes, %d clothing + %d board slots)\n",
                             cn[0], cn[1], (int)merged.nChar, (int)merged.nBoard);
        }
    }
    // ---- CHAT. It is the only wire field a STRANGER types, so this gate is about hostile input as
    // much as round-tripping: a truncated packet, a control character and an empty message must be
    // refused or cleaned rather than reaching a text renderer.
    {
        int cbad = 0;
        uint8_t cp[256];
        ChatMsg m; m.id = 0x12345678u;
        strcpy_s(m.name, "matsix");
        strcpy_s(m.text, "kickflip that rail, i dare you");
        const int cn = PackChat(m, cp, sizeof(cp));
        if (cn <= 0) { printf("  chat: PackChat failed\n"); cbad++; }
        else {
            if (!IsChatPacket(cp, cn))       { printf("  chat: own packet not recognised\n"); cbad++; }
            if (IsCosmeticsPacket(cp, cn))   { printf("  chat: MAGIC COLLIDES with cosmetics\n"); cbad++; }
            if (IsChatPacket(pkt, n))        { printf("  chat: a POSE packet parsed as chat\n"); cbad++; }
            ChatMsg o;
            if (!UnpackChat(cp, cn, o))      { printf("  chat: round-trip failed\n"); cbad++; }
            else if (o.id != m.id || strcmp(o.name, m.name) || strcmp(o.text, m.text)) {
                printf("  chat: round-trip mismatch ('%s' / '%s')\n", o.name, o.text); cbad++; }
            for (int k = 0; k < cn; k++) {          // every truncation REFUSED, never half-parsed
                ChatMsg t;
                if (UnpackChat(cp, k, t)) { printf("  chat: accepted a %d-byte truncation\n", k); cbad++; break; }
            }
        }
        ChatMsg ctl; ctl.id = 1;
        strcpy_s(ctl.name, "x");
        strcpy_s(ctl.text, "a\x01" "b\x1b" "c");
        const int ctn = PackChat(ctl, cp, sizeof(cp));
        ChatMsg ctlOut;
        if (ctn > 0 && UnpackChat(cp, ctn, ctlOut)) {
            for (const char* q = ctlOut.text; *q; q++)
                if ((unsigned char)*q < 0x20) { printf("  chat: a control character survived\n"); cbad++; break; }
        }
        ChatMsg empty; empty.id = 2; strcpy_s(empty.name, "x");
        if (PackChat(empty, cp, sizeof(cp)) != 0) { printf("  chat: packed an EMPTY message\n"); cbad++; }
        bad += cbad;
        if (!cbad) printf("codec chat: PASS (%d bytes; truncation, control chars, magic isolation)\n", cn);
    }
    if (!bad) printf("codec round-trip + truncation fuzz: PASS (%d bytes packed, was ~%d raw)\n", n, 363 + (int)s.animLen);
    return bad == 0;
}

// =====================================================================================================
// THE AUDIO EVENT QUEUE. A different test shape from the rest of this harness: everything else judges
// a continuous signal by its residual, but a one-shot sound is judged by IDENTITY and TIME. The three
// properties that matter, and the failure each one guards against:
//   1. EXACTLY ONCE -- events are deliberately repeated in several packets to survive loss, so a
//      receiver that plays each copy would machine-gun the sound.
//   2. NOT LOST TO A PLAYHEAD JUMP -- Sample() skips whole snapshots on resync/starve/teleport, which
//      is why events do not ride the sampled State. This proves the queue survives that case.
//   3. NEVER PLAYED EARLY -- an event must wait for the playback clock to reach the moment it fired,
//      or a peer's pop arrives ahead of the animation that caused it.
// =====================================================================================================
// =====================================================================================================
// THE PUSH-TRANSITION QUEUE. Same shape as the audio gate, one property stronger: pushes are a STATE
// MACHINE, so ORDER is part of correctness, not just identity and time.
// The failure it guards: pushing is a TAP (faster tapping = more pushes/second) and the receiver's
// SetPushState is edge-triggered, so a transition that lives entirely between two snapshots is lost
// forever if the edge is read from a SAMPLED State. The test therefore taps FASTER than the publish
// rate, which a sampled path cannot represent at all.
// =====================================================================================================
static bool pushQueueCheck() {
    int bad = 0;
    Stream st{};
    State s{};
    s.deckQuat[3] = 1.f; s.bodyPosOk = 1;
    // 60 Hz publish, a push cycle every OTHER packet: 0,1,0,1,... i.e. a transition every 16.7 ms.
    // Sampling at a receiver that is a whole buffer-delay behind cannot see these as level changes.
    // Push and Sample must be INTERLEAVED, as the session does it: the stream maps a local clock to a
    // sender clock, so filing every packet first and only then sampling presents a local time that
    // has run backwards past every packet, and everything reads as lost. Draining every frame also
    // keeps the queue well inside its 12 slots, which is the load the shipping code sees.
    const int kTaps = 40;
    uint8_t expect[kTaps * 2]; int nExpect = 0;
    uint8_t got[kTaps * 4];    int nGot = 0;
    uint64_t now = 1000000ull;
    auto drain = [&]() {
        uint8_t b[8];
        const int n = st.DrainPushStates(b, 8);
        for (int i = 0; i < n && nGot < (int)sizeof(got); i++) got[nGot++] = b[i];
    };
    for (int i = 0; i < kTaps * 2; i++) {
        s.pushState = (uint8_t)(i & 1);                  // a transition every 16.7 ms
        if (nExpect == 0 || expect[nExpect - 1] != s.pushState) expect[nExpect++] = s.pushState;
        st.Push(s, now, now);
        State out; st.Sample(now, out);
        drain();
        now += 16700ull;
    }
    // Let the playback clock run past the tail -- it trails by the jitter buffer, so the last few
    // transitions are still queued when the packets stop.
    for (int f = 0; f < 60; f++) { State out; st.Sample(now, out); drain(); now += 8000ull; }
    // ORDER + COMPLETENESS. Every transition the sender made, in the order it made them. A gate that
    // only counted them would pass on a queue that shuffled the state machine.
    if (nGot < nExpect) {
        printf("  push: only %d of %d transitions arrived (fast taps are being lost)\n", nGot, nExpect);
        bad++;
    }
    const int cmp = nGot < nExpect ? nGot : nExpect;
    for (int i = 0; i < cmp; i++) {
        if (got[i] != expect[i]) {
            printf("  push: transition %d was %u, expected %u (queue reordered the state machine)\n",
                   i, got[i], expect[i]);
            bad++; break;
        }
    }
    // NEVER EARLY: nothing may be released before the playback clock reaches it. A fresh stream whose
    // playhead has not started must hand back nothing at all.
    {
        Stream st2{}; State s2{}; s2.deckQuat[3] = 1.f; s2.bodyPosOk = 1;
        s2.pushState = 1; st2.Push(s2, 5000000ull, 5000000ull);
        uint8_t b[4];
        if (st2.DrainPushStates(b, 4) != 0) { printf("  push: released with no playback clock\n"); bad++; }
    }
    // A REPEAT of the state already held is not an edge and must not be filed -- otherwise a steady
    // 60 Hz stream of "still pushing" would fire a push every packet.
    {
        Stream st3{}; State s3{}; s3.deckQuat[3] = 1.f; s3.bodyPosOk = 1;
        uint64_t t = 1000000ull;
        int total = 0;
        s3.pushState = 1;
        for (int i = 0; i < 30; i++) {
            st3.Push(s3, t, t);
            State o; st3.Sample(t, o);
            uint8_t b[8]; total += st3.DrainPushStates(b, 8);
            t += 16700ull;
        }
        for (int f = 0; f < 60; f++) {
            State o; st3.Sample(t, o);
            uint8_t b[8]; total += st3.DrainPushStates(b, 8);
            t += 8000ull;
        }
        if (total != 1) { printf("  push: a held state fired %d times, expected 1\n", total); bad++; }
    }
    if (!bad) printf("push transitions: PASS (%d fast taps, order preserved, none early)\n", nExpect);
    return bad == 0;
}

static bool audioEventCheck() {
    int bad = 0;
    // --- 1 + 2: repeats through a lossy wire. Each of 40 events is sent in 4 consecutive packets.
    // The two properties are NOT equally strong, and the gate states which is which:
    //   * EXACTLY-ONCE is absolute. It is a property of the id ring and holds at ANY loss rate.
    //   * DELIVERY is best-effort by construction -- 4 repeats through p loss leaves p^4. At 5% that
    //     is 6 in a million, so perfection is demanded. At 60% it is 13%, where demanding perfection
    //     would assert a coin flip rather than a behaviour. Assert the mechanism, not a lucky number.
    auto lossRun = [&](int lossPct, int demandDeliveredPct, const char* label) {
        Stream st{};
        State s{};
        s.bodyPosOk = 1; s.deckQuat[3] = 1.f;
        std::mt19937 rng(99);
        int played[64] = {};
        const uint64_t t0 = 1000000ull;
        int pending[8] = {}, repeats[8] = {}, nPend = 0, nextId = 1;
        // 60 tail frames with no new events: the playback clock runs a buffer-delay BEHIND the
        // sender, so the last events are still in flight when the sending stops. A test that stopped
        // with the sender would score its own impatience as packet loss.
        for (int frame = 0; frame < 260; frame++) {
            const uint64_t now = t0 + (uint64_t)frame * 16667ull;
            if (frame < 200 && frame % 5 == 0 && nextId <= 40 && nPend < 8) {
                pending[nPend] = nextId++; repeats[nPend] = 4; nPend++;
            }
            s.nEvents = 0;
            for (int i = 0; i < nPend && s.nEvents < kAudioMaxEvents; i++) {
                if (repeats[i] <= 0) continue;
                AudioEvent& e = s.events[s.nEvents++];
                e = AudioEvent{};
                e.id = (uint16_t)pending[i];
                strcpy_s(e.cue, "SC_Test");
                e.ageMs = (uint16_t)((4 - repeats[i]) * 17);   // it fired when the FIRST copy went out
                repeats[i]--;
            }
            int k = 0;
            for (int i = 0; i < nPend; i++) if (repeats[i] > 0) { pending[k] = pending[i]; repeats[k] = repeats[i]; k++; }
            nPend = k;
            if ((int)(rng() % 100) >= lossPct) st.Push(s, now, now);
            State out;
            st.Sample(now, out);
            AudioEvent got[8];
            const int n = st.DrainAudio(got, 8);
            for (int i = 0; i < n; i++) if (got[i].id < 64) played[got[i].id]++;
        }
        int delivered = 0, dupes = 0;
        for (int id = 1; id <= 40; id++) {
            if (played[id] == 1) delivered++;
            else if (played[id] > 1) dupes++;
        }
        // EXACTLY-ONCE: absolute, at any loss rate.
        if (dupes) { printf("  audio [%s]: %d event(s) played MORE THAN ONCE\n", label, dupes); bad++; }
        const int pct = delivered * 100 / 40;
        if (pct < demandDeliveredPct) {
            printf("  audio [%s]: only %d%% delivered (expected >= %d%%)\n", label, pct, demandDeliveredPct);
            bad++;
        }
    };
    lossRun(5,  100, "5% loss");     // realistic: 4 repeats make loss vanish
    lossRun(60,  75, "60% loss");    // extreme: exactly-once still absolute, delivery degrades gracefully
    // --- 3: an event must not fire before the playback clock reaches it. Push one stamped in the
    // sender's FUTURE relative to where the playhead will be, and demand silence.
    {
        Stream st{};
        State s{};
        s.bodyPosOk = 1; s.deckQuat[3] = 1.f;
        const uint64_t t0 = 5000000ull;
        for (int frame = 0; frame < 6; frame++) {              // prime the clock with plain snapshots
            const uint64_t now = t0 + (uint64_t)frame * 16667ull;
            s.nEvents = 0;
            st.Push(s, now, now);
            State out; st.Sample(now, out);
        }
        s.nEvents = 1;
        s.events[0] = AudioEvent{};
        s.events[0].id = 777; strcpy_s(s.events[0].cue, "SC_Late"); s.events[0].ageMs = 0;
        const uint64_t tNow = t0 + 6 * 16667ull;
        st.Push(s, tNow, tNow);
        AudioEvent got[4];
        State out; st.Sample(tNow, out);                        // playhead sits BEHIND by the buffer delay
        if (st.DrainAudio(got, 4) != 0) { printf("  audio: event fired BEFORE its playback time\n"); bad++; }
        // ...and it must arrive once the clock gets there.
        int seen = 0;
        for (int frame = 7; frame < 40 && !seen; frame++) {
            const uint64_t now = t0 + (uint64_t)frame * 16667ull;
            s.nEvents = 0; st.Push(s, now, now);
            State o2; st.Sample(now, o2);
            seen += st.DrainAudio(got, 4);
        }
        if (!seen) { printf("  audio: event never fired at all\n"); bad++; }
    }
    printf("audio one-shot queue (loss + repeats + ordering): %s\n", bad ? "FAIL" : "PASS");
    return bad == 0;
}


// ---- EXTRAPOLATION COHERENCE. When the sender goes quiet the stream projects position forward, and
// every world-space part of the skater must move by the SAME displacement. The deck and body always
// did; the world-space IK targets did not, so a projected ollie rendered as the body rising out of
// its own feet -- the limbs were not lagging, they were anchored to a position the body had left.
// The invariant is rigid: foot-to-deck and hand-to-deck offsets are whatever was last RECEIVED, and
// extrapolation may not change them by so much as a millimetre.
static bool extrapCoherenceCheck() {
    Stream st;
    State a{}; State b{};
    auto fill = [](State& s, float x) {
        s.bodyPosOk = 1; s.feetOk = 1; s.feetWorld = 1; s.handOk = 1; s.handWorld = 1;
        s.deckPos[0] = x;        s.deckPos[1] = 0; s.deckPos[2] = 0;
        s.bodyPos[0] = x;        s.bodyPos[1] = 0; s.bodyPos[2] = 90.f;
        s.lFootPos[0] = x - 15;  s.lFootPos[1] = 0; s.lFootPos[2] = 5.f;
        s.rFootPos[0] = x + 15;  s.rFootPos[1] = 0; s.rFootPos[2] = 5.f;
        s.lHandPos[0] = x - 25;  s.lHandPos[1] = 0; s.lHandPos[2] = 60.f;
        s.rHandPos[0] = x + 25;  s.rHandPos[1] = 0; s.rHandPos[2] = 60.f;
        s.deckQuat[3] = 1; s.bodyQuat[3] = 1;
    };
    fill(a, 0.f); fill(b, 100.f);                    // 100 cm in 100 ms = 10 m/s, a real skating speed
    st.Push(a,      0, 0);
    st.Push(b, 100000, 100000);

    // Sample far enough past the newest packet that the stream must project rather than interpolate.
    State out{};
    bool sawExtrap = false;
    for (int i = 0; i < 400 && !sawExtrap; i++) {
        st.Sample(200000 + (uint64_t)i * 5000, out);
        sawExtrap = st.stats().extrap > 0;
    }
    if (!sawExtrap) { printf("  extrap coherence: stream never extrapolated -- test is not exercising it\n"); return false; }

    struct Pair { const char* what; const float* p; };
    const Pair parts[] = {
        { "bodyPos", out.bodyPos }, { "lFootPos", out.lFootPos }, { "rFootPos", out.rFootPos },
        { "lHandPos", out.lHandPos }, { "rHandPos", out.rHandPos },
    };
    const Pair truth[] = {
        { "bodyPos", b.bodyPos }, { "lFootPos", b.lFootPos }, { "rFootPos", b.rFootPos },
        { "lHandPos", b.lHandPos }, { "rHandPos", b.rHandPos },
    };
    int bad = 0;
    for (int k = 0; k < 5; k++) {
        for (int i = 0; i < 3; i++) {
            const float wantOff = truth[k].p[i] - b.deckPos[i];   // offset as last RECEIVED
            const float gotOff  = parts[k].p[i] - out.deckPos[i]; // offset after projection
            if (fabsf(gotOff - wantOff) > 0.01f) {
                printf("  extrap coherence: %s[%d] offset drifted %.2f cm from the deck "
                       "(extrapolation moved the deck without it)\n",
                       parts[k].what, i, (double)(gotOff - wantOff));
                bad++;
            }
        }
    }
    if (!bad) printf("  extrap coherence: deck, body, feet and hands project as one rigid body  PASS\n");
    return bad == 0;
}

// ---- ANIM-BLOB INTERPOLATION. Continuous anim fields (floats and FVector deltas) must ride the
// same interpolation t as the positions; a blob stepped whole from the older snapshot renders the
// body rising into a pop before the pose plays it. Bools must keep STEPPING -- a lerped enum is a
// corrupted enum.
static bool animLerpCheck() {
    Stream st;
    State a{}; State b{};
    a.deckQuat[3] = a.bodyQuat[3] = 1; b.deckQuat[3] = b.bodyQuat[3] = 1;
    a.bodyPosOk = b.bodyPosOk = 1;
    // A real-shaped blob, filled to the table's full length. The first field is a float (blob
    // position 0); the first size-1 field proves stepping.
    int total = 0, boolPos = -1;
    for (int i = 0; i < AnimFieldCount(); i++) {
        const AnimField& f = AnimFieldAt(i);
        if (boolPos < 0 && f.size == 1) boolPos = total;
        total += f.size;
    }
    a.animLen = b.animLen = (uint16_t)total;
    // A steady 10 Hz stream so the playback clock locks and the playhead actually lands between
    // snapshots. The float field carries the SNAPSHOT INDEX, so a fractional read proves
    // interpolation and its integer part names the bracket; the bool flips with index parity, so
    // the stepped expectation is decidable from the same read.
    // Interleave pushes and samples the way live operation does, so the playback clock locks and
    // the playhead genuinely lands between snapshots.
    State out{};
    float got = -1, frac = 0;
    bool found = false;
    int pushed = 0;
    for (uint64_t us = 0; us < 3000000 && !found; us += 2000) {
        while (pushed < 20 && (uint64_t)pushed * 100000 <= us) {
            State s2 = a;
            const float v = (float)pushed;
            memcpy(s2.anim, &v, 4);
            s2.anim[boolPos] = (uint8_t)(pushed & 1);
            st.Push(s2, (uint64_t)pushed * 100000, (uint64_t)pushed * 100000);
            pushed++;
        }
        if (!st.Sample(us, out)) continue;
        memcpy(&got, out.anim, 4);
        frac = got - (float)(int)got;
        if (got > 0.5f && frac > 0.25f && frac < 0.75f) found = true;
    }
    bool ok = true;
    if (!found) {
        printf("  anim lerp: float field never read fractional (last %.3f) -- the blob is not "
               "riding the interpolation\n", (double)got);
        ok = false;
    } else if (out.anim[boolPos] != (uint8_t)((int)got & 1)) {
        printf("  anim lerp: BOOL field failed to step (lerped enums are corrupted enums)\n");
        ok = false;
    }
    if (ok) printf("  anim lerp: floats ride the position timeline (read %.2f), bools step  PASS\n",
                   (double)got);
    return ok;
}


// ==== REPLAY SYNC TRANSFER =========================================================================
// The whole protocol, offline: an "owner" ring of packed snapshots is requested by a "requester"
// over a loopback send that DROPS a deterministic slice of chunks; the NAK loop must recover, the
// blob must validate, and SampleAt must reproduce the original states exactly at their own
// timestamps and interpolate between them. Fails loudly at each stage.
static int  g_syncDropMod = 7;                  // every 7th chunk is dropped on first send
static int  g_syncChunksSeen = 0;
static bool g_syncLoopback = true;
static void syncLoopSend(int peerIdx, const void* data, int len, bool) {
    if (!g_syncLoopback) return;
    // Chunks are type 3 at byte 4; drop a deterministic slice on FIRST delivery only. Resends are
    // recognizable because the drop counter keeps advancing -- every index eventually gets through.
    const uint8_t* d = (const uint8_t*)data;
    if (len > 4 && d[4] == 3) {
        g_syncChunksSeen++;
        if (g_syncDropMod > 0 && (g_syncChunksSeen % g_syncDropMod) == 0) return;   // lost
    }
    // deliver to the OTHER end: both ends live in this process, peer index is just echoed back
    omp::replaysync::OnPacket(peerIdx, d, len, 0, nullptr);
}

static bool syncTransferCheck() {
    using namespace omp::replaysync;
    SetSendFn(&syncLoopSend);
    // ---- the owner's ring: 600 packed snapshots at 60 Hz, deck X = the snapshot index
    State src{}; src.deckQuat[3] = src.bodyQuat[3] = 1; src.bodyPosOk = 1;
    uint8_t pkt[1024];
    const int N = 600;
    for (int i = 0; i < N; i++) {
        src.deckPos[0] = (float)i;
        src.bodyPos[0] = (float)i * 2;
        const uint64_t us = 1000000ull + (uint64_t)i * 16667;
        const int n = Pack(src, us, pkt, sizeof(pkt));
        if (n <= 0) { printf("  sync: Pack failed at %d\n", i); return false; }
        RecordOwn(pkt, n, us);
    }
    // ---- request as "peer 3" and pump both ends until Ready (Tick paces chunks + NAKs)
    g_syncChunksSeen = 0;
    if (!RequestSync(3, 5000000ull)) { printf("  sync: RequestSync refused\n"); return false; }
    uint64_t us = 5000000ull;
    SyncState st = SyncState::None;
    for (int it = 0; it < 20000; it++) {
        Tick(us, nullptr);
        us += 16000;
        st = PeerSyncState(3, nullptr);
        if (st == SyncState::Ready || st == SyncState::Failed) break;
    }
    if (st != SyncState::Ready) {
        printf("  sync: transfer never completed (state %d, %d chunk sends)\n", (int)st, g_syncChunksSeen);
        return false;
    }
    // ---- exact reproduction at an entry's own timestamp, interpolation between entries
    const uint64_t oldest = BufferOldestUs(3), newest = BufferNewestUs(3);
    if (!(oldest == 1000000ull && newest == 1000000ull + (uint64_t)(N - 1) * 16667)) {
        printf("  sync: window wrong (%llu..%llu)\n",
               (unsigned long long)oldest, (unsigned long long)newest);
        return false;
    }
    State out{};
    if (!SampleAt(3, 1000000ull + 100 * 16667, out) || out.deckPos[0] != 100.f) {
        printf("  sync: exact sample wrong (deckX %.3f, want 100)\n", (double)out.deckPos[0]);
        return false;
    }
    if (!SampleAt(3, 1000000ull + 100 * 16667 + 8333, out) ||
        out.deckPos[0] < 100.4f || out.deckPos[0] > 100.6f ||
        out.bodyPos[0] < 200.8f || out.bodyPos[0] > 201.2f) {
        printf("  sync: midpoint sample wrong (deckX %.3f want ~100.5, bodyX %.3f want ~201)\n",
               (double)out.deckPos[0], (double)out.bodyPos[0]);
        return false;
    }
    // clamped ends: before the window holds the oldest state
    if (!SampleAt(3, 0, out) || out.deckPos[0] != 0.f) {
        printf("  sync: pre-window clamp wrong (deckX %.3f, want 0)\n", (double)out.deckPos[0]);
        return false;
    }
    DropAll();
    SetSendFn(nullptr);
    printf("  sync transfer: %d chunk sends incl. drops+resends, window+exact+lerp+clamp  PASS\n",
           g_syncChunksSeen);
    return true;
}

int main(int argc, char**) {
    const bool dbg = argc > 1;                      // any arg = per-second clock internals, clean profile
    printf("SessionOpenMP replication loop test\n");
    if (!codecCheck()) { printf("\nCODEC FAIL\n"); return 1; }
    if (!audioEventCheck()) { printf("\nAUDIO EVENT FAIL\n"); return 1; }
    if (!pushQueueCheck())  { printf("\nPUSH QUEUE FAIL\n");  return 1; }
    if (!extrapCoherenceCheck()) { printf("\nEXTRAP COHERENCE FAIL\n"); return 1; }
    if (!animLerpCheck()) { printf("\nANIM LERP FAIL\n"); return 1; }
    if (!syncTransferCheck()) { printf("\nSYNC TRANSFER FAIL\n"); return 1; }
    printf("%-13s %8s %8s %8s %6s %7s %7s %7s %7s\n",
           "profile", "outEwma", "outMax", "delay", "alpha", "starve", "resync", "extrap", "verdict");
    bool allPass = true;

    for (const Profile& pf : kProfiles) {
        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> jit(-pf.jitMs, pf.jitMs);
        std::uniform_int_distribution<int> loss(0, 99);
        Stream::Tuning tun{};                       // library defaults -- the test judges THOSE
        Stream stream(tun);
        std::vector<Delivery> wire;

        // sender state: circle r=500cm at ~800cm/s; every 3s a 0.6s "trick" spins the deck 500 deg/s
        double sendClock = 0, recvClock = 1000.0;   // absurd epoch offset on purpose: epochs never compare
        double ewma = 0, mx = 0; int ewmaN = 0;
        float prev[2][3]; double prevT[2]; int prevN = 0;
        uint64_t nanFail = 0;

        const double dtSend = 1.0 / 60.0, dtRecv = 1.0 / 59.3;  // deliberately unequal rates
        double nextSend = 0, nextRecv = 0.0043;                 // and phases
        double burstUntil = -1, nextBurst = pf.burstEveryS > 0 ? pf.burstEveryS : 1e18;

        for (double now = 0; now < 30.0; now += 0.0005) {
            if (now >= nextSend) {
                const double sendT = nextSend;                  // stamp the SCHEDULED time: the 0.5 ms sim
                nextSend += dtSend;                             // grid otherwise aliases the cadence EWMA
                const double a = 800.0 / 500.0 * sendT;         // omega = v/r
                State s{};
                s.deckPos[0] = 500.f * (float)cos(a); s.deckPos[1] = 500.f * (float)sin(a); s.deckPos[2] = -52.f;
                const double trickPhase = fmod(sendT, 3.0);
                const double roll = (trickPhase < 0.6) ? trickPhase * 500.0 : 0.0;   // the flip
                const float yaw = (float)(a * 57.29578 + 90.0);
                const float cr = (float)cos(roll * 0.00872665), sr = (float)sin(roll * 0.00872665);
                const float cy = (float)cos(yaw * 0.00872665),  sy = (float)sin(yaw * 0.00872665);
                s.deckQuat[0] = sr * cy; s.deckQuat[1] = sr * sy; s.deckQuat[2] = cr * sy; s.deckQuat[3] = cr * cy;
                { float n = 0; for (int i = 0; i < 4; i++) n += s.deckQuat[i]*s.deckQuat[i];
                  n = 1.f / sqrtf(n); for (int i = 0; i < 4; i++) s.deckQuat[i] *= n; }
                s.bodyPosOk = 1; memcpy(s.bodyPos, s.deckPos, 12); s.bodyPos[2] += 92;
                s.bodyRotOk = 1; s.bodyYaw = yaw;
                s.onBoard = 1; s.animLen = 229;                 // realistic payload size
                uint8_t pkt[900];
                const int n = Pack(s, (uint64_t)((sendClock + sendT) * 1e6), pkt, sizeof(pkt));
                // the wire
                if (now >= nextBurst) { burstUntil = now + pf.burstHoldMs / 1000.0; nextBurst += pf.burstEveryS; }
                const bool held = now < burstUntil;
                if (pf.lossPct == 0 || loss(rng) >= pf.lossPct) {
                    const double due = (held ? burstUntil : now) + (pf.latMs + jit(rng)) / 1000.0;
                    wire.push_back({ due, std::vector<uint8_t>(pkt, pkt + n) });
                }
            }
            for (auto& d : wire) {
                if (d.dueS >= 0 && now >= d.dueS) {
                    State s; uint64_t su = 0;
                    if (Unpack(d.bytes.data(), (int)d.bytes.size(), s, &su))
                        stream.Push(s, su, (uint64_t)((recvClock + now) * 1e6));
                    d.dueS = -1;
                }
            }
            static double nextDbg = 1.0;
            if (dbg && &pf == &kProfiles[0] && now >= nextDbg) {
                nextDbg += 1.0;
                const Stream::Stats d = stream.stats();
                printf("  t=%4.1fs lead=%+7.2fms delay=%5.1fms alpha=%4.2f rate=%.3f..%.3f starve=%u gapAvg=%.2f\n",
                       now, d.leadMs, d.delayMs, d.alpha, d.rateMin, d.rateMax, d.starved, d.gapAvgMs);
            }
            if (now >= nextRecv) {
                nextRecv += dtRecv;
                State out;
                if (stream.Sample((uint64_t)((recvClock + now) * 1e6), out)) {
                    for (int i = 0; i < 3; i++) if (!std::isfinite(out.deckPos[i])) nanFail++;
                    if (prevN >= 2) {
                        const double r = residual(prev[0], prevT[0], prev[1], prevT[1], out.deckPos, now);
                        if (r >= 0) { ewma += (r - ewma) * 0.05; if (ewmaN > 60 && r > mx) mx = r; ewmaN++; }
                        if (dbg && r > 20 && ewmaN > 60) {
                            const Stream::Stats d = stream.stats();
                            printf("  [%s] JUMP %6.1fcm t=%7.4fs out=(%7.1f,%7.1f) prev=(%7.1f,%7.1f) "
                                   "lead=%+7.1fms delay=%5.1fms alpha=%4.2f starve=%u resync=%u\n",
                                   pf.name, r, now, out.deckPos[0], out.deckPos[1], prev[1][0], prev[1][1],
                                   d.leadMs, d.delayMs, d.alpha, d.starved, d.resyncs);
                        }
                    }
                    memcpy(prev[0], prev[1], 12); prevT[0] = prevT[1];
                    memcpy(prev[1], out.deckPos, 12); prevT[1] = now;
                    if (prevN < 2) prevN++;
                }
            }
        }
        const Stream::Stats st = stream.stats();
        // Verdicts: real motion at these speeds contributes ~0.1 cm to the residual; the thresholds are
        // generous multiples so only a real mechanism failure (clock wobble, double-writer, unclamped
        // extrapolation) trips them. Bursty is allowed its holds -- that IS the designed behaviour.
        const double maxEwma = (pf.burstEveryS > 0) ? 3.0 : (pf.lossPct > 0 ? 1.5 : 0.8);
        const double maxPeak = (pf.burstEveryS > 0) ? 40.0 : (pf.lossPct > 0 ? 25.0 : 8.0);
        const bool pass = nanFail == 0 && ewma < maxEwma && mx < maxPeak && st.resyncs <= 2;
        allPass = allPass && pass;
        printf("%-13s %7.2fcm %7.2fcm %7.1fms %6.2f %7u %7u %7u   %s\n",
               pf.name, ewma, mx, st.delayMs, st.alpha, st.starved, st.resyncs, st.extrap,
               pass ? "PASS" : "*** FAIL ***");
    }
    printf(allPass ? "\nALL PROFILES PASS\n" : "\n*** FAILURES -- the clock or interpolation regressed ***\n");
    return allPass ? 0 : 1;
}
