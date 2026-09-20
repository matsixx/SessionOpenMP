// Diagnostic switches, in one place.
//
// Everything here is OFF in a shipping build and costs nothing while off. These are not tuning
// knobs -- they do not change behaviour, only what gets logged -- so flipping one is always safe.
// They are plain mutable globals rather than #defines so a symptom can be investigated in a running
// game (from a debugger, or by wiring one to a menu row) without a rebuild.
//
// Instrumentation rules these follow, learned the hard way:
//   * edge-triggered, not interval-sampled -- a 2 s sample misses a 100 ms event entirely
//   * label WIRE values separately from APPLIED ones; conflating them hides the bug you are chasing
//   * a permanent early-out announces itself once, so "nothing happened" is never silent
#pragma once

namespace omp { namespace debug {

struct Flags {
    // Per-frame comparison of the local pose blob against what the replay system holds, plus a
    // per-bone quaternion diff. Only meaningful while the replay editor is open, and expensive
    // enough that it must stay off otherwise.
    bool replayPoseDiff   = false;
    // 1 Hz line naming which animation-gate assets a proxy resolved, against what the wire sent.
    // A blend node with a null asset renders as a T-pose, so "which gate is missing" is the first
    // question when a proxy animates wrongly.
    bool animGates        = false;
    // Crank (push) edge probes, sender and receiver side: one line per rising edge and per change of
    // def identity. Pushes are one-shots, so they are invisible to any sampled instrument.
    bool crankEdges       = false;
    // Per-item asset-load accounting while dressing a proxy. The dress path is proven; turn this on
    // only when an item refuses to appear.
    bool cosmeticsLoad    = false;
    // One-time dump of each game menu page's key, item count and item keys, as the page is built.
    // How you find the key of a page you want to add a row to.
    bool menuPages        = false;
    // MEASUREMENT ROUND: expose what a proxy's own anim graph produces during a local replay.
    // On the sender it stops unicasting results to scrubbing peers (drivers keep flowing); on the
    // scrubber it disables the pose stamp AND the hold, so the skeleton shows the graph's raw output.
    // The [rprobe] line proved the graph still UPDATES at 60/s while scrubbing; this answers whether
    // its EVALUATION is usable. If a peer skates live under this flag, the driver lane works in
    // replay and the pose lane can shrink to the sender side only. Both clients need it on.
    bool replayDriverTest = false;
    // Both ends log the SAME quantity every frame around a pose -- the rendered skeleton's first
    // bones, each one's yaw and how far it moved since the last frame -- from pose start to 2 s past
    // the end. Turn it on in BOTH games, do one emote, and the frame where the two logs part company
    // is the fault, named by bone and by how far it jumped. This is what found the end-of-pose twitch
    // (the viewer moved 137 degrees on one bone in one frame; the sender moved 2) after two plausible
    // fixes had missed it. A line per frame per proxy, so it stays off.
    // NOTE: on ONE PC both clients share GetTickCount64, so the two logs align exactly.
    bool poseTwitch       = false;
    // ONE LINE A SECOND: the head bone as we stamp it vs the head bone as it was actually PUBLISHED
    // (rendered). A gap between them means something writes the head after the pose does, and says by
    // how many degrees. This is what found the doubled head turn on 2026-09-20 -- it proved the
    // published pose differed from BOTH the graph and our stamp, which named a third writer.
    bool headProbe        = false;
    // The level's-own-props handover, traced leg by leg: claimed locally, published, received,
    // driving. That state machine spans two machines, and three rounds went on guessing which leg
    // was missing before it was written -- the feature is unfinished, so it will be wanted again.
    // Capped internally, so turning it on cannot flood a log.
    bool dropWorld        = false;
    // MEASUREMENT ROUND: every 2 s per skater, is the game's physical animation live -- the
    // component found and bound to the mesh, and how many bodies carry a PhysicsBlendWeight (that
    // weight IS the visible body physics). We never enable it on a proxy; whether the Blueprint does
    // so on a wire-driven skater decides if a peer's body-feel settings could be re-run locally.
    bool paProbe          = false;
    // One-shot dump of the local skater's rig -- every skinned mesh with its bones and morph targets
    // by name -- once the world settles, and again on a change of outfit while others are connected.
    // Answered "can the head do lip sync" (no: the head is one bone, no morph targets); kept for
    // the next question about what the rig carries.
    bool rigDump          = false;
};

// Header-only: one shared instance across every translation unit that includes this.
inline Flags& Get() { static Flags f; return f; }

}} // namespace omp::debug
