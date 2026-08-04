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
};

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
static bool     g_anyPeerReplaying = false;   // somebody is scrubbing and needs our finished pose
static repl::State g_ownLast;      // last successfully gathered own state (for spawn placement)
static bool        g_haveOwn = false;

void Init(void (*logf)(const char*)) { g_logf = logf; }
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
        s.haveCosmetics = false; s.wornForActor = nullptr;
        g_cosResend = true;                          // they need OUR look too, now rather than in 10 s
        if (g_logf) { char m[120]; snprintf(m, sizeof(m), "[session] stream opened for peer %d", peerIdx); g_logf(m); }
        return &s;
    }
    return nullptr;                                  // full: drop rather than steal another peer's slot
}

void OnPacket(int peerIdx, const uint8_t* data, int len, uint64_t nowUs) {
    if (!g_cfg.enabled) return;
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
    if (proxyActorOut) *proxyActorOut = s.proxy.actor();
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

void* PeerActorById(int peerId) {
    if (peerId < 0) return nullptr;
    for (auto& s : g_slots) if (s.used && s.peerIdx == peerId) return s.proxy.actor();
    return nullptr;                       // they left, or the slot was reused by somebody else
}

void ForgetProxies() {
    for (auto& s : g_slots) if (s.used) s.proxy.Forget();
    if (g_logf) g_logf("[session] world changed -- all proxy pointers dropped");
}

void Frame(void* ownPawn, uint64_t nowUs, uint64_t nowMs, GatherFn gatherOwn) {
    if (!g_cfg.enabled) return;
    // Both thresholds live in microseconds here so they are compared against nowUs and nothing else.
    // `nowMs` is passed through to the PROXY only (its own spawn/visual timers are self-consistent).
    const uint64_t quietUs = (uint64_t)(g_cfg.quietMs * 1000.0f);
    const uint64_t dropUs  = (uint64_t)(g_cfg.dropMs  * 1000.0f);

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
    float hz = g_cfg.publishHzMax / (float)(sendable > 0 ? sendable : 1);
    if (hz > g_cfg.publishHzMax) hz = g_cfg.publishHzMax;
    if (hz < g_cfg.publishHzMin) hz = g_cfg.publishHzMin;
    g_st.publishHz = hz;
    const bool unlimited = (hz >= g_cfg.publishHzMax);
    const uint64_t period = unlimited ? 0 : (uint64_t)(1.0e6f / hz);
    const bool due = unlimited || !g_lastPubUs || (nowUs - g_lastPubUs) >= period;

    if (ownPawn && gatherOwn && due && sendable > 0) {
        repl::State own;
        if (gatherOwn(ownPawn, own)) {
            g_ownLast = own; g_haveOwn = true;
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
            uint8_t pkt[1024];             // the shm mailbox's exact message cap; EOS P2P allows more.
                                           // Audio rides the snapshot and is sized LAST, so a full
                                           // frame drops trailing sounds, never pose.
            const int n = repl::Pack(own, nowUs, pkt, sizeof(pkt));
            if (n > 0) {
                PeerStats ps;
                for (int i = 0; i < nPeers; i++)
                    if (GetStats(i, &ps) && ps.state != 5) Send(i, pkt, n, false);
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
            repl::CosmeticSet own;
            if (game::GatherOwnCosmetics(ownPawn, own, g_logf)) {
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
        repl::State out;
        if (!s.stream.Sample(nowUs, out)) continue;    // fewer than 2 snapshots: nothing to show yet
        // This peer is in the replay editor, so their machine will not evaluate OUR skeleton -- they
        // need results, not drivers. Latched per frame and read by pose::Capture on the way out. Only
        // peers we are actually driving count: a quiet or departed peer's last known flag must not
        // keep us paying for a pose lane nobody is watching.
        if (out.replaying) anyPeerReplaying = true;

        if (!s.proxy.actor()) {
            // Spawn needs a world handle, which only our own pawn provides.
            if (!ownPawn) continue;
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
        if (s.haveCosmetics && s.proxy.actor() && s.proxy.actor() != s.wornForActor
            && s.proxy.VisualsSettled(nowMs)) {
            s.wornForActor = s.proxy.actor();             // one attempt per look PER ACTOR: no storm,
                                                          // but a respawn is a different actor and so
                                                          // dresses again by construction
            int unresolved = 0;
            game::DressProxy(s.proxy.actor(), s.cosmetics, &unresolved, g_logf);
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
