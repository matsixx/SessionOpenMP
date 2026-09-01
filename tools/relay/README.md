# omp_relay — running one

A small UDP server that forwards packets between the players in a room. It does not run the game and
knows nothing about skating, so it needs no GPU, no Windows and no game files — a cheap Linux VPS is
the natural home for it.

Players type its address into the mod's F1 window (**Session → Relay server**) and pick a room name.
The first person to use a name opens that room; it closes when the last player leaves.

---

## Build

**Linux** — no project, no dependencies, nothing to install beyond a compiler:

```
g++ -O2 -o omp_relay relay_main.cpp
```

It needs `relay_proto.h` from `src/transport/` at the path the `#include` expects, so copy the two
files keeping that layout, or just clone the repo and build in place.

**Windows** — it is a normal target of the main project:

```
cmake --build build --config Release --target omp_relay
```

## Run

```
./omp_relay              # UDP 47800
./omp_relay 30000        # or any port you like
```

Open that **UDP** port in the VPS firewall — UDP, not TCP; there is no TCP here at all.

```
sudo ufw allow 30000/udp
```

Then give players `your.address:30000`.

## As a service

```ini
# /etc/systemd/system/omp-relay.service
[Unit]
Description=SessionOpenMP relay
After=network.target

[Service]
ExecStart=/opt/omp/omp_relay 30000
Restart=always
RestartSec=5
User=omp
# It keeps no files and needs no privileges. Take them away.
DynamicUser=no
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

```
sudo systemctl enable --now omp-relay
journalctl -u omp-relay -f
```

The log is stdout, one line per event, and a summary every minute:
`3 room(s), 7 player(s), 1284003 forwarded, 0 dropped`.

---

## What it costs to run

Forwarding is not CPU work, and the loop **blocks** rather than spins — an idle relay uses
essentially nothing. Bandwidth is the only real number, and it is the one that scales:

| Players in a room | Relay bandwidth out |
|---|---|
| 4 | ~1.7 Mbps |
| 8 | ~8 Mbps |
| 16 | ~23 Mbps |

(From a measured ~400 B snapshot at 30 Hz per player, fanned out to everyone else. `omp_looptest`
prints the real packed sizes.)

**The relay is nowhere near the limit** — the binding constraint is frames on the weakest player's
PC, since every other player is a full skeletal mesh with cloth in their game.

## What it does not do

- **No encryption.** Whoever runs the relay can see the traffic. It removes peer-to-peer IP exposure
  and puts an operator in its place. That is a real trade and the mod says so in its own UI.
- **No accounts, no persistence, no moderation.** A room is a name; anyone with the address and the
  name is in it. Nothing is written to disk, so a restart is a clean slate.
- **No amplification.** The one reply larger than its request — the room list — requires the request
  to be padded to the reply's size, so the server is useless as a reflector for an attack on
  somebody else. That rule is in `relay_proto.h` and is gated by `omp_relaytest`.

Caps are compile-time and are refusals rather than resizes: 64 rooms, 16 players per room, 512 KB/s
per client. A game stream is nowhere near that last one.
