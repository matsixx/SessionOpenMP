// SessionTweaks -- THE RADIO: the first prop. A speaker you take out, carry under your arm, put down and
// walk away from, playing the game's own music from where it stands -- and, in co-op, everybody else's too.
// Copyright (C) 2026 matsix -- GNU GPL v3 or later, with the section 7 permission in LICENSE-EXCEPTION.txt.
//
// WHAT IT IS MADE OF (all of it the game's):
//  - THE MESH. The game has no radio or boombox; it has speakers. This is the skate shop's
//    (STM_NYC_SkateshopSpeaker), loaded by path (it is only in memory in the shop), on a plain
//    AStaticMeshActor, made movable, collision off. Nothing is assumed about its shape: its bounds are read off
//    the mesh and it is CARRIED BY SHAPE (emote.cpp DoCarry) and SET DOWN on its bounds' bottom.
//  - THE MUSIC. The game ships 148 songs in three stations (UMusicStationDefinition::_songList ->
//    USongDefinition::_soundWave). Every player has them, so NOTHING IS STREAMED: a radio's state is a station,
//    a song and how far in. Played with the ENGINE'S OWN UGameplayStatics::SpawnSoundAttached, NOT the replay
//    manager's wrapper: that one records into replays and feeds the co-op audio funnel, and a radio is each
//    game's own to play.
//  - THE CHANNEL. A song is a SNDCLASS_Music sound, so it was silent with the Music slider down -- and turning
//    that up turns the game's own music on as well (the field). So it is played under another class
//    (UAudioComponent::SoundClassOverride, SNDCLASS_Sfx unless the ini says otherwise): the Sound slider is the
//    radio's, the Music slider is the game's. The override is read when a sound STARTS, so the component is
//    spawned all but silent, given its class and its volume, and played again from the same second.
//  - THE FALLOFF. SAT_Sfx_Apt_Radio, the attenuation the apartment's radio uses; by distance here if it cannot be had.
//
// HOLDING IT is an emote (EM_CARRY, not on the wheel): the pose works out where the actor belongs, this puts it
// there and, once the pose is fully up, HANGS IT FROM THE CHEST BONE so it rides the body with no frame of lag.
// B puts it down. Getting on the board, sitting or an editor ends the carry; the radio is set down at your feet.
//
// CO-OP (OpenMP's mod channel, "sessiontweaks.radio"; radio_wire.h is the message). Every player owns ONE radio
// and is the only one who speaks for it: no authority, nothing to merge. Its owner sends its whole state when it
// changes, to a newcomer when they appear, and as a heartbeat. Everyone else BUILDS that radio in their own
// world: set down where the owner says, or hung from the chest bone of the owner's character in THEIR game, at
// the same place relative to it (the owner's arm is already posed there: the emote's pose travels with the
// skeleton). The song is played locally from the second the owner says. An owner on another map has no
// character here, and so no radio here.
//
// ITS VOLUME IS ITS OWN, AND YOURS. The field: at the engine's x1 it was "very loud". Every radio has a level of
// its own IN THIS GAME (Radio::volume), turned up and down from the wheel for the radio you hold or stand at --
// like the mute, nothing is sent: what you hear is yours to set, radio by radio. Your OWN radio's level is kept
// in the ini (RadioVolumePct) and is what any radio you have not touched starts at.
//
// STREAMING: instead of the game's stations, a radio can play ONE APP from its owner's PC -- Spotify, a browser,
// a media player; never the game, a voice call or system sounds (Windows' process loopback, Windows 11 / 10
// build 20348+). OpenMP owns the capture, the lane ("OMPr"), the decoding and the wave (OmpRadio_*: a bridge
// like OmpSession_*); this module owns the prop, the choice of app, and the ASK: a listener's radio playing that
// owner's stream (and not muted) tells the owner "send it to me" every few seconds over this channel, and the
// owner sends to exactly those players -- so nobody who is not listening costs anything. The owner hears the
// app straight from their own PC; their radio plays nothing for them (it would be the same song twice).
//
// MUTING IS LOCAL, by design ("only turn off locally for whoever turned it off"): it stops THIS game's sound of
// THAT radio -- yours or anybody's you walk up to -- and nothing is sent. Unmuting rejoins the song where it is.
//
// EVERY pointer kept across frames is watched (SitUI_Track / SitUI_Alive): actors and sounds die with the level,
// and an address is not an identity. Other players' actors are asked for again every frame, never kept.
#include <windows.h>
#include <psapi.h>          // EnumProcessModules: OpenMP's stream bridge is found by its exports
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tweaks_common.h"
#include "radio.h"
#include "radio_wire.h"
#include "radial.h"          // the actor kit: spawn / destroy / place / movable
#include "sit.h"             // attach / detach, the ground trace, Sit_PoseHeld
#include "sit_ui.h"          // SitObjRef: is what we remember still that thing
#include "emote.h"           // the carry pose; the chest bone's name
#include "catch_tweaks.h"    // the skater; an FName from a string
#include "catch_sound.h"     // a loaded object by name
#include "../../sdk/omp_mod_api.h"

namespace {

enum {
    ACT_ROOT     = 0x130,   // AActor::RootComponent
    SC_C2W       = 0x1c0,   // USceneComponent::ComponentToWorld (quat, +0x10 pos, +0x20 scale)
    SC_REL_LOC   = 0x11c,   // USceneComponent::RelativeLocation
    SC_REL_ROT   = 0x128,   //   ...RelativeRotation (pitch, yaw, roll)
    CH_MESH      = 0x280,   // ACharacter::Mesh
    SMA_COMP     = 0x220,   // AStaticMeshActor::StaticMeshComponent
    STM_BOUNDS   = 0x110,   // UStaticMesh::ExtendedBounds (FBoxSphereBounds: origin 3f, extent 3f, radius)
    MSD_SONGS    = 0x58,    // UMusicStationDefinition::_songList (TArray<USongDefinition*>)
    SONG_WAVE    = 0x60,    // USongDefinition::_soundWave
    SND_DURATION = 0x108,   // USoundBase::Duration (seconds)
    AC_CLASS_OVR = 0x210,   // UAudioComponent::SoundClassOverride
    AC_VOLUME    = 0x238,   // UAudioComponent::VolumeMultiplier
};
// bool UStaticMeshComponent::SetStaticMesh(UStaticMesh*)            Epic 0x2b90740 / Steam 0x2b52f80
const char* SIG_SET_STATIC_MESH =
    "40 55 53 56 57 48 8D 6C 24 B8 48 81 EC 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 48 8B FA 48 8B D9 48 3B 91 88 04 00 00";
// void AActor::SetActorEnableCollision(bool)                        Epic 0x29cdc60 / Steam 0x2990550
const char* SIG_ACTOR_COLLISION =
    "4C 8B DC 55 48 81 EC 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 00 00 00 48 8B E9 0F B6 49 5C 0F B6 C1 C0 E8 03 24 01";
// UAudioComponent* UGameplayStatics::SpawnSoundAttached(...13 args) Epic 0x2c5c220 / Steam 0x2c1ea00 -- the ENGINE's,
// found as the one call UReplayAudioManager::SpawnSoundAttached forwards to; the same argument list as that wrapper.
const char* SIG_SPAWN_SOUND_ATT =
    "4C 8B DC 4D 89 43 18 55 53 56 57 41 54 41 56 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 48 8B 85 F0 00 00 00";
// void UAudioComponent::Stop()                                      Epic 0x2af3020 / Steam 0x2ab5860
const char* SIG_AUDIO_STOP =
    "40 53 48 83 EC 20 F6 81 8A 00 00 00 01 48 8B D9 ?? ?? E8 ?? ?? ?? ?? 48 85 C0 ?? ?? 48 8B 93 D0 07 00 00 48 8B C8 80 A3 8A 00 00 00 FE";
// void UAudioComponent::Play(float StartTime)                       Epic 0x2aed420 / Steam 0x2aafc60
const char* SIG_AUDIO_PLAY =
    "48 89 5C 24 08 48 89 7C 24 10 55 48 8B EC 48 81 EC 80 00 00 00 33 C0 F3 0F 11 4D A0 BF FF FF FF FF 48 89 45 B0 48 8D 55 A0 48 89 45 B8";
// void UAudioComponent::SetVolumeMultiplier(float)                  Epic 0x2af28e0 / Steam 0x2ab5120
const char* SIG_AUDIO_VOLUME =
    "40 53 48 81 EC 90 00 00 00 F6 81 8A 00 00 00 01 48 8B D9 0F 29 B4 24 80 00 00 00 0F 28 F1 F3 0F 11 B1 38 02 00 00 C7 81 34 02 00 00 00 00 80 3F";
// UObject* FSoftObjectPath::TryLoad(FUObjectSerializeContext*) -- the co-op module's verified signature
const char* SIG_SOFT_TRY_LOAD =
    "48 89 5C 24 08 57 48 83 EC 70 48 8B 01 33 DB 48 8B F9 48 85 C0 0F 84 ?? ?? ?? ?? 83 79 10 01 ?? ?? 48 8D 4C 24 50 48 89 44 24 50";
// void USceneComponent::SetRelativeLocationAndRotation(FVector, FRotator, bool bSweep, FHitResult*, ETeleportType)
// -- both vectors by pointer.                                        Epic 0x2b653f0 / Steam 0x2b27c30
const char* SIG_COMP_SET_REL =
    "48 8B C4 48 89 58 08 57 48 81 EC 80 00 00 00 F2 0F 10 81 1C 01 00 00 41 0F B6 F9 0F 2E 02 48 8B D9 F2 0F 11 40 A8 0F 85 ?? ?? ?? ??";

typedef bool  (*SetMeshFn)(void* comp, void* mesh);
typedef void  (*ActorCollisionFn)(void* actor, bool on);
typedef void* (*SpawnSoundFn)(void* sound, void* attachTo, uint64_t attachPoint, const float* loc, const float* rot, int locType,
                              bool stopWhenDetached, float vol, float pitch, float start, void* atten, void* conc, bool autoDestroy);
typedef void  (*AudioStopFn)(void* audioComponent);
typedef void  (*AudioPlayFn)(void* audioComponent, float startTime);
typedef void  (*AudioVolumeFn)(void* audioComponent, float v);
typedef void* (*TryLoadFn)(void* softObjectPath, void* loadContext);
typedef void  (*SetRelFn)(void* comp, const float* loc3, const float* rot3, bool sweep, void* hit, int teleport);
SetMeshFn        g_setMesh = nullptr;
ActorCollisionFn g_collision = nullptr;
SpawnSoundFn     g_spawnSound = nullptr;
AudioStopFn      g_audioStop = nullptr;
AudioPlayFn      g_audioPlay = nullptr;
AudioVolumeFn    g_audioVolume = nullptr;
TryLoadFn        g_tryLoad = nullptr;
SetRelFn         g_setRel = nullptr;
bool             g_looked = false, g_ok = false;
char             g_lookWhy[96] = "Radio: not ready";

// ---- settings
int   g_on = 1;
float g_volume = 0.40f;                       // RadioVolumePct: x1 was "very loud" (the field). The start for every radio; the wheel moves each
float g_volumeOthers = 0.10f;                 // RadioVolumeOthersPct: ...and what SOMEONE ELSE'S starts at
float g_rangeMax = 12000.0f;                  // RadioRangeCm: how far a radio at FULL volume carries (120 m)
// RadioCloseCm: "standing AT a speaker", which is what makes a click of the stick mean that speaker.
float g_closeCm = 150.0f;
float g_rangeMin = 800.0f;                    // RadioRangeMinCm: ...and how far one turned right down does (8 m)
float g_falloffStart = 0.45f;                 // RadioFalloffStartPct: full volume out to this much of the range
char  g_attenPath[160] = "";                  // RadioAttenuation: the falloff asset, or "none"
bool  g_rangeDebug = false;                   // RadioRangeDebug: 1/s line of distance vs our own gain
const char* const kMeshPath = "/Game/Art/Env/NYC/Skateshop/Props/Basemesh/Revamp/STM_NYC_SkateshopSpeaker.STM_NYC_SkateshopSpeaker";
char  g_meshPath[200] = "";
char  g_soundClass[64] = "SNDCLASS_Sfx";      // RadioSoundClass: the volume slider it answers to ("" = the song's own: Music)
// THE FALLOFF CURVE. Not a sound -- a SoundAttenuation asset, the shape of how the radio fades with
// distance. The obvious pick, SAT_Sfx_Apt_Radio, is the game's APARTMENT radio: MEASURED out of the pak,
// full volume within 3 m and SILENT PAST 23 m. Right for a radio in a room, hopeless for a speaker in a
// car park -- a peer's radio was inaudible across one at full volume, and no volume setting could reach
// past it, because this is the asset's curve and not ours. The siren's curve has the same 3 m near field
// and falls off over 190 m instead of 20, which is what a speaker outdoors actually does. Its name is
// irrelevant: no siren is involved, only the distance curve.
// Others measured, if this wants changing (RadioAttenuation): SAT_Sfx_Emitter_Pedestrians and
// SAT_Sfx_Emitter_Wolf are identical to the siren; SAT_Sfx_Skate_RE 120 m; SAT_Sfx_Boathorn_Paris 224 m
// (76 m of it at FULL volume); SAT_Sfx_Emitter_Bell 308 m (157 m flat out -- far too much).
const char* const kAttenPath = "/Game/Audio/SAT_Sfx_Emitter_Siren.SAT_Sfx_Emitter_Siren";
const char* const kAttenApt  = "/Game/Audio/SAT_Sfx_Apt_Radio.SAT_Sfx_Apt_Radio";   // the old one, if it is ever wanted back
const struct { const char* name; const char* asset; const char* path; } kStations[3] = {
    { "RedRobin Radio",   "MSD_Station_RedRobin",          "/Game/Audio/Music/RedRobinRadio/MSD_Station_RedRobin.MSD_Station_RedRobin" },
    { "Chillhop Lofi",    "MSD_Station_ChillHop_Lofi",     "/Game/Audio/Music/Chillhop_Lofi/MSD_Station_ChillHop_Lofi.MSD_Station_ChillHop_Lofi" },
    { "Chillhop Uptempo", "MSD_Station_ChillHop_Uptempo",  "/Game/Audio/Music/ChilHop_Uptempo/MSD_Station_ChillHop_Uptempo.MSD_Station_ChillHop_Uptempo" },
};

// ---- a radio: mine, or another player's as it stands in THIS world
enum { R_NONE = 0, R_HELD = 1, R_PLACED = 2, MAX_REMOTE = 16 };
struct Radio {
    bool      used; int player;                  // player -1 = mine
    int       state;                             // mine: what it is. theirs: what is BUILT here (their word is in `last`)
    void*     actor; void* comp; SitObjRef aref, cref;
    bool      attached; void* hungFrom;          // the mesh component it hangs from
    int       station, song; float songPos, songLen;
    bool      muted;                             // LOCAL: this game does not play it
    float     volume; bool volumeSet;            // LOCAL: how loud THIS game plays it (the wheel); unset = g_volume
    void*     sound; SitObjRef sref;
    radiowire::State last; bool have, applied;   // theirs: the owner's last word, and whether it has been built
    float     sinceMsg;
    ULONGLONG nextTryMs;                         // a sound that would not start is not asked for again every frame
    int       source;                            // radiowire::SRC_*: the game's stations, or a stream from the owner's PC
    char      srcName[48]; uint32_t srcPid;      // mine, streaming: which app
    bool      wantSent; float wantBeat;          // theirs, streaming: we have asked for it (and when we last did)
    bool      farOut;                            // we were outside its range: the engine will have STOPPED the sound
};
Radio     g_mine, g_remote[MAX_REMOTE];
float     g_boundsO[3] = { 0, 0, 0 }, g_boundsE[3] = { 30, 14, 7 }; bool g_boundsRead = false;
bool      g_attenMissingSaid = false, g_classSaid = false;
volatile LONG g_reqPutDown = 0;
LONGLONG  g_pumpQpc = 0; double g_qpf = 0.0;
char      g_whyNot[96] = "";
char      g_label[8][64];
int       g_acts[8]; int g_nActs = 0; Radio* g_muteTarget = nullptr;
enum { A_TAKE = 1, A_PUTDOWN, A_PICKUP, A_NEXTSONG, A_NEXTSTATION, A_STREAM, A_MUTE, A_VOLUP, A_VOLDOWN, A_AWAY };
unsigned  g_rng = 0x2545f491u;
// ---- the stream bridge to OpenMP (OmpRadio_*), found by export like the mod API
struct OmpRadioApi {
    int   (*Version)();
    int   (*Sources)(uint32_t* pids, char* names, int nameCap, int cap);
    int   (*StreamStart)(uint32_t pid, const char* name);
    void  (*StreamStop)();
    int   (*StreamState)(char* why, int cap);
    void  (*SetListener)(int player, int wanted);
    void* (*PeerWave)(int player, int rewind);
    void* (*OwnWave)(int rewind);                 // ...and ours, so our own radio is audible to us too
    int   (*PeerStreaming)(int player);
};
OmpRadioApi g_orad = {};
bool BindRadioApi() {
    if (g_orad.Version) return true;
    HMODULE mods[1024]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return false;
    const DWORD n = need / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 1024; i++) {
        FARPROC v = GetProcAddress(mods[i], "OmpRadio_Version");
        if (!v) continue;
        OmpRadioApi a = {};
        a.Version       = (int (*)())v;
        a.Sources       = (int (*)(uint32_t*, char*, int, int))GetProcAddress(mods[i], "OmpRadio_Sources");
        a.StreamStart   = (int (*)(uint32_t, const char*))GetProcAddress(mods[i], "OmpRadio_StreamStart");
        a.StreamStop    = (void (*)())GetProcAddress(mods[i], "OmpRadio_StreamStop");
        a.StreamState   = (int (*)(char*, int))GetProcAddress(mods[i], "OmpRadio_StreamState");
        a.SetListener   = (void (*)(int, int))GetProcAddress(mods[i], "OmpRadio_SetListener");
        a.PeerWave      = (void* (*)(int, int))GetProcAddress(mods[i], "OmpRadio_PeerWave");
        a.OwnWave       = (void* (*)(int))GetProcAddress(mods[i], "OmpRadio_OwnWave");
        a.PeerStreaming = (int (*)(int))GetProcAddress(mods[i], "OmpRadio_PeerStreaming");
        if (!a.Sources || !a.StreamStart || !a.StreamStop || !a.StreamState || !a.SetListener || !a.PeerWave || !a.PeerStreaming) return false;
        g_orad = a;
        TwkLog("[radio] streaming: OpenMP's stream bridge found (v%d)", a.Version());
        return true;
    }
    return false;
}
// what the owner can stream right now: the apps with sound open (refreshed while the wheel is up)
enum { kMaxSrc = 8 };
uint32_t  g_srcPid[kMaxSrc]; char g_srcName[kMaxSrc][48]; int g_srcN = 0, g_streamNext = 0;
char      g_streamErr[96] = ""; ULONGLONG g_streamErrUntil = 0;
// ---- co-op
OmpModApi g_omp; int g_channel = 0; ULONGLONG g_bindTryMs = 0, g_bindWaitMs = 4000;
bool      g_dirty = false; float g_beatS = 0.0f; int g_sent = 0, g_got = 0, g_bad = 0;

bool Look() {
    if (g_looked) return g_ok;
    g_looked = true;
    g_setMesh    = (SetMeshFn)TwkScanExe(SIG_SET_STATIC_MESH);
    g_collision  = (ActorCollisionFn)TwkScanExe(SIG_ACTOR_COLLISION);
    g_spawnSound = (SpawnSoundFn)TwkScanExe(SIG_SPAWN_SOUND_ATT);
    g_audioStop  = (AudioStopFn)TwkScanExe(SIG_AUDIO_STOP);
    g_audioPlay  = (AudioPlayFn)TwkScanExe(SIG_AUDIO_PLAY);
    g_audioVolume = (AudioVolumeFn)TwkScanExe(SIG_AUDIO_VOLUME);
    g_tryLoad    = (TryLoadFn)TwkScanExe(SIG_SOFT_TRY_LOAD);
    g_setRel     = (SetRelFn)TwkScanExe(SIG_COMP_SET_REL);
    g_ok = g_setMesh && g_spawnSound && g_audioStop && g_tryLoad;
    // Without one of these the radio cannot work at all, and calling a function that was not found would crash
    // the game -- so it refuses. The refusal says WHAT is missing (the first version just said "not available").
    if (!g_ok) {
        const char* what = !g_spawnSound ? "sound spawner" : !g_setMesh ? "mesh setter" : !g_audioStop ? "sound stopper" : "asset loader";
        snprintf(g_lookWhy, sizeof(g_lookWhy), "Radio off: game's %s not found", what);      // the wheel shows 63 characters; the log has them all
    }
    TwkLog("[radio] set mesh %s, collision %s, spawn sound %s, stop %s, play %s, volume %s, load %s, set relative %s -> %s",
           g_setMesh ? "ok" : "MISSING", g_collision ? "ok" : "missing", g_spawnSound ? "ok" : "MISSING", g_audioStop ? "ok" : "MISSING",
           g_audioPlay ? "ok" : "missing (it stays on the Music slider)", g_audioVolume ? "ok" : "missing", g_tryLoad ? "ok" : "MISSING",
           g_setRel ? "ok" : "missing (a carried radio of another player's is not shown)", g_ok ? "ready" : "THE RADIO IS OFF in this build");
    return g_ok;
}
// An asset by its full path: found if it is in memory, loaded if it is not. ONE FName is interned per path.
void* LoadByPath(const char* path) {
    if (!g_tryLoad) return nullptr;
    struct { uint64_t name; void* sub; int subNum, subMax; } sp = { 0, nullptr, 0, 0 };       // FSoftObjectPath: FName + FString
    unsigned long long nm = 0;
    if (!CatchTweaks_MakeName(path, true, &nm)) return nullptr;
    sp.name = nm;
    __try { return g_tryLoad(&sp, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool ReadC2W(void* comp, float q[4], float p[3]) {
    if (!comp) return false;
    for (int i = 0; i < 4; i++) q[i] = twkF(comp, SC_C2W + i * 4);
    for (int i = 0; i < 3; i++) p[i] = twkF(comp, SC_C2W + 0x10 + i * 4);
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n2 > 0.9f && n2 < 1.1f;
}
bool Alive(const Radio& r) { return r.actor && SitUI_Alive(&r.aref) && r.comp && SitUI_Alive(&r.cref); }
const char* Whose(const Radio& r, char* buf, int cap) {
    if (r.player < 0) { snprintf(buf, cap, "yours"); return buf; }
    char nm[48] = "";
    if (g_omp.version && g_omp.PlayerName) g_omp.PlayerName(r.player, nm, sizeof(nm));
    snprintf(buf, cap, "%s's", nm[0] ? nm : "a player");
    return buf;
}

// ---- the music
void* Station(int i) {
    void* st = CatchSound_FindObject(kStations[i].asset, "MusicStationDefinition");
    return st ? st : LoadByPath(kStations[i].path);
}
int SongCount(void* station) { const int n = station ? twkI(station, MSD_SONGS + 8) : 0; return (n > 0 && n < 1000) ? n : 0; }
void* SongWave(void* station, int i) {
    const int n = SongCount(station);
    if (i < 0 || i >= n) return nullptr;
    void* list = twkP(station, MSD_SONGS);
    void* song = list ? twkP(list, i * 8) : nullptr;
    return song ? twkP(song, SONG_WAVE) : nullptr;
}
// SOMEONE ELSE'S RADIO STARTS QUIETER THAN YOUR OWN. You chose to put yours down and you know what is on
// it; theirs is something you walked past, and in a busy lobby there may be several. So an untouched radio
// of a peer's starts at RadioVolumeOthersPct rather than your own level -- turn any of them up or down from
// the wheel and that one keeps whatever you gave it.
float VolumeOf(const Radio& r) { return r.volumeSet ? r.volume : (r.player < 0 ? g_volume : g_volumeOthers); }
void StopSound(Radio& r) {
    // The component is spawned with autoDestroy FALSE (see SpawnOn), so stopping is ours to do. It is
    // attached to the radio's own component and dies with the prop, so a stopped one left behind costs a
    // component per song change -- a handful in a long session, against a restart every 2 s. There is no
    // DestroyComponent bound here and this is not the round to add one.
    if (r.sound && SitUI_Alive(&r.sref) && g_audioStop) { __try { g_audioStop(r.sound); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    r.sound = nullptr; r.sref.obj = nullptr; r.farOut = false;
}
void* SpawnOn(Radio& r, void* wave, float start);
// Start THIS game's sound of what a radio is playing, from where in the song the radio is -- or, for a radio that
// is STREAMING, the stream OpenMP is decoding for that player (yours is not played: you hear the app itself).
void StartSound(Radio& r) {
    StopSound(r);
    if (r.muted || !Alive(r) || !g_spawnSound) return;
    r.nextTryMs = GetTickCount64() + 2000;
    if (r.source == radiowire::SRC_STREAM) {
        r.songLen = 0.0f;
        // OURS or a peer's -- either way it is a radio standing in the world and it is heard from where it
        // stands. Ours was silent while the assumption held that the app itself is audible to us; once that
        // app is sent to an output nobody listens to (the only way to not hear the music twice), silence was
        // all the streamer got. Turn your own radio off locally if you would rather hear the app direct.
        void* wave = (r.player < 0) ? (g_orad.OwnWave ? g_orad.OwnWave(1) : nullptr)
                                    : (g_orad.PeerWave ? g_orad.PeerWave(r.player, 1) : nullptr);
        if (!wave) return;
        void* ac = SpawnOn(r, wave, 0.0f);
        char who[64];
        TwkLog("[radio] %s: playing their stream%s", Whose(r, who, sizeof(who)), ac ? "" : " -- NOT PLAYED (the engine refused)");
        return;
    }
    void* st = Station(r.station);
    const int n = SongCount(st);
    if (n > 0) r.song = ((r.song % n) + n) % n;
    void* wave = SongWave(st, r.song);
    if (!wave) { TwkLog("[radio] station %d song %d has no sound to play (station %p)", r.station, r.song, st); r.songLen = 0.0f; return; }
    const float len = twkF(wave, SND_DURATION);
    r.songLen = (len > 5.0f && len < 3600.0f) ? len : 180.0f;
    if (r.songPos >= r.songLen - 0.5f) return;                          // over: the owner's next word (or our own clock) moves it on
    void* ac = SpawnOn(r, wave, r.songPos);
    char who[64];
    TwkLog("[radio] %s: %s, song %d of %d, from %.0f s of %.0f%s", Whose(r, who, sizeof(who)), kStations[r.station].name, r.song + 1, n, r.songPos, r.songLen,
           ac ? "" : " -- NOT PLAYED (the engine refused)");
}
// A sound on a radio: its falloff, its channel (see the header), its level, from `start` seconds in.
void* SpawnOn(Radio& r, void* wave, float start) {
    // THE FALLOFF SHAPE IS THIS ASSET'S, not ours -- ours only ends the tail (see the pump). The default
    // is the game's own radio attenuation, and note what it is FOR: SAT_Sfx_Apt_RADIO, the APARTMENT one,
    // authored for a sound source in a room. Outdoors across a skatepark it runs out of road long before
    // you would expect a speaker to ("it just gets silent closer than it should" -- field, with a peer's
    // radio inaudible across a car park at full volume). RadioAttenuation names a different one to try;
    // "none" drops it entirely and leaves the distance wholly to us -- louder and further, but the engine
    // then has nothing to place it with, so it stops being properly 3D. Try another asset first.
    void* atten = nullptr;
    if (_stricmp(g_attenPath, "none") != 0) {
        atten = LoadByPath(g_attenPath);                                // asked for each time: nothing of ours keeps it in memory between songs
        if (!atten && strcmp(g_attenPath, kAttenPath) != 0) atten = LoadByPath(kAttenPath);
        if (!atten && !g_attenMissingSaid) { g_attenMissingSaid = true; TwkLog("[radio] %s could not be loaded -- the volume is set by distance here instead", g_attenPath); }
    } else if (!g_attenMissingSaid) { g_attenMissingSaid = true; TwkLog("[radio] RadioAttenuation=none -- distance is set by hand, and the sound is not placed in 3D by the engine"); }
    // THE CHANNEL: not the song's own class (Music) but the one the ini names, so the Music slider is not the radio's
    void* cls = nullptr;
    if (g_soundClass[0] && g_audioPlay) {
        cls = CatchSound_FindObject(g_soundClass, "SoundClass");
        if (!cls) { char path[160]; snprintf(path, sizeof(path), "/Game/Audio/SoundClasses/%s.%s", g_soundClass, g_soundClass); cls = LoadByPath(path); }
        if (!g_classSaid) { g_classSaid = true; TwkLog(cls ? "[radio] played under %s: that slider is its volume, not Music" : "[radio] sound class %s not found -- it stays on the Music slider", g_soundClass); }
    }
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    void* ac = nullptr;
    __try {
        // autoDestroy FALSE -- WE own this component's life, not the engine. With it true the engine
        // destroyed the component the instant the sound stopped for any reason, including being culled
        // for distance, and the 2 s retry below then spawned a NEW one: field-logged as StartSound
        // firing every two seconds for minutes ("from 162 s", "164 s", "166 s" ...), heard as music
        // that restarts instead of playing, and as a radio that never gets going when you join or walk
        // back into range. StopSound stops it explicitly, so nothing was gained by letting it go.
        ac = g_spawnSound(wave, r.comp, 0, zero, zero, 0 /* keep relative */, true, cls ? 0.001f : VolumeOf(r), 1.0f, start, atten, nullptr, false);
        if (ac && cls) {                                                // the class is read when a sound STARTS: so it is started again
            *(void**)((uint8_t*)ac + AC_CLASS_OVR) = cls;
            *(float*)((uint8_t*)ac + AC_VOLUME) = VolumeOf(r);
            g_audioPlay(ac, start);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ac = nullptr; }
    if (ac) { SitUI_Track(&r.sref, ac); if (!r.sref.obj) ac = nullptr; }
    r.sound = ac;
    return ac;
}

// ---- the prop
bool MakeActor(Radio& r, void* anchorActor, const float at[3]) {
    if (!Look()) { snprintf(g_whyNot, sizeof(g_whyNot), "%s", g_lookWhy); return false; }
    if (!g_meshPath[0]) snprintf(g_meshPath, sizeof(g_meshPath), "%s", kMeshPath);
    void* cls = CatchSound_FindObject("StaticMeshActor", "Class");
    void* mesh = LoadByPath(g_meshPath);
    if (!mesh && strcmp(g_meshPath, kMeshPath) != 0) mesh = LoadByPath(kMeshPath);
    if (!cls || !mesh) {
        TwkLog("[radio] cannot be made: actor class %p, mesh %p (%s)", cls, mesh, g_meshPath);
        snprintf(g_whyNot, sizeof(g_whyNot), "The radio's model could not be loaded");
        return false;
    }
    const float rot[3] = { 0.0f, 0.0f, 0.0f };
    void* actor = Radial_SpawnActor(anchorActor, cls, at, rot);
    void* comp = actor ? twkP(actor, SMA_COMP) : nullptr;
    if (!actor || !comp) { TwkLog("[radio] the engine would not spawn it (actor %p, component %p)", actor, comp); snprintf(g_whyNot, sizeof(g_whyNot), "The radio could not be made"); return false; }
    SitUI_Track(&r.aref, actor); SitUI_Track(&r.cref, comp);
    if (!r.aref.obj || !r.cref.obj) { Radial_DestroyActor(actor); snprintf(g_whyNot, sizeof(g_whyNot), "The radio could not be made"); return false; }
    Radial_SetMovable(comp);                                   // BEFORE the mesh: a static component refuses a new one
    bool set = false;
    __try { set = g_setMesh(comp, mesh); if (g_collision) g_collision(actor, false); } __except (EXCEPTION_EXECUTE_HANDLER) { set = false; }
    if (!g_boundsRead) {
        g_boundsRead = true;
        for (int i = 0; i < 3; i++) { g_boundsO[i] = twkF(mesh, STM_BOUNDS + i * 4); g_boundsE[i] = twkF(mesh, STM_BOUNDS + 12 + i * 4); }
        for (int i = 0; i < 3; i++) if (!(g_boundsE[i] > 0.5f && g_boundsE[i] < 300.0f) || !(fabsf(g_boundsO[i]) < 500.0f)) { g_boundsO[0] = g_boundsO[1] = g_boundsO[2] = 0.0f; g_boundsE[0] = 30.0f; g_boundsE[1] = 14.0f; g_boundsE[2] = 7.0f; break; }
        TwkLog("[radio] the model: %.0f x %.0f x %.0f cm (centre %.0f %.0f %.0f off its origin)", g_boundsE[0] * 2.0f, g_boundsE[1] * 2.0f, g_boundsE[2] * 2.0f, g_boundsO[0], g_boundsO[1], g_boundsO[2]);
    }
    r.actor = actor; r.comp = comp; r.attached = false; r.hungFrom = nullptr;
    char who[64];
    TwkLog("[radio] made (%s): mesh %s, collision %s", Whose(r, who, sizeof(who)), set ? "set" : "NOT SET", g_collision ? "off" : "LEFT ON (no call)");
    return true;
}
void Unmake(Radio& r) {
    StopSound(r);
    if (Alive(r)) Radial_DestroyActor(r.actor);
    r.actor = nullptr; r.comp = nullptr; r.attached = false; r.hungFrom = nullptr;
}
void Detach(Radio& r) { if (r.attached && Alive(r)) Sit_DetachKeepWorld(r.comp); r.attached = false; r.hungFrom = nullptr; }

// ---- where mine goes
bool SkaterFooting(void* sk, float feet[3], float* yawRad) {
    float q[4], p[3];
    if (!ReadC2W(twkP(sk, ACT_ROOT), q, p)) return false;
    *yawRad = atan2f(2.0f * (q[3] * q[2] + q[0] * q[1]), 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]));
    feet[0] = p[0]; feet[1] = p[1]; feet[2] = p[2] - 90.0f;
    return true;
}
float DistanceTo(const Radio& r, void* sk) {
    float q[4], p[3], rq[4], rp[3];
    if (!Alive(r) || !ReadC2W(twkP(sk, ACT_ROOT), q, p) || !ReadC2W(r.comp, rq, rp)) return 1e9f;
    return sqrtf((p[0] - rp[0]) * (p[0] - rp[0]) + (p[1] - rp[1]) * (p[1] - rp[1]) + (p[2] - rp[2]) * (p[2] - rp[2]));
}
// Set down in front of the skater, on whatever is there, on the bottom of its bounds, its long side across your view.
void SetDown(void* sk, float ahead) {
    Radio& r = g_mine;
    float feet[3], yaw;
    if (!Alive(r) || !SkaterFooting(sk, feet, &yaw)) return;
    Detach(r);
    const float spot[3] = { feet[0] + cosf(yaw) * ahead, feet[1] + sinf(yaw) * ahead, feet[2] };
    const float a[3] = { spot[0], spot[1], spot[2] + 70.0f }, b[3] = { spot[0], spot[1], spot[2] - 250.0f };
    float hit[3] = { spot[0], spot[1], spot[2] }; int surface = 0;
    if (!Sit_TraceSurface(sk, a, b, hit, &surface)) { hit[0] = spot[0]; hit[1] = spot[1]; hit[2] = spot[2]; }
    int iL = 0; for (int i = 1; i < 3; i++) if (g_boundsE[i] > g_boundsE[iL]) iL = i;
    const float ry = yaw + (iL == 0 ? 1.5707963f : 0.0f);
    const float c = cosf(ry), s = sinf(ry);
    // the bounds' bottom centre, in the mesh's own space, turned by the yaw: that point goes on the ground
    const float bx = g_boundsO[0], by = g_boundsO[1], bz = g_boundsO[2] - g_boundsE[2];
    const float loc[3] = { hit[0] - (c * bx - s * by), hit[1] - (s * bx + c * by), hit[2] - bz + 0.5f };
    const float quat[4] = { 0.0f, 0.0f, sinf(ry * 0.5f), cosf(ry * 0.5f) };
    Radial_PlaceComp(r.comp, loc, quat);
    r.state = R_PLACED; g_dirty = true;
    TwkLog("[radio] set down %.0f cm ahead, on %s", ahead, surface ? "the ground found there" : "the level of your feet (nothing found under it)");
}
bool Hold(void* sk) {
    (void)sk;
    if (!Emote_CarryStart(g_boundsO, g_boundsE)) { snprintf(g_whyNot, sizeof(g_whyNot), "%s", Sit_PoseHeld() ? "Stand up first" : "Not right now"); return false; }
    Detach(g_mine);
    g_mine.state = R_HELD; g_dirty = true;
    return true;
}
void StopStreaming(const char* why) {
    Radio& r = g_mine;
    if (r.source != radiowire::SRC_STREAM) return;
    if (g_orad.StreamStop) g_orad.StreamStop();
    r.source = radiowire::SRC_STATION; r.srcName[0] = 0; r.srcPid = 0; g_dirty = true;
    TwkLog("[radio] streaming stopped (%s)", why);
}
void Away() {
    StopStreaming("put away");
    if (g_mine.state == R_HELD) Emote_CarryStop();
    Unmake(g_mine);
    g_mine.state = R_NONE; g_dirty = true;
    TwkLog("[radio] put away");
}
void NextSong(int step) {
    Radio& r = g_mine;
    const int n = SongCount(Station(r.station));
    r.song = n > 0 ? ((r.song + step) % n + n) % n : 0;
    r.songPos = 0.0f; g_dirty = true;
    StartSound(r);
}
void NextStation() {
    Radio& r = g_mine;
    r.station = (r.station + 1) % 3;
    const int n = SongCount(Station(r.station));
    g_rng = g_rng * 1664525u + 1013904223u;
    r.song = n > 0 ? (int)((g_rng >> 8) % (unsigned)n) : 0;               // a station is joined somewhere in its list, as a radio is
    r.songPos = 0.0f; g_dirty = true;
    StartSound(r);
}

// ------------------------------------------------------------------ co-op
void FillState(radiowire::State* s) {
    const Radio& r = g_mine;
    memset(s, 0, sizeof(*s));
    s->state = (uint8_t)(Alive(r) ? r.state : R_NONE);
    s->station = (uint8_t)r.station; s->song = (uint16_t)r.song; s->songPos = r.songPos;
    s->source = (uint8_t)r.source;
    s->quat[3] = 1.0f;
    if (s->state == R_PLACED) { float q[4], p[3]; if (ReadC2W(r.comp, q, p)) { for (int i = 0; i < 3; i++) s->pos[i] = p[i]; for (int i = 0; i < 4; i++) s->quat[i] = q[i]; } else s->state = R_NONE; }
    if (s->state == R_HELD && r.attached) {
        s->relValid = 1;
        for (int i = 0; i < 3; i++) { s->relLoc[i] = twkF(r.comp, SC_REL_LOC + i * 4); s->relRot[i] = twkF(r.comp, SC_REL_ROT + i * 4); }
    }
}
void SendState(int to, bool reliable) {
    if (!g_channel || !g_omp.version || !g_omp.InSession()) return;
    radiowire::State s; FillState(&s);
    uint8_t buf[radiowire::kSize];
    const int n = radiowire::Encode(s, buf, sizeof(buf));
    if (n > 0 && g_omp.Send(g_channel, to, buf, n, reliable ? 1 : 0)) { if (g_sent++ < 4) TwkLog("[radio] sent: state %d, station %d song %d at %.0f s%s", s.state, s.station, s.song, s.songPos, reliable ? "" : " (heartbeat)"); }
}
void SendWant(int to, bool want) {
    if (!g_channel || !g_omp.version) return;
    uint8_t b[radiowire::kWantSize];
    const int n = radiowire::EncodeWant(want, b, sizeof(b));
    if (n > 0) g_omp.Send(g_channel, to, b, n, 1);
}
Radio* RemoteOf(int player, bool make) {
    Radio* spare = nullptr;
    for (Radio& r : g_remote) { if (r.used && r.player == player) return &r; if (!r.used && !spare) spare = &r; }
    if (!make || !spare) return nullptr;
    memset(spare, 0, sizeof(*spare));
    spare->used = true; spare->player = player;
    return spare;
}
void OnMessage(int player, const uint8_t* data, int len, void*) {
    bool want = false;
    if (radiowire::DecodeWant(data, len, &want)) {                      // a listener asking for (or done with) OUR stream
        if (g_orad.SetListener) g_orad.SetListener(player, want ? 1 : 0);
        return;
    }
    radiowire::State s;
    if (!radiowire::Decode(data, len, &s)) { if (g_bad++ < 4) TwkLog("[radio] a message from player %d was not believed (%d bytes)", player, len); return; }
    Radio* r = RemoteOf(player, true);
    if (!r) return;
    const bool sameSong = r->have && r->last.station == s.station && r->last.song == s.song;
    const bool sameSource = r->have && r->last.source == s.source;
    r->last = s; r->have = true; r->applied = false; r->sinceMsg = 0.0f;
    if (!sameSource) { r->source = s.source; StopSound(*r); }           // stations <-> a stream: what is heard changes
    // the song: a new one, or the same one further off than a heartbeat's jitter explains
    if (s.source == radiowire::SRC_STATION && (!sameSong || fabsf(r->songPos - s.songPos) > 1.5f)) { r->station = s.station; r->song = s.song; r->songPos = s.songPos; StopSound(*r); }
    if (g_got++ < 6) TwkLog("[radio] player %d: state %d, station %d song %d at %.0f s", player, s.state, s.station, s.song, s.songPos);
}
void OnPlayer(int player, int joined, void*) {
    if (joined) { SendState(player, true); return; }                    // a newcomer is told what mine is doing
    if (g_orad.SetListener) g_orad.SetListener(player, 0);             // one who left listens to nothing of ours
    Radio* r = RemoteOf(player, false);                                 // ...and takes their radio with them
    if (r) { Unmake(*r); r->used = false; TwkLog("[radio] player %d left: their radio with them", player); }
}
void Bind() {
    if (g_channel) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_bindTryMs) return;
    g_bindTryMs = now + g_bindWaitMs;
    if (g_bindWaitMs < 64000) g_bindWaitMs *= 2;                        // no OpenMP in this game: asked less and less often
    if (!g_omp.version && !OmpMod_Bind(&g_omp)) return;                 // no OpenMP in this game: a radio of one
    BindRadioApi();
    g_channel = g_omp.Register("sessiontweaks.radio", OnMessage, OnPlayer, nullptr);
    TwkLog(g_channel ? "[radio] co-op: on OpenMP's mod channel (api %d)" : "[radio] co-op: OpenMP would not register the channel (api %d)", g_omp.version);
}
// Build another player's radio in this world, as their last word has it.
void PumpRemote(Radio& r, float dt) {
    r.sinceMsg += dt;
    void* owner = (g_omp.version && g_omp.PlayerActor) ? g_omp.PlayerActor(r.player) : nullptr;
    if (!r.have || r.last.state == radiowire::ST_NONE || !owner) {        // put away, or its owner is not in this world (another map)
        if (r.actor) { Unmake(r); r.state = R_NONE; }
        if (r.wantSent) { SendWant(r.player, false); r.wantSent = false; }
        return;
    }
    r.source = r.last.source;
    if (!Alive(r)) {
        r.actor = nullptr; r.comp = nullptr; r.sound = nullptr;
        float q[4], p[3];
        if (!ReadC2W(twkP(owner, ACT_ROOT), q, p)) return;
        if (!MakeActor(r, owner, p)) { r.have = false; return; }        // not to be had here: do not try every frame
        r.applied = false;
    }
    if (r.last.state == radiowire::ST_PLACED) {
        if (!r.applied || r.attached) { Detach(r); Radial_PlaceComp(r.comp, r.last.pos, r.last.quat); r.applied = true; r.state = R_PLACED; }
    } else {                                                            // HELD: hung from their chest bone, where it hangs from the owner's
        void* mesh = twkP(owner, CH_MESH);
        if (mesh && r.last.relValid && g_setRel && (!r.attached || r.hungFrom != mesh || !r.applied)) {
            const unsigned long long bone = Emote_ChestBoneOf(mesh);
            if (bone) {
                if (r.attached && r.hungFrom != mesh) Detach(r);
                if (!r.attached) r.attached = Sit_AttachKeepWorld(r.comp, mesh, bone);
                if (r.attached) {
                    r.hungFrom = mesh;
                    __try { g_setRel(r.comp, r.last.relLoc, r.last.relRot, false, nullptr, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                    r.applied = true; r.state = R_HELD;
                }
            }
        } else if (mesh && !r.last.relValid && !r.attached && !r.applied) {   // on its way up to the arm: beside them, until the owner says where
            float q[4], p[3];
            if (ReadC2W(twkP(owner, ACT_ROOT), q, p)) { const float at[3] = { p[0], p[1], p[2] - 30.0f }; Radial_PlaceComp(r.comp, at, q); }
        }
    }
    // THEIR STREAM: asked for while we play it (renewed every 3 s; OpenMP forgets an ask after 8), and let go of
    // when we do not -- muted, or it went back to the stations. It plays from their radio like a song would.
    if (r.source == radiowire::SRC_STREAM) {
        const bool want = !r.muted && Alive(r) && g_orad.PeerWave;
        r.wantBeat += dt;
        if (want && (!r.wantSent || r.wantBeat > 3.0f)) { SendWant(r.player, true); r.wantSent = true; r.wantBeat = 0.0f; }
        else if (!want && r.wantSent) { SendWant(r.player, false); r.wantSent = false; }
        if (!r.muted && (!r.sound || !SitUI_Alive(&r.sref)) && GetTickCount64() >= r.nextTryMs) StartSound(r);
        return;
    }
    if (r.wantSent) { SendWant(r.player, false); r.wantSent = false; }
    // their song, on OUR clock between their words
    r.songPos += dt;
    if (r.songLen > 0.0f && r.songPos >= r.songLen - 0.25f) { StopSound(r); return; }   // over: their next word starts the next
    // A restart here should now be RARE -- the component outlives going out of earshot. If this ever
    // churns again the log says so at once, instead of it being heard as music that will not settle.
    if (!r.muted && (!r.sound || !SitUI_Alive(&r.sref)) && GetTickCount64() >= r.nextTryMs) {
        if (r.sound) { char who[64]; TwkLog("[radio] %s: their sound went away on its own -- starting it again from %.0f s", Whose(r, who, sizeof(who)), r.songPos); }
        StartSound(r);
    }
}

// ------------------------------------------------------------------ the wheel
Radio* NearestRadio(void* sk, float within) {
    Radio* best = nullptr; float bd = within;
    if (g_mine.state == R_HELD && Alive(g_mine)) return &g_mine;
    if (Alive(g_mine)) { const float d = DistanceTo(g_mine, sk); if (d < bd) { bd = d; best = &g_mine; } }
    for (Radio& r : g_remote) if (r.used && Alive(r)) { const float d = DistanceTo(r, sk); if (d < bd) { bd = d; best = &r; } }
    return best;
}
void BuildWheel(void* sk) {
    g_nActs = 0; g_muteTarget = nullptr;
    auto add = [](int act, const char* label) { if (g_nActs < 8) { g_acts[g_nActs] = act; snprintf(g_label[g_nActs], sizeof(g_label[0]), "%s", label); g_nActs++; } };
    const bool mine = g_mine.state != R_NONE && Alive(g_mine);
    // "Take out radio", not "Radio": the page above already says which prop this is, so the entry has
    // to say what it DOES. It read as a bare label repeating the title.
    if (!mine) add(A_TAKE, "Take out radio");
    else {
        const bool near_ = g_mine.state == R_HELD || (sk && DistanceTo(g_mine, sk) < 300.0f);
        if (g_mine.state == R_HELD) add(A_PUTDOWN, "Put radio down");
        else add(A_PICKUP, near_ ? "Pick radio up" : "Bring radio here");
        const bool streaming = g_mine.source == radiowire::SRC_STREAM;
        if (!streaming) add(A_NEXTSONG, "Next song");
        char st[64];
        if (streaming) snprintf(st, sizeof(st), "Back to %s", kStations[g_mine.station].name);
        else snprintf(st, sizeof(st), "Station: %s", kStations[(g_mine.station + 1) % 3].name);
        add(A_NEXTSTATION, st);
        // STREAM ONE APP FROM THIS PC: the apps with sound open, one at a time -- each press moves to the next
        if (g_orad.Sources) {
            char names[kMaxSrc * 48];
            g_srcN = g_orad.Sources(g_srcPid, names, 48, kMaxSrc);
            for (int k = 0; k < g_srcN; k++) snprintf(g_srcName[k], sizeof(g_srcName[k]), "%s", names + k * 48);
            g_streamNext = 0;
            if (streaming) for (int k = 0; k < g_srcN; k++) if (!_stricmp(g_srcName[k], g_mine.srcName)) { g_streamNext = (k + 1) % g_srcN; break; }
            char sl[64];
            if (g_streamErr[0] && GetTickCount64() < g_streamErrUntil) snprintf(sl, sizeof(sl), "Stream: %.50s", g_streamErr);
            else if (!g_srcN) snprintf(sl, sizeof(sl), "Stream: play something on your PC");
            else if (streaming && g_srcN == 1 && !_stricmp(g_srcName[0], g_mine.srcName)) snprintf(sl, sizeof(sl), "Streaming %.40s", g_mine.srcName);
            else if (streaming) snprintf(sl, sizeof(sl), "Streaming %.20s (next: %.20s)", g_mine.srcName, g_srcName[g_streamNext]);
            else snprintf(sl, sizeof(sl), "Stream: %.44s", g_srcName[g_streamNext]);
            add(A_STREAM, sl);
        }
    }
    // THE RADIO YOU ARE STANDING AT -- yours or anyone's -- can be turned off FOR YOU
    g_muteTarget = sk ? NearestRadio(sk, 300.0f) : nullptr;
    if (g_muteTarget) {
        char vl[64];
        const float vnow = VolumeOf(*g_muteTarget);
        const int pct = (int)(vnow * 100.0f + 0.5f);
        // ...and how far it now carries, because that is what the volume changes (see the pump). Metres,
        // so the number means something to stand next to.
        float vc = vnow; if (vc < 0.0f) vc = 0.0f; if (vc > 1.0f) vc = 1.0f;
        const int reach = (int)((g_rangeMin + (g_rangeMax - g_rangeMin) * vc) / 100.0f + 0.5f);
        snprintf(vl, sizeof(vl), "Volume up (%d%%, %d m)", pct, reach);   add(A_VOLUP, vl);
        snprintf(vl, sizeof(vl), "Volume down (%d%%, %d m)", pct, reach); add(A_VOLDOWN, vl);
        char who[64], lb[64];
        if (g_muteTarget->player < 0) snprintf(lb, sizeof(lb), "Turn radio %s (for me)", g_muteTarget->muted ? "on" : "off");
        else snprintf(lb, sizeof(lb), "Turn %.30s radio %s (for me)", Whose(*g_muteTarget, who, sizeof(who)), g_muteTarget->muted ? "on" : "off");
        add(A_MUTE, lb);
    }
    if (mine) add(A_AWAY, "Put radio away");
}

} // namespace

// Any asset by its full path (the clap's sound and falloff, 539): found if loaded, loaded if not.
void* Radio_LoadAsset(const char* path) { Look(); return path && path[0] ? LoadByPath(path) : nullptr; }

// ------------------------------------------------------------------ the module's face
void Radio_ReadConfig(const char* buf) {
    g_on = TwkIniIntQuiet(buf, "RadioEnabled", 1) ? 1 : 0;
    g_rangeMax = (float)TwkIniIntQuiet(buf, "RadioRangeCm", 12000);
    g_closeCm  = (float)TwkIniIntQuiet(buf, "RadioCloseCm", 150);
    g_rangeMin = (float)TwkIniIntQuiet(buf, "RadioRangeMinCm", 800);
    TwkIniStr(buf, "RadioAttenuation", g_attenPath, sizeof(g_attenPath), kAttenPath);
    g_rangeDebug = TwkIniIntQuiet(buf, "RadioRangeDebug", 0) != 0;
    if (!g_attenPath[0]) snprintf(g_attenPath, sizeof(g_attenPath), "%s", kAttenPath);
    g_falloffStart = (float)TwkIniIntQuiet(buf, "RadioFalloffStartPct", 45) / 100.0f;
    if (g_falloffStart < 0.0f) g_falloffStart = 0.0f;
    if (g_falloffStart > 0.9f) g_falloffStart = 0.9f;
    if (g_rangeMax < 100.0f) g_rangeMax = 100.0f;
    if (g_rangeMin < 50.0f)  g_rangeMin = 50.0f;
    if (g_rangeMin > g_rangeMax) g_rangeMin = g_rangeMax;
    const int vo = TwkIniIntQuiet(buf, "RadioVolumeOthersPct", 10);
    g_volumeOthers = (float)(vo < 0 ? 0 : vo > 200 ? 200 : vo) / 100.0f;
    const int v = TwkIniIntQuiet(buf, "RadioVolumePct", 40);
    g_volume = (float)(v < 0 ? 0 : v > 400 ? 400 : v) / 100.0f;
    TwkIniStr(buf, "RadioMesh", g_meshPath, sizeof(g_meshPath), kMeshPath);
    TwkIniStr(buf, "RadioSoundClass", g_soundClass, sizeof(g_soundClass), "SNDCLASS_Sfx");
    const int s = TwkIniIntQuiet(buf, "RadioStation", 1);
    g_mine.station = ((s < 1 ? 1 : s > 3 ? 3 : s) - 1);
    g_mine.player = -1;
}
void Radio_SaveConfig(char* buf, size_t cap) { TwkIniSetInt(buf, cap, "RadioVolumePct", (int)(g_volume * 100.0f + 0.5f)); }
bool Radio_Holding() { return g_mine.state == R_HELD; }
// STANDING AT ONE -- near enough that a click of the stick plainly means "this speaker" rather than
// "open the wheel". Deliberately tighter than the 3 m the wheel's own mute entry uses: that only has
// to decide WHICH radio you meant once the wheel is already open, where being wrong costs a glance.
// This CHANGES WHAT THE BUTTON DOES, so it wants you unmistakably at the thing.
// A radio under your arm does not count -- the root wheel is what you want then, not the speaker page.
bool Radio_AtSpeaker() {
    if (!g_on || g_mine.state == R_HELD) return false;
    void* sk = CatchTweaks_Skater();
    return sk && NearestRadio(sk, g_closeCm) != nullptr;
}
void Radio_RequestPutDown() { InterlockedExchange(&g_reqPutDown, 1); }
const char* Radio_WhyNot() { return g_whyNot; }
// THE PROPS THEMSELVES, one entry each -- the page above the options. One for now; when there is a
// second prop it gets a line here and a page of its own for free.
int Radio_PropCount() { return g_on ? 1 : 0; }
const char* Radio_PropLabel(int i) { return i == 0 ? "Radio" : ""; }
int Radio_WheelCount() { BuildWheel(CatchTweaks_Skater()); return g_nActs; }
const char* Radio_WheelLabel(int i) { return (i >= 0 && i < g_nActs) ? g_label[i] : ""; }
// false = refused (Radio_WhyNot says why). `keepOpen` comes back true for the ones you may want again at once
// (next song, next station): the wheel stays up for those.
bool Radio_WheelTake(int i, bool* keepOpen) {
    if (keepOpen) *keepOpen = false;
    snprintf(g_whyNot, sizeof(g_whyNot), "Not right now");
    void* sk = CatchTweaks_Skater();
    if (!g_on || !sk || i < 0 || i >= g_nActs) return false;
    Radio& r = g_mine;
    switch (g_acts[i]) {
    case A_TAKE: {
        float feet[3], yaw;
        if (!SkaterFooting(sk, feet, &yaw)) return false;
        const float at[3] = { feet[0], feet[1], feet[2] + 90.0f };
        r.player = -1;
        if (!MakeActor(r, sk, at)) return false;
        if (!Hold(sk)) { Unmake(r); r.state = R_NONE; return false; }
        r.songPos = 0.0f; r.muted = false;
        { const int n = SongCount(Station(r.station)); g_rng ^= (unsigned)GetTickCount64(); g_rng = g_rng * 1664525u + 1013904223u; r.song = n > 0 ? (int)((g_rng >> 8) % (unsigned)n) : 0; }
        StartSound(r);
        return true;
    }
    case A_PUTDOWN:     Radio_RequestPutDown(); return true;
    case A_PICKUP:      return Alive(r) && Hold(sk);
    case A_NEXTSONG:    NextSong(1); if (keepOpen) *keepOpen = true; return true;
    case A_NEXTSTATION:
        if (r.source == radiowire::SRC_STREAM) { StopStreaming("back to the stations"); StartSound(r); }
        else NextStation();
        if (keepOpen) *keepOpen = true;
        return true;
    case A_STREAM: {
        if (!g_orad.StreamStart) return false;
        if (!g_srcN) { snprintf(g_whyNot, sizeof(g_whyNot), "Play something on your PC first"); return false; }
        const int k = g_streamNext < g_srcN ? g_streamNext : 0;
        if (r.source == radiowire::SRC_STREAM && !_stricmp(g_srcName[k], r.srcName)) { if (keepOpen) *keepOpen = true; return true; }
        StopSound(r);
        g_orad.StreamStart(g_srcPid[k], g_srcName[k]);
        r.source = radiowire::SRC_STREAM; r.srcPid = g_srcPid[k];
        snprintf(r.srcName, sizeof(r.srcName), "%s", g_srcName[k]);
        g_streamErr[0] = 0; g_dirty = true;
        r.nextTryMs = 0;                 // ...and our own radio plays it back: StartSound, once there is audio to play
        TwkLog("[radio] streaming %s from this PC (pid %u)", r.srcName, r.srcPid);
        // The app goes on playing out of YOUR speakers as well as out of the radio, so the streamer
        // hears their music twice. Muting the app does NOT help -- process loopback taps it AFTER the
        // app's volume, so muting it captures silence (measured, omp_radiosolo). Sending the app to an
        // output device you are not listening to DOES work: the capture is unaffected (measured, 100%
        // of the level), and then the music only exists inside the game. Said once per session.
        {
            static bool saidOnce = false;
            if (!saidOnce) {
                saidOnce = true;
                TwkLog("[radio] note: %s is still playing out of your own speakers too. To hear it ONLY "
                       "from the radio, send that app to an output you do not listen to: Windows Settings > "
                       "System > Sound > Volume mixer > %s > Output device. Muting it will NOT work -- it "
                       "would mute the stream as well.", r.srcName, r.srcName);
            }
        }
        if (keepOpen) *keepOpen = true;
        return true;
    }
    case A_MUTE: {
        Radio* t = g_muteTarget;
        if (!t || !Alive(*t)) return false;
        t->muted = !t->muted;
        if (t->muted) StopSound(*t); else StartSound(*t);
        char who[64];
        TwkLog("[radio] %s: %s for this game only", Whose(*t, who, sizeof(who)), t->muted ? "turned off" : "turned on");
        return true;
    }
    case A_VOLUP:
    case A_VOLDOWN: {
        // THE RADIO YOU HOLD OR STAND AT, in THIS game only. Fine steps at the quiet end, where a step is heard most.
        Radio* t = g_muteTarget;
        if (!t || !Alive(*t)) return false;
        const int now = (int)(VolumeOf(*t) * 100.0f + 0.5f);
        const int step = now < 20 || (now == 20 && g_acts[i] == A_VOLDOWN) ? 5 : 10;
        int pct = now + (g_acts[i] == A_VOLUP ? step : -step);
        pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
        t->volume = (float)pct / 100.0f; t->volumeSet = true;
        // Your own radio's level is the one KEPT for next time -- but only as the level a radio STARTS at.
        // It used to be the live default that every radio you had not individually touched read through
        // (VolumeOf), so turning yours up turned up everyone else's with it. Radios already out keep what
        // they were playing at; the new default reaches only radios that appear later.
        if (t->player < 0) {
            if (!g_mine.volumeSet) { g_mine.volume = g_volume; g_mine.volumeSet = true; }
            g_volume = t->volume; TwkMarkDirty();   // theirs read g_volumeOthers, so this cannot reach them
        }
        bool applied = false;
        if (g_audioVolume && t->sound && SitUI_Alive(&t->sref) && !g_attenMissingSaid) {
            __try { g_audioVolume(t->sound, t->volume); applied = true; } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        if (!applied && !g_attenMissingSaid && !t->muted) StartSound(*t);     // no call for it in this build: started again at the new level
        char who[64];
        TwkLog("[radio] %s: volume %d%% in this game", Whose(*t, who, sizeof(who)), pct);
        if (keepOpen) *keepOpen = true;
        return true;
    }
    case A_AWAY:        Away(); return true;
    default: return false;
    }
}
void Radio_PumpFrame() {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (g_qpf == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart; }
    float dt = g_pumpQpc ? (float)((double)(q.QuadPart - g_pumpQpc) / g_qpf) : 0.0f;
    g_pumpQpc = q.QuadPart;
    if (dt > 0.1f) dt = 0.1f;
    if (!g_on) return;
    Bind();
    void* sk = CatchTweaks_Skater();
    Radio& r = g_mine;
    r.player = -1;

    // ---- mine
    if (r.state != R_NONE) {
        if (!Alive(r)) { TwkLog("[radio] gone: the level went, and it with it"); StopStreaming("the radio is gone"); r.actor = nullptr; r.comp = nullptr; r.sound = nullptr; r.attached = false; r.state = R_NONE; g_dirty = true; }
        else if (!sk) { if (r.state == R_HELD) Emote_CarryStop(); }
        else {
            const bool putDown = InterlockedExchange(&g_reqPutDown, 0) != 0;
            if (r.state == R_HELD) {
                if (putDown) { Emote_CarryStop(); SetDown(sk, 75.0f); }
                else if (!Emote_Carrying()) SetDown(sk, 55.0f);         // the carry ended by itself: got on the board, sat down, an editor
                else if (!r.attached) {
                    // until the pose is fully up the radio is PUT where the pose wants it each frame; then it is hung
                    // from the chest bone and rides the body itself
                    float pos[3], quat[4], w = 0.0f; unsigned long long bone = 0;
                    if (Emote_CarryPlace(pos, quat, &w, &bone)) {
                        Radial_PlaceComp(r.comp, pos, quat);
                        void* mesh = twkP(sk, CH_MESH);
                        if (w >= 0.985f && mesh && bone) {
                            r.attached = Sit_AttachKeepWorld(r.comp, mesh, bone);
                            if (r.attached) { r.hungFrom = mesh; g_dirty = true; TwkLog("[radio] under the arm: hung from the chest bone"); }   // now there is a place to tell the others
                        }
                    }
                }
            }
            // STREAMING: our own radio plays our own stream, so it needs starting like any other -- but
            // only once the capture has produced something, or the wave starts on an empty ring and the
            // engine drops it. StartSound sets nextTryMs, so a refusal is retried rather than given up on.
            if (r.source == radiowire::SRC_STREAM) {
                if (!r.sound && !r.muted && Alive(r) && GetTickCount64() >= r.nextTryMs &&
                    g_orad.OwnWave && g_orad.StreamState && g_orad.StreamState(nullptr, 0) == 2) StartSound(r);
                char why[160] = "";
                if (g_orad.StreamState && g_orad.StreamState(why, sizeof(why)) == -1) {
                    snprintf(g_streamErr, sizeof(g_streamErr), "%s", why[0] ? why : "it stopped");
                    g_streamErrUntil = GetTickCount64() + 8000;
                    StopStreaming(g_streamErr);
                    StartSound(r);
                }
            }
            // what it is playing moves on by THIS clock (it stops with the game), heard or not
            else if (r.songLen > 0.0f) {
                r.songPos += dt;
                if (r.songPos >= r.songLen - 0.25f) NextSong(1);
                else if (!r.muted && (!r.sound || !SitUI_Alive(&r.sref)) && GetTickCount64() >= r.nextTryMs) StartSound(r);   // it stopped by itself (a level's audio reset): rejoin
            } else if (!r.muted && !r.sound && GetTickCount64() >= r.nextTryMs) StartSound(r);
        }
    }
    // ---- everyone else's
    for (Radio& o : g_remote) if (o.used) PumpRemote(o, dt);
    // ---- HOW FAR IT CARRIES FOLLOWS HOW LOUD IT IS SET.
    // Turning a radio down used to make it quieter everywhere and no smaller: the attenuation asset's
    // radius is fixed, so a radio at 10% was still "present" across the whole park, just faintly. A real
    // one turned down is simply a thing you have to be near. So the range is scaled by the volume --
    // RadioRangeCm at full, RadioRangeMinCm at nothing -- and the gain rolls off to silence there,
    // SQUARED so the near field holds up and the tail goes quietly rather than stopping.
    // This multiplies the attenuation asset rather than replacing it (the asset still does the
    // spatialisation and its own falloff), so the audible distance is whichever of the two is shorter --
    // which is the point. It also covers the case the old code was written for: no asset at all.
    if (sk && g_audioVolume) {
        auto byDistance = [&](Radio& x) {
            if (!x.sound || !SitUI_Alive(&x.sref)) return;
            const float vol = VolumeOf(x);
            float v = vol; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
            const float range = g_rangeMin + (g_rangeMax - g_rangeMin) * v;
            const float full  = range * g_falloffStart;            // ...holds full volume this far out
            const float d = DistanceTo(x, sk);
            const float k = d <= full ? 1.0f : d >= range ? 0.0f : 1.0f - (d - full) / (range - full);
            // LINEAR, and only across the outer part of the range. This MULTIPLIES the attenuation asset,
            // which is already doing a natural near-field falloff of its own -- so squaring it here on top
            // of that, from 15% of the range out, compounded into "really loud when close but it gets quiet
            // quicker than I feel like it should" (field). Our job is only to end the tail where the volume
            // says it should end; the asset shapes the rest.
            __try { g_audioVolume(x.sound, vol * k); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            // WHO IS SILENCING IT. Two things can: this gain, and the attenuation ASSET's own curve,
            // which we cannot read. So print ours next to the distance -- if it reads ~1.00 where the
            // radio has already gone quiet, ours is not the limiter and the asset is (see SpawnOn).
            if (g_rangeDebug) {
                static ULONGLONG saidMs = 0;
                const ULONGLONG nowMs = GetTickCount64();
                if (nowMs > saidMs + 1000) {
                    saidMs = nowMs;
                    char who[64];
                    TwkLog("[radio] %s: %.0f m away, volume %.0f%%, our gain %.2f (range %.0f m, flat to %.0f m) -- "
                           "if you cannot hear it and this gain is near 1.00, the attenuation asset is what stops it",
                           Whose(x, who, sizeof(who)), d / 100.0f, vol * 100.0f, k, range / 100.0f, full / 100.0f);
                }
            }
            // COMING BACK INTO RANGE HAS TO START IT AGAIN. The engine stops a sound it has culled for
            // distance, and since the component is spawned with autoDestroy FALSE it survives that --
            // STOPPED. So "is the component still there" answers yes forever and nothing would ever
            // restart it: the radio went silent for good once you walked away, until it was turned off
            // and on again. Field 2026-09-19, and the direct cost of the autoDestroy fix that stopped it
            // restarting every 2 s -- this is the other half of owning the sound's life ourselves.
            // Hysteresis: out at the range, back in at 92% of it, so standing on the line cannot
            // retrigger this every frame.
            if (d >= range) x.farOut = true;
            else if (x.farOut && d < range * 0.92f) {
                x.farOut = false;
                if (x.source == radiowire::SRC_STREAM) StartSound(x);      // rewound: a live stream resumes at NOW
                else if (g_audioPlay) {                                    // the same component, from where the song is
                    __try { g_audioPlay(x.sound, x.songPos); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                    char who[64];
                    TwkLog("[radio] %s: back in range -- playing again from %.0f s", Whose(x, who, sizeof(who)), x.songPos);
                } else StartSound(x);
            }
        };
        byDistance(r);
        for (Radio& o : g_remote) if (o.used) byDistance(o);
    }
    // ---- tell the others: at once when something changed, and every few seconds so the song's clock stays true
    g_beatS += dt;
    if (g_dirty) { g_dirty = false; g_beatS = 0.0f; SendState(OMPMOD_EVERYONE, true); }
    else if (g_beatS > 3.0f && r.state != R_NONE) { g_beatS = 0.0f; SendState(OMPMOD_EVERYONE, false); }
}
