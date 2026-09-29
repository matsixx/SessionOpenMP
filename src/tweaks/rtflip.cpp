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
// SessionTweaks -- REAL-TIME FLIPS (a test).
//
// The board in the air is a rigid body the game torques toward a target rotation (ApplyPIDRotation toward
// GetLocalAnimatorBoardQuat, built from _localAnimationRotator). A flick pop pops a plain ollie; then a
// sideways flick of the OTHER stick starts the board spinning about its length at a speed set by how hard
// the flick was (the flip-speed measure). It keeps spinning until you catch it as in the base game -- a fresh
// push of either stick -- then finishes to the next whole turn and stops flat. The spin is added to the target roll for the read (grind_rock's hook),
// so the physics body flips through the game's own PID. Both sticks are hidden from the game while the flip is
// ours (no late trick on top, from the flick or the catch). The body plays the game's own kickflip/heelflip clip timed from the spin (THE
// BODY below); with that off, the feet are procedural (THE FEET): kick, tuck, catch.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "rtflip.h"
#include "pop_probe.h"       // PopProbe_LastFlickPop
#include "catch_tweaks.h"    // CatchTweaks_Skater
#include "foot_place.h"      // FootPlace_AnimInstance
#include "pad_sampler.h"     // PadSampler_Flick
#include "camera_height.h"   // CameraHeight_ViewForward -- the foot goes the way you flicked, on screen
#include "grind_pop.h"       // GrindPop_NameOfFName
#include "catch_sound.h"     // CatchSound_PlayNow -- the catch's foot-on-board sound
#include "catch_level.h"     // CatchLevel_* -- the catch levels the board as the user's catch does
#include "run_out.h"         // RunOut_BailNow -- a bad catch bails (or runs out) as the user's catch does
#include "MinHook.h"
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>

enum { AN_GROUNDED = 0x5fa, AN_L_SOCK_LOC = 0x404, AN_R_SOCK_LOC = 0x41c, MC_SKATER = 0x340,
       AN_L_SOCK_ROT = 0x410, AN_R_SOCK_ROT = 0x428, AN_SKATER = 0x608, SK_MESH = 0x280 };
enum { SK_CUR_TRICK = 0x590, AN_IK_L = 0x3fc, AN_IK_R = 0x400, AN_TRICK_PENDING = 0x310, AN_HAS_CATCH_LOOP = 0x492,
       AN_HAS_TRICK_LOOP = 0x494, AN_BOARD_FLIPPING = 0x495, AN_PLAYRATE = 0x4d4, AN_SET_IDX = 0x4e8,
       AN_TRICKS_DB = 0x638, AN_SETS = 0x640, DB_FLIPTRICKS = 0x2c0, BS_LENGTH = 0x8c,
       UO_CLASS = 0x10, UO_NAME = 0x18, US_SIZE = 0x58, AN_FLIPTRICK = 0x4c8,
       AN_CATCH_MODE = 0x311, AN_CATCH_ORIENT = 0x312, AN_HAS_L_CATCH = 0x313, AN_HAS_R_CATCH = 0x314,
       AN_AUTO_CATCHING = 0x320,
       SK_BOARD = 0x568, BD_AUDIO_TRICK = 0x4c8, BD_FLIP_AUDIO = 0x348, DEF_OTHER_FOOT = 0x268 };
// FAnimNode_BlendSpacePlayer (4.26): BlendWeight, InternalTimeAccumulator (normalized), PlayRate, bLoop, StartPosition, BlendSpace
enum { ND_WEIGHT = 0x1c, ND_TIME = 0x20, ND_RATE = 0x44, ND_LOOP = 0x48, ND_START = 0x4c, ND_BS = 0x50 };

static int   g_on    = 1;        // RtFlip
static float g_gain  = 2.4f;     // RtFlipGainPct/100 -- deg/s of spin per unit of flick speed (flip speed's scale)
static float g_min   = 500.0f;   // RtFlipMinDeg -- the softest flick's spin
static float g_max   = 1800.0f;  // RtFlipMaxDeg -- the hardest
static int   g_sign  = 1;        // RtFlipSign (ini) -- -1 if kickflip and heelflip come out swapped
static float g_lift  = 16.0f;    // RtFlipFootLiftCm -- the front foot's tuck above where it was at the flick
static float g_kick  = 30.0f;    // RtFlipKickCm -- how far the front foot kicks off the side on the hardest flick
static float g_slide = 18.0f;    // RtFlipKickFwdCm -- and forward toward the nose
static float g_roll  = 22.0f;    // RtFlipAnkleDeg -- the ankle's roll as it flicks off the board
static float g_sole  = 45.0f;    // RtFlipSoleDeg -- once off the board, the sole turns out toward the nose
static float g_bLift = 26.0f;    // RtFlipBackLiftCm -- the back foot comes up off the tail
static float g_bFwd  = 8.0f;     // RtFlipBackFwdCm -- and forward toward the middle of the board
static float g_bTilt = 20.0f;    // RtFlipBackTiltDeg -- its sole tilts forward
static int   g_catchMs = 130;    // RtFlipCatchMs -- the feet come down this long before the board is flat
static int   g_anim  = 1;        // RtFlipAnim -- the body plays the game's trick clip (else the procedural feet)
static float g_animTurn = 360.0f; // RtFlipAnimTurnDeg -- the trick clip plays over this much of the spin
static int   g_watch = 1;        // RtFlipWatchGame (ini) -- log the game's own flip tricks too (the catch stage)
static int   g_animFeet = 1;     // !RtFlipFootControl -- 1 = the foot IK off in the air, the clip's feet (the
                                 // look that works); 0 = mid-air foot control (the IK on, the deck's spin taken
                                 // out of the foot targets -- a test)
static int   g_animSwap = 0;     // RtFlipAnimSwap (ini) -- 1 if kickflip and heelflip come out swapped
static int   g_animLog = 1;      // RtFlipAnimLog (ini)
static int   g_sounds = 1;       // RtFlipSounds -- the flip whoosh and the catch sound a game flip trick makes
static float g_pitchDeg = 30.0f; // RtFlipPitchDeg -- the flip's pitch from a flick all the way into the corner
static int   g_pitchSign = 1;    // RtFlipPitchSign (ini) -- -1 if flicking up pitches the wrong way
static int   g_pitchDelayMs = 70; // RtFlipPitchDelayMs -- the pitch starts this long after the flick (the foot gets there)
static int   g_pitchInMs = 220;  // RtFlipPitchInMs -- and eases in over this
static int   g_boardOffsets = 1; // RtFlipBoardOffsets -- the board sits where a game flip trick puts it

void RtFlip_ReadConfig(const char* buf) {
    g_on   = (TWK_RTFLIP && TwkIniInt(buf, "RtFlip", 1)) ? 1 : 0;      // disabled: the ini cannot turn it on
    g_gain = (float)TwkIniInt(buf, "RtFlipGainPct", 240) / 100.0f;
    g_min  = (float)TwkIniInt(buf, "RtFlipMinDeg", 500);
    g_max  = (float)TwkIniInt(buf, "RtFlipMaxDeg", 1800);
    g_sign = TwkIniInt(buf, "RtFlipSign", 1) < 0 ? -1 : 1;
    g_lift = (float)TwkIniInt(buf, "RtFlipFootLiftCm", 16);
    g_kick = (float)TwkIniInt(buf, "RtFlipKickCm", 30);
    g_slide = (float)TwkIniInt(buf, "RtFlipKickFwdCm", 18);
    g_roll = (float)TwkIniInt(buf, "RtFlipAnkleDeg", 22);
    g_sole = (float)TwkIniInt(buf, "RtFlipSoleDeg", 45);
    g_bLift = (float)TwkIniInt(buf, "RtFlipBackLiftCm", 26);
    g_bFwd = (float)TwkIniInt(buf, "RtFlipBackFwdCm", 8);
    g_bTilt = (float)TwkIniInt(buf, "RtFlipBackTiltDeg", 20);
    g_catchMs = TwkIniInt(buf, "RtFlipCatchMs", 130);
    g_anim = TwkIniInt(buf, "RtFlipAnim", 1) ? 1 : 0;
    g_animTurn = (float)TwkIniInt(buf, "RtFlipAnimTurnDeg", 360);
    if (g_animTurn < 90.0f) g_animTurn = 90.0f;
    g_watch = TwkIniInt(buf, "RtFlipWatchGame", 1) ? 1 : 0;
    g_animFeet = TwkIniInt(buf, "RtFlipFootControl", 0) ? 0 : 1;
    g_animSwap = TwkIniInt(buf, "RtFlipAnimSwap", 0) ? 1 : 0;
    g_animLog = TwkIniInt(buf, "RtFlipAnimLog", 1);
    g_sounds = TwkIniInt(buf, "RtFlipSounds", 1) ? 1 : 0;
    g_pitchDeg = (float)TwkIniInt(buf, "RtFlipPitchDeg", 30);
    g_pitchSign = TwkIniInt(buf, "RtFlipPitchSign", 1) < 0 ? -1 : 1;
    g_pitchDelayMs = TwkIniInt(buf, "RtFlipPitchDelayMs", 70);
    g_pitchInMs = TwkIniInt(buf, "RtFlipPitchInMs", 220);
    g_boardOffsets = TwkIniInt(buf, "RtFlipBoardOffsets", 1) ? 1 : 0;
    if (g_max < g_min) g_max = g_min;
    TwkLog("[rtflip] config: %s | spin = flick x %.2f, %.0f..%.0f deg/s | front: kick %.0f out %.0f fwd, ankle %.0f, "
           "sole %.0f, tuck %.0f | back: lift %.0f fwd %.0f tilt %.0f | catch %d ms (only after a flick pop)",
           g_on ? "ON" : "off", g_gain, g_min, g_max, g_kick, g_slide, g_roll, g_sole, g_lift, g_bLift, g_bFwd, g_bTilt,
           g_catchMs);
    TwkLog("[rtflip] body: %s | trick clip over %.0f deg | clip feet %s | swap %d | watch the game's flips %d",
           g_anim ? "the game's trick clip" : "procedural feet", g_animTurn, g_animFeet ? "on" : "off", g_animSwap, g_watch);
}
void RtFlip_SaveConfig(char* buf, size_t cap) {
    if (!TWK_RTFLIP) return;                                            // disabled: no keys in the ini
    TwkIniSetInt(buf, cap, "RtFlip", g_on);
    TwkIniSetInt(buf, cap, "RtFlipGainPct", (int)lroundf(g_gain * 100.0f));
    TwkIniSetInt(buf, cap, "RtFlipMinDeg", (int)lroundf(g_min));
    TwkIniSetInt(buf, cap, "RtFlipMaxDeg", (int)lroundf(g_max));
    TwkIniSetInt(buf, cap, "RtFlipSign", g_sign);
    TwkIniSetInt(buf, cap, "RtFlipFootLiftCm", (int)lroundf(g_lift));
    TwkIniSetInt(buf, cap, "RtFlipKickCm", (int)lroundf(g_kick));
    TwkIniSetInt(buf, cap, "RtFlipKickFwdCm", (int)lroundf(g_slide));
    TwkIniSetInt(buf, cap, "RtFlipAnkleDeg", (int)lroundf(g_roll));
    TwkIniSetInt(buf, cap, "RtFlipSoleDeg", (int)lroundf(g_sole));
    TwkIniSetInt(buf, cap, "RtFlipBackLiftCm", (int)lroundf(g_bLift));
    TwkIniSetInt(buf, cap, "RtFlipBackFwdCm", (int)lroundf(g_bFwd));
    TwkIniSetInt(buf, cap, "RtFlipBackTiltDeg", (int)lroundf(g_bTilt));
    TwkIniSetInt(buf, cap, "RtFlipCatchMs", g_catchMs);
    TwkIniSetInt(buf, cap, "RtFlipAnim", g_anim);
    TwkIniSetInt(buf, cap, "RtFlipAnimTurnDeg", (int)lroundf(g_animTurn));
    TwkIniSetInt(buf, cap, "RtFlipWatchGame", g_watch);
    TwkIniSetInt(buf, cap, "RtFlipFootControl", g_animFeet ? 0 : 1);
    TwkIniSetInt(buf, cap, "RtFlipAnimSwap", g_animSwap);
    TwkIniSetInt(buf, cap, "RtFlipAnimLog", g_animLog);
    TwkIniSetInt(buf, cap, "RtFlipSounds", g_sounds);
    TwkIniSetInt(buf, cap, "RtFlipPitchDeg", (int)lroundf(g_pitchDeg));
    TwkIniSetInt(buf, cap, "RtFlipPitchSign", g_pitchSign);
    TwkIniSetInt(buf, cap, "RtFlipPitchDelayMs", g_pitchDelayMs);
    TwkIniSetInt(buf, cap, "RtFlipPitchInMs", g_pitchInMs);
    TwkIniSetInt(buf, cap, "RtFlipBoardOffsets", g_boardOffsets);
}
void RtFlip_ResetDefaults() { g_on = TWK_RTFLIP ? 1 : 0; g_gain = 2.4f; g_min = 500.0f; g_max = 1800.0f; g_sign = 1; g_lift = 16.0f;
                              g_kick = 30.0f; g_slide = 18.0f; g_roll = 22.0f; g_sole = 45.0f;
                              g_bLift = 26.0f; g_bFwd = 8.0f; g_bTilt = 20.0f; g_catchMs = 130;
                              g_anim = 1; g_animTurn = 360.0f; g_animFeet = 1; g_animSwap = 0; g_animLog = 1; g_watch = 1;
                              g_sounds = 1; g_pitchDeg = 30.0f; g_pitchSign = 1; g_boardOffsets = 1;
                              g_pitchDelayMs = 70; g_pitchInMs = 220; }

// ---- the spin (game thread: the input tick drives it, the physics read and the feet follow it)
enum { S_IDLE, S_SPIN, S_FINISH };
static int    s_state = S_IDLE;
static int    s_stick = -1;              // the flip stick
static float  s_angle = 0.0f, s_rate = 0.0f, s_target = 0.0f;
static float  s_catchRate = 0.0f;        // caught: how fast it goes onto the flat (deg/s, either way)
static double s_lastT = 0.0, s_startT = 0.0;
static float  s_feetW = 0.0f;            // how much the feet are ours
static float  s_flickX = 0.0f;           // the flick's sideways value (its side on screen)
static float  s_power = 0.5f;            // how hard, 0..1 over the spin range
static bool   s_capture = false;
static float  s_cap[2][3];
static float  s_capQ[2][4];              // each foot's rotation at the spin's start (mesh space)
// The feet's rotations are the game's own placement, which rides the spinning deck -- the feet turned with it.
// Ours is written after the game's update and taken back out before the next (it slerps from the socket).
static bool   s_rotWrote = false;
static float  s_rotGame[2][3];
static float  s_side = 1.0f;             // the edge the front foot goes off: +1 the toe edge, -1 the heel
static float  s_down = 0.0f;             // 0 tucked .. 1 on the deck (the catch)
static bool   s_ikWrote = false;
static float  s_ikGame[2];
// The body (THE BODY below).
static int    g_hookOk = 0;
static void*  s_animDef = nullptr;       // the trick the body plays
static void*  s_animMove = nullptr;      // the skater's own trick at the flick (the ollie)
static double s_animGroundT = 0.0;
static double s_popAge = 0.0;            // flick pop -> flip
static float  s_u0 = -1.0f;              // where the trick clip starts
static bool   s_clipDone = false;        // the trick clip has played; the game's flip loop has the body
// The catch, as in the base game: a fresh push of either stick after the flick.
static float  s_prevM[2] = {};           // each stick's deflection last tick
static bool   s_flickBack = false;       // the flip stick has come back in since the flick
static bool   s_hide[2] = {};            // hidden from the game (while ours, then until it comes back in)

static double NowS() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)f.QuadPart;
}
static void Norm(float v[3]) { const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (l > 1e-5f) for (int i = 0; i < 3; i++) v[i] /= l; }
// The feet's frame (mesh space): along = back -> front foot, toes = across the feet toward the toes. Facing your
// toes the left foot is on your left: left->right = cross(up, toes), so toes = (lr.y, -lr.x).
static void FeetFrame(const float cur[2][3], int front, float along[3], float toes[3]) {
    for (int i = 0; i < 3; i++) along[i] = cur[front][i] - cur[1 - front][i];
    along[2] = 0.0f; Norm(along);
    toes[0] = cur[1][1] - cur[0][1]; toes[1] = -(cur[1][0] - cur[0][0]); toes[2] = 0.0f;
    Norm(toes);
}
// The edge a sideways flick sends the front foot off: the way the stick went, on screen (the camera's level right
// taken into mesh space). No camera = by the flick's sign.
static float ScreenSide(void* an, const float toes[3], float flickX, bool* cam) {
    const float sx = flickX > 0.0f ? 1.0f : -1.0f;
    *cam = false;
    float fw[3], qm[4];
    void* sk = twkP(an, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    if (mesh && TwkCompQuat(mesh, qm) && CameraHeight_ViewForward(fw)) {
        const float rw[3] = { -fw[1], fw[0], 0.0f };                 // cross(up, forward)
        float rm[3]; TwkQuatInvRotate(qm, rw, rm);
        const float d = (rm[0] * toes[0] + rm[1] * toes[1]) * sx;
        if (fabsf(d) > 0.1f) { *cam = true; return d > 0.0f ? 1.0f : -1.0f; }
    }
    return sx;
}

static bool NameOf(void* obj, char* out, int cap) { return obj && GrindPop_NameOfFName((uint8_t*)obj + UO_NAME, out, cap); }
static bool IEq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
// The kickflip/heelflip for the stance of the skater's current trick (TRICK_<stance>_Ollie -> TRICK_<stance>_KickFlip).
static void* PickAnimDef(void* an, void* sk, bool kick) {
    char cur[96] = "", want[96];
    void* move = twkP(sk, SK_CUR_TRICK);
    NameOf(move, cur, sizeof(cur));
    char st[8] = "RGS";
    if (!_strnicmp(cur, "TRICK_", 6)) {
        int i = 0;
        for (const char* q = cur + 6; *q && *q != '_' && i < 7; q++) st[i++] = *q;
        st[i] = 0;
    }
    snprintf(want, sizeof(want), "TRICK_%s_%s", st, kick ? "KickFlip" : "HeelFlip");
    void* db = twkP(an, AN_TRICKS_DB);
    void** arr = db ? (void**)twkP(db, DB_FLIPTRICKS) : nullptr;
    const int n = db ? twkI(db, DB_FLIPTRICKS + 8) : 0;
    for (int i = 0; arr && i < n && i < 512; i++) {
        char nm[96];
        if (arr[i] && NameOf(arr[i], nm, sizeof(nm)) && IEq(nm, want)) {
            s_animMove = move;
            TwkLog("[rtflip] trick: %s (the skater's own trick stays %s)", nm, cur[0] ? cur : "?");
            return arr[i];
        }
    }
    TwkLog("[rtflip] trick: no %s among %d tricks (the skater's trick is %s) -- no body clip or flip sounds", want, n,
           cur[0] ? cur : "?");
    return nullptr;
}
// The body keeps the clip through the landing; the game's own trick comes back on a new trick or once landed.
static void AnimExpire(void* an, bool grounded, double now) {
    void* sk = an ? twkP(an, AN_SKATER) : nullptr;
    void* cur = sk ? twkP(sk, SK_CUR_TRICK) : nullptr;
    if (!grounded) s_animGroundT = 0.0; else if (s_animGroundT == 0.0) s_animGroundT = now;
    const char* why = nullptr;
    if (!sk) why = "no skater";
    else if (cur && cur != s_animMove) why = "a new trick";
    else if (s_state == S_IDLE && s_animGroundT > 0.0 && now - s_animGroundT > 0.8) why = "landed";
    if (why) { TwkLog("[rtflip] body: handed back to the game (%s)", why); s_animDef = nullptr; }
}

// ---- the sounds a game flip trick makes. The in-air whoosh: the board starts it at a flip trick's pop
// (ASkateboardEx::StartOnBoardFlippingInAirAudio, the trick read from board+0x4c8 -- an ollie is on its no-whoosh
// list, so ours never got one) and the catch stops it (HandleOnAnimationPlayCatchSound). The catch's own
// foot-on-board sound is catch_sound's cue (the kickflip's catch clip carries none).
static const char* SIG_FLIP_AUDIO_START =
    "40 53 48 81 EC C0 00 00 00 48 83 B9 30 03 00 00 00 48 8B D9 0F 84 ?? ?? ?? ?? 48 89 BC 24 D8 00 00 00 48 8D B9 80 02 00 00";
static const char* SIG_FLIP_AUDIO_STOP =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 48 03 00 00 48 85 C9 ?? ?? 48 8B 01 FF 90 ?? ?? ?? ??";
typedef void (__fastcall* BoardFn)(void* board);
static BoardFn g_flipAudioStart = nullptr, g_flipAudioStop = nullptr;
static bool    s_whoosh = false;
static void*   s_def = nullptr;          // this flip's kickflip/heelflip: its sounds, and the body's clip when that is on
static bool    s_offsets = false;        // the board sits where the kickflip puts it (flick -> landing)
static void*   g_origBoardTarget = nullptr;   // the board-place hook (where the board sits, below); null = not installed
static void Whoosh(bool on) {
    if (on && !g_sounds) return;
    void* sk = CatchTweaks_Skater();
    void* board = sk ? twkP(sk, SK_BOARD) : nullptr;
    if (!board) return;
    __try {
        if (on && g_flipAudioStart && s_def && !s_whoosh) {
            void* saved = twkP(board, BD_AUDIO_TRICK);
            *(void**)((uint8_t*)board + BD_AUDIO_TRICK) = s_def;
            g_flipAudioStart(board);
            *(void**)((uint8_t*)board + BD_AUDIO_TRICK) = saved;
            s_whoosh = twkP(board, BD_FLIP_AUDIO) != nullptr;
            TwkLog("[rtflip] sound: flip whoosh %s", s_whoosh ? "on" : "not started (the board refused it)");
        } else if (!on && s_whoosh) {
            if (g_flipAudioStop) g_flipAudioStop(board);
            s_whoosh = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_whoosh = false; g_flipAudioStart = nullptr; g_flipAudioStop = nullptr;
        TwkLog("[rtflip] sound: caught a fault -- the flip whoosh is off for this session");
    }
}

// ---- the catch, as a game flip trick's: the foot whose stick caught comes down with the board as it goes flat,
// the other foot the trick's OtherFootCatchTime after.
static int    s_catchFoot = -1;          // the foot that caught (its stick)
static double s_flatT = 0.0;             // when the caught board came flat
static float  s_otherFoot = 0.12f;       // the other foot's delay
static bool OtherFootPending(double now) { return s_catchFoot >= 0 && s_flatT > 0.0 && now - s_flatT < s_otherFoot + 0.05; }

static void Done(const char* why) {
    TwkLog("[rtflip] %s after %.0f ms: %.0f deg turned", why, (NowS() - s_startT) * 1000.0, fabsf(s_target != 0.0f ? s_target : s_angle));
    s_state = S_IDLE; s_angle = 0.0f; s_rate = 0.0f; s_target = 0.0f;
    Whoosh(false);
}

// A flick of the other stick after a flick pop: sideways, or into a corner up to 65 deg off sideways (the base
// game's AxisXMaxAngleThreshold) -- the vertical part is the flip's pitch. A stick still out from the last flip
// has to come back in first.
static bool CatchHoldOn() { CatchTweaksParams cp; CatchTweaks_Params(&cp); return cp.holdPose; }
static bool SeeFlick(const float* s, int* kOut, float* xOut, float* yOut, double* ageOut) {
    int cs = -1; double age = 99.0;
    if (!PopProbe_LastFlickPop(&cs, &age) || age > 1.5 || cs < 0) return false;
    const int k = 1 - cs;
    if (s_hide[k]) return false;
    const float x = s[k * 2], y = s[k * 2 + 1], m = sqrtf(x * x + y * y);
    if (m < 0.6f || fabsf(x) < 0.42f * m) return false;
    *kOut = k; *xOut = x; *yOut = y; *ageOut = age;
    return true;
}
// The flip's pitch from the flick's elevation, the base game's way (FlipTricksHandler::GetBoardExtraPitchAngle):
// |sin(elevation)| over 1 - cos(65 deg), through its mild S-curve, times ExtraPitchUp/Down (30).
static float s_pitch = 0.0f, s_pitchPeak = 0.0f;
static float s_pitchNow = 0.0f;           // the pitch held now (the log; the catch levels from it)
static long long s_startQpc = 0, s_lastClickQpc = 0;
static bool   s_airUsed = false;          // a flip was caught (or bailed) this air
static float  s_owed0 = 0.0f;             // the degrees owed at the catch (the foot's descent window)
static double s_catchT = 0.0;             // when it was caught
static float  s_pitchAtCatch = 0.0f;
static bool   s_levelHold = false;        // levelled after the catch, held to the landing
static bool   s_unspinCap = false;        // the deck's mesh-space turn at the flick is taken (the feet's un-spin)
static bool   s_pitchCap = false;         // take the board's pitch at the first read of this flip

static float FlickPitch(float x, float y) {
    const float m = sqrtf(x * x + y * y);
    if (m < 0.01f) return 0.0f;
    float r = (fabsf(y) / m) / 0.5774f;
    r = r > 1.0f ? 1.0f : r;
    // Up into the corner dips the nose, down lifts it (a regular kickflip, confirmed in game).
    return (y > 0.0f ? -1.0f : 1.0f) * g_pitchDeg * r * r * (3.0f - 2.0f * r) * (float)g_pitchSign;
}
static void StartFlip(void* an, double now, int k, float x, float y, float speed, double age) {
    float r = speed * g_gain; if (r < g_min) r = g_min; else if (r > g_max) r = g_max;
    // The edge the front foot goes off decides the trick (heel edge = kickflip) and so the spin's way.
    float cur[2][3], along[3], toes[3]; bool cam = false;
    for (int f = 0; f < 2; f++) for (int i = 0; i < 3; i++) cur[f][i] = twkF(an, (f ? AN_R_SOCK_LOC : AN_L_SOCK_LOC) + i * 4);
    FeetFrame(cur, k, along, toes);
    s_side = ScreenSide(an, toes, x, &cam);
    const bool kick = (s_side < 0.0f) != (g_animSwap != 0);
    s_stick = k; s_rate = r * s_side * (float)g_sign;
    s_angle = 0.0f; s_target = 0.0f; s_state = S_SPIN; s_startT = now; s_capture = true;
    s_flickX = x; s_power = (g_max > g_min) ? (r - g_min) / (g_max - g_min) : 0.5f;
    s_down = 0.0f; s_u0 = -1.0f; s_clipDone = false; s_popAge = age; s_flickBack = false;
    s_pitch = FlickPitch(x, y); s_pitchPeak = sqrtf(x * x + y * y);
    s_pitchCap = true; s_levelHold = false; s_unspinCap = false;
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); s_startQpc = q.QuadPart; }
    TwkLog("[rtflip] flip on the %s stick, flicked %s%s at %.0f -> %s off the %s edge (%s), spinning %.0f deg/s, "
           "pitch %+.0f", k ? "right" : "left", y > 0.3f ? "up-" : y < -0.3f ? "down-" : "", x < 0.0f ? "left" : "right",
           speed, kick ? "kickflip" : "heelflip", s_side > 0.0f ? "toe" : "heel",
           cam ? "on screen" : "no camera: by the flick's sign", s_rate, s_pitch);
    void* sk = twkP(an, AN_SKATER);
    s_def = sk ? PickAnimDef(an, sk, kick) : nullptr;
    s_animDef = (g_anim && g_hookOk) ? s_def : nullptr;
    s_offsets = s_def != nullptr && g_boardOffsets && g_origBoardTarget != nullptr;
    s_catchFoot = -1; s_flatT = 0.0;
    const float of = s_def ? twkF(s_def, DEF_OTHER_FOOT) : -1.0f;
    s_otherFoot = (of > 0.0f && of < 0.6f) ? of : 0.12f;
    Whoosh(true);
}
// A flick in the instant before the board leaves the ground (the pop's crouch-release) is kept for the takeoff;
// seen only on the ground, it used to be dropped.
static bool   s_pend = false;
static double s_pendT = 0.0, s_pendAge = 0.0;
static int    s_pendK = 0;
static float  s_pendX = 0.0f, s_pendY = 0.0f, s_pendSpeed = 0.0f;

// ---- THE CATCH, by the user's catch rules (catch_tweaks' settings, read live). Measured from the nearest flat:
//   short of the next flat by up to CatchManualFlipTolDeg (120) -> snapped on within CatchSnapMs (90), at least
//     the spin's speed, at most CatchSnapMaxBoost (3) x it;
//   past the flat just turned by up to CatchOverBailDeg (70) -> rolled back within CatchOverMs (40);
//   anything else is a bad catch -> a bail, through run_out (its run-out policy applies). Never back before the
//   first whole turn. The foot comes down with the board over the last CatchDescendDeg (90) of it; the board
//   then levels as CatchLevel does, held to the landing. One catch per air (CatchHoldPose).
static void Catch(void* an, int j, double now, const CatchTweaksParams& cp) {
    const float a = fabsf(s_angle), sg = s_rate < 0.0f ? -1.0f : 1.0f;
    const float past = fmodf(a, 360.0f), prev = a - past, ahead = 360.0f - past;
    const float overOk = cp.overBailDeg > 0.0f ? cp.overBailDeg : 180.0f;
    const char* how;
    if (prev >= 360.0f && past <= overOk) {                             // over-rotated: back onto the flat
        s_target = sg * prev;
        s_catchRate = past / (fmaxf(cp.overMs, 10.0f) / 1000.0f);
        how = "over-rotated -- rolled back";
    } else if (ahead <= cp.manualTolDeg) {                              // under-rotated: snapped on
        s_target = sg * (prev + 360.0f);
        const float snap = (ahead <= cp.snapMaxDeg) ? ahead / (fmaxf(cp.snapMs, 10.0f) / 1000.0f) : 0.0f;
        s_catchRate = fminf(fmaxf(fabsf(s_rate), snap), fabsf(s_rate) * fmaxf(cp.snapMaxBoost, 1.0f));
        how = "under-rotated -- snapped on";
    } else {                                                            // too far from flat: a bad catch
        TwkLog("[rtflip] caught with the %s stick at %.0f deg, %.0f ms after the flick -- %.0f past / %.0f short of "
               "flat is outside the catch (over %.0f, under %.0f): bad catch, bail", j ? "right" : "left", a,
               (now - s_startT) * 1000.0, past, ahead, overOk, cp.manualTolDeg);
        s_airUsed = true;
        Done("bad catch");
        s_animDef = nullptr; s_offsets = false;                        // the bail takes the body and the board
        if (void* sk = an ? twkP(an, AN_SKATER) : nullptr) RunOut_BailNow(sk);
        return;
    }
    s_owed0 = fabsf(s_target - s_angle);
    s_state = S_FINISH; s_catchFoot = j; s_catchT = now; s_pitchAtCatch = s_pitchNow; s_airUsed = true;
    TwkLog("[rtflip] caught with the %s stick at %.0f deg, %.0f ms after the flick -> %s %.0f deg to flat at %.0f, "
           "%.0f deg/s", j ? "right" : "left", a, (now - s_startT) * 1000.0, how, s_owed0, fabsf(s_target), s_catchRate);
    // The catch as a game flip trick's: that stick's foot takes the board, the whoosh stops, the foot-on-board sound.
    Whoosh(false);
    if (g_sounds && !CatchSound_PlayNow(twkP(an, AN_SKATER)))
        TwkLog("[rtflip] sound: the catch sound did not play (catch sound off or unavailable)");
}

void RtFlip_TickSticks(float* s) {
    if (!s) return;
    const double now = NowS();
    float dt = (s_lastT > 0.0) ? (float)(now - s_lastT) : 0.0f;
    s_lastT = now;
    if (dt > 0.1f) dt = 0.1f;
    if (!g_on) { if (s_state != S_IDLE) Done("turned off"); s_hide[0] = s_hide[1] = false; return; }
    __try {
        void* an = FootPlace_AnimInstance();
        const bool grounded = an && twkB(an, AN_GROUNDED) != 0;
        float mag[2];
        for (int j = 0; j < 2; j++) mag[j] = sqrtf(s[j * 2] * s[j * 2] + s[j * 2 + 1] * s[j * 2 + 1]);
        if (s_animDef) AnimExpire(an, grounded, now);
        if (s_offsets) {                                     // the flip's board offsets end with the landing
            void* sk = an ? twkP(an, AN_SKATER) : nullptr;
            void* cur = sk ? twkP(sk, SK_CUR_TRICK) : nullptr;
            if (!sk || (grounded && now - s_startT > 0.08) || (cur && cur != s_animMove)) s_offsets = false;
        }
        if (grounded && (s_state == S_IDLE || now - s_startT > 0.08)) { s_airUsed = false; s_levelHold = false; }
        if (s_state == S_IDLE && an && !(s_airUsed && CatchHoldOn())) {
            int k = 0; float x = 0.0f, y = 0.0f; double age = 99.0;
            const bool seen = SeeFlick(s, &k, &x, &y, &age);
            float speed = 0.0f;
            if (seen) { PadFlick pf; if (PadSampler_Flick(k == 1, 0.25f, &pf)) speed = pf.speed * 10.0f; }
            if (grounded) {
                if (seen && age < 0.5 && !s_pend) {
                    s_pend = true; s_pendT = now; s_pendK = k; s_pendX = x; s_pendY = y; s_pendSpeed = speed; s_pendAge = age;
                }
            } else {
                if (seen) StartFlip(an, now, k, x, y, speed, age);
                else if (s_pend && now - s_pendT < 0.3) {
                    TwkLog("[rtflip] the flick came %.0f ms before the takeoff -- flipping from it", (now - s_pendT) * 1000.0);
                    StartFlip(an, now, s_pendK, s_pendX, s_pendY, s_pendSpeed, s_pendAge + (now - s_pendT));
                }
                s_pend = false;
            }
            if (s_pend && now - s_pendT > 0.3) s_pend = false;
            if (s_state != S_IDLE) { s_prevM[0] = mag[0]; s_prevM[1] = mag[1]; }   // a stick already out is not a catch
        }
        if (s_state != S_IDLE) {
            // Grounded in the first 80 ms is the takeoff flickering, not a landing.
            if (grounded && now - s_startT > 0.08) Done("landed before the catch -- put flat");
            else if (!grounded) {
                if (s_state == S_SPIN) {
                    s_angle += s_rate * dt;
                    if (now - s_startT < 0.06 && mag[s_stick] > s_pitchPeak) {   // the flick's peak sets the pitch
                        s_pitchPeak = mag[s_stick]; s_pitch = FlickPitch(s[s_stick * 2], s[s_stick * 2 + 1]);
                    }
                    if (!s_flickBack && mag[s_stick] < 0.3f) s_flickBack = true;
                    // The catch: a fresh push of either stick, or a stick click (click-to-catch), that stick's foot.
                    int cj = -1;
                    for (int j = 0; j < 2 && cj < 0; j++)
                        if (s_prevM[j] < 0.3f && mag[j] >= 0.5f && !(j == s_stick && !s_flickBack)) cj = j;
                    CatchTweaksParams cp; CatchTweaks_Params(&cp);
                    if (cp.clickToCatch) {
                        int cw = 0; long long cq = 0;
                        if (CatchTweaks_LastClick(&cw, &cq) && cq != s_lastClickQpc) {
                            s_lastClickQpc = cq;
                            if (cq > s_startQpc && cw >= 1 && cw <= 2 && cj < 0) cj = cw - 1;
                        }
                    }
                    // The user's gates: the fresh-flick veto (150 ms) and CatchMinSpinDeg of turn first.
                    if (cj >= 0 && (now - s_startT < 0.15 || fabsf(s_angle) < cp.minSpinDeg)) cj = -1;
                    if (cj >= 0) Catch(an, cj, now, cp);
                } else {                                         // caught: onto the flat at the catch's own speed
                    const float d = s_target - s_angle, st = s_catchRate * dt;
                    if (fabsf(d) <= st) { s_angle = s_target; s_flatT = now; s_levelHold = true; Done("caught flat"); }
                    else s_angle += d > 0.0f ? st : -st;
                }
            }
        }
        // The game sees neither stick while the flip is ours, nor one still out after it, until it comes back in.
        for (int j = 0; j < 2; j++) {
            s_prevM[j] = mag[j];
            if (s_state != S_IDLE) s_hide[j] = true;
            else if (s_hide[j] && mag[j] < 0.3f) s_hide[j] = false;
            if (s_hide[j]) { s[j * 2] = 0.0f; s[j * 2 + 1] = 0.0f; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_state = S_IDLE; s_angle = 0.0f; s_hide[0] = s_hide[1] = false; }
}

void* RtFlip_TrickDef() { return (s_state == S_SPIN && s_def && !g_animFeet) ? s_def : nullptr; }

float RtFlip_BoardRoll(void* comp) {
    if (s_state == S_IDLE || !comp) return 0.0f;
    void* sk = twkP(comp, MC_SKATER);
    if (!sk || sk != CatchTweaks_Skater()) return 0.0f;
    return fmodf(s_angle, 360.0f);
}
// The flip's pitch, levelled out as the caught board comes flat. It comes in after the flick and eases in, as the
// game's own does (the flick's extra pitch rides the trick's END pitch phase, eased along its curve) -- all at
// once at the flick read as the board pitching before the foot got to it.
// The pitch is HELD, not added: the game's own pitch (the ollie's pop-up, then its nose-down end phase) keeps
// running under an added one, so a straight flick was not level and a nose-up corner flick sagged. From where
// the board was at the flick it eases to the flick's pitch (straight = level), holds it, levels out as the
// caught board comes flat and then hands back to the game's own pitch over 150 ms.
enum { MC_LOCAL_PITCH = 0x620 };
static float  s_pitch0 = 0.0f;            // the board's pitch at the flick
float RtFlip_BoardPitch(void* comp) {
    if (!comp) return 0.0f;
    void* sk = twkP(comp, MC_SKATER);
    if (!sk || sk != CatchTweaks_Skater()) return 0.0f;
    const float game = twkF(comp, MC_LOCAL_PITCH);
    if (!(game > -1000.0f && game < 1000.0f)) return 0.0f;
    const double now = NowS();
    // Levelled from the catch, as CatchLevel does (its target and time constant; off = level to 0), and held
    // there to the landing -- the ollie's own nose-down end phase would take a caught board otherwise.
    const bool lvlOn = CatchLevel_Enabled();
    float lvl = lvlOn ? CatchLevel_TargetDeg() : 0.0f;
    if (!(lvl > -90.0f && lvl < 90.0f)) lvl = 0.0f;                     // -999 = "the trick's authored" -> flat
    const float tau = fmaxf((lvlOn ? CatchLevel_ResponseMs() : 60.0f) / 1000.0f, 0.01f);
    if (s_state == S_IDLE) {
        if (!s_levelHold) return 0.0f;
        const float w = lvl + (s_pitchAtCatch - lvl) * expf(-(float)(now - s_catchT) / tau);
        s_pitchNow = w;
        return w - game;
    }
    if (s_pitchCap) { s_pitch0 = game; s_pitchCap = false; }
    float want;
    if (s_state == S_FINISH) {
        want = lvl + (s_pitchAtCatch - lvl) * expf(-(float)(now - s_catchT) / tau);
    } else {
        float in = ((float)(now - s_startT) - (float)g_pitchDelayMs / 1000.0f) / fmaxf((float)g_pitchInMs / 1000.0f, 0.01f);
        in = in < 0.0f ? 0.0f : in > 1.0f ? 1.0f : in;
        in = in * in * (3.0f - 2.0f * in);
        want = s_pitch0 + (s_pitch - s_pitch0) * in;
    }
    s_pitchNow = want;
    return want - game;
}

void RtFlip_PreAnchors(void* an) {
    if (s_ikWrote && an) { *(float*)((uint8_t*)an + AN_IK_L) = s_ikGame[0]; *(float*)((uint8_t*)an + AN_IK_R) = s_ikGame[1]; }
    s_ikWrote = false;
    if (!s_rotWrote || !an) return;
    const int off[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
    for (int f = 0; f < 2; f++) for (int i = 0; i < 3; i++) *(float*)((uint8_t*)an + off[f] + i * 4) = s_rotGame[f][i];
    s_rotWrote = false;
}
static void Nlerp(const float a[4], const float b[4], float t, float out[4]) {
    const float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float s = dot < 0.0f ? -1.0f : 1.0f;
    float n = 0.0f;
    for (int i = 0; i < 4; i++) { out[i] = a[i] + (s * b[i] - a[i]) * t; n += out[i] * out[i]; }
    n = sqrtf(n);
    for (int i = 0; i < 4; i++) out[i] = n > 1e-6f ? out[i] / n : (i == 3 ? 1.0f : 0.0f);
}
// ---- THE FEET, procedural: a kickflip's legs, driven by the live flip (mesh space, where the sockets live).
//   KICK   -- the front foot slides up toward the nose, then kicks off the side you flicked (on screen, the way
//             the stick went: the heel edge for a kickflip, the toe edge for a heelflip), rising as it goes, the
//             ankle rolling into the flick. Once it leaves the board the sole turns out toward the nose. A harder
//             flick kicks further and faster. It stays out while the board turns.
//   BACK   -- the back foot comes up off the tail and forward toward the middle, its sole tilting forward.
//   CATCH  -- from the angle the board has left to turn and its speed, the feet come down onto the game's own
//             placement (the deck) as it goes flat, so the hand-back after the catch is seamless.
// Front foot = the flip stick's (the left stick moves the left foot). The frame is taken at the flick: along the
// feet (back -> front), across them toward the toes, and up.
static float s_along[3], s_toes[3];
static float Ease(float u) { u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u; return u * u * (3.0f - 2.0f * u); }
static float EaseOut(float u) { u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u; const float v = 1.0f - u; return 1.0f - v * v * v; }
// A mesh-space rotation turning the sole (facing down) toward the level direction `dir` by `deg`. The turn's
// sense is checked rather than derived, so the sole cannot come out facing backward.
static void SoleToward(const float dir[3], float deg, float out[4]) {
    const float axis[3] = { -dir[1], dir[0], 0.0f };
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    float q[4], v[3];
    TwkQuatAxisAngle(axis, 30.0f, q); TwkQuatRotate(q, down, v);
    TwkQuatAxisAngle(axis, (v[0] * dir[0] + v[1] * dir[1]) >= 0.0f ? deg : -deg, out);
}

static void TakeFrame(const float cur[2][3]) {
    FeetFrame(cur, s_stick, s_along, s_toes);
    TwkLog("[rtflip] feet: front = %s foot, power %.2f, the kick goes off the %s edge", s_stick ? "right" : "left",
           s_power, s_side > 0.0f ? "toe" : "heel");
}

// How close the board is to flat: the feet come down over the last g_catchMs of the turn. Landed or caught = all
// the way down (rate-limited, so landing mid-turn does not snap).
static float CatchDown(float dt) {
    float want = 1.0f;
    if (s_state == S_SPIN) want = 0.0f;
    else if (s_state == S_FINISH && fabsf(s_rate) > 1.0f) {
        const float left = fabsf(s_target - s_angle) / fmaxf(s_catchRate, 1.0f);   // s to flat
        want = 1.0f - left / fmaxf(0.02f, (float)g_catchMs / 1000.0f);
    }
    want = want < 0.0f ? 0.0f : want > 1.0f ? 1.0f : want;
    s_down += fmaxf(-dt / 0.06f, fminf(dt / 0.06f, want - s_down));
    return Ease(s_down);
}
// The body's feet: the clip's own in the air (the foot IK off); at the catch the catching foot's IK comes back
// with the board as it goes flat, the other foot's after its delay; landed = both. Taken back out before the
// game's next foot update, like the rotations.
static void AnimFeet(void* an, float dt) {
    const float down = CatchDown(dt);
    if (!g_animFeet) return;
    const double now = NowS();
    const bool gnd = twkB(an, AN_GROUNDED) != 0;
    float planted[2];
    for (int f = 0; f < 2; f++) {
        if (gnd) planted[f] = 1.0f;
        else if (s_catchFoot < 0) planted[f] = s_state == S_IDLE ? down : 0.0f;          // uncaught
        else if (f == s_catchFoot) {
            if (s_state == S_FINISH) {
                CatchTweaksParams cp; CatchTweaks_Params(&cp);
                const float win = cp.footDescends ? fminf(s_owed0, cp.descendDeg) : s_owed0;
                const float owed = fabsf(s_target - s_angle);
                float r = win > 1.0f ? 1.0f - owed / win : 1.0f;
                r = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
                planted[f] = r * r * (3.0f - 2.0f * r);
            } else planted[f] = s_state == S_IDLE ? 1.0f : 0.0f;
        }
        else planted[f] = s_flatT > 0.0 ? Ease((float)(now - s_flatT) / fmaxf(s_otherFoot, 0.02f)) : 0.0f;
    }
    float* a = (float*)((uint8_t*)an + AN_IK_L);
    s_ikGame[0] = a[0]; s_ikGame[1] = a[1];
    for (int f = 0; f < 2; f++) a[f] *= 1.0f - s_feetW * (1.0f - planted[f]);
    s_ikWrote = true;
}

// ---- THE FEET ON THE GAME'S OWN PLACEMENT. In a flip trick the game keeps the foot IK on and places the feet
// from the animation (measured on its own kickflip: the sockets ride the flicking leg, not the spinning deck) --
// and with the kickflip clip on the body it does the same for ours, so the IK stays on and mid-air foot control
// (foot_steer, the socket offsets) reaches the feet. At the flick each foot's spot is taken in the DECK's frame;
// at the catch the catching foot is brought back onto it as the board comes flat, the other after its delay, and
// both are kept there to the landing (the clip's tuck would otherwise hold them up until touchdown).
enum { UC_POS = 0x1d0, UC_SCALE = 0x1e0 };                          // ComponentToWorld translation / scale
static float s_deckLocal[2][3], s_deckLocalQ[2][4];
static bool  s_deckOk = false;
static bool CompXform(void* comp, float q[4], float p[3], float* sc) {
    if (!comp || !TwkCompQuat(comp, q)) return false;
    for (int i = 0; i < 3; i++) p[i] = twkF(comp, UC_POS + i * 4);
    if (sc) { *sc = twkF(comp, UC_SCALE); if (!(*sc > 0.01f && *sc < 100.0f)) *sc = 1.0f; }
    return true;
}
static void QuatConj(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
static void CatchFeet(void* an, float dt, float dL[3], float dR[3]) {
    CatchDown(dt);
    void* sk = twkP(an, AN_SKATER);
    void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
    void* board = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* deck = board ? twkP(board, 0x4e8) : nullptr;                 // ASkateboardEx::_flipper
    float mq[4], mp[3], ms = 1.0f, dq[4], dp[3];
    if (!CompXform(mesh, mq, mp, &ms) || !CompXform(deck, dq, dp, nullptr)) return;
    float mqc[4], dqc[4]; QuatConj(mq, mqc); QuatConj(dq, dqc);
    float* dd[2] = { dL, dR };
    const int off[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC }, roff[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
    float cur[2][3], r[2][3], q[2][4];
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < 3; i++) { cur[f][i] = twkF(an, off[f] + i * 4) + dd[f][i]; r[f][i] = twkF(an, roff[f] + i * 4); }
        TwkRotatorToQuat(r[f], q[f]);
    }
    if (s_capture) {                                                    // the flick: each foot's spot on the deck
        for (int f = 0; f < 2; f++) {
            float v[3] = { cur[f][0] * ms, cur[f][1] * ms, cur[f][2] * ms }, w[3];
            TwkQuatRotate(mq, v, w);
            for (int i = 0; i < 3; i++) w[i] += mp[i] - dp[i];
            TwkQuatInvRotate(dq, w, s_deckLocal[f]);
            float wq[4]; TwkQuatMul(mq, q[f], wq); TwkQuatMul(dqc, wq, s_deckLocalQ[f]);
        }
        s_deckOk = true; s_capture = false;
    }
    if (!s_deckOk) return;
    const double now = NowS();
    const bool gnd = twkB(an, AN_GROUNDED) != 0;
    bool any = false;
    for (int f = 0; f < 2; f++) {
        float planted = 0.0f;
        if (gnd || (s_state == S_IDLE && s_catchFoot < 0)) planted = 1.0f;
        else if (f == s_catchFoot) {
            if (s_state == S_FINISH) {
                CatchTweaksParams cp; CatchTweaks_Params(&cp);
                const float win = cp.footDescends ? fminf(s_owed0, cp.descendDeg) : s_owed0;
                float k = win > 1.0f ? 1.0f - fabsf(s_target - s_angle) / win : 1.0f;
                k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
                planted = k * k * (3.0f - 2.0f * k);
            } else if (s_state == S_IDLE) planted = 1.0f;
        } else if (s_catchFoot >= 0 && s_flatT > 0.0) {
            float k = (float)(now - s_flatT) / fmaxf(s_otherFoot, 0.02f);
            k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
            planted = k * k * (3.0f - 2.0f * k);
        }
        const float wgt = planted * s_feetW;
        if (wgt <= 0.001f) continue;
        float tw[3]; TwkQuatRotate(dq, s_deckLocal[f], tw);
        for (int i = 0; i < 3; i++) tw[i] += dp[i] - mp[i];
        float tm[3]; TwkQuatInvRotate(mq, tw, tm);
        for (int i = 0; i < 3; i++) dd[f][i] += (tm[i] / ms - cur[f][i]) * wgt;
        float t1[4], tq[4]; TwkQuatMul(dq, s_deckLocalQ[f], t1); TwkQuatMul(mqc, t1, tq);
        float qw[4]; Nlerp(q[f], tq, wgt, qw);
        float rw[3]; TwkQuatToRotator(qw, rw);
        if (!s_rotWrote) for (int i = 0; i < 3; i++) s_rotGame[f][i] = r[f][i];   // PreSteer already kept the game's
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)an + roff[f] + i * 4) = rw[i];
        any = true;
    }
    if (any) s_rotWrote = true;
}

// MID-AIR FOOT CONTROL (a test, RtFlipFootControl). With the foot IK on, the feet go where the game's foot
// targets are -- and on our flip those ride the deck anchors, positions AND rotations, round with the spin
// (the log: the back foot's target swung y -27 -> +17, z -28 -> +44; a base-game kickflip's never circle). The
// deck's turn since the flick (mesh space) is taken back out of both here, about the deck's centre, before
// foot_steer lays its offsets and twist on and before the catch blends them; the game's own rotation is put
// back in PreAnchors.
static float s_deckMesh0[4];
static bool FeetOnPlacement() {
    return s_animDef && !g_animFeet && s_feetW > 0.0f &&
           (s_state != S_IDLE || s_levelHold || OtherFootPending(NowS()));
}
void RtFlip_PreSteer(void* an, float dL[3], float dR[3]) {
    if (!an || !s_animDef || g_animFeet) return;
    const bool apply = FeetOnPlacement();
    if (!apply && (s_unspinCap || s_state == S_IDLE)) return;       // the reference is taken on the flip's first frame
    __try {
        void* sk = twkP(an, AN_SKATER);
        void* mesh = sk ? twkP(sk, SK_MESH) : nullptr;
        void* board = sk ? twkP(sk, SK_BOARD) : nullptr;
        void* deck = board ? twkP(board, 0x4e8) : nullptr;
        float mq[4], dq[4], mp[3], dp[3], ms = 1.0f;
        if (!CompXform(mesh, mq, mp, &ms) || !CompXform(deck, dq, dp, nullptr)) return;
        float mqc[4]; QuatConj(mq, mqc);
        float dm[4]; TwkQuatMul(mqc, dq, dm);                          // the deck in mesh space
        if (!s_unspinCap) { for (int i = 0; i < 4; i++) s_deckMesh0[i] = dm[i]; s_unspinCap = true; }
        if (!apply) return;
        float d0c[4]; QuatConj(s_deckMesh0, d0c);
        float delta[4]; TwkQuatMul(dm, d0c, delta);                    // its turn since the flick
        float dc[4]; QuatConj(delta, dc);
        // The deck's centre in mesh space: the positions turn back about it.
        float dpm[3] = { dp[0] - mp[0], dp[1] - mp[1], dp[2] - mp[2] };
        TwkQuatInvRotate(mq, dpm, dpm);
        for (int i = 0; i < 3; i++) dpm[i] /= ms;
        float* dd[2] = { dL, dR };
        const int loff[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC };
        for (int f = 0; f < 2; f++) {
            float v[3];
            for (int i = 0; i < 3; i++) v[i] = twkF(an, loff[f] + i * 4) + dd[f][i] - dpm[i];
            float u[3]; TwkQuatRotate(dc, v, u);
            for (int i = 0; i < 3; i++) dd[f][i] += (u[i] - v[i]) * s_feetW;
        }
        const int roff[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
        for (int f = 0; f < 2; f++) {
            float r[3], q[4];
            for (int i = 0; i < 3; i++) r[i] = twkF(an, roff[f] + i * 4);
            TwkRotatorToQuat(r, q);
            float un[4]; TwkQuatMul(dc, q, un);
            float qw[4]; Nlerp(q, un, s_feetW, qw);
            float rw[3]; TwkQuatToRotator(qw, rw);
            if (!s_rotWrote) for (int i = 0; i < 3; i++) s_rotGame[f][i] = r[i];   // the game's own, for PreAnchors
            for (int i = 0; i < 3; i++) *(float*)((uint8_t*)an + roff[f] + i * 4) = rw[i];
        }
        s_rotWrote = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void RtFlip_AddOffset(void* an, float dt, float dL[3], float dR[3]) {
    if (!an) return;
    const bool active = s_state != S_IDLE || (s_animDef && (OtherFootPending(NowS()) || s_levelHold));
    const float rate = active ? 1.0f / 0.05f : 1.0f / 0.12f;
    s_feetW += fmaxf(-rate * dt, fminf(rate * dt, (active ? 1.0f : 0.0f) - s_feetW));
    if (s_feetW <= 0.0f) return;
    if (s_animDef) {
        __try { if (g_animFeet) AnimFeet(an, dt); else CatchFeet(an, dt, dL, dR); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    __try {
        float* dd[2] = { dL, dR };
        const int off[2] = { AN_L_SOCK_LOC, AN_R_SOCK_LOC };
        float cur[2][3];
        for (int f = 0; f < 2; f++) for (int i = 0; i < 3; i++) cur[f][i] = twkF(an, off[f] + i * 4) + dd[f][i];
        const int roff[2] = { AN_L_SOCK_ROT, AN_R_SOCK_ROT };
        float r[2][3], q[2][4];
        for (int f = 0; f < 2; f++) {
            for (int i = 0; i < 3; i++) r[f][i] = twkF(an, roff[f] + i * 4);
            TwkRotatorToQuat(r[f], q[f]);
        }
        if (s_capture) {
            for (int f = 0; f < 2; f++) {
                for (int i = 0; i < 3; i++) s_cap[f][i] = cur[f][i];
                for (int i = 0; i < 4; i++) s_capQ[f][i] = q[f][i];
            }
            TakeFrame(cur);
            s_down = 0.0f;
            s_capture = false;
        }
        const float down = CatchDown(dt);
        const float up = 1.0f - down;
        // The kick, on the clock since the flick.
        const float t = (float)(NowS() - s_startT);
        const float kickT = 0.14f - 0.06f * s_power;                    // a harder flick kicks out faster...
        const float reach = 0.6f + 0.4f * s_power;                       // ...and further
        const float ahead = EaseOut(t / (kickT * 0.7f));                 // slides up toward the nose first...
        const float out   = Ease((t - kickT * 0.2f) / (kickT * 0.8f));   // ...then off the side, and stays out
        const float flick = EaseOut(t / kickT);                          // the ankle rolling into the flick
        const float gone  = Ease((t - kickT) / 0.08f);                   // left the board: the sole turns out
        const float rise  = EaseOut(t / (kickT * 0.8f));
        const float bRise = EaseOut(t / 0.15f);                          // the back foot comes off the tail
        for (int f = 0; f < 2; f++) {
            float tgt[3];
            for (int i = 0; i < 3; i++) tgt[i] = s_cap[f][i];
            float qTgt[4] = { s_capQ[f][0], s_capQ[f][1], s_capQ[f][2], s_capQ[f][3] };
            // Rotations are mesh space, so they layer on the parent side of the foot's own.
            if (f == s_stick) {
                const float side = g_kick * reach * out * s_side;
                for (int i = 0; i < 3; i++) tgt[i] += s_along[i] * g_slide * reach * ahead + s_toes[i] * side;
                tgt[2] += g_lift * rise + 0.25f * fabsf(g_kick * reach) * out;   // the leg reaches out and up
                // The ankle: rolled about the board's length as it flicks...
                float qa[4], qf[4]; TwkQuatAxisAngle(s_along, g_roll * flick * s_side, qa); TwkQuatMul(qa, s_capQ[f], qf);
                // ...then, off the board, the sole turned out toward the nose.
                float qs[4], qo[4]; SoleToward(s_along, g_sole, qs); TwkQuatMul(qs, s_capQ[f], qo);
                Nlerp(qf, qo, gone, qTgt);
            } else {
                for (int i = 0; i < 3; i++) tgt[i] += s_along[i] * g_bFwd * bRise;
                tgt[2] += g_bLift * bRise;
                float qs[4]; SoleToward(s_along, g_bTilt * bRise, qs);
                TwkQuatMul(qs, s_capQ[f], qTgt);
            }
            // The catch: down onto where the game puts the foot (the deck, nearly flat by now).
            for (int i = 0; i < 3; i++) tgt[i] = tgt[i] * up + cur[f][i] * down;
            float qc[4]; Nlerp(qTgt, q[f], down * down, qc);             // the turn joins the deck's last
            for (int i = 0; i < 3; i++) dd[f][i] += (tgt[i] - cur[f][i]) * s_feetW;
            // The foot keeps the turn it had at the flick (plus the ankle): it never rides the deck round.
            float qw[4]; Nlerp(q[f], qc, s_feetW, qw);
            float rw[3]; TwkQuatToRotator(qw, rw);
            for (int i = 0; i < 3; i++) { s_rotGame[f][i] = r[f][i]; *(float*)((uint8_t*)an + roff[f] + i * 4) = rw[i]; }
        }
        s_rotWrote = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_feetW = 0.0f; }
}

// ---- THE BODY: the game's own trick clip, timed from the spin.
// The anim instance copies the skater's current trick every frame (SetFlipTrick, from NativeUpdateAnimation) and
// plays that trick's blend spaces in its trick state machine (trick -> flip loop -> catch -> land). Around that
// copy the skater's trick reads as the kickflip/heelflip for the stance, so the BODY plays it while the board's
// own trick stays the ollie and the spin stays ours. The trick clip's time is then set from the spin: level with
// the ollie clip it replaces at the flick, on with the board's angle over the first g_animTurn of the spin; then
// the game's own flip loop holds the body until the catch. Each blend space's player node is found in the anim instance by the blend
// space it plays (FAnimNode_BlendSpacePlayer: normalized time +0x20, PlayRate +0x44 -- both set, the play rate
// may be bound to FlipTrickPlaybackRate).
static const char* SIG_SET_FLIP_TRICK =
    "48 8B C4 48 89 58 20 57 48 81 EC C0 00 00 00 0F 29 70 E8 48 8B D9 48 8B 89 08 06 00 00 44 0F 29 48 B8 44 0F 28 C9 44 0F 29 60 88";
typedef void (__fastcall* SetFlipTrickFn)(void* an, float dt);
static void*  g_origSFT = nullptr;
static bool   s_rateOurs = false;
static float  s_gameRate = 1.0f;
static int    s_node[5] = { -1, -1, -1, -1, -1 };    // trick, loop, catch, land, the ollie's trick
static void*  s_nodeBS[5] = {};
static double s_animLogT = 0.0;
static uintptr_t s_exeLo = 0, s_exeHi = 0;

static bool NodeOk(const uint8_t* nd) {
    const uintptr_t vt = *(const uintptr_t*)nd;
    if (s_exeLo && (vt < s_exeLo || vt >= s_exeHi)) return false;      // a node starts with its vtable
    const float t = *(const float*)(nd + ND_TIME), r = *(const float*)(nd + ND_RATE);
    const float st = *(const float*)(nd + ND_START), w = *(const float*)(nd + ND_WEIGHT);
    return t >= 0.0f && t <= 1.001f && fabsf(r) < 50.0f && st >= 0.0f && st <= 1.001f && w >= 0.0f && w <= 1.001f &&
           nd[ND_LOOP] <= 1;
}
static void FindNodes(uint8_t* an, void* const bs[5]) {
    bool need = false;
    for (int k = 0; k < 5; k++) {
        if (s_node[k] >= 0 && s_nodeBS[k] == bs[k] && *(void**)(an + s_node[k] + ND_BS) == bs[k]) continue;
        s_node[k] = -1; s_nodeBS[k] = bs[k];
        if (bs[k]) need = true;
    }
    if (!need) return;
    void* cls = twkP(an, UO_CLASS);
    int size = cls ? twkI(cls, US_SIZE) : 0;
    if (size < 0x900 || size > 0x80000) size = 0x20000;
    for (int off = 0x8a0; off + ND_BS + 8 <= size; off += 8) {
        void* v = *(void**)(an + off + ND_BS);
        if (!v) continue;
        for (int k = 0; k < 5; k++) {
            if (s_node[k] >= 0 || v != bs[k] || !NodeOk(an + off)) continue;
            s_node[k] = off;
            if (g_animLog) {
                char nm[96] = "?"; NameOf(bs[k], nm, sizeof(nm));
                static const char* const kStage[5] = { "trick", "loop", "catch", "land", "ollie trick" };
                TwkLog("[rtflip] body: %s clip %s (%.2f s) plays at anim+0x%x (loop %d, start %.2f)", kStage[k], nm,
                       twkF(bs[k], BS_LENGTH), off, an[off + ND_LOOP], *(float*)(an + off + ND_START));
            }
        }
    }
}
// MEASUREMENT: the deck's roll about its own long axis against world up, signed (0 flat, +-180 upside down) --
// the log's run of it shows which way the board really turns (a heelflip was seen spinning like a kickflip
// while the target turned the other way).
static float DeckRoll() {
    void* sk = CatchTweaks_Skater();
    void* board = sk ? twkP(sk, SK_BOARD) : nullptr;
    void* fl = board ? twkP(board, 0x4e8) : nullptr;                   // ASkateboardEx::_flipper, the deck
    float q[4];
    if (!fl || !TwkCompQuat(fl, q)) return -999.0f;
    const float X[3] = { 1.0f, 0.0f, 0.0f }, Y[3] = { 0.0f, 1.0f, 0.0f }, Z[3] = { 0.0f, 0.0f, 1.0f };
    float fwd[3], side[3], up[3];
    TwkQuatRotate(q, X, fwd); TwkQuatRotate(q, Y, side); TwkQuatRotate(q, Z, up);
    const float d = fwd[2];
    float r[3] = { -fwd[0] * d, -fwd[1] * d, 1.0f - fwd[2] * d };      // world up, the long axis taken out
    const float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (l < 0.05f) return -999.0f;
    const float c = (up[0] * r[0] + up[1] * r[1] + up[2] * r[2]) / l;
    const float sn = (side[0] * r[0] + side[1] * r[1] + side[2] * r[2]) / l;
    return atan2f(sn, c) * 57.2957795f;
}
static void NodeLog(char* out, int cap, const uint8_t* an, int k) {
    if (s_node[k] < 0) { snprintf(out, cap, "--"); return; }
    const uint8_t* nd = an + s_node[k];
    snprintf(out, cap, "w%.2f t%.2f", *(const float*)(nd + ND_WEIGHT), *(const float*)(nd + ND_TIME));
}
static float  s_k = 1.9f;                // the trick clip's normalized time per second at play rate 1 (measured live)
// The trick clip's player RESETS its time to StartPosition when its state is entered (Initialize), after our
// seek -- a lone seek was lost and the clip replayed from the pop (a second pop, a re-crouch, then a race to
// catch up at up to 6x that tripped the state machine's end-of-clip transition early). So its StartPosition is
// our start while it plays (the game's value put back after), and a clip that still lags is seeked, not raced.
static int    s_startOff = -1;           // the node whose StartPosition is ours
static float  s_startOrig = 0.0f;
static void RestoreStart(uint8_t* an) {
    if (s_startOff >= 0) *(float*)(an + s_startOff + ND_START) = s_startOrig;
    s_startOff = -1;
}
static float  s_lastNodeT = -1.0f, s_lastRate = 0.0f;
static void AnimDrive(uint8_t* an, double t0, float dt) {
    const int idx = twkB(an, AN_SET_IDX), n = twkI(an, AN_SETS + 8);
    void** sets = (void**)twkP(an, AN_SETS);
    if (!sets || n <= 0 || idx >= n || !sets[idx]) return;
    void** set = (void**)sets[idx];                                     // FTrickAnimationSet: trick, loop, catch, land
    void* bs[5] = { set[0], set[1], set[2], set[3], nullptr };
    if (n > 1) { void** o = (void**)sets[(idx + 1) % n]; if (o && o[0] != set[0]) bs[4] = o[0]; }
    FindNodes(an, bs);
    float u = -1.0f;
    if (s_state != S_IDLE && s_animDef && !s_clipDone && s_node[0] >= 0) {
        uint8_t* nd = an + s_node[0];
        if (s_u0 < 0.0f) {                                              // level with the ollie clip it replaces
            const float len = bs[0] ? twkF(bs[0], BS_LENGTH) : 0.0f;
            float u0 = s_node[4] >= 0 ? *(float*)(an + s_node[4] + ND_TIME) : -1.0f;
            const char* from = "the ollie clip's time";
            if (!(u0 > 0.0f && u0 < 0.9f)) { u0 = len > 0.05f ? (float)s_popAge / len : 0.2f; from = "the time since the pop"; }
            s_u0 = fminf(fmaxf(u0, 0.0f), 0.9f);
            *(float*)(nd + ND_TIME) = s_u0;                             // the seek; from here it PLAYS
            RestoreStart(an);
            s_startOff = s_node[0]; s_startOrig = *(float*)(nd + ND_START);
            *(float*)(nd + ND_START) = s_u0;                            // ...and the reset lands there too
            s_lastNodeT = -1.0f; s_lastRate = 0.0f;
            TwkLog("[rtflip] body: the trick clip (%.2f s) starts at %.2f (%s)", len, s_u0, from);
        }
        // Through the flick and into the flip with the board; at the end the game's own flip loop takes the body
        // until the catch (the trick state machine moves on by itself once the clip has played). PLAYED, at a rate
        // that keeps it with the board -- a clip that is only set to a time fires none of its sounds, and the
        // ollie clip fading out shares the rate, so a frozen rate cut its pop.
        float nowU = *(float*)(nd + ND_TIME);
        {
            const float want = s_u0 + (0.999f - s_u0) * fminf(fabsf(s_angle) / g_animTurn, 1.0f);
            if (nowU < want - 0.12f) {                                  // restarted (or badly behind): seek
                TwkLog("[rtflip] body: the trick clip was at %.2f, wanted %.2f -- seeked", nowU, want);
                *(float*)(nd + ND_TIME) = want; nowU = want; s_lastNodeT = -1.0f;
            }
        }
        if (s_lastNodeT >= 0.0f && s_lastRate > 0.05f && dt > 0.0f) {       // how far a rate of 1 really moves it
            const float du = nowU - s_lastNodeT;
            if (du > 0.0f && du < 0.5f) s_k += (fminf(fmaxf(du / (s_lastRate * dt), 0.3f), 8.0f) - s_k) * 0.3f;
        }
        const float span = 0.999f - s_u0;
        const float p = fminf(fabsf(s_angle) / g_animTurn, 1.0f);
        u = s_u0 + span * p;
        if (p >= 1.0f || nowU >= 0.99f) {
            s_clipDone = true;
            TwkLog("[rtflip] body: the trick clip has played (%.0f deg, clip at %.2f, %.2f/s per rate) -- the game's "
                   "flip loop until the catch", fabsf(s_angle), nowU, s_k);
        } else {
            // Forward speed of the spin in clip time, plus the error closed within ~1/12 s.
            const float fwd = s_state == S_SPIN ? fabsf(s_rate)
                            : (fabsf(s_target) > fabsf(s_angle) ? s_catchRate : 0.0f);
            float rate = (span / g_animTurn * fwd + (u - nowU) * 12.0f) / s_k;
            rate = fminf(fmaxf(rate, 0.0f), 3.0f);
            *(float*)(nd + ND_RATE) = rate;
            s_gameRate = twkF(an, AN_PLAYRATE);                         // the game's (put back before its next lerp)
            *(float*)(an + AN_PLAYRATE) = rate;
            s_rateOurs = true;
            s_lastRate = rate; s_lastNodeT = nowU;
        }
    }
    const double now = NowS();
    if (g_animLog && now - s_animLogT > 0.04) {
        s_animLogT = now;
        char t[64], l[64], c[64], d[64], o[64];
        NodeLog(t, 64, an, 0); NodeLog(l, 64, an, 1); NodeLog(c, 64, an, 2); NodeLog(d, 64, an, 3); NodeLog(o, 64, an, 4);
        TwkLog("[rtflip] %s %4.0f ms %s ang %4.0f deck %+4.0f pitch %+3.0f u %.2f | trick %s | loop %s | catch %s | land %s | ollie %s | "
               "flags tl%d cl%d flip%d pend%d gnd%d | catch mode%d orient%d L%d R%d auto%d | IK %.2f/%.2f | rate %.2f",
               s_animDef ? "body" : "watch", (now - t0) * 1000.0,
               s_state == S_SPIN ? "FLIP" : s_state == S_FINISH ? "CATCH" : "after", s_angle, DeckRoll(),
               s_state != S_IDLE ? s_pitchNow : 0.0f, u, t, l, c, d, o,
               twkB(an, AN_HAS_TRICK_LOOP), twkB(an, AN_HAS_CATCH_LOOP), twkB(an, AN_BOARD_FLIPPING),
               twkB(an, AN_TRICK_PENDING), twkB(an, AN_GROUNDED), twkB(an, AN_CATCH_MODE), twkB(an, AN_CATCH_ORIENT),
               twkB(an, AN_HAS_L_CATCH), twkB(an, AN_HAS_R_CATCH), twkB(an, AN_AUTO_CATCHING),
               s_ikWrote ? s_ikGame[0] : twkF(an, AN_IK_L), s_ikWrote ? s_ikGame[1] : twkF(an, AN_IK_R),
               s_rateOurs ? s_gameRate : twkF(an, AN_PLAYRATE));
    }
}
// The game's own flip tricks (not ours, not an ollie), watched through the air and just past the landing: the
// stages and the catch variables, to see what moves the body into its catch.
static void*  s_watchDef = nullptr;
static bool   s_watchOn = false;
static double s_watchT0 = 0.0, s_watchGnd = 0.0;
static void Watch(uint8_t* an) {
    void* def = twkP(an, AN_FLIPTRICK);
    const double now = NowS();
    if (def != s_watchDef) {
        s_watchDef = def; s_watchOn = false; s_watchGnd = 0.0;
        char nm[96] = "", lo[96];
        if (def && NameOf(def, nm, sizeof(nm))) {
            int i = 0; for (; nm[i] && i < 95; i++) lo[i] = (char)tolower((unsigned char)nm[i]); lo[i] = 0;
            s_watchOn = !strstr(lo, "ollie");
            if (s_watchOn) { s_watchT0 = now; TwkLog("[rtflip] watch: the game's own %s", nm); }
        }
    }
    if (!s_watchOn) return;
    if (twkB(an, AN_GROUNDED)) { if (s_watchGnd == 0.0) s_watchGnd = now; else if (now - s_watchGnd > 0.6) return; }
    else {
        if (s_watchGnd > 0.0 && now - s_watchGnd > 0.6) {          // the same trick again, a new air
            s_watchT0 = now; char nm[96] = "?"; NameOf(def, nm, sizeof(nm));
            TwkLog("[rtflip] watch: the game's own %s (again)", nm);
        }
        s_watchGnd = 0.0;
    }
    AnimDrive(an, s_watchT0, 0.0f);
}
static void __fastcall hkSetFlipTrick(void* an, float dt) {
    void* sk = nullptr; void* saved = nullptr;
    bool mine = false;
    __try {
        mine = an && an == FootPlace_AnimInstance();
        if (mine && s_rateOurs) { *(float*)((uint8_t*)an + AN_PLAYRATE) = s_gameRate; s_rateOurs = false; }
        if (mine && s_animDef) {
            sk = twkP(an, AN_SKATER);
            if (sk) { saved = twkP(sk, SK_CUR_TRICK); *(void**)((uint8_t*)sk + SK_CUR_TRICK) = s_animDef; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { sk = nullptr; mine = false; }
    ((SetFlipTrickFn)g_origSFT)(an, dt);
    if (mine && s_startOff >= 0 && (!s_animDef || s_clipDone || s_state == S_IDLE)) {
        __try { RestoreStart((uint8_t*)an); } __except (EXCEPTION_EXECUTE_HANDLER) { s_startOff = -1; }
    }
    if (!sk) {
        if (mine && g_watch && g_animLog) { __try { Watch((uint8_t*)an); } __except (EXCEPTION_EXECUTE_HANDLER) { g_watch = 0; } }
        return;
    }
    __try {
        *(void**)((uint8_t*)sk + SK_CUR_TRICK) = saved;
        AnimDrive((uint8_t*)an, s_startT, dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_animDef = nullptr; g_hookOk = 0;
        TwkLog("[rtflip] body: caught a fault -- the game's trick clip is off for this session (procedural feet)");
    }
}

// Every sound cue a clip fires goes through UAnimNotify_PlaySoundRecorded::Notify. Logged on our skater from a
// flick pop to 1.5 s after: which sound, which clip, and where the trick and ollie clips were -- where the pop
// sits and which clip fires it.
static const char* SIG_SOUND_NOTIFY =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 49 38 48 85 C9 ?? ?? 48 85 D2 ?? ?? E8 ?? ?? ?? ?? 84 C0";
typedef void (__fastcall* SoundNotifyFn)(void* self, void* mesh, void* anim);
static void* g_origNotify = nullptr;
static void __fastcall hkSoundNotify(void* self, void* mesh, void* anim) {
    __try {
        void* sk = (g_animLog && self && mesh) ? CatchTweaks_Skater() : nullptr;
        int cs = -1; double age = 99.0;
        if (sk && mesh == twkP(sk, SK_MESH) && PopProbe_LastFlickPop(&cs, &age) && age < 1.5) {
            char snd[96] = "?", clip[96] = "?";
            NameOf(twkP(self, 0x38), snd, sizeof(snd));
            NameOf(anim, clip, sizeof(clip));
            const uint8_t* an = (const uint8_t*)FootPlace_AnimInstance();
            char tr[48] = "--", ol[48] = "--";
            if (an) { NodeLog(tr, sizeof(tr), an, 0); NodeLog(ol, sizeof(ol), an, 4); }
            TwkLog("[rtflip] sound cue: %s from %s, %.0f ms after the flick pop (%s) | trick %s | ollie %s", snd, clip,
                   age * 1000.0, s_state != S_IDLE ? "our flip" : "no flip", tr, ol);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ((SoundNotifyFn)g_origNotify)(self, mesh, anim);
}

// The board-control cues in the clips (UAnimNotify_SkateboardRotation / SkaterboardFlip / SkateboardRoll) kick the
// board with the SKATER's trick data (the ollie's) whenever a clip carrying them PLAYS. While our flip owns the
// board those are not the game's to give: the kickflip clip playing on the body fired a second set of them -- the
// "sort of scoop" on top of the flip. Swallowed on our skater while the flip is ours, and from the body's
// kickflip/heelflip clip until the body is handed back; each one logged.
static const char* SIG_NOTIFY_ROT =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B CA 48 8B FA E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 ?? ?? E8 ?? ?? "
    "?? ?? 48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38 ?? ?? 48 8B 49 30 48 39 04 D1 ?? ?? 48 8B 83 "
    "00 03 00 00 48 8B B8 58 05 00 00 48 85 FF 0F 84 ?? ?? ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B "
    "F8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4F 10 48 83 C0 30 48 63 50 08 3B 51 38 0F 8F "
    "?? ?? ?? ?? 48 8B 49 30 48 39 04 D1 0F 85 ?? ?? ?? ?? 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? ?? 48 "
    "8D 4C 24 48 E8 ?? ?? ?? ?? 48 8B CF 48 8B 10 E8 ?? ?? ?? ?? 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? "
    "?? 48 8D 4C 24 48 48 8B D8 E8 ?? ?? ?? ?? 48 8B CF 48 8B 10 E8 ?? ?? ?? ?? 48 85 DB 0F 84 ?? ?? "
    "?? ?? 48 8B 8F 08 06 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 81 90 05 00 00 F3 0F 10 91 24 06 00 "
    "00 F3 0F 10 80 C8 01 00 00";
static const char* SIG_NOTIFY_FLIP =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B CA 48 8B FA E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 ?? ?? E8 ?? ?? "
    "?? ?? 48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38 ?? ?? 48 8B 49 30 48 39 04 D1 ?? ?? 48 8B 83 "
    "00 03 00 00 48 8B B8 58 05 00 00 48 85 FF 0F 84 ?? ?? ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B "
    "F8 48 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 4F 10 48 83 C0 30 48 63 50 08 3B 51 38 0F 8F "
    "?? ?? ?? ?? 48 8B 49 30 48 39 04 D1 0F 85 ?? ?? ?? ?? 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? ?? 48 "
    "8D 4C 24 48 E8 ?? ?? ?? ?? 48 8B CF 48 8B 10 E8 ?? ?? ?? ?? 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? "
    "?? 48 8D 4C 24 48 48 8B D8 E8 ?? ?? ?? ?? 48 8B CF 48 8B 10 E8 ?? ?? ?? ?? 48 85 DB 0F 84 ?? ?? "
    "?? ?? 48 8B 8F 08 06 00 00 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 81 90 05 00 00 F3 0F 10 89 24 06 00 "
    "00 F3 0F 10 80 C0 01 00 00";
static const char* SIG_NOTIFY_ROLL =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F1 48 8B FA 48 8B CA E8 ?? ?? ?? ?? 48 8B D8 "
    "48 85 C0 ?? ?? E8 ?? ?? ?? ?? 48 8B 4B 10 48 83 C0 30";
typedef void (__fastcall* BoardCueFn)(void* self, void* mesh, void* anim);
static void* g_origCueRot = nullptr;
static void* g_origCueFlip = nullptr;
static void* g_origCueRoll = nullptr;
static bool SwallowBoardCue(void* mesh, void* anim, const char* what) {
    bool swallow = false;
    __try {
        void* sk = CatchTweaks_Skater();
        if (!sk || mesh != twkP(sk, SK_MESH)) return false;
        char clip[96] = "", lo[96];
        NameOf(anim, clip, sizeof(clip));
        int i = 0; for (; clip[i] && i < 95; i++) lo[i] = (char)tolower((unsigned char)clip[i]); lo[i] = 0;
        const bool flipClip = strstr(lo, "flip") && !strstr(lo, "ollie");
        swallow = s_state != S_IDLE || (s_animDef && flipClip);
        if (swallow && g_animLog)
            TwkLog("[rtflip] board cue: %s from %s -- swallowed (the flip owns the board)", what, clip[0] ? clip : "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) { swallow = false; }
    return swallow;
}
static void __fastcall hkCueRot(void* self, void* mesh, void* anim) {
    if (!SwallowBoardCue(mesh, anim, "rotation")) ((BoardCueFn)g_origCueRot)(self, mesh, anim);
}
static void __fastcall hkCueFlip(void* self, void* mesh, void* anim) {
    if (!SwallowBoardCue(mesh, anim, "flip")) ((BoardCueFn)g_origCueFlip)(self, mesh, anim);
}
static void __fastcall hkCueRoll(void* self, void* mesh, void* anim) {
    if (!SwallowBoardCue(mesh, anim, "roll")) ((BoardCueFn)g_origCueRoll)(self, mesh, anim);
}
// ---- where the board sits. USkaterMovementComponent::GetControlledBoardTargetLocation places the board in the
// air from the SKATER's trick: its PopBoard forward/side/height offsets and their curves (def +0x174..+0x1a8)
// over the trick's time. Ours stays an ollie there, so the flipping board rode ollie-close to the feet and
// clipped them; around that call the trick reads as our kickflip/heelflip, from the flick to the landing.
static const char* SIG_BOARD_TARGET =
    "4C 8B DC 55 56 57 49 8D AB 98 FE FF FF 48 81 EC 50 02 00 00 48 8B F9 48 8B F2 48 8B 89 30 0B 00 00 48 81 C1 80 02 00 00 F6 87 84 0C 00 00 01 48 8B 01";
enum { SK_MOVE = 0x550 };
typedef void* (__fastcall* BoardTargetFn)(void* comp, void* out, void* a3, void* a4);
static void* __fastcall hkBoardTarget(void* comp, void* out, void* a3, void* a4) {
    void* sk = nullptr; void* saved = nullptr;
    if (s_offsets && s_def) {
        __try {
            void* me = CatchTweaks_Skater();
            if (me && twkP(me, SK_MOVE) == comp) {
                sk = me; saved = twkP(sk, SK_CUR_TRICK);
                *(void**)((uint8_t*)sk + SK_CUR_TRICK) = s_def;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { sk = nullptr; }
    }
    void* r = ((BoardTargetFn)g_origBoardTarget)(comp, out, a3, a4);
    if (sk) { __try { *(void**)((uint8_t*)sk + SK_CUR_TRICK) = saved; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return r;
}

static void HookCue(const char* sig, void* hk, void** orig, const char* what) {
    uint8_t* at = TwkScanExe(sig);
    if (at && MH_CreateHook(at, hk, orig) == MH_OK && MH_EnableHook(at) == MH_OK)
        TwkLog("[rtflip] installed: the board's %s cue @ %p (swallowed while the flip is ours)", what, at);
    else { *orig = nullptr; TwkLog("[rtflip] board %s cue: %s -- the kickflip clip's cues reach the board", what, at ? "hook failed" : "not found"); }
}

void RtFlip_Install() {
    if (!TWK_RTFLIP) { TwkLog("[rtflip] real-time flips are disabled for now -- nothing installed"); return; }
    HookCue(SIG_NOTIFY_ROT, (void*)&hkCueRot, &g_origCueRot, "rotation");
    HookCue(SIG_NOTIFY_FLIP, (void*)&hkCueFlip, &g_origCueFlip, "flip");
    HookCue(SIG_NOTIFY_ROLL, (void*)&hkCueRoll, &g_origCueRoll, "roll");
    if (uint8_t* bt = TwkScanExe(SIG_BOARD_TARGET)) {
        if (MH_CreateHook(bt, (void*)&hkBoardTarget, &g_origBoardTarget) == MH_OK && MH_EnableHook(bt) == MH_OK)
            TwkLog("[rtflip] installed: the board's place in the air @ %p (a flip trick's board-to-body offsets)", bt);
        else { g_origBoardTarget = nullptr; TwkLog("[rtflip] board offsets: hook failed -- off"); }
    } else TwkLog("[rtflip] board offsets: GetControlledBoardTargetLocation not found -- off");
    if (uint8_t* sn = TwkScanExe(SIG_SOUND_NOTIFY)) {
        if (MH_CreateHook(sn, (void*)&hkSoundNotify, &g_origNotify) == MH_OK && MH_EnableHook(sn) == MH_OK)
            TwkLog("[rtflip] installed: the sound-cue log @ %p", sn);
        else { g_origNotify = nullptr; TwkLog("[rtflip] sound-cue log: hook failed (no log, nothing else affected)"); }
    } else TwkLog("[rtflip] sound-cue log: PlaySoundRecorded::Notify not found (no log, nothing else affected)");
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe) {
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((uint8_t*)exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
        s_exeLo = (uintptr_t)exe; s_exeHi = s_exeLo + nt->OptionalHeader.SizeOfImage;
    }
    g_flipAudioStart = (BoardFn)TwkScanExe(SIG_FLIP_AUDIO_START);
    g_flipAudioStop  = (BoardFn)TwkScanExe(SIG_FLIP_AUDIO_STOP);
    TwkLog("[rtflip] sound: flip whoosh start %p, stop %p%s", g_flipAudioStart, g_flipAudioStop,
           (g_flipAudioStart && g_flipAudioStop) ? "" : " -- no flip whoosh (game updated?)");
    if (!g_flipAudioStop) g_flipAudioStart = nullptr;                  // never start one we could not stop
    uint8_t* at = TwkScanExe(SIG_SET_FLIP_TRICK);
    if (!at) { TwkLog("[rtflip] body: SetFlipTrick not found (game updated?) -- procedural feet"); return; }
    if (MH_CreateHook(at, (void*)&hkSetFlipTrick, &g_origSFT) != MH_OK || MH_EnableHook(at) != MH_OK) {
        g_origSFT = nullptr; TwkLog("[rtflip] body: hook failed on SetFlipTrick -- procedural feet"); return;
    }
    g_hookOk = 1;
    TwkLog("[rtflip] installed: SetFlipTrick @ %p (the body's trick each frame)", at);
}

void RtFlip_DrawMenu(const OmpMenuApi* api) {
    if (!api || !TWK_RTFLIP) return;
    bool on = g_on != 0;
    if (api->Checkbox("Real-time flips (test)", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(after a flick pop, flick the other stick sideways; it flips until you catch with a stick push)");
    if (!g_on) return;
    float v = g_gain * 100.0f;
    if (api->SliderFloat("Flick to spin (%)", &v, 50.0f, 600.0f, "%.0f")) { g_gain = v / 100.0f; TwkMarkDirty(); }
    v = g_min;
    if (api->SliderFloat("Slowest spin (deg/s)", &v, 100.0f, 2000.0f, "%.0f")) { g_min = v; if (g_max < g_min) g_max = g_min; TwkMarkDirty(); }
    v = g_max;
    if (api->SliderFloat("Fastest spin (deg/s)", &v, 200.0f, 3000.0f, "%.0f")) { g_max = v; if (g_min > g_max) g_min = g_max; TwkMarkDirty(); }
    bool snd = g_sounds != 0;
    if (api->Checkbox("Flip sounds", &snd)) { g_sounds = snd ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the in-air whoosh and the catch sound, as a game flip trick makes them)");
    bool offs = g_boardOffsets != 0;
    if (api->Checkbox("Flip board placement", &offs)) { g_boardOffsets = offs ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled(g_origBoardTarget ? "(the board sits away from the feet as a game flip trick puts it)"
                                                         : "(not available: the hook did not install)");
    v = g_pitchDeg;
    if (api->SliderFloat("Flip pitch (deg)", &v, 0.0f, 60.0f, "%.0f")) { g_pitchDeg = v; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(the pitch a flick all the way into the corner gives, as the game's 30)");
    v = (float)g_pitchDelayMs;
    if (api->SliderFloat("Pitch starts after (ms)", &v, 0.0f, 300.0f, "%.0f")) { g_pitchDelayMs = (int)v; TwkMarkDirty(); }
    v = (float)g_pitchInMs;
    if (api->SliderFloat("Pitch eases in over (ms)", &v, 20.0f, 600.0f, "%.0f")) { g_pitchInMs = (int)v; TwkMarkDirty(); }
    bool anim = g_anim != 0;
    if (api->Checkbox("Body plays the game's trick", &anim)) { g_anim = anim ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled(g_hookOk ? "(its kickflip/heelflip clip, timed from the board's spin; off = procedural feet)"
                                                : "(not available: the hook did not install)");
    if (g_anim) {
        v = g_animTurn;
        if (api->SliderFloat("Trick clip over (deg)", &v, 90.0f, 720.0f, "%.0f")) { g_animTurn = v; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(how much of the spin the kickflip clip plays over before the flip loop)");
        bool ctl = g_animFeet == 0;
        if (api->Checkbox("Mid-air foot control (test)", &ctl)) { g_animFeet = ctl ? 0 : 1; TwkMarkDirty(); }
        api->SameLine(); api->TextDisabled("(off: the clip's own feet. On: the foot IK stays on and the sticks steer the feet)");
        v = (float)g_catchMs;
        if (api->SliderFloat("Catch timing (ms)", &v, 40.0f, 400.0f, "%.0f")) { g_catchMs = (int)v; TwkMarkDirty(); }
        return;
    }
    v = g_kick;
    if (api->SliderFloat("Kick-out (cm)", &v, -60.0f, 60.0f, "%.0f")) { g_kick = v; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(front foot off the side on your hardest flick; negative = the other side)");
    v = g_slide;
    if (api->SliderFloat("Kick forward (cm)", &v, 0.0f, 40.0f, "%.0f")) { g_slide = v; TwkMarkDirty(); }
    v = g_roll;
    if (api->SliderFloat("Flick ankle roll (deg)", &v, -45.0f, 45.0f, "%.0f")) { g_roll = v; TwkMarkDirty(); }
    v = g_sole;
    if (api->SliderFloat("Sole out (deg)", &v, 0.0f, 90.0f, "%.0f")) { g_sole = v; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(once the foot leaves the board, the sole turns toward the nose)");
    v = g_lift;
    if (api->SliderFloat("Front foot lift (cm)", &v, 0.0f, 40.0f, "%.0f")) { g_lift = v; TwkMarkDirty(); }
    v = g_bLift;
    if (api->SliderFloat("Back foot lift (cm)", &v, 0.0f, 50.0f, "%.0f")) { g_bLift = v; TwkMarkDirty(); }
    v = g_bFwd;
    if (api->SliderFloat("Back foot forward (cm)", &v, -20.0f, 30.0f, "%.0f")) { g_bFwd = v; TwkMarkDirty(); }
    v = g_bTilt;
    if (api->SliderFloat("Back foot tilt (deg)", &v, -45.0f, 45.0f, "%.0f")) { g_bTilt = v; TwkMarkDirty(); }
    v = (float)g_catchMs;
    if (api->SliderFloat("Catch timing (ms)", &v, 40.0f, 400.0f, "%.0f")) { g_catchMs = (int)v; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(how long before the board is flat the feet come down)");
}
