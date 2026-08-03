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
// SessionOpenMP -- the shared look. Rationale and thread rule: theme.h.
#include "theme.h"
#include "overlay.h"      // OvLog
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

ThemePalette g_pal;
float        g_fontPx = 20.0f;

std::mutex g_fontMx;
void*      g_fontData = nullptr;
int        g_fontSize = 0;
char       g_fontName[64] = {};
bool       g_fontPending = false;
ImFont*    g_font = nullptr;        // render thread only

// How many of each stack Theme_Push left behind, so Theme_Pop cannot drift out of step when the
// palette grows. Not a counter of nested pushes -- these are not designed to nest.
int  g_nColors = 0, g_nVars = 0;
bool g_fontPushed = false;

} // namespace

const ThemePalette& Theme() { return g_pal; }

void Theme_Push(bool solid) {
    const ThemePalette& p = g_pal;
    const ImVec4 bg      = solid ? p.panelSolid : p.panel;
    const ImVec4 field   = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    const ImVec4 fieldHi = ImVec4(1.0f, 1.0f, 1.0f, 0.11f);
    const ImVec4 accentDim = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);

    struct C { ImGuiCol id; ImVec4 v; };
    const C cols[] = {
        { ImGuiCol_WindowBg,        bg },
        { ImGuiCol_ChildBg,         ImVec4(0, 0, 0, 0) },
        { ImGuiCol_PopupBg,         p.panelSolid },
        { ImGuiCol_Border,          p.rule },
        { ImGuiCol_Text,            p.text },
        { ImGuiCol_TextDisabled,    p.dim },
        { ImGuiCol_Separator,       p.rule },
        // Title bar: the same panel, not ImGui's blue. A window that owns a screen and one that
        // floats over play should differ in ALPHA, never in hue.
        { ImGuiCol_TitleBg,         bg },
        { ImGuiCol_TitleBgActive,   bg },
        { ImGuiCol_TitleBgCollapsed,bg },
        { ImGuiCol_FrameBg,         field },
        { ImGuiCol_FrameBgHovered,  fieldHi },
        { ImGuiCol_FrameBgActive,   fieldHi },
        { ImGuiCol_Button,          field },
        { ImGuiCol_ButtonHovered,   accentDim },
        { ImGuiCol_ButtonActive,    p.accent },
        { ImGuiCol_Header,          field },        // CollapsingHeader / selectables
        { ImGuiCol_HeaderHovered,   accentDim },
        { ImGuiCol_HeaderActive,    accentDim },
        { ImGuiCol_CheckMark,       p.accent },
        { ImGuiCol_SliderGrab,      p.accent },
        { ImGuiCol_SliderGrabActive,p.accent },
        { ImGuiCol_ScrollbarBg,     ImVec4(0, 0, 0, 0) },
        { ImGuiCol_ScrollbarGrab,   p.rule },
        { ImGuiCol_ScrollbarGrabHovered, accentDim },
        { ImGuiCol_ScrollbarGrabActive,  p.accent },
        { ImGuiCol_ResizeGrip,      p.rule },
        { ImGuiCol_ResizeGripHovered, accentDim },
        { ImGuiCol_ResizeGripActive,  p.accent },
    };
    for (const C& c : cols) ImGui::PushStyleColor(c.id, c.v);
    g_nColors = (int)(sizeof(cols) / sizeof(cols[0]));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,  0.0f);
    g_nVars = 8;

    g_fontPushed = (g_font != nullptr);
    if (g_fontPushed) ImGui::PushFont(g_font);
}

void Theme_Pop() {
    if (g_fontPushed) { ImGui::PopFont(); g_fontPushed = false; }
    ImGui::PopStyleVar(g_nVars);
    ImGui::PopStyleColor(g_nColors);
    g_nVars = 0; g_nColors = 0;
}

void Theme_OfferFont(void* data, int size, const char* debugName) {
    if (!data || size <= 0) { free(data); return; }
    // Re-copy through ImGui's allocator: the atlas takes ownership and frees with ImGui's free, which
    // is not necessarily the CRT free this DLL's malloc pairs with. One copy, once, at startup.
    void* mine = ImGui::MemAlloc((size_t)size);
    if (!mine) { free(data); return; }
    memcpy(mine, data, (size_t)size);
    free(data);
    std::lock_guard<std::mutex> lk(g_fontMx);
    if (g_fontData) ImGui::MemFree(g_fontData);          // a second offer replaces the first
    g_fontData = mine; g_fontSize = size;
    strncpy_s(g_fontName, debugName ? debugName : "?", _TRUNCATE);
    g_fontPending = true;
}

bool Theme_ConsumePendingFont() {
    void* data = nullptr; int size = 0; char name[64] = {};
    {
        std::lock_guard<std::mutex> lk(g_fontMx);
        if (!g_fontPending) return false;
        g_fontPending = false;
        data = g_fontData; size = g_fontSize;
        strncpy_s(name, g_fontName, _TRUNCATE);
        g_fontData = nullptr; g_fontSize = 0;
    }
    if (!data || size <= 0) return false;
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;                     // ImGui frees it with its own allocator
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(data, size, g_fontPx, &cfg);
    if (!f) {
        char m[128]; snprintf(m, sizeof(m), "[theme] font '%s' rejected by ImGui", name); OvLog(m);
        return false;
    }
    g_font = f;
    io.Fonts->Build();
    char m[160];
    snprintf(m, sizeof(m), "[theme] UI font: '%s' (%d bytes, %.0fpx)", name, size, g_fontPx);
    OvLog(m);
    return true;
}
