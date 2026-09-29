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
// =====================================================================================================
// SessionTweaks -- CARVING.
//
// The trigger turn on the real board (USkateboardExMovementComponent::PhysSkateboarding, Epic 0xfca2d0 /
// Steam 0xf8a100, each physics substep) is one line:
//     _currentBankingRatio (+0x758) += (_targetBankingRatio (+0x75c) - current) * clamp(smoothing * dt, 0, 1)
// where the target IS the trigger and smoothing is BankingTightnessSmoothingCurve(tightness) -- 50 per second
// on loose trucks down to 3 on the tightest (NoBanking... on release). At ordinary tightness the lean is where
// the trigger is within a couple of frames: instant, and exactly as linear as the pull. Everything after it
// reads that lean -- the deck's roll on its trucks, the truck physics that steers the board, the body's lean.
//
// So the lean gets real dynamics instead: the trigger is shaped by a progressive curve (a light pull is a
// gentle carve, the last part of the pull tightens it), and the lean follows that as a mass on a spring --
// it builds into the turn over CarveLeanMs and, let go, swings back with a little carry-through
// (CarveFlowPct is its damping; tighter trucks answer a little slower). It is written as both target and
// current for the step, so the game's own follow has nothing left to do and every reader sees the same
// lean; the trigger's value is put back after. Our board only.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "carve.h"
#include "catch_tweaks.h"    // CatchTweaks_Skater
#include "MinHook.h"
#include <cmath>

static const char* SIG_PHYS_SKATE =
    "48 8B C4 55 56 41 55 41 56 41 57 48 8D A8 F8 FE FF FF 48 81 EC E0 01 00 00 0F 29 70 B8 0F 29 78 A8";
typedef void (__fastcall* PhysSkateFn)(void* comp, float dt, uint64_t a3, uint64_t a4);
enum {
    MC_BANK_CUR = 0x758, MC_BANK_TGT = 0x75c,
    MC_BOARD = 0x338,    MC_SKATER = 0x340,
    BOARD_TIGHT_F = 0x4a0, BOARD_TIGHT_B = 0x4a4,   // FSkateboardSettings truck tightness
};

static int   g_on      = 1;       // Carve
static float g_curve   = 160.0f;  // CarveCurvePct -- the trigger's response: 100 = as pulled, higher = progressive
static float g_leanMs  = 320.0f;  // CarveLeanMs -- time to lean all the way in
static float g_flow    = 70.0f;   // CarveFlowPct -- damping: lower swings through more, 100 settles without it
static int   g_log     = 1;       // CarveLog (ini)
static int   g_landProbe = 1;     // CarveLandProbe (ini) -- the landing probe below
// LANDING GIVE: for a moment after each touchdown the trucks are as loose as g_giveTight, so the game's own
// physics tips the board on the landing and the tip turns you -- the lean a looser setup gives that you have to
// steer out of. The board's lean is held by a PID whose gains ApplyForceOnBoard blends from PIDSettingsBanking-
// TruckMin (P 1750) to Max (P 25000) by tightness, every step; tight trucks damp the tip before it can turn you.
static int   g_give     = 1;       // CarveLandGive
static float g_giveTight = 50.0f;  // CarveLandGiveTightPct -- the trucks' tightness right after a landing (%)
static float g_giveMs   = 700.0f;  // CarveLandGiveMs -- back to your own tightness over this
static int   g_ok      = 1;
static void* g_orig    = nullptr;

static void Clamp() {
    if (g_giveTight < 0.0f) g_giveTight = 0.0f; else if (g_giveTight > 100.0f) g_giveTight = 100.0f;
    if (g_giveMs < 100.0f) g_giveMs = 100.0f; else if (g_giveMs > 2000.0f) g_giveMs = 2000.0f;
    if (g_curve < 100.0f) g_curve = 100.0f; else if (g_curve > 300.0f) g_curve = 300.0f;
    if (g_leanMs < 60.0f) g_leanMs = 60.0f; else if (g_leanMs > 1200.0f) g_leanMs = 1200.0f;
    if (g_flow < 30.0f) g_flow = 30.0f; else if (g_flow > 150.0f) g_flow = 150.0f;
}
void Carve_ReadConfig(const char* buf) {
    g_on     = TwkIniInt(buf, "Carve", 1) ? 1 : 0;
    g_curve  = (float)TwkIniInt(buf, "CarveCurvePct", 160);
    g_leanMs = (float)TwkIniInt(buf, "CarveLeanMs", 320);
    g_flow   = (float)TwkIniInt(buf, "CarveFlowPct", 70);
    g_log    = TwkIniInt(buf, "CarveLog", 1);
    g_landProbe = TwkIniInt(buf, "CarveLandProbe", 1) ? 1 : 0;
    g_give      = TwkIniInt(buf, "CarveLandGive", 1) ? 1 : 0;
    g_giveTight = (float)TwkIniInt(buf, "CarveLandGiveTightPct", 50);
    g_giveMs    = (float)TwkIniInt(buf, "CarveLandGiveMs", 700);
    Clamp();
    TwkLog("[carve] config: %s | trigger curve %.2f, lean-in %.0f ms, flow (damping) %.2f | landing give %s: "
           "trucks at %.0f%% after a landing, back over %.0f ms",
           g_on ? "ON" : "off", g_curve / 100.0f, g_leanMs, g_flow / 100.0f, g_give ? "ON" : "off", g_giveTight, g_giveMs);
}
void Carve_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "Carve", g_on);
    TwkIniSetInt(buf, cap, "CarveCurvePct", (int)lroundf(g_curve));
    TwkIniSetInt(buf, cap, "CarveLeanMs", (int)lroundf(g_leanMs));
    TwkIniSetInt(buf, cap, "CarveFlowPct", (int)lroundf(g_flow));
    TwkIniSetInt(buf, cap, "CarveLog", g_log);
    TwkIniSetInt(buf, cap, "CarveLandGive", g_give);
    TwkIniSetInt(buf, cap, "CarveLandGiveTightPct", (int)lroundf(g_giveTight));
    TwkIniSetInt(buf, cap, "CarveLandGiveMs", (int)lroundf(g_giveMs));
}
void Carve_ResetDefaults() { g_on = 1; g_curve = 160.0f; g_leanMs = 320.0f; g_flow = 70.0f; g_log = 1;
                             g_give = 1; g_giveTight = 50.0f; g_giveMs = 700.0f; }

// ---- the lean
static double NowS() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)f.QuadPart;
}
static float  s_x = 0.0f, s_v = 0.0f;     // the lean and how fast it is moving
static float  s_wrote = 0.0f;             // what we left in the current ratio
static double s_lastT = 0.0, s_logT = 0.0;
static bool   s_live = false;
static float  s_peak = 0.0f, s_trigPeak = 0.0f;

// Before the step: the lean for this step, as target and current. Returns true when written.
static bool Lean(void* comp, float dt, float* rawOut) {
    void* sk = twkP(comp, MC_SKATER);
    if (!sk || sk != CatchTweaks_Skater()) return false;             // our board only
    float* cur = (float*)((uint8_t*)comp + MC_BANK_CUR);
    float* tgt = (float*)((uint8_t*)comp + MC_BANK_TGT);
    const float raw = *tgt;
    if (!(raw >= -1.5f && raw <= 1.5f) || !(*cur >= -1.5f && *cur <= 1.5f)) return false;
    const double now = NowS();
    // A new ride (a gap: in the air, off the board) or someone else setting the lean (a reset): start from it.
    if (!s_live || now - s_lastT > 0.1 || fabsf(*cur - s_wrote) > 0.02f) { s_x = *cur; s_v = 0.0f; }
    s_lastT = now; s_live = true;
    if (!(dt > 0.0f) || dt > 0.05f) dt = 0.0055f;
    // The trigger, shaped: gentle at first, the last of the pull tightens it.
    const float a = fabsf(raw) > 1.0f ? 1.0f : fabsf(raw);
    const float want = (raw < 0.0f ? -1.0f : 1.0f) * powf(a, g_curve / 100.0f);
    // A mass on a spring toward it. 95% of the way in at about CarveLeanMs; tighter trucks answer slower.
    void* bd = twkP(comp, MC_BOARD);
    float tight = bd ? 0.5f * (twkF(bd, BOARD_TIGHT_F) + twkF(bd, BOARD_TIGHT_B)) : 0.3f;
    if (!(tight >= 0.0f && tight <= 1.0f)) tight = 0.3f;
    const float w = 4.7f / (g_leanMs / 1000.0f) * (1.15f - 0.5f * tight);
    const float z = g_flow / 100.0f;
    const float acc = w * w * (want - s_x) - 2.0f * z * w * s_v;
    s_v += acc * dt;
    s_x += s_v * dt;
    if (s_x > 1.0f) { s_x = 1.0f; if (s_v > 0.0f) s_v = 0.0f; }
    else if (s_x < -1.0f) { s_x = -1.0f; if (s_v < 0.0f) s_v = 0.0f; }
    *tgt = s_x; *cur = s_x; s_wrote = s_x;
    *rawOut = raw;
    if (g_log) {
        if (fabsf(s_x) > s_peak) s_peak = fabsf(s_x);
        if (a > s_trigPeak) s_trigPeak = a;
        if (now - s_logT > 1.0 && (s_peak > 0.05f || s_trigPeak > 0.05f)) {
            TwkLog("[carve] trigger %.0f%% (peak %.0f%%) -> shaped %.0f%%, lean now %.0f%% (peak %.0f%%), moving %.2f/s | trucks %.2f",
                   a * 100.0f, s_trigPeak * 100.0f, fabsf(want) * 100.0f, fabsf(s_x) * 100.0f, s_peak * 100.0f, s_v, tight);
            s_logT = now; s_peak = 0.0f; s_trigPeak = 0.0f;
        }
    }
    return true;
}
// ---- LANDING PROBE (measurement, CarveLandProbe): for 0.8 s after each touchdown, every ~40 ms, the trucks'
// tightness, the trigger, the lean the game holds (before carve writes it), carve's own, and the deck's real roll
// and heading -- how the lean a landing leaves behind turns the board, at each tightness.
static double s_landT = -1.0, s_probeLastT = 0.0, s_probeLogT = 0.0;
static float  s_yaw0 = 0.0f;
static bool DeckAngles(void* comp, float* roll, float* yaw) {
    void* bd = twkP(comp, MC_BOARD);
    void* fl = bd ? twkP(bd, 0x4e8) : nullptr;                         // ASkateboardEx::_flipper, the deck
    float q[4];
    if (!fl || !TwkCompQuat(fl, q)) return false;
    const float X[3] = { 1.0f, 0.0f, 0.0f }, Y[3] = { 0.0f, 1.0f, 0.0f }, Z[3] = { 0.0f, 0.0f, 1.0f };
    float f[3], sd[3], up[3];
    TwkQuatRotate(q, X, f); TwkQuatRotate(q, Y, sd); TwkQuatRotate(q, Z, up);
    const float d = f[2];
    const float r[3] = { -f[0] * d, -f[1] * d, 1.0f - f[2] * d };        // world up, the long axis taken out
    const float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (l < 0.05f) return false;
    *roll = atan2f((sd[0] * r[0] + sd[1] * r[1] + sd[2] * r[2]) / l, (up[0] * r[0] + up[1] * r[1] + up[2] * r[2]) / l) * 57.2957795f;
    *yaw = atan2f(f[1], f[0]) * 57.2957795f;
    return true;
}
// Our board's riding steps: a gap of 0.15 s (in the air, off the board) then a step = a touchdown.
static bool Touchdown(void* comp) {
    void* sk = twkP(comp, MC_SKATER);
    if (!sk || sk != CatchTweaks_Skater()) return false;
    const double now = NowS();
    if (now - s_probeLastT > 0.15) {
        float roll = 0.0f, yaw = 0.0f;
        DeckAngles(comp, &roll, &yaw);
        s_landT = now; s_yaw0 = yaw; s_probeLogT = 0.0;
    }
    s_probeLastT = now;
    return true;
}
static void LandProbe(void* comp) {
    const double now = NowS();
    float roll = 0.0f, yaw = 0.0f;
    const bool deck = DeckAngles(comp, &roll, &yaw);
    if (s_landT < 0.0 || now - s_landT > 0.8 || now - s_probeLogT < 0.04) return;
    s_probeLogT = now;
    void* bd = twkP(comp, MC_BOARD);
    const float tight = bd ? 0.5f * (twkF(bd, BOARD_TIGHT_F) + twkF(bd, BOARD_TIGHT_B)) : -1.0f;
    float dy = yaw - s_yaw0;
    while (dy > 180.0f) dy -= 360.0f; while (dy < -180.0f) dy += 360.0f;
    TwkLog("[carve] land +%3.0f ms trucks %.2f | trigger %+.2f  game lean %+.2f  carve %+.2f | deck roll %+5.1f  turned %+6.1f%s",
           (now - s_landT) * 1000.0, tight, twkF(comp, MC_BANK_TGT), twkF(comp, MC_BANK_CUR), s_x, roll, dy,
           deck ? "" : " (no deck)");
}

// The give's envelope: all of it for the first third, then eased back to the rider's own trucks.
static float GiveNow() {
    if (!g_give || s_landT < 0.0) return 0.0f;
    const float t = (float)(NowS() - s_landT), T = g_giveMs / 1000.0f, hold = T / 3.0f;
    if (t >= T) return 0.0f;
    if (t <= hold) return 1.0f;
    const float k = (t - hold) / (T - hold);
    return 1.0f - k * k * (3.0f - 2.0f * k);
}
static void __fastcall hkPhysSkate(void* comp, float dt, uint64_t a3, uint64_t a4) {
    bool wrote = false; float raw = 0.0f;
    bool mine = false;
    if (g_ok && comp) { __try { mine = Touchdown(comp); } __except (EXCEPTION_EXECUTE_HANDLER) { mine = false; } }
    if (mine && g_landProbe) { __try { LandProbe(comp); } __except (EXCEPTION_EXECUTE_HANDLER) { g_landProbe = 0; } }
    if (g_on && g_ok && comp) {
        __try { wrote = Lean(comp, dt, &raw); } __except (EXCEPTION_EXECUTE_HANDLER) { wrote = false; g_ok = 0; TwkLog("[carve] caught a fault -- carving off for this session"); }
    }
    // The landing give, around this step only (carve's own spring above read the rider's real trucks).
    float* tf = nullptr; float keep[2] = { 0.0f, 0.0f };
    if (mine) {
        __try {
            const float env = GiveNow();
            void* bd = env > 0.0f ? twkP(comp, MC_BOARD) : nullptr;
            if (bd) {
                tf = (float*)((uint8_t*)bd + BOARD_TIGHT_F);
                keep[0] = tf[0]; keep[1] = tf[1];
                const float soft = g_giveTight / 100.0f;
                bool any = false;
                for (int i = 0; i < 2; i++)
                    if (keep[i] > soft && keep[i] <= 1.0f) { tf[i] = keep[i] - (keep[i] - soft) * env; any = true; }
                if (!any) tf = nullptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { tf = nullptr; }
    }
    ((PhysSkateFn)g_orig)(comp, dt, a3, a4);
    if (tf) { __try { tf[0] = keep[0]; tf[1] = keep[1]; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (wrote) {
        __try {
            float* tgt = (float*)((uint8_t*)comp + MC_BANK_TGT);
            const float cur = *(float*)((uint8_t*)comp + MC_BANK_CUR);
            if (*tgt == s_x) *tgt = raw;                                   // the trigger's own value back
            if (cur != s_x) { s_x = cur; s_v = 0.0f; }                     // the game reset it during the step
            s_wrote = cur;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void Carve_Install() {
    uint8_t* at = TwkScanExe(SIG_PHYS_SKATE);
    if (!at) { g_ok = 0; TwkLog("[carve] PhysSkateboarding not found -- carving off (game updated?)"); return; }
    if (MH_CreateHook(at, (void*)&hkPhysSkate, &g_orig) != MH_OK || MH_EnableHook(at) != MH_OK) {
        g_ok = 0; g_orig = nullptr; TwkLog("[carve] hook failed on PhysSkateboarding -- carving off"); return;
    }
    TwkLog("[carve] installed: PhysSkateboarding @ %p (the board's lean each substep)", at);
}

void Carve_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    bool on = g_on != 0;
    if (api->Checkbox("Carve turning", &on)) Carve_SetEnabled(on);
    api->SameLine(); api->TextDisabled("(the lean builds into a turn with weight and flows back out, instead of snapping to the trigger)");
    if (!g_on) return;
    float v = g_leanMs;
    if (api->SliderFloat("Lean-in time (ms)", &v, 60.0f, 1200.0f, "%.0f")) Carve_SetLeanMs(v);
    api->SameLine(); api->TextDisabled("(how long it takes to lean all the way in)");
    v = g_curve / 100.0f;
    if (api->SliderFloat("Trigger curve", &v, 1.0f, 3.0f, "%.2f")) Carve_SetCurvePct(v * 100.0f);
    api->SameLine(); api->TextDisabled("(1 = as pulled; higher = gentle at first, the end of the pull tightens it)");
    v = g_flow;
    if (api->SliderFloat("Flow (%)", &v, 30.0f, 150.0f, "%.0f")) Carve_SetFlowPct(v);
    api->SameLine(); api->TextDisabled("(lower swings through more when you let off; 100+ settles without it)");
}
void Carve_DrawLandMenu(const OmpMenuApi* api) {
    if (!api) return;
    bool gv = g_give != 0;
    if (api->Checkbox("Landing give", &gv)) Carve_SetLandGive(gv);
    api->SameLine(); api->TextDisabled("(for a moment after a landing the trucks are looser, so the board can tip and turn you like looser trucks do)");
    if (!g_give) return;
    float v = g_giveTight / 10.0f;
    if (api->SliderFloat("Trucks after a landing", &v, 0.0f, 10.0f, "%.1f")) { g_giveTight = v * 10.0f; Clamp(); TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the game's tightness scale; no effect on trucks already this loose)");
    v = g_giveMs;
    if (api->SliderFloat("Back to your trucks over (ms)", &v, 100.0f, 2000.0f, "%.0f")) { g_giveMs = v; Clamp(); TwkMarkDirty(); }
}

bool  Carve_Enabled()            { return g_on != 0; }
void  Carve_SetEnabled(bool on)  { g_on = on ? 1 : 0; TwkMarkDirty(); }
float Carve_LeanMs()             { return g_leanMs; }
void  Carve_SetLeanMs(float ms)  { g_leanMs = ms; Clamp(); TwkMarkDirty(); }
float Carve_CurvePct()           { return g_curve; }
void  Carve_SetCurvePct(float p) { g_curve = p; Clamp(); TwkMarkDirty(); }
float Carve_FlowPct()            { return g_flow; }
void  Carve_SetFlowPct(float p)  { g_flow = p; Clamp(); TwkMarkDirty(); }
bool  Carve_LandGive()           { return g_give != 0; }
void  Carve_SetLandGive(bool on) { g_give = on ? 1 : 0; TwkMarkDirty(); }
