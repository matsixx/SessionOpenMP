// Custom maps in the Select Map screen -- see custom_maps.h.
#include "custom_maps.h"
#include "game_syms.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif

namespace omp::game::maps {

namespace {
// One custom map, as the scan found and accepted it. No fixed sizes anywhere: no cap on how many maps,
// how long a name or path is, or how deep an install nests its level.
struct Entry {
    std::string label;     // what the spot list shows (the file name, the manager's custom name, or
                           // either of those with its author folder added when two would read the same)
    std::string name;      // the level's own name (the file name without .umap)
    std::string path;      // the package path OpenLevel loads: /Game/CustomMaps/...
    std::string rel;       // under CustomMaps, backslashes, extension kept: Author\Map\Map\Map.umap
    std::string dir;       // the folder it sits in, same form: Author\Map\Map
    std::string portal;    // the spot's id -- unique per level, see portalFor
};

void (*g_logf)(const char*) = nullptr;
void logv(const char* fmt, ...) {
    if (!g_logf) return;
    char m[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    g_logf(m);
}

// ---- the scan's result, handed over once ----------------------------------------------------------
// The scan runs on its own thread (it reads every level's header, which is real disk time with many
// maps installed) and PUBLISHES a finished list exactly once. The game thread only ever reads a
// published list, so there is nothing to lock: the pointer is written once and never freed.
std::vector<Entry>* volatile g_ready = nullptr;
HANDLE g_scanDone = nullptr;              // signalled when g_ready is published

const std::vector<Entry>* readyList() { return g_ready; }

#ifdef _WIN32
// ---- file access without MAX_PATH -------------------------------------------------------------------
// Every path goes through the \\?\ prefix, which lifts Windows' 260-character limit, so an install
// nested arbitrarily deep is still found.
std::wstring longPath(const std::wstring& abs) { return L"\\\\?\\" + abs; }

bool contentRoot(std::wstring* out) {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &exe[0], (DWORD)exe.size());
    if (!n || n >= exe.size()) return false;
    exe.resize(n);
    for (int up = 0; up < 3; up++) {                    // file name, Win64, Binaries
        const size_t s = exe.find_last_of(L'\\');
        if (s == std::wstring::npos) return false;
        exe.resize(s);
    }
    *out = exe + L"\\Content";
    return true;
}

std::string narrow(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 32 && c < 127) ? (char)c : '?');
    return s;
}
bool asciiOnly(const std::wstring& s) { for (wchar_t c : s) if (c < 32 || c > 126) return false; return true; }
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// The first `cap` bytes of a file (all of it when smaller; cap 0 = no cap).
bool readFile(const std::wstring& absPath, std::string* out, uint64_t cap) {
    out->clear();
    HANDLE h = CreateFileW(longPath(absPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return false; }
    uint64_t want = (uint64_t)sz.QuadPart;
    if (cap && want > cap) want = cap;
    bool ok = true;
    try { out->resize((size_t)want); } catch (...) { ok = false; }
    uint64_t done = 0;
    while (ok && done < want) {
        const DWORD chunk = (DWORD)((want - done) > (1u << 26) ? (1u << 26) : (want - done));
        DWORD got = 0;
        if (!ReadFile(h, &(*out)[(size_t)done], chunk, &got, nullptr) || got == 0) { ok = false; break; }
        done += got;
    }
    CloseHandle(h);
    if (!ok) out->clear();
    return ok;
}

// ---- the mod manager's JSON, by key -----------------------------------------------------------------
// Enough for a file one tool writes; no JSON library. String values are unescaped (\\ and \").
std::string jsonString(const std::string& text, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos) return std::string();
    p = text.find(':', p + k.size());
    if (p == std::string::npos) return std::string();
    p++;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) p++;
    if (p >= text.size() || text[p] != '"') return std::string();     // null or not a string
    std::string out;
    for (p++; p < text.size() && text[p] != '"'; p++) {
        char c = text[p];
        if (c == '\\' && p + 1 < text.size()) c = text[++p];
        out.push_back((c >= 32 && c < 127) ? c : '?');
    }
    return out;
}
bool jsonTrue(const std::string& text, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos || (p = text.find(':', p + k.size())) == std::string::npos) return false;
    p++;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) p++;
    return text.compare(p, 4, "true") == 0;
}
std::string underCustomMaps(const std::string& full) {
    const size_t p = lower(full).find("custommaps\\");
    return p == std::string::npos ? std::string() : full.substr(p + 11);
}

// ---- THE MOD MANAGER'S RECORDS: Content\MapSwitcherMetaData\*_meta.json, one per level it knows.
// Read for three things: the player HID the level, gave it a CUSTOM NAME, or the manager INSTALLED it
// as a map from an archive (AssetName; a level it merely came across has none). Those fields sit at
// the top of the file, ahead of its file list, so only the head is read.
struct MapRecord {
    std::string mapName, asset, dir, custom;
    bool hidden = false;
};

std::vector<MapRecord> loadRecords(const std::wstring& root) {
    std::vector<MapRecord> recs;
    const std::wstring folder = root + L"\\MapSwitcherMetaData";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(longPath(folder + L"\\*_meta.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return recs;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string text;
        if (!readFile(folder + L"\\" + fd.cFileName, &text, 64u << 10)) continue;
        MapRecord r;
        r.mapName = jsonString(text, "MapName");
        if (r.mapName.empty()) continue;
        r.asset  = jsonString(text, "AssetName");
        r.custom = jsonString(text, "CustomName");
        r.dir    = underCustomMaps(jsonString(text, "MapFileDirectory"));
        r.hidden = jsonTrue(text, "IsHiddenByUser");
        recs.push_back(std::move(r));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return recs;
}

// ---- IS THIS LEVEL BUILT TO BE PLAYED IN SESSION? ------------------------------------------------
// A playable map's world settings override the game mode with Session's in-game one -- without it
// nothing spawns the skater -- so its package names PBP_InGameSessionGameMode. Measured on every
// installed map (DCP, CCSesion, Downtown3, Calida, 16thAveDIY, Sunset_City, Warehouse_Skatepark): all
// do; neither Kite-demo showroom shipped inside the Chevy Chase install does, and a streaming
// sub-level's own world settings carry no override either.
// The name lives in the package's name table, inside the package HEADER. In a cooked game the .umap
// file IS the header (FPackageFileSummary::TotalHeaderSize == the file size on every level measured;
// the level's contents are in the .uexp beside it), so the whole .umap is read, with no size cap. A
// package that is not split that way still only needs its header: TotalHeaderSize bounds the read.
// TotalHeaderSize from FPackageFileSummary, or 0 when the file does not parse as a UE4 package.
uint64_t headerSize(const std::string& head) {
    auto i32 = [&](size_t o, int32_t* v) { if (o + 4 > head.size()) return false; memcpy(v, head.data() + o, 4); return true; };
    int32_t tag = 0, legacy = 0, n = 0, total = 0;
    if (!i32(0, &tag) || (uint32_t)tag != 0x9E2A83C1u || !i32(4, &legacy)) return 0;
    size_t o = 8;
    if (legacy != -4) o += 4;                          // LegacyUE3Version
    o += 8;                                            // FileVersionUE4, FileVersionLicenseeUE4
    if (legacy <= -2) {                                // custom versions
        if (!i32(o, &n) || n < 0) return 0;
        o += 4;
        if (legacy < -5)       o += (size_t)n * 20;    // optimized: FGuid + int32 (UE 4.27 is legacy -7)
        else if (legacy == -2) o += (size_t)n * 8;     // enum-based
        else return 0;                                 // guid + friendly name: not expected here
    }
    if (!i32(o, &total) || total <= 0) return 0;
    return (uint64_t)total;
}
bool isSessionLevel(const std::wstring& absPath) {
    std::string buf;
    if (!readFile(absPath, &buf, 64u << 10)) return false;
    uint64_t total = headerSize(buf);
    if (total > buf.size() && !readFile(absPath, &buf, total)) return false;
    if (!total && !readFile(absPath, &buf, 0)) return false;       // not a recognisable package: read it all
    // Exact case: the name is Session's own asset's, spelled as the game ships it -- and an exact
    // search is a fast library scan, where a case-folding one walked every byte (2.6 s over 616 files).
    return buf.find("PBP_InGameSessionGameMode") != std::string::npos;
}

// THE SPOT'S ID. It must be set and unique: the list builder hides the node whose portal equals the
// widget's current portal (a None portal hid every custom map, first round), and two installs that both
// ship a Map.umap must not share one. The level's cleaned name plus a hash of its full path; only
// [A-Za-z0-9_], because the game hands the id to the level load as a string. No such portal exists in
// the level, so arrival falls back to its player start.
std::string portalFor(const Entry& e) {
    std::string clean = "CST_";
    for (char c : e.name) if (isalnum((unsigned char)c) || c == '_') clean.push_back(c);
    if (clean.size() > 64) clean.resize(64);
    uint32_t h = 2166136261u;
    for (char c : lower(e.rel)) { h ^= (uint8_t)c; h *= 16777619u; }
    char hex[16]; snprintf(hex, sizeof(hex), "_%08X", h);
    return clean + hex;
}

// ---- the scan ------------------------------------------------------------------------------------
struct DirId { DWORD vol, hi, lo; };
bool dirIdentity(const std::wstring& absDir, DirId* id) {
    HANDLE h = CreateFileW(longPath(absDir).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION fi{};
    const bool ok = GetFileInformationByHandle(h, &fi) != 0;
    CloseHandle(h);
    if (ok) { id->vol = fi.dwVolumeSerialNumber; id->hi = fi.nFileIndexHigh; id->lo = fi.nFileIndexLow; }
    return ok;
}

struct ScanState {
    std::wstring root;
    std::vector<MapRecord> recs;
    std::vector<Entry> maps;
    std::vector<DirId> stack;          // the folders on the current path, to refuse a link loop
    int levels = 0, skipped = 0;
};
// One line per level left out, up to a point: an install folder full of demo scenes or sub-levels
// must not bury the log. The count of the rest is said once at the end.
void notListed(ScanState& st, const char* fmt, ...) {
    st.skipped++;
    if (st.skipped > 50) return;
    char m[900];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    logv("%s", m);
}

const MapRecord* ownRecord(const ScanState& st, const Entry& e) {
    for (const auto& r : st.recs)
        if (!_stricmp(r.mapName.c_str(), e.name.c_str()) && !_stricmp(r.dir.c_str(), e.dir.c_str())) return &r;
    return nullptr;
}

// DECIDE, as each level is found -- by what the level IS, never by where it sits. (The first rule
// listed only the shallowest level of each install folder and hid real maps nested below another
// level of their install; it also counted demo scenes and sub-levels against a 64-level cap.)
void consider(ScanState& st, const std::wstring& relDirW, const std::wstring& fileW) {
    st.levels++;
    const std::wstring relW = relDirW.empty() ? fileW : relDirW + L"\\" + fileW;
    if (!asciiOnly(relW)) { notListed(st, "[maps]   not listed: '%s' (its path has non-ASCII characters)", narrow(relW).c_str()); return; }
    Entry e;
    e.rel  = narrow(relW);
    e.dir  = narrow(relDirW);
    e.name = narrow(fileW.substr(0, fileW.size() - 5));
    std::string slashed = e.rel.substr(0, e.rel.size() - 5);
    for (auto& c : slashed) if (c == '\\') c = '/';
    e.path = "/Game/CustomMaps/" + slashed;

    const MapRecord* own = ownRecord(st, e);
    if (own && own->hidden) { notListed(st, "[maps]   not listed: '%s' (hidden in the mod manager)", e.name.c_str()); return; }
    const bool session = isSessionLevel(st.root + L"\\CustomMaps\\" + relW);
    if (!session && !(own && !own->asset.empty())) {
        notListed(st, "[maps]   not listed: '%s' (not a Session map -- the level does not use Session's game mode; a demo scene or a sub-level)", e.name.c_str());
        return;
    }
    if (!session)
        logv("[maps]   '%s' does not name Session's game mode, but the mod manager installed it as a map (%s) -- listed",
             e.name.c_str(), own->asset.c_str());
    e.label  = (own && !own->custom.empty()) ? own->custom : e.name;
    e.portal = portalFor(e);
    st.maps.push_back(std::move(e));
}

void walk(ScanState& st, const std::wstring& relDirW) {
    const std::wstring abs = st.root + L"\\CustomMaps" + (relDirW.empty() ? L"" : L"\\" + relDirW);
    DirId id{};
    bool pushed = false;
    if (dirIdentity(abs, &id)) {
        for (const auto& a : st.stack)
            if (a.vol == id.vol && a.hi == id.hi && a.lo == id.lo) {
                logv("[maps]   skipped a folder link that loops back on itself: CustomMaps\\%s", narrow(relDirW).c_str());
                return;
            }
        st.stack.push_back(id);
        pushed = true;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(longPath(abs + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::vector<std::wstring> subdirs;
        do {
            const std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { subdirs.push_back(nm); continue; }
            if (nm.size() > 5 && !_wcsicmp(nm.c_str() + nm.size() - 5, L".umap")) consider(st, relDirW, nm);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (const auto& d : subdirs) walk(st, relDirW.empty() ? d : relDirW + L"\\" + d);
    }
    if (pushed) st.stack.pop_back();
}

// Two maps that would read the same in the list get their author folder added; if that still does not
// tell them apart, their folder under CustomMaps.
void disambiguate(std::vector<Entry>& maps) {
    auto dupes = [&](const Entry& e) {
        int n = 0;
        for (const auto& o : maps) if (!_stricmp(o.label.c_str(), e.label.c_str())) n++;
        return n > 1;
    };
    std::vector<bool> mark(maps.size());
    for (size_t i = 0; i < maps.size(); i++) mark[i] = dupes(maps[i]);
    for (size_t i = 0; i < maps.size(); i++)
        if (mark[i]) maps[i].label += " (" + maps[i].rel.substr(0, maps[i].rel.find('\\')) + ")";
    for (size_t i = 0; i < maps.size(); i++) mark[i] = dupes(maps[i]);
    for (size_t i = 0; i < maps.size(); i++)
        if (mark[i]) maps[i].label = (maps[i].label.substr(0, maps[i].label.rfind(" (")) + " (" + maps[i].dir + ")");
}

DWORD WINAPI scanThread(void*) {
    const ULONGLONG t0 = GetTickCount64();
    auto* out = new std::vector<Entry>();
    ScanState st;
    if (!contentRoot(&st.root)) {
        logv("[maps] could not locate the Content folder");
    } else {
        st.recs = loadRecords(st.root);
        int installs = 0;
        for (const auto& r : st.recs) if (!r.asset.empty()) installs++;
        if (!st.recs.empty()) logv("[maps] mod manager records: %d (%d from an installed archive)", (int)st.recs.size(), installs);
        walk(st, L"");
        if (st.skipped > 50) logv("[maps]   ...and %d more level file(s) not listed", st.skipped - 50);
        disambiguate(st.maps);
        std::sort(st.maps.begin(), st.maps.end(),
                  [](const Entry& a, const Entry& b) { return _stricmp(a.label.c_str(), b.label.c_str()) < 0; });
        if (st.maps.empty()) {
            logv("[maps] no custom maps under Content\\CustomMaps (%d level file(s) checked) -- the Select Map screen is unchanged", st.levels);
        } else {
            logv("[maps] %d custom map(s) under Content\\CustomMaps (%d level file(s) checked in %llu ms; read at start-up --"
                 " a map installed while the game runs appears after a restart):",
                 (int)st.maps.size(), st.levels, (unsigned long long)(GetTickCount64() - t0));
            for (const auto& e : st.maps) logv("[maps]   '%s' -> %s", e.label.c_str(), e.path.c_str());
        }
        *out = std::move(st.maps);
    }
    g_ready = out;                    // published once; never freed, never written again
    if (g_scanDone) SetEvent(g_scanDone);
    return 0;
}
#endif // _WIN32

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

// Called inside hkOpen's __try: no object with a destructor may live in this frame's caller, and none
// is created here -- the list is read through a pointer.
void Inject(uint8_t* asset, const std::vector<Entry>* maps) {
    if (!asset || !maps || maps->empty()) return;
    TArrayHdr* cities = (TArrayHdr*)(asset + off::kTransitAssetCities);
    TArrayHdr* nodes  = (TArrayHdr*)(asset + off::kTransitAssetNodes);
    uint64_t prefix = 0;
    if (!makeFName(kPrefix, &prefix)) return;
    if (findCity(cities, prefix) >= 0) return;         // already in this asset
    if (cities->num <= 0 || nodes->num <= 0) return;   // nothing to borrow the look from
    const int count = (int)maps->size();

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

    if (!reserve(nodes, off::kTransitNodeStride, count) || !reserve(cities, off::kTransitCityStride, 1)) {
        logv("[maps] transit map: could not grow the asset's lists -- custom maps not added");
        return;
    }
    tc = (const uint8_t*)cities->data + (size_t)tmplCity * off::kTransitCityStride;   // reserve may have moved it
    const uint8_t* tn = (const uint8_t*)nodes->data + (size_t)tmplNode * off::kTransitNodeStride;

    int added = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        const Entry& e = (*maps)[i];
        uint64_t level = 0, portal = 0;
        uint8_t* n = (uint8_t*)nodes->data + (size_t)nodes->num * off::kTransitNodeStride;
        memset(n, 0, off::kTransitNodeStride);
        // An FName holds up to 1023 characters; a package path or label past that cannot be named,
        // so that one map is left out and said so rather than truncated into a wrong level.
        if (e.path.size() > 1023 || e.label.size() > 1023 ||
            !makeFName(e.path.c_str(), &level) || !makeFName(e.portal.c_str(), &portal) ||
            !makeText(e.label.c_str(), n + off::kTransitNodeDisplay)) {
            if (++failed <= 16) logv("[maps] could not add '%s' to the list (its name or path cannot be named in the engine)", e.name.c_str());
            continue;
        }
        *(uint64_t*)(n + off::kTransitNodeCity)   = prefix;
        *(uint64_t*)(n + off::kTransitNodeLevel)  = level;
        // Apartment level and world location stay none: the default apartment (TeleportPlayer reads
        // gi+0x380 when the field is 0) and no marker on the borrowed map. See portalFor for the id.
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

int Count() { const auto* m = readyList(); return m ? (int)m->size() : 0; }

#ifdef OMP_USE_MINHOOK
namespace {
using OpenFn = void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
OpenFn o_Open = nullptr;

// The list for this opening. If the scan is still running the first time Select Map opens, wait for
// it -- briefly, once: the transit map is opened by a button press, and holding that frame a moment
// beats opening it without the page. A scan that takes longer than that shows the page from the next
// opening on.
const std::vector<Entry>* listForOpen() {
    const auto* m = readyList();
    if (m) return m;
    static bool waited = false;
    if (!waited && g_scanDone) {
        waited = true;
        if (WaitForSingleObject(g_scanDone, 3000) != WAIT_OBJECT_0)
            logv("[maps] still reading the custom maps folder -- the Custom Maps page appears the next time Select Map opens");
    }
    return readyList();
}

// UTransitMapWidget::SetOpenTransitMap -- the asset is completed BEFORE the game reads it to build
// the city widgets and the enabled-city list.
void hkOpen(void* widget, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    const std::vector<Entry>* maps = listForOpen();
    __try {
        Inject(*(uint8_t**)((uint8_t*)widget + off::kTransitWidgetAsset), maps);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logv("[maps] transit map: adding custom maps faulted -- the screen is unchanged"); }
    o_Open(widget, a2, a3, a4, a5, a6, a7, a8);
}
} // namespace

void Install(void (*logf)(const char*)) {
    static bool done = false;
    if (done) return;
    done = true;
    g_logf = logf;
    // The folder is read on its own thread, so launch never waits on it however many maps are installed.
    g_scanDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, scanThread, nullptr, 0, nullptr);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
    else    scanThread(nullptr);                       // no thread: scan here rather than not at all
    const Syms& S = Get();
    if (!S.TransitOpenMap || !S.FNameCtor || !S.TextFromName || !S.MemMalloc || !S.MemFree) {
        logv("[maps] a symbol is missing -- custom maps stay out of the Select Map screen");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { logv("[maps] MH_Initialize failed"); return; }
    if (MH_CreateHook(S.TransitOpenMap, (void*)&hkOpen, (void**)&o_Open) == MH_OK && MH_EnableHook(S.TransitOpenMap) == MH_OK)
        logv("[maps] Select Map hooked -- custom maps are being read in the background");
    else logv("[maps] hook FAILED on the transit map -- custom maps stay out of the Select Map screen");
}
#else
// Without the hook layer (offline tools and tests): the same scan, run here and published.
void Install(void (*logf)(const char*)) { g_logf = logf; scanThread(nullptr); }
#endif

// For offline tests: the published list, entry by entry. Null past the end or before the scan is done.
const char* DebugEntry(int i, int field) {
    const auto* m = readyList();
    if (!m || i < 0 || i >= (int)m->size()) return nullptr;
    const Entry& e = (*m)[i];
    return field == 0 ? e.label.c_str() : field == 1 ? e.path.c_str() : e.portal.c_str();
}

} // namespace omp::game::maps
