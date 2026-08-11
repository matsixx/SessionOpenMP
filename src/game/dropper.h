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
//
// THE OBJECT DROPPER, game side. LB off the board opens Session's prop editor: you spawn, drag and
// call back rails/ramps/ledges from an inventory, and the result is saved PER MAP IN YOUR OWN PROFILE
// and re-spawned at every level load. So two players arrive on a map each already standing in their
// own set, and "just send everyone's objects" would spawn two near-but-not-identical copies of every
// popular spot -- intersecting geometry you can actually grind into. Matching them up fuzzily makes it
// worse; a rail 20 cm off is uglier than either extreme.
//
// The model instead:
//   * every object has exactly ONE owner. Yours are real, game-owned actors. A peer's are mod-spawned
//     REMOTE actors -- invisible to your dropper, never saved, destroyed when they leave.
//   * a peer contributes the objects it OWNS AND SHOWS (_allObjects minus its hidden set).
//   * with the shared policy, a non-host HIDES its own pre-session set and shows the host's instead.
//     Hiding is visual + collision only. NOTHING HERE EVER WRITES A SAVE.
// One saved set is canonical, so overlap is impossible by construction rather than something detected.
//
// Change detection is a DIFF OF `_allObjects`, not a set of delegate hooks: place, duplicate,
// call-back, cancel, revert-rotation and stick-to-ground all show up as an added/removed/moved entry,
// with no per-operation reverse engineering. Same call as the board carry -- transport the resolved
// pose, do not reproduce the machinery.
#pragma once
#include <cstdint>

namespace omp { namespace game { namespace dropper {

// Hard cap on objects tracked in either direction. Session's dropper is inventory-limited (you buy
// the props), so a real set is dozens; the cap exists so a hostile or broken peer cannot make us
// spawn without bound, and so the wire's part count is knowable in advance.
static const int kMaxObjects = 256;

struct Tuning {
    bool  enabled      = true;    // dev kill switch for the whole feature
    float moveEpsCm    = 0.5f;    // pose delta that counts as a move, per axis
    float moveEpsQuat  = 0.0015f; // ~0.2 degrees
    // How fast a remote object chases the pose we were sent. Objects are static most of the time and
    // the move lane runs at ~25 Hz, so a straight stamp visibly steps while someone drags one.
    // 0 = stamp (no smoothing).
    float followRate   = 14.0f;
    // Anything further than this from the incoming target is placed instantly rather than slid: a
    // brand-new object, a duplicate, or a set arriving after a level load must not fly across the map.
    float followSnapCm = 300.0f;
    // How far a settled object may drift from where we put it before we put it back. The drive stops
    // writing once it has arrived, so this is what notices the prop's own tick or a physics settle
    // moving it afterwards -- without it, an object silently ends up somewhere its owner never put it.
    float driftEpsCm   = 2.0f;
};
extern Tuning g_tun;

// One dropped object as it travels. `cls` is the object's CLASS name, which is exactly the id
// Session's own save file uses: UObjectDropperObjectsDatabase::GetObjectInformationByID matches an
// FName against FSoftObjectPath::GetAssetName() of each catalogue entry's class path, and that asset
// name IS the class's own name ("BP_Whatever_C").
// Position and rotation are read from, and written to, the root component's world transform -- a
// FVector and an FQuat on both ends, so no euler ever enters the path (standing rule 1). The save's
// FRotator is Session's business, not ours.
// ONLY INVENTORY OBJECTS TRAVEL. The dropper also lets you shove the LEVEL's own benches and barriers
// around, and those are deliberately left alone: they are not published, not hidden and not driven.
// (Syncing them was built and withdrawn -- see the design note at the end of this header.)
// The two are told apart by the component the game itself attaches: UObjectDropperStorableObject
// derives from UObjectDropperPickableObject (272-byte base, one extra byte at 0x110), and only an
// object that can be stored back into an inventory carries the derived one.
struct ObjRec {
    char  id[64];          // the object's CLASS name -- the id Session's own save file uses
    float loc[3];
    float quat[4];
};

// ---- availability -----------------------------------------------------------------------------
bool  Available();              // symbols resolved AND the singleton decoded
void* Manager();                // the level's AObjectDropperManager, or null before it exists
bool  LocalActive();            // the local player is IN the dropper right now (drives the send rate)

// ---- our own set ------------------------------------------------------------------------------
// Walk `_allObjects` and write out the objects we own and show. Hidden (adopted-away) objects and any
// remote actor of ours are excluded -- and remote actors found in there are REMOVED from the array as
// a side effect, which is the primary guard against a peer's prop reaching the local save.
// Returns the count written; `actorsOut` (optional) receives the matching actor pointers.
int   EnumerateOwn(ObjRec* out, void** actorsOut, int cap);

// ---- adoption ---------------------------------------------------------------------------------
// Snapshot the current `_allObjects` as "mine, from my save" and hide + decollide all of it, so the
// world shows exactly one canonical set. Reversible and save-neutral. Objects placed AFTER this are
// not in the snapshot, so they stay visible and get published as ours.
void  HideOwnSet(void (*logf)(const char*));
void  RestoreOwnSet(void (*logf)(const char*));
bool  OwnSetHidden();

// ---- remote objects ---------------------------------------------------------------------------
// Resolve `r.cls` through the game's own catalogue, load its class, spawn it and neuter it. Null when
// this install does not have that object (a DLC the peer owns and we do not) -- the same shape as a
// cosmetic item we lack, and the caller treats it as "skip and say so once".
void* SpawnRemote(void* ownPawn, const ObjRec& r, void (*logf)(const char*));
// Place a remote object. `cur` is the caller's smoothing state (in/out, seeded from the first place);
// pass dt = 0 or followRate = 0 to stamp.
void  DriveRemote(void* actor, const float loc[3], const float quat[4],
                  float curLoc[3], float curQuat[4], float dt);
void  DestroyRemote(void* actor, void (*logf)(const char*));
bool  IsRemote(void* actor);
int   RemoteCount();

// ---- THE SESSION'S OWN COPIES OF THE LEVEL'S PROPS -------------------------------------------------
// The level's benches and barriers are NOT moved for a session. Instead each client HIDES its own
// (which its save has arranged its own way) and everyone spawns a fresh copy at the prop's MAP
// DEFAULT -- so every machine reconstructs the same starting layout with nothing transferred, because
// "where the map put it" is the same everywhere by definition.
// What that buys, and why the previous three attempts failed without it: a session prop is an actor
// WE own, exactly like a peer's inventory object. Nothing writes to the game's own level actors, so
// there is no drift correction fighting the game, no restoring poses on the way out, and no need to
// know where a bench "should" be at leave time -- the copy is simply destroyed and the original
// unhidden.
// They are deliberately left PICKABLE, so anybody can rearrange them. That is safe only because of
// the Save hook (see game_syms.h): a pickable prop of ours can otherwise be called back into the
// local player's inventory and written into their profile.
// Identity is the ORIGINAL actor's name, which is baked into the map package and therefore the same
// on every install -- the one shared handle available for an object every client spawns for itself.
bool  SessionWorldBegin(void* ownPawn, const char* actorName, void (*logf)(const char*));
void* SessionWorldCopy(const char* actorName);       // our spawned copy, or null
bool  SessionWorldPose(const char* actorName, float loc[3], float quat[4]);   // where OUR copy is now
void  SessionWorldEnd(void (*logf)(const char*));    // destroy every copy, unhide every original
// Put the hide back. The originals are real LEVEL actors, so things other than us have opinions about
// their visibility -- the replay editor's exit pass restores whatever it has registered, which
// un-hides every one of them and leaves the player looking at their own arrangement standing inside
// the session's. Cheap enough to simply re-assert on a slow beat rather than trying to enumerate
// everything that might interfere.
void  ReassertSessionWorldHidden();
int   SessionWorldCount();
// Actor names of the level props THIS player's save has moved -- our contribution to the session's
// set. A prop nobody has ever moved is already at its map default on every machine and needs no copy.
int   MovedWorldNames(char out[][64], int cap);
// Is the local player holding this actor RIGHT NOW? (`_selectedObjects`, the dropper's own list.)
// Picking a session prop up is what claims it: a discrete, observable event, rather than a guess made
// by comparing poses that disagree -- which is how two clients ended up in a tug of war.
bool  IsSelectedLocally(void* actor);
// Where an actor is right now.
bool  ActorPose(void* actor, float loc[3], float quat[4]);
// THE HARD SAVE GUARD. Strip every actor of ours out of the manager's `_allObjects` and report how
// many went. Called from the Save hook, immediately before the game writes the player's profile:
// session props are deliberately pickable, so one can be selected and land in that array between our
// polls, and this is the only point at which "it is not in the list the save reads" is a guarantee
// rather than a race. Returns the number removed -- non-zero is worth a line, because it means the
// race was real.
int   PurgeOursFromSaveList();
// Is that guarantee actually in place? Session props are only left PICKABLE when it is. Without the
// hook the feature degrades to look-but-do-not-touch rather than risking somebody's profile -- a
// missing symbol must cost a capability, never safety.
void  SetSaveGuardArmed(bool armed);
bool  SaveGuardArmed();

// ---- THE MAP DEFAULT -------------------------------------------------------------------------------
// Where the LEVEL put a prop, as opposed to where this player's save moved it to. Captured at the one
// moment it is knowable: `UObjectDropperPersistentHandler::Load` walks the save and moves each prop it
// names, so the pose it is about to overwrite IS the map default. Nothing later can recover it -- by
// the time anything else can look, every player's copy of the map is already arranged their own way.
// MEASUREMENT ONLY for now: this records and reports, and changes nothing about how the game behaves.
// It is the foundation two withdrawn features both needed -- restoring a prop EXACTLY on leaving a
// session, and showing a joiner the host's layout instead of their own.
void  NoteMapDefault(void* actor);          // called from the Load seam, before the save is applied
bool  MapDefaultOf(void* actor, float loc[3], float quat[4]);
int   MapDefaultCount();
void  SetInPersistentLoad(bool inside);     // the Load hook brackets its call with this
bool  InPersistentLoad();

struct Stats {
    int own = 0, remote = 0;
    int spawned = 0, spawnFails = 0, unknownIds = 0, destroyed = 0;
    int purgedFromAll = 0;      // remote actors pulled back out of `_allObjects` -- see EnumerateOwn
    int faults = 0;
    int driftFixes = 0;         // settled objects the game moved and we put back
    int madeMovable = 0;        // props flipped out of Static mobility so a write can land at all
    int skipWorld = 0;          // the level's own props, left alone on purpose
    int sessionWorld = 0;       // level props currently swapped for a session copy
    int worldMissing = 0;       // names a peer contributed that this install's map does not have
    int mapDefaults = 0;        // props whose map-default pose we captured at the Load seam
    int mapDefaultMissed = 0;   // ...and ones we could not read, which would be a silent hole
    int resolvedByName = 0;     // ids the catalogue's own lookup missed and a name walk found
    // Why an object in `_allObjects` did not make it onto the wire. All four are normal in small
    // numbers; one of them equalling the array count is the whole diagnosis.
    int arrayNum = 0;           // what the game's own `_allObjects` says it holds
    int skipRemote = 0, skipHidden = 0, skipNoClass = 0, skipNoRoot = 0;
};
const Stats& St();

// Drop every remote object and un-hide our own set. Called on world change, on leaving a session and
// on ResetAll -- a dropped-object actor does not survive a level load, and forgetting one would let a
// stale pointer be destroyed later.
void  ResetAll(void (*logf)(const char*));
// The world changed under us: UE destroyed every actor. Forget the pointers WITHOUT touching them.
void  Forget();

}}} // namespace omp::game::dropper
