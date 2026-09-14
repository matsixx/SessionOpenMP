// Custom maps in the Select Map screen -- see custom_maps.h.
#include "custom_maps.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif

namespace omp::game::maps {

namespace {
constexpr int kMaxEntries = 64;
struct Entry {
    char label[64];     // what the node list shows
    char name[128];     // the level's own name (the file name) -- doubles as the node's portal id
    char path[300];     // the package path OpenLevel loads: /Game/CustomMaps/...
    char rel[300];      // under CustomMaps, backslashes, extension kept: Author\Map\Map\Map.umap
    char dir[300];      // the folder the level sits in, same form: Author\Map\Map
};
Entry g_entries[kMaxEntries];
int   g_count = 0;
void (*g_logf)(const char*) = nullptr;

void logv(const char* fmt, ...) {
    if (!g_logf) return;
    char m[420];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    g_logf(m);
}

// ---- the folder scan -------------------------------------------------------------------------------
#ifdef _WIN32
// <exe dir>\..\..\Content -- the exe lives in SessionGame\Binaries\Win64.
bool contentRoot(wchar_t* out, int cap) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    for (int up = 0; up < 3; up++) {                    // file name, Win64, Binaries
        wchar_t* s = wcsrchr(exe, L'\\');
        if (!s) return false;
        *s = 0;
    }
    return swprintf(out, cap, L"%s\\Content", exe) > 0;
}

// The first `cap` bytes of a file (all of it when smaller), malloc'd and NUL-terminated.
bool readHead(const wchar_t* path, char** out, size_t* len, size_t cap) {
    *out = nullptr; *len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return false; }
    const DWORD want = (DWORD)((uint64_t)sz.QuadPart < cap ? (uint64_t)sz.QuadPart : cap);
    char* b = (char*)malloc((size_t)want + 1);
    DWORD got = 0;
    const bool ok = b && ReadFile(h, b, want, &got, nullptr) && got > 0;
    CloseHandle(h);
    if (!ok) { free(b); return false; }
    b[got] = 0;
    *out = b; *len = got;
    return true;
}

// A string or boolean field of the mod manager's JSON, by key. Enough for a file that one tool writes;
// no JSON library. String values are unescaped (\\ and \"). False = the key is not there.
bool jsonString(const char* text, const char* key, char* out, int cap) {
    out[0] = 0;
    char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
    const char* p = strstr(text, k);
    if (!p) return false;
    p = strchr(p + strlen(k), ':');
    if (!p) return false;
    p++; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return true;                         // null or not a string: present, empty
    p++;
    int n = 0;
    for (; *p && *p != '"' && n < cap - 1; p++) {
        char c = *p;
        if (c == '\\' && p[1]) { p++; c = *p; }
        out[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    out[n] = 0;
    return true;
}
bool jsonTrue(const char* text, const char* key) {
    char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
    const char* p = strstr(text, k);
    if (!p || !(p = strchr(p + strlen(k), ':'))) return false;
    p++; while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
}
// The part of a path after "CustomMaps\", or "" when it has none.
void underCustomMaps(const char* full, char* out, int cap) {
    out[0] = 0;
    for (const char* p = full; *p; p++)
        if (!_strnicmp(p, "CustomMaps\\", 11)) { strncpy_s(out, (size_t)cap, p + 11, _TRUNCATE); return; }
}

// ---- THE MOD MANAGER'S RECORDS: Content\MapSwitcherMetaData\*_meta.json, one per level it knows.
// Read for three things: the player HID the level, gave it a CUSTOM NAME, or the manager INSTALLED it
// as a map from an archive (AssetName; a level it merely came across has none). Those fields sit at
// the top of the file, ahead of the file list, so only the head is read.
struct MapRecord {
    char   mapName[128];
    char   asset[128];      // empty = not an install, just a level the manager found
    char   dir[300];        // MapFileDirectory under CustomMaps
    char   custom[64];
    bool   hidden;
};
constexpr int kMaxRecords = 128;
MapRecord* g_rec = nullptr;
int        g_nRec = 0;

void loadRecords(const wchar_t* root) {
    g_nRec = 0;
    g_rec = (MapRecord*)calloc(kMaxRecords, sizeof(MapRecord));
    if (!g_rec) return;
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%s\\MapSwitcherMetaData\\*_meta.json", root);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_nRec >= kMaxRecords) break;
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\MapSwitcherMetaData\\%s", root, fd.cFileName);
        MapRecord& r = g_rec[g_nRec];
        char* text = nullptr; size_t len = 0;
        if (!readHead(p, &text, &len, 16u << 10)) continue;
        char full[300];
        jsonString(text, "MapName", r.mapName, sizeof(r.mapName));
        jsonString(text, "AssetName", r.asset, sizeof(r.asset));
        jsonString(text, "CustomName", r.custom, sizeof(r.custom));
        jsonString(text, "MapFileDirectory", full, sizeof(full));
        underCustomMaps(full, r.dir, sizeof(r.dir));
        r.hidden = jsonTrue(text, "IsHiddenByUser");
        free(text);
        if (r.mapName[0]) g_nRec++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
void freeRecords() {
    free(g_rec); g_rec = nullptr; g_nRec = 0;
}

bool asciiOnly(const wchar_t* s) { for (; *s; s++) if (*s < 32 || *s > 126) return false; return true; }

// rel is relative to Content, e.g. "CustomMaps\Author\Map". Collects every level; Decide filters.
void scanDir(const wchar_t* root, const wchar_t* rel, int depth) {
    if (depth > 8 || g_count >= kMaxEntries) return;
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%s\\%s\\*", root, rel);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t sub[MAX_PATH];
        swprintf(sub, MAX_PATH, L"%s\\%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scanDir(root, sub, depth + 1); continue; }
        const size_t n = wcslen(fd.cFileName);
        if (n < 6 || _wcsicmp(fd.cFileName + n - 5, L".umap") != 0) continue;
        if (!asciiOnly(sub)) { logv("[maps] skipped (non-ASCII path): %ls", sub); continue; }
        if (g_count >= kMaxEntries) { logv("[maps] more than %d levels -- the rest are not listed", kMaxEntries); break; }
        Entry& e = g_entries[g_count];
        snprintf(e.name, sizeof(e.name), "%.*ls", (int)(n - 5), fd.cFileName);
        snprintf(e.label, sizeof(e.label), "%s", e.name);
        snprintf(e.rel, sizeof(e.rel), "%ls", sub + 11);            // drop "CustomMaps\"
        snprintf(e.dir, sizeof(e.dir), "%ls", rel[10] ? rel + 11 : L"");
        // "CustomMaps\Author\Map\Map.umap" -> "/Game/CustomMaps/Author/Map/Map"
        char relA[300]; snprintf(relA, sizeof(relA), "%ls", sub);
        for (char* c = relA; *c; c++) if (*c == '\\') *c = '/';
        relA[strlen(relA) - 5] = 0;
        snprintf(e.path, sizeof(e.path), "/Game/%s", relA);
        g_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- which levels are maps -------------------------------------------------------------------------
const MapRecord* ownRecord(const Entry& e) {
    for (int i = 0; i < g_nRec; i++)
        if (!_stricmp(g_rec[i].mapName, e.name) && !_stricmp(g_rec[i].dir, e.dir)) return &g_rec[i];
    return nullptr;
}
// Case-insensitive search of an ASCII string inside a binary buffer.
bool containsNoCase(const char* buf, size_t len, const char* s) {
    const size_t n = strlen(s);
    if (!n || len < n) return false;
    const char f = (char)tolower((unsigned char)s[0]);
    for (size_t i = 0; i + n <= len; i++) {
        if ((char)tolower((unsigned char)buf[i]) != f) continue;
        if (!_strnicmp(buf + i, s, n)) return true;
    }
    return false;
}
// IS THIS LEVEL BUILT TO BE PLAYED IN SESSION? A playable map's world settings override the game mode
// with Session's in-game one -- without it nothing spawns the skater -- so its package names
// PBP_InGameSessionGameMode. Measured on every installed map (DCP, CCSesion, Downtown3, Calida,
// 16thAveDIY): all five do, and neither Kite-demo showroom shipped inside the Chevy Chase install
// does; a streaming sub-level's own world settings carry no override either. The name sits in the
// package's name table near the start of the file (byte 234k of the 3.9 MB Downtown3 at most), so
// the head of the file is enough.
bool isSessionLevel(const wchar_t* root, const Entry& e) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\CustomMaps\\%hs", root, e.rel);
    char* buf = nullptr; size_t len = 0;
    if (!readHead(p, &buf, &len, 8u << 20)) return false;
    const bool yes = containsNoCase(buf, len, "PBP_InGameSessionGameMode");
    free(buf);
    return yes;
}

void Scan() {
    g_count = 0;
    wchar_t root[MAX_PATH];
    if (!contentRoot(root, MAX_PATH)) { logv("[maps] could not locate the Content folder"); return; }
    scanDir(root, L"CustomMaps", 0);
    loadRecords(root);
    int installs = 0;
    for (int i = 0; i < g_nRec; i++) if (g_rec[i].asset[0]) installs++;
    if (g_nRec) logv("[maps] mod manager records: %d (%d from an installed archive)", g_nRec, installs);
    // DECIDE -- by what the LEVEL is, not where it sits. The old rule listed only the shallowest level
    // of each install folder, and hid real maps whenever an install nested its map below another level
    // (a map pack at two depths, a stray test level at the top).
    for (int i = 0; i < g_count; ) {
        Entry& e = g_entries[i];
        const MapRecord* own = ownRecord(e);
        const char* why = nullptr;
        if (own && own->hidden) {
            why = "hidden in the mod manager";
        } else if (isSessionLevel(root, e)) {
            // a Session map
        } else if (own && own->asset[0]) {
            // The manager installed it as a map from an archive: trust that over the game-mode test.
            logv("[maps]   '%s' does not name Session's game mode, but the mod manager installed it as a map (%s) -- listed",
                 e.name, own->asset);
        } else {
            why = "not a Session map -- the level does not use Session's game mode (a demo scene or a sub-level)";
        }
        if (why) {
            logv("[maps]   not listed: '%s' (%s)", e.name, why);
            for (int j = i + 1; j < g_count; j++) g_entries[j - 1] = g_entries[j];
            g_count--;
            continue;
        }
        if (own && own->custom[0]) snprintf(e.label, sizeof(e.label), "%s", own->custom);
        i++;
    }
    freeRecords();
    for (int i = 1; i < g_count; i++) {                 // by label, case-insensitive
        Entry t = g_entries[i]; int j = i - 1;
        while (j >= 0 && _stricmp(g_entries[j].label, t.label) > 0) { g_entries[j + 1] = g_entries[j]; j--; }
        g_entries[j + 1] = t;
    }
    if (!g_count) { logv("[maps] no custom maps under Content\\CustomMaps -- the Select Map screen is unchanged"); return; }
    logv("[maps] %d custom map(s) under Content\\CustomMaps (read at start-up; a map installed while the game runs appears after a restart):", g_count);
    for (int i = 0; i < g_count; i++) logv("[maps]   '%s' -> %s", g_entries[i].label, g_entries[i].path);
}
#else
void Scan() {}
#endif

// ---- engine value helpers ------------------------------------------------------------------------
struct TArrayHdr { void* data; int32_t num; int32_t max; };

bool makeFName(const char* s, uint64_t* out) {
    const Syms& S = Get();
    if (!S.FNameCtor || !s || !*s) return false;
    *out = 0;
    S.FNameCtor(out, s, 1 /* FNAME_Add */);
    return *out != 0;
}
// An FText from a plain string, via FName (same reasoning as pause_menu.cpp: no ownership question).
bool makeText(const char* s, uint8_t* out24) {
    const Syms& S = Get();
    uint64_t fn = 0;
    if (!S.TextFromName || !makeFName(s, &fn)) return false;
    memset(out24, 0, 24);
    S.TextFromName(out24, &fn);
    return true;
}
// A second owner of an existing FText: bitwise copy plus one on the shared reference count
// ({ITextData*; FReferenceController*; flags}, count at controller+8 -- the engine's own copy does
// exactly this `lock inc`).
void copyTextRef(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, 24);
    void* ctl = *(void**)(src + 8);
    if (ctl) InterlockedIncrement((volatile LONG*)((uint8_t*)ctl + 8));
}
// A TSoftObjectPtr copy: {weak ptr; tag; FName AssetPathName; FString SubPathString}. The FName and
// the weak pointer are plain values; the FString must be empty (no buffer to share), and is made so.
void copySoftPtr(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, 40);
    memset(dst + 0x18, 0, 16);
}
// Capacity for `add` more elements. The engine's allocator, so the engine can free or regrow it.
bool reserve(TArrayHdr* a, int stride, int add) {
    if (a->max - a->num >= add) return true;
    const Syms& S = Get();
    if (!S.MemMalloc || !S.MemFree) return false;
    const int newMax = a->num + add;
    uint8_t* p = (uint8_t*)S.MemMalloc((size_t)newMax * stride, 0);
    if (!p) return false;
    if (a->data && a->num > 0) memcpy(p, a->data, (size_t)a->num * stride);
    memset(p + (size_t)a->num * stride, 0, (size_t)(newMax - a->num) * stride);
    if (a->data) S.MemFree(a->data);
    a->data = p;
    a->max  = newMax;
    return true;
}

// ---- the injection -------------------------------------------------------------------------------
const char* kPrefix      = "CST";
const char* kCityName    = "Custom Maps";
const char* kCityDisplay = "Custom Maps Network";

int findCity(const TArrayHdr* cities, uint64_t prefix) {
    for (int i = 0; i < cities->num; i++)
        if (*(const uint64_t*)((const uint8_t*)cities->data + (size_t)i * off::kTransitCityStride + off::kTransitCityPrefix) == prefix) return i;
    return -1;
}

void Inject(uint8_t* asset) {
    if (!asset || g_count == 0) return;
    TArrayHdr* cities = (TArrayHdr*)(asset + off::kTransitAssetCities);
    TArrayHdr* nodes  = (TArrayHdr*)(asset + off::kTransitAssetNodes);
    uint64_t prefix = 0;
    if (!makeFName(kPrefix, &prefix)) return;
    if (findCity(cities, prefix) >= 0) return;         // already in this asset
    if (cities->num <= 0 || nodes->num <= 0) return;   // nothing to borrow the look from

    // The look is borrowed from "Extra Network": its map blueprint and one of its nodes' placeholder
    // image and (empty) locked-state texts. Any non-editor city and any node would do.
    uint64_t ext = 0;
    int tmplCity = makeFName("EXT", &ext) ? findCity(cities, ext) : -1;
    if (tmplCity < 0)
        for (int i = cities->num - 1; i >= 0; i--)
            if (!((const uint8_t*)cities->data)[(size_t)i * off::kTransitCityStride + off::kTransitCityEditorOnly]) { tmplCity = i; break; }
    if (tmplCity < 0) return;
    const uint8_t* tc = (const uint8_t*)cities->data + (size_t)tmplCity * off::kTransitCityStride;
    const uint64_t tcPrefix = *(const uint64_t*)(tc + off::kTransitCityPrefix);
    int tmplNode = 0;
    for (int i = 0; i < nodes->num; i++)
        if (*(const uint64_t*)((const uint8_t*)nodes->data + (size_t)i * off::kTransitNodeStride + off::kTransitNodeCity) == tcPrefix) { tmplNode = i; break; }

    if (!reserve(nodes, off::kTransitNodeStride, g_count) || !reserve(cities, off::kTransitCityStride, 1)) {
        logv("[maps] transit map: could not grow the asset's lists -- custom maps not added");
        return;
    }
    tc = (const uint8_t*)cities->data + (size_t)tmplCity * off::kTransitCityStride;   // reserve may have moved it
    const uint8_t* tn = (const uint8_t*)nodes->data + (size_t)tmplNode * off::kTransitNodeStride;

    int added = 0;
    for (int i = 0; i < g_count; i++) {
        uint64_t level = 0, portal = 0;
        uint8_t* n = (uint8_t*)nodes->data + (size_t)nodes->num * off::kTransitNodeStride;
        memset(n, 0, off::kTransitNodeStride);
        if (!makeFName(g_entries[i].path, &level) || !makeFName(g_entries[i].name, &portal) ||
            !makeText(g_entries[i].label, n + off::kTransitNodeDisplay)) continue;
        *(uint64_t*)(n + off::kTransitNodeCity)   = prefix;
        *(uint64_t*)(n + off::kTransitNodeLevel)  = level;
        // The portal id MUST be set and unique: the list builder hides the node whose portal equals
        // the widget's current portal, and with nobody standing at a station that is None -- a None
        // portal here hid every custom map (field, first round). The level's own name, as the game's
        // dev entries do; no such portal exists in the level, so arrival falls back to its player
        // start. Apartment level and world location stay none: the default apartment (TeleportPlayer
        // reads gi+0x380 when the field is 0) and no marker on the borrowed map.
        *(uint64_t*)(n + off::kTransitNodePortal) = portal;
        copyTextRef(n + off::kTransitNodeLockedData, tn + off::kTransitNodeLockedData);
        copySoftPtr(n + off::kTransitNodeLockedData + 0x18, tn + off::kTransitNodeLockedData + 0x18);
        copySoftPtr(n + off::kTransitNodeImage, tn + off::kTransitNodeImage);
        nodes->num++;
        added++;
    }
    if (!added) return;

    uint8_t* c = (uint8_t*)cities->data + (size_t)cities->num * off::kTransitCityStride;
    memset(c, 0, off::kTransitCityStride);
    *(uint64_t*)(c + off::kTransitCityPrefix) = prefix;
    if (!makeText(kCityName, c + off::kTransitCityName) || !makeText(kCityDisplay, c + off::kTransitCityDisplay)) {
        nodes->num -= added;                                  // leave nothing half-made
        return;
    }
    memcpy(c + off::kTransitCityColor, tc + off::kTransitCityColor, 16);
    *(void**)(c + off::kTransitCityMapBp) = *(void* const*)(tc + off::kTransitCityMapBp);
    cities->num++;
    logv("[maps] transit map: '%s' city added with %d spot(s), map look borrowed from city '%d'", kCityName, added, tmplCity);
}
} // namespace

int Count() { return g_count; }

#ifdef OMP_USE_MINHOOK
namespace {
using OpenFn = void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
OpenFn o_Open = nullptr;

// UTransitMapWidget::SetOpenTransitMap -- the asset is completed BEFORE the game reads it to build
// the city widgets and the enabled-city list.
void hkOpen(void* widget, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    __try {
        Inject(*(uint8_t**)((uint8_t*)widget + off::kTransitWidgetAsset));
    } __except (EXCEPTION_EXECUTE_HANDLER) { logv("[maps] transit map: adding custom maps faulted -- the screen is unchanged"); }
    o_Open(widget, a2, a3, a4, a5, a6, a7, a8);
}
} // namespace

void Install(void (*logf)(const char*)) {
    static bool done = false;
    if (done) return;
    done = true;
    g_logf = logf;
    Scan();
    if (!g_count) return;
    const Syms& S = Get();
    if (!S.TransitOpenMap || !S.FNameCtor || !S.TextFromName || !S.MemMalloc || !S.MemFree) {
        logv("[maps] a symbol is missing -- custom maps stay out of the Select Map screen");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { logv("[maps] MH_Initialize failed"); return; }
    if (MH_CreateHook(S.TransitOpenMap, (void*)&hkOpen, (void**)&o_Open) == MH_OK && MH_EnableHook(S.TransitOpenMap) == MH_OK)
        logv("[maps] Select Map hooked -- a 'Custom Maps' page joins the transit map");
    else logv("[maps] hook FAILED on the transit map -- custom maps stay out of the Select Map screen");
}
#else
void Install(void (*logf)(const char*)) { g_logf = logf; Scan(); }
#endif

} // namespace omp::game::maps
