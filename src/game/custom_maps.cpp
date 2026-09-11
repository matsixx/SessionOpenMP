// Custom maps in the Select Map screen -- see custom_maps.h.
#include "custom_maps.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
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
    char group[160];    // the install folder: CustomMaps\<author>\<map>
    int  depth;         // folders between the install folder and the level file
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

// The mod manager's per-map record, when present: Content\MapSwitcherMetaData\<dir>_<map>_meta.json.
// Only the two fields that matter are read, off the first few KB (the file list behind them can be
// megabytes). No JSON library: two keyed string scans are enough for a file that tool writes.
void readMeta(const wchar_t* root, const wchar_t* parentDir, const wchar_t* mapName, bool* hidden, char* customName, int cap) {
    *hidden = false; customName[0] = 0;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\MapSwitcherMetaData\\%s_%s_meta.json", root, parentDir, mapName);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    static char buf[8192];
    DWORD got = 0;
    const bool ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return;
    buf[got] = 0;
    if (const char* k = strstr(buf, "\"IsHiddenByUser\"")) {
        k = strchr(k + 16, ':');
        if (k) { k++; while (*k == ' ' || *k == '\t') k++; *hidden = strncmp(k, "true", 4) == 0; }
    }
    if (const char* k = strstr(buf, "\"CustomName\"")) {
        k = strchr(k + 12, ':');
        if (k) {
            k++; while (*k == ' ' || *k == '\t') k++;
            if (*k == '"') {
                k++;
                int n = 0;
                for (; *k && *k != '"' && n < cap - 1; k++) customName[n++] = (*k >= 32 && *k < 127) ? *k : '?';
                customName[n] = 0;
            }
        }
    }
}

bool asciiOnly(const wchar_t* s) { for (; *s; s++) if (*s < 32 || *s > 126) return false; return true; }

// rel is relative to Content, e.g. "CustomMaps\Author\Map".
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
        wchar_t mapName[128]; wcsncpy_s(mapName, fd.cFileName, n - 5);
        const wchar_t* parent = wcsrchr(rel, L'\\'); parent = parent ? parent + 1 : rel;
        bool hidden = false; char custom[64];
        readMeta(root, parent, mapName, &hidden, custom, sizeof(custom));
        if (hidden) { logv("[maps] hidden in the mod manager, not listed: %ls", mapName); continue; }
        Entry& e = g_entries[g_count];
        snprintf(e.name, sizeof(e.name), "%ls", mapName);
        // Install folder = the first two folders under CustomMaps; depth = how many folders deeper
        // than the first one below it the file sits. "CustomMaps\Lucas\CCSESION\Map\CCSesion.umap"
        // -> group CustomMaps\Lucas\CCSESION, depth 0; the same install's KiteDemo leftovers, depth 4.
        { int seg = 0; const wchar_t* cut = nullptr;
          for (const wchar_t* c = rel; *c; c++) if (*c == L'\\' && ++seg == 3) { cut = c; break; }
          if (cut) { snprintf(e.group, sizeof(e.group), "%.*ls", (int)(cut - rel), rel); e.depth = 0;
                     for (const wchar_t* c = cut + 1; *c; c++) if (*c == L'\\') e.depth++; }
          else { snprintf(e.group, sizeof(e.group), "%ls", rel); e.depth = 0; } }
        if (custom[0]) snprintf(e.label, sizeof(e.label), "%s", custom);
        else           snprintf(e.label, sizeof(e.label), "%ls", mapName);
        // "CustomMaps\Author\Map\Map.umap" -> "/Game/CustomMaps/Author/Map/Map"
        char relA[300]; snprintf(relA, sizeof(relA), "%ls", sub);
        for (char* c = relA; *c; c++) if (*c == '\\') *c = '/';
        relA[strlen(relA) - 5] = 0;
        snprintf(e.path, sizeof(e.path), "/Game/%s", relA);
        g_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Scan() {
    g_count = 0;
    wchar_t root[MAX_PATH];
    if (!contentRoot(root, MAX_PATH)) { logv("[maps] could not locate the Content folder"); return; }
    scanDir(root, L"CustomMaps", 0);
    // One install can carry more level files than its map -- demo scenes an author left in, sub-
    // levels -- always deeper in the tree than the map itself. Within an install folder, only the
    // shallowest level files are listed.
    for (int i = 0; i < g_count; ) {
        int minDepth = g_entries[i].depth;
        for (int j = 0; j < g_count; j++)
            if (!_stricmp(g_entries[j].group, g_entries[i].group) && g_entries[j].depth < minDepth) minDepth = g_entries[j].depth;
        if (g_entries[i].depth > minDepth) {
            logv("[maps]   not listed: '%s' (a level %d folder(s) inside the %s install, deeper than the map itself)",
                 g_entries[i].name, g_entries[i].depth, g_entries[i].group);
            for (int j = i + 1; j < g_count; j++) g_entries[j - 1] = g_entries[j];
            g_count--;
        } else i++;
    }
    for (int i = 1; i < g_count; i++) {                 // by label, case-insensitive
        Entry t = g_entries[i]; int j = i - 1;
        while (j >= 0 && _stricmp(g_entries[j].label, t.label) > 0) { g_entries[j + 1] = g_entries[j]; j--; }
        g_entries[j + 1] = t;
    }
    if (!g_count) { logv("[maps] no custom maps under Content\\CustomMaps -- the Select Map screen is unchanged"); return; }
    logv("[maps] %d custom map(s) under Content\\CustomMaps:", g_count);
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
