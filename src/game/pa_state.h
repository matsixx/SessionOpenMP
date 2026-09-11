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
// THE OWNER'S PHYSICAL-ANIMATION LIFECYCLE, as the game broadcasts it.
//
// Field report: when a peer returns to their marker, an observer sees their body physics thrash for a
// second; the owner sees nothing. The owner's own log says why: the game holds physical animation OFF
// for ~100 ms around a marker return (GotoMarker -> BroadcastDisablePhysicalAnimation, re-enabled when
// the return completes), so the pose snap and the move land on kinematic bodies. The proxy kept 21
// simulating bodies live through the same snap. A first attempt keyed on a teleport-sized jump of the
// root, off and on in one frame, and missed on both counts: returns to a nearby marker move the root
// less than the threshold yet still cycle PA and snap the pose, and the game's window is 100 ms, not 0.
//
// The faithful signal is the broadcasts themselves. The +0x711 enable bit is NOT it: only
// SetIsPhysicalAnimationEnabled writes that bit, and GotoMarker calls the disable broadcast directly,
// so the bit stays set through a marker return. Both broadcasts are hooked on the owner (signatures
// unique on both builds) and reduced to one byte on the wire: bit 0 valid (a minor-1 packet reads 0
// there and the proxy keeps today's behaviour), bit 1 the state, six bits a change count so the
// receiver sees every edge even when two land inside one snapshot interval. The proxy's PA mirror
// ANDs the owner's state with its own judgement: off exactly when the owner's is, for as long as it is,
// on marker returns, teleports, bails, pickups, replay mode -- every lifecycle event the game has.
// The blueprint's at-speed unbind bypasses the broadcasts and is deliberately left as it is today.
#pragma once
#include <cstdint>

namespace omp::game::pa {

// Hook the two broadcasts (once; idempotent). Needs the resolved symbols.
void    Install(void (*logf)(const char*));
// The gather tells the hooks which skater is ours; the state is seeded from its enable bit.
void    NoteOwnPawn(void* pawn);
// 0 = unknown (never sent as such by this build). Else bit 0 set, bit 1 = enabled, bits 2-7 = changes.
uint8_t OwnPaSerial();

} // namespace omp::game::pa
