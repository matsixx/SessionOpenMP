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
    uint16_t animLen = 0; uint8_t anim[320] = {};

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
    // FSkateboardingAnimParams::PushSpeedMultiplier. Pushing is a TAP -- tap faster and this rises --
    // and it is read by the ANIM BLUEPRINT off the character, not off the anim instance, so the driver
    // blob can never carry it. Without it a proxy animates every push at 1.0x while its transported
    // body accelerates at the real rate: the push looks disconnected from its own speed.
    float pushSpeed = 1.0f;
    uint8_t poseN = 0;
    float   poseRot[kPoseMaxBones][4];
    float   posePos[kPoseMaxBones][3];
};

// Wire <-> State. Pack stamps the sender clock; Unpack validates (magic, finiteness, unit quat) --
// P2P input is untrusted even when today's only sender is a friend.
int  Pack(const State& s, uint64_t senderUs, uint8_t* out, int cap);                 // -> bytes or 0
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
    char     name[40] = {};       // the item's FName as a STRING; empty = slot cleared
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
enum CosmeticSection : uint8_t { kCosClothing = 0, kCosBoard = 1 };
int  PackCosmetics(const CosmeticSet& c, uint8_t section, uint8_t* out, int cap);
// `sectionOut` says which half arrived; the caller merges it into its view of that peer.
bool UnpackCosmetics(const uint8_t* data, int len, CosmeticSet& out, uint8_t* sectionOut);
// True if the buffer carries a cosmetics packet (checked before the snapshot path -- one transport,
// two message types, routed by magic).
bool IsCosmeticsPacket(const uint8_t* data, int len);

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
