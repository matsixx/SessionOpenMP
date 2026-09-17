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
// The mod channel for UE4SS Lua mods -- see lua_api.h for the design.
#include "lua_api.h"
#include "session/modapi.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace omp { namespace luaapi {

using RC::LuaMadeSimple::Lua;

static void (*g_logf)(const char*) = nullptr;
static void say(const char* m) { if (g_logf) g_logf(m); }

// ---- per-channel event queues ------------------------------------------------------------------------
static const size_t kQueueMax = 512;   // messages; join/leave events are always kept

struct Event {
    int         kind = 0;              // 1 player event, 2 message
    int         player = -1;
    int         joined = 0;
    std::string hex;
};

struct Chan {
    std::mutex        m;
    std::deque<Event> q;
    uint32_t          dropped = 0;
    std::string       mod;
    int               handle = 0;
    bool              live = true;
};

// Chans are never freed: a callback from the channel layer can still be in flight for one that was just
// unregistered, and a mod registers a handful of channels in its whole life.
static std::mutex         g_chansLock;
static std::vector<Chan*> g_chans;

static Chan* chanByHandle(int handle) {
    std::lock_guard<std::mutex> lk(g_chansLock);
    for (Chan* c : g_chans) if (c->live && c->handle == handle) return c;
    return nullptr;
}

static const char kHex[] = "0123456789abcdef";

static void onMessage(int player, const uint8_t* data, int len, void* user) {
    Chan* c = (Chan*)user;
    Event e; e.kind = 2; e.player = player;
    e.hex.resize((size_t)len * 2);
    for (int i = 0; i < len; i++) { e.hex[(size_t)i * 2] = kHex[data[i] >> 4]; e.hex[(size_t)i * 2 + 1] = kHex[data[i] & 15]; }
    std::lock_guard<std::mutex> lk(c->m);
    if (!c->live) return;
    if (c->q.size() >= kQueueMax) {
        if (c->dropped++ == 0) {
            char m[200]; snprintf(m, sizeof(m), "[modapi/lua] %s: messages are not being read -- dropping new ones"
                                  " (is the mod's game thread busy, or did its pump stop?)", c->mod.c_str());
            say(m);
        }
        return;
    }
    c->q.push_back(std::move(e));
}

static void onPlayer(int player, int joined, void* user) {
    Chan* c = (Chan*)user;
    Event e; e.kind = 1; e.player = player; e.joined = joined;
    std::lock_guard<std::mutex> lk(c->m);
    if (c->live) c->q.push_back(std::move(e));
}

// ---- argument helpers (LuaMadeSimple's get_* read index 1 and remove it) ------------------------------
static bool argNumber(const Lua& lua, double* out) {
    if (lua.get_stack_size() < 1) return false;
    if (!lua.is_number()) { lua.discard_value(); return false; }
    *out = lua.get_number();
    return true;
}
static bool argInt(const Lua& lua, int* out) {
    double d = 0;
    if (!argNumber(lua, &d)) return false;
    if (!(d > -2147483648.0 && d < 2147483647.0)) return false;
    *out = (int)d;
    return true;
}
static bool argString(const Lua& lua, std::string* out) {
    if (lua.get_stack_size() < 1) return false;
    if (!lua.is_string()) { lua.discard_value(); return false; }
    const std::string_view v = lua.get_string();
    out->assign(v.data(), v.size());       // copy before anything else can touch the Lua heap
    return true;
}
static void clearArgs(const Lua& lua) { while (lua.get_stack_size() > 0) lua.discard_value(); }
static int retInt(const Lua& lua, int64_t v) { lua.set_integer(v); return 1; }
static int retNil(const Lua& lua) { lua.set_nil(); return 1; }
static int retString(const Lua& lua, const std::string& s) { lua.set_string(std::string_view(s.c_str(), s.size())); return 1; }

static int hexVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Paths are what Lua can turn back into objects (StaticFindObject). Game thread only.
static std::string pathOf(void* actor) {
    if (!actor) return std::string();
    const std::wstring w = ((RC::Unreal::UObject*)actor)->GetPathName(nullptr);
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- natives ------------------------------------------------------------------------------------------
static int nApiVersion(const Lua& lua) { clearArgs(lua); return retInt(lua, modapi::kApiVersion); }

static int nRegister(const Lua& lua) {
    std::string mod, name;
    const bool ok = argString(lua, &mod) && argString(lua, &name);
    clearArgs(lua);
    if (!ok) return retInt(lua, 0);
    Chan* c = new Chan();
    c->mod = mod;
    const int h = modapi::Register(name.c_str(), &onMessage, &onPlayer, c);
    if (!h) { delete c; return retInt(lua, 0); }     // refused: no callback can ever carry this pointer
    c->handle = h;
    { std::lock_guard<std::mutex> lk(g_chansLock); g_chans.push_back(c); }
    return retInt(lua, h);
}

static void retire(Chan* c) {
    modapi::Unregister(c->handle);
    std::lock_guard<std::mutex> lk(c->m);
    c->live = false;
    c->q.clear();
}

static int nUnregister(const Lua& lua) {
    int h = 0;
    const bool ok = argInt(lua, &h);
    clearArgs(lua);
    if (ok) if (Chan* c = chanByHandle(h)) retire(c);
    return 0;
}

static int nSend(const Lua& lua) {
    int h = 0, player = -1, reliable = 1;
    std::string hex;
    const bool ok = argInt(lua, &h) && argInt(lua, &player) && argString(lua, &hex) && argInt(lua, &reliable);
    clearArgs(lua);
    if (!ok || (hex.size() & 1) || hex.size() > (size_t)modapi::kMaxPayload * 2) return retInt(lua, 0);
    uint8_t buf[modapi::kMaxPayload];
    const int len = (int)(hex.size() / 2);
    for (int i = 0; i < len; i++) {
        const int hi = hexVal(hex[(size_t)i * 2]), lo = hexVal(hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) return retInt(lua, 0);
        buf[i] = (uint8_t)(hi << 4 | lo);
    }
    return retInt(lua, modapi::Send(h, player, buf, len, reliable));
}

static int nPending(const Lua& lua) {
    std::string mod;
    const bool ok = argString(lua, &mod);
    clearArgs(lua);
    if (!ok) return retInt(lua, 0);
    int64_t n = 0;
    std::lock_guard<std::mutex> lk(g_chansLock);
    for (Chan* c : g_chans) {
        if (!c->live || c->mod != mod) continue;
        std::lock_guard<std::mutex> lq(c->m);
        n += (int64_t)c->q.size();
    }
    return retInt(lua, n);
}

// -> kind, player, joined (1/0) | hex payload; or nil when the queue is empty
static int nPoll(const Lua& lua) {
    int h = 0;
    const bool ok = argInt(lua, &h);
    clearArgs(lua);
    Chan* c = ok ? chanByHandle(h) : nullptr;
    if (!c) return retNil(lua);
    Event e;
    {
        std::lock_guard<std::mutex> lk(c->m);
        if (c->q.empty()) return retNil(lua);
        e = std::move(c->q.front());
        c->q.pop_front();
    }
    lua.set_integer(e.kind);
    lua.set_integer(e.player);
    if (e.kind == 1) lua.set_integer(e.joined);
    else             lua.set_string(std::string_view(e.hex.c_str(), e.hex.size()));
    return 3;
}

static int nInSession(const Lua& lua) { clearArgs(lua); return retInt(lua, modapi::InSession()); }

// -> player, player, ... (multiple returns; the prelude packs them into a table)
static int nPlayers(const Lua& lua) {
    int h = 0;
    const bool ok = argInt(lua, &h);
    clearArgs(lua);
    if (!ok) return 0;
    int players[32];
    int n = modapi::Players(h, players, 32);
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) lua.set_integer(players[i]);
    return n;
}

static int nIsAuthority(const Lua& lua) {
    int h = 0;
    const bool ok = argInt(lua, &h);
    clearArgs(lua);
    return retInt(lua, ok ? modapi::IsAuthority(h) : 0);
}

static int nAuthority(const Lua& lua) {
    int h = 0;
    const bool ok = argInt(lua, &h);
    clearArgs(lua);
    return retInt(lua, ok ? modapi::Authority(h) : -1);
}

static int nPlayerName(const Lua& lua) {
    int p = 0;
    const bool ok = argInt(lua, &p);
    clearArgs(lua);
    char buf[64];
    if (!ok || !modapi::PlayerName(p, buf, sizeof(buf))) return retNil(lua);
    return retString(lua, buf);
}

static int nPlayerId(const Lua& lua) {
    int p = 0;
    const bool ok = argInt(lua, &p);
    clearArgs(lua);
    char buf[64];
    if (!ok || !modapi::PlayerId(p, buf, sizeof(buf))) return retNil(lua);
    return retString(lua, buf);
}

static int nLocalName(const Lua& lua) {
    clearArgs(lua);
    char buf[64];
    modapi::LocalName(buf, sizeof(buf));
    return retString(lua, buf);
}

static int nLocalId(const Lua& lua) {
    clearArgs(lua);
    char buf[64];
    if (!modapi::LocalId(buf, sizeof(buf))) return retNil(lua);
    return retString(lua, buf);
}

static int nPlayerActorPath(const Lua& lua) {
    int p = 0;
    const bool ok = argInt(lua, &p);
    clearArgs(lua);
    if (!ok || !modapi::OnGameThread()) return retNil(lua);
    const std::string path = pathOf(modapi::PlayerActor(p));
    if (path.empty()) return retNil(lua);
    return retString(lua, path);
}

static int nActorPlayerByPath(const Lua& lua) {
    std::string path;
    const bool ok = argString(lua, &path);
    clearArgs(lua);
    if (!ok || path.empty() || !modapi::OnGameThread()) return retInt(lua, -1);
    for (int p = 0; p < 32; p++) {
        void* actor = modapi::PlayerActor(p);
        if (actor && pathOf(actor) == path) return retInt(lua, p);
    }
    return retInt(lua, -1);
}

// ---- the prelude: `OpenMP` on top of the natives, per mod ---------------------------------------------
static const char kPrelude[] = R"LUA(
local N = {
    ver = __OmpMod_ApiVersion, reg = __OmpMod_Register, unreg = __OmpMod_Unregister, send = __OmpMod_Send,
    pending = __OmpMod_Pending, poll = __OmpMod_Poll, inSession = __OmpMod_InSession,
    players = __OmpMod_Players, isAuth = __OmpMod_IsAuthority, auth = __OmpMod_Authority,
    pname = __OmpMod_PlayerName, pid = __OmpMod_PlayerId, lname = __OmpMod_LocalName, lid = __OmpMod_LocalId,
    actorPath = __OmpMod_PlayerActorPath, byPath = __OmpMod_ActorPlayerByPath,
}

local function toHex(s)
    return (s:gsub(".", function(c) return string.format("%02x", string.byte(c)) end))
end
local function fromHex(h)
    return (h:gsub("%x%x", function(cc) return string.char(tonumber(cc, 16)) end))
end

local channels = {}
local queued = false
local pumpStarted = false

local function report(what, err)
    print(string.format("[OpenMP] %s: error in %s: %s\n", MOD, what, tostring(err)))
end

local function pump()
    queued = false
    local handles = {}
    for h in pairs(channels) do handles[#handles + 1] = h end
    for _, h in ipairs(handles) do
        local c = channels[h]
        while c and channels[h] do
            local kind, player, value = N.poll(h)
            if not kind then break end
            if kind == 1 then
                if c.onPlayer then
                    local ok, err = pcall(c.onPlayer, player, value == 1)
                    if not ok then report("onPlayer", err) end
                end
            elseif c.onMessage then
                local ok, err = pcall(c.onMessage, player, fromHex(value))
                if not ok then report("onMessage", err) end
            end
        end
    end
end

local function startPump()
    if pumpStarted then return end
    pumpStarted = true
    LoopAsync(10, function()
        if not queued and N.pending(MOD) > 0 then
            queued = true
            ExecuteInGameThread(pump)
        end
        return false
    end)
end

OpenMP = {
    ApiVersion = N.ver(),
    EVERYONE = -1,
    MAX_PAYLOAD = 1000,
}

function OpenMP.Register(name, onMessage, onPlayer)
    local h = N.reg(MOD, tostring(name))
    if h == 0 then return nil end
    channels[h] = { onMessage = onMessage, onPlayer = onPlayer }
    startPump()
    return h
end

function OpenMP.Unregister(channel)
    if channels[channel] then
        channels[channel] = nil
        N.unreg(channel)
    end
end

function OpenMP.Send(channel, player, data, reliable)
    if type(channel) ~= "number" or type(data) ~= "string" or #data > 1000 then return false end
    return N.send(channel, player or -1, toHex(data), reliable == false and 0 or 1) == 1
end

function OpenMP.InSession() return N.inSession() == 1 end
function OpenMP.Players(channel) return { N.players(channel) } end
function OpenMP.IsAuthority(channel) return N.isAuth(channel) == 1 end
function OpenMP.Authority(channel) return N.auth(channel) end
function OpenMP.PlayerName(player) return N.pname(player) end
function OpenMP.PlayerId(player) return N.pid(player) end
function OpenMP.LocalName() return N.lname() end
function OpenMP.LocalId() return N.lid() end

function OpenMP.PlayerActor(player)
    local path = N.actorPath(player)
    if not path then return nil end
    local obj = StaticFindObject(path)
    if obj and obj:IsValid() then return obj end
    return nil
end

function OpenMP.ActorPlayer(actor)
    if not actor or not actor:IsValid() then return -1 end
    local full = actor:GetFullName()
    return N.byPath(full:match("^%S+%s+(.+)$") or full)
end
)LUA";

// Lua mod folder names become a Lua string literal and a queue key: keep them to plain characters.
static std::string modKey(RC::StringViewType name) {
    std::string s;
    for (wchar_t ch : name) {
        const bool plain = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                           ch == L'_' || ch == L'-' || ch == L'.' || ch == L' ';
        s.push_back(plain ? (char)ch : '_');
    }
    return s.empty() ? std::string("unnamed") : s;
}

void OnLuaStart(RC::StringViewType modName, const Lua& lua, void (*logf)(const char*)) {
    if (logf) g_logf = logf;
    const std::string mod = modKey(modName);
    struct Native { const char* name; Lua::LuaFunction fn; };
    static const Native kNatives[] = {
        { "__OmpMod_ApiVersion", &nApiVersion },       { "__OmpMod_Register", &nRegister },
        { "__OmpMod_Unregister", &nUnregister },       { "__OmpMod_Send", &nSend },
        { "__OmpMod_Pending", &nPending },             { "__OmpMod_Poll", &nPoll },
        { "__OmpMod_InSession", &nInSession },         { "__OmpMod_Players", &nPlayers },
        { "__OmpMod_IsAuthority", &nIsAuthority },     { "__OmpMod_Authority", &nAuthority },
        { "__OmpMod_PlayerName", &nPlayerName },       { "__OmpMod_PlayerId", &nPlayerId },
        { "__OmpMod_LocalName", &nLocalName },         { "__OmpMod_LocalId", &nLocalId },
        { "__OmpMod_PlayerActorPath", &nPlayerActorPath }, { "__OmpMod_ActorPlayerByPath", &nActorPlayerByPath },
    };
    try {
        for (const Native& n : kNatives) lua.register_function(std::string(n.name), n.fn);
        const std::string code = "local MOD = \"" + mod + "\"\n" + kPrelude;
        lua.execute_string(std::string_view(code.c_str(), code.size()));
    } catch (...) {
        char m[200]; snprintf(m, sizeof(m), "[modapi/lua] %s: could not install the OpenMP table", mod.c_str());
        say(m);
    }
}

void OnLuaStop(RC::StringViewType modName) {
    const std::string mod = modKey(modName);
    std::vector<Chan*> stop;
    {
        std::lock_guard<std::mutex> lk(g_chansLock);
        for (Chan* c : g_chans) if (c->live && c->mod == mod) stop.push_back(c);
    }
    for (Chan* c : stop) retire(c);
}

}} // namespace omp::luaapi
