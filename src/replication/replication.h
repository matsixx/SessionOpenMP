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
// SessionOpenMP -- replication core: the per-peer snapshot ring, playback clock and wire codecs.
// The rules this machinery encodes:
//   * transport the truth; never derive it on the observed copy (the project's founding rule)
//   * the playback clock slews against a FILTERED error, per second, never the raw sawtooth
//   * network jitter = |localDelta - remoteDelta| with a poll-quantum deadband, asymmetric
//   * bursts beyond the rate cap need a SNAP, threshold 150 ms
//   * bools/enums are STEPPED from the at-render-time sample, never blended
//   * extrapolation is POSITION-ONLY, bounded in time and distance; pose is never guessed
#pragma once
#include <cstdint>

namespace omp { namespace repl {

// ---- audio records (see the block at the end of State for the model) -------------------------------
// Caps chosen against the transports' ~1 KB message ceiling. The format's PROMISE is that audio can
// never starve the pose: the section is written LAST and sized to what is left, so an overfull frame
// drops whole trailing audio records and the skater still moves. Assert the promise, not a packet size.
constexpr int kAudioMaxLoops  = 4;
constexpr int kAudioMaxEvents = 3;
constexpr int kAudioMaxParams = 2;    // per loop: surface type, powerslide fullness, wheel lock...
// These lengths are MEASURED against the longest names the game actually uses, and they are sized to
// hold them whole: a truncated cue (`SCU_Onboard_WheelSpinning_Looping` is 33 chars) can never resolve
// on the receiver, and a truncated parameter (`GrindDropHeightIndex` is exactly 20) sets a DIFFERENT,
// nonexistent parameter. Names travel whole or the identity is a lie. (Overflow is still safe by
// construction -- the section is sized last and drops whole records.)
constexpr int kAudioCueLen    = 48;   // the sound cue's OBJECT NAME -- name identity, never an index
constexpr int kAudioParamLen  = 24;
// Where a sound sits. A proxy stands at the sender's own world position, but its pose is delayed by
// the jitter buffer, so positions travel RELATIVE to bodyPos and are rebuilt against the proxy's
// interpolated body -- an absolute world position would drift ahead of the animation that caused it.
enum AudioAttach : uint8_t {
    kAudWorld = 0,     // spawned at a point in the world
    kAudBoard = 1,     // attached to the board's deck component (rolling, grind, powerslide...)
    kAudMesh  = 2,     // attached to the skater's mesh
    kAudRoot  = 3,     // attached to the actor root
};
// An int parameter on a live sound. This is how the game applies SURFACE TYPE
// (FSessionUtils::UpdateAudioWithSurfaceType), powerslide fullness and the wheel-lock flag -- i.e.
// replaying these is what makes a peer's grind sound like the concrete they are actually on, without
// transporting surface state at all.
struct AudioParam {
    char    name[kAudioParamLen] = {};
    int16_t value = 0;
};
struct AudioLoop {                     // a sound the sender still has playing
    uint8_t    slot   = 0;             // the sender's own handle id -- stable while the sound lives
    uint8_t    attach = kAudWorld;
    uint8_t    nParam = 0;
    // NAME INTERNING. Cue and parameter names are the bulk of a loop's wire cost -- string data the
    // receiver has already seen, resent 60 times a second for as long as the sound plays. The sender
    // sets this on a loop's FIRST appearance, on any name change, and on a ~1 s refresh; every other
    // packet carries values only and the receiver fills the names from its per-peer cache (see
    // AudioNameCache). A cache miss -- a joiner mid-loop, or every packet since the named one lost --
    // leaves the cue empty, and an empty cue is already the "skip this loop" signal downstream, so
    // the sound is simply silent until the next refresh lands. Never wrong, at worst briefly quiet.
    uint8_t    sendNames = 1;
    char       cue[kAudioCueLen] = {};
    float      rel[3] = {};
    float      vol = 1.f, pitch = 1.f;
    AudioParam param[kAudioMaxParams];
};
struct AudioEvent {                    // a one-shot
    uint16_t id     = 0;
    uint8_t  attach = kAudWorld;
    char     cue[kAudioCueLen] = {};
    float    rel[3] = {};
    float    vol = 1.f, pitch = 1.f, start = 0.f;
    uint16_t ageMs  = 0;               // how long before this packet's senderUs it fired
};

// The skater rig measured 70 bones; the cap leaves headroom without letting a hostile packet claim an
// unbounded skeleton. Rotations are smallest-three (4 B) and component-space positions f16 (6 B), so a
// full skeleton is ~700 B -- affordable precisely because it is sent only while someone is scrubbing.
constexpr int kPoseMaxBones = 96;
// THE PHYSICS OVERLAY -- BUILT, FIELD-FAILED, AND RETIRED. The idea was to stop every observer
// re-simulating every peer by transporting the OWNER's result instead: the few bones their own body
// physics moved. The lane worked end to end -- the log proved the right bones (pelvis, both arm
// chains), a perfect name mapping, and physically coherent values (0 cm at the shoulder, 13 at the
// elbow, 23 at the hand) -- but the arms still came out drooping and elongated. Three anchorings
// were tried: component space, parent-relative, and pelvis-anchored. All three failed the same way.
// The premise is what is wrong: a SUBSET of someone's bones does not compose onto a differently-posed
// skeleton. The receiver's spine and clavicle never sit exactly where the sender's do, and the error
// compounds down the arm.
// The fields and the codec REMAIN, carrying a single zero byte, deliberately: removing them changes
// the snapshot layout, which by the rule at the top of replication.cpp costs a magic letter, and Z is
// the only one left. The offline test still round-trips them, so a revival starts from working code.
constexpr int kPovMaxBones = 12;

// One instant of a remote skater.
struct State {
    float    deckPos[3]  = {};
    float    deckQuat[4] = {0,0,0,1};
    float    meshQuat[4] = {0,0,0,1};  int meshOk = 0;      // mesh rotation RELATIVE to the capsule
    float    bodyYaw = 0, bodyPitch = 0, bodyRoll = 0; int bodyRotOk = 0;
    // The body travels as a RAW QUATERNION, end to end, with no euler round-trip: the extract/rebuild
    // pair is NOT self-inverse at yaw ~ +/-180 (a pure yaw-180 quat extracts as (yaw 180, roll 180),
    // and rebuilding that lands on (0,1,0,0) -- an upside-down skater whenever the sender faces one
    // world direction). No conversion, no convention, no bug.
    // (bodyYaw/pitch/roll stay on the wire for debug display only; nothing applies them.)
    float    bodyQuat[4] = {0,0,0,1};
    float    bodyPos[3] = {};          int bodyPosOk = 0;
    float    relPos[3]  = {};          int relOk = 0;       // deck in the sender's mesh frame
    int      feetOk = 0, feetWorld = 0;
    float    lFootPos[3] = {}, lFootRot[3] = {}, rFootPos[3] = {}, rFootRot[3] = {};
    int      handOk = 0, handWorld = 0;
    float    lHandPos[3] = {}, lHandRot[3] = {}, rHandPos[3] = {}, rHandRot[3] = {};
    int      artOk = 0;                                     // 1 = trucks, 2 = trucks + wheel
    float    truckB[4] = {0,0,0,1}, truckF[4] = {0,0,0,1}, wheelBL[4] = {0,0,0,1};
    uint8_t  onBoard = 0, bailing = 0;
    // EXPLICIT, not dug out of `anim`: that blob is a PACKED FIELD SEQUENCE (offset -> index mapping
    // lives in the sender's field table), so indexing it by a struct offset reads the wrong byte. The
    // grounded flag gates the board velocity drive, so it gets its own wire field.
    uint8_t  grounded = 1;
    // The push-machinery flag byte from the sender's movement component: bit 0x40 is the push REQUEST
    // the game consumes to play its push montage. The blob's IsPushing bool renders nothing by itself
    // -- the RECEIVER re-raises this bit on its proxy's component (edge-triggered) so the game plays
    // its own animation. Transported raw; the receiver decides which bits to honour.
    uint8_t  pushFlags = 0;
    // The push STATE (skater+0xa0c, what SetPushState stores): persistent for the whole push cycle, so
    // 60 Hz sampling catches it where the request BIT (a one-frame pulse) never could. The receiver
    // calls the game's own SetPushState on every change.
    uint8_t  pushState = 0;
    // bit0 = skater+0x612, bit1 = +0x613 -- the brake flags the StartBraking multicast writes.
    uint8_t  brakeState = 0;
    // The current flip-trick definition's OBJECT NAME, empty = none. FName indices are per-process, so
    // the STRING is the only valid identity. Un-gates the crouch + every trick ratio.
    char     trickName[48] = {};
    // The crank = the trick-setup crouch: skater +0x580 bit0 / +0x658, plus the crank def's INDEX in
    // the shared UTricksDatabase::_crankList (0xffff = none). An INDEX, never an offset or a pointer
    // -- the def lives in the DB's heap TArray, so nothing about its address can travel. The receiver
    // rebuilds the pointer through its OWN controller's DB; its anim SetCrank then populates the crank
    // blend space and the crouch gates pass.
    uint8_t  crankOn = 0;
    uint16_t crankDefOff = 0xffff;   // _crankList index
    float    crankPocket = 0;
    // The current GRIND def's OBJECT NAME -- the trickName pattern verbatim. The grind upper-body pose
    // is a blend space fetched THROUGH skater +0x6d0/_currentGrindDef (+0x6c8 _targetGrindDef
    // fallback) by USkaterAnimInstance::GetGrindBlendSpace, and the grind anim-set population runs
    // INSIDE NativeUpdateAnimation (live on proxies too) -- so delivering the def un-gates the whole
    // grind pose by construction. Empty = none. (The stock game replicates grinds by NAME too: skater
    // +0x6f0 _replicatedGrindName -- name identity is the game's own choice.)
    char     grindName[48] = {};
    float    grindPitch = 0, grindYaw = 0;  // skater +0x6d8/_grindPitchRatio, +0x6dc/_grindYawRatio --
                                            // +0x6dc is copied into the active grind anim set's blend
                                            // input every anim update; plain floats, applied raw.
    // The BOARD's movement mode byte (boardMoveComp +0x188; 9 = in-hand/carried). bOnBoard flips at
    // the START of the mount while the board is still hand-carried for a beat -- driving the board
    // during that window chases the swinging hand, so mode 9 means stamp instead of drive.
    uint8_t  boardMode = 0;
    // Is the sender's board SIMULATING right now? Off the board that is the whole difference between
    // a board tucked under an arm and one loose on the ground, and the receiver cannot tell from the
    // pose alone -- both just move. Sitting sets a board down, and so does a mount the game refuses;
    // this is read from the deck's own body rather than from any one feature, so it covers both and
    // anything later that lets go of a board.
    uint8_t  boardSim = 0;
    // The board's _currentBrokenBoardState byte (0 = intact). A STATE, not an event: the receiver
    // latch-compares and calls the game's own break/rebuild on change, so a peer already broken at
    // proxy spawn renders broken with no edge ever seen. Travels value-preserving -- the game's own
    // BreakBoard_Internal takes the value as its argument.
    uint8_t  brokenState = 0;
    // The sender's HEAD LOOK off the board: SessionTweaks turns the head to look where the camera
    // looks (yaw + right, pitch + up, degrees off the body's facing), and the receiver turns the
    // proxy's head the same way. Zero when they are not looking anywhere -- riding, or SEATED: a seated
    // head rides the pose hold, which carries every bone, so the sender zeroes these there and the
    // receiver never turns a head twice. Two bytes, always.
    int8_t   headYaw = 0, headPitch = 0;
    uint16_t animLen = 0; uint8_t anim[320] = {};
    // SessionTweaks' analog crouch: the CrankIn descent clock, in seconds, while the sender is
    // scrubbing it to the stick (see the crouch-visual sync). Negative = not scrubbing -- either no
    // SessionTweaks on the sender or a plain full-depth crank -- and the proxy plays the vanilla
    // descent. Travels only while crankOn, as a u16 with a sentinel: two bytes, and only when cranked.
    float crankClock = -1.f;
    // Trick and grind names resend every frame for the length of the trick. The sender omits them on
    // packets where the name is unchanged AND a recent packet carried it in full (every 4th packet
    // refreshes, so a loss window is ~66 ms at 60 Hz); the wire marks the omission with a length
    // sentinel and the receiver restores the name from its cache. 1 = this packet carries the name.
    uint8_t trickNamed = 1, grindNamed = 1;

    // ==== AUDIO ======================================================================================
    // Captured at the game's own sound-spawn funnel, so every field here is already FULLY DETERMINED
    // -- cue, place, volume, pitch, parameters, nothing the receiver has to derive. Transporting
    // gameplay EDGES instead and replaying the handlers does not work: those handlers interrogate a
    // movement component that does not simulate on a proxy, so they no-op.
    //
    // Two shapes, deliberately different, because loss behaves differently for each:
    //   * LOOPS are STATE. The full set of still-playing sounds is resent in every packet, so a
    //     dropped packet self-heals on the next one and a sound that stops simply stops appearing.
    //   * EVENTS are one-shots. They are repeated in the next few packets with an id, and the
    //     receiver plays each id exactly once -- and they are drained on the PLAYBACK CLOCK, not on
    //     arrival, so the pop lands with the visibly jitter-buffered animation instead of ahead of it.
    // The receiver reads events out of Stream's own queue, never out of a SAMPLED State: Sample() can
    // skip whole snapshots (the playhead jumps on resync/starve/teleport) and would lose them exactly
    // when the network is worst. The copy that rides along in a sampled State is ignored by design.
    uint8_t    nLoops = 0, nEvents = 0;
    AudioLoop  loops[kAudioMaxLoops];
    AudioEvent events[kAudioMaxEvents];

    // ---- APPENDED, minor 1 (1.1.6): a spawn / marker-return collision-grace flag, bit 0. The
    // feature was WITHDRAWN in 1.1.8; the byte stays so the layout does not move, and is always
    // sent as 0. A 1.1.6 or 1.1.7 peer still reads bit 0 as its grace flag, so bit 0 must stay 0 for as
    // long as such peers exist. Bits 1-7 remain spare for later minors.
    uint8_t    spare1 = 0;
    // ---- APPENDED, minor 2 (1.1.8): the TRICK SERIAL, the owner's SetTrick count (trick_pulse.h).
    // A flick sets IsTrickPending for ONE frame and the graph consumes it; sampled once a frame it
    // reads 0 forever, so it cannot travel as a field. The count of flicks can. A minor-1 reader does
    // not see this byte and a proxy of theirs starts no trick -- which is what every build so far does.
    uint8_t    trickSerial = 0;
    // ---- APPENDED, minor 2 (1.1.8), second byte: the owner's PHYSICAL-ANIMATION state (pa_state.h).
    // Bit 0 valid, bit 1 enabled, bits 2-7 a change count. 0 = the sender did not say (a minor-1
    // packet), and the receiver keeps its own judgement.
    uint8_t    paSerial = 0;

    // ==== THE POSE LANE ==============================================================================
    // Component-space bone transforms, sent when the receiver's own anim graph cannot produce the
    // pose. There are TWO such cases and they are mirror images, both measured:
    //   1. THE SENDER is in replay playback: their drivers are inert (0 of 97 anim fields move) while
    //      58 of 70 bones do.
    //   2. A RECEIVER is in replay playback: the game does not run the anim graph for a skater during
    //      playback, so a live peer's drivers are useless to them however good they are -- their
    //      skeleton simply never gets evaluated.
    // `replaying` is what makes case 2 possible: each sender publishes whether IT is scrubbing, and a
    // live peer that sees the flag starts sending results. It rides f2 bit 4, so it costs no bytes.
    // See pose.h for why this stays a gated lane and not the default.
    // When poseN is non-zero the sender drops the anim blob and feet -- both together do not fit the
    // 1024 B mailbox (700 B of bones + 273 B of drivers + feet + header). So while ANY peer is
    // scrubbing, live receivers get results rather than drivers: visually exact, but no extrapolation
    // on loss. Acceptable because audio rides the transported sound funnel rather than anim notifies.
    bool replaying = false;
    // The sender's chat box is open with text in it. Rides a spare flag bit like `replaying`, so it
    // costs nothing and travels with every snapshot -- the receiver shows an animated "..." bubble
    // over their skater while it holds.
    uint8_t typing = 0;
    // FSkateboardingAnimParams::PushSpeedMultiplier. Pushing is a TAP -- tap faster and this rises --
    // and it is read by the ANIM BLUEPRINT off the character, not off the anim instance, so the driver
    // blob can never carry it. Without it a proxy animates every push at 1.0x while its transported
    // body accelerates at the real rate: the push looks disconnected from its own speed.
    float pushSpeed = 1.0f;
    // poseN is the TOTAL bone count of the sender's skeleton; poseFirst/poseCount say which SLICE
    // of it this frame carries. A whole 95-bone pose is 951 B against a 1024 B mailbox and never
    // fit -- it was silently dropped every frame (the `else { w.u8(0); }` below), so a scrubbing
    // player with a rig-carrying garment transmitted NO pose at all and observers saw them freeze
    // with only position and rotation tracking. Slicing spreads one skeleton over consecutive
    // frames; the receiver accumulates and only calls the pose usable once it has seen all of it.
    uint8_t poseN = 0;          // total bones in the sender's skeleton (0 = no pose this frame)
    uint8_t poseFirst = 0;      // index of the first bone in this frame's slice
    uint8_t poseCount = 0;      // bones carried this frame; poseRot/posePos hold them AT THEIR OWN
                                // indices, not packed from zero
    float   poseRot[kPoseMaxBones][4];
    float   posePos[kPoseMaxBones][3];
    // The physics overlay (kPovMaxBones above). povBone holds the SENDER'S bone index, which is not
    // portable on its own -- two merged skeletons agree on bone NAMES and on nothing else -- so the
    // receiver maps it through the same name fingerprint the pose lane already carries.
    uint8_t povN = 0;
    uint8_t povBone[kPovMaxBones] = {};
    float   povRot[kPovMaxBones][4] = {};
    float   povPos[kPovMaxBones][3] = {};
};

// ---- SNAPSHOT WIRE VERSION ------------------------------------------------------------------------
// The two bytes behind the magic. See the version rule at the top of replication.cpp: MAJOR is the
// fixed layout and a reader rejects a different one; MINOR counts APPENDED fields and a reader gates
// each on `minor >= N`, so peers one minor apart still see each other. Append and bump the minor
// wherever it is possible; bump the major only when a field has to move.
constexpr uint8_t kWireMajor = 1;
// 1.1: one flags byte appended (grace in 1.1.6-1.1.7; spare1, always 0, since 1.1.8) -- the first
//      field added under the version rule rather than a magic letter.
// 1.2: trickSerial + paSerial (1.1.8) -- the owner's SetTrick count and body-physics state; see
//      trick_pulse.h and pa_state.h.
constexpr uint8_t kWireMinor = 2;

// What a snapshot that would not parse actually was. Lets the reject path say who needs to update
// instead of leaving a peer silently invisible.
enum WirePeek : uint8_t {
    kWireOk         = 0,   // ours, and this build can read it
    kWireForeign    = 1,   // not a snapshot at all (or too short to be one)
    kWireOtherMagic = 2,   // an "OMP?" message with a different letter -- a pre-OMPZ build's snapshot
    kWireMajorSkew  = 3,   // ours, but a major this build cannot parse (major/minor are filled in)
};
WirePeek PeekWire(const uint8_t* data, int len, uint8_t* major, uint8_t* minor);

// Wire <-> State. Pack stamps the sender clock; Unpack validates (magic, version, finiteness, unit
// quat) -- P2P input is untrusted even when today's only sender is a friend.
// `poseWrote`, when given, reports how many bones of the pose this packet actually carried, so the
// caller can advance its own slice cursor. Callers with a big cap (the replay ring) take the whole
// skeleton in one go and simply pass null.
int  Pack(const State& s, uint64_t senderUs, uint8_t* out, int cap, int* poseWrote = nullptr);                 // -> bytes or 0
bool Unpack(const uint8_t* data, int len, State& out, uint64_t* senderUs);

// Interpolate two states at alpha t in [0,1]: positions and quats blend, whitelisted continuous anim
// floats blend, and every bool/enum/rotator/state byte steps from `a`. This is the EXACT math
// Stream::Sample renders live peers with, extracted so replay playback of a transferred state
// history (replaysync) looks identical to live replication. t <= 0 copies `a`.
void InterpStates(const State& a, const State& b, float t, State& out);

// ============================ COSMETICS ==============================================================
// A player's whole look: the game's FSkaterInstance reduced to what can legally travel. Item identity
// is a NAME (FName indices are per-process, so only the string can travel), plus the two small ints
// the game stores beside it. Categories are the game's own fixed slot keys.
//
// This rides its OWN packet type, NOT the 60 Hz snapshot: it is large (names) and changes almost
// never, and the snapshot is deliberately a self-contained per-packet truth we keep small. Sent on
// change, on a slow heartbeat, and when a new peer appears. Loss just delays it.
// One user-chosen colour on a garment. The key is the game's own colour-slot string; the value is a
// linear colour, which can exceed 1.0, so it travels as full floats rather than bytes.
struct CosmeticColor {
    char    key[24] = {};
    float   rgba[4] = {};
    uint8_t enabled = 0;
};
struct CosmeticItem {
    int32_t  cat     = 0;         // the game's category key within its map
    int32_t  variant = 0;         // UsedVariantId (on the PROFILE)
    uint8_t  inst    = 0;         // UsedInstanceId
    // 64, not 40: a real item name reached 39 characters -- 'CIT_UB_TEE_LiquidDeath_ExclusiveDeathTe'
    // -- which is exactly what a 40-byte field holds once its terminator is counted, so the name was
    // silently cut and could never resolve on the far side. The giveaway was the WEARER'S OWN client
    // failing to resolve an item it was itself wearing. The wire is length-prefixed, so a longer
    // field costs nothing for ordinary names; only a genuinely longer name sends more bytes.
    char     name[64] = {};       // the item's FName as a STRING; empty = slot cleared
    // ---- the attributes that live on the INVENTORY INSTANCE, not the profile. Without these a peer's
    // index selects the RECEIVER'S OWN instance, so their custom colours and variants silently render
    // as yours.
    int32_t  instVariant = -1;    // the instance's own VariantId (same mesh, different colourway)
    uint8_t  sockHeight  = 0;
    uint8_t  nColors     = 0;
    CosmeticColor colors[6];
};
struct CosmeticSet {
    uint8_t      stance = 0;
    char         skaterName[40] = {};
    // WHERE they are. The INTERNAL level name -- transport the name, resolve it locally; the receiver
    // turns it into the game's own pretty label. It rides this packet rather than a new one because
    // the set is memcmp'd for changes, so loading a different map republishes automatically, which is
    // exactly the event we want to hear about.
    char         mapName[40] = {};
    char         visualDef[64] = {};    // USkaterVisualsDefinition asset name (the base body)
    uint8_t      nChar = 0, nBoard = 0;
    CosmeticItem chr[24];               // clothing
    CosmeticItem brd[16];               // board parts (deck graphic, trucks, wheels, ...)
    // A cheap digest of the sender's installed cosmetic content (pak file names+sizes). Item NAMES are
    // identical across replacement mods -- the same "Deck_01" can be different art on each machine --
    // so names alone can never answer "do we see the same thing". Equal digests => identical content.
    uint64_t     modDigest = 0;
};
// The transports cap a message around 1 KB (the shm mailbox's slot size) and a full wardrobe does not
// fit: 24 clothing slots consume the whole packet and leave room for 2 of 16 board slots. So cosmetics
// travel as ONE PACKET PER SECTION (clothing / board), each repeating the tiny header, and the
// receiver merges by section. Two 0.1 Hz packets is nothing, and neither section can starve the other.
// Within a section the packer still fills what fits and reports what it wrote -- a dropped slot
// renders as that slot's default, never a corrupt packet.
// BOARD WEAR AND TEAR. Grinding and sliding scuff a board, and the game keeps that as two ratio maps
// per item instance keyed by contact part (the deck's nose, middle and tail each wear separately, and
// so does the griptape). It was never transported, so a peer's board rendered with whatever wear the
// LOCAL profile happened to carry when they were dressed -- everyone looked like your board, frozen at
// the moment you joined. Keyed by the item's CATEGORY, not by list position, so it survives a peer
// changing one part of their setup.
struct WearEntry {
    int32_t cat  = 0;      // which board item -- the same key CosmeticItem::cat uses
    uint8_t part = 0;      // contact part (the ratio map's key)
    uint8_t dirt = 0;      // ratio quantised to a byte: nobody can see finer than that on a deck
    uint8_t wear = 0;
};
struct WearSet {
    uint8_t   n = 0;
    WearEntry e[24];
};

// kCosWear rides the COSMETICS magic as a third section rather than claiming a magic letter of its
// own -- only Z is left in the namespace, and wear is not worth it. It is sent on its own cadence,
// because wear creeps constantly and must not drag a whole wardrobe across every time it moves.
enum CosmeticSection : uint8_t { kCosClothing = 0, kCosBoard = 1, kCosWear = 2 };
int  PackWear(const WearSet& w, uint8_t* out, int cap);
bool UnpackWear(const uint8_t* data, int len, WearSet& out);
int  PackCosmetics(const CosmeticSet& c, uint8_t section, uint8_t* out, int cap);
// `sectionOut` says which half arrived; the caller merges it into its view of that peer.
bool UnpackCosmetics(const uint8_t* data, int len, CosmeticSet& out, uint8_t* sectionOut);
// True if the buffer carries a cosmetics packet (checked before the snapshot path -- one transport,
// two message types, routed by magic).
bool IsCosmeticsPacket(const uint8_t* data, int len);

// ---- BODY FEEL SETTINGS: SessionTweaks' riding-body knobs, a short fixed vector of ints, OPAQUE to
// this layer. The tweaks module on the receiving machine re-runs its riding body on the sender's
// proxy with these values, so a peer moves on your screen the way they move on theirs. Rare and tiny
// (a few dozen bytes: on change, on a new peer, and a 10 s heartbeat), sent reliable.
struct BodyFeelSet {
    uint8_t ver = 0;                 // 0 = nothing received / nothing to send
    uint8_t n = 0;
    int16_t v[32] = {};
};
int  PackBodyFeel(const BodyFeelSet& b, uint8_t* out, int cap);
bool IsBodyFeelPacket(const uint8_t* data, int len);
bool UnpackBodyFeel(const uint8_t* data, int len, BodyFeelSet& out);

// ---- VOICE: Opus frames, unreliable and frequent (one 20 ms frame per packet while someone
// talks, ~90 bytes). magic, u16 sequence of the first frame, u8 count, then count x (u8 len, bytes).
// The sequence counts FRAMES, so a gap is exactly the number of frames lost.
enum { kVoiceMaxFrames = 4, kVoiceFrameMax = 400 };
int  PackVoice(uint16_t seq, const uint8_t* const* frames, const int* lens, int n, uint8_t* out, int cap);
bool IsVoicePacket(const uint8_t* data, int len);
// The frame count; `frames` point INTO `data`.
int  UnpackVoice(const uint8_t* data, int len, uint16_t* seqOut, const uint8_t** frames, int* lens, int maxFrames);

// ---- POSE HOLD: "keep my last transported skeleton". Sent while the sender's pose is a HELD one
// (sitting: quasi-static, so the skeleton itself goes out only when it moves or once a second),
// at 4 Hz, and as an explicit release on the way out. 6 bytes: magic, u8 hold, u16 ttl ms. Its own
// packet because both snapshot flag bytes are full; an older peer warns once and plays on.
int  PackPoseHold(bool hold, uint16_t ttlMs, uint8_t* out, int cap);
bool IsPoseHoldPacket(const uint8_t* data, int len);
bool UnpackPoseHold(const uint8_t* data, int len, bool* holdOut, uint16_t* ttlOut);

// ---- CHAT -----------------------------------------------------------------------------------------
// A third message type on the same transport, routed by magic like the other two. Sent RELIABLE: a
// dropped pose is invisible a sixtieth of a second later, a dropped sentence is just gone.
// The sender's NAME travels with the message rather than being looked up on arrival, because chat has
// to work in the seconds before a peer's cosmetics land -- and because a line should keep the name it
// was said under even if they rename afterwards.
constexpr int kChatTextLen = 160;      // one line of talk; longer is a paragraph, not a chat message
constexpr int kChatNameLen = 32;       // == the multiplayer name limit
struct ChatMsg {
    uint32_t id = 0;                   // sender-local, increasing -- dedupe, so a resend cannot echo
    char     name[kChatNameLen] = {};
    char     text[kChatTextLen] = {};
};
int  PackChat(const ChatMsg& m, uint8_t* out, int cap);
bool UnpackChat(const uint8_t* data, int len, ChatMsg& out);
bool IsChatPacket(const uint8_t* data, int len);

// ---- THE SKELETON FINGERPRINT ------------------------------------------------------------------
// A further message type on the same transport, routed by magic like the others, and sent on the
// cadence cosmetics are: once when a peer appears and again whenever our own skeleton changes. It is
// what makes a transported pose apply correctly to a player whose character is built differently
// from ours -- see game_syms.h SkeletonBoneHashes for why a NAME is the only handle that survives
// the trip. About 390 B, a handful of times per session; never per frame.
struct SkelPrint {
    uint8_t  n = 0;
    uint32_t hash[kPoseMaxBones] = {};
};
// The receiver's per-peer name cache: fills in what an interned packet omitted. One per peer,
// owned by the session; Resolve() runs right after Unpack, BEFORE the state is pushed anywhere, so
// everything downstream still sees complete states and nothing else changes.
struct AudioNameCache {
    struct Ent { uint8_t slot = 0; uint8_t used = 0; char cue[kAudioCueLen] = {};
                 uint8_t nParam = 0; char pname[kAudioMaxParams][kAudioParamLen] = {}; };
    Ent  ent[8];                       // slots are sender handles, not indices -- keyed, not addressed
    char trick[40] = {};               // the last full trick/grind names seen from this peer
    char grind[40] = {};
    void Resolve(State& s);            // fill omissions from the cache; learn from named entries
};
int  PackSkeleton(const SkelPrint& s, uint8_t* out, int cap);
bool UnpackSkeleton(const uint8_t* data, int len, SkelPrint& out);
bool IsSkeletonPacket(const uint8_t* data, int len);

// One remote sender's stream: snapshot ring + playback clock. Feed Push() from the transport,
// call Sample() whenever a consumer needs the pose; both take YOUR clock in microseconds.
class Stream {
public:
    struct Tuning {
        float baseDelayMs   = 17.0f;    // measured floor for an authoritative sim
        float gapFloorMult  = 1.6f;     // delay >= this x cadence. Sized for MARGIN: the clock's
                                        // sawtooth plus sampling-beat wander must never reach the
                                        // newest sample. At 1.25 the stream starves on ~30% of beats.
        float jitterAllow   = 1.0f;     // + measured net jitter (on: this transport is a real wire)
        float jitterCapMs   = 250.0f;
        float jitterPeakDecay = 60.0f;  // ms/s -- the floor rides the PEAK of recent jitter, bleeding out
                                        // slowly. An EMA cannot absorb bursts (one 233 ms spike per 120
                                        // packets averages to ~15 ms and the clock snaps every burst).
        float pollQuantumMs = 17.0f;    // receiver poll quantum, subtracted before believing jitter
        float errFilterRate = 4.0f;     // /s -- clock error EMA
        float catchupRate   = 6.0f;     // /s -- slew onto that estimate
        float rateCap       = 0.25f;    // clock never runs faster/slower than 1 +/- this
        float resyncMs      = 150.0f;   // beyond this the clock JUMPS
        float snapDistCm    = 500.0f;   // a teleport is not smeared across the gap
        float extrapMaxMs   = 100.0f;   // position-only projection bound
        float extrapMaxCm   = 300.0f;
        float delaySlewUpMsS   = 400.0f; // the DELAY itself must SLEW, never step: a +200 ms delay step
        float delaySlewDownMsS = 80.0f;  // yanks the target backwards through the resync threshold,
                                         // which renders as a backward snap.
    };
    struct Stats {
        float    delayMs = 0, leadMs = 0, netJitMs = 0, gapAvgMs = 0, alpha = 0, rateMin = 1, rateMax = 1;
        uint32_t pushed = 0, sampled = 0, starved = 0, resyncs = 0, stale = 0, extrap = 0;
    };

    Stream() = default;                          // default-constructible: sessions hold arrays of these
    explicit Stream(const Tuning& t) : tun_(t) {}
    void Reset();                                // reuse a slot for a NEW peer without reallocating
    void Push(const State& s, uint64_t senderUs, uint64_t localUs);
    bool Sample(uint64_t localUs, State& out);   // false until 2+ snapshots buffered
    // ---- audio one-shots, deliberately OUTSIDE the interpolation path (see State's audio block).
    // Push files each new event by the SENDER time it actually fired; this releases the ones the
    // playback clock has now reached, so a peer's pop lands with the animation that caused it rather
    // than ~a buffer-delay early. Call it right after Sample(). Returns how many were written.
    // Events the playhead has already run past by more than kAudioStaleMs are DROPPED, not played
    // late: after a stall the clock resyncs forward, and firing the whole backlog at once would be a
    // burst of machine-gun pops. Silence is the better failure.
    int  DrainAudio(AudioEvent* out, int cap);
    // ---- push-state transitions, outside the interpolation path for the SAME reason. Pushing in this
    // game is a TAP -- the faster you tap, the more pushes per second -- and the receiver's
    // `ASkaterCharacterBase::SetPushState` is EDGE-triggered (it stores the last state at +0x6e9 and
    // returns immediately when handed the same value again). At a 30-60 Hz publish rate a fast tap's
    // state transition can be shorter than the gap between two snapshots, so reading pushState out of
    // a SAMPLED State misses it completely and the proxy fires fewer pushes than the sender did while
    // its transported body accelerates at the full rate. The impulse itself is a UAnimNotify_Push
    // inside the push animation, so one animation IS one push -- a dropped edge is a missing push.
    // Filed as they ARRIVE, released when the playback clock reaches the moment they happened, so a
    // burst of tapping replays with its original spacing instead of collapsing into one frame.
    int  DrainPushStates(uint8_t* out, int cap);
    Stats stats() const { return st_; }
    uint64_t lastPushLocalUs() const { return lastArriveUs_; }

private:
    static const int kRing = 24;                 // 400 ms of 60 Hz -- MUST exceed jitterCapMs, or a
                                                 // fully-absorbed burst parks the playhead behind the ring
    struct Snap { State s; uint64_t t = 0; };
    // The audio one-shot queue. Small on purpose: one-shots are rare (a pop, a landing, a revert),
    // and a queue that could grow is a queue that can burst. Ids are remembered separately because
    // each event is REPEATED in the next few packets to survive loss -- the id ring is what turns
    // that redundancy back into exactly one playback.
    static const int kAudioQueue   = 16;
    static const int kAudioSeen    = 64;
    static const int kAudioStaleMs = 300;
    struct PendAudio { AudioEvent e; uint64_t t = 0; };
    PendAudio audioQ_[kAudioQueue];
    int       audioN_ = 0;
    uint16_t  seenId_[kAudioSeen] = {};
    uint8_t   seenOk_[kAudioSeen] = {};      // ids are u16 and wrap; a parallel "occupied" flag keeps
    int       seenHead_ = 0;                 // id 0 from being indistinguishable from an empty slot
    bool audioSeen(uint16_t id);
    void audioRemember(uint16_t id);
    void audioFile(const State& s, uint64_t senderUs);
    // The push queue is ORDER-SENSITIVE, unlike the audio one: these are state-machine transitions,
    // so replaying them out of order would drive the receiver's push machine through a sequence that
    // never happened. Kept sorted by sender time, drained from the front, never swap-removed.
    static const int kPushQueue   = 12;
    static const int kPushStaleMs = 250;
    struct PendPush { uint8_t state = 0; uint64_t t = 0; };
    PendPush pushQ_[kPushQueue];
    int      pushN_ = 0;
    int      lastPushedState_ = -1;          // -1 = nothing seen yet, so the first packet is not an edge
    void     pushFile(const State& s, uint64_t senderUs);
    Tuning   tun_;
    Stats    st_;
    Snap     ring_[kRing];
    int      head_ = -1, count_ = 0;
    uint64_t playUs_ = 0, lastLocalUs_ = 0, lastArriveUs_ = 0;
    double   errAvgUs_ = 0;
    float    gapAvgMs_ = 16.7f, floorMs_ = 21.0f, netJitMs_ = 0, jitPeakMs_ = 0, delayMs_ = 21.0f;
};

}} // namespace omp::repl
