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
// SPAWN GRACE -- a skater who has just appeared cannot be collided with, and cannot collide.
//
// Arriving at a spawn point or a marker puts a skater at a fixed spot, possibly inside someone who
// is already standing there, and the solver resolves that by shoving whichever of the two it likes
// least. So from the moment the LOCAL skater spawns or returns to a marker until a second after
// their first push, they are "in grace": the fact travels on the wire (one appended byte) and every
// observer drops that proxy's collision, skater and board; and locally every proxy's collision is
// dropped too, so the grace skater cannot hit anyone either. One without the other still leaves
// somebody stuck.
//
// The two triggers are the game's own, read where they cannot be raced:
//   * SPAWN: ASkaterCharacterBase::_wasJustSpawned, set in PostInitCharacter and cleared by the
//     first BroadcastEnablePhysicalAnimation (the first mount). Long-lived, so it is simply polled.
//   * MARKER RETURN: ASkaterCharacter::UpdatePendingGotoMarker, pre-hooked. It runs from Tick
//     every frame and early-outs unless _isGotoMarkerPending is set, and the same call consumes the
//     flag -- so polling that flag races the tick, and reading it ON ENTRY does not. (The marker
//     path does not go through TeleportTo: that was checked against the vtable before choosing.)
// What ENDS it is a push: the push state the movement component stores for the whole push cycle,
// already gathered for the wire. Steering, looking around and menus do not count.
#pragma once
#include <cstdint>

namespace omp { namespace game { namespace grace {

// Live knob: how long after the first push the collision comes back, in ms.
extern int releaseMs;

// Installs the marker-return pre-hook, once. Without the symbol, spawn grace still works and the
// marker case says so in the log.
void Install(void (*logf)(const char*));

// Per frame, from the session with the local pawn and its gathered push state. `replaying` is the
// local replay-editor state, which is its own no-collide reason and not part of grace.
void Tick(void* ownPawn, uint64_t nowMs, uint8_t pushState, void (*logf)(const char*));

// Is the local skater in grace right now? This is the byte that goes on the wire.
bool Active();

}}} // namespace omp::game::grace
