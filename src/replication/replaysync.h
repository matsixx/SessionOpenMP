// SessionOpenMP -- co-op multiplayer for Session, as an overlay on N solo games.
// Copyright (C) 2026 matsix
//
// This program is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version. It is
// distributed WITHOUT ANY WARRANTY; see the GNU GPL (LICENSE) for details.
//
// Additional permission under GNU GPL version 3 section 7: you may link and convey this
// work combined with the Epic Online Services SDK and the proprietary game runtime it
// loads into. See LICENSE-EXCEPTION.txt.
// SessionOpenMP -- replay sync: a peer's replay in YOUR editor, from THEIR data.
//
// The replay editor is the local player's own instance (peers never touch the game's replay
// system -- that design is retired, see proxy.h's recordPeers note). When the local player wants
// another skater IN their replay, this module transfers that peer's OWN recent state history --
// the same wire snapshots live replication runs on, which each client already produces 60 times a
// second -- and the session drives their proxy through the normal live pipeline, clocked by the
// scrub position instead of the network. First-party data, identical rendering math
// (repl::InterpStates), zero dependence on what the local client ever rendered or recorded.
//
// Three pieces:
//   * THE OWN RING (sender side, always on): every published snapshot is retained in a circular
//     byte arena covering the last ~kRingSeconds. Costs memory and a memcpy per publish, nothing
//     else, so it simply always runs -- any peer may ask at any time.
//   * THE TRANSFER: requester sends REQ; owner snapshots its ring into one serialized blob and
//     streams it in chunks; the requester fills a bitmap, NAKs holes, and DONEs when complete.
//     All packets ride the session transport (magic "OMPJ", validated like all P2P input). The
//     module never touches the transport directly -- the session hands it a send function -- so
//     the whole protocol runs offline in the loop test.
//   * PLAYBACK SAMPLING: the received blob is indexed by sender timestamp; SampleAt() finds the
//     bracketing pair for a target time and interpolates with the exact live math.
#pragma once
#include "replication.h"

namespace omp { namespace replaysync {

// How the module sends. Wired by the session to transport Send; wired by tests to a loopback.
using SendFn = void (*)(int peerIdx, const void* data, int len, bool reliable);
void SetSendFn(SendFn fn);
// The wire's per-Tick reliable-message budget (transport SendBudget()), refreshed by the session
// each frame; chunk bursts are capped at min(this, Tuning::chunksPerTick). 0 = unknown = no cap
// beyond the tuning (tests).
void SetChunkBudget(int budget);

// True if this buffer carries a replay-sync packet (magic peek; route before the snapshot path).
bool IsSyncPacket(const uint8_t* data, int len);

// ---- sender side: retain every published snapshot. Called once per publish with the packed bytes.
void RecordOwn(const uint8_t* pkt, int len, uint64_t senderUs);

// ---- protocol pump. OnPacket from the session's packet router; Tick once per frame (chunk
// pacing, NAK cadence, timeouts). Both run on the game thread like everything else.
void OnPacket(int peerIdx, const uint8_t* data, int len, uint64_t nowUs, void (*logf)(const char*));
void Tick(uint64_t nowUs, void (*logf)(const char*));

// ---- requester control (driven by the session on behalf of the menu).
enum class SyncState : uint8_t { None = 0, Transferring = 1, Ready = 2, Failed = 3 };
bool      RequestSync(int peerIdx, uint64_t nowUs);   // false = another transfer is in flight
void      CancelSync(int peerIdx);                    // abandon transfer and/or drop the buffer
SyncState PeerSyncState(int peerIdx, int* pctOut);    // pct meaningful while Transferring
void      DropAll();                                  // playback ended / session reset: free everything

// ---- playback sampling against a READY buffer. `targetUs` is OWNER-clock microseconds (the
// entries' own timestamps); the session computes it from the scrub position and the transfer
// anchors below. Clamps to the buffer's ends (scrubbing before the peer's history holds their
// oldest moment). False = no ready buffer for this peer.
bool     SampleAt(int peerIdx, uint64_t targetUs, repl::State& out);
uint64_t BufferNewestUs(int peerIdx);                 // 0 = no ready buffer
uint64_t BufferOldestUs(int peerIdx);

// Live-tunable knobs (statics in the .cpp): transfer pacing etc.
struct Tuning {
    int   chunksPerTick   = 20;     // per-Tick burst ceiling; the WINDOW below is the real governor
    // ---- FLOW CONTROL. The sweep keeps at most `windowChunks` un-ACKed chunks in flight: the
    // requester reports its received count every ackIntervalMs, and the sweep only advances as those
    // reports come back -- self-clocked to what the path actually carries. Open-loop pacing is a
    // measured field failure: a fixed 64/tick dumped the whole history into the EOS reliable queue
    // in ~3 s, and the relay spent ~30 s force-delivering the backlog -- drowning the live snapshot
    // lane with it (the peer froze for the duration, on BOTH ends' screens). ~32 x ~0.9 KB = ~29 KB
    // in flight adds <100 ms of queue to a typical relayed path, so the live lane keeps breathing;
    // throughput = window/RTT (~300 KB/s at 100 ms RTT), and a NAK-driven resend bypasses the window
    // (the receiver asked for it, so it has room; sweep-closed + NAK resends cannot deadlock).
    int   windowChunks    = 32;
    float ackIntervalMs   = 150.f;  // requester's progress-report cadence (13 B reliable each)
    // ---- HISTORY DOWNSAMPLE. Entries closer than this are skipped when the outgoing blob is built:
    // ~33 ms = ~30 Hz, the SAME cadence the live pose lane serves a scrubbing peer at, and the
    // scrub-side InterpStates blends drivers AND bones between entries -- so fidelity matches live
    // scrubbing while the transfer halves (~9.4 MB -> ~4.7 MB of a full 120 s ring).
    float historyMinGapMs = 33.f;
    float nakIntervalMs   = 400.f;  // how often the requester re-asks for holes
    float hdrTimeoutMs    = 3000.f; // REQ sent, no HDR: retry REQ (x3) then fail
    float stallTimeoutMs  = 12000.f;// mid-transfer NO-PROGRESS -> fail (sender: -> free). Progress
                                    // resets it, so a slow flow-controlled transfer never trips.
};
Tuning& Tune();

}} // namespace omp::replaysync
