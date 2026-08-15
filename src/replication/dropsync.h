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
//
// SessionOpenMP -- the DROPPED-OBJECT lane (magic "OMPO"). The codec and the part assembly only; it
// never touches the game (game/dropper.cpp) and never touches the transport (the session injects a
// send function, exactly as replaysync does), so the whole protocol runs in the offline loop test.
//
// FOUR MESSAGES, one shape:
//   kSet     a peer's COMPLETE current set, in as many parts as it takes. Not a one-time join
//            transfer but a full-state RESYNC -- sent when a new peer appears, when the world
//            changes, and on a slow heartbeat -- so a third player arriving mid-session needs no
//            history replay, and a lost delta self-heals at the next beat.
//   kPlace   one new object.
//   kMove    up to kMoveBatch objects' poses, UNRELIABLE, while someone drags them.
//   kRemove  ids that are gone.
// Every packet is SELF-CONTAINED (the snapshot lane's rule): a kSet part carries its own name table,
// so a part parses without its siblings and only ASSEMBLY needs them all.
//
// IDENTITY. An object is (peer index, generation, localId). The peer index comes from the transport
// and never from the payload -- that is what stops a sender wearing another peer's objects. localId
// is the sender's own u16 handle, so no pointer ever travels (standing rule 2). The generation is
// bumped by the sender whenever its ids stop meaning what they meant (a world change, a reset); a
// receiver seeing a new generation drops everything it had for that peer, and deltas quoting an
// unassembled generation are ignored rather than guessed at.
//
// The class NAME is the id Session's own save file uses (see game/dropper.h). Position and rotation
// are absolute f32 and an f32 quaternion: a placed object must land exactly where its owner put it,
// and it is world-anchored, so there is nothing to encode it relative TO. Quats end to end, no euler
// anywhere in the path (standing rule 1).
#pragma once
#include <cstdint>

namespace omp { namespace dropsync {

static const int kMaxSetRecords = 256;   // must match game::dropper::kMaxObjects
static const int kRecsPerPacket = 30;    // 30 x 32 B + header + name table stays under the 1 KB cap
static const int kMoveBatch     = 24;
static const int kMaxNameLen    = 63;

// One dropped object. `id` is the object's CLASS name, which is exactly the id Session's own save
// file uses, and what the receiver resolves through the game's catalogue before spawning one.
struct Rec {
    char     id[64];       // "" on a kMove/kRemove record -- the localId already names the object
    uint16_t localId;
    float    loc[3];
    float    quat[4];
};

// How the module sends. Wired by the session to transport Send; wired by tests to a loopback.
using SendFn = void (*)(int peerIdx, const void* data, int len, bool reliable);
void SetSendFn(SendFn fn);

// True if this buffer carries a dropped-object packet (magic peek). Route BEFORE the snapshot
// Unpack -- that is the fallback branch, and an unrecognised magic there trips the once-per-peer
// "different SessionOpenMP version" warning.
bool IsPacket(const uint8_t* d, int len);

// ---- sender ------------------------------------------------------------------------------------
// Each returns the number of packets sent. `SendSet` is capped at `budget` packets per call
// (transport SendBudget()) and returns 0 having sent nothing if it cannot send the WHOLE set: a
// half-set would apply as "everything else was deleted". Sets are small enough that this is always a
// single burst in practice -- the cap is a guard, not a pacing scheme.
// `authKey` is the sender's stable identity, hashed. It rides the set because the SHARED policy has
// to pick one canonical saved set and every machine must pick the SAME one, with no host concept to
// lean on: the transport cannot help (PeerIdStr is EOS-only -- it is "" on shared memory and UDP,
// which is exactly the same-PC test rig), and a peer INDEX is local to whoever is counting. Lowest
// key wins, so the choice is symmetric, needs no agreement protocol, and survives the host leaving.
int SendSet(int peerIdx, uint8_t gen, uint64_t authKey, const Rec* recs, int n, int budget);
int SendPlace(int peerIdx, uint8_t gen, const Rec& r);
int SendMove(int peerIdx, uint8_t gen, const Rec* recs, int n);
int SendRemove(int peerIdx, uint8_t gen, const uint16_t* ids, int n);

// ---- THE LEVEL'S OWN PROPS ---------------------------------------------------------------------
// A separate lane, deliberately NOT part of the object set above: these actors already exist on
// every machine (they are baked into the map), so nothing is ever spawned for them -- each client
// MOVES ITS OWN copy of the actor named.
//   kWorldSet   the HOST'S ARRANGEMENT: name + pose for every level prop the session layout has an
//               opinion about. Sent by the canonical player only, assembled from parts exactly like
//               kSet (a half-received layout would revert the missing props to their defaults), and
//               a COMPLETE EMPTY set is meaningful -- it says "the host moved nothing", which still
//               tells a joiner to stand its own moved props back on their map defaults.
//   kWorldMove  one prop's pose, sent by whoever is holding it. `claim` is true while they still
//               have it. Only the holder ever sends, so there is exactly one writer per prop at all
//               times; the host folds the release pose into its layout so later resyncs carry it.
static const int kMaxWorldRecords   = 128;   // must match the session's world table
static const int kWorldRecsPerPacket = 10;   // 92 B worst-case record; 10 stays under the 1 KB cap

struct WorldRec {
    char  name[64];        // the level actor's own name -- identical on every install
    float loc[3];
    float quat[4];
};
// All-or-nothing like SendSet: returns parts sent, or 0 having sent nothing if the whole layout
// cannot go. n = 0 is a legal, meaningful send (one empty part). `map` is the level the layout
// describes: a layout is meaningless anywhere else, and an EMPTY one actively harmful (it reads as
// "revert your furniture to its defaults") -- so the receiver refuses a map that is not its own.
int SendWorldSet(int peerIdx, uint8_t gen, const char* map, const WorldRec* recs, int n, int budget);
int SendWorldMove(int peerIdx, uint8_t gen, const char* name, const float loc[3], const float quat[4],
                  bool claim);

// ---- receiver ----------------------------------------------------------------------------------
// What one received packet asks the caller to do. Pointers are into dropsync's own storage and are
// valid until the next OnPacket for that peer.
struct Update {
    uint8_t  gen       = 0;
    bool     genReset  = false;   // this peer's ids changed meaning: drop every object you hold for it
    bool     setReady  = false;   // a COMPLETE set is available from SetRecords(peerIdx)
    bool     haveAuth  = false;   // authKey below is from this packet (kSet only)
    uint64_t authKey   = 0;
    int     nPlace     = 0;   const Rec*      place  = nullptr;
    int     nMove      = 0;   const Rec*      move   = nullptr;
    int     nRemove    = 0;   const uint16_t* remove = nullptr;
    // The level's own props. `worldSetReady` means a COMPLETE layout is available from
    // WorldSetRecords(peerIdx); a world MOVE is one prop's pose from whoever is holding it.
    bool        worldSetReady = false;
    bool        haveWorldMove = false;
    const char* worldMoveName = nullptr;
    float       worldMoveLoc[3] = {0,0,0};
    float       worldMoveQuat[4] = {0,0,0,1};
    bool        worldMoveClaim = false;
};
// False = not ours, malformed, truncated, or from a peer index we cannot track. Never partially
// applies: `out` is only filled from a packet that parsed completely.
bool OnPacket(int peerIdx, const uint8_t* d, int len, Update& out);
const Rec* SetRecords(int peerIdx, int* nOut);   // the last COMPLETE set for this peer
// The last COMPLETE world layout from this peer, or null if none has ever assembled. A non-null
// return with *nOut == 0 is the empty layout -- a real answer, not an absence. `mapOut` (optional)
// receives the map the layout is FOR.
const WorldRec* WorldSetRecords(int peerIdx, int* nOut, const char** mapOut = nullptr);
void ForgetPeer(int peerIdx);
void ResetAll();

// True if this class name can travel. The SENDER checks each record with it and drops just that one,
// because a name the receiver refuses rejects the whole packet -- and for a kSet that is the entire
// set plus the authority key riding in it.
bool NameIsSendable(const char* cls);

struct Stats {
    int sent = 0, recv = 0, rejected = 0, partsDropped = 0;
    int setsSent = 0, setRecordsSent = 0;      // what we published
    int setsRecv = 0, setRecordsRecv = 0;      // ...and what completed on the way in
    int unsendable = 0;                        // records dropped for an untransportable name
};
const Stats& St();

}} // namespace omp::dropsync
