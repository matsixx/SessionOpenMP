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
//
// MAX DETAIL -- quality past the end of the game's own sliders.
//
// The game's highest preset is not the engine's. Session ships scalability groups whose "Epic" row is
// a sensible 2019 console-adjacent target, while the renderer underneath will happily do considerably
// more: the high-quality paths are already compiled into the shipped exe and merely turned down. So
// there is nothing to build here -- only values to raise, through the console the upscale module
// already owns.
//
// WHY IT IS A LOOP AND NOT A ONE-SHOT. Every one of these is a plain console variable, and the game's
// own settings screen re-applies its scalability groups over the top of them (opening the options
// menu, changing any graphics setting, a level load). A value set once is a value that quietly goes
// back. The upscale module solved that for the render scale by re-applying on a timer; this rides the
// same idea, and for the same reason.
//
// WHAT IS DELIBERATELY NOT HERE:
//   * Mesh distance fields (r.DistanceFieldAO, distance field shadows). Those need data generated at
//     COOK time, and no console variable can conjure it into shipped content. The symbols are in the
//     exe; the data almost certainly is not. Setting them would cost frames and change nothing.
//   * Ray tracing. Not because it is impossible -- the whole RT renderer IS in this exe (the D3D12
//     backend with acceleration structures and shader tables, and the reflection, GI, AO and shadow
//     passes), so the engine was built with it. But r.RayTracing is read ONCE at RHI init, which makes
//     it a launch-config question rather than a settings row, and whether the cooked content carries
//     RT shaders for its materials is decided at cook time and cannot be seen from here.
//   * Anything that needs a shader the cooked pipeline does not contain.
//   * Screen space global illumination. Tried and REMOVED: it switches the ambient occlusion off (the
//     engine's own doing -- with SSGI on, lighting goes through the diffuse-indirect composite, which
//     takes over from the SSAO pass) and gave nothing back but broken-looking spots. Session is
//     outdoor daylight nearly everywhere, the worst case for screen-space bounce.
//     found, written, render state marked dirty -- and the effect was real; it just never looked
//     right. Shortening the ray and restricting it to the sun each helped and neither was enough: it
//     thickened shadows where a shadow had no business being. A screen-space ray march cannot know
//     what is behind what it can see, and on this geometry that shows.
//
// Every group is INDEPENDENT and 0 always means "the game's own setting, untouched" -- so a player can
// take the sharpness (nearly free) without the draw distance (the expensive one). Turning a group back
// to 0 restores the game's value rather than leaving ours behind: see kOff below.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "max_detail.h"
#include "tweaks_common.h"
#include "upscale.h"          // Upscale_Console -- the engine's console, already resolved there
#include "ui/menu_ext.h"

// ------------------------------------------------------------------ settings
// 0 = the game's own, then rising. Off by default, every one of them: this is opt-in, and a mod that
// silently costs somebody frames on first launch has made their decision for them.
static int g_textures    = 0;   // MaxDetailTextures    0..3
static int g_shadows     = 0;   // MaxDetailShadows     0..3
static int g_distance    = 100; // MaxDetailDistancePct 100..400 (100 = the game's own)
static int g_reflections = 0;   // MaxDetailReflections 0..2
static int g_fog         = 0;   // MaxDetailFog         0..2
static int g_holdMs      = 3000;// MaxDetailHoldMs -- how often the values are re-asserted

static char g_status[192] = "off";
static int  g_applied = 0;      // how many variables the last pass set
static uint64_t g_lastMs = 0;
static bool g_dirty = true;     // a setting changed: apply now rather than at the next beat

// ------------------------------------------------------------------ the variables
// One row per console variable, per level. Index 0 of each list is what a group set to 0 sends -- the
// value the game would have had -- so turning a group off actively restores rather than just stopping.
struct Var { const char* name; const char* v[4]; };

// TEXTURE SHARPNESS. The best value-for-money on this game by some distance: a skateboarding camera
// looks ALONG the ground almost all the time, which is exactly the grazing angle anisotropic filtering
// exists for, and the streaming pool is what decides whether you are looking at the real texture or a
// low mip that never got replaced.
static const Var kTextures[] = {
    { "r.MaxAnisotropy",              { "8",    "16",   "16",   "16"   } },
    { "r.Streaming.PoolSize",         { "1000", "2000", "3000", "4000" } },   // MB; the game ships ~1 GB
    { "r.Streaming.MipBias",          { "0",    "0",    "-0.5", "-1"   } },
    { "r.Streaming.LimitPoolSizeToVRAM", { "1", "0",    "0",    "0"    } },
    { "r.Streaming.MaxEffectiveScreenSize", { "0", "0", "0",    "0"    } },
};
// SHADOWS. Session's are visibly coarse: the cascade resolution is modest and small objects drop out
// of the shadow pass entirely, which on a spot full of rails and ledges is most of the geometry.
static const Var kShadows[] = {
    { "r.ShadowQuality",              { "3",    "5",    "5",    "5"    } },
    { "r.Shadow.MaxResolution",       { "2048", "2048", "4096", "4096" } },
    { "r.Shadow.MaxCSMResolution",    { "2048", "2048", "4096", "4096" } },
    { "r.Shadow.CSM.MaxCascades",     { "3",    "4",    "4",    "5"    } },
    { "r.Shadow.DistanceScale",       { "1",    "1.5",  "2",    "3"    } },
    { "r.Shadow.RadiusThreshold",     { "0.03", "0.01", "0.005","0.0"  } },   // 0 = even the small stuff casts
    { "r.Shadow.CSM.TransitionScale", { "1",    "1",    "1",    "1"    } },
};
// DRAW DISTANCE. The expensive one, and the only group here that is mostly CPU: it is draw calls, not
// pixels. Sent as a percentage so the slider reads in units that mean something.
static const Var kDistance[] = {
    { "r.ViewDistanceScale",          { nullptr } },   // filled per-value below
    { "foliage.LODDistanceScale",     { nullptr } },
    { "r.StaticMeshLODDistanceScale", { nullptr } },
};
// AMBIENT OCCLUSION IS GONE FROM THIS PAGE, and the reason is worth keeping.
//
// It went through two versions and neither earned its place. The first raised
// r.AmbientOcclusionLevels and switched the estimator to GTAO: both are LOOK changes wearing a quality
// label (Levels is how many mip levels the occlusion gathers over, so it widens the radius; GTAO has
// its own intensity response, and this game's AO intensity and radius are authored against the SSAO
// curve). Field verdict: "darkens everything in a way it shouldn't". The second version was honest --
// quality ceiling and the compute path only -- and the field verdict was that it did not look
// meaningfully better than the game's own. So it is removed rather than kept as a row that costs
// something and returns nothing visible. The game's ambient occlusion is the game's.
// SCREEN SPACE REFLECTIONS. Wet ground and polished concrete are most of what a skate spot is made of.
static const Var kReflections[] = {
    { "r.SSR.Quality",                { "3",    "4",    "4"    } },
    { "r.SSR.MaxRoughness",           { "-1",   "0.8",  "1.0"  } },
    { "r.SSR.Temporal",               { "0",    "1",    "1"    } },
    { "r.SSR.HalfResSceneColor",      { "1",    "0",    "0"    } },
};
// VOLUMETRIC FOG. Grid resolution, which is what decides whether a shaft of light has an edge or a
// staircase. Cheap in pixels, not free in memory.
static const Var kFog[] = {
    { "r.VolumetricFog.GridPixelSize", { "8",   "6",    "4"    } },
    { "r.VolumetricFog.GridSizeZ",     { "64",  "96",   "128"  } },
    { "r.VolumetricFog.HistoryMissSupersampleCount", { "4", "8", "16" } },
};

static bool send(const char* name, const char* value) {
    char c[128];
    snprintf(c, sizeof(c), "%s %s", name, value);
    return Upscale_Console(c);
}
static int sendGroup(const Var* vars, int n, int level) {
    int sent = 0;
    for (int i = 0; i < n; i++) if (vars[i].v[level] && send(vars[i].name, vars[i].v[level])) sent++;
    return sent;
}

// ------------------------------------------------------------------ apply
static void Apply() {
    int n = 0;
    n += sendGroup(kTextures,    (int)(sizeof(kTextures)    / sizeof(kTextures[0])),    g_textures);
    n += sendGroup(kShadows,     (int)(sizeof(kShadows)     / sizeof(kShadows[0])),     g_shadows);
    n += sendGroup(kReflections, (int)(sizeof(kReflections) / sizeof(kReflections[0])), g_reflections);
    n += sendGroup(kFog,         (int)(sizeof(kFog)         / sizeof(kFog[0])),         g_fog);
    {   // the distance group is a scale, not a level: one number, three variables. The mesh LOD scale
        // runs the OTHER way (smaller = higher LODs held further out), which is why it is not simply
        // the same value three times.
        const float s = (float)g_distance * 0.01f;
        char v[32];
        snprintf(v, sizeof(v), "%.2f", s);                    if (send("r.ViewDistanceScale", v)) n++;
        snprintf(v, sizeof(v), "%.2f", s);                    if (send("foliage.LODDistanceScale", v)) n++;
        snprintf(v, sizeof(v), "%.2f", s > 0.01f ? 1.0f / s : 1.0f);
        if (send("r.StaticMeshLODDistanceScale", v)) n++;
    }
    g_applied = n;
    const bool any = g_textures || g_shadows || g_reflections || g_fog || g_distance != 100;
    snprintf(g_status, sizeof(g_status),
             any ? "textures %d, shadows %d, distance %d%%, reflections %d, fog %d (%d variables held)"
                 : "off -- the game's own settings (%d %d %d%% %d %d, %d restored)",
             g_textures, g_shadows, g_distance, g_reflections, g_fog, n);
}

void MaxDetail_PumpFrame() {
    const uint64_t ms = GetTickCount64();
    if (!g_dirty && ms - g_lastMs < (uint64_t)(g_holdMs > 250 ? g_holdMs : 250)) return;
    // Nothing to hold and nothing to restore: stay silent rather than sending a dozen commands a
    // second for a player who never turned this on.
    static bool everOn = false;
    const bool any = g_textures || g_shadows || g_reflections || g_fog || g_distance != 100;
    if (!any && !everOn && !g_dirty) { g_lastMs = ms; return; }
    if (any) everOn = true;
    g_lastMs = ms;
    const bool wasDirty = g_dirty;
    g_dirty = false;
    Apply();
    if (wasDirty) TwkLog("[detail] %s", g_status);
    if (!any) everOn = false;      // restored once; now there is nothing left to hold
}

// ------------------------------------------------------------------ shell surface
static void set(int* dst, int v, int lo, int hi) {
    if (v < lo) v = lo; if (v > hi) v = hi;
    if (*dst == v) return;
    *dst = v; g_dirty = true; TwkMarkDirty();
}
int  MaxDetail_Textures()          { return g_textures; }
void MaxDetail_SetTextures(int v)  { set(&g_textures, v, 0, 3); }
int  MaxDetail_Shadows()           { return g_shadows; }
void MaxDetail_SetShadows(int v)   { set(&g_shadows, v, 0, 3); }
int  MaxDetail_Distance()          { return g_distance; }
void MaxDetail_SetDistance(int v)  { set(&g_distance, v, 100, 400); }
int  MaxDetail_Reflections()       { return g_reflections; }
void MaxDetail_SetReflections(int v){ set(&g_reflections, v, 0, 2); }
int  MaxDetail_Fog()               { return g_fog; }
void MaxDetail_SetFog(int v)       { set(&g_fog, v, 0, 2); }
const char* MaxDetail_Status()     { return g_status; }
void MaxDetail_ApplyPreset() {
    set(&g_textures, 3, 0, 3); set(&g_shadows, 3, 0, 3); set(&g_distance, 250, 100, 400);
    set(&g_reflections, 2, 0, 2); set(&g_fog, 2, 0, 2);
    TwkLog("[detail] everything to maximum");
}

void MaxDetail_ReadConfig(const char* buf) {
    g_textures    = TwkIniInt(buf, "MaxDetailTextures", 0);
    g_shadows     = TwkIniInt(buf, "MaxDetailShadows", 0);
    g_distance    = TwkIniInt(buf, "MaxDetailDistancePct", 100);
    g_reflections = TwkIniInt(buf, "MaxDetailReflections", 0);
    g_fog         = TwkIniInt(buf, "MaxDetailFog", 0);
    g_holdMs      = TwkIniInt(buf, "MaxDetailHoldMs", 3000);
    if (g_textures < 0 || g_textures > 3) g_textures = 0;
    if (g_shadows  < 0 || g_shadows  > 3) g_shadows  = 0;
    if (g_reflections < 0 || g_reflections > 2) g_reflections = 0;
    if (g_fog < 0 || g_fog > 2) g_fog = 0;
    if (g_distance < 100 || g_distance > 400) g_distance = 100;
    g_dirty = true;
    TwkLog("[detail] config: textures=%d shadows=%d distance=%d%% reflections=%d fog=%d",
           g_textures, g_shadows, g_distance, g_reflections, g_fog);
}
void MaxDetail_SaveConfig(char* buf, size_t cap) {
    TwkIniSetInt(buf, cap, "MaxDetailTextures",    g_textures);
    TwkIniSetInt(buf, cap, "MaxDetailShadows",     g_shadows);
    TwkIniSetInt(buf, cap, "MaxDetailDistancePct", g_distance);
    TwkIniSetInt(buf, cap, "MaxDetailReflections", g_reflections);
    TwkIniSetInt(buf, cap, "MaxDetailFog",         g_fog);
    TwkIniSetInt(buf, cap, "MaxDetailHoldMs",      g_holdMs);
}
void MaxDetail_ResetDefaults() {
    g_textures = g_shadows = g_reflections = g_fog = 0;
    g_distance = 100; g_holdMs = 3000; g_dirty = true;
}

void MaxDetail_DrawMenu(const OmpMenuApi* api) {
    if (!api) return;
    api->TextDisabled("Quality past the end of the game's own sliders. Each is independent; 0 or 100% is the game's own.");
    float v = (float)g_textures;
    if (api->SliderFloat("Texture sharpness", &v, 0.0f, 3.0f, "%.0f")) MaxDetail_SetTextures((int)v);
    v = (float)g_shadows;
    if (api->SliderFloat("Shadows", &v, 0.0f, 3.0f, "%.0f")) MaxDetail_SetShadows((int)v);
    v = (float)g_distance;
    if (api->SliderFloat("Draw distance (%)", &v, 100.0f, 400.0f, "%.0f")) MaxDetail_SetDistance((int)v);
    v = (float)g_reflections;
    if (api->SliderFloat("Reflections", &v, 0.0f, 2.0f, "%.0f")) MaxDetail_SetReflections((int)v);
    v = (float)g_fog;
    if (api->SliderFloat("Volumetric fog", &v, 0.0f, 2.0f, "%.0f")) MaxDetail_SetFog((int)v);
    if (api->Button("Everything to maximum")) MaxDetail_ApplyPreset();
    api->TextDisabled(g_status);
}
