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
// SessionTweaks -- POP CONTROL probe. See pop_probe.h for what is being measured and why.
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "pop_probe.h"
#include "scoop_speed.h"     // ScoopSpeed_StickRaw -- both sticks, sampled on the input tick
#include "foot_place.h"      // FootPlace_AnimInstance / FootPlace_SettingUpTrick (asset-gated crank)
#include "catch_tweaks.h"    // CatchTweaks_Skater -- the root the handler scan starts from
#include "flip_speed.h"      // FlipSpeed_Stance -- the pad gate's switch/fakie answer
#include "MinHook.h"      // the pad-level hook (injection mode 3)
#include <cmath>
#include <cstring>

// ------------------------------------------------------------------ offsets (all previously measured)
enum {
    AN_GROUNDED    = 0x5fa,   // foot_place/catch_tweaks -- same USkaterAnimInstance field
    AN_IS_GOOFY    = 0x304,   // catch_tweaks -- stance context for the input mapping table
    AN_IS_GRINDING = 0x33c,   // PDB: IsGrinding, with IsGrindingInLiptrick right behind at +0x33d
                              // -- both read as one hands-off signal (see g_padGrind)
    // From the shipped PDB (named, not scanned): CrankInBlendSpace at +0x4a0 is the descent's
    // blend space asset. CrankInStartTime (+0x4c0) was round 52's dead end: it read ~0.0 at every
    // crank start, so it is a start-OFFSET into the descent consumed once at state entry (its
    // neighbor IsQuickCrankSwapping explains why it exists), not a timestamp the graph subtracts
    // from -- per-frame writes fed a value nothing reads (rebases 0, visual unmoved). The
    // descent's live progress is the blend space player NODE's InternalTimeAccumulator, in the
    // anim BP's property area ABOVE the native class (native size 0x7e8) -- which is also why the
    // old 0x100..0xb00 value scans never found a crouch float.
    AN_CRANKIN_BS = 0x4a0,    // USkaterAnimInstance::CrankInBlendSpace (UBlendSpace*)
    // FAnimNode_BlendSpacePlayer (engine PDB), the fields the scrub reads/writes:
    BSP_GROUP    = 0x10,      // GroupName (FName comp index; nonzero = sync group could fight us)
    BSP_WEIGHT   = 0x1c,      // BlendWeight
    BSP_TIME     = 0x20,      // InternalTimeAccumulator -- THE descent clock (seconds into the anim)
    BSP_X        = 0x38,      // axis input (likely the pocket)
    BSP_PLAYRATE = 0x44,
    BSP_LOOP     = 0x48,      // bLoop -- if set, writing past the clip length WRAPS the pose
    BSP_BS       = 0x50,      // BlendSpace* -- the scan key
    // The scan target is the FLIP TRICKS HANDLER, not the anim instance. Two full anim-instance
    // sweeps established that the crouch is not mirrored there as a writable float -- with world
    // drift eliminated, everything left was state timers, session-time stamps and a smoothed pose
    // follower converging on ~0.8 from arbitrary starts. The crank's own clock has to live on the
    // handler, where CheckForTrick computes the pop ratio from it, and it is upstream of both
    // halves the redesign needs: the ratio, and (via the anim graph) very likely the crouch visual.
    //
    // The handler is found rather than hooked: FTH_SKATER (+0x08) back-points to the skater and
    // FTH_TRICKS_DB (+0x28) to the tricks database, so a scan of the skater's pointer slots
    // self-verifies the match. Both offsets are scoop_speed's, long since measured.
    FTH_SKATER      = 0x08,
    FTH_TRICKS_DB   = 0x28,
    // The handler window is scanned twice per tick: as floats (an accumulated duration) and as
    // doubles (a start TIMESTAMP in game seconds -- which is CONSTANT during the crank and
    // therefore invisible to any range/correlation test; those are caught by the latch report,
    // which looks for slots that changed at crank entry and then held still).
    FTH_SCAN_BYTES  = 0x300,
    FTH_FLOAT_SLOTS = FTH_SCAN_BYTES / 4,
    FTH_DBL_SLOTS   = FTH_SCAN_BYTES / 8,
    SCAN_SLOTS      = FTH_FLOAT_SLOTS + FTH_DBL_SLOTS,
};

// ------------------------------------------------------------------ config
static int g_on   = 1;   // PopProbe -- the whole module; measurement only, so it ships on like gpop
static int g_scan = 0;   // PopProbeScanAlpha -- the anim-float scan. DEV ONLY: it found the crank
                         // clock and has nothing left to say, so it ships off; its output is
                         // per-crank spam in a shipped build.
static int g_log  = 0;   // PopProbeLog -- the development diagnostics (stick-visibility records,
                         // crouch-visual and mode traces, crank/jump correlation, pad-poll counts).
                         // Off in shipped builds: this module logged every frame of its own
                         // debugging. Install lines and real failures always print.
// Development diagnostics. This module logged every frame of its own debugging; in a shipped
// build all of that sits behind PopProbeLog, leaving install lines and real failures.
#define TwkLogDev(...) do { if (g_log) TwkLog(__VA_ARGS__); } while (0)
// The scheme runs on one of two injection paths: 3 = the XInput pad hook (only sees
// controllers the game reads through XInput), 4 = the game-side read path (the stick getters
// every gesture consumer calls -- works for ANY controller on ANY backend, the shipped path).
// THE ROUND-1 EXPERIMENT -- the module's one deliberate write, off by default. The handler's crank
// clock was found at +0x38 (seconds, accumulated per frame; starts at one frame's dt every crank)
// and maps to the pop ratio as clamp01((v - 0.076) / 0.234), a five-point fit within +/-0.002 with
// bit-identical repeats. -1 = untouched. 0..100 = while cranking, the clock is HELD at the value
// that produces that percent of a full pop. One long crank then answers two questions at once: does
// the pop obey the written clock (it must, if the fit is real), and does the CROUCH VISUAL freeze
// with it -- which decides whether one field drives both halves of the redesign or the visual is
// paced separately by the anim state.
static int g_force = -1; // PopProbeForceCrank
enum { FTH_CRANK_CLOCK = 0x38 };
static const float kRatio0 = 0.076f, kRatioSpan = 0.234f;   // the fit's intercept and span
// THE ROUND-2 EXPERIMENT -- synthetic stick input, off by default. While the user holds the LEFT
// stick down past kInjectGate (a gesture vanilla ignores on the ground), the raw stick fields on
// the InputHandler are rewritten to what the redesign will feed the game: LS neutral, RS full down.
// If the game enters its crank from that, injection is proven and the whole remap becomes assembly.
// The unknown is WHERE in the frame the write must land, so the knob picks the timing:
//   0 = off   1 = write BEFORE the game's input tick   2 = write AFTER it
// Whichever mode produces a CRANK line while the physical right stick is untouched is the answer.
// The physical sticks are sampled before the early write point, so the probe's own logs always
// show the user's real hands; mode 2's write survives into the next frame, and whether the sampled
// "physical" sticks then read as our synthetic values is itself reported -- it reveals whether the
// game refreshes the fields upstream of its tick.
static int g_inject = 0;  // PopProbeInject
static bool SchemeOn() { return g_on && (g_inject == 3 || g_inject == 4); }
// The pad gate's two feel knobs (mode 3); the discriminator rationale sits with the hook below.
static int g_dwellMs = 110;   // PopProbeDwellMs -- LS must SIT in the down zone this long to engage
static int g_maskMs  = 180;   // PopProbeReleaseMaskMs -- LS stays hidden this long after disengage
// THE SCHEME ITSELF, layered on the proven injection (all mode 3):
//   PopProbeDepthPop -- LS depth IS the pop height: while crouched, the crank clock is written from
//   how far the stick is buried past the engage point, so a barely-held crouch pops small and a
//   buried one pops full. The write is the ForceCrank plumbing fed live from the stick.
//   PopProbeTrickWindowMs -- the "wait briefly" pop: a physical RS flick DOWN while crouched arms
//   the pop and opens this window; the LEFT stick unmasks so the user's own trick flick reaches the
//   still-cranked game (pop + trick in one gesture, as the game expects). No flick inside the
//   window = a plain ollie is synthesized. The armed DEPTH is frozen at the RS flick, so moving LS
//   to do the trick cannot change the pop height.
static int g_depthPop = 1;        // PopProbeDepthPop
static int g_windowMs = 200;      // PopProbeTrickWindowMs
static int g_graceMs  = 60;       // PopProbeGraceMs -- how long a flick-ended crouch waits for RS
static int g_manualGate   = 1;    // PopProbeManualGate -- the game-side manual gate (see its block)
static int g_landSettleMs = 250;  // PopProbeLandSettleMs -- how long after a pop's touchdown the
                                  // manual gate stays closed (recovering thumbs come home well
                                  // inside this; a stick STILL held past it is a real manual ask)
static int g_crankVis     = 1;    // PopProbeCrankVis -- scrub the crank descent VISUAL to the held
                                  // crouch depth via the blend space player's clock (pump block)
static int g_crankVisTime = 950;  // PopProbeCrankVisTime -- ms into the CrankIn anim that depth
                                  // 1.0 maps to; tune until full stick bottoms out exactly. The
                                  // round-54 log clocked the natural descent past 0.498s (rate
                                  // 1.5), so 350 undershot the bottom -- 550 is the new default,
                                  // but an ini line saved by an older build still wins
static int g_crankVisMin  = 400;  // PopProbeCrankVisMinMs -- ms of the clip's front skipped: the
                                  // descent OPENS with a pre-crouch (the back foot hops onto the
                                  // tail) before any real crouching, so depth maps into
                                  // [min..Time] and a slight pull already shows a slight crouch
static int g_crankVisSmoothMs = 250; // PopProbeCrankVisSmoothMs -- time constant of the visual's
                                  // ease toward the stick's depth (raw 1:1 tracking read as
                                  // jerky/mechanical on slow pulls; field report). 0 = raw. The
                                  // POP depth is untouched -- this shapes only the pose
static int g_stances = 1;         // PopProbeStances -- the scheme on every stance/pop family:
                                  // regular nollie = RS up crouches, switch ollie = RS down,
                                  // switch nollie = LS up (fakie = regular inputs, no mapping
                                  // needed). 0 = the original regular-ollie-only behavior with
                                  // the switch veto back in the context gate
// The crouch gate is a FEEL number and configurable (PopProbeCrouchGatePct); the 0.65 it started at
// was an experiment safety and read as a huge deadzone. Hysteresis: releases 15 points shallower
// than it engages. Written once at config time on the game thread; the hook reads plain shorts.
static short g_lsEngage  = (short)(-0.40f * 32767);
static short g_lsRelease = (short)(-0.25f * 32767);
enum { IH_RAW_LEFT = 0x24, IH_RAW_RIGHT = 0x2c };           // scoop_speed's measured offsets
static const float kInjectGate = 0.60f;

void PopProbe_ReadConfig(const char* buf) {
    g_on   = TwkIniInt(buf, "PopProbe", 1);
    g_scan = TwkIniInt(buf, "PopProbeScanAlpha", 0);
    g_log  = TwkIniInt(buf, "PopProbeLog", 0);
    g_force = TwkIniInt(buf, "PopProbeForceCrank", -1);
    if (g_force > 100) g_force = 100;
    g_inject = TwkIniInt(buf, "PopProbeInject", 0);
    if (g_inject < 0 || g_inject > 4) g_inject = 0;
    if (g_inject == 3) {
        // The pad-level path is superseded: it only sees XInput controllers, and every ini
        // that enabled the scheme before 3.19.75 says 3. Upgraded on read; the save writes 4.
        g_inject = 4;
        TwkLog("[pop] PopProbeInject=3 (XInput pad path) upgraded to 4 (the game-side read path)");
    }
    g_dwellMs = TwkIniInt(buf, "PopProbeDwellMs", 110);
    if (g_dwellMs < 0) g_dwellMs = 0; else if (g_dwellMs > 500) g_dwellMs = 500;
    g_maskMs  = TwkIniInt(buf, "PopProbeReleaseMaskMs", 180);
    if (g_maskMs < 0) g_maskMs = 0; else if (g_maskMs > 500) g_maskMs = 500;
    g_depthPop = TwkIniInt(buf, "PopProbeDepthPop", 1);
    // A window-0 "instant pop, trick becomes a late flick" mode was tried here and REJECTED in the
    // field -- the wait-briefly grammar at ~180 ms felt better in hand. Do not re-pitch it.
    g_windowMs = TwkIniInt(buf, "PopProbeTrickWindowMs", 200);
    if (g_windowMs < 60) g_windowMs = 60; else if (g_windowMs > 600) g_windowMs = 600;
    {   // The crouch gate, as a percent of stick travel. Hysteresis releases 15 points shallower.
        int pct = TwkIniInt(buf, "PopProbeCrouchGatePct", 36);
        if (pct < 20) pct = 20; else if (pct > 80) pct = 80;
        g_lsEngage  = (short)(-(pct * 32767) / 100);
        g_lsRelease = (short)(-((pct - 15) * 32767) / 100);
    }
    g_graceMs = TwkIniInt(buf, "PopProbeGraceMs", 60);
    if (g_graceMs < 0) g_graceMs = 0; else if (g_graceMs > 150) g_graceMs = 150;
    g_manualGate   = TwkIniInt(buf, "PopProbeManualGate", 1);
    g_landSettleMs = TwkIniInt(buf, "PopProbeLandSettleMs", 250);
    if (g_landSettleMs < 0) g_landSettleMs = 0; else if (g_landSettleMs > 1000) g_landSettleMs = 1000;
    g_crankVis     = TwkIniInt(buf, "PopProbeCrankVis", 1);
    g_crankVisTime = TwkIniInt(buf, "PopProbeCrankVisTime", 950);
    if (g_crankVisTime < 50) g_crankVisTime = 50; else if (g_crankVisTime > 2000) g_crankVisTime = 2000;
    g_crankVisMin  = TwkIniInt(buf, "PopProbeCrankVisMinMs", 400);
    if (g_crankVisMin < 0) g_crankVisMin = 0;
    if (g_crankVisMin > g_crankVisTime - 50) g_crankVisMin = g_crankVisTime - 50;
    g_crankVisSmoothMs = TwkIniInt(buf, "PopProbeCrankVisSmoothMs", 250);
    if (g_crankVisSmoothMs < 0) g_crankVisSmoothMs = 0; else if (g_crankVisSmoothMs > 1000) g_crankVisSmoothMs = 1000;
    g_stances = TwkIniInt(buf, "PopProbeStances", 1);
    if (SchemeOn() && g_inject != 0)
        TwkLog("[pop] config: pop control scheme ON | trick window %d ms, crouch gate %d%%, "
               "crouch visual %d-%d ms smoothed %d ms%s%s", g_windowMs,
               (int)(-(int)g_lsEngage * 100 / 32767), g_crankVisMin, g_crankVisTime,
               g_crankVisSmoothMs, g_crankVis ? "" : " (crouch visual OFF)",
               g_log ? " [diagnostics on]" : "");
    else
        TwkLog("[pop] config: pop control scheme off%s", g_log ? " [diagnostics on]" : "");
    if (g_force >= 0 || g_scan)
        TwkLog("[pop] DEV KEYS ACTIVE: ForceCrank=%d ScanAlpha=%d -- development only",
               g_force, g_scan);
}
void PopProbe_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "PopProbe", g_on);
    TwkIniSetInt(buf, cap, "PopProbeScanAlpha", g_scan);
    TwkIniSetInt(buf, cap, "PopProbeLog", g_log);
    TwkIniSetInt(buf, cap, "PopProbeForceCrank", g_force);
    TwkIniSetInt(buf, cap, "PopProbeInject", g_inject);
    TwkIniSetInt(buf, cap, "PopProbeDwellMs", g_dwellMs);
    TwkIniSetInt(buf, cap, "PopProbeReleaseMaskMs", g_maskMs);
    TwkIniSetInt(buf, cap, "PopProbeDepthPop", g_depthPop);
    TwkIniSetInt(buf, cap, "PopProbeTrickWindowMs", g_windowMs);
    TwkIniSetInt(buf, cap, "PopProbeCrouchGatePct", (int)(-(int)g_lsEngage * 100 / 32767));
    TwkIniSetInt(buf, cap, "PopProbeGraceMs", g_graceMs);
    TwkIniSetInt(buf, cap, "PopProbeManualGate", g_manualGate);
    TwkIniSetInt(buf, cap, "PopProbeLandSettleMs", g_landSettleMs);
    TwkIniSetInt(buf, cap, "PopProbeCrankVis", g_crankVis);
    TwkIniSetInt(buf, cap, "PopProbeCrankVisTime", g_crankVisTime);
    TwkIniSetInt(buf, cap, "PopProbeCrankVisMinMs", g_crankVisMin);
    TwkIniSetInt(buf, cap, "PopProbeCrankVisSmoothMs", g_crankVisSmoothMs);
    TwkIniSetInt(buf, cap, "PopProbeStances", g_stances);
}
void PopProbe_ResetDefaults() {
    g_on = 1; g_scan = 0; g_log = 0; g_force = -1;
    g_inject = 0;                     // the scheme is OPT-IN: reset leaves it off
    g_dwellMs = 110; g_maskMs = 180; g_graceMs = 60; g_depthPop = 1; g_windowMs = 200;
    g_lsEngage = (short)(-(36 * 32767) / 100);
    g_lsRelease = (short)(-(21 * 32767) / 100);
    g_manualGate = 1; g_landSettleMs = 250; g_stances = 1;
    g_crankVis = 1; g_crankVisTime = 950; g_crankVisMin = 400; g_crankVisSmoothMs = 250;
}

// ------------------------------------------------------------------ the sample ring
// One entry per input tick. Everything here is read on the game thread from the shell pump and
// consumed on the same thread, so there is nothing to synchronise.
struct Tick {
    double t;
    float  lx, ly, rx, ry;
    unsigned char crank, grounded;
};
static const int kTicks = 256;                    // ~2 s at the input rate
static Tick g_ring[kTicks];
static int  g_wr = 0;
static int  g_n  = 0;

static double NowS() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ------------------------------------------------------------------ injection state
// Shared between the write points (defined below), the pad hook, and the pump's bookkeeping --
// declared up here because the pump reads them (the declaration-order trap, again).
static bool          g_injActive = false;
static double        g_injT0 = 0.0;
static bool          g_injCranked = false;
static bool          g_injNoCrankSaid = false;
// The pad hook's CONTEXT GATE, computed on the game thread each input tick. The first field test ran
// with only "grounded", and grounded is true walking around off the board too -- so holding LS down
// off-board fed the game a phantom RS (camera spin), and fakie's LS-down flick got eaten. The gate is
// where the pad-level approach earns its keep: the rewrite is blunt, the game state deciding WHEN it
// applies is surgical. Allowed = grounded AND on the board (bOnBoard, movement+0xe20, PDB-named) AND
// not riding switch/fakie (stance flips the stick roles; the finished remap MIRRORS there instead of
// disabling, but that is design still owed). The QPC stamp is the pause/menu guard: world paused =
// the pump stops = the stamp ages out = the hook stands down within 150 ms, so menus never see the
// rewrite even though the pad keeps being polled.
static volatile long      g_padAllow = 0;         // written by the pump, read by the pad hook
static volatile long      g_padSw    = 0;         // pump: switch stance -- the stick roles swap
static volatile long      g_padGrind = 0;         // pump: grinding/liptrick -- total hands-off
static volatile long      g_padModeDbg = 0;       // hook: bit0 = nollie family, bit1 = switch --
                                                  // published so the log can finally SHOW which
                                                  // family the machine was in at any moment
static volatile long long g_padHotUntil = 0;      // hook: QPC until which the scheme is "hot"
                                                  // (crouched/armed + 500 ms) -- the quick-shove
                                                  // gate's window (see hkSetTrick)
static volatile long long g_padStamp = 0;         // QPC of the last allow computation
static volatile long g_padActive   = 0;           // written by the pad hook, read by the pump
// The scheme's cross-thread state. The hook owns the gesture machine; the pump owns every game
// write. Depths travel as float bits in a long, the only atomic float this needs.
static volatile long g_padCrank    = 0;           // pump: the game's crank flag, for the hook
// The physical pad, pre-rewrite, for PopProbe_PhysSticks. Shorts fit a long; torn reads across two
// fields cost one poll of skew at worst, which the trackers cannot perceive.
static volatile long      g_physLx = 0, g_physLy = 0, g_physRx = 0, g_physRy = 0;
static volatile long long g_physStamp = 0;
static volatile long g_padDepthBits = 0;          // hook: live crouch depth 0..1 while crouched
// ---- THE CRANK-VISUAL CLOCK, PUBLISHED FOR OPENMP -------------------------------------------------
// The multiplayer mod transports the scrubbed descent clock so peers see the held crouch DEPTH
// instead of the vanilla full dive (their proxy is wire-driven; installing SessionTweaks on the
// observer cannot help). Published as bits + a tick stamp and read through the exported accessor
// below -- the same GetProcAddress seam the menu pages ride, in the other direction. The stamp is
// what makes staleness detectable: a scrub that stops publishing (feature off, pad idle, fault
// path) simply goes stale within a frame or two rather than needing a "stopped" edge of its own.
static volatile long      g_visPubBits = 0;        // the smoothed clock, as float bits
static volatile long long g_visPubTick = 0;        // GetTickCount64 at the last publish
static volatile long g_padArmed    = 0;           // hook: pop armed (trick window open)
static volatile long g_padArmedDepthBits = 0;     // hook: depth FROZEN at the RS flick
// LEAK EVIDENCE ring. Four rounds of manual fixes went in on theory; this ends that. The hook
// records every sample the game is actually SHOWN with a manual-capable component in it while the
// scheme's context is live: the crouch stick's down half visible in the idle branch (nose-manual
// input), or the pop stick's slight-down band visible while the crouch stick is still above the
// band gate (back-manual input from a resting thumb, the one spot the RS neutralizer cannot
// cover). The pump drains and logs. A manual in the field with no line here = the trigger is not
// an input sample at all, and the hunt moves game-side; a line = the exact hole, timestamped.
struct PadLeakRec { long long qpc; short outLy, outLx, outRy, phLy; short mov; char fast, kind; };
static PadLeakRec    g_leakRecs[32];
static volatile LONG g_leakWr = 0;
static LONG          g_leakRd = 0;                // pump-side cursor

// ------------------------------------------------------------------ the manual gate (game-side)
// Four rounds of input-side masking could not kill stray manuals, and the leak ring finally showed
// why: the game latches them at the LANDING after a pop. Skate_CheckForManuals reads the sticks at
// touchdown while the thumbs are still recovering from the pop press and trick flick, and lands
// the skater into a manual -- a first-class mechanic (_landedInManual on the skater, authored
// TrickManualStartPitchData in UManualsDatabase). The pad masks are grounded-gated and structurally
// cannot cover that instant. So the DECISION is gated instead: every SetManuals(true) caller in
// the exe lives inside ASessionPlayerController::Skate_CheckForManuals (xref-verified; the other
// callers -- Pop, Bail, SetTrick, ResetRagDoll, SetOnFootMode, anim notifies -- clear it), and
// while the scheme runs, that function is skipped ONLY inside the diagnosed danger window: the
// crouch/armed gesture itself, and from each pop until its landing settles (or both sticks come
// home first). Outside that window manual entry is untouched vanilla -- tip a stick, it manuals,
// nose or tail as stance maps it (the game's own test is |axis| > a ManualsDatabase threshold per
// stick, direction-agnostic; entry hold at db+0x38, ExitManualDelay at db+0x44). Two rejected
// designs, do not revisit: a posture WHITELIST (.49, "RS parked slight-down only") silently
// killed every other entry including nose manuals; a PARK test (.50, stick still inside a
// drift box for a hold time) field-tested as "manuals nearly impossible" -- a human thumb cannot
// hold a stick inside a fraction-of-travel box, so the clock restarted forever. Deliberate entry
// must pass NO test; only the danger window is closed. A manual already in progress always runs
// vanilla (its balance and exit paths live in the same function; the pad masks also stand down
// while one is live so balance input flows raw), and Pop clears manuals itself, so popping out
// of one still works. Sig verified 1-hit on BOTH exes; both call sites pass (this, float):
// movaps xmm1,dt / mov rcx,controller / call.
static const char* SIG_CHECK_MANUALS =
    "4C 8B DC 53 56 57 41 54 41 55 41 56 48 81 EC A8 00 00 00 48 8B 81 ?? ?? 00 00 49 8D 53 08 "
    "41 0F 29 73 B8 48 8B F9 48 8B 89 ?? ?? 00 00";
typedef void (*CheckManualsFn)(void* pc, float dt);
static void* g_origCheckManuals = nullptr;
static volatile long g_forbidManual = 0;          // pump verdict, read by the hook (game thread)
static volatile long g_inManual = 0;              // pump: skater's manual bits live right now
// _isInManual = 0x02, _isInNoseManual = 0x04, bitfield byte on ASkaterCharacterBase (PDB layout;
// bit positions read from SetManuals' own and/or masks). 0x06 = "in any manual".
enum { SK_MANUAL_BITS = 0x918, MANUAL_BITS_MASK = 0x06 };
static bool SkaterInManual() {
    __try {
        void* sk = CatchTweaks_Skater();
        return sk && (twkB(sk, SK_MANUAL_BITS) & MANUAL_BITS_MASK) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// ------------------------------------------------------------------ the quick-shove gate
// Three rounds of input shaping (67-70: stricter neutralizers, motion-extended release masks, a
// crank release ramp) never touched the crouch-release quick shove, and the user called the
// revert. This is the direct fix they asked for, in the pattern that killed the manuals: quick
// shoves are TRICK DEFS flagged UFlipTrickDefinition::IsQuickShove (+0x63, PDB), every trick
// selection funnels through ASkaterCharacterBase::SetTrick, and while the scheme is HOT (crouch
// engaged, pop armed, or within 500 ms of either -- the pad hook stamps g_padHotUntil) a
// flagged def is simply refused. Popped shoves are different defs and pass untouched; deliberate
// quick shoves from neutral riding are outside the window and stay vanilla. Sig 1-hit on BOTH
// exes; args (skater, def, float, float, then stack) -- eight forwarded to be safe.
static const char* SIG_SET_TRICK =
    "48 8B C4 53 56 48 81 EC 98 00 00 00 F6 81 ?? ?? 00 00 04 48 8B F2 0F 29 70 D8 48 8B D9 "
    "0F 29 78 C8 0F 28 F2 0F 28 FB";
enum { FTD_IS_QUICK_SHOVE = 0x63 };
typedef void (*SetTrickFn)(void*, void*, float, float,
                           uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static void* g_origSetTrick = nullptr;
static volatile long g_quickBlocked = 0;          // count, drained into a throttled pump log
static void hkSetTrick(void* sk, void* def, float a3, float a4,
                       uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) {
    if (SchemeOn() && def) {
        bool block = false;
        __try {
            if (twkB(def, FTD_IS_QUICK_SHOVE)) {
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                block = q.QuadPart < g_padHotUntil;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { block = false; }
        if (block) { InterlockedIncrement(&g_quickBlocked); return; }
    }
    if (g_origSetTrick)
        ((SetTrickFn)g_origSetTrick)(sk, def, a3, a4, a5, a6, a7, a8);
}

static void hkCheckManuals(void* pc, float dt) {
    // The verdict must be FRESH -- a stale forbid (pump stopped: pause, load) falls back to
    // vanilla rather than sticking. Same 150 ms rule as the pad hook's stamp.
    if (g_forbidManual && g_manualGate) {
        static long long freq = 0;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if ((q.QuadPart - g_padStamp) < freq / 6 && !SkaterInManual()) return;
    }
    if (g_origCheckManuals) ((CheckManualsFn)g_origCheckManuals)(pc, dt);
}
enum { MC_ON_BOARD = 0xe20, SK_MOVE_COMP = 0x550 };   // OpenMP's kMoveOnBoard / grind_pop's offset
static float BitsToF(long b) { float f; memcpy(&f, &b, 4); return f; }
static long  FToBits(float f) { long b; memcpy(&b, &f, 4); return b; }

// ------------------------------------------------------------------ crank bookkeeping
static bool   g_wasCrank   = false;
static double g_crankT0    = 0.0;                 // crank start
static double g_crankT1    = 0.0;                 // crank end (0 while active)
static float  g_maxL = 0.0f, g_maxR = 0.0f;       // peak deflection during the crank
static float  g_endLx = 0.0f, g_endLy = 0.0f;     // stick vectors at their peak (corner reading)
static float  g_endRx = 0.0f, g_endRy = 0.0f;
static int    g_goofy = 0;

// Stick angle in degrees, screen convention: 0 = up, 90 = right, 180 = down. A pocket pop shows as
// the crank-stick angle sitting off the 180 line, so the raw angle IS the corner measurement.
static float StickAngle(float x, float y) {
    if (x*x + y*y < 1e-6f) return 0.0f;
    float a = atan2f(x, y) * 57.29578f;           // y-up, so atan2(x, y) puts 0 at up
    if (a < 0.0f) a += 360.0f;
    return a;
}

// ------------------------------------------------------------------ the alpha scan
// Per-slot correlation over each crank, not a moved/not-moved gate. The first pass filtered on "was
// still in the second before the crank", which held up only while standing: once riding, everything
// physiological moves between tricks and every slot was rejected. What identifies the crouch value
// is not stillness but CORRELATION -- against time since the crank began (a duration-driven pose)
// and against the crank stick's deflection (a depth-driven one). Which of those wins is itself the
// answer the redesign needs: whether the game HAS crouch depth, or only crouch time.
struct SlotAcc {
    float first, last, mn, mx;
    // Doubles, deliberately: with values in the thousands (world coordinates land in the window),
    // float accumulators cancel catastrophically in n*sum(v^2) - sum(v)^2 and printed correlations
    // over 1.0 -- impossible, and enough to rank garbage above real candidates.
    double sv, svv, stv, smv;                    // sums of v, v*v, t*v, m*v over the crank's ticks
};
static SlotAcc g_acc[SCAN_SLOTS];
static double  g_st, g_stt, g_sm, g_smm;         // sums of t, t*t, m, m*m (shared by every slot)
static int     g_sn;

// Slot i < FTH_FLOAT_SLOTS reads a float at i*4; the rest read a double at (i-FTH_FLOAT_SLOTS)*8,
// narrowed to float for the accumulators (game-second timestamps fit comfortably).
static float SlotVal(const uint8_t* fth, int i) {
    if (i < FTH_FLOAT_SLOTS) { float v; memcpy(&v, fth + (size_t)i * 4, 4); return v; }
    double d; memcpy(&d, fth + (size_t)(i - FTH_FLOAT_SLOTS) * 8, 8);
    return (float)d;
}
static void SlotName(int i, char* out, int cap) {
    if (i < FTH_FLOAT_SLOTS) snprintf(out, cap, "f+0x%03x", i * 4);
    else                     snprintf(out, cap, "d+0x%03x", (i - FTH_FLOAT_SLOTS) * 8);
}
// Where each slot ENDED the previous crank -- the latch report's baseline. A start timestamp shows
// up as: changed between the cranks, dead still during this one.
static float g_prevEnd[SCAN_SLOTS];
static bool  g_prevValid = false;

static void ScanReset(const uint8_t* fth) {
    for (int i = 0; i < SCAN_SLOTS; i++) {
        const float v = SlotVal(fth, i);
        SlotAcc& a = g_acc[i];
        a.first = a.last = a.mn = a.mx = v;
        a.sv = a.svv = a.stv = a.smv = 0.0;
    }
    g_st = g_stt = g_sm = g_smm = 0.0; g_sn = 0;
}
static void ScanFeed(const uint8_t* fth, double t, double m) {
    g_st += t; g_stt += t * t; g_sm += m; g_smm += m * m; g_sn++;
    for (int i = 0; i < SCAN_SLOTS; i++) {
        const float v = SlotVal(fth, i);
        if (!(v == v)) continue;                  // NaN reinterpretations are not data
        SlotAcc& a = g_acc[i];
        if (v < a.mn) a.mn = v;
        if (v > a.mx) a.mx = v;
        a.last = v;
        a.sv += v; a.svv += v * v; a.stv += t * v; a.smv += m * v;
    }
}
static float Corr(double n, double sx, double sxx, double sxy, double sy, double syy) {
    const double num = n * sxy - sx * sy;
    const double den = sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    return (den > 1e-9) ? (float)(num / den) : 0.0f;
}

static void ReportAlphaCandidates(double heldS) {
    if (g_sn < 8) return;                         // too short a crank to correlate anything
    const double n = (double)g_sn;
    // Ranking by range would drown the list in velocities; rank by how well the slot rides the
    // crank instead, and print both correlations so duration-driven and depth-driven candidates are
    // distinguishable at a glance.
    struct Hit { int i; float range, ct, cm; };
    Hit hits[16]; int nh = 0;
    for (int i = 0; i < SCAN_SLOTS; i++) {
        const SlotAcc& a = g_acc[i];
        const float range = a.mx - a.mn;
        if (!(range > 0.02f) || fabsf(a.mn) > 10000.0f || fabsf(a.mx) > 10000.0f) continue;
        const float ct = Corr(n, g_st, g_stt, a.stv, a.sv, a.svv);
        const float cm = Corr(n, g_sm, g_smm, a.smv, a.sv, a.svv);
        const float best = fabsf(ct) > fabsf(cm) ? fabsf(ct) : fabsf(cm);
        if (best < 0.90f) continue;               // does not ride the crank
        if (nh < 16) { hits[nh].i = i; hits[nh].range = range; hits[nh].ct = ct; hits[nh].cm = cm; nh++; }
    }
    for (int a2 = 0; a2 < nh; a2++) for (int b2 = a2 + 1; b2 < nh; b2++) {
        const float ba = fabsf(hits[a2].ct) > fabsf(hits[a2].cm) ? fabsf(hits[a2].ct) : fabsf(hits[a2].cm);
        const float bb = fabsf(hits[b2].ct) > fabsf(hits[b2].cm) ? fabsf(hits[b2].ct) : fabsf(hits[b2].cm);
        if (bb > ba) { Hit t = hits[a2]; hits[a2] = hits[b2]; hits[b2] = t; }
    }
    char nm[16];
    for (int k = 0; k < nh && k < 10; k++) {
        const SlotAcc& a = g_acc[hits[k].i];
        SlotName(hits[k].i, nm, sizeof(nm));
        TwkLogDev("[pop]   candidate fth%s: %.3f -> %.3f (range %.3f) corrTime=%+.2f corrDepth=%+.2f",
               nm, a.first, a.last, hits[k].range, hits[k].ct, hits[k].cm);
    }
    if (!nh) TwkLogDev("[pop]   nothing on the handler accumulates with the crank (%.0f ms, %d samples)",
                    heldS * 1000.0, g_sn);
    // ---- the latch report: a slot that jumped between the cranks and then held dead still through
    // this one is how a START TIMESTAMP behaves -- set once at crank entry, read at the pop.
    if (g_prevValid) {
        int shown = 0;
        for (int i = 0; i < SCAN_SLOTS && shown < 8; i++) {
            const SlotAcc& a = g_acc[i];
            if (!(a.mx - a.mn < 0.005f)) continue;                    // moved during the crank
            const float jump = fabsf(a.first - g_prevEnd[i]);
            if (!(jump > 0.01f) || !(jump < 1e7f)) continue;          // did not change between cranks
            if (fabsf(a.first) > 1e7f) continue;
            SlotName(i, nm, sizeof(nm));
            TwkLogDev("[pop]   latched at crank entry fth%s: %.3f (was %.3f last crank)",
                   nm, a.first, g_prevEnd[i]);
            shown++;
        }
    }
    for (int i = 0; i < SCAN_SLOTS; i++) g_prevEnd[i] = g_acc[i].last;
    g_prevValid = true;
}

static void PopProbe_NoteCrankForInjection();
void PopProbe_PadReport();
static int  PopProbe_InstallPadHooks();    // defined with the pad hooks below
static bool PopProbe_PadPolled();
// ------------------------------------------------------------------ finding the handler
// The skater owns its handlers; the FlipTricksHandler is the pointer slot whose target back-points
// to the skater (+0x08) and to the tricks database (+0x28). Scanned once, re-verified every tick --
// a map change replaces the skater and the cached pointer must never be read stale.
static void* g_fth = nullptr;
static bool  g_fthLogged = false;

static void* ResolveFth() {
    void* skater = CatchTweaks_Skater();
    if (!skater) return nullptr;
    __try {
        if (g_fth && twkP(g_fth, FTH_SKATER) == skater) return g_fth;   // still the live one
        g_fth = nullptr;
        // The handler is an EMBEDDED sub-object, not a pointer the skater carries -- a scan of the
        // skater's pointer slots found nothing across a whole session (and said nothing, which cost
        // a test run; hence the log line below). scoop_speed's trick hook is handed the handler as
        // `this` on every trick, so it is taken from there, identity-verified.
        void* fth = ScoopSpeed_FlipHandler();
        if (fth && twkP(fth, FTH_SKATER) == skater) {
            g_fth = fth;
            if (!g_fthLogged) {
                g_fthLogged = true;
                TwkLogDev("[pop] FlipTricksHandler in hand (%p, via the scoop hook) -- scanning it from "
                       "the next crank on", fth);
            }
            g_prevValid = false;                  // a new handler invalidates the latch baseline
            return g_fth;
        }
        static bool waitLogged = false;           // separate from g_fthLogged, or the wait message
        if (!waitLogged) {                        // would eat the later success line
            waitLogged = true;                    // once: silence here already wasted one field run
            TwkLogDev("[pop] handler not seen yet -- it is captured on the first trick, so this crank "
                   "goes unscanned and the next one will not");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_fth = nullptr; }
    return nullptr;
}

// ------------------------------------------------------------------ per-tick sampling
void PopProbe_PumpFrame() {
    if (!g_on) return;
    void* an = FootPlace_AnimInstance();
    if (!an) return;

    float lx = 0, ly = 0, rx = 0, ry = 0;
    const bool haveL = ScoopSpeed_StickRaw(false, &lx, &ly);
    const bool haveR = ScoopSpeed_StickRaw(true,  &rx, &ry);
    if (!haveL && !haveR) return;                 // no live input tick yet

    bool crank = false, grounded = false, grinding = false;
    __try {
        crank    = FootPlace_SettingUpTrick();
        grounded = twkB(an, AN_GROUNDED) != 0;
        g_goofy  = twkB(an, AN_IS_GOOFY) ? 1 : 0;
        grinding = (twkB(an, AN_IS_GRINDING) | twkB(an, AN_IS_GRINDING + 1)) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    const double t = NowS();
    Tick& s = g_ring[g_wr];
    s.t = t; s.lx = lx; s.ly = ly; s.rx = rx; s.ry = ry;
    s.crank = crank ? 1 : 0; s.grounded = grounded ? 1 : 0;
    g_wr = (g_wr + 1) % kTicks;
    if (g_n < kTicks) g_n++;

    // ---- the pad hook's context gate: grounded + on the board + not switch/fakie, stamped so the
    // hook can tell a live gameplay tick from a paused menu (see the declaration note).
    {
        long allow = 0;
        if (grounded) {
            void* skater = CatchTweaks_Skater();
            void* mv = skater ? twkP(skater, SK_MOVE_COMP) : nullptr;
            const bool onBoard = mv && twkB(mv, MC_ON_BOARD) > 0;
            bool sw = false;
            if (onBoard) FlipSpeed_Stance(skater, nullptr, &sw);
            g_padSw = sw ? 1 : 0;                 // the hook's mode table swaps sticks on this
            // With stances on, switch riding is a supported mode (the hook mirrors the sticks);
            // with them off, the original switch veto stands and the scheme goes vanilla there.
            allow = (onBoard && (g_stances || !sw)) ? 1 : 0;
        }
        g_padAllow = allow;
        g_padGrind = grinding ? 1 : 0;            // grinds/liptricks: total hands-off (see hook)
        g_padCrank = crank ? 1 : 0;               // the pop machine's "game is cranked" signal
        // Log every input-mode transition -- the one state the log never showed, and the prime
        // suspect whenever a gesture reads on the wrong stick (mode decides which physical stick
        // the machine treats as crouch vs crank).
        // The quick-shove gate's visibility: every block shows up here, so the field log proves
        // whether the gate fired (count climbs) or the shove came from something else (silent).
        {
            static long saidBlocked = 0; static double saidAt3 = 0.0;
            const long b = g_quickBlocked;
            if (b != saidBlocked && t - saidAt3 > 1.0) {
                saidAt3 = t;
                TwkLogDev("[pop] quick-shove gate: %ld blocked so far (+%ld)", b, b - saidBlocked);
                saidBlocked = b;
            }
        }
        {
            static long wasMode = -1;
            const long m = g_padModeDbg;
            if (m != wasMode) {
                if (wasMode >= 0)
                    TwkLogDev("[pop] input mode -> %s family%s (LS %+.2f,%+.2f RS %+.2f,%+.2f)",
                           (m & 1) ? "NOLLIE" : "ollie", (m & 2) ? " SWITCH" : "",
                           g_physLx / 32767.0f, g_physLy / 32767.0f,
                           g_physRx / 32767.0f, g_physRy / 32767.0f);
                wasMode = m;
            }
        }
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        g_padStamp = q.QuadPart;

        // ---- the manual gate's verdict (see the gate block above). Entry is forbidden ONLY in
        // the diagnosed danger window: while the scheme is mid-gesture (crouched or pop armed),
        // and from each pop until its landing has settled -- grounded for g_landSettleMs after
        // actually being airborne, or earlier the moment both physical sticks come home (a clean
        // recovery reopens vanilla instantly; a stick still held g_landSettleMs after touchdown
        // is a player asking for a manual and gets one). Outside the window there is NOTHING
        // between the player and a vanilla manual.
        {
            static long long freq2 = 0;
            if (!freq2) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq2 = f.QuadPart; }
            const bool physLive = (q.QuadPart - g_physStamp) < freq2 / 10;   // pad actually polled
            static double guardUntil = 0.0;       // absolute cap on the post-pop window
            static double landAt = -1.0;          // when the post-pop landing began
            static bool   wasAir = false;         // did this pop actually leave the ground?
            static long   wasArmed = 0;
            if (g_padArmed && !wasArmed) { guardUntil = t + 2.0; landAt = -1.0; wasAir = false; }
            wasArmed = g_padArmed;
            bool popGuard = false;
            if (t < guardUntil) {
                if (!grounded) { wasAir = true; landAt = -1.0; }
                else if (wasAir && landAt < 0.0) landAt = t;
                const long pLx = g_physLx, pLy = g_physLy, pRx = g_physRx, pRy = g_physRy;
                const long mL = (pLx < 0 ? -pLx : pLx) + (pLy < 0 ? -pLy : pLy);
                const long mR = (pRx < 0 ? -pRx : pRx) + (pRy < 0 ? -pRy : pRy);
                const bool home = mL < 3500 && mR < 3500;
                const bool settled = landAt >= 0.0 &&
                                     (t - landAt) * 1000.0 >= (double)g_landSettleMs;
                if (home || settled) guardUntil = 0.0; else popGuard = true;
            }
            const bool danger = g_padActive || g_padArmed || popGuard;
            g_forbidManual = (SchemeOn() && g_manualGate && physLive && danger) ? 1 : 0;
        }
        // The definitive field signal: log every flip of the skater's manual bits. If a manual
        // still starts with forbid=1 on this line, the funnel assumption is wrong -- an entry
        // path outside Skate_CheckForManuals exists and the xref hunt reopens.
        {
            static int wasBits = 0;
            int bits = 0;
            __try {
                void* sk = CatchTweaks_Skater();
                bits = sk ? (twkB(sk, SK_MANUAL_BITS) & MANUAL_BITS_MASK) : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { bits = wasBits; }
            g_inManual = bits ? 1 : 0;            // the pad hook's masks stand down while live
            if (bits != wasBits) {
                TwkLogDev("[pop] manual bits %d -> %d (forbid=%ld physLY=%ld physRY=%ld active=%ld armed=%ld)",
                       wasBits, bits, g_forbidManual, g_physLy, g_physRy, g_padActive, g_padArmed);
                wasBits = bits;
            }
        }
    }

    // ---- drain the leak-evidence ring (see its declaration). Throttled: consecutive samples are
    // near-identical (a held thumb records every poll), so a line prints on a kind change or every
    // 250 ms, carrying the count of samples it stands for. Any 'L' line outside a gesture (fast<3,
    // sit past 30 ms) is the manual leak caught red-handed; a manual with NO line = not input.
    {
        static double lastLine = 0.0; static char lastKind = 0; static int held = 0;
        const LONG wr = g_leakWr;
        while (g_leakRd < wr) {
            if (wr - g_leakRd > 32) g_leakRd = wr - 32;          // writer lapped; keep the newest
            const PadLeakRec r = g_leakRecs[((g_leakRd % 32) + 32) % 32];
            g_leakRd++;
            held++;
            if (r.kind != lastKind || t - lastLine > 0.25) {
                TwkLogDev("[pop] SHOWN %c-band: outLY=%d outLX=%d outRY=%d physLY=%d mov=%d fast=%d (x%d)",
                       r.kind, r.outLy, r.outLx, r.outRy, r.phLy, r.mov, r.fast, held);
                lastLine = t; lastKind = r.kind; held = 0;
            }
        }
    }

    // ---- LS depth IS the pop height: while the scheme is crouching (or a pop is armed with its
    // frozen depth), the crank clock is written from the stick instead of accruing on the game's
    // timer. Same write ForceCrank proved -- 19 bit-identical pops -- just fed live. An explicit
    // ForceCrank experiment still wins if someone sets both.
    // The handler is resolved HERE, not left to the dev scan: ResolveFth() used to be called
    // only on the g_scan (PopProbeScanAlpha) path, so the release build -- which ships the
    // scan OFF -- never populated g_fth on a fresh ini and the depth write silently never
    // fired: every pop at the natural crank timing, i.e. constant height (Epic field report;
    // Steam only kept working because its older ini still said ScanAlpha=1). A dev knob must
    // never be load-bearing for a shipped feature.
    if (SchemeOn() && g_depthPop && g_force < 0 && crank && ResolveFth() &&
        (g_padActive || g_padArmed)) {
        const float depth = BitsToF(g_padArmed ? g_padArmedDepthBits : g_padDepthBits);
        const float held  = kRatio0 + depth * kRatioSpan;
        __try { *(float*)((uint8_t*)g_fth + FTH_CRANK_CLOCK) = held; }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_depthPop = 0; }
        static double saidAt2 = 0.0;
        if (t - saidAt2 > 1.0) { saidAt2 = t;
            TwkLogDev("[pop] depth-pop: crouch %.0f%% -> clock %.3fs (expect popRatio %.2f)%s",
                   depth * 100.0f, held, depth, g_padArmed ? " [ARMED]" : "");
        }
    }

    // ---- CROUCH VISUAL, round 2 (the blend space player's own clock; UNVERIFIED). Round 52's
    // CrankInStartTime write was a dead letterbox (see the offsets note). The live descent clock
    // is the CrankIn player node's InternalTimeAccumulator, found by scanning the anim BP's
    // property area (0x7e8 up -- above the native class, where the old value scans never looked)
    // for the CrankInBlendSpace POINTER; node base = hit - 0x50. The ascending scan meets the
    // node's own BlendSpace slot before its PreviousBlendSpace copy at +0xe0, and field sanity
    // rejects lookalikes. While the crouch is held the clock is written to depth * T every tick;
    // the graph advances it one frame past our value before evaluating, a constant bias too small
    // to see. The 1 Hz line prints the pre-write readback: tracking our last write = the write
    // sticks; diverging = a sync group or another writer owns the clock and the write point must
    // move post-anim-update. Writing stops at the pop, which plays from wherever the pose sat.
    {
        static void*     nodeAn   = nullptr;     // anim instance the offsets belong to
        static uintptr_t inOff    = 0;           // CrankIn player node base; 0 = not found
        static uintptr_t loopOff  = 0;           // CrankLoop player node base (diagnostic + plan B)
        static double    nextScan = 0.0;
        static int       scans    = 0;
        static float     origRate = 1.5f;        // the node's own PlayRate, restored after a scrub
        static bool      ratePinned = false;
        static float     visClock = -1.0f;       // the smoothed visual clock; -1 = fresh crouch
        static double    lastVisT = 0.0;
        static double    lingerFrom = 0.0;       // release linger: rate stays 0 through blend-out
        const bool scrub = SchemeOn() && g_crankVis && crank && g_padActive;
        if (an != nodeAn) { nodeAn = an; inOff = 0; loopOff = 0; nextScan = 0.0; scans = 0;
                            ratePinned = false; visClock = -1.0f; lingerFrom = 0.0; }
        // Round 53's scan matched only CrankIn, once, 17 ms into the first crank, then gave up --
        // three ways to lose at once: a pin-bound node copies its asset pointer in during its own
        // UPDATE (possibly not yet run at state entry), 0x10000 can undershoot a graph this size,
        // and a held crank does not even sit in CrankIn -- the descent is transient, the HOLD is
        // the CrankLoop player (whose axis input may be the real depth lever). So: rescan every
        // second while crouched (never give up), range to 0x40000, and match the In AND Loop
        // asset pointers, logging every hit's live fields so the next log names the true lever.
        if (scrub && !inOff && t >= nextScan) {
            nextScan = t + 1.0;
            __try {
                void* bsIn   = twkP(an, AN_CRANKIN_BS);
                void* bsLoop = twkP(an, AN_CRANKIN_BS + 8);      // CrankLoopBlendSpace (+0x4a8)
                if (!bsIn && !bsLoop) {
                    if (scans++ == 0) TwkLogDev("[pop] crankvis: crank blend spaces still null -- retrying");
                } else {
                    for (uintptr_t off = 0x7e8; off < 0x40000; off += 8) {
                        void* p = *(void**)((uint8_t*)an + off);
                        if (!p || (p != bsIn && p != bsLoop)) continue;
                        const char* which = (p == bsIn) ? "CrankIn" : "CrankLoop";
                        uint8_t* node = (uint8_t*)an + off - BSP_BS;
                        const float w  = *(float*)(node + BSP_WEIGHT);
                        const float ta = *(float*)(node + BSP_TIME);
                        const float pr = *(float*)(node + BSP_PLAYRATE);
                        const bool sane = w >= -0.001f && w <= 1.001f && ta >= -0.001f &&
                                          ta < 100.0f && pr > -100.0f && pr < 100.0f;
                        TwkLogDev("[pop] crankvis: %s ptr at an+0x%llx (weight %.2f time %.3f rate %.2f "
                               "X %.2f Y %.2f loop %d group %u) %s",
                               which, (unsigned long long)off, w, ta, pr, *(float*)(node + BSP_X),
                               *(float*)(node + BSP_X + 4), (int)*(uint8_t*)(node + BSP_LOOP),
                               *(unsigned*)(node + BSP_GROUP),
                               sane ? "<- node" : "(not node-shaped, skipped)");
                        if (sane && p == bsIn   && !inOff)   inOff   = off - BSP_BS;
                        if (sane && p == bsLoop && !loopOff) loopOff = off - BSP_BS;
                    }
                    if (!inOff && (++scans % 10) == 1)
                        TwkLogDev("[pop] crankvis: no CrankIn node yet in an+0x7e8..0x40000 (scan %d) -- "
                               "still retrying each second while crouched", scans);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // The instance ended before 0x40000 -- everything on-object was already compared,
                // so this is a completed no-match scan, not a failure: keep the 1 s retry (a
                // pin-bound node may simply not have copied its asset pointer in yet).
                if (!inOff && (++scans % 10) == 1)
                    TwkLogDev("[pop] crankvis: scan reached the object edge, no CrankIn node yet (scan %d)", scans);
            }
        }
        if (scrub && inOff) {
            lingerFrom = 0.0;                    // a new crouch cancels any release linger
            __try {
                uint8_t* node = (uint8_t*)an + inOff;
                // THE CHURN FIX (round 54's field log): the write stuck ("was" = ours + one frame
                // at rate 1.5, weight 1.00) but the state kept EXITING and RE-ENTERING (weight
                // 0.01 + accumulator reset to one frame from zero, over and over during a held
                // crouch = the pose popping toward standing and blending back). State machines
                // auto-exit on TIME REMAINING, and a clock parked deep in the clip keeps the exit
                // rule armed. Remaining time is computed against PlayRate, so rate 0 = infinite
                // remaining = the state CANNOT auto-exit, and the clock stops self-advancing (no
                // one-frame bias either). Restored the moment the scrub ends.
                if (!ratePinned) {
                    origRate = *(float*)(node + BSP_PLAYRATE);
                    if (origRate == 0.0f) origRate = 1.5f;      // never restore a stuck zero
                    ratePinned = true;
                }
                *(float*)(node + BSP_PLAYRATE) = 0.0f;
                float* clock = (float*)(node + BSP_TIME);
                const float before = *clock;
                const float depth = BitsToF(g_padDepthBits);
                // Depth maps into [min..Time]: the clip opens with the pre-crouch foot hop, so
                // engaging the crouch lands past it and a slight pull is already a slight crouch.
                const float lo = (float)g_crankVisMin / 1000.0f;
                const float target = lo + depth * ((float)g_crankVisTime / 1000.0f - lo);
                // The pose EASES toward the stick instead of tracking it 1:1 -- raw tracking put
                // every stick wobble straight into the body (field: slow pulls jerky/mechanical).
                // First-order time constant; a fresh crouch starts at the floor and eases in.
                const float tau = (float)g_crankVisSmoothMs / 1000.0f;
                if (visClock < 0.0f || tau <= 0.0f) visClock = (visClock < 0.0f) ? lo : target;
                else {
                    float vdt = (float)(t - lastVisT);
                    if (vdt < 0.0f) vdt = 0.0f; else if (vdt > 0.05f) vdt = 0.05f;
                    visClock += (target - visClock) * (1.0f - expf(-vdt / tau));
                }
                lastVisT = t;
                *clock = visClock;
                g_visPubBits = FToBits(visClock);          // OpenMP reads this pair -- see the export
                g_visPubTick = (long long)GetTickCount64();
                static double saidVis = 0.0;
                if (t - saidVis > 1.0) { saidVis = t;
                    const float inW = *(float*)(node + BSP_WEIGHT);
                    if (loopOff) {
                        uint8_t* ln = (uint8_t*)an + loopOff;
                        TwkLogDev("[pop] crankvis: depth %.0f%% -> In clock %.3fs (was %.3f, In w %.2f, rate pinned) | "
                               "Loop w %.2f time %.3f X %.2f Y %.2f",
                               depth * 100.0f, *clock, before, inW, *(float*)(ln + BSP_WEIGHT),
                               *(float*)(ln + BSP_TIME), *(float*)(ln + BSP_X), *(float*)(ln + BSP_X + 4));
                    } else {
                        TwkLogDev("[pop] crankvis: depth %.0f%% -> In clock %.3fs (was %.3f, In w %.2f, rate pinned) | no Loop node",
                               depth * 100.0f, *clock, before, inW);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                __try { if (ratePinned) *(float*)((uint8_t*)an + inOff + BSP_PLAYRATE) = origRate; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                ratePinned = false; inOff = 0; g_crankVis = 0;
            }
        } else if (ratePinned && inOff) {
            // Scrub over (pop armed, crouch released, crank dropped). Round-56 field: restoring
            // the rate HERE let the frozen descent resume playing forward as the exit blend's
            // SOURCE -- releasing a shallow crouch finished the full dive before standing. So the
            // rate stays pinned through the blend-out: the pose holds at the released depth and
            // the exit blend carries it to standing FROM THERE. The rate is handed back once the
            // node's blend weight collapses (capped -- if the weight never drops, the graph's
            // exit needs the clip end and this linger only delays it; the log line says which).
            bool handBack = false;
            __try {
                const float w = *(float*)((uint8_t*)an + inOff + BSP_WEIGHT);
                if (lingerFrom == 0.0) lingerFrom = t;
                handBack = (w < 0.05f) || (t - lingerFrom > 0.5);
                if (handBack)
                    TwkLogDev("[pop] crankvis: release -- In weight %s after %.0f ms",
                           w < 0.05f ? "faded" : "STILL UP (cap hit)", (t - lingerFrom) * 1000.0);
            } __except (EXCEPTION_EXECUTE_HANDLER) { handBack = true; }
            if (handBack) {
                __try { *(float*)((uint8_t*)an + inOff + BSP_PLAYRATE) = origRate; }
                __except (EXCEPTION_EXECUTE_HANDLER) { inOff = 0; }
                ratePinned = false; lingerFrom = 0.0;
            }
            visClock = -1.0f;                    // next crouch eases in fresh from the floor
        }
    }

    __try {
        // ---- mode-3 bookkeeping: the pad hook only flips g_padActive (it may run off-thread and
        // must not log or touch game objects); the events and the crank-success window live here.
        static long wasPad = 0;
        const long pad = g_padActive;
        if (pad != wasPad) {
            wasPad = pad;
            g_injActive = (pad != 0);
            if (pad) {
                g_injT0 = t; g_injCranked = false; g_injNoCrankSaid = false;
                TwkLogDev("[pop] INJECTING (mode %d): the game now reads LS neutral + RS full down",
                          g_inject);
            } else {
                TwkLogDev("[pop] pad injection released (%s)",
                       g_injCranked ? "the game had cranked" : "no crank was seen");
            }
        }
        // Said ONCE per session, unconditionally: a crouch that never cranks means this pad is
        // not read through XInput, i.e. the scheme is silently doing nothing -- the one thing a
        // player needs told without turning diagnostics on.
        static bool saidNoCrank = false;
        if (pad && !g_injCranked && !saidNoCrank && t - g_injT0 > 0.6) {
            saidNoCrank = true; g_injNoCrankSaid = true;
            if (g_inject == 3)
                TwkLog("[pop] the crouch was held 600 ms and the game never cranked -- this "
                       "controller is not polled through XInput, so the pad-level path cannot "
                       "drive it (PopProbeInject=4 uses the game-side path instead)");
            else
                TwkLog("[pop] the crouch was held 600 ms and the game never cranked -- the "
                       "game-side injection did not reach the crank logic; please report this "
                       "with the log");
        }
        // THE SUPPORT VERDICT (mode 3 only -- mode 4 needs no XInput), said once,
        // unconditionally. Every other part of the scheme can
        // load perfectly and still do nothing if the game reads this controller somewhere other
        // than XInput (no Steam Input layer in front of a DirectInput-only pad, GameInput). That
        // is invisible otherwise -- the player just sees "it does nothing" -- so it gets one plain
        // line either way, and until polls appear the hooks are re-probed in case the input path
        // bound late.
        if (g_inject == 3) {
            static double onSince = 0.0, lastProbe = 0.0, lastRep = 0.0;
            static bool   saidVerdict = false;
            if (onSince == 0.0) { onSince = t; lastProbe = t; }
            const bool polled = PopProbe_PadPolled();
            if (!polled && t - lastProbe > 2.0 && t - onSince < 60.0) {
                lastProbe = t;
                PopProbe_InstallPadHooks();        // an XInput DLL may have loaded since startup
            }
            if (!saidVerdict && t - onSince > 8.0) {
                saidVerdict = true;
                if (polled) PopProbe_PadReport();  // which function the game calls, and how often
                else {
                    TwkLog("[pop] the pop control scheme is ON but nothing has polled XInput in "
                           "8 s -- this controller is being read another way (no Steam Input "
                           "layer, a DirectInput-only pad, or GameInput), so the scheme cannot "
                           "drive it");
                    // Name the backend: which input DLLs the process actually loaded. This is
                    // what decides where an alternate hook would go, and it costs one line.
                    static const char* kProbe[] = { "dinput8.dll", "gameinput.dll",
                                                    "xinput1_4.dll", "xinput1_3.dll",
                                                    "xinput9_1_0.dll", "hid.dll" };
                    char have[192]; int used = 0;
                    for (int i = 0; i < 6; i++) {
                        if (!GetModuleHandleA(kProbe[i])) continue;
                        const int w = snprintf(have + used, sizeof(have) - used, "%s%s",
                                               used ? ", " : "", kProbe[i]);
                        if (w > 0 && used + w < (int)sizeof(have) - 1) used += w;
                    }
                    TwkLog("[pop] input DLLs loaded: %s", used ? have : "(none of the usual ones)");
                }
            }
            if (g_log && t - lastRep > 3.0) { lastRep = t; PopProbe_PadReport(); }
        }

        // ---- crank edges
        if (crank && !g_wasCrank) {
            g_crankT0 = t; g_crankT1 = 0.0;
            g_maxL = g_maxR = 0.0f;
            g_endLx = g_endLy = g_endRx = g_endRy = 0.0f;
            if (g_scan) { void* fth = ResolveFth(); if (fth) ScanReset((const uint8_t*)fth); }
            TwkLogDev("[pop] CRANK start  LS=(%+.2f,%+.2f) RS=(%+.2f,%+.2f) goofy=%d grounded=%d",
                   lx, ly, rx, ry, g_goofy, grounded ? 1 : 0);
            PopProbe_NoteCrankForInjection();     // the injection experiment's success signal
        }
        if (crank) {
            const float ml = sqrtf(lx*lx + ly*ly), mr = sqrtf(rx*rx + ry*ry);
            if (ml > g_maxL) { g_maxL = ml; g_endLx = lx; g_endLy = ly; }
            if (mr > g_maxR) { g_maxR = mr; g_endRx = rx; g_endRy = ry; }
            // The crank stick's CURRENT deflection is the depth signal the correlation runs against;
            // whichever stick is deeper is the one doing the cranking (RS for ollies, LS for nollies).
            if (g_scan && g_fth) ScanFeed((const uint8_t*)g_fth, t - g_crankT0, ml > mr ? ml : mr);
            // Round 1: hold the crank clock at the forced percent. Written every crank tick, so the
            // game's own += dt each frame drifts it by at most one frame before the next write; at
            // the pop, CheckForTrick reads a clock that says "held exactly this long". SEH-guarded
            // like every other game write, and the whole branch is dead at the default -1.
            if (g_force >= 0 && g_fth) {
                const float held = kRatio0 + (g_force / 100.0f) * kRatioSpan;
                __try { *(float*)((uint8_t*)g_fth + FTH_CRANK_CLOCK) = held; }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_force = -1; }
                static double saidAt = 0.0;
                if (t - saidAt > 2.0) { saidAt = t;
                    TwkLog("[pop] FORCING crank clock to %.3fs -> expect popRatio %.2f on every pop "
                           "regardless of hold; watch whether the crouch freezes with it",
                           held, g_force / 100.0f);
                }
            }
        }
        if (!crank && g_wasCrank) {
            g_crankT1 = t;
            const double held = t - g_crankT0;
            TwkLogDev("[pop] CRANK end after %.0f ms  peak LS=%.2f@%.0fdeg RS=%.2f@%.0fdeg",
                   held * 1000.0, g_maxL, StickAngle(g_endLx, g_endLy),
                   g_maxR, StickAngle(g_endRx, g_endRy));
            if (g_scan && g_fth) ReportAlphaCandidates(held);
        }
        g_wasCrank = crank;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_on = 0; TwkLog("[pop] probe faulted -- off for this session"); }
}

// ------------------------------------------------------------------ per-trick correlation + trace
// Two feeds arrive per trick -- JumpForTrick's ring and the pitch drain's START record -- and either
// can be the one that works (the ring was dark for a long time without anyone noticing). Whichever
// lands first prints the block; the other, inside half a second, stays quiet.
static double g_lastCorrT = 0.0;
static bool CorrDedupe(double t) {
    if (g_lastCorrT > 0.0 && t - g_lastCorrT < 0.5) return true;
    g_lastCorrT = t;
    return false;
}
static void DumpTrace(double t);

void PopProbe_OnJump(const char* trick, int onGrind, float argHeight, float argPopRatio,
                     float grindRatio, float trickPopRatio) {
    if (!g_on) return;
    __try {
        const double t = NowS();
        if (CorrDedupe(t)) return;
        const double heldMs  = (g_crankT0 > 0.0)
                             ? ((g_crankT1 > g_crankT0 ? g_crankT1 : t) - g_crankT0) * 1000.0 : 0.0;
        const double sinceMs = (g_crankT1 > 0.0) ? (t - g_crankT1) * 1000.0 : 0.0;
        TwkLogDev("[pop] JUMP trick='%s'%s  popRatio=%.3f trickPopRatio=%.3f height=%.1f grindRatio=%.3f "
               "| crank held %.0f ms (ended %.0f ms before the jump) peak LS=%.2f@%.0fdeg "
               "RS=%.2f@%.0fdeg goofy=%d",
               trick ? trick : "?", onGrind ? " (grind)" : "", argPopRatio, trickPopRatio, argHeight,
               grindRatio, heldMs, sinceMs, g_maxL, StickAngle(g_endLx, g_endLy),
               g_maxR, StickAngle(g_endRx, g_endRy), g_goofy);
        DumpTrace(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The pitch-drain feed: no trick name or height here, but TrickPopRatio is the same crank-derived
// number, so the duration -> pop correlation still gets its line per trick.
void PopProbe_OnArm(float trickPopRatio) {
    if (!g_on) return;
    __try {
        const double t = NowS();
        if (CorrDedupe(t)) return;
        const double heldMs  = (g_crankT0 > 0.0)
                             ? ((g_crankT1 > g_crankT0 ? g_crankT1 : t) - g_crankT0) * 1000.0 : 0.0;
        TwkLogDev("[pop] ARM  trickPopRatio=%.3f | crank held %.0f ms  peak LS=%.2f@%.0fdeg "
               "RS=%.2f@%.0fdeg goofy=%d  (pitch-drain feed; the jump ring stayed quiet)",
               trickPopRatio, heldMs, g_maxL, StickAngle(g_endLx, g_endLy),
               g_maxR, StickAngle(g_endRx, g_endRy), g_goofy);
        DumpTrace(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The raw stream the redesign has to reproduce: ~1.2 s of both sticks, downsampled to a dozen rows.
// C = cranking, G = grounded.
static void DumpTrace(double t) {
    __try {
        const int want = 14;
        int have = 0;
        int idx[kTicks];
        for (int i = 0; i < g_n; i++) {
            const int k = (g_wr - 1 - i + kTicks * 2) % kTicks;
            if (t - g_ring[k].t > 1.2) break;
            idx[have++] = k;
        }
        const int step = (have > want) ? (have / want) : 1;
        for (int i = have - 1; i >= 0; i -= step) {
            const Tick& s = g_ring[idx[i]];
            TwkLogDev("[pop]   t-%4.0fms LS(%+.2f,%+.2f) RS(%+.2f,%+.2f) %c%c",
                   (t - s.t) * 1000.0, s.lx, s.ly, s.rx, s.ry,
                   s.crank ? 'C' : '.', s.grounded ? 'G' : '.');
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ------------------------------------------------------------------ the injection experiment
static void PopProbe_NoteCrankForInjection() {
    if (!g_injActive || g_injCranked) return;
    g_injCranked = true;
    TwkLogDev("[pop] INJECTION PROVEN (mode %d): the game entered its crank %.0f ms after the synthetic "
           "RS-down, with the physical right stick untouched", g_inject,
           (NowS() - g_injT0) * 1000.0);
}

static void InjectAt(void* ih, int mode) {
    if (g_inject != mode || !g_on || !ih) return;
    __try {
        // The user's PHYSICAL left stick. Where to read it depends on the write point, and getting
        // this wrong produced a tick-rate flip-flop in the field: mode 2's write PERSISTS into the
        // next tick's entry, where the shared stick sampler runs -- so the gate read back its own
        // zeroed LS, released, re-armed on the game's refresh, ~8 ms around, and the game saw a
        // 60 Hz flicker instead of a held stick (which it rightly refused to crank from). The
        // flip-flop itself proved the field lifecycle: the game refreshes the raw sticks INSIDE its
        // tick -- so at the LATE point they have just been refreshed and are guaranteed physical.
        // Mode 2 therefore reads them directly, immediately before overwriting them; mode 1 runs
        // before any write of ours can be in the fields, so the entry samples are fine there.
        float lx = 0, ly = 0;
        if (mode == 2) {
            const float* L = (const float*)((const uint8_t*)ih + IH_RAW_LEFT);
            lx = L[0]; ly = L[1];
            if (!(fabsf(lx) <= 1.5f) || !(fabsf(ly) <= 1.5f)) return;   // implausible = bad read
        } else {
            if (!ScoopSpeed_StickRaw(false, &lx, &ly)) return;
        }
        bool want = (ly < -kInjectGate);
        if (want) {                                // grounded only: airborne RS-down is a grab/lean
            void* an = FootPlace_AnimInstance();
            bool grounded = false;
            if (an) { __try { grounded = twkB(an, AN_GROUNDED) != 0; }
                      __except (EXCEPTION_EXECUTE_HANDLER) {} }
            if (!grounded) want = false;
        }
        if (want) {
            float* L = (float*)((uint8_t*)ih + IH_RAW_LEFT);
            float* R = (float*)((uint8_t*)ih + IH_RAW_RIGHT);
            L[0] = 0.0f; L[1] = 0.0f;              // the game must not see the user's crouch stick
            R[0] = 0.0f; R[1] = -1.0f;             // full deflection, or the game manuals instead
            if (!g_injActive) {
                const double now = NowS();
                g_injActive = true;
                // Only a re-engage after real quiet is worth a line -- the flip-flop bug printed
                // hundreds of these at tick rate, and a log that floods is a log nobody reads.
                static double lastSaid = 0.0;
                if (now - lastSaid > 0.5) {
                    lastSaid = now;
                    g_injT0 = now; g_injCranked = false; g_injNoCrankSaid = false;
                    TwkLogDev("[pop] INJECTING (mode %d): physical LS %.2f down -> game is fed LS "
                           "neutral + RS full down", mode, -ly);
                }
            } else if (!g_injCranked && !g_injNoCrankSaid && NowS() - g_injT0 > 0.6) {
                g_injNoCrankSaid = true;
                TwkLogDev("[pop] injection mode %d held 600 ms with NO crank -- this write point does "
                       "not reach the crank logic; try the other mode", mode);
            }
        } else if (g_injActive) {
            g_injActive = false;
            // Same discipline on release: silence tick-rate chatter, keep the human-scale event.
            static double lastRel = 0.0;
            const double now = NowS();
            if (now - lastRel > 0.5) {
                lastRel = now;
                TwkLogDev("[pop] injection released (%s)", g_injCranked ? "the game had cranked"
                                                                     : "no crank was seen");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_inject = 0;
        TwkLog("[pop] injection faulted -- off for this session");
    }
}
void PopProbe_TickEarly(void* ih) { InjectAt(ih, 1); }
void PopProbe_TickLate(void* ih)  { InjectAt(ih, 2); }

// See the header note. Only meaningful while the pad scheme is live -- otherwise the fields are
// physical anyway and the caller's own read is the right one.
bool PopProbe_PhysSticks(float* lx, float* ly, float* rx, float* ry) {
    if (!SchemeOn() || !g_physStamp) return false;
    static long long freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (q.QuadPart - g_physStamp > freq / 10) return false;    // pad gone quiet (100 ms)
    if (lx) *lx = (float)g_physLx / 32767.0f;
    if (ly) *ly = (float)g_physLy / 32767.0f;
    if (rx) *rx = (float)g_physRx / 32767.0f;
    if (ry) *ry = (float)g_physRy / 32767.0f;
    return true;
}

// ------------------------------------------------------------------ mode 3: the pad itself
// Modes 1 and 2 settled that the raw stick fields are a downstream MIRROR: a synthetic RS-down held
// rock-steady in them for 600+ ms never cranked the game. The gesture logic reads its sticks from
// the real input path upstream, so the injection moves to the top of that path: the XInput API the
// game polls the controller through. Rewriting the pad state it returns makes the synthetic stick
// indistinguishable from hardware to EVERY consumer.
//
// The first cut hooked one function in one DLL and never fired: no INJECTING line in a whole run
// while the pass-through sat idle. So this hooks EVERY plausible poll target -- XInputGetState AND
// XInputGetStateEx (ordinal 100, the variant engines favour) in every XInput DLL present -- accepts
// any user index, and COUNTS calls per target. The pump prints the counts, so the next log states
// which function the game actually polls instead of leaving it to a fourth guess.
//
// The hook may run on whatever thread the engine polls input from, so it does no logging and no
// game reads: the gate comes from the REAL pad state it just fetched (with hysteresis) plus two
// volatile flags the game thread maintains. Transitions and counters are logged from the pump.
typedef struct { unsigned short wButtons; unsigned char bLeftTrigger, bRightTrigger;
                 short sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PadGamepad;
typedef struct { unsigned long dwPacketNumber; PadGamepad Gamepad; } PadState;
typedef unsigned long (WINAPI* XInputGetStateFn)(unsigned long, PadState*);
enum { kPadTargets = 6 };                         // 3 DLLs x (GetState, GetStateEx)
static XInputGetStateFn g_padOrig[kPadTargets] = {};
static char             g_padName[kPadTargets][32] = {};
static volatile long    g_padCalls[kPadTargets] = {};
static volatile long    g_padUserSeen = -1;       // last connected user index a poll returned
// The pop trigger is its own, deliberately deeper than a stray touch but early in a real press.
static const short kPadPopTrigger = (short)(-0.50f * 32767);
// A FLICK moves this much per poll (~120 Hz; a full-range flick sweeps 65k units in ~10 polls).
// Used twice: upward = unmask the trick flick the moment it is IN FLIGHT even while still deep
// (position-only unmasking truncated the gesture and made tricks inconsistent -- field report);
// any direction = the stick is not SITTING, so the crouch dwell does not accumulate (which is what
// keeps a lower gate from swallowing scoop gestures that arc through the down zone).
static const short kFlickPerPoll = 2500;
// The crouch is a HOLD; everything else that crosses the down zone is a TRANSIT. Without the dwell
// (g_dwellMs, declared with the config), a nollie 360 scoop sweeping through down engaged the
// rewrite mid-gesture and the game got a mangled shove-it (field report). Same discriminator
// foot_steer uses for its catch veto. And the flick that ENDS a crouch must never reach the game:
// it is still cranked from the synthetic RS at that instant, and an exposed upward flick is exactly
// the vanilla pop gesture (field report: crouch + LS-up-flick popped an ollie). LS stays masked for
// g_maskMs after disengage while RS goes physical at once -- the crank releases without a pop, like
// abandoning a vanilla crank.

// ---- MODE SPACE (round 60: the scheme on every stance/pop family). The gesture machine below is
// the field-proven regular-ollie engine, UNCHANGED -- it now runs on abstract axes where the
// crouch/trick stick is always "LS" with the crouch DOWN and the crank/pop stick always "RS" with
// the crank DOWN. One rule generates the whole matrix (and matches the user's spec exactly): the
// analog crouch lives on the MIRROR of the stick vanilla cranks with, same direction, and the
// synthetic crank feeds the vanilla stick:
//     regular ollie  : crouch LS down -> crank RS down   (identity transform; unchanged behavior)
//     regular nollie : crouch RS up   -> crank LS up
//     switch  ollie  : crouch RS down -> crank LS down
//     switch  nollie : crouch LS up   -> crank RS up
// Fakie = regular inputs rolling backward (no mapping); goofy only mirrors left/right, which
// passes through as raw angles. cIsRS picks the physical crouch stick, yFlip = -1 for the nollie
// family (mode "down" = physical up). yFlip*yFlip = 1 and the stick swap is symmetric, so a
// pass-through emit reproduces the physical pad exactly.
static short PadClampS(int v) { return (short)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v)); }
static void PadEmit(PadState* st, bool cIsRS, int yFlip, int tLx, int tLy, int kRx, int kRy) {
    if (cIsRS) {
        st->Gamepad.sThumbRX = PadClampS(tLx); st->Gamepad.sThumbRY = PadClampS(yFlip * tLy);
        st->Gamepad.sThumbLX = PadClampS(kRx); st->Gamepad.sThumbLY = PadClampS(yFlip * kRy);
    } else {
        st->Gamepad.sThumbLX = PadClampS(tLx); st->Gamepad.sThumbLY = PadClampS(yFlip * tLy);
        st->Gamepad.sThumbRX = PadClampS(kRx); st->Gamepad.sThumbRY = PadClampS(yFlip * kRy);
    }
}

// The gesture machine, extracted from the XInput hook so BOTH input paths share it verbatim:
// mode 3 feeds it real pad polls, mode 4 (the game-side read path, see below) feeds it the
// values the game itself just read. It mutates st->Gamepad sticks in place.
static void PadMachine(PadState* st) {
    // Context gate + freshness: a stale stamp means the game thread is not ticking (pause menu,
    // loading), and the rewrite must stand down no matter what the sticks say.
    static long long freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const long long now = q.QuadPart;
    const bool fresh = (now - g_padStamp) < freq / 6;          // ~150 ms
    // All of this state lives in the hook alone (one polling thread); the pump only reads
    // g_padActive.
    static long long downSince = 0, releasedAt = 0;
    // ---- the pop machine (hook-local, one polling thread). CROUCH is the proven injection state;
    // ARMED is the trick window: opened by a physical RS flick down while crouched, during which the
    // LEFT stick is handed back to the game raw so the user's own trick flick reaches a still-
    // cranked board -- pop and trick in the one gesture the game already understands. The window
    // closes when the game pops (the crank flag drops) or times out into a synthesized plain ollie.
    static long long armedAt = 0, synthAt = 0;
    static long long graceUntil = 0;               // the simultaneity grace deadline (see below)
    static bool lsFreed = false;                   // has LS LEFT the crouch since the pop armed?
    static bool lsMoved = false;                   // has LS MOVED since the pop armed? (the LX gate)
    static long long downStillAt = 0;              // when LS began SITTING below center (manual band)
    static int  fastPolls = 0;                     // consecutive fast polls; sustained = a gesture
    static bool bandMasked = false;                // the down-band mask, hysteretic (see the branch)
    // The synthetic crank VECTOR -- the pocket. Vanilla places the back foot by the crank stick's
    // ANGLE (measured: center 180deg, pockets out to ~127/258), so the crouch stick's own angle
    // steers it here: angle the LS into a corner and the foot sits in that pocket. Magnitude is
    // always forced full (a part-deflected crank makes the game manual, measured rule), and the
    // vector freezes at the pop press along with the depth. Straight down until a crouch says
    // otherwise.
    static short synRx = 0, synRy = -32767;
    // Raw physical pad, published for the trackers BEFORE any rewrite or mode transform.
    const short phLx = st->Gamepad.sThumbLX, phLy = st->Gamepad.sThumbLY;
    const short phRx = st->Gamepad.sThumbRX, phRy = st->Gamepad.sThumbRY;
    g_physLx = phLx; g_physLy = phLy; g_physRx = phRx; g_physRy = phRy;
    g_physStamp = now;
    // Per-PHYSICAL-stick deltas, continuous across mode flips.
    static short pvLy = 0, pvLx = 0, pvRy = 0, pvRx = 0;
    const int dPhLy = (int)phLy - (int)pvLy, dPhLx = (int)phLx - (int)pvLx;
    const int dPhRy = (int)phRy - (int)pvRy, dPhRx = (int)phRx - (int)pvRx;
    pvLy = phLy; pvLx = phLx; pvRy = phRy; pvRx = phRx;
    // A FOREIGN CRANK: the game is cranked by a gesture we did not synthesize = the player is
    // running a stock trick (vanilla RS-down crank into a 360-shove scoop, a vanilla tre...).
    // The machine keeps its hands off EVERYTHING while it runs and for a short tail after: the
    // scoop's follow-through sweeps the crank stick UP, which is otherwise indistinguishable
    // from a nollie crouch approach -- the band mask was hiding the stick 30 ms into its up-hold
    // and the nollie dwell could even engage, either way the game saw the stick "let go" and
    // fired a quick shove with whatever rotation it had (field report, round 64).
    static long long foreignUntil = 0;
    if (g_padCrank && !g_padActive && !g_padArmed)
        foreignUntil = now + freq * 3 / 10;        // refreshed while it lasts; ~300 ms tail
    // The scheme-hot stamp for the QUICK-SHOVE GATE (see hkSetTrick): while the crouch is
    // engaged or a pop is armed, quick-shove trick defs are refused game-side for this window --
    // input shaping was tried three rounds running and never touched the symptom.
    if (g_padActive || g_padArmed) g_padHotUntil = now + freq / 2;       // 500 ms past release
    // A live GRIND (or liptrick) is hands-off in exactly the same way -- the sticks belong to
    // grind balance and the vanilla exit crank. This is the round-66 fix: landing a 180 into a
    // grind with the crouch stick still held from the rotation had the dwell (accumulated
    // through the air) ENGAGE 10 ms after touchdown, and a synthetic full crank arriving on a
    // grind is an instant exit pop -- the log showed INJECTING at rail entry followed 36 ms
    // later by a grindRatio=1.0 jump.
    const bool foreign = (now < foreignUntil) || g_padGrind != 0;
    // Mode resolution (see the block comment above PadCommon). The mode may only change while the
    // machine is fully idle -- mid-crouch, mid-window, mid-grace, and through the release mask the
    // established mapping stands. Selection is by commitment: the candidate whose crouch stick is
    // meaningfully past neutral AND deeper than the incumbent's takes over.
    static int  mode = 0;                          // 0 = ollie family, 1 = nollie family
    static bool swL  = false;                      // stance, LATCHED with the mode -- the game's
                                                   // switch flag flipping mid-gesture (a turn)
                                                   // must not swap the sticks under the machine
    {
        const bool idle = !g_padActive && !g_padArmed && !graceUntil && !foreign &&
                          !(releasedAt && (now - releasedAt) < freq * g_maskMs / 1000);
        if (!g_stances) { mode = 0; swL = false; }
        else if (idle) {
            swL = g_padSw != 0;
            const int aLy = swL ? (int)phRy : (int)phLy;   // ollie-family crouch axis (down = neg)
            const int bLy = -(swL ? (int)phLy : (int)phRy);// nollie-family, in mode convention
            const int aLx = swL ? (int)phRx : (int)phLx;
            const int bLx = swL ? (int)phLx : (int)phRx;
            // Candidates must sit inside the crouch cone (see inCone below): a mostly-sideways
            // stick is a boardslide/lean hold, not a crouch approach, and must not steal the mode.
            const bool aCone = (-aLy) * 100 >= abs(aLx) * 84;
            const bool bCone = (-bLy) * 100 >= abs(bLx) * 84;
            if (mode == 0 && bCone && bLy < -1500 && bLy < aLy - 1000) mode = 1;
            else if (mode == 1 && aCone && aLy < -1500 && aLy < bLy - 1000) mode = 0;
        }
    }
    g_padModeDbg = (mode ? 1 : 0) | (swL ? 2 : 0);
    const bool cIsRS = (mode == 0) ? swL : !swL;   // which physical stick crouches (and tricks)
    const int  yFlip = (mode == 0) ? 1 : -1;       // -1: mode "down" is physical up
    // The machine's working axes, in mode space. Names kept from the regular-ollie original so
    // the proven logic below reads unchanged: ly/lx = the crouch/trick stick, ryPhys/rxPhys = the
    // crank/pop stick, negative Y = toward the crouch/crank.
    const short ly     = PadClampS(yFlip * (cIsRS ? (int)phRy : (int)phLy));
    const short lx     = cIsRS ? phRx : phLx;
    const short ryPhys = PadClampS(yFlip * (cIsRS ? (int)phLy : (int)phRy));
    const short rxPhys = cIsRS ? phLx : phRx;
    const int dly    = yFlip * (cIsRS ? dPhRy : dPhLy);
    const int moving = (dly > 0 ? dly : -dly) + abs(cIsRS ? dPhRx : dPhLx);
    const int dry    = yFlip * (cIsRS ? dPhLy : dPhRy);
    const int rsMove = (dry > 0 ? dry : -dry) + abs(cIsRS ? dPhLx : dPhRx);
    // The crouch CONE: a stick only counts as "toward the crouch" inside the pocket's own 50-deg
    // cone (|lx| <= 1.19 * downness -- the same kMaxSide the crank vector is clamped to). A stick
    // parked mostly SIDEWAYS is a different gesture entirely: the boardslide hold pushes both
    // sticks horizontally, and a few degrees of thumb slope was enough for the band mask (or
    // worse, the crouch dwell) to claim it -- the game lost a stick 30 ms into the slide and
    // dumped the player off the rail (field report, round 65). Everything that watches for a
    // crouch approach -- dwell, band mask, neutralizer trigger, mode selection -- requires it.
    const bool inCone = (-(int)ly) * 100 >= abs((int)lx) * 84;

    if (g_padArmed) {
        // THE SCOOP IS THE BACK FOOT. A shove/tre pop is not a different trick input, it is the pop
        // press itself SWEPT instead of straight -- so during the window the user's physical RS
        // passes through whenever the hand is actually doing something, floored to crank strength
        // so a shallow press cannot break the crank. Straight press = clean pop; swept press = the
        // scoop, which vanilla's cranked-gesture recognition already understands. When the hand
        // comes off (back to center), the frozen crank vector holds so the trick flick still pops.
        const float rMag = sqrtf((float)rxPhys * rxPhys + (float)ryPhys * ryPhys) / 32767.0f;
        const bool  rsBusy = rMag > 0.35f;
        short outRx = synRx, outRy = synRy;
        if (rsBusy) {
            const float scale = (rMag < 0.90f) ? (0.90f / rMag) : 1.0f;
            outRx = (short)((float)rxPhys * scale);
            outRy = (short)((float)ryPhys * scale);
        }
        const bool crankDropped = !g_padCrank;     // the game consumed a gesture and popped
        const bool timedOut = (now - armedAt) > freq * g_windowMs / 1000;
        // A scoop in progress IS the pop gesture arriving -- synthesizing the ollie flick over it
        // would hybridize the trick, so the synth waits for the RS to come to rest. The hard cap
        // keeps a held press from leaving the machine armed forever.
        const bool capped = (now - armedAt) > freq / 2;         // 500 ms, absolute
        if (crankDropped || capped || (timedOut && synthAt && (now - synthAt) > freq / 12)) {
            g_padArmed = 0; synthAt = 0;           // window over; the air gate takes it from here
            releasedAt = 0; downSince = 0;
        } else if (timedOut && !rsBusy) {
            // No trick gesture arrived: synthesize the plain pop -- the trick stick full
            // ANTI-crouch (up for ollies, down for nollies via the emit) for ~80 ms while the
            // crank vector holds (a cornered crouch pops from its pocket).
            if (!synthAt) synthAt = now;
            PadEmit(st, cIsRS, yFlip, 0, 32767, synRx, synRy);
        } else {
            // Window open, crank held. Two masking generations both failed here, in opposite ways:
            // raw-from-arm exposed the still-held crouch (tiny-pop, every jump ShittyOllie), and
            // velocity-unmask exposed the stick the instant it MOVED -- still ~80% down, because a
            // sprung stick moves fast long before it leaves the down half (field trace: LS 0.81 at
            // 180 degrees inside the pop's final 25 ms; tiny-pop again). Thresholds cannot fix a
            // structural problem: any unmask below center hands a cranked game a down-position
            // sample. So no masking at all -- the DOWN COMPONENT is clamped instead. Sideways and
            // upward pass raw from the first armed poll (zero added latency on the trick flick);
            // downward reads as center until the stick has come up to center once, after which it
            // is fully raw.
            if (!lsFreed && ly > -1000) lsFreed = true;         // reached center: hand it all over
            // LX has its own gate, because the pocket put a large SIDEWAYS component into the stale
            // crouch position: passing it raw from the arm poll read as "cranked + LS held hard
            // right" = an instant heelflip on every pocketed pop (field log: HeelFlip/VarialHeel at
            // LS 117-158 degrees, fired at the arm). Hidden until the stick actually MOVES -- a
            // flick crosses the motion threshold on its first poll, a held or drifting pocket never
            // does, so trick latency stays zero and held positions stay invisible.
            if (!lsMoved && moving > kFlickPerPoll) lsMoved = true;
            PadEmit(st, cIsRS, yFlip, lsMoved ? lx : 0, (!lsFreed && ly < 0) ? 0 : ly, outRx, outRy);
        }
        return;
    }

    // Crank-stick velocity, for the earlier pop trigger: a real press crosses the whole zone in a
    // couple of polls, so crank-ward speed at shallow deflection is the press, ~15 ms before the
    // position threshold would say so. And the consecutive-fast counter: the crank stick's own
    // motion-vs-held discriminator (same shape as the band mask's) -- a real scoop sustains
    // gesture speed; a hovering thumb and its noise spikes never make three in a row.
    static int rsFast = 0;
    if (rsMove > kFlickPerPoll) { if (rsFast < 100) rsFast++; } else rsFast = 0;
    const bool rsPressed = (ryPhys < kPadPopTrigger) ||
                           (ryPhys < -8192 && dry < -kFlickPerPoll);

    bool engage;
    if (!g_padActive) {
        // SITTING in the zone, not merely inside it: a gesture arcing through the down region moves
        // fast, and motion restarts the dwell -- which is what lets the gate sit at 40% without
        // swallowing scoops that sweep deeper than that.
        if (ly < g_lsEngage && inCone) { if (!downSince || moving > kFlickPerPoll) downSince = now; }
        else downSince = 0;
        engage = downSince && (now - downSince) > freq * g_dwellMs / 1000 && g_padAllow && fresh &&
                 !foreign;                         // never take over a stock gesture in progress
    } else {
        engage = (ly < g_lsRelease) && g_padAllow && fresh;
        // THE SIMULTANEITY GRACE. Real pop muscle memory flicks both sticks together, and when the
        // LS flick led the RS press by even one poll the crouch disengaged and the release mask ate
        // the whole gesture -- input dead, which read as sluggishness (the player learns to sequence
        // deliberately). A crouch that ends in an upward FLICK (velocity, not drift) therefore
        // lingers: crank and mask held exactly as if still crouched, waiting a beat for the RS
        // press. Press arrives = the pop, with the flick already in flight; no press = a normal
        // release once the grace expires.
        if (!engage && g_padAllow && fresh && dly > kFlickPerPoll && !graceUntil)
            graceUntil = now + freq * g_graceMs / 1000;
        if (graceUntil) {
            if (now < graceUntil && g_padAllow && fresh) engage = true;
            else graceUntil = 0;
        }
    }
    if (engage) {
        // Live crouch depth and the POCKET, both from the crouch stick, both frozen during the
        // grace (the stick is already leaving; the last real crouch stands). Depth is the stick's
        // MAGNITUDE past the gate, not its downness -- a full-deflection corner hold means a full
        // pop, not a shallower one. The pocket is the stick's ANGLE, clamped to at most ~70 degrees
        // off straight down (past that a "crank" stops being one) and forced to full magnitude.
        if (!graceUntil) {
            const float mag = sqrtf((float)lx * lx + (float)ly * ly);
            const float gate = (float)(-g_lsEngage);
            float depth = (mag - gate) / (32767.0f - gate);
            if (depth < 0.0f) depth = 0.0f; else if (depth > 1.0f) depth = 1.0f;
            g_padDepthBits = FToBits(depth);
            if (ly < g_lsRelease && mag > 1.0f) {
                float dx = (float)lx, dy = (float)ly;
                // tan(50 deg). The first cut allowed 70 degrees off down, which leaves the crank
                // vector's DOWN component as small as 0.34 -- and to the game's own crank/manual
                // discriminator that is a "slight pull", i.e. a MANUAL (field report: manuals on
                // every pocketed crouch). At 50 degrees the down component never drops below ~0.64,
                // safely a crank, and the measured vanilla pockets sit at ~53 degrees anyway.
                const float kMaxSide = 1.19f;
                const float side = (dx < 0 ? -dx : dx);
                if (side > kMaxSide * -dy) dx = (dx < 0 ? -1.0f : 1.0f) * kMaxSide * -dy;
                const float m = sqrtf(dx * dx + dy * dy);
                if (m > 1.0f) {
                    synRx = (short)(dx / m * 32767.0f);
                    synRy = (short)(dy / m * 32767.0f);
                }
            }
        }
        // The pop trigger: a physical crank-stick press while crouched. Depth and pocket freeze
        // HERE -- the crouch stick is about to leave for the trick, and that must not change the
        // pop. The press test alone was not enough (round 62): a 360-shove scoop can START
        // laterally, and position/velocity-down only fires once the sweep reaches the down zone
        // -- everything before that was overwritten by the synthetic crank, eating the arc's
        // first segment (field: "the crouch interferes with 360 shoves a bit"). While crouched
        // the crank stick has exactly one legitimate job, so any sustained gesture-speed sweep
        // at window strength arms the pop too, and the window's pass-through then carries the
        // whole arc from its first fast poll.
        const float kMag = sqrtf((float)rxPhys * rxPhys + (float)ryPhys * ryPhys) / 32767.0f;
        const bool rsSwept = rsFast >= 2 && kMag > 0.35f;
        if ((rsPressed || rsSwept) && g_padCrank) {
            g_padArmedDepthBits = g_padDepthBits;
            g_padArmed = 1; armedAt = now; synthAt = 0;
            lsFreed = (graceUntil != 0);           // grace = the trick flick is ALREADY in flight
            lsMoved = lsFreed;                     // ...and then LX is already earned too
            graceUntil = 0;
            g_padActive = 0; downSince = 0; releasedAt = 0;
            // Same gates as the window: a held pocket is invisible until the stick moves/frees.
            PadEmit(st, cIsRS, yFlip, lsMoved ? lx : 0, (ly < 0 && !lsFreed) ? 0 : ly, synRx, synRy);
            return;
        }
        // The game must not see the crouch stick; the crank goes out aimed at the pocket.
        PadEmit(st, cIsRS, yFlip, 0, 0, synRx, synRy);
        g_padActive = 1;
    } else {
        if (g_padActive) { releasedAt = now; downSince = 0;     // disengage edge: start the mask
                           synRx = 0; synRy = -32767; }         // and the pocket resets to center
        g_padActive = 0;
        // downSince is NOT cleared here: this branch runs on every poll while the dwell is still
        // counting, and clearing it made the timer restart per poll -- 110 ms could never
        // accumulate and the crouch simply stopped engaging (field report). The !active branch
        // above owns the reset, on the stick actually leaving the down zone.
        //
        // THE MANUAL BAND: the road from neutral to the crouch gate runs straight through the
        // game's manual input (a slight-down stick), and a slow or shallow pull lingered there long
        // enough to trigger one (field report: "sometimes pulling left stick down manuals"). Same
        // law as every other leak fixed today -- motion passes, held positions do not: a stick
        // SITTING below center has its down component masked after a short beat, while a stick
        // MOVING through the band stays raw so scoop transits keep their shape. Gated on the
        // scheme's context, so walking off the board is untouched.
        // Slow-moving counts as sitting: a very slow pull toward the crouch is indistinguishable
        // from a deliberate vanilla manual input. Two earlier cuts of this mask leaked anyway, and
        // the flaw was the reset: a SINGLE poll of motion above the flick threshold re-exposed the
        // stick raw, and a slow pull is full of single fast polls -- thumb tremor and pocket
        // micro-corrections -- each one showing the game 20-40% down for the next beat, the exact
        // manual band (field: manuals persisted through both cuts, worst on pocket pulls). The mask
        // is now hysteretic: once on, it holds until the stick leaves the down half or shows
        // SUSTAINED fast motion -- three consecutive fast polls is a gesture, noise never is.
        if (moving > kFlickPerPoll) { if (fastPolls < 100) fastPolls++; } else fastPolls = 0;
        if (ly >= -1500 || !inCone) { downStillAt = 0; bandMasked = false; }   // sideways = not ours
        else if (fastPolls >= 3)    { downStillAt = 0; bandMasked = false; }   // a real gesture
        else if (!downStillAt)      downStillAt = now;
        if (downStillAt && (now - downStillAt) > freq * 3 / 100) bandMasked = true;
        // Mode-space outputs, pass-through until a mask says otherwise; the emit at the end maps
        // them back onto the physical pad (identity when nothing masked).
        int tLx = lx, tLy = ly, kRx = rxPhys, kRy = ryPhys;
        if (g_inManual || foreign) {
            // A live manual balances on the raw sticks, and a foreign crank IS a stock gesture in
            // flight -- every idle-branch mask stands down for both (the .49 cut left them running
            // against manual balance; the .63 cut let the band mask hide a scoop's up-hold).
        } else {
        if (releasedAt && (now - releasedAt) < freq * g_maskMs / 1000) {
            tLx = 0; tLy = 0;                                    // hide the exit flick; crank stick real
        } else if (bandMasked && g_padAllow && fresh) {
            tLx = 0; tLy = 0;                                    // a held slight-crouch is invisible --
        }                                                        //    a sitting pocket approach too
        // Whenever the crouch stick is in its crouch half, the game receives a NEUTRAL crank
        // stick -- but only a SITTING, slight one: a MOVING crank stick passes (round 56: nollie
        // tres were being chopped mid-scoop) and so does a FULL-deflection one even when held
        // (round 62: vanilla tres cranked stationary while the LS scooped). Only manual-depth,
        // not-moving = safe to hide. (Rounds 67-70 tried stricter shapes here plus release ramps
        // against the crouch-release quick shove and NONE of it touched the symptom -- all
        // reverted at the user's word; that fight moved game-side to the SetTrick quick-shove
        // gate, where it belongs.)
        const float kStickMag = sqrtf((float)rxPhys * rxPhys + (float)ryPhys * ryPhys);
        if (ly < -1500 && inCone && rsFast < 3 && kStickMag < 22000.0f && g_padAllow && fresh) {
            kRx = 0; kRy = 0;
        }
        // Evidence, not theory: record what the game was actually SHOWN whenever a manual-capable
        // sample survives the masks (see the ring's comment). MODE-SPACE values: 'L' = the crouch
        // stick's crouch half visible (expected only inside the 30 ms sit delay or a sustained
        // gesture -- the fast field tells them apart); 'R' = the crank stick in the slight band
        // while the crouch stick sits above the neutralizer's gate (a resting thumb).
        if (g_padAllow && fresh) {
            char kind = 0;
            if (tLy < -1500 && inCone) kind = 'L';   // out-of-cone sideways holds are expected raw
            else if (kRy < -3000 && kRy > -22000 && ly >= -1500) kind = 'R';
            if (kind) {
                const LONG i = InterlockedIncrement(&g_leakWr) - 1;
                PadLeakRec* r = &g_leakRecs[((i % 32) + 32) % 32];
                r->qpc = now; r->outLy = (short)tLy; r->outLx = (short)tLx; r->outRy = (short)kRy;
                r->phLy = ly; r->mov = (short)(moving > 32767 ? 32767 : moving);
                r->fast = (char)(fastPolls > 99 ? 99 : fastPolls); r->kind = kind;
            }
        }
        }
        PadEmit(st, cIsRS, yFlip, tLx, tLy, kRx, kRy);
    }}

static unsigned long PadCommon(int idx, unsigned long user, PadState* st) {
    InterlockedIncrement(&g_padCalls[idx]);
    const unsigned long ret = g_padOrig[idx] ? g_padOrig[idx](user, st)
                                             : 1167L /*ERROR_DEVICE_NOT_CONNECTED*/;
    if (ret != 0 || !st || g_inject != 3 || !g_on) return ret;
    g_padUserSeen = (long)user;                    // whichever index answers is the live pad
    PadMachine(st);
    return ret;
}
static unsigned long WINAPI hkPad0(unsigned long u, PadState* s) { return PadCommon(0, u, s); }
static unsigned long WINAPI hkPad1(unsigned long u, PadState* s) { return PadCommon(1, u, s); }
static unsigned long WINAPI hkPad2(unsigned long u, PadState* s) { return PadCommon(2, u, s); }
static unsigned long WINAPI hkPad3(unsigned long u, PadState* s) { return PadCommon(3, u, s); }
static unsigned long WINAPI hkPad4(unsigned long u, PadState* s) { return PadCommon(4, u, s); }
static unsigned long WINAPI hkPad5(unsigned long u, PadState* s) { return PadCommon(5, u, s); }
static void* const g_padStubs[kPadTargets] = { (void*)&hkPad0, (void*)&hkPad1, (void*)&hkPad2,
                                               (void*)&hkPad3, (void*)&hkPad4, (void*)&hkPad5 };

// ------------------------------------------------------------------ mode 4: the game's own input
// The XInput hook (mode 3) only sees controllers the game reads through XInput -- on the Epic
// client a DualSense is read elsewhere and the scheme did nothing (3.19.73 verdict line). The
// first game-side attempt hooked the stick GETTERS; the field round proved that insufficient:
// consumers read positions through them, but the gesture EVENTS (flick/hold classification) are
// derived inside InputHandler::Tick before any getter runs, so the crank state machine never saw
// the synthetic crank. The true choke point is Tick's own INPUT: its 4th argument points at a
// 4-float buffer (LSx, LSy, RSx, RSy -- down negative) the controller filled from whatever
// backend read the pad; Tick copies it into its fields at +0x24..0x30 (disasm 0x106518e) and
// derives everything from there. Rewriting that buffer BEFORE the original Tick makes the field
// refresh, the input-frame array, the event derivation and the getters all process the machine's
// values as if the controller produced them -- any pad, any backend, one write. Called from
// scoop_speed's existing InputHandler::Tick hook, immediately before the original; the buffer is
// only touched inside the call (never stored), and the capture is the exact physical read the
// game itself made, which is what PhysSticks hands the flick trackers.
void PopProbe_TickSticks(float* sticks) {
    if (g_inject != 4 || !g_on || !sticks) return;
    __try {
        PadState st = {};
        st.Gamepad.sThumbLX = PadClampS((int)(sticks[0] * 32767.0f));
        st.Gamepad.sThumbLY = PadClampS((int)(sticks[1] * 32767.0f));
        st.Gamepad.sThumbRX = PadClampS((int)(sticks[2] * 32767.0f));
        st.Gamepad.sThumbRY = PadClampS((int)(sticks[3] * 32767.0f));
        PadMachine(&st);
        sticks[0] = (float)st.Gamepad.sThumbLX / 32767.0f;
        sticks[1] = (float)st.Gamepad.sThumbLY / 32767.0f;
        sticks[2] = (float)st.Gamepad.sThumbRX / 32767.0f;
        sticks[3] = (float)st.Gamepad.sThumbRY / 32767.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Printed by the pump every few seconds while mode 3 is on -- the whole point of this build.
void PopProbe_PadReport() {
    char line[256]; int used = 0;
    for (int i = 0; i < kPadTargets; i++) {
        if (!g_padOrig[i]) continue;
        const int w = snprintf(line + used, sizeof(line) - used, "%s%s=%ld",
                               used ? "  " : "", g_padName[i], g_padCalls[i]);
        if (w > 0 && used + w < (int)sizeof(line) - 1) used += w;
    }
    TwkLogDev("[pop] pad polls: %s | live user index %ld", used ? line : "(no hooks)", g_padUserSeen);
}

// The pad hooks, re-probeable. Hooking only what was loaded at STARTUP is a silent dead end when
// the input path binds late: nothing is hooked, the scheme does nothing, and no line says why.
// Called again from the pump while no polls have been seen; already-hooked procs skipped by address.
static void* g_padProc[kPadTargets] = {};
static int   g_padHooked = 0;
static int PopProbe_InstallPadHooks() {
    static const char* kDlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int added = 0;
    for (int d = 0; d < 3 && g_padHooked < kPadTargets; d++) {
        HMODULE m = GetModuleHandleA(kDlls[d]);
        if (!m) continue;
        for (int fn = 0; fn < 2 && g_padHooked < kPadTargets; fn++) {
            // Ordinal 100 = XInputGetStateEx, exported nameless; same signature plus the Guide bit.
            void* p = fn == 0 ? (void*)GetProcAddress(m, "XInputGetState")
                              : (void*)GetProcAddress(m, (const char*)(uintptr_t)100);
            if (!p) continue;
            bool already = false;
            for (int i = 0; i < g_padHooked; i++) if (g_padProc[i] == p) { already = true; break; }
            if (already) continue;
            const int n = g_padHooked;
            if (MH_CreateHook(p, g_padStubs[n], (void**)&g_padOrig[n]) == MH_OK &&
                MH_EnableHook(p) == MH_OK) {
                g_padProc[n] = p;
                snprintf(g_padName[n], sizeof(g_padName[0]), "%.9s:%s",
                         kDlls[d], fn == 0 ? "GetState" : "Ex100");
                TwkLog("[pop] pad hook %d installed: %s %s", n, kDlls[d],
                       fn == 0 ? "XInputGetState" : "XInputGetStateEx(ord 100)");
                g_padHooked++; added++;
            } else TwkLog("[pop] pad hook FAILED: %s fn %d", kDlls[d], fn);
        }
    }
    return added;
}
// True once a hooked target has actually been CALLED -- i.e. the game really reads this pad
// through XInput, which is the one precondition the scheme cannot work without.
static bool PopProbe_PadPolled() {
    for (int i = 0; i < kPadTargets; i++) if (g_padCalls[i] > 0) return true;
    return false;
}

void PopProbe_Install() {
    PopProbe_InstallPadHooks();
    if (!g_padHooked)
        TwkLog("[pop] no XInput module loaded yet -- will keep looking while the scheme is on");
    // The quick-shove gate (see hkSetTrick). Installed unconditionally; inert unless the scheme
    // is on AND hot.
    void* stk = TwkScanExe(SIG_SET_TRICK);
    if (stk && MH_CreateHook(stk, (void*)&hkSetTrick, &g_origSetTrick) == MH_OK &&
        MH_EnableHook(stk) == MH_OK) {
        TwkLog("[pop] quick-shove gate installed: SetTrick @ %p (quick-shove defs refused while "
               "the crouch is engaged or just released)", stk);
    } else {
        g_origSetTrick = nullptr;
        TwkLog("[pop] quick-shove gate NOT installed (%s) -- quick shoves stay vanilla",
               stk ? "hook failed" : "sig not found");
    }
    // The manual gate (see the gate block up top). Installed unconditionally like the pads; the
    // runtime verdict is what turns it on, and it forces vanilla whenever the scheme is off.
    void* cm = TwkScanExe(SIG_CHECK_MANUALS);
    if (cm && MH_CreateHook(cm, (void*)&hkCheckManuals, &g_origCheckManuals) == MH_OK &&
        MH_EnableHook(cm) == MH_OK) {
        TwkLog("[pop] manual gate installed: Skate_CheckForManuals @ %p (manual entry now requires "
               "a deliberate RS hold while the scheme is on)", cm);
    } else {
        g_origCheckManuals = nullptr;
        TwkLog("[pop] manual gate NOT installed (%s) -- manuals stay vanilla",
               cm ? "hook failed" : "sig not found");
    }
}

// ------------------------------------------------------------------ F1 status line
void PopProbe_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    api->TextDisabled((g_inject == 3 || g_inject == 4)
        ? "Pop control scheme: ON (settings on the Session Tweaks pause-menu page)"
        : g_on ? "Pop probe: measuring (see [pop] lines in SessionTweaks.log)"
               : "Pop probe: off");
}

// ------------------------------------------------------------------ pause-menu accessors
// GAME THREAD (menu_ext contract): plain int reads/writes; every setter marks the ini dirty so a
// menu change persists exactly like an ini edit.
float PopProbe_CrouchDepth01() {
    if (!SchemeOn() || !g_padActive) return 0.0f;
    const float d = BitsToF(g_padDepthBits);
    return (d > 0.0f && d <= 1.0f) ? d : 0.0f;
}
bool PopProbe_SchemeEnabled()           { return g_inject == 3 || g_inject == 4; }
void PopProbe_SetSchemeEnabled(bool on) { g_inject = on ? 4 : 0; TwkMarkDirty(); }
// (No stances accessor: all stances is simply how the scheme works. PopProbeStances stays as an
// ini-only dev fallback, never surfaced in the menu.)
float PopProbe_TrickWindowMs() { return (float)g_windowMs; }
void  PopProbe_SetTrickWindowMs(float v) {
    g_windowMs = (int)(v + 0.5f);
    if (g_windowMs < 60) g_windowMs = 60; else if (g_windowMs > 600) g_windowMs = 600;
    TwkMarkDirty();
}
float PopProbe_CrouchGatePct() { return (float)(-(int)g_lsEngage * 100 / 32767); }
void  PopProbe_SetCrouchGatePct(float v) {
    int pct = (int)(v + 0.5f);
    if (pct < 20) pct = 20; else if (pct > 80) pct = 80;
    g_lsEngage  = (short)(-(pct * 32767) / 100);
    g_lsRelease = (short)(-((pct - 15) * 32767) / 100);
    TwkMarkDirty();
}
// OpenMP's window into the live scrub, resolved by GetProcAddress across the module boundary (both
// mods ship as main.dll in different folders, so an export is the only sane linkage). Returns 1 and
// writes the clock while a scrub published within the last 100 ms; 0 otherwise. 100 ms is generous
// against a hitchy frame and still an order of magnitude inside a human crouch.
extern "C" __declspec(dllexport) int Twk_CrankVisClock(float* out) {
    const long long tick = g_visPubTick;
    if (!tick || (long long)GetTickCount64() - tick > 100) return 0;
    if (out) *out = BitsToF(g_visPubBits);
    return 1;
}

float PopProbe_CrankVisTimeMs() { return (float)g_crankVisTime; }
void  PopProbe_SetCrankVisTimeMs(float v) {
    g_crankVisTime = (int)(v + 0.5f);
    if (g_crankVisTime < 50) g_crankVisTime = 50; else if (g_crankVisTime > 2000) g_crankVisTime = 2000;
    if (g_crankVisMin > g_crankVisTime - 50) g_crankVisMin = g_crankVisTime - 50;
    TwkMarkDirty();
}
float PopProbe_CrankVisMinMs() { return (float)g_crankVisMin; }
void  PopProbe_SetCrankVisMinMs(float v) {
    g_crankVisMin = (int)(v + 0.5f);
    if (g_crankVisMin < 0) g_crankVisMin = 0;
    if (g_crankVisMin > g_crankVisTime - 50) g_crankVisMin = g_crankVisTime - 50;
    TwkMarkDirty();
}
float PopProbe_CrankVisSmoothMs() { return (float)g_crankVisSmoothMs; }
void  PopProbe_SetCrankVisSmoothMs(float v) {
    g_crankVisSmoothMs = (int)(v + 0.5f);
    if (g_crankVisSmoothMs < 0) g_crankVisSmoothMs = 0; else if (g_crankVisSmoothMs > 1000) g_crankVisSmoothMs = 1000;
    TwkMarkDirty();
}
