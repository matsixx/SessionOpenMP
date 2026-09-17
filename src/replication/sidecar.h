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
// SessionOpenMP -- THE REPLAY SIDECAR: the peers of a saved replay, in a file beside it.
//
// The game's replay file holds only what its own recorder captured, and peers never enter that
// recorder (proxy.cpp clearReplayTags). A peer shown in the editor is driven LIVE from a synced copy
// of their own state history, so nothing of theirs survives a save. This file does: for each peer
// synced at the moment of saving, their history as the wire-format entry stream replaysync exports,
// the cosmetics that dress them, their skeleton fingerprint, and ONE time anchor -- their history
// time at the END of the replay's timeline. Playback then samples at anchor - (total - cur), which
// owes nothing to any session clock. Local only: a replay file handed to somebody else plays back
// with no peers unless this travels with it.
//
// This is the CODEC only (game-free, gated by omp_looptest). File I/O lives in the loader, the
// playback in session/replay_ghosts.
//
// FORMAT (little-endian): u32 "OMPS", u16 version, u8 wireMajor, u8 wireMinor, u32 peerCount, then
// per peer: u32 blockLen (everything after it), name[40], u32 sizeof(CosmeticSet), CosmeticSet,
// u8 haveWear, u32 sizeof(WearSet), WearSet, u8 haveSkel, u32 sizeof(SkelPrint), SkelPrint,
// u64 anchorEndUs, u32 entryCount, u32 entriesLen, entries. The struct sizes are the layout check: a
// build whose structs moved refuses the file rather than reading garbage as a wardrobe.
// =====================================================================================================
#pragma once
#include "replication.h"
#include <cstdint>

namespace omp { namespace sidecar {

constexpr uint32_t kMagic    = 0x53504D4Fu;   // "OMPS" -- a FILE magic, not an OMP? wire letter
constexpr uint16_t kVersion  = 1;
constexpr int      kMaxPeers = 8;
constexpr int      kNameMax  = 40;

struct Peer {
    char              name[kNameMax] = {};
    repl::CosmeticSet cosmetics;
    bool              haveWear = false;
    repl::WearSet     wear;
    bool              haveSkel = false;
    repl::SkelPrint   skel;
    uint64_t          anchorEndUs = 0;      // their history time at the replay's end
    uint32_t          entryCount = 0;
    const uint8_t*    entries = nullptr;    // replaysync entry stream; on Unpack, points INTO the file bytes
    uint32_t          entriesLen = 0;
};

// -> bytes written, 0 = nothing to write or `out` too small (`out` null asks for the size).
uint32_t Pack(const Peer* peers, int n, uint8_t* out, uint32_t cap);
// `why` (optional) names the refusal. Peers past `cap` are dropped, never an error.
bool     Unpack(const uint8_t* d, uint32_t len, Peer* out, int cap, int* nOut, char* why, int whyCap);

}} // namespace omp::sidecar
