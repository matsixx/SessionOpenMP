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
// SessionTweaks -- CLOTH, phase A: the outfit un-merge. The skater's outfit is one runtime
// FSkeletalMeshMerge of body + garments, and merge output carries no cloth-sim data -- so a garment
// that is ever going to simulate cloth must be excluded from the merge and ride the body as its own
// master-posed component. Round 1 is the merge recon: hook FSkeletalMeshMerge::DoMerge and log every
// merge's caller and source meshes, so the exclusion filter and the re-dress cadence are designed
// from measured names rather than guesses.
#pragma once
// How many garments can be simulated at once, and -- because a garment is assigned the slot of the
// ClothGarmentTags entry it matched -- how many tags can be defined. Both files index by slot, so
// this must be ONE number: cloth_sim iterates ClothMerge_SlaveCount() while indexing its own
// per-slot arrays, and when those two disagreed it walked off the end of them. It lives here so
// neither side can raise it alone; cloth_sim static_asserts against it.
//
// It was 4, which was also the tag limit, so a fifth tag was silently discarded -- the whole reason
// a custom garment could be tagged and still not simulate.
enum { kClothMaxGarments = 12 };
struct OmpMenuApi;
void ClothMerge_ReadConfig(const char* iniText);
void ClothMerge_SaveConfig(char* iniText, size_t cap);
void ClothMerge_ResetDefaults();
void ClothMerge_Install();
void ClothMerge_PumpFrame();                     // GAME THREAD
// The outfit the wardrobe last offered us, for the menu's include/exclude rows. Names only; whether
// one counts as included is derived from the live tag/exclusion lists, never stored, so a row cannot
// drift from the rule that decides. Setting one edits ClothGarmentTags / ClothExclude and marks the
// settings dirty, exactly as typing them by hand would.
int         ClothMerge_WornCount();
const char* ClothMerge_WornName(int i);
bool        ClothMerge_WornIsBody(int i);      // the body mesh: listed, never selectable
bool        ClothMerge_WornIncluded(int i);
bool        ClothMerge_SetWornIncluded(int i, bool on);   // false = refused, see the note below
// Why the last include/exclude was refused, or null. Tags and exclusions are CAPPED lists, and a
// tick that springs back without saying why reads as broken rather than as full.
const char* ClothMerge_WornNote();
// Clear the tag and exclusion lists back to the stock patterns. The ONLY thing that does -- they
// survive Reset Defaults on purpose -- and it logs both lists first so it can be undone by hand.
void        ClothMerge_ForgetClothingChoices();
int         ClothMerge_TagsUsed();  int ClothMerge_TagsMax();
int         ClothMerge_ExclUsed();  int ClothMerge_ExclMax();
void ClothMerge_DrawMenu(const OmpMenuApi* api); // RENDER THREAD (menu_ext contract)
// Shared with cloth_sim (phase B), so the object-construction symbols have ONE owner and one set of
// signatures. Null until the un-merge has built a garment component.
void* ClothMerge_SlaveComponent(int slot);           // our un-merged garment component, or null
int   ClothMerge_SlaveCount();
// The body component the garments are posed from. A master-posed component carries no bone
// transforms of its own -- anything needing the live skeleton has to read them from here.
void* ClothMerge_MasterComponent();
// The material the customization system resolved for a garment mesh on the last dress, and which of
// the mesh's material slots it dresses. Null if the customization listener is not running.
void* ClothMerge_ConfiguredMaterial(void* mesh, int* outIdx);
// Which of a garment mesh's material slots is the garment itself (-1 if unknown).
int ClothMerge_GarmentMaterialIndex(void* mesh);
// True when this garment's own material cannot draw cloth, so it must be driven rather than bound.
bool ClothMerge_GarmentWantsDirect(void* mesh);
// The engine call that attaches/detaches a component from another's skeleton, already resolved here.
void* ClothMerge_SetMasterPoseFn();                       // how many garment slots exist (tops, bottoms, ...)
void* ClothMerge_FindClass(const wchar_t* name);     // UClass* by name, type-checked
void* ClothMerge_NewObject(void* cls, void* outer);  // StaticConstructObject_Internal, RF_Transient
// Ticks every time the slave's garment actually changes. Phase B watches this instead of polling
// the mesh pointer -- merged meshes are POOLED, so pointer identity cannot detect a re-dress.
long  ClothMerge_GarmentSerial();
