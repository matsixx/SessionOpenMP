// voice_capture.cpp -- see the header.
#include "voice_capture.h"
#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif
#include <opus.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

namespace omp { namespace voice {

static void (*g_logf)(const char*) = nullptr;
static void say(const char* fmt, ...) {
    if (!g_logf) return;
    char m[300]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    g_logf(m);
}

// ---- settings and state shared with the game thread
static volatile LONG g_mode = kOff, g_talkKey = 0, g_sens = 50;
static volatile LONG g_speaking = 0, g_deviceOk = 0, g_quit = 0, g_listWanted = 1;
static volatile LONG g_gateOpenFrames = 0, g_gateTotalFrames = 0;
static volatile LONG g_levelMilliDb = -90000, g_floorMilliDb = -60000, g_thrMilliDb = -40000;
static char g_deviceName[96] = {0};

// ---- the encoded-frame queue: capture thread pushes, game thread pops. Small on purpose: a frame
// that waited longer than the queue is stale voice, and the oldest is the one to drop.
enum { kQCap = 32, kFrameMax = 400, kMaxDevs = 7, kIdCap = 200 };
struct Frame { uint16_t seq; uint16_t len; uint8_t data[kFrameMax]; };
static Frame g_q[kQCap];
static int g_qHead = 0, g_qTail = 0;
static uint16_t g_seq = 0;
// the device list (a snapshot the capture thread refreshes) and the wanted device id
struct DevInfo { char name[64]; char id[kIdCap]; };
static DevInfo g_devs[kMaxDevs];
static int     g_devN = 0;
static char    g_wantId[kIdCap] = {0};
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static HANDLE g_thread = nullptr;
#endif

void Capture_SetMode(int mode)          { InterlockedExchange(&g_mode, (LONG)mode); }
void Capture_SetTalkKey(bool down)      { InterlockedExchange(&g_talkKey, down ? 1 : 0); }
void Capture_SetSensitivity(int pct)    { if (pct < 0) pct = 0; if (pct > 100) pct = 100; InterlockedExchange(&g_sens, pct); }
bool Capture_Speaking()                 { return g_speaking != 0; }
bool Capture_DeviceOk()                 { return g_deviceOk != 0; }
const char* Capture_DeviceName()        { return g_deviceName; }
void Capture_RequestDeviceList()        { InterlockedExchange(&g_listWanted, 1); }
bool Capture_GateStats(float* levelDb, float* floorDb, float* thrDb, int* openPct) {
    if (levelDb) *levelDb = (float)g_levelMilliDb / 1000.0f;
    if (floorDb) *floorDb = (float)g_floorMilliDb / 1000.0f;
    if (thrDb)   *thrDb   = (float)g_thrMilliDb / 1000.0f;
    const LONG tot = g_gateTotalFrames;
    if (openPct) *openPct = tot > 0 ? (int)(100 * g_gateOpenFrames / tot) : 0;
    InterlockedExchange(&g_gateOpenFrames, 0); InterlockedExchange(&g_gateTotalFrames, 0);
    return g_deviceOk != 0;
}

#ifdef _WIN32
void Capture_SetDevice(const char* id) {
    if (!g_thread) return;
    EnterCriticalSection(&g_lock);
    strncpy_s(g_wantId, id ? id : "", _TRUNCATE);
    LeaveCriticalSection(&g_lock);
}
int Capture_DeviceCount() { return g_thread ? g_devN : 0; }
bool Capture_DeviceAt(int i, char* name, int nameCap, char* id, int idCap) {
    if (!g_thread) return false;
    bool ok = false;
    EnterCriticalSection(&g_lock);
    if (i >= 0 && i < g_devN) {
        if (name && nameCap > 0) strncpy_s(name, (size_t)nameCap, g_devs[i].name, _TRUNCATE);
        if (id && idCap > 0)     strncpy_s(id,   (size_t)idCap,   g_devs[i].id,   _TRUNCATE);
        ok = true;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

static void push(const uint8_t* d, int n) {
    if (n <= 0 || n > kFrameMax) return;
    EnterCriticalSection(&g_lock);
    const int next = (g_qHead + 1) % kQCap;
    if (next == g_qTail) g_qTail = (g_qTail + 1) % kQCap;        // full: the oldest goes
    Frame& f = g_q[g_qHead];
    f.seq = g_seq++; f.len = (uint16_t)n; memcpy(f.data, d, (size_t)n);
    g_qHead = next;
    LeaveCriticalSection(&g_lock);
}
int Capture_Pop(uint8_t* out, int cap, uint16_t* seqOut) {
    if (!g_thread) return 0;
    EnterCriticalSection(&g_lock);
    if (g_qTail == g_qHead) { LeaveCriticalSection(&g_lock); return 0; }
    const Frame& f = g_q[g_qTail];
    int n = f.len;
    if (n > cap) n = 0; else memcpy(out, f.data, (size_t)n);
    if (seqOut) *seqOut = f.seq;
    g_qTail = (g_qTail + 1) % kQCap;
    LeaveCriticalSection(&g_lock);
    return n;
}

// ---- the device
static void friendlyName(IMMDevice* dev, char* out, int cap) {
    out[0] = 0;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, out, cap - 1, nullptr, nullptr);
        PropVariantClear(&pv);
        ps->Release();
    }
}
// The list of active capture devices, into the shared snapshot.
static void enumerate(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) || !col) return;
    UINT cnt = 0; col->GetCount(&cnt);
    DevInfo tmp[kMaxDevs]; int n = 0;
    for (UINT i = 0; i < cnt && n < kMaxDevs; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        tmp[n].id[0] = 0; tmp[n].name[0] = 0;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) {
            WideCharToMultiByte(CP_UTF8, 0, id, -1, tmp[n].id, kIdCap - 1, nullptr, nullptr);
            CoTaskMemFree(id);
        }
        friendlyName(d, tmp[n].name, sizeof(tmp[n].name));
        d->Release();
        if (tmp[n].id[0]) n++;
    }
    col->Release();
    EnterCriticalSection(&g_lock);
    memcpy(g_devs, tmp, sizeof(DevInfo) * (size_t)n); g_devN = n;
    LeaveCriticalSection(&g_lock);
}

struct Dev {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    HANDLE               ev = nullptr;
    WAVEFORMATEX*        fmt = nullptr;
    int  channels = 0, rate = 0, bits = 0;
    bool isFloat = false;
    char openedId[kIdCap] = {0};          // the wanted id this device was opened for ("" = default)
};
static void devClose(Dev& d) {
    if (d.ac) d.ac->Stop();
    if (d.cc) { d.cc->Release(); d.cc = nullptr; }
    if (d.ac) { d.ac->Release(); d.ac = nullptr; }
    if (d.fmt) { CoTaskMemFree(d.fmt); d.fmt = nullptr; }
    if (d.dev) { d.dev->Release(); d.dev = nullptr; }
    if (d.ev) { CloseHandle(d.ev); d.ev = nullptr; }
    InterlockedExchange(&g_deviceOk, 0);
}
static bool devOpen(Dev& d, const char* wantId) {
    devClose(d);
    if (!d.en) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&d.en)) || !d.en) return false;
    }
    strncpy_s(d.openedId, wantId ? wantId : "", _TRUNCATE);
    if (d.openedId[0]) {
        wchar_t wide[kIdCap];
        MultiByteToWideChar(CP_UTF8, 0, d.openedId, -1, wide, kIdCap);
        if (FAILED(d.en->GetDevice(wide, &d.dev)) || !d.dev) {
            say("[voice] the chosen microphone is not available -- using the default");
            d.dev = nullptr;
        }
    }
    if (!d.dev && FAILED(d.en->GetDefaultAudioEndpoint(eCapture, eCommunications, &d.dev))) d.dev = nullptr;
    if (!d.dev) { say("[voice] no microphone: no capture device"); devClose(d); return false; }
    friendlyName(d.dev, g_deviceName, sizeof(g_deviceName));
    HRESULT hr = d.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&d.ac);
    if (FAILED(hr) || !d.ac) { devClose(d); return false; }
    hr = d.ac->GetMixFormat(&d.fmt);
    if (FAILED(hr) || !d.fmt) { devClose(d); return false; }
    d.channels = d.fmt->nChannels; d.rate = (int)d.fmt->nSamplesPerSec; d.bits = d.fmt->wBitsPerSample;
    d.isFloat = false;
    if (d.fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) d.isFloat = true;
    else if (d.fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)d.fmt;
        d.isFloat = (x->SubFormat.Data1 == 3);              // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    }
    if (d.channels < 1 || d.rate < 8000 || (!d.isFloat && d.bits != 16 && d.bits != 32)) {
        say("[voice] microphone format not supported (%d ch, %d Hz, %d bit, %s)", d.channels, d.rate, d.bits, d.isFloat ? "float" : "pcm");
        devClose(d); return false;
    }
    hr = d.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                          1000000 /* 100 ms, in 100 ns units */, 0, d.fmt, nullptr);
    if (FAILED(hr)) { say("[voice] microphone open failed (0x%08x)", (unsigned)hr); devClose(d); return false; }
    d.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.ev || FAILED(d.ac->SetEventHandle(d.ev))) { devClose(d); return false; }
    hr = d.ac->GetService(__uuidof(IAudioCaptureClient), (void**)&d.cc);
    if (FAILED(hr) || !d.cc) { devClose(d); return false; }
    if (FAILED(d.ac->Start())) { devClose(d); return false; }
    InterlockedExchange(&g_deviceOk, 1);
    say("[voice] microphone open: '%s' (%d ch, %d Hz, %s) -> 48 kHz mono Opus", g_deviceName, d.channels, d.rate, d.isFloat ? "float" : "pcm");
    return true;
}
static inline float monoSample(const Dev& d, const BYTE* p, UINT32 i) {
    float acc = 0.0f;
    if (d.isFloat) {
        const float* s = (const float*)p + (size_t)i * d.channels;
        for (int c = 0; c < d.channels; c++) acc += s[c];
    } else if (d.bits == 16) {
        const int16_t* s = (const int16_t*)p + (size_t)i * d.channels;
        for (int c = 0; c < d.channels; c++) acc += (float)s[c] / 32768.0f;
    } else {
        const int32_t* s = (const int32_t*)p + (size_t)i * d.channels;
        for (int c = 0; c < d.channels; c++) acc += (float)s[c] / 2147483648.0f;
    }
    return acc / (float)d.channels;
}

// ---- the frame pipeline: high-pass -> resample -> 20 ms frames -> the gate -> encode
//
// THE GATE. A threshold alone opens on every breath through a hot headset mic. This one has two
// parts, and the louder of the two is the threshold:
//   * relative to the room -- an adaptive noise floor (minimum statistics: it drops at once to
//     anything quieter, rises only 1.5 dB/s and never above -30 dB, so it cannot learn your speech
//     as "noise") plus a margin from the sensitivity slider (6 dB at 100, 30 dB at 0);
//   * absolute -- from the same slider, -60 dBFS at 100 up to -25 dBFS at 0. Field: a mic that idles
//     at -77 dB drives the floor to its clamp, and a floor-relative gate then opens on breathing at
//     -45 dB no matter what the slider says; the absolute part is what the slider means on such a mic.
// Then 6 dB of hysteresis to close, a 300 ms hold so the ends of words survive, and one frame of
// pre-roll so the first syllable is not clipped. The gate applies in BOTH modes: push-to-talk means
// the key allows it, the level still has to earn it (field: breathing while holding the key went
// through when the key alone was the gate). Releasing the key closes at once.
struct Pipe {
    OpusEncoder* enc = nullptr;
    float  frame[960]; int fill = 0;
    float  prev[960];  bool havePrev = false;       // the pre-roll frame
    float  rsPrev = 0.0f; double rsPos = 0.0, rsStep = 1.0;
    float  hpX = 0.0f, hpY = 0.0f;                  // the 90 Hz high-pass state
    float  envDb = -90.0f, floorDb = -60.0f;
    int    frames = 0;                              // since the device opened (the floor converges fast at first)
    bool   open = false; int hold = 0;
    uint8_t out[kFrameMax];
};
static void encodeAndPush(Pipe& p, const float* f) {
    if (!p.enc) return;
    const int n = opus_encode_float(p.enc, f, 960, p.out, kFrameMax);
    if (n > 2) push(p.out, n);                      // 1-2 bytes = an empty (DTX) frame: nothing to send
}
static void processFrame(Pipe& p) {
    const int mode = g_mode;
    float sq = 0.0f;
    for (int i = 0; i < 960; i++) sq += p.frame[i] * p.frame[i];
    const float rms = sqrtf(sq / 960.0f);
    float levelDb = 20.0f * log10f(rms + 1e-7f);
    if (levelDb < -100.0f) levelDb = -100.0f; else if (levelDb > 0.0f) levelDb = 0.0f;
    // envelope: instant attack, 40 dB/s release
    p.envDb = levelDb > p.envDb - 0.8f ? levelDb : p.envDb - 0.8f;
    // the floor: down at once, up slowly (fast for the first half second after a device opens)
    const float rise = (p.frames < 25) ? 0.5f : 0.03f;
    if (levelDb < p.floorDb) p.floorDb = levelDb;
    else if (p.floorDb + rise < -30.0f) p.floorDb += rise;
    if (p.floorDb < -85.0f) p.floorDb = -85.0f;
    p.frames++;
    const float sens = (float)g_sens / 100.0f;
    const float marginDb = 30.0f - 24.0f * sens;
    const float absDb    = -25.0f - 35.0f * sens;
    float openDb = p.floorDb + marginDb;
    if (openDb < absDb) openDb = absDb;
    const float closeDb = openDb - 6.0f;
    const bool allowed = (mode == kOpenMic) || (mode == kPushToTalk && g_talkKey != 0);
    bool open = false, preRoll = false;
    if (!allowed) {
        p.open = false; p.hold = 0;                  // the key came up: closed at once, no tail
    } else if (!p.open) {
        if (p.envDb >= openDb) { p.open = true; p.hold = 15; preRoll = true; }
    } else {
        if (p.envDb >= closeDb) p.hold = 15;
        else if (--p.hold <= 0) p.open = false;
    }
    open = p.open;
    InterlockedExchange(&g_speaking, open ? 1 : 0);
    InterlockedExchange(&g_levelMilliDb, (LONG)(levelDb * 1000.0f));
    InterlockedExchange(&g_floorMilliDb, (LONG)(p.floorDb * 1000.0f));
    InterlockedExchange(&g_thrMilliDb, (LONG)(openDb * 1000.0f));
    InterlockedIncrement(&g_gateTotalFrames);
    if (open) {
        InterlockedIncrement(&g_gateOpenFrames);
        if (preRoll && p.havePrev) encodeAndPush(p, p.prev);
        encodeAndPush(p, p.frame);
    }
    memcpy(p.prev, p.frame, sizeof(p.prev)); p.havePrev = true;
    p.fill = 0;
}
static inline void emit(Pipe& p, float s) {
    // 90 Hz one-pole high-pass: handling rumble and breath thumps do not reach the gate
    const float y = 0.9883f * (p.hpY + s - p.hpX);
    p.hpX = s; p.hpY = y;
    p.frame[p.fill++] = y;
    if (p.fill == 960) processFrame(p);
}
static inline void onSource(Pipe& p, float cur) {
    while (p.rsPos < 1.0) { emit(p, p.rsPrev + (cur - p.rsPrev) * (float)p.rsPos); p.rsPos += p.rsStep; }
    p.rsPos -= 1.0; p.rsPrev = cur;
}

static DWORD WINAPI captureThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Pipe p;
    int err = 0;
    p.enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!p.enc || err != OPUS_OK) { say("[voice] Opus encoder failed (%d) -- voice capture off", err); p.enc = nullptr; }
    else {
        opus_encoder_ctl(p.enc, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(p.enc, OPUS_SET_COMPLEXITY(6));
        opus_encoder_ctl(p.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(p.enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(p.enc, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(p.enc, OPUS_SET_DTX(0));
    }
    Dev d;
    ULONGLONG nextTryMs = 0, nextListMs = 0;
    while (!g_quit) {
        const ULONGLONG now = GetTickCount64();
        // the device list: on request (the settings page opening) and every 10 s regardless
        if (g_listWanted || now >= nextListMs) {
            InterlockedExchange(&g_listWanted, 0);
            nextListMs = now + 10000;
            if (!d.en) CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&d.en);
            if (d.en) enumerate(d.en);
        }
        if (g_mode == kOff || !p.enc) {
            if (d.ac) { devClose(d); say("[voice] microphone closed"); }
            InterlockedExchange(&g_speaking, 0);
            Sleep(100);
            continue;
        }
        char want[kIdCap];
        EnterCriticalSection(&g_lock); strncpy_s(want, g_wantId, _TRUNCATE); LeaveCriticalSection(&g_lock);
        if (d.ac && strcmp(want, d.openedId) != 0) { devClose(d); nextTryMs = 0; }   // a different microphone was chosen
        if (!d.ac) {
            if (now < nextTryMs) { Sleep(100); continue; }
            nextTryMs = now + 3000;
            if (!devOpen(d, want)) continue;
            p.rsStep = (double)d.rate / 48000.0; p.rsPos = 0.0; p.rsPrev = 0.0f; p.fill = 0;
            p.hpX = p.hpY = 0.0f; p.envDb = -90.0f; p.floorDb = -60.0f; p.frames = 0;
            p.open = false; p.hold = 0; p.havePrev = false;
        }
        if (WaitForSingleObject(d.ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 pk = 0;
        while (d.cc && SUCCEEDED(d.cc->GetNextPacketSize(&pk)) && pk > 0) {
            BYTE* buf = nullptr; UINT32 nf = 0; DWORD flags = 0;
            const HRESULT hr = d.cc->GetBuffer(&buf, &nf, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { say("[voice] microphone lost -- retrying"); devClose(d); }
                break;
            }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < nf; i++) onSource(p, silent ? 0.0f : monoSample(d, buf, i));
            d.cc->ReleaseBuffer(nf);
        }
    }
    devClose(d);
    if (d.en) { d.en->Release(); d.en = nullptr; }
    if (p.enc) opus_encoder_destroy(p.enc);
    CoUninitialize();
    return 0;
}

void Capture_Init(void (*logf)(const char*)) {
    g_logf = logf;
    if (g_thread) return;
    InitializeCriticalSection(&g_lock);
    g_quit = 0;
    g_thread = CreateThread(nullptr, 0, &captureThread, nullptr, 0, nullptr);
    if (!g_thread) say("[voice] capture thread failed to start");
}
void Capture_Shutdown() {
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread); g_thread = nullptr;
    DeleteCriticalSection(&g_lock);
}
#else
void Capture_Init(void (*)(const char*)) {}
void Capture_Shutdown() {}
int  Capture_Pop(uint8_t*, int, uint16_t*) { return 0; }
void Capture_SetDevice(const char*) {}
int  Capture_DeviceCount() { return 0; }
bool Capture_DeviceAt(int, char*, int, char*, int) { return false; }
#endif

}} // namespace omp::voice
