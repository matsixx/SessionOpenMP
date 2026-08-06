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
// Remote players as LOCAL, NON-REPLICATED game-class skaters driven from the wire:
//   * an observed player is OUR actor, spawned locally, replicating to nobody
//   * every value it shows is TRANSPORTED; nothing about it is derived locally
//   * therefore both machines run identical code -- no Role branch, no mesh net smoothing, no Bail
//     role gate, no relevancy or dormancy, and no asymmetry to compensate for
// Real game-class actors (not SkeletalMeshActors) are what buy player collision and native audio.
#pragma once
#include "../replication/replication.h"
#include <cstdint>

namespace omp { namespace game {

struct ProxyTuning {
    // Spawn NEVER at the sender's exact position: a Character spawn defaults to
    // AdjustIfPossibleButDontSpawnIfColliding and returns NULL inside another character.
    float spawnOffsetCm   = 200.0f;
    int   spawnMaxTries   = 8;
    float spawnRetryMs    = 1000.0f;      // throttled: an unthrottled retry storm hides behind a capped log
    bool  velocityDrive   = true;         // false = teleport stamp only
    bool  driveGroundedOnly = true;       // airborne/trick frames use the stamp: a flip outruns the chase
    float driveSnapCm     = 120.0f;       // beyond this: teleport (join/respawn), not drive
    float driveSnapAngRad = 1.75f;        // ~100 deg: an upside-down deck is an energy trap the angular
                                          // chase cannot escape (it would have to rotate THROUGH the
                                          // ground), so stamp the pose instead of chasing it
    float driveMaxVel     = 3500.0f;      // cm/s clamp
    float driveMaxAngRad  = 25.0f;        // rad/s clamp
    float quietStopMs     = 1500.0f;      // stream silent this long -> stop simulating (no zombie boards)
    bool  vetoBail        = true;         // a proxy must not DECIDE to bail; the owner transports it
    bool  carryBoard      = true;         // off board: stamp the transported pose. PlaceInHand runs on
                                          // the SENDER's machine and the deck pose is already on the
                                          // wire, so the carried board needs no local reproduction.
                                          // false = park+hide, which leaves an invisible but
                                          // query-collidable board at the dismount point.
    bool  bailSync        = true;         // owner's ragdoll edge -> execute Bail locally on the proxy,
                                          // and ResetRagDoll on the falling edge to recover
    // ---- SCALING. What N players cost is dominated by the game simulating N real skaters; these
    // two shed the cost where it cannot be seen or felt.
    bool  offscreenAnimThrottle = true;   // proxy meshes get AlwaysTickPose (NOT OnlyTickPoseWhen-
                                          // Rendered): the graph still updates every frame -- the
                                          // replay RECORDS anim fields and playback ADVANCES them,
                                          // so a visibility-gated graph froze peers' replay tracks
                                          // in stretches ("randomly on and off") -- but the bone
                                          // evaluation/refresh, the expensive half, is skipped
                                          // while the mesh is not rendered (shadow included).
    float boardSimMaxDistM = 25.0f;       // beyond this the peer's board stops SIMULATING and is
                                          // stamped instead: collision response only matters at
                                          // contact range, and by the time you can reach a board it
                                          // is simulating again.
    float boardSimHystM    = 5.0f;        // re-enter band, so the boundary cannot flap sim on/off
    // Record peers into the local replay. Their replay components self-register at BeginPlay (a
    // proxy is a real skater); with this on they are left registered, so the game records everyone
    // and a replay scrubs through the whole session, not just you. The fight that used to make a
    // recorded peer thrash -- live replication and replay playback both writing the same actor --
    // is resolved by the OTHER half of this feature: while the local player is in replay playback,
    // every live writer for proxies goes quiet (session skips Apply, the anim post-pass and the
    // pose stamp skip), so the recording is the only author. false = the old behaviour: peers are
    // pruned from the recording and shown LIVE while you scrub, served by the unicast pose lane.
    bool  recordPeers     = true;
    // ---- TYPE GATES. A peer chooses the NAME we resolve, and StaticFindObject with ANY_PACKAGE
    // returns the first object of ANY class bearing it -- so without these the peer also chooses the
    // TYPE, and the resulting wrong-typed pointer is read by engine code where our SEH is no help.
    // Each gate rejects to NULL, the same path an asset this install does not have already takes, so
    // nothing new has to handle the failure.
    // They SELF-CHECK: the same test runs against the LOCAL player's own def/cue, and a gate that
    // rejects a locally-owned object has a wrong expectation, so it disables itself and says so
    // rather than filtering peers on a bad premise.
    bool  typeGateTrick   = true;
    bool  typeGateGrind   = true;
};

struct ProxyStats {
    uint32_t spawnTries = 0, spawnFails = 0, driven = 0, snaps = 0, airSkips = 0, carryStamps = 0,
             stops = 0, bailVetoes = 0, bails = 0, pushes = 0;
    float    driveErrCm = 0;
    bool     alive = false, boardOwned = false, onBoard = false;
};

// One remote player: the actor, its board, and everything we do to them each frame.
class Proxy {
public:
    // `ownPawn` is only used as a world handle + spawn reference; nothing about it is modified.
    bool EnsureSpawned(void* ownPawn, const repl::State& first, uint64_t nowMs, void (*logf)(const char*));
    // Called once per frame with the sampled state. Order inside matters and is documented in the .cpp.
    void Apply(const repl::State& s, uint64_t nowMs, uint64_t nowUs, void (*logf)(const char*));
    // The peer's one-shot sounds, released by their stream when the playback clock reaches the moment
    // they actually fired. Called right after Apply so the body position they are placed against is
    // this frame's. (Kept separate from Apply because Sample() can skip whole snapshots -- an event
    // carried in a sampled State would be lost exactly when the network is worst.)
    void PlayAudioEvents(const repl::AudioEvent* e, int n);
    // The peer's push-state transitions, released by their stream on the playback clock. Same shape
    // and same justification as PlayAudioEvents: a tap-driven push is a one-shot, and one-shots are
    // lost by Sample(). See repl::Stream::DrainPushStates.
    void PlayPushStates(const uint8_t* states, int n);
    // Stream went quiet / session ended: stop the board simulating, leave the actor alone. Never
    // destroy the actor -- destroying a proxy mid-session crashes the client.
    void OnQuiet(void (*logf)(const char*));
    // A peer LEFT (slot released), as opposed to merely going quiet. Hide the actor and stop its board
    // before the pointers are dropped: Forget() only forgets, so without this the abandoned skater
    // stands there frozen while its board -- no longer written by anyone -- rolls off on its own.
    void Retire(void (*logf)(const char*));
    void Forget();                        // world changed: every actor died with it; drop pointers

    void*      actor() const { return actor_; }
    ProxyStats stats() const { return st_; }
    // Written by the session each frame from the distance between the LOCAL player and this peer
    // (with hysteresis). Far = the board is stamped, never simulated.
    void       SetNearLocal(bool near) { nearLocal_ = near; }
    static ProxyTuning& Tuning();

    // ---- visuals handshake with the cosmetics layer -------------------------------------------------
    // A freshly spawned skater is built from the GAME INSTANCE's look, i.e. the LOCAL player's, and the
    // game's own rebuild is deliberately delayed until the actor has finished constructing. Cosmetics
    // must respect both facts: dress no earlier than that same beat (dressing in the same frame as the
    // spawn faults the rebuild), and once we HAVE dressed, the plain rebuild must never run afterwards
    // -- it would re-read the restored, local profile and silently undo the dressing.
    static const uint64_t kVisualSettleMs = 1500;
    bool VisualsSettled(uint64_t nowMs) const { return actor_ && (nowMs - bornMs_) > kVisualSettleMs; }
    void MarkVisualsRefreshed() { refreshed_ = true; }   // our dress WAS the rebuild, with the right data

private:
    void* OwnBoard() const;               // board ONLY if `board+0x4d8` links back to us
    bool  VelocityDrive(const repl::State& s, uint64_t nowUs);
    void  StampBoard(const repl::State& s);
    void  StopBoardSim();
    // The peer's ACTUAL sounds, captured at their end and re-issued here. Loops are diffed against
    // what we have playing (the wire's set IS the truth: appear = start, vanish = stop, present =
    // keep the parameters current). One-shots arrive separately, off the stream's playback clock, so
    // they land with the animation instead of a buffer-delay early.
    void  AudioApply(const repl::State& s, void* bd);
    void  AudioStopAll();

    void*      actor_ = nullptr;
    void*      world_ = nullptr;
    uint64_t   bornMs_ = 0, lastTryMs_ = 0, lastDriveUs_ = 0;
    int        tries_ = 0;
    bool       refreshed_ = false, repOff_ = false, boardRepOff_ = false, tickOff_ = false;
    bool       boardHidden_ = false, simOn_ = false, boardLogged_ = false;
    bool       nearLocal_ = true;         // default near: sim until the session has measured
    bool       animThrottled_ = false;    // the tick-option write happens once per actor
    uint8_t    lastPushState_ = 0, lastBrakeState_ = 0, lastBailing_ = 0;
    char       lastTrickName_[48] = {}, lastGrindName_[48] = {};
    void*      trickDef_ = nullptr;       // the RESOLVED trick def on OUR side
    uint16_t   lastCrankIdx_ = 0xfffe;    // crank edge probes (0xfffe = "never seen", so the first
    uint8_t    lastCrankOn_ = 0;          // def-idx line prints even when the wire says none/0xffff)
    // The peer's live sounds, keyed by THEIR slot id -- stable for as long as the sound lives on
    // their machine, which a name would not be (two grind loops can share a cue).
    struct LoopSlot { uint8_t slot = 0; void* comp = nullptr; };
    LoopSlot   audioLoops_[repl::kAudioMaxLoops];
    float      lastBodyPos_[3] = {};      // where to place a world-anchored sound this frame
    ProxyStats st_;
};

// The pose blob's apply point. Proxy::Apply only STORES the frame's blob, because a pre-tick write is
// stomped by the game's own anim recomputation before render; the loader's POST-hook on
// USkaterAnimInstance::NativeUpdateAnimation calls this with the instance that just updated, and if it
// belongs to one of our proxies the stored blob is applied ON TOP -- the last writer before evaluation.
void AnimPostApply(void* animInstance);

// Replay-driver probe: how often the game's anim update ran for proxy instances (cumulative), and a
// once-per-second log line emitted only while the local player is in replay playback. Measurement
// only -- it changes nothing. It exists to settle whether the graph still updates proxies during a
// local replay, which decides whether the driver lane could ever serve the scrubber.
uint32_t AnimUpdateCalls();
void     ReplayDriverProbe(uint64_t nowMs, void (*logf)(const char*));

// Transported names rejected by a type gate since launch. Rides the 1 Hz board line as `rej=`; 0 in
// normal play.
uint32_t TypeRejects();

}} // namespace omp::game
