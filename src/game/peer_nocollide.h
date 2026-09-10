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
// A PROXY SKATER THAT IGNORES OTHER SKATERS -- with its collision left ON.
//
// 1.1.6 made a skater in spawn grace "uncollidable" with SetActorEnableCollision(false), and that
// silently switched off peer body physics for anyone parked at a marker: the engine forces a body
// whose collision is disabled to KINEMATIC (FBodyInstance::UpdatePhysicsFilterData ->
// SetIsKinematic_AssumesLocked, in the PDB), so physical animation had nothing left to drive. The
// field test that settled it: the other player's torso looseness appeared the instant the watcher
// pushed -- pushing ends grace, grace had every proxy decollided.
//
// So the skater keeps its collision and instead RESPONDS Ignore to the channels other skaters
// arrive on. UE resolves a contact only when both sides Block, so one side ignoring is enough in
// both directions. The bodies keep simulating; nothing can shove them and they can shove nothing.
//
// WHERE THE IGNORE HAS TO BE WRITTEN, read out of FBodyInstance::BuildBodyFilterData rather than
// assumed -- the first two rounds of this wrote each body's own response container and were never
// effective on the bodies:
//   - the CAPSULE is a plain component, so its own container is what its shape is filtered by.
//   - a BODY INSIDE A SKELETAL MESH is not. The skeletal branch of BuildBodyFilterData overwrites
//     the body's container with all-Block (when the body setup says collision is enabled), then
//     takes the per-channel MIN with the MESH COMPONENT's container (meshComp+0x2c8+0x78). So the
//     component's container is the only per-channel say the bodies get, and after writing it every
//     body's shape filter is rebuilt from it -- which is exactly what the engine's own
//     USkeletalMeshComponent::OnComponentCollisionSettingsChanged does. A body a re-dress builds
//     later reads that same container at its init, so the write survives a re-dress on its own.
// The channels themselves are read live off this skater's capsule, mesh and board (the two skaters
// are the same class) on top of the engine's Pawn and PhysicsBody -- the capsule turned out to be
// object type 15, a custom channel, not Pawn. Every response byte written is recorded and put back
// exactly; the record is separate from the trim's because the two run on different lifecycles.
#pragma once

namespace omp::game {

// Make this proxy's capsule and every physics body ignore skaters. Idempotent once it holds, and
// decided by state on every call, so a response the game writes back is caught. `capsuleBody` is
// the root component's FBodyInstance; `boardBody` the board root's (either may be null).
void PeerNoCollideApply(void* meshComp, void* capsuleBody, void* boardBody, void (*logf)(const char*));

// Put every response back exactly as found. A no-op on a mesh that was never changed.
void PeerNoCollideRestore(void* meshComp, void* capsuleBody, void (*logf)(const char*));

// The actor is gone (world change, destroy): drop the record WITHOUT writing.
void PeerNoCollideForget(void* meshComp);

} // namespace omp::game
