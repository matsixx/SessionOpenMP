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
// SessionOpenMP -- a page of our text inside ONE OF THE GAME'S OWN PANELS.
//
// The widget is `PBP_MyNacon_ErrorMessaging` -- the message panel from the MyNacon family, the same
// framing as the offer shown at start-up. It was chosen over the offer panel itself
// (PBP_MyNacon_RegisterOffer) for three reasons, all read out of the extracted assets:
//   * it is a GENERIC MESSAGE panel -- a text block and a continue button -- where the offer is a
//     promo built of PromoItem rows and a register button wired to Nacon account logic we do not want
//     to run;
//   * its `_textBlock` has AutoWrapText and sits in a ScaleBox, so a page of text WRAPS and SCALES
//     instead of marching off the bottom of the screen, which is exactly what the plain dialog did;
//   * it is small -- no widget animation, no sub-blueprint rows -- so constructing it does the least.
//
// GAME THREAD ONLY, and everything is inside SEH: a panel nobody sees is never worth the game for.
// =====================================================================================================
#pragma once

namespace omp { namespace ui {

// Is the panel reachable -- symbols resolved and the class loadable?
bool NaconPanel_Available();

// Can it take text longer than an FName holds? True when FText::FromString resolved, which removes
// the 1024-character NAME_SIZE ceiling; false means the caller must keep its page under it or the
// widget renders ERROR_NAME_SIZE_EXCEEDED and nothing else.
bool NaconPanel_LongTextOk();

// Put `text` on screen in the game's message panel. `buttonText` labels the continue prompt (the
// panel draws the A-button glyph beside it). One panel at a time: showing another replaces it.
bool NaconPanel_Show(const char* text, const char* buttonText, void (*logf)(const char*) = nullptr);

// Is ours on screen right now?
bool NaconPanel_Showing();

// Take it down. Safe to call when nothing is up.
void NaconPanel_Close();

} }  // namespace omp::ui
