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
// SessionTweaks -- CLOTH, phase B: bring the shipped NvCloth solver to life. Session ships the
// ENTIRE solver and the clothed-mesh render path but zero cloth DATA and no authoring tools, so
// the job is to build a UClothingAssetCommon at runtime. This module's first milestone is
// deliberately the smallest one that cannot lie: a hand-built quad (4 verts, 2 triangles) attached
// to the un-merged garment's mesh, with the solver's own SimulationTime read back as proof it is
// ticking. No render surgery yet -- rendering cloth needs the mesh's GPU buffers rebuilt, which is
// the one genuinely crash-capable step, and it is not worth risking before the solver is proven.
#pragma once
struct OmpMenuApi;
void ClothSim_ReadConfig(const char* iniText);
void ClothSim_SaveConfig(char* iniText, size_t cap);
void ClothSim_ResetDefaults();
void ClothSim_Install();
void ClothSim_PumpFrame();
// A garment's component was destroyed under us: drop every cached pointer for that slot so the next
// dress rebuilds instead of driving a corpse.
void ClothSim_SlaveGone(int slot);
// Undo every mark we made on the garment meshes. Required before a marked garment can be merged
// again -- see the note on the definition.
void ClothSim_ReleaseAll();
// Read-only audit of the mod-built garment copy against the asset it was copied from: vertex
// positions, skin weights and section bone maps. Everything downstream assumes these are the same
// mesh; nothing verified it. A difference indicts the un-merge, identical output clears it.
void ClothSim_CompareCopyToSource(void* copy, void* src, const char* name);
// 1 when the garment draws itself from the simulation, so no cloth-capable material is needed and
// the game's own configured material can be used as-is.
int  ClothSim_DirectRender();
int  ClothSim_DirectEnabled();
// 1 only while the engine's cloth renderer is in use -- the one case where a garment's material
// must be cloth-capable. Everything else draws ordinarily and has no such requirement.
int  ClothSim_NeedsClothMaterial();                     // GAME THREAD
void ClothSim_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)
// Forget what we built; the next frame rebuilds it. How a shape knob takes effect without a reload.
void ClothSim_Rebuild();
// ---- the pause-menu surface. Every one of these takes effect immediately.
bool  ClothSim_Enabled();          void ClothSim_SetEnabled(bool on);
float ClothSim_TravelCm();         void ClothSim_SetTravelCm(float v);
float ClothSim_HemPushMm();        void ClothSim_SetHemPushMm(float v);
// How far up the garment the lift reaches, as a percentage of its height. The push is strongest at
// the bottom edge and fades to nothing at the top of this band, so raising it flares more of the
// garment rather than just its rim.
float ClothSim_HemPushBandPct();   void ClothSim_SetHemPushBandPct(float v);
float ClothSim_CuffGripPct();      void ClothSim_SetCuffGripPct(float v);
