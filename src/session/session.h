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
// SessionOpenMP -- the session: the only place that knows about BOTH the wire and the game.
// Responsibilities, and deliberately nothing else:
//   * find OUR pawn (IsLocallyControlled -- never a heuristic)
//   * publish our own State at a fixed rate, scaled by peer count
//   * keep one Stream + one Proxy PER PEER, bound 1:1 by the TRANSPORT's peer index
//   * spawn/drive/retire proxies as peers arrive, go quiet, and leave
// Identity comes from the transport, never from anything inside a payload. That is what stops a buggy
// or hostile sender from wearing another peer's stream.
#pragma once
#include "../replication/replication.h"
#include "../game/proxy.h"
#include <cstdint>

namespace omp { namespace session {

struct Config {
    float publishHzMax   = 60.0f;   // one peer: full rate
    float publishHzMin   = 30.0f;   // many peers. The quantized wire costs ~1/3 the bytes, which buys
                                    // this floor -- a higher floor tightens the snapshot buffer's
                                    // delay, so remotes in big lobbies lag less.
    float quietMs        = 1500.0f; // stream silent this long -> stop the proxy's board simulating
    float dropMs         = 30000.0f;// ...and this long -> release the slot entirely
    bool  enabled        = true;
    // A chat line arrived. Called on the GAME thread, from OnPacket, with the SENDER'S OWN name (it
    // travels with the message so a line keeps the name it was said under). A callback rather than a
    // queue because the session layer must not know the UI exists.
    // `peerId` is the sender's TRANSPORT INDEX -- the identity a slot is keyed by, and the only one of
    // the three (name / row / actor pointer) that a caller may hold on to. Anything that has to attach
    // the line to the person who said it -- a speech bubble over their skater -- keys on this; the
    // name is a label two people can share.
    void (*onChat)(int peerId, const char* name, const char* text) = nullptr;
    // A peer sent a packet this build cannot parse -- in practice, a different mod version. Fired
    // ONCE per peer, so it is safe to put in front of the player. Optional: the session already logs
    // it, and this exists so the UI can say it where the player is actually looking.
    void (*onVersionMismatch)(int peerId) = nullptr;
};

struct Stats {
    int      peers = 0, proxiesAlive = 0;
    uint32_t published = 0, received = 0, appliedFrames = 0;
    float    publishHz = 0;
};

// Called once per game frame. `ownPawn` may be null (menus, loading) -- everything degrades to a no-op.
// `nowUs` is a monotonic microsecond clock; `sampleState` fills our OWN State from the game.
void Init(void (*logf)(const char*));
void Shutdown();
void SetConfig(const Config& c);
const Config& GetConfig();
Stats GetStats();

// The frame hook. `gatherOwn` returns false if our own state cannot be read this frame (menu, dead pawn).
using GatherFn = bool (*)(void* ownPawn, repl::State& out);
void Frame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, GatherFn gatherOwn);

// Transport delivery: called for every sideband packet, with the transport-established peer index.
void OnPacket(int peerIdx, const uint8_t* data, int len, uint64_t nowUs);
// A peer's world changed / left: drop its proxy pointers (they died with the world).
void ForgetProxies();
// Release EVERY slot -- required before a transport BACKEND SWITCH, because slots are keyed by
// transport peer index and shm slot 0 / EOS peer 0 are different humans (a reused stream would render
// the newcomer on the old sender's clock). Boards get OnQuiet first (no board may keep simulating
// without a writer). NOTE: proxy ACTORS are forgotten, not destroyed -- an old proxy may remain
// standing as a statue until the next world change (known limitation; no DestroyActor signature yet).
void ResetAll();

// ---- the live peer roster, for UI. Index 0..PeerSlots()-1; returns false for an empty slot, so
// callers iterate the whole range rather than assuming density -- slots are keyed by transport index
// and are deliberately never compacted, because the slot IS the identity.
// `name` is the peer's own skater name from their cosmetics packet, or empty if it has not arrived.
// Is any peer we are currently driving in the replay editor? Their machine will not evaluate our
// skeleton while they scrub, so our anim DRIVERS are useless to them however good they are -- we have
// to send them the finished pose instead. Read by pose::Capture; see the pose lane in replication.h
// for the full mirror-image argument.
bool AnyPeerReplaying();

int  PeerSlots();
// `peerIdOut` is the peer's TRANSPORT INDEX -- the identity a slot is keyed by, stable for as long as
// that peer is connected and never reused while they are. UI must remember a selection by THIS, not
// by a name (two players may share one) and not by a row position (the roster is rebuilt live, so a
// join or a leave silently re-points an index at somebody else).
bool PeerAt(int slot, char* nameOut, int nameCap, void** proxyActorOut, int* peerIdOut = nullptr);
// Where that peer is: the INTERNAL level name they published. Empty until their cosmetics packet has
// landed. Resolve it for display with game::PrettyMapName.
bool PeerMap(int slot, char* out, int cap);
// Resolve an identity to the actor it currently has, or null if they have left / have no proxy yet.
// Always go through this at the moment of use. A cached actor pointer from an earlier frame is a
// dangling pointer as soon as that peer drops, and handing one to the engine (e.g. the replay
// camera's _lookAtTarget) is a crash with our DLL nowhere in the stack.
void* PeerActorById(int peerId);
// True if this actor is one of OUR proxies. The local pawn discovery uses it as a veto: a proxy
// is a real actor of the player's own skater class, so the engine's own "is this locally
// controlled" test cannot tell them apart if a controller ever possesses one.
bool  IsProxyActor(void* actor);
// The per-peer replay view (ProxyTuning::recordPeers): during the LOCAL player's playback a peer
// either shows LIVE (default) or is driven by the recording. Set by the pause menu, read by the
// live writers, reset by the loader when playback ends.
void  SetPeerViewRecorded(int peerId, bool recorded);
bool  PeerViewRecorded(void* actor);
void  ResetPeerViews();
// A proxy the playback machinery drove and has not yet released -- the mesh's anim rate is zeroed
// until the game's own transition restores it. Read by the replay-mode guard to decide which exit
// broadcasts must pass; cleared where the restore runs.
bool  PeerPlaybackTouched(void* actor);
void  ClearPlaybackTouched(void* actor);

}} // namespace omp::session
