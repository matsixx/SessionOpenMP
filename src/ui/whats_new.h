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
// SessionOpenMP -- WHAT'S NEW, in the game's own popup.
//
// The same surface the game shows its own notices on (UTRXPopupManager -- the family the MyNacon offer
// at start-up belongs to), driven through ui::TrxPopup_Show. It appears ONCE per build, at the main
// menu, the first time a player runs a version they have not seen: the version is remembered in the
// preferences file (`SeenVersion`), so clearing that line shows the notes again.
//
// WHY A HAND-WRITTEN SUMMARY AND NOT THE CHANGELOG FILE: the popup is one page of body text with one
// button, and CHANGELOG.md runs to hundreds of lines per release and is not shipped (it is gitignored,
// developer-facing, and full of offsets and field notes). What belongs here is the few lines a PLAYER
// would want -- so the notes live as text in the .cpp, next to the version they describe, and are
// updated with the version number as part of cutting a release.
// =====================================================================================================
#pragma once

namespace omp { namespace ui {

// Has this build's What's New already been seen? False the first time a new version runs.
bool WhatsNew_Pending();

// Show it, if it is pending and the popup system is up. Returns true once it has actually been shown
// (and remembers it, so it will not appear again for this build). Safe to call every menu draw.
bool WhatsNew_ShowIfDue(void (*logf)(const char*) = nullptr);

// Show it regardless of whether it has been seen -- for a menu row, or after the player asks for it.
bool WhatsNew_ShowNow(void (*logf)(const char*) = nullptr);

// THE NOTES AS PART OF THE START MENU. Call every frame with `want` = "the start menu is up and the
// player has them switched on" (MpPrefs_ShowChangelog). They go up when it becomes true and come down
// when it becomes false; there is no key, because every button a pad has at a menu already belongs to
// the game (see whats_new.cpp). Building retries at a second's pace and gives up after a while.
void WhatsNew_KeepUp(bool want, void (*logf)(const char*) = nullptr);

} }  // namespace omp::ui
