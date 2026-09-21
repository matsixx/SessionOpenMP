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
// HOW MANY PLAYERS A LOBBY HOLDS -- one number, because it used to be nine.
//
// Every module that keeps something per peer sizes its table off this: the session's slots, the
// transports' peer tables, the dropped-object assembly buffers, the transported-pose slots and the
// anim post-pass registry. They were nine separate literals, and the bug that produces is not
// hypothetical -- the pose table sat at 8 while the session held 16, so in a lobby of ten the ninth
// proxy on every machine had NO transported pose at all: no sit, no bail rag-doll, no emote, and for
// a pose that MOVES a frozen broken-looking skater, because those packets carry the skeleton INSTEAD
// of the drivers. It took a field round to find. Each site now static_asserts against this, so the
// next person to raise the cap gets a compiler error instead of a silent hole at player N+1.
//
// NOT INCLUDED BY THE RELAY. tools/relay/relay_main.cpp is standalone on purpose -- it is meant to be
// copied to a machine with no game, no UE and no EOS and built with one g++ command (see
// docs/relay-scope.md), so it carries its own kMaxSlots in relay_proto.h with a comment pointing here.
// That one is checked by hand; its roster message is the thing to watch, being the only per-peer
// structure that has to fit inside a single datagram.
//
// WHAT IT COSTS TO RAISE. Bandwidth is a full mesh at ~13 KB/s per other player (omp_wirecost prints
// the table), so each client uploads that times N-1: 1.6 Mbit/s at 16, 3.2 at 32. Memory is dominated
// by dropsync's per-peer assembly buffers, ~215 KB each at a 1024-object cap. Neither is the reason
// to be careful -- the reason is that a remote player is a full anim graph, and that is CPU.
// =====================================================================================================
#pragma once

#define OMP_MAX_PEERS 32
