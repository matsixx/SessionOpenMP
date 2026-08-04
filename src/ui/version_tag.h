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
// SessionOpenMP -- the mod version, beside the game's own, in the corner of the screen.
//
// The game prints its build in the bottom-left ("0.6.42 (48691)") from
// `USessionGameInstance::GetGameVersion`; the hook appends " | OpenMP <ver>".
//
// THE WHOLE POINT OF THIS FILE IS THE FILTER. GetGameVersion has three real callers and only two of
// them draw anything: `ASessionPlayerController::CreateIntroUI` and
// `UPauseMenuPageContainer::NativeOnInitialized`. The third is `UPlayerProfile::GetNewsSaveData` --
// PERSISTENT SAVE DATA. Appending to that one would write the tag into the player's profile and
// corrupt the version the news system compares against. So the tag is applied by RETURN ADDRESS:
// hook the funnel, act only for the callers you actually mean.
#pragma once

// The one place the mod's version number is written. It reaches the intro screen, the pause menu and
// UE4SS's mod list from here -- there is no second copy to keep in step.
#define OMP_VERSION_STRING "0.7.1b"
// ...and the same thing wide, for the engine's TCHAR strings. Two levels because a single-level
// `L##x` macro pastes the macro NAME instead of expanding it (UE4SS's own STR() has exactly that
// shape, which is why it cannot be handed OMP_VERSION_STRING directly).
#define OMP_VER_WIDEN2(x) L##x
#define OMP_VER_WIDEN(x)  OMP_VER_WIDEN2(x)
#define OMP_VERSION_WIDE  OMP_VER_WIDEN(OMP_VERSION_STRING)

void VersionTag_Install();   // game thread, from on_unreal_init (after MinHook is up)
