# Third-party components

Fanfare is licensed under GPL v3 (see [LICENSE](LICENSE)). The following
third-party code is included in this repository:

## msfa FM synthesis core — `Source/Synth/Fm/msfa/`

Files: `aligned_buf.h`, `controllers.h`, `env.cc/h`, `exp2.cc/h`,
`fm_core.cc/h`, `fm_op_kernel.cc/h`, `freqlut.cc/h`, `lfo.cc/h`,
`pitchenv.cc/h`, `sin.cc/h`, `synth.h`, `module.h`

- Copyright 2012 Google Inc. (Pascal Gauthier for later additions).
- From [Dexed](https://github.com/asb2m10/dexed) (`source/msfa/`).
- Licensed under the **Apache License 2.0** (see the license header in each
  file). Dexed's README states the msfa component remains Apache 2.0 so it
  can be shared between projects. Apache 2.0 is compatible with GPL v3.
- These files are vendored **unmodified**. Fanfare's note/voice layer
  (`FmVoice.h`, `FmEngine.h`) is Fanfare's own code built on top of these
  modules and is GPL v3. `FmSupport.cpp` provides the `dexed_trace` stub the
  core expects.

## DX7 voice/sysex data layout — `Source/Synth/Fm/FmVoiceData.h`

The 155-byte DX7 voice parameter map and the 32-voice bulk-dump
packing/unpacking are ported from Dexed's `core/Cartridge.cpp` and
`core/Program.cpp` (Dexed is GPL v3, same license as Fanfare). The DX7
"INIT VOICE" factory bytes are likewise from Dexed.

## TX81Z operator waveforms — `Source/Synth/Fm/FmWaves.h`

The 8 TX81Z operator waveform shapes are the ones measured/simulated by the
Csound `tx81z` opcode (and its sc-tx81z SuperCollider port), expressed as
closed-form single-cycle tables.

Everything else in `Source/Synth/Fm/` (`FmEngine.h`, `FmVoice.h`, `FmCore4.h`,
`FmPresets.h`, `FmSynthProcessor.h`, `ui/FmSynthComponent.h`) is original
Fanfare code under GPL v3.
