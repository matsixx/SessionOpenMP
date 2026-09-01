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
// SessionOpenMP -- the UE4SS C++ mod shell. This is the ONLY file that knows how the mod is loaded.
// WHY UE4SS AND NOT A PROXY DLL: UE4SS already owns the loader shim (dwmapi.dll) and is already
// installed for this game, so a system-DLL proxy would be a second, redundant injection path. As a
// UE4SS C++ mod this drops into `ue4ss/Mods/SessionOpenMP/dlls/main.dll` exactly like ConsoleEnablerMod.
// The per-frame game-thread anchor is a MinHook on UGameEngine::Tick (below), not UE4SS's on_update.
//
// BUILD CONSTRAINTS:
//   * UE4SS.dll imports MSVCP140/VCRUNTIME140 => the DYNAMIC CRT. CppUserModBase's ctor/dtor are
//     RC_UE4SS_API (imported), so a /MT mod would link two CRTs and corrupt on the first std:: type that
//     crosses the boundary. This target builds /MD, and ONLY this target.
//   * There is no prebuilt UE4SS.lib in the source tree, so the import library is generated from the
//     INSTALLED UE4SS.dll's exports (tools/ue4sslib). The ctor `??0CppUserModBase@RC@@QEAA@XZ` is exported,
//     which is what makes that legal.
// The RE-UE4SS zip lacks its submodules, so the real header cannot parse -- ue4ss_abi.h holds an
// ABI-faithful transcription instead (same members, same virtual order, ctor imported).
#include "ue4ss_abi.h"

#include "transport/transport.h"
#include "session/session.h"
#include "game/game_syms.h"
#include "game/gather.h"
#include "game/proxy.h"
#include "game/audio.h"
#include "game/pose.h"
#include "game/spectate.h"
#include "game/dropper.h"
#include "replication/dropsync.h"
#include "ui/overlay.h"
#include "ui/pause_menu.h"
#include "ui/version_tag.h"
#include "ui/trx_popup.h"
#include "ui/update_check.h"
#include "ui/mp_name.h"
#include "ui/chat.h"
#include "replication/replaysync.h"   // the sync-progress bubble below
#include "ui/nameplates.h"
#include "game/game_font.h"
#include "session/banlist.h"
#include "ui/mp_prefs.h"

#include <cstdio>
#include <cstring>
#include <windows.h>
#include "MinHook.h"

using namespace omp;

static FILE* g_log = nullptr;
static void logLine(const char* s) {
    SYSTEMTIME t; GetLocalTime(&t);
    if (g_log) { fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s); fflush(g_log); }
}
static uint64_t nowUs() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

// =====================================================================================================
// FINDING THE LOCAL PAWN
// In overlay mode every instance is a STANDALONE game, so there is exactly one local PlayerController --
// which makes controller->Pawn the shortest correct route, with no actor enumeration and no heuristics.
//   * the controller comes from UE4SS: `UObjectGlobals::FindFirstOf(L"PlayerController")`
//   * `AController::Pawn` is +0x250, read out of `AController::K2_GetPawn` (`mov rax,[rcx+0x250]`);
//     an 8-byte accessor is an exact oracle for the offset
//   * the result is CONFIRMED with `APawn::IsLocallyControlled` before it is believed. Heuristics
//     mistake NPCs, the other player and pre-travel skaters for the local one, so the engine's own
//     test gates everything.
// Re-validated EVERY frame: a level change or possession swap must never leave a freed pointer in use.
// =====================================================================================================
static const int kControllerPawn = 0x250;
static void* g_ownPawn = nullptr;
static void* g_lastWorld = nullptr;

// ARMED GATE: the mod is FULLY INERT until a session is armed by host/join. UE4SS's on_update fires
// on its EVENT-LOOP thread, and per-frame work run from there while a level streams in (FindFirstOf
// walking GUObjectArray from a third thread, vcalls on a freed menu pawn) parks the load forever.
static bool g_armed = false;

// =====================================================================================================
// THE GAME-THREAD ANCHOR: a MinHook on UGameEngine::Tick -- once per frame, on the game thread,
// OUTSIDE all script dispatch.
// Deliberately NOT a ProcessEvent pre-callback: that callback fires INSIDE the engine's script
// dispatcher, and SpawnActor from there synchronously loads the skater's assets in a context the asset
// linker does not expect. It faults in FLinkerLoad, and SEH cannot make that safe -- unwinding the
// engine's stack without its cleanup leaves the linker poisoned, which detonates much later in
// VerifyImportInner reading FLOAT BITS as a pointer. The visible half of the same fault is skaters
// frozen mid-construction with no clothes. RULE: work that can LOAD ASSETS runs from the engine's own
// tick, never from inside script dispatch. UE4SS's on_update remains event-loop-only.
// =====================================================================================================
static bool ownPawnStillValid();
static void discoverOwnPawn();
// Speech bubbles are keyed by transport peer index, so they must be dropped wherever the SLOTS are:
// a released index can be handed to a different human, and a line must never survive onto them.
static void clearChatBubbles();

// =====================================================================================================
// SESSION LIFECYCLE: F8 = HOST, F9 = JOIN, F6 = LEAVE. (F10 belongs to ConsoleEnabler.)
// EVERY transport call happens on the GAME THREAD. There is no transport thread and there must never
// be one: the EOS SDK is shared with the game, which ticks it from the game thread, so a second thread
// inside it crashes the game's own tick. Init only STARTS sign-in; `g_tpState` follows
// omp::InitState(), polled right after the per-frame omp::Tick that resolves it.
// =====================================================================================================
static volatile LONG g_tpState = 0;              // 0 not started, 1 initializing, 2 ready, 3 failed
static bool g_wantHost = false;
static bool g_lobbyRequested = false;
static char g_eosLogPath[MAX_PATH] = {0};
static omp::Backend g_wantBackend = omp::BK_SHM;

static bool g_tpFailLogged = false;
// The menu and the hotkeys both land here, in EVERY state -- fresh start, lobby retry, backend
// switch -- and every refusal is logged. There must be no blanket early-out on `g_tpState`: without a
// branch for each state, every menu click after any transport start (including a FAILED start and an
// F6 leave) silently does nothing, and a backend can never be switched without restarting the game.
// LOCAL (shared memory) initializes instantly, so it runs inline; ONLINE blocks on EOS login and
// must not touch the game thread (~1 s, 15 s worst case).
static void MpBegin(omp::Backend bk, bool asHost, const char* origin) {
    char m[200];
    const LONG tp = g_tpState;
    if (tp == 1) { logLine("[mp] transport is still starting -- give it a moment"); return; }
    if (g_armed) { logLine("[mp] already in a session -- Leave (F6) first"); return; }
    if (tp == 2 && omp::Current() == bk) {
        // The right wire is already up: this is a lobby (re)request -- host after a failed join,
        // retry after "nobody hosting", re-host after an F6 leave.
        const int ls = omp::LobbyStatus();
        if (ls == 1) { logLine("[mp] lobby request already in flight"); return; }
        if (ls >= 2) { logLine("[mp] already in a lobby -- Leave (F6) first"); return; }
        g_wantHost = asHost;
        snprintf(m, sizeof(m), "[mp] %s: %s on the current transport", origin, asHost ? "HOST" : "JOIN");
        logLine(m);
        if (!(asHost ? omp::LobbyHost() : omp::LobbyJoin())) logLine("[mp] lobby op refused");
        return;
    }
    if (tp == 2) {
        // Backend SWITCH (not armed). Session slots must die FIRST: they are keyed by transport peer
        // index, and shm slot 0 / EOS peer 0 are different humans -- a surviving stream would render
        // the newcomer on the old sender's clock.
        snprintf(m, sizeof(m), "[mp] %s: switching transport -> %s", origin,
                 (bk == omp::BK_EOS) ? "EOS (online)" : "shared memory (this PC)");
        logLine(m);
        session::ResetAll();
        clearChatBubbles();
        omp::Deactivate();
        g_lobbyRequested = false;
        InterlockedExchange(&g_tpState, 0);
    } else if (tp == 3) {
        logLine("[mp] retrying after a failed transport start");
        InterlockedExchange(&g_tpState, 0);
    }
    g_tpFailLogged = false;
    g_wantBackend = bk; g_wantHost = asHost;
    snprintf(m, sizeof(m), "[mp] %s: %s %s -- starting transport", origin,
             (bk == omp::BK_SHM) ? "LOCAL" : (bk == omp::BK_UDP) ? "DIRECT" : "ONLINE",
             asHost ? "HOST" : "JOIN");
    logLine(m);
    // THERE IS NO TRANSPORT THREAD, and there must never be one: the EOSSDK is SHARED with Session's
    // own EOSShared plugin, which ticks it from the game thread every frame, and two threads in one
    // SDK crash the GAME's tick (access violation in VCRUNTIME under FEOSSDKManager::Tick). Init only
    // STARTS sign-in and returns; readiness is polled from the game thread below. Shm and UDP come up
    // in microseconds and report ready immediately.
    // The route preference is passed for every backend so there is exactly one source for it.
    const bool started = omp::Init(bk, g_eosLogPath[0] ? g_eosLogPath : nullptr, MpPrefs_HideAddress());
    if (!started) { InterlockedExchange(&g_tpState, 3); return; }
    InterlockedExchange(&g_tpState, (LONG)omp::InitState());
}
static MpUiState g_ui;          // written on the game thread, read by the render thread (see overlay.h)

// ---- the lobby advertisement. Refreshed just before hosting, so the browser shows a real name and
// the map you are actually standing on rather than whatever you loaded into.
static void refreshLobbyAd() {
    char who[40] = {0}, map[40] = {0};
    if (g_ownPawn) game::LocalMapName(g_ownPawn, map, sizeof(map));
    // The advertised host name is the MULTIPLAYER name (filtered, chosen), never the private
    // in-game skater name -- same rule as the cosmetics packet.
    strncpy_s(who, MpName_Get(), _TRUNCATE);
    omp::SetLobbyAd(who, map[0] ? map : "Session");
}
// Browsing needs the EOS wire up but NO lobby operation -- looking is not joining. This is the one
// path that starts the transport without immediately asking for a lobby, so `g_lobbyRequested` is
// pre-set: MpPump's "transport ready -> host or join" step must not fire behind a browse.
static bool g_wantBrowse = false;
// A join code typed before the wire was up: consumed by the readiness step below, exactly like
// g_wantBrowse. Empty = nothing pending.
static char g_pendingCode[16] = {0};
// The pawn the lobby advertisement was last built from -- so the map name is re-published exactly
// once per pawn rather than every frame.
static void* g_adPawn = nullptr;

// The overlay logs through us so there is ONE log file for the whole mod.
void OvLog(const char* msg) { logLine(msg); }
static bool keyEdge(int vk, bool* held) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !*held;
    *held = down;
    return edge;
}
// Refresh what the menu displays. Called on EVERY MpPump path -- the pre-session states ("signing in",
// "transport failed") are exactly the ones the user most needs to see, and an early return that skipped
// the publish would leave the menu insisting there is no session while EOS is mid-login.
static void publishUi() {
    const session::Stats st = session::GetStats();
    g_ui.backend = (int)omp::Current();
    g_ui.tpState = (int)g_tpState;
    g_ui.lobby   = omp::LobbyStatus();
    g_ui.armed   = g_armed;
    g_ui.peers   = st.peers; g_ui.proxies = st.proxiesAlive; g_ui.pubHz = st.publishHz;
    // Read the bound endpoint back from the socket rather than echoing what was typed: a host should
    // see the port they ACTUALLY got, not the one they asked for.
    strncpy_s(g_ui.bound, (omp::Current() == omp::BK_UDP) ? omp::DirectBoundTo() : "", _TRUNCATE);
    g_ui.myMap[0] = 0;
    if (g_ownPawn) game::LocalMapName(g_ownPawn, g_ui.myMap, sizeof(g_ui.myMap));  // cached on the world ptr

    // The pretty-label asset must NOT wait for an armed session. It hangs off the game instance, and
    // the client that most needs map labels is the one BROWSING for a session to join -- which by
    // definition has not armed yet and so has no pawn to reach the game instance through.
    // `AActor::GetGameInstance` works on any actor, so the PlayerController does just as well as the
    // pawn. Throttled, and it stops the moment it succeeds -- FindFirstOf walks GUObjectArray.
    if (!game::HaveMapSelectData()) {
        static uint64_t lastMapTry = 0;
        const uint64_t ms = GetTickCount64();
        if (ms - lastMapTry > 2000) {
            lastMapTry = ms;
            void* pc = nullptr;
            __try { pc = (void*)RC::Unreal::UObjectGlobals::FindFirstOf(L"PlayerController"); }
            __except (EXCEPTION_EXECUTE_HANDLER) { pc = nullptr; }
            if (pc && game::CacheMapSelectData(pc))
                logLine("[mod] map labels available (UMapSelectDataAsset via the game instance)");
        }
    }
    // The chat box would like the game's own typeface (game_font.h). Same throttle and the same
    // reason -- it walks GUObjectArray -- and it stops for good on its first real answer, success or
    // "this build streams its fonts".
    {
        static uint64_t lastFontTry = 0;
        static bool fontSettled = false;
        const uint64_t ms = GetTickCount64();
        if (!fontSettled && ms - lastFontTry > 3000) {
            lastFontTry = ms;
            fontSettled = GameFont_Grab(logLine);
        }
    }
    // Relay room list, if one was asked for. Cheap: these are reads of an array the backend
    // already filled when the reply arrived.
    g_ui.browseState = omp::BrowseStatus();
    g_ui.browseCount = 0;
    if (g_ui.browseState == 2 && omp::Current() == omp::BK_RELAY) {
        const int n = omp::BrowseCount();
        for (int i = 0; i < n && g_ui.browseCount < 8; i++) {
            omp::LobbyInfo L{};
            if (!omp::BrowseAt(i, &L)) continue;
            MpUiState::RoomRow& r = g_ui.rooms[g_ui.browseCount++];
            strncpy_s(r.code, L.id, _TRUNCATE);
            strncpy_s(r.map, L.map, _TRUNCATE);
            r.players = L.players;
        }
    }
    // ---- "YOUR COPY IS OLD", in the game's own popup.
    //
    // The timing is not a guess: VersionTag_SawMenu() is true only when the version line was just
    // drawn BY A MENU, which is the moment the main menu exists, the game instance is reachable and
    // nothing is being interrupted. Waiting for it also means this can never fire mid-run.
    //
    // Said ONCE per launch. A version warning that reappears is a version warning people learn to
    // click past, and the one time it matters is the time they need to read it.
    {
        static bool told = false;
        char latest[32] = {0};
        if (!told && omp::ui::UpdateCheck_NewerAvailable(latest, sizeof(latest)) &&
            VersionTag_SawMenu()) {
            char body[320];
            snprintf(body, sizeof(body),
                     "You have %s. The latest is %s.\n\n"
                     "Close the game and run update.bat in your Session folder to update.\n\n"
                     "Players on different versions can join the same session and never see each "
                     "other, so it is worth doing before you play together.",
                     OMP_VERSION_STRING, latest);
            if (omp::ui::TrxPopup_Show("SessionOpenMP update available", body, "OK", logLine)) {
                told = true;
                char m[128];
                snprintf(m, sizeof(m), "[update] told the player about %s (running %s)",
                         latest, OMP_VERSION_STRING);
                logLine(m);
            }
            // Not shown yet? Leave `told` false and try on the next menu draw: the popup manager
            // may simply not be up, and there is no value in a warning that was swallowed.
        }
    }
    Overlay_Publish(&g_ui);
    PauseMenu_Publish(&g_ui);      // same snapshot, second surface
}

static void MpPump() {
    publishUi();
    // Perform any pause-menu page swap queued by a confirm LAST frame. It has to happen here, on the
    // engine tick, because our frame runs before the engine's -- and Slate (where the confirm fired)
    // runs inside the engine's. See pause_menu.h.
    PauseMenu_Pump();
    // The menu (F1) is the primary UI; these hotkeys stay as a keyboard path and a headless fallback.
    static bool f8 = false, f9 = false, f6 = false;
    bool host  = keyEdge(VK_F8, &f8);
    bool join  = keyEdge(VK_F9, &f9);
    bool leave = keyEdge(VK_F6, &f6);

    // ---- CHAT. Enter opens the box; from that moment the box owns the keyboard (its WndProc gate
    // lives in overlay.cpp) and closes itself on Enter or Escape.
    // Gated so it can only ever mean "chat" when nothing else could want the key: not in the pause
    // menu, not behind the F1 panel or the code prompt, and only once a session is armed -- there is
    // nobody to talk to otherwise. Nothing else is needed here: the opening key press reaching the
    // text field a frame later is handled where it lands, by chat.cpp's send arming.
    {
        static bool enterHeld = false;
        const bool edge = keyEdge(VK_RETURN, &enterHeld);
        if (edge && g_armed && !Chat_IsOpen() && !Overlay_Visible() && !game::PauseMenuOpen())
            Chat_SetOpen(true);
    }
    // Anything the player typed goes out RELIABLE to every live peer, and is echoed locally at the
    // same moment -- our own line must appear whether or not anybody is listening.
    {
        char line[160];
        while (Chat_Take(line, sizeof(line))) {
            static uint32_t chatId = 0;
            repl::ChatMsg cm;
            cm.id = ++chatId;
            strncpy_s(cm.name, MpName_Get(), _TRUNCATE);
            strncpy_s(cm.text, line, _TRUNCATE);
            Chat_Push(cm.name, cm.text, true);
            uint8_t pkt[256];
            const int n = repl::PackChat(cm, pkt, sizeof(pkt));
            if (n > 0) {
                omp::PeerStats ps{};
                const int nPeers = omp::PeerCount();
                for (int i = 0; i < nPeers; i++)
                    if (omp::GetStats(i, &ps) && ps.state != 5) omp::Send(i, pkt, n, true);
            }
        }
    }

    // ---- drain whatever the overlay queued (menu clicks land on the RENDER thread; they are executed
    // HERE, on the game thread, exactly like the hotkeys -- the UI never touches session state itself).
    // The origin tag reaches the log so "which button was it" is never a reconstruction.
    const OvAction act = (OvAction)Overlay_TakeAction();
    switch (act) {
        case OVA_HOST_LOCAL:  MpBegin(omp::BK_SHM, true,  "menu");  break;
        case OVA_JOIN_LOCAL:  MpBegin(omp::BK_SHM, false, "menu");  break;
        case OVA_HOST_ONLINE: MpBegin(omp::BK_EOS, true,  "menu");  break;
        case OVA_JOIN_ONLINE: MpBegin(omp::BK_EOS, false, "menu");  break;
        // The address was typed on the render thread and parked; collect it HERE and hand it to the
        // transport before starting, the same order SetLobbyAd/SetLobbyCode use: intent must be in
        // place before the backend exists.
        case OVA_HOST_DIRECT:
        case OVA_JOIN_DIRECT: {
            char addr[128] = {0}; int port = 0;
            Overlay_TakeDirect(addr, sizeof(addr), &port);
            omp::SetDirectEndpoint(addr, port);
            MpBegin(omp::BK_UDP, act == OVA_HOST_DIRECT, "menu");
            break;
        }
        // A relay session. Host and Join are the same act -- the first player to name a room
        // opens it -- so both arrive here and the flag only decides what the log says.
        case OVA_HOST_RELAY:
        case OVA_JOIN_RELAY: {
            char addr[128] = {0}; char room[16] = {0}; int port = 0;
            Overlay_TakeDirect(addr, sizeof(addr), &port);
            Overlay_TakeRoom(room, sizeof(room));
            omp::SetRelayServer(addr, port);
            omp::SetLobbyCode(room);
            MpBegin(omp::BK_RELAY, act == OVA_HOST_RELAY, "menu");
            break;
        }
        // Asking a relay what rooms it has does NOT start a session: the backend opens its own
        // socket for the question and the answer is polled from the menu.
        case OVA_BROWSE_RELAY: {
            char addr[128] = {0}; int port = 0;
            Overlay_TakeDirect(addr, sizeof(addr), &port);
            omp::SetRelayServer(addr, port);
            if (omp::Current() != omp::BK_RELAY) omp::Init(omp::BK_RELAY, nullptr, false);
            omp::LobbyBrowse();
            break;
        }
        case OVA_LEAVE:       leave = true; break;
        default: break;
    }
    // ---- and the same drain for the GAME'S OWN pause menu. It posts the identical OvAction values
    // from the game thread, so the only difference that reaches the log is the origin tag -- which is
    // exactly the point: "which surface was it" must never be a reconstruction.
    const int pauseAct = PauseMenu_TakeAction();
    switch (pauseAct) {
        case OVA_HOST_LOCAL:  MpBegin(omp::BK_SHM, true,  "pause");  break;
        case OVA_JOIN_LOCAL:  MpBegin(omp::BK_SHM, false, "pause");  break;
        case OVA_HOST_ONLINE: refreshLobbyAd(); omp::SetLobbyCode(""); MpBegin(omp::BK_EOS, true, "pause"); break;
        case OVA_JOIN_ONLINE: MpBegin(omp::BK_EOS, false, "pause");  break;
        case OVA_LEAVE:       leave = true; break;
        // Open the browser: bring EOS up if it is not already, then search. If the wire is still
        // starting we only remember the intent -- the search fires from the readiness check below.
        case OVA_BROWSE:
            g_wantBrowse = true;
            if (g_tpState == 2 && omp::Current() == omp::BK_EOS) { omp::LobbyBrowse(); g_wantBrowse = false; }
            else if (g_tpState != 1) MpBegin(omp::BK_EOS, false, "browse");
            break;
        // ---- PRIVATE GAMES. Hosting mints a code and advertises it; the public browser skips any
        // lobby that carries one, so the code is the only way in.
        case OVA_HOST_PRIVATE: {
            char code[16] = {0};
            omp::MakeLobbyCode(code, sizeof(code));
            refreshLobbyAd();
            omp::SetLobbyCode(code);
            char m[120];
            snprintf(m, sizeof(m), "[mp] pause: PRIVATE HOST -- code %s", code);
            logLine(m);
            MpBegin(omp::BK_EOS, true, "pause");
            break;
        }
        // A menu page cannot take free text, so the name is typed in the overlay's box. It validates
        // and saves itself on the render thread (overlay.h) -- we only re-advertise afterwards.
        case OVA_SET_NAME:
            Overlay_PromptName(true);
            logLine("[mp] pause: name box open");
            break;
        case OVA_JOIN_PRIVATE:
            // The overlay owns the typing; the code comes back through Overlay_TakeCode below.
            Overlay_PromptCode(true);
            logLine("[mp] pause: waiting for a join code");
            break;
        // ---- moderation of OUR OWN session. Both actions resolve the target by identity. There is no
        // exemption list and no identity this refuses to act on -- see banlist.h.
        case OVA_KICK: case OVA_BAN: {
            char id[80] = {0}, nm[40] = {0};
            if (!PauseMenu_TakePeerId(id, sizeof(id), nm, sizeof(nm)) || !id[0]) break;
            const bool ban = (pauseAct == OVA_BAN);
            char m[220];
            if (ban) Ban_Add(id, nm);
            if (!omp::LobbyKick(id)) {
                snprintf(m, sizeof(m), "[mp] pause: not the host of this session -- '%s' %s", nm,
                         ban ? "is on your ban list and will be removed from games you host" : "was not removed");
                logLine(m);
            } else {
                snprintf(m, sizeof(m), "[mp] pause: removed '%s'%s", nm, ban ? " and banned them" : "");
                logLine(m);
            }
            break;
        }
        case OVA_JOIN_INDEX: {
            const int idx = PauseMenu_TakeJoinIndex();
            char m[120];
            snprintf(m, sizeof(m), "[mp] pause: joining lobby #%d from the browser", idx);
            logLine(m);
            if (!omp::LobbyJoinAt(idx)) logLine("[mp] join refused -- the list may be stale; Refresh");
            break;
        }
        default: break;
    }
    // Bare F8/F9 mean LOCAL (this PC) by definition -- an accidental EOS login (a network op, a real
    // identity) must be an explicit menu choice. MpBegin handles every state, including retry.
    if (host || join) MpBegin(omp::BK_SHM, host, "hotkey");

    // A code typed into the prompt: bring EOS up if it is not already, then join by code. Same
    // shape as the browser's OVA_BROWSE -- looking up a code is not a lobby op until it matches.
    {
        char code[16] = {0};
        if (Overlay_TakeCode(code, sizeof(code)) && code[0]) {
            strncpy_s(g_pendingCode, code, _TRUNCATE);
            char m[120];
            snprintf(m, sizeof(m), "[mp] pause: joining private game with code %s", code);
            logLine(m);
            if (g_tpState == 2 && omp::Current() == omp::BK_EOS) {
                if (!omp::LobbyJoinByCode(g_pendingCode)) logLine("[mp] join-by-code refused");
                g_pendingCode[0] = 0;
            } else if (g_tpState != 1) {
                omp::SetLobbyCode("");            // we are a GUEST; do not advertise a code of our own
                MpBegin(omp::BK_EOS, false, "code");
            }
        }
    }

    const LONG tp = g_tpState;
    if (tp == 0) return;                          // nothing started
    // SIGN-IN MUST STILL BE TICKED. Sign-in is driven from omp::Tick, so a blanket early-out while
    // `tp == 1` leaves nothing driving it: the state sits at 1 forever and not even the 15 s timeout
    // can fire, because that lives in Tick too.
    // Everything below this needs a READY transport, so signing in ticks and returns.
    if (tp == 1) {
        omp::Tick([](int peer, const uint8_t* d, int len, void*) { session::OnPacket(peer, d, len, nowUs()); }, nullptr);
        const LONG st = (LONG)omp::InitState();
        if (st == 2 || st == 3) InterlockedExchange(&g_tpState, st);
        return;
    }
    if (tp == 3) {
        if (!g_tpFailLogged) {
            g_tpFailLogged = true;
            logLine("[mp] transport init FAILED -- see SessionOpenMP_eos.log; press Host/Join to retry");
        }
        return;
    }

    // ---- transport is up and game-thread-owned from here on.
    if (!g_lobbyRequested) {
        g_lobbyRequested = true;
        char m[160];
        snprintf(m, sizeof(m), "[mp] transport ready; my id = %s", omp::MyId());
        logLine(m);
        // A pending BROWSE takes precedence over the host/join intent: the wire was started to LOOK,
        // and auto-joining the first hit here is exactly the behaviour the browser replaces.
        if (g_pendingCode[0]) { if (!omp::LobbyJoinByCode(g_pendingCode)) logLine("[mp] join-by-code refused"); g_pendingCode[0] = 0; }
        else if (g_wantBrowse) { g_wantBrowse = false; omp::LobbyBrowse(); }
        else if (!(g_wantHost ? omp::LobbyHost() : omp::LobbyJoin())) logLine("[mp] lobby op refused");
    }
    if (g_pendingCode[0]) { if (!omp::LobbyJoinByCode(g_pendingCode)) logLine("[mp] join-by-code refused"); g_pendingCode[0] = 0; }
    if (g_wantBrowse) { g_wantBrowse = false; omp::LobbyBrowse(); }   // wire came up on an earlier path
    if (leave && omp::LobbyStatus() >= 2) {
        omp::LobbyLeave();
        // RELEASE THE SLOTS TOO, and do it BEFORE disarming. `Frame()` is gated on `g_armed`, so once
        // disarmed nothing retires the proxies -- dropping only the lobby leaves everyone you were
        // skating with standing in your now single-player world until the next map load. This is the
        // game thread, which is the only place allowed to hide an actor.
        session::ResetAll();
        clearChatBubbles();
        g_armed = false;
        logLine("[mp] leave -> left the lobby; session DISARMED");
    }

    // A name change has to reach the lobby advert, or the browser keeps offering your session under
    // the name you just stopped using. Cheap: one publish, only on an actual change.
    if (Overlay_TakeNameChanged()) {
        refreshLobbyAd();
        char m[120];
        snprintf(m, sizeof(m), "[mp] name is now \"%s\" -- advert refreshed", MpName_Get());
        logLine(m);
    }

    // ---- apply preference changes HERE, on the game thread. Both menus can write a preference from
    // whichever thread they run on (F1 = render, pause menu = game), and the EOS SDK must only be
    // driven from the thread that ticks its platform -- so a write only bumps a generation counter and
    // this is where it turns into an actual SDK call.
    {
        static unsigned lastGen = 0;
        const unsigned gen = MpPrefs_Generation();
        if (gen != lastGen) { lastGen = gen; omp::SetRelayControl(MpPrefs_HideAddress()); }
    }

    // pump the wire EVERY run once the transport is up -- lobby callbacks ride the platform tick, and
    // packets from roster peers may arrive before we consider ourselves armed.
    omp::Tick([](int peer, const uint8_t* d, int len, void*) { session::OnPacket(peer, d, len, nowUs()); }, nullptr);

    // (Sign-in cannot be in flight here -- the tp == 1 branch above owns that case and returns.)

    // ---- ENFORCE THE BAN LIST. Only while hosting: EOS grants kick authority to the lobby owner and
    // nobody else, so this is the whole of the moderation available -- and it is enough, because it
    // is exactly "you decide who plays in YOUR game". A kick is async and the member takes a moment
    // to leave, so each target is re-kicked at most once every few seconds rather than every frame.
    if (omp::LobbyIsHost()) {
        static uint64_t lastKickMs[16] = {0};
        const uint64_t nowMs = GetTickCount64();
        const int n = omp::PeerCount();
        for (int i = 0; i < n && i < 16; i++) {
            const char* id = omp::PeerIdStr(i);
            if (!id || !*id || !Ban_Is(id)) continue;
            if (nowMs - lastKickMs[i] < 5000) continue;
            lastKickMs[i] = nowMs;
            char m[160];
            snprintf(m, sizeof(m), "[ban] %s is on your ban list -- removing them from your session", id);
            logLine(m);
            omp::LobbyKick(id);
        }
    }

    // ---- arm on lobby entry; disarm if the lobby dissolves under us.
    static int lastLs = 0;
    const int ls = omp::LobbyStatus();
    if (ls != lastLs) {
        lastLs = ls;
        if (ls == 2 || ls == 3) {
            g_armed = true; logLine(ls == 2 ? "[mp] SESSION ARMED (hosting)" : "[mp] SESSION ARMED (joined)");
            { char p[192], m[224];
              omp::Posture(p, sizeof(p));
              snprintf(m, sizeof(m), "[mp] posture: %s", p);
              logLine(m); }
        }
        else if (ls == 0 && g_armed) { g_armed = false; clearChatBubbles(); logLine("[mp] lobby gone -- session DISARMED"); }
        else if (ls == -1) logLine("[mp] lobby op FAILED (retry from the menu)");
    }
}

// ---- THE FLOATING PLAYER NAMES: the GAME-thread half (ui/nameplates.h owns the drawing).
// Everything here has to happen on this thread -- the roster, each proxy's head position, and the
// world-to-screen projection all read game state -- and what crosses to the render thread is plain
// numbers. Positions leave NORMALISED against the game's own viewport, because the viewport and the
// window are different sizes whenever a resolution scale is set and only the render thread knows the
// second one.
static const int kMaxNameplates = 16;            // the lobby cap: one plate per peer is the maximum
// ---- SPEECH BUBBLES. What each peer last said, keyed by their TRANSPORT INDEX -- the name is a label
// two people can share and an actor pointer is valid only for the instant it is read, so the peer
// index is the one identity a store is allowed to hold on to.
struct ChatBubble { int peerId = -1; char text[160] = {0}; uint64_t atMs = 0; };
static ChatBubble g_bubbles[kMaxNameplates];
static void clearChatBubbles() { for (auto& b : g_bubbles) { b.peerId = -1; b.text[0] = 0; b.atMs = 0; } }
static void noteChatBubble(int peerId, const char* text) {
    if (peerId < 0 || !text || !*text) return;
    ChatBubble* slot = nullptr;
    for (auto& b : g_bubbles) if (b.peerId == peerId) { slot = &b; break; }
    if (!slot) for (auto& b : g_bubbles) if (b.peerId < 0) { slot = &b; break; }
    if (!slot) { slot = &g_bubbles[0]; for (auto& b : g_bubbles) if (b.atMs < slot->atMs) slot = &b; }
    slot->peerId = peerId;
    strncpy_s(slot->text, text, _TRUNCATE);
    slot->atMs = GetTickCount64();
}
// Their live line, or null. Expired entries are cleared here rather than on a timer -- this runs every
// frame anyway, and a line that has finished fading has no reason to still exist.
static const char* liveChatBubble(int peerId, uint64_t ms, uint32_t* ageOut) {
    const NameplateTuning& T = Nameplates_Tuning();
    const uint64_t life = (uint64_t)((T.bubbleHoldSec + T.bubbleFadeSec) * 1000.0f);
    for (auto& b : g_bubbles) {
        if (b.peerId != peerId || !b.text[0]) continue;
        const uint64_t age = ms - b.atMs;
        if (age > life) { b.text[0] = 0; return nullptr; }
        if (ageOut) *ageOut = (uint32_t)age;
        return b.text;
    }
    return nullptr;
}
// Last frame's outcome, for the heartbeat. "No names appeared" has several possible causes that look
// identical on screen, so each is counted separately rather than left to be guessed at.
static int  g_npPlated = 0, g_npNoName = 0, g_npOffScreen = 0, g_npVw = 0, g_npVh = 0;
static bool g_npShow = false;
static void publishNameplates() {
    NameplateItem items[kMaxNameplates];
    int  n = 0;
    bool show = false;
    // The player's own settings are the source of truth for the two ranges, pushed into the live
    // tuning here rather than copied at startup -- a change from either menu then takes effect on the
    // next frame with no apply step and no second copy to keep in sync.
    NameplateTuning& T = Nameplates_Tuning();
    T.maxDistCm       = (float)MpPrefs_NameDistM()   * 100.0f;
    T.bubbleMaxDistCm = (float)MpPrefs_BubbleDistM() * 100.0f;
    int vw = 0, vh = 0;
    g_npNoName = g_npOffScreen = 0;
    // A permanent early-out announces itself once: without these two symbols there is no projection
    // and no plate will ever be drawn, and that must not present as a silent nothing.
    {
        static bool warned = false;
        if (!warned && (!game::Get().ProjectToScreen || !game::Get().GetViewportSize)) {
            warned = true;
            logLine("[names] no world-to-screen symbol -- floating player names are OFF for this build");
        }
    }
    // No plates inside the replay editor (playback): a floating name over a replay -- yours or a
    // synced peer's -- is clutter in what is essentially a camera shot. Publishing the empty list
    // fades them out; live play resumes them the frame the editor closes.
    // ONE EXCEPTION: a peer whose replay is being FETCHED right now. That transfer takes seconds --
    // longer over a relay -- and it is started from inside the editor, so this gate is exactly where
    // the person waiting for it is standing. Their progress is the one thing worth drawing over a
    // replay, and nothing else gets through: the loop below drops every peer that is not mid-sync
    // while `inReplay`, and `show` stays false so no NAME fades in behind it.
    const bool inReplay = game::LocalReplayMode() == 2;
    if (T.enabled && g_ownPawn && game::ViewportSize(&vw, &vh)) {
        // The fade follows OUR board, not theirs: names are for looking around on foot, and a clean
        // screen is for skating.
        bool onBoard = false;
        __try {
            void* mv = *(void**)((uint8_t*)g_ownPawn + game::off::kSkaterMoveComp);
            if (mv) onBoard = *(uint8_t*)((uint8_t*)mv + game::off::kMoveOnBoard) > 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { onBoard = false; }

        const uint64_t ms = GetTickCount64();
        const int slots = session::PeerSlots();
        bool anyPeer = false;
        for (int s = 0; s < slots && n < kMaxNameplates; s++) {
            char  nm[32] = {0};
            void* actor = nullptr;
            int   peerId = -1;
            if (!session::PeerAt(s, nm, sizeof(nm), &actor, &peerId)) continue;
            anyPeer = true;
            // An empty name means their cosmetics packet (which is what carries it) has not landed
            // yet; it arrives within a second, and a blank plate in the meantime helps nobody.
            if (!nm[0] || !actor) { if (actor) g_npNoName++; continue; }
            float head[3], px[2], dist = 0.0f;
            if (!game::ActorHeadPoint(actor, T.headroomCm, head)) continue;
            // Behind the camera, out of view, or too far: all normal, and all indistinguishable from a
            // broken feature unless they are counted.
            if (!game::ProjectWorldToViewport(head, px, &dist)) { g_npOffScreen++; continue; }
            if (dist > T.maxDistCm) { g_npOffScreen++; continue; }
            NameplateItem& it = items[n++];
            memset(&it, 0, sizeof(it));
            strncpy_s(it.name, nm, _TRUNCATE);
            it.x = px[0] / (float)vw;
            it.y = px[1] / (float)vh;
            it.distCm = dist;
            uint32_t age = 0;
            // A SYNC IN PROGRESS OUTRANKS ANYTHING THEY SAID. Fetching a peer's replay takes seconds
            // -- longer over a relay -- and until now nothing on screen distinguished "working" from
            // "hung", which is most of why the wait felt broken. The transfer already knows exactly
            // where it is (the sender announces the chunk count, the requester counts what landed),
            // so this is a real percentage, not a guess. It rides the chat-bubble lane deliberately:
            // bubbles ignore the on/off-board fade, which is what makes this visible inside the
            // replay editor where the nameplates themselves are held down.
            int syncPct = 0;
            const replaysync::SyncState ss = replaysync::PeerSyncState(s, &syncPct);
            const bool syncing = (ss == replaysync::SyncState::Transferring ||
                                  ss == replaysync::SyncState::Failed);
            if (inReplay && !syncing) { n--; continue; }   // in the editor, ONLY a sync shows
            // A sync readout is not chat and must not be culled or faded by the chat-bubble
            // distance: the peer being fetched is frequently across the map, and "no indicator"
            // is indistinguishable from "hung", which is the whole reason this exists. Reporting
            // zero distance keeps it fully opaque at any range; the plate itself is already
            // suppressed, so nothing else rides in on it.
            if (syncing) it.distCm = 0.0f;
            if (ss == replaysync::SyncState::Transferring) {
                snprintf(it.msg, sizeof(it.msg), "syncing replay  %d%%", syncPct);
                it.msgAgeMs = 0;                      // never fades while it is still going
            } else if (ss == replaysync::SyncState::Failed) {
                strncpy_s(it.msg, "replay sync failed", _TRUNCATE);
                it.msgAgeMs = 0;
            } else if (const char* said = liveChatBubble(peerId, ms, &age)) {
                strncpy_s(it.msg, said, _TRUNCATE);
                it.msgAgeMs = age;
            } else if (session::PeerTyping(peerId)) {
                // The typing indicator: one dot, two, three, repeat -- synthesized fresh every
                // frame (age 0 keeps it fully opaque; the flag dropping is what removes it). A real
                // message always wins the bubble: the words beat the fact that words are coming.
                const int dots = 1 + (int)((ms / 350) % 3);
                for (int d = 0; d < dots; d++) it.msg[d] = '.';
                it.msg[dots] = 0;
                it.msgAgeMs = 0;
            }
        }
        // Whose board decides, and whether it decides at all, is the player's setting. The default
        // follows OUR board, not theirs: names are for looking around on foot, and a clean screen is
        // for skating. Gated on there being SOMEBODY here as well -- alone in a session the answer
        // can never be visible, and the render thread only pays for a frame while this is true.
        // Deliberately the ROSTER and not the plate count: a peer stepping behind you empties the
        // list for a moment and must not restart the fade.
        // Names OFF does not mean plates off: the bubbles ride the same list and are a separate
        // setting, so the items are still published and only the fade target is held down.
        const int mode = MpPrefs_NameMode();
        // Never in the editor: the sync bubble rides the bubble lane, which ignores this fade, so a
        // progress readout does not drag a nameplate onto the shot with it.
        show = !inReplay && anyPeer && (mode == MPNAME_ALWAYS || (mode == MPNAME_OFFBOARD && !onBoard));
    }
    // Published every frame, empty list included -- that is what makes the plates go away when the
    // session does, instead of hanging over a world that has moved on.
    Nameplates_Publish(items, n, show);
    g_npPlated = n; g_npShow = show; g_npVw = vw; g_npVh = vh;
}

static void GameThreadFrame() {
    static thread_local bool inFrame = false;    // our own spawns fire BeginPlay scripts; never re-enter
    if (inFrame) return;
    inFrame = true;
    const uint64_t us = nowUs();
    const uint64_t ms = GetTickCount64();

    MpPump();                                    // input + transport/lobby state machine; sets g_armed
    if (!g_armed) { inFrame = false; return; }

    // ---- audio capture. Installed on the first ARMED frame, never before: these detours sit on the
    // path EVERY sound in the game takes, and the standing rule is that the mod is fully inert until
    // a session exists. Idempotent, so the repeated call costs a bool test.
    game::audio::Install(&logLine);

    // pawn validity: re-checked every run, so a level change or possession swap cannot leave a freed
    // pointer in use.
    // The lobby advertisement is published before the pawn exists, so its map name starts empty; it is
    // refreshed once the pawn is known -- once per pawn, and only while hosting. It is refreshed again
    // on becoming host without having asked to (the previous host left and EOS handed the lobby over):
    // until then the browser offers strangers a session under the name and map of somebody who is no
    // longer in it, which is worse than no listing because it looks like a working session and is not
    // the one described.
    {
        static bool wasHost = false;
        const bool nowHost = (omp::LobbyStatus() == 2);
        if (nowHost && !wasHost) g_adPawn = nullptr;      // force the refresh below
        wasHost = nowHost;
    }
    if (g_ownPawn && g_ownPawn != g_adPawn && omp::LobbyStatus() == 2) {
        g_adPawn = g_ownPawn;
        refreshLobbyAd();
    }

    if (!ownPawnStillValid()) {
        g_ownPawn = nullptr;
        // Re-discover at 4 Hz: FindFirstOf walks GUObjectArray -- too costly to run 60x/s in a menu.
        static uint64_t lastTry = 0;
        if (ms - lastTry >= 250) { lastTry = ms; discoverOwnPawn(); }
    }

    // world change -> every actor we spawned died with it; drop the pointers before anything reads them.
    if (g_ownPawn) {
        const game::Syms& S = game::Get();
        void* w = nullptr;
        if (S.GetWorld) { __try { w = S.GetWorld(g_ownPawn); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (w && g_lastWorld && w != g_lastWorld) session::ForgetProxies();
        if (w) g_lastWorld = w;
    }

    // Audio attribution: a captured sound is transported only if it belongs to the LOCAL skater or
    // the LOCAL board. That is a WHITELIST, which is why no proxy registry is needed -- a proxy's own
    // sounds fail the test by construction, and so cannot be captured and echoed back out.
    {
        void* ownBoard = nullptr;
        if (g_ownPawn) {
            __try { ownBoard = *(void**)((uint8_t*)g_ownPawn + game::off::kSkaterBoard); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ownBoard = nullptr; }
        }
        game::audio::SetLocalParts(g_ownPawn, ownBoard);
        // The replay camera's "Me" needs a real actor to orbit, and the pawn is respawned across
        // deaths and map changes -- so republish it here rather than latching it once.
        game::spectate::SetLocalSkater(g_ownPawn);
    }

    // Keep peers out of YOUR recording -- EVERY frame, not on a timer. Replay components register
    // themselves at BeginPlay, and a proxy's board can arrive a frame or a minute after the skater,
    // so there is no single moment to hook: the invariant "no proxy component is registered" has to
    // hold continuously, or each appearance writes some of a peer's skating into your replay. The
    // pass is a scan of a couple of dozen pointers and, once the +0xb8 latch is set, removes nothing
    // -- cheaper than the check that would decide whether to run it.
    // ...unless peers are being RECORDED (the default): then their components stay registered and
    // the replay system captures everyone. The old prune remains the kill-switch path.
    // The replay camera's subject, checked before anything else this frame can use it: the engine
    // dereferences that pointer on its own tick, so it has to stop being a dead actor here rather
    // than at the next teardown that happens to notice.
    game::spectate::ValidateLookTarget(&logLine);
    if (!game::Proxy::Tuning().recordPeers) {
        game::spectate::PruneProxyComponents(&logLine);
    } else {
        // Peers stay registered through playback -- the replay editor shows recordings, and the
        // manager's own end-of-playback pass restores whatever is registered when it ends. The one
        // edge that needs help is a peer HIDDEN at that moment: parked components miss the restore,
        // so the exit handler re-registers them, runs the game's per-skater transition, and unhides.
        static uint8_t lastReplayMode = 0;
        const uint8_t rm = game::LocalReplayMode();
        if (rm != lastReplayMode) {
            if (lastReplayMode == 2) session::RestoreHiddenAfterReplay(&logLine);
            lastReplayMode = rm;
        }
    }

    // the wire was already pumped in MpPump (same thread, same run); just drive the frame.
    session::Frame(g_ownPawn, us, ms, &game::GatherOwnState);

    // After the frame, so a plate is placed on where its skater was just put rather than a frame behind.
    publishNameplates();

    // 1 Hz heartbeat: peers, proxies, publish rate. One line, so a dead channel is visible without
    // turning on anything special.
    static uint64_t lastLog = 0;
    if (ms - lastLog >= 1000) {
        lastLog = ms;
        const session::Stats st = session::GetStats();
        if (st.peers || st.proxiesAlive) {
            // Wide enough for the LONGEST line here, which is the dropped-object one. It was 200 and
            // silently lost its tail -- the world counters this feature is diagnosed by never
            // printed at all, and a truncated diagnostic reads exactly like a working one.
            char m[420];
            snprintf(m, sizeof(m), "[mod] peers=%d proxies=%d pubHz=%.0f published=%u received=%u applied=%u pawn=%p",
                     st.peers, st.proxiesAlive, st.publishHz, st.published, st.received, st.appliedFrames, g_ownPawn);
            logLine(m);
            // Audio, both directions on one line. SEND: what the funnel captured, how much of it was
            // ours, how many loops are live. RECV: what was played for peers and what could not be
            // (a cue this install does not have is named once, separately).
            // Dropped objects. `own` is what we publish, `remote` what is standing here for peers,
            // and `purged` counts remote props pulled back out of the game's own _allObjects -- the
            // guard that keeps a peer's rail out of the local save file. purged climbing steadily is
            // normal (it is re-asserted every enumeration); own or remote stuck at 0 with a peer in
            // the dropper is what names the broken half.
            const game::dropper::Stats d = game::dropper::St();
            const dropsync::Stats ds = dropsync::St();
            if (session::DropPolicy() && (st.dropOwn || st.dropRemote || d.arrayNum || ds.recv)) {
                // `array` is what the GAME says it has; `own` is what we published. The four skips
                // account for every object in between, so a gap can be attributed instead of guessed
                // at -- own < array with all skips at 0 means the array shrank under us mid-walk.
                snprintf(m, sizeof(m),
                         "[drop] array=%d own=%d (skip: world=%d remote=%d hidden=%d noClass=%d"
                         " noRoot=%d) remote=%d | spawned=%d failed=%d notInstalled=%d destroyed=%d"
                         " drift=%d movable=%d byName=%d purged=%d faults=%d mapDefaults=%d/%d"
                         " | world: session=%d missing=%d guard=%s",
                         d.arrayNum, st.dropOwn, d.skipWorld, d.skipRemote, d.skipHidden,
                         d.skipNoClass, d.skipNoRoot,
                         st.dropRemote, d.spawned, d.spawnFails, d.unknownIds, d.destroyed,
                         d.driftFixes, d.madeMovable, d.resolvedByName, d.purgedFromAll, d.faults,
                         d.mapDefaults, d.mapDefaultMissed,
                         st.dropWorld, d.worldMissing,
                         game::dropper::SaveGuardArmed() ? "on" : "OFF(look-only)");
                logLine(m);
                // The wire, both directions on one line: sets out vs sets in is the single comparison
                // that says which end of the lane lost a set.
                snprintf(m, sizeof(m),
                         "[drop/wire] sent=%d (sets=%d recs=%d unsendable=%d) | recv=%d"
                         " (sets=%d recs=%d) rejected=%d partsDropped=%d",
                         ds.sent, ds.setsSent, ds.setRecordsSent, ds.unsendable,
                         ds.recv, ds.setsRecv, ds.setRecordsRecv, ds.rejected, ds.partsDropped);
                logLine(m);
            }
            const game::audio::Stats a = game::audio::GetStats();
            snprintf(m, sizeof(m),
                     "[audio] sent: cap=%u loops=%u(live %u) shots=%u rej=%u nfySkip=%u | recv:"
                     " played=%u started=%u stopped=%u nfyMuted=%u noCue=%u faults=%u",
                     a.captured, a.loopsStarted, a.liveLoops, a.oneShots, a.rejected, a.notifySkipped,
                     a.played, a.playStarted, a.playStopped, a.notifyMuted, a.unresolved, a.faults);
            logLine(m);
            // The floating names. `show` is the local on/off-board fade target; `plated` is how many
            // reached the render thread. plated=0 with peers alive says which step lost them:
            // viewport 0x0 = no view resolved, noName = their cosmetics packet has not landed,
            // offScreen = behind the camera or past maxDistCm (both normal).
            snprintf(m, sizeof(m), "[names] show=%d plated=%d noName=%d offScreen=%d viewport=%dx%d",
                     g_npShow ? 1 : 0, g_npPlated, g_npNoName, g_npOffScreen, g_npVw, g_npVh);
            logLine(m);
            // The pose lane, both ends. `noted` climbing with `applied` flat means the seam is not
            // firing for that mesh; a `skipCnt` means the wire and the mesh disagree on bone count,
            // which is a different problem entirely. Silent when nobody is scrubbing.
            // `holdApplied` is the one to read while scrubbing with a peer on screen: climbing means
            // FinalizeBoneTransform still fires for a proxy mesh during playback, so a peer's LIVE
            // pose can reach that same seam; flat at 0 means the seam is dead during replay.
            const game::pose::Stats p = game::pose::GetStats();
            if (p.captured || p.noted || p.applied || p.faults || p.holdApplied) {
                snprintf(m, sizeof(m),
                         "[pose] cap=%u(%uB) noted=%u hook=%u applied=%u pfx=%u stale=%u skipCnt=%u"
                         " meshBones=%u faults=%u held=%u holdApplied=%u"
                         " | sweeps=%u liveN=%u slice=%u wiped=%u noSlice=%u mapped=%u/%u",
                         p.captured, p.bones, p.noted, p.hookCalls, p.applied, p.prefixStamps,
                         p.stale, p.skippedCount, p.meshBones, p.faults, p.held, p.holdApplied,
                         p.sweeps, p.liveN, p.sliceBones, p.wiped, p.noSlice, p.mappedBones,
                         p.unmappedBones);
                logLine(m);
                // Measurement only, and silent outside replay playback: whether proxy anim graphs
                // still UPDATE while the local player scrubs, and the state of the standard
                // suppression knobs. Settles whether the driver lane could ever serve the scrubber.
                game::ReplayDriverProbe(ms, logLine);
            }
        }
    }
    inFrame = false;
}

static bool pawnIsOurs(void* p) {
    const game::Syms& S = game::Get();
    if (!p || !S.IsLocallyControlled) return false;
    __try { return S.IsLocallyControlled(p); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ownPawnStillValid() { return g_ownPawn && pawnIsOurs(g_ownPawn); }

static void discoverOwnPawn() {
    void* pc = nullptr;
    __try { pc = (void*)RC::Unreal::UObjectGlobals::FindFirstOf(L"PlayerController"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pc) return;
    void* pawn = nullptr;
    __try { pawn = *(void**)((uint8_t*)pc + kControllerPawn); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pawnIsOurs(pawn)) return;                   // engine's own test, never a guess
    // ...but "the engine says this is the local pawn" is not enough on its own, because a proxy is a
    // real actor of the local player's own skater class. If the controller ever possesses one, the
    // engine's test passes and every downstream consumer -- publishing, spawning, the world handle --
    // starts working from a remote player's body. Refusing here keeps the last good pawn instead,
    // which is recoverable; adopting a proxy is not. This should now be unreachable (spawning is held
    // until the world settles), so it says so loudly rather than papering over it silently.
    if (session::IsProxyActor(pawn)) {
        static void* moaned = nullptr;
        if (moaned != pawn) {
            moaned = pawn;
            char m[190];
            snprintf(m, sizeof(m), "[mod] *** the player controller is possessing PROXY %p -- refusing to treat it "
                                   "as our pawn (keeping %p)", pawn, g_ownPawn);
            logLine(m);
        }
        return;
    }
    g_ownPawn = pawn;
    game::SetLocalController(pc);                    // crank defs resolve through the local
                                                     // controller's tricks DB on both ends
    static void* announced = nullptr;
    if (announced != pawn) {
        announced = pawn;
        char m[190];
        snprintf(m, sizeof(m), "[mod] own pawn = %p (via PlayerController %p +0x250, IsLocallyControlled confirmed)",
                 pawn, pc);
        logLine(m);
    }
}


// =====================================================================================================
// THE RENAME GUARD (see game_syms.h for the why).
// PRE-EMPT, NEVER CATCH: if the requested name is already held in the target outer, skip the rename and
// claim success. Nothing is mutated, nothing is half-done, and the object keeps the unique auto-name UE
// already gave it. Catching the fatal instead corrupts the UObject hash tables, which surfaces later as
// a wild jump or a GC crash in ConditionalBeginDestroy.
// =====================================================================================================
using RenameFn = bool (*)(void* obj, const wchar_t* newName, void* newOuter, uint32_t flags);
static RenameFn o_Rename = nullptr;
static bool renameWouldCollide(void* obj, const wchar_t* newName, void* newOuter) {
    const game::Syms& S = game::Get();
    if (!obj || !newName || !S.StaticFindObject) return false;
    __try {
        void* outer = newOuter ? newOuter : *(void**)((uint8_t*)obj + 0x20);  // null NewOuter = keep current
        if (!outer) return false;
        void* hit = S.StaticFindObject(nullptr, outer, newName, 0);
        if (!hit || hit == obj) return false;                                 // name free, or already ours
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 6) {
            char m[240];
            snprintf(m, sizeof(m), "[rename] PRE-EMPTED #%ld: obj=%p wanted a name held by %p (outer=%p) -- "
                                   "skipped; the auto-name is valid, the fatal is not survivable", n, obj, hit, outer);
            logLine(m);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool hkRename(void* obj, const wchar_t* newName, void* newOuter, uint32_t flags) {
    if (renameWouldCollide(obj, newName, newOuter)) return true;              // "renamed OK"
    return o_Rename(obj, newName, newOuter, flags);
}
static bool InstallRenameGuard() {
    const game::Syms& S = game::Get();
    if (!S.RenameObj || !S.StaticFindObject) { logLine("[mod] *** rename guard unresolved"); return false; }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { logLine("[mod] *** MinHook init failed"); return false; }
    if (MH_CreateHook(S.RenameObj, (void*)&hkRename, (void**)&o_Rename) == MH_OK &&
        MH_EnableHook(S.RenameObj) == MH_OK) {
        logLine("[mod] rename guard armed (UObject::Rename collisions pre-empted)");
        return true;
    }
    logLine("[mod] *** rename guard hook failed");
    return false;
}

// UGameEngine::Tick(float DeltaSeconds, bool bIdleMode). The mod's frame runs BEFORE the engine's --
// the world is in its settled post-render state, exactly what the gather wants.
static void (*o_EngineTick)(void*, float, char) = nullptr;
static void hkEngineTick(void* self, float dt, char idle) {
    GameThreadFrame();
    o_EngineTick(self, dt, idle);
}
// The pose blob's apply point: POST NativeUpdateAnimation, the last writer before the pose is
// evaluated. The original runs first; AnimPostApply overwrites for proxy-owned instances only.
static void (*o_AnimUpdate)(void*, float) = nullptr;
static void hkAnimUpdate(void* self, float dt) {
    o_AnimUpdate(self, dt);
    game::AnimPostApply(self);
}
static void InstallAnimApplyHook() {
    const game::Syms& S = game::Get();
    if (!S.AnimUpdate) { logLine("[mod] AnimUpdate unresolved -- proxy pose will be LOCAL-derived (stiff)"); return; }
    if (MH_CreateHook(S.AnimUpdate, (void*)&hkAnimUpdate, (void**)&o_AnimUpdate) == MH_OK &&
        MH_EnableHook(S.AnimUpdate) == MH_OK)
        logLine("[mod] anim post-pass hooked (pose blob applies AFTER NativeUpdateAnimation)");
    else
        logLine("[mod] *** anim post-pass hook failed -- proxy pose will be LOCAL-derived (stiff)");
}

// =====================================================================================================
// THE REPLAY-EDITOR GUARD. The hooked function is a THIN WRAPPER: its only own work is re-parenting
// the skater's replay camera LIGHT -- `this->CameraLight[0xa68]->AttachToComponent(` playback ?
// `<manager>->Root : this->FollowCamera[0xa50])` -- then it tail-jumps (+0x6f, E9 rel32) into the
// REAL mode handler at +0x80, which saves/restores movement mode, ragdoll and physics state and
// never touches either camera field (verified over its full 226 instructions). CameraLight is null
// on proxies (the original AV inside AttachToComponent), but the light attach is replay-editor
// LIGHTING for the filmed skater -- a proxy does not need it. So proxy transitions go STRAIGHT TO
// THE INNER HANDLER: full record/playback state transitions (which keep their replay cursors sane
// across editor sessions), zero exposure to the null field. Gating entries on the field instead
// (the previous shape) silently kept the proxy OUT of playback whenever it was null -- the skater
// stood off its replayed board while the board's own recording played fine.
// It must PASS THROUGH for the local player, or the replay editor breaks for everyone.
static void (*o_SkaterReplayMode)(void*, uint8_t) = nullptr;
static void (*g_replayModeInner)(void*, uint8_t) = nullptr;
// Byte-verified derivation of the inner handler from the wrapper. Must run BEFORE MH_CreateHook --
// MinHook overwrites the wrapper's first instruction (the +0x00 anchor) with its detour jmp.
static void DeriveReplayModeInner(void* wrapper) {
#ifdef _WIN32
    __try {
        const uint8_t* w = (const uint8_t*)wrapper;
        static const uint8_t kPro[5]   = { 0x48, 0x89, 0x5C, 0x24, 0x08 };  // mov [rsp+8], rbx
        static const uint8_t kCmp[3]   = { 0x80, 0xFA, 0x02 };              // cmp dl, 2
        static const uint8_t kInner[5] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };  // mov [rsp+0x10], rbx
        if (memcmp(w, kPro, 5) != 0 || memcmp(w + 0x17, kCmp, 3) != 0 || w[0x6f] != 0xE9) return;
        int32_t rel = *(const int32_t*)(w + 0x70);
        const uint8_t* inner = w + 0x74 + rel;
        if (memcmp(inner, kInner, 5) != 0) return;
        g_replayModeInner = (void(*)(void*, uint8_t))inner;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_replayModeInner = nullptr; }
#endif
}
static void hkSkaterReplayMode(void* skater, uint8_t mode) {
    bool isProxy = false;
    __try { isProxy = game::IsProxyActor(skater); } __except (EXCEPTION_EXECUTE_HANDLER) { isProxy = false; }
    if (isProxy) {
        // Proxies are OUT of the replay system entirely (recordPeers false: components pruned
        // continuously, actors concealed during local playback), so the game's per-skater replay
        // transition has nothing to manage on them -- and its playback entry attaches CameraLight
        // [0xa68], null on proxies (the original AV). Block every transition: the months-stable
        // original shape. When a transferred-replay mode arrives, it drives proxies through the
        // MOD's live pipeline, never through this handler. (g_replayModeInner stays derived above
        // for that future -- calling it would run the state work without the light attach.)
        return;
    }
    // Ours: pass through, and record the mode. This is the exact call that knows whether the local
    // player is scrubbing, so the diagnostics get it for free.
    game::SetLocalReplayMode(mode);
    {
        char m[120];
        snprintf(m, sizeof(m), "[replay] local replay mode -> %u", (unsigned)mode);
        logLine(m);
    }
    o_SkaterReplayMode(skater, mode);
}
static void InstallReplayGuard() {
    const game::Syms& S = game::Get();
    if (!S.SkaterReplayMode) {
        logLine("[mod] SkaterReplayMode unresolved -- the replay editor will CRASH while a proxy exists");
        return;
    }
    DeriveReplayModeInner(S.SkaterReplayMode);   // before the hook patches the wrapper's prologue
    if (MH_CreateHook(S.SkaterReplayMode, (void*)&hkSkaterReplayMode, (void**)&o_SkaterReplayMode) == MH_OK &&
        MH_EnableHook(S.SkaterReplayMode) == MH_OK) {
        // Hand the game layer a transition caller for restoring a proxy after playback. Its only
        // callers are proxy-side, so it gets the inner handler too when derived -- the wrapper's
        // light attach would AV (SEH-swallowed, transition lost) on a null CameraLight.
        game::SetSkaterReplayModeCaller(g_replayModeInner ? g_replayModeInner : o_SkaterReplayMode);
        logLine(g_replayModeInner
            ? "[mod] replay-editor guard installed (proxy transitions -> inner handler)"
            : "[mod] replay-editor guard installed (inner handler NOT derived -- proxy playback "
              "entries gated on the attach field)");
    }
    else
        logLine("[mod] *** replay-editor guard FAILED -- opening the replay editor may crash");
}

// ---- THE MARKER GUARD. ASkaterCharacter::PopulateMarkerInfo reads FollowCamera without a null
// check; on proxies that component is null (the replay guard's finding, one field over), and break
// sync opened the game's own path there for proxies: PostInitCharacter's deferred streamable
// completion populates the marker only when the skater's BOARD IS BROKEN -- a state only load-ins
// with a saved broken board reached before, and one every broken proxy board now sits in (crash:
// AV at PopulateMarkerInfo+0x1e3 off a streamable-manager tick). `self` is the derived-part
// SUB-OBJECT at actor+0xa40 -- the virtual dispatch carries that pointer, not the actor.
// The skip writes a zeroed struct: IsSet=0 is the "no marker" shape every consumer handles.
static void (*o_PopulateMarkerInfo)(void*, void*) = nullptr;
static void hkPopulateMarkerInfo(void* self, void* out) {
    bool skip = false;
#ifdef _WIN32
    __try {
        void* actor = (uint8_t*)self - game::off::kSkaterDerivedPart;
        void* cam   = *(void**)((uint8_t*)actor + game::off::kSkaterFollowCam);
        skip = (cam == nullptr) || game::IsProxyActor(actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) { skip = true; }
    if (skip) {
        __try { memset(out, 0, game::off::kMarkerInfoSize); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
#endif
    o_PopulateMarkerInfo(self, out);
}
static void InstallMarkerGuard() {
    const game::Syms& S = game::Get();
    if (!S.PopulateMarkerInfo) {
        logLine("[mod] PopulateMarkerInfo unresolved -- a broken proxy board can CRASH the deferred"
                " character init");
        return;
    }
    if (MH_CreateHook(S.PopulateMarkerInfo, (void*)&hkPopulateMarkerInfo,
                      (void**)&o_PopulateMarkerInfo) == MH_OK &&
        MH_EnableHook(S.PopulateMarkerInfo) == MH_OK)
        logLine("[mod] marker guard installed (null-camera / proxy skaters -> IsSet=0)");
    else
        logLine("[mod] *** marker guard FAILED -- a broken proxy board can crash the deferred init");
}

// ---- THE POSE SEAM. Fires for every skeletal mesh in the game and is silent unless the mesh belongs
// to a proxy with a fresh transported pose. PRE-hook by necessity: the original's first act is the
// buffer flip, so this is the last moment the finished pose can still be replaced.
static void (*o_MeshFinalizeBones)(void*) = nullptr;
static void hkMeshFinalizeBones(void* mesh) {
    game::pose::OnFinalizeBones(mesh, GetTickCount64());
    o_MeshFinalizeBones(mesh);
}
// ---- THE MAP-DEFAULT SEAM ---------------------------------------------------------------------------
// `UObjectDropperPersistentHandler::Load` applies the player's saved arrangement of the level's own
// props. Before it runs, every prop is where the MAP put it; after it, they are where that player
// left them -- and nothing later can tell the two apart, because each player's copy of the level is
// already arranged their own way by the time anything else gets to look.
// So Load is bracketed with a flag, and `AActor::SetActorLocation` -- which is how Load moves each
// prop -- captures the pose it is ABOUT to overwrite, but only while that flag is set. Outside that
// one moment the hook is a single predictable branch, and it fires for exactly the props whose
// default differs from where they now stand.
// MEASUREMENT ONLY: nothing consumes the table yet. It is the piece two withdrawn features both
// needed, being proven on its own before anything is built on top of it.
static void (*o_DropperLoad)(void*) = nullptr;
static void hkDropperLoad(void* handler) {
    game::dropper::SetInPersistentLoad(true);
    o_DropperLoad(handler);
    game::dropper::SetInPersistentLoad(false);
    char m[160];
    snprintf(m, sizeof(m), "[drop/map] the dropper applied this player's save -- captured %d map"
             " default(s)", game::dropper::MapDefaultCount());
    logLine(m);
}
static bool (*o_ActorSetLocation)(void*, const void*, bool, void*, unsigned char) = nullptr;
static bool hkActorSetLocation(void* actor, const void* newLoc, bool sweep, void* hit,
                               unsigned char teleport) {
    if (game::dropper::InPersistentLoad()) game::dropper::NoteMapDefault(actor);
    return o_ActorSetLocation(actor, newLoc, sweep, hit, teleport);
}
static void InstallMapDefaultSeam() {
    static bool done = false;                 // called from the ctor AND the unreal-init chain
    if (done) return;
    done = true;
    const game::Syms& S = game::Get();
    // Both or neither: the flag without the capture records nothing, and the capture without the flag
    // would fire for every SetActorLocation in the game. Say which one is missing -- "it silently did
    // nothing" is the failure mode this whole feature exists to remove.
    if (!S.DropperLoad || !S.ActorSetLocation) {
        char m[190];
        snprintf(m, sizeof(m), "[drop/map] not hooked (%s%s%s unresolved) -- map defaults unavailable",
                 S.DropperLoad ? "" : "DropperLoad",
                 (!S.DropperLoad && !S.ActorSetLocation) ? " and " : "",
                 S.ActorSetLocation ? "" : "ActorSetLocation");
        logLine(m);
        return;
    }
    if (MH_CreateHook(S.DropperLoad, (void*)&hkDropperLoad, (void**)&o_DropperLoad) == MH_OK &&
        MH_EnableHook(S.DropperLoad) == MH_OK &&
        MH_CreateHook(S.ActorSetLocation, (void*)&hkActorSetLocation, (void**)&o_ActorSetLocation) == MH_OK &&
        MH_EnableHook(S.ActorSetLocation) == MH_OK)
        logLine("[drop/map] map-default seam hooked (level props' original poses will be captured)");
    else
        logLine("[drop/map] *** map-default seam hook FAILED -- map defaults unavailable");
}

// ---- THE HARD SAVE GUARD ----------------------------------------------------------------------------
// Session copies of the level's props are deliberately PICKABLE -- that is what lets anybody rearrange
// them -- and a pickable prop of ours can be selected, which puts it in the manager's `_allObjects`,
// which is the list the save is written from. The per-poll purge is a race against exactly that
// window. This is not: whatever the state of the world, our actors are out of that list at the moment
// the game writes the file.
static void (*o_DropperSave)(void*, unsigned char) = nullptr;
static void hkDropperSave(void* handler, unsigned char flag) {
    const int removed = game::dropper::PurgeOursFromSaveList();
    if (removed > 0) {
        char m[170];
        snprintf(m, sizeof(m), "[drop/save] pulled %d of our object(s) out of the save list before"
                 " writing -- your profile keeps only your own", removed);
        logLine(m);
    }
    // THE WORLD BRACKET. During a session the level's own props stand on the HOST'S arrangement --
    // and this save is what writes their poses into the local profile. Standing each touched prop on
    // its remembered original for exactly the duration of the write means a mid-session save always
    // records the player's OWN arrangement; the session poses come straight back after. Without this,
    // leaving the dropper mid-session would quietly overwrite the player's park with the host's.
    game::dropper::WorldSaveRestoreBegin();
    o_DropperSave(handler, flag);
    game::dropper::WorldSaveRestoreEnd();
}
static void InstallSaveGuard() {
    static bool done = false;                 // called from the ctor AND the unreal-init chain
    if (done) return;
    done = true;
    const game::Syms& S = game::Get();
    if (!S.DropperSave) {
        logLine("[drop/save] *** DropperSave unresolved -- session props stay UNPICKABLE for safety");
        game::dropper::SetSaveGuardArmed(false);
        return;
    }
    if (MH_CreateHook(S.DropperSave, (void*)&hkDropperSave, (void**)&o_DropperSave) == MH_OK &&
        MH_EnableHook(S.DropperSave) == MH_OK) {
        game::dropper::SetSaveGuardArmed(true);
        logLine("[drop/save] save guard hooked (our objects can never reach your profile)");
    } else {
        game::dropper::SetSaveGuardArmed(false);
        logLine("[drop/save] *** save guard hook FAILED -- session props stay UNPICKABLE for safety");
    }
}

static void InstallPoseSeam() {
    const game::Syms& S = game::Get();
    if (!S.MeshFinalizeBones) { logLine("[mod] MeshFinalizeBones unresolved -- peers will not animate in replay"); return; }
    if (MH_CreateHook(S.MeshFinalizeBones, (void*)&hkMeshFinalizeBones, (void**)&o_MeshFinalizeBones) == MH_OK &&
        MH_EnableHook(S.MeshFinalizeBones) == MH_OK)
        logLine("[mod] pose seam hooked (transported skeletons apply before the buffer flip)");
    else
        logLine("[mod] *** pose seam hook failed -- peers will not animate in replay");
}

// ---- clamp the replay camera's keyframe index into its own array. Signature read off the caller and
// the callee together: (this, int idxA /*edx*/, int /*r8d*/, int idxB /*r9d*/, float alpha)
// -- arg5 is a stack float ([rsp+0x20] at the call site, [rsp+0x100] inside the 0xd8-byte frame).
static void (*o_CamReplaying)(void*, int, int, int, float) = nullptr;
static void hkCamReplaying(void* self, int a, int b, int c, float alpha) {
    int num = 0;
    __try { num = *(const int*)((const uint8_t*)self + 0x50); }   // the array's own Num
    __except (EXCEPTION_EXECUTE_HANDLER) { num = 0; }
    if (num >= 2) {
        const int last = num - 1, a0 = a, c0 = c;
        if (a < 0) a = 0; else if (a > last) a = last;
        if (c < 0) c = 0; else if (c > last) c = last;
        if (a != a0 || c != c0) {
            static long clamped = 0;
            if (InterlockedIncrement(&clamped) <= 3) {
                char m[190];
                snprintf(m, sizeof(m), "[replay] camera keyframe index out of range (%d,%d vs %d keys)"
                                       " -- clamped; the game does not bounds-check this", a0, c0, num);
                logLine(m);
            }
        }
    }
    o_CamReplaying(self, a, b, c, alpha);
}
// ---- same clamp for the derived float-track Replaying override. It calls the base (guarded above)
// first, then lerps its OWN float array -- count at [this+0xa0] -- with its own unclamped index
// copies. A component registered after recording began (a peer who joined mid-session) has tracks
// shorter than the manager's timeline, and the manager passes ONE global index pair to every
// component, so the short track reads past its allocation: garbage floats when the overrun lands on
// mapped heap, an AV when it does not (field crash: index 2722 into 2260 keys, seconds after a
// playback entry with a 76-second-old peer).
static void (*o_FloatTrackReplaying)(void*, int, int, int, float) = nullptr;
static void hkFloatTrackReplaying(void* self, int a, int b, int c, float alpha) {
    int num = 0;
    __try { num = *(const int*)((const uint8_t*)self + 0xa0); }   // this track's own Num
    __except (EXCEPTION_EXECUTE_HANDLER) { num = 0; }
    if (num >= 2) {
        const int last = num - 1, a0 = a, c0 = c;
        if (a < 0) a = 0; else if (a > last) a = last;
        if (c < 0) c = 0; else if (c > last) c = last;
        if (a != a0 || c != c0) {
            static long clamped = 0;
            if (InterlockedIncrement(&clamped) <= 3) {
                char m[190];
                snprintf(m, sizeof(m), "[replay] float-track keyframe index out of range (%d,%d vs %d"
                                       " keys) -- clamped; the game does not bounds-check this", a0, c0, num);
                logLine(m);
            }
        }
    }
    o_FloatTrackReplaying(self, a, b, c, alpha);
}
static void InstallReplayCamGuard() {
    const game::Syms& S = game::Get();
    if (!S.CamReplaying) { logLine("[mod] CamReplaying unresolved -- the replay editor may still crash"); return; }
    if (MH_CreateHook(S.CamReplaying, (void*)&hkCamReplaying, (void**)&o_CamReplaying) == MH_OK &&
        MH_EnableHook(S.CamReplaying) == MH_OK)
        logLine("[mod] replay camera guard installed (keyframe index clamped to the component's array)");
    else
        logLine("[mod] *** replay camera guard FAILED -- the replay editor may crash while scrubbing");
    if (!S.FloatTrackReplaying) { logLine("[mod] FloatTrackReplaying unresolved -- playback may crash on short tracks"); return; }
    if (MH_CreateHook(S.FloatTrackReplaying, (void*)&hkFloatTrackReplaying, (void**)&o_FloatTrackReplaying) == MH_OK &&
        MH_EnableHook(S.FloatTrackReplaying) == MH_OK)
        logLine("[mod] replay float-track guard installed (keyframe index clamped to the track's array)");
    else
        logLine("[mod] *** replay float-track guard FAILED -- playback may crash on short tracks");
}

static void InstallEngineTickAnchor() {
    const game::Syms& S = game::Get();
    if (!S.EngineTick) { logLine("[mod] *** EngineTick unresolved -- NO game-thread anchor, sessions dead"); return; }
    // The overlay thread also calls MH_Initialize; ALREADY_INITIALIZED is the benign collision.
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        logLine("[mod] *** MinHook init failed -- NO game-thread anchor, sessions dead");
        return;
    }
    if (MH_CreateHook(S.EngineTick, (void*)&hkEngineTick, (void**)&o_EngineTick) == MH_OK &&
        MH_EnableHook(S.EngineTick) == MH_OK)
        logLine("[mod] game-thread anchor: UGameEngine::Tick hooked (per-frame, outside script dispatch)");
    else
        logLine("[mod] *** UGameEngine::Tick hook failed -- NO game-thread anchor, sessions dead");
}

class SessionOpenMP : public RC::CppUserModBase
{
public:
    SessionOpenMP() {
        ModName = STR("SessionOpenMP");
        ModVersion = OMP_VERSION_WIDE;          // the same string the on-screen tag shows
        ModDescription = STR("Multiplayer as an overlay on N solo games -- no listen server, no UE netcode");
        ModAuthors = STR("matsix");

        char dir[MAX_PATH]{};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *(slash + 1) = 0;
        char path[MAX_PATH]{};
        snprintf(path, sizeof(path), "%sSessionOpenMP.log", dir);
        g_log = fopen(path, "w");    // fresh log per launch
        snprintf(g_eosLogPath, sizeof(g_eosLogPath), "%sSessionOpenMP_eos.log", dir);
        logLine("=== SessionOpenMP loading (UE4SS C++ mod) ===");
        MpName_Init(dir, logLine);   // multiplayer name + word filter (mp_name.h)
        Ban_Init(dir, logLine);     // who this host refuses to play with (banlist.h)
        MpPrefs_Init(dir, logLine);  // multiplayer preferences (mp_prefs.h) -- must precede any Init
        // Our permanent identity, handed to the transport before any backend starts. EOS ignores it
        // (a ProductUserId already identifies you); the direct-UDP wire has nothing else to go on.
        omp::SetLocalIdentity(MpPrefs_PeerId());
        logLine("[mp] keys: F8 = HOST session, F9 = JOIN session, F6 = leave");
        // THE BOOT RACE. Session applies the world-prop save during its INITIAL level load, ~7 s
        // after process start -- and the unreal-init chain used to hook the Load seam at ~8 s.
        // Whoever won that coin flip got map defaults; whoever lost reported an empty arrangement
        // for the rest of the run (field-measured: three friend boots won it, two boots the same
        // day lost it). The exe is fully mapped from the moment this DLL exists, so the sig scan
        // and these two hooks run HERE, before the game's own boot can reach a level load. The
        // handlers are early-safe by construction: pure statics until their functions first fire.
        {
            const MH_STATUS ms = MH_Initialize();
            if (ms == MH_OK || ms == MH_ERROR_ALREADY_INITIALIZED) {
                game::Resolve(logLine);
                InstallMapDefaultSeam();
                InstallSaveGuard();
            } else {
                logLine("[mod] *** MinHook unavailable at construction -- dropper seams wait for unreal-init");
            }
        }
    }
    ~SessionOpenMP() override {
        session::Shutdown();
        omp::Shutdown();
        logLine("=== SessionOpenMP unloaded ===");
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }

    // Resolve game symbols as soon as the Unreal module is up: the exe is fully mapped by then, and doing
    // it here (not in the ctor) keeps the scan off UE4SS's own startup path.
    auto on_unreal_init() -> void override {
        const game::Syms& S = game::Resolve(logLine);
        game::SetGatherLog(logLine);       // publish-side one-time facts only (see gather.h)
        session::Init(logLine);
        {   // Incoming chat -> the box. The session layer holds a function pointer so it never has to
            // know a UI exists (same seam shape as the log callback).
            session::Config sc = session::GetConfig();
            // Two consumers, one event: the chat window gets the line, and the speaker's own skater
            // gets a bubble. Keyed by the peer index, not the name -- see noteChatBubble.
            sc.onChat = [](int peerId, const char* name, const char* text) {
                Chat_Push(name, text, false);
                noteChatBubble(peerId, text);
            };
            // Version skew has no other symptom than a player who never appears, so it is said in the
            // chat box -- the one surface already on screen during play. Fires once per peer.
            sc.onNotice = [](const char* text) { Chat_System(text); };
            sc.isTyping = []() { return Chat_IsTyping(); };
            sc.onVersionMismatch = [](int) {
                Chat_System("A player here is running a different SessionOpenMP version, so you will not "
                            "see each other. Run update.bat in the game folder to get the latest.");
            };
            session::SetConfig(sc);
        }
        // The transport comes up lazily: EOS login costs ~1 s and must not block the game's first frames.
        // It is started on the first session action (host/join).
        char m[160];
        snprintf(m, sizeof(m), "[mod] symbols %d/%d resolved -- %s",
                 S.resolved, S.total,
                 (S.SpawnActor && S.SetReplicates && S.BoardSetLinVel)
                     ? "proxy pipeline ready"
                     : "*** required symbols missing; proxies disabled");
        logLine(m);
        // Register the game-thread anchor NOW, while the game is still early: the callback vector is
        // appended without a lock inside UE4SS, so the quietest possible moment is the safest one.
        // Unarmed cost: one branch per frame.
        // ORDER MATTERS: without the rename guard, the first proxy spawn is a GUARANTEED
        // LowLevelFatalError -- so no guard means no anchor, which means no sessions at all.
        if (InstallRenameGuard()) { InstallAnimApplyHook(); InstallReplayGuard(); InstallMarkerGuard(); InstallPoseSeam(); InstallMapDefaultSeam(); InstallSaveGuard(); InstallReplayCamGuard(); InstallEngineTickAnchor(); }
        else logLine("[mod] *** sessions DISABLED (rename guard missing)");
        // Pause must not freeze the world: in overlay mode YOUR pause stops your own skater and every
        // proxy in your world, so you cannot watch anyone while the menu is open. Unconditional -- it
        // is a 3-byte patch and the menu still works.
        game::DisablePause(logLine);
        // ...and, since the pause MENU still opens, put multiplayer in it. Game-thread hooks on the
        // engine's own menu code, so unlike the overlay this installs right here. It self-disables
        // loudly on any fault and shares nothing with the F1 path but MpUiState.
        PauseMenu_Install();
        // ...and put the mod's version beside the game's own, bottom-left.
        VersionTag_Install();
        // Ask GitHub whether this build is the newest, once, off the game thread. The answer is
        // shown at the main menu below if it says no -- see the pump.
        omp::ui::UpdateCheck_Start();
        // The menu installs itself on its own thread: it waits for the game's swapchain and, on DX12,
        // for the game's command queue -- neither exists this early, and neither may be waited on here.
        Overlay_Install();
    }

    // Fires on UE4SS's EVENT-LOOP thread. Nothing that touches the game may EVER live here: this
    // thread races the async loader and hangs level loads. All per-frame work runs in GameThreadFrame
    // (the engine-tick anchor) instead. Kept as an explicit empty override so the next reader finds
    // this warning instead of an inviting extension point.
    auto on_update() -> void override {}

    // Set the local pawn from UE4SS's own notification path when available; otherwise the per-frame
    // validity check keeps the last known one.
    auto set_own_pawn(void* p) -> void { g_ownPawn = p; }
};

#define OMP_MOD_API __declspec(dllexport)
extern "C" {
    OMP_MOD_API RC::CppUserModBase* start_mod()      { return new SessionOpenMP(); }
    OMP_MOD_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
