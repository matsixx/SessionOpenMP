// SessionTweaks -- the radio's message, as it crosses the wire (OpenMP's mod channel "sessiontweaks.radio").
// Pure: no game, no engine -- so it is tested by itself (tools/radiotest).
//
// ONE MESSAGE, the owner's whole radio: every player owns exactly one, and only its owner ever speaks for it, so
// there is no authority to elect and nothing to merge. Sent reliably when it changes, to a newcomer when they
// appear, and as an unreliable heartbeat in between (a song's clock drifts; the heartbeat is what corrects it).
//
// IT IS A STRANGER'S BYTES. Everything is checked before it is believed: the length, the version, every float
// finite and in range, the rotation a rotation, the station one of ours. A message that fails is dropped whole.
//
//   0 version u8 | 1 state u8 | 2 station u8 | 3 relValid u8 | 4 side u8 | 5 source u8 | 6 song u16 | 8 songPos f32
//  12 pos f32 x3 | 24 quat f32 x4 | 40 relLoc f32 x3 | 52 relRot f32 x3 | = 64 bytes
// `source` (byte 5, 0 in the first builds that sent this) says what the radio plays: 0 the game's stations, 1 an
// app streamed from its owner's PC -- heard only by asking for it:
//
// THE ASK, the other message: 4 bytes { version, kWantKind, want 0/1, 0 }, sent by a LISTENER to a radio's owner
// while their radio is playing that owner's stream (and not muted), renewed every few seconds, and a 0 when it
// stops. The owner sends its stream to exactly the players who asked. A build that knows only the 64-byte message
// drops it on the length.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace radiowire {

enum { kVersion = 1, kSize = 64 };
enum { ST_NONE = 0, ST_HELD = 1, ST_PLACED = 2 };
enum { SRC_STATION = 0, SRC_STREAM = 1 };
enum { kWantSize = 4, kWantKind = 0xA7 };

struct State {
    uint8_t  state;          // ST_*
    uint8_t  station;        // 0..2
    uint16_t song;           // index in the station's list (the receiver wraps it to its own count)
    float    songPos;        // seconds into the song when this was sent
    uint8_t  relValid;       // HELD: rel* are set (it is hung from the chest bone already)
    uint8_t  side;           // HELD: the arm it is under (0 left, 1 right) -- for the log only
    uint8_t  source;         // SRC_*: the game's stations, or a stream from the owner's PC
    float    pos[3];         // PLACED: the actor's origin, world
    float    quat[4];        // PLACED: its rotation, world
    float    relLoc[3];      // HELD: the actor's place relative to the owner's chest bone
    float    relRot[3];      // HELD: ...and its turn there (pitch, yaw, roll -- degrees, as the engine keeps them)
};

inline bool Finite(float v, float lim) { return v == v && v > -lim && v < lim; }

inline int Encode(const State& s, uint8_t* out, int cap) {
    if (!out || cap < kSize) return 0;
    memset(out, 0, kSize);
    out[0] = (uint8_t)kVersion; out[1] = s.state; out[2] = s.station; out[3] = s.relValid ? 1 : 0; out[4] = s.side ? 1 : 0; out[5] = s.source;
    memcpy(out + 6, &s.song, 2);
    memcpy(out + 8, &s.songPos, 4);
    memcpy(out + 12, s.pos, 12);
    memcpy(out + 24, s.quat, 16);
    memcpy(out + 40, s.relLoc, 12);
    memcpy(out + 52, s.relRot, 12);
    return kSize;
}
inline bool Decode(const uint8_t* in, int len, State* s) {
    if (!in || !s || len != kSize || in[0] != (uint8_t)kVersion) return false;
    memset(s, 0, sizeof(*s));
    s->state = in[1]; s->station = in[2]; s->relValid = in[3] ? 1 : 0; s->side = in[4] ? 1 : 0; s->source = in[5];
    memcpy(&s->song, in + 6, 2);
    memcpy(&s->songPos, in + 8, 4);
    memcpy(s->pos, in + 12, 12);
    memcpy(s->quat, in + 24, 16);
    memcpy(s->relLoc, in + 40, 12);
    memcpy(s->relRot, in + 52, 12);
    if (s->state > ST_PLACED || s->station > 2 || s->song > 2000 || s->source > SRC_STREAM) return false;
    if (!Finite(s->songPos, 7200.0f) || s->songPos < 0.0f) return false;
    for (int i = 0; i < 3; i++) if (!Finite(s->pos[i], 1.0e7f) || !Finite(s->relLoc[i], 300.0f) || !Finite(s->relRot[i], 720.0f)) return false;
    float n2 = 0.0f;
    for (int i = 0; i < 4; i++) { if (!Finite(s->quat[i], 1.5f)) return false; n2 += s->quat[i] * s->quat[i]; }
    if (s->state == ST_PLACED) {                     // a placed radio is turned by this: it has to be a rotation
        if (!(n2 > 0.8f && n2 < 1.2f)) return false;
        const float k = 1.0f / sqrtf(n2);
        for (int i = 0; i < 4; i++) s->quat[i] *= k;
    }
    return true;
}

inline int EncodeWant(bool want, uint8_t* out, int cap) {
    if (!out || cap < kWantSize) return 0;
    out[0] = (uint8_t)kVersion; out[1] = (uint8_t)kWantKind; out[2] = want ? 1 : 0; out[3] = 0;
    return kWantSize;
}
inline bool DecodeWant(const uint8_t* in, int len, bool* want) {
    if (!in || len != kWantSize || in[0] != (uint8_t)kVersion || in[1] != (uint8_t)kWantKind || in[2] > 1 || in[3] != 0) return false;
    if (want) *want = in[2] != 0;
    return true;
}

} // namespace radiowire
