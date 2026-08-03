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
// SessionOpenMP -- ONE look for every ImGui surface this mod draws.
//
// WHY IT IS ITS OWN FILE: colour constants copied into a second file drift the first time one of them
// is tuned, and the surfaces then read as two different products in the same screenshot. The palette,
// the font and the push/pop live here, and every surface calls the same pair.
//
// RENDER THREAD ONLY, like everything else drawn from the Present hook -- with the exception of
// Theme_OfferFont, which is the game thread handing bytes across (see the note there).
#pragma once
#include "imgui.h"

// Session's pause menu: near-black panels, a thin light rule, pale slightly-tracked text, and one
// warm accent. No rounding anywhere, no gradients.
struct ThemePalette {
    ImVec4 panel      = ImVec4(0.05f, 0.05f, 0.06f, 0.82f);   // translucent (chat, drawn over play)
    ImVec4 panelSolid = ImVec4(0.05f, 0.05f, 0.06f, 0.94f);   // when it owns the screen
    ImVec4 text       = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
    ImVec4 dim        = ImVec4(0.62f, 0.62f, 0.62f, 1.00f);
    ImVec4 accent     = ImVec4(0.98f, 0.78f, 0.22f, 1.00f);   // the game's highlight amber -- "you"
    ImVec4 other      = ImVec4(0.55f, 0.80f, 1.00f, 1.00f);   // somebody else
    ImVec4 system     = ImVec4(0.55f, 0.60f, 0.65f, 1.00f);
    ImVec4 warn       = ImVec4(1.00f, 0.55f, 0.35f, 1.00f);
    ImVec4 rule       = ImVec4(1.00f, 1.00f, 1.00f, 0.16f);
};
const ThemePalette& Theme();

// Push the whole look (colours, zero rounding, spacing, and the game-matched font if one is loaded)
// and pop exactly what was pushed. ALWAYS pair them -- ImGui's stacks are unforgiving.
// `solid` picks the opaque panel background, for a surface that owns the screen rather than floating
// over play.
void Theme_Push(bool solid = false);
void Theme_Pop();


// ---- the font handover ---------------------------------------------------------------------------
// GAME THREAD -> render thread. Takes ownership of `data` (freed with free()). The render thread picks
// it up on its next frame: adding a font means rebuilding the atlas, which may only happen BETWEEN
// frames, and the game thread may not touch ImGui at all.
void Theme_OfferFont(void* data, int size, const char* debugName);
// RENDER THREAD, OUTSIDE a frame. True when a rebuild happened -- the caller must then invalidate its
// backend's device objects so the new atlas texture is uploaded.
bool Theme_ConsumePendingFont();
