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
