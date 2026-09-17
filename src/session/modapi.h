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
// SessionOpenMP -- THE MOD CHANNEL: other mods' own messages, carried between players.
//
// Other mods register a named channel and send bytes to one player or everyone; OpenMP moves them
// over the session transport and never reads them. The public contract is sdk/omp_mod_api.h (exported
// as OmpMod_* by the loader) for DLL mods, the `OpenMP` table (loader/lua_api.cpp) for UE4SS Lua mods,
// and docs/modding-api.md. Once released, nothing in the public half may
// change meaning -- functions are only ever added, with kApiVersion bumped.
//
// WIRE: magic "OMPm", then a lane version byte and a kind byte.
//   LIST (kind 1): gen u16, part u8, parts u8, idLen u8, id, count u8, count x (len u8, name).
//          The sender's registered channel names and its own id, resent every 2 s. Self-healing:
//          a receiver replaces its view of that peer when every part of one generation has arrived,
//          and forgets it after 7 s of silence -- no reliable ordering or reset message is assumed.
//   DATA (kind 2): FNV-1a hash of the channel name u32, then the mod's payload (<= kMaxPayload).
//          Delivered only when the sender announced that channel under the SAME name we registered.
//
// OLDER BUILDS. A pre-1.1.12 build hands any packet type it does not know to the snapshot parser and
// tells the player "A player here is running a different SessionOpenMP version". So this lane is only
// ever SENT to a peer whose snapshots report wire minor >= kCapableMinor (or who sent us a list).
//
// THREADS. The public functions lock and may be called from any thread. Callbacks only ever run on the
// game thread, from OnPacket / Frame / Reset, never with the lock held.
// =====================================================================================================
#pragma once
#include <cstdint>

namespace omp { namespace modapi {

constexpr int     kApiVersion   = 1;
constexpr int     kMaxPayload   = 1000;   // every backend carries 1024 B per message; 10 B of header
constexpr int     kMaxChannels  = 32;     // registered in this game
constexpr int     kNameMax      = 63;     // channel name characters: A-Z a-z 0-9 _ . -
constexpr uint8_t kCapableMinor = 3;      // repl::kWireMinor at which a peer understands this lane

using OnMessageFn = void (*)(int player, const uint8_t* data, int len, void* user);
using OnPlayerFn  = void (*)(int player, int joined, void* user);

// ---- the public half (OmpMod_*). `player` is the transport peer index.
int   Register(const char* channel, OnMessageFn onMessage, OnPlayerFn onPlayer, void* user);  // > 0, or 0
void  Unregister(int handle);
int   Send(int handle, int player, const uint8_t* data, int len, int reliable);   // 1 queued, 0 refused
int   InSession();
int   Players(int handle, int* out, int cap);   // players who have this channel; returns the total
int   IsAuthority(int handle);
int   Authority(int handle);                    // -1 = this game
int   PlayerName(int player, char* out, int cap);
int   PlayerId(int player, char* out, int cap);
int   LocalName(char* out, int cap);
int   LocalId(char* out, int cap);
void* PlayerActor(int player);
int   ActorPlayer(void* actor);

// True on the thread that runs Frame (the game thread). PlayerActor answers live there, and from the
// cache of the last frame anywhere else.
bool  OnGameThread();

// ---- the session half. Game thread only.
void SetLogger(void (*logf)(const char*));
bool IsPacket(const uint8_t* data, int len);
void OnPacket(int peerIdx, const uint8_t* data, int len, uint64_t nowUs);
void NoteSnapshot(int peerIdx, uint8_t wireMinor);   // every parsed snapshot: the capability signal
void Frame(uint64_t nowUs);                          // roster, lists, outbox, expiry
void Reset();                                        // the session ended: everyone leaves every channel

}} // namespace omp::modapi
