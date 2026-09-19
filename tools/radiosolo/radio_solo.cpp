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
// omp_radiosolo -- A MEASUREMENT, not a gate. One question about Windows, answered empirically.
//
// Streaming an app through the in-game radio captures that app with process loopback -- and the app
// goes on playing out of the speakers at the same time. So the person streaming hears their music
// twice: once raw from their PC, once positionally from the radio. Everyone else hears only the radio,
// which is the point. Field, 2026-09-19: "being able to play your own music is great but it also means
// playing the music locally on my PC".
//
// The cheap cure would be to SILENCE THE SOURCE LOCALLY while streaming it -- but only if the capture
// survives that. Process loopback might tap the app's stream BEFORE the session volume is applied (we
// keep the audio, the speakers go quiet: exactly what is wanted) or AFTER it (we capture silence and
// the idea is dead). The docs do not say, so this measures it, on this machine, with a known tone:
//
//   baseline           -- capture the tone with the app at normal volume
//   session MUTED      -- ISimpleAudioVolume::SetMute(TRUE) on the app's session, then capture
//   session VOLUME 0   -- SetMasterVolume(0), then capture
//
// Each phase prints the captured RMS and the power at 440 Hz. If the muted phases still show the tone,
// the fix is a few lines in radio.cpp. If they show silence, the answer is a different mechanism
// (route the app to a virtual output device, or have the mod play the file itself).
// =====================================================================================================
#include "../../src/voice/radio_capture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <opus.h>
#include <cstdio>
#include <cmath>
#include <vector>

#pragma comment(lib, "ole32.lib")

// ---- the player: a quiet 440 Hz tone, so there is something to look for. `endpoint` < 0 = the default
// one; otherwise that index into the active render endpoints -- which is how the "send the app to
// another output device" idea gets tested without installing a virtual cable first.
static int Play(double seconds, int endpoint = -1) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr; WAVEFORMATEX* wf = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 2;
    if (endpoint >= 0) {
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) || !col) return 2;
        if (FAILED(col->Item((UINT)endpoint, &dev))) { col->Release(); return 2; }
        col->Release();
    }
    else if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return 2;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return 2;
    ac->GetMixFormat(&wf);
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wf, nullptr))) return 2;
    UINT32 total = 0; ac->GetBufferSize(&total);
    if (FAILED(ac->GetService(IID_PPV_ARGS(&rc)))) return 2;
    ac->Start();
    const int ch = wf->nChannels; const double rate = wf->nSamplesPerSec;
    const bool isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                         (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    double ph = 0; const double step = 2.0 * 3.14159265358979 * 440.0 / rate;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
    while (GetTickCount64() < until) {
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        UINT32 want = total - pad;
        if (want) {
            BYTE* p = nullptr;
            if (SUCCEEDED(rc->GetBuffer(want, &p))) {
                for (UINT32 i = 0; i < want; i++) {
                    const float v = (float)(0.20 * sin(ph)); ph += step;
                    for (int c = 0; c < ch; c++) {
                        if (isFloat) ((float*)p)[i * ch + c] = v;
                        else ((int16_t*)p)[i * ch + c] = (int16_t)(v * 32767.0f);
                    }
                }
                rc->ReleaseBuffer(want, 0);
            }
        }
        Sleep(10);
    }
    ac->Stop();
    if (wf) CoTaskMemFree(wf);
    if (rc) rc->Release(); if (ac) ac->Release(); if (dev) dev->Release(); if (en) en->Release();
    CoUninitialize();
    return 0;
}

static void logf_(const char*) {}

static double Goertzel(const std::vector<float>& x, double f, double rate) {
    const double w = 2.0 * 3.14159265358979 * f / rate, c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) { const double s = v + c * s1 - s2; s2 = s1; s1 = s; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)(x.size() * x.size());
}

// The app's own volume control, found by process id among the default endpoint's sessions.
static ISimpleAudioVolume* SessionVolumeOf(DWORD pid) {
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
    ISimpleAudioVolume* found = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
        int n = 0; se->GetCount(&n);
        for (int i = 0; i < n && !found; i++) {
            IAudioSessionControl* c = nullptr;
            if (FAILED(se->GetSession(i, &c)) || !c) continue;
            IAudioSessionControl2* c2 = nullptr;
            if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                DWORD spid = 0;
                if (SUCCEEDED(c2->GetProcessId(&spid)) && spid == pid)
                    c2->QueryInterface(IID_PPV_ARGS(&found));
                c2->Release();
            }
            c->Release();
        }
    }
    if (se) se->Release(); if (mgr) mgr->Release(); if (dev) dev->Release(); if (en) en->Release();
    return found;
}

// Capture for `ms` and report what came through.
struct Result { int frames = 0; double rms = 0; double p440 = 0; };
static Result CaptureFor(int ms, OpusDecoder* dec) {
    Result r;
    std::vector<float> pcm;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)ms;
    while (GetTickCount64() < until) {
        uint8_t f[256]; uint16_t seq = 0; int n;
        while ((n = omp::radiocap::Pop(f, sizeof(f), &seq)) > 0) {
            r.frames++;
            int16_t out[960];
            const int got = opus_decode(dec, f, n, out, 960, 0);
            for (int i = 0; i < got; i++) pcm.push_back(out[i] / 32768.0f);
        }
        Sleep(20);
    }
    if (pcm.size() > 48000 / 2) {
        std::vector<float> tail(pcm.end() - 48000 / 2, pcm.end());
        for (float v : tail) r.rms += v * v;
        r.rms = sqrt(r.rms / tail.size());
        r.p440 = Goertzel(tail, 440, 48000);
    }
    return r;
}

// Name the active render endpoints, and say which is the default.
static int ListEndpoints(wchar_t names[8][128], int* defaultIdx) {
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr; IMMDevice* def = nullptr;
    int n = 0; *defaultIdx = -1;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 0;
    LPWSTR defId = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def) def->GetId(&defId);
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT cnt = 0; col->GetCount(&cnt);
        for (UINT i = 0; i < cnt && n < 8; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            IPropertyStore* ps = nullptr; PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
                    wcsncpy_s(names[n], 128, pv.pwszVal, _TRUNCATE);
                PropVariantClear(&pv); ps->Release();
            }
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                if (defId && !wcscmp(id, defId)) *defaultIdx = n;
                CoTaskMemFree(id);
            }
            d->Release(); n++;
        }
        col->Release();
    }
    if (defId) CoTaskMemFree(defId);
    if (def) def->Release(); if (en) en->Release();
    return n;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && !wcscmp(argv[1], L"--play"))
        return Play(argc >= 3 ? _wtof(argv[2]) : 12.0, argc >= 4 ? _wtoi(argv[3]) : -1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    printf("omp_radiosolo -- can the source app be silenced LOCALLY and still be captured?\n\n");

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 32]; swprintf(cmd, MAX_PATH + 32, L"\"%s\" --play 14", self);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        printf("could not start the tone player\n"); return 2;
    }
    printf("tone player: pid %lu, a 440 Hz tone for 14 s\n", pi.dwProcessId);
    Sleep(600);

    omp::radiocap::Init(&logf_);
    omp::radiocap::Start(pi.dwProcessId, "the test tone");
    int err = 0; OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    Sleep(400);

    const Result base = CaptureFor(2600, dec);
    printf("  baseline (app at normal volume) : frames %3d  rms %.4f  440 Hz %.2e\n", base.frames, base.rms, base.p440);

    ISimpleAudioVolume* vol = SessionVolumeOf(pi.dwProcessId);
    if (!vol) {
        printf("\n  could not find the app's audio session -- cannot answer. (Is it still playing?)\n");
    } else {
        vol->SetMute(TRUE, nullptr);
        Sleep(500);
        const Result muted = CaptureFor(2600, dec);
        printf("  session MUTED (SetMute TRUE)    : frames %3d  rms %.4f  440 Hz %.2e\n", muted.frames, muted.rms, muted.p440);
        vol->SetMute(FALSE, nullptr);

        Sleep(400);
        vol->SetMasterVolume(0.0f, nullptr);
        Sleep(500);
        const Result quiet = CaptureFor(2600, dec);
        printf("  session VOLUME 0                : frames %3d  rms %.4f  440 Hz %.2e\n", quiet.frames, quiet.rms, quiet.p440);
        vol->SetMasterVolume(1.0f, nullptr);
        vol->Release();

        printf("\nVERDICT\n");
        const bool baseOk = base.rms > 0.005;
        if (!baseOk) printf("  INCONCLUSIVE: the baseline captured no tone, so nothing can be concluded from the rest.\n");
        else {
            const double keptMute  = muted.rms / base.rms;
            const double keptQuiet = quiet.rms / base.rms;
            printf("  muting the app keeps %.0f%% of the captured level; volume 0 keeps %.0f%%.\n",
                   keptMute * 100.0, keptQuiet * 100.0);
            if (keptMute > 0.5)
                printf("  => process loopback taps the app BEFORE its session volume. The source CAN be silenced\n"
                       "     locally while it streams: mute the app's session for as long as it is the radio source.\n");
            else if (keptQuiet > 0.5)
                printf("  => mute is applied to the capture but VOLUME is not (or vice versa) -- use the one that survives.\n");
            else
                printf("  => process loopback taps the app AFTER its session volume: silencing it locally captures\n"
                       "     silence. This approach is DEAD -- the source has to be routed elsewhere (a virtual output\n"
                       "     device) or the mod has to play the audio itself instead of capturing it.\n");
        }
    }

    omp::radiocap::Stop();
    Sleep(300);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    // ---- THE OTHER IDEA: leave the app at full volume but send it to an output device nobody is
    // listening to (Windows can do this per app: Settings > Sound > Volume mixer > the app > Output).
    // Process loopback is scoped to a PROCESS, not to an endpoint, so it ought to capture anyway --
    // ought to. If it does, the cure needs no virtual-cable driver at all on a machine that already
    // has a spare output, and needs a free one (VB-Cable) on a machine that does not.
    wchar_t names[8][128] = {}; int defIdx = -1;
    const int nEp = ListEndpoints(names, &defIdx);
    printf("\nOUTPUT DEVICES on this machine (%d active)\n", nEp);
    for (int i = 0; i < nEp; i++) printf("  [%d] %ls%s\n", i, names[i], i == defIdx ? "   <- default" : "");
    int other = -1;
    for (int i = 0; i < nEp; i++) if (i != defIdx) { other = i; break; }
    if (other < 0) {
        printf("\n  Only one output device here, so this machine cannot test it. On a machine with a\n"
               "  second output (or a virtual cable), the same run would answer it.\n");
    } else {
        printf("\nPHASE 2: the same tone, rendered to [%d] %ls instead of the default.\n", other, names[other]);
        wchar_t cmd2[MAX_PATH + 48];
        swprintf(cmd2, MAX_PATH + 48, L"\"%s\" --play 8 %d", self, other);
        STARTUPINFOW si2 = { sizeof(si2) }; PROCESS_INFORMATION pi2 = {};
        if (!CreateProcessW(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2)) {
            printf("  could not start the second player\n");
        } else {
            Sleep(900);
            omp::radiocap::Start(pi2.dwProcessId, "the test tone, other endpoint");
            Sleep(500);
            const Result off = CaptureFor(2600, dec);
            char why2[160]; const int st2 = omp::radiocap::State(why2, sizeof(why2));
            printf("  app on another output device    : frames %3d  rms %.4f  440 Hz %.2e  (state %d%s%s)\n",
                   off.frames, off.rms, off.p440, st2, why2[0] ? ": " : "", why2);
            if (off.rms > 0.005 && base.rms > 0.005)
                printf("\n  => CAPTURE SURVIVES a different output device (%.0f%% of the baseline level).\n"
                       "     So: point the music app at an output nobody hears, stream it, and it only exists\n"
                       "     inside the game. No code needed in the mod -- it is a Windows setting per app.\n",
                       100.0 * off.rms / base.rms);
            else
                printf("\n  => capture does NOT survive it either. Then the only clean answer left is for the mod\n"
                       "     to play the audio itself (a local file, or an internet-radio URL) instead of\n"
                       "     capturing an app at all.\n");
            omp::radiocap::Stop();
            TerminateProcess(pi2.hProcess, 0);
            WaitForSingleObject(pi2.hProcess, 3000);
            CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread);
        }
    }

    omp::radiocap::Shutdown();
    if (dec) opus_decoder_destroy(dec);
    CoUninitialize();
    return 0;
}
