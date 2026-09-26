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
// SessionOpenMP -- the player's multiplayer PREFERENCES (as opposed to their name, which has its own
// module because it carries a word filter).
//
// Deliberately PURE: this module stores and persists, and touches nothing else. In particular it does
// NOT call the transport. Both menus can write a preference from whichever thread they run on (the F1
// overlay is the RENDER thread, the pause menu is the GAME thread), and the EOS SDK must only be
// driven from the thread that ticks its platform -- so a write bumps a generation counter and the
// game-thread pump applies the change. `MpPrefs_Generation()` is that counter.
//
// File: `SessionOpenMP_prefs.txt` next to the log, `key=value` per line, rewritten whole on change.
#pragma once

void MpPrefs_Init(const char* dir, void (*logf)(const char*));

// ---- "Hide my address" (default ON).
// EOS peer-to-peer will NAT-punch a direct connection when it can, and a direct connection means the
// people you are playing with learn your IP address -- which matters once the lobby browser puts you
// in sessions with strangers. Forcing Epic's relays keeps the address private at the cost of some
// latency.
// This is an EOS capability, not a universal one. It is named for the INTENT rather than for
// ForceRelays because other transports answer the question differently (or cannot answer it at all);
// those report what they actually do rather than pretending the switch worked.
// It applies to connections made AFTER it changes; links already established keep their route.
bool MpPrefs_HideAddress();
void MpPrefs_SetHideAddress(bool on);

// ---- THE RELEASE NOTES on the start menu. On, they stay on screen for as long as that menu is up
// and come down when you leave it. Off, they never appear. A preference rather than a once-per-build
// popup because every button a controller has at a menu already belongs to the game -- there is no key
// to open them with that the menus do not already use.
bool MpPrefs_ShowChangelog();
void MpPrefs_SetShowChangelog(bool on);

// ---- FLOATING PLAYER NAMES + SPEECH BUBBLES (ui/nameplates.h).
// Stored here because they are the player's own settings and have to survive a restart; the
// game-thread publish copies them into the live tuning every frame, so a change from either menu
// takes effect immediately and there is exactly one source of truth.
// Distances are in METRES, not centimetres, because the pause menu draws them on a slider and the
// game prints a slider's value with "%d" -- the units have to be ones whose integers mean something.
enum { MPNAME_OFF = 0, MPNAME_OFFBOARD = 1, MPNAME_ALWAYS = 2 };
// How far back a synced replay reaches, in seconds. Every snapshot of a peer's history carries a
// whole skeleton, so the length is what the transfer costs: 15 s is a trick, 80 s is mostly footage
// nobody scrubs back to. The REQUESTER's setting is the one that counts -- it is their wait.
// 0 = everything they have. The menu offers presets (15/30/60/120/All); the range here only
// has to admit those, and 0 has to survive the clamp or "All" could never be stored.
enum { MPSYNC_SEC_MIN = 0, MPSYNC_SEC_MAX = 600, MPSYNC_SEC_DEFAULT = 30 };
int  MpPrefs_SyncSeconds();
void MpPrefs_SetSyncSeconds(int seconds);

int  MpPrefs_NameMode();                 // MPNAME_* -- default MPNAME_OFFBOARD
void MpPrefs_SetNameMode(int mode);
int  MpPrefs_NameDistM();                // how far away a name is still drawn
void MpPrefs_SetNameDistM(int metres);
int  MpPrefs_BubbleDistM();              // ...and a chat bubble, which is deliberately much shorter
void MpPrefs_SetBubbleDistM(int metres);
// How big the text in a speech bubble is, at the distance a nameplate is drawn at its natural size.
// It still shrinks and grows with distance from there; this is what it grows and shrinks AROUND.
// How big a player's floating NAME is at the distance it is drawn at its natural size; it still
// shrinks with distance from there. 18 is what it always was.
enum { MPNAME_TEXT_MIN = 10, MPNAME_TEXT_MAX = 32, MPNAME_TEXT_DEFAULT = 18 };
int  MpPrefs_NameTextSize();
void MpPrefs_SetNameTextSize(int size);
int  MpPrefs_BubbleTextSize();
void MpPrefs_SetBubbleTextSize(int size);
// The slider limits, so the menu row and the setter's clamp cannot drift apart.
enum { MPNAME_DIST_MIN = 10, MPNAME_DIST_MAX = 250, MPBUBBLE_DIST_MIN = 5, MPBUBBLE_DIST_MAX = 100 };
enum { MPBUBBLE_TEXT_MIN = 8, MPBUBBLE_TEXT_MAX = 22, MPBUBBLE_TEXT_DEFAULT = 12 };

// THE REST OF THE BUBBLE'S LOOK. Panel and border are on, because that is what a bubble has been
// since 1.2.5. Blur DEFAULTS OFF and stays a choice: it was taken off bubbles for the cost of six of
// them on screen at once, and one blur per talking player is a real bill on a busy lobby.
// A bubble is a label in the world, not a slab to read a conversation off: light panel, no frame,
// no blur. Blur especially -- it is one per talking player, so it is opt-in.
enum { MPBUBBLE_BLUR_DEFAULT = 0, MPBUBBLE_PANEL_DEFAULT = 20 };   // both 0..100
int  MpPrefs_BubblePanelPct(); void MpPrefs_SetBubblePanelPct(int v);
int  MpPrefs_BubblePanel();   void MpPrefs_SetBubblePanel(int on);
int  MpPrefs_BubbleBorder();  void MpPrefs_SetBubbleBorder(int on);
int  MpPrefs_BubbleBlurPct(); void MpPrefs_SetBubbleBlurPct(int v);

// ---- WHERE THE CHAT BOX SITS, and whether it shows at all. Position is two percentages of the room
// the OPEN box leaves on screen: 0 = flush left / bottom, 100 = flush right / top, so the whole box is
// on screen at every value. The defaults put it in the bottom-left corner it has always used.
// Hidden hides the talk (F2 toggles it); ENTER still opens the box to type.
enum { MPCHAT_POSX_DEFAULT = 2, MPCHAT_POSY_DEFAULT = 14 };
int  MpPrefs_ChatPosX();       void MpPrefs_SetChatPosX(int pct);
int  MpPrefs_ChatPosY();       void MpPrefs_SetChatPosY(int pct);
int  MpPrefs_ChatHidden();     void MpPrefs_SetChatHidden(int on);

// ---- THE F1 MENU IN ITS OWN WINDOW, for a second monitor. OFF by default, and off the menu is
// exactly what it always was: drawn over the game, taking the mouse and keys while it is up.
int  MpPrefs_F1PopOut();       void MpPrefs_SetF1PopOut(int on);

// ---- THE CHAT BOX's look. Stored here for the same reason the nameplate settings are: they are the
// player's, they have to survive a restart, and the game-thread publish copies them into the live
// tuning every frame so there is exactly one source of truth and no apply step.
enum { MPCHAT_TEXT_MIN = 10, MPCHAT_TEXT_MAX = 26, MPCHAT_TEXT_DEFAULT = 15 };
// The header and the key hints. Their own setting rather than a fraction of the talk: somebody who
// wants big text does not necessarily want a big header, and the two were tied together.
enum { MPCHAT_SMALL_MIN = 8, MPCHAT_SMALL_MAX = 22, MPCHAT_SMALL_DEFAULT = 12 };
enum { MPCHAT_WIDTH_MIN = 360, MPCHAT_WIDTH_MAX = 1200, MPCHAT_WIDTH_DEFAULT = 620 };
enum { MPCHAT_LINES_MIN = 4, MPCHAT_LINES_MAX = 16, MPCHAT_LINES_DEFAULT = 12 };
enum { MPCHAT_HOLD_MIN = 3, MPCHAT_HOLD_MAX = 30, MPCHAT_HOLD_DEFAULT = 9 };
enum { MPCHAT_PANEL_DEFAULT = 48, MPCHAT_BLUR_DEFAULT = 70 };    // both 0..100
int  MpPrefs_ChatTextSize();   void MpPrefs_SetChatTextSize(int v);
int  MpPrefs_ChatSmallSize();  void MpPrefs_SetChatSmallSize(int v);
int  MpPrefs_ChatWidth();      void MpPrefs_SetChatWidth(int v);
int  MpPrefs_ChatLines();      void MpPrefs_SetChatLines(int v);
int  MpPrefs_ChatHoldSec();    void MpPrefs_SetChatHoldSec(int v);
int  MpPrefs_ChatPanelPct();   void MpPrefs_SetChatPanelPct(int v);
int  MpPrefs_ChatBlurPct();    void MpPrefs_SetChatBlurPct(int v);

// ---- DROPPED OBJECTS (the object dropper). What happens to the props everyone already had SAVED on
// the map when a session starts; live placements replicate above Off either way. See session.h.
// Stored here so the choice survives a restart, like the nameplate settings; the game-thread publish
// copies it into the session every frame, so there is one source of truth and either menu can write it.
enum { MPDROP_OFF = 0, MPDROP_LIVE = 1, MPDROP_SHARED = 2 };
int  MpPrefs_DropMode();                 // MPDROP_* -- default MPDROP_SHARED
void MpPrefs_SetDropMode(int mode);

// ---- PEER BODY PHYSICS. Whether other players' skaters get body physics on YOUR screen: the game's
// own physical animation switched on for their proxies (it never is otherwise -- a peer rides as pure
// animation), and, when they run SessionTweaks, their riding-body settings re-run on their proxy.
// Costs ~21 simulated bodies per visible peer. Default on. The loader pushes it into the proxy tuning
// every frame; the tweaks module asks through the bridge.
// LIGHT keeps the value 1 that "On" had, so an existing prefs file reads back unchanged. FULL is the
// game's own body physics with nothing trimmed away -- the A/B against the trim, and the fallback if
// the trim ever turns out to cost more than it saves.
// A fourth tier, SYNCED, transported the owner's own physics result so nobody had to simulate anyone.
// It was built and it FAILED in the field -- see the changelog. The bones and the mapping were proven
// correct and the values were coherent, but a subset of someone's bones will not compose onto a
// differently-posed skeleton: the arms hung off the receiver's shoulder and drooped. Removed rather
// than left switchable, because a broken option is worse than no option.
enum { MPBODY_OFF = 0, MPBODY_LIGHT = 1, MPBODY_FULL = 2, MPBODY_ON = MPBODY_LIGHT };
int  MpPrefs_PeerBodyPhysics();          // MPBODY_* -- default MPBODY_OFF
void MpPrefs_SetPeerBodyPhysics(int on);

// ---- PROXIMITY VOICE CHAT. Off, push-to-talk or open mic; the talk key; how far a voice carries;
// how loud other players are; how easily open mic opens. Applied every frame by the session (the
// microphone only runs in a session with other players). Mutes are a separate list (mutelist.h).
enum { MPVOICE_OFF = 0, MPVOICE_PTT = 1, MPVOICE_OPEN = 2 };
enum { MPVOICE_KEY_COUNT = 7 };                     // V, B, T, Left Alt, Left Ctrl, Mouse 4, Mouse 5
enum { MPVOICE_RANGE_MIN = 5, MPVOICE_RANGE_MAX = 100, MPVOICE_VOL_MIN = 0, MPVOICE_VOL_MAX = 200 };
int  MpPrefs_VoiceMode();            void MpPrefs_SetVoiceMode(int mode);        // default push-to-talk
int  MpPrefs_VoiceKey();             void MpPrefs_SetVoiceKey(int idx);          // index into the key list, default V
int  MpPrefs_VoiceRangeM();          void MpPrefs_SetVoiceRangeM(int metres);    // default 25
int  MpPrefs_VoiceVolume();          void MpPrefs_SetVoiceVolume(int pct);       // default 100
int  MpPrefs_VoiceSensitivity();     void MpPrefs_SetVoiceSensitivity(int pct);  // default 50
const char* MpPrefs_VoiceDevice();   void MpPrefs_SetVoiceDevice(const char* id); // a Windows endpoint id; "" = default

// ---- THE LEVEL'S OWN PROPS (the benches and barriers the map ships with, which the dropper can also
// shove around). A SEPARATE setting from the one above, and OFF by default, deliberately: sharing
// them is the unfinished half of this feature and it must not be able to destabilise the inventory
// sharing, which works. Off = the level's furniture is left entirely alone.
// The old separate world-props mode (MPWORLD_*) is retired: the level's own props are part of
// "Share one set" now -- the host's arrangement IS the session layout. The WorldMode ini key is
// ignored on read and no longer written.

// Bumped by the setters whose value the TRANSPORT has to be told about. The game thread applies
// those when this changes, so a menu can write from any thread without ever calling into the SDK
// itself. The nameplate settings above deliberately do NOT bump it: nothing about them reaches EOS,
// and a spurious bump would re-issue a relay-control call for a change that has nothing to do with it.
unsigned MpPrefs_Generation();

// ---- This install's PERMANENT peer identity: 32 lowercase hex characters (128 random bits),
// generated once and kept in the preferences file.
// EOS hands out a ProductUserId, so the EOS backend never needed one. Every other transport does: a
// peer has to be recognisable across a reconnect, and it must NOT be recognisable by its address --
// a NAT rebind changes the address mid-session, and keying identity to an endpoint is how a peer
// silently becomes a stranger (the same rule that makes shm key on its SLOT and not on a PID).
// Random rather than derived from anything about the machine: it should identify an install to the
// people it plays with, and say nothing about the person to anyone else.
// The build whose What's New notes have already been shown. Empty on a fresh install -- which is why a
// fresh install sees them once too: somebody arriving new still wants to know what is in here.
const char* MpPrefs_SeenVersion();   void MpPrefs_SetSeenVersion(const char* v);
const char* MpPrefs_PeerId();
