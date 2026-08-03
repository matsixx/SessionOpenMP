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
// SessionOpenMP -- the USkaterAnimInstance field table: the pose blob's WIRE LAYOUT.
//
// The blob in repl::State::anim is this table's fields packed IN TABLE ORDER -- the table defines the
// wire format, so it must be IDENTICAL on sender and receiver (one binary ships to everyone), and fields
// are APPEND-ONLY within a release. Offsets are into the game's USkaterAnimInstance (reached via
// skater +0x280 mesh -> +0x6b0 AnimScriptInstance, both PDB-derived).
//
// WHY A FIELD TABLE AT ALL: the game publishes anim params that NOTHING on an observer reads, and the
// anim graph computes from live component state a proxy does not have. So the owner samples what the
// graph READS (plain members) and the observer writes them -- transport the truth, never derive it.
//
// THE THREE RULES THIS TABLE ENCODES:
//   1. NO indices, NO pointers, NO world-space positions: a value that is only meaningful in the
//      sender's frame cannot travel. The *AnimSetIndex fields are absent, not clamped; the foot/hand
//      IK world POSITIONS travel separately, rebased (State's feet/hands fields); only alphas, ratios,
//      local deltas and state bytes ride this blob.
//   2. ASSET GATES: a transported ratio OR BOOL that alphas/enables an asset-driven node is written
//      ONLY if the observer's anim instance has that asset; else forced to 0. A null asset in a blend
//      node evaluates to the reference pose = a T-pose. Gate EVERY size -- a bool that enables a state
//      is as dangerous as the ratio that alphas it.
//   3. STEP vs BLEND: continuous floats/vectors interpolate between snapshots against the explicit
//      whitelist below; every bool, enum and type byte is STEPPED from the at-render-time sample. A
//      half-applied IsGrinding or fractional FootPositionType is meaningless. The whitelist is
//      deliberately opt-in: a field added later is snapped (safe) until someone decides otherwise.
#pragma once
#include <cstdint>
#include <cstring>

namespace omp { namespace repl {

struct AnimField { uint16_t off; uint8_t size; };

static const AnimField kAnimFields[] = {
    {0x2c8,4},  // BankingRatio
    {0x2f4,4},  // SpeedRatio
    {0x2f8,4},  // Speed
    {0x2fc,4},  // OnFootTurnRatio
    {0x305,1},  // FootPositionType
    {0x306,1},  // LandingFootPositionType
    {0x307,1},  // FootPositionTransitionType
    {0x308,1},  // FootToBoardTransitionType
    {0x309,1},  // BoardToFootTransitionType
    {0x30c,4},  // FlipTrickStanceTransferRatio
    {0x310,1},  // IsTrickPending
    {0x313,1},  // HasLeftFootCatchOrient
    {0x314,1},  // HasRightFootCatchOrient
    {0x324,4},  // CatchOrientPreLandRatio
    {0x329,1},  // IsPushing
    {0x32a,1},  // IsPushingRegular
    {0x32b,1},  // IsPushingMongo
    {0x32c,1},  // IsPushingSwitch
    {0x32d,1},  // WasPushingMongo
    {0x32e,1},  // IsInManual
    {0x32f,1},  // IsInNoseManual
    {0x330,4},  // ManualRatio
    {0x33c,1},  // IsGrinding
    {0x33d,1},  // IsGrindingInLiptrick
    {0x380,4},  // GrindPreLandRatio
    {0x3c8,1},  // IsKickTurnLeft
    {0x3c9,1},  // IsKickTurnRight
    {0x3ca,1},  // WasKickTurnLeft
    {0x3cb,1},  // WasKickTurnRight
    // -- the rest of the pose. A subset of a COUPLED group is its own bug: catch orient without its
    // state/angles leaves a persistent post-landing lean.
    {0x2cc,4},  // BodyRotationRatio          <- upper-body turn
    {0x2d0,4},  // ShoulderAdditiveAlpha
    {0x2d4,4},  // ShoulderRotationRatio
    {0x2d8,1},  // IsInPowerslideStance
    {0x2d9,1},  // IsPowersliding
    {0x2da,1},  // IsExitingPowerslide
    {0x2dc,4},  // PowerslideCOMOffsetRatio
    {0x2e0,4},  // PowerslidePitchRatio
    {0x2e4,4},  // PowerslideRatio
    {0x2e8,4},  // PowerslideTargetRatio
    {0x2ec,4},  // PowerslideExitRatio
    {0x300,1},  // IsOnBoard
    {0x301,1},  // IsPickingUp
    {0x302,1},  // IsThrowingDown
    {0x303,1},  // IsSkatingSwitch            <- stance; wrong here mirrors the whole pose
    {0x304,1},  // IsSkatingGoofy
    {0x311,1},  // CatchMode                  } the catch/grab group -- must travel whole; a
    {0x312,1},  // CatchOrientState           } partial group leaves a stuck in-air lean
    {0x315,1},  // HasDarkSlideCatchOrient    }
    {0x318,4},  // CatchOrientYawRatio        }
    {0x31c,4},  // CatchOrientPitchRatio      }
    {0x320,1},  // IsAutoCatching
    {0x328,1},  // GrabState
    {0x334,1},  // IsInCasper
    {0x335,1},  // IsInAntiCasper
    {0x338,4},  // CasperRatio
    {0x3cc,1},  // IsInPrimo
    {0x3cd,1},  // IsWallRiding
    {0x3d0,4},  // WallRideAngleRatio
    {0x3d4,1},  // WallRideState
    {0x3d5,1},  // IsAnimMirrorOn
    {0x3d6,1},  // IsAnimatedThrowdown
    {0x3d8,4},  // ThrowdownFeetIKAlpha
    {0x3dc,12}, // HipOffset (FVector, a delta -- deltas travel, world points do not)
    // -- the foot IK ALPHAS (positions stay off this blob; they travel rebased in State). The alpha
    // DROP when feet leave the board is the signal that stops the observer gluing feet to a spinning
    // deck.
    {0x3fc,4},  // LeftFootIKAlpha
    {0x400,4},  // RightFootIKAlpha
    // -- the pelvis. Per-axis alpha + two LOCAL bone adjusts -- the class that is safe raw.
    // PelvisLocationAlpha pulls the pelvis DOWN when the legs tuck; computed locally on an observer it
    // reads ~0, which renders as a fully extended, too-high hop.
    {0x434,12}, // LeftFootBoneLocationAdjust  (FVector, local bone delta)
    {0x440,12}, // RightFootBoneLocationAdjust (FVector, local bone delta)
    {0x44c,12}, // PelvisLocationAlpha         (FVector -- per-axis blend weight, NOT a world point)
    // -- hand IK alphas -- legal ONLY because their targets travel too (State's hands, rebased). An
    // alpha whose target the observer invented blends the arm toward garbage.
    {0x458,4},  // LeftHandIKAlpha
    {0x45c,4},  // RightHandIKAlpha
    {0x490,1},  // MongoPushAvoidanceActive
    {0x491,1},  // MongoPushAvoidanceLeftFoot
    {0x492,1},  // HasCatchLoop
    {0x493,1},  // HasLateTrick
    {0x494,1},  // HasTrickLoop
    {0x495,1},  // IsBoardFlipping
    {0x496,1},  // IsBoardRotating
    {0x497,1},  // IsCranking
    {0x498,4},  // CrankPocketRatio
    {0x4c4,1},  // IsQuickCrankSwapping
    {0x4d0,4},  // FlipTrickPopRatio          <- the ollie crouch/charge
    {0x4d4,4},  // FlipTrickPlaybackRate
    {0x4d8,1},  // RevertType
    {0x4dc,4},  // RevertRatio
    {0x570,12}, // HeadLookAtLocalLocation (LOCAL, so it travels)
    {0x57c,4},  // HeadLookAtAlpha
    {0x59f,1},  // OnFootIsJumping
    {0x5a0,4},  // OnFootPreLandRatio
    {0x5a4,12}, // PelvisAdditionalOffset (delta)
    {0x5b0,12}, // PelvisAdditionalRotation (delta)
    {0x5fa,1},  // IsGrounded                 } the air/land group: without these nothing on the
    {0x5fb,1},  // IsGroundedWheels           } observer ever ENDS the in-air pose. IsGrounded ALSO
    {0x5fc,1},  // IsFalling                  } travels as the explicit State::grounded field --
    {0x5fd,1},  // IsLanding                  } that one gates the board drive and must not depend
    {0x5fe,1},  // IsJustLanded               } on blob decoding.
    {0x600,4},  // LandHeightRatio
    {0x604,4},  // LandDropHeightRatio
};
static const int kAnimFieldN = (int)(sizeof(kAnimFields) / sizeof(kAnimFields[0]));

inline int              AnimFieldCount()  { return kAnimFieldN; }
inline const AnimField& AnimFieldAt(int i){ return kAnimFields[i]; }

// Read ONE field out of a packed blob by its anim-instance offset. This is the ONLY legal way to do
// it: the blob is a packed field SEQUENCE, so `blob[off]` reads an unrelated byte. This walks the
// table to find the field's real position, which is what the sender's packer did to put it there.
// Returns null when the field is absent or the blob is short -- callers must treat that as "no
// information", never 0.
inline const uint8_t* AnimFieldPtr(const uint8_t* blob, int blobLen, uint16_t off, uint8_t size) {
    if (!blob) return nullptr;
    int n = 0;
    for (int i = 0; i < kAnimFieldN; i++) {
        const AnimField& f = kAnimFields[i];
        if (n + f.size > blobLen) break;
        if (f.off == off) return (f.size == size) ? blob + n : nullptr;
        n += f.size;
    }
    return nullptr;
}
inline uint8_t AnimFieldByte(const uint8_t* blob, int blobLen, uint16_t off, uint8_t dflt = 0) {
    const uint8_t* p = AnimFieldPtr(blob, blobLen, off, 1);
    return p ? *p : dflt;
}
inline float AnimFieldFloat(const uint8_t* blob, int blobLen, uint16_t off, float dflt = 0.f) {
    const uint8_t* p = AnimFieldPtr(blob, blobLen, off, 4);
    if (!p) return dflt;
    float v; memcpy(&v, p, 4); return v;
}

// Rule 2: {blob field, anim-instance offset that must hold a non-null asset pointer}.
struct AnimAssetGate { uint16_t off; uint16_t needPtrAt; };
static const AnimAssetGate kAnimAssetGates[] = {
    {0x30c, 0x4c8},   // FlipTrickStanceTransferRatio <- FlipTrick
    {0x4d0, 0x4c8},   // FlipTrickPopRatio            <- FlipTrick
    {0x4d4, 0x4c8},   // FlipTrickPlaybackRate        <- FlipTrick
    {0x4dc, 0x4e0},   // RevertRatio                  <- RevertBlendSpace
    {0x497, 0x4a8},   // IsCranking                   <- CrankLoopBlendSpace (the T-posing crouch)
    {0x498, 0x4a8},   // CrankPocketRatio             <- CrankLoopBlendSpace
    {0x4c4, 0x4a8},   // IsQuickCrankSwapping         <- CrankLoopBlendSpace
};
static const int kAnimAssetGateN = (int)(sizeof(kAnimAssetGates) / sizeof(kAnimAssetGates[0]));
inline uint16_t AnimAssetGateFor(uint16_t off) {
    for (int i = 0; i < kAnimAssetGateN; i++) if (kAnimAssetGates[i].off == off) return kAnimAssetGates[i].needPtrAt;
    return 0;
}

// Rule 3: the blend whitelist -- continuous ratios/angles/offsets only.
static const uint16_t kAnimSmoothOffs[] = {
    0x2c8, 0x2cc, 0x2d0, 0x2d4,                       // banking, body rotation, shoulder x2
    0x2dc, 0x2e0, 0x2e4, 0x2e8, 0x2ec,                // powerslide ratios
    0x2f4, 0x2f8, 0x2fc,                              // speed ratio, speed, on-foot turn
    0x30c, 0x318, 0x31c, 0x324,                       // stance transfer, catch yaw/pitch/preland
    0x330, 0x338, 0x380, 0x3d0, 0x3d8,                // manual, casper, grind preland, wallride, throwdown
    0x3dc,                                            // HipOffset
    0x3fc, 0x400,                                     // foot IK alphas
    0x434, 0x440, 0x44c,                              // foot bone adjusts + pelvis alpha
    0x458, 0x45c,                                     // hand IK alphas
    0x498, 0x4d0, 0x4d4, 0x4dc,                       // crank pocket, flip pop, playback rate, revert
    0x570, 0x57c,                                     // head look-at location + alpha
    0x5a0, 0x5a4, 0x5b0,                              // onfoot preland, pelvis offset, pelvis rotation
    0x600, 0x604,                                     // land height / drop height ratios
};
static const int kAnimSmoothOffN = (int)(sizeof(kAnimSmoothOffs) / sizeof(kAnimSmoothOffs[0]));
inline bool AnimFieldSmoothed(uint16_t off, uint8_t size) {
    if (size != 4 && size != 12) return false;
    for (int i = 0; i < kAnimSmoothOffN; i++) if (kAnimSmoothOffs[i] == off) return true;
    return false;
}

}} // namespace omp::repl
