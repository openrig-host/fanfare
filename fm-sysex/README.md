# Vintage Yamaha FM Factory SysEx Archive (`DX7`, `DX100`, `TX81Z`)

> **Using these in Fanfare:** CoplanDX's **SYX** button in the editor lists
> every `0x09` bank in this folder automatically (it looks for `fm-sysex/`
> next to `Fanfare.exe`, in the working directory, or in
> `%APPDATA%/Fanfare/fm-sysex/`). That covers all of `dx7/` and all of
> `dx7-converted/` — 256 DX7 ROM voices plus all 192 DX100 and 128 TX81Z
> factory voices as 6-op DX7 cartridges. The native 4-op `0x04` dumps in
> `dx100/` and `tx81z/` are kept for reference; CoplanDX points you at their
> `dx7-converted/` twins when you pick them.

Verified Yamaha System Exclusive (`.syx`) factory ROM patch banks for the **DX7**, **DX100**, and **TX81Z**. Every file is `4,104` bytes and verified against the 7-bit two's-complement Yamaha SysEx checksum (`(-sum) & 0x7F`).

Because Fanfare's `FanfareFm` engine (`Source/Synth/Fm/msfa`) uses the 6-operator DX7 MSFA synthesis core, this archive includes **both**:
1. **Native Hardware SysEx Dumps** (`dx7/`, `dx100/`, `tx81z/`) in their exact original Yamaha formats (`0x09` for 6-op DX7, `0x04` for 4-op DX100 and TX81Z).
2. **DX7 6-Operator VMEM Conversions** (`dx7-converted/`) generated with Martin Tarenskeen's `DXconvert 3.2.4`, translating all 192 DX100 voices and 128 TX81Z voices into standard `0x09` 6-op DX7 32-voice cartridges (using the spare 5th and 6th DX7 operators to approximate TX81Z alternate waveforms `W2`–`W8`).

---

## Directory Layout

### 1. `dx7/` — Yamaha DX7 Factory ROMs (256 Voices, Native 6-Op VMEM `0x09`)
Header: `F0 43 00 09 20 00` + `32 * 128` bytes + checksum + `F7` (`4,104` bytes each).

| File | Cartridge / Group | Notable Factory Patches |
|---|---|---|
| `ROM1A.SYX` | ROM 1A — Master Group (JP/EU Preload) | `BRASS 1`, `STRINGS 1`, `E.PIANO 1`, `BASS 1`, `MARIMBA`, `KOTO`, `TUB BELLS` |
| `ROM1B.SYX` | ROM 1B — Keyboard & Plucked Group | `PIANO 4`, `E.PIANO 2..4`, `CLAV 2..3`, `E.ORGAN 2..5`, `SITAR`, `BASS 3..4` |
| `ROM2A.SYX` | ROM 2A — Orchestral & Percussive Group | `PICCOLO`, `OBOE`, `SAX BC`, `STRINGS 4..8`, `BRASS 4..8`, `GLOKENSPL` |
| `ROM2B.SYX` | ROM 2B — Complex & Effects Group | `SYN-LEAD 2..5`, `SYNBRASS 1..2`, `SYN-BASS 1..2`, `SHIMMER`, `WATER GDN` |
| `ROM3A.SYX` | ROM 3A — Master Group (US Preload) | `FLUTE 1`, `BRIGHT BOW`, `BRASSHORNS`, `E.PIANO 1`, `FRETLESS 1`, `HEAVYMETAL` |
| `ROM3B.SYX` | ROM 3B — Keyboard & Plucked Group (US) | `E.GRAND 1..2`, `HONKY TONK`, `FUNK CLAV`, `60-S ORGAN`, `PLUCK BASS` |
| `ROM4A.SYX` | ROM 4A — Orchestral & Percussive (US) | `PAN FLUTE`, `LEAD BRASS`, `SYNTHBRASS`, `PIZZ STGS`, `TUB BELLS` |
| `ROM4B.SYX` | ROM 4B — Complex & Effects Group (US) | `PERC BRASS`, `ORCHESTRA`, `FILTER SWP`, `SHIMMER`, `EVOLUTION` |

---

### 2. `dx100/` — Yamaha DX100 Factory ROMs (192 Voices, Native 4-Op VMEM `0x04`)
Header: `F0 43 00 04 20 00` + `32 * 128` bytes + checksum + `F7` (`4,104` bytes each).
*(Hardware DX100 banks hold 24 patches each; per standard Yamaha 4-op VMEM convention, slots `1..24` contain the factory ROM voices and slots `25..32` are `INIT VOICE` padding.)*

| File | Hardware Bank | Notable Factory Patches (Slots 1–24) |
|---|---|---|
| `dx100_1.syx` | Preset Bank A (001–024) | `IvoryEbony`, `Uprt piano`, `HonkeyTonk`, `Elec Grand`, `Wood Piano`, `Jazz Organ`, `Club Organ` |
| `dx100_2.syx` | Preset Bank B (025–048) | `Solid Bass` (Slot 1), `SyntheBass`, `Mono Bass`, `Elec Bass`, `Fretless`, `PowerBrass`, `Fuzz Clav` |
| `dx100_3.syx` | Preset Bank C (049–072) | `Easy Synth`, `Easy Clav`, `Metal Keys`, `S/H Synth`, `Heavysynth`, `Feed Lead`, `Mono Lead` |
| `dx100_4.syx` | Preset Bank D (073–096) | `Glocken`, `Hamarimba`, `SteelDrums`, `Tube Bells`, `FM SQUARE`, `FM PULSE`, `FMSAWTOOTH` |
| `dx100_5.syx` | Shift Bank A (097–120) | `Piano 1..3`, `Deep Grand`, `PhaseGrand`, `E Piano 1..3`, `Hard Tines`, `Organ 1..2` |
| `dx100_6.syx` | Shift Bank B (121–144) | `Clickorgan`, `Drawbars`, `SynthBass1..2`, `Pluck Bass`, `Flap Bass`, `Brass 1..7`, `Strings 1..3` |
| `dx100_7.syx` | Shift Bank C (145–168) | `Rich Strg1..3`, `Pizzicato`, `Clav 1..2`, `Mute Clav1..2`, `LeadSynth1`, `RubberBand`, `Huff Talk` |
| `dx100_8.syx` | Shift Bank D (169–192) | `Snare Bass`, `Tom Toms`, `Marimba`, `Vibe`, `TublarBell`, `Fantasy`, `SpaceChime`, `Space Talk` |

---

### 3. `tx81z/` — Yamaha TX81Z Factory ROMs (128 Unique Voices, Native 4-Op VMEM `0x04` + ACED)
Header: `F0 43 00 04 20 00` + `32 * 128` bytes + checksum + `F7` (`4,104` bytes each).
*(TX81Z embeds its ACED extended parameters—operator waveforms `W1..W8`, fine frequency ratios, EG shift, and reverb rate—inside bytes `73..83` of each 128-byte VMEM voice record.)*

| File | Hardware Bank | Notable Factory Patches (Slots 1–32) |
|---|---|---|
| `tx81z_a.syx` | Preset Bank A (`A01–A32`) | `GrandPiano`, `Deep Grd`, `LoTine81Z`, `HiTine81Z`, `DynomiteEP`, `DynoWurlie`, `PercOrgan`, `EZ Clav` |
| `tx81z_b.syx` | Preset Bank B (`B01–B32`) | `Trumpet81Z`, `Full Brass`, `ChorusBras`, `AtackBrass`, `DoubleBass`, `SoloViolin`, `ReverbStrg`, `HarmoPad` |
| `tx81z_c.syx` | Preset Bank C (`C01–C32`) | `Funky Pick`, `HeavyMetal`, `SqncrBass`, `SynFunkBas`, `AnalogBass`, `Jaco Bass`, **`LatelyBass`** (Slot 15), `WarmSquare`, `Sync Lead` |
| `tx81z_d.syx` | Preset Bank D (`D01–D32`) | `BaadBreath`, `KrstlChoir`, `Metalimba`, `WaterGlass`, `BowedBell`, `Spc Midiot`, `MalibuNite`, `Space Vibe` |
| `tx81z_i_factory.syx` | Factory Internal RAM (`I01–I32`) | Factory preload of Internal User Bank I (identical to Preset Bank C; `LatelyBass` at `I15`). |

---

### 4. `dx7-converted/` — Ready-to-Load 6-Op DX7 VMEM (`0x09`) Conversions
Standard `4,104`-byte DX7 `0x09` cartridges directly loadable by any 6-operator DX7 / MSFA / Dexed VMEM parser:
- `dx7-converted/dx100/dx100_dx7_1.syx` .. `dx100_dx7_6.syx`: All `192` DX100 factory presets packed contiguously into `6 x 32-voice` DX7 cartridges (`Solid Bass` is in `dx100_dx7_1.syx` slot 25).
- `dx7-converted/tx81z/tx81z_a_dx7.syx` .. `tx81z_d_dx7.syx`: All `4 x 32-voice` TX81Z factory banks (`LatelyBass` is in `tx81z_c_dx7.syx` slot 15).
