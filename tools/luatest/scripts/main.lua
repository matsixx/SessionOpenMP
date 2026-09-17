-- OmpLuaTest -- in-game test of the OpenMP mod channel from a UE4SS Lua mod. Not shipped.
-- Install: Mods\OmpLuaTest\scripts\main.lua plus "OmpLuaTest : 1" in Mods\mods.txt.
-- Output:  "[OmpLuaTest]" lines in UE4SS.log, and OmpLuaTest.log beside the game exe.
--
-- Speaks the SAME protocol as OmpModTest (the DLL test) on the same channel, "omp.modtest", so a game
-- running this and a game running the DLL test exercise each other. Enable only one of the two per game:
-- a channel name can be registered once.
--   join/leave     every onPlayer, with name, id, skater actor (and ActorPlayer back) and the authority
--   counter        the authority broadcasts +1 every 3 s; others check the sender and look for gaps
--   big            every 10 s, a full 1000-byte message with a checksum, verified on arrival
--   Ctrl+Shift+P   ping everyone, each replies
--   Ctrl+Shift+L   40 sends at once, logs how many were accepted (expect about 10)
--   status         a summary line every 10 s while in a session

local logFile = io.open("OmpLuaTest.log", "w")
local function Log(fmt, ...)
    local line = string.format("[OmpLuaTest] " .. fmt, ...)
    print(line .. "\n")
    if logFile then
        logFile:write(os.date("%H:%M:%S "), line, "\n")
        logFile:flush()
    end
end

if not OpenMP then
    Log("no OpenMP table: SessionOpenMP is not loaded, or it is older than 1.1.12")
    return
end
Log("OpenMP table present, API version %d", OpenMP.ApiVersion)

local FORMAT, COUNTER, BIG, PING, PONG = 1, 1, 2, 3, 4
local ch = nil
local counterSent, counterLast, counterSeen, counterGaps = 0, 0, 0, 0
local bigSent, bigOk, bigBad = 0, 0, 0
local pingSeq, pingsSent, pongs, answered = 0, 0, 0, 0
local refused, ignored = 0, 0
local ticks = 0          -- 10 ms each
local pingAt = {}

local function fnv1a(s, n)
    local h = 2166136261
    for i = 1, n do h = ((h ~ s:byte(i)) * 16777619) & 0xFFFFFFFF end
    return h
end

local function nameOf(player)
    if player < 0 then return OpenMP.LocalName() end
    return OpenMP.PlayerName(player) or ("player " .. player)
end

local function send(player, data)
    local ok = OpenMP.Send(ch, player, data)
    if not ok then refused = refused + 1 end
    return ok
end

-- ---------------------------------------------------------------- callbacks (game thread)
local function onPlayer(player, joined)
    local a = OpenMP.Authority(ch)
    local actor = OpenMP.PlayerActor(player)
    Log("%s %s the channel (id %s, skater actor %s). Authority now: %s%s", nameOf(player),
        joined and "JOINED" or "LEFT", OpenMP.PlayerId(player) or "?",
        actor and actor:GetFullName() or "none", nameOf(a), a < 0 and " (this game)" or "")
    if actor then
        Log("ActorPlayer(their skater) = %d (expect %d)", OpenMP.ActorPlayer(actor), player)
    end
end

local function onMessage(player, data)
    local len = #data
    if len < 2 or data:byte(1) ~= FORMAT then ignored = ignored + 1; return end
    local kind = data:byte(2)
    if kind == COUNTER and len == 6 then
        if player ~= OpenMP.Authority(ch) then
            Log("counter from %s IGNORED: they are not the authority", nameOf(player))
            return
        end
        local v = string.unpack("<I4", data, 3)
        if counterSeen > 0 and v ~= counterLast + 1 then
            counterGaps = counterGaps + 1
            Log("counter jumped %d -> %d (a gap is expected only when the authority changed)", counterLast, v)
        end
        counterLast, counterSeen = v, counterSeen + 1
    elseif kind == BIG and len == 1000 then
        local want = string.unpack("<I4", data, 997)
        local ok = fnv1a(data, 996) == want
        if ok then bigOk = bigOk + 1 else bigBad = bigBad + 1 end
        Log("1000-byte message from %s: %s", nameOf(player), ok and "checksum OK" or "*** CHECKSUM BAD")
    elseif kind == PING and len == 14 then
        send(player, string.char(FORMAT, PONG) .. data:sub(3))
        answered = answered + 1
        Log("ping from %s, answered", nameOf(player))
    elseif kind == PONG and len == 14 then
        local seq = string.unpack("<I4", data, 3)
        pongs = pongs + 1
        local at = pingAt[seq]
        Log("pong #%d from %s%s", seq, nameOf(player),
            at and string.format(": about %d ms round trip", (ticks - at) * 10) or "")
    else
        ignored = ignored + 1
    end
end

ch = OpenMP.Register("omp.modtest", onMessage, onPlayer)
if not ch then
    Log("Register REFUSED -- is OmpModTest (the DLL test) also enabled in this game? See SessionOpenMP.log")
    return
end
Log("channel omp.modtest registered (handle %d)", ch)

-- ---------------------------------------------------------------- keys
RegisterKeyBind(Key.P, { ModifierKey.CONTROL, ModifierKey.SHIFT }, function()
    if not OpenMP.InSession() then Log("Ctrl+Shift+P: not in a session"); return end
    pingSeq = pingSeq + 1
    pingsSent = pingsSent + 1
    pingAt[pingSeq] = ticks
    local ok = send(OpenMP.EVERYONE, string.pack("<BBI4I8", FORMAT, PING, pingSeq, ticks))
    Log("Ctrl+Shift+P: ping #%d to %d player(s) %s", pingSeq, #OpenMP.Players(ch), ok and "queued" or "REFUSED")
    ExecuteInGameThread(function()          -- actors only work on the game thread
        for _, p in ipairs(OpenMP.Players(ch)) do
            local actor = OpenMP.PlayerActor(p)
            Log("  %s: skater actor %s, ActorPlayer -> %d (expect %d)", nameOf(p),
                actor and actor:GetFullName() or "none", actor and OpenMP.ActorPlayer(actor) or -1, actor and p or -1)
        end
    end)
end)

RegisterKeyBind(Key.L, { ModifierKey.CONTROL, ModifierKey.SHIFT }, function()
    local accepted = 0
    for _ = 1, 40 do
        if OpenMP.Send(ch, OpenMP.EVERYONE, string.char(FORMAT, 0xEE), false) then accepted = accepted + 1 end
    end
    Log("Ctrl+Shift+L: rate limit test -- %d of 40 sends accepted (expect about 10)", accepted)
end)

-- ---------------------------------------------------------------- timers
local wasInSession = false
local seconds = 0
LoopAsync(10, function()
    ticks = ticks + 1
    if ticks % 100 ~= 0 then return false end
    seconds = seconds + 1

    local inSession = OpenMP.InSession()
    if inSession ~= wasInSession then
        wasInSession = inSession
        if inSession then
            Log("session started -- you are %s (id %s)", OpenMP.LocalName(), OpenMP.LocalId() or "?")
        else
            Log("session ended")
        end
    end
    if not inSession then return false end

    if seconds % 3 == 0 and OpenMP.IsAuthority(ch) then
        counterSent = counterSent + 1
        counterLast = counterSent
        send(OpenMP.EVERYONE, string.pack("<BBI4", FORMAT, COUNTER, counterSent))
    end

    local players = OpenMP.Players(ch)
    if seconds % 10 == 0 and #players > 0 then
        local parts = { string.char(FORMAT, BIG) }
        for i = 2, 995 do parts[#parts + 1] = string.char((i * 31 + ticks) & 0xFF) end
        local body = table.concat(parts)
        if send(OpenMP.EVERYONE, body .. string.pack("<I4", fnv1a(body, 996))) then bigSent = bigSent + 1 end
    end

    if seconds % 10 == 5 then
        local names = {}
        for i, p in ipairs(players) do names[i] = nameOf(p) end
        local a = OpenMP.Authority(ch)
        Log("STATUS players with the channel: %d [%s] | authority: %s | counter %d (gaps %d) | "
            .. "1000-byte sent %d, ok %d, bad %d | pings sent %d, pongs %d, answered %d | refused %d, ignored %d",
            #players, table.concat(names, ", "), a < 0 and "this game" or nameOf(a), counterLast, counterGaps,
            bigSent, bigOk, bigBad, pingsSent, pongs, answered, refused, ignored)
    end
    return false
end)
