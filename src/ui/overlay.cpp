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
// SessionOpenMP -- the in-game menu. Dear ImGui drawn from a hooked IDXGISwapChain::Present.
//
// Do not "simplify" the hook machinery below -- every branch is a fact about Session:
//  * the game runs the DX12 RHI (D3D12Core loaded; ID3D11Device off the swapchain fails silently),
//    so the D3D12 path is the live one and D3D11 is the fallback for another RHI.
//  * on DX12 you cannot render to the swapchain without the game's OWN direct command queue, which is
//    captured via an ID3D12CommandQueue::ExecuteCommandLists hook. This is why the menu appears a beat
//    after the game window: Present binds only once that queue has been seen.
//  * Present is found through a THROWAWAY D3D11 swapchain's vtable -- the DXGI swapchain class and its
//    vtable are shared regardless of which device API is behind it.
//  * the window-class check ("UnrealWindow") rejects other swapchains in the process (the EOS overlay).
//
// The session panel toggles on F1 (F8/F9/F6 remain direct hotkeys, so the menu must not steal one),
// and the interface to the rest of the mod is the 3-call thread boundary in overlay.h -- publish a
// snapshot, post an action. No EOS, no session, no Unreal on this thread.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "MinHook.h"
#include "overlay.h"
#include "menu_ext.h"
#include "mp_name.h"
#include "mp_prefs.h"
#include "chat.h"
#include "theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(WINAPI* pfn_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* pfn_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(WINAPI* pfn_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static pfn_Present       o_Present = nullptr;
static pfn_ResizeBuffers o_ResizeBuffers = nullptr;
static pfn_ECL           o_ECL = nullptr;

static std::atomic<bool> g_visible{false};
// ---- the JOIN-CODE PROMPT. A second, much smaller overlay mode: the pause menu can show a code but
// it cannot take one -- Session's editable-text widgets need style assets that are not available, and
// its own name entry is welded into the customization container. So the code is typed into an ImGui
// box that appears ON DEMAND, over the pause menu, and closes itself the moment it has an answer.
// It shares the render/input machinery with the F1 window and NOTHING else: the prompt has its own
// visibility flag, so opening it does not open the dev panel.
enum PromptKind { PK_NONE = 0, PK_CODE, PK_NAME };
static std::atomic<int>  g_promptKind{PK_NONE};
static std::mutex        g_promptMx;
static char              g_promptResult[16] = {0};   // a submitted code, waiting for the game thread
static bool              g_promptHave = false;
static std::atomic<bool> g_nameChanged{false};       // the name box saved a new one
static HWND    g_gameHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool    g_init = false, g_dead = false, g_isD3D12 = false;

// D3D11 path
static ID3D11Device*           g_dev11 = nullptr;
static ID3D11DeviceContext*    g_ctx11 = nullptr;
static ID3D11RenderTargetView* g_rtv11 = nullptr;

// D3D12 path
#define MAX_BB 4
static ID3D12Device*              g_dev12 = nullptr;
static ID3D12CommandQueue*        g_queue12 = nullptr;      // the game's direct queue
static IDXGISwapChain3*           g_swap3 = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*      g_srvHeap = nullptr;
static ID3D12CommandAllocator*    g_alloc12[MAX_BB] = {};
static ID3D12GraphicsCommandList* g_list12 = nullptr;
static ID3D12Resource*            g_bb12[MAX_BB] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtv12[MAX_BB] = {};
static UINT g_bbCount = 0, g_rtvSize = 0;

// direct queues seen through the ExecuteCommandLists hook (the game's is among them;
// matched to the swapchain's device at bind time)
static ID3D12CommandQueue* g_qCand[4] = {};
static volatile LONG g_nQCand = 0;

bool Overlay_Visible() { return g_visible.load() || g_promptKind.load() != PK_NONE; }
void Overlay_PromptCode(bool open) { g_promptKind = open ? PK_CODE : PK_NONE; }
void Overlay_PromptName(bool open) { g_promptKind = open ? PK_NAME : PK_NONE; }
bool Overlay_PromptOpen() { return g_promptKind.load() != PK_NONE; }
bool Overlay_TakeNameChanged() { return g_nameChanged.exchange(false); }
// ---- the DIRECT-CONNECT endpoint, handed across the thread boundary exactly like the join code.
// This file deliberately does not know the transport exists (it is the render thread; it publishes a
// snapshot in and posts actions out), so the typed address is PARKED here and the game thread collects
// it alongside the action it was posted with.
static std::mutex g_directMx;
static char       g_directAddr[128] = {0};
static int        g_directPort = 7777;
static void directPark(const char* addr, int port) {
    std::lock_guard<std::mutex> lk(g_directMx);
    strncpy_s(g_directAddr, addr ? addr : "", _TRUNCATE);
    g_directPort = port;
}
void Overlay_TakeDirect(char* addrOut, int cap, int* portOut) {
    std::lock_guard<std::mutex> lk(g_directMx);
    if (addrOut && cap > 0) strncpy_s(addrOut, (size_t)cap, g_directAddr, _TRUNCATE);
    if (portOut) *portOut = g_directPort;
}

bool Overlay_TakeCode(char* out, int cap) {
    if (!out || cap <= 0) return false;
    std::lock_guard<std::mutex> lk(g_promptMx);
    if (!g_promptHave) return false;
    g_promptHave = false;
    strncpy_s(out, (size_t)cap, g_promptResult, _TRUNCATE);
    return true;
}
static void ovlogf(const char* fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); OvLog(b);
}

static void WINAPI hkECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (q && g_nQCand < 4 && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        bool known = false;
        for (LONG i = 0; i < g_nQCand; i++) if (g_qCand[i] == q) known = true;
        if (!known) { LONG i = InterlockedIncrement(&g_nQCand) - 1; if (i < 4) g_qCand[i] = q; }
    }
    o_ECL(q, n, lists);
}
static ID3D12CommandQueue* matchQueue(ID3D12Device* dev) {
    for (LONG i = 0; i < g_nQCand && i < 4; i++) {
        ID3D12CommandQueue* q = g_qCand[i]; if (!q) continue;
        ID3D12Device* qd = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), (void**)&qd)) && qd) {
            bool m = (qd == dev); qd->Release();
            if (m) return q;
        }
    }
    return nullptr;
}

static LRESULT CALLBACK hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Chat is in this list for the same reason the code prompt is: while it is OPEN the player is
    // typing, and a WASD that reaches the game would send them rolling down the street mid-sentence.
    const bool capturing = g_visible.load() || Overlay_PromptOpen() || Chat_IsOpen();
    if (capturing) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        switch (m) { // swallow input while the menu is up so clicks/keys don't leak into the game
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            return 0;
        }
    } else if (m == WM_KEYUP || m == WM_SYSKEYUP) {
        // RELEASES ALWAYS REACH IMGUI, capturing or not. The capture gate opens and closes
        // MID-KEYSTROKE: the Escape that closes the chat is delivered while capturing, the box closes
        // on it, and the matching KEYUP then arrives with the gate shut -- which leaves ImGui
        // convinced Escape is held DOWN forever. `IsKeyPressed` defaults to repeat=true, so from then
        // on a phantom repeat shuts every chat box within a frame of it opening, permanently, because
        // nothing can ever deliver that release. Handing ImGui a release it did not expect is
        // harmless (it clears a key that is genuinely up); withholding one corrupts its state until
        // restart. The message still falls through to the game below -- this adds a listener, it
        // swallows nothing.
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

// ---- the thread boundary (see overlay.h). A mutex, not a seqlock: this is 60 Hz of one small struct,
// and correctness here is worth more than the handful of nanoseconds a lock-free version would save.
static std::mutex  g_uiMx;
static MpUiState   g_uiState;
static volatile LONG g_pending = OVA_NONE;

void Overlay_Publish(const MpUiState* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lk(g_uiMx);
    g_uiState = *s;
}
int Overlay_TakeAction() { return (int)InterlockedExchange(&g_pending, OVA_NONE); }
static void post(OvAction a) { InterlockedExchange(&g_pending, (LONG)a); }

// ---- the menu-extension seam (menu_ext.h): sections registered by OTHER mod DLLs.
// The api struct hands the guest widget calls that run in OUR ImGui context; the guest never
// links ImGui. Registration may arrive from any thread; drawing happens on the render thread
// under the same mutex, so a mid-registration draw can never see a half-written entry.
static void extText(const char* s)         { if (s) ImGui::TextUnformatted(s); }
static void extTextDisabled(const char* s) { if (s) ImGui::TextDisabled("%s", s); }
static void extTextWrapped(const char* s)  { if (s) ImGui::TextWrapped("%s", s); }
static void extSeparator(void)             { ImGui::Separator(); }
static void extSameLine(void)              { ImGui::SameLine(); }
static bool extCheckbox(const char* l, bool* v)  { return (l && v) ? ImGui::Checkbox(l, v) : false; }
static bool extSliderFloat(const char* l, float* v, float lo, float hi, const char* f)
                                           { return (l && v) ? ImGui::SliderFloat(l, v, lo, hi, f ? f : "%.3f") : false; }
static void extIndent(void)                { ImGui::Indent(); }
static void extUnindent(void)              { ImGui::Unindent(); }
static bool extButton(const char* l)       { return l ? ImGui::Button(l) : false; }
// APPEND-ONLY, and the version number is the guest's only way to know what is safe to call: a
// guest DLL built against v1 has a struct one pointer SHORTER, so new entries go at the END and the
// version goes up with them. Never insert, never reorder.
static const OmpMenuApi g_extApi = {
    2, extText, extTextDisabled, extTextWrapped, extSeparator, extSameLine,
    extCheckbox, extSliderFloat, extIndent, extUnindent,
    extButton,
};
struct ExtSection { char title[48]; OmpMenuDrawFn draw; void* user; bool dead; };
static ExtSection g_ext[8];
static int        g_nExt = 0;
static std::mutex g_extMx;

extern "C" __declspec(dllexport) int OmpMenu_Register(const char* title, OmpMenuDrawFn draw, void* user) {
    if (!title || !draw) return 0;
    std::lock_guard<std::mutex> lk(g_extMx);
    if (g_nExt >= (int)(sizeof(g_ext) / sizeof(g_ext[0]))) { OvLog("[overlay] menu extension table FULL"); return 0; }
    ExtSection& s = g_ext[g_nExt];
    strncpy(s.title, title, sizeof(s.title) - 1); s.title[sizeof(s.title) - 1] = 0;
    s.draw = draw; s.user = user; s.dead = false;
    g_nExt++;
    ovlogf("[overlay] menu extension registered: \"%s\"", s.title);
    return 1;
}

// A guest fault must not take the render thread (and the game) with it. SEH lives in its own
// C-shaped helper: a function with __try may not contain objects needing unwinding.
static bool extCallGuarded(OmpMenuDrawFn fn, void* user) {
    __try { fn(&g_extApi, user); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void drawExtSections() {
    std::lock_guard<std::mutex> lk(g_extMx);
    for (int i = 0; i < g_nExt; i++) {
        ExtSection& s = g_ext[i];
        if (s.dead) continue;
        ImGui::Separator();
        if (ImGui::CollapsingHeader(s.title, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!extCallGuarded(s.draw, s.user)) {
                s.dead = true;   // drawn never again; the game outlives the guest's bug
                ovlogf("[overlay] menu extension \"%s\" FAULTED -- disabled for this run", s.title);
            }
        }
    }
}

// The typing box -- join code or multiplayer name. Centred, small, styled like the chat window
// (theme.h), and it OWNS the keyboard while it is up (the wndproc swallows input for it exactly as it
// does for the F1 window), so typing cannot leak into the game behind it.
//
// ONE box for both, because they differ only in their strings and their validation: two near-identical
// copies would drift the first time either was touched, which is the same argument theme.h makes about
// the palette. The kind is read ONCE per frame into a local, so a close from inside the body cannot
// make the rest of the frame draw the other box.
static void buildPrompt() {
    const int kind = g_promptKind.load();
    if (kind == PK_NONE) return;
    const bool isName = (kind == PK_NAME);

    static char buf[40] = {0};
    static int  primedFor = PK_NONE;
    static char msg[96] = {0};
    static bool msgOk = false;
    if (primedFor != kind) {
        primedFor = kind;
        msg[0] = 0; msgOk = false;
        // The name box opens on your CURRENT name, so a small edit is a small edit; the code box is
        // always blank, because a stale code is never the one you want.
        if (isName) strncpy_s(buf, MpName_Get(), _TRUNCATE); else buf[0] = 0;
    }
    const auto close = [&]() { primedFor = PK_NONE; msg[0] = 0; g_promptKind = PK_NONE; };

    const ThemePalette& T = Theme();
    const ImGuiIO& io = ImGui::GetIO();
    const float sc = io.FontGlobalScale > 0.01f ? io.FontGlobalScale : 1.0f;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400 * sc, 0), ImGuiCond_Always);

    Theme_Push(true);                       // it owns the screen while it is up
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * sc, 16 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8 * sc, 8 * sc));
    if (ImGui::Begin("##ompprompt", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(T.dim, isName ? "YOUR NAME" : "JOIN A PRIVATE GAME");
        ImGui::Separator();
        ImGui::TextColored(T.dim, isName ? "Everyone in the session sees this."
                                         : "Type the 6-character code the host gave you.");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        // Codes are generated uppercase and matched exactly, so force the case as you type; a name is
        // whatever the player wants (within the filter).
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
            (isName ? 0 : (ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank));
        const bool entered = ImGui::InputText("##omppromptin", buf,
                                              isName ? sizeof(buf) : 8, flags);
        const bool ready = isName ? (buf[0] != 0) : (strlen(buf) >= 6);

        ImGui::BeginDisabled(!ready);
        const bool go = ImGui::Button(isName ? "  Save  " : "  Join  ");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(" Cancel ") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { close(); }
        else if ((entered || go) && ready) {
            if (isName) {
                // Validated and saved HERE -- MpName_Set already runs on this thread for the F1
                // panel's field, and a rejected name has to say WHY on the spot rather than being
                // silently swallowed on its way to the game thread.
                msgOk = MpName_Set(buf, msg, sizeof(msg));
                if (msgOk) { g_nameChanged = true; close(); }
            } else {
                { std::lock_guard<std::mutex> lk(g_promptMx);
                  strncpy_s(g_promptResult, buf, _TRUNCATE); g_promptHave = true; }
                close();
            }
        }
        if (!isName && !ready && buf[0]) ImGui::TextColored(T.dim, "%d of 6 characters", (int)strlen(buf));
        if (msg[0]) ImGui::TextColored(msgOk ? T.dim : T.warn, "%s", msg);
        ImGui::TextColored(T.dim, "ENTER confirm    ESC cancel");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    Theme_Pop();
}

static void buildUI() {
    if (!g_visible.load()) return;      // the prompt can request a frame without opening the panel
    MpUiState st;
    { std::lock_guard<std::mutex> lk(g_uiMx); st = g_uiState; }

    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 470), ImGuiCond_FirstUseEver);
    // Same look as the chat box and the pause menu (theme.h), so the mod's surfaces read as one.
    Theme_Push(true);
    const bool panelOpen = ImGui::Begin("SessionOpenMP  (F1 closes)");
    if (panelOpen) {
        // ---- status: everything needed to tell "it is working" from "it is not", without the log.
        const char* bk = (st.backend == 2) ? "shared memory (this PC)"
                       : (st.backend == 1) ? "EOS (online)" : "-";
        if (st.armed) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "SESSION ACTIVE");
            ImGui::SameLine(); ImGui::TextDisabled("via %s", bk);
            ImGui::Text("players connected: %d     skaters drawn: %d     sending: %.0f Hz",
                        st.peers, st.proxies, st.pubHz);
            if (st.peers == 0)
                ImGui::TextDisabled("waiting for the other player -- they must also press Host/Join");
            else if (st.proxies == 0)
                ImGui::TextDisabled("connected, but no skater yet -- are you both loaded into a map?");
        } else if (st.tpState == 1) {
            ImGui::TextDisabled("signing in to EOS...");
        } else if (st.tpState == 3) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "transport failed -- see SessionOpenMP_eos.log");
        } else if (st.lobby == -1) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "last attempt failed -- nobody hosting? try again");
        } else if (st.backend != 0) {
            // transport up but no lobby: after an F6 leave or a failed attempt. Clicks from here
            // re-request the lobby or switch backends.
            ImGui::TextDisabled("no session -- transport ready via %s", bk);
        } else {
            ImGui::TextDisabled("no session yet");
        }
        if (st.myId[0]) { ImGui::SameLine(); ImGui::TextDisabled("   id: %s", st.myId); }

        ImGui::Separator();
        // ---- YOUR NAME. The one place in the mod you can actually type: the pause menu can show
        // this name but has no text-entry widget of its own (the game's editable-text widgets need
        // style assets that are not available). Applied through the filter, saved to disk, and
        // published in the cosmetics packet's existing name field -- see mp_name.h.
        {
            static char nameBuf[OMP_NAME_MAX + 1] = {0};
            static char nameMsg[128] = {0};
            static bool nameOk = true;
            static bool primed = false;
            if (!primed) { primed = true; strncpy_s(nameBuf, MpName_Get(), _TRUNCATE); }
            ImGui::Text("Your name");
            ImGui::SetNextItemWidth(220);
            const bool entered = ImGui::InputText("##ompname", nameBuf, sizeof(nameBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button(" Set ") || entered) {
                nameOk = MpName_Set(nameBuf, nameMsg, sizeof(nameMsg));
                if (nameOk) snprintf(nameMsg, sizeof(nameMsg), "Saved.");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("shown as: %s", MpName_Get());
            if (nameMsg[0]) {
                if (nameOk) ImGui::TextDisabled("%s", nameMsg);
                else        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", nameMsg);
            }
        }

        ImGui::Separator();
        // ---- PRIVACY. Writing a preference is pure storage (mp_prefs.h): it bumps a generation and
        // the GAME thread turns it into the SDK call, because this is the render thread and the EOS
        // platform must only be driven from the thread that ticks it.
        {
            bool hide = MpPrefs_HideAddress();
            if (ImGui::Checkbox("Hide my IP address", &hide)) MpPrefs_SetHideAddress(hide);
            ImGui::TextDisabled(hide
                ? "Traffic goes through Epic's relays. Peers never see your IP."
                : "Direct connections when possible: lower latency, but peers can see your IP.");
            ImGui::TextDisabled("Applies to new connections -- it does not re-route a session already up.");
        }

        ImGui::Separator();
        const bool busy = (st.tpState == 1);
        const bool live = st.armed;

        // ---- ONLINE. The only mode a player is expected to use, so it leads.
        ImGui::Text("Online (another player, another PC)");
        ImGui::TextDisabled("Signs in to Epic's relay. Both players load the same map, one Hosts.");
        ImGui::BeginDisabled(busy || live);
        if (ImGui::Button("  Host online  "))  { post(OVA_HOST_ONLINE); }
        ImGui::SameLine();
        if (ImGui::Button("  Join online  "))  { post(OVA_JOIN_ONLINE); }
        ImGui::EndDisabled();
        ImGui::TextDisabled("(Join finds any open SessionOpenMP session)");

        ImGui::Separator();
        ImGui::BeginDisabled(!live);
        if (ImGui::Button("  Leave session  ")) { post(OVA_LEAVE); }
        ImGui::EndDisabled();
        if (live) ImGui::TextDisabled("Leave first to switch between this-PC and online.");

        ImGui::Separator();
        // ---- DIRECT CONNECT. No Epic at all: the host opens a port, the joiner is given an address.
        // THIS SECTION ONLY EXISTS IN F1, and it has to: joining needs a typed address and the
        // game's pause menu has no text-entry widget (mp_name.h explains why). Collapsed by default
        // because it is the harder path -- it works for LAN and for a host who can forward a port,
        // and it asks the player to know something about their network.
        if (ImGui::CollapsingHeader("Direct connect (no Epic account)")) {
            static char addrBuf[128] = {0};
            static int  portBuf = 7777;

            ImGui::TextDisabled("For people who cannot or would rather not use Epic Online Services.");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Not encrypted, and the people you play with see your IP address.");

            ImGui::Text("Host");
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("port##ompport", &portBuf);
            if (portBuf < 1)     portBuf = 1;
            if (portBuf > 65535) portBuf = 65535;
            ImGui::BeginDisabled(busy || live);
            if (ImGui::Button("  Open this port  ")) {
                directPark(addrBuf, portBuf);          // parked BEFORE the action is posted
                post(OVA_HOST_DIRECT);
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Others reach you at <your IP>:%d. On a LAN that is your local address;", portBuf);
            ImGui::TextDisabled("over the internet it is your public one AND this port must be forwarded.");
            if (st.bound[0]) ImGui::Text("listening on %s", st.bound);

            ImGui::Spacing();
            ImGui::Text("Join");
            ImGui::SetNextItemWidth(240);
            const bool entered = ImGui::InputText("##ompaddr", addrBuf, sizeof(addrBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || live || !addrBuf[0]);
            if (ImGui::Button(" Connect ") || (entered && addrBuf[0] && !busy && !live)) {
                directPark(addrBuf, portBuf);
                post(OVA_JOIN_DIRECT);
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Their address and port, e.g. 203.0.113.7:7777 (or 192.168.1.20:7777 on a LAN).");
        }

        ImGui::Separator();
        // ---- DEV TOOLS. The same-PC (shared memory) rig lives here and ONLY here: it is a
        // development wire, not a way to play, and in the pause menu a player would reasonably try it
        // and end up in a session with nobody. Collapsed by default.
        if (ImGui::CollapsingHeader("Dev tools")) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "For testing on ONE PC. Not for playing with friends.");
            ImGui::TextDisabled("Two windows of the game talk through a shared-memory mailbox: no sign-in,");
            ImGui::TextDisabled("no network. Launch the game twice, load the SAME map in both, then press");
            ImGui::TextDisabled("Host in one window and Join in the other.");
            ImGui::BeginDisabled(busy || live);
            if (ImGui::Button("  Host on this PC  ")) { post(OVA_HOST_LOCAL); }
            ImGui::SameLine();
            if (ImGui::Button("  Join on this PC  ")) { post(OVA_JOIN_LOCAL); }
            ImGui::EndDisabled();
            ImGui::TextDisabled("(either order works -- both just claim a mailbox slot)");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Restart the game before going online after using this.");
            ImGui::TextDisabled("Switching this-PC -> online inside one session can stall the Epic");
            ImGui::TextDisabled("sign-in. A fresh launch signs in normally.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Keyboard: F8 host / F9 join (this PC), F6 leave, F1 this menu.");
        ImGui::TextDisabled("Everyone keeps playing their own game -- nobody is a server.");

        drawExtSections();   // sections registered by other mod DLLs (menu_ext.h)
    }
    ImGui::End();
    Theme_Pop();
}

// ------------------------------------------------------------------ D3D11 render
static void renderD3D11(IDXGISwapChain* sc) {
    if (!g_rtv11) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            g_dev11->CreateRenderTargetView(bb, nullptr, &g_rtv11); bb->Release();
        }
        if (!g_rtv11) return;
    }
    // The game's own typeface arrives from the game thread; swapping it means rebuilding the
    // atlas, which may only happen BETWEEN frames -- hence here, and hence the backend's
    // device objects going with it so the new texture is uploaded.
    if (Theme_ConsumePendingFont()) ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    buildUI();
    buildPrompt();
    Chat_Draw();
    ImGui::Render();
    g_ctx11->OMSetRenderTargets(1, &g_rtv11, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ------------------------------------------------------------------ D3D12 render
static void releaseBB12() {
    for (UINT i = 0; i < MAX_BB; i++) { if (g_bb12[i]) { g_bb12[i]->Release(); g_bb12[i] = nullptr; } }
    g_bbCount = 0;
}
static bool acquireBB12(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d; if (FAILED(sc->GetDesc(&d))) return false;
    g_bbCount = d.BufferCount; if (g_bbCount > MAX_BB) g_bbCount = MAX_BB; if (!g_bbCount) return false;
    for (UINT i = 0; i < g_bbCount; i++) {
        if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_bb12[i])) || !g_bb12[i]) { releaseBB12(); return false; }
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_rtvSize;
        g_dev12->CreateRenderTargetView(g_bb12[i], nullptr, h);
        g_rtv12[i] = h;
    }
    return true;
}
static void renderD3D12(IDXGISwapChain* sc) {
    if (!g_bbCount && !acquireBB12(sc)) return;   // (re)acquired lazily after ResizeBuffers
    UINT idx = g_swap3->GetCurrentBackBufferIndex(); if (idx >= g_bbCount) return;
    // The game's own typeface arrives from the game thread; swapping it means rebuilding the
    // atlas, which may only happen BETWEEN frames -- hence here, and hence the backend's
    // device objects going with it so the new texture is uploaded.
    if (Theme_ConsumePendingFont()) ImGui_ImplDX12_InvalidateDeviceObjects();
    ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    buildUI();
    buildPrompt();
    Chat_Draw();
    ImGui::Render();
    // by the time buffer idx comes around again its previous frame is done (Present-throttled)
    g_alloc12[idx]->Reset();
    g_list12->Reset(g_alloc12[idx], nullptr);
    D3D12_RESOURCE_BARRIER b; ZeroMemory(&b, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_bb12[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list12->ResourceBarrier(1, &b);
    g_list12->OMSetRenderTargets(1, &g_rtv12[idx], FALSE, nullptr);
    g_list12->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list12);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_list12->ResourceBarrier(1, &b);
    g_list12->Close();
    g_queue12->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_list12);
}

// ------------------------------------------------------------------ bind & hooks
static void bindCommon(HWND hwnd) {
    g_gameHwnd = hwnd;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDrawCursor = true;    // the game may keep the HW cursor hidden; draw our own
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.3f); io.FontGlobalScale = 1.3f;
    ImGui_ImplWin32_Init(hwnd);
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
}
static void initFromSwapchain(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d; if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;
    char cls[32] = {0}; GetClassNameA(d.OutputWindow, cls, sizeof(cls));
    if (strcmp(cls, "UnrealWindow")) return;   // some other swapchain (EOS overlay etc.)

    ID3D11Device* d11 = nullptr; ID3D12Device* d12 = nullptr;
    HRESULT h11 = sc->GetDevice(__uuidof(ID3D11Device), (void**)&d11);
    HRESULT h12 = d11 ? E_FAIL : sc->GetDevice(__uuidof(ID3D12Device), (void**)&d12);
    static bool logged = false;
    if (!logged) {
        ovlogf("[overlay] game swapchain: hwnd=%p fmt=%d buffers=%u d3d11=0x%08lX d3d12=0x%08lX",
               (void*)d.OutputWindow, (int)d.BufferDesc.Format, d.BufferCount,
               (unsigned long)h11, (unsigned long)h12);
        logged = true;
    }
    if (d11) {
        g_dev11 = d11; g_dev11->GetImmediateContext(&g_ctx11);
        bindCommon(d.OutputWindow);
        ImGui_ImplDX11_Init(g_dev11, g_ctx11);
        g_isD3D12 = false; g_init = true;
        ovlogf("[overlay] ready (D3D11) - press F1 for the menu");
        return;
    }
    if (!d12) { g_dead = true; OvLog("[overlay] swapchain device is neither D3D11 nor D3D12  -- no menu; F8/F9/F6 hotkeys still work"); return; }

    // D3D12: we also need the game's direct queue; wait until the ECL hook has seen it
    ID3D12CommandQueue* q = matchQueue(d12);
    if (!q) { static bool once = false; if (!once) { OvLog("[overlay] d3d12: waiting to capture the game's command queue..."); once = true; }
        d12->Release(); return; }
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap3)) || !g_swap3) {
        d12->Release(); g_dead = true; OvLog("[overlay] no IDXGISwapChain3  -- no menu; F8/F9/F6 hotkeys still work"); return; }
    g_dev12 = d12; g_queue12 = q;

    D3D12_DESCRIPTOR_HEAP_DESC hd; ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = MAX_BB;
    if (FAILED(g_dev12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap))) { g_dead = true; OvLog("[overlay] rtv heap failed"); return; }
    g_rtvSize = g_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ZeroMemory(&hd, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap))) { g_dead = true; OvLog("[overlay] srv heap failed"); return; }
    for (UINT i = 0; i < MAX_BB; i++)
        if (FAILED(g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc12[i]))) { g_dead = true; OvLog("[overlay] allocator failed"); return; }
    if (FAILED(g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc12[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list12))) { g_dead = true; OvLog("[overlay] command list failed"); return; }
    g_list12->Close();

    bindCommon(d.OutputWindow);
    ImGui_ImplDX12_Init(g_dev12, (int)(d.BufferCount ? d.BufferCount : 3), d.BufferDesc.Format, g_srvHeap,
                        g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                        g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    g_isD3D12 = true; g_init = true;
    ovlogf("[overlay] ready (D3D12, %u buffers) - press F1 for the menu", d.BufferCount);
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_dead) {
        if (!g_init) initFromSwapchain(sc);
        if (g_init) {
            DXGI_SWAP_CHAIN_DESC d;
            if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow == g_gameHwnd) {
                static bool f1Held = false;
                bool now = (GetAsyncKeyState(VK_F1) & 0x8000) && GetForegroundWindow() == g_gameHwnd;
                if (now && !f1Held) g_visible = !g_visible.load();
                f1Held = now;
                if (g_visible.load() || Overlay_PromptOpen() || Chat_HasVisible()) {
                    if (g_isD3D12) renderD3D12(sc); else renderD3D11(sc);
                }
            }
        }
    }
    return o_Present(sc, sync, flags);
}

static HRESULT WINAPI hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv11) { g_rtv11->Release(); g_rtv11 = nullptr; }   // both recreated lazily
    releaseBB12();
    return o_ResizeBuffers(sc, n, w, h, f, fl);
}

static DWORD WINAPI ovlThread(LPVOID) {
    HMODULE d3d = nullptr;
    for (int i = 0; i < 240 && !d3d; i++) { d3d = GetModuleHandleA("d3d11.dll"); if (!d3d) Sleep(500); }
    if (!d3d) { OvLog("[overlay] d3d11.dll never loaded  -- no menu; F8/F9/F6 hotkeys still work"); return 0; }
    Sleep(2000); // let the game create its real device/swapchain first

    MH_STATUS ms = MH_Initialize();   // dllmain's EOS tick hook may already have init'd it
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) { ovlogf("[overlay] MinHook init failed (%d)", ms); return 0; }

    // D3D12 queue capture first, so the game's queue is known by the time Present binds.
    if (GetModuleHandleA("d3d12.dll")) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev)) && dev) {
            D3D12_COMMAND_QUEUE_DESC qd; ZeroMemory(&qd, sizeof(qd));
            ID3D12CommandQueue* q = nullptr;
            if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
                void** vt = *(void***)q;
                if (MH_CreateHook(vt[10] /*ExecuteCommandLists*/, (void*)&hkECL, (void**)&o_ECL) == MH_OK)
                    MH_EnableHook(vt[10]);
                q->Release();
            }
            dev->Release();
        }
    }

    // throwaway D3D11 swapchain just to read the shared DXGI vtable
    WNDCLASSEXA wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "SMPOvlDummy";
    RegisterClassExA(&wc);
    HWND hw = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = 2; sd.BufferDesc.Height = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, nullptr);
    if (FAILED(hr) || !sc) {
        ovlogf("[overlay] dummy swapchain failed (0x%08lX)  -- no menu; F8/F9/F6 hotkeys still work", (unsigned long)hr);
        if (hw) DestroyWindow(hw);
        return 0;
    }
    void** vt = *(void***)sc;
    void* pPresent = vt[8]; void* pResize = vt[13];
    sc->Release(); dev->Release(); DestroyWindow(hw); UnregisterClassA(wc.lpszClassName, wc.hInstance);

    if (MH_CreateHook(pPresent, (void*)&hkPresent, (void**)&o_Present) != MH_OK || MH_EnableHook(pPresent) != MH_OK) {
        OvLog("[overlay] Present hook failed  -- no menu; F8/F9/F6 hotkeys still work"); return 0;
    }
    if (MH_CreateHook(pResize, (void*)&hkResizeBuffers, (void**)&o_ResizeBuffers) == MH_OK) MH_EnableHook(pResize);
    OvLog("[overlay] Present hooked -- waiting for the game swapchain");
    return 0;
}

void Overlay_Install() { CreateThread(nullptr, 0, ovlThread, nullptr, 0, nullptr); }
