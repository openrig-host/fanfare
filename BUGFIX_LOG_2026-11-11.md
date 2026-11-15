# Bugfix & UX Log — 2026-11-11 (ZCode, THINKPAD)

Executes the 2026-11-11 audit list (12 bug fixes) plus the strip-tools UX rework
requested by Dave the same day. Base: trunk `8eca1c5` (v1.60, "The 1984 Keyboard
Rig Edition" line). `Fanfare.exe` rebuilt Release via CMake; selftest relinked
from fresh objects; corpus parse suite **38/38 OK** after every change round.

## Part 1 — Audit bugfixes

### 1. IEM level ignored in the mix — FIXED (was real)
- `RackSlot.h` `sumToBuses` / `sumToIemBus`: IEM bus gain was always
  `fohLevel * iemOffset`; the rack IEM fader (`iemLevel`) was saved, restored,
  MIDI-learnable — and had zero effect on audio.
- Now: `iemBase = iemLevel * iemOffset` (IEM fader = base level, IEM TRIM in
  DYN = relative trim, 0..2). Rack IEM fader is live.
- MIDI-CC learn for IEM now writes `iemLevel` only (previously also stomped
  `iemOffset = norm*2`, which would double-apply under the new formula).
- Comments + tooltips updated: trim is "relative to the IEM fader"
  (`RackSlotComponent.cpp` IEM badge tooltips, `ChannelStripComponent.h` DYN
  IEM TRIM tooltip, `docs/manual.html` IEM TRIM section).
- Behaviour note: strips saved with the IEM fader untouched (0.8 default) now
  hear 0.8 x trim on the IEM bus instead of the FOH fader value x trim. On a
  default rig (both faders 0.8, trim 1.0) the mix is identical.

### 2. Song JSON round-trip dropped unknown fields — FIXED (was real)
- `RigModel.h`: `Song` gained `juce::var extra`.
- `RigSerializer.cpp`: `varToSong` stashes every top-level key the struct does
  not model into `Song::extra` (new `isKnownSongKey` list, includes legacy
  top-level `name`); `songToVar` re-attaches extras that the struct still
  doesn't model. Newer-schema or third-party keys now survive save/load.

### 3. Save-history atomicity / restore failures — ALREADY FIXED in tree
- Commit `975d47f` already shipped temp+rename snapshots
  (`SaveHistory.h` 233-260) and checked restore (`VersionHistoryModal.h`
  195-201). Nothing to do.
- Live remnant fixed: `SetlistManager::saveSetlist` wrote a raw
  `FileOutputStream` straight onto the target. Now stages through a `.tmp`
  sibling + `moveFileTo` (MOVEFILE_REPLACE_EXISTING), same pattern as
  `RigSerializer::save`.

### 4. Static file path traversal — ALREADY MITIGATED
- `OpenRigWebServer.cpp` `serveStaticFile` already rejects `..`, `:`, `\`,
  NUL, protocol-relative and non-`/`-prefixed paths after URL-decoding,
  enforces an extension whitelist, and re-checks `isAChildOf` + symlink.
  Verified, no change needed.

### 5. Unauthenticated control endpoints — ALREADY MITIGATED
- All `/api/*` + WebSocket upgrades already require a 128-bit session token
  (header/query/cookie, constant-time compare) plus an origin check; the
  token ships in the QR-paired URL. Verified, no change needed.

### 6. yt-dlp invocation — hardening applied
- `YoutubeDownloadManager.h`: invocation was already a `ChildProcess` argv
  array (no shell). Added `--` end-of-options separator before the pasted URL
  in both call sites (`fetchVideoInfoAsync`, download job) so a URL starting
  with `-` can never be parsed as a yt-dlp flag.

### 7. MIDI status segment showed song text — FIXED (was real)
- `MainComponent_RigOps.cpp`: the two `SONG: ...` completion messages moved
  off `midiMonitorLabel` onto `preloadStatusLabel` with a 5-second hold
  (`showSongStatus`, honoured by `updatePreloadStatus` so preload text can't
  wipe the message instantly).
- `MainComponent.h/.cpp`: the left status segment is now a real MIDI readout.
  `handleIncomingMidiMessage` formats the last event (note-on/off with name
  and velocity, CC, PC, bend, aftertouch, channel pressure; clock/active-sense
  floods ignored) into a SpinLock-guarded stash; `timerCallback` drains it on
  the message thread. Click-to-open MIDI monitor behaviour unchanged.

### 8. ConfigModalOverlay re-centre — ALREADY FIXED in tree
- `MainComponent.cpp` `resized()` already re-centres `configOverlay`
  (`centreWithSize`). Verified, no change needed.

### 9. Version resource + stale docs — FIXED
- Root cause for the stale/empty VERSIONINFO: JUCE's juceaide-generated
  `Fanfare_resources.rc` only re-fires on icon changes, so the exe shipped
  1.5.2 metadata while `project(VERSION)` said 1.6.0. `CMakeLists.txt` now
  deletes the generated rc at configure time so every build regenerates it.
  Verified: rebuilt exe reports FileVersion/ProductVersion 1.6.0.
- Projucer route: `Builds/VisualStudio2026/resources.rc` updated
  1.55 -> 1.60.
- `docs/manual.html`: four "(v1.55)" chapter tags -> v1.60.

### 10. Hard-coded colours off the theme system — FIXED
- `LibraryPanel.cpp` `detectBadgeColour`: the five stage-tape badge hues are
  kept for dark themes; on light themes (background perceived brightness
  > 0.5) they get a `darker(0.35f)` pass so badges stay readable.
- Reviewed and left alone: `0xFF1D2023` exists only in the `Golden_Aug30`
  archive snapshot, and the near-white Panic ink `0xFFFFF8F6` is correct on
  every theme's red panic cap.

### 11. FM msfa FIXMEs — ADDRESSED
- MPE switchable: `FmEngine::setMpeEnabled(bool, int bendRange)` (+ getters)
  added; `controllers.h` FIXME replaced with a pointer to the switch. Note:
  the vendored msfa has no per-channel MIDI decode layer that reads these
  flags yet, so a UI toggle would be cosmetic; true per-note MPE tracking is
  future work.
- `env.cc` low-level TODO: finished envelopes (post-release) that park at the
  16<<16 floor now snap to true silence so operators stop grinding
  denormal-range math. Nonzero-R4 hold levels park above the floor and keep
  their authentic hold. Upstream attack-accuracy TODO left as-is (risky,
  low value).

### 12. Studio button focus ring — FIXED
- `BoutiqueLookAndFeel.cpp`: Studio `drawButtonBackground` branch now draws a
  1px accent outline when `button.hasKeyboardFocus(false)`. Front Panel
  (hardware-look) themes intentionally unchanged.

## Part 2 — Strip tools UX (Dave's same-day request)

Problem: Studio strips buried 12 actions behind `CC | DYN | ...`; the menu's
"Note range & transpose..." item lied (transpose lived only in the MIDI FX
dialog). Decision (Dave, via options): smart row + right-click, transpose
moved into the note-range dialog.

- **KEY button in the strip row.** `noteRangeButton` relabelled NR -> KEY,
  now visible on all non-return strips in Studio too. Row is
  `CC | DYN | KEY | ...` (normal) and `CC | KEY | ...` (monitor-in).
  Front Panel themes keep their existing full button rows (relabelled).
- **Menu regrouped.** `showStripActionsMenu` now: strip name header,
  **Performance** (MIDI FX, 8-pad sampler, BLEEP-216, CoplanDX, MP3 deck),
  **Strip** (save/load preset, rename, colour). KEY/DYN/CC items removed
  (they are buttons); right-click on a Studio strip still opens the menu
  (pre-existing behaviour, verified).
- **Amber tool lights.** Strip timer drives a `toolActive` button property;
  `studioButtonTextColour` renders it in `Theme::warn`. KEY lights when the
  strip note filter is narrowed or transpose is non-zero; `...` lights when
  arp / harmonizer / sampler / any chain synth or FM / MP3 deck is active.
- **Transpose in the note-range dialog.** `NoteRangeComponent` gained a
  TRANSPOSE bar: -48..+48 slider (same `slot.setTransposeSemis` setting as
  the MIDI FX dialog, so the two stay in sync), quick buttons
  -12 / -1 / 0 / +1 / +12, live readout, recessed-well chrome, external-change
  sync via the existing 25 Hz timer. Dialog 860x310 -> 860x358 (all four
  launch sites updated); title now "NOTE RANGES, SPLITS & TRANSPOSE".
- **Docs**: manual strip-action-row and note-range chapters updated for KEY,
  the regrouped menu, the lights, the transpose bar, and the corrected IEM
  TRIM semantics.

## Verification

- Release build via CMake (VS 18 toolchain): clean.
- `build/selftest.exe` relinked from fresh Release objects each round.
- `.vibe-runtests.py parse`: **38/38 corpus files OK** (songs+sets+strips).
- VERSIONINFO verified on the rebuilt exe (1.6.0).
- Full-engine load run (selftest `full`) was skipped by Dave's call — plugin
  builds exceed short timeouts and he went straight to live testing.

## Part 3 — FM mode bar "LAYER" / "8-WAVE" chips (Dave's second ask)

Dave suspected the arch chips on the CoplanDX mode cards do nothing. Confirmed
with nuance: the whole mode card is the real control (click selects the mode);
the chips are painted labels (FmModeBar.h) with no hit areas — commit
`92f7563` even calls its own icons "eyecandy". The labels are honest (8-WAVE
is a real feature: per-operator W1-W8 via `FmOpWaveButton` + `FmWaves.h`;
LAYER describes the real dual detuned engine) but were drawn with a fill +
border identical to actual toggles. Fix (Dave's pick): restyled as passive
text tags — no fill/border, `textFaint` (dim accent when selected) — so
nothing reads as a switch. UI-only paint change; rebuilt clean.

## Deferred / open

- Enhancement backlog from the same audit (per-strip widths, songs sidebar,
  F11 stage mode, docked inspector, tablet HUD restyle, history mirror,
  README roadmap) — untouched.
- Full MPE decode layer for the FM synth (the switch API exists; nothing
  reads it yet).
- Remaining direct-write saves flagged by the audit explorer
  (`LibraryPanel.cpp:39`, `MainComponent.cpp:1138/1171/1190`,
  `MainComponent_RigOps.cpp:383`) — candidates for the same tmp+rename
  pattern next pass.
- Working tree left uncommitted for Dave's review; no tag/bump (still v1.60).
