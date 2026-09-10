// Teleport to a player -- see teleport.h for why this goes through the game's own marker return.
#include "teleport.h"
#include "game_syms.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace game {

float teleportBehindCm = 150.0f;

#ifdef _WIN32
namespace {

bool readBytes(const void* p, void* out, int n) {
    __try { memcpy(out, p, (size_t)n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// An actor's world transform, off its root component. Both are plain reads of a cached
// ComponentToWorld, which is what every other position read in this mod uses.
bool actorTransform(void* actor, float pos[3], float quat[4]) {
    if (!actor) return false;
    void* root = nullptr;
    if (!readBytes((const uint8_t*)actor + off::kActorRootComp, &root, sizeof(root)) || !root) return false;
    if (!readBytes((const uint8_t*)root + off::kCompPos,  pos,  12)) return false;
    if (!readBytes((const uint8_t*)root + off::kCompQuat, quat, 16)) return false;
    // A quat that is not a rotation means the read landed somewhere wrong; better to decline than to
    // hand the marker path a target that puts a skater into the floor.
    const float n = quat[0]*quat[0] + quat[1]*quat[1] + quat[2]*quat[2] + quat[3]*quat[3];
    return n > 0.5f && n < 1.5f;
}

// Forward (+X) of a quaternion, and its yaw in degrees -- the two things a marker needs that a
// quaternion does not hand over directly.
void quatForward(const float q[4], float out[3]) {
    out[0] = 1.f - 2.f * (q[1]*q[1] + q[2]*q[2]);
    out[1] =       2.f * (q[0]*q[1] + q[3]*q[2]);
    out[2] =       2.f * (q[0]*q[2] - q[3]*q[1]);
}
float quatYawDeg(const float q[4]) {
    const float s = 2.f * (q[3]*q[2] + q[0]*q[1]);
    const float c = 1.f - 2.f * (q[1]*q[1] + q[2]*q[2]);
    return atan2f(s, c) * 57.2957795f;
}

}  // namespace

bool TeleportLocalToProxy(void* ownPawn, void* proxyActor, void* proxyBoard, bool onBoard,
                          void (*logf)(const char*)) {
    if (!ownPawn || !proxyActor) return false;
    // The marker fields are ASkaterCharacter's, so a pawn that is not one would take these writes
    // somewhere else entirely. Checked by CLASS rather than by "it looks like a skater".
    if (!IsObjectOfClass(ownPawn, "SkaterCharacter")) {
        if (logf) logf("[teleport] your pawn is not a SkaterCharacter -- the marker return does not "
                       "apply to it, so nothing was moved");
        return false;
    }

    float sPos[3], sQuat[4];
    if (!actorTransform(proxyActor, sPos, sQuat)) {
        if (logf) logf("[teleport] could not read where they are standing -- nothing moved");
        return false;
    }
    float bPos[3], bQuat[4];
    const bool haveBoard = actorTransform(proxyBoard, bPos, bQuat);

    // Land BEHIND them rather than inside them. Arriving in the same cubic metre as another skater
    // shoves whichever of you the solver likes least.
    float fwd[3]; quatForward(sQuat, fwd);
    const float back[3] = { -fwd[0] * teleportBehindCm,
                            -fwd[1] * teleportBehindCm,
                            0.f };                       // never vertical: their Z is the ground here

    uint8_t mk[off::kMarkerInfoSize];
    memset(mk, 0, sizeof(mk));
    mk[off::kMkIsSet]         = 1;
    mk[off::kMkIsOnBoard]     = onBoard ? 1 : 0;
    mk[off::kMkBoardReversed] = 0;
    mk[off::kMkFootPosition]  = 0;

    float loc[3] = { sPos[0] + back[0], sPos[1] + back[1], sPos[2] + back[2] };
    memcpy(mk + off::kMkSkaterLoc, loc, 12);
    memcpy(mk + off::kMkSkaterRot, sQuat, 16);
    // The board goes with them. Without a board position the marker return puts one at the origin.
    if (haveBoard) {
        const float bl[3] = { bPos[0] + back[0], bPos[1] + back[1], bPos[2] + back[2] };
        memcpy(mk + off::kMkBoardLoc, bl, 12);
        memcpy(mk + off::kMkBoardRot, bQuat, 16);
    } else {
        memcpy(mk + off::kMkBoardLoc, loc, 12);
        memcpy(mk + off::kMkBoardRot, sQuat, 16);
    }
    // Look where they are looking. FRotator is (Pitch, Yaw, Roll); pitch and roll stay level, because
    // inheriting someone else's camera pitch on arrival is disorienting.
    const float yaw = quatYawDeg(sQuat);
    const float rot[3] = { 0.f, yaw, 0.f };
    memcpy(mk + off::kMkControlRot,    rot, 12);
    memcpy(mk + off::kMkCameraBoomRot, rot, 12);
    // MoveDirection stays zero: arriving with a direction is how you arrive already moving.

    __try {
        memcpy((uint8_t*)ownPawn + off::kSkaterPendingMarker, mk, sizeof(mk));
        uint8_t* flag = (uint8_t*)ownPawn + off::kSkaterGotoMarkerFlag;
        *flag = (uint8_t)(*flag | 1);          // the game's own tick picks it up on the next frame
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (logf) logf("[teleport] writing the pending marker faulted -- nothing moved");
        return false;
    }
    if (logf) {
        char m[190];
        snprintf(m, sizeof(m), "[teleport] going to them: (%.0f,%.0f,%.0f) facing %.0f deg, %s",
                 loc[0], loc[1], loc[2], yaw, onBoard ? "on their board" : "on foot");
        logf(m);
    }
    return true;
}

#else
bool TeleportLocalToProxy(void*, void*, void*, bool, void (*)(const char*)) { return false; }
#endif

}} // namespace omp::game
