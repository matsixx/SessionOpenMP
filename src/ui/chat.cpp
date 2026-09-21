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
#include <math.h>
#include <float.h>

namespace {

ChatTuning g_tune;

// ---- the model. One ring, one mutex. Sized so a busy session cannot make the lock interesting.
const int kMaxLines = 64;
const int kNameLen  = 32;
const int kTextLen  = 160;
struct Line {
    char     name[kNameLen] = {};
    char     text[kTextLen] = {};
    uint64_t atUs = 0;
    bool     mine = false;
    bool     system = false;
};
std::mutex g_mx;
Line       g_lines[kMaxLines];
int        g_head = 0;          // next write slot
int        g_count = 0;

std::atomic<bool> g_open{false};
std::atomic<bool> g_typing{false};
std::atomic<bool> g_enterPending{false};
// "an Enter right now would open the box": a game-thread fact the window hook needs in the message.
std::atomic<bool> g_enterOurs{false};
std::atomic<int>  g_players{0};
// The press that OPENS the box is still physically down when the box appears, and ImGui's text field
// sees it on its very next frame -- which would send an empty line and close again instantly. A send
// is therefore accepted only once Enter has been observed UP since opening. A fact, not a timer.
std::atomic<bool> g_sendArmed{false};
std::atomic<bool> g_justOpened{false};   // set on open (game thread), consumed by the draw
// Which surface owns the box. Up here with the rest of the state because Chat_HasVisible, which
// sits above the game-drawn section, is one of the things it gates.
std::atomic<bool> g_gameDrawn{false};
// How far back into the history the box is scrolled, in VISUAL lines. Zero -- the bottom -- is
// restored whenever the box opens or anything is sent, so it is never a surprise on the way in.
std::atomic<int>  g_scroll{0};

// Typed lines waiting for the game thread. A tiny queue rather than one slot: a fast typist can send
// twice inside one game tick, and dropping the second is a bug the player would never understand.
const int kOutMax = 8;
char       g_out[kOutMax][kTextLen];
int        g_outN = 0;
char       g_lastSent[kTextLen] = {};   // whichever surface is up; Up recalls it. Only one of
                                        // them ever runs, so there is nothing to race with.

// Microseconds from the performance counter. Both threads stamp and age with THIS clock: the tick
// count's 16 ms grain shows as steps in a 180 ms fade.
uint64_t nowUs() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)freq.QuadPart);
}

void pushLine(const char* name, const char* text, bool mine, bool system) {
    if (!text || !*text) return;
    std::lock_guard<std::mutex> lk(g_mx);
    Line& l = g_lines[g_head];
    l = Line{};
    if (name) strncpy_s(l.name, name, _TRUNCATE);
    strncpy_s(l.text, text, _TRUNCATE);
    l.atUs = nowUs();
    l.mine = mine;
    l.system = system;
    g_head = (g_head + 1) % kMaxLines;
    if (g_count < kMaxLines) g_count++;
}

// ---- one line, drawn the same way on both surfaces ------------------------------------------------
float snapf(float v)  { return floorf(v + 0.5f); }
float smooth(float t) { t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); return t * t * (3.0f - 2.0f * t); }
ImU32 tint(const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// The name stays on the first line and the text runs to the right of it, continuation lines indented
// under the text's own start -- what makes a wrapped line still read as ONE person talking. A name
// too long for that puts the text on the next line rather than squeezing it into a column. A system
// line has no name; a short rule stands in front of it instead.
struct LineLayout { float textX = 0, textY = 0, h = 0; };
LineLayout measure(ImFont* font, float size, float width, const char* name, const char* text, bool system) {
    LineLayout L;
    const float gap = snapf(size * 0.45f);
    if (system) {
        L.textX = snapf(size * 1.0f);
    } else if (name && name[0]) {
        const float nameW = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name).x;
        if (nameW + gap <= width * 0.45f) L.textX = snapf(nameW + gap);
        else                              L.textY = snapf(size * 1.15f);
    }
    float wrapW = width - L.textX; if (wrapW < size) wrapW = size;
    L.h = snapf(L.textY + font->CalcTextSizeA(size, FLT_MAX, wrapW, text).y);
    return L;
}
// `shadow` > 0 draws the text over play with no panel behind it: the nameplates' dark rim, exactly
// -- the same four passes at the same alpha -- so a name over a head and a line of chat read as one
// treatment. Inside the open box the panel does that job and the text is drawn plain.
void shadowText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* s,
                float wrapW, float shadow) {
    if (shadow > 0.005f) {
        const float rim = (size >= 18.0f) ? 2.0f : 1.0f;
        const ImU32 dark = ImGui::GetColorU32(ImVec4(0, 0, 0, shadow));
        dl->AddText(font, size, ImVec2(at.x - rim, at.y), dark, s, nullptr, wrapW);
        dl->AddText(font, size, ImVec2(at.x + rim, at.y), dark, s, nullptr, wrapW);
        dl->AddText(font, size, ImVec2(at.x, at.y - rim), dark, s, nullptr, wrapW);
        dl->AddText(font, size, ImVec2(at.x, at.y + rim), dark, s, nullptr, wrapW);
    }
    dl->AddText(font, size, at, col, s, nullptr, wrapW);
}
void drawLine(ImDrawList* dl, ImFont* font, float size, ImVec2 at, float width, const LineLayout& L,
              const char* name, const char* text, bool mine, bool system, float alpha, float shadow) {
    const ThemePalette& T = Theme();
    float wrapW = width - L.textX; if (wrapW < size) wrapW = size;
    if (system) {
        const float rule = snapf(size * 0.6f), mid = snapf(at.y + size * 0.55f);
        if (shadow > 0.005f)
            dl->AddRectFilled(ImVec2(at.x - 1.0f, mid - 1.0f), ImVec2(at.x + rule + 1.0f, mid + 2.0f),
                              ImGui::GetColorU32(ImVec4(0, 0, 0, shadow)));
        dl->AddRectFilled(ImVec2(at.x, mid), ImVec2(at.x + rule, mid + 1.0f), tint(T.system, alpha * 0.8f));
        shadowText(dl, font, size, ImVec2(at.x + L.textX, at.y), tint(T.system, alpha), text, wrapW, shadow);
        return;
    }
    if (name && name[0]) shadowText(dl, font, size, at, tint(mine ? T.accent : T.other, alpha), name, 0.0f, shadow);
    shadowText(dl, font, size, ImVec2(at.x + L.textX, at.y + L.textY), tint(T.text, alpha), text, wrapW, shadow);
}

// What a frame draws: copied out from under the lock, with each line's fade and growth resolved.
struct Shown { char name[kNameLen]; char text[kTextLen]; float alpha, grow; bool mine, system; };

// ---- CLOSED: recent talk over play. No window (chat.h says why) and no panel either -- a box in
// the corner pulls the eye while skating; a dark rim on the glyphs keeps them readable instead. The
// lines are laid out here, bottom-anchored, from measured heights -- exact every frame. A new line
// grows in from nothing and a faded one shrinks away, so the block above never jumps.
void drawClosed(const Shown* shown, int n) {
    ImGuiIO& io = ImGui::GetIO();
    if (n <= 0 || io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) return;
    Theme_Push(false);
    ImFont* font = ImGui::GetFont();
    const float sc   = io.FontGlobalScale > 0.01f ? io.FontGlobalScale : 1.0f;
    // The font's own size times the global scale -- what a window would report -- read directly
    // because no window is current here.
    const float size = snapf(font->FontSize * sc);
    const float w = snapf(g_tune.width * sc), gap = snapf(5.0f * sc);
    const float x0 = snapf(g_tune.marginX * sc);
    const float y1 = snapf(io.DisplaySize.y - g_tune.marginY * sc);

    LineLayout L[kMaxLines];
    float total = 0.0f, peak = 0.0f;
    for (int i = 0; i < n; i++) {
        L[i] = measure(font, size, w, shown[i].name, shown[i].text, shown[i].system);
        total += (L[i].h + (i ? gap : 0.0f)) * shown[i].grow;
        if (shown[i].alpha > peak) peak = shown[i].alpha;
    }
    if (peak <= 0.005f || total <= 0.5f) { Theme_Pop(); return; }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float rim = 3.0f;                              // room for the rim and shadow at the edges
    float y = snapf(y1 - total);
    for (int i = 0; i < n; i++) {
        const Shown& s = shown[i];
        y += (i ? gap : 0.0f) * s.grow;
        const float hh = L[i].h * s.grow;
        if (hh < 0.5f) continue;
        const bool partial = s.grow < 0.999f;
        if (partial) dl->PushClipRect(ImVec2(x0 - rim, snapf(y) - rim), ImVec2(x0 + w + rim, snapf(y + hh)), true);
        if (s.alpha > 0.005f)
            drawLine(dl, font, size, ImVec2(x0, snapf(y)), w, L[i], s.name, s.text, s.mine, s.system, s.alpha,
                     s.alpha * g_tune.shadowAlpha);
        if (partial) dl->PopClipRect();
        y += hh;
    }
    Theme_Pop();
}

// Up recalls the last line sent (a typo is easier to fix than to retype); Down clears the field.
int historyCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    if (d->EventKey == ImGuiKey_UpArrow && g_lastSent[0]) {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, g_lastSent);
    } else if (d->EventKey == ImGuiKey_DownArrow) {
        d->DeleteChars(0, d->BufTextLen);
    }
    return 0;
}

// ---- OPEN: the box. A fixed-width window at the same anchor: header, a scrolling history drawn by
// the same line renderer, the field, and the keys that work.
void drawOpen(const Shown* shown, int n) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) return;
    const ThemePalette& T = Theme();
    const float sc = io.FontGlobalScale > 0.01f ? io.FontGlobalScale : 1.0f;
    const float w = snapf(g_tune.width * sc), histH = snapf(g_tune.height * sc), padX = snapf(16.0f * sc);

    ImGui::SetNextWindowPos(ImVec2(snapf(g_tune.marginX * sc), snapf(io.DisplaySize.y - g_tune.marginY * sc)),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);
    // AlwaysAutoResize OVERRIDES SetNextWindowSize: constrain the WIDTH to exactly `w` and let the
    // height follow the content, which is fixed (the history child has a set height), so it settles on
    // the first frame and never moves again.
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, 1.0e6f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    Theme_Push(true);                                   // opaque: it owns the keyboard
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, snapf(12.0f * sc)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(snapf(8.0f * sc), snapf(6.0f * sc)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(snapf(12.0f * sc), snapf(7.0f * sc)));

    static char buf[kTextLen] = {};
    if (ImGui::Begin("##ompchat", nullptr, flags)) {
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();

        // ---- header: what this is, and who is here
        ImGui::TextColored(T.dim, "SESSION CHAT");
        {
            const int others = g_players.load();
            char who[40];
            snprintf(who, sizeof(who), "%d ONLINE", others + 1);
            ImGui::SameLine(w - padX - ImGui::CalcTextSize(who).x);
            ImGui::TextColored(T.dim, "%s", who);
        }
        ImGui::Separator();

        // ---- history
        ImGui::BeginChild("##omphist", ImVec2(0.0f, histH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float W = ImGui::GetContentRegionAvail().x;
            if (n == 0) ImGui::TextColored(T.dim, "Nothing said yet.");
            for (int i = 0; i < n; i++) {
                const Shown& s = shown[i];
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const LineLayout L = measure(font, size, W, s.name, s.text, s.system);
                drawLine(dl, font, size, ImVec2(snapf(p.x), snapf(p.y)), W, L, s.name, s.text, s.mine, s.system, 1.0f, 0.0f);
                ImGui::Dummy(ImVec2(W, L.h));
            }
            // Stick to the end unless the player has scrolled up to read.
            if (g_justOpened.load() || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::Separator();

        // ---- the field
        ImGui::SetNextItemWidth(-1.0f);
        if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) ImGui::SetKeyboardFocusHere();
        const bool sent = ImGui::InputText("##ompchatin", buf, sizeof(buf),
                                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                           historyCb);
        {   // the accent edge: lit while there is something to send
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + snapf(3.0f * sc), b.y),
                                                      tint(T.accent, buf[0] ? 1.0f : 0.35f));
        }
        g_typing = buf[0] != 0;      // every drawn frame: the flag tracks the buffer exactly
        // Arm once the key that opened the box is up (both Enters -- the field accepts either).
        if (!ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter))
            g_sendArmed = true;
        if (sent && g_sendArmed.load()) {
            // Trim: a line of spaces is not a message, and trailing space is invisible noise on
            // everyone else's screen.
            char* p = buf; while (*p == ' ') p++;
            int len = (int)strlen(p); while (len > 0 && p[len - 1] == ' ') p[--len] = 0;
            if (len > 0) {
                strncpy_s(g_lastSent, p, _TRUNCATE);
                std::lock_guard<std::mutex> lk(g_mx);
                if (g_outN < kOutMax) { strncpy_s(g_out[g_outN], p, _TRUNCATE); g_outN++; }
            }
            buf[0] = 0; g_typing = false;
            g_open = false;                       // Enter sends AND closes, like every game chat
        }
        // repeat=false: a close is an EVENT, never something a held key should keep doing.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { buf[0] = 0; g_typing = false; g_open = false; }

        // ---- the keys, and the room left when it starts to matter
        auto hint = [&](const char* key, const char* what, bool first) {
            if (!first) ImGui::SameLine(0.0f, snapf(18.0f * sc));
            ImGui::TextColored(T.text, "%s", key);
            ImGui::SameLine(0.0f, snapf(5.0f * sc));
            ImGui::TextColored(T.dim, "%s", what);
        };
        hint("ENTER", "send", true);
        hint("ESC", "close", false);
        hint("UP", "last line", false);
        const int len = (int)strlen(buf);
        if (len >= kTextLen - 40) {
            char c[24];
            snprintf(c, sizeof(c), "%d/%d", len, kTextLen - 1);
            ImGui::SameLine(w - padX - ImGui::CalcTextSize(c).x);
            ImGui::TextColored(len >= kTextLen - 1 ? T.warn : T.dim, "%s", c);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    Theme_Pop();
    g_justOpened = false;
}

} // namespace

ChatTuning& Chat_Tuning() { return g_tune; }
bool Chat_IsOpen()   { return g_open.load(); }
bool Chat_IsTyping() { return g_open.load() && g_typing.load(); }   // closed box can never be typing

void Chat_SetOpen(bool open) {
    g_scroll = 0;                       // always at the newest on the way in or out
    if (open && !g_open.load()) { g_sendArmed = false; g_justOpened = true; }   // wait for the opening key to come up
    g_open = open;
}
void Chat_NoteEnterPressed() { g_enterPending = true; }
void Chat_SetEnterOurs(bool ours) { g_enterOurs = ours; }
bool Chat_EnterOurs() { return g_enterOurs.load() && !g_open.load(); }
bool Chat_TakeEnterPressed() { return g_enterPending.exchange(false); }
void Chat_SetPresence(int players) { g_players = players < 0 ? 0 : players; }

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
    if (!g_tune.enabled || g_gameDrawn.load()) return false;
    if (g_open.load()) return true;
    const uint64_t t = nowUs();
    const float keep = g_tune.fadeAfterSec + g_tune.fadeOverSec + g_tune.collapseSec;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_count <= 0) return false;
    const Line& newest = g_lines[(g_head + kMaxLines - 1) % kMaxLines];
    return (float)((double)(t - newest.atUs) * 1e-6) <= keep;
}

// =====================================================================================================
// THE GAME-DRAWN SURFACE: the compose line, and what the HUD reads.
//
// The editing is ours -- see chat.h for why the game's own editable text box cannot hold the keyboard
// during play. What lives here is a buffer, a caret, and the handful of keys that move them. It is
// touched by the WINDOW-MESSAGE thread (the keys) and read by the GAME thread (the draw), so it has
// its own small lock: deliberately NOT g_mx, which a send has to take, and nesting the two would be
// a deadlock waiting for somebody to reorder a line.
// =====================================================================================================
namespace {

std::mutex g_cmx;
char       g_compose[kTextLen] = {};
int        g_caret = 0;

// Printable ASCII only. Every name and message in this mod travels the wire as plain bytes and is
// drawn through an FText built from ASCII, so a character that cannot survive that round trip is
// better refused at the keyboard than turned into a question mark on somebody else's screen.
bool typable(unsigned int ch) { return ch >= 32 && ch < 127; }
// How far a wheel notch or a page key moves the history.
const int kScrollStep = 3;      // a page key; the wheel uses the same

void insertChar(char c) {
    const int len = (int)strlen(g_compose);
    if (len >= kTextLen - 1) return;
    if (g_caret < 0) g_caret = 0;
    if (g_caret > len) g_caret = len;
    memmove(g_compose + g_caret + 1, g_compose + g_caret, (size_t)(len - g_caret) + 1);
    g_compose[g_caret++] = c;
}
void eraseAt(int at) {
    const int len = (int)strlen(g_compose);
    if (at < 0 || at >= len) return;
    memmove(g_compose + at, g_compose + at + 1, (size_t)(len - at));
}
// Ctrl+V. On the window-message thread, which is the one allowed to open the clipboard.
void pasteClipboard() {
    if (!OpenClipboard(nullptr)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            // A pasted NEWLINE ends the paste rather than sending it: somebody pasting two lines
            // meant to say the first one, and a chat box that fires on its own is a way to say
            // something you did not mean to.
            for (int i = 0; w[i] && w[i] != 13 && w[i] != 10; i++)
                if (typable((unsigned int)w[i])) insertChar((char)w[i]);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

} // namespace

void Chat_SetGameDrawn(bool on) { g_gameDrawn = on; }
int  Chat_TypedMax() { return kTextLen - 1; }
void Chat_Scroll(int lines) {
    if (!g_gameDrawn.load() || !g_open.load() || !lines) return;
    int v = g_scroll.load() + lines;
    if (v < 0) v = 0;
    g_scroll = v;                       // the surface clamps the far end; it is the one that counts
}
int  Chat_ScrollGet() { return g_open.load() ? g_scroll.load() : 0; }
void Chat_ScrollClamp(int maxBack) {
    if (maxBack < 0) maxBack = 0;
    if (g_scroll.load() > maxBack) g_scroll = maxBack;
}
int  Chat_Presence() { return g_players.load(); }
int  Chat_TypedLen() {
    if (!g_open.load()) return 0;
    std::lock_guard<std::mutex> lk(g_cmx);
    return (int)strlen(g_compose);
}

void Chat_NoteChar(unsigned int ch) {
    if (!g_gameDrawn.load() || !g_open.load() || !typable(ch)) return;
    std::lock_guard<std::mutex> lk(g_cmx);
    insertChar((char)ch);
    g_typing = g_compose[0] != 0;
}

void Chat_NoteKey(int vk, bool ctrl, bool repeat) {
    if (!g_gameDrawn.load() || !g_open.load()) return;
    bool send = false;
    char outLine[kTextLen] = {};
    {
        std::lock_guard<std::mutex> lk(g_cmx);
        const int len = (int)strlen(g_compose);
        if (g_caret > len) g_caret = len;
        if (ctrl && (vk == 'V' || vk == 'v')) {
            pasteClipboard();
        } else switch (vk) {
        case VK_BACK:   if (g_caret > 0) { eraseAt(g_caret - 1); g_caret--; } break;
        case VK_DELETE: eraseAt(g_caret); break;
        case VK_LEFT:   if (g_caret > 0) g_caret--; break;
        case VK_RIGHT:  if (g_caret < len) g_caret++; break;
        case VK_HOME:   g_caret = 0; break;
        case VK_END:    g_caret = len; break;
        case VK_PRIOR:  g_scroll = g_scroll.load() + kScrollStep; break;   // PageUp: back
        case VK_NEXT: { int v = g_scroll.load() - kScrollStep; g_scroll = v < 0 ? 0 : v; } break;
        case VK_UP:     // the last thing you said, for fixing a typo or saying it again
            strncpy_s(g_compose, g_lastSent, _TRUNCATE);
            g_caret = (int)strlen(g_compose);
            break;
        case VK_ESCAPE:
            g_compose[0] = 0; g_caret = 0;
            g_typing = false; g_open = false;
            break;
        case VK_RETURN:
            // THE ENTER THAT OPENED THE BOX IS STILL DOWN. Its auto-repeats arrive here and must not
            // send the empty line and shut the box again -- which is the whole reason `repeat` is
            // passed in rather than filtered at the hook (backspace wants the opposite).
            if (repeat) break;
            {   // Trim: a line of spaces is not a message, and a trailing space is invisible noise
                // on everyone else's screen.
                char* p = g_compose; while (*p == ' ') p++;
                int n = (int)strlen(p); while (n > 0 && p[n - 1] == ' ') p[--n] = 0;
                if (n > 0) { strncpy_s(outLine, p, _TRUNCATE); send = true; }
            }
            g_compose[0] = 0; g_caret = 0; g_scroll = 0;
            g_typing = false; g_open = false;       // Enter sends AND closes, like every game chat
            break;
        default: break;
        }
        if (vk != VK_RETURN && vk != VK_ESCAPE) g_typing = g_compose[0] != 0;
    }
    // The send queue is g_mx's, and it is taken HERE -- outside g_cmx, which is the whole point of
    // there being two locks.
    if (send) {
        strncpy_s(g_lastSent, outLine, _TRUNCATE);
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_outN < kOutMax) { strncpy_s(g_out[g_outN], outLine, _TRUNCATE); g_outN++; }
    }
}

int Chat_Compose(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!g_open.load()) return 0;
    // The caret is a CHARACTER inserted at its own index, not a drawn bar: one text widget draws one
    // string and nothing here knows how wide a glyph came out. It blinks on a ~1 s cycle, so an empty
    // box still reads as "waiting for you" rather than as something that failed to open.
    const bool on = ((nowUs() / 530000ull) & 1ull) == 0ull;
    std::lock_guard<std::mutex> lk(g_cmx);
    const int len = (int)strlen(g_compose);
    const int at = g_caret < 0 ? 0 : (g_caret > len ? len : g_caret);
    int k = 0;
    for (int i = 0; i <= len && k < cap - 1; i++) {
        if (i == at && on) out[k++] = '|';
        if (i < len && k < cap - 1) out[k++] = g_compose[i];
    }
    out[k] = 0;
    return k;
}

int Chat_Lines(ChatLineView* out, int cap) {
    if (!out || cap <= 0) return 0;
    const bool open = g_open.load();
    const uint64_t t = nowUs();
    // Closed, a line is kept until it has faded AND given its height back -- the same life the ImGui
    // surface gives it, so switching between the two does not change how long anything stays up.
    const float keep = g_tune.fadeAfterSec + g_tune.fadeOverSec + g_tune.collapseSec;

    std::lock_guard<std::mutex> lk(g_mx);
    const int first = (g_head - g_count + kMaxLines) % kMaxLines;
    // START NEAR THE END. This walked from the OLDEST message and stopped once it had `cap` of them,
    // so with more than `cap` in the ring the newest were never even looked at: open the box after a
    // busy minute and it showed a conversation from earlier and nothing you had just said. The trim
    // that used to follow could not save it -- by then the newest were not in the array to trim to.
    // Everything alive is handed over now; the WINDOW is the surface's business, because the surface
    // is what knows how many lines these wrap into and how far back it has been scrolled.
    int k0 = g_count - cap; if (k0 < 0) k0 = 0;
    int n = 0;
    for (int k = k0; k < g_count && n < cap; k++) {
        const Line& l = g_lines[(first + k) % kMaxLines];
        const float age = (float)((double)(t - l.atUs) * 1e-6);
        if (!open && age > keep) continue;
        ChatLineView v{};
        if (l.system || !l.name[0]) { snprintf(v.text, sizeof(v.text), "%s", l.text); v.nameLen = 0; }
        else {
            snprintf(v.text, sizeof(v.text), "%s  %s", l.name, l.text);
            const size_t nl = strlen(l.name);
            v.nameLen = (uint8_t)(nl > 255 ? 255 : nl);
        }
        // OPEN MEANS NO FADE. Reported as age zero rather than with a second flag, so the caller has
        // one rule for alpha and the two states cannot get out of step.
        v.ageMs = open ? 0u : (uint32_t)(age * 1000.0f);
        v.mine = l.mine; v.system = l.system;
        out[n++] = v;
    }
    return n;
}

void Chat_Draw() {
    if (!g_tune.enabled || g_gameDrawn.load()) return;
    const bool open = g_open.load();
    const uint64_t t = nowUs();

    // Snapshot under the lock, draw outside it: the game thread must never wait on a frame.
    Shown shown[kMaxLines];
    int nShown = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const int first = (g_count < kMaxLines) ? 0 : g_head;
        // Closed, only the newest few are drawn: skip the rest before they are even measured.
        int start = 0;
        if (!open && g_count > g_tune.maxShownIdle) start = g_count - g_tune.maxShownIdle;
        const float hold = g_tune.fadeAfterSec, fade = g_tune.fadeOverSec > 0.01f ? g_tune.fadeOverSec : 1.0f;
        const float grow = g_tune.appearSec > 0.01f ? g_tune.appearSec : 0.01f;
        const float shrink = g_tune.collapseSec > 0.01f ? g_tune.collapseSec : 0.01f;
        for (int k = start; k < g_count; k++) {
            const Line& l = g_lines[(first + k) % kMaxLines];
            float a = 1.0f, g = 1.0f;
            if (!open) {
                // Closed: recent talk still shows, then fades, then gives its height back. This is
                // what makes chat usable while skating -- you read without stopping, and the screen
                // goes clean on its own.
                const float age = (float)((double)(t - l.atUs) * 1e-6);
                if (age > hold + fade + shrink) continue;
                if (age < grow) { g = smooth(age / grow); a = g; }
                else if (age > hold + fade) { a = 0.0f; g = 1.0f - smooth((age - (hold + fade)) / shrink); }
                else if (age > hold) a = 1.0f - (age - hold) / fade;
            }
            Shown& s = shown[nShown++];
            memcpy(s.name, l.name, sizeof(s.name));
            memcpy(s.text, l.text, sizeof(s.text));
            s.alpha = a; s.grow = g; s.mine = l.mine; s.system = l.system;
        }
    }
    if (open) drawOpen(shown, nShown);
    else      drawClosed(shown, nShown);
}
