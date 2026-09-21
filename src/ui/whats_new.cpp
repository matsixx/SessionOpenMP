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
#include "whats_new.h"
#include "trx_popup.h"
#include "nacon_panel.h"
#include "news_panel.h"
#include "mp_prefs.h"
#include "update_check.h"
#include "version_tag.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace omp { namespace ui {

// ---- THE NOTES ------------------------------------------------------------------------------------
// Newest first, a handful of lines each, written for a PLAYER: what changed that they will notice, not
// how it was done. Keep each release to about six short lines -- the popup is one page and a wall of
// text is read by nobody.
// WHEN CUTTING A RELEASE: add the new block at the TOP and bump OMP_VERSION_STRING. The popup then
// appears once for everyone on their first launch of it. Nothing has to be deleted from the bottom --
// `show` fits as many as the panel holds and stops -- but the oldest ones below the cut are dead
// weight in the binary, so a handful is plenty.
struct Release { const char* version; const char* notes; };

// THE WHOLE BODY MUST STAY UNDER ~1000 CHARACTERS. The popup's text goes through an FName, and FName's
// limit is NAME_SIZE (1024) -- overrun it and the widget renders the words ERROR_NAME_SIZE_EXCEEDED
// instead of anything else (field 2026-09-20, at 1600). `show` measures and trims, but keep the source
// short enough that trimming never has to happen: budget ~300 characters a release, three releases.
static const Release kReleases[] = {
    { "1.2.6",
      "- Fixes 1.2.5: the pause menu's confirm dialogs work again.\n"
      "- Hold-to-apply on a graphics change could not be used." },
    { "1.2.5",
      "- Fixes a crash when changing maps with other players around.\n"
      "- Speech bubbles have a panel and a tail; the name sits on them.\n"
      "- The pause menu no longer loses control to your skater.\n"
      "- Rejoining a same-PC test session works from both sides." },
    { "1.2.4",
      "- Names and chat are drawn with the game's own UI now.\n"
      "- The chat box has a frosted panel, a header and a character counter.\n"
      "- Scroll it back with the wheel; chat from the pause menu too.\n"
      "- Lobbies of 32 players, up from 16." },
    { "1.2.3",
      "- The object dropper syncs 1024 objects, not 256.\n"
      "- ...and stops re-sending the whole list every 6 seconds.\n"
      "- Throw the board exactly where you look, down included.\n"
      "- How hard you throw is how fast you pull the trigger." },
    { "1.2.2",
      "- Release notes on the start menu, pulled from the GitHub release.\n"
      "- Update from inside the game: Multiplayer options, hold X on Update.\n"
      "- The hand actually holds the board, and you can pose it yourself.\n"
      "- Throw your board by holding LT and pulling RT; RB holds it for a tap." },
    { "1.2.1",
      "- Radios carry properly now, and volume sets how far one reaches.\n"
      "- Each radio has its own volume; other people's start quiet.\n"
      "- Your own radio plays your stream, and music no longer restarts.\n"
      "- Other players' legs stay put when you walk into them." },
    { "1.2.0",
      "- Emotes: wave, thumbs up, point, facepalm, clap, board tap, dance.\n"
      "- A radio you can carry, set down, and stream your PC through.\n"
      "- Watched players no longer twitch at the end of an emote.\n"
      "- Lobbies of 10+: the 9th player and beyond now sync." },
    { "1.1.12",
      "- Players who leave are really dropped, not left as ghosts.\n"
      "- Saved replays keep the other players who were in them." },
    { "1.1.11",
      "- No limit on how many custom maps you can install.\n"
      "- SessionTweaks: first person while skating." },
    { "1.1.10",
      "- Newcomers turn up in a lobby that has been running a while." },
    { "1.1.9",
      "- Custom maps appear in the Select Map screen." },
};
// HOW MANY OF THESE ACTUALLY SHOW is not decided here -- `show` fits releases into the panel until the
// next one would run past the bottom or past FName's limit, and stops. A short release therefore buys
// more history for free. The notes are for a player glancing at them, not an archive: the full history
// is in the changelog on the release page.
static const int kReleaseAll = (int)(sizeof(kReleases) / sizeof(kReleases[0]));

// ---- ------------------------------------------------------------------------------------------------
static bool g_shown = false;      // this run: never ask the popup manager twice in one session

bool WhatsNew_Pending() {
    if (g_shown) return false;
    const char* seen = MpPrefs_SeenVersion();
    return !seen || strcmp(seen, OMP_VERSION_STRING) != 0;
}

// FName's NAME_SIZE. Go past it and the popup shows ERROR_NAME_SIZE_EXCEEDED and nothing else, so the
// body is built into a buffer that CANNOT reach it and is cut on a line boundary if it ever tries.
// Sized for the SCROLLING panel, which has no page to run off the bottom of -- the whole history can
// go in. The fixed message box takes as much of it as its lines allow.
static const int kBodyMax = 8000;
// ...unless FText::FromString did not resolve, in which case the text still goes in through an FName
// and MUST stay under NAME_SIZE. `show` uses this budget, so a build where the decode failed simply
// shows a shorter page instead of the words ERROR_NAME_SIZE_EXCEEDED.
static const int kBodyMaxFName = 950;
// HOW MANY LINES THE PANEL DRAWS, and the width it wraps at. Both measured off the rendered panel
// (2026-09-20): the text block wraps at about 84 characters, and the box holds about 27 of the
// resulting lines before it runs out of bottom.
// IT IS THE WRAPPED LINES THAT COUNT. Counting SOURCE lines under-counts badly -- a release-note
// bullet is routinely two or three lines once drawn -- which is why the page kept stopping with room
// to spare. Raise these together with kBoxH in nacon_panel.cpp if the box ever grows.
static const int kMaxLines   = 27;
static const int kWrapChars  = 84;
// The notes stop this many lines short of the bottom: running right up to the edge looks like the
// page was cut off rather than ended, and the sign-off below needs somewhere to sit.
static const int kTrimLines  = 3;
// The last line, after a blank one. The panel is a summary; the releases page is the whole thing.
static const char* const kMoreLine = "Read more on GitHub";
// What one source line costs on screen.
static int drawnLines(const char* line, int len) {
    if (len <= 0) return 1;
    return (len + kWrapChars - 1) / kWrapChars;
    (void)line;
}

// ---- WHERE THE TEXT COMES FROM ----------------------------------------------------------------------
// THE PUBLISHED GITHUB RELEASES, when they have arrived: update_check's one-shot worker asks for the
// release list alongside the version check and keeps each release's tag and body. So the panel shows
// what is actually on the releases page, and nothing here has to be kept in sync by hand.
// kReleases above is the FALLBACK -- offline, rate-limited, or simply asked before the answer landed.
// `outVer`/`outNotes` are filled for entry `i`; false = no more.
static bool releaseAt(int i, char* ver, int verCap, char* notes, int notesCap) {
    if (UpdateCheck_NotesCount() > 0) return UpdateCheck_NoteAt(i, ver, verCap, notes, notesCap);
    if (i >= kReleaseAll) return false;
    snprintf(ver,   (size_t)verCap,   "%s", kReleases[i].version);
    snprintf(notes, (size_t)notesCap, "%s", kReleases[i].notes);
    return true;
}
static bool usingGitHub() { return UpdateCheck_NotesCount() > 0; }

// "Are the notes on screen" / "take them down", whichever widget they ended up in. Asking only the
// message panel would have left a news article up for ever the moment the better path started working.
static bool PanelShowing() { return NewsPanel_Showing() || NaconPanel_Showing(); }
static void PanelClose()   { NewsPanel_Close(); NaconPanel_Close(); }

static bool show(void (*logf)(const char*)) {
    char body[kBodyMax + 1];
    const int cap = NaconPanel_LongTextOk() ? (int)sizeof(body) : kBodyMaxFName + 1;
    // NO LINE BUDGET WHEN IT SCROLLS. The news article widget's body sits in a scroll box, so there is
    // no bottom edge to stop short of and every release can go in; the fixed message box still has to
    // be fitted. 0 means "do not count".
    const int lineBudget = NewsPanel_Available() ? 0 : kMaxLines - kTrimLines;
    int n = 0;
    // AS MANY RELEASES AS FIT, newest first. The panel holds a fixed number of LINES, so each release
    // is measured before it goes in and the first one that would run past the bottom stops the list.
    // Better a short page than words spilling off the panel -- and when a release is short, more of
    // the history shows for free.
    int lines = 1;                                   // the title line
    for (int i = 0; n < cap - 1; i++) {
        char ver[32], notes[2048];
        if (!releaseAt(i, ver, sizeof(ver), notes, sizeof(notes))) break;
        const int head = snprintf(body + n, (size_t)cap - (size_t)n, "%s%s\n", i ? "\n" : "", ver);
        if (head < 0 || n + head >= cap - 8) break;                    // no room even for the heading
        if (lineBudget && lines + 2 >= lineBudget) break;
        n += head; lines += (i ? 2 : 1);
        // AS MANY WHOLE LINES AS FIT, not all-or-nothing. A release whose notes overrun what is left
        // used to be dropped entirely, which threw away a screenful of room to avoid half a line --
        // real release notes are long and the newest one can easily fill the page on its own. Cutting
        // on a line boundary means the page always ends on a complete thought.
        const char* c = notes;
        bool wroteAny = false;
        while (*c) {
            const char* eol = strchr(c, '\n');
            const int len = eol ? (int)(eol - c) : (int)strlen(c);
            if (n + len + 2 >= cap) break;
            if (lineBudget && lines + drawnLines(c, len) > lineBudget) break;
            memcpy(body + n, c, (size_t)len);
            n += len; body[n++] = '\n'; body[n] = 0;
            lines += drawnLines(c, len); wroteAny = true;
            // PAST the text just written, including on the LAST line, which has no newline after it.
            // Leaving `c` on that line made the "did it all fit?" test below read a line that had in
            // fact been written and conclude the page was full -- so only ever ONE release appeared,
            // with half the panel empty under it.
            c += len;
            if (!eol) break;
            c++;                                       // step over the newline
        }
        if (*c) break;                                // ran out of page part-way: nothing more fits
        if (!wroteAny) break;
    }
    // ...and the sign-off, after a blank line. It is written outside the budget above on purpose --
    // kTrimLines holds room for exactly this, so the last release can never crowd it off the page.
    if (n + (int)strlen(kMoreLine) + 3 < cap)
        n += snprintf(body + n, (size_t)cap - (size_t)n, "\n%s\n", kMoreLine);
    // NO "press Y" line. It is not Y on every pad -- it is Triangle on a PlayStation one -- and naming
    // the wrong button is worse than naming none. The panel's own continue button is the prompt, and
    // the game draws the right glyph on it for whatever pad is plugged in.
    // NO VERSION IN THE TITLE. It would be the INSTALLED one, and the notes below are whatever is
    // published -- so somebody running an old build got a heading claiming their version above
    // releases that came after it. Each release is labelled with its own number anyway.
    char title[64];
    snprintf(title, sizeof(title), "SessionOpenMP -- what's new");
    // THREE PLACES TO PUT IT, best first, each falling through to the next on any failure:
    //   1. the NEWS ARTICLE widget -- a real title, a scrolling body, the game's own article styling;
    //   2. the MyNacon MESSAGE PANEL -- wraps and scales, but one fixed screenful;
    //   3. the plain dialog -- no wrapping at all, so the notes run off the bottom, but it is always
    //      there, where both widget classes have to be resident to be used.
    // Each is our OWN instance of a game widget; none of them touches the system that owns it.
    char page[kBodyMax + 96];
    snprintf(page, sizeof(page), "%s\n\n%s", title, body);
    if (!NewsPanel_Show(title, body, nullptr, logf) &&
        !NaconPanel_Show(page, "Close", logf) &&
        !TrxPopup_Show(title, body, "OK", logf)) return false;
    g_shown = true;
    MpPrefs_SetSeenVersion(OMP_VERSION_STRING);
    if (logf) {
        char m[160];
        snprintf(m, sizeof(m), "[whatsnew] showed the notes for %s (%s, %d chars)", OMP_VERSION_STRING,
                 usingGitHub() ? "from the GitHub releases" : "built in -- GitHub not answered yet", n);
        logf(m);
    }
    return true;
}

bool WhatsNew_ShowIfDue(void (*logf)(const char*)) {
    if (!WhatsNew_Pending()) return false;
    // Not shown yet is not a failure: the popup manager may simply not be up. Leave it pending and
    // try again on the next menu draw -- the same rule the update notice follows.
    return show(logf);
}

bool WhatsNew_ShowNow(void (*logf)(const char*)) {
    g_shown = false;                       // asked for deliberately: show it however many times they ask
    return show(logf);
}

// ---- UP FOR AS LONG AS THE START MENU IS ------------------------------------------------------------
// Not a once-per-build popup with a key to bring it back. Every button a controller has at a menu
// already belongs to the game: a Y tap opened the notes whenever the player pressed Y for the menu's
// own purposes, and a Y hold could not be shown to work at all. So there is no key -- the notes are
// simply part of the start menu, and the preference turns them off for good.
// Call every frame. `want` is "the start menu is up and the player wants them".
void WhatsNew_KeepUp(bool want, void (*logf)(const char*)) {
    static uint64_t nextTry = 0;
    static int fails = 0;
    if (!want) {
        if (PanelShowing()) { PanelClose(); if (logf) logf("[whatsnew] the notes came down"); }
        fails = 0; nextTry = 0;
        return;
    }
    // THE GITHUB NOTES CAN ARRIVE AFTER THE PANEL IS ALREADY UP -- the request is fired at start-up
    // and the title screen can beat it. Rather than leaving the built-in fallback on screen until the
    // next launch, the panel is rebuilt once, when the real notes land.
    static unsigned shownGen = 0;
    NewsPanel_Tick();                         // scrolls the notes; harmless when they are not up
    if (PanelShowing()) {
        const unsigned gen = UpdateCheck_NotesGeneration();
        if (gen == shownGen) return;
        shownGen = gen;
        PanelClose();                             // and fall through: it is rebuilt below, this frame
    }
    // Building it is not free and can fail (the widget class may not be resident yet), so retry at a
    // second's pace and give up after a while rather than churning every frame for a whole session.
    const uint64_t now = GetTickCount64();
    if (fails > 15 || now < nextTry) return;
    nextTry = now + 1000;
    shownGen = UpdateCheck_NotesGeneration();
    if (show(logf)) fails = 0;
    else            fails++;
}

} }  // namespace omp::ui
