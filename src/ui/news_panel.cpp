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
// =====================================================================================================
// SessionOpenMP -- the release notes in the game's OWN NEWS ARTICLE widget.
//
// OUR OWN INSTANCE OF THE WIDGET, NEVER THE NEWS SYSTEM. UNewsSystem fetches articles from Nacon's
// online feed, keeps a seen-state in the player's save, and -- the reason this line matters -- its
// flow also carries the EULA (UNewsEULAWidget / UIntroUI::HandleOnNewsEULAClosed). Injecting an
// article, or forcing the panel to stay up, would put a mod in front of a legal prompt and could hide
// a real announcement. So nothing here touches UNewsSystem: this constructs WBP_DefaultNewsContent
// the same way nacon_panel constructs the MyNacon message box, fills its text blocks and puts it on
// the viewport. The game's news is left entirely alone.
//
// WHY THIS WIDGET over the message box: its body sits in a SCROLL BOX (`_descScrollBlock`), so the
// notes are not limited to what one screenful holds -- which is what forced the line budget, the
// wrap-width arithmetic and the "stop three lines short" trimming on the other panel. It also has a
// title, a DATE and a fallback header image already authored, which is exactly the shape of a release.
//
// GAME THREAD ONLY, everything inside SEH, every failure a false return -- the caller falls back to
// the message panel, which is known to work.
// =====================================================================================================
#include "news_panel.h"
#include "version_tag.h"                 // the game instance, for creating the widget
#include "../game/game_syms.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace omp { namespace ui {

using namespace omp::game;

// The widget blueprint's GENERATED class. Found, never LOADED: loading a UI package on the game
// thread at an arbitrary moment is a worse risk than falling back to the other panel.
static const wchar_t* const kNewsClass =
    L"/Game/News/UI/WBP_DefaultNewsContent.WBP_DefaultNewsContent_C";
// Sub-widget names, read out of the extracted asset's own name table -- the blueprint's, not guesses.
static const char* const kTitleWidget = "_titleTextBlock";
static const char* const kDescWidget  = "_descTextBlock";
static const char* const kDateWidget  = "_dateTextBlock";
// Chrome that belongs to the real news flow and means nothing here: the "more info" prompt opens an
// article's external URL, the loading spinner is for a fetch in progress, and the pagination dots
// count articles we do not have.
static const char* const kHideWidgets[] = { "_moreInfoContainer", "_loadingContainer", "_paginationWidget" };

static void* g_panel = nullptr;
static bool  g_saidMissing = false;
static void* g_scrollBox = nullptr;      // the notes, for handing focus to while they are scrolled

// Where it goes, in viewport units. The blueprint authors its own size, so only the corner is set --
// clear of the MyNacon "Register now" banner at the top of the intro screen.
// Left edge lined up with the MyNacon banner above it, measured off the rendered screen.
float kNewsX = 74.0f, kNewsY = 260.0f;
// ...and how big. A user widget added to the viewport gets its DESIRED size, and a canvas-rooted
// widget's desired size can be zero -- which draws nothing at all however much text is in it. 0 here
// leaves the blueprint's own size alone; anything else forces a footprint.
float kNewsW = 760.0f, kNewsH = 600.0f;
// How tall the scrolling description is allowed to be. The blueprint wraps it in a SizeBox sized for
// one news article, so without this the notes scroll inside a strip at the top of an otherwise empty
// panel (field 2026-09-20).
// THIS ONE ON ITS OWN, not with kNewsH: growing the panel moves the box, and the box is where it
// should be -- what was wanted is the TEXT reaching the bottom of it. A line is about 20 units.
float kDescH = 610.0f;

struct TextBlob { void* data; void* refCtrl; unsigned flags; unsigned pad; };
struct FStr { wchar_t* d; int n; int max; };

// FText::FromString when it resolved (no length limit), else the FName route and its NAME_SIZE 1024.
// Same rule and the same reason as nacon_panel's makeText -- see game_syms.cpp for why FromString has
// to be decoded from a call site rather than signatured.
static bool makeText(const char* ascii, TextBlob* out) {
    const Syms& S = Get();
    if (!ascii || !out) return false;
    if (S.TextFromString && S.MemMalloc && S.MemFree) {
        const int len = (int)strlen(ascii);
        memset(out, 0, sizeof(*out));
        __try {
            wchar_t* buf = (wchar_t*)S.MemMalloc((size_t)(len + 1) * sizeof(wchar_t), 0);
            if (buf) {
                for (int k = 0; k < len; k++) buf[k] = (wchar_t)(unsigned char)ascii[k];
                buf[len] = 0;
                FStr s{ buf, len + 1, len + 1 };
                S.TextFromString(out, &s);
                S.MemFree(buf);                       // the overload we resolve to COPIES
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (out->data) return true;
    }
    if (!S.TextFromName || !S.FNameCtor) return false;
    memset(out, 0, sizeof(*out));
    unsigned long long fname[2] = { 0, 0 };
    __try {
        S.FNameCtor(fname, ascii, 1 /* FNAME_Add */);
        S.TextFromName(out, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return out->data != nullptr;
}

static void* findWidget(void* widget, const char* name) {
    const Syms& S = Get();
    if (!S.WidgetTreeFind || !S.FNameCtor || !widget) return nullptr;
    void* tree = nullptr;
    __try { tree = *(void**)((uint8_t*)widget + off::kUserWidgetTree); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!tree) return nullptr;
    unsigned long long fname[2] = { 0, 0 };
    __try {
        S.FNameCtor(fname, name, 1 /* FNAME_Add */);
        return S.WidgetTreeFind(tree, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool setText(void* widget, const char* name, const char* text) {
    const Syms& S = Get();
    void* found = findWidget(widget, name);
    if (!found || !S.TextBlockSetText) return false;
    TextBlob ft;
    if (!makeText(text, &ft)) return false;
    __try { S.TextBlockSetText(found, &ft); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static void hide(void* widget, const char* name) {
    const Syms& S = Get();
    void* found = findWidget(widget, name);
    if (found && S.WidgetSetVisible)
        __try { S.WidgetSetVisible(found, 1 /* Collapsed */); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// ESlateVisibility: 0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4 SelfHitTestInvisible.
static void show(void* widget, const char* name) {
    const Syms& S = Get();
    void* found = name ? findWidget(widget, name) : widget;
    if (found && S.WidgetSetVisible)
        __try { S.WidgetSetVisible(found, 0 /* Visible: the scroll box needs hit-testing */); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// A widget's class name, for walking a parent chain and recognising what is in it.
static bool classNameOf(void* obj, char* out, int cap) {
    const Syms& S = Get();
    out[0] = 0;
    if (!obj || !S.FNameToString) return false;
    __try {
        void* cls = *(void**)((uint8_t*)obj + 0x10);          // UObjectBase::ClassPrivate
        if (!cls) return false;
        struct FStrOut { wchar_t* d; int n; int max; } fs{};
        S.FNameToString((const uint8_t*)cls + 0x18, &fs);     // the UClass's own FName
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// THE DESCRIPTION'S HEIGHT. The scroll box does its job -- the notes scroll -- but the blueprint caps
// how tall it may be, because a news article is a paragraph and this is a changelog. The cap lives on
// a SizeBox somewhere ABOVE the text in the tree, so the chain is walked (UWidget::Slot ->
// UPanelSlot::Parent, the same route the other panel uses to reach its ScaleBox) until one turns up.
// Both the height override and the max are raised: either alone can still be clamped by the other.
static void growDescription(void* w, void (*logf)(const char*)) {
    const Syms& S = Get();
    if (!S.SizeBoxSetHeight && !S.SizeBoxSetMaxHeight) return;
    void* node = findWidget(w, kDescWidget);
    char chain[320]; int at = snprintf(chain, sizeof(chain), "[news] desc chain:");
    bool grew = false;
    for (int hop = 0; node && hop < 8; hop++) {
        char cn[64] = "?";
        classNameOf(node, cn, sizeof(cn));
        if (at < (int)sizeof(chain) - 40) at += snprintf(chain + at, sizeof(chain) - (size_t)at, " %s", cn);
        if (!grew && strstr(cn, "SizeBox")) {
            __try {
                if (S.SizeBoxSetMaxHeight) S.SizeBoxSetMaxHeight(node, kDescH);
                if (S.SizeBoxSetHeight)    S.SizeBoxSetHeight(node, kDescH);
                grew = true;
                if (at < (int)sizeof(chain) - 24) at += snprintf(chain + at, sizeof(chain) - (size_t)at, " <- grown");
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        void* slot = nullptr;
        __try { slot = *(void**)((uint8_t*)node + off::kWidgetSlot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!slot) break;
        __try { node = *(void**)((uint8_t*)slot + off::kSlotParent); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
    if (!grew && at < (int)sizeof(chain) - 32)
        snprintf(chain + at, sizeof(chain) - (size_t)at, " -- NO SizeBox found");
    if (logf) logf(chain);
}

static void* findClass() {
    const Syms& S = Get();
    if (!S.StaticFindObject) return nullptr;
    __try { return S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kNewsClass, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool NewsPanel_Available() {
    const Syms& S = Get();
    return S.WidgetCreate && S.WidgetAddToViewport && S.WidgetTreeFind && S.TextBlockSetText &&
           S.WidgetRemoveParent && findClass() != nullptr;
}

bool NewsPanel_Showing() { return g_panel != nullptr; }

// SCROLLING IS THE SCROLL BOX'S OWN. It answers the stick and the d-pad itself, and it does that
// well -- which is why nothing here drives it. An attempt to move it from outside cost a long run of
// builds and never once beat what the widget does unaided (2026-09-20): writing Slate's internals did
// nothing, re-writing the text a line at a time was jerky and left the scroll bar describing a window
// rather than the notes, and pushing an offset in every frame fought the widget's own easing -- while
// reading its value back multiplied the position by ~0.937 a frame and slid it to the top, which read
// as the box fighting us. It was not. Leave it alone.
//
// ---- WHO HOLDS FOCUS --------------------------------------------------------------------------------
// The one real problem: a scroll box answers the stick only while it HAS keyboard focus, and whatever
// holds focus swallows the menu's X, so after scrolling the notes "Skate" did nothing until the window
// was alt-tabbed. Taking focus away from the panel altogether fixes X and kills scrolling; leaving it
// fixes scrolling and kills X. It cannot be one or the other -- so it is HANDED BETWEEN them, on the
// only signal that distinguishes the two: whether the player is moving the stick right now.
// The pad is read through UPlayerInput, not XInput, because this game is played on a DualSense and
// XInput never sees one (the same reason SessionTweaks reads its trigger that way).
static void* localController() {
    void* gi = VersionTag_GameInstance();
    if (!gi) return nullptr;
    __try {
        struct TArr { void** data; int num, max; };
        const TArr* lp = (const TArr*)((const uint8_t*)gi + off::kGiLocalPlayers);
        if (!lp->data || lp->num <= 0) return nullptr;
        void* player = lp->data[0];
        if (!player) return nullptr;
        return *(void**)((uint8_t*)player + off::kPlayerController);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// A key's value by NAME. The FKey is 24 bytes and the map looks it up by name alone, so one with null
// details is a whole key. Names are FOUND, never added -- they all exist already.
static float keyValue(void* pi, const char* name, unsigned long long* cache) {
    const Syms& S = Get();
    if (!pi || !S.PlayerInputKeyValue || !S.FNameCtor) return 0.0f;
    __try {
        if (!*cache) {
            unsigned long long nm[2] = { 0, 0 };
            S.FNameCtor(nm, name, 0 /* FNAME_Find */);
            if (!nm[0]) return 0.0f;
            *cache = nm[0];
        }
        unsigned long long fkey[3] = { *cache, 0, 0 };
        const float v = S.PlayerInputKeyValue(pi, fkey);
        return (v > -1.5f && v < 1.5f) ? v : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}
// Is the player working the notes right now? The left stick or the d-pad, which is what they scroll with.
static bool scrolling() {
    void* pc = localController();
    if (!pc) return false;
    void* pi = nullptr;
    __try { pi = *(void**)((uint8_t*)pc + off::kPcPlayerInput); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!pi) return false;
    static unsigned long long kY = 0, kUp = 0, kDown = 0;
    const float y = keyValue(pi, "Gamepad_LeftY", &kY);
    if (y > 0.25f || y < -0.25f) return true;               // dead zone: a resting stick is not scrolling
    return keyValue(pi, "Gamepad_DPad_Up", &kUp) > 0.5f || keyValue(pi, "Gamepad_DPad_Down", &kDown) > 0.5f;
}
static void focusWidget(void* w) {
    const Syms& S = Get();
    if (!w || !S.WidgetSetFocus) return;
    __try { S.WidgetSetFocus(w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// The intro screen itself -- what X belongs to, and where focus goes back to.
static void* introUi() {
    void* pc = localController();
    if (!pc) return nullptr;
    __try { return *(void**)((uint8_t*)pc + off::kPcIntroUI); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// How long after the stick stops before X works again. Short enough not to be noticed, long enough
// that easing off the stick between flicks does not hand focus back and forth.
unsigned kFocusReturnMs = 200;

void NewsPanel_Tick() {
    if (!g_panel || !g_scrollBox) return;
    static bool held = false;                               // do WE have focus on the notes?
    static uint64_t idleSince = 0;
    const uint64_t now = GetTickCount64();
    if (scrolling()) {
        idleSince = 0;
        if (!held) { focusWidget(g_scrollBox); held = true; }
        return;
    }
    if (!held) return;
    if (!idleSince) { idleSince = now; return; }
    if (now - idleSince < kFocusReturnMs) return;
    focusWidget(introUi());                                 // X is the menu's again
    held = false;
    idleSince = 0;
}

void NewsPanel_Close() {
    const Syms& S = Get();
    if (!g_panel) return;
    void* p = g_panel;
    g_panel = nullptr;
    g_scrollBox = nullptr;                    // cleared FIRST: a fault below must not leave us think it is up
    if (!S.WidgetRemoveParent) return;
    __try { S.WidgetRemoveParent(p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool NewsPanel_Show(const char* title, const char* body, const char* date, void (*logf)(const char*)) {
    const Syms& S = Get();
    if (!title || !body) return false;
    if (!S.WidgetCreate || !S.WidgetAddToViewport || !S.WidgetTreeFind || !S.TextBlockSetText) {
        if (logf && !g_saidMissing) { g_saidMissing = true; logf("[news] the widget calls are missing this build"); }
        return false;
    }
    void* cls = findClass();
    if (!cls) {
        if (logf && !g_saidMissing) {
            g_saidMissing = true;
            logf("[news] WBP_DefaultNewsContent_C is not loaded here -- using the message panel instead");
        }
        return false;
    }
    NewsPanel_Close();                    // one at a time

    void* gi = VersionTag_GameInstance();
    if (!gi) { if (logf) logf("[news] no game instance yet"); return false; }
    void* w = nullptr;
    __try { w = S.WidgetCreate(gi, cls, nullptr /* owning player: the first local one */); }
    __except (EXCEPTION_EXECUTE_HANDLER) { w = nullptr; }
    if (!w) { if (logf) logf("[news] the widget could not be created"); return false; }

    // THE BODY FIRST, and it is the one that decides whether this panel is usable at all: if the
    // description block cannot be written the widget is no better than an empty box, so drop it and
    // let the caller fall back rather than showing a blank article.
    if (!setText(w, kDescWidget, body)) {
        if (logf) logf("[news] could not write _descTextBlock -- falling back to the message panel");
        __try { if (S.WidgetRemoveParent) S.WidgetRemoveParent(w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }
    setText(w, kTitleWidget, title);                       // nice to have, not worth failing over
    if (date && *date) setText(w, kDateWidget, date);
    else               hide(w, kDateWidget);
    for (int i = 0; i < (int)(sizeof(kHideWidgets) / sizeof(kHideWidgets[0])); i++)
        hide(w, kHideWidgets[i]);
    // ...and REVEAL the parts that carry the article. The news system fills this widget and then
    // shows it, so what it hands a caller who builds it directly can be collapsed -- writing the text
    // is not enough on its own.
    show(w, nullptr);                                      // the root
    show(w, "NewsContentContainer");
    show(w, "TitleContainer");
    show(w, kDescWidget);
    show(w, "_descScrollBlock");
    show(w, kTitleWidget);
    growDescription(w, logf);
    g_scrollBox = findWidget(w, "_descScrollBlock");
    // FOCUS STAYS WITH THE MENU until the player actually reaches for the notes. Showing the panel
    // must not cost them the X they were about to press.
    focusWidget(introUi());


    __try { S.WidgetAddToViewport(w, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { if (logf) logf("[news] adding it to the viewport faulted"); return false; }
    if (S.WidgetSetAlignInVp && S.WidgetSetPosInVp) {
        auto v2 = [](float x, float y) {
            unsigned long long p; const float f[2] = { x, y }; memcpy(&p, f, 8); return p;
        };
        __try {
            if (kNewsW > 0.0f && S.WidgetSetSizeInVp) S.WidgetSetSizeInVp(w, v2(kNewsW, kNewsH));
            S.WidgetSetAlignInVp(w, v2(0.0f, 0.0f));       // position means the TOP-LEFT corner
            S.WidgetSetPosInVp  (w, v2(kNewsX, kNewsY), false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_panel = w;
    if (logf) logf("[news] shown in the game's own news article widget");
    return true;
}

} }  // namespace omp::ui
