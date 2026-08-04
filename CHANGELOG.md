# Changelog

## SessionOpenMP

### 0.7.0b
- **Player nameplates.** Each peer's name floats above their skater, drawn from the mod's own overlay
  so it needs no widget blueprint or font asset from the game. Only other players get a plate.
- **Chat appears over the speaker** as a speech bubble, keyed by connection rather than by name, so a
  message can never end up attached to the wrong person.
- **Version mismatches are now visible.** A host advertises which version they are running, so the
  session browser warns you *before* you join rather than leaving you in a session where the other
  player never appears. If a mismatched packet does arrive, the chat box says so once.
- **Fixed: the browser kept showing the old host's name** after they left and the session passed to
  someone else. The new host now re-advertises, including when the service is briefly slower to agree
  that they are the host than their own game is.
- **A self-service updater** (`update.bat`) ships in the package: it checks GitHub for a newer
  release, installs it, and keeps your settings and your enabled/disabled mods.
- **SessionTweaks writes its settings file on first run**, so it can be configured when installed on
  its own without the multiplayer mod (and therefore without its menus).
- Added a read-only probe for the grind-exit pop, to settle whether it is limited by per-trick data
  or by the crank-ratio formula. It changes nothing in game.

### 0.6.0b
- First open-source release.
- Builds from a clean clone: Dear ImGui and MinHook are fetched at pinned revisions instead of being
  read out of a local path, and missing dependencies now fail the configure step rather than silently
  producing a mod with no menu, chat or pause-menu integration.
- EOS credentials are generated from the template when absent, so the project compiles without them.
  The UDP and shared-memory backends work fully; the EOS backend reports what to fill in.
- `cmake --preset vs2022` generates the Visual Studio solution.
- Removed two superseded audio paths (gameplay-handler sound replay, and the local board-audio tick)
  along with the 15 byte signatures only they used. All audio comes from the transported sound funnel.
- Diagnostics consolidated behind `omp::debug` (`src/debug.h`), all off by default. Menu page dumping
  is no longer on by default.
- Documentation added: README, architecture notes with the project's standing rules, and tooling docs.

### 0.5.1b and earlier
Pre-release test builds. Development history is in the project's design notes rather than here.

## SessionTweaks

Gameplay fixes and quality-of-life changes for singleplayer Session. Ships as its own UE4SS mod and
runs with or without SessionOpenMP.

### 2.11.3
- Catch levelling no longer disables itself. The sanity check that guards against a bad pointer used
  to fire on any single all-zero read of the pitch block — but a run-out legitimately resets the
  board state, so the next catch frame reads all zeros for real. A wrong pointer reads zero *always*,
  so the test is now "never read live across six catches" rather than "zero this frame".
- Runtime faults (a gate, an SEH handler, a signature missing after a game patch) can no longer touch
  the persisted enable flag. A transient hiccup was being saved as a permanent setting.

### 2.11.2
- Pause-menu page gains "Level board on catch" and a catch-sound volume slider. The page is now
  exactly at its item limit, enforced by a `static_assert` — the host truncates the tail silently, so
  overflowing it would have deleted the "Reset to defaults" row instead of the new one.

### 2.11.1
- Removed the catch-sound blacklist override. The blacklisted tricks are all ollies and nollies —
  no flip means nothing to catch, so the silence is deliberate. Overriding it only added catch sounds
  where they were not wanted.
- One sound per catch. Where the notify does fire it lands 250–300 ms after the catch, past both the
  self-play grace period and the correlation window, producing a double sound. A late notify is now
  denied its cue rather than skipped — the same function also stops the in-air flip whoosh, and
  returning early would silently change that.

### 2.11.0
- Catch sound plays reliably (self-play).

### 2.10.x
- Catch sound investigated and implemented. The sound is an anim notify gated by
  `ASkaterCharacterBase::ShouldPlayCatchAudio`. The per-animation volume multiplier is floored, and
  the one-shot is attached to the skater rather than the fixed world point the stock notify uses.

### 2.9.x
- Catch levelling added (default off). The pitch component is derived from
  `SetBoardPitchExtraAngle`'s own `this` rather than guessed from a struct offset, and the sanity
  gate compares with an epsilon.
- The grind-pitch module was removed: six field rounds disproved every theory behind it. The
  remaining lead is that it is a pop bug rather than a pitch bug.

### 2.8.x
- Run-out direction fixed. `UMovementComponent::Velocity` is frozen while skating — it holds the
  vector from mount time — so both momentum and facing now come from position-derived travel.
- F1 settings persist: live values are spliced back into the ini with comments intact.

### 2.7.x
- The darkslide fix is angle-gated: the reservation is honoured only near grip-down, and elsewhere
  the press resolves natively at press time.
- Grip angle is read from the rendered deck (the flipper component's world quaternion) because
  `_boardFlipCurrentAngle` reads zero.
- Run-out momentum heading comes from 0.2 s of velocity history, so an impact-deflected instant
  vector cannot send the skater in a random direction.
- The post-bail facing rotation is queued and applied on the next input tick under its own guard,
  rather than run inside the Bail callstack where it faulted.

### 2.4.0 – 2.6.0
- A bad catch converts immediately to on-foot, keeping the riding momentum, with an armed fall watch
  measuring the drop from the conversion point; past the configured threshold it triggers the game's
  own bail at impact.

### 2.2.x – 2.3.0
- Run-out height gating is measured geometry (skater world-Z apex minus Z at bail, knob
  `RunOutMaxDropCm`). The engine's air-time fields all read zero at bail time, and the Big Drop
  bitfield reads a constant on flat ground, so neither can gate it.
- Catch grace reworked into flick-versus-hold: a catch is a flick, a darkslide is a hold.

### 2.0.0 – 2.1.0
- Renamed from SessionScoopFix and split into modules.
- Darkslide grace added; run-out on low-air missed-trick bails via the native on-foot transition.
