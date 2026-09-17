# Modding API

Your mod can send its own data to the other players in a SessionOpenMP session. OpenMP carries the
bytes over the connection players already have and never reads them.

You need this because there is no Unreal network connection between players. Everyone runs their
own single-player game and OpenMP draws the other skaters in, so replicated variables, RPCs and
multicasts never reach anyone else.

- **For:** UE4SS C++ (DLL) mods and UE4SS Lua mods. There is no Blueprint support yet.
- **Needs:** SessionOpenMP 1.1.12 or later. Players on older versions never show up to your mod.
- **Header (C++ only):** [`sdk/omp_mod_api.h`](../sdk/omp_mod_api.h). Copy it into your project; it is free
  to use under any license. Lua mods need nothing: see [Lua mods](#lua-mods).

---

## Connecting

```cpp
#include "omp_mod_api.h"

static OmpModApi omp;     // zeroed until bound
static int channel = 0;

// Call from something that runs regularly, e.g. once a second. Not from your mod's constructor:
// UE4SS may load your mod before OpenMP.
void TryConnect() {
    if (channel) return;
    if (!omp.version && !OmpMod_Bind(&omp)) return;   // OpenMP not loaded: behave as single player
    channel = omp.Register("yourname.yourmod", OnMessage, OnPlayer, nullptr);
}
```

- `OmpMod_Bind` finds OpenMP by its exports and fills the struct. It returns 0 when OpenMP isn't
  loaded, so your mod keeps working without it.
- **Channel names** are 1-63 characters from `A-Z a-z 0-9 _ . -`. Start with your name so two mods
  never pick the same one. Only players who registered the same name receive your messages.
- `Register` works any time, in or out of a session. The handle stays valid across sessions and map
  changes until you call `Unregister`. Don't call it when the game closes: OpenMP may already be
  unloaded by then.

## Sending

```cpp
omp.Send(channel, player, data, len, reliable);
```

- `player` is `OMPMOD_EVERYONE` or one player's number.
- Up to **1000 bytes** per message. Put your own format version in the first byte.
- `reliable = 1` arrives, in order. `reliable = 0` can be dropped; use it only for values you resend
  constantly.
- Returns 1 when queued, 0 when refused: not in a session, too big, invalid handle, or over the limit.

**Limits.** 30 messages and 8 KB per second per channel, with short bursts of 10 messages allowed.
Receivers drop anything past 60 messages per second from one player.

## Receiving

```cpp
void OnMessage(int player, const uint8_t* data, int len, void* user);
```

`data` is only valid during the call, so copy what you keep.

**Treat everything you receive as untrusted.** Any player in the lobby can send any bytes on your
channel, including a modified copy of your mod. Check `len` before reading. Range-check every value.
Never use a received number as an array index, size or pointer without checking it.

## Players

```cpp
void OnPlayer(int player, int joined, void* user);
```

- `joined = 1`: a player with your channel appeared, including everyone already there when you
  register. Send them your current state now.
- `joined = 0`: they left, or unregistered the channel. `PlayerId` and `PlayerName` still return them
  here, so you can tell who it was.

**Player numbers are local to your game.** The same person has a different number on every other
machine, and a number can later belong to someone else. Never put a player number in a message.
Use `PlayerId` for anything you send or keep.

| Function | Returns |
|---|---|
| `Players(channel, out, cap)` | How many players have your channel; fills up to `cap` numbers |
| `PlayerId(player, out, cap)` | 1 and their stable ID, or 0 |
| `PlayerName(player, out, cap)` | 1 and their skater name, or 0. A label only: names can repeat or be empty for a moment |
| `LocalId(out, cap)` / `LocalName(out, cap)` | Your own ID and name |
| `PlayerActor(player)` | Their skater actor in your world, or null (not spawned, or on another map) |
| `ActorPlayer(actor)` | The player behind a skater actor, or -1 (your own skater is -1 too) |
| `InSession()` | 1 while in a session |

Use actors on the game thread only, and look them up again each frame instead of keeping them.

## Shared state

There is no server. For anything everyone has to agree on, like a score or a round, one player
decides: the **authority**.

- `IsAuthority(channel)` returns 1 if that's you.
- `Authority(channel)` returns the authority's player number, or -1 for you.

The authority is the player with the lowest player ID among everyone who has your channel. Every game
works out the same answer without sending anything, and it moves on its own when that player leaves.

The pattern: other players send requests, the authority applies them and sends the full state, and
everyone ignores state that doesn't come from `Authority(channel)`.

## Threads

- Every function is safe to call from any thread.
- Callbacks run on the game thread. Don't block in them. Sending from inside a callback is fine.
- Call `Unregister` from the game thread. From another thread, a callback already in flight can
  still run once.

## Example: a shared round timer

The authority owns the countdown and everyone else shows it. If the authority leaves, the next one
already has the latest state and carries on.

```cpp
#include "omp_mod_api.h"

static OmpModApi omp;
static int       channel = 0;

static uint8_t   roundNo = 0;
static uint16_t  secondsLeft = 0;

enum : uint8_t { MSG_STATE = 1, MSG_START = 2 };

static void SendState(int player) {
    if (!channel) return;
    const uint8_t buf[] = { MSG_STATE, roundNo, (uint8_t)(secondsLeft & 0xFF), (uint8_t)(secondsLeft >> 8) };
    omp.Send(channel, player, buf, sizeof(buf), 1);
}

static void StartRound() {
    roundNo++;
    secondsLeft = 120;
    SendState(OMPMOD_EVERYONE);
}

static void OnPlayer(int player, int joined, void*) {
    if (joined && omp.IsAuthority(channel)) SendState(player);      // late joiners get the round
}

static void OnMessage(int player, const uint8_t* data, int len, void*) {
    if (len < 1) return;
    if (data[0] == MSG_STATE && len == 4) {
        if (player != omp.Authority(channel)) return;                // only the authority's state counts
        const uint16_t secs = (uint16_t)(data[2] | (data[3] << 8));
        if (secs > 120) return;                                      // validate
        roundNo = data[1];
        secondsLeft = secs;
    } else if (data[0] == MSG_START && len == 1 && omp.IsAuthority(channel)) {
        StartRound();
    }
}

// The local player pressed "start".
void RequestStart() {
    if (!channel || !omp.InSession() || omp.IsAuthority(channel)) { StartRound(); return; }
    const uint8_t buf[] = { MSG_START };
    omp.Send(channel, omp.Authority(channel), buf, sizeof(buf), 1);
}

// Once a second.
void Tick() {
    if (!channel && (omp.version || OmpMod_Bind(&omp)))
        channel = omp.Register("yourname.round_timer", OnMessage, OnPlayer, nullptr);
    if (secondsLeft && (!channel || omp.IsAuthority(channel))) {
        secondsLeft--;
        SendState(OMPMOD_EVERYONE);
    }
}
```

## Lua mods

Every UE4SS Lua mod gets a global `OpenMP` table, ready from the first line of `main.lua`. It's the same
API with Lua types, and everything above (limits, untrusted data, player numbers, authority) applies.

```lua
if not OpenMP then return end   -- SessionOpenMP isn't installed, or is older than 1.1.12

local ch                        -- declared first, so the functions below can use it
ch = OpenMP.Register("yourname.yourmod",
    function(player, data)       -- data: a Lua string of bytes
        print(("got %d bytes from %s\n"):format(#data, OpenMP.PlayerName(player) or "?"))
    end,
    function(player, joined)     -- joined: true or false
        if joined then OpenMP.Send(ch, player, "welcome") end
    end)
```

- `Register` returns nil when refused.
- `Send(channel, player, data, reliable)` takes any Lua string up to 1000 bytes. `reliable` defaults
  to true. Returns true or false.
- `Players` returns a table. `InSession` and `IsAuthority` return booleans. `PlayerName`, `PlayerId` and
  `LocalId` return nil when unknown.
- `PlayerActor(player)` returns their skater as a UObject; `ActorPlayer(actor)` takes one. Both only work
  on the game thread: inside your callbacks, or in `ExecuteInGameThread`.
- Callbacks run on the game thread, a frame or two after the message arrives. An error in one is printed
  to `UE4SS.log` as an `[OpenMP]` line and doesn't stop the others.
- Lua and C++ mods can share a channel; bytes arrive exactly as sent. `string.pack` and `string.unpack`
  read and write the same layouts a C++ mod uses.

| Lua | Returns |
|---|---|
| `OpenMP.Register(name, onMessage, onPlayer)` | Channel, or nil |
| `OpenMP.Unregister(channel)` | Nothing |
| `OpenMP.Send(channel, player, data, reliable)` | true or false |
| `OpenMP.InSession()` | true or false |
| `OpenMP.Players(channel)` | Table of player numbers |
| `OpenMP.IsAuthority(channel)` / `OpenMP.Authority(channel)` | true or false / player number, -1 for you |
| `OpenMP.PlayerId(player)` / `OpenMP.PlayerName(player)` | String, or nil |
| `OpenMP.LocalId()` / `OpenMP.LocalName()` | Your ID (or nil) / your name |
| `OpenMP.PlayerActor(player)` / `OpenMP.ActorPlayer(actor)` | UObject or nil / player number or -1 |
| `OpenMP.EVERYONE`, `OpenMP.MAX_PAYLOAD`, `OpenMP.ApiVersion` | -1, 1000, 1 |

## Checking it works

`SessionOpenMP.log` in the game's Win64 folder has a `[modapi]` line for each registration, each
player joining or leaving your channel, refused sends, and messages dropped over the limit. Lua callback
errors show up in `UE4SS.log`.
