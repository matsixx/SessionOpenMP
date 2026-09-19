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
// omp_posetest -- the RELEASE FADE's arithmetic.
//
// The bug this guards against is the one it was written for: watching someone's emote end, their body
// twitched. Measured on a shared clock, the viewer's skeleton moved up to 137 degrees on one bone in
// one frame while the sender moved 2. The cure is to fade the last transported pose out instead of
// dropping it -- and a fade has exactly two ways to reproduce the symptom it is curing:
//   * not being continuous with the stamp at w = 1 (a step at the START of the fade), and
//   * taking the long way round the quaternion hypersphere (the bone spins instead of settling).
// Both are checked here, plus the weight curve's endpoints and monotonicity.
// =====================================================================================================
#include "../../src/game/pose_blend.h"
#include <cstdio>
#include <cmath>

using namespace omp::game::pose;

static int g_checks = 0, g_fail = 0;
static void ok(bool c, const char* what) {
    g_checks++;
    if (!c) { g_fail++; printf("  FAIL: %s\n", what); }
}
static void nearly(float a, float b, float tol, const char* what) {
    g_checks++;
    if (!(fabsf(a - b) <= tol)) { g_fail++; printf("  FAIL: %s (%.6f vs %.6f)\n", what, a, b); }
}

static void norm4(float* q) {
    const float l = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    for (int k = 0; k < 4; k++) q[k] /= l;
}
// Quaternion for a rotation of `deg` about Z -- enough to reason about "which way did it go".
static void quatZ(float deg, float* q) {
    const float h = deg * 3.14159265f / 360.0f;
    q[0] = 0.0f; q[1] = 0.0f; q[2] = sinf(h); q[3] = cosf(h);
}
// Signed Z angle, degrees, -180..180.
static float zDeg(const float* q) {
    return atan2f(2.0f * (q[3]*q[2] + q[0]*q[1]), 1.0f - 2.0f * (q[1]*q[1] + q[2]*q[2])) * 57.2957795f;
}
static float dot4(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
}

int main() {
    printf("omp_posetest -- release-fade arithmetic\n");

    // ---- 1. THE WEIGHT CURVE.
    printf("[weight]\n");
    nearly(fadeWeight(0.0f), 1.0f, 1e-6f, "w(0) = 1: the fade starts holding the pose entirely");
    nearly(fadeWeight(1.0f), 0.0f, 1e-6f, "w(1) = 0: the fade ends with the graph owning the bone");
    nearly(fadeWeight(0.5f), 0.5f, 1e-6f, "w(0.5) = 0.5");
    ok(fadeWeight(-0.3f) == 1.0f, "w clamps below 0");
    ok(fadeWeight(1.7f) == 0.0f, "w clamps above 1");
    {   // strictly decreasing, and flat at both ends (no corner going in or coming out)
        float prev = fadeWeight(0.0f); bool mono = true;
        for (int i = 1; i <= 100; i++) { const float w = fadeWeight(i / 100.0f); if (w > prev) mono = false; prev = w; }
        ok(mono, "w is monotonically decreasing across the fade");
        ok(fabsf(fadeWeight(0.02f) - 1.0f) < 0.01f, "w leaves 1 gently (smoothstep, not linear)");
        ok(fabsf(fadeWeight(0.98f) - 0.0f) < 0.01f, "w arrives at 0 gently");
    }

    // ---- 2. CONTINUITY WITH THE STAMP. w = 1 must reproduce the kept pose EXACTLY: the hard stamp IS
    // this operation at w = 1, so the fade's first frame has to be what was already on screen.
    printf("[continuity]\n");
    {
        float graph[4], kept[4];
        quatZ(-30.0f, graph); quatZ(140.0f, kept);
        float q[4] = { graph[0], graph[1], graph[2], graph[3] };
        blendQuat(q, kept, 1.0f);
        for (int k = 0; k < 4; k++) nearly(q[k], kept[k], 1e-5f, "w=1 reproduces the kept rotation");
        float p[3] = { 1.0f, 2.0f, 3.0f };
        const float kp[3] = { -7.0f, 11.0f, 0.5f };
        blendVec3(p, kp, 1.0f);
        for (int k = 0; k < 3; k++) nearly(p[k], kp[k], 1e-5f, "w=1 reproduces the kept position");
    }
    {   // ...and w = 0 must leave the graph's own evaluation untouched.
        float graph[4]; quatZ(-30.0f, graph);
        float kept[4];  quatZ(140.0f, kept);
        float q[4] = { graph[0], graph[1], graph[2], graph[3] };
        blendQuat(q, kept, 0.0f);
        for (int k = 0; k < 4; k++) nearly(q[k], graph[k], 1e-5f, "w=0 leaves the graph rotation alone");
        float p[3] = { 1.0f, 2.0f, 3.0f };
        const float kp[3] = { -7.0f, 11.0f, 0.5f };
        blendVec3(p, kp, 0.0f);
        nearly(p[0], 1.0f, 1e-5f, "w=0 leaves the graph position alone");
        nearly(p[2], 3.0f, 1e-5f, "w=0 leaves the graph position alone (z)");
    }

    // ---- 3. SHORTEST ARC. The failure mode is a bone SPINNING the long way round rather than
    // settling -- a worse twitch than the one being cured. Kept at +170, graph at -170: the short way
    // is 20 degrees ACROSS +/-180, not 340 degrees back through zero.
    printf("[shortest arc]\n");
    {
        float kept[4]; quatZ(170.0f, kept);
        float graph[4]; quatZ(-170.0f, graph);
        // Sample the whole fade; every step must stay in the 20-degree corridor near +/-180.
        bool inCorridor = true;
        for (int i = 0; i <= 20; i++) {
            float q[4] = { graph[0], graph[1], graph[2], graph[3] };
            blendQuat(q, kept, fadeWeight(i / 20.0f));
            const float a = zDeg(q);
            if (fabsf(a) < 169.0f) inCorridor = false;   // it went back through zero = the long way
        }
        ok(inCorridor, "an antipodal pair blends the SHORT way (never through zero)");
    }
    {   // The sign fix is what does it: with kept negated, the result must be identical.
        float kept[4]; quatZ(140.0f, kept);
        float keptNeg[4]; for (int k = 0; k < 4; k++) keptNeg[k] = -kept[k];
        float graph[4]; quatZ(-30.0f, graph);
        float a[4] = { graph[0], graph[1], graph[2], graph[3] };
        float b[4] = { graph[0], graph[1], graph[2], graph[3] };
        blendQuat(a, kept, 0.5f);
        blendQuat(b, keptNeg, 0.5f);
        for (int k = 0; k < 4; k++) nearly(a[k], b[k], 1e-5f, "negating the kept quaternion changes nothing");
    }

    // ---- 4. THE WHOLE FADE IS SMOOTH. This is the property the field bug violated: no single frame
    // of the hand-over may be a big step. Run a 250 ms fade at 60 fps between two rotations a long way
    // apart and assert no frame moves more than a few degrees.
    printf("[no step]\n");
    {
        float kept[4]; quatZ(137.0f, kept);          // the worst single-frame jump measured in the field
        float graph[4]; quatZ(0.0f, graph);
        const int   frames = 15;                      // 250 ms at 60 fps
        float prev[4] = { kept[0], kept[1], kept[2], kept[3] };   // the fade starts ON the kept pose
        float worst = 0.0f;
        for (int i = 0; i <= frames; i++) {
            float q[4] = { graph[0], graph[1], graph[2], graph[3] };
            blendQuat(q, kept, fadeWeight((float)i / (float)frames));
            float d = fabsf(zDeg(q) - zDeg(prev));
            if (d > 180.0f) d = 360.0f - d;
            if (d > worst) worst = d;
            for (int k = 0; k < 4; k++) prev[k] = q[k];
        }
        printf("  worst single frame over a 137-degree, 250 ms fade: %.1f deg\n", worst);
        ok(worst < 20.0f, "no frame of the fade steps more than 20 degrees");
        ok(worst > 0.5f,  "...and the fade does actually move (not a frozen pose)");
    }
    {   // The first frame specifically: this is the one that used to be the snap.
        float kept[4]; quatZ(137.0f, kept);
        float graph[4]; quatZ(0.0f, graph);
        float q[4] = { graph[0], graph[1], graph[2], graph[3] };
        blendQuat(q, kept, fadeWeight(0.0f));
        nearly(zDeg(q), 137.0f, 0.1f, "frame 1 of the fade is exactly what was on screen");
    }

    // ---- 5. UNIT LENGTH. A denormalised quaternion reaching the engine is a scaled bone.
    printf("[normalisation]\n");
    {
        bool unit = true;
        float kept[4]; quatZ(90.0f, kept);
        float graph[4]; quatZ(-90.0f, graph);
        for (int i = 0; i <= 20; i++) {
            float q[4] = { graph[0], graph[1], graph[2], graph[3] };
            blendQuat(q, kept, fadeWeight(i / 20.0f));
            if (fabsf(dot4(q, q) - 1.0f) > 1e-4f) unit = false;
        }
        ok(unit, "every blended rotation comes out unit length");
    }
    {   // Exactly antipodal at w = 0.5 cancels to zero: keep what is there rather than emit garbage.
        float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float kept[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
        blendQuat(q, kept, 0.5f);
        ok(fabsf(dot4(q, q) - 1.0f) < 1e-4f, "a degenerate blend still leaves a unit quaternion");
    }

    printf("\nomp_posetest: %d checks passed, %d failed\n", g_checks - g_fail, g_fail);
    if (g_fail) { printf("POSE TEST FAIL\n"); return 1; }
    printf("POSE TEST PASS\n");
    return 0;
}
