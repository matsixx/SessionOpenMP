# Architecture

How SessionOpenMP turns N independent solo games into one shared session, and the rules that
machinery has to obey.

---

## The overlay model

There is no server. There is no host authority. Every player runs a normal, unmodified-in-principle
solo game, and the mod paints the other players into it.

- Your game is authoritative for exactly one skater: **yours**.
- Every other player is a **locally spawned, non-replicated actor** of the game's own skater class.
- That actor is driven **entirely** from the wire. Nothing about it is computed locally.

Because both machines run the same code with the same role, there is no client/server asymmetry
anywhere: no `Role` branches, no Unreal replication, no relevancy or dormancy, no net smoothing, no
"the host does X but a client does Y". A bug reproduces identically on both ends.

Remote players are real game-class actors rather than skeletal-mesh puppets. That is what buys
player-versus-player collision and the game's own audio behaviour.

```
   your game                         their game
 ┌────────────┐                    ┌────────────┐
 │ your skater│──gather──┐   ┌─────│ your proxy │
 │  (real)    │          │   │     │            │
 │            │          ▼   │     │            │
 │ their proxy│◄──apply──[transport]──apply────►│ their skater
 └────────────┘                    └────────────┘
```

The four layers, bottom up:

| Layer | Directory | Job |
|---|---|---|
| Transport | `src/transport/` | Move opaque bytes between peers. EOS, UDP or shared memory behind one seam. |
| Replication | `src/replication/` | The snapshot format, the per-peer ring buffer, the playback clock. Game-free and unit-tested. |
| Game | `src/game/` | Read the local skater; drive proxy actors; cosmetics, audio, pose. All the reverse-engineered game contact. |
| Session | `src/session/` | Who is in the session, peer lifecycle, routing snapshots to proxies. |

`src/loader/` is the UE4SS entry point and the per-frame anchor; `src/ui/` is the F1 overlay, the
in-game pause-menu integration and chat.

---

## The wire

A snapshot is **self-contained**: it carries everything needed to render one instant of one remote
skater, with no dependency on a previous packet. A lost packet costs one frame of freshness, never
desynchronisation.

Compression is by construction rather than by a general-purpose compressor:

- Rotations use **smallest-three** quaternion encoding (4 bytes).
- Positions and most scalars are **16-bit floats**.
- Fields that have not changed are **zero-suppressed** behind presence masks.
- The pose lane (a full skeleton) is sent **only** when it is the only thing that can work — see
  below.

Each packet begins with a four-character magic identifying the wire version, so a peer on a different
build is rejected rather than allowed to misparse. That rejection is otherwise invisible — the lobby
join and the P2P link both succeed regardless of version, and only the snapshots fail, so the symptom
is a player who connects and then never appears. Two things make it legible instead: the host's
version rides the lobby advertisement, so the browser can flag a mismatch *before* anyone joins, and
the first unreadable packet from a peer announces itself once (`Config::onVersionMismatch`).

### Drivers, not results

The default is to transport the **drivers** of an animation — the trick definition, the push state,
the grounded flag — and let the receiver's own animation graph derive the pose. That is deliberate:
foot IK adapts to local geometry, and drivers extrapolate cleanly, so a dropped packet leaves a
running skater still running instead of freezing mid-stride.

Audio is the exception, and it does not come from the animation at all. A proxy's own anim-notify
sounds are **muted**, and every sound a remote player makes is captured at the sender's own
sound-spawn funnel and re-issued here. The reason is that the gameplay handlers which start those
sounds interrogate the movement component for grind type, powerslide fullness and impact context —
and a proxy's movement component is not simulating, so it answers "nothing" and they fall silent.
Only the machine that was actually simulating knows what it played.

It has exactly one failure mode: the **replay editor**. While a player scrubs a replay there are no
drivers — the pose being played exists only in the skeleton. So there is a second, narrowly gated
lane that transports the skeleton itself, active only in that state.

---

## Time

One playback clock per peer, slewed against a **filtered** error rather than the raw per-packet
sawtooth. Snapshots are buffered and rendered slightly in the past, so ordinary jitter is absorbed
by the buffer instead of by the skater's position.

- Network jitter is measured as `|localDelta - remoteDelta|` with a deadband the size of the poll
  quantum, and it is asymmetric — it must rise fast and fall slowly.
- A burst beyond the rate cap is not smoothed, it is **snapped** (threshold 150 ms). Trying to
  interpolate through a 500 ms gap produces a skater skating through walls.
- Extrapolation is **position-only**, and bounded in both time and distance. Pose is never guessed.

---

## The rules

Every one of these was a shipped bug. They are stated here because they are not obvious from
reading the code that obeys them.

1. **Quaternions end to end. No euler round-trips in any transport or write path.**
   The extract/rebuild pair is not self-inverse: a pure yaw-180 quaternion extracts as
   (yaw 180, roll 180), and rebuilding that lands upside-down. Any place a rotation is decomposed
   and recomposed is a bug waiting for the player to face a particular direction.

2. **Pointer identities never travel.** A pointer is meaningless in another process, and so is an
   `FName` index — those are per-process. Transport a **name string** (resolved with
   `StaticFindObject`) or an **index into a shared asset**. Never an address, never an offset into a
   heap-allocated array.

3. **Per-frame recomputed values must be applied *after* the game's writer.** The animation apply
   point is a **post**-hook on `NativeUpdateAnimation`; anything written before the game's own
   recomputation is silently overwritten and renders as nothing.

4. **Asset gates: a transported ratio or flag whose driving asset is null on the receiver is forced
   to zero.** A null blend node renders as a T-pose. Opening such a gate means *delivering the
   asset*, not fighting the gate.

5. **Request bits are events, not state.** Some flags pulse within a single frame and cannot be
   polled at any sample rate. Hook the setter, or find the persistent state behind the pulse. And
   never assign a state byte wholesale — mask in only the bits you own.

6. **A proxy's self-driving logic is disabled at link time** (skater actor tick, board actor tick).
   Any second writer fights the transported pose, and the visible result is a board that tumbles or
   jitters.

7. **Spawn only from the engine-tick anchor.** Spawning from inside script dispatch corrupts the
   asset linker in ways that surface much later as an unrelated crash. Install the rename guard
   before any spawn, and disable replication on the new actor in the same frame. Structured
   exception handling around engine code is not recovery — if something faults there, fail loudly
   and permanently rather than continuing in an unknown state.

8. **One clock per subtraction.** Mixing two time sources in one subtraction produces enormous
   garbage values (unsigned underflow). Publishing clocks phase-accumulate; they never do
   `last = now`, which silently drops the remainder every tick.

9. **Instruments are edge-triggered, not interval-sampled.** A 2-second sampler misses a 100 ms
   event entirely. Label wire values separately from applied values — conflating them hides the
   bug being chased. Readiness checks enumerate their dependencies, and any permanent early-out
   announces itself once so "nothing happened" is never silent.

10. **Byte signatures: never wildcard a displacement — it *is* the identity.** The signature table
    in `game_syms.cpp` and its assignment block are **positional**: inserting a row requires
    inserting the matching assignment at the same index. `omp_symcheck` is the guard, and it must
    pass against both the Epic and Steam builds with exactly one hit per signature.

11. **The packed animation blob is a field *sequence*, never a struct.** Indexing it by a
    source-struct offset reads the wrong bytes. Go through the field table in `anim_fields.h`.

12. **Never transport a blend alpha without its target.** A weight that arrives on time while the
    thing it weights does not is worse than sending neither.

---

## Testing without the game

Four harnesses in `tools/` run on the command line, with no game and no second player, and all four
must pass before a change ships. They exist because the alternative — two people, two machines, and
a guess — is not a test. See `tools/README.md`.

The important one to understand is `omp_symcheck`: it scans both shipped game executables on disk
and verifies that every byte signature resolves to exactly one address in each. A signature that
matches zero places fails loudly; one that matches two is worse, and it catches that too.

---

## Reverse engineering

The mod talks to a shipped, optimised game binary. Function addresses come from byte signatures
built from the game's own code; struct offsets come from the shipped PDB where one exists and from
disassembly where it does not.

Two habits that repeatedly turn out to matter:

- **Diagnose from what the consumer reads, not from a field's name or a writer's store.** An offset
  derived from where a setter writes is a guess about where the value lives. Reading through the
  game's own accessor is the value the game itself uses.
- **Measure the caller.** Identify which overload or call site you are in from the return address
  and from what the function body actually reads — not from what a disassembled call site appears to
  pass. On MSVC x64 several distinct types are passed identically.
