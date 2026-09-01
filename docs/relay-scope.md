# A relay for SessionOpenMP

Scoped and then BUILT, 2026-09-01. What it buys, what it does not, and how it is put together.

**Status: built and gated, not yet played on.** `tools/relay` is the server, `BK_RELAY` in
`src/transport/relay_transport.cpp` is the client, and `omp_relaytest` runs one server and three
real clients on loopback and asserts every player sees every other. Untested with real people over
a real network -- that is the next thing it needs.

**The four decisions, as made:** forward-only (no direct upgrade yet); public room listing;
the relay address is typed in by players rather than baked in, so anyone can host one; and a room
lives exactly as long as its players.

**The server is PORTABLE.** The mod is Windows-only because the game is; the relay is not, because
the natural home for a service that runs all day is a cheap Linux VPS. One compat block at the top
of `relay_main.cpp` holds everything platform-specific and nothing below it knows which OS it is on.
`tools/relay/README.md` has the build line, a systemd unit and the firewall rule. **The Linux
compile is UNVERIFIED** -- this was written on a Windows machine with no cross-compiler.

---

## The one-line version

A small standalone UDP server that **forwards packets between players and introduces them to each
other**. It does not run the game and knows nothing about skating. It is what makes more than two
players work over the internet, and it is the same process that would later become a dedicated
server if that is ever wanted.

**It is not a headless client.** Session has no dedicated-server target, the mod hooks DXGI Present
for its overlay (there is no swapchain without a display), and architecturally there is nothing to
host — the mod is "an overlay on N solo games", so a headless peer would be a player standing
still. Running the game itself on a VPS also raises a licensing question the relay does not.

---

## What it fixes

| | today (direct UDP) | with a relay |
|---|---|---|
| 3+ players on a LAN | works (peer introduction) | works |
| 3+ players over the internet | **joiners see the host, not each other** | works |
| Anyone behind a normal NAT | needs a forwarded port | works, nobody forwards anything |
| Your IP | every peer learns it | only the relay sees it |
| Client upload at N players | N-1 streams | **1 stream, whatever N is** |
| Session with nobody hosting | impossible | possible: the room outlives its players |
| Lobby browser without Epic | not available | the relay knows its own rooms |

The NAT point is the one that matters. Every client **dials out** to the relay, which is what opens
a return path through a home router. That is why a relay works where hole punching is unreliable and
port forwarding is unreasonable to ask for.

---

## Why this is cheaper than it looks

**The session layer needs no changes at all.** Verified, not assumed:

- `transport.h` states the rule the whole seam exists to enforce: *"the game must never know or care
  which wire its packets rode."* Peers are addressed by index; identity is established by the
  transport, never by anything inside a payload.
- Its trust section already names this exact case: *"custom relay servers, direct P2P and dedicated
  servers are all on the roadmap and none of them has a lobby roster to consult."*
- The only two places `session.cpp` consults trust (the join announcement, and the ghost-peer spawn
  gate) both read `BackendTrust()` **first** and degrade correctly for a backend that can prove
  nothing. Nothing above the seam is written in EOS's vocabulary.

**The game's wire format does not change either.** A relay header is ~8 bytes on top of the existing
12-byte frame header. The current datagram is at most `12 + 8 + 1024 = 1044` bytes; adding a relay
header makes it ~1052, still far below any MTU. The session keeps packing into 1024 exactly as now,
and `repl::Pack` is untouched.

So the work is confined to: **one new backend, one new standalone server, and widening a handful of
`g_cur == BK_EOS` checks in the dispatcher.**

---

## Architecture

**The relay is a dumb forwarder.** It never parses a game packet, never knows what a snapshot is,
and holds no game state. Reliability, sequencing, dedupe and the peer tokens all stay **end to end
between clients**, exactly as they are today. This is the decision that keeps the server small
enough to trust running unattended.

```
   client A  ──┐                        ┌──►  client B
               ├──►  relay (room "ABC")  ┤
   client C  ──┘        forwards          └──►  client A ...
```

- A client opens ONE flow to the relay and sends `{room, destSlot, payload}`.
- The relay looks up `destSlot` in that room and forwards `{srcSlot, payload}` to its endpoint.
- Slot -> peer index mapping happens in the client backend, so the seam's "indices are never reused"
  rule is honoured client-side where it already is.

**The client backend is largely today's `udp_transport.cpp`.** Its peer table, reliable lane
(`RelOut`, seq/ack, the 64-entry dedupe bitmap), timeout handling and stats all carry over unchanged.
The single thing that differs is how a peer is *reached*: a `sockaddr_in` becomes a relay slot. That
is the natural seam for the refactor — one `Link` that is either an address or a slot.

**The relay can vouch.** It knows exactly who is in a room, which no direct-UDP wire can. That means
it can honestly report `TRUST_VOUCHED`, which **re-enables the ghost-peer protections that currently
no-op on direct UDP** — the same guards that were written for EOS's ghost lobbies. That is a real
correctness gain, not just convenience.

---

## Phases

**1. The relay server.** Standalone executable, no game dependency, no UE, no EOS. Room table, slot
assignment, forwarding, per-endpoint timeouts, room codes, a room-list reply for the browser. Builds
and tests entirely offline. *Largest single piece, and the most self-contained.*

**2. The client backend (`BK_RELAY`).** Adapted from `udp_transport.cpp` as described above, plus the
room protocol behind `LobbyHost` / `LobbyJoin` / `LobbyJoinByCode` / `LobbyLeave` / `LobbyStatus`.
*Mostly adaptation rather than new logic.*

**3. The seam and the UI.** Register the backend in `transport.cpp`; widen the `g_cur == BK_EOS`
checks on `LobbyBrowse` / `BrowseStatus` / `BrowseAt` / `LobbyJoinAt` / `PeerIdStr` / `LobbyIsHost` /
`LobbyKick` so a relay session gets the browser and moderation it can actually support; a relay
address + room code in the F1 Session tab and the pause menu; `Posture()` telling the truth about
what a relay does and does not protect. *Small, but touches several files.*

**4. Gates.** Extend the pattern `omp_udptest` just grew: spawn a relay plus three clients on
loopback and assert everyone sees everyone, reliable messages survive, and a departure is noticed.
Same discipline as the mesh gate — **prove it fails without the feature before trusting it to pass.**

---

## The hard parts

**Abuse is the real risk, and it is a server on the public internet.** An open UDP relay is a
reflection/amplification vector and a free bandwidth donation to whoever finds it. The rules that
matter:

- **Never send an unverified endpoint more bytes than it has sent you.** This is what makes the
  server useless for amplification, and it has to be true of the room-list reply too.
- Per-endpoint and per-room rate limits, and a hard cap on rooms, clients per room, and bytes/second
  per client.
- Rooms are code-gated by default; a public listing is opt-in per room.
- The relay should be able to run with no persistent state at all, so a restart is a clean slate.

**It is a service you now operate.** It has to survive being unattended: no unbounded growth, no
state that needs cleaning, and a log that says what happened. Worth budgeting for as much as the
protocol.

**Latency.** One extra hop. For this game that is fine, and a later "try direct first, fall back to
the relay" is a pure addition — the client already knows how to talk directly.

**Not encryption.** A relay sees every packet. It removes peer-to-peer IP exposure and adds an
operator who can see traffic instead. `Posture()` must say that plainly rather than let the F1 menu
imply a relay is "secure".

---

## Capacity, from measured numbers

A snapshot carrying a 70-bone pose is ~1 KB; steady-state traffic is nearer **400 B at 30 Hz, about
12 KB/s per stream** (`omp_looptest` prints the packed sizes).

- **Client:** one stream up (~96 kbps) regardless of headcount, `(N-1)` streams down.
- **Relay at 16 players:** ~190 KB/s in, ~2.9 MB/s out — roughly **23 Mbps**, which is nothing for a
  cheap VPS. Forwarding is not CPU work.

**The binding constraint is not the network — it is frames on the weakest player's PC.** Every proxy
is a full skeletal mesh with cloth and per-frame animation, in UE4. Nobody has run more than a
handful, so "somewhere around 6–10 visible skaters gets uncomfortable" is an educated guess and
should be **measured before it is promised**. `kPeers = 16` in both backends and the session slot
table is today's hard ceiling regardless.

---

## Decisions to make before starting

1. **Forward-only, or forward with a direct-connect upgrade later?** Recommend forward-only first:
   it always works, and the upgrade is additive.
2. **Public room listing, or codes only?** Codes only is the safer default and much less to police.
3. **Who runs it?** One relay you host, or a relay address players type in. The second is less
   commitment and makes the project self-hostable; the first is the nicer experience.
4. **Does it need to outlive its players** (an empty room persisting so people can join "the usual
   session"), or is a room just the lifetime of whoever is in it? The second is simpler and is the
   assumption above.
