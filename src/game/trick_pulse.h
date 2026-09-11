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
//
// THE TRICK SERIAL -- why a flick has to travel as a COUNT, not as a flag.
//
// Field report: on an observer, a skater's arms lift only on the way back down from a trick, where
// the owner's lift the instant the trick starts. Four measurement rounds, each one ruling out the
// layer above: not the body physics (every pump number identical on both sides), not the blend
// weights (identical once settled), not the trick assets (the same four blend spaces by name), not
// a blueprint variable (the anim blueprint has none -- all 553 of its properties are graph nodes),
// not any transported input (a byte-diff of the whole anim instance found only pointers and
// world-space feet). What differed was the GRAPH'S STATE: the owner's 'Default' machine enters
// 'Street Trick State' the frame of the flick; the proxy's stays in 'Idle Skate State' through the
// pop and jumps straight to 'Street Trick Catch State'. The flip-trick animation never plays on a
// proxy. What the observer sees is the catch loop's arm rise -- later, and higher.
//
// The transition's trigger is IsTrickPending (anim +0x310). ASkaterCharacterBase::SetTrick sets it
// at the flick, nothing native clears it, and the blueprint consumes it on state entry -- inside ONE
// frame. A once-a-frame gather never sees it, so the whitelisted field always carries 0. The game
// itself solved this for its remote skaters: MulticastRPCSetTrick_Implementation sets IsTrickPending
// on the observer's copy so ITS graph takes the transition. The overlay has no RPC; this is that path.
//
// The wire carries a SERIAL -- SetTrick hooked on the owner, counted once per call -- because the
// trick NAME changes only when the def changes, and four kickflips in a row are four flicks on one
// def. The proxy fires on any change of the serial and writes IsTrickPending in the anim post-pass
// AFTER the blob (which would otherwise erase it with the sender's sampled 0), the last writer before
// its graph updates. The skater-side _isPopPending bit the multicast also sets is deliberately NOT
// mirrored: it feeds board and pop physics, and unlike a stock replicated board a proxy's board is
// simulated locally when near. The transition rule cannot read a private bitfield anyway.
#pragma once
#include <cstdint>

namespace omp::game::trick {

// Hook ASkaterCharacterBase::SetTrick (once; idempotent). Needs the resolved symbol.
void    Install(void (*logf)(const char*));
// The gather tells the hook which skater is ours; only that one's flicks count.
void    NoteOwnPawn(void* pawn);
// The count of the own skater's SetTrick calls, wrapping. Edges are what a receiver acts on.
uint8_t OwnSerial();

} // namespace omp::game::trick
