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
// SessionOpenMP -- the RELAY wire, shared by the standalone server (tools/relay) and the client
// backend (src/transport/relay_transport.cpp). One file so the two can never disagree.
//
// WHAT THE RELAY IS: a dumb forwarder. It moves opaque payloads between clients in a room and tells
// each client who else is there. It never parses a game packet, never knows what a snapshot is, and
// holds no game state -- reliability, sequencing, dedupe and the peer tokens all stay END TO END
// between clients, exactly as they do on the direct wire. That is what keeps a server small enough
// to leave running unattended.
//
// WHY A RELAY AT ALL: every client DIALS OUT to it, which is what opens a return path through a home
// router. Direct UDP needs the host to forward a port and cannot connect two joiners behind
// different NATs at all; a relay needs nobody to configure anything.
//
// WHAT IT IS NOT: encryption. The operator of a relay sees every byte. It removes peer-to-peer IP
// exposure and adds an operator in its place, which is a real trade and is said out loud in
// Posture() rather than implied away.
#pragma once
#include <cstdint>

namespace omp { namespace relay {

// "OMPL" -- the LINK wire, distinct from the game wire's magic ("OMPR" and its lineage) and from the
// direct-UDP wire's ("OMPU"). Three different conversations; a packet from the wrong one must be
// rejected on sight rather than misparsed.
static const uint32_t kMagic = 0x4C504D4Fu;
static const uint8_t  kVer   = 1;

static const int kRoomCodeLen = 8;      // "ABC123" and room for growth; NUL-padded, not terminated
static const int kNameLen     = 32;     // a display name, and a peer identity string
static const int kMapLen      = 32;
static const int kMaxSlots    = 16;     // per room; matches kPeers in every other backend
static const int kMaxPayload  = 1100;   // an OMPU-framed game datagram, with room for its own header
static const int kListRooms   = 8;      // rooms per LIST reply

enum : uint8_t {
    RT_HELLO   = 1,   // client->relay  + code[8] + name[32] + map[32] + id[32]: "put me in this room"
    RT_WELCOME = 2,   // relay->client  + slot(u8) + token(u32): "you are slot N, stamp this token"
    RT_FULL    = 3,   // relay->client  the room is full, or the server is
    RT_ROSTER  = 4,   // relay->client  + count(u8) + N x { slot(u8), id[32] }
    RT_FWD     = 5,   // both ways      + peer(u8) + payload. To the relay `peer` is the DESTINATION
                      //                 slot; from the relay it is the SOURCE slot.
    RT_BYE     = 6,   // client->relay  a clean exit, so the room does not wait out a timeout
    RT_PING    = 7,   // client->relay  keepalive: holds the NAT mapping open and proves liveness
    RT_LISTREQ = 8,   // client->relay  "what rooms are here" -- SEE THE PADDING RULE BELOW
    RT_LIST    = 9,   // relay->client  + count(u8) + N x RoomInfo
};

// THE PADDING RULE, and the reason it exists.
// RT_LIST is the only reply larger than its request, which is exactly the shape of a UDP
// amplification attack: an attacker spoofs a victim's source address, sends a tiny request, and the
// server sprays a much larger reply at the victim. A public listing is a fine feature and a terrible
// reflector, so the request must be AT LEAST as big as the largest possible reply. Then the
// amplification factor is <= 1 and the server is useless for that purpose -- which is cheaper and
// more reliable than any rate limit, and needs no state at all.
// A relay MUST drop a short RT_LISTREQ. This is not an optimisation to skip.
struct RoomInfo {
    char    code[kRoomCodeLen];
    char    host[kNameLen];
    char    map[kMapLen];
    uint8_t players;
    uint8_t pad[3];
};
static const int kListReplyMax = 1 + kListRooms * (int)sizeof(RoomInfo);
static const int kListReqMin   = kListReplyMax;      // the rule above, as a number

#pragma pack(push, 1)
struct Hdr {
    uint32_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint8_t  slot;     // the sender's own slot (client->relay), or unused. 0xFF = "not in a room yet"
    uint8_t  pad;
    uint32_t token;    // issued by the relay in RT_WELCOME; 0 before that
};
#pragma pack(pop)
static const int kHdrLen = (int)sizeof(Hdr);         // 12
static const uint8_t kNoSlot = 0xFF;

static const int kDefaultPort   = 47800;
static const int kPingMs        = 1000;
static const int kClientTimeout = 12000;   // silence past this and the relay drops a client
static const int kHelloResendMs = 300;
static const int kHelloGiveUpMs = 15000;

} }  // namespace omp::relay
