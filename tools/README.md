# tools

Offline harnesses and build scripts. The five gates below run without the game, without a second
player and without a network, and **all five must pass before a change ships**.

Build them with the rest of the project (`cmake --build build --config Release`); they land in
`build\Release\`. None of them takes arguments except `omp_symcheck`. `offcheck` is the exception
to all of that: it is a Python tool, because it reads the game's PDB.

Between them, `omp_symcheck` and `offcheck` cover the **entire** game contact surface of **both
mods** -- 158 signatures and ~490 offsets -- which is what makes a game update a short list of jobs
instead of an audit. See [../docs/game-update.md](../docs/game-update.md).

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

### `offcheck` — the other half of the contact surface

```
python tools\offcheck\offcheck.py              # verify
python tools\offcheck\offcheck.py --discover   # propose new map entries
python tools\offcheck\offcheck.py --pdb <exe>  # a different build
```

Covers everything `omp_symcheck` does not:

- Every **struct offset** against the game's shipped PDB — the 188 constants in `off::` and the 305
  bare enum constants scattered through `src/tweaks`. **423 verified**, 35 recorded as `-` (a stride,
  an enum value, a vtable slot — not a field), 35 left unmapped with the reason written down.
- **Sizes and strides** via `sizeof:Class`. Worth checking even where members are not: a struct that
  gains a field changes its stride, and iterating an array with a stale one reads garbage from the
  second element onward.
- Whether a constant declared in more than one tweaks module (26 of them are) **agrees with itself**
  — a disagreement there is a live bug, not just an update risk.
- **SessionTweaks' own 69 byte signatures.** `omp_symcheck` reads the `kSigs` table in
  `game_syms.cpp` and nothing else, so it has never seen these; the tweaks modules keep their
  patterns as bare string literals next to the code that uses them. Each must resolve **1-hit in
  every executable present**; `sigs.expect` records the ones deliberately used despite matching
  twice, and those are order-dependent and need re-checking by hand after an update. This half needs
  only the exes, so it runs even when the PDB is missing.

Why it exists: a broken signature is loud (the symbol table names it and the feature disables
itself), but **a moved offset is silent** — the mod reads a neighbouring field, and the first anyone
hears is a crash with nothing useful attached.

It reads `game_syms.h` and never writes to it; the expected symbol for each constant lives in
`offsets.map`, so none of this tooling can affect how the mod behaves. **Unmapped offsets are
unchecked, not proven correct** — the number is there to grow. `--discover` is deliberately strict
and only proposes what a comment already names and the PDB confirms: a map entry that agrees for the
wrong reason is worse than no entry, because it keeps agreeing after the real field has moved.

Not wired into the build: it needs the PDB, which only the Epic install ships.

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
