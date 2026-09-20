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
// SessionOpenMP -- the release notes in the game's own NEWS ARTICLE widget (WBP_DefaultNewsContent).
//
// OUR OWN INSTANCE, never the news SYSTEM -- see news_panel.cpp for why that distinction matters (its
// flow carries the EULA). Preferred over the message panel because the article's body sits in a scroll
// box, so the notes are not limited to one screenful.
// =====================================================================================================
#pragma once

namespace omp { namespace ui {

// Is the widget reachable -- symbols resolved and its class already loaded? False means the caller
// should use the message panel instead.
bool NewsPanel_Available();

// Put the notes on screen. `date` may be null or empty, in which case the date line is collapsed.
// False = it could not be shown, for any reason, and nothing was left on the viewport.
bool NewsPanel_Show(const char* title, const char* body, const char* date,
                    void (*logf)(const char*) = nullptr);

// GAME THREAD, every frame while it is up: moves the description under the stick, the d-pad or the
// arrow/page keys. The panel is deliberately UNFOCUSABLE -- taking focus stopped the menu's own X
// working -- so the scrolling cannot come from Slate and is driven from here instead.
void NewsPanel_Tick();

bool NewsPanel_Showing();
void NewsPanel_Close();

} }  // namespace omp::ui
