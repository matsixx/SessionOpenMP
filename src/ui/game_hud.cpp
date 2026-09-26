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
#include "chat.h"
#include "mp_prefs.h"                        // the chat is this surface's other user
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
// YOU. The game's highlight amber, opened up: the theme's 0.98/0.78/0.22 read as a deep mustard
// against a dark panel, where the overlay's version of the same colour had looked brighter.
float kSayR  = 1.00f, kSayG  = 0.87f, kSayB  = 0.42f;
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
// HOW MANY CHARACTERS FIT ON A LINE is not a constant -- it falls out of how wide the box is and how
// big the text is, and BOTH are the player's to set now. It was 66, which is what the default width
// and the default size happen to give; widen the box or enlarge the text and the wrap went on
// breaking at 66 anyway, so long lines ran straight out of the panel (field 2026-09-21). Derived
// per frame by `chatCols` instead, from the same kCharWEm the panels are sized with, so the box and
// the wrapping cannot disagree about how wide a character is.
// The BUFFER is fixed and the knob is clamped into it, because a live static cannot size an array
// and the whole point of the knob is that it can be corrected without a rebuild.
// The widest box at the smallest text asks for ~190 columns, so the ceiling has to clear that or
// the widest setting would wrap narrower than the box it was given.
enum { kChatRows = 128, kChatColsMax = 200 };
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
// ---- SPEECH BUBBLES. The gap is in font sizes and is measured from the top of the NAME to the
// bottom of the bubble's panel, so the two never look like one block of text (field: "it's too close
// to their name. It's also not much of a chat bubble anymore").
float kBubbleGap   = 1.05f;
// A MESSAGE IS BODY TEXT; A NAME IS A LABEL. The bubble used to be drawn at exactly the name's size,
// distance boost and all -- which at the 1.3x close-range clamp made it the biggest text anywhere in
// the mod, bigger than the chat box it is quoting. This brings it to about the chat's own size at the
// reference distance, and it still scales with distance like the plate it hangs over.
// The PLAYER sets this (Player names -> Chat bubble text size); the multiplier is gone. It is the
// size at the reference distance and still scales with distance from there, like the plate it hangs
// over.
float kBubbleSizeMul = 0.85f;   // unused; kept so an old ini that mentions it does not look wrong
int   kBubbleCols  = 26;
enum { kBubbleMaxLines = 4 };  // past this a sentence is cut: a paragraph over a head is not readable       // characters per line before it wraps -- wrapped HERE, see `wrapInto`
// Room inside the panel, in slate units. TOP AND BOTTOM ARE NOT THE SAME, because the text is placed
// by its BOTTOM edge and a text block's box carries the font's descender space below the glyphs --
// so an even padding lands low, and the ascenders come out of the top of the panel while the bottom
// has room to spare. Measured off a field screenshot rather than reasoned about: the caps were
// crossing the top edge with a clear gap underneath.
float kBubblePadX = 9.0f, kBubblePadTop = 10.0f, kBubblePadBottom = 3.0f;
// A THIN FRAME AROUND THE BUBBLE, drawn with the game's own `Border` texture 9-sliced -- the object
// placement UI's selection frame: 111x109, a one-pixel light rule round the edge with L brackets
// inset at the corners, hollow in the middle. Sliced at kFrameMargin of its width the corners keep
// whatever falls inside the margin and the edges stretch the rule, so a small margin gives a plain
// rectangle and a large one keeps the brackets. Square corners either way -- see the note on
// `borderTexture` for why there is no rounded one to reach for.
bool  kBubbleFrame  = false;
// THE NAME HAS NO BOX. It was given one to match the reference picture and it did not look good in
// the field -- a box round every name on screen is a lot of boxes, where a box round the occasional
// sentence is not. The name moves to the bubble's top-left corner instead whenever there is a bubble
// to sit on, which is the part of that picture worth having.
//
// THE TAIL is a square rotated 45 degrees, hung under the panel so the part below the edge is a
// downward triangle. There is no triangle in the game's UI textures and there is no clipping to be
// had, so the overlap is kept small rather than hidden: `kTailDrop` is how far the diamond's CENTRE
// sits below the panel, and at the default only a sliver of it is behind the panel, where two
// translucent layers would otherwise show as a darker patch.
bool  kBubbleTail   = true;
float kTailSize     = 13.0f;    // the square's side, slate units
float kTailDrop     = 4.0f;     // its centre, this far below the panel's bottom edge
float kFrameMargin  = 0.03f;                       // 0.03 = the plain rule; ~0.11 keeps the brackets
float kFrameR = 1.0f, kFrameG = 1.0f, kFrameB = 1.0f;
float kFrameAlpha   = 0.28f;
bool  kBubblePanel = true;     // the dark panel behind it, the chat box's in miniature
// NO FROST ON A BUBBLE. The chat box is one big surface and the blur earns its keep there; a bubble
// is a couple of words over a head, where a plain dark panel reads just as well -- and a blur is a
// real per-instance cost, so N people talking would have meant N of them. Dropping it is also what
// lets there be one panel per peer instead of a pool small enough to bound the cost.
bool  kBubbleBlur  = false;
// A BUBBLE'S BLUR IS ITS OWN, not the chat box's. They used to share kBoxBlurMax/kBoxBlurRadius, so
// turning the box's blur up turned every bubble's up with it -- and a bubble is a tenth the size, so
// the box's radius on one reads as a smeared square rather than frosted glass. Weaker and tighter.
// A BUBBLE'S DARKNESS IS ITS OWN. It used to borrow the chat box's kBoxAlpha, so the two could not
// be set apart -- and they want different answers: the box is a slab you read a conversation off,
// a bubble is a label hanging over somebody's head in the world.
float kBubbleAlpha      = 0.20f;
float kBubbleBlurMax    = 17.0f;
int   kBubbleBlurRadius = 14;
// HOW WIDE A CHARACTER IS, as a fraction of the font size. Nothing on this side can measure a
// proportional font, so a panel that has to fit text is sized from a count and this number. Taken
// from the chat's own wrap (620 slate units over 66 characters at size 15), so the two agree.
float kCharWEm     = 0.63f;
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
       kLaneChatEmpty = 95, kLaneChatName = 200, kLaneObjTag = 300 };
// The pool is two zones, because the two halves are added to the viewport at different depths and a
// slot cannot change its mind afterwards. Names first, then the chat's rows and its furniture.
// A name and up to a few wrapped bubble lines per peer. Slots cost nothing until something claims
// one -- a widget is built on demand -- so this is a ceiling, not an allocation.
// THREE ZONES, because a widget's depth is fixed when it is added to the viewport and a slot cannot
// change its mind afterwards. Floating names under the menus; the chat above them; and the chat's
// NAME OVERLAYS above the chat -- an overlay that shares a depth with the line it has to cover is
// ordered by which of the two happened to be built first, which is not something to rely on. (It was
// relied on, and the field found it: a name would come out white, because the plain line had been
// created after the overlay and drew over it.)
enum { kSlotsName     = OMP_MAX_PEERS * 4,
       kSlotsChat     = 32,
       kSlotsChatName = 16,
       kSlots         = kSlotsName + kSlotsChat + kSlotsChatName };
enum { kZoneName = 0, kZoneChat = 1, kZoneChatName = 2 };

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
static int wrapInto(const char* text, int from, char* out, int cap, int wantCols) {
    const int len = (int)strlen(text);
    int cols = wantCols; if (cols < 8) cols = 8; if (cols > cap - 1) cols = cap - 1;
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
static Slot* claim(uint64_t key, int zone, void* cls, void* gi, int* madeThisFrame) {
    const int lo = (zone == kZoneName) ? 0
                 : (zone == kZoneChat) ? kSlotsName
                                       : kSlotsName + kSlotsChat;
    const int hi = (zone == kZoneName) ? kSlotsName
                 : (zone == kZoneChat) ? kSlotsName + kSlotsChat
                                       : kSlots;
    const int z  = (zone == kZoneName) ? kZOrder
                 : (zone == kZoneChat) ? kZOrderChat
                                       : kZOrderChat + 1;   // the overlay, over the line it tints
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && s.key == key && !s.claimed) { s.claimed = true; return &s; } }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && !s.claimed && !s.shown) { s.claimed = true; s.key = key; return &s; } }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w || *madeThisFrame >= kMakePerFrame) continue;
        (*madeThisFrame)++;
        if (!build(s, cls, gi, z, zone != kZoneName && kChatPlayerLayer)) return nullptr;
        s.claimed = true; s.key = key;
        return &s; }
    for (int i = lo; i < hi; i++) { Slot& s = g_slot[i];
        if (s.w && !s.claimed) { s.claimed = true; s.key = key; return &s; } }
    return nullptr;
}

// ---- the frame ------------------------------------------------------------------------------------
// What Begin works out and the rest of the frame borrows. Not statics with a longer life than a
// frame: if Begin said no, none of this is valid and nothing may run.
static void* g_cls = nullptr;
static void* g_gi  = nullptr;
static int   g_vw = 0, g_vh = 0, g_made = 0, g_drawn = 0;
static void* g_frameWorld = nullptr;

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

// The chat box's four, then one per peer for the speech bubbles -- they carry no blur (see
// kBubbleBlur), so a panel is a tinted quad and there is no reason to ration them. They cost nothing
// until somebody talks: a rect builds its widget the first time it is asked for, not before.
// The chat box's four, then FOUR BANKS of one-per-peer: a fill and a frame for each speech bubble,
// and the same again for each name. None of them carry a blur, so a rect is a tinted quad, and none
// of them exist until something asks for one -- a rect builds its widget the first time it is placed.
enum { kRectPanel = 0, kRectRuleTop, kRectRuleBot, kRectAccent,
       kRectBubble0, kRectBubbles = OMP_MAX_PEERS,
       kRectFrame0  = kRectBubble0 + kRectBubbles,          // the frame over each bubble's fill
       kRectTail0   = kRectFrame0  + kRectBubbles,          // ...and the pointer under it
       kRectCount   = kRectTail0   + kRectBubbles };

// WHICH BLUR THIS RECT ANSWERS TO. The chat box and a speech bubble are set separately: one panel
// against one per talking player, so they get their own strength, their own radius and their own off.
static bool  rectWantsBlur (int i) {
    if (i == kRectPanel) return kBoxBlur;
    // THE FILL ONLY, never the frame or the tail. A blur child is NOT rotated with its image, so the
    // tail -- a square turned 45 degrees into a diamond -- carried an axis-aligned blur square around
    // itself. The frame and the tail also sit on glass the fill has already frosted, so blurring them
    // is three blurs per bubble paying for one.
    return (i >= kRectBubble0 && i < kRectFrame0) && kBubbleBlur;
}
static float rectBlurMax   (int i) { return (i == kRectPanel) ? kBoxBlurMax : kBubbleBlurMax; }
static int   rectBlurRadius(int i) { return (i == kRectPanel) ? kBoxBlurRadius : kBubbleBlurRadius; }
// `blurOn` is what we last told the blur child -- -1 nothing yet, 0 collapsed, 1 shown -- so the
// per-frame check costs a compare instead of an engine call.
struct Rect { void* w = nullptr; void* img = nullptr; void* blur = nullptr; bool shown = false;
              int blurOn = -1; };
static Rect g_rect[kRectCount];
static int  g_rectTried = 0;
static float g_boxFade = 0.0f;       // 0 = gone, 1 = fully up; eased by the frame clock

// THE FRAME'S TEXTURE. `Border` is the object-placement UI's selection frame and it is the only
// thing in the game's UI textures that is a frame at all -- everything else under Menus/Textures is
// a button glyph or a slider bar. It is SQUARE-CORNERED, so the rounded-corner look a chat bubble
// usually has is not available from the game's own assets; getting that would mean building a
// UTexture2D at runtime, which is a different and much larger job.
//
// Found, never loaded -- if the object-placement UI has not been up this session the frame simply
// does not appear, and a bubble without its frame is the bubble we had before.
static const wchar_t* const kBorderTexPath =
    L"/Game/ObjectPlacement/UI/Textures/UI/Border.Border";
static void* g_borderTex = nullptr;
static bool  g_borderLooked = false;
// The earliest the texture may be LOADED (as opposed to found). Pushed out whenever the cache is
// dropped, so a load never lands in the frames around a map change.
static uint64_t g_borderTryAtMs = 0;
unsigned kAssetSettleMs = 3000;
static const char* g_borderHow = "not looked for yet";
static void* borderTexture() {
    const Syms& S = Get();
    if (g_borderLooked) return g_borderTex;
    if (!S.StaticFindObject) return nullptr;
    g_borderLooked = true;
    __try { g_borderTex = S.StaticFindObject(nullptr, (void*)(intptr_t)-1, kBorderTexPath, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_borderTex = nullptr; }
    if (g_borderTex) { g_borderHow = "already resident"; return g_borderTex; }
    // NOT DURING A LEVEL CHANGE. What follows is a SYNCHRONOUS package load on the game thread, and
    // the cache is dropped whenever the widget pool empties -- which is exactly what a map change
    // does. So without this the load lands in the worst possible frame, and a hard freeze on a map
    // switch was reported once and could not be reproduced. Unproven as the cause, but a synchronous
    // load in a level transition is worth moving out of the way whether or not it was this one.
    if (GetTickCount64() < g_borderTryAtMs) { g_borderLooked = false; return nullptr; }
    // LOAD IT. "Found, never loaded" is the rule for a CLASS, where loading a UI package at an
    // arbitrary moment can run construction script and cost a hitch. This is a 111x109 texture with
    // no behaviour attached, and the alternative is the frame never appearing unless the player
    // happens to have opened the object dropper -- so this one is worth loading outright.
    //
    // An FSoftObjectPath is FName + FString = 24 bytes (game_syms.h says so, from the TSoftObjectPtr
    // layout), so it is built by hand: the name in the first 8, the string left empty.
    if (S.SoftPathTryLoad && S.FNameCtor) {
        __try {
            uint8_t path[24] = {0};
            char ansi[128];
            int k = 0;
            for (; kBorderTexPath[k] && k < (int)sizeof(ansi) - 1; k++) ansi[k] = (char)kBorderTexPath[k];
            ansi[k] = 0;
            S.FNameCtor(path, ansi, 1 /* FNAME_Add */);
            memset(path + 8, 0, sizeof(path) - 8);      // the FString must be empty, whatever the ctor wrote
            g_borderTex = S.SoftPathTryLoad(path, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_borderTex = nullptr; }
    }
    g_borderHow = g_borderTex ? "loaded on demand" : "NOT FOUND -- no frame";
    return g_borderTex;
}

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
    // Which BANK this rect belongs to decides both its depth and whether it carries the frame
    // brush, so it is worked out once, here, above the first thing that asks.
    const bool isFrame = (i >= kRectFrame0 && i < kRectTail0);
    // THESE TWO ARE WRITTEN WHETHER OR NOT THE BLUR IS WANTED, because they are the only ones that
    // CANNOT be changed afterwards: a property write reaches Slate only before the widget is
    // realised, and a blur has no SynchronizeProperties to call later the way a text block has.
    // They are inert while the blur is collapsed, so writing them always costs nothing and means a
    // blur switched on later is correctly configured instead of half-built.
    // Everything that CAN change -- whether it shows at all, its radius, its strength -- moved to
    // placeRect. It used to be decided here, once, and a bubble built while "Bubble blur" was 0
    // (the default) kept a collapsed blur for the rest of the session: turning the slider up did
    // nothing, for ever, which is exactly how it was reported.
    if (r.blur) __try {
        *(uint8_t*)((uint8_t*)r.blur + off::kBlurApplyAlpha) = 0;   // full strength, whatever the tint
        *(uint8_t*)((uint8_t*)r.blur + off::kBlurAutoRadius) = 1;   // ...and OUR radius, not a derived one
        *(int32_t*)((uint8_t*)r.blur + off::kBlurRadius)     = rectBlurRadius(i);
    } __except (EXCEPTION_EXECUTE_HANDLER) { r.blur = nullptr; }
    // THE BRUSH IS SET BEFORE THE WIDGET IS REALISED, so Slate picks it up when it builds the image
    // -- the same rule as the blur's radius, and the reason no setter has to be signatured for it.
    // DrawAs = Box is the 9-slice: corners kept, edges stretched.
    // NOT gated on kBubbleFrame. A frame rect built while the border was switched off never got the
    // nine-slice texture, and a brushless image draws as a SOLID QUAD -- so turning the border back
    // on painted a filled block over the bubble instead of a bracketed edge (reported as the bubble
    // going 'a lot darker'). The brush is inert while the rect is hidden; visibility is what decides
    // whether a border shows, not whether it was configured.
    if (isFrame) {
        void* tex = borderTexture();
        if (tex) __try {
            uint8_t* br = (uint8_t*)r.img + off::kImageBrush;
            *(void**)  (br + off::kBrushResource) = tex;
            *(uint8_t*)(br + off::kBrushDrawAs)   = 1;            // ESlateBrushDrawType::Box
            float* m = (float*)(br + off::kBrushMargin);
            m[0] = m[1] = m[2] = m[3] = kFrameMargin;             // left, top, right, bottom
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
    // THE TAIL IS THE SAME SQUARE, TURNED. RenderTransform is written before the widget is realised
    // like everything else here, so Slate picks the angle up when it builds.
    // AND THE PIVOT WITH IT, which rotating about the centre is what makes a square a diamond rather
    // than a lever. It was only ever assumed to be the centre; it is, on this asset, but the square
    // reported next to the tail was not this -- it was the blur child, which is not rotated with the
    // image (see rectWantsBlur). Written anyway: one store against an assumption about somebody
    // else's blueprint.
    if (i >= kRectTail0) __try {
        *(float*)((uint8_t*)r.img + off::kWidgetRenderXform + off::kXformAngle) = 45.0f;
        float* piv = (float*)((uint8_t*)r.img + off::kWidgetRenderPivot);
        piv[0] = 0.5f; piv[1] = 0.5f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    // UNDER the text, which is the entire point of them.
    // DEPTH BELONGS TO WHAT THE RECT IS FOR. The chat's furniture goes just under the chat, over the
    // menus; a speech bubble's panel goes just under the NAMES, under the menus -- it hangs in the
    // world over somebody's head and has no business on top of a pause screen.
    // Getting this wrong put every bubble panel at the chat's depth, which is both why the panels
    // showed over the pause menu and why the words looked grey: the panel was drawing OVER its own
    // text, tinting it 48% black.
    const bool chatRect = (i < kRectBubble0);
    // Fill UNDER frame UNDER text, and all three under the menus. A frame drawn beneath its own fill
    // is invisible, and equal z-orders resolve by insertion order, which is not something to rely on.
    const int z = chatRect ? (kZOrderChat - 1)
                : isFrame  ? (kZOrder - 1)      // the frame
                           : (kZOrder - 2);     // the fill it sits on
    if (!addToScreen(w, z, chatRect && kChatPlayerLayer)) return false;
    __try {
        S.WidgetSetAlignInVp(w, pack2(0.0f, 1.0f));          // pinned by its own bottom-left, like a row
        S.WidgetSetVisible(w, 1 /* Collapsed */);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    r.w = w; r.shown = false;
    if (i == kRectPanel && g_hudLog) {
        char m[320];
        snprintf(m, sizeof(m),
                 "[hud] chat panel from WBP_FadeIn --%s (image %s, blur %s, z=%d via %s); bubble frame: %s",
                 shape, r.img ? "yes" : "NO", r.blur ? (kBoxBlur ? "ON" : "off") : "none", kZOrderChat - 1,
                 g_addPath, (borderTexture(), g_borderHow));
        g_hudLog(m);
    }
    return true;
}

// Show one at a rect: SLATE units for the size, PIXELS for the corner -- the same split as everything
// else here (see the note on GameHud_Chat).
// The nine-slice, applied late. borderTexture() can still be loading when the first bubble goes up,
// and the setting can be switched on at any time -- so a frame rect checks once per frame whether it
// is still a plain quad and fixes itself when the texture is there. Cheap: one pointer compare.
static void ensureFrameBrush(Rect& R) {
    if (!R.img) return;
    void* tex = borderTexture();
    if (!tex) return;
    __try {
        uint8_t* br = (uint8_t*)R.img + off::kImageBrush;
        if (*(void**)(br + off::kBrushResource) == tex) return;     // already ours
        *(void**)  (br + off::kBrushResource) = tex;
        *(uint8_t*)(br + off::kBrushDrawAs)   = 1;                  // ESlateBrushDrawType::Box
        float* m = (float*)(br + off::kBrushMargin);
        m[0] = m[1] = m[2] = m[3] = kFrameMargin;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}

static void placeRect(int i, float xPx, float bottomPx, float wS, float hS,
                      float r, float g, float b, float a) {
    const Syms& S = Get();
    if (!ensureRect(i)) return;
    Rect& R = g_rect[i];
    if (i >= kRectFrame0 && i < kRectTail0) ensureFrameBrush(R);
    __try {
        pinToRect(R.img, wS, hS);
        // EVERY FRAME, NOT AT BIRTH. Whether a blur shows is a setting the player can move at any
        // moment; deciding it once when the widget was built is why turning bubble blur on did
        // nothing until the level changed.
        const bool blurThis = rectWantsBlur(i);
        if (R.blur && S.WidgetSetVisible && R.blurOn != (blurThis ? 1 : 0)) {
            R.blurOn = blurThis ? 1 : 0;
            S.WidgetSetVisible(R.blur, blurThis ? 4 /* SelfHitTestInvisible */ : 1 /* Collapsed */);
        }
        if (R.blur && blurThis) {
            pinToRect(R.blur, wS, hS);
            // THROUGH THE SETTERS, NOT THE PROPERTIES. UBackgroundBlur::SetBlurStrength stores the
            // value AND pushes it into the live SBackgroundBlur (MyBackgroundBlur, +0x1c8); a write
            // to the property alone reaches Slate only if the widget has not been realised yet, and
            // there is no SynchronizeProperties to call afterwards the way a text block has. This
            // code realised the blur at strength ZERO so it could fade in, and then set the property
            // every frame -- so Slate's copy stayed at zero and there was never any blur at all.
            // Measured on a field screenshot: edge energy per unit brightness INSIDE the panel was
            // higher than outside it, which is the opposite of blurred.
            // A BUBBLE HAS ITS OWN FADE, not the chat box's: it is put up and taken down by what
            // somebody said, and g_boxFade is about the box being open.
            const float bf = (i == kRectPanel) ? g_boxFade : (a > 0.0f ? 1.0f : 0.0f);
            if (S.BlurSetRadius)   S.BlurSetRadius(R.blur, rectBlurRadius(i));
            if (S.BlurSetStrength) S.BlurSetStrength(R.blur, rectBlurMax(i) * bf);
            else *(float*)((uint8_t*)R.blur + off::kBlurStrength) = rectBlurMax(i) * bf;
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
// The CHAT's rects only -- the bubbles are hidden by the names pass, which is the only thing that
// knows whether anybody is still talking.
static void hideRects() {
    const Syms& S = Get();
    g_boxFade = 0.0f;
    if (!S.WidgetSetVisible) return;
    for (int i = 0; i < kRectBubble0; i++) {
        Rect& R = g_rect[i];
        if (!R.w || !R.shown) continue;
        R.shown = false;
        __try { S.WidgetSetVisible(R.w, 1 /* Collapsed */); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
    }
}
// One rect down, for a piece the player has turned off. A rect that is simply not placed this frame
// keeps whatever it was showing last frame, so "off" has to be said out loud.
static void hideRect(int i) {
    const Syms& S = Get();
    if (!S.WidgetSetVisible || i < 0 || i >= kRectCount) return;
    Rect& R = g_rect[i];
    if (!R.w || !R.shown) return;
    R.shown = false;
    __try { S.WidgetSetVisible(R.w, 1 /* Collapsed */); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
}
// A dark box with the game's frame over it.
// THE TWO PIECES ARE INDEPENDENT: the player can have the panel, the border, both or neither, so
// each is placed or collapsed on its own. The frame used to ride on the fill being drawn at all,
// which made "no panel" silently mean "no border either" -- two settings, one of them a lie.
static void placeFramedBox(int fillIdx, int frameIdx, float xPx, float bottomPx,
                           float wS, float hS, float alpha) {
    if (kBubblePanel) placeRect(fillIdx, xPx, bottomPx, wS, hS, kBoxR, kBoxG, kBoxB, kBubbleAlpha * alpha);
    else              hideRect(fillIdx);
    if (kBubbleFrame && borderTexture())
        placeRect(frameIdx, xPx, bottomPx, wS, hS, kFrameR, kFrameG, kFrameB, kFrameAlpha * alpha);
    else hideRect(frameIdx);
}

static void hideBubbleRects(int fromIndex) {
    const Syms& S = Get();
    if (!S.WidgetSetVisible) return;
    // Every bank a bubble uses: its fill, its frame and its tail.
    const int base[3] = { kRectBubble0, kRectFrame0, kRectTail0 };
    for (int pass = 0; pass < 3; pass++) {
        for (int i = base[pass] + fromIndex; i < base[pass] + kRectBubbles; i++) {
            Rect& R = g_rect[i];
            if (!R.w || !R.shown) continue;
            R.shown = false;
            __try { S.WidgetSetVisible(R.w, 1 /* Collapsed */); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
        }
    }
}

// =====================================================================================================
// THE FLOATING NAMES
// The alphas, the distance fade and the perspective scale are deliberately the SAME arithmetic as the
// ImGui path (nameplates.cpp) -- the two surfaces have to agree about when a name is visible, or
// switching between them would look like a bug in whichever one was on.
// =====================================================================================================
// Declared with the rest of the lifetime code; used here, which is where the invariant lives.
static void forgetCachedAssets();

bool GameHud_Begin(void* world) {
    g_cls = nullptr; g_gi = nullptr; g_vw = g_vh = 0; g_made = 0; g_drawn = 0; g_frameWorld = nullptr;
    if (!g_enabled) return false;
    // NO WIDGETS MEANS NO REFERENCES, AND NO REFERENCES MEANS THE CACHED ASSETS ARE SUSPECT. The font
    // and the border texture are only kept alive by our own widgets pointing at them, so an empty
    // pool is exactly the state in which the collector is free to take them. Stating it here makes it
    // structural instead of something every teardown path has to remember -- and a teardown path that
    // forgets is what the crash was.
    if (g_built == 0) forgetCachedAssets();
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

// WHOSE OBJECT THIS IS. One widget per tagged object, keyed by the object so it keeps its widget as
// the camera moves; the nameplates' zone (under the menus, and names are claimed first, so a full
// pool costs a tag, never a name) and their distance scale, a little smaller and in the message white
// so it never reads as somebody standing there. Unclaimed frames take them down in End.
void GameHud_Tag(const char* text, uint32_t key, float nx, float ny, float distCm) {
    if (!g_cls || !g_gi || !text || !*text || g_vw <= 0 || g_vh <= 0) return;
    const NameplateTuning& T = Nameplates_Tuning();
    float scale = (distCm > 1.0f) ? (T.refDistCm / distCm) : T.maxScale;
    if (scale < T.minScale) scale = T.minScale;
    if (scale > T.maxScale) scale = T.maxScale;
    int size = (int)((float)MpPrefs_NameTextSize() * 0.8f * scale + 0.5f);
    if (size < 6) size = 6;
    Slot* s = claim(hashOf("objtag", kLaneObjTag + (int)(key & 0x00ffffff)), kZoneName, g_cls, g_gi, &g_made);
    if (!s) return;
    style(*s, size, rgba(kMsgR, kMsgG, kMsgB, 0.95f), 0.0f);
    align(*s, 0.5f, 1.0f);
    say(*s, text);
    place(*s, nx * (float)g_vw, ny * (float)g_vh);
    showIt(*s, true);
    g_drawn++;
}

void GameHud_End() {
    if (!g_enabled) return;
    // Everything nobody claimed goes away. Collapsed, not removed: the widget is kept for whoever
    // needs it next, and building one is the only part of this that costs anything.
    // It runs even when Begin said no -- as long as there IS a world, because the reason Begin
    // refused may be that the names were switched off, and their plates still have to come down.
    if (g_frameWorld) for (Slot& s : g_slot) if (s.w && !s.claimed) { showIt(s, false); s.key = 0; }
    // A frame that could not draw at all -- no class, no game instance -- still has to take the
    // furniture down, or the last panel drawn hangs there with nothing in it.
    if (g_frameWorld && !g_cls) { hideRects(); hideBubbleRects(0); }
    g_lastDrawn = g_drawn; g_lastMade = g_made;
    if (g_built > 0 && g_frameWorld) g_world = g_frameWorld;   // what the pool now belongs to
}

void GameHud_Names(const NameplateItem* items, int n, bool show) {
    if (!g_cls || !g_gi) return;
    int nBubblePanels = 0;       // how many of each bank have been handed out this frame
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
            // The player's size, not kBaseSize (which stays as the default it always was). The bubble
            // above is placed from this same `size`, so it stays seated on a bigger or smaller name.
            int size = (int)((float)MpPrefs_NameTextSize() * scale + 0.5f);
            if (size < 6) size = 6;

            const float cx = it.x * (float)vw;
            const float hy = it.y * (float)vh;
            // PIXELS. `size` is a font size in slate units and `hy` is a pixel, and these used to be
            // added to each other -- which is exactly right at a DPI scale of 1 and too small at
            // every other one, so the bubble sat closer to the name the higher the resolution.
            const float lineH = px((float)size * kLineFactor);

            // ---- WHAT THEY SAID, WORKED OUT FIRST, because the name hangs off it. The bubble sits
            // ON TOP of where the name would be whether or not the name is showing, so mounting a
            // board does not make an open bubble jump down the screen.
            //
            // It is wrapped HERE, never by Slate (see `wrapInto`), one widget per line, on a dark
            // panel of its own -- which is what makes a sentence over somebody's shoulder readable
            // against a bright wall, and what stops it reading as a second line of their name.
            char  bl[kBubbleMaxLines][kChatColsMax + 1];
            int   nLines = 0, cols = 0, bsize = 0;
            float panelLeft = 0.0f, panelTop = 0.0f, panelBottom = 0.0f, bubBottom = 0.0f;
            float bubLineH = 0.0f, wS = 0.0f, hS = 0.0f;
            const bool haveBubble = msgA > 0.01f;
            if (haveBubble) {
                bsize = (int)((float)MpPrefs_BubbleTextSize() * scale + 0.5f);
                if (bsize < 6) bsize = 6;
                for (int at = 0; at >= 0 && nLines < kBubbleMaxLines; ) {
                    at = wrapInto(it.msg, at, bl[nLines], kChatColsMax + 1, kBubbleCols);
                    const int len = (int)strlen(bl[nLines]);
                    if (len > cols) cols = len;
                    nLines++;
                }
                bubLineH    = px((float)bsize * kLineFactor);
                bubBottom   = hy - lineH - px((float)size * kBubbleGap);
                wS          = (float)cols * ((float)bsize * kCharWEm) + kBubblePadX * 2.0f;
                hS          = (float)nLines * ((float)bsize * kLineFactor)
                            + kBubblePadTop + kBubblePadBottom;
                panelLeft   = cx - px(wS) * 0.5f;
                panelBottom = bubBottom + px(kBubblePadBottom);
                panelTop    = panelBottom - px(hS);
            }

            // ---- the name. Over the head normally; at the BUBBLE'S TOP-LEFT when there is one, so
            // the two read as one object rather than as a label parked under a panel.
            if (nameA > 0.01f) {
                Slot* s = claim(hashOf(it.name, kLaneName), kZoneName, cls, gi, &made);
                if (s) {
                    style(*s, size, rgba(kNameR, kNameG, kNameB, nameA), 0.0f);
                    if (haveBubble) {
                        align(*s, 0.0f, 1.0f);              // by its bottom-LEFT, on the panel's corner
                        say(*s, it.name);
                        place(*s, panelLeft + px(kBubblePadX), panelTop);
                    } else {
                        align(*s, 0.5f, 1.0f);              // hangs from its bottom centre, over the head
                        say(*s, it.name);
                        place(*s, cx, hy);
                    }
                    showIt(*s, true);
                    drawn++;
                }
            }

            // ---- and the bubble itself: the panel, its frame, its tail, then the words
            if (haveBubble) {
                // The panel rides the frame's BUILD BUDGET like every other widget here: a first-time
                // panel counts against it, an existing one is free. A lobby that all starts talking
                // at once gets its panels over the next frame or two rather than in one hitch, and a
                // bubble without its panel yet is a bubble with no panel -- readable, just plainer.
                const int ri = kRectBubble0 + nBubblePanels;
                const bool haveRect = (nBubblePanels < kRectBubbles) && g_rect[ri].w != nullptr;
                if ((kBubblePanel || kBubbleFrame) && nBubblePanels < kRectBubbles
                    && (haveRect || made < kMakePerFrame)) {
                    if (!haveRect) made++;
                    placeFramedBox(ri, kRectFrame0 + nBubblePanels,
                                   panelLeft, panelBottom, wS, hS, msgA);
                    // THE TAIL, a square turned 45 degrees so the half below the panel is a downward
                    // triangle. Placed by its CENTRE, which is why the rect is offset by half of
                    // itself: `place` takes a bottom-left corner.
                    // The tail points AT the panel. Without one it is a diamond floating under the
                    // words, so it goes with the panel rather than standing on its own.
                    if (kBubbleTail && kBubblePanel) {
                        const float cy = panelBottom + px(kTailDrop);
                        placeRect(kRectTail0 + nBubblePanels,
                                  cx - px(kTailSize) * 0.5f, cy + px(kTailSize) * 0.5f,
                                  kTailSize, kTailSize, kBoxR, kBoxG, kBoxB, kBubbleAlpha * msgA);
                    } else {
                        hideRect(kRectTail0 + nBubblePanels);
                    }
                    nBubblePanels++;
                }
                for (int k = 0; k < nLines; k++) {
                    // Bottom line first, so k counts UP the screen exactly as the chat's rows do.
                    Slot* s = claim(hashOf(it.name, kLaneBubble + k), kZoneName, cls, gi, &made);
                    if (!s) break;
                    style(*s, bsize, rgba(kMsgR, kMsgG, kMsgB, msgA), 0.0f);
                    align(*s, 0.5f, 1.0f);
                    say(*s, bl[nLines - 1 - k]);
                    place(*s, cx, bubBottom - (float)k * bubLineH);
                    showIt(*s, true);
                    drawn++;
                }
            }
        }
    }
    // Whatever nobody talked into this frame comes down. The chat box's own rects are not touched --
    // they belong to hideRects and to a different question entirely.
    hideBubbleRects(nBubblePanels);
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
    // Being placed from F1 (Chat_PreviewBox): drawn OPEN, so there is a box to see where it goes.
    const bool open = Chat_IsOpen() || Chat_Preview(nullptr, nullptr);
    // F2 / "Hide the chat": the talk goes. The box still opens to type -- then it is the one thing
    // the player is looking at.
    if (C.hidden && !open) { hideRects(); return; }
    // Nothing to say and nobody typing: the panel goes with the words. Hidden HERE rather than only
    // on the open-to-closed edge, because the last line can expire in the same frame the box shuts.
    if (n <= 0 && !open) { hideRects(); return; }

    const int   size   = (int)(kChatSize + 0.5f);
    // NOT `small`: windows.h (rpcndr.h) defines that as `char`, and the error it produces names
    // neither the macro nor this line.
    const int   tiny   = (int)(kChatSmall + 0.5f);
    const float lineS  = kChatSize * kLineFactor;
    const float lineH  = px(lineS);
    // WHERE IT SITS: the player's two percentages (F1 > Chat), each across the room the OPEN box
    // leaves on screen -- 0 flush with the left / bottom edge, 100 with the right / top -- so the whole
    // box is on screen at every value, whatever its size. Measured from the OPEN box even while it is
    // closed, so the lines sit exactly where the box will open around them. The height is the same
    // arithmetic as the vertical layout below; `openRows` gets the same clamps as `histSlots`.
    int openRows = C.maxShownOpen > 0 ? C.maxShownOpen : 8;
    if (openRows > kSlotsChat - 8) openRows = kSlotsChat - 8;
    if (openRows > kSlotsChatName) openRows = kSlotsChatName;
    const float openTop = 2.0f * px(kChatSmall * kLineFactor) + 5.0f * px(kRuleGap)
                        + (float)(openRows + 1) * lineH + px(kBoxPadY);   // base -> panel top
    const float boxW = px(C.width + kBoxPadX * 2.0f), boxH = openTop + px(kBoxPadY);
    const float roomX = (float)g_vw > boxW ? (float)g_vw - boxW : 0.0f;
    const float roomY = (float)g_vh > boxH ? (float)g_vh - boxH : 0.0f;
    const float fx = C.posXPct < 0.0f ? 0.0f : C.posXPct > 100.0f ? 1.0f : C.posXPct / 100.0f;
    const float fy = C.posYPct < 0.0f ? 0.0f : C.posYPct > 100.0f ? 1.0f : C.posYPct / 100.0f;
    const float panelLeft = roomX * fx;
    float x = panelLeft + px(kBoxPadX);
    // The bottom edge of the LOWEST row, and the one number everything else here is measured from.
    float yBase = (float)g_vh - roomY * fy - px(kBoxPadY);
    if (menuUp) {
        // Out from under the pause menu's rows, which are on the LEFT: a box on the left half moves
        // to the mirror-image spot on the right, and it lifts clear of the menu's footer -- but never
        // off the top. The default corner lands exactly where it always went.
        if (panelLeft + boxW * 0.5f < (float)g_vw * 0.5f) x = (float)g_vw - panelLeft - boxW + px(kBoxPadX);
        yBase -= px(kPauseLiftY);
        if (yBase < openTop) yBase = openTop;
    }

    // ---- everything that was said, as visual lines, oldest first
    // `nameLen` is how much of the row is the speaker's name, and only on the row the name is ON --
    // a wrapped continuation carries none of it. The row is drawn in the plain text colour and the
    // name re-drawn over the top in the speaker's, which is how one colour per widget becomes two.
    struct Row { const char* text; float a; int nameLen; bool mine, system; };
    // The wrap width, from the box the player asked for and the text they asked for.
    int cols = (int)((float)C.width / ((float)size * kCharWEm));
    if (cols < 8) cols = 8;
    if (cols > kChatColsMax) cols = kChatColsMax;

    // WHAT EACH MESSAGE COSTS IN ROWS, before any of them are built. The build below fills `rows`
    // oldest-first and used to simply stop at kChatRows -- so when the talk wrapped into more rows
    // than there was room for, the ones that fell off the end were the NEWEST. The box would open on
    // a conversation that stopped part-way, with nothing below it to scroll down to, and it came
    // right on its own once those long messages aged out of the window. It got easier to hit once
    // the player could set the width and the text size, because both move `cols`.
    // The count is taken by running the REAL wrapper, not a second copy of its rules: a count that
    // disagrees with the build is the same bug wearing a different hat.
    float rowAlpha[kChatLines];
    int   rowCost[kChatLines];
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
        rowAlpha[i] = a;
        int cost = 0;
        if (a > 0.01f) {
            char scratch[kChatColsMax + 1];
            for (int at = 0; at >= 0; ) { at = wrapInto(l.text, at, scratch, (int)sizeof(scratch), cols); cost++; }
        }
        rowCost[i] = cost;
    }
    // Spend the budget from the NEWEST backwards: the bottom of the box has to be the bottom of the
    // conversation. The newest message goes in even if it alone overruns -- clipped beats absent.
    int start = n;
    for (int i = n - 1, budget = kChatRows; i >= 0; i--) {
        if (rowCost[i] > budget && start < n) break;
        budget -= rowCost[i];
        start = i;
        if (budget <= 0) break;
    }

    char  rowText[kChatRows][kChatColsMax + 1];
    Row   rows[kChatRows];
    int   nr = 0;
    for (int i = start; i < n; i++) {
        const ChatLineView& l = lines[i];
        const float a = rowAlpha[i];
        if (a <= 0.01f) continue;
        bool first = true;
        for (int at = 0; at >= 0 && nr < kChatRows; ) {
            const int from = at;
            at = wrapInto(l.text, at, rowText[nr], kChatColsMax + 1, cols);
            rows[nr].text = rowText[nr];
            rows[nr].a = a; rows[nr].mine = l.mine; rows[nr].system = l.system;
            // The name only belongs to the row it starts on, and only as far as that row goes -- a
            // name long enough to wrap keeps its colour on the part that fits and no further.
            int nl = 0;
            if (first && l.nameLen > 0) {
                nl = (int)l.nameLen - from;
                const int have = (int)strlen(rowText[nr]);
                if (nl > have) nl = have;
                if (nl < 0) nl = 0;
            }
            rows[nr].nameLen = nl;
            first = false;
            nr++;
        }
    }

    // ---- the shape of it. Open, the history is always `histSlots` rows tall whether or not there
    // is anything in them, which is what makes the panel the same size every time it appears.
    int histSlots = open ? C.maxShownOpen : C.maxShownIdle;
    if (histSlots <= 0) histSlots = 8;
    // ...and never more rows than the chat's half of the pool can hold, or the furniture at the
    // bottom of this function would find nothing left to claim.
    if (histSlots > kSlotsChat - 8)  histSlots = kSlotsChat - 8;
    // ...and no more rows than there are overlays to tint their names with, or the rows past
    // the end would come out white -- which is the bug this zone was added for.
    if (histSlots > kSlotsChatName)  histSlots = kSlotsChatName;
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
        if (tn > cols) tail = typed + (tn - cols);
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
        Slot* s = claim(hashOf("hdr", kLaneChatHdr), kZoneChat, g_cls, g_gi, &g_made);
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
        Slot* o = claim(hashOf("online", kLaneChatOnline), kZoneChat, g_cls, g_gi, &g_made);
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
        const float yRow = yHist0 - (float)k * lineH;
        Slot* s = claim(hashOf("line", kLaneChat + k), kZoneChat, g_cls, g_gi, &g_made);
        if (s) {
            // THE WORDS ARE PLAIN. A system line has no name and keeps its own grey.
            const float r = row.system ? kSysR : kMsgR;
            const float g = row.system ? kSysG : kMsgG;
            const float b = row.system ? kSysB : kMsgB;
            style(*s, size, rgba(r, g, b, row.a), 0.0f);
            align(*s, 0.0f, 1.0f);
            say(*s, row.text);
            place(*s, x, yRow);
            showIt(*s, true);
            g_drawn++;
        }
        // ...and THE NAME OVER THE TOP of it, in whose it is. Same font, same size, same left edge,
        // so every glyph lands exactly on the one underneath and nothing has to be measured.
        if (row.nameLen > 0) {
            Slot* ns = claim(hashOf("name", kLaneChatName + k), kZoneChatName, g_cls, g_gi, &g_made);
            if (ns) {
                char nm[64];
                int nl = row.nameLen; if (nl > (int)sizeof(nm) - 1) nl = (int)sizeof(nm) - 1;
                memcpy(nm, row.text, (size_t)nl); nm[nl] = 0;
                style(*ns, size, rgba(row.mine ? kSayR : kNameR, row.mine ? kSayG : kNameG,
                                      row.mine ? kSayB : kNameB, row.a), 0.0f);
                align(*ns, 0.0f, 1.0f);
                say(*ns, nm);
                place(*ns, x, yRow);
                showIt(*ns, true);
                g_drawn++;
            }
        }
        if (yHist0 - (float)k * lineH < lineH) break;    // ran off the top of the screen
    }
    // An empty history says so, rather than leaving a panel that looks like it failed to load.
    if (open && histShown == 0) {
        Slot* s = claim(hashOf("empty", kLaneChatEmpty), kZoneChat, g_cls, g_gi, &g_made);
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
        Slot* s = claim(hashOf("compose", kLaneChatIn), kZoneChat, g_cls, g_gi, &g_made);
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
        Slot* s = claim(hashOf("hints", kLaneChatHints), kZoneChat, g_cls, g_gi, &g_made);
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
            Slot* c = claim(hashOf("count", kLaneChatCount), kZoneChat, g_cls, g_gi, &g_made);
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
// EVERY ENGINE OBJECT WE CACHED, DROPPED. This is not housekeeping; it is the fix for a crash.
//
// The font and the border texture are looked up ONCE and remembered. What keeps them alive is OUR
// widgets referencing them -- FSlateFontInfo::FontObject and FSlateBrush::ResourceObject are both
// UPROPERTYs, so the collector sees them through the text block and the image. The moment the pool is
// dropped, nothing references either one, the collector is free to take them, and the remembered
// pointers are dangling. The next widget built then gets a dead UFont, and Slate asks it for its
// composite font: "Pure virtual function being called", in FSlateFontInfo::GetCompositeFont, on the
// first frame after a map change with somebody else in the session (field crash).
//
// So the rule the widgets already had -- nothing outlives the world it was found in -- applies to
// these too. They cost one lookup each to re-resolve.
static void forgetCachedAssets() {
    g_regularFont = nullptr; g_fontLooked = false;
    g_borderTex   = nullptr; g_borderLooked = false;
    g_borderHow   = "not looked for yet";
    // ...and hold the LOAD off until the world has settled. Finding it is free and happens at once;
    // loading it is not, and the frames after a map change are the wrong ones to spend on it.
    g_borderTryAtMs = GetTickCount64() + kAssetSettleMs;
}

void GameHud_Forget() {
    // The world went and took the viewport with it. NOTHING here may be dereferenced -- see game_hud.h.
    forgetCachedAssets();
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
    // The world is still here, but OUR references to the font and the texture have just gone with the
    // widgets -- which is exactly the state that leaves a remembered pointer dangling.
    forgetCachedAssets();
}

// THE SPEECH BUBBLE'S LOOK, from the player's settings. Its text size is not here: the bubble reads
// MpPrefs_BubbleTextSize() where it draws, because the size it wants depends on how far away the
// speaker is and that is only known there.
void GameHud_SetBubbleLook(bool panel, bool border, int panelPct, int blurPct) {
    kBubblePanel = panel;
    kBubbleFrame = border;
    if (panelPct >= 0 && panelPct <= 100) kBubbleAlpha = (float)panelPct / 100.0f;
    if (blurPct >= 0 && blurPct <= 100) {
        // Half the chat box's strength at the same number and a tighter radius: a bubble is small, and
        // the box's settings on one read as a smear. The same percentage means "as frosted as the box"
        // rather than "the same pixels".
        kBubbleBlurMax    = (float)blurPct * 0.25f;
        kBubbleBlurRadius = (int)((float)blurPct * 0.20f + 0.5f);
        kBubbleBlur       = blurPct > 0;
    }
}

void GameHud_SetChatLook(int textSize, int smallSize, int panelPct, int blurPct) {
    // The header and the hints used to be a fixed fraction of the talk. They are their own setting
    // now: wanting big text is not the same as wanting a big header, and tying them together meant
    // neither could be chosen.
    if (textSize  >= 8 && textSize  <= 40) kChatSize  = (float)textSize;
    if (smallSize >= 6 && smallSize <= 40) kChatSmall = (float)smallSize;
    if (panelPct >= 0 && panelPct <= 100) kBoxAlpha = (float)panelPct / 100.0f;
    if (blurPct  >= 0 && blurPct  <= 100) {
        kBoxBlurMax    = (float)blurPct * 0.50f;
        kBoxBlurRadius = (int)((float)blurPct * 0.43f + 0.5f);
        kBoxBlur       = blurPct > 0;
    }
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
