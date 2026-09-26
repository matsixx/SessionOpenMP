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
//
// The controller, sampled on its own clock. See pad_sampler.h.
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "pad_sampler.h"
#include "pop_probe.h"
#include "ui/menu_ext.h"
#include <windows.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ settings
static int g_hz = 1000;          // PadSampleHz -- reads per second; 0 = off (per-frame measurement)

void PadSampler_ReadConfig(const char* buf) {
    g_hz = TwkIniInt(buf, "PadSampleHz", 1000);
    if (g_hz < 0) g_hz = 0;
    if (g_hz > 0 && g_hz < 125) g_hz = 125;
    if (g_hz > 2000) g_hz = 2000;
}
void PadSampler_SaveConfig(char* buf, size_t cap) { TwkIniSetInt(buf, cap, "PadSampleHz", g_hz); }
void PadSampler_ResetDefaults() { g_hz = 1000; }
bool PadSampler_Enabled() { return g_hz > 0; }
void PadSampler_SetEnabled(bool on) { g_hz = on ? 1000 : 0; TwkMarkDirty(); }

#ifdef PAD_SAMPLER_TEST
// tools/padtest drives the measurements with synthetic reports and its own clock.
static double g_testNow = -1.0;
static bool   g_testLive = false;
#endif
static double QpcSec() {
#ifdef PAD_SAMPLER_TEST
    if (g_testNow >= 0.0) return g_testNow;
#endif
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
double PadSampler_Now() { return QpcSec(); }

// ------------------------------------------------------------------ the sample ring
// One writer (the sampler thread), readers on the game thread. The writer fills a slot and then
// publishes the count; a reader only touches slots below the published count and never within
// kSafety of the slot the writer will reuse next, so nothing it copies can be half-written.
struct Sample { double t; float lx, ly, rx, ry; };
static const uint64_t kRing = 8192, kMask = kRing - 1, kSafety = 512;
static Sample                g_ring[kRing];
static std::atomic<uint64_t> g_w{0};
static void Push(const Sample& s) {
    const uint64_t w = g_w.load(std::memory_order_relaxed);
    g_ring[w & kMask] = s;
    g_w.store(w + 1, std::memory_order_release);
}
// Copies the samples from the one HELD at tFrom (the last report at or before it) to the newest,
// oldest first. Returns how many.
static int Snapshot(double tFrom, Sample* out, int cap) {
    const uint64_t w = g_w.load(std::memory_order_acquire);
    if (!w) return 0;
    const uint64_t lo = (w > kRing - kSafety) ? w - (kRing - kSafety) : 0;
    uint64_t i = w;
    while (i > lo && g_ring[(i - 1) & kMask].t >= tFrom) --i;
    if (i > lo) --i;
    if (w - i > (uint64_t)cap) i = w - (uint64_t)cap;
    int n = 0;
    for (; i < w; ++i) out[n++] = g_ring[i & kMask];
    return n;
}

// ------------------------------------------------------------------ the thread
static std::atomic<bool>   g_run{false};
static HANDLE              g_thread = nullptr;
static std::atomic<double> g_lastOk{0.0};          // when a connected pad last answered
// Published once a second for the status line and the flick measure.
static std::atomic<int>    g_statPolls{0}, g_statReports{0}, g_statReadUs{0}, g_statWorstGapUs{0};
static std::atomic<int>    g_statUser{-1}, g_statHiRes{0};
// The controller's own report interval while the stick is MOVING, in microseconds (an average of
// the gaps between consecutive changed reports under 20 ms). A resting stick sends nothing, so the
// sample before a flick can be arbitrarily old -- this is what bounds how far back a crossing may
// be interpolated. 4000 until measured.
static std::atomic<int>    g_reportUs{4000};

static float NormStick(short v) { return v >= 0 ? (float)v / 32767.0f : (float)v / 32768.0f; }

static DWORD WINAPI SamplerMain(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // A high-resolution waitable timer ticks at the requested period without changing the system
    // timer resolution for the whole machine (Windows 10 1803+). Older systems get a normal timer,
    // which the status line says; the measurements still use the real timestamps either way.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    g_statHiRes = timer ? 1 : 0;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    unsigned long lastPacket = 0;
    bool   havePacket = false;
    double lastPush = 0.0, lastProbe = 0.0, lastLoop = 0.0, lastChange = 0.0;
    // An XInput read of a DISCONNECTED slot can be slow on some systems (it goes looking for the
    // device), so a failed read backs off for a second instead of being retried 1000 times a second.
    double retryAt = 0.0;
    double secStart = QpcSec(), readSum = 0.0, repAvgUs = 4000.0;
    int    polls = 0, reports = 0, worstGapUs = 0, readN = 0;
    long   user = -1;
    while (g_run.load(std::memory_order_relaxed)) {
        const int hz = g_hz;
        if (hz <= 0) { Sleep(50); havePacket = false; lastLoop = 0.0; continue; }
        LARGE_INTEGER due; due.QuadPart = -(10000000LL / hz);              // relative, 100 ns units
        if (!(timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
              WaitForSingleObject(timer, 100) == WAIT_OBJECT_0))
            Sleep(1);
        const double t0 = QpcSec();
        if (lastLoop > 0.0) {
            const int gap = (int)((t0 - lastLoop) * 1e6);
            if (gap > worstGapUs) worstGapUs = gap;
        }
        lastLoop = t0;

        // WHICH PAD: the one the game polls. Until the game has polled one, the first connected
        // index, looked for once a second.
        long u = PopProbe_PadUser();
        if (u < 0 || u > 3) u = user;
        if ((u < 0 || u > 3) && t0 - lastProbe > 1.0) {
            lastProbe = t0;
            for (long k = 0; k < 4; ++k) {
                unsigned long pk = 0; short sv[4];
                if (PopProbe_ReadPadUnhooked((unsigned long)k, &pk, sv)) { u = k; break; }
            }
        }
        double t1 = t0;
        if (u >= 0 && u <= 3 && t0 >= retryAt) {
            unsigned long pkt = 0; short s[4] = {};
            const bool ok = PopProbe_ReadPadUnhooked((unsigned long)u, &pkt, s);
            t1 = QpcSec();
            ++polls; readSum += t1 - t0; ++readN;
            if (!ok) {
                if (user == u) user = -1;
                havePacket = false;
                retryAt = t1 + 1.0;
            } else {
                user = u;
                g_lastOk.store(t1, std::memory_order_relaxed);
                const bool changed = !havePacket || pkt != lastPacket;
                // Every change, plus a keep-alive every 20 ms so a still stick is still a sample.
                if (changed || t1 - lastPush >= 0.020) {
                    Sample smp;
                    smp.t  = 0.5 * (t0 + t1);                                  // mid-read: +-half a read
                    smp.lx = NormStick(s[0]); smp.ly = NormStick(s[1]);
                    smp.rx = NormStick(s[2]); smp.ry = NormStick(s[3]);
                    Push(smp);
                    lastPush = t1;
                }
                if (changed && havePacket) {
                    ++reports;
                    const double dt = t1 - lastChange;
                    if (dt > 0.0003 && dt < 0.020) {                          // a moving stick's cadence
                        repAvgUs += (dt * 1e6 - repAvgUs) * 0.05;
                        g_reportUs.store((int)repAvgUs, std::memory_order_relaxed);
                    }
                }
                if (changed) lastChange = t1;
                lastPacket = pkt; havePacket = true;
            }
        }
        if (t1 - secStart >= 1.0) {
            g_statPolls = polls; g_statReports = reports;
            g_statReadUs = readN ? (int)(readSum / readN * 1e6) : 0;
            g_statWorstGapUs = worstGapUs; g_statUser = (int)user;
            polls = reports = worstGapUs = readN = 0; readSum = 0.0; secStart = t1;
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}

void PadSampler_Start() {
    if (g_thread) return;
    g_run = true;
    g_thread = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
    TwkLog("[pad] controller sampler %s (%d reads/s requested%s)", g_thread ? "started" : "FAILED to start",
           g_hz, g_hz ? "" : " -- off: flick timing follows the frame rate");
}
void PadSampler_Stop() {
    if (!g_thread) return;
    g_run = false;
    WaitForSingleObject(g_thread, 250);      // bounded: this can run under the loader lock at exit
    CloseHandle(g_thread);
    g_thread = nullptr;
}
bool PadSampler_Live() {
#ifdef PAD_SAMPLER_TEST
    if (g_testLive) return true;
#endif
    if (g_hz <= 0 || !g_thread) return false;
    const double t = g_lastOk.load(std::memory_order_relaxed);
    return t > 0.0 && QpcSec() - t <= 0.100;
}

// ------------------------------------------------------------------ the flick
static const float kRestR     = 0.04f;    // a resting thumb stays within this of one point...
static const float kRestSec   = 0.015f;   // ...for this long
static const float kMinTravel = 0.15f;    // less than this is not a flick

static Sample g_snap[4096];               // GAME THREAD only (the flip and scoop hooks)
static float  g_px[4096], g_py[4096];

static inline float Dist(int a, int b) {
    const float dx = g_px[a] - g_px[b], dy = g_py[a] - g_py[b];
    return sqrtf(dx * dx + dy * dy);
}
// Did the stick rest at sample i: within kRestR of it for the kRestSec before it? Between reports
// the stick holds its last reported value, so the sample at or before (t_i - kRestSec) counts too.
static bool StillAt(int i) {
    const double from = g_snap[i].t - kRestSec;
    int j = i;
    while (j > 0 && g_snap[j].t > from) {
        if (Dist(j, i) > kRestR) return false;
        --j;
    }
    return Dist(j, i) <= kRestR;
}
// When the distance from the rest point first reached `thr`, interpolated between the report that
// crossed it and the one before -- but never from further back than one report interval: a resting
// stick reports nothing, so the previous sample can be a keep-alive from long before the thumb moved.
static bool Crossing(int anchor, int last, float thr, double repSec, double* t) {
    for (int j = anchor + 1; j <= last; ++j) {
        const float dj = Dist(j, anchor);
        if (dj < thr) continue;
        const float dp = Dist(j - 1, anchor);
        float f = (dj > dp) ? (thr - dp) / (dj - dp) : 1.0f;
        if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
        double base = g_snap[j - 1].t;
        if (g_snap[j].t - base > repSec) base = g_snap[j].t - repSec;
        *t = base + (g_snap[j].t - base) * (double)f;
        return true;
    }
    return false;
}

bool PadSampler_Flick(bool right, float windowSec, PadFlick* out) {
    if (!out) return false;
    *out = PadFlick{};
    if (!PadSampler_Live()) return false;
    const double now = QpcSec();
    const int n = Snapshot(now - (double)windowSec, g_snap, 4096);
    if (n < 3) return false;
    for (int i = 0; i < n; ++i) {
        g_px[i] = right ? g_snap[i].rx : g_snap[i].lx;
        g_py[i] = right ? g_snap[i].ry : g_snap[i].ly;
    }
    // THE REST THE FLICK LEFT FROM: the most recent point the stick sat still with a real movement
    // after it. Walking back from the newest sample skips the stick held at the flick's far end (still,
    // but nothing after it) and lands on where the thumb started -- the centre, or wherever it was
    // being held (a crouch), which is the same thing to a flick.
    // A movement that heads BACK TOWARD the centre is the thumb letting go -- a flick released before
    // the trick was asked for leaves exactly that behind it, and measured it reads as the spring's
    // return. It is skipped and the search goes on back to the flick itself, which moves outward (from
    // the centre, or from a held crouch through the centre to the other side). A flick still on its
    // way out when asked is outward too, however little of it has happened yet.
    int anchor = -1, peak = -1;
    float D = 0.0f;
    for (int i = n - 2; i >= 0; --i) {
        if (!StillAt(i)) continue;
        float d = 0.0f; int p = i;
        for (int j = i + 1; j < n; ++j) { const float dj = Dist(j, i); if (dj > d) { d = dj; p = j; } }
        if (d < kMinTravel) continue;
        const float magPeak = sqrtf(g_px[p] * g_px[p] + g_py[p] * g_py[p]);
        const float magRest = sqrtf(g_px[i] * g_px[i] + g_py[i] * g_py[i]);
        if (magPeak < magRest - 0.25f) continue;
        anchor = i; peak = p; D = d;
        break;
    }
    if (anchor < 0) return false;
    const double repSec = (double)g_reportUs.load(std::memory_order_relaxed) * 1e-6;
    double t10 = 0.0, t90 = 0.0;
    if (!Crossing(anchor, peak, 0.1f * D, repSec, &t10) || !Crossing(anchor, peak, 0.9f * D, repSec, &t90))
        return false;
    double rise = t90 - t10;
    if (rise < 0.0005) rise = 0.0005;                  // below the read's own precision
    int inRise = 0;
    for (int j = anchor + 1; j <= peak; ++j) if (g_snap[j].t >= t10 && g_snap[j].t <= t90 + 1e-6) ++inRise;
    out->speed      = (float)(0.8 * (double)D / rise);
    out->dist       = D;
    out->riseMs     = (float)(rise * 1000.0);
    out->onsetAgoMs = (float)((now - t10) * 1000.0);
    out->samples    = inRise;
    return true;
}

// ------------------------------------------------------------------ the scoop
bool PadSampler_Sweep(bool right, float minMag, float freshSec, float sustainFrac, PadSweep* out) {
    if (!out) return false;
    *out = PadSweep{};
    if (!PadSampler_Live()) return false;
    const double now = QpcSec();
    const int n = Snapshot(now - 1.5, g_snap, 4096);
    if (n < 2) return false;
    auto magAt = [&](int i) {
        const float x = right ? g_snap[i].rx : g_snap[i].lx, y = right ? g_snap[i].ry : g_snap[i].ly;
        return sqrtf(x * x + y * y);
    };
    // The current gesture: the newest run of samples out past minMag, and it must still be fresh --
    // ended (stick back in) no more than freshSec ago, as scoop_speed's own tracker requires.
    int e = -1;
    for (int i = n - 1; i >= 0; --i) if (magAt(i) >= minMag) { e = i; break; }
    if (e < 0) return false;
    const double endedAt = (e + 1 < n) ? g_snap[e + 1].t : now;
    if (now - endedAt > (double)freshSec) return false;
    int s = e;
    while (s > 0 && magAt(s - 1) >= minMag) --s;
    // Unwrapped angle, thinned to >= 2 ms steps (the controller rarely reports faster, and it bounds
    // the pair search).
    static double T[2048]; static float C[2048];
    int m = 0; float cum = 0.0f, lastAng = 0.0f; double lastT = -1.0;
    for (int i = s; i <= e && m < 2048; ++i) {
        const float x = right ? g_snap[i].rx : g_snap[i].lx, y = right ? g_snap[i].ry : g_snap[i].ly;
        const float ang = atan2f(y, x) * 57.2957795f;
        if (lastT >= 0.0) {
            float dg = ang - lastAng;
            while (dg >  180.0f) dg -= 360.0f;
            while (dg < -180.0f) dg += 360.0f;
            cum += dg;
        }
        lastAng = ang;
        if (m > 0 && g_snap[i].t - T[m - 1] < 0.002) { C[m - 1] = cum; T[m - 1] = g_snap[i].t; }
        else { T[m] = g_snap[i].t; C[m] = cum; ++m; }
        lastT = g_snap[i].t;
    }
    if (m < 2) return false;
    double lo = 0.05, hi = 0.12;
    if (sustainFrac > 0.0f) {
        const double dur = T[m - 1] - T[0];
        if (dur > 0.0) {
            lo = dur * (double)sustainFrac;
            if (lo < 0.05) lo = 0.05; else if (lo > 0.20) lo = 0.20;
            hi = lo * 2.0; if (hi > 0.35) hi = 0.35;
        }
    }
    float best = 0.0f, peak = 0.0f;
    int i0 = 0;
    for (int j = 1; j < m; ++j) {
        while (i0 < j && T[j] - T[i0] > hi) ++i0;
        for (int i = i0; i < j && T[j] - T[i] >= lo; ++i) {
            const float r = fabsf(C[j] - C[i]) / (float)(T[j] - T[i]);
            if (r > best) best = r;
        }
        // ~8 ms peak, for the log
        int k = j - 1;
        while (k > 0 && T[j] - T[k] < 0.008) --k;
        if (T[j] - T[k] >= 0.008) {
            const float r = fabsf(C[j] - C[k]) / (float)(T[j] - T[k]);
            if (r > peak) peak = r;
        }
    }
    out->sustained = best;
    out->sweep     = fabsf(C[m - 1] - C[0]);
    out->peak      = peak;
    out->samples   = m;
    return best > 0.0f;
}

// ------------------------------------------------------------------ status
void PadSampler_PumpFrame() {
    static double lastLog = 0.0;
    static int lastState = -2;
    const double now = QpcSec();
    if (now - lastLog < 10.0) return;
    const int state = g_hz <= 0 ? 0 : (PadSampler_Live() ? 2 : 1);
    if (state == lastState && now - lastLog < 300.0) return;
    lastLog = now; lastState = state;
    if (state == 0) { TwkLog("[pad] controller sampler OFF -- flick timing follows the frame rate"); return; }
    if (state == 1) { TwkLog("[pad] no XInput controller answering -- flick timing follows the frame rate"); return; }
    TwkLog("[pad] controller %d sampled %d times/s (%s timer, worst gap %.1f ms, read %d us) -- it "
           "reports every %.1f ms while moving (%d reports/s)",
           g_statUser.load(), g_statPolls.load(), g_statHiRes.load() ? "high-res" : "LOW-RES",
           g_statWorstGapUs.load() / 1000.0, g_statReadUs.load(), g_reportUs.load() / 1000.0,
           g_statReports.load());
}

void PadSampler_DrawStatus(const OmpMenuApi* api) {
    char b[220];
    if (g_hz <= 0) { api->TextDisabled("Controller timing off -- flick timing follows the frame rate."); return; }
    if (!PadSampler_Live()) {
        api->TextDisabled("No XInput controller answering -- flick timing follows the frame rate.");
        return;
    }
    snprintf(b, sizeof(b), "Controller read %d times/s%s; it reports every %.1f ms while moving. "
                           "Flick timing ignores the frame rate.",
             g_statPolls.load(), g_statHiRes.load() ? "" : " (low-res timer)", g_reportUs.load() / 1000.0);
    api->TextDisabled(b);
}

#ifdef PAD_SAMPLER_TEST
void PadSampler_TestReset(double reportSec) {
    g_w.store(0); g_testLive = true; g_reportUs.store((int)(reportSec * 1e6)); g_testNow = 0.0;
}
void PadSampler_TestPush(double t, float lx, float ly, float rx, float ry) {
    Sample s; s.t = t; s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry; Push(s);
}
void PadSampler_TestSetNow(double t) { g_testNow = t; }
#endif
