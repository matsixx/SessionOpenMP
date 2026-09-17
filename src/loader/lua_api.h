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
// SessionOpenMP -- THE MOD CHANNEL FOR UE4SS LUA MODS: an `OpenMP` table in every Lua mod, with the
// same functions DLL mods get (docs/modding-api.md).
//
// UE4SS calls on_lua_start for every Lua mod BEFORE its main.lua runs (LuaMod::start_mod), so the table
// exists from the script's first line. Each mod gets native functions (__OmpMod_*) plus a Lua prelude
// that builds `OpenMP` on top of them.
//
// C++ NEVER CALLS INTO LUA. Messages and join/leave events queue here per channel; the prelude polls
// them from a LoopAsync and runs the mod's callbacks inside ExecuteInGameThread, UE4SS's own
// scheduling, which serializes Lua execution across threads with its action mutex. So Lua callbacks run
// on the game thread, like DLL callbacks, without this DLL touching a Lua state from a foreign thread.
//
// Payloads cross the native boundary as HEX: LuaMadeSimple's get_string/set_string stop at a zero byte.
// The prelude encodes and decodes, so Lua sees raw byte strings and the wire is identical to a DLL
// mod's -- a Lua mod and a DLL mod can share a channel.
// =====================================================================================================
#pragma once
#include "ue4ss_abi.h"

namespace omp { namespace luaapi {

void OnLuaStart(RC::StringViewType modName, const RC::LuaMadeSimple::Lua& lua, void (*logf)(const char*));
void OnLuaStop(RC::StringViewType modName);

}} // namespace omp::luaapi
