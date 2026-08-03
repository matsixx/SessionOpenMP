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
// SessionOpenMP -- cosmetics: read our own look, wear someone else's on a proxy.
//
// The fact this file exists for (read out of ASkaterCharacterBase::RefreshVisuals): a skater's look is
// NOT stored on the skater. RefreshVisuals fetches it from the GAME INSTANCE --
// `USessionGameInstance::_skaterInstance` (+0x3f0), whose CustomizationProfile sits at +0x400 -- so
// there is exactly ONE cosmetic identity per process and every skater in the world rebuilds from it.
// That is why proxies wear defaults, and why dressing one means BORROWING the global for the duration
// of a single RefreshVisuals call and putting it back.
#pragma once
#include "../replication/replication.h"

namespace omp { namespace game {

// Read the LOCAL player's look. Enumerates the profile's two TMaps read-only -- a wrong layout
// assumption can only mis-read, never corrupt, and the first call logs what it found so a bad walk is
// obvious immediately instead of shipping silently wrong cosmetics.
bool GatherOwnCosmetics(void* ownPawn, repl::CosmeticSet& out, void (*logf)(const char*));

// Dress a proxy in a peer's look: borrow the global profile, RefreshVisuals(proxy), restore.
// Item names resolve through StaticFindObject -- an item we do not have installed resolves to null and
// that slot is left EMPTY rather than faked (the same gate discipline as trick defs), and it is
// reported as an attributable divergence.
// `unresolved` (optional) receives the count of items that did not resolve locally.
bool DressProxy(void* proxyActor, const repl::CosmeticSet& c, int* unresolved, void (*logf)(const char*));

// FNV-1a over the installed cosmetic-content files (pak names + sizes). Two players with equal digests
// resolve every shared item name to identical content; different digests mean "some items may look
// different on each screen", which is the only honest thing name-sync can say about REPLACEMENT mods.
uint64_t LocalModDigest();

}} // namespace omp::game
