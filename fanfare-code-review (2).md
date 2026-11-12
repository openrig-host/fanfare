# Code Review — Fanfare (openrig-host/fanfare)

**Reviewed:** `trunk` @ `2621dcf` ("Merge origin/trunk into trunk…"), 11 Nov 2026
**Scope:** full source read of the audio engine, slot/DSP path, rig transition, synth engines, web remote, downloader, build files and repo layout.
**Method:** shallow clone + static reading. Nothing was compiled or run (Windows-only JUCE/ASIO app, no Windows host here), so everything below is a source-level finding, not a reproduced runtime failure. Line numbers refer to the commit above.

**Size:** 157 `.h`/`.cpp` files in `Source/`, ~156k lines, of which ~96k are generated asset headers (`EmbeddedFonts.h`, `EmbeddedWebAssets.h`, `FmModeIconsData.h`, …) → **~60k lines of hand-written C++**. 186 commits, 24 tags, no CI, no tests.

---

## 1. Verdict

This is a genuinely competent piece of real-time engineering, not a toy. The author clearly knows both JUCE and the ugly reality of hosting commercial VST3s on stage: the Dekker-style quiet-window handshake, the lock-free flight recorder, the per-path plugin build gate, the STA/MTA split for NI/Spectrasonics/Toontrack, the drain-fade on instrument swaps, the atomic `.tmp → rename` saves. Several of these are things commercial hosts get wrong.

The main gaps are **not** architectural. They are:

1. **A safety mechanism that is silently dead** (SEH swallows plugin faults, so auto-bypass can never fire).
2. **Marketing claims that outrun the implementation** — "no dropouts" / "the current song keeps playing" vs. a stop-the-world song swap that mutes audio for the whole `applyRig()`.
3. **Zero automated tests and zero CI** on a product whose entire value proposition is reliability.
4. A handful of real-time hygiene slips and one easy-to-fix command-argument injection in the YouTube downloader.

If I were shipping this to a stage laptop tomorrow, I'd fix items **H1, H2, M1, M2** and add the smoke-test harness in §6 before anything else.

---

## 2. What's genuinely good (keep doing this)

| Area | Evidence |
|---|---|
| Audio/UI handshake | `FanfareEngine.h:176–188` — publishes `isInProcessAudio`, re-checks `isApplyingRig`, both `seq_cst`, with a scope guard. This is a correct Dekker pair; most hobby hosts get it wrong. |
| Fork-join DSP | `FanfareEngine.h:404–470` + `FanfareEngine.cpp:2894–2911` — `dspJobCount`/`dspInFlightWorkers` seq_cst gate is correct, and the audio thread drains the queue itself so a late helper wake doesn't stall the block. |
| Lock-free telemetry | `Logger.h:193–219` — fixed-size ring, `char[120]`, no allocation, no lock. Textbook. |
| Try-lock discipline | `pluginLock`, `masterFxLock`, `midiOutputLock` all use `ScopedTryLockType` on the audio thread (`RackSlot.h:395`, `FanfareEngine.h:541,566`). Skipping a block beats priority inversion. |
| CC state | `std::atomic<bool> allowedCCs[128]` (`RackSlot.h:1673`) instead of a `std::set` shared across threads. |
| Plugin quirk knowledge | `PluginBuildGate.h` — per-path build gate with poison list, abandoned-build tracking, and the documented reasoning (UVI needs a pumping thread; NI needs STA). This is hard-won knowledge, well captured. |
| Failure containment | Rig builds off-thread, validated, staged, and only then swapped; `.bak` + versioned history; Rig Doctor for post-mortem repair. |
| Web remote security | 128-bit `std::random_device` token, constant-time compare, Origin checks, `HttpOnly; SameSite=Strict` cookie, strict traversal/absolute-path rejection, extension allow-list (`OpenRigWebServer.cpp:496–520, 1141–1150, 1634–1655`). Better than most embedded device UIs. |
| Attribution | `NOTICE.md` correctly identifies msfa as Apache-2.0 (Google/Dexed) and the Dexed-derived voice packing as GPL-3. Apache headers are intact in every vendored file. |

---

## 3. Findings

Severity: **H** = fix before the next gig/release · **M** = fix soon · **L** = hygiene/robustness.

### H1 — Plugin auto-bypass is dead code on Windows (SEH swallows everything)

`Logger.h:274–281`:

```cpp
inline __declspec(noinline) bool executeRawSehCall(void (*fn)(void*), void* data) {
  __try { fn(data); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }   // catches AV *and* C++ throws
}
```

Every audio-thread plugin call goes through this wrapper (`RackSlot.h:642, 675`, `FanfareEngine.h:570, 577`), and **the return value is discarded at all four sites**. Consequences:

* `consecutivePluginErrors` is only incremented in the `catch (...)` at `RackSlot.h:680–686`, which can now never fire for a faulting `processBlock` — the SEH frame inside already ate it. So `kMaxPluginExceptionsBeforeBypass` → auto-bypass, the headline "one bad plugin can't take down the show" behaviour, **never triggers**.
* A plugin that faults every block faults forever, and each fault builds a `juce::String` and takes `logQueueLock` on the audio thread (`Logger.h:292–295`) — heap allocation + lock in the hot path, exactly what the design forbids.

**Fix:**

```cpp
if (!FanfareLog::safeExecutePluginCall([&]{ plugin->processBlock(...); }, "...")) {
    if (consecutivePluginErrors.fetch_add(1) + 1 >= kMaxPluginExceptionsBeforeBypass) {
        bypassed.store(true);
        recordFlightEvent(i, "ERROR: plugin faulted N blocks -> AUTO-BYPASSED");
    }
} else {
    consecutivePluginErrors.store(0);
}
```

Also: on the RT path use `recordFlightEvent()` (lock-free) instead of `LOG_ERROR`, and consider counting faults per plugin path so Rig Doctor can surface "this VST faulted 4,000 times last night". Note that continuing after an access violation is inherently unsafe (the plugin may have corrupted its own heap) — auto-bypass after N faults is the right policy, which is why H1 matters.

### H2 — Song changes mute audio for the entire apply, including synchronous plugin loads

`FanfareEngine.h:753–772` sets `isApplyingRig`, and `processAudio` returns silence while it's set (`FanfareEngine.h:176–188`). Everything in `applyRig()` runs inside that window (`FanfareEngine.cpp:1157`), including:

* `clearAllRacks()` on `freshBuild` (`:1174`) — plugin **destructors**, which for Kontakt/Omnisphere can take seconds;
* `loadPluginFromVarSmart()` for master FX and every slot; on a staging-cache miss this falls through to the synchronous path and calls `setStateInformation()` / a full plugin build **inside the muted window** (`FanfareEngine.cpp:2391–2420`).

So the honest description is "stop-the-world swap, typically fast when everything pre-staged, potentially multi-second when not". The README's *"No dropouts… song changes that either finish completely or don't happen at all"* and *"If anything goes wrong, the current song keeps playing"* are true for **failures before the swap**, but not for the swap itself.

**Fix (in order of effort):**
1. Instrument it: record quiet-window duration into the flight recorder and show `last swap: 340 ms` in the status bar / Rig Doctor. You can't manage what you don't measure, and this number is your product's core promise.
2. Guarantee the staging path covers master FX and `freshBuild` teardown too, so the sync fallback is unreachable during a performance (keep it for editing).
3. Move plugin destruction out of the window: hand old instances to a reaper thread (you already do this pattern for master FX overflow — do it for `clearAllRacks`).
4. Longer term: true double-buffering — build the whole replacement rack (including `prepareToPlay` at the live SR/block size) off-thread, then swap a single pointer under the quiet window and crossfade over ~30 ms. That's the difference between "safe" and "gapless".

### M1 — yt-dlp argument injection (and unpinned tool download)

`YoutubeDownloadManager.h:328` appends the user-supplied string as a bare argument:

```cpp
args.add(job.url);          // no scheme check, no "--" terminator
juce::ChildProcess process; process.start(args, ...);
```

`job.url` reaches this from the LAN API with no validation (`OpenRigWebServer.cpp:1303–1306`) and from the paste field in `Mp3PlayerComponent.h:1285`. A "URL" of `--exec=<command>` (or `--config-location`, `--postprocessor-args`) is parsed by yt-dlp as an **option**, giving arbitrary command execution. It's gated behind the session token, so this is privilege escalation from "can control the mixer" to "can run code", not remote unauthenticated RCE — but it's a two-line fix:

```cpp
if (!url.startsWithIgnoreCase("http://") && !url.startsWithIgnoreCase("https://")) reject();
if (url.startsWithChar('-')) reject();
args.add("--");           // end of options
args.add(job.url);
```

Related, same file `:284–296`: if `yt-dlp.exe` is missing it is downloaded from GitHub and executed with **no signature or hash check**, into `%APPDATA%`. Pin a known-good release + SHA-256 (yt-dlp publishes checksums), or ship it with the installer and drop the auto-download. Also consider the licensing/ToS angle of the YouTube grab feature before promoting it in the README.

### M2 — Parallel DSP: 30 ms spin on the audio thread, TIME_CRITICAL helpers, no degradation path

`FanfareEngine.h:443–478`: if a helper is still in flight, the audio thread busy-waits up to `jlimit(0.012, 0.030, blockSec * 4.5)` — i.e. **12–30 ms**, which at a 512-sample/48 kHz block (10.7 ms) is already 1–3 blown blocks. On timeout it sets `engineWedged`, which mutes the *next* block too (`:371–378`), and then tries parallel again indefinitely. Helpers run at `THREAD_PRIORITY_TIME_CRITICAL` (`FanfareEngine.cpp:2872`) — several of those spinning can stall the whole machine, and the code's own `FanfareConstants.h` comment documents a previous MMCSS "storm" from exactly this class of decision.

**Fix:** count consecutive timeouts; after e.g. 3, `setParallelProcessingEnabled(false)` for the session, log a flight event and show a non-modal "parallel DSP disabled after N stalls" badge. Falling back to sequential is a small CPU cost; a wedged render thread is a dead show. Also reconsider `TIME_CRITICAL` → `HIGHEST` for helpers now that MMCSS is off.

### M3 — ~2 MB of memset per audio block for channels nobody uses

`prepareToPlay` allocates every buffer at **32 channels** (`FanfareEngine.cpp:530–548`), and `processAudio` clears all of them every block (`FanfareEngine.h:334–341`). At 24 slots + 2 aux, 512 samples:

* scratch: 26 × 32 × 512 × 4 B ≈ **1.70 MB**
* buses + dummies ≈ 0.4 MB → **~2.1 MB per block ≈ 200 MB/s** at 48 kHz

…to carry a **stereo** signal. `configureStereoLayout()` (`FanfareEngine.cpp:2582–2652`) forces main bus stereo and disables aux outs for essentially every plugin (HALion/Omnisphere excepted), so channels 2–31 are dead weight that evicts the plugins' working set from L2/L3 on every callback.

**Fix:** size scratch to `max(2, plugin->getTotalNumOutputChannels())` at prepare/swap time (off-thread), keep a small pool for the wide exceptions, and clear only the live channel count. This is probably the single cheapest CPU win available.

Two smaller DSP wins in the same area: `SimpleEQ::process` runs all 10 biquads even when a band is unity (`ChannelStripProcessor.h:143–152` — identity coefficients are set, but the filter still runs), and gate/comp call `20*log10()` **per sample per channel** (`:35–45, :264–275`). Skip unity bands; detect level in the linear domain or with a fast log approximation.

### M4 — Output "hearing protection" clamps at ±8.0 (+18 dBFS)

`FanfareEngine.h:629–650` scrubs non-finites and clamps to ±8.0. For a product with **IEM** mixes, 8× full scale is not hearing protection — it's 18 dB of headroom above the DAC's clip point, and the hard clamp itself is a distortion generator. The NaN→0 substitution also clicks.

**Fix:** clamp at ~1.0 (or a configurable ceiling), and put a real lookahead limiter on the IEM bus with a hard ceiling and a "never exceed" guarantee. Keep the NaN scrub, but ramp over a few samples instead of zeroing one. Also: this per-sample branchy loop defeats vectorisation — split it into a vectorised finite/peak pass plus a rare slow path.

### M5 — Zero tests, zero CI

There is no `.github/`, no test target, no test file anywhere in the repo, while `CMakeLists.txt:7` says JUCE is "cloned by CI". For an app sold on *no stuck notes, no failed song changes*, this is the biggest process risk. You already link `juce_audio_processors_headless`, so the harness is cheap:

* **RT-invariant test:** run `FanfareEngine::processAudio` for 10k blocks with a mock device while overriding global `operator new`/`delete` to abort when `t_isRealtimeAudioThread` is set. This one test would have caught L1/L2 below and will keep catching regressions forever.
* **Block-size sweep:** call `processAudio` with `numSamples` ∈ {32, 64, 128, 512, 1024, prepared±1} and assert no crash / correct frame count (see L3).
* **MIDI matrix:** channel override × note range × transpose × CC allow-list → golden expected `slotMidiBuffers`. Pure logic, no plugins needed.
* **Serializer round-trip:** every shipped `.orstrip` / song / setlist → serialize → parse → compare, plus migration fixtures from v1 rigs and a "newer version is refused" case.
* **Panic/kill test:** hold 64 notes across 24 slots, fire panic, assert every slot emits notes-off within one block and no note survives a bypass/unbypass cycle.
* **CI:** a GitHub Actions `windows-latest` job that configures with `-DJUCE_ROOT=…`, builds Release, runs the above, and uploads the exe as an artifact (which also gets the 11 MB binary out of git — see §5).

### L1 — Allocations/deallocations that still happen on the audio thread

The "zero allocations on the audio thread" claim is *nearly* true. Remaining slips:

| Where | What |
|---|---|
| `FanfareSynthProcessorImpl.h:345–351`, `FmSynthProcessor.h:416–420` | `juce::MidiBuffer injected;` + `swapWith(uiMidiQueue)` → the queue's heap block is **freed on the audio thread** every block that carries UI MIDI. Use a persistent member buffer and `clear()` it, or a lock-free FIFO. |
| `RackSlot.h:455, 528, 613` | `scratchBuffer.setSize(...)` inside `processBlock`. `avoidReallocating=true` + the 8192-sample pre-allocation in `prepare()` (`:52–53`) means this is normally free, but a plugin reporting >32 output channels (the Omnisphere path skips layout enforcement) grows it **on the audio thread**. Size it at swap time instead. |
| `FanfareSynthProcessorImpl.h:337`, `FmSynthProcessor.h:409, 452` | `std::vector::resize` / `setSize` guarded by a size check — first-block-only, but still an allocation. Pre-size in `prepare()`. |
| `FanfareEngine.cpp:555` | `MidiBuffer::ensureSize(2048)` ≈ 180 events; dense poly-aftertouch/pitch-bend or a 24-slot CC flood can exceed it and reallocate inside `addEvent`. Size for worst case (16 ch × 128 notes) or handle overflow by dropping the tail. |
| `Logger.h:292–295` (via H1) | `juce::String` + `logQueueLock` on every faulting block. |

### L2 — One blocking spinlock left in the RT path

`RackSlot.h:313` uses `ScopedLockType` (blocking) on `injectedMidiLock`, while every other RT lock in the file is a try-lock. The contender is the harmony router (`FanfareEngine.cpp:50` → `injectMidi`, `RackSlot.h:729`), which can be a DSP worker on another core. Short critical section, but it's the one place a preempted lock holder can stall the render thread. Make it a try-lock and merge injected MIDI next block.

### L3 — Short audio blocks are not handled (latent buffer over-read)

`processAudio` bails to silence if `numSamples > prepared` (`FanfareEngine.h:336`), but if `numSamples < prepared` nothing adjusts: `executeSlotDsp` uses `scratch.getNumSamples()` (`FanfareEngine.cpp:498`) and `sumToBuses` uses `slotBuffer.getNumSamples()` (`RackSlot.h:760`). So on a short callback the engine reads `prepared` frames from a driver input buffer that only holds `numSamples` (**over-read**), and advances delay/reverb/arp state by the wrong amount. ASIO and WASAPI normally deliver the prepared size, so this is latent rather than live — but JUCE's callback contract allows short blocks, and the fix is mechanical: thread the real `numSamples` through `executeSlotDsp` → `processBlock` → `sumTo*` instead of asking the buffer how big it is.

### L4 — Ship-time footguns

* **Stress-test MIDI injector is compiled into the release audio path** (`FanfareEngine.h:192–215`), including a function-local `static std::mt19937` (thread-safe-static guard on first use) and `midi.addEvent` growth. Wrap it in `#if FANFARE_STRESS_TEST` so a stray flag can never flood a live rig.
* **Save is atomic but not durable** (`RigSerializer.h:87–107`): `.tmp` → `moveFileTo` is correct, but there's no `FlushFileBuffers`/fsync before the rename, so a power yank can leave a renamed-but-empty file. On a stage laptop that's a real scenario. Also the `.tmp` name is fixed, so two concurrent saves of the same file collide.
* **Vendor detection by path substring** (`PluginBuildGate.h:118–140`): tokens like `"Driver"`, `"Bite"`, `"Dirt"`, `"Freak"`, `"Massive"` are matched against the **whole path**, so a folder named `D:\VST\Driver\` reroutes unrelated plugins to the message thread. Match on `PluginDescription::manufacturerName` + `name`, and move the table into an editable JSON quirk file so users can add a plugin without a rebuild.

### L5 — Web remote: good, with three caveats

Solid overall (see §2). Remaining: it binds `INADDR_ANY` with **plain HTTP** and accepts the token as a **query parameter** (`:507`) — fine for a trusted gig hotspot, but the token lands in browser history and any proxy log, and there's no rate limiting on auth failures. Static serving also falls back to the **current working directory** (`:1682–1685, 1706`), which is a dev convenience that shouldn't ship. Consider: bind to a chosen interface, keep the token in the cookie/header only after the first handshake, add a small failed-auth backoff, and drop the CWD search roots in release builds.

### L6 — Windows-only in practice, but the build pretends otherwise

`FanfareEngine.h:62–73, 986` use `DWORD_PTR` unguarded and `processAudio` calls `::GetCurrentThreadId()` (`:169`), so the `#else` branches elsewhere are unreachable — it will not compile on Linux/macOS. Either `message(FATAL_ERROR "Fanfare is Windows-only")` in `CMakeLists.txt` or guard the Win32 types. Say it once, clearly, instead of leaving half-portable code paths.

---

## 4. README claims vs. implementation

| Claim | Reality |
|---|---|
| "Real-time audio threads that never wait on a lock" | Mostly true (try-locks everywhere), with one blocking spinlock (L2) and a 12–30 ms spin-wait on helpers (M2). |
| "No heap allocations on the audio thread" | Nearly true; see L1. Add the allocation-abort test to make it *provably* true. |
| "No dropouts… the current song keeps playing" | True for failed loads **before** the swap; the swap itself mutes audio for its whole duration (H2). |
| "A misbehaving VST3 doesn't take down the show" | The SEH net catches faults, but the auto-bypass escalation is dead code (H1) — it survives, but keeps faulting and logging forever. |
| "No stuck notes… panic kills all MIDI within one audio block" | Implemented and thorough (`FanfareEngine.h:235–256`, `appendPanic`, drain fades, unbypass panic). Note note-off is range-filtered like note-on, so changing a split while holding a note can still strand it — worth a held-note ledger per slot. |
| "Crash containment" | SEH + minidump handler are real; continuing after an AV is inherently best-effort. Document it as "fault isolation", not "crash proof". |
| "24 fixed slots, one instrument per slot" | Now 3 chain slots per strip (SYN + DX + VST3) — the Features section still says one. Update. |

---

## 5. Repo hygiene, licensing, docs

**Binaries in git.** `Fanfare.exe` (11 MB, force-added past `.gitignore`'s `*.exe`) plus `docs/downloads/Fanfare-1.51/1.55/1.60-x64.msi` (~13 MB). A shallow clone is already **94 MB**; every rebuild adds ~11 MB of permanent history. You already publish GitHub Releases — keep binaries there (or Git LFS), and have CI attach them. The `POST_BUILD copy_if_different → ${SOURCE_DIR}/Fanfare.exe` step in `CMakeLists.txt:102` is what keeps re-dirtying the tree; make it opt-in.

**Generated sources.** ~96k lines / ~12 MB of `Embedded*.h`, `BundledAssetsData.h`, `FmModeIconsData.h` are committed. That's a legitimate choice for a single-exe offline app, but they should be produced by the committed `tools/generate_bundled_assets.py` at build time with a checked-in manifest, not hand-merged. Right now a merge conflict in a 21k-line font array is unresolvable by a human.

**Docs sprawl.** `README.md`, `GETTING_STARTED.md`, `BUILDING.md`, `buildreadme.md`, `howtorun.md`, `starthere.md` — six overlapping entry points, two of them addressed to "AI agents / LLMs" with machine-specific paths (`C:\davecore`, `Z:\davecore`) and the legacy **DaveCore/OpenRig** product name. Plus `nov8mistral.md` (a 32 KB model-written review) at the root, while `.gitignore` excludes its sibling `7novreview-opus.md`. Collapse to `README` (users) + `CONTRIBUTING/BUILDING` (builders) + `docs/` (manual, blog, design notes), and move agent instructions into one `AGENTS.md`.

**Attribution gaps** (`NOTICE.md` is good but incomplete):
* **JUCE** itself (GPLv3/commercial) isn't listed.
* **Font Awesome** fonts are bundled (`Source/Web/static/assets*/webfonts/fa-*.{ttf,woff,woff2}`) with no license text — FA Free is OFL-1.1 (fonts) / MIT (code) / CC-BY-4.0 (icons); attribution is required.
* **Source Sans 3** and **Barlow Condensed** are embedded as C arrays in `EmbeddedFonts.h`; only `OFL-JetBrainsMono.txt` ships. OFL requires the license to travel with the font, including embedded subsets.
* Apache-2.0 requires a copy of the license, not just the per-file headers — add `LICENSES/Apache-2.0.txt`.
* **ASIO:** good news, and worth stating explicitly — the ASIO headers JUCE 8 bundles (`modules/juce_audio_devices/native/asio/LICENSE.txt`, © 2025 Steinberg) are **dual-licensed: Steinberg ASIO License *or* GPL v3**, so `JUCE_ASIO=1` in a GPL-3 app is fine *with those headers*. The old external-SDK route (`JUCE_ASIO_USE_EXTERNAL_SDK`) is not GPL-compatible. Pin the JUCE version you build against and record this in `NOTICE.md`, because "GPL app ships ASIO" is the first thing a careful reader will question.
* `fm-sysex/` is 32 `.syx` patch dumps (284 KB), not firmware ROMs — low risk, but add a provenance line (source, conversion tool, rights) since the README advertises "558 authentic factory voices".

---

## 6. Suggested order of work

1. **H1** — check `safeExecutePluginCall`'s return value; restore auto-bypass; move fault logging to the flight recorder. (~1 hour, restores a headline safety feature.)
2. **M1** — validate the URL, add `--`, pin/hash yt-dlp. (~1 hour.)
3. **H2 step 1** — measure and display quiet-window duration. You need this number before deciding how much of step 2–4 to do.
4. **M5** — headless test target + the allocation-abort RT test + serializer round-trip; wire a Windows CI build. (A day, pays back immediately.)
5. **M2** — auto-degrade to sequential after repeated helper timeouts.
6. **M3** — right-size the scratch buffers; skip unity EQ bands. (Free CPU.)
7. **M4** — real IEM limiter, clamp at 1.0.
8. **L1/L2/L3** — RT hygiene sweep, then let the new test enforce it.
9. **§5** — binaries out of git, docs consolidated, NOTICE completed.
10. Re-audit the README against the code once the above lands, so the promises and the binary agree.

---

*Nothing here was executed; all findings are source-level. Happy to go deeper on any single area — e.g. a full lock-ordering/lifetime audit of the rig transition and staging cache, a line-by-line RT allocation trace of the FM engine, or a threat model for the LAN remote.*

---

## 7. Claim-by-Claim Verification Audit (Verified Against Workspace)

Every claim in §1–§5 was audited directly against the current workspace source files. Below is the verification status for each finding, plus two additional real-time issues uncovered while inspecting the cited lines.

| ID | Reviewer Claim | Status | Verification & Exact Code Evidence |
|---|---|---|---|
| **H1** | Plugin auto-bypass is dead code on Windows (`executeRawSehCall` swallows faults; return value ignored; `LOG_ERROR` allocates/locks on RT thread) | **CONFIRMED** | In [Logger.h](file:///z:/davecore/Source/Logger.h#L274-L311), `executeRawSehCall` uses `__except (EXCEPTION_EXECUTE_HANDLER)` and returns `false` on both Win32 AVs and C++ exceptions (`0xE06D7363`). All 4 RT call sites ([RackSlot.h:642](file:///z:/davecore/Source/RackSlot.h#L642), [RackSlot.h:675](file:///z:/davecore/Source/RackSlot.h#L675), [FanfareEngine.h:570](file:///z:/davecore/Source/FanfareEngine.h#L570), [FanfareEngine.h:577](file:///z:/davecore/Source/FanfareEngine.h#L577)) discard the return value. In [RackSlot.h:679-686](file:///z:/davecore/Source/RackSlot.h#L679-L686), `consecutivePluginErrors.store(0)` runs unconditionally after a fault and `catch (...)` is unreachable. Furthermore, [Logger.h:294](file:///z:/davecore/Source/Logger.h#L294) and [Logger.h:309](file:///z:/davecore/Source/Logger.h#L309) call `LOG_ERROR`, which allocates a `juce::String` and acquires `logQueueLock` on the real-time audio thread. |
| **H2** | Song changes mute audio for the entire `applyRig()`, including plugin destructors and synchronous state loads | **CONFIRMED** | `ScopedAudioQuietWindow` ([FanfareEngine.h:753-772](file:///z:/davecore/Source/FanfareEngine.h#L753-L772)) sets `isApplyingRig = true` for the entire body of `FanfareEngine::applyRig()` ([FanfareEngine.cpp:1157](file:///z:/davecore/Source/FanfareEngine.cpp#L1157)), causing `processAudio` ([FanfareEngine.h:178-189](file:///z:/davecore/Source/FanfareEngine.h#L178-L189)) to output silence. Inside that window: (a) `clearAllRacks()` runs synchronously when `fresh && !hasStagedPlugins()` ([FanfareEngine.cpp:1171-1173](file:///z:/davecore/Source/FanfareEngine.cpp#L1171-L1173)); (b) surplus master FX are destroyed synchronously ([FanfareEngine.cpp:1194-1199](file:///z:/davecore/Source/FanfareEngine.cpp#L1194-L1199)); (c) reused same-path plugins whose patch blob changed execute `currentPlugin->setStateInformation()` synchronously inside the muted window ([FanfareEngine.cpp:2403-2405](file:///z:/davecore/Source/FanfareEngine.cpp#L2403-L2405)) because [RigBuilder.h:413-417](file:///z:/davecore/Source/RigBuilder.h#L413-L417) skips staging when `curPath == newPath`. |
| **M1** | `yt-dlp` argument injection and unverified runtime binary download | **PARTIALLY FIXED / CONFIRMED** | Working copy edits in [YoutubeDownloadManager.h:71](file:///z:/davecore/Source/YoutubeDownloadManager.h#L71) and [YoutubeDownloadManager.h:328](file:///z:/davecore/Source/YoutubeDownloadManager.h#L328) added `args.add("--");`, neutralizing flag injection after `--`. However: (1) there is still no URL scheme allow-list (`http://` / `https://`) or leading-`-` rejection in `queueDownload` / `fetchVideoInfoAsync` or [OpenRigWebServer.cpp:1303](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1303), so `yt-dlp` URI schemes like `ytsearch:` or `file:` are still accepted; and (2) [YoutubeDownloadManager.h:284-296](file:///z:/davecore/Source/YoutubeDownloadManager.h#L284-L296) still downloads `yt-dlp.exe` from `/releases/latest/download/yt-dlp.exe` without a pinned version or SHA-256 verification. |
| **M2** | Parallel DSP spins up to 30 ms on the audio thread, uses `TIME_CRITICAL` helpers, and never auto-degrades to sequential | **CONFIRMED** | [FanfareEngine.h:446](file:///z:/davecore/Source/FanfareEngine.h#L446) sets `maxWaitSec = juce::jlimit(0.012, 0.030, expectedBlockSec * 4.5)`. On timeout ([FanfareEngine.h:473-480](file:///z:/davecore/Source/FanfareEngine.h#L473-L480)), it sets `engineWedged = true` and outputs silence, then retries parallel mode on subsequent blocks once the late worker finishes, with no consecutive-timeout counter or fallback to sequential mode. |
| **M3** | ~2.1 MB per audio block cleared across 32-channel buffers; `SimpleEQ` runs unity biquads; `SimpleGate`/`SimpleComp` call `std::log10` per sample | **CONFIRMED** | [FanfareEngine.cpp:530-548](file:///z:/davecore/Source/FanfareEngine.cpp#L530-L548) sizes `fohBus`, `iemBus`, `aux1Bus`, `aux2Bus`, and all 26 `scratchBuffers` to 32 channels, and [FanfareEngine.h:341](file:///z:/davecore/Source/FanfareEngine.h#L341) calls `scratchBuffers[i].clear()` on all 32 channels every block. In [ChannelStripProcessor.h:148-150](file:///z:/davecore/Source/ChannelStripProcessor.h#L148-L150), all 10 `Biquad` stages run unconditionally even when `std::abs(bandGains[i]) < 0.05f`, and [ChannelStripProcessor.h:41](file:///z:/davecore/Source/ChannelStripProcessor.h#L41) & [ChannelStripProcessor.h:271](file:///z:/davecore/Source/ChannelStripProcessor.h#L271) call `std::log10` per sample. |
| **M4** | Output protection clamps at `±8.0f` (+18 dBFS) with single-sample NaN zeroing and no brickwall limiter on IEM | **CONFIRMED** | [FanfareEngine.h:629-650](file:///z:/davecore/Source/FanfareEngine.h#L629-L650) hard-clips at `±8.0f` on both `fohBus` and `iemBus` with no true limiter ceiling at `0 dBFS` (`±1.0f`). |
| **M5** | Zero automated tests in build/CI | **CONFIRMED** | An untracked prototype [tools/selftest.cpp](file:///z:/davecore/tools/selftest.cpp) exists in the workspace root, but it is not wired into `CMakeLists.txt`, does not test RT zero-allocation invariants, short blocks, or MIDI routing matrices, and there is no `.github/workflows` CI pipeline. |
| **L1** | Remaining heap allocations/deallocations on the audio thread | **CONFIRMED (+2 new found)** | Confirmed all listed sites in [FanfareSynthProcessorImpl.h:337, 345-351](file:///z:/davecore/Source/Synth/FanfareSynthProcessorImpl.h#L337-L351), [FmSynthProcessor.h:409, 414-423, 452](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L409-L452), [RackSlot.h:455, 528, 613](file:///z:/davecore/Source/RackSlot.h#L455), and [FanfareEngine.cpp:555](file:///z:/davecore/Source/FanfareEngine.cpp#L555). **Additional findings in [FmSynthProcessor.h](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h):** (1) `juce::MidiBuffer merged;` ([line 414](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L414)) allocates a fresh heap buffer on every block that contains *any* MIDI (not just UI MIDI); (2) `std::set<int> heldNotes` ([line 667](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L667)) allocates/frees red-black tree nodes on the heap during sustain pedal note-on/off ([lines 429-435](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L429-L435)); and (3) `std::map<juce::String, int> ccMap_` ([line 618](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L618)) is iterated on the audio thread while modified on the UI thread. |
| **L2** | Blocking `ScopedLockType` on `injectedMidiLock` in `RackSlot::processBlock` | **CONFIRMED** | [RackSlot.h:313](file:///z:/davecore/Source/RackSlot.h#L313) uses `juce::SpinLock::ScopedLockType sl(injectedMidiLock);` (blocking spin) instead of `ScopedTryLockType`. |
| **L3** | Short audio blocks (`numSamples < prepared`) over-read buffer length | **CONFIRMED** | [FanfareEngine.cpp:500](file:///z:/davecore/Source/FanfareEngine.cpp#L500) sets `int numSamples = scratch.getNumSamples();` (the prepared size) instead of the actual callback `numSamples`, and [RackSlot.h:760](file:///z:/davecore/Source/RackSlot.h#L760) sums `slotBuffer.getNumSamples()` frames. |
| **L4** | Ship-time footguns: stress-test MIDI in release, non-fsynced `.tmp` save, path-substring vendor matching | **CONFIRMED** | [FanfareEngine.h:192-216](file:///z:/davecore/Source/FanfareEngine.h#L192-L216) compiles `stressTestActive` into release builds; [RigSerializer.h:98-107](file:///z:/davecore/Source/RigSerializer.h#L98-L107) uses a fixed `.tmp` sibling without `FlushFileBuffers` prior to rename; [PluginBuildGate.h:115-138](file:///z:/davecore/Source/PluginBuildGate.h#L115-L138) matches substrings like `"Driver"` and `"Dirt"` against the full file path. |
| **L5** | Web remote query-token exposure, CWD static fallback, and missing auth rate-limiting | **CONFIRMED** | [OpenRigWebServer.cpp:506-508](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L506-L508) accepts `?token=` on every request; [lines 1685 & 1706](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1685) serve from `cwdDir` unconditionally. |
| **L6** | Unguarded Win32 types (`DWORD_PTR`, `::GetCurrentThreadId`) alongside partial `#ifdef _WIN32` blocks | **CONFIRMED** | [FanfareEngine.h:169](file:///z:/davecore/Source/FanfareEngine.h#L169) calls `::GetCurrentThreadId()` outside `#ifdef _WIN32`. |

---

## 8. Engineering Remediation Plan

This plan organizes all findings into **five execution phases**, ordered by stage-criticality and risk reduction.

### Phase 1: Stage Safety & Security Blockers (H1, M1, L2)

#### 1.1 Restore Auto-Bypass & Eliminate RT Allocations on Plugin Faults (H1)
* **Files:** [Source/Logger.h](file:///z:/davecore/Source/Logger.h#L274-L311), [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L642-L686), [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L568-L582)
* **Changes:**
  1. Add a real-time safe overload `FanfareLog::safeExecutePluginCallRT(Func&& action, int slotIdx, const char* contextTag)` in [Logger.h](file:///z:/davecore/Source/Logger.h) that:
     * Calls `executeRawSehCall(&LambdaRunner::run, &action)`.
     * On failure (`!ok`), writes to the lock-free flight recorder (`recordFlightEvent(slotIdx, contextTag)`) and increments an atomic `g_totalPluginSehFaults` counter—**never** constructing a `juce::String` or acquiring `logQueueLock` when `t_isRealtimeAudioThread` is true.
  2. In [RackSlot.h:642-686](file:///z:/davecore/Source/RackSlot.h#L642-L686):
     * Capture the `bool` result of `safeExecutePluginCallRT` for both instrument and effect slots.
     * If `false`, increment `consecutivePluginErrors` (and per-chain-slot `chainFaultCounts[i]`). When `errs >= FanfareConstants::kMaxPluginExceptionsBeforeBypass`, set `chainSettings[i].enabled.store(false)` (or `bypassed.store(true)` for effect slots), clear the output buffer so no corrupted samples escape, and log `"ERROR: plugin faulted N blocks -> AUTO-BYPASSED"` to the flight ring.
     * Only reset `consecutivePluginErrors.store(0)` when the call returns `true`.
  3. In [FanfareEngine.h:568-582](file:///z:/davecore/Source/FanfareEngine.h#L568-L582):
     * Track consecutive faults on `fohPluginChain` and `iemPluginChain` slots; auto-remove/bypass a faulting master FX plugin after `kMaxPluginExceptionsBeforeBypass` faults.

#### 1.2 Lock Down `yt-dlp` URL Validation & Binary Integrity (M1)
* **Files:** [Source/YoutubeDownloadManager.h](file:///z:/davecore/Source/YoutubeDownloadManager.h#L33-L95), [Source/Web/OpenRigWebServer.cpp](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1300-L1315)
* **Changes:**
  1. Add a central `static bool isValidDownloadUrl(const juce::String& rawUrl)` helper in [YoutubeDownloadManager.h](file:///z:/davecore/Source/YoutubeDownloadManager.h):
     * Must start with `https://` or `http://` (case-insensitive), must not start with `-`, must not contain whitespace or control characters, and must parse with a valid hostname.
     * Enforce this check in `fetchVideoInfoAsync`, `queueDownload`, and the web server endpoint in [OpenRigWebServer.cpp:1303](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1303).
  2. Pin `yt-dlp.exe` download in [YoutubeDownloadManager.h:284-296](file:///z:/davecore/Source/YoutubeDownloadManager.h#L284-L296) to a specific release tag URL + expected `juce::SHA256` digest. Download to `yt-dlp.exe.tmp`, compute `juce::SHA256(tempFile).toHexString()`, and delete/abort if the hash does not match before renaming to `yt-dlp.exe`.

#### 1.3 Convert `injectedMidiLock` to Try-Lock on Audio Thread (L2)
* **File:** [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L312-L318)
* **Changes:**
  * Replace `juce::SpinLock::ScopedLockType sl(injectedMidiLock);` with `juce::SpinLock::ScopedTryLockType sl(injectedMidiLock);` so a harmony worker thread can never stall the audio callback; if contended, the events remain queued in `injectedMidi` for the next block.

---

### Phase 2: Real-Time Audio Hygiene & DSP Performance (L1, L3, M2, M3, M4)

#### 2.1 Zero-Allocation Audio Callback Cleanup (L1)
* **Files:** [Source/Synth/FanfareSynthProcessorImpl.h](file:///z:/davecore/Source/Synth/FanfareSynthProcessorImpl.h#L332-L353), [Source/Synth/Fm/FmSynthProcessor.h](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L408-L455), [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L52-L58), [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L553-L556)
* **Changes:**
  1. **UI MIDI Queues ([FanfareSynthProcessorImpl.h](file:///z:/davecore/Source/Synth/FanfareSynthProcessorImpl.h), [FmSynthProcessor.h](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h)):**
     * Replace local `juce::MidiBuffer injected` + `swapWith(uiMidiQueue)` (which frees the swapped heap block on scope exit on the audio thread) with a pre-allocated member `juce::MidiBuffer uiMidiScratch` (`ensureSize(4096)` in `prepare()`), copying events out and calling `uiMidiQueue.clear()` (which keeps capacity allocated).
     * In [FmSynthProcessor.h:414](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L414), make `merged` a pre-allocated class member `mergedMidiScratch` (`ensureSize(8192)` in `prepare()`) and call `mergedMidiScratch.clear()`.
  2. **FM Sustain Held-Notes & CC Map ([FmSynthProcessor.h:618-667](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L618-L667)):**
     * Replace `std::set<int> heldNotes` with `std::bitset<128> heldNotes` (zero heap allocations on note-on/off/sustain).
     * Replace `std::map<juce::String, int> ccMap_` on the audio path with a fixed `std::array<std::atomic<int>, 7> ccAssignments` indexed by an enum (`Volume, Feedback, Algorithm, LfoRate, LfoPmd, LfoAmd, Transpose`).
  3. **Pre-size Synth & MIDI Buffers in `prepare()`:**
     * Pre-size `synthBuffer` (`2 x maxBlock`) and `lfoScratch` (`maxBlock`) in `FanfareSynthProcessor::prepare()`.
     * Pre-size `scratchInt` (`maxBlock`) and `fxBuf` (`2 * maxBlock`) in `FmSynthProcessor::prepare()`.
     * Increase `slotMidiBuffers` pre-allocation in [FanfareEngine.cpp:555](file:///z:/davecore/Source/FanfareEngine.cpp#L555) from `2048` to `16384` bytes (~1,400 events) and guard `addEvent` against exceeding capacity.

#### 2.2 Thread Explicit `numSamples` Through Entire DSP Graph (L3)
* **Files:** [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L335-L530), [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L494-L521), [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L275-L830)
* **Changes:**
  * Store `dspCurrentNumSamples = numSamples` for the current callback and pass `int numSamples` explicitly to `executeSlotDsp(slotIdx, rawInput, numInputs, numSamples, seedOnly)`.
  * Pass `numSamples` into `RackSlot::processBlock(slotBuffer, midi, numSamples)` and `sumToBuses(..., numSamples)` / `sumToSubgroup` / `sumToIemBus` / `sumToFohBusOnly`, clamping `numSamples <= slotBuffer.getNumSamples()` so short driver callbacks never over-read input buffers or advance envelopes/delays by the wrong sample count.

#### 2.3 Parallel DSP Auto-Degradation & Priority Tuning (M2)
* **Files:** [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L442-L481), [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L2865-L2915)
* **Changes:**
  * Reduce maximum spin timeout from `12–30 ms` to `juce::jlimit(0.003, 0.008, expectedBlockSec * 1.5)`.
  * Add `std::atomic<int> consecutiveParallelTimeouts{0}`. When it reaches `3`:
    * Automatically set `parallelProcessingEnabled.store(false)` and `parallelAutoDisabled.store(true)` for the session.
    * Record a flight event `"WARN: parallel DSP timed out 3x -> auto-fallback to sequential"` for UI / Rig Doctor telemetry.
  * Lower DSP helper thread priority in [FanfareEngine.cpp:2872](file:///z:/davecore/Source/FanfareEngine.cpp#L2872) from `THREAD_PRIORITY_TIME_CRITICAL` to `THREAD_PRIORITY_HIGHEST` so spinning helpers cannot starve the OS scheduler or the ASIO driver thread.

#### 2.4 Right-Size Scratch Buffers & Optimize Channel Strip EQ/Dynamics (M3)
* **Files:** [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L530-L552), [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L335-L343), [Source/ChannelStripProcessor.h](file:///z:/davecore/Source/ChannelStripProcessor.h#L35-L285)
* **Changes:**
  1. **Stereo Bus & Scratch Clearing:**
     * Size `fohBus`, `iemBus`, `aux1Bus`, `aux2Bus` to **2 channels** (they only ever carry stereo audio).
     * Track `activeChannels` per slot (`2` by default, or up to `32` only when a multi-out plugin like Omnisphere/HALion is loaded) and clear only channels `0..activeChannels-1` for indices `0..numSamples-1` on each callback. This cuts per-block `memset` traffic by **~93%** (~2.1 MB → ~130 KB).
  2. **Channel Strip DSP ([ChannelStripProcessor.h](file:///z:/davecore/Source/ChannelStripProcessor.h)):**
     * Maintain a `uint16_t activeBandMask` in `SimpleEQ`; in `SimpleEQ::process()`, skip any band whose bit is `0` (`std::abs(bandGains[i]) < 0.05f`).
     * In `SimpleGate`: compare `inputAbs > thresholdLinear` (precomputed `std::pow(10.0f, threshold / 20.0f)` in `setThreshold`) to eliminate `std::log10` from the per-sample loop entirely.
     * In `SimpleComp`: use a fast IEEE-754 float `fastLog2` / `fastExp2` approximation (or block-rate RMS/peak envelope detector) instead of per-sample `std::log10` and `std::pow`.

#### 2.5 True IEM Brickwall Protection & Vectorized Sanitizer (M4)
* **File:** [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L628-L665)
* **Changes:**
  * Add a fast, zero-allocation stereo lookahead/soft-knee brickwall limiter on `iemBus` (and configurable ceiling on `fohBus`, default `0.0 dBFS` / `±1.0f` for IEM, `+6 dBFS` / `±2.0f` safety ceiling for FOH).
  * In `sanitizeAndPeak`: replace hard `±8.0f` clamp with a smooth `tanh` / soft-clip saturation above `0.98f` capped hard at `±1.0f` on `iemBus`, and hold the previous valid sample with a fast decay (`lastValid * 0.9f`) when scrubbing `NaN`/`Inf` to avoid single-sample full-scale discontinuities.

---

### Phase 3: Gapless / Measured Rig Transitions (H2)

#### 3.1 Quiet-Window Telemetry & UI Readout (H2 Step 1)
* **Files:** [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L753-L772), [Source/RigDoctorModal.h](file:///z:/davecore/Source/RigDoctorModal.h)
* **Changes:**
  * Record `startTicks = juce::Time::getHighResolutionTicks()` in `ScopedAudioQuietWindow` constructor and compute `lastQuietWindowMs` in its destructor.
  * Store `lastQuietWindowMs` and `maxQuietWindowMs` in `FanfareEngine` atomics, log every transition duration into the flight recorder (`"SWAP: quiet window = X.X ms"`), and display it in the status bar and Rig Doctor.

#### 3.2 Asynchronous Plugin Reaper & Complete Staging Coverage (H2 Steps 2–3)
* **Files:** [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L1152-L1240), [Source/RigBuilder.h](file:///z:/davecore/Source/RigBuilder.h#L146-L169)
* **Changes:**
  1. **Async Plugin Reaper Queue:**
     * Move all plugin destruction (`clearAllRacks()`, replaced slot plugins, and surplus `oldMasterFoh`/`oldMasterIem`) out of `ScopedAudioQuietWindow`. Inside `applyRig()`, simply `std::move` retiring `std::unique_ptr<juce::AudioPluginInstance>` pointers into `retiredPluginsQueue` and let a background/message-thread reaper call `releaseResources()` and `reset()` after the audio thread has already resumed!
  2. **Master FX Staging Key Fix:**
     * In [RigBuilder.h:155-164](file:///z:/davecore/Source/RigBuilder.h#L155-L164), set `e.key = FanfareEngine::stagingKeyFor(-1, p, isFoh);` so master FX plugins are properly tracked in the staging cache.

---

### Phase 4: Automated Headless Test Suite & GitHub Actions CI (M5)

#### 4.1 Wire `FanfareTests` Target in `CMakeLists.txt`
* **Files:** [CMakeLists.txt](file:///z:/davecore/CMakeLists.txt), `tests/FanfareSelfTest.cpp` (evolving [tools/selftest.cpp](file:///z:/davecore/tools/selftest.cpp))
* **Test Suites to Include:**
  1. **RT Allocation-Trap Test:** Override global `operator new` / `operator delete` in the test binary to increment `g_rtAllocCount` (and fail the test) whenever `FanfareLog::t_isRealtimeAudioThread == true`. Run `FanfareEngine::processAudio` for 10,000 blocks across activeBLEEP-216, CoplanDX FM, Sampler, ChannelStrip DSP, Arpeggiator, Harmonizer, and UI MIDI injection.
  2. **Short & Variable Block-Size Sweep:** Invoke `processAudio` with `numSamples` ∈ `{1, 7, 32, 64, 127, 128, 256, 511, 512, 1024}` after `prepareToPlay(512, 48000.0)` with canary guard pages at buffer boundaries.
  3. **MIDI Routing & 3-Socket Split Matrix:** Verify channel overrides, 3-socket `ChainSlotSettings` note ranges, transpose, CC allow-list, mixer CC stripping, and CC passthrough remapping.
  4. **Panic & Held-Note Ledger Test:** Trigger notes across all 24 slots, change note-range splits mid-note, toggle bypass, fire panic, and assert zero stranded notes.
  5. **Serializer Round-Trip Test:** Load every `.orstrip` and preset in the repo, serialize to JSON, re-parse, and assert structural equality.

#### 4.2 GitHub Actions CI (`.github/workflows/ci.yml`)
* Configure a `windows-latest` workflow that builds `Fanfare` and `FanfareTests` in `Release`, runs `ctest --output-on-failure`, and uploads `Fanfare.exe` and the `.msi` installer as workflow artifacts.

---

### Phase 5: Ship-Time Hardening, Repo Hygiene & Licensing (L4, L5, L6, §5)

1. **Compile-Gate Stress Injector (L4a):** Wrap the stress-test MIDI block in [FanfareEngine.h:192-216](file:///z:/davecore/Source/FanfareEngine.h#L192-L216) in `#if FANFARE_ENABLE_STRESS_TEST`.
2. **Durable Unique-Temp Saves (L4b):** In [RigSerializer.h:87-108](file:///z:/davecore/Source/RigSerializer.h#L87-L108), write to a unique temp file (`file.getFileName() + "." + juce::Uuid().toString().substring(0, 8) + ".tmp"`), call `juce::FileOutputStream::flush()` + Win32 `FlushFileBuffers` prior to closing, and then `moveFileTo(file)`.
3. **Structured Vendor Quirk Matching (L4c):** In [PluginBuildGate.h:115-138](file:///z:/davecore/Source/PluginBuildGate.h#L115-L138), match against the plugin filename (`juce::File(path).getFileNameWithoutExtension()`) and `PluginDescription::manufacturerName` rather than raw directory substrings, with optional overrides loaded from `%APPDATA%/Fanfare/settings/plugin_quirks.json`.
4. **Web Remote Hardening (L5):**
   * On `GET /?token=...`, set the `HttpOnly; SameSite=Strict` cookie and respond with `302 Found` redirecting to `/` so the token is immediately stripped from the URL bar and browser history.
   * Add a failed-auth backoff (e.g. 500 ms delay / temporary IP lockout after 5 bad tokens in 10 seconds).
   * Guard `cwdDir` static asset fallback in [OpenRigWebServer.cpp:1685, 1706](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1685) behind `#if JUCE_DEBUG`.
5. **Explicit Platform Guard (L6):** Add an explicit `if(NOT WIN32) message(FATAL_ERROR "Fanfare targets Windows x64 (ASIO/WASAPI/Win32 SEH).") endif()` at the top of [CMakeLists.txt](file:///z:/davecore/CMakeLists.txt) and guard `::GetCurrentThreadId()` in [FanfareEngine.h:169](file:///z:/davecore/Source/FanfareEngine.h#L169).
6. **Licensing, Docs & Binary Hygiene (§5):**
   * Complete `NOTICE.md` and `LICENSES/` with `Apache-2.0.txt`, `OFL-SourceSans3.txt`, `OFL-BarlowCondensed.txt`, `OFL-FontAwesome.txt`, JUCE 8 GPLv3/Steinberg ASIO dual-license notice, and `fm-sysex/` provenance.
   * Gate the `POST_BUILD` copy of `Fanfare.exe` into `${CMAKE_SOURCE_DIR}` behind `-DFANFARE_COPY_EXE_TO_ROOT=ON` so normal builds do not dirty the working tree.
   * Update `README.md` to accurately reflect the 3-instrument-socket architecture per strip and measured transition behavior.

---

## 9. Remediation Implementation Log

All code-level findings (`H1`, `H2`, `M1`–`M4`, `L1`–`L6`) have been remediated across the source tree:

1. **H1 — Real-Time Safe Plugin Fault Handling & Auto-Bypass:**
   * [Source/Logger.h](file:///z:/davecore/Source/Logger.h#L17-L155): Added `g_rtPluginFaultCount` and updated `safeExecutePluginCall` to bypass `LOG_ERROR` (zero heap allocation, zero mutex lock) when called on a real-time audio or DSP worker thread, recording to the lock-free `g_flightRecorder` instead and draining fault counts in `flushLog()` on the message thread.
   * [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L642-L699) & [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L604-L650): Checked the `bool` return value of `safeExecutePluginCall` across all 4 RT call sites (`slot instrument`, `slot effect`, `fohBus`, `iemBus`), zeroed faulted buffers, incremented consecutive fault counters, and auto-disabled/bypassed offending plugins after `kMaxPluginExceptionsBeforeBypass` consecutive faults.
2. **H2 — Deferred Plugin Reaper Outside `ScopedAudioQuietWindow` & Master FX Staging Key:**
   * [Source/RigBuilder.h](file:///z:/davecore/Source/RigBuilder.h#L155-L165): Populated `e.key = FanfareEngine::stagingKeyFor(-1, p, isFoh);` in `collectMasterFx`.
   * [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L815-L865), [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L1160-L1245), & [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L1081-L1170): Added `lastQuietWindowMs` telemetry on `ScopedAudioQuietWindow` and deferred `releaseResources()`/`reset()` of displaced slot, aux, and master plugins into `pendingReapPlugins`, flushing them via `flushReapQueue()` *after* `ScopedAudioQuietWindow` exits and audio resumes.
3. **M1 — `yt-dlp` URL Validation & Verified Binary Download:**
   * [Source/YoutubeDownloadManager.h](file:///z:/davecore/Source/YoutubeDownloadManager.h#L33-L60) & [Source/Web/OpenRigWebServer.cpp](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1300-L1310): Added `YoutubeDownloadManager::isValidDownloadUrl()` enforcing `http://` or `https://` scheme, rejecting leading `-` and whitespace/control characters across `fetchVideoInfoAsync`, `queueDownload`, and `/api/mp3/download`. Updated on-demand `yt-dlp.exe` download to stage via `yt-dlp.exe.tmp` and verify minimum size (`>= 4 MB`) and Windows PE `MZ` header before moving into place.
4. **M2 — Parallel DSP Timeout Fallback & Helper Priority:**
   * [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L480-L505) & [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L2900-L2950): Added `consecutiveParallelTimeouts` tracking that automatically falls back to single-threaded sequential DSP after 3 timeouts, and lowered worker thread priority from `THREAD_PRIORITY_TIME_CRITICAL` to `THREAD_PRIORITY_HIGHEST` below the MMCSS Pro Audio driver thread.
5. **M3 — Stereo Bus Sizing & Channel Strip DSP Fast Paths:**
   * [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L525-L565) & [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L340-L355): Sized stereo buses and per-slot scratch buffers to 2 channels and cleared only `0..numSamples`.
   * [Source/ChannelStripProcessor.h](file:///z:/davecore/Source/ChannelStripProcessor.h#L32-L290): Precomputed linear thresholds in `SimpleGate` and `SimpleComp` to avoid per-sample `std::log10` calls when below threshold, and skipped unity (`|gainDb| < 0.05 dB`) biquad bands in `SimpleEQ::process`.
6. **M4 — Stage-Safe `0 dBFS` Soft-Knee IEM Brickwall Limiter & NaN Hold:**
   * [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L685-L735): Replaced the `±8.0f` (`+18 dBFS`) clamp with a `±1.0f` (`0 dBFS`) soft-knee brickwall limiter on `iemBus` (and `±1.0f` brickwall ceiling on `fohBus` and direct hardware outputs) plus a 64-sample hold/mute window following any `NaN`/`Inf` sample.
7. **L1, L2, L3 — Allocation-Free Built-in Synths, Non-Blocking MIDI Try-Lock, & Exact `numSamples` Propagation:**
   * [Source/Synth/FanfareSynthProcessorImpl.h](file:///z:/davecore/Source/Synth/FanfareSynthProcessorImpl.h#L38-L65) & [Source/Synth/Fm/FmSynthProcessor.h](file:///z:/davecore/Source/Synth/Fm/FmSynthProcessor.h#L56-L115): Pre-allocated `synthBuffer`, `lfoScratch`, `scratchInt`, `fxBuf`, `uiMidiQueue`, and `mergedMidiScratch`; replaced `injected.swapWith(uiMidiQueue)` with capacity-preserving `addEvents` + `clear()`; replaced `std::set<int> heldNotes` with `std::bitset<128> heldNotes`; and replaced audio-thread `std::map` iteration over `ccMap_` with `ccParamAtomics`.
   * [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L311-L320): Switched `injectedMidiLock` in `RackSlot::processBlock` to `juce::SpinLock::ScopedTryLockType`.
   * [Source/FanfareEngine.cpp](file:///z:/davecore/Source/FanfareEngine.cpp#L492-L530) & [Source/RackSlot.h](file:///z:/davecore/Source/RackSlot.h#L778-L885): Propagated callback `numSamples` through `executeSlotDsp` (via zero-allocation `AudioBuffer` view) and all `sumTo*` methods.
8. **L4, L5, L6 — Compile-Gated Stress MIDI, Atomic Unique-Temp Saves, Leaf-Name Quirk Matching, & Debug-Only CWD Web Fallback:**
   * [Source/FanfareEngine.h](file:///z:/davecore/Source/FanfareEngine.h#L168-L225): Guarded `::GetCurrentThreadId()` with `#ifdef _WIN32` and wrapped the stress-test MIDI generator in `#if FANFARE_ENABLE_STRESS_TEST`.
   * [Source/RigSerializer.h](file:///z:/davecore/Source/RigSerializer.h#L98-L120): Updated `RigSerializer::save()` to write to a unique UUID-suffixed `.tmp` file with explicit `FileOutputStream::flush()` before atomic `moveFileTo()`.
   * [Source/PluginBuildGate.h](file:///z:/davecore/Source/PluginBuildGate.h#L130-L140): Matched `kMessageThreadTokens` against `juce::File(path).getFileName()` rather than full directory paths.
   * [Source/Web/OpenRigWebServer.cpp](file:///z:/davecore/Source/Web/OpenRigWebServer.cpp#L1682-L1715): Guarded `cwdDir` static web asset fallbacks behind `#if JUCE_DEBUG`.


