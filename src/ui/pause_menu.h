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
// Is the pause page this code last built still realised on screen? Read off the widget's own Slate
// handle -- the game's IsPauseMenuDisplayed is always false with world pausing disabled.
bool PauseMenu_IsShown();
// The world changed: whatever page was last built belongs to a menu that no longer exists, and its
// memory may already hold something else. Forget it rather than read it.
void PauseMenu_ForgetPage();

// ==== KEEPING THE PAUSE MENU'S KEYBOARD FOCUS =========================================================
// THE BUG: with the menu up, control sometimes goes to the SKATER instead -- the menu stops answering
// and there is no way out of it. It has been around since the world-freeze was disabled.
//
// WHAT IT IS: the menu is a UMG widget and answers keys through UPauseMenuPageContainer::NativeOnKeyDown,
// which only runs while it holds Slate keyboard focus. Lose that and the keys fall through to the
// player controller -- so the skater moves and the menu is deaf. Field confirmation: the replay-editor
// button still worked while stuck (it is a controller action, not a menu one), and going in and out of
// the editor fixed it, because that transition sets focus properly on the way out.
//
// WHY THE FREEZE MATTERS: paused, the pawn does not tick, so losing focus was invisible -- the menu was
// just as deaf, but nothing moved to tell you. Running, the same loss is obvious AND reachable. The
// likely trigger is nearby in the same class: `_gameWasPausedBeforeAltTab`. Coming back from alt-tab
// the game restores the pause it thinks it had, and our patch makes that fail.
//
// THE REPAIR, AND WHY IT IS NOT A BEAT. The first version put focus back on a 250 ms beat, on the
// grounds that re-asserting focus already held costs nothing. That is true of the MENU and false of
// everything else: a confirm dialog opened from inside the menu -- "hold X to apply" on a graphics
// change -- takes focus legitimately, and the beat took it straight back, so the dialog could not be
// used at all. A repair that breaks a working path is worse than the fault it repairs.
//
// So it fires on EVENTS instead, and only ones where nothing else can reasonably own focus:
//   * the menu becoming displayed (it should be the menu's, and briefly after, so the game's own
//     setup finishes first);
//   * the window being re-activated, which is the suspected trigger -- see _gameWasPausedBeforeAltTab.
// A dialog opened while navigating the menu happens at neither moment, so it is never disturbed.
//
// What this gives up: if focus is lost at some third moment, nothing puts it back. Catching that
// needs to ask Slate who actually holds focus, which needs another signature, and is worth doing only
// if the fault is seen again. `PauseMenuFocusFix` in the ini turns the whole thing off.
void PauseMenu_KeepFocus(bool menuDisplayed, void (*logf)(const char*));
// WINDOW-MESSAGE THREAD: the window just became active again. Consumed by the next game frame.
void PauseMenu_NoteWindowActivated();
// How many times the IN-GAME pause page has been built. The start menu and the pause menu are the
// same kind of thing to the rest of the mod; the page key is what tells them apart, and this counter
// is the only place that distinction is available outside this file. Watch it for CHANGE, not value.
unsigned PauseMenu_PausePageBuilds();
int  PauseMenu_TakeAction();                // game thread; returns OvAction and clears it
int  PauseMenu_TakeJoinIndex();             // the browse index that OVA_JOIN_INDEX refers to
// Who OVA_KICK / OVA_BAN refer to. An IDENTITY, never a row position: the roster rebuilds live, so an
// index would silently re-point at somebody else between the press and the action.
bool PauseMenu_TakePeerId(char* idOut, int idCap, char* nameOut, int nameCap);
