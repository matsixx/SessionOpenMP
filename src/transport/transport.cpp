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
// SessionOpenMP -- the transport ROUTER. Owns the shared log file and dispatches the seam to whichever
// backend the session chose. Adding a wire (direct UDP, a dedicated relay) means one more case here and
// one more file -- the game side never learns which one it was, which is the whole point of the seam.
//
// Only ONE backend is active at a time, and deliberately so. Running shared memory and EOS together
// lets the reader prefer whichever is cheaper, and local testing then never exercises the network path
// at all. Choosing explicitly means a local session tests the mailbox and an online session tests EOS,
// and neither can silently stand in for the other.
#include "transport.h"
#include <cstdio>
#include <cstdarg>
#include <windows.h>

namespace omp {

namespace eosb {
    bool Init(bool forceRelays); void Shutdown(); void Deactivate();
    const char* MyId(); int AddPeer(const char*); int PeerCount();
    void Send(int, const void*, int, bool); void Tick(RecvFn, void*); bool GetStats(int, PeerStats*);
    bool LobbyHost(); bool LobbyJoin(); void LobbyLeave(); int LobbyStatus();
    void SetLobbyAd(const char*, const char*); bool LobbyBrowse(); int BrowseStatus();
    void SetLobbyCode(const char*); const char* LobbyCode(); const char* MakeLobbyCode(char*, int);
    bool LobbyJoinByCode(const char*);
    const char* PeerIdStr(int); bool LobbyIsHost(); bool LobbyKick(const char*);
    const char* LobbyOwnerId();
    int InitState();
    int BrowseCount(); bool BrowseAt(int, LobbyInfo*); bool LobbyJoinAt(int);
    TrustLevel PeerTrust(int); bool SessionOpen();
    void SetRelayControl(bool); bool RelaysForced();
    void AllowLearnWithoutSession(bool);
}
namespace shmb {
    bool Init(bool forceRelays); void Shutdown();
    const char* MyId(); int AddPeer(const char*); int PeerCount();
    void Send(int, const void*, int, bool); void Tick(RecvFn, void*); bool GetStats(int, PeerStats*);
    bool LobbyHost(); bool LobbyJoin(); void LobbyLeave(); int LobbyStatus();
    TrustLevel PeerTrust(int); bool SessionOpen();
}
namespace udpb {
    bool Init(bool forceRelays); void Shutdown(); void Deactivate();
    const char* MyId(); int AddPeer(const char*); int PeerCount();
    void Send(int, const void*, int, bool); void Tick(RecvFn, void*); bool GetStats(int, PeerStats*);
    bool LobbyHost(); bool LobbyJoin(); void LobbyLeave(); int LobbyStatus();
    TrustLevel PeerTrust(int); bool SessionOpen();
    void SetRelayControl(bool); bool RelaysForced();
    void SetDirectEndpoint(const char*, int); const char* DirectEndpoint(); const char* BoundDescription();
    void SetLocalIdentity(const char*);
}

static FILE*    g_log = nullptr;
static Backend  g_cur = BK_NONE;

void Log(const char* fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    if (g_log) { fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line); fflush(g_log); }
    fprintf(stderr, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
}

bool Init(Backend which, const char* logPath, bool forceRelays) {
    if (g_cur != BK_NONE) return g_cur == which;      // already up; switching wires needs a Shutdown
    if (logPath && !g_log) g_log = fopen(logPath, "a");
    bool ok = false;
    if (which == BK_SHM)      { Log("=== transport: SHARED MEMORY (same-PC rig) ==="); ok = shmb::Init(forceRelays); }
    else if (which == BK_EOS) { Log("=== transport: EOS P2P ===");                      ok = eosb::Init(forceRelays); }
    else if (which == BK_UDP) { Log("=== transport: DIRECT UDP (no Epic) ===");         ok = udpb::Init(forceRelays); }
    if (ok) g_cur = which;
    else    Log("[transport] backend %d failed to initialize", (int)which);
    return ok;
}

int SendBudget() {
    switch (Current()) {
    case BK_SHM: return 32;    // = shm's kRelRing/2 (v3) -- keep in step with shm_transport.cpp
    case BK_EOS: return 64;
    case BK_UDP: return 32;
    default:     return 8;
    }
}

Backend Current() { return g_cur; }

// Free the seam for a different backend. See the header for the shm-full-teardown / EOS-resident
// asymmetry.
void Deactivate() {
    if (g_cur == BK_SHM)      { shmb::Shutdown(); Log("[transport] shared memory deactivated"); }
    else if (g_cur == BK_EOS) { eosb::Deactivate(); Log("[transport] EOS deactivated (platform stays resident)"); }
    // UDP tears down fully like shm: a socket is cheap to rebuild and nothing about it is global.
    else if (g_cur == BK_UDP) { udpb::Deactivate(); Log("[transport] direct UDP deactivated"); }
    g_cur = BK_NONE;
}

void Shutdown() {
    if (g_cur == BK_SHM) shmb::Shutdown();
    else if (g_cur == BK_EOS) eosb::Shutdown();
    else if (g_cur == BK_UDP) udpb::Shutdown();
    g_cur = BK_NONE;
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

// Every forward below is written so that "no backend" is a silent no-op rather than a crash: the game
// thread calls Tick/Send unconditionally once a session is armed, and an armed session whose backend
// died must degrade to nothing happening.
const char* MyId() {
    return (g_cur == BK_SHM) ? shmb::MyId() : (g_cur == BK_EOS) ? eosb::MyId()
         : (g_cur == BK_UDP) ? udpb::MyId() : "";
}
int AddPeer(const char* id) {
    return (g_cur == BK_SHM) ? shmb::AddPeer(id) : (g_cur == BK_EOS) ? eosb::AddPeer(id)
         : (g_cur == BK_UDP) ? udpb::AddPeer(id) : -1;
}
int PeerCount() {
    return (g_cur == BK_SHM) ? shmb::PeerCount() : (g_cur == BK_EOS) ? eosb::PeerCount()
         : (g_cur == BK_UDP) ? udpb::PeerCount() : 0;
}
void Send(int i, const void* d, int n, bool rel) {
    if      (g_cur == BK_SHM) shmb::Send(i, d, n, rel);
    else if (g_cur == BK_EOS) eosb::Send(i, d, n, rel);
    else if (g_cur == BK_UDP) udpb::Send(i, d, n, rel);
}
void Tick(RecvFn f, void* u) {
    if      (g_cur == BK_SHM) shmb::Tick(f, u);
    else if (g_cur == BK_EOS) eosb::Tick(f, u);
    else if (g_cur == BK_UDP) udpb::Tick(f, u);
}
bool GetStats(int i, PeerStats* o) {
    return (g_cur == BK_SHM) ? shmb::GetStats(i, o) : (g_cur == BK_EOS) ? eosb::GetStats(i, o)
         : (g_cur == BK_UDP) ? udpb::GetStats(i, o) : false;
}
// ---- direct-connect intent. NOT backend-gated, for the reason stated in the header: the caller sets
// the address and THEN starts the transport, so at the moment of the call g_cur is BK_NONE and a gate
// would silently discard it.
void SetDirectEndpoint(const char* a, int port) { udpb::SetDirectEndpoint(a, port); }
const char* DirectEndpoint() { return udpb::DirectEndpoint(); }
const char* DirectBoundTo()  { return udpb::BoundDescription(); }
void SetLocalIdentity(const char* id) { udpb::SetLocalIdentity(id); }
// Route policy. Shared memory has no route to choose -- the "connection" is a page of RAM -- so this
// is genuinely not applicable there rather than merely unimplemented, and RelaysForced() says false
// so the posture line reports the truth instead of claiming a hidden address.
void SetRelayControl(bool force) { if (g_cur == BK_EOS) eosb::SetRelayControl(force); }
bool RelaysForced()              { return (g_cur == BK_EOS) && eosb::RelaysForced(); }


void Posture(char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    // Two independent facts: WHO can get in, and whether your address is exposed. A backend that
    // cannot hide the address says nothing about relays rather than implying the switch worked.
    switch (g_cur) {
    case BK_EOS:
        snprintf(out, (size_t)cap, "EOS -- signed-in players from your lobby; address %s",
                 eosb::RelaysForced() ? "hidden behind Epic's relays"
                                      : "VISIBLE to peers you connect to directly");
        break;
    case BK_SHM:
        snprintf(out, (size_t)cap, "shared memory -- this PC only, no sign-in and no network");
        break;
    case BK_UDP:
        // Says the uncomfortable part first. This wire authenticates nobody and encrypts nothing, and
        // a player who chose it is entitled to be reminded rather than reassured.
        snprintf(out, (size_t)cap,
                 "direct UDP -- anyone who can reach the port; not encrypted, and peers see your IP");
        break;
    default:
        snprintf(out, (size_t)cap, "no transport running");
        break;
    }
}

// ---- trust. Note the failure value: with NO backend up, nothing can be vouched for, so the honest
// answer is TRUST_NONE and SessionOpen() is false. Callers above the seam are written so that is the
// safe direction (admission rules stop admitting; they never start letting more through).
TrustLevel BackendTrust() {
    // BK_UDP falls through to TRUST_NONE and that is not an omission -- a direct socket authenticates
    // nobody, so the admission rules above the seam correctly become no-ops on it.
    return (g_cur == BK_SHM) ? TRUST_LOCAL : (g_cur == BK_EOS) ? TRUST_VOUCHED : TRUST_NONE;
}
TrustLevel PeerTrust(int i) {
    return (g_cur == BK_SHM) ? shmb::PeerTrust(i) : (g_cur == BK_EOS) ? eosb::PeerTrust(i)
         : (g_cur == BK_UDP) ? udpb::PeerTrust(i) : TRUST_NONE;
}
bool SessionOpen() {
    return (g_cur == BK_SHM) ? shmb::SessionOpen() : (g_cur == BK_EOS) ? eosb::SessionOpen()
         : (g_cur == BK_UDP) ? udpb::SessionOpen() : false;
}
void AllowLearnWithoutSession(bool on) { eosb::AllowLearnWithoutSession(on); }
bool LobbyHost() {
    return (g_cur == BK_SHM) ? shmb::LobbyHost() : (g_cur == BK_EOS) ? eosb::LobbyHost()
         : (g_cur == BK_UDP) ? udpb::LobbyHost() : false;
}
bool LobbyJoin() {
    return (g_cur == BK_SHM) ? shmb::LobbyJoin() : (g_cur == BK_EOS) ? eosb::LobbyJoin()
         : (g_cur == BK_UDP) ? udpb::LobbyJoin() : false;
}
void LobbyLeave() {
    if      (g_cur == BK_SHM) shmb::LobbyLeave();
    else if (g_cur == BK_EOS) eosb::LobbyLeave();
    else if (g_cur == BK_UDP) udpb::LobbyLeave();
}
int LobbyStatus() {
    return (g_cur == BK_SHM) ? shmb::LobbyStatus() : (g_cur == BK_EOS) ? eosb::LobbyStatus()
         : (g_cur == BK_UDP) ? udpb::LobbyStatus() : 0;
}
// ---- the browser is EOS-only BY NATURE, not by omission: shared memory has no directory to search.
// Claiming a mailbox slot IS joining, so "which lobby?" is not a question that exists on that wire.
// THESE ARE INTENT, NOT OPERATIONS -- do NOT gate them on the active backend. The caller sets
// who/where/what-code and THEN starts the transport, so at the moment of the call `g_cur` is still
// BK_NONE; gating them means a cold-start host advertises no host name, no map and no join code, with
// the only visible symptom being a code missing from the roster panel. They write static strings in
// the EOS backend; nothing about that needs a live platform, and the values must already be sitting
// there BEFORE the lobby is created for publishAd to find them.
void SetLobbyAd(const char* h, const char* m) { eosb::SetLobbyAd(h, m); }
bool LobbyBrowse(){ return (g_cur == BK_EOS) && eosb::LobbyBrowse(); }
int  BrowseStatus(){ return (g_cur == BK_EOS) ? eosb::BrowseStatus() : 0; }
int  BrowseCount(){ return (g_cur == BK_EOS) ? eosb::BrowseCount() : 0; }
bool BrowseAt(int i, LobbyInfo* o){ return (g_cur == BK_EOS) && eosb::BrowseAt(i, o); }
bool LobbyJoinAt(int i){ return (g_cur == BK_EOS) && eosb::LobbyJoinAt(i); }
// Private games are an EOS concept -- shared memory has one mailbox and no directory, so there is
// nothing for a code to select between.
void SetLobbyCode(const char* c){ eosb::SetLobbyCode(c); }
// The getter is safe to forward too: g_myCode is only ever non-empty while we are in a lobby that
// HAS a code, which can only happen on the EOS wire in the first place.
const char* LobbyCode(){ return eosb::LobbyCode(); }
const char* MakeLobbyCode(char* o, int c){ return eosb::MakeLobbyCode(o, c); }
bool LobbyJoinByCode(const char* c){ return (g_cur == BK_EOS) && eosb::LobbyJoinByCode(c); }
// Moderation is EOS-only: shared memory has no lobby, no owner and no membership to remove anyone
// from -- claiming a mailbox slot IS joining.
const char* PeerIdStr(int i){ return (g_cur == BK_EOS) ? eosb::PeerIdStr(i) : ""; }
bool LobbyIsHost(){ return (g_cur == BK_EOS) && eosb::LobbyIsHost(); }
// Only EOS has a lobby with an owner; the other backends have no such concept, so "" is the honest
// answer rather than pretending somebody is in charge.
const char* LobbyOwnerId(){ return (g_cur == BK_EOS) ? eosb::LobbyOwnerId() : ""; }
// Shared memory and UDP come up in microseconds and have nothing to sign in to, so a successful Init
// IS readiness for them. Only EOS has a state to poll.
int InitState(){ return (g_cur == BK_EOS) ? eosb::InitState() : (g_cur == BK_NONE ? 0 : 2); }
bool LobbyKick(const char* id){ return (g_cur == BK_EOS) && eosb::LobbyKick(id); }

} // namespace omp
