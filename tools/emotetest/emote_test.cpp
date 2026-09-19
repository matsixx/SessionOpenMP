// omp_emotetest -- the emote solver, judged on a made-up skeleton with no game behind it.
//
// Nothing here can say whether a gesture LOOKS good; that takes a screen. What it can say is whether the
// geometry is what the code claims: that a palm faces where it was told to, that a fist closes toward the
// palm and its thumb lies ACROSS it, that an arm reaches its target with every bone the length it started,
// that the board stays in the hand that moves it, that a board tap's low end really lands on the ground,
// that a dance leaves the feet where the game put them. Those are exactly the mistakes that cannot be seen
// from the source and cost a field round each.
//
// The skeleton is the game's own by NAME and ORDER (a 3ds Max biped: AMXX_*, Finger0 the thumb .. Finger4
// the little finger, the two hands' children listed in different orders, as the real rig has them; and the
// board rig, SKXX_SkateSkel_*, which hangs off the ROOT and not off a hand -- the fact the first field
// round taught), built standing with its arms at its sides, FACING +Y -- not +X -- so anything that quietly
// assumed the component's axes are the body's fails here. How the game's carry animation actually holds
// the board is NOT known, so every board check runs on two grips: hanging from the front truck, and
// carried flat at the side.
//
// emote.cpp is compiled INTO this file, so its anonymous namespace is in reach; what it calls out to is
// stubbed below.
#include "../../src/tweaks/emote.cpp"

#include <vector>
#include <string>
#include <stdarg.h>

// ------------------------------------------------------------------ the world outside emote.cpp
static float g_camFwd[3] = { 0.0f, 1.0f, 0.0f };
static bool  g_stubInHand = false;
void  TwkLog(const char*, ...) {}
int   TwkIniIntQuiet(const char*, const char*, int def) { return def; }
int   Twk_IsProxy(void*) { return 0; }
void  Twk_SetPoseHold(bool) {}
void* CatchTweaks_Skater() { return nullptr; }
void* FootPlace_AnimInstance() { return nullptr; }
bool  GrindPop_FNameToString(const void*, char*, int) { return false; }
bool  CameraHeight_ViewForward(float out[3]) { out[0] = g_camFwd[0]; out[1] = g_camFwd[1]; out[2] = g_camFwd[2]; return true; }
bool  Sit_PoseHeld() { return false; }
bool  Sit_EditorOpen() { return false; }
bool  Sit_BoardInHand(void*) { return g_stubInHand; }
bool  Sit_BoardThrow(void*, const float*, const float*) { return true; }
void  Sit_BoardKick(const float*, const float*) {}
bool  Sit_BoardOut() { return false; }
bool  Sit_BoardWhere(float*) { return false; }
void  Sit_BoardBack(const char*) {}
static int g_soundsPlayed = 0;
bool  Sit_TraceSurface(void*, const float*, const float*, float*, int* surface) { if (surface) *surface = 3; return true; }
void* CatchSound_SpawnAttached(void*, void*, float, float) { g_soundsPlayed++; return nullptr; }
uint8_t* TwkScanExe(const char*) { return nullptr; }
static float g_stubRT = -1.0f;        // what the engine says the right trigger is at; < 0 = it cannot be asked
bool  CatchTweaks_RightTrigger(float* out) { if (out) *out = g_stubRT < 0.0f ? 0.0f : g_stubRT; return g_stubRT >= 0.0f; }
static int g_stubSound = 0;          // what CatchSound_FindSound hands back (a loaded cue), or 0 for none
void* CatchSound_FindSound(const char*) { return g_stubSound ? (void*)&g_stubSound : nullptr; }
void  TwkIniStr(const char*, const char*, char* out, size_t cap, const char* def) { snprintf(out, cap, "%s", def ? def : ""); }
void  SitUI_Track(SitObjRef* r, void* o) { if (r) { r->obj = o; r->index = 0; r->serial = 0; r->cls = nullptr; } }
bool  SitUI_Alive(const SitObjRef* r) { return r && r->obj; }

// ------------------------------------------------------------------ the made-up body
static const V3 kF = { 0.0f, 1.0f, 0.0f }, kU = { 0.0f, 0.0f, 1.0f }, kR = { -1.0f, 0.0f, 0.0f };   // r = u x f
static V3 at(float f, float r, float u) { return add(add(mul(kF, f), mul(kR, r)), mul(kU, u)); }

struct B { std::string name; int parent; V3 p; };
static std::vector<B> g_body;
static int bone(const char* name, int parent, V3 p) { g_body.push_back({ name, parent, p }); return (int)g_body.size() - 1; }

static void Finger(const char* side, int hand, int digit, V3 knuckle, V3 dir, float l1, float l2) {
    char n[64];
    snprintf(n, sizeof(n), "amxx_%s_finger%d", side, digit);
    const int j0 = bone(n, hand, knuckle);
    snprintf(n, sizeof(n), "amxx_%s_finger%d1", side, digit);
    const int j1 = bone(n, j0, add(knuckle, mul(dir, l1)));
    snprintf(n, sizeof(n), "amxx_%s_finger%d2", side, digit);
    bone(n, j1, add(knuckle, mul(dir, l1 + l2)));
}
enum { GRIP_HANG = 0, GRIP_FLAT = 1 };
static Q4 g_boardRot = { 0.0f, 0.0f, 0.0f, 1.0f };      // reference pose -> as carried, for the board's bones
static V3 g_trueTop = { 0.0f, 0.0f, 1.0f };             // which way the deck's top REALLY faces, as built
// The real rig's thumb is LONGER than anyone's guess (the field: an aimed thumb ran right past the little
// finger), so the thumb checks run on more than one length.
static float g_thumbLen = 1.0f;
// boardHand: -1 the board is not being carried (it lies ahead on the ground), 0 / 1 the hand it is in.
static void BuildBody(int boardHand, int grip) {
    g_body.clear();
    const int master = bone("amxx_master", -1, at(0, 0, 0));
    const int root = bone("amxx_root", master, at(0, 0, 0));
    const int pelvis = bone("amxx_pelvis", root, at(0, 0, 95));
    const int s1 = bone("amxx_spine1", pelvis, at(0, 0, 105));
    const int s2 = bone("amxx_spine2", s1, at(0, 0, 118));
    const int s3 = bone("amxx_spine3", s2, at(0, 0, 133));
    const int neck = bone("amxx_neck", s3, at(0, 0, 151));
    bone("amxx_head", neck, at(1, 0, 160));
    V3 wrist[2];
    for (int sd = 0; sd < 2; sd++) {
        const char* S = sd ? "r" : "l"; const float sg = sd ? 1.0f : -1.0f;
        char n[64];
        // THE FIELD'S NUMBERS (the tap's own log line): this skater's shoulder is 146 cm up and the arm 56 cm to
        // the wrist, so a straight arm ends at about 90 -- and the board is 82.
        snprintf(n, sizeof(n), "amxx_%s_clavicle", S); const int cl = bone(n, s3, at(0, sg * 4, 146));
        snprintf(n, sizeof(n), "amxx_%s_upperarm", S); const int ua = bone(n, cl, at(0, sg * 18, 146));
        snprintf(n, sizeof(n), "amxx_%s_forearm", S);  const int fa = bone(n, ua, at(1, sg * 19, 116));
        snprintf(n, sizeof(n), "amxx_%s_hand", S);     const int ha = bone(n, fa, at(3, sg * 20, 90));
        wrist[sd] = g_body[ha].p;
        // Arms at the sides: the fingers run DOWN, the palm faces the thigh, the thumb is to the FRONT --
        // so the index finger is the front one and the little finger the back one. The real rig lists
        // the two hands' children in different orders; so does this.
        const V3 w = g_body[ha].p, down = mul(kU, -1.0f);
        const float fwdOf[5] = { 0.0f, 3.5f, 1.2f, -1.2f, -3.5f };
        snprintf(n, sizeof(n), "amxx_%s_hand_xtra", S);
        if (sd) bone(n, ha, add(w, mul(down, 5.0f)));
        auto thumb = [&]() {
            char t[64];
            const V3 base = add(w, add(mul(kF, 3.0f), mul(down, 3.0f))), k = mul(add(mul(kF, 2.0f), mul(down, 3.0f)), g_thumbLen);
            snprintf(t, sizeof(t), "amxx_%s_finger0", S);  const int t0 = bone(t, ha, base);
            snprintf(t, sizeof(t), "amxx_%s_finger01", S); const int t1 = bone(t, t0, add(base, k));
            snprintf(t, sizeof(t), "amxx_%s_finger02", S); bone(t, t1, add(base, add(k, mul(add(mul(kF, 1.0f), mul(down, 3.0f)), g_thumbLen))));
        };
        if (sd) thumb();
        for (int k = 4; k >= 1; k--) Finger(S, ha, k, add(w, add(mul(kF, fwdOf[k]), mul(down, 9.0f))), norm(add(down, mul(kF, 0.15f))), 4.0f, 2.6f);
        if (!sd) { thumb(); bone(n, ha, add(w, mul(down, 5.0f))); }
        snprintf(n, sizeof(n), "amxx_%s_thigh", S); const int th = bone(n, pelvis, at(0, sg * 9, 92));
        snprintf(n, sizeof(n), "amxx_%s_calf", S);  const int ca = bone(n, th, at(2, sg * 9, 50));
        snprintf(n, sizeof(n), "amxx_%s_foot", S);  const int ft = bone(n, ca, at(0, sg * 9, 9));
        snprintf(n, sizeof(n), "amxx_%s_toe0", S);  bone(n, ft, at(14, sg * 9, 2));
    }
    // THE BOARD RIG HANGS OFF THE ROOT, never off a hand: the carry animation puts it beside one.
    V3 tf_, tb_;
    if (boardHand < 0) { tf_ = at(119, 10, 6); tb_ = at(81, 10, 6); }                                      // lying on the ground ahead
    else {
        const V3 w = wrist[boardHand]; const float sg = boardHand ? 1.0f : -1.0f;
        if (grip == GRIP_HANG) { tf_ = add(w, add(mul(kU, -6.0f), mul(kR, sg * 4.0f))); tb_ = add(tf_, mul(kU, -42.27f)); }     // held by the front truck
        else                   { tf_ = add(w, add(mul(kF, 21.13f), mul(kU, -4.0f))); tb_ = add(w, add(mul(kF, -21.14f), mul(kU, -4.0f))); }  // carried flat
    }
    const V3 mid = mul(add(tf_, tb_), 0.5f);
    // THE BOARD'S BONES TURN WITH IT. In the reference pose it lies flat (line = forward, top = up); carried,
    // every board bone has that pose's orientation turned by the one rotation that took the board there.
    // Which face is the top is the TEST's to know (g_trueTop) and the rig's to be asked.
    {
        const V3 axc = norm(sub(tf_, tb_));
        V3 tt = boardHand >= 0 && grip == GRIP_HANG ? mul(kR, boardHand ? 1.0f : -1.0f) : kU;       // hanging: the grip tape faces away from the leg
        tt = norm(sub(tt, mul(axc, dot(tt, axc))));
        const Q4 r1 = qfromto(kF, axc);
        const V3 t1 = norm(qrot(r1, kU));
        const float roll = atan2f(dot(cross(t1, tt), axc), dot(t1, tt)) * 57.2957795f;
        g_boardRot = qmul(qaxis(axc, roll), r1);
        g_trueTop = tt;
    }
    const int broot = bone("skxx_skateskel_root", root, mid);
    const int flip = bone("skxx_skateskel_flipper", broot, mid);
    const int tfb = bone("skxx_skateskel_truck_front", flip, tf_);
    const int tbb = bone("skxx_skateskel_truck_back", flip, tb_);
    const V3 bR = qrot(g_boardRot, kR);                 // the BOARD's right: an axle is square to its top, wherever the world's right is
    bone("skxx_skateskel_wheel_fl", tfb, add(tf_, mul(bR, -9.0f))); bone("skxx_skateskel_wheel_fr", tfb, add(tf_, mul(bR, 9.0f)));
    bone("skxx_skateskel_wheel_bl", tbb, add(tb_, mul(bR, -9.0f))); bone("skxx_skateskel_wheel_br", tbb, add(tb_, mul(bR, 9.0f)));
    bone("skxx_skateskel_hand_anchor_r", flip, add(mid, mul(kU, 3.0f))); bone("skxx_skateskel_hand_anchor_l", flip, add(mid, mul(kU, -3.0f)));
    // THE FOOT ANCHORS ARE ANIMATED, and the field paid for believing them: lying flat (the reference pose) they
    // are where feet stand, 5 cm over the truck line; CARRIED, the animation has them off to the deck's SIDE.
    // A tap that judges the deck's top by them holds the board a quarter turn round -- as the real one did.
    const V3 axb = norm(sub(tf_, tb_));
    const V3 lie = boardHand >= 0 ? norm(cross(axb, g_trueTop)) : g_trueTop;
    bone("skxx_skateskel_foot_anchor_l", flip, add(add(mid, mul(axb, -14.0f)), mul(lie, boardHand >= 0 ? 3.0f : 5.0f)));
    bone("skxx_skateskel_foot_anchor_r", flip, add(add(mid, mul(axb, 14.0f)), mul(lie, boardHand >= 0 ? 3.0f : 5.0f)));
}

// A stand-in USkinnedMeshComponent: just the fields emote.cpp reads.
static std::vector<uint8_t> g_meshBlob;
static std::vector<float>   g_buffer;
// A REAL rig's bones each have an orientation of their own, and no two agree. The solver claims not to
// care -- it measures directions off positions and turns bones in component space -- so with this set
// every bone is given an arbitrary one, and every pose must come out where it did without.
static bool g_tumbled = false;
// ...and the mesh component may sit turned in the world (a character's usually is, a quarter turn).
static float g_meshYaw = 0.0f;
static std::vector<Q4> BoneQuats(int n) {            // each bone's own orientation: the same in every pose of the same rig
    std::vector<Q4> out((size_t)n, qid());
    unsigned rng = 12345u;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFF) / 32767.5f - 1.0f; };
    for (int i = 0; i < n; i++) if (g_tumbled) { const float a = rnd(), b = rnd(), c = rnd(), d = rnd(); out[(size_t)i] = qnorm(q4(a, b, c, d + 0.2f)); }
    return out;
}
// The REFERENCE pose, as the game's skeletal mesh would hand it over: the board flat on the ground. Call with
// the flat body built and its rig loaded.
static void TakeRefPose() {
    const int n = (int)g_body.size();
    const std::vector<Q4> bq = BoneQuats(n);
    static TF ref[MAX_BONES];
    for (int i = 0; i < n; i++) { ref[i].q = bq[(size_t)i]; ref[i].p = g_body[i].p; ref[i].s = v3(1.0f, 1.0f, 1.0f); }
    DeckTopFromRef(ref, false);
}
static void* MakeMesh() {
    const int n = (int)g_body.size();
    g_buffer.assign((size_t)n * 12, 0.0f);
    const std::vector<Q4> bq = BoneQuats(n);
    for (int i = 0; i < n; i++) {
        float* t = &g_buffer[(size_t)i * 12];
        Q4 q = bq[(size_t)i];
        if (strstr(g_body[i].name.c_str(), "skateskel")) q = qmul(g_boardRot, q);
        t[0] = q.x; t[1] = q.y; t[2] = q.z; t[3] = q.w;
        t[4] = g_body[i].p.x; t[5] = g_body[i].p.y; t[6] = g_body[i].p.z; t[8] = t[9] = t[10] = 1.0f;
    }
    g_meshBlob.assign(0x600, 0);
    uint8_t* m = g_meshBlob.data();
    float* c2w = (float*)(m + SC_C2W);
    const Q4 my = qaxis(kU, g_meshYaw);
    c2w[0] = my.x; c2w[1] = my.y; c2w[2] = my.z; c2w[3] = my.w; c2w[8] = c2w[9] = c2w[10] = 1.0f;
    *(int*)(m + SKM_EDIT) = 0;
    *(float**)(m + SKM_CST) = g_buffer.data();
    *(int*)(m + SKM_CST + 8) = n;
    return m;
}
static void LoadRig() {
    g_nBones = (int)g_body.size();
    for (int i = 0; i < g_nBones; i++) { snprintf(g_bones[i].name, sizeof(g_bones[i].name), "%s", g_body[i].name.c_str()); g_bones[i].parent = g_body[i].parent; }
    ResolveNames();
}

// ------------------------------------------------------------------ the judging
static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what, const char* detail = "") {
    if (ok) { g_pass++; return; }
    g_fail++; printf("  FAIL  %s %s\n", what, detail);
}
static const char* Fmt(const char* f, ...) { static char b[256]; va_list a; va_start(a, f); vsnprintf(b, sizeof(b), f, a); va_end(a); return b; }

// Play emote `id` at time t, fully blended in, on a fresh standing body carrying the board in `hand`
// (-1: not carried) with `grip`. Which hand has it is left for the emote to work out, as in the game.
static int g_grip = GRIP_HANG;
static float g_testSwingAt = 1.30f, g_testPeak = 1.0f;      // when a played Rage's trigger was pulled (emote time; < 0 = not yet), how far
// Point the camera: `yaw` to the body's RIGHT of straight ahead and `pitch` up, both radians, in the body's terms
// -- handed over in the WORLD's, as the game's camera is (the mesh component may sit turned in it).
static void Look(float yaw, float pitch) {
    const V3 d = add(mul(add(mul(kF, cosf(yaw)), mul(kR, sinf(yaw))), cosf(pitch)), mul(kU, sinf(pitch)));
    const V3 w = qrot(qaxis(kU, g_meshYaw), d);
    g_camFwd[0] = w.x; g_camFwd[1] = w.y; g_camFwd[2] = w.z;
}
// What the rig has to go by for the deck's top. THE REAL ONE IS TOP_DRAWN: its reference pose has the board's
// bones in a heap and says nothing, its carried foot anchors lie, and the board that is drawn is measured.
enum { TOP_ANCHORS = 0, TOP_REF = 1, TOP_DRAWN = 2, TOP_BOTH = 3 };
static int g_topMode = TOP_DRAWN;
static V3  g_drawnOff = { 0.0f, 0.0f, 0.0f };      // the drawn board sits this far off the bones' (component space)
// THE DRAWN BOARD, as the game leaves it each frame: a deck, two trucks and four wheels in the WORLD, where
// the bones were (plus g_drawnOff). The deck is 5 cm over the truck line; its own up is its top.
static void TakeDrawn(const void* mesh) {
    const float* buf = *(const float* const*)((const uint8_t*)mesh + SKM_CST);
    const float* c2w = (const float*)((const uint8_t*)mesh + SC_C2W);
    Vis v; v.meshQ = q4(c2w[0], c2w[1], c2w[2], c2w[3]); v.meshP = v3(c2w[4], c2w[5], c2w[6]); v.meshS = 1.0f;
    static TF bones[MAX_BONES];
    for (int i = 0; i < g_nBones; i++) { const float* t = buf + (size_t)i * 12; bones[i].q = q4(t[0], t[1], t[2], t[3]); bones[i].p = v3(t[4], t[5], t[6]); }
    auto world = [&](V3 cs) { return add(v.meshP, qrot(v.meshQ, add(cs, g_drawnOff))); };
    v.truckF = world(bones[g_truckF].p); v.truckB = world(bones[g_truckB].p);
    const char* const w[4] = { "wheel_bl", "wheel_br", "wheel_fl", "wheel_fr" };
    for (int i = 0; i < 4; i++) v.wheel[i] = world(bones[AnyNamed(w[i])].p);
    v.deck = world(add(mul(add(bones[g_truckF].p, bones[g_truckB].p), 0.5f), mul(g_trueTop, 5.0f)));
    v.deckQ = qmul(v.meshQ, g_boardRot);
    MeasureFromVisible(v, bones, true, true);
}
static void Play(int id, float t, int hand) {
    BuildBody(-1, g_grip); LoadRig();
    g_deckTopOk = false; g_deckTopSrc = 0; g_visOff = v3(0.0f, 0.0f, 0.0f); g_deckRise = 4.0f;
    if (g_topMode == TOP_REF || g_topMode == TOP_BOTH) TakeRefPose();
    BuildBody(hand, g_grip); LoadRig();
    void* mesh = MakeMesh();
    if (hand >= 0 && (g_topMode == TOP_DRAWN || g_topMode == TOP_BOTH)) TakeDrawn(mesh);
    g_id = id; g_t = t; g_w = 1.0f; g_carry = -2; g_lowerW = 1.0f; g_seated = false; g_ptSet = false;
    g_boardInHand = g_stubInHand = hand >= 0; g_thrown = false; g_throwDirSet = false; g_boardPosed = false;
    g_rageAimSet = false; g_rageLastT = 0.0f;
    g_rageSwingAt = g_testSwingAt; g_ragePeak = g_testPeak;
    g_mesh = mesh;
    Apply(mesh);
}
static bool Finite() {
    for (int i = 0; i < g_nBones; i++) {
        const TF& c = g_Ct[i];
        const float v[7] = { c.q.x, c.q.y, c.q.z, c.q.w, c.p.x, c.p.y, c.p.z };
        for (float f : v) if (!(f == f) || f > 1e6f || f < -1e6f) return false;
    }
    return true;
}
static bool IsBoardRoot(int i) { for (int k = 0; k < g_nBoardRoot; k++) if (g_boardRoot[k] == i) return true; return false; }
// Every bone is as far from its parent as it was standing: nothing was stretched to make a pose. The
// pelvis and the board rig's top bone are MOVED on purpose (a crouch, a carried board), so not them.
static float WorstStretch() {
    float worst = 0.0f;
    for (int i = 0; i < g_nBones; i++) {
        const int p = g_bones[i].parent;
        if (p < 0 || i == g_pelvis || IsBoardRoot(i)) continue;
        const float d = fabsf(len(sub(g_body[i].p, g_body[p].p)) - len(sub(g_Ct[i].p, g_Ct[p].p)));
        if (d > worst) worst = d;
    }
    return worst;
}
// The board is where it was IN THE HAND: every rig bone as far from the wrist, and from each other, as before.
static float BoardSlip(int hand) {
    const int pts[3] = { g_flipper, g_truckF, g_truckB };
    float worst = 0.0f;
    for (int a : pts) {
        const float d = fabsf(len(sub(g_C[a].p, g_C[g_hand[hand]].p)) - len(sub(g_Ct[a].p, g_Ct[g_hand[hand]].p)));
        if (d > worst) worst = d;
    }
    // ...and turned WITH the hand, not merely kept at arm's length: a point fixed in the hand's own frame
    const Q4 d = qmul(g_Ct[g_hand[hand]].q, qconj(g_C[g_hand[hand]].q));
    const V3 want = add(g_Ct[g_hand[hand]].p, qrot(d, sub(g_C[g_truckB].p, g_C[g_hand[hand]].p)));
    const float e = len(sub(want, g_Ct[g_truckB].p));
    return e > worst ? e : worst;
}
static float LowTipU() {
    const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p, ax = sub(a, b);
    const float n = dot(add(a, mul(ax, 0.47f)), kU), t = dot(sub(b, mul(ax, 0.47f)), kU);
    return n < t ? n : t;
}
static V3 LowTip() {
    const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p, ax = sub(a, b);
    const V3 n = add(a, mul(ax, 0.47f)), t = sub(b, mul(ax, 0.47f));
    return dot(n, kU) < dot(t, kU) ? n : t;
}

// "dump": where each emote puts the arms, in the body's own terms (cm ahead, to the RIGHT, up) -- for a
// person to read, since no check can say whether an elbow looks natural.
static void Dump() {
    auto say = [](const char* what, V3 p) { printf("    %-9s %6.1f ahead %6.1f right %6.1f up\n", what, dot(p, kF), dot(p, kR), dot(p, kU)); };
    for (int id = 0; id < EM_COUNT; id++) {
        const float t = id == EM_RAGE ? 1.0f : 1.0f;                      // a Rage at 1.0 is wound up and being aimed
        Look(id == EM_RAGE ? 0.5f : 0.0f, 0.0f); g_tapLift = 0.0f; g_tapSwing = 0.0f; g_tapHit = 0.0f;
        Play(id, t, 1);
        printf("%s (t = %.2f, the board hanging in the right hand)\n", kDefs[id].name, t);
        for (int sd = 1; sd >= 0; sd--) {
            if (len(sub(g_Ct[g_hand[sd]].p, g_C[g_hand[sd]].p)) < 0.5f) { printf("  %s arm: as animated\n", sd ? "right" : "left"); continue; }
            printf("  %s arm\n", sd ? "right" : "left");
            say("shoulder", g_Ct[g_uarm[sd]].p); say("elbow", g_Ct[g_larm[sd]].p); say("wrist", g_Ct[g_hand[sd]].p);
        }
        say("head", g_Ct[g_head].p); say("pelvis", g_Ct[g_pelvis].p); say("board low", LowTip());
    }
}
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "dump")) { Dump(); return 0; }
    BuildBody(1, GRIP_HANG); LoadRig();
    printf("omp_emotetest: %d bones\n", g_nBones);

    // ---- who is who
    Check(g_rigOk && g_legsOk, "the rig resolves");
    Check(g_nSpine == 3, "three spine bones", Fmt("(%d)", g_nSpine));
    Check(g_nBoardRoot == 1 && !strcmp(g_bones[g_boardRoot[0]].name, "skxx_skateskel_root"), "the board rig's one top bone", Fmt("(%d)", g_nBoardRoot));
    Check(g_truckF >= 0 && g_truckB >= 0 && g_flipper >= 0, "trucks and flipper found");
    for (int sd = 0; sd < 2; sd++) {
        Check(g_fingersOk[sd], sd ? "right fingers found" : "left fingers found");
        for (int f = 0; f < 5; f++) {
            char want[64]; snprintf(want, sizeof(want), "amxx_%s_finger%d", sd ? "r" : "l", f);
            const int j0 = g_fing[sd][f][0];
            Check(j0 >= 0 && !strcmp(g_bones[j0].name, want), "finger root by name", want);
            Check(g_fing[sd][f][1] >= 0 && g_fing[sd][f][2] >= 0, "finger chain of three", want);
        }
    }

    // ---- the palm, on a hand built from anatomy rather than from the formula
    {
        BuildBody(-1, GRIP_HANG); LoadRig();
        void* fresh = MakeMesh();                                              // a body nothing has posed yet
        g_id = EM_WAVE; g_t = 0.5f; g_w = 0.0f; g_carry = -2; g_boardInHand = g_stubInHand = false; g_mesh = fresh;
        Apply(fresh);                                                          // weight 0: only loads the pose
        Check(WorstStretch() < 1e-3f && Finite(), "a weightless emote changes nothing");
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        for (int sd = 0; sd < 2; sd++) {
            V3 F, N, A;
            Check(HandAxes(sd, &F, &N, &A), "hand axes measurable");
            const V3 toBody = mul(kR, sd ? -1.0f : 1.0f);      // a hanging hand's palm faces the thigh
            Check(dot(F, kU) < -0.9f, "hanging fingers run down");
            Check(dot(N, toBody) > 0.9f, sd ? "RIGHT palm faces the body" : "LEFT palm faces the body", Fmt("(N.toBody = %.2f)", dot(N, toBody)));
        }
    }

    // ---- hand orientation, the fist, and the thumb that stuck out of it
    for (int sd = 0; sd < 2; sd++) {
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        HandPose(sd, kU, kF);
        V3 F, N, A; HandAxes(sd, &F, &N, &A);
        Check(dot(F, kU) > 0.99f && dot(N, kF) > 0.99f, "HandPose: fingers up, palm forward", Fmt("(F.up %.3f N.fwd %.3f)", dot(F, kU), dot(N, kF)));
        HandPose(sd, kF, mul(kU, -1.0f));
        HandAxes(sd, &F, &N, &A);
        Check(dot(F, kF) > 0.99f && dot(N, kU) < -0.99f, "HandPose: fingers forward, palm down");
        // the hard case for a shortest-arc roll: the palm asked to face exactly the other way
        HandPose(sd, kF, kU);
        HandAxes(sd, &F, &N, &A);
        Check(dot(F, kF) > 0.99f && dot(N, kU) > 0.99f, "HandPose: a half-turn roll keeps the fingers where they were", Fmt("(F.fwd %.3f)", dot(F, kF)));
        const V3 tipBefore = g_Ct[g_fing[sd][2][2]].p, knuckle = g_Ct[g_fing[sd][2][0]].p;
        Fist(sd, TH_IN);
        const V3 tip = g_Ct[g_fing[sd][2][2]].p;
        Check(dot(sub(tip, knuckle), N) > 1.0f, "a fist closes TOWARD the palm", Fmt("(tip %.2f cm to the palm side)", dot(sub(tip, knuckle), N)));
        Check(len(sub(tip, knuckle)) < len(sub(tipBefore, knuckle)) - 1.0f, "a fist brings the fingertip in");
        // THE FOLDED THUMB, judged by where it ENDS UP rather than which way it was aimed: its last knuckle on
        // the first closed finger's middle bone, proud of it on the palm side -- on a thumb of the length
        // guessed AND on one half as long again, which is what the real rig turned out to have. (Aimed, the
        // long one ran the height of the fist and out past the little finger: the second field round.)
        for (int pointing = 0; pointing <= 1; pointing++)
            for (float tl = 1.0f; tl < 1.7f; tl += 0.5f) {
                g_thumbLen = tl;
                BuildBody(-1, GRIP_HANG); LoadRig();
                void* fresh = MakeMesh();
                g_id = EM_WAVE; g_t = 0.5f; g_w = 0.0f; g_carry = -2; g_boardInHand = g_stubInHand = false; g_mesh = fresh;
                Apply(fresh);
                for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
                BodyFrame(fresh);
                HandPose(sd, kF, mul(kU, -1.0f));                          // palm DOWN: the case that pointed a thumb at the ground
                if (pointing) Fingers(sd, TH_IN, 0.0f, 1.0f, 1.0f, 1.0f, 0.3f, 0.0f); else Fist(sd, TH_IN);
                V3 F2, N2, A2; HandAxes(sd, &F2, &N2, &A2);
                const int on = pointing ? 2 : 1;
                const V3 bone_ = mul(add(g_Ct[g_fing[sd][on][1]].p, g_Ct[g_fing[sd][on][2]].p), 0.5f);
                const V3 kn = g_Ct[g_fing[sd][0][2]].p;
                const char* who = Fmt("(%s hand, %s, thumb x%.1f: %.2f cm from it, %.2f proud)", sd ? "right" : "left", pointing ? "pointing" : "fist", tl,
                                      len(sub(kn, bone_)), dot(sub(kn, bone_), N2));
                char keep[160]; snprintf(keep, sizeof(keep), "%s", who);
                Check(len(sub(kn, bone_)) < 2.6f, "folded thumb: its knuckle rests on the first closed finger", keep);
                Check(dot(sub(kn, bone_), N2) > 0.4f, "folded thumb: ...on the palm side of it, not through it", keep);
                Check(dot(sub(kn, g_Ct[g_fing[sd][4][1]].p), A2) < -1.5f, "folded thumb: ...and nowhere near the little finger", keep);
                // NOTHING OF IT STICKS OUT OF THE FIST. Not a question of which way a bone points -- a thumb
                // folded under a palm-down fist rightly runs downward -- but of how far out its joints get:
                // no further to the palm side than the closed fingers themselves, and its ball no more than a
                // thumb's width out at the side.
                float fingersOut = 0.0f;
                for (int f = on; f <= 4; f++) for (int j = 1; j <= 2; j++) { const float o = dot(sub(g_Ct[g_fing[sd][f][j]].p, g_Ct[g_hand[sd]].p), N2); if (o > fingersOut) fingersOut = o; }
                const V3 ball = g_Ct[g_fing[sd][0][1]].p;
                const float ballOut = dot(sub(ball, g_Ct[g_hand[sd]].p), N2), ballSide = dot(sub(ball, g_Ct[g_fing[sd][1][0]].p), mul(A2, -1.0f));
                Check(ballOut < fingersOut + 1.0f, "folded thumb: its ball is not proud of the fist", Fmt("%s ball %.1f, fingers %.1f", keep, ballOut, fingersOut));
                Check(dot(sub(kn, g_Ct[g_hand[sd]].p), N2) < fingersOut + 2.2f, "folded thumb: its knuckle lies ON the fingers, not off them", keep);
                Check(ballSide < 5.5f, "folded thumb: its ball is at the side of the fist, not out from it", Fmt("%s %.1f cm", keep, ballSide));
                Check(Finite() && WorstStretch() < 0.05f, "folded thumb: finite and unstretched", keep);
            }
        g_thumbLen = 1.0f;
        BuildBody(-1, GRIP_HANG); LoadRig();
        { void* fresh = MakeMesh(); g_id = EM_WAVE; g_t = 0.5f; g_w = 0.0f; g_carry = -2; g_mesh = fresh; Apply(fresh); BodyFrame(fresh); }
        // ...and the open one: beside a flat hand, about 45 degrees off the index finger, in the palm's plane
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        HandPose(sd, kU, kF); OpenHand(sd, 1.3f); HandAxes(sd, &F, &N, &A);
        const V3 o0 = norm(sub(g_Ct[g_fing[sd][0][1]].p, g_Ct[g_fing[sd][0][0]].p));
        Check(dot(o0, A) < -0.5f && dot(o0, F) > 0.4f && fabsf(dot(o0, N)) < 0.35f, "open thumb: out to the side of a flat hand", Fmt("(A %.2f F %.2f N %.2f)", dot(o0, A), dot(o0, F), dot(o0, N)));
        Check(Finite(), "hands are finite");
    }

    // ---- every emote, several moments, the board in either hand or neither, both grips: finite, unstretched,
    //      the carrying hand found, and the board still IN it
    for (int grip = 0; grip < 2; grip++)
        for (int id = 0; id < EM_COUNT; id++)
            for (int hand = -1; hand <= 1; hand++)
                for (float t = 0.05f; t < 3.0f; t += 0.37f) {
                    g_grip = grip; Look(id == EM_RAGE ? 0.4f * (hand ? 1.0f : -1.0f) : 0.0f, 0.0f);
                    Play(id, t, hand);
                    const char* who = Fmt("(%s t=%.2f hand=%d grip=%d)", kDefs[id].name, t, hand, grip);
                    Check(Finite(), "finite", who);
                    Check(g_carry == hand, "the hand with the board is found", who);
                    const float st = WorstStretch();
                    Check(st < 0.05f, "no bone stretched", Fmt("(%s t=%.2f hand=%d grip=%d: %.3f cm)", kDefs[id].name, t, hand, grip, st));
                    if (hand >= 0 && id != EM_TAP && id != EM_CLAP) { const float sl = BoardSlip(hand);      /* the tap REGRIPS and the clap TUCKS it, on purpose: they have their own checks below */ Check(sl < 0.05f, "the board stays in the hand", Fmt("(%s t=%.2f hand=%d grip=%d: %.3f cm)", kDefs[id].name, t, hand, grip, sl)); }
                }
    g_grip = GRIP_HANG;

    // ---- what each free-hand gesture must show
    for (int hand = -1; hand <= 1; hand++) {
        const int sd = hand == 1 ? 0 : 1;                   // the free hand
        V3 F, N, A;

        Play(EM_WAVE, 0.4f, hand); HandAxes(sd, &F, &N, &A);
        Check(dot(F, kU) > 0.85f, "wave: fingers up", Fmt("(%.2f)", dot(F, kU)));
        Check(dot(N, kF) > 0.85f, "wave: palm to the front", Fmt("(%.2f, hand %d)", dot(N, kF), hand));
        Check(g_Ct[g_hand[sd]].p.z > g_Ct[g_uarm[sd]].p.z + 15.0f, "wave: the hand is above the shoulder");
        if (hand >= 0) Check(len(sub(g_Ct[g_hand[hand]].p, g_C[g_hand[hand]].p)) < 3.0f, "wave: the hand with the board stays put");

        Play(EM_THUMBS, 1.0f, hand); HandAxes(sd, &F, &N, &A);
        const V3 thumbDir = norm(sub(g_Ct[g_fing[sd][0][2]].p, g_Ct[g_fing[sd][0][0]].p));
        Check(dot(thumbDir, kU) > 0.6f, "thumbs up: the thumb points UP", Fmt("(%.2f, hand %d)", dot(thumbDir, kU), hand));
        Check(dot(F, kF) > 0.85f, "thumbs up: knuckles forward");
        Check(dot(sub(g_Ct[g_hand[sd]].p, g_Ct[g_uarm[sd]].p), kF) > 25.0f, "thumbs up: the hand is out in front");

        g_camFwd[0] = -0.6f; g_camFwd[1] = 0.8f; g_camFwd[2] = 0.0f;          // the camera looks ahead and 37 deg to the RIGHT (r = -x)
        Play(EM_POINT, 1.5f, hand); HandAxes(sd, &F, &N, &A);
        const V3 armDir = norm(sub(g_Ct[g_hand[sd]].p, g_Ct[g_uarm[sd]].p));
        const V3 index = norm(sub(g_Ct[g_fing[sd][1][2]].p, g_Ct[g_fing[sd][1][0]].p));
        const V3 pthumb = norm(sub(g_Ct[g_fing[sd][0][1]].p, g_Ct[g_fing[sd][0][0]].p));
        Check(dot(armDir, kF) > 0.6f, "point: the arm goes forward");
        if (sd == 1) Check(dot(armDir, kR) > 0.3f, "point: ...and toward where the camera looks", Fmt("(%.2f to the right)", dot(armDir, kR)));
        Check(dot(index, armDir) > 0.85f, "point: the index finger runs along the arm", Fmt("(%.2f)", dot(index, armDir)));
        {   // the pointing hand's thumb rests on the middle finger, which is closed: no part of it hangs below the fist
            const V3 kn = g_Ct[g_fing[sd][0][2]].p, mb = mul(add(g_Ct[g_fing[sd][2][1]].p, g_Ct[g_fing[sd][2][2]].p), 0.5f);
            Check(len(sub(kn, mb)) < 2.6f, "point: the thumb rests on the middle finger", Fmt("(%.2f cm from it, hand %d)", len(sub(kn, mb)), hand));
            (void)pthumb;
        }
        g_camFwd[0] = 0.0f; g_camFwd[1] = 1.0f;

        Play(EM_FACEPALM, 1.5f, hand); HandAxes(sd, &F, &N, &A);
        const int hb = g_head;
        Check(len(sub(g_Ct[g_hand[sd]].p, g_Ct[hb].p)) < 24.0f, "facepalm: the hand is at the head", Fmt("(%.1f cm)", len(sub(g_Ct[g_hand[sd]].p, g_Ct[hb].p))));
        Check(dot(N, kF) < -0.5f, "facepalm: the palm faces the face");
        Check(dot(sub(g_Ct[g_hand[sd]].p, g_Ct[hb].p), kF) > 9.0f, "facepalm: ...held OFF it, not in it", Fmt("(%.1f cm in front of the head bone)", dot(sub(g_Ct[g_hand[sd]].p, g_Ct[hb].p), kF)));
    }

    // ---- THE TAP: STANDING, as in the photograph. The nose in a hanging hand, the tail set down OUT AHEAD of
    //      the foot and well outside it, the GRAPHIC to the front -- and NOBODY BENDS: the pelvis and the feet are
    //      exactly where the game put them. Judged on the body the FIELD measured (shoulder 146, arm 56, board
    //      82), on either carry grip and either hand, and on a rig whose carried foot anchors LIE about the top.
    for (int grip = 0; grip < 2; grip++)
        for (int hand = 0; hand <= 1; hand++) {
            g_grip = grip;
            const char* who = Fmt("(hand %d, %s)", hand, grip ? "carried flat" : "hanging");
            char keep[96]; snprintf(keep, sizeof(keep), "%s", who);
            const float ground = 0.5f;                                  // the ankles are at 9; an ankle is 8.5
            const int flip = AnyNamed("skateskel_flipper");
            // the END OF THE WOOD at each end: on the truck line's end, then over it by the deck's rise and its kick
            auto topNow = [&]() { return norm(qrot(qmul(g_Ct[flip].q, qconj(g_C[flip].q)), g_trueTop)); };
            // ...of the board that is DRAWN: the bones' one, plus whatever the game sets it off by
            auto lineTail = []() { const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p; return add(sub(b, mul(sub(a, b), 0.47f)), g_drawnOff); };
            auto lineNose = []() { const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p; return add(add(a, mul(sub(a, b), 0.47f)), g_drawnOff); };
            auto tailP = [&]() { return add(lineTail(), mul(topNow(), g_deckRise + 3.0f * SC)); };
            auto noseP = [&]() { return add(lineNose(), mul(topNow(), g_deckRise + 3.0f * SC)); };
            g_tapLift = 0.0f; g_tapSwing = 0.0f; g_tapHit = 0.0f;
            Play(EM_TAP, 1.0f, hand);
            Check(g_deckTopOk && g_deckTopSrc == 2 && fabsf(g_deckRise - 5.0f) < 0.05f, "tap: the DRAWN board was measured: the deck's top, and its rise over the trucks", Fmt("%s source %d rise %.2f", keep, g_deckTopSrc, g_deckRise));
            {   // the rig's own account of the carried board: the reference pose is RIGHT, the anchors a quarter turn out
                V3 a0 = norm(sub(g_C[g_truckF].p, g_C[g_truckB].p));
                const float byRef = acosf(clampf(dot(DeckTop(a0), g_trueTop), -1.0f, 1.0f)) * 57.2957795f;
                const float byAnchors = acosf(clampf(dot(DeckTopByAnchors(a0), g_trueTop), -1.0f, 1.0f)) * 57.2957795f;
                Check(byRef < 1.0f, "tap: the deck's top is known from the board that is drawn", Fmt("%s %.1f deg out", keep, byRef));
                Check(byAnchors > 60.0f, "tap: ...where the carried foot anchors would have had it a quarter turn out (the field's bug)", Fmt("%s %.1f deg", keep, byAnchors));
            }
            Check(len(sub(g_Ct[g_pelvis].p, g_C[g_pelvis].p)) < 0.01f, "tap: the hips are EXACTLY where the game has them (no crouch)", Fmt("%s moved %.2f cm", keep, len(sub(g_Ct[g_pelvis].p, g_C[g_pelvis].p))));
            const V3 side = mul(kR, hand ? 1.0f : -1.0f);
            for (int sd = 0; sd < 2; sd++)
                Check(len(sub(g_Ct[g_foot[sd]].p, g_C[g_foot[sd]].p)) < 0.01f && len(sub(g_Ct[g_calf[sd]].p, g_C[g_calf[sd]].p)) < 0.01f, "tap: ...and so are the legs", keep);
            const float lean = acosf(clampf(dot(norm(sub(g_Ct[g_neck].p, g_Ct[g_pelvis].p)), norm(sub(g_C[g_neck].p, g_C[g_pelvis].p))), -1.0f, 1.0f)) * 57.2957795f;
            Check(lean < 3.0f, "tap: the body stands upright", Fmt("%s %.1f deg off how it stood", keep, lean));
            Check(fabsf(dot(tailP(), kU) - ground) < 1.0f, "tap: at no lift the TAIL's end is on the ground", Fmt("%s %.2f cm off", keep, dot(tailP(), kU) - ground));
            // WHERE IT STANDS: the tail out ahead of the toes and well outside the foot -- a walking leg goes past
            // a board 20 cm wide, not through it -- leaning back to a hand that hangs by the hip
            const V3 toe = g_Ct[AnyNamed(hand ? "r_toe0" : "l_toe0")].p, foot = g_Ct[g_foot[hand]].p;
            Check(dot(sub(tailP(), toe), kF) > 6.0f, "tap: the tail is set down AHEAD of the foot", Fmt("%s %.1f cm past the toes", keep, dot(sub(tailP(), toe), kF)));
            Check(dot(sub(lineTail(), foot), side) > 22.0f, "tap: ...and well OUTSIDE it: the leg walks past the board", Fmt("%s %.1f cm outside the foot", keep, dot(sub(lineTail(), foot), side)));
            Check(dot(sub(lineNose(), g_Ct[g_thigh[hand]].p), side) - 10.0f > 9.0f, "tap: the board's inner edge is clear of the hip", Fmt("%s %.1f cm", keep, dot(sub(lineNose(), g_Ct[g_thigh[hand]].p), side) - 10.0f));
            const V3 axis = norm(sub(lineNose(), lineTail()));
            Check(dot(axis, kU) > 0.95f && dot(axis, kU) < 0.995f, "tap: the board stands nose up, LEANING back to the hand", Fmt("%s %.3f", keep, dot(axis, kU)));
            Check(dot(sub(lineTail(), lineNose()), kF) > 10.0f, "tap: ...its tail further forward than its nose", keep);
            Check(dot(lineNose(), kF) < 16.0f, "tap: ...and the nose by the hip, not held out in front", keep);
            // the GRAPHIC to the front: the deck's TRUE top faces back at the skater
            Check(dot(topNow(), kF) < -0.93f, "tap: the graphic faces the FRONT (the deck's true top faces the skater)", Fmt("%s top.forward %.2f", keep, dot(topNow(), kF)));
            // the hand LIES OVER THE NOSE'S END from the skater's side: wrist above the end and back toward the
            // body, fingers gone forward over it and down, palm down onto it
            const V3 wrist = g_Ct[g_hand[hand]].p;
            const float above = dot(sub(wrist, noseP()), kU);
            Check(above > 4.0f && above < 14.0f, "tap: the wrist is over the nose's end", Fmt("%s %.1f cm above", keep, above));
            Check(dot(sub(wrist, noseP()), kF) < -0.5f, "tap: ...on the skater's side of it", Fmt("%s %.1f cm", keep, dot(sub(wrist, noseP()), kF)));
            V3 F, N, A; HandAxes(hand, &F, &N, &A);
            Check(dot(F, kU) < -0.45f && dot(F, kF) > 0.3f, "tap: the fingers go forward OVER the end and down", Fmt("%s up %.2f fwd %.2f", keep, dot(F, kU), dot(F, kF)));
            Check(dot(N, kU) < -0.3f && dot(N, kF) < -0.3f, "tap: the palm is down on it, facing back", Fmt("%s up %.2f fwd %.2f", keep, dot(N, kU), dot(N, kF)));
            {   // IN THE HAND: the end of the wood is under the knuckles -- a knuckle's depth off the palm's plane, no more
                V3 kn = v3(0, 0, 0); int k = 0;
                for (int f = 1; f <= 4; f++) if (g_fing[hand][f][0] >= 0) { kn = add(kn, g_Ct[g_fing[hand][f][0]].p); k++; }
                kn = mul(kn, 1.0f / (float)k);
                Check(len(sub(kn, noseP())) < 6.0f, "tap: the nose's end is IN the hand, under the knuckles", Fmt("%s %.1f cm from them", keep, len(sub(kn, noseP()))));
            }
            // the arm hangs: nearly straight, elbow behind the line from shoulder to wrist rather than out front
            const V3 Sh = g_Ct[g_uarm[hand]].p, El = g_Ct[g_larm[hand]].p;
            Check(len(sub(wrist, Sh)) > 0.93f * (len(sub(El, Sh)) + len(sub(wrist, El))), "tap: the arm hangs nearly straight", keep);
            const V3 tail0 = tailP();
            g_tapLift = 24.0f; Play(EM_TAP, 1.0f, hand);
            Check(fabsf(dot(tailP(), kU) - ground - 24.0f) < 1.5f, "tap: lifted, the tail is up by the lift", Fmt("%s %.1f cm up, wanted 24", keep, dot(tailP(), kU) - ground));
            Check(len(sub(g_Ct[g_pelvis].p, g_C[g_pelvis].p)) < 0.01f, "tap: ...and still nobody bent", keep);
            Check(dot(sub(g_Ct[g_larm[hand]].p, g_Ct[g_uarm[hand]].p), kF) < 2.0f, "tap: the lifting arm's elbow goes BACK, not out front", keep);
            g_tapLift = 0.0f; g_tapSwing = 1.0f; Play(EM_TAP, 1.0f, hand);
            Check(dot(sub(tailP(), tail0), kR) > 8.0f, "tap: the stick swings the tail to the side it is pushed", Fmt("%s %.1f cm", keep, dot(sub(tailP(), tail0), kR)));
            Check(fabsf(dot(tailP(), kU) - ground) < 1.5f, "tap: ...and it still lands", keep);
            g_tapSwing = 0.0f;
            // asked to, the ini turns the board about its own length and nothing else: the hand stays put
            Play(EM_TAP, 1.0f, hand);
            const V3 w0 = g_Ct[g_hand[hand]].p;
            g_tapRollDeg = 90.0f; Play(EM_TAP, 1.0f, hand);
            Check(fabsf(dot(topNow(), kF)) < 0.35f && len(sub(g_Ct[g_hand[hand]].p, w0)) < 0.05f, "tap: EmoteTapRollDeg turns the board in the hand, a quarter turn for 90", Fmt("%s top.forward %.2f", keep, dot(topNow(), kF)));
            g_tapRollDeg = 0.0f;
        }
    g_grip = GRIP_HANG;
    // the other things a rig may have to go by: a reference pose that does say (alone, or with the drawn board,
    // which then has the last word), and nothing at all -- the anchors' guess, wrong, but it still plays
    {
        const int flip = AnyNamed("skateskel_flipper");
        auto topNow = [&]() { return norm(qrot(qmul(g_Ct[flip].q, qconj(g_C[flip].q)), g_trueTop)); };
        g_tapLift = 0.0f; g_tapSwing = 0.0f;
        for (int hand = 0; hand <= 1; hand++) {
            g_topMode = TOP_REF; Play(EM_TAP, 1.0f, hand);
            Check(g_deckTopSrc == 1 && dot(topNow(), kF) < -0.93f, "tap: by the reference pose alone the graphic still faces the front", Fmt("(hand %d, source %d, top.forward %.2f)", hand, g_deckTopSrc, dot(topNow(), kF)));
            g_topMode = TOP_BOTH; Play(EM_TAP, 1.0f, hand);
            Check(g_deckTopSrc == 2 && dot(topNow(), kF) < -0.93f, "tap: with both, the drawn board has the last word", Fmt("(hand %d, source %d)", hand, g_deckTopSrc));
        }
        g_topMode = TOP_ANCHORS; Play(EM_TAP, 1.0f, 0);
        Check(!g_deckTopOk && g_boardPosed, "tap: with nothing to go by it falls back to the anchors and still plays");
        g_topMode = TOP_DRAWN;
        // THE DRAWN BOARD SITS OFF THE BONES (PlaceInHand scales and shifts the socket): it is the drawn one that
        // goes in the hand and on the ground
        g_drawnOff = at(3.0f, -2.0f, 4.0f);
        for (int hand = 0; hand <= 1; hand++) {
            Play(EM_TAP, 1.0f, hand);
            const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p;
            const V3 e = mul(topNow(), g_deckRise + 3.0f * SC);
            const V3 tail = add(add(sub(b, mul(sub(a, b), 0.47f)), g_drawnOff), e), nose = add(add(add(a, mul(sub(a, b), 0.47f)), g_drawnOff), e);
            Check(len(sub(g_visOff, g_drawnOff)) < 0.01f, "tap: how far the drawn board sits off the bones is measured", Fmt("(hand %d)", hand));
            Check(fabsf(dot(tail, kU) - 0.5f) < 1.0f, "tap: ...and it is the DRAWN tail that is on the ground", Fmt("(hand %d, %.2f cm off)", hand, dot(tail, kU) - 0.5f));
            V3 kn = v3(0, 0, 0); int k = 0;
            for (int f = 1; f <= 4; f++) if (g_fing[hand][f][0] >= 0) { kn = add(kn, g_Ct[g_fing[hand][f][0]].p); k++; }
            kn = mul(kn, 1.0f / (float)k);
            Check(len(sub(kn, nose)) < 6.0f, "tap: ...and the DRAWN nose that is in the hand", Fmt("(hand %d, %.1f cm from the knuckles)", hand, len(sub(kn, nose))));
        }
        g_drawnOff = v3(0.0f, 0.0f, 0.0f);
        // a measurement that jumps for one frame is not believed; one that has plainly changed is
        Play(EM_TAP, 1.0f, 0);
        const V3 was = g_deckTopL;
        Vis v; v.meshQ = qid(); v.meshP = v3(0, 0, 0); v.meshS = 1.0f; v.deckQ = qid();
        static TF bones[MAX_BONES];
        for (int i = 0; i < g_nBones; i++) bones[i] = g_C[i];
        const V3 tfp = g_C[g_truckF].p, tbp = g_C[g_truckB].p, axl = norm(sub(tfp, tbp));
        const V3 other = norm(cross(axl, g_trueTop));                                 // a deck turned a quarter round on its trucks
        v.truckF = tfp; v.truckB = tbp; v.deck = add(mul(add(tfp, tbp), 0.5f), mul(other, 5.0f));
        const V3 axle = norm(cross(other, axl));
        v.wheel[0] = add(tbp, mul(axle, -9.0f)); v.wheel[1] = add(tbp, mul(axle, 9.0f)); v.wheel[2] = add(tfp, mul(axle, -9.0f)); v.wheel[3] = add(tfp, mul(axle, 9.0f));
        MeasureFromVisible(v, bones, false, true);
        Check(dot(g_deckTopL, was) > 0.999f, "drawn board: one odd frame is not believed");
        for (int i = 0; i < 6; i++) MeasureFromVisible(v, bones, false, true);
        Check(dot(qrot(g_C[g_flipper].q, g_deckTopL), other) > 0.99f, "drawn board: ...but a deck that has plainly turned is");
        g_visSay = false;
    }
    // ---- the stick and the spring: a lift and a letting-go is ONE tap; a board left alone makes none; driven
    //      down it lands harder than dropped; and nothing is heard while the stick only holds it up
    {
        auto run = [](float seconds, float stickY, int* taps, float* hardest) {
            for (float t = 0.0f; t < seconds; t += 1.0f / 120.0f) {
                const float before = g_tapHit;
                g_stickX = 0.0f; g_stickY = stickY;
                PumpTap(nullptr, 1.0f / 120.0f);
                if (g_tapHit > before + 0.05f) { (*taps)++; if (g_tapHit > *hardest) *hardest = g_tapHit; }
            }
        };
        int taps = 0; float hard = 0.0f, dropped = 0.0f, driven = 0.0f;
        g_tapLift = 0.0f; g_tapVel = 0.0f; g_tapHit = 0.0f; g_tapDown = true;
        run(3.0f, 0.0f, &taps, &hard);
        Check(taps == 0, "spring: a board left alone does not tap", Fmt("(%d)", taps));
        run(0.6f, 1.0f, &taps, &hard);
        Check(taps == 0 && g_tapLift > 20.0f, "spring: the stick lifts it, in silence", Fmt("(%d taps, %.1f cm up)", taps, g_tapLift));
        run(1.5f, 0.0f, &taps, &dropped);
        Check(taps == 1, "spring: letting go is exactly ONE tap", Fmt("(%d)", taps));
        Check(g_tapLift < 0.5f, "spring: ...and it comes to rest on the ground", Fmt("(%.2f cm)", g_tapLift));
        taps = 0; run(0.6f, 1.0f, &taps, &hard); run(1.5f, -1.0f, &taps, &driven);
        Check(taps == 1, "spring: driven down is one tap too", Fmt("(%d)", taps));
        Check(driven > dropped + 0.03f, "spring: ...and a harder one than a drop", Fmt("(driven %.2f, dropped %.2f)", driven, dropped));
        // HOW FAST IT MOVES. The field: "the pull down and drop down make the board pull down too quick... all
        // very abrupt". The first cut fell 32 cm in ~60 ms -- four times faster than falling. A hand's pace:
        {
            auto settle = [&](float y, float secs) { int t_ = 0; float h_ = 0.0f; run(secs, y, &t_, &h_); };
            auto fall = [&](float y, float* landedAt, float* speed) {       // from held at full height: time to the ground, and how fast it got there
                settle(1.0f, 1.5f);
                *landedAt = -1.0f; *speed = 0.0f;
                for (float t = 0.0f; t < 1.5f; t += 1.0f / 120.0f) {
                    const float v0 = -g_tapVel, wasDown = g_tapDown;
                    g_stickX = 0.0f; g_stickY = y; PumpTap(nullptr, 1.0f / 120.0f);
                    if (!wasDown && g_tapDown) { *landedAt = t; *speed = v0; break; }
                }
            };
            settle(0.0f, 1.0f);
            float peak = 0.0f, t90 = -1.0f;
            for (float t = 0.0f; t < 1.5f; t += 1.0f / 120.0f) {
                g_stickX = 0.0f; g_stickY = 1.0f; PumpTap(nullptr, 1.0f / 120.0f);
                if (g_tapLift > peak) peak = g_tapLift;
                if (t90 < 0.0f && g_tapLift > 0.9f * 32.0f) t90 = t;
            }
            Check(t90 > 0.18f && t90 < 0.50f, "pace: lifting takes a moment, not a frame", Fmt("(%.2f s to nine tenths)", t90));
            Check(peak < 32.0f * 1.04f, "pace: ...and arrives without springing past", Fmt("(peak %.1f of 32)", peak));
            float tDrop, vDrop, tPull, vPull;
            fall(0.0f, &tDrop, &vDrop);
            Check(tDrop > 0.22f && tDrop < 0.45f, "pace: let go from full height it takes about a third of a second to land", Fmt("(%.3f s)", tDrop));
            Check(vDrop > 110.0f && vDrop < 260.0f, "pace: ...landing at a falling thing's speed", Fmt("(%.0f cm/s)", vDrop));
            fall(-1.0f, &tPull, &vPull);
            Check(tPull > 0.12f && tPull < 0.30f, "pace: pulled down it is quicker, but still a movement you can see", Fmt("(%.3f s)", tPull));
            Check(vPull > vDrop * 1.3f, "pace: ...and lands harder than a drop", Fmt("(%.0f vs %.0f cm/s)", vPull, vDrop));
            // lowered IN THE AIR (stick eased from full to half) it goes to the new height and stops: no knock, no dip
            settle(1.0f, 1.5f);
            int tp = 0; float hd = 0.0f, low = 99.0f;
            for (float t = 0.0f; t < 1.5f; t += 1.0f / 120.0f) {
                const float before = g_tapHit;
                g_stickX = 0.0f; g_stickY = 0.14f + 0.86f * 0.5f; PumpTap(nullptr, 1.0f / 120.0f);
                if (g_tapHit > before + 0.05f) tp++;
                if (g_tapLift < low) low = g_tapLift;
            }
            (void)hd;
            Check(tp == 0 && fabsf(g_tapLift - 16.0f) < 0.5f && low > 14.5f, "pace: eased down in the air it settles at the new height, silently", Fmt("(%d taps, at %.1f, lowest %.1f)", tp, g_tapLift, low));
            settle(0.0f, 1.5f);
        }
        // WALKING: the tail is carried clear of the ground, in silence; stopping sets it down with a quiet knock
        // at most -- never the drop a released stick gives
        taps = 0; float soft = 0.0f;
        run(2.0f, 0.0f, &taps, &hard);
        g_speed = 160.0f; run(1.5f, 0.0f, &taps, &hard);
        Check(taps == 0 && g_tapLift > 4.0f && g_tapLift < 9.0f, "spring: walking, the tail is carried just clear of the ground", Fmt("(%d taps, %.1f cm up)", taps, g_tapLift));
        g_speed = 0.0f; run(2.0f, 0.0f, &taps, &soft);
        Check(taps <= 1 && g_tapLift < 0.5f, "spring: stopping sets it down again", Fmt("(%d taps, %.2f cm)", taps, g_tapLift));
        Check(soft < dropped - 0.05f, "spring: ...softly: quieter than a drop", Fmt("(set down %.2f, dropped %.2f)", soft, dropped));
        g_speed = 160.0f; taps = 0; run(0.8f, 0.0f, &taps, &hard); run(0.5f, 1.0f, &taps, &hard); run(1.0f, -1.0f, &taps, &hard);
        Check(taps == 1, "spring: and it still taps on the stick while walking", Fmt("(%d)", taps));
        g_speed = 0.0f; run(2.0f, 0.0f, &taps, &hard);
        // what ends a tap: B, picking it again, the board going. NOT walking (that rule is for emotes that have
        // the legs -- a dance) and NOT the stick being left alone.
        Check(kDefs[EM_TAP].loop && !kDefs[EM_TAP].lower, "tap: it has no lower body, so walking does not end it");
        Check(kDefs[EM_DANCE].loop && kDefs[EM_DANCE].lower, "dance: ...as it does a dance, which has");
        taps = 0;
        for (int k = 0; k < 6; k++) { run(0.22f, 1.0f, &taps, &hard); run(0.22f, -1.0f, &taps, &hard); }
        Check(taps >= 5 && taps <= 7, "spring: worked up and down it taps in time with the stick", Fmt("(%d taps from 6 strokes)", taps));
    }

    // ---- RAGE IS AIMED: it goes the way the camera looks. Wound up by half a second, HELD and aimed until the
    //      RIGHT TRIGGER is pulled (here: at 1.30), let go a moment into the swing; the look is followed until then
    //      and not after. Up lobs it, down throws it flat. And it tells the pump where, in the WORLD's terms.
    {
        const float kRageSwing = 1.30f, kRageRelease = kRageSwing + kRageLetGo;
        const float tHeld = 1.0f, tThrough = kRageSwing + 0.28f;
        auto throwW = []() { return v3(g_throwDirW[0], g_throwDirW[1], g_throwDirW[2]); };
        auto flatYaw = [](V3 d) { return atan2f(dot(d, kR), dot(d, kF)); };             // to the body's right of ahead, mesh yaw 0
        Check(kRageLetGo > 0.05f && kRageLetGo < kRageSwingLen, "rage: it lets go inside the swing");
        // HELD AS LONG AS YOU LIKE: no trigger, no throw -- a minute on it is still cocked over the head, and aiming
        g_testSwingAt = -1.0f;
        for (int hand = 0; hand <= 1; hand++) {
            Look(0.7f, 0.0f); Play(EM_RAGE, 60.0f, hand);
            Check(g_Ct[g_hand[hand]].p.z > g_Ct[g_head].p.z && BoardSlip(hand) < 0.05f, "rage: with no trigger it stays wound up, board in hand, for as long as you like", Fmt("(hand %d)", hand));
            Check(fabsf(g_rageYaw - 0.7f) < 0.02f, "rage: ...still aiming");
        }
        g_testSwingAt = kRageSwing;
        // THE ONE EMOTE THAT ENDS ITSELF: "Throw board" on the wheel. Aimed for as long as you like -- but once it has
        // gone he is angry about it for a bit (the old one-shot's length) and then it is over, with no B needed.
        Check(!strcmp(kDefs[EM_RAGE].name, "Throw board"), "throw board: that is what the wheel calls it");
        g_rageSwingAt = -1.0f;
        Check(!RageDone(0.0f) && !RageDone(600.0f), "throw board: never over while it is still being aimed");
        g_rageSwingAt = kRageSwing;
        Check(!RageDone(kRageSwing + 2.0f) && RageDone(kRageSwing + kRageAfter + 0.01f), "throw board: thrown, it ends by itself a couple of seconds on", Fmt("(%.2f s after the swing starts)", kRageAfter));
        {   // ...angry in between (both fists down by the hips, hunched), and calm again by the end
            Play(EM_RAGE, kRageSwing + 1.4f, 1);
            const float hunch = dot(sub(g_Ct[g_neck].p, g_Ct[g_pelvis].p), kF);
            Check(Finite() && WorstStretch() < 0.012f && g_Ct[g_hand[1]].p.z < g_Ct[g_head].p.z && hunch > 2.0f, "throw board: angry about it for a bit", Fmt("(hunched %.1f cm)", hunch));
            Play(EM_RAGE, kRageSwing + 2.6f, 1);
            Check(dot(sub(g_Ct[g_neck].p, g_Ct[g_pelvis].p), kF) < hunch - 1.5f, "throw board: ...and over it by the time it ends");
        }
        // THE TRIGGER. Held when the Rage begins (through the wheel), it is not listened to until let go; a pull
        // before the wind-up is done throws as soon as it is; the STRENGTH is the most it reached before the board
        // left the hand, and nothing after.
        {
            auto fresh = [](float trigNow) { g_trigger = trigNow; g_rageSwingAt = -1.0f; g_ragePeak = 0.0f; g_pulling = false; g_trigArmed = g_trigger < kTrigOff; };
            auto hold = [](float from, float to, float v) { for (float t = from; t < to; t += 0.008f) { g_trigger = v; RageTrigger(t); } };
            fresh(1.0f);
            hold(0.0f, 2.0f, 1.0f);
            Check(g_rageSwingAt < 0.0f, "trigger: one held while the wheel was up does not throw");
            hold(2.0f, 2.02f, 0.0f); hold(2.02f, 2.30f, 0.6f);
            Check(g_rageSwingAt > 2.02f && g_rageSwingAt < 2.13f && fabsf(g_ragePeak - 0.6f) < 0.01f, "trigger: ...let go and pulled again, it does -- once the pull has stopped", Fmt("(at %.3f, %.2f)", g_rageSwingAt, g_ragePeak));
            fresh(0.0f);
            hold(0.0f, 0.30f, 0.9f);
            Check(g_rageSwingAt < 0.0f, "trigger: pulled before the wind-up is done, it waits for it");
            hold(0.30f, 0.70f, 0.9f);
            Check(g_rageSwingAt > 0.40f && g_rageSwingAt < 0.56f, "trigger: ...and goes as soon as it is", Fmt("(%.2f)", g_rageSwingAt));
            // A QUICK FULL PULL goes at once, at full strength
            fresh(0.0f);
            { float t = 1.0f; for (int k = 0; k < 40 && g_rageSwingAt < 0.0f; k++, t += 0.008f) { g_trigger = k < 6 ? 0.18f * (float)k : 1.0f; RageTrigger(t); }
              Check(g_rageSwingAt > 0.0f && g_rageSwingAt < 1.07f && g_ragePeak > 0.96f, "trigger: a quick full pull goes at once, and hard", Fmt("(%.3f s, %.2f)", g_rageSwingAt - 1.0f, g_ragePeak)); }
            // A SQUEEZE TO PART WAY, AND HELD: known a moment after it stops, and that is how hard -- pulling on
            // further after the board has gone changes nothing
            fresh(0.0f);
            float t = 1.0f;
            for (int k = 0; k <= 11; k++, t += 0.008f) { g_trigger = 0.05f * (float)k; RageTrigger(t); }       // ...stops at 0.55
            Check(g_rageSwingAt < 0.0f, "trigger: it does not go while the pull is still going further");
            for (; g_rageSwingAt < 0.0f && t < 2.0f; t += 0.008f) RageTrigger(t);
            const float at = g_rageSwingAt;
            Check(at > 1.0f && at < 1.0f + 12 * 0.008f + kTrigSettle + 0.02f, "trigger: ...and goes a moment after it stops", Fmt("(%.3f)", at));
            for (; t < at + kRageLetGo; t += 0.008f) RageTrigger(t);
            g_trigger = 1.0f; RageTrigger(at + kRageLetGo + 0.01f);                                              // ...pulled harder AFTER it has gone
            Check(fabsf(g_ragePeak - 0.55f) < 0.01f, "trigger: how hard is how far it was pulled", Fmt("(%.2f)", g_ragePeak));
            // A SLOW SQUEEZE ALL THE WAY is a full throw: not whatever it had reached early on
            fresh(0.0f);
            { float tt = 1.0f; for (; tt < 3.0f && g_rageSwingAt < 0.0f; tt += 0.008f) { g_trigger = clampf((tt - 1.0f) / 0.6f, 0.0f, 1.0f); RageTrigger(tt); }
              Check(g_ragePeak > 0.96f && g_rageSwingAt > 1.5f, "trigger: a slow squeeze all the way is a FULL throw", Fmt("(%.2f, at %.2f)", g_ragePeak, g_rageSwingAt)); }
            // a blip -- touched and let go before it settled -- goes with what it reached
            fresh(0.0f);
            hold(1.0f, 1.03f, 0.30f); hold(1.03f, 1.10f, 0.0f);
            Check(g_rageSwingAt > 1.0f && fabsf(g_ragePeak - 0.30f) < 0.01f, "trigger: a blip goes with what it reached");
            // THE ENGINE'S VALUE IS THE ONE THAT COUNTS: with it to be had, the press (a "full pull") is not listened to
            g_stubRT = 0.35f; PollTrigger(); Emote_Trigger(1.0f);
            Check(g_trigPolled && fabsf(g_trigger - 0.35f) < 1e-4f, "trigger: the analogue value is what counts, not the press");
            g_stubRT = -1.0f; PollTrigger(); Emote_Trigger(1.0f);
            Check(!g_trigPolled && g_trigger > 0.99f, "trigger: ...and with none to be had, the press is a full pull");
            Check(RageSpeed(kTrigOn) > 350.0f && RageSpeed(kTrigOn) < 500.0f && RageSpeed(1.0f) > 1400.0f && RageSpeed(1.0f) < 1700.0f, "trigger: a touch is a toss, a full pull a hurl", Fmt("(%.0f .. %.0f cm/s)", RageSpeed(kTrigOn), RageSpeed(1.0f)));
            float prev = 0.0f; bool rising = true;
            for (float v = kTrigOn; v <= 1.0f; v += 0.04f) { if (RageSpeed(v) <= prev) rising = false; prev = RageSpeed(v); }
            Check(rising, "trigger: ...and every bit further is a bit harder");
            g_trigger = 0.0f;
            // a harder throw is a bigger one in the body, too
            g_testPeak = 1.0f;  Play(EM_RAGE, kRageSwing + 0.16f, 1); const float leanHard = dot(sub(g_Ct[g_neck].p, g_Ct[g_pelvis].p), kF);
            g_testPeak = 0.15f; Play(EM_RAGE, kRageSwing + 0.16f, 1); const float leanSoft = dot(sub(g_Ct[g_neck].p, g_Ct[g_pelvis].p), kF);
            Check(leanHard > leanSoft + 1.0f, "trigger: the body goes into a hard throw more than a soft one", Fmt("(%.1f vs %.1f cm)", leanHard, leanSoft));
            g_testPeak = 1.0f;
        }
        for (int hand = 0; hand <= 1; hand++) {
            Look(0.0f, 0.0f);
            Play(EM_RAGE, tHeld, hand);
            Check(g_Ct[g_hand[hand]].p.z > g_Ct[g_head].p.z, "rage: wound up over the head", Fmt("(hand %.0f, head %.0f)", g_Ct[g_hand[hand]].p.z, g_Ct[g_head].p.z));
            Check(dot(sub(g_Ct[g_hand[hand]].p, g_Ct[g_uarm[hand]].p), kF) < 0.0f, "rage: ...and behind the shoulder");
            Check(BoardSlip(hand) < 0.05f, "rage: the board goes up with the hand");
            Play(EM_RAGE, tThrough, hand);
            Check(dot(sub(g_Ct[g_hand[hand]].p, g_Ct[g_uarm[hand]].p), kF) > 25.0f, "rage: thrown THROUGH, out in front");
            Check(g_throwDirSet && throwW().y > 0.8f && throwW().z > 0.1f, "rage: looking ahead, the world is told it goes ahead and up", Fmt("(%.2f %.2f %.2f)", throwW().x, throwW().y, throwW().z));
            // LOOK SOMEWHERE ELSE and that is where it goes: to either side, and behind
            Play(EM_RAGE, tHeld, hand);
            const V3 acrossAhead = norm(sub(g_Ct[g_uarm[1]].p, g_Ct[g_uarm[0]].p));          // the shoulders, wound up and aimed straight ahead
            const float yaws[5] = { 1.0f, -1.0f, 0.4f, 2.6f, -2.9f };
            for (float yw : yaws) {
                Look(yw, 0.0f);
                Play(EM_RAGE, tThrough, hand);
                float dy = flatYaw(throwW()) - yw; while (dy > 3.14159265f) dy -= 6.2831853f; while (dy < -3.14159265f) dy += 6.2831853f;
                Check(fabsf(dy) < 0.03f, "rage: it goes the way the camera LOOKS", Fmt("(hand %d, look %.1f rad, out by %.3f)", hand, yw, dy));
                const V3 dirH = norm(add(mul(kF, cosf(yw)), mul(kR, sinf(yw))));
                Check(dot(sub(g_Ct[g_hand[hand]].p, g_Ct[g_uarm[hand]].p), dirH) > 18.0f, "rage: ...and the arm throws THAT way, even behind you", Fmt("(hand %d, look %.1f rad, %.0f cm)", hand, yw, dot(sub(g_Ct[g_hand[hand]].p, g_Ct[g_uarm[hand]].p), dirH)));
                Check(Finite() && WorstStretch() < 0.012f, "rage: ...without coming apart", Fmt("(hand %d, look %.1f rad)", hand, yw));
                Play(EM_RAGE, tHeld, hand);
                Check(dot(sub(g_Ct[g_hand[hand]].p, g_Ct[g_uarm[hand]].p), dirH) < 0.0f, "rage: held, the hand is cocked AWAY from where it is going", Fmt("(hand %d, look %.1f rad)", hand, yw));
                if (fabsf(yw) <= 1.0f) {
                    // the shoulders come round to it, and the head looks the rest of the way
                    // ...measured against the same wind-up aimed straight ahead (it has a twist of its own, to the
                    // throwing side). This frame is forward +y, RIGHT -x: a turn to the right is the positive one.
                    const V3 across = norm(sub(g_Ct[g_uarm[1]].p, g_Ct[g_uarm[0]].p));
                    const float turned = atan2f(dot(cross(acrossAhead, across), kU), dot(acrossAhead, across));
                    Check(turned * (yw > 0.0f ? 1.0f : -1.0f) > 0.15f, "rage: the shoulders turn toward the look", Fmt("(hand %d, look %.1f, turned %.2f)", hand, yw, turned));
                }
            }
            // the LOOK's height is the throw's: up lobs it, down throws it flat, never into the ground
            Look(0.3f, 0.5f);  Play(EM_RAGE, tThrough, hand); const float hi = throwW().z;
            Look(0.3f, -0.5f); Play(EM_RAGE, tThrough, hand); const float lo = throwW().z;
            Check(hi > lo + 0.3f && lo > 0.05f && hi < 0.75f, "rage: looking up lobs it, looking down throws it flat -- never at your feet", Fmt("(up %.2f, down %.2f)", hi, lo));
            // the body may sit turned in the world: the camera's say is in the WORLD's terms, and so is the answer
            g_meshYaw = 90.0f; Look(1.0f, 0.0f);
            Play(EM_RAGE, tThrough, hand);
            const V3 camFlat = norm(v3(g_camFwd[0], g_camFwd[1], 0.0f)), thrFlat = norm(v3(throwW().x, throwW().y, 0.0f));
            Check(dot(camFlat, thrFlat) > 0.999f, "rage: ...in the WORLD's terms, whichever way the body faces", Fmt("(%.3f)", dot(camFlat, thrFlat)));
            g_meshYaw = 0.0f;
            // FOLLOWED until it goes, and not after: a camera that swings away mid-throw does not bend the throw
            Look(0.8f, 0.0f); Play(EM_RAGE, tHeld, hand);
            const float y0 = g_rageYaw;
            Look(-0.8f, 0.0f);
            for (int k = 1; k <= 30; k++) { g_t = tHeld + k * 0.008f; Apply(g_mesh); }                       // a quarter of a second of looking the other way
            Check(g_rageYaw < y0 - 0.5f && g_rageYaw > -0.85f, "rage: held, the aim FOLLOWS the look -- smoothly, not in a frame", Fmt("(%.2f -> %.2f, look -0.8)", y0, g_rageYaw));
            const float y1 = g_rageYaw;
            Look(2.0f, 0.4f);
            for (int k = 1; k <= 30; k++) { g_t = kRageRelease + k * 0.008f; Apply(g_mesh); }
            Check(fabsf(g_rageYaw - y1) < 1e-4f, "rage: once it has gone, the look no longer steers it");
            // ...and a look swept right round BEHIND takes the body the short way, never a snap through the front
            Look(2.9f, 0.0f); Play(EM_RAGE, tHeld, hand);
            float worst = 0.0f, prev = g_rageTwist;
            for (int k = 1; k <= 60; k++) { Look(2.9f + k * 0.012f, 0.0f); g_t = 0.6f + k * 0.008f; Apply(g_mesh); worst = fabsf(g_rageTwist - prev) > worst ? fabsf(g_rageTwist - prev) : worst; prev = g_rageTwist; }
            Check(worst < 5.0f, "rage: a look passing behind you does not whip the body round in a frame", Fmt("(%.1f deg in one frame)", worst));
            Look(0.0f, 0.0f);
        }
    }

    // ---- THE CLAP. Hands apart, then together in front of the lower chest, palms to each other, fingers up and away;
    //      with a board in hand it is tucked under that arm (both hands are wanted) -- nose forward, grip tape to the
    //      ribs -- and the hands still meet.
    {
        const float tOpen = kClapStart, tMeet = kClapStart + kClapContact / kClapRate;
        Check(ClapGap(0.0f) > 0.999f && ClapGap(kClapContact) < 1e-4f && ClapGap(0.999f) > 0.95f, "clap: apart, together at the contact, apart again");
        { bool closing = true; for (float ph = 0.02f; ph < kClapContact; ph += 0.02f) if (ClapGap(ph) >= ClapGap(ph - 0.02f)) closing = false;
          Check(closing, "clap: ...closing all the way in"); }
        { int n = 0; float last = 0.0f;                                                       // the pump's own test for "the hands met", at 120 Hz
          for (float t = 0.0f; t < 2.0f; t += 1.0f / 120.0f) { const float ph = ClapPhase(t); if (t >= kClapStart && last < kClapContact && ph >= kClapContact) n++; last = ph; }
          Check(n == 5, "clap: heard once each time the hands meet", Fmt("(%d in 2 s)", n)); }
        for (int grip = 0; grip < 2; grip++)
            for (int hand = -1; hand <= 1; hand++) {
                g_grip = grip;
                char keep[64]; snprintf(keep, sizeof(keep), "(hand %d, grip %d)", hand, grip);
                Play(EM_CLAP, tOpen, hand);
                const float apart = len(sub(g_Ct[g_hand[0]].p, g_Ct[g_hand[1]].p));
                Play(EM_CLAP, tMeet, hand);
                const float together = len(sub(g_Ct[g_hand[0]].p, g_Ct[g_hand[1]].p));
                Check(apart > together + 12.0f && together < 9.5f, "clap: the hands come together", Fmt("%s %.1f -> %.1f cm between the wrists", keep, apart, together));
                Check(Finite() && WorstStretch() < 0.05f, "clap: finite and unstretched", keep);
                const V3 sh = mul(add(g_Ct[g_uarm[0]].p, g_Ct[g_uarm[1]].p), 0.5f);
                for (int sd = 0; sd < 2; sd++) {
                    const V3 w = g_Ct[g_hand[sd]].p;
                    Check(dot(sub(w, g_Ct[g_pelvis].p), kF) > 14.0f && w.z < sh.z - 12.0f && w.z > g_Ct[g_pelvis].p.z + 12.0f, "clap: in front of the lower chest", Fmt("%s side %d: %.0f ahead, %.0f up", keep, sd, dot(sub(w, g_Ct[g_pelvis].p), kF), w.z));
                    Check(fabsf(dot(sub(g_Ct[g_larm[sd]].p, g_Ct[g_uarm[sd]].p), kR)) < 12.0f, "clap: elbows by the ribs, not out like wings", Fmt("%s side %d: %.0f cm outside the shoulder", keep, sd, fabsf(dot(sub(g_Ct[g_larm[sd]].p, g_Ct[g_uarm[sd]].p), kR))));
                    V3 F, N, A; HandAxes(sd, &F, &N, &A);
                    Check(dot(N, kR) * (sd ? -1.0f : 1.0f) > 0.75f, "clap: palm to palm", Fmt("%s side %d %.2f", keep, sd, dot(N, kR)));
                    Check(dot(F, kU) > 0.35f && dot(F, kF) > 0.35f, "clap: fingers up and away", keep);
                }
                if (hand >= 0) {
                    const int flip = AnyNamed("skateskel_flipper");
                    const V3 top = norm(qrot(qmul(g_Ct[flip].q, qconj(g_C[flip].q)), g_trueTop));
                    const V3 a = g_Ct[g_truckF].p, b = g_Ct[g_truckB].p, midB = mul(add(a, b), 0.5f), side = mul(kR, hand ? 1.0f : -1.0f);
                    Check(dot(norm(sub(a, b)), kF) > 0.85f, "clap: the board is tucked nose forward", keep);
                    Check(dot(top, side) < -0.9f, "clap: ...grip tape to the ribs, graphic out", Fmt("%s %.2f", keep, dot(top, side)));
                    Check(len(sub(midB, g_Ct[g_uarm[hand]].p)) < 30.0f && midB.z < g_Ct[g_uarm[hand]].p.z - 10.0f, "clap: ...under the arm that carried it", Fmt("%s %.0f cm from the shoulder", keep, len(sub(midB, g_Ct[g_uarm[hand]].p))));
                    Check(g_boardPosed, "clap: ...and not left in a hand that has let go of it", keep);
                }
            }
        g_grip = GRIP_HANG;
    }
    // ---- EVERY EMOTE IS HELD UNTIL IT IS PUT AWAY. None ends on a clock; a minute in, each is still itself. B puts
    //      it away and that press does nothing else: the next B is whoever else wants it (the seat's next position).
    {
        for (int id = 0; id < EM_COUNT; id++) Check(kDefs[id].loop, "held: no emote ends by itself", kDefs[id].name);
        Play(EM_WAVE, 61.3f, -1);     Check(g_Ct[g_hand[1]].p.z > g_Ct[g_uarm[1]].p.z, "held: a minute on, still waving");
        Play(EM_THUMBS, 61.3f, -1);   Check(g_Ct[g_hand[1]].p.z > g_Ct[g_pelvis].p.z + 25.0f, "held: ...the thumb still up");
        Play(EM_FACEPALM, 61.3f, -1); Check(len(sub(g_Ct[g_hand[1]].p, g_Ct[g_head].p)) < 28.0f, "held: ...the hand still on the face");
        g_pumpMs = (LONGLONG)GetTickCount64();
        g_id = EM_WAVE; g_ending = false; g_w = 1.0f;
        Check(Emote_Stoppable() && !Emote_WantsTrigger(), "B: a held emote is B's to put away (and the trigger is nobody's)");
        Emote_Stop();
        Check(!Emote_Stoppable(), "B: ...once, and the next press is somebody else's");
        g_id = EM_RAGE; g_ending = false;
        Check(Emote_WantsTrigger() && Emote_Stoppable(), "rage: the trigger is its own while it is up; B still puts it away");
        {   // what the prompt bar says: B is on screen for as long as B is the emote's; an aiming Rage says what throws it
            SitPromptEntry e[2];
            g_thrown = false; g_rageSwingAt = -1.0f;
            int n = Emote_Prompts(e, 2);
            Check(n == 2 && e[0].button == 'T' && e[1].button == 'B' && !strcmp(e[1].label, "Cancel"), "prompts: an aiming Rage says the trigger throws and B cancels", Fmt("(%d)", n));
            g_rageSwingAt = 1.0f; g_thrown = true;
            n = Emote_Prompts(e, 2);
            Check(n == 1 && e[0].button == 'B', "prompts: ...thrown, only how to put it away");
            g_id = EM_CLAP; g_thrown = false;
            n = Emote_Prompts(e, 2);
            Check(n == 1 && e[0].button == 'B' && !strcmp(e[0].label, "Stop emote"), "prompts: any held emote shows B");
            g_ending = true;
            Check(Emote_Prompts(e, 2) == 0, "prompts: ...and not once it is on its way out");
            g_ending = false; g_id = EM_RAGE;
        }
        g_pumpMs = (LONGLONG)GetTickCount64() - 1000;
        { SitPromptEntry e[2]; Check(Emote_Prompts(e, 2) == 0, "prompts: no pump, no prompt"); }
        Check(!Emote_Stoppable() && !Emote_WantsTrigger() && !Emote_WantsStick(), "no pump, no buttons held: a stopped clock owns nothing");
        g_id = EM_NONE; g_ending = false; g_w = 0.0f;
    }

    // ---- A BOX CARRIED UNDER THE ARM, LONG WAYS (the radio). Whatever the mesh's own axes are, it is carried by its
    //      SHAPE: longest side forward, thinnest across against the ribs, under the arm that has no board in it, the
    //      hand beneath it -- and where its ACTOR belongs is handed over in the WORLD's terms, body turned or not.
    {
        Check(Emote_Count() == EM_WHEEL && EM_CARRY >= EM_WHEEL && !Emote_Name(EM_CARRY)[0], "carry: it is not on the wheel");
        const float shapes[3][6] = { { 30, 3, 2,  30, 14, 7 },      // long in X, thin in Z, its origin at a corner-ish
                                     {  0, 0, 0,   7, 14, 30 },     // long in Z (a tower), thin in X
                                     {  4, 20, 0,  14, 30, 7 } };   // long in Y
        for (int sh = 0; sh < 3; sh++)
            for (int hand = 0; hand <= 1; hand++)
                for (int yaw = 0; yaw <= 90; yaw += 90) {
                    for (int i = 0; i < 3; i++) { g_boxO[i] = shapes[sh][i]; g_boxE[i] = shapes[sh][3 + i]; }
                    g_meshYaw = (float)yaw;
                    Play(EM_CARRY, 1.0f, hand);
                    char keep[96]; snprintf(keep, sizeof(keep), "(shape %d, board in hand %d, body turned %d)", sh, hand, yaw);
                    const int fr = 1 - hand;                                           // the FREE arm carries
                    Check(g_boxSide == fr && g_boxWorldOk, "carry: the arm with no board in it carries", keep);
                    // back from the world's terms to the body's, to be judged there
                    const Q4 my = qaxis(kU, g_meshYaw);
                    const Q4 Rc = qmul(qconj(my), q4(g_boxQuatW[0], g_boxQuatW[1], g_boxQuatW[2], g_boxQuatW[3]));
                    const V3 origin = qrot(qconj(my), v3(g_boxPosW[0], g_boxPosW[1], g_boxPosW[2]));
                    const V3 centre = add(origin, qrot(Rc, v3(g_boxO[0], g_boxO[1], g_boxO[2])));
                    int iL = 0, iS = 0; for (int i = 1; i < 3; i++) { if (g_boxE[i] > g_boxE[iL]) iL = i; if (g_boxE[i] < g_boxE[iS]) iS = i; }
                    const int iM = 3 - iL - iS;
                    auto ax = [&](int i) { return qrot(Rc, v3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f)); };
                    Check(dot(ax(iL), kF) > 0.9f, "carry: its LONG side runs forward", Fmt("%s %.2f", keep, dot(ax(iL), kF)));
                    Check(fabsf(dot(ax(iS), kR)) > 0.9f, "carry: its THIN side lies across, flat to the ribs", Fmt("%s %.2f", keep, dot(ax(iS), kR)));
                    const V3 S = g_Ct[g_uarm[fr]].p, side = mul(kR, fr ? 1.0f : -1.0f);
                    const float top = centre.z + g_boxE[iM];
                    Check(top < S.z - 6.0f && top > S.z - 16.0f, "carry: its top is in the armpit", Fmt("%s %.1f cm under the shoulder", keep, S.z - top));
                    const float inner = dot(sub(centre, S), side) - g_boxE[iS];        // its inner face, against the shoulder's line
                    Check(inner > -9.0f && inner < -2.0f, "carry: its inner face is at the ribs, not in them nor out in the air", Fmt("%s %.1f cm", keep, inner));
                    const V3 w = g_Ct[g_hand[fr]].p;
                    Check(w.z < centre.z - g_boxE[iM] * 0.8f && dot(sub(w, centre), kF) > 0.0f, "carry: the hand is under it, ahead of its middle", keep);
                    Check(Finite() && WorstStretch() < 0.05f, "carry: finite and unstretched", keep);
                    Check(len(sub(g_Ct[g_pelvis].p, g_C[g_pelvis].p)) < 0.01f, "carry: the legs are left alone (you walk with it)", keep);
                }
        g_meshYaw = 0.0f;
        // while something is carried the wheel's emotes wait, and B is the prop's (put it down), not "stop emote"
        g_pumpMs = (LONGLONG)GetTickCount64();
        g_id = EM_CARRY; g_ending = false; g_w = 1.0f;
        Check(!Emote_Play(EM_WAVE) && strstr(Emote_WhyNot(), "down first"), "carry: the wheel's emotes wait until it is put down", Emote_WhyNot());
        SitPromptEntry e[2];
        Check(!Emote_Stoppable() && Emote_Prompts(e, 2) == 1 && e[0].button == 'B' && strstr(e[0].label, "down"), "carry: B is the radio's -- it is put down, not 'stopped'");
        g_id = EM_NONE; g_w = 0.0f;
    }

    // ---- the dance leaves the feet where the game put them, and moves the hips
    for (float t = 0.1f; t < 2.0f; t += 0.23f) {
        Play(EM_DANCE, t, -1);
        for (int sd = 0; sd < 2; sd++) {
            const V3 d = sub(g_Ct[g_foot[sd]].p, g_C[g_foot[sd]].p);
            Check(len(v3(d.x, d.y, 0.0f)) < 0.6f, "dance: the foot does not slide", Fmt("(%.2f cm at t=%.2f)", len(v3(d.x, d.y, 0.0f)), t));
            Check(d.z > -0.3f && d.z < 3.5f, "dance: the foot only lifts a little", Fmt("(%.2f cm)", d.z));
            const int toe = ChildNamed(g_foot[sd], "toe", sd);
            Check(dot(norm(sub(g_Ct[toe].p, g_Ct[g_foot[sd]].p)), norm(sub(g_C[toe].p, g_C[g_foot[sd]].p))) > 0.995f, "dance: the foot still points the way it did");
            Check(dot(sub(g_Ct[g_calf[sd]].p, g_Ct[g_thigh[sd]].p), kF) > -1.0f, "dance: the knee bends forward, not back");
        }
    }
    { Play(EM_DANCE, 0.25f, -1); Check(g_Ct[g_pelvis].p.z < g_C[g_pelvis].p.z - 1.0f, "dance: the hips dip on the beat"); }

    // ---- the named senses of rotation, which everything else leans on
    {
        BuildBody(-1, GRIP_HANG); LoadRig();
        void* fresh = MakeMesh();
        g_id = EM_WAVE; g_t = 0.5f; g_w = 0.0f; g_carry = -2; g_boardInHand = g_stubInHand = false; g_mesh = fresh;
        Apply(fresh);
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        BodyFrame(fresh);
        Check(dot(BF, kF) > 0.999f && dot(BR, kR) > 0.999f && dot(U, kU) > 0.999f, "the body frame is forward/right/up", Fmt("(BF %.2f %.2f %.2f)", BF.x, BF.y, BF.z));
        const V3 n0 = g_Ct[g_neck].p;
        SpineLean(20.0f, 0.0f, 0.0f);  Check(dot(sub(g_Ct[g_neck].p, n0), kF) > 3.0f, "spine pitch + leans FORWARD");
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        SpineLean(0.0f, 20.0f, 0.0f);  Check(dot(sub(g_Ct[g_neck].p, n0), kR) > 3.0f, "spine lean + goes RIGHT");
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        const V3 rs0 = g_Ct[g_uarm[1]].p;
        SpineLean(0.0f, 0.0f, 30.0f);  Check(dot(sub(g_Ct[g_uarm[1]].p, rs0), kF) < -2.0f, "spine yaw + turns RIGHT (the right shoulder goes back)");
        for (int i = 0; i < g_nBones; i++) { g_Ct[i] = g_C[i]; g_Lt[i] = g_L[i]; }
        TorsoFrame();
        const Q4 hq = HeadTurn(0.0f, 30.0f, 0.0f);
        Check(qrot(hq, mul(kF, 10.0f)).z < -3.0f, "head nod + looks DOWN");
        Check(dot(qrot(qaxis(tu, 30.0f), kF), kR) > 0.3f, "head yaw + looks RIGHT");
    }

    // ---- the claim the whole solver rests on: a bone's OWN orientation is nobody's business. Every pose,
    // on a body whose bones are each turned some arbitrary way, lands every bone where it did before.
    {
        static V3 was[MAX_BONES];
        float worst = 0.0f; const char* worstName = ""; int worstBone = -1;
        for (int grip = 0; grip < 2; grip++)
            for (int id = 0; id < EM_COUNT; id++)
                for (int hand = -1; hand <= 1; hand++)
                    for (float t = 0.2f; t < 2.0f; t += 0.61f) {
                        g_grip = grip; Look(id == EM_RAGE ? 0.3f : 0.0f, 0.0f);
                        g_tumbled = false; Play(id, t, hand);
                        for (int i = 0; i < g_nBones; i++) was[i] = g_Ct[i].p;
                        g_tumbled = true;  Play(id, t, hand);
                        for (int i = 0; i < g_nBones; i++) {
                            const float d = len(sub(g_Ct[i].p, was[i]));
                            if (d > worst) { worst = d; worstName = kDefs[id].name; worstBone = i; }
                        }
                        Check(Finite(), "finite on a tumbled rig", kDefs[id].name);
                    }
        g_tumbled = false; g_grip = GRIP_HANG;
        Check(worst < 0.02f, "bone orientations do not matter", Fmt("(worst %.4f cm: %s, %s)", worst, worstName, worstBone >= 0 ? g_bones[worstBone].name : "-"));
        printf("  tumbled rig: worst difference %.5f cm (%s)\n", worst, worstName);
    }
    // ---- ...and neither is the way the mesh component happens to sit in the world: a Point aimed by the
    // camera's WORLD direction must come out the same against the body whichever way the body faces.
    {
        const V3 look = norm(add(mul(kF, 0.8f), mul(kR, 0.6f)));              // ahead and to the right of the BODY
        static V3 was[MAX_BONES];
        g_meshYaw = 0.0f; g_camFwd[0] = look.x; g_camFwd[1] = look.y; g_camFwd[2] = look.z;
        Play(EM_POINT, 1.5f, -1);
        for (int i = 0; i < g_nBones; i++) was[i] = g_Ct[i].p;
        g_meshYaw = 117.0f;
        const V3 lw = qrot(qaxis(kU, g_meshYaw), look);                       // the same look, as the WORLD sees it now
        g_camFwd[0] = lw.x; g_camFwd[1] = lw.y; g_camFwd[2] = lw.z;
        Play(EM_POINT, 1.5f, -1);
        float worst = 0.0f;
        for (int i = 0; i < g_nBones; i++) { const float d = len(sub(g_Ct[i].p, was[i])); if (d > worst) worst = d; }
        Check(worst < 0.05f, "point: the body's facing in the world does not matter", Fmt("(worst %.3f cm)", worst));
        g_meshYaw = 0.0f; g_camFwd[0] = 0.0f; g_camFwd[1] = 1.0f; g_camFwd[2] = 0.0f;
    }

    printf("omp_emotetest: %d checks passed, %d failed\n", g_pass, g_fail);
    printf(g_fail ? "EMOTE TEST FAIL\n" : "EMOTE TEST PASS\n");
    return g_fail ? 1 : 0;
}
