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
// SessionOpenMP -- the in-game chat box.
//
// WHY IT IS NOT A PAUSE-MENU PAGE: a menu page is a list of rows the engine builds when a page
// activates, and it cannot take free text at all -- the same reason the join-code prompt lives in
// ImGui (see mp_name.h). Chat also has to be readable and typable WHILE SKATING, which a pause menu
// by definition is not.
//
// THE THREAD RULE IS overlay.h's, because this is drawn from the Present hook:
//   * the RENDER thread owns the window, the input line and the history rendering
//   * the GAME thread pushes received messages in (Chat_Push) and drains typed ones out (Chat_Take)
//   * everything crossing that line goes through the mutex in chat.cpp, and nothing in this file ever
//     touches Unreal.
#pragma once
#include <stdint.h>

// Live knobs, so the look can be corrected in-game without a rebuild.
struct ChatTuning {
    bool  enabled       = true;
    float width         = 620.0f;   // px at 1080p-equivalent; scaled by the ImGui global scale
    float height        = 260.0f;   // history area when open
    float marginX       = 40.0f;    // from the left edge
    float marginY       = 120.0f;   // from the BOTTOM edge
    float fadeAfterSec  = 9.0f;     // how long a message stays visible with the box CLOSED
    float fadeOverSec   = 1.5f;
    int   maxShownIdle  = 6;        // lines drawn when closed
};
ChatTuning& Chat_Tuning();

// ---- render thread -----------------------------------------------------------------------------
void Chat_Draw();                       // called every frame from the Present hook
bool Chat_IsOpen();                     // true = the box owns the keyboard (the overlay gate reads this)
void Chat_SetOpen(bool open);
// True when there is anything on screen -- open, or recent lines still fading. The Present hook only
// pays for an ImGui frame when something actually wants to draw.
bool Chat_HasVisible();

// ---- game thread -------------------------------------------------------------------------------
// A line to display. `mine` picks the "you" colour; a null/empty name renders as a system line, which
// is how join/leave notices reach the same window without a second mechanism.
void Chat_Push(const char* name, const char* text, bool mine);
// System line (join/leave notices). Implemented and ready; nothing calls it yet.
void Chat_System(const char* text);
// Drain one line the player typed. Returns false when there is nothing to send. The caller owns
// transmitting it AND echoing it locally -- chat.cpp deliberately does not know a transport exists.
bool Chat_Take(char* out, int cap);

// The chat's own tunables live above; the LOOK (palette + font) is shared with every other surface
// this mod draws -- see theme.h.
