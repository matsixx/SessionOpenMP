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
#include "nacon_panel.h"
#include "version_tag.h"
#include "../game/game_syms.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace omp { namespace ui {

using namespace omp::game;

// The widget blueprint's GENERATED CLASS. A blueprint asset `X` compiles to a class object named
// `X_C` inside the same package, and it is the CLASS the widget factory wants, not the asset.
static const wchar_t* const kPanelClass =
    L"/Game/MyNacon/UI/PBP_MyNacon_ErrorMessaging.PBP_MyNacon_ErrorMessaging_C";
// The two widgets inside it we write to. Names read out of the extracted asset's own name table, so
// they are the blueprint's, not a guess.
static const char* const kTextWidget   = "_textBlock";
static const char* const kButtonWidget = "_continueButton";
static const char* const kBgWidget     = "BgImage";

static void*  g_panel = nullptr;     // ours, while it is on screen
static bool   g_saidMissing = false;

// WHERE AND HOW BIG, in viewport units (the widget layer is DPI-scaled, so these are not raw pixels --
// they behave like a 1080p-ish canvas and hold their proportions on other resolutions). Top-leftish
// with a comfortable margin, and wide: the notes are lines of prose, and every extra 100 units of width
// removes a wrapped line. Live so they can be nudged without a rebuild.
float kPanelX = 120.0f, kPanelY = 110.0f;      // top-left corner of the panel
float kPanelW = 0.0f,   kPanelH = 0.0f;        // 0 = leave the blueprint's own size alone
// The DARK BOX and the text inside it, set through their own slots (see sizeWidget). The blueprint
// sizes both for a single short error line; a page of release notes needs them bigger, and the text is
// inset so it wraps inside the box instead of running over its edge.
float kBoxW   = 680.0f, kBoxH = 620.0f;        // the background panel
float kTextPad = 26.0f;                        // margin between the box edge and the words
// WHERE THE BOX SITS, in the panel's own canvas, and it is ON TOP OF kPanelX/kPanelY -- the widget's
// viewport slot puts the panel somewhere and this offsets the box within it, so the two ADD. 250 here
// therefore landed at 360 and pushed the notes into the SESSION logo. Far enough down to clear the
// MyNacon "Register now" banner (which ends around 240 units) and no further.
float kBoxX = 0.0f, kBoxY = 150.0f;
// DARKER, STILL TRANSPARENT. The background is the blueprint's artwork multiplied by its
// ColorAndOpacity, and the first attempt at this wrote a flat { 0.35, 0.35, 0.35, 1 } -- which threw
// away whatever alpha the blueprint had and turned a black translucent panel into an opaque grey one.
// So the existing colour is READ and only adjusted: RGB scaled toward black, alpha nudged up a little
// for coverage but never past 1, and the before/after is logged so it is tuned from what is actually
// there rather than from a guess.
float kBoxTintRGB   = 0.55f;   // < 1 darkens the artwork
float kBoxAlphaMul  = 1.25f;   // > 1 makes it cover more; the authored alpha is the starting point

// An FText from plain ASCII, via FName -- the same route and the same reason as trx_popup's makeText:
// FromString comes in const-ref and rvalue-ref twins no signature can tell apart, and picking wrong
// makes the engine steal or double-free the buffer. An FName argument is 8 POD bytes with no
// ownership question. NOTE its limit: FName is NAME_SIZE (1024), and overrunning it renders the words
// ERROR_NAME_SIZE_EXCEEDED instead of the text (field 2026-09-20). Callers keep well under.
struct TextBlob { void* data; void* refCtrl; unsigned flags; unsigned pad; };
// The game's FString: a TArray<TCHAR> whose Num INCLUDES the null terminator.
struct FStr { wchar_t* d; int n; int max; };

// PREFERRED: FText::FromString, which has NO length limit. The FName route below dies at NAME_SIZE
// (1024) and renders the words ERROR_NAME_SIZE_EXCEEDED instead of the text, which is what kept the
// release notes to about two thirds of the panel.
// The overload this resolves to COPIES the characters (read from its disassembly, see game_syms.cpp),
// so the buffer is ours to free the moment the call returns.
static bool makeTextLong(const char* ascii, TextBlob* out) {
    const Syms& S = Get();
    if (!S.TextFromString || !S.MemMalloc || !S.MemFree || !ascii || !out) return false;
    const int len = (int)strlen(ascii);
    memset(out, 0, sizeof(*out));
    __try {
        wchar_t* buf = (wchar_t*)S.MemMalloc((size_t)(len + 1) * sizeof(wchar_t), 0);
        if (!buf) return false;
        for (int k = 0; k < len; k++) buf[k] = (wchar_t)(unsigned char)ascii[k];
        buf[len] = 0;
        FStr s{ buf, len + 1, len + 1 };             // Num counts the terminator
        S.TextFromString(out, &s);
        S.MemFree(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return out->data != nullptr;
}

// An FText from plain ASCII. FromString when it resolved; otherwise the FName route, with its limit.
static bool makeText(const char* ascii, TextBlob* out) {
    if (makeTextLong(ascii, out)) return true;
    const Syms& S = Get();
    if (!S.TextFromName || !S.FNameCtor || !ascii || !out) return false;
    memset(out, 0, sizeof(*out));
    unsigned long long fname[2] = { 0, 0 };
    __try {
        S.FNameCtor(fname, ascii, 1 /* FNAME_Add */);
        S.TextFromName(out, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return out->data != nullptr;
}
// Is the long route available? The caller's page budget depends on it.
bool NaconPanel_LongTextOk() {
    const Syms& S = Get();
    return S.TextFromString && S.MemMalloc && S.MemFree;
}

static void* findClass() {
    const Syms& S = Get();
    if (!S.StaticFindObject) return nullptr;
    // ANY_PACKAGE (-1), the same shape every other lookup in the mod uses. The class is LOADED rather
    // than looked up: the MyNacon flow runs at start-up, so its widgets are already resident. If that
    // ever stops being true this returns null and the caller falls back -- it does not load it, because
    // loading a UI package on the game thread at an arbitrary moment is a worse risk than no panel.
    __try { return S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kPanelClass, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool NaconPanel_Available() {
    const Syms& S = Get();
    return S.WidgetCreate && S.WidgetAddToViewport && S.WidgetTreeFind && S.TextBlockSetText &&
           S.WidgetRemoveParent && S.FNameCtor && S.TextFromName && findClass() != nullptr;
}

bool NaconPanel_Showing() { return g_panel != nullptr; }

void NaconPanel_Close() {
    const Syms& S = Get();
    if (!g_panel) return;
    void* p = g_panel;
    g_panel = nullptr;                       // cleared FIRST: a fault below must not leave us think it is up
    if (!S.WidgetRemoveParent) return;
    __try { S.WidgetRemoveParent(p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Set one TextBlock inside the panel, by the blueprint's own widget name.
static bool setWidgetText(void* widget, const char* widgetName, const char* text) {
    const Syms& S = Get();
    void* tree = nullptr;
    __try { tree = *(void**)((uint8_t*)widget + off::kUserWidgetTree); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!tree) return false;
    unsigned long long fname[2] = { 0, 0 };
    void* found = nullptr;
    __try {
        S.FNameCtor(fname, widgetName, 1 /* FNAME_Add */);
        found = S.WidgetTreeFind(tree, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!found) return false;
    TextBlob ft;
    if (!makeText(text, &ft)) return false;
    __try { S.TextBlockSetText(found, &ft); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// Find a named widget inside the panel and collapse it -- gone, and taking no layout space with it.
static void hideWidget(void* widget, const char* widgetName) {
    const Syms& S = Get();
    if (!S.WidgetSetVisible || !S.WidgetTreeFind || !S.FNameCtor) return;
    void* tree = nullptr;
    __try { tree = *(void**)((uint8_t*)widget + off::kUserWidgetTree); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!tree) return;
    unsigned long long fname[2] = { 0, 0 };
    __try {
        S.FNameCtor(fname, widgetName, 1 /* FNAME_Add */);
        void* found = S.WidgetTreeFind(tree, fname);
        if (found) S.WidgetSetVisible(found, 1 /* ESlateVisibility::Collapsed */);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Resize a named widget through ITS OWN canvas slot. The panel's background and text block are laid
// out by the blueprint at a fixed size, so resizing our widget does nothing to them -- the dark box
// stayed small while the text ran out of the bottom. This reaches the slot (UWidget::Slot) and sets the
// size there, which is the only thing the blueprint's layout answers to.
static void* findWidget(void* widget, const char* widgetName) {
    const Syms& S = Get();
    if (!S.WidgetTreeFind || !S.FNameCtor) return nullptr;
    void* tree = nullptr;
    __try { tree = *(void**)((uint8_t*)widget + off::kUserWidgetTree); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!tree) return nullptr;
    unsigned long long fname[2] = { 0, 0 };
    __try {
        S.FNameCtor(fname, widgetName, 1 /* FNAME_Add */);
        return S.WidgetTreeFind(tree, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static void placeSlotOf(void* found, float x, float y, float w, float h) {
    const Syms& S = Get();
    if (!found || !S.SlotSetSize || !S.SlotSetAnchors || !S.SlotSetPosition) return;
    __try {
        void* slot = *(void**)((uint8_t*)found + off::kWidgetSlot);
        if (!slot) return;
        const float topLeftPoint[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        S.SlotSetAnchors(slot, topLeftPoint);
        unsigned long long pos; const float p2[2] = { x, y }; memcpy(&pos, p2, 8);
        unsigned long long size; const float s2[2] = { w, h }; memcpy(&size, s2, 8);
        S.SlotSetPosition(slot, pos);
        S.SlotSetSize(slot, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void placeWidget(void* widget, const char* widgetName, float x, float y, float w, float h) {
    const Syms& S = Get();
    if (!S.SlotSetSize || !S.SlotSetAnchors || !S.SlotSetPosition) return;
    void* found = findWidget(widget, widgetName);
    if (!found) return;
    __try {
        void* slot = *(void**)((uint8_t*)found + off::kWidgetSlot);
        if (!slot) return;
        // ANCHORS FIRST, and this is the whole trick. These widgets are anchored to STRETCH across
        // their parent, and for a stretched anchor a slot's offsets are MARGINS FROM THE PARENT'S
        // EDGES -- so writing "size 760x620" set the right and bottom margins to 760 and 620 and
        // produced a tiny box in the corner (field 2026-09-20: "just getting a small box at top
        // left"). Collapsing the anchor to a POINT at the top-left makes the offsets mean position
        // and size, which is what SetPosition/SetSize are then free to say.
        const float topLeftPoint[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        S.SlotSetAnchors(slot, topLeftPoint);
        unsigned long long pos; const float p2[2] = { x, y }; memcpy(&pos, p2, 8);
        unsigned long long size; const float s2[2] = { w, h }; memcpy(&size, s2, 8);
        S.SlotSetPosition(slot, pos);
        S.SlotSetSize(slot, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// WHAT THIS WIDGET ACTUALLY IS, read off the live object. Four rounds of reasoning from screenshots
// each produced a plausible story and none of them was right, so this stops inferring: the object's
// own class, its slot's class, and the wrap values as they stand AFTER everything has been applied.
// UObjectBase layout: +0x10 ClassPrivate, and a UClass is a UObject whose FName is at +0x18.
static bool classNameOf(void* obj, char* out, int cap) {
    const Syms& S = Get();
    out[0] = 0;
    if (!obj || !S.FNameToString) return false;
    __try {
        void* cls = *(void**)((uint8_t*)obj + 0x10);          // UObjectBase::ClassPrivate
        if (!cls) return false;
        const void* fn = (const uint8_t*)cls + 0x18;          // UObjectBase::NamePrivate on the UClass
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fn, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
static void describe(void* obj, const char* label, void (*logf)(const char*)) {
    if (!logf) return;
    if (!obj) { char m[96]; snprintf(m, sizeof(m), "[panel] %s: NOT FOUND", label); logf(m); return; }
    char cname[96] = "?", sname[96] = "(no slot)";
    classNameOf(obj, cname, sizeof(cname));
    __try {
        void* slot = *(void**)((uint8_t*)obj + off::kWidgetSlot);
        if (slot) classNameOf(slot, sname, sizeof(sname));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    char m[320];
    snprintf(m, sizeof(m), "[panel] %s: class=%s slot=%s", label, cname, sname);
    logf(m);
}

bool NaconPanel_Show(const char* text, const char* buttonText, void (*logf)(const char*)) {
    const Syms& S = Get();
    if (!text) return false;
    if (!S.WidgetCreate || !S.WidgetAddToViewport || !S.WidgetTreeFind || !S.TextBlockSetText) {
        if (logf && !g_saidMissing) {
            g_saidMissing = true;
            logf("[panel] the game's widget calls were not found in this build -- falling back to the dialog");
        }
        return false;
    }
    void* cls = findClass();
    if (!cls) {
        if (logf && !g_saidMissing) {
            g_saidMissing = true;
            logf("[panel] PBP_MyNacon_ErrorMessaging_C is not loaded here -- falling back to the dialog");
        }
        return false;
    }
    // One at a time: a second panel over the first would leave the first orphaned on the viewport.
    NaconPanel_Close();

    void* gi = VersionTag_GameInstance();     // the world context. The menu has no pawn to ask.
    void* w = nullptr;
    __try { w = S.WidgetCreate(gi, cls, nullptr /* owning player: the first local one */); }
    __except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
    if (!w) {
        if (logf) logf("[panel] the widget could not be created");
        return false;
    }
    // The text FIRST, then the viewport: a panel must never appear holding whatever the blueprint's
    // default text was (which, for this one, is a Nacon error string).
    if (!setWidgetText(w, kTextWidget, text)) {
        if (logf) logf("[panel] could not write the text into _textBlock -- not showing it");
        __try { if (S.WidgetRemoveParent) S.WidgetRemoveParent(w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }
    // THE CONTINUE BUTTON GOES. There is nothing to continue TO: the notes are part of the start menu
    // now, not a box to dismiss -- they go up with it and come down with it, and no button is involved.
    // (It also belongs to the Nacon flow we do not run, and the blueprint anchors it to ITS layout, so
    // once the panel is resized it lands in the middle of the text.) Collapsed, not hidden, so it takes
    // no layout space either.
    hideWidget(w, kButtonWidget);
    (void)buttonText;

    // WIDER, AND THE BOX AROUND THE WORDS. The background and the text block are sized by the blueprint
    // for one short error line; a page of notes needs both made bigger, through their slots. The text
    // block is given the inner size (the background minus a margin) so its AutoWrapText wraps to the
    // box rather than past it.
    // ONLY THE BACKGROUND is placed through a canvas slot. The asset carries BOTH a CanvasPanelSlot
    // and a ScaleBoxSlot, and a UCanvasPanelSlot call aimed at a ScaleBoxSlot writes canvas offsets
    // into a different struct entirely -- so the text block, whose parent is not known to be a canvas,
    // is left to the blueprint's own layout and simply told to wrap.
    // THE SCALE BOX FIRST, and this is why every width set so far did nothing visible. The panel's
    // content sits in a ScaleBox, which LAYS OUT at natural size and then SCALES the result -- so the
    // text was wrapping at the width asked for and then being shrunk, landing at roughly 0.57 of it
    // while the background (a sibling in the canvas, unscaled) stayed full size. The two were simply
    // in different coordinate spaces. EStretch::None renders content 1:1, after which a width set
    // anywhere means what it says.
    // THE SCALE BOX, REACHED THROUGH THE TEXT'S OWN SLOT.
    // Measured, not assumed (the diagnostic below prints all of this): _textBlock's slot is a
    // ScaleBoxSlot, so it sits in a ScaleBox -- which lays its content out at natural size and then
    // SCALES the result. That is why every width set on the text was honoured and then shrunk, landing
    // at ~0.57 of it while BgImage, a CanvasPanelSlot sibling, stayed full size: the two were in
    // different coordinate spaces.
    // Searching for a widget NAMED "ScaleBox" found nothing -- that is a CLASS name, and the instance
    // is called something else -- so the previous attempt at this silently did nothing. The slot knows
    // its own container: UPanelSlot::Parent IS the ScaleBox, whatever it happens to be called.
    if (S.ScaleBoxSetStretch) {
        void* tb0 = findWidget(w, kTextWidget);
        __try {
            void* slot = tb0 ? *(void**)((uint8_t*)tb0 + off::kWidgetSlot) : nullptr;
            void* box  = slot ? *(void**)((uint8_t*)slot + off::kSlotParent) : nullptr;
            if (box) {
                S.ScaleBoxSetStretch(box, 0 /* EStretch::None */);
                // ...AND SIZE THE BOX ITSELF. Unscaling it was not enough: measured, the ScaleBox was
                // found and set to 1:1 and the text STILL wrapped narrow, because a text block wraps to
                // whichever is smaller -- its own WrapTextAt, or the container holding it. The blueprint
                // sizes this container for one short line. Sizing BgImage never touched it: they are
                // siblings, not parent and child.
                placeSlotOf(box, kBoxX + kTextPad, kBoxY + kTextPad,
                            kBoxW - kTextPad * 2.0f, kBoxH - kTextPad * 2.0f);
                if (logf) logf("[panel] the text's ScaleBox was unscaled AND sized to the box");
            } else if (logf) logf("[panel] the text has no panel parent -- nothing to unscale");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    placeWidget(w, kBgWidget, kBoxX, kBoxY, kBoxW, kBoxH);
    // ...and darken it, so a page of small text reads against whatever the menu is showing behind it.
    if (S.ImageSetColor) {
        void* bg = findWidget(w, kBgWidget);
        if (bg) __try {
            const float* cur = (const float*)((const uint8_t*)bg + off::kImageColor);
            float tint[4] = { cur[0] * kBoxTintRGB, cur[1] * kBoxTintRGB, cur[2] * kBoxTintRGB,
                              cur[3] * kBoxAlphaMul };
            if (tint[3] > 1.0f) tint[3] = 1.0f;
            if (logf) {
                char m[192];
                snprintf(m, sizeof(m), "[panel] BgImage tint %.2f %.2f %.2f a=%.2f -> %.2f %.2f %.2f a=%.2f",
                         cur[0], cur[1], cur[2], cur[3], tint[0], tint[1], tint[2], tint[3]);
                logf(m);
            }
            S.ImageSetColor(bg, tint);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    {
        void* tb = findWidget(w, kTextWidget);
        if (tb) __try {
            // WrapTextAt is a HARD wrap width and beats the widget's own width whenever it is
            // non-zero -- the blueprint sets it for a short error line, which is why the notes kept
            // wrapping to half the box (field 2026-09-20: "text still same"). Set to the box's inner
            // width so the words use the panel they are sitting in.
            *(float*)((uint8_t*)tb + off::kTextWrapAt) = kBoxW - kTextPad * 2.0f;
            *(unsigned char*)((uint8_t*)tb + off::kTextAutoWrap) = 1;
            if (S.TextSetAutoWrap) S.TextSetAutoWrap(tb, 1);
            // AND PUSH IT TO SLATE. The property alone changes nothing once the widget exists: the
            // live STextBlock (MyTextBlock, +0x298) was built when the panel was created and keeps its
            // own copy. SetAutoWrapText works precisely because it forwards; WrapTextAt has no setter
            // to forward for it, so SynchronizeProperties is what re-reads the whole lot into slate.
            if (S.TextSyncProps) S.TextSyncProps(tb);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (tb) { describe(tb, "_textBlock", logf);
                  __try { char m[160];
                      snprintf(m, sizeof(m), "[panel] _textBlock wrapAt=%.1f autoWrap=%d (asked for %.1f)",
                               *(float*)((uint8_t*)tb + off::kTextWrapAt),
                               (int)*(unsigned char*)((uint8_t*)tb + off::kTextAutoWrap),
                               kBoxW - kTextPad * 2.0f);
                      if (logf) logf(m);
                  } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        describe(findWidget(w, kBgWidget), "BgImage", logf);
        describe(findWidget(w, "ScaleBox"), "ScaleBox", logf);
        describe(findWidget(w, "windowpanel"), "windowpanel", logf);
    }

    __try { S.WidgetAddToViewport(w, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (logf) logf("[panel] adding it to the viewport faulted");
        return false;
    }
    // ---- WHERE IT SITS. AddToViewport alone stretches a widget across the whole screen, which is why
    // the panel came up centred with the text running off the bottom of its own background. Giving it a
    // DESIRED SIZE takes it out of fill mode, and then position + alignment place it: alignment (0,0)
    // means the position is its TOP-LEFT corner, so the offsets below are a margin from the screen edge
    // rather than the middle of anything. Sized generously wide -- the notes are lines of text, and a
    // wide box needs far fewer of them than the narrow default did.
    if (S.WidgetSetSizeInVp && S.WidgetSetAlignInVp && S.WidgetSetPosInVp) {
        auto v2 = [](float x, float y) {
            unsigned long long p; const float f[2] = { x, y }; memcpy(&p, f, 8); return p;
        };
        __try {
            // NO DESIRED SIZE. Forcing one resizes OUR widget but not the blueprint's insides: BgImage
            // is a fixed-size image anchored by the blueprint, so the dark box stayed small while the
            // text ran out of the bottom of it (field 2026-09-20). The panel keeps its authored size
            // and looks native; the TEXT is fitted to the panel instead, which is the half we own.
            if (kPanelW > 0.0f) S.WidgetSetSizeInVp(w, v2(kPanelW, kPanelH));
            S.WidgetSetAlignInVp(w, v2(0.0f, 0.0f));          // position means the TOP-LEFT corner
            S.WidgetSetPosInVp  (w, v2(kPanelX, kPanelY), false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (logf) logf("[panel] placing it faulted -- it is on screen, wherever the game put it");
        }
    }
    g_panel = w;
    if (logf) logf("[panel] shown in the game's own message panel");
    return true;
}

} }  // namespace omp::ui
