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
// SessionOpenMP -- the in-game menu (F1). Dear ImGui drawn from a hooked IDXGISwapChain::Present.
//
// WHY A PRESENT HOOK AND NOT SOMETHING NATIVE: Session ships no usable mod UI, its UMG widgets need
// blueprint assets that are not available, and UE4SS's own ImGui console is a SEPARATE WINDOW (and is
// kept disabled -- its object listeners race the async loader and crash the game). A Present hook
// draws INSIDE the game window with no engine cooperation at all, on both the DX11 and DX12 RHI.
//
// THE THREAD RULE, which is the whole reason this interface is so small: the UI runs on the RENDER
// thread. It may NEVER call the session, the transport, or anything Unreal. So:
//   * the game thread PUBLISHES a snapshot of what to display (Overlay_Publish)
//   * the render thread only POSTS an action (a click), which the game thread DRAINS and performs
//     (Overlay_TakeAction) on its own next run -- exactly like a hotkey press.
#pragma once

enum OvAction {
    OVA_NONE = 0,
    OVA_HOST_LOCAL,      // shared memory: 2+ instances on THIS PC
    OVA_JOIN_LOCAL,
    OVA_HOST_ONLINE,     // EOS: between machines
    OVA_JOIN_ONLINE,
    OVA_LEAVE,
    // Posted only by the pause menu's lobby browser (the F1 window has no browser):
    OVA_BROWSE,          // bring EOS up if needed, then run a lobby search that KEEPS its results
    OVA_JOIN_INDEX,      // join the browse result whose index rides alongside (PauseMenu_TakeJoinIndex)
    // Private games. A private lobby is an ordinary EOS lobby carrying a join CODE; the public
    // browser skips any lobby that has one, so the code is the only way in.
    OVA_HOST_PRIVATE,    // make a code, host with it
    OVA_JOIN_PRIVATE,    // open the code prompt (the overlay owns the typing)
    // Moderation of YOUR OWN session (EOS grants kick only to the lobby owner):
    OVA_KICK,            // remove the selected player from the session this instance hosts
    OVA_BAN,             // ...and never host them again (PauseMenu_TakePeerId supplies who)
    OVA_SET_NAME,        // open the name box (a menu page cannot take text itself)
    // Direct UDP -- no Epic at all. F1 ONLY: joining needs a typed address, and the game's pause menu
    // has no text-entry widget (see mp_name.h). The address is set through omp::SetDirectEndpoint
    // before the action is posted, so by the time the game thread acts on it the target is already in.
    OVA_HOST_DIRECT,
    OVA_JOIN_DIRECT,
};

// What the menu displays. Plain data, copied under a lock -- no pointers into game state.
struct MpUiState {
    int   backend = 0;          // omp::Backend: 0 none, 1 EOS, 2 shared memory, 3 direct UDP
    int   tpState = 0;          // 0 not started, 1 initializing, 2 ready, 3 failed
    int   lobby = 0;            // omp::LobbyStatus()
    bool  armed = false;
    int   peers = 0, proxies = 0;
    float pubHz = 0;
    char  myMap[40] = {0};      // the local INTERNAL level name -- the roster resolves it to a label
    char  bound[64] = {0};      // direct UDP: the local "ip:port" we are actually listening on, so a
                                // host can read their own port back instead of trusting they typed it
};

void Overlay_Install();                          // async; hooks Present once the game swapchain exists
bool Overlay_Visible();
void Overlay_Publish(const MpUiState* s);        // game thread -> UI
int  Overlay_TakeAction();                       // game thread; returns OvAction and clears it
// ---- TYPED INPUT. A pause-menu page is a list of rows and cannot take free text at all, so anything
// the player has to TYPE opens a small themed box over the menu. Opening one does NOT open the F1
// panel; it owns the keyboard while it is up.
void Overlay_PromptCode(bool open);              // any thread -- join a private game
void Overlay_PromptName(bool open);              // any thread -- change your multiplayer name
bool Overlay_PromptOpen();                       // true while EITHER box is up
bool Overlay_TakeCode(char* out, int cap);       // game thread; true once, when a code was submitted
// The name box validates and SAVES on its own -- MpName_Set is already called from the render thread
// by the F1 panel, so there is nothing for the game thread to collect. It only needs to know the name
// CHANGED, so it can re-publish the lobby advert.
bool Overlay_TakeNameChanged();                  // game thread; true once per change
// The address/port typed into the direct-connect section, parked when OVA_HOST_DIRECT/OVA_JOIN_DIRECT
// was posted. Read on the GAME thread, which is the only place allowed to hand it to the transport.
void Overlay_TakeDirect(char* addrOut, int cap, int* portOut);
void OvLog(const char* msg);                     // implemented by the loader (writes SessionOpenMP.log)
