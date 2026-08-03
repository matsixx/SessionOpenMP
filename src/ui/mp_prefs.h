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
// SessionOpenMP -- the player's multiplayer PREFERENCES (as opposed to their name, which has its own
// module because it carries a word filter).
//
// Deliberately PURE: this module stores and persists, and touches nothing else. In particular it does
// NOT call the transport. Both menus can write a preference from whichever thread they run on (the F1
// overlay is the RENDER thread, the pause menu is the GAME thread), and the EOS SDK must only be
// driven from the thread that ticks its platform -- so a write bumps a generation counter and the
// game-thread pump applies the change. `MpPrefs_Generation()` is that counter.
//
// File: `SessionOpenMP_prefs.txt` next to the log, `key=value` per line, rewritten whole on change.
#pragma once

void MpPrefs_Init(const char* dir, void (*logf)(const char*));

// ---- "Hide my address" (default ON).
// EOS peer-to-peer will NAT-punch a direct connection when it can, and a direct connection means the
// people you are playing with learn your IP address -- which matters once the lobby browser puts you
// in sessions with strangers. Forcing Epic's relays keeps the address private at the cost of some
// latency.
// This is an EOS capability, not a universal one. It is named for the INTENT rather than for
// ForceRelays because other transports answer the question differently (or cannot answer it at all);
// those report what they actually do rather than pretending the switch worked.
// It applies to connections made AFTER it changes; links already established keep their route.
bool MpPrefs_HideAddress();
void MpPrefs_SetHideAddress(bool on);

// Bumped by every setter. The game thread applies preferences when this changes, so a menu can write
// from any thread without ever calling into the SDK itself.
unsigned MpPrefs_Generation();

// ---- This install's PERMANENT peer identity: 32 lowercase hex characters (128 random bits),
// generated once and kept in the preferences file.
// EOS hands out a ProductUserId, so the EOS backend never needed one. Every other transport does: a
// peer has to be recognisable across a reconnect, and it must NOT be recognisable by its address --
// a NAT rebind changes the address mid-session, and keying identity to an endpoint is how a peer
// silently becomes a stranger (the same rule that makes shm key on its SLOT and not on a PID).
// Random rather than derived from anything about the machine: it should identify an install to the
// people it plays with, and say nothing about the person to anyone else.
const char* MpPrefs_PeerId();
