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
//
// CUSTOM MAPS IN THE GAME'S OWN SELECT MAP SCREEN.
//
// Custom maps are installed (by the community's mod manager) as loose cooked assets under
// SessionGame/Content/CustomMaps/<author>/<map>/..., and the game itself never looks there. The
// pause menu's "Select Map" is the TRANSIT MAP (UTransitMapWidget): its cities and their spots come
// from one UTransitDataAsset -- a city list (prefix, texts, the blueprint that draws that city's
// map) and a node list (city, label, level name, portal, ...). The D-pad cycles cities, the node
// list is rebuilt per city from the asset, and confirming a node runs ATransitManager::TeleportPlayer
// -> USessionGameInstance::LoadLevel -> UGameplayStatics::OpenLevel. All data-driven, so:
//
//   * at start-up, every *.umap under Content/CustomMaps is found, one entry per level, its package
//     path derived from where it sits ("/Game/CustomMaps/<author>/<map>/<Map>"), its label from the
//     file name or the mod manager's CustomName, hidden ones honoured;
//   * when the transit map is about to open, one "Custom Maps" city and one node per level are
//     appended to that asset (idempotent: the city's prefix is looked for first). The city borrows
//     the "Extra Network" city's map blueprint, the nodes borrow an Extra node's placeholder image.
//
// The game then draws, pages, selects and travels on its own: OpenLevel with a full package path
// loads the loose level directly. No files are copied, no console command is run. A level name
// that is a full path never equals the running world's short name, so the map does not open on the
// custom city while standing in one and the current custom map stays selectable (a reload) -- both
// accepted for the first cut.
#pragma once

namespace omp::game::maps {
void Install(void (*logf)(const char*));   // start-up: scan the folder, hook the transit map opening
int  Count();                              // how many custom levels were found
} // namespace omp::game::maps
