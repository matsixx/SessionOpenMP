// PEER BODY TRIM -- what a wire-driven proxy actually pays for its physical animation.
//
// Switching the game's physical animation on for a peer (Proxy::Apply, the syncPhysAnim block) hands
// the engine that skater's whole PhysicsAsset: about 21 simulated bodies, plus a kinematic target
// body and a motor joint for each, solved and collided against the level every physics tick, for
// every peer in the lobby. It is the largest thing this mod asks of the CPU, and most of it cannot
// be seen. The game's own blueprint zeroes the blend weight on nearly every body while riding -- the
// bodies are still simulated, the result is simply thrown away -- so cutting them back costs far
// less on screen than it saves.
//
// Three cuts, each undone with the exact values that were read:
//   * the legs and pelvis ride the animation (kinematic) instead of the solver
//   * the bodies that remain stop colliding with the level
//   * their solver iteration counts are divided down
//
// EVERY CUT IS SCOPED TO THE WINDOW WHERE PHYSICAL ANIMATION IS ON, and the restore is not optional.
// A proxy can still ragdoll for real: a bail that arrives without the owner's skeleton attached is
// played by the game's own Bail (proxy.cpp), on these same bodies -- and a ragdoll with no level
// collision falls through the floor. So the caller must run the restore on its own every frame
// rather than from inside the throttled enable/disable block, whose 500 ms guard would otherwise
// swallow it on a bail that lands just after the enable.
//
// Nothing here may ever be pointed at the local skater. It is for proxies only.
#pragma once

#include <cstdint>

namespace omp::game {

// Live knobs. There is no menu row: the "Peer body physics" toggle (Multiplayer -> Other options)
// already governs the whole feature, and these exist to A/B the three cuts against each other.
extern bool trimPeerLegs;        // legs and pelvis kinematic instead of simulated
extern bool trimPeerWorldHits;   // the remaining bodies ignore the level
extern int  trimPeerIterDiv;     // solver iterations divided by this (1 = leave them alone)

using TrimLogFn = void (*)(const char*);

// Apply the cuts to this proxy's mesh, and hold them. Cheap to call every frame: after the first
// pass it is two pointer reads until the re-check timer comes round, and the re-check only writes
// to a body that has drifted back.
void PeerBodiesTrim(void* meshComp, uint64_t nowMs, TrimLogFn logf);

// Put everything back. A no-op on a mesh that was never trimmed, so it is safe to call every frame.
void PeerBodiesRestore(void* meshComp, TrimLogFn logf);

// The actor died with its world: drop what is remembered about this mesh WITHOUT touching any of it.
void PeerBodiesForget(void* meshComp);

// For the log line: how many meshes are currently trimmed.
int PeerBodiesTrimmedCount();

}  // namespace omp::game
