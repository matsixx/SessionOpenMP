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
// SessionOpenMP -- floating player names. Contract and thread rule: nameplates.h.
#include "nameplates.h"
#include "theme.h"
#include "imgui.h"
#include <windows.h>
#include <mutex>
#include <string.h>
#include <float.h>

namespace {

NameplateTuning g_tune;

const int kMaxPlates = 16;          // the lobby cap; a plate per peer is the most there can ever be

std::mutex    g_mx;
NameplateItem g_items[kMaxPlates];
int           g_n = 0;
bool          g_show = false;
uint64_t      g_atMs = 0;

// The published set goes STALE rather than persisting. The game thread stops publishing when the mod
// disarms, when the pawn goes away, or when a level is loading -- and a plate left hanging over a
// world that no longer exists is worse than no plate at all.
const uint64_t kStaleMs = 400;

float g_alpha = 0.0f;               // RENDER THREAD ONLY -- the on/off-board fade
bool  g_anyBubble = false;          // is anybody mid-sentence? bubbles ignore the on/off-board fade,
                                    // so they need their own reason to keep the frame rendering

uint64_t nowMs() { return GetTickCount64(); }

// Whole pixels. A glyph quad landing on a fractional pixel is resampled every frame, and a position
// that slides smoothly across sub-pixel phases makes the text shimmer -- which reads as jitter even
// though the position is perfectly smooth. Snapping costs nothing and is why ImGui floors its own
// window positions.
float snap(float v) { return (float)(int)(v + (v >= 0.0f ? 0.5f : -0.5f)); }

// Deliberately about TIME only, not about how many plates are in the set. A peer walking behind you
// empties the list for a moment, and if that counted as "nothing to show" the fade would start
// running backwards -- so turning around would re-fade a plate that never should have gone anywhere.
// The item count decides what is DRAWN; the publish decides what the fade is aiming at.
bool freshLocked() { return g_atMs != 0 && (nowMs() - g_atMs) <= kStaleMs; }

} // namespace

NameplateTuning& Nameplates_Tuning() { return g_tune; }

void Nameplates_Publish(const NameplateItem* items, int n, bool show) {
    if (n < 0) n = 0;
    if (n > kMaxPlates) n = kMaxPlates;
    std::lock_guard<std::mutex> lk(g_mx);
    if (items && n > 0) memcpy(g_items, items, sizeof(NameplateItem) * (size_t)n);
    g_n = n;
    g_show = show;
    g_atMs = nowMs();
    g_anyBubble = false;
    for (int i = 0; i < n; i++) if (g_items[i].msg[0]) { g_anyBubble = true; break; }
}

bool Nameplates_HasVisible() {
    if (!g_tune.enabled) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!freshLocked()) return g_alpha > 0.01f;   // still fading out: those frames must keep rendering
    return g_show || g_alpha > 0.01f || (g_tune.bubbles && g_anyBubble);
}

void Nameplates_Draw() {
    if (!g_tune.enabled) return;

    // Snapshot under the lock, draw outside it -- the game thread must never wait on a frame.
    NameplateItem items[kMaxPlates];
    int  n = 0;
    bool show = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const bool fresh = freshLocked();
        if (fresh && g_n > 0) { n = g_n; memcpy(items, g_items, sizeof(NameplateItem) * (size_t)n); }
        show = fresh && g_show;
    }

    // The fade. Driven by the render thread's own frame time so it is smooth regardless of what the
    // publish rate happens to be, and it runs even with nothing to draw so the alpha is already
    // correct the moment a peer appears.
    ImGuiIO& io = ImGui::GetIO();
    const float dt = (io.DeltaTime > 0.0f && io.DeltaTime < 0.25f) ? io.DeltaTime : (1.0f / 60.0f);
    const float target = show ? 1.0f : 0.0f;
    const float step = (g_tune.fadeSec > 0.01f) ? (dt / g_tune.fadeSec) : 1.0f;
    if (g_alpha < target)      g_alpha = (g_alpha + step > target) ? target : g_alpha + step;
    else if (g_alpha > target) g_alpha = (g_alpha - step < target) ? target : g_alpha - step;
    if (n <= 0) return;                       // (the fade above still had to run)

    // Far first, so a nearer name overlaps a further one rather than the other way round. n is at most
    // the lobby cap, so the simplest sort that is obviously correct is the right one.
    for (int i = 1; i < n; i++) {
        NameplateItem key = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].distCm < key.distCm) { items[j + 1] = items[j]; j--; }
        items[j + 1] = key;
    }

    const ThemePalette& T = Theme();
    // BACKGROUND, not foreground: it still draws over the game (everything ImGui draws does), but it
    // draws UNDER our own windows, so the chat box and the F1 panel are never covered by a name.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    if (!font) return;
    const float baseSize = ImGui::GetFontSize();
    const ImVec2 disp = io.DisplaySize;
    if (disp.x < 1.0f || disp.y < 1.0f) return;

    for (int i = 0; i < n; i++) {
        const NameplateItem& it = items[i];
        // Off the sides of the screen entirely -- the draw list would clip these anyway, but a plate
        // whose position is far outside the viewport is not worth measuring.
        if (it.x < -0.5f || it.x > 1.5f || it.y < -0.5f || it.y > 1.5f) continue;

        // The NAME's alpha: the on/off-board fade, thinned out over the far end of its range so a peer
        // skating away goes gradually instead of vanishing at the limit.
        float nameA = it.name[0] ? g_alpha : 0.0f;
        if (nameA > 0.0f) {
            if (it.distCm > g_tune.maxDistCm) nameA = 0.0f;
            const float fs = g_tune.maxDistCm * g_tune.fadeDistFrac;
            if (nameA > 0.0f && it.distCm > fs && g_tune.maxDistCm > fs)
                nameA *= 1.0f - (it.distCm - fs) / (g_tune.maxDistCm - fs);
        }
        // The BUBBLE's alpha: its own age and its own (shorter) range, and deliberately NOT the
        // on/off-board fade -- somebody talking to you is worth seeing while you skate.
        float bubbleA = 0.0f;
        if (g_tune.bubbles && it.msg[0] && it.distCm <= g_tune.bubbleMaxDistCm) {
            const float age = (float)it.msgAgeMs / 1000.0f;
            bubbleA = 1.0f;
            if (age > g_tune.bubbleHoldSec) {
                const float over = g_tune.bubbleFadeSec > 0.01f ? g_tune.bubbleFadeSec : 1.0f;
                bubbleA = 1.0f - (age - g_tune.bubbleHoldSec) / over;
            }
            const float fs = g_tune.bubbleMaxDistCm * g_tune.bubbleFadeDistFrac;
            if (bubbleA > 0.0f && it.distCm > fs && g_tune.bubbleMaxDistCm > fs)
                bubbleA *= 1.0f - (it.distCm - fs) / (g_tune.bubbleMaxDistCm - fs);
        }
        if (nameA <= 0.01f && bubbleA <= 0.01f) continue;

        // Perspective: a plate is drawn smaller with distance like the skater under it, but clamped at
        // both ends -- unclamped it would be unreadable across a park and enormous in your face.
        // The SIZE is snapped to whole pixels as well as the position: a glyph rasterised at 17.3 px
        // one frame and 17.4 the next shimmers, and distance changes continuously.
        float scale = (it.distCm > 1.0f) ? (g_tune.refDistCm / it.distCm) : g_tune.maxScale;
        if (scale < g_tune.minScale) scale = g_tune.minScale;
        if (scale > g_tune.maxScale) scale = g_tune.maxScale;
        const float size = snap(baseSize * scale);
        if (size < 1.0f) continue;

        const float cx  = snap(it.x * disp.x);          // the head, in whole pixels
        const float hy  = snap(it.y * disp.y);
        const float rim = (size >= 18.0f) ? 2.0f : 1.0f;

        // ---- the name
        float nameTop = hy;
        if (nameA > 0.01f) {
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, it.name);
            const ImVec2 pos(snap(cx - ts.x * 0.5f), snap(hy - ts.y));
            nameTop = pos.y;
            // A dark rim rather than a panel: a box per player turns a session into a HUD, and an
            // outline stays readable over both a white wall and a black shadow.
            const ImU32 rc = ImGui::GetColorU32(ImVec4(0, 0, 0, nameA * g_tune.outlineAlpha));
            dl->AddText(font, size, ImVec2(pos.x - rim, pos.y), rc, it.name);
            dl->AddText(font, size, ImVec2(pos.x + rim, pos.y), rc, it.name);
            dl->AddText(font, size, ImVec2(pos.x, pos.y - rim), rc, it.name);
            dl->AddText(font, size, ImVec2(pos.x, pos.y + rim), rc, it.name);
            // The same colour chat uses for somebody else, so a name reads as the same person on both
            // surfaces.
            dl->AddText(font, size, pos,
                        ImGui::GetColorU32(ImVec4(T.other.x, T.other.y, T.other.z, nameA)), it.name);
        } else {
            // The bubble sits where it would sit if the name were showing, so mounting a board does
            // not make an open bubble jump down the screen.
            nameTop = snap(hy - size);
        }

        // ---- the bubble
        if (bubbleA <= 0.01f) continue;
        const float pad  = snap(size * 0.45f);
        const float tail = snap(size * 0.40f);
        const float gap  = snap(size * 0.30f);
        const float wrap = size * g_tune.bubbleWidthEm;
        const ImVec2 ms  = font->CalcTextSizeA(size, FLT_MAX, wrap, it.msg);
        const float bw   = snap(ms.x + pad * 2.0f);
        const float bh   = snap(ms.y + pad * 2.0f);
        const float bBot = snap(nameTop - gap - tail);
        const ImVec2 b0(snap(cx - bw * 0.5f), snap(bBot - bh));
        const ImVec2 b1(b0.x + bw, b0.y + bh);

        const ImU32 bg  = ImGui::GetColorU32(ImVec4(T.panel.x, T.panel.y, T.panel.z, 0.90f * bubbleA));
        const ImU32 brd = ImGui::GetColorU32(ImVec4(T.rule.x, T.rule.y, T.rule.z, T.rule.w * bubbleA));
        const float r   = snap(size * 0.35f);
        dl->AddRectFilled(b0, b1, bg, r);
        dl->AddRect(b0, b1, brd, r);
        // The tail: what makes it a speech bubble rather than a floating label. Drawn in the same fill
        // so the two read as one shape, pointing down at the head.
        const float tw = snap(size * 0.30f);
        dl->AddTriangleFilled(ImVec2(cx - tw, b1.y - 1.0f), ImVec2(cx + tw, b1.y - 1.0f),
                              ImVec2(cx, b1.y + tail), bg);
        dl->AddText(font, size, ImVec2(b0.x + pad, b0.y + pad),
                    ImGui::GetColorU32(ImVec4(T.text.x, T.text.y, T.text.z, bubbleA)),
                    it.msg, nullptr, wrap);
    }
}
