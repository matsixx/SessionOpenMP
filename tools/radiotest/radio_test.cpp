// omp_radiotest -- the radio's message (src/tweaks/radio_wire.h), with no game behind it.
// It comes from another player's machine: it is believed only if every part of it is sane, and dropped whole if not.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits>
#include "radio_wire.h"
#include "replication/replication.h"      // the stream lane ("OMPr")

using namespace radiowire;
static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what) { if (ok) { g_pass++; return; } g_fail++; printf("  FAIL  %s\n", what); }

static State Held() {
    State s; memset(&s, 0, sizeof(s));
    s.state = ST_HELD; s.station = 2; s.song = 17; s.songPos = 93.5f; s.relValid = 1; s.side = 1;
    s.quat[3] = 1.0f;
    s.relLoc[0] = 4.0f; s.relLoc[1] = -21.0f; s.relLoc[2] = 12.5f; s.relRot[0] = -7.0f; s.relRot[1] = 88.0f; s.relRot[2] = 179.0f;
    return s;
}
static State Placed() {
    State s; memset(&s, 0, sizeof(s));
    s.state = ST_PLACED; s.station = 0; s.song = 3; s.songPos = 12.0f;
    s.pos[0] = -2775.0f; s.pos[1] = 13527.0f; s.pos[2] = -228.0f;
    s.quat[2] = 0.7071068f; s.quat[3] = 0.7071068f;
    return s;
}
int main() {
    uint8_t buf[128];
    // ---- what is sent is what arrives
    for (int k = 0; k < 2; k++) {
        const State a = k ? Placed() : Held();
        const int n = Encode(a, buf, sizeof(buf));
        State b;
        Check(n == kSize, "a message is exactly its size");
        Check(Decode(buf, n, &b), "a good message is believed");
        Check(b.state == a.state && b.station == a.station && b.song == a.song && b.songPos == a.songPos && b.relValid == a.relValid && b.side == a.side, "...the state, the song and how far in");
        bool same = true;
        for (int i = 0; i < 3; i++) same = same && b.pos[i] == a.pos[i] && b.relLoc[i] == a.relLoc[i] && b.relRot[i] == a.relRot[i];
        for (int i = 0; i < 4; i++) same = same && fabsf(b.quat[i] - a.quat[i]) < 1e-5f;
        Check(same, "...where it is and how it is turned, all three of the roll too");
    }
    Check(Encode(Held(), buf, kSize - 1) == 0, "too small a buffer is refused, not overrun");
    // ---- what is NOT believed
    State out;
    int n = Encode(Placed(), buf, sizeof(buf));
    Check(!Decode(buf, n - 1, &out) && !Decode(buf, n + 1, &out) && !Decode(buf, 0, &out) && !Decode(nullptr, n, &out), "a wrong length, or nothing");
    { uint8_t b2[128]; memcpy(b2, buf, n); b2[0] = 2;  Check(!Decode(b2, n, &out), "another version"); }
    { uint8_t b2[128]; memcpy(b2, buf, n); b2[1] = 3;  Check(!Decode(b2, n, &out), "a state there is not"); }
    { uint8_t b2[128]; memcpy(b2, buf, n); b2[2] = 3;  Check(!Decode(b2, n, &out), "a station there is not"); }
    auto poison = [&](int off, float v, const char* what) { uint8_t b2[128]; memcpy(b2, buf, n); memcpy(b2 + off, &v, 4); Check(!Decode(b2, n, &out), what); };
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    poison(8, nan, "a song position that is not a number");
    poison(8, -1.0f, "...or before the start");
    poison(8, 1.0e9f, "...or a day long");
    poison(12, nan, "a place that is not a number");
    poison(16, inf, "...or infinitely far");
    poison(20, 5.0e8f, "...or off the edge of any world");
    poison(24, nan, "a rotation that is not a number");
    poison(40, 1.0e6f, "a place under the arm a kilometre from the arm");
    poison(56, nan, "a turn under the arm that is not a number");
    { uint8_t b2[128]; memcpy(b2, buf, n); const float z[4] = { 0, 0, 0, 0 }; memcpy(b2 + 24, z, 16); Check(!Decode(b2, n, &out), "a PLACED radio turned by nothing at all (a zero quaternion)"); }
    { int m = Encode(Held(), buf, sizeof(buf)); uint8_t b2[128]; memcpy(b2, buf, m); const float z[4] = { 0, 0, 0, 0 }; memcpy(b2 + 24, z, 16);
      Check(Decode(b2, m, &out), "...but a HELD one has no use for that rotation, and is believed without it"); }
    { n = Encode(Placed(), buf, sizeof(buf)); uint8_t b2[128]; memcpy(b2, buf, n); const float q[4] = { 0.0f, 0.0f, 0.68f, 0.68f }; memcpy(b2 + 24, q, 16);
      Check(Decode(b2, n, &out) && fabsf(out.quat[2] * out.quat[2] + out.quat[3] * out.quat[3] - 1.0f) < 1e-4f, "a rotation a little off length is made a rotation"); }
    // ---- noise: never believed into something unsafe, never a crash
    unsigned rng = 0xC0FFEEu; int believed = 0;
    for (int t = 0; t < 200000; t++) {
        uint8_t b2[kSize];
        for (int i = 0; i < kSize; i++) { rng = rng * 1664525u + 1013904223u; b2[i] = (uint8_t)(rng >> 24); }
        if (t & 1) {                                   // ...and the dangerous kind: a GOOD message with a few bytes turned
            Encode((t & 2) ? Placed() : Held(), b2, kSize);
            const int flips = 1 + (int)((rng >> 8) % 3u);
            for (int f = 0; f < flips; f++) { rng = rng * 1664525u + 1013904223u; b2[(rng >> 16) % (unsigned)kSize] = (uint8_t)(rng >> 24); }
        }
        State s;
        if (!Decode(b2, kSize, &s)) continue;
        believed++;
        bool sane = s.state <= ST_PLACED && s.station <= 2 && s.songPos >= 0.0f && s.songPos < 7200.0f;
        for (int i = 0; i < 3; i++) sane = sane && fabsf(s.pos[i]) < 1.0e7f && fabsf(s.relLoc[i]) < 300.0f && fabsf(s.relRot[i]) < 720.0f;
        if (!sane) { Check(false, "noise was believed into something insane"); break; }
    }
    printf("omp_radiotest: %d of 200000 noise messages were believed (each one sane)\n", believed);

    // ---- STREAMING: what a radio plays travels in byte 5 -- 0 in the builds before it, which still decode --
    //      and the listener's ask is its own 4-byte message that neither decoder can mistake for the other
    {
        State a = Held(); a.source = SRC_STREAM;
        int m = Encode(a, buf, sizeof(buf)); State b;
        Check(Decode(buf, m, &b) && b.source == SRC_STREAM, "stream: the source travels");
        a.source = SRC_STATION; m = Encode(a, buf, sizeof(buf));
        Check(Decode(buf, m, &b) && b.source == SRC_STATION && buf[5] == 0, "stream: ...and a station radio sends 0 there, as every earlier build did");
        { uint8_t b2[128]; memcpy(b2, buf, m); b2[5] = 2; Check(!Decode(b2, m, &b), "stream: a source there is not is refused"); }
        uint8_t w[8]; bool want = false;
        const int wn = EncodeWant(true, w, sizeof(w));
        Check(wn == kWantSize && DecodeWant(w, wn, &want) && want, "ask: 'send me your stream' round-trips");
        EncodeWant(false, w, sizeof(w));
        Check(DecodeWant(w, kWantSize, &want) && !want, "ask: ...and 'stop'");
        Check(!Decode(w, kWantSize, &b), "ask: the state decoder does not take an ask for a radio");
        Check(!DecodeWant(buf, m, &want), "ask: ...and the ask decoder does not take a radio for an ask");
        { uint8_t w2[8]; memcpy(w2, w, 4); w2[2] = 7; Check(!DecodeWant(w2, 4, &want), "ask: a stranger's odd byte is refused"); }
        Check(!DecodeWant(w, 3, &want) && !DecodeWant(w, 5, &want) && !DecodeWant(nullptr, 4, &want), "ask: a wrong length, or nothing");
    }
    // ---- THE STREAM LANE ("OMPr", replication.cpp): the voice lane's layout under its own tag
    {
        using namespace omp::repl;
        uint8_t f0[120], f1[200], f2[255];
        for (int i = 0; i < 255; i++) { if (i < 120) f0[i] = (uint8_t)i; if (i < 200) f1[i] = (uint8_t)(i * 3); f2[i] = (uint8_t)(255 - i); }
        const uint8_t* fp[3] = { f0, f1, f2 }; const int fl[3] = { 120, 200, 255 };
        uint8_t pkt[900];
        const int n = PackRadio(4242, fp, fl, 3, pkt, sizeof(pkt));
        const uint8_t* got[kRadioMaxFrames]; int gl[kRadioMaxFrames]; uint16_t seq = 0;
        Check(n == 4 + 2 + 1 + 3 + 120 + 200 + 255, "lane: three frames pack to their exact size");
        Check(IsRadioPacket(pkt, n) && !IsVoicePacket(pkt, n), "lane: it is a radio packet, and NOT a voice packet");
        const int k = UnpackRadio(pkt, n, &seq, got, gl, kRadioMaxFrames);
        Check(k == 3 && seq == 4242 && gl[0] == 120 && gl[1] == 200 && gl[2] == 255 && !memcmp(got[1], f1, 200) && !memcmp(got[2], f2, 255), "lane: ...and unpack to the same frames");
        Check(UnpackRadio(pkt, n - 1, &seq, got, gl, kRadioMaxFrames) == 0 && UnpackRadio(pkt, n + 1, &seq, got, gl, kRadioMaxFrames) == 0, "lane: a short or padded packet is refused whole");
        const int big[1] = { 256 };
        Check(PackRadio(1, fp, big, 1, pkt + 0, sizeof(pkt)) == 0, "lane: a frame too long for its length byte is refused");
        uint8_t v[400]; const int vn = PackVoice(1, fp, fl, 1, v, sizeof(v));
        Check(vn > 0 && !IsRadioPacket(v, vn) && UnpackRadio(v, vn, &seq, got, gl, kRadioMaxFrames) == 0, "lane: a voice packet is never taken for a stream");
        // noise: a GOOD packet with bytes turned -- whatever is believed has every frame inside the packet
        const int n2 = PackRadio(4242, fp, fl, 3, pkt, sizeof(pkt));
        unsigned rng2 = 0xBADC0DEu; int okN = 0; bool bad = false;
        for (int t = 0; t < 200000 && !bad; t++) {
            uint8_t z[900]; memcpy(z, pkt, n2);
            int zl = n2;
            rng2 = rng2 * 1664525u + 1013904223u;
            if ((rng2 >> 28) == 0) zl = 7 + (int)((rng2 >> 8) % (unsigned)(n2 - 7));        // sometimes cut short too
            for (int f = 0; f < 3; f++) { rng2 = rng2 * 1664525u + 1013904223u; z[4 + (rng2 >> 8) % (unsigned)(zl - 4)] = (uint8_t)(rng2 >> 24); }
            const int c = UnpackRadio(z, zl, &seq, got, gl, kRadioMaxFrames);
            if (c > 0) {
                okN++;
                int sum = 7;
                for (int i = 0; i < c; i++) { sum += 1 + gl[i]; if (got[i] < z || got[i] + gl[i] > z + zl || gl[i] <= 0) bad = true; }
                if (sum != zl) bad = true;
            }
        }
        Check(!bad, "lane: noise never unpacks to a frame outside its packet, or frames that do not add up to it");
        printf("omp_radiotest: %d of 200000 mangled stream packets unpacked (every frame inside its packet)\n", okN);
    }
    printf("omp_radiotest: %d checks passed, %d failed\n%s\n", g_pass, g_fail, g_fail ? "RADIO TEST FAIL" : "RADIO TEST PASS");
    return g_fail ? 1 : 0;
}
