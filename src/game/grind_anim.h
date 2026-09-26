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
// SessionOpenMP -- WHICH GRIND ANIMATION A PEER'S SKATER PLAYS.
//
// The grind DEFINITION travels by name (replication.h grindName) and resolves correctly. But the pose
// the game draws for it is chosen by USkaterAnimInstance::GetGrindBlendSpace (Epic 0xf58450), which
// takes two more inputs from the SKATER on the machine doing the drawing:
//   * ASkaterCharacterBase::IsSkatingSwitch -> skater+0x598 `_footPosition` (EFootPositionType
//     None 0 / Regular 1 / Fakie 2 / Nollie 3 / Switch 4; 2 and 4 read as switch) picks the
//     definition's RegularAnimData or SwitchAnimData;
//   * USkaterMovementComponent::IsFacingMoveDirection(boardVelocity) -- is the board moving the way
//     it faces -- and when it is NOT, a definition with a MirrorGrindDefinition is swapped for its
//     mirror with the switch choice flipped. The mirror of a tailslide is a nose trick.
// On a proxy both were the OBSERVER's guesses: `_footPosition` was never sent, and the facing test ran
// against the proxy board's velocity, which is the velocity drive's correction and not the owner's
// motion -- a slow slide or a correction pointed it backwards for a moment and the peer's tailslide
// played as a noseslide or noseblunt until it lined up again. Field report: "randomly their grind
// animations look wrong, usually fixes itself".
//
// Now the owner's game answers both. The sender records IsFacingMoveDirection's result for its own
// skater exactly where GetGrindBlendSpace asks it (a hook pair: the blend-space call marks the thread,
// the facing call inside it records), and sends it with `_footPosition` (wire minor 5). The receiver
// writes `_footPosition` onto the proxy and, again ONLY inside GetGrindBlendSpace, answers the facing
// question for a proxy with the owner's answer. Nothing else the proxy's movement asks is touched.
#pragma once
#include <cstdint>

namespace omp::game::grindanim {

void    Install(void (*logf)(const char*));
void    NoteOwnPawn(void* pawn);
// The game's own answer for the local skater, as last asked by GetGrindBlendSpace: 1 facing, 2 not,
// 0 = not asked recently (no mirrored grind in play) -- the receiver then keeps its own judgement.
uint8_t OwnFacing();
// Receiver: the owner's answer for this proxy skater (0 = none: let the proxy's own test run).
void    NoteProxy(void* skater, uint8_t face);
void    ForgetProxy(void* skater);
// Counters for the 1 Hz line: how often a proxy's grind pose was given the owner's answer, and how
// often that answer DIFFERED from what the proxy's own test would have said (= the bug, caught).
uint32_t Overrides();
uint32_t Disagreed();

} // namespace omp::game::grindanim
