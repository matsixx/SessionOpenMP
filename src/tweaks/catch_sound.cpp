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
// SessionTweaks -- CATCH SOUND, rebuilt: ONE source, recorded into the replay.
//
// The game's own catch sound is an ANIM NOTIFY (UAnimNotify_PlayCatchSound) that flip-trick catch
// animations mostly do not carry at all, and where they do it fires ~250-300 ms AFTER the catch
// engages. The first version of this module RACED the two sources -- the game's late notify against
// our own catch-edge play -- with a correlation window, a grace period and a suppression timer, all
// timing heuristics, all least reliable exactly where the game is played most (co-op). Rebuilt model:
//
//   * the game's catch sound is DISABLED outright for the local skater (its notify's Sound is nulled
//     for the duration of the call -- see the hook for why the call itself must still run), and
//   * OUR sound plays on every catch edge, unconditionally, through
//     **UReplayAudioManager::SpawnSoundAttached** -- the game's own replay-aware wrapper -- so the
//     catch sound is WRITTEN INTO THE REPLAY RECORDING like any native sound. That is also the seam
//     SessionOpenMP captures for peer sync, so in co-op the sound rides the existing audio lane with
//     no extra work (the earlier engine-spawn choice deliberately kept it OUT of the recording; the
//     user reversed that call: replays are the co-op audio source of truth, so the catch sound
//     belongs in them).
//
// The catch edge (_catchOrientState rising) is trustworthy now in a way it was not when v1 shipped:
// the fresh-flick veto, one-catch-per-air and the wedge repair (SessionTweaks 2.59-2.67) mean one
// rising edge = exactly one real catch.
//
// Kept from v1, because each was measured the hard way:
//   * the designers' `_noCatchFlipTricks` blacklist is honoured (all entries are ollies/nollies --
//     nothing is caught, the silence is intentional), judged on the trick def CAPTURED AT EDGE TIME
//     (re-reading _currentFlipTrickDef later judged a stale def -- 74 self-plays once landed on
//     blacklisted ollies that way);
//   * a PEER proxy's notify passes through completely untouched -- muting or boosting it is exactly
//     the "worse with multiple players" report, and OpenMP owns proxy-notify policy;
//   * never return early from PlayCatchSound: it also stops the in-air flip whoosh
//     (board+0x348 _flippingInAirLoopAudio). Null its Sound instead; the base spawn no-ops.
//
// NEVER hook an audio funnel function (UReplayAudioManager::SpawnSound*,
// UGameplayStatics::SpawnSound*, UAudioComponent::SetVolume/SetPitchMultiplier). SessionOpenMP hooks
// all of those from its own MinHook instance once a co-op session arms, and two DLLs detouring the
// same address is a real conflict. The wrapper below is RESOLVED AND CALLED, never hooked -- calling
// the detoured entry is precisely how the sound enters OpenMP's capture.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "catch_sound.h"
#include "catch_tweaks.h"
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    OBJ_CLASS           = 0x10,   // UObjectBase::ClassPrivate -- its name IS the C++ class name
    OBJ_NAME            = 0x18,   // UObjectBase::NamePrivate (FName) -- how anything gets a name
    COMP_OWNER          = 0xa0,   // UActorComponent::OwnerPrivate -- MeshComp -> the skater
    NTF_SOUND           = 0x38,   // UAnimNotify_PlaySound::Sound -- the catch cue, learned here
    SK_ROOT_COMP        = 0x130,  // AActor::RootComponent -- what the self-spawn attaches to
    // ASkaterCharacterBase
    SK_AUDIO_DATA       = 0x4e8,  // USkaterAudioData* _audioData
                                  // +0x4e8 is ALSO ASkateboardEx::_flipper, so a board mistaken for
                                  // a skater reads a plausible pointer here -- one reason the
                                  // blacklist verdict treats "unreadable" as PLAY, not silence.
    SK_CUR_FLIP_DEF     = 0x590,  // UFlipTrickDefinition* _currentFlipTrickDef
    SK_CATCH_MODE       = 0x63d,  // ECatchMode -- the MENU's Catch Mode
    SK_CATCH_ORIENT_ST  = 0x63e,  // ECatchOrientState -- nonzero = a catch actually ENGAGED
    // USkaterAudioData::_noCatchFlipTricks (TArray<UFlipTrickDefinition*>)
    AD_NOCATCH_DATA     = 0x68,
    AD_NOCATCH_NUM      = 0x70,
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int g_on       = 1;     // CatchSound          -- kill-switch, ini only (bug-fix policy: no menu row)
static int g_volPct   = 100;   // CatchSoundVolumePct -- OUR sound's volume; 100 = the cue's authored level
static int g_delayMs  = 40;    // CatchSoundSelfDelayMs -- edge -> sound; small, so it lands with the pose
static int g_diag     = 0;     // CatchSoundDiag      -- one log line per catch; OFF by default
                               // (release logging: diagnostics stay quiet unless asked for)
// The cue name. The catch sound observed firing in-game is 'SCU_FootOnBoard'. It is not referenced
// from any data asset -- neither USkateboardAudioDataAsset nor USkaterAudioData holds a catch cue --
// so this name is the only handle on it that does not require a notify to fire first.
static const char* const kCueName = "SCU_FootOnBoard";
// ---- runtime kill-switches. NEVER SAVED: a transient SEH fault must never become permanent config.
static int g_okEdge = 1, g_okSelfPlay = 1;
static volatile LONG g_faults = 0;
// ---- counters (F1 readout; the diagnosis at a glance)
static volatile LONG g_uiEdges = 0, g_uiPlayed = 0, g_uiMutedGame = 0, g_uiBlacklisted = 0;
static volatile LONG g_uiUnreadable = 0;   // blacklist unreadable -> played anyway (should stay 0)

void CatchSound_ReadConfig(const char* buf) {
    g_on      = TwkIniInt(buf, "CatchSound", 1);
    g_volPct  = TwkIniInt(buf, "CatchSoundVolumePct", 100);
    g_delayMs = TwkIniInt(buf, "CatchSoundSelfDelayMs", 40);
    g_diag    = TwkIniInt(buf, "CatchSoundDiag", 0);
    if (g_volPct  < 25) g_volPct  = 25;   if (g_volPct  > 300) g_volPct  = 300;
    if (g_delayMs < 0)  g_delayMs = 0;    if (g_delayMs > 250) g_delayMs = 250;
    TwkLog("[csnd] config: CatchSound=%d VolumePct=%d DelayMs=%d Diag=%d",
           g_on, g_volPct, g_delayMs, g_diag);
}
void CatchSound_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "CatchSound",            g_on);
    TwkIniSetInt(buf, cap, "CatchSoundVolumePct",   g_volPct);
    TwkIniSetInt(buf, cap, "CatchSoundSelfDelayMs", g_delayMs);
    TwkIniSetInt(buf, cap, "CatchSoundDiag",        g_diag);
}
void CatchSound_ResetDefaults() {
    g_on = 1; g_volPct = 100; g_delayMs = 40; g_diag = 0;
    g_okEdge = g_okSelfPlay = 1;   // re-arm; a past fault is not a preference
}
bool  CatchSound_Enabled()           { return g_on != 0; }
void  CatchSound_SetEnabled(bool on) { g_on = on ? 1 : 0; TwkMarkDirty(); }
float CatchSound_VolumePct()         { return (float)g_volPct; }
void  CatchSound_SetVolumePct(float pct) {
    int v = (int)(pct + 0.5f);
    if (v < 25)  v = 25;                 // same clamps as ReadConfig -- one place to disagree is one
    if (v > 300) v = 300;                // too many
    g_volPct = v;
    TwkMarkDirty();
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
// UAnimNotify_PlayCatchSound::PlaySound -- Epic 0x1149690 / Steam 0x1109b60, size 0x91.
// Arity: (this, USkeletalMeshComponent*) -- two pointers, no floats. It is virtual, so there is no
// direct call site to read and the proof comes from the callee bodies: both this function and its
// tail-jump target read only rcx/rdx, and this one makes three intervening calls before tail-jumping
// with its args intact, which a float argument could not survive without a visible xmm spill/reload
// that a 0x91-byte body does not contain.
static const char* SIG_PLAY_CATCH =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 9A A0 00 00 00 48 8B FA 48 8B F1 48 85 DB "
    "?? ?? E8 ?? ?? ?? ?? 48 8B 53 10";
// FName::ToString(FString&) -- Epic 0x133a4f0 / Steam 0x12fb7b0. CALLED, never hooked, and only from
// the pump (never inside a game callstack). Sig copied verbatim from src/game/game_syms.cpp.
static const char* SIG_FNAME_TOSTR =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00";
// StaticFindObject(UClass*, Outer, const TCHAR* name, bool exactClass) -- Epic 0x1530ec0 / Steam 0x14f2130
static const char* SIG_STATIC_FIND =
    "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 "
    "45 0F B6 F1 49 8B F8 48 8B DA 4C 8B";
// UReplayAudioManager::SpawnSoundAttached -- Epic 0x340e400 / Steam 0x33d52c0, sig copied verbatim
// from src/game/game_syms.cpp where it is proven on both exes. Arg-for-arg identical to
// UGameplayStatics::SpawnSoundAttached (measured there), PLUS: it broadcasts the spawn to the replay
// recorder's _onPlaySound, which is exactly why it is used -- the catch sound enters the local
// replay recording, and replays are what co-op uses as the audio source of truth. RESOLVED AND
// CALLED, never hooked (see the header): when OpenMP is armed, calling the detoured entry routes the
// sound through its capture funnel and onto the wire as a one-shot; solo, it is just the wrapper.
static const char* SIG_RAM_SPAWN_ATT =
    "48 8B C4 48 89 58 10 48 89 70 18 55 41 54 41 55 41 56 41 57 48 8D 68 B8 48 81 EC 20 01 00 00 "
    "48 8B 75 70";

typedef void* (*PlayCatchFn)(void*, void*);
typedef void  (*FNameToStrFn)(const void*, void*);
typedef void* (*StaticFindFn)(void* cls, void* outer, const wchar_t* name, int exactClass);
// 13 args, measured (game_syms.h). This one is CALLED, so the compiler lays out the ABI -- the arity
// traps that govern thunks do not apply here; only getting the list right matters.
typedef void* (*SpawnAttachedFn)(void* sound, void* attachTo, uint64_t attachPoint, const float* loc,
                                 const float* rot, int locType, bool stopWhenDetached, float vol,
                                 float pitch, float start, void* atten, void* conc, bool autoDestroy);
static void* g_origPlay = nullptr, *g_startPlay = nullptr;
static FNameToStrFn    g_fnameToStr = nullptr;
static StaticFindFn    g_staticFind = nullptr;
static SpawnAttachedFn g_spawnAtt   = nullptr;
static void* g_catchCue = nullptr;   // the cue, learned from a real notify -- correct by construction
static void* g_lastNotifySkater = nullptr;

static double CsNow() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static bool IsManual(int catchMode) {
    // ManualMode() is -1 when unknown and an unreadable byte is ALSO -1; comparing them directly
    // would label every unknown case "manual" and corrupt the log.
    const int m = CatchTweaks_ManualMode();
    return m >= 0 && catchMode == m;
}

// ---- FName -> string. FName::ToString hands back a GAME-ALLOCATED FString that cannot be freed
// here (that would need the game's allocator, which this DLL deliberately does not couple to). An
// FName's text never changes, so caching by the FName value bounds this at exactly one leak per
// distinct name. Exported (CatchSound_ObjName) -- flip_speed names tricks through it.
static bool NameOf(const void* obj, char* out, int cap);
bool CatchSound_ObjName(const void* obj, char* out, int cap) { return NameOf(obj, out, cap); }
// Raw-FName variant (a bone name is an FName in a struct, not a UObject). Same cache, same rules.
static bool FNameText(const void* fn, char* out, int cap);
bool CatchSound_FNameText(const void* fname, char* out, int cap) { return FNameText(fname, out, cap); }
static bool NameOf(const void* obj, char* out, int cap) {
    out[0] = 0;
    if (!obj) return false;
    return FNameText((const uint8_t*)obj + OBJ_NAME, out, cap);
}
static bool FNameText(const void* fn, char* out, int cap) {
    out[0] = 0;
    if (!fn || !g_fnameToStr) return false;
    uint64_t key = 0;
    __try { key = *(const uint64_t*)fn; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!key) return false;                                   // NAME_None
    struct Entry { uint64_t key; char name[64]; };
    static Entry cache[96];
    static int nCached = 0;
    for (int i = 0; i < nCached; i++)
        if (cache[i].key == key) { strncpy_s(out, (size_t)cap, cache[i].name, _TRUNCATE); return out[0] != 0; }
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        g_fnameToStr(fn, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        if (k > 0 && nCached < (int)(sizeof(cache) / sizeof(cache[0]))) {
            cache[nCached].key = key;
            strncpy_s(cache[nCached].name, sizeof(cache[nCached].name), out, _TRUNCATE);
            nCached++;
        }
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// ---- the designers' silence list. `def` is the trick to judge, CAPTURED BY THE CALLER AT EDGE
// TIME -- never re-read `_currentFlipTrickDef` later and judge that. Field-measured: 74 self-plays
// once landed on blacklisted ollies because the def was re-read ~40 ms after the edge, by which time
// the game had already cleared it. Returns: -1 unreadable, 0 not blacklisted, 1 blacklisted.
// The CALLER maps -1 to "play": a bad offset must cost a blacklist check, never the whole feature
// (v1 had this backwards, and its own header claimed otherwise).
static int DerivedVerdict(void* skater, void* def) {
    if (!skater) return -1;
    void* ad = twkP(skater, SK_AUDIO_DATA);
    if (!ad) return 0;                                    // the game's own answer with no audio data
    void* data = twkP(ad, AD_NOCATCH_DATA);
    const int num = twkI(ad, AD_NOCATCH_NUM);
    if (num < 0 || num > 512) return -1;                  // implausible -> refuse to walk it
    if (num == 0) return 0;
    if (!data || !def) return -1;
    for (int i = 0; i < num; i++)
        if (twkP(data, i * 8) == def) return 1;
    return 0;
}

// ------------------------------------------------------------------ the ONE hook: the game's spawn
// UAnimNotify_PlayCatchSound::PlaySound. For the LOCAL skater the notify's Sound is nulled for the
// duration of the call -- the game's catch sound is simply gone, ours is the only source. The call
// itself always runs: this function also stops the in-air flip whoosh
// (HandleOnAnimationPlayCatchSound, board+0x348 _flippingInAirLoopAudio), and the base spawn no-ops
// on a null Sound. A PEER proxy's notify passes through completely untouched -- their catch sound is
// OpenMP's business (it mutes proxy notifies itself when the transported one plays instead).
// This is also where the cue is LEARNED: +0x38 on this notify class IS the catch cue, correct by
// construction, which beats StaticFindObject-by-name whenever a notify has fired at least once.
static void* hkPlayCatchSound(void* self, void* meshComp) {
    __try {
        void* snd = self ? twkP(self, NTF_SOUND) : nullptr;
        if (snd && !g_catchCue) g_catchCue = snd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    void* owner   = meshComp ? twkP(meshComp, COMP_OWNER) : nullptr;
    void* localSk = CatchTweaks_Skater();
    const bool isLocal = !localSk || !owner || owner == localSk;
    if (isLocal && owner) g_lastNotifySkater = owner;    // never let a proxy become the fallback

    bool  muted = false;
    void* soundSaved = nullptr;
    if (isLocal && g_on && self) {
        __try {
            soundSaved = *(void**)((uint8_t*)self + NTF_SOUND);
            if (soundSaved) {
                *(void**)((uint8_t*)self + NTF_SOUND) = nullptr;
                muted = true;
                InterlockedIncrement(&g_uiMutedGame);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { muted = false; }
    }

    void* r = nullptr;
    __try { r = ((PlayCatchFn)g_origPlay)(self, meshComp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[csnd] caught fatal in PlayCatchSound -> recovered");
    }
    // Restore ALWAYS, outside the __try: the notify is a SHARED ASSET (a peer's skater can drive the
    // same object), so the modification window must be exactly the length of the call.
    if (muted) { __try { *(void**)((uint8_t*)self + NTF_SOUND) = soundSaved; }
                 __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return r;
}

// ------------------------------------------------------------------ our sound
// Two ways to obtain the cue, preferring the one that cannot be wrong:
//   1. the pointer learned from a real catch notify (correct type by construction), or
//   2. StaticFindObject by name. That searches ANY class and returns the FIRST match, so a
//      redirector with the same name would come back looking like a cue and spawning it would
//      fault. Hence the class-name check below.
static bool  g_cueTried = false;
static volatile LONG g_spawnFaults = 0;   // fresh-resolve faults; 3 strikes = self-play off
static void* ResolveCue() {
    if (g_catchCue) return g_catchCue;
    if (g_cueTried || !g_staticFind) return nullptr;
    g_cueTried = true;
    wchar_t wide[64];
    int i = 0;
    for (; kCueName[i] && i < 63; i++) wide[i] = (wchar_t)(uint8_t)kCueName[i];
    wide[i] = 0;
    void* o = nullptr;
    __try { o = g_staticFind(nullptr, (void*)(intptr_t)-1, wide, 0); }   // ANY_PACKAGE
    __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }
    if (!o) { TwkLog("[csnd] cue '%s' not found in this install -- catch sound unavailable", kCueName); return nullptr; }
    char cls[64];
    if (!NameOf(twkP(o, OBJ_CLASS), cls, sizeof(cls)) || !strstr(cls, "Sound")) {
        TwkLog("[csnd] '%s' resolved to a %s, not a sound -- refusing to play it",
               kCueName, cls[0] ? cls : "?");
        return nullptr;
    }
    TwkLog("[csnd] cue '%s' resolved by name (%s) -- armed", kCueName, cls);
    g_catchCue = o;
    return o;
}

// Attached to the skater's root, through the REPLAY wrapper: recorded into the replay, captured by
// OpenMP's funnel for peer sync, audible locally -- one call, all three.
static bool PlayCatchCue(void* skater) {
    if (!g_okSelfPlay || !g_spawnAtt || !skater) return false;
    void* cue = ResolveCue();
    if (!cue) return false;
    void* root = twkP(skater, SK_ROOT_COMP);
    if (!root) return false;
    float vol = (float)g_volPct / 100.0f;
    if (vol > 4.0f) vol = 4.0f;                        // never drive the mixer into clipping
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    __try {
        g_spawnAtt(cue, root, 0 /*NAME_None*/, zero, zero, 0 /*KeepRelativeOffset*/,
                   false /*stopWhenDetached*/, vol, 1.0f, 0.0f,
                   nullptr /*attenuation: the cue's own*/, nullptr /*concurrency: the cue's own*/,
                   true /*autoDestroy -- a one-shot, same as the notify stages*/);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Most likely a STALE CUE (GC'd with an old level). Drop the cache so the next attempt
        // re-resolves by name instead of writing the feature off for the session -- the pawn-change
        // reset re-arms g_okSelfPlay, so only repeated faults on a FRESH resolve stay fatal.
        g_catchCue = nullptr; g_cueTried = false;
        if (InterlockedIncrement(&g_spawnFaults) >= 3) {
            g_okSelfPlay = 0;
            TwkLog("[csnd] catch-cue spawn faulted %d times -> catch sound off (nothing else affected)",
                   (int)g_spawnFaults);
        } else {
            TwkLog("[csnd] catch-cue spawn faulted (stale cue after a map switch?) -- cache dropped, will re-resolve");
        }
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ the catch edge -> the sound
struct EdgeRec { double t; void* skater; void* trickDef; int catchMode; bool live; };
static const int kEdgeRing = 8;
static EdgeRec g_edges[kEdgeRing];
static int     g_edgeHead = 0;
static int     g_lastCatchSt = 0;
static bool    g_dumpedBlacklist = false;

static void DumpBlacklist(void* skater) {
    if (g_dumpedBlacklist || !skater) return;
    void* ad = twkP(skater, SK_AUDIO_DATA);
    if (!ad) return;
    const int num = twkI(ad, AD_NOCATCH_NUM);
    void* data = twkP(ad, AD_NOCATCH_DATA);
    if (num < 0 || num > 512) return;
    g_dumpedBlacklist = true;
    if (num == 0) { TwkLog("[csnd] _noCatchFlipTricks is EMPTY -- no trick is silenced by design"); return; }
    if (!data)    { TwkLog("[csnd] _noCatchFlipTricks: %d entries but the array data is null", num); return; }
    TwkLog("[csnd] _noCatchFlipTricks -- %d trick(s) the game silences by design (we honour this):", num);
    char nm[64];
    for (int i = 0; i < num; i++) {
        void* def = twkP(data, i * 8);
        TwkLog("[csnd]    %2d. %s", i + 1, (def && NameOf(def, nm, sizeof(nm))) ? nm : "<unnamed>");
    }
}

void CatchSound_PumpFrame() {
    if (!g_startPlay || !g_on) return;
    __try {
        const double now = CsNow();
        void* skater = CatchTweaks_Skater();
        if (!skater) skater = g_lastNotifySkater;

        // ---- map switch / respawn = a NEW PAWN. Everything remembered about the old world is now
        // dangling: the cached cue may have been GC'd with the old level (a stale spawn is the
        // "sound stopped after switching maps" shape -- one SEH fault used to kill the feature for
        // the whole session), and a pending edge's skater is gone. Reset and re-resolve.
        {
            static void* prevSkater = nullptr;
            if (skater && prevSkater && skater != prevSkater) {
                g_catchCue = nullptr; g_cueTried = false;
                g_lastNotifySkater = nullptr;
                g_lastCatchSt = 0;
                for (int i = 0; i < kEdgeRing; i++) g_edges[i].live = false;
                g_okSelfPlay = 1;                                   // a fault on the OLD world is stale
                InterlockedExchange(&g_spawnFaults, 0);
                TwkLog("[csnd] new skater (map switch/respawn) -- state reset, cue re-resolve armed");
            }
            if (skater) prevSkater = skater;
        }
        if (skater) DumpBlacklist(skater);

        // ---- REPLAY-PLAYBACK GUARD. The sound now lives in the replay recording, so during
        // playback the recorder replays it; a self-play on top would double it. "In playback" is
        // detected by the catch system going quiet: CatchTweaks_RecentMaxZ() returns -999999 when
        // the CanCatchOrient hook has seen no calls for ~1.5 s, and the InAirHandler does not run
        // while a replay is being scrubbed. Live skating keeps it fresh every frame.
        const bool catchSystemLive = CatchTweaks_RecentMaxZ() > -999998.0f;

        // ---- the edge. One rising _catchOrientState edge = one real catch (post-2.67 guarantees).
        if (g_okEdge && skater && catchSystemLive) {
            const int st = twkB(skater, SK_CATCH_ORIENT_ST);
            if (st > 0 && g_lastCatchSt == 0) {
                EdgeRec& e = g_edges[g_edgeHead];
                g_edgeHead = (g_edgeHead + 1) % kEdgeRing;
                e.t = now; e.skater = skater;
                e.trickDef = twkP(skater, SK_CUR_FLIP_DEF);   // captured NOW; judged later (see above)
                e.catchMode = twkB(skater, SK_CATCH_MODE);
                e.live = true;
                InterlockedIncrement(&g_uiEdges);
            }
            if (st >= 0) g_lastCatchSt = st;
        }

        // ---- the sound, after the short grace so it lands with the pose rather than ahead of it.
        const double grace = (double)g_delayMs / 1000.0;
        for (int i = 0; i < kEdgeRing; i++) {
            EdgeRec& e = g_edges[i];
            if (!e.live || now - e.t < grace) continue;
            e.live = false;
            const int verdict = DerivedVerdict(e.skater, e.trickDef);
            if (verdict == 1) { InterlockedIncrement(&g_uiBlacklisted); continue; }   // ollies: by design
            if (verdict < 0)  InterlockedIncrement(&g_uiUnreadable);                  // play anyway
            const bool played = PlayCatchCue(e.skater);
            if (played) InterlockedIncrement(&g_uiPlayed);
            if (g_diag) {
                char trick[64];
                if (!e.trickDef || !NameOf(e.trickDef, trick, sizeof(trick)))
                    strcpy(trick, e.trickDef ? "?" : "none");
                TwkLog("[csnd] catch -> %s (trick='%s' mode=%s%s)",
                       played ? "played" : "NOT played (cue unavailable)",
                       trick, IsManual(e.catchMode) ? "manual" : "auto",
                       verdict < 0 ? ", blacklist unreadable" : "");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_okEdge = 0;
        TwkLog("[csnd] caught fatal in the catch-sound pump -> edge watch off (the game mute is unaffected)");
    }
}

void CatchSound_Install() {
    g_startPlay = TwkScanExe(SIG_PLAY_CATCH);
    if (!g_startPlay) {
        TwkLog("[csnd] UAnimNotify_PlayCatchSound::PlaySound sig NOT FOUND -- catch sound off (game updated?)");
        g_on = 0; return;
    }
    if (MH_CreateHook(g_startPlay, (void*)&hkPlayCatchSound, &g_origPlay) != MH_OK ||
        MH_EnableHook(g_startPlay) != MH_OK) {
        TwkLog("[csnd] hook failed on PlayCatchSound -- catch sound off");
        g_startPlay = nullptr; g_on = 0; return;
    }
    g_fnameToStr = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR);
    if (!g_fnameToStr) TwkLog("[csnd] FName::ToString not found -- log lines will be unnamed");
    g_staticFind = (StaticFindFn)TwkScanExe(SIG_STATIC_FIND);
    g_spawnAtt   = (SpawnAttachedFn)TwkScanExe(SIG_RAM_SPAWN_ATT);
    if (!g_staticFind || !g_spawnAtt) {
        // Without the spawn, muting the game's sound would leave EVERY catch silent -- fail safe to
        // the game's own (inconsistent) sound rather than to none at all.
        TwkLog("[csnd] catch sound unavailable (StaticFindObject=%p ReplaySpawnAttached=%p) -- "
               "leaving the game's own sound alone", g_staticFind, g_spawnAtt);
        g_okSelfPlay = 0; g_on = 0;
    }
    TwkLog("[csnd] installed @ %p (replay-recorded spawn @ %p) -- one source, vol %d%%, delay %dms (%s)",
           g_startPlay, (void*)g_spawnAtt, g_volPct, g_delayMs, g_on ? "ON" : "off");
}

void CatchSound_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    if (!g_startPlay) { api->TextDisabled("Catch sound: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Catch sound (ours, replay-recorded)", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(replaces the game's -- one sound, in the recording too)");
    if (on) {
        api->Indent();
        float vp = (float)g_volPct;
        if (api->SliderFloat("Volume (%)", &vp, 50.0f, 300.0f, "%.0f")) CatchSound_SetVolumePct(vp);
        float sd = (float)g_delayMs;
        if (api->SliderFloat("Delay after the catch (ms)", &sd, 0.0f, 200.0f, "%.0f")) {
            g_delayMs = (int)sd; TwkMarkDirty();
        }
        bool diag = g_diag != 0;
        if (api->Checkbox("Log each catch", &diag)) { g_diag = diag ? 1 : 0; TwkMarkDirty(); }
        snprintf(b, sizeof(b), "catches %d  |  played %d  |  game's muted %d  |  by design (ollies) %d%s",
                 (int)g_uiEdges, (int)g_uiPlayed, (int)g_uiMutedGame, (int)g_uiBlacklisted,
                 g_uiUnreadable ? "  |  blacklist unreadable!" : "");
        api->TextDisabled(b);
        snprintf(b, sizeof(b), "cue: %s", g_catchCue ? kCueName : "not resolved yet");
        api->TextDisabled(b);
        api->Unindent();
    }
}
