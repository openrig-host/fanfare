# Fanfare: ergonomic proposals, round 2 (8 Nov 2026, Mistral)

> Xiaomi inline comments (blockquote lines starting `> Xiaomi:`) were added 8 Nov 2026 as a second-opinion review pass: agree/disagree per section, plus corrections verified against the code. Section 5 is an added proposal (cloud revisioning). Mistral's original text is unchanged.

Scope: the next round of ergonomics *on top of* what already shipped. The 7 Nov plan
(`7novreview-opus.md` §7) delivered the Studio shell: menu bar, top bar with isolated
PANIC, scene bar, 22 px status bar, tool rail + context sidebar, Studio strip declutter,
splash, welcome modal, save history. The screenshots (main mixer view, `docs/*.png`)
show that state. This document proposes what is still visibly rough in that state.

Method: read `Source/MainComponent.cpp/.h` (`resized()` at `MainComponent.cpp:3134`,
key handler at `:4146`), `RackSlotComponent.*`, `LibraryPanel.*`, `ToolRail.h`,
`SceneButtonComponent.*`, `QueueTilesPanel.h`, `SetlistPanel.h`, `Theme.h`,
`FanfareConstants.h`, and the manual/index pages that embed the screenshots.

---

## 0. What the current screenshots still say

From the main mixer screenshot:

1. **The middle of the top bar is dead space.** Menu bar left, `◀ No song loaded ▶`
   left-of-center, then nothing until the clock and PANIC on the right. The single most
   stage-relevant fact — *what is next, and is it ready* — sits in the bottom-left
   status bar (`preloadStatusLabel`, `MainComponent.cpp:2958-2971`).
2. **Scene tabs truncate** (`maverick and B3 and Pad – …`) and silently disappear when
   the row is narrow (`MainComponent.cpp:3185-3200` hides them with `setVisible(false)`),
   with no overflow affordance. Active scene is an accent underline inside a gradient
   beveled button (`SceneButtonComponent.cpp` `paintButton` still uses
   `faceTop/faceBot/bevelHi` gradients — not the flat Studio path).
3. **The Songs sidebar** groups by raw uppercased folder names (`CORE SETUPS`,
   `LibraryPanel.cpp:174`), root files fall into an unlabeled `GENERAL` group, and the
   Search field shares its row with `+ Folder` (`LibraryPanel.cpp:504`). The colored
   dot on each row (`detectBadgeColour`) is a name-sniffing guess with no legend.
4. **Every strip draws three `+` / `E` slot rows even when all three chain positions
   are empty** (`RackSlotComponent::resized()`, expanded branch), then two stacked
   `No Input` combos (input row + output row, each 20 px), a nameplate, and a 64 px
   bottom cluster (`RackSlotComponent.cpp:1180`) plus the CC/DYN/`...` row (`:1187`).
   The range text `C-1 - G9 (Full)` is a passive `Label` (`:1177`).
5. **Strip width is global.** Dragging a strip's right edge sets one
   `preferredExpandedWidth` for *all* strips (`MainComponent.cpp:910-912`). There is
   no per-strip width, so the keyboard strip cannot be wider than a muted pad strip.
6. **Master column (right, 176 px, `MainComponent.cpp:3235`)** has FOH L/R analog VUs
   but no IEM metering, bus FX rows show only an `E` button with no plugin name
   (`:3262`), and the Aux returns carry rotated vertical text (`Aux 1 Return`) that is
   unreadable at a glance.
7. **Status bar** shows `MIDI: --`, CPU, RAM, LAT, XRUNS — but no audio device, sample
   rate, or buffer size, and no threshold coloring (CPU label is plain text,
   `MainComponent.cpp:3656`). Nothing in it is clickable.
8. **No stage mode, no scene hotkeys.** `keyPressed` (`MainComponent.cpp:4146-4179`)
   only knows Space = next song, Ctrl+N/O/S, F1, P = panic. The Phase E items from the
   7 Nov plan (F11 stage mode, 1–9 scenes, PgUp/PgDn) were never implemented.

---

> Xiaomi review of section 0 (verify pass, 8 Nov): the observations check out against the code - scene tabs hidden with `setVisible(false)` when narrow (`MainComponent.cpp:3193`), scene buttons still painted with `faceTop/faceBot/bevelHi` gradients (`SceneButtonComponent.cpp:88-89,111`), `GENERAL` root group (`LibraryPanel.cpp:174`), name-sniffing badge colours (`LibraryPanel.cpp:52`), search row sharing with `+ Folder` (`:504`), three slot rows + input row + output row at 20 px each and a 64 px bottom cluster (`RackSlotComponent.cpp:1148-1195`), one global `preferredExpandedWidth` for all strips (`MainComponent.cpp:910-912`), 176 px master column (`MainComponent.cpp:3235`), status bar with no device/sr/buffer and plain-text CPU (`:3656`), and `keyPressed` knowing only Space/Ctrl+N/Ctrl+O/Ctrl+S/F1/P (`MainComponent.cpp:4146-4179`). Two corrections: (1) the range label is NOT passive - `noteRangeLabel` already has a hand cursor, tooltip and click handler (`RackSlotComponent.cpp:550-557`), only its styling is passive; (2) there are SEVEN themes now (Studio + Scarlett/Sapphire/Emerald/Violet + Front Panel Dark/Bright) - High Contrast no longer exists, its old id migrates to Studio (see `xiaomireviewoct7.md` M12). AGREE with all eight observations otherwise.

## 1. Proposals, by surface

### 1.1 Top bar — put "next up" where the eye is

- Add a **Next-up capsule in the empty center of the top bar**
  (`MainComponent::resized()`, after `nextSetlistBtn`, ~`:3170`): `Next: <song>` with a
  state dot — dim = not preloaded, amber pulsing = preloading, green = ready
  (reuse the exact strings/logic from `updatePreloadStatus`, `MainComponent.cpp:2958-2971`,
  but keep the status-bar copy too). Clicking it triggers the same handler as the `▶`
  button. This is the one thing you check between songs; it should not live in the
  bottom-left corner.
- Make the **song title a dropdown**: click `setupNameLabel` ("No song loaded") opens
  a recent-songs menu (the same list the welcome modal shows, from `SaveHistory`),
  plus "No song (new)". Tooltip shows full path + last-modified.
- Keep PANIC isolated as is; add `Esc Esc` (double-Esc within 500 ms) as a second
  panic trigger alongside `P`.

> Xiaomi on 1.1: AGREE with the Next-up capsule - highest-value item here; the state strings and colours already exist in `updatePreloadStatus` (`MainComponent.cpp:2955-2971`) so this is mostly a move, not new logic. AGREE with the title dropdown, but the recents source is `RecentSongs` (`recent_songs.json`, `StartupWelcomeModal.h:19`), NOT `SaveHistory` - SaveHistory stores version snapshots. DISAGREE with Esc-Esc panic as written: Esc already closes modals and (per 1.7) would exit Stage mode, so a double-tap fires panic right after closing a dialog. If wanted, gate it to "no modal open and not in a text field", or use a long-press on PANIC instead.

### 1.2 Scene bar — overflow, active state, hotkeys, reorder

- **Overflow chevron instead of silent hiding.** When `sr.getWidth() < minW`
  (`:3193-3197`), collect hidden scenes into a `▾` button at the right of the row whose
  menu lists them (click = recall). Never let a scene vanish without a way back.
- **Flat Studio rendering for scene buttons**: route `SceneButtonComponent::paintButton`
  through the same flat path `BoutiqueLookAndFeel` uses for other buttons in Studio
  (1 px border, no `faceTop/faceBot` gradient, no `bevelHi`). Active = accent border +
  accent number chip, not a gradient wash.
- **Full name on tooltip**, and give the *active* scene the widest allocation in the
  width loop instead of `sr.getWidth() / fitCount` for everyone.
- **Hotkeys 1–9 recall scenes** (add to `keyPressed`; `SceneButtonComponent` already
  stores `midiPC`, so PC triggers and keys share one path). Shift+1–9 = assign current
  song to that scene (with confirm if occupied).
- **Drag-to-reorder scenes**: `SceneButtonComponent` already implements
  `DragAndDropTarget` for file drops; extend it to accept another scene button as a
  drag source and swap `sceneSetupFilePaths` entries.
- Move `+ Scene` to the **left end** of the scene row (next to the tabs it grows),
  keep `…` (scene menu) at the right.

> Xiaomi on 1.2: AGREE on all six items - the overflow fix repairs a real bug (scenes vanish with no path back). Implementation notes: hotkeys 1-9 must go through the existing TextEditor guard in `keyPressed` (`MainComponent.cpp:4147`) so typing digits in the search box never recalls a scene; and drag-reorder must swap whole scene records (name, file, midiPC), not just `sceneSetupFilePaths`, or the engine's MIDI PC triggers desync. The `DragAndDropTarget` hook already exists (`SceneButtonComponent.h:30`), so this is cheap. Moving `+ Scene` to the left is cosmetic; fine either way.

### 1.3 Songs sidebar — recents, legend, hover actions, import

- Add a **RECENT group** at the top when the search box is empty (last 5–8 loaded songs
  from `SaveHistory`, mono timestamp right-aligned). Favorites stay below it.
- Label the root group **"Songs"** instead of `GENERAL` (`LibraryPanel.cpp:174`), and
  show **counts on group headers** (`CORE SETUPS · 34`).
- Search row: move `+ Folder` into a small `…` menu at the right of the header row, so
  the search field gets full width. Add a **sort toggle** (name ⇅ / date) in the same
  menu; store it in settings.
- Row hover actions (drawn icons, no emoji): star = favorite toggle, pencil = rename,
  folder = move to…, trash = delete (confirm). Today only some of these exist via
  right-click (`listBoxItemClicked`); make them discoverable on hover.
- **Badge legend**: the colored dot is guessed from the filename
  (`detectBadgeColour`). Add a one-line tooltip on the first dot you hover
  ("colour guessed from song name — strip tape colour"), or replace with the strip's
  actual dominant category colour when the song file is loaded once and cached.
- **Import by drag-drop**: accept `.fanfare`/song files dropped from Explorer onto the
  list (`LibraryPanel` is already a `DragAndDropContainer`) and copy them into the
  songs directory, then refresh.

> Xiaomi on 1.3: AGREE on all six. Two corrections: the recents group should read from `RecentSongs` (`recent_songs.json`), not `SaveHistory`; and song files are `.json` (the setups list scans `*.json`) - there is no `.fanfare` extension, so the drag-import filter must be `*.json`. `LibraryPanel` already derives from `DragAndDropContainer` (`LibraryPanel.h:74`) - verified. On hover actions: keep the right-click menu too, and keep the delete confirm next to double-click-to-load.

### 1.4 Channel strips — fewer empty rows, per-strip width, readable range

- **Empty chain positions collapse to one `+ add` row.** If slot positions 2 and 3 are
  empty, draw a single dim "+ add layer" row instead of three identical `+`/`E` rows
  (the 7 Nov plan §4.3 said this; the screenshot shows it still isn't done). Keep
  right-click on the row for the full layer-config menu (`showPluginConfigMenu`).
- **Input + output on one row**: two half-width combos side by side instead of two
  stacked 20 px rows (`RackSlotComponent.cpp:1160-1170`), freeing ~20 px for the fader.
- **Range chip**: make `noteRangeLabel` (`C-1 - G9 (Full)`, `:1177`) look and behave
  like a button in Studio too — hand cursor, hover state, accent-on-armed. It already
  opens the note-range dialog via `NoteRangeLabelListener`; just style it.
- **Fader readout**: small mono dB value under the fader cap, visible while dragging
  and on hover; unity (0 dB) tick mark; double-click on the *fader track* resets to
  0 dB. (Careful: `mouseDoubleClick` at `RackSlotComponent.cpp` currently maps the
  nameplate area to rename and everything else to `onOpenEditor(0)` — use the fader
  track region only, or Alt+double-click, so the existing behaviors don't move.)
- **Bottom cluster as a consistent 3-column grid**: `[M] [FOH] [IEM]` on one row,
  `[CC] [DYN] […]` on the next (today Mute is a full-width row and FOH/IEM are slab
  bars below it, `:1180-1195`). Keep the inverted send logic from Studio (off = amber
  outline) but apply it to the grid buttons uniformly. Long-press-to-learn on
  M/FOH/IEM (`LongPressMouseListener`) stays.
- **Per-strip widths**: store `width` per strip index in the song JSON (optional
  array, backward compatible — absent means current global `preferredExpandedWidth`).
  Dragging a strip's right edge then resizes only that strip
  (`onDragWidth` at `MainComponent.cpp:910` currently changes the global). Double-click
  the edge resets that strip to the global width. This is the biggest single win for
  fitting 10+ strips on a 1366 px laptop: wide keyboard strip, narrow pads.
- **Empty-rack state**: with no song loaded, draw a centered hint overlay in the rack
  area ("Double-click a song on the left, or click `+` on a strip to load a plugin —
  F1 for the manual") instead of 24 identical empty strips. Strips stay clickable
  underneath; the overlay is informational only.

> Xiaomi on 1.4: AGREE strongly. Empty-chain collapse was promised in the 7 Nov plan 4.3 and the three slot rows are still unconditional in `RackSlotComponent::resized()`. Per-strip widths is the right call, but ship it in the SAME batch as the song-bridge field-loss fix (`xiaomireviewoct7.md` M18: songToVar/varToSong drop unknown fields) or widths become the next silently-lost field - write/read through both `RigSerializer` and `FanfareEngine::exportRigToJson/applyRig`, optional array, absent = global width. Range chip: already has hand cursor + tooltip + dialog wiring (`RackSlotComponent.cpp:550-557`); only hover/armed styling is missing, as the proposal says. The mouseDoubleClick caution is accurate (y 105-140 = rename, everything else `onOpenEditor(0)`, `RackSlotComponent.cpp:1314-1321`) - restrict the fader reset to the fader-track rect only. AGREE with the empty-rack hint.

### 1.5 Master column — IEM metering, named FX, readable aux labels

- **Add IEM level indication next to the FOH L/R VUs** (`MainComponent.cpp:3240-3245`):
  either a second compact analog VU pair (there is room if the bus FX rows shrink) or
  a two-segment LED bar labeled IEM L/R. Today you can see FOH clipping but not IEM.
- **Show the loaded FX name on the bus slot buttons** (`fohFxBtns[i]`/`iemFxBtns[i]`,
  `:3262`) — currently only the `E` (edit GUI) button is visible; an empty vs loaded FX
  row is indistinguishable.
- **Aux returns**: replace the rotated `Aux 1 Return` text with a short horizontal
  label (`A1`, `A2`) above the fader, full name in the tooltip, or a horizontal
  mini-strip at the bottom of the master column. Vertical text is the least readable
  element in the screenshot.
- **Master faders**: same treatment as strip faders — mono dB readout on drag/hover,
  unity tick.
- The Last Waltz logo in the corner: keep, but tooltip "Double-click to choose image"
  (it already has `chooseAuxLogoImage`) and don't let it eat clicks meant for the
  adjacent fader.

> Xiaomi on 1.5: AGREE on all five. While adding IEM metering, fix the related dead state from `xiaomireviewoct7.md`: slot `iemLevel` is saved into scenes but never used in the mix sum (`iemBase = fohLevel * iemOffset`, `RackSlot.h:439-441`) - a meter that reads `iemLevel` would display a lie. Meter the actual IEM bus output instead. AGREE with FX names on the bus slots, horizontal `A1`/`A2` aux labels (vertical text is the worst element on screen), master fader readouts, and the logo tooltip.

### 1.6 Status bar — device info, thresholds, clickable segments

- Add **audio device + sample rate + buffer** to the left cluster:
  `ASIO: US-1800 · 44.1 kHz · 512 smp` (all available in `timerCallback` from
  `deviceManager.getCurrentAudioDevice()`). This is the first thing you check when
  something sounds wrong.
- **Threshold coloring**: CPU amber > 70 %, red > 90 %; XRUNS red with count whenever >
  0 (today plain text); LAT amber when round-trip latency exceeds one buffer period.
- **Make segments clickable**: MIDI label → switches sidebar to MIDI monitor context;
  CPU/RAM → opens Resource Inspector; preload label → triggers next song. Cursor +
  tooltip to advertise it.
- Separate the right-hand telemetry with `·` separators so it reads as one cluster,
  not five unrelated labels.

> Xiaomi on 1.6: AGREE - device/sample-rate/buffer in the status bar is overdue; it is the first thing checked when audio misbehaves. Threshold colours should use the existing `Theme::Role::warn/ok/danger` that `preloadStatusLabel` already uses (`MainComponent.cpp:2959-2968`). If the MIDI segment becomes clickable, also fix its content: `midiMonitorLabel` currently shows `SONG: ...` text, not MIDI data (`xiaomireviewoct7.md` trivial #2).

### 1.7 Stage mode + the full keyboard map (finish Phase E from 7 Nov)

- **F11 Stage mode**: hides menu bar and sidebar (rack gets ~300 px more width), grows
  scene tabs and `▶` to 44–48 px targets, locks destructive actions (scene delete,
  strip clear, plugin swap, save-over) behind a "hold to unlock" or a settings toggle,
  keeps PANIC + master faders + scene recall. F11 again or Esc exits.
- **Key map** (extend `MainComponent::keyPressed`, `:4146`): Space = next song (exists),
  Shift+Space = previous, PgUp/PgDn = prev/next, `1–9` = scenes, Ctrl+L = Songs
  sidebar, Ctrl+M = MIDI monitor, Ctrl+/ = shortcut cheat-sheet overlay, F11 = stage
  mode, `P` = panic (exists). Check the web remote bindings
  (`Source/Web/OpenRigWebServer.cpp`) so nothing clashes.
- **Help → "Keyboard shortcuts…"** menu item listing the map (single source: one
  `std::map` of KeyPress → command used by both the handler and the menu).

> Xiaomi on 1.7: AGREE - Phase E was deferred and now is the time. Resolve three conflicts before coding: (1) Esc cannot both trigger Esc-Esc panic (1.1) and exit Stage mode here - pick one; (2) keys 1-9 must respect the TextEditor guard in `keyPressed` (`MainComponent.cpp:4147`); (3) PgUp/PgDn and Space can collide with plugin editor windows that hold keyboard focus - route through the same `originatingComponent` guard. The single `std::map<KeyPress, command>` shared by handler and Help menu is the right structure. AGREE with Stage mode keeping PANIC, master faders and scene recall visible.

### 1.8 Editors: dock the strip editors into the sidebar when there is room

- `ChannelStripComponent` (DSP, ~980×520), CC Mapping, Note Range and MIDI FX are
  still centered modals. When the window is ≥ ~1400 px wide, open them **docked in the
  right side of the rack** (a 320 px inspector column, like Nota) for the selected
  strip, so you can tweak EQ while playing; below that, keep the modal. Sampler and
  MP3 deck stay modal (they are big editors).
- One shared modal frame style for all of them (flat panel, 1 px border, sentence-case
  title, Esc closes, close button top-right). Esc already closes modals — make sure a
  single Esc closes the topmost only.

> Xiaomi on 1.8: AGREE with the direction and AGREE with the proposal's own "high risk" rating - schedule it last. Fold in the modal fixes from `xiaomireviewoct7.md` while there: `ConfigModalOverlay` is the one modal not re-centred on resize, and "single Esc closes topmost only" is unverified. One shared modal frame plus one inspector host beats four bespoke frames. On open question 3: my recommendation is docked for EQ/DYN/CC only, modal for anything that grabs a MIDI learner, so learn mode can never hide behind the rack.

### 1.9 Tablet remote (Source/Web) — match the Studio palette, add the stage essentials

- Restyle `tablet_stage_hud.png` / `tablet_touch_mixer.png` surfaces (HTML in
  `Source/Web/static`) to the Studio tokens: same greys, `#FFA02F`-style accent, mono
  numbers, sentence case.
- **Min 44 px touch targets**, and add to the HUD: **Panic**, scene buttons, and the
  next-up/preload indicator from §1.1 — the three things you actually need from a
  phone on stage.

> Xiaomi on 1.9: AGREE with the palette restyle and with adding Panic / scenes / next-up to the HUD - but sequence it AFTER the web security fixes in `xiaomireviewoct7.md` (C1 arbitrary file read, H1 PowerShell injection via yt-dlp args, H2 unauthenticated control endpoints): pushing more stage-critical control through the tablet before those are patched widens the blast radius. The 44 px touch-target rule should also apply to the existing HUD mixer faders, not just the new buttons.

### 1.10 Cross-cutting polish

- **Hard-coded colour audit**: grep `juce::Colour(0x` and `juce::Colours::` across
  `Source/*.h/.cpp` components and route through `ThemeManager::get(Role)` so all four
  themes (Studio, Front Panel Dark/Bright, High Contrast) stay coherent. `detectBadgeColour`
  already hard-codes five palette entries.
- **Focus-visible ring** in the flat Studio LookAndFeel: flat 1 px buttons currently
  may show no keyboard focus. Add a 1 px accent outline on `hasKeyboardFocus`.
- **Tooltip pass**: sentence case, no HAAALP!-only labels; every abbreviated control
  (`NR`, `DYN`, `CC`, `SMP`) gets a tooltip with the words.
- **Glyph sweep**: no emoji/non-ASCII literals passed to `juce::String(const char*)`
  (debug assert); drawn vector glyphs where an icon is needed.
- **Numbers in mono** everywhere (telemetry, dB readouts, counts, timestamps) — the
  JetBrains Mono stack exists; use it consistently.

---

> Xiaomi on 1.10: AGREE on all five sweeps. One correction: the audit target is SEVEN themes (Studio, Studio Scarlett/Sapphire/Emerald/Violet, Front Panel Dark/Bright), not four - High Contrast no longer exists and its old id migrates to Studio (`Theme.h:579`). Known hard-coded offenders from the round-1 review: `detectBadgeColour`'s five palette entries (`LibraryPanel.cpp:52`) and the LibraryPanel tab colour `0xFF1D2023`.

## 2. Suggested order, risk, payoff

| # | Proposal | Touches | Risk | Payoff |
|---|---|---|---|---|
| 1 | Next-up capsule in top bar (§1.1) | `MainComponent.cpp` resized + `updatePreloadStatus` | low | highest: checked every song change |
| 2 | Status bar device/sr/buffer + thresholds + clickable (§1.6) | `timerCallback`, status bar paint | low | diagnosis at a glance |
| 3 | Scene overflow chevron + flat render + 1–9 hotkeys (§1.2) | `MainComponent.cpp` scene row, `SceneButtonComponent`, `keyPressed` | low | no lost scenes, faster recall |
| 4 | Per-strip widths (§1.4) | `RackSlotComponent` drag, `RigSerializer` (optional field), `MainComponent::resized` | medium (JSON schema — keep optional) | fits 10+ strips at 1366 px |
| 5 | Empty slot rows → one `+ add`; in/out on one row (§1.4) | `RackSlotComponent::resized/paint` | low–medium | ~40 px more fader per strip |
| 6 | Songs sidebar recents/legend/hover actions/import (§1.3) | `LibraryPanel.cpp` | low | faster song finding |
| 7 | F11 stage mode + full key map + shortcuts menu (§1.7) | `MainComponent::keyPressed`, shell visibility | low–medium | live reliability |
| 8 | Master column: IEM meter, FX names, aux labels, fader readouts (§1.5) | `MainComponent.cpp` master area, `AnalogVuMeter.h` | low | mix visibility |
| 9 | Docked strip inspector when wide (§1.8) | strip editor components + new inspector host | high (touches DSP/CC/MIDI-FX UIs) | edit while playing |
| 10 | Tablet HUD restyle + panic/scene/next (§1.9) | `Source/Web/static` | low | remote matches the app |
| 11 | Colour audit, focus ring, tooltip/glyph/mono sweep (§1.10) | all components | low | one coherent look |
| 12 | Cloud backup mirror: SaveHistory to a synced folder (section 5) | `SaveHistory.h`, settings UI | low | survives laptop loss |

All UI work stays on the message thread; nothing here touches the audio callback,
`FanfareEngine`, or the DSP path. The only data-format change is the optional
per-strip width array in the song JSON (§1.4 #4) — read it defensively
(`v["stripWidths"]` may be absent) so old songs load unchanged.

---

> Xiaomi on the order (section 2): sound as ranked. Two insertions: (a) run the `xiaomireviewoct7.md` security batch (C1/H1/H2) before or beside item 10 - the tablet work depends on it; (b) items 4 and 5 ship safer together with the song-bridge field-loss fix (M18). Item 12 (cloud mirror, section 5) added to the table above.

## 3. Verification checklist (per batch)

- Release build through both the VS solution and CMake; 0 new warnings in touched files.
- Screenshots at 1920×1080 and 1366×768 in all four themes; nothing clips at minimum
  window width (`kDefaultWindowWidth` 1240 is the floor).
- Stage checklist: PANIC reachable in one move and ≥ 24 px from anything else; scene
  recall and next song in one click *or* one key; every destructive action confirms;
  every scene reachable even when the scene row overflows.
- Load the three heaviest songs (*Superman's Song w- Cello*, *Land down under*,
  *Last Waltz – Nektar-1*) and confirm song-switch time is unchanged vs. the current
  build (guards against any accidental message-thread work in the load path).
- Web remote still drives songs/scenes after §1.9; test from the tablet HUD screenshot
  setup (SoftAP hotspot).
- Old song files (pre-width-array) load and save without migration prompts.

---

> Xiaomi on the verification checklist (section 3): add two checks - old songs without `stripWidths` load unchanged and re-save cleanly (item 4), and the mirror folder receives every snapshot exactly once, including prune deletions (item 12). Also correct "all four themes" to all seven.

## 4. Open questions for Dave

1. Per-strip widths (§1.4 #4): OK to add an optional `stripWidths` array to the song
   JSON, or do you prefer keeping one global width and just making Stage Compact smarter?
2. Scene hotkeys 1–9: any conflict with existing controller PC mappings on your CK88
   (scenes already bind to MIDI PC via `SceneButtonComponent::midiPC`)?
3. Docked inspector (§1.8) vs modal: do you want DSP editing visible *while playing*,
   or is modal safer on stage (no accidental edits)?
4. Aux return labels: short `A1/A2` chips, or horizontal mini-strips at the bottom of
   the master column?
5. Badge dots in the Songs list: keep the name-sniffed guesses with a legend, or derive
   from the strip tape colours stored inside the song file?

---

## 5. Cloud revisioning: mirror the version history to a synced folder (added by Xiaomi, 8 Nov)

The versioning enhancement requested alongside this review. Motivation: `SaveHistory.h` is local-only, so a stolen or dead laptop takes every snapshot with it. No cloud APIs are needed: keep the local history authoritative and mirror it into a folder the user's existing sync client (OneDrive, Google Drive, Dropbox) already watches.

- **Setting**: "Backup mirror folder" (empty by default = today's behaviour), picked with a file chooser in the settings overlay. Suggest `%USERPROFILE%\OneDrive\Fanfare` as the obvious choice.
- **What mirrors**: on every `recordSave` / `snapshotBeforeLoadAsync` completion, enqueue an async copy of the same relative path (`history/<kind>/<name>/<stamp>.<ext>`) into the mirror, reusing the existing `AsyncSnapshotTracker` worker so the message thread never blocks. Prune and delete operations (`SaveHistory.h:186` `pruneUnlocked`) apply to both trees.
- **Atomic in the mirror too**: copy to `<name>.tmp` + rename; never hold the local file open during the copy (sync clients lock files mid-upload). Fix M9 (non-atomic snapshot writes) and H13 (restore ignores `copyFileTo` failure) first - a mirror would otherwise replicate corrupt or half-written snapshots as gospel.
- **Machine namespace**: mirror under `<mirror>/<machine-name>/history/...` so two rigs syncing the same cloud folder never fight over conflict copies.
- **Optional second job**: mirror the songs/sets/strips library roots the same way - arguably worth more than the history itself.
- **Notes**: the sync client does the upload, so this works offline and costs zero network code in Fanfare. Google Drive's virtual-drive mode is slow with many small files; the existing prune (20 snapshots / 7 days) keeps counts low - document the mirror for Drive users.
- **Verification**: kill the app mid-save and confirm no partial snapshot appears in either tree; disable the mirror mid-session and confirm local history still works.

---

## 6. Implementation log (8 Nov 2026, Mistral)

Batch 1 of the section 2 order (items 1-3) is implemented and Release-build verified:

- **Item 1 (next-up capsule)**: new `nextUpLabel` in the top bar center (`MainComponent::resized`), fed by `updatePreloadStatus()` with the same state colours as the status-bar label (warn/ok/danger/dim). Clicking the capsule or the status-bar label triggers the same handler as the `>` button (`mouseDown`).
- **Item 2 (status bar)**: new `audioDeviceLabel` shows `<device> | <kHz> | <buffer> smp` (danger-coloured "audio: no device" when none); CPU turns warn > 70 % / danger > 90 %; LAT turns warn when round-trip exceeds one buffer period. Segments are clickable: MIDI label toggles the MIDI monitor sidebar context, device label opens Audio & MIDI settings, preload/next-up labels load the next song.
- **Item 3 (scene bar)**: scenes that no longer fit are collected into `hiddenSceneIndices` and surfaced via a `>>` overflow button (menu lists them, click recalls). Keys 1-9 recall scenes through a new shared `recallScene()` (also used by the scene buttons and the overflow menu); the existing TextEditor guard keeps search-box digits safe. Scene buttons render flat (1 px border, no gradient/bevel) in the Studio theme.
- **Build fix found along the way**: the CMake build did not compile `JuceLibraryCode/include_juce_dsp.cpp`, so `juce::dsp::Convolution` was unresolved at link (7 unresolved externals, pre-existing). Added the TU to `target_sources` in `CMakeLists.txt:44`.
- **Build result**: `MSBuild build/Fanfare.vcxproj -p:Configuration=Release` links clean; `build/Fanfare_artefacts/Release/Fanfare.exe` produced (deployed to workspace root by the post-build step). No errors, no new warnings in the touched TUs.

Not done yet (next batches): items 4-5 (per-strip widths, empty-slot collapse), 6 (sidebar), 7 (F11 stage mode + full key map), 8 (master column), 9 (inspector), 10 (tablet), 11 (sweeps), 12 (cloud mirror).
