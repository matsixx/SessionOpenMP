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
// =====================================================================================================
// SessionOpenMP -- the RELEASE FADE's arithmetic, on its own so a gate can reach it.
//
// Letting go of a transported pose used to be a step function: the peer's skeleton one frame, the
// proxy's own anim graph at full strength the next. The graph has been running underneath the whole
// time and is nowhere near the posed skeleton, so that hand-over is a visible snap -- measured in the
// field 2026-09-19 at 48-137 degrees on a single bone in a single frame, against ~2 degrees on the
// SENDER at the same instant. That asymmetry is the whole bug: it cannot be seen by the person doing
// the emote, only by everyone watching them.
//
// So the last stamped pose is faded out instead. Two properties matter and both are gated
// (omp_posetest), because getting either wrong reproduces the symptom being fixed:
//   * CONTINUITY -- w = 1 must reproduce the kept pose EXACTLY. The hard stamp is this operation at
//     w = 1, so the first fade frame has to be bit-for-bit what was already on screen. Anything else
//     is a step at the START of the fade instead of the end of it.
//   * SHORTEST ARC -- a quaternion and its negation are the same rotation. Blending without the sign
//     fix goes the long way round the hypersphere: the bone SPINS instead of settling, which is a
//     worse twitch than the one being cured.
// =====================================================================================================
#pragma once
#include <cmath>

namespace omp { namespace game { namespace pose {

// Ease the weight so the fade leaves and arrives without a corner. u = 0..1 elapsed fraction.
inline float fadeWeight(float u) {
    if (u <= 0.0f) return 1.0f;
    if (u >= 1.0f) return 0.0f;
    return 1.0f - (u * u * (3.0f - 2.0f * u));      // 1 -> 0, smoothstep
}

// Blend the kept rotation over the graph's, in place. w = 1 keeps, w = 0 leaves `q` untouched.
inline void blendQuat(float* q, const float* kq, float w) {
    float d = q[0] * kq[0] + q[1] * kq[1] + q[2] * kq[2] + q[3] * kq[3];
    const float s = d < 0.0f ? -1.0f : 1.0f;        // shortest arc; see the header note
    float r[4];
    for (int k = 0; k < 4; k++) r[k] = q[k] * (1.0f - w) + kq[k] * s * w;
    const float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (len <= 1e-6f) return;                       // antipodal and w = 0.5: keep what is there
    const float inv = 1.0f / len;
    for (int k = 0; k < 4; k++) q[k] = r[k] * inv;
}

inline void blendVec3(float* p, const float* kp, float w) {
    for (int k = 0; k < 3; k++) p[k] = p[k] * (1.0f - w) + kp[k] * w;
}

}}} // namespace omp::game::pose
