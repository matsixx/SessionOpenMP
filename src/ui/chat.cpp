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
// SessionOpenMP -- the chat box. Contract and thread rule: chat.h.
#include "chat.h"
#include "overlay.h"
#include "theme.h"
#include "imgui.h"
#include <windows.h>
#include <mutex>
#include <atomic>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

ChatTuning g_tune;

// ---- the model. One ring, one mutex. Sized so a busy session cannot make the lock interesting.
const int kMaxLines = 64;
const int kNameLen  = 32;
const int kTextLen  = 160;
struct Line {
    char     name[kNameLen] = {};
    char     text[kTextLen] = {};
    uint64_t atMs = 0;
    bool     mine = false;
    bool     system = false;
};
std::mutex g_mx;
Line       g_lines[kMaxLines];
int        g_head = 0;          // next write slot
int        g_count = 0;

std::atomic<bool> g_open{false};
// The press that OPENS the box is still physically down when the box appears, and ImGui's text field
// sees it on its very next frame -- which would send an empty line and close again instantly. A send
// is therefore accepted only once Enter has been observed UP since opening. A fact, not a timer.
std::atomic<bool> g_sendArmed{false};

// Typed lines waiting for the game thread. A tiny queue rather than one slot: a fast typist can send
// twice inside one game tick, and dropping the second is a bug the player would never understand.
const int kOutMax = 8;
char       g_out[kOutMax][kTextLen];
int        g_outN = 0;

uint64_t nowMs() { return GetTickCount64(); }

void pushLine(const char* name, const char* text, bool mine, bool system) {
    if (!text || !*text) return;
    std::lock_guard<std::mutex> lk(g_mx);
    Line& l = g_lines[g_head];
    l = Line{};
    if (name) strncpy_s(l.name, name, _TRUNCATE);
    strncpy_s(l.text, text, _TRUNCATE);
    l.atMs = nowMs();
    l.mine = mine;
    l.system = system;
    g_head = (g_head + 1) % kMaxLines;
    if (g_count < kMaxLines) g_count++;
}

} // namespace

ChatTuning& Chat_Tuning() { return g_tune; }
static std::atomic<bool> g_typing{false};
bool Chat_IsOpen()   { return g_open.load(); }
bool Chat_IsTyping() { return g_open.load() && g_typing.load(); }   // closed box can never be typing

void Chat_SetOpen(bool open) {
    if (open && !g_open.load()) g_sendArmed = false;   // wait for the opening key to come up
    g_open = open;
}

void Chat_Push(const char* name, const char* text, bool mine) { pushLine(name, text, mine, false); }
void Chat_System(const char* text) { pushLine(nullptr, text, false, true); }

bool Chat_Take(char* out, int cap) {
    if (!out || cap <= 0) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_outN <= 0) return false;
    strncpy_s(out, (size_t)cap, g_out[0], _TRUNCATE);
    for (int i = 1; i < g_outN; i++) memcpy(g_out[i - 1], g_out[i], sizeof(g_out[0]));
    g_outN--;
    return out[0] != 0;
}


bool Chat_HasVisible() {
    if (!g_tune.enabled) return false;
    if (g_open.load()) return true;
    const uint64_t t = nowMs();
    const float keep = g_tune.fadeAfterSec + g_tune.fadeOverSec;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_count <= 0) return false;
    const Line& newest = g_lines[(g_head + kMaxLines - 1) % kMaxLines];
    return (float)(t - newest.atMs) / 1000.0f <= keep;
}

void Chat_Draw() {
    if (!g_tune.enabled) return;
    const bool open = g_open.load();
    const uint64_t t = nowMs();

    // Snapshot under the lock, draw outside it: the game thread must never wait on a frame.
    struct Shown { char name[kNameLen]; char text[kTextLen]; float alpha; bool mine, system; };
    Shown shown[kMaxLines];
    int nShown = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const int first = (g_count < kMaxLines) ? 0 : g_head;
        for (int k = 0; k < g_count; k++) {
            const Line& l = g_lines[(first + k) % kMaxLines];
            float a = 1.0f;
            if (!open) {
                // Closed: recent talk still shows, then fades. This is what makes chat usable while
                // skating -- you read without stopping, and the screen goes clean on its own.
                const float age = (float)(t - l.atMs) / 1000.0f;
                if (age > g_tune.fadeAfterSec + g_tune.fadeOverSec) continue;
                if (age > g_tune.fadeAfterSec)
                    a = 1.0f - (age - g_tune.fadeAfterSec) / (g_tune.fadeOverSec > 0.01f ? g_tune.fadeOverSec : 1.0f);
            }
            if (nShown >= kMaxLines) break;
            Shown& s = shown[nShown++];
            memcpy(s.name, l.name, sizeof(s.name));
            memcpy(s.text, l.text, sizeof(s.text));
            s.alpha = a; s.mine = l.mine; s.system = l.system;
        }
    }
    if (!open) {
        if (nShown == 0) return;                                   // nothing to say: draw nothing at all
        if (nShown > g_tune.maxShownIdle) {                        // keep only the newest few
            const int drop = nShown - g_tune.maxShownIdle;
            memmove(shown, shown + drop, sizeof(Shown) * (size_t)(nShown - drop));
            nShown -= drop;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    const float sc = io.FontGlobalScale > 0.01f ? io.FontGlobalScale : 1.0f;
    const float w  = g_tune.width * sc;
    const float h  = (open ? g_tune.height : 0.0f) * sc;

    ImGui::SetNextWindowPos(ImVec2(g_tune.marginX * sc,
                                   io.DisplaySize.y - g_tune.marginY * sc),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    // AlwaysAutoResize OVERRIDES SetNextWindowSize, so without this the panel grows to fit the longest
    // line and runs off the screen. Constrain the WIDTH to exactly `w` and leave the height free: the
    // box hugs its content vertically, and text has a hard right edge to wrap against.
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, 1.0e6f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!open) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    const ThemePalette& T = Theme();
    Theme_Push(open);                                   // opaque while typing, translucent while idle
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * sc, 12 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8 * sc, 5 * sc));

    if (ImGui::Begin("##ompchat", nullptr, flags)) {
        if (open) {
            ImGui::TextColored(T.dim, "SESSION CHAT");
            ImGui::Separator();
            ImGui::BeginChild("##omphist", ImVec2(0, h), false,
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        }
        for (int i = 0; i < nShown; i++) {
            const Shown& s = shown[i];
            const ImVec4 nameCol = s.system ? T.system : (s.mine ? T.accent : T.other);
            // Wrap at the window's right edge. The name stays on the first line and continuation
            // lines indent under the start of the message, which is what makes a wrapped chat line
            // still read as ONE person talking.
            ImGui::PushTextWrapPos(0.0f);
            if (s.system) {
                ImGui::TextColored(ImVec4(T.system.x, T.system.y, T.system.z, s.alpha), "%s", s.text);
            } else {
                ImGui::TextColored(ImVec4(nameCol.x, nameCol.y, nameCol.z, s.alpha), "%s", s.name[0] ? s.name : "?");
                ImGui::SameLine(0, 6 * sc);
                ImGui::TextColored(ImVec4(T.text.x, T.text.y, T.text.z, s.alpha), "%s", s.text);
            }
            ImGui::PopTextWrapPos();
        }
        if (open) {
            // Stick to the bottom unless the player has scrolled up to read.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            ImGui::Separator();

            static char buf[kTextLen] = {};
            ImGui::SetNextItemWidth(-1.0f);
            if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) ImGui::SetKeyboardFocusHere();
            const bool sent = ImGui::InputText("##ompchatin", buf, sizeof(buf),
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            g_typing = buf[0] != 0;      // every drawn frame: the flag tracks the buffer exactly
            // Arm once the key that opened the box is up (both Enters -- the field accepts either).
            if (!ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter))
                g_sendArmed = true;
            if (sent && g_sendArmed.load()) {
                // Trim: a line of spaces is not a message, and trailing space is invisible noise on
                // everyone else's screen.
                char* p = buf; while (*p == ' ') p++;
                int n = (int)strlen(p); while (n > 0 && p[n - 1] == ' ') p[--n] = 0;
                if (n > 0) {
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (g_outN < kOutMax) { strncpy_s(g_out[g_outN], p, _TRUNCATE); g_outN++; }
                }
                buf[0] = 0; g_typing = false;
                g_open = false;                       // Enter sends AND closes, like every game chat
            }
            // repeat=false: a close is an EVENT, never something a held key should keep doing.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { buf[0] = 0; g_typing = false; g_open = false; }
            ImGui::TextColored(T.dim, "ENTER send    ESC cancel");
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    Theme_Pop();
}
