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
// SessionTweaks -- BOARD PITCH RANGE. More fine control over how boned/rocket a flip comes out.
//
// THE DEFECT (measured previously; see the project notes on flip pitch). Board pitch is:
//     pitch = DirectionalCurve(ratio) * (+-30 envelope) * BoardControlExtraPitchMultiplier
//     ratio = clamp( |sin(flick elevation)| / (1 - cos(AxisXMaxAngleThreshold)), 0, 1 )
// With the shipped `AxisXMaxAngleThreshold = 65`, the denominator is 0.577, so ratio reaches 1.0 at
// only ~35 deg of flick elevation. Everything from 35 deg to straight up produces IDENTICAL pitch --
// well over half the stick's usable throw is dead, and all the modulation is crammed into the
// bottom third. Reshaping the directional curve (the earlier asset-side mod) redistributes response
// WITHIN that 0-35 deg band but cannot move the ceiling; if anything it pushes the mid-range closer
// to it, which is why the curve fix helped the low end and still felt short of control.
//
// WHY THE THRESHOLD CANNOT SIMPLY BE EDITED. `AxisXMaxAngleThreshold` does double duty: it is also
// `InputHandler::CheckForHorizontalInput`'s recognition cone -- how far off horizontal a flick may
// be and still count as a flip trick at all. Raising it in the asset would widen pitch range AND
// start stealing vertical flicks (ollie/nollie live inside `AxisYMaxAngleThreshold` = 25).
//
// HOW THIS SEPARATES THEM. `GetBoardExtraPitchAngle` fetches the settings block fresh, mid-call, via
// `InputHandler::GetInputSettings` -- verified, it is one of the calls inside it. So we hook the
// GETTER and hand back a patched COPY only when the immediate caller is the pitch function. Two
// properties make this exact rather than hopeful:
//   * The recogniser is NOT called from inside the pitch function (its full call list was scanned:
//     GetInputSettings, GetConvertedRelativeDirection, ConvertToCurrentInputModeInput,
//     IsLeftStickInput, IsRightStickInput, IsSkatingGoofy/Switch, UCurveFloat::GetFloatValue).
//     Input recognition therefore cannot see this.
//   * The check is on the IMMEDIATE return address, so nested users of GetInputSettings are
//     untouched automatically -- IsLeftStickInput calling it returns into ITSELF, not into range.
// Nothing of the game's is written: we copy the 60-byte block, patch one float in the copy, and
// return that. The asset itself is never modified, so there is no state to restore and nothing that
// can be left dirty by a crash.
//
// THE DEFAULT IS 80, AND THE CEILING IS NOT 90. Saturation elevation = asin(1 - cos(T)):
//     T=65 (stock) -> 35.3 deg      T=80 -> 55.7 deg      T=85 -> 65.9 deg      T=90 -> 90.0 deg
// A flick may be at most ~65 deg off horizontal and still be recognised as a flip trick, so 85 is
// the theoretical maximum useful value -- saturation exactly at the recognition limit. 80 is the
// shipped default because it FELT better in play: it leaves a little headroom below that limit, so
// full pitch does not demand a flick right at the edge of what still reads as a flip.
// ⚠️ T=90 would look like "even more range" and is a trap -- max pitch would then require a
// STRAIGHT-UP flick, which is not a flip-trick input at all, making peak pitch unreachable.
//
// Peak pitch is unchanged either way: ratio still reaches 1.0, it just takes a bigger flick to get
// there. What changes is how much stick travel sits between flat and full.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "pitch_range.h"
#include "catch_level.h"     // for the already-resolved GetBoardExtraPitchAngle address, see Install
#include <cmath>
#include <intrin.h>          // _ReturnAddress -- the caller check is the whole scoping mechanism
#include "MinHook.h"

// ------------------------------------------------------------------ measured offsets (PDB-confirmed)
enum {
    // FInputModeInputSettings, 60 bytes, three consecutive blocks on the inputs data asset
    // (Legacy +0xa8, Normal +0x6c, Default +0x30 -- GetInputSettings picks one).
    IMS_SIZE            = 60,
    IMS_AXIS_X_MAX_ANG  = 0x04,   // AxisXMaxAngleThreshold (ships 65 in the Normal block)
    // The return-address window that identifies "this fetch is for board pitch".
    EXTRA_PITCH_SIZE    = 0x4a0,  // FlipTricksHandler::GetBoardExtraPitchAngle, Epic 0x10234c0
};

// ------------------------------------------------------------------ knobs (ALL above the reader)
static int   g_on      = 1;      // PitchRange
static float g_maxAng  = 80.0f;  // PitchRangeMaxAngle -- user-picked; see the header on why not 90
static volatile LONG g_faults = 0;
static volatile LONG g_uiHits = 0;
static volatile float g_uiStock = 0;

void PitchRange_ReadConfig(const char* buf) {
    g_on     = TwkIniInt(buf, "PitchRange", 1);
    g_maxAng = (float)TwkIniInt(buf, "PitchRangeMaxAngle", 80);
    // Below the stock 65 this would NARROW the range (saturate even sooner), which is the opposite
    // of the point; above 89 the saturation elevation runs past what still reads as a flip trick.
    if (g_maxAng < 65.0f) g_maxAng = 65.0f;
    if (g_maxAng > 89.0f) g_maxAng = 89.0f;
    const float sat = asinf(fminf(1.0f, 1.0f - cosf(g_maxAng * 3.14159265f / 180.0f))) * 57.2957795f;
    TwkLog("[pitch] config: PitchRange=%d PitchRangeMaxAngle=%.0f (pitch spreads over 0-%.0f deg of "
           "flick instead of the stock 0-35; peak pitch unchanged)", g_on, g_maxAng, sat);
}
void PitchRange_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "PitchRange",         g_on);
    TwkIniSetInt(buf, cap, "PitchRangeMaxAngle", (int)(g_maxAng + 0.5f));
}
void PitchRange_ResetDefaults() { g_on = 1; g_maxAng = 80.0f; }
bool  PitchRange_Enabled()             { return g_on != 0; }
void  PitchRange_SetEnabled(bool on)   { g_on = on ? 1 : 0; TwkMarkDirty(); }
float PitchRange_MaxAngleDeg()         { return g_maxAng; }
void  PitchRange_SetMaxAngleDeg(float d) {
    if (d < 65.0f) d = 65.0f;   if (d > 89.0f) d = 89.0f;   // same clamps as ReadConfig
    g_maxAng = d;
    TwkMarkDirty();
}

// ------------------------------------------------------------------ sigs (dual-exe-verified)
// InputHandler::GetInputSettings -- Epic 0x1053b10 / Steam 0x1013e90, size 0x27. (this) -> settings*.
static const char* SIG_GET_INPUT_SETTINGS =
    "80 79 20 03 ?? ?? 48 8B 41 10 48 05 A8 00 00 00 C3 80 79 77 00 B8 30 00 00 00 BA 6C 00 00 00 "
    "0F 44 C2";
// FlipTricksHandler::GetBoardExtraPitchAngle -- Epic 0x10234c0 / Steam 0xfe3630. NOT hooked here;
// wanted only as an address range. ⚠️ THIS SCAN IS THE FALLBACK, NOT THE PRIMARY: catch_level hooks
// that function, and MinHook overwrites its prologue with a jump, so this signature stops matching
// the moment that hook is installed. Field-caught exactly that way -- "sig NOT FOUND" purely because
// catch_level happened to install first. Ask catch_level for the address first; this scan only runs
// when catch_level never resolved it (then the prologue is still intact).
static const char* SIG_EXTRA_PITCH =
    "4C 8B DC 49 89 5B 20 56 41 56 41 57 48 81 EC F0 00 00 00 48 8B 41 08 49 8B D9 45 0F B6 F8";

typedef void* (*GetInputSettingsFn)(void*);
static void* g_orig = nullptr, *g_start = nullptr;
static const uint8_t* g_pitchFn = nullptr;

static void* hkGetInputSettings(void* self) {
    void* s = nullptr;
    __try { s = ((GetInputSettingsFn)g_orig)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_faults) == 1) TwkLog("[pitch] caught fatal in GetInputSettings -> recovered");
        return nullptr;
    }
    if (!g_on || !s || !g_pitchFn) return s;
    // Only the board-pitch fetch gets the widened block. Immediate caller only -- see the header.
    const uint8_t* ret = (const uint8_t*)_ReturnAddress();
    if ((size_t)(ret - g_pitchFn) >= (size_t)EXTRA_PITCH_SIZE) return s;
    __try {
        // thread_local: the game thread is the only caller in practice, but a per-thread copy costs
        // nothing and removes the question entirely.
        static thread_local uint8_t copy[IMS_SIZE];
        memcpy(copy, s, IMS_SIZE);
        float* thr = (float*)(copy + IMS_AXIS_X_MAX_ANG);
        const float stock = *thr;
        if (!(stock > 1.0f && stock < 179.0f)) return s;   // implausible -> not this field, hands off
        if (g_maxAng <= stock) return s;                   // never NARROW the range
        *thr = g_maxAng;
        g_uiStock = stock;
        if (InterlockedIncrement(&g_uiHits) == 1)
            TwkLog("[pitch] widening board-pitch range: AxisXMaxAngleThreshold %.0f -> %.0f for this "
                   "call only (input recognition still sees %.0f)", stock, g_maxAng, stock);
        return copy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_on = 0;
        TwkLog("[pitch] caught fatal patching the settings copy -> pitch range off (game unaffected)");
        return s;
    }
}

void PitchRange_Install() {
    // Order-independent by construction: whichever module got there first has an answer.
    const char* how = "from catch_level";
    g_pitchFn = (const uint8_t*)CatchLevel_ExtraPitchFn();
    if (!g_pitchFn) { g_pitchFn = (const uint8_t*)TwkScanExe(SIG_EXTRA_PITCH); how = "by scan"; }
    if (!g_pitchFn) {
        TwkLog("[pitch] GetBoardExtraPitchAngle address unavailable (catch_level has none and the "
               "scan missed) -- cannot scope the change, pitch range off");
        g_on = 0; return;
    }
    g_start = TwkScanExe(SIG_GET_INPUT_SETTINGS);
    if (!g_start) {
        TwkLog("[pitch] GetInputSettings sig NOT FOUND -- pitch range off (game updated?)");
        g_on = 0; return;
    }
    if (MH_CreateHook(g_start, (void*)&hkGetInputSettings, &g_orig) != MH_OK ||
        MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[pitch] hook failed on GetInputSettings -- pitch range off");
        g_start = nullptr; g_on = 0; return;
    }
    TwkLog("[pitch] installed @ %p (pitch fn @ %p %s) -- board pitch spread to %.0f deg (%s)",
           g_start, g_pitchFn, how, g_maxAng, g_on ? "ON" : "off");
}

void PitchRange_DrawMenu(const OmpMenuApi* api) {
    char b[224];
    if (!g_start) { api->TextDisabled("Board pitch range: not installed"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Wider board pitch control", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(stock wastes half the flick range)");
    if (on) {
        api->Indent();
        float a = g_maxAng;
        if (api->SliderFloat("Pitch spread (deg)", &a, 65.0f, 89.0f, "%.0f")) { PitchRange_SetMaxAngleDeg(a); TwkMarkDirty(); }
        const float sat = asinf(fminf(1.0f, 1.0f - cosf(g_maxAng * 3.14159265f / 180.0f))) * 57.2957795f;
        snprintf(b, sizeof(b), "65 = stock. full pitch now needs a %.0f deg flick (was 35); peak pitch unchanged",
                 sat);
        api->TextDisabled(b);
        snprintf(b, sizeof(b), "applied %d time(s) this session", (int)g_uiHits);
        api->TextDisabled(b);
        api->Unindent();
    }
}
