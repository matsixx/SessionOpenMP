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
// SessionOpenMP -- which grind animation a peer's skater plays. See grind_anim.h.
#include "grind_anim.h"
#include "game_syms.h"
#include <atomic>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef OMP_USE_MINHOOK
#include <MinHook.h>
#endif

namespace omp::game::grindanim {

namespace {
std::atomic<void*>    g_ownPawn{nullptr};
std::atomic<uint8_t>  g_ownFace{0};
std::atomic<uint64_t> g_ownFaceMs{0};
std::atomic<uint32_t> g_overrides{0}, g_disagreed{0};

// One entry per proxy skater. Written on the game thread (Proxy::Apply); read from wherever the anim
// graph evaluates, which can be a worker thread -- hence atomics, and a fixed table nobody resizes.
struct Entry { std::atomic<void*> skater{nullptr}; std::atomic<uint8_t> face{0}; };
Entry g_tab[32];

uint64_t nowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}
}  // namespace

void    NoteOwnPawn(void* pawn) { g_ownPawn.store(pawn, std::memory_order_relaxed); }
uint8_t OwnFacing() {
    const uint64_t at = g_ownFaceMs.load(std::memory_order_relaxed);
    if (!at || nowMs() - at > 250) return 0;             // not asked lately: say nothing
    return g_ownFace.load(std::memory_order_relaxed);
}
void NoteProxy(void* skater, uint8_t face) {
    if (!skater) return;
    Entry* freeE = nullptr;
    for (Entry& e : g_tab) {
        void* s = e.skater.load(std::memory_order_relaxed);
        if (s == skater) { e.face.store(face, std::memory_order_relaxed); return; }
        if (!s && !freeE) freeE = &e;
    }
    if (freeE) { freeE->face.store(face, std::memory_order_relaxed); freeE->skater.store(skater, std::memory_order_release); }
}
void ForgetProxy(void* skater) {
    if (!skater) return;
    for (Entry& e : g_tab)
        if (e.skater.load(std::memory_order_relaxed) == skater) {
            e.skater.store(nullptr, std::memory_order_release);
            e.face.store(0, std::memory_order_relaxed);
        }
}
uint32_t Overrides() { return g_overrides.load(std::memory_order_relaxed); }
uint32_t Disagreed() { return g_disagreed.load(std::memory_order_relaxed); }

#ifdef OMP_USE_MINHOOK
namespace {
using BlendSpaceFn = void* (*)(void* animInstance);
using FacingFn     = bool  (*)(void* moveComp, const void* velocity);
BlendSpaceFn o_BlendSpace = nullptr;
FacingFn     o_Facing     = nullptr;
thread_local int t_inBlendSpace = 0;   // GetGrindBlendSpace is on this thread's stack
void (*g_logf)(const char*) = nullptr;

// The ONE call site this module cares about: the grind pose choice. Everything the facing hook does
// is gated on it, so the proxy's movement and every other caller keep the game's own answer.
void* hkBlendSpace(void* ai) {
    ++t_inBlendSpace;
    void* r = o_BlendSpace(ai);
    --t_inBlendSpace;
    return r;
}

bool hkFacing(void* mc, const void* vel) {
    if (t_inBlendSpace <= 0) return o_Facing(mc, vel);
    void* sk = nullptr;
#ifdef _WIN32
    __try { sk = *(void**)((uint8_t*)mc + off::kMoveCompSkater); } __except (EXCEPTION_EXECUTE_HANDLER) { sk = nullptr; }
#endif
    const bool game = o_Facing(mc, vel);
    if (!sk) return game;
    // Ours: record the game's own answer for the wire, and change nothing.
    if (sk == g_ownPawn.load(std::memory_order_relaxed)) {
        g_ownFace.store(game ? 1 : 2, std::memory_order_relaxed);
        g_ownFaceMs.store(nowMs(), std::memory_order_relaxed);
        return game;
    }
    // A proxy with the owner's answer: that answer, whatever this machine's copy of the board says.
    for (Entry& e : g_tab) {
        if (e.skater.load(std::memory_order_acquire) != sk) continue;
        const uint8_t f = e.face.load(std::memory_order_relaxed);
        if (f != 1 && f != 2) break;
        const bool owner = (f == 1);
        g_overrides.fetch_add(1, std::memory_order_relaxed);
        if (owner != game) g_disagreed.fetch_add(1, std::memory_order_relaxed);
        return owner;
    }
    return game;
}
}  // namespace

void Install(void (*logf)(const char*)) {
    static bool done = false;
    if (done) return;
    done = true;
    g_logf = logf;
    const Syms& S = Get();
    if (!S.GrindBlendSpace || !S.IsFacingMoveDir) {
        if (logf) logf("[grindanim] GetGrindBlendSpace / IsFacingMoveDirection NOT RESOLVED -- a peer's grinds "
                       "keep this machine's guess at their stance and direction");
        return;
    }
    const MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { if (logf) logf("[grindanim] MH_Initialize failed"); return; }
    const bool ok = MH_CreateHook(S.GrindBlendSpace, (void*)&hkBlendSpace, (void**)&o_BlendSpace) == MH_OK &&
                    MH_CreateHook(S.IsFacingMoveDir, (void*)&hkFacing,     (void**)&o_Facing)     == MH_OK &&
                    MH_EnableHook(S.GrindBlendSpace) == MH_OK &&
                    MH_EnableHook(S.IsFacingMoveDir) == MH_OK;
    if (logf) logf(ok ? "[grindanim] grind pose seam hooked -- a peer's grind uses THEIR stance and direction, "
                        "not this machine's copy of their board"
                      : "[grindanim] hook FAILED -- a peer's grinds keep this machine's guess");
}
#else
void Install(void (*)(const char*)) {}
#endif

} // namespace omp::game::grindanim
