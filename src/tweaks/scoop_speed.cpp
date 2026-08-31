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
// SessionTweaks -- SCOOP SPEED. The stock behaviour has three measured defects:
//  * Scoop speed maps from the flick's DURATION over a 0.10-0.25 s window; real scoops take
//    0.30-0.55 s, so nearly all of them clamp to the 0.600 floor.
//  * The lookup curve is a staircase, dead flat across its middle third.
//  * Pocket/corner starts usually produce NO arc input at all (the trick fires before the recogniser
//    closes), giving a hardcoded 1.0 with no speed control whatsoever.
// The fix tracks the raw sticks per InputHandler::Tick, accumulates an unwrapped sweep per GESTURE
// (stick leaves centre -> returns), rates it as the best SUSTAINED speed over a 50-120 ms baseline,
// normalises by the gesture's size (a 360 shove travels ~2x a 180's arc), then maps linearly, which
// bypasses the staircase. Peak-instantaneous rating is noise-dominated -- a single 16 ms spike sets
// the whole gesture -- so the sustained baseline is required. Both sticks are tracked because
// nollie/switch scoop on the LEFT; the arc element's EInputType names its stick (left/right values
// mirror at +50), and with no arc the larger fresh gesture wins.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "scoop_speed.h"
#include "tweaks_mod.h"
#include "pop_probe.h"       // the injection experiment's two write points in this module's tick hook
#include <cmath>
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    FTH_SKATER              = 0x08,    // FlipTricksHandler -> ASkaterCharacterBase*
    FTH_TRICKS_DB           = 0x28,    // FlipTricksHandler -> UTricksDatabase*
    DB_ROT_SPEED_MIN_MULT   = 0x1a8,   // RotationSpeedMinMultiplier          (ships 0.6)
    DB_ROT_SPEED_MAX_MULT   = 0x1ac,   // RotationSpeedMaxMultiplier          (ships 1.4)
    SKATER_ROT_SPEED_MODE   = 0x650,   // _boardRotationSpeedMode -- ==2 is the only implemented branch
    SKATER_SCOOP_SPEED_MULT = 0x9d8,   // _advancedSettings.ScoopSpeedMultiplier (the in-game slider)
    IH_FRAME_RAW_LEFT       = 0x24,    // InputHandler -> _frameRawLeftInput  (FVector2D)
    IH_FRAME_RAW_RIGHT      = 0x2c,    // InputHandler -> _frameRawRightInput (FVector2D)
    // FInputData (36 B): the TArray the speed function receives
    FID_INPUT = 0x00, FID_PRIORITY = 0x04, FID_LSTICK = 0x08, FID_RSTICK = 0x10,
    FID_ANGLE = 0x18, FID_INPUT_TIME = 0x1c, FID_TOTAL_TIME = 0x20, FID_STRIDE = 0x24,
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int   g_scoopFix    = 1;       // AngularVelocity: 0 = measure only, stock value returned
static int   g_scoopLog    = 1;       // one log line (+input array) per scoop
static int   g_neutralUnmeasured = 1;   // ScoopNeutralWhenUnmeasured
// ScoopSustainFracPct -- scale the sustained window to a fraction of the gesture instead of the
// absolute 50-120 ms.
// DEFAULT 0 (OFF). TESTED AND REJECTED at 35: real scoops ACCELERATE rather than holding one speed,
// so requiring the peak to hold for 130-260 ms averages the fast part together with the wind-up and
// drags every genuine scoop down. Measured, same gesture shape before and after:
//     before: sweep=184 raw=2328 -> OURS=1.399     after: sweep=183 raw=1295 -> OURS=0.860
//     a FAST 125 ms 180deg scoop (arcVel=720) came out at 0.674, near the floor
// It did not merely fail to isolate the yank -- it penalised exactly what the feature rewards.
// Kept as a knob because the mechanism is sound for a LONG gesture; it just cannot be the default.
static float g_sustainFrac = 0.0f;      // ScoopSustainFracPct
static int   g_useTracker  = 1;       // 0 = fall back to the game's arc input
static int   g_normArc     = 1;       // normalise by gesture size (360 vs 180 shove)
static float g_velMin      = 350.0f;  // sustained deg/s mapping to the slowest scoop
static float g_velMax      = 1600.0f; // ... and the fastest (calibrated against measured play)
static float g_minAngle    = 45.0f;   // arc fallback: below this swept angle it is not a scoop
static float g_minTime     = 0.05f;   // ... nor briefer; stray sub-25deg flicks would pin the max
static float g_trackWindow = 0.70f;   // staleness bound on a finished gesture
static float g_trackMinMag = 0.50f;   // below this magnitude = centre region, gesture boundary
static int   g_scoopElem   = -1;      // arc element override; -1 = auto (largest |angle|)
static volatile LONG g_faults = 0;
// F1 readout of the most recent scoop (game thread writes, render thread reads)
static volatile float g_uiSweep = 0, g_uiRaw = 0, g_uiNorm = 0, g_uiStock = 0, g_uiOurs = 0;
static volatile LONG  g_uiHadArc = 0, g_uiCount = 0, g_uiStickSide = -1;

void ScoopSpeed_ReadConfig(const char* buf) {
    g_scoopFix    = TwkIniInt(buf, "AngularVelocity", 1);
    g_scoopLog    = TwkIniInt(buf, "ScoopLog", 1);
    g_useTracker  = TwkIniInt(buf, "UseStickTracker", 1);
    g_neutralUnmeasured = TwkIniInt(buf, "ScoopNeutralWhenUnmeasured", 1);
    g_sustainFrac = (float)TwkIniInt(buf, "ScoopSustainFracPct", 0) / 100.0f;
    if (g_sustainFrac < 0.0f) g_sustainFrac = 0.0f; else if (g_sustainFrac > 0.9f) g_sustainFrac = 0.9f;
    g_normArc     = TwkIniInt(buf, "NormalizeArc", 1);
    g_velMin      = (float)TwkIniInt(buf, "VelMin", 350);
    g_velMax      = (float)TwkIniInt(buf, "VelMax", 1600);
    g_minAngle    = (float)TwkIniInt(buf, "MinAngle", 45);
    g_minTime     = (float)TwkIniInt(buf, "MinTimeMs", 50) / 1000.0f;
    g_trackWindow = (float)TwkIniInt(buf, "TrackWindowMs", 700) / 1000.0f;
    g_trackMinMag = (float)TwkIniInt(buf, "TrackMinMag", 50) / 100.0f;
    g_scoopElem   = TwkIniInt(buf, "ScoopElement", -1);
    if (g_velMax <= g_velMin) g_velMax = g_velMin + 1.0f;
    TwkLog("[scoop] config: AngularVelocity=%d ScoopLog=%d UseStickTracker=%d NormalizeArc=%d "
           "Vel=[%.0f..%.0f] MinAngle=%.0f MinTimeMs=%.0f TrackWindowMs=%.0f TrackMinMag=%.2f ScoopElement=%d",
           g_scoopFix, g_scoopLog, g_useTracker, g_normArc, g_velMin, g_velMax,
           g_minAngle, g_minTime * 1000.0f, g_trackWindow * 1000.0f, g_trackMinMag, g_scoopElem);
}

void ScoopSpeed_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "AngularVelocity", g_scoopFix);
    TwkIniSetInt(buf, cap, "UseStickTracker", g_useTracker);
    TwkIniSetInt(buf, cap, "ScoopNeutralWhenUnmeasured", g_neutralUnmeasured);
    TwkIniSetInt(buf, cap, "ScoopSustainFracPct", (int)(g_sustainFrac * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "NormalizeArc",    g_normArc);
    TwkIniSetInt(buf, cap, "VelMin",          (int)g_velMin);
    TwkIniSetInt(buf, cap, "VelMax",          (int)g_velMax);
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
static const char* SIG_SCOOP_SPEED =    // FlipTricksHandler::GetBoardRotationSpeedMultiplier
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 70 0F 29 74 24 60 44 0F B6 CA "
    "0F 29 7C 24 50 48 8B F1 84 D2 0F 84 ?? ?? ?? ??";                        // Epic 0x1024bb0 / Steam 0xfe4d20
static const char* SIG_INPUT_TICK =     // InputHandler::Tick -- TWO float args (xmm1+xmm2)!
    "48 8B C4 53 57 48 81 EC 18 01 00 00 48 83 79 10 00 49 8B F9 44 0F 29 A0 68 FF FF FF 48 8B D9 "
    "44 0F 29 74 24 70 44 0F 28 E1 44 0F 28 F2 0F 84 ?? ?? ?? ??";            // Epic 0x10650e0 / Steam 0x1025460

// ------------------------------------------------------------------ the gesture-scoped trackers
static const int kVelHist = 96;
struct StickTracker {
    double velT[kVelHist];      // QPC seconds of each sample
    float  velCum[kVelHist];    // UNWRAPPED cumulative swept angle, degrees
    float  velInst[kVelHist];   // frame-to-frame rate, kept only for the log
    int    nSamples;
    double lastSampleT;
    float  lastAngle, cumAngle;
    bool   haveLast;
};
static StickTracker g_trkL = {}, g_trkR = {};

static double nowSeconds() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ---- radial (flick) history, for flip_speed --------------------------------------------------
// The tracker above measures ANGULAR sweep, which is what a scoop is. A flip flick is RADIAL --
// straight out through the centre -- so it needs its own samples, and crucially they must be taken
// with NO magnitude gate: the interesting part of a flick is the climb from the centre, which is
// entirely below `g_trackMinMag` and never reaches the tracker at all.
static const int kMagHist = 96;
struct MagTrack { double t[kMagHist]; float x[kMagHist], y[kMagHist], m[kMagHist]; int n; };
static MagTrack g_magL = {}, g_magR = {};
// Below this the stick is still in the centre region -- the push is considered to start here.

// A gesture that never gets this far out is not a flick; reporting a speed for it would let a nudge
// of the stick register as a fast one just because it happened quickly.

// How far the stick must actually TRAVEL for the measurement to mean anything. Below this we report
// no measurement and the caller keeps the game's own value -- never the slowest setting.
static const float kStillStep      = 0.004f;  // per-sample movement below this = the stick is at rest
static const float kFlickMinTravel = 0.10f;   // total 2D travel a gesture must cover to be measured
static void trackMag(MagTrack& M, float x, float y, double t) {
    if (M.n >= kMagHist) {                       // keep the recent half, same shape as StickTracker
        const int keep = kMagHist / 2;
        memmove(M.t, M.t + (kMagHist - keep), keep * sizeof(M.t[0]));
        memmove(M.x, M.x + (kMagHist - keep), keep * sizeof(M.x[0]));
        memmove(M.y, M.y + (kMagHist - keep), keep * sizeof(M.y[0]));
        memmove(M.m, M.m + (kMagHist - keep), keep * sizeof(M.m[0]));
        M.n = keep;
    }
    M.t[M.n] = t;
    M.x[M.n] = x; M.y[M.n] = y;
    M.m[M.n] = sqrtf(x * x + y * y);
    M.n++;
}
// The FLICK, measured as the gesture it actually is: how far the stick travelled from where the push
// began to where it peaked, over how long that took.
//
// THE OBVIOUS VERSION IS FRAME-RATE DEPENDENT, which is the whole bug this replaces. Taking the
// fastest rate over sample pairs at least `lo` seconds apart sounds frame-rate independent -- it
// divides by real time -- but the CLOSEST usable pair is bounded by the frame interval, so the
// highest speed it can report is one full stick throw in that interval. At 60 fps that ceiling is
// ~30 u/s; at 144 fps it is ~48. The same physical flick reads differently, and a threshold set on
// one machine is unreachable on another.
//
// Measuring start-to-peak has no such ceiling: both the distance and the duration are properties of
// YOUR HAND, and more samples only locate them more precisely. A flick that takes 45 ms to reach full
// deflection reads ~22 u/s whether that was sampled 3 times or 12.
// Prints the raw stick magnitudes leading up to NOW. The trick fires while the flick is still
// travelling, so what the samples look like at that instant is the only thing that says whether a
// speed can be measured there at all -- and that cannot be reasoned out from the outside.
void ScoopSpeed_DumpFlick(bool rightStick, float windowSec) {
    const MagTrack& M = rightStick ? g_magR : g_magL;
    if (M.n < 2) { TwkLog("[flick] no samples"); return; }
    const double now = nowSeconds();
    char line[400]; int len = 0;
    len += snprintf(line + len, sizeof(line) - len, "[flick] %c:", rightStick ? 'R' : 'L');
    // x,y and not just the magnitude: a magnitude of 1.00 could be (1,0), (0,0.71,0.71) or a
    // normalised direction, and those mean completely different things about the device. Every
    // fourth sample, so the line still fits.
    for (int i = 0; i < M.n && len < (int)sizeof(line) - 26; i++) {
        if (now - M.t[i] > windowSec) continue;
        if ((M.n - 1 - i) % 4 && i != M.n - 1) continue;
        len += snprintf(line + len, sizeof(line) - len, " %.0f:(%+.2f,%+.2f)",
                        (now - M.t[i]) * 1000.0, M.x[i], M.y[i]);
    }
    TwkLog("%s   (ms-ago:(x,y), newest last)", line);
}

bool ScoopSpeed_FlickMeasure(bool rightStick, float windowSec, float* outSpeed, float* outPeak,
                             float* outFrameMs) {
    const MagTrack& M = rightStick ? g_magR : g_magL;
    if (outSpeed)   *outSpeed = 0.0f;
    if (outPeak)    *outPeak = 0.0f;
    if (outFrameMs) *outFrameMs = 0.0f;
    if (M.n < 2) return false;
    const double now = nowSeconds();
    if (now - M.t[M.n - 1] > 0.25) return false;             // no live input tick: say nothing

    int first = M.n - 1;                                     // oldest sample still inside the window
    while (first > 0 && (now - M.t[first - 1]) <= windowSec) first--;
    if (first >= M.n - 1) return false;
    if (outFrameMs)
        *outFrameMs = (float)((M.t[M.n - 1] - M.t[first]) / (M.n - 1 - first) * 1000.0);
    for (int i = first; i < M.n; i++) if (M.m[i] > (outPeak ? *outPeak : 0.0f) && outPeak) *outPeak = M.m[i];

    // MEASURE THE 2D PATH THE STICK TRAVELLED, not how far it got from centre. Distance travelled is
    // the gesture regardless of where it started, so this reads a push out from rest and a movement
    // made while already deflected identically; dividing by the burst's own duration keeps it
    // frame-rate independent. The radial version it replaced also rejected genuine flicks whose peak
    // was only ~0.25, which is where this game's tricks actually fire.
    // The burst may have ENDED just before the query. A trick is recognised at the END of its
    // flick, so by the time the multiplier is asked for, the thumb has often just reached the
    // flick's endpoint and sat still for a poll or two -- and anchoring the walk-back on the newest
    // sample made the whole measurement a race: the identical flick read full speed when the query
    // landed mid-motion and EXACTLY ZERO when it landed a poll after the stop (field log: L0 on a
    // third of one session's kickflips, each mapped to the minimum multiplier while the stick that
    // merely drifted was picked instead). Skip trailing stillness first, bounded so a long-dead
    // gesture can never be resurrected.
    int end = M.n - 1;
    while (end > first) {
        const float dx = M.x[end] - M.x[end - 1], dy = M.y[end] - M.y[end - 1];
        if (dx * dx + dy * dy >= kStillStep * kStillStep) break;
        if (now - M.t[end - 1] > 0.15) break;                // stillness older than the bound: stop
        end--;
    }
    int start = end;                                         // walk back over that movement
    for (int i = end; i > first; i--) {
        const float dx = M.x[i] - M.x[i - 1], dy = M.y[i] - M.y[i - 1];
        if (dx * dx + dy * dy < kStillStep * kStillStep) break;   // the stick was at rest here
        start = i - 1;
    }
    float path = 0.0f;
    for (int i = start + 1; i <= end; i++) {
        const float dx = M.x[i] - M.x[i - 1], dy = M.y[i] - M.y[i - 1];
        path += sqrtf(dx * dx + dy * dy);
    }
    const double span = M.t[end] - M.t[start];
    if (span <= 0.0) return false;
    // A gesture has to have actually gone somewhere before its duration means anything. Below this
    // there is no measurement and the caller keeps the game's own value -- never the slowest setting.
    if (path < kFlickMinTravel) return false;
    if (outSpeed) *outSpeed = path / (float)span;
    return true;
}


static void trackOne(StickTracker& T, float x, float y, double t) {
    const float mag = sqrtf(x * x + y * y);
    if (mag < g_trackMinMag) { T.haveLast = false; return; }
    const float ang = atan2f(y, x) * 57.2957795f;
    const bool newGesture = !T.haveLast || (t - T.lastSampleT) > 0.30;
    if (newGesture) {
        T.nSamples = 0; T.cumAngle = 0.0f;
    } else {
        const double dt = t - T.lastSampleT;
        if (dt > 0.0005) {
            float dg = ang - T.lastAngle;
            while (dg >  180.0f) dg -= 360.0f;
            while (dg < -180.0f) dg += 360.0f;
            T.cumAngle += dg;
            if (T.nSamples >= kVelHist) {
                const int keep = kVelHist / 2;
                memmove(T.velT,    T.velT    + (kVelHist - keep), keep * sizeof(T.velT[0]));
                memmove(T.velCum,  T.velCum  + (kVelHist - keep), keep * sizeof(T.velCum[0]));
                memmove(T.velInst, T.velInst + (kVelHist - keep), keep * sizeof(T.velInst[0]));
                T.nSamples = keep;
            }
            T.velT[T.nSamples]    = t;
            T.velCum[T.nSamples]  = T.cumAngle;
            T.velInst[T.nSamples] = fabsf(dg) / (float)dt;
            T.nSamples++;
        }
    }
    T.lastAngle = ang; T.lastSampleT = t; T.haveLast = true;
}
// ---- live stick POSITION, for foot_steer ------------------------------------------------------
// Both trackers above keep derivatives (angular sweep, radial speed). A consumer that wants where
// the stick simply IS gets it from here for the same reason flip_speed takes its flick measure from
// here: only one detour may exist on InputHandler::Tick. Stamped, so a centred stick stays
// distinguishable from an input tick that has stopped running.
static float  g_rawL[2] = { 0.0f, 0.0f }, g_rawR[2] = { 0.0f, 0.0f };
static double g_rawT = 0.0;

static void trackStick(void* handler) {
    const double t = nowSeconds();
    float lx, ly, rx, ry;
    // While the pop scheme rewrites the pad, the input fields hold what the GAME is being shown,
    // not what the player is doing -- and a flick tracker measuring the rewritten stick made flip
    // speed swing with thumb depth (the trick flick reached the fields clamped). The pad hook
    // publishes the pre-rewrite sticks; with the scheme off this returns false and the fields are
    // the truth exactly as before.
    if (!PopProbe_PhysSticks(&lx, &ly, &rx, &ry)) {
        lx = twkF(handler, IH_FRAME_RAW_LEFT);  ly = twkF(handler, IH_FRAME_RAW_LEFT + 4);
        rx = twkF(handler, IH_FRAME_RAW_RIGHT); ry = twkF(handler, IH_FRAME_RAW_RIGHT + 4);
    }
    trackOne(g_trkL, lx, ly, t);
    trackOne(g_trkR, rx, ry, t);
    trackMag(g_magL, lx, ly, t);
    trackMag(g_magR, rx, ry, t);
    g_rawL[0] = lx; g_rawL[1] = ly;
    g_rawR[0] = rx; g_rawR[1] = ry;
    g_rawT = t;
}

// See the header note: the handler is only reachable from a hook that is handed it, and this
// module's speed hook fires on every trick. Written from the game thread, read from the same pump.
static void* g_lastFth = nullptr;
void* ScoopSpeed_FlipHandler() { return g_lastFth; }

bool ScoopSpeed_StickRaw(bool rightStick, float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (g_rawT <= 0.0 || nowSeconds() - g_rawT > 0.25) return false;   // no live input tick
    const float* s = rightStick ? g_rawR : g_rawL;
    // twkF hands back an implausible sentinel on a bad read. A consumer that turns this into a
    // position offset must never see one, so an out-of-range pair is "no measurement", not data.
    if (!(fabsf(s[0]) <= 1.5f) || !(fabsf(s[1]) <= 1.5f)) return false;
    if (x) *x = s[0];
    if (y) *y = s[1];
    return true;
}
static bool gestureFresh(const StickTracker& T) {
    return T.nSamples > 1 && (nowSeconds() - T.velT[T.nSamples - 1]) <= g_trackWindow;
}
// THE BASELINE SCALES WITH THE GESTURE. It used to be an absolute 50-120 ms window, which asks
// "was the stick ever fast for 120 ms" -- and a brief YANK inside a long slow gesture answers yes.
// Measured on a spread-eagle tre, where the back stick is pulled down straight after the flick:
//     angle=133.8 totalTime=0.4582 arcVel=292   (the game: a slow 458 ms scoop, rated 0.600)
//     raw=1357 sustained                        (us: 4.6x the gesture's own average) -> OURS=1.601
// The gesture is scoped stick-leaves-centre -> stick-returns-centre, so the scoop AND the pull-down
// after it are one gesture and the fastest slice wins. Requiring the speed to hold for a FRACTION of
// the gesture makes "sustained" mean sustained relative to the movement being rated: a 458 ms gesture
// now demands ~160 ms of held speed, which a yank cannot supply, while a brisk 200 ms scoop only has
// to hold ~70 ms and is unaffected.
// A ratio test (reject sustained >> average) was considered and REJECTED against real data: a
// legitimate scoop measured arcVel=204 -> norm=852, a ratio of 4.2, indistinguishable from the
// artifact's 4.6. Duration separates them; magnitude does not.
static float sustainedStickVel(const StickTracker& T) {
    if (!gestureFresh(T)) return 0.0f;
    double loBase = 0.05, hiBase = 0.12;
    if (g_sustainFrac > 0.0f && T.nSamples > 1) {
        const double dur = T.velT[T.nSamples - 1] - T.velT[0];
        if (dur > 0.0) {
            loBase = dur * (double)g_sustainFrac;
            if (loBase < 0.05) loBase = 0.05; else if (loBase > 0.20) loBase = 0.20;
            hiBase = loBase * 2.0;
            if (hiBase > 0.35) hiBase = 0.35;
        }
    }
    float best = 0.0f;
    for (int j = 1; j < T.nSamples; j++)
        for (int i = 0; i < j; i++) {
            const double span = T.velT[j] - T.velT[i];
            if (span < loBase || span > hiBase) continue;
            const float rate = fabsf(T.velCum[j] - T.velCum[i]) / (float)span;
            if (rate > best) best = rate;
        }
    return best;
}
static float gestureSweep(const StickTracker& T) {
    if (!gestureFresh(T)) return 0.0f;
    return fabsf(T.velCum[T.nSamples - 1] - T.velCum[0]);
}
static float peakStickVel(const StickTracker& T) {   // logged only, for calibration
    if (!gestureFresh(T)) return 0.0f;
    float best = 0.0f;
    for (int i = 0; i < T.nSamples; i++) if (T.velInst[i] > best) best = T.velInst[i];
    return best;
}

// ------------------------------------------------------------------ hooks
// InputHandler::Tick is `(this, float, float, void*)` -- TWO float args, both declared `double`
// and forwarded UNCONVERTED so the compiler only copies the registers; timing comes from QPC.
typedef void* (*InputTickFn)(void*, double, double, void*);
static void* g_origTick  = nullptr;
static void* g_startTick = nullptr;
static void* hkInputTick(void* self, double a, double b, void* d) {
    if (self) {
        __try { trackStick(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_useTracker = 0; }
    }
    // The injection experiment's early write point: the physical sticks are already sampled above,
    // so a rewrite here cannot pollute what the trackers saw. No-op unless its knob selects it.
    PopProbe_TickEarly(self);
    // Mode 4: `d` is the tick's stick buffer (see PopProbe_TickSticks) -- rewritten in place,
    // within this call only, before the game derives anything from it.
    PopProbe_TickSticks((float*)d);
    Tweaks_PumpFrame();                           // menu-registration retry etc. (shell)
    void* ret = nullptr;
    __try { ret = ((InputTickFn)g_origTick)(self, a, b, d); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[scoop] caught fatal in InputHandler::Tick -> recovered");
        return nullptr;
    }
    PopProbe_TickLate(self);                      // ...and the late one, after the game's tick ran
    return ret;
}

// Pure RETURN-VALUE substitution: the original always runs, no game state is written.
typedef float (*ScoopSpeedFn)(void*, uint64_t, void*, void*);
static void* g_origScoop  = nullptr;
static void* g_startScoop = nullptr;
static float hkScoopSpeed(void* handler, uint64_t bArg, void* inputs, void* d) {
    float stock = 1.0f;
    __try { stock = ((ScoopSpeedFn)g_origScoop)(handler, bArg, inputs, d); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[scoop] caught fatal in GetBoardRotationSpeedMultiplier -> recovered");
        return 1.0f;
    }
    if (handler) g_lastFth = handler;             // publish for pop_probe (see ScoopSpeed_FlipHandler)
    if ((!g_scoopLog && !g_scoopFix) || !handler || !inputs) return stock;

    float out = stock;
    __try {
        void* db     = twkP(handler, FTH_TRICKS_DB);
        void* skater = twkP(handler, FTH_SKATER);
        if (!db || !skater) return stock;
        const int   mode = twkB(skater, SKATER_ROT_SPEED_MODE);
        const float minM = twkF(db, DB_ROT_SPEED_MIN_MULT);
        const float maxM = twkF(skater, SKATER_SCOOP_SPEED_MULT) * twkF(db, DB_ROT_SPEED_MAX_MULT);

        uint8_t* data = (uint8_t*)twkP(inputs, 0);
        int num = twkI(inputs, 8);
        if (num < 0 || num > 64) num = 0;

        // arc fallback: largest |Angle| selects the arc pair; 0 when none was recognised
        int pick = -1;
        float pAngle = 0.0f, pTotal = 0.0f, vel = 0.0f;
        if (data && num > 0) {
            if (g_scoopElem >= 0 && g_scoopElem < num) pick = g_scoopElem;
            else {
                float best = 0.0f;
                for (int i = 0; i < num; i++) {
                    const float a = fabsf(twkF(data + i * FID_STRIDE, FID_ANGLE));
                    if (a > best) { best = a; pick = i; }
                }
            }
            if (pick >= 0) {
                pAngle = twkF(data + pick * FID_STRIDE, FID_ANGLE);
                pTotal = twkF(data + pick * FID_STRIDE, FID_TOTAL_TIME);
                if (fabsf(pAngle) >= g_minAngle && pTotal >= g_minTime) {
                    float ang = fabsf(pAngle);
                    if (g_normArc) {
                        float expected = 90.0f * floorf(ang / 90.0f + 0.5f);
                        if (expected < 45.0f) expected = 45.0f;
                        ang = (ang / expected) * 90.0f;
                    }
                    vel = ang / pTotal;
                }
            }
        }
        // Which stick scooped. The entry's OWN stick fields decide it: whichever of LeftStick /
        // RightStick actually holds the displacement is the one that moved, which is true in any
        // stance and any input mode.
        // The EInputType >= 50 left/right mirror is only the FALLBACK now, and only when the sticks
        // are too close to call. That byte is the RAW recorded type, and the game normalises it
        // through IsSkatingGoofy / IsSkatingSwitch / ConvertToCurrentInputModeInput before it means
        // anything -- so raw it is only reliable in regular stance on the default input mode. The
        // same assumption in flip_speed made that feature silently do nothing for anyone else.
        int scoopStick = -1;
        if (pick >= 0) {
            const uint8_t* e = data + pick * FID_STRIDE;
            const float lx = twkF(e, FID_LSTICK), ly = twkF(e, FID_LSTICK + 4);
            const float rx = twkF(e, FID_RSTICK), ry = twkF(e, FID_RSTICK + 4);
            const float lm = sqrtf(lx * lx + ly * ly), rm = sqrtf(rx * rx + ry * ry);
            if (lm > rm * 1.25f)      scoopStick = 0;
            else if (rm > lm * 1.25f) scoopStick = 1;
            else {                                   // too close to call: fall back to the type byte
                const int ty = twkB(e, FID_INPUT);
                if (ty >= 0) scoopStick = (ty >= 50) ? 1 : 0;
            }
        }
        const StickTracker* T = nullptr;
        if      (scoopStick == 0) T = &g_trkL;
        else if (scoopStick == 1) T = &g_trkR;
        else {
            const float swL = gestureSweep(g_trkL), swR = gestureSweep(g_trkR);
            if (swL >= g_minAngle || swR >= g_minAngle) {
                if (swL >= swR) { T = &g_trkL; scoopStick = 0; }
                else            { T = &g_trkR; scoopStick = 1; }
            }
        }
        const float arcVel   = vel;
        const float rawTrack = T ? sustainedStickVel(*T) : 0.0f;
        const float peakVel  = T ? peakStickVel(*T)      : 0.0f;
        const float sweep    = T ? gestureSweep(*T)      : 0.0f;
        float trackVel = rawTrack;
        if (g_normArc && rawTrack > 0.0f && sweep > 0.0f) {
            float expected = 90.0f * floorf(sweep / 90.0f + 0.5f);
            if (expected < 90.0f) expected = 90.0f;
            trackVel = rawTrack * 90.0f / expected;
        }
        const char* src = "arc";
        bool unmeasured = false;
        if (g_useTracker && trackVel > 0.0f) { vel = trackVel; src = "tracker"; }

        if (g_scoopFix && mode == 2 && vel > 0.0f) {
            float r = (vel - g_velMin) / (g_velMax - g_velMin);
            if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
            out = minM + r * (maxM - minM);
        } else if (g_scoopFix && mode == 2 && g_neutralUnmeasured && stock > 1.0f) {
            // NO SUSTAINED SWEEP WAS MEASURED, so do NOT inherit the game's value here.
            // With no timing the game derives its multiplier from the RAW ARC alone, and a fast yank
            // traces a huge arc in almost no time -- measured on a tre flip where the back stick was
            // pulled down straight after the flick:
            //     angle=-177.2 totalTime=0.0000 arcVel=0 sweep=0  ->  STOCK=1.750 (the maximum)
            // against a real scoop, which always carries timing (angle=-128.5 totalTime=0.2166
            // arcVel=593 -> STOCK=1.029). Inheriting the first case is the mod claiming ownership of
            // scoop speed and then passing through the exact artifact it exists to replace.
            // Neutral (1.0), not minM: this says "no scoop gesture", not "the slowest possible scoop".
            out = 1.0f;
            unmeasured = true;
        }
        g_uiSweep = sweep; g_uiRaw = rawTrack; g_uiNorm = trackVel;
        g_uiStock = stock; g_uiOurs = out;
        InterlockedExchange(&g_uiHadArc, arcVel > 0.0f ? 1 : 0);
        InterlockedExchange(&g_uiStickSide, (LONG)scoopStick);
        InterlockedIncrement(&g_uiCount);

        if (g_scoopLog) {
            TwkLog("[scoop] SCOOP mode=%d n=%d pick=%d stick=%s | angle=%.1f totalTime=%.4f | arcVel=%.0f "
                   "sweep=%.0f raw=%.0f norm=%.0f (peak=%.0f) used=%s(%.0f) | min=%.3f max=%.3f "
                   "| STOCK=%.3f -> OURS=%.3f%s%s",
                   mode, num, pick, scoopStick == 0 ? "L" : scoopStick == 1 ? "R" : "-",
                   pAngle, pTotal, arcVel, sweep, rawTrack, trackVel, peakVel,
                   src, vel, minM, maxM, stock, out, g_scoopFix ? "" : "  (measure only -- stock returned)",
                   unmeasured ? "  <-- NO SWEEP MEASURED, neutralised (was a raw-arc max)" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[scoop] caught fatal reading scoop input -> returning stock");
        return stock;
    }
    return out;
}

// ------------------------------------------------------------------ install + menu
void ScoopSpeed_Install() {
    g_startScoop = TwkScanExe(SIG_SCOOP_SPEED);
    if (!g_startScoop) { TwkLog("[scoop] GetBoardRotationSpeedMultiplier sig NOT FOUND -- scoop fix off (game updated?)"); return; }
    if (MH_CreateHook(g_startScoop, (void*)&hkScoopSpeed, &g_origScoop) != MH_OK ||
        MH_EnableHook(g_startScoop) != MH_OK) {
        TwkLog("[scoop] hook failed on GetBoardRotationSpeedMultiplier -- scoop fix off");
        g_startScoop = nullptr; return;
    }
    g_startTick = TwkScanExe(SIG_INPUT_TICK);
    if (!g_startTick) { TwkLog("[scoop] InputHandler::Tick sig NOT FOUND -- tracker off, arc fallback only"); g_useTracker = 0; }
    else if (MH_CreateHook(g_startTick, (void*)&hkInputTick, &g_origTick) != MH_OK ||
             MH_EnableHook(g_startTick) != MH_OK) {
        TwkLog("[scoop] InputHandler::Tick hook failed -- tracker off, arc fallback only");
        g_startTick = nullptr; g_useTracker = 0;
    }
    TwkLog("[scoop] installed (speed fn @ %p, tick @ %p)", g_startScoop, g_startTick);
}

bool ScoopSpeed_Enabled() { return g_scoopFix != 0; }
void ScoopSpeed_SetEnabled(bool on) { g_scoopFix = on ? 1 : 0; TwkMarkDirty(); }
// The shipped calibration (VelMin 350 / VelMax 1600) is the default: it is measured from real play,
// so "reset" restores the tuned values rather than something neutral.
void ScoopSpeed_ResetDefaults() {
    g_scoopFix = 1; g_scoopLog = 1; g_useTracker = 1; g_normArc = 1;
    g_velMin = 350.0f; g_velMax = 1600.0f;
    g_minAngle = 45.0f; g_minTime = 0.050f; g_trackWindow = 0.700f; g_trackMinMag = 0.50f;
    g_scoopElem = -1;
    TwkMarkDirty();
}
float ScoopSpeed_VelMin() { return g_velMin; }
float ScoopSpeed_VelMax() { return g_velMax; }
// The pair must stay ordered whichever end the user drags, or the normalisation divides by <= 0.
void ScoopSpeed_SetVelMin(float v) { g_velMin = v; if (g_velMax <= g_velMin) g_velMax = g_velMin + 1.0f; TwkMarkDirty(); }
void ScoopSpeed_SetVelMax(float v) { g_velMax = v; if (g_velMax <= g_velMin) g_velMin = g_velMax - 1.0f; TwkMarkDirty(); }

void ScoopSpeed_DrawMenu(const OmpMenuApi* api) {
    char b[192];
    bool fix = g_scoopFix != 0;
    if (api->Checkbox("Scoop speed fix", &fix)) { g_scoopFix = fix ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(off = stock, for A/B)");
    if (fix) {
        api->Indent();
        bool trk = g_useTracker != 0;
        if (api->Checkbox("Measure the stick directly", &trk)) { g_useTracker = trk ? 1 : 0; TwkMarkDirty(); }
        bool nrm = g_normArc != 0;
        if (api->Checkbox("Normalise by gesture size", &nrm)) { g_normArc = nrm ? 1 : 0; TwkMarkDirty(); }
        float lo = g_velMin, hi = g_velMax;
        bool ch = api->SliderFloat("Slowest at (deg/s)", &lo, 50.0f, 1500.0f, "%.0f");
        ch     |= api->SliderFloat("Fastest at (deg/s)", &hi, 200.0f, 3000.0f, "%.0f");
        if (ch) { if (hi <= lo) hi = lo + 1.0f; g_velMin = lo; g_velMax = hi; TwkMarkDirty(); }
        api->Unindent();
    }
    const int cnt = (int)g_uiCount;
    if (cnt == 0) api->TextDisabled("last scoop: none yet");
    else {
        const LONG side = g_uiStickSide;
        snprintf(b, sizeof(b), "last scoop #%d (%s):  sweep %.0f deg   rate %.0f -> %.0f deg/s",
                 cnt, side == 0 ? "LEFT stick" : side == 1 ? "RIGHT stick" : "no gesture",
                 (float)g_uiSweep, (float)g_uiRaw, (float)g_uiNorm);
        api->Text(b);
        snprintf(b, sizeof(b), "   game would give %.2fx    we gave %.2fx    %s",
                 (float)g_uiStock, (float)g_uiOurs, g_uiHadArc ? "" : "(no arc - pocket start)");
        api->Text(b);
    }
}
