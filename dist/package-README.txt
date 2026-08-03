SessionOpenMP -- co-op test build
=================================

Online co-op for Session: Skate Sim. Test build -- expect rough edges.
Works on the Steam and Epic versions.

This package contains two mods:

  SessionOpenMP  -- the multiplayer itself.
  SessionTweaks  -- gameplay fixes for your own skating (see below). It is
                    part of this package but is a separate mod, and it can be
                    turned off on its own without affecting multiplayer.

INSTALL
-------
1. Find your game's Win64 folder (the one with SessionGame-Win64-Shipping.exe):
   Steam:  <Steam>\steamapps\common\Session\SessionGame\Binaries\Win64
   Epic:   <Epic>\SessionSkateSim\SessionGame\Binaries\Win64
2. Copy EVERYTHING from this zip into that folder.
   Say YES when asked to replace EOSSDK-Win64-Shipping.dll -- the mod needs a
   newer version of that file than the game ships. (If Steam ever "verifies
   file integrity" it will restore the old one; just copy it in again.)
3. Launch the game normally.

UPDATING
--------
Double-click update.bat in this folder. It checks GitHub for a newer release,
downloads it, and installs it over this one.

Your settings are kept: SessionOpenMP_prefs.txt, SessionOpenMP_bans.txt,
SessionTweaks.ini and UE4SS-settings.ini are never overwritten, and if you
disabled a mod in Mods\mods.txt it stays disabled.

It is a plain text script, not a program -- open update.ps1 in Notepad and
read it first if you like. It only contacts github.com, and it only writes
inside this folder. Close the game before running it.

   update.bat                 install the latest release
   update.ps1 -Check          just tell me if there is one, change nothing

PLAY
----
1. Both players get in-game (skating around in a level, not in a menu).
2. One player presses F1 and clicks "Host online".
3. The other presses F1 and clicks "Join online" (give the host a few
   seconds head start).
4. You should see each other skate. F6 leaves the session.

There is also a "Multiplayer" entry in the game's own pause menu, with a
list of open sessions you can pick from.

The first host/join can take a few seconds -- it logs into Epic Online
Services anonymously in the background. No account or sign-in needed.

SESSIONTWEAKS (the second mod in this package)
----------------------------------------------
Singleplayer gameplay fixes, bundled here because they are developed together.
They change how YOUR skating feels -- they are not part of multiplayer and are
not synced to anyone:

  * scoop speed follows how fast you actually sweep the stick
  * a wider window for manual catches
  * a darkslide-aware catch fix
  * you run out of low missed tricks instead of always bailing
  * the board catch sound plays reliably

Settings are under F1 and in the game's pause menu.

If your skating feels different from vanilla and you did not expect that,
this is why. To turn it off, open Mods\mods.txt in the Win64 folder and
change "SessionTweaks : 1" to "SessionTweaks : 0", then restart the game.
Multiplayer is unaffected either way.

PRIVACY
-------
"Hide my address" is ON by default. Your traffic goes through Epic's relays,
so the people you play with never see your IP address. Turning it off (F1, or
the pause menu) allows a direct connection instead: a little faster, but the
players you connect to can see your address. It applies to new connections,
not to a session already running.

ADVANCED: PLAYING WITHOUT EPIC
------------------------------
Under F1 -> "Direct connect (no Epic account)" one player can open a port and
the other connects straight to their address. Useful on a LAN, or if you would
rather not use Epic Online Services at all. It needs the host to be reachable
(same network, or that port forwarded), it is not encrypted, and the people
you play with will see your IP address. The Epic route above is the easier and
the safer one -- this is here for people who want or need the alternative.

IF SOMETHING BREAKS
-------------------
Send these files from the Win64 folder:
   SessionOpenMP.log
   SessionOpenMP_eos.log
   SessionTweaks.log      (only if the problem is with your own skating)

UNINSTALL
---------
Delete dwmapi.dll from the Win64 folder -- that alone disables everything.
To fully restore, also delete UE4SS.dll, UE4SS-settings.ini, the Mods
folder, and verify game files (restores the original EOSSDK dll).

LICENSE
-------
SessionOpenMP and SessionTweaks are free software under the GNU General
Public License v3 (LICENSE), with an additional permission under GPL
section 7 that allows linking with the Epic Online Services SDK and the
game itself (LICENSE-EXCEPTION.txt). You may use, study, modify and share
them.

Source code: the complete Corresponding Source for the SessionOpenMP and
SessionTweaks parts of this package is at

   https://github.com/matsixx/SessionOpenMP

at the tag matching this release. If you cannot obtain it there, it is
available from the author on request, free of charge, for at least three
years -- ask whoever gave you this copy, or contact matsix.

SessionOpenMP is an unofficial fan project, not affiliated with or endorsed
by Creature Studios or Epic Games.

It is built on other people's work: Dear ImGui (Omar Cornut), MinHook
(Tsuda Kageyu), RE-UE4SS (Narknon), and the Epic Online Services SDK.
Full license texts are in THIRD-PARTY-NOTICES.txt, and Epic's own SDK
notices are in EOS-ThirdPartySoftwareNotice.txt. Keep LICENSE,
LICENSE-EXCEPTION.txt and both notice files with any copy you pass along.
