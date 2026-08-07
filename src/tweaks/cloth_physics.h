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
// SessionTweaks -- cloth wind. Two halves:
//  * CLOTH WIND BOOST: wind-capable garments (the MAT_ClothMaster shirts with a WindMask texture)
//    flap by the 'PlayerSpeed' scalar of the MPC_ClothWind parameter collection. A hook on the
//    game's own SetScalarParameterValue scales that write and floors it, so those clothes flap
//    harder with speed and keep a breeze at a standstill. Garments on the procedural masters
//    (MAT_Cloth_UB_Master etc.) have no wind in their shader and cannot be given one.
//  * RECON DUMP (F1 button): read-only dump of the merged mesh + props (cloth sim data, physics
//    bodies), every material chain with parameters at each level, and the parameter-collection
//    census -- the instrument that decoded the above; kept for future outfits and game updates.
#pragma once
struct OmpMenuApi;
void ClothPhys_ReadConfig(const char* iniText);
void ClothPhys_SaveConfig(char* iniText, size_t cap);
void ClothPhys_Install();
void ClothPhys_DrawMenu(const OmpMenuApi* api);   // RENDER THREAD (menu_ext contract)
void ClothPhys_PumpFrame();                       // GAME THREAD: runs a requested dump
void ClothPhys_ResetDefaults();
