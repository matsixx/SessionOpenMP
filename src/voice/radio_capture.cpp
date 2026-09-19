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
// radio_capture -- see the header. The shape is voice_capture's (own thread, COM MTA, a queue of encoded
// frames); what differs is WHERE the sound comes from and what it is tuned for:
//  - The device is Windows' virtual "VAD\Process_Loopback", activated asynchronously
//    (ActivateAudioInterfaceAsync, resolved at run time from MMDevAPI.dll) with the target process id and
//    INCLUDE_TARGET_PROCESS_TREE. It has no mix format of its own: we ask for 48 kHz 16-bit stereo and let
//    the engine convert (AUTOCONVERTPCM).
//  - The encoder is Opus AUDIO (music), mono, 48 kbps constrained VBR, every frame capped at 255 bytes
//    (the wire's u8 length). A radio in the world is a point source: stereo would buy nothing it can play.
// Process loopback's structures are declared here rather than taken from audioclientactivationparams.h,
// which hides them behind NTDDI_WIN10_FE; their layout is fixed ABI.
#include "radio_capture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <objidl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <opus.h>

namespace omp { namespace radiocap {

namespace {
// ---- process loopback, as the SDK declares it (audioclientactivationparams.h)
enum { kActivationProcessLoopback = 1 };
enum { kLoopbackIncludeTree = 0 };
struct ProcessLoopbackParams { DWORD targetPid; int mode; };
struct ActivationParams { int type; ProcessLoopbackParams loopback; };
static_assert(sizeof(ActivationParams) == 12, "AUDIOCLIENT_ACTIVATION_PARAMS is 12 bytes");
const wchar_t* const kLoopbackDevice = L"VAD\\Process_Loopback";
// IAudioSessionControl2::GetProcessId's "this session spans several processes" (AUDCLNT_SUCCESS(0x012))
const HRESULT kNoSingleProcess = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_AUDCLNT, 0x012);
typedef HRESULT (WINAPI *ActivateAsyncFn)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                          IActivateAudioInterfaceAsyncOperation**);

void (*g_logf)(const char*) = nullptr;
void say(const char* fmt, ...) {
    if (!g_logf) return;
    char m[300]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof(m), fmt, a); va_end(a);
    g_logf(m);
}

CRITICAL_SECTION g_lock;
bool             g_lockInit = false;
HANDLE           g_thread = nullptr, g_wake = nullptr;
volatile LONG    g_quit = 0, g_wantPid = 0, g_gen = 0, g_state = 0, g_listWanted = 0;
char             g_why[160] = "";
char             g_wantName[48] = "";

struct Frame { uint8_t b[256]; int n; uint16_t seq; };
enum { kQCap = 64 };                                 // 1.28 s: nobody popping just drops the oldest
Frame g_q[kQCap];
int   g_qHead = 0, g_qTail = 0;
uint16_t g_seq = 0;

enum { kMaxSources = 12 };
Source g_src[kMaxSources];
int    g_srcN = 0;
ULONGLONG g_srcAtMs = 0;

void setState(int st, const char* why) {
    EnterCriticalSection(&g_lock);
    g_state = st;
    snprintf(g_why, sizeof(g_why), "%s", why ? why : "");
    LeaveCriticalSection(&g_lock);
}
void push(const uint8_t* d, int n) {
    EnterCriticalSection(&g_lock);
    Frame& f = g_q[g_qHead];
    memcpy(f.b, d, (size_t)n); f.n = n; f.seq = g_seq++;
    g_qHead = (g_qHead + 1) % kQCap;
    if (g_qHead == g_qTail) g_qTail = (g_qTail + 1) % kQCap;
    LeaveCriticalSection(&g_lock);
}

// ---- the activation's completion: a tiny free-threaded COM object
class ActHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG ref_ = 1;
public:
    HANDLE done;
    HRESULT hr = E_PENDING;
    IAudioClient* client = nullptr;
    ActHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    virtual ~ActHandler() { if (client) client->Release(); if (done) CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); AddRef(); return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) { *ppv = static_cast<IAgileObject*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { const LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT act = E_FAIL; IUnknown* unk = nullptr;
        const HRESULT h = op ? op->GetActivateResult(&act, &unk) : E_POINTER;
        if (SUCCEEDED(h) && SUCCEEDED(act) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        else hr = FAILED(h) ? h : act;
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }
};

struct Dev {
    IAudioClient*        ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    HANDLE               evt = nullptr;
    uint32_t             pid = 0;
};
void devClose(Dev& d) {
    if (d.ac) d.ac->Stop();
    if (d.cc) { d.cc->Release(); d.cc = nullptr; }
    if (d.ac) { d.ac->Release(); d.ac = nullptr; }
    if (d.evt) { CloseHandle(d.evt); d.evt = nullptr; }
    d.pid = 0;
}
// false = could not; `why` is for the player
bool devOpen(Dev& d, uint32_t pid, char* why, int whyCap) {
    static ActivateAsyncFn activate = nullptr;
    if (!activate) {
        HMODULE m = LoadLibraryW(L"Mmdevapi.dll");
        activate = m ? (ActivateAsyncFn)GetProcAddress(m, "ActivateAudioInterfaceAsync") : nullptr;
        if (!activate) { snprintf(why, whyCap, "this Windows has no app audio capture"); return false; }
    }
    ActivationParams ap = {};
    ap.type = kActivationProcessLoopback;
    ap.loopback.targetPid = pid; ap.loopback.mode = kLoopbackIncludeTree;
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
    ActHandler* h = new ActHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = activate(kLoopbackDevice, __uuidof(IAudioClient), &pv, h, &op);
    if (SUCCEEDED(hr) && WaitForSingleObject(h->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    else if (SUCCEEDED(hr)) hr = h->hr;
    if (SUCCEEDED(hr) && h->client) { d.ac = h->client; h->client = nullptr; }
    if (op) op->Release();
    h->Release();
    if (FAILED(hr) || !d.ac) {
        say("[radio] app audio capture could not start for pid %u (0x%08lx)", pid, (unsigned long)hr);
        snprintf(why, whyCap, "%s", hr == E_INVALIDARG || hr == E_NOTIMPL || hr == E_NOINTERFACE
                                     ? "streaming needs Windows 11 (or Windows 10 build 20348+)"
                                     : "Windows would not capture that app");
        devClose(d);
        return false;
    }
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = 48000; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = 48000 * 4;
    hr = d.ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                          2000000 /* 200 ms */, 0, &fmt, nullptr);
    if (SUCCEEDED(hr)) { d.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr); hr = d.evt ? d.ac->SetEventHandle(d.evt) : E_FAIL; }
    if (SUCCEEDED(hr)) hr = d.ac->GetService(__uuidof(IAudioCaptureClient), (void**)&d.cc);
    if (SUCCEEDED(hr)) hr = d.ac->Start();
    if (FAILED(hr)) {
        say("[radio] app audio capture: the stream would not open (0x%08lx)", (unsigned long)hr);
        snprintf(why, whyCap, "Windows would not capture that app");
        devClose(d);
        return false;
    }
    d.pid = pid;
    return true;
}

// ---- which apps have sound open
void baseName(uint32_t pid, char* out, int cap) {
    out[0] = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* b = wcsrchr(path, L'\\'); b = b ? b + 1 : path;
        char tmp[96]; int k = 0;
        for (; b[k] && k < 95; k++) tmp[k] = (b[k] < 128 && b[k] >= 32) ? (char)b[k] : '_';
        tmp[k] = 0;
        char* dot = strrchr(tmp, '.'); if (dot && !_stricmp(dot, ".exe")) *dot = 0;
        if (tmp[0] >= 'a' && tmp[0] <= 'z') tmp[0] = (char)(tmp[0] - 'a' + 'A');
        snprintf(out, cap, "%s", tmp);
    }
    CloseHandle(p);
}
void enumerate() {
    Source found[kMaxSources]; int n = 0;
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT nd = 0; col->GetCount(&nd);
        const DWORD self = GetCurrentProcessId();
        for (UINT di = 0; di < nd; di++) {
            IMMDevice* dev = nullptr; IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
            if (FAILED(col->Item(di, &dev))) continue;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                int ns = 0; se->GetCount(&ns);
                for (int si = 0; si < ns; si++) {
                    IAudioSessionControl* ctl = nullptr; IAudioSessionControl2* c2 = nullptr;
                    if (FAILED(se->GetSession(si, &ctl))) continue;
                    if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) {
                        DWORD pid = 0; AudioSessionState st = AudioSessionStateExpired;
                        const bool system = c2->IsSystemSoundsSession() == S_OK;
                        const HRESULT hp = c2->GetProcessId(&pid);
                        ctl->GetState(&st);
                        if (!system && SUCCEEDED(hp) && hp != kNoSingleProcess && pid && pid != self && st != AudioSessionStateExpired) {
                            char nm[48]; baseName(pid, nm, sizeof(nm));
                            bool dup = !nm[0];
                            for (int k = 0; k < n && !dup; k++) {
                                if (!_stricmp(found[k].name, nm)) {           // one entry per app: the playing session wins
                                    dup = true;
                                    if (st == AudioSessionStateActive && !found[k].playing) { found[k].pid = pid; found[k].playing = true; }
                                }
                            }
                            if (!dup && n < kMaxSources) { found[n].pid = pid; snprintf(found[n].name, sizeof(found[n].name), "%s", nm); found[n].playing = st == AudioSessionStateActive; n++; }
                        }
                        c2->Release();
                    }
                    ctl->Release();
                }
            }
            if (se) se->Release();
            if (mgr) mgr->Release();
            dev->Release();
        }
    }
    if (col) col->Release();
    if (en) en->Release();
    for (int i = 1; i < n; i++) {                             // playing first, then by name
        Source k = found[i]; int j = i - 1;
        while (j >= 0 && ((k.playing && !found[j].playing) || (k.playing == found[j].playing && _stricmp(k.name, found[j].name) < 0))) { found[j + 1] = found[j]; j--; }
        found[j + 1] = k;
    }
    EnterCriticalSection(&g_lock);
    memcpy(g_src, found, sizeof(Source) * (size_t)n); g_srcN = n; g_srcAtMs = GetTickCount64();
    LeaveCriticalSection(&g_lock);
}

// ---- the thread
DWORD WINAPI captureThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !enc) { enc = nullptr; say("[radio] Opus encoder failed -- no streaming"); }
    else {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(enc, OPUS_SET_VBR(1));
        opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(enc, OPUS_SET_DTX(0));
    }
    Dev d;
    LONG openGen = -1;
    int16_t frame[960]; int fill = 0;
    while (!g_quit) {
        if (InterlockedExchange(&g_listWanted, 0)) enumerate();
        const uint32_t want = (uint32_t)g_wantPid;
        const LONG gen = g_gen;
        if (!want || !enc) {
            if (d.ac) { devClose(d); say("[radio] app audio capture stopped"); }
            if (g_state != 0 && !want) setState(0, "");
            WaitForSingleObject(g_wake, 250);
            continue;
        }
        if (gen != openGen) {                                // a new request: (re)open, once -- a failure waits for the next
            devClose(d); fill = 0; openGen = gen;
            setState(1, "");
            char why[160] = "";
            char nm[48]; EnterCriticalSection(&g_lock); snprintf(nm, sizeof(nm), "%s", g_wantName); LeaveCriticalSection(&g_lock);
            if (devOpen(d, want, why, sizeof(why))) { setState(2, ""); say("[radio] capturing %s (pid %u): 48 kHz mono, Opus music 48 kbps", nm[0] ? nm : "an app", want); }
            else setState(-1, why);
        }
        if (!d.ac) { WaitForSingleObject(g_wake, 250); continue; }
        WaitForSingleObject(d.evt, 100);
        for (int guard = 0; guard < 64; guard++) {
            UINT32 next = 0;
            HRESULT hr = d.cc->GetNextPacketSize(&next);
            if (FAILED(hr)) { say("[radio] app audio capture lost (0x%08lx) -- the app closed?", (unsigned long)hr); devClose(d); setState(-1, "the app stopped playing (closed?)"); break; }
            if (!next) break;
            BYTE* buf = nullptr; UINT32 nf = 0; DWORD flags = 0;
            hr = d.cc->GetBuffer(&buf, &nf, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const int16_t* s = (const int16_t*)buf;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !buf;
            for (UINT32 i = 0; i < nf; i++) {
                frame[fill++] = silent ? 0 : (int16_t)(((int)s[i * 2] + (int)s[i * 2 + 1]) / 2);
                if (fill == 960) {
                    uint8_t out[256];
                    const int n = opus_encode(enc, frame, 960, out, 255);
                    if (n > 0) push(out, n);
                    fill = 0;
                }
            }
            d.cc->ReleaseBuffer(nf);
        }
    }
    devClose(d);
    if (enc) opus_encoder_destroy(enc);
    CoUninitialize();
    return 0;
}
void ensureThread() {
    if (g_thread) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, &captureThread, nullptr, 0, nullptr);
}
} // namespace

void Init(void (*logf)(const char*)) {
    g_logf = logf;
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
}
void Shutdown() {
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    if (g_wake) SetEvent(g_wake);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread); g_thread = nullptr;
}
void Start(uint32_t pid, const char* name) {
    if (!g_lockInit) return;
    ensureThread();
    EnterCriticalSection(&g_lock);
    snprintf(g_wantName, sizeof(g_wantName), "%s", name ? name : "");
    g_qHead = g_qTail = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_wantPid, (LONG)pid);
    InterlockedIncrement(&g_gen);
    if (g_wake) SetEvent(g_wake);
}
void Stop() { InterlockedExchange(&g_wantPid, 0); if (g_wake) SetEvent(g_wake); }
int State(char* why, int cap) {
    if (!g_lockInit) { if (why && cap) why[0] = 0; return 0; }
    EnterCriticalSection(&g_lock);
    const int st = (int)g_state;
    if (why && cap) snprintf(why, cap, "%s", g_why);
    LeaveCriticalSection(&g_lock);
    return st;
}
int Pop(uint8_t* out, int cap, uint16_t* seqOut) {
    if (!g_lockInit || !out) return 0;
    int n = 0;
    EnterCriticalSection(&g_lock);
    if (g_qTail != g_qHead) {
        const Frame& f = g_q[g_qTail];
        if (f.n <= cap) { memcpy(out, f.b, (size_t)f.n); n = f.n; if (seqOut) *seqOut = f.seq; }
        g_qTail = (g_qTail + 1) % kQCap;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}
void RequestSources() {
    if (!g_lockInit) return;
    ensureThread();
    if (GetTickCount64() - g_srcAtMs < 2000 && g_srcAtMs) return;
    InterlockedExchange(&g_listWanted, 1);
    if (g_wake) SetEvent(g_wake);
}
int Sources(Source* out, int cap) {
    if (!g_lockInit || !out || cap <= 0) return 0;
    EnterCriticalSection(&g_lock);
    const int n = g_srcN < cap ? g_srcN : cap;
    memcpy(out, g_src, sizeof(Source) * (size_t)n);
    LeaveCriticalSection(&g_lock);
    return n;
}

}} // namespace omp::radiocap
