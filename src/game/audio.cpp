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
#include "audio.h"
#include "game_syms.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#ifdef _WIN32
  #include <windows.h>
  #include <intrin.h>
  #ifdef OMP_USE_MINHOOK
    #include <MinHook.h>
  #endif
#endif

namespace omp { namespace game { namespace audio {

using namespace omp::repl;

static Tuning g_tun;
static Stats  g_st;
Tuning& Tune() { return g_tun; }
Stats   GetStats() { return g_st; }

static void* g_pawn  = nullptr;      // the local skater actor
static void* g_board = nullptr;      // the local board actor
static void (*g_logf)(const char*) = nullptr;
// True while the local player is in replay playback -- capture and publish are both off. See
// SetInLocalReplay.
static bool g_inLocalReplay = false;
void SetLocalParts(void* pawn, void* board) { g_pawn = pawn; g_board = board; }

// ---- reentrancy flags. Both are thread_local because the funnel runs on the game thread but nothing
// promises it is the ONLY thread that ever spawns a sound.
//   g_inReplay : a captured sound is being re-issued. Without it, a 3-player mesh would capture the
//                sounds it just played for peer A and transmit them to peer B as its own, forever.
//   g_inFunnel : execution is inside the game's own funnel detour, so the engine-level detour
//                underneath must not count the same sound twice. What it DOES still catch is the
//                measured bypass: ASkateboardEx::StartGoingFastLoopAudio spawns straight through
//                UGameplayStatics, never touching the funnel.
static thread_local int g_inReplay = 0;
static thread_local int g_inFunnel = 0;
bool Replaying() { return g_inReplay != 0; }

// =====================================================================================================
// capture bookkeeping
// =====================================================================================================
// A sound the sender still has playing. Published as STATE in every packet, so a dropped packet costs
// nothing and a sound that stops simply stops appearing.
struct Live {
    void*      comp = nullptr;
    uint8_t    slot = 0;
    uint8_t    attach = kAudWorld;
    char       cue[kAudioCueLen] = {};
    float      rel[3] = {};
    float      vol = 1.f, pitch = 1.f;
    uint8_t    nParam = 0;
    AudioParam param[kAudioMaxParams];
    uint64_t   touchUs = 0;
    uint64_t   namedUs = 0;    // when this loop last travelled WITH its names (see Gather)
};
static const int kLive = 8;
static Live      g_live[kLive];
static uint8_t   g_nextSlot = 1;

// A one-shot waiting to be published. It rides several consecutive packets so a drop cannot silence
// it; the receiver's id ring turns that redundancy back into exactly one playback.
static const int kPendMax    = 8;
static const int kRepeatPkts = 4;      // ~130 ms of cover at a 30 Hz publish floor
struct Pending { AudioEvent e; uint64_t firedUs = 0; int left = 0; };
static Pending  g_pend[kPendMax];
static int      g_nPend = 0;
static uint16_t g_nextId = 1;

// ---- the cue-name inventory. One line per distinct cue, ever: the instrument that names what is
// still missing. A sound absent from the SENDER's inventory bypassed the funnel and needs its own
// hook; one that never resolves on the RECEIVER is a cue that install does not have. The two lists
// are kept SEPARATE on purpose -- sharing one would let a cue the local player happens to play
// suppress the receive-side "this peer's sound does not exist here" line.
static const int kInvMax = 48;
static char      g_invSend[kInvMax][kAudioCueLen];  static int g_nSend = 0;
static char      g_invMiss[kInvMax][kAudioCueLen];  static int g_nMiss = 0;
static bool listAdd(char list[][kAudioCueLen], int& n, const char* cue) {
    for (int i = 0; i < n; i++) if (!strcmp(list[i], cue)) return false;   // already known
    if (n < kInvMax) { strncpy_s(list[n], cue, _TRUNCATE); n++; }
    return true;                                                           // first time
}

// ---- name caches. `FName::ToString` ALLOCATES an FString the caller owns, and the funnel fires many
// times a second, so resolving a name per call would leak steadily on the game thread. Both caches are
// keyed by an identity that cannot change under us: the USoundBase POINTER for cue names (a UObject
// outlives the sounds it spawns) and the FName's own 8-byte value for parameter names.
// The cue cache is keyed by object POINTER, and a pointer is not proof of identity: UE can collect a
// UObject and hand the same address to another one. So each entry also stores the object's own 8-byte
// FName and re-resolves if it ever disagrees. That check is a single aligned read -- far cheaper than
// the allocation it protects, and it cannot go stale.
struct CueName { const void* obj; uint64_t fname; char name[kAudioCueLen]; };
static const int kCueCache = 32;
static CueName   g_cueCache[kCueCache];
static int       g_nCue = 0;
struct ParamName { uint64_t fname; char name[kAudioParamLen]; };
static const int kParamCache = 16;
static ParamName g_paramCache[kParamCache];
static int       g_nParam = 0;

#ifdef _WIN32
// ---- small safe readers (the codebase's house style: SEH around every game deref) --------------------
static bool rdPtr(const void* base, int off, void** out) {
    __try { *out = *(void**)((const uint8_t*)base + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool rdVec(const void* comp, float* out3) {
    __try {
        const float* p = (const float*)((const uint8_t*)comp + off::kCompPos);
        out3[0] = p[0]; out3[1] = p[1]; out3[2] = p[2];
        return (out3[0] > -1e9f && out3[0] < 1e9f);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// FName -> string. Same shape as the trick/grind/cosmetics name transports: FName indices are
// per-process, so only the STRING is a valid identity. `fnamePtr` points AT the 8-byte FName, wherever
// it lives -- inside a UObject or in a register spill.
static bool fnameStr(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    const Syms& S = Get();
    if (!S.FNameToString || !fnamePtr) return false;
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        S.FNameToString(fnamePtr, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;                     // the FString buffer is deliberately leaked -- see the caches:
        return k > 0;                   // this runs once per distinct cue / parameter name, not per call
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
// The object's own name, cached by object pointer AND validated against its FName (see CueName).
static bool nameOf(const void* obj, char* out, int cap) {
    out[0] = 0;
    if (!obj) return false;
    uint64_t fn = 0;
    __try { fn = *(const uint64_t*)((const uint8_t*)obj + off::kObjNamePrivate); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < g_nCue; i++) {
        if (g_cueCache[i].obj != obj) continue;
        if (g_cueCache[i].fname != fn) {                       // recycled address: re-resolve, rebind
            if (!fnameStr(&fn, g_cueCache[i].name, kAudioCueLen)) return false;
            g_cueCache[i].fname = fn;
        }
        strncpy_s(out, cap, g_cueCache[i].name, _TRUNCATE);
        return out[0] != 0;
    }
    if (!fnameStr(&fn, out, cap)) return false;
    if (g_nCue < kCueCache) {
        g_cueCache[g_nCue].obj = obj; g_cueCache[g_nCue].fname = fn;
        strncpy_s(g_cueCache[g_nCue].name, out, _TRUNCATE);
        g_nCue++;
    }
    return true;
}
// A parameter FName's string, cached by the FName's value.
static bool paramName(uint64_t fname, char* out, int cap) {
    out[0] = 0;
    for (int i = 0; i < g_nParam; i++) {
        if (g_paramCache[i].fname == fname) { strncpy_s(out, cap, g_paramCache[i].name, _TRUNCATE); return out[0] != 0; }
    }
    if (!fnameStr(&fname, out, cap)) return false;
    if (g_nParam < kParamCache) { g_paramCache[g_nParam].fname = fname; strncpy_s(g_paramCache[g_nParam].name, out, _TRUNCATE); g_nParam++; }
    return true;
}
static void* findByName(const char* name) {
    const Syms& S = Get();
    if (!S.StaticFindObject || !name || !name[0]) return nullptr;
    wchar_t wide[kAudioCueLen + 8];
    int i = 0;
    for (; name[i] && i < (int)(sizeof(wide) / sizeof(wide[0])) - 1; i++) wide[i] = (wchar_t)(uint8_t)name[i];
    wide[i] = 0;
    __try { return S.StaticFindObject(nullptr, (void*)(intptr_t)-1, wide, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// UAudioComponent::IsPlaying through the vtable slot the funnel itself calls right after a spawn.
static bool isPlaying(void* comp) {
    if (!comp) return false;
    __try {
        void** vt = *(void***)comp;
        if (!vt) return false;
        using Fn = bool (*)(void*);
        return ((Fn)vt[off::kAcVtblIsPlaying / 8])(comp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void StopSound(void* comp) {
    if (!comp) return;
    g_inReplay++;                       // this Stop must not be mistaken for game activity
    __try {
        void** vt = *(void***)comp;
        if (vt) { using Fn = void (*)(void*); ((Fn)vt[off::kAcVtblStop / 8])(comp); g_st.playStopped++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
    g_inReplay--;
}
#else
static bool rdPtr(const void*, int, void** out) { *out = nullptr; return false; }
static bool rdVec(const void*, float*) { return false; }
static bool nameOf(const void*, char* out, int) { out[0] = 0; return false; }
static bool paramName(uint64_t, char* out, int) { out[0] = 0; return false; }
static void* findByName(const char*) { return nullptr; }
static bool isPlaying(void*) { return false; }
void StopSound(void*) {}
#endif

// ---- who owns a component, and is it ours? ----------------------------------------------------------
// UActorComponent::OwnerPrivate (+0xa0) was read out of GetOwnerRole's 21-byte body -- an accessor that
// small is an exact oracle. USceneComponent inherits it, so an attached sound's owner is one deref.
static void* ownerOf(void* comp) {
    void* o = nullptr;
    if (comp) rdPtr(comp, off::kCompOwner, &o);
    return o;
}
// Classify an attach component against the LOCAL player's parts. Returns false if the sound is not
// ours -- which includes every sound belonging to a PROXY (their audio comes off the wire; capturing
// it would loop it straight back out again).
static bool classifyAttach(void* attachTo, uint8_t* attachOut) {
    if (!attachTo || !g_pawn) return false;
    void* deck = nullptr; void* mesh = nullptr; void* pawnRoot = nullptr; void* boardRoot = nullptr;
    if (g_board) { rdPtr(g_board, off::kBoardFlipper, &deck); rdPtr(g_board, off::kActorRootComp, &boardRoot); }
    rdPtr(g_pawn, off::kSkaterMesh, &mesh);
    rdPtr(g_pawn, off::kActorRootComp, &pawnRoot);
    if (attachTo == deck || attachTo == boardRoot) { *attachOut = kAudBoard; return true; }
    if (attachTo == mesh)                          { *attachOut = kAudMesh;  return true; }
    if (attachTo == pawnRoot)                      { *attachOut = kAudRoot;  return true; }
    // Not one of the four named components: fall back to the owning ACTOR, which still settles whether
    // the sound is ours, and pick the nearest equivalent attach point on the proxy.
    void* own = ownerOf(attachTo);
    if (own && own == g_board) { *attachOut = kAudBoard; return true; }
    if (own && own == g_pawn)  { *attachOut = kAudMesh;  return true; }
    return false;
}
// The local body position, for the relative encoding. Read at CAPTURE time off our own root component.
static bool ourBodyPos(float* out3) {
    void* root = nullptr;
    if (!g_pawn || !rdPtr(g_pawn, off::kActorRootComp, &root) || !root) return false;
    return rdVec(root, out3);
}
// Is this world point close enough to be the local player's sound? A sound spawned AT A LOCATION
// carries no owner at all, so proximity is the only attribution available -- and it is enough here
// because in a solo game the only skater running the code that spawns these is the local player.
static bool nearUs(const float* loc, float* relOut) {
    float body[3];
    if (!ourBodyPos(body)) return false;
    float d2 = 0;
    for (int i = 0; i < 3; i++) { relOut[i] = loc[i] - body[i]; d2 += relOut[i] * relOut[i]; }
    const float r = g_tun.captureRadiusCm;
    return d2 <= r * r;
}

// ---- live proxies, for muting their own notify sounds ------------------------------------------------
// The registry itself lives in game_syms: it is not an audio concern, since the replay-mode guard asks
// the same "is this actor one of ours?" question.
void NoteProxy(void* actor, void* board) { NoteProxyActor(actor, board); }
// Does this component belong to a proxy? Same one-deref ownership test capture uses.
static bool proxyOwns(void* comp) {
    if (!comp) return false;
    if (IsProxyActor(comp)) return true;
    void* own = ownerOf(comp);
    return own && IsProxyActor(own);
}
// ...and for a sound spawned at a bare world point, which carries no owner at all: closer to a proxy
// than to the local player. Only consulted for notify sounds, where the alternative is a double-play.
static bool nearerAProxy(const float* loc) {
    if (!loc) return false;
    float me[3];
    const bool haveMe = ourBodyPos(me);
    float best = 1e18f;
    for (int i = 0, n = ProxyRefCount(); i < n; i++) {
        void* actor = nullptr; void* board = nullptr;
        if (!ProxyRefAt(i, &actor, &board) || !actor) continue;
        void* root = nullptr; float pp[3];
        if (!rdPtr(actor, off::kActorRootComp, &root) || !root || !rdVec(root, pp)) continue;
        float d = 0; for (int c = 0; c < 3; c++) { const float e = loc[c] - pp[c]; d += e * e; }
        if (d < best) best = d;
    }
    if (best > 1e17f) return false;                    // no proxy to compare against
    if (!haveMe) return true;
    float dm = 0; for (int i = 0; i < 3; i++) { const float e = loc[i] - me[i]; dm += e * e; }
    // "Closer to them than to me" is not enough on its own: standing next to a friend, a LOCAL sound
    // can measure marginally nearer their capsule and the local player's own footstep gets muted. A
    // sound within arm's reach is ours, full stop -- muting local audio is a far worse failure than
    // letting one of theirs through, which at worst plays twice for a moment.
    const float kMine = 150.f * 150.f;
    if (dm < kMine) return false;
    return best < dm;
}

static Live* liveFor(void* comp) {
    for (int i = 0; i < kLive; i++) if (g_live[i].comp == comp) return &g_live[i];
    return nullptr;
}
static Live* liveAlloc() {
    for (int i = 0; i < kLive; i++) if (!g_live[i].comp) return &g_live[i];
    // Full: evict the oldest. Eight simultaneous board loops is already beyond what the game runs.
    Live* oldest = &g_live[0];
    for (int i = 1; i < kLive; i++) if (g_live[i].touchUs < oldest->touchUs) oldest = &g_live[i];
    return oldest;
}

#ifdef _WIN32
static uint64_t nowUs() {
    static double invF = 0;
    if (invF == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); invF = 1e6 / (double)f.QuadPart; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * invF);
}
// Did this call come from an anim notify? Notify sounds need their own handling on both sides (see
// Tuning::transportAnimNotify / suppressProxyNotify) so that exactly one copy is heard. The return
// address is the honest way to ask: MEASURE the caller, never infer it from a disassembled call site.
static bool fromAnimNotify(void* ret) {
    const Syms& S = Get();
    if (!S.NotifyPlaySound || !ret) return false;
    const uint8_t* lo = (const uint8_t*)S.NotifyPlaySound;
    const uint8_t* p  = (const uint8_t*)ret;
    return p >= lo && p < lo + off::kNotifyPlaySoundLen;
}
// A PROXY's own anim-notify sound must not play locally: its audio comes off the wire, and letting
// both happen would double every catch and footfall. Called BEFORE the original, so the sound is
// never spawned at all -- safe because the notify callers null-check the spawn result.
static bool muteProxyNotify(void* attachTo, const float* loc, void* ret) {
    // g_inReplay guards OUR OWN re-issues: PlayLoop/PlayOneShot raise it while spawning a peer's
    // transported sound onto their proxy, and muting those would silence the very thing we came to
    // play. Everything below is therefore a sound the PROXY started for itself.
    if (!g_tun.enabled || g_inReplay) return false;
    if (fromAnimNotify(ret)) {
        if (!g_tun.suppressProxyNotify) return false;
        const bool mute = attachTo ? proxyOwns(attachTo) : nearerAProxy(loc);
        if (mute) g_st.notifyMuted++;
        return mute;
    }
    // Not a notify: the proxy's own board or mesh spawning gameplay audio locally. Its board really
    // is simulating (that is what makes it collidable), so the game's board-audio block runs on it and
    // produces rolling/push sound that nobody asked for and that the owner is already sending us.
    // ATTACHED ONLY -- see the note on suppressProxyLocal for why a world spawn is left alone.
    if (!g_tun.suppressProxyLocal || !attachTo) return false;
    const bool mute = proxyOwns(attachTo);
    if (mute) g_st.localMuted++;
    return mute;
}
#else
static uint64_t nowUs() { return 0; }
static bool fromAnimNotify(void*) { return false; }
static bool muteProxyNotify(void*, const float*, void*) { return false; }
#endif

// ---- the one place a captured spawn is turned into wire data ----------------------------------------
// DOES THIS CUE END BY ITSELF? UE stamps INDEFINITELY_LOOPING_DURATION (1e6 s) on a looping sound,
// so the cue answers for itself rather than us inferring it from the caller's intent.
static bool cueLoops(void* sound) {
    if (!sound) return false;
#ifdef _WIN32
    __try {
        const float d = *(const float*)((const uint8_t*)sound + off::kSoundBaseDuration);
        return d > 1.0e5f;                    // the sentinel is 1e6; no real clip is near it
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

static void onSpawn(void* comp, void* sound, void* attachTo, const float* loc,
                    float vol, float pitch, float start, bool autoDestroy, void* ret) {
    if (!g_tun.enabled || g_inReplay || (g_tun.muteWhileReplaying && g_inLocalReplay) || !g_pawn) return;
    // `sound` here came from the GAME's own spawn call, so it is a genuine USoundBase by construction,
    // which makes it the proof of the receiver's cue gate. If the gate rejects this, "SoundBase" is the
    // wrong expectation and the gate disables itself rather than silencing every peer sound.
    if (sound) GateSelfCheck("SoundBase", IsObjectOfClass(sound, "SoundBase"), g_logf);
    const bool notify = fromAnimNotify(ret);
    if (notify && !g_tun.transportAnimNotify) { g_st.notifySkipped++; return; }

    uint8_t attach = kAudWorld;
    float   rel[3] = {0, 0, 0};
    if (attachTo) {
        if (!classifyAttach(attachTo, &attach)) { g_st.rejected++; return; }
        // For an ATTACHED sound the location argument is an offset in the attach component's own
        // terms, so it travels as-is -- but only if it is plausibly an offset. A caller using
        // KeepWorldPosition passes a world coordinate, and applying that as an offset would fling the
        // sound across the map; anything implausible is dropped to the component origin, which is
        // where every skate sound actually sits.
        if (loc) {
            const float lim = 200.f;
            const bool isOffset = fabsf(loc[0]) < lim && fabsf(loc[1]) < lim && fabsf(loc[2]) < lim;
            if (isOffset) { rel[0] = loc[0]; rel[1] = loc[1]; rel[2] = loc[2]; }
        }
    } else {
        if (!loc || !nearUs(loc, rel)) { g_st.rejected++; return; }
    }

    char cue[kAudioCueLen];
    if (!nameOf(sound, cue, sizeof(cue))) { g_st.rejected++; return; }
    g_st.captured++;
    if (g_tun.inventoryLog && g_logf && listAdd(g_invSend, g_nSend, cue)) {
        char m[200];
        snprintf(m, sizeof(m), "[audio] cue '%s' (%s, %s%s)", cue,
                 attachTo ? (attach == kAudBoard ? "board" : attach == kAudMesh ? "mesh" : "root") : "world",
                 (autoDestroy && !cueLoops(sound)) ? "one-shot"
                     : (autoDestroy ? "loop (cue never ends; spawned as a one-shot)" : "loop"),
                 notify ? ", notify" : "");
        g_logf(m);
    }

    // THE LOOP/ONE-SHOT DISCRIMINATOR IS UE's OWN bAutoDestroy FLAG, read out of the callers:
    // ASkateboardEx::SpawnGrindLoopingAudio stages `mov byte [rsp+0x60], 0` for its looping grind
    // sound, and UAnimNotify_PlaySoundRecorded::PlaySound stages 1 for its fire-and-forget hit.
    // A sound the game intends to stop later is a sound the game keeps the component for.
    // ...BUT bAutoDestroy IS THE CALLER'S INTENT, NOT THE SOUND'S NATURE, and the two disagree.
    // Session spawns SCU_PushRolling -- an internally LOOPING cue -- with bAutoDestroy set. On the
    // owner that is harmless: auto-destroy on a sound that never finishes simply never fires, and the
    // game drops the component when it feels like it. Re-issued on a PROXY as a fire-and-forget
    // one-shot it is unstoppable: we keep no handle, it never ends by itself, and nothing in the
    // loop reconcile can reach it. Field report: "the pushing sound plays over and over on the
    // proxy's skateboard after they bail" -- it had been playing since before the bail; the bail is
    // just when everything else went quiet.
    // So a cue that never ends is tracked as a LOOP whatever the caller asked for. It then travels as
    // loop STATE, the receiver owns the component, and it stops the moment it leaves the published
    // set -- which is also what finally makes the bail teardown reach it.
    if (autoDestroy && !cueLoops(sound)) {
        if (g_nPend >= kPendMax) return;                 // a burst beyond this is already inaudible
        Pending& p = g_pend[g_nPend++];
        p.e = AudioEvent{};
        p.e.id = g_nextId++;
        if (!g_nextId) g_nextId = 1;                     // id 0 stays free as "never set"
        p.e.attach = attach;
        strncpy_s(p.e.cue, cue, _TRUNCATE);
        for (int i = 0; i < 3; i++) p.e.rel[i] = rel[i];
        p.e.vol = vol; p.e.pitch = pitch; p.e.start = start;
        p.firedUs = nowUs();
        p.left = kRepeatPkts;
        g_st.oneShots++;
    } else {
        if (!comp) return;                               // no handle = nothing to track or update
        Live* l = liveAlloc();
        *l = Live{};
        l->comp = comp;
        l->slot = g_nextSlot++;
        if (!g_nextSlot) g_nextSlot = 1;
        l->attach = attach;
        strncpy_s(l->cue, cue, _TRUNCATE);
        for (int i = 0; i < 3; i++) l->rel[i] = rel[i];
        l->vol = vol; l->pitch = pitch;
        l->touchUs = nowUs();
        g_st.loopsStarted++;
    }
}

// A parameter set on a component we are tracking. This is where surface type arrives -- the game
// applies it AFTER the spawn (FSessionUtils::UpdateAudioWithSurfaceType), so replaying these is what
// makes a peer's grind sound like the surface they are actually on.
static void onSetParam(void* comp, const char* name, int value, float fval, int kind) {
    if (!g_tun.enabled || g_inReplay) return;
    Live* l = liveFor(comp);
    if (!l) return;                                      // not one of ours: the game is full of sounds
    l->touchUs = nowUs();
    if (kind == 0) {                                     // volume
        l->vol = fval; return;
    }
    if (kind == 1) {                                     // pitch
        l->pitch = fval; return;
    }
    if (!name || !name[0]) return;
    for (int i = 0; i < l->nParam; i++) {
        if (!strcmp(l->param[i].name, name)) { l->param[i].value = (int16_t)value; return; }
    }
    if (l->nParam < kAudioMaxParams) {
        strncpy_s(l->param[l->nParam].name, name, _TRUNCATE);
        l->param[l->nParam].value = (int16_t)value;
        l->nParam++;
    }
}

// =====================================================================================================
// the detours
// =====================================================================================================
// Only the mod DLL hooks: the static lib is linked into the offline gates (symcheck/sessiontest),
// which have neither MinHook nor a game to hook.
#if defined(_WIN32) && defined(OMP_USE_MINHOOK)
static SpawnSndAtLocFn o_SndAtLoc = nullptr, o_GsAtLoc = nullptr;
static SpawnSndAttFn   o_SndAtt   = nullptr, o_GsAtt   = nullptr;
static AcSetIntFn      o_SetInt   = nullptr;
static AcSetFloatFn    o_SetPitch = nullptr, o_SetVol = nullptr;

// Defined below, next to the rest of the concurrency reasoning. Declared here because the widening
// has to happen BEFORE the spawn it belongs to -- see the note on that definition.
static void widenConcurrency(void* cue);

static void* hkSndAtLoc(void* wc, void* sound, const float* loc, const float* rot,
                        float vol, float pitch, float start, void* att, void* conc, bool autoDestroy) {
    void* ret = _ReturnAddress();
    if (muteProxyNotify(nullptr, loc, ret)) return nullptr;   // a proxy's own notify: the wire has it
    if (g_tun.enabled) widenConcurrency(sound);          // BEFORE the spawn -- see widenConcurrency
    g_inFunnel++;
    void* r = o_SndAtLoc(wc, sound, loc, rot, vol, pitch, start, att, conc, autoDestroy);
    g_inFunnel--;
    onSpawn(r, sound, nullptr, loc, vol, pitch, start, autoDestroy, ret);
    return r;
}
static void* hkSndAtt(void* sound, void* attachTo, uint64_t point, const float* loc, const float* rot,
                      int locType, bool stopDetach, float vol, float pitch, float start,
                      void* att, void* conc, bool autoDestroy) {
    void* ret = _ReturnAddress();
    if (muteProxyNotify(attachTo, loc, ret)) return nullptr;
    if (g_tun.enabled) widenConcurrency(sound);          // BEFORE the spawn -- see widenConcurrency
    g_inFunnel++;
    void* r = o_SndAtt(sound, attachTo, point, loc, rot, locType, stopDetach, vol, pitch, start,
                       att, conc, autoDestroy);
    g_inFunnel--;
    onSpawn(r, sound, attachTo, loc, vol, pitch, start, autoDestroy, ret);
    return r;
}
// The engine layer. Everything the funnel spawns passes through here too, so these only ACT when the
// funnel was bypassed -- measured: ASkateboardEx::StartGoingFastLoopAudio, the wind loop.
static void* hkGsAtLoc(void* wc, void* sound, const float* loc, const float* rot,
                       float vol, float pitch, float start, void* att, void* conc, bool autoDestroy) {
    void* ret = _ReturnAddress();
    if (!g_inFunnel && muteProxyNotify(nullptr, loc, ret)) return nullptr;
    void* r = o_GsAtLoc(wc, sound, loc, rot, vol, pitch, start, att, conc, autoDestroy);
    if (!g_inFunnel) onSpawn(r, sound, nullptr, loc, vol, pitch, start, autoDestroy, ret);
    return r;
}
static void* hkGsAtt(void* sound, void* attachTo, uint64_t point, const float* loc, const float* rot,
                     int locType, bool stopDetach, float vol, float pitch, float start,
                     void* att, void* conc, bool autoDestroy) {
    void* ret = _ReturnAddress();
    if (!g_inFunnel && muteProxyNotify(attachTo, loc, ret)) return nullptr;
    void* r = o_GsAtt(sound, attachTo, point, loc, rot, locType, stopDetach, vol, pitch, start,
                      att, conc, autoDestroy);
    if (!g_inFunnel) onSpawn(r, sound, attachTo, loc, vol, pitch, start, autoDestroy, ret);
    return r;
}
// The parameter setters are hooked at the ENGINE layer only, one level below the funnel's wrappers:
// the funnel's SetPitch/SetVolume bodies are byte-identical and no signature can tell them apart, and
// AdjustGoingFastAudio calls the engine setters directly anyway. One layer, both cases, no double
// counting possible.
static void hkSetInt(void* comp, uint64_t fname, int32_t value) {
    o_SetInt(comp, fname, value);
    if (!g_tun.enabled || g_inReplay || !liveFor(comp)) return;
    char nm[kAudioParamLen] = {};
    paramName(fname, nm, sizeof(nm));            // cached: the string is resolved once per name, ever
    onSetParam(comp, nm, value, 0.f, 2);
}
static void hkSetPitch(void* comp, float v) { o_SetPitch(comp, v); onSetParam(comp, nullptr, 0, v, 1); }
static void hkSetVol  (void* comp, float v) { o_SetVol(comp, v);   onSetParam(comp, nullptr, 0, v, 0); }
#endif

// =====================================================================================================
// install
// =====================================================================================================
bool Install(void (*logf)(const char*)) {
    g_logf = logf;
#if defined(_WIN32) && defined(OMP_USE_MINHOOK)
    static bool done = false;
    if (done) return true;
    const Syms& S = Get();
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        if (logf) logf("[audio] MH_Initialize failed -- audio sync OFF");
        return false;
    }
    int n = 0;
    auto hook = [&](void* target, void* detour, void** orig, const char* what) {
        if (!target) {
            if (logf) { char m[160]; snprintf(m, sizeof(m), "[audio] %s NOT RESOLVED -- its sounds will not sync", what); logf(m); }
            return;
        }
        if (MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) n++;
        else if (logf) { char m[160]; snprintf(m, sizeof(m), "[audio] hook FAILED: %s", what); logf(m); }
    };
    hook((void*)S.SndSpawnAtLoc,    (void*)&hkSndAtLoc, (void**)&o_SndAtLoc, "SpawnSoundAtLocation");
    hook((void*)S.SndSpawnAttached, (void*)&hkSndAtt,   (void**)&o_SndAtt,   "SpawnSoundAttached");
    if (g_tun.captureBypasses) {
        hook((void*)S.GsSpawnAtLoc,    (void*)&hkGsAtLoc, (void**)&o_GsAtLoc, "UGameplayStatics::SpawnSoundAtLocation");
        hook((void*)S.GsSpawnAttached, (void*)&hkGsAtt,   (void**)&o_GsAtt,   "UGameplayStatics::SpawnSoundAttached");
    }
    hook((void*)S.AcSetIntParam, (void*)&hkSetInt,   (void**)&o_SetInt,   "SetIntParameter");
    hook((void*)S.AcSetPitch,    (void*)&hkSetPitch, (void**)&o_SetPitch, "SetPitchMultiplier");
    hook((void*)S.AcSetVolume,   (void*)&hkSetVol,   (void**)&o_SetVol,   "SetVolumeMultiplier");
    done = true;
    if (logf) {
        char m[200];
        snprintf(m, sizeof(m), "[audio] capture installed (%d hooks)%s", n,
                 S.NotifyPlaySound ? "" : " -- NO notify marker: anim-notify sounds may double-play");
        logf(m);
    }
    return n > 0;
#else
    (void)logf;
    return false;
#endif
}

// =====================================================================================================
// publish
// =====================================================================================================
void ForceNames() { for (auto& l : g_live) l.namedUs = 0; }

// SCRUBBING IS NOT SKATING, AND PEERS MUST NOT HEAR IT.
// While the local player sits in their replay editor, the game re-spawns their recorded sounds through
// the very funnel this module captures -- that is what UReplayAudioManager is FOR. So a scrub was
// captured and broadcast as if the skater were making those sounds live, and because scrubbing starts
// and stops loops constantly (a fresh slot each time), the receiving end faithfully reproduced every
// start and stop: measured at 14-28 loop starts per second, with starts and stops in lockstep. That is
// the "rolling sound spams while another skater scrubs" report, heard by everyone EXCEPT the person
// scrubbing -- which is why it took so long to notice.
// The pose lane deliberately does the opposite (a scrubbing peer still animates on your screen, round
// 370). That is a considered difference, not an inconsistency: a replayed POSE is a coherent motion at
// any scrub speed, while replayed AUDIO at scrub speed is a machine-gun that cannot be made to sound
// like anything. Silence is the honest rendering of it.
// Entering clears the live table rather than letting it drain: those components belong to the replay
// system now, and the entries would otherwise be published as "still playing" -- the same drone bug
// the receive side had when a concealed peer's loops were left running.
static void clearLive() {
    for (auto& l : g_live) l = Live{};
    for (int i = 0; i < g_nPend; i++) g_pend[i].left = 0;   // one-shots mid-repeat: stop resending
    g_nPend = 0;
}
void SetInLocalReplay(bool on) {
    if (on == g_inLocalReplay) return;
    g_inLocalReplay = on;
    if (on) clearLive();          // no deref: the entries are dropped, never inspected
}

void Gather(State& s, uint64_t now) {
    s.nLoops = 0; s.nEvents = 0;
    if (!g_tun.enabled || (g_tun.muteWhileReplaying && g_inLocalReplay)) return;
    // A BAIL ABANDONS THE SKATING LOOPS, AND THE GAME NEVER TELLS US.
    // A loop only leaves the published set when it is BOTH stale (untouched for loopTtlMs) and
    // reports !IsPlaying. When the ragdoll starts, the board's audio block stops running -- so the
    // push/rolling loop stops being touched, but its component still answers IsPlaying = true, and
    // it was therefore published forever. Field-measured: the sender sat at "live 1" for 13 s across
    // three bails while the observer sat at started=15 stopped=14, one loop stuck open, which is the
    // push sound looping through a bail.
    // Dropping the set on the rising edge is the honest signal: whatever was rolling a moment ago is
    // not rolling now, because the skater is a heap on the floor. It stops our PUBLISHING only -- the
    // owner's own components are untouched, so nothing here can affect what the local player hears.
    if (g_tun.dropLoopsOnBail) {
        static uint8_t lastBail = 0;
        if (s.bailing && !lastBail) clearLive();
        lastBail = s.bailing;
    }
    // Live loops. A loop the game has stopped simply stops being published, and the receiver stops its
    // copy -- which is why loops are STATE and not start/stop events: there is no stop funnel to hook,
    // so "still here" is the only signal that exists, and it is loss-proof.
    const uint64_t ttl = (uint64_t)g_tun.loopTtlMs * 1000ull;
    for (int i = 0; i < kLive; i++) {
        Live& l = g_live[i];
        if (!l.comp) continue;
        const bool stale = (now > l.touchUs) && (now - l.touchUs > ttl);
        if (stale && !isPlaying(l.comp)) { l.comp = nullptr; continue; }
        if (s.nLoops >= kAudioMaxLoops) continue;
        AudioLoop& w = s.loops[s.nLoops++];
        w = AudioLoop{};
        w.slot = l.slot; w.attach = l.attach; w.nParam = l.nParam;
        // Names ride the FIRST publish of a loop and a ~1 s refresh; every packet between carries
        // values only (the receiver's cache supplies the strings). The refresh is what makes the
        // scheme loss-proof and joiner-proof: worst case a loop is silent for under a second.
        if (!l.namedUs || (now > l.namedUs && now - l.namedUs > 1000000ull)) {
            w.sendNames = 1; l.namedUs = now;
        } else w.sendNames = 0;
        strncpy_s(w.cue, l.cue, _TRUNCATE);
        for (int c = 0; c < 3; c++) w.rel[c] = l.rel[c];
        w.vol = l.vol; w.pitch = l.pitch;
        for (int p = 0; p < l.nParam && p < kAudioMaxParams; p++) w.param[p] = l.param[p];
    }
    g_st.liveLoops = s.nLoops;
    // One-shots, each riding several packets. `ageMs` is how long ago it actually fired, so the
    // receiver can schedule it on ITS playback clock instead of playing it the moment it arrives --
    // that is what keeps a peer's pop with the animation rather than ahead of it.
    for (int i = 0; i < g_nPend && s.nEvents < kAudioMaxEvents; i++) {
        Pending& p = g_pend[i];
        if (p.left <= 0) continue;
        const uint64_t age = (now > p.firedUs) ? (now - p.firedUs) : 0;
        p.e.ageMs = (uint16_t)(age / 1000ull > 60000 ? 60000 : age / 1000ull);
        s.events[s.nEvents++] = p.e;
        p.left--;
    }
    int k = 0;                                            // compact out the finished ones
    for (int i = 0; i < g_nPend; i++) if (g_pend[i].left > 0) g_pend[k++] = g_pend[i];
    g_nPend = k;
}

// =====================================================================================================
// playback
// =====================================================================================================
// Resolve the attach component on the PROXY that matches where the sound sat on the sender.
static void* attachOn(uint8_t kind, void* actor, void* board) {
    void* c = nullptr;
    switch (kind) {
        case kAudBoard: if (board) rdPtr(board, off::kBoardFlipper, &c); break;
        case kAudMesh:  if (actor) rdPtr(actor, off::kSkaterMesh, &c);   break;
        case kAudRoot:  if (actor) rdPtr(actor, off::kActorRootComp, &c); break;
        default: break;
    }
    return c;
}
// ---- WIDEN THE CUE'S CONCURRENCY LIMIT -------------------------------------------------------------
// Session is a single-player game, so its sound assets are authored for ONE skater. The rolling loop's
// concurrency group has a small MaxCount, and the resolution rule STEALS: with two skaters rolling
// exactly one is audible, and the newest spawn silences the other.
// MaxCount is raised on whatever concurrency the cue actually uses, once per cue, via the game's OWN
// resolver: `GetConcurrencyHandles` returns the override struct, or every ConcurrencySet entry, or the
// project default -- so no case is missed and no TSet layout has to be assumed.
// THE TIMING IS THE WHOLE FIX, and getting it wrong is why this looked built but did not work.
// `FConcurrencyGroup::Settings` is an FSoundConcurrencySettings BY VALUE (+0x18 of the group, 40 B,
// measured in the PDB -- not a pointer). A group therefore keeps the MaxCount it was BORN with, and
// re-reads the asset never. It dies only when it empties.
// The first cut widened only from resolveCue -- the RECEIVE path -- so the sequence was:
//   local player rolls  ->  group created with the stock MaxCount of 1
//   a peer's rolling arrives  ->  asset widened, but the live group still holds its copy of 1
//   ... and with three people somebody is always rolling, so that group never empties and never
//       gets rebuilt. The limit of 1 outlives the widening for the whole session.
// That is the reported symptom exactly: rolling cutting in and out, "fighting" other players,
// intermittent because it depends on whether the group ever happens to drain.
// So the widening also runs on the game's own skate-audio funnel BEFORE the spawn is forwarded,
// which means the group is BORN wide -- from the local player's very first roll, before any peer
// exists. The funnel is Session's skate audio specifically, so this stays off unrelated sound.
static const int kWidenTo   = 32;
static const int kConcCache = 32;
static const void* g_widened[kConcCache];
static int         g_nWidened = 0;
static void widenConcurrency(void* cue) {
    const Syms& S = Get();
    if (!cue || !S.GetConcHandles) return;
    for (int i = 0; i < g_nWidened; i++) if (g_widened[i] == cue) return;   // once per cue, ever
    if (g_nWidened < kConcCache) g_widened[g_nWidened++] = cue;
#ifdef _WIN32
    __try {
        // The out TArray is deliberately LEAKED (a handful of 16-byte entries per distinct cue, once):
        // freeing it would mean calling FMemory::Free, and a wrong free is far worse than this.
        struct { void* data; int32_t num; int32_t max; } handles{};
        S.GetConcHandles(cue, &handles);
        if (!handles.data || handles.num <= 0 || handles.num > 32) return;
        int raised = 0;
        for (int h = 0; h < handles.num; h++) {
            // FConcurrencyHandle = { FSoundConcurrencySettings* Settings; uint32 ObjectID; bool ovr; }
            int32_t* maxCount = *(int32_t**)((uint8_t*)handles.data + (size_t)h * 16);
            if (!maxCount) continue;
            const int32_t cur = maxCount[off::kSoundConcMaxCount / 4];
            // 0 is left alone -- it is not "small", it is a different meaning. Only a real, low limit
            // gets raised, and never lowered.
            if (cur > 0 && cur < kWidenTo) { maxCount[off::kSoundConcMaxCount / 4] = kWidenTo; raised++; }
        }
        if (raised && g_logf) {
            char m[190];
            snprintf(m, sizeof(m), "[audio] raised concurrency on %d group(s) to %d -- the stock limit"
                                   " is authored for one skater and STEALS the loop", raised, kWidenTo);
            g_logf(m);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
#endif
}

// Cue name -> the local USoundBase. A cue this install does not have resolves to null and is COUNTED
// and named, never crashed on: an identity is not ownership.
static void* resolveCue(const char* cue) {
    void* o = findByName(cue);
    // TYPE GATE, AND IT MUST COME BEFORE widenConcurrency. The cue name is a peer's, findByName is
    // StaticFindObject(ANY_PACKAGE), and widenConcurrency does not merely read the pointer -- it
    // passes it to USoundBase::GetConcurrencyHandles and then WRITES through what comes back, so a
    // wrong-typed pointer would reach a game function and a memory write one step earlier than it
    // reaches the spawn. Checking here covers both.
    // Subclasses pass by construction: real cues are USoundCue/USoundWave, both USoundBase.
    if (o && g_tun.typeGate && GateTrusted("SoundBase") && !IsObjectOfClass(o, "SoundBase")) {
        g_st.rejectedType++;
        if (g_logf && listAdd(g_invMiss, g_nMiss, cue)) {
            char m[200];
            snprintf(m, sizeof(m), "[audio] cue '%s' REJECTED -- that name is not a sound on this install", cue);
            g_logf(m);
        }
        return nullptr;                                  // same path as "not present": counted, silent
    }
    if (o) widenConcurrency(o);      // before the spawn: concurrency is evaluated when a sound starts
    if (!o) {
        g_st.unresolved++;
        if (g_tun.inventoryLog && g_logf && listAdd(g_invMiss, g_nMiss, cue)) {
            char m[180];
            snprintf(m, sizeof(m), "[audio] cue '%s' does NOT resolve here -- that sound will be silent", cue);
            g_logf(m);
        }
    }
    return o;
}
// BOUND THE FName TABLE. `FNameCtor(..., 1)` is FNAME_Add: it INTERNS the string into the process-wide
// FName table, permanently, and the strings here come off the wire. A peer sending a fresh 23-byte
// parameter name every packet would grow that table for the life of the session -- not type confusion,
// just attacker-driven allocation that never comes back.
// A distinct-name BUDGET, not a whitelist: a whitelist of names the local player has happened to
// produce would refuse a legitimate parameter until that sound was made locally. Genuine play uses a
// handful of names (`GrindDropHeightIndex` and friends), so a budget of 64 cannot be reached by
// anything legitimate and caps an attacker at 64 permanent entries.
static const int kParamNameBudget = 64;
static char      g_paramSeen[kParamNameBudget][kAudioParamLen];
static int       g_nParamSeen = 0;
static bool      g_paramBudgetSaid = false;
static bool paramNameAllowed(const char* name) {
    for (int i = 0; i < g_nParamSeen; i++) if (!strcmp(g_paramSeen[i], name)) return true;
    if (g_nParamSeen >= kParamNameBudget) {
        if (!g_paramBudgetSaid && g_logf) {
            g_paramBudgetSaid = true;
            g_logf("[audio] parameter-name budget reached -- further NEW names are ignored "
                   "(normal play uses a handful; this many means someone is generating them)");
        }
        return false;
    }
    strncpy_s(g_paramSeen[g_nParamSeen], name, _TRUNCATE);
    g_nParamSeen++;
    return true;
}

static void applyParams(void* comp, const AudioLoop& l) {
    const Syms& S = Get();
    if (!comp) return;
    if (S.AcSetVolume) S.AcSetVolume(comp, l.vol);
    if (S.AcSetPitch)  S.AcSetPitch(comp, l.pitch);
    if (!S.AcSetIntParam || !S.FNameCtor) return;
    for (int i = 0; i < l.nParam && i < kAudioMaxParams; i++) {
        if (!l.param[i].name[0]) continue;
        if (!paramNameAllowed(l.param[i].name)) continue;
#ifdef _WIN32
        __try {
            uint64_t fn = 0;
            S.FNameCtor(&fn, l.param[i].name, 1);          // FNAME_Add, the funnel's own usage
            S.AcSetIntParam(comp, fn, l.param[i].value);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
#endif
    }
}

void* PlayLoop(const AudioLoop& l, void* actor, void* board, const float* bodyPos) {
    const Syms& S = Get();
    if (!g_tun.enabled || !actor) return nullptr;
    void* cue = resolveCue(l.cue);
    if (!cue) return nullptr;
    void* comp = nullptr;
    g_inReplay++;
#ifdef _WIN32
    __try {
#endif
        float loc[3] = { l.rel[0], l.rel[1], l.rel[2] };
        float rot[3] = { 0, 0, 0 };
        void* att = attachOn(l.attach, actor, board);
        // Play through the ENGINE functions, NOT UReplayAudioManager's wrappers. The wrappers
        // BROADCAST to the replay recorder's _onPlaySound, so replaying a peer's sound through them
        // writes that sound into the LOCAL player's replay recording as if they had made it.
        if (att && S.GsSpawnAttached) {
            // Location as a relative OFFSET, matching how it was captured. Attenuation/concurrency are
            // left null so the cue's own settings apply -- which is what the game's own callers rely on.
            comp = S.GsSpawnAttached(cue, att, 0ull, loc, rot, 0 /*KeepRelativeOffset*/, false,
                                     l.vol, l.pitch, 0.f, nullptr, nullptr, false);
        } else if (S.GsSpawnAtLoc) {
            float w[3] = { bodyPos[0] + l.rel[0], bodyPos[1] + l.rel[1], bodyPos[2] + l.rel[2] };
            comp = S.GsSpawnAtLoc(actor, cue, w, rot, l.vol, l.pitch, 0.f, nullptr, nullptr, false);
        }
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; comp = nullptr; }
#endif
    if (comp) { applyParams(comp, l); g_st.playStarted++; }
    g_inReplay--;
    return comp;
}

void UpdateLoop(void* comp, const AudioLoop& l) {
    if (!g_tun.enabled || !comp) return;
    g_inReplay++;
    applyParams(comp, l);
    g_inReplay--;
}

void PlayOneShot(const AudioEvent& e, void* actor, void* board, const float* bodyPos) {
    const Syms& S = Get();
    if (!g_tun.enabled || !actor) return;
    void* cue = resolveCue(e.cue);
    if (!cue) return;
    g_inReplay++;
#ifdef _WIN32
    __try {
#endif
        float loc[3] = { e.rel[0], e.rel[1], e.rel[2] };
        float rot[3] = { 0, 0, 0 };
        void* att = attachOn(e.attach, actor, board);
        if (att && S.GsSpawnAttached) {          // engine, not the funnel -- see PlayLoop's note
            S.GsSpawnAttached(cue, att, 0ull, loc, rot, 0, false,
                              e.vol, e.pitch, e.start, nullptr, nullptr, true);
        } else if (S.GsSpawnAtLoc) {
            float w[3] = { bodyPos[0] + e.rel[0], bodyPos[1] + e.rel[1], bodyPos[2] + e.rel[2] };
            S.GsSpawnAtLoc(actor, cue, w, rot, e.vol, e.pitch, e.start, nullptr, nullptr, true);
        }
        g_st.played++;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_st.faults++; }
#endif
    g_inReplay--;
}

}}} // namespace omp::game::audio
