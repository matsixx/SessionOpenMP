// voice_capture -- the microphone: WASAPI shared-mode capture on its own thread, downmixed and
// resampled to 48 kHz mono, gated (push-to-talk or voice activation), Opus-encoded in 20 ms
// frames, handed to the game thread through a small mutex-protected queue.
//
// Everything the game thread decides (the mode, whether the talk key is down, the sensitivity)
// arrives through atomics; nothing here touches the game or the wire.
#pragma once
#include <cstdint>

namespace omp { namespace voice {

enum Mode : int { kOff = 0, kPushToTalk = 1, kOpenMic = 2 };

void Capture_Init(void (*logf)(const char*));
void Capture_Shutdown();
// Settings, pushed every frame from the game thread (cheap: atomics).
void Capture_SetMode(int mode);                // kOff stops the device; the thread idles
void Capture_SetTalkKey(bool down);            // push-to-talk state, read by the capture thread
void Capture_SetSensitivity(int pct);          // 0..100: how easily open-mic opens
// One encoded 20 ms frame off the queue, or 0. seqOut counts frames, wrapping, gaps meaning loss.
int  Capture_Pop(uint8_t* out, int cap, uint16_t* seqOut);
bool Capture_Speaking();                       // the gate is open right now (for an indicator)
bool Capture_DeviceOk();                       // a microphone is open and delivering
const char* Capture_DeviceName();              // for the settings page; "" when none
// Which microphone. Devices are enumerated on the capture thread (COM stays off the game thread);
// the list is a snapshot, refreshed on request and every 10 s. An id of "" means Windows' default.
void Capture_SetDevice(const char* id);
void Capture_RequestDeviceList();
int  Capture_DeviceCount();
bool Capture_DeviceAt(int i, char* name, int nameCap, char* id, int idCap);
// The gate as it stands: the last frame's level, the adaptive noise floor, the open threshold (all
// dBFS) and the share of frames the gate was open for since the last call. False = no microphone.
bool Capture_GateStats(float* levelDb, float* floorDb, float* thrDb, int* openPct);

}} // namespace omp::voice
