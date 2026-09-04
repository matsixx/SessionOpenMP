# SessionOpenMP

**NOTE FROM DEV: This is a vibecoded project through and through. I won't sugarcoat it. This would be an
incredibly massive undertaking for a solo developer. So, I used AI with the experience I have using it
for many other very complex projects. This took 1-2 weeks of work day and night to get it into a working state.**

**I can guarantee though, I will always do my best to deliver on a high quality experience. I am working on this
as someone with 2,000+ hours on this game. I care about this game a lot.**

**License notice: This project is licensed under GPL-3.0. You are free to fork, modify, and redistribute it, 
provided your version remains licensed under GPL-3.0 and its complete corresponding source is made available. 
Incorporating this code into a closed-source or proprietary distributed product is a license violation. 
Violations will be pursued, including DMCA takedown requests where applicable.**

Multiplayer for **Session: Skate Sim** — skate with other people in the same world, cross-play
between the Epic and Steam builds.

Every release also ships **SessionTweaks**, a companion mod with singleplayer gameplay fixes
(stick-sweep scoop speed, a wider manual-catch window, a darkslide-aware catch fix, run-out on
missed tricks, cloth physics on your clothes) and an optional **pop control scheme**: hold one
stick to crouch to any depth you like — how deep you are is how high you pop — and flick the other
to pop, in every stance. It is off until you switch it on under Pop control in the Session Tweaks
menu. All of it is built from this repository and bundled in the same package — you get both.

They are two separate DLLs rather than one because they do different jobs: SessionTweaks changes how
*your own* skating feels and works with or without multiplayer, so it can be switched off on its own
line in `mods.txt` without disturbing the session. Bundled, not merged.

**SessionTweaks on its own, without the multiplayer mod:** copy only `Mods\SessionTweaks\` out of the
package onto a UE4SS install and list it in `mods.txt`. It shares no code with SessionOpenMP — no
networking, no EOS, no account, nothing to sign into. The only thing you give up is the in-game
menus: SessionTweaks finds its UI by asking the loaded modules whether any of them hosts the menu
seam, and with SessionOpenMP absent nothing answers, so it configures itself from
`SessionTweaks.ini` instead. That file is written for you on first launch with every setting at its
default, so there is nothing to author by hand.

**[Full feature list -> FEATURES.md](FEATURES.md)** -- everything both mods do, what is a new
capability and what is a base-game defect corrected, with the version each landed in.

> Status: working and in testing. Sessions of five players have run without trouble. Expect rough
> edges, and please report them with a log.

---

## How it works

Session has no multiplayer, and this does not add a server to it. Instead every player runs an
ordinary **solo game**, and the mod overlays the others on top:

- Each player's game is authoritative for exactly one skater — their own.
- Every remote player is a **locally spawned, non-replicated actor** of the game's real skater class,
  driven entirely from data on the wire.
- Nothing about a remote player is derived locally. Position, pose, board state, cosmetics and sound
  are all transported; the receiving machine never guesses.

The consequence worth knowing: both machines run identical code. There is no host and no client, no
authority split, no Unreal replication, no relevancy or dormancy, and no "am I the server?" branch
anywhere in the mod. Because remote players are real game-class actors rather than puppets, they
collide with you and make the game's own sounds.

The wire is a self-contained quantised snapshot — smallest-three quaternions, 16-bit floats,
zero-suppression and presence masks — sent at a fixed rate and interpolated on a shared playback
clock. `docs/ARCHITECTURE.md` describes the design and the rules it has to obey.

Three transports sit behind one interface:

| Backend | Use |
|---|---|
| **EOS** | Between machines over the internet, including Epic↔Steam cross-play. |
| **UDP** | Between machines without Epic, direct or over a LAN. |
| **Shared memory** | Two games on one PC. Development and testing only. |

---

## Installing (players)

**Easy install**
1. Go to releases and download the latest .zip
2. Extract everything into SessionGame\Binaries\Win64
3. Open game and press pause or F1 to use MP and adjust Tweaks settings

**Manual Install**

1. Install [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 3.0.1 into the game.
2. Copy the mod folders into `SessionGame\Binaries\Win64\Mods\`, so you have
   `Mods\SessionOpenMP\dlls\main.dll` and `Mods\SessionTweaks\dlls\main.dll`.
3. Make sure `Mods\mods.txt` lists both:
   ```
   SessionOpenMP : 1
   SessionTweaks : 1
   ```
   Set either to `0` to disable that mod without uninstalling anything.
4. Launch the game and press **F1** for the mod menu.

The release package contains all of this laid out correctly, including the EOS runtime that the
networking needs — start there rather than assembling it by hand.

### Updating

Double-click `update.bat` in the game's `Win64` folder. It asks GitHub for the latest release,
downloads it and installs it over the existing copy, keeping your settings — `SessionOpenMP_prefs.txt`,
`SessionOpenMP_bans.txt`, `SessionTweaks.ini` and `UE4SS-settings.ini` are never overwritten, and a mod
you disabled in `Mods\mods.txt` stays disabled.

It's a readable PowerShell script rather than an executable, deliberately: it lives at
[`dist/update/update.ps1`](dist/update/update.ps1), talks to no host but GitHub, and writes nothing
outside the game folder. `update.ps1 -Check` reports without changing anything, and
`update.ps1 -ZipPath <file>` installs a package you already have.

---

## Building

Requires Visual Studio 2022 with the C++ desktop workload (MSVC + CMake).

```
cmake --preset vs2022
cmake --build build --config Release
```

That writes `build\SessionOpenMP.sln`, which you can open in Visual Studio directly — or just use
*File → Open → Folder* on the repository root and Visual Studio will pick up `CMakePresets.json`.
`tools\build\gen_vs.ps1` does the same in one command and prints where the solution landed.

Outputs:

| Target | Output | Goes to |
|---|---|---|
| `omp_mod` | `build\Release\main.dll` | `Mods\SessionOpenMP\dlls\` |
| `tweaks_mod` | `build\tweaks\Release\main.dll` | `Mods\SessionTweaks\dlls\` |

### Dependencies

**Fetched automatically** at pinned revisions — nothing to do:
Dear ImGui 1.90.9 and MinHook (both compiled directly into the mod DLL). Point `-DIMGUI_SRC=` /
`-DMINHOOK_SRC=` at local checkouts if you need to build offline.

**You must supply two things:**

- **EOS SDK 1.17.0 (C)** — download from [dev.epicgames.com](https://dev.epicgames.com/) (a free
  Epic developer account is required; the SDK cannot be redistributed as a source dependency).
  Then configure with `-DEOS_SDK=<path to its SDK folder>`, or edit the default at the top of
  `CMakeLists.txt`.
- **`third_party/ue4ss/UE4SS.lib`** — an import library generated from your installed `UE4SS.dll`.
  One is checked in; see `third_party/ue4ss/README.md` to regenerate it.

To run a session on EOS you also need your **own** EOS product credentials: copy
`src/transport/eos_creds.h.template` to `src/transport/eos_creds.h` and fill in your product,
sandbox, deployment and client values. That file is deliberately gitignored. The UDP and
shared-memory backends need none of this.

### Tests

Six offline gates run without the game and must all pass before a change ships. See
`tools/README.md`.

```
build\Release\omp_symcheck.exe      # every byte signature resolves, uniquely, in both game builds
build\Release\omp_looptest.exe      # clock, interpolation and wire codec under simulated network loss
build\Release\omp_sessiontest.exe   # session lifecycle: peers joining, going quiet, leaving
build\Release\omp_shmtest.exe       # two real processes over the shared-memory transport
build\Release\omp_udptest.exe       # two real processes over loopback UDP
build\Release\omp_relaytest.exe     # a relay server and three clients, including the amplification rule
```

`tools/offcheck/offcheck.py` additionally verifies every struct offset and byte signature in the
source against the game's shipped PDB.

The shm and udp gates each run two real processes against a named section or a loopback port, so
back-to-back runs can overlap and report a false failure; re-run one on its own before believing it.

---

## Licence

SessionOpenMP is free software, licensed under the **GNU General Public License, version 3 or
later** — see [LICENSE](LICENSE).

It carries one **additional permission under GPL section 7** ([LICENSE-EXCEPTION.txt](LICENSE-EXCEPTION.txt)):
you may link and convey this work combined with the Epic Online Services SDK and with the
proprietary game runtime it loads into. Without that permission the licences would be incompatible
and nobody — including the author — could distribute the result at all. The copyleft on everything
in `src/` is unaffected, and a build using only the UDP or shared-memory transport is plain GPLv3
and needs no exception.

Third-party components and their licences are listed in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt); Epic's own notice for software inside the EOS
SDK is in [EOS-ThirdPartySoftwareNotice.txt](EOS-ThirdPartySoftwareNotice.txt). Both must travel with
any binary distribution.

**Written offer (GPL section 6b):** for any binary release of this project, the complete
corresponding source code is this repository — <https://github.com/matsixx/SessionOpenMP> — at the
tag matching that release. If you received a binary and cannot obtain the source here, open an issue
and it will be provided.

*Session: Skate Sim* is the property of Crea-ture Studios. This project is an unofficial,
independently developed modification, is not affiliated with or endorsed by them, and contains no
game code or assets.
