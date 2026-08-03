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
// SessionTweaks -- the shell (log, ini, MinHook init, F1 registration, UE4SS plumbing).
#pragma once

// Called once per InputHandler::Tick from the scoop module's hook -- the one reliable game-thread
// heartbeat the mod owns. Currently just re-offers the F1 menu registration until the host appears.
void Tweaks_PumpFrame();
