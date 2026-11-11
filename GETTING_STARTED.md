# Getting Started with Fanfare (Windows VST3 Host)

Welcome to **Fanfare**! Fanfare is a standalone live performance VST3 host built specifically for Windows 10/11 x64. This guide walks you through your first 5 minutes with Fanfare, from downloading the standalone `.exe` to running your first live rig, hosting VST3 instruments, and tuning your Windows laptop for dropout-free stage performance.

---

## 1. Download & Launch

1. Go to the [Fanfare Releases Page](https://github.com/openrig-host/fanfare/releases/latest).
2. Download `Fanfare.exe`.
3. **No installation is required.** Place `Fanfare.exe` in any local folder on your Windows 10/11 PC and double-click to launch.
4. **Stage Power Rule:** Always run your laptop plugged into its **AC power supply** during rehearsals and gigs. On battery power, Windows and laptop firmware aggressively downclock the CPU, sleep USB ports, and generate battery-polling interrupts that cause audio clicks.

---

## 2. Initial Setup (Audio & MIDI)

When Fanfare launches, the **Welcome Screen** gives you quick actions for creating or opening songs and scanning VST3s (reopen it anytime via **`Help → Welcome screen...`**). To configure your hardware, open **`Rig → Audio & MIDI settings...`** from the top menu bar (or click the **Settings** gear icon at the bottom of the left rail):

1. **Audio Settings (ASIO Recommended)**:
   - Select **ASIO** as your audio device type.
   - Choose your audio interface driver (for example, *Yamaha Steinberg USB ASIO*, *UMC ASIO Driver*, *Focusrite USB ASIO*, etc.).
   - Set your sample rate (`44.1 kHz` or `48 kHz`) and buffer size (`128` to `256` samples for low-latency live play).
   - *(Fallback)*: If you do not have an ASIO driver installed, select **Windows Audio (WASAPI)**.

2. **MIDI Input Device**:
   - Enable your keyboard controller(s) in the same Audio & MIDI Settings dialog (for example, *Yamaha CK88*, *Roland RD-88*, *Impact LX61+*, *KeyLab Essential 88*).
   - **Live USB Hotplug & Auto-Recovery (New in v1.3):** When your bass player accidentally yanks your USB cable out of the computer, just put it back in, count to three, go back to playing, and smile at your bass player. Possibly the single most important update in Fanfare history! Fanfare automatically silences any held notes on disconnect, shows `LAT: RECONNECT...` in the top bar, and brings both your ASIO audio driver and USB MIDI inputs right back online within a couple of seconds of plugging the cable back in.

---

## 3. Engine & CPU Tuning (New in v1.2)

Click the **Settings** gear icon and open **ENGINE & CPU TUNING** to inspect or tailor how Fanfare schedules real-time audio on your computer:

- **Hybrid CPU Core Mode (P-Cores Only, Default ON):** Locks real-time ASIO and DSP threads onto fast Performance cores while steering UI rendering and background setlist preloading onto Efficiency cores. Turn this to *Use All Cores* if you run an older PC or AMD Ryzen laptop where all cores share the same speed and you want extra polyphony headroom.
- **Shelter CPU Core 0 (Default ON):** Keeps real-time audio off Core 0, where Windows routes Wi-Fi, USB hub, and battery driver interrupts.
- **Block Windows 11 EcoQoS Power Throttling (Default ON):** Prevents Windows 11 from clocking down Fanfare when VST editor windows are in front or the main window is minimized.
- **DSP Multiprocessing Pipeline (Parallel Work-Sharing Default):** Splits heavy multi-synth rigs across physical Performance cores alongside the ASIO thread using a lock-free queue. You can switch to *Sequential Inline* anytime for single-synth rigs or troubleshooting.

---

## 4. Populating Your VST3 Plugins

Fanfare scans standard Windows VST3 directories (`C:\Program Files\Common Files\VST3`) to locate your installed instruments and effects:

1. Open **`Rig → Scan for plugins`** from the menu bar (or click **Scan VST3 plug-ins...** on the Startup Welcome screen).

---

## 5. Build Your First Rig

### Step A: Load a VST3 Instrument or Open the Built-In 1984 Keyboard Rig (`BLEEP-216` + `CoplanDX`)
1. Click **`+`** on any empty channel strip slot (`1`, `2`, or `3`) to pick a VST3 instrument from your list (for example, *Hammond B-3X*, *Kontakt 8*, *ZENOLOGY*, *EZkeys 2*, *Jun-6 V*), and click **`E`** to open its floating editor window.
2. **Or use the built-in 1984 Keyboard Rig with zero VSTs:** Every channel strip has 3 chain slots (`1`, `2`, `3`) that can host:
   - **BLEEP-216 Polysynth (`SYN`)**: 16-voice Juno/Jupiter-inspired analog polysynth with **68 stage-ready presets** (*Jump 1984 Brass*, *Take On Me Riff*, *Final Countdown*, *Enyaesque*, *1983 Poly*, *Dreamscape Pad*, *Juno Pad 1*, *Hollow Square Pad 1*, *Cathedral Pad*, *Square Lead*, *Dark Bass*, and more).
   - **CoplanDX FM Synth (`DX`)**: 16-voice 6-op / 4-op FM synthesizer (`DX7`, `DX100`, `TX81Z`, `2x4 OP`, `2x6 OP`) with **558 categorized vintage presets** (*E.PIANO 1*, *BASS 1*, *MARIMBA*, *TUB BELLS*, *BELL-FLUTE*, *LatelyBass*, *GreatVibes*, *SpaceChime*, *GLOKENSPL*, *Xylosnare*) plus `.syx` import.
   - **Hybrid FM + Analog Strips (`Strips` sidebar):** Open the left **Strips** sidebar to load any of the **11 included dual-engine `.orstrip` combos** (*01 FM Bell & Flute Pad* through *10 Space Chime & Nostalgia Flute*, plus *flute+fmxylo*) that stack a percussive FM mallet in Slot 1 with a lush analog synth pad in Slot 2.

### Step B: Set Key Split / Note Range
1. Click **`...`** at the bottom of the channel strip (or click the note range readout under the strip name) and choose **`Note range & transpose...`**.
2. Strike the lowest key and highest key on your controller to define the split zone, then click **Done**.

### Step C: Map Hardware Knobs & Controller Profile (**`CC`**)
1. Click **`CC`** at the bottom of the channel strip (or long-press **Mute**, **FOH**, or **IEM** directly on the mixer).
2. Click **`Edit...`** (or **`Rig → Controller profile...`**) once to define your keyboard's physical CCs, then click **`Profile`** on any strip to apply your standard controller passthroughs in one click — or click **`Quick`** / **`Learn`** and move any VST control + hardware knob to bind them directly.

---

## 6. Saving, Setlists, Version History & Stage Telemetry

- **Save Song & Version History:** Press **`Ctrl+S`** (`File → Save song as...`) to save your Song, or click **`... → Save strip preset...`** to save a reusable `.orstrip` brick. Every save writes atomically and records a timestamped snapshot — right-click any Song, Set, or Strip in the left Library Sidebar and choose **`Version history...`** to restore any earlier version.
- **Rig Doctor (Setup Diagnostics & Repair):** If a saved `.json` song ever fails to load or hangs after a third-party VST3 update, right-click the song in the **Songs** sidebar and choose **`Diagnose & repair (Rig Doctor)...`** (or open **`Tools → Rig doctor...`**). Click **`TEST ALL PLUGINS`** to dry-run every plugin in isolation, inspect `.bak` diffs and log clues, and set any broken plugin to **`RESET`** (clear corrupt patch state), **`OFF`**, or **`REMOVE`** without losing your faders, splits, or scenes.
- **Vertical Ribbon Sidebar, Next-Up Capsule & `1–9` Scene Hotkeys:** Use the left icon rail to switch between **Songs**, **Strips**, **Setlist**, **Queue** (10 instant stage tiles), and a live **MIDI Monitor**. While you play the active song, Fanfare preloads the next song on non-audio cores and lights up the clickable **`Next-Up`** capsule in the top bar; press number keys **`1`–`9`** anytime to recall Scenes instantly.
- **Panic Button:** Click the red **`Panic`** button at the top-right (or press `P`) to send All Notes Off and Sustain Off across every slot in the very next audio block.
- **Telemetry Footer:** The bottom bar displays your live audio device (`SR / buffer`), Song status, color-thresholded `CPU` and `LAT`, `RAM`, and `XRUNS` in crisp monospace type. Click `CPU` / `RAM` to open the Resource Inspector, click the audio device readout to open Audio Settings, or click `XRUNS:` anytime to reset the counter to zero and open `audio_underrun_log.txt`.

---

## Next Steps

- Click **DYN** on any strip for the built-in channel strip (Gate, EQ, Compressor, Chorus, Convolution Reverb, and Musette detuner).
- Explore the **8-Pad Sampler**, **Arpeggiator / Harmonizer**, **Dual-Deck Break Music Player**, and **Gig Notepad**.
- See [BUILDING.md](BUILDING.md) if you want to compile Fanfare from source or customize the engine.
