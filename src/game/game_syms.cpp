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
// SessionOpenMP -- game symbol resolution. Every sig is verified 1-hit in BOTH the Epic and Steam
// exes; `omp_symcheck` re-proves that from disk on every build.
#include "game_syms.h"
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game {

// The table. `required=false` = the overlay degrades gracefully without it (logged, not fatal).
static const SigEntry kSigs[] = {
    // --- actor plumbing ------------------------------------------------------------------------------
    { "SpawnActor",           "40 53 56 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 0F 28 1D ?? ?? ?? ?? 0F 57 D2 48 8B B4 24 B0 00 00 00 0F 28 CB", true },
    // AActor::GetWorld (Epic 0x29c3690 / Steam 0x2985f70). `Proxy::EnsureSpawned` guards on this in
    // its first line, so without the entry every spawn attempt fails silently.
    { "GetWorld",             "40 53 48 83 EC 20 8B 41 08 48 8B D9 C1 E8 04 A8 01 75 76 48 8B 51 20 48 85 D2 74 6D 8B 42 08 C1 E8 0F A8 01 75 63 8B 42 0C 3B 05 ?? ?? ?? ??", true },
    // AActor::GetGameInstance (Epic 0x29c01e0 / Steam 0x2982ac0, sigmake) -- the COSMETICS anchor:
    // a skater's look lives on the game instance, not on the skater.
    { "GetGameInstance",      "48 83 EC 28 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 80 80 01 00 00 48 83 C4 28 C3", false },
    // ACharacterCustomization::GetCustomizationItem (Epic 0x1025c20 / Steam 0xfe5d90, sigmake) -- the
    // ASSET GATE for cosmetics: an item this install does not have returns null here, and writing it
    // anyway crashes the rebuild.
    { "GetCustomizationItem", "4C 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 ?? ?? 48 63 81 C8 01 00 00 85 C0 ?? ?? 45 8B 1A 4C 8B C8 48 8B 89 C0 01 00 00 33 D2", false },
    // FSoftObjectPath::TryLoad (Epic 0x1533120 / Steam 0x14f4390, sigmake) -- see game_syms.h: the
    // rebuild only RESOLVES, so a peer's garments must be loaded here or the null mesh crashes it.
    { "SoftPathTryLoad",      "48 89 5C 24 08 57 48 83 EC 70 48 8B 01 33 DB 48 8B F9 48 85 C0 0F 84 ?? ?? ?? ?? 83 79 10 01 ?? ?? 48 8D 4C 24 50 48 89 44 24 50", false },
    { "SetActorHidden",       "44 0F B6 41 58 41 0F B6 C0 C0 E8 05 24 01 3A C2 74 13 41 80 E0 DF C0 E2 05 44 0A C2 44 88 41 58 E9 ?? ?? ?? ?? C3", true },
    // Hiding is visual only; a retired proxy keeps its capsule and the camera still collides with it.
    { "SetActorCollision",    "4C 8B DC 55 48 81 EC 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 00 00 00 48 8B E9 0F B6 49 5C 0F B6 C1 C0 E8 03 24 01 3A C2 0F 84 ?? ?? ?? ??", false },
    { "SetActorTick",         "48 89 5C 24 08 57 48 83 EC 20 F6 41 32 02 0F B6 FA 48 8B D9 74 1B BA 30 00 00 00 E8 ?? ?? ?? ?? 84 C0", true },
    // Non-negotiable in overlay mode: without this a host's proxy replicates to every client and lands
    // inside the player it mirrors.
    { "SetReplicates",        "40 53 48 83 EC 20 80 B9 F0 00 00 00 03 48 8B D9 75 59 0F B6 41 5B 48 89 7C 24", true },
    // FQuat overload -- FOUR floats. A 3-float rotator normalises to (0,1,0,0) = upside down.
    { "SetActorLocRot",       "48 81 EC E8 00 00 00 48 8B 89 30 01 00 00 48 85 C9 74 76 0F 10 99 D0 01 00 00 F3 0F 10 02", true },
    { "SetWorldRotQuat",      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 0F B6 F8 49 8B D9 4C 8B C2 48 8B F1 48 8D 54 24 40 E8", true },
    { "RefreshVisuals",       "40 55 53 56 48 8D AC 24 90 FE FF FF 48 81 EC 70 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 50 01 00 00 48 8B F1 E8", false },
    // A wildcard is a promise that the byte does not matter, and a DISPLACEMENT is usually the
    // opposite: it is the identity of the field being touched. Wildcarding the two here (Controller at
    // +0x258, the vcall slot vtbl+0x6c8) makes this sig ambiguous at 2 hits. PDB says
    // APawn::IsLocallyControlled is exe+0x2e51800 and only 43 bytes, so the sig covers the WHOLE
    // function, jump offsets and all.
    { "IsLocallyControlled",  "48 83 EC 28 48 8B 89 58 02 00 00 48 85 C9 74 14 48 8B 01 FF 90 C8 06 00 00 84 C0 74 07 B0 01 48 83 C4 28 C3 32 C0 48 83 C4 28 C3", true },
    // --- board ---------------------------------------------------------------------------------------
    // ASkateboardEx::TeleportTo -- StopMovementImmediately + SetSimulatePhysics + interface move. Use
    // it for the SNAP path only: per-frame stamping through it destroys board-vs-board collision.
    { "BoardTeleport",        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 48 8B F9 49 8B E8", true },
    { "BoardSetRot",          "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 19 48 8B C2 48 8B F9 48 8D", true },
    { "SetSimulatePhysics",   "48 89 5C 24 18 55 56 57 48 83 EC 40 48 8D 99 88 02 00 00", true },
    { "CompSetSimPhys",       "48 81 C1 C8 02 00 00 45 33 C0 E9 ?? ?? ?? ??", false },
    // The whole-board velocity setter -- iterates compound parts, vcalls each part's
    // UPrimitiveComponent::SetPhysicsLinearVelocity (vtbl+0x610). The velocity DRIVE runs on this.
    { "BoardSetLinVel",       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D B9 88 02 00 00 49 8B D9 48 8B 07 48 8B CF 41 0F B6 E8 48 8B F2 FF 50 10", true },
    // --- bail -----------------------------------------------------------------------------------------
    // ASkaterCharacterBase::Bail, small overload (Epic 0xfeb310 / Steam 0xfab140, sigmake): keeps the
    // _canBail displacement 0x649 concrete -- it IS the identity of the gate this function opens with.
    { "Bail",                 "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 70 80 B9 49 06 00 00 00 41 0F B6 F9 41 0F B6 F0 48 8B EA 48 8B D9", false },
    // ASkaterCharacterBase::ResetRagDoll (Epic 0x1000dd0 / Steam 0xfc0c00, sigmake) -- the recovery.
    { "ResetRagDoll",         "48 89 5C 24 20 55 48 83 EC 40 48 8B 01 48 8B D9 48 89 74 24 58 48 89 7C 24 60 FF 90 ?? ?? ?? ?? 48 8B B8 80 01 00 00 48 85 FF", false },
    // USkaterAnimInstance::SaveFootToBoardTransition (Epic 0xf61ee0 / Steam 0xf21cf0, sigmake) --
    // the on/off-board transition pose save; called at the wire's onBoard edges to avoid a T-pose.
    { "SaveFootTrans",        "48 8B C4 55 53 56 57 48 8D A8 88 FE FF FF 48 81 EC 58 02 00 00 44 8B 0D ?? ?? ?? ?? 0F B6 F2 4C 89 70 10 48 8B F9 4C 89 78 D8", false },
    // PATCH TARGET, not a call: AGameModeBase::AllowPausing -- see DisablePause() / game_syms.h.
    { "AllowPausing",         "48 83 EC 28 F6 81 A8 02 00 00 04 ?? ?? E8 ?? ?? ?? ?? 85 C0", false },
    // HOOK TARGET: UGameEngine::Tick (Epic 0x2c2c000 / Steam 0x2bee7e0, sig built by sigmake.py from
    // the PDB address, RIP-relative displacement wildcarded). The game-thread anchor -- see game_syms.h.
    { "EngineTick",           "44 88 44 24 18 55 53 57 41 54 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 4C 8B E1 0F 29 B4 24 A0 00 00 00 48 8D 0D ?? ?? ?? ??", true },
    // The rename-guard pair (see game_syms.h). StaticFindObject Epic 0x1530ec0 / Steam 0x14f2130,
    // UObject::Rename Epic 0x14d9600 / Steam 0x149a870.
    { "StaticFindObject",     "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 45 0F B6 F1 49 8B F8 48 8B DA 4C 8B", true },
    { "RenameObj",            "44 89 4C 24 20 4C 89 44 24 18 55 53 56 57 41 54", true },
    // HOOK TARGET: USkaterAnimInstance::NativeUpdateAnimation (Epic 0xf5e700 / Steam 0xf1e510, sigmake).
    // The pose blob is applied from its POST-hook. Writing it anywhere earlier in the frame loses to
    // the game's own per-frame recomputation, which stomps a pre-tick write before render.
    { "AnimUpdate",           "40 55 57 48 8D AC 24 28 FF FF FF 48 81 EC D8 01 00 00 44 0F 29 9C 24 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 40", false },
    // ASkaterCharacterBase::SetPushState (Epic 0x10062a0 / Steam 0xfc60d0, sigmake) -- see game_syms.h.
    { "SetPushState",         "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 48 8B 89 50 05 00 00 48 8B 01 FF 90 ?? ?? ?? ??", false },
    // The brake multicast implementations (Epic 0xff9a10/0xff9a70, Steam 0xfb9840/0xfb98a0).
    { "StartBraking",         "80 B9 F0 00 00 00 01 ?? ?? 80 FA 01 C6 81 1C 06 00 00 00 66 C7 81 E8 06 00 00 00 00", false },
    { "StopBraking",          "40 53 48 83 EC 20 80 B9 F0 00 00 00 01 0F B6 DA ?? ?? 44 8B C3 41 83 E8 01", false },
    // FName::ToString(FString&) (Epic 0x133a4f0 / Steam 0x12fb7b0, sigmake) -- see game_syms.h.
    { "FNameToString",        "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 8B 01 48 8B F1 8B F8 48 8B DA C1 EF 10 83 79 04 00", false },
    // --- THE AUDIO FUNNEL, all sigmake-verified 1-hit in BOTH exes. See game_syms.h for why this seam
    // is the right one and why the argument lists are known rather than guessed.
    // UReplayAudioManager::SpawnSoundAtLocation  Epic 0x340e120 / Steam 0x33d4fe0
    { "SndSpawnAtLoc",        "48 8B C4 48 89 58 10 55 56 41 54 41 56 41 57 48 8D 68 D1 48 81 EC 00 01 00 00 F2 41 0F 10 01 49 8B D9", false },
    // UReplayAudioManager::SpawnSoundAttached    Epic 0x340e400 / Steam 0x33d52c0
    { "SndSpawnAttached",     "48 8B C4 48 89 58 10 48 89 70 18 55 41 54 41 55 41 56 41 57 48 8D 68 B8 48 81 EC 20 01 00 00 48 8B 75 70", false },
    // UGameplayStatics::SpawnSoundAtLocation     Epic 0x2c5bff0 / Steam 0x2c1e7d0
    { "GsSpawnAtLoc",         "40 53 55 56 57 41 57 48 81 EC 10 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 00 00 00", false },
    // UGameplayStatics::SpawnSoundAttached       Epic 0x2c5c220 / Steam 0x2c1ea00
    { "GsSpawnAttached",      "4C 8B DC 4D 89 43 18 55 53 56 57 41 54 41 56 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 8B 05 ?? ?? ?? ??", false },
    // UAudioComponent::SetIntParameter           Epic 0x2af1a30 / Steam 0x2ab4270
    { "AcSetIntParam",        "48 89 54 24 10 53 55 41 57 48 81 EC 80 00 00 00 48 8B DA 48 8B E9 8B CB 33 D2 45 8B F8 E8 ?? ?? ?? ??", false },
    // SetPitchMultiplier and SetVolumeMultiplier are the same function body writing a DIFFERENT field
    // -- the last four bytes (+0x258 vs +0x238) are the only thing that tells them apart, and that
    // displacement IS the identity. Do not shorten these two or wildcard their tails.
    { "AcSetPitch",           "40 53 48 81 EC 90 00 00 00 F6 81 8A 00 00 00 01 48 8B D9 0F 29 B4 24 80 00 00 00 0F 28 F1 F3 0F 11 B1 58 02 00 00", false },
    { "AcSetVolume",          "40 53 48 81 EC 90 00 00 00 F6 81 8A 00 00 00 01 48 8B D9 0F 29 B4 24 80 00 00 00 0F 28 F1 F3 0F 11 B1 38 02 00 00", false },
    // RANGE MARKER only: UAnimNotify_PlaySoundRecorded::PlaySound  Epic 0x3406cd0 / Steam 0x33cdb90
    { "NotifyPlaySound",      "4C 8B DC 53 48 81 EC 90 00 00 00 F6 41 48 01 48 8B D9 F2 0F 10 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ??", false },
    // FName::FName(const ANSICHAR*, EFindName)   Epic 0x132d4e0 / Steam 0x12ee7a0
    { "FNameCtor",            "48 89 5C 24 08 57 48 83 EC 30 41 8B F8 4C 8B CA 48 8B D9 48 85 D2 ?? ?? 48 C7 C0 FF FF FF FF 90", false },
    // HOOK TARGET: ASkaterCharacter::OnReplayModeChanged  Epic 0xffaa40 / Steam 0xfba870.
    // The replay editor broadcasts a mode change to EVERY subscribed skater, and each one re-parents a
    // component of its own -- `this->[0xa68]->AttachToComponent(<manager>->RootComponent | this->[0xa50])`.
    // On a PROXY one of those pointers is null, so opening the editor with a second skater in the world
    // is a hard AV at +0xc0 inside USceneComponent::AttachToComponent. The guard makes proxies ignore
    // the broadcast, which is also the correct behaviour -- a wire-driven skater is not part of your
    // replay. The derived handler TAIL-JUMPS into ASkaterCharacterBase::OnReplayModeChanged, so one
    // guard covers both halves.
    { "SkaterReplayMode",     "48 89 5C 24 08 57 48 83 EC 20 33 C0 0F B6 FA 48 89 44 24 40 48 8B D9 80 FA 02 ?? ?? 48 8B 0D ?? ?? ?? ??", false },
    // USoundBase::GetConcurrencyHandles  Epic 0x2fc8970 / Steam 0x2f8b450 -- see game_syms.h. Used to
    // raise MaxCount on whatever concurrency a rolling cue actually uses: the stock assets are built
    // for ONE skater, so a second one steals the first's loop instead of playing alongside it.
    { "GetConcHandles",       "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B DA 48 8B F1 E8 ?? ?? ?? ?? 48 8B F8 48 83 B8 18 01 00 00 00", false },
    // ASessionReplayManager::GetInstance  Epic 0x10fa110 / Steam 0x10ba500 -- the route to the replay
    // camera: instance -> _replayInputController (+0x280) -> _replayCamera (+0x260).
    { "ReplayMgrInstance",    "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB ?? ?? E8 ?? ?? ?? ?? 48 8B 4B 10 48 83 C0 30 48 63 50 08 3B 51 38 ?? ??", false },
    // AReplayCamera::SetCameraType  Epic 0x340c080 / Steam 0x33d2f40.
    { "ReplayCamSetType",     "40 53 48 83 EC 70 F6 81 D4 08 00 00 01 48 8B D9 0F 84 ?? ?? ?? ?? 48 89 BC 24 80 00 00 00", false },
    // HOOK TARGET: UCameraReplayComponent::Replaying  Epic 0x340a090 / Steam 0x33d0f50.
    // Clamped, not replaced -- see game_syms.h for why the game reads past its own array.
    { "CamReplaying",         "48 8B C4 53 48 81 EC D0 00 00 00 83 79 50 02 48 8B D9 0F 8C ?? ?? ?? ?? 4C 8B 51 48", false },
    // HOOK TARGET: the derived float-track Replaying override  Epic 0x1158a80 / Steam 0x1118f50.
    // It calls the base above FIRST (which the CamReplaying clamp protects), then lerps ITS OWN
    // float array with its own UNCLAMPED index copies -- count at [this+0xa0], data [this+0x98] --
    // so a track shorter than the manager timeline (a peer who joined after recording began) reads
    // past the allocation. Clamped by the same guard shape as the base.
    { "FloatTrackReplaying",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 48 8B D9 F3 0F 10 74 24 70 49 63 F9 48 63 F2 44 8B CF 8B D6 F3 0F 11 74 24 20 E8 ?? ?? ?? ?? 83 BB A0 00 00 00 02", false },
    // HOOK TARGET: USkeletalMeshComponent::FinalizeBoneTransform  Epic 0x2b58280 / Steam 0x2b1aac0.
    // THE POSE SEAM. Its very first act is the buffer flip (USkinnedMeshComponent::FinalizeBoneTransform
    // -> FlipEditableSpaceBases), so at PRE-hook time the EDITABLE component-space array holds this
    // frame's finished pose and is about to be published. Overwriting it there is the last honest
    // moment to replace a proxy's pose -- the same "be the last writer before the consumer runs" rule
    // as the anim post-pass. PostAnimEvaluation is NOT the seam: it COPIES the evaluated pose into
    // BoneSpaceTransforms and only then fills component space, so anything written before it is simply
    // overwritten.
    { "MeshFinalizeBones",    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8D 8B 20 0B 00 00", false },
    // --- THE GAME'S OWN PAUSE MENU, all sigmake-verified 1-hit in BOTH exes.
    // See game_syms.h for why CreatePageItems is the injection point and why the confirm is hooked at
    // the PAGE level rather than at UPauseMenuPageContainer.
    // UMenuPage::CreatePageItems        Epic 0x1071150 / Steam 0x10314d0
    { "MenuCreateItems",      "48 8B C4 44 88 40 18 55 41 55 41 57 48 8D 68 A1 48 81 EC C0 00 00 00 48 83 B9 D0 02 00 00 00", false },
    // UMenuPage::OnSelectionConfirmed   Epic 0x1090c80 / Steam 0x1051000
    // (23 bytes covers all but the tail `jmp Broadcast` of a 32-byte function -- the two displacements
    // in it, _activePageDefinition +0x298 and the delegate +0x3b0, ARE its identity.)
    { "MenuSelConfirmed",     "F6 81 B0 02 00 00 01 ?? ?? 48 8B 81 98 02 00 00 48 81 C1 B0 03 00 00", false },
    // UMenuPage::RefreshItemsPanel      Epic 0x1096b70 / Steam 0x1056ef0
    { "MenuRefreshItems",     "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 8B D0 02 00 00 48 8B 01", false },
    // UMenuPage::SetTitle               Epic 0x109ab60 / Steam 0x105aee0
    { "MenuSetTitle",         "40 53 48 83 EC 40 48 83 B9 D8 02 00 00 00 48 8D 5A 08 ?? ?? 48 8B 02 48 89 44 24 20", false },
    // UMenuPage::SetSelectedIndex       Epic 0x1099eb0 / Steam 0x105a230
    { "MenuSetSelIndex",      "85 D2 0F 88 ?? ?? ?? ?? 48 89 5C 24 10 48 89 6C 24 18 56 48 81 EC D0 00 00 00 8B 81 A8 02 00 00", false },
    // The VALUE-CHANGE funnels, the exact twins of OnSelectionConfirmed one row above: same
    // (page, params) shape, same "stamp the active definition then Broadcast" body, different
    // delegate offset (+0x380 / +0x398 vs +0x3b0) -- which is the identity, so it stays in the sig.
    // UMenuPage::OnMultiOptionItemSelectionChanged  Epic 0x10822e0 / Steam 0x1042660
    { "MenuMultiChanged",     "40 53 48 83 EC 20 F6 81 B0 02 00 00 01 48 8B D9 ?? ?? 48 8B 81 98 02 00 00", false },
    // UMenuPage::OnProgressBarValueChanged         Epic 0x1090c50 / Steam 0x1050fd0
    { "MenuProgressChanged",  "F6 81 B0 02 00 00 01 ?? ?? 48 8B 81 98 02 00 00 48 81 C1 98 03 00 00 48 89 02", false },
    // UMenuPage::UMenuPageItem::ProgressBarSetPercent  Epic 0x1096330 / Steam 0x10566b0.
    // The float is a NORMALISED 0..1 fraction, not the displayed number: the body clamps it to
    // [0,1] against a constant before touching the UProgressBar (+0x3b0). The displayed value comes
    // from the definition's _progressDisplayValueMinimum/Maximum, so the caller maps both ways.
    { "MenuProgressSetPct",   "48 8B C4 57 48 81 EC 10 01 00 00 F3 0F 10 15 ?? ?? ?? ?? 48 8B F9 48 8B 89 B0 03 00 00", false },
    // ASessionPlayerController::IsPauseMenuDisplayed  Epic 0xf8e7c0 / Steam 0xf4e5d0. Twelve bytes:
    // `cmp qword [rcx+0x710], 0 ; setne al`. It is the game's own answer to "is the pause menu on
    // screen", which is what makes it safe to keep refreshing a menu page from the frame pump.
    { "PauseMenuShown",       "48 83 B9 10 07 00 00 00 0F 95 C0 C3 CC CC CC CC 0F B6 81 70 01 00 00 24 01 C3", false },
    // UMenuPageItem::MultiOptionSetSelectedItemIndex  Epic 0x1078e80 / Steam 0x1039200. (this, int32)
    // -- 2 args. It BROADCASTS _onMultiOptionItemSelectionChanged (+0x278), so calling it merely to
    // display a value re-enters the mod's own change hook; the caller must guard for that.
    { "MenuMultiSetIndex",    "85 D2 0F 88 ?? ?? ?? ?? 4C 8B DC 56 48 81 EC C0 00 00 00 8B 81 D0 03 00 00", false },
    // --- THE VERSION TAG. See version_tag.h for why two of these are RANGE MARKERS.
    // USessionGameInstance::GetGameVersion             Epic 0x11807b0 / Steam 0x1140ed0 -- HOOKED
    { "GameVersion",          "40 53 48 83 EC 40 48 8B 0D ?? ?? ?? ?? 33 C0 48 89 44 24 30 48 8B DA 48 89 44 24 38", false },
    // RANGE MARKER: ASessionPlayerController::CreateIntroUI   Epic 0xf84090 / Steam 0xf43ea0
    { "IntroUiRange",         "48 89 5C 24 18 56 48 83 EC 50 48 83 B9 70 06 00 00 00 48 8B F2 48 8B D9 0F 85 ?? ?? ?? ??", false },
    // RANGE MARKER: UPauseMenuPageContainer::NativeOnInitialized  Epic 0x107a8a0 / Steam 0x103ac20
    { "PauseInitRange",       "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B F9 E8 ?? ?? ?? ??", false },
    // FMemory::Malloc  Epic 0x123e750 / Steam 0x11ff180   ·   FMemory::Free  Epic 0x1231ff0 / Steam 0x11f29d0
    { "MemMalloc",            "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 ?? ??", false },
    { "MemFree",              "48 85 C9 ?? ?? 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 ?? ?? E8 ?? ?? ?? ??", false },
    // NOT A FUNCTION START -- a CALL SITE. This is 61 bytes of
    // `FCustomizationMenuPageEventHandler::OnPageMultiOptionItemAutoGenerated` ending exactly on its
    // `call FText::FromName`, because FromName's own body cannot be sig'd (it is byte-identical to
    // FPackageName::GetShortName -- 2 hits in both exes at ANY length). The bind block decodes the
    // trailing E8's displacement to get the real address; verified from disk to land on Epic 0x126d3f0
    // and Steam 0x122ded0.  Site: Epic 0x108949b / Steam 0x104981b.
    { "MenuTextSite",         "49 8B CD E8 ?? ?? ?? ?? 48 8B C8 49 8D 56 08 E8 ?? ?? ?? ?? 48 85 C0 ?? ?? 0F B6 90 B0 00 00 00 48 8B 4D D7 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 49 8B D6 48 8D 4D 0F E8 ?? ?? ?? ??", false },
    // --- THE FLOATING PLAYER NAMES. Both optional: without them no nameplate is ever drawn.
    // APlayerController::ProjectWorldLocationToScreenWithDistance  Epic 0x2ed3c00 / Steam 0x2e966e0
    { "ProjectToScreen",      "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 20 01 00 00 48 8B 99 98 02 00 00 41 0F B6 F1 49 8B E8 48 8B FA", false },
    // APlayerController::GetViewportSize                           Epic 0x2ecd8a0 / Steam 0x2e90380
    { "GetViewportSize",      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 33 C0 49 8B F8 89 02 48 8B F2 41 89 00 48 8B 99 98 02 00 00", false },
    // --- BOARD BREAKAGE. Both optional: without them a peer's break simply does not show.
    // ASkateboardEx::BreakBoard_Internal  Epic 0xf81580 / Steam 0xf41390 (sigmake)
    { "BreakBoardInternal",   "48 89 5C 24 18 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC C0 00 00 00 41 0F B6 F8 0F B6 F2 4C 8B F1", false },
    // ASkateboardEx::RebuildBrokenBoard   Epic 0xf965b0 / Steam 0xf563c0 (sigmake): keeps the
    // _currentBrokenBoardState displacement 0x3f1 concrete -- it IS the identity of the gate it opens with.
    { "RebuildBrokenBoard",   "44 88 44 24 18 88 54 24 10 48 89 4C 24 08 55 41 54 41 57 48 8D 6C 24 B9 48 81 EC B0 00 00 00 80 B9 F1 03 00 00 00 45 0F B6 E0", false },
    // --- CALL SITE, not a function (the MenuTextSite pattern): the Emplace call inside
    // ACharacterCustomization::SetProfileItem, whose E8 targets TMapBase<int,
    // FCustomizationProfileItem>::Emplace. The Emplace BODY cannot be sig'd -- 7+ template
    // instantiations are byte-twins once the helper rel32s are wildcarded. The site also proves the
    // ABI on its face: rcx = &map, rdx = &int32 key (rsp+0x30), r8 = &16-byte item (rsp+0x38).
    // Site: Epic 0x1034237 / Steam 0xff4497; target Epic 0x7dbb60 / Steam 0x895eb0.
    { "ProfileEmplaceSite",   "4C 8D 97 E0 02 00 00 49 8B 4F 18 48 89 4C 24 38 4C 8D 44 24 38 49 8B CA 44 88 6C 24 40 48 8D 54 24 30 44 89 64 24 44 E8 ?? ?? ?? ??", false },
    // HOOK TARGET: ASkaterCharacter::PopulateMarkerInfo  Epic 0xffe090 / Steam 0xfbdec0 (sigmake).
    // Keeps the -0x4f0 sub-object displacement concrete. See the Syms field comment for the guard.
    { "PopulateMarkerInfo",   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 C6 02 01 48 8B DA 48 8B 81 10 FB FF FF 48 8B F9 48 85 C0 ?? ?? 0F B6 88 20 0E 00 00", false },
};
static const int kSigN = (int)(sizeof(kSigs) / sizeof(kSigs[0]));

int             SigCount()      { return kSigN; }
const SigEntry& SigAt(int i)    { return kSigs[i]; }

static Syms g_syms;
const Syms& Get() { return g_syms; }

#ifdef _WIN32
// FName -> ASCII. The FString buffer is deliberately leaked, exactly as in audio.cpp/pause_menu.cpp:
// this runs when a lobby is advertised or browsed, not per frame.
static bool fnameToAscii(const void* fnamePtr, char* out, int cap) {
    out[0] = 0;
    if (!g_syms.FNameToString || !fnamePtr) return false;
    __try {
        struct FStr { wchar_t* d; int n; int max; } fs{};
        g_syms.FNameToString(fnamePtr, &fs);
        if (!fs.d || fs.n <= 0) return false;
        int k = 0;
        for (; k < fs.n && k < cap - 1 && fs.d[k]; k++) out[k] = (char)(fs.d[k] < 128 ? fs.d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
bool ObjectName(const void* obj, char* out, int cap) {
    if (out && cap) out[0] = 0;
    if (!obj || !out || cap <= 0) return false;
    return fnameToAscii((const uint8_t*)obj + off::kObjNamePrivate, out, cap);
}
bool LocalSkaterName(void* pawn, char* out, int cap) {
    if (out && cap) out[0] = 0;
    if (!pawn || !g_syms.GetGameInstance || !out) return false;
    __try {
        void* gi = g_syms.GetGameInstance(pawn);
        if (!gi) return false;
        // The look (and the name) live on the GAME INSTANCE, not the skater.
        const uint8_t* inst = (const uint8_t*)gi + off::kGiSkaterInstance;
        return fnameToAscii(inst + off::kSkInstName, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// ---- pretty map labels -------------------------------------------------------------------------------
static void* g_mapData = nullptr;
bool HaveMapSelectData() { return g_mapData != nullptr; }
bool CacheMapSelectData(void* pawn) {
    if (g_mapData) return true;
    if (!pawn || !g_syms.GetGameInstance) return false;
    __try {
        void* gi = g_syms.GetGameInstance(pawn);
        if (!gi) return false;
        void* a = *(void**)((uint8_t*)gi + off::kGiMapSelectData);
        if (!a) return false;
        // Believe it only if it looks like the array the consumer bounds-checks: a sane count and a
        // non-null buffer. A wrong pointer here would be read as an array of 120-byte structs and
        // have its FTexts called through -- worth one cheap sanity test.
        const void*   arr = *(const void* const*)((const uint8_t*)a + off::kMapDataMaps);
        const int32_t num = *(const int32_t*)((const uint8_t*)a + off::kMapDataMaps + 8);
        if (!arr || num <= 0 || num > 512) return false;
        g_mapData = a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// FText -> ASCII, with NO new symbol. Both `FText::ToString` and `FTextInspector::GetDisplayString`
// disassemble to the same three instructions: take the FText's TextData pointer, walk its vtable, and
// tail-jump slot +0x10 = ITextData::GetDisplayString(), which returns a const FString*. A vtable slot
// beats a signature whenever the body is this small and this shared -- `GetDisplayString` is even
// COMDAT-folded with an unrelated function in this build, so it cannot be sig'd at all.
static bool ftextToAscii(const void* ftext, char* out, int cap) {
    out[0] = 0;
    if (!ftext) return false;
    __try {
        void* textData = *(void**)ftext;
        if (!textData) return false;
        void** vt = *(void***)textData;
        if (!vt) return false;
        using GetDispFn = const void* (*)(void*);
        const void* fstr = ((GetDispFn)vt[2])(textData);         // vtbl + 0x10 = slot 2
        if (!fstr) return false;
        struct FStr { const wchar_t* d; int n; int max; };
        const FStr* s = (const FStr*)fstr;
        if (!s->d || s->n <= 0) return false;
        int k = 0;
        for (; k < s->n && k < cap - 1 && s->d[k]; k++) out[k] = (char)(s->d[k] < 128 ? s->d[k] : '?');
        out[k] = 0;
        return k > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// The world object name is the PERSISTENT level ("NYC01_LP_Persistent"); a table entry may be
// authored either with that suffix or without it. Comparing on the stem makes both spellings the
// same key, which is a real equivalence rather than a guess -- it is the same level either way.
static void mapStem(const char* in, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!in) return;
    strncpy_s(out, (size_t)cap, in, _TRUNCATE);
    const size_t n = strlen(out);
    static const char kSuf[] = "_Persistent";
    const size_t sn = sizeof(kSuf) - 1;
    if (n > sn && _stricmp(out + n - sn, kSuf) == 0) out[n - sn] = 0;
}

bool PrettyMapName(const char* internalName, char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (!internalName || !*internalName) return false;
    strncpy_s(out, (size_t)cap, internalName, _TRUNCATE);         // fall back to the name given
    if (!g_mapData) return false;
    char wantStem[64]; mapStem(internalName, wantStem, sizeof(wantStem));
    __try {
        // UMapSelectDataAsset::_maps at +0x30; FMapSelectData is 120 bytes with MapName at +0x00 and
        // MapLabel (FText) at +0x10 -- all PDB-exact.
        const uint8_t* arr = *(const uint8_t* const*)((const uint8_t*)g_mapData + off::kMapDataMaps);
        const int32_t  num = *(const int32_t*)((const uint8_t*)g_mapData + off::kMapDataMaps + 8);
        if (!arr || num <= 0 || num > 512) return false;
        // Exact first, stem second: an exact hit must never lose to a stem collision.
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < num; i++) {
                const uint8_t* e = arr + (size_t)i * off::kMapDataStride;
                char nm[64];
                if (!fnameToAscii(e + off::kMapDataName, nm, sizeof(nm))) continue;
                if (pass == 0) {
                    if (_stricmp(nm, internalName) != 0) continue;
                } else {
                    char haveStem[64]; mapStem(nm, haveStem, sizeof(haveStem));
                    if (!haveStem[0] || _stricmp(haveStem, wantStem) != 0) continue;
                }
                char label[64];
                if (ftextToAscii(e + off::kMapDataLabel, label, sizeof(label)) && label[0]) {
                    strncpy_s(out, (size_t)cap, label, _TRUNCATE);
                    return true;
                }
                return false;      // entry found but its label would not read: keep the raw name
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return false;
}

// Every entry of the label table, once per session. A map whose pretty name does not appear is
// either absent from this table or carries a label that will not read, and those two have identical
// symptoms on screen -- this is what tells them apart without another build.
void LogMapSelectTable(void (*logf)(const char*)) {
    if (!logf || !g_mapData) return;
    __try {
        const uint8_t* arr = *(const uint8_t* const*)((const uint8_t*)g_mapData + off::kMapDataMaps);
        const int32_t  num = *(const int32_t*)((const uint8_t*)g_mapData + off::kMapDataMaps + 8);
        if (!arr || num <= 0 || num > 512) return;
        { char m[80]; snprintf(m, sizeof(m), "[maps] label table: %d entr%s", num, num == 1 ? "y" : "ies");
          logf(m); }
        char line[220]; int n = 0;
        for (int i = 0; i < num; i++) {
            const uint8_t* e = arr + (size_t)i * off::kMapDataStride;
            char nm[64] = {0}, label[64] = {0};
            fnameToAscii(e + off::kMapDataName, nm, sizeof(nm));
            const bool haveLabel = ftextToAscii(e + off::kMapDataLabel, label, sizeof(label)) && label[0];
            const uint8_t dlc = *(const uint8_t*)(e + off::kMapDataDlc);
            const int w = snprintf(line + n, sizeof(line) - n, "%s%s=%s%s",
                                   n ? " | " : "   ", nm[0] ? nm : "(unnamed)",
                                   haveLabel ? label : "<NO LABEL>", dlc ? "*" : "");
            if (w <= 0) break;
            n += w;
            if (n > 150 || i == num - 1) { logf(line); n = 0; line[0] = 0; }
        }
        logf("[maps] (* = DLC entry; <NO LABEL> = in the table but its display text would not read)");
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// CACHED ON THE WORLD POINTER, AND IT MUST STAY THAT WAY. `fnameToAscii` leaks the FString that
// FName::ToString allocates (there is no Free for it), so it is only safe on one-time or cached paths.
// Calling this per frame leaks an engine allocation 60-120 times a second and hammers FMallocBinned2
// until the renderer dies inside it. The world pointer changes exactly when the map does, so this
// recomputes precisely as often as the answer can change: once per level load.
bool LocalMapName(void* pawn, char* out, int cap) {
    if (out && cap) out[0] = 0;
    if (!pawn || !g_syms.GetWorld || !out) return false;
    static void* s_world = nullptr;
    static char  s_name[64] = {0};
    __try {
        void* w = g_syms.GetWorld(pawn);
        if (!w) return false;
        if (w != s_world) {
            s_world = w;
            s_name[0] = 0;
            fnameToAscii((const uint8_t*)w + off::kObjNamePrivate, s_name, sizeof(s_name));
        }
        if (!s_name[0]) return false;
        strncpy_s(out, (size_t)cap, s_name, _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
bool ObjectName(const void*, char* o, int c) { if (o && c) o[0] = 0; return false; }
bool LocalSkaterName(void*, char* o, int c) { if (o && c) o[0] = 0; return false; }
bool LocalMapName(void*, char* o, int c)    { if (o && c) o[0] = 0; return false; }
#endif

#ifdef _WIN32
// ---- pattern scan over the loaded image's executable sections ----------------------------------------
struct Pat { uint8_t b[64]; bool wild[64]; int n; };
static bool parsePat(const char* sig, Pat& p) {
    p.n = 0;
    for (const char* c = sig; *c && p.n < 64; ) {
        if (*c == ' ') { c++; continue; }
        if (c[0] == '?') { p.wild[p.n] = true; p.b[p.n] = 0; p.n++; c += (c[1] == '?') ? 2 : 1; continue; }
        unsigned v = 0; int k = 0;
        while (k < 2 && ((c[k] >= '0' && c[k] <= '9') || (c[k] >= 'A' && c[k] <= 'F') || (c[k] >= 'a' && c[k] <= 'f'))) {
            const char h = c[k];
            v = v * 16 + (unsigned)((h <= '9') ? h - '0' : ((h | 32) - 'a' + 10)); k++;
        }
        if (!k) return false;
        p.wild[p.n] = false; p.b[p.n] = (uint8_t)v; p.n++; c += k;
    }
    return p.n > 0;
}
static void* scanRange(const uint8_t* base, size_t len, const Pat& p, int* hits) {
    void* first = nullptr;
    if (p.n == 0 || len < (size_t)p.n) return nullptr;
    for (size_t i = 0; i + p.n <= len; i++) {
        int k = 0;
        for (; k < p.n; k++) if (!p.wild[k] && base[i + k] != p.b[k]) break;
        if (k == p.n) { if (hits) (*hits)++; if (!first) first = (void*)(base + i); }
    }
    return first;
}

const Syms& Resolve(void (*logf)(const char*)) {
    auto say = [&](const char* s) { if (logf) logf(s); };
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    void* found[kSigN] = {};
    int   hits [kSigN] = {};
    for (int i = 0; i < kSigN; i++) {
        Pat p;
        if (!parsePat(kSigs[i].sig, p)) continue;
        for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
            if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            void* f = scanRange(base + sec[s].VirtualAddress, sec[s].Misc.VirtualSize, p, &hits[i]);
            if (f && !found[i]) found[i] = f;
        }
        char m[220];
        snprintf(m, sizeof(m), "[sym] %-22s %s exe+%p (hits=%d)%s", kSigs[i].name,
                 found[i] ? "->" : "!! NOT FOUND", found[i] ? (void*)((uint8_t*)found[i] - base) : nullptr,
                 hits[i], hits[i] > 1 ? "  *** AMBIGUOUS -- sig needs lengthening" : "");
        say(m);
    }
    // bind (order matches the table)
    int i = 0;
    // THIS BLOCK IS POSITIONAL: `found[]` is indexed in kSigs ORDER, so inserting a table entry
    // without inserting its assignment here silently shifts every symbol after it onto the wrong
    // address. Keep the two lists in lockstep; the readiness check below catches mistakes.
    g_syms.SpawnActor         = (SpawnActorFn)      found[i++];
    g_syms.GetWorld           = (GetWorldFn)        found[i++];
    g_syms.GetGameInstance    = (GetGameInstFn)     found[i++];
    g_syms.GetCustomizationItem = (GetCustomItemFn) found[i++];
    g_syms.SoftPathTryLoad    = (TryLoadFn)         found[i++];
    g_syms.SetActorHidden     = (SetHiddenFn)       found[i++];
    g_syms.SetActorCollision  = (SetCollisionFn)    found[i++];
    g_syms.SetActorTick       = (SetActorTickFn)    found[i++];
    g_syms.SetReplicates      = (SetReplicatesFn)   found[i++];
    g_syms.SetActorLocRot     = (SetActorLocRotFn)  found[i++];
    g_syms.SetWorldRotQuat    = (SetWorldRotQFn)    found[i++];
    g_syms.RefreshVisuals     = (RefreshVisualsFn)  found[i++];
    g_syms.IsLocallyControlled= (IsLocallyCtrlFn)   found[i++];
    g_syms.BoardTeleport      = (BoardTeleportFn)   found[i++];
    g_syms.BoardSetRot        = (BoardSetRotFn)     found[i++];
    g_syms.SetSimulatePhysics = (SetSimPhysFn)      found[i++];
    g_syms.CompSetSimPhys     = (CompSetSimPhysFn)  found[i++];
    g_syms.BoardSetLinVel     = (BoardSetLinVelFn)  found[i++];
    g_syms.Bail               = (BailFn)            found[i++];
    g_syms.ResetRagDoll       = (ResetRagDollFn)    found[i++];
    g_syms.SaveFootTrans      = (SaveFootTransFn)   found[i++];
    g_syms.AllowPausing       =                     found[i++];   // patched, never called
    g_syms.EngineTick         =                     found[i++];   // hooked, never called directly
    g_syms.StaticFindObject   = (SFOFn)             found[i++];
    g_syms.RenameObj          =                     found[i++];   // hooked, never called directly
    g_syms.AnimUpdate         =                     found[i++];   // hooked, never called directly
    g_syms.SetPushState       = (SetPushStateFn)    found[i++];
    g_syms.StartBraking       = (BrakeRpcFn)        found[i++];
    g_syms.StopBraking        = (BrakeRpcFn)        found[i++];
    g_syms.FNameToString      = (FNameToStringFn)   found[i++];
    g_syms.SndSpawnAtLoc      = (SpawnSndAtLocFn)   found[i++];
    g_syms.SndSpawnAttached   = (SpawnSndAttFn)     found[i++];
    g_syms.GsSpawnAtLoc       = (SpawnSndAtLocFn)   found[i++];
    g_syms.GsSpawnAttached    = (SpawnSndAttFn)     found[i++];
    g_syms.AcSetIntParam      = (AcSetIntFn)        found[i++];
    g_syms.AcSetPitch         = (AcSetFloatFn)      found[i++];
    g_syms.AcSetVolume        = (AcSetFloatFn)      found[i++];
    g_syms.NotifyPlaySound    =                     found[i++];   // range marker, never called
    g_syms.FNameCtor          = (FNameCtorFn)       found[i++];
    g_syms.SkaterReplayMode   =                     found[i++];   // hooked, never called directly
    g_syms.GetConcHandles     = (GetConcHandlesFn)  found[i++];
    g_syms.ReplayMgrInstance  = (GetReplayMgrFn)    found[i++];
    g_syms.ReplayCamSetType   = (SetCamTypeFn)      found[i++];
    g_syms.CamReplaying       =                     found[i++];   // hooked, never called directly
    g_syms.FloatTrackReplaying =                    found[i++];   // hooked, never called directly
    g_syms.MeshFinalizeBones  =                     found[i++];   // hooked, never called directly
    g_syms.MenuCreateItems    =                     found[i++];   // hooked, never called directly
    g_syms.MenuSelConfirmed   =                     found[i++];   // hooked, never called directly
    g_syms.MenuRefreshItems   = (MenuRefreshFn)     found[i++];
    g_syms.MenuSetTitle       = (MenuSetTitleFn)    found[i++];
    g_syms.MenuSetSelIndex    = (MenuSetSelIdxFn)   found[i++];
    g_syms.MenuMultiChanged   =                     found[i++];   // hooked, never called directly
    g_syms.MenuProgressChanged=                     found[i++];   // hooked, never called directly
    g_syms.MenuProgressSetPct = (MenuProgSetPctFn)  found[i++];
    g_syms.PauseMenuShown     = (PauseShownFn)      found[i++];
    g_syms.MenuMultiSetIndex  = (MenuMultiSetIdxFn) found[i++];
    g_syms.GameVersion        =                     found[i++];   // hooked, never called directly
    g_syms.IntroUiRange       =                     found[i++];   // range marker, never called
    g_syms.PauseInitRange     =                     found[i++];   // range marker, never called
    g_syms.MemMalloc          = (MemMallocFn)       found[i++];
    g_syms.MemFree            = (MemFreeFn)         found[i++];
    // The one entry whose match is NOT the target function: MenuTextSite is a CALL SITE, so the symbol
    // is the target of the `E8 rel32` in its last five bytes. Decoding beats a body sig here because
    // FText::FromName has an exact byte twin (see the table comment). The opcode must actually BE an
    // E8 -- if the pattern ever matches something else, no address is taken at all.
    {
        const int siteIdx = i;
        const uint8_t* site = (const uint8_t*)found[i++];
        // Length comes from re-parsing the entry's OWN sig, never a hardcoded 61: lengthening the
        // pattern later would otherwise silently decode the wrong five bytes.
        Pat sp;
        if (site && parsePat(kSigs[siteIdx].sig, sp) && sp.n >= 5) {
            const uint8_t* call = site + sp.n - 5;
            if (*call == 0xE8) {
                int32_t rel = 0; memcpy(&rel, call + 1, 4);
                g_syms.TextFromName = (TextFromNameFn)(call + 5 + rel);
            }
        }
        char m[160];
        snprintf(m, sizeof(m), "[sym] %-22s %s exe+%p (decoded from MenuTextSite)", "FText::FromName",
                 g_syms.TextFromName ? "->" : "!! NOT DECODED",
                 g_syms.TextFromName ? (void*)((uint8_t*)g_syms.TextFromName - base) : nullptr);
        say(m);
    }
    // Appended AFTER the MenuTextSite block on purpose: that block consumes its own slot with
    // `found[i++]`, so the positional run continues here and the table stays append-only.
    g_syms.ProjectToScreen    = (ProjectToScreenFn) found[i++];
    g_syms.GetViewportSize    = (ViewportSizeFn)    found[i++];
    g_syms.BreakBoardInternal = (BreakBoardIntFn)   found[i++];
    g_syms.RebuildBrokenBoard = (RebuildBoardFn)    found[i++];
    // ProfileEmplaceSite is a CALL SITE like MenuTextSite: the symbol is the E8 target in its last
    // five bytes. Same guard -- if the last opcode is not an E8, no address is taken at all.
    {
        const int siteIdx = i;
        const uint8_t* site = (const uint8_t*)found[i++];
        Pat sp;
        if (site && parsePat(kSigs[siteIdx].sig, sp) && sp.n >= 5) {
            const uint8_t* call = site + sp.n - 5;
            if (*call == 0xE8) {
                int32_t rel = 0; memcpy(&rel, call + 1, 4);
                g_syms.ProfileEmplace = (ProfileEmplaceFn)(call + 5 + rel);
            }
        }
        char m[160];
        snprintf(m, sizeof(m), "[sym] %-22s %s exe+%p (decoded from ProfileEmplaceSite)", "ProfileEmplace",
                 g_syms.ProfileEmplace ? "->" : "!! NOT DECODED",
                 g_syms.ProfileEmplace ? (void*)((uint8_t*)g_syms.ProfileEmplace - base) : nullptr);
        say(m);
    }
    g_syms.PopulateMarkerInfo = found[i++];

    // LOCKSTEP CHECK. The block above is POSITIONAL, and a table entry added without its assignment --
    // or vice versa -- shifts every later symbol onto the wrong address SILENTLY: sigs still resolve
    // 1-hit, symcheck still passes, and the mod hooks whatever happens to sit at the shifted index.
    // `i` has consumed exactly one slot per assignment, so comparing it to kSigN catches the whole
    // class of mistake in one line, loudly, at startup.
    if (i != kSigN) {
        char mm[220];
        snprintf(mm, sizeof(mm),
                 "[sym] *** SIG TABLE/BINDING MISMATCH: %d assignments for %d table entries -- every"
                 " symbol after the gap is bound to the WRONG address. Fix before trusting anything.",
                 i, kSigN);
        say(mm);
    }

    g_syms.total = kSigN; g_syms.resolved = 0;
    int missingRequired = 0;
    for (int k = 0; k < kSigN; k++) {
        if (found[k]) g_syms.resolved++;
        else if (kSigs[k].required) missingRequired++;
    }
    char m[260];
    snprintf(m, sizeof(m), "[sym] resolved %d/%d%s", g_syms.resolved, g_syms.total,
             missingRequired ? "  *** REQUIRED SYMBOLS MISSING -- overlay will not drive proxies" : "");
    say(m);

    // A resolved COUNT cannot report a symbol that has no table entry at all, and a hand-picked
    // readiness triple can silently exclude one. Readiness is therefore derived from the FIELDS THE
    // SPAWN PATH ACTUALLY DEREFERENCES, named one by one: a check that enumerates its own dependencies
    // cannot drift out of date.
    struct Dep { const char* name; const void* p; };
    const Dep deps[] = {
        { "SpawnActor",     (const void*)g_syms.SpawnActor },
        { "GetWorld",       (const void*)g_syms.GetWorld },
        { "SetReplicates",  (const void*)g_syms.SetReplicates },
        { "SetActorTick",   (const void*)g_syms.SetActorTick },
        { "SetActorLocRot", (const void*)g_syms.SetActorLocRot },
        { "BoardSetLinVel", (const void*)g_syms.BoardSetLinVel },
        { "IsLocallyControlled", (const void*)g_syms.IsLocallyControlled },
        // Not dereferenced by the spawn CODE, but a spawn without the rename guard is a GUARANTEED
        // LowLevelFatalError (the game renames every new skater onto the taken name "Skater") -- so the
        // guard's two symbols are spawn-path dependencies in the only sense that matters.
        { "StaticFindObject", (const void*)g_syms.StaticFindObject },
        { "RenameObj",      (const void*)g_syms.RenameObj },
    };
    char missing[200] = {0};
    for (const Dep& d : deps) {
        if (d.p) continue;
        if (missing[0]) strncat_s(missing, ", ", _TRUNCATE);
        strncat_s(missing, d.name, _TRUNCATE);
    }
    if (missing[0]) snprintf(m, sizeof(m), "[sym] *** PROXIES DISABLED -- spawn path needs: %s", missing);
    else            snprintf(m, sizeof(m), "[sym] spawn path complete (%d dependencies present)", (int)(sizeof(deps)/sizeof(deps[0])));
    say(m);
    return g_syms;
}

// See the rationale in game_syms.h. Idempotent: re-running it re-reads the bytes and no-ops if the
// patch is already in place, so a second call (a level change, a re-arm) can never corrupt the head.
bool DisablePause(void (*logf)(const char*)) {
    uint8_t* p = (uint8_t*)g_syms.AllowPausing;
    if (!p) {
        if (logf) logf("[pause] AllowPausing not resolved -- pause still freezes the world");
        return false;
    }
    if (p[0] == 0x30 && p[1] == 0xC0 && p[2] == 0xC3) return true;      // already patched
    DWORD old = 0;
    if (!VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old)) {
        if (logf) logf("[pause] VirtualProtect failed -- NOT patched");
        return false;
    }
    p[0] = 0x30; p[1] = 0xC0; p[2] = 0xC3;                             // xor al,al ; ret  => return false
    VirtualProtect(p, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 3);
    if (logf) {
        char m[200];
        snprintf(m, sizeof(m), "[pause] world-freeze DISABLED (AGameModeBase::AllowPausing -> false at %p)"
                               " -- the pause menu still opens", (void*)p);
        logf(m);
    }
    return true;
}
#else
const Syms& Resolve(void (*)(const char*)) { return g_syms; }
bool DisablePause(void (*)(const char*)) { return false; }
#endif

// =====================================================================================================
// LIVE PROXY REGISTRY. See game_syms.h for how entry lifetime is keyed.
// =====================================================================================================
static uint8_t g_localReplayMode = 0;
static uint8_t g_lastLiveReplayMode = 1;    // what "live" is on this build; every non-playback
                                            // mode the local skater reports refreshes it
void    SetLocalReplayMode(uint8_t m) { g_localReplayMode = m; if (m != 2) g_lastLiveReplayMode = m; }
uint8_t LocalReplayMode() { return g_localReplayMode; }

// The scrub clock: CurrentPlayTime/TotalPlayTime in seconds off the live manager's active instance
// data. Pure reads under SEH -- the manager getter and both pointers are game-owned and may be down
// during loads.
bool ReplayPlayTime(float* cur, float* total) {
    const Syms& S = Get();
    if (!S.ReplayMgrInstance) return false;
#ifdef _WIN32
    __try {
        void* mgr = S.ReplayMgrInstance();
        if (!mgr) return false;
        void* inst = *(void**)((uint8_t*)mgr + off::kReplayMgrActiveInst);
        if (!inst) return false;
        const float c = *(const float*)((uint8_t*)inst + off::kReplayInstCurTime);
        const float t = *(const float*)((uint8_t*)inst + off::kReplayInstTotalTime);
        if (!(c >= 0.f && c < 1e6f) || !(t > 0.f && t < 1e6f)) return false;
        if (cur) *cur = c;
        if (total) *total = t;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
uint8_t LastLiveReplayMode() { return g_lastLiveReplayMode; }

// The loader owns the MinHook trampoline for ASkaterCharacterBase's replay-mode handler; it registers
// it here so game-side code can invoke the game's OWN transition on a specific skater -- the full
// restore (GlobalAnimRateScale back to 1, component states, all of it), not a hand-rebuilt subset.
static void (*g_skaterReplayModeFn)(void*, uint8_t) = nullptr;
void SetSkaterReplayModeCaller(void (*fn)(void*, uint8_t)) { g_skaterReplayModeFn = fn; }
bool CallSkaterReplayMode(void* skater, uint8_t mode) {
    if (!g_skaterReplayModeFn || !skater) return false;
#ifdef _WIN32
    __try { g_skaterReplayModeFn(skater, mode); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

static const int kProxyRefs = 16;                       // the peer cap
struct ProxyRef { void* actor = nullptr; void* board = nullptr; };
static ProxyRef g_proxyRefs[kProxyRefs];

// NO EXPIRY -- see the header. An entry lives exactly as long as the proxy actor does.
void NoteProxyActor(void* actor, void* board) {
    if (!actor) return;
    ProxyRef* free_ = nullptr;
    for (auto& p : g_proxyRefs) {
        if (p.actor == actor) { p.board = board; return; }      // refresh the board link, idempotent
        if (!free_ && !p.actor) free_ = &p;
    }
    if (free_) { free_->actor = actor; free_->board = board; }
}
void DropProxyActor(void* actor) {
    if (!actor) return;
    for (auto& p : g_proxyRefs) if (p.actor == actor) { p.actor = nullptr; p.board = nullptr; }
}
bool IsProxyActor(void* a) {
    if (!a) return false;
    for (const auto& p : g_proxyRefs)
        if (p.actor && (p.actor == a || (p.board && p.board == a))) return true;
    return false;
}
// The SKATER an actor belongs to: itself, or the rider if `a` is a proxy's board. The canonical key
// for anything stored per peer -- a board and its rider are one player.
void* ProxyOwnerOf(void* a) {
    if (!a) return nullptr;
    for (const auto& p : g_proxyRefs)
        if (p.actor && (p.actor == a || (p.board && p.board == a))) return p.actor;
    return nullptr;
}
void* ProxyBoardOf(void* skater) {
    if (!skater) return nullptr;
    for (const auto& p : g_proxyRefs) if (p.actor == skater) return p.board;
    return nullptr;
}
int ProxyRefCount() { return kProxyRefs; }
bool ProxyRefAt(int i, void** actor, void** board) {
    if (i < 0 || i >= kProxyRefs) return false;
    const ProxyRef& p = g_proxyRefs[i];
    if (!p.actor) return false;
    if (actor) *actor = p.actor;
    if (board) *board = p.board;
    return true;
}

// =====================================================================================================
// CRANK DEF <-> INDEX. The FCrankDefinition the skater points at lives in the tricks database's
// _crankList (heap TArray inside a shared asset); its INDEX is the only identity that survives the
// wire. Resolution runs through the LOCAL controller on both ends.
// =====================================================================================================
static void* g_localController = nullptr;
void SetLocalController(void* pc) { g_localController = pc; }
// Is the game's pause menu on screen right now? The one liveness signal the menu code has -- without
// it, refreshing a page from the frame pump means touching a widget pointer that may be long gone.
bool PauseMenuOpen() {
    if (!g_syms.PauseMenuShown || !g_localController) return false;
    __try { return g_syms.PauseMenuShown(g_localController); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// =====================================================================================================
// WORLD -> SCREEN. Everything the floating player names need to know about the camera, asked of the
// engine rather than reconstructed: a hand-rolled projection would have to reproduce the game's FOV,
// aspect handling and any constrained view rect, and would be wrong the first time one of them changed.
// =====================================================================================================
bool ViewportSize(int* outW, int* outH) {
    if (!g_syms.GetViewportSize || !g_localController) return false;
    int x = 0, y = 0;
    __try { g_syms.GetViewportSize(g_localController, &x, &y); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (x <= 0 || y <= 0) return false;                 // the function's own "cannot know yet"
    if (outW) *outW = x;
    if (outH) *outH = y;
    return true;
}

bool ProjectWorldToViewport(const float world[3], float outPx[2], float* outDistCm) {
    if (!g_syms.ProjectToScreen || !g_localController || !world) return false;
    float xyd[3] = {0, 0, 0};
    bool ok = false;
    // viewportRelative = false: the answer wanted is a position in the viewport the game is actually
    // rendering, which is what the caller normalises against the viewport SIZE above.
    __try { ok = g_syms.ProjectToScreen(g_localController, world, xyd, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!ok) return false;                              // behind the camera, or no view to project into
    if (outPx) { outPx[0] = xyd[0]; outPx[1] = xyd[1]; }
    if (outDistCm) *outDistCm = xyd[2];
    return true;
}

bool ActorHeadPoint(void* actor, float headroomCm, float out[3]) {
    if (!actor || !out) return false;
    __try {
        void* root = *(void**)((uint8_t*)actor + off::kActorRootComp);
        if (!root) return false;
        memcpy(out, (uint8_t*)root + off::kCompPos, 12);
        float half = *(float*)((uint8_t*)root + off::kCapsuleHalfHeight);
        // A skater capsule is on the order of a metre. Anything else means the root is not a capsule
        // (or the read went somewhere it should not have), so fall back rather than trust it -- the
        // plate ends up in roughly the right place either way.
        if (!(half > 20.0f && half < 300.0f)) half = 90.0f;
        out[2] += half + headroomCm;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

#ifdef _WIN32
// data ptr + sane count of the local DB's _crankList, or false. One SEH frame around the whole walk.
static bool crankList(uint8_t** dataOut, int* numOut) {
    __try {
        if (!g_localController) return false;
        uint8_t* db = *(uint8_t**)((uint8_t*)g_localController + off::kPcTricksDb);
        if (!db) return false;
        uint8_t* data = *(uint8_t**)(db + off::kDbCrankList);
        const int num = *(int*)(db + off::kDbCrankList + 8);
        if (!data || num <= 0 || num > 256) return false;   // a handful of stances is the real shape
        *dataOut = data; *numOut = num;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int CrankIndexFromPtr(void* crankDef) {
    uint8_t* data; int num;
    if (!crankDef || !crankList(&data, &num)) return -1;
    const intptr_t d = (intptr_t)((uint8_t*)crankDef - data);
    if (d < 0 || d % off::kCrankDefSize != 0) return -1;
    const intptr_t idx = d / off::kCrankDefSize;
    return (idx < num) ? (int)idx : -1;
}

void* CrankPtrFromIndex(int idx) {
    uint8_t* data; int num;
    if (idx < 0 || !crankList(&data, &num) || idx >= num) return nullptr;
    return data + (intptr_t)idx * off::kCrankDefSize;
}

// ---- TYPE IDENTITY -----------------------------------------------------------------------------------
// The class chain walk. Depth is capped because the input is attacker-influenced: if a wrong-typed hit
// happens to make SuperStruct point into garbage, the walk must end rather than wander. UE class
// hierarchies here are a handful deep, so 16 is generous by an order of magnitude.
// MEMOISED BY CLASS POINTER, and it has to be: `fnameToAscii` LEAKS its FString by design, which is
// fine for a one-off but not for the audio path, which validates a cue every time a sound starts.
// A process contains a bounded number of classes, so caching pointer -> name makes the leak one FString
// per distinct UClass ever seen. A full cache stops caching rather than evicting: a wrong answer would
// be worse than a slow one.
static struct { void* cls; char name[96]; } g_classNames[96];
static int g_nClassNames = 0;
static const char* classNameCached(void* cls) {
    for (int i = 0; i < g_nClassNames; i++) if (g_classNames[i].cls == cls) return g_classNames[i].name;
    if (g_nClassNames >= (int)(sizeof(g_classNames)/sizeof(g_classNames[0]))) return nullptr;
    char name[96];
    if (!fnameToAscii((uint8_t*)cls + off::kObjNamePrivate, name, sizeof(name))) return nullptr;
    g_classNames[g_nClassNames].cls = cls;
    strncpy_s(g_classNames[g_nClassNames].name, name, _TRUNCATE);
    return g_classNames[g_nClassNames++].name;
}

bool IsObjectOfClass(void* obj, const char* className) {
    const Syms& S = Get();
    if (!obj || !className || !*className || !S.FNameToString) return false;
    __try {
        void* cls = *(void**)((uint8_t*)obj + off::kObjClassPrivate);
        // Depth is capped because the input is attacker-influenced: if a wrong-typed hit makes
        // SuperStruct point into garbage, the walk must END rather than wander. Real hierarchies here
        // are a handful deep.
        for (int depth = 0; cls && depth < 16; depth++) {
            const char* name = classNameCached(cls);
            if (name && !_stricmp(name, className)) return true;
            cls = *(void**)((uint8_t*)cls + off::kStructSuperStruct);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// data ptr + sane count of the local DB's _flipTricks, or false. Mirrors crankList exactly.
static bool flipTrickList(void*** dataOut, int* numOut) {
    __try {
        if (!g_localController) return false;
        uint8_t* db = *(uint8_t**)((uint8_t*)g_localController + off::kPcTricksDb);
        if (!db) return false;
        void** data = *(void***)(db + off::kDbFlipTricks);
        const int num = *(int*)(db + off::kDbFlipTricks + 8);
        if (!data || num <= 0 || num > 4096) return false;
        *dataOut = data; *numOut = num;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool FlipTrickListAvailable() { void** d; int n; return flipTrickList(&d, &n); }

// ---- the self-check registry. Tiny and fixed: one entry per expectation the mod holds.
//
// A SUCCESS is final: the expectation is proven and nothing later can un-prove it. A FAILURE is not,
// and must not be, because the sample can be meaningless through no fault of the expectation --
// `_currentGrindDef` and `_targetGrindDef` only hold a real definition WHILE GRINDING, and outside a
// grind they keep a stale pointer to a destroyed object. Reading its class gives garbage, and
// condemning the gate on that one read disabled grind-name checking for the entire session, every
// session. So a failure only counts once it has repeated: a wrong expectation fails every time and
// still gets caught, while one stale read costs nothing.
static struct GateState { const char* what; bool trusted; bool checked; int fails; } g_gates[] = {
    { "FlipTrickDefinition",    true, false, 0 },
    { "GrindOrSlideDefinition", true, false, 0 },
    { "SoundBase",              true, false, 0 },
};
static const int kGateFailsToCondemn = 4;
static GateState* gateFind(const char* what) {
    if (!what) return nullptr;
    for (auto& g : g_gates) if (!strcmp(g.what, what)) return &g;
    return nullptr;
}
bool GateTrusted(const char* what) {
    const GateState* g = gateFind(what);
    return g ? g->trusted : true;            // an unregistered gate is nobody's business to veto
}
void GateSelfCheck(const char* what, bool localPassed, void (*logf)(const char*)) {
    GateState* g = gateFind(what);
    if (!g || g->checked) return;            // already settled; this runs on a hot path
    if (localPassed) {                       // proven -- final, and say nothing
        g->checked = true;
        return;
    }
    if (++g->fails < kGateFailsToCondemn) return;   // one bad sample is not evidence
    g->checked = true;
    g->trusted = false;
    if (logf) {
        char m[320];
        snprintf(m, sizeof(m),
                 "[gate] *** '%s' REJECTED THE LOCAL PLAYER'S OWN OBJECT %d times -- the expectation is "
                 "wrong, not the peer. Gate DISABLED for this run (peer names of this kind go "
                 "unchecked). Fix the class name or the lookup.", what, g->fails);
        logf(m);
    }
}

bool IsKnownFlipTrickDef(void* obj) {
    void** data; int num;
    if (!obj || !flipTrickList(&data, &num)) return false;
    __try {
        for (int i = 0; i < num; i++) if (data[i] == obj) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int BoardMovementMode(void* board) {
    if (!board) return -1;
    __try {
        // The accessor pair ASkateboardEx::Tick's own audio gate uses, reproduced exactly:
        //   iface = board + 0x280 ; move = iface->vtbl[0x108]() ; mode = move->vtbl[0x1b0]()
        void*  iface = (uint8_t*)board + off::kBoardIface;
        void** ivt   = *(void***)iface;
        if (!ivt) return -1;
        using GetMoveFn = void* (*)(void*);
        void* mv = ((GetMoveFn)ivt[off::kIVtblGetBoardMove / 8])(iface);
        if (!mv) return -1;
        void** mvt = *(void***)mv;
        if (!mvt) return -1;
        using GetModeFn = uint8_t (*)(void*);
        return (int)((GetModeFn)mvt[off::kMVtblGetMoveMode / 8])(mv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Fallback: the derived offset off the movement component. Unknown (-1) if even that fails --
    // callers treat unknown as "not in hand", the harmless direction.
    __try {
        void* mv = *(void**)((uint8_t*)board + off::kBoardMoveComp);
        if (mv) return (int)*(uint8_t*)((uint8_t*)mv + off::kBoardMoveMode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
#else
int   CrankIndexFromPtr(void*) { return -1; }
void* CrankPtrFromIndex(int) { return nullptr; }
int   BoardMovementMode(void*) { return -1; }
#endif

}} // namespace omp::game
