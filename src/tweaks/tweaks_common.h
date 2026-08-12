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
// SessionTweaks -- shared plumbing for the tweak modules. Everything here is deliberately tiny:
// logging, fault-tolerant reads, the sig scanner, and the ini parser. Game knowledge (offsets,
// sigs, hooks) lives in the module that owns it, one concern per TU. Hard rules:
//   * Before ANY new thunk, establish the real arity and parameter sizes: scan the callee for
//     arg-zone reads with no prior write (real args; watch `mov rax,rsp` prologues -- home writes
//     go through RAX) and the CALL SITE for `movaps xmm1/2/3` (float args). Declare floats
//     `double` and forward them unconverted. An under-declared thunk hands the original garbage
//     and the failure is silent.
//   * Sigs are shipped only after tools/sigmake.py reports them unique in both exes.
//   * Every ini-backed knob is declared at the TOP of its module, above the config reader.
//   * Never log per-frame from a game-thread hook; buffer or edge-trigger.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

void TwkLog(const char* fmt, ...);            // tweaks_mod.cpp; writes SessionTweaks.log

// Fault-tolerant reads: a tweak must never be the thing that crashes the game. Sentinels are
// implausible on purpose so a bad read is visible in the log rather than treated as data.
static inline float twkF(const void* p, int off) {
    __try { return *(const float*)((const uint8_t*)p + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -999999.0f; }
}
static inline int twkB(const void* p, int off) {
    __try { return *((const uint8_t*)p + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static inline int twkI(const void* p, int off) {
    __try { return *(const int*)((const uint8_t*)p + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static inline void* twkP(const void* p, int off) {
    __try { return *(void* const*)((const uint8_t*)p + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Byte-signature scan over the exe's executable sections ("??" = wildcard).
uint8_t* TwkScanExe(const char* sig);

// World-space Z of an actor (UE units = cm), or -999999 when unreadable. PDB-confirmed:
// AActor+0x130 RootComponent, USceneComponent+0x1c0 ComponentToWorld (FTransform: quat 16B,
// translation at +0x10 -> Z at +0x1d8).
static inline float TwkActorZ(const void* actor) {
    void* root = actor ? twkP(actor, 0x130) : nullptr;
    return root ? twkF(root, 0x1d8) : -999999.0f;
}

// Rotate a vector by a quaternion (x,y,z,w) and by its inverse. A DELTA between two component
// bases is pure rotation -- the translation half of a ComponentToWorld never enters it, which is
// why a delta can cross spaces that an absolute position could not.
static inline void TwkQuatRotate(const float q[4], const float v[3], float out[3]) {
    const float vx = v[0], vy = v[1], vz = v[2];      // read first: out may alias v
    const float tx = 2.0f * (q[1] * vz - q[2] * vy);
    const float ty = 2.0f * (q[2] * vx - q[0] * vz);
    const float tz = 2.0f * (q[0] * vy - q[1] * vx);
    out[0] = vx + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = vy + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = vz + q[3] * tz + (q[0] * ty - q[1] * tx);
}
static inline void TwkQuatInvRotate(const float q[4], const float v[3], float out[3]) {
    const float c[4] = { -q[0], -q[1], -q[2], q[3] };  // conjugate == inverse for a unit quaternion
    TwkQuatRotate(c, v, out);
}

// Quaternion product, standard Hamilton order to match TwkQuatRotate above: the result rotates by
// `b` first and then by `a`, so `TwkQuatMul(extra, current, out)` applies `extra` in the PARENT
// frame -- which is how a rotation about a world-stable axis is layered onto a local orientation.
static inline void TwkQuatMul(const float a[4], const float b[4], float out[4]) {
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}
static inline void TwkQuatAxisAngle(const float axis[3], float deg, float out[4]) {
    float n = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(n > 1e-6f)) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }
    const float h = deg * 0.008726646f;                       // (pi/180)/2
    const float s = sinf(h) / n;
    out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(h);
}

// FRotator <-> FQuat, Unreal's own formulas (Pitch/Yaw/Roll in degrees, quat as x,y,z,w).
// The standing "no euler round-trips" rule exists because the GAME's conversion pair is not
// self-inverse. This pair is, by construction -- it is the engine's own math written out both ways,
// so a value can go quat -> rotator -> quat without drifting. Never mix these with the game's.
static inline void TwkRotatorToQuat(const float rot[3], float out[4]) {
    const float k = 0.008726646f;                             // (pi/180)/2
    const float sp = sinf(rot[0] * k), cp = cosf(rot[0] * k); // Pitch
    const float sy = sinf(rot[1] * k), cy = cosf(rot[1] * k); // Yaw
    const float sr = sinf(rot[2] * k), cr = cosf(rot[2] * k); // Roll
    out[0] =  cr * sp * sy - sr * cp * cy;
    out[1] = -cr * sp * cy - sr * cp * sy;
    out[2] =  cr * cp * sy - sr * sp * cy;
    out[3] =  cr * cp * cy + sr * sp * sy;
}
static inline void TwkQuatToRotator(const float q[4], float out[3]) {
    const float X = q[0], Y = q[1], Z = q[2], W = q[3];
    const float sing = Z * X - W * Y;
    const float yawY = 2.0f * (W * Z + X * Y);
    const float yawX = 1.0f - 2.0f * (Y * Y + Z * Z);
    const float R2D = 57.29577951f;
    if (sing < -0.4999995f) {                                  // straight down
        out[0] = -90.0f;
        out[1] = atan2f(yawY, yawX) * R2D;
        out[2] = -out[1] - (2.0f * atan2f(X, W) * R2D);
    } else if (sing > 0.4999995f) {                            // straight up
        out[0] = 90.0f;
        out[1] = atan2f(yawY, yawX) * R2D;
        out[2] = out[1] - (2.0f * atan2f(X, W) * R2D);
    } else {
        float s = 2.0f * sing;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        out[0] = asinf(s) * R2D;
        out[1] = atan2f(yawY, yawX) * R2D;
        out[2] = atan2f(-2.0f * (W * X + Y * Z), 1.0f - 2.0f * (X * X + Y * Y)) * R2D;
    }
}

// A component's ComponentToWorld rotation (USceneComponent+0x1c0, FQuat x,y,z,w). False when the
// result is not a unit quaternion: that means the pointer was not a component, and a basis built
// from it would throw whatever it rotates clean out of the rig rather than fail visibly.
static inline bool TwkCompQuat(const void* comp, float q[4]) {
    if (!comp) return false;
    q[0] = twkF(comp, 0x1c0); q[1] = twkF(comp, 0x1c4);
    q[2] = twkF(comp, 0x1c8); q[3] = twkF(comp, 0x1cc);
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n > 0.98f && n < 1.02f;
}

// Line-based ini int lookup. Comment lines (';' '#' '[') are skipped BEFORE matching: a whole-file
// substring search would match a key inside its own explanatory comment and silently disable the
// feature.
int TwkIniInt(const char* text, const char* key, int def);

// In-place ini value update: replaces the value of `Key=...` (same line matching as TwkIniInt, so
// comments are preserved untouched) or appends `Key=value` at the end. Returns 0 if the text would
// overflow cap.
int TwkIniSetInt(char* text, size_t cap, const char* key, int value);

// The same line splicing for a text value, and the matching reader. A list-valued setting has to be
// editable by hand, so it must survive the auto-save like every other key.
int  TwkIniSetStr(char* text, size_t cap, const char* key, const char* val);
void TwkIniStr(const char* text, const char* key, char* out, size_t cap, const char* def);

// F1-menu changes call this (render thread); the shell auto-saves the ini once things go quiet.
void TwkMarkDirty();
