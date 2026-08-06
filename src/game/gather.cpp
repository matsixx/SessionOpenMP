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
// SessionOpenMP -- read the local skater's state for publication.
// PROVENANCE OF THE OFFSETS (this matters, because a wrong one here is silent): the shipping PDB gives
// function addresses and member NAMES but NOT layouts (`TI_GET_OFFSET` fails), so offsets come from
// disassembling small accessors, which is exact:
//     ASkaterCharacterBase::GetSkateboard  -> `mov rax,[rcx+0x568]`   => kSkaterBoard
//     ASkateboardEx::IsRagDoll             -> `movzx eax,[rcx+0x170]` (board's own ragdoll bit)
// The ONE thing this file must never do is derive anything. It reads what the game already computed.
#include "gather.h"
#include "../debug.h"
#include "game_syms.h"
#include "pose.h"
#include "../replication/anim_fields.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game {

static void (*g_logf)(const char*) = nullptr;
void SetGatherLog(void (*logf)(const char*)) { g_logf = logf; }

#ifdef _WIN32
static void* rdPtr(void* b, int o) {
    if (!b) return nullptr;
    __try { return *(void**)((uint8_t*)b + o); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int rdByte(void* b, int o) {
    if (!b) return -1;
    __try { return *(uint8_t*)((uint8_t*)b + o); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static bool rd(void* p, void* dst, int n) {
    if (!p) return false;
    __try { memcpy(dst, p, (size_t)n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
static void* rdPtr(void*, int) { return nullptr; }
static int   rdByte(void*, int) { return -1; }
static bool  rd(void*, void*, int) { return false; }
#endif

static void qConj(const float* q, float* o) { o[0]=-q[0]; o[1]=-q[1]; o[2]=-q[2]; o[3]=q[3]; }
static void qMul(const float* a, const float* b, float* o) {
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static void quatToRot3(const float* q, float* r) {
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float sing=w*y-x*z, r2d=57.2957795f;
    if (fabsf(sing) > 0.49999f) { r[0]=sing>0?90.f:-90.f; r[1]=2.f*atan2f(z,w)*r2d*(sing>0?1.f:-1.f); r[2]=0; return; }
    r[0]=asinf(2.f*sing)*r2d;
    r[1]=atan2f(2.f*(w*z+x*y),1.f-2.f*(y*y+z*z))*r2d;
    r[2]=atan2f(2.f*(w*x+y*z),1.f-2.f*(x*x+z*z))*r2d;
}

bool GatherOwnState(void* pawn, repl::State& out) {
    if (!pawn) return false;
    out = repl::State{};

    // ---- body: the skater ACTOR's root component (capsule). Position AND rotation, both published --
    // the receiver writes them directly, so nothing about the remote body is ever derived locally.
    void* root = rdPtr(pawn, off::kActorRootComp);
    if (!root) return false;
    float rq[4];
    if (rd((uint8_t*)root + off::kCompPos, out.bodyPos, 12)) out.bodyPosOk = 1;
    if (rd((uint8_t*)root + off::kCompQuat, rq, 16)) {
        // The RAW capsule quat is what travels and what the receiver applies: the euler round-trip is
        // not self-inverse at yaw ~ +/-180 and flips the skater upside down. The euler triple is DEBUG
        // data only; pitch/roll carry the slope lean, which lives in the CAPSULE, not in a
        // mesh-relative offset.
        memcpy(out.bodyQuat, rq, 16);
        float r3[3]; quatToRot3(rq, r3);
        out.bodyPitch = r3[0]; out.bodyYaw = r3[1]; out.bodyRoll = r3[2];
        out.bodyRotOk = 1;
    } else return false;

    // ---- mesh rotation RELATIVE to the capsule: meshRel = conj(root) * mesh.
    // Relative, never world: a world write on the receiver overrides the body's own facing and drags
    // whatever is attached to the mesh.
    void* mesh = rdPtr(pawn, off::kSkaterMesh);
    if (mesh) {
        float mq[4], cj[4];
        if (rd((uint8_t*)mesh + off::kCompQuat, mq, 16)) {
            qConj(rq, cj); qMul(cj, mq, out.meshQuat); out.meshOk = 1;
        }
    }

    // ---- on-board + grounded: the two state bits the receiver's board logic gates on.
    void* mv = rdPtr(pawn, off::kSkaterMoveComp);
    out.onBoard = (mv && rdByte(mv, off::kMoveOnBoard) > 0) ? 1 : 0;
    { const int pf = mv ? rdByte(mv, off::kMovePushFlags) : -1;   // the push-machinery byte
      out.pushFlags = (pf >= 0) ? (uint8_t)pf : 0; }
    { const int ps = rdByte(pawn, off::kSkaterPushState);          // FSkateboardingAnimParams::PushState
      out.pushState = (ps >= 0) ? (uint8_t)ps : 0; }
    // PushSpeedMultiplier: how fast the player is pushing, which the anim blueprint reads off the
    // character. Defaults to 1.0 on an unreadable pawn -- the animation's own neutral rate.
    { float m = 1.0f;
      if (!rd((uint8_t*)pawn + off::kSkaterPushSpeedMul, &m, 4) || !(m > 0.01f && m < 8.f)) m = 1.0f;
      out.pushSpeed = m; }
    { const int b1 = rdByte(pawn, off::kSkaterBrake1);             // brake flags
      const int b2 = rdByte(pawn, off::kSkaterBrake2);
      out.brakeState = (uint8_t)(((b1 > 0) ? 1 : 0) | ((b2 > 0) ? 2 : 0)); }
    // ragdoll: bit 0x02 of the skater's bitfield byte -- MASKED, never assigned (the same byte carries
    // _isPopPending at bit 2).
    const int f710 = rdByte(pawn, off::kSkaterFlags710);
    out.bailing = (f710 > 0 && (f710 & 0x02)) ? 1 : 0;

    // ---- the board (deck) pose, if we are on one. `GetSkateboard` = [pawn+0x568], PDB-confirmed.
    void* bd = rdPtr(pawn, off::kSkaterBoard);
    if (bd) {
        // The board's movement MODE lets the receiver stamp instead of drive while the board is still
        // hand-carried (mode 9) even though bOnBoard has already flipped for the mount. Read through
        // the game's OWN accessor: a raw offset taken from SetMovementMode's store is a guess about
        // where the value lives, not the value the consumers actually read.
        { const int bm = BoardMovementMode(bd);
          out.boardMode = (bm >= 0) ? (uint8_t)bm : 0; }
        // The broken-state byte, raw. 0 = intact; unreadable defaults to intact (a wrong "broken"
        // would break every peer's view of this board, a wrong "intact" just misses a break).
        { const int bs = rdByte(bd, off::kBoardBrokenState);
          out.brokenState = (bs > 0) ? (uint8_t)bs : 0; }
        void* deck = rdPtr(bd, off::kBoardFlipper);          // the deck you can SEE
        if (!deck) deck = rdPtr(bd, off::kBoardTruckBack);
        if (!deck) deck = rdPtr(bd, off::kActorRootComp);
        if (deck) {
            rd((uint8_t*)deck + off::kCompPos,  out.deckPos,  12);
            rd((uint8_t*)deck + off::kCompQuat, out.deckQuat, 16);
            // ---- and the deck IN THE SENDER'S OWN BODY FRAME. The receiver hangs the board off the
            // rider rather than off a world point, so any phase error between the body and board
            // channels shows as `v_rel * tau` (~0 while riding) instead of `v_body * tau`, which is
            // visible the whole time.
            if (out.meshOk && mesh) {
                float mp[3], mqw[4];
                if (rd((uint8_t*)mesh + off::kCompPos, mp, 12) && rd((uint8_t*)mesh + off::kCompQuat, mqw, 16)) {
                    const float d[3] = { out.deckPos[0]-mp[0], out.deckPos[1]-mp[1], out.deckPos[2]-mp[2] };
                    float cj[4]; qConj(mqw, cj);
                    // rel = conj(qMesh) * (world - posMesh)
                    const float x=cj[0],y=cj[1],z=cj[2],w=cj[3];
                    const float tx=2*(y*d[2]-z*d[1]), ty=2*(z*d[0]-x*d[2]), tz=2*(x*d[1]-y*d[0]);
                    out.relPos[0]=d[0]+w*tx+(y*tz-z*ty);
                    out.relPos[1]=d[1]+w*ty+(z*tx-x*tz);
                    out.relPos[2]=d[2]+w*tz+(x*ty-y*tx);
                    out.relOk = 1;
                }
            }
        }
    }

    // ---- grounded: gates the receiver's velocity drive (drive on the ground, stamp in the air).
    // Published as its OWN field: the anim blob is a packed field sequence, so it must never be indexed
    // by a struct offset. Default 1 (grounded) when unreadable -- the safe direction, since a false
    // "airborne" would silently disable board collision.
    out.grounded = 1;
    // The anim instance is mesh + kMeshAnimInstance (0x6b0, USkeletalMeshComponent::AnimScriptInstance).
    // Not mesh+0x460: that is LeftHandIKLocation INSIDE the instance, and reads as a garbage pointer.
    void* anim = mesh ? rdPtr(mesh, off::kMeshAnimInstance) : nullptr;
    if (anim) {
        const int g = rdByte(anim, off::kAnimIsGrounded);
        if (g >= 0) out.grounded = (uint8_t)(g ? 1 : 0);
        // ---- the FOOT SOCKETS, RAW WORLD: the proxy stands at the sender's exact world position (no
        // net smoothing in overlay mode), so no rebasing and ZERO rotator conversions -- the receiver
        // writes these bytes straight back into its instance's same fields.
        if (rd((uint8_t*)anim + off::kAnimLFootLoc, out.lFootPos, 12) &&
            rd((uint8_t*)anim + off::kAnimLFootRot, out.lFootRot, 12) &&
            rd((uint8_t*)anim + off::kAnimRFootLoc, out.rFootPos, 12) &&
            rd((uint8_t*)anim + off::kAnimRFootRot, out.rFootRot, 12)) {
            out.feetOk = 1; out.feetWorld = 1;
        }
        // ---- the HAND IK TARGETS, same rules as the feet. The alphas that blend the arms toward these
        // travel in the pose blob; without the targets the proxy blends toward its own locally-invented
        // ones and the arms lag going into a pop.
        if (rd((uint8_t*)anim + off::kAnimLHandLoc, out.lHandPos, 12) &&
            rd((uint8_t*)anim + off::kAnimLHandRot, out.lHandRot, 12) &&
            rd((uint8_t*)anim + off::kAnimRHandLoc, out.rHandPos, 12) &&
            rd((uint8_t*)anim + off::kAnimRHandRot, out.rHandRot, 12)) {
            out.handOk = 1; out.handWorld = 1;
        }
    }

    // ---- the TRICK DEF's name: resolved to a string ONCE per def change (ToString allocates) and
    // cached. Empty = no trick. The receiver un-gates the crouch + trick ratios by resolving it.
    {
        static void* lastDef = nullptr;
        static char  cached[48] = {};
        void* def = rdPtr(pawn, off::kSkaterTrickDef);
        if (def != lastDef) {
            lastDef = def;
            cached[0] = 0;
            // Prove the receiver's trick gate against an object known to be genuine -- the local
            // player's own. If membership in the local _flipTricks list rejects it, the premise is
            // wrong and the gate must switch itself off rather than reject every peer.
            if (def && FlipTrickListAvailable())
                GateSelfCheck("FlipTrickDefinition", IsKnownFlipTrickDef(def), g_logf);
            const Syms& S = Get();
            if (def && S.FNameToString) {
#ifdef _WIN32
                __try {
                    struct FStr { wchar_t* d; int n; int max; } fs{};
                    S.FNameToString((uint8_t*)def + off::kObjNamePrivate, &fs);
                    if (fs.d && fs.n > 0) {
                        int k = 0;
                        for (; k < fs.n && k < (int)sizeof(cached) - 1 && fs.d[k]; k++)
                            cached[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
                        cached[k] = 0;
                        // the FString buffer is game-allocated; leaked deliberately (a few bytes per
                        // trick CHANGE -- freeing it needs the game's allocator, not worth the coupling)
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { cached[0] = 0; }
#endif
            }
        }
        memcpy(out.trickName, cached, sizeof(out.trickName));

        // ---- the GRIND def's name, the trickName pattern verbatim: _currentGrindDef, falling back to
        // _targetGrindDef (GetGrindBlendSpace itself prefers current while grinding, target otherwise,
        // so sampling either non-null one matches its lookup). Resolved to a string once per def
        // CHANGE; the receiver un-gates the grind upper-body pose by resolving it.
        {
            static void* lastGrindDef = nullptr;
            static char  cachedGrind[48] = {};
            void* gdef = rdPtr(pawn, off::kSkaterGrindDefCur);
            if (!gdef) gdef = rdPtr(pawn, off::kSkaterGrindDefTgt);
            if (gdef != lastGrindDef) {
                lastGrindDef = gdef;
                cachedGrind[0] = 0;
                // ...and the same proof for the grind gate, off the local grind definition.
                if (gdef) GateSelfCheck("GrindOrSlideDefinition",
                                        IsObjectOfClass(gdef, "GrindOrSlideDefinition"), g_logf);
                const Syms& S = Get();
                if (gdef && S.FNameToString) {
#ifdef _WIN32
                    __try {
                        struct FStr { wchar_t* d; int n; int max; } fs{};
                        S.FNameToString((uint8_t*)gdef + off::kObjNamePrivate, &fs);
                        if (fs.d && fs.n > 0) {
                            int k = 0;
                            for (; k < fs.n && k < (int)sizeof(cachedGrind) - 1 && fs.d[k]; k++)
                                cachedGrind[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
                            cachedGrind[k] = 0;   // FString buffer leaked deliberately, bytes per change
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) { cachedGrind[0] = 0; }
#endif
                }
            }
            memcpy(out.grindName, cachedGrind, sizeof(out.grindName));
            float gr;
            if (rd((uint8_t*)pawn + off::kSkaterGrindPitch, &gr, 4)) out.grindPitch = gr;
            if (rd((uint8_t*)pawn + off::kSkaterGrindYaw,   &gr, 4)) out.grindYaw   = gr;
        }

        // ---- the crank: bit 0 of +0x580, pocket ratio, and the crank def's INDEX in the shared tricks
        // database's _crankList. It must be the index, not an offset into the TRICK def -- the def
        // lives in the DB's heap TArray. The game leaves _currentCrankDef set after a crank ends, so
        // the index transports continuously and the receiver's blend space is populated long before
        // the next crank.
        const int ck = rdByte(pawn, off::kSkaterIsCranking);
        out.crankOn = (ck > 0 && (ck & 1)) ? 1 : 0;
        float cp;
        if (rd((uint8_t*)pawn + off::kSkaterCrankPocket, &cp, 4)) out.crankPocket = cp;
        out.crankDefOff = 0xffff;
        const int cidx = CrankIndexFromPtr(rdPtr(pawn, off::kSkaterCrankDef));
        if (cidx >= 0 && cidx < 0xffff) out.crankDefOff = (uint16_t)cidx;
    }

    // ---- the pose blob (M2.4): the field-table sample of USkaterAnimInstance -- what the anim graph
    // READS, so the observer's graph renders the sender's pose without deriving anything. The table in
    // anim_fields.h IS the wire layout; a mid-walk read failure drops the WHOLE blob (a truncated blob
    // is valid on the wire, but a failing anim instance is not worth trusting at all this frame).
    out.animLen = 0;
    if (anim) {
        int n = 0;
        const int N = repl::AnimFieldCount();
        for (int i = 0; i < N; i++) {
            const repl::AnimField& f = repl::AnimFieldAt(i);
            if (n + f.size > (int)sizeof(out.anim)) break;
            if (!rd((uint8_t*)anim + f.off, out.anim + n, f.size)) { n = 0; break; }
            n += f.size;
        }
        out.animLen = (uint16_t)n;
    }
    // ---- REPLAY DIAGNOSTIC: while the local player scrubs the replay editor, their capsule still
    // moves (so position/rotation replicate) but the pose may not. This one line separates the
    // sender-side possibilities:
    //   * animLen 0            => the anim instance is gone/replaced during playback; no pose is
    //                             published at all and the proxy falls back to its own idle graph.
    //   * ai POINTER CHANGED   => replay swapped in a different anim instance, so the live one is
    //                             being read while the recorded pose drives another.
    //   * animLen normal but the values sit at IDLE => the graph is still deriving from a movement
    //                             component that is no longer simulating, and the recorded pose is
    //                             applied DOWNSTREAM of the transported variables.
    // Fields are read through the FIELD TABLE; never index the blob by struct offset.
    if (debug::Get().replayPoseDiff && LocalReplayMode()) {
        // Count changes across the WHOLE blob, never a hand-picked subset: the fields that drive the
        // POSE are the CONTINUOUS ones (SpeedRatio, BankingRatio, the powerslide/catch ratios), and a
        // replay can drive those perfectly well while leaving the state booleans stale, so sampling
        // booleans alone reads as "the whole blob is frozen". The full count separates:
        //    0 fields changing  => nothing drives the anim instance; the pose must be transported.
        //   >0 fields changing  => it IS being driven and the proxy's graph is misreading it, which is
        //                          a completely different and much cheaper fix.
        static uint64_t lastMs = 0; static void* lastAi = nullptr;
        static uint8_t  prev[sizeof(out.anim)]; static uint16_t prevLen = 0; static bool havePrev = false;
        static uint32_t chgFields = 0, chgBytes = 0;
        // Diffed EVERY frame, not between log lines: a value that wobbles and returns reads as static
        // at a 1 Hz sample, and the windows of interest here are ~100 ms.
        if (havePrev && prevLen == out.animLen) {
            uint32_t cb = 0, cf = 0; int off = 0;
            for (int i = 0, N = repl::AnimFieldCount(); i < N; i++) {
                const repl::AnimField& f = repl::AnimFieldAt(i);
                if (off + f.size > (int)out.animLen) break;
                bool changed = false;
                for (int b = 0; b < f.size; b++)
                    if (out.anim[off + b] != prev[off + b]) { cb++; changed = true; }
                if (changed) cf++;
                off += f.size;
            }
            if (cb > chgBytes)  chgBytes  = cb;
            if (cf > chgFields) chgFields = cf;
        }
        memcpy(prev, out.anim, out.animLen); prevLen = out.animLen; havePrev = true;

        // ---- and the SKELETON. The blob being inert does not say WHERE the visible pose lives, and
        // that decides the whole transport: LOCAL (bone-space) rotations are ~4 B/bone quantised,
        // while COMPONENT space needs a position too and roughly triples the cost. So measure both.
        // A wrong FTransform stride reads garbage, so the probe checks the quaternions are unit-length
        // and reports it, validating its own assumption instead of trusting it.
        int boneN = 0, bsChg = -1, csChg = -1; bool unitOk = false;
        if (mesh) {
            static float prevBs[128][4], prevCs[128][4];
            static int   prevN = 0; static bool haveBones = false;
            uint8_t* bsData = nullptr; int bsNum = 0;
            uint8_t* csData = nullptr; int csNum = 0;
            rd((uint8_t*)mesh + off::kMeshBoneSpaceData, &bsData, sizeof(bsData));
            rd((uint8_t*)mesh + off::kMeshBoneSpaceNum,  &bsNum,  sizeof(bsNum));
            int readIdx = 0;
            rd((uint8_t*)mesh + off::kMeshCompReadIdx, &readIdx, sizeof(readIdx));
            if (readIdx == 0 || readIdx == 1) {
                uint8_t* csBase = (uint8_t*)mesh + off::kMeshCompSpaceArr + readIdx * off::kMeshCompSpaceStride;
                rd(csBase,     &csData, sizeof(csData));
                rd(csBase + 8, &csNum,  sizeof(csNum));
            }
            boneN = bsNum;
            if (bsData && bsNum > 0 && bsNum <= 512) {
                const int n = bsNum < 128 ? bsNum : 128;
                float bs[128][4], cs[128][4];
                bool okAll = true;
                for (int b = 0; b < n; b++) {
                    if (!rd(bsData + (size_t)b * off::kTransformStride + off::kTransformRotOff, bs[b], 16)) { okAll = false; break; }
                    if (csData && b < csNum)
                        rd(csData + (size_t)b * off::kTransformStride + off::kTransformRotOff, cs[b], 16);
                    else memset(cs[b], 0, 16);
                }
                if (okAll) {
                    // Stride self-check: a real FQuat is unit length.
                    int unit = 0;
                    for (int b = 0; b < n; b++) {
                        const float d = bs[b][0]*bs[b][0] + bs[b][1]*bs[b][1] + bs[b][2]*bs[b][2] + bs[b][3]*bs[b][3];
                        if (d > 0.98f && d < 1.02f) unit++;
                    }
                    unitOk = (unit > n * 3 / 4);
                    if (haveBones && prevN == n) {
                        bsChg = 0; csChg = 0;
                        for (int b = 0; b < n; b++) {
                            if (memcmp(bs[b], prevBs[b], 16)) bsChg++;
                            if (memcmp(cs[b], prevCs[b], 16)) csChg++;
                        }
                    }
                    memcpy(prevBs, bs, sizeof(float) * 4 * n);
                    memcpy(prevCs, cs, sizeof(float) * 4 * n);
                    prevN = n; haveBones = true;
                }
            }
        }
        static int bsChgMax = 0, csChgMax = 0;
        if (bsChg > bsChgMax) bsChgMax = bsChg;
        if (csChg > csChgMax) csChgMax = csChg;
        // The PEAK push multiplier this second. A push is brief, so an instantaneous sample would
        // usually miss it; if fast tapping does not move this, it stays 1.00 and the log says so.
        static float pushMax = 0.f;
        if (out.pushSpeed > pushMax) pushMax = out.pushSpeed;

        const uint64_t nowMs =
#ifdef _WIN32
            GetTickCount64();
#else
            0;
#endif
        if (nowMs - lastMs >= 1000 || anim != lastAi) {
            const bool aiChanged = (lastAi && anim != lastAi);
            lastMs = nowMs; lastAi = anim;
            char m[320];
            snprintf(m, sizeof(m),
                     "[replay] mode=%u animLen=%u fields %u/%d | BONES n=%d local=%d comp=%d unitQ=%s"
                     " | spd=%.3f bank=%.3f pslide=%.3f pushMul=%.2f",
                     (unsigned)LocalReplayMode(), (unsigned)out.animLen, chgFields,
                     repl::AnimFieldCount(), boneN, bsChgMax, csChgMax, unitOk ? "ok" : "BAD",
                     repl::AnimFieldFloat(out.anim, out.animLen, 0x2f8, -1.f),   // Speed
                     repl::AnimFieldFloat(out.anim, out.animLen, 0x2c8, -1.f),   // BankingRatio
                     repl::AnimFieldFloat(out.anim, out.animLen, 0x2e4, -1.f),   // PowerslideRatio
                     pushMax);                                                   // peak push rate
            if (g_logf) g_logf(m);
            (void)aiChanged;
            chgFields = chgBytes = 0; bsChgMax = csChgMax = 0; pushMax = 0.f;    // per-second window
        }
    }
    // ---- tell peers this machine is scrubbing, so they send RESULTS: their drivers cannot be turned
    // into a pose on a machine whose replay editor is up. Costs no bytes (f2 bit 4).
    out.replaying = (LocalReplayMode() == 2);
    // ---- if the driver blob is inert (replay playback), send the SKELETON instead. Fires only while
    // the LOCAL player scrubs; a scrubbing PEER is served by a separate unicast packet built in the
    // session's publish loop, so everyone else keeps drivers.
    // Deliberately last: Capture clears animLen/feetOk, which only makes sense once they are sampled.
    pose::Capture(mesh, out);
    return out.bodyPosOk != 0;
}

}} // namespace omp::game
