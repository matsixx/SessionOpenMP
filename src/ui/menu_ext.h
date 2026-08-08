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
// SessionOpenMP -- the F1 menu EXTENSION seam: how a SEPARATE mod DLL puts a section in the F1 menu.
//
// WHY THIS EXISTS: gameplay mods (SessionTweaks is the first) are deliberately separate UE4SS
// mods -- separate DLLs, separately enable-able in mods.txt -- but one game wants ONE menu, and
// ImGui cannot be called across a DLL boundary (each DLL would have its own ImGui instance and
// context). So the host exposes a tiny C ABI instead: a struct of widget function pointers, valid
// ONLY inside the draw callback, which the host invokes between its own sections while the F1
// window is open. The guest never links ImGui at all.
//
// THREADING: the callback runs on the RENDER thread (the host's Present hook), exactly like the
// host's own buildUI. A guest must treat its knobs as plain aligned scalars shared with its game-
// thread hooks -- the same volatile-scalar discipline as overlay.h's snapshot/action boundary.
// Registration itself may happen from any thread; the registry is mutex-guarded.
//
// DISCOVERY (guest side): both the host and every guest are named main.dll (the UE4SS layout is
// Mods\<Name>\dlls\main.dll), so GetModuleHandle by name is ambiguous. Enumerate loaded modules
// and GetProcAddress each for "OmpMenu_Register"; retry for a while -- mods.txt order decides who
// loads first, and the seam must work either way.
//
// COMPATIBILITY: C ABI, no C++ types cross the boundary. `version` only ever grows; new function
// pointers are appended at the END of the struct, never inserted. A guest checks `version >= N`
// for the fields it needs.
#pragma once

struct OmpMenuApi {
    int  version;                                              // 2 (see Button, appended below)
    void (*Text)(const char* utf8);
    void (*TextDisabled)(const char* utf8);
    void (*TextWrapped)(const char* utf8);
    void (*Separator)(void);
    void (*SameLine)(void);
    bool (*Checkbox)(const char* label, bool* v);              // true = value changed this frame
    bool (*SliderFloat)(const char* label, float* v, float lo, float hi, const char* fmt);
    void (*Indent)(void);
    void (*Unindent)(void);
    // ---- appended in version 2. Check `version >= 2` before calling. Returns true on the frame the
    // button is clicked -- for one-shot rows such as "Reset to defaults".
    bool (*Button)(const char* label);
};
typedef void (*OmpMenuDrawFn)(const OmpMenuApi* api, void* user);

// Exported by the host's main.dll. Returns 1 on success, 0 if the table is full or args are null.
// `title` becomes a collapsing header in the F1 window; the callback draws the section body under
// it. Registration is permanent for the process (C++ mods only unload at shutdown).
//   extern "C" __declspec(dllimport) int OmpMenu_Register(const char* title, OmpMenuDrawFn draw, void* user);

// =====================================================================================================
// THE PAUSE-MENU SEAM -- a page in the GAME'S OWN pause menu, not in the F1 window.
//
// Deliberately a SEPARATE export and a separate shape rather than a change to OmpMenuApi, so guest
// binaries built against the ImGui seam alone keep working unchanged. The two seams are different in
// kind -- this one is DECLARATIVE (you hand over a list of rows and a callback) because the host is
// filling in a native UMG menu, not running an immediate-mode draw loop.
//
// THE THREADING CONTRACT IS THE OPPOSITE OF THE ImGui SEAM ABOVE. `OmpPageSelectFn` and
// `OmpPageStatusFn` are called on the GAME THREAD, from inside the engine's own menu code. They may
// touch game state directly; they must NOT touch ImGui or anything else the render thread owns. A
// guest that already has render-thread draw callbacks now has both kinds -- do not mix them up.
//
// The host COPIES every string at registration, so the guest's arrays need not outlive the call; the
// `key` handed back to the callback is the host's own copy. Callbacks run inside SEH: a page that
// faults is marked dead and never shown again for the rest of the run (it can never take the game
// down with it). Discovery is the same rule as above -- probe every loaded module for the export.
// =====================================================================================================
// The host's per-page row cap. It sizes fixed arrays here and in pause_menu.cpp, and the host clamps
// a guest's count to it, so raising it is host-side only: a guest DLL built against a smaller value
// still registers correctly (it simply never offers more rows than it knew about).
// This is OUR limit, not the game's -- the page's own `_maxVisibleItems` is raised for the duration
// of the row build so every row gets created.
//
// Measured: `PauseMenuPage` reports items=8 maxVisible=14, so 14 rows is what the panel is actually
// laid out for. Beyond that the rows still build (the cap is raised) but they crowd the screen,
// because forcing `_maxVisibleItems` up is also exactly what suppresses the native scrollbar --
// the scroll math triggers on `_pageItemDefinitions.Num > _maxVisibleItems`. A page wanting more
// A page longer than the engine's ~14-row window does NOT scroll: UMenuPage's scroll math reads
// `_pageItemDefinitions`, which still describes the real page underneath, not the array we hand the
// builder -- so it concludes everything fits. Publishing our count there means owning an array the
// engine element-wise destructs (FText refcounts included) through its own allocator. Until that is
// worth doing, SPLIT A LONG PAGE with OMP_ITEM_PAGE rows rather than letting it run off the screen.
enum { OMP_PAGEITEM_MAX = 48 };

struct OmpPageItem {
    const char* key;      // stable ASCII id, unique within the page; becomes the row's FName
    const char* label;    // what the row says
    const char* desc;     // the footer description line, or null
};

// The user confirmed a row. GAME THREAD, inside SEH.
typedef void (*OmpPageSelectFn)(const char* key, void* user);
// Optional. Asked for a row's current footer text while the page is being built. GAME THREAD, inside
// SEH. Return null to leave the row's registered description alone. Must be cheap and must not
// allocate -- the host copies the bytes immediately.
typedef const char* (*OmpPageStatusFn)(const char* key, void* user);

// Exported by the host's main.dll. Returns 1 on success, 0 if the page table is full or args are null.
// `title` becomes a row in the pause menu, and the heading of the sub-page it opens. The host adds a
// "Back" row for you.
//   extern "C" __declspec(dllimport) int OmpMenu_RegisterPage(
//       const char* title, const OmpPageItem* items, int nItems,
//       OmpPageSelectFn onSelect, OmpPageStatusFn onStatus, void* user);

// ---- NATIVE CONTROLS (seam v2) ----------------------------------------------------------------------
// The rows above are all "press to do something". Session's own settings pages use two richer row
// types, and a mod page sitting next to them looks wrong without them -- so a guest can now ask for:
//   * a TOGGLE  -- the game's MultiOption row, with < Off / On > arrows, exactly like Dark Slides.
//   * a SLIDER  -- the game's ProgressBar row with a min/max/step, exactly like Master Volume.
// Registered through a SECOND export rather than by growing `OmpPageItem`, so a guest built against
// v1 keeps working byte-for-byte. v1 registrations are simply all-action pages.
//   * a PAGE    -- a row that opens ANOTHER page this guest registered, so a long list can be split
//                  into categories instead of growing past what fits on screen. Set `key` to the
//                  exact title of the page to open; that page is then reachable only through this
//                  row and stops appearing in the pause menu itself. The host gives it a Back row
//                  that returns HERE, not to the pause menu.
enum { OMP_ITEM_ACTION = 0, OMP_ITEM_TOGGLE = 1, OMP_ITEM_SLIDER = 2, OMP_ITEM_PAGE = 3 };

struct OmpPageItem2 {
    int         kind;         // OMP_ITEM_*
    const char* key;          // stable ASCII id, unique within the page
    const char* label;
    const char* desc;         // footer description, or null
    // TOGGLE only -- the two option labels. Null/empty gives "Off" and "On".
    const char* offLabel;
    const char* onLabel;
    // SLIDER only -- the DISPLAYED range and the step. The host converts to and from the engine's
    // normalised 0..1 bar for you, so these are in your own units.
    // The game prints a slider's value with "%d". CHOOSE UNITS WHOSE INTEGERS MEAN SOMETHING:
    // centimetres, degrees, percent -- not "0.5 to 6.0 metres", which can only ever read 0..6.
    float       minValue, maxValue, step;
};

// The user changed a TOGGLE or a SLIDER. GAME THREAD, inside SEH. For a toggle `intValue` is the
// option index (0 = off); for a slider `floatValue` is in the item's own units.
typedef void (*OmpPageValueFn)(const char* key, int intValue, float floatValue, void* user);
// Asked for a row's CURRENT value while the page is being built, so the row opens showing the truth
// rather than a default. GAME THREAD, inside SEH. Return 0 to leave the row at its registered value.
typedef int  (*OmpPageGetFn)(const char* key, int* outInt, float* outFloat, void* user);

// Exported by the host's main.dll. `onSelect` still handles OMP_ITEM_ACTION rows and may be null if
// the page has none; `onValue`/`onGet` may be null if it has no toggles or sliders.
//   extern "C" __declspec(dllimport) int OmpMenu_RegisterPage2(
//       const char* title, const OmpPageItem2* items, int nItems, OmpPageSelectFn onSelect,
//       OmpPageValueFn onValue, OmpPageGetFn onGet, OmpPageStatusFn onStatus, void* user);
