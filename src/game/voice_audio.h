// voice_audio -- a remote player's voice played through the GAME's own audio engine, from their proxy.
//
// One USoundWaveProcedural per peer slot, attached to the peer's proxy by UGameplayStatics::
// SpawnSoundAttached with a USoundAttenuation authored here: natural-sound falloff, air-absorption
// low-pass with distance, occlusion by the level's geometry, a reverb send. The engine then does what
// it does for every other 3D sound in the game -- panning, distance, the master volume, ducking in
// menus -- so a voice sounds like it is coming from the skater, not from a second mixer.
//
// The shipped exe has no USoundWaveProcedural::QueueAudio (dead-stripped: the game never queues
// PCM), so the wave is fed from the OTHER end: each wave object gets a private copy of its vtable
// with GeneratePCMData pointing here, and the audio render thread pulls PCM straight out of a
// lock-free ring the game thread fills. No engine allocator, no engine queue. A ring that runs dry
// yields silence, never a short read -- a short read is how the engine decides a sound has finished.
#pragma once
#include <cstdint>

namespace omp { namespace game { namespace voice {

// The pull side of one peer's audio: a single-producer (game thread) / single-consumer (audio
// thread) ring of 16-bit mono 48 kHz samples.
struct Ring {
    enum { kCap = 48000 };                 // 1 s -- far more than the jitter target ever holds
    int16_t  buf[kCap];
    volatile long head = 0, tail = 0;      // head = write (game), tail = read (audio)
    int  Level() const;                    // samples queued
    int  Write(const int16_t* s, int n);   // drops what does not fit
    int  Read(int16_t* out, int n);        // zero-fills past the level
    void Clear();
};

// Per-slot playback state, owned by the caller (session's slot table); this module only fills it.
struct Voice {
    void*   wave = nullptr;          // USoundWaveProcedural, rooted, reused across worlds
    void*   comp = nullptr;          // UAudioComponent on the current proxy (null = none / gone)
    void*   compActor = nullptr;     // the actor it was spawned on -- a different actor = stale comp
    bool    playing = false;
    Ring    ring;
    bool    primed = false;          // the reader waits for a small buffer before it streams
};

// Resolve the classes and build the shared attenuation object. Cheap and idempotent; call each frame
// before use (the classes can only be found once the engine has loaded them).
bool Ready(void (*logf)(const char*));
// Author the shared attenuation from the player's settings (range in metres, 1 = full volume ...).
void SetRange(float metres);
// Make sure the slot's wave exists and its component sits on THIS actor, then start it. False = the
// engine side is not ready or the spawn failed (nothing to hear; the packets are dropped upstream).
bool Start(Voice& v, void* proxyActor, float volume, void (*logf)(const char*));
void Stop(Voice& v);
void SetVolume(Voice& v, float volume);
// The proxy actor died with its world (or the peer left): drop the component pointer WITHOUT
// touching it. The wave survives (rooted) for the next spawn.
void ProxyGone(Voice& v);

}}} // namespace omp::game::voice
