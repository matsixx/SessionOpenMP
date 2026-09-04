# Features

Everything the two mods in this package do. **SessionOpenMP** is the multiplayer half; **SessionTweaks**
is the singleplayer gameplay half, and it works with or without the other. They ship together and are
listed together here, but they are separate DLLs and either can be switched off on its own line in
`mods.txt`.

\[feature] = a new capability. \[fix] = a defect in the base game, measured and corrected.
\[restored] = something the game used to do and an update removed. Versions are where each landed.

---

# SessionOpenMP -- multiplayer

Session has no multiplayer and this does not add a server to it. Every player runs an ordinary solo
game and the mod overlays the others on top: each game is authoritative for exactly one skater, and
every remote player is a locally spawned actor **of the game's real skater class**, driven entirely
from data on the wire. Nothing about a remote player is guessed locally. Because they are real
game-class actors rather than puppets, they collide with you and make the game's own sounds.
Both machines run identical code -- there is no host/client split anywhere in the mod.

## Connecting

* \[feature] **Online sessions over Epic Online Services**, including **Epic-Steam cross-play**. Host or
join from the pause menu; the browser lists each session's host, map, player count and mod version.
* \[feature] **Private games by code** -- a session mints a code and is skipped by the public browser, so
the code is the only way in.
* \[feature] **Direct connect** without Epic, over the internet or a LAN. Everyone is introduced to
everyone, so all players see each other rather than only the host (1.0.0-rc4; before that it worked
properly for two).
* \[feature] **A relay server** for more than two players over the internet without port forwarding
(1.0.0-rc4). Everyone connects out to one address; the relay forwards and holds no game state.
It ships as source and a prebuilt binary in the package, and runs on Windows or Linux.
* \[feature] **Shared memory** between two games on one PC, for development and testing.
* \[feature] **Hide my IP address** -- on by default. Traffic goes through Epic's relays so peers never
see your address; off is a direct connection, which is faster.
* \[feature] **Ban list**, per player, kept between sessions.
* \[fix] Ghost lobbies are hidden: a crashed or killed game leaves its session alive on Epic's backend,
and those are filtered out of the browser rather than offered as joinable (0.9.5b). Your own crash
leftovers never appear in your list, and clicking a session joins the one you clicked (0.9.5b).
* \[fix] The browser follows its own search: signing in, searching, then the results, with nothing
pressed (1.0.0).

## Skating together

* \[feature] **Every other player's skater and board**, transported and interpolated on a shared playback
clock -- position, orientation, the full animation state, board state and stance.
* \[feature] **Ragdolls are synced** (1.0.0-rc5). A bail used to be re-simulated on every machine from its
own starting conditions, so nobody watched the same fall; the owner's actual skeleton is now sent
while they are down and everyone sees the same flop.
* \[feature] **Board breaking syncs** -- you see another player snap their board, and see it rebuilt
(0.8.1b).
* \[feature] **The analog crouch is visible to other players** when SessionTweaks' pop-control scheme is
on: peers see the depth you are crouched to, not a stock pop (1.0.0-rc2).
* \[feature] **Real collision.** Peers are game-class actors, so you can bump into them and their boards.
* \[fix] Players on a poor connection no longer appear to stretch when their data stops arriving
(0.7.2b), and a peer's legs follow their board correctly (1.0.0-rc2).
* \[fix] Players in a different level are not spawned into yours; they disappear when they go to another
map and reappear when they come back (0.7.1b, 0.8.3b). Going to the apartment mid-session no longer
breaks the game (0.7.1b).

## Cosmetics

* \[feature] **Clothing and board parts sync** -- other players wear what they are actually wearing, and
ride the deck, trucks and wheels they picked.
* \[feature] **Pro and female skater models sync** (0.8.1b): peers render with the body they chose, not
with yours.
* \[fix] Clothing no longer depends on your own outfit -- playing as a pro skater used to change what
other people looked like to you (0.8.1b).
* \[fix] Items with long names sync (0.9.9b), and a peer whose body differs from yours keeps their own
clothes (1.0.1).

## Sound

* \[feature] **Other players make the game's own sounds** -- rolling, grinds, powerslides, reverts, impacts,
pushes, board hits and the trick pop -- captured at the game's sound-spawn layer on their machine and
re-issued on their skater on yours, with the real surface type and parameters.
* \[fix] Rolling no longer cuts in and out between skaters. The game ships a voice limit of one on that
sound, so with two people rolling only one was audible; the limit is widened before the first sound
creates it (1.0.1).
* \[fix] A peer's board no longer makes its own sound on your machine on top of the transported one, and
a looping cue the game spawns as fire-and-forget can no longer play forever on a proxy (1.0.1).

## Talking

* \[feature] **Player nameplates** above each skater, with a distance setting (0.7.0b).
* \[feature] **Chat**, appearing as a speech bubble over the speaker (0.7.0b), with a **typing indicator**
while someone is composing (0.9.0b) and notices when players join or leave a session (0.8.3b).
* \[feature] **Multiplayer name** of your own, separate from your profile name, with a filter.

## The replay editor

* \[feature] **Your replay editor is your own instance** (0.8.0b). Other players are concealed while you
scrub rather than recorded into your timeline, so your replay is yours.
* \[feature] **Sync Replay** (0.8.0b): pick a player on the Look At page and turn it on, and their skater
is driven from *their* recorded history at your scrub position -- their tricks, in your replay, with
their sound (1.0.0-rc1).
* \[feature] **Synced replay length** -- 15 s / 30 s / 60 s / 120 s / All (0.9.6b). Shorter is a much faster
fetch; the setting decides your own wait, not theirs.
* \[feature] **A progress readout** over the player whose replay is being fetched (0.9.6b), so a long
transfer is visibly working rather than hung.
* \[feature] Watching someone scrub is smooth at full rate (1.0.0-rc1), and a scrubbing player animates
on your screen rather than freezing.
* \[fix] **Entering the replay editor with mixed outfits no longer crashes** (1.0.1). Each skater's replay
component caches a bone index per bone, resolved by name against the mesh it had at the time, and the
game then writes a transform into the pose at each cached index with no bounds check, on a worker
thread. A remote player's skater is dressed outside the game's own wardrobe rebuild, so it kept the
indices of the skeleton it was spawned in; dressed into anything smaller, the replay wrote past the
pose and the heap damage surfaced later, anywhere. The cache is now re-synced after every dress.
* \[fix] The replay camera and its float tracks no longer read past the end of their keyframe arrays --
a base-game fault that uneven frame pacing provokes (1.0.0-rc1).
* \[fix] Peers in modded clothing no longer deform each other in the editor (0.9.9b); a peer's skater no
longer stands frozen in your replay (0.9.6b); the Look At list shows everyone in the session
(1.0.0-rc5).

## World objects

* \[feature] **The object dropper syncs** (0.9.0b). Rails, ramps and ledges placed with the dropper are
shared, with three modes: **Share one set** (everybody sees the same objects -- the host's), **Live edits
only**, and **off**.
* \[feature] **The level's own props** are part of the shared set, so a bench somebody moved is where they
moved it for everyone (0.9.0b, reworked 0.9.5b).
* \[feature] **Host migration follows the same rule**: when the host leaves, their objects go and the new
host's set takes over (0.9.5b).
* \[fix] Your saved profile can never catch the session's furniture -- the game saves world props' live
positions into your profile, and that path is guarded (0.9.5b).

## Menus, settings and updates

* \[feature] **In-game pause-menu integration** -- a Multiplayer page in the game's own menu, in the game's
own style, with the sessions browser, players, connection and options as real menu pages.
* \[feature] **A tabbed F1 panel** -- You, Session, Session Tweaks, Dev (1.0.0-rc4).
* \[feature] The **B button steps back** through the mod's pages instead of closing the pause menu
(1.0.0-rc2).
* \[feature] **An update notice in the game's own popup** at the main menu when your copy is out of date
(1.0.0-rc4), and **`update.bat`**, a readable PowerShell updater that installs the latest release over
your copy and keeps your settings (0.7.0b).
* \[feature] **Version mismatches are visible** in the browser before you join (0.7.0b).
* \[fix] The multiplayer page follows the session live -- peers joining and leaving, connection state --
which it had never actually done (1.0.0).

## Performance and networking

* \[feature] A quantised snapshot wire -- smallest-three quaternions, 16-bit floats, zero-suppression and
presence masks -- at a fixed rate, interpolated on a shared clock, with a reliable lane for the things
that must not be lost.
* \[feature] **About a third less bandwidth**, and smoother movement with more players (1.0.0-rc2).
* \[feature] **A whole skeleton fits in one packet for every character** (1.0.0-rc5). Modded garments merge
as a union of bones, and a 95-bone character turned out to be a 70-bone one plus 25 childless
terminator bones that describe no motion; those and the merged board rig (the deck already travels as
its own transform) are no longer sent, so every character updates at the full rate.
* \[feature] Skaters out of view throttle their animation, and boards beyond 25 m stop physically
simulating (0.8.0b).
* \[fix] A send the network refuses is counted and named rather than silently lost (1.0.0-rc4).

---

# SessionTweaks -- singleplayer gameplay

Every base-game fix and feature, from the start of the mod. Compiled from CHANGELOG.md
(2.0.0 through 3.19.274), the module headers and the pause-menu pages. Works with or without
SessionOpenMP; standalone it configures itself from `SessionTweaks.ini`.

## Board \& tricks

* \[fix] Scoop speed follows the stick sweep (the original SessionScoopFix, pre-2.0). Stock maps scoop speed
from the flick's DURATION over a 0.10-0.25 s window, so nearly every real scoop (0.30-0.55 s) clamps to
the slowest floor; the lookup curve is a staircase, flat across its middle third; pocket/corner starts
give no speed control at all. Side effect: the "board finishes the flip then freezes while the shove
completes" artifact largely disappeared.
* \[feature] Flip speed from how fast you actually flick (2.24), with Slowest/Fastest sliders. Later fixes:
a frame-rate-dependent ceiling in the measurement (2.27), speed pinned by a stick that never returns
toward centre (2.28.6), identical flicks randomly reading as the slowest flip (3.19.41), measurement from
the real thumb inside the pop scheme (3.19.37).
* \[fix] Wider board pitch control (2.16/2.17). Stock, board pitch reaches its maximum at \~35 deg of flick
elevation and everything above is identical; the response is spread over the whole flick (Pitch spread).

## Catch \& bail

* \[feature] Wider manual catch (2.0 era: manual catch window x2) -> the REAL window (3.19.239): the slider
drives the game's own bad-catch threshold (ships 120), so an early or late press bails mid-air through the
game's verdict instead of engaging a hopeless catch (3.19.238).
* \[fix] Dark slide catch input (2.0-2.7): the dark-slide reservation ate press-time catches; it is now honoured
only near grip-down (Dark slide zone), elsewhere the press catches at press time.
* \[feature] Catch grace as flick-versus-hold: a catch is a flick, a darkslide is a hold (2.2/2.3).
* \[feature] Run out on missed tricks (2.1-2.8, 2.12.1): a low missed-trick bail becomes the game's own on-foot
consequence with riding momentum kept, height-gated by measured geometry (the engine's air fields read
zero at bail time), fall watch for a real bail past the drop threshold, facing/momentum from position
history (Velocity is frozen while skating), no ini hand-editing.
* \[restored] Auto leveling on catch (2.9-2.11, 2.28.2/2.28.4, 3.19.248): the game removed board levelling in
code (no native reader of the authored catch-align fields); restored keyed to the catch, with ease speed,
wait and level-angle knobs.
* \[fix] Catch sound (2.10-2.11, rebuilt 2.68): the game's catch sound is an anim notify most flip catches do
not carry, and where they do it fires \~300 ms late (double sounds). One source, played at the catch,
recorded into the replay, volume slider; ollies/nollies stay silent by design.
* \[fix] Catch ends the flip (2.28.0): a caught board stops at griptape-up instead of a second revolution.
* \[fix] Foot always attaches (pre-3.18, aim corrected 3.19.251): the game extends the flip target to 720 the
moment the deck runs past 360 uncaught, which drops the foot's attach ratio to 0 mid-catch and the foot
never plants; the target is aimed at the nearest flat (on the counter, not the rendered angle).
* \[fix] Catch with the foot you flicked (2.47-2.58): the game chose the catching foot itself; now the flicked
foot, with the game's own stance rule in all four stances.
* \[fix] Fix stuck catch pose (2.60): feet wedged floating in the catch pose after landing, ride after ride
(the only reseat path was a bail's remount); the game's own cleaners are run.
* \[fix] Second stick can't break the catch -> one catch per air (2.61/2.62): a second flick restarted the
whole catch and left the feet floating.
* \[fix] A held flick no longer becomes a catch at the pop (2.64, CatchNeedsFreshFlick).
* \[fix] Mid-scoop catches no longer pitch the board nose-down (2.65); held sticks no longer pitch the board
after a catch (2.66/2.67).
* \[fix] Primo slides keep their feet (3.19.208); pushing both sticks inward orients the board again (3.19.209).
* \[fix] Mid-scoop catch put the foot on the nose (3.19.209-219): the two-stick orient's foot type is
re-decided to the plain foot the game would have chosen.
* \[feature] Click a stick to catch, Skater XL style (3.19.192-199), pause-menu row, configurable keys.
* \[fix] An ordinary ollie occasionally tilting the wrong way once (2.53.2); tricks popping with the board held
flat after a late trick (3.14.0); a very slow flip caught before it had begun (3.18.0-3.18.2, auto catch).
* \[fix] The catching foot hovering above the deck for the whole air (3.19.223/224): the foot descends with
the board; the game's attach window is opened to the rotation still owed.
* \[fix] Fakie backside pop shove could never be caught (3.19.227): the min-spin gate only counted the flip axis.
* \[fix] Over-rotated flip catches (3.19.249-252): the board rolls back to flat with the foot coming down on it
and plants every time, driven on the game's flip counter (the rendered angle trails it by \~100 deg at flip
speed). Over-rotation bail slider on the rendered deck, past side only (3.19.250/256).
* \[fix] 180-shove flip tricks never planting (3.19.253): the plant fix runs on the shove axis too.
* \[fix] Under-rotated shove catches (3.19.254/263/264): the shove is finished by the snap when 30 deg or less
short (ceiling 720 deg/s), otherwise caught where it is; over-rotated shoves stop where caught (3.19.259).
* \[feature] Shove sideways bail (3.19.272): a shove caught within a band of sideways -- where the game cannot
decide which end each foot belongs to -- bails.
* \[fix] Feet oscillating on a board parked at an odd yaw (3.19.265/267): a foot socket that jumps 20+ cm in a
frame keeps last frame's placement; the end nearer the foot's riding spot wins.

## Grinds

* \[fix] Pitch control out of grinds (defaults on 2.28.3): Board Control pitch is ignored coming off a grind.
* \[fix] Pop swing out of grinds (defaults on 2.28.3): the grind pop path skips the tail-drop crank swing that
flat-ground pops have.

## Feet

* \[fix] The floating shoe (2.17-2.20, at the source 2.40): since a Session update the shoe hovers above the
griptape. Cause measured: the foot auto-adjust sweep hits the ROAD, not the deck. The auto-adjust is
suppressed while riding; per-shoe nudge profiles (2.21-2.23) were retired by the source fix. Sole lift
per stance for the pocket (3.19.225/226).
* \[feature] Mid-trick foot control (2.29-2.32): the sticks move and twist your feet in the air; catching with
one foot leaves the other foot yours (2.31); the foot no longer jitters (2.30.1).
* \[feature] Boned ollie (%) (2.33/2.35): the game's built-in bone scaled, removed, or extended, with per-axis
adds.

## Pop control

* \[feature] Analog pop/crouch scheme (3.19.20-3.19.77): crouch to any depth and pop from it, deeper is higher;
the crouch visual follows the stick (3.19.52-58); every stance and pop family from one rule (3.19.60);
goofy (3.19.202); trick window, crouch gate and smoothing knobs; fixes for stray manuals, tiny-pops,
quick shoves, boardslides and 180s into grinds along the way.

## Camera

* \[fix] Camera always follows height: the camera's height only tracked the skater when landing HIGHER than
the launch point (data thresholds); ordinary airs and drops left it behind. Pitch camera before a drop.

## Clothing

* \[feature] Cloth physics on tops and bottoms (2.69-2.85, 2.96-3.13, 3.19.0-3.19.19): garments un-merged from
the character mesh and simulated (NvCloth on a runtime-built asset), materials read from the customization
system, lighting/seams/joint-spike fixes, replay-editor support, a per-garment list with ticks, custom
clothing on other skeletons (3.19.0), settings persistence and reset that keeps your tags.

## Physical animation

* \[feature] Reactive body (3.19.79-3.19.201): the ragdoll braces, holds what hurt (hardest hit, head/neck to
the top of the head), flails in the air, rolls over from face-down, tucks, falls heavier, arm reflexes while
riding; every steering force an internal couple; joints driven in torque space with muscle tone.
* \[fix] The floating ragdoll (3.19.108): bodies resting on an invisible surface -- solved at the cause.
* \[fix] The reactive body switching itself off on a map change (3.19.207).
* \[feature] Ragdoll self-collision (3.19.228-231): continuous collision on every body, upper arms collide with
the torso, real shoulder travel; backward-fall brace splays the arms with palms to the ground (3.19.232).

## Settings and menus

* \[feature] F1 panel and pause-menu pages (2.25/2.26, categories 2.26 and 3.19.203, nested pages 3.19.262),
settings persisted from the pause menu (2.28.5) and the F1 panel (2.8), Reset to defaults.
