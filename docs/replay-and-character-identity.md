# Replay and character identity

Notes from the SessionTweaks cloth work (2026-08-17) that turned out to be about **replay**, not cloth.
Written for whoever is working on replay sync in SessionOpenMP.

## The one rule

**The replay system keeps its own copy of a character and moves per-bone data between that copy and the
live one. It assumes the two are identical. Anything that changes one and not the other corrupts it.**

Corruption here does not announce itself. It surfaced as access violations reading `-1` inside
`USkinnedMeshComponent::ComputeMinLOD`, in garbage collection walking a `UMaterialInstance`, in the D3D12
descriptor cache during a draw, and in `FMallocBinned2` freeing an unrelated Slate string — all minutes
later, on worker threads, nowhere near the code responsible. If replay-related crashes look random and
land in unrelated engine code, suspect a divergence between a character and its replay copy.

## Why a character can silently differ

Session dresses a character with `FSkeletalMeshMerge::DoMerge`, fusing the body and every worn garment
into one skeletal mesh. Two consequences that matter:

1. **The merged skeleton is the UNION of the sources' bones.** A garment weighted only to the character's
   own bones adds none. A garment carrying bones of its own (custom/replacement items do this — one
   measured example added 25, making the character 95 bones instead of 70) *changes the character*.
2. **Therefore, changing what goes into the merge changes the character's bone count.** We were removing
   a garment from the merge for the local player only. The live character became 70 bones while the
   replay's copy — dressed through the same path but not intercepted, because our hook is gated on "is
   this the local player" — stayed 95. Replay then moved per-bone data between them.

Measured directly: `[cloth] the merged character has N bones` logged 95 with the garment merged and 70
with it stripped.

## What fixed it, and the transferable principle

Stop modifying the character. The garment is now **left in the merge exactly as the game built it**, then
**hidden on the body** (`ShowMaterialSection`, per-component, through the engine's own call so the shared
mesh is untouched) and drawn from a separate component instead.

The principle for any per-character feature, cosmetic or otherwise:

- **Overlay, don't modify.** Adding or hiding things on a *component* is per-instance and safe. Changing
  what the character is *built from* is global to that character and will diverge from its replay copy.
- **If you must modify, modify every copy identically** — including ones you did not create and do not
  control. Our identity gate (`t_dressing == local skater`) is exactly what made this asymmetric.
- **Per-component state does not carry.** A hide applied to one body component must be re-applied when a
  new character/component appears — including the replay's.

## Other replay facts worth knowing

- **The replay's characters go through the same dress path.** Our merge hook sees them and classifies
  them `another skater`; that is how they can be detected without any replay-specific API.
- **Input keeps ticking inside the replay editor** — that is how scrubbing works. Anything driven from an
  input-tick hook runs *in addition to* whatever `AReplayManager::Tick` drives, so per-frame work can run
  twice per frame in the editor. Worth checking for any per-frame replication or capture code.
- **Runtime-created UObjects must be GC-rooted or replay will outlive them.** `RF_Public|RF_Standalone`
  do NOT protect an object in a cooked build. The only real protection is `EInternalObjectFlags::RootSet`
  on the object's `FUObjectItem` in `GUObjectArray`. Reaching that: signature the *code* that reads the
  array (a data symbol cannot be signature-matched) — `APedestrianCharacter::InitCharacterVisuals` holds
  the standard index→item lookup, and scanning its first bytes for `48 8B 05 <disp32>` yields
  `&GUObjectArray.ObjObjects`. Layouts (from the PDB): `FUObjectArray::ObjObjects +0x10`;
  `FChunkedFixedUObjectArray{Objects +0x00, MaxElements +0x10, MaxChunks +0x18}`;
  `FUObjectItem{Object +0x00, Flags +0x08}` stride 24; `UObjectBase::InternalIndex +0x0c`;
  `RootSet = 1<<30`. Validate by round-trip before writing. Working implementation:
  `RootObject()` in `src/tweaks/cloth_merge.cpp`.

## Debugging method that actually worked

Four speculative fixes failed before any measurement was taken. What cracked it:

1. **Install the mod into the EPIC copy of the game** — it ships `SessionGame-Win64-Shipping.pdb`, so
   fatal errors arrive with function names and line numbers instead of raw addresses. Do this first.
2. For Steam addresses, bridge them: `F:\SessionMPDev\tools\sigbridge.py <steam-rva>` →
   `pdbsym.py addr <epic-rva>`. Module base comes from the log (an absolute hook address minus its
   logged `exe+0x…`).
3. **Catch the fault address yourself** with an SEH filter capturing `ExceptionAddress`, then name it the
   same way. That is how a null `Skeleton` field and a null `ForceSectionMapping` were each found in one
   round instead of by guessing.
4. **Log the exact fields the crashing engine function reads**, at build time and at use time, and diff
   them. A crash in GC or the renderer names the victim, never the culprit.
