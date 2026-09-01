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
// SessionOpenMP -- transport seam.
// The rule this API encodes: the game must never know or care which wire its packets rode. Backends
// live behind this seam (EOS P2P, shared memory, direct UDP), identity is established BY the transport
// (a peer index, never an id inside a payload), and everything a session needs to know about link
// health is exposed as numbers.
#pragma once
#include <cstdint>

namespace omp {

struct PeerStats {
    uint64_t sent = 0, recv = 0;
    int      state = 0;          // 0 none/connecting, 1 established DIRECT, 2 established RELAY,
                                 // 3 interrupted, 4 closed, 5 DEPARTED (left the lobby: sends muted,
                                 // index stays valid -- indices are handed out once, never reused)
    double   lastRecvAgoMs = -1; // -1 = never heard from them
};

using RecvFn = void (*)(int peerIdx, const uint8_t* data, int len, void* user);

// ---- backends. The seam exists so the game never knows which wire it rode; these are the wires.
//   Eos -- Epic P2P + lobbies. Between MACHINES. Cannot test two instances on ONE PC: they share a
//          device id, so both log in as the SAME PUID and there is nobody to talk to.
//   Shm -- a named shared-memory mailbox. Same PC only, no login, no sockets, no firewall, instant.
//          This is the local development rig.
//   Udp -- DIRECT sockets. No Epic, no sign-in, no third party: the host listens on a port and the
//          joiner is given an address. For LAN, for a host who can port-forward, and for anyone who
//          cannot or will not use EOS. Not encrypted, not authenticated, and not hole-punched -- see
//          udp_transport.cpp's header, and Posture(), which says so to the player.
// One backend is active per session; the mode is chosen when the session starts.
//   Relay -- everyone dials OUT to one forwarding server. The only wire that works for 3+
//            players over the internet without somebody forwarding a port, because a client
//            never has to be reachable -- only the relay does. Not encrypted: whoever runs it
//            sees the traffic, which Posture() says out loud. See docs/relay-scope.md.
enum Backend { BK_NONE = 0, BK_EOS = 1, BK_SHM = 2, BK_UDP = 3, BK_RELAY = 4 };

// CALL ON THE GAME THREAD, always: the EOS SDK is SHARED with the game's own EOSShared plugin, and a
// second thread inside it takes down the game's tick (see eos_transport.cpp). Returns as soon as
// sign-in has been STARTED -- it does not block and it does not mean "ready"; poll InitState().
// `forceRelays` (Eos only) routes all traffic through Epic's relays.
bool        Init(Backend which, const char* logPath, bool forceRelays);
Backend     Current();
void        Shutdown();
// Release the active wire so a DIFFERENT one can Init -- backend switching ("host a local test, then
// go online") needs it. Shm tears down fully (stateless, rebuilt in microseconds); EOS leaves its
// lobby but the PLATFORM stays resident -- the SDK cannot be re-initialized mid-process,
// and a later Init(BK_EOS) reuses the logged-in platform. Callers must reset SESSION peer state too:
// slots are keyed by transport index, and shm slot 0 / EOS peer 0 are different humans.
void        Deactivate();
const char* MyId();                          // Eos: PUID string. Shm: "shm:<slot>". Valid after Init.
int         AddPeer(const char* puidStr);    // -> peer index, or -1 (Shm: peers are discovered, not added)
int         PeerCount();                     // peer indices are [0, PeerCount) and are NEVER reused
void        Send(int peerIdx, const void* data, int len, bool reliable);
// How many RELIABLE messages this wire can absorb per Tick without loss -- the backpressure hint
// for bulk senders (replay sync paces its chunk bursts by it). Shm: half its reliable ring, so one
// burst can never lap a reader that drains every poll; EOS/UDP: the SDK/socket queues buffer far
// more, capped to keep bursts polite. Numbers, not identities: the seam's rule holds.
int         SendBudget();
void        Tick(RecvFn onRecv, void* user); // platform tick + receive drain; call every ~10 ms
bool        GetStats(int peerIdx, PeerStats* out);
void        Log(const char* fmt, ...);

// ---- TRUST. What the ACTIVE BACKEND can actually prove about who is sending, asked as a question
// rather than assumed. The admission rules above the seam must not be written in EOS's vocabulary:
// custom relay servers, direct P2P and dedicated servers are all on the roadmap and none of them has
// a lobby roster to consult. A backend answers with what it HAS, and a backend that can prove nothing
// says so rather than having a stricter backend's semantics faked on its behalf.
//   TRUST_NONE    -- anyone who can reach the socket. The rules degrade to no-ops; that is the correct
//                    behaviour for a wire the user chose knowing it is unauthenticated.
//   TRUST_LOCAL   -- same machine, same logon session (Shm: the sender owns a claimed mailbox slot).
//   TRUST_VOUCHED -- the backend authenticated them AND places them in this session (Eos: a logged-in
//                    product user id that is on our lobby roster).
enum TrustLevel { TRUST_NONE = 0, TRUST_LOCAL = 1, TRUST_VOUCHED = 2 };

// The best this backend can EVER vouch for, regardless of any particular peer. Drives the posture
// line and lets a caller skip work a TRUST_NONE backend could never answer.
// Change the route policy on a LIVE backend. Eos: EOS_P2P_SetRelayControl on the running platform --
// Init's `forceRelays` only ever ran during first-time platform creation, and the EOS platform stays
// resident for the process lifetime, so there is no re-Init path to change it through. Backends that
// have no such concept (Shm) are a no-op, and callers must NOT read that as "the address is hidden".
// Applies to connections established AFTER the call; existing links keep the route they negotiated.
void        SetRelayControl(bool forceRelays);
// Are we currently forcing relays? Reports what the backend was last TOLD, so the posture line can
// state the live route policy rather than the preference the player set.
bool        RelaysForced();

// A one-line, plain-English statement of what the ACTIVE configuration actually gives you: who can be
// vouched for, and whether your address is exposed. Written into `out`.
// The model is "pick your transport, here are its risks" -- a player should be able to read the
// posture rather than remember which build they launched, and every backend describes itself here
// instead of the UI growing a special case per wire.
// DESCRIPTIVE, never advisory: it reports the mode the user chose; it does not nag about it.
void        Posture(char* out, int cap);

TrustLevel  BackendTrust();
// What we can prove about THIS peer right now. Never better than BackendTrust().
TrustLevel  PeerTrust(int peerIdx);
// Are we deliberately in a session (hosting, joined, or joining)? The honest answer to "should this
// process be accepting gameplay from anyone at all". Shm answers from its own in-session flag, which
// already exists for exactly this reason: two solo instances sharing a PC must not adopt each other.
bool        SessionOpen();
// Normally a sender is only LEARNED from an inbound packet while we are deliberately in a session --
// an idle game must not adopt whoever sends it bytes. `omp_ping` is the deliberate exception: it is a
// point-to-point transport proof with no lobby at all, and it ticks the wire while the operator is
// still pasting the other id. Tools opt in; the mod never does.
void        AllowLearnWithoutSession(bool on);

// ---- lobby session management. The lobby ROSTER is the peer list: hosting advertises a public
// lobby in the mod's bucket, joining searches that bucket and joins the first hit, and every OTHER
// member -- present at join or arriving later -- is added as a peer automatically. A member who leaves
// is marked DEPARTED (state 5): sends stop, the index stays valid. All async; poll LobbyStatus().
// Shm has no lobby to speak of: claiming a mailbox slot IS joining, so host and join are the same act
// and both report success immediately.
bool        LobbyHost();     // false = not logged in or an op is already in flight / in a lobby
bool        LobbyJoin();     // "
void        LobbyLeave();
int         LobbyStatus();   // 0 idle, 1 op in flight, 2 hosting, 3 joined, -1 last op failed

// ---- THE LOBBY BROWSER. `LobbyJoin` above is "find the bucket and take the first hit", which is all
// a two-person rig needs. Browsing is the same search, kept instead of consumed: the results stay
// live so a human can pick one. The search HANDLE is retained too, because
// joining is done by re-copying a result by index -- an EOS lobby is joined through its details
// handle, never through its id string, so the list and the join must come from the same search.
struct LobbyInfo {
    char id[80];        // EOS lobby id (display/diagnostics only -- you join by INDEX)
    char host[40];      // the host's skater name, advertised as a lobby attribute
    char map[40];       // the map they are on, ditto
    int  players;       // members right now (from the search result, no join required)
    int  maxPlayers;
    // The host's mod version, empty if they are too old to advertise one. Compare against
    // OMP_VERSION_STRING: a mismatch means the wire formats may differ, and joining anyway produces a
    // session where the other player never appears rather than an error -- so it is worth saying
    // before the join, not after.
    char version[16];
    // Which SEARCH RESULT this row came from. The list is FILTERED (private lobbies, stale ghosts,
    // our own orphans never become rows), so a row's position in the list and its position in the
    // search results diverge -- and joining by the LIST index picked the wrong lobby whenever they
    // did. Joins must use this, never the row number.
    int  searchIdx;
};
// What WE advertise while hosting. Call before LobbyHost(); the attributes are attached to the lobby
// immediately after it is created (EOS_Lobby_CreateLobby takes no attributes of its own). Safe to call
// again later -- it re-publishes, which is how the map name follows you across a level change.
void        SetLobbyAd(const char* hostName, const char* mapName);
// ---- PRIVATE LOBBIES. A private game is an ordinary EOS lobby carrying a CODE
// attribute. It is still `PUBLICADVERTISED` -- it has to be, or a search could never find it and the
// code would be unusable -- but the public browser SKIPS any lobby that has a code, so the only way
// in is to know the six characters. That is what a join code is everywhere else, and it costs no new
// machinery: the code is one more searchable attribute and joining is the browse we already have,
// filtered by it.
// Empty/null code = an ordinary public game (the default).
// ---- DIRECT CONNECT (Udp). Intent setters, like SetLobbyAd above: call them BEFORE the transport
// starts, so they are deliberately NOT gated on the active backend -- at the moment of the call the
// backend is still BK_NONE, and gating on it silently drops every one.
//   `joinAddress` -- "1.2.3.4:7777", or a hostname; the port defaults to 7777 if omitted. Used by JOIN.
//   `listenPort`  -- the port HOST binds. 0 = 7777.
void        SetDirectEndpoint(const char* joinAddress, int listenPort);
const char* DirectEndpoint();                // what we were last told to join ("" if never set)
const char* DirectBoundTo();                 // the local "ip:port" we actually bound, for showing a host
// This install's permanent identity, for backends that have no account system to borrow one from.
// EOS ignores it (a ProductUserId already identifies you). Set it before Init.
void        SetLocalIdentity(const char* id);
// The relay server to dial. Same act as SetDirectEndpoint from the player's side -- type where
// to connect -- so the UI sets both and the chosen backend decides which one matters.
void        SetRelayServer(const char* addr, int port);

void        SetLobbyCode(const char* code);  // "" = public. Call before LobbyHost().
const char* LobbyCode();                     // the code of the lobby we are IN (host or guest), "" if public
const char* MakeLobbyCode(char* out, int cap);   // a fresh unambiguous 6-character code
bool        LobbyJoinByCode(const char* code);   // async: search by code, join the hit. Poll LobbyStatus()

// ---- MODERATION. Every host polices their OWN session and nobody else's, because that
// is exactly the authority EOS grants: `EOS_Lobby_KickMember` documents that the caller "must be the
// lobby owner". There is no server in this design -- a host is just the peer who made the lobby -- so
// kicking someone out of a session you did not create is not a thing any client-side check could
// grant. `LobbyKick` therefore refuses unless we are HOSTING.
const char* PeerIdStr(int peerIdx);          // the peer's stable identity (Eos: PUID). "" if unknown.
// Sign-in is ASYNCHRONOUS: Init() only STARTS it, because the EOS SDK is shared with the game's own
// EOSShared plugin and may only be touched from the game thread (eos_transport.cpp's note). Poll this
// every frame until it leaves 1. Backends with no sign-in report 2 the moment Init succeeds.
//   0 idle   1 signing in   2 ready   3 failed
int         InitState();
bool        LobbyIsHost();                   // we own the lobby -> kick is available
// WHO owns it, as a peer identity, or "" if unknown. Ownership migrates, so this is re-read from the
// service on every membership change rather than remembered from who created the lobby.
const char* LobbyOwnerId();
// Can this wire EVER name a lobby owner? True on EOS (the answer may still be in flight); false on
// shared memory and UDP, which are symmetric by construction. The difference matters: a fallback
// that decides authority some other way is correct on a wire that will never know, and a data race
// on one that merely does not know YET.
bool LobbyOwnershipKnowable();
bool        LobbyKick(const char* peerId);   // async; false = not hosting / no such member
bool        LobbyBrowse();                   // start an async search that KEEPS its results
int         BrowseStatus();                  // 0 idle, 1 searching, 2 results ready, -1 failed
int         BrowseCount();
bool        BrowseAt(int i, LobbyInfo* out);
bool        LobbyJoinAt(int i);              // join the i-th result of the last browse

} // namespace omp
