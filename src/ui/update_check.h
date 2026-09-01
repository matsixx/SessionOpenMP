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
// SessionOpenMP -- "is there a newer release?", asked once at startup off the game thread.
// See update_check.cpp for why it is one shot, detached, and silent on every failure.
#pragma once

namespace omp { namespace ui {

// Fire the one request. Safe to call repeatedly -- only the first ever does anything. Returns at
// once; the answer arrives later, or never.
void UpdateCheck_Start();

// Has an answer arrived AND is it "you are behind"? Any thread. False while asking, false when
// current, and false when the question could not be answered at all -- a check that cannot reach
// GitHub must look exactly like a check that found nothing.
bool UpdateCheck_NewerAvailable(char* latestOut, int cap);

// Exposed for the gate. -1 = a is older, 0 = same, 1 = a is newer. Dotted numbers first, then a
// pre-release suffix: "1.0.0-rc4" is OLDER than "1.0.0", which is the case that actually matters --
// somebody on a release candidate has to be told when the release lands. Anything unparseable
// compares EQUAL, because equal is silence and silence is the safe answer.
int UpdateCheck_CompareVersions(const char* a, const char* b);

} }  // namespace omp::ui
