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
// SessionOpenMP -- borrow the game's own typeface for the chat box.
//
// A UE4 `UFontFace` does not merely reference a font file, it can CARRY it -- `UFontFace::FontFaceData`
// is a `TSharedRef<FFontFaceData>` whose single member is a `TArray<uint8>` holding the raw TTF/OTF
// bytes, present in memory whenever the asset's `LoadingPolicy` is Inline. Those bytes are exactly what
// ImGui's AddFontFromMemoryTTF wants, so the chat can be drawn in the game's real typeface rather than
// something that merely resembles it. Offsets are PDB lookups (UFontFace +0x48, FFontFaceData +0x00).
//
// GAME THREAD ONLY (it walks GUObjectArray and reads game memory). It hands the bytes to
// Chat_OfferFont, and the RENDER thread rebuilds the atlas on its own next frame.
#pragma once

// Runs the hunt once. Safe to call repeatedly -- it no-ops after it has handed a font over, and
// returns false until the fonts are actually loaded (early in startup there are none yet).
// `logf` receives one line per candidate the first time it looks, so the log names every face the
// build has and which one was taken.
bool GameFont_Grab(void (*logf)(const char*));
