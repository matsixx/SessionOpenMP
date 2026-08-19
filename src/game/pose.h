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
// SessionOpenMP -- POSE TRANSPORT: the RESULT lane.
//
// Everything else in this project transports the DRIVERS of an animation and lets the receiver's own
// anim graph derive the pose. That is the right default -- the graph fires anim notifies (which is
// where a peer's board-catch sound comes from), foot IK adapts locally, and drivers EXTRAPOLATE, so a
// dropped packet leaves a running skater still running instead of freezing mid-stride.
//
// It has exactly one failure mode, and the replay editor is it: when there are no drivers, there is
// nothing to send. Measured: while a peer scrubs their replay, 0 of 97 anim-instance fields change
// across 30 s, yet 58 of 70 BONES change every second. The pose being played exists only in the
// skeleton. Position and rotation replicate through all of this because they are transforms; the
// skater does not because it is a skeleton.
//
// So this is a narrow, GATED second lane: while a peer is in replay playback the mod sends their
// skeleton and stops sending the (inert) driver blob. Normal skating never touches this path and keeps
// every property above -- transport the result only for the one thing that cannot be derived.
//
// THE SEAM (why FinalizeBoneTransform and nothing else):
//   PostAnimEvaluation COPIES the evaluated pose into BoneSpaceTransforms and only then fills
//   component space, so anything written before it is overwritten. USkeletalMeshComponent::
//   FinalizeBoneTransform begins with the buffer FLIP, so at pre-hook time the EDITABLE component-
//   space array holds this frame's finished pose and is about to be published. That is the last
//   honest moment to replace it -- the same rule as the anim post-pass.
// =====================================================================================================
#pragma once
#include "../replication/replication.h"
#include <cstdint>

namespace omp { namespace game { namespace pose {

struct Tuning {
    bool enabled = true;        // master: false = no pose is captured or applied
    // Capture only while the local player is in replay PLAYBACK. Gating this tightly is the whole
    // point: live skating keeps the cheap driver path, its notifies and its extrapolation.
    uint8_t captureMode = 2;
    uint32_t freshMs = 500;     // a pose older than this is ignored, so a quiet stream hands the
                                // skeleton back to the proxy's own graph rather than freezing it
    // RETIRED (default false): re-stamp the last held pose over a proxy's skeleton during a local
    // replay. It served the live-view-while-scrubbing era, where fresh pose-lane packets always won
    // the race above and the hold only covered gaps. Today it has no beneficiary and one victim:
    // unsynced peers are CONCEALED during playback (a hold on an invisible skater is nothing), and
    // a SYNC-DRIVEN peer's graph IS live -- driven from their transferred history -- so the hold
    // stomped the finished pose every frame at FinalizeBones and froze them standing
    // (field-logged: holdApplied climbing ~120/s while the synced skater stood still).
    bool holdInLocalReplay = false;
    // Serve live results to a PEER who is scrubbing. OFF: the replay editor shows recordings only,
    // so a scrubbing peer applies no live poses and sending them is pure bandwidth. The machinery
    // stays because a future spectate mode outside the editor is exactly this lane switched on.
    bool serveScrubbingPeers = false;
    // On a bone-COUNT mismatch, stamp the common PREFIX instead of refusing. The character mesh is
    // MERGED from body + garments and garments can carry rig bones, so two ends dressing the same
    // player from different installed content get DIFFERENT counts -- one item "not installed here"
    // and every transferred pose was refused (field: skipCnt == noted, the synced skater a heap of
    // clothes; invisible on the same-PC rig, where both installs wear identical content). Merged
    // meshes share the base skeleton as their index prefix, so the prefix is the body; component-
    // space transforms are absolute, so a subset stamp is geometrically consistent. In the common
    // direction (their mesh carries MORE bones than our stripped-down proxy) our mesh is fully
    // posed. false = the old exact-match refuse, kept for A/B: if the prefix assumption were wrong
    // it would show instantly as a scrambled pose, and this switch restores the refuse.
    bool prefixOnMismatch = true;
    // NO SMOOTHING HERE -- TESTED AND REVERTED. Easing each bone toward the transported pose looked
    // like the obvious way to take the staircase off a sliced skeleton, and it BROKE the animation
    // outright. The blend read the component-space array as "last frame's pose", but the animator
    // re-evaluates and re-stomps that array every frame, and during a replay its output is the
    // degenerate one this file already documents as a heap of clothes. So the ease ran from garbage
    // toward the good pose and never arrived. Anything that smooths a transported pose must keep its
    // OWN copy of the previous stamp -- never read back what it wrote last frame.
    // ---- THE SKELETON LANE. Publish our merged skeleton as bone-name hashes, and apply a peer's
    // pose BY NAME instead of by index. Without it the pose is stamped index-for-index, which is
    // only correct while both characters merged the same way: a garment either side is wearing that
    // the other has not got -- or that the bone floor has substituted to keep the counts equal --
    // shifts what every index after the body MEANS, so the sender's garment bones drive ours and
    // the garment deforms wildly. Field-reported as two players in modded clothing seeing each other
    // break in the replay editor.
    // Sampling walks a runtime-created merge and resolves ~95 FNames, so it is cached and polled at
    // 2 Hz, never per frame. (An earlier note here blamed this sampler for a fatal in the renderer's
    // draw-command build; that was disproven -- the crash reproduced with this whole lane disabled.)
    bool skeletonSync = true;
    // Map bones by NAME when a fingerprint is in hand. Inert while skeletonSync is off, because no
    // fingerprint is published or applied then.
    bool nameKeyedBones = true;
};
Tuning& Tune();

struct Stats {
    uint32_t captured = 0, applied = 0, skippedCount = 0, faults = 0;
    uint32_t prefixStamps = 0;   // mismatched-count frames stamped as a prefix (see prefixOnMismatch)
    uint32_t mappedStamps = 0;   // frames stamped through a NAME map -- the correct path
    uint32_t sweeps = 0;         // slice sets that COMPLETED: without these a pose is never usable
    uint32_t wiped = 0;          // a pose-less frame cleared a pose that had completed
    uint32_t noSlice = 0;        // pose frames that carried no slice for us at all
    uint8_t  liveN = 0;          // the newest slot's usable bone count (0 = nothing to stamp)
    uint8_t  sliceBones = 0;     // bones in the last slice: skeleton/this = frames per refresh
    uint8_t  mappedBones  = 0;   // how many of the local skeleton's bones that map resolves
    uint8_t  unmappedBones = 0;  // ...and how many the sender simply does not have
    uint32_t hookCalls = 0, noted = 0, stale = 0;
    // held        = frames a proxy's own finished pose was snapshotted (normal play)
    // holdApplied = frames that snapshot was re-stamped during a local replay
    uint32_t held = 0, holdApplied = 0;
    uint8_t  bones = 0, meshBones = 0;
};
Stats GetStats();

// ---- sender: fill s.poseN/poseRot/posePos from this mesh's finished pose. Returns false (and leaves
// the pose empty) whenever the driver path should be used instead. Fires ONLY while the local player
// is scrubbing -- that state is broadcast to everyone, and everyone needs the pose then.
bool Capture(void* mesh, repl::State& s);
// Ungated capture off the pawn, for the packet the session unicasts to a SCRUBBING peer alone. Their
// replay editor cannot evaluate our drivers, so they get results -- while everyone else keeps the
// driver lane and never pays for somebody else's replay session.
bool CaptureFromPawn(void* pawn, repl::State& s);

// ---- receiver: remember a proxy's transported pose, keyed by the mesh that must wear it.
// The peer whose pose this mesh wears, and what their bones are called. Fed by the session when a
// fingerprint arrives; the remote-index -> local-index map is built lazily at stamp time, where the
// local mesh (and therefore ITS names) is finally in hand. Passing n = 0 forgets it. False = it
// could not be stored (no free slot), and the caller MUST retry rather than marking the peer fed:
// a fingerprint normally arrives before this mesh has ever carried a pose.
bool SetPeerSkeleton(void* mesh, const uint32_t* hashes, int n);
void Note(void* mesh, const repl::State& s, uint64_t nowMs);
void Forget(void* mesh);
// Called from the FinalizeBoneTransform pre-hook for EVERY skeletal mesh in the game; cheap and
// silent unless the mesh is a proxy we have a fresh pose for.
void OnFinalizeBones(void* mesh, uint64_t nowMs);
// A live proxy mesh for diagnostics, or null. The pose slots track meshes directly.
void* FirstProxyMesh();

}}} // namespace omp::game::pose
