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
// SessionOpenMP -- session orchestration.
#include "session.h"
#include "../debug.h"
#include "../transport/transport.h"
#include "../game/game_syms.h"
#include "../game/cosmetics.h"
#include "../game/audio.h"
#include "../game/pose.h"
#include "../game/spectate.h"
#include "../game/dropper.h"
#include "../ui/mp_prefs.h"
#ifdef _WIN32
#define PSAPI_VERSION 2          // K32EnumProcessModules from kernel32 -- no psapi.lib dependency
#include <windows.h>
#include <psapi.h>
#endif
#include "../replication/replaysync.h"
#include "../replication/dropsync.h"
#include <cstring>
#include <cstdio>

namespace omp { namespace session {

static const int kMaxPeers = 16;

struct Slot {
    bool        used = false;
    int         peerIdx = -1;
    repl::Stream stream;
    game::Proxy  proxy;
    // MICROSECONDS, and from the SAME clock Frame() is given. ONE CLOCK PER SUBTRACTION: mixing epochs
    // (say, a millisecond stamped from nowUs against a GetTickCount64 millisecond) underflows the
    // unsigned subtraction, which reads as an enormous quiet time and releases every slot the frame
    // after it opens. Session liveness is nowUs, end to end.
    uint64_t    lastPacketUs = 0;
    bool        quietHandled = false;
    // ---- cosmetics: their look, and whether this proxy is currently wearing it.
    repl::CosmeticSet cosmetics;
    bool        haveCosmetics = false;
    // Keyed on the ACTOR, never a plain "already dressed" bool: `Proxy::Forget()` drops the actor on a
    // world change while the SLOT survives (same peer), so a bool stays true and the RESPAWNED proxy is
    // never dressed -- it keeps the look a fresh skater is built from, i.e. the LOCAL player's. A
    // different actor has by definition not been dressed, so every respawn re-dresses by construction.
    void*       wornForActor = nullptr;  // null = needs dressing (new set, respawn, world change)
    // ---- chat: the last message id seen from this peer, so a redelivery cannot print twice.
    uint32_t    lastChatId = 0;
    bool        haveChatId = false;
    // Said once per peer: an unreadable packet repeats at the send rate, and a warning that
    // repeats 60 times a second is a log flood, not a warning.
    bool        rejectedSpoken = false;
    // Said once per peer: held unspawned awaiting the lobby roster's vouch (see the spawn gate).
    bool        unvouchedSpoken = false;
    // This peer is composing a chat message (their latest sampled snapshot says so). Drives the
    // "..." bubble over their skater; read through PeerTyping(), which also gates out quiet and
    // away peers so a dropped connection cannot leave the dots hanging over a frozen skater.
    bool        peerTyping = false;
    // This peer has the replay editor open (their latest sampled snapshot says so). Drives the
    // per-peer packet choice in the publish loop: a scrubbing peer gets RESULTS, everyone else
    // keeps DRIVERS. Latched from the stream, so a quiet peer keeps their last known state.
    bool        peerReplaying = false;
    // Hidden from the LOCAL player's replay: their components are parked and the actor concealed
    // while playback runs. Reset when playback ends -- the next replay starts with everyone shown.
    bool        replayHidden = false;
    // Within board-sim range of the local player (hysteresis lives here, where the distance is
    // measured). Starts near so a board simulates until proven far.
    bool        boardNear = true;
    // Non-null = this actor is concealed because the LOCAL player is in replay playback. The replay
    // editor is your own instance: peers stay fully live underneath (driven, buffered, current the
    // frame you exit) but invisible. Keyed on the ACTOR, wornForActor-style, so a respawn mid-
    // playback gets hidden too and a world change cannot leave the flag pointing at a dead actor.
    void*       replayConcealedActor = nullptr;
    // This peer has been announced in chat ("<name> joined"). Set when their name first lands (it
    // rides cosmetics), so the announcement can use it; reset with the slot.
    bool        joinAnnounced = false;
    // In a DIFFERENT level from us. Their proxy (if one exists) is hidden and undriven until they
    // come back -- see the gate in Frame. Read by PeerAt/PeerActorById, which report no actor for an
    // away peer: it is hidden in a world they are not in, so nothing should point a nameplate or a
    // replay camera at it.
    bool        away = false;
    // ---- replay sync: this peer's own state history, transferred on request, shown in OUR replay.
    bool        syncOn = false;          // the menu's wish; the transfer/buffer state lives in replaysync
    uint64_t    syncReqSentUs = 0;       // when we asked -- anchors their buffer's newest to our clock
    // Their stable identity, hashed, as it rides their set packets -- the SHARED policy's tiebreak.
    // Unknown until their first set lands, and "unknown" deliberately means "not in the running":
    // a peer with the feature off never sends one and can never be picked as the canonical set.
    uint64_t    dropAuthKey = 0;
    bool        haveDropAuth = false;
    bool        worldSetFresh = false;   // a COMPLETE world layout from this peer awaits the frame
    // THE SENDER'S SKELETON, by bone-name hash. Arrives on its own rare message and is HELD here
    // until their proxy mesh exists to attach it to -- the packet routinely beats the spawn. Fed to
    // the pose layer whenever either side of that pair changes.
    repl::SkelPrint skel;
    bool        haveSkel = false;
    void*       skelFedFor = nullptr;    // the mesh we last fed it to; a respawn re-feeds
    // The per-peer name cache for interned loop/trick names -- see AudioNameCache. Applied at
    // OnPacket, before the state reaches the stream, so everything downstream is complete.
    repl::AudioNameCache nameCache;
    // Synced-replay AUDIO. The cursor is the owner-clock time up to which their history's one-shot
    // events have already been fired; each frame fires (cursor, now] exactly once. A scrub JUMP --
    // backwards, or forward by more than a breath -- resyncs the cursor silently instead of firing
    // the gap: jumping the timeline must not burst-replay every sound in between.
    uint64_t    syncAudioUs = 0;
    bool        syncWasDriven = false;   // falling edge stops any loops the sync playback started
    // ---- dropped objects: the props THIS peer has in OUR world. The wire updates this table (ids,
    // target poses, what is new and what is gone); Frame does every game call from it. Splitting it
    // that way keeps actor spawning on the engine-tick anchor where the proxy spawn already lives,
    // and means a burst of packets cannot spawn a burst of actors mid-pump.
    struct DropObj {
        bool     used = false;
        uint16_t id = 0;
        void*    actor = nullptr;        // null = not spawned yet
        char     cls[64] = {0};          // needed until it is spawned; kept for the log after
        bool     spawnFailed = false;    // resolved to nothing here: never retried, never re-logged
        float    tgtLoc[3] = {0,0,0}, tgtQuat[4] = {0,0,0,1};
        float    curLoc[3] = {0,0,0}, curQuat[4] = {0,0,0,1};
        bool     seeded = false;         // cur == tgt at least once (a fresh object is never slid in)
        bool     dead = false;           // the wire says it is gone; Frame destroys it and frees the slot
    };
    DropObj     drop[dropsync::kMaxSetRecords];
    bool        dropGenSeen = false;
    uint8_t     dropGen = 0;
};

// When the local player ENTERED playback (mode 2), our clock. The replay timeline's END is this
// moment; a synced peer's buffer-newest is their syncReqSentUs moment. The difference maps scrub
// seconds onto their history. 0 = not in playback.
static uint64_t g_playbackEnteredUs = 0;

// Unsigned timestamps must never be subtracted raw: a sample stamped slightly in the future (clock
// granularity, a reordered call) wraps to ~1.8e19 instead of going negative, which reads as "ancient"
// to every > comparison. Saturating at 0 makes the worst case "treat it as brand new", which is the
// harmless direction for a liveness test.
static uint64_t sinceUs(uint64_t now, uint64_t then) { return (now > then) ? (now - then) : 0; }

static Slot   g_slots[kMaxPeers];
static Config g_cfg;
static Stats  g_st;
static void (*g_logf)(const char*) = nullptr;
static uint64_t g_lastPubUs = 0;
static bool     g_cosResend = false;   // a new peer appeared: re-send our look without waiting 10 s
static bool     g_skelResend = false;  // ...and our skeleton, for the same reason
static bool     g_nameResend = false;  // ...and un-interned loop/trick names, ditto
// SessionTweaks' analog-crouch clock, resolved by GetProcAddress -- the same seam its menu pages use
// to find OUR exports, run the other way. Both mods ship as main.dll in different folders, so module
// enumeration + a distinctive export name is the only sane linkage. Probed until found, then cached;
// a session without SessionTweaks (or an older one) simply never resolves it, and every cranked
// frame carries the "not scrubbed" sentinel -- peers play the vanilla descent, exactly as today.
typedef int (*TwkCrankVisClockFn)(float*);
static TwkCrankVisClockFn twkCrankVisClock() {
#ifdef _WIN32
    static TwkCrankVisClockFn fn = nullptr;
    static uint64_t nextProbeMs = 0;
    if (fn) return fn;
    const uint64_t now = GetTickCount64();
    if (now < nextProbeMs) return nullptr;
    nextProbeMs = now + 1000;
    HMODULE mods[512]; DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        const int n = (int)(need / sizeof(HMODULE));
        for (int i = 0; i < n && i < 512; i++) {
            auto f = (TwkCrankVisClockFn)GetProcAddress(mods[i], "Twk_CrankVisClock");
            if (f) { fn = f; break; }
        }
    }
    return fn;
#else
    return nullptr;
#endif
}
static bool     g_anyPeerReplaying = false;   // somebody is scrubbing and needs our finished pose
static repl::State g_ownLast;      // last successfully gathered own state (for spawn placement)
static bool        g_haveOwn = false;

// ---- DROPPED OBJECTS, our own side. See session.h for what the policy means.
static uint8_t  g_dropPolicy = 2;             // Shared -- the default the menu row starts on
static bool     g_dropResend = false;         // a new peer, or our world changed: full resync now
static uint8_t  g_dropGen = 0;                // bumped whenever OUR localIds stop meaning what they meant
static uint16_t g_dropNextId = 1;
static bool     g_dropAdopted = false;        // we are the one hiding our own saved set
// ---- THE LEVEL'S OWN PROPS. The session layout is THE HOST'S ARRANGEMENT, applied by every client
// to its OWN copy of the real actors (nothing is spawned -- the actor named is baked into the map).
// This table is that layout: `tgt` is the pose the session says each prop stands on, seeded by the
// host from its own world, folded forward by moves from whoever is holding a prop, and REPUBLISHED
// by the host alone on the resync beat -- from this table, never from an actor readback, because an
// actor mid-chase lags its target and echoing it is what made two clients fight. One writer per
// prop at any moment: the holder while held, the host's stored value otherwise.
struct WorldEntry {
    char     name[64];
    bool     mine;             // WE are holding it: we publish, and we ignore everyone else
    int      holder;           // peer index currently holding it, or -1
    uint64_t heldUs;           // when we last heard from that holder -- a claim goes stale
    float    tgtLoc[3], tgtQuat[4];
    float    curLoc[3], curQuat[4];
    bool     seeded;           // cur primed (a fresh entry is stamped, never slid across the map)
    bool     driving;          // the actor is being driven toward tgt
    bool     inSet;            // transient mark while applying a host layout
    float    lastSentLoc[3], lastSentQuat[4];
};
static WorldEntry g_world[dropsync::kMaxWorldRecords];
static int        g_worldN = 0;
static bool       g_worldOn = false;          // the session layout is applied
static bool       g_worldSeeded = false;      // the HOST half: our arrangement has been tabled
static const uint64_t kWorldClaimStaleUs = 2000000ull;

static WorldEntry* worldFind(const char* name) {
    for (int i = 0; i < g_worldN; i++) if (_stricmp(g_world[i].name, name) == 0) return &g_world[i];
    return nullptr;
}
static WorldEntry* worldAdd(const char* name) {
    if (WorldEntry* w = worldFind(name)) return w;
    if (g_worldN >= (int)(sizeof(g_world) / sizeof(g_world[0]))) return nullptr;
    WorldEntry& w = g_world[g_worldN++];
    memset(&w, 0, sizeof(w));
    strncpy_s(w.name, sizeof(w.name), name, _TRUNCATE);
    w.holder = -1; w.tgtQuat[3] = 1; w.curQuat[3] = 1; w.lastSentQuat[3] = 1;
    return &w;
}
static void worldTearDown() {
    game::dropper::RestoreWorldAll(g_logf);
    g_worldN = 0; g_worldOn = false; g_worldSeeded = false;
}
// Our published set, as of the last enumeration: the diff baseline. Keyed by ACTOR pointer, which is
// stable for as long as the object exists and dies with the world (which is exactly when the
// generation bumps), so no id ever outlives its meaning.
struct OwnDrop {
    void*    actor = nullptr;
    uint16_t id = 0;
    char     cls[64] = {0};
    float    loc[3] = {0,0,0}, quat[4] = {0,0,0,1};
    bool     seen = false;                    // marked during a diff pass; unmarked = gone
};
static OwnDrop g_ownDrop[game::dropper::kMaxObjects];
static int     g_ownDropN = 0;
static int     g_dropSpawnCapSaid = 0;

uint8_t DropPolicy() { return g_dropPolicy; }

void Init(void (*logf)(const char*)) {
    g_logf = logf;
    replaysync::SetSendFn(&Send);
    dropsync::SetSendFn(&Send);
}

// Our dropped-object ids stop meaning what they meant: a world change (every actor died with it), a
// session reset, a policy change. Bump the generation, forget the baseline, and tell everyone.
static void dropNewGeneration(const char* why) {
    g_dropGen++;
    g_ownDropN = 0; g_dropNextId = 1;
    g_dropResend = true;
    if (g_logf && why) { char m[140]; snprintf(m, sizeof(m), "[drop] generation %u (%s)",
                         (unsigned)g_dropGen, why); g_logf(m); }
}
void ResetAll() {
    int n = 0;
    for (auto& s : g_slots) if (s.used) {
        // RETIRE, not OnQuiet. `Forget` only drops our POINTERS -- the actors keep standing in the
        // world, so on its own it strands everyone you were skating with in your now single-player
        // game until the next map load. OnQuiet stops their boards but leaves them visible; Retire
        // does that AND hides them, matching the drop path. This is the LAST code that will ever hold
        // these pointers, so if it does not hide them, nothing will.
        s.proxy.Retire(g_logf);
        s.proxy.Forget();
        s.used = false; n++;
    }
    if (n && g_logf) { char m[120]; snprintf(m, sizeof(m), "[session] reset -- %d peer slot(s) released", n); g_logf(m); }
    replaysync::DropAll(); g_playbackEnteredUs = 0;
    // Peers' props go with them, and OUR OWN SET COMES BACK. Leaving a session with your park still
    // hidden would look exactly like the mod deleted it.
    game::dropper::ResetAll(g_logf);
    g_dropAdopted = false;
    dropsync::ResetAll();
    dropNewGeneration("session reset");
}
// NOT ResetAll: this runs from the mod's destructor, i.e. DLL unload, where the game may already be
// tearing itself down -- and Retire touches actors, the replay camera and the audio system. Hiding a
// skater buys nothing in a process that is exiting, so drop the pointers and touch nothing.
void Shutdown() {
    for (auto& s : g_slots) { s.proxy.Forget(); s.used = false; }
}
void SetConfig(const Config& c) { g_cfg = c; }
const Config& GetConfig() { return g_cfg; }
Stats GetStats() { return g_st; }

static Slot* slotFor(int peerIdx, uint64_t nowUs) {
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerIdx) return &s;
    for (auto& s : g_slots) {
        if (s.used) continue;
        s.stream.Reset(); s.proxy.Forget();          // nothing carries across to a new peer
        s.used = true; s.peerIdx = peerIdx; s.lastPacketUs = nowUs; s.quietHandled = false;
        memset(&s.cosmetics, 0, sizeof(s.cosmetics));   // padding too -- it is memcmp'd for changes
        s.haveCosmetics = false; s.wornForActor = nullptr; s.peerReplaying = false;
        s.peerTyping = false;
        s.rejectedSpoken = false; s.unvouchedSpoken = false;
        s.replayHidden = false; s.boardNear = true; s.replayConcealedActor = nullptr;
        s.away = false; s.joinAnnounced = false;
        s.syncOn = false; s.syncReqSentUs = 0;
        for (auto& d : s.drop) d = Slot::DropObj();
        s.dropGenSeen = false; s.dropGen = 0;
        s.dropAuthKey = 0; s.haveDropAuth = false; s.worldSetFresh = false;
        s.skel = repl::SkelPrint(); s.haveSkel = false; s.skelFedFor = nullptr;
        s.syncAudioUs = 0; s.syncWasDriven = false;
        // THE NAME CACHE DIES WITH THE SLOT. It is keyed by the SENDER's loop handles, which mean
        // nothing across peers -- a recycled slot that kept it would resolve a new peer's interned
        // (nameless) loops and tricks to the PREVIOUS peer's names, on every packet that is not a
        // refresh. That is 3 packets in 4 carrying a wrong trick name, which reads as a proxy stuck
        // in an animation that never ends, and it never self-heals because the wrong answer is
        // always available. Missed in the interning round; a reconnect is exactly when it bites.
        s.nameCache = repl::AudioNameCache{};
        dropsync::ForgetPeer(peerIdx);
        g_skelResend = true;                         // they need OUR skeleton too
        g_nameResend = true;                         // ...and full loop/trick NAMES on the next
                                                     // publish: the refresh counters are per-process,
                                                     // so a peer joining mid-trick would otherwise
                                                     // wait on a cycle that is not theirs
        g_cosResend = true;                          // they need OUR look too, now rather than in 10 s
        g_dropResend = true;                         // ...and our dropped objects, for the same reason
        if (g_logf) { char m[120]; snprintf(m, sizeof(m), "[session] stream opened for peer %d", peerIdx); g_logf(m); }
        return &s;
    }
    return nullptr;                                  // full: drop rather than steal another peer's slot
}

// ---- DROPPED OBJECTS: wire -> the peer's object table. No game calls here (see the Slot comment).
static Slot::DropObj* dropFind(Slot& s, uint16_t id) {
    for (auto& d : s.drop) if (d.used && d.id == id) return &d;
    return nullptr;
}
static Slot::DropObj* dropAlloc(Slot& s, uint16_t id) {
    if (Slot::DropObj* e = dropFind(s, id)) return e;
    for (auto& d : s.drop) if (!d.used) { d = Slot::DropObj(); d.used = true; d.id = id; return &d; }
    return nullptr;                                   // their set is bigger than the cap: ignore the rest
}
static void dropSetTarget(Slot::DropObj& d, const dropsync::Rec& r, bool withClass) {
    memcpy(d.tgtLoc,  r.loc,  sizeof(d.tgtLoc));
    memcpy(d.tgtQuat, r.quat, sizeof(d.tgtQuat));
    if (withClass && r.id[0]) {
        strncpy_s(d.cls, sizeof(d.cls), r.id, _TRUNCATE);
        // A class-carrying record is a fresh chance: a spawn that failed TRANSIENTLY (the dangling
        // resolve was one; an engine hiccup could be another) retries on the peer's next resync
        // beat instead of staying a hole until their generation changes. Bounded by the 6 s beat,
        // so a genuinely uninstallable object costs one attempt per beat, not a spawn storm.
        d.spawnFailed = false;
    }
    d.dead = false;
}
// Mark every object this peer still has for destruction. Frame does the destroying; a peer whose
// packets stop arriving keeps its objects (they are still THERE in their world) until it is released.
static void dropMarkAllDead(Slot& s) { for (auto& d : s.drop) if (d.used) d.dead = true; }

static void applyDropUpdate(Slot& s, const dropsync::Update& up, int peerIdx, uint64_t nowUs) {
    if (up.haveAuth) { s.dropAuthKey = up.authKey; s.haveDropAuth = true; }
    if (up.genReset) {
        // Their ids stopped meaning what they meant (their world changed, or they reset). Everything
        // we hold for them is stale by definition -- including the ACTORS, which are still standing
        // in our world and must go.
        dropMarkAllDead(s);
        s.dropGenSeen = true; s.dropGen = up.gen;
    }
    if (up.setReady) {
        int n = 0;
        const dropsync::Rec* set = dropsync::SetRecords(peerIdx, &n);
        // The other half of the publish line above. A set that arrives and a set that publishes are
        // the two facts that, together, say which side of the wire lost it.
        static int lastN = -1; static uint8_t lastGen = 0xff;
        if (g_logf && (n != lastN || up.gen != lastGen)) {
            lastN = n; lastGen = up.gen;
            char m[220];
            snprintf(m, sizeof(m), "[drop] peer %d set gen=%u: %d object(s)%s%s", peerIdx,
                     (unsigned)up.gen, n, n ? " -- first is " : "", n ? set[0].id : "");
            g_logf(m);
        }
        // MARK AND SWEEP against the full set: what is in it lives, what is not is gone. This is the
        // one message that may delete, which is why an incomplete set is never applied (dropsync only
        // reports setReady once every part of a generation has landed).
        for (auto& d : s.drop) if (d.used) d.dead = true;
        for (int i = 0; i < n; i++) {
            Slot::DropObj* d = dropAlloc(s, set[i].localId);
            if (!d) break;
            dropSetTarget(*d, set[i], true);
        }
    }
    for (int i = 0; i < up.nPlace; i++) {
        Slot::DropObj* d = dropAlloc(s, up.place[i].localId);
        if (d) dropSetTarget(*d, up.place[i], true);
    }
    for (int i = 0; i < up.nMove; i++) {
        // A move for an id we do not know is not an error and not a spawn: a kMove carries no class
        // name, so there is nothing to spawn FROM. The next set or place brings it, at its current
        // pose, and the drag simply becomes visible a beat later.
        if (Slot::DropObj* d = dropFind(s, up.move[i].localId)) dropSetTarget(*d, up.move[i], false);
    }
    for (int i = 0; i < up.nRemove; i++)
        if (Slot::DropObj* d = dropFind(s, up.remove[i])) d->dead = true;

    // ---- THE LEVEL'S OWN PROPS. A complete layout is only a FLAG here: whether this peer is the
    // one whose layout counts is the frame's decision (it knows who the host is), and the records
    // stay retrievable from dropsync storage until then -- so a layout that arrives before authority
    // resolves is simply picked up late rather than lost.
    if (up.worldSetReady && g_dropPolicy == 2) s.worldSetFresh = true;
    // ...then a pose, from whoever is holding it. WE WIN WHILE WE HOLD IT: a pose for a prop in our
    // own hands would pull it out of them every packet, and both clients would pull against each
    // other for as long as both were dragging. Letting go hands it straight back.
    if (up.haveWorldMove && g_dropPolicy == 2) {
        WorldEntry* w = worldAdd(up.worldMoveName);
        if (g_logf && debug::Get().dropWorld) { static int said = 0; if (said < 20) { said++; char m[200];
            snprintf(m, sizeof(m), "[drop/world] peer %d moved '%s' claim=%d -> %s", peerIdx,
                     up.worldMoveName, (int)up.worldMoveClaim,
                     (!w ? "NO ENTRY (table full)" : w->mine ? "IGNORED, we are holding it" : "accepted"));
            g_logf(m); } }
        if (w && !w->mine) {
            memcpy(w->tgtLoc,  up.worldMoveLoc,  sizeof(w->tgtLoc));
            memcpy(w->tgtQuat, up.worldMoveQuat, sizeof(w->tgtQuat));
            w->driving = true;
            w->holder  = up.worldMoveClaim ? peerIdx : -1;
            w->heldUs  = up.worldMoveClaim ? nowUs : 0;
        }
    }
}

void OnPacket(int peerIdx, const uint8_t* data, int len, uint64_t nowUs) {
    if (!g_cfg.enabled) return;
    // Replay-sync protocol traffic (requests for and chunks of a peer's own state history). Checked
    // first: chunks arrive at hundreds per second mid-transfer.
    if (replaysync::IsSyncPacket(data, len)) {
        replaysync::OnPacket(peerIdx, data, len, nowUs, g_logf);
        return;
    }
    // ---- DROPPED OBJECTS. Routed before the snapshot path like every other lane: the snapshot
    // Unpack is the FALLBACK branch, and an unrecognised magic there trips the once-per-peer
    // "different SessionOpenMP version" warning. Nothing here touches the game -- it only updates the
    // slot's object table, which Frame then turns into spawns, moves and removals.
    if (dropsync::IsPacket(data, len)) {
        if (g_dropPolicy == 0) return;
        dropsync::Update up;
        if (!dropsync::OnPacket(peerIdx, data, len, up)) return;
        Slot* ds = slotFor(peerIdx, nowUs);
        if (!ds) return;
        applyDropUpdate(*ds, up, peerIdx, nowUs);
        return;                                            // NOT a snapshot: no stream push, no liveness
    }
    // The sender's skeleton, by bone name. Rare, and it must be HELD rather than applied here: the
    // pose layer is keyed by the MESH that wears the pose, and their proxy may not exist yet.
    if (repl::IsSkeletonPacket(data, len)) {
        repl::SkelPrint sp;
        if (!repl::UnpackSkeleton(data, len, sp)) return;
        Slot* ss = slotFor(peerIdx, nowUs);
        if (!ss) return;
        const bool changed = !ss->haveSkel || ss->skel.n != sp.n ||
                             memcmp(ss->skel.hash, sp.hash, sizeof(uint32_t) * (size_t)sp.n) != 0;
        ss->skel = sp; ss->haveSkel = true;
        if (changed) {
            ss->skelFedFor = nullptr;                // re-feed: their character was rebuilt
            if (g_logf) { char m[140]; snprintf(m, sizeof(m),
                "[pose] peer %d skeleton: %d bones (poses map by name)", peerIdx, (int)sp.n);
                g_logf(m); }
        }
        return;                                      // NOT a snapshot: no stream push, no liveness
    }
    // Several message types share this transport, routed by magic. Cosmetics are rare and large; the
    // 60 Hz snapshot stays small and self-contained.
    if (repl::IsCosmeticsPacket(data, len)) {
        repl::CosmeticSet c; uint8_t section = 0;
        if (!repl::UnpackCosmetics(data, len, c, &section)) return;
        Slot* cs = slotFor(peerIdx, nowUs);
        if (!cs) return;
        // MERGE by section (clothing and board arrive separately so neither can starve the other), and
        // re-dress only when that section actually changed -- RefreshVisuals is not free and the sender
        // heartbeats these packets.
        repl::CosmeticSet merged = cs->cosmetics;
        merged.stance = c.stance; merged.modDigest = c.modDigest;
        // EVERY per-sender field must be listed here. The merge is deliberately field-selective (so
        // the two sections cannot starve each other), which means a field added to CosmeticSet and to
        // the wire is STILL dropped on the floor until it is named below -- the packet arrives intact
        // and the merged copy keeps its zero.
        memcpy(merged.skaterName, c.skaterName, sizeof(merged.skaterName));
        memcpy(merged.visualDef,  c.visualDef,  sizeof(merged.visualDef));
        memcpy(merged.mapName,    c.mapName,    sizeof(merged.mapName));
        if (section == repl::kCosBoard) { memcpy(merged.brd, c.brd, sizeof(merged.brd)); merged.nBoard = c.nBoard; }
        else                            { memcpy(merged.chr, c.chr, sizeof(merged.chr)); merged.nChar  = c.nChar;  }
        // ---- chat notices. Both ride this packet because it is where the facts live: the NAME
        // arrives only with cosmetics, and the map name travels here too. The join line waits for
        // the name rather than firing on the slot opening -- "somebody joined" with no name helps
        // nobody, and the name lands within a second.
        // Held to the same vouch as the spawn gate: the field case's ghost-lobby knockers sent
        // cosmetics too, and "<name> joined the session" for somebody EOS never placed in our lobby
        // is the same lie as their skater appearing. A slow-vouched real joiner announces on their
        // next cosmetics packet instead (heartbeats every 10 s).
        if (g_cfg.onNotice &&
            (BackendTrust() < TRUST_VOUCHED || PeerTrust(peerIdx) == TRUST_VOUCHED)) {
            char note[160] = {0};
            if (!cs->joinAnnounced && merged.skaterName[0]) {
                cs->joinAnnounced = true;
                char where[64] = {0};
                if (merged.mapName[0]) game::PrettyMapName(merged.mapName, where, sizeof(where));
                if (where[0]) snprintf(note, sizeof(note), "%s joined the session (%s)", merged.skaterName, where);
                else          snprintf(note, sizeof(note), "%s joined the session", merged.skaterName);
            } else if (cs->joinAnnounced && merged.mapName[0] && cs->cosmetics.mapName[0] &&
                       _stricmp(merged.mapName, cs->cosmetics.mapName) != 0) {
                char where[64] = {0};
                game::PrettyMapName(merged.mapName, where, sizeof(where));
                snprintf(note, sizeof(note), "%s went to %s",
                         merged.skaterName[0] ? merged.skaterName : "A player",
                         where[0] ? where : merged.mapName);
            }
            if (note[0]) g_cfg.onNotice(note);
        }
        const bool changed = !cs->haveCosmetics || memcmp(&cs->cosmetics, &merged, sizeof(merged)) != 0;
        cs->cosmetics = merged; cs->haveCosmetics = true;
        if (changed) {
            cs->wornForActor = nullptr;               // a new look: dress again
            // Both halves of a clothes change are now attributable from one log: the SENDER prints
            // "own look changed -> published", the RECEIVER prints this. If a peer changes clothes
            // and only one line appears, that names the side at fault immediately.
            if (g_logf) { char m[120];
                snprintf(m, sizeof(m), "[cosmetics] peer %d changed their look -> re-dressing", peerIdx);
                g_logf(m); }
        }
        return;                                            // NOT a snapshot: no stream push, no liveness
    }
    // ---- CHAT. Also not a snapshot. Dedupe by the sender's own message id: the reliable
    // channel should never deliver one twice, but a line printed twice is a visible defect and the
    // guard is one comparison.
    if (repl::IsChatPacket(data, len)) {
        repl::ChatMsg cm;
        if (!repl::UnpackChat(data, len, cm)) return;
        Slot* cs = slotFor(peerIdx, nowUs);
        if (!cs) return;
        if (cs->haveChatId && cm.id == cs->lastChatId) return;
        cs->lastChatId = cm.id; cs->haveChatId = true;
        if (g_cfg.onChat) g_cfg.onChat(peerIdx, cm.name, cm.text);
        return;
    }
    repl::State s; uint64_t senderUs = 0;
    if (!repl::Unpack(data, len, s, &senderUs)) {          // validated: magic, finiteness, unit quat
        // Dropping this silently is how version skew becomes unexplainable. The lobby join and the
        // P2P link both succeed no matter what build the peer runs; only the snapshot fails to
        // parse. The visible result is a player who connects and is then simply never there, which
        // reads as a broken mod rather than as a mismatch. So the FIRST rejection from a peer says
        // so, once, naming the likely cause. Not per-packet: a mismatched peer rejects 60 a second.
        Slot* bad = slotFor(peerIdx, nowUs);
        if (bad && !bad->rejectedSpoken) {
            bad->rejectedSpoken = true;
            if (g_logf) {
                char m[200];
                snprintf(m, sizeof(m),
                         "[session] peer %d sent %d byte(s) this build cannot read -- almost certainly a "
                         "different SessionOpenMP version. One of you needs to update; until then you "
                         "will not see each other.", peerIdx, len);
                g_logf(m);
            }
            if (g_cfg.onVersionMismatch) g_cfg.onVersionMismatch(peerIdx);
        }
        return;
    }
    Slot* sl = slotFor(peerIdx, nowUs);
    if (!sl) return;
    // Interned names filled in HERE, before the state reaches the stream: named entries teach the
    // cache, nameless ones draw from it, and everything downstream still sees complete states.
    sl->nameCache.Resolve(s);
    sl->stream.Push(s, senderUs, nowUs);
    sl->lastPacketUs = nowUs;
    sl->quietHandled = false;
    g_st.received++;
}

int PeerSlots() { return kMaxPeers; }
bool PeerAt(int slot, char* nameOut, int nameCap, void** proxyActorOut, int* peerIdOut) {
    if (slot < 0 || slot >= kMaxPeers) return false;
    const Slot& s = g_slots[slot];
    if (!s.used) return false;
    if (nameOut && nameCap > 0) {
        // Their own skater name, which cosmetics already carries -- no new wire field for a label.
        // It is a LABEL and nothing else: two players can pick the same one, so nothing may key on it.
        const char* n = (s.haveCosmetics && s.cosmetics.skaterName[0]) ? s.cosmetics.skaterName : "";
        strncpy_s(nameOut, (size_t)nameCap, n, _TRUNCATE);
    }
    // An away peer's actor is reported as NONE: it is hidden in a world they are not in, so a
    // nameplate over it, or a replay camera locked to it, would be pointing at nothing.
    if (proxyActorOut) *proxyActorOut = s.away ? nullptr : s.proxy.actor();
    if (peerIdOut)     *peerIdOut     = s.peerIdx;
    return true;
}

bool PeerMap(int slot, char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (slot < 0 || slot >= kMaxPeers) return false;
    const Slot& s = g_slots[slot];
    if (!s.used || !s.haveCosmetics) return false;
    strncpy_s(out, (size_t)cap, s.cosmetics.mapName, _TRUNCATE);
    return out[0] != 0;
}

// Is this peer composing a chat message? False for quiet peers (a dropped connection must not
// leave "..." hanging) and for away peers (no skater on screen to hang it over).
bool PeerTyping(int peerId) {
    if (peerId < 0) return false;
    for (auto& s : g_slots)
        if (s.used && s.peerIdx == peerId)
            return s.peerTyping && !s.quietHandled && !s.away;
    return false;
}

void* PeerActorById(int peerId) {
    if (peerId < 0) return nullptr;
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerId) return s.away ? nullptr : s.proxy.actor();
    return nullptr;                       // they left, or the slot was reused by somebody else
}

// ---- replay sync (the pause menu's "Sync Replay" row). The setter only records the WISH; the
// request itself is stamped from Frame so it shares Frame's clock epoch. Toggling off cancels the
// transfer and drops any buffer; toggling on after a failure retries from scratch.
bool SetPeerReplaySync(int peerId, bool on) {
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerId) {
        if (!on) {
            s.syncOn = false; s.syncReqSentUs = 0;
            replaysync::CancelSync(peerId);
            return true;
        }
        if (game::LocalReplayMode() != 2) return false;   // the row only means something in playback
        if (replaysync::PeerSyncState(peerId, nullptr) == replaysync::SyncState::Failed)
            replaysync::CancelSync(peerId);               // clear the failure so Frame re-requests
        s.syncOn = true; s.syncReqSentUs = 0;
        return true;
    }
    return false;
}
int PeerReplaySyncState(int peerId) {
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerId) {
        if (!s.syncOn) return 0;
        int pct = 0;
        switch (replaysync::PeerSyncState(peerId, &pct)) {
        case replaysync::SyncState::Transferring: return 1;
        case replaysync::SyncState::Ready:        return 2;
        case replaysync::SyncState::Failed:       return 3;
        default:                                  return s.syncReqSentUs ? 1 : 0;
        }
    }
    return 0;
}

// The per-peer visibility switch for the replay editor, written by the pause menu.
void SetPeerReplayHidden(int peerId, bool hidden) {
    if (!game::Proxy::Tuning().recordPeers) return;
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerId) { s.replayHidden = hidden; return; }
}
bool PeerReplayHidden(void* actor) {
    if (!actor) return false;
    for (auto& s : g_slots) if (s.used && s.proxy.actor() == actor) return s.replayHidden;
    return false;
}
// Playback ended. Any peer hidden from the replay gets three things, in order: their components
// re-registered (they were parked, so the manager's own end-of-playback restore missed them), the
// game's own per-skater transition (being playback-driven zeroes the mesh's GlobalAnimRateScale,
// and only that call reliably puts the whole state back), and their actor unhidden. Then the flags
// reset, so the next replay starts with everyone shown.
void RestoreHiddenAfterReplay(void (*logf)(const char*)) {
    for (auto& s : g_slots) {
        if (!s.used || !s.replayHidden) continue;
        void* actor = s.proxy.actor();
        if (actor) {
            game::spectate::SetPeerShownInReplay(actor, true, logf);
            game::CallSkaterReplayMode(actor, game::LastLiveReplayMode());
        }
        s.replayHidden = false;
    }
}

bool IsProxyActor(void* actor) {
    if (!actor) return false;
    for (auto& s : g_slots) if (s.used && s.proxy.actor() == actor) return true;
    return false;
}

// A world change starts a settle window during which NOTHING is spawned. Loading a level does not
// hand the player their pawn instantly: the game destroys the old one, builds the new level, spawns a
// character and possesses it. Spawning a proxy inside that window put an actor of the LOCAL PLAYER'S
// OWN SKATER CLASS into a world that had not yet chosen a pawn -- and the controller possessed OURS.
// The player then owns a remote peer's skater, standing wherever the proxy was placed, and the level
// they asked for appears not to have loaded at all.
//
// The window is measured from the world change and re-armed by any change of own pawn, so it ends
// only once the game's own pawn has been stable for the whole period. Spawning late costs a peer a
// second of invisibility; spawning early costs the player their character.
static uint64_t g_settleUntilMs = 0;
static void*    g_lastOwnPawn   = nullptr;
static char     g_ownMap[40]    = {0};   // the level WE are in, for the peer-is-elsewhere gate

// Levels where a proxy must never be spawned, matched as a case-insensitive prefix on the world name.
//
// "HUB_" is the apartment. Two players there are not in a shared place -- each is in their own copy
// of it -- so a peer has nothing to show there in the first place, and the transported coordinates
// belong to a different world entirely (one observed spawn landed at 39146,-5319,7097).
//
// It is also actively harmful: spawning a skater in the hub gets it POSSESSED by the local player
// controller within ~20 ms, whenever it happens. That is not a load-timing race and no delay avoids
// it -- the spawn itself is the damage, and the player loses their character to a remote peer's body.
// Not spawning is the only thing that helps.
static const char* kNoProxyWorlds[] = { "HUB_" };

static bool worldTakesProxies(const char* world) {
    if (!world || !world[0]) return true;            // unknown: behave as before rather than block
    for (const char* p : kNoProxyWorlds) {
        const size_t n = strlen(p);
        if (_strnicmp(world, p, n) == 0) return false;
    }
    return true;
}
static const uint64_t kWorldSettleMs = 2000;

void ForgetProxies() {
    for (auto& s : g_slots) if (s.used) { s.proxy.Forget(); s.replayHidden = false; s.away = false; }
    g_settleUntilMs = 0;                 // re-armed by Frame, which owns the clock
    g_lastOwnPawn   = nullptr;
    g_ownMap[0]     = 0;
    // Every dropped object died with the world -- OURS and the ones we spawned for peers -- so the
    // pointers are dropped WITHOUT being touched, and our ids stop meaning anything. Peers' props
    // come back on their next resync beat; the new set they send is for whatever level THEY are in,
    // and the away gate is what keeps it out of ours if that is not this one.
    for (auto& s : g_slots) {
        for (auto& d : s.drop) d = Slot::DropObj();
        s.dropGenSeen = false;
    }
    game::dropper::Forget();
    g_worldN = 0; g_worldOn = false; g_worldSeeded = false;   // the actors died with the level
    g_dropAdopted = false;               // the hidden list went with the world; nothing to restore
    dropNewGeneration("world changed");
    if (g_logf) g_logf("[session] world changed -- all proxy pointers dropped, spawning held until the world settles");
}

// ==== DROPPED OBJECTS =================================================================================
// Our own stable identity, hashed. FNV-1a rather than the string itself so the key is a fixed 8 bytes
// on the wire and one comparison to rank; distinct ids collide with vanishing probability, and a
// collision would cost a session two visible sets rather than anything unsafe. 0 = we do not know who
// we are yet, which is treated as "not in the running" at both ends.
// The PREFERENCE id, not the transport's: it is 128 random bits generated once per install and kept
// in the preferences file, so it is the same key on every backend AND across restarts, where MyId()
// is a lobby-session artefact ("shm:1" is whichever process claimed a mailbox slot first). Which
// player's park is the shared one should not depend on who launched first.
static uint64_t dropOwnAuthKey() {
    const char* id = MpPrefs_PeerId();
    if (!id || !id[0]) id = MyId();                    // before prefs init (headless tests)
    if (!id || !id[0]) return 0;
    uint64_t h = 1469598103934665603ull;
    for (const char* p = id; *p; p++) { h ^= (uint8_t)(unsigned char)*p; h *= 1099511628211ull; }
    return h ? h : 1;                                  // never collide with the "unknown" sentinel
}

static void dropSendToAll(int kind, int nPeers, uint8_t gen,
                          const dropsync::Rec* recs, int n, const uint16_t* ids) {
    PeerStats ps;
    for (int i = 0; i < nPeers; i++) {
        if (!GetStats(i, &ps) || ps.state == 5) continue;
        if (kind == 0) {
            // A set is ALL OR NOTHING (a half-received one applies as "everything else was
            // deleted"), so SendSet refuses rather than truncating -- and a refusal must not be
            // silent, or a big park simply never appears for anyone with no line to explain it.
            if (dropsync::SendSet(i, gen, dropOwnAuthKey(), recs, n, SendBudget()) == 0 && n > 0) {
                static bool said = false;
                if (!said) { said = true; if (g_logf) { char m[200]; snprintf(m, sizeof(m),
                    "[drop] %d objects need more packets than this wire takes at once (budget %d)"
                    " -- peers will not see them", n, SendBudget()); g_logf(m); } }
            }
        }
        else if (kind == 1) { for (int k = 0; k < n; k++) dropsync::SendPlace(i, gen, recs[k]); }
        else if (kind == 2) dropsync::SendMove(i, gen, recs, n);
        else                dropsync::SendRemove(i, gen, ids, n);
    }
}

// Enumerate our own visible set, diff it against the baseline, and publish what changed. This is the
// whole sender: place, duplicate, drag, stick-to-ground, revert and call-back are all just an added,
// removed or moved entry, so none of them needs its own hook.
static void dropPublishOwn(uint64_t nowUs, int nPeers, bool forceResync, bool includeBaseline) {
    using namespace game;
    // STATIC, not stack: 256 objects across five buffers is ~100 KB, which is a lot to put on the
    // game thread's frame several times a second. Safe because everything here runs on that one
    // thread, like the rest of the session.
    static dropper::ObjRec recs[dropper::kMaxObjects];
    static void*           actors[dropper::kMaxObjects];
    int n = dropper::EnumerateOwn(recs, actors, dropper::kMaxObjects);
    // The publish filter (see dropFrame): pre-session objects travel only for the canonical set.
    // Filtered here, before the diff, so an excluded object never enters the baseline table at all --
    // to the wire it simply does not exist, and when canonical status flips the resync that the flip
    // forces is what introduces (or withdraws) them wholesale.
    if (!includeBaseline) {
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (dropper::IsPreSession(actors[i])) continue;
            if (w != i) { recs[w] = recs[i]; actors[w] = actors[i]; }
            w++;
        }
        n = w;
    }

    static dropsync::Rec place[dropper::kMaxObjects]; int nPlace = 0;
    static dropsync::Rec move [dropper::kMaxObjects]; int nMove  = 0;
    static uint16_t      gone [dropper::kMaxObjects]; int nGone  = 0;

    for (int i = 0; i < g_ownDropN; i++) g_ownDrop[i].seen = false;
    for (int i = 0; i < n; i++) {
        OwnDrop* e = nullptr;
        for (int k = 0; k < g_ownDropN; k++) if (g_ownDrop[k].actor == actors[i]) { e = &g_ownDrop[k]; break; }
        if (!e) {
            if (g_ownDropN >= dropper::kMaxObjects) continue;
            e = &g_ownDrop[g_ownDropN++];
            *e = OwnDrop();
            e->actor = actors[i];
            e->id = g_dropNextId++;
            strncpy_s(e->cls, sizeof(e->cls), recs[i].id, _TRUNCATE);
            memcpy(e->loc, recs[i].loc, sizeof(e->loc));
            memcpy(e->quat, recs[i].quat, sizeof(e->quat));
            e->seen = true;
            dropsync::Rec& r = place[nPlace++];
            strncpy_s(r.id, sizeof(r.id), e->cls, _TRUNCATE);
            r.localId = e->id;
            memcpy(r.loc, e->loc, sizeof(r.loc)); memcpy(r.quat, e->quat, sizeof(r.quat));
            continue;
        }
        e->seen = true;
        const float dx = recs[i].loc[0]-e->loc[0], dy = recs[i].loc[1]-e->loc[1], dz = recs[i].loc[2]-e->loc[2];
        float qd = 0; for (int k = 0; k < 4; k++) { const float d = recs[i].quat[k]-e->quat[k]; qd += d*d; }
        const float eps = dropper::g_tun.moveEpsCm;
        if (dx*dx + dy*dy + dz*dz > eps*eps || qd > dropper::g_tun.moveEpsQuat * dropper::g_tun.moveEpsQuat) {
            memcpy(e->loc, recs[i].loc, sizeof(e->loc));
            memcpy(e->quat, recs[i].quat, sizeof(e->quat));
            dropsync::Rec& r = move[nMove++];
            r.id[0] = 0; r.localId = e->id;
            memcpy(r.loc, e->loc, sizeof(r.loc)); memcpy(r.quat, e->quat, sizeof(r.quat));
        }
    }
    // Compact out what is gone. In place and order-preserving, so the baseline never grows holes.
    int w = 0;
    for (int k = 0; k < g_ownDropN; k++) {
        if (g_ownDrop[k].seen) { if (w != k) g_ownDrop[w] = g_ownDrop[k]; w++; }
        else if (nGone < dropper::kMaxObjects) gone[nGone++] = g_ownDrop[k].id;
    }
    g_ownDropN = w;

    if (nPeers <= 0) return;
    if (forceResync) {
        // The set supersedes every delta this pass would have sent -- it IS the current state.
        static dropsync::Rec full[dropper::kMaxObjects];
        for (int k = 0; k < g_ownDropN; k++) {
            strncpy_s(full[k].id, sizeof(full[k].id), g_ownDrop[k].cls, _TRUNCATE);
            full[k].localId = g_ownDrop[k].id;
            memcpy(full[k].loc, g_ownDrop[k].loc, sizeof(full[k].loc));
            memcpy(full[k].quat, g_ownDrop[k].quat, sizeof(full[k].quat));
        }
        // Say WHAT is being published, not just how much. The class name is the id the receiver looks
        // up, so seeing the actual strings is what distinguishes "we published nothing" from "we
        // published something the other end could not resolve". Only on a change, so the 6 s
        // heartbeat does not become a log flood.
        static int lastSaidN = -1; static uint8_t lastSaidGen = 0xff;
        if (g_logf && (g_ownDropN != lastSaidN || g_dropGen != lastSaidGen)) {
            lastSaidN = g_ownDropN; lastSaidGen = g_dropGen;
            char names[220] = {0};
            for (int k = 0; k < g_ownDropN && k < 6; k++) {
                if (names[0]) strncat_s(names, ", ", _TRUNCATE);
                strncat_s(names, g_ownDrop[k].cls, _TRUNCATE);
            }
            char m[320];
            snprintf(m, sizeof(m), "[drop] publishing set gen=%u: %d object(s) to %d peer(s)%s%s",
                     (unsigned)g_dropGen, g_ownDropN, nPeers, g_ownDropN ? " -- " : "", names);
            g_logf(m);
        }
        dropSendToAll(0, nPeers, g_dropGen, full, g_ownDropN, nullptr);
        return;
    }
    if (nPlace) dropSendToAll(1, nPeers, g_dropGen, place, nPlace, nullptr);
    if (nMove)  dropSendToAll(2, nPeers, g_dropGen, move,  nMove,  nullptr);
    if (nGone)  dropSendToAll(3, nPeers, g_dropGen, nullptr, nGone, gone);
}

// Apply a complete layout from the host: every named prop's target becomes the host's pose, and any
// prop OUR OWN save moved that the layout has no opinion about steps back onto its map default -- the
// joiner's arrangement giving way to the host's, which is what "join someone's game and skate their
// spot" means for the furniture.
static void worldApplyHostSet(const dropsync::WorldRec* recs, int n) {
    using namespace game;
    for (int i = 0; i < g_worldN; i++) g_world[i].inSet = false;
    for (int i = 0; i < n; i++) {
        WorldEntry* w = worldAdd(recs[i].name);
        if (!w) continue;
        w->inSet = true;
        if (w->mine) continue;                       // never yank a prop out of the local player's hands
        memcpy(w->tgtLoc,  recs[i].loc,  sizeof(w->tgtLoc));
        memcpy(w->tgtQuat, recs[i].quat, sizeof(w->tgtQuat));
        w->driving = true;
    }
    // Our own moved props the host does not mention. The map default is known for exactly these: the
    // Load seam captured it when our save moved them, and first-sight capture covers ones we dragged
    // around during play.
    static char names[dropsync::kMaxWorldRecords][64];
    const int nOwn = dropper::MovedWorldNames(names, dropsync::kMaxWorldRecords);
    for (int i = 0; i < nOwn; i++) {
        WorldEntry* w = worldFind(names[i]);
        if (w && w->inSet) continue;
        void* actor = dropper::WorldTouch(names[i]);
        if (!actor) continue;
        float dl[3], dq[4];
        if (!dropper::MapDefaultOf(actor, dl, dq)) continue;   // unknowable: leave it where it stands
        w = worldAdd(names[i]);
        if (!w || w->mine) continue;
        memcpy(w->tgtLoc, dl, sizeof(w->tgtLoc));
        memcpy(w->tgtQuat, dq, sizeof(w->tgtQuat));
        w->driving = true; w->inSet = true;
    }
    if (g_logf) {
        static int lastN = -1;
        if (n != lastN) {
            lastN = n;
            char m[180];
            snprintf(m, sizeof(m), "[drop/world] the host's layout: %d prop(s)%s", n,
                     nOwn ? " (own extras step back to their map defaults)" : "");
            g_logf(m);
        }
    }
}

// ---- THE LEVEL'S OWN PROPS, once per frame ---------------------------------------------------------
// The host's arrangement is the session layout; every client moves its OWN real actors to it, and
// exactly one client speaks for a prop afterwards -- whoever is holding it. A resting prop is
// published by nobody except the host's slow resync FROM ITS TABLE, which is what carries the layout
// to late joiners without ever echoing a chased pose.
static void worldFrame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, int nPeers, bool sessionLive,
                       bool forceResync, float dt, bool iAmCanonical, int canonicalPeer,
                       bool authoritySettled) {
    using namespace game;
    // World sharing is PART OF "Share one set" -- the whole point is one arrangement, the host's.
    if (g_dropPolicy != 2) sessionLive = false;
    if (!sessionLive) { if (g_worldOn) worldTearDown(); return; }
    if (!ownPawn || nowMs < g_settleUntilMs) return;

    // THE SOURCE CHANGED. A migration (or the first resolution) means the layout everyone shows is a
    // different player's arrangement now: put our own world back first, then reseed or reapply from
    // the new source. The held-canonical rule upstream keeps this from firing during the gap itself.
    {
        static bool haveLast = false; static bool lastIAm = false; static int lastPeer = -2;
        if (!haveLast || iAmCanonical != lastIAm || (!iAmCanonical && canonicalPeer != lastPeer)) {
            if (haveLast && g_worldOn) {
                worldTearDown();
                if (g_logf) g_logf("[drop/world] the layout's owner changed -- reverting, then taking the new one");
            }
            haveLast = true; lastIAm = iAmCanonical; lastPeer = canonicalPeer;
        }
    }
    if (!authoritySettled) return;                    // a guess is not a layout owner
    g_worldOn = true;

    if (iAmCanonical) {
        // HOSTING: my arrangement IS the layout. Seeded once, from where my props stand right now --
        // which at rest is exactly where my save put them, so this is data my save told me, not a
        // chased pose read back mid-flight.
        if (!g_worldSeeded) {
            g_worldSeeded = true;
            static char names[dropsync::kMaxWorldRecords][64];
            const int n = dropper::MovedWorldNames(names, dropsync::kMaxWorldRecords);
            for (int i = 0; i < n; i++) {
                void* actor = dropper::WorldTouch(names[i]);
                if (!actor) continue;
                float loc[3], quat[4];
                if (!dropper::ActorPose(actor, loc, quat)) continue;
                WorldEntry* w = worldAdd(names[i]);
                if (!w) continue;
                memcpy(w->tgtLoc, loc, sizeof(w->tgtLoc));
                memcpy(w->tgtQuat, quat, sizeof(w->tgtQuat));
                memcpy(w->curLoc, loc, sizeof(w->curLoc));
                memcpy(w->curQuat, quat, sizeof(w->curQuat));
                memcpy(w->lastSentLoc, loc, sizeof(w->lastSentLoc));
                memcpy(w->lastSentQuat, quat, sizeof(w->lastSentQuat));
                w->seeded = true;
            }
            // The table size rides along because "hosting: 0" has two very different meanings --
            // an arrangement that IS the map default, and a map-default table that never got
            // filled. One line tells them apart without another field round.
            if (g_logf) { char m[180]; snprintf(m, sizeof(m),
                "[drop/world] hosting the world layout: %d moved prop(s) (map-default table knows"
                " %d, map '%s')", g_worldN, dropper::MapDefaultCount(),
                g_ownMap[0] ? g_ownMap : "?"); g_logf(m); }
        }
        // ...and republished from the TABLE on the resync beat, so late joiners get the layout as it
        // stands -- including props peers moved and released, whose poses were folded in on receive.
        if (forceResync && nPeers > 0) {
            static dropsync::WorldRec recs[dropsync::kMaxWorldRecords];
            for (int i = 0; i < g_worldN; i++) {
                strncpy_s(recs[i].name, sizeof(recs[i].name), g_world[i].name, _TRUNCATE);
                memcpy(recs[i].loc,  g_world[i].tgtLoc,  sizeof(recs[i].loc));
                memcpy(recs[i].quat, g_world[i].tgtQuat, sizeof(recs[i].quat));
            }
            PeerStats ps;
            for (int p = 0; p < nPeers; p++)
                if (GetStats(p, &ps) && ps.state != 5)
                    dropsync::SendWorldSet(p, g_dropGen, g_ownMap, recs, g_worldN, SendBudget());
        }
    } else {
        // JOINED: the canonical peer's COMPLETE layout, whenever a fresh one has assembled -- or on
        // the frame the canonical peer finally resolved, the one already sitting in dropsync storage.
        Slot* cs = nullptr;
        for (auto& s : g_slots) if (s.used && s.peerIdx == canonicalPeer) { cs = &s; break; }
        static int appliedFromPeer = -2;
        const bool sourceIsNew = (appliedFromPeer != canonicalPeer);
        if (cs && (cs->worldSetFresh || sourceIsNew)) {
            int n = 0;
            const char* forMap = nullptr;
            const dropsync::WorldRec* recs = dropsync::WorldSetRecords(canonicalPeer, &n, &forMap);
            if (recs) {                              // null = nothing assembled yet: stay patient
                cs->worldSetFresh = false;
                // A LAYOUT IS FOR ONE MAP. The host mid-travel (or standing in another level
                // entirely) legitimately publishes a layout that has nothing to do with our world --
                // and an EMPTY one is not harmless there: it reads as "revert your furniture to its
                // defaults". Refused on any mismatch; the 6-second beat re-delivers once the maps
                // agree. An empty map string on EITHER side counts as a mismatch: "unknown" is not
                // "the same place".
                if (!forMap || !forMap[0] || !g_ownMap[0] || _stricmp(forMap, g_ownMap) != 0) {
                    static int said = 0;
                    if (g_logf && said < 8) { said++; char m[200];
                        snprintf(m, sizeof(m), "[drop/world] holding the host's layout: it is for"
                                 " '%s', we are on '%s'", (forMap && forMap[0]) ? forMap : "?",
                                 g_ownMap[0] ? g_ownMap : "?");
                        g_logf(m); }
                } else {
                    appliedFromPeer = canonicalPeer;
                    worldApplyHostSet(recs, n);
                }
            }
        }
        if (sourceIsNew && !cs) appliedFromPeer = -2;
    }

    // EVERYONE, host included: claims, publishing our own holds, driving everything else.
    for (int i = 0; i < g_worldN; i++) {
        WorldEntry& w = g_world[i];
        void* actor = dropper::WorldTouch(w.name);   // resolves once, remembers the original once
        static int traced = 0;
        auto trace = [&](const char* what) {
            if (traced >= 40 || !g_logf || !debug::Get().dropWorld) return;
            traced++;
            char m[220];
            snprintf(m, sizeof(m), "[drop/world] '%s' %s (actor=%p mine=%d driving=%d holder=%d)",
                     w.name, what, actor, (int)w.mine, (int)w.driving, w.holder);
            g_logf(m);
        };
        if (!actor) { trace("NOT IN THIS MAP -- cannot be moved or driven"); continue; }
        if (!w.seeded) {
            float loc[3], quat[4];
            if (dropper::ActorPose(actor, loc, quat)) {
                memcpy(w.curLoc, loc, sizeof(w.curLoc));
                memcpy(w.curQuat, quat, sizeof(w.curQuat));
                memcpy(w.lastSentLoc, loc, sizeof(w.lastSentLoc));
                memcpy(w.lastSentQuat, quat, sizeof(w.lastSentQuat));
                w.seeded = true;
            }
        }

        // ARE WE HOLDING IT? Picking a prop up is a discrete, observable event, which is why the claim
        // is keyed on it rather than inferred from poses that disagree -- inference is what turned
        // two clients into a tug of war last time.
        const bool held = dropper::IsSelectedLocally(actor);
        if (held && !w.mine) { w.mine = true; w.holder = -1; trace("CLAIMED locally"); }
        if (w.mine) {
            float loc[3], quat[4];
            if (dropper::ActorPose(actor, loc, quat)) {
                const float dx = loc[0]-w.lastSentLoc[0], dy = loc[1]-w.lastSentLoc[1], dz = loc[2]-w.lastSentLoc[2];
                float qd = 0; for (int k = 0; k < 4; k++) { const float d = quat[k]-w.lastSentQuat[k]; qd += d*d; }
                const float eps = dropper::g_tun.moveEpsCm;
                const bool moved = dx*dx + dy*dy + dz*dz > eps*eps ||
                                   qd > dropper::g_tun.moveEpsQuat * dropper::g_tun.moveEpsQuat;
                // The RELEASE always goes, moved or not: it is the packet that says "it is yours
                // again", and it is the one that has to be reliable.
                if (moved || !held) {
                    memcpy(w.lastSentLoc, loc, sizeof(w.lastSentLoc));
                    memcpy(w.lastSentQuat, quat, sizeof(w.lastSentQuat));
                    memcpy(w.tgtLoc, loc, sizeof(w.tgtLoc));
                    memcpy(w.tgtQuat, quat, sizeof(w.tgtQuat));
                    memcpy(w.curLoc, loc, sizeof(w.curLoc));
                    memcpy(w.curQuat, quat, sizeof(w.curQuat));
                    int sentTo = 0;
                    PeerStats ps;
                    for (int p = 0; p < nPeers; p++)
                        if (GetStats(p, &ps) && ps.state != 5) {
                            dropsync::SendWorldMove(p, g_dropGen, w.name, loc, quat, held);
                            sentTo++;
                        }
                    if (!held || sentTo == 0) trace(sentTo ? "published RELEASE" : "moved but NOBODY to send to");
                }
            }
            if (!held) { w.mine = false; w.driving = false; }   // let go: ours to publish no longer
            continue;                                            // never drive a prop we are holding
        }
        // A holder who goes silent releases it, so a disconnect mid-drag cannot leave a prop locked
        // to somebody who is gone.
        if (w.holder >= 0 && sinceUs(nowUs, w.heldUs) > kWorldClaimStaleUs) { w.holder = -1; w.driving = false; }
        if (w.driving) {
            static void* lastDriven = nullptr;
            if (lastDriven != actor) { lastDriven = actor; trace("DRIVING toward the session pose"); }
            dropper::DriveRemote(actor, w.tgtLoc, w.tgtQuat, w.curLoc, w.curQuat, dt);
        }
    }
    g_st.dropWorld = g_worldN;
}

static void dropFrame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, int nPeers, uint64_t dropUs) {
    using namespace game;
    static uint64_t lastFrameUs = 0, lastEnumUs = 0, lastResyncUs = 0;
    const float dt = lastFrameUs ? (float)((double)sinceUs(nowUs, lastFrameUs) / 1.0e6) : 0.f;
    lastFrameUs = nowUs;

    // The player's own setting is the source of truth, read every frame rather than copied at
    // startup, so a change from either menu takes effect on the next tick with no apply step.
    {
        const uint8_t pol = (uint8_t)MpPrefs_DropMode();
        if (pol != g_dropPolicy) {
            g_dropPolicy = pol;
            g_dropResend = true;                 // what we contribute just changed
            if (g_logf) { char m[120]; snprintf(m, sizeof(m), "[drop] policy -> %s",
                          pol == 0 ? "Off" : pol == 1 ? "Live only" : "Shared"); g_logf(m); }
        }
    }
    const bool wanted = (g_dropPolicy != 0);
    if (!wanted || !dropper::Available() || !dropper::Manager()) {
        // Turned off, unavailable, or no manager in this level yet: leave the world exactly as we
        // found it. Both of these are no-ops when there is nothing to undo.
        if (g_dropAdopted || dropper::RemoteCount() > 0) {
            for (auto& s : g_slots) for (auto& d : s.drop) d = Slot::DropObj();
            dropper::ResetAll(g_logf);      // also puts the level's own props back
            g_dropAdopted = false;
        }
        if (wanted && !dropper::Available()) {
            static bool said = false;
            if (!said) { said = true; if (g_logf)
                g_logf("[drop] object-dropper symbols unavailable -- dropped objects will not sync"); }
        }
        if (g_worldOn) worldTearDown();
        g_st.dropOwn = 0; g_st.dropRemote = 0; g_st.dropWorld = 0;
        return;
    }

    // ---- WHO IS CANONICAL. Lowest authority key among us and every peer who is present and has told
    // us theirs. A peer we cannot rank (no set yet, or the feature off on their side) is simply not in
    // the running, so the worst case while a session settles is that both sets are briefly visible --
    // never that somebody's park vanishes because of a key we guessed at.
    // THE HOST OWNS THE SHARED WORLD, whenever the wire can say who that is. Ranking by key alone was
    // deterministic but arbitrary: the same player won every session regardless of who hosted, so
    // whoever happened to hold the lower key always kept their park and the other player always lost
    // theirs -- in both directions, which is exactly backwards from "join someone's game and skate
    // THEIR spot". The lowest key stays as the fallback for wires with no host concept (shared memory
    // is symmetric by construction, and EOS before the owner has resolved), where it is still the
    // right answer: deterministic, identical on both machines, and needing no agreement protocol.
    const uint64_t myKey = dropOwnAuthKey();
    const char* ownerId = LobbyOwnerId();
    const char* myWireId = MyId();
    const bool hostKnown = ownerId && ownerId[0] && myWireId && myWireId[0];
    bool anyPresent = false, iAmCanonical = true;
    int  rankedAgainst = 0, canonicalPeer = -1;
    uint64_t lowestKey = myKey;
    for (auto& s : g_slots) {
        if (!s.used || s.away || sinceUs(nowUs, s.lastPacketUs) >= dropUs) continue;
        anyPresent = true;
        if (hostKnown) {
            const char* theirId = PeerIdStr(s.peerIdx);
            if (theirId && theirId[0] && _stricmp(theirId, ownerId) == 0) canonicalPeer = s.peerIdx;
            continue;
        }
        if (!s.haveDropAuth || !s.dropAuthKey || !myKey) continue;
        rankedAgainst++;
        if (s.dropAuthKey < lowestKey) { lowestKey = s.dropAuthKey; canonicalPeer = s.peerIdx; iAmCanonical = false; }
    }
    if (hostKnown) iAmCanonical = (_stricmp(ownerId, myWireId) == 0);
    // HOST MIGRATION. When the host walks out, EOS hands the lobby to somebody else -- but for a
    // moment in between nobody owns it, and falling back to the key rule for those few frames would
    // flip the canonical player twice: everyone's park would vanish and come back for no reason a
    // player could see. So once a host has been known in this session, the last decision is HELD
    // through the gap and only a newly resolved owner can change it.
    {
        static bool everHadHost = false, heldCanonical = true;
        if (hostKnown) { everHadHost = true; heldCanonical = iAmCanonical; }
        else if (everHadHost && anyPresent) iAmCanonical = heldCanonical;
        if (!anyPresent) everHadHost = false;            // session over: start clean next time
    }
    // Is the answer above WORTH ACTING ON YET? Straight after joining, no host has resolved and no
    // peer key has arrived, so the default "canonical = me" is a guess, not a decision. And on a
    // wire that HAS an owner concept, a peer key arriving first must not settle it either: EOS
    // resolves the owner within moments of a join, and letting the key rule bridge that window
    // handed the world layout to whichever player held the lower key -- both field logs show the
    // JOINER seeding the layout and the real host adopting it. The key rule is for wires that will
    // NEVER know (shared memory, UDP), not ones that do not know yet.
    const bool authoritySettled = hostKnown || (rankedAgainst > 0 && !LobbyOwnershipKnowable())
                                || !anyPresent;
    // THE DECISION, said out loud whenever it changes. Without this, "nobody adopted" and "everybody
    // thought they were canonical" look identical from a log -- and the second one is the failure
    // where two peers' keys never reached each other, which is a wire problem, not a policy one.
    {
        static int lastRanked = -1, lastCanon = -2; static bool lastMine = true;
        if (g_logf && (rankedAgainst != lastRanked || canonicalPeer != lastCanon || iAmCanonical != lastMine)) {
            lastRanked = rankedAgainst; lastCanon = canonicalPeer; lastMine = iAmCanonical;
            char m[240];
            if (hostKnown)
                snprintf(m, sizeof(m), "[drop] canonical set: %s -- the session HOST%s",
                         iAmCanonical ? "MINE" : "a peer's",
                         (!iAmCanonical && canonicalPeer < 0)
                             ? " (who is not on our peer list yet)" : "");
            else
                snprintf(m, sizeof(m), "[drop] canonical set: %s -- no host on this wire, so lowest key"
                         " wins (mine %016llX, ranked against %d)", iAmCanonical ? "MINE" : "a peer's",
                         (unsigned long long)myKey, rankedAgainst);
            g_logf(m);
        }
    }
    // ---- THE PRE-SESSION BASELINE. Taken at the moment the first peer appears -- before anything
    // this frame publishes -- and cleared when the session empties. It is the single list that
    // decides both what adoption hides and what the publish below is allowed to share.
    {
        static bool hadPresent = false;
        if (anyPresent && !hadPresent) dropper::SnapshotPreSession();
        if (!anyPresent && hadPresent) dropper::ClearPreSession();
        hadPresent = anyPresent;
    }

    // ---- ADOPTION. Only under the SHARED policy, only with somebody actually here, and only when
    // somebody else is canonical. Visual + collision, never a save.
    const bool wantAdopt = (g_dropPolicy == 2) && anyPresent && !iAmCanonical;
    if (wantAdopt != g_dropAdopted) {
        if (wantAdopt) dropper::HideOwnSet(g_logf);
        else           dropper::RestoreOwnSet(g_logf);
        g_dropAdopted = wantAdopt;
        g_dropResend = true;                     // what we contribute just changed
    }

    // ---- WHOSE SAVED PARK TRAVELS. The baseline goes on the wire only while WE are the canonical
    // set under Shared AND that answer is settled: before it settles, iAmCanonical is a default, not
    // a decision, and publishing on it is exactly the joiner's-park-flash the field reported.
    // Live-only never shares anybody's saved park. Post-session placements always travel. A flip in
    // either direction is a content change: full resync.
    const bool includeBaseline = (g_dropPolicy == 2) && iAmCanonical && authoritySettled;
    {
        static bool last = false; static bool haveLastIB = false;
        if (!haveLastIB || includeBaseline != last) { haveLastIB = true; last = includeBaseline; g_dropResend = true; }
    }

    // ---- KEEP THE ADOPTION HIDDEN. The replay editor's exit pass un-hides everything it has
    // registered -- the adopted-away park included, which then stayed visible to its owner for the
    // rest of the session (field-logged). Re-asserted on a slow beat for as long as adoption stands.
    if (g_dropAdopted) {
        static uint64_t lastReassertUs = 0;
        if (!lastReassertUs || sinceUs(nowUs, lastReassertUs) > 1000000ull) {
            lastReassertUs = nowUs;
            dropper::ReassertOwnSetHidden();
        }
    }

    // ---- PUBLISH. Every frame while the local player is actually in the dropper (that is when
    // objects move); a slow poll otherwise, because a set that nobody is editing cannot change.
    const bool active = dropper::LocalActive();
    const uint64_t enumPeriod = active ? 33000ull : 250000ull;
    const bool resyncDue = g_dropResend || !lastResyncUs || sinceUs(nowUs, lastResyncUs) > 6000000ull;
    if (!lastEnumUs || sinceUs(nowUs, lastEnumUs) >= enumPeriod || resyncDue) {
        lastEnumUs = nowUs;
        dropPublishOwn(nowUs, nPeers, resyncDue, includeBaseline);
        if (resyncDue) { lastResyncUs = nowUs; g_dropResend = false; }
    }

    // ---- THE LEVEL'S OWN PROPS. Its own lane, and the host's to define.
    worldFrame(ownPawn, nowUs, nowMs, nPeers, anyPresent, resyncDue, dt,
               iAmCanonical, canonicalPeer, authoritySettled);

    // ---- APPLY. Every game call for peers' objects happens here, on the engine-tick anchor the
    // proxy spawn already rides -- never inside the packet pump.
    int liveRemote = 0;
    for (auto& s : g_slots) {
        if (!s.used) continue;
        // A peer in ANOTHER level: their props describe a world that is not ours, exactly like their
        // body position does. Their table is kept (nothing is forgotten), the actors are not.
        const bool elsewhere = s.away;
        for (auto& d : s.drop) {
            if (!d.used) continue;
            if (d.dead || elsewhere) {
                if (d.actor) dropper::DestroyRemote(d.actor, g_logf);
                d.actor = nullptr;
                if (d.dead) d = Slot::DropObj();
                continue;
            }
            if (!d.actor) {
                if (d.spawnFailed || !ownPawn || nowMs < g_settleUntilMs) continue;
                {
                    dropper::ObjRec r;
                    strncpy_s(r.id, sizeof(r.id), d.cls, _TRUNCATE);
                    memcpy(r.loc, d.tgtLoc, sizeof(r.loc)); memcpy(r.quat, d.tgtQuat, sizeof(r.quat));
                    d.actor = dropper::SpawnRemote(ownPawn, r, g_logf);
                    if (!d.actor) {
                        // Not installed here, or the world-wide cap is full. Either way this record is
                        // settled: retrying every frame would be a spawn storm behind a one-line log.
                        d.spawnFailed = true;
                        if (dropper::RemoteCount() >= dropper::kMaxObjects && !g_dropSpawnCapSaid) {
                            g_dropSpawnCapSaid = 1;
                            if (g_logf) { char m[180]; snprintf(m, sizeof(m),
                                "[drop] %d peer objects is the cap -- later ones are not being spawned",
                                dropper::kMaxObjects); g_logf(m); }
                        }
                        continue;
                    }
                }
                memcpy(d.curLoc, d.tgtLoc, sizeof(d.curLoc));
                memcpy(d.curQuat, d.tgtQuat, sizeof(d.curQuat));
                d.seeded = true;
            }
            if (!d.seeded) {
                memcpy(d.curLoc, d.tgtLoc, sizeof(d.curLoc));
                memcpy(d.curQuat, d.tgtQuat, sizeof(d.curQuat));
                d.seeded = true;
            }
            dropper::DriveRemote(d.actor, d.tgtLoc, d.tgtQuat, d.curLoc, d.curQuat, dt);
            liveRemote++;
        }
    }
    g_st.dropOwn = g_ownDropN;
    g_st.dropRemote = liveRemote;
}

// =====================================================================================================
// THE BONE FLOOR -- a stopgap, not a fix. See the note below.
// =====================================================================================================
// MEASURED RULE (many headset rounds): a proxy whose merged skeleton has FEWER bones than the local
// character corrupts memory on the machine that owns that local character, and the fatal surfaces on
// the next big allocation -- entering the replay editor, reliably. Equal counts never crash. MORE
// bones on the proxy never crashes. Bone NAMES are irrelevant: two players in DIFFERENT 95-bone
// garments are fine, so this is an index-and-size fault, not a layout one.
//
// The real defect is somewhere in how a smaller skeleton is indexed with a larger character's count,
// and it has NOT been found -- five mechanisms were tried and each was disproven in the headset
// (replay-set membership three ways, the merged-mesh pool, the pose seam, our own component prune,
// and render-state recreation, which made it fire SOONER). Until it is found, this keeps the fault
// out of reach by making sure no proxy is ever short: if a peer's outfit leaves their proxy below our
// bone count, one of their garments is swapped for OURS in the same slot, which is guaranteed
// installed locally and produces our own count.
//
// The cost is honest and visible: that one item looks wrong on that peer, on our screen only. Their
// other clothing, their board and everyone else's look are untouched. Categories are tried ONE AT A
// TIME and the first that lifts the count wins, so the usual case (a single garment carrying a rig)
// swaps exactly one item. Cumulative substitution is the fallback when no single swap is enough.
static repl::CosmeticSet g_ownLook;
static bool              g_haveOwnLook = false;

static int proxyBones(void* actor) { return game::SkeletonBoneCount(game::SkaterMeshOf(actor)); }

// Copy OUR item for `cat` over theirs. False = we have nothing in that slot, so nothing to lend.
static bool borrowOurItem(repl::CosmeticSet& set, int32_t cat) {
    for (int o = 0; o < (int)g_ownLook.nChar; o++) {
        if (g_ownLook.chr[o].cat != cat) continue;
        for (int i = 0; i < (int)set.nChar; i++) {
            if (set.chr[i].cat != cat) continue;
            set.chr[i] = g_ownLook.chr[o];
            return true;
        }
    }
    return false;
}

static void EnforceBoneFloor(void* proxyActor, const repl::CosmeticSet& theirs, void* ownPawn) {
    if (!proxyActor || !ownPawn || !g_haveOwnLook) return;
    const int mine = game::SkeletonBoneCount(game::SkaterMeshOf(ownPawn));
    int have = proxyBones(proxyActor);
    if (mine <= 0 || have <= 0 || have >= mine) return;      // 0 = unreadable: change nothing

    // ---- try each of their clothing categories alone; first one that lifts the count wins.
    int unresolved = 0;
    for (int i = 0; i < (int)theirs.nChar; i++) {
        repl::CosmeticSet trial = theirs;
        if (!borrowOurItem(trial, theirs.chr[i].cat)) continue;
        game::DressProxy(proxyActor, trial, &unresolved, g_logf);
        const int wasShort = have;                  // the count that TRIGGERED this, not the result
        have = proxyBones(proxyActor);
        if (have >= mine) {
            if (g_logf) { char m[240]; snprintf(m, sizeof(m),
                "[cosmetics] bone floor: peer's '%s' swapped for ours -- their proxy came out %d "
                "bone(s) against our %d, which crashes the replay editor; now %d. Only that item "
                "looks wrong.",
                theirs.chr[i].name[0] ? theirs.chr[i].name : "?", wasShort, mine, have); g_logf(m); }
            return;
        }
    }
    // ---- no single swap was enough: take ours cumulatively until the count is met.
    repl::CosmeticSet trial = theirs;
    for (int i = 0; i < (int)theirs.nChar; i++) {
        if (!borrowOurItem(trial, theirs.chr[i].cat)) continue;
        game::DressProxy(proxyActor, trial, &unresolved, g_logf);
        have = proxyBones(proxyActor);
        if (have >= mine) break;
    }
    if (g_logf) { char m[200]; snprintf(m, sizeof(m),
        "[cosmetics] bone floor: needed several of our garments -- proxy now %d bone(s) against our %d%s",
        have, mine, have >= mine ? "" : " (STILL SHORT -- the replay editor may crash)"); g_logf(m); }
}

void Frame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, GatherFn gatherOwn) {
    game::pose::SetLogger(g_logf);           // idempotent; the stale-pose line needs a voice
    if (!g_cfg.enabled) return;
    // The bone floor's substitutions are judged against OUR merged bone count, and nothing else
    // re-judges them when WE change clothes: a peer kept wearing our lent shirt after we no longer
    // wore a rig-carrying garment (field-reported as a wrongly dressed skater), and the reverse --
    // changing INTO one -- leaves every proxy short until its next look change, which is exactly the
    // crash window the floor exists to close. So our merged count changing re-dresses everyone.
    // Cheap: an integer read per frame, and the rebuilds only happen on a wardrobe-change cadence.
    {
        static int lastMine = 0;
        const int mine = ownPawn ? game::SkeletonBoneCount(game::SkaterMeshOf(ownPawn)) : 0;
        if (mine > 0 && lastMine > 0 && mine != lastMine) {
            for (auto& sl : g_slots) if (sl.used) sl.wornForActor = nullptr;
            if (g_logf) { char m[170]; snprintf(m, sizeof(m),
                "[cosmetics] our skeleton changed (%d -> %d bones) -- re-dressing every peer so the"
                " bone floor can re-judge its substitutions", lastMine, mine); g_logf(m); }
        }
        if (mine > 0) lastMine = mine;
    }
    // Both thresholds live in microseconds here so they are compared against nowUs and nothing else.
    // `nowMs` is passed through to the PROXY only (its own spawn/visual timers are self-consistent).
    const uint64_t quietUs = (uint64_t)(g_cfg.quietMs * 1000.0f);
    const uint64_t dropUs  = (uint64_t)(g_cfg.dropMs  * 1000.0f);

    // ---- replay sync: pump the transfer machine, and anchor the playback-entry moment. A sync
    // window is single-playback-session: leaving the editor drops every transferred buffer and
    // clears the toggles -- re-entering starts fresh (the histories would be stale anyway).
    replaysync::SetChunkBudget(SendBudget());
    replaysync::Tick(nowUs, g_logf);
    {
        static uint8_t lastRm = 0;
        const uint8_t rm = game::LocalReplayMode();
        // Every frame, not just on the edge: cheap, and it cannot be left stuck on by a missed
        // transition. A scrub must not be broadcast as skating -- see game::audio::SetInLocalReplay.
        game::audio::SetInLocalReplay(rm == 2);
        if (rm != lastRm) {
            if (rm == 2) g_playbackEnteredUs = nowUs;
            if (lastRm == 2) {
                g_playbackEnteredUs = 0;
                replaysync::DropAll();
                for (auto& sl : g_slots) { sl.syncOn = false; sl.syncReqSentUs = 0; }
            }
            lastRm = rm;
        }
    }

    // Hold spawning until the game's own pawn has stopped moving around. Any change re-arms the
    // window, so a level load -- which swaps the pawn at least once -- is covered from whichever end
    // it is observed, and a settled world costs one pointer compare per frame.
    if (ownPawn != g_lastOwnPawn) {
        g_lastOwnPawn   = ownPawn;
        g_settleUntilMs = nowMs + kWorldSettleMs;
        g_ownMap[0] = 0;
        if (ownPawn) {
            game::LocalMapName(ownPawn, g_ownMap, sizeof(g_ownMap));
            // Name the level we landed in. Without this a level-specific problem is only ever
            // describable as "the apartment" -- the log should say which world that is.
            if (g_logf) { char m[140]; snprintf(m, sizeof(m), "[session] now in world '%s'",
                          g_ownMap[0] ? g_ownMap : "(unknown)"); g_logf(m); }
        }
    }

    // ---- how many peers are live right now (receive-based; drives the stats line + proxy retiring)
    int live = 0;
    for (auto& s : g_slots) if (s.used && sinceUs(nowUs, s.lastPacketUs) < dropUs) live++;
    g_st.peers = live;

    // ---- who we SEND to: the TRANSPORT's roster (lobby membership), never the heard-from slots.
    // Receive-driven sending deadlocks: two freshly-joined players would each wait for the other's
    // first packet forever. Departed members (state 5) are excluded; everyone else on the roster gets
    // our state whether or not they have spoken yet.
    // Enumerate via PeerCount(), NOT "GetStats until it fails": indices are handed out densely but a
    // backend is free to leave gaps in meaning (the shm mailbox's identity is a SLOT), and a
    // stop-on-first-false loop would quit at the first non-peer index and silently publish to nobody.
    const int nPeers = PeerCount();
    int sendable = 0;
    {
        PeerStats ps;
        for (int i = 0; i < nPeers; i++) if (GetStats(i, &ps) && ps.state != 5) sendable++;
    }

    // ---- 1. PUBLISH our own state.
    // Rate scales with peer count so one peer stays at full 60 Hz while N peers stay bounded. At or
    // above the max there is NO rate limiting at all: a limiter whose period is within a frame of the
    // frame time ALIASES against it (16666 us against a ~16600 us frame halves the real send rate and
    // reads as lag). There is nothing to limit when one packet per frame IS the target.
    // 60 / 45 / 30 by peer count, not 60 / 30: the size batch (interned names, h16 limbs, the
    // shrunk timestamp, conditional crank) cut the steady packet by roughly a third, and this curve
    // spends that saving on rate. Two peers at 45 Hz costs about what two at 30 used to; the floor
    // stays where it was for a full lobby.
    float hz = g_cfg.publishHzMax;
    if (sendable == 2)     hz = (g_cfg.publishHzMax + g_cfg.publishHzMin) * 0.5f;
    else if (sendable > 2) hz = g_cfg.publishHzMin;
    g_st.publishHz = hz;
    const bool unlimited = (hz >= g_cfg.publishHzMax);
    const uint64_t period = unlimited ? 0 : (uint64_t)(1.0e6f / hz);
    const bool due = unlimited || !g_lastPubUs || (nowUs - g_lastPubUs) >= period;

    if (ownPawn && gatherOwn && due && sendable > 0) {
        repl::State own;
        if (gatherOwn(ownPawn, own)) {
            g_ownLast = own; g_haveOwn = true;
            own.typing = (g_cfg.isTyping && g_cfg.isTyping()) ? 1 : 0;
            // Sender-side crank edge probe: one line per rising edge and per def-identity change, so
            // the log can distinguish "the crank never left this machine" from "it was sent and the
            // receiver gated it to zero".
            if (g_logf && debug::Get().crankEdges) {
                static uint8_t lastOn = 0; static uint16_t lastIdx = 0xfffe;
                if (own.crankDefOff != lastIdx) {
                    lastIdx = own.crankDefOff;
                    char m[110];
                    snprintf(m, sizeof(m), "[gather] crank def idx=%d", (int)(int16_t)own.crankDefOff);
                    g_logf(m);
                }
                if (own.crankOn && !lastOn) {
                    char m[110];
                    snprintf(m, sizeof(m), "[gather] crank ON (idx=%d pocket=%.2f)",
                             (int)(int16_t)own.crankDefOff, own.crankPocket);
                    g_logf(m);
                }
                lastOn = own.crankOn ? 1 : 0;
            }
            // The sounds our own game actually played since the last publish, plus the ones still
            // playing. Gathered HERE rather than in gatherOwn because it is not read off the pawn --
            // it is what the audio funnel captured, on its own schedule, between publishes.
            game::audio::Gather(own, nowUs);
            // Trick and grind names: full string on change and every 4th packet; the sentinel
            // between. The counter is per-name so a trick change mid-grind refreshes only itself.
            {
                static char lastTrick[40] = {}, lastGrind[40] = {};
                static int trickTick = 0, grindTick = 0;
                if (g_nameResend) { trickTick = 0; grindTick = 0; lastTrick[0] = 0; lastGrind[0] = 0; }
                if (own.trickName[0] && !strcmp(own.trickName, lastTrick) && (++trickTick & 3) != 0)
                    own.trickNamed = 0;
                else { strncpy_s(lastTrick, own.trickName, _TRUNCATE); own.trickNamed = 1; trickTick = 0; }
                if (own.grindName[0] && !strcmp(own.grindName, lastGrind) && (++grindTick & 3) != 0)
                    own.grindNamed = 0;
                else { strncpy_s(lastGrind, own.grindName, _TRUNCATE); own.grindNamed = 1; grindTick = 0; }
                if (g_nameResend) { game::audio::ForceNames(); g_nameResend = false; }
            }
            // The analog-crouch clock, when SessionTweaks is scrubbing one. Read at publish time so
            // the transported value is the frame's rendered depth, not an earlier tick's.
            own.crankClock = -1.f;
            if (own.crankOn) {
                if (TwkCrankVisClockFn f = twkCrankVisClock()) {
                    float c = -1.f;
                    if (f(&c) && c >= 0.f) own.crankClock = c;
                }
            }
            uint8_t pkt[1024];             // the shm mailbox's exact message cap; EOS P2P allows more.
                                           // Audio rides the snapshot and is sized LAST, so a full
                                           // frame drops trailing sounds, never pose.
            // WHERE THIS FRAME'S SLICE STARTS. A skeleton too big for the 1 KB wire packet ships
            // over consecutive frames (95 bones is 951 B and never fit -- it was dropped silently,
            // which is why a scrubbing player in rig-carrying trousers animated for nobody). The
            // cursor lives here, not inside Pack: the replay ring packs the SAME state with a 2 KB
            // cap and takes the whole skeleton, and a cursor shared with it would be reset every
            // frame, so the wire packet would resend the opening slice forever and the tail bones
            // would never arrive.
            // WHILE WE SCRUB, OUR DRIVERS ARE DEAD WEIGHT TOO. Capture only fills poseN during replay
            // playback, and a scrubbing player is not skating: their anim drivers are inert (measured
            // at 0 of 97 fields moving), so every receiver is animating them from the POSE and from
            // nothing else. Carrying the blob anyway leaves almost nothing for the skeleton in a
            // 1 KB mailbox, and the pose is what gets cut -- field-measured at FIVE bones per packet,
            // which is nineteen frames per refresh of a 95-bone character and reads as a peer stuck
            // in a stuttering animation. The mirror case (serving a scrubbing peer while we skate)
            // already drops them for the same reason; this is the case where WE are the one scrubbing.
            if (own.poseN) {
                own.animLen = 0;
                // ...and the FEET and HANDS with them, for the same reason and the same 6 bones.
                // Both are world transforms the receiver uses to place IK targets for the DRIVER
                // path; a transported pose stamps every bone of the skeleton directly, feet and
                // hands included, so they decide nothing while one is in hand. They cost 72 B --
                // and the measurement said the skeleton was missing a single packet by 6 bones
                // (slice=89 of 95), which is 60. Dropping them is what puts a whole 95-bone
                // character in one packet instead of two, doubling how often it refreshes.
                // The flags are the wire's own "absent" signal, so a receiver needs no change.
                own.feetOk = false;
                own.handOk = false;
            }
            static int poseCursor = 0;
            if (own.poseN && poseCursor >= own.poseN) poseCursor = 0;
            own.poseFirst = (uint8_t)poseCursor;
            int wroteBones = 0;
            const int n = repl::Pack(own, nowUs, pkt, sizeof(pkt), &wroteBones);
            if (wroteBones > 0)
                poseCursor = (poseCursor + wroteBones >= own.poseN) ? 0 : poseCursor + wroteBones;
            // Retain a FAT copy for the replay-sync ring: drivers + anim blob + our CAPTURED
            // skeleton. The bones are the piece playback cannot live without -- the requester's
            // anim graph cannot evaluate a skater during their local replay (the heap-of-clothes,
            // thrice-measured), so their editor stamps this pose at FinalizeBones instead. Cap
            // 2048 so Pack keeps blob AND bones (the 1 KB cap is the WIRE's, not Pack's); the
            // capture itself is a 70-bone read, microseconds.
            if (n > 0) {
                repl::State ringSt = own;
                game::pose::CaptureFromPawn(ownPawn, ringSt);
                ringSt.poseFirst = 0;                    // 2 KB cap: the ring takes the whole skeleton
                // The ring must stay SELF-CONTAINED: its entries are unpacked seconds later, on
                // another machine, with no name cache in reach -- an interned entry there would play
                // silence. Names on, trick names in full; the 2 KB cap has room to spare.
                for (int li = 0; li < (int)ringSt.nLoops; li++) ringSt.loops[li].sendNames = 1;
                ringSt.trickNamed = 1; ringSt.grindNamed = 1;
                uint8_t ringPkt[2048];
                const int rn = repl::Pack(ringSt, nowUs, ringPkt, sizeof(ringPkt));
                replaysync::RecordOwn(rn > 0 ? ringPkt : pkt, rn > 0 ? rn : n, nowUs);
            }
            // A peer with the replay editor open cannot evaluate our drivers -- their anim graph is
            // not running a skater. They get a RESULTS packet (the finished skeleton), built once per
            // publish and unicast to scrubbing peers alone. Everyone else keeps the driver packet,
            // so one player's replay session no longer degrades what every other player sees.
            // If WE are scrubbing, `own` already IS the results packet (pose::Capture in the gather)
            // and there is nothing to split. If the pose capture fails, the driver packet goes to
            // everyone -- a scrubbing peer seeing stale drivers beats seeing nothing.
            uint8_t posePkt[1024]; int pn = 0;
            if (!own.poseN && game::pose::Tune().serveScrubbingPeers && !debug::Get().replayDriverTest) {
                bool anyScrub = false;
                for (auto& sl : g_slots) if (sl.used && sl.peerReplaying) { anyScrub = true; break; }
                if (anyScrub) {
                    repl::State posed = own;
                    // THE DRIVERS ARE DEAD WEIGHT IN THIS PACKET. It goes only to peers with the
                    // replay editor open, and the whole reason the pose exists is that their anim
                    // graph cannot evaluate our drivers -- they are not running a skater. Carrying
                    // them anyway costs a few hundred bytes of a 1 KB mailbox, and the pose is what
                    // gets squeezed: a 95-bone skeleton that would fit whole then takes several
                    // frames of slices to arrive, which reads as a peer animating at a fraction of
                    // the frame rate. Dropping them here buys those bytes back for the one lane this
                    // packet exists to serve. Everyone NOT scrubbing keeps the ordinary driver
                    // packet, untouched.
                    posed.animLen = 0;
                    posed.feetOk = false;   // same reasoning as the scrubbing branch above:
                    posed.handOk = false;   // a full pose already places every bone
                    if (game::pose::CaptureFromPawn(ownPawn, posed)) {
                        if (poseCursor >= posed.poseN) poseCursor = 0;
                        posed.poseFirst = (uint8_t)poseCursor;
                        int wrote = 0;
                        pn = repl::Pack(posed, nowUs, posePkt, sizeof(posePkt), &wrote);
                        if (wrote > 0)
                            poseCursor = (poseCursor + wrote >= posed.poseN) ? 0 : poseCursor + wrote;
                    }
                }
            }
            if (n > 0) {
                PeerStats ps;
                for (int i = 0; i < nPeers; i++) {
                    if (!GetStats(i, &ps) || ps.state == 5) continue;
                    const uint8_t* d = pkt; int dn = n;
                    if (pn > 0)
                        for (auto& sl : g_slots)
                            if (sl.used && sl.peerIdx == i && sl.peerReplaying) { d = posePkt; dn = pn; break; }
                    Send(i, d, dn, false);
                }
                g_st.published++;
                // PHASE-ACCUMULATE the publish clock. `last = now`, sampled at the caller's
                // granularity, aliases against the period; accumulating keeps the true cadence. A
                // stall longer than one period rebases instead of bursting.
                if (unlimited || !g_lastPubUs) g_lastPubUs = nowUs;
                else {
                    g_lastPubUs += period;
                    if (nowUs - g_lastPubUs >= period) g_lastPubUs = nowUs;
                }
            }
        }
    }

    // ---- 1.5 PUBLISH our COSMETICS: rare, large, and its own message type. Sent when the
    // set changes, on a slow heartbeat (a late joiner must not wait for us to change clothes), and
    // whenever a new peer slot opens. Loss only delays it -- nothing downstream depends on cadence.
    if (ownPawn && sendable > 0) {
        static uint64_t lastCosCheckUs = 0, lastCosSendUs = 0;
        static repl::CosmeticSet lastSent;
        static bool haveLast = false;
        // Sample at 2 Hz so a wardrobe change is noticed in half a second rather than two.
        // This costs nothing on the wire: sampling only READS our own profile, and a packet goes out
        // solely when the set actually differs (or on the slow heartbeat). It is affordable only
        // because the FName->string lookup is cached; without that, sampling leaks and does real work
        // per item.
        if (!lastCosCheckUs || sinceUs(nowUs, lastCosCheckUs) > 500000ull) {
            lastCosCheckUs = nowUs;
            // OUR SKELETON, on the same beat. What a transported pose MEANS depends on it: a
            // character is merged from body + garments and the merged skeleton is the UNION of their
            // bones, so two players agree on bone NAMES and on nothing else. Sampled here rather
            // than per frame because it walks a runtime merge and resolves ~95 names, and sent only
            // when it actually differs, plus a slow heartbeat and whenever a peer appears.
            {
                static uint64_t lastSkelSendUs = 0;
                static repl::SkelPrint lastSkel;
                static bool haveLastSkel = false;
                void* mesh = game::SkaterMeshOf(ownPawn);
                repl::SkelPrint mineSk;
                const int sn = mesh ? game::SkeletonBoneHashes(mesh, mineSk.hash, repl::kPoseMaxBones) : 0;
                if (sn > 0) {
                    mineSk.n = (uint8_t)sn;
                    const bool changed = !haveLastSkel || lastSkel.n != mineSk.n ||
                                         memcmp(lastSkel.hash, mineSk.hash, sizeof(uint32_t) * (size_t)sn) != 0;
                    const bool heartbeat = !lastSkelSendUs || sinceUs(nowUs, lastSkelSendUs) > 10000000ull;
                    if (changed || heartbeat || g_skelResend) {
                        uint8_t pkt[520];
                        const int len = repl::PackSkeleton(mineSk, pkt, sizeof(pkt));
                        if (len > 0) {
                            PeerStats ps;
                            for (int i = 0; i < nPeers; i++)
                                if (GetStats(i, &ps) && ps.state != 5) Send(i, pkt, len, true);   // reliable
                            lastSkel = mineSk; haveLastSkel = true;
                            lastSkelSendUs = nowUs; g_skelResend = false;
                            if (changed && g_logf) { char m[120];
                                snprintf(m, sizeof(m), "[pose] published our skeleton: %d bones", sn);
                                g_logf(m); }
                        }
                    }
                }
            }
            repl::CosmeticSet own;
            if (game::GatherOwnCosmetics(ownPawn, own, g_logf)) {
                g_ownLook = own; g_haveOwnLook = true;   // the bone floor borrows garments from this
                const bool changed = !haveLast || memcmp(&own, &lastSent, sizeof(own)) != 0;
                const bool heartbeat = !lastCosSendUs || sinceUs(nowUs, lastCosSendUs) > 10000000ull;
                if (changed || heartbeat || g_cosResend) {
                    // One packet per SECTION: a full wardrobe does not fit in the shm mailbox's 1 KB
                    // slot, and a single packet let clothing starve the board items entirely.
                    int sent = 0;
                    for (uint8_t sec = repl::kCosClothing; sec <= repl::kCosBoard; sec++) {
                        uint8_t pkt[1000];
                        const int n = repl::PackCosmetics(own, sec, pkt, sizeof(pkt));
                        if (n <= 0) continue;
                        PeerStats ps;
                        for (int i = 0; i < nPeers; i++)
                            if (GetStats(i, &ps) && ps.state != 5) Send(i, pkt, n, true);   // reliable
                        sent++;
                    }
                    if (sent) {
                        lastSent = own; haveLast = true; lastCosSendUs = nowUs; g_cosResend = false;
                        if (changed && g_logf) g_logf("[cosmetics] own look changed -> published");
                    }
                }
            }
        }
    }

    // ---- 1.6 DROPPED OBJECTS. Its own lane, and deliberately not folded into the drive loop below:
    // a peer's props stand in the world whether or not their snapshot stream is currently sampling,
    // so nothing here may ride the `continue`s that gate a skater.
    dropFrame(ownPawn, nowUs, nowMs, nPeers, dropUs);

    // ---- 2. DRIVE each peer's proxy from its own stream, sampled at OUR clock.
    int alive = 0;
    bool anyPeerReplaying = false;
    for (auto& s : g_slots) {
        if (!s.used) continue;
        const uint64_t quietForUs = sinceUs(nowUs, s.lastPacketUs);

        // ---- GONE? Two different questions, deliberately kept apart.
        // The TRANSPORT knows the moment somebody leaves: EOS delivers a member-status event and the
        // shared-memory mailbox notices the owning process vanish, and both mark the peer departed
        // (state 5). Waiting out `dropMs` after that would be thirty seconds of a frozen statue
        // standing where a player used to be. The timeout covers only a sender that goes silent
        // without saying anything (a crash, a pulled cable), so: leave on the event, and keep the
        // timer purely as the fallback for unannounced silence.
        PeerStats ps{};
        const bool departed = (s.peerIdx >= 0 && GetStats(s.peerIdx, &ps) && ps.state == 5);
        if (departed || quietForUs > dropUs) {
            s.proxy.Retire(g_logf);     // hide them first -- Forget only drops pointers
            s.proxy.Forget(); s.used = false;
            // Their props leave with them. Nobody else will ever hold these pointers.
            for (auto& d : s.drop) {
                if (d.actor) game::dropper::DestroyRemote(d.actor, g_logf);
                d = Slot::DropObj();
            }
            dropsync::ForgetPeer(s.peerIdx);
            s.haveDropAuth = false; s.dropAuthKey = 0;
            if (g_logf) { char m[140];
                if (departed) snprintf(m, sizeof(m), "[session] peer %d released (they left)", s.peerIdx);
                else          snprintf(m, sizeof(m), "[session] peer %d released (quiet %llums, no goodbye)",
                                       s.peerIdx, (unsigned long long)(quietForUs / 1000));
                g_logf(m); }
            continue;
        }
        if (quietForUs > quietUs) {
            // Sender stopped. Stop the board simulating -- no board may simulate without a writer --
            // and HOLD the last pose; a stalled sender is never extrapolated into the distance.
            if (!s.quietHandled) { s.quietHandled = true; s.proxy.OnQuiet(g_logf); }
            if (s.proxy.actor()) alive++;
            continue;
        }
        // ---- the local player is in replay playback: the editor is their own instance. Every
        // peer is concealed but stays live underneath -- except a SYNCED one, who is revealed and
        // driven from their TRANSFERRED history at the scrub position. The live stream only
        // BUFFERS either way (Push keeps running in OnPacket); it resumes the moment playback
        // ends. Runs before the stream gates so a quiet peer is concealed too.
        if (game::LocalReplayMode() == 2) {
            void* actor = s.proxy.actor();
            // the menu set the wish; the request is stamped HERE so it shares Frame's clock epoch
            // with g_playbackEnteredUs (one clock per subtraction)
            if (actor && s.syncOn && !s.syncReqSentUs &&
                replaysync::PeerSyncState(s.peerIdx, nullptr) == replaysync::SyncState::None) {
                if (replaysync::RequestSync(s.peerIdx, nowUs, MpPrefs_SyncSeconds()))
                    s.syncReqSentUs = nowUs;
            }
            bool syncDriven = false;
            if (actor && s.syncOn && s.syncReqSentUs &&
                replaysync::PeerSyncState(s.peerIdx, nullptr) == replaysync::SyncState::Ready) {
                float cur = 0, total = 0;
                const uint64_t newest = replaysync::BufferNewestUs(s.peerIdx);
                if (newest && game::ReplayPlayTime(&cur, &total) &&
                    s.syncReqSentUs >= g_playbackEnteredUs) {
                    // Map the scrub position onto their history. Our timeline's END is the moment
                    // playback was entered; their buffer's NEWEST is the moment we asked for it.
                    // Both offsets are receiver-clock durations, applied to their owner-clock
                    // timestamps -- clock RATE drift over a 90 s window is ppm-level noise.
                    const uint64_t back = (s.syncReqSentUs - g_playbackEnteredUs)
                                        + (uint64_t)((double)(total - cur) * 1e6);
                    repl::State rs;
                    const uint64_t tPos = newest > back ? newest - back : 0;
                    if (replaysync::SampleAt(s.peerIdx, tPos, rs)) {
                        // No ragdoll trigger and never the scrubbing flag (a live-routing signal).
                        // AUDIO does replay now, in the two shapes it actually has:
                        //  * LOOPS are a reconciled SET -- fed straight from the sampled snapshot
                        //    while the scrub is moving forward, so rolling starts and stops exactly
                        //    as it did when it was recorded. A paused or rewinding scrub feeds the
                        //    empty set instead: a frozen frame must not drone.
                        //  * one-shot EVENTS cannot ride the sample (the same snapshot pair is
                        //    re-read for many frames) -- they fire from the history interval the
                        //    cursor crossed this frame, exactly once, below.
                        const bool advancing = s.syncAudioUs && tPos > s.syncAudioUs;
                        if (!advancing) rs.nLoops = 0;
                        rs.nEvents = 0; rs.bailing = 0; rs.replaying = 0;
                        s.proxy.SetNearLocal(false);      // the board STAMPS at the replayed pose
                        s.proxy.Apply(rs, nowMs, nowUs, g_logf);
                        if (advancing && tPos - s.syncAudioUs < 400000ull) {
                            repl::AudioEvent ev[repl::kAudioMaxEvents * 2];
                            const int nev = replaysync::AudioEventsBetween(
                                s.peerIdx, s.syncAudioUs, tPos, ev, (int)(sizeof(ev) / sizeof(ev[0])));
                            if (nev > 0) s.proxy.PlayAudioEvents(ev, nev);
                        }
                        s.syncAudioUs = tPos;             // a jump lands here too: resync, no burst
                        syncDriven = true;
                    }
                }
            }
            if (s.syncWasDriven && !syncDriven) {
                // Sync stopped driving this proxy (toggled off, buffer gone, playback restarted):
                // nothing will reconcile its loops again until live play resumes, so stop them here
                // rather than leave a rolling sound orbiting a concealed skater.
                s.proxy.AudioStopAll();
                s.syncAudioUs = 0;
            }
            s.syncWasDriven = syncDriven;
            if (actor) {
                if (!syncDriven && s.replayConcealedActor != actor) {
                    game::spectate::SetPeerShownInReplay(actor, false, g_logf);
                    // A CONCEALED SKATER MUST BE SILENT. Their loops are reconciled by the SET they
                    // publish, and that reconcile lives in Apply -- which is exactly what concealing
                    // them skips. So whatever they had playing at the instant the editor opened had
                    // no one left to stop it: enter replay while somebody is grinding and the grind
                    // drones for the whole session. With more than one peer it also reads as the
                    // replay "doubling", because a synced peer's recorded grind plays over the top
                    // of another peer's stuck live one. Stop them here, at the moment they are
                    // hidden; a peer who is revealed again is driven by Apply and starts over.
                    s.proxy.AudioStopAll();
                    s.replayConcealedActor = actor;
                } else if (syncDriven && s.replayConcealedActor == actor) {
                    game::spectate::SetPeerShownInReplay(actor, true, g_logf);
                    s.replayConcealedActor = nullptr;
                }
                alive++;
            }
            continue;
        }
        if (s.replayConcealedActor) {                   // playback ended: reveal
            if (s.proxy.actor() == s.replayConcealedActor)
                game::spectate::SetPeerShownInReplay(s.replayConcealedActor, true, g_logf);
            s.replayConcealedActor = nullptr;
        }

        // ---- IS THIS PEER EVEN HERE? Their level travels with their cosmetics, so once both names
        // are known a difference is decisive. Checked for EVERY peer, not just unspawned ones: a peer
        // who walks to another map keeps publishing, and their coordinates now describe a world that
        // is not ours -- driving an existing proxy with them leaves their skater standing at a
        // meaningless point in your level (that was the visible bug). Unknown either side = treat
        // them as here, so a peer whose cosmetics have not landed is never wrongly hidden.
        // `worldTakesProxies` rides along because it answers the same question for OUR world: in the
        // apartment nobody else is really present, each player being in their own copy of it.
        const bool elsewhere =
            !worldTakesProxies(g_ownMap) ||
            (s.haveCosmetics && s.cosmetics.mapName[0] && g_ownMap[0] &&
             _stricmp(s.cosmetics.mapName, g_ownMap) != 0);
        s.away = elsewhere;              // read by PeerAt/PeerActorById: an away peer offers no actor
        // RE-ASSERTED from the proxy's own visibility, not from a latch: the replay-playback exit
        // above reveals whatever it concealed, and a latch that only fires on change would leave an
        // away peer visible after that. Asking the proxy what it currently is makes every path
        // converge, however it got there.
        if (s.proxy.actor() && s.proxy.Present() == elsewhere) {
            s.proxy.SetPresent(!elsewhere, g_logf);
            if (g_logf) { char m[190];
                if (elsewhere) snprintf(m, sizeof(m), "[session] peer %d is on '%s' and we are on '%s'"
                                        " -- hidden here until they come back", s.peerIdx,
                                        s.cosmetics.mapName[0] ? s.cosmetics.mapName : "(unknown)",
                                        g_ownMap[0] ? g_ownMap : "(unknown)");
                else           snprintf(m, sizeof(m), "[session] peer %d is back in our level", s.peerIdx);
                g_logf(m); }
        }
        if (elsewhere) continue;         // nothing of theirs belongs in this world: do not drive it

        repl::State out;
        if (!s.stream.Sample(nowUs, out)) continue;    // fewer than 2 snapshots: nothing to show yet

        // The board-sim distance gate. Both positions are in the same world frame: ours from the
        // last gathered own state, theirs from the interpolated stream. Hysteresis so the boundary
        // cannot flap the sim; unknown positions keep the last verdict rather than guessing.
        if (g_haveOwn && g_ownLast.bodyPosOk && out.bodyPosOk) {
            const float dx = out.bodyPos[0] - g_ownLast.bodyPos[0];
            const float dy = out.bodyPos[1] - g_ownLast.bodyPos[1];
            const float dz = out.bodyPos[2] - g_ownLast.bodyPos[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            const float inCm  = game::Proxy::Tuning().boardSimMaxDistM * 100.f;
            const float outCm = inCm + game::Proxy::Tuning().boardSimHystM * 100.f;
            if (s.boardNear) { if (d2 > outCm * outCm) s.boardNear = false; }
            else             { if (d2 < inCm  * inCm)  s.boardNear = true;  }
        }
        s.proxy.SetNearLocal(s.boardNear);
        // This peer is in the replay editor, so their machine will not evaluate OUR skeleton -- they
        // need results, not drivers. Latched per frame and read by pose::Capture on the way out. Only
        // peers we are actually driving count: a quiet or departed peer's last known flag must not
        // keep us paying for a pose lane nobody is watching.
        s.peerReplaying = out.replaying != 0;
        s.peerTyping    = out.typing != 0;
        if (out.replaying) anyPeerReplaying = true;

        if (!s.proxy.actor()) {
            // Spawn needs a world handle, which only our own pawn provides.
            if (!ownPawn) continue;
            // ...and a sender the WIRE can vouch for, whenever it can vouch at all. On EOS the
            // lobby roster IS the session: a sender who is not on it is somebody ELSE'S session
            // state. The field case: a host closed their game without leaving, the abandoned
            // lobby's members kept knocking at the ghost membership for minutes, and the moment
            // that player re-hosted, both walked into the brand-new session as skaters without
            // ever joining its lobby. Their packets still buffer above (the join window -- a real
            // joiner's first packets legitimately beat the roster event), but nobody gets a
            // SKATER until EOS places them in our lobby. Wires that can vouch for nothing (UDP by
            // its nature, shm by same-PC trust) behave as before -- the user chose them knowingly.
            if (BackendTrust() >= TRUST_VOUCHED && PeerTrust(s.peerIdx) != TRUST_VOUCHED) {
                if (!s.unvouchedSpoken) {
                    s.unvouchedSpoken = true;
                    if (g_logf) { char m[160]; snprintf(m, sizeof(m),
                        "[session] peer %d sends to us but is not on our lobby roster -- held"
                        " unspawned until EOS vouches for them", s.peerIdx); g_logf(m); }
                }
                continue;
            }
            // ...and a world that has finished deciding whose pawn is whose. See kWorldSettleMs.
            if (nowMs < g_settleUntilMs) continue;
            // A peer who is elsewhere never reaches here -- the presence gate above already
            // continued -- which is what keeps a skater from being spawned into your apartment, and
            // is the only defence in levels where the game possesses a freshly spawned character:
            // there the spawn ITSELF is the damage, whatever the timing.
            if (!s.proxy.EnsureSpawned(ownPawn, out, nowMs, g_logf)) continue;
        }
        s.proxy.Apply(out, nowMs, nowUs, g_logf);
        // Release the peer's one-shot sounds whose moment the playback clock has now reached. AFTER
        // Apply so they are placed against this frame's body pose, and drained from the stream's own
        // queue rather than read out of `out` -- Sample() can skip whole snapshots, and an event
        // carried inside one would be lost exactly when the network is worst.
        {
            repl::AudioEvent ev[repl::kAudioMaxEvents * 2];
            const int nev = s.stream.DrainAudio(ev, (int)(sizeof(ev) / sizeof(ev[0])));
            if (nev > 0) s.proxy.PlayAudioEvents(ev, nev);
        }
        // And their push-state transitions, on the same clock and for the same reason -- a tapped
        // push is a one-shot that a sampled State cannot represent.
        {
            uint8_t ps[8];
            const int nps = s.stream.DrainPushStates(ps, (int)(sizeof(ps) / sizeof(ps[0])));
            if (nps > 0) s.proxy.PlayPushStates(ps, nps);
        }
                // ---- dress it, once per look. AFTER Apply so the actor is fully set up, and only when we
        // actually have their set -- an undressed proxy simply wears the local defaults.
        // Wait for the same construction beat the game's own rebuild waits for, and then OWN that
        // rebuild (MarkVisualsRefreshed): otherwise the plain rebuild fires later and re-reads the
        // restored LOCAL profile, which renders as the proxy wearing the local player's clothes.
        // Their skeleton, handed to the pose layer as soon as their mesh exists. Keyed on the MESH,
        // because that is what the pose slots are keyed by and what a re-dress replaces. One pointer
        // compare per frame until something actually changes.
        if (s.haveSkel && s.proxy.actor()) {
            void* pmesh = game::SkaterMeshOf(s.proxy.actor());
            if (pmesh && pmesh != s.skelFedFor) {
                if (game::pose::SetPeerSkeleton(pmesh, s.skel.hash, s.skel.n)) s.skelFedFor = pmesh;
            }
        }
        if (s.haveCosmetics && s.proxy.actor() && s.proxy.actor() != s.wornForActor
            && s.proxy.VisualsSettled(nowMs)) {
            s.wornForActor = s.proxy.actor();             // one attempt per look PER ACTOR: no storm,
                                                          // but a respawn is a different actor and so
                                                          // dresses again by construction
            int unresolved = 0;
            game::DressProxy(s.proxy.actor(), s.cosmetics, &unresolved, g_logf);
            EnforceBoneFloor(s.proxy.actor(), s.cosmetics, ownPawn);
            s.proxy.MarkVisualsRefreshed();
            // DIVERGENCE, the honest version: unresolved items are attributable exactly (they are
            // wearing something we do not have installed); a differing mod digest means shared item
            // NAMES may still resolve to different art on each screen, which names alone can never
            // detect. Both are reported, neither is guessed at.
            if (g_logf && (unresolved > 0 || (s.cosmetics.modDigest && s.cosmetics.modDigest != game::LocalModDigest()))) {
                char m[220];
                snprintf(m, sizeof(m),
                         "[cosmetics] peer %d: %d item(s) not installed here%s -- their look may differ"
                         " from what you see", s.peerIdx, unresolved,
                         (s.cosmetics.modDigest && s.cosmetics.modDigest != game::LocalModDigest())
                             ? ", and their installed content differs from yours" : "");
                g_logf(m);
            }
        }
        g_st.appliedFrames++;
        alive++;
    }
    g_st.proxiesAlive = alive;
    // Published for pose::Capture, which runs during the NEXT frame's gather. One frame of latency on
    // a flag that changes when somebody opens a menu is not worth reordering the frame for.
    g_anyPeerReplaying = anyPeerReplaying;
}

bool AnyPeerReplaying() { return g_anyPeerReplaying; }

}} // namespace omp::session
