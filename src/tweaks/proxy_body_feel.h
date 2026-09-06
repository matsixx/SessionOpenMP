// proxy_body_feel -- SessionTweaks' riding body on REMOTE players (SessionOpenMP proxies), driven by
// THEIR settings, so a peer moves on your screen the way they move on theirs.
//
// The bridge is a handful of exports on SessionOpenMP's main.dll, bound by name at runtime (the two
// mods are separate DLLs and either may be installed alone): this module hands over the local
// player's riding-body knobs, reads back each proxy's owner's, and asks whether the player wants
// peer body physics at all (Multiplayer -> Other options). With OpenMP absent, everything here is a
// no-op.
#pragma once
#include <cstdint>

// The settings vector as it travels: a fixed order of ints in the units the ini stores them in
// (percents and cm/s^2), so a value is a value on both ends. Append only -- a receiver on an older
// build reads the prefix it knows, a newer one fills what the sender did not send with defaults.
enum BfWire : int {
    BF_ON = 0,          // BodyFeel (the module's live toggle)
    BF_AMOUNT,          // BodyFeelAmount %
    BF_ARM_PCT,         // BodyFeelArmPct -- arm reactivity, scales the intent forces
    BF_ARM_LOOSE,       // BodyFeelArmLoosePct
    BF_ARM_HOLD,        // BodyFeelArmHoldPct
    BF_ARM_DAMP,        // BodyFeelArmDampPct
    BF_ARM_INERTIA,     // BodyFeelArmInertiaPct
    BF_ARM_SPREAD,      // BodyFeelArmSpread (cm/s^2)
    BF_ARM_LANDDROP,    // BodyFeelArmLandDrop (cm/s^2)
    BF_TORSO_ON,        // BodyFeelTorso
    BF_TORSO_LOOSE,     // BodyFeelTorsoLoosePct
    BF_TORSO_HOLD,      // BodyFeelTorsoHoldPct
    BF_TORSO_DAMP,      // BodyFeelTorsoDampPct
    BF_TORSO_LEAN,      // BodyFeelTorsoInertiaPct
    BF_HEAD_LOOSE,      // BodyFeelHeadLoosePct
    BF_HEAD_HOLD,       // BodyFeelHeadHoldPct
    BF_HEAD_DAMP,       // BodyFeelHeadDampPct
    BF_HEAD_LAG,        // BodyFeelHeadInertiaPct
    BF_COUNT
};
enum { kBfWireVer = 1 };

void ProxyBodyFeel_PumpFrame();                  // GAME THREAD, after BodyFeel_PumpFrame
void ProxyBodyFeel_PostPhysApply(void* skater);  // from foot_place's anim-update detour, non-local skaters
