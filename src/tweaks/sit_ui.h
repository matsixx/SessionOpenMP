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
// SessionTweaks -- the on-screen prompt for sitting, built from the game's own button-prompt widget
// so it matches the rest of the HUD. See sit_ui.cpp; driven entirely by sit.cpp.
#pragma once
#include <stddef.h>
void SitUI_Install();                          // sig-scan + the class-discovery hook; non-fatal
void SitUI_ReadConfig(const char* iniText);
void SitUI_SaveConfig(char* iniText, size_t cap);
// One line of the prompt bar: what a button does. `button` is the Xbox name ('A' 'B' 'X' 'Y'); the
// PlayStation equivalent is drawn on a DualSense. `ring` (0..1) fills the button's progress ring --
// the hold-to-confirm feedback the game's own prompts use.
struct SitPromptEntry { const char* label; char button; float ring; };
// GAME THREAD, every frame while the feature is in play: `show` puts the bar on screen with these
// entries (entry 0 is the prompt's own button, the rest are rows along the bar to its left). `skater`
// is the pawn the widgets belong to -- a different one means a new level, and the old widgets are
// dropped rather than touched. Up to six entries.
void SitUI_PumpFrame(void* skater, bool show, const SitPromptEntry* entries, int count);
const char* SitUI_Status();
// GAME THREAD: collapse every row NOW, without waiting for the next pump. The pump rides
// InputHandler::Tick, which the replay and prop editors stop -- so the bar has to be hidden from
// whatever is still ticking, or it stays on screen for as long as the editor is open.
void SitUI_HideNow();
