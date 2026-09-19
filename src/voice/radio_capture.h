// radio_capture -- ONE APP's sound, from this PC, for a player's radio (SessionTweaks' radio prop).
//
// Windows' PROCESS LOOPBACK (Windows 11 / Windows 10 build 20348 and later): the audio a chosen process -- and
// the processes it started -- renders, and NOTHING else: not the game, not a voice call, not system sounds.
// Captured on its own thread, downmixed to mono, Opus-encoded for MUSIC in 20 ms frames, handed to the game
// thread through a small queue. Also lists the apps that have sound open right now, to choose from.
// Nothing here touches the game or the wire; the session decides who hears it.
#pragma once
#include <cstdint>

namespace omp { namespace radiocap {

struct Source { uint32_t pid; char name[48]; bool playing; };

void Init(void (*logf)(const char*));
void Shutdown();
// Capture this process's sound (and its children's). 0 = stop. Takes effect on the capture thread.
void Start(uint32_t pid, const char* name);
void Stop();
// 0 off, 1 starting, 2 capturing, -1 failed (`why` says why, in words for the player).
int  State(char* why, int cap);
// One encoded frame off the queue, or 0. seqOut counts frames (wrapping; a gap is a loss).
int  Pop(uint8_t* out, int cap, uint16_t* seqOut);
// The apps with sound open, as last listed (the list is refreshed on the capture thread when asked for,
// at most every 2 s). Playing ones first.
void RequestSources();
int  Sources(Source* out, int cap);

}} // namespace omp::radiocap
