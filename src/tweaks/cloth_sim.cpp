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
// SessionTweaks -- CLOTH phase B round 1: THE QUAD FLAG. See cloth_sim.h for why this shape.
//
// Built here at runtime, none of it shipped by the game:
//   UClothingAssetCommon
//    +0xa0 LodData[1] = FClothLODDataCommon
//            +0x00 PhysicalMeshData = FClothPhysicalMeshData
//                    Vertices / Normals / Indices / InverseMasses / BoneData   <- our quad
//   -> appended to USkeletalMesh::MeshClothingAssets (+0x320), bHasActiveClothingAssets (+0x15f) set
//   -> USkeletalMeshComponent::RecreateClothingActors() on the un-merged garment component
//   -> FClothingSimulationNv::CreateActor runs and the solver owns our particles.
//
// PROOF OF LIFE is read out of the solver itself: FClothingSimulationNv +0x20 Actors (how many it
// built) and +0x34 SimulationTime, a float the solver advances as it steps. That clock rises whether
// or not our particles visibly move, which is exactly what makes it the right first measurement: it
// separates "the solver is running our asset" from "the particles are configured to move", and only
// the first question is being asked this round. Movement needs the MaxDistance weight map -- a TMap,
// and round 2's job; without it particles stay locked to the animated pose.
//
// EVERY array handed to the engine is allocated with the engine's OWN allocator (FMemory::Malloc).
// A TArray the engine may later resize or free must not point at our CRT heap.
//
// SAFETY: built ONCE per garment, from the pump (never inside a hooked callstack), only on our own
// un-merged garment, and OFF by default -- this is the first code here that hands hand-made
// structures to a physics solver, so it stays opt-in until it has been seen to work. One fault
// disables it for the session.
// =====================================================================================================
#define _CRT_SECURE_NO_WARNINGS
#include "MinHook.h"
#include "tweaks_common.h"
#include "ui/menu_ext.h"
#include "cloth_sim.h"
#include "cloth_merge.h"
#include "catch_sound.h"    // CatchSound_ObjName
#include <string.h>
#include <stdlib.h>
#include <math.h>

enum {
    SMC_SKELMESH     = 0x480,   // USkinnedMeshComponent::SkeletalMesh
    SM_MESHCLOTH     = 0x320,   // USkeletalMesh::MeshClothingAssets (TArray<UClothingAssetBase*>)
    SM_HASCLOTH      = 0x15f,   // USkeletalMesh::bHasActiveClothingAssets
    SMC_CLOTHSIM     = 0xa30,   // USkeletalMeshComponent::ClothingSimulation (IClothingSimulation*)
    SMC_DISABLECLOTH = 0x8ba,   // bDisableClothSimulation
    // UClothingAssetCommon (240 B)
    CA_LODDATA       = 0xa0,    // TArray<FClothLODDataCommon>
    CA_LODMAP        = 0xb0,    // TArray<int32>
    CA_USEDBONEIDX   = 0xd0,    // TArray<int32>
    CA_REFBONEIDX    = 0xe0,    // int32
    // FClothLODDataCommon (352 B) -- PhysicalMeshData sits at its start
    LOD_STRIDE       = 352,
    PMD_VERTS        = 0x00,    // TArray<FVector>
    PMD_NORMALS      = 0x10,
    PMD_INDICES      = 0x20,    // TArray<uint32>
    PMD_INVMASS      = 0x80,    // TArray<float>
    PMD_BONEDATA     = 0x90,    // TArray<FClothVertBoneData>, stride 76
    PMD_WEIGHTMAPS   = 0x30,    // TMap<uint32, FPointWeightMap> -- see the TMap note below
    PMD_MAXBONEW     = 0xa0,    // int32
    PMD_NUMFIXED     = 0xa4,    // int32
    BONEDATA_STRIDE  = 76,
    SM_LODINFO       = 0xf8,    // USkeletalMesh::LODInfo (TArray<FSkeletalMeshLODInfo>)
    SM_RENDERDATA    = 0x78,    // USkeletalMesh::SkeletalMeshRenderData (TUniquePtr -> raw ptr)
    RD_LODARRAY      = 0x00,    // FSkeletalMeshRenderData::LODRenderData -- TIndirectArray: PTRS
    LODRD_SECTIONS   = 0x10,    // FSkeletalMeshLODRenderData::RenderSections
    SEC_STRIDE       = 232,     // sizeof(FSkelMeshRenderSection)
    SEC_CLOTHASSETID = 0x40,    // FSkelMeshRenderSection::CorrespondClothAssetIndex -- int16!
    SEC_CLOTHMAPPING = 0x18,
    SEC_CLOTHDATA    = 0x44,    // FSkelMeshRenderSection::ClothingData -- FGuid + int32 LodIndex
    CA_ASSETGUID     = 0x38,    // UClothingAssetBase::AssetGuid
    // --- the gate chain, straight out of the disassembly of RecreateClothingActors/CreateActor ---
    SMC_REGFLAGS     = 0x88,    // UActorComponent bitfield; bit0 = bRegistered
    SMC_WORLD        = 0xa8,    // UActorComponent::WorldPrivate
    SMC_CLOTHFACTORY = 0x900,   // USkeletalMeshComponent::ClothingSimulationFactory (UClass*)
    WORLD_PHYSSCENE  = 0x1f0,   // UWorld::PhysicsScene       -- CreateActor bails if null
    WORLD_FLAGS      = 0x10c,   // bitfield; bit2 (0x4) = bShouldSimulatePhysics
    SMC_PREDLOD      = 0x5b8,   // USkinnedMeshComponent::PredictedLODLevel
    SMC_MASTERPOSE   = 0x488,   // USkinnedMeshComponent::MasterPoseComponent (weak ptr)
    ACTOR_STRIDE     = 0x1f0,   // sizeof(FClothingActorNv)
    ACTOR_CURLOD     = 0x60,    // FClothingActorNv::CurrentLodIndex -- -1 == actor is asleep
    ACTOR_LODDATA    = 0x130,   // FClothingActorNv::LodData
    ACTOR_SKINNORM   = 0x170,   // SkinnedPhysicsMeshNormals -- the ANIMATED reference (ours)
    ACTOR_CURNORM    = 0x180,   // CurrentNormals -- what the solver computed and the renderer uses
    // --- direct render: drive the ordinary skinned path from the simulation ---
    // Playback zeroes the body's animation rate and drives its components from recorded data (the
    // OpenMP replay work names this as the thing only OnReplayModeChanged puts back). Reading it is
    // the cheapest unambiguous "am I in a replay?" marker there is, and it costs one load a second.
    CA_CONFIGS       = 0x50,    // UClothingAssetCommon::ClothConfigs -- TMap<FName, UClothConfigBase*>
    CFGMAP_VALUE     = 0x08,    // element 0: FName key (8 bytes), THEN the pointer
    CFG_WINDDRAG     = 0x88,    // UClothConfigNv::WindDragCoefficient
    CFG_WINDLIFT     = 0x8c,    // UClothConfigNv::WindLiftCoefficient
    SMC_ANIMRATE     = 0x8b0,   // USkeletalMeshComponent::GlobalAnimRateScale
    SMC_PAUSEANIMS   = 0x8c1,   // bPauseAnims / bNoSkeletonUpdate share this byte
    SMC_SIMDATA      = 0xa60,   // TMap<int32, FClothSimulData> -- the simulated result, per actor
    SMC_CSTA         = 0x4b0,   // ComponentSpaceTransformsArray[2] (double buffered)
    SMC_CST_READ     = 0x4f4,   // which of the two the game is currently reading
    SC_COMPLOCATION  = 0x1c0,   // USceneComponent::ComponentToWorld translation
    SMC_CST_EDIT     = 0x4f0,   // which of the two buffers we may write
    SMC_MASTERBONEMAP= 0x510,   // garment bone -> body bone. Two meshes number their bones their
                                // own way, so this table is not optional.
    SIMDATA_STRIDE   = 160,     // TSetElement<TTuple<int32,FClothSimulData>>, 16-aligned
    SIMDATA_POS      = 16,      // ...the value starts here: Positions, then Normals
    SIMDATA_NRM      = 32,
    // FClothSimulData, read from the shipped PDB rather than guessed: Positions +0x00, Normals
    // +0x10, FTransform Transform +0x20, FTransform ComponentRelativeTransform +0x50 (size 128).
    // These are FTransforms -- quat, translation, scale, 4 floats each -- NOT matrices; reading them
    // as matrices produced nonsense and wrote NaN into the mesh.
    SIMDATA_XFORM    = 16 + 0x20,
    SIMDATA_COMPREL  = 16 + 0x50,
    // FinalRefBonePose, to MATCH FinalRefBoneInfo. Pairing Final bone info with the Raw pose is
    // two different skeletons' worth of data and puts every vertex in the wrong place.
    SM_REFPOSE       = 0x1b0 + 0x30,
    XFORM_STRIDE     = 48,
    SMC_CLOTHTICK    = 0x9a8,   // USkeletalMeshComponent::ClothTickFunction (FTickFunction)
    SMC_CLOTHTICK_ID = 0x9c8,   // ...its InternalData -- non-null ONLY once registered
    VT_SHOULDRUNCLOTH= 0xa18,   // vtable slot: USkeletalMeshComponent::ShouldRunClothTick()
    // --- LOD render data: the garment's actual geometry, for driving REAL vertices ---
    LODRD_IDXCONT    = 0x20,    // FMultiSizeIndexContainer: +0x00 DataTypeSize, +0x08 IndexBuffer*
    LODRD_POSVB      = 0xc8,    // StaticVertexBuffers(0x40) + PositionVertexBuffer(0x88)
    POSVB_DATA       = 0x28,    // raw CPU bytes -- null when cooked without CPU access
    POSVB_STRIDE     = 0x30,
    POSVB_NUMVERTS   = 0x34,
    POSVB_CPUACCESS  = 0x38,
    LODRD_SKINVB     = 0x148,   // FSkinWeightVertexBuffer -> DataVertexBuffer at +0x00
    SKINVB_CPUACCESS = 0x20,
    SKINVB_MAXINFL   = 0x24,
    SKINVB_16BITBONE = 0x28,
    SKINVB_DATA      = 0x38,
    SKINVB_NUMVERTS  = 0x40,
    SKINVB_VARBONES  = 0x21,    // bVariableBonesPerVertex -- changes the whole weight layout
    SEC_MATIDX       = 0x00,    // FSkelMeshRenderSection::MaterialIndex (uint16) -- section order
                                // is NOT material order, which is the whole trap below
    SEC_NUMTRIS      = 0x08,
    SEC_BASEIDX      = 0x04,
    SEC_BASEVERT     = 0x10,
    SEC_BONEMAP      = 0x28,    // TArray<uint16>: section-local bone -> SKELETON bone
    SEC_NUMVERTS     = 0x38,
    IDXBUF_DATA      = 0x28,    // FRawStaticIndexBuffer16or32: TResourceArray -> vptr,Data,Num,Max
    IDXBUF_NUM       = 0x30,
    // --- render binding: what actually makes the simulated shape show up on screen ---
    LODRD_CLOTHVB    = 0x1d0,   // FSkeletalMeshVertexClothBuffer
    CVB_IDXMAP       = 0x20,    // ClothIndexMapping TArray<uint64>, one entry per section
    CVB_VERTEXDATA   = 0x30,    // FSkeletalMeshVertexDataInterface*
    CVB_DATA         = 0x38,
    CVB_STRIDE       = 0x40,
    CVB_NUMVERTS     = 0x44,
    M2M_STRIDE       = 64,      // sizeof(FMeshToMeshVertData)
    LODRD_TANGDATA   = 0xa0,    // StaticVertexBuffers(0x40) + FStaticMeshVertexBuffer TangentsDataPtr(0x60)
    LODRD_TANGSTRIDE = 0xb0,
    LODRD_TANGHIPREC = 0xc1,    // bUseHighPrecisionTangentBasis
    ACTOR_USEGRAVOVR = 0x10,    // FClothingActorNv::bUseGravityOverride
    ACTOR_GRAVOVR    = 0x14,    // FClothingActorNv::GravityOverride (FVector)
    SM_REFSKELETON   = 0x1b0,   // USkeletalMesh::RefSkeleton
    REFSK_BONEINFO   = 0x20,    // FReferenceSkeleton::FinalRefBoneInfo (TArray<FMeshBoneInfo>)
    BONEINFO_STRIDE  = 12,      // FMeshBoneInfo: FName Name, int32 ParentIndex
    // FClothingSimulationNv (56 B)
    NVSIM_ACTORS     = 0x20,    // TArray<FClothingActorNv>
    NVSIM_SIMTIME    = 0x34,    // TAtomic<float> -- the proof of life
};

// ---- sigs. All CALLED, never hooked.
// FMemory::Malloc -- Epic 0x123e750 / Steam 0x11ff180. The engine's allocator: an array we hand over must be
// resizable and freeable BY THE ENGINE, so its buffer has to come from here, not our CRT heap.
static const char* SIG_MALLOC =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 ?? ?? "
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??";
// UClothingAssetCommon::AddClothConfigs -- Epic 0x3267980 / Steam 0x322a560. Builds the default UClothConfigNv and
// files it in the asset's ClothConfigs TMap. Calling the engine's own builder avoids hand-making
// that TMap, the one container in this structure genuinely painful to fake.
static const char* SIG_ADD_CONFIGS =
    "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 18 FF FF FF";
// USkeletalMeshComponent::RecreateClothingActors -- Epic 0x2f71490 / Steam 0x2f33f70. Rebuilds the component's cloth
// actors from its mesh's asset list -- i.e. this is what notices our new asset.
static const char* SIG_RECREATE =
    "40 53 41 55 48 83 EC 48 44 8B 2D ?? ?? ?? ?? 48 8B D9 41 8B D5";

typedef void* (*MallocFn)(size_t, uint32_t);
typedef void  (*AddConfigsFn)(void*);
typedef void  (*RecreateFn)(void*);
static MallocFn     g_malloc   = nullptr;
static AddConfigsFn g_addCfg   = nullptr;
static RecreateFn   g_recreate = nullptr;
// USkeletalMesh::GetClothingAssetsInUse(TArray<UClothingAssetBase*>& Out) -- the function whose
// answer decides whether CreateActor is called at all. We call it ourselves to read that answer
// directly instead of inferring it from the silence.
typedef void (*AssetsInUseFn)(void* mesh, void* outArray);
static AssetsInUseFn g_assetsInUse = nullptr;
// USkeletalMeshComponent::UpdateClothTickRegisteredState -- Epic 0x2b69d60 / Steam 0x2b2c5a0.
// Cloth is ticked by its OWN tick function, registered separately from the component's. Ours was
// registered before it had any cloth, so that never happened; this is the engine's own "register
// the cloth tick if it should be running" call, which is exactly what we need after adding cloth.
typedef void (*UpdClothTickFn)(void* comp);
static UpdClothTickFn g_updClothTick = nullptr;
static const char* SIG_UPD_CLOTHTICK =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 50 48 8B D9 48 85 C0 ?? ??";

// FSkeletalMeshVertexClothBuffer::AllocateData -- Epic 0x2eec9f0 / Steam 0x2eaf4d0. Creates the
// buffer's vertex-data object; InitRHI reads the upload size back out of it, so there is no way to
// skip this and just hand over a raw pointer.
typedef void (*AllocClothDataFn)(void* clothBuffer);
static AllocClothDataFn g_allocClothData = nullptr;
static const char* SIG_ALLOC_CLOTHDATA =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B 49 30 48 85 C9 ?? ?? 48 8B 01";

// BeginInitResource -- Epic 0x24c2cd0 / Steam 0x2484bf0. Queues the buffer's InitRHI onto the
// render thread. Four other functions share its opening shape once call targets are wildcarded,
// hence the long signature.
typedef void (*BeginInitResFn)(void* renderResource);
static BeginInitResFn g_beginInitRes = nullptr;
// BeginUpdateResourceRHI -- Epic 0x24c2ec0 / Steam 0x2484de0. THIS is how a buffer that is already
// live gets new contents onto the card: BeginInitResource returns immediately for an initialised
// resource, so the rewritten vertices never went anywhere and the whole thing cost nothing, which is
// exactly what "no change, no fps drop" meant.
typedef void (*BeginUpdateResFn)(void* renderResource);
static BeginUpdateResFn g_beginUpdateRes = nullptr;
static const char* SIG_BEGIN_UPDATE_RES =
    "40 53 48 83 EC 40 48 8B D9 E8 ?? ?? ?? ?? 84 C0 ?? ?? E8 ?? ?? ?? ?? 83 7B 08 FF 0F 84";
static const char* SIG_BEGIN_INIT_RES =
    "40 53 48 83 EC 40 48 8B D9 E8 ?? ?? ?? ?? 84 C0 ?? ?? 38 05 ?? ?? ?? ?? ?? ?? 38 05 ?? ?? "
    "?? ?? ?? ?? FF 15 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 0F 94 C0 84 C0 ?? ?? E8 ?? ?? ?? ?? 48 8B 03 "
    "48 8B CB 48 83 C4 40 5B ?? ?? ?? ?? 33 D2 48 8D 4C 24 20";

// UActorComponent::RecreateRenderState_Concurrent -- Epic 0x2aeeec0 / Steam 0x2ab1700. Rebuilds the
// component's render proxy, which is what makes it pick up the cloth vertex factories.
// USkeletalMeshComponent::FinalizeBoneTransform -- Epic 0x2b58280. Swaps in the bones just written
// and tells the renderer, which is exactly what a mod driving a skeleton by hand needs.
typedef void (*FinalizeBonesFn)(void* comp);
static FinalizeBonesFn g_finalizeBones = nullptr;
// Matched 22 bytes INTO the function and stepped back: the opening bytes are detoured in memory by
// something else in the process, so a prologue signature finds nothing at runtime even though it is
// a perfect match against the file on disk. Same technique as the object-construction signature.
static const char* SIG_FINALIZE_BONES =
    "48 8D 8B 20 0B 00 00 33 D2 E8 ?? ?? ?? ?? 48 8D 8B B0 0E 00 00 48 83 C4 20 5B E9 ?? ?? ?? ??";
static const int kFinalizeAnchorOff = 22;
// USkinnedMeshComponent::SetMasterPoseComponent -- taking the garment OFF the body's skeleton is
// what lets it carry its own.
typedef void (*SetMasterPoseFn)(void* comp, void* master, bool bForce);
static SetMasterPoseFn g_setMasterPose = nullptr;

typedef void (*RecreateRenderFn)(void* comp);
static RecreateRenderFn g_recreateRender = nullptr;
static const char* SIG_RECREATE_RENDER =
    "40 53 48 83 EC 20 F6 81 88 00 00 00 02 48 8B D9 ?? ?? 48 8B 01 FF 90 ?? ?? ?? ??";

static const char* SIG_ASSETS_IN_USE =
    "48 89 4C 24 08 41 56 41 57 48 83 EC 48 8B 42 0C 4C 8B FA C7 42 08 00 00 00 00 4C 8B F1";

static int   g_on = 0;            // ClothQuad -- OFF by default (see the safety note above)
static int   g_realMesh = 1;      // ClothRealMesh -- simulate the GARMENT, not the test quad
// Defined further down with the mesh builder; declared here because the config reader needs them.
extern float ClothSim_Gravity;
extern float ClothSim_MaxTravel;
extern int   ClothSim_FlipNormals;
extern int   ClothSim_TangentMode;
extern int   ClothSim_LightFlip;
extern int   ClothSim_SimXform;
extern float ClothSim_PeakAt;
extern float ClothSim_HemGrip;
extern float ClothSim_HemGripLower;
extern float ClothSim_HemPush;
extern float ClothSim_HemPushBand;
extern int   ClothSim_AutoWinding;
static int   g_lag       = 1;     // ClothLag -- the garment sits slightly behind the body
static float g_lagRate   = 9.0f;  // how quickly it catches up (lower = looser)
static float g_lagMax    = 4.0f;  // cm it may ever sit behind
static void  LagDrive(void* comp, int slot, float dt);
static int   g_sway      = 0;     // ClothBoneSway -- OFF: see the T-pose note below -- give the garment its own, lagging bones
static float g_swayRate  = 14.0f; // how quickly it catches up (lower = looser)
static float g_swayMax   = 9.0f;  // cm a bone may trail the body
static float g_swayRot   = 12.0f; // ...and degrees
static bool  SwayArm(void* comp, int slot);
static void  SwayDrive(void* comp, int slot, float dt);
static int   g_direct   = 0;      // ClothDirectRender -- draw the simulation ourselves
static int   g_directRefresh = 0; // ClothDirectRefresh -- costly test: force the renderer to re-read
static bool  DirectArm(void* comp, void* mesh, int slot);   // defined with the direct-render block
static int   g_render   = 0;      // ClothRender -- feed the simulation back into the drawn mesh.
                                  // OFF by default: this is the first thing here that touches
                                  // render resources, and it edits a mesh other characters wear.
static int   g_ok = 1;            // runtime kill-switch; never saved
// Nothing here may run before Install has resolved the engine functions. Modules install across
// several frames, so if the character is already dressed by then -- which happens whenever the skater
// spawns early -- the pump would reach a null engine call and fault, taking cloth down for the whole
// session. Ordering, not logic: the work is simply not ready yet, so wait a frame.
static bool  g_ready = false;
static int   g_windPct  = 100;    // ClothWindPct -- how much of the world's wind reaches clothing
static int   g_debugLog = 0;      // ClothDebugLog -- the per-second telemetry, off for release
static int   g_armDelayMs = 750;  // ClothArmDelayMs -- settle time after a dress before cloth resources
                                  // are created; see the note in ClothSim_PumpFrame
static int   g_maxVerts = 120000; // ClothMaxVerts -- see the read; a garbage guard, not a poly budget
// One entry per garment slot the un-merge owns (tops, bottoms, ...). Each garment gets its own
// cloth asset; the weld map is scratch, consumed by the render binding in the same pass.
enum { kSimSlots = 4 };
struct SimSlot { void* mesh; void* asset; long serial; };
static SimSlot g_sim[kSimSlots] = {};
static void* g_asset = nullptr;      // most recent, for the status line only
static void* g_builtOnMesh = nullptr;
static long  g_builtSerial = -1;
static char  g_status[96] = "not built";

void ClothSim_ReadConfig(const char* buf) {
    g_on = TwkIniInt(buf, "ClothQuad", 0);
    g_realMesh = TwkIniInt(buf, "ClothRealMesh", 1);
    g_render   = TwkIniInt(buf, "ClothRender", 0);
    g_direct   = TwkIniInt(buf, "ClothDirectRender", 0);
    g_directRefresh = TwkIniInt(buf, "ClothDirectRefresh", 0);
    // Giving a garment its own skeleton is DISABLED outright, not merely defaulted off: the
    // hand-written bones never reached the renderer, so the garment was left with nothing posing it
    // and stood in its rest pose. A setting left over in someone's file must not be able to bring
    // that back, so the value is read and then forced off.
    g_sway     = TwkIniInt(buf, "ClothBoneSway", 0);
    if (g_sway) { TwkLog("[sway] ClothBoneSway is set but the feature is withdrawn -- ignoring it"); }
    g_sway = 0;
    g_lag      = TwkIniInt(buf, "ClothLag", 1);
    g_lagRate  = (float)TwkIniInt(buf, "ClothLagRate",  9);
    g_lagMax   = (float)TwkIniInt(buf, "ClothLagMaxMm", 40) / 10.0f;
    g_swayRate = (float)TwkIniInt(buf, "ClothSwayRate",    14) ;
    g_swayMax  = (float)TwkIniInt(buf, "ClothSwayMaxMm",   90) / 10.0f;
    g_swayRot  = (float)TwkIniInt(buf, "ClothSwayMaxDeg",  12);
    ClothSim_Gravity   = (float)TwkIniInt(buf, "ClothGravityPct",  0)   / 100.0f;
    ClothSim_MaxTravel = (float)TwkIniInt(buf, "ClothTravelMm",    60)  / 10.0f;
    ClothSim_FlipNormals = TwkIniInt(buf, "ClothFlipNormals", 0);
    ClothSim_TangentMode = TwkIniInt(buf, "ClothTangentMode", 0);
    ClothSim_LightFlip   = TwkIniInt(buf, "ClothLightFlip", 1);
    ClothSim_SimXform    = TwkIniInt(buf, "ClothSimXform", 2);
    g_debugLog           = TwkIniInt(buf, "ClothDebugLog", 0);
    // ClothMaxVerts: the sanity ceiling on a garment section, RAW section verts before welding (UV
    // and tangent splits inflate this well above the cloth particle count in the build line).
    // The guard is here to catch a MISREAD section -- a bad offset gives a wild or negative count --
    // not to enforce a poly budget, and the old hardcoded 40000 turned away a legitimate 55651-vertex
    // modded garment. (The random crash first blamed on that garment turned out to belong to another
    // mod entirely.) Lower it to exclude a garment that misbehaves -- below its vertex count is a
    // per-garment off switch needing no rebuild.
    g_maxVerts           = TwkIniInt(buf, "ClothMaxVerts", 120000);
    g_armDelayMs         = TwkIniInt(buf, "ClothArmDelayMs", 750);
    if (g_armDelayMs < 0) g_armDelayMs = 0; else if (g_armDelayMs > 10000) g_armDelayMs = 10000;
    if (g_maxVerts < 1000) g_maxVerts = 1000; else if (g_maxVerts > 1000000) g_maxVerts = 1000000;
    g_windPct            = TwkIniInt(buf, "ClothWindPct", 100);
    if (g_windPct < 0) g_windPct = 0; else if (g_windPct > 200) g_windPct = 200;
    ClothSim_PeakAt  = (float)TwkIniInt(buf, "ClothPeakAtPct",  65) / 100.0f;
    ClothSim_HemGrip = (float)TwkIniInt(buf, "ClothHemGripPct", 35) / 100.0f;
    ClothSim_HemGripLower = (float)TwkIniInt(buf, "ClothHemGripPctLower", 12) / 100.0f;
    ClothSim_HemPush      = (float)TwkIniInt(buf, "ClothHemPushMm",   0) / 10.0f;
    ClothSim_HemPushBand  = (float)TwkIniInt(buf, "ClothHemPushBandPct", 30) / 100.0f;
    ClothSim_AutoWinding = TwkIniInt(buf, "ClothAutoWinding", 1);
    TwkLog("[quad] config: ClothQuad=%d ClothRealMesh=%d ClothRender=%d ClothMaxVerts=%d ArmDelayMs=%d",
           g_on, g_realMesh, g_render, g_maxVerts, g_armDelayMs);
}
void ClothSim_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "ClothQuad", g_on);
    TwkIniSetInt(buf, cap, "ClothRealMesh", g_realMesh);
    TwkIniSetInt(buf, cap, "ClothRender", g_render);
    TwkIniSetInt(buf, cap, "ClothDirectRender", g_direct);
    TwkIniSetInt(buf, cap, "ClothDirectRefresh", g_directRefresh);
    TwkIniSetInt(buf, cap, "ClothBoneSway",   g_sway);
    TwkIniSetInt(buf, cap, "ClothLag",        g_lag);
    TwkIniSetInt(buf, cap, "ClothLagRate",    (int)g_lagRate);
    TwkIniSetInt(buf, cap, "ClothLagMaxMm",   (int)(g_lagMax * 10.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothSwayRate",   (int)g_swayRate);
    TwkIniSetInt(buf, cap, "ClothSwayMaxMm",  (int)(g_swayMax * 10.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothSwayMaxDeg", (int)g_swayRot);
    TwkIniSetInt(buf, cap, "ClothGravityPct", (int)(ClothSim_Gravity * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothTravelMm",   (int)(ClothSim_MaxTravel * 10.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothFlipNormals", ClothSim_FlipNormals);
    TwkIniSetInt(buf, cap, "ClothTangentMode", ClothSim_TangentMode);
    TwkIniSetInt(buf, cap, "ClothLightFlip",   ClothSim_LightFlip);
    TwkIniSetInt(buf, cap, "ClothSimXform",    ClothSim_SimXform);
    TwkIniSetInt(buf, cap, "ClothDebugLog",    g_debugLog);
    TwkIniSetInt(buf, cap, "ClothMaxVerts",    g_maxVerts);
    TwkIniSetInt(buf, cap, "ClothArmDelayMs",  g_armDelayMs);
    TwkIniSetInt(buf, cap, "ClothWindPct",     g_windPct);
    TwkIniSetInt(buf, cap, "ClothPeakAtPct",  (int)(ClothSim_PeakAt  * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothHemGripPct", (int)(ClothSim_HemGrip * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothHemGripPctLower", (int)(ClothSim_HemGripLower * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothHemPushMm",   (int)(ClothSim_HemPush * 10.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothHemPushBandPct", (int)(ClothSim_HemPushBand * 100.0f + 0.5f));
    TwkIniSetInt(buf, cap, "ClothAutoWinding", ClothSim_AutoWinding);
}
void ClothSim_ResetDefaults() { g_on = 0; g_ok = 1; }

// A UE TArray is { void* Data; int32 Num; int32 Max }. Building one means allocating with the
// engine's allocator and filling all three fields -- Max matters as much as Num: the engine reads
// it before any resize, and a Max below Num describes an overfull buffer.
static bool MakeArray(void* arrayField, const void* src, int count, int stride) {
    if (!g_malloc || count <= 0) return false;
    void* mem = g_malloc((size_t)count * (size_t)stride, 16);
    if (!mem) return false;
    if (src) memcpy(mem, src, (size_t)count * (size_t)stride);
    else     memset(mem, 0, (size_t)count * (size_t)stride);
    *(void**)arrayField = mem;
    *(int*)((uint8_t*)arrayField + 8)  = count;   // Num
    *(int*)((uint8_t*)arrayField + 12) = count;   // Max
    return true;
}


// ---------------------------------------------------------------------------------------------
// Hand-build a UE TMap<uint32, FPointWeightMap> holding ONE entry.
//
// Why by hand: the per-particle MaxDistance lives ONLY in this map in 4.26+ (the flat
// MaxDistances_DEPRECATED array is load-time migration and is never read at runtime), and every
// weight-map helper in the shipping exe is a 3-byte stub -- there is nothing to call. Without it
// the solver has no constraint data and refuses to build an actor at all, which is exactly the
// actors=0 measured in the field.
//
// A TMap IS a TSet<TPair<K,V>> and nothing else, which is why the whole thing is 80 bytes:
//   +0x00 Elements.Data            TArray {ptr, Num, Max}          -- the element buffer
//   +0x10 Elements.AllocationFlags TBitArray: uint32 Inline[4],    -- which slots are live
//         +0x20 SecondaryData ptr, +0x28 NumBits, +0x2c MaxBits
//   +0x30 Elements.FirstFreeIndex  int32   (-1 = no free list)
//   +0x34 Elements.NumFreeIndices  int32
//   +0x38 Hash inline FSetElementId[1], +0x40 Hash secondary ptr
//   +0x48 HashSize                 int32
// One element is TSetElement<TPair<uint32,FPointWeightMap>> = 32 bytes:
//   +0x00 Key uint32 (padded to 8 -- FPointWeightMap holds a TArray, so it is 8-aligned)
//   +0x08 Value FPointWeightMap { TArray<float> }   (16 bytes)
//   +0x18 HashNextId int32 (-1, no collision chain)
//   +0x1c HashIndex  int32 (0)
// With HashSize 1 every key hashes to bucket 0, so the hash function never has to be reproduced --
// the single bucket points at element 0. That is the trick that makes this tractable by hand.
static bool MakeSingleEntryWeightMap(void* mapField, uint32_t key, const float* values, int count) {
    if (!g_malloc) return false;
    uint8_t* m = (uint8_t*)mapField;
    memset(m, 0, 0x50);

    uint8_t* elem = (uint8_t*)g_malloc(32, 8);       // one TSetElement
    if (!elem) return false;
    memset(elem, 0, 32);
    *(uint32_t*)(elem + 0x00) = key;
    if (!MakeArray(elem + 0x08, values, count, 4)) return false;   // FPointWeightMap::Values
    *(int*)(elem + 0x18) = -1;                        // HashNextId = INDEX_NONE
    *(int*)(elem + 0x1c) = 0;                         // HashIndex

    *(void**)(m + 0x00) = elem;                       // Elements.Data
    *(int*)(m + 0x08) = 1;                            // Num
    *(int*)(m + 0x0c) = 1;                            // Max
    *(uint32_t*)(m + 0x10) = 1u;                      // AllocationFlags inline word: slot 0 live
    *(int*)(m + 0x28) = 1;                            // NumBits
    *(int*)(m + 0x2c) = 128;                          // MaxBits = 4 inline words
    *(int*)(m + 0x30) = -1;                           // FirstFreeIndex
    *(int*)(m + 0x34) = 0;                            // NumFreeIndices
    *(int*)(m + 0x38) = 0;                            // Hash bucket 0 -> element 0
    *(int*)(m + 0x48) = 1;                            // HashSize = 1
    return true;
}

// A fixed, distinctive guid for the test asset. Any non-zero value works; the only requirement is
// that the asset and the render sections carry the SAME one.
static const uint8_t kQuadGuid[16] = { 0x51,0x55,0x41,0x44, 0x53,0x45,0x53,0x53,
                                       0x43,0x4c,0x4f,0x54, 0x48,0x30,0x30,0x31 };


// ---------------------------------------------------------------------------------------------
// Build the simulation mesh from the GARMENT OWN render mesh.
//
// The usual way to author cloth is a coarse sim mesh plus a mesh-to-mesh binding solve -- and the
// function that does that solve (GenerateMeshToMeshSkinningData) is one of the editor-only pieces
// stripped from the shipping exe. Using the render mesh directly as the sim mesh sidesteps it
// entirely: every render vertex IS a sim vertex, so the binding is the identity and there is
// nothing to solve. It costs particles (a few thousand instead of a few hundred), which is the
// trade being made knowingly.
//
// Everything here comes from buffers confirmed CPU-readable in this build; if any of it is missing
// the caller falls back to the test quad rather than guessing.
float ClothSim_PinAbove  = 0.55f;   // above this fraction of garment height, welded to the body
float ClothSim_MaxTravel = 6.0f;    // cm the hem may stray from its animated position
// Gravity as a fraction of the world's. At 0 the garment has no weight of its own: it sits exactly
// where the animation puts it and only MOVES because the body moves, which is what stops a hoodie
// sagging into the pants while standing still. Momentum and wind still apply.
float ClothSim_Gravity   = 0.0f;
// Two shading choices that cannot be read out of the game: the surface reconstruction happens
// entirely on the graphics card, and there is no copy of that maths on the CPU to inspect. Rather
// than keep guessing, each is a switch so one look in game settles it.
//   ClothFlipNormals  0 = as the garment stores them, 1 = the other way round
//   ClothTangentMode  0 = matched to the garment, 1 = along a triangle edge, 2 = old (collapsed)
int ClothSim_FlipNormals = 0;
int ClothSim_TangentMode = 0;
// Which published transform takes the simulation's points into component space: 0 none, 1 Transform,
// 2 ComponentRelativeTransform. Both are logged once so the right one is a reading, not a guess.
int ClothSim_SimXform = 2;
// Which way the renderer is told each surface faces. The simulation's own directions were measured
// correct, but the renderer rebuilds them from this, and the sign of that rebuild is the one thing
// that cannot be read from the game -- it had to be settled in-game. CONFIRMED 2026-08-16: negated is
// right; front-lit clothing came out dark until this was flipped. Leave it at 1.
int ClothSim_LightFlip = 1;
// Where the garment is loosest. A hem is a finished edge -- hemmed, often ribbed -- so it holds its
// shape better than the loose middle of the garment does; letting the very bottom swing the most
// reads as a flag rather than a hoodie. Travel rises to a peak partway down and eases off again.
int ClothSim_AutoWinding = 1;     // measure our winding against the garment and match it
float ClothSim_PeakAt  = 0.65f;   // 0 = at the pin line, 1 = at the bottom edge
float ClothSim_HemGrip = 0.35f;   // how much of peak travel the bottom edge keeps
// Trousers want a much tighter hem than a shirt: their bottom edge is at your ANKLE, and a swinging
// cuff opens a gap where the leg used to be hidden.
float ClothSim_HemGripLower = 0.12f;
// A gentle outward inflation of the hem, so a shirt's bottom edge sits proud of what is underneath
// instead of clipping into it. Applied to the rest shape, not as a force -- there is no body
// collision in this simulation, so the shape is what keeps it out.
float ClothSim_HemPush  = 0.0f;   // cm
float ClothSim_HemPushBand = 0.3f;// fraction of the garment's height, up from the bottom edge
static bool g_buildLower = false; // is the garment being built a lower-body one?
static int g_realVerts = 0, g_realTris = 0, g_realBones = 0;


// The tangent basis is stored packed: two components per vertex, TangentX (the tangent) then
// TangentZ (the normal), each either 4 bytes of unsigned bytes or 8 bytes of unsigned shorts.
// Both map the full range onto -1..1, so the decode is the same shape either way.
static inline bool DecodeTangentBasis(const uint8_t* base, int stride, bool hiPrec, int vert,
                                      float* outTangent, float* outNormal) {
    if (!base || stride <= 0) return false;
    const uint8_t* e = base + (size_t)vert * stride;
    if (hiPrec) {
        const uint16_t* t = (const uint16_t*)e;
        for (int k = 0; k < 3; k++) outTangent[k] = (float)t[k]     / 32767.5f - 1.0f;
        for (int k = 0; k < 3; k++) outNormal[k]  = (float)t[k + 4] / 32767.5f - 1.0f;
    } else {
        for (int k = 0; k < 3; k++) outTangent[k] = (float)e[k]     / 127.5f - 1.0f;
        for (int k = 0; k < 3; k++) outNormal[k]  = (float)e[k + 4] / 127.5f - 1.0f;
    }
    return true;
}


// ---------------------------------------------------------------------------------------------
// Welding. A render mesh splits vertices wherever the UVs or the smoothing do -- the same point in
// space appears two or three times so it can carry different texture coordinates. Simulated
// independently, those copies drift apart and the garment splits open along exactly those seams,
// which is the tearing seen along the hood and shoulders. So the SIM mesh welds them back into one
// particle, and several render vertices then read from that single simulated point.
static int   g_weldUnique = 0;
static int*  g_weld       = nullptr;   // render vertex -> sim particle
static int   g_weldCount  = 0;

static void FreeWeld() { free(g_weld); g_weld = nullptr; g_weldCount = 0; g_weldUnique = 0; }

static bool BuildWeld(const float* verts, int nVerts) {
    FreeWeld();
    g_weld = (int*)malloc((size_t)nVerts * sizeof(int));
    if (!g_weld) return false;
    g_weldCount = nVerts;

    int cap = 1; while (cap < nVerts * 2) cap <<= 1;
    int* table = (int*)malloc((size_t)cap * sizeof(int));
    if (!table) { FreeWeld(); return false; }
    for (int i = 0; i < cap; i++) table[i] = -1;

    int unique = 0;
    for (int i = 0; i < nVerts; i++) {
        // Quantise to a hundredth of a millimetre: split copies are bitwise identical in practice,
        // and the tolerance costs nothing while covering any that are not.
        const int qx = (int)(verts[i*3+0] * 1000.0f), qy = (int)(verts[i*3+1] * 1000.0f),
                  qz = (int)(verts[i*3+2] * 1000.0f);
        uint32_t h = (uint32_t)(qx * 73856093) ^ (uint32_t)(qy * 19349663) ^ (uint32_t)(qz * 83492791);
        h &= (uint32_t)(cap - 1);
        int found = -1;
        while (table[h] != -1) {
            const int c = table[h];
            const int cx = (int)(verts[c*3+0] * 1000.0f), cy = (int)(verts[c*3+1] * 1000.0f),
                      cz = (int)(verts[c*3+2] * 1000.0f);
            if (cx == qx && cy == qy && cz == qz) { found = g_weld[c]; break; }
            h = (h + 1) & (uint32_t)(cap - 1);
        }
        if (found >= 0) { g_weld[i] = found; }
        else { table[h] = i; g_weld[i] = unique++; }
    }
    free(table);
    g_weldUnique = unique;
    return true;
}

// Cuffs and collars are held by the body; hems and skirts hang free. Height alone cannot tell them
// apart -- with the arms down a sleeve cuff sits as low as the hem and gets treated like one, which
// is why the sleeve floated off the wrist. The bone a vertex is bound to says it plainly.
static bool BoneIsTight(const char* n) {
    if (!n) return false;
    char l[64]; int i = 0;
    for (; n[i] && i < 63; i++) l[i] = (char)((n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i]);
    l[i] = 0;
    return strstr(l, "hand") || strstr(l, "wrist") || strstr(l, "forearm") || strstr(l, "lowerarm")
        || strstr(l, "neck")  || strstr(l, "head");
}

// The section that IS the garment. "Section 0" held for every one-section garment and was wrong for
// the first that was not: the sweater's section 0 is its SKIN layer (hands and nape) and the sweater
// itself is section 1, so the simulation was built on the hands and the sweater never moved.
// Section order is not material order -- match on the material slot the customization named.
static uint8_t* GarmentSection(void* mesh, uint8_t* lod) {
    uint8_t* secs = (uint8_t*)twkP(lod, LODRD_SECTIONS);
    const int nSec = twkI(lod, LODRD_SECTIONS + 8);
    if (!secs || nSec <= 0) return nullptr;
    const int want = ClothMerge_GarmentMaterialIndex(mesh);
    if (want >= 0)
        for (int k = 0; k < nSec && k < 64; k++) {
            uint8_t* c = secs + (size_t)k * SEC_STRIDE;
            if (*(uint16_t*)(c + SEC_MATIDX) == (uint16_t)want) return c;
        }
    return secs;
}

static bool BuildRealMeshData(void* mesh, uint8_t* lod, uint8_t* clothLod, void* asset) {
    uint8_t* sec = GarmentSection(mesh, lod);
    if (!sec) return false;

    const int nVerts   = twkI(sec, SEC_NUMVERTS);
    const int nTris    = twkI(sec, SEC_NUMTRIS);
    const int baseIdx  = twkI(sec, SEC_BASEIDX);
    const int baseVert = twkI(sec, SEC_BASEVERT);
    if (nVerts <= 0 || nVerts > g_maxVerts || nTris <= 0 || nTris > g_maxVerts * 2) {
        TwkLog("[real] section past the ClothMaxVerts ceiling: verts=%d tris=%d (limit %d/%d) -- not "
               "building. If this garment really is that dense, raise ClothMaxVerts in "
               "SessionTweaks.ini", nVerts, nTris, g_maxVerts, g_maxVerts * 2);
        return false;
    }

    uint8_t* posData    = (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA);
    const int posStride = twkI(lod, LODRD_POSVB + POSVB_STRIDE);
    uint8_t* skinData   = (uint8_t*)twkP(lod, LODRD_SKINVB + SKINVB_DATA);
    void*    idxBuf     = twkP(lod, LODRD_IDXCONT + 8);
    const int idxSize   = *(uint8_t*)(lod + LODRD_IDXCONT);
    uint8_t* idxData    = idxBuf ? (uint8_t*)twkP(idxBuf, IDXBUF_DATA) : nullptr;
    const int idxNum    = idxBuf ? twkI(idxBuf, IDXBUF_NUM) : 0;
    if (!posData || posStride < 12 || !idxData || (idxSize != 2 && idxSize != 4)) {
        TwkLog("[real] geometry not CPU-readable (pos=%p idx=%p size=%d)", posData, idxData, idxSize);
        return false;
    }
    if (baseIdx + nTris * 3 > idxNum) {
        TwkLog("[real] index range past buffer (%d) -- not building", idxNum);
        return false;
    }

    uint8_t*  tangData   = (uint8_t*)twkP(lod, LODRD_TANGDATA);
    const int tangStride = twkI(lod, LODRD_TANGSTRIDE);
    const bool tangHi    = *(uint8_t*)(lod + LODRD_TANGHIPREC) != 0;

    const int  maxInfl = twkI(lod, LODRD_SKINVB + SKINVB_MAXINFL);
    const bool bone16  = *(uint8_t*)(lod + LODRD_SKINVB + SKINVB_16BITBONE) != 0;
    const bool varBone = *(uint8_t*)(lod + LODRD_SKINVB + SKINVB_VARBONES) != 0;
    const int  skinStride = maxInfl * (bone16 ? 2 : 1) + maxInfl;
    uint16_t*  boneMap    = (uint16_t*)twkP(sec, SEC_BONEMAP);
    const int  boneMapNum = twkI(sec, SEC_BONEMAP + 8);
    const bool useSkin = skinData && !varBone && maxInfl > 0 && maxInfl <= 12
                      && boneMap && boneMapNum > 0;
    if (!useSkin)
        TwkLog("[real] skin weights unusable (var=%d maxInfl=%d) -- binding to one bone",
               varBone ? 1 : 0, maxInfl);

    // ---- which skeleton bones hold cloth tight against the body
    uint8_t* boneInfo = (uint8_t*)twkP((uint8_t*)mesh + SM_REFSKELETON, REFSK_BONEINFO);
    const int nBoneInfo = twkI((uint8_t*)mesh + SM_REFSKELETON, REFSK_BONEINFO + 8);

    float*    rawPos  = (float*)   malloc((size_t)nVerts * 12);
    uint32_t* tri     = (uint32_t*)malloc((size_t)nTris * 3 * 4);
    bool ok = rawPos && tri;
    int nUnique = 0, nFixed = 0, maxUsedInfl = 1, nUsedBones = 0, nTight = 0;
    float zMin = 1e9f, zMax = -1e9f;

    float* verts = nullptr; float* norms = nullptr; float* invMass = nullptr; float* maxDist = nullptr;
    uint8_t* boneDat = nullptr; int16_t* boneRemap = nullptr; int* usedBones = nullptr;
    uint8_t* tightBone = nullptr;

    if (ok) {
        for (int i = 0; i < nVerts; i++) {
            const float* v = (const float*)(posData + (size_t)(baseVert + i) * posStride);
            rawPos[i*3+0] = v[0]; rawPos[i*3+1] = v[1]; rawPos[i*3+2] = v[2];
            if (v[2] < zMin) zMin = v[2];
            if (v[2] > zMax) zMax = v[2];
        }
        ok = BuildWeld(rawPos, nVerts);
    }
    if (ok) {
        nUnique = g_weldUnique;
        verts     = (float*)  malloc((size_t)nUnique * 12);
        norms     = (float*)  calloc((size_t)nUnique, 12);
        invMass   = (float*)  malloc((size_t)nUnique * 4);
        maxDist   = (float*)  malloc((size_t)nUnique * 4);
        boneDat   = (uint8_t*)calloc((size_t)nUnique, BONEDATA_STRIDE);
        boneRemap = (int16_t*)malloc(1024 * sizeof(int16_t));
        usedBones = (int*)    malloc(256 * sizeof(int));
        tightBone = (uint8_t*)calloc(1024, 1);
        ok = verts && norms && invMass && maxDist && boneDat && boneRemap && usedBones && tightBone;
    }

    if (ok) {
        for (int i = 0; i < 1024; i++) boneRemap[i] = -1;

        // classify every skeleton bone once, by name
        for (int b = 0; b < nBoneInfo && b < 1024 && boneInfo; b++) {
            char nm[64];
            if (CatchSound_FNameText(boneInfo + (size_t)b * BONEINFO_STRIDE, nm, sizeof(nm))
                && BoneIsTight(nm)) { tightBone[b] = 1; nTight++; }
        }

        for (int i = 0; i < nVerts; i++) {
            const int u = g_weld[i];
            verts[u*3+0] = rawPos[i*3+0]; verts[u*3+1] = rawPos[i*3+1]; verts[u*3+2] = rawPos[i*3+2];
        }

        for (int t = 0; t < nTris * 3; t++) {
            const int gi = baseIdx + t;
            const uint32_t vi = (idxSize == 2) ? (uint32_t)((uint16_t*)idxData)[gi]
                                               : ((uint32_t*)idxData)[gi];
            const int local = (int)vi - baseVert;
            tri[t] = (local >= 0 && local < nVerts) ? (uint32_t)g_weld[local] : 0u;
        }

        // ---- normals: the mesh's own, averaged across the copies that were welded together
        bool haveReal = tangData && tangStride > 0;
        if (haveReal) {
            for (int i = 0; i < nVerts; i++) {
                float tg[3], nr[3];
                if (!DecodeTangentBasis(tangData, tangStride, tangHi, baseVert + i, tg, nr)) {
                    haveReal = false; break;
                }
                const int u = g_weld[i];
                norms[u*3+0] += nr[0]; norms[u*3+1] += nr[1]; norms[u*3+2] += nr[2];
            }
        }
        if (!haveReal) {
            memset(norms, 0, (size_t)nUnique * 12);
            for (int t = 0; t < nTris; t++) {
                const uint32_t a = tri[t*3], b = tri[t*3+1], c = tri[t*3+2];
                const float* A = verts + a*3; const float* B = verts + b*3; const float* C = verts + c*3;
                const float e1[3] = { B[0]-A[0], B[1]-A[1], B[2]-A[2] };
                const float e2[3] = { C[0]-A[0], C[1]-A[1], C[2]-A[2] };
                const float n[3]  = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                                      e1[0]*e2[1]-e1[1]*e2[0] };
                const uint32_t v3[3] = { a, b, c };
                for (int k = 0; k < 3; k++) {
                    norms[v3[k]*3+0] += n[0]; norms[v3[k]*3+1] += n[1]; norms[v3[k]*3+2] += n[2];
                }
            }
        }
        for (int i = 0; i < nUnique; i++) {
            float* n = norms + i*3;
            const float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-6f) { n[0] /= len; n[1] /= len; n[2] /= len; }
            else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
        }
        // ---- WINDING, measured rather than assumed. The solver recomputes cloth normals every
        // frame from the triangle winding of the mesh we hand it, so if our winding runs opposite
        // to the garment's own, every normal comes out inverted: light arrives from behind and the
        // garment goes soft and formless. The garment's real normals are right here, so compare
        // against them and settle it -- no guessing, and it adapts per garment.
        int agree = 0, disagree = 0;
        if (haveReal) {
            const int step = (nTris > 2048) ? nTris / 2048 : 1;
            for (int t = 0; t < nTris; t += step) {
                const uint32_t a = tri[t*3], b = tri[t*3+1], c = tri[t*3+2];
                if (a == b || b == c || a == c) continue;
                const float* A = verts + a*3; const float* B = verts + b*3; const float* C = verts + c*3;
                const float e1[3] = { B[0]-A[0], B[1]-A[1], B[2]-A[2] };
                const float e2[3] = { C[0]-A[0], C[1]-A[1], C[2]-A[2] };
                const float fn[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                                      e1[0]*e2[1]-e1[1]*e2[0] };
                const float mn[3] = { norms[a*3+0]+norms[b*3+0]+norms[c*3+0],
                                      norms[a*3+1]+norms[b*3+1]+norms[c*3+1],
                                      norms[a*3+2]+norms[b*3+2]+norms[c*3+2] };
                const float d = fn[0]*mn[0] + fn[1]*mn[1] + fn[2]*mn[2];
                if (d > 0.0f) agree++; else if (d < 0.0f) disagree++;
            }
        }
        const bool autoFlip = ClothSim_AutoWinding && disagree > agree;
        if (autoFlip) {
            for (int t = 0; t < nTris; t++) {
                const uint32_t sw = tri[t*3+1]; tri[t*3+1] = tri[t*3+2]; tri[t*3+2] = sw;
            }
        }
        TwkLog("[real] winding vs the garment: %d agree, %d disagree -> %s",
               agree, disagree, autoFlip ? "REVERSED to match" : "kept as-is");

        if (ClothSim_FlipNormals) {
            for (int i = 0; i < nUnique * 3; i++) norms[i] = -norms[i];
            for (int t = 0; t < nTris; t++) {           // winding too, or the solver disagrees with us
                const uint32_t sw = tri[t*3+1]; tri[t*3+1] = tri[t*3+2]; tri[t*3+2] = sw;
            }
        }
        // Vertex colour often carries baked ambient occlusion or tint for character materials. If
        // the merged body has it and the separated garment does not, shading differs for that reason
        // alone -- worth knowing, since normals have now been measured correct.
        TwkLog("[real] normals: %s%s | vertexColours: data=%p num=%d",
               haveReal ? "from the mesh" : "derived from winding",
               ClothSim_FlipNormals ? " (FLIPPED)" : "",
               twkP(lod, 0x108 + 0x28), twkI(lod, 0x108 + 0x34));

        // ---- bone data, per welded particle (first render copy wins -- they are the same point)
        uint8_t* seen = (uint8_t*)calloc((size_t)nUnique, 1);
        const float zSpan = (zMax - zMin) > 0.001f ? (zMax - zMin) : 1.0f;
        for (int i = 0; i < nVerts; i++) {
            const int u = g_weld[i];
            if (seen && seen[u]) continue;
            if (seen) seen[u] = 1;

            uint8_t*  bd   = boneDat + (size_t)u * BONEDATA_STRIDE;
            uint16_t* bIdx = (uint16_t*)(bd + 0x04);
            float*    bWt  = (float*)   (bd + 0x1c);
            int count = 0; int domBone = -1; float domW = -1.0f;
            if (useSkin) {
                const uint8_t* sw  = skinData + (size_t)(baseVert + i) * skinStride;
                const uint8_t* wts = sw + maxInfl * (bone16 ? 2 : 1);
                for (int k = 0; k < maxInfl && count < 12; k++) {
                    const int w = wts[k];
                    if (w == 0) continue;
                    const int secBone = bone16 ? (int)((const uint16_t*)sw)[k] : (int)sw[k];
                    if (secBone < 0 || secBone >= boneMapNum) continue;
                    const int skelBone = boneMap[secBone];
                    if (skelBone < 0 || skelBone >= 1024) continue;
                    if (boneRemap[skelBone] < 0) {
                        if (nUsedBones >= 256) continue;
                        boneRemap[skelBone] = (int16_t)nUsedBones;
                        usedBones[nUsedBones++] = skelBone;
                    }
                    bIdx[count] = (uint16_t)boneRemap[skelBone];
                    bWt[count]  = (float)w / 255.0f;
                    if ((float)w > domW) { domW = (float)w; domBone = skelBone; }
                    count++;
                }
            }
            if (count == 0) {
                if (nUsedBones == 0) { usedBones[nUsedBones++] = 0; boneRemap[0] = 0; }
                bIdx[0] = 0; bWt[0] = 1.0f; count = 1;
            }
            *(int*)(bd + 0x00) = count;
            if (count > maxUsedInfl) maxUsedInfl = count;

            // ---- how far this particle may stray. Height gives the hem its swing; a cuff or collar
            // is held regardless of how low the arm happens to hang.
            const float h = (verts[u*3+2] - zMin) / zSpan;
            float d = 0.0f;
            if (h < ClothSim_PinAbove && ClothSim_PinAbove > 0.001f) {
                // t: 0 at the pin line, 1 at the bottom edge. Travel climbs to the peak and then
                // eases back, so the loose middle moves most and the hem keeps its shape.
                const float t = (ClothSim_PinAbove - h) / ClothSim_PinAbove;
                const float pk = (ClothSim_PeakAt < 0.05f) ? 0.05f
                               : (ClothSim_PeakAt > 0.95f) ? 0.95f : ClothSim_PeakAt;
                const float grip = g_buildLower ? ClothSim_HemGripLower : ClothSim_HemGrip;
                d = (t <= pk) ? (t / pk)
                              : (1.0f - (1.0f - grip) * ((t - pk) / (1.0f - pk)));
                d *= ClothSim_MaxTravel;
            }
            if (domBone >= 0 && domBone < 1024 && tightBone[domBone]) d = 0.0f;
            maxDist[u] = d;
            invMass[u] = (d <= 0.001f) ? 0.0f : 1.0f;
            if (invMass[u] == 0.0f) nFixed++;
        }
        // ---- hem push: ease the bottom band outward from the garment's own vertical axis, so the
        // edge rests proud of whatever is underneath. The simulation has no body to collide with, so
        // the rest shape is the only thing that can hold a hem out of a pair of trousers.
        if (ClothSim_HemPush > 0.001f && ClothSim_HemPushBand > 0.01f && nUnique > 0) {
            float cx = 0.0f, cy = 0.0f;
            for (int u = 0; u < nUnique; u++) { cx += verts[u*3+0]; cy += verts[u*3+1]; }
            cx /= (float)nUnique; cy /= (float)nUnique;
            int pushed = 0;
            for (int u = 0; u < nUnique; u++) {
                const float h = (verts[u*3+2] - zMin) / zSpan;      // 0 at the bottom edge
                if (h > ClothSim_HemPushBand) continue;
                const float w = 1.0f - (h / ClothSim_HemPushBand);  // full at the very bottom
                float dx = verts[u*3+0] - cx, dy = verts[u*3+1] - cy;
                const float len = sqrtf(dx*dx + dy*dy);
                if (len < 0.01f) continue;
                const float k = ClothSim_HemPush * w / len;
                verts[u*3+0] += dx * k; verts[u*3+1] += dy * k;
                pushed++;
            }
            TwkLog("[real] hem pushed out %.1f cm over the bottom %.0f%% (%d particles)",
                   ClothSim_HemPush, ClothSim_HemPushBand * 100.0f, pushed);
        }
        free(seen);

        ok = MakeArray(clothLod + PMD_VERTS,    verts,   nUnique,   12)
          && MakeArray(clothLod + PMD_NORMALS,  norms,   nUnique,   12)
          && MakeArray(clothLod + PMD_INDICES,  tri,     nTris * 3,  4)
          && MakeArray(clothLod + PMD_INVMASS,  invMass, nUnique,    4)
          && MakeArray(clothLod + PMD_BONEDATA, boneDat, nUnique, BONEDATA_STRIDE)
          && MakeSingleEntryWeightMap(clothLod + PMD_WEIGHTMAPS, 1u, maxDist, nUnique)
          && MakeArray((uint8_t*)asset + CA_USEDBONEIDX, usedBones,
                       nUsedBones ? nUsedBones : 1, 4);
        if (ok) {
            *(int*)(clothLod + PMD_MAXBONEW) = maxUsedInfl;
            *(int*)(clothLod + PMD_NUMFIXED) = nFixed;
            g_realVerts = nUnique; g_realTris = nTris; g_realBones = nUsedBones;
            TwkLog("[real] sim mesh: %d render verts welded to %d particles, %d tris, %d bones, "
                   "%d pinned (%d tight bones, z %.1f..%.1f, travel=%.1fcm)",
                   nVerts, nUnique, nTris, nUsedBones, nFixed, nTight, zMin, zMax, ClothSim_MaxTravel);
        }
    }

    free(rawPos); free(tri); free(verts); free(norms); free(invMass); free(maxDist);
    free(boneDat); free(boneRemap); free(usedBones); free(tightBone);
    if (!ok) { FreeWeld(); TwkLog("[real] build failed -- falling back to the test quad"); }
    return ok;
}


static void* BuildQuadAsset(void* mesh) {
    void* cls = ClothMerge_FindClass(L"ClothingAssetCommon");
    if (!cls) { TwkLog("[quad] UClothingAssetCommon class not found"); return nullptr; }
    void* asset = ClothMerge_NewObject(cls, mesh);
    if (!asset) { TwkLog("[quad] asset construction returned null"); return nullptr; }

    if (!MakeArray((uint8_t*)asset + CA_LODDATA, nullptr, 1, LOD_STRIDE)) return nullptr;
    uint8_t* lod = *(uint8_t**)((uint8_t*)asset + CA_LODDATA);

    // Prefer simulating the garment itself. The quad below stays as the fallback: if anything about
    // the mesh is not what we expect, a known-good test shape beats a half-built one.
    uint8_t*  rdp   = (uint8_t*)twkP(mesh, SM_RENDERDATA);
    uint8_t** lodsp = rdp ? (uint8_t**)twkP(rdp, RD_LODARRAY) : nullptr;
    uint8_t*  lodRD = (lodsp && twkI(rdp, RD_LODARRAY + 8) > 0) ? lodsp[0] : nullptr;
    bool realBuilt = false;
    if (g_realMesh && lodRD) {
        __try { realBuilt = BuildRealMeshData(mesh, lodRD, lod, asset); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            realBuilt = false;
            TwkLog("[real] faulted while reading the garment -- falling back to the test quad");
        }
    }

    // ---- the quad. Local space, hanging BELOW the reference bone so gravity has somewhere to take
    // it: two pinned verts on top (inverse mass 0 = infinite mass = immovable), two free below.
    if (!realBuilt) {
    const float S = 20.0f;                                   // 20 cm square -- modest but visible
    const float verts[4][3]   = { {-S, 0.0f, 0.0f}, { S, 0.0f, 0.0f },
                                  {-S, 0.0f,   -S}, { S, 0.0f,   -S} };
    const float normals[4][3] = { {0.0f,-1.0f,0.0f}, {0.0f,-1.0f,0.0f},
                                  {0.0f,-1.0f,0.0f}, {0.0f,-1.0f,0.0f} };
    const uint32_t idx[6]     = { 0, 2, 1,  1, 2, 3 };       // two triangles, consistent winding
    const float invMass[4]    = { 0.0f, 0.0f, 1.0f, 1.0f };  // top row pinned, bottom row free

    if (!MakeArray(lod + PMD_VERTS,   verts,   4, 12)) return nullptr;
    if (!MakeArray(lod + PMD_NORMALS, normals, 4, 12)) return nullptr;
    if (!MakeArray(lod + PMD_INDICES, idx,     6,  4)) return nullptr;
    if (!MakeArray(lod + PMD_INVMASS, invMass, 4,  4)) return nullptr;

    // ---- bone data: every particle bound 100% to entry 0 of the asset's OWN UsedBoneIndices list
    // (an asset-local index the engine remaps through, not a skeleton index).
    uint8_t bone[4 * BONEDATA_STRIDE];
    memset(bone, 0, sizeof(bone));
    for (int i = 0; i < 4; i++) {
        uint8_t* b = bone + i * BONEDATA_STRIDE;
        *(int*)(b + 0x00) = 1;        // NumInfluences
        b[0x04] = 0;                  // BoneIndices[0] = 0
        *(float*)(b + 0x1c) = 1.0f;   // BoneWeights[0] = 1
    }
    if (!MakeArray(lod + PMD_BONEDATA, bone, 4, BONEDATA_STRIDE)) return nullptr;
    *(int*)(lod + PMD_MAXBONEW) = 1;
    *(int*)(lod + PMD_NUMFIXED) = 2;
    }

    // ---- identity. Assets are matched to render sections BY GUID (GetClothingAssetsInUse xors
    // the section's guid against this field), so a zero guid is an asset nothing can ever refer to.
    memcpy((uint8_t*)asset + CA_ASSETGUID, kQuadGuid, 16);

    // ---- MaxDistance: how far each particle may stray from its animated position. The pinned top
    // row gets 0 (welded to the animation), the free bottom row a generous 30 cm so the quad has
    // somewhere to swing. This is the map the solver needs before it will build an actor at all.
    // The real-garment path builds its own per-vertex map (ramped by height), so leave it alone --
    // writing this 4-entry one over it would throw away every vertex past the third.
    if (!realBuilt) {
        const float maxDist[4] = { 0.0f, 0.0f, 30.0f, 30.0f };
        if (!MakeSingleEntryWeightMap(lod + PMD_WEIGHTMAPS, 1u /*EWeightMapTargetCommon::MaxDistance,
                                      read out of the exe's own enum table, not assumed*/, maxDist, 4))
            return nullptr;
    }

    // ---- asset-level binding. Bone 0 is the skeleton root, which every Session rig has (the
    // 70-bone census), so the quad hangs off the character whatever garment or body is worn.
    const int zero = 0;
    // LodMap is indexed by the MESH's LOD, mapping each to an asset LOD -- one entry is only
    // correct for a one-LOD mesh, and a garment has several, so the component's current LOD would
    // index past the end. Size it to the mesh and point every LOD at our single asset LOD.
    int meshLods = twkI(mesh, SM_LODINFO + 8);
    if (meshLods < 1 || meshLods > 16) meshLods = 1;
    int lodMap[16];
    for (int i = 0; i < meshLods; i++) lodMap[i] = 0;
    if (!MakeArray((uint8_t*)asset + CA_LODMAP, lodMap, meshLods, 4)) return nullptr;
    TwkLog("[quad] LodMap sized to %d mesh LOD(s); source=%s", meshLods, realBuilt ? "GARMENT MESH" : "test quad");
    if (!realBuilt && !MakeArray((uint8_t*)asset + CA_USEDBONEIDX, &zero, 1, 4)) return nullptr;
    *(int*)((uint8_t*)asset + CA_REFBONEIDX) = 0;

    __try { g_addCfg(asset); }
    __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[quad] AddClothConfigs faulted"); return nullptr; }

    // ---- how much of the world's WIND reaches this garment -------------------------------------
    // Wind is a world-space force, so it pulls the same way whichever way the skater faces -- which is
    // exactly what a hem leaning permanently to one side looks like. Gravity is already overridden to
    // nothing, so wind is the only steady force left.
    //
    // UClothingAssetCommon::ClothConfigs is a TMap<FName, UClothConfigBase*> at +0x50. Its element is
    // an 8-byte FName followed by the pointer, so the config is at +0x08 -- an earlier attempt read
    // +0x10 (the hash fields), came back null, and did nothing at all. That silent no-op is why wind
    // was wrongly cleared of causing this. The log below prints what it actually found, so a repeat
    // cannot hide.
    __try {
        void* cfg = twkP(twkP(asset, CA_CONFIGS), CFGMAP_VALUE);
        char cn[96];
        if (cfg && CatchSound_ObjName(cfg, cn, sizeof(cn))) {
            float* drag = (float*)((uint8_t*)cfg + CFG_WINDDRAG);
            float* lift = (float*)((uint8_t*)cfg + CFG_WINDLIFT);
            const float d0 = *drag, l0 = *lift;
            const float k = (float)g_windPct / 100.0f;
            *drag = d0 * k; *lift = l0 * k;
            TwkLog("[quad] wind on '%s': drag %.4f -> %.4f, lift %.4f -> %.4f (%d%%)",
                   cn, d0, *drag, l0, *lift, g_windPct);
        } else {
            TwkLog("[quad] no cloth config found to set the wind on -- wind is left as the engine made it");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[quad] setting the wind faulted"); }

    return asset;
}

// Append to the mesh's cloth list. Every Session mesh ships an EMPTY one (the game has no cloth), so
// this is an allocate-and-fill, not a resize -- and it refuses if anything is already there, because
// growing an engine-owned buffer needs the engine's growth policy, not ours.
// mesh+0x15f is a BITFIELD, and bHasActiveClothingAssets is bit 4 of it -- the engine reads it as
// (byte >> 4) & 1. Writing the byte outright both misses that flag and wipes the seven unrelated
// flags sharing it, which matters because this mesh is a shared cooked asset other characters wear.
static void*   g_flagMesh[kSimSlots] = {};
static uint8_t g_flagOrig[kSimSlots] = {};

static void RestoreMeshFlag(void* mesh) {
    for (int i = 0; i < kSimSlots; i++)
        if (g_flagMesh[i] && (!mesh || g_flagMesh[i] == mesh)) {
            *(uint8_t*)((uint8_t*)g_flagMesh[i] + SM_HASCLOTH) = g_flagOrig[i];
            g_flagMesh[i] = nullptr;
        }
}

// A garment we have already prepared keeps our cloth asset ON THE MESH, and these are shared assets
// that outlive the character wearing them. So a garment worn, changed away from, then worn again
// arrives with its asset still attached -- AttachToMesh would refuse to append and the garment would
// come back with no cloth. Remember what we built and re-adopt it instead.
enum { kBuiltMeshes = 16 };
static void* g_doneMesh[kBuiltMeshes]  = {};
static void* g_doneAsset[kBuiltMeshes] = {};

static void* AlreadyBuilt(void* mesh) {
    for (int i = 0; i < kBuiltMeshes; i++) if (g_doneMesh[i] == mesh) return g_doneAsset[i];
    return nullptr;
}
static void RememberBuilt(void* mesh, void* asset) {
    for (int i = 0; i < kBuiltMeshes; i++)
        if (!g_doneMesh[i] || g_doneMesh[i] == mesh) { g_doneMesh[i] = mesh; g_doneAsset[i] = asset; return; }
}
static void MarkMeshHasCloth(void* mesh) {
    uint8_t* flag = (uint8_t*)mesh + SM_HASCLOTH;
    bool known = false;
    for (int i = 0; i < kSimSlots; i++) if (g_flagMesh[i] == mesh) known = true;
    if (!known)
        for (int i = 0; i < kSimSlots; i++)
            if (!g_flagMesh[i]) { g_flagMesh[i] = mesh; g_flagOrig[i] = *flag; break; }
    *flag |= 0x10;                                       // bHasActiveClothingAssets, bit 4
}

static bool AttachToMesh(void* mesh, void* asset) {
    uint8_t* arr = (uint8_t*)mesh + SM_MESHCLOTH;
    const int num = *(int*)(arr + 8);
    if (num != 0) { TwkLog("[quad] mesh already has %d cloth asset(s) -- refusing to append", num); return false; }
    void* one = asset;
    if (!MakeArray(arr, &one, 1, 8)) return false;
    RestoreMeshFlag(mesh);                               // put this garment back before re-marking
    MarkMeshHasCloth(mesh);
    RememberBuilt(mesh, asset);
    return true;
}


// ---------------------------------------------------------------------------------------------
// Mark the garment's render sections as using our cloth asset.
//
// This is THE gate, and it is not obvious: RecreateClothingActors does not build an actor per entry
// in the mesh's cloth list. It first asks USkeletalMesh::GetClothingAssetsInUse which assets are
// actually referenced, and that answer comes purely from the RENDER data -- each section's
// CorrespondClothAssetIndex. An asset the render sections never point at is skipped silently, with
// no actor and no complaint, which is exactly the actors=0 we measured for two rounds.
//
// Only the index is written, never ClothMappingData. The renderer decides a section is clothed by
// HasClothingData() (ClothMappingData.Num() > 0), so leaving that empty means the draw path is
// bit-for-bit unchanged and this cannot corrupt anything on screen -- it only makes the asset
// visible to the simulation. That distinction is what makes this safe to try on a SHARED cooked
// mesh that NPCs and other players are also wearing; the originals are saved and put back.
struct SecSave { void* owner; uint8_t* sec; int16_t origIdx; uint8_t origCloth[20]; uint8_t origMap[16];
                 int origTris; };
static SecSave g_secSave[64];
static int     g_secSaved = 0;

// Restore only the garment named, or everything when owner is null: several garments are patched
// at once now, so a blanket restore would undo the others.
static void RestoreSections(void* owner) {
    int kept = 0;
    for (int i = 0; i < g_secSaved; i++) {
        uint8_t* sec = g_secSave[i].sec;
        if (!sec) continue;
        if (owner && g_secSave[i].owner != owner) { g_secSave[kept++] = g_secSave[i]; continue; }
        *(int16_t*)(sec + SEC_CLOTHASSETID) = g_secSave[i].origIdx;
        memcpy(sec + SEC_CLOTHDATA, g_secSave[i].origCloth, 20);
        // The mapping array is what makes the renderer treat a section as cloth, so it matters most
        // of all that this goes back -- other characters wear this same garment.
        memcpy(sec + SEC_CLOTHMAPPING, g_secSave[i].origMap, 16);
        *(int*)(sec + SEC_NUMTRIS) = g_secSave[i].origTris;
    }
    if (g_secSaved != kept) TwkLog("[quad] restored %d render section(s) to their shipped state",
                                   g_secSaved - kept);
    g_secSaved = kept;
}

// Every section is marked, and the FIRST is the one bound. Restricting this to the garment's own
// material slot is correct in principle and was tried (3.02.x): the simulation is built per MESH, so
// binding a non-first section found no matching weld map and cloth stopped entirely, and hiding the
// other sections by zeroing their triangle count crashed a render worker. Both are reverted. Doing it
// properly means building the simulation per SECTION, not per mesh -- wantMatIdx is logged so the
// mismatch stays visible.
static int MarkSectionsInUse(void* mesh, int wantMatIdx) {
    RestoreSections(mesh);
    uint8_t* rd = (uint8_t*)twkP(mesh, SM_RENDERDATA);
    if (!rd) { TwkLog("[quad] no render data on the garment -- cannot bind sections"); return 0; }
    uint8_t** lods = (uint8_t**)twkP(rd, RD_LODARRAY);      // TIndirectArray holds POINTERS
    const int nLods = twkI(rd, RD_LODARRAY + 8);
    if (!lods || nLods <= 0 || nLods > 16) { TwkLog("[quad] odd LOD count %d -- not binding", nLods); return 0; }

    int marked = 0;
    for (int l = 0; l < nLods; l++) {
        uint8_t* lod = lods[l];
        if (!lod) continue;
        uint8_t* secs = (uint8_t*)twkP(lod, LODRD_SECTIONS);
        const int nSec = twkI(lod, LODRD_SECTIONS + 8);
        if (!secs || nSec <= 0 || nSec > 64) continue;
        for (int sIdx = 0; sIdx < nSec; sIdx++) {
            uint8_t* sec = secs + (size_t)sIdx * SEC_STRIDE;
            if (l == 0 && nSec > 1)      // multi-section garments: the layout, once, in plain numbers
                TwkLog("[quad]   section %d: material %d, verts %d starting at %d", sIdx,
                       (int)*(uint16_t*)(sec + SEC_MATIDX), twkI(sec, SEC_NUMVERTS),
                       twkI(sec, SEC_BASEVERT));
            if (g_secSaved < 64) {
                g_secSave[g_secSaved].owner   = mesh;
                g_secSave[g_secSaved].sec     = sec;
                g_secSave[g_secSaved].origIdx = *(int16_t*)(sec + SEC_CLOTHASSETID);
                memcpy(g_secSave[g_secSaved].origCloth, sec + SEC_CLOTHDATA, 20);
                memcpy(g_secSave[g_secSaved].origMap,   sec + SEC_CLOTHMAPPING, 16);
                g_secSave[g_secSaved].origTris = twkI(sec, SEC_NUMTRIS);
                g_secSaved++;
            }
            // The guid is the part that matters -- it is what the asset lookup matches on. The
            // index is written too because the skinning path uses that one instead.
            memcpy(sec + SEC_CLOTHDATA, kQuadGuid, 16);
            *(int*)(sec + SEC_CLOTHDATA + 16) = 0;           // AssetLodIndex
            *(int16_t*)(sec + SEC_CLOTHASSETID) = 0;
            marked++;
        }
    }
    TwkLog("[quad] bound %d render section(s) across %d LOD(s) to cloth asset 0 (garment material %d)",
           marked, nLods, wantMatIdx);
    return marked;
}


// One shot of hard numbers after a bind: every gate between us and a running solver, read rather
// than reasoned about. The interesting one is inUse -- 0 means our section binding never reached
// the filter list, 1 means it did and the refusal is inside CreateActor's world checks.
static void LogGates(void* comp, void* mesh) {
    void* boundMesh = twkP(comp, SMC_SKELMESH);
    void* world     = twkP(comp, SMC_WORLD);
    void* physScene = world ? twkP(world, WORLD_PHYSSCENE) : nullptr;
    const int wflags = world ? twkI(world, WORLD_FLAGS) : 0;

    int inUse = -1;
    if (g_assetsInUse) {
        void* out[3] = { nullptr, nullptr, nullptr };   // TArray {Data, Num, Max}, zeroed
        __try { g_assetsInUse(mesh, out); inUse = *(int*)((uint8_t*)out + 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { inUse = -2; }
        // the engine allocated Out's buffer; we deliberately leak those few bytes rather than guess
        // at the matching free -- this runs once per garment change on a dev-only path.
    }
    TwkLog("[quad] gates: inUse=%d | meshMatch=%d | registered=%d | clothNum=%d | factory=%p | sim=%p",
           inUse, boundMesh == mesh ? 1 : 0, twkI(comp, SMC_REGFLAGS) & 1,
           twkI(mesh, SM_MESHCLOTH + 8), twkP(comp, SMC_CLOTHFACTORY), twkP(comp, SMC_CLOTHSIM));
    TwkLog("[quad] gates: world=%p physScene=%p simPhysics=%d",
           world, physScene, (wflags & 4) ? 1 : 0);

    // The actor exists but something must still wake it. ShouldSimulate() is literally "does any
    // actor have CurrentLodIndex != -1", so recompute it here rather than calling through the
    // vtable -- same answer, nothing to get wrong about calling convention.
    void* sim = twkP(comp, SMC_CLOTHSIM);
    if (sim) {
        uint8_t* actors = (uint8_t*)twkP(sim, NVSIM_ACTORS);
        const int nAct  = twkI(sim, NVSIM_ACTORS + 8);
        int awake = 0, curLod = -99, lodDataNum = -1;
        for (int i = 0; i < nAct && actors; i++) {
            uint8_t* a = actors + (size_t)i * ACTOR_STRIDE;
            const int cl = twkI(a, ACTOR_CURLOD);
            if (i == 0) { curLod = cl; lodDataNum = twkI(a, ACTOR_LODDATA + 8); }
            if (cl != -1) awake++;
        }
        TwkLog("[quad] actor: n=%d curLod=%d lodData=%d | lodMapNum=%d predLod=%d masterPose=%p | shouldSim=%d",
               nAct, curLod, lodDataNum,
               g_asset ? twkI(g_asset, CA_LODMAP + 8) : -1,
               twkI(comp, SMC_PREDLOD), twkP(comp, SMC_MASTERPOSE), awake > 0 ? 1 : 0);
    }
}


// ---------------------------------------------------------------------------------------------
// Read-only recon for the NEXT phase: driving the garment's real vertices instead of a test quad.
// That needs the mesh's positions, triangles and skin weights on the CPU, and cooked builds often
// strip exactly that (bNeedsCPUAccess false, Data null). Whether these pointers are live decides
// the whole shape of the next step, so it is worth knowing before building anything on top.
static void LogGeometry(void* mesh) {
    uint8_t* rd = (uint8_t*)twkP(mesh, SM_RENDERDATA);
    if (!rd) return;
    uint8_t** lods = (uint8_t**)twkP(rd, RD_LODARRAY);
    const int nLods = twkI(rd, RD_LODARRAY + 8);
    if (!lods || nLods <= 0) return;
    uint8_t* lod = lods[0];
    if (!lod) return;

    uint8_t* secs = (uint8_t*)twkP(lod, LODRD_SECTIONS);
    const int nSec = twkI(lod, LODRD_SECTIONS + 8);
    if (secs && nSec > 0)
        TwkLog("[geo] section0: verts=%d tris=%d baseIdx=%d baseVert=%d maxInfl=%d clothMap=%d",
               twkI(secs, 0x38), twkI(secs, 0x08), twkI(secs, 0x04), twkI(secs, 0x10),
               twkI(secs, 0x3c), twkI(secs, SEC_CLOTHMAPPING + 8));

    void* posData = twkP(lod, LODRD_POSVB + POSVB_DATA);
    TwkLog("[geo] positions: data=%p stride=%d num=%d cpuAccess=%d",
           posData, twkI(lod, LODRD_POSVB + POSVB_STRIDE), twkI(lod, LODRD_POSVB + POSVB_NUMVERTS),
           *(uint8_t*)(lod + LODRD_POSVB + POSVB_CPUACCESS));

    TwkLog("[geo] skinWeights: data=%p num=%d maxInfl=%d bone16=%d cpuAccess=%d",
           twkP(lod, LODRD_SKINVB + SKINVB_DATA), twkI(lod, LODRD_SKINVB + SKINVB_NUMVERTS),
           twkI(lod, LODRD_SKINVB + SKINVB_MAXINFL),
           *(uint8_t*)(lod + LODRD_SKINVB + SKINVB_16BITBONE),
           *(uint8_t*)(lod + LODRD_SKINVB + SKINVB_CPUACCESS));

    TwkLog("[geo] indices: typeSize=%d buffer=%p",
           *(uint8_t*)(lod + LODRD_IDXCONT), twkP(lod, LODRD_IDXCONT + 8));

    // Sanity-check the positions actually look like a garment: three verts, in centimetres, and
    // a crude bounding box. Garbage here means the pointer is live but not what we think it is.
    const int stride = twkI(lod, LODRD_POSVB + POSVB_STRIDE);
    const int nv     = twkI(lod, LODRD_POSVB + POSVB_NUMVERTS);
    if (posData && stride >= 12 && nv > 0) {
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        const int step = nv > 512 ? nv / 512 : 1;
        for (int i = 0; i < nv; i += step) {
            const float* v = (const float*)((uint8_t*)posData + (size_t)i * stride);
            for (int k = 0; k < 3; k++) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
        }
        const float* v0 = (const float*)posData;
        TwkLog("[geo] v0=(%.1f %.1f %.1f) bounds=(%.1f..%.1f, %.1f..%.1f, %.1f..%.1f)",
               v0[0], v0[1], v0[2], mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }
}


// ---------------------------------------------------------------------------------------------
// Make the simulated shape actually show up: bind the render section to the cloth data.
//
// Two pieces have to land TOGETHER. The renderer decides a section is cloth by
// ClothMappingData.Num() > 0, so writing the mapping without also filling the GPU cloth buffer
// makes it build cloth vertex factories against an empty buffer. There is no safe half-step here.
//
// The mapping is trivial only because the sim mesh IS the render mesh: render vertex i is sim
// vertex i, so each entry names a triangle with i first and barycentric weight (1,0,0), which
// reconstructs to exactly vertex i. The w component is the offset along the normal -- 0 for the
// position (sit ON the vertex) and 1 for normal/tangent, whose reconstruction subtracts the
// position and so yields the vertex normal itself.
// Meshes whose ClothVertexBuffer we have already handed to BeginInitResource.
//
// ⚠️ NEVER cleared, and deliberately NOT part of the release registry. BeginInitResource links a render
// resource into the engine's global list, and there is no matching release here -- so calling it twice
// on the same buffer leaves it linked twice, and the next teardown walks a corrupt list. The garment
// mesh is a shared wardrobe asset that outlives every map, so this happened on the SECOND map change,
// every time: build (init #1) -> map change -> rebuild (init #2) -> map change -> the render thread dies
// in FBatchedReleaseResources::Flush reading 0x94e4002f. The buffer belongs to the mesh, stays valid as
// long as the mesh is loaded, and only its CONTENTS need refilling on a rebuild -- so initialise once
// and refill thereafter.
enum { kVBInitMeshes = 16 };
static void* g_vbInitMesh[kVBInitMeshes] = {};
static bool VBAlreadyInit(void* mesh) {
    for (int i = 0; i < kVBInitMeshes; i++) if (g_vbInitMesh[i] == mesh) return true;
    return false;
}
static void VBMarkInit(void* mesh) {
    for (int i = 0; i < kVBInitMeshes; i++)
        if (!g_vbInitMesh[i]) { g_vbInitMesh[i] = mesh; return; }
}

static bool BuildClothMapping(void* mesh, uint8_t* lod, uint8_t* sec) {
    const int nVerts = twkI(sec, SEC_NUMVERTS);
    const int nTris  = twkI(sec, SEC_NUMTRIS);
    const int baseIdx = twkI(sec, SEC_BASEIDX);
    const int baseVert = twkI(sec, SEC_BASEVERT);
    void*    idxBuf  = twkP(lod, LODRD_IDXCONT + 8);
    const int idxSize = *(uint8_t*)(lod + LODRD_IDXCONT);
    uint8_t* idxData = idxBuf ? (uint8_t*)twkP(idxBuf, IDXBUF_DATA) : nullptr;
    if (!idxData || nVerts <= 0 || nTris <= 0) return false;

    uint8_t*  posData   = (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA);
    const int posStride  = twkI(lod, LODRD_POSVB + POSVB_STRIDE);
    uint8_t*  tangData   = (uint8_t*)twkP(lod, LODRD_TANGDATA);
    const int tangStride = twkI(lod, LODRD_TANGSTRIDE);
    const bool tangHi    = *(uint8_t*)(lod + LODRD_TANGHIPREC) != 0;

    // The sim mesh is WELDED, so a render vertex reads from the particle its copies collapsed into.
    // Several render vertices sharing one particle is exactly what closes the seams.
    // The weld map is built for the whole mesh, so a section reads from BaseVertexIndex onward. This
    // was indexed from 0, which is right only when the garment is the mesh's ONLY section -- on a
    // two-section garment (sweater: skin layer + sweater) the count never matched and it refused to
    // bind at all. baseVert is 0 for single-section garments, so nothing else changes.
    if (!g_weld || g_weldCount < baseVert + nVerts || g_weldUnique <= 0) {
        TwkLog("[render] no weld map for this garment -- not binding");
        return false;
    }
    const int nPart = g_weldUnique;

    uint8_t* map   = (uint8_t*)calloc((size_t)nVerts, M2M_STRIDE);
    int32_t* own   = (int32_t*)malloc((size_t)nPart * 3 * sizeof(int32_t));
    int32_t* first = (int32_t*)malloc((size_t)nPart * sizeof(int32_t));
    if (!map || !own || !first) { free(map); free(own); free(first); return false; }
    for (int i = 0; i < nPart * 3; i++) own[i] = -1;
    for (int i = 0; i < nPart; i++) first[i] = -1;
    for (int i = nVerts - 1; i >= 0; i--) first[g_weld[baseVert + i]] = i;  // a render vertex per particle

    // first triangle that mentions each vertex, with that vertex placed first
    for (int t = 0; t < nTris; t++) {
        int v[3];
        for (int k = 0; k < 3; k++) {
            const int gi = baseIdx + t * 3 + k;
            const uint32_t raw = (idxSize == 2) ? (uint32_t)((uint16_t*)idxData)[gi]
                                                : ((uint32_t*)idxData)[gi];
            const int local = (int)raw - baseVert;
            v[k] = (local >= 0 && local < nVerts) ? g_weld[baseVert + local] : -1;
        }
        if (v[0] < 0 || v[1] < 0 || v[2] < 0) continue;
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;   // collapsed by the weld
        for (int k = 0; k < 3; k++) {
            const int me = v[k];
            if (own[me*3] >= 0) continue;
            own[me*3+0] = me;
            own[me*3+1] = v[(k+1)%3];
            own[me*3+2] = v[(k+2)%3];
        }
    }

    int orphans = 0;
    for (int u = 0; u < nPart; u++)
        if (own[u*3] < 0) { own[u*3+0] = u; own[u*3+1] = u; own[u*3+2] = u; orphans++; }

    for (int i = 0; i < nVerts; i++) {
        const int u = g_weld[baseVert + i];
        uint8_t* e = map + (size_t)i * M2M_STRIDE;
        float* pos = (float*)(e + 0x00);
        float* nrm = (float*)(e + 0x10);
        float* tan = (float*)(e + 0x20);
        pos[0] = 1.0f; pos[1] = 0.0f; pos[2] = 0.0f; pos[3] = 0.0f;
        // The last value is the offset along the surface direction; its SIGN is what decides which
        // way the rebuilt surface faces. Negative turns the garment inside out to the light.
        nrm[0] = 1.0f; nrm[1] = 0.0f; nrm[2] = 0.0f;
        nrm[3] = ClothSim_LightFlip ? -1.0f : 1.0f;

        // The TANGENT cannot be written the same way. Each of these is reconstructed as a point in
        // the triangle minus the vertex position, so a (1,0,0) weighting always yields the normal
        // -- writing that for the tangent too collapses the tangent frame and the lighting with it.
        // With weights summing to one and no normal offset the result is y*(B-A) + z*(C-A), i.e.
        // any direction in the triangle plane, so solve for the y,z that best reproduce the mesh's
        // own tangent. Degenerate triangles fall back to the first edge, which is at least a real
        // direction along the surface.
        tan[0] = 0.0f; tan[1] = 1.0f; tan[2] = 0.0f; tan[3] = 0.0f;   // mode 1: along an edge
        if (ClothSim_TangentMode == 2) { tan[0] = 1.0f; tan[1] = 0.0f; tan[2] = 0.0f; tan[3] = 1.0f; }
        if (ClothSim_TangentMode == 0 && posData && posStride >= 12 && tangData && tangStride > 0) {
            const int fa = first[own[u*3+0]], fb = first[own[u*3+1]], fc = first[own[u*3+2]];
            const float* A = (const float*)(posData + (size_t)(baseVert + (fa < 0 ? i : fa)) * posStride);
            const float* B = (const float*)(posData + (size_t)(baseVert + (fb < 0 ? i : fb)) * posStride);
            const float* C = (const float*)(posData + (size_t)(baseVert + (fc < 0 ? i : fc)) * posStride);
            float T[3], N[3];
            // the tangent is this RENDER vertex's own -- copies at a seam legitimately differ
            if (DecodeTangentBasis(tangData, tangStride, tangHi, baseVert + i, T, N)) {
                const float e1[3] = { B[0]-A[0], B[1]-A[1], B[2]-A[2] };
                const float e2[3] = { C[0]-A[0], C[1]-A[1], C[2]-A[2] };
                const float a11 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2];
                const float a22 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2];
                const float a12 = e1[0]*e2[0] + e1[1]*e2[1] + e1[2]*e2[2];
                const float b1  = e1[0]*T[0]  + e1[1]*T[1]  + e1[2]*T[2];
                const float b2  = e2[0]*T[0]  + e2[1]*T[1]  + e2[2]*T[2];
                const float det = a11*a22 - a12*a12;
                if (fabsf(det) > 1e-9f) {
                    const float y = (a22*b1 - a12*b2) / det;
                    const float z = (a11*b2 - a12*b1) / det;
                    tan[0] = 1.0f - y - z; tan[1] = y; tan[2] = z; tan[3] = 0.0f;
                }
            }
        }
        uint16_t* si = (uint16_t*)(e + 0x30);
        si[0] = (uint16_t)own[u*3+0]; si[1] = (uint16_t)own[u*3+1];
        si[2] = (uint16_t)own[u*3+2]; si[3] = 0;
        *(float*)(e + 0x38) = 1.0f;                       // Weight (single influence)
    }

    bool ok = MakeArray(sec + SEC_CLOTHMAPPING, map, nVerts, M2M_STRIDE);

    // ---- the GPU side. InitRHI reads the upload size back out of the vertex-data object, so the
    // engine has to allocate it; the interface's slots were confirmed against InitRHI, which calls
    // GetResourceArray at vtable+0x28.
    if (ok) {
        uint8_t* cvb = lod + LODRD_CLOTHVB;
        g_allocClothData(cvb);
        void* vd = twkP(cvb, CVB_VERTEXDATA);
        if (!vd) { TwkLog("[render] cloth buffer has no vertex data after allocate"); return false; }
        void** vt = *(void***)vd;
        ((void (*)(void*, uint32_t))vt[1])(vd, (uint32_t)nVerts);      // ResizeBuffer
        const uint32_t stride = ((uint32_t (*)(void*))vt[2])(vd);      // GetStride
        uint8_t* dp = ((uint8_t* (*)(void*))vt[3])(vd);                // GetDataPointer
        if (!dp || stride != M2M_STRIDE) {
            TwkLog("[render] unexpected cloth vertex stride %u (want %d) -- not binding", stride, M2M_STRIDE);
            return false;
        }
        memcpy(dp, map, (size_t)nVerts * stride);
        *(void**)(cvb + CVB_DATA)   = dp;
        *(int*)  (cvb + CVB_STRIDE) = (int)stride;
        *(int*)  (cvb + CVB_NUMVERTS) = nVerts;
        // One entry per section, packing the section base with its offset into the buffer. This
        // garment is a single section based at vertex 0 with offset 0, so the entry is 0 whichever
        // half holds which -- the packing never has to be guessed at here.
        const uint64_t idxMap = 0;
        ok = MakeArray(cvb + CVB_IDXMAP, &idxMap, 1, 8);
        // ONCE per mesh -- see the note on g_vbInitMesh. A rebuild refills the data above and must not
        // register the resource a second time.
        if (ok) {
            if (!VBAlreadyInit(mesh)) {
                g_beginInitRes(cvb);
                VBMarkInit(mesh);
            } else {
                if (g_beginUpdateRes) { __try { g_beginUpdateRes(cvb); }
                                        __except (EXCEPTION_EXECUTE_HANDLER) {} }
                TwkLog("[render] cloth buffer already initialised for this garment -- refilled, "
                       "not re-registered");
            }
        }
        TwkLog("[render] bound %d render verts to %d welded particles (%d orphan), stride %u",
               nVerts, nPart, orphans, stride);
    }

    free(map); free(own); free(first);
    return ok;
}

// Set a single garment up: build its cloth asset, bind the drawn mesh to it, and get it ticking.
// Returns true once the garment is simulating.
// =====================================================================================================
// PRISTINE SNAPSHOT  -- what the garment looked like the moment we built it
//
// Now that a copy is ROOTED it survives every map, which means anything we leave written on it survives
// too. Two things do: the DIRECT path writes simulated positions into the mesh's vertex buffer, and the
// cloth build rewrites section fields. Both are supposed to be undone on release -- but a fault during
// teardown (one happens on every map change) drops our saved originals without restoring them, so the
// last deformation is baked in. The next map then captures THAT as the rest pose and deforms from there:
// the drift the user sees getting worse with every map change, and the section state that comes back
// wrong as a changed texture a few seconds after spawn.
//
// So we keep our own copy of both, taken the FIRST time we see the mesh -- straight after the merge,
// when it is guaranteed clean -- and put it back before every arm. Immune to faults and to ordering.
// ~55 KB per garment.
struct Pristine { void* mesh; uint8_t* pos; int nVerts, stride; uint8_t* secs; int secBytes; };
enum { kPristine = 8 };
static Pristine g_pristine[kPristine] = {};

static Pristine* PristineFor(void* mesh) {
    for (int i = 0; i < kPristine; i++) if (g_pristine[i].mesh == mesh) return &g_pristine[i];
    return nullptr;
}

// Capture on first sight, restore on every sight after. Returns quietly if the mesh is not readable --
// this must never be the thing that stops a garment working.
static void PristineSyncMesh(void* mesh) {
    __try {
        uint8_t*  rd   = (uint8_t*)twkP(mesh, SM_RENDERDATA);
        uint8_t** lods = rd ? (uint8_t**)twkP(rd, RD_LODARRAY) : nullptr;
        uint8_t*  lod  = (lods && twkI(rd, RD_LODARRAY + 8) > 0) ? lods[0] : nullptr;
        if (!lod) return;
        uint8_t*  pos    = (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA);
        const int stride = twkI(lod, LODRD_POSVB + POSVB_STRIDE);
        const int nVerts = twkI(lod, LODRD_POSVB + POSVB_NUMVERTS);
        uint8_t*  secs   = (uint8_t*)twkP(lod, LODRD_SECTIONS);
        const int nSec   = twkI(lod, LODRD_SECTIONS + 8);
        if (!pos || stride < 12 || nVerts <= 0 || nVerts > 400000 || !secs || nSec <= 0 || nSec > 64)
            return;
        const int secBytes = nSec * SEC_STRIDE;

        Pristine* p = PristineFor(mesh);
        if (p) {                                   // seen before: put the garment back as it was built
            if (p->pos && p->nVerts == nVerts && p->stride == stride)
                memcpy(pos, p->pos, (size_t)nVerts * stride);
            if (p->secs && p->secBytes == secBytes)
                memcpy(secs, p->secs, (size_t)secBytes);
            return;
        }
        for (int i = 0; i < kPristine; i++) {      // first sight: remember it
            if (g_pristine[i].mesh) continue;
            uint8_t* pc = (uint8_t*)malloc((size_t)nVerts * stride);
            uint8_t* sc = (uint8_t*)malloc((size_t)secBytes);
            if (!pc || !sc) { free(pc); free(sc); return; }
            memcpy(pc, pos, (size_t)nVerts * stride);
            memcpy(sc, secs, (size_t)secBytes);
            g_pristine[i].mesh = mesh; g_pristine[i].pos = pc; g_pristine[i].nVerts = nVerts;
            g_pristine[i].stride = stride; g_pristine[i].secs = sc; g_pristine[i].secBytes = secBytes;
            TwkLog("[pristine] remembered '%s' as built: %d verts x %d, %d section byte(s)",
                   "garment", nVerts, stride, secBytes);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool SetupGarment(void* comp, void* mesh, int slot) {
    // Both of these used to return in silence, which is why a garment could simply have no cloth with
    // nothing in the log to say why. The caller retries, so these are throttled to one per garment.
    if (!g_ready || !g_recreate) {
        static double whineAt = 0.0;
        const double now = (double)GetTickCount64() / 1000.0;
        if (now >= whineAt) { whineAt = now + 2.0;
            TwkLog("[quad] slot %d: not ready to build yet (ready=%d recreate=%p)",
                   slot, g_ready ? 1 : 0, (void*)g_recreate); }
        return false;
    }
    // Put the garment back exactly as it was built before measuring or binding anything -- see Pristine.
    PristineSyncMesh(mesh);
    char nm[64];
    if (!CatchSound_ObjName(mesh, nm, sizeof(nm))) {
        static double whineAt2 = 0.0;
        const double now = (double)GetTickCount64() / 1000.0;
        if (now >= whineAt2) { whineAt2 = now + 2.0;
            TwkLog("[quad] slot %d: the garment mesh %p has no resolvable name yet -- will retry",
                   slot, mesh); }
        return false;
    }
    void* prior = AlreadyBuilt(mesh);
    if (prior && *(int*)((uint8_t*)mesh + SM_MESHCLOTH + 8) > 0) {
        // Everything we built the first time is still on the mesh -- the asset, the section marks and
        // the render binding. Only the component's own state is new.
        g_sim[slot].asset = prior;
        g_asset = prior;
        *(uint8_t*)((uint8_t*)comp + SMC_DISABLECLOTH) = 0;
        MarkMeshHasCloth(mesh);
        if (g_recreate) g_recreate(comp);
        if (g_recreateRender) { __try { g_recreateRender(comp); }
                                __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (g_updClothTick)   { __try { g_updClothTick(comp); }
                                __except (EXCEPTION_EXECUTE_HANDLER) {} }
        TwkLog("[quad] '%s' already carries our cloth asset -- re-adopted (slot %d)", nm, slot);
        return true;
    }
    TwkLog("[quad] building a cloth asset on '%s' (slot %d)", nm, slot);
    g_buildLower = (strstr(nm, "_LB_") != nullptr) || (strstr(nm, "LB_") != nullptr);

    // Which material slot the garment itself occupies; everything below keys off it.
    const int wantMatIdx = ClothMerge_GarmentMaterialIndex(mesh);

    void* asset = BuildQuadAsset(mesh);
    if (!asset || !AttachToMesh(mesh, asset) || MarkSectionsInUse(mesh, wantMatIdx) <= 0) {
        TwkLog("[quad] build failed for '%s' -- nothing was attached", nm);
        FreeWeld();
        return false;
    }
    g_sim[slot].asset = asset;
    g_asset = asset;
    *(uint8_t*)((uint8_t*)comp + SMC_DISABLECLOTH) = 0;

    // Bind the drawn mesh to the simulation. Opt-in, and only once the sim mesh is the real garment
    // -- mapping render vertices onto a 4-vert test quad would be meaningless.
    // Drive only the garments that need it -- the ones whose own material cannot draw cloth. Tops
    // already have cloth-capable materials and stay on the engine's own, better path.
    const bool driveThis = g_direct && ClothMerge_GarmentWantsDirect(mesh);

    bool bound = false;
    if (driveThis) {
        // Nothing is bound to the cloth renderer at all -- the garment keeps its own material and
        // draws normally, and we feed it the simulation ourselves every frame.
        __try { DirectArm(comp, mesh, slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[direct] arming faulted"); }
    } else if (g_render && g_realMesh && g_allocClothData && g_beginInitRes) {
        uint8_t*  rd2   = (uint8_t*)twkP(mesh, SM_RENDERDATA);
        uint8_t** lods2 = rd2 ? (uint8_t**)twkP(rd2, RD_LODARRAY) : nullptr;
        uint8_t*  lod2  = (lods2 && twkI(rd2, RD_LODARRAY + 8) > 0) ? lods2[0] : nullptr;
        uint8_t*  secs2 = lod2 ? (uint8_t*)twkP(lod2, LODRD_SECTIONS) : nullptr;
        if (secs2) {
            __try { bound = BuildClothMapping(mesh, lod2, secs2); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                bound = false;
                TwkLog("[render] faulted while binding '%s' -- restored on teardown", nm);
            }
        }
    }
    FreeWeld();                                   // scratch: the binding was its only consumer

    if (g_recreate) g_recreate(comp);             // this is what builds the simulation actors
    // Ask the engine to (re)decide whether the cloth tick should be registered; without this the
    // actors exist and simply never get stepped. Must run BEFORE any unmarking below.
    if (g_updClothTick) {
        __try { g_updClothTick(comp); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[quad] cloth-tick registration faulted"); }
    }
    if (driveThis) {
        // The sections had to be marked for the engine to build the simulation at all -- that is how
        // it discovers an asset is in use. But a marked section also makes the RENDERER take the
        // cloth path, which demands a cloth-capable material and would draw the default tile. The
        // actors exist now, so hand the sections back: the garment draws as an ordinary skinned mesh
        // wearing its own material, and we put the simulated positions into its vertices ourselves.
        RestoreSections(mesh);
        if (g_recreateRender) {
            __try { g_recreateRender(comp); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        TwkLog("[quad] '%s' is DRIVEN -- own material kept, simulation written into its vertices", nm);
    } else if (bound && g_recreateRender) {
        // The proxy was built before any of this existed, so it is still drawing the garment as a
        // plain skinned mesh; rebuilding it is what swaps in the cloth path.
        __try { g_recreateRender(comp); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[render] render-state rebuild faulted"); }
    }
    TwkLog("[quad] '%s': bound=%d clothTick=%d", nm, bound ? 1 : 0,
           twkP(comp, SMC_CLOTHTICK_ID) != nullptr ? 1 : 0);
    if (g_sway) {
        __try { SwayArm(comp, slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[sway] could not arm garment %d", slot); }
    }
    return true;
}


// Does the solver's idea of "outward" agree with the garment's? Its normals are recomputed every
// frame from the triangles we hand it, and if its winding convention runs opposite to ours they come
// out inverted -- light then arrives from behind, which on a fuzzy cloth material reads as suede
// rather than fabric. The animated normals sitting next to them come from the garment itself, so the
// two can simply be compared. Measured once per garment, and it fixes itself.
static bool g_normalsChecked[kSimSlots] = {};
static void CheckSolverNormals(void* comp, int slot) {
    if (slot < 0 || slot >= kSimSlots || g_normalsChecked[slot]) return;
    void* sim = twkP(comp, SMC_CLOTHSIM);
    uint8_t* actors = sim ? (uint8_t*)twkP(sim, NVSIM_ACTORS) : nullptr;
    if (!actors || twkI(sim, NVSIM_ACTORS + 8) <= 0) return;

    const float* cur  = (const float*)twkP(actors, ACTOR_CURNORM);
    const int    nCur = twkI(actors, ACTOR_CURNORM + 8);
    const float* skin = (const float*)twkP(actors, ACTOR_SKINNORM);
    const int    nSkn = twkI(actors, ACTOR_SKINNORM + 8);
    if (!cur || !skin || nCur <= 0 || nSkn < nCur) return;   // not populated yet -- try next frame

    int agree = 0, disagree = 0;
    const int step = (nCur > 512) ? nCur / 512 : 1;
    for (int i = 0; i < nCur; i += step) {
        const float d = cur[i*3+0]*skin[i*3+0] + cur[i*3+1]*skin[i*3+1] + cur[i*3+2]*skin[i*3+2];
        if (d > 0.05f) agree++; else if (d < -0.05f) disagree++;
    }
    if (agree + disagree < 16) return;                       // too little signal; wait
    g_normalsChecked[slot] = true;
    TwkLog("[real] solver normals vs the garment: %d agree, %d disagree", agree, disagree);
    if (disagree > agree && !ClothSim_FlipNormals) {
        ClothSim_FlipNormals = 1;
        TwkLog("[real] the solver faces them the other way -- rebuilding every garment flipped");
        for (int i = 0; i < kSimSlots; i++) { g_sim[i].mesh = nullptr; g_sim[i].serial = -1; }
    }
}


// =============================================================================================
// DIRECT RENDER -- the escape from the cloth renderer.
//
// The simulation was never the problem. The problem is that drawing its result through the engine's
// cloth path demands a material built for cloth (trousers do not have one, and that is decided when
// the game is built), and rebuilds the surface directions itself, which is where the shading went
// wrong. None of that is necessary: the simulation already produces finished positions and normals,
// so we can simply put them into the garment's own vertices and let it draw like any other mesh.
// Then the material, the colours and the lighting are the game's, untouched.
//
// One wrinkle: the garment still follows the body's bones, so the graphics card would skin our
// finished positions a second time. We undo that first -- each vertex is written in the space that
// its own bones will transform back to exactly where the simulation put it.
// =============================================================================================
struct DirectState {
    void*    mesh;
    uint8_t* origPos;                      // the garment's shipped vertices, put back on teardown
    uint8_t* origTan;
    int      nVerts, posStride, tanStride;
    float*   invRefPose;                   // 12 floats per bone: inverse of the rest pose
    int      nBones;
    int*     weld;                         // OUR OWN copy: the shared one is scratch, freed after
    int      weldCount, weldUnique;        // setup, and this is needed on every single frame
    float*   refPoseCS;                    // the garment's own rest pose, component space
    float*   boneMats;                     // rest->posed, ONE per bone per frame (see the loop)
    uint8_t* boneOk;
    bool     armed;
};
static DirectState g_dir[kSimSlots] = {};

// --- small 3x4 matrix helpers (row-major, 12 floats: 3 rows of {x,y,z,w}) ---------------------
static void XformToMat(const float* q, const float* t, const float* sc, float* m) {
    const float x=q[0],y=q[1],z=q[2],w=q[3];
    const float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=(1-2*(yy+zz))*sc[0]; m[1]=(2*(xy-wz))*sc[1];   m[2]=(2*(xz+wy))*sc[2];   m[3]=t[0];
    m[4]=(2*(xy+wz))*sc[0];   m[5]=(1-2*(xx+zz))*sc[1]; m[6]=(2*(yz-wx))*sc[2];   m[7]=t[1];
    m[8]=(2*(xz-wy))*sc[0];   m[9]=(2*(yz+wx))*sc[1];   m[10]=(1-2*(xx+yy))*sc[2];m[11]=t[2];
}
static void MatMul(const float* a, const float* b, float* o) {   // o = a * b
    for (int r = 0; r < 3; r++) {
        o[r*4+0] = a[r*4+0]*b[0] + a[r*4+1]*b[4] + a[r*4+2]*b[8];
        o[r*4+1] = a[r*4+0]*b[1] + a[r*4+1]*b[5] + a[r*4+2]*b[9];
        o[r*4+2] = a[r*4+0]*b[2] + a[r*4+1]*b[6] + a[r*4+2]*b[10];
        o[r*4+3] = a[r*4+0]*b[3] + a[r*4+1]*b[7] + a[r*4+2]*b[11] + a[r*4+3];
    }
}
static bool MatInv(const float* m, float* o) {
    const float d = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[1]*(m[4]*m[10]-m[6]*m[8])
                  + m[2]*(m[4]*m[9]-m[5]*m[8]);
    if (fabsf(d) < 1e-12f) return false;
    const float id = 1.0f/d;
    o[0]=(m[5]*m[10]-m[6]*m[9])*id;  o[1]=(m[2]*m[9]-m[1]*m[10])*id; o[2]=(m[1]*m[6]-m[2]*m[5])*id;
    o[4]=(m[6]*m[8]-m[4]*m[10])*id;  o[5]=(m[0]*m[10]-m[2]*m[8])*id; o[6]=(m[2]*m[4]-m[0]*m[6])*id;
    o[8]=(m[4]*m[9]-m[5]*m[8])*id;   o[9]=(m[1]*m[8]-m[0]*m[9])*id;  o[10]=(m[0]*m[5]-m[1]*m[4])*id;
    o[3]  = -(o[0]*m[3] + o[1]*m[7] + o[2]*m[11]);
    o[7]  = -(o[4]*m[3] + o[5]*m[7] + o[6]*m[11]);
    o[11] = -(o[8]*m[3] + o[9]*m[7] + o[10]*m[11]);
    return true;
}
static void MatXfmPos(const float* m, const float* v, float* o) {
    o[0] = m[0]*v[0]+m[1]*v[1]+m[2]*v[2]+m[3];
    o[1] = m[4]*v[0]+m[5]*v[1]+m[6]*v[2]+m[7];
    o[2] = m[8]*v[0]+m[9]*v[1]+m[10]*v[2]+m[11];
}
static void MatXfmDir(const float* m, const float* v, float* o) {
    o[0] = m[0]*v[0]+m[1]*v[1]+m[2]*v[2];
    o[1] = m[4]*v[0]+m[5]*v[1]+m[6]*v[2];
    o[2] = m[8]*v[0]+m[9]*v[1]+m[10]*v[2];
}

// Rest pose, walked up the parent chain into component space and inverted -- computed once.
static bool BuildInvRefPose(void* mesh, DirectState& d) {
    uint8_t* rs = (uint8_t*)mesh + SM_REFSKELETON;
    uint8_t* info = (uint8_t*)twkP(rs, REFSK_BONEINFO);
    const int nInfo = twkI(rs, REFSK_BONEINFO + 8);
    uint8_t* pose = (uint8_t*)twkP((uint8_t*)mesh + SM_REFPOSE, 0);
    const int nPose = twkI((uint8_t*)mesh + SM_REFPOSE, 8);
    if (!info || !pose || nInfo <= 0 || nPose < nInfo || nInfo > 512) return false;

    float* cs  = (float*)malloc((size_t)nInfo * 12 * sizeof(float));
    d.invRefPose = (float*)malloc((size_t)nInfo * 12 * sizeof(float));
    if (!cs || !d.invRefPose) { free(cs); return false; }
    for (int b = 0; b < nInfo; b++) {
        const float* t = (const float*)(pose + (size_t)b * XFORM_STRIDE);
        float local[12];
        XformToMat(t /*rot*/, t + 4 /*trans*/, t + 8 /*scale*/, local);
        const int parent = twkI(info + (size_t)b * BONEINFO_STRIDE, 8);
        if (parent >= 0 && parent < b) MatMul(cs + parent*12, local, cs + b*12);
        else                            memcpy(cs + b*12, local, 12*sizeof(float));
        if (!MatInv(cs + b*12, d.invRefPose + b*12)) memset(d.invRefPose + b*12, 0, 12*sizeof(float));
    }
    // Keep the component-space rest pose: the bind-pose comparison needs it, and it can only be
    // compared THROUGH the bone map -- the two skeletons do not share an ordering, so a bone-index
    // comparison is meaningless (it reported a 21 cm "mismatch" on a garment that renders perfectly).
    free(d.refPoseCS);
    d.refPoseCS = (float*)malloc((size_t)nInfo * 12 * sizeof(float));
    if (d.refPoseCS) memcpy(d.refPoseCS, cs, (size_t)nInfo * 12 * sizeof(float));

    free(cs);
    d.nBones = nInfo;
    return true;
}

static void DirectRelease(int slot) {
    DirectState& d = g_dir[slot];
    if (d.mesh && d.origPos) {
        uint8_t* rd = (uint8_t*)twkP(d.mesh, SM_RENDERDATA);
        uint8_t** lods = rd ? (uint8_t**)twkP(rd, RD_LODARRAY) : nullptr;
        uint8_t* lod = (lods && twkI(rd, RD_LODARRAY + 8) > 0) ? lods[0] : nullptr;
        uint8_t* pos = lod ? (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA) : nullptr;
        if (pos) memcpy(pos, d.origPos, (size_t)d.nVerts * d.posStride);
    }
    free(d.origPos); free(d.origTan); free(d.invRefPose); free(d.weld);
    free(d.boneMats); free(d.boneOk); free(d.refPoseCS);
    memset(&d, 0, sizeof(d));
}

// Put the simulation's result into the garment's own vertices, undoing the skinning that will be
// applied to them, and hand the buffer back to the renderer.
// Say once, per garment, exactly which step stopped us -- there are several ways to bail here and
// silence tells you nothing.
static const char* g_dirWhy[kSimSlots] = {};
static void DirWhy(int slot, const char* why) {
    if (slot < 0 || slot >= kSimSlots || g_dirWhy[slot]) return;
    g_dirWhy[slot] = why;
    TwkLog("[direct] slot %d not driving: %s", slot, why);
}

static void DirectDrive(void* comp, void* mesh, int slot) {
    DirectState& d = g_dir[slot];
    if (!d.armed) { DirWhy(slot, "not armed"); return; }
    if (!g_beginInitRes) { DirWhy(slot, "no buffer-update function"); return; }

    // ---- the simulated result. Validate the layout before trusting it: one entry, our index, and
    // a point count that matches the particles we built. Anything else and we leave well alone.
    uint8_t* mapData = (uint8_t*)twkP(comp, SMC_SIMDATA);
    const int mapNum = twkI(comp, SMC_SIMDATA + 8);
    if (!mapData || mapNum != 1) {
        static int spam[kSimSlots] = {};
        if (!spam[slot]) { spam[slot] = 1;
            TwkLog("[direct] slot %d: simulation results map has %d entries (data=%p)",
                   slot, mapNum, mapData); }
        DirWhy(slot, "no simulation result published yet");
        return;
    }
    const float* simPos = (const float*)twkP(mapData + SIMDATA_POS, 0);
    const int    nSim   = twkI(mapData + SIMDATA_POS, 8);
    const float* simNrm = (const float*)twkP(mapData + SIMDATA_NRM, 0);
    if (!simPos || nSim <= 0) { DirWhy(slot, "result has no points"); return; }
    if (!d.weld || nSim < d.weldUnique) {
        static bool warned = false;
        if (!warned) { warned = true;
            TwkLog("[direct] simulation result does not line up (%d points for %d particles)",
                   nSim, d.weldUnique); }
        return;
    }

    uint8_t* rd = (uint8_t*)twkP(mesh, SM_RENDERDATA);
    uint8_t** lods = rd ? (uint8_t**)twkP(rd, RD_LODARRAY) : nullptr;
    uint8_t* lod = (lods && twkI(rd, RD_LODARRAY + 8) > 0) ? lods[0] : nullptr;
    if (!lod) return;
    uint8_t* posData = (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA);
    if (!posData) { DirWhy(slot, "no vertex data"); return; }
    uint8_t* secs = GarmentSection(mesh, lod);
    if (!secs) { DirWhy(slot, "no sections"); return; }
    const int baseVert = twkI(secs, SEC_BASEVERT);

    // ---- the bones as the renderer will use them. The garment follows the body, and a component
    // posed that way keeps no bones of its own -- they belong to the body component, so read there.
    // Which body bone each garment bone corresponds to. Without this we index the body's bones
    // with the garment's numbering and every vertex lands somewhere arbitrary. Read BEFORE the
    // transform buffer, because it decides how many transforms are actually needed.
    const int* boneXlat = (const int*)twkP(comp, SMC_MASTERBONEMAP);
    const int  nXlat    = twkI(comp, SMC_MASTERBONEMAP + 8);

    // How many body transforms this garment really needs. With a bone map the garment's own bone
    // COUNT is beside the point -- what matters is the highest BODY bone any of its bones maps to.
    // A custom mesh can carry bones the body has never heard of (helper, twist, export leftovers);
    // those map to -1, the per-bone loop below skips them, and their vertices ride the influences
    // that did map. Demanding one body transform per garment bone refused modded trousers with a
    // 95-bone skeleton on the 70-bone body, mappable in every bone that mattered.
    int needCst = d.nBones;
    if (boneXlat && nXlat > 0) {
        int hi = -1;
        for (int b = 0; b < d.nBones && b < nXlat; b++) if (boneXlat[b] > hi) hi = boneXlat[b];
        needCst = hi + 1;                                // hi < 0 (nothing mapped) -> needs nothing
    }

    void* poseComp = ClothMerge_MasterComponent();
    if (!poseComp) poseComp = comp;
    int readIdx = twkI(poseComp, SMC_CST_READ) & 1;
    uint8_t* cstArr = (uint8_t*)poseComp + SMC_CSTA + (size_t)readIdx * 16;
    const float* cst = (const float*)twkP(cstArr, 0);
    int nCst = twkI(cstArr, 8);
    if (!cst || nCst < needCst) {                        // try the other of the two buffers
        readIdx ^= 1;
        cstArr = (uint8_t*)poseComp + SMC_CSTA + (size_t)readIdx * 16;
        cst = (const float*)twkP(cstArr, 0);
        nCst = twkI(cstArr, 8);
    }
    if (!cst || nCst < needCst) {
        static int spam[kSimSlots] = {};
        if (!spam[slot]) { spam[slot] = 1;
            TwkLog("[direct] slot %d: body has %d bone transforms, garment needs %d (%d bones, "
                   "highest mapped body bone %d)", slot, nCst, needCst, d.nBones, needCst - 1); }
        DirWhy(slot, "bone transforms unavailable");
        return;
    }
    static int xlatLogged[kSimSlots] = {};
    if (!xlatLogged[slot]) { xlatLogged[slot] = 1;
        // Count the bones that do NOT resolve onto the body. A separated garment is master-posed, so
        // every bone of its own skeleton has to exist on the body's; one that does not maps to -1 and
        // every vertex weighted to it collapses. Merged, the garment never shows this -- the merge
        // rebakes bone indices -- so an unmatched skeleton only becomes visible once it is separated,
        // as a mesh that renders as a few thin strips. Worth naming rather than inferring.
        int unmapped = 0;
        if (boneXlat)
            for (int b = 0; b < nXlat && b < d.nBones; b++) if (boneXlat[b] < 0) unmapped++;
        TwkLog("[direct] slot %d: bone translation table has %d entries for %d garment bones%s",
               slot, nXlat, d.nBones,
               unmapped ? " -- SOME DO NOT MATCH THE BODY" : "");

        // ---- does this garment share the body's BIND POSE? --------------------------------------
        // Master-posing hands the garment the body's animated bones, but it skins them against its
        // OWN rest pose. Where the two rest poses disagree, every vertex is deformed by exactly that
        // difference -- which reads as the mesh collapsing toward its bones. Merged, it never shows,
        // because the merge rebakes the garment against the merged skeleton.
        //
        // Compared THROUGH THE BONE MAP and in COMPONENT SPACE. Comparing by bone index instead is
        // meaningless -- the skeletons do not share an ordering, which is the whole reason the map
        // exists -- and doing so reported a large mismatch on a garment that renders perfectly.
        void* bodyMesh = poseComp ? twkP(poseComp, SMC_SKELMESH) : nullptr;
        uint8_t* bInfo = bodyMesh ? (uint8_t*)twkP((uint8_t*)bodyMesh + SM_REFSKELETON, REFSK_BONEINFO) : nullptr;
        const int nBInfo = bodyMesh ? twkI((uint8_t*)bodyMesh + SM_REFSKELETON, REFSK_BONEINFO + 8) : 0;
        uint8_t* bPose = bodyMesh ? (uint8_t*)twkP((uint8_t*)bodyMesh + SM_REFPOSE, 0) : nullptr;
        if (d.refPoseCS && bInfo && bPose && nBInfo > 0 && nBInfo <= 512) {
            float* bcs = (float*)malloc((size_t)nBInfo * 12 * sizeof(float));
            if (bcs) {
                for (int b = 0; b < nBInfo; b++) {          // body rest pose -> component space
                    const float* t = (const float*)(bPose + (size_t)b * XFORM_STRIDE);
                    float local[12];
                    XformToMat(t, t + 4, t + 8, local);
                    const int parent = twkI(bInfo + (size_t)b * BONEINFO_STRIDE, 8);
                    if (parent >= 0 && parent < b) MatMul(bcs + parent*12, local, bcs + b*12);
                    else                            memcpy(bcs + b*12, local, 12*sizeof(float));
                }
                int differing = 0, compared = 0; float worst = 0.0f;
                for (int b = 0; b < d.nBones; b++) {
                    int mb = b;
                    if (boneXlat && b < nXlat) mb = boneXlat[b];
                    if (mb < 0 || mb >= nBInfo) continue;
                    const float* g = d.refPoseCS + b*12;
                    const float* y = bcs + mb*12;
                    const float dx = g[3]-y[3], dy = g[7]-y[7], dz = g[11]-y[11];
                    const float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    compared++;
                    if (dist > 1.0f) differing++;
                    if (dist > worst) worst = dist;
                }
                if (differing > 0)
                    TwkLog("[direct] slot %d: BIND POSE MISMATCH -- %d of %d mapped bones sit up to "
                           "%.1f cm from where the body has them. Separated, the garment is skinned "
                           "against a rest pose the body does not share, which collapses it.",
                           slot, differing, compared, worst);
                else
                    TwkLog("[direct] slot %d: bind pose agrees with the body across %d mapped bones "
                           "(worst %.2f cm)", slot, compared, worst);
                free(bcs);
            }
        }
    }

    uint8_t* skinData = (uint8_t*)twkP(lod, LODRD_SKINVB + SKINVB_DATA);
    const int maxInfl = twkI(lod, LODRD_SKINVB + SKINVB_MAXINFL);
    const bool bone16 = *(uint8_t*)(lod + LODRD_SKINVB + SKINVB_16BITBONE) != 0;
    const int  skinStride = maxInfl * (bone16 ? 2 : 1) + maxInfl;
    uint16_t*  boneMap = (uint16_t*)twkP(secs, SEC_BONEMAP);
    const int  boneMapNum = twkI(secs, SEC_BONEMAP + 8);
    if (!skinData || !boneMap || maxInfl <= 0) { DirWhy(slot, "skin weights unavailable"); return; }

    const float* simXf = (ClothSim_SimXform == 1) ? (const float*)(mapData + SIMDATA_XFORM)
                       : (ClothSim_SimXform == 2) ? (const float*)(mapData + SIMDATA_COMPREL)
                       : nullptr;
    float simMat[12];
    bool  useXf = false;
    if (simXf) { XformToMat(simXf, simXf + 4, simXf + 8, simMat); useXf = true; }
    static int xfLogged[kSimSlots] = {};
    if (!xfLogged[slot]) { xfLogged[slot] = 1;
        const float* a = (const float*)(mapData + SIMDATA_XFORM);
        const float* b = (const float*)(mapData + SIMDATA_COMPREL);
        TwkLog("[direct] slot %d transforms -- Transform q=(%.2f %.2f %.2f %.2f) t=(%.1f %.1f %.1f) "
               "s=(%.2f %.2f %.2f)", slot, a[0],a[1],a[2],a[3], a[4],a[5],a[6], a[8],a[9],a[10]);
        TwkLog("[direct] slot %d transforms -- ComponentRelative q=(%.2f %.2f %.2f %.2f) "
               "t=(%.1f %.1f %.1f) s=(%.2f %.2f %.2f) (using %d)",
               slot, b[0],b[1],b[2],b[3], b[4],b[5],b[6], b[8],b[9],b[10], ClothSim_SimXform); }

    // Each bone's rest->posed matrix is the same for every vertex it touches, so build them ONCE.
    // Doing it inside the vertex loop meant thousands of quaternion conversions and matrix multiplies
    // a frame for a handful of distinct answers.
    for (int b = 0; b < d.nBones; b++) {
        d.boneOk[b] = 0;
        int mb = b;
        if (boneXlat && b < nXlat) { mb = boneXlat[b]; if (mb < 0) continue; }
        if (mb >= nCst) continue;
        const float* x = cst + (size_t)mb * 12;
        float cur[12];
        XformToMat(x, x + 4, x + 8, cur);
        MatMul(cur, d.invRefPose + b*12, d.boneMats + b*12);
        d.boneOk[b] = 1;
    }

    // The distance report costs a square root per vertex, so only measure on the frames that log.
    static double lastDev[kSimSlots] = {};
    const double nowD    = (double)GetTickCount64() / 1000.0;
    const bool   wantDev = g_debugLog && (nowD - lastDev[slot] >= 1.0);

    int driven = 0; float devSum = 0.0f, devMax = 0.0f;
    for (int i = 0; i < d.weldCount; i++) {
        const int u = d.weld[i];
        if (u < 0 || u >= nSim) continue;

        // the blended bone transform this vertex will be put through
        float blend[12] = {0};
        const uint8_t* sw  = skinData + (size_t)(baseVert + i) * skinStride;
        const uint8_t* wts = sw + maxInfl * (bone16 ? 2 : 1);
        float wsum = 0.0f;
        for (int k = 0; k < maxInfl; k++) {
            const int w = wts[k];
            if (!w) continue;
            const int sb = bone16 ? (int)((const uint16_t*)sw)[k] : (int)sw[k];
            if (sb < 0 || sb >= boneMapNum) continue;
            const int b = boneMap[sb];                  // bone in the GARMENT's numbering
            if (b < 0 || b >= d.nBones || !d.boneOk[b]) continue;
            const float* ref2loc = d.boneMats + b*12;   // built once, above
            const float fw = (float)w / 255.0f;
            for (int e = 0; e < 12; e++) blend[e] += ref2loc[e] * fw;
            wsum += fw;
        }
        if (wsum < 0.001f) continue;
        if (wsum > 0.001f && fabsf(wsum - 1.0f) > 0.01f)
            for (int e = 0; e < 12; e++) blend[e] /= wsum;

        // write the position that skins back to where the simulation put it
        float inv[12];
        if (!MatInv(blend, inv)) continue;
        float pt[3] = { simPos[u*3+0], simPos[u*3+1], simPos[u*3+2] };
        if (useXf) { float tmp[3]; MatXfmPos(simMat, pt, tmp);
                     pt[0]=tmp[0]; pt[1]=tmp[1]; pt[2]=tmp[2]; }
        float out[3];
        MatXfmPos(inv, pt, out);
        // Never hand the card a broken vertex: one bad transform used to spray NaN through the mesh.
        if (!(out[0]==out[0]) || !(out[1]==out[1]) || !(out[2]==out[2])) continue;

        float* dst = (float*)(posData + (size_t)(baseVert + i) * d.posStride);
        // How far this is from where the garment would sit unsimulated. If these stay near zero the
        // cloth is simply not moving; if they are healthy and nothing moves on screen, the new
        // vertices are not reaching the card. Two different problems, so measure rather than guess.
        if (wantDev) {
            const float* was = (const float*)(d.origPos + (size_t)(baseVert + i) * d.posStride);
            const float dx = out[0]-was[0], dy = out[1]-was[1], dz = out[2]-was[2];
            const float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            devSum += dist; if (dist > devMax) devMax = dist;
        }
        dst[0] = out[0]; dst[1] = out[1]; dst[2] = out[2];
        driven++;
    }
    static int reported[kSimSlots] = {};
    if (!reported[slot]) { reported[slot] = 1;
        TwkLog("[direct] driving %d of %d vertices from the simulation", driven, d.weldCount); }
    if (driven > 0 && wantDev) {
        lastDev[slot] = nowD;
        TwkLog("[direct] slot %d: moved on average %.2f cm, at most %.2f cm from the plain pose",
               slot, devSum / driven, devMax);
    }

    // Hand the rebuilt vertices to the renderer. NOTE: this makes a NEW buffer, and the renderer
    // may still be drawing from the one it already had -- ClothDirectRefresh forces it to pick the
    // new one up. That is expensive, so it is a test to prove where the problem is, not a fix.
    // Push the rewritten vertices to the card. UPDATE, not init: an initialised resource ignores
    // init outright. Faults here are reported rather than swallowed -- a silent handler is how this
    // looked like "the upload runs and does nothing" for a whole round.
    static int upFail[kSimSlots] = {};
    if (g_beginUpdateRes) {
        __try { g_beginUpdateRes((uint8_t*)lod + LODRD_POSVB); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!upFail[slot]) { upFail[slot] = 1; TwkLog("[direct] slot %d: the vertex upload faulted", slot); }
        }
    } else {
        static bool once = false;
        if (!once) { once = true; TwkLog("[direct] no buffer-update function -- vertices cannot reach the card"); }
    }
    if (g_directRefresh && g_recreateRender) {
        __try { g_recreateRender(comp); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            if (upFail[slot] < 2) { upFail[slot] = 2; TwkLog("[direct] slot %d: render-state refresh faulted", slot); }
        }
    }
}

static bool DirectArm(void* comp, void* mesh, int slot) {
    DirectState& d = g_dir[slot];
    DirectRelease(slot);
    uint8_t* rd = (uint8_t*)twkP(mesh, SM_RENDERDATA);
    uint8_t** lods = rd ? (uint8_t**)twkP(rd, RD_LODARRAY) : nullptr;
    uint8_t* lod = (lods && twkI(rd, RD_LODARRAY + 8) > 0) ? lods[0] : nullptr;
    if (!lod) return false;
    uint8_t* posData = (uint8_t*)twkP(lod, LODRD_POSVB + POSVB_DATA);
    d.posStride = twkI(lod, LODRD_POSVB + POSVB_STRIDE);
    d.nVerts    = twkI(lod, LODRD_POSVB + POSVB_NUMVERTS);
    if (!posData || d.posStride < 12 || d.nVerts <= 0) return false;

    d.origPos = (uint8_t*)malloc((size_t)d.nVerts * d.posStride);
    if (!d.origPos) return false;
    memcpy(d.origPos, posData, (size_t)d.nVerts * d.posStride);   // the garment as shipped
    d.mesh = mesh;
    if (!g_weld || g_weldCount <= 0) { DirectRelease(slot); return false; }
    d.weld = (int*)malloc((size_t)g_weldCount * sizeof(int));
    if (!d.weld) { DirectRelease(slot); return false; }
    memcpy(d.weld, g_weld, (size_t)g_weldCount * sizeof(int));
    d.weldCount = g_weldCount; d.weldUnique = g_weldUnique;
    if (!BuildInvRefPose(mesh, d)) { DirectRelease(slot); return false; }
    d.boneMats = (float*)malloc((size_t)d.nBones * 12 * sizeof(float));
    d.boneOk   = (uint8_t*)malloc((size_t)d.nBones);
    if (!d.boneMats || !d.boneOk) { DirectRelease(slot); return false; }
    d.armed = true;
    // Every buffer parameter this module READS, printed per garment. Three theories about why one
    // custom garment renders collapsed were each disproved by their own control, so this stops
    // reasoning about causes: dump the numbers the code depends on and compare a garment that works
    // against one that does not. Whatever differs is the answer.
    {
        uint8_t* secs0 = (uint8_t*)twkP(lod, LODRD_SECTIONS);
        const int nSec = twkI(lod, LODRD_SECTIONS + 8);
        uint8_t* g = GarmentSection(mesh, lod);
        TwkLog("[direct] slot %d BUFFERS: pos stride=%d verts=%d | sections=%d (garment: verts=%d "
               "base=%d tris=%d baseIdx=%d) | skin maxInfl=%d bone16=%d varBones=%d | LODs=%d | "
               "weld %d verts -> %d particles",
               slot, d.posStride, d.nVerts, nSec,
               g ? twkI(g, SEC_NUMVERTS) : -1, g ? twkI(g, SEC_BASEVERT) : -1,
               g ? twkI(g, SEC_NUMTRIS) : -1, g ? twkI(g, SEC_BASEIDX) : -1,
               twkI(lod, LODRD_SKINVB + SKINVB_MAXINFL),
               (int)*(uint8_t*)(lod + LODRD_SKINVB + SKINVB_16BITBONE),
               (int)*(uint8_t*)(lod + LODRD_SKINVB + SKINVB_VARBONES),
               twkI((uint8_t*)twkP(mesh, SM_RENDERDATA), RD_LODARRAY + 8),
               d.weldCount, d.weldUnique);
        (void)secs0;
    }
    TwkLog("[direct] armed on %d vertices, %d bones -- the garment draws as an ordinary mesh",
           d.nVerts, d.nBones);
    return true;
}

// ---- menu surface -------------------------------------------------------------------------
bool  ClothSim_Enabled()      { return g_on != 0; }
float ClothSim_TravelCm()     { return ClothSim_MaxTravel; }
float ClothSim_HemPushMm()    { return ClothSim_HemPush * 10.0f; }
float ClothSim_CuffGripPct()  { return (1.0f - ClothSim_HemGripLower) * 100.0f; }
void  ClothSim_SetTravelCm(float v)    { ClothSim_MaxTravel = v; ClothSim_Rebuild(); TwkMarkDirty(); }
void  ClothSim_SetHemPushMm(float v)   { ClothSim_HemPush = v / 10.0f; ClothSim_Rebuild(); TwkMarkDirty(); }
void  ClothSim_SetCuffGripPct(float v) { ClothSim_HemGripLower = 1.0f - v / 100.0f; ClothSim_Rebuild(); TwkMarkDirty(); }
// Turning cloth off hands every garment back exactly as it shipped; turning it on rebuilds on the
// next frame. Both take effect immediately -- no reload, nothing left half-applied.
void ClothSim_SetEnabled(bool on) {
    if ((g_on != 0) == on) return;
    g_on = on ? 1 : 0;
    if (!on) ClothSim_ReleaseAll();
    else     ClothSim_Rebuild();
    TwkMarkDirty();
    TwkLog("[quad] cloth physics %s", on ? "ON" : "off");
}

int ClothSim_DirectRender() { return g_direct; }
int ClothSim_DirectEnabled() { return g_direct; }
// Only the engine's cloth renderer demands a cloth-capable material. Bone sway and the direct path
// both draw ordinarily, so a garment must NOT be held back for lacking that flag.
int ClothSim_NeedsClothMaterial() { return (g_render && !g_direct) ? 1 : 0; }


// =============================================================================================
// BONE-DRIVEN CLOTH.
//
// The separated garment is posed from the body, bone for bone, which is why it moves exactly like a
// second skin. Give it its OWN bones, following the body's but arriving slightly late, and it drapes
// and swings instead. This is not a cloth solve -- it is lag and settle -- but it goes through the
// completely ordinary drawing path, so the garment keeps its real material, its real colours and its
// real lighting, and there is no way for it to come out grey, black or checkered.
//
// It suits these garments better than it sounds: the trousers are shaped by 8 bones and the hoodie by
// 20, so bone-level motion is most of what they can physically express anyway.
// =============================================================================================
struct SwayState {
    void*  comp;
    float* pose;        // our lagged bones: 8 floats each (quat + position)
    int    nBones;
    bool   armed;
};
static SwayState g_sway_st[kSimSlots] = {};

static void SwayRelease(int slot) {
    // Put the garment back on the body's skeleton. Leaving it detached with nothing driving it is
    // what left clothing standing in its rest pose.
    if (g_sway_st[slot].armed && g_sway_st[slot].comp && g_setMasterPose) {
        void* body = ClothMerge_MasterComponent();
        if (body) { __try { g_setMasterPose(g_sway_st[slot].comp, body, true); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }
    free(g_sway_st[slot].pose);
    memset(&g_sway_st[slot], 0, sizeof(SwayState));
}

// Detach the garment from the body's skeleton so it can carry its own, seeded with the body's
// current pose so nothing snaps on the frame it takes over.
static bool SwayArm(void* comp, int slot) {
    SwayRelease(slot);
    void* body = ClothMerge_MasterComponent();
    if (!body || !g_setMasterPose || !g_finalizeBones) {
        TwkLog("[sway] cannot arm garment %d: body=%p detach=%p publish=%p",
               slot, body, (void*)g_setMasterPose, (void*)g_finalizeBones);
        return false;
    }

    const int rIdx = twkI(body, SMC_CST_READ) & 1;
    const float* src = (const float*)twkP((uint8_t*)body + SMC_CSTA + (size_t)rIdx * 16, 0);
    const int nSrc = twkI((uint8_t*)body + SMC_CSTA + (size_t)rIdx * 16, 8);
    if (!src || nSrc <= 0 || nSrc > 512) {
        TwkLog("[sway] cannot arm garment %d: body has %d bone transforms", slot, nSrc);
        return false;
    }

    SwayState& w = g_sway_st[slot];
    w.pose = (float*)malloc((size_t)nSrc * 8 * sizeof(float));
    if (!w.pose) return false;
    for (int b = 0; b < nSrc; b++) {                     // seed from the body, exactly
        const float* t = src + (size_t)b * 12;
        memcpy(w.pose + b*8, t, 4 * sizeof(float));       // rotation
        memcpy(w.pose + b*8 + 4, t + 4, 3 * sizeof(float));// position
    }
    w.nBones = nSrc; w.comp = comp;

    __try { g_setMasterPose(comp, nullptr, true); }       // take the garment off the body's skeleton
    __except (EXCEPTION_EXECUTE_HANDLER) { SwayRelease(slot); return false; }
    w.armed = true;
    TwkLog("[sway] garment %d now carries its own %d bones -- lag %.0f/s, up to %.0f cm",
           slot, nSrc, g_swayRate, g_swayMax);
    return true;
}

static void SwayDrive(void* comp, int slot, float dt) {
    SwayState& w = g_sway_st[slot];
    if (!w.armed || !g_finalizeBones) return;
    void* body = ClothMerge_MasterComponent();
    if (!body) return;

    const int rIdx = twkI(body, SMC_CST_READ) & 1;
    const float* src = (const float*)twkP((uint8_t*)body + SMC_CSTA + (size_t)rIdx * 16, 0);
    const int nSrc = twkI((uint8_t*)body + SMC_CSTA + (size_t)rIdx * 16, 8);
    if (!src || nSrc < w.nBones) return;

    const int eIdx = twkI(comp, SMC_CST_EDIT) & 1;
    float* dst = (float*)twkP((uint8_t*)comp + SMC_CSTA + (size_t)eIdx * 16, 0);
    const int nDst = twkI((uint8_t*)comp + SMC_CSTA + (size_t)eIdx * 16, 8);
    if (!dst || nDst < w.nBones) return;

    if (dt < 0.0001f) dt = 0.0001f; else if (dt > 0.1f) dt = 0.1f;
    const float a = 1.0f - expf(-g_swayRate * dt);        // frame-rate independent catch-up

    for (int b = 0; b < w.nBones; b++) {
        const float* t = src + (size_t)b * 12;
        float* p = w.pose + b*8;

        // rotation: shortest-arc blend toward the body's
        float d = p[0]*t[0] + p[1]*t[1] + p[2]*t[2] + p[3]*t[3];
        const float sgn = (d < 0.0f) ? -1.0f : 1.0f;
        for (int k = 0; k < 4; k++) p[k] += (t[k]*sgn - p[k]) * a;
        float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2] + p[3]*p[3]);
        if (len > 1e-6f) for (int k = 0; k < 4; k++) p[k] /= len;
        // ...but never let it trail so far that the garment leaves the body
        d = p[0]*t[0] + p[1]*t[1] + p[2]*t[2] + p[3]*t[3];
        if (d > 1.0f) d = 1.0f; else if (d < -1.0f) d = -1.0f;
        const float angDeg = 2.0f * acosf(fabsf(d)) * 57.29578f;
        if (angDeg > g_swayRot) {
            const float k = 1.0f - (g_swayRot / angDeg);
            for (int k2 = 0; k2 < 4; k2++) p[k2] += (t[k2] - p[k2]) * k;
            len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2] + p[3]*p[3]);
            if (len > 1e-6f) for (int k2 = 0; k2 < 4; k2++) p[k2] /= len;
        }

        // position, with the same hard limit
        for (int k = 0; k < 3; k++) p[4+k] += (t[4+k] - p[4+k]) * a;
        const float ox = p[4]-t[4], oy = p[5]-t[5], oz = p[6]-t[6];
        const float off = sqrtf(ox*ox + oy*oy + oz*oz);
        if (off > g_swayMax) {
            const float k = g_swayMax / off;
            p[4] = t[4] + ox*k; p[5] = t[5] + oy*k; p[6] = t[6] + oz*k;
        }

        float* o = dst + (size_t)b * 12;
        memcpy(o, p, 4 * sizeof(float));                  // rotation
        memcpy(o + 4, p + 4, 3 * sizeof(float));          // position
        o[7] = 0.0f;
        // DEAD END, tested: passing the body's bone SCALE through here instead of forcing 1 does NOT
        // fix custom gear that renders at the wrong size (the "scale up, apply transforms, scale back
        // down" authoring workflow). Those bones are the body's own and carry scale 1 regardless, so
        // there is nothing to pass through -- whatever scale such an asset carries lives elsewhere,
        // most likely in its vertices relative to the shared skeleton, which makes it the asset's
        // problem and not this path's. Other custom trousers work untouched on this same code.
        o[8] = 1.0f; o[9] = 1.0f; o[10] = 1.0f; o[11] = 0.0f;   // scale
    }

    __try { g_finalizeBones(comp); }                      // publish them, the engine's own way
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}


// =============================================================================================
// GARMENT LAG -- the safe way to make clothing move.
//
// The garment stays posed from the body exactly as before, so it can never lose its pose or its
// material; the whole garment is simply allowed to sit slightly behind where the body is, easing
// back as you settle. That reads as weight and looseness rather than clothing painted on skin.
//
// It is deliberately modest -- a few centimetres. It cannot detach clothing, cannot break drawing,
// and cannot produce a rest pose, because nothing about how the garment is posed or drawn changes.
// =============================================================================================
typedef void (*SetRelLocRotFn)(void* comp, const void* loc, const void* rot,
                               bool sweep, void* hit, int teleport);
static SetRelLocRotFn g_setRelLocRot = nullptr;
static const char* SIG_SET_RELLOCROT =
    "48 8B C4 48 89 58 08 57 48 81 EC 80 00 00 00 F2 0F 10 81 1C 01 00 00 41 0F B6 F9 0F 2E 02";

struct LagState { float lagged[3]; bool have; };
static LagState g_lagSt[kSimSlots] = {};

static void LagDrive(void* comp, int slot, float dt) {
    if (!g_lag || !g_setRelLocRot || slot < 0 || slot >= kSimSlots) return;
    void* body = ClothMerge_MasterComponent();
    if (!body) return;
    const float* bodyPos = (const float*)((uint8_t*)body + SC_COMPLOCATION);
    LagState& L = g_lagSt[slot];
    if (!L.have) { L.lagged[0]=bodyPos[0]; L.lagged[1]=bodyPos[1]; L.lagged[2]=bodyPos[2];
                   L.have = true; return; }

    if (dt < 0.0001f) dt = 0.0001f; else if (dt > 0.1f) dt = 0.1f;
    const float a = 1.0f - expf(-g_lagRate * dt);
    for (int k = 0; k < 3; k++) L.lagged[k] += (bodyPos[k] - L.lagged[k]) * a;

    // how far behind we are, in world space, hard-capped so clothing never leaves the body
    float d[3] = { L.lagged[0]-bodyPos[0], L.lagged[1]-bodyPos[1], L.lagged[2]-bodyPos[2] };
    const float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (len > g_lagMax) { const float k = g_lagMax / len; d[0]*=k; d[1]*=k; d[2]*=k; }

    const float rot[3] = { 0.0f, 0.0f, 0.0f };
    __try { g_setRelLocRot(comp, d, rot, false, nullptr, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Hand every garment mesh back exactly as it shipped.
//
// MUST be called before a marked garment can go through FSkeletalMeshMerge again. A bound garment
// carries our cloth asset id, guid and per-vertex mapping ON ITS SECTIONS, and the merge copies
// section data into the new body mesh -- which has no cloth assets. The merged mesh then draws
// sections that claim cloth asset 0, and the render thread reads a null cloth buffer and dies. That
// is the "fatal error after changing clothes in the shop": nothing is wrong with the cloth, it is
// that a mesh still wearing our marks was allowed into a merge.
//
// The per-vertex mapping we allocated is left to the engine's own array teardown.
// A shape knob (movement, cuff tightness, hem lift) is baked into the simulation when it is BUILT,
// so changing one has to build again. Clearing the slots alone is not enough: SetupGarment re-adopts
// a mesh that still carries our asset -- the shortcut that keeps re-worn clothing cheap -- and hands
// back the OLD shape, which is why a slider only took effect after toggling cloth off and on.
//
// Deferred, because a slider fires continuously while it is being dragged and each rebuild is a real
// piece of work. Mark the moment it changed; the pump does the release once the value settles.
static uint64_t g_shapeDirtyMs = 0;
void ClothSim_Rebuild() { g_shapeDirtyMs = GetTickCount64(); }

void ClothSim_ReleaseAll() {
    __try {
        // FIRST: put the garments' VERTICES back. A driven garment has our simulated shape written
        // into its mesh, stored pre-skinning, so leaving it there strands the clothing in a distorted
        // rest pose that no longer follows the body -- and a later rebuild would measure that warped
        // geometry as if it were the garment. Handing the sections back is not enough on its own.
        for (int i = 0; i < kSimSlots; i++) DirectRelease(i);

        RestoreSections(nullptr);                 // nullptr = every owner we ever marked
        for (int i = 0; i < kBuiltMeshes; i++) {
            if (!g_doneMesh[i]) continue;
            *(int*)((uint8_t*)g_doneMesh[i] + SM_MESHCLOTH + 8) = 0;   // drop our asset from the list
            g_doneMesh[i] = nullptr; g_doneAsset[i] = nullptr;
        }
        RestoreMeshFlag(nullptr);
        for (int i = 0; i < kSimSlots; i++) {
            g_sim[i].mesh = nullptr; g_sim[i].asset = nullptr; g_sim[i].serial = -1;
            g_normalsChecked[i] = false;
        }
        g_asset = nullptr;

        // ⛔ DO NOT rebuild the slave components' render proxies from here. It was added in 3.17.1 so
        // that switching cloth off returned a BOUND garment immediately instead of leaving it frozen
        // in its last simulated shape -- but this function also runs on a map change, where those
        // components belong to a character that no longer exists, and rebuilding a proxy on one is a
        // wild read on a render/worker thread. Two crashes entering the apartment; reverted in 3.17.3.
        // Clearing the slaves first (3.17.2) was not enough on its own.
        //
        // The toggle-off freeze is the cosmetic price and is a KNOWN ISSUE: a bound garment holds its
        // last shape until the next map load. Fixing it properly means rebuilding the proxy from the
        // MENU path only, where the component is known live -- not from the shared release path.
        TwkLog("[quad] every garment handed back in its shipped state");
    } __except (EXCEPTION_EXECUTE_HANDLER) { TwkLog("[quad] release faulted"); }
}

// Faults IN A ROW. Reset by any clean pump, so a fault at every map change can never accumulate to the
// five that switch cloth off for the session -- which would present as "cloth stopped working" with a
// cause five map loads in the past.
static int g_pumpFaults = 0;

// Drop our own cached state after a fault, touching nothing that belongs to the game. See the handler.
static void ForgetAfterFault() {
    for (int i = 0; i < kSimSlots; i++) {
        free(g_dir[i].origPos);   free(g_dir[i].origTan); free(g_dir[i].invRefPose);
        free(g_dir[i].weld);      free(g_dir[i].boneMats); free(g_dir[i].boneOk);
        free(g_dir[i].refPoseCS);
        memset(&g_dir[i], 0, sizeof(g_dir[i]));
        g_sim[i].mesh = nullptr; g_sim[i].asset = nullptr; g_sim[i].serial = -1;
        g_normalsChecked[i] = false;
        g_dirWhy[i] = nullptr;
    }
    g_asset = nullptr;
}

void ClothSim_PumpFrame() {
    if (!g_ok || !g_ready) return;
    // A shape knob changed and has settled: hand the garments back, which drops the asset registry
    // too, so the next frame builds them again with the new value.
    if (g_shapeDirtyMs && GetTickCount64() - g_shapeDirtyMs > 300) {
        g_shapeDirtyMs = 0;
        if (g_on) { ClothSim_ReleaseAll(); TwkLog("[quad] cloth shape changed -- rebuilding"); }
    }
    __try {
        const long serial = ClothMerge_GarmentSerial();
        const int  nSlots = ClothMerge_SlaveCount();
        // ---- let a dress SETTLE before creating cloth resources ------------------------------------
        // Building a cloth asset allocates cloth data and creates RHI resources on meshes the character
        // shares. A map load dresses the new character while the old world's resources are still being
        // released in batch, and doing both at once died on the RENDERING thread inside
        // FBatchedReleaseResources::Flush (`reading 0x94e4002f`, entering the apartment) -- the same
        // address and the same class as the 3.17.1 proxy rebuild that had to be reverted: never touch
        // the renderer while teardown is in flight. The field log showed cloth arming 100 ms into the
        // load dress, and the process gone 17 ms later.
        //
        // ⚠️ This is a TIMING mitigation, not a proven mechanism. `ClothArmDelayMs` exists so it can be
        // measured rather than believed: if a crash survives the default, raising it well up (3000) and
        // finding it clean says the race is real and the window is just longer; still crashing at 3000
        // says the cause is elsewhere and this should come back out.
        static long   lastSerial  = -1;
        static double serialAt    = 0.0;
        static bool   heldLogged  = false;
        const double nowSec = (double)GetTickCount64() / 1000.0;
        if (serial != lastSerial) {
            // A dress arrived. Restart the settle window and say so -- if this line repeats with a
            // RISING serial, the window is being re-armed faster than it can expire and the delay is
            // what is stopping cloth coming back, which is exactly the failure to look for.
            if (g_on && lastSerial != -1)
                TwkLog("[quad] dress %ld -> %ld: holding the cloth build for %d ms",
                       lastSerial, serial, g_armDelayMs);
            lastSerial = serial; serialAt = nowSec; heldLogged = false;
        }
        const double heldMs = (nowSec - serialAt) * 1000.0;
        // The window expires on its own, so it can only DELAY a build -- unless dresses keep arriving,
        // which re-arms it and can hold cloth off indefinitely. That is the one way this mitigation
        // could cost cloth entirely, so both edges are logged above rather than left to be inferred.
        const bool armSettled = heldMs >= (double)g_armDelayMs;
        if (g_on && armSettled && !heldLogged) {
            heldLogged = true;
            TwkLog("[quad] dress %ld settled after %.0f ms -- building now", serial, heldMs);
        }
        int running = 0, totalActors = 0;
        float simTime = 0.0f;
        void* firstSim = nullptr;

        for (int slot = 0; slot < nSlots && slot < kSimSlots; slot++) {
            void* comp = ClothMerge_SlaveComponent(slot);
            if (!comp) continue;
            void* mesh = twkP(comp, SMC_SKELMESH);

            // Key off the un-merge's own garment event, not pointer polling: merged meshes are
            // pooled, so a mesh pointer neither reliably changes on a re-dress nor proves the slave
            // is wearing anything. Validate before building -- a mesh whose name will not resolve
            // is one that must not be handed to a solver (that faulted in the first field run).
            // ⚠️ THE SERIAL IS CONSUMED ONLY ON SUCCESS. It used to be taken at the top, before any of
            // the work: one transient failure (SetupGarment bails silently if the mesh's name will not
            // resolve yet) therefore threw that dress away for good, and cloth stayed missing until
            // something dressed the character again. Field symptom: fine at load, fine on the first map
            // change, then silently no physics on the second and every one after. Leaving the serial
            // alone makes a failure a RETRY instead of a loss.
            //
            // The old chain had a second hole: same mesh, no asset matched nothing at all, so the slot
            // was marked done having built nothing. That is now an explicit retry.
            if (g_on && serial != g_sim[slot].serial && armSettled) {
                static double nextTry[kSimSlots] = {};
                static double nextWhine[kSimSlots] = {};
                bool ok = false;
                if (!mesh) {
                    // Nothing worn in this slot: a real outcome, and settled -- take the serial.
                    g_sim[slot].mesh = nullptr; g_sim[slot].asset = nullptr;
                    ok = true;
                } else if (mesh != g_sim[slot].mesh) {
                    if (nowSec >= nextTry[slot]) {
                        nextTry[slot] = nowSec + 0.25;      // a failing build must not run every frame
                        if (SetupGarment(comp, mesh, slot)) { g_sim[slot].mesh = mesh; ok = true; }
                        else                               { g_sim[slot].asset = nullptr; }
                    }
                } else if (g_sim[slot].asset) {
                    // Same garment, fresh component state: the actors went with the old render
                    // state, so re-create them rather than rebuilding the asset.
                    *(uint8_t*)((uint8_t*)comp + SMC_DISABLECLOTH) = 0;
                    if (g_recreate) g_recreate(comp);
                    ok = true;
                } else if (nowSec >= nextTry[slot]) {
                    // Same mesh recorded but no asset -- a build that failed earlier. Retry it rather
                    // than sitting there with the slot marked handled.
                    nextTry[slot] = nowSec + 0.25;
                    if (SetupGarment(comp, mesh, slot)) ok = true;
                }
                if (ok) g_sim[slot].serial = serial;
                else if (nowSec >= nextWhine[slot]) {
                    nextWhine[slot] = nowSec + 2.0;
                    char gn[64];
                    TwkLog("[quad] slot %d: could not arm '%s' for dress %ld -- retrying", slot,
                           (mesh && CatchSound_ObjName(mesh, gn, sizeof(gn))) ? gn : "?", serial);
                }
            }

            // ---- gravity, re-applied every frame: the actors are rebuilt whenever the garment
            // changes, and a value that only sometimes takes is worse than none.
            void* sim = twkP(comp, SMC_CLOTHSIM);
            uint8_t* actors = sim ? (uint8_t*)twkP(sim, NVSIM_ACTORS) : nullptr;
            const int nAct = sim ? twkI(sim, NVSIM_ACTORS + 8) : 0;
            for (int i = 0; i < nAct && actors; i++) {
                uint8_t* a = actors + (size_t)i * ACTOR_STRIDE;
                *(uint8_t*)(a + ACTOR_USEGRAVOVR) = 1;
                float* g = (float*)(a + ACTOR_GRAVOVR);
                g[0] = 0.0f; g[1] = 0.0f; g[2] = -980.0f * ClothSim_Gravity;
            }
            if (nAct > 0) CheckSolverNormals(comp, slot);
            {
                static double lastL = 0.0;
                const double nw2 = (double)GetTickCount64() / 1000.0;
                const float dtl = (lastL > 0.0) ? (float)(nw2 - lastL) : 0.016f;
                if (slot == 0) lastL = nw2;
                __try { LagDrive(comp, slot, dtl); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (g_sway) {
                static double lastT = 0.0;
                const double nw = (double)GetTickCount64() / 1000.0;
                const float dt = (lastT > 0.0) ? (float)(nw - lastT) : 0.016f;
                if (slot == 0) lastT = nw;
                __try { SwayDrive(comp, slot, dt); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (g_direct && nAct > 0 && ClothMerge_GarmentWantsDirect(mesh)) {
                __try { DirectDrive(comp, mesh, slot); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (sim) {
                totalActors += nAct;
                if (nAct > 0) running++;
                if (!firstSim) { firstSim = sim; simTime = *(float*)((uint8_t*)sim + NVSIM_SIMTIME); }
            }
        }

        // ---- proof of life, once a second. The solver's own clock is the honest witness.
        static double lastLog = 0.0;
        static float  prevTime = -1.0f;
        const double now = (double)GetTickCount64() / 1000.0;
        if (firstSim && now - lastLog >= 1.0) {
            lastLog = now;
            const bool ticking = simTime != prevTime;
            // Only when something CHANGES -- a garment count, or the solver starting/stopping. Once a
            // second forever was the single largest thing in the log (a five-minute session ran to
            // 432 KB, a third of it this line) and it says the same thing every time. ClothDebugLog=1
            // restores the full per-second heartbeat for diagnosis. The menu's status string below is
            // updated either way, so "solver:" stays live without any of this.
            // ---- FREEZE FORENSICS -------------------------------------------------------------
            // A hard freeze writes nothing, so the last line before silence has to be worth reading.
            // Every second: how many pump passes have completed (a frozen game thread stops this dead),
            // the solver's own clock (a stalled SOLVER stops THIS while the pump keeps counting), and
            // the actor count (residue accumulating across map changes would show here). Which of the
            // two clocks stops first says whether the game thread or the cloth task went down.
            static unsigned long long pumps = 0;
            TwkLog("[beat] pump #%llu | garments=%d actors=%d simTime=%.4f (%s)",
                   ++pumps, running, totalActors, simTime,
                   (simTime != prevTime) ? "solver advancing" : "SOLVER STOPPED");
            static int  lastRunning = -1, lastActors = -1, lastTicking = -1;
            if (g_debugLog || running != lastRunning || totalActors != lastActors ||
                (int)ticking != lastTicking) {
                lastRunning = running; lastActors = totalActors; lastTicking = (int)ticking;
                TwkLog("[quad] garments=%d actors=%d | simTime=%.4f %s", running, totalActors, simTime,
                       ticking ? "<-- TICKING" : (simTime > 0.0f ? "(steady)" : "(idle)"));
            }
            prevTime = simTime;
            snprintf(g_status, sizeof(g_status), "garments=%d actors=%d simTime=%.3f",
                     running, totalActors, simTime);
        }
        g_pumpFaults = 0;                  // a clean pass: the run of faults is over
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A FAULT MEANS OUR CACHED POINTERS ARE ALREADY DEAD -- so stop using them, do not merely skip
        // the frame. Measured during a map change: three faults over ~370 ms while the old character was
        // being torn down and before the new one was noticed. Skipping kept us coming back to the same
        // dead component every frame, and DirectDrive does not only READ -- it writes the garment's
        // vertex buffer and calls BeginUpdateResourceRHI on it, which enqueues a render command against
        // a resource the teardown is releasing. That is how a caught game-thread fault turns into an
        // uncatchable render-thread crash in FBatchedReleaseResources::Flush.
        //
        // Forgetting is deliberately NOT ClothSim_ReleaseAll: that restores vertices (a memcpy into
        // memory that may already be freed) and unmarks meshes. Here we drop only our OWN allocations
        // and cached pointers, touching neither the meshes nor the renderer. The next dress rebuilds.
        ForgetAfterFault();
        if (++g_pumpFaults >= 5) { g_ok = 0; TwkLog("[quad] pump faulted %d times in a row -- cloth off",
                                                   g_pumpFaults); }
        else TwkLog("[quad] pump faulted (%d) -- dropped our cached state, will rebuild on the next dress",
                    g_pumpFaults);
    }
}


// ---------------------------------------------------------------------------------------------
// KEEPING CLOTH ALIVE IN THE REPLAY EDITOR
//
// Everything in this mod is pumped from InputHandler::Tick -- and in a replay you are not driving
// the skater, so that stops and the whole mod goes quiet (a 14-second hole in the log, no lines from
// ANY module). The simulation itself keeps running: the garment components still tick, the solver
// still solves. What stops is US putting its result into the mesh, so the garment holds the last
// vertices we wrote -- frozen in whatever pose the replay was entered from, exactly as reported.
//
// AReplayManager::Tick (Epic 0x340edb0 / Steam 0x33d5c70, unique in both) runs while the editor is
// up, so drive the cloth from there as well. ONLY the cloth: the rest of the mod is about driving a
// skater and has no business running over a replay.
typedef void (*ReplayTickFn)(void* self, float dt);
static ReplayTickFn g_origReplayTick = nullptr;
static void*        g_replayTickAt   = nullptr;
static const char*  SIG_REPLAY_TICK =
    "40 57 48 83 EC 50 0F 29 74 24 40 48 8B F9 0F 28 F1 E8 ?? ?? ?? ?? F6 87 A9 02 00 00 02";

static void hkReplayTick(void* self, float dt) {
    if (g_origReplayTick) g_origReplayTick(self, dt);
    // After the replay has posed everything for this frame, so we read the pose it just set.
    // NOTE: the shell pump rides InputHandler::Tick, which keeps ticking in the replay editor, so
    // cloth is pumped twice a frame in there. Standing one of them down was tried while chasing a
    // random crash that turned out to be ANOTHER MOD's, and reverted with the rest of that work.
    __try { ClothSim_PumpFrame(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ClothSim_Install() {
    g_malloc         = (MallocFn)TwkScanExe(SIG_MALLOC);
    g_addCfg         = (AddConfigsFn)TwkScanExe(SIG_ADD_CONFIGS);
    g_recreate       = (RecreateFn)TwkScanExe(SIG_RECREATE);
    g_assetsInUse    = (AssetsInUseFn)TwkScanExe(SIG_ASSETS_IN_USE);   // probe only, never fatal
    g_updClothTick   = (UpdClothTickFn)TwkScanExe(SIG_UPD_CLOTHTICK);
    g_allocClothData = (AllocClothDataFn)TwkScanExe(SIG_ALLOC_CLOTHDATA);
    g_beginInitRes   = (BeginInitResFn)TwkScanExe(SIG_BEGIN_INIT_RES);
    g_beginUpdateRes = (BeginUpdateResFn)TwkScanExe(SIG_BEGIN_UPDATE_RES);
    g_recreateRender = (RecreateRenderFn)TwkScanExe(SIG_RECREATE_RENDER);
    { uint8_t* anchor = TwkScanExe(SIG_FINALIZE_BONES);
      g_finalizeBones = anchor ? (FinalizeBonesFn)(anchor - kFinalizeAnchorOff) : nullptr; }
    g_setMasterPose  = (SetMasterPoseFn)ClothMerge_SetMasterPoseFn();
    g_setRelLocRot   = (SetRelLocRotFn)TwkScanExe(SIG_SET_RELLOCROT);
    if (!g_malloc || !g_addCfg || !g_recreate) {
        g_ok = 0;
        TwkLog("[quad] NOT ready (malloc=%p addConfigs=%p recreate=%p) -- cloth off",
               (void*)g_malloc, (void*)g_addCfg, (void*)g_recreate);
        return;
    }
    g_replayTickAt = TwkScanExe(SIG_REPLAY_TICK);
    if (g_replayTickAt) {
        if (MH_CreateHook(g_replayTickAt, (void*)&hkReplayTick, (void**)&g_origReplayTick) != MH_OK
            || MH_EnableHook(g_replayTickAt) != MH_OK) {
            g_origReplayTick = nullptr;
            TwkLog("[quad] could not drive cloth from the replay editor -- it will freeze there");
        } else TwkLog("[quad] cloth also driven from the replay editor");
    } else TwkLog("[quad] replay tick not found -- cloth will freeze in the replay editor");

    g_ready = true;
    TwkLog("[quad] ready (malloc=%p addConfigs=%p recreate=%p) -- ClothQuad=%d render=%d",
           (void*)g_malloc, (void*)g_addCfg, (void*)g_recreate, g_on, g_render);
    if (g_lag) TwkLog("[lag] ON (setter=%p, rate %.0f/s, up to %.1f cm behind the body)",
                      (void*)g_setRelLocRot, g_lagRate, g_lagMax);
    TwkLog("[direct] vertex upload=%p (init=%p) -- update is the one that matters",
           (void*)g_beginUpdateRes, (void*)g_beginInitRes);
    if (g_sway) TwkLog("[sway] ON (publish=%p detach=%p, rate %.0f/s, max %.0f cm / %.0f deg)",
                       (void*)g_finalizeBones, (void*)g_setMasterPose, g_swayRate, g_swayMax, g_swayRot);
}

void ClothSim_DrawMenu(const OmpMenuApi* api) {
    char b[128];
    if (!g_ok) { api->TextDisabled("Cloth quad: unavailable / disabled by a fault"); return; }
    bool on = g_on != 0;
    if (api->Checkbox("Cloth quad flag (phase B test)", &on)) { g_on = on ? 1 : 0; TwkMarkDirty(); }
    api->SameLine(); api->TextDisabled("(hand-built cloth on the un-merged shirt; re-dress to apply)");
    snprintf(b, sizeof(b), "solver: %s", g_status);
    api->TextDisabled(b);
}
