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
    // The level's-own-props handover, traced leg by leg: claimed locally, published, received,
    // driving. That state machine spans two machines, and three rounds went on guessing which leg
    // was missing before it was written -- the feature is unfinished, so it will be wanted again.
    // Capped internally, so turning it on cannot flood a log.
    bool dropWorld        = false;
};

// Header-only: one shared instance across every translation unit that includes this.
inline Flags& Get() { static Flags f; return f; }

}} // namespace omp::debug
