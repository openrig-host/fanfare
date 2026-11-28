# Fanfare Windows-to-macOS Port Plan (Apple Silicon Mac Mini)

**Target Machine (Verified):** Apple M4 (`arm64`), macOS 26.3.1 (Darwin 25.3.0)  
**Source Baseline:** Fanfare `v1.7.40` (`CMakeLists.txt` + JUCE 8 / C++17, commit `2e0b4a0` on `trunk`)  
**Status:** Phases 0, 1, and 2 (code + Universal build + CTest) Complete — Rescued Locally

### Locked-In Decisions
1. **Plugin Formats:** **VST3 + Audio Unit (AU)** both enabled on macOS (VST3 preserves cross-platform rig state; AU adds Mac-native plugins and Rosetta out-of-process bridging for Intel-only AUs like `Super 8.component`).
2. **CPU Architecture:** **Universal (`arm64` + `x86_64`)** binary (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for Release/distributable builds; optional single-arch `arm64` toggle for fast Debug iteration).
3. **Windows → macOS Rig Portability:** **Safe, non-destructive automatic plugin remapping** at load time with explicit logging/UI visibility (never silently mutates saved `.json` files on disk, never feeds VST3 state blobs into AU plugins).
4. **Full Scope (Phases 0–5):** Local Mac Mini bring-up, full Apple Silicon tuning & feature parity, plus Universal signed/notarized `.dmg` packaging and macOS CI.

---

## 1. Executive Summary

Fanfare is well-positioned for a native macOS port. Roughly 90% of the codebase—the JUCE 8 UI, channel strips, built-in B3 Organ, MSFA/Dexed FM synth, Fanfare Synth, Sampler, MP3 Break Player, Setlist/Rig serializer, and HTTP/WebSocket Stage HUD server—is already standard C++17 + JUCE and already includes an initial `if(APPLE)` block in `CMakeLists.txt`.

A full source audit uncovered **11 concrete compile/link blockers** (including unguarded Win32 `DWORD_PTR` types, mismatched `#ifdef _WIN32` method declarations, a Projucer `AppConfig.h` clash, missing `SQLite3` linkage, a macOS 15+ SDK obsolete API in `PluginMirrorCapture.cpp`, an ARM intrinsic mismatch, and a macOS main-thread AppKit bug in `selftest.cpp`) plus **critical macOS runtime differences** (most notably that `.vst3` plugins are **bundle directories** on macOS rather than regular files, and `SIGPIPE` will immediately kill the app when a tablet disconnects from the WebServer).

---

## 2. Verified Environment & Plugin Inventory (This Mac Mini)

### Hardware & Toolchain

| Component | Status on Mac Mini |
|---|---|
| CPU / OS | Apple M4 (`arm64`), macOS 26.3.1 |
| Compiler | Apple Clang 17.0.0 (`/Library/Developer/CommandLineTools`) |
| CMake | `4.3.0` (`/opt/homebrew/bin/cmake`) |
| Ninja | `1.13.2` (`/opt/homebrew/bin/ninja`) |
| JUCE 8 | Pinned at tag `8.0.12` at `/Users/davidwaugh/JUCE` (local SSD) |
| SQLite3 | Ships in macOS SDK (`libsqlite3.tbd`), linked via `SQLite3::SQLite3` |
| FFmpeg | `/opt/homebrew/bin/ffmpeg` |
| Python 3 | `/opt/homebrew/bin/python3` |

### Installed Audio Plugins on This Mac Mini (Verified via `lipo -info`)

| Plugin | VST3 (`/Library/Audio/Plug-Ins/VST3`) | AU (`/Library/Audio/Plug-Ins/Components`) | Architectures |
|---|---|---|---|
| **Kontakt 8** | Installed | Installed | `x86_64` + `arm64` (Universal) |
| **Replika XT** | Installed | Installed | `x86_64` + `arm64` (Universal) |
| **Supercharger GT** | Installed | Installed | `x86_64` + `arm64` (Universal) |
| **Super 8** | Installed | Installed | `x86_64` only (Intel) |

---

## 3. Phase 0 — Mac Mini Toolchain & Workspace Prep (DONE)
- [x] Clone JUCE `8.0.12` to `~/JUCE`: `git clone --depth 1 --branch 8.0.12 https://github.com/juce-framework/JUCE.git ~/JUCE`
- [x] Install `ninja` via Homebrew: `brew install ninja`
- [x] Add `build-mac/` to `.gitignore`.

## 4. Phase 1 — Clean Compile, Link & Headless `selftest` Pass (DONE)
- [x] `CMakeLists.txt`: `FANFARE_MAC_UNIVERSAL`, `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`, macOS privacy `Info.plist` keys, `juce_generate_juce_header(...)`, `DONT_SET_USING_JUCE_NAMESPACE=1`, `SQLite3::SQLite3`.
- [x] Fixed all C++ compile/runtime blockers (`DWORD_PTR` → `uintptr_t`, `startUsbDeviceWatcher` guard, ARM `yield`/`x86_64` `pause`, `OSAtomic` → `<atomic>`, `PluginMirrorCapture.cpp` macOS 15+ SDK guard, `Main.cpp` `juce_CreateApplication`, `SIGPIPE` + `SO_NOSIGPIPE`, `PluginBuildGate.h` leaf extraction).
- [x] Fixed `tools/selftest.cpp` macOS AppKit thread-0 `initialiseNSApplication()` + `runDispatchLoop()`.
- [x] Verified Universal (`arm64;x86_64`) build + `lipo -info` + 3/3 `ctest` pass.

## 5. Phase 2 — CoreAudio, CoreMIDI & Native Plugin Hosting (CODE DONE, LIVE TEST PENDING)
- [x] **Phase 2.1:** CoreAudio default device type & `~/Library/Application Support/Fanfare` path layout.
- [x] **Phase 2.2:** macOS `.vst3` bundle-directory scanning + `FanfareConstants::pluginExistsOnDisk()` across `FanfareEngine`, `RigBuilder`, `RigDoctorModal`, and `MissingPluginsToast`.
- [x] **Phase 2.3:** Audio Unit (`AudioUnitPluginFormat`) registration, enumeration, and format routing.
- [x] **Phase 2.4:** Safe, non-destructive Windows → macOS VST3 path remapping (`resolveWindowsVst3PathFromList` / `resolvePluginPathForCurrentPlatform`) + unit tests.
- [x] **Phase 2.5:** CoreMIDI `MidiDeviceListConnection` USB hotplug listener.
- [ ] **Phase 2.6:** Interactive Cocoa editor & live third-party plugin verification (`Kontakt 8`, `Replika XT`, `Supercharger GT`, `Super 8` AU).

## 6. Phase 3 — Live-Stage Reliability, Apple Silicon Tuning & Telemetry (PENDING)
- App Nap / sleep prevention (`IOPMAssertionCreateWithName` + `NSProcessInfo` latency-critical activity).
- Apple Silicon parallel DSP worker pool (`dispatch_semaphore_t` + `THREAD_TIME_CONSTRAINT_POLICY` + `os_workgroup_t`).
- `CpuTopology.h` (`hw.perflevel0.physicalcpu` / `hw.perflevel1.physicalcpu`) & `EngineTuningModal.h` macOS copy.
- `Logger.h` `getMemoryStats()` (`TASK_VM_INFO` + `hw.memsize` + `host_statistics64`) and POSIX signal crash/Flight Recorder dump.
- `ThermalMonitor.h` (`NSProcessInfo.processInfo.thermalState`).

## 7. Phase 4 — Feature Parity & macOS UX Polish (PENDING)
- Live Plugin Mirror (`ScreenCaptureKit` + `NSWindow` / `CGEventPostToPid` in `Source/Web/PluginMirrorMac.mm`).
- Stage Hotspot modal macOS LAN / `.local` Bonjour guidance + System Settings Internet Sharing helper.
- `YoutubeDownloadManager.h` macOS `yt-dlp` / `ffmpeg` discovery (`/opt/homebrew/bin`, Mach-O magic validation, `chmod +x`).
- macOS `Cmd` shortcuts & copy audit (`ASIO` → `CoreAudio`, `Explorer` → `Finder`).

## 8. Phase 5 — Packaging, Signing & Optional CI/Distribution (PENDING)
- `installer/macos/entitlements.plist` (`disable-library-validation`, `allow-jit`, `allow-unsigned-executable-memory`, `device.audio-input`).
- `tools/build_macos_dmg.sh` (Universal Release `Fanfare.app` → codesign → `.dmg` → optional `notarytool` + `stapler`).
- macOS GitHub Actions CI job (`macos-15` runner).
