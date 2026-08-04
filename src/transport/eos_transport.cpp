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
// SessionOpenMP -- EOS P2P transport backend.
// Compiled and LINKED against the EOS 1.17.0 SDK with the MATCHED 1.17.0 runtime DLL shipped beside it.
// The headers and the runtime MUST match:
//   * `AddNotifyPeerConnectionEstablished` exists only from 1.17, so connection state can be OBSERVED
//     rather than inferred.
//   * `*_API_LATEST` is correct with a matched SDK. Under a runtime older than the headers, LATEST
//     structs are newer than the runtime understands and every call returns IncompatibleVersion.
//   * the send-after-close cooldown below is kept regardless of SDK version: it is correct behaviour,
//     not a workaround for one release.
#include "transport.h"
#include "eos_creds.h"
// Macro-only, no dependencies: the mod version is written in exactly one place and the lobby ad
// advertises that same string rather than keeping a second copy in step.
#include "../ui/version_tag.h"
#include <eos_sdk.h>
#include <eos_init.h>
#include <eos_version.h>
#include <eos_logging.h>
#include <eos_connect.h>
#include <eos_p2p.h>
#include <eos_lobby.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <windows.h>

namespace omp { namespace eosb {

static EOS_HPlatform        g_plat = nullptr;
static EOS_HConnect         g_connect = nullptr;
static EOS_HP2P             g_p2p = nullptr;
static EOS_ProductUserId    g_me = nullptr;
static char                 g_myId[64] = {0};
static EOS_P2P_SocketId     g_sock{};
static bool                 g_loginDone = false, g_loginOk = false;
// The route policy the platform was last TOLD (not what any one connection negotiated). Mirrors the
// SDK's own default so a process that never calls SetRelayControl still reports the truth.
static bool                 g_relaysForced = false;

struct Peer {
    EOS_ProductUserId puid = nullptr;
    char              idStr[64] = {0};
    PeerStats         st{};
    uint64_t          lastRecvMs = 0;
    uint64_t          closedUntilMs = 0;   // send cooldown after a remote close
    // ---- PROVENANCE. How we came to know this peer, which is a different question from whether they
    // are reachable. Set ONLY by the two roster paths -- rosterAddAll at join and the LOBBY_MEMBER
    // JOINED notification -- so it means "EOS says this human is in our session", never "this human
    // sent us a packet". A peer learned from an inbound packet stays false until a roster event
    // confirms them, which is the distinction the admission rules are built on.
    // Deliberately NOT cleared on DEPARTED: they really were a member, and the state byte already
    // carries "not any more". Clearing it would let a departed peer re-enter as a fresh stranger.
    bool              rosterSeen = false;
};
static Peer g_peers[16];   // sized for the 16-member lobby (15 remotes + headroom)
static int  g_nPeers = 0;

// ---- lobby state. One bucket for the whole mod: search-by-bucket IS the server browser for now.
static const char*    kBucket = "openmp:1";
static EOS_HLobby     g_lobbyH = nullptr;
static EOS_HLobbySearch g_search = nullptr;
static volatile LONG  g_lobbyStatus = 0;    // 0 idle, 1 busy, 2 hosting, 3 joined, -1 failed
static char           g_lobbyId[256] = {0};
// ---- private-game codes. Declared here, with the other lobby state, because onCreateLobby reads
// them and it is defined long before the browser section.
static char  g_adCode[16] = {0};   // what WE advertise while hosting ("" = public)
static char  g_myCode[16] = {0};   // the code of the lobby we are actually IN
static char  g_joinCode[16] = {0}; // the code the pending search is looking for

// Log() lives in transport.cpp (the router): both backends share one log file.

// "Are we deliberately in a session?" -- hosting, joined, or an op in flight. Declared up here because
// the P2P callbacks below gate on it and they are defined long before the public SessionOpen().
// -1 (last op FAILED) is deliberately NOT open: a failed join is not a session, and treating it as one
// is exactly how an idle instance ends up adopting strangers.
static bool SessionOpenLocal() { const LONG s = g_lobbyStatus; return s > 0; }
// Tools-only escape hatch for the learn gate -- see transport.h. Never set by the mod.
static bool g_learnWithoutSession = false;
void AllowLearnWithoutSession(bool on) {
    g_learnWithoutSession = on;
    if (on) Log("[p2p] learn-without-session ENABLED (tool mode: any sender may become a peer)");
}

void SetRelayControl(bool force);            // defined with the public seam; Init calls it
// Auth-expiry recovery, defined with the login chain. Declared up here because the LOBBY callbacks
// above route their results through it -- see the AUTH LIFETIME note.
static bool NoteAuthResult(EOS_EResult r, const char* what);

static Peer* peerByPuid(EOS_ProductUserId p) {
    for (int i = 0; i < g_nPeers; i++) if (g_peers[i].puid == p) return &g_peers[i];
    return nullptr;
}
// ESTABLISHED can arrive BEFORE any Peer entry exists (their connect lands first), and dropping the
// event leaves the state stuck on "connecting" while packets flow. So learn on the event.
static int addPeerEx(const char* puidStr, bool fromRoster);
static Peer* peerByPuidOrLearn(EOS_ProductUserId p) {
    Peer* pe = peerByPuid(p);
    if (pe) return pe;
    char id[64] = {0}; int32_t n = sizeof(id);
    if (EOS_ProductUserId_ToString(p, id, &n) != EOS_EResult::EOS_Success) return nullptr;
    // A connection-state notification is NOT a roster event: EOS is telling us a link changed, not
    // that this human belongs in our session. Unvouched until the roster says otherwise.
    const int i = addPeerEx(id, false);
    return (i >= 0) ? &g_peers[i] : nullptr;
}

// ---- connection-state notifications. Established? Direct or relayed? Why did it die? All of it
// arrives as a callback here instead of having to be inferred from traffic.
static void EOS_CALL onEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo* i) {
    Peer* p = peerByPuidOrLearn(i->RemoteUserId);
    const bool relay = (i->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection);
    if (p) p->st.state = relay ? 2 : 1;
    Log("[p2p] ESTABLISHED %s (%s, %s)", p ? p->idStr : "?",
        i->ConnectionType == EOS_EConnectionEstablishedType::EOS_CET_Reconnection ? "reconnect" : "new",
        relay ? "RELAYED" : "DIRECT");
}
static void EOS_CALL onInterrupted(const EOS_P2P_OnPeerConnectionInterruptedInfo* i) {
    Peer* p = peerByPuidOrLearn(i->RemoteUserId);
    if (p) p->st.state = 3;
    Log("[p2p] INTERRUPTED %s -- the SDK is attempting recovery", p ? p->idStr : "?");
}
static void EOS_CALL onClosed(const EOS_P2P_OnRemoteConnectionClosedInfo* i) {
    Peer* p = peerByPuidOrLearn(i->RemoteUserId);
    if (p) { p->st.state = 4; p->closedUntilMs = GetTickCount64() + 4000; }
    Log("[p2p] CLOSED %s reason=%d -- sends muted 4s", p ? p->idStr : "?", (int)i->Reason);
}
static void EOS_CALL onConnRequest(const EOS_P2P_OnIncomingConnectionRequestInfo* i) {
    char id[64] = {0}; int32_t n = sizeof(id);
    EOS_ProductUserId_ToString(i->RemoteUserId, id, &n);
    // Gate on being deliberately in a session, or an instance that never hosted or joined opens a
    // channel to anyone who asks. NOT on the roster -- a connection request legitimately PRECEDES the
    // roster during a join, so a membership test here would refuse the very peer we are joining.
    if (!SessionOpenLocal() && !g_learnWithoutSession) {
        Log("[p2p] incoming connection from %s -- IGNORED (not in a session)", id);
        return;
    }
    EOS_P2P_AcceptConnectionOptions a{};
    a.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
    a.LocalUserId = g_me; a.RemoteUserId = i->RemoteUserId; a.SocketId = &g_sock;
    EOS_P2P_AcceptConnection(g_p2p, &a);
    Log("[p2p] incoming connection from %s -- accepted", id);
}

// =====================================================================================================
// LOBBY -- the roster is the peer list. All callbacks fire from whichever thread ticks the platform,
// which after bring-up is the game thread only.
// =====================================================================================================
static void publishAd();     // defined with the browser below; called the moment a lobby exists
// HOST MIGRATION. Ownership is a property of the lobby, not a memory of who created it: we call
// LeaveLobby (never DestroyLobby), so when the host walks away EOS hands the lobby to somebody else
// and everyone stays in it. Nothing here may infer "am I the host" from having hosted, or the promoted
// player silently keeps a guest's powers and the lobby keeps advertising the person who left.
static void refreshOwnership();

static void rosterAddAll(const char* lobbyId) {
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{};
    o.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
    o.LobbyId = lobbyId; o.LocalUserId = g_me;
    EOS_HLobbyDetails det = nullptr;
    if (EOS_Lobby_CopyLobbyDetailsHandle(g_lobbyH, &o, &det) != EOS_EResult::EOS_Success || !det) return;
    EOS_LobbyDetails_GetMemberCountOptions mc{};
    mc.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
    const uint32_t n = EOS_LobbyDetails_GetMemberCount(det, &mc);
    for (uint32_t i = 0; i < n; i++) {
        EOS_LobbyDetails_GetMemberByIndexOptions mo{};
        mo.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST; mo.MemberIndex = i;
        EOS_ProductUserId m = EOS_LobbyDetails_GetMemberByIndex(det, &mo);
        if (!m) continue;
        char id[64] = {0}; int32_t idn = sizeof(id);
        if (EOS_ProductUserId_ToString(m, id, &idn) != EOS_EResult::EOS_Success) continue;
        if (!_stricmp(id, g_myId)) continue;             // handles are not guaranteed canonical; compare strings
        const int pi = addPeerEx(id, true);              // straight from the lobby: vouched
        if (pi >= 0 && g_peers[pi].st.state == 5) g_peers[pi].st.state = 0;   // rejoiner: un-depart
    }
    EOS_LobbyDetails_Release(det);
}

static void EOS_CALL onMemberStatus(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* d) {
    char who[64] = {0}; int32_t n = sizeof(who);
    EOS_ProductUserId_ToString(d->TargetUserId, who, &n);
    const bool isMe = !_stricmp(who, g_myId);
    switch (d->CurrentStatus) {
    case EOS_ELobbyMemberStatus::EOS_LMS_JOINED:
        if (!isMe) {
            const int i = addPeerEx(who, true);          // the lobby vouching for them, by definition
            if (i >= 0 && g_peers[i].st.state == 5) g_peers[i].st.state = 0;
            Log("[lobby] member JOINED: %s", who);
        }
        break;
    case EOS_ELobbyMemberStatus::EOS_LMS_LEFT:
    case EOS_ELobbyMemberStatus::EOS_LMS_DISCONNECTED:
    case EOS_ELobbyMemberStatus::EOS_LMS_KICKED:
        if (isMe) { InterlockedExchange(&g_lobbyStatus, 0); g_lobbyId[0] = 0; Log("[lobby] we are OUT (status %d)", (int)d->CurrentStatus); }
        else for (int i = 0; i < g_nPeers; i++)
            if (!_stricmp(g_peers[i].idStr, who)) { g_peers[i].st.state = 5; Log("[lobby] member DEPARTED: %s", who); }
        break;
    case EOS_ELobbyMemberStatus::EOS_LMS_CLOSED:
        Log("[lobby] lobby CLOSED");
        InterlockedExchange(&g_lobbyStatus, 0); g_lobbyId[0] = 0;
        for (int i = 0; i < g_nPeers; i++) g_peers[i].st.state = 5;
        break;
    case EOS_ELobbyMemberStatus::EOS_LMS_PROMOTED:
        // The roster is unchanged -- who can DO things is not. Handled by refreshOwnership below,
        // which asks EOS rather than trusting this event to be about us.
        Log("[lobby] %s was promoted to host", isMe ? "we" : who);
        break;
    default: break;
    }
    // Any membership change can move ownership (the owner leaving is the whole point, but a kick or a
    // disconnect does it too). Event-driven -- one lobby-details copy per change, never a poll.
    refreshOwnership();
}

static void EOS_CALL onCreateLobby(const EOS_Lobby_CreateLobbyCallbackInfo* d) {
    if (d->ResultCode == EOS_EResult::EOS_Success) {
        strncpy_s(g_lobbyId, d->LobbyId, _TRUNCATE);
        InterlockedExchange(&g_lobbyStatus, 2);
        strncpy_s(g_myCode, g_adCode, _TRUNCATE);
        Log("[lobby] HOSTING lobby %s (bucket %s)%s%s", g_lobbyId, kBucket,
            g_myCode[0] ? " PRIVATE, code " : " (public)", g_myCode);
        publishAd();          // who/where -- CreateLobby carries no attributes of its own
    } else {
        InterlockedExchange(&g_lobbyStatus, -1);
        Log("[lobby] create failed: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "create");
    }
}
bool LobbyHost() {
    if (!g_plat || !g_me || g_lobbyStatus == 1 || g_lobbyStatus >= 2) return false;
    InterlockedExchange(&g_lobbyStatus, 1);
    EOS_Lobby_CreateLobbyOptions o{};
    o.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    o.LocalUserId = g_me;
    o.MaxLobbyMembers = 16;   // matches g_peers[16] + the session's 16 slots. The practical ceiling is
                              // proxy CPU (each remote = a full anim graph), not this number.
    o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
    o.bPresenceEnabled = EOS_FALSE;
    o.bAllowInvites = EOS_TRUE;
    o.BucketId = kBucket;
    o.bDisableHostMigration = EOS_FALSE;   // overlay has no in-game host; the lobby outlives its creator
    o.bEnableRTCRoom = EOS_FALSE;
    EOS_Lobby_CreateLobby(g_lobbyH, &o, nullptr, onCreateLobby);
    return true;
}

static void EOS_CALL onJoinLobby(const EOS_Lobby_JoinLobbyCallbackInfo* d) {
    if (d->ResultCode == EOS_EResult::EOS_Success) {
        strncpy_s(g_lobbyId, d->LobbyId, _TRUNCATE);
        InterlockedExchange(&g_lobbyStatus, 3);
        Log("[lobby] JOINED lobby %s", g_lobbyId);
        rosterAddAll(g_lobbyId);                         // everyone already inside becomes a peer NOW
    } else {
        InterlockedExchange(&g_lobbyStatus, -1);
        Log("[lobby] join failed: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "join");
    }
}
// ---- the browser ---------------------------------------------------------------------------------
// The attribute keys we advertise. Short and fixed: EOS indexes them, and a peer running an older
// build simply reports blanks rather than failing to appear.
static const char* kAttrHost = "OMPHOST";
static const char* kAttrMap  = "OMPMAP";
static const char* kAttrCode = "OMPCODE";   // present = a PRIVATE game; the browser skips these
// The host's mod version, so the browser can warn about a mismatch BEFORE anyone joins. Version skew
// is otherwise invisible: the lobby and the P2P connection both succeed regardless, and only the
// snapshots fail to parse, so the symptom is a peer who is simply never there. A build too old to
// advertise this reports blank, which the browser shows as "unknown" rather than as a mismatch.
static const char* kAttrVer  = "OMPVER";

static char  g_adHost[40] = {0};
static char  g_adMap [40] = {0};
static LobbyInfo g_browse[16];
static int   g_nBrowse = 0;
static volatile LONG g_browseState = 0;     // 0 idle, 1 searching, 2 ready, -1 failed
static uint64_t nowMs() { return GetTickCount64(); }
// ---- ASYNCHRONOUS SIGN-IN --------------------------------------------------------------------------
// NOTHING here may block, and no SDK call may leave the game thread. Session ships its OWN EOS
// integration (Engine/Plugins/Online/EOSShared), so `EOSSDK-Win64-Shipping.dll` is ALREADY loaded when
// this runs -- EOS_Initialize returning EOS_AlreadyConfigured is the proof, and Windows binds our
// import to the module already in the process. It is ONE SDK, shared with the game, and the game ticks
// it from FEOSSDKManager::Tick on the GAME thread every frame. A second thread busy-ticking the
// platform (which is what a blocking sign-in needs) puts two threads inside that one SDK and corrupts
// it under the game's own tick -- an intermittent access violation inside FEOSSDKManager::Tick.
// So sign-in is STARTED here and POLLED from Tick, and there is no transport thread at all.
static volatile LONG g_initState = 0;      // 0 idle, 1 signing in, 2 ready, 3 failed
static uint64_t      g_initDeadlineMs = 0;
static const uint64_t kLoginTimeoutMs = 15000;
static const uint64_t kBrowseTimeoutMs = 15000;   // generous: a cold search over a relay is slow
static uint32_t      g_searchGen   = 0;     // bumped by every startSearch; rides in the Find ClientData
static uint64_t      g_browseStartMs = 0;   // when the live browse began (0 = none) -- for the timeout
static char          g_lobbyOwner[64] = {0};  // the lobby owner's PUID, refreshed with ownership

void SetLobbyAd(const char* hostName, const char* mapName) {
    if (hostName) strncpy_s(g_adHost, hostName, _TRUNCATE);
    if (mapName)  strncpy_s(g_adMap,  mapName,  _TRUNCATE);
    // If we are ALREADY hosting, re-publish. The map name is not knowable at the moment a session
    // starts: our own pawn (and therefore the world) is only discovered once the session is armed,
    // which is a few hundred milliseconds AFTER the lobby exists. The first advertisement necessarily
    // goes out with an empty map, and this is what corrects it.
    if (g_lobbyId[0]) publishAd();
}
void SetLobbyCode(const char* code) { strncpy_s(g_adCode, (code && *code) ? code : "", _TRUNCATE); }
const char* LobbyCode() { return g_myCode; }
// Unambiguous alphabet: no O/0, no I/1/L -- the code gets read aloud and typed by hand, and a
// character people cannot tell apart is a support ticket.
const char* MakeLobbyCode(char* out, int cap) {
    static const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    static const int  kN = (int)sizeof(kAlpha) - 1;
    if (!out || cap < 7) return "";
    uint64_t x = (uint64_t)GetTickCount64() * 6364136223846793005ull
               + (uint64_t)GetCurrentProcessId() * 1442695040888963407ull
               + (uint64_t)(uintptr_t)&x;
    for (int i = 0; i < 6; i++) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        out[i] = kAlpha[(x >> 17) % kN];
    }
    out[6] = 0;
    return out;
}
int  BrowseStatus() { return (int)g_browseState; }
int  BrowseCount()  { return g_nBrowse; }
bool BrowseAt(int i, LobbyInfo* out) {
    if (i < 0 || i >= g_nBrowse || !out) return false;
    *out = g_browse[i];
    return true;
}

// Pull one string attribute out of a search result. A lobby hosted by an older build has no such
// attribute; that is a blank field in the browser, never an error.
static void readAttr(EOS_HLobbyDetails det, const char* key, char* out, int cap) {
    out[0] = 0;
    EOS_LobbyDetails_CopyAttributeByKeyOptions o{};
    o.ApiVersion = EOS_LOBBYDETAILS_COPYATTRIBUTEBYKEY_API_LATEST;
    o.AttrKey = key;
    EOS_Lobby_Attribute* a = nullptr;
    if (EOS_LobbyDetails_CopyAttributeByKey(det, &o, &a) != EOS_EResult::EOS_Success || !a) return;
    if (a->Data && a->Data->ValueType == EOS_ELobbyAttributeType::EOS_AT_STRING && a->Data->Value.AsUtf8)
        strncpy_s(out, cap, a->Data->Value.AsUtf8, _TRUNCATE);
    EOS_Lobby_Attribute_Release(a);
}

// Ask EOS who owns the lobby we are in, and move our own status to match. This is the ONLY thing that
// decides whether we are the host after the lobby has been created: ownership migrates, so a remembered
// "I created it" is a claim that expires. Status 2 = we own it, 3 = we are a guest -- the in-flight
// values (1) and the terminal ones (0, -1) are left alone, because an operation half-done is not a
// membership question.
static void refreshOwnership() {
    const LONG st = g_lobbyStatus;
    if ((st != 2 && st != 3) || !g_lobbyH || !g_me || !g_lobbyId[0]) return;
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{};
    o.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
    o.LobbyId = g_lobbyId; o.LocalUserId = g_me;
    EOS_HLobbyDetails det = nullptr;
    if (EOS_Lobby_CopyLobbyDetailsHandle(g_lobbyH, &o, &det) != EOS_EResult::EOS_Success || !det) return;
    EOS_LobbyDetails_GetLobbyOwnerOptions oo{};
    oo.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
    EOS_ProductUserId owner = EOS_LobbyDetails_GetLobbyOwner(det, &oo);
    char id[64] = {0}; int32_t n = sizeof(id);
    const bool mine = owner && EOS_ProductUserId_ToString(owner, id, &n) == EOS_EResult::EOS_Success
                            && !_stricmp(id, g_myId);
    strncpy_s(g_lobbyOwner, id, _TRUNCATE);          // "" if it could not be resolved -- say so, do not guess
    if (mine && st != 2) {
        // We just inherited the lobby. Take the join CODE with it, or a private game silently becomes
        // one nobody -- including us -- can tell anyone how to enter. The advertised name and map are
        // still the departed host's; the loader notices the change of host and re-publishes ours.
        readAttr(det, kAttrCode, g_myCode, sizeof(g_myCode));
        strncpy_s(g_adCode, g_myCode, _TRUNCATE);
        InterlockedExchange(&g_lobbyStatus, 2);
        Log("[lobby] WE ARE NOW THE HOST of %s%s%s", g_lobbyId,
            g_myCode[0] ? " -- join code carried over: " : "", g_myCode);
    } else if (!mine && st != 3) {
        InterlockedExchange(&g_lobbyStatus, 3);
        Log("[lobby] host is now %s", id[0] ? id : "someone else");
    }
    EOS_LobbyDetails_Release(det);
}

// Publish who/where onto the lobby we own. Runs after create (and again on demand), because
// EOS_Lobby_CreateLobby has no attribute list of its own.
static void EOS_CALL onUpdateLobby(const EOS_Lobby_UpdateLobbyCallbackInfo* d) {
    if (d->ResultCode == EOS_EResult::EOS_Success) return;
    Log("[lobby] attribute update failed: %s", EOS_EResult_ToString(d->ResultCode));
    // THE SERVICE IS THE AUTHORITY ON OWNERSHIP, not our copy of the lobby details.
    // `EOS_LobbyDetails_GetLobbyOwner` reads the LOCAL cache, which can move the owner to us when the
    // previous host drops even though the backend never does -- so migration can announce a promotion
    // that did not happen and show the player host powers they do not have. A refusal here is the only
    // truthful signal available, so take it and demote to guest. Same shape as the InvalidAuth
    // handling below: believe the call that failed, not the state that predicted it would succeed.
    if (d->ResultCode == EOS_EResult::EOS_Lobby_NotOwner && g_lobbyStatus == 2) {
        InterlockedExchange(&g_lobbyStatus, 3);
        Log("[lobby] ...so we are NOT the host after all -- back to guest");
    }
    NoteAuthResult(d->ResultCode, "attribute update");
}
static void publishAd() {
    if (!g_lobbyH || !g_me || !g_lobbyId[0]) return;
    EOS_Lobby_UpdateLobbyModificationOptions mo{};
    mo.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
    mo.LocalUserId = g_me; mo.LobbyId = g_lobbyId;
    EOS_HLobbyModification mod = nullptr;
    if (EOS_Lobby_UpdateLobbyModification(g_lobbyH, &mo, &mod) != EOS_EResult::EOS_Success || !mod) return;
    auto add = [&](const char* key, const char* val) {
        if (!val || !*val) return;
        EOS_Lobby_AttributeData ad{};
        ad.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        ad.Key = key; ad.Value.AsUtf8 = val;
        ad.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbyModification_AddAttributeOptions ao{};
        ao.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
        ao.Attribute = &ad;
        // PUBLIC or the browser cannot see it: a private attribute is only visible to MEMBERS, which
        // is exactly who does not need to browse.
        ao.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
        EOS_LobbyModification_AddAttribute(mod, &ao);
    };
    add(kAttrHost, g_adHost);
    add(kAttrMap,  g_adMap);
    add(kAttrCode, g_adCode);          // absent for a public game -- `add` skips empty values
    add(kAttrVer,  OMP_VERSION_STRING);
    EOS_Lobby_UpdateLobbyOptions uo{};
    uo.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
    uo.LobbyModificationHandle = mod;
    EOS_Lobby_UpdateLobby(g_lobbyH, &uo, nullptr, onUpdateLobby);
    EOS_LobbyModification_Release(mod);
    Log("[lobby] advertising host='%s' map='%s'", g_adHost, g_adMap);
}

static void EOS_CALL onFind(const EOS_LobbySearch_FindCallbackInfo* d) {
    // WHOSE SEARCH IS THIS? `startSearch` RELEASES the previous handle, so a second search started
    // while the first is still in flight orphans it and fewer callbacks arrive than searches started.
    // Two ways a shared "am I browsing?" flag strands the browser at "Searching..." forever:
    //   * a superseded browse's callback arrives and is read against the CURRENT value of the flag
    //   * a JOIN search (browse=false) starting behind a live browse, so when the browse completes it
    //     takes the join branch and never moves `g_browseState` off 1
    // The generation rides in ClientData, so every callback knows which search it belongs to and
    // whether THAT search was a browse -- no shared flag to race.
    const uintptr_t tag   = (uintptr_t)d->ClientData;
    const uint32_t  gen   = (uint32_t)(tag >> 1);
    const bool      wasBrowse = (tag & 1) != 0;
    if (gen != g_searchGen) {
        Log("[lobby] ignoring a superseded %s result", wasBrowse ? "browse" : "search");
        return;                     // its handle is gone; the search that replaced it owns the state
    }
    // Joining BY CODE: the search was filtered to that one code, so the only result is the lobby we
    // want. No list, no index -- take the hit or report that the code is not live.
    if (g_joinCode[0]) {
        char want[16]; strncpy_s(want, g_joinCode, _TRUNCATE);
        g_joinCode[0] = 0;
        uint32_t n = 0;
        if (d->ResultCode == EOS_EResult::EOS_Success) {
            EOS_LobbySearch_GetSearchResultCountOptions co{};
            co.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
            n = EOS_LobbySearch_GetSearchResultCount(g_search, &co);
        }
        if (!n) {
            InterlockedExchange(&g_lobbyStatus, -1);
            Log("[lobby] code %s: no such game (wrong code, or the host closed it)", want);
            return;
        }
        EOS_LobbySearch_CopySearchResultByIndexOptions ro{};
        ro.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST; ro.LobbyIndex = 0;
        EOS_HLobbyDetails det = nullptr;
        if (EOS_LobbySearch_CopySearchResultByIndex(g_search, &ro, &det) != EOS_EResult::EOS_Success || !det) {
            InterlockedExchange(&g_lobbyStatus, -1);
            return;
        }
        strncpy_s(g_myCode, want, _TRUNCATE);      // we are joining a private game; remember its code
        Log("[lobby] code %s matched -- joining", want);
        EOS_Lobby_JoinLobbyOptions jo{};
        jo.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
        jo.LobbyDetailsHandle = det; jo.LocalUserId = g_me;
        jo.bPresenceEnabled = EOS_FALSE;
        EOS_Lobby_JoinLobby(g_lobbyH, &jo, nullptr, onJoinLobby);
        EOS_LobbyDetails_Release(det);
        return;
    }
    if (wasBrowse) {
        g_nBrowse = 0;
        if (d->ResultCode != EOS_EResult::EOS_Success) {
            InterlockedExchange(&g_browseState, -1);
            g_browseStartMs = 0;
            Log("[lobby] browse failed: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "browse");
            return;
        }
        EOS_LobbySearch_GetSearchResultCountOptions co{};
        co.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
        const uint32_t n = EOS_LobbySearch_GetSearchResultCount(g_search, &co);
        for (uint32_t i = 0; i < n && g_nBrowse < 16; i++) {
            EOS_LobbySearch_CopySearchResultByIndexOptions ro{};
            ro.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST; ro.LobbyIndex = i;
            EOS_HLobbyDetails det = nullptr;
            if (EOS_LobbySearch_CopySearchResultByIndex(g_search, &ro, &det) != EOS_EResult::EOS_Success || !det)
                continue;
            LobbyInfo& L = g_browse[g_nBrowse];
            L = LobbyInfo{};
            EOS_LobbyDetails_GetMemberCountOptions mc{};
            mc.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
            L.players = (int)EOS_LobbyDetails_GetMemberCount(det, &mc);
            EOS_LobbyDetails_CopyInfoOptions io{};
            io.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;
            EOS_LobbyDetails_Info* info = nullptr;
            if (EOS_LobbyDetails_CopyInfo(det, &io, &info) == EOS_EResult::EOS_Success && info) {
                L.maxPlayers = (int)info->MaxMembers;
                if (info->LobbyId) strncpy_s(L.id, info->LobbyId, _TRUNCATE);
                EOS_LobbyDetails_Info_Release(info);
            }
            readAttr(det, kAttrHost, L.host, sizeof(L.host));
            readAttr(det, kAttrMap,  L.map,  sizeof(L.map));
            readAttr(det, kAttrVer,  L.version, sizeof(L.version));
            char code[16] = {0};
            readAttr(det, kAttrCode, code, sizeof(code));
            EOS_LobbyDetails_Release(det);
            // A lobby with a code is PRIVATE: it must not appear in the public list, or the code
            // would protect nothing. (It is still publicly advertised -- that is what lets a search
            // by code find it at all -- so the filter has to be here, on the browsing side.)
            if (code[0]) continue;
            Log("[lobby] browse[%d]: '%s' on '%s' %d/%d v%s%s", g_nBrowse,
                L.host[0] ? L.host : "?", L.map[0] ? L.map : "?", L.players, L.maxPlayers,
                L.version[0] ? L.version : "?",
                (L.version[0] && strcmp(L.version, OMP_VERSION_STRING) != 0) ? " *** DIFFERENT VERSION" : "");
            g_nBrowse++;
        }
        InterlockedExchange(&g_browseState, 2);
        g_browseStartMs = 0;
        Log("[lobby] browse: %d lobby(ies) in bucket %s", g_nBrowse, kBucket);
        return;
    }
    if (d->ResultCode != EOS_EResult::EOS_Success) {
        InterlockedExchange(&g_lobbyStatus, -1);
        Log("[lobby] search failed: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "search");
        return;
    }
    EOS_LobbySearch_GetSearchResultCountOptions co{};
    co.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
    const uint32_t n = EOS_LobbySearch_GetSearchResultCount(g_search, &co);
    if (!n) {
        InterlockedExchange(&g_lobbyStatus, -1);
        Log("[lobby] search: no lobbies in bucket %s -- is anyone hosting?", kBucket);
        return;
    }
    EOS_LobbySearch_CopySearchResultByIndexOptions ro{};
    ro.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST; ro.LobbyIndex = 0;
    EOS_HLobbyDetails det = nullptr;
    if (EOS_LobbySearch_CopySearchResultByIndex(g_search, &ro, &det) != EOS_EResult::EOS_Success || !det) {
        InterlockedExchange(&g_lobbyStatus, -1);
        return;
    }
    Log("[lobby] search: %u lobby(ies) in bucket %s; joining the first", n, kBucket);
    EOS_Lobby_JoinLobbyOptions jo{};
    jo.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
    jo.LobbyDetailsHandle = det; jo.LocalUserId = g_me;
    jo.bPresenceEnabled = EOS_FALSE;
    EOS_Lobby_JoinLobby(g_lobbyH, &jo, nullptr, onJoinLobby);
    EOS_LobbyDetails_Release(det);
}
// One search builder for both callers -- the ONLY difference is what onFind does with the results.
static bool startSearch(bool browse, const char* code = nullptr) {
    if (g_search) { EOS_LobbySearch_Release(g_search); g_search = nullptr; }
    EOS_Lobby_CreateLobbySearchOptions so{};
    so.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST; so.MaxResults = 10;
    if (EOS_Lobby_CreateLobbySearch(g_lobbyH, &so, &g_search) != EOS_EResult::EOS_Success) {
        InterlockedExchange(&g_lobbyStatus, -1);
        return false;
    }
    EOS_Lobby_AttributeData ad{};
    ad.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    ad.Key = EOS_LOBBY_SEARCH_BUCKET_ID; ad.Value.AsUtf8 = kBucket;
    ad.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
    EOS_LobbySearch_SetParameterOptions sp{};
    sp.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    sp.Parameter = &ad; sp.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
    EOS_LobbySearch_SetParameter(g_search, &sp);
    if (code && *code) {
        // A SECOND parameter ANDs with the bucket: same mod, that one code.
        EOS_Lobby_AttributeData cd{};
        cd.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        cd.Key = kAttrCode; cd.Value.AsUtf8 = code;
        cd.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
        EOS_LobbySearch_SetParameterOptions cp{};
        cp.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
        cp.Parameter = &cd; cp.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
        EOS_LobbySearch_SetParameter(g_search, &cp);
    }
    EOS_LobbySearch_FindOptions fo{};
    fo.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST; fo.LocalUserId = g_me;
    // Tag the callback with this search's identity: generation, plus whether IT was a browse. See the
    // header of onFind for what goes wrong when a callback has to consult a shared flag instead.
    const uintptr_t tag = ((uintptr_t)(++g_searchGen) << 1) | (browse ? 1u : 0u);
    EOS_LobbySearch_Find(g_search, &fo, (void*)tag, onFind);
    Log("[lobby] %s bucket %s ...", browse ? "browsing" : "searching", kBucket);
    return true;
}
bool LobbyJoin() {
    if (!g_plat || !g_me || g_lobbyStatus == 1 || g_lobbyStatus >= 2) return false;
    InterlockedExchange(&g_lobbyStatus, 1);
    return startSearch(false);
}
// Browsing is deliberately NOT gated on g_lobbyStatus: looking at the list is not a lobby operation,
// and a player already hosting may well want to see who else is out there.
bool LobbyJoinByCode(const char* code) {
    if (!g_plat || !g_me || !code || !*code) return false;
    if (g_lobbyStatus == 1 || g_lobbyStatus >= 2) return false;
    strncpy_s(g_joinCode, code, _TRUNCATE);
    InterlockedExchange(&g_lobbyStatus, 1);
    if (startSearch(false, g_joinCode)) return true;
    g_joinCode[0] = 0;
    InterlockedExchange(&g_lobbyStatus, -1);
    return false;
}
bool LobbyBrowse() {
    // A refusal must SAY SO -- it has to move the state, not just return false. Otherwise a Refresh
    // pressed before EOS is ready leaves the browser reading whatever it read before, typically
    // "Searching...", forever, with nothing in flight to ever finish. A UI must always be able to
    // distinguish "working on it" from "never started".
    if (!g_plat || !g_me) {
        InterlockedExchange(&g_browseState, -1);
        Log("[lobby] browse refused: not signed in yet");
        return false;
    }
    InterlockedExchange(&g_browseState, 1);
    g_browseStartMs = nowMs();
    if (startSearch(true)) return true;
    InterlockedExchange(&g_browseState, -1);
    g_browseStartMs = 0;
    return false;
}
// Joins through the SEARCH RESULT's details handle, which is why the search handle is kept alive: an
// EOS lobby cannot be joined from its id string. A browse that has been superseded therefore
// invalidates every index -- callers must re-browse rather than hold an index across refreshes.
bool LobbyJoinAt(int i) {
    if (!g_plat || !g_me || !g_search) return false;
    if (i < 0 || i >= g_nBrowse) return false;
    if (g_lobbyStatus == 1 || g_lobbyStatus >= 2) return false;
    EOS_LobbySearch_CopySearchResultByIndexOptions ro{};
    ro.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST; ro.LobbyIndex = (uint32_t)i;
    EOS_HLobbyDetails det = nullptr;
    if (EOS_LobbySearch_CopySearchResultByIndex(g_search, &ro, &det) != EOS_EResult::EOS_Success || !det)
        return false;
    InterlockedExchange(&g_lobbyStatus, 1);
    Log("[lobby] joining browse[%d] '%s' on '%s'", i, g_browse[i].host, g_browse[i].map);
    EOS_Lobby_JoinLobbyOptions jo{};
    jo.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
    jo.LobbyDetailsHandle = det; jo.LocalUserId = g_me;
    jo.bPresenceEnabled = EOS_FALSE;
    EOS_Lobby_JoinLobby(g_lobbyH, &jo, nullptr, onJoinLobby);
    EOS_LobbyDetails_Release(det);
    return true;
}

static void EOS_CALL onLeaveLobby(const EOS_Lobby_LeaveLobbyCallbackInfo* d) {
    g_myCode[0] = 0;
    Log("[lobby] leave: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "leave");
}
void LobbyLeave() {
    if (!g_lobbyH || !g_lobbyId[0]) return;
    EOS_Lobby_LeaveLobbyOptions o{};
    o.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
    o.LobbyId = g_lobbyId; o.LocalUserId = g_me;
    EOS_Lobby_LeaveLobby(g_lobbyH, &o, nullptr, onLeaveLobby);
    InterlockedExchange(&g_lobbyStatus, 0); g_lobbyId[0] = 0; g_lobbyOwner[0] = 0;
    for (int i = 0; i < g_nPeers; i++) g_peers[i].st.state = 5;   // we left; nobody to send to
}
int LobbyStatus() { return (int)g_lobbyStatus; }

// ---- AUTH LIFETIME ---------------------------------------------------------------------------------
// AN EOS CONNECT TOKEN EXPIRES ABOUT AN HOUR AFTER LOGIN AND MUST BE RENEWED. Once it lapses EVERY
// authenticated call fails -- search, create, even leave -- and lobby NOTIFICATIONS stop arriving too,
// so the peer you were playing with vanishes without a DEPARTED line. It presents as "they
// disconnected and now search is broken", which is one fault wearing two costumes.
// An idempotence check must test whether the thing is still GOOD, not merely whether it happened: an
// `Init` guarded only on `g_plat && g_loginOk` resumes straight back onto a dead token, and no amount
// of leaving and rejoining -- or switching backends away and back -- ever recovers.
// Three layers, because each covers a hole the others do not:
//   1. PROACTIVE  -- EOS_Connect_AddNotifyAuthExpiration fires ~10 min before expiry; re-login there
//                    and a session that is going fine never notices.
//   2. REACTIVE   -- any lobby op that comes back EOS_InvalidAuth marks the token stale and kicks off
//                    a re-login. Covers a missed notification: a suspended or slept machine, a
//                    notification that arrived while the platform was not being ticked.
//   3. THE GATE   -- Init's resume path refuses to resume onto a stale token.
// The device id is stable, so a refresh returns the SAME product user id and peers still recognise us.
static volatile LONG g_authStale   = 0;   // set the moment we learn the token is no good
static volatile LONG g_authRefresh = 0;   // a refresh is in flight; do not stack them
static uint64_t      g_lastAuthTryMs = 0;

static void EOS_CALL onLogin(const EOS_Connect_LoginCallbackInfo* d);
static void loginWithDeviceId();

// Called from anywhere that discovers the token is dead. Safe to call repeatedly: throttled, and a
// no-op while a refresh is already running.
static void RefreshAuth(const char* why) {
    InterlockedExchange(&g_authStale, 1);
    if (!g_connect) return;
    if (InterlockedCompareExchange(&g_authRefresh, 1, 0) != 0) return;   // already refreshing
    const uint64_t now = GetTickCount64();
    if (g_lastAuthTryMs && now - g_lastAuthTryMs < 5000) { InterlockedExchange(&g_authRefresh, 0); return; }
    g_lastAuthTryMs = now;
    Log("[eos] auth refresh: %s -- signing in again", why);
    loginWithDeviceId();          // async; onLogin clears the flags
}
// Every lobby callback routes its result through here, so a single expiry cannot be noticed by one
// code path and missed by three others.
static bool NoteAuthResult(EOS_EResult r, const char* what) {
    if (r != EOS_EResult::EOS_InvalidAuth) return false;
    char m[120];
    snprintf(m, sizeof(m), "%s returned InvalidAuth", what);
    RefreshAuth(m);
    return true;
}

static void loginWithDeviceId() {
    EOS_Connect_Credentials cred{};
    cred.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
    cred.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
    EOS_Connect_UserLoginInfo info{};
    info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
    info.DisplayName = "OpenMP";
    EOS_Connect_LoginOptions lo{};
    lo.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
    lo.Credentials = &cred; lo.UserLoginInfo = &info;
    EOS_Connect_Login(g_connect, &lo, nullptr, onLogin);
}
static void EOS_CALL onCreateUser(const EOS_Connect_CreateUserCallbackInfo* d) {
    if (d->ResultCode == EOS_EResult::EOS_Success) { g_me = d->LocalUserId; g_loginOk = true; InterlockedExchange(&g_authStale, 0); }
    else Log("[eos] CreateUser failed: %s", EOS_EResult_ToString(d->ResultCode));
    g_loginDone = true;
    InterlockedExchange(&g_authRefresh, 0);
}
// EOS tells us BEFORE the token dies (~10 minutes' notice). Acting on this is what keeps a live
// session from ever seeing the failure at all.
static void EOS_CALL onAuthExpiring(const EOS_Connect_AuthExpirationCallbackInfo*) {
    RefreshAuth("token is about to expire");
}
static void EOS_CALL onLogin(const EOS_Connect_LoginCallbackInfo* d) {
    if (d->ResultCode == EOS_EResult::EOS_Success) {
        // A REFRESH must come back as the same person. The device id is stable so it always should --
        // but if it ever did not, every peer would suddenly be talking to a stranger, and that is
        // worth a loud line rather than a silent mystery.
        if (g_me && d->LocalUserId != g_me) {
            char old[64] = {0}; int32_t n = sizeof(old);
            EOS_ProductUserId_ToString(g_me, old, &n);
            Log("[eos] *** identity CHANGED on re-login (was %s) -- peers will not recognise us", old);
        }
        g_me = d->LocalUserId; g_loginOk = true; g_loginDone = true;
        if (InterlockedExchange(&g_authStale, 0)) {
            int32_t n = sizeof(g_myId);
            EOS_ProductUserId_ToString(g_me, g_myId, &n);
            Log("[eos] auth refreshed -- signed in again as %s", g_myId);
        }
        InterlockedExchange(&g_authRefresh, 0);
        return;
    }
    if (d->ResultCode == EOS_EResult::EOS_InvalidUser && d->ContinuanceToken) {
        EOS_Connect_CreateUserOptions co{};
        co.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
        co.ContinuanceToken = d->ContinuanceToken;
        EOS_Connect_CreateUser(g_connect, &co, nullptr, onCreateUser);
        return;
    }
    Log("[eos] login failed: %s", EOS_EResult_ToString(d->ResultCode));
    g_loginDone = true;
    // A failed REFRESH must leave the door open for the next attempt rather than latching shut.
    InterlockedExchange(&g_authRefresh, 0);
}
static void EOS_CALL onCreateDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo* d) {
    // DuplicateNotAllowed = the device id already exists, which is a normal, expected result.
    if (d->ResultCode != EOS_EResult::EOS_Success && d->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed)
        Log("[eos] CreateDeviceId: %s (continuing to login anyway)", EOS_EResult_ToString(d->ResultCode));
    loginWithDeviceId();
}

static bool g_notifiesOn = false;

bool Init(bool forceRelays) {
    // IDEMPOTENT, because backend switching re-enters here. A resident, logged-in platform is reused
    // as-is; a platform whose login never completed retries JUST the login. Full teardown
    // (EOS_Shutdown/Platform_Release) is process-exit only -- the SDK cannot re-initialize after it.
    // `g_loginOk` alone is NOT a valid resume condition -- it records that a login once succeeded, not
    // that the token is still good, so the staleness flag has to be part of the test.
    if (g_plat && g_loginOk && !g_authStale) {
        Log("=== transport: EOS resumed (resident platform, id %s) ===", g_myId);
        InterlockedExchange(&g_initState, 2);
        return true;
    }
    if (g_plat && g_authStale) {
        // Resident platform, dead token: re-login in place. This is the recovery path a player reaches
        // by leaving and going online again, so it must actually re-authenticate.
        Log("=== transport: EOS resuming with a STALE token -- signing in again ===");
        g_loginDone = false; g_loginOk = false;
        InterlockedExchange(&g_authRefresh, 1);
        g_lastAuthTryMs = GetTickCount64();
        loginWithDeviceId();
        InterlockedExchange(&g_initState, 1);
        g_initDeadlineMs = nowMs() + kLoginTimeoutMs;
        return true;                       // STARTED, not finished -- Tick resolves it
    }
    if (!g_plat) {
        Log("=== SessionOpenMP transport (EOS %s) ===", EOS_GetVersion());

        EOS_InitializeOptions io{};
        io.ApiVersion = EOS_INITIALIZE_API_LATEST;
        io.ProductName = "SessionOpenMP";
        io.ProductVersion = "0.1";
        EOS_EResult r = EOS_Initialize(&io);
        if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) {
            Log("[eos] EOS_Initialize failed: %s", EOS_EResult_ToString(r)); return false;
        }

        // A source build with no credentials of its own gets the placeholder header CMake generated
        // from eos_creds.h.template. Say so plainly here: EOS_Platform_Create would otherwise fail
        // with a generic error, or worse, succeed and strand the player at "signing in" forever. The
        // UDP and shared-memory backends need no credentials and still work.
        if (strstr(OMP_PRODUCT_ID, "PUT_YOUR_") != nullptr) {
            Log("[eos] no EOS credentials configured -- src/transport/eos_creds.h still holds the");
            Log("[eos] template placeholders. Fill in your own product/sandbox/deployment/client");
            Log("[eos] values from the Epic Dev Portal, or use the Direct connect (UDP) backend.");
            return false;
        }

        EOS_Platform_Options po{};
        po.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        po.ProductId = OMP_PRODUCT_ID; po.SandboxId = OMP_SANDBOX_ID; po.DeploymentId = OMP_DEPLOYMENT_ID;
        po.ClientCredentials.ClientId = OMP_CLIENT_ID; po.ClientCredentials.ClientSecret = OMP_CLIENT_SECRET;
        po.Flags = EOS_PF_DISABLE_OVERLAY;
        g_plat = EOS_Platform_Create(&po);
        if (!g_plat) { Log("[eos] Platform_Create failed"); return false; }

        g_connect = EOS_Platform_GetConnectInterface(g_plat);
        g_p2p     = EOS_Platform_GetP2PInterface(g_plat);
        g_lobbyH  = EOS_Platform_GetLobbyInterface(g_plat);
        memset(&g_sock, 0, sizeof(g_sock));
        g_sock.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        strcpy_s(g_sock.SocketName, "OPENMPP1");

    }
    // OUTSIDE the platform-creation branch on purpose. The EOS platform stays resident for the process
    // lifetime (the SDK cannot be re-initialised), so a route policy set only at creation could never
    // be changed afterwards. A resumed platform must be re-told.
    SetRelayControl(forceRelays);

    // Device-id login (kick off the chain; CreateDeviceId -> Login -> maybe CreateUser).
    g_loginDone = false; g_loginOk = false;
    EOS_Connect_CreateDeviceIdOptions dio{};
    dio.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
    dio.DeviceModel = "PC";
    EOS_Connect_CreateDeviceId(g_connect, &dio, nullptr, onCreateDeviceId);
    InterlockedExchange(&g_initState, 1);
    g_initDeadlineMs = nowMs() + kLoginTimeoutMs;

    return true;                       // STARTED. Tick resolves it -- see finishLogin.
}

// Called from Tick the moment sign-in resolves. Everything here needs `g_me`, which does not exist
// until then -- which is exactly why Init cannot do it any more.
static void finishLogin() {
    // Connection-state notifications -- the 1.17 payoff. Registered ONCE per process (a login retry
    // must not stack duplicate callbacks).
    if (g_notifiesOn) return;
    g_notifiesOn = true;
    {
        EOS_P2P_AddNotifyPeerConnectionRequestOptions o{};
        o.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
        o.LocalUserId = g_me; o.SocketId = &g_sock;
        EOS_P2P_AddNotifyPeerConnectionRequest(g_p2p, &o, nullptr, onConnRequest);
    }
    {
        EOS_P2P_AddNotifyPeerConnectionEstablishedOptions o{};
        o.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
        o.LocalUserId = g_me; o.SocketId = &g_sock;
        EOS_P2P_AddNotifyPeerConnectionEstablished(g_p2p, &o, nullptr, onEstablished);
    }
    {
        EOS_P2P_AddNotifyPeerConnectionInterruptedOptions o{};
        o.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONINTERRUPTED_API_LATEST;
        o.LocalUserId = g_me; o.SocketId = &g_sock;
        EOS_P2P_AddNotifyPeerConnectionInterrupted(g_p2p, &o, nullptr, onInterrupted);
    }
    {
        EOS_P2P_AddNotifyPeerConnectionClosedOptions o{};
        o.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
        o.LocalUserId = g_me; o.SocketId = &g_sock;
        EOS_P2P_AddNotifyPeerConnectionClosed(g_p2p, &o, nullptr, onClosed);
    }
    {
        // The proactive half of the token renewal -- see the AUTH LIFETIME note.
        EOS_Connect_AddNotifyAuthExpirationOptions o{};
        o.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
        EOS_Connect_AddNotifyAuthExpiration(g_connect, &o, nullptr, onAuthExpiring);
    }
    {   // lobby membership changes -> the peer list (fires for any lobby we are in)
        EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions o{};
        o.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
        EOS_Lobby_AddNotifyLobbyMemberStatusReceived(g_lobbyH, &o, nullptr, onMemberStatus);
    }
}

// The resolver itself. Runs on the GAME thread, from Tick, because that is the only thread allowed to
// touch this SDK (see the ASYNCHRONOUS SIGN-IN note).
static void pollLogin() {
    if (g_initState != 1) return;
    if (!g_loginDone) {
        if (nowMs() <= g_initDeadlineMs) return;
        InterlockedExchange(&g_authRefresh, 0);
        InterlockedExchange(&g_initState, 3);
        Log("[eos] sign-in timed out after %llums", (unsigned long long)kLoginTimeoutMs);
        return;
    }
    InterlockedExchange(&g_authRefresh, 0);
    if (!g_loginOk) {
        InterlockedExchange(&g_initState, 3);
        Log("[eos] login did not complete");
        return;
    }
    InterlockedExchange(&g_authStale, 0);
    int32_t n = sizeof(g_myId);
    EOS_ProductUserId_ToString(g_me, g_myId, &n);
    Log("[eos] logged in: %s", g_myId);
    finishLogin();
    InterlockedExchange(&g_initState, 2);
}

int InitState() { return (int)g_initState; }

void Shutdown() {
    if (g_plat) { EOS_Platform_Release(g_plat); g_plat = nullptr; }
    EOS_Shutdown();
}

// Stop being the active wire WITHOUT killing what cannot be rebuilt (see Init). Leaves the lobby and
// gives the async request a short burst of ticks so the other members actually see us go (best effort
// -- the lobby's own member timeout covers a missed leave). ~100 ms, user-initiated only.
void Deactivate() {
    if (!g_plat) return;
    LobbyLeave();
    for (int i = 0; i < 12; i++) { EOS_Platform_Tick(g_plat); Sleep(8); }
}

const char* MyId() { return g_myId; }
int PeerCount() { return g_nPeers; }
const char* PeerIdStr(int i) { return (i >= 0 && i < g_nPeers) ? g_peers[i].idStr : ""; }
bool LobbyIsHost() { return g_lobbyStatus == 2 && g_lobbyId[0] != 0; }
const char* LobbyOwnerId() { return g_lobbyId[0] ? g_lobbyOwner : ""; }

static void EOS_CALL onKickMember(const EOS_Lobby_KickMemberCallbackInfo* d) {
    Log("[lobby] kick: %s", EOS_EResult_ToString(d->ResultCode)); NoteAuthResult(d->ResultCode, "kick");
}
// HOSTING ONLY, and that is EOS's rule rather than ours: KickMemberOptions says the local user
// "must be the lobby owner". A member asking to remove another member is simply refused by the
// service, so there is nothing to be gained by trying.
bool LobbyKick(const char* peerId) {
    if (!g_plat || !g_me || !peerId || !*peerId) return false;
    if (!LobbyIsHost()) { Log("[lobby] kick refused: we are not the host of this session"); return false; }
    EOS_ProductUserId target = EOS_ProductUserId_FromString(peerId);
    if (!target) return false;
    EOS_Lobby_KickMemberOptions o{};
    o.ApiVersion = EOS_LOBBY_KICKMEMBER_API_LATEST;
    o.LobbyId = g_lobbyId; o.LocalUserId = g_me; o.TargetUserId = target;
    Log("[lobby] kicking %s", peerId);
    EOS_Lobby_KickMember(g_lobbyH, &o, nullptr, onKickMember);
    return true;
}

// ---- SLOT RESERVE. Peer indices are handed out once and NEVER reused (session slots are keyed by
// them, and reuse aliases two humans onto one proxy), so the table is a budget spent over the life of
// the process. That is fine for the people you invited and terrible against a stranger: 16 packets
// from 16 fresh product user ids would consume the whole table permanently, after which real players
// could never join. So the two provenances get different budgets. A roster event ("EOS says this human
// is in our lobby") may use every slot; a peer learned only because a packet arrived may use
// kMaxLearned, leaving the rest for people who are actually in the session. A refusal is never silent
// -- it says which budget ran out.
static const int kPeerCap    = 16;
static const int kRosterOnly = 4;                            // slots a packet-learned peer may not touch
static const int kMaxLearned = kPeerCap - kRosterOnly;
static uint64_t  g_lastFullLogMs = 0;

static int addPeerEx(const char* puidStr, bool fromRoster) {
    if (!puidStr || !*puidStr) return -1;
    for (int i = 0; i < g_nPeers; i++) {                     // the same human can arrive twice
        if (_stricmp(g_peers[i].idStr, puidStr)) continue;   // (auto-learn + paste) -- dedupe by id
        // A peer we first met through a packet is UPGRADED the moment the roster confirms them; that
        // is how the join-window learn becomes a vouched member without a second slot.
        if (fromRoster && !g_peers[i].rosterSeen) {
            g_peers[i].rosterSeen = true;
            Log("[p2p] peer #%d %s confirmed by the lobby roster", i, g_peers[i].idStr);
        }
        return i;
    }
    const int budget = fromRoster ? kPeerCap : kMaxLearned;
    if (g_nPeers >= budget) {
        // Rate-limited: a flood must not become a log flood, but the refusal must still be visible.
        const uint64_t now = GetTickCount64();
        if (now - g_lastFullLogMs > 5000) {
            g_lastFullLogMs = now;
            Log("[p2p] REFUSED peer '%s': %s budget full (%d/%d used%s)", puidStr,
                fromRoster ? "roster" : "unvouched", g_nPeers, budget,
                fromRoster ? "" : " -- reserve held for lobby members");
        }
        return -1;
    }

    EOS_ProductUserId p = EOS_ProductUserId_FromString(puidStr);
    if (!p || EOS_ProductUserId_IsValid(p) != EOS_TRUE) { Log("[p2p] invalid peer id '%s'", puidStr); return -1; }
    Peer& pe = g_peers[g_nPeers];
    pe.puid = p; strncpy_s(pe.idStr, puidStr, _TRUNCATE); pe.st = {};
    pe.rosterSeen = fromRoster;
    Log("[p2p] peer #%d = %s (%s)", g_nPeers, pe.idStr, fromRoster ? "lobby roster" : "unvouched, learned from traffic");
    return g_nPeers++;
}

// The public seam. Its only caller is omp_ping's paste-an-id flow -- a deliberate local action by the
// person running the tool, so it gets the roster budget; the mod itself never calls it.
int AddPeer(const char* puidStr) { return addPeerEx(puidStr, true); }

// ---- trust. EOS can prove two things and this reports exactly those two: the sender
// holds an authenticated product user id (the SDK would not deliver their packet otherwise), and the
// LOBBY places them in our session. Both are required for VOUCHED -- authentication alone only says
// "a real Epic user", which is every stranger who ever found the browser.
// A peer in the join window is legitimately unvouched for a moment; callers treat that as provisional
// rather than hostile, which is what keeps a join from dropping its leading packets.
TrustLevel PeerTrust(int idx) {
    if (idx < 0 || idx >= g_nPeers) return TRUST_NONE;
    const Peer& p = g_peers[idx];
    return (p.rosterSeen && p.st.state != 5) ? TRUST_VOUCHED : TRUST_NONE;
}
bool SessionOpen() { return SessionOpenLocal(); }

// ---- route policy. Callable on the LIVE platform, which is what makes an in-game toggle possible at
// all -- see the note at the Init call site.
// EOS_RC_AllowRelays is the SDK default: try to punch a direct connection, fall back to a relay. That
// is the fastest route and it hands your IP address to whoever you are connected to. ForceRelays
// keeps every packet inside Epic's infrastructure, so peers see a relay and never you.
void SetRelayControl(bool force) {
    if (!g_p2p) return;
    EOS_P2P_SetRelayControlOptions rc{};
    rc.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
    rc.RelayControl = force ? EOS_ERelayControl::EOS_RC_ForceRelays
                            : EOS_ERelayControl::EOS_RC_AllowRelays;
    const EOS_EResult r = EOS_P2P_SetRelayControl(g_p2p, &rc);
    if (r != EOS_EResult::EOS_Success) {
        Log("[p2p] SetRelayControl(%s) FAILED: %s -- route policy unchanged",
            force ? "force" : "allow", EOS_EResult_ToString(r));
        return;
    }
    if (g_relaysForced != force) {
        Log("[p2p] route policy: %s", force
            ? "FORCE RELAYS -- peers connect to an Epic relay, not to your address"
            : "ALLOW DIRECT -- lower latency, and peers you connect to can see your IP");
        // Said once, where it matters: a mid-session change does not re-route what is already up.
        if (g_nPeers > 0) Log("[p2p] (existing connections keep the route they negotiated)");
    }
    g_relaysForced = force;
}
bool RelaysForced() { return g_relaysForced; }

void Send(int idx, const void* data, int len, bool reliable) {
    if (idx < 0 || idx >= g_nPeers || len <= 0 || len > EOS_P2P_MAX_PACKET_SIZE) return;
    Peer& p = g_peers[idx];
    if (p.st.state == 5) return;                             // departed the lobby: sends muted
    if (GetTickCount64() < p.closedUntilMs) return;          // cooling down after a remote close
    EOS_P2P_SendPacketOptions o{};
    o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    o.LocalUserId = g_me; o.RemoteUserId = p.puid; o.SocketId = &g_sock;
    o.Channel = 0; o.DataLengthBytes = (uint32_t)len; o.Data = data;
    o.bAllowDelayedDelivery = EOS_TRUE;
    o.Reliability = reliable ? EOS_EPacketReliability::EOS_PR_ReliableOrdered
                             : EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
    o.bDisableAutoAcceptConnection = EOS_FALSE;
    if (EOS_P2P_SendPacket(g_p2p, &o) == EOS_EResult::EOS_Success) p.st.sent++;
}

void Tick(RecvFn onRecv, void* user) {
    if (!g_plat) return;
    EOS_Platform_Tick(g_plat);
    pollLogin();          // sign-in completes HERE, on the game thread, or nowhere
    // A browse whose callback never arrives must not spin forever. Every known cause of that is fixed
    // above, but "the UI can get permanently stuck" deserves a backstop that does not depend on having
    // enumerated them all: after this long, say it failed and let the player press Refresh.
    if (g_browseState == 1 && g_browseStartMs && nowMs() - g_browseStartMs > kBrowseTimeoutMs) {
        InterlockedExchange(&g_browseState, -1);
        g_browseStartMs = 0;
        Log("[lobby] browse timed out after %llums -- no answer from the service",
            (unsigned long long)kBrowseTimeoutMs);
    }
    uint8_t buf[EOS_P2P_MAX_PACKET_SIZE];
    for (;;) {
        EOS_P2P_GetNextReceivedPacketSizeOptions so{};
        so.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
        so.LocalUserId = g_me;
        uint32_t sz = 0;
        if (EOS_P2P_GetNextReceivedPacketSize(g_p2p, &so, &sz) != EOS_EResult::EOS_Success || !sz) break;
        EOS_P2P_ReceivePacketOptions ro{};
        ro.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
        ro.LocalUserId = g_me; ro.MaxDataSizeBytes = sizeof(buf);
        EOS_ProductUserId from = nullptr; EOS_P2P_SocketId sk{}; uint8_t ch = 0; uint32_t got = 0;
        if (EOS_P2P_ReceivePacket(g_p2p, &ro, &from, &sk, &ch, buf, &got) != EOS_EResult::EOS_Success) break;
        Peer* p = peerByPuid(from);
        if (!p) {                                            // first contact: learn the peer
            char id[64] = {0}; int32_t n = sizeof(id);
            EOS_ProductUserId_ToString(from, id, &n);
            // Only LEARN a sender while we are deliberately in a session. An idle instance that never
            // hosted or joined has no business adopting whoever sends it bytes -- the same reason shm
            // gates on its own in-session flag. Peers we ALREADY know keep working regardless, so this
            // can never interrupt a session in progress.
            if (SessionOpenLocal() || g_learnWithoutSession) {
                const int i = addPeerEx(id, false);          // traffic is not a vouching event
                p = (i >= 0) ? &g_peers[i] : nullptr;
            } else {
                static uint64_t lastIdleLogMs = 0;
                const uint64_t now = GetTickCount64();
                if (now - lastIdleLogMs > 5000) {
                    lastIdleLogMs = now;
                    Log("[p2p] dropped a packet from %s -- not in a session", id);
                }
            }
        }
        if (p) {
            // DEPARTED means they left our lobby. Muting sends is not enough on its own: without a
            // receive-side drop their packets still reach the game, so leaving costs an attacker
            // nothing and the channel lives as long as the process.
            // The rejoin case is why this is a re-check and not a plain drop: a returning player is
            // revived to state 0 by rosterAddAll or the JOINED notification, and if that notification
            // is late their first packets would be discarded. So traffic from a departed peer ASKS
            // THE LOBBY (throttled -- it is an SDK round trip, not a free read) instead of being
            // trusted. The roster stays the authority; the packet just prompts the question.
            if (p->st.state == 5) {
                static uint64_t lastRecheckMs = 0;
                const uint64_t now = GetTickCount64();
                if (g_lobbyId[0] && now - lastRecheckMs > 2000) {
                    lastRecheckMs = now;
                    rosterAddAll(g_lobbyId);                 // revives them IF they really rejoined
                }
                if (p->st.state == 5) continue;              // still gone: their packet is not ours
            }
            p->st.recv++; p->lastRecvMs = GetTickCount64();
            if (onRecv) onRecv((int)(p - g_peers), buf, (int)got, user);
        }
    }
}

bool GetStats(int idx, PeerStats* out) {
    if (idx < 0 || idx >= g_nPeers || !out) return false;
    Peer& p = g_peers[idx];
    p.st.lastRecvAgoMs = p.lastRecvMs ? (double)(GetTickCount64() - p.lastRecvMs) : -1.0;
    *out = p.st;
    return true;
}

}} // namespace omp::eosb
