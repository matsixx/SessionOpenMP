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
// SessionOpenMP -- the in-game pause-menu integration. Design + the measured facts: pause_menu.h.
#include "pause_menu.h"
#include "../debug.h"
#include "overlay.h"
#include "menu_ext.h"
#include "../game/game_syms.h"
#include "../game/spectate.h"
#include "../session/session.h"
#include "../session/banlist.h"
#include "../transport/transport.h"
#include "mp_name.h"
#include "mp_prefs.h"
#include "version_tag.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#include "MinHook.h"
#endif

using namespace omp::game;

PauseMenuTuning& PauseMenu_Tuning() { static PauseMenuTuning t; return t; }

// ---- state ------------------------------------------------------------------------------------------
// A fault anywhere in here disables the whole feature for the run and says so ONCE: the pause menu is
// cosmetic, the session is not, and the F1 path (a separate TU with separate hooks) is unaffected.
static bool  g_dead      = false;
static bool  g_installed = false;
static MpUiState g_state{};                 // published by the game thread, read while building rows
static volatile LONG g_pending = OVA_NONE;  // same single-slot handoff shape as the overlay's

// Which of OUR pages the pause menu is currently showing. This is a FAKE page: the engine's
// `_activePageDefinition` stays the pause root the whole time -- we simply hand CreatePageItems a
// different row array and re-run it. So the reset signal cannot be the page definition; it is
// "CreatePageItems ran on the root page and WE did not ask for it" (see g_selfRefresh).
enum Page { PG_ROOT = 0, PG_MP = 1, PG_BROWSE = 2, PG_PLAYERS = 3, PG_PLAYER = 4, PG_NAMES = 5,
            PG_OTHER = 6, PG_GUEST0 = 7 };
static int  g_page        = PG_ROOT;
// True for pages whose row list is ENTIRELY ours (as opposed to the pause root, where we append to
// the game's own rows). Only these can safely be left to the engine's own scroll window.
static bool pageScrollsItself() { return g_page >= PG_GUEST0; }
// A guest page that some OTHER guest page opens is a SUB-page: it belongs to its parent's list and
// must not also appear in the pause menu, or the split would just move the clutter up a level.
static bool guestIsSubPage(int idx);
static bool g_selfRefresh = false;          // true only across our own MenuRefreshItems call

static void log(const char* s) { OvLog(s); }
static void die(const char* why) {
    if (g_dead) return;
    g_dead = true;
    char m[220]; snprintf(m, sizeof(m), "[menu] *** DISABLED for this run: %s (F1 menu is unaffected)", why);
    log(m);
}
static void post(OvAction a) {
#ifdef _WIN32
    InterlockedExchange(&g_pending, (LONG)a);
#else
    g_pending = (LONG)a;
#endif
}

// ---- FName -> string --------------------------------------------------------------------------------
// Same helper (and the same deliberate leak) as audio.cpp's `fnameStr`: FName::ToString allocates the
// FString buffer through the engine allocator and there is no Free symbol, so this is only ever called
// on one-time / cached paths -- never per frame. Duplicated rather than shared because each TU owns its
// own concern here; if a third copy appears, promote it to game_syms.
static bool fnameStr(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    const Syms& S = Get();
    if (!S.FNameToString || !fnamePtr) return false;
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fnamePtr, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// A UE TArray header, as the engine lays it out. We only ever READ the game's and WRITE our own.
struct TArrayHdr { void* data; int32_t num; int32_t max; };

// ---- text -------------------------------------------------------------------------------------------
// An FText is 24 bytes and is treated as opaque here: built once, copied bitwise into the row blocks,
// NEVER destructed. That is the ownership model -- see the note on g_rows below.
struct FTextBlob { uint8_t bytes[24]; };

static bool makeFName(const char* s, uint64_t* out) {
    const Syms& S = Get();
    if (!S.FNameCtor || !s || !*s) return false;
    __try { *out = 0; S.FNameCtor(out, s, 1 /* FNAME_Add */); return *out != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// FText from a plain ASCII string, via FName. Deliberately NOT FromString/AsCultureInvariant: those
// come in const-ref and rvalue-ref twins that no byte signature can tell apart, and picking the wrong
// one means the engine either steals or double-frees a buffer we own. An FName argument is a POD 8
// bytes -- there is no ownership question to get wrong. (Cost: FNames are interned forever, which is
// exactly right for a fixed set of menu labels.)
static bool makeText(const char* s, FTextBlob* out) {
    const Syms& S = Get();
    uint64_t fn = 0;
    if (!S.TextFromName || !makeFName(s, &fn)) return false;
    __try { memset(out, 0, sizeof(*out)); S.TextFromName(out, &fn); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Status strings change (peer counts), so they are cached by STRING -- an FText is only ever built for
// a string we have not shown before. Bounded by construction: at most kTextCache distinct strings for
// the life of the process, so the "never destructed" rule stays affordable.
//
// "Bounded by construction" is only true of the strings this table is FOR. It was not true of the
// two that were quietly using it: player names, which arrive as people join, and page titles, one of
// which IS a player name. A five-player session filled all 24 in under an hour, and the visible
// symptom was the replay Look At list offering two people out of five -- the caller read a full
// cache as "stop building the list". Both now own their text (see spectateOptText and setTitle);
// what is left here really is a fixed set of literals. The headroom is for whatever gets added next
// before somebody notices this comment.
static const int kTextCache = 64;
static struct { char s[112]; FTextBlob t; bool used; } g_textCache[kTextCache];
static const FTextBlob* cachedText(const char* s) {
    if (!s || !*s) return nullptr;
    for (int i = 0; i < kTextCache; i++) {
        if (!g_textCache[i].used) break;
        if (strcmp(g_textCache[i].s, s) == 0) return &g_textCache[i].t;
    }
    for (int i = 0; i < kTextCache; i++) {
        if (g_textCache[i].used) continue;
        if (!makeText(s, &g_textCache[i].t)) return nullptr;
        strncpy_s(g_textCache[i].s, s, _TRUNCATE);
        g_textCache[i].used = true;
        return &g_textCache[i].t;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        log("[menu] text cache full -- status text stops updating from here. If a LIST looks short,"
            " this is why: something is putting per-player strings through a fixed table.");
    }
    return nullptr;
}

// ---- the guest seam (menu_ext.h) --------------------------------------------------------------------
// Other mod DLLs register a page of their own. Everything is COPIED at registration (the guest's
// strings need not outlive the call) and every callback runs on the GAME thread inside SEH -- the
// opposite thread contract from the ImGui seam, which is why it is a separate export.
struct GuestItem {
    // `desc` is the footer line, sized for a couple of sentences -- a shorter buffer silently
    // truncates anything that explains a trade-off in both directions. This is the host's own copy,
    // not the ABI struct (the seam passes `const char*`), so growing it costs guests nothing.
    char  key[48]; char label[64]; char desc[192];
    int   kind;                                  // OMP_ITEM_*
    char  offLabel[32], onLabel[32];             // toggle
    float minValue, maxValue, step;              // slider, in the guest's own units
};
struct GuestPage {
    char title[64];
    GuestItem items[OMP_PAGEITEM_MAX];
    int  n;
    OmpPageSelectFn onSelect;
    OmpPageStatusFn onStatus;
    OmpPageValueFn  onValue;
    OmpPageGetFn    onGet;
    void* user;
    bool  used, dead;
    int   parent;    // guest page index this one was opened FROM, -1 = from the pause menu itself.
                     // Set when the row is confirmed, so Back returns the way the user came in.
};
// 8: splitting a long page into categories costs one registration per category, and the parent
// counts too. The pause menu only ever lists the pages nothing else opens.
// 16: SessionTweaks alone registers 9 (8 category pages + its front page). A refused
// registration is worse than it looks -- an unregistered FRONT page leaves every category
// page unclaimed, and they all spill into the pause-menu root as loose rows.
static const int kMaxGuestPages = 16;
static GuestPage g_guests[kMaxGuestPages];
static int       g_nGuests = 0;
#ifdef _WIN32
static CRITICAL_SECTION g_guestMx;
static bool             g_guestMxInit = false;
#endif

// Resolved by TITLE rather than by an index the guest would have to predict: registration order is
// the guest's business, and a name that does not match simply leaves the row inert instead of
// opening the wrong page.
static int guestPageByTitle(const char* title) {
    if (!title || !*title) return -1;
    for (int i = 0; i < g_nGuests; i++)
        if (!g_guests[i].dead && strcmp(g_guests[i].title, title) == 0) return i;
    return -1;
}
static bool guestIsSubPage(int idx) {
    if (idx < 0 || idx >= g_nGuests) return false;
    for (int p = 0; p < g_nGuests; p++) {
        if (p == idx || g_guests[p].dead) continue;
        for (int i = 0; i < g_guests[p].n; i++)
            if (g_guests[p].items[i].kind == OMP_ITEM_PAGE &&
                strcmp(g_guests[p].items[i].key, g_guests[idx].title) == 0) return true;
    }
    return false;
}

// SEH cannot live in a function with C++ unwind semantics, so the guarded calls are their own tiny
// functions -- the same shape as overlay.cpp's extCallGuarded. A page that faults is marked dead and
// never shown again this run.
static bool guestSelectGuarded(GuestPage* g, const char* key) {
    __try { g->onSelect(key, g->user); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static const char* guestStatusGuarded(GuestPage* g, const char* key) {
    __try { return g->onStatus(key, g->user); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static bool guestValueGuarded(GuestPage* g, const char* key, int iv, float fv) {
    __try { g->onValue(key, iv, fv, g->user); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int guestGetGuarded(GuestPage* g, const char* key, int* oi, float* of) {
    __try { return g->onGet(key, oi, of, g->user); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }      // -1 = faulted, distinct from 0 = declined
}

// The real registration. v1's export is a thin wrapper that fills in an all-action page.
static int registerPage(const char* title, const OmpPageItem2* items, int nItems,
                        OmpPageSelectFn onSelect, OmpPageValueFn onValue,
                        OmpPageGetFn onGet, OmpPageStatusFn onStatus, void* user) {
    if (!title || !items || nItems <= 0) return 0;
#ifdef _WIN32
    if (!g_guestMxInit) { InitializeCriticalSection(&g_guestMx); g_guestMxInit = true; }
    EnterCriticalSection(&g_guestMx);
#endif
    int rc = 0;
    if (g_nGuests < kMaxGuestPages) {
        GuestPage& g = g_guests[g_nGuests];
        memset(&g, 0, sizeof(g));
        g.parent = -1;      // NOT the memset's 0 -- that is page index 0, a real page to fall back to
        strncpy_s(g.title, title, _TRUNCATE);
        g.n = (nItems < OMP_PAGEITEM_MAX) ? nItems : OMP_PAGEITEM_MAX;
        for (int i = 0; i < g.n; i++) {
            GuestItem& d = g.items[i];
            strncpy_s(d.key,   items[i].key   ? items[i].key   : "", _TRUNCATE);
            strncpy_s(d.label, items[i].label ? items[i].label : "", _TRUNCATE);
            strncpy_s(d.desc,  items[i].desc  ? items[i].desc  : "", _TRUNCATE);
            d.kind = items[i].kind;
            strncpy_s(d.offLabel, (items[i].offLabel && *items[i].offLabel) ? items[i].offLabel : "Off", _TRUNCATE);
            strncpy_s(d.onLabel,  (items[i].onLabel  && *items[i].onLabel)  ? items[i].onLabel  : "On",  _TRUNCATE);
            d.minValue = items[i].minValue; d.maxValue = items[i].maxValue; d.step = items[i].step;
            // A slider with a degenerate range would divide by zero mapping to the 0..1 bar.
            if (d.kind == OMP_ITEM_SLIDER && !(d.maxValue > d.minValue)) { d.kind = OMP_ITEM_ACTION; }
        }
        g.onSelect = onSelect; g.onStatus = onStatus; g.onValue = onValue; g.onGet = onGet;
        g.user = user; g.used = true;
        g_nGuests++;
        rc = 1;
    }
#ifdef _WIN32
    LeaveCriticalSection(&g_guestMx);
#endif
    char m[160];
    snprintf(m, sizeof(m), "[menu] guest page '%s' (%d items) %s", title, nItems,
             rc ? "registered" : "REFUSED -- page table full");
    log(m);
    return rc;
}

extern "C" __declspec(dllexport)
int OmpMenu_RegisterPage(const char* title, const OmpPageItem* items, int nItems,
                         OmpPageSelectFn onSelect, OmpPageStatusFn onStatus, void* user) {
    // v1 compatibility: every row is an action. Kept exact so a guest binary built before the
    // toggle/slider seam existed keeps working unchanged.
    if (!items || nItems <= 0 || !onSelect) return 0;
    OmpPageItem2 conv[OMP_PAGEITEM_MAX] = {};
    if (nItems > OMP_PAGEITEM_MAX) nItems = OMP_PAGEITEM_MAX;
    for (int i = 0; i < nItems; i++) {
        conv[i].kind = OMP_ITEM_ACTION;
        conv[i].key = items[i].key; conv[i].label = items[i].label; conv[i].desc = items[i].desc;
    }
    return registerPage(title, conv, nItems, onSelect, nullptr, nullptr, onStatus, user);
}

extern "C" __declspec(dllexport)
int OmpMenu_RegisterPage2(const char* title, const OmpPageItem2* items, int nItems,
                          OmpPageSelectFn onSelect, OmpPageValueFn onValue,
                          OmpPageGetFn onGet, OmpPageStatusFn onStatus, void* user) {
    return registerPage(title, items, nItems, onSelect, onValue, onGet, onStatus, user);
}

// ---- our rows ---------------------------------------------------------------------------------------
// OWNERSHIP, the single most important thing in this file. These blocks are 144-byte
// FMenuPageItemDefinitions built here and NEVER destructed. That is safe -- and only safe -- because
// the engine never takes ownership of them: `CreatePageItems` reads the array it is handed and
// COPY-constructs each element into a widget (bumping the FText refcounts), so the permanent +1 held
// here means the refcount can never reach zero when those widgets die. Do not "fix" this into a
// destructor, and never let this storage reach the page's own `_pageItemDefinitions` -- the next page
// activation destructs that array element-wise and then FMemory::Free()s its buffer.
struct RowSpec { const char* key; const char* label; const char* desc; OvAction act; };

// The multiplayer sub-page. Same five actions the F1 menu offers, in the same order, posting the same
// OvAction values -- one behaviour, two surfaces.
// The same-PC (shared-memory) rows are DELIBERATELY ABSENT. That wire is a development rig, not a
// way to play: a player who found it in the pause menu would end up in a session with nobody, and
// switching from it to online inside one session can stall the Epic sign-in. It lives in the F1
// window under "Dev tools", with that warning beside it.
static const RowSpec kMpRows[] = {
    { "OmpHostOnline",  "Host online",         "Start a game anyone can find in the session list", OVA_HOST_ONLINE },
    { "OmpHostPrivate", "Create private game", "Start a game only people with your code can join", OVA_HOST_PRIVATE },
    { "OmpJoinPrivate", "Join private game",   "Enter a friend's 6-character code", OVA_JOIN_PRIVATE },
    { "OmpLeave",       "Leave session",       "Disconnect and keep playing solo", OVA_LEAVE },
    { "OmpBack",        "Back",                "Return to the pause menu", OVA_NONE },
};
static const int kMpRowCount = (int)(sizeof(kMpRows) / sizeof(kMpRows[0]));
// "Join online" opens the browser rather than acting, so it is inserted rather than listed -- right
// under "Host online", where the public pair belongs.
static const int kMpBrowseAfter = 0;
// "Players" goes after "Join private game" -- the moderation tools belong next to the session
// controls, not up with the ways of starting one.
static const int kMpPlayersAfter = 2;

// The row array we substitute. Sized for the stock page plus everything we could ever add.
static const int kRowCap = 96;
static uint8_t   g_rowBuf[kRowCap * 0x90];
// Our own row templates, built once on the game thread the first time a page is built.
static uint8_t   g_mpRows[(kMpRowCount) * 0x90];
static uint8_t   g_rootRow[0x90];                       // the "Multiplayer" entry on the pause page
static uint8_t   g_guestRootRows[kMaxGuestPages * 0x90];
static uint8_t   g_guestRows[kMaxGuestPages][OMP_PAGEITEM_MAX * 0x90];
static uint64_t  g_mpRowKeys[kMpRowCount];
static uint64_t  g_rootRowKey = 0;
// ---- the privacy toggle. A native MultiOption row of the mod's own on the multiplayer page, so it
// is built and tracked here rather than through the guest seam -- the seam exists for other DLLs, and
// routing a first-party row through it would mean inventing a fake guest.
// It goes right after the join rows: it is a property of how you connect, so it belongs beside the
// buttons that connect, not buried somewhere else.
// ---- YOUR NAME. An INFO row showing the name you currently go by; selecting it opens the typing box
// (a menu page cannot take free text -- see overlay.h). Rebuilt every page build so it always shows
// the live value, including right after the box has changed it.
static uint8_t   g_nameRow[0x90];
static FTextBlob g_nameOpt;
static uint64_t  g_nameKey = 0;
static const int kMpNameAfter = 0;       // first thing on the page: it is who you ARE in the session
static uint8_t   g_privacyRow[0x90];
static FTextBlob g_privacyOpt[2];
static uint64_t  g_privacyKey = 0;
static int       g_privacyAt  = -1;      // widget index within the LAST build; -1 = not on this page
static const int kMpPrivacyAfter = 2;    // after "Join private game", before "Leave session"
// ---- PLAYER NAMES. A sub-page of the multiplayer page, because "how the names look" is a set of
// preferences and the multiplayer page is a list of things to DO -- mixing the two makes both harder
// to read. Its rows are the game's own native controls (one MultiOption, two ProgressBars), first-party
// like the privacy toggle rather than routed through the guest seam.
static uint8_t   g_otherOpenRow[0x90];                  // "Other options" on the MP page
static uint64_t  g_otherOpenKey = 0;
static uint8_t   g_namesOpenRow[0x90];                  // "Player names", now on PG_OTHER
static uint64_t  g_namesOpenKey = 0;
static uint8_t   g_nameModeRow[0x90];
static FTextBlob g_nameModeOpts[3];
static uint64_t  g_nameModeKey = 0;
static uint8_t   g_nameDistRow[0x90], g_bubbleDistRow[0x90];
static uint64_t  g_nameDistKey = 0, g_bubbleDistKey = 0;
// Widget indices within the LAST build, so the current values can be stamped once the whole rebuild
// is finished (they cannot be stamped any earlier -- see stampValues). -1 = not on this page.
static int       g_nameModeAt = -1, g_nameDistAt = -1, g_bubbleDistAt = -1;
static const int kMpNamesAfter = 0;      // directly under "Your name": both are about who you see
// ---- DROPPED OBJECTS. A three-state MultiOption on the multiplayer page: off / live edits only /
// share one set. It sits with "Player names" rather than with the connect buttons because it is about
// what you SEE in a session, not about how you get into one.
static uint8_t   g_dropRow[0x90];
static FTextBlob g_dropOpts[3];
static uint64_t  g_dropKey = 0;
static int       g_dropAt  = -1;         // widget index within the LAST build; -1 = not on this page
static const int kMpDropAfter = 0;       // beside "Player names", under "Your name"
// ---- THE LEVEL'S OWN PROPS. Its own row rather than another state on "Dropped objects", because it
// is a different feature with a different maturity: sharing the map's furniture is unfinished, and
// keeping it separate means it cannot take the working half down with it.
// ---- the posture row. An INFO row (label + right-hand value) rebuilt on every page build, like the
// lobby rows, so it reports the LIVE configuration rather than whatever was true at startup. Its
// strings come from the transport, so a future backend describes itself and this code does not grow a
// case per wire. Descriptive only -- it states what you chose, it does not argue with it.
static uint8_t   g_postureRow[0x90];
static FTextBlob g_postureOpt;
static uint64_t  g_postureKey = 0;
// ---- the lobby browser page -------------------------------------------------------------------------
// Rows are rebuilt from the transport's result list every time the page is built, so they are storage
// we own like every other row: a "Join online" opener, a Refresh, one row per lobby, and Back.
static const int kMaxLobbyRows = 12;
static uint8_t   g_browseOpenRow[0x90];                       // the "Join online" row on the MP page
// ---- MODERATION. Your session, your rules -- and only your session, because that is the only lobby
// EOS lets you remove anyone from.
static uint8_t   g_playersOpenRow[0x90];                      // "Players" on the MP page
static uint8_t   g_playerRows[kMaxLobbyRows * 0x90];          // one per peer in YOUR session
static FTextBlob g_playerOptText[kMaxLobbyRows];
static uint64_t  g_playersOpenKey = 0;
static uint64_t  g_playerRowKeys[kMaxLobbyRows];
static int       g_playerRowCount = 0;
static char      g_playerRowIds[kMaxLobbyRows][80];
static char      g_playerRowNames[kMaxLobbyRows][40];
static uint8_t   g_kickRow[0x90], g_banRow[0x90];
static uint64_t  g_kickKey = 0, g_banKey = 0;
static char      g_selPeerId[80] = {0}, g_selPeerName[40] = {0};
static volatile LONG g_havePeerAction = 0;
static char      g_actPeerId[80] = {0}, g_actPeerName[40] = {0};
static uint8_t   g_refreshRow  [0x90];
static uint8_t   g_lobbyRows   [kMaxLobbyRows * 0x90];
static FTextBlob g_lobbyOptText[kMaxLobbyRows];               // the right-hand "3/8" column
static uint64_t  g_browseOpenKey = 0, g_refreshKey = 0;
static uint64_t  g_lobbyRowKeys[kMaxLobbyRows];
static int       g_lobbyRowCount = 0;                         // rows built on the LAST page build
static int       g_lobbyRowIdx[kMaxLobbyRows];                // row -> transport browse index
static int       g_lastBrowseState = -99;                     // to notice "results arrived"
static void*     g_lastPage = nullptr;                        // the pause page, as of the last build
static int       g_browsePolls = 0;                           // rebuilds still owed to a live search
static uint32_t  g_lastSig = 0xffffffffu;                     // last session state the page showed
static volatile LONG g_pendingJoinIdx = -1;
static uint64_t  g_guestRootKeys[kMaxGuestPages];
static uint64_t  g_guestItemKeys[kMaxGuestPages][OMP_PAGEITEM_MAX];
static bool      g_rowsBuilt = false;
static uint64_t  g_pauseKey  = 0;                       // FName("PauseMenuPage")

// A ZERO-INITIALISED FText IS NOT AN EMPTY FText -- it is a null pointer with a crash attached.
// `UMenuPageContainer::HandlePageItemSelectionChanged` takes the item's `_longDescription`
// (params+0x48) and hands it to `UTRXUtilities::FindLocalizedTextForPlatformFromText`, whose FIRST
// act is to dereference `TextData`; the `_shortDescription` reaches the footer text block by the same
// path. Navigating onto a row with zeroed descriptions therefore dies in
// `FTextInspector::GetTableIdAndKey` reading address 0. A real default-constructed FText points at
// the shared empty text data and memset does not produce one. RULE: every FText field in a definition
// built here gets a REAL FText, always -- there is no such thing as "leave it blank".
static bool buildRow(uint8_t* dst, const char* key, const char* label, const char* desc, uint64_t* keyOut) {
    memset(dst, 0, off::kItemSize);
    uint64_t fn = 0;
    if (!makeFName(key, &fn)) return false;
    *(uint64_t*)(dst + off::kItemKey) = fn;
    if (keyOut) *keyOut = fn;
    *(dst + off::kItemType) = 1;                        // EMenuPageItemType::Selection
    FTextBlob t;
    if (!makeText(label, &t)) return false;
    memcpy(dst + off::kItemLabel, &t, sizeof(t));
    // Both descriptions, unconditionally. Falling back to the label keeps the field valid when a row
    // has nothing to say; sharing one blob between the two fields is fine because the engine's copy
    // and destroy are balanced per field and our own construction reference is never released.
    const char* d = (desc && *desc) ? desc : label;
    if (!makeText(d, &t)) return false;
    memcpy(dst + off::kItemShortDesc, &t, sizeof(t));
    memcpy(dst + off::kItemLongDesc,  &t, sizeof(t));
    return true;
}

// ---- the native control row types (toggle / slider) -------------------------------------------------
// A TOGGLE is the game's MultiOption row. Everything it needs lives on the DEFINITION -- the option
// texts (+0x68, a TArray<FText>) and the current index (+0x78) -- so it needs no widget poking at all;
// we just rebuild the row with the guest's live value each time the page is built. The option-text
// array points at OUR static FText pairs: CreatePageItems copy-constructs from it (ResizeForCopy +
// per-element copy), so, exactly like the row array itself, the engine never owns our storage.
static FTextBlob g_optTexts[kMaxGuestPages][OMP_PAGEITEM_MAX][2];

static bool buildToggleRow(uint8_t* dst, const GuestItem& it, uint64_t* keyOut, FTextBlob* opts) {
    if (!buildRow(dst, it.key, it.label, it.desc, keyOut)) return false;
    if (!makeText(it.offLabel, &opts[0]) || !makeText(it.onLabel, &opts[1])) return false;
    *(dst + off::kItemType) = 2;                        // EMenuPageItemType::MultiOption
    // A TArray header we own, pointing at storage we own and never free.
    *(void**)  (dst + off::kItemMultiTexts)        = opts;
    *(int32_t*)(dst + off::kItemMultiTexts + 0x08) = 2;   // Num
    *(int32_t*)(dst + off::kItemMultiTexts + 0x0c) = 2;   // Max
    return true;
}
// The same row with N options instead of two. A setting with three states (off / off board only /
// always) is not a toggle, and forcing it into one would cost the player a row to say it.
static bool buildOptionRow(uint8_t* dst, const char* key, const char* label, const char* desc,
                           const char* const* opts, int nOpts, uint64_t* keyOut, FTextBlob* blobs) {
    if (!opts || nOpts < 1 || nOpts > 8) return false;
    if (!buildRow(dst, key, label, desc, keyOut)) return false;
    for (int i = 0; i < nOpts; i++) if (!makeText(opts[i], &blobs[i])) return false;
    *(dst + off::kItemType) = 2;                        // MultiOption
    *(void**)  (dst + off::kItemMultiTexts)        = blobs;
    *(int32_t*)(dst + off::kItemMultiTexts + 0x08) = nOpts;
    *(int32_t*)(dst + off::kItemMultiTexts + 0x0c) = nOpts;
    return true;
}
// An INFO row: a Selection-looking row that ALSO gets the right-hand value column. It is a
// MultiOption row with exactly ONE option, which is the only way the game draws a value beside a
// label -- the arrows have nowhere to go (MultiOptionSetSelectedItemIndex bounds-checks against
// option count - 1 = 0) and confirming still fires OnSelectionConfirmed like any other row. That is
// what turns a flat list into a server browser: "Brooklyn Banks        3/8".
static bool buildInfoRow(uint8_t* dst, const char* key, const char* label, const char* desc,
                         const char* value, uint64_t* keyOut, FTextBlob* opt) {
    if (!buildRow(dst, key, label, desc, keyOut)) return false;
    if (!makeText((value && *value) ? value : " ", opt)) return false;
    *(dst + off::kItemType) = 2;                        // MultiOption
    *(void**)  (dst + off::kItemMultiTexts)        = opt;
    *(int32_t*)(dst + off::kItemMultiTexts + 0x08) = 1;
    *(int32_t*)(dst + off::kItemMultiTexts + 0x0c) = 1;
    return true;
}

static bool buildSliderRow(uint8_t* dst, const GuestItem& it, uint64_t* keyOut) {
    if (!buildRow(dst, it.key, it.label, it.desc, keyOut)) return false;
    *(dst + off::kItemType) = 3;                        // EMenuPageItemType::ProgressBar
    *(float*)(dst + off::kItemProgMin)       = it.minValue;
    *(float*)(dst + off::kItemProgMax)       = it.maxValue;
    *(float*)(dst + off::kItemProgIncrement) = (it.step > 0.0f) ? it.step : 1.0f;
    return true;
}

// The three non-text fields whose right value is the STOCK page's, not a guess: platform flags, the
// editor-only bit and the per-item input delay (which the container copies into `_inputDelayedMax`).
// Captured from a real row on the pause page rather than invented -- the stock rows demonstrably work
// on whatever platform this build is.
// Row values to stamp onto the widgets once the rebuild is complete (see stampValues). Filled by the
// guest-page branch of chooseArray. `g_valSet` is a separate flag rather than a sentinel value,
// because a guest's range is allowed to include zero and negatives.
static int   g_sliderPage = -1;
static float g_sliderRow[OMP_PAGEITEM_MAX];
static bool  g_valSet    [OMP_PAGEITEM_MAX];
static int   g_sliderAt  [OMP_PAGEITEM_MAX];

// ---- the replay-editor "Look At" row. A MultiOption cycling the players in the session; choosing one
// aims the replay camera at them (src/game/spectate.cpp). Scrubbing is untouched -- it still drives
// only your own replay. This row lives on a page the mod does not own, so it carries none of the
// g_page sub-page machinery: it is appended to whatever the game built and read back by key.
static const int kSpecMax = 9;                      // "Me" + 8 peers, which is the lobby cap in practice
static uint64_t  g_replayPageKey = 0;
static uint8_t   g_spectateRow[0x90];
static uint64_t  g_spectateKey  = 0;
static FTextBlob g_spectateOpts[kSpecMax];
static char      g_spectateNames[kSpecMax][64];
// ---- Rows are identified by PEER ID, never by name or position.
// The name is a LABEL and nothing else -- two players can choose the same one, so two identical rows
// must still be two different people. And the roster is rebuilt from the live session every time the
// page opens, so a row POSITION is not an identity either: one join or leave and the remembered index
// is quietly pointing at somebody else.
// The actor pointer is not an identity that survives time: it is only valid this instant, so it is
// resolved from the id at the moment of use (session::PeerActorById) and never cached across frames.
static int       g_spectatePeerIds[kSpecMax];       // -1 = "Me"
static int       g_spectateCount = 1;               // always at least "Me"
static int       g_spectateSel   = 0;               // row position, display only -- derived from the id
static int       g_spectateSelPeer = -1;            // THE selection. -1 = your own skater.
// THE NAMES OWN THEIR OWN FTexts, deliberately NOT the shared cachedText table -- the same reason
// the roster panel does, and this list is what proved the reason. That cache is 24 entries for the
// life of the process and never evicts, so a session where people come and go eventually fills it
// with names; after that cachedText returns null forever. The old loop treated null as "stop here",
// which silently TRUNCATED the list: a five-player session offered two names to look at, and the
// only clue was one line in the log an hour earlier.
//
// Each slot owns one blob and rebuilds it ONLY when that slot's string actually changes, so the
// cost is per NEW NAME rather than per page build, and status text can never crowd names out.
static char      g_spectateOptStr[kSpecMax][64] = {};
static FTextBlob g_spectateOptText[kSpecMax]{};
static bool      g_spectateOptHave[kSpecMax] = {};
static FTextBlob g_spectateFallback{};
static bool      g_spectateFallbackHave = false;

// One Look At name. Returns false only if this slot has no usable text at all, which is the one
// case the caller must handle -- and it handles it by showing a placeholder, never by dropping the
// player: somebody you cannot name is still somebody you should be able to watch.
static bool spectateOptText(int i, const char* str) {
    if (i < 0 || i >= kSpecMax || !str) return false;
    if (g_spectateOptHave[i] && strcmp(g_spectateOptStr[i], str) == 0) return true;   // unchanged
    FTextBlob t{};
    if (!makeText(str, &t)) return g_spectateOptHave[i];      // keep last frame's rather than lose it
    g_spectateOptText[i] = t;
    strncpy_s(g_spectateOptStr[i], str, _TRUNCATE);
    g_spectateOptHave[i] = true;
    return true;
}

static const LONG kSpecNoPending = 0x7FFFFFFF;
static volatile LONG g_spectatePending = kSpecNoPending;   // a PEER ID, posted by the menu callback
// ---- the per-player "Sync Replay: Off / On" row. Applies to whoever the Look At row selects; ON
// requests that player's own recent state history over the wire and, once it lands, shows their
// skater in the local replay driven by THEIR data at the scrub position (session::SetPeerReplaySync;
// the transfer itself is src/replication/replaysync.*). The posted value is (peerId << 1) | on, so
// the pump applies the choice to the player it was made for even if the Look At selection moves
// before the game thread runs.
static uint8_t   g_viewRow[0x90];
static uint64_t  g_viewKey = 0;
static FTextBlob g_viewOpts[2];
// ---- "Synced replay length", sitting with Look At and Sync Replay because that is the feature it
// belongs to. A MultiOption, not a slider: both its neighbours are, this page injects a fixed row
// count onto a NATIVE page and has no path to stamp a progress bar's current value onto its widget
// (a slider here builds but shows nothing), and presets are what anyone actually wants -- the dial
// is really "how long am I willing to wait", and every snapshot of their history carries a whole
// skeleton, so the seconds and the wait are the same number.
static const int kSyncLenCount = 5;
static const int kSyncLenSecs[kSyncLenCount] = { 15, 30, 60, 120, 0 };   // 0 = everything they have
static const char* const kSyncLenNames[kSyncLenCount] = { "15 s", "30 s", "60 s", "120 s", "All" };
static uint8_t   g_syncLenRow[0x90];
static uint64_t  g_syncLenKey = 0;
static FTextBlob g_syncLenOpts[kSyncLenCount];
static int       g_syncLenSel = 1;
static int syncLenIndexFromPrefs() {
    const int want = MpPrefs_SyncSeconds();
    int best = 1;
    for (int i = 0; i < kSyncLenCount; i++) if (kSyncLenSecs[i] == want) { best = i; break; }
    return best;
}
static int       g_viewSel = 0;                            // display only, rebuilt with the page
static volatile LONG g_viewPending = kSpecNoPending;

static uint32_t g_tmplPlatforms  = 0xFFFFFFFFu;
static uint8_t  g_tmplEditorOnly = 0;
static float    g_tmplInputDelay = 0.0f;
static bool     g_tmplCaptured   = false;

static void captureTemplate(const TArrayHdr* items) {
    if (g_tmplCaptured || !items || !items->data || items->num <= 0) return;
    const uint8_t* it = (const uint8_t*)items->data;
    g_tmplPlatforms  = *(const uint32_t*)(it + 0x08);
    g_tmplEditorOnly = *(it + 0x0c);
    g_tmplInputDelay = *(const float*)(it + 0x60);
    g_tmplCaptured   = true;
    char m[160];
    snprintf(m, sizeof(m), "[menu] row template from the stock page: platforms=0x%08x inputDelay=%.3f",
             g_tmplPlatforms, g_tmplInputDelay);
    log(m);
}
static void stampTemplate(uint8_t* row) {
    *(uint32_t*)(row + 0x08) = g_tmplPlatforms;
    *(row + 0x0c)            = g_tmplEditorOnly;
    *(float*)(row + 0x60)    = g_tmplInputDelay;
}

static void buildRows() {
    if (g_rowsBuilt) return;
    g_rowsBuilt = true;                                  // one attempt; a failure disables, never retries
    if (!makeFName("PauseMenuPage", &g_pauseKey)) { die("could not intern the pause page key"); return; }
    if (!buildRow(g_rootRow, "OmpMultiplayer", "Multiplayer", "Play with friends", &g_rootRowKey)) {
        die("could not build the Multiplayer row"); return;
    }
    for (int i = 0; i < kMpRowCount; i++) {
        if (!buildRow(g_mpRows + (size_t)i * off::kItemSize, kMpRows[i].key, kMpRows[i].label,
                      kMpRows[i].desc, &g_mpRowKeys[i])) { die("could not build a multiplayer row"); return; }
    }
    // The privacy toggle. A failure disables ONLY this row (g_privacyKey stays 0 and it is never
    // added), exactly like the replay row below -- the connect buttons must not depend on it.
    {
        GuestItem it{};
        strncpy_s(it.key,      "OmpHideAddr", _TRUNCATE);
        strncpy_s(it.label,    "Hide my IP address", _TRUNCATE);
        // Both sides of the trade, because there IS a reason to turn it off: relays add a hop, and a
        // direct link is faster. Saying only the privacy half makes "Off" look like a mistake.
        strncpy_s(it.desc,     "On: traffic goes through Epic's relays, so nobody sees your IP. "
                               "Off: players connect directly -- faster, but they can see it. "
                               "Applies to new connections.", _TRUNCATE);
        strncpy_s(it.offLabel, "Off", _TRUNCATE);
        strncpy_s(it.onLabel,  "On",  _TRUNCATE);
        if (!buildToggleRow(g_privacyRow, it, &g_privacyKey, g_privacyOpt)) {
            g_privacyKey = 0;
            log("[menu] could not build the privacy row -- the toggle is F1-only this run");
        }
    }
    // The player-names page. Every row here fails INDEPENDENTLY -- a key left at 0 is simply never
    // added, so a page with one broken control still offers the other two, and the connect buttons
    // never depend on any of it.
    {
        if (!buildRow(g_namesOpenRow, "OmpNames", "Player names",
                      "Names and chat bubbles above the other players", &g_namesOpenKey))
            g_namesOpenKey = 0;
        // The multiplayer page is a list of things to DO; everything that is a PREFERENCE moves
        // behind this one row. That keeps the four ways of connecting adjacent at the top instead of
        // separated by settings, and takes the page from thirteen rows to nine.
        if (!buildRow(g_otherOpenRow, "OmpOther", "Other options",
                      "Your name, player names, and what the session shares", &g_otherOpenKey))
            g_otherOpenKey = 0;
        // Dropped objects. Built alongside the names page's rows and failing just as independently:
        // this row is a preference, and the buttons that connect you to a session must never depend
        // on one having built.
        static const char* kDropOpts[3] = { "Off", "Live edits only", "Share one set" };
        if (!buildOptionRow(g_dropRow, "OmpDropMode", "Dropped objects",
                            "Whether the rails and ramps you place with the object dropper are shared. "
                            "Share one set shows everybody the same objects: your own are hidden for "
                            "the session and come back when you leave -- nothing is ever deleted.",
                            kDropOpts, 3, &g_dropKey, g_dropOpts))
            g_dropKey = 0;
        static const char* kModeOpts[3] = { "Off", "Off board only", "Always" };
        if (!buildOptionRow(g_nameModeRow, "OmpNameMode", "Show names",
                            "When to show a player's name above their head. Off board only keeps "
                            "your screen clear while you skate. Chat bubbles are not affected.",
                            kModeOpts, 3, &g_nameModeKey, g_nameModeOpts))
            g_nameModeKey = 0;
        // Sliders are built through the same helper the guest seam uses -- the item struct is just
        // the parameter list. Units are METRES: the game prints a slider with "%d", so a range has to
        // be one whose integers mean something.
        GuestItem it{};
        strncpy_s(it.key,   "OmpNameDist", _TRUNCATE);
        strncpy_s(it.label, "Name distance", _TRUNCATE);
        strncpy_s(it.desc,  "How far away a player's name is still shown, in metres", _TRUNCATE);
        it.minValue = (float)MPNAME_DIST_MIN; it.maxValue = (float)MPNAME_DIST_MAX; it.step = 5.0f;
        if (!buildSliderRow(g_nameDistRow, it, &g_nameDistKey)) g_nameDistKey = 0;

        GuestItem b{};
        strncpy_s(b.key,   "OmpBubbleDist", _TRUNCATE);
        strncpy_s(b.label, "Chat bubble distance", _TRUNCATE);
        strncpy_s(b.desc,  "How far away a chat bubble is still shown, in metres. Shorter than names "
                           "on purpose -- a sentence needs far more room to read than a name.", _TRUNCATE);
        b.minValue = (float)MPBUBBLE_DIST_MIN; b.maxValue = (float)MPBUBBLE_DIST_MAX; b.step = 5.0f;
        if (!buildSliderRow(g_bubbleDistRow, b, &g_bubbleDistKey)) g_bubbleDistKey = 0;
    }
    // The replay-editor row. A failure here disables only this row: the page key not interning, or
    // the row not building, must never take the multiplayer menu down with it.
    if (makeFName("ReplayEditors", &g_replayPageKey)) {
        if (buildRow(g_spectateRow, "OmpLookAt", "Look At",
                     "Point the replay camera at another player in your session", &g_spectateKey)) {
            *(g_spectateRow + off::kItemType) = 2;                       // MultiOption
            *(void**)  (g_spectateRow + off::kItemMultiTexts)        = g_spectateOpts;
            *(int32_t*)(g_spectateRow + off::kItemMultiTexts + 0x08) = 1;
            *(int32_t*)(g_spectateRow + off::kItemMultiTexts + 0x0c) = kSpecMax;
            *(int32_t*)(g_spectateRow + off::kItemMultiStart)        = 0;
        } else {
            g_replayPageKey = 0;
            log("[menu] could not build the replay Look At row -- skipped");
        }
        // The view row fails independently: no Look At page, no view row, but never the reverse.
        if (g_replayPageKey &&
            buildRow(g_viewRow, "OmpPeerReplay", "Sync Replay",
                     "Fetch the Look At player's own replay data and show their skater in your "
                     "replay. Lasts until you leave the editor.", &g_viewKey)) {
            const FTextBlob* offT = cachedText("Off");
            const FTextBlob* onT  = cachedText("On");
            if (offT && onT) {
                g_viewOpts[0] = *offT; g_viewOpts[1] = *onT;
                *(g_viewRow + off::kItemType) = 2;                       // MultiOption
                *(void**)  (g_viewRow + off::kItemMultiTexts)        = g_viewOpts;
                *(int32_t*)(g_viewRow + off::kItemMultiTexts + 0x08) = 2;
                *(int32_t*)(g_viewRow + off::kItemMultiTexts + 0x0c) = 2;
                *(int32_t*)(g_viewRow + off::kItemMultiStart)        = 0;
            } else g_viewKey = 0;
        } else g_viewKey = 0;
        // The length row fails independently too: losing it must not cost the page its other rows.
        if (g_replayPageKey &&
            buildRow(g_syncLenRow, "OmpSyncLen", "Synced replay length",
                     "How far back Sync Replay fetches, newest first. Shorter is a much faster "
                     "sync -- 15 s covers a trick, where the whole history is mostly footage "
                     "nobody scrubs back to. Your setting decides your own wait, not theirs.",
                     &g_syncLenKey)) {
            bool okAll = true;
            for (int i = 0; i < kSyncLenCount && okAll; i++) {
                const FTextBlob* t = cachedText(kSyncLenNames[i]);
                if (t) g_syncLenOpts[i] = *t; else okAll = false;
            }
            if (okAll) {
                *(g_syncLenRow + off::kItemType) = 2;                       // MultiOption
                *(void**)  (g_syncLenRow + off::kItemMultiTexts)        = g_syncLenOpts;
                *(int32_t*)(g_syncLenRow + off::kItemMultiTexts + 0x08) = kSyncLenCount;
                *(int32_t*)(g_syncLenRow + off::kItemMultiTexts + 0x0c) = kSyncLenCount;
                *(int32_t*)(g_syncLenRow + off::kItemMultiStart)        = syncLenIndexFromPrefs();
            } else g_syncLenKey = 0;
        } else g_syncLenKey = 0;
    }
    if (!buildRow(g_playersOpenRow, "OmpPlayers", "Players",
                  "Kick or ban someone from the game you are hosting", &g_playersOpenKey) ||
        !buildRow(g_kickRow, "OmpKick", "Kick from this session",
                  "Remove them now. They can join again afterwards.", &g_kickKey) ||
        !buildRow(g_banRow, "OmpBan", "Ban from your sessions",
                  "Remove them now and never host them again", &g_banKey)) {
        die("could not build the moderation rows"); return;
    }
    if (!buildRow(g_browseOpenRow, "OmpJoinOnline", "Join online",
                  "Browse the sessions people are hosting right now", &g_browseOpenKey) ||
        !buildRow(g_refreshRow, "OmpRefresh", "Refresh", "Search again for open sessions", &g_refreshKey)) {
        die("could not build the browser rows"); return;
    }
    for (int p = 0; p < g_nGuests; p++) {
        GuestPage& g = g_guests[p];
        if (!buildRow(g_guestRootRows + (size_t)p * off::kItemSize, g.title, g.title,
                      "Settings for this mod", &g_guestRootKeys[p])) { g.dead = true; continue; }
        for (int i = 0; i < g.n; i++) {
            uint8_t* row = g_guestRows[p] + (size_t)i * off::kItemSize;
            const GuestItem& it = g.items[i];
            bool ok;
            if      (it.kind == OMP_ITEM_TOGGLE) ok = buildToggleRow(row, it, &g_guestItemKeys[p][i], g_optTexts[p][i]);
            else if (it.kind == OMP_ITEM_SLIDER) ok = buildSliderRow(row, it, &g_guestItemKeys[p][i]);
            else                                 ok = buildRow(row, it.key, it.label, it.desc, &g_guestItemKeys[p][i]);
            if (!ok) { g.dead = true; break; }
        }
    }
    char m[120];
    snprintf(m, sizeof(m), "[menu] rows built (multiplayer + %d guest page(s))", g_nGuests);
    log(m);
}

// ---- the roster panel -------------------------------------------------------------------------------
// The wide empty area on the right of a menu page is the container's `_longDescriptionTextBlock` (a
// URichTextBlock at +0x2b8) -- it renders the SELECTED row's `_longDescription`. So showing who is in
// the lobby needs no new widget at all: the list is written into that field on every row of the
// multiplayer page, and the game draws it wherever it normally draws help text.
// Its FTexts do NOT go through cachedText: the roster string changes as people join and leave, and
// that cache is a fixed table which would fill up and stop updating. This owns one blob and rebuilds
// it only when the text actually changes (the old one is left alone -- see the ownership note above).
static char      g_rosterStr[512] = {0};
static FTextBlob g_rosterText{};
static bool      g_rosterHave = false;

static void buildRosterText() {
    const MpUiState& s = g_state;
    char buf[512];
    int n = 0;
    // A private game's whole point is the code, so it leads -- spaced out, because it gets read
    // aloud and typed by hand.
    const char* code = omp::LobbyCode();
    if (code && code[0]) {
        n += snprintf(buf + n, sizeof(buf) - n, "PRIVATE GAME\n\nJOIN CODE:  ");
        for (const char* c = code; *c && n < (int)sizeof(buf) - 8; c++)
            n += snprintf(buf + n, sizeof(buf) - n, "%c ", *c);
        n += snprintf(buf + n, sizeof(buf) - n, "\n\nGive this to whoever you want to play with.\n\n");
    }
    n += snprintf(buf + n, sizeof(buf) - n, "IN THIS SESSION\n\n");
    // The local location comes straight from the world; a peer's rides their cosmetics packet.
    char myMap[64] = {0}, myLabel[64] = {0};
    strncpy_s(myMap, s.myMap, _TRUNCATE);
    if (myMap[0]) PrettyMapName(myMap, myLabel, sizeof(myLabel));
    // Who is running this session. Ownership migrates, so it is asked of the transport every rebuild
    // rather than inferred from who pressed Host -- and it is an IDENTITY, because two players can
    // pick the same name.
    const char* ownerId = omp::LobbyOwnerId();
    n += snprintf(buf + n, sizeof(buf) - n, "%s  (you)%s\n", MpName_Get(),
                  omp::LobbyIsHost() ? "  (host)" : "");
    if (myLabel[0]) n += snprintf(buf + n, sizeof(buf) - n, "   %s\n", myLabel);
    int shown = 0;
    for (int i = 0; i < omp::session::PeerSlots() && n < (int)sizeof(buf) - 64; i++) {
        char who[48] = {0};
        void* actor = nullptr;
        int pid = -1;
        if (!omp::session::PeerAt(i, who, sizeof(who), &actor, &pid)) continue;
        const char* peerId = (pid >= 0) ? omp::PeerIdStr(pid) : "";
        const bool peerIsHost = ownerId[0] && peerId[0] && _stricmp(ownerId, peerId) == 0;
        // A peer whose cosmetics have not landed yet has no name to show -- say so rather than
        // printing a blank line, so "connected but silent" is visibly different from "not there".
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s%s\n",
                      who[0] ? who : "(connecting...)", peerIsHost ? "  (host)" : "",
                      actor ? "" : "   [no skater yet]");
        // Their map, on its own indented line. A DIFFERENT map from yours is the single most useful
        // thing this panel can tell you: the session is fine, you simply cannot see each other.
        char theirMap[64] = {0}, theirLabel[64] = {0};
        if (omp::session::PeerMap(i, theirMap, sizeof(theirMap)) && theirMap[0]) {
            PrettyMapName(theirMap, theirLabel, sizeof(theirLabel));
            const bool elsewhere = (myMap[0] && _stricmp(theirMap, myMap) != 0);
            n += snprintf(buf + n, sizeof(buf) - n, "   %s%s\n",
                          theirLabel[0] ? theirLabel : theirMap, elsewhere ? "   (different map)" : "");
        }
        shown++;
    }
    if (!shown) {
        n += snprintf(buf + n, sizeof(buf) - n, "\nNobody else yet.\n%s",
                      s.armed ? "Waiting for someone to join." : "Host or join to start a session.");
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "\n%d skater%s in the session.", shown + 1,
                      shown ? "s" : "");
    }
    if (strcmp(buf, g_rosterStr) == 0) return;          // unchanged: keep the FText we already built
    strncpy_s(g_rosterStr, buf, _TRUNCATE);
    FTextBlob t;
    if (makeText(g_rosterStr, &t)) { g_rosterText = t; g_rosterHave = true; }
}
// Paste the roster into a row's long description -- the field the right-hand panel renders.
static void setRowRoster(uint8_t* row) {
    if (g_rosterHave) memcpy(row + off::kItemLongDesc, &g_rosterText, sizeof(g_rosterText));
}

// The multiplayer page's HEADING, which is where the "connecting..." acknowledgement lives. It has
// to be able to say "and now you are connected" too, or the acknowledgement is a promise that never
// resolves.
static const char* mpTitle() {
    static char buf[64];
    const MpUiState& s = g_state;
    if (s.armed) {
        if (s.peers <= 0) snprintf(buf, sizeof(buf), "Multiplayer - waiting for players");
        else              snprintf(buf, sizeof(buf), "Multiplayer - %d connected", s.peers);
        return buf;
    }
    if (s.tpState == 1) return "Multiplayer - signing in...";
    if (s.tpState == 3) return "Multiplayer - connection failed";
    if (s.lobby  == -1) return "Multiplayer - nobody hosting";
    return "Multiplayer";
}
// One number that changes whenever anything the page displays changes. Cheaper and more honest than
// comparing the rendered strings: if this is equal, nothing on screen could differ.
static uint32_t sessionSig() {
    const MpUiState& s = g_state;
    // The join code is part of what the page DISPLAYS, so it has to be part of what decides a
    // rebuild -- otherwise a code that arrives without any other state moving would never show.
    uint32_t txt = 0;
    for (const char* c = omp::LobbyCode(); c && *c; c++) txt = txt * 31u + (uint32_t)*c;
    // WHO is the host is on screen too (the roster's "(host)" tag), and ownership can migrate without
    // any other displayed field moving -- so it belongs in here for the same reason the code does. A
    // field that is rendered but not signed is a field that silently goes stale.
    for (const char* c = omp::LobbyOwnerId(); c && *c; c++) txt = txt * 31u + (uint32_t)*c;
    for (const char* c = MpName_Get(); c && *c; c++) txt = txt * 31u + (uint32_t)*c;
    return (uint32_t)(s.armed ? 1 : 0) | ((uint32_t)(s.tpState & 3) << 1)
         | ((uint32_t)(s.lobby & 7) << 3) | ((uint32_t)(s.peers & 31) << 6)
         | ((uint32_t)(s.proxies & 31) << 11) | ((txt & 0xffffu) << 16);
}

// The live session line. Mirrors what the F1 menu says, so the two surfaces never disagree.
static const char* statusLine() {
    static char buf[112];
    const MpUiState& s = g_state;
    if (s.armed) {
        snprintf(buf, sizeof(buf), "Session active - %d player%s connected, %d skater%s drawn",
                 s.peers, s.peers == 1 ? "" : "s", s.proxies, s.proxies == 1 ? "" : "s");
        return buf;
    }
    if (s.tpState == 1) return "Signing in to Epic Online Services";
    if (s.tpState == 3) return "Connection failed - choose Host or Join to retry";
    if (s.lobby == -1)  return "Last attempt failed - is anyone hosting?";
    if (s.backend != 0) return "Ready - choose Host or Join";
    return "Not connected";
}
// Point a row's footer description at the current status, keyed by ROW rather than by string.
//
// Keying by string is what the shared cache does, and it is the wrong axis here: statusLine() alone
// spells out both a player count and a skater count, which is dozens of distinct strings over a long
// session, and a row's status is the most-rewritten text in the menu. Keyed by row it is bounded by
// something that genuinely cannot grow -- there are at most kRowCap rows -- and each row rebuilds
// its text only when its own status actually changes, which is the same rule the roster panel and
// the Look At names follow.
static struct RowStatus {
    uint8_t*  row;
    char      str[112];
    FTextBlob text;
} g_rowStatus[kRowCap];
static int g_rowStatusN = 0;

static void setRowStatus(uint8_t* row, const char* text) {
    if (!row || !text) return;
    RowStatus* e = nullptr;
    for (int i = 0; i < g_rowStatusN; i++) {
        if (g_rowStatus[i].row == row) { e = &g_rowStatus[i]; break; }
    }
    if (e && strcmp(e->str, text) == 0) {                 // unchanged: just re-stamp the same blob
        memcpy(row + off::kItemShortDesc, &e->text, sizeof(e->text));
        return;
    }
    FTextBlob t{};
    if (!makeText(text, &t)) return;                      // leave whatever the row already shows
    if (!e) {
        // Out of slots would mean more live rows than the menu can even build; fall back to stamping
        // an unowned blob rather than dropping the status, and let the next rebuild try again.
        if (g_rowStatusN >= kRowCap) { memcpy(row + off::kItemShortDesc, &t, sizeof(t)); return; }
        e = &g_rowStatus[g_rowStatusN++];
        e->row = row;
    }
    e->text = t;
    strncpy_s(e->str, text, _TRUNCATE);
    memcpy(row + off::kItemShortDesc, &e->text, sizeof(e->text));
}

// ---- one-time page dump -----------------------------------------------------------------------------
// Proves which page is which, how many rows it really has and what `_maxVisibleItems` is -- the cap
// that decides whether an appended row is even built. De-duplicated by page key so a menu that
// rebuilds on every scroll cannot spam the log.
static const int kMaxLoggedPages = 12;
static uint64_t  g_loggedPages[kMaxLoggedPages] = {};
static int       g_nLoggedPages = 0;

// The page geometry that decides whether a page can scroll, logged ONCE per page and NOT behind the
// debug flag: `defs` is `_pageItemDefinitions.Num`, the count the engine's scroll math compares
// against `_maxVisibleItems`. CreatePageItems builds from the array we PASS but never writes that
// field, so on our substituted pages the two disagree -- and that disagreement is the whole reason
// the page will not scroll. Without this line the difference is invisible.
static void dumpGeometry(void* page, const TArrayHdr* items) {
    static uint64_t seen[8] = {}; static int nSeen = 0;
    const uint8_t* p = (const uint8_t*)page;
    const uint64_t id = (uint64_t)(uintptr_t)page ^ (uint64_t)(items ? items->num : 0);
    for (int i = 0; i < nSeen; i++) if (seen[i] == id) return;
    if (nSeen < 8) seen[nSeen++] = id; else return;
    __try {
        char m[240];
        snprintf(m, sizeof(m), "[menu] geometry: our rows=%d | maxVisible=%d headerIndex=%d | "
                 "engine defs num=%d data=%p | widgets=%d",
                 items ? items->num : -1,
                 *(const int32_t*)(p + off::kPageMaxVisible),
                 *(const int32_t*)(p + off::kPageHeaderIndex),
                 *(const int32_t*)(p + off::kPageItemDefs + 8),
                 *(void* const*)(p + off::kPageItemDefs),
                 *(const int32_t*)(p + off::kPageItemWidgets + 8));
        log(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void dumpPage(void* page, const TArrayHdr* items, uint64_t key, const char* keyName) {
    if (!omp::debug::Get().menuPages || g_nLoggedPages >= kMaxLoggedPages) return;
    for (int i = 0; i < g_nLoggedPages; i++) if (g_loggedPages[i] == key) return;
    g_loggedPages[g_nLoggedPages++] = key;
    const uint8_t* p = (const uint8_t*)page;
    char m[300];
    snprintf(m, sizeof(m), "[menu] page '%s' items=%d maxVisible=%d headerIndex=%d defs=%d",
             keyName[0] ? keyName : "?", items ? items->num : -1,
             *(const int32_t*)(p + off::kPageMaxVisible), *(const int32_t*)(p + off::kPageHeaderIndex),
             *(const int32_t*)(p + off::kPageItemDefs + 8));
    log(m);
    if (!items || !items->data) return;
    for (int i = 0; i < items->num && i < 32; i++) {
        const uint8_t* it = (const uint8_t*)items->data + (size_t)i * off::kItemSize;
        char itemName[96]; fnameStr(it + off::kItemKey, itemName, sizeof(itemName));
        snprintf(m, sizeof(m), "[menu]   [%d] '%s' type=%u", i, itemName[0] ? itemName : "?",
                 (unsigned)*(it + off::kItemType));
        log(m);
    }
}

// ---- the injection ----------------------------------------------------------------------------------
// Returns the array CreatePageItems should build from: either the caller's (untouched) or ours.
// `bumpVisible` is set when the substituted list is longer than the page's visible-row cap, because
// CreatePageItems only builds rows [headerIndex, headerIndex + _maxVisibleItems) -- an appended row past
// that cap is silently never created, which would look exactly like "the injection did not work".
static const TArrayHdr* chooseArray(void* page, const TArrayHdr* items, TArrayHdr* ours) {
    const uint8_t* p = (const uint8_t*)page;
    void* def = *(void* const*)(p + off::kPageActiveDef);
    if (!def) return items;
    const uint64_t key = *(const uint64_t*)((const uint8_t*)def + off::kPageDefKey);
    char keyName[96]; keyName[0] = 0;
    if (omp::debug::Get().menuPages) fnameStr(&key, keyName, sizeof(keyName));
    dumpPage(page, items, key, keyName);

    // ---- the replay editor's own page gets ONE appended row. Handled before the pause page so the
    // two paths never interact; this page has no sub-page state of the mod's to maintain.
    if (g_replayPageKey && key == g_replayPageKey) {
        if (!PauseMenu_Tuning().injectRow || !items || !items->data || items->num < 0) return items;
        if (items) captureTemplate(items);
        // Rebuild the option list from the LIVE roster every time the page is built -- opening the
        // menu is exactly when "who is in this session" should be re-read.
        g_spectateCount = 0;
        snprintf(g_spectateNames[0], sizeof(g_spectateNames[0]), "%s", "Me");
        g_spectatePeerIds[0] = -1;                           // -1 = hand the camera back to us
        g_spectateCount = 1;
        for (int s2 = 0; s2 < omp::session::PeerSlots() && g_spectateCount < kSpecMax; s2++) {
            char nm[64] = {0}; void* actor = nullptr; int pid = -1;
            if (!omp::session::PeerAt(s2, nm, sizeof(nm), &actor, &pid) || !actor) continue;
            // Duplicate names are deliberately left duplicated: the list shows what people called
            // themselves, and the id underneath keeps two "Skater"s apart.
            snprintf(g_spectateNames[g_spectateCount], sizeof(g_spectateNames[0]), "%s",
                     nm[0] ? nm : "Player");
            g_spectatePeerIds[g_spectateCount] = pid;
            g_spectateCount++;
        }
        for (int o = 0; o < g_spectateCount; o++) {
            if (spectateOptText(o, g_spectateNames[o])) {
                g_spectateOpts[o] = g_spectateOptText[o];
                continue;
            }
            // No text for this slot. Show a placeholder rather than shortening the list -- the row
            // still carries the right peer id, so the camera goes to the right person even when the
            // label is generic. Dropping them is what the old code did, and it is the worse answer.
            if (!g_spectateFallbackHave) g_spectateFallbackHave = makeText("Player", &g_spectateFallback);
            if (g_spectateFallbackHave) { g_spectateOpts[o] = g_spectateFallback; continue; }
            g_spectateCount = o;                          // truly nothing to draw with
            break;
        }
        if (g_spectateCount < 1) return items;
        // Re-derive the highlighted ROW from the remembered IDENTITY. Whoever you picked keeps being
        // the selection even if peers joined or left and moved them up or down the list; if they have
        // gone, it falls back to "Me" rather than silently landing on whoever inherited their row.
        g_spectateSel = 0;
        for (int o = 0; o < g_spectateCount; o++)
            if (g_spectatePeerIds[o] == g_spectateSelPeer) { g_spectateSel = o; break; }
        if (g_spectateSel == 0) g_spectateSelPeer = -1;
        *(int32_t*)(g_spectateRow + off::kItemMultiTexts + 0x08) = g_spectateCount;
        *(int32_t*)(g_spectateRow + off::kItemMultiStart)        = g_spectateSel;
        const int extra = 1 + (g_viewKey ? 1 : 0) + (g_syncLenKey ? 1 : 0);
        if (items->num + extra > PauseMenu_Tuning().maxItems || items->num + extra > kRowCap) return items;
        uint8_t* out2 = g_rowBuf;
        for (int i = 0; i < items->num; i++)
            memcpy(out2 + (size_t)i * off::kItemSize,
                   (const uint8_t*)items->data + (size_t)i * off::kItemSize, off::kItemSize);
        memcpy(out2 + (size_t)items->num * off::kItemSize, g_spectateRow, off::kItemSize);
        stampTemplate(out2 + (size_t)items->num * off::kItemSize);
        if (g_viewKey) {
            // The row shows the CURRENT Look At player's state, re-read at build time -- opening the
            // menu is when "what am I looking at" gets refreshed, same as the roster above.
            g_viewSel = (g_spectateSelPeer >= 0 &&
                         omp::session::PeerReplaySyncState(g_spectateSelPeer) != 0) ? 1 : 0;
            *(int32_t*)(g_viewRow + off::kItemMultiStart) = g_viewSel;
            memcpy(out2 + (size_t)(items->num + 1) * off::kItemSize, g_viewRow, off::kItemSize);
            stampTemplate(out2 + (size_t)(items->num + 1) * off::kItemSize);
        }
        if (g_syncLenKey) {
            // Re-read at build time like the rows above: opening the menu is when a setting should
            // show what it actually is.
            g_syncLenSel = syncLenIndexFromPrefs();
            *(int32_t*)(g_syncLenRow + off::kItemMultiStart) = g_syncLenSel;
            const int at = items->num + 1 + (g_viewKey ? 1 : 0);
            memcpy(out2 + (size_t)at * off::kItemSize, g_syncLenRow, off::kItemSize);
            stampTemplate(out2 + (size_t)at * off::kItemSize);
        }
        ours->data = out2; ours->num = items->num + extra; ours->max = ours->num;
        return ours;
    }

    if (key != g_pauseKey) return items;                 // not the pause menu: never touched
    g_lastPage = page;                                   // only ever used by the bounded browse poll
    // A rebuild of the root page that this code did not ask for means the game re-activated it (the
    // menu was opened, or navigation returned here) -- so the fake sub-page is no longer on screen.
    if (!g_selfRefresh && g_page != PG_ROOT) { g_page = PG_ROOT; g_browsePolls = 0; }
    if (!PauseMenu_Tuning().injectRow) return items;

    // Capture the stock row template BEFORE we build anything, so even the very first injected row
    // carries the page's own platform flags and input delay rather than our defaults.
    if (items) captureTemplate(items);

    uint8_t* out = g_rowBuf;
    int n = 0;
    // `ours` = stamp the captured template over it. Stock rows are copied verbatim, untouched.
    auto add = [&](const uint8_t* src, bool ours) {
        if (n >= kRowCap) return;
        uint8_t* dst = out + (size_t)n * off::kItemSize;
        memcpy(dst, src, off::kItemSize);
        if (ours) stampTemplate(dst);
        n++;
    };
    if (g_page == PG_ROOT) {
        if (!items || !items->data || items->num < 0) return items;
        int nRootGuests = 0;
        for (int p2 = 0; p2 < g_nGuests; p2++)
            if (!g_guests[p2].dead && !guestIsSubPage(p2)) nRootGuests++;
        if (items->num + 1 + nRootGuests > PauseMenu_Tuning().maxItems ||
            items->num + 1 + nRootGuests > kRowCap) {
            static bool warned = false;
            if (!warned) { warned = true; log("[menu] pause page too long -- injection skipped, not truncated"); }
            return items;
        }
        // Stock rows are copied BITWISE and only read for the duration of this call: the page still
        // owns them and we never destruct our copy, so no refcount is touched either way.
        for (int i = 0; i < items->num; i++)
            add((const uint8_t*)items->data + (size_t)i * off::kItemSize, false);
        if (PauseMenu_Tuning().statusText) setRowStatus(g_rootRow, statusLine());
        add(g_rootRow, true);
        for (int p2 = 0; p2 < g_nGuests; p2++)
            if (!g_guests[p2].dead && !guestIsSubPage(p2))
                add(g_guestRootRows + (size_t)p2 * off::kItemSize, true);
    } else if (g_page == PG_MP) {
        // The roster goes on EVERY row of this page, so the right-hand panel keeps showing it no
        // matter which row the player happens to be sitting on.
        buildRosterText();
        g_privacyAt = -1; g_dropAt = -1;
        for (int i = 0; i < kMpRowCount; i++) {
            uint8_t* row = g_mpRows + (size_t)i * off::kItemSize;
            setRowRoster(row);
            add(row, true);
            // ORDER, deliberately: the four ways of CONNECTING run together at the top, then the
            // session tools, then everything that is a preference behind "Other options".
            //   Host online / Join online / Create private / Join private / Players / Connection /
            //   Other options / Leave session / Back
            if (i == kMpBrowseAfter) { setRowRoster(g_browseOpenRow); add(g_browseOpenRow, true); }
            // "Players" sits with the session actions, and only means anything while hosting.
            if (i == kMpPlayersAfter) { setRowRoster(g_playersOpenRow); add(g_playersOpenRow, true); }
            if (i == kMpPlayersAfter) {
                // The posture, stated on the page itself: it is what you are connected THROUGH,
                // which is worth seeing without opening anything.
                char posture[192], value[32];
                omp::Posture(posture, sizeof(posture));
                const omp::Backend bk = omp::Current();
                snprintf(value, sizeof(value), "%s",
                         (bk == omp::BK_SHM) ? "This PC"
                       : (bk == omp::BK_EOS) ? (omp::RelaysForced() ? "Relayed" : "Direct")
                       : "Not connected");
                if (buildInfoRow(g_postureRow, "OmpPosture", "Connection", posture, value,
                                 &g_postureKey, &g_postureOpt)) {
                    setRowRoster(g_postureRow);
                    add(g_postureRow, true);
                }
                // Your name stays on the MAIN page: it is who you ARE in the session, not a
                // preference to go hunting for. Value column = the live name, so the row answers
                // "who am I?" without being opened.
                if (buildInfoRow(g_nameRow, "OmpName", "Your name",
                                 "Everyone in the session sees this", MpName_Get(),
                                 &g_nameKey, &g_nameOpt)) {
                    setRowRoster(g_nameRow);
                    add(g_nameRow, true);
                }
                if (g_otherOpenKey) { setRowRoster(g_otherOpenRow); add(g_otherOpenRow, true); }
            }
        }
    } else if (g_page == PG_OTHER) {
        // Everything that was cluttering the multiplayer page. Same rows, same storage, same
        // independent-failure rule -- a key left at 0 is simply never added, so one broken control
        // still leaves the others usable.
        buildRosterText();
        g_privacyAt = -1; g_dropAt = -1;
        if (g_namesOpenKey) add(g_namesOpenRow, true);
        if (g_dropKey) {
            // The value goes on the DEFINITION here (so the row reads right even if the stamp is
            // skipped) and onto the widget in stampValues, which is the only point at which it
            // survives the rebuild.
            *(int32_t*)(g_dropRow + off::kItemMultiStart) = MpPrefs_DropMode();
            g_dropAt = n; add(g_dropRow, true);
        }
        if (g_privacyKey) { g_privacyAt = n; add(g_privacyRow, true); }
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);       // the shared Back row
    } else if (g_page == PG_NAMES) {
        // Three settings and a way out. The rows carry their CURRENT values twice over: the option
        // index goes on the definition here (so the row is right even if the stamp is skipped), and
        // stampValues writes both it and the slider positions onto the widgets after the whole
        // rebuild, which is the only point at which they survive (see the note there).
        g_nameModeAt = g_nameDistAt = g_bubbleDistAt = -1;
        if (g_nameModeKey) {
            *(int32_t*)(g_nameModeRow + off::kItemMultiStart) = MpPrefs_NameMode();
            g_nameModeAt = n; add(g_nameModeRow, true);
        }
        if (g_nameDistKey)   { g_nameDistAt   = n; add(g_nameDistRow, true); }
        if (g_bubbleDistKey) { g_bubbleDistAt = n; add(g_bubbleDistRow, true); }
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);       // the shared Back row
    } else if (g_page == PG_PLAYERS) {
        // Everyone in YOUR session. Kick is only offered for a session you host, so say plainly when
        // you do not -- "the button is missing" is never a good explanation.
        g_playerRowCount = 0;
        const bool hosting = omp::LobbyIsHost();
        for (int i = 0; i < omp::session::PeerSlots() && g_playerRowCount < kMaxLobbyRows; i++) {
            char who[48] = {0}; void* actor = nullptr; int pid = -1;
            if (!omp::session::PeerAt(i, who, sizeof(who), &actor, &pid)) continue;
            const char* id = (pid >= 0) ? omp::PeerIdStr(pid) : "";
            if (!id || !*id) continue;                       // no identity yet: nothing to act on
            const int r = g_playerRowCount;
            strncpy_s(g_playerRowIds[r], id, _TRUNCATE);
            strncpy_s(g_playerRowNames[r], who[0] ? who : "(connecting...)", _TRUNCATE);
            // Value column = where they are, the same fact the roster panel reports -- a banned
            // player overrides it, since that is the one thing you came to this page to check.
            char key[48], desc[112], value[64], theirMap[64] = {0}, label[64] = {0};
            snprintf(key, sizeof(key), "OmpPlayer%d", i);
            if (omp::session::PeerMap(i, theirMap, sizeof(theirMap)) && theirMap[0])
                PrettyMapName(theirMap, label, sizeof(label));
            snprintf(value, sizeof(value), "%s",
                     Ban_Is(id) ? "BANNED" : (label[0] ? label : (theirMap[0] ? theirMap : " ")));
            snprintf(desc, sizeof(desc), "%s", hosting ? "Select to kick or ban this player"
                                                       : "Only the host of a session can remove anyone");
            uint8_t* row = g_playerRows + (size_t)r * off::kItemSize;
            if (!buildInfoRow(row, key, g_playerRowNames[r], desc, value,
                              &g_playerRowKeys[r], &g_playerOptText[r])) continue;
            g_playerRowCount++;
            add(row, true);
        }
        if (!g_playerRowCount) {
            setRowStatus(g_playersOpenRow, g_state.armed ? "Nobody else is in your session yet."
                                                         : "You are not in a session.");
            add(g_playersOpenRow, true);
        }
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);   // Back
    } else if (g_page == PG_PLAYER) {
        const bool hosting   = omp::LobbyIsHost();
        const bool banned    = Ban_Is(g_selPeerId);
        // Say WHY a row will not do anything, on the row itself. A button that silently declines is
        // indistinguishable from a broken one. The only reason left is "you are not the host" -- no
        // player is exempt from either action (banlist.h).
        setRowStatus(g_kickRow, hosting ? "Remove them from your session now"
                                        : "You are not the host of this session");
        setRowStatus(g_banRow,  banned ? "Already on your ban list"
                                       : (hosting ? "Remove them and never host them again"
                                                  : "Adds them to your ban list for sessions you host"));
        add(g_kickRow, true);
        add(g_banRow, true);
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);   // Back
    } else if (g_page == PG_BROWSE) {
        add(g_refreshRow, true);
        g_lobbyRowCount = 0;
        const int st = omp::BrowseStatus();
        const int n  = (st == 2) ? omp::BrowseCount() : 0;
        for (int i = 0; i < n && g_lobbyRowCount < kMaxLobbyRows; i++) {
            omp::LobbyInfo L{};
            if (!omp::BrowseAt(i, &L)) continue;
            const int r = g_lobbyRowCount;
            char key[48], label[64], value[32], desc[112];
            snprintf(key,   sizeof(key),   "OmpLobby%d", i);
            // WHO and WHERE, both on the row, because both decide whether you want it: the host is
            // who you would be skating with, the map is where. The lobby advertises the INTERNAL
            // level name and each client resolves it against its own map data, rather than the host
            // advertising a pretty one. A map this install does not have simply shows the raw name,
            // which is still informative.
            char lobbyLabel[64] = {0};
            if (L.map[0]) PrettyMapName(L.map, lobbyLabel, sizeof(lobbyLabel));
            const char* mapText  = lobbyLabel[0] ? lobbyLabel : (L.map[0] ? L.map : "");
            const char* hostText = L.host[0] ? L.host : "";
            if (hostText[0] && mapText[0])      snprintf(label, sizeof(label), "%s  -  %s", hostText, mapText);
            else if (hostText[0])               snprintf(label, sizeof(label), "%s", hostText);
            else if (mapText[0])                snprintf(label, sizeof(label), "%s", mapText);
            else                                snprintf(label, sizeof(label), "Session");   // advertises neither
            snprintf(value, sizeof(value), "%d/%d", L.players, L.maxPlayers > 0 ? L.maxPlayers : 16);
            const bool full = (L.maxPlayers > 0 && L.players >= L.maxPlayers);
            // A different mod version is worth more of this line than anything else on it. Joining
            // still works and still looks like it worked -- the lobby and the P2P link both succeed
            // -- but the snapshots may not parse, and then the other player never appears. Say it
            // here, where the choice is being made. A blank version is a build too old to advertise
            // one, which is itself a mismatch worth flagging rather than treating as unknown.
            const bool verOld  = (L.version[0] == 0);
            const bool verDiff = (!verOld && strcmp(L.version, OMP_VERSION_STRING) != 0);
            if (verDiff)     snprintf(desc, sizeof(desc), "DIFFERENT VERSION (theirs %s, yours %s) - you may not see each other",
                                      L.version, OMP_VERSION_STRING);
            else if (verOld) snprintf(desc, sizeof(desc), "OLDER VERSION - they may not see you. Yours is %s", OMP_VERSION_STRING);
            else             snprintf(desc, sizeof(desc), "%s%d player%s on %s%s", full ? "FULL - " : "",
                                      L.players, L.players == 1 ? "" : "s", mapText[0] ? mapText : "an unknown map",
                                      full ? "" : " - press to join");
            uint8_t* row = g_lobbyRows + (size_t)r * off::kItemSize;
            if (!buildInfoRow(row, key, label, desc, value, &g_lobbyRowKeys[r], &g_lobbyOptText[r])) continue;
            g_lobbyRowIdx[r] = i;
            g_lobbyRowCount++;
            add(row, true);
        }
        // Nothing to show yet: say WHICH nothing. A browser that is merely empty while it is still
        // signing in reads as broken.
        if (n == 0) {
            const char* why = (st == 1) ? "Searching for sessions..."
                            : (st == -1) ? "Search failed - press Refresh to try again"
                            : (g_state.tpState == 1) ? "Signing in to Epic Online Services..."
                            : (st == 2) ? "No sessions found - press Refresh, or Host online yourself"
                            : "Starting up...";
            setRowStatus(g_refreshRow, why);
        }
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);   // the shared "Back" row
    } else {
        const int gi = g_page - PG_GUEST0;
        if (gi < 0 || gi >= g_nGuests || g_guests[gi].dead) { g_page = PG_ROOT; return items; }
        GuestPage& g = g_guests[gi];
        g_sliderPage = gi;                                 // so the post-build pass can find the sliders
        for (int i = 0; i < g.n; i++) {
            uint8_t* row = g_guestRows[gi] + (size_t)i * off::kItemSize;
            const GuestItem& it = g.items[i];
            if (g.onStatus) {
                const char* st = guestStatusGuarded(&g, it.key);
                if (st && *st) setRowStatus(row, st);      // null = leave it alone, per the seam contract
            }
            // Ask the guest what the row is currently set to, so it opens showing the truth. A toggle
            // carries its value on the definition (index at +0x78); a slider's has to be stamped on
            // the WIDGET after it exists, so remember where this one lands.
            g_sliderRow[i] = 0.0f; g_valSet[i] = false;
            if (g.onGet && it.kind != OMP_ITEM_ACTION && it.kind != OMP_ITEM_PAGE) {
                int iv = 0; float fv = 0.0f;
                const int got = guestGetGuarded(&g, it.key, &iv, &fv);
                if (got < 0) { g.dead = true; log("[menu] guest onGet FAULTED -- page disabled"); break; }
                if (got > 0) {
                    // The definition's starting index is the row's opening look; the widget stamp
                    // after the rebuild is what actually survives DeserializePage. Both are set.
                    if (it.kind == OMP_ITEM_TOGGLE) {
                        *(int32_t*)(row + off::kItemMultiStart) = (iv != 0) ? 1 : 0;
                        g_sliderRow[i] = (iv != 0) ? 1.0f : 0.0f;
                    } else {
                        g_sliderRow[i] = fv;
                    }
                    g_valSet[i] = true;
                }
            }
            g_sliderAt[i] = n;                             // the index this row will occupy
            add(row, true);
        }
        // A guest page always gets a way out, even if the guest forgot one.
        add(g_mpRows + (size_t)(kMpRowCount - 1) * off::kItemSize, true);   // the shared "Back" row
    }
    if (n <= 0) return items;
    ours->data = g_rowBuf; ours->num = n; ours->max = n;
    return ours;
}

// A ROW'S DISPLAYED VALUE MUST BE STAMPED AFTER THE *WHOLE* REBUILD, NOT INSIDE THE CreatePageItems
// HOOK. `RefreshItemsPanel` is SerializePage -> clear -> CreatePageItems -> DeserializePage, and
// DeserializePage restores per-row values by calling these very setters
// (`ProgressBarSetPercent` at 0x1072d31, `MultiOptionSetSelectedItemIndex` at 0x1072daa). Stamping
// inside the CreatePageItems hook therefore lands one step too early and is overwritten -- a slider
// stamped there always draws empty.
// A slider's value can ONLY live on the widget (the definition has just min/max/increment). A toggle's
// could ride the definition's _multiOptionStartingIndex, but DeserializePage can stomp that too, so
// both are stamped here for one rule instead of two.
//
// AND NOTHING THAT HAPPENS DURING A REBUILD IS USER INPUT. Both display setters BROADCAST the change
// exactly as a controller press would (`ProgressBarSetPercent` ends in a Broadcast of
// _onProgressBarValueChanged; `MultiOptionSetSelectedItemIndex` likewise) -- and DeserializePage
// calls them with whatever the page's serialize map holds, which for an injected key is nothing,
// i.e. zero. Unguarded, every page build reports "the user just dragged this to the minimum" and
// writes it into the guest's settings, so sliders reset to their minimum on every trip through the
// menu. The guard therefore spans the WHOLE rebuild, not just the mod's own stamp -- the engine's
// writes are no more user input than the mod's are.
static bool g_rebuilding = false;

static void stampValues(void* page) {
    const Syms& S = Get();
    // The privacy row first -- it is not a guest row, and it must show the CURRENT preference
    // whenever the page opens. The definition's starting index does not survive DeserializePage, so
    // the value has to be stamped on the WIDGET after the whole rebuild.
    if (g_privacyAt >= 0 && S.MenuMultiSetIndex) {
        const TArrayHdr* pw = (const TArrayHdr*)((uint8_t*)page + off::kPageItemWidgets);
        if (pw->data && g_privacyAt < pw->num) {
            if (void* widget = ((void**)pw->data)[g_privacyAt])
                S.MenuMultiSetIndex(widget, MpPrefs_HideAddress() ? 1 : 0);
        }
        g_privacyAt = -1;                                 // one shot per build, like the guest pass
    }
    // The dropped-objects row, same page and same argument as the privacy toggle.
    if (g_dropAt >= 0 && S.MenuMultiSetIndex) {
        const TArrayHdr* pw = (const TArrayHdr*)((uint8_t*)page + off::kPageItemWidgets);
        if (pw->data && g_dropAt < pw->num) {
            if (void* widget = ((void**)pw->data)[g_dropAt])
                S.MenuMultiSetIndex(widget, MpPrefs_DropMode());
        }
        g_dropAt = -1;
    }
    // The player-names page, same argument: a slider's value can ONLY live on the widget, and the
    // definition's option index does not survive DeserializePage.
    if (g_nameModeAt >= 0 || g_nameDistAt >= 0 || g_bubbleDistAt >= 0) {
        const TArrayHdr* pw = (const TArrayHdr*)((uint8_t*)page + off::kPageItemWidgets);
        auto widgetAt = [&](int i) -> void* {
            return (pw->data && i >= 0 && i < pw->num) ? ((void**)pw->data)[i] : nullptr;
        };
        auto pct = [](int v, int lo, int hi) {
            const float f = (hi > lo) ? (float)(v - lo) / (float)(hi - lo) : 0.0f;
            return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        };
        if (S.MenuMultiSetIndex)
            if (void* w = widgetAt(g_nameModeAt)) S.MenuMultiSetIndex(w, MpPrefs_NameMode());
        if (S.MenuProgressSetPct) {
            if (void* w = widgetAt(g_nameDistAt))
                S.MenuProgressSetPct(w, pct(MpPrefs_NameDistM(), MPNAME_DIST_MIN, MPNAME_DIST_MAX));
            if (void* w = widgetAt(g_bubbleDistAt))
                S.MenuProgressSetPct(w, pct(MpPrefs_BubbleDistM(), MPBUBBLE_DIST_MIN, MPBUBBLE_DIST_MAX));
        }
        g_nameModeAt = g_nameDistAt = g_bubbleDistAt = -1;   // one shot per build
    }
    if (g_sliderPage < 0) return;
    const int gi = g_sliderPage;
    g_sliderPage = -1;                                    // one shot per build
    if (gi >= g_nGuests || g_guests[gi].dead) return;
    GuestPage& g = g_guests[gi];
    const TArrayHdr* w = (const TArrayHdr*)((uint8_t*)page + off::kPageItemWidgets);
    if (!w->data) return;
    for (int i = 0; i < g.n; i++) {
        const GuestItem& it = g.items[i];
        if (it.kind == OMP_ITEM_ACTION || !g_valSet[i]) continue;
        const int idx = g_sliderAt[i];
        if (idx < 0 || idx >= w->num) continue;           // scrolled out of the built window
        void* widget = ((void**)w->data)[idx];
        if (!widget) continue;
        if (it.kind == OMP_ITEM_SLIDER) {
            if (!S.MenuProgressSetPct) continue;
            float pct = (g_sliderRow[i] - it.minValue) / (it.maxValue - it.minValue);
            if (pct < 0.0f) pct = 0.0f; else if (pct > 1.0f) pct = 1.0f;
            S.MenuProgressSetPct(widget, pct);
        } else {
            if (!S.MenuMultiSetIndex) continue;
            S.MenuMultiSetIndex(widget, (g_sliderRow[i] >= 0.5f) ? 1 : 0);
        }
    }
}

// ---- the hooks --------------------------------------------------------------------------------------
static MenuCreateItemsFn o_CreateItems    = nullptr;
static MenuSelConfirmFn  o_SelConfirm     = nullptr;
static MenuSelConfirmFn  o_MultiChanged   = nullptr;   // same (page, params) shape as the confirm
static MenuSelConfirmFn  o_ProgressChanged = nullptr;
// UMenuPageContainer::HandlePageBackAction(UMenuPageContainer*, UMenuPageDefinition* activePage)
typedef void (*MenuBackActionFn)(void*, void*);
static MenuBackActionFn  o_PageBack       = nullptr;

// UMenuPage::CreatePageItems -- the injection point (pre-hook: the rows must exist before the widgets
// are made). Every deref is inside SEH; on any fault the feature dies quietly and the game's own menu
// carries on with its own array.
static void hkCreateItems(void* page, void* itemsArray, bool flag) {
    const TArrayHdr* use = (const TArrayHdr*)itemsArray;
    TArrayHdr ours{};
    int32_t savedVisible = 0;
    uint8_t* p = (uint8_t*)page;
    bool bumped = false;
    if (!g_dead && PauseMenu_Tuning().enabled && page && itemsArray) {
        __try {
            buildRows();
            if (!g_dead) {
                use = chooseArray(page, (const TArrayHdr*)itemsArray, &ours);
                if (use == &ours && pageScrollsItself()) dumpGeometry(page, use);
                // The bump exists for APPENDING: on a game-owned page we add rows past the end of a
                // list the page already sized its window for, and a row past the window is silently
                // never created. It costs the page its SCROLLBAR, because forcing every row to build
                // at once is precisely what "no scrolling needed" looks like to the engine.
                // Our own full pages hand over the whole array, so they do not need it -- leaving
                // _maxVisibleItems alone lets the engine keep its natural window and scroll the page
                // itself. Scrolling calls RefreshItemsPanel, which re-enters this hook and rebuilds
                // from the same substituted array at the new header index, so the engine walks its
                // own path and only the source array differs.
                if (use == &ours && (!PauseMenu_Tuning().nativeScroll || !pageScrollsItself())) {
                    savedVisible = *(int32_t*)(p + off::kPageMaxVisible);
                    const int32_t need = *(int32_t*)(p + off::kPageHeaderIndex) + ours.num;
                    if (savedVisible < need) { *(int32_t*)(p + off::kPageMaxVisible) = need; bumped = true; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            die("faulted while building menu rows");
            use = (const TArrayHdr*)itemsArray;
        }
    }
    o_CreateItems(page, (void*)use, flag);
    if (bumped) {
        __try { *(int32_t*)(p + off::kPageMaxVisible) = savedVisible; }
        __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted restoring _maxVisibleItems"); }
    }
    // NOTE: row VALUES are deliberately not stamped here -- DeserializePage runs afterwards inside
    // RefreshItemsPanel and would overwrite them. See stampValues.
}

// Re-run the page's row build so the substituted array takes effect. RefreshItemsPanel is the game's
// OWN "rebuild the rows" (it is what scrolling calls), so this is the engine walking its own path --
// only the array it reads is different.
//
// NEVER CALL THIS FROM INSIDE THE CONFIRM CALLBACK. `UMenuPageContainer::OnConfirmAction` broadcasts
// the confirm from the MIDDLE of itself, then carries on using the widget pointers it captured BEFORE
// the broadcast -- and finishes by calling `PlaySoundOnConfirm`, which re-reads
// `_pageItemWidgets[_selectedIndex]` guarded by nothing but a `Num <= 0` test. Rebuilding the rows
// underneath it destroys those widgets and shrinks the array while `_selectedIndex` still points past
// the new end: a use-after-free plus an out-of-bounds read. A page swap is therefore QUEUED and
// performed from the engine-tick pump one frame later -- `UGameEngine::Tick` runs the mod's frame
// BEFORE the engine's, and Slate (hence NativeTick -> OnConfirmAction) runs inside the engine's, so
// the confirm always completes on intact widgets and the rebuild happens before anything touches them
// again. The general rule: do not mutate a structure the caller above you is still walking.
static void refreshPage(void* page) {
    const Syms& S = Get();
    if (!S.MenuRefreshItems) return;
    g_selfRefresh = true;
    // Everything from here to the end of the stamp is the mod rebuilding the page. Any value-change
    // event that arrives in that window -- the mod's own OR the engine's DeserializePage restoring a
    // row -- is not the user touching anything, and must never reach the guest.
    g_rebuilding  = true;
    __try {
        S.MenuRefreshItems(page);
        // The rebuilt list is a DIFFERENT length, so the old selection index is meaningless and, left
        // alone, is an out-of-bounds read for everything that indexes by it (RefreshItemsPanel's own
        // DeserializePage restores the saved one, which is exactly the stale value). -1 first so
        // SetSelectedIndex's deselect-the-old-row branch is skipped, then select the top row properly
        // -- through the game's own setter, so the footer/description broadcast happens as normal.
        *(int32_t*)((uint8_t*)page + off::kPageSelectedIndex) = -1;
        if (S.MenuSetSelIndex) S.MenuSetSelIndex(page, 0, false);
        // ...and only NOW are the row values safe to write: MenuRefreshItems has finished, which
        // means DeserializePage (which drives these same setters) is behind us.
        stampValues(page);
    } __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted refreshing the page"); }
    g_rebuilding  = false;
    g_selfRefresh = false;
}

// The queued page swap. `page` is only ever a pointer the engine handed over on the previous frame,
// and every use is inside SEH.
static void* g_pendingPage  = nullptr;
static bool  g_pendingRefresh = false;
static char  g_pendingTitle[112] = {0};
static bool  g_pendingTitleRoot = false;     // true = restore the root definition's own display name
// AN FText ARGUMENT PASSED BY VALUE IS CONSUMED -- BUMP THE REFCOUNT BEFORE HANDING IT OVER.
// `UMenuPage::SetTitle(this, FText*)` takes its text BY VALUE: MSVC passes such a struct as a pointer
// to a caller-owned temporary, and the callee destroys it. SetTitle copies-with-a-`lock inc` into the
// text block and then runs `lock xadd [refctrl+8], -1` on the argument, calling the destructor if it
// reaches zero. Passing a pointer to storage that was never incremented is therefore a decrement of
// somebody else's reference every single time -- for a sub-page heading the mod's own cached blob,
// and for the "restore the root heading" path the pause page ASSET'S `_displayName`, whose shared
// text data is then destroyed underneath the still-live title widget (it surfaces as a crash in
// `FText::Rebuild` under `STextBlock::ComputeDesiredSize`). The game's own caller does exactly what
// passOwned does (SetActivePageDefinition at +0x117: copy the 24 bytes, `lock inc` the controller,
// pass the copy).
// RULE: FText is a refcounted handle, not a POD. Copying its bytes does not make a reference.
static void passOwned(const void* src, FTextBlob* out) {
    memcpy(out, src, sizeof(FTextBlob));
    void* refCtrl = *(void**)((uint8_t*)out + 8);
    if (refCtrl) InterlockedIncrement((volatile LONG*)((uint8_t*)refCtrl + 8));
}

// The fake sub-page never re-activates a definition, so the heading would still read "PAUSE MENU".
// SetTitle is the game's own setter; passing null restores the root definition's own display name.
// ONE title is on screen at a time, so one blob is all a title needs -- and it must NOT come from
// the shared cache. A player's own name is used as a page title (the per-player page), so every
// different person you opened burned a permanent cache slot; that table filling is what silently
// truncated the replay Look At list. Rebuilt only when the string actually changes, like the roster
// panel, so re-showing the same title costs nothing at all.
static char      g_titleStr[112] = {0};
static FTextBlob g_titleText{};
static bool      g_titleHave = false;

static void setTitle(void* page, const char* text) {
    const Syms& S = Get();
    if (!S.MenuSetTitle) return;
    __try {
        const void* src = nullptr;
        if (text) {
            if (!g_titleHave || strcmp(g_titleStr, text) != 0) {
                FTextBlob t{};
                if (makeText(text, &t)) {
                    g_titleText = t;
                    strncpy_s(g_titleStr, text, _TRUNCATE);
                    g_titleHave = true;
                }
            }
            src = g_titleHave ? &g_titleText : nullptr;
        } else {
            void* def = *(void**)((uint8_t*)page + off::kPageActiveDef);
            if (def) src = (const uint8_t*)def + 0x38;                   // _displayName
        }
        if (!src) return;
        FTextBlob owned;
        passOwned(src, &owned);          // the callee destroys this copy; the source is left intact
        S.MenuSetTitle(page, &owned);
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* cosmetic only -- never worth dying for */ }
}

// Queue a swap/title change for the next frame instead of doing it under the engine's feet.
static void queueSwap(void* page, const char* title, bool rootTitle, bool refresh) {
    g_pendingPage      = page;
    g_pendingRefresh   = refresh;
    g_pendingTitleRoot = rootTitle;
    if (title) strncpy_s(g_pendingTitle, title, _TRUNCATE); else g_pendingTitle[0] = 0;
}

void PauseMenu_Pump() {
    if (g_dead) return;
    // Apply a pending Look At selection here -- on the game thread, outside the engine's menu
    // callbacks.
    {
        const LONG want = InterlockedExchange(&g_spectatePending, kSpecNoPending);
        if (want != kSpecNoPending) {
            // Resolve the identity to an actor HERE, at the moment of use. Between the click and this
            // frame that peer may have left; PeerActorById then returns null and the camera goes back
            // to the local skater instead of a freed pointer reaching the engine.
            void* target = (want < 0) ? nullptr : omp::session::PeerActorById((int)want);
            if (want >= 0 && !target) {
                g_spectateSelPeer = -1;
                log("[menu] the player you selected is no longer in the session -- staying on your"
                    " own skater");
            }
            omp::game::spectate::SetLookTarget(target, &log);
        }
    }
    {
        const LONG want = InterlockedExchange(&g_viewPending, kSpecNoPending);
        if (want != kSpecNoPending) {
            const int  peer = (int)(want >> 1);
            const bool on   = (want & 1) != 0;
            if (!omp::session::PeerActorById(peer)) {
                log("[menu] that player is no longer in the session -- nothing changed");
            } else if (on && omp::game::LocalReplayMode() != 2) {
                // A sync window is anchored to the playback session; outside one there is nothing
                // to anchor to and the buffers would be discarded on entry anyway. Say so.
                log("[menu] Sync Replay applies while you are in a replay -- open one and toggle there");
            } else if (omp::session::SetPeerReplaySync(peer, on)) {
                log(on ? "[menu] Sync Replay ON -- fetching their replay data..."
                       : "[menu] Sync Replay off -- their skater leaves your replay");
            }
        }
    }
    // A search FINISHING is the one thing that changes a page of ours without a button press, so it
    // is polled (an int compare) and turned into a rebuild through the same queue as every other
    // swap -- still outside the engine's menu callbacks.
    // This is the only place a page pointer handed over on an EARLIER frame is touched. It is bounded
    // on purpose: polling stops the moment the search reaches a terminal state, so the pointer is
    // only ever used within the second or two while the user is sitting in the browser they just
    // opened. (Closing and reopening the menu rebuilds the ROOT page, which resets g_page and stops
    // this anyway.) A proper liveness signal would be a hook on the container's menu-close, but its
    // body has a byte-identical twin in the exe and cannot be signatured.
    // Refreshing from this pump is what makes the multiplayer page follow the session in real time --
    // heading, footer status AND the roster panel, so "connecting..." actually resolves and the other
    // skater is visibly seen to arrive. It is safe because `IsPauseMenuDisplayed` gates it: the widget
    // cannot be gone while the game says its menu is on screen.
    if (g_page == PG_MP && !g_pendingPage && g_lastPage && PauseMenuOpen()) {
        const uint32_t sig = sessionSig();
        if (sig != g_lastSig) {
            g_lastSig = sig;
            queueSwap(g_lastPage, mpTitle(), false, true);
        }
    }
    if (g_page == PG_BROWSE && !g_pendingPage && g_lastPage && g_browsePolls > 0) {
        const int st = omp::BrowseStatus();
        if (st != g_lastBrowseState) {
            g_lastBrowseState = st;
            if (st != 1) g_browsePolls = 0;             // terminal: this is the last rebuild we owe
            else         g_browsePolls--;
            queueSwap(g_lastPage, "Online sessions", false, true);
        }
    }
    if (!g_pendingPage) return;
    void* page = g_pendingPage;
    g_pendingPage = nullptr;                    // one shot, whatever happens below
    if (g_pendingRefresh) refreshPage(page);
    if (g_pendingTitleRoot)   setTitle(page, nullptr);
    else if (g_pendingTitle[0]) setTitle(page, g_pendingTitle);
    g_pendingRefresh = false; g_pendingTitle[0] = 0; g_pendingTitleRoot = false;
}

// The menu's own "you went back" blip. Taking a step back in silence sounds like a dropped input, so
// our Back plays the same sound the stock one does -- and reaches it the same way the stock code does,
// through the container's vtable, because a byte signature CANNOT find this function: PlaySoundOnCancel,
// PlaySoundOnClose and every other PlaySoundOn* in the family are byte-identical apart from the
// RIP-relative displacements a signature has to wildcard. (Slot 0x510 is read straight off the
// disassembly of HandlePageBackAction, which calls it two instructions before it pops the page stack.)
//
// A vtable index is a weaker thing to depend on than a signature, so the slot proves itself before it
// is ever called: the family has a distinctive 23-byte prologue -- save, frame, load _audioSet from
// this+0x368, null-test -- and anything that does not start with those exact bytes is not a
// "play a UI sound from a field on this container" function and is left alone. Verified once and
// remembered; a failed check costs the blip and nothing else.
static void playCancelSound(void* container) {
    static int state = 0;                                    // 0 unchecked, 1 usable, -1 refused
    static void* fn  = nullptr;
    if (state < 0) return;
    __try {
        if (!state) {
            static const uint8_t kPrologue[] = {
                0x48,0x89,0x5C,0x24,0x08,             // mov  [rsp+8], rbx
                0x57,                                 // push rdi
                0x48,0x83,0xEC,0x20,                  // sub  rsp, 0x20
                0x48,0x8B,0xB9,0x68,0x03,0x00,0x00,   // mov  rdi, [rcx+0x368]   (_audioSet)
                0x48,0x8B,0xD9,                       // mov  rbx, rcx
                0x48,0x85,0xFF,                       // test rdi, rdi
            };
            void* cand = (*(void***)container)[0x510 / 8];
            state = (cand && memcmp(cand, kPrologue, sizeof(kPrologue)) == 0) ? 1 : -1;
            if (state > 0) fn = cand;
            else log("[menu] the menu's cancel sound could not be identified -- Back will be silent");
        }
        if (state > 0) ((void (*)(void*))fn)(container);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        state = -1;                                          // cosmetic: never try again, never die
    }
}

// ONE STEP BACK out of whichever page of ours is showing -- from the browser to Multiplayer, from a
// guest sub-page to the page that opened it, from anywhere else to the pause menu proper. Two callers
// reach it: the "Back" row, and the gamepad's own back button (hkPageBack). It is deliberately a
// single function so the two can never disagree about where Back goes.
// Returns false when there is nothing of ours to leave, which is the caller's cue to let the stock
// behaviour happen -- for the button that means closing the menu, exactly as it does today.
static bool navigateBack(void* page) {
    if (g_page == PG_ROOT || !page) return false;
    g_browsePolls = 0;
    if (g_page >= PG_GUEST0) {
        const int gi = g_page - PG_GUEST0;
        const int up = (gi < g_nGuests) ? g_guests[gi].parent : -1;
        if (up >= 0 && up < g_nGuests && !g_guests[up].dead) {
            g_guests[gi].parent = -1;                  // one step per entry, never a stale chain
            g_page = PG_GUEST0 + up;
            queueSwap(page, g_guests[up].title, false, true);
            return true;
        }
    }
    if (g_page == PG_PLAYER)  { g_page = PG_PLAYERS; queueSwap(page, "Players", false, true); return true; }
    // PG_NAMES now sits under PG_OTHER, so Back from it returns THERE, not to the MP page.
    if (g_page == PG_NAMES) { g_page = PG_OTHER; queueSwap(page, "Other options", false, true); return true; }
    const bool toMp = (g_page == PG_BROWSE || g_page == PG_PLAYERS || g_page == PG_OTHER);
    g_page = toMp ? PG_MP : PG_ROOT;
    if (toMp) g_lastSig = sessionSig();
    queueSwap(page, toMp ? mpTitle() : nullptr, !toMp, true);
    return true;
}

// UMenuPage::OnSelectionConfirmed -- the page-level funnel every confirm passes through. A row of ours
// is handled here and the trampoline is NOT called, which suppresses the stock chain entirely; anything
// else falls straight through untouched.
static bool handleConfirm(void* page, void* params) {
    if (g_dead || !PauseMenu_Tuning().enabled || !page || !params) return false;
    const uint64_t itemKey = *(const uint64_t*)((const uint8_t*)params + off::kSelParamsItem + off::kItemKey);
    if (!itemKey) return false;

    if (g_page == PG_ROOT) {
        if (itemKey == g_rootRowKey && PauseMenu_Tuning().subPage) {
            g_page = PG_MP;
            g_lastSig = sessionSig();
            queueSwap(page, mpTitle(), false, true);
            log("[menu] pause: entered the multiplayer page");
            return true;
        }
        for (int p = 0; p < g_nGuests; p++) {
            if (g_guests[p].dead || itemKey != g_guestRootKeys[p]) continue;
            g_page = PG_GUEST0 + p;
            queueSwap(page, g_guests[p].title, false, true);
            char m[160];
            snprintf(m, sizeof(m), "[menu] pause: entered the guest page '%s'", g_guests[p].title);
            log(m);
            return true;
        }
        return false;
    }

    // A row inside a guest page that opens another of that guest's pages. Checked before the shared
    // Back row so a sub-page's own rows are resolved on the page they belong to.
    if (g_page >= PG_GUEST0) {
        const int gi = g_page - PG_GUEST0;
        if (gi < g_nGuests && !g_guests[gi].dead) {
            for (int i = 0; i < g_guests[gi].n; i++) {
                if (g_guests[gi].items[i].kind != OMP_ITEM_PAGE) continue;
                if (itemKey != g_guestItemKeys[gi][i]) continue;
                const int target = guestPageByTitle(g_guests[gi].items[i].key);
                if (target < 0) return true;               // names nothing: inert, never wrong
                g_guests[target].parent = gi;              // Back returns the way we came in
                g_page = PG_GUEST0 + target;
                queueSwap(page, g_guests[target].title, false, true);
                char m[160];
                snprintf(m, sizeof(m), "[menu] pause: entered the guest sub-page '%s'",
                         g_guests[target].title);
                log(m);
                return true;
            }
        }
    }

    // "Back" is shared by every one of our pages -- and the B button reaches the same code, so the
    // row and the button can never drift apart (see hkPageBack).
    if (itemKey == g_mpRowKeys[kMpRowCount - 1]) { navigateBack(page); return true; }
    if (g_page == PG_MP && itemKey == g_browseOpenKey) {
        g_page = PG_BROWSE;
        post(OVA_BROWSE);                       // MpPump brings EOS up if needed, then searches
        g_lastBrowseState = -99;                // force the next poll to notice whatever happens
        g_browsePolls = 8;                      // bounded: sign-in -> searching -> results
        queueSwap(page, "Online sessions", false, true);
        log("[menu] pause: opened the lobby browser");
        return true;
    }
    if (g_page == PG_MP && g_otherOpenKey && itemKey == g_otherOpenKey) {
        g_page = PG_OTHER;
        queueSwap(page, "Other options", false, true);
        log("[menu] pause: opened the other options");
        return true;
    }
    if (g_page == PG_MP && g_nameKey && itemKey == g_nameKey) {
        post(OVA_SET_NAME);              // the loader opens the box; the box saves and closes itself
        log("[menu] pause: opening the name box");
        return true;
    }
    if (g_page == PG_OTHER && g_namesOpenKey && itemKey == g_namesOpenKey) {
        g_page = PG_NAMES;
        queueSwap(page, "Player names", false, true);
        log("[menu] pause: opened the player-names settings");
        return true;
    }
    // Every other row on this page is a toggle or a slider: the confirm is the engine acknowledging
    // the row, not a command. Swallow it rather than letting it fall through to the stock chain.
    if (g_page == PG_NAMES || g_page == PG_OTHER) return true;
    if (g_page == PG_MP && itemKey == g_playersOpenKey) {
        g_page = PG_PLAYERS;
        queueSwap(page, "Players", false, true);
        log("[menu] pause: opened the player list");
        return true;
    }
    if (g_page == PG_PLAYERS) {
        for (int r = 0; r < g_playerRowCount; r++) {
            if (itemKey != g_playerRowKeys[r]) continue;
            // Remember WHO by identity, not by row: the list rebuilds live.
            strncpy_s(g_selPeerId,   g_playerRowIds[r],   _TRUNCATE);
            strncpy_s(g_selPeerName, g_playerRowNames[r], _TRUNCATE);
            g_page = PG_PLAYER;
            queueSwap(page, g_selPeerName, false, true);
            return true;
        }
        return true;
    }
    if (g_page == PG_PLAYER) {
        const bool kick = (itemKey == g_kickKey), ban = (itemKey == g_banKey);
        if (kick || ban) {
            strncpy_s(g_actPeerId,   g_selPeerId,   _TRUNCATE);
            strncpy_s(g_actPeerName, g_selPeerName, _TRUNCATE);
            InterlockedExchange(&g_havePeerAction, 1);
            post(ban ? OVA_BAN : OVA_KICK);
            char m[200];
            snprintf(m, sizeof(m), "[menu] pause: %s '%s'", ban ? "BAN" : "KICK", g_selPeerName);
            log(m);
            g_page = PG_PLAYERS;
            queueSwap(page, "Players", false, true);
            return true;
        }
        return true;
    }
    if (g_page == PG_BROWSE) {
        if (itemKey == g_refreshKey) {
            post(OVA_BROWSE);
            g_lastBrowseState = -99;
            g_browsePolls = 8;
            queueSwap(page, "Online sessions", false, true);
            return true;
        }
        for (int r = 0; r < g_lobbyRowCount; r++) {
            if (itemKey != g_lobbyRowKeys[r]) continue;
            g_browsePolls = 0;                  // leaving the list; stop rebuilding it
            InterlockedExchange(&g_pendingJoinIdx, g_lobbyRowIdx[r]);
            post(OVA_JOIN_INDEX);
            // HAND OFF TO THE MULTIPLAYER PAGE rather than sitting on the browser saying "Joining...".
            // The browse page has no live refresh once its poll budget is spent, so a heading written
            // there is never updated and a successful join looks identical to a hung one. The MP page
            // follows the session in real time (the block in PauseMenu_Pump above: heading, footer and
            // roster all re-render on a state change), which is the acknowledgement a join needs.
            g_page = PG_MP;
            g_lastSig = 0xffffffffu;            // unequal to anything: force the first live refresh
            queueSwap(page, "Joining...", false, true);
            return true;
        }
        return true;                            // any other row on our page: swallow, never fall through
    }
    if (g_page == PG_MP) {
        for (int i = 0; i < kMpRowCount - 1; i++) {
            if (itemKey != g_mpRowKeys[i]) continue;
            post(kMpRows[i].act);        // performed by MpPump on this same thread, next frame
            char m[160];
            snprintf(m, sizeof(m), "[menu] pause: '%s' selected", kMpRows[i].label);
            log(m);
            // ACKNOWLEDGE IMMEDIATELY: a click that looks like nothing happened is the worst possible
            // outcome, and the real status cannot appear yet because the action itself does not run
            // until MpPump drains it next frame. The heading therefore states what was just accepted,
            // from a value already known, and the live poll in PauseMenu_Pump replaces it with the
            // real state a moment later.
            char ack[128];
            snprintf(ack, sizeof(ack), "Multiplayer - %s...",
                     (kMpRows[i].act == OVA_LEAVE) ? "leaving" : "connecting");
            queueSwap(page, ack, false, false);      // title only -- no rebuild, so no index churn
            g_lastSig = 0xffffffffu;                 // force the next poll to publish the real state
            return true;
        }
        return false;
    }
    const int gi = g_page - PG_GUEST0;
    if (gi >= 0 && gi < g_nGuests && !g_guests[gi].dead) {
        GuestPage& g = g_guests[gi];
        for (int i = 0; i < g.n; i++) {
            if (itemKey != g_guestItemKeys[gi][i]) continue;
            // Only ACTION rows are "pressed" -- a confirm landing on a toggle or slider is the engine
            // acknowledging the row, not a command, and onSelect may legitimately be null for a page
            // made entirely of controls.
            if (g.items[i].kind != OMP_ITEM_ACTION || !g.onSelect) return true;
            if (!guestSelectGuarded(&g, g.items[i].key)) {
                g.dead = true;
                char m[160];
                snprintf(m, sizeof(m), "[menu] guest page '%s' FAULTED -- disabled for this run", g.title);
                log(m);
                g_page = PG_ROOT;
            }
            // Rebuild next frame so the guest's status text updates on the rows.
            queueSwap(page, nullptr, g_page == PG_ROOT, true);
            return true;
        }
    }
    return false;
}

static void hkSelConfirm(void* page, void* params) {
    bool handled = false;
    __try { handled = handleConfirm(page, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted handling a confirm"); handled = false; }
    if (!handled) o_SelConfirm(page, params);
}

// UMenuPageContainer::HandlePageBackAction -- what the container runs for EKeys::Virtual_Back, the
// platform's back button (B on a controller), one level above the page. The stock body pops the
// container's own `_pageStack` (+0x308, count at +0x310) and CLOSES THE WHOLE MENU when that stack is
// empty. Our sub-pages are never on it: they are row swaps performed on the one real page the
// container already owns, so as far as the engine is concerned nothing was ever navigated and back
// was always a close. That is why the "Back" ROW worked and the button did not.
//
// So: if a page of ours is showing, the button does exactly what that row does and the stock body
// never runs. Everywhere else -- the pause root, the game's own sub-pages, any other menu in the game
// -- the trampoline runs untouched and back keeps closing or popping as it always has.
//
// Two guards decide "a page of ours", and both must hold. The definition the container hands us must
// be the pause page (its `_key`, the same FName the row injection keys off), which rules out every
// other menu in the game; and `g_page` must not be the root. The key test is what makes this safe
// after the menu is closed from one of our sub-pages by some other means: `g_page` still says
// "Multiplayer" until the pause page is next built, and without the key check a back press in an
// unrelated menu would be answered by swapping rows on a page that is no longer on screen.
static void hkPageBack(void* container, void* pageDef) {
    bool handled = false;
    __try {
        if (!g_dead && PauseMenu_Tuning().enabled && container && pageDef && g_page != PG_ROOT &&
            *(const uint64_t*)((const uint8_t*)pageDef + off::kPageDefKey) == g_pauseKey) {
            // The page the container is actually showing, rather than the one we last built: they are
            // the same object in practice, and taking it from the container is what makes that true
            // rather than assumed.
            void* page = *(void**)((uint8_t*)container + off::kContainerPage);
            if (page) {
                handled = navigateBack(page);
                if (handled) playCancelSound(container);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted handling the back button"); handled = false; }
    if (!handled) o_PageBack(container, pageDef);
}

// ---- value changes (toggle / slider) ----------------------------------------------------------------
// Both funnels are the confirm's twins: (page, params) with the item definition inline at +0x08 and
// old/new in the two trailing slots. The change is only ever REPORTED to the guest -- the engine has
// already updated the row's own state, so nothing here rebuilds anything, which is what keeps this
// clear of the "do not mutate what the caller is still walking" rule.
static bool handleValueChange(void* params, bool isSlider) {
    // See the note on g_rebuilding: during a rebuild these events are the display being restored, not
    // the user changing anything. Reporting them writes the MINIMUM into every slider on every menu
    // open.
    if (g_rebuilding) return false;
    if (g_dead || !PauseMenu_Tuning().enabled || !params) return false;
    // The replay-editor row is on a FOREIGN page, so it is matched by key before any guest-page
    // logic. The work is POSTED, not done here: aiming calls into the replay camera, and the engine
    // is still walking its own widgets underneath at this point.
    {
        const uint64_t k = *(const uint64_t*)((const uint8_t*)params + off::kSelParamsItem + off::kItemKey);
        // The privacy toggle. Matched by key before any guest-page logic, same as the replay row --
        // it is a first-party row, not a guest's. The write is pure storage; the game thread's pump
        // turns it into the SDK call (mp_prefs.h), so nothing here touches EOS.
        if (g_privacyKey && k == g_privacyKey && !isSlider) {
            const int idx = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
            // An engine echo of the value just stamped is not a user action. MpPrefs_SetHideAddress
            // already early-outs on a no-op write, so this is belt and braces.
            if ((idx != 0) != MpPrefs_HideAddress()) {
                MpPrefs_SetHideAddress(idx != 0);
                char m[96];
                snprintf(m, sizeof(m), "[menu] 'hide my address' -> %s", idx ? "On" : "Off");
                log(m);
            }
            return true;
        }
        // The player-names page. First-party rows, matched by key like the privacy toggle. The
        // setters no-op on an unchanged value, so an engine echo that slips past g_rebuilding
        // cannot churn the settings file either.
        if (g_nameModeKey && k == g_nameModeKey && !isSlider) {
            MpPrefs_SetNameMode(*(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew));
            return true;
        }
        if (g_syncLenKey && k == g_syncLenKey && !isSlider) {
            const int idx = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
            if (idx >= 0 && idx < kSyncLenCount) {
                g_syncLenSel = idx;
                MpPrefs_SetSyncSeconds(kSyncLenSecs[idx]);
            }
            return true;
        }
        if (g_dropKey && k == g_dropKey && !isSlider) {
            MpPrefs_SetDropMode(*(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew));
            return true;
        }
        if (isSlider && (k == g_nameDistKey || k == g_bubbleDistKey)) {
            // NewPercent is the normalised bar position; the displayed number -- and the value the
            // player thinks they chose -- is min + pct*(max-min) rounded, which is exactly what the
            // game prints.
            const float pct = *(const float*)((const uint8_t*)params + off::kChangeParamsNew);
            if (k == g_nameDistKey && g_nameDistKey) {
                const float v = MPNAME_DIST_MIN + pct * (float)(MPNAME_DIST_MAX - MPNAME_DIST_MIN);
                MpPrefs_SetNameDistM((int)(v + 0.5f));
            } else if (g_bubbleDistKey) {
                const float v = MPBUBBLE_DIST_MIN + pct * (float)(MPBUBBLE_DIST_MAX - MPBUBBLE_DIST_MIN);
                MpPrefs_SetBubbleDistM((int)(v + 0.5f));
            }
            return true;
        }
        if (g_viewKey && k == g_viewKey && !isSlider) {
            const int idx = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
            if (idx == g_viewSel) return true;               // engine echo of the current value
            g_viewSel = idx;
            if (g_spectateSelPeer < 0) {
                log("[menu] pick a player in Look At first -- Sync Replay applies to them");
                return true;
            }
            InterlockedExchange(&g_viewPending, (LONG)((g_spectateSelPeer << 1) | (idx ? 1 : 0)));
            return true;
        }
        if (g_spectateKey && k == g_spectateKey && !isSlider) {
            const int idx = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
            // BUILDING the row fires a change event reporting its initial index, and `g_rebuilding`
            // does not cover it -- that flag wraps the mod's OWN page rebuilds, and this row lives on
            // a page the game builds. Without this guard, merely opening the pause menu in the replay
            // editor reads as "the user selected Me" and re-aims the camera moments after the page
            // appears, throwing away wherever they had it. A selection that does not CHANGE the
            // selection is not a user action: ignore it (still consumed, so the engine sees it
            // handled). The same rule holds for guest-page rows -- never treat an engine-generated
            // echo of the current value as input.
            if (idx == g_spectateSel) return true;
            if (idx >= 0 && idx < g_spectateCount) {
                // Post the IDENTITY, not the row. The pump runs on a later frame, by which time the
                // roster may have changed underneath this index -- and posting a row number would
                // then aim at whoever now occupies it.
                g_spectateSel     = idx;
                g_spectateSelPeer = g_spectatePeerIds[idx];
                InterlockedExchange(&g_spectatePending, (LONG)g_spectateSelPeer);
            }
            return true;
        }
    }
    const int gi = g_page - PG_GUEST0;
    if (gi < 0 || gi >= g_nGuests || g_guests[gi].dead) return false;
    GuestPage& g = g_guests[gi];
    if (!g.onValue) return false;
    const uint64_t itemKey = *(const uint64_t*)((const uint8_t*)params + off::kSelParamsItem + off::kItemKey);
    for (int i = 0; i < g.n; i++) {
        if (itemKey != g_guestItemKeys[gi][i]) continue;
        const GuestItem& it = g.items[i];
        int   iv = 0;
        float fv = 0.0f;
        if (isSlider) {
            // NewPercent is the normalised bar position; hand the guest its own units back.
            const float pct = *(const float*)((const uint8_t*)params + off::kChangeParamsNew);
            fv = it.minValue + pct * (it.maxValue - it.minValue);
            iv = (int)fv;
        } else {
            iv = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
            fv = (float)iv;
        }
        if (!guestValueGuarded(&g, it.key, iv, fv)) {
            g.dead = true;
            log("[menu] guest onValue FAULTED -- page disabled for this run");
        } else {
            // Capped, but ON by default: a phantom change looks exactly like a real one in the
            // guest's settings file, so this log is the only way to see one happening.
            static int logged = 0;
            if (logged < 12) {
                logged++;
                char m[200];
                snprintf(m, sizeof(m), "[menu] '%s' changed -> %s%d", it.key, isSlider ? "" : "index ",
                         isSlider ? (int)fv : iv);
                log(m);
            }
        }
        return true;
    }
    return false;
}
static void hkMultiChanged(void* page, void* params) {
    __try { handleValueChange(params, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted handling a toggle change"); }
    o_MultiChanged(page, params);      // always through: the engine still has to update the row
}
static void hkProgressChanged(void* page, void* params) {
    __try { handleValueChange(params, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { die("faulted handling a slider change"); }
    o_ProgressChanged(page, params);
}

// ---- install ----------------------------------------------------------------------------------------
void PauseMenu_Install() {
    if (g_installed) return;
    g_installed = true;
    const Syms& S = Get();
    // Readiness ENUMERATES its dependencies: "menu disabled" must name the missing symbol, never
    // leave the reader to guess which of seven it was.
    struct Dep { const char* name; const void* p; };
    const Dep deps[] = {
        { "MenuCreateItems",  (const void*)S.MenuCreateItems },
        { "MenuSelConfirmed", (const void*)S.MenuSelConfirmed },
        { "MenuRefreshItems", (const void*)S.MenuRefreshItems },
        { "MenuSetSelIndex",  (const void*)S.MenuSetSelIndex },
        { "FText::FromName",  (const void*)S.TextFromName },
        { "FNameCtor",        (const void*)S.FNameCtor },
        { "FNameToString",    (const void*)S.FNameToString },
    };
    char missing[220] = {0};
    for (const Dep& d : deps) {
        if (d.p) continue;
        if (missing[0]) strncat_s(missing, ", ", _TRUNCATE);
        strncat_s(missing, d.name, _TRUNCATE);
    }
    if (missing[0]) {
        char m[300]; snprintf(m, sizeof(m), "unresolved: %s", missing);
        die(m);
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { die("MinHook init failed"); return; }
    if (MH_CreateHook(S.MenuCreateItems, (void*)&hkCreateItems, (void**)&o_CreateItems) != MH_OK ||
        MH_EnableHook(S.MenuCreateItems) != MH_OK) { die("CreatePageItems hook failed"); return; }
    if (MH_CreateHook(S.MenuSelConfirmed, (void*)&hkSelConfirm, (void**)&o_SelConfirm) != MH_OK ||
        MH_EnableHook(S.MenuSelConfirmed) != MH_OK) { die("OnSelectionConfirmed hook failed"); return; }
    // OPTIONAL, like the funnels below: without it the pages still work, the back button just closes
    // the menu from one of ours instead of stepping back out of it -- which is the old behaviour.
    bool backBtn = false;
    if (S.MenuBackAction &&
        MH_CreateHook(S.MenuBackAction, (void*)&hkPageBack, (void**)&o_PageBack) == MH_OK &&
        MH_EnableHook(S.MenuBackAction) == MH_OK)
        backBtn = true;
    // The two value-change funnels are OPTIONAL: without them a guest page still works, it just
    // cannot own toggles or sliders. Announce the loss rather than dying for it.
    bool values = false;
    if (S.MenuMultiChanged && S.MenuProgressChanged &&
        MH_CreateHook(S.MenuMultiChanged, (void*)&hkMultiChanged, (void**)&o_MultiChanged) == MH_OK &&
        MH_EnableHook(S.MenuMultiChanged) == MH_OK &&
        MH_CreateHook(S.MenuProgressChanged, (void*)&hkProgressChanged, (void**)&o_ProgressChanged) == MH_OK &&
        MH_EnableHook(S.MenuProgressChanged) == MH_OK)
        values = true;
    char m[260];
    snprintf(m, sizeof(m), "[menu] pause-menu integration armed (a 'Multiplayer' row joins the pause menu%s%s)",
             values ? "; toggle/slider rows available to guest pages" : "; NO toggle/slider rows -- change funnels unresolved",
             backBtn ? "; the back button steps back through our pages"
                     : "; NO back button -- HandlePageBackAction unresolved, B still closes the menu");
    log(m);
}

void PauseMenu_Publish(const MpUiState* s) { if (s) g_state = *s; }

bool PauseMenu_TakePeerId(char* idOut, int idCap, char* nameOut, int nameCap) {
    if (!InterlockedExchange(&g_havePeerAction, 0)) return false;
    if (idOut   && idCap   > 0) strncpy_s(idOut,   (size_t)idCap,   g_actPeerId,   _TRUNCATE);
    if (nameOut && nameCap > 0) strncpy_s(nameOut, (size_t)nameCap, g_actPeerName, _TRUNCATE);
    return idOut ? (idOut[0] != 0) : true;
}

int PauseMenu_TakeJoinIndex() {
#ifdef _WIN32
    return (int)InterlockedExchange(&g_pendingJoinIdx, -1);
#else
    const int i = (int)g_pendingJoinIdx; g_pendingJoinIdx = -1; return i;
#endif
}

int PauseMenu_TakeAction() {
#ifdef _WIN32
    return (int)InterlockedExchange(&g_pending, OVA_NONE);
#else
    const int a = (int)g_pending; g_pending = OVA_NONE; return a;
#endif
}
