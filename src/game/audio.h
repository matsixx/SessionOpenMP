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
// =====================================================================================================
// SessionOpenMP -- AUDIO SYNC: capture the local player's sounds at the game's own sound-spawn funnel,
// transport cue + parameters, re-issue them on the peer's proxy.
//
// The principle this module exists to express: REPLICATE AT THE LAYER WHERE THE DATA IS ALREADY FULLY
// DETERMINED. Transporting gameplay EDGES and calling the game's own handlers (HandleOnEnterGrind,
// PlayRevertSound, ...) on the proxy does not work: those handlers do not merely play a sound, they
// INTERROGATE THE MOVEMENT COMPONENT, which on a wire-driven proxy is not simulating and answers
// "nothing". A gameplay event is UPSTREAM of a pile of local derivation a proxy cannot do. A
// sound-SPAWN call is DOWNSTREAM of all of it: cue, place, volume, pitch, parameters -- nothing left
// to derive, nothing left to interrogate.
//
// The game itself is built the same way:
//   * Session routes its skate audio through `UReplayAudioManager`, whose four static delegates exist
//     so its REPLAY recorder can capture sound with parameters and re-spawn it later.
//   * `ASkateboardEx::Tick` MUTES its whole audio block at board movement mode 9 and lets that replay
//     path drive the sound instead. A proxy is exactly a replay -- externally driven motion whose
//     audio must come from recorded events -- so this is the network version of a shipped path.
//   * `FReplayAudioSound::StartReplayAudio` re-spawns a captured sound on a DIFFERENT component, and
//     the captured params struct carries `_attachToComponentOwnerName`/`_attachToComponentName` --
//     FName fields whose only purpose is re-resolving the attach target across a serialization
//     boundary, i.e. name-based replication designed in.
//
// Measured coverage: 30 call sites reach the two spawn funnels, covering the continuous loops AND the
// one-shots -- grind in/out, revert, impacts, banking, board broken, push, board hit and powerslide.
// It also carries sounds nobody enumerated, including `FSessionUtils::UpdateAudioWithSurfaceType`, so
// a peer's grind sounds like the concrete they are actually on without transporting any surface state.
// =====================================================================================================
#pragma once
#include "../replication/replication.h"
#include <cstdint>

namespace omp { namespace game { namespace audio {

struct Tuning {
    bool  enabled          = true;   // master kill switch: false = capture nothing, play nothing
    // TYPE GATE: a cue name arrives from a peer and is resolved with StaticFindObject(ANY_PACKAGE),
    // which matches ANY class -- so without this the sender picks the type of a pointer that is handed
    // to GetConcurrencyHandles (and written through) and to the spawn. Rejection is the same silent,
    // counted path as a cue this install does not have. Off only to A/B a legitimate cue that fails it.
    bool  typeGate         = true;
    // Hook the ENGINE layer under the funnel as well. Measured bypasses that only this catches:
    // ASkateboardEx::StartGoingFastLoopAudio (the wind loop) spawns through UGameplayStatics
    // directly, and AdjustGoingFastAudio sets its volume/pitch on the component directly.
    bool  captureBypasses  = true;
    // Anim-notify sounds travel like everything else rather than being left to the proxy's own anim
    // graph: a proxy does not reproduce them, because its graph never plays the montage that carries
    // the notify (the trick pop is a notify sound and exists nowhere in the board's audio asset). The
    // proxy's OWN notify sounds are therefore muted so nothing can play twice, keeping one source of
    // truth. Both are identified by RETURN ADDRESS inside UAnimNotify_PlaySoundRecorded::PlaySound --
    // every Session notify that plays a sound routes through it.
    bool  transportAnimNotify = true;
    // Muting returns null from the spawn WITHOUT calling the original. Safe:
    // UAnimNotify_PlaySurfaceTypeSound::PlaySound null-checks the result (`test rax,rax; je`) before
    // touching it, as any UE caller must, since SpawnSound* legitimately returns null.
    bool  suppressProxyNotify = true;
    // A sound spawned at a WORLD POINT carries no owner, so it is attributed by proximity to our own
    // board/skater. Generous on purpose: skate sounds spawn on the board, and in a solo game the only
    // simulating skater is us (proxies do not run the code that spawns these).
    float captureRadiusCm  = 400.0f;
    // A live loop is touched by the game's audio block every 0.05 s. Untouched for this long, and not
    // reporting IsPlaying, means it stopped -- there is NO stop funnel and no _onStop delegate, so
    // loop lifetime is entirely ours to observe.
    uint32_t loopTtlMs     = 250;
    // Log each distinct cue name once, with where it came from. This is the instrument that names
    // anything still missing: a sound absent from the SENDER's inventory bypassed the funnel and
    // needs its own hook; one absent from the RECEIVER's resolve log is a cue this install lacks.
    bool  inventoryLog     = true;
};
Tuning& Tune();

struct Stats {
    uint32_t captured = 0, oneShots = 0, loopsStarted = 0, rejected = 0, notifySkipped = 0, notifyMuted = 0;
    uint32_t played = 0, unresolved = 0, playStarted = 0, playStopped = 0;
    // Transported cue names that resolved to an object of the WRONG TYPE. Distinct from `unresolved`
    // on purpose: "you do not have this sound" is normal across installs, "that name is not a sound at
    // all" is not, and merging them would hide the second in the first.
    uint32_t rejectedType = 0;
    uint32_t faults = 0;
    uint8_t  liveLoops = 0;
};
Stats GetStats();

// Install the capture detours. Idempotent, and called only once a session ARMS -- the mod is inert
// until then by standing rule, and these sit on a path every sound in the game takes.
bool Install(void (*logf)(const char*));
// Who "we" are, for attribution. Refreshed every frame from the loader's own pawn discovery; a null
// pawn disables capture rather than guessing.
void SetLocalParts(void* pawn, void* board);
// Fill the publish snapshot's audio section: the loops still playing, plus any one-shots fired since
// the last call (repeated across the next few packets by the redundancy window).
void Gather(repl::State& s, uint64_t nowUs);

// ---- receiver side ----------------------------------------------------------------------------------
// Every playback runs with an "this is a replay" flag set, so our own capture hooks ignore the sounds
// we ourselves spawn -- without it a mesh of 3+ players would re-capture and re-transmit each other's
// audio forever.
// Returns the spawned UAudioComponent* (loops) or nothing (one-shots). `bodyPos` is the proxy's
// interpolated body position: audio positions travel RELATIVE to it so a sound lands with the
// jitter-buffered animation that caused it rather than ahead of it.
void* PlayLoop(const repl::AudioLoop& l, void* actor, void* board, const float* bodyPos);
void  UpdateLoop(void* comp, const repl::AudioLoop& l);
void  PlayOneShot(const repl::AudioEvent& e, void* actor, void* board, const float* bodyPos);
void  StopSound(void* comp);
// True while we are re-issuing a captured sound. Anything that could re-enter the funnel checks it.
bool  Replaying();
// Register a live proxy so its OWN anim-notify sounds can be muted (its audio comes off the wire
// instead). Called every frame from Proxy::AudioApply; entries expire on their own, so a forgotten
// or destroyed proxy cannot leave a stale pointer behind to be dereferenced.
void  NoteProxy(void* actor, void* board);

}}} // namespace omp::game::audio
