// omp_wirecost -- what a lobby costs on the wire, measured with the REAL packer (repl::Pack).
//
// Packs the frames a player actually sends -- skating (drivers + the whole anim field table), and a transported
// SKELETON (what goes instead while a pose is held and moving: an emote, a carried prop, a bail's rag-doll, a sit
// that shifts) -- then works out, from the session's own send rules, what each player uploads and downloads in
// a lobby of N. The send rules copied here are session.cpp's: one packet per publish to EVERY other player
// (a full mesh, no server), 60 Hz with one peer, 45 with two, 30 with more; a skeleton too big for one packet
// is spread over consecutive publishes, never sent faster.
//
//   omp_wirecost            the table
//
// Bytes are PAYLOAD. Each packet also carries the network's own headers (UDP/IP and the EOS relay's framing),
// estimated below and shown separately so it is not mistaken for a measurement.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "omp_peers.h"
#include "replication/replication.h"
#include "replication/anim_fields.h"

using namespace omp::repl;

static const double kHeaderEst = 60.0;          // UDP/IPv4 28 B + the relay's framing: an ESTIMATE, not measured

static void Riding(State& s, unsigned seed) {
    s = State{};
    s.deckQuat[0] = 0.1f; s.deckQuat[1] = 0.2f; s.deckQuat[2] = 0.3f; s.deckQuat[3] = sqrtf(1.0f - 0.14f);
    s.deckPos[0] = -2775.3f; s.deckPos[1] = 13527.8f; s.deckPos[2] = -228.1f;
    s.bodyPos[0] = -2771.0f; s.bodyPos[1] = 13530.0f; s.bodyPos[2] = -140.0f;
    s.bodyPosOk = true; s.bodyRotOk = true; s.meshOk = true; s.relOk = true;
    s.feetOk = true; s.handOk = 1; s.onBoard = 1; s.grounded = 1; s.artOk = 2;
    int n = 0;
    for (int i = 0; i < AnimFieldCount(); i++) {                 // the whole field table, as gather.cpp fills it
        const AnimField& f = AnimFieldAt(i);
        if (n + f.size > (int)sizeof(s.anim)) break;
        for (int b = 0; b < f.size; b++) { seed = seed * 1664525u + 1013904223u; s.anim[n + b] = (uint8_t)(seed >> 24); }
        n += f.size;
    }
    s.animLen = (uint16_t)n;
}
static void Posed(State& s, int bones, unsigned seed) {
    Riding(s, seed);
    s.animLen = 0; s.feetOk = false; s.handOk = 0;           // what session.cpp drops while a skeleton ships
    s.poseN = (uint8_t)bones; s.poseFirst = 0; s.poseCount = (uint8_t)bones;
    for (int b = 0; b < bones; b++) {
        seed = seed * 1664525u + 1013904223u;
        const float a = (float)(seed >> 8) / 16777216.0f * 3.0f;
        s.poseRot[b][0] = sinf(a) * 0.5f; s.poseRot[b][1] = cosf(a) * 0.5f; s.poseRot[b][2] = 0.5f; s.poseRot[b][3] = 0.5f;
        s.posePos[b][0] = 10.0f * b; s.posePos[b][1] = -3.0f * b; s.posePos[b][2] = 95.0f + b;
    }
}
struct Frame { int bytes; int bonesPerPacket; };
static Frame Measure(const State& s) {
    uint8_t buf[1024];                                           // the wire's cap: session.cpp packs into 1024
    int wrote = 0;
    const int n = Pack(s, 123456789ull, buf, sizeof(buf), &wrote);
    return { n, wrote };
}
static double Hz(int peers) { return peers <= 1 ? 60.0 : peers == 2 ? 45.0 : 30.0; }

int main() {
    State s;
    Riding(s, 7);                     const Frame ride = Measure(s);
    printf("omp_wirecost: the anim field table is %d B raw (%d fields)\n", s.animLen, AnimFieldCount());
    Posed(s, 70, 9);                  const Frame p70 = Measure(s);
    Posed(s, 95, 9);                  const Frame p95 = Measure(s);
    printf("\nONE PACKET (payload, the real packer)\n");
    printf("  skating (drivers + anim)          %4d B\n", ride.bytes);
    printf("  skeleton, 70-bone body            %4d B  (%d bones in it: one packet per refresh)\n", p70.bytes, p70.bonesPerPacket);
    printf("  skeleton, 95 bones (rigged outfit) %4d B  (%d bones in it: %d packets per refresh)\n", p95.bytes, p95.bonesPerPacket,
           p95.bonesPerPacket > 0 ? (95 + p95.bonesPerPacket - 1) / p95.bonesPerPacket : 0);
    if (ride.bytes <= 0 || p70.bytes <= 0 || p95.bytes <= 0) { printf("a frame did not pack\nWIRE COST FAIL\n"); return 1; }

    // what ONE player sends each second to each other player, by what they are doing. A held pose that is NOT
    // moving (sitting still) ships its skeleton once a second and drivers in between.
    printf("\nPER PLAYER, UPLOAD AND DOWNLOAD (every other player sends to you what you send to them)\n");
    printf("  %-8s %-44s %11s %11s   %s\n", "lobby", "what everyone is doing", "upload", "download", "(+ headers, est.)");
    const int lobbies[4] = { 4, 10, 16, OMP_MAX_PEERS };
    for (int li = 0; li < (int)(sizeof(lobbies) / sizeof(lobbies[0])); li++) {
        const int N = lobbies[li], peers = N - 1;
        const double hz = Hz(peers);
        struct Mix { const char* what; double skating, sitting, moving; } mixes[4] = {
            { "all skating",                                 1.0, 0.0, 0.0 },
            { "a busy lobby: 40% sitting, 20% bail/emote",   0.4, 0.4, 0.2 },
            { "half of them bailing or emoting",             0.5, 0.0, 0.5 },
            { "everyone bailing or emoting at once",         0.0, 0.0, 1.0 },
        };
        for (const Mix& m : mixes) {
            // bytes/s one player sends to ONE peer, averaged over what the lobby is doing
            const double sitPerPkt = (ride.bytes * (hz - 1.0) + p70.bytes) / hz;      // skeleton once a second, drivers between
            const double perPkt = m.skating * ride.bytes + m.sitting * sitPerPkt + m.moving * p70.bytes;
            const double up = perPkt * hz * peers, dn = up;                            // symmetric: same mix on both sides
            const double hdr = kHeaderEst * hz * peers;
            printf("  %2d (%2.0f Hz) %-44s %6.0f KB/s %6.0f KB/s   (+%3.0f KB/s)  = %4.2f Mbit/s each way\n", N, hz, m.what,
                   up / 1024.0, dn / 1024.0, hdr / 1024.0, (up + hdr) * 8.0 / 1.0e6);
        }
    }
    printf("\nWIRE COST MEASURED\n");
    return 0;
}
