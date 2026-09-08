// voice_audio.cpp -- see the header. The engine side of proximity voice: objects, attenuation, the
// vtable-routed PCM pull, and the attached component per proxy.
#include "voice_audio.h"
#include "game_syms.h"
#include "audio.h"           // BeginOwnSpawn: the audio funnel must neither mute nor capture a voice
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game { namespace voice {

// ---- offsets (PDB, UE 4.26 layout of this game)
namespace o {
    constexpr int kObjFlags        = 0x08;    // UObjectBase::ObjectFlags
    constexpr int kSoundDuration   = 0x108;   // USoundBase::Duration
    constexpr int kWaveNumChannels = 0x25c;   // USoundWave::NumChannels
    constexpr int kWaveSampleRate  = 0x260;   // USoundWave::SampleRate
    constexpr int kAttenSettings   = 0x28;    // USoundAttenuation::Attenuation (FSoundAttenuationSettings)
    // FSoundAttenuationSettings (FBaseAttenuationSettings first, 176 bytes)
    constexpr int kDistanceAlgorithm = 0x08;  // EAttenuationDistanceModel (u8)
    constexpr int kAttenuationShape  = 0x09;  // EAttenuationShape (u8)
    constexpr int kDbAtMax           = 0x0c;  // float
    constexpr int kFalloffMode       = 0x10;  // ENaturalSoundFalloffMode (u8)
    constexpr int kShapeExtents      = 0x14;  // FVector
    constexpr int kFalloffDistance   = 0x24;  // float
    constexpr int kBits0             = 0xb0;  // bAttenuate.. bEnableReverbSend
    constexpr int kBits1             = 0xb1;  // bEnablePriorityAttenuation.. bEnableSubmixSends
    constexpr int kSpatAlgo          = 0xb2;  // u8
    constexpr int kAbsorptionMethod  = 0xb8;  // u8
    constexpr int kOcclusionChannel  = 0xb9;  // u8 ECollisionChannel
    constexpr int kReverbMethod      = 0xba;  // u8
    constexpr int kOmniRadius        = 0xbc;
    constexpr int kStereoSpread      = 0xc0;
    constexpr int kLPFRadiusMin      = 0xc4;
    constexpr int kLPFRadiusMax      = 0xc8;
    constexpr int kLPFFreqAtMin      = 0x1e0;
    constexpr int kLPFFreqAtMax      = 0x1e4;
    constexpr int kHPFFreqAtMin      = 0x1e8;
    constexpr int kHPFFreqAtMax      = 0x1ec;
    constexpr int kOccLPF            = 0x218;
    constexpr int kOccVolume         = 0x21c;
    constexpr int kOccInterp         = 0x220;
    constexpr int kReverbWetMin      = 0x224;
    constexpr int kReverbWetMax      = 0x228;
    constexpr int kReverbDistMin     = 0x22c;
    constexpr int kReverbDistMax     = 0x230;
    // FStaticConstructObjectParameters (64 bytes)
    constexpr int kSCOClass = 0x00, kSCOOuter = 0x08, kSCOName = 0x10, kSCOFlags = 0x18;
    constexpr uint32_t RF_Transient = 0x40, RF_MarkAsRootSet = 0x80;
}

// ---- the ring -------------------------------------------------------------------------------------
int Ring::Level() const {
    const long h = head, t = tail;
    return (int)((h - t + kCap) % kCap);
}
int Ring::Write(const int16_t* s, int n) {
    int room = kCap - 1 - Level();
    if (n > room) n = room;
    long h = head;
    for (int i = 0; i < n; i++) { buf[h] = s[i]; h = (h + 1) % kCap; }
    head = h;                              // one store publishes the block
    return n;
}
int Ring::Read(int16_t* out, int n) {
    int have = Level();
    const int got = have < n ? have : n;
    long t = tail;
    for (int i = 0; i < got; i++) { out[i] = buf[t]; t = (t + 1) % kCap; }
    tail = t;
    for (int i = got; i < n; i++) out[i] = 0;
    return got;
}
void Ring::Clear() { tail = head; }

// ---- engine objects --------------------------------------------------------------------------------
static void*  g_waveClass  = nullptr;
static void*  g_attenClass = nullptr;
static void*  g_transient  = nullptr;
static void*  g_atten      = nullptr;      // the shared USoundAttenuation, rooted
static void** g_vtableCopy = nullptr;      // USoundWaveProcedural's vtable with GeneratePCMData rerouted
static int    g_pcmSlot    = -1;
static float  g_rangeM     = 30.0f;
static bool   g_authored   = false;
static bool   g_saidReady  = false, g_saidFail = false;
static void (*g_logf)(const char*) = nullptr;

// wave -> its Voice, for the audio thread. Written on the game thread before the component plays.
enum { kMaxVoices = 16 };
static void*  volatile g_mapWave[kMaxVoices];
static Voice* volatile g_mapVoice[kMaxVoices];

static Voice* voiceForWave(void* wave) {
    for (int i = 0; i < kMaxVoices; i++) if (g_mapWave[i] == wave) return g_mapVoice[i];
    return nullptr;
}
static void mapWave(void* wave, Voice* v) {
    for (int i = 0; i < kMaxVoices; i++) if (g_mapWave[i] == wave) { g_mapVoice[i] = v; return; }
    for (int i = 0; i < kMaxVoices; i++) if (!g_mapWave[i]) { g_mapVoice[i] = v; g_mapWave[i] = wave; return; }
}

// AUDIO THREAD. The engine asks for up to SamplesNeeded 16-bit mono samples per pull and takes
// what it is handed; ZERO bytes is its "this sound has finished", so a pull always returns some.
// Each pull is ONE voice frame (960 samples, 20 ms). The engine asks for thousands at a time, and
// handing it all of that meant the first buffer of a burst was the cushion's 40 ms of voice padded
// with 130 ms of silence -- heard as a cut at the start of every sentence -- and every later buffer
// ended in a small gap. The engine's own procedural wave caps a pull at 1024 for the same reason.
static int32_t __fastcall GeneratePCM(void* wave, uint8_t* pcm, int32_t samplesNeeded) {
    if (!pcm || samplesNeeded <= 0) return 0;
    const int32_t n = samplesNeeded < 960 ? samplesNeeded : 960;
    int16_t* out = (int16_t*)pcm;
    Voice* v = voiceForWave(wave);
    if (!v) { memset(pcm, 0, (size_t)n * 2); return n * 2; }
    Ring& r = v->ring;
    if (!v->primed) {
        // a three-frame cushion (60 ms): frames arrive on the game's ~17 ms tick, so two frames
        // was one late tick from running dry at the start of every burst
        if (r.Level() >= 2880) v->primed = true;
        else { memset(pcm, 0, (size_t)n * 2); return n * 2; }
    }
    const int got = r.Read(out, n);
    if (got < n) v->primed = false;                      // ran dry: re-cushion before the next stretch
    return n * 2;
}

#ifdef _WIN32
static bool textRange(uintptr_t* lo, uintptr_t* hi) {
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return false;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                *lo = (uintptr_t)base + sec[i].VirtualAddress;
                *hi = *lo + sec[i].Misc.VirtualSize;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void* findObject(const wchar_t* path) {
    const Syms& S = Get();
    if (!S.StaticFindObject) return nullptr;
    __try { return S.StaticFindObject(nullptr, nullptr, path, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* construct(void* cls) {
    const Syms& S = Get();
    if (!S.StaticConstructObject || !cls || !g_transient) return nullptr;
    uint8_t params[64]; memset(params, 0, sizeof(params));
    *(void**)(params + o::kSCOClass) = cls;
    *(void**)(params + o::kSCOOuter) = g_transient;
    *(uint64_t*)(params + o::kSCOName) = 0;                      // NAME_None: the engine names it
    *(uint32_t*)(params + o::kSCOFlags) = o::RF_Transient | o::RF_MarkAsRootSet;
    __try { return S.StaticConstructObject(params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The one-time vtable copy: the run of .text pointers starting at the object's vtable, with the
// GeneratePCMData slot (found by its resolved address) pointing at ours.
static bool buildVtable(void* wave) {
    if (g_vtableCopy) return true;
    const Syms& S = Get();
    if (!S.SoundGeneratePCM) return false;
    uintptr_t lo = 0, hi = 0;
    if (!textRange(&lo, &hi)) return false;
    __try {
        void** vt = *(void***)wave;
        int n = 0, slot = -1;
        for (; n < 512; n++) {
            const uintptr_t e = (uintptr_t)vt[n];
            if (e < lo || e >= hi) break;                        // the run of code pointers ended
            if (vt[n] == (void*)S.SoundGeneratePCM) slot = n;
        }
        if (slot < 0 || n < 8) return false;
        void** copy = (void**)malloc(sizeof(void*) * (size_t)n);
        if (!copy) return false;
        memcpy(copy, vt, sizeof(void*) * (size_t)n);
        copy[slot] = (void*)&GeneratePCM;
        g_vtableCopy = copy; g_pcmSlot = slot;
        if (g_logf) { char m[160];
            snprintf(m, sizeof(m), "[voice] SoundWaveProcedural vtable copied (%d entries), GeneratePCMData at slot %d rerouted", n, slot);
            g_logf(m); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void author(uint8_t* a, float rangeM) {
    float falloff = rangeM * 100.0f - 150.0f;
    if (falloff < 100.0f) falloff = 100.0f;
    // the shape: a 1.5 m sphere at full volume, then a natural-sound roll-off to silence
    a[o::kDistanceAlgorithm] = 4;                            // NaturalSound
    a[o::kAttenuationShape]  = 0;                            // Sphere
    *(float*)(a + o::kDbAtMax) = -60.0f;
    a[o::kFalloffMode] = 1;                                  // Silent past the falloff
    float* ext = (float*)(a + o::kShapeExtents); ext[0] = 150.0f; ext[1] = 0.0f; ext[2] = 0.0f;
    *(float*)(a + o::kFalloffDistance) = falloff;
    // attenuate | spatialize | LPF with distance | occlusion | reverb send
    a[o::kBits0] = 0x01 | 0x02 | 0x04 | 0x20 | 0x80;
    a[o::kBits1] = 0x04;                                     // log frequency scaling (natural)
    a[o::kSpatAlgo] = 0;                                     // the engine's panner
    a[o::kAbsorptionMethod] = 0;                             // Linear
    a[o::kOcclusionChannel] = 3;                             // ECC_Visibility
    a[o::kReverbMethod] = 0;                                 // Linear
    *(float*)(a + o::kOmniRadius)   = 60.0f;                 // a voice at your ear is not hard-panned
    *(float*)(a + o::kStereoSpread) = 0.0f;
    // air absorption: crisp up close, muffled toward the edge of the range
    *(float*)(a + o::kLPFRadiusMin) = 400.0f;
    *(float*)(a + o::kLPFRadiusMax) = falloff * 0.9f;
    *(float*)(a + o::kLPFFreqAtMin) = 20000.0f;
    *(float*)(a + o::kLPFFreqAtMax) = 1200.0f;
    *(float*)(a + o::kHPFFreqAtMin) = 0.0f;
    *(float*)(a + o::kHPFFreqAtMax) = 0.0f;
    // a wall between you: quieter and muffled, eased over 150 ms so it does not click
    *(float*)(a + o::kOccLPF)    = 700.0f;
    *(float*)(a + o::kOccVolume) = 0.45f;
    *(float*)(a + o::kOccInterp) = 0.15f;
    // the room: a little wet up close, more of it far away, where the level has reverb
    *(float*)(a + o::kReverbWetMin)  = 0.15f;
    *(float*)(a + o::kReverbWetMax)  = 0.70f;
    *(float*)(a + o::kReverbDistMin) = 150.0f;
    *(float*)(a + o::kReverbDistMax) = falloff;
}

bool Ready(void (*logf)(const char*)) {
    if (logf) g_logf = logf;
    const Syms& S = Get();
    if (!S.StaticFindObject || !S.StaticConstructObject || !S.SoundGeneratePCM ||
        !S.SpawnSoundAttached || !S.AudioPlay || !S.AudioStop) {
        if (!g_saidFail && g_logf) { g_saidFail = true;
            g_logf("[voice] engine audio symbols unresolved -- voice playback is off (capture and mute still work)"); }
        return false;
    }
    if (!g_waveClass)  g_waveClass  = findObject(L"/Script/Engine.SoundWaveProcedural");
    if (!g_attenClass) g_attenClass = findObject(L"/Script/Engine.SoundAttenuation");
    if (!g_transient)  g_transient  = findObject(L"/Engine/Transient");
    if (!g_waveClass || !g_attenClass || !g_transient) {
        static bool said = false;
        if (!said && g_logf) { said = true; char m[200];
            snprintf(m, sizeof(m), "[voice] engine playback: class lookup (SoundWaveProcedural=%p SoundAttenuation=%p /Engine/Transient=%p) -- retrying",
                     g_waveClass, g_attenClass, g_transient);
            g_logf(m); }
        return false;
    }
    if (!g_atten) {
        g_atten = construct(g_attenClass);
        if (!g_atten) {
            static bool said = false;
            if (!said && g_logf) { said = true; g_logf("[voice] engine playback: could not construct the SoundAttenuation object -- retrying"); }
            return false;
        }
        g_authored = false;
    }
    if (!g_authored) {
        __try { author((uint8_t*)g_atten + o::kAttenSettings, g_rangeM); g_authored = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    if (!g_saidReady && g_logf) { g_saidReady = true;
        char m[160]; snprintf(m, sizeof(m), "[voice] engine playback ready: classes found, attenuation authored (range %.0f m)", g_rangeM);
        g_logf(m); }
    return true;
}

void SetRange(float metres) {
    if (metres < 2.0f) metres = 2.0f; else if (metres > 200.0f) metres = 200.0f;
    if (metres != g_rangeM) { g_rangeM = metres; g_authored = false; }
}

static bool ensureWave(Voice& v) {
    if (v.wave) return true;
    void* w = construct(g_waveClass);
    if (!w) {
        static bool said = false;
        if (!said && g_logf) { said = true; g_logf("[voice] engine playback: could not construct a SoundWaveProcedural -- retrying"); }
        return false;
    }
    __try {
        *(int32_t*)((uint8_t*)w + o::kWaveNumChannels) = 1;
        *(int32_t*)((uint8_t*)w + o::kWaveSampleRate)  = 48000;
        *(float*)((uint8_t*)w + o::kSoundDuration) = 10000.0f;   // INDEFINITELY_LOOPING_DURATION
        if (!buildVtable(w)) {
            static bool said = false;
            if (!said && g_logf) { said = true; g_logf("[voice] engine playback: GeneratePCMData slot NOT found in the wave's vtable -- no voice playback"); }
            return false;
        }
        *(void***)w = g_vtableCopy;                               // this object's calls come here now
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_logf) g_logf("[voice] engine playback: wave setup FAULTED");
        return false;
    }
    v.wave = w;
    mapWave(w, &v);
    return true;
}

bool Start(Voice& v, void* proxyActor, float volume, void (*logf)(const char*)) {
    if (!proxyActor || !Ready(logf)) return false;
    if (!ensureWave(v)) return false;
    const Syms& S = Get();
    if (v.comp && v.compActor != proxyActor) { v.comp = nullptr; v.compActor = nullptr; v.playing = false; }
    if (!v.comp) {
        void* mesh = SkaterMeshOf(proxyActor);
        if (!mesh) {
            static bool said = false;
            if (!said && g_logf) { said = true; g_logf("[voice] engine playback: the skater has no mesh yet -- retrying"); }
            return false;
        }
        FVec3 loc{ 0.0f, 0.0f, 160.0f };                                 // mesh-local: the head
        FRot3 rot{ 0.0f, 0.0f, 0.0f };
        void* comp = nullptr;
        // Through the audio funnel's hook on this native. Field: a spawn attached to a PROXY is what
        // the funnel mutes (the proxy's own board sounds), so without the guard every peer's voice
        // came back null while our own skater's played. The guard also keeps the spawn out of the
        // funnel's capture, or our voice would go out to peers as one of our game sounds.
        audio::BeginOwnSpawn();
        __try {
            comp = S.SpawnSoundAttached(v.wave, mesh, 0ull, loc, rot, 0 /*KeepRelativeOffset*/,
                                        true /*stop when the actor goes*/, volume, 1.0f, 0.0f,
                                        g_atten, nullptr, false /*keep the component*/);
        } __except (EXCEPTION_EXECUTE_HANDLER) { comp = nullptr; if (g_logf) g_logf("[voice] SpawnSoundAttached FAULTED"); }
        audio::EndOwnSpawn();
        if (!comp) {
            static bool said = false;
            if (!said && g_logf) { said = true; char m[160];
                snprintf(m, sizeof(m), "[voice] SpawnSoundAttached returned null (wave=%p mesh=%p atten=%p) -- retrying", v.wave, mesh, g_atten);
                g_logf(m); }
            return false;
        }
        v.comp = comp; v.compActor = proxyActor; v.playing = true;   // SpawnSoundAttached plays
        v.ring.Clear(); v.primed = false;
        return true;
    }
    if (!v.playing) {
        v.ring.Clear(); v.primed = false;
        __try { S.AudioPlay(v.comp, 0.0f); if (S.AudioSetVolume) S.AudioSetVolume(v.comp, volume); }
        __except (EXCEPTION_EXECUTE_HANDLER) { v.comp = nullptr; v.compActor = nullptr; return false; }
        v.playing = true;
    }
    return true;
}

void Stop(Voice& v) {
    if (!v.comp || !v.playing) return;
    const Syms& S = Get();
    v.playing = false;
    __try { if (S.AudioStop) S.AudioStop(v.comp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v.comp = nullptr; v.compActor = nullptr; }
    v.ring.Clear(); v.primed = false;
}

void SetVolume(Voice& v, float volume) {
    if (!v.comp) return;
    const Syms& S = Get();
    __try { if (S.AudioSetVolume) S.AudioSetVolume(v.comp, volume); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ProxyGone(Voice& v) {
    v.comp = nullptr; v.compActor = nullptr; v.playing = false;
    v.ring.Clear(); v.primed = false;
}
#else
bool Ready(void (*)(const char*)) { return false; }
void SetRange(float) {}
bool Start(Voice&, void*, float, void (*)(const char*)) { return false; }
void Stop(Voice&) {}
void SetVolume(Voice&, float) {}
void ProxyGone(Voice& v) { v.comp = nullptr; v.compActor = nullptr; v.playing = false; }
#endif

}}} // namespace omp::game::voice
