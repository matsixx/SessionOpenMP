// omp_radiocaptest -- does the radio's app capture (src/voice/radio_capture) really capture ONE app?
//
// A second copy of this program is started with --play: it renders a quiet 440 Hz tone to the speakers
// for a few seconds. This copy then captures THAT process through the real capture module -- Windows'
// process loopback, the same thread, the same Opus encoder -- decodes what comes out, and measures it:
// the tone must be there (440 Hz standing well above its neighbours), and the app must have been listed
// among the ones with sound open.
//
// It makes a faint beep for about three seconds. Needs Windows 11 / Windows 10 build 20348+ for the capture.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <opus.h>
#include "voice/radio_capture.h"

static int g_fail = 0;
static void Check(bool ok, const char* what, const char* detail = "") {
    printf("  %s  %s %s\n", ok ? "ok  " : "FAIL", what, detail);
    if (!ok) g_fail++;
}
static void logf_(const char* m) { printf("    log: %s\n", m); }

// ---- the child: a quiet sine on the default speakers
static int Play(double seconds) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || FAILED(ac->GetMixFormat(&mix))) return 2;
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr)) || FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) return 3;
    const bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
    UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
    ac->Start();
    const double rate = mix->nSamplesPerSec; double ph = 0.0;
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
    while (GetTickCount64() < end) {
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        const UINT32 room = bufFrames - pad;
        if (room > 0) {
            BYTE* b = nullptr;
            if (SUCCEEDED(rc->GetBuffer(room, &b))) {
                for (UINT32 i = 0; i < room; i++) {
                    const float v = 0.03f * (float)sin(ph); ph += 2.0 * 3.14159265358979 * 440.0 / rate;
                    for (int c = 0; c < mix->nChannels; c++) {
                        if (isFloat) ((float*)b)[i * mix->nChannels + c] = v;
                        else ((int16_t*)b)[i * mix->nChannels + c] = (int16_t)(v * 32767.0f);
                    }
                }
                rc->ReleaseBuffer(room, 0);
            }
        }
        Sleep(10);
    }
    ac->Stop();
    return 0;
}
static double Goertzel(const std::vector<float>& x, double f, double rate) {
    const double w = 2.0 * 3.14159265358979 * f / rate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) { const double s = v + c * s1 - s2; s2 = s1; s1 = s; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)(x.size() * x.size());
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && !wcscmp(argv[1], L"--play")) return Play(argc >= 3 ? _wtof(argv[2]) : 3.0);

    // start the player: this same program
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 32]; swprintf(cmd, MAX_PATH + 32, L"\"%s\" --play 3.5", self);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { printf("could not start the player\n"); return 2; }
    printf("the player: pid %lu, a quiet 440 Hz tone for 3.5 s\n", pi.dwProcessId);
    Sleep(400);

    omp::radiocap::Init(&logf_);
    // it is listed among the apps with sound open
    omp::radiocap::RequestSources();
    Sleep(600);
    omp::radiocap::Source src[12];
    const int ns = omp::radiocap::Sources(src, 12);
    bool listed = false; char names[400] = ""; int at = 0;
    for (int i = 0; i < ns; i++) {
        if (src[i].pid == pi.dwProcessId) listed = true;
        at += snprintf(names + at, sizeof(names) - (size_t)at, "%s%s(%u%s)", i ? ", " : "", src[i].name, src[i].pid, src[i].playing ? " playing" : "");
        if (at > 360) break;
    }
    Check(listed, "the playing app is listed among the ones with sound open", names);

    // capture it
    omp::radiocap::Start(pi.dwProcessId, "the test tone");
    std::vector<float> pcm;
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    int frames = 0, bytes = 0, maxFrame = 0;
    const ULONGLONG until = GetTickCount64() + 2200;
    while (GetTickCount64() < until) {
        uint8_t f[256]; uint16_t seq = 0;
        int n;
        while ((n = omp::radiocap::Pop(f, sizeof(f), &seq)) > 0) {
            frames++; bytes += n; if (n > maxFrame) maxFrame = n;
            int16_t out[960];
            const int got = opus_decode(dec, f, n, out, 960, 0);
            for (int i = 0; i < got; i++) pcm.push_back(out[i] / 32768.0f);
        }
        Sleep(20);
    }
    char why[160]; const int st = omp::radiocap::State(why, sizeof(why));
    char b[200]; snprintf(b, sizeof(b), "(state %d%s%s)", st, why[0] ? ": " : "", why);
    Check(st == 2, "the capture opened on that app alone", b);
    snprintf(b, sizeof(b), "(%d frames in 2.2 s, %d B each on average, the largest %d)", frames, frames ? bytes / frames : 0, maxFrame);
    Check(frames > 60, "Opus frames come out, 20 ms each", b);
    Check(maxFrame <= 255, "no frame is bigger than the wire's u8 length", b);
    if (pcm.size() > 48000 / 2) {
        std::vector<float> tail(pcm.end() - 48000 / 2, pcm.end());      // the last half second: settled
        double rms = 0; for (float v : tail) rms += v * v; rms = sqrt(rms / tail.size());
        const double p440 = Goertzel(tail, 440, 48000), p300 = Goertzel(tail, 300, 48000), p620 = Goertzel(tail, 620, 48000);
        snprintf(b, sizeof(b), "(rms %.4f; power at 440 Hz %.2e, at 300 %.2e, at 620 %.2e)", rms, p440, p300, p620);
        Check(rms > 0.005, "what was captured is not silence", b);
        Check(p440 > 20.0 * p300 && p440 > 20.0 * p620, "...it is the TONE: 440 Hz stands far above its neighbours", b);
    } else Check(false, "enough decoded audio to measure", "");
    omp::radiocap::Stop();
    Sleep(300);
    Check(omp::radiocap::State(nullptr, 0) == 0, "stopping stops it");
    omp::radiocap::Shutdown();
    if (dec) opus_decoder_destroy(dec);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    printf(g_fail ? "RADIO CAPTURE FAIL (%d)\n" : "RADIO CAPTURE PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
