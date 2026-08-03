# tools

Offline harnesses and build scripts. The four gates below run without the game, without a second
player and without a network, and **all four must pass before a change ships**.

Build them with the rest of the project (`cmake --build build --config Release`); they land in
`build\Release\`. None of them takes arguments except `omp_symcheck`.

## The gates

### `omp_symcheck` — the game contact surface

Scans the shipped game executables on disk and verifies that **every byte signature resolves to
exactly one address** in each. Zero matches is a failure; two matches is a worse failure, and it
catches that too.

```
omp_symcheck                      # both known install paths
omp_symcheck <path-to-exe> [...]  # explicit
```

With no arguments it checks the default Epic and Steam install paths; an executable it cannot find
is reported as `SKIP` rather than failing. Run this after **any** change to `src/game/game_syms.cpp`
— the signature table and its assignment block are positional, and this is the only thing that
catches a misalignment.

### `omp_looptest` — replication core

Simulates a skater at 60 Hz through five network profiles (clean, internet, relayed, 5% loss,
bursty) and judges the prediction residual, then round-trips every wire structure through the codec
and compares field by field. Catches clock drift, interpolation errors, and any encode/decode
mismatch — including fields that were added to a struct but forgotten in the codec.

### `omp_sessiontest` — session lifecycle

Headless test of peers arriving, going quiet, timing out and leaving, with the transport mocked out.

### `omp_shmtest` / `omp_udptest` — real two-process transport

Each **relaunches itself** as a second process and talks to itself over the real transport — shared
memory, or UDP on loopback port 47811. One person can run them alone.

Do not pass arguments: `child` is the internal re-entry token. Both write
`omp_*test_parent.log` / `omp_*test_child.log` into the current working directory, so run them from
somewhere you do not mind that.

## Other

- **`build/gen_vs.ps1`** — configures the CMake `vs2022` preset and prints the path to the generated
  `build\SessionOpenMP.sln`.
- **`dist/refresh_dist.ps1`** — rebuilds the release zip from the last one plus freshly built DLLs.
  It refreshes shipped data from the repository, prunes `Mods\` to an allowlist, regenerates
  `mods.txt`, and **refuses to write a zip** that is missing any licence or notice file.
- **`ping/`** (`omp_ping`) — a standalone connectivity test that proves the EOS link works without
  involving the game at all. Two players run it, exchange ids, and it reports each direction
  separately plus whether EOS routed the connection directly or via relay.
