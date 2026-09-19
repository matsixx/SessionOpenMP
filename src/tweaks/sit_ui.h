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
// SessionTweaks -- the on-screen prompt for sitting, built from the game's own button-prompt widget
// so it matches the rest of the HUD. See sit_ui.cpp; driven entirely by sit.cpp.
#pragma once
#include <stddef.h>
void SitUI_Install();                          // sig-scan + the class-discovery hook; non-fatal
void SitUI_ReadConfig(const char* iniText);
void SitUI_SaveConfig(char* iniText, size_t cap);
// One line of the prompt bar: what a button does. `button` is the Xbox name ('A' 'B' 'X' 'Y'); the
// PlayStation equivalent is drawn on a DualSense. `ring` (0..1) fills the button's progress ring --
// the hold-to-confirm feedback the game's own prompts use.
struct SitPromptEntry { const char* label; char button; float ring; };
// GAME THREAD, every frame while the feature is in play: `show` puts the bar on screen with these
// entries (entry 0 is the prompt's own button, the rest are rows along the bar to its left). `skater`
// is the pawn the widgets belong to -- a different one means a new level, and the old widgets are
// dropped rather than touched. Up to six entries.
void SitUI_PumpFrame(void* skater, bool show, const SitPromptEntry* entries, int count);
const char* SitUI_Status();
// GAME THREAD: collapse every row NOW, without waiting for the next pump. The pump rides
// InputHandler::Tick, which the replay and prop editors stop -- so the bar has to be hidden from
// whatever is still ticking, or it stays on screen for as long as the editor is open.
void SitUI_HideNow();

// ---- FREE LABELS (the radial menu). The same button widget with its glyph collapsed -- an entry on a
// wheel is a word, not a button to press -- placed wherever the caller says: x, y are where the label's
// CENTRE should be, in Slate units from the viewport's top-left. `lit` is the selected look; the rest
// are dimmed. Pool rules are the bar's: built once per skater, hidden rather than destroyed. Up to 12.
struct SitFreeLabel { const char* label; float x, y; bool lit; };
void SitUI_PumpFree(void* skater, bool show, const SitFreeLabel* labels, int count);
void SitUI_HideFreeNow();                       // for an owner whose pump has stopped; verified before any touch
bool SitUI_Viewport(void* skater, float* w, float* h);      // Slate units; false when it cannot be measured
// How wide one character of a label draws (Slate units) and how faint an unlit label is (0..1). The
// widget reports neither, so the caller's layout owns the numbers.
void SitUI_SetFreeMetrics(float charW, float dim);
// An instance whose class is, or derives from, the NATIVE class the engine names this way (no A/U
// prefix: "SkateShop"). Default objects and archetypes are never returned. Null when there is none.
// ONE-OFF USE: it walks the whole object array, which is a few milliseconds.
// `exclude` is skipped, so a caller that made one of its own can still ask for the level's.
void* SitUI_FindInstanceOf(const char* nativeClassName, void* seed, void* exclude);
// An object by its full path ("/Game/Dir/Asset.Asset_C"): found if it is loaded, else loaded now --
// which blocks, so it belongs on a button press and never in a frame.
void* SitUI_LoadObject(const char* path);

// A game object kept across frames, remembered the way the engine's own weak pointers do -- by its slot
// in the global object table -- so it can be asked "are you still the object I was given?" before it is
// touched. A pointer alone cannot answer that: a level change frees the object and hands its memory to
// something else, and a write through the old pointer then lands in the new owner. SitUI_Alive is false
// for a null reference, for an object that is gone, replaced or on its way out, and on any fault.
struct SitObjRef { void* obj; int index; int serial; void* cls; };
void SitUI_Track(SitObjRef* ref, void* obj);
bool SitUI_Alive(const SitObjRef* ref);
void* SitUI_ResolveWeak(int objectIndex, int serialNumber);   // an engine FWeakObjectPtr: the object, or null
bool SitUI_CanVerify();                         // false: no object table in this build, nothing can be checked
// Diagnostic: log the names of the loaded objects of exactly this native class, up to `cap`.
int   SitUI_LogInstancesOf(const char* nativeClassName, void* seed, int cap, const char* tag);
