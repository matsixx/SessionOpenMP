# Adding rows to Session's own menus

How the mod puts its own rows into the game's native menus, what the row types are, and the rules
that keep it from crashing. Read the **Rules** section before writing code — each one is there
because breaking it crashes the game.

Implementation lives in [`src/ui/pause_menu.cpp`](../src/ui/pause_menu.cpp); the symbols and offsets
are in [`src/game/game_syms.h`](../src/game/game_syms.h) (search `kPage`, `kItem`, `Menu`).

---

## 1. How it works, in one paragraph

Session's menus are **data**, not blueprint logic. A page is a `UMenuPageDefinition` asset holding a
`TArray<FMenuPageItemDefinition>` (144 bytes per row), and `UMenuPage::CreatePageItems(page, items,
flag)` turns that array into one `UMenuPageItem` widget per row. We **pre-hook CreatePageItems and
hand it a different array** — a copy of the game's rows plus ours. The page's own
`_pageItemDefinitions` is never written, so nothing of ours is ever owned, destructed or freed by the
engine. Navigation counts *widgets*, and the confirm/change events carry each widget's **own embedded
copy** of the definition, so an injected row behaves exactly like a stock one all the way to the
handler and its `_key` arrives intact.

Nothing is blueprint-dependent and no assets are needed.

---

## 2. Recon first: find your page

Turn on page dumps (`omp::debug::Get().menuPages`, see `src/debug.h`). Open the menu you care about once and
read `SessionOpenMP.log`:

```
[menu] page 'ReplayEditors' items=6 maxVisible=14 headerIndex=0
[menu]   [0] 'SaveReplay' type=1
[menu]   [1] 'ReplayLoadReplay' type=1
[menu]   [2] 'ReplayAutoShowUI' type=2
[menu]   [3] 'ReplayNotifications' type=2
[menu]   [4] 'Camera' type=1
[menu]   [5] 'DayNight' type=1
```

That gives you the **page key** (`ReplayEditors`), the existing item keys, how many rows there are,
and `maxVisible` — the cap on how many rows get built (see Rule 7). Dumps are capped at 12 distinct
pages per run and de-duplicated by key.

Known keys so far: `PauseMenuPage`, `OptionsPage`, `ReplayEditors`, `ReplayModeButtons`,
`GameplaySettingsPage`, `AdvancedSettingsPage`, `DisplaySettingsPage`, `AudioSettingsPage`,
`CameraSettingsPage`, `FlipTricksSettingsPage`, `BoardSettingsPage`, `GrindsSettingsPage`,
`RevertsSettingsPage`, `MapSettingsPage`.

---

## 3. Row types

`_itemType` at `+0x0d` picks the row widget (`UMenuPage::CreatePageItem`'s jump table):

| Value | Type | Looks like | Fires |
|---|---|---|---|
| 1 | Selection | `Resume Game` | `OnSelectionConfirmed` |
| 2 | MultiOption | `Dark Slides      < On >` | `OnMultiOptionItemSelectionChanged` |
| 3 | ProgressBar | `Master Volume    [====  ] 70` | `OnProgressBarValueChanged` |
| 4 | SliderBar | (unused by us) | — |

**MultiOption is the "toggle through several options" row** — it is what you want for a cycling
setting, and it is what `ReplayAutoShowUI` already is. Everything it needs lives on the definition:

- `_multiOptionTexts` (`+0x68`, a `TArray<FText>`) — point it at **your own** static array of
  `FText`s and set Num/Max. The engine copy-constructs from it; it never owns your storage.
- `_multiOptionStartingIndex` (`+0x78`) — the option shown when the row is built.

A **one-option** MultiOption row is also the only way to draw a right-aligned *value* next to a
label (that trick is what makes the lobby browser look like a server browser).

ProgressBar carries only `_progressDisplayValueMinimum/Maximum/_progressIncrement`
(`+0x80/84/88`) — **no current value**; that has to be stamped on the widget (Rule 5).

---

## 4. Recipe: a cycling option on the replay-editor page

The existing code injects into `PauseMenuPage` only, and wraps it in a page state machine
(`g_page`) for the Multiplayer sub-page. **You do not need any of that** to add one row to another
page. Minimal path:

**a. Build the row once** (next to `buildRows()`), using the existing helpers:

```cpp
static uint8_t   g_replayRow[0x90];
static FTextBlob g_replayOpts[3];          // one FText per option
static uint64_t  g_replayKey = 0;

// buildRow fills key/label/both descriptions; then make it a MultiOption.
buildRow(g_replayRow, "OmpReplayThing", "My Option", "What it does", &g_replayKey);
makeText("Off", &g_replayOpts[0]);
makeText("Mode A", &g_replayOpts[1]);
makeText("Mode B", &g_replayOpts[2]);
*(g_replayRow + off::kItemType) = 2;                                   // MultiOption
*(void**)  (g_replayRow + off::kItemMultiTexts)        = g_replayOpts;
*(int32_t*)(g_replayRow + off::kItemMultiTexts + 0x08) = 3;            // Num
*(int32_t*)(g_replayRow + off::kItemMultiTexts + 0x0c) = 3;            // Max
*(int32_t*)(g_replayRow + off::kItemMultiStart)        = g_myValue;    // current
```

**b. Append it in `chooseArray`.** Today that function early-outs with
`if (key != g_pauseKey) return items;`. Add your page beside it — intern the key once with
`makeFName("ReplayEditors", &g_replayPageKey)` and, when it matches, copy the stock rows into
`g_rowBuf`, append `g_replayRow`, and return that array. Use `stampTemplate()` on your row so it
inherits the page's own platform flags and input delay. Respect `maxItems`/`kRowCap`.

**c. Handle the change.** `hkMultiChanged` already fires for every page. Its helper
`handleValueChange` currently resolves a *guest* page from `g_page`; for a foreign page, match your
key directly instead:

```cpp
const uint64_t itemKey = *(const uint64_t*)((const uint8_t*)params + off::kSelParamsItem + off::kItemKey);
if (itemKey == g_replayKey) {
    const int newIdx = *(const int32_t*)((const uint8_t*)params + off::kChangeParamsNew);
    // apply it
}
```

Always call the trampoline afterwards — the engine still owns the row's visual state.

**d. If the value does not stick** across closing and reopening the page, stamp it on the widget
after the build the way `stampValues()` does (Rule 5).

### Easier alternative
If the option belongs to a **separate mod DLL**, skip all of the above and use the C ABI seam —
`OmpMenu_RegisterPage2` in [`src/ui/menu_ext.h`](../src/ui/menu_ext.h) gives you toggles and sliders
with get/set callbacks and no engine knowledge at all. It only creates pages under *our* Multiplayer
entry, though — it cannot add a row to an arbitrary game page.

---

## 5. Rules — each one is a crash that already happened

1. **A zero-initialised `FText` is not an empty `FText`.** It is a null pointer with a crash
   attached. `HandlePageItemSelectionChanged` feeds `_longDescription` into
   `FTextInspector::GetTableIdAndKey`, which dereferences `TextData` — merely *navigating onto* a row
   with a memset description is an access violation. Fill **label, short AND long** descriptions
   always; `buildRow` does this for you.
2. **An `FText` argument passed by value is CONSUMED.** `UMenuPage::SetTitle` copies-with-a-bump into
   the text block and then destroys *your* argument. Pass a copy whose refcount you bumped
   (`passOwned()`), or you decrement someone else's reference — doing that to the page asset's own
   `_displayName` kills the shared text data and Slate faults later, far from your code.
   **`FText` is a refcounted handle, not a POD; copying its bytes does not make a reference.**
3. **Never rebuild the rows from inside the confirm callback.** `UMenuPageContainer::OnConfirmAction`
   broadcasts to you from the *middle* of itself and keeps using the widget pointers it captured
   before the broadcast. Queue the rebuild and do it from the next engine tick (`PauseMenu_Pump`).
4. **After swapping rows, `_selectedIndex` is stale and out of bounds.** Write `-1` first (that makes
   `SetSelectedIndex`'s deselect branch skip) then select 0 through the game's own setter.
5. **Stamp row values AFTER the whole rebuild, not inside the CreatePageItems hook.**
   `RefreshItemsPanel` is `SerializePage → clear → CreatePageItems → DeserializePage`, and
   `DeserializePage` restores values by calling `ProgressBarSetPercent` /
   `MultiOptionSetSelectedItemIndex` — i.e. it lands *after* you.
6. **Nothing that happens during a rebuild is user input.** Both of those display setters **broadcast**
   the change exactly as a controller press would, so an unguarded change handler sees "the user just
   set this to the minimum" on every page build — and saves it. Guard the whole rebuild
   (`g_rebuilding`), not just your own writes.
7. **`_maxVisibleItems` (`+0x290`) caps row creation** to `[headerIndex, headerIndex + maxVisible)`.
   An appended row past the cap is silently never built, which looks exactly like "the injection
   didn't work". Raise it for the duration of the call and restore after.
8. **A ProgressBar's readout is an integer** — `display = (int)(2*min + 2*pct*(max-min) + 0.5) >> 1`,
   printed `%d`. Choose units whose integers mean something (cm, deg, percent), never "0.5 to 6.0
   metres".
9. **Copy the non-text fields from a stock row** (`_targetPlatforms`, `_isEditorOnly`,
   `_selectionInputDelay`) rather than inventing them — `stampTemplate()` does this from the page's
   own first row.
10. **Everything is SEH-guarded and self-disabling.** Any fault calls `die()`, which switches the
    whole feature off for the run and logs once. Keep that property: the menu is cosmetic, the game
    is not.

---

## 6. Symbols involved

All resolved by byte signature, 1-hit in **both** the Epic and Steam exes, verified offline by
`omp_symcheck` on every build. Epic RVAs for orientation:

| Sig name | Function | Epic RVA | Use |
|---|---|---|---|
| `MenuCreateItems` | `UMenuPage::CreatePageItems` | `0x1071150` | **hooked** — the injection point |
| `MenuSelConfirmed` | `UMenuPage::OnSelectionConfirmed` | `0x1090c80` | **hooked** — confirm funnel (all pages) |
| `MenuMultiChanged` | `::OnMultiOptionItemSelectionChanged` | `0x10822e0` | **hooked** — MultiOption changes |
| `MenuProgressChanged` | `::OnProgressBarValueChanged` | `0x1090c50` | **hooked** — slider changes |
| `MenuRefreshItems` | `UMenuPage::RefreshItemsPanel` | `0x1096b70` | rebuild the rows |
| `MenuSetSelIndex` | `UMenuPage::SetSelectedIndex` | `0x1099eb0` | fix the selection after a swap |
| `MenuSetTitle` | `UMenuPage::SetTitle` | `0x109ab60` | page heading (see Rule 2) |
| `MenuProgressSetPct` | `UMenuPageItem::ProgressBarSetPercent` | `0x1096330` | slider value (**normalised 0..1**) |
| `MenuMultiSetIndex` | `::MultiOptionSetSelectedItemIndex` | `0x1078e80` | toggle value (**broadcasts**) |
| `MenuTextSite` | *a call site*, not a function | `0x108949b` | see below |

⚠️ **`FText::FromName` cannot be signatured** — `FPackageName::GetShortName` is byte-for-byte
identical, 2 hits at any length. We sig a unique **call site** and decode its trailing `E8 rel32`.
Use `makeText()`; do not try to sig it directly. And prefer `FromName` over `FromString` /
`AsCultureInvariant`: those come in const-ref and rvalue-ref twins no signature can tell apart, and
picking wrong means the engine steals or double-frees your buffer. An `FName` argument is a POD 8
bytes — no ownership question to get wrong.

Key offsets (`off::` in `game_syms.h`, all PDB-exact):
`kItemSize 0x90` · `kItemKey 0x00` · `kItemType 0x0d` · `kItemLabel 0x10` · `kItemShortDesc 0x28` ·
`kItemLongDesc 0x40` · `kItemSubPage 0x58` · `kItemMultiTexts 0x68` · `kItemMultiStart 0x78` ·
`kItemProgMin/Max/Increment 0x80/84/88` · `kPageActiveDef 0x298` · `kPageItemWidgets 0x2a0` ·
`kPageItemDefs 0x2e0` · `kPageDefKey 0x30` · `kMenuItemDef 0x2c0` ·
change params: item key at `+0x08`, old/new at `+0x98`/`+0x9c`.

---

## 7. Tooling

The Epic build ships its full PDB, so **member layouts and RVAs are lookups, never guesses**. Any
PDB reader will do; the useful queries are "print every field of `UMenuPage` with its offset" and
"build a byte signature for this RVA and report the hit count in *both* executables" — one hit in
both, or it is not a symbol yet.

Build and verify:

```
cmake --build build --config Release
build\Release\omp_symcheck.exe
```

then deploy `main.dll` to `Mods\SessionOpenMP\dlls\` in each install. See `tools/README.md` for the
full set of gates.

---

## 8. Worked example: a row on a page that is not ours

`ReplayEditors` now carries a **Look At** MultiOption that cycles the players in the session and aims
the replay camera at the chosen one. It is the smallest possible version of section 4, and it lives
entirely inside `chooseArray` / `handleValueChange` — no `g_page` state, because the page is the
game's and we never navigate it.

What it adds beyond the recipe:

- **The option list is rebuilt on every page build** from `session::PeerAt()`, because "who is in this
  session" is exactly what should be re-read when the menu opens. `_multiOptionTexts` Num is rewritten
  each time; Max stays at the array's real capacity.
- **The change is POSTED, not applied in the callback.** Aiming calls into `AReplayCamera`, and the
  engine is still walking its widgets underneath us — same reasoning as Rule 3, so the work happens in
  `PauseMenu_Pump` on the next tick.
- **Failure is scoped.** If the page key does not intern or the row will not build, `g_replayPageKey`
  is zeroed and only this row is lost; the multiplayer menu is unaffected.

The aiming itself is in [`src/game/spectate.cpp`](../src/game/spectate.cpp): write
`AReplayCamera::_lookAtTarget` (+0x868) then `SetCameraType(RCT_Orbit)`, reached via
`ASessionReplayManager::GetInstance()` → `_replayInputController` (+0x280) → `_replayCamera` (+0x260).
Driving the game's own target beats overriding the camera manager's cached POV: the latter rotates
only the rendered view, so anything the editor keyframes or records would disagree with what you saw.
