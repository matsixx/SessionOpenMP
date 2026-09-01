# When Session updates

What breaks, what does not, and the order to fix it in. Written while nothing was broken, because
the day it happens is the wrong day to work out the method.

---

## 0. Before anything else: archive the new binaries

```
F:\SessionOpenMP\exe&pdb backups\<version>\
```

Epic ships the game's **full 859 MB PDB** next to the executable and **overwrites it on every
update**. Once the patch has landed, a copy of the old pair is the only way to map an old address to
a symbol -- which is what makes diffing old against new possible instead of re-deriving the whole
contact surface by hand. That copy cannot be made after the fact.

Steam ships **no PDB** and is a **separate build** (different link stamp and image size from Epic's
on the same version), which is why `sigbridge.py` exists: it names a Steam address by finding the
same function in the Epic exe.

`exe&pdb backups\MANIFEST.txt` records what each archived set is, with hashes.

---

## 1. Find out how bad it is, before touching any code

Two commands. Neither needs the game running, and between them they cover the entire contact
surface: **158 signatures and 493 offsets, across both mods.**

```
build\Release\omp_symcheck.exe
```
Every byte signature, against both executables, from disk. Reports `NOT FOUND` (the function
changed) separately from `AMBIGUOUS` (the signature now matches more than one place, which is the
dangerous one -- it will resolve, to the wrong function).

```
python tools\offcheck\offcheck.py
```
Every mapped offset against the new PDB -- reporting `MOVED` (the field is at a different offset
now), `MEMBER GONE`, `CLASS GONE` and `SIZE CHANGED` (a struct grew, so every stride derived from it
is now wrong) -- **and SessionTweaks' own 69
signatures**, which `omp_symcheck` never sees: it reads the `kSigs` table in `game_syms.cpp`, and
the tweaks modules keep their patterns as bare string literals beside the code that uses them.

The signature half needs only the executables, so it still runs when the PDB is missing.

**What they do not name is still correct and does not need looking at.** That is the entire value of
running them first: an update that moves six things becomes six jobs instead of an audit.

Both are offline and take seconds. Run them before forming any theory about what broke.

---

## 2. The three failure modes, in the order they hurt

**Signatures that no longer resolve** are the loudest and the least dangerous (for OpenMP's table;
SessionTweaks' copies mostly just go inert, each module logging its own miss). The symbol table logs
each one by name at startup, and the feature that needed it disables itself
(`*** PROXIES DISABLED -- spawn path needs: X`). Re-cut with:

```
python F:\SessionMPDev\tools\sigmake.py 0x<epic-rva> <bytes>
```

It wildcards branch and RIP-relative displacements -- exactly the bytes that differ between the Epic
and Steam builds -- and **refuses to bless a signature that is not unique in both**. Find the new RVA
with `pdbsym.py name <Symbol>`. Never ship a signature `sigmake` called ambiguous; lengthen it until
it is 1-hit in both, and if it cannot get there, say why in the table comment (see `MenuBackAction`,
which needs 76 bytes because it has a byte twin).

**Offsets that moved** are silent, and they are what actually crashes players. Nothing about a wrong
offset announces itself: the mod reads a neighbouring field and behaves strangely, or reads a pointer
that is not one and dies somewhere unrelated. `offcheck` is the only thing standing between a moved
field and a bug report with no useful information in it. Fix with `pdbmembers.py <Class>` and update
both the header and `tools\offcheck\offsets.map`.

**Behaviour that changed underneath a correct address** is the one no tool catches: the function is
still there and still called, but it now does something slightly different, or the event ordering
around it moved. Nothing to do but test. The gates get you to the point where this is the only thing
left, which is the best any static check can do.

### Re-verify the known-ambiguous signatures BY HAND

`tools/offcheck/sigs.expect` lists signatures that match more than once and are used anyway, because
the first match was measured to be the wanted one. **Every entry is a bet on two twins keeping their
relative order in the binary** -- something an update can flip without changing a byte of either
function. Nothing would look wrong: the mod would simply call the other twin.

Today that is `SIG_ADDFORCE`, where `FBodyInstance::AddForce` and `::AddTorqueInRadians` have
byte-identical prologues. Re-check each entry with `pdbsym.py addr <rva>` on both matches before
trusting a green run.

---

## 3. Growing the offset coverage

423 of the 493 are mapped and verified. 35 more are recorded as `-` -- a stride, an enum value, a
vtable slot -- and the remaining 35 are unmapped **with the reason written into `offsets.map`**, so
nobody re-derives the same dead end. **An unmapped offset is unchecked, not proven correct.**

```
python tools\offcheck\offcheck.py --discover
```

proposes map lines, and is **deliberately strict**: it only proposes what a comment already names
and the PDB confirms. The obvious looser version -- take any class-looking word from the comment,
keep whichever has some member at that offset -- writes confident nonsense. It proposed
`UMenuPage::_pageItemWidgets` for the container's `_menuPage` because both sit at `0x2a0`, and
`FName::ComparisonIndex` for anything at offset 0.

**A map entry that agrees with the PDB for the wrong reason is worse than no entry**, because it
will still agree after the field it was supposed to be watching has moved. The class an offset
belongs to lives in the code that dereferences it, not in a pattern match -- so the rest are for a
human, and `-` is a real answer for a stride, an enum value or an index.

`offsets.map` also records the ones deliberately left unmapped, with the reason, so nobody
re-guesses them and gets the plausible wrong answer a second time.

---

## 4. What survives an update untouched

Worth knowing, so the panic is proportionate:

- **Everything resolved by signature survives a code patch** unless that specific function changed.
  The mod holds no absolute addresses; a patch that shifts every function in the binary costs
  nothing on its own.
- **Every offset in an unmodified class survives.** Layout only moves for classes that gained,
  lost or reordered a member.
- **Engine-level offsets** (`AActor`, `USceneComponent`, `USkeletalMeshComponent`) only move on an
  engine version change, which is a different-sized problem entirely.
- **Every signature in the table already survives two independent compilations** -- Epic and Steam
  are separate builds and each signature is 1-hit in both. That is real evidence they are anchored
  on something stable rather than fitted to one binary.

A **content patch** (a map, assets) typically breaks nothing at all. A **code patch** breaks a
handful. An **engine version bump** is a rewrite, and no preparation helps with that.

---

## 5. Order of operations

1. Archive the new exe + PDB. **First**, before the old one is gone.
2. `omp_symcheck` and `offcheck.py`. Write down what they name.
3. Re-cut only that. Do not audit anything they did not name.
4. Build, run all five gates, deploy to both installs.
5. Test in-game. What remains is behavioural, and only playing finds it.
6. Bump the version, and say plainly in the release notes which game build it is for.

---

## 6. Tooling reference

In `F:\SessionMPDev\tools\` (they read the Epic install's PDB; they fail while a game is running):

| Tool | What it answers |
|---|---|
| `pdbsym.py name\|addr\|grep` | symbol <-> RVA, both directions |
| `pdbmembers.py <Class> [0xoff]` | exact member layout, or what lives at an offset |
| `xref.py <rva>` | who calls this |
| `disasm.py <rva>` | what it does |
| `sigmake.py <rva> <n>` | build a signature, verified unique in **both** exes |
| `sigbridge.py <steam-rva>` | name a Steam address via the Epic PDB |
| `mdmp.py` | crash dumps (`%LOCALAPPDATA%\SessionGame\Saved\Crashes\`) |

In this repo, `tools\offcheck\offcheck.py` -- the offset half of the same job.

**Decode the crash in the exe that produced it.** Epic and Steam RVAs are not interchangeable, and
reading one build's address in the other's disassembly produces a confident, wrong answer.
