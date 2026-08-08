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
// SessionOpenMP -- multiplayer inside the GAME'S OWN pause menu.
//
// WHY, given F1 already works: the F1 overlay is an ImGui window floating over the game -- fine for a
// developer, foreign to a player. Session's pause menu is data, not blueprint logic (a
// `UMenuPageDefinition` holding a `TArray<FMenuPageItemDefinition>`), so a native-looking
// "Multiplayer" row costs one hook and no UI code at all. The F1 overlay STAYS, unchanged: it is the
// dev surface and the fallback if a game patch ever moves these offsets.
//
// THE THREAD RULE IS THE OPPOSITE OF overlay.h. Everything here runs on the GAME thread (inside the
// engine's own menu code, reached from the engine tick), so it may touch game state directly and
// must NEVER be called from the render thread. The two UIs share only `MpUiState` and `OvAction`.
//
// HOW THE INJECTION WORKS (see game_syms.h for the disassembly notes):
//   * `UMenuPage::CreatePageItems(page, TArray<FMenuPageItemDefinition>* items, bool)` builds one row
//     widget per element and is the LAST step of a page activation. The pre-hook hands it a DIFFERENT
//     array -- one owned here -- so the page's own `_pageItemDefinitions` is never written and nothing
//     of the mod's is ever owned, destructed or freed by the engine.
//   * Navigation counts WIDGETS (`_pageItemWidgets`), and the confirm params are built from each
//     widget's OWN embedded copy of the definition -- so an injected row behaves exactly like a stock
//     one all the way to the handler, and its `_key` arrives intact.
//   * The visibility filter runs BEFORE CreatePageItems, and the enabled check only overwrites keys it
//     recognises, so an injected row is never filtered out or greyed.
#pragma once

struct MpUiState;   // overlay.h -- the same snapshot the F1 menu displays

// Live knobs, so a bad in-game symptom can be bisected without a rebuild. Each one narrows what the
// integration does, down to doing nothing at all.
// (Page dumping lives in omp::debug::Get().menuPages -- see src/debug.h.)
struct PauseMenuTuning {
    bool enabled     = true;    // master kill switch (false = the game's menu is untouched)
    bool injectRow   = true;    // add the "Multiplayer" row to the pause page
    bool subPage     = true;    // false = the row is present but inert, to isolate row injection
                                // from page navigation when something misbehaves
    bool statusText  = true;    // put session status in the footer description line
    int  maxItems    = 64;      // hard cap on the substituted array -- over it, skip; never truncate
    bool nativeScroll = false;  // OFF: leaving _maxVisibleItems alone does NOT make our pages
                                // scroll, it just truncates them. CreatePageItems builds
                                // [headerIndex, min(headerIndex + _maxVisibleItems, ourNum)) from the
                                // array we hand it and never writes _pageItemDefinitions, so the
                                // engine's scroll math still sees the ROOT page's short list and
                                // decides there is nothing to scroll. Until the count the scroll math
                                // reads reflects our rows, forcing every row to build is strictly
                                // better -- all of them at least exist.
};
PauseMenuTuning& PauseMenu_Tuning();

void PauseMenu_Install();                   // game thread, from on_unreal_init (after MinHook is up)
// Game thread, EVERY frame from the engine-tick anchor. Performs page swaps queued by a confirm.
// The deferral is mandatory (see refreshPage): the engine broadcasts the confirm from the middle of
// `OnConfirmAction` and keeps using the row widgets afterwards, so rebuilding them inside the callback
// is a use-after-free plus an out-of-bounds index. One frame later is safe.
void PauseMenu_Pump();
void PauseMenu_Publish(const MpUiState* s); // game thread -> what the injected rows display
int  PauseMenu_TakeAction();                // game thread; returns OvAction and clears it
int  PauseMenu_TakeJoinIndex();             // the browse index that OVA_JOIN_INDEX refers to
// Who OVA_KICK / OVA_BAN refer to. An IDENTITY, never a row position: the roster rebuilds live, so an
// index would silently re-point at somebody else between the press and the action.
bool PauseMenu_TakePeerId(char* idOut, int idCap, char* nameOut, int nameCap);
