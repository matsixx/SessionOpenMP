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
// SessionOpenMP -- our text on screen, drawn by the game. Contract and thread rule: game_hud.h.
#include "game_hud.h"
#include "nameplates.h"
#include "chat.h"                        // the chat is this surface's other user
#include "version_tag.h"                  // the game instance, for creating a widget
#include "../omp_peers.h"                 // OMP_MAX_PEERS -- how many plates there can ever be
#include "../game/game_syms.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace omp { namespace ui {

using namespace omp::game;

// =====================================================================================================
// THE WIDGET. PBP_TrickDisplayTextWidget is the game's own floating trick text: ONE UTextBlock in the
// menu font (Arial_Unicode_MS_Bold), and nothing else in the asset -- no panel, no image, no animation.
// That is exactly a nameplate, which is why it is this one and not a dialog with chrome to hide.
//
// Its class is FOUND, not loaded: the trick readout is part of the skating HUD, so by the time there
// is anybody to put a name over, this is already resident. A bare-name lookup is tried as well because
// StaticFindObject(ANY_PACKAGE) is how every other name in this mod is resolved.
// =====================================================================================================
static const wchar_t* const kTextClassPath =
    L"/Game/Character/UI/PBP_TrickDisplayTextWidget.PBP_TrickDisplayTextWidget_C";
static const wchar_t* const kTextClassName = L"PBP_TrickDisplayTextWidget_C";

// UTrickDisplayTextWidget's own members (PDB). `_text` saves a widget-tree walk, and Duration is the
// one thing about the class that has to be neutralised: its NativeTick counts DeltaTime into `_time`
// and broadcasts `_onRemoveWidget` once it passes Duration. OUR instance has nothing bound to that
// delegate, so the broadcast does nothing -- but it would then fire every frame forever, so the
// duration is simply pushed out of reach at construction and the tick never reaches it.
enum {
    TDT_TEXT     = 0x260,   // UTrickDisplayTextWidget::_text (UTextBlock*)
    TDT_DURATION = 0x29c,   // ::_displayTextParams.Duration (float; params at +0x288, Duration at +0x14)
};
// Everything about a UTextBlock's look is in game_syms.h's off:: block with the rest of the widget
// offsets -- written directly and pushed into the live STextBlock with SynchronizeProperties, which
// is the route the MyNacon panel already proved in the field.

// ---- the look. Live statics, so every one of these can be corrected in a build without touching the
// call site. The two colours are the ones the ImGui plates use (theme.h `other` and `text`), repeated
// as plain numbers rather than included: nothing in this file may depend on the overlay.
//
// THEY ARE sRGB, AND SLATE WANTS LINEAR. This is the one trap in the whole file. An ImVec4 is used
// as written; an FLinearColor is converted to sRGB on its way to the screen, so the SAME numbers come
// out much paler. (0.55, 0.80, 1.00) handed over raw measured (197, 232, 255) on the rendered frame
// -- a blue so washed out it reads as white, which is exactly what it looked like. So the numbers
// here stay in the space they were chosen in and `toLinear` converts them at the point of the write.
float kNameR = 0.55f, kNameG = 0.80f, kNameB = 1.00f;    // somebody else's name -- pale blue
float kMsgR  = 0.88f, kMsgG  = 0.88f, kMsgB  = 0.88f;    // what they said -- near-white
float kSayR  = 0.98f, kSayG  = 0.78f, kSayB  = 0.22f;    // you -- the game's own highlight amber
float kSysR  = 0.55f, kSysG  = 0.60f, kSysB  = 0.65f;    // joins, leaves, notices
float kDimR  = 0.62f, kDimG  = 0.62f, kDimB  = 0.62f;    // the header and the key hints
float kWarnR = 1.00f, kWarnG = 0.55f, kWarnB = 0.35f;    // ...and a counter with no room left
// The header and the hints are set smaller than the talk, which is most of what stops a box
// with furniture in it reading as a wall of text.
float kChatSmall = 12.0f;
int   kCountWarnAt = 40;                                 // show the counter this near the limit
// The chat is a fixed size: it is not standing anywhere in the world, so nothing about it is
// perspective. Slate units, so it holds its share of the screen at any resolution.
float kChatSize = 15.0f;
// What sits in front of the line being typed, so an empty box still looks like one.
const char* kChatPrompt = "> ";
// HOW WIDE A CHAT LINE IS ALLOWED TO GET, in characters. See `wrapInto` for why this is counted in
// characters rather than measured in units.
int kChatCols = 66;
// The BUFFER is fixed and the knob is clamped into it, because a live static cannot size an array
// and the whole point of the knob is that it can be corrected without a rebuild.
enum { kChatRows = 64, kChatColsMax = 160 };
// The dark rim that keeps a name readable over a bright wall. On the other surface this is four
// offset copies of the text; here it is the font's own outline, which is cheaper and rounder.
// IT IS SIZED WITH THE TEXT. A flat 2 is a fifth of the way across a 14 px glyph -- the rims of
// neighbouring letters meet and the name turns into a black slab with something written on it. One
// pixel up to `kOutlineBigAt`, two above, which is what the ImGui plates did for the same reason.
int   kOutlineThin = 1, kOutlineThick = 2, kOutlineBigAt = 26;
float kOutlineA    = 0.90f;
// A soft drop shadow UNDER the thinner rim, for the depth the fat one was providing by accident.
float kShadowX = 1.0f, kShadowY = 1.0f, kShadowA = 0.55f;
// Tracking, in thousandths of an em. The game's own menus set their text slightly open and it is
// most of why they read as clean rather than cramped.
int   kTracking    = 45;
// THE TYPEFACE. The trick text is authored in Arial Unicode MS BOLD, which is heavy for a name
// floating over somebody's head. The same family's REGULAR cut is in the game and the menus use it,
// so the plate is switched to it when it can be found -- and left bold when it cannot, which costs
// nothing but weight. Found, never loaded: see findClass.
bool  kRegularWeight = true;
// The font size a plate is drawn at when the peer is at NameplateTuning::refDistCm, in SLATE units
// (the position is handed over in pixels and converted, so this stays a constant share of the screen
// at any resolution).
float kBaseSize    = 18.0f;
// A line's height as a multiple of its font size -- only needed to sit the speech bubble on top of
// the name, since the widgets themselves are placed by their own bottom edge and size themselves.
float kLineFactor  = 1.25f;
float kBubbleGap   = 0.30f;    // ...and the gap between the two, in font sizes
float kBubbleWrapEm = 15.0f;   // wrap width as a multiple of the font size, as on the other surface
int   kZOrder      = 100;      // the floating names: over the world, under the menus
// THE CHAT GOES OVER EVERYTHING, the pause menu included -- that is what was asked for, and it is
// right anyway: a message arriving while somebody is in a menu is exactly when they most need to see
// it. A widget's z-order is fixed when it is added, so this cannot be a per-frame decision; the pool
// is SPLIT instead, and a chat widget is never a nameplate widget.
//
// WHICH LAYER, AND WHY THIS ONE. Two combinations have been tried in the field and both lost to the
// menu: the viewport at z 9000, and the player screen at z 1000000. This is the third -- the ordinary
// viewport at a z nothing sane would author -- and it is the one that was never tried, because the
// first attempt raised the LAYER when it should have raised the NUMBER. `kChatPlayerLayer` switches
// back to the player screen if that turns out to be the answer after all.
int   kZOrderChat  = 1000000;
bool  kChatPlayerLayer = false;
// HOW FAR THE BOX RIDES UP while the pause menu is displayed, in slate units -- roughly two inches on
// a normal screen, which is what was asked for. It lifts the whole thing: the panel, its rules and
// every row all hang off one base, so there is one number and nothing to keep in step.
float kPauseLiftY  = 180.0f;
// How many widgets may be built in one frame. A session filling up must not cost a hitch, and there
// is no hurry: a plate that appears a frame or two late is invisible to the eye.
int   kMakePerFrame = 2;

// =====================================================================================================
// THE POOL
// =====================================================================================================
// WHAT THE POOL HAS TO HOLD: a name and a speech bubble for every possible peer, plus the chat's own
// rows and the line being typed. The lane numbers only have to be distinct -- they are salt on the
// key, so that two surfaces asking about the same text never land on the same widget.
// How many MESSAGES are fetched, and how many VISUAL lines they can wrap into. Both are far
// larger than the window that is drawn: the window is what fits on screen, these are what
// there is to scroll back through.
enum { kChatLines = 48 };
enum { kLaneName = 0, kLaneBubble = 1, kLaneChat = 100, kLaneChatIn = 90,
       kLaneChatHdr = 91, kLaneChatOnline = 92, kLaneChatHints = 93, kLaneChatCount = 94,
       kLaneChatEmpty = 95 };
// The pool is two zones, because the two halves are added to the viewport at different depths and a
// slot cannot change its mind afterwards. Names first, then the chat's rows and its furniture.
enum { kSlotsName = OMP_MAX_PEERS * 2, kSlotsChat = 32, kSlots = kSlotsName + kSlotsChat };

struct Slot {
    void*    w    = nullptr;      // the UUserWidget
    void*    tb   = nullptr;      // its UTextBlock
    uint64_t key  = 0;            // who it is drawing this frame; 0 = free
    bool     claimed = false;
    bool     shown   = false;
    int      size  = 0;           // what it was last styled with, so nothing is re-applied for nothing
    uint32_t rgba  = 0;
    float    wrap  = -1.0f;
    float    ax = -1.0f, ay = -1.0f;   // where its own box is pinned; -1 = never set

    char     text[256] = {};      // wide enough for the longest chat line, name and all
};
static Slot g_slot[kSlots];
static int  g_built = 0;          // widgets that exist
// The world every one of them was built in. The pool is keyed to it -- see game_hud.h for what that
// buys and what it cost to learn.
static void* g_world = nullptr;
static bool g_enabled = true;
static int  g_lastDrawn = 0, g_lastMade = 0, g_faults = 0;
static float g_fade = 0.0f;       // the on/off-board fade; this thread owns it
static float g_frameDt = 1.0f / 60.0f;   // this frame's length, measured by Begin
static uint64_t g_lastUs = 0;

static uint64_t nowUs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)freq.QuadPart);
}
static uint64_t hashOf(const char* s, int lane) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(lane + 1);
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h ? h : 1;
}
static uint64_t pack2(float x, float y) {
    uint64_t p; const float f[2] = { x, y }; memcpy(&p, f, 8); return p;
}
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// THE DPI SCALE, measured once a frame by Begin. Pixels / it = slate units, which is the space a
// font is sized in and a wrap width measured in; everything this file PLACES is in pixels, so a
// number that arrives in slate units goes through here on its way to a position.
// Somewhere to say the one thing about this surface that is worth saying out loud: what the
// borrowed panel widget actually turned out to be made of. Set by the loader.
static void (*g_hudLog)(const char*) = nullptr;
static float g_dpi = 1.0f;
static float px(float slateUnits) { return slateUnits * g_dpi; }
static float slateOf(float pixels) { return g_dpi > 0.01f ? pixels / g_dpi : pixels; }
// One colour, packed the way `style` wants it. sRGB in -- the conversion happens at the write.
static uint32_t rgba(float r, float g, float b, float a) {
    auto q = [](float v) { return (uint32_t)(clamp01(v) * 255.0f + 0.5f); };
    // The ALPHA is quantised coarsely on purpose: it changes continuously while anything is fading,
    // and re-synchronising a Slate font for a hundredth of a step is work nobody can see.
    return (q(r) << 24) | (q(g) << 16) | (q(b) << 8) | (q(a) & 0xf8);
}
// sRGB -> linear, the exact curve Slate undoes on the way to the screen. See the colour note above:
// without this every colour in this file arrives on screen several shades too pale.
static float toLinear(float c) {
    c = clamp01(c);
    return (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
}
// How heavy the rim is at a given font size (see kOutlineThin).
static int outlineFor(int size) { return size >= kOutlineBigAt ? kOutlineThick : kOutlineThin; }

// ---- FText from plain ASCII. FText::FromString when it resolved (no length limit), else the FName
// route and its NAME_SIZE ceiling -- the same rule, and the same reason, as news_panel's makeText.
struct TextBlob { void* data; void* refCtrl; unsigned flags; unsigned pad; };
struct FStr { wchar_t* d; int n; int max; };
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

// THE REGULAR CUT of the menu font, found once and remembered. Found, never loaded: the menus use it,
// so it is resident wherever a menu has been; where it is not, the plate simply stays bold. Writing
// the pointer into a UTextBlock's Font is safe against the collector -- Font is a UPROPERTY, so the
// widget that holds it is what keeps it alive.
static const wchar_t* const kRegularFontPath =
    L"/Game/Menus/Fonts/arial-unicode-ms_Font.arial-unicode-ms_Font";
static void* g_regularFont = nullptr;
static bool  g_fontLooked  = false;
static void* regularFont() {
    const Syms& S = Get();
    if (g_fontLooked || !S.StaticFindObject) return g_regularFont;
    g_fontLooked = true;
    __try { g_regularFont = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kRegularFontPath, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_regularFont = nullptr; }
    return g_regularFont;
}

static void* findClass() {
    const Syms& S = Get();
    if (!S.StaticFindObject) return nullptr;
    void* c = nullptr;
    __try { c = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kTextClassPath, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (c) return c;
    __try { c = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kTextClassName, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return c;
}

bool GameHud_Available() {
    const Syms& S = Get();
    return S.WidgetCreate && S.WidgetAddToViewport && S.TextBlockSetText && S.WidgetRemoveParent &&
           S.WidgetSetVisible && S.WidgetSetPosInVp && S.WidgetSetAlignInVp && findClass() != nullptr;
}
void GameHud_SetLog(void (*logf)(const char*)) { g_hudLog = logf; }
bool GameHud_Enabled()          { return g_enabled; }
void GameHud_SetEnabled(bool on) {
    if (on == g_enabled) return;
    // Going back to the overlay must not leave ours up. The world is whatever they were built in --
    // switching surfaces does not change worlds, so that is the one this is safe against.
    if (!on) GameHud_Clear(g_world);
    g_enabled = on;
}

// ---- one widget ------------------------------------------------------------------------------------
// Built, styled once with everything that never changes, and left on the viewport COLLAPSED. Every
// later frame only moves it, re-texts it and shows it.
// WHICH LAYER A WIDGET GOES ON. The floating names belong with the world, under the menus, so they
// go on the viewport the ordinary way. The CHAT has to sit over the game's own pause menu -- and no
// z-order can do that, because the menu is not in the same layer: SGameLayerManager slots the
// per-player canvas AFTER the viewport overlay, so anything added with AddToPlayerScreen draws above
// everything AddToViewport put down. (Field: the chat at z 9000 was still being blurred and darkened
// by the menu's own backdrop, which only happens to something BENEATH it.)
// Falls back to the viewport when the call is missing or the widget has no owning local player --
// which is the behaviour every build had until now, so the worst case is where we started.
static const char* g_addPath = "nothing added yet";
static bool addToScreen(void* w, int zOrder, bool playerLayer) {
    const Syms& S = Get();
    if (playerLayer && S.WidgetAddToPlayer) {
        bool ok = false;
        __try { ok = S.WidgetAddToPlayer(w, zOrder); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        // IT CAN REFUSE. AddToPlayerScreen needs an owning LOCAL player and returns false without
        // one, which is why the widgets are created against the local controller below.
        if (ok) { g_addPath = "player screen"; return true; }
        g_addPath = "player screen REFUSED -> viewport";
    } else {
        g_addPath = "viewport";
    }
    __try { S.WidgetAddToViewport(w, zOrder); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// The local player controller, to OWN the widgets we build. Two reasons it matters: AddToPlayerScreen
// refuses a widget with no owning local player, and a widget created against the game instance alone
// does not necessarily get one.
static void* ownerPC() {
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

static bool build(Slot& s, void* cls, void* gi, int zOrder, bool playerLayer) {
    const Syms& S = Get();
    void* w = nullptr;
    __try { w = S.WidgetCreate(gi, cls, ownerPC()); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
    if (!w) return false;
    void* tb = nullptr;
    __try {
        tb = *(void**)((uint8_t*)w + TDT_TEXT);
        *(float*)((uint8_t*)w + TDT_DURATION) = 1.0e9f;      // its tick must never reach the end
    } __except (EXCEPTION_EXECUTE_HANDLER) { tb = nullptr; }
    // The widget tree is the fallback if the native pointer ever moves: the sub-widget's name is the
    // blueprint's own, read out of the asset.
    if (!tb && S.WidgetTreeFind && S.FNameCtor) {
        __try {
            void* tree = *(void**)((uint8_t*)w + off::kUserWidgetTree);
            if (tree) {
                unsigned long long fn[2] = { 0, 0 };
                S.FNameCtor(fn, "_text", 1 /* FNAME_Add */);
                tb = S.WidgetTreeFind(tree, fn);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { tb = nullptr; }
    }
    if (!tb) { __try { S.WidgetRemoveParent(w); } __except (EXCEPTION_EXECUTE_HANDLER) {} return false; }

    void* regular = kRegularWeight ? regularFont() : nullptr;
    __try {
        uint8_t* t = (uint8_t*)tb;
        uint8_t* f = t + off::kTextFont;
        *(uint8_t*)(t + off::kTextJustify) = 1;                     // Center: the blueprint authors it Right
        *(uint8_t*)(t + off::kTextAutoWrap) = 0;                    // ...and never auto-wrap: see `wrapInto`
        // The rim and the shadow are BLACK here; their alphas are set per frame with the text's,
        // because Slate does NOT multiply a text block's ColorAndOpacity into either of them. That
        // was assumed when this was written and the field found it out: a line that faded left its
        // own outline behind, hanging in the air after the words had gone.
        float* oc = (float*)(f + off::kFontOutline + off::kFontOutlineColor);
        oc[0] = 0.0f; oc[1] = 0.0f; oc[2] = 0.0f; oc[3] = kOutlineA;
        float* so = (float*)(t + off::kTextShadowOffset);
        so[0] = kShadowX; so[1] = kShadowY;
        float* sc = (float*)(t + off::kTextShadowColor);
        sc[0] = 0.0f; sc[1] = 0.0f; sc[2] = 0.0f; sc[3] = kShadowA;
        *(int32_t*)(f + off::kFontLetterSpacing) = kTracking;
        *(uint8_t*)(t + off::kTextColor + off::kSlateColorRule) = 0;          // UseColor_Specified
        // The lighter cut of the same family, if it turned up. The typeface NAME goes with it: both
        // font assets carry a single face called "Default", and a name that is not in the object
        // being pointed at would fall back to something that is not this family at all.
        if (regular && S.FNameCtor) {
            *(void**)(f + off::kFontObject) = regular;
            S.FNameCtor(f + off::kFontTypeface, "Default", 1 /* FNAME_Add */);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }

    if (!addToScreen(w, zOrder, playerLayer)) { g_faults++; return false; }
    // The alignment is NOT set here -- it belongs to whoever claims the slot (see `align`).
    __try { S.WidgetSetVisible(w, 1 /* Collapsed */); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    s.w = w; s.tb = tb; s.shown = false; s.size = 0; s.rgba = 0; s.wrap = -1.0f;
    s.ax = s.ay = -1.0f; s.text[0] = 0;
    g_built++;
    return true;
}

// ---- styling, applied only when it actually changed -------------------------------------------------
static void style(Slot& s, int size, uint32_t rgba, float wrapPx) {
    const Syms& S = Get();
    if (s.size == size && s.rgba == rgba && s.wrap == wrapPx) return;
    s.size = size; s.rgba = rgba; s.wrap = wrapPx;
    __try {
        uint8_t* t = (uint8_t*)s.tb;
        *(int32_t*)(t + off::kTextFont + off::kFontSize) = size;
        *(int32_t*)(t + off::kTextFont + off::kFontOutline + off::kFontOutlineSize) = outlineFor(size);
        // THE RIM AND THE SHADOW FADE WITH THE TEXT, by hand -- see the note in `build`. Without
        // this the words go and their outline stays, which is what "the colour fades quicker than
        // the shadow behind it" was.
        const float alpha = (float)(rgba & 0xff) / 255.0f;
        *(float*)(t + off::kTextFont + off::kFontOutline + off::kFontOutlineColor + 12) = kOutlineA * alpha;
        *(float*)(t + off::kTextShadowColor + 12) = kShadowA * alpha;
        // sRGB in, LINEAR out -- see the colour note at the top. The ALPHA is not converted: it is
        // already a plain coverage fraction and putting it through the curve would make every fade
        // sit at the wrong place.
        float* c = (float*)(t + off::kTextColor + off::kSlateColorValue);
        c[0] = toLinear((float)((rgba >> 24) & 0xff) / 255.0f);
        c[1] = toLinear((float)((rgba >> 16) & 0xff) / 255.0f);
        c[2] = toLinear((float)((rgba >>  8) & 0xff) / 255.0f);
        c[3] = (float)( rgba        & 0xff) / 255.0f;
        // AutoWrapText is NEVER set -- see the note on `wrapInto`. WrapTextAt alone is a hard width
        // that does not depend on any geometry, so it gives the same answer every frame.
        *(float*)(t + off::kTextWrapAt) = wrapPx > 0.0f ? wrapPx : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return; }
    if (S.TextSyncProps) __try { S.TextSyncProps(s.tb); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

static void say(Slot& s, const char* text) {
    const Syms& S = Get();
    if (strncmp(s.text, text, sizeof(s.text) - 1) == 0) return;   // nothing changed: no FText, no rebuild
    strncpy_s(s.text, text, _TRUNCATE);
    TextBlob ft;
    if (!makeText(text, &ft)) return;
    __try { S.TextBlockSetText(s.tb, &ft); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

static void place(Slot& s, float xPx, float yPx) {
    const Syms& S = Get();
    // Whole pixels. A glyph quad landing on a fractional pixel is resampled every frame and shimmers
    // -- the same reason the ImGui plates snap, and it matters more here because Slate caches.
    const float x = floorf(xPx + 0.5f), y = floorf(yPx + 0.5f);
    __try { S.WidgetSetPosInVp(s.w, pack2(x, y), true /* we hand over PIXELS */); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

// WHICH CORNER OF ITSELF A WIDGET IS PINNED BY. A name hangs from its bottom CENTRE over a head; a
// chat line starts at its bottom LEFT against the margin. Set only when it changes, because a slot
// gets reused across both.
static void align(Slot& s, float ax, float ay) {
    const Syms& S = Get();
    if (s.ax == ax && s.ay == ay) return;
    s.ax = ax; s.ay = ay;
    __try { S.WidgetSetAlignInVp(s.w, pack2(ax, ay)); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

static void showIt(Slot& s, bool on) {
    const Syms& S = Get();
    if (s.shown == on) return;
    s.shown = on;
    // HitTestInvisible, never plain Visible: a floating name must not be able to eat a click.
    __try { S.WidgetSetVisible(s.w, on ? 3 : 1); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

// ---- WRAPPING, DONE HERE AND NOT BY SLATE ------------------------------------------------------
// Slate's AutoWrapText wraps to the width the widget was ALLOTTED, and a widget in an auto-sized
// viewport slot is allotted whatever its own desired size was LAST frame -- which is the width of
// the text it held last frame. Reusing one widget for a row whose text changes therefore wraps the
// new text at the old text's width, and the result was exactly the field report: a long line not
// wrapping at all, a short one broken after its first word, and the caret blinking on and off
// shoving the whole prompt up a line and back down twice a second (each blink changed the string,
// so each blink re-wrapped it against the other one's width).
//
// WrapTextAt on its own has none of that -- it is a hard width in slate units and needs no geometry
// -- but a wrapped widget is still TALLER, and these are placed by their bottom edge, so a line that
// wrapped would grow up into the row above it. So the wrapping happens here, one widget per visual
// line, and Slate is told not to wrap at all.
//
// It counts CHARACTERS because nothing on this side can measure a proportional font: the mod never
// gets to ask Slate how wide a string came out. The right edge is therefore ragged by a few
// characters, which is invisible with nothing drawn behind the text.
//
// Copies one line's worth out of `text` starting at `from`, breaking at the last space that fits.
// Returns where the next line starts, or -1 when that was the last one.
static int wrapInto(const char* text, int from, char* out, int cap) {
    const int len = (int)strlen(text);
    int cols = kChatCols; if (cols < 8) cols = 8; if (cols > cap - 1) cols = cap - 1;
    if (from >= len) { out[0] = 0; return -1; }
    int take = len - from;
    if (take > cols) {
        take = cols;
        // Back up to the last space, unless the "word" is so long that there is none to back up to
        // -- in which case it is split where it lands rather than pushed off the screen.
        int brk = -1;
        for (int k = take; k > 0; k--) if (text[from + k] == ' ') { brk = k; break; }
        if (brk > 0) take = brk;
    }
    memcpy(out, text + from, (size_t)take);
    out[take] = 0;
    int next = from + take;
    while (text[next] == ' ') next++;          // the break's own space is not drawn on the next line
    return next >= len ? -1 : next;
}

// A slot for `key`, in order of preference: the one that ALREADY has it (so a peer keeps their widget
// and their text is never rebuilt for nothing), then an idle one, then a fresh one, then any free one
// at all. The last step matters: with the pool full and every widget still showing from last frame,
// the first three all come up empty, and a peer who had just been given a new key would get nothing.
static Slot* claim(uint64_t key, bool chat, void* cls, void* gi, int* madeThisFrame) {
    const int lo = chat ? kSlotsName : 0, hi = chat ? kSlots : kSlotsName;
    const int z  = chat ? kZOrderChat : kZOrder;
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && s.key == key && !s.claimed) { s.claimed = true; return &s; } }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && !s.claimed && !s.shown) { s.claimed = true; s.key = key; return &s; } }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w || *madeThisFrame >= kMakePerFrame) continue;
        (*madeThisFrame)++;
        if (!build(s, cls, gi, z, chat && kChatPlayerLayer)) return nullptr;
        s.claimed = true; s.key = key;
        return &s; }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && !s.claimed) { s.claimed = true; s.key = key; return &s; } }
    return nullptr;
}

// =====================================================================================================
// THE FLOATING NAMES
// The alphas, the distance fade and the perspective scale are deliberately the SAME arithmetic as the
// ImGui path (nameplates.cpp) -- the two surfaces have to agree about when a name is visible, or
// switching between them would look like a bug in whichever one was on.
// =====================================================================================================
// ---- the frame ------------------------------------------------------------------------------------
// What Begin works out and the rest of the frame borrows. Not statics with a longer life than a
// frame: if Begin said no, none of this is valid and nothing may run.
static void* g_cls = nullptr;
static void* g_gi  = nullptr;
static int   g_vw = 0, g_vh = 0, g_made = 0, g_drawn = 0;
static void* g_frameWorld = nullptr;

bool GameHud_Begin(void* world) {
    g_cls = nullptr; g_gi = nullptr; g_vw = g_vh = 0; g_made = 0; g_drawn = 0; g_frameWorld = nullptr;
    if (!g_enabled) return false;
    // THE LIVENESS RULE, BEFORE ANYTHING ELSE IS READ. A world we do not recognise means every widget
    // in the pool belongs to a level that is gone: drop them where they stand. No world at all means
    // we cannot tell, so nothing is touched -- not drawn, not moved, not even hidden.
    if (world && g_world && world != g_world) GameHud_Forget();
    if (!world) return false;

    // The name fade runs even with nothing to draw, so the alpha is already right the moment a peer
    // turns up. Our own clock: this is a game-thread frame and there is no io.DeltaTime here.
    const NameplateTuning& T = Nameplates_Tuning();
    const uint64_t us = nowUs();
    float dt = g_lastUs ? (float)(us - g_lastUs) / 1.0e6f : (1.0f / 60.0f);
    g_lastUs = us;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    g_frameDt = dt;

    for (Slot& s : g_slot) s.claimed = false;
    g_frameWorld = world;
    g_cls = findClass();
    g_gi  = g_cls ? VersionTag_GameInstance() : nullptr;
    if (!g_cls || !g_gi) return false;
    if (!omp::game::ViewportSize(&g_vw, &g_vh) || g_vw <= 0 || g_vh <= 0) return false;
    // The DPI scale, once a frame. Optional: without it everything is placed as though the screen
    // were 1080p, which is exactly what this file did before the symbol existed.
    g_dpi = 1.0f;
    if (const Syms& S = Get(); S.ViewportScale) {
        float d = 1.0f;
        __try { d = S.ViewportScale(g_gi); } __except (EXCEPTION_EXECUTE_HANDLER) { d = 1.0f; }
        if (d > 0.05f && d < 20.0f) g_dpi = d;
    }
    (void)T;
    return true;
}

void GameHud_End() {
    if (!g_enabled) return;
    // Everything nobody claimed goes away. Collapsed, not removed: the widget is kept for whoever
    // needs it next, and building one is the only part of this that costs anything.
    // It runs even when Begin said no -- as long as there IS a world, because the reason Begin
    // refused may be that the names were switched off, and their plates still have to come down.
    if (g_frameWorld) for (Slot& s : g_slot) if (s.w && !s.claimed) { showIt(s, false); s.key = 0; }
    g_lastDrawn = g_drawn; g_lastMade = g_made;
    if (g_built > 0 && g_frameWorld) g_world = g_frameWorld;   // what the pool now belongs to
}

void GameHud_Names(const NameplateItem* items, int n, bool show) {
    if (!g_cls || !g_gi) return;
    const NameplateTuning& T = Nameplates_Tuning();
    const float target = (show && T.enabled) ? 1.0f : 0.0f;
    const float step = (T.fadeSec > 0.01f) ? (g_frameDt / T.fadeSec) : 1.0f;
    if (g_fade < target)      g_fade = (g_fade + step > target) ? target : g_fade + step;
    else if (g_fade > target) g_fade = (g_fade - step < target) ? target : g_fade - step;

    void* const cls = g_cls; void* const gi = g_gi;
    const int vw = g_vw, vh = g_vh;
    int& drawn = g_drawn; int& made = g_made;
    if (T.enabled && n > 0) {
        for (int i = 0; i < n; i++) {
            const NameplateItem& it = items[i];
            if (it.x < -0.5f || it.x > 1.5f || it.y < -0.5f || it.y > 1.5f) continue;

            float nameA = it.name[0] ? g_fade : 0.0f;
            if (nameA > 0.0f) {
                if (it.distCm > T.maxDistCm) nameA = 0.0f;
                const float fs = T.maxDistCm * T.fadeDistFrac;
                if (nameA > 0.0f && it.distCm > fs && T.maxDistCm > fs)
                    nameA *= 1.0f - (it.distCm - fs) / (T.maxDistCm - fs);
            }
            float msgA = 0.0f;
            if (T.bubbles && it.msg[0] && it.distCm <= T.bubbleMaxDistCm) {
                const float age = (float)it.msgAgeMs / 1000.0f;
                msgA = 1.0f;
                if (age > T.bubbleHoldSec) {
                    const float over = T.bubbleFadeSec > 0.01f ? T.bubbleFadeSec : 1.0f;
                    msgA = 1.0f - (age - T.bubbleHoldSec) / over;
                }
                const float fs = T.bubbleMaxDistCm * T.bubbleFadeDistFrac;
                if (msgA > 0.0f && it.distCm > fs && T.bubbleMaxDistCm > fs)
                    msgA *= 1.0f - (it.distCm - fs) / (T.bubbleMaxDistCm - fs);
            }
            if (nameA <= 0.01f && msgA <= 0.01f) continue;

            float scale = (it.distCm > 1.0f) ? (T.refDistCm / it.distCm) : T.maxScale;
            if (scale < T.minScale) scale = T.minScale;
            if (scale > T.maxScale) scale = T.maxScale;
            int size = (int)(kBaseSize * scale + 0.5f);
            if (size < 6) size = 6;

            const float cx = it.x * (float)vw;
            const float hy = it.y * (float)vh;
            const float lineH = (float)size * kLineFactor;

            // ---- the name, its bottom edge on the head point
            if (nameA > 0.01f) {
                Slot* s = claim(hashOf(it.name, kLaneName), false, cls, gi, &made);
                if (s) {
                    style(*s, size, rgba(kNameR, kNameG, kNameB, nameA), 0.0f);
                    align(*s, 0.5f, 1.0f);      // hangs from its bottom centre, over the head
                    say(*s, it.name);
                    place(*s, cx, hy);
                    showIt(*s, true);
                    drawn++;
                }
            }
            // ---- what they said, sitting on top of where the name is (whether or not it is showing,
            // so mounting a board does not make an open bubble jump down the screen)
            if (msgA > 0.01f) {
                Slot* s = claim(hashOf(it.name, kLaneBubble), false, cls, gi, &made);
                if (s) {
                    style(*s, size, rgba(kMsgR, kMsgG, kMsgB, msgA), (float)size * kBubbleWrapEm);
                    align(*s, 0.5f, 1.0f);
                    say(*s, it.msg);
                    place(*s, cx, hy - lineH - (float)size * kBubbleGap);
                    showIt(*s, true);
                    drawn++;
                }
            }
        }
    }
}

// =====================================================================================================
// THE PANEL BEHIND THE CHAT
//
// A UTextBlock draws no background, and the mod has no way to construct a bare UImage -- so the box
// is ONE MORE GAME WIDGET: WBP_FadeIn, the screen-fade overlay, which is a canvas holding a
// full-bleed image and nothing else that matters. Confirmed resident during play from the field
// heartbeat (`box=WBP_FadeIn resident`) before any of this was written.
//
// Its image is found by WALKING THE TREE for the first UImage rather than by name. The names inside
// a blueprint are the designer's and could be anything; "the first image in the fade overlay" is a
// description of the asset that stays true. The blur that sits with it is collapsed -- it is driven
// by an animation we never play, so whatever strength it was authored at is not ours to inherit.
// =====================================================================================================
// THE FURNITURE: plain rectangles, borrowed.
//
// A UTextBlock draws no background and the mod cannot construct a bare UImage, so every solid shape
// on this surface -- the panel, its rules, the accent down the side of the input -- is an instance of
// WBP_FadeIn, the game's screen-fade overlay, which is a canvas holding a full-bleed image. Cheap:
// one image each, collapsed until something wants it.
//
// The PANEL also uses that widget's UBackgroundBlur, which is what makes it look like part of the
// game rather than a rectangle sitting on top of it. The other rects collapse theirs.
// =====================================================================================================
static const wchar_t* const kBoxClassPath = L"/Game/Utils/UI/WBP_FadeIn.WBP_FadeIn_C";
float kBoxR = 0.04f, kBoxG = 0.04f, kBoxB = 0.05f;   // theme.h's panel, near-black
float kBoxAlpha = 0.48f;                             // ...and how much of it, over the blur
float kBoxPadX  = 16.0f, kBoxPadY = 12.0f;           // slate units of room inside the panel
float kBoxFadeSec = 0.12f;                           // it arrives and leaves rather than blinking
bool  kBoxOn    = true;
// THE BLUR. Strength drives Slate's own radius; the radius is ALSO set outright, because the
// automatic one is derived from the strength by a formula that is not ours and a blur nobody can see
// is the same as no blur at all (field: "I'm not noticing much blur").
bool  kBoxBlur      = true;
float kBoxBlurMax   = 34.0f;
// MEASURED, not guessed: at radius 14 the detail inside the panel was already a third of what
// it was on the same ground outside it -- the blur was working and simply did not read as one
// over flat dirt under a dark tint. The radius is the lever that makes it obvious.
int   kBoxBlurRadius = 30;
float kRuleAlpha  = 0.16f;                           // theme.h's rule: a thin light line
float kRuleThick  = 1.0f;
// Air around each rule. Without it the header sits ON its line and the first message sits on
// the other side of it, which reads as cramped however good everything else looks.
float kRuleGap    = 7.0f;
float kAccentW    = 3.0f;                            // the lit edge down the side of the input

enum { kRectPanel = 0, kRectRuleTop, kRectRuleBot, kRectAccent, kRectCount };
struct Rect { void* w = nullptr; void* img = nullptr; void* blur = nullptr; bool shown = false; };
static Rect g_rect[kRectCount];
static int  g_rectTried = 0;
static float g_boxFade = 0.0f;       // 0 = gone, 1 = fully up; eased by the frame clock

// A widget's class name, for telling a UImage from everything else in a tree.
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

// Breadth-first over the widget tree, collecting the image and the blur. The structure of the asset
// goes into the log the first time, because which of the two is inside the other decides what the
// panel ends up looking like and it is not worth guessing at from a screenshot.
static void findPartsIn(void* userWidget, Rect& r, char* shape, int shapeCap) {
    void* queue[64];
    int   head = 0, tail = 0, at = 0;
    if (shapeCap > 0) shape[0] = 0;
    __try {
        void* tree = *(void**)((uint8_t*)userWidget + off::kUserWidgetTree);
        if (!tree) return;
        void* root = *(void**)((uint8_t*)tree + off::kWidgetTreeRoot);
        if (root) queue[tail++] = root;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    while (head < tail && tail < (int)(sizeof(queue) / sizeof(queue[0]))) {
        void* node = queue[head++];
        char cn[64] = "?";
        classNameOf(node, cn, sizeof(cn));
        if (at < shapeCap - 20) at += snprintf(shape + at, (size_t)(shapeCap - at), " %s", cn);
        if (!r.img  && strstr(cn, "Image"))          { r.img  = node; continue; }   // a leaf
        if (!r.blur && strstr(cn, "BackgroundBlur"))   r.blur = node;
        // CHILDREN, AND ONLY OFF SOMETHING THAT HAS THEM. UPanelWidget::Slots is at +0x108, which on
        // a UImage is the middle of its brush -- a brush that happened to hold a plausible pointer
        // and a small count would send this walking rubbish. There is no cheap way to ask "is this a
        // UPanelWidget", so the panel classes are named; anything else is a leaf and is left alone.
        if (!strstr(cn, "Panel") && !strstr(cn, "Canvas") && !strstr(cn, "Box") &&
            !strstr(cn, "Overlay") && !strstr(cn, "Border") && !strstr(cn, "Blur") &&
            !strstr(cn, "Grid") && !strstr(cn, "Wrap")) continue;
        __try {
            void** data = *(void***)((uint8_t*)node + off::kPanelSlots);
            const int num = *(const int*)((uint8_t*)node + off::kPanelSlots + 8);
            if (!data || num <= 0 || num > 64) continue;
            for (int i = 0; i < num && tail < (int)(sizeof(queue) / sizeof(queue[0])); i++) {
                void* slot = data[i];
                if (!slot) continue;
                void* child = *(void**)((uint8_t*)slot + off::kSlotContent);
                if (child) queue[tail++] = child;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }
}

// Pin a widget to an explicit rect inside its canvas. ONLY when its slot really is a canvas slot:
// these setters are UCanvasPanelSlot's, and a UBackgroundBlurSlot (which is what a child of the blur
// has) would take the writes at the wrong offsets entirely.
static void pinToRect(void* widget, float wS, float hS) {
    const Syms& S = Get();
    if (!widget || !S.SlotSetAnchors || !S.SlotSetPosition || !S.SlotSetSize) return;
    void* slot = nullptr;
    __try { slot = *(void**)((uint8_t*)widget + off::kWidgetSlot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!slot) return;
    char sn[64] = "?";
    classNameOf(slot, sn, sizeof(sn));
    if (!strstr(sn, "CanvasPanelSlot")) return;
    __try {
        const float topLeft[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        S.SlotSetAnchors(slot, topLeft);
        S.SlotSetPosition(slot, pack2(0.0f, 0.0f));
        S.SlotSetSize(slot, pack2(wS, hS));
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

// Build one. Failure is remembered so a missing asset costs a handful of lookups, not one per frame.
static bool ensureRect(int i) {
    const Syms& S = Get();
    Rect& r = g_rect[i];
    if (r.w) return true;
    if (g_rectTried > kRectCount * 2 || !kBoxOn) return false;
    if (!S.StaticFindObject || !S.WidgetCreate || !S.WidgetAddToViewport || !S.ImageSetColor) return false;
    g_rectTried++;
    void* cls = nullptr;
    __try { cls = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kBoxClassPath, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!cls) return false;
    void* w = nullptr;
    __try { w = S.WidgetCreate(g_gi, cls, ownerPC()); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!w) return false;
    char shape[256];
    findPartsIn(w, r, shape, sizeof(shape));
    if (!r.img) { __try { S.WidgetRemoveParent(w); } __except (EXCEPTION_EXECUTE_HANDLER) {} return false; }
    // The blur is the PANEL's alone, and its strength is written BEFORE the widget is realised so the
    // Slate side picks it up when it is built -- a blur has no synchronise to call afterwards the way
    // a text block has. Everything else collapses the one it came with.
    const bool wantBlur = (i == kRectPanel) && kBoxBlur;
    if (r.blur) __try {
        if (wantBlur) {
            *(uint8_t*)((uint8_t*)r.blur + off::kBlurApplyAlpha) = 0;   // full strength, whatever the tint
            *(uint8_t*)((uint8_t*)r.blur + off::kBlurAutoRadius) = 1;   // ...and OUR radius, not a derived one
            *(int32_t*)((uint8_t*)r.blur + off::kBlurRadius)     = kBoxBlurRadius;
            if (S.WidgetSetVisible) S.WidgetSetVisible(r.blur, 4 /* SelfHitTestInvisible */);
        } else if (S.WidgetSetVisible) {
            S.WidgetSetVisible(r.blur, 1 /* Collapsed */);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r.blur = nullptr; }
    // UNDER the text, which is the entire point of them.
    if (!addToScreen(w, kZOrderChat - 1, kChatPlayerLayer)) return false;
    __try {
        S.WidgetSetAlignInVp(w, pack2(0.0f, 1.0f));          // pinned by its own bottom-left, like a row
        S.WidgetSetVisible(w, 1 /* Collapsed */);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    r.w = w; r.shown = false;
    if (i == kRectPanel && g_hudLog) {
        char m[320];
        snprintf(m, sizeof(m), "[hud] chat panel from WBP_FadeIn --%s (image %s, blur %s, z=%d via %s)",
                 shape, r.img ? "yes" : "NO", r.blur ? (kBoxBlur ? "ON" : "off") : "none", kZOrderChat - 1,
                 g_addPath);
        g_hudLog(m);
    }
    return true;
}

// Show one at a rect: SLATE units for the size, PIXELS for the corner -- the same split as everything
// else here (see the note on GameHud_Chat).
static void placeRect(int i, float xPx, float bottomPx, float wS, float hS,
                      float r, float g, float b, float a) {
    const Syms& S = Get();
    if (!ensureRect(i)) return;
    Rect& R = g_rect[i];
    __try {
        pinToRect(R.img, wS, hS);
        if (R.blur && i == kRectPanel && kBoxBlur) {
            pinToRect(R.blur, wS, hS);
            // THROUGH THE SETTERS, NOT THE PROPERTIES. UBackgroundBlur::SetBlurStrength stores the
            // value AND pushes it into the live SBackgroundBlur (MyBackgroundBlur, +0x1c8); a write
            // to the property alone reaches Slate only if the widget has not been realised yet, and
            // there is no SynchronizeProperties to call afterwards the way a text block has. This
            // code realised the blur at strength ZERO so it could fade in, and then set the property
            // every frame -- so Slate's copy stayed at zero and there was never any blur at all.
            // Measured on a field screenshot: edge energy per unit brightness INSIDE the panel was
            // higher than outside it, which is the opposite of blurred.
            if (S.BlurSetRadius)   S.BlurSetRadius(R.blur, kBoxBlurRadius);
            if (S.BlurSetStrength) S.BlurSetStrength(R.blur, kBoxBlurMax * g_boxFade);
            else *(float*)((uint8_t*)R.blur + off::kBlurStrength) = kBoxBlurMax * g_boxFade;
        }
        const float tint[4] = { toLinear(r), toLinear(g), toLinear(b), a };
        S.ImageSetColor(R.img, tint);
        if (S.WidgetSetSizeInVp) S.WidgetSetSizeInVp(R.w, pack2(wS, hS));
        S.WidgetSetPosInVp(R.w, pack2(floorf(xPx + 0.5f), floorf(bottomPx + 0.5f)), true);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return; }
    if (!R.shown && S.WidgetSetVisible) {
        R.shown = true;
        __try { S.WidgetSetVisible(R.w, 3 /* HitTestInvisible */); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
}
static void hideRects() {
    const Syms& S = Get();
    g_boxFade = 0.0f;
    if (!S.WidgetSetVisible) return;
    for (Rect& R : g_rect) {
        if (!R.w || !R.shown) continue;
        R.shown = false;
        __try { S.WidgetSetVisible(R.w, 1 /* Collapsed */); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
}

// =====================================================================================================
// THE CHAT
//
// CLOSED: recent lines up the bottom-left corner, newest lowest, on the world with nothing behind
// them -- that is what keeps the screen clean while skating.
//
// OPEN: the same lines inside a panel of FIXED size, with a header, a rule, the line being typed and
// the keys that work. Fixed on purpose -- a box that resized itself around whatever had just been
// said would move the input out from under the player's eyes every time somebody spoke.
//
// One widget per visual line; the wrapping is done here (see `wrapInto`) so each row is exactly one
// line tall and the stack is exact.
//
// THE TUNING IS IN SLATE UNITS, THE PLACING IS IN PIXELS. ChatTuning's numbers are described in its
// own header as "px at 1080p-equivalent", which is exactly what a slate unit is -- so they go through
// `px()` on the way to a position and are left alone on the way to a font size or a width, which
// Slate already measures in that space.
// =====================================================================================================
// `menuUp` = the in-game pause menu is really displayed (IsPauseMenuDisplayed). The box moves to the
// RIGHT of the screen while it is, so it is not sitting on top of the menu's own rows -- and it draws
// over the menu either way, which is what kZOrderChat is for.
void GameHud_Chat(bool menuUp) {
    if (!g_cls || !g_gi) return;
    const ChatTuning& C = Chat_Tuning();
    if (!C.enabled) { hideRects(); return; }

    ChatLineView lines[kChatLines];
    const int n = Chat_Lines(lines, kChatLines);
    const bool open = Chat_IsOpen();
    // Nothing to say and nobody typing: the panel goes with the words. Hidden HERE rather than only
    // on the open-to-closed edge, because the last line can expire in the same frame the box shuts.
    if (n <= 0 && !open) { hideRects(); return; }

    const int   size   = (int)(kChatSize + 0.5f);
    // NOT `small`: windows.h (rpcndr.h) defines that as `char`, and the error it produces names
    // neither the macro nor this line.
    const int   tiny   = (int)(kChatSmall + 0.5f);
    const float lineS  = kChatSize * kLineFactor;
    const float lineH  = px(lineS);
    // Left normally; right while the pause menu is up, where the menu's own rows are not.
    const float x      = menuUp ? ((float)g_vw - px(C.marginX) - px(C.width)) : px(C.marginX);
    // The bottom edge of the LOWEST row, and the one number everything else here is measured from.
    const float yBase  = (float)g_vh - px(C.marginY) - (menuUp ? px(kPauseLiftY) : 0.0f);

    // ---- everything that was said, as visual lines, oldest first
    struct Row { const char* text; float r, g, b, a; };
    char  rowText[kChatRows][kChatColsMax + 1];
    Row   rows[kChatRows];
    int   nr = 0;
    for (int i = 0; i < n; i++) {
        const ChatLineView& l = lines[i];
        // The same fade the other surface gives a line, so switching between them does not change how
        // long anything stays readable. Age zero means the box is open: no fade at all.
        float a = 1.0f;
        const float age = (float)l.ageMs / 1000.0f;
        if (age > C.fadeAfterSec) {
            const float over = C.fadeOverSec > 0.01f ? C.fadeOverSec : 1.0f;
            a = 1.0f - (age - C.fadeAfterSec) / over;
        }
        if (a <= 0.01f) continue;
        // WHO SAID IT IS THE COLOUR OF THE WHOLE LINE. The ImGui surface tints the name and leaves
        // the words pale, which one text widget cannot do -- a second widget per line would double
        // the pool and still have to guess where the name ended, and guessing the width of a
        // proportional name would show as a ragged left edge on every message. Colouring the line
        // reads at least as well and carries the thing that actually matters: whose words these are.
        const float r = l.system ? kSysR : (l.mine ? kSayR : kNameR);
        const float g = l.system ? kSysG : (l.mine ? kSayG : kNameG);
        const float b = l.system ? kSysB : (l.mine ? kSayB : kNameB);
        for (int at = 0; at >= 0 && nr < kChatRows; ) {
            at = wrapInto(l.text, at, rowText[nr], kChatColsMax + 1);
            rows[nr].text = rowText[nr];
            rows[nr].r = r; rows[nr].g = g; rows[nr].b = b; rows[nr].a = a;
            nr++;
        }
    }

    // ---- the shape of it. Open, the history is always `histSlots` rows tall whether or not there
    // is anything in them, which is what makes the panel the same size every time it appears.
    int histSlots = open ? C.maxShownOpen : C.maxShownIdle;
    if (histSlots <= 0) histSlots = 8;
    // ...and never more rows than the chat's half of the pool can hold, or the furniture at the
    // bottom of this function would find nothing left to claim.
    if (histSlots > kSlotsChat - 8) histSlots = kSlotsChat - 8;
    if (!open && histSlots > nr) histSlots = nr;      // closed, the panel is not there to be filled
    // SCROLLING BACK. The offset is in visual lines from the newest, and THIS is where it can be
    // clamped, because this is the only place that knows how many lines there are -- so the answer
    // is written back rather than left to accumulate every time the wheel turns at the end.
    const int maxBack = nr > histSlots ? nr - histSlots : 0;
    Chat_ScrollClamp(maxBack);
    int back = open ? Chat_ScrollGet() : 0;
    if (back > maxBack) back = maxBack;
    int from = 0;
    if (nr > histSlots) from = nr - histSlots - back;     // only a window fits; `back` slides it
    if (from < 0) from = 0;
    const int upto = (from + histSlots < nr) ? from + histSlots : nr;
    const int histShown = upto - from;

    // THE VERTICAL LAYOUT, worked out in pixels from the bottom up. Explicit rather than a row index
    // times a line height, because the header and the hints are set smaller than the talk and the
    // rules need air on both sides -- three different spacings that a single row pitch cannot hold.
    const float lineT = px(kChatSmall * kLineFactor);     // the header line and the hints line
    const float gap   = px(kRuleGap);
    const float yHints  = yBase;
    const float yRuleB  = yHints - lineT - gap;
    const float yInput  = yRuleB - gap;
    const float yHist0  = yInput - lineH - gap;           // the NEWEST line, just above the rule
    const float yRuleT  = yHist0 - (float)histSlots * lineH - gap;
    const float yHeader = yRuleT - gap;

    // ---- the typed line, built before the panel because the counter below reads its length
    char compose[kChatColsMax * 2 + 8] = {0};
    if (open) {
        char typed[256];
        const int tn = Chat_Compose(typed, sizeof(typed));
        // A LONG MESSAGE SCROLLS rather than running off the right of the panel. This row does not
        // wrap -- an input that grew upward and shoved the history every time it gained a line would
        // be worse than a window -- so what is shown is the TAIL, which is where the caret is.
        const char* tail = typed;
        if (tn > kChatCols) tail = typed + (tn - kChatCols);
        snprintf(compose, sizeof(compose), "%s%s", kChatPrompt, tail);
    }

    // ---- the panel, the rules and the accent
    {
        const float target = (open && kBoxOn) ? 1.0f : 0.0f;
        const float step = (kBoxFadeSec > 0.01f) ? (g_frameDt / kBoxFadeSec) : 1.0f;
        if (g_boxFade < target)      g_boxFade = (g_boxFade + step > target) ? target : g_boxFade + step;
        else if (g_boxFade > target) g_boxFade = (g_boxFade - step < target) ? target : g_boxFade - step;
    }
    if (g_boxFade > 0.01f) {
        const float f  = g_boxFade;
        const float wS = C.width + kBoxPadX * 2.0f;                       // FIXED, every time
        const float panelBottom = yBase + px(kBoxPadY);
        const float panelTop    = yHeader - lineT - px(kBoxPadY);
        placeRect(kRectPanel, x - px(kBoxPadX), panelBottom, wS, slateOf(panelBottom - panelTop),
                  kBoxR, kBoxG, kBoxB, kBoxAlpha * f);
        // A rule under the header and another above the input: it is most of what makes a box read
        // as a box rather than as text that happens to have something behind it.
        placeRect(kRectRuleTop, x, yRuleT, C.width, kRuleThick, 1.0f, 1.0f, 1.0f, kRuleAlpha * f);
        placeRect(kRectRuleBot, x, yRuleB, C.width, kRuleThick, 1.0f, 1.0f, 1.0f, kRuleAlpha * f);
        // ...and the lit edge down the side of the input, bright while there is something to send.
        placeRect(kRectAccent, x - px(kAccentW + 5.0f), yInput, kAccentW, kChatSize * kLineFactor,
                  kSayR, kSayG, kSayB, (Chat_TypedLen() > 0 ? 1.0f : 0.35f) * f);
    } else {
        hideRects();
    }

    // ---- the header: what this is, and who is here
    if (open) {
        Slot* s = claim(hashOf("hdr", kLaneChatHdr), true, g_cls, g_gi, &g_made);
        if (s) {
            style(*s, tiny, rgba(kDimR, kDimG, kDimB, 1.0f), 0.0f);
            align(*s, 0.0f, 1.0f);
            // It says when it is NOT showing the newest, because a box that quietly stopped
            // following the conversation is the kind of thing a player blames on the mod.
            say(*s, back > 0 ? "SESSION CHAT  (SCROLLED)" : "SESSION CHAT");
            place(*s, x, yHeader);
            showIt(*s, true);
            g_drawn++;
        }
        Slot* o = claim(hashOf("online", kLaneChatOnline), true, g_cls, g_gi, &g_made);
        if (o) {
            char who[40];
            // Everybody, not everybody else: "1 ONLINE" alone in a session is the true answer and
            // the one a player expects to see.
            snprintf(who, sizeof(who), "%d ONLINE", Chat_Presence() + 1);
            style(*o, tiny, rgba(kDimR, kDimG, kDimB, 1.0f), 0.0f);
            align(*o, 1.0f, 1.0f);                       // pinned by its RIGHT edge, at the far side
            say(*o, who);
            place(*o, x + px(C.width), yHeader);
            showIt(*o, true);
            g_drawn++;
        }
    }

    // ---- what has been said, newest nearest the input, so this walks BACKWARDS up the screen
    for (int i = upto - 1; i >= from; i--) {
        const Row& row = rows[i];
        const int k = upto - 1 - i;                      // 0 = the lowest row on screen
        // Keyed by the row's place ON SCREEN, not by its index in the list: the list shifts every
        // time a line arrives or expires, and a key that shifted with it would move every row onto a
        // different widget for no reason.
        Slot* s = claim(hashOf("line", kLaneChat + k), true, g_cls, g_gi, &g_made);
        if (s) {
            style(*s, size, rgba(row.r, row.g, row.b, row.a), 0.0f);
            align(*s, 0.0f, 1.0f);
            say(*s, row.text);
            place(*s, x, yHist0 - (float)k * lineH);
            showIt(*s, true);
            g_drawn++;
        }
        if (yHist0 - (float)k * lineH < lineH) break;    // ran off the top of the screen
    }
    // An empty history says so, rather than leaving a panel that looks like it failed to load.
    if (open && histShown == 0) {
        Slot* s = claim(hashOf("empty", kLaneChatEmpty), true, g_cls, g_gi, &g_made);
        if (s) {
            style(*s, size, rgba(kDimR, kDimG, kDimB, 0.75f), 0.0f);
            align(*s, 0.0f, 1.0f);
            say(*s, "Nothing said yet.");
            place(*s, x, yHist0);
            showIt(*s, true);
            g_drawn++;
        }
    }

    // ---- the line being typed
    if (open) {
        Slot* s = claim(hashOf("compose", kLaneChatIn), true, g_cls, g_gi, &g_made);
        if (s) {
            style(*s, size, rgba(kSayR, kSayG, kSayB, 1.0f), 0.0f);
            align(*s, 0.0f, 1.0f);
            say(*s, compose);
            place(*s, x, yInput);
            showIt(*s, true);
            g_drawn++;
        }
    }

    // ---- the keys that work, and how much room is left
    if (open) {
        Slot* s = claim(hashOf("hints", kLaneChatHints), true, g_cls, g_gi, &g_made);
        if (s) {
            style(*s, tiny, rgba(kDimR, kDimG, kDimB, 1.0f), 0.0f);
            align(*s, 0.0f, 1.0f);
            say(*s, "ENTER send    ESC close    UP last line    WHEEL scroll");
            place(*s, x, yHints);
            showIt(*s, true);
            g_drawn++;
        }
        // The counter appears only once it starts to matter, and turns when there is no room left --
        // a number that is always there stops being read.
        const int len = Chat_TypedLen(), maxLen = Chat_TypedMax();
        if (len >= maxLen - kCountWarnAt) {
            Slot* c = claim(hashOf("count", kLaneChatCount), true, g_cls, g_gi, &g_made);
            if (c) {
                char t[24]; snprintf(t, sizeof(t), "%d/%d", len, maxLen);
                const bool full = len >= maxLen;
                style(*c, tiny, full ? rgba(kWarnR, kWarnG, kWarnB, 1.0f)
                                      : rgba(kDimR, kDimG, kDimB, 1.0f), 0.0f);
                align(*c, 1.0f, 1.0f);
                say(*c, t);
                place(*c, x + px(C.width), yHints);
                showIt(*c, true);
                g_drawn++;
            }
        }
    }
}

// =====================================================================================================
// LIFETIME
// =====================================================================================================
void GameHud_Forget() {
    // The world went and took the viewport with it. NOTHING here may be dereferenced -- see game_hud.h.
    for (Rect& r : g_rect) r = Rect{};
    g_rectTried = 0; g_boxFade = 0.0f;
    for (Slot& s : g_slot) s = Slot{};
    g_built = 0; g_lastDrawn = 0; g_fade = 0.0f; g_lastUs = 0; g_world = nullptr;
}

void GameHud_Clear(void* world) {
    // Only a world we still recognise may be reached into. Anything else is the forget path, which
    // is always safe because it dereferences nothing.
    if (!world || !g_world || world != g_world) { GameHud_Forget(); return; }
    const Syms& S = Get();
    for (Rect& r : g_rect) {
        void* b = r.w;
        r = Rect{};
        if (b && S.WidgetRemoveParent) __try { S.WidgetRemoveParent(b); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    g_rectTried = 0; g_boxFade = 0.0f;
    for (Slot& s : g_slot) {
        void* w = s.w;
        s = Slot{};
        if (w && S.WidgetRemoveParent) __try { S.WidgetRemoveParent(w); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    g_built = 0; g_lastDrawn = 0; g_fade = 0.0f; g_lastUs = 0; g_world = nullptr;
}

// Every way this can come to nothing looks identical on screen -- no names -- so each is said in
// words rather than left to be guessed at: switched off, the class never turned up, or it is running
// and simply had nothing to draw.
int GameHud_Status(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    const char* what = !g_enabled            ? "off (the overlay owns the names)"
                     : !GameHud_Available()  ? "no widget class -- nothing can be drawn this way"
                                             : "game-drawn";
    return snprintf(out, (size_t)cap, "[hud] %s built=%d drawn=%d made=%d fade=%.2f font=%s %s",
                    what, g_built, g_lastDrawn, g_lastMade, g_fade,
                    // Which cut of the menu font the plates ended up in. "bold" with kRegularWeight
                    // on means the lighter one was not resident -- the look is the only casualty.
                    !kRegularWeight ? "bold (asked)" : (g_regularFont ? "regular" : "bold (not found)"),
                    g_faults ? "  FAULTS" : "");
}

} }  // namespace omp::ui
