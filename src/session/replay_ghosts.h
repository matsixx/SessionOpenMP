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
// =====================================================================================================
// SessionOpenMP -- REPLAY GHOSTS: the peers of a SAVED replay, driven back during its playback.
//
// A loaded replay's sidecar (replication/sidecar.h) gives each peer a history buffer, injected into
// replaysync under a GHOST index, and the cosmetics to dress them. While the local player is in
// playback (LocalReplayMode() == 2) every ghost gets a real proxy skater -- the same game::Proxy a live
// peer gets, spawned from the engine tick like always -- dressed once, and driven each frame from its
// buffer at the scrub-derived time, exactly the way a synced peer is driven in session.cpp.
//
// This runs WITHOUT A SESSION. Nothing here touches the transport, the lobby or the publish path;
// the loader gives it a frame whenever a sidecar is loaded, armed or not. Ghosts are never released
// by quiet or departure -- they end when playback ends (the editor closes), when a replay with no
// sidecar loads, or when the world changes. Their actors are hidden the moment they should go and
// destroyed only outside playback: the recorder still holds actor pointers during playback.
// =====================================================================================================
#pragma once
#include <cstdint>

namespace omp { namespace ghosts {

constexpr int kGhostBase = 100;      // replaysync peer indices for ghosts: never a transport index

// A sidecar's bytes: parse, inject the histories, hold the wardrobes. Replaces whatever was loaded.
// False = refused (a log line says why). Game thread.
bool LoadSidecar(const uint8_t* data, uint32_t len, void (*logf)(const char*));
// A replay with no sidecar was loaded: hide every ghost now, destroy them when playback ends.
void Clear(void (*logf)(const char*));
// A sidecar is loaded (ghosts may or may not be spawned yet).
bool Active();
// Anything still standing that Frame has to finish with (retired ghosts awaiting a safe destroy).
bool AnyAlive();
// The per-frame drive. `ownPawn` may be null (nothing spawns without it).
void Frame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, void (*logf)(const char*));
// The world changed: every actor died with it. Pointers dropped, buffers dropped, ghosts ended.
void ForgetAll();
// One of ours? (The local-pawn veto and the "is this a proxy" seams include ghosts.)
bool IsGhostActor(void* actor);

}} // namespace omp::ghosts
