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
// omp_padtest -- the controller sampler's measurements (src/tweaks/pad_sampler.cpp) against a
// simulated controller. The controller reports its stick at a fixed interval starting at a random
// phase, with a little noise while the thumb rests; the sampler's own policy is reproduced exactly
// (a read every 1 ms, a sample on every change plus a keep-alive every 20 ms). What is checked is the
// promise the module makes: the same flick measures the same however the reports and the query fall
// relative to it, a faster flick always measures faster, and a let-go or a crouch is not mistaken for
// the flick.
#define PAD_SAMPLER_TEST 1
#include "../../src/tweaks/pad_sampler.cpp"
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <random>
#include <vector>

// ---- the module's outside world, stubbed
void TwkLog(const char*, ...) {}
int  TwkIniInt(const char*, const char*, int def) { return def; }
int  TwkIniSetInt(char*, size_t, const char*, int) { return 0; }
void TwkMarkDirty() {}
long PopProbe_PadUser() { return -1; }
bool PopProbe_ReadPadUnhooked(unsigned long, unsigned long*, short*) { return false; }
void PadSampler_TestReset(double reportSec);
void PadSampler_TestPush(double t, float lx, float ly, float rx, float ry);
void PadSampler_TestSetNow(double t);

// A stick trajectory: position at time t (left stick; the right stays centred).
struct Traj { virtual void at(double t, float* x, float* y) const = 0; virtual ~Traj() {} };
static float smooth01(double u) { if (u <= 0) return 0.f; if (u >= 1) return 1.f; return (float)(u * u * (3 - 2 * u)); }
// Rest at `from`, then a smooth move to `to` over `dur` starting at t0, then hold; optionally let go
// (back to the centre over relDur) `holdSec` after arriving.
struct Flick : Traj {
    double t0, dur; float fx, fy, tx, ty; double holdSec = -1, relDur = 0.02;
    void at(double t, float* x, float* y) const override {
        const float u = smooth01((t - t0) / dur);
        float px = fx + (tx - fx) * u, py = fy + (ty - fy) * u;
        if (holdSec >= 0) {
            const float r = smooth01((t - (t0 + dur + holdSec)) / relDur);
            px *= (1 - r); py *= (1 - r);
        }
        *x = px; *y = py;
    }
};
// A quarter-circle scoop: angle from a0 to a1 (degrees) over dur at full deflection.
struct Scoop : Traj {
    double t0, dur; float a0, a1;
    void at(double t, float* x, float* y) const override {
        double u = (t - t0) / dur; if (u < 0) u = 0; if (u > 1) u = 1;
        const double a = (a0 + (a1 - a0) * u) * 3.14159265358979 / 180.0;
        const float mag = (t < t0 - 0.05 || t > t0 + dur + 0.05) ? 0.f : 1.f;
        *x = (float)cos(a) * mag; *y = (float)sin(a) * mag;
    }
};

// The controller + the sampler thread, reproduced: reports every `rep` s from `phase`; the sampler
// reads every 1 ms and keeps a sample on each change and every 20 ms regardless.
static void feed(const Traj& tr, double rep, double phase, double tEnd, std::mt19937& rng, float restNoise) {
    std::uniform_real_distribution<float> nz(-restNoise, restNoise);
    float repX = 0, repY = 0; double nextRep = phase;
    float lastX = 1e9f, lastY = 1e9f; double lastPush = -1;
    for (double t = 0.0; t <= tEnd + 1e-9; t += 0.001) {
        while (nextRep <= t) {
            float x, y; tr.at(nextRep, &x, &y);
            if (fabsf(x) < 0.05f && fabsf(y) < 0.05f && restNoise > 0) { x += nz(rng); y += nz(rng); }
            repX = x; repY = y; nextRep += rep;
        }
        const bool changed = (repX != lastX || repY != lastY);
        if (changed || t - lastPush >= 0.020) {
            PadSampler_TestPush(t, repX, repY, 0.f, 0.f);
            lastX = repX; lastY = repY; lastPush = t;
        }
    }
}
// Both sticks, the same policy (no rest noise).
static void feed2(const Traj& trL, const Traj& trR, double rep, double phase, double tEnd) {
    float rep4[4] = { 0, 0, 0, 0 }, last4[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
    double nextRep = phase, lastPush = -1;
    for (double t = 0.0; t <= tEnd + 1e-9; t += 0.001) {
        while (nextRep <= t) {
            trL.at(nextRep, &rep4[0], &rep4[1]); trR.at(nextRep, &rep4[2], &rep4[3]);
            nextRep += rep;
        }
        bool changed = false;
        for (int k = 0; k < 4; ++k) if (rep4[k] != last4[k]) changed = true;
        if (changed || t - lastPush >= 0.020) {
            PadSampler_TestPush(t, rep4[0], rep4[1], rep4[2], rep4[3]);
            for (int k = 0; k < 4; ++k) last4[k] = rep4[k];
            lastPush = t;
        }
    }
}
// A straight sweep at constant speed (stick units / s) along Y, from `from` downward.
struct Sweep : Traj {
    double t0; float speed, from;
    void at(double t, float* x, float* y) const override {
        *x = 0.f; *y = from - (t > t0 ? (float)(t - t0) * speed : 0.f);
    }
};
static bool measure(const Traj& tr, double rep, double phase, double queryAt, std::mt19937& rng,
                    PadFlick* out, float restNoise = 0.01f) {
    PadSampler_TestReset(rep);
    feed(tr, rep, phase, queryAt, rng, restNoise);
    PadSampler_TestSetNow(queryAt);
    return PadSampler_Flick(false, 0.45f, out);
}

static int g_fails = 0;
static void check(bool ok, const char* what) { printf("  %-78s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) ++g_fails; }

int main() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> ph01(0.0, 1.0);
    printf("omp_padtest -- controller-clock flick and scoop measurement\n");

    // ---- 1. the same flick, every report phase and query moment: how consistent is it?
    const double durs[] = { 0.015, 0.025, 0.040, 0.070, 0.120 };
    const double reps[] = { 0.001, 0.004, 0.008 };
    double meanAt[3][5] = {};
    for (int r = 0; r < 3; ++r) {
        printf(" reports every %.0f ms:\n", reps[r] * 1000);
        for (int d = 0; d < 5; ++d) {
            std::vector<double> v;
            for (int k = 0; k < 200; ++k) {
                Flick f; f.t0 = 0.300; f.dur = durs[d]; f.fx = 0; f.fy = 0; f.tx = -0.7f; f.ty = 0.7f;
                const double phase = ph01(rng) * reps[r];
                // WHEN THE GAME ASKS: it recognises a flick only once its polled stick is well out, and
                // asks within that frame -- so from the first REPORT at >= 80% of the flick's travel,
                // to half a flick plus 30 ms later.
                double firstOut = f.t0 + f.dur;
                for (double tr = phase; tr < f.t0 + f.dur + reps[r]; tr += reps[r]) {
                    float x, y; f.at(tr, &x, &y);
                    if (sqrtf(x * x + y * y) >= 0.8f) { firstOut = tr; break; }
                }
                const double query = firstOut + (f.dur * 0.5 + 0.03) * ph01(rng);
                PadFlick pf;
                if (measure(f, reps[r], phase, query, rng, &pf)) v.push_back(pf.speed);
            }
            double m = 0; for (double x : v) m += x; m /= (v.empty() ? 1 : v.size());
            double sd = 0; for (double x : v) sd += (x - m) * (x - m); sd = v.size() > 1 ? sqrt(sd / (v.size() - 1)) : 0;
            double lo = 1e9, hi = 0; for (double x : v) { if (x < lo) lo = x; if (x > hi) hi = x; }
            meanAt[r][d] = m;
            printf("   flick %3.0f ms: measured %3zu/200, speed %6.1f u/s  (min %6.1f max %6.1f, spread %4.1f%%)\n",
                   durs[d] * 1000, v.size(), m, lo, hi, m > 0 ? 100.0 * sd / m : 0.0);
            char what[128];
            // Resolution is the controller's report interval: a flick shorter than ~3 reports cannot be
            // timed finely by anything, so its tolerance is wider.
            const double tol = (durs[d] >= 3 * reps[r]) ? 0.15 : 0.40;
            snprintf(what, sizeof(what), "  %2.0f ms reports, %3.0f ms flick: always measured, spread < %2.0f%%",
                     reps[r] * 1000, durs[d] * 1000, tol * 100);
            check(v.size() == 200 && m > 0 && sd / m < tol, what);
        }
        char what[128];
        snprintf(what, sizeof(what), "  %2.0f ms reports: a faster flick always measures faster", reps[r] * 1000);
        bool mono = true; for (int d = 1; d < 5; ++d) if (!(meanAt[r][d] < meanAt[r][d - 1])) mono = false;
        check(mono, what);
    }
    // ---- 2. the report rate does not change the answer (beyond its own resolution)
    {
        const double a = meanAt[0][2], b = meanAt[1][2], c = meanAt[2][2];     // the 40 ms flick
        check(fabs(b - a) / a < 0.10 && fabs(c - a) / a < 0.15,
              "40 ms flick reads the same at 1, 4 and 8 ms reports (within 10% / 15%)");
    }
    // ---- 3. a let-go before the question is not the flick
    {
        Flick base; base.t0 = 0.3; base.dur = 0.035; base.fx = 0; base.fy = 0; base.tx = 0; base.ty = 1;
        Flick rel = base; rel.holdSec = 0.010; rel.relDur = 0.015;               // a faster spring-back
        PadFlick a, b;
        const bool okA = measure(base, 0.004, 0.0013, 0.36, rng, &a);
        const bool okB = measure(rel,  0.004, 0.0013, 0.40, rng, &b);
        printf("   held %.1f u/s vs released-before-asked %.1f u/s\n", a.speed, b.speed);
        check(okA && okB && fabs(a.speed - b.speed) / a.speed < 0.05,
              "flick released before the trick is asked for: measures the FLICK, not the let-go");
    }
    // ---- 4. from a held crouch: the rest point is the crouch, not the centre
    {
        Flick crouch; crouch.t0 = 0.30; crouch.dur = 0.040; crouch.fx = 0; crouch.fy = -1; crouch.tx = 0; crouch.ty = 1;
        struct Pre : Traj { Flick f; void at(double t, float* x, float* y) const override {
            if (t < 0.10) { *x = 0; *y = 0; return; }
            if (t < 0.13) { *x = 0; *y = -smooth01((t - 0.10) / 0.03); return; }
            f.at(t, x, y); } } pre; pre.f = crouch;
        PadFlick pf;
        const bool ok = measure(pre, 0.004, 0.002, 0.36, rng, &pf);
        printf("   crouch-to-flick: D %.2f, rise %.1f ms, %.1f u/s\n", pf.dist, pf.riseMs, pf.speed);
        check(ok && pf.dist > 1.8f && pf.riseMs > 15.f && pf.riseMs < 35.f,
              "flick out of a held crouch: measured from the crouch, across the whole stick");
    }
    // ---- 5. a stick that only drifts is not a flick
    {
        Flick drift; drift.t0 = 0.3; drift.dur = 0.050; drift.fx = 0; drift.fy = 0; drift.tx = 0.05f; drift.ty = 0.05f;
        PadFlick pf;
        check(!measure(drift, 0.004, 0.001, 0.4, rng, &pf), "a nudge under the travel floor measures nothing (caller keeps its own)");
    }
    // ---- 6. the scoop: sustained sweep rate off the reports
    {
        std::vector<double> v;
        for (int k = 0; k < 100; ++k) {
            Scoop sc; sc.t0 = 0.3; sc.dur = 0.150; sc.a0 = -90.f; sc.a1 = -180.f;     // 90 deg in 150 ms
            PadSampler_TestReset(0.004);
            feed(sc, 0.004, ph01(rng) * 0.004, 0.46, rng, 0.0f);
            PadSampler_TestSetNow(0.46);
            PadSweep ps;
            if (PadSampler_Sweep(false, 0.5f, 0.35f, 0.0f, &ps)) v.push_back(ps.sustained);
        }
        double m = 0; for (double x : v) m += x; m /= (v.empty() ? 1 : v.size());
        double lo = 1e9, hi = 0; for (double x : v) { if (x < lo) lo = x; if (x > hi) hi = x; }
        printf("   scoop 90 deg / 150 ms: sustained %.0f deg/s (min %.0f max %.0f), 600 exact\n", m, lo, hi);
        check(v.size() == 100 && fabs(m - 600.0) / 600.0 < 0.08 && (hi - lo) / m < 0.10,
              "scoop's sustained rate is the true rate (within 8%), whatever the report phase");
    }
    // ---- 7. the pop scheme's speed test (pop_probe's machine): travel over one reference interval
    // (1/90 s) ending at the game's read, taken from StickAt, against the threshold of 2500/32767 per
    // interval. It must give the same answer at any frame rate; the per-read delta it replaces does not.
    {
        const double iv = 1.0 / 90.0, thr = 2500.0 / 32767.0;
        const float speeds[2] = { 5.5f, 10.0f };          // below / above thr / iv = 6.9 units/s
        const double fps[3] = { 60.0, 90.0, 144.0 };
        bool padSame = true;
        for (int si = 0; si < 2; ++si) {
            Sweep sw; sw.t0 = 0.300; sw.speed = speeds[si]; sw.from = 0.95f;
            Flick still; still.t0 = 9.0; still.dur = 0.01; still.fx = still.fy = still.tx = still.ty = 0.f;
            char line[160]; int w = snprintf(line, sizeof(line), "   sweep %.1f/s:", speeds[si]);
            for (int fi = 0; fi < 3; ++fi) {
                int padFast = 0, readFast = 0, reads = 0;
                for (int k = 0; k < 20; ++k) {
                    const double phase = ph01(rng) * 0.004;
                    PadSampler_TestReset(0.004);
                    feed2(sw, still, 0.004, phase, 0.420);
                    const double tRead = 0.330 + ph01(rng) * 0.070;   // mid-sweep, any phase
                    PadSampler_TestSetNow(tRead);
                    float x0, y0, x1, y1, xp, yp;
                    if (!PadSampler_StickAt(false, tRead, &x0, &y0) || !PadSampler_StickAt(false, tRead - iv, &x1, &y1) ||
                        !PadSampler_StickAt(false, tRead - 1.0 / fps[fi], &xp, &yp)) continue;
                    ++reads;
                    if (fabs(y0 - y1) > thr) ++padFast;
                    if (fabs(y0 - yp) > thr) ++readFast;
                }
                const bool want = (si == 1);
                if (reads == 0 || (want ? padFast != reads : padFast != 0)) padSame = false;
                w += snprintf(line + w, sizeof(line) - w, "  %3.0f fps: pad %s, per-read %s", fps[fi],
                              padFast == reads ? "fast" : padFast == 0 ? "slow" : "MIXED",
                              readFast == reads ? "fast" : readFast == 0 ? "slow" : "MIXED");
            }
            printf("%s\n", line);
        }
        check(padSame, "pop speed test on the pad clock: same answer at 60, 90 and 144 fps");
    }
    // ---- 8. the catch foot: which stick left the deadzone later, when both did inside one frame
    {
        int right = 0, same = 0, n = 0;
        for (int k = 0; k < 100; ++k) {
            Flick fl; fl.t0 = 0.300; fl.dur = 0.020; fl.fx = 0; fl.fy = 0; fl.tx = 0; fl.ty = 1;
            Flick fr = fl; fr.t0 = 0.306;                      // R leaves 6 ms after L
            PadSampler_TestReset(0.004);
            feed2(fl, fr, 0.004, ph01(rng) * 0.004, 0.340);
            PadSampler_TestSetNow(0.340);
            double tL, tR, rL, rR;
            if (!PadSampler_OutwardCrossing(false, 0.2f, 0.240, &tL, &rL) ||
                !PadSampler_OutwardCrossing(true, 0.2f, 0.240, &tR, &rR)) continue;
            ++n;
            if (rL != rR && tR > tL) ++right;
            // and the same flick on both sticks: one report, no order
            PadSampler_TestReset(0.004);
            feed2(fl, fl, 0.004, ph01(rng) * 0.004, 0.340);
            PadSampler_TestSetNow(0.340);
            if (PadSampler_OutwardCrossing(false, 0.2f, 0.240, &tL, &rL) &&
                PadSampler_OutwardCrossing(true, 0.2f, 0.240, &tR, &rR) && rL == rR) ++same;
        }
        printf("   catch order: R-after-L named in %d of %d, simultaneous left unordered in %d of 100\n", right, n, same);
        check(n == 100 && right == 100, "a stick leaving 6 ms after the other is always named the later one");
        check(same == 100, "two sticks leaving on the same report are never ordered");
    }
    printf("%s (%d failed)\n", g_fails ? "PAD TEST FAIL" : "PAD TEST PASS", g_fails);
    return g_fails ? 1 : 0;
}
