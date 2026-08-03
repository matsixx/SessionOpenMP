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
// SessionOpenMP -- publish-side state reader. Reads what the game already computed for OUR pawn; derives
// nothing. See gather.cpp for the provenance of every offset it touches.
#pragma once
#include "../replication/replication.h"

namespace omp { namespace game {

// false = our state could not be read this frame (no pawn, menu, torn read). The session then publishes
// nothing rather than sending a half-built pose.
bool GatherOwnState(void* pawn, repl::State& out);

// Optional log sink for the publish side. Deliberately sparse: gather runs every frame on the game
// thread, so it only ever reports ONE-TIME facts (currently the board movement-mode accessor check).
void SetGatherLog(void (*logf)(const char*));

}} // namespace omp::game
