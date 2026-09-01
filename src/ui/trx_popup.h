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
// SessionOpenMP -- the game's own popup, for messages that have to be seen.
//
// GAME THREAD ONLY. It calls into the engine's UI, and the popup manager is a game-instance
// subsystem: touching either from the render thread or a worker is the usual way to take the game
// down. Everything here is inside SEH, and every failure is a false return and a log line rather
// than a crash -- a message the player misses is never worth the game dying for.
#pragma once

namespace omp { namespace ui {

// Is the popup system reachable yet? False before the game instance and its subsystems exist, which
// is most of startup -- so this is also "are we far enough into the boot to say anything".
bool TrxPopup_Available();

// One popup, one button. Returns false if it could not be shown, for any reason.
bool TrxPopup_Show(const char* title, const char* body, const char* buttonText,
                   void (*logf)(const char*) = nullptr);

} }  // namespace omp::ui
