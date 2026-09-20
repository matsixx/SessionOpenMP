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
// SessionTweaks -- THE BUTTON PROMPTS: a short list in the lower right saying what each button does,
// drawn with the GAME's own button widget. Sitting is the first user; the list is the API.
//
// THE WIDGET IS THE GAME'S. UUIGamePadButton is one prompt -- a controller glyph and a label in a
// single widget -- and is what the object-dropper HUD is built from. One is created per entry; the
// mod sets the text, the key and the progress ring, and nothing else about how it looks.
//
// THE GLYPH IS THE GAME'S TOO, and that is the point of using its widget. The game answers exactly
// this question for its own prompts: UTRXUtilities::RetrieveCurrentControllerType says what is in your
// hands, and UTRXPluginSettings::FindControllerKeyIconForControllerType turns a key plus that type
// into the icon -- so a DualSense gets Circle where an Xbox pad gets B, from the game's own table. The
// answer is put in the button's brush and refreshed periodically, so swapping a pad mid-session is
// picked up. Where any of that is unavailable there is a fallback that edits the icon name in place,
// keeping whatever family the brush is already in.
//
// (Running the button blueprint's own construct script would be the other way, but that needs
// UObject::ProcessEvent, whose prologue is detoured by UE4SS itself -- a signature for it cannot match
// in memory. Do not reach for ProcessEvent here.)
//
// ITS CLASS IS FOUND, NOT GUESSED. The look lives in a blueprint asset, so the blueprint class is what
// must be instantiated. It is found by name in the engine's object array (class objects are picked out
// by two pointer reads -- a class's class's class is UClass -- and only those get named), and its asset
// path is written to the ini so later launches load it outright.
//
// TEXT is built the pause menu's way -- FName through FText::FromName -- never FromString, whose
// const-ref and rvalue twins no byte signature can tell apart (the wrong one steals or double-frees a
// buffer we own). FromName is byte-identical to its siblings too, so it is read out of the one call
// site in UTrickDisplayWidget::SetTrickDisplayText that lands on it, checked against its prologue.
//
// NOTHING HERE IS LOAD-BEARING. Every symbol is optional and every call is guarded: a missing
// signature, a class that never turns up or a fault in the engine's widget code costs the prompts and
// nothing else -- the feature behind them never depends on it.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tweaks_common.h"
#include "sit_ui.h"
#include "grind_pop.h"     // GrindPop_FNameToString -- names the discovered class
#include "MinHook.h"

// ------------------------------------------------------------------ knobs
static int   g_on      = 1;         // SitPromptEnabled
static int   g_zOrder  = 500;       // SitPromptZ: above the game's own HUD layers
static int   g_holdRing = 1;        // SitPromptHoldRing: fill a button's progress ring while its key is held
static char  g_rowPath[160] = "";   // SitPromptRowPath: the button's asset path, written when first seen
static char  g_rowMatch[64] = "UIGamePadButton";   // SitPromptRowMatch: what its class name contains
// Placement, in Slate units (pixels over the DPI scale) from the bottom-right corner. Fresh names: an
// ini written by an older build must not pin values a later default changed.
static float g_barRight  = 72.0f;   // SitBarRight: right edge to the glyph
static float g_barBottom = 64.0f;   // SitBarBottom: bottom edge to the lowest entry
static float g_barGap    = 56.0f;   // SitBarGap: between entries
static float g_barW      = 56.0f;   // SitBarW / SitBarH: the slot an entry is given (its glyph's size;
static float g_barH      = 48.0f;   //   the label hangs to the LEFT of it, outside the slot)

// ------------------------------------------------------------------ layouts (PDB, both builds)
enum {
    UOBJ_CLASS   = 0x10,    // UObjectBase::ClassPrivate
    UOBJ_NAME    = 0x18,    // UObjectBase::NamePrivate
    UOBJ_OUTER   = 0x20,    // UObjectBase::OuterPrivate (a class's outer is its package: the asset path)
    UW_VIS       = 0xc3,    // UWidget::Visibility
    UUW_FULLSCREEN = 0x238, // UUserWidget::FullScreenWidget (the Slate object, once on screen)
    PAWN_CTRL    = 0x258,   // APawn::Controller
    BP_KEY       = 0x348,   // PBP_UIGamePadButton_C::ButtonKey (FKey: FName, then a details cache)
    BP_BRUSH     = 0x360,   //   ...ButtonBrush (FSlateBrush) -- what its image binding draws
    BP_PROG_VIS  = 0x400,   //   ...ProgressCircleVisibility (ESlateVisibility) and
    BP_PROG_VAL  = 0x404,   //   ...ProgressCircleValue -- both read by the blueprint's own bindings
    BRUSH_RES    = 0x48,    // FSlateBrush::ResourceObject
    BRUSH_NAME   = 0x50,    // FSlateBrush::ResourceName
    BRUSH_HANDLE = 0x70,    // FSlateBrush::ResourceHandle (16 B): the renderer's cache
    OA_OBJECTS   = 0x10,    // FUObjectArray::ObjObjects: chunks +0, MaxElements +0x10, NumElements +0x14, MaxChunks +0x18
    UOBJ_INDEX   = 0x0c,    // UObjectBase::InternalIndex: its slot in that table
    OAI_SIZE     = 0x18,    // FUObjectItem: Object +0, Flags +8, ClusterRootIndex +0xc, SerialNumber +0x10
    OAI_FLAGS    = 0x08,
    OAI_SERIAL   = 0x10,
    OAI_DEAD     = (1 << 28) | (1 << 29),    // EInternalObjectFlags: Unreachable, PendingKill
    VIS_COLLAPSED = 1, VIS_HITTEST_INVISIBLE = 3,
    MAX_ROWS     = 6,
};

// ------------------------------------------------------------------ engine functions
// UUserWidget* UWidgetBlueprintLibrary::Create(UObject* worldContext, UClass* type, APlayerController*)
static const char* SIG_CREATE =
    "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 49 8B E8 48 8B DA 48 8B F1 48 85 D2";
// void UUserWidget::AddToViewport(int32 zOrder)
static const char* SIG_ADD_VIEWPORT =
    "48 8B 01 44 8B C2 33 D2 48 FF A0 ?? ?? ?? ?? CC 45 33 C0 41 8B D0 38 91 79 05 00 00 48 0F 45 D1";
// void UUserWidget::SetPositionInViewport(FVector2D, bool bRemoveDPIScale)   -- the vector packs into rdx
static const char* SIG_SET_POS =
    "40 57 48 83 EC 20 48 89 54 24 30 48 8B F9 45 84 C0 ?? ?? E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ??";
// void UUserWidget::SetAlignmentInViewport(FVector2D) / ::SetDesiredSizeInViewport(FVector2D)
static const char* SIG_SET_ALIGN =
    "48 83 EC 28 66 48 0F 6E CA 0F 2E 89 30 02 00 00 F2 0F 11 4C 24 30 ?? ?? F3 0F 10 44 24 34 0F 2E 81 34 02 00 00";
static const char* SIG_SET_DESIRED =
    "48 83 EC 28 48 89 54 24 30 0F 57 DB F3 0F 10 54 24 30 0F 2E 91 28 02 00 00 F3 0F 10 4C 24 34 ?? ??";
// void UUserWidget::SetVisibility(ESlateVisibility)
static const char* SIG_SET_VIS =
    "40 56 41 56 41 57 48 83 EC 50 44 0F B6 F2 4C 8B F9 48 8D 54 24 20 48 89 5C 24 70 44 88 B1 C3 00 00 00";
// void UUIGamePadButton::SetText(const FText&) / ::SetEnabled(bool)
static const char* SIG_BTN_SETTEXT =
    "48 83 EC 48 48 8B 02 48 89 44 24 20 48 8B 42 08 48 89 44 24 28 48 85 C0 ?? ?? F0 FF 40 08 8B 42 10 48 8D 54 24 20 48 8B 89 00 03 00 00 89 44 24 30";
static const char* SIG_BTN_SETENABLED =
    "48 89 5C 24 08 57 48 83 EC 50 88 91 20 03 00 00 48 8B D9 84 D2 0F 84 ?? ?? ?? ?? 0F B6 81 70 02 00 00";
// FVector2D UWidgetLayoutLibrary::GetViewportSize(UObject*) -- hidden return: rcx = out, rdx = context; pixels
static const char* SIG_VP_SIZE =
    "48 89 5C 24 10 57 48 83 EC 20 44 8B 05 ?? ?? ?? ?? 48 8B D9 65 48 8B 04 25 58 00 00 00 48 8B FA";
// float UWidgetLayoutLibrary::GetViewportScale(UObject*) -- the DPI scale, in xmm0
static const char* SIG_VP_SCALE =
    "40 53 48 83 EC 30 8B 15 ?? ?? ?? ?? 48 8B D9 65 48 8B 04 25 58 00 00 00 B9 08 01 00 00 48 8B 04 D0 8B 04 01 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ??";
// UObject* UTRXPluginSettings::FindControllerKeyIconForControllerType(const FKey&, ETRXControllerType)
// -- the game's own key-to-icon table, called on the settings default object
static const char* SIG_FIND_ICON =
    "48 89 5C 24 10 55 56 57 48 83 EC 20 48 8B 99 18 01 00 00 48 8B EA 48 63 81 20 01 00 00 48 8B F9";
// UTRXControllerKeyWidget::SetKeyToDisplay(const FKey&) -- carries the one call to
// UTRXUtilities::RetrieveCurrentControllerType, which is ICF-folded and so cannot be scanned for
static const char* SIG_SET_KEY_TO_DISPLAY =
    "40 53 48 83 EC 20 48 8B D9 C6 81 98 02 00 00 00 48 81 C1 A0 02 00 00 E8 ?? ?? ?? ?? 48 8B 03 48 8B CB";
static const uint8_t kControllerTypePrologue[13] = {
    0x48,0x83,0xEC,0x28,0x48,0x8B,0xD1,0x41,0xB8,0x01,0x00,0x00,0x00
};
// UObject* StaticFindObject(UClass*, UObject* outer, const TCHAR* name, bool exactClass) -- catch_sound's sig
static const char* SIG_STATIC_FIND =
    "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 "
    "45 0F B6 F1 49 8B F8 48 8B DA 4C 8B";
// UObject* StaticLoadObject(UClass*, UObject* outer, const TCHAR* name, const TCHAR* file, uint32 flags, ...)
static const char* SIG_STATIC_LOAD =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FC FF FF 48 81 EC 78 04 00 00 48 8B 05 ?? ?? ?? ??";
// FName::FName(const ANSICHAR*, EFindName) -- the signature the pause menu already relies on
static const char* SIG_FNAME_CTOR =
    "48 89 5C 24 08 57 48 83 EC 30 41 8B F8 4C 8B CA 48 8B D9 48 85 D2 ?? ?? 48 C7 C0 FF FF FF FF 90";
// UTrickDisplayWidget::SetTrickDisplayText -- carries the one call site FText::FromName is read from
static const char* SIG_TRICK_TEXT =
    "4C 8B DC 55 57 49 8D 6B D8 48 81 EC 18 01 00 00 48 63 81 14 03 00 00 48 8B F9 83 F8 FF 0F 8E ?? ?? ?? ??";
static const uint8_t kFromNamePrologue[20] = {
    0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xC2,0x48,0x8B,0xD9,0x48,0x8B,0xC8,0x48,0x8D,0x54,0x24,0x20
};
// UPropertyValue::HasValidResolve -- carries the one reference to GUObjectArray this needs: exactly one
// rip-relative lea, at the same offset in both shipped builds, pointing at the array (data, so not
// scannable itself)
static const char* SIG_OBJ_ARRAY_REF =
    "48 89 5C 24 08 57 48 83 EC 20 48 83 79 68 00 48 8B D9 0F 84 ?? ?? ?? ?? 48 83 79 60 00 0F 84 ?? ?? ?? ??";

typedef void* (*CreateFn)(void* worldCtx, void* cls, void* owningPc);
typedef void  (*AddViewportFn)(void* self, int zOrder);
typedef void  (*SetPosFn)(void* self, uint64_t packedXY, bool removeDpi);
typedef void  (*SetVec2Fn)(void* self, uint64_t packedXY);
typedef void  (*SetVisFn)(void* self, unsigned char vis);
typedef void  (*BtnSetTextFn)(void* self, const void* ftext);
typedef void  (*BtnSetEnabledFn)(void* self, bool on);
typedef float* (*VpSizeFn)(float* out2, void* worldCtx);
typedef float  (*VpScaleFn)(void* worldCtx);
typedef unsigned char (*ControllerTypeFn)(void* worldCtx);
typedef void* (*FindIconFn)(void* settingsCdo, const void* fkey, unsigned char controllerType);
typedef void* (*StaticFindFn)(void* cls, void* outer, const wchar_t* name, bool exact);
typedef void* (*StaticLoadFn)(void* cls, void* outer, const wchar_t* name, const wchar_t* file,
                              unsigned flags, void* sandbox, bool reconcile, void* ctx);
typedef void* (*FNameCtorFn)(void* outName, const char* ansi, int findType);
typedef void* (*FromNameFn)(void* outText, const void* fname);

static CreateFn        g_create    = nullptr;
static AddViewportFn   g_addVp     = nullptr;
static SetPosFn        g_setPos    = nullptr;
static SetVec2Fn       g_setAlign  = nullptr;
static SetVec2Fn       g_setDesired= nullptr;
static SetVisFn        g_setVis    = nullptr;
static BtnSetTextFn    g_btnText   = nullptr;
static BtnSetEnabledFn g_btnEnable = nullptr;
static VpSizeFn        g_vpSize    = nullptr;
static VpScaleFn       g_vpScale   = nullptr;
static ControllerTypeFn g_ctrlType = nullptr;
static FindIconFn      g_findIcon  = nullptr;
static StaticFindFn    g_find      = nullptr;
static StaticLoadFn    g_load      = nullptr;
static FNameCtorFn     g_fnameCtor = nullptr;
static FromNameFn      g_fromName  = nullptr;
static uint8_t*        g_objArray  = nullptr;
static bool            g_ok        = false;

// ------------------------------------------------------------------ state
static void* g_rowClass = nullptr; static char g_rowName[80] = "";
static void* g_settings = nullptr;      // the UTRXPluginSettings default object: it owns the icon table
static void* g_classFix = nullptr;      // the class of UClass, for telling a UObject from a plain struct
static int   g_iconSaid = 0;
struct Row { void* w; char label[96]; char button; void* glyph; bool shown; SitObjRef ref; };
static SitObjRef g_rowClassRef = { nullptr, 0, 0, nullptr };
static Row   g_rows[MAX_ROWS];
static int   g_nRows = 0, g_laidOut = 0;    // widgets built (the pool), and the count they are stacked for
static void* g_builtFor = nullptr;
static float g_vpW = 0.0f, g_vpH = 0.0f, g_vpScaleV = 1.0f;
static int   g_faults = 0, g_tries = 0, g_scanned = 0, g_reported = 0, g_glyphSaid = 0, g_frame = 0;
static int   g_gateLog = 0;          // SitPromptGateLog: 1/s line saying what the bar was told and what it is
static uint64_t g_gateMs = 0;
static char  g_status[128] = "idle";

// ------------------------------------------------------------------ is it still the object we were given?
// THE CRASH THIS ANSWERS (1.1.12 field test): a fatal in the garbage collector, reading 0x100000c, half
// a minute after a map change. That address is offset 0xc of an "object" at 0x01000000 -- one byte, 1,
// at byte 3 of an otherwise empty pointer slot. UWidget::Visibility is at +0xc3, byte 3 of the slot at
// +0xc0, and 1 is Collapsed: we had collapsed a widget that no longer existed. The pools below kept
// their widgets until a DIFFERENT skater turned up, and with NO skater -- the frames of a level change
// -- they re-collapsed every one of them, every frame. By then the level that made them was gone and
// their memory belonged to new objects; each call wrote its 1 into one of those, and the collector
// found it on its next pass. So nothing kept across frames is touched on the strength of a pointer.
static const uint8_t* ObjItem(int index) {
    if (!g_objArray || index < 0) return nullptr;
    const uint8_t* ch = g_objArray + OA_OBJECTS;
    uint8_t** chunks = *(uint8_t** const*)(ch + 0x00);
    const int maxElems = *(const int*)(ch + 0x10), num = *(const int*)(ch + 0x14), maxChunks = *(const int*)(ch + 0x18);
    if (!chunks || maxChunks <= 0 || index >= num) return nullptr;
    const int per = maxElems / maxChunks;
    if (per <= 0) return nullptr;
    const uint8_t* chunk = chunks[index / per];
    return chunk ? chunk + (size_t)(index % per) * OAI_SIZE : nullptr;
}
bool SitUI_CanVerify() { return g_objArray != nullptr; }
// An engine FWeakObjectPtr is this table's index and the serial the slot had: the object, or null if it
// has gone or the slot is somebody else's now. (A hit result names its physical material this way.)
void* SitUI_ResolveWeak(int index, int serial) {
    if (!g_objArray || index < 0 || serial == 0) return nullptr;
    __try {
        const uint8_t* item = ObjItem(index);
        if (!item || *(const int32_t*)(item + OAI_SERIAL) != serial) return nullptr;
        if (*(const int32_t*)(item + OAI_FLAGS) & OAI_DEAD) return nullptr;
        return *(void* const*)item;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void SitUI_Track(SitObjRef* ref, void* obj) {
    if (!ref) return;
    ref->obj = nullptr; ref->index = -1; ref->serial = 0; ref->cls = nullptr;
    if (!obj) return;
    __try {
        const int index = *(const int32_t*)((const uint8_t*)obj + UOBJ_INDEX);
        const uint8_t* item = ObjItem(index);
        // Without the table the reference is still kept, unverifiable: SitUI_Alive then answers for the
        // pointer alone, and the callers drop everything whenever there is no skater (see the pumps).
        if (g_objArray && (!item || *(void* const*)item != obj)) return;
        ref->index  = index;
        ref->serial = item ? *(const int32_t*)(item + OAI_SERIAL) : 0;
        ref->cls    = *(void* const*)((const uint8_t*)obj + UOBJ_CLASS);
        ref->obj    = obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ref->obj = nullptr; }
}
bool SitUI_Alive(const SitObjRef* ref) {
    if (!ref || !ref->obj) return false;
    if (!g_objArray) return true;
    __try {
        const uint8_t* item = ObjItem(ref->index);
        if (!item || *(void* const*)item != ref->obj) return false;             // freed, or the slot is someone else's
        if (*(const int32_t*)(item + OAI_FLAGS) & OAI_DEAD) return false;       // on its way out
        // A serial number is only handed out once something takes a weak pointer, so none at the time we
        // looked proves nothing; one that has since changed is a different object in the same slot.
        if (ref->serial && *(const int32_t*)(item + OAI_SERIAL) != ref->serial) return false;
        return *(void* const*)((const uint8_t*)ref->obj + UOBJ_CLASS) == ref->cls;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void Widen(const char* a, wchar_t* w, int cap) {
    int i = 0; for (; a[i] && i < cap - 1; i++) w[i] = (wchar_t)(unsigned char)a[i]; w[i] = 0;
}
static bool IsBpName(const char* n) { const size_t l = strlen(n); return l > 2 && n[l - 2] == '_' && n[l - 1] == 'C'; }
static uint64_t Pack2(float x, float y) {
    uint32_t a, b; memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    return (uint64_t)a | ((uint64_t)b << 32);
}
static uint64_t MakeName(const char* s) {
    uint64_t fn = 0;
    if (!g_fnameCtor || !s || !*s) return 0;
    __try { g_fnameCtor(&fn, s, 1 /* FNAME_Add */); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return 0; }
    return fn;
}

// ------------------------------------------------------------------ finding the class
static uint8_t* ResolveObjArray() {
    uint8_t* fn = TwkScanExe(SIG_OBJ_ARRAY_REF);
    if (!fn) return nullptr;
    __try {
        for (int o = 0; o < 0xd1 - 7; o++) {
            if (fn[o] != 0x48 || fn[o + 1] != 0x8D) continue;
            const uint8_t m = fn[o + 2];
            if (m != 0x0D && m != 0x15 && m != 0x05 && m != 0x1D && m != 0x35 && m != 0x3D) continue;
            int32_t disp; memcpy(&disp, fn + o + 3, 4);
            return fn + o + 7 + disp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}
// UTRXUtilities::RetrieveCurrentControllerType is byte-identical to other functions, so it is read out
// of the single call in SetKeyToDisplay that lands on it, and believed only if the target starts the
// way it does.
static ControllerTypeFn ResolveControllerType() {
    uint8_t* fn = TwkScanExe(SIG_SET_KEY_TO_DISPLAY);
    if (!fn) return nullptr;
    __try {
        for (int o = 0; o < 0x40; o++) {
            if (fn[o] != 0xE8) continue;
            int32_t disp; memcpy(&disp, fn + o + 1, 4);
            uint8_t* tgt = fn + o + 5 + disp;
            if (memcmp(tgt, kControllerTypePrologue, sizeof(kControllerTypePrologue)) == 0) return (ControllerTypeFn)tgt;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}
static void Learn(void* cls, const char* nb) {
    g_rowClass = cls;
    SitUI_Track(&g_rowClassRef, cls);
    snprintf(g_rowName, sizeof(g_rowName), "%s", nb);
    void* outer = *(void**)((uint8_t*)cls + UOBJ_OUTER);
    char pk[128] = "";
    if (outer && GrindPop_FNameToString((const uint8_t*)outer + UOBJ_NAME, pk, sizeof(pk)) && pk[0] == '/') {
        char path[160]; snprintf(path, sizeof(path), "%s.%s", pk, nb);
        if (strcmp(path, g_rowPath) != 0) { snprintf(g_rowPath, sizeof(g_rowPath), "%s", path); TwkMarkDirty(); }
    }
    TwkLog("[situi] button class: %s (%s)", g_rowName, g_rowPath[0] ? g_rowPath : "no package");
}
static void LoadByPath() {
    if (g_rowClass || !g_rowPath[0] || !g_load) return;
    __try {
        wchar_t wp[160]; Widen(g_rowPath, wp, 160);
        void* c = g_find ? g_find(nullptr, (void*)-1, wp, false) : nullptr;
        if (!c) c = g_load(nullptr, nullptr, wp, nullptr, 0, nullptr, true, nullptr);
        char cn[80] = "";
        if (c) GrindPop_FNameToString((const uint8_t*)c + UOBJ_NAME, cn, sizeof(cn));
        if (c && IsBpName(cn)) { g_rowClass = c; SitUI_Track(&g_rowClassRef, c); snprintf(g_rowName, sizeof(g_rowName), "%s", cn);
                                 TwkLog("[situi] button class loaded from %s", g_rowPath); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
// Class objects are the ones whose class's class is the fixpoint of the chain (the class of UClass is
// UClass): two pointer reads each, and only those get their names resolved. The skater seeds it.
static void ScanForClass(void* seed) {
    if ((g_rowClass && g_settings) || g_scanned || !seed || !g_objArray) return;
    g_scanned = 1;
    __try {
        void* fix = *(void**)((uint8_t*)seed + UOBJ_CLASS);
        for (int hop = 0; fix && hop < 8; hop++) { void* up = *(void**)((uint8_t*)fix + UOBJ_CLASS); if (up == fix) break; fix = up; }
        if (!fix) return;
        g_classFix = fix;
        const uint8_t* ch = g_objArray + OA_OBJECTS;
        uint8_t** chunks = *(uint8_t***)(ch + 0x00);
        const int maxElems = *(const int*)(ch + 0x10), numElems = *(const int*)(ch + 0x14), maxChunks = *(const int*)(ch + 0x18);
        if (!chunks || numElems <= 0 || maxChunks <= 0) return;
        const int perChunk = maxElems / maxChunks;
        if (perChunk <= 0) return;
        int classes = 0;
        for (int i = 0; i < numElems && !(g_rowClass && g_settings); i++) {
            uint8_t* chunk = chunks[i / perChunk];
            if (!chunk) continue;
            uint8_t* obj = *(uint8_t**)(chunk + (size_t)(i % perChunk) * 24);
            if (!obj) continue;
            uint8_t* cls = *(uint8_t**)(obj + UOBJ_CLASS);
            if (!cls || *(void**)(cls + UOBJ_CLASS) != fix) continue;
            classes++;
            char nb[80];
            if (!GrindPop_FNameToString(obj + UOBJ_NAME, nb, sizeof(nb))) continue;
            // the settings class holds the icon table on its default object
            if (!g_settings && !strcmp(nb, "TRXPluginSettings")) {
                g_settings = *(void**)(obj + 0x118);          // UClass::ClassDefaultObject
                TwkLog("[situi] controller icon table: %s default object %p", nb, g_settings);
            }
            if (!IsBpName(nb)) continue;
            if (!g_rowClass && strstr(nb, g_rowMatch)) Learn(obj, nb);
        }
        TwkLog("[situi] object array: %d objects, %d classes; button class %s, icon table %s", numElems, classes,
               g_rowClass ? "found" : "NOT FOUND", g_settings ? "found" : "not found");
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

// ------------------------------------------------------------------ FText, the safe way
static bool MakeText(const char* s, void* out24) {
    if (!g_fromName || !s || !*s) return false;
    const uint64_t fn = MakeName(s);
    if (!fn) return false;
    __try { memset(out24, 0, 24); g_fromName(out24, &fn); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
}

// ------------------------------------------------------------------ the button
// The key it stands for, and the blueprint's own construct script to turn that into the right icon for
// the controller in your hands. This is what the game does for its own prompts.
static const char* KeyNameFor(char button) {
    switch (button) {
        case 'A': return "Gamepad_FaceButton_Bottom";
        case 'B': return "Gamepad_FaceButton_Right";
        case 'X': return "Gamepad_FaceButton_Left";
        case 'Y': return "Gamepad_FaceButton_Top";
        case 'L': return "Gamepad_DPad_Left";
        case 'R': return "Gamepad_DPad_Right";
        case 'T': return "Gamepad_RightTrigger";          // a Rage's throw (emote.cpp)
        case 'S': return "Gamepad_RightShoulder";         // the board tap's RB (emote.cpp)
        default:  return nullptr;
    }
}
// Is this pointer a UObject? Its class's class is the class of UClass, same test the scan uses. The
// icon table is documented as returning "an icon" and that is either the texture itself or a brush
// holding one, so the answer decides how to read it.
static void* AsTexture(void* p) {
    if (!p) return nullptr;
    __try {
        void* cls = *(void**)((uint8_t*)p + UOBJ_CLASS);
        if (cls && g_classFix && *(void**)((uint8_t*)cls + UOBJ_CLASS) == g_classFix) return p;   // a UObject
        void* res = *(void**)((uint8_t*)p + BRUSH_RES);                                            // an FSlateBrush
        if (res) {
            void* rc = *(void**)((uint8_t*)res + UOBJ_CLASS);
            if (rc && g_classFix && *(void**)((uint8_t*)rc + UOBJ_CLASS) == g_classFix) return res;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    return nullptr;
}
// Write the button's key, then ask the game for the icon that key wears on the controller in your
// hands and put it in the brush the button draws.
static void SetRowKey(void* w, char button) {
    const char* kn = KeyNameFor(button);
    if (!w || !kn) return;
    const uint64_t fn = MakeName(kn);
    if (!fn) return;
    uint64_t key[3] = { fn, 0, 0 };        // FKey: the name, and an empty details cache to force a resolve
    __try { memcpy((uint8_t*)w + BP_KEY, key, sizeof(key)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return; }
    if (!g_ctrlType || !g_findIcon || !g_settings) {
        if (!g_iconSaid++) TwkLog("[situi] the game's icon table is not usable (%s%s%s) -- falling back to the icon name swap",
                                  g_ctrlType ? "" : "no controller-type call ", g_findIcon ? "" : "no lookup call ",
                                  g_settings ? "" : "no settings object");
        return;
    }
    __try {
        const unsigned char type = g_ctrlType(w);
        void* icon = g_findIcon(g_settings, key, type);
        void* tex = AsTexture(icon);
        if (!tex) {
            if (!g_iconSaid++) TwkLog("[situi] the game's icon table gave nothing for %s on controller type %u", kn, (unsigned)type);
            return;
        }
        uint8_t* brush = (uint8_t*)w + BP_BRUSH;
        *(void**)(brush + BRUSH_RES) = tex;
        *(uint64_t*)(brush + BRUSH_NAME) = *(const uint64_t*)((uint8_t*)tex + UOBJ_NAME);
        *(uint64_t*)(brush + BRUSH_HANDLE) = 0; *(uint64_t*)(brush + BRUSH_HANDLE + 8) = 0;
        if (g_iconSaid++ < 3) {
            char tn[64] = "?"; GrindPop_FNameToString((const uint8_t*)tex + UOBJ_NAME, tn, sizeof(tn));
            TwkLog("[situi] %s on controller type %u is %s", kn, (unsigned)type, tn);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
// The fallback, for if the construct script does not set the icon: swap the last token of whatever
// icon the brush holds, keeping its family, so an Xbox A becomes an Xbox B and a PlayStation Cross
// becomes a PlayStation Circle.
static bool WantedIcon(const char* cur, char button, char* out, int cap) {
    static const char* const xb[4] = { "A", "B", "X", "Y" };
    static const char* const ps[4] = { "Cross", "Circle", "Square", "Triangle" };
    const int k = button == 'A' ? 0 : button == 'B' ? 1 : button == 'X' ? 2 : button == 'Y' ? 3 : -1;
    if (k < 0 || !cur) return false;
    const char* us = strrchr(cur, '_');
    if (!us) return false;
    const char* tail = us + 1;
    bool isXb = false, isPs = false;
    for (int i = 0; i < 4; i++) { if (!strcmp(tail, xb[i])) isXb = true; if (!strcmp(tail, ps[i])) isPs = true; }
    if (!isXb && !isPs) return false;
    snprintf(out, cap, "%.*s%s", (int)(us + 1 - cur), cur, isXb ? xb[k] : ps[k]);
    return true;
}
static void ApplyGlyph(Row& r) {
    if (!r.w || !g_find) return;
    if (g_ctrlType && g_findIcon && g_settings) return;    // the game's own table already answered
    __try {
        uint8_t* brush = (uint8_t*)r.w + BP_BRUSH;
        void* cur = *(void**)(brush + BRUSH_RES);
        if (!cur || cur == r.glyph) return;              // unchanged since we last looked
        char cn[64] = ""; GrindPop_FNameToString((const uint8_t*)cur + UOBJ_NAME, cn, sizeof(cn));
        char want[64];
        if (!WantedIcon(cn, r.button, want, sizeof(want)) || !strcmp(want, cn)) { r.glyph = cur; return; }
        wchar_t ww[64]; Widen(want, ww, 64);
        void* outer = *(void**)((uint8_t*)cur + UOBJ_OUTER);
        void* tex = outer ? g_find(nullptr, outer, ww, false) : nullptr;
        if (!tex) tex = g_find(nullptr, (void*)-1, ww, false);
        if (!tex) { if (!g_glyphSaid++) TwkLog("[situi] button texture %s not found", want); r.glyph = cur; return; }
        *(void**)(brush + BRUSH_RES) = tex;
        *(uint64_t*)(brush + BRUSH_NAME) = *(const uint64_t*)((uint8_t*)tex + UOBJ_NAME);
        *(uint64_t*)(brush + BRUSH_HANDLE) = 0; *(uint64_t*)(brush + BRUSH_HANDLE + 8) = 0;
        r.glyph = tex;
        if (g_glyphSaid++ < 4) TwkLog("[situi] glyph: %s -> %s", cn, want);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
static void SetRing(void* w, float frac) {
    if (!w) return;
    if (!g_holdRing) frac = 0.0f;
    __try {
        *(float*)((uint8_t*)w + BP_PROG_VAL) = frac;
        *(unsigned char*)((uint8_t*)w + BP_PROG_VIS) = frac > 0.01f ? 0 /* Visible */ : (unsigned char)VIS_COLLAPSED;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
static void SetRowText(Row& r, const char* label) {
    if (!r.w || !g_btnText || !label || !strcmp(label, r.label)) return;
    uint8_t ft[24];
    if (!MakeText(label, ft)) return;
    __try { g_btnText(r.w, ft); snprintf(r.label, sizeof(r.label), "%s", label); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

// ------------------------------------------------------------------ building
static void MeasureViewport(void* skater) {
    g_vpW = g_vpH = 0.0f; g_vpScaleV = 1.0f;
    if (!g_vpSize) return;
    __try {
        float px[2] = { 0.0f, 0.0f };
        g_vpSize(px, skater);
        float sc = g_vpScale ? g_vpScale(skater) : 1.0f;
        if (!(sc > 0.01f)) sc = 1.0f;
        g_vpScaleV = sc; g_vpW = px[0] / sc; g_vpH = px[1] / sc;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; g_vpW = g_vpH = 0.0f; }
}
// One entry. The viewport slot keeps the engine's default point anchor at the top-left (the anchor
// setter does not take on a widget added this way), so the position IS the slot's corner and the
// desired size IS the slot's size. Entry 0 is the top of the list.
static bool BuildRow(int i, int count, void* skater, void* pc) {
    Row& r = g_rows[i];
    r.w = nullptr; r.label[0] = 0; r.glyph = nullptr; r.shown = false; r.button = 0;
    if (!g_rowClass || g_vpW <= 0.0f) return false;
    __try {
        void* w = g_create(skater, g_rowClass, pc);
        if (!w) return false;
        g_addVp(w, g_zOrder);
        if (g_setAlign) g_setAlign(w, Pack2(0.0f, 0.0f));
        const float x = g_vpW - g_barRight - g_barW;
        const float y = g_vpH - g_barBottom - g_barH - g_barGap * (float)(count - 1 - i);
        g_setPos(w, Pack2(x, y), false);
        if (g_setDesired) g_setDesired(w, Pack2(g_barW, g_barH));
        if (g_btnEnable) g_btnEnable(w, true);
        g_setVis(w, VIS_COLLAPSED);
        SitUI_Track(&r.ref, w);
        if (!r.ref.obj) { TwkLog("[situi] entry %d is not in the object table as itself -- not kept", i); return false; }
        r.w = w;
        TwkLog("[situi] entry %d at (%.0f, %.0f) of %.0fx%.0f, scale %.2f", i, x, y, g_vpW, g_vpH, g_vpScaleV);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; r.w = nullptr; return false; }
}
// `force` re-issues the visibility even when we believe it is already right. Our cached `shown` is only
// as good as the assumption that nothing ELSE writes these widgets -- and a bar that stayed on screen
// in the replay and prop editors says something might. Cheap to re-assert now and then.
static void ShowRow(Row& r, bool on, bool force = false) {
    if (!r.w || (r.shown == on && !force) || !g_setVis) return;
    r.shown = on;
    __try { g_setVis(r.w, on ? VIS_HITTEST_INVISIBLE : VIS_COLLAPSED); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; r.w = nullptr; }
}

// Forgotten, never touched: whatever they were, they are the engine's now.
static void DropRows(const char* why) {
    if (g_nRows && why) TwkLog("[situi] the bar's %d widgets forgotten: %s", g_nRows, why);
    for (int i = 0; i < MAX_ROWS; i++) { g_rows[i].w = nullptr; g_rows[i].ref.obj = nullptr; }
    g_nRows = 0; g_builtFor = nullptr; g_reported = 0; g_laidOut = 0; g_tries = 0;
}
static bool RowsAlive() {
    for (int i = 0; i < g_nRows; i++) if (g_rows[i].w && !SitUI_Alive(&g_rows[i].ref)) return false;
    return true;
}
static void CheckRowClass() {
    if (g_rowClass && !SitUI_Alive(&g_rowClassRef)) { TwkLog("[situi] the button class was unloaded -- looked up again"); g_rowClass = nullptr; }
}
void SitUI_PumpFrame(void* skater, bool show, const SitPromptEntry* entries, int count) {
    if (!g_ok || !g_on) return;
    if (count > MAX_ROWS) count = MAX_ROWS;
    // FIRST, before anything reads or writes them: are they still the widgets that were built? They are
    // made together and die together, so one that is not settles it for all (see SitUI_Alive's note).
    if (g_nRows && !RowsAlive()) DropRows("no longer the objects they were (the level that made them is gone)");
    // The widgets are dropped, never touched, ONLY on a new skater: the level that made them is gone
    // and the engine owns what it collected. A change in how many entries are asked for is NOT that.
    // Dropping the references on a count change left the old widgets in the viewport with new ones
    // built on top -- entering first person (3 -> 4 rows) stacked the bar on itself every time, and
    // standing up could not hide what it had forgotten. The pool grows to the largest count ever asked
    // for and is stacked again for the current one; rows past the count collapse.
    // A NULL skater is NOT a new level -- it is the replay editor or the prop editor taking the pawn
    // away for as long as it is open. Treating it as one dropped every widget reference while the
    // widgets were still VISIBLE in the viewport (the same orphaning as the count-change bug above, by
    // another road), and the early return below then meant nothing ever hid them: the bar stayed on
    // screen for the whole editor session. So hide them and KEEP them -- only a different, NON-NULL
    // skater means the level that made them is gone.
    if (skater && skater != g_builtFor) DropRows(nullptr);
    if (!skater) {
        // No skater is an editor holding the pawn -- or a level change. Hiding is only safe because every
        // row was just verified; in a build where nothing can be verified they are forgotten instead.
        if (!SitUI_CanVerify()) { DropRows("no skater, and this build cannot verify them"); return; }
        for (int i = 0; i < g_nRows; i++) ShowRow(g_rows[i], false, true);
        return;
    }
    if (g_nRows < count && show && count > 0) {
        CheckRowClass();
        if (!g_rowClass) LoadByPath();
        // The scan also finds the settings object the icon table lives on, which no ini path can
        // supply -- so it runs whenever either is still missing, not only when the class is.
        if (!g_rowClass || !g_settings) ScanForClass(skater);
        if (!g_rowClass) {
            static bool said = false;
            if (!said) { said = true; TwkLog("[situi] the button widget's class is not loaded -- no prompts yet"); }
            snprintf(g_status, sizeof(g_status), "waiting for the button class");
            return;
        }
        if (g_tries++ > 3) return;
        void* pc = nullptr;
        __try { pc = *(void**)((uint8_t*)skater + PAWN_CTRL); } __except (EXCEPTION_EXECUTE_HANDLER) { pc = nullptr; }
        if (!pc) { snprintf(g_status, sizeof(g_status), "no player controller"); return; }
        MeasureViewport(skater);
        g_builtFor = skater;
        for (int i = g_nRows; i < count; i++) { if (!BuildRow(i, count, skater, pc)) break; g_nRows = i + 1; }
        if (!g_nRows) { snprintf(g_status, sizeof(g_status), "the buttons could not be created"); return; }
        g_laidOut = 0;
        snprintf(g_status, sizeof(g_status), "ready (%s, %d)", g_rowName, g_nRows);
    }
    if (!g_nRows) return;
    if (show && g_laidOut != count) {   // stack the pool for THIS count: entry 0 at the top, the last at the bottom
        for (int i = 0; i < g_nRows; i++) {
            if (!g_rows[i].w) continue;
            __try {
                const float x = g_vpW - g_barRight - g_barW;
                const float y = g_vpH - g_barBottom - g_barH - g_barGap * (float)(count - 1 - i);
                g_setPos(g_rows[i].w, Pack2(x, y), false);
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; g_rows[i].w = nullptr; }
        }
        g_laidOut = count;
    }
    g_frame++;
    if (show) {
        for (int i = 0; i < count && i < g_nRows; i++) {
            Row& r = g_rows[i];
            SetRowText(r, entries[i].label);
            // The key drives the glyph through the blueprint's own script; re-run now and then so a
            // controller swapped mid-session is picked up. The brush swap below is only a fallback.
            if (r.button != entries[i].button || (g_frame % 240) == 0) { r.button = entries[i].button; SetRowKey(r.w, r.button); }
            ApplyGlyph(r);
            SetRing(r.w, entries[i].ring);
        }
        if (g_reported < 2 && g_nRows) {
            g_reported++;
            __try {
                char bn[64] = "-";
                void* cur = *(void**)((uint8_t*)g_rows[0].w + BP_BRUSH + BRUSH_RES);
                if (cur) GrindPop_FNameToString((const uint8_t*)cur + UOBJ_NAME, bn, sizeof(bn));
                TwkLog("[situi] showing %d %s | entry 0 slate %s, vis %u, icon %s", g_nRows, g_nRows == 1 ? "entry" : "entries",
                       *(void**)((uint8_t*)g_rows[0].w + UUW_FULLSCREEN) ? "yes" : "NO",
                       (unsigned)*(const unsigned char*)((uint8_t*)g_rows[0].w + UW_VIS), bn);
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
        }
    }
    // Re-assert about twice a second, so a hide that something else undid does not stick.
    const bool force = (g_frame % 30) == 0;
    for (int i = 0; i < g_nRows; i++) ShowRow(g_rows[i], show && i < count, force);
    if (g_gateLog && g_nRows) {
        const uint64_t ms = GetTickCount64();
        if (ms - g_gateMs > 1000) {
            g_gateMs = ms;
            unsigned vis = 255;
            __try { vis = *(const unsigned char*)((const uint8_t*)g_rows[0].w + UW_VIS); }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
            TwkLog("[situi] gate: show=%d rows=%d count=%d row0 shown=%d engineVis=%u",
                   show ? 1 : 0, g_nRows, count, g_rows[0].shown ? 1 : 0, vis);
        }
    }
}

void SitUI_HideNow() {
    if (!g_ok || !g_on) return;
    if (g_nRows && (!SitUI_CanVerify() || !RowsAlive())) { DropRows("gone by the time they were to be hidden"); return; }
    for (int i = 0; i < g_nRows; i++) ShowRow(g_rows[i], false, true);
}

// ------------------------------------------------------------------ free labels (the radial menu)
// void UWidget::SetVisibility(ESlateVisibility) -- the BASE one. SIG_SET_VIS above is UUserWidget's
// override, which reads members a plain panel does not have; a sub-widget must go through this.
static const char* SIG_WIDGET_SET_VIS =
    "40 53 48 83 EC 50 0F B6 DA 48 8D 54 24 20 88 99 C3 00 00 00 E8 ?? ?? ?? ?? 4C 8B 44 24 20 4D 85 C0 ?? ?? 8B CB 84 DB ?? ??";
// void UWidget::SetRenderOpacity(float). UE::MovieScene::SetRenderOpacity(UObject*, float) is the same
// bytes and the same effect, so whichever the scan lands on first is right.
static const char* SIG_WIDGET_SET_OPACITY =
    "40 53 48 83 EC 40 0F 29 74 24 30 48 8D 54 24 20 0F 28 F1 F3 0F 11 B1 C4 00 00 00 E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 85 C9";
// void UTextBlock::SetColorAndOpacity(FSlateColor) -- the struct is 40 bytes, so it arrives by
// pointer; the callee destroys it, which is nothing for one whose linked-colour pointer is null.
// What is lit is said with COLOUR. Fading the rest instead made every entry hard to read over play.
static const char* SIG_TEXT_SET_COLOR =
    "40 53 48 83 EC 60 48 8B D9 48 89 7C 24 78 48 81 C1 50 01 00 00 48 8B FA E8 ?? ?? ?? ?? 48 83 BB 98 02 00 00 00 0F 84 ?? ?? ?? ??";
typedef void (*WidgetSetOpacityFn)(void* self, float opacity);
typedef void (*TextSetColorFn)(void* self, const void* slateColor40);
static SetVisFn           g_widgetVis  = nullptr;
static WidgetSetOpacityFn g_setOpacity = nullptr;
static TextSetColorFn     g_textColor  = nullptr;
static bool               g_freeLooked = false;

enum { MAX_FREE = 12, BP_IMAGE_PANEL = 0x330 /* PBP_UIGamePadButton_C::ButtonImagePanel: the glyph */,
       BTN_TEXT_BLOCK = 0x300 /* UUIGamePadButton::_buttonTextBlock (UTextBlock*) */,
       TB_FONT = 0x188 /* UTextBlock::Font (FSlateFontInfo) */, FONT_OUTLINE_SIZE = 0x10, FONT_OUTLINE_COLOR = 0x20,
       FONT_SIZE = 0x48, TB_SHADOW_OFFSET = 0x268, TB_SHADOW_COLOR = 0x270,
       UOBJ_FLAGS = 0x08, RF_CDO_OR_ARCHETYPE = 0x30, USTRUCT_SUPER = 0x40,
       ITEM_FLAGS = 0x08, ITEM_DEAD = (1 << 28) | (1 << 29) /* unreachable, pending kill */ };
struct FreeRow { void* w; char label[64]; float x, y; int lit; bool shown; SitObjRef ref; };
static FreeRow g_free[MAX_FREE];
static int     g_nFree = 0, g_freeTries = 0, g_freeFrame = 0;
static void*   g_freeFor = nullptr;
static bool    g_freeWasShown = false;
// How wide a character of the button's label draws, in Slate units. The widget right-aligns its label
// against its slot, so centring a label on a point needs its width, and nothing reports one.
static float   g_freeCharW = 16.0f, g_freeDim = 0.9f, g_freeTextScale = 1.3f;
void SitUI_SetFreeMetrics(float charW, float dim) { if (charW > 1.0f) g_freeCharW = charW; if (dim > 0.0f) g_freeDim = dim; }

static void FreeResolve() {
    if (g_freeLooked) return;
    g_freeLooked = true;
    g_widgetVis  = (SetVisFn)TwkScanExe(SIG_WIDGET_SET_VIS);
    g_setOpacity = (WidgetSetOpacityFn)TwkScanExe(SIG_WIDGET_SET_OPACITY);
    g_textColor  = (TextSetColorFn)TwkScanExe(SIG_TEXT_SET_COLOR);
    TwkLog("[situi] free labels: sub-widget visibility %s, opacity %s, text colour %s", g_widgetVis ? "ok" : "MISSING (the glyph stays)",
           g_setOpacity ? "ok" : "MISSING (no dimming)", g_textColor ? "ok" : "MISSING (the lit entry is not tinted)");
}
static bool BuildFree(int i, void* skater, void* pc) {
    FreeRow& r = g_free[i];
    r.w = nullptr; r.label[0] = 0; r.x = r.y = -1.0e9f; r.lit = -1; r.shown = false;
    if (!g_rowClass || g_vpW <= 0.0f) return false;
    __try {
        void* w = g_create(skater, g_rowClass, pc);
        if (!w) return false;
        // THE LOOK IS WRITTEN BEFORE THE WIDGET REACHES THE SCREEN. Created, the text block is only a
        // UObject holding properties; its Slate half is built by AddToViewport, which reads them. So an
        // outline, a larger size and a shadow cost four plain writes here and no engine call at all.
        // A label over play has nothing behind it, and a black outline is what makes it readable on a
        // bright floor and a dark one alike.
        if (uint8_t* t = *(uint8_t**)((uint8_t*)w + BTN_TEXT_BLOCK)) {
            *(int32_t*)(t + TB_FONT + FONT_OUTLINE_SIZE) = 2;
            float* oc = (float*)(t + TB_FONT + FONT_OUTLINE_COLOR); oc[0] = oc[1] = oc[2] = 0.0f; oc[3] = 1.0f;
            int32_t* size = (int32_t*)(t + TB_FONT + FONT_SIZE);
            if (*size > 4 && *size < 96) *size = (int32_t)((float)*size * g_freeTextScale + 0.5f);
            float* so = (float*)(t + TB_SHADOW_OFFSET); so[0] = so[1] = 2.0f;
            float* sc = (float*)(t + TB_SHADOW_COLOR);  sc[0] = sc[1] = sc[2] = 0.0f; sc[3] = 0.9f;
        }
        g_addVp(w, g_zOrder + 1);
        if (g_setAlign) g_setAlign(w, Pack2(0.0f, 0.0f));
        g_setPos(w, Pack2(-4000.0f, -4000.0f), false);          // off screen until it is given a place
        if (g_setDesired) g_setDesired(w, Pack2(g_barW, g_barH));
        if (g_btnEnable) g_btnEnable(w, true);
        void* glyph = *(void**)((uint8_t*)w + BP_IMAGE_PANEL);
        if (glyph && g_widgetVis) g_widgetVis(glyph, VIS_COLLAPSED);
        g_setVis(w, VIS_COLLAPSED);
        SitUI_Track(&r.ref, w);
        if (!r.ref.obj) { TwkLog("[situi] free label %d is not in the object table as itself -- not kept", i); return false; }
        r.w = w;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; r.w = nullptr; return false; }
}
static void ShowFree(FreeRow& r, bool on, bool force) {
    if (!r.w || (r.shown == on && !force) || !g_setVis) return;
    r.shown = on;
    __try { g_setVis(r.w, on ? VIS_HITTEST_INVISIBLE : VIS_COLLAPSED); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; r.w = nullptr; }
}
bool SitUI_Viewport(void* skater, float* w, float* h) {
    if (!g_ok || !skater) return false;
    MeasureViewport(skater);
    if (w) *w = g_vpW;
    if (h) *h = g_vpH;
    return g_vpW > 1.0f && g_vpH > 1.0f;
}
static void DropFree(const char* why) {
    if (g_nFree && why) TwkLog("[situi] the %d free labels forgotten: %s", g_nFree, why);
    for (int i = 0; i < MAX_FREE; i++) { g_free[i].w = nullptr; g_free[i].ref.obj = nullptr; }
    g_nFree = 0; g_freeFor = nullptr; g_freeTries = 0; g_freeWasShown = false;
}
static bool FreeAlive() {
    for (int i = 0; i < g_nFree; i++) if (g_free[i].w && !SitUI_Alive(&g_free[i].ref)) return false;
    return true;
}
// Off the screen NOW, for an owner whose pump has stopped (see Radial_OnInputKey). Verified first, like
// every other touch: after a level change there is nothing left to hide and nothing may be written.
void SitUI_HideFreeNow() {
    if (!g_ok || !g_nFree) return;
    if (!SitUI_CanVerify() || !FreeAlive()) { DropFree("gone by the time they were to be hidden"); return; }
    for (int i = 0; i < g_nFree; i++) ShowFree(g_free[i], false, true);
    g_freeWasShown = false;
}
void SitUI_PumpFree(void* skater, bool show, const SitFreeLabel* labels, int count) {
    if (!g_ok) return;
    if (count > MAX_FREE) count = MAX_FREE;
    if (count < 0 || !labels) { count = 0; show = false; }
    // The same order as the bar, for the same reason: verified first, and only then read or written.
    if (g_nFree && !FreeAlive()) DropFree("no longer the objects they were (the level that made them is gone)");
    if (skater && skater != g_freeFor) DropFree(nullptr);
    if (!skater) {
        if (!SitUI_CanVerify()) { DropFree("no skater, and this build cannot verify them"); return; }
        for (int i = 0; i < g_nFree; i++) ShowFree(g_free[i], false, true);
        g_freeWasShown = false; return;
    }
    if (g_nFree < count && show) {
        FreeResolve();
        CheckRowClass();
        if (!g_rowClass) LoadByPath();
        if (!g_rowClass || !g_settings) ScanForClass(skater);
        if (!g_rowClass || g_freeTries++ > 3) return;
        void* pc = nullptr;
        __try { pc = *(void**)((uint8_t*)skater + PAWN_CTRL); } __except (EXCEPTION_EXECUTE_HANDLER) { pc = nullptr; }
        if (!pc) return;
        MeasureViewport(skater);
        g_freeFor = skater;
        for (int i = g_nFree; i < count; i++) { if (!BuildFree(i, skater, pc)) break; g_nFree = i + 1; }
        TwkLog("[situi] free labels: %d built of %d wanted (viewport %.0fx%.0f, scale %.2f)", g_nFree, count, g_vpW, g_vpH, g_vpScaleV);
    }
    if (!g_nFree) return;
    g_freeFrame++;
    const bool force = (g_freeFrame % 30) == 0 || show != g_freeWasShown;
    g_freeWasShown = show;
    for (int i = 0; i < g_nFree; i++) {
        FreeRow& r = g_free[i];
        const bool on = show && i < count && labels[i].label && labels[i].label[0];
        if (on && r.w) {
            const SitFreeLabel& L = labels[i];
            if (strcmp(L.label, r.label) != 0 && g_btnText) {
                uint8_t ft[24];
                if (MakeText(L.label, ft)) {
                    __try { g_btnText(r.w, ft); snprintf(r.label, sizeof(r.label), "%s", L.label); r.x = -1.0e9f; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
                }
            }
            if (L.x != r.x || L.y != r.y) {
                // The label's right edge sits on the slot's right edge, so the slot is put half a
                // label to the right of the wanted centre.
                const float half = (float)strlen(r.label) * g_freeCharW * 0.5f;
                __try { g_setPos(r.w, Pack2(L.x + half - g_barW, L.y - g_barH * 0.5f), false); r.x = L.x; r.y = L.y; }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; r.w = nullptr; }
            }
            const int lit = L.lit ? 1 : 0;
            if (r.w && lit != r.lit) {
                __try {
                    if (g_setOpacity) g_setOpacity(r.w, lit ? 1.0f : g_freeDim);
                    void* text = *(void**)((uint8_t*)r.w + BTN_TEXT_BLOCK);
                    if (text && g_textColor) {
                        // FSlateColor: the colour, the rule byte (0 = use this colour), a null link.
                        struct { float rgba[4]; uint8_t rule; uint8_t pad[7]; void* link[2]; } col = {};
                        if (lit) { col.rgba[0] = 1.0f;  col.rgba[1] = 0.74f; col.rgba[2] = 0.16f; }   // the game's amber
                        else     { col.rgba[0] = 0.92f; col.rgba[1] = 0.92f; col.rgba[2] = 0.92f; }
                        col.rgba[3] = 1.0f;
                        g_textColor(text, &col);
                    }
                    r.lit = lit;
                } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
            }
        }
        ShowFree(r, on, force);
    }
}

// ------------------------------------------------------------------ looking things up in the object array
// The class of UClass: the fixpoint any object's Class chain ends at.
static void* ClassFixpoint(void* seed) {
    if (g_classFix) return g_classFix;
    if (!seed) return nullptr;
    void* fix = *(void**)((uint8_t*)seed + UOBJ_CLASS);
    for (int hop = 0; fix && hop < 8; hop++) { void* up = *(void**)((uint8_t*)fix + UOBJ_CLASS); if (up == fix) break; fix = up; }
    g_classFix = fix;
    return fix;
}
struct ObjWalk { uint8_t** chunks; int num, perChunk; };
static bool ObjWalkBegin(ObjWalk& w) {
    if (!g_objArray) return false;
    const uint8_t* ch = g_objArray + OA_OBJECTS;
    w.chunks = *(uint8_t***)(ch + 0x00);
    const int maxElems = *(const int*)(ch + 0x10), maxChunks = *(const int*)(ch + 0x18);
    w.num = *(const int*)(ch + 0x14);
    if (!w.chunks || w.num <= 0 || maxChunks <= 0) return false;
    w.perChunk = maxElems / maxChunks;
    return w.perChunk > 0;
}
// A NATIVE class object is one whose own class is the fixpoint; those are few, so naming them is cheap.
static void* FindNativeClass(const char* name, void* seed) {
    void* fix = ClassFixpoint(seed);
    ObjWalk w;
    if (!fix || !name || !ObjWalkBegin(w)) return nullptr;
    for (int i = 0; i < w.num; i++) {
        uint8_t* chunk = w.chunks[i / w.perChunk];
        if (!chunk) continue;
        uint8_t* obj = *(uint8_t**)(chunk + (size_t)(i % w.perChunk) * 24);
        if (!obj || *(void**)(obj + UOBJ_CLASS) != fix) continue;
        char nb[80];
        if (GrindPop_FNameToString(obj + UOBJ_NAME, nb, sizeof(nb)) && !strcmp(nb, name)) return obj;
    }
    return nullptr;
}
void* SitUI_LoadObject(const char* path) {
    if (!path || !path[0] || (!g_find && !g_load)) return nullptr;
    __try {
        wchar_t wp[200]; Widen(path, wp, 200);
        void* o = g_find ? g_find(nullptr, (void*)-1, wp, false) : nullptr;
        if (!o && g_load) o = g_load(nullptr, nullptr, wp, nullptr, 0, nullptr, true, nullptr);
        return o;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return nullptr; }
}
void* SitUI_FindInstanceOf(const char* nativeClassName, void* seed, void* exclude) {
    __try {
        void* want = FindNativeClass(nativeClassName, seed);
        ObjWalk w;
        if (!want || !ObjWalkBegin(w)) return nullptr;
        // Most objects share a handful of classes, so the answer per class is remembered: the walk up
        // the super chain then runs a few thousand times rather than once per object.
        struct { void* cls; int is; } memo[256] = {};
        for (int i = 0; i < w.num; i++) {
            uint8_t* chunk = w.chunks[i / w.perChunk];
            if (!chunk) continue;
            const uint8_t* item = chunk + (size_t)(i % w.perChunk) * 24;
            uint8_t* obj = *(uint8_t* const*)item;
            if (!obj || (*(const int32_t*)(item + ITEM_FLAGS) & ITEM_DEAD)) continue;
            void* cls = *(void**)(obj + UOBJ_CLASS);
            if (!cls) continue;
            auto& m = memo[((uintptr_t)cls >> 4) & 255];
            if (m.cls != cls) {
                m.cls = cls; m.is = 0;
                void* c = cls;
                for (int hop = 0; c && hop < 16; hop++) { if (c == want) { m.is = 1; break; } c = *(void**)((uint8_t*)c + USTRUCT_SUPER); }
            }
            if (!m.is || obj == exclude || (*(const uint32_t*)(obj + UOBJ_FLAGS) & RF_CDO_OR_ARCHETYPE)) continue;
            return obj;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    return nullptr;
}
int SitUI_LogInstancesOf(const char* nativeClassName, void* seed, int cap, const char* tag) {
    int found = 0;
    __try {
        void* want = FindNativeClass(nativeClassName, seed);
        ObjWalk w;
        if (!want || !ObjWalkBegin(w)) { TwkLog("[%s] no native class named %s", tag, nativeClassName); return 0; }
        char line[480]; int at = 0, n = 0;
        for (int i = 0; i < w.num; i++) {
            uint8_t* chunk = w.chunks[i / w.perChunk];
            if (!chunk) continue;
            uint8_t* obj = *(uint8_t**)(chunk + (size_t)(i % w.perChunk) * 24);
            if (!obj || *(void**)(obj + UOBJ_CLASS) != want) continue;
            found++;
            if (found > cap) continue;
            char nb[80];
            if (!GrindPop_FNameToString(obj + UOBJ_NAME, nb, sizeof(nb))) continue;
            if (n == 0) at = snprintf(line, sizeof(line), "[%s] %s: ", tag, nativeClassName);
            at += snprintf(line + at, sizeof(line) - (size_t)at, "%s%s", n ? ", " : "", nb);
            if (++n == 8 || at > 380) { TwkLog("%s", line); n = 0; at = 0; }
        }
        if (n) TwkLog("%s", line);
        TwkLog("[%s] %s: %d loaded%s", tag, nativeClassName, found, found > cap ? " (list capped)" : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    return found;
}
const char* SitUI_Status() { return g_status; }

void SitUI_ReadConfig(const char* buf) {
    g_on       = TwkIniInt(buf, "SitPromptEnabled", 1) ? 1 : 0;
    g_zOrder   = TwkIniInt(buf, "SitPromptZ", 500);
    g_holdRing = TwkIniInt(buf, "SitPromptHoldRing", 1) ? 1 : 0;
    g_gateLog  = TwkIniInt(buf, "SitPromptGateLog", 0) ? 1 : 0;
    TwkIniStr(buf, "SitPromptRowPath",  g_rowPath,  sizeof(g_rowPath),  "");
    TwkIniStr(buf, "SitPromptRowMatch", g_rowMatch, sizeof(g_rowMatch), "UIGamePadButton");
    g_barRight  = (float)TwkIniInt(buf, "SitBarRight", 72);
    g_barBottom = (float)TwkIniInt(buf, "SitBarBottom", 64);
    g_barGap    = (float)TwkIniInt(buf, "SitBarGap", 56);
    g_barW      = (float)TwkIniInt(buf, "SitBarW", 56);
    g_barH      = (float)TwkIniInt(buf, "SitBarH", 48);
}
void SitUI_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "SitPromptEnabled",  g_on);
    TwkIniSetInt(buf, cap, "SitPromptZ",        g_zOrder);
    TwkIniSetInt(buf, cap, "SitPromptHoldRing", g_holdRing);
    TwkIniSetInt(buf, cap, "SitPromptGateLog",  g_gateLog);
    TwkIniSetStr(buf, cap, "SitPromptRowPath",  g_rowPath);
    TwkIniSetStr(buf, cap, "SitPromptRowMatch", g_rowMatch);
    TwkIniSetInt(buf, cap, "SitBarRight",  (int)g_barRight);
    TwkIniSetInt(buf, cap, "SitBarBottom", (int)g_barBottom);
    TwkIniSetInt(buf, cap, "SitBarGap",    (int)g_barGap);
    TwkIniSetInt(buf, cap, "SitBarW",      (int)g_barW);
    TwkIniSetInt(buf, cap, "SitBarH",      (int)g_barH);
}

static FromNameFn ResolveFromName() {
    uint8_t* fn = TwkScanExe(SIG_TRICK_TEXT);
    if (!fn) return nullptr;
    __try {
        for (int o = 0; o < 0x200; o++) {
            if (fn[o] != 0xE8) continue;
            int32_t disp; memcpy(&disp, fn + o + 1, 4);
            uint8_t* tgt = fn + o + 5 + disp;
            if (memcmp(tgt, kFromNamePrologue, sizeof(kFromNamePrologue)) == 0) return (FromNameFn)tgt;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}

void SitUI_Install() {
    g_create     = (CreateFn)TwkScanExe(SIG_CREATE);
    g_addVp      = (AddViewportFn)TwkScanExe(SIG_ADD_VIEWPORT);
    g_setPos     = (SetPosFn)TwkScanExe(SIG_SET_POS);
    g_setAlign   = (SetVec2Fn)TwkScanExe(SIG_SET_ALIGN);
    g_setDesired = (SetVec2Fn)TwkScanExe(SIG_SET_DESIRED);
    g_setVis     = (SetVisFn)TwkScanExe(SIG_SET_VIS);
    g_btnText    = (BtnSetTextFn)TwkScanExe(SIG_BTN_SETTEXT);
    g_btnEnable  = (BtnSetEnabledFn)TwkScanExe(SIG_BTN_SETENABLED);
    g_vpSize     = (VpSizeFn)TwkScanExe(SIG_VP_SIZE);
    g_vpScale    = (VpScaleFn)TwkScanExe(SIG_VP_SCALE);
    g_findIcon   = (FindIconFn)TwkScanExe(SIG_FIND_ICON);
    g_ctrlType   = ResolveControllerType();
    g_find       = (StaticFindFn)TwkScanExe(SIG_STATIC_FIND);
    g_load       = (StaticLoadFn)TwkScanExe(SIG_STATIC_LOAD);
    g_fnameCtor  = (FNameCtorFn)TwkScanExe(SIG_FNAME_CTOR);
    g_fromName   = ResolveFromName();
    g_objArray   = ResolveObjArray();
    g_ok = g_create && g_addVp && g_setPos && g_setVis;
    if (!g_ok) {
        TwkLog("[situi] prompts unavailable: %s%s%s%s(game updated?)", g_create ? "" : "Create ",
               g_addVp ? "" : "AddToViewport ", g_setPos ? "" : "SetPosition ", g_setVis ? "" : "SetVisibility ");
        snprintf(g_status, sizeof(g_status), "unavailable this build");
        return;
    }
    snprintf(g_status, sizeof(g_status), "armed");
    TwkLog("[situi] armed: button prompts, lower right | text %s, game icon table %s, glyph fallback %s, object array %s | class %s",
           g_btnText && g_fnameCtor && g_fromName ? "ok" : "MISSING",
           g_ctrlType && g_findIcon ? "ok" : "MISSING", g_find ? "ok" : "MISSING", g_objArray ? "ok" : "MISSING",
           g_rowPath[0] ? g_rowPath : "(not learned yet)");
}
