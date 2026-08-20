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
// SessionOpenMP -- POINT THE REPLAY CAMERA AT A PEER.
//
// While you are in the replay editor, a row on the ReplayEditors page cycles the players in the
// session; choosing one aims the replay camera at them. Scrubbing still drives only your own replay --
// their skater is LIVE (and may be scrubbing their own replay, which the pose lane replicates).
// Nothing about their replay is transported; each player's recording stays entirely in their own game.
//
// This drives the game's OWN aim target rather than overriding the rendered view:
//   ASessionReplayManager::GetInstance() -> _replayInputController (+0x280) -> _replayCamera (+0x260)
//   AReplayCamera::_lookAtTarget (+0x868) is what GetLookAtLocation reads, and the Orbit and Tripod
//   tick modes honour it. Write it, then SetCameraType(RCT_Orbit) so the camera recomputes its orbit
//   distance and rotation and snaps onto the new subject.
// Overwriting the camera manager's cached POV instead would only rotate the RENDERED view: the
// AReplayCamera actor's own transform would stay where it was, so anything the editor records or
// keyframes would disagree with what you saw. Driving the game's target keeps them the same thing.
// =====================================================================================================
#pragma once
#include <cstdint>

namespace omp { namespace game { namespace spectate {

// Your own skater, republished every frame by the loader -- lets us tell "watch a peer" (an override)
// apart from "watch myself" (an undo of it).
void SetLocalSkater(void* pawn);

// `actor` = the peer's proxy skater, or null for "Me".
// The replay camera belongs to the USER -- they placed it and chose its mode. Aiming at a peer is an
// override, saved once on entry; "Me" UNDOES that override and restores exactly what was there. If
// nothing was ever overridden, "Me" does nothing at all -- it must never force an Orbit onto the local
// skater, which would throw an untouched camera across the map on entering a solo replay.
// Safe to call when not in the replay editor: it resolves the camera and quietly does nothing if the
// editor is not up. Returns true if the camera was actually re-aimed.
bool SetLookTarget(void* actor, void (*logf)(const char*));

// The current target, and whether we have aimed at anything (so the menu row can show the selection).
void* Target();

// A proxy actor is about to die (peer left, or the world changed). If it was the camera's subject,
// clear BOTH our pointer and the game's own `_lookAtTarget` -- the game dereferences its copy every
// frame of playback, so leaving it set is a fault inside the camera with our DLL nowhere in the
// stack. Safe and near-free to call for every retiring proxy: it returns immediately unless the actor
// is the one being watched.
void OnActorGone(void* actor, void (*logf)(const char*));
// Per-frame safety net for the camera's subject. OnActorGone covers the teardowns the mod runs
// itself; this covers every other way a proxy can stop existing, because the engine reads that
// pointer on its own tick and a stale one faults inside the camera with this DLL nowhere in the
// stack (AReplayCamera::GetLookAtLocation, field-reported).
void ValidateLookTarget(void (*logf)(const char*));

// ---- KEEP PEERS OUT OF YOUR RECORDING ---------------------------------------------------------------
// A proxy's replay components register THEMSELVES into ASessionReplayManager at BeginPlay -- a proxy is
// a real game-class skater, so it owns the same UDebugSkaterReplayComponent every skater does, and its
// board a UDebugSkateboardReplayComponent. Left alone, every peer and every peer's board lands in your
// replay buffer, for as many players as join.
// This unregisters any component owned by one of our proxies, and leaves the component's `_manager`
// back-pointer set so BeginPlay's own idempotence guard stops it ever registering again. The peer is
// then neither recorded nor played back -- during your replay you see their LIVE skating, because the
// wire drive is the only thing writing them. Call it every frame: it is a scan of a couple of dozen
// pointers, and once the latch is set it removes nothing. Returns how many were removed.
// Full derivation of the array layout is in the .cpp.
int PruneProxyComponents(void (*logf)(const char*));
// Per-peer visibility in the LOCAL player's replay: hiding parks the peer's components (stashed)
// and conceals the actor + board; showing re-appends and unhides. No-ops when nothing has to move.
void SetPeerShownInReplay(void* skaterActor, bool shown, void (*logf)(const char*));

}}} // namespace omp::game::spectate
