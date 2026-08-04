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
// SessionOpenMP -- each player's multiplayer name, floating above their head.
//
// WHY IMGUI AND NOT A WORLD WIDGET: a UMG billboard needs a widget blueprint and a font asset we do
// not have (the same wall the join-code prompt and the name box hit), and a UTextRenderComponent needs
// a material. Drawing from the Present hook needs no engine cooperation at all -- and the mod already
// owns that surface.
//
// ONLY PEERS EVER GET A PLATE. There is no "hide my own" filter to get wrong: the list is built from
// the peer roster, and the local player is not in it.
//
// THE THREAD RULE IS overlay.h's:
//   * the GAME thread owns everything Unreal -- the roster, each proxy's head position, and the
//     world-to-screen projection -- and publishes a plain-data snapshot (Nameplates_Publish)
//   * the RENDER thread only draws it, and owns the fade
// Positions cross the boundary NORMALISED (0..1 across the game's viewport) rather than in pixels: the
// game's render resolution and the window's client size are different numbers whenever a resolution
// scale is set, and only the render thread knows the second one.
#pragma once
#include <stdint.h>

struct NameplateTuning {
    bool  enabled      = true;
    // Sit the plate this far above the top of the skater's capsule (the capsule's own half-height is
    // measured, so this is pure headroom, not a guess at how tall anybody is).
    float headroomCm   = 28.0f;
    // The on/off-board fade. Off the board you are looking around and want to know who is who; on it
    // you want a clean screen. Long enough to read as a fade, short enough not to lag the transition.
    float fadeSec      = 0.30f;
    float maxDistCm    = 12000.0f;  // past this a plate is not drawn at all (120 m)
    float fadeDistFrac = 0.75f;     // ...and it fades out over the last quarter of that range
    float refDistCm    = 900.0f;    // the distance at which a plate is drawn at its natural size
    float minScale     = 0.60f;     // clamps, so a distant name stays legible and a close one sane
    float maxScale     = 1.30f;
    float outlineAlpha = 0.75f;     // the dark rim that keeps a name readable against a bright wall

    // ---- SPEECH BUBBLES. Deliberately NOT tied to the on/off-board fade: somebody talking to you is
    // worth seeing while you are skating, which a name is not.
    bool  bubbles      = true;
    float bubbleHoldSec = 7.0f;     // fully opaque for this long...
    float bubbleFadeSec = 1.5f;     // ...then out over this
    // Bubbles are close-range on purpose: a legible sentence needs far more screen than a name, and
    // reading one across a whole park is neither possible nor wanted.
    float bubbleMaxDistCm  = 3500.0f;
    float bubbleFadeDistFrac = 0.80f;
    float bubbleWidthEm    = 15.0f; // wrap width as a multiple of the drawn text height, so the box
                                    // keeps its shape at every distance
};
NameplateTuning& Nameplates_Tuning();

// ---- game thread ---------------------------------------------------------------------------------
// One peer with a name and a screen position. `x`/`y` are 0..1 across the game's viewport, y DOWN.
// `msg` is their most recent chat line, empty when there is none live; `msgAgeMs` is how long ago they
// said it, which is what the bubble's own fade runs on. The whole line is republished every frame
// rather than sent as an event: the render thread then holds no state that could get out of step with
// who is actually here.
struct NameplateItem {
    char     name[32];
    float    x, y;
    float    distCm;
    char     msg[160];
    uint32_t msgAgeMs;
};
// Publish this frame's plates. `show` is the fade TARGET -- true = the local player is off their
// board. Publishing an empty list is normal and meaningful (no session, no peers, no view): it is what
// makes the plates disappear rather than freeze on screen.
void Nameplates_Publish(const NameplateItem* items, int n, bool show);

// ---- render thread -------------------------------------------------------------------------------
void Nameplates_Draw();
// Is there anything to put on screen? The Present hook only pays for an ImGui frame when a surface
// actually wants to draw, so this has to stay true through the whole fade-out -- the fade lives on the
// render thread and cannot advance in frames that are never drawn.
bool Nameplates_HasVisible();
