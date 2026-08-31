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
// SessionOpenMP -- the game-function table.
// ONE place, with provenance, for every native symbol the overlay needs. Two hard rules:
//   * A sig must match EXACTLY ONCE in BOTH exes (Epic + Steam). `omp_symcheck` proves that from disk,
//     with no game running, and is part of the build -- a table entry that cannot be verified is a bug
//     report, not a symbol.
//   * VTABLE SLOTS beat sigs when two functions share a body shape (the linear/angular velocity setters
//     are byte-twins; a sig cannot tell them apart, a slot can). Slots come from the PDB.
// Deliberately absent: crash guards, role-gate patches, possession/camera/input heals and the whole
// listen-server apparatus. In overlay mode that code never runs.
#pragma once
#include <cstdint>

namespace omp { namespace game {

// ---- the functions the overlay CALLS (resolved by byte sig; provenance in game_syms.cpp) ------------
using SpawnActorFn      = void* (*)(void* world, void* cls, const void* loc, const void* rot, void* params);
using GetWorldFn        = void* (*)(void* actor);
using GetGameInstFn     = void* (*)(void* actor);   // AActor::GetGameInstance -- cosmetics live there
// ACharacterCustomization::GetCustomizationItem(const FName&) -- the game's OWN name -> item
// definition lookup: it scans the global item catalog and returns NULL for an item this install does
// not have. Nothing downstream checks that null, so writing a peer's item we lack would send the
// rebuild into `MapItemMaterials` with a null definition and it would dereference it (AV at
// MapItemMaterials+0x60, reading a TArray at +0xd8). The vanilla menu can only offer items you own,
// so the null path is unreachable in the real game and unguarded. Every peer item must be checked here.
using GetCustomItemFn   = void* (*)(const void* fname);
// TMapBase<int32,FCustomizationProfileItem>::Emplace -- overwrites the value at `key` or INSERTS a
// new element (allocation, hashing and the free list are all its own). The sanctioned way to give a
// borrowed profile a category slot it does not have: a PRO skater's clothing map is EMPTY, so a pro
// user's client could not dress anyone (0 slots to overwrite = every peer in underwear). Reached by
// DECODING a call site (see the table): the body has 7+ byte-twin template instantiations.
using ProfileEmplaceFn  = void  (*)(void* map, const int32_t* key, const void* value16);
// FSoftObjectPath::TryLoad(FUObjectSerializeContext* = null) -- LOADS the asset and returns it.
// The customization rebuild only ever calls FSoftObjectPath::ResolveObject, which returns null for
// anything not already in memory: the local player's garments were streamed in when their own profile
// was applied, a PEER's never were, and a null mesh reaches MapItemMaterials, which walks
// USkeletalMesh::Materials at +0xd8 unguarded. So a peer's item meshes are loaded here before the game
// is asked to rebuild, and the return value doubles as the proof: an asset that will not load is
// simply never worn.
using TryLoadFn         = void* (*)(void* softObjectPath, void* loadContext);
using SetHiddenFn       = void  (*)(void* actor, bool hidden);
// AActor::SetActorEnableCollision(bool). Hiding an actor is PURELY VISUAL -- its components keep
// colliding, which is why a retired proxy left an invisible obstacle behind.
using SetCollisionFn    = void  (*)(void* actor, bool enable);
using SetActorTickFn    = void  (*)(void* actor, bool enabled);
using SetReplicatesFn   = void  (*)(void* actor, bool replicates);
using SetActorLocRotFn  = void  (*)(void* actor, const float* loc, const float* quat4, bool sweep,
                                    void* hitResult, unsigned char teleportType);
using RefreshVisualsFn  = void  (*)(void* skater);
using BoardTeleportFn   = void  (*)(void* board, const float* loc, const float* rot3);
using BoardSetRotFn     = void  (*)(void* board, const float* rot3);
using SetSimPhysFn      = void  (*)(void* actor, bool simulate, bool includeChildren);
using CompSetSimPhysFn  = void  (*)(void* comp, bool simulate);
using BoardSetLinVelFn  = void  (*)(void* board, const float* vel, bool addToCurrent, uint64_t boneName);
// ASkaterCharacterBase::Bail, the SMALL overload: (this, const FString* reason, bool, bool). Gates on
// _canBail (+0x649) itself, synthesizes the bail location (vcall / socket / board-movement fallback --
// the fallback derefs skater+0x568, so only call this with the board link up), then forwards to the
// big 5-arg overload. In-game callers pass (&printfString, 1, 1).
using BailFn            = void  (*)(void* skater, void* reasonFString, bool a, bool bRagdoll);
using ResetRagDollFn    = void  (*)(void* skater);   // safe when not ragdolling (gates on +0x710 bit 2)
// ASkateboardEx::BreakBoard_Internal: the pure visual/physics half of a board break -- swaps in the
// broken meshes and stores `state` into _currentBrokenBoardState (+0x3f1). Called DIRECTLY on a proxy
// board (the public BreakBoard is the wrong entry there: its CanBreakBoard gate lets the VIEWER's own
// gameplay setting veto a peer's transported break, and it also plays the crack sound -- which already
// arrives via the audio funnel -- and writes the local save). Self-guards world membership; clears
// _breakBoardRequested (+0x783) at entry, so the proxy's veto hold re-asserts next Apply. Its embedded
// Bail call is suppressed on proxies by the _canBail hold; the real bail is transported. The game
// schedules it as (state, 0).
using BreakBoardIntFn   = void  (*)(void* board, uint8_t state, uint8_t flag);
// ASkateboardEx::RebuildBrokenBoard: the un-break -- restores the intact meshes and resets +0x3f1 to
// 0. Gates on actually-broken itself. (1, 0) mirrors the player-facing repair input action's args.
using RebuildBoardFn    = void  (*)(void* board, uint8_t a, uint8_t b);
// USkaterAnimInstance::SaveFootToBoardTransition: saves the CURRENT pose into the transition buffer
// (anim+0x5c0 FTransform array) the graph blends FROM during the on/off-board switch. The game calls
// it from ApplyToggleOnBoard/DoBoardPickup -- input-driven gameplay code that never runs on a proxy,
// so without calling it here a proxy's transition blends from an EMPTY buffer and T-POSES at every
// mount. Args: boardToFoot = direction (0 = mount, 1 = pickup -- ApplyToggleOnBoard passes its live
// bOnBoard); flag = the ([skater+0xa38] == 2 || == 4) predicate, mirrored verbatim.
using SaveFootTransFn   = void  (*)(void* animInst, uint8_t boardToFoot, uint8_t flag);
using SetWorldRotQFn    = void  (*)(void* comp, const float* quat4, bool sweep, void* hit, unsigned char tp);
using IsLocallyCtrlFn   = bool  (*)(void* pawn);
using SFOFn             = void* (*)(void* cls, void* outer, const wchar_t* name, int exactClass);
using SetPushStateFn    = void  (*)(void* skater, uint8_t state);
using BrakeRpcFn        = void  (*)(void* skater, uint8_t type);
using FNameToStringFn   = void  (*)(const void* fname, void* fstringOut);   // FName::ToString(FString&)
// ---- THE AUDIO FUNNEL ------------------------------------------------------------------------------
// Session plays its skate audio through `UReplayAudioManager` -- the seam its OWN replay recorder
// captures sound through (four static TMulticastDelegates sit beside these, bound by
// UAudioReplayComponent when a recording starts). A proxy IS a replay -- `ASkateboardEx::Tick` even
// mutes its whole audio block at movement mode 9 and lets the replay path drive the sound instead --
// so capture happens at the same seam and is re-issued on the peer.
// The two spawn wrappers take the ENGINE's own UGameplayStatics argument list ARG FOR ARG, read out of
// their bodies: no `this`, no extra leading arg, no missing Rotation. FVector/FRotator are 12 bytes, so
// MSVC passes them by HIDDEN POINTER; declaring them `const float*` is the identical ABI and spares a
// struct definition. FName rides by value in 8 bytes. The floats really ARE floats here (xmm5-xmm8
// loads off the stack slots), so they are declared `float` -- tweaks_common's "declare unknown float
// args as double and forward unconverted" rule applies to GUESSED types only, not to one read off the
// code.
using SpawnSndAtLocFn   = void* (*)(void* worldCtx, void* sound, const float* loc, const float* rot,
                                    float vol, float pitch, float start, void* atten, void* conc,
                                    bool autoDestroy);
using SpawnSndAttFn     = void* (*)(void* sound, void* attachTo, uint64_t attachPoint, const float* loc,
                                    const float* rot, int locType, bool stopWhenDetached, float vol,
                                    float pitch, float start, void* atten, void* conc, bool autoDestroy);
// UAudioComponent's own parameter setters, hooked at the ENGINE layer rather than at the funnel's
// wrappers for two reasons: the funnel's SetPitch/SetVolume wrappers are byte-identical bodies no
// signature can tell apart, and `AdjustGoingFastAudio` calls the engine setters DIRECTLY, bypassing
// the funnel entirely. One layer covers both cases.
using AcSetIntFn        = void  (*)(void* audioComp, uint64_t fname, int32_t value);
using AcSetFloatFn      = void  (*)(void* audioComp, float value);      // pitch / volume multiplier
// FName::FName(const ANSICHAR*, EFindName) -- the ANSI overload the funnel itself uses to build
// FName("isReplayed"). Needed to replay a captured sound's int PARAMETERS: those carry the surface
// type ("UpdateAudioWithSurfaceType"), the powerslide fullness and the wheel-lock flag, i.e. what
// makes a grind sound like concrete instead of generic. Returns the out pointer; FindType 1 = Add.
using FNameCtorFn       = void* (*)(void* outFName, const char* ansi, int findType);
// USoundBase::GetConcurrencyHandles(this, TArray<FConcurrencyHandle>& out). The game's own resolver:
// it returns the ConcurrencyOverrides struct when bOverrideConcurrency (+0x38 bit 1) is set, every
// entry of ConcurrencySet (+0x90) otherwise, and the project default when the cue has neither -- so
// calling it beats walking a TSet layout by hand and cannot miss a case.
//   FConcurrencyHandle (16 B) = { FSoundConcurrencySettings* Settings; uint32 ObjectID; bool bIsOverride; }
//   FSoundConcurrencySettings::MaxCount is at +0x00 of *Settings.
using GetConcHandlesFn  = void  (*)(void* soundBase, void* outTArray);
// AReplayCamera::SetCameraType(this, EReplayCameraType). 1 = RCT_Orbit. Recomputes the orbit distance
// and rotation from _lookAtTarget, and broadcasts _onCameraTypeChanged so the game's own editor UI and
// lens logic update themselves. Early-outs if the type is already the active one.
using SetCamTypeFn      = void  (*)(void* replayCamera, uint8_t type);
using GetReplayMgrFn    = void* (*)();      // ASessionReplayManager::GetInstance()
// ---- THE GAME'S OWN PAUSE MENU ---------------------------------------------------------------------
// Session's menus are NOT blueprint graphs: a page is a `UMenuPageDefinition` data asset holding a
// `TArray<FMenuPageItemDefinition>`, and `UMenuPage` turns that array into one `UMenuPageItem` widget
// per row. Measured order inside `UMenuPage::SetActivePageDefinition`: copy the asset's items into the
// page's OWN working array (+0x2e0) -> RemoveAll<lambda> filters them (platform / editor-only /
// visibility) -> scrollbar -> CreatePageItems. So a POST-hook on SetActivePageDefinition is too late,
// and the injection point is CreatePageItems.
// UMenuPage::CreatePageItems(this, TArray<FMenuPageItemDefinition>* items, bool) -- takes the array as
// an ARGUMENT and only READS it: each element is copy-constructed onto the stack (the three FTexts get
// a `lock inc` refcount bump) and handed to CreatePageItem, which builds the widget and stores its own
// copy at widget+0x2c0. Substitute the POINTER; never grow the page's own array. There is then zero
// ownership transfer, so nothing of ours is ever destructed or freed by the engine. Growing +0x2e0
// would hand our buffer to the destruct-then-ResizeTo(0) loop at the next page activation, i.e.
// FMemory::Free on static storage.
using MenuCreateItemsFn = void  (*)(void* menuPage, void* itemsArray, bool flag);
// UMenuPage::OnSelectionConfirmed(this, FOnMenuPageSelectionConfirmedParams*) -- the PAGE-level funnel
// every confirm passes through (widget -> page -> container). It stamps params->PageDefinition with
// the page's active definition and broadcasts; the container's HandlePageSelectionConfirmed then looks
// up a per-page FMenuPageEventHandler and finally calls the container's own virtual (vtbl+0x4c8, e.g.
// UPauseMenuPageContainer::OnPageSelectionConfirmed and its inline FName compare chain). Hooking the
// page-level one covers EVERY container with a single hook, and not calling the trampoline suppresses
// the whole stock chain for a row the overlay owns.
using MenuSelConfirmFn  = void  (*)(void* menuPage, void* params);
// UMenuPage::RefreshItemsPanel(this) -- the game's own "rebuild the rows" (it is what the scroll path
// calls): clears _pageItemWidgets and re-runs CreatePageItems on +0x2e0. The CreatePageItems hook
// substitutes the array, so this is how the pause page's rows are swapped for the sub-page and back.
using MenuRefreshFn     = void  (*)(void* menuPage);
using MenuSetTitleFn    = void  (*)(void* menuPage, const void* ftext);   // UMenuPage::SetTitle
// UMenuPage::SetSelectedIndex(this, int32 index, bool) -- REQUIRED after swapping a page's rows.
// It DESELECTS the old index first (`_pageItemWidgets.data[_selectedIndex]`) with only a "is it
// negative" test, so a stale index left over from a longer page reads out of bounds. Write
// `_selectedIndex = -1` first (that branch is `js`, i.e. skipped) and then call this with 0.
using MenuSetSelIdxFn   = void  (*)(void* menuPage, int32_t index, bool flag);
// UMenuPageItem::ProgressBarSetPercent(this, float). NORMALISED 0..1, clamped in the body -- the
// NUMBER the row displays comes from the definition's _progressDisplayValueMinimum/Maximum, so a
// caller maps value->fraction going in and fraction->value coming back out of the change event.
using MenuProgSetPctFn  = void  (*)(void* menuPageItem, float pct01);
// UMenuPageItem::MultiOptionSetSelectedItemIndex(this, int32) -- 2 args. Bounds checks against the
// widget's own option count, then broadcasts the change like a user input would.
using MenuMultiSetIdxFn = void  (*)(void* menuPageItem, int32_t index);
using PauseShownFn      = bool  (*)(void* playerController);   // IsPauseMenuDisplayed
// USessionGameInstance::GetGameVersion(this, FString* out) -> out. Builds the "0.6.42 (48691)" line
// from GGameIni. HOOKED to append the mod's own version -- but only for the two callers that DRAW it
// (see version_tag.h: the third writes the player's profile).
using GameVersionFn     = void* (*)(void* gameInstance, void* outFString);
// The engine allocator, for building the appended string with the game's own memory.
using MemMallocFn       = void* (*)(size_t size, uint32_t alignment);   // alignment 0 = default
using MemFreeFn         = void  (*)(void* p);
// FText FText::FromName(const FName&) -- Epic 0x126d3f0. `rcx` = the 24-byte out FText (MSVC hidden
// return pointer), `rdx` = the FName. Internally it is just FName::ToString + FromString, so a label
// costs one FNameCtor + this: no FString of ours, no allocator, and NO OWNERSHIP QUESTION -- the arg
// is a POD 8-byte FName, unlike every FromString/AsCultureInvariant overload where the const-ref and
// rvalue-ref twins differ only in whether the callee steals the buffer.
// It cannot be sig'd DIRECTLY: `FPackageName::GetShortName(const FName&)` is byte-for-byte identical
// (both are "ToString the FName, forward the FString, free it"), so any length gives 2 hits in both
// exes. A unique CALL SITE is sig'd instead and the target decoded from its `E8 rel32` -- reading the
// displacement rather than wildcarding it, the same identity rule pointed the other way.
using TextFromNameFn    = void* (*)(void* outFText, const void* fname);
// ---- WORLD -> SCREEN, for the floating player names (nameplates.h) ---------------------------------
// APlayerController::ProjectWorldLocationToScreenWithDistance(this, const FVector* world,
//   FVector* outScreenXY_and_Distance, bool bViewportRelative) -> bool.
// The `WithDistance` overload rather than the plain one for two reasons: it hands back the distance to
// the point for free (which is what sizes and range-limits a plate), and the plain one is a 14-byte
// forwarding stub -- exactly the shape MSVC folds with any other identical stub, so the name at that
// address is not evidence of what lives there. This one is a real 0x1bc-byte body.
// It cannot be called at a bad moment: it returns false on a null ULocalPlayer, a null ViewportClient,
// a failed GetProjectionData, and a point behind the camera (FSceneView::ProjectWorldToScreen's W<=0).
using ProjectToScreenFn = bool  (*)(void* playerController, const float* world, float* outXYDist,
                                    bool viewportRelative);
// APlayerController::GetViewportSize(this, int32* x, int32* y). Zeroes both outs first and leaves them
// at zero on any failure, so 0 IS the "unknown" answer and needs no separate return value.
// Needed because the projection above answers in the GAME's viewport pixels, which are not the window's
// client pixels whenever a resolution scale is set -- so the screen position has to be normalised here
// and un-normalised against whatever the render thread's display size turns out to be.
using ViewportSizeFn    = void  (*)(void* playerController, int* outX, int* outY);
// ---- THE OBJECT DROPPER ----------------------------------------------------------------------------
// UObjectDropperObjectsDatabase::GetObjectInformationByID(this, FName id) -> FObjectDropperObjectInformation*
// The game's OWN id -> object lookup: it walks every category, takes each entry's class soft path,
// runs FSoftObjectPath::GetAssetName() on it and compares the resulting FName -- so the id it matches
// is the CLASS name ("BP_Whatever_C"), and a null return is exactly "this install does not have that
// object" (a DLC the peer owns and we do not). FName rides by value in rdx as its 8 raw bytes.
using ObjInfoByIdFn     = void* (*)(void* database, uint64_t objectId);
// AObjectDropperManager::GetObjectDropperObjectComponent(AActor*) -> UObjectDropperPickableObject*
// Despite the class qualifier it takes the ACTOR as `this` and is simply
// GetComponentByClass(UObjectDropperPickableObject::StaticClass()) with a re-check of the result --
// so it doubles as "is this actor a dropped object at all?". Needed to reach _isCurrentlyPickable on
// the objects we spawn for peers; see kPickableIsPickable.
using PickableOfFn      = void* (*)(void* actor);
// AActor::Destroy(bNetForce, bShouldModifyLevel) -> bool. Unlike skater proxies -- which are forgotten
// rather than destroyed because destroying one mid-session crashes the client -- dropped objects are
// plain props with no components anyone else drives, and they ACCUMULATE: every peer's set arrives on
// every map change and at every rejoin, so leaving them hidden would pile up statues for the session.
using ActorDestroyFn    = bool  (*)(void* actor, bool netForce, bool shouldModifyLevel);
// UObjectDropperPersistentHandler::Load -- HOOKED, never called. It is the one moment at which the
// level's own props are still where the MAP put them: Load walks the player's save and moves each
// prop it names to the saved pose, so anything captured before it runs is the map default and
// anything captured after is that player's own arrangement. Nothing else at runtime can tell the two
// apart, which is why "put this bench back" and "show me the host's layout instead of mine" both
// depend on this seam.
// AActor::SetActorLocation(this, const FVector*, bool bSweep, FHitResult*, ETeleportType) -- HOOKED,
// never called, and only ACTIVE while inside Load: it is how Load moves each prop, so intercepting
// it there captures the pre-move pose of exactly the props that need a default, and no others.

struct Syms {
    SpawnActorFn     SpawnActor        = nullptr;
    GetWorldFn       GetWorld          = nullptr;
    GetGameInstFn    GetGameInstance   = nullptr;
    GetCustomItemFn  GetCustomizationItem = nullptr;
    TryLoadFn        SoftPathTryLoad   = nullptr;
    ProfileEmplaceFn ProfileEmplace    = nullptr;   // decoded from ProfileEmplaceSite, see the bind block
    SetHiddenFn      SetActorHidden    = nullptr;
    SetCollisionFn   SetActorCollision = nullptr;
    SetActorTickFn   SetActorTick      = nullptr;
    SetReplicatesFn  SetReplicates     = nullptr;
    SetActorLocRotFn SetActorLocRot    = nullptr;
    RefreshVisualsFn RefreshVisuals    = nullptr;
    BoardTeleportFn  BoardTeleport     = nullptr;
    BoardSetRotFn    BoardSetRot       = nullptr;
    SetSimPhysFn     SetSimulatePhysics= nullptr;
    CompSetSimPhysFn CompSetSimPhys    = nullptr;
    BoardSetLinVelFn BoardSetLinVel    = nullptr;
    // The bail pair. Bail is CALLED on the proxy when the owner's transported ragdoll bit rises --
    // each client decides its own bail, every machine executes it locally -- and ResetRagDoll on the
    // falling edge is the recovery half, without which a bailed copy stays down forever.
    // Deliberately absent: a "SetOnBoardMode"/"SetOnFootMode" mode-transition pair. The transition is
    // not needed -- nothing in the overlay reads the proxy's bOnBoard, and the carried board is
    // TRANSPORTED pose (the sender's own PlaceInHand output), not locally reproduced.
    BailFn           Bail              = nullptr;
    ResetRagDollFn   ResetRagDoll      = nullptr;
    // The board-break pair, the bail pair's shape exactly: the owner's transported broken-state byte
    // rises -> BreakBoardInternal on the proxy's board; falls -> RebuildBrokenBoard.
    BreakBoardIntFn  BreakBoardInternal = nullptr;
    RebuildBoardFn   RebuildBrokenBoard = nullptr;
    SaveFootTransFn  SaveFootTrans     = nullptr;   // see the typedef's comment
    SetWorldRotQFn   SetWorldRotQuat   = nullptr;
    IsLocallyCtrlFn  IsLocallyControlled = nullptr;
    // Not called -- HOOKED (MinHook, loader). ASkaterCharacter::PopulateMarkerInfo reads FollowCamera
    // (skater+0xa50) with no null check; that component is null on proxies (the replay-guard
    // finding), and break sync opened a path there for proxies: PostInitCharacter's deferred
    // streamable completion populates the marker only WHEN THE BOARD IS BROKEN (it calls IsBroken
    // and skips otherwise) -- a state only load-ins with a saved broken board reached before, and
    // one every broken proxy board now sits in. The guard hands back IsSet=0 instead.
    void*            PopulateMarkerInfo = nullptr;
    // Not called -- PATCHED. `AGameModeBase::AllowPausing` is the single gate every pause request goes
    // through, so the table keeps its address rather than a callable type. See DisablePause().
    void*            AllowPausing      = nullptr;
    // Not called -- HOOKED (MinHook, by the loader). UGameEngine::Tick is the per-frame game-thread
    // anchor, and the only safe one for work that loads assets: the ProcessEvent pre-callback is
    // inside script dispatch, where SpawnActor faults in the LINKER (half-built skaters, a poisoned
    // loader, and a delayed FLinkerLoad::VerifyImportInner crash reading float bits as a pointer).
    void*            EngineTick        = nullptr;
    // The RENAME GUARD. The game assumes ONE skater per world: it renames a freshly spawned skater to
    // the fixed name "Skater" (and its board to "Skateboard"). With a proxy in the world that rename
    // COLLIDES with the local player's object, and a name collision is a LowLevelFatalError which SEH
    // must NEVER catch -- Rename rewrites the UObject hash tables, so swallowing the fatal partway
    // leaves the object system corrupt and the process dies later somewhere unrelated (observed in GC's
    // ConditionalBeginDestroy). The loader therefore HOOKS RenameObj and PRE-EMPTS: if StaticFindObject
    // says the name is taken in that outer, skip the rename and report success. The proxy keeps its
    // unique auto-generated name, which is entirely valid; the rename was cosmetic tidying.
    // ASkaterCharacterBase::SetPushState -- the push STATE MACHINE entry. Stores the state byte and
    // drives the montage machinery. Called DIRECTLY on the proxy when the transported state changes:
    // the request-BIT alternative cannot work, because the bit pulses within one sender frame
    // (unpollable) and its consumer runs on the actor tick the proxy has disabled.
    SetPushStateFn   SetPushState      = nullptr;
    // The brake pair: braking is MULTICAST-RPC-replicated in the stock game, so an overlay proxy never
    // gets it for free. These are the _Implementation bodies observers run. Both open with a Role==1
    // check (written for replicated proxies; ours is Role 3), so the receiver ALSO writes the state
    // bytes (+0x612/+0x613) directly -- if the role gate skips part of the body, the anim graph still
    // sees the state.
    BrakeRpcFn       StartBraking      = nullptr;   // MulticastRPCStartBraking_Implementation
    BrakeRpcFn       StopBraking       = nullptr;   // MulticastRPCStopBraking_Implementation
    // FName::ToString -- the trick-def transport needs the def's NAME as a string: FName indices are
    // per-process (load-order dependent), so only the string travels. The receiver resolves it with
    // StaticFindObject(ANY_PACKAGE) and writes skater+0x590; the proxy's own anim update then populates
    // the trick assets and the T-pose gates PASS by construction (the crouch/"crank" and all trick
    // ratios are gated on those assets -- this one transport un-gates them all).
    FNameToStringFn  FNameToString     = nullptr;
    // ---- the audio funnel. All optional: a missing one costs its own sounds, never correctness --
    // audio.cpp enumerates what it has and announces what it is missing, once.
    SpawnSndAtLocFn  SndSpawnAtLoc     = nullptr;   // UReplayAudioManager::SpawnSoundAtLocation
    SpawnSndAttFn    SndSpawnAttached  = nullptr;   // UReplayAudioManager::SpawnSoundAttached
    // The ENGINE layer underneath. Hooked only to catch what bypasses the funnel (measured: exactly
    // ASkateboardEx::StartGoingFastLoopAudio, the wind loop), guarded by an in-funnel TLS flag so a
    // funnel-routed sound is never counted twice. Also the playback primitive's fallback.
    SpawnSndAtLocFn  GsSpawnAtLoc      = nullptr;   // UGameplayStatics::SpawnSoundAtLocation
    SpawnSndAttFn    GsSpawnAttached   = nullptr;   // UGameplayStatics::SpawnSoundAttached
    AcSetIntFn       AcSetIntParam     = nullptr;   // UAudioComponent::SetIntParameter
    AcSetFloatFn     AcSetPitch        = nullptr;   // UAudioComponent::SetPitchMultiplier
    AcSetFloatFn     AcSetVolume       = nullptr;   // UAudioComponent::SetVolumeMultiplier
    // RANGE MARKER, never called: UAnimNotify_PlaySoundRecorded::PlaySound. A captured sound whose
    // RETURN ADDRESS lands inside this function came from an anim notify; that classification drives
    // audio's transportAnimNotify / suppressProxyNotify decisions, so exactly one copy of a notify
    // sound is heard. Both UAnimNotify_PlayCatchSound and UAnimNotify_PlaySurfaceTypeSound route
    // through this one function, and the tail-`jmp` the catch notify enters by does not disturb the
    // return address the funnel sees.
    void*            NotifyPlaySound   = nullptr;
    FNameCtorFn      FNameCtor         = nullptr;   // FName::FName(const ANSICHAR*, EFindName)
    // ---- the rolling-sound concurrency fix + the replay-camera look-at target.
    GetConcHandlesFn GetConcHandles    = nullptr;
    GetReplayMgrFn   ReplayMgrInstance = nullptr;
    SetCamTypeFn     ReplayCamSetType  = nullptr;
    // HOOKED, never called: ASkaterCharacter::OnReplayModeChanged -- see the sig's comment.
    void*            SkaterReplayMode  = nullptr;
    // HOOKED, never called: UCameraReplayComponent::Replaying -- an out-of-range READ in the GAME's
    // own code, which multiplayer merely provokes. AReplayManager::TickReplaying computes
    // ONE keyframe index pair from the replay timeline and hands it to EVERY registered component
    // (`mov r9d,r15d / mov r8d,r14d / mov edx,ebp` are all hoisted OUT of the component loop), while
    // the callee's only guard is `cmp [this+0x50], 2` -- it checks the array has two entries, never
    // that the INDEX is inside it. A component that missed frames while recording therefore gets
    // indexed past its own data: `movss xmm13,[r10 + idx*20]` off the end. Uneven frame pacing is all
    // it takes, which is why two game instances on one PC hit it and a solo session does not.
    // The hook clamps the indices to the component's own array. Worst case the camera holds its last
    // keyframe for a frame; the alternative is reading garbage.
    void*            CamReplaying      = nullptr;
    void*            FloatTrackReplaying = nullptr;
    // HOOKED, never called: USkeletalMeshComponent::FinalizeBoneTransform -- THE POSE SEAM. Pre-hook,
    // because its first act is the component-space buffer flip: that is the last moment a finished
    // pose can still be replaced before it is published. See pose.h.
    void*            MeshFinalizeBones = nullptr;
    // ---- the pause menu. All optional: a missing one costs the in-game menu, never correctness --
    // the F1 overlay is a separate TU with separate hooks and keeps working either way.
    // pause_menu.cpp enumerates these by name and announces what it is missing.
    void*            MenuCreateItems   = nullptr;   // UMenuPage::CreatePageItems -- HOOKED (inject rows)
    void*            MenuSelConfirmed  = nullptr;   // UMenuPage::OnSelectionConfirmed -- HOOKED (intercept)
    void*            MenuBackAction    = nullptr;   // UMenuPageContainer::HandlePageBackAction -- HOOKED
    MenuRefreshFn    MenuRefreshItems  = nullptr;   // UMenuPage::RefreshItemsPanel -- called (rebuild)
    MenuSetTitleFn   MenuSetTitle      = nullptr;   // UMenuPage::SetTitle -- called (sub-page heading)
    MenuSetSelIdxFn  MenuSetSelIndex   = nullptr;   // UMenuPage::SetSelectedIndex -- called after a swap
    void*            MenuMultiChanged  = nullptr;   // UMenuPage::OnMultiOptionItemSelectionChanged -- HOOKED
    void*            MenuProgressChanged = nullptr; // UMenuPage::OnProgressBarValueChanged -- HOOKED
    MenuProgSetPctFn MenuProgressSetPct = nullptr;  // UMenuPageItem::ProgressBarSetPercent -- called
    MenuMultiSetIdxFn MenuMultiSetIndex = nullptr;  // UMenuPageItem::MultiOptionSetSelectedItemIndex
    PauseShownFn     PauseMenuShown    = nullptr;   // ASessionPlayerController::IsPauseMenuDisplayed
    // ---- the version tag. All optional: missing = the game's line is untouched.
    void*            GameVersion       = nullptr;   // HOOKED
    MemMallocFn      MemMalloc         = nullptr;
    MemFreeFn        MemFree           = nullptr;
    // RANGE MARKERS, never called: the two functions that DISPLAY the version. A GetGameVersion whose
    // return address lands inside one of these is drawing it; anything else (the profile's news save
    // data) must get the string untouched.
    void*            IntroUiRange      = nullptr;   // ASessionPlayerController::CreateIntroUI
    void*            PauseInitRange    = nullptr;   // UPauseMenuPageContainer::NativeOnInitialized
    TextFromNameFn   TextFromName      = nullptr;   // decoded from MenuTextSite's trailing E8, not sig'd
    SFOFn            StaticFindObject  = nullptr;
    void*            RenameObj         = nullptr;   // UObject::Rename -- hooked, never called directly
    void*            AnimUpdate        = nullptr;   // USkaterAnimInstance::NativeUpdateAnimation --
                                                    // POST-hooked; the pose blob applies there
    // ---- the floating player names. Optional: missing = no nameplates, nothing else changes.
    ProjectToScreenFn ProjectToScreen  = nullptr;
    ViewportSizeFn    GetViewportSize  = nullptr;
    // ---- the object dropper. All optional: missing = dropped-object sync announces itself off.
    // AObjectDropperManager is a SINGLETON and `_instance` is a static global, so the address is
    // DECODED from the RIP displacement of the two tiny accessors that read it (see the bind block) --
    // and both must decode to the same address before either is believed.
    void**            DropperInstance  = nullptr;   // &AObjectDropperManager::_instance
    ObjInfoByIdFn     DropperObjInfoById = nullptr;
    PickableOfFn      DropperPickableOf = nullptr;
    ActorDestroyFn    ActorDestroy     = nullptr;
    void*             DropperLoad      = nullptr;   // HOOKED, never called directly
    // UObjectDropperPersistentHandler::Save -- HOOKED, never called. THE HARD SAVE GUARD. A prop we
    // spawn for a session is a real dropped object as far as the game is concerned, and the session
    // ones are deliberately PICKABLE so anyone can move them -- which means one can be selected, land
    // in `_allObjects` and be written into the player's own profile. The per-enumeration purge is a
    // race against exactly that; this is the guarantee. Nothing else in this feature can damage
    // something the player cannot get back.
    void*             DropperSave      = nullptr;   // HOOKED, never called directly
    void*             ActorSetLocation = nullptr;   // HOOKED, never called directly
    int  resolved = 0, total = 0;
};

// ==== NEVER FREEZE THE WORLD ======================================================================
// Overlay multiplayer makes pausing worse than useless: your pause freezes YOUR world, which stops
// your own skater AND every proxy in it -- so you cannot even watch your friends skate while the menu
// is open, and they see you stand still. Refusing the freeze costs nothing and fixes both.
// `AGameModeBase::SetPause` calls AllowPausing through vtbl+0x6e8 and bails on false, so patching that
// 24-byte function's head to `xor al,al ; ret` makes every pause request fail -- ONE patch, no hook, no
// arity question, and it covers all seven callers for free. THE PAUSE MENU STILL OPENS; only the
// world-freezing part is refused.
bool DisablePause(void (*logf)(const char*));

// ==== LIVE PROXY REGISTRY ============================================================================
// "Is this actor one of ours?" -- needed by process-wide HOOKS, which see every actor in the game and
// have no other way to tell a proxy from the local player. A dangling actor pointer here would
// mis-tag a REAL skater, so entries expire rather than being removed: a proxy that is released,
// forgotten, or dies with its world simply stops refreshing. Refreshed every frame from
// Proxy::AudioApply.
// The LOCAL player's replay-editor mode, recorded by the replay guard hook as it passes the broadcast
// through for our own skater. Free to capture from that call, and it is the gate for the replay-mode
// diagnostics -- there is no other cheap way to ask "am I scrubbing?".
void    SetLocalReplayMode(uint8_t mode);
uint8_t LocalReplayMode();
// The replay playhead in seconds (CurrentPlayTime / TotalPlayTime off the live manager's active
// instance). False when the manager or instance is not up. Timeline END (cur == total) is the
// moment playback was entered; the session maps that to peers' transferred history windows.
bool ReplayPlayTime(float* cur, float* total);
uint8_t LastLiveReplayMode();               // the most recent non-playback mode (default 1)
// The game's own per-skater replay-mode transition, via the loader's trampoline. Used to RESTORE a
// proxy the playback machinery touched -- its mesh's GlobalAnimRateScale is zeroed while its
// components are playback-driven, and only this call puts the whole state back.
void SetSkaterReplayModeCaller(void (*fn)(void*, uint8_t));
bool CallSkaterReplayMode(void* skater, uint8_t mode);

// These entries have an ACTOR's lifetime, NOT a packet's: a registry that answers "is this actor one
// of ours?" must be keyed to the actor's LIFETIME. Register at spawn, drop at Forget -- both
// deterministic, neither dependent on the network. Expiring entries on stream activity instead loses
// a peer whose stream merely went QUIET (OnQuiet skips Apply) while their proxy is still standing in
// the world; the replay guard then fails open, the proxy runs OnReplayModeChanged, and it attaches a
// null (AV at OnReplayModeChanged+0x5e -> AttachToComponent).
void NoteProxyActor(void* actor, void* board);  // spawn + per-frame refresh (idempotent)
void DropProxyActor(void* actor);               // Forget: the actor is gone or no longer ours
bool IsProxyActor(void* actorOrBoard);          // true for the proxy skater OR its board
void* ProxyOwnerOf(void* actorOrBoard);         // ...and WHICH skater that is (null = not ours)
void* ProxyBoardOf(void* skaterActor);          // the skater's board, or null
int  ProxyRefCount();                           // iteration, for hooks that need proxy POSITIONS
bool ProxyRefAt(int i, void** actor, void** board);

// ---- who and where the local player is, for the lobby advertisement. Both read through symbols the
// table already owns and both are best-effort: a blank string just means that column is empty in the
// browser. The skater NAME is the one human-meaningful identity available -- an EOS product user id
// is an opaque GUID, and the display name would need Epic account scope the mod does not ask for.
// Any UObject's own name (UObjectBase::NamePrivate). ONE-SHOT USES ONLY -- FName::ToString allocates
// an FString that is deliberately leaked, so a per-frame caller corrupts the heap. Enumerating fonts
// once at startup is fine.
bool ObjectName(const void* obj, char* out, int cap);
// A skater's mesh component, and the bone count of the skeleton it is drawing.
void* SkaterMeshOf(void* skaterActor);
int   SkeletonBoneCount(void* meshComp);
// The merged skeleton's bones as name hashes, in index order. A NAME is the only handle on a bone
// that survives the trip between two players: a character is merged from body + garments and the
// merge takes the UNION of their bones, so two people's skeletons agree on names and on nothing
// else -- not count, not order, not which index is the head. An index-keyed pose therefore drives
// the wrong bones the moment either side wears something the other does not.
// Cached, because resolving ~95 FNames is not something to do per frame.
int   SkeletonBoneHashes(void* meshComp, uint32_t* out, int cap);
bool LocalSkaterName(void* pawn, char* out, int cap);   // gi -> FSkaterInstance::SkaterName
bool LocalMapName(void* pawn, char* out, int cap);      // the UWorld object's own name
// ---- pretty map labels. The internal level name is what travels, but it is a long asset name and
// the GAME already ships the human one. `UMapSelectDataAsset::_maps` is a TArray<FMapSelectData> and
// each entry pairs `MapName` (FName, the internal one) with `MapLabel` (FText, exactly what the Map
// Select page displays), so no hardcoded table is needed. Unknown name in / same name out, so a map
// the asset does not list still shows.
// The asset must NOT be searched for -- `FindFirstOf(L"MapSelectDataAsset")` never finds it, and the
// labels then silently fall back to the raw name. `FMapSelectMenuPageEventHandler::
// OnPageItemsAutoGenerated` reads it off the GAME INSTANCE (`mov r14,[rbx+0x388]` right after
// GetGameInstance), which is already available: ask the consumer where it gets its data.
bool CacheMapSelectData(void* pawn);                    // resolves gi+0x388; true once it is cached
bool HaveMapSelectData();
bool PrettyMapName(const char* internalName, char* out, int cap);
// DLC maps are NOT resolvable: their label tables live in per-DLC instances of UMapSelectDataAsset
// that are not resident during play (FindAllOf found nothing while standing in one -- field-tested
// 2026-08-07, the whole hunt was reverted). A DLC map shows its internal level name, accepted.

// ---- member offsets + vtable slots (PDB-derived; a wrong one here is silent, so each cites its source)
namespace off {
    constexpr int kActorRole          = 0xf0;    // AActor::Role  (1 SimulatedProxy, 2 Autonomous, 3 Authority)
    constexpr int kActorRootComp      = 0x130;   // AActor::RootComponent
    // UCapsuleComponent::CapsuleHalfHeight -- a full PDB type layout (2 members, class size 0x470, so
    // these two ARE its tail), not an accessor. A Character's root component IS its capsule, which is
    // what makes "half-height above the origin" the top of the skater's head.
    constexpr int kCapsuleHalfHeight  = 0x468;
    // AActor::Tags (TArray<FName>), read out of AActor::ActorHasTag's own loop:
    //   `mov rax,[rdi+0x170]` (Data) / `movsxd rcx,[rdi+0x178]` (Num) / 8-byte stride.
    // Actors also opt into the replay system BY TAG -- AReplayManager::AddReplayComponents calls
    // UGameplayStatics::GetAllActorsWithTag for each of NAME_ACTORTAG_ActorReplayComponent,
    // ...CameraReplayComponent, ...AnimInstanceReplayComponent, ...FilmerReplayComponent and friends --
    // but that path runs from Init only, and clearing a proxy's tags does not stop it being recorded.
    // ---- the replay camera's own aim target, and the chain to reach it.
    // The registered-component list, read out of UReplayComponentBase::BeginPlay/EndPlay, which is
    // also the answer to "why is a peer in my replay": replay components register THEMSELVES, so a
    // proxy signs itself up simply by being a real skater. See spectate.cpp.
    constexpr int kReplayMgrCompData  = 0x298;   // TArray<IReplayComponent*> data
    constexpr int kReplayMgrCompNum   = 0x2a0;   // ... num  (max at +0x2a4)
    // THE ELEMENT IS NOT A UObject*. Both BeginPlay and EndPlay do `lea rbx,[rsi+0xb0]` before
    // searching the array, i.e. what is stored is the INTERFACE sub-object at `component + 0xb0`.
    // Reading entries as UObjects mistakes `entry+0x10` (= component+0xc0) for a UClass*.
    constexpr int kReplayIfaceToComp  = 0xb0;
    // UReplayComponentBase::_manager. BeginPlay's first act is `if (this->_manager) return;`, so once
    // it is set the component can NEVER register again -- used as a permanent latch.
    constexpr int kReplayCompManager  = 0xb8;
    constexpr int kReplayMgrInputCtl  = 0x280;   // AReplayManager::_replayInputController
    constexpr int kInputCtlCamera     = 0x260;   // AReplayInputController::_replayCamera
    constexpr int kReplayCamLookAt    = 0x868;   // AReplayCamera::_lookAtTarget (AActor*) -- what
                                                 // GetLookAtLocation reads; honoured by the Orbit and
                                                 // Tripod tick modes (Free/Recorded ignore it)
    constexpr int kReplayCamActiveType= 0x878;   // _activeCameraType (EReplayCameraType, uint8)
    constexpr int kReplayCamOrbit     = 1;       // RCT_Orbit
    constexpr int kSoundConcMaxCount  = 0x00;    // FSoundConcurrencySettings::MaxCount

    constexpr int kActorTagsData      = 0x170;
    constexpr int kActorTagsNum       = 0x178;
    constexpr int kCompQuat           = 0x1c0;   // USceneComponent::ComponentToWorld.Rotation (FQuat)
    constexpr int kCompPos            = 0x1d0;   // ...Translation (FVector)
    constexpr int kSkaterMesh         = 0x280;   // ASkaterCharacterBase mesh component
    // The merged skeleton's size. USkinnedMeshComponent::SkeletalMesh -> USkeletalMesh::RefSkeleton
    // -> FReferenceSkeleton::FinalRefBoneInfo (TArray<FMeshBoneInfo>) -- FinalRefBoneInfo, not Raw:
    // it is the array the component-space transforms are indexed by. A garment carrying its own rig
    // merges to 95 where the stock body is 70.
    constexpr int kMeshSkeletalMesh     = 0x480;
    constexpr int kSkelMeshRefSkeleton  = 0x1b0;
    constexpr int kRefSkelFinalBoneInfo = 0x20;
    constexpr int kMeshBoneInfoStride   = 12;    // FMeshBoneInfo { FName Name; int32 ParentIndex; }
    constexpr int kSkaterMoveComp     = 0x550;   // -> USkaterMovementComponent
    constexpr int kSkaterBoard        = 0x568;   // -> ASkateboardEx   (ownership MUST be back-link checked)
    constexpr int kBoardSkater        = 0x4d8;   // ASkateboardEx -> owning skater (the back-link)
    constexpr int kMoveOnBoard        = 0xe20;   // USkaterMovementComponent::bOnBoard
    constexpr int kSkaterFlags710     = 0x710;   // bitfield: bit 0x02 = _isRagDoll, 0x10@+0x711 = physAnim
    constexpr int kSkaterCanBail      = 0x649;   // _canBail -- the game's own bail veto
    // ---- ASkaterCharacterBase::_skateboardingAnimParams -- an FSkateboardingAnimParams at +0x610 that
    // the ANIM BLUEPRINT reads directly off the character. It is NOT mirrored on USkaterAnimInstance,
    // so the 97-field driver blob structurally cannot carry any of it: every field here has to be
    // transported and written by hand. (kSkaterBrake1/2 below are two of its members -- +0x2
    // IsBrakingFoot / +0x3 IsBrakingTail -- which independently confirms the base offset.)
    //   +0x0 FootPosition  +0x1 FootPositionTransition  +0x2 IsBrakingFoot  +0x3 IsBrakingTail
    //   +0x4 IsFalling  +0x5 IsLanding  +0x6 IsAboutToLand  +0x8 LandHeightRatio
    //   +0xc PushState (EPushState)  +0x10 PushSpeedMultiplier (float)  +0x14 TrickPopRatio
    // Note 0xa0c is `_primoType`, NOT the push state: it is ~always 0, so a misread there looks stable
    // and plausible while feeding SetPushState on nothing. Name a field in the PDB before trusting it.
    constexpr int kSkaterAnimParams   = 0x610;
    constexpr int kSkaterPushState    = 0x61c;   // FSkateboardingAnimParams::PushState -- what
                                                 // SetPushState actually assigns (`mov [rbx+0x61c],dil`)
    // FSkateboardingAnimParams::PushSpeedMultiplier. THE push-rate knob: tapping faster raises it, and
    // SetPushState resets it to 1.0f. Must be transported, or a proxy animates every push at 1.0x
    // however fast the sender is tapping.
    constexpr int kSkaterPushSpeedMul = 0x620;
    constexpr int kSkaterBrake1       = 0x612;   // brake type 1 flag -- written by StartBraking impl
    constexpr int kSkaterBrake2       = 0x613;   // brake type 2 flag -- written by StartBraking impl
    constexpr int kSkaterTrickDef     = 0x590;   // ASkaterCharacterBase::_currentFlipTrickDef (PDB)
    // The GRIND def pair + ratios. USkaterAnimInstance::GetGrindBlendSpace (0xf58450, called from
    // inside NativeUpdateAnimation -- so it runs on proxies) fetches the grind pose's blend space
    // THROUGH these: _currentGrindDef preferred while grinding, _targetGrindDef otherwise; the blend
    // space pointers live inside the def (+0x1a0/+0x1c8 region, stance-picked). Null on a proxy means
    // no grind upper-body pose, the same gate class as the crank/trick assets.
    // SetCurrentGrindDefinition (0x1004820) is 3 field writes: if changed, store +0x6d0 and cache
    // def+0x38 -> skater+0x708. Mirrored directly on the proxy rather than sig'd, being that small.
    constexpr int kSkaterGrindDefTgt  = 0x6c8;   // _targetGrindDef  (UGrindOrSlideDefinition*)
    constexpr int kSkaterGrindDefCur  = 0x6d0;   // _currentGrindDef
    constexpr int kSkaterGrindCache   = 0x708;   // <- def+0x38, cached by SetCurrentGrindDefinition
    constexpr int kGrindDefCacheSrc   = 0x38;    // the field inside the def that +0x708 caches
    constexpr int kSkaterGrindPitch   = 0x6d8;   // float _grindPitchRatio
    constexpr int kSkaterGrindYaw     = 0x6dc;   // float _grindYawRatio -- NativeUpdateAnimation copies
                                                 // it into the active grind anim set's blend input
    // The board's movement MODE byte: USkateboardMovementComponent::SetMovementMode stores at
    // iface+0x50 with iface = comp+0x138 => comp+0x188. Mode 9 = in-hand/carried (the byte
    // ASkateboardEx::Tick compares against 9 to mute riding audio). Transported so the receiver can
    // tell "mounted but the board is still in the hand" from "riding" -- bOnBoard alone flips at the
    // START of the mount, a beat before the board leaves the hand.
    constexpr int kBoardMoveComp      = 0x298;   // ASkateboardEx -> USkateboardExMovementComponent
    constexpr int kBoardMoveMode      = 0x188;   // its movement mode byte (iface+0x50 via +0x138) --
                                                 // a DERIVED offset, used only as a fallback:
                                                 // BoardMovementMode() prefers the game's own accessor
    constexpr int kBoardModeInHand    = 9;
    constexpr int kBoardAudioData     = 0x330;   // ASkateboardEx::_audioData -- Tick's audio block is
                                                 // gated on this being non-null (PDB member name)
    constexpr int kBoardIface         = 0x280;   // the board's interface sub-object (its own vtable)
    constexpr int kIVtblGetBoardMove  = 0x108;   // iface vtbl: GetSkateboardMovement
    constexpr int kMVtblGetMoveMode   = 0x1b0;   // returned movement's vtbl: the movement-mode getter
                                                 // (the exact pair ASkateboardEx::Tick's audio gate
                                                 //  calls before its `cmp al, 9`)
    // The byte ApplyToggleOnBoard's transition-save FLAG is derived from (== 2 || == 4 => flag 1).
    // Semantics unproven -- the predicate is mirrored, not understood; read off the proxy's own skater
    // exactly as the game reads its own.
    constexpr int kSkaterToggleState  = 0xa38;
    // The CRANK = the trick-setup CROUCH. Skater-level state the anim's SetCrank (called from
    // NativeUpdateAnimation, so it RUNS on the proxy) derives the crouch from each frame, copying the
    // def's blend spaces into the anim (def+0x40/+0x48 -> anim+0x4a0/+0x4a8), which is what un-NULLs
    // the crouch gates. The def's identity travels as its INDEX in `UTricksDatabase::_crankList`, not
    // as an offset: FCrankDefinition lives in a heap TArray, so no offset inside the trick def can
    // name it. Both processes load the same DB asset, reached through each side's OWN controller.
    constexpr int kSkaterIsCranking   = 0x580;   // BIT 0 of this byte (SetCranking: and 0xfe / or)
    constexpr int kSkaterCrankDef     = 0x588;   // FCrankDefinition* (into the DB's _crankList; the
                                                 // game LEAVES it set after a crank ends, so the index
                                                 // transports continuously, not just mid-crank)
    constexpr int kSkaterCrankPocket  = 0x658;   // float _crankPocketRatio (the crouch DEPTH)
    constexpr int kPcTricksDb         = 0x6c8;   // ASessionPlayerController::_tricksDatabase (PDB)
    constexpr int kDbCrankList        = 0x2b0;   // UTricksDatabase::_crankList TArray data ptr; int32
                                                 // Num at +0x2b8 (PDB)
    constexpr int kCrankDefSize       = 0x60;    // sizeof(FCrankDefinition) = 96 (PDB)
    // anim-instance gate assets, for the ANIMGATE diagnostic
    constexpr int kAnimCrankBS        = 0x4a8;   // CrankLoopBlendSpace (gates the crouch fields)
    constexpr int kAnimFlipTrick      = 0x4c8;   // FlipTrick (gates the trick ratios)
    constexpr int kAnimRevertBS       = 0x4e0;   // RevertBlendSpace
    constexpr int kObjNamePrivate     = 0x18;    // UObjectBase::NamePrivate (FName)
    // USkeletalMeshComponent, for the replay-driver probe (PDB): the bitfield byte carrying
    // bNoSkeletonUpdate and bPauseAnims, and the per-mesh anim rate. Read-only diagnostics.
    // ASkaterCharacterBase fields the playback-entry handler ATTACHES: `[0xa68]->AttachToComponent(
    // manager->Root | [0xa50])`. Null [0xa68] on a proxy was the original replay-editor crash; the
    // guard now reads it at call time and passes the entry only when the attach can succeed.
    constexpr int kSkaterReplayAttachA = 0xa68;
    constexpr int kSkaterReplayAttachB = 0xa50;
    // AReplayManager (PDB): the active instance data pointer, and on it the playhead in SECONDS.
    // The scrub clock for replay sync -- no keyframe index math, the game keeps the float for us.
    constexpr int kReplayMgrActiveInst = 0x278;   // FReplayManagerInstanceData*
    constexpr int kReplayInstCurTime   = 0x8d4;   // CurrentPlayTime  (float, seconds into the timeline)
    constexpr int kReplayInstTotalTime = 0x8dc;   // TotalPlayTime    (float, timeline length)
    // USkinnedMeshComponent::VisibilityBasedAnimTickOption (PDB +0x604). 3 = OnlyTickPoseWhenRendered:
    // an unrendered mesh skips anim update+evaluation entirely, which is where a proxy's per-frame
    // cost lives. bRecentlyRendered includes the shadow passes, so a peer whose shadow you can see
    // still animates.
    constexpr int kMeshAnimTickOption = 0x604;
    constexpr int kMeshAnimFlagsByte  = 0x8c1;
    constexpr int kMeshGlobalAnimRate = 0x8b0;
    // ---- TYPE IDENTITY. All three read out of the PDB, not inferred:
    //   pdbmembers.py UObjectBase -> ClassPrivate +0x10, NamePrivate +0x18
    //   pdbmembers.py UStruct     -> SuperStruct  +0x40
    //   pdbmembers.py UTricksDatabase -> _flipTricks +0x2c0
    // Every name accepted from a peer is resolved with StaticFindObject(ANY_PACKAGE), which returns
    // the first object of ANY class with that name -- so the SENDER chooses the type unless it is
    // checked. These make that check possible.
    constexpr int kObjClassPrivate    = 0x10;    // UObjectBase::ClassPrivate (UClass*)
    constexpr int kStructSuperStruct  = 0x40;    // UStruct::SuperStruct -- the class chain walk
    constexpr int kDbFlipTricks       = 0x2c0;   // UTricksDatabase::_flipTricks
                                                 // TArray<UFlipTrickDefinition const*>, Num at +0x2c8
    // UMapSelectDataAsset / FMapSelectData (PDB-exact). The array's Num sits at +8 of the TArray, as
    // everywhere else.
    constexpr int kGiMapSelectData    = 0x388;   // USessionGameInstance -> UMapSelectDataAsset*, read
                                                 // out of OnPageItemsAutoGenerated's own instructions
    constexpr int kMapDataMaps        = 0x30;    // UMapSelectDataAsset::_maps (its Num at +0x38 is the
                                                 // very field that handler bounds-checks -- independent
                                                 // confirmation of this offset)
    constexpr int kMapDataStride      = 120;     // sizeof(FMapSelectData)
    constexpr int kMapDataName        = 0x00;    //   ::MapName  (FName, the internal level name)
    constexpr int kMapDataLabel       = 0x10;    //   ::MapLabel (FText, what Map Select displays)
    // ---- the pause menu. Every offset below is PDB-exact (pdbmembers) and each was re-confirmed
    // against the code that uses it, because a wrong one here is silent.
    // UMenuPageContainer (the widget that OWNS the page and routes the gamepad):
    constexpr int kContainerPage      = 0x2a0;   // _menuPage (UMenuPage*) -- the page the container is
                                                 // showing, which is the page a back action applies to
    // UMenuPage:
    constexpr int kPageMaxVisible     = 0x290;   // _maxVisibleItems -- CreatePageItems creates rows
                                                 // [_itemsPanelHeaderIndex .. +_maxVisibleItems) only,
                                                 // so an appended row past the cap is silently not built
    constexpr int kPageActiveDef      = 0x298;   // _activePageDefinition (UMenuPageDefinition*)
    constexpr int kPageItemWidgets    = 0x2a0;   // TArray<UMenuPageItem*>; Num at +0x2a8 -- THIS is
                                                 // what GotoNextItem/SetSelectedIndex bound on, NOT the
                                                 // definitions array: navigation counts WIDGETS
    constexpr int kPageHeaderIndex    = 0x2b4;   // _itemsPanelHeaderIndex (the scroll window's start)
    constexpr int kPageSelectedIndex  = 0x2b8;   // _selectedIndex
    constexpr int kPageItemsPanel     = 0x2d0;   // _itemsPanel (UVerticalBox) -- null = CreatePageItems
                                                 // early-outs, so it doubles as "is this page built"
    constexpr int kPageItemDefs       = 0x2e0;   // _pageItemDefinitions, the page's OWN working copy;
                                                 // Num at +0x2e8. Never written by the mod -- only the
                                                 // scroll math reads it (Num vs _maxVisibleItems)
    // UMenuPageDefinition:
    constexpr int kPageDefKey         = 0x30;    // _key (FName) -- the pause root page is "PauseMenuPage"
    constexpr int kPageDefItems       = 0x58;    // _menuItems TArray<FMenuPageItemDefinition>
    // FMenuPageItemDefinition -- 144 B, stride confirmed by SetActivePageDefinition's destruct loop
    // (`add rsi, 0x90`) and field-by-field by CreatePageItems' element copy. An FText is
    // {ITextData* ObjectPtr; FReferenceController* RefCtrl; uint32 Flags} = 24 B, and the refcount
    // `lock inc` lands on RefCtrl+8.
    constexpr int kItemSize           = 0x90;
    constexpr int kItemKey            = 0x00;    // _key (FName)
    constexpr int kItemType           = 0x0d;    // _itemType (1 Selection, 2 MultiOption, 3 ProgressBar,
                                                 //            4 SliderBar -- CreatePageItem's jump table)
    constexpr int kItemLabel          = 0x10;    // _label FText
    constexpr int kItemShortDesc      = 0x28;    // _shortDescription FText -- the footer line
    constexpr int kItemLongDesc       = 0x40;    // _longDescription FText
    constexpr int kItemSubPage        = 0x58;    // _subPageDefinition (non-null = opens a sub-page)
    constexpr int kItemMultiTexts     = 0x68;    // _multiOptionTexts TArray<FText>; Num +0x70, Max +0x74
    constexpr int kItemMultiStart     = 0x78;    // _multiOptionStartingIndex -- the row's CURRENT value
                                                 // for a MultiOption row, and it lives on the DEFINITION,
                                                 // so a toggle needs no widget poking at all
    constexpr int kItemProgMin        = 0x80;    // _progressDisplayValueMinimum  (the DISPLAYED range;
    constexpr int kItemProgMax        = 0x84;    //  _progressDisplayValueMaximum  the bar itself is 0..1)
    constexpr int kItemProgIncrement  = 0x88;    // _progressIncrement
    // A ProgressBar row's CURRENT value is NOT on the definition -- min/max/increment are all it
    // carries. Stock pages set it on the created WIDGET (UMenuPageItem::ProgressBarSetPercent) from
    // their serialize path, so injected rows must call that setter once the rows are built.
    // UMenuPageItem (the row WIDGET):
    constexpr int kMenuItemDef        = 0x2c0;   // its OWN embedded FMenuPageItemDefinition copy -- the
                                                 // one SetSelectedIndex and the confirm params are built
                                                 // from, which is why an injected row's _key survives
                                                 // all the way to the confirm handler
    // FOnMenuPageSelectionConfirmedParams (PDB size 152): the item definition is INLINE, not a pointer.
    constexpr int kSelParamsPageDef   = 0x00;    // UMenuPageDefinition*
    constexpr int kSelParamsItem      = 0x08;    // FMenuPageItemDefinition (so the item _key is at +0x08)
    constexpr int kSelParamsFlag      = 0x98;    // the trailing bool (the enabled-check writes here)
    // The VALUE-CHANGE params share that head and put old/new in the same two trailing slots (PDB:
    // FOnMenuPageProgressBarValueChangedParams is 160 = 8 + 144 + OldPercent + NewPercent; the
    // multi-option twin was read out of FAudioMenuPageEventHandler's own `movzx edx,[rsi+0x9c]`).
    constexpr int kChangeParamsOld    = 0x98;    // int32 old index  / float old percent
    constexpr int kChangeParamsNew    = 0x9c;    // int32 new index  / float new percent (0..1)
    // ---- audio funnel ----
    // UActorComponent::OwnerPrivate, read straight out of `UActorComponent::GetOwnerRole`'s 21-byte
    // body (`mov rax,[rcx+0xa0]` -> `movzx eax,[rax+0xf0]`) -- an accessor that small is an exact
    // oracle. USceneComponent inherits it, so an attached sound's owner is one deref: this is how a
    // captured sound is attributed to a skater. It also re-confirms kActorRole = 0xf0 independently.
    constexpr int kCompOwner          = 0xa0;
    // UAudioComponent vtable slots. IsPlaying is the slot the funnel itself calls right after the
    // spawn returns (at 0x340e1de) before tagging the component; Stop sits one slot below it and is
    // what every ASkateboardEx::Stop*Audio ends up calling. There is NO stop funnel and no _onStop
    // delegate, so loop lifetime has to be tracked here, and these two are how.
    constexpr int kAcVtblStop         = 0x560;
    constexpr int kAcVtblIsPlaying    = 0x568;
    // Byte length of UAnimNotify_PlaySoundRecorded::PlaySound (PDB: rva 0x3406cd0 size 0x122), for the
    // return-address range test. Both of its funnel call sites fall inside it.
    constexpr int kNotifyPlaySoundLen = 0x122;
    // Byte lengths of the two version-DISPLAY functions, for the same return-address range test
    // (PDB: CreateIntroUI 0xf84090 size 0x199, UPauseMenuPageContainer::NativeOnInitialized
    // 0x107a8a0 size 0x32a).
    constexpr int kIntroUiLen         = 0x199;
    constexpr int kPauseInitLen       = 0x32a;
    // ---- COSMETICS. A skater's look is NOT on the skater: ASkaterCharacterBase::RefreshVisuals reads
    // it off the GAME INSTANCE, so there is one cosmetic identity per PROCESS and dressing a proxy
    // means borrowing it for one RefreshVisuals call. See cosmetics.cpp.
    constexpr int kGiSkaterInstance   = 0x3f0;   // USessionGameInstance::_skaterInstance (FSkaterInstance)
    constexpr int kSkInstName         = 0x00;    //   FSkaterInstance::SkaterName (FName)
    constexpr int kSkInstVisualDef    = 0x08;    //   ::BaseVisualDefinition (USkaterVisualsDefinition*)
    constexpr int kSkInstProfile      = 0x10;    //   ::CustomizationProfile  (=> gi+0x400, what
                                                 //   RefreshVisuals was seen reading)
    // The BASE BODY. The rebuild takes the definition from the ACTOR, not the instance:
    // RefreshVisuals (Epic 0x1000590 @+0x156) passes skater+0x570 into BuildCharacterMesh_From-
    // CustomizationProfile. A peer's definition NAME is resolved through the game instance's own
    // _skaterDefinitions soft-pointer array -- its DECLARED element type (PDB:
    // TArray<TSoftObjectPtr<USkaterVisualsDefinition>>) is the type gate a bare
    // StaticFindObject(ANY_PACKAGE) can never provide, and TryLoad on the matching path both loads a
    // never-resident pro/female body asset and proves it.
    constexpr int kSkaterVisualDef    = 0x570;   // ASkaterCharacterBase::_skaterDefinition
    // ASkaterCharacter's derived part starts at 0xa40 (ASkaterCharacterBase is size 0xa40); its
    // camera components live right at its head. PopulateMarkerInfo is dispatched on the SUB-OBJECT
    // at actor+0xa40, so a hook there sees `this` = that sub-object, not the actor.
    constexpr int kSkaterDerivedPart  = 0xa40;
    constexpr int kSkaterFollowCam    = 0xa50;   // ASkaterCharacter::FollowCamera -- null on proxies
    constexpr int kMarkerInfoSize     = 112;     // sizeof(FSessionPlayerMarkerInfo), PDB
    constexpr int kGiDefaultVisualDef = 0x1d0;   // USessionGameInstance::_defaultVisualsDefinition
    constexpr int kGiSkaterDefs       = 0x1d8;   // ::_skaterDefinitions TArray<TSoftObjectPtr<...>>
                                                 // (data +0, Num +8; entry stride kSoftPtrSize)
    // UCustomizationItemDefinition + the mesh entries its rebuild resolves (PDB-exact).
    // A TSoftObjectPtr is {FWeakObjectPtr(8), int32 TagAtLastTest, pad, FSoftObjectPath(24)} = 40 B,
    // so the loadable PATH sits at +0x10 inside each of these and the cached weak pointer at +0x00.
    constexpr int kItemDefMeshes      = 0x78;    // TArray<FCustomizationItemMesh> (data, Num at +0x80)
    constexpr int kItemMeshStride     = 0x108;   // sizeof(FCustomizationItemMesh) = 264
    constexpr int kSoftPathInPtr      = 0x10;    // FSoftObjectPath inside a TSoftObjectPtr
    // the four soft meshes per entry: AMXX_StaticMesh, AFXX_StaticMesh, AMXX_Mesh, AFXX_Mesh
    constexpr int kItemMeshSoftOffs[] = { 0x00, 0x28, 0x50, 0x78 };
    // The MATERIALS are soft pointers too, and must be loaded alongside the meshes: the rebuild only
    // ever RESOLVES, so loading meshes alone gives the right garment in the WRONG skin (a branded
    // shirt renders as the plain black one because its material never resolved).
    // FCustomizationItemMesh::ItemMaterials is the TArray whose Num MapItemMaterials reads at +0xa8.
    constexpr int kItemMeshMaterials  = 0x0a0;   // TArray<FCustomizationItemMaterial> (Num at +0xa8)
    constexpr int kItemMatStride      = 0x078;   // sizeof(FCustomizationItemMaterial) = 120
    constexpr int kItemMatSoft        = 0x008;   //   ::Material        TSoftObjectPtr<UMaterialInterface>
    constexpr int kItemMatVariants    = 0x030;   //   ::MaterialVariants TArray (Num at +0x38)
    constexpr int kItemMatSoftAFXX    = 0x040;   //   ::Material_AFXX
    constexpr int kItemMatVariantsAF  = 0x068;   //   ::MaterialVariants_AFXX (Num at +0x70)
    constexpr int kItemMatVarStride   = 0x068;   // sizeof(FCustomizationItemMaterialVariant) = 104
    constexpr int kItemMatVarSoft     = 0x040;   //   ::Material inside a variant
    constexpr int kItemDefAlphaMasks  = 0x088;   // UCustomizationItemDefinition::AlphaMasks TArray
    constexpr int kItemDefAlphaMaskAF = 0x098;   //   ::AlphaMasks_AFXX
    constexpr int kSoftPtrSize        = 0x028;   // sizeof(TSoftObjectPtr<...>) = 40
    // ---- the INVENTORY INSTANCE: where a garment's LOOK-modifying attributes actually live.
    // The profile only says WHICH item and WHICH instance; colour, pattern, variant and sock height
    // hang off the instance in the OWNER'S inventory, so a transported index alone can never carry a
    // colour -- it selects the receiver's own instance. Every offset here was read out of
    // `FCustomizationInventoryPersistentData::GetInventoryItem`'s own instructions (Epic 0x10fa8e0):
    // it hashes the FName key, indexes with `lea rdx,[rcx+rcx*4]` + `[r8+rdx*8]` (stride 40) and reads
    // the next-link at element+0x20, which pins the 24-byte value to element+0x08.
    constexpr int kGiPlayerProfile    = 0x3e0;   // USessionGameInstance::_playerProfile
    constexpr int kProfileInventory   = 0x6a0;   // UPlayerProfile -> FCustomizationInventoryPersistentData
    constexpr int kInvItemsMap        = 0x10;    //   ::CustomizationItems (after Version FString)
    constexpr int kInvElemStride      = 0x28;    //   TMap element: FName key +0x00, value +0x08
    constexpr int kInvElemInstances   = 0x10;    //   -> FCustomizationInventoryItem::InstanceList TArray
    constexpr int kInvInstanceStride  = 0x1a0;   // sizeof(FCustomizationInventoryItemInstance) = 416
    constexpr int kInstSockHeight     = 0x0f0;   //   ItemAttributes::SockHeightIndex (uint8)
    constexpr int kInstColorOverrides = 0x0f8;   //   ::CustomColorAttribute.CustomColorOverrides
    constexpr int kInstColorPatterns  = 0x148;   //   ::CustomColorAttribute.CustomColorPatterns
    constexpr int kInstVariantId      = 0x198;   //   the INSTANCE's own VariantId (int32)
    // TMap<FString, FCustomizationInventoryItemCustomColorItem>: FString(16) + {bool,FLinearColor}(20)
    // padded to 40, + the two hash ints = 48. NOT read out of an instruction like the others, so the
    // walker VALIDATES each key FString before trusting it and bails out loudly if the layout is wrong.
    constexpr int kColorElemStride    = 0x30;
    constexpr int kColorValueOff      = 0x10;    // the value inside the element (after the FString key)
    constexpr int kColorEnabledOff    = 0x00;    //   IsEnabled (uint8)
    constexpr int kColorRgbaOff       = 0x04;    //   FLinearColor (4 floats)
    constexpr int kProfStance         = 0x00;    // FCustomizationProfile::Stance (EStanceType)
    constexpr int kProfCharItems      = 0x08;    //   ::CharacterItems  TMap<int, FCustomizationProfileItem>
    constexpr int kProfBoardItems     = 0x58;    //   ::SkateboardItems TMap<same>
                                                 // (item = FName +0x00, u8 inst +0x08, i32 variant +0x0c)
    // USkaterAnimInstance foot sockets (PDB, exact): transported RAW WORLD -- the proxy stands at the
    // sender's exact world position (no net smoothing in overlay mode), so no rebasing is needed and
    // there are ZERO rotator conversions anywhere in the path.
    constexpr int kAnimLFootLoc       = 0x404;   // FVector
    constexpr int kAnimLFootRot       = 0x410;   // FRotator (native field: sampled raw, written raw)
    constexpr int kAnimRFootLoc       = 0x41c;   // FVector
    constexpr int kAnimRFootRot       = 0x428;   // FRotator
    // The HAND IK TARGETS. The blob carries LeftHandIKAlpha/RightHandIKAlpha (0x458/0x45c), and those
    // alphas are legal ONLY because these targets travel with them: send the blend WEIGHT without the
    // TARGET and the proxy's own graph invents a target on its own schedule, so the arms move late.
    // Same treatment as the feet: raw world, no conversions.
    constexpr int kAnimLHandLoc       = 0x460;   // FVector
    constexpr int kAnimLHandRot       = 0x46c;   // FRotator
    constexpr int kAnimRHandLoc       = 0x478;   // FVector
    constexpr int kAnimRHandRot       = 0x484;   // FRotator
    constexpr int kMovePushFlags      = 0x7e8;   // the push-machinery flag byte on the movement comp
                                                 // (== iface+0x6b0 via iface=comp+0x138). Bit 0x40 =
                                                 // the push REQUEST the game consumes to play its own
                                                 // push montage -- the anim blob's IsPushing bool
                                                 // alone renders nothing.
    constexpr int kAnimIsGrounded     = 0x5fa;   // USkaterAnimInstance::IsGrounded (the trick gate)
    // ---- the SKELETON itself. Needed because a skater in REPLAY PLAYBACK has a totally inert anim
    // instance -- measured: 0 of 97 blob fields change across 30 s of scrubbing -- so the pose that is
    // visibly playing exists only in the bone arrays. Every offset here was read out of an accessor's
    // own instructions:
    //   USkeletalMeshComponent::GetBoneSpaceTransforms -> `movsxd rsi,[rdi+0x6e0]` (Num),
    //                                                     `mov rdi,[rdi+0x6d8]`   (Data)
    //   USkinnedMeshComponent::FlipEditableSpaceBases  -> `(idx + 0x4b) << 4` + this  => the
    //     ComponentSpaceTransformsArray[2] base, with the two buffer indices at +0x4f0 / +0x4f4.
    constexpr int kMeshBoneSpaceData  = 0x6d8;   // TArray<FTransform> BoneSpaceTransforms (LOCAL space)
    constexpr int kMeshBoneSpaceNum   = 0x6e0;
    constexpr int kMeshCompSpaceArr   = 0x4b0;   // ComponentSpaceTransformsArray[2], stride 0x10
    constexpr int kMeshCompSpaceStride= 0x10;
    constexpr int kMeshCompEditIdx    = 0x4f0;   // CurrentEditableComponentTransforms
    constexpr int kMeshCompReadIdx    = 0x4f4;   // CurrentReadComponentTransforms (what renders)
    // FTransform = FQuat(16) + FVector+pad(16) + FVector+pad(16). The probe VALIDATES this by checking
    // the quaternions come out unit-length -- a wrong stride shows up immediately as garbage rotations
    // rather than as a subtly wrong pose.
    constexpr int kTransformStride    = 48;
    constexpr int kTransformRotOff    = 0;       // FQuat  (x,y,z,w)
    constexpr int kTransformPosOff    = 16;      // FVector

    constexpr int kMeshAnimInstance   = 0x6b0;   // USkeletalMeshComponent::AnimScriptInstance (PDB).
                                                 // Not to be confused with 0x460, which is
                                                 // LeftHandIKLocation INSIDE the instance.
    // Measure the DECK (`_flipper`, board+0x4e8), never the actor root: the actor root's transform is
    // STATIC while riding, so a sender publishes a frozen deck position, the receiver parks on it with
    // err=0cm, and the telemetry reads "perfect" while the visible board never moves. The root can
    // even vanish from probes while the deck still rides. Fallback chain: _flipper -> _truckBack
    // (+0x4f0) -> actor root.
    constexpr int kBoardFlipper       = 0x4e8;   // ASkateboardEx::_flipper -- the deck you can SEE
    constexpr int kBoardTruckBack     = 0x4f0;   // ASkateboardEx::_truckBack -- first fallback
    // Board breakage (PDB: pdbmembers ASkateboardEx). The state byte is what BreakBoard_Internal
    // stores and RebuildBrokenBoard clears, so transporting it covers every break/repair path.
    constexpr int kBoardBrokenState   = 0x3f1;   // ASkateboardEx::_currentBrokenBoardState (u8, 0=intact)
    constexpr int kBoardBreakRequested= 0x783;   // ASkateboardEx::_breakBoardRequested -- CanBreakBoard
                                                 // fails while set: held at 1 on proxy boards so they
                                                 // can never DECIDE to break (the _canBail-hold shape)
    // vtable slots on UPrimitiveComponent (PDB: linear +0x610, angular 0x10 before it in 4 vtables)
    constexpr int kVtblSetLinearVel   = 0x610;
    constexpr int kVtblSetAngularVel  = 0x620;

    // ---- THE OBJECT DROPPER (PDB: pdbmembers AObjectDropperManager / ...ObjectsDatabase).
    // The first two are corroborated independently of the PDB by the two accessors we sig: IsActive's
    // body IS `_instance && _instance[0x490] != 0`, and GetObjectsDatabase's IS `_instance[0x220]`.
    constexpr int kDropMgrDatabase    = 0x220;   // AObjectDropperManager::_objectsDatabase
    constexpr int kDropMgrMode        = 0x490;   // _currentMode (EObjectDropperModes u8; 0 = inactive)
    constexpr int kDropMgrAllObjects  = 0x498;   // _allObjects TArray<AActor*> -- EVERY dropped object
                                                 // in the world. Diffing it is how placements, moves,
                                                 // duplicates and call-backs are all detected at once.
    constexpr int kDropMgrSelected    = 0x4d8;   // _selectedObjects TArray<AActor*> (being dragged)
    constexpr int kDropDbCategories   = 0x30;    // UObjectDropperObjectsDatabase::_objectCategories
    constexpr int kDropCatStride      = 0x28;    // sizeof FObjectDropperObjectCategory
    constexpr int kDropCatObjects     = 0x18;    // FObjectDropperObjectCategory::_objectList
    constexpr int kDropInfoStride     = 0x90;    // sizeof FObjectDropperObjectInformation
    // FObjectDropperObjectInformation::_objectClass is a TSoftClassPtr at +0, whose FSoftObjectPath
    // sits at +0x10 inside it (the FWeakObjectPtr + TagAtLastTest come first). That path is what
    // GetObjectInformationByID itself calls GetAssetName on, and what TryLoad turns into the UClass.
    constexpr int kDropInfoClassPath  = 0x10;
    // ---- MOBILITY. A dropped prop sits at its DEFAULT mobility -- Static -- whenever the dropper is
    // closed, and UE silently ignores a move on a Static component: the write is accepted, the
    // transform does not change, and nothing reports it. That is why a peer's object appeared where it
    // was first spawned and then never moved again, and why opening the local dropper made it jump to
    // the right place (the game flips every prop to Movable on activation).
    // Read out of UObjectDropperPickableObject::OnObjectDropperActivated, which caches
    // RootComponent[+0x14f] into _defaultObjectMobilityType and then vcalls slot +0x500 with 2.
    constexpr int kCompMobility       = 0x14f;   // USceneComponent::Mobility (EComponentMobility, u8)
    constexpr int kVtblSetMobility    = 0x500;   // USceneComponent::SetMobility(EComponentMobility)
    constexpr int kMobilityMovable    = 2;       // EComponentMobility::Movable
    // UObjectDropperPickableObject::_isCurrentlyPickable. Cleared on every object we spawn for a peer:
    // a mod-spawned prop still carries the real component, so the LOCAL dropper would happily
    // highlight it, pick it up and call it back -- into the local player's own inventory and save.
    constexpr int kPickableIsPickable = 0xb1;
}

// Crank def identity = its INDEX in the shared UTricksDatabase::_crankList. Both the gather and the
// proxy resolve through the LOCAL player controller (set at pawn discovery) -- the DB is one shared
// asset, so the sender's index and the receiver's index name the same FCrankDefinition. All reads are
// SEH-guarded; a stale controller resolves to "no crank" and the gates keep protecting.
void  SetLocalController(void* pc);
bool  PauseMenuOpen();                     // the game's own "is the pause menu up"

// ---- WORLD -> SCREEN. Both go through the local controller set above, and both are SEH-guarded.
// The game's viewport in pixels. False (and untouched outputs) when it cannot be known yet.
bool  ViewportSize(int* outW, int* outH);
// A world point in VIEWPORT pixels, plus its distance from the camera in cm. False when the point is
// behind the camera or the view cannot be resolved -- the caller draws nothing rather than guessing.
bool  ProjectWorldToViewport(const float world[3], float outPx[2], float* outDistCm);
// How high above an actor's origin its head is: the root capsule's own half-height (UCapsuleComponent
// +0x468) plus `headroomCm`. Measured rather than assumed, so it is right for whatever capsule the
// skater actually has -- with a plain fallback if the root is not a capsule or reads implausibly.
bool  ActorHeadPoint(void* actor, float headroomCm, float out[3]);
int   CrankIndexFromPtr(void* crankDef);   // -1 = not resolvable (no controller/DB, out of list, misaligned)
void* CrankPtrFromIndex(int idx);          // nullptr = not resolvable

// ---- TYPE IDENTITY FOR TRANSPORTED NAMES -------------------------------------------------------------
// A peer sends a NAME, resolved with StaticFindObject(ANY_PACKAGE), which returns the first object of
// ANY class bearing that name. Writing that pointer into a typed field, or handing it to a game
// function, is a type confusion the SENDER controls -- and the crash lands in engine code, where the
// mod's SEH cannot help. These answer "is this actually the kind of thing I asked for?" before use.
//
// `IsObjectOfClass` walks ClassPrivate -> SuperStruct comparing class FNames, so a SUBCLASS passes
// (which is required: a cue is a USoundCue or a USoundWave, both USoundBase). Give it the UE name
// without the C++ prefix -- "SoundBase", not "USoundBase". Depth-capped; SEH-guarded; false on any
// doubt, and callers treat false exactly like "this install does not have that asset", a path that
// already ships.
bool IsObjectOfClass(void* obj, const char* className);
// The strongest check available for flip tricks, and it needs no class name at all: the LOCAL
// UTricksDatabase::_flipTricks is a typed array of every legitimate trick def, so membership in it is
// proof -- validating against a local, typed, always-present list rather than trusting a name, the
// same argument as the crank def's index. False if the list cannot be read at all, which callers must
// treat as "unknown", not as "hostile" (see FlipTrickListAvailable).
bool IsKnownFlipTrickDef(void* obj);
bool FlipTrickListAvailable();             // false = no controller/DB yet; membership cannot be judged

// ---- THE SELF-CHECK. A type gate is only as good as its expectation, and a WRONG expectation would
// reject every legitimate trick, grind or sound -- turning a security measure into a broken game.
// So each gate is also run against an object known to be genuine: the LOCAL player's own trick def,
// their own grind def, a cue the game itself handed over through the audio funnel. If the gate rejects
// one of those, the expectation is wrong, and the gate DISABLES ITSELF loudly instead of filtering
// peers on a bad premise.
//   `GateSelfCheck(what, localPassed, logf)` -- report the gate's verdict on a locally-owned object.
//                                              First call decides; later calls are ignored.
//   `GateTrusted(what)`                      -- gate sites ask this before rejecting anything.
// `what` is the same string the gate uses, matched by content.
void GateSelfCheck(const char* what, bool localPassed, void (*logf)(const char*));
bool GateTrusted(const char* what);

// The board's movement MODE, read the way the game's own audio gate reads it: GetSkateboardMovement
// through the board's interface vtable, then the mode getter on what that returns. Diagnose from what
// the CONSUMER reads -- deriving the byte's offset from SetMovementMode's store instead is a plausible
// guess, not the accessor. Falls back to that raw offset if either vcall is unavailable. Returns -1
// when nothing could be read; callers treat unknown as "not in hand", the harmless direction.
int   BoardMovementMode(void* board);

// Resolve every symbol against the loaded exe. Logs one line per symbol (name, address, hit count) so a
// miss is attributable at a glance instead of presenting as an inexplicably dead feature.
const Syms& Resolve(void (*logf)(const char*));
const Syms& Get();

// Table access for the offline verifier (omp_symcheck) -- same data, no process needed.
struct SigEntry { const char* name; const char* sig; bool required; };
int             SigCount();
const SigEntry& SigAt(int i);

}} // namespace omp::game
