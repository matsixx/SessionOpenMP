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
// SessionOpenMP -- per-host moderation: who you refuse to play with.
//
// EVERY PLAYER POLICES THEIR OWN SESSION AND NOBODY ELSE'S. That is not a design preference, it is
// the only authority that exists here: `EOS_Lobby_KickMember` requires the caller to be the lobby
// owner, and in an overlay design a "host" is simply the peer who created the lobby. So there is no
// global ban and no admin-over-other-people's-games -- each host keeps their own list, and it applies
// to every session they run.
//
// The list is a plain text file beside the log, one entry per line: `<productUserId> <name>`. Plain
// text on purpose -- it is the host's OWN list about their OWN sessions, so there is nothing to
// protect it from; being able to read and edit it by hand is a feature.
#pragma once

enum { OMP_BAN_MAX = 128 };

// `dir` is the folder holding SessionOpenMP.log. Loads the saved list; absence is normal.
void Ban_Init(const char* dir, void (*logf)(const char*));
bool Ban_Add(const char* peerId, const char* name);   // false = full, or already listed
bool Ban_Is(const char* peerId);
// Read/undo half of the list. Complete and tested, but no UI reaches them yet -- unbanning today
// means editing the text file. Wire these up to a menu page if you want it in-game.
bool Ban_Remove(const char* peerId);
int  Ban_Count();
bool Ban_At(int i, char* idOut, int idCap, char* nameOut, int nameCap);

// There is no protected-identity list here, and one must not be added.
// A host's authority over their own session is the entire point of this file; carving out an
// exception -- for anyone, including the author -- takes a right away from the person running the
// game. It would also be self-defeating in published GPL source: the constant would sit in plain
// view, and the id it protected is printed in every log line the mod writes.
