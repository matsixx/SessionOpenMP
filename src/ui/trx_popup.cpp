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
// SessionOpenMP -- the GAME'S OWN popup, borrowed.
//
// Session ships a TRX popup system, and the controller-disconnected dialog is one of its clients.
// Reusing it means a message from the mod looks like the game's own rather than like a mod's --
// which for "your version is out of date" is the whole point: it has to be believed and it has to
// be seen, and an ImGui window behind the main menu is neither.
//
// THE ROUTE IS THE GAME'S OWN, copied out of UTRXDiscoPadManager::CreateDisconnectedPadPopup rather
// than invented. That function does, in order:
//     rax = <the game instance>                     (two vtable hops off its own this)
//     rbx = UTRXPopupManager::StaticClass()
//     r15 = GetSubsystemInternal(gameInstance + 0xe0, rbx)     <- UGameInstance::SubsystemCollection
//     CreatePopup(r15, &params)
//     ~FTRXPopupCreationParameters(&params)
// We take the game instance from the version hook's own `this` instead of the two hops (see
// popupManager below for why), and the rest is identical.
//
// WARNING: A ZERO-INITIALISED FText IS NOT AN EMPTY FText -- it is a null pointer with a crash attached,
// the same rule the pause menu learned the hard way. The params struct holds FOUR of them and every
// one must be a real FText, including the ones we do not care about.
//
// WARNING: the params are passed BY POINTER, so the callee copies what it keeps and WE still own the
// struct. It has to be destructed or every popup leaks four refcounted texts and four delegates.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../game/game_syms.h"
#include "trx_popup.h"
#include "version_tag.h"

namespace omp { namespace ui {

using namespace omp::game;

// FTRXPopupCreationParameters -- 208 bytes, layout from the PDB. Only the fields we set are named;
// the rest is padding we must still zero and, for the FTexts, fill.
namespace pp {
    constexpr int kSize          = 208;
    constexpr int kTitle         = 0x00;   // FText (24)
    constexpr int kText          = 0x20;   // FText (24)
    constexpr int kPriority      = 0x40;   // int32
    constexpr int kBtnVisibility = 0x44;   // ETRXPopupWidgetButtonsVisibility (1 byte)
    constexpr int kPrimaryText   = 0x48;   // FText (24)
    constexpr int kSecondaryText = 0x60;   // FText (24)
    constexpr int kTag           = 0x88;   // FName
    // 0 = none, 1 = primary only, 2 = both. Taken from the disconnected-pad popup, which shows one
    // button and passes 1; anything else here is a guess and a guess would be visible.
    constexpr unsigned char kPrimaryOnly = 1;
}

// An FText built from plain ASCII, via FName -- the same route and the same reason as the pause
// menu's makeText: FromString and AsCultureInvariant come in const-ref and rvalue-ref twins that no
// signature can tell apart, and picking wrong means the engine steals or double-frees the buffer.
// An FName argument is a POD 8 bytes with no ownership question to get wrong.
struct TextBlob { void* data; void* refCtrl; unsigned flags; unsigned pad; };
static bool makeText(const char* ascii, TextBlob* out) {
    const Syms& S = Get();
    if (!S.TextFromName || !S.FNameCtor || !ascii || !out) return false;
    memset(out, 0, sizeof(*out));
    unsigned long long fname[2] = {0, 0};
    __try {
        S.FNameCtor(fname, ascii, 1 /* FNAME_Add */);
        S.TextFromName(out, fname);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return out->data != nullptr;
}

static void* popupManager() {
    const Syms& S = Get();
    if (!S.GetSubsystem || !S.PopupMgrClass) return nullptr;
    __try {
        // NOT AActor::GetGameInstance: that needs an actor, and at a main menu there is no pawn --
        // which is precisely where this popup has to work. The version hook's own `this` is the game
        // instance, captured for free while the menu draws its version line.
        void* gi = VersionTag_GameInstance();
        if (!gi) return nullptr;
        void* cls = ((void* (*)())S.PopupMgrClass)();
        if (!cls) return nullptr;
        // UGameInstance::SubsystemCollection lives at +0xe0 -- PDB-exact, and the same offset the
        // game's own popup path uses (`lea rcx, [rdi+0xe0]`).
        return S.GetSubsystem((unsigned char*)gi + 0xe0, cls);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool TrxPopup_Available() { return popupManager() != nullptr; }

bool TrxPopup_Show(const char* title, const char* body, const char* buttonText,
                   void (*logf)(const char*)) {
    const Syms& S = Get();
    auto say = [&](const char* m) { if (logf) logf(m); };
    if (!S.PopupCreate || !S.PopupParamsDtor) { say("[popup] not available -- symbols unresolved"); return false; }
    void* mgr = popupManager();
    if (!mgr) { say("[popup] the popup manager is not up yet"); return false; }

    unsigned char params[pp::kSize];
    memset(params, 0, sizeof(params));

    // EVERY FText, not just the ones we use: the struct's destructor walks all four, and a zeroed
    // one is a null dereference rather than an empty string.
    TextBlob t{}, b{}, p1{}, p2{};
    if (!makeText(title      ? title      : " ", &t)  ||
        !makeText(body       ? body       : " ", &b)  ||
        !makeText(buttonText ? buttonText : "OK", &p1) ||
        !makeText(" ", &p2)) {
        say("[popup] could not build the text -- nothing shown");
        return false;
    }
    memcpy(params + pp::kTitle,         &t,  sizeof(t));
    memcpy(params + pp::kText,          &b,  sizeof(b));
    memcpy(params + pp::kPrimaryText,   &p1, sizeof(p1));
    memcpy(params + pp::kSecondaryText, &p2, sizeof(p2));
    *(int*)(params + pp::kPriority) = 0;
    params[pp::kBtnVisibility] = pp::kPrimaryOnly;

    bool ok = false;
    __try {
        S.PopupCreate(mgr, params);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        say("[popup] CreatePopup faulted -- no popup this run");
    }
    // Ours to destroy whether or not the call worked: the struct is built either way, and the
    // callee never took ownership of it.
    __try { S.PopupParamsDtor(params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* nothing left to save */ }
    if (ok) say("[popup] shown");
    return ok;
}

} }  // namespace omp::ui
