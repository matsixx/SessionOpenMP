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
// TELEPORT TO A PLAYER -- through the game's own marker return, not a position write.
//
// Session already knows how to put a skater somewhere: it is what returning to a marker does, and it
// is far more than a move. ASkaterCharacter::Tick calls UpdatePendingGotoMarker every frame, which
// early-outs unless _isGotoMarkerPending is set and otherwise places the skater AND the board, sets
// both rotations, points the camera, and resets the movement state that would otherwise carry your
// old velocity into the new spot. Writing a location instead would leave a skater travelling at
// whatever speed they left at, mid-trick, with a board somewhere else entirely.
//
// So this fills the game's own FSessionPlayerMarkerInfo and raises its own flag. The next tick does
// the work, by the same code path the game uses for its own markers.
//
// WHERE the target comes from is the proxy actor, not the peer's wire state: the proxy is already
// standing where you can see them, so arriving where it stands is what "teleport to them" means on
// screen. It also needs no transported data, which is why this costs nothing on the wire.
#pragma once

namespace omp { namespace game {

// How far BEHIND the target to land, in centimetres. Arriving exactly inside somebody shoves them,
// and there is deliberately no arrival grace to hide behind (1.1.8 withdrew it), so this stays real.
extern float teleportBehindCm;

// Put the local skater where this proxy is standing. `proxyBoard` may be null -- an off-board peer's
// board is placed with them anyway, because the marker return expects both.
// Returns false and logs why if the pawn is not a skater the marker path applies to.
bool TeleportLocalToProxy(void* ownPawn, void* proxyActor, void* proxyBoard, bool onBoard,
                          void (*logf)(const char*));

}} // namespace omp::game
