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
// Replay ghosts -- see replay_ghosts.h. The drive mirrors session.cpp's synced-peer block; keep them
// in step when one changes.
#include "replay_ghosts.h"
#include "../replication/sidecar.h"
#include "../replication/replaysync.h"
#include "../game/game_syms.h"
#include "../game/proxy.h"
#include "../game/cosmetics.h"
#include "../game/pose.h"
#include "../game/spectate.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace omp { namespace ghosts {

struct Ghost {
    bool              used = false;
    bool              retired = false;     // hidden, awaiting a destroy outside playback
    game::Proxy       proxy;
    char              name[sidecar::kNameMax] = {};
    repl::CosmeticSet cosmetics;
    bool              haveWear = false;
    repl::WearSet     wear;
    bool              haveSkel = false;
    repl::SkelPrint   skel;
    uint64_t          anchorEndUs = 0;
    // Keyed on the ACTOR / MESH / BOARD like the session's slots: a respawn is a fresh object and
    // gets dressed, fed and worn again by construction.
    void*             wornForActor = nullptr;
    void*             wearAppliedFor = nullptr;
    void*             skelFedFor = nullptr;
    uint64_t          audioUs = 0;         // the history time last driven; audio events fire (last, now]
    bool              wasDriven = false;
    bool              spawnSaid = false;
};

static Ghost   g_ghosts[sidecar::kMaxPeers];
static bool    g_active = false;
static uint8_t g_lastMode = 0;

static void say(void (*logf)(const char*), const char* fmt, ...) {
    if (!logf) return;
    char m[300];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    logf(m);
}

static void resetLatches(Ghost& g) {
    g.wornForActor = nullptr; g.wearAppliedFor = nullptr; g.skelFedFor = nullptr;
    g.audioUs = 0; g.wasDriven = false;
}

bool LoadSidecar(const uint8_t* data, uint32_t len, void (*logf)(const char*)) {
    sidecar::Peer peers[sidecar::kMaxPeers];
    int n = 0;
    char why[120] = {};
    if (!sidecar::Unpack(data, len, peers, sidecar::kMaxPeers, &n, why, sizeof(why))) {
        say(logf, "[ghost] sidecar refused: %s", why[0] ? why : "unreadable");
        return false;
    }
    int loaded = 0;
    for (int i = 0; i < sidecar::kMaxPeers; i++) {
        Ghost& g = g_ghosts[i];
        const int idx = kGhostBase + i;
        if (i >= n) {
            // A previously loaded replay had more peers: those ghosts go, this replay has no data for them.
            if (g.used && !g.retired) { g.retired = true; g.proxy.SetPresent(false, logf); }
            replaysync::CancelSync(idx);
            continue;
        }
        const sidecar::Peer& p = peers[i];
        if (!replaysync::InjectBuffer(idx, p.entries, p.entriesLen, p.entryCount, logf)) {
            say(logf, "[ghost] %s: history rejected (%u entries, %u bytes)", p.name, p.entryCount, p.entriesLen);
            if (g.used && !g.retired) { g.retired = true; g.proxy.SetPresent(false, logf); }
            continue;
        }
        // An existing ghost actor is KEPT and re-dressed: the same slot is the same skater on screen,
        // and a destroy mid-playback is exactly what must not happen.
        const bool keepActor = g.used && !g.retired && g.proxy.actor();
        if (!keepActor) { g.proxy.Forget(); g = Ghost{}; }
        g.used = true; g.retired = false;
        memcpy(g.name, p.name, sizeof(g.name));
        g.cosmetics = p.cosmetics; g.haveWear = p.haveWear; g.wear = p.wear;
        g.haveSkel = p.haveSkel; g.skel = p.skel;
        g.anchorEndUs = p.anchorEndUs;
        resetLatches(g);
        if (keepActor) g.proxy.SetPresent(true, logf);
        loaded++;
        say(logf, "[ghost] %s: %u snapshots, %.0f s of history, %d clothing / %d board item(s)%s",
            g.name[0] ? g.name : "(unnamed)", p.entryCount,
            (double)(replaysync::BufferNewestUs(idx) - replaysync::BufferOldestUs(idx)) / 1e6,
            (int)p.cosmetics.nChar, (int)p.cosmetics.nBoard, keepActor ? " (actor kept)" : "");
    }
    g_active = loaded > 0;
    say(logf, "[ghost] saved replay: %d of %d peer(s) will play back", loaded, n);
    return g_active;
}

static void endAll(void (*logf)(const char*), bool destroyNow) {
    for (int i = 0; i < sidecar::kMaxPeers; i++) {
        Ghost& g = g_ghosts[i];
        replaysync::CancelSync(kGhostBase + i);
        if (!g.used) continue;
        if (destroyNow) { g.proxy.Destroy(logf); g = Ghost{}; }
        else if (!g.retired) { g.retired = true; g.proxy.SetPresent(false, logf); }
    }
    g_active = false;
}

void Clear(void (*logf)(const char*)) {
    if (!g_active && !AnyAlive()) return;
    endAll(logf, game::LocalReplayMode() != 2);
    say(logf, "[ghost] no sidecar for this replay -- peers cleared");
}

bool Active() { return g_active; }
bool AnyAlive() { for (auto& g : g_ghosts) if (g.used) return true; return false; }

void ForgetAll() {
    for (int i = 0; i < sidecar::kMaxPeers; i++) {
        replaysync::CancelSync(kGhostBase + i);
        if (g_ghosts[i].used) { g_ghosts[i].proxy.Forget(); g_ghosts[i] = Ghost{}; }
    }
    g_active = false;
}

bool IsGhostActor(void* actor) {
    if (!actor) return false;
    for (auto& g : g_ghosts) if (g.used && g.proxy.actor() == actor) return true;
    return false;
}

void Frame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, void (*logf)(const char*)) {
    const uint8_t mode = game::LocalReplayMode();
    // Playback ended: the loaded replay is over as far as we are concerned. Everything goes, and it
    // is safe to destroy now. A re-load reads the sidecar again.
    if (g_lastMode == 2 && mode != 2 && (g_active || AnyAlive())) {
        endAll(logf, true);
        say(logf, "[ghost] playback ended -- saved-replay peers removed");
    }
    g_lastMode = mode;
    if (mode != 2) {
        // Retired ghosts wait for exactly this moment; no drive outside playback.
        for (auto& g : g_ghosts) if (g.used && g.retired) { g.proxy.Destroy(logf); g = Ghost{}; }
        return;
    }
    if (!g_active || !ownPawn) return;

    // Their replay components register themselves at BeginPlay; the invariant that no proxy component
    // is registered has to hold every frame here too (the session's loop is not running).
    game::spectate::PruneProxyComponents(logf);

    float cur = 0, total = 0;
    const bool haveTime = game::ReplayPlayTime(&cur, &total);
    for (int i = 0; i < sidecar::kMaxPeers; i++) {
        Ghost& g = g_ghosts[i];
        if (!g.used || g.retired) continue;
        const int idx = kGhostBase + i;
        // anchor - (total - cur): the replay's end is the anchor, and the scrub position counts back
        // from it. Without a play time (no active instance yet) the end pose stands in.
        uint64_t target = g.anchorEndUs;
        if (haveTime && total > cur) {
            const uint64_t back = (uint64_t)((double)(total - cur) * 1e6);
            target = g.anchorEndUs > back ? g.anchorEndUs - back : 0;
        }
        repl::State st;
        if (!replaysync::SampleAt(idx, target, st)) continue;

        if (!g.proxy.actor()) {
            if (!g.proxy.EnsureSpawned(ownPawn, st, nowMs, logf)) continue;
            g.proxy.SetNoCollide(true, logf);        // a replay's cast collides with nobody
            resetLatches(g);
            if (!g.spawnSaid) { g.spawnSaid = true;
                say(logf, "[ghost] %s spawned for the saved replay (loaded timeline %.2f s, at %.2f; history covers %.2f s before its end)",
                    g.name, total, cur, (double)(g.anchorEndUs - replaysync::BufferOldestUs(idx)) / 1e6); }
        }
        // Same shape as the synced-peer drive: loops only while the scrub advances, no one-shots in
        // the sample (they fire from the interval below), no ragdoll trigger, never the scrub flag.
        const bool advancing = g.audioUs && target > g.audioUs;
        if (!advancing) st.nLoops = 0;
        st.nEvents = 0; st.bailing = 0; st.replaying = 0;
        g.proxy.SetNearLocal(false);
        g.proxy.SetBoardNear(false);
        g.proxy.SetArticulate(true);             // a stamped board: trucks and wheels come off the history
        g.proxy.Apply(st, nowMs, nowUs, logf);
        if (advancing && target - g.audioUs < 400000ull) {
            repl::AudioEvent ev[repl::kAudioMaxEvents * 2];
            const int nev = replaysync::AudioEventsBetween(idx, g.audioUs, target, ev, (int)(sizeof(ev) / sizeof(ev[0])));
            if (nev > 0) g.proxy.PlayAudioEvents(ev, nev);
        }
        g.audioUs = target;
        g.wasDriven = true;

        void* actor = g.proxy.actor();
        if (!actor) continue;
        // Dress AFTER Apply, once per actor, when the game's own construction beat has passed --
        // the session's rule, for the session's reason (a later rebuild re-reads the LOCAL profile).
        if (actor != g.wornForActor && g.proxy.VisualsSettled(nowMs)) {
            g.wornForActor = actor;
            int unresolved = 0;
            game::DressProxy(actor, g.cosmetics, &unresolved, logf);
            g.proxy.MarkVisualsRefreshed();
            g.wearAppliedFor = nullptr;
            if (unresolved > 0) say(logf, "[ghost] %s: %d item(s) not installed here -- their look may differ", g.name, unresolved);
        }
        if (g.haveWear && g.wear.n) {
            void* bd = g.proxy.OwnBoard();
            if (bd && bd != g.wearAppliedFor) { g.wearAppliedFor = bd; game::ApplyPeerWear(bd, g.wear, logf); }
        }
        if (g.haveSkel && g.skel.n) {
            void* pmesh = game::SkaterMeshOf(actor);
            if (pmesh && pmesh != g.skelFedFor && game::pose::SetPeerSkeleton(pmesh, g.skel.hash, g.skel.n))
                g.skelFedFor = pmesh;
        }
    }
}

}} // namespace omp::ghosts
