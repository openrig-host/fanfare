actually Nov 23

## Verdict

The real-time core is disciplined and worth keeping. In every `processBlock` I scanned, cross-thread access is atomics and try-locks, buffers are pre-sized, and plugins are evicted under a lock and destroyed outside it. One lapse: two latched `callAsync` calls on the audio thread in the MP3 player.

The structural problem is that the same knowledge is written down in many places and the layers leak into each other. Adding one strip parameter means editing about fifteen sites, and that has already produced defects.

## Defects the structure has already caused

| Defect | Evidence | Effect |
|---|---|---|
| Compressor makeup gain and preset are saved by no path | Only their own UI handlers store them ([ChannelStripComponent.h:330](Z:/davecore/Source/ChannelStripComponent.h), line 314) | Both reset on every rig reload |
| Per-layer MIDI channel is saved by no path | Set at [RackSlotComponent.cpp:368](Z:/davecore/Source/RackSlotComponent.cpp); `exportSlot` omits it ([FanfareEngine.cpp:2138](Z:/davecore/Source/FanfareEngine.cpp)) | Layer channel reverts to "Strip" on reload |
| Arpeggiator defaults disagree across four copies | 127 BPM / 0 octaves ([SimpleArpeggiator.h:235](Z:/davecore/Source/SimpleArpeggiator.h)), 120 / 1 ([Scene.h:57](Z:/davecore/Source/Scene.h)), 127 / 2 / gate 0.75 ([RigModel.h:173](Z:/davecore/Source/RigModel.h)), plus read-time literals at `FanfareEngine.cpp:1789` | New scenes and songs get different defaults depending on the code path |
| MP3 play callback is overwritten | [RackSlot.h:35](Z:/davecore/Source/RackSlot.h) sets auto-un-bypass on play; [MainComponent.cpp:323](Z:/davecore/Source/MainComponent.cpp) replaces it with a web notification | Slot 10's deck no longer un-mutes its strip, and the web layer only hears slot 10 |

All four are small patches and worth doing before any refactor.

## Recommendations, in priority order

### 1. One definition of strip state (highest leverage)
- **Problem:** strip settings exist as live atomics, as `SlotState`, as the `Fanfare::SongSlot` model, and as hand-built JSON. The 10-band EQ field alone is enumerated at 15 sites (9 in `FanfareEngine.cpp`, 6 in `RigSerializer.cpp`). `RigSerializer.h` itself calls the engine path "shims during migration".
- **Change:** make one plain `StripSettings` struct the only definition, with capture, apply and JSON conversion beside it. Scenes, rig export/import, strip files and the web remote all go through it.
- **Compatibility:** keep existing JSON keys exactly; add the missing fields as new optional keys.
- **Risk and effort:** low to medium risk, medium effort. It is mostly mechanical, and `selftest` already has a byte-stable round-trip check to verify against.

### 2. Put a core layer between the GUI and the engine
- **Problem:** `MainComponent` inherits seven roles and is the audio callback and MIDI router. It privately owns the engine and device manager, and holds the Windows power-plan and CPU-affinity logic ([MainComponent.h:54](Z:/davecore/Source/MainComponent.h), lines 128, 208–236, 444–455). The web server takes a `MainComponent*` and includes its header.
- **Change:** a non-Component `AppHost` owning the device manager, engine and MIDI routing. The UI observes it and the web layer talks to it.
- **Benefit:** removes the two-way dependency between the web server and the main window, and makes everything except widgets testable headless.
- **Risk and effort:** medium risk, high effort, but incremental. Extract glitch telemetry, MIDI trigger routing and CPU topology first; leave the audio callback forwarding for last.

### 3. Make rack location and chain-slot source proper types
- **Problem:** rack positions are magic integers (≥100 means aux return, negative means master bus, with a bool named `isFohOrChannel`). A chain slot's kind is five separate flags with hand-maintained exclusion; one copy already misses a term ([RackSlot.h:332](Z:/davecore/Source/RackSlot.h)). Three near-identical 60-line render branches plus a fourth copy of the drain logic sit in `RackSlot::processBlock`. Every strip owns nine built-in synths, so the app constructs 234 at startup.
- **Change:** a `RackLocation` struct and a `SourceKind` enum with one render helper. The three synths already share the same method surface and differ only in panic naming. Optionally construct synths on first use, later.
- **Risk and effort:** medium on both, because it touches the audio path. Do it as pure extraction after the test harness in the order below.

### 4. One plugin build service
- **Problem:** the create, configure and prepare sequence appears three times in `FanfareEngine.cpp`. Two of those spawn a thread and pump window messages while waiting, which on the fallback path happens inside `applyRig` with the rack half-applied. There are 21 plugin-release sites and seven different timeouts. B3 external plugins are built inside the audio-silence window (line 1607), which lengthens rig-load dropouts.
- **Change:** an async build that returns to the event loop instead of pumping, one retire-and-reap helper, and a single plugin-quirks table (build thread, layout, timeouts) that users can override.
- **Risk and effort:** medium-high risk because this is where the October hang work lives. Do it after 1 and 2.

### 5. Split the engine along its existing seams
- **Problem:** `processAudio` is 672 lines inline in the header ([FanfareEngine.h:175](Z:/davecore/Source/FanfareEngine.h)). `applyRig` is 616 lines. The audio engine also holds the gig notepad, scene store, plugin registry and legacy file migration. FOH and IEM master-bus code is copy-pasted.
- **Change:** extract a DSP work pool, a `MasterBus` used twice, a plugin registry, a scene store and a notes store. Split `processAudio` into named stages in a `.cpp`.
- **Risk and effort:** low risk if it is pure movement with an offline render test, medium effort.

### 6. Settings and notification services
- **Problem:** JUCE's `ApplicationProperties` is never used. About twenty hand-rolled settings and state files each have their own read/write code, and three have two owners. There are eight hand-rolled singletons. Callbacks are single-subscriber, which is what caused the overwritten MP3 hook.
- **Change:** one typed settings service and multicast listeners.
- **Risk and effort:** low on both.

### 7. Close the web layer's boundary
- **Problem:** the server always starts on all interfaces on port 8080 and the result is ignored ([MainComponent.cpp:315](Z:/davecore/Source/MainComponent.cpp)). The in-app Help depends on it ([HelpLauncher.h:18](Z:/davecore/Source/HelpLauncher.h)). Commands are 40 string comparisons across three chains; the survey found 11 with no client. GET builders run on network threads while WebSocket equivalents use `callAsync`.
- **Change:** a table-driven command path, an off / loopback / LAN setting, Help that does not need the server, and mirror targets that unregister when a plugin window closes.
- **Risk and effort:** low on both. More relevant now that the app is published.

### 8. Build and file hygiene
- **Problem:** 69% of hand-written code is in headers (52K of 75K lines), so any core-header edit recompiles nearly everything. Projucer and CMake lists are synced by hand (identical today). Twelve comments cite `nov8mistral.md`, which is not in the repo. `DWORD_PTR` is used unguarded, so the macOS branch cannot compile. Your personal hardware CCs, input mapping, default scenes and plugin paths are compiled in ([FanfareConstants.h:66](Z:/davecore/Source/FanfareConstants.h), [FanfareEngine.h:1262](Z:/davecore/Source/FanfareEngine.h)).
- **Change:** glob sources in CMake, move header code to `.cpp` as files are touched, and ship your rig as a first-run template instead of constants.
- **Risk and effort:** low on both.

### 9. Consolidate the UI kits gradually
- **Problem:** about 100 colour literals sit outside the theme (350 total, but 220 are the B3 walnut panel and the look-and-feel). FM and B3 each carry their own knob, fader, header, patch display and keyboard alongside the shared kit.
- **Change:** converge onto the existing shared kit as each editor is touched, and add a check that blocks new colour literals. No third-party framework.

## Leave alone
- The real-time discipline: the quiet window, try-locks and bail-to-silence.
- The JSON rig format: wrap it, do not redesign it.
- The DSP code itself: it scanned clean.

## Suggested order
1. Patch the four defects and make `selftest` a build target, with an offline render harness and the audio-path scan as tests.
2. Items 1 and 6, which touch no audio code.
3. Items 2 and 7, which remove the UI, engine and web cycle.
4. Items 3 and 4, outside gig season.
5. Items 5, 8 and 9, opportunistically.

If you want one starting point, it is item 1 plus the test harness. It fixes real data loss and makes every later step verifiable.











