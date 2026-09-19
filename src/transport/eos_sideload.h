// SessionOpenMP -- OUR EOS SDK, LOADED BESIDE THE GAME'S.
//
// Session has an EOS integration of its own and ships EOSSDK-Win64-Shipping.dll 1.13 for it. This mod is
// built against 1.17. It used to be installed by OVERWRITING the game's file, which made one SDK of the two
// -- the game on an SDK it was not built for -- and on the Epic client players' DLC stopped working.
//
// So: the game's file is never touched. Ours ships under ANOTHER NAME, next to main.dll
//     <Win64>\Mods\SessionOpenMP\dlls\OMP_EOSSDK-Win64-Shipping.dll
// and main.dll no longer imports EOS at load time: its EOS imports are DELAY-LOADED, and the loader's hook
// (eos_sideload.cpp) answers "which module?" with that file, by full path. Every EOS_* call in the mod is
// unchanged; each is simply bound, on first use, to our copy. Two SDKs, two states: tools/eosdual proves they
// coexist (each initialises with EOS_Success, each makes a platform, and the name
// EOSSDK-Win64-Shipping.dll still means the game's module -- which is WHY ours is renamed: any by-name
// lookup the game makes must never find ours).
#pragma once
#include <cstddef>

namespace omp { namespace eosb {

// Load our SDK (idempotent). false = it is not to be had, and NO EOS_* function may be called: the EOS backend
// must refuse instead (a delay-loaded call with no module behind it raises an exception). `why` says what
// is wrong, in words for the log and the player.
bool SideloadReady(char* why, size_t cap);
// The full path it was (or would be) loaded from, for the log.
const char* SideloadPath();
// The version the GAME'S OWN SDK reports, if the game has one loaded ("" if not). Ours in its slot -- an older
// install of this mod overwrote it -- is the broken-DLC state: true, and the player is told to verify files.
bool GameSdkWasReplaced(char* gameVersion, size_t cap);

}} // namespace omp::eosb
