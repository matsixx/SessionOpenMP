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
// SessionOpenMP -- the in-game chat box.
//
// TWO SURFACES, ONE MODEL. The lines, the compose buffer and the send queue all live here; what draws
// them is chosen elsewhere:
//   * the GAME's own text widgets (ui/game_hud.h) -- the default, and what "built into the game" means
//   * the ImGui window below it -- the fallback and the A/B, switched with the names
// Everything in this file is surface-agnostic except Chat_Draw, which is the ImGui one.
//
// WHY THE EDITING IS OURS AND NOT THE GAME'S EDITABLE TEXT BOX. The game does have one -- two, in
// fact: PBP_ReplaySaveUI and PBP_EditSkaterName_Widget both carry a real UEditableTextBox with a
// style, a hint and a font, which is why the old claim in this header that there was "no UI for
// typing" was wrong. The problem is not the widget, it is FOCUS. A Slate editable text only receives
// keys while it holds keyboard focus, and during play the viewport owns input (FInputModeGameOnly):
// focus handed to a widget is taken back on the next tick. Making it stick means putting the player
// controller into GameAndUI for as long as the box is open, which changes mouse capture and cursor
// behaviour mid-session for a text field -- a poor trade.
//
// So the KEYS come from the window hook, which already swallows every keystroke while the box is open
// (overlay.cpp's hkWndProc -- it has to, or a WASD typed mid-sentence rolls you down the street), and
// the LINE is drawn by the game like every other line. The player sees the game's font, the game's
// text; what they do not see is whose caret it is. The cost is that clipboard, selection and IME are
// ours to implement rather than Slate's to provide -- paste is done, the rest is not.
//
// WHY IT IS NOT A PAUSE-MENU PAGE: a menu page is a list of rows the engine builds when a page
// activates, and it cannot take free text at all -- the same reason the join-code prompt lives in
// ImGui (see mp_name.h). Chat also has to be readable and typable WHILE SKATING, which a pause menu
// by definition is not.
//
// THE THREAD RULE IS overlay.h's, because this is drawn from the Present hook:
//   * the RENDER thread owns the window, the input line and the history rendering
//   * the GAME thread pushes received messages in (Chat_Push) and drains typed ones out (Chat_Take)
//   * everything crossing that line goes through the mutex in chat.cpp, and nothing in this file ever
//     touches Unreal.
//
// TWO SURFACES, ONE LAYOUT. Closed, recent lines are drawn straight onto the background draw list at
// measured positions with a dark rim for legibility and nothing behind them: no ImGui window, because
// an auto-sized window takes a frame to learn its own height and a bottom-anchored one therefore
// JUMPS a line every time a message arrives or expires; and no panel, because a box in the corner
// pulls the eye while skating. Open, the same line renderer runs inside a fixed-size window with a
// scrollable history and the input field. Both use the shared look in theme.h.
#pragma once
#include <stdint.h>

// Live knobs, so the look can be corrected in-game without a rebuild.
struct ChatTuning {
    bool  enabled       = true;
    float width         = 620.0f;   // px at 1080p-equivalent; scaled by the ImGui global scale
    float height        = 260.0f;   // history area when open
    float marginX       = 40.0f;    // from the left edge
    float marginY       = 120.0f;   // from the BOTTOM edge
    float fadeAfterSec  = 9.0f;     // how long a message stays visible with the box CLOSED
    float fadeOverSec   = 1.5f;
    float appearSec     = 0.18f;    // a new line grows and fades in over this
    float collapseSec   = 0.22f;    // ...and a faded line gives its height back over this
    float shadowAlpha   = 0.75f;    // the dark rim under closed-box text: the nameplates' outlineAlpha
    int   maxShownIdle  = 6;        // lines drawn when closed
    int   maxShownOpen  = 12;       // ...and when it is open, where there is no fade to thin them out
    // Where the game-drawn box sits (mp_prefs.h says what the numbers mean), and whether the talk is
    // hidden. Published every frame from the prefs, or from the F1 sliders while they are moving.
    float posXPct       = 2.0f;
    float posYPct       = 14.0f;
    bool  hidden        = false;
};
ChatTuning& Chat_Tuning();

// ---- render thread -----------------------------------------------------------------------------
void Chat_Draw();                       // called every frame from the Present hook
bool Chat_IsOpen();                     // true = the box owns the keyboard (the overlay gate reads this)
// The box is open AND holds text: what everyone else's "..." bubble runs on. Atomic like IsOpen --
// written by the render thread's draw, read by the game thread's publish.
bool Chat_IsTyping();
void Chat_SetOpen(bool open);
// True when there is anything on screen -- open, or recent lines still fading. The Present hook only
// pays for an ImGui frame when something actually wants to draw.
bool Chat_HasVisible();

// ---- the open key ------------------------------------------------------------------------------
// Enter is an EVENT, not a polled key state. The window hook (overlay.cpp) reports each fresh press
// that reached the game -- never an auto-repeat, and never while the box, the F1 panel or a prompt
// already owns the keyboard, since those swallow the message before it gets here. The game frame
// takes the press and decides whether it may open the box. Polling GetAsyncKeyState instead missed
// a quick tap across a frame hitch, fired for Enter pressed in OTHER windows while the game ran in
// the background, and raced the render thread's Enter-to-send close into an immediate reopen.
void Chat_NoteEnterPressed();           // window-message thread
bool Chat_TakeEnterPressed();           // game thread; true once per press
// ...and whether that press is OURS to take. Published by the game thread, read by the hook, which
// swallows an Enter it knows will open the box: the chat opens from the pause menu now, and a press
// that fell through to the game would confirm whatever menu row was selected on its way past.
void Chat_SetEnterOurs(bool ours);
bool Chat_EnterOurs();

// ---- game thread -------------------------------------------------------------------------------
// A line to display. `mine` picks the "you" colour; a null/empty name renders as a system line, which
// is how join/leave notices reach the same window without a second mechanism.
void Chat_Push(const char* name, const char* text, bool mine);
// System line (join/leave notices).
void Chat_System(const char* text);
// Drain one line the player typed. Returns false when there is nothing to send. The caller owns
// transmitting it AND echoing it locally -- chat.cpp deliberately does not know a transport exists.
bool Chat_Take(char* out, int cap);
// How many other players are in the session -- shown in the open box's header. Display only.
void Chat_SetPresence(int players);

// ---- typing, when the editing is ours (see the header note) ------------------------------------
// WINDOW-MESSAGE THREAD, from overlay.cpp's hook, and only while the box owns the keyboard. `ch` is
// one typed character; `vk` is a virtual key for the ones that edit rather than insert (backspace,
// delete, the arrows, home/end, escape, enter) plus Ctrl+V. Both are no-ops unless the game is
// drawing the box AND it is open, so the hook does not have to decide anything.
//
// `repeat` IS THE KEY'S AUTO-REPEAT BIT, and it is passed rather than filtered because the two kinds
// of key want opposite things: holding backspace should keep deleting, while the ENTER that opened
// the box is still physically down and its repeats must not send the empty line and shut it again.
void Chat_NoteChar(unsigned int ch);
void Chat_NoteKey(int vk, bool ctrl, bool repeat);

// ---- what the GAME-DRAWN surface reads (ui/game_hud.h). Any thread; the mutex is inside. --------
struct ChatLineView {
    char     text[208];     // "name  what they said", already joined -- one widget draws one line
    // HOW MUCH OF THAT IS THE NAME. The surface draws the whole line in the plain text colour and
    // then draws just the name over the top in the speaker's colour -- same font, same start, so the
    // glyphs land exactly on themselves. That gets a two-tone line out of a widget that can only
    // hold one colour, and without having to guess how wide a proportional name came out.
    uint8_t  nameLen;       // 0 for a system line or anything with no name
    uint32_t ageMs;         // ...and 0 whenever the box is open, which is what holds the fade off
    bool     mine, system;
};
// The lines that should be on screen right now, OLDEST FIRST (so the last one drawn sits lowest).
int  Chat_Lines(ChatLineView* out, int cap);
// The line being typed, with a caret already in it at the right place; empty when the box is shut.
// The caret BLINKS, so this changes on its own and the caller must expect a new string without a
// keystroke behind it.
int  Chat_Compose(char* out, int cap);

// ---- SCROLLING BACK. There is no cursor while the box is open (the window hook swallows the mouse
// so a click cannot reach the game), so the history is walked with the WHEEL and with PageUp/PageDown
// -- both arrive through the same hook as the typing. The offset is in VISUAL LINES from the newest,
// and it is the SURFACE that knows how many there are, so it clamps and writes the answer back.
void Chat_Scroll(int lines);        // window-message thread: + is back into the history
int  Chat_ScrollGet();
void Chat_ScrollClamp(int maxBack); // the surface's answer: this is as far back as there is to go

// How much has been typed, and how much room is left: the character counter in the open box. The
// LIMIT is the model's, not the surface's, so both draw the same number.
int  Chat_TypedLen();
int  Chat_TypedMax();
// How many OTHER players are here, for the box's header. What Chat_SetPresence was last told.
int  Chat_Presence();

// Is the game drawing it? While true Chat_Draw does nothing and Chat_HasVisible is false, so the two
// surfaces can never both be up. Set once a frame by the caller that decides, exactly like the names.
void Chat_SetGameDrawn(bool on);

// PLACING THE BOX from the F1 menu. The sliders call this every frame they are held (render thread):
// for a moment afterwards the box is drawn OPEN at that spot -- there has to be something on screen to
// place -- and Chat_Preview reports the spot to the game thread's publish. Shows even when hidden.
void Chat_PreviewBox(int xPct, int yPct);
bool Chat_Preview(int* xPct, int* yPct);

// The chat's own tunables live above; the LOOK (palette + font) is shared with every other surface
// this mod draws -- see theme.h.
