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
// SessionOpenMP -- OUR TEXT ON SCREEN, DRAWN BY THE GAME.
//
// A pool of the game's OWN text widget (PBP_TrickDisplayTextWidget -- one UTextBlock in the menu font,
// which is what the trick readout is made of), created once, added to the viewport, and moved around
// in viewport coordinates every frame. The floating player names are the first user.
//
// WHY THIS EXISTS. The names and the chat have been ImGui drawn from the Present hook since they were
// written. That works, but it is the mod's own surface: a second renderer stitched into the swapchain,
// with its own font atlas, its own frame, its own thread rule, and no relationship to anything the
// game draws. The game already has a text widget with a font -- so the header on nameplates.h that
// says "a UMG billboard needs a widget blueprint and a font asset we do not have" is simply out of
// date, and this is what replaces it. Nothing here touches ImGui, the swapchain or the render thread.
//
// GAME THREAD ONLY, and every engine call is inside SEH. The data it draws is already computed on the
// game thread (publishNameplates does the roster, the head points and the world-to-screen projection),
// so unlike the ImGui path there is no snapshot, no mutex and no boundary to get out of step.
//
// WHAT THE WIDGETS ARE OWNED BY, AND THE ONE RULE THAT MATTERS. AddToViewport gives the viewport a
// reference; the pool holds the pointers. Both die with the LEVEL -- so every entry point here takes
// the world the local pawn is in right now, and the pool is KEYED TO IT:
//   * a DIFFERENT world  -> the old widgets died with the old level; every pointer is dropped WITHOUT
//     being dereferenced;
//   * an UNKNOWN world (null -- mid level change, no pawn yet) -> nothing is touched AT ALL, not even
//     to hide it.
// That is not caution for its own sake. Collapsing a widget that a map change had already freed is
// what crashed the prompt bar in 1.1.12: the write landed in whatever object had been given the
// memory, and the garbage collector found it half a minute later (sit_ui.cpp has the full account).
// Hiding a stale nameplate is the same write for the same reason, so it gets the same rule.
// =====================================================================================================
#pragma once
#include <stdint.h>

struct NameplateItem;   // nameplates.h -- one peer's name, screen position, distance and chat line

namespace omp { namespace ui {

// Symbols resolved and the game's text widget class reachable? False means nothing can be drawn this
// way and the caller should keep using the overlay.
bool GameHud_Available();

// Where to say the one thing about this surface worth saying out loud: what the borrowed panel
// widget turned out to be made of, once, the first time it is built.
void GameHud_SetLog(void (*logf)(const char*));

// ONE FRAME, IN THREE PARTS. Begin takes the world (the rule above) and answers whether anything may
// be drawn at all; the middle calls each claim the widgets they want; End hides everything nobody
// claimed. They belong together -- a surface that drew outside the pair would have its widgets swept
// away by the next End -- so the caller runs all three or none.
bool GameHud_Begin(void* world);

// THE FLOATING NAMES. The same list the ImGui path is published, with the same meaning: `show` is the
// name fade's TARGET (true = the local player is off their board), and an empty list is normal and
// meaningful. Chat bubbles ride the items' `msg` and ignore `show`, as on the other surface.
void GameHud_Names(const NameplateItem* items, int n, bool show);

// THE CHAT: recent lines up a bottom corner, and the line being typed under them. Reads ui/chat.h
// directly -- the model is surface-agnostic and this is one of its two surfaces.
// `menuUp` = the in-game pause menu is really displayed. It moves the box to the RIGHT of the screen
// so it is clear of the menu's own rows; it is drawn OVER the menu regardless.
void GameHud_Chat(bool menuUp);

void GameHud_End();

// The world changed: every widget died with it. Drops the pointers without dereferencing one.
void GameHud_Forget();
// Nobody is in a session any more. With the world still here and still the one they were built in,
// they come off the viewport properly; otherwise this is GameHud_Forget.
void GameHud_Clear(void* world);

// THE CHAT BOX'S LOOK, pushed from the player's settings every frame -- the same rule the nameplate
// tuning follows, so a change from either menu takes effect on the next frame with no apply step.
// `panelPct` and `blurPct` are 0..100; the rest are in slate units.
void GameHud_SetChatLook(int textSize, int smallSize, int panelPct, int blurPct);

// Is the game-drawn path the one in use? When false the overlay still owns the names and
// GameHud_Names does nothing -- the A/B, and the way back if the game's own widget ever misbehaves
// on somebody's machine. Not persisted: this is a fallback, not a preference.
bool GameHud_Enabled();
void GameHud_SetEnabled(bool on);

// One short line for the 1 Hz heartbeat: what the pool is doing and why nothing is on screen when
// nothing is. Returns the length written.
int  GameHud_Status(char* out, int cap);

} }  // namespace omp::ui
