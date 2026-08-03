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
// SessionTweaks -- CATCH SOUND. The board-catch sound is inconsistent, sometimes quiet, and often
// missing entirely under manual catch, while a replay of the same run can still contain it.
//
// The catch sound is an ANIM NOTIFY, not gameplay audio, and it is gated by a function that returns
// early with no sound at all. Decompiled from the Epic exe + PDB:
//
//   UAnimNotify_PlayCatchSound::PlaySound(this, USkeletalMeshComponent* MeshComp)
//     skater = MeshComp->[0xa0]                          // UActorComponent::OwnerPrivate
//     if (!skater || !skater->IsA(ASkaterCharacterBase)) goto tail;
//     if (!ShouldPlayCatchAudio(skater)) return 0;       // <-- SILENT EXIT. No sound, ever.
//     board = skater->GetSkateboard();
//     if (board) HandleOnAnimationPlayCatchSound(board); // stops _flippingInAirLoopAudio (board+0x348)
//   tail:
//     tail-JMP UAnimNotify_PlaySoundRecorded::PlaySound(this, MeshComp)   // the real spawn
//
//   ASkaterCharacterBase::ShouldPlayCatchAudio(this)
//     USkaterAudioData* ad = this->_audioData;                 // skater +0x4e8
//     if (!ad) return true;
//     for (t in ad->_noCatchFlipTricks)                        // TArray Data +0x68 / Num +0x70
//         if (t == this->_currentFlipTrickDef) return FALSE;   // skater +0x590 -- BLACKLISTED
//     return true;
//
// So `_noCatchFlipTricks` is a designer-authored blacklist of tricks that get no catch sound. The
// spawn then reads, off the notify object (UAnimNotify_PlaySound layout, PDB-confirmed):
// +0x38 Sound, +0x40 VolumeMultiplier, +0x44 PitchMultiplier, +0x48 bFollow bit0, +0x4c AttachName.
// The notify is a PER-ANIMATION-ASSET instance -- volume is authored per catch animation -- and with
// bFollow clear the sound spawns at a FIXED WORLD POINT that the skater then rides away from.
//
// What this module does: it floors the per-animation volume, re-anchors the sound to the skater
// instead of a world point, plays the cue itself on the catch edge when no notify fires at all (flip
// tricks carry no catch notify, which is the main cause of silence), swallows a late game notify so
// only one sound lands per catch, and honours the designers' blacklist throughout. Everything else
// is telemetry that names which stage ate a given catch sound.
//
// NEVER hook an audio funnel function (UReplayAudioManager::SpawnSound*,
// UGameplayStatics::SpawnSound*, UAudioComponent::SetVolume/SetPitchMultiplier). SessionOpenMP hooks
// all of those from its own MinHook instance once a co-op session arms, and two DLLs detouring the
// same address is a real conflict. The addresses hooked below are untouched by it.
//
// Design property: the fixes depend ONLY on the two hooks' return values. Only the telemetry reads
// the inferred offsets (+0x4e8/+0x590/+0x68/+0x70), so a wrong offset costs a log line, never a fix.
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
    // UAnimNotify_PlaySound base layout. UAnimNotify_PlayCatchSound derives through
    // UAnimNotify_PlaySoundRecorded and adds NOTHING (both report 0 members, size 88), so the base
    // fields are exactly where the engine puts them.
    NTF_SOUND           = 0x38,   // USoundBase*  -- NULL here = the notify fires and nothing plays
    NTF_VOLUME          = 0x40,   // float VolumeMultiplier -- AUTHORED PER CATCH ANIMATION
    NTF_PITCH           = 0x44,   // float PitchMultiplier
    NTF_FOLLOW          = 0x48,   // bitfield; bit0 = bFollow (set -> Attached, clear -> AtLocation)
    // ASkaterCharacterBase
    SK_AUDIO_DATA       = 0x4e8,  // USkaterAudioData* _audioData
                                  // +0x4e8 is ALSO ASkateboardEx::_flipper, so a board mistaken for
                                  // a skater reads a plausible pointer here. Hence the agreement gate.
    SK_CUR_FLIP_DEF     = 0x590,  // UFlipTrickDefinition* _currentFlipTrickDef
    SK_CATCH_MODE       = 0x63d,  // ECatchMode -- the MENU's Catch Mode
    SK_CATCH_ORIENT_ST  = 0x63e,  // ECatchOrientState -- nonzero = a catch actually ENGAGED
    // USkaterAudioData::_noCatchFlipTricks (TArray<UFlipTrickDefinition*>)
    AD_NOCATCH_DATA     = 0x68,
    AD_NOCATCH_NUM      = 0x70,
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
// User intent only -- these are the values SaveConfig writes. The runtime kill-switches below are
// deliberately NOT saved: a transient SEH fault must never become permanent config.
static int g_on        = 1;     // CatchSound              -- master
static int g_follow    = 1;     // CatchSoundFollow        -- attach to the skater, not a world point
static int g_minVolPct = 100;   // CatchSoundMinVolumePct  -- floor; never LOWERS anything
static int g_volPct    = 100;   // CatchSoundVolumePct     -- scale on top (100 = stock)
static int g_diag      = 1;     // CatchSoundDiag          -- the telemetry
static int g_windowMs  = 150;   // CatchSoundWindowMs      -- edge<->notify correlation window
static int g_selfPlay  = 1;     // CatchSoundSelfPlay      -- play the cue ourselves when nothing did
static int g_selfDelay = 40;    // CatchSoundSelfDelayMs   -- grace before deciding nothing will
// CatchSoundSuppressMs -- after self-play, swallow the game's own notify for this long. Where the
// notify does fire on a flip trick it arrives ~250-300 ms after the catch engages (its own lines log
// catchState=0, i.e. the catch is already over), so it lands past both the self-play grace and the
// correlation window and no grace period can wait it out. The rule is one sound per catch: whoever
// went first wins.
static int g_suppressMs = 450;
// The cue name. The catch sound observed firing in-game is 'SCU_FootOnBoard'. It is not referenced
// from any data asset -- neither USkateboardAudioDataAsset nor USkaterAudioData holds a catch cue --
// so this name is the only handle on it that does not require a notify to fire.
static const char* const kCueName = "SCU_FootOnBoard";
// ---- runtime kill-switches. NEVER SAVED.
static int g_okShould = 1, g_okVol = 1, g_okFollow = 1, g_okEdge = 1;
static volatile LONG g_faults = 0;
// ---- counters (F1 readout; the diagnosis at a glance)
static volatile LONG g_uiNotify = 0, g_uiBlacklisted = 0, g_uiUnmuted = 0, g_uiQuiet = 0;
static volatile LONG g_uiNullCue = 0, g_uiEdges = 0, g_uiEdgeNoNotify = 0, g_uiNotifyNoEdge = 0;
static volatile LONG g_uiEdgesManual = 0, g_uiSilentManual = 0, g_uiSelfPlayed = 0;
static volatile LONG g_uiSuppressed = 0;    // the game's LATE notify, swallowed after a self-play
// When each side last actually made the sound. Deliberately NOT part of the diagnostic rings: the
// one-sound-per-catch rule has to work with logging switched off.
static double g_lastPlayT     = -1000.0;    // the game played it
static double g_lastSelfPlayT = -1000.0;    // this module played it
static volatile LONG g_pumpTicks = 0;
static volatile LONG g_logLines = 0;
static const LONG kLogBudget = 600;   // the log is a diagnostic, not a firehose

void CatchSound_ReadConfig(const char* buf) {
    g_on        = TwkIniInt(buf, "CatchSound", 1);
    g_follow    = TwkIniInt(buf, "CatchSoundFollow", 1);
    g_suppressMs = TwkIniInt(buf, "CatchSoundSuppressMs", 450);
    if (g_suppressMs < 0) g_suppressMs = 0;      if (g_suppressMs > 1000) g_suppressMs = 1000;
    g_minVolPct = TwkIniInt(buf, "CatchSoundMinVolumePct", 100);
    g_volPct    = TwkIniInt(buf, "CatchSoundVolumePct", 100);
    g_diag      = TwkIniInt(buf, "CatchSoundDiag", 1);
    g_windowMs  = TwkIniInt(buf, "CatchSoundWindowMs", 150);
    g_selfPlay  = TwkIniInt(buf, "CatchSoundSelfPlay", 1);
    g_selfDelay = TwkIniInt(buf, "CatchSoundSelfDelayMs", 40);
    if (g_selfDelay < 0)   g_selfDelay = 0;      if (g_selfDelay > 250) g_selfDelay = 250;
    if (g_minVolPct < 1)   g_minVolPct = 1;      if (g_minVolPct > 200) g_minVolPct = 200;
    if (g_volPct   < 25)   g_volPct   = 25;      if (g_volPct   > 300)  g_volPct   = 300;
    if (g_windowMs < 40)   g_windowMs = 40;      if (g_windowMs > 500)  g_windowMs = 500;
    TwkLog("[csnd] config: CatchSound=%d Follow=%d MinVolumePct=%d VolumePct=%d Diag=%d "
           "WindowMs=%d SelfPlay=%d SelfDelayMs=%d SuppressMs=%d",
           g_on, g_follow, g_minVolPct, g_volPct, g_diag, g_windowMs,
           g_selfPlay, g_selfDelay, g_suppressMs);
}
void CatchSound_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "CatchSound",             g_on);
    TwkIniSetInt(buf, cap, "CatchSoundFollow",       g_follow);
    TwkIniSetInt(buf, cap, "CatchSoundSuppressMs",   g_suppressMs);
    TwkIniSetInt(buf, cap, "CatchSoundMinVolumePct", g_minVolPct);
    TwkIniSetInt(buf, cap, "CatchSoundVolumePct",    g_volPct);
    TwkIniSetInt(buf, cap, "CatchSoundDiag",         g_diag);
    TwkIniSetInt(buf, cap, "CatchSoundSelfPlay",     g_selfPlay);
    TwkIniSetInt(buf, cap, "CatchSoundSelfDelayMs",  g_selfDelay);
}
void CatchSound_ResetDefaults() {
    g_on = 1; g_follow = 1; g_minVolPct = 100; g_volPct = 100; g_diag = 1;
    g_windowMs = 150; g_selfPlay = 1; g_selfDelay = 40; g_suppressMs = 450;
    g_okShould = g_okVol = g_okFollow = g_okEdge = 1;   // re-arm; a past fault is not a preference
}
bool  CatchSound_Enabled()           { return g_on != 0; }
void  CatchSound_SetEnabled(bool on) { g_on = on ? 1 : 0; }
float CatchSound_VolumePct()         { return (float)g_volPct; }
void  CatchSound_SetVolumePct(float pct) {
    int v = (int)(pct + 0.5f);
    if (v < 25)  v = 25;                 // same clamps as ReadConfig -- one place to disagree is one
    if (v > 300) v = 300;                // too many
    g_volPct = v;
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
// UAnimNotify_PlayCatchSound::PlaySound -- Epic 0x1149690 / Steam 0x1109b60, size 0x91.
// Arity: (this, USkeletalMeshComponent*) -- two pointers, no floats. It is virtual, so there is no
// direct call site to read and the proof comes from the callee bodies: both this function and its
// tail-jump target read only rcx/rdx, and this one makes three intervening calls before tail-jumping
// with its args intact, which a float argument could not survive without a visible xmm spill/reload
// that a 0x91-byte body does not contain. Declare exactly (void*, void*); padding with spare params
// does not make a mismatch safer (a double in slot 3 goes to xmm2 and would not preserve r8 anyway).
static const char* SIG_PLAY_CATCH =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 9A A0 00 00 00 48 8B FA 48 8B F1 48 85 DB "
    "?? ?? E8 ?? ?? ?? ?? 48 8B 53 10";
// ASkaterCharacterBase::ShouldPlayCatchAudio -- Epic 0x1007b60 / Steam 0xfc7990, size 0x38.
// (this) -> bool. Exactly 1 xref (from PlayCatchSound). A real 0x38 body with a source line, so not
// an ICF-folded accessor stub.
static const char* SIG_SHOULD_CATCH =
    "48 8B 91 E8 04 00 00 48 85 D2 ?? ?? 48 8B 42 68 4C 8B 81 90 05 00 00 48 63 4A 70 48 8D 14 C8 "
    "48 3B C2 ?? ?? 4C 39 00 ?? ??";
// UAnimNotify_PlaySoundRecorded::Notify -- Epic 0x3402130 / Steam 0x33c8ff0, size 0x41. The
// DISPATCHER. PlaySound is only reached if Notify clears three gates first:
//     if (!this->Sound) return;                      // gate 1 -- an unauthored cue looks identical
//     if (!MeshComp)    return;                      // gate 2   to "the animation never got here"
//     if (Sound->IsLooping()) return;                // gate 3 -- UE's own one-shot rule
//     vcall [vtbl + 0x298](this, MeshComp);          // -> PlaySound
// Hooking here separates "the notify never fired" from "it fired and was gated", which the PlaySound
// hook alone cannot do. Arity: the body reads only rcx (this) and rdx (MeshComp) and forwards
// exactly those; the engine's third arg is never touched, so (void*, void*) is safe by the same
// callee-body argument as PlayCatchSound.
static const char* SIG_NOTIFY =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 49 38 48 85 C9 ?? ?? 48 85 D2 ?? ?? "
    "E8 ?? ?? ?? ??";
// Gate 3 is USoundBase::IsLooping (Epic 0x2fccef0). It is neither hooked nor called; it is
// identified by ELIMINATION: if Notify ran with a Sound and a MeshComp and PlaySound still did not,
// the only remaining gate is the looping test. That is one less signature to own for the same answer.
// FName::ToString(FString&) -- Epic 0x133a4f0 / Steam 0x12fb7b0. CALLED, never hooked, and only from
// the pump (never inside a game callstack). Sig copied verbatim from src/game/game_syms.cpp.
static const char* SIG_FNAME_TOSTR =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00";
// ---- the SELF-PLAY pair. Both CALLED, never hooked; sigs copied verbatim from game_syms.cpp, where
// they are proven on both exes.
// StaticFindObject(UClass*, Outer, const TCHAR* name, bool exactClass) -- Epic 0x1530ec0 / Steam 0x14f2130
static const char* SIG_STATIC_FIND =
    "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 "
    "45 0F B6 F1 49 8B F8 48 8B DA 4C 8B";
// UGameplayStatics::SpawnSoundAttached -- Epic 0x2c5c220 / Steam 0x2c1ea00. The ENGINE spawn, chosen
// over UReplayAudioManager's wrapper deliberately: the wrapper BROADCASTS to the replay recorder, and
// a sound the game never made should not be written into the recording as if it had.
static const char* SIG_GS_SPAWN_ATT =
    "4C 8B DC 4D 89 43 18 55 53 56 57 41 54 41 56 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 8B 05 "
    "?? ?? ?? ??";

typedef void* (*PlayCatchFn)(void*, void*);
typedef bool  (*ShouldCatchFn)(void*);
typedef void  (*NotifyFn)(void*, void*);
typedef void  (*FNameToStrFn)(const void*, void*);
typedef void* (*StaticFindFn)(void* cls, void* outer, const wchar_t* name, int exactClass);
// 13 args, measured (game_syms.h). This one is CALLED, so the compiler lays out the ABI -- the arity
// traps that govern thunks do not apply here; only getting the list right matters.
typedef void* (*SpawnAttachedFn)(void* sound, void* attachTo, uint64_t attachPoint, const float* loc,
                                 const float* rot, int locType, bool stopWhenDetached, float vol,
                                 float pitch, float start, void* atten, void* conc, bool autoDestroy);
static void* g_origPlay = nullptr,   *g_startPlay = nullptr;
static void* g_origShould = nullptr, *g_startShould = nullptr;
static void* g_origNotify = nullptr, *g_startNotify = nullptr;
static FNameToStrFn    g_fnameToStr  = nullptr;
static StaticFindFn    g_staticFind  = nullptr;
static SpawnAttachedFn g_spawnAtt    = nullptr;
static int             g_okSelfPlay  = 1;   // runtime kill-switch, never saved
// PlaySound's vtable slot, read straight out of the dispatcher (`call [rax + 0x298]`). Comparing a
// notify's own slot against the resolved PlayCatchSound address is an exact class test that needs no
// per-exe vtable RVA -- it works on Epic and Steam alike, and it survives the MinHook detour because
// the vtable still points at the patched function start.
enum { VT_PLAYSOUND_SLOT = 0x298 / 8 };
static void* g_catchCue = nullptr;   // the first catch cue ever seen -- primes the self-spawn

// ------------------------------------------------------------------ cross-hook handoff
// Anim-notify dispatch is game-thread by contract, but nothing proves it is the only thread, and the
// state below must never leak between threads. thread_local costs nothing and removes the question
// (the same reason src/game/audio.cpp uses it for its funnel flags).
static thread_local int   t_inNotify   = 0;
static thread_local int   t_verdict    = -1;   // -1 = the gate never ran for this notify
static thread_local void* t_shouldThis = nullptr;
static thread_local int   t_playRan    = 0;    // did PlaySound run inside this Notify? (gate 3 by elimination)

static double CsNow() {   // QPC: GetTickCount64's 15 ms granularity is noise inside a 150 ms window
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static bool LogBudget() { return InterlockedIncrement(&g_logLines) <= kLogBudget; }
// ManualMode() is -1 when the ini never said which ECatchMode value is manual, and an unreadable
// catchMode is ALSO -1, so comparing them directly would label every unknown case "manual" and
// corrupt the statistic.
static bool IsManual(int catchMode) {
    const int m = CatchTweaks_ManualMode();
    return m >= 0 && catchMode == m;
}

// ---- FName -> string. FName::ToString hands back a GAME-ALLOCATED FString that cannot be freed
// here (that would need the game's allocator, which this DLL deliberately does not couple to). An
// FName's text never changes, so caching by the FName value bounds this at exactly one leak per
// distinct name.
static bool NameOf(const void* obj, char* out, int cap) {
    out[0] = 0;
    if (!obj || !g_fnameToStr) return false;
    const void* fn = (const uint8_t*)obj + OBJ_NAME;
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

// ---- an independent read of the blacklist, used two ways: to cross-check the hooked verdict (if
// they disagree, one of the four inferred offsets is wrong and only the telemetry, never a fix, is
// affected), and to dump the list by name once per session.
// Returns: -1 unreadable, 0 not blacklisted, 1 blacklisted.
static int DerivedVerdict(void* skater) {
    if (!skater) return -1;
    void* ad = twkP(skater, SK_AUDIO_DATA);
    if (!ad) return 0;                                    // the game's own answer with no audio data
    void* data = twkP(ad, AD_NOCATCH_DATA);
    const int num = twkI(ad, AD_NOCATCH_NUM);
    if (num < 0 || num > 512) return -1;                  // implausible -> refuse to walk it
    if (num == 0) return 0;
    if (!data) return -1;
    void* cur = twkP(skater, SK_CUR_FLIP_DEF);
    for (int i = 0; i < num; i++)
        if (twkP(data, i * 8) == cur) return 1;
    return 0;
}

// ------------------------------------------------------------------ SELF-PLAY
// On a flip-trick catch no sound notify of any kind fires: those catch animations carry no sound
// notify, so there is nothing upstream to un-gate, un-quiet or re-anchor. The cue is therefore
// played here on the catch edge, which is what the notify would have done had one been authored.
//
// Two ways to obtain the cue, preferring the one that cannot be wrong:
//   1. the pointer captured from a real catch notify (correct type by construction), or
//   2. StaticFindObject by name. That searches ANY class and returns the FIRST match, so a
//      redirector or generated class with the same name would come back looking like a cue and
//      spawning it would fault. Hence the class-name check below.
static bool  g_cueTried = false;
static void* ResolveCue() {
    if (g_catchCue) return g_catchCue;                 // learned from a notify: trustworthy
    if (g_cueTried || !g_staticFind) return nullptr;
    g_cueTried = true;
    wchar_t wide[64];
    int i = 0;
    for (; kCueName[i] && i < 63; i++) wide[i] = (wchar_t)(uint8_t)kCueName[i];
    wide[i] = 0;
    void* o = nullptr;
    __try { o = g_staticFind(nullptr, (void*)(intptr_t)-1, wide, 0); }   // ANY_PACKAGE
    __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }
    if (!o) { TwkLog("[csnd] cue '%s' not found in this install -- self-play unavailable", kCueName); return nullptr; }
    char cls[64];
    if (!NameOf(twkP(o, OBJ_CLASS), cls, sizeof(cls)) || !strstr(cls, "Sound")) {
        TwkLog("[csnd] '%s' resolved to a %s, not a sound -- refusing to play it",
               kCueName, cls[0] ? cls : "?");
        return nullptr;
    }
    TwkLog("[csnd] cue '%s' resolved by name (%s) -- self-play armed", kCueName, cls);
    g_catchCue = o;
    return o;
}

// Play it on the skater. Attached, so it rides the skater instead of staying at the spawn point,
// which is what the stock notify does when bFollow is set.
static bool PlayCatchCue(void* skater) {
    if (!g_okSelfPlay || !g_spawnAtt || !skater) return false;
    void* cue = ResolveCue();
    if (!cue) return false;
    void* root = twkP(skater, 0x130);                  // AActor::RootComponent (USceneComponent)
    if (!root) return false;
    float vol = (float)g_volPct / 100.0f;
    const float floorV = (float)g_minVolPct / 100.0f;
    if (vol < floorV) vol = floorV;
    if (vol > 4.0f)   vol = 4.0f;
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    __try {
        g_spawnAtt(cue, root, 0 /*NAME_None*/, zero, zero, 0 /*KeepRelativeOffset*/,
                   false /*stopWhenDetached*/, vol, 1.0f, 0.0f,
                   nullptr /*attenuation: the cue's own*/, nullptr /*concurrency: the cue's own*/,
                   true /*autoDestroy -- a one-shot, same as the notify stages*/);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_okSelfPlay = 0;
        TwkLog("[csnd] caught fatal spawning the catch cue -> self-play off (nothing else affected)");
        return false;
    }
    g_lastSelfPlayT = CsNow();      // arms the suppression of a late game notify
    return true;
}

// ------------------------------------------------------------------ hook 1: the gate (fixes A)
static bool hkShouldCatch(void* skater) {
    bool r = true;
    __try { r = ((ShouldCatchFn)g_origShould)(skater); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[csnd] caught fatal in ShouldPlayCatchAudio -> recovered");
        return true;                       // the game's own answer when there is no audio data
    }
    if (t_inNotify) { t_verdict = r ? 1 : 0; t_shouldThis = skater; }
    // There is deliberately no blacklist override here (the old `CatchSoundAlways` ini key is
    // ignored). Every blacklist entry is an ollie or nollie -- tricks with no flip, where nothing is
    // caught and the silence is intentional -- while flip tricks are already allowed through this
    // gate, so overriding it only adds a catch sound to ollies. The real gap is covered by self-play.
    // This hook only OBSERVES, so the log can still report which verdict each notify got.
    return r;
}

// ------------------------------------------------------------------ the notify record ring
// The hook stores RAW SCALARS ONLY -- no logging, no game calls (both are forbidden from inside a
// game callstack). The pump resolves names and writes the lines.
struct NotifyRec {
    double t;
    void*  skater; void* soundObj; void* trickDef;
    float  volStock, volApplied, pitch;
    int    verdict;        // -1 gate never ran (not a skater) / 0 blacklisted / 1 allowed / 2 overridden
    int    derived;        // the independent re-derivation: -1 unreadable / 0 no / 1 blacklisted
    int    follow, catchSt, catchMode, sameSkater;
    bool   muted;          // arrived after this catch was already played -- denied its cue
    bool   matched, logged;
    volatile LONG ready;   // published LAST so the pump never reads a half-written record
};
static const int kNotifyRing = 24;
static NotifyRec g_ring[kNotifyRing];
static volatile LONG g_ringSeq = 0;
static void* g_lastNotifySkater = nullptr;

// ------------------------------------------------------------------ hook 3: the DISPATCHER (measures C)
// PlaySound is only reached once Notify clears three gates, so "the animation never reached the
// notify" and "the notify is there but has no cue" produce identical silence. This ring separates
// them. It records EVERY recorded-sound notify, not just catch ones, because if some other notify
// class is really the catch sound the only way to find out is to see what fires beside a catch.
struct DispatchRec {
    double t;
    void*  notifyObj; void* soundObj;
    int    isCatch, meshNull, reachedPlay;
    bool   logged;
    volatile LONG ready;
};
static const int kDispRing = 48;
static DispatchRec g_disp[kDispRing];
static volatile LONG g_dispSeq = 0;
static volatile LONG g_uiCatchNotify = 0;   // catch-class notifies that reached the DISPATCHER

static void hkNotify(void* self, void* meshComp) {
    const int outer = t_playRan;            // nested notifies are not expected, but cost nothing to allow
    t_playRan = 0;
    void* sound = nullptr;
    int   isCatch = 0;
    __try {
        sound = self ? twkP(self, NTF_SOUND) : nullptr;
        void** vtbl = self ? (void**)twkP(self, 0) : nullptr;
        if (vtbl && g_startPlay) isCatch = (twkP(vtbl, VT_PLAYSOUND_SLOT * 8) == g_startPlay) ? 1 : 0;
        if (isCatch && sound && !g_catchCue) {
            g_catchCue = sound;             // first cue ever seen; self-play can then spawn it directly
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try { ((NotifyFn)g_origNotify)(self, meshComp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[csnd] caught fatal in Notify -> recovered");
    }

    if (g_diag) {
        __try {
            if (isCatch) InterlockedIncrement(&g_uiCatchNotify);
            const LONG seq = InterlockedIncrement(&g_dispSeq) - 1;
            DispatchRec& d = g_disp[seq % kDispRing];
            d.ready = 0;
            d.t = CsNow(); d.notifyObj = self; d.soundObj = sound;
            d.isCatch = isCatch; d.meshNull = meshComp ? 0 : 1; d.reachedPlay = t_playRan;
            d.logged = false;
            InterlockedExchange(&d.ready, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    t_playRan = outer;
}

// ------------------------------------------------------------------ hook 2: the spawn (fixes B + the anchor)
static void* hkPlayCatchSound(void* self, void* meshComp) {
    const double tNow = CsNow();
    // ---- ONE SOUND PER CATCH. If this catch was already self-played, deny the game's late notify
    // its cue rather than let both land. On a flip trick this notify arrives ~250-300 ms after the
    // catch engages (its own log lines read catchState=0, i.e. the catch is already over), so no
    // grace period can wait it out and it has to be handled after the fact.
    // The call is NOT skipped. This function also runs HandleOnAnimationPlayCatchSound, which stops
    // the in-air flip whoosh (board+0x348 _flippingInAirLoopAudio), so returning early would
    // silently change that too. Nulling `Sound` for the duration lets everything else happen while
    // the base spawn no-ops (UGameplayStatics early-returns on a null sound).
    bool  muted = false;
    void* soundSaved = nullptr;
    if (g_on && g_selfPlay && g_suppressMs > 0 && g_lastSelfPlayT > 0.0 && self &&
        (tNow - g_lastSelfPlayT) * 1000.0 < (double)g_suppressMs) {
        __try {
            soundSaved = *(void**)((uint8_t*)self + NTF_SOUND);
            if (soundSaved) {
                *(void**)((uint8_t*)self + NTF_SOUND) = nullptr;
                muted = true;
                InterlockedIncrement(&g_uiSuppressed);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { muted = false; }
    }
    t_playRan++;
    if (!muted) g_lastPlayT = tNow;   // the game is making the sound, so self-play must stand down
    t_inNotify++;
    t_verdict = -1;
    t_shouldThis = nullptr;

    // Stack locals, never statics: two skaters can drive the same notify ASSET in one frame, so the
    // saves must NEST rather than overwrite.
    uint32_t volSaved = 0, folSaved = 0;
    bool  volTouched = false, folTouched = false;
    float volStock = -1.0f, volApplied = -1.0f, pitch = -1.0f;
    int   followStock = -1;

    if (self) {
        volStock    = twkF(self, NTF_VOLUME);
        pitch       = twkF(self, NTF_PITCH);
        followStock = twkB(self, NTF_FOLLOW);
        // Plausibility gate, epsilon-based, never `== 0.0f`: a dead pointer can hold tiny nonzero
        // values that merely PRINT as 0.00. An authored VolumeMultiplier is (0, 10]; anything else
        // means +0x40 does not describe this object, so refuse to write and say so once.
        if (!(volStock > 1e-4f && volStock <= 10.0f) && g_okVol) {
            g_okVol = 0;
            TwkLog("[csnd] VolumeMultiplier reads %.4f at +0x40 -- that offset does not describe this "
                   "notify. Volume/anchor fixes off; the blacklist fix and diagnosis continue.", volStock);
            g_okFollow = 0;
        }
        if (g_on && g_okVol) {
            float want = volStock * ((float)g_volPct / 100.0f);
            const float floorV = (float)g_minVolPct / 100.0f;
            if (want < floorV) want = floorV;
            if (want > 4.0f)   want = 4.0f;                       // never drive the mixer into clipping
            if (want != volStock) {
                __try {
                    volSaved = *(uint32_t*)((uint8_t*)self + NTF_VOLUME);
                    *(float*)((uint8_t*)self + NTF_VOLUME) = want;
                    volTouched = true; volApplied = want;
                } __except (EXCEPTION_EXECUTE_HANDLER) { g_okVol = 0; }
            }
        }
        // The stock spawn anchors the one-shot at a FIXED WORLD POINT (bFollow clear -> the base
        // takes SpawnSoundAtLocation at the mesh's position), so at skating speed the skater rides
        // away from it while it plays. Setting bit0 attaches it to the mesh instead.
        if (g_on && g_follow && g_okFollow && followStock >= 0 && !(followStock & 1)) {
            __try {
                folSaved = *(uint32_t*)((uint8_t*)self + NTF_FOLLOW);
                *(uint32_t*)((uint8_t*)self + NTF_FOLLOW) = folSaved | 1u;
                folTouched = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_okFollow = 0; }
        }
    }

    void* r = nullptr;
    __try { r = ((PlayCatchFn)g_origPlay)(self, meshComp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1)
            TwkLog("[csnd] caught fatal in PlayCatchSound -> recovered");
    }
    // Restore ALWAYS, outside the __try and before any other post-call work. The notify is a SHARED
    // ASSET, so the modification window must be exactly the length of the call.
    if (muted)      { __try { *(void**)((uint8_t*)self + NTF_SOUND) = soundSaved; }
                      __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (volTouched) { __try { *(uint32_t*)((uint8_t*)self + NTF_VOLUME) = volSaved; }
                      __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (folTouched) { __try { *(uint32_t*)((uint8_t*)self + NTF_FOLLOW) = folSaved; }
                      __except (EXCEPTION_EXECUTE_HANDLER) {} }

    __try {
        void* skater = meshComp ? twkP(meshComp, COMP_OWNER) : nullptr;
        if (skater) g_lastNotifySkater = skater;
        if (volTouched)                                     InterlockedIncrement(&g_uiQuiet);
        if (self && !twkP(self, NTF_SOUND))                 InterlockedIncrement(&g_uiNullCue);
        if (t_verdict == 0)                                 InterlockedIncrement(&g_uiBlacklisted);
        InterlockedIncrement(&g_uiNotify);

        if (g_diag) {
            const LONG seq = InterlockedIncrement(&g_ringSeq) - 1;
            NotifyRec& n = g_ring[seq % kNotifyRing];
            n.ready      = 0;
            n.t          = CsNow();
            n.skater     = skater;
            n.soundObj   = self ? twkP(self, NTF_SOUND) : nullptr;
            n.trickDef   = skater ? twkP(skater, SK_CUR_FLIP_DEF) : nullptr;
            n.volStock   = volStock;   n.volApplied = volApplied;   n.pitch = pitch;
            n.verdict    = t_verdict;
            n.derived    = DerivedVerdict(skater);
            n.follow     = followStock;
            n.catchSt    = skater ? twkB(skater, SK_CATCH_ORIENT_ST) : -1;
            n.catchMode  = skater ? twkB(skater, SK_CATCH_MODE) : -1;
            // The two pointers are derived by INDEPENDENT routes (MeshComp+0xa0 here, rcx in the
            // gate hook). Agreement confirms both arities on a live case; disagreement means one of
            // them is wrong.
            n.sameSkater = (t_shouldThis && skater) ? (t_shouldThis == skater ? 1 : 0) : -1;
            n.muted   = muted;
            n.matched = false; n.logged = false;
            InterlockedExchange(&n.ready, 1);   // full barrier: publishes the record, never half of it
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_diag = 0; }

    t_inNotify--;
    return r;
}

// ------------------------------------------------------------------ the catch-edge watch (measures C)
// The rising edge of _catchOrientState is the "board hit the foot" detector (catch_level uses the
// same one). A catch edge with no notify beside it means the notify never fired.
struct EdgeRec { double t; void* skater; void* trickDef; int catchMode; bool live; bool selfDone; };
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
    if (!g_startPlay) return;
    InterlockedIncrement(&g_pumpTicks);
    __try {
        const double now = CsNow();
        const double win = (double)g_windowMs / 1000.0;
        char cue[64], trick[64];

        // ---- 1. drain the notify ring. Deferred by one window so a matching edge (which the pump
        // cannot see until the NEXT tick, see the ordering note below) has had its chance.
        if (g_diag) {
            for (int i = 0; i < kNotifyRing; i++) {
                NotifyRec& n = g_ring[i];
                if (!n.ready || n.logged || now - n.t <= win) continue;
                n.logged = true;
                if (!LogBudget()) continue;
                const char* verdict = (n.verdict == 0) ? "silenced by design (blacklisted trick)"
                                    : (n.verdict == 1) ? "allowed" : "gate never ran (owner not a skater)";
                cue[0] = trick[0] = 0;
                if (!n.soundObj || !NameOf(n.soundObj, cue, sizeof(cue)))   strcpy(cue, n.soundObj ? "?" : "NULL");
                if (!n.trickDef || !NameOf(n.trickDef, trick, sizeof(trick))) strcpy(trick, n.trickDef ? "?" : "none");
                char vol[48];
                if (n.volApplied >= 0.0f) snprintf(vol, sizeof(vol), "%.2f->%.2f", n.volStock, n.volApplied);
                else                      snprintf(vol, sizeof(vol), "%.2f", n.volStock);
                TwkLog("[csnd] notify: %s%s | trick='%s' cue='%s' vol=%s pitch=%.2f followStock=%d "
                       "catchState=%d mode=%s ours=%d sameSkater=%d",
                       verdict, n.muted ? " -- LATE, we already played it, so muted" : "",
                       trick, cue, vol, n.pitch, n.follow & 1, n.catchSt,
                       IsManual(n.catchMode) ? "manual" : "auto", n.derived, n.sameSkater);
            }
        }

        // ---- 1b. CATCH-CLASS dispatches always get a line: this names which of the three gates ate
        // the sound, or shows that the notify was never dispatched at all.
        if (g_diag) {
            for (int i = 0; i < kDispRing; i++) {
                DispatchRec& d = g_disp[i];
                if (!d.ready || d.logged || !d.isCatch || now - d.t <= win) continue;
                d.logged = true;
                if (d.reachedPlay) continue;             // it played; the notify line above covers it
                if (!LogBudget()) continue;
                cue[0] = 0;
                const char* why = !d.soundObj ? "the notify has NO Sound asset (nothing to play)"
                                : d.meshNull  ? "MeshComp was null"
                                              : "the cue is LOOPING -- UE suppresses looping cues from notifies";
                if (d.soundObj && NameOf(d.soundObj, cue, sizeof(cue)))
                    TwkLog("[csnd] catch notify DISPATCHED but stopped: %s (cue='%s')", why, cue);
                else
                    TwkLog("[csnd] catch notify DISPATCHED but stopped: %s", why);
            }
        }

        // ---- 2. the catch edge
        void* skater = CatchTweaks_Skater();
        if (!skater) skater = g_lastNotifySkater;
        if (skater) DumpBlacklist(skater);
        // The edge VERDICT is only meaningful when the notify ring is being filled: with diag off
        // there are no records to match against and every edge would read as "no notify". The edge
        // itself is also what self-play triggers on, so the detector runs for either consumer and
        // only the verdict rides g_diag.
        if (g_okEdge && skater && (g_diag || (g_on && g_selfPlay))) {
            const int st = twkB(skater, SK_CATCH_ORIENT_ST);
            if (st > 0 && g_lastCatchSt == 0) {
                EdgeRec& e = g_edges[g_edgeHead];
                g_edgeHead = (g_edgeHead + 1) % kEdgeRing;
                e.t = now; e.skater = skater;
                e.trickDef = twkP(skater, SK_CUR_FLIP_DEF);
                e.catchMode = twkB(skater, SK_CATCH_MODE);
                e.live = true; e.selfDone = false;
                InterlockedIncrement(&g_uiEdges);
                if (IsManual(e.catchMode)) InterlockedIncrement(&g_uiEdgesManual);
            }
            if (st >= 0) g_lastCatchSt = st;
        }

        // ---- 2b. SELF-PLAY. Short grace first: the pump sees the edge one tick after the catch, so
        // a notify that was going to fire has already fired, but g_selfDelay covers the case where
        // it lands a frame or two later, at the cost of being that much behind the visual.
        if (g_on && g_selfPlay && g_okSelfPlay) {
            const double grace = (double)g_selfDelay / 1000.0;
            for (int i = 0; i < kEdgeRing; i++) {
                EdgeRec& e = g_edges[i];
                if (!e.live || e.selfDone || now - e.t < grace) continue;
                e.selfDone = true;
                if (fabs(g_lastPlayT - e.t) <= win) continue;      // the game already made it
                // Honour the designers' own silence list on this path too, or self-play would simply
                // reintroduce the unwanted ollie sound. In practice no catch edge fires on a plain
                // ollie, so this is a guard rather than a fix.
                if (DerivedVerdict(e.skater) == 1) { InterlockedIncrement(&g_uiBlacklisted); continue; }
                if (PlayCatchCue(e.skater)) InterlockedIncrement(&g_uiSelfPlayed);
            }
        }

        // ---- 3. resolve edges that are old enough to judge.
        // Ordering hazard: the pump runs EARLY in the frame, the catch engages during movement, and
        // the notify fires later in the SAME frame, so the pump does not see the rising edge until
        // the next tick, by which time the notify is already in the past. The correlation window is
        // therefore TWO-SIDED; a one-sided "notify after the edge?" test would miss systematically.
        for (int i = 0; i < kEdgeRing; i++) {
            EdgeRec& e = g_edges[i];
            if (!e.live || now - e.t <= win) continue;
            e.live = false;
            if (!g_diag) continue;                 // ring not being filled -> no verdict is possible
            bool hit = false;
            for (int j = 0; j < kNotifyRing; j++) {
                const NotifyRec& n = g_ring[j];
                if (!n.ready || n.skater != e.skater) continue;
                if (fabs(n.t - e.t) <= win) { g_ring[j].matched = true; hit = true; }
            }
            if (hit) continue;
            const bool covered = e.selfDone && g_on && g_selfPlay && g_okSelfPlay;
            if (!covered) InterlockedIncrement(&g_uiEdgeNoNotify);   // only count a catch that stayed SILENT
            const bool manual = IsManual(e.catchMode);
            if (!covered && manual) InterlockedIncrement(&g_uiSilentManual);
            if (LogBudget()) {
                trick[0] = 0;
                if (!e.trickDef || !NameOf(e.trickDef, trick, sizeof(trick)))
                    strcpy(trick, e.trickDef ? "?" : "none");
                if (covered) {
                    TwkLog("[csnd] catch: the game had no notify, we played it (trick='%s' mode=%s)",
                           trick, manual ? "manual" : "auto");
                    continue;                                       // covered: no need to list what else fired
                }
                TwkLog("[csnd] catch was SILENT -- nothing played it (trick='%s' mode=%s)",
                       trick, manual ? "manual" : "auto");
                // What did fire beside this catch? If the catch sound is really some other notify
                // class, this is the only way it shows itself, and if nothing at all fired that is
                // itself the answer: the animation carries no sound notify here.
                int shown = 0;
                for (int j = 0; j < kDispRing && shown < 6; j++) {
                    const DispatchRec& d = g_disp[j];
                    if (!d.ready || fabs(d.t - e.t) > win) continue;
                    char cls[64]; cls[0] = 0;
                    if (!NameOf(twkP(d.notifyObj, OBJ_CLASS), cls, sizeof(cls))) strcpy(cls, "?");
                    cue[0] = 0;
                    if (!d.soundObj || !NameOf(d.soundObj, cue, sizeof(cue))) strcpy(cue, d.soundObj ? "?" : "NULL");
                    TwkLog("[csnd]    beside it: %s cue='%s' isCatchClass=%d played=%d",
                           cls, cue, d.isCatch, d.reachedPlay);
                    shown++;
                }
                if (!shown) TwkLog("[csnd]    beside it: NO sound notify of any kind fired");
            }
        }

        // ---- 4. retire notify records. An unmatched one is the SELF-CHECK: if the edge detector
        // ever breaks this counter climbs, so "the notify never fires" cannot be read off a dead
        // probe.
        if (g_diag) {
            for (int i = 0; i < kNotifyRing; i++) {
                NotifyRec& n = g_ring[i];
                if (!n.ready || now - n.t <= win * 3.0) continue;
                if (!n.matched) InterlockedIncrement(&g_uiNotifyNoEdge);
                n.ready = 0;
            }
            for (int i = 0; i < kDispRing; i++)
                if (g_disp[i].ready && now - g_disp[i].t > win * 3.0) g_disp[i].ready = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_okEdge = 0; g_diag = 0;
        TwkLog("[csnd] caught fatal in the edge watch -> diagnosis off (the sound fixes are unaffected)");
    }
}

void CatchSound_Install() {
    g_startPlay = TwkScanExe(SIG_PLAY_CATCH);
    if (!g_startPlay) {
        TwkLog("[csnd] UAnimNotify_PlayCatchSound::PlaySound sig NOT FOUND -- catch sound fixes off (game updated?)");
        g_on = 0; return;
    }
    if (MH_CreateHook(g_startPlay, (void*)&hkPlayCatchSound, &g_origPlay) != MH_OK ||
        MH_EnableHook(g_startPlay) != MH_OK) {
        TwkLog("[csnd] hook failed on PlayCatchSound -- catch sound fixes off");
        g_startPlay = nullptr; g_on = 0; return;
    }
    g_startShould = TwkScanExe(SIG_SHOULD_CATCH);
    if (!g_startShould) {
        TwkLog("[csnd] ShouldPlayCatchAudio sig NOT FOUND -- the blacklist fix is off "
               "(volume/anchor fixes still apply)");
        g_okShould = 0;
    } else if (MH_CreateHook(g_startShould, (void*)&hkShouldCatch, &g_origShould) != MH_OK ||
               MH_EnableHook(g_startShould) != MH_OK) {
        TwkLog("[csnd] hook failed on ShouldPlayCatchAudio -- the blacklist fix is off");
        g_startShould = nullptr; g_okShould = 0;
    }
    // The dispatcher probe. Diagnosis only -- it never changes what the game does, so a miss here
    // costs the investigation, not the fixes.
    g_startNotify = TwkScanExe(SIG_NOTIFY);
    if (!g_startNotify) {
        TwkLog("[csnd] PlaySoundRecorded::Notify sig NOT FOUND -- cannot tell 'no notify' from "
               "'notify with no cue'");
    } else if (MH_CreateHook(g_startNotify, (void*)&hkNotify, &g_origNotify) != MH_OK ||
               MH_EnableHook(g_startNotify) != MH_OK) {
        TwkLog("[csnd] hook failed on PlaySoundRecorded::Notify -- dispatcher diagnosis off");
        g_startNotify = nullptr;
    }
    g_fnameToStr = (FNameToStrFn)TwkScanExe(SIG_FNAME_TOSTR);
    if (!g_fnameToStr) TwkLog("[csnd] FName::ToString not found -- log lines will be unnamed");
    // Self-play needs both; either one missing costs self-play only, never the hooks above.
    g_staticFind = (StaticFindFn)TwkScanExe(SIG_STATIC_FIND);
    g_spawnAtt   = (SpawnAttachedFn)TwkScanExe(SIG_GS_SPAWN_ATT);
    if (!g_staticFind || !g_spawnAtt) {
        TwkLog("[csnd] self-play unavailable (StaticFindObject=%p SpawnSoundAttached=%p)",
               g_staticFind, g_spawnAtt);
        g_okSelfPlay = 0;
    }
    TwkLog("[csnd] installed @ %p (gate @ %p, dispatcher @ %p) -- follow=%d vol>=%d%% x%d%% "
           "selfPlay=%d suppress=%dms (%s)",
           g_startPlay, g_startShould, g_startNotify, g_follow, g_minVolPct, g_volPct,
           g_selfPlay && g_okSelfPlay, g_suppressMs, g_on ? "ON" : "off");
}

void CatchSound_DrawMenu(const OmpMenuApi* api) {
    char b[224];
    if (!g_startPlay) { api->TextDisabled("Catch sound: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Fix the catch sound", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(plays it every catch, and keeps it audible)");
    if (on) {
        api->Indent();
        bool self = g_selfPlay != 0;
        if (api->Checkbox("Play it when the game doesn't", &self)) { g_selfPlay = self ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled(g_okSelfPlay ? "(flip tricks have no catch sound at all)"
                                                        : "(unavailable on this build)");
        if (self) {
            float sd = (float)g_selfDelay;
            if (api->SliderFloat("  Wait first (ms)", &sd, 0.0f, 200.0f, "%.0f")) { g_selfDelay = (int)sd; TwkMarkDirty(); }
        }
        bool follow = g_follow != 0;
        if (api->Checkbox("Sound follows you", &follow)) { g_follow = follow ? 1 : 0; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(stock leaves it at a fixed spot you skate away from)");
        float mn = (float)g_minVolPct;
        if (api->SliderFloat("Minimum volume (%)", &mn, 50.0f, 200.0f, "%.0f")) { g_minVolPct = (int)mn; TwkMarkDirty(); }
        float vp = (float)g_volPct;
        if (api->SliderFloat("Volume (%)", &vp, 50.0f, 300.0f, "%.0f")) { g_volPct = (int)vp; TwkMarkDirty(); }
        bool diag = g_diag != 0;
        if (api->Checkbox("Log what happens on each catch", &diag)) { g_diag = diag ? 1 : 0; TwkMarkDirty(); }
        snprintf(b, sizeof(b), "catches %d  |  the game played %d  |  we played %d  |  STILL SILENT %d",
                 (int)g_uiEdges, (int)g_uiNotify, (int)g_uiSelfPlayed, (int)g_uiEdgeNoNotify);
        api->TextDisabled(b);
        snprintf(b, sizeof(b), "late game notifies swallowed %d  |  by design (ollies) %d  |  cue: %s",
                 (int)g_uiSuppressed, (int)g_uiBlacklisted,
                 g_catchCue ? kCueName : "not resolved yet");
        api->TextDisabled(b);
        if (g_uiNotifyNoEdge > 0 || g_pumpTicks == 0) {
            snprintf(b, sizeof(b), "(diag: notifies with no catch edge %d, pump ticks %d)",
                     (int)g_uiNotifyNoEdge, (int)g_pumpTicks);
            api->TextDisabled(b);
        }
        api->Unindent();
    }
}
