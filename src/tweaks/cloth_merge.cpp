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
// SessionTweaks -- CLOTH, phase A round 2: the UN-MERGE. Round 1 mapped the merge completely:
//   * five DoMerge call sites; the NPC dresser is its own caller, everything else is the LOCAL
//     skater (load/map dress, wardrobe preview scrub ~300 ms apart, and a 5-sibling apply family
//     whose spacing matches across both exes -- per-slot dress paths);
//   * source naming is uniform: body `A?XX_FullBody_*`/`Unique_*`, `_HD_` head, `_UB_` upper,
//     `_LB_` lower, `_FT_` feet. A typical merge is 5 sources;
//   * merged OUTPUT meshes are pooled/reused, so slave lifetime keys on re-dress, never on mesh
//     pointer identity.
//
// What this round does: on a skater-site merge, the `_UB_` source is REMOVED from the merge's own
// source array before DoMerge runs, and the pump then rebuilds that garment as a separate
// USkeletalMeshComponent on the skater -- attached to the body mesh, master-posed to it, wearing
// the garment's own materials. Milestone: the shirt looks the same but is its own component; the
// platform cloth data lands on in phase B.
//
// THE SAFETY INVARIANT (why unhandled callers cannot double the shirt): the pump watches the
// skater's SkeletalMesh pointer. When it changes to OUR excluded output, the slave is (re)built;
// when it changes to ANYTHING else (a merge we did not exclude -- e.g. the wardrobe-entry caller
// this round deliberately skips), the slave is EMPTIED, because that merge put the shirt back in.
// Missing a call site therefore degrades to "shirt merged normally", never to two shirts.
//
// Construction is DEFERRED TO THE PUMP -- never done inside the DoMerge callstack (the standing
// rule: no game-API calls from inside a hooked game callstack when a next-tick deferral works).
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "cloth_merge.h"
#include "cloth_sim.h"
#include "catch_tweaks.h"   // CatchTweaks_Skater()
#include "catch_sound.h"    // CatchSound_ObjName -- UObject name text, cached
#include "MinHook.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

// ------------------------------------------------------------------ measured layout (PDB-confirmed)
enum {
    MM_MERGE_MESH   = 0x00,   // FSkeletalMeshMerge::MergeMesh (USkeletalMesh* -- the OUTPUT)
    MM_SRC_LIST     = 0x08,   // FSkeletalMeshMerge::SrcMeshList (TArray<USkeletalMesh*>: data, num)
    MM_STRIP_LODS   = 0x18,
    MM_BUF_ACCESS   = 0x1c,
    OBJ_CLASS       = 0x10,   // UObjectBase::ClassPrivate
    CH_MESH         = 0x280,  // ACharacter::Mesh (USkeletalMeshComponent*)
    SMC_SKELMESH    = 0x480,  // USkinnedMeshComponent::SkeletalMesh
    COMP_WORLD      = 0xa8,   // UActorComponent::WorldPrivate -- the world for registration
    COMP_OWNER      = 0xa0,   // UActorComponent::OwnerPrivate
    // FStaticConstructObjectParameters (PDB, size 64): +0x00 Class, +0x08 Outer, +0x10 Name(FName),
    // +0x18 SetFlags, +0x1c InternalSetFlags, +0x20/21 bools, +0x28 Template, +0x30 InstanceGraph,
    // +0x38 ExternalPackage.
};

// ------------------------------------------------------------------ sigs (dual-exe-verified, sigmake)
// FSkeletalMeshMerge::DoMerge -- Epic 0x2f9c5f0 / Steam 0x2f5f0d0. (this, RefPoseOverrides*).
static const char* SIG_DO_MERGE =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B F9 48 8B EA 48 8B 09";
// StaticFindObject -- Epic 0x1530ec0 / Steam 0x14f2130 (verbatim from game_syms.cpp; called, never hooked).
static const char* SIG_STATIC_FIND =
    "48 89 5C 24 08 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ?? ?? ?? ?? 00 "
    "45 0F B6 F1 49 8B F8 48 8B DA 4C 8B";
// StaticConstructObject_Internal(const FStaticConstructObjectParameters&) -- Epic 0x152fef0 /
// Steam 0x14f1160. ANCHORED MID-FUNCTION (+0x30), NOT at the prologue: something detours this
// function's first bytes at load (UE4SS itself -- it must intercept object construction for its
// object listeners), so a prologue pattern that sigmake proves in the FILE finds nothing in
// MEMORY. The entry address is still perfectly callable (the detour chains); it just cannot be
// located by its head. The hit minus 0x30 is the entry on BOTH exes (Steam hit 0x14f1190 ==
// 0x14f1160 + 0x30, verified -- the bodies align at that offset).
static const int  kScoAnchorOff = 0x30;
static const char* SIG_SCO =
    "00 00 48 8B 39 4C 8D 25 ?? ?? ?? ?? 4C 8B 79 08 48 8B D9 8B 71 18 4C 8B 71 28 F7 87 CC 00 00 00 "
    "80 00 00 10";
// UActorComponent::RegisterComponentWithWorld(UWorld*, ctx) -- Epic 0x2aef090 / Steam 0x2ab18d0.
// The parameterless RegisterComponent() is a tiny wrapper whose bytes match 7 unrelated thunks once
// the call immediate is wildcarded -- sig the real body instead and pass the world ourselves.
static const char* SIG_REG_WORLD =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 7C 24 20 41 56 48 83 EC 40 8B 41 0C 45 33 F6 3B 05 ?? ?? ?? ?? "
    "49 8B F8 48 8B EA 48 8B D9";
// USceneComponent::AttachToComponent(parent, const FAttachmentTransformRules&, FName) -- Epic
// 0x2b502b0 / Steam 0x2b12af0.
static const char* SIG_ATTACH =
    "40 55 53 56 41 54 41 56 48 8D AC 24 80 FA FF FF";
// USkeletalMeshComponent::SetSkeletalMesh(USkeletalMesh*, bool bReinitPose) -- Epic 0x2b65d90 /
// Steam 0x2b285d0.
static const char* SIG_SET_SKELMESH =
    "48 89 5C 24 18 48 89 6C 24 20 56 48 83 EC 20 41 0F B6 E8 48 8B F2 48 8B D9 48 3B 91 80 04 00 00 "
    "0F 84 ?? ?? ?? ??";
// USkinnedMeshComponent::SetMasterPoseComponent(USkinnedMeshComponent*, bool bForceUpdate) -- Epic
// 0x2b8e430 / Steam 0x2b50c70.
static const char* SIG_SET_MASTER =
    "40 53 55 56 48 83 EC 40 48 8B DA 48 8B F1";

// ---- the SKATER-side DoMerge call sites, matched by RETURN ADDRESS. Each sig's bytes START AT
// the return address, so a scan hit IS the retaddr -- no offset arithmetic.
//
// HOW THESE WERE DERIVED, because the first attempt was wrong in a way worth naming: the mod
// logs runtime RVAs of the RUNNING exe, and this user plays the STEAM build -- but every tool in
// SessionMPDev	ools (pdbsym/sigmake/disasm) targets the EPIC exe, which is the only one with a
// PDB. Feeding a logged Steam RVA to those tools reads a DIFFERENT, unrelated function; the sigs
// built that way matched arbitrary bytes and never identified a single real call site.
// The correct method needs no PDB and no guessing: scan BOTH exes for `E8 <rel32 -> DoMerge>` to
// enumerate every call site exactly (6 in each build), then build a sig from one build and confirm
// it lands on a call site in the other -- that PROVES the pairing by bytes. Naming then comes free
// from the Epic PDB. Verified pairs (Steam -> Epic -> source):
//   0xf38f39 -> 0xf79129  CharacterVisualsComponent.cpp:74   NPC/pedestrian dresser  -- NOT ours
//   0xfa7737 -> 0xfe7907  SkaterCharacterBase.cpp:868        the skater's dress (load/map change)
//   0xfc0566 -> 0x1000736 ASkaterCharacterBase::RefreshVisuals   re-dress / apply
//   0xfd3876 -> 0x1013a46 CustomizationCharacter.cpp:774     wardrobe preview scrub
//   0xfd3cc4 -> 0x1013e94 CustomizationCharacter.cpp:552     wardrobe entry
//   0x89ad9e -> 0x7e0e6e  (pedestrian path, never observed)  -- NOT ours
// Each sig below is 16 bytes and UNIQUE IN BOTH EXES, so exactly one hit resolves per site
// whichever build is running.
static const char* SIG_SITE_DRESS =        // SkaterCharacterBase.cpp:868
    "48 8B 07 4C 8B 47 08 48 8B 88 70 05 00 00 41 8B";
static const char* SIG_SITE_REFRESH =      // ASkaterCharacterBase::RefreshVisuals
    "48 8B 86 70 05 00 00 49 8B CC 48 8B 58 48 E8 ??";
static const char* SIG_SITE_WARDROBE_PREV = // CustomizationCharacter.cpp:774
    "49 8B 0C 24 48 8B 89 D0 04 00 00 E8 ?? ?? ?? ??";
static const char* SIG_SITE_WARDROBE_IN =  // CustomizationCharacter.cpp:552
    "48 8B 07 48 8B 88 28 02 00 00 48 8B 90 D0 04 00";

// ASkaterCharacterBase::RefreshVisuals -- Epic 0x1000590 / Steam 0xfc03c0.
// THE IDENTITY SOURCE, and the reason this hook exists at all. The dress paths are METHODS ON
// THE SKATER BASE CLASS, so they run for EVERY skater in the world: the wardrobe's preview
// character and, in co-op, every remote player's proxy. A call site therefore only says "a skater
// is being dressed", never WHICH -- and excluding on call site alone stripped the garment off
// preview characters and off other players (field-reported: no top in the wardrobe, peers with no
// shirts) while the replacement component only ever exists on OUR skater.
// This hook publishes `this` for the duration of the call, so the merge below can require that the
// skater being dressed is ours. Everyone else merges exactly as the game intended.
static const char* SIG_REFRESH_VISUALS =
    "40 55 53 56 48 8D AC 24 90 FE FF FF 48 81 EC 70 02 00 00 48 8B 05 ?? ?? ?? ??";

typedef void  (*RefreshVisualsFn)(void*);
static void* g_origRefresh = nullptr, *g_startRefresh = nullptr;
// Thread-local: dressing happens on the game thread, but scoping it to the thread costs nothing and
// means a background dress (if one ever exists) can never be mistaken for the local player's.
static thread_local void* t_dressing = nullptr;
// Bumped at the start of every dress of OUR character; stamps the material records so a stale one is
// never handed back. See GarmentMatRec.
static long g_matGen = 0;

static void hkRefreshVisuals(void* self) {
    // A new dress: everything the last one resolved is now suspect. See GarmentMatRec.
    if (self && self == CatchTweaks_Skater()) g_matGen++;
    void* prev = t_dressing;      // save/restore rather than clear: RefreshVisuals can nest
    t_dressing = self;
    __try { ((RefreshVisualsFn)g_origRefresh)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    t_dressing = prev;
}

typedef void  (*DoMergeFn)(void*, void*);
typedef void* (*StaticFindFn)(void* cls, void* outer, const wchar_t* name, int exactClass);
typedef void* (*ScoFn)(const void* params);
typedef void  (*RegWorldFn)(void* comp, void* world, void* ctx);
typedef bool  (*AttachFn)(void* comp, void* parent, const void* rules, uint64_t socketName);
typedef void  (*SetSkelMeshFn)(void* comp, void* mesh, bool bReinitPose);
typedef void  (*SetMasterFn)(void* comp, void* master, bool bForceUpdate);

static void* g_orig = nullptr, *g_start = nullptr;
static StaticFindFn  g_staticFind  = nullptr;
static ScoFn         g_sco         = nullptr;
static RegWorldFn    g_regWorld    = nullptr;
static AttachFn      g_attach      = nullptr;
static SetSkelMeshFn g_setSkelMesh = nullptr;
static SetMasterFn   g_setMaster   = nullptr;

// ------------------------------------------------------------------ knobs
static int  g_unmerge = 1;            // ClothUnmerge -- the whole phase-A behavior; live toggle
static int  g_ownMesh = 1;            // ClothOwnMesh -- build our own garment mesh instead of editing
                                      // the shared wardrobe asset; see OwnGarmentMesh for why
// Keyed on the garment's NAME, not its pointer.
//
// The wardrobe's garment meshes are RE-CREATED on every map load (measured: the same garment has a
// different USkeletalMesh address, and different render data, after each switch). Keying this on the
// pointer therefore missed on every map change and built a brand-new multi-megabyte mesh each time --
// and once the table filled, it rebuilt on EVERY DRESS, unbounded, releasing nothing. That is the
// memory growth behind the hard freezes, and heap exhaustion/corruption shows up wherever the allocator
// next runs (observed: Slate text layout teardown freeing a corrupt pointer).
// The NAME is stable across reloads, so one copy per garment per session is built and then reused.
enum { kOwnMeshes = 8 };
static char  g_ownName[kOwnMeshes][64] = {};  // the garment's name -- the only stable key across maps
static void* g_ownSrc[kOwnMeshes]  = {};   // the shared garment mesh AS LAST SEEN (for SourceOfOwn)
static void* g_ownCopy[kOwnMeshes] = {};   // ...and our private build of it
static int   g_ownNext = 0;                // round-robin when the table is full: bounded, never unbounded

// NOTE: there is deliberately NO "forget on character change". The copies outlive characters and maps
// by design -- that is the entire point. Dropping them is what let GC collect a mesh a cloth actor was
// still simulating against.

// Our copies are new objects, so anything keyed on the mesh POINTER -- the material the customization
// resolved, the garment's material index -- misses on them. Translate back to the asset those answers
// were recorded against. (Symptom without this: 'garment material -1', and a garment with no material
// at all, which draws nothing.)
static void* SourceOfOwn(void* mesh) {
    for (int i = 0; i < kOwnMeshes; i++) if (g_ownCopy[i] == mesh) return g_ownSrc[i];
    return mesh;
}

// One slot per garment kind, in tag order: slot 0 is tops, slot 1 bottoms. Keying the slot to the
// TAG rather than to the order garments happen to appear in a merge keeps a given garment on the
// same component across re-dresses, which is what lets the simulation keep its state.
enum { kMaxGarments = kClothMaxGarments, kMaxExcl = 12 };
// 32, not 16: 'AMXX_Double_Knee' is exactly 16 characters and was being stored as 15 plus a
// terminator. It kept working only because a truncated tag is still a substring of the name it was
// meant to match -- a longer name that differs only past character 15 would have matched wrongly.
enum { kTagLen = 32 };
static char g_tags[kMaxGarments][kTagLen] = { "_UB_", "_LB_" };
static int  g_nTags = 2;
static char g_excl[kMaxExcl][40] = {};
static int  g_nExcl = 0;
static int  g_requireClothMat = 1;   // ClothRequireMaterialFlag
static int  g_tintFromColor = 0;     // ClothTintFromColour -- see the note in ApplySwapMaterial
// Per-garment gain overrides, "<name fragment>:<percent>,..." (ClothTintPerGarment). Different
// garments sit on different base tones, so one global gain cannot suit them all; 0 skips the tint.
enum { kMaxTintRules = 8 };
static char g_tintRuleName[kMaxTintRules][40] = {};
static int  g_tintRuleGain[kMaxTintRules] = {};
static int  g_nTintRules = 0;

static int TintGainFor(const char* garmentName) ;

static int  g_tintGain = 200;        // ClothTintGainPct -- the tint MULTIPLIES a mid-tone base, so a
                                     // hue-only colour lands darker than it should; this lifts it
// Re-reading a garment's material when you change clothes. OFF: it works by putting the garment back
// through a merge, and every attempt at that has ended in a render-thread crash. With it off, changing
// clothes keeps the previous garment's textures -- wrong, but stable, which is where this was before.
static int  g_driveMulti = 1;        // ClothDriveMultiSection
static int  g_recapture = 0;         // ClothRecapture
static int  g_matSwap = 1;           // ClothMaterialSwap -- drive a non-cloth garment through a
                                     // cloth-capable master, carrying its own textures across
static void* g_clothMaster = nullptr;  // learned from whichever garment already supports cloth

// Your chosen variant and colours are not on the garment -- the game builds a configured material
// for each customisable item during dressing and hangs it on the character as an override. Pulling a
// garment out of the merge means that never happens for it, which is why separated clothing came out
// in its unconfigured base colours. So: let the FIRST dress run untouched, take the material the game
// built for each garment, and only then dress again with the garments separated out.
static uint64_t g_tintName = 0;        // the cloth master's tint parameter, lifted from a garment
                                       // that already uses it -- no need to build a name ourselves
static void*   g_capturedMat[kMaxGarments] = {};
static void*   g_seenGarment[kMaxGarments] = {};   // what the untouched pass was wearing
static void*   g_capturedFor[kMaxGarments] = {};   // WHICH garment each capture belongs to
static int     g_garmentMatIdx[kMaxGarments] = {}; // which of the garment's materials is its own
// The variant you chose, as the ASSET rather than the per-dress instance. A MaterialInstanceDynamic is
// created per dress and only stays alive while something holds it, so reading one later is a dangling
// pointer; its parent is an ordinary loaded asset and is safe to keep.
static void*   g_capturedVariant[kMaxGarments] = {};
static volatile LONG g_capturePass = 1;           // 1 = observe only, do not exclude yet
static bool    g_captureDone = false;

// Session flags its UPPER-body clothing master as cloth-capable but not its lower-body one, and
// that is fixed when the game is built. But a material instance is mostly its textures and numbers,
// so a garment whose master cannot do cloth can still be drawn THROUGH the master that can, keeping
// its own textures. Whether that looks right depends on the two masters taking the same parameters,
// which is why every value is copied by NAME and anything unmatched is simply left at its default.
typedef void* (*MidCreateFn)(void* parent, void* outer);
typedef void  (*SetMaterialFn)(void* comp, int index, void* material);
typedef void  (*SetTexParamFn)(void* mid, uint64_t name, void* tex);
typedef void  (*SetSclParamFn)(void* mid, uint64_t name, float v);
typedef void  (*SetVecParamFn)(void* mid, uint64_t name, const void* linearColor);
static MidCreateFn   g_midCreate = nullptr;
static SetMaterialFn g_setMaterial = nullptr;
static SetTexParamFn g_setTex = nullptr;
static SetSclParamFn g_setScl = nullptr;
static SetVecParamFn g_setVec = nullptr;
static const char* SIG_MID_CREATE =
    "48 89 5C 24 08 57 48 83 EC 60 48 8B DA 48 8B F9 48 85 D2 ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 8D 15 ?? ?? ?? ??";
static const char* SIG_SET_MATERIAL =
    "85 D2 0F 88 ?? ?? ?? ?? 53 55 56 48 83 EC 20 48 8B D9 48 63 EA 8B 89 58 04 00 00 49 8B F0";
static const char* SIG_SET_TEXPARAM =
    "48 8B C4 55 57 41 56 48 83 EC 50 4D 8B F0 48 89 50 B8 33 FF C6 40 C0 02 4C 8B C2 48 89 78 10";
static const char* SIG_SET_SCLPARAM =
    "48 83 EC 38 48 89 54 24 20 48 8D 54 24 20 C6 44 24 28 02 C7 44 24 2C FF FF FF FF E8 ?? ?? ?? ??";
static const char* SIG_SET_VECPARAM =
    "48 83 EC 48 41 0F 10 00 48 89 54 24 20 4C 8D 44 24 30 48 8D 54 24 20 C6 44 24 28 02 0F 29 44 24 30";

static bool MaterialSupportsCloth(void* garmentMesh);   // defined with the material helpers below

// Split "a,b,c" into the table. Blank entries are skipped so a trailing comma is harmless.
// `what` names the setting so a list that does not fit says so. It used to drop the overflow in
// silence, which is how a tagged garment could go un-simulated with nothing in the log to explain it:
// the fifth tag in a four-tag array simply ceased to exist. Truncated ENTRIES matter too -- a clipped
// tag still matches as a substring, so it half-works and misleads.
static int SplitList(const char* src, char* dst, int maxItems, int itemLen, const char* what) {
    int n = 0, w = 0; bool overLen = false, overCount = false;
    for (const char* c = src; ; c++) {
        if (*c == ',' || *c == 0) {
            if (w > 0) {
                dst[n * itemLen + w] = 0; n++; w = 0;
                if (n >= maxItems) { overCount = (*c != 0 && *(c + 1) != 0); break; }
            }
            if (*c == 0) break;
        } else if (*c != ' ') {
            if (w < itemLen - 1) { dst[n * itemLen + w] = *c; w++; }
            else overLen = true;
        }
    }
    if (overCount)
        TwkLog("[cloth] %s holds more than %d entries -- the rest are IGNORED. Everything past entry "
               "%d does nothing; shorten the list or reuse a tag that already matches.",
               what, maxItems, maxItems);
    if (overLen)
        TwkLog("[cloth] an entry in %s is longer than %d characters and was cut short. It may still "
               "match, but it will also match anything else starting the same way.", what, itemLen - 1);
    return n;
}

// A garment is taken over if its name carries one of the tags AND is not excluded. The exclusion
// list is how a garment that does not suit simulation is left exactly as the game made it.
//
// Recognising garments from their BONES instead was tried (3.16.0) and reverted: it classified some
// things wrongly and broke more than it fixed. Names it is -- a custom garment is included by adding
// a fragment of its name to ClothGarmentTags.
static int GarmentSlot(const char* name) {
    if (!name) return -1;
    for (int e = 0; e < g_nExcl; e++)
        if (g_excl[e][0] && strstr(name, g_excl[e])) return -1;
    for (int t = 0; t < g_nTags; t++)
        if (g_tags[t][0] && strstr(name, g_tags[t])) return t;
    return -1;
}
// Can this garment survive being taken out of the merge?
//
// Un-merging draws the garment as its OWN component, master-posed by the body. That is only valid
// while the garment's rig matches the body's: the merge normally rebakes bone indices and LODs into
// one mesh, and separating it removes that rebake. Two ways a custom garment can fail it, both seen
// on the same replacement trousers -- a mesh authored on a different skeleton (95 bones against the
// body's 70, so the engine's bone map has entries pointing at nothing) and a mesh built with a single
// LOD (the slave follows the body's LOD, and the body has five). Merged, neither matters, which is
// why the garment is perfectly fine in the stock game and only the un-merge brings it down.
//
// CONFIRMED MECHANISM (measured 2026-08-17): DoMerge's output skeleton is the UNION of its sources'
// bones. A properly built garment is weighted to the character's own bones and contributes none, so
// removing it changes the merged skeleton not at all. A garment carrying bones of its own makes the
// merged character 95 bones when present and 70 when stripped -- both counts logged and verified -- and
// because the strip is gated on identity, the REPLAY's copy of the character keeps all 95 while the
// live one has 70. Replay moves per-bone data between characters it assumes are identical, which is
// why replay is where it lands, and why it looks random everywhere else.
//
// So this is the rule, not a workaround: a garment may only be separated if it contributes no bones.
// Structural, not name-based: it costs two reads per source and catches any such asset, not this one.
// A garment that fails is left MERGED -- exactly what the game would do without this mod.
// ClothUnfitProbe -- BISECT ONLY, ships at 0.
//
// ALREADY ANSWERED, 2026-08-17: **level 1 crashes**, and level 1 builds nothing at all -- it only
// strips the garment from the merge. The component, its materials, ShowMaterialSection and the render
// config are all innocent; the STRIP is the damage, so there is no step to fix. Kept because it is the
// tool that established that, and it is the way to test the next odd garment.
//
// Levels are cumulative and follow BuildOrRefreshSlave's own order:
//   0  refuse it (SHIPPING behaviour -- the garment stays merged)
//   1  strip it from the merge, build NO component (garment will be INVISIBLE -- that is expected)
//   2  + build the slave: construct, attach, SetMasterPoseComponent, SetSkeletalMesh
//   3  + CopyRenderConfig and CopyCustomPrimitiveData
//   4  + the material apply
//   5  + ShowMaterialSection  (== the full un-merge, expected to crash)
static int g_unfitProbe = 0;
static bool g_slotUnfit[kMaxGarments] = {};   // was a hardcoded 4 next to a comment saying so

enum { SM_LODINFO_M = 0xf8,   // USkeletalMesh::LODInfo    (count at +8)
       SM_REFSKEL_M = 0x1b0,  // USkeletalMesh::RefSkeleton
       REFSK_BONES  = 0x20,   // FReferenceSkeleton::FinalRefBoneInfo (count at +8)
       SM_RENDERDATA_M = 0x78,  // USkeletalMesh::SkeletalMeshRenderData -- the offset cloth_sim already proves
       SM_SKELETON_M   = 0x80,  // USkeletalMesh::Skeleton (from the PDB member dump)
       UOBJ_FLAGS   = 0x08,   // UObject::ObjectFlags (EObjectFlags)
       UOBJ_OUTER   = 0x20 }; // UObject::OuterPrivate

static bool GarmentFitsBody(void* garment, void* body, int* gBones, int* bBones, int* gLods, int* bLods)
{
    if (!garment || !body) return true;              // nothing to compare against -- do not block
    const int gb = twkI((uint8_t*)garment + SM_REFSKEL_M, REFSK_BONES + 8);
    const int bb = twkI((uint8_t*)body    + SM_REFSKEL_M, REFSK_BONES + 8);
    const int gl = twkI(garment, SM_LODINFO_M + 8);
    const int bl = twkI(body,    SM_LODINFO_M + 8);
    if (gBones) *gBones = gb;  if (bBones) *bBones = bb;
    if (gLods)  *gLods  = gl;  if (bLods)  *bLods  = bl;
    // Implausible reads mean we are looking at the wrong thing; never block on those.
    if (gb <= 0 || bb <= 0 || gl <= 0 || bl <= 0 || gb > 4096 || bb > 4096 || gl > 64 || bl > 64)
        return true;
    // SKELETON only. The LOD counts are logged but NOT judged: USkinnedMeshComponent::UpdateLODStatus_Internal
    // was disassembled to check, and it clamps the master's level to the slave's own mesh
    // (`cmp r14d, edi / cmovl` against LODRenderData.Num()-1), so a one-LOD garment simply stops at its
    // own last LOD. Refusing those turned away custom gear that would have been perfectly fine -- a
    // single LOD is common in modded clothing.
    return (gb <= bb);
}

// Name every garment we were offered and did NOT take, so a player wearing custom gear can see what
// to put in ClothGarmentTags. Without this the loop skipped an unmatched name in silence and there
// was no way to learn it -- custom items rarely carry Session's _UB_/_LB_ naming.
// Once per NAME rather than once per dress: a wardrobe session re-dresses constantly and the useful
// content (the name) is identical every time. Deliberately outside g_logBudget, which is shared with
// the merge diagnostics and would be spent long before a player got to the wardrobe.
static char g_saidName[24][64];
static int  g_nSaid = 0;
static void ReportUnmatchedGarment(const char* nm) {
    if (!nm || !nm[0]) return;
    for (int i = 0; i < g_nSaid; i++) if (!strcmp(g_saidName[i], nm)) return;
    if (g_nSaid >= (int)(sizeof(g_saidName) / sizeof(g_saidName[0]))) return;
    const bool first = (g_nSaid == 0);
    strncpy(g_saidName[g_nSaid], nm, sizeof(g_saidName[0]) - 1);
    g_saidName[g_nSaid][sizeof(g_saidName[0]) - 1] = 0;
    g_nSaid++;
    const char* why = "no ClothGarmentTags match";
    for (int e = 0; e < g_nExcl; e++)
        if (g_excl[e][0] && strstr(nm, g_excl[e])) { why = "matched ClothExclude"; break; }
    // The caveat rides the first line only -- it is advice, not per-item data, and repeating it on
    // every name would bury the names themselves, which are the whole point of the list.
    if (first)
        TwkLog("[cloth] the items below were offered by the wardrobe and NOT simulated. To give one "
               "cloth, add a distinctive part of its name to ClothGarmentTags in SessionTweaks.ini. "
               "The list includes the body and shoes -- tag clothing only.");
    TwkLog("[cloth] not simulated: '%s' -- %s", nm, why);
}

static int  g_okBuild = 1;            // runtime kill-switch, never saved: a build fault stops
                                      // CONSTRUCTION; the exclusion also stops so nothing is lost
static volatile LONG g_merges = 0, g_excluded = 0, g_slaveBuilds = 0;
static int  g_logBudget = 60;

// ---- skater call sites (resolved at install; runtime retaddr is compared against these)
static const int kMaxSites = 10;
static void* g_sites[kMaxSites];
static int   g_nSites = 0;

// ---- the hook -> pump handoff. One pending slot; the newest merge wins (wardrobe scrubs at
// ~300 ms replace it faster than staleness matters).
// The hook publishes an EVENT, deliberately NOT a mesh pointer to compare against: merged output
// meshes are POOLED AND REUSED, so a re-dress very often lands on the SAME USkeletalMesh address.
// Change-detection by pointer identity therefore misses most re-dresses -- measured as a wardrobe
// full of exclusions with the garment stripped and the slave never re-dressed (no top on screen).
// A serial cannot miss: every exclusion bumps it, and the pump acts on the change.
static void* volatile g_pendGarment[kMaxGarments] = {};   // the garment to wear, or null = wear nothing
// HIDE-INSTEAD-OF-STRIP. A garment whose skeleton does not match the body cannot be taken out of the
// merge: the merged character's bone count is the union of what it is wearing, so removing such an item
// CHANGES the character (95 bones -> 70) while the replay's copy keeps all of them, and replay moves
// per-bone data between the two. That was the crash behind the modded trousers.
// Now that the garment is drawn from a mesh the MOD owns, stripping is no longer required: leave the
// merge exactly as the game built it -- character identical to vanilla, replay happy -- and simply hide
// the garment's material on the body so it is not drawn twice, over the top of our own copy.
static bool volatile g_pendHideOnBody[kMaxGarments] = {};
static bool          g_hidOnBody[kMaxGarments] = {};   // already hidden for this character
static void*         g_builtSet[kMaxGarments]  = {};   // the outfit the current build was made for
static volatile LONG  g_pendSerial  = 0;
static volatile LONG  g_appliedSerial = 0;
static double g_pendT = 0.0;
long ClothMerge_GarmentSerial() { return (long)g_appliedSerial; }

// ---- the slave component
static void* g_slave[kMaxGarments] = {};   // our USkeletalMeshComponents, one per garment slot
static void* g_masterComp  = nullptr;   // the body component our garments follow -- a master-posed
                                        // component has no bones of its own, they live here
static void* g_slaveSkater = nullptr;   // the skater they were built on (map change invalidates ALL)

// The settled half of this module's settings; see the note on kRetiredKeys in cloth_sim.cpp for why
// they are code now. g_unmerge, g_ownMesh, g_requireClothMat, g_matSwap and g_driveMulti are the
// pipeline itself -- with any of them off, cloth either does nothing or does it wrongly, so there was
// never a second value worth offering. g_recapture is the rebuild path that shipped looping.
// g_unfitProbe was a bisect knob whose upper levels crash by design.
void ClothMerge_ReadConfig(const char* buf) {
    char tags[512], excl[512];
    TwkIniStr(buf, "ClothGarmentTags", tags, sizeof(tags), "_UB_,_LB_");
    TwkIniStr(buf, "ClothExclude",     excl, sizeof(excl), "");
    g_nTags = SplitList(tags, &g_tags[0][0], kMaxGarments, kTagLen, "ClothGarmentTags");
    g_nExcl = SplitList(excl, &g_excl[0][0], kMaxExcl, 40, "ClothExclude");
    // ClothTintPerGarment is retired with the tint feature that consumed it: the master flag
    // gating every tint application is off for good, so these rules parsed into a gain nothing
    // applied -- a setting that silently does nothing, which is the whole class being removed.
    if (g_nTags <= 0) { strcpy(g_tags[0], "_UB_"); g_nTags = 1; }
    TwkLog("[cloth] config: tags='%s' (%d) exclude='%s' (%d)", tags, g_nTags, excl, g_nExcl);
}
void ClothMerge_SaveConfig(char* buf, size_t cap) {
    // Only what is still configurable -- the garment tag lists and the tint rules. See the note on
    // ClothMerge_ReadConfig for why the rest is code now.
    {
        char tags[128] = {}, excl[512] = {};
        for (int i = 0; i < g_nTags; i++) { if (i) strcat(tags, ","); strcat(tags, g_tags[i]); }
        for (int i = 0; i < g_nExcl; i++) { if (i) strcat(excl, ","); strcat(excl, g_excl[i]); }
        TwkIniSetStr(buf, cap, "ClothGarmentTags", tags);
        TwkIniSetStr(buf, cap, "ClothExclude",     excl);
    }
}
// The garment tag and exclusion lists SURVIVE a reset, deliberately.
//
// Reset Defaults restores tunables -- values with a known-good setting that a user can always move
// back. These two are not that: they are names the user typed in, found by reading which garments the
// log said were not simulated. Nothing can regenerate them, so clearing them destroyed work silently
// and unrecoverably (field report: custom shirt lost its physics an hour after a reset, with the
// cause a session and a half earlier). Clearing exclusions is worse still -- it re-enables the very
// garment someone excluded for misbehaving.
void ClothMerge_ResetDefaults() {
    g_unmerge = 1; g_okBuild = 1;
    if (g_nTags > 2 || g_nExcl > 0)
        TwkLog("[cloth] reset: keeping your %d garment tag(s) and %d exclusion(s) -- reset restores "
               "settings, and these are names only you can know. Edit them in SessionTweaks.ini.",
               g_nTags, g_nExcl);
}

static double CmNow() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ---- multi-match scanner (TwkScanExe returns only the first hit; the LOAD and APPLY site sigs
// legitimately match a known family of addresses, all of which are ours).
static int ScanAllExe(const char* sig, void** out, int maxOut) {
    uint8_t bytes[64]; bool wild[64]; int n = 0;
    for (const char* p = sig; *p && n < 64; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?' ) { wild[n] = true; bytes[n] = 0; }
        else {
            auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0';
                                           if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                                           if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; };
            const int hi = hex(p[0]), lo = hex(p[1]);
            if (hi < 0 || lo < 0) return 0;
            wild[n] = false; bytes[n] = (uint8_t)((hi << 4) | lo);
        }
        n++;
        while (*p && *p != ' ') p++;
    }
    if (!n) return 0;
    int found = 0;
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections && found < maxOut; s++) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec[s].VirtualAddress;
        const size_t len = sec[s].Misc.VirtualSize;
        for (size_t i = 0; i + n <= len && found < maxOut; i++) {
            int j = 0;
            for (; j < n; j++) if (!wild[j] && p[i + j] != bytes[j]) break;
            if (j == n) out[found++] = p + i;
        }
    }
    return found;
}

static bool IsSkaterSite(const void* ret) {
    for (int i = 0; i < g_nSites; i++) if (g_sites[i] == ret) return true;
    return false;
}

// ------------------------------------------------------------------ the hook: exclusion at the source
static void hkDoMerge(void* self, void* refPose) {
    InterlockedIncrement(&g_merges);
    const void* ret = _ReturnAddress();
    void* excludedGarment[kMaxGarments] = {};
    void* unfitGarment[kMaxGarments]    = {};   // worn from our own copy, hidden on the body, NOT stripped
    bool  skaterSiteSeen  = false;
    __try {
        void* data = twkP(self, MM_SRC_LIST);
        int   num  = twkI(self, MM_SRC_LIST + 8);
        // OURS = a known skater dress path AND the actor being dressed is the local player's.
        // Without the identity half this strips preview characters and co-op peers (see the
        // RefreshVisuals note above). An unidentified dress is simply left alone.
        void* mine = CatchTweaks_Skater();
        const bool ours = IsSkaterSite(ret) && mine && t_dressing == mine;
        skaterSiteSeen = ours;
        if (g_unmerge && g_okBuild && ours && data && num > 1 && num <= 64) {
            char nm[64];
            // The body among the sources, found BEFORE anything is stripped: it is what every garment
            // has to fit, and once a source is removed it can no longer be compared against. Named
            // rather than guessed at -- Session's base bodies are 'AMXX_FullBody_<character>_Base'.
            // No body found (a renamed or replaced base) simply means no structural check.
            void* bodyMesh = nullptr;
            for (int i = 0; i < num; i++) {
                void* m = twkP(data, i * 8);
                char bn[64];
                if (m && CatchSound_ObjName(m, bn, sizeof(bn)) && strstr(bn, "FullBody")) {
                    bodyMesh = m; break;
                }
            }
            // The body mesh itself must never be taken, so never strip the last source.
            for (int i = 0; i < num && num > 1; ) {
                void* mesh = twkP(data, i * 8);
                int slot = -1;
                const bool named = (mesh && CatchSound_ObjName(mesh, nm, sizeof(nm)));
                if (named) slot = GarmentSlot(nm);
                if (slot < 0) ReportUnmatchedGarment(named ? nm : nullptr);
                if (slot < 0 || slot >= kMaxGarments || excludedGarment[slot]) { i++; continue; }
                // A garment whose rig does not match the body must stay merged -- separating it hands
                // the renderer a component it cannot draw, and the game dies later somewhere else
                // entirely (observed: garbage collection and mesh-draw-command building, on worker
                // threads, at random, with cloth switched fully off).
                {
                    int gb = 0, bb = 0, gl = 0, bl = 0;
                    const bool fits = GarmentFitsBody(mesh, bodyMesh, &gb, &bb, &gl, &bl);
                    if (slot < kMaxGarments) g_slotUnfit[slot] = !fits;
                    if (!fits) {
                        static char saidUnfit[kMaxGarments][64] = {};
                        if (strncmp(saidUnfit[slot], nm, sizeof(saidUnfit[0]) - 1) != 0) {
                            strncpy(saidUnfit[slot], nm, sizeof(saidUnfit[0]) - 1);
                            saidUnfit[slot][sizeof(saidUnfit[0]) - 1] = 0;
                            TwkLog("[cloth] '%s' is built on a different skeleton to the character "
                                   "(%d bones vs %d; %d LOD(s) vs %d) -- LEFT IN THE MERGE so the "
                                   "character stays exactly as the game built it; it will be hidden on "
                                   "the body and drawn from our own copy instead", nm, gb, bb, gl, bl);
                        }
                        // Worn, but NOT stripped: the pump dresses a slave and hides it on the body.
                        // It counts as SEEN by the capture pass all the same. This branch returns
                        // before the g_capturePass block below that normally records that, so an
                        // unfit garment used to be invisible to the capture -- and if EVERY garment
                        // on the character is unfit, the pass then found nothing, never completed,
                        // and no garment was ever built: cloth silently absent for the whole
                        // session, with the log stopping after "building now". It needed a whole
                        // outfit of custom items to show up, which is why it took until someone
                        // wore a custom shirt AND custom trousers. The capture wants this garment
                        // anyway: it is still in the merged body, which is exactly where the
                        // material match looks.
                        if (g_capturePass) g_seenGarment[slot] = mesh;
                        unfitGarment[slot] = mesh;
                        i++; continue;
                    }
                }
                const bool canCloth = MaterialSupportsCloth(mesh);
                if (!canCloth && g_matSwap && g_clothMaster && g_midCreate && g_setMaterial) {
                    // handled below by giving the slave a cloth-capable stand-in material
                } else if (g_requireClothMat && !canCloth && ClothSim_NeedsClothMaterial()) {
                    if (g_logBudget > 0) { g_logBudget--;
                        TwkLog("[cloth] '%s' has no cloth support in its material -- left as normal "
                               "clothing (simulating it would draw the default checkerboard)", nm); }
                    i++; continue;
                }
                // Remove entry i from the merge's OWN copy of the source array. Shrinking num is
                // safe -- the allocation is untouched, DoMerge just sees one fewer source. Do NOT
                // advance i: the next source has slid into this slot.
                if (g_capturePass) {
                    // Observe only: the game must dress normally this once so it produces the
                    // configured material we are going to borrow.
                    g_seenGarment[slot] = mesh;
                    i++;
                    continue;
                }
                memmove((uint8_t*)data + (size_t)i * 8, (uint8_t*)data + (size_t)(i + 1) * 8,
                        (size_t)(num - 1 - i) * 8);
                num--;
                *(int*)((uint8_t*)self + MM_SRC_LIST + 8) = num;
                excludedGarment[slot] = mesh;
                InterlockedIncrement(&g_excluded);
                if (g_logBudget > 0) { g_logBudget--;
                    TwkLog("[cloth] excluded '%s' (slot %d) from a skater merge -- rebuild pending", nm, slot); }
            }
        } else if (g_logBudget > 0 && data && num > 0 && num <= 64) {
            g_logBudget--;
            const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
            TwkLog("[cloth] merge #%d from exe+0x%llx (%s): %d sources", (int)g_merges,
                   (unsigned long long)((const uint8_t*)ret - base),
                   ours ? "OURS" : (IsSkaterSite(ret) ? "another skater" : "other"), num);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { memset(excludedGarment, 0, sizeof(excludedGarment)); }

    __try { ((DoMergeFn)g_orig)(self, refPose); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        TwkLog("[cloth] caught fatal in DoMerge -> recovered (merge output may be incomplete)");
    }

    // Publish the outcome of EVERY skater-site merge: a garment to wear, or null meaning this
    // re-dress put the garment back in the body mesh and the slave must render nothing.
    if (skaterSiteSeen) {
        for (int i = 0; i < kMaxGarments; i++) {
            // An unfit garment is worn the same way; only the "was it stripped" answer differs, and
            // that decides whether the body still draws it.
            g_pendGarment[i]     = excludedGarment[i] ? excludedGarment[i] : unfitGarment[i];
            g_pendHideOnBody[i]  = (excludedGarment[i] == nullptr && unfitGarment[i] != nullptr);
        }
        g_pendT = CmNow();
        InterlockedIncrement(&g_pendSerial);
    }
}

// ------------------------------------------------------------------ the slave build (PUMP ONLY)
// Is this component still a live component of THIS character?
//
// A wardrobe change rebuilds the character's components, taking ours with them -- and a destroyed
// UObject's memory gets reused, so the cached pointer stays non-null and reads return whatever moved
// in. Reusing one silently produced a garment with no physics for the rest of the session (the pump
// read its "mesh" and got 0198040428091800), and hiding through a stale BODY component put the hide on
// something else entirely, which is what kept taking the shoes.
// Checked by ownership rather than by reading a field and hoping: a live slave's Outer is the skater
// we are dressing. Anything else -- unreadable, or owned by someone else -- means rebuild.
static bool ComponentStillOurs(void* comp, void* skater) {
    if (!comp || !skater) return false;
    bool ok = false;
    __try {
        char cn[96];
        // Literals: this sits above the offset enums. 0x10 = UObject::ClassPrivate (UOBJ_CLASS),
        // 0x20 = UObject::OuterPrivate (UOBJ_OUTER).
        void* cls = twkP(comp, 0x10);
        ok = (twkP(comp, 0x20) == skater)
          && cls && CatchSound_ObjName(cls, cn, sizeof(cn))
          && strstr(cn, "SkeletalMeshComponent") != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// EVERY caller gets a validated component, not just the dress path.
//
// The component dies AFTER a dress, not during it: the field log has slot 0 dressed correctly and then,
// twenty seconds later, the cloth pump reading 428900002DC4001C out of it. Validating only when we dress
// therefore proved nothing -- for every frame in between, a destroyed component was handed to the
// simulation, which is why one garment lost its physics for the rest of the session and the body hide
// landed on freed memory (the vanishing shoes).
// So the check lives HERE, on the accessor everything goes through. When a slave has gone, it is
// forgotten and a fresh dress is requested, which rebuilds it the way a map change would.
void* ClothMerge_SlaveComponent(int i) {
    if (i < 0 || i >= kMaxGarments) return nullptr;
    void* comp = g_slave[i];
    if (!comp) return nullptr;
    void* skater = CatchTweaks_Skater();
    if (skater && !ComponentStillOurs(comp, skater)) {
        TwkLog("[cloth] slot %d: its garment component is gone (destroyed by a re-dress) -- forgetting "
               "it and asking for a fresh dress", i);
        g_slave[i]     = nullptr;
        g_hidOnBody[i] = false;      // whatever it hid on the body went with it
        g_builtSet[i]  = nullptr;    // force the outfit check to rebuild
        ClothSim_SlaveGone(i);       // drop the simulation state that pointed at it
        return nullptr;
    }
    return comp;
}
int ClothMerge_SlaveCount() { return kMaxGarments; }
void* ClothMerge_MasterComponent() { return g_masterComp; }
void* ClothMerge_SetMasterPoseFn() { return (void*)g_setMaster; }

void* ClothMerge_NewObject(void* cls, void* outer) {
    if (!g_sco || !cls) return nullptr;
    // FStaticConstructObjectParameters: +0 Class, +8 Outer, +0x10 Name (0 = auto-unique),
    // +0x18 SetFlags = RF_Transient so nothing ever tries to persist what we build.
    uint8_t params[64] = {};
    *(void**)(params + 0x00) = cls;
    *(void**)(params + 0x08) = outer;
    *(uint64_t*)(params + 0x10) = 0;
    *(uint32_t*)(params + 0x18) = 0x40;
    void* o = nullptr;
    __try { o = g_sco(params); } __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }
    return o;
}

static void* FindClassByName(const wchar_t* wname, const char* expect) {
    if (!g_staticFind) return nullptr;
    void* o = nullptr;
    __try { o = g_staticFind(nullptr, (void*)(intptr_t)-1, wname, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!o) return nullptr;
    // StaticFindObject returns the FIRST object of ANY class with that name; a UClass's own class
    // is named "Class" -- anything else is some namesake and constructing from it would fault.
    char cls[32];
    if (!CatchSound_ObjName(twkP(o, OBJ_CLASS), cls, sizeof(cls)) || strcmp(cls, expect) != 0) return nullptr;
    return o;
}


// The slave is born with UE's DEFAULTS, but the game CONFIGURES its character mesh -- and the gap is
// visible: a floor decal projected onto the un-merged shirt (field-reported), because
// bReceivesDecals defaults ON while the game turns it off for the character. Rather than hardcode
// individual flags, inherit the master's whole render configuration, so the garment renders exactly
// as the body does (decals, every shadow flag, custom depth/stencil, draw distances, DPG).
// Flags are BITFIELDS packed several to a byte, so this copies BYTES, not bits -- which is also why
// the spans are picked precisely:
// 0x210, 0x211 and 0x217 are EXCLUDED ON PURPOSE. They hold streaming-manager registration bits
// (bAttachedToStreamingManagerAsDynamic/Static, bHandledByStreamingManagerAsDynamic) and a
// collideable-descendants CACHE -- per-component RUNTIME STATE, not configuration. Copying another
// component's registration state is how you get a component the streaming manager thinks it owns
// twice. 0x211 is also collision config, which a garment should not inherit.
static void CopyRenderConfig(void* dst, void* src) {
    if (!dst || !src) return;
    static const int kOff[4] = { 0x200, 0x212, 0x21b, 0x220 };
    static const int kLen[4] = { 0x10,  0x05,  0x02,  0x04  };
    //                            |      |      |      +-- CustomDepthStencilValue (int32)
    //                            |      |      +--------- LightingChannels, CustomDepthStencilWriteMask
    //                            |      +---------------- decals, main/depth pass, owner-see, ray
    //                            |                        tracing, sky captures + ALL shadow flags
    //                            +----------------------- draw distances, DepthPriorityGroup, lightmap
    for (int i = 0; i < 4; i++)
        memcpy((uint8_t*)dst + kOff[i], (const uint8_t*)src + kOff[i], (size_t)kLen[i]);
}


// ---------------------------------------------------------------------------------------------
// Where does the garment's COLOUR live? A red hoodie rendering black is not a lighting problem --
// it is the wrong material. Our slave is a fresh component handed the garment mesh, so it draws
// with that mesh's DEFAULT materials; if the game applies your chosen colour as an override on the
// character's own component, our garment never receives it. Log all three sides once per dress and
// let the answer decide the fix, rather than assuming which it is.
enum { COMP_CUSTOMPRIM     = 0x228,  // UPrimitiveComponent::CustomPrimitiveData (TArray<float>)
       COMP_CUSTOMPRIM_INT = 0x238,  // ...and the cached copy the renderer actually reads
       COMP_OVERRIDE_MATS = 0x450,   // UMeshComponent::OverrideMaterials
       SM_MATERIALS       = 0xd8,    // USkeletalMesh::Materials (FSkeletalMaterial, stride 40)
       SKELMAT_STRIDE     = 40,
       MI_PARENT          = 0xd0,   // UMaterialInstance::Parent
       COMP_LODINFO       = 0x5d0 }; // USkinnedMeshComponent::LODInfo (HiddenMaterials at +0x00)


// A material only has a shader for a given vertex factory if it was authored to be used with it.
// Session ships no cloth, so its clothing materials were almost certainly never flagged for it --
// and a material with no shader for the factory in use is replaced by the engine's default, which
// is the grey checkerboard. Walk the instance chain to the base material and read the flags.
enum { UOBJ_CLASS   = 0x10,   // UObject::ClassPrivate
       MI_PARENT_P  = 0xd0,   // UMaterialInstance::Parent
       MAT_USAGE1   = 0x1f8,  // bit7 (0x80) = bUsedWithSkeletalMesh -- the control
       MAT_USAGE3   = 0x1fa,  // bit6 (0x40) = bUsedWithClothing
       SM_MATS      = 0xd8,   // USkeletalMesh::Materials
       SKELMAT_SZ   = 40,
       SMC_SKELMESH_M = 0x480,   // USkinnedMeshComponent::SkeletalMesh
       MI_SCALARS   = 0xe0,   // UMaterialInstance parameter arrays; each starts with
       MI_VECTORS   = 0xf0,   //   FMaterialParameterInfo { FName Name; ... }
       MI_TEXTURES  = 0x100,
       SCALAR_SZ    = 36, VECTOR_SZ = 48, TEXTURE_SZ = 40 };

// List a material instance's parameters. The upper-body clothing master supports cloth and the
// lower-body one does not, so the cheap way to give trousers cloth is to drive them through the
// upper-body master instead -- but only if the two take the same parameters, or the textures would
// not carry across. That is a question about the actual assets, so ask them.
static void LogParams(void* mi, const char* who) {
    if (!mi) return;
    struct { int off, stride; const char* kind; } sets[3] = {
        { MI_TEXTURES, TEXTURE_SZ, "tex" }, { MI_SCALARS, SCALAR_SZ, "scalar" },
        { MI_VECTORS,  VECTOR_SZ,  "vec" } };
    for (int k = 0; k < 3; k++) {
        void* arr = twkP(mi, sets[k].off);
        const int n = twkI(mi, sets[k].off + 8);
        if (!arr || n <= 0) continue;
        char line[400]; line[0] = 0;
        for (int i = 0; i < n && i < 14; i++) {
            char nm[64];
            uint8_t* pe = (uint8_t*)arr + (size_t)i * sets[k].stride;
            if (!CatchSound_FNameText(pe, nm, sizeof(nm))) continue;
            if (!g_tintName && strcmp(nm, "Tint Modifier") == 0) g_tintName = *(uint64_t*)pe;
            strncat(line, i ? ", " : "", sizeof(line) - strlen(line) - 1);
            strncat(line, nm, sizeof(line) - strlen(line) - 1);
        }
        TwkLog("[mat] %s %s params (%d): %s", who, sets[k].kind, n, line);
    }
}

static void LogMaterialUsage(void* mi, const char* who) {
    char cn[96], nm[96];
    void* cur = mi;
    for (int depth = 0; cur && depth < 8; depth++) {
        void* cls = twkP(cur, UOBJ_CLASS);
        if (!cls || !CatchSound_ObjName(cls, cn, sizeof(cn))) return;
        CatchSound_ObjName(cur, nm, sizeof(nm));
        if (strcmp(cn, "Material") == 0) {
            const uint8_t u1 = *(const uint8_t*)((const uint8_t*)cur + MAT_USAGE1);
            const uint8_t u3 = *(const uint8_t*)((const uint8_t*)cur + MAT_USAGE3);
            TwkLog("[mat] %s base material '%s': usedWithSkeletalMesh=%d usedWithClothing=%d "
                   "(flag bytes %02X %02X)", who, nm, (u1 & 0x80) ? 1 : 0, (u3 & 0x40) ? 1 : 0, u1, u3);
            return;
        }
        cur = twkP(cur, MI_PARENT_P);       // an instance -- keep walking up
    }
    TwkLog("[mat] %s: could not reach a base material", who);
}


// A material only has a shader for the cloth vertex factory if it was built with cloth support. If
// it was not, the renderer has nothing to draw the garment with and substitutes its default grey
// checkerboard -- which is the "no material" look on the trousers. That is decided when the game is
// built and cannot be turned on from here: the shader simply is not in the shipped game. So a
// garment whose material lacks it is left exactly as the game made it, rather than simulated and
// ruined. Session flags its upper-body clothing master but not the lower-body one, so this is per
// garment and has to be asked, not assumed.
static bool MaterialSupportsCloth(void* garmentMesh) {
    if (!garmentMesh) return false;
    void* mats = twkP(garmentMesh, SM_MATS);
    const int n = twkI(garmentMesh, SM_MATS + 8);
    if (!mats || n <= 0) return false;
    bool any = false;
    for (int i = 0; i < n && i < 8; i++) {
        void* cur = twkP((uint8_t*)mats + (size_t)i * SKELMAT_SZ, 0);
        char cn[96];
        for (int depth = 0; cur && depth < 8; depth++) {
            void* cls = twkP(cur, UOBJ_CLASS);
            if (!cls || !CatchSound_ObjName(cls, cn, sizeof(cn))) break;
            if (strcmp(cn, "Material") == 0) {
                if (*(const uint8_t*)((const uint8_t*)cur + MAT_USAGE3) & 0x40) {
                    any = true;
                    if (!g_clothMaster) {          // remember it: this is what a swap parents to
                        g_clothMaster = cur;
                        char mn[96];
                        TwkLog("[mat] cloth-capable master found: %s",
                               CatchSound_ObjName(cur, mn, sizeof(mn)) ? mn : "?");
                    }
                }
                break;
            }
            cur = twkP(cur, MI_PARENT_P);
        }
    }
    return any;
}

static void LogMaterials(void* masterComp, void* garment, void* slaveComp) {
    char nm[96], pn[96];
    void* arr = twkP(masterComp, COMP_OVERRIDE_MATS);
    int   num = twkI(masterComp, COMP_OVERRIDE_MATS + 8);
    TwkLog("[mat] master overrides: %d", num);
    for (int i = 0; i < num && i < 12 && arr; i++) {
        void* mi = twkP(arr, i * 8);
        if (!mi) { TwkLog("[mat]   [%d] (null)", i); continue; }
        void* parent = twkP(mi, MI_PARENT);
        TwkLog("[mat]   [%d] %s  parent=%s", i,
               CatchSound_ObjName(mi, nm, sizeof(nm)) ? nm : "?",
               (parent && CatchSound_ObjName(parent, pn, sizeof(pn))) ? pn : "-");
    }

    // The merged BODY mesh's own materials. Component overrides are empty for clothing, so if the
    // game builds a per-dress material carrying your chosen variant and colours, this is where it
    // has to live -- and it is the thing our separated garment never receives.
    void* bodyMesh = twkP(masterComp, SMC_SKELMESH_M);
    void* bm  = bodyMesh ? twkP(bodyMesh, SM_MATERIALS) : nullptr;
    const int bmn = bodyMesh ? twkI(bodyMesh, SM_MATERIALS + 8) : 0;
    {   // Which body geometry the game hides under clothing. ANSWERED 2026-08-17: nothing --
        // "hidden materials (0): none" -- so the legs are not hidden at the component level and the
        // geometry must simply not be in the merge. (The section walk that used to be here guessed
        // its offsets, faulted, and took the whole un-merge down with it. Don't re-add it blind.)
        static bool once = false;
        if (!once) { once = true;
            void* li  = twkP(masterComp, COMP_LODINFO);
            const int lin = twkI(masterComp, COMP_LODINFO + 8);
            TwkLog("[body] LODInfo entries: %d", lin);
            if (li && lin > 0) {
                void* hid = twkP(li, 0);                 // FSkelMeshComponentLODInfo::HiddenMaterials
                const int hn = twkI(li, 8);
                char buf2[128] = {}; size_t at = 0;
                for (int i = 0; i < hn && i < 16 && hid; i++)
                    at += (size_t)_snprintf_s(buf2 + at, sizeof(buf2) - at, _TRUNCATE, "%s%d",
                                              i ? "," : "", (int)((uint8_t*)hid)[i]);
                TwkLog("[body] hidden materials (%d): %s", hn, hn ? buf2 : "none");
            }
        }
    }
    TwkLog("[mat] merged body mesh materials: %d", bmn);
    for (int i = 0; i < bmn && i < 12 && bm; i++) {
        void* mi2 = twkP((uint8_t*)bm + (size_t)i * SKELMAT_SZ, 0);
        void* pa2 = mi2 ? twkP(mi2, MI_PARENT_P) : nullptr;
        TwkLog("[mat]   body[%d] %s  parent=%s", i,
               (mi2 && CatchSound_ObjName(mi2, nm, sizeof(nm))) ? nm : "(null)",
               (pa2 && CatchSound_ObjName(pa2, pn, sizeof(pn))) ? pn : "-");
    }

    void* gm  = garment ? twkP(garment, SM_MATERIALS) : nullptr;
    int   gnum = garment ? twkI(garment, SM_MATERIALS + 8) : 0;
    TwkLog("[mat] garment mesh materials: %d", gnum);
    for (int i = 0; i < gnum && i < 8 && gm; i++) {
        void* mi = twkP((uint8_t*)gm + (size_t)i * SKELMAT_STRIDE, 0);
        void* parent = mi ? twkP(mi, MI_PARENT) : nullptr;
        TwkLog("[mat]   [%d] %s  parent=%s", i,
               (mi && CatchSound_ObjName(mi, nm, sizeof(nm))) ? nm : "(null)",
               (parent && CatchSound_ObjName(parent, pn, sizeof(pn))) ? pn : "-");
        if (mi) { LogMaterialUsage(mi, "garment"); LogParams(mi, "garment"); }
    }
    TwkLog("[mat] slave overrides: %d", twkI(slaveComp, COMP_OVERRIDE_MATS + 8));

    // The garment already carries its right material, and there is no clothing override on the
    // character to copy -- so the colour has to be arriving some other way. This is the per-component
    // data a material can read directly, which is how one shared clothing material gets tinted per
    // character. Our component starts with none of it.
    const float* mc = (const float*)twkP(masterComp, COMP_CUSTOMPRIM);
    const int    mn = twkI(masterComp, COMP_CUSTOMPRIM + 8);
    char vals[192]; vals[0] = 0;
    for (int i = 0; i < mn && i < 12; i++) {
        char one[20]; snprintf(one, sizeof(one), "%s%.3f", i ? ", " : "", mc ? mc[i] : 0.0f);
        strncat(vals, one, sizeof(vals) - strlen(vals) - 1);
    }
    TwkLog("[mat] master customPrimitiveData: %d [%s] | slave: %d", mn, vals,
           twkI(slaveComp, COMP_CUSTOMPRIM + 8));
}

// FMemory::Malloc -- an array the engine may later resize or free must come from its allocator,
// never our CRT heap. Same function cloth_sim uses.
typedef void* (*MergeMallocFn)(size_t, uint32_t);
static MergeMallocFn g_mmalloc = nullptr;
static const char* SIG_MERGE_MALLOC =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 ?? ?? "
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??";

// Give the slave the same per-component material data the body has, so a shared clothing material
// tints the same way on both. Copied into engine memory: the renderer owns it from here.
static void CopyCustomPrimitiveData(void* dst, void* src) {
    if (!dst || !src || !g_mmalloc) return;
    for (int which = 0; which < 2; which++) {
        const int off = which ? COMP_CUSTOMPRIM_INT : COMP_CUSTOMPRIM;
        const void* sd = twkP(src, off);
        const int   sn = twkI(src, off + 8);
        if (!sd || sn <= 0 || sn > 64) continue;
        void* mem = g_mmalloc((size_t)sn * 4, 16);
        if (!mem) continue;
        memcpy(mem, sd, (size_t)sn * 4);
        *(void**)((uint8_t*)dst + off)       = mem;
        *(int*)((uint8_t*)dst + off + 8)     = sn;
        *(int*)((uint8_t*)dst + off + 12)    = sn;
    }
}


// Give the slave a stand-in material that CAN be simulated, wearing the garment's own textures.
// Values are copied by name; whatever the stand-in master does not have simply keeps its default,
// so a mismatch degrades to "slightly wrong shading" rather than to nothing being drawn.

// Walk a material instance chain down to its base material.
static void* BaseMaterialOf(void* mi) {
    char cn[96];
    for (int depth = 0; mi && depth < 8; depth++) {
        void* cls = twkP(mi, UOBJ_CLASS);
        if (!cls || !CatchSound_ObjName(cls, cn, sizeof(cn))) return nullptr;
        if (strcmp(cn, "Material") == 0) return mi;
        mi = twkP(mi, MI_PARENT_P);
    }
    return nullptr;
}

// After an untouched dress, find the material the game built for each garment. Garments are matched
// by their BASE material -- tops and bottoms each come from their own master, and nothing else on the
// character shares it -- so this needs no assumption about section ordering.
static bool MaterialInstanceSupportsCloth(void* mi);
static bool MaterialInstanceSupportsCloth(void* mi) {
    void* base = BaseMaterialOf(mi);
    return base && (*(const uint8_t*)((const uint8_t*)base + MAT_USAGE3) & 0x40) != 0;
}


// =============================================================================================
// THE GARMENT'S CONFIGURED MATERIAL, READ FROM THE CUSTOMIZATION SYSTEM
//
// Everything before this got a garment's material by BORROWING it off the merged body -- which meant
// the garment had to be merged to be readable, which is why re-reading it after a clothes change
// needed a second merge, and every attempt at that crashed the render thread.
//
// The game already resolves this itself. ACharacterCustomization::MapItemMaterials runs during every
// dress with the item's material list and the garment's own USkeletalMesh, and resolves each entry
// for the COLOURWAY you picked through GetMaterialInterfaceFromItemMaterial(entry, gender, variant).
// Listening to it gives us the exact material, at dress time, for nothing -- no second merge, no
// borrowing, and what comes back is a loaded ASSET rather than a per-dress instance, so it cannot be
// collected out from under us.
//
// It also settles which of a garment's materials is its own: the item names the material SLOT, so a
// mesh whose slot 0 is a placeholder (the sweater's MAT_WheelEmpty) resolves correctly by name.
enum { ITEMMESH_MATERIALS = 0xa0,   // FCustomizationItemMesh::ItemMaterials (Num +0xa8)
       ITEMMAT_STRIDE     = 0x78,   // FCustomizationItemMaterial
       SKELMAT_SLOTNAME   = 0x08 }; // FSkeletalMaterial::MaterialSlotName

typedef void* (*GetMatFromItemMatFn)(void* itemMaterial, uint8_t gender, int variantIdx);
typedef void  (*MapItemMatsFn)(void* self, uint8_t gender, int a3, int variantIdx,
                               void* itemMesh, void* mesh);
static GetMatFromItemMatFn g_getItemMat = nullptr;
static MapItemMatsFn       g_origMapMats = nullptr;
static void*               g_mapMatsAddr = nullptr;
// ACharacterCustomization::MapItemMaterials -- Epic 0x102c1d0 / Steam 0xfec340
static const char* SIG_MAP_ITEM_MATS =
    "44 89 4C 24 20 88 54 24 10 55 56 41 54 41 57 48 83 EC 68 4C 8D 79 50 41 8B D0 49 8B CF 45 8B E0";
// ACharacterCustomization::GetMaterialInterfaceFromItemMaterial -- Epic 0x1027050 / Steam 0xfe71c0
static const char* SIG_GET_ITEM_MAT =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B C9 80 FA 02 ?? ?? 45 85 C0 ?? ?? 44 3B 41 70 "
    "?? ?? 49 63 C0 48 6B D0 68";

// What the last dress resolved, per garment mesh. Last write wins, and we read it immediately after
// the dress that wrote it, so it is always that dress's answer.
// NOTE (unproven, left as-is on request): both fields are raw UObject pointers and nothing clears this
// table -- not a re-dress, a skater change, or a map change. A stale entry would be a freed material
// handed to a live component. Clearing it on the skater change was tried while chasing a random crash
// that turned out to belong to ANOTHER MOD, and was reverted with the rest of that work.
enum { kMatRecords = 24 };
// GENERATION-STAMPED. Both fields are raw UObject pointers with nothing keeping them alive: the
// material the customization resolves is usually a per-dress dynamic instance, so it dies with the
// dress that made it. Kept and handed back later, it goes onto a LIVE component -- and garbage
// collection then walks a freed UMaterialInstance and takes the game down, on a worker thread, minutes
// away from anything to do with clothes. Reproducible by entering the shop twice.
// Stamping each record with the dress it came from means a stale one is never returned: it simply does
// not match the current generation, and gets overwritten in place.
struct GarmentMatRec { void* mesh; void* mat; int matIdx; long gen; };
static GarmentMatRec g_matRec[kMatRecords] = {};

static void LogGarmentMat(const char* what, void* mesh, int idx, void* mat) {
    char gn[96], mn[96];
    TwkLog("[mat] customization %s: '%s' material %d = %s", what,
           CatchSound_ObjName(mesh, gn, sizeof(gn)) ? gn : "?", idx,
           CatchSound_ObjName(mat, mn, sizeof(mn)) ? mn : "?");
}

static void RecordGarmentMat(void* mesh, int matIdx, void* mat) {
    int free = -1;
    for (int i = 0; i < kMatRecords; i++) {
        if (g_matRec[i].mesh == mesh) {
            const bool changed = (g_matRec[i].mat != mat);
            g_matRec[i].mat = mat; g_matRec[i].matIdx = matIdx; g_matRec[i].gen = g_matGen;
            if (changed) LogGarmentMat("changed", mesh, matIdx, mat);
            return;
        }
        if (!g_matRec[i].mesh && free < 0) free = i;
    }
    if (free < 0) free = 0;                       // wrap: the oldest is the least interesting
    g_matRec[free].mesh = mesh; g_matRec[free].mat = mat; g_matRec[free].matIdx = matIdx;
    g_matRec[free].gen  = g_matGen;
    LogGarmentMat("learned", mesh, matIdx, mat);
}

// Which of a garment mesh's material slots is the garment itself. -1 when unknown.
int ClothMerge_GarmentMaterialIndex(void* mesh) {
    mesh = SourceOfOwn(mesh);
    for (int i = 0; i < kMatRecords; i++)
        if (g_matRec[i].mesh == mesh && g_matRec[i].gen == g_matGen) return g_matRec[i].matIdx;
    return -1;
}

// A garment whose own material cannot draw cloth has to be DRIVEN rather than bound: the cloth
// renderer would demand a cloth-capable material and the stand-in cannot reproduce the trousers'
// fabric, which is generated inside their own shader. So those garments keep their material and take
// the simulation through their vertices instead.
bool ClothMerge_GarmentWantsDirect(void* mesh) {
    mesh = SourceOfOwn(mesh);
    int idx = -1;
    void* m = ClothMerge_ConfiguredMaterial(mesh, &idx);
    if (m && !MaterialInstanceSupportsCloth(m)) return true;
    // A garment with more than one section has to be driven too. The cloth renderer would flag EVERY
    // section as cloth, including the skin layer some garments carry (the sweater's hands and nape),
    // and a skin layer cannot wear a cloth material. Driven, nothing is flagged and each section can
    // wear what it should.
    return g_driveMulti && twkI(mesh, SM_MATS + 8) > 1;
}

void* ClothMerge_ConfiguredMaterial(void* mesh, int* outIdx) {
    mesh = SourceOfOwn(mesh);
    for (int i = 0; i < kMatRecords; i++)
        if (g_matRec[i].mesh == mesh) {
            if (g_matRec[i].gen != g_matGen) return nullptr;   // an older dress: that material may be gone
            if (outIdx) *outIdx = g_matRec[i].matIdx;
            return g_matRec[i].mat;
        }
    return nullptr;
}

static void hkMapItemMaterials(void* self, uint8_t gender, int a3, int variantIdx,
                               void* itemMesh, void* mesh) {
    if (g_origMapMats) g_origMapMats(self, gender, a3, variantIdx, itemMesh, mesh);
    __try {
        if (!itemMesh || !mesh || !g_getItemMat) return;
        // ONLY while dressing OUR character. Garment meshes are shared by every character in the
        // game, and this runs for all of them -- the roster presets at startup and the shop's item
        // previews all resolve the SAME mesh to THEIR material. Keyed by mesh alone, the last one to
        // run wins, which is how a hoodie ended up wearing a pro's shirt material and how browsing
        // the shop decided what you were wearing. This is the same identity gate the un-merge uses.
        void* mine = CatchTweaks_Skater();
        if (!mine || t_dressing != mine) {
            static int otherLog = 40;
            if (otherLog > 0) { otherLog--;
                char on[96];
                TwkLog("[mat] customization (not us, ignored): '%s'",
                       CatchSound_ObjName(mesh, on, sizeof(on)) ? on : "?"); }
            return;
        }
        void*     mats  = twkP(itemMesh, ITEMMESH_MATERIALS);
        const int nMats = twkI(itemMesh, ITEMMESH_MATERIALS + 8);
        void*     sm    = twkP(mesh, SM_MATS);
        const int nSm   = twkI(mesh, SM_MATS + 8);
        if (!mats || nMats <= 0 || nMats > 32 || !sm || nSm <= 0 || nSm > 32) return;

        for (int i = 0; i < nMats; i++) {
            uint8_t* entry = (uint8_t*)mats + (size_t)i * ITEMMAT_STRIDE;
            const uint64_t slot = *(uint64_t*)entry;          // the material SLOT this entry dresses
            int idx = -1;
            for (int k = 0; k < nSm; k++)
                if (*(uint64_t*)((uint8_t*)sm + (size_t)k * SKELMAT_STRIDE + SKELMAT_SLOTNAME) == slot)
                { idx = k; break; }
            if (idx < 0) continue;
            void* m = nullptr;
            __try { m = g_getItemMat(entry, gender, variantIdx); }
            __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }
            if (!m) continue;
            RecordGarmentMat(mesh, idx, m);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Only a material INSTANCE has parameter arrays and a parent. A variant can perfectly well be a
// plain material (MAT_AMXX_NoComply_CargoPants is one), and reading instance fields off it is a wild
// read -- it faulted mid-copy, the garment was left with no cloth material, and it rendered as the
// engine's default tile.
static bool IsMaterialInstance(void* m) {
    if (!m) return false;
    void* cls = twkP(m, UOBJ_CLASS);
    char cn[96];
    if (!cls || !CatchSound_ObjName(cls, cn, sizeof(cn))) return false;
    return strstr(cn, "MaterialInstance") != nullptr;
}

// The body's configured skin material. A garment's skin section has to wear this to be
// indistinguishable from the arms underneath -- which is exactly what the merge does with it.
// USkinnedMeshComponent::ShowMaterialSection -- Epic 0x2b91b80 / Steam 0x2b543c0. A per-COMPONENT
// hide (it writes LODInfo[lod].HiddenMaterials and issues the render command itself), so the shared
// garment mesh is never touched -- unlike zeroing a section's triangle count, which crashed a render
// worker.
typedef void (*ShowMatSectionFn)(void* comp, int materialId, int sectionIndex, bool bShow, int lodIndex);
static ShowMatSectionFn g_showMatSection = nullptr;
static const char* SIG_SHOW_MAT_SECTION =
    "44 88 4C 24 20 55 56 57 41 54 41 55 41 56 48 8B EC 48 83 EC 68 48 83 B9 80 04 00 00 00 45 0F B6 E1";

static void* BodySkinMaterial(void* masterComp) {
    void* bodyMesh = twkP(masterComp, SMC_SKELMESH_M);
    void* bm  = bodyMesh ? twkP(bodyMesh, SM_MATERIALS) : nullptr;
    const int bmn = bodyMesh ? twkI(bodyMesh, SM_MATERIALS + 8) : 0;
    void* ov  = twkP(masterComp, COMP_OVERRIDE_MATS);
    const int ovn = twkI(masterComp, COMP_OVERRIDE_MATS + 8);
    char nm[96];
    for (int i = 0; i < bmn && bm; i++) {
        void* mi = twkP((uint8_t*)bm + (size_t)i * SKELMAT_SZ, 0);
        void* base = BaseMaterialOf(mi);
        if (!base || !CatchSound_ObjName(base, nm, sizeof(nm))) continue;
        if (strcmp(nm, "M_Body_Master") != 0) continue;
        void* chosen = (ov && i < ovn) ? twkP(ov, i * 8) : nullptr;
        return chosen ? chosen : mi;
    }
    return nullptr;
}

static void CaptureGarmentMaterials(void* masterComp) {
    void* bodyMesh = twkP(masterComp, SMC_SKELMESH_M);
    void* bm  = bodyMesh ? twkP(bodyMesh, SM_MATERIALS) : nullptr;
    const int bmn = bodyMesh ? twkI(bodyMesh, SM_MATERIALS + 8) : 0;
    void* ov  = twkP(masterComp, COMP_OVERRIDE_MATS);
    const int ovn = twkI(masterComp, COMP_OVERRIDE_MATS + 8);
    if (!bm || bmn <= 0) return;

    for (int slot = 0; slot < kMaxGarments; slot++) {
        void* garment = g_seenGarment[slot];
        if (!garment || g_capturedMat[slot]) continue;
        void* gmats = twkP(garment, SM_MATS);
        const int gnum = twkI(garment, SM_MATS + 8);
        if (!gmats || gnum <= 0) continue;

        // A garment mesh can carry SEVERAL materials and its own is not necessarily the first --
        // the sweater's slot 0 is MAT_WheelEmpty, a placeholder, and taking it produced a stand-in
        // with no textures at all ("0 textures, 0 numbers, 0 colours"). The garment's real material
        // is the one that also appears on the merged body, so try every slot and keep that one.
        for (int g = 0; g < gnum && g < 8 && !g_capturedMat[slot]; g++) {
            void* want = BaseMaterialOf(twkP((uint8_t*)gmats + (size_t)g * SKELMAT_STRIDE, 0));
            if (!want) continue;
            for (int i = 0; i < bmn; i++) {
                void* mi = twkP((uint8_t*)bm + (size_t)i * SKELMAT_SZ, 0);
                if (BaseMaterialOf(mi) != want) continue;
                // the override wins when there is one: that is the configured copy
                void* chosen = (ov && i < ovn) ? twkP(ov, i * 8) : nullptr;
                if (!chosen) chosen = mi;
                g_capturedMat[slot]     = chosen;
                g_capturedFor[slot]     = garment;
                g_garmentMatIdx[slot]   = g;
                g_capturedVariant[slot] = IsMaterialInstance(chosen) ? twkP(chosen, MI_PARENT) : chosen;
                char nm[96];
                TwkLog("[mat] captured the game's material for slot %d (garment material %d): %s",
                       slot, g, CatchSound_ObjName(chosen, nm, sizeof(nm)) ? nm : "?");
                break;
            }
        }
        if (!g_capturedMat[slot])
            TwkLog("[mat] slot %d: none of its %d material(s) matched the body", slot, gnum);
    }
}

// Find a named colour parameter on a material instance.
// The gain to use for this garment: its own rule if it has one, otherwise the global.
static int TintGainFor(const char* garmentName) {
    if (garmentName)
        for (int i = 0; i < g_nTintRules; i++)
            if (g_tintRuleName[i][0] && strstr(garmentName, g_tintRuleName[i]))
                return g_tintRuleGain[i];
    return g_tintGain;
}

static bool FindVecParam(void* mi, const char* want, float out[4]) {
    if (!IsMaterialInstance(mi)) return false;
    void* arr = twkP(mi, MI_VECTORS);
    const int n = twkI(mi, MI_VECTORS + 8);
    char pn[64];
    for (int i = 0; i < n && arr; i++) {
        uint8_t* e = (uint8_t*)arr + (size_t)i * VECTOR_SZ;
        if (CatchSound_FNameText(e, pn, sizeof(pn)) && strcmp(pn, want) == 0) {
            const float* c = (const float*)(e + 0x10);
            out[0]=c[0]; out[1]=c[1]; out[2]=c[2]; out[3]=c[3];
            return true;
        }
    }
    return false;
}

static bool ApplySwapMaterial(void* slaveComp, void* garment, void* captured, int slot, int matIdx) {
    if (!slaveComp || !garment || !g_clothMaster || !g_midCreate || !g_setMaterial) return false;
    // TWO sources, in order. A dynamic material instance only stores what it OVERRIDES, so the
    // captured one holds your colour choices but none of the garment's textures -- taking it alone
    // is what stripped the trousers of their detail. Lay the garment's own values down first, then
    // your choices on top.
    void* mats = twkP(garment, SM_MATS);
    const int nMats = twkI(garment, SM_MATS + 8);
    const int mIdx  = (slot >= 0 && slot < kMaxGarments && g_garmentMatIdx[slot] < nMats)
                    ? g_garmentMatIdx[slot] : 0;
    void* meshMat = (mats && nMats > mIdx)
                  ? twkP((uint8_t*)mats + (size_t)mIdx * SKELMAT_STRIDE, 0) : nullptr;
    if (!meshMat && !captured) return false;

    // WHICH material to copy is the whole ballgame. The mesh is shared across every colourway, so its
    // own material is just the factory default -- for the cargos, literally 'Denim'. The variant you
    // picked is the material the game HANDED the component, and since a dynamic instance stores only
    // what it overrides (usually nothing), the values live on that instance's PARENT.
    //
    // So: mesh default at the bottom, your chosen variant over it, your overrides last. Copying only
    // the bottom layer is what dressed every pair of trousers in denim.
    // Use the remembered VARIANT ASSET and the mesh's own material only. The captured instance is
    // deliberately not touched here: it is a per-dress object that nothing roots unless it was applied,
    // so a slot on this path was reading memory the collector had already taken -- the copy faulted,
    // the garment kept a dead material, and that reached the render thread.
    void* chosen = (slot >= 0 && slot < kMaxGarments) ? g_capturedVariant[slot] : nullptr;
    void* base   = chosen ? chosen : meshMat;
    void* srcs[3] = { meshMat, (chosen && chosen != meshMat) ? chosen : nullptr, nullptr };
    { char n1[96], n2[96];
      TwkLog("[mat] copying from mesh default '%s' then your variant '%s'",
             (meshMat && CatchSound_ObjName(meshMat, n1, sizeof(n1))) ? n1 : "-",
             (chosen  && CatchSound_ObjName(chosen,  n2, sizeof(n2))) ? n2 : "(none)"); }

    void* mid = nullptr;
    __try { mid = g_midCreate(g_clothMaster, slaveComp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { mid = nullptr; }
    if (!mid) { TwkLog("[mat] could not build a stand-in material"); return false; }

    int nt = 0, ns = 0, nv = 0;
    __try {
      // ONLY the garment's own values. The captured material belongs to a different colour system
      // (a mask plus four colours) that the cloth master has no equivalent for, so layering it on
      // top does not translate -- it just corrupts what the base already got right.
      for (int pass = 0; pass < 3; pass++) {
        void* src = srcs[pass];
        if (!IsMaterialInstance(src)) continue;   // a plain material has no parameters to copy
        void* arr = twkP(src, MI_TEXTURES);
        int   n   = twkI(src, MI_TEXTURES + 8);
        for (int i = 0; i < n && arr && g_setTex; i++) {
            uint8_t* e = (uint8_t*)arr + (size_t)i * TEXTURE_SZ;
            void* tex = twkP(e, 0x10);
            if (tex) { g_setTex(mid, *(uint64_t*)e, tex); nt++; }
        }
        arr = twkP(src, MI_SCALARS); n = twkI(src, MI_SCALARS + 8);
        for (int i = 0; i < n && arr && g_setScl; i++) {
            uint8_t* e = (uint8_t*)arr + (size_t)i * SCALAR_SZ;
            g_setScl(mid, *(uint64_t*)e, *(float*)(e + 0x10)); ns++;
        }
        arr = twkP(src, MI_VECTORS); n = twkI(src, MI_VECTORS + 8);
        for (int i = 0; i < n && arr && g_setVec; i++) {
            uint8_t* e = (uint8_t*)arr + (size_t)i * VECTOR_SZ;
            g_setVec(mid, *(uint64_t*)e, e + 0x10); nv++;
        }
      }

      // The garment's colour, onto the stand-in's tint. This is how a garment whose colour lives in a
      // mask-plus-tints system the stand-in has no input for still comes out the right colour.
      //
      // WHICH INSTANCE holds it matters: a dynamic instance stores only what it OVERRIDES, so your
      // chosen colour is on the captured one and the base carries the factory default. Reading the
      // base gave every pair of trousers the default denim regardless of what was being worn.
      //
      // The tint MULTIPLIES, which is why a raw copy drove dark denim to black. Mode 1 keeps only the
      // HUE -- brightened until its strongest channel is full -- so it can tint but never darken.
      char gnm[96];
      const int gain = TintGainFor(CatchSound_ObjName(garment, gnm, sizeof(gnm)) ? gnm : nullptr);
      if (g_tintFromColor && g_tintName && g_setVec && gain != 0) {
          float c[4]; const char* from = nullptr;
          if (FindVecParam(chosen, "Color 1", c))       from = "your chosen variant";
          else if (FindVecParam(meshMat, "Color 1", c)) from = "the mesh default";
          if (from) {
              float t[4] = { c[0], c[1], c[2], 1.0f };
              if (g_tintFromColor == 1) {
                  float mx = t[0] > t[1] ? t[0] : t[1]; if (t[2] > mx) mx = t[2];
                  if (mx > 0.0001f) { t[0] /= mx; t[1] /= mx; t[2] /= mx; }
                  // The stand-in's own base colour is a mid-tone and this tint multiplies it, so the
                  // hue alone comes out a stop or two dark -- tan reads as brown. Lift it back.
                  const float g = (float)gain / 100.0f;
                  t[0] *= g; t[1] *= g; t[2] *= g;
              }
              g_setVec(mid, g_tintName, t);
              TwkLog("[mat] tint %.2f %.2f %.2f from %s (raw %.2f %.2f %.2f, gain %d%%)",
                     t[0], t[1], t[2], from, c[0], c[1], c[2], gain);
          } else TwkLog("[mat] no 'Color 1' anywhere to tint from");

          // If that colour is not the right one, the answer is in here.
          void* arr2 = IsMaterialInstance(captured) ? twkP(captured, MI_VECTORS) : nullptr;
          const int n2 = arr2 ? twkI(captured, MI_VECTORS + 8) : 0;
          char pn3[64];
          for (int i = 0; i < n2 && arr2 && i < 12; i++) {
              uint8_t* e2 = (uint8_t*)arr2 + (size_t)i * VECTOR_SZ;
              const float* v2 = (const float*)(e2 + 0x10);
              if (CatchSound_FNameText(e2, pn3, sizeof(pn3)))
                  TwkLog("[mat]   your instance: '%s' = %.3f %.3f %.3f", pn3, v2[0], v2[1], v2[2]);
          }
      }
      g_setMaterial(slaveComp, matIdx, mid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TwkLog("[mat] faulted while copying material values");
        return false;
    }
    TwkLog("[mat] stand-in cloth material applied: %d textures, %d numbers, %d colours carried over",
           nt, ns, nv);
    return true;
}

// =====================================================================================================
// A GARMENT MESH THE MOD OWNS  (ClothOwnMesh)
//
// THE ROOT CAUSE of the map-change crashes, established by bisection: everything cloth does -- marking
// render sections, creating a cloth vertex buffer, writing pre-skinned vertices, asking the RHI to
// re-upload them -- is done TO THE SHARED WARDROBE MESH, whose lifetime belongs to the game. A map change
// destroys it, our in-flight commands and bound SRVs outlive it, and the renderer dies later releasing
// or drawing them. Both cloth paths crashed, differently, for the same reason. No amount of retiming on
// the game thread can fix that; four attempts proved it.
//
// The fix is to stop touching the shared asset. FSkeletalMeshMerge already builds a skeletal mesh at
// runtime, with real cooked render data, every time the game dresses a character -- and it happily takes
// a single source (the game itself does one-source merges). So: run a one-source merge of the garment
// into a mesh WE created, and give that to the slave component. Cloth is then built on an object whose
// lifetime we control, and the shared asset is never modified at all.
//
// The struct is filled by hand rather than through the constructor: its layout is known from the PDB
// (MergeMesh +0x00, SrcMeshList +0x08, StripTopLODs +0x18, MeshBufferAccess +0x1c, NewRefSkeleton +0x30,
// ForceSectionMapping +0x138, SectionUVTransforms +0x140), everything else is output the merge fills, and
// zeroed TArrays are exactly what a fresh instance holds. DoMerge itself we already have -- it is the
// function we hook, so g_orig is the real one.
enum { SMM_SIZE        = 344,    // sizeof(FSkeletalMeshMerge)
       SMM_MERGEMESH   = 0x00,
       SMM_SRCLIST     = 0x08,   // TArray<USkeletalMesh*>: data, Num, Max
       SMM_STRIPTOPLOD = 0x18,
       SMM_BUFFERACCESS= 0x1c,
       SMM_FORCESECMAP = 0x138,
       SMM_UVTRANSFORM = 0x140 };


static DWORD g_ownExCode = 0;
static void* g_ownExAddr = nullptr;
static void* g_ownExInfo = nullptr;
static int OwnMergeFilter(EXCEPTION_POINTERS* p) {
    if (p && p->ExceptionRecord) {
        g_ownExCode = p->ExceptionRecord->ExceptionCode;
        g_ownExAddr = p->ExceptionRecord->ExceptionAddress;
        g_ownExInfo = (p->ExceptionRecord->NumberParameters >= 2)
                    ? (void*)p->ExceptionRecord->ExceptionInformation[1] : nullptr;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// =====================================================================================================
// THE GC ROOT SET  -- the only thing that actually keeps a runtime-built UObject alive
//
// RF_Public|RF_Standalone do NOT protect an object in a cooked build: they are the editor's asset
// bookkeeping, and shipping GC collects anything unreferenced regardless. Our garment copy loses its
// last referencer the moment its component dies, and GC then takes it AND THE OBJECTS IT OWNS -- proven
// in the field twice: a copy whose memory had been recycled for a UTF-16 string, and later a copy whose
// own fields were all still correct while the game died in USkeletalMesh::GetActiveSocketList walking
// its collected SOCKETS. So a shallow validity check can never be enough; the object has to be a root.
//
// Rooting means setting EInternalObjectFlags::RootSet (1<<30) on the object's FUObjectItem, which lives
// in GUObjectArray. That is a DATA symbol, so it cannot be signature-matched directly -- but the code
// that reads it can be. APedestrianCharacter::InitCharacterVisuals contains the engine's standard
// index->item lookup, and its prologue is a signature verified UNIQUE IN BOTH builds:
//
//     mov rax, [rip + disp]      <- &GUObjectArray.ObjObjects   (48 8B 05 disp32)
//     mov rcx, [rax + rcx*8]     <- chunk = Objects[idx / perChunk]
//     lea rax, [rcx + rdx*8]     <- item  = chunk + idx*24
//
// So: find the function, read the displacement out of that first instruction, and we have the array.
// Layouts are from the PDB, not guessed: FUObjectArray::ObjObjects +0x10; FChunkedFixedUObjectArray
// { Objects +0x00, MaxElements +0x10, MaxChunks +0x18 }; FUObjectItem { Object +0x00, Flags +0x08 },
// stride 24; UObjectBase::InternalIndex +0x0c.
static const char* SIG_INIT_CHAR_VISUALS =
    "48 89 74 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 50 48 8B 81 D0 04 00 00 4C 8B C2 48 8B F9 "
    "48 85 C0 0F 84";

enum { UOBJ_INTERNALINDEX = 0x0c,
       CFOA_OBJECTS       = 0x00,   // FChunkedFixedUObjectArray::Objects (chunk table)
       CFOA_MAXELEMENTS   = 0x10,
       CFOA_MAXCHUNKS     = 0x18,
       UOBJITEM_STRIDE    = 24,
       UOBJITEM_FLAGS     = 0x08,
       INTERNAL_ROOTSET   = 0x40000000 };   // EInternalObjectFlags::RootSet = 1 << 30

static uint8_t* g_objObjects = nullptr;     // &GUObjectArray.ObjObjects

static void ResolveObjectArray() {
    uint8_t* fn = TwkScanExe(SIG_INIT_CHAR_VISUALS);
    if (!fn) { TwkLog("[root] object-array anchor not found -- copies cannot be rooted"); return; }
    // The instruction offset is NOT hardcoded: scan the function for the first `mov rax,[rip+disp32]`.
    // Codegen may place it differently in the two builds, and the result is validated below anyway.
    for (int i = 0; i < 0x120; i++) {
        if (fn[i] == 0x48 && fn[i+1] == 0x8B && fn[i+2] == 0x05) {
            const int32_t disp = *(const int32_t*)(fn + i + 3);
            uint8_t* tgt = fn + i + 7 + disp;
            __try {
                void* chunks   = *(void**)(tgt + CFOA_OBJECTS);
                const int maxEl = *(const int*)(tgt + CFOA_MAXELEMENTS);
                const int maxCh = *(const int*)(tgt + CFOA_MAXCHUNKS);
                // Sanity: a live object array has chunks, a sane capacity, and a whole number per chunk.
                if (chunks && maxCh > 0 && maxEl > 0 && maxEl <= (1 << 26) && (maxEl % maxCh) == 0) {
                    g_objObjects = tgt;
                    TwkLog("[root] object array at %p -- %d objects max across %d chunks (%d per chunk)",
                           tgt, maxEl, maxCh, maxEl / maxCh);
                    return;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    TwkLog("[root] could not read the object array from the anchor -- copies cannot be rooted");
}

// Put an object in the GC root set. Returns false rather than guessing if anything does not add up --
// the lookup must round-trip (the item we compute has to point back at the object we asked about).
static bool RootObject(void* obj, const char* what) {
    if (!obj || !g_objObjects) return false;
    bool ok = false;
    __try {
        uint8_t** chunks = *(uint8_t***)(g_objObjects + CFOA_OBJECTS);
        const int maxEl  = *(const int*)(g_objObjects + CFOA_MAXELEMENTS);
        const int maxCh  = *(const int*)(g_objObjects + CFOA_MAXCHUNKS);
        if (chunks && maxCh > 0 && maxEl > 0) {
            const int perChunk = maxEl / maxCh;
            const int idx = *(const int*)((uint8_t*)obj + UOBJ_INTERNALINDEX);
            if (perChunk > 0 && idx >= 0 && idx < maxEl) {
                uint8_t* chunk = chunks[idx / perChunk];
                if (chunk) {
                    uint8_t* item = chunk + (size_t)(idx % perChunk) * UOBJITEM_STRIDE;
                    if (*(void**)item == obj) {          // it really is this object's slot
                        *(int*)(item + UOBJITEM_FLAGS) |= INTERNAL_ROOTSET;
                        ok = true;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    TwkLog("[root] %s '%s' (%p)", ok ? "ROOTED" : "could NOT root", what ? what : "?", obj);
    return ok;
}

// Print the fields the ENGINE reads off a garment mesh when a component adopts it. Logged both when we
// build a copy and every time one is re-worn, so comparing the two lines names whatever goes bad between
// maps -- ComputeMinLOD and UpdateMasterBoneMap both die reading -1 off a copy that was perfect on the
// first map, and only these fields are involved.
// Does this still look like the mesh we built, or has GC taken it and something else moved in?
// Compared against the SOURCE, which cannot be stale: a real copy has the same bone count, LOD count and
// material count, since it was merged from exactly that mesh. Garbage fails on every count at once.
static bool CopyStillOurs(void* copy, void* src) {
    if (!copy || !src) return false;
    bool ok = false;
    __try {
        const int cBones = twkI((uint8_t*)copy + 0x1b0, 0x20 + 8);
        const int sBones = twkI((uint8_t*)src  + 0x1b0, 0x20 + 8);
        const int cLods  = twkI(copy, 0xf8 + 8), sLods = twkI(src, 0xf8 + 8);
        const int cMats  = twkI(copy, 0xd8 + 8), sMats = twkI(src, 0xd8 + 8);
        void*     cRd    = twkP(copy, 0x78);
        ok = (cBones == sBones) && (cLods == sLods) && (cMats == sMats) &&
             cRd && ((uintptr_t)cRd > 0x10000) && cBones > 0 && cBones < 4096;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

static void LogMeshFields(const char* when, const char* name, void* mesh) {
    void* rd = nullptr; void* skel = nullptr; int lods = -1, bones = -1, mats = -1, minLod = -1;
    __try {
        rd     = twkP(mesh, 0x78);            // SkeletalMeshRenderData
        skel   = twkP(mesh, 0x80);            // Skeleton
        lods   = twkI(mesh, 0xf8 + 8);        // LODInfo.Num
        mats   = twkI(mesh, 0xd8 + 8);        // Materials.Num
        bones  = twkI((uint8_t*)mesh + 0x1b0, 0x20 + 8);   // RefSkeleton.FinalRefBoneInfo.Num
        minLod = twkI(mesh, 0x158);           // MinLod
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    TwkLog("[own] %s '%s' (%p): renderData=%p skeleton=%p LODInfo=%d bones=%d mats=%d minLod=%d",
           when, name, mesh, rd, skel, lods, bones, mats, minLod);
}

static void* OwnGarmentMesh(void* src) {
    if (!src || !g_ownMesh || !g_orig || !g_sco) return nullptr;
    char nm[64];
    if (!CatchSound_ObjName(src, nm, sizeof(nm))) return nullptr;
    const char* srcName = nm;
    // Reused across maps -- now that the copy is ROOTED it genuinely survives, along with everything it
    // owns. The contents check stays as belt-and-braces: if rooting ever fails to resolve, this catches
    // a collected copy instead of handing it to a component.
    for (int i = 0; i < kOwnMeshes; i++)
        if (g_ownCopy[i] && strncmp(g_ownName[i], nm, sizeof(g_ownName[0]) - 1) == 0) {
            if (CopyStillOurs(g_ownCopy[i], src)) {
                g_ownSrc[i] = src;
                LogMeshFields("REUSING", nm, g_ownCopy[i]);
                return g_ownCopy[i];
            }
            TwkLog("[own] '%s' did not survive -- rebuilding (rooting may not have resolved)", nm);
            LogMeshFields("STALE  ", nm, g_ownCopy[i]);
            g_ownCopy[i] = nullptr; g_ownSrc[i] = nullptr; g_ownName[i][0] = 0;
            break;
        }

    // NOTE (was true before rooting): a copy could not be reused across a map at all.
    //
    // Reuse cannot be made safe without real GC rooting. The copy is unreferenced the moment its
    // component dies, and a cooked build's GC then takes it AND EVERYTHING IT OWNS. Checking the mesh's
    // own fields is not enough: the last failure had a copy whose bone/LOD/material counts were all
    // perfect, and the game still died in USkeletalMesh::GetActiveSocketList walking the mesh's SOCKET
    // objects, which had been collected separately. The graph rots piecemeal, so no shallow test can
    // clear it.
    //
    // Building costs ~1 ms per garment and gives a mesh whose lifetime matches the component wearing
    // it -- which is the lifetime we actually want. The table below only remembers copy->source so the
    // material lookups can still resolve; it is not a cache.
    //
    // (The better answer is to add the copy to the GC ROOT SET so it genuinely survives -- that needs
    // GUObjectArray, a data symbol, which cannot be signature-bridged the way functions can.)
    void* cls = FindClassByName(L"SkeletalMesh", "Class");
    if (!cls) { TwkLog("[own] SkeletalMesh class not found -- cannot build our own '%s'", srcName);
                g_ownMesh = 0; return nullptr; }
    // Outer = the SOURCE mesh's own outer (its package), so our build lives exactly as long as the
    // wardrobe asset it came from -- no root-set surgery, and nothing tied to a map or an actor.
    // OUTER = the SkeletalMesh CLASS, not the source asset's package.
    //
    // The package is unloaded with the level, which is why a copy looked alive after a map change (the
    // pointer still read) and then faulted on use. A UClass lives for the whole process and its outer
    // chain ends at /Script/Engine, so a copy owned by it survives every map.
    void* copy = ClothMerge_NewObject(cls, cls);
    if (!copy) { TwkLog("[own] could not construct a mesh for '%s'", srcName); return nullptr; }
    // RF_Public | RF_Standalone: GC must NEVER take this.
    //
    // Letting it be collected is what killed the game: on a map change the copy and its cloth asset
    // became unreferenced, GC took them, and a cloth actor still simulating against that data wrote to
    // a null pointer INSIDE PhysX (hang dump: PhysX3_x64.dll+0x57634, write to 0x0, while our heartbeat
    // showed the solver advancing and actors steady at 2 -- so it was never accumulation, it was
    // collection). Keeping it immortal costs one mesh PER GARMENT for the session -- not per map, which
    // is what leaked before -- because the cache below is keyed on the garment's stable NAME.
    *(uint32_t*)((uint8_t*)copy + UOBJ_FLAGS) |= 0x01u | 0x02u;

    uint8_t  merge[SMM_SIZE] = {};
    void*    srcArr[1] = { src };
    *(void**)(merge + SMM_MERGEMESH)  = copy;
    *(void**)(merge + SMM_SRCLIST)    = srcArr;          // data
    *(int*)  (merge + SMM_SRCLIST + 8)  = 1;             // Num
    *(int*)  (merge + SMM_SRCLIST + 12) = 1;             // Max
    *(int*)  (merge + SMM_STRIPTOPLOD)  = 0;
    // EMeshBufferAccess::ForceCPUAndGPU. Default leaves the merged buffers GPU-only, and every cloth
    // step READS this geometry on the CPU -- positions, indices, skin weights. With Default,
    // BuildRealMeshData faulted part-way through (it logged a winding count, then died).
    *(uint8_t*)(merge + SMM_BUFFERACCESS) = 1;
    // ForceSectionMapping is a POINTER TO a TArray, and the merge reads through it without checking:
    // GenerateNewSectionArray faulted at +0x8a reading address 0x8, which is Num() on a null array.
    // So it gets a real, EMPTY array rather than nothing.
    uint8_t forceSecMap[16] = {};       // TArray<FSkelMeshMergeSectionMapping>: null data, Num 0, Max 0
    *(void**)(merge + SMM_FORCESECMAP)  = forceSecMap;
    *(void**)(merge + SMM_UVTRANSFORM)  = nullptr;   // guarded by the merge; left null until proven otherwise

    bool ok = false;
    __try { ((DoMergeFn)g_orig)(merge, nullptr); ok = true; }
    __except (OwnMergeFilter(GetExceptionInformation())) {
        // WHERE it faulted, not just that it did: the offset names the function through the PDB, which
        // says which field of the hand-built struct is wrong. Guessing at 344 bytes of layout is how
        // this goes round in circles.
        const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
        TwkLog("[own] DoMerge faulted building '%s' -- code 0x%08lx at exe+0x%llx (read %p)",
               srcName, g_ownExCode,
               (unsigned long long)((const uint8_t*)g_ownExAddr - base), g_ownExInfo);
    }
    if (!ok) return nullptr;

    // THE SKELETON. FSkeletalMeshMerge builds a reference skeleton (the bone hierarchy) but never sets
    // the mesh's USkeleton ASSET -- the game's own dress path does that afterwards, so a hand-driven
    // merge leaves it null. Nothing notices while the first component wears the copy; the moment a
    // SECOND component adopts it, UpdateMasterBoneMap goes down the "different skeletons" branch, reads
    // Skeleton, and dies. Measured: our copy had skeleton=0000000000000000 where the source had a real
    // pointer, with every other field identical. The garment shares the character's skeleton by
    // definition -- it is the same rig the merge just copied -- so the source's is the right one.
    *(void**)((uint8_t*)copy + SM_SKELETON_M) = twkP(src, SM_SKELETON_M);

    // Did it actually produce geometry? A copy with no render data would draw nothing, and silently.
    void* rd = twkP(copy, SM_RENDERDATA_M);
    const int bones = twkI((uint8_t*)copy + SM_REFSKEL_M, REFSK_BONES + 8);
    const int mats  = twkI(copy, SM_MATS + 8);
    if (!rd) { TwkLog("[own] '%s': the merge produced no render data -- falling back to the shared mesh",
                      srcName); return nullptr; }
    // Rolling, bounded: the newest few copies are what SourceOfOwn needs to resolve; older entries are
    // dead objects and simply age out.
    int slot = -1;
    for (int i = 0; i < kOwnMeshes; i++) if (!g_ownCopy[i]) { slot = i; break; }
    if (slot < 0) { slot = g_ownNext; g_ownNext = (g_ownNext + 1) % kOwnMeshes; }
    g_ownSrc[slot] = src; g_ownCopy[slot] = copy;
    strncpy(g_ownName[slot], nm, sizeof(g_ownName[0]) - 1);
    g_ownName[slot][sizeof(g_ownName[0]) - 1] = 0;
    strncpy(g_ownName[slot], nm, sizeof(g_ownName[0]) - 1);
    g_ownName[slot][sizeof(g_ownName[0]) - 1] = 0;
    // ROOT IT. Everything the mesh owns -- sockets, our cloth asset, its render data -- is reachable
    // from here, so one root keeps the whole graph alive across maps.
    RootObject(copy, srcName);
    TwkLog("[own] built our own '%s' (%p): %d bones, %d material(s) -- the shared asset is now never "
           "touched", srcName, copy, bones, mats);
    LogMeshFields("BUILT  ", srcName, copy);
    LogMeshFields("source ", srcName, src);      // what a HEALTHY mesh looks like, for comparison
    // Those two lines compare counts only -- bones, LODs, materials -- which have always matched even
    // when a garment rendered wrongly. This compares the DATA: positions, skin weights, bone maps.
    ClothSim_CompareCopyToSource(copy, src, srcName);
    return copy;
}

// Which material slot on the MERGED BODY this garment occupies, so it can be hidden there.
// Same matching the capture already relies on: a garment's real material is the one that also appears
// on the merged body, compared by BASE material so variants and dynamic instances still line up.
// -1 when it cannot be identified -- in which case nothing is hidden and the garment simply draws
// twice, which is a cosmetic fault rather than a broken character.
static int BodyMaterialIndexFor(void* bodyMesh, void* garment) {
    if (!bodyMesh || !garment) return -1;
    void*     gm   = twkP(garment, SM_MATS);
    const int gnum = twkI(garment, SM_MATS + 8);
    void*     bm   = twkP(bodyMesh, SM_MATS);
    const int bnum = twkI(bodyMesh, SM_MATS + 8);
    if (!gm || !bm || gnum <= 0 || bnum <= 0 || gnum > 16 || bnum > 32) return -1;

    // Matching on the ROOT material was far too coarse and hid the wrong thing: it walks up to the
    // base Material, and many garments share one master, so a pair of trousers matched the SHOES and
    // the shoes vanished. Compare the actual instances instead, most specific first, and give up
    // rather than hide something at random -- a garment drawn twice is cosmetic, a vanished item is not.
    for (int g = 0; g < gnum; g++) {
        void* gmat = twkP((uint8_t*)gm + (size_t)g * SKELMAT_STRIDE, 0);
        if (!gmat) continue;
        for (int b = 0; b < bnum; b++)
            if (twkP((uint8_t*)bm + (size_t)b * SKELMAT_SZ, 0) == gmat) return b;   // the same object
    }
    for (int g = 0; g < gnum; g++) {                                   // ...or the same direct parent
        void* gmat = twkP((uint8_t*)gm + (size_t)g * SKELMAT_STRIDE, 0);
        void* gpar = gmat ? twkP(gmat, MI_PARENT_P) : nullptr;
        if (!gpar) continue;
        for (int b = 0; b < bnum; b++) {
            void* bmat = twkP((uint8_t*)bm + (size_t)b * SKELMAT_SZ, 0);
            if (!bmat) continue;
            if (bmat == gpar || twkP(bmat, MI_PARENT_P) == gpar) return b;
        }
    }
    return -1;
}

// Stop the BODY drawing a garment we are drawing ourselves.
//
// HOW ShowMaterialSection ACTUALLY WORKS -- read from the disassembly, because assuming it cost a day:
//   void ShowMaterialSection(int32 MaterialID, int32 SectionIndex, bool bShow, int32 LODIndex)
// It hides the material belonging to the SECTION AT SectionIndex. `MaterialID` is discarded (the
// compiled body zeroes the register). So passing the material index and SectionIndex 0 hides whatever
// section 0 happens to use -- on the merged character that is the SHOES, which is exactly what kept
// vanishing no matter which material we computed.
// Therefore: walk the body's render sections, find the ones whose MaterialIndex is the garment's, and
// hide THOSE by section index. Per-component and through the engine's own call, so the shared mesh is
// untouched.
enum { SMESH_RENDERDATA = 0x78,   // USkeletalMesh::SkeletalMeshRenderData
       SRD_LODARRAY     = 0x00,   // FSkeletalMeshRenderData::LODRenderData (TIndirectArray -> ptrs)
       SLOD_SECTIONS    = 0x10,   // FSkeletalMeshLODRenderData::RenderSections
       SSEC_STRIDE      = 232,    // sizeof(FSkelMeshRenderSection)
       SSEC_MATIDX      = 0x00 }; // FSkelMeshRenderSection::MaterialIndex (uint16)

// Clear every hide on the body before applying this dress's.
//
// THE UNDERLYING BUG, not a shoe fix: ShowMaterialSection records hides in a HiddenMaterials array that
// lives on the COMPONENT and is indexed by MATERIAL INDEX. Change clothes and the merged body mesh is
// rebuilt with its materials in a different order, while the component -- and its flags -- survive. The
// index that meant "the trousers I am drawing myself" now means something else, and that item vanishes.
// It was the shoes because they happened to land on the freed index; it could be any item, and it
// depends on bone counts only because those decide which garment gets hidden in the first place.
// A map change never showed it: a new character means a new component with no flags.
// So: show everything, every dress, then hide what THIS outfit needs. Same discipline the slave
// components already use.
static void ShowAllBodySections(void* masterComp) {
    if (!masterComp || !g_showMatSection) return;
    void*     bodyMesh = twkP(masterComp, SMC_SKELMESH_M);
    uint8_t*  rd    = bodyMesh ? (uint8_t*)twkP(bodyMesh, SMESH_RENDERDATA) : nullptr;
    uint8_t** lods  = rd ? (uint8_t**)twkP(rd, SRD_LODARRAY) : nullptr;
    const int nLods = rd ? twkI(rd, SRD_LODARRAY + 8) : 0;
    if (!lods || nLods <= 0 || nLods > 16) return;
    int shown = 0;
    for (int l = 0; l < nLods; l++) {
        uint8_t*  lod  = lods[l];
        uint8_t*  secs = lod ? (uint8_t*)twkP(lod, SLOD_SECTIONS) : nullptr;
        const int nSec = lod ? twkI(lod, SLOD_SECTIONS + 8) : 0;
        if (!secs || nSec <= 0 || nSec > 64) continue;
        for (int sIdx = 0; sIdx < nSec; sIdx++) {
            const int secMat = *(uint16_t*)(secs + (size_t)sIdx * SSEC_STRIDE + SSEC_MATIDX);
            __try { g_showMatSection(masterComp, secMat, sIdx, true, l); shown++; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    if (shown) TwkLog("[cloth] body: cleared %d section hide(s) from the previous outfit", shown);
}

static void HideGarmentOnBody(void* masterComp, void* garment, int slot) {
    if (!masterComp || !garment || !g_showMatSection) return;
    void* bodyMesh = twkP(masterComp, SMC_SKELMESH_M);
    const int matIdx = BodyMaterialIndexFor(bodyMesh, garment);
    if (matIdx < 0) {
        TwkLog("[cloth] slot %d: could not find this garment among the body's materials -- not hiding "
               "it (it will draw twice, which is cosmetic; hiding the wrong thing is not)", slot);
        return;
    }
    uint8_t*  rd   = bodyMesh ? (uint8_t*)twkP(bodyMesh, SMESH_RENDERDATA) : nullptr;
    uint8_t** lods = rd ? (uint8_t**)twkP(rd, SRD_LODARRAY) : nullptr;
    const int nLods = rd ? twkI(rd, SRD_LODARRAY + 8) : 0;
    if (!lods || nLods <= 0 || nLods > 16) {
        TwkLog("[cloth] slot %d: body has no readable LODs -- not hiding", slot); return;
    }
    int hidden = 0;
    for (int l = 0; l < nLods; l++) {
        uint8_t*  lod  = lods[l];
        uint8_t*  secs = lod ? (uint8_t*)twkP(lod, SLOD_SECTIONS) : nullptr;
        const int nSec = lod ? twkI(lod, SLOD_SECTIONS + 8) : 0;
        if (!secs || nSec <= 0 || nSec > 64) continue;
        for (int sIdx = 0; sIdx < nSec; sIdx++) {
            const int secMat = *(uint16_t*)(secs + (size_t)sIdx * SSEC_STRIDE + SSEC_MATIDX);
            if (secMat != matIdx) continue;
            __try { g_showMatSection(masterComp, matIdx, sIdx, false, l); hidden++; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    g_hidOnBody[slot] = true;
    char mn[96]; void* bmats = twkP(bodyMesh, SM_MATS);
    void* hm = bmats ? twkP((uint8_t*)bmats + (size_t)matIdx * SKELMAT_SZ, 0) : nullptr;
    TwkLog("[cloth] slot %d: hid %d body section(s) using material %d '%s' across %d LOD(s)",
           slot, hidden, matIdx, (hm && CatchSound_ObjName(hm, mn, sizeof(mn))) ? mn : "?", nLods);
}

static bool BuildOrRefreshSlave(void* skater, void* masterComp, void* garment, int slot) {
    if (!g_okBuild || slot < 0 || slot >= kMaxGarments) return false;
    // Bisect: for an UNFIT garment only, stop after the step named by ClothUnfitProbe. A fit garment
    // always gets the whole thing, so a probe session leaves normal clothing completely untouched.
    const int probe = (slot < kMaxGarments && g_slotUnfit[slot] && g_unfitProbe > 0)
                    ? g_unfitProbe : 99;
    if (probe <= 1) {
        static int said1[kMaxGarments] = {};
        if (!said1[slot]) { said1[slot] = 1;
            TwkLog("[probe] slot %d: level 1 -- stripped from the merge, NO component built "
                   "(the garment will be invisible; that is expected)", slot); }
        return false;
    }
    __try {
        // Never reuse a component that did not survive the last dress -- see ComponentStillOurs.
        if (g_slave[slot] && !ComponentStillOurs(g_slave[slot], skater)) {
            TwkLog("[cloth] slot %d: the previous garment component did not survive the re-dress -- "
                   "building a new one", slot);
            g_slave[slot] = nullptr;
            g_hidOnBody[slot] = false;      // whatever it hid on the body went with it
        }
        if (!g_slave[slot]) {
            void* cls = FindClassByName(L"SkeletalMeshComponent", "Class");
            if (!cls) { TwkLog("[cloth] SkeletalMeshComponent class not found -- un-merge off"); g_okBuild = 0; return false; }
            void* world = twkP(masterComp, COMP_WORLD);
            if (!world) { TwkLog("[cloth] master has no world yet -- retrying next re-dress"); return false; }
            // FStaticConstructObjectParameters: Outer = the skater actor (ownership resolves from
            // the outer chain at registration), auto-generated unique name (NAME_None), and
            // RF_Transient (0x40) so nothing ever tries to persist it.
            uint8_t params[64] = {};
            *(void**)(params + 0x00) = cls;
            *(void**)(params + 0x08) = skater;
            *(uint64_t*)(params + 0x10) = 0;            // NAME_None -> unique auto name
            *(uint32_t*)(params + 0x18) = 0x40;         // RF_Transient
            void* comp = g_sco(params);
            if (!comp) { TwkLog("[cloth] StaticConstructObject returned null -- un-merge off"); g_okBuild = 0; return false; }
            // VISUAL-ONLY: a fresh skeletal mesh component ships with the CLASS-DEFAULT
            // collision (a blocking profile), and two garment components colliding with the
            // world rode along with the skater for months -- field-reported as "the ragdoll
            // gets stuck on things / more friction since the cloth work". NoCollision is
            // written into the component's BodyInstance BEFORE registration, so no physics
            // body is ever created for a garment at all.
            *((uint8_t*)comp + 0x2c8 /*BodyInstance*/ + 0x20 /*CollisionEnabled*/) = 0;
            g_regWorld(comp, world, nullptr);
            // Invisible-surface diagnostic: prove whether the garment can collide at all.
            // Registration may re-derive collision state, and a merged mesh may or may not
            // create physics bodies -- read back AFTER registering.
            {
                const int gb = *(int*)((uint8_t*)comp + 0x980 /*Bodies*/ + 8);
                TwkLog("[cloth] slave after register: collisionEnabled=%d physicsBodies=%d",
                       (int)*((uint8_t*)comp + 0x2c8 + 0x20), gb);
            }
            // SnapToTarget on all three axes: a master-posed slave renders from the master's bone
            // buffer, but its component transform should still sit exactly on the body.
            const uint8_t rules[4] = { 2, 2, 2, 0 };    // Location/Rotation/Scale = SnapToTarget, no weld
            g_attach(comp, masterComp, rules, 0 /*no socket*/);
            g_setMaster(comp, masterComp, true);
            g_slave[slot] = comp;
            g_slaveSkater = skater;
            InterlockedIncrement(&g_slaveBuilds);
            TwkLog("[cloth] slave garment component BUILT (%p) for slot %d on skater %p",
                   comp, slot, skater);
        }
        // What the MERGE produced, once per distinct answer. DoMerge's output skeleton is the union of
        // its sources' bones, so a garment carrying bones the body lacks CHANGES this number by being
        // present -- and stripping it changes it back. That is invisible for a normal garment (weighted
        // to the character's own bones, it adds none) and is the difference that matters for one that
        // is not. Cheap, and it is the number to quote when telling an author their item is wrong.
        {
            void* bodyM = twkP(masterComp, SMC_SKELMESH_M);
            const int bodyBones = bodyM ? twkI((uint8_t*)bodyM + SM_REFSKEL_M, REFSK_BONES + 8) : -1;
            static int lastBodyBones = -2;
            if (bodyBones != lastBodyBones) {
                lastBodyBones = bodyBones;
                TwkLog("[cloth] the merged character has %d bones", bodyBones);
            }
        }
        // BEFORE SetSkeletalMesh: that call recreates the render state, which is what publishes
        // these flags to the renderer. Re-applied on every re-dress -- cheap, and it defends
        // against anything the game resets when the garment changes.
        g_masterComp = masterComp;
        if (probe >= 3) {
            CopyRenderConfig(g_slave[slot], masterComp);
            CopyCustomPrimitiveData(g_slave[slot], masterComp);
        }
        // Prefer the material the game itself configured -- that is the one carrying your chosen
        // variant and colours. The stand-in is only for a garment whose own material cannot be
        // simulated at all, and it necessarily loses those choices.
        // The captured material carries your variant and colours, but it is only usable if it can
        // be simulated -- a captured lower-body material still cannot, and handing it over is what
        // put the trousers back to grey squares. So: use it when it works, otherwise fall back to
        // the stand-in and feed THAT the captured values, so the colours still come across.
        // When the garment draws itself from the simulation there is no cloth-capable material
        // requirement at all, so the game's own configured material is always the right answer --
        // no stand-in, and the colours are exactly what you chose.
        // What the customization system resolved for THIS garment on THIS dress. Read fresh every
        // time, which is what makes a clothes change come out right: no capture to go stale, and no
        // second merge to get it. Falls back to the borrowed-from-the-body material if the listener
        // is not running.
        int   cfgIdx = -1;
        void* configured = (probe >= 4) ? ClothMerge_ConfiguredMaterial(garment, &cfgIdx) : nullptr;
        if (configured) {
            g_garmentMatIdx[slot]   = cfgIdx;
            g_capturedVariant[slot] = configured;
        } else {
            // No answer for THIS garment, so anything remembered belongs to the one before it.
            // Forget it: a garment in its own default colourway is wrong, but a garment wearing a
            // DIFFERENT garment's material is nonsense -- which is what the stale value produced.
            g_capturedVariant[slot] = nullptr;
            g_garmentMatIdx[slot]   = 0;
        }
        // Likewise the borrowed fallback: only usable if it was taken from this very garment.
        void* useMat = configured ? configured
                     : (g_capturedFor[slot] == garment ? g_capturedMat[slot] : nullptr);
        {   char gn2[96];
            TwkLog("[mat] slot %d '%s': customization %s", slot,
                   CatchSound_ObjName(garment, gn2, sizeof(gn2)) ? gn2 : "?",
                   configured ? "answered" : "has NO record for this garment"); }

        // A garment mesh can have several sections, and the garment's own is not necessarily the
        // first: the sweater's slot 0 is a skin layer carrying the hands, its slot 1 is the sweater.
        // Writing to 0 put the sweater's material on the hands.
        const int mIdxApply = (cfgIdx >= 0) ? cfgIdx : g_garmentMatIdx[slot];

        // Driven garments draw through the ordinary skinned path, so there is no cloth-capable
        // requirement at all -- they wear exactly what the game chose, which is the entire point.
        const bool driven = ClothSim_DirectEnabled() && ClothMerge_GarmentWantsDirect(garment);
        const bool capturedOk = useMat
                             && (driven || !ClothSim_NeedsClothMaterial()
                                 || MaterialInstanceSupportsCloth(useMat));
        bool applied = (probe < 4);         // level < 4: touch no materials at all, including the fallback
        if (probe >= 4 && capturedOk && g_setMaterial) {
            __try { g_setMaterial(g_slave[slot], mIdxApply, useMat); applied = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        } else if (g_matSwap && !driven) {
            applied = ApplySwapMaterial(g_slave[slot], garment, useMat, slot, mIdxApply);
        }
        // The component MUST come out of here holding a material we just set. If the copy failed it
        // would otherwise keep the one from the previous dress -- and that one can already have been
        // collected, which is a null material reaching the render thread and killing the game.
        if (!applied && g_setMaterial) {
            void* mats2 = twkP(garment, SM_MATS);
            const int n2 = twkI(garment, SM_MATS + 8);
            const int i2 = (g_garmentMatIdx[slot] < n2) ? g_garmentMatIdx[slot] : 0;
            void* own = (mats2 && n2 > i2)
                      ? twkP((uint8_t*)mats2 + (size_t)i2 * SKELMAT_STRIDE, 0) : nullptr;
            // The garment's OWN material is the wrong fallback here: its sections are bound to cloth,
            // and a material not built for cloth draws the engine's default tile. The bare cloth
            // master is plain but it draws.
            void* safe = (own && MaterialInstanceSupportsCloth(own)) ? own : g_clothMaster;
            if (safe) { __try { g_setMaterial(g_slave[slot], mIdxApply, safe); }
                        __except (EXCEPTION_EXECUTE_HANDLER) {} }
            TwkLog("[mat] slot %d fell back to %s", slot,
                   (safe == g_clothMaster) ? "the plain cloth master (its own cannot draw cloth)"
                                           : "the garment's own material");
        }
        // Sections that are not the garment: on a driven garment these draw normally, so give them
        // the body's skin. Left with the asset's placeholder they read as a broken overlay sitting on
        // top of the real hands and neck.
        const int nSlots = twkI(garment, SM_MATS + 8);
        // SHOW EVERYTHING FIRST. Hiding is remembered by the COMPONENT, not by the garment, and a
        // slot's component is reused every time you change clothes. A hide left over from the last
        // garment therefore lands on whatever is worn next -- and on a single-material top that is
        // its ONLY section, so the shirt disappears entirely. Clear the slate on every build; the
        // block below then hides only what THIS garment needs.
        if (g_showMatSection && probe >= 5)
            for (int k = 0; k < 8; k++) {
                __try { g_showMatSection(g_slave[slot], k, 0, true, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        if (driven && nSlots > 1 && probe >= 5) {
            // HIDE the garment's skin layer rather than dress it. One material cannot serve both ends
            // of it -- the hands take the body texture and the neck does not, which is why the neck
            // stayed grey after the material fix. The body draws its own neck and hands underneath,
            // so the layer is pure duplication and the right answer is for it not to draw.
            //
            // Per-COMPONENT, through the engine's own call: it writes HiddenMaterials and issues the
            // render command. The shared mesh is untouched (zeroing triangle counts crashed a worker).
            void* skin = g_showMatSection ? nullptr : BodySkinMaterial(masterComp);
            for (int k = 0; k < nSlots; k++) {
                if (k == mIdxApply) continue;
                if (g_showMatSection) {
                    __try { g_showMatSection(g_slave[slot], k, 0, false, 0); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                } else if (skin && g_setMaterial) {   // fallback: at least make it look like skin
                    __try { g_setMaterial(g_slave[slot], k, skin); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
            }
            TwkLog("[mat] slot %d: %d extra section(s) %s", slot, nSlots - 1,
                   g_showMatSection ? "hidden (the body draws them already)"
                                    : "given the body's skin (no hide function)");
        }
        // Our own build of the garment if we have one -- see OwnGarmentMesh. Everything downstream
        // (cloth asset, section marks, vertex writes) then happens to an object the mod owns.
        void* wear = OwnGarmentMesh(garment);
        g_setSkelMesh(g_slave[slot], wear ? wear : garment, false);
        // Left in the merge (skeleton mismatch): the body still draws it, so hide it there.
        if (g_pendHideOnBody[slot] && !g_hidOnBody[slot])
            HideGarmentOnBody(masterComp, garment, slot);
        char nm[64];
        TwkLog("[cloth] slot %d now wearing '%s' -- the garment is UN-MERGED%s", slot,
               (garment && CatchSound_ObjName(garment, nm, sizeof(nm))) ? nm : "?",
               (probe < 99) ? "  [ClothUnfitProbe: truncated setup]" : "");
        if (probe < 99)
            TwkLog("[probe] slot %d ran at level %d -- render config %s, materials %s, section show/hide %s",
                   slot, probe, (probe >= 3) ? "YES" : "no", (probe >= 4) ? "YES" : "no",
                   (probe >= 5) ? "YES" : "no");
        static int matLogBudget = 4;
        if (matLogBudget > 0) { matLogBudget--; LogMaterials(masterComp, garment, g_slave[slot]); }
        return true;
    } __except (OwnMergeFilter(GetExceptionInformation())) {
        const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
        TwkLog("[cloth] slave build FAULTED -> un-merge disabled for this session (exclusion too)"
               " -- code 0x%08lx at exe+0x%llx (addr %p)", g_ownExCode,
               (unsigned long long)((const uint8_t*)g_ownExAddr - base), g_ownExInfo);
        g_okBuild = 0;
        g_slave[slot] = nullptr;                        // do not touch a half-built component again
        return false;
    }
}

void ClothMerge_PumpFrame() {
    if (!g_start) return;
    __try {
        void* skater = CatchTweaks_Skater();
        if (!skater) return;
        // Map switch / respawn: the old actor took our component with it. Forget, never touch.
        // Map switch / respawn: the old actor took our component with it. Forget, never touch. The
        // captured materials went with it too -- they were made for THAT character -- so the whole
        // two-pass has to run again on the new one. Keeping them was why a map change lost cloth
        // until the game was restarted.
        if (g_slaveSkater && skater != g_slaveSkater) {
            // FORGET THE COMPONENTS FIRST, then release. The old actor took them with it, and the
            // release rebuilds each live component's render proxy -- doing that to a component whose
            // actor is gone is a wild read on the RENDER thread, which is a hard crash on the next
            // map load. Clearing them here makes "never touch the old actor's components" true by
            // construction; the release still restores the MESHES, which are shared and outlive it.
            for (int i = 0; i < kMaxGarments; i++) {
                g_slave[i] = nullptr;
                g_capturedMat[i] = nullptr; g_capturedFor[i] = nullptr; g_seenGarment[i] = nullptr;
                g_hidOnBody[i] = false;      // new body component -- the hide is per-component
                g_builtSet[i]  = nullptr;
            }
            ClothSim_ReleaseAll();     // these meshes are shared, and the new character merges them
            g_logBudget = 60;          // a new character is worth describing again (replay, respawn)
            g_slaveSkater = nullptr;
            g_masterComp  = nullptr;
            g_captureDone = false;
            InterlockedExchange(&g_capturePass, 1);
            TwkLog("[cloth] new character -- dressing from scratch");
        }

        // The load dress does NOT go through RefreshVisuals (it runs from its own path), so at spawn
        // there is no identified dress to act on and the garment would stay merged until the player
        // happened to visit the wardrobe. Ask the game to re-dress us once instead: our identity
        // hook then publishes the skater, the merge is recognised as ours, and the slave is built.
        // Bounded and one-shot per skater -- a re-dress we requested must never become a loop.
        static void* armedFor = nullptr;
        static int   armTries = 0;
        static double lastTry = 0.0;
        // Whether we currently have anything built -- nothing else. Seeding this from "the capture
        // has happened" latched true for the rest of the session, so after a map change the request
        // below could never fire again and the garments stayed merged until a restart.
        bool haveAny = false;
        for (int i = 0; i < kMaxGarments; i++) if (g_slave[i]) haveAny = true;
        if (g_unmerge && g_okBuild && g_origRefresh && !haveAny) {
            if (armedFor != skater) { armedFor = skater; armTries = 0; lastTry = 0.0; }
            // Wait until the skater actually has its mesh, and leave a beat between attempts. The
            // first cut fired all three on three consecutive frames at spawn, which is no retry at
            // all if the character simply is not ready yet.
            const bool ready = twkP(skater, CH_MESH) != nullptr;
            if (ready && armTries < 5 && CmNow() - lastTry > 0.5) {
                armTries++;
                lastTry = CmNow();
                TwkLog("[cloth] no garment component yet -- requesting a re-dress (attempt %d)", armTries);
                // Call the ORIGINAL, but publish identity around it ourselves. Going through the
                // trampoline skips our RefreshVisuals hook, so t_dressing would stay null and the
                // merge inside would not be recognised as ours -- which is why the garment only
                // ever un-merged after a trip to the wardrobe (a dress the GAME initiated).
                void* prev = t_dressing;
                t_dressing = skater;
                __try { ((RefreshVisualsFn)g_origRefresh)(skater); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                t_dressing = prev;
                // The hook ran inside that call and published a pending garment; fall through and
                // apply it on this same tick.
            }
        }

        const LONG serial = g_pendSerial;
        if (serial == g_appliedSerial) return;            // no re-dress since we last acted
        if (CmNow() - g_pendT > 5.0) { g_appliedSerial = serial; return; }   // stale; drop it

        void* meshComp = twkP(skater, CH_MESH);
        if (!meshComp) return;                            // not ready -- retry next frame
        g_appliedSerial = serial;

        // First dress of the session: the game has just dressed normally, so the configured
        // materials exist right now. Take them, then ask for one more dress -- that one separates
        // the garments out, and the slaves wear what we just captured.
        if (g_capturePass && !g_captureDone) {
            bool any = false;
            for (int i = 0; i < kMaxGarments; i++) if (g_seenGarment[i]) any = true;
            // Finding nothing is a DEAD END, not a wait: this pass only ever sees what the merge hook
            // recorded, and if that was empty once it will be empty every time. It used to return here
            // in silence forever. Say it once -- an unbuildable state that produces no log entry cost
            // an afternoon to find, twice.
            if (!any) {
                static int quiet = 0;
                if (++quiet == 30)
                    TwkLog("[cloth] the material capture has nothing to work with -- no garment was "
                           "recorded during the dress, so nothing can be built. Cloth is OFF for this "
                           "character. If your clothing is all custom/replacement gear, this is a bug "
                           "-- please report the log.");
            }
            if (any) {
                __try { CaptureGarmentMaterials(meshComp); }
                __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[mat] capture faulted"); }
                g_captureDone = true;
                InterlockedExchange(&g_capturePass, 0);
                TwkLog("[cloth] materials captured -- dressing again with the garments separated");
                void* prev = t_dressing;
                t_dressing = skater;
                __try { ((RefreshVisualsFn)g_origRefresh)(skater); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                t_dressing = prev;
            }
            return;                       // nothing to build yet; the next dress does the work
        }

        // Changed clothes: the capture belongs to the garment we took it FROM, so wearing something
        // else means the slave would keep the old one's textures and colours. Run the two-pass again
        // -- the game must dress normally once more before its configured material exists to borrow.
        // Bounded by construction: a capture that fails leaves capturedFor null, which cannot match
        // again, so this can never become a loop.
        // ANY change to the garment SET rebuilds from scratch, exactly as a map change does.
        //
        // This used to trigger only when a slot that already had a garment got a different one, which
        // missed the common case: garments are slotted by TAG, so changing trousers can move them from
        // one slot to another (slot 1 emptied, slot 3 appeared) and neither slot looked "changed".
        // Everything then limped along on incremental updates -- the unchanged shirt kept a cloth asset
        // nobody was driving, the body kept a hide belonging to the previous garment -- and that
        // half-updated state is what crashed on entering replay.
        //
        // Rebuilding wholesale is what already works on every map change. It costs one extra dress.
        bool setChanged = false;
        for (int i = 0; i < kMaxGarments && !setChanged; i++)
            if (g_pendGarment[i] != g_builtSet[i]) setChanged = true;
        // ...but only against a set we actually built. The capture pass dresses NORMALLY -- no
        // garment is excluded, so it publishes an EMPTY set and returns without recording one. The
        // separated dress that follows it therefore always differs from the empty baseline and looks
        // like a wardrobe change, which re-armed the capture and re-dressed, forever: ~30 rebuilds a
        // second, clothes flashing, animation destroyed. It reached release because the machine it
        // was written on had ClothRecapture=0 saved from before the default flipped, so the branch
        // never ran there. With nothing built there is also nothing to tear down -- the fall-through
        // below builds the garments and records the set, which is exactly what this branch would
        // have asked for.
        bool hadBuild = false;
        for (int i = 0; i < kMaxGarments && !hadBuild; i++) if (g_builtSet[i]) hadBuild = true;
        // Belt and braces after the above shipped: a rebuild asks the game to dress twice, so any
        // future path that mistakes its own re-dress for a wardrobe change melts the character
        // rather than degrading. Nobody changes clothes six times in three seconds; a burst like
        // that is a loop, and dropping back to the incremental path leaves cloth imperfect instead
        // of leaving the player unable to play.
        static double rebuildTimes[6] = {};
        static int    rebuildAt = 0;
        static bool   runawaySaid = false;
        bool runaway = false;
        if (g_recapture && hadBuild && setChanged && g_captureDone && !g_capturePass) {
            const double now = CmNow();
            const double oldest = rebuildTimes[rebuildAt];   // the 6th most recent
            if (oldest > 0.0 && now - oldest < 3.0) {
                runaway = true;
                if (!runawaySaid) {
                    runawaySaid = true;
                    TwkLog("[cloth] REFUSING to rebuild again -- 6 outfit rebuilds in under 3 s is a "
                           "loop, not a wardrobe. Falling back to updating in place; cloth may be "
                           "imperfect until the next map change. Please report this log.");
                }
            } else {
                rebuildTimes[rebuildAt] = now;
                rebuildAt = (rebuildAt + 1) % 6;
            }
        }
        if (g_recapture && hadBuild && setChanged && g_captureDone && !g_capturePass && !runaway) {
            char n1[96];
            void* g2 = nullptr;
            for (int i = 0; i < kMaxGarments && !g2; i++)
                if (g_pendGarment[i] != g_builtSet[i]) g2 = g_pendGarment[i];
            TwkLog("[cloth] the outfit changed ('%s') -- rebuilding from scratch, the same way a map "
                   "change does", (g2 && CatchSound_ObjName(g2, n1, sizeof(n1))) ? n1 : "nothing");
            // Stop drawing the garments first, THEN unmark the meshes -- a live render proxy must
            // never be left pointing at cloth data we have just taken away.
            for (int i = 0; i < kMaxGarments; i++)
                if (g_slave[i]) { __try { g_setSkelMesh(g_slave[i], nullptr, false); }
                                  __except (EXCEPTION_EXECUTE_HANDLER) {} }
            ClothSim_ReleaseAll();
            // Drop EVERY slot's capture, not just the one that changed. A captured material is a
            // per-dress object; the only thing keeping one alive is our having applied it, so a slot
            // that merely READS its capture (the stand-in path) holds a pointer the collector is free
            // to take. Re-reading them all together keeps every capture from the same dress.
            for (int i = 0; i < kMaxGarments; i++) {
                g_capturedMat[i] = nullptr; g_capturedVariant[i] = nullptr;
                g_capturedFor[i] = nullptr; g_seenGarment[i] = nullptr;
            }
            for (int i = 0; i < kMaxGarments; i++) {
                g_hidOnBody[i] = false;    // the body's hide belongs to the OLD outfit
                g_builtSet[i]  = nullptr;
            }
            g_captureDone = false;
            InterlockedExchange(&g_capturePass, 1);
            void* prev = t_dressing;
            t_dressing = skater;
            __try { ((RefreshVisualsFn)g_origRefresh)(skater); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            t_dressing = prev;
            return;
        }

        // Every dress starts from a body with nothing hidden -- see ShowAllBodySections.
        ShowAllBodySections(meshComp);
        for (int i = 0; i < kMaxGarments; i++) g_hidOnBody[i] = false;

        for (int slot = 0; slot < kMaxGarments; slot++) {
            void* garment = g_pendGarment[slot];
            g_builtSet[slot] = garment;              // what this build is for -- see the outfit check
            if (garment) {
                BuildOrRefreshSlave(skater, meshComp, garment, slot);
            } else if (g_slave[slot]) {
                // This re-dress left that garment in the body mesh, so the slave must render
                // nothing -- which is what keeps a missed case at "merged normally" rather than a
                // doubled garment.
                __try { g_setSkelMesh(g_slave[slot], nullptr, false); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                TwkLog("[cloth] slot %d not excluded this dress -- emptied (no double garment)", slot);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ClothMerge_Install() {
    // ORDER: resolve EVERYTHING before the hook goes live. The first field round proved the
    // race: the skater's boot dress fired between MH_EnableHook and the site scans finishing, ran
    // through the hook with an empty site table, and was classified "other" -- the one merge of
    // the session went unexcluded. Sites and functions first; the detour is the last thing armed.
    ResolveObjectArray();      // the GC root set -- see the note; resolved before anything is built
    g_nSites += ScanAllExe(SIG_SITE_DRESS,         g_sites + g_nSites, kMaxSites - g_nSites);
    g_nSites += ScanAllExe(SIG_SITE_REFRESH,       g_sites + g_nSites, kMaxSites - g_nSites);
    g_nSites += ScanAllExe(SIG_SITE_WARDROBE_PREV, g_sites + g_nSites, kMaxSites - g_nSites);
    g_nSites += ScanAllExe(SIG_SITE_WARDROBE_IN,   g_sites + g_nSites, kMaxSites - g_nSites);
    g_staticFind  = (StaticFindFn)TwkScanExe(SIG_STATIC_FIND);
    { uint8_t* scoAnchor = TwkScanExe(SIG_SCO);
      g_sco = scoAnchor ? (ScoFn)(scoAnchor - kScoAnchorOff) : nullptr; }
    g_regWorld    = (RegWorldFn)TwkScanExe(SIG_REG_WORLD);
    g_attach      = (AttachFn)TwkScanExe(SIG_ATTACH);
    g_setSkelMesh = (SetSkelMeshFn)TwkScanExe(SIG_SET_SKELMESH);
    g_setMaster   = (SetMasterFn)TwkScanExe(SIG_SET_MASTER);
    g_mmalloc     = (MergeMallocFn)TwkScanExe(SIG_MERGE_MALLOC);
    g_midCreate   = (MidCreateFn)TwkScanExe(SIG_MID_CREATE);
    g_setMaterial = (SetMaterialFn)TwkScanExe(SIG_SET_MATERIAL);
    g_getItemMat  = (GetMatFromItemMatFn)TwkScanExe(SIG_GET_ITEM_MAT);
    g_showMatSection = (ShowMatSectionFn)TwkScanExe(SIG_SHOW_MAT_SECTION);
    g_mapMatsAddr = TwkScanExe(SIG_MAP_ITEM_MATS);
    g_setTex      = (SetTexParamFn)TwkScanExe(SIG_SET_TEXPARAM);
    g_setScl      = (SetSclParamFn)TwkScanExe(SIG_SET_SCLPARAM);
    g_setVec      = (SetVecParamFn)TwkScanExe(SIG_SET_VECPARAM);
    const bool haveAll = g_staticFind && g_sco && g_regWorld && g_attach && g_setSkelMesh && g_setMaster;
    if (!haveAll || g_nSites == 0) {
        g_okBuild = 0;
        TwkLog("[cloth] un-merge unavailable (sites=%d find=%p sco=%p reg=%p att=%p mesh=%p master=%p) "
               "-- merge recon still logs", g_nSites, g_staticFind, (void*)g_sco, (void*)g_regWorld,
               (void*)g_attach, (void*)g_setSkelMesh, (void*)g_setMaster);
    }
    // The identity hook must be live BEFORE the merge hook, or a dress in between would be judged
    // with no `this` published and be treated as not-ours (harmless, but it would miss a re-dress).
    g_startRefresh = TwkScanExe(SIG_REFRESH_VISUALS);
    if (!g_startRefresh ||
        MH_CreateHook(g_startRefresh, (void*)&hkRefreshVisuals, &g_origRefresh) != MH_OK ||
        MH_EnableHook(g_startRefresh) != MH_OK) {
        g_okBuild = 0;
        TwkLog("[cloth] RefreshVisuals hook failed (%p) -- un-merge OFF: without it the garment "
               "would be stripped from preview characters and co-op peers", g_startRefresh);
    }
    g_start = TwkScanExe(SIG_DO_MERGE);
    if (!g_start) { TwkLog("[cloth] FSkeletalMeshMerge::DoMerge sig NOT FOUND -- cloth off (game updated?)"); return; }
    // Listening to the customization system is what lets a garment's material be known at dress time.
    if (g_mapMatsAddr && g_getItemMat) {
        if (MH_CreateHook(g_mapMatsAddr, (void*)&hkMapItemMaterials, (void**)&g_origMapMats) != MH_OK
            || MH_EnableHook(g_mapMatsAddr) != MH_OK) {
            g_origMapMats = nullptr;
            TwkLog("[mat] could not listen to the customization system -- materials fall back to the body");
        } else TwkLog("[mat] listening to the customization system for garment materials");
    } else TwkLog("[mat] customization material functions not found (map=%p get=%p)",
                  g_mapMatsAddr, (void*)g_getItemMat);

    if (MH_CreateHook(g_start, (void*)&hkDoMerge, &g_orig) != MH_OK || MH_EnableHook(g_start) != MH_OK) {
        TwkLog("[cloth] hook failed on DoMerge -- cloth off");
        g_start = nullptr; return;
    }
    if (haveAll && g_nSites > 0)
        TwkLog("[cloth] UN-MERGE armed: %d skater call sites, %d garment tag(s), %d exclusion(s)",
               g_nSites, g_nTags, g_nExcl);
}

void* ClothMerge_FindClass(const wchar_t* name) { return FindClassByName(name, "Class"); }

void ClothMerge_DrawMenu(const OmpMenuApi* api) {
    char b[128];
    if (!g_start) { api->TextDisabled("Cloth (phase A): merge hook not installed"); return; }
    bool on = g_unmerge != 0;
    if (api->Checkbox("Un-merge the shirt (cloth phase A)", &on)) { g_unmerge = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled(g_okBuild ? "(looks identical; the platform cloth lands on)"
                                                 : "(DISABLED by a fault this session)");
    snprintf(b, sizeof(b), "merges %d | excluded %d | slave builds %d | sites %d",
             (int)g_merges, (int)g_excluded, (int)g_slaveBuilds, g_nSites);
    api->TextDisabled(b);
}
