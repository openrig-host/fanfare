#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// ==============================================================================
// FanfareSynthProcessor — the native subtractive synth ("the Fanfare SYN").
//
// Juno-106-flavoured voice architecture living directly on a RackSlot, next to
// the sampler and the MP3 player. Header-only, following the SamplerProcessor
// pattern: all parameter state lives in std::atomic<float>, the audio thread
// snapshots the parameter block once per processBlock, and the full patch
// state serialises to/from a single juce::var via getState()/applyState().
//
// Voice: 2 band-limited main OSCs (saw/pulse/tri/noise, PWM on pulse) + a sub
// OSC, into a 12/24 dB TPT state-variable lowpass with its own (or the amp)
// envelope, amp + filter ADSRs, a shared free-running LFO with fixed routings
// (PWM / pitch / filter / amp), chorus (Juno I/II/custom), ping-pong delay and
// a simple reverb. Poly / Mono / Legato with Juno-style glide modes I & II and
// a unison stack (2/4/6 voices, detune + stereo width).
// ==============================================================================

class FanfareSynthProcessor {
public:
  // ===== Parameter table ======================================================

  enum ParamId {
    // OSC
    o1Wave, o1Oct, o1Detune, o1Level,
    o2Wave, o2Oct, o2Detune, o2Level,
    pwmRate, pwmDepth, subWave, subLevel,
    // FILTER
    cutoff, reso, filtEnvAmt, keyTrack, velToFilt, filtEnvSrc, filtSlope,
    // AMP ENV
    atk, dec, sus, rel, velToAmp,
    // FILTER ENV
    fatk, fdec, fsus, frel,
    // LFO
    lfoRate, lfoWave, lfoDelay, lfoToPwm, lfoToPitch, lfoToFilt, lfoToAmp,
    // PERF
    voiceMode, glideMode, glideTime, unison, uniDetune, uniWidth,
    // CHORUS
    choMode, choRate, choDepth, choMix,
    // DELAY
    dlyTime, dlyFb, dlyMix,
    // REVERB
    revSize, revDamp, revMix,
    // MASTER
    volume,
    kNumParams
  };

  enum SectionId {
    SecOsc, SecFilter, SecAmpEnv, SecFltEnv, SecLfo,
    SecPerf, SecChorus, SecDelay, SecReverb, SecMaster, kNumSections
  };

  // kind: 0 = continuous (skewed knob), 1 = enumerated (combo/index)
  struct ParamDef {
    const char *id;
    const char *label;
    int section;
    int kind;
    float min, max, def;
    float skew;          // 1.0 = linear; <1 emphasises the low end
    const char *choices; // comma separated, kind 1 only
    const char *unit;    // "" / "Hz" / "s" / "%" / "ct"
    bool isPercent;      // render min..max as 0..100 %
  };

  static const std::array<ParamDef, kNumParams> &paramDefs() {
    static const std::array<ParamDef, kNumParams> defs = {{
      //  id            label         section      kind min    max    def   skew choices                      unit  pct
        {"o1Wave",     "OSC1 Wave",   SecOsc,   1, 0, 3, 0, 1, "Saw,Pul,Tri,Nse", "", false},
        {"o1Oct",      "OSC1 Oct",    SecOsc,   1, -2, 2, 0, 1, "-2,-1,0,+1,+2",       "", false},
        {"o1Detune",   "OSC1 Detune", SecOsc,   0, -50, 50, 0, 1, nullptr, "ct", false},
        {"o1Level",    "OSC1 Level",  SecOsc,   0, 0, 1, 0.85f, 1, nullptr, "", true},
        {"o2Wave",     "OSC2 Wave",   SecOsc,   1, 0, 3, 0, 1, "Saw,Pul,Tri,Nse", "", false},
        {"o2Oct",      "OSC2 Oct",    SecOsc,   1, -2, 2, 0, 1, "-2,-1,0,+1,+2",       "", false},
        {"o2Detune",   "OSC2 Detune", SecOsc,   0, -50, 50, 0, 1, nullptr, "ct", false},
        {"o2Level",    "OSC2 Level",  SecOsc,   0, 0, 1, 0.0f, 1, nullptr, "", true},
        {"pwmRate",    "PWM Rate",    SecOsc,   0, 0.05f, 20, 0.8f, 0.5f, nullptr, "Hz", false},
        {"pwmDepth",   "PWM Depth",   SecOsc,   0, 0, 1, 0, 1, nullptr, "", true},
        {"subWave",    "Sub Wave",    SecOsc,   1, 0, 2, 0, 1, "Sqr,Pul,Tri", "", false},
        {"subLevel",   "Sub Level",   SecOsc,   0, 0, 1, 0, 1, nullptr, "", true},

        {"cutoff",     "Cutoff",      SecFilter, 0, 20, 18000, 9000, 0.22f, nullptr, "Hz", false},
        {"reso",       "Resonance",   SecFilter, 0, 0, 1, 0.15f, 1, nullptr, "", true},
        {"filtEnvAmt", "Flt Env Amt", SecFilter, 0, -1, 1, 0.4f, 1, nullptr, "", true},
        {"keyTrack",   "Key Track",   SecFilter, 0, 0, 1, 0, 1, nullptr, "", true},
        {"velToFilt",  "Vel > Flt",   SecFilter, 0, 0, 1, 0.3f, 1, nullptr, "", true},
        {"filtEnvSrc", "Flt Env Src", SecFilter, 1, 0, 1, 0, 1, "Flt,Amp", "", false},
        {"filtSlope",  "Slope",       SecFilter, 1, 0, 1, 1, 1, "12 dB,24 dB", "", false},

        {"atk",        "Attack",      SecAmpEnv, 0, 0, 10, 0.01f, 0.4f, nullptr, "s", false},
        {"dec",        "Decay",       SecAmpEnv, 0, 0, 10, 0.25f, 0.4f, nullptr, "s", false},
        {"sus",        "Sustain",     SecAmpEnv, 0, 0, 1, 0.8f, 1, nullptr, "", true},
        {"rel",        "Release",     SecAmpEnv, 0, 0, 20, 0.4f, 0.4f, nullptr, "s", false},
        {"velToAmp",   "Vel > Amp",   SecAmpEnv, 0, 0, 1, 0.4f, 1, nullptr, "", true},

        {"fatk",       "Attack",      SecFltEnv, 0, 0, 10, 0.01f, 0.4f, nullptr, "s", false},
        {"fdec",       "Decay",       SecFltEnv, 0, 0, 10, 0.4f, 0.4f, nullptr, "s", false},
        {"fsus",       "Sustain",     SecFltEnv, 0, 0, 1, 0.3f, 1, nullptr, "", true},
        {"frel",       "Release",     SecFltEnv, 0, 0, 20, 0.5f, 0.4f, nullptr, "s", false},

        {"lfoRate",    "LFO Rate",    SecLfo, 0, 0.05f, 20, 5, 0.5f, nullptr, "Hz", false},
        {"lfoWave",    "LFO Wave",    SecLfo, 1, 0, 2, 0, 1, "Tri,Sqr,S&H", "", false},
        {"lfoDelay",   "LFO Delay",   SecLfo, 0, 0, 5, 0, 0.5f, nullptr, "s", false},
        {"lfoToPwm",   "LFO > PWM",   SecLfo, 0, 0, 1, 0.5f, 1, nullptr, "", true},
        {"lfoToPitch", "LFO > Pitch", SecLfo, 0, 0, 100, 0, 1, nullptr, "ct", false},
        {"lfoToFilt",  "LFO > Flt",   SecLfo, 0, 0, 1, 0, 1, nullptr, "", true},
        {"lfoToAmp",   "LFO > Amp",   SecLfo, 0, 0, 1, 0, 1, nullptr, "", true},

        {"voiceMode",  "Voices",      SecPerf, 1, 0, 2, 0, 1, "Poly,Mono,Leg", "", false},
        {"glideMode",  "Glide",       SecPerf, 1, 0, 2, 0, 1, "Off,I,II", "", false},
        {"glideTime",  "Glide Time",  SecPerf, 0, 0.01f, 2, 0.1f, 0.5f, nullptr, "s", false},
        {"unison",     "Unison",      SecPerf, 1, 0, 3, 0, 1, "Off,2,4,6", "", false},
        {"uniDetune",  "Uni Detune",  SecPerf, 0, 0, 50, 12, 1, nullptr, "ct", false},
        {"uniWidth",   "Uni Width",   SecPerf, 0, 0, 1, 0.6f, 1, nullptr, "", true},

        {"choMode",    "Chorus",      SecChorus, 1, 0, 3, 0, 1, "Off,I,II,Cust", "", false},
        {"choRate",    "Cho Rate",    SecChorus, 0, 0.1f, 8, 0.6f, 0.5f, nullptr, "Hz", false},
        {"choDepth",   "Cho Depth",   SecChorus, 0, 0, 1, 0.5f, 1, nullptr, "", true},
        {"choMix",     "Cho Mix",     SecChorus, 0, 0, 1, 0.35f, 1, nullptr, "", true},

        {"dlyTime",    "Delay Time",  SecDelay, 0, 0.02f, 1.5f, 0.3f, 0.5f, nullptr, "s", false},
        {"dlyFb",      "Delay Fdbk",  SecDelay, 0, 0, 0.95f, 0.35f, 1, nullptr, "", true},
        {"dlyMix",     "Delay Mix",   SecDelay, 0, 0, 1, 0.25f, 1, nullptr, "", true},

        {"revSize",    "Rev Size",    SecReverb, 0, 0, 1, 0.5f, 1, nullptr, "", true},
        {"revDamp",    "Rev Damp",    SecReverb, 0, 0, 1, 0.5f, 1, nullptr, "", true},
        {"revMix",     "Rev Mix",     SecReverb, 0, 0, 1, 0.2f, 1, nullptr, "", true},

        {"volume",     "Volume",      SecMaster, 0, 0, 1, 0.8f, 1, nullptr, "", true},
    }};
    return defs;
  }

  static int paramIndexForId(const juce::String &id) {
    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      if (id == defs[i].id)
        return i;
    return -1;
  }

  // ===== Construction / lifecycle =============================================

  FanfareSynthProcessor() { applyDefaults(); }

  void prepare(double sampleRate, int maxBlockSize = 8192) {
    currentSampleRate = juce::jmax(8000.0, sampleRate);

    int fxLen = (int)(currentSampleRate * 1.6) + 64;
    delayBufL.assign((size_t)fxLen, 0.0f);
    delayBufR.assign((size_t)fxLen, 0.0f);
    delayWpos = 0;
    chorusWpos = 0;
    chorusBufL.assign((size_t)(currentSampleRate * 0.06 + 64), 0.0f);
    chorusBufR.assign((size_t)(currentSampleRate * 0.06 + 64), 0.0f);
    chorusPhase = 0.0;

    synthBuffer.setSize(2, juce::jmax(maxBlockSize, 256), false, false, true);
    synthBuffer.clear();

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)juce::jmax(maxBlockSize, 256);
    spec.numChannels = 2;
    reverb.prepare(spec);

    for (auto &u : units) {
      u.ampEnv.sr = currentSampleRate;
      u.filtEnv.sr = currentSampleRate;
      u.reset();
    }
    allNotesOff();
  }

  void reset() {
    allNotesOff();
    std::fill(delayBufL.begin(), delayBufL.end(), 0.0f);
    std::fill(delayBufR.begin(), delayBufR.end(), 0.0f);
    std::fill(chorusBufL.begin(), chorusBufL.end(), 0.0f);
    std::fill(chorusBufR.begin(), chorusBufR.end(), 0.0f);
  }

  // ===== Parameter access (message thread) ====================================

  std::atomic<bool> enabled{false};

  float getParam(int idx) const {
    return (idx >= 0 && idx < kNumParams) ? p[idx].load() : 0.0f;
  }
  void setParam(int idx, float value) {
    if (idx < 0 || idx >= kNumParams)
      return;
    const auto &d = paramDefs()[idx];
    p[idx].store(juce::jlimit(d.min, d.max, value));
  }

  void applyDefaults() {
    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      p[i].store(defs[i].def);
  }

  // ===== Presets ==============================================================

  static juce::Array<juce::var> parsePresetJson(const char *json) {
    juce::Array<juce::var> out;
    auto parsed = juce::JSON::parse(json);
    if (auto *arr = parsed.getArray())
      for (const auto &v : *arr)
        if (v.isObject())
          out.add(v);
    return out;
  }

  // Applies a preset object {"p":{paramId:value,...}} to this slot's params.
  void applyPreset(const juce::var &preset) {
    auto pObj = preset.getProperty("p", juce::var());
    if (auto *obj = pObj.getDynamicObject()) {
      for (auto &entry : obj->getProperties()) {
        int idx = paramIndexForId(entry.name.toString());
        if (idx >= 0)
          setParam(idx, (float)(double)entry.value);
      }
    }
  }

  // ===== Serialization ========================================================

  juce::var getState() const {
    auto *obj = new juce::DynamicObject();
    obj->setProperty("enabled", enabled.load());
    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      obj->setProperty(juce::String(defs[i].id), (double)p[i].load());
    // CC overrides (-1 = use built-in default)
    obj->setProperty("ccCutoff", (int)ccCutoff.load());
    obj->setProperty("ccReso", (int)ccReso.load());
    obj->setProperty("ccAtk", (int)ccAtk.load());
    obj->setProperty("ccRel", (int)ccRel.load());
    obj->setProperty("ccVol", (int)ccVol.load());
    return juce::var(obj);
  }

  void applyState(const juce::var &v) {
    if (auto *obj = v.getDynamicObject()) {
      if (obj->hasProperty("enabled"))
        enabled.store((bool)obj->getProperty("enabled"));
      const auto &defs = paramDefs();
      for (int i = 0; i < kNumParams; ++i) {
        if (obj->hasProperty(defs[i].id))
          setParam(i, (float)(double)obj->getProperty(defs[i].id));
      }
      auto readCC = [&](const char *key, std::atomic<int> &target) {
        if (obj->hasProperty(key))
          target.store(juce::jlimit(-1, 127, (int)obj->getProperty(key)));
      };
      readCC("ccCutoff", ccCutoff);
      readCC("ccReso", ccReso);
      readCC("ccAtk", ccAtk);
      readCC("ccRel", ccRel);
      readCC("ccVol", ccVol);
    }
  }

  // ===== MIDI CC mapping ======================================================
  // Built-in defaults follow the GM convention (74 cutoff / 71 reso / 73 atk /
  // 72 rel / 7 volume) so a standard controller works with no setup. User
  // re-assignments override these per-slot and are saved in the rig.
  std::atomic<int> ccCutoff{74};
  std::atomic<int> ccReso{71};
  std::atomic<int> ccAtk{73};
  std::atomic<int> ccRel{72};
  std::atomic<int> ccVol{7};

  void setCCForParam(const juce::String &paramId, int cc) {
    if (paramId == "cutoff") ccCutoff.store(cc);
    else if (paramId == "reso") ccReso.store(cc);
    else if (paramId == "atk") ccAtk.store(cc);
    else if (paramId == "rel") ccRel.store(cc);
    else if (paramId == "volume") ccVol.store(cc);
  }
  int getCCForParam(const juce::String &paramId) const {
    if (paramId == "cutoff") return ccCutoff.load();
    if (paramId == "reso") return ccReso.load();
    if (paramId == "atk") return ccAtk.load();
    if (paramId == "rel") return ccRel.load();
    if (paramId == "volume") return ccVol.load();
    return -1;
  }

  // UI-thread note injection (onscreen keyboard / future tools). Events are
  // queued under a spin lock and merged at the next audio block — same
  // convention as RackSlot::injectMidi.
  void injectNote(int note, bool on, float vel = 0.85f) {
    juce::SpinLock::ScopedLockType sl(uiMidiLock);
    uiMidiQueue.addEvent(on ? juce::MidiMessage::noteOn(1, note, vel)
                            : juce::MidiMessage::noteOff(1, note), 0);
  }

  // ===== Audio =================================================================

  // Safe from any thread: just requests the panic, the audio thread performs
  // the actual kill at the top of its next block (mutating voice/stack state
  // from the message thread while the audio thread renders would be a race).
  void allNotesOff() {
    panicPending.store(true);
    {
      juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
      if (sl.isLocked())
        uiMidiQueue.clear();
    }
  }

  void processBlock(juce::AudioBuffer<float> &buffer,
                    juce::MidiBuffer &midiMessages) {
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
      return;

    // Snapshot all params once (message thread owns the atomics)
    Snap s;
    for (int i = 0; i < kNumParams; ++i)
      s.v[i] = p[i].load(std::memory_order_relaxed);

    if (synthBuffer.getNumSamples() < numSamples ||
        synthBuffer.getNumChannels() < 2)
      synthBuffer.setSize(2, numSamples, false, false, true);
    synthBuffer.clear(0, numSamples);

    // Reap voices whose envelope has finished
    for (auto &u : units)
      if (u.active && u.midiNote == -1 && u.ampEnv.idle()) {
        u.active = false;
        u.reservedMono = false;
      }

    // Deferred panic (from the UI thread / CC120/123)
    if (panicPending.exchange(false))
      panicNow();

    { // Merge UI-injected notes (onscreen keyboard); skip this block if the
      // UI thread holds the lock — the queue just rides to the next block
      juce::MidiBuffer ui;
      {
        juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
        if (sl.isLocked())
          ui.swapWith(uiMidiQueue);
      }
      if (!ui.isEmpty())
        midiMessages.addEvents(ui, 0, -1, 0);
    }

    // --- MIDI ---
    for (const auto meta : midiMessages) {
      auto msg = meta.getMessage();
      if (msg.isNoteOn())
        handleNoteOn(msg.getNoteNumber(), msg.getVelocity() / 127.0f, s);
      else if (msg.isNoteOff())
        handleNoteOff(msg.getNoteNumber(), s);
      else if (msg.isController())
        handleController(msg, s);
      else if (msg.isPitchWheel())
        pitchBend.store((msg.getPitchWheelValue() - 8192) / 8192.0f);
    }

    // --- Render voices ---
    const double sr = currentSampleRate;
    float *outL = synthBuffer.getWritePointer(0);
    float *outR = synthBuffer.getWritePointer(1);
    for (auto &u : units)
      if (u.active)
        renderUnit(u, s, outL, outR, numSamples, sr);

    // --- FX chain + master ---
    processFx(s, numSamples, sr);

    // Sum into the slot buffer
    for (int ch = 0; ch < 2 && ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, 0, synthBuffer, ch, 0, numSamples);

    // Advance the shared free-running LFO phase (new voices pick up from here)
    lfoGlobalPhase =
        std::fmod(lfoGlobalPhase + s.v[lfoRate] * numSamples / sr, 1.0);
    globalTick += (juce::uint64)numSamples;
  }

private:
  void panicNow() {
    for (auto &u : units) {
      u.active = false;
      u.reservedMono = false;
      u.midiNote = -1;
    }
    monoStack.clear();
    monoUnits.clear();
    sustainedNotes.clear();
    sustainOn = false;
  }

  // ===== Parameter snapshot (audio-thread local) ==============================
  struct Snap {
    std::array<float, kNumParams> v;
    float operator[](int i) const { return v[i]; }
  };

  // ===== DSP primitives =======================================================

  // Band-limited sawtooth with PolyBLEP correction.
  struct BlepSaw {
    double phase = 0.0, inc = 0.0;
    inline double next() {
      double t = phase;
      phase += inc;
      if (phase >= 1.0)
        phase -= 1.0;
      double y = 2.0 * t - 1.0;
      y -= polyBlep(t, inc);
      return y;
    }
    static inline double polyBlep(double t, double dt) {
      if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0; // [-1,0]: lifts the post-wrap side
      }
      if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0; // [0,1]: lowers the pre-wrap side
      }
      return 0.0;
    }
  };

  // Trapezoidal-integration state-variable lowpass (Simper TPT SVF).
  // Per-sample cutoff modulation. `damping` k = 1/Q: 2 = no resonance,
  // ->0.12 = strong resonance.
  struct SvfLP {
    double g = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, s1 = 0.0, s2 = 0.0;
    void setCutoff(double hz, double sr, double damping) {
      hz = juce::jlimit(16.0, juce::jmin(20000.0, sr * 0.45), hz);
      g = std::tan(3.14159265358979323846 * hz / sr);
      double k = juce::jlimit(0.12, 2.0, damping);
      a1 = 1.0 / (1.0 + g * (g + k));
      a2 = g * a1;
      a3 = g * a2;
    }
    void reset() { s1 = s2 = 0.0; }
    inline double next(double x) {
      double v3 = x - s2;
      double v1 = a1 * s1 + a2 * v3;
      double v2 = s2 + a2 * s1 + a3 * v3;
      s1 = 2.0 * v1 - s1;
      s2 = 2.0 * v2 - s2;
      return v2; // lowpass output
    }
  };

  // ADSR with linear attack and exponential decay/release (tau = time/3).
  struct AdsrEnv {
    enum Stage { Idle, Att, Dec, Rel } stage = Idle;
    double level = 0.0;
    double aCoef = 0.0, dCoef = 0.0, rCoef = 0.0;
    double aTime = 0.01, dTime = 0.25, sLevel = 0.8, rTime = 0.4;
    double sr = 44100.0;

    void updateCoefs() {
      aCoef = 1.0 / juce::jmax(1.0, aTime * sr);
      dCoef = std::exp(-3.0 / juce::jmax(1.0, dTime * sr));
      rCoef = std::exp(-3.0 / juce::jmax(1.0, rTime * sr));
    }
    void noteOn() {
      stage = Att;
      updateCoefs();
    }
    void noteOff() {
      if (stage != Idle) {
        stage = Rel;
        updateCoefs();
      }
    }
    void hardKill() {
      stage = Idle;
      level = 0.0;
    }
    inline double next() {
      switch (stage) {
      case Att:
        level += aCoef;
        if (level >= 1.0) {
          level = 1.0;
          stage = Dec;
        }
        break;
      case Dec:
        level = sLevel + (level - sLevel) * dCoef;
        break;
      case Rel:
        level *= rCoef;
        if (level < 0.0004) {
          level = 0.0;
          stage = Idle;
        }
        break;
      case Idle:
      default:
        level = 0.0;
        break;
      }
      return level;
    }
    bool idle() const { return stage == Idle; }
  };

  // ===== One voice unit =======================================================

  struct Unit {
    BlepSaw saw1a, saw1b, saw2a, saw2b, suba, subb;
    SvfLP lp1, lp2;
    AdsrEnv ampEnv, filtEnv;
    juce::Random rng;

    bool active = false;
    bool reservedMono = false; // owned by the mono/unison stack
    int midiNote = -1;
    float velocity = 0.0f;
    double baseFreq = 440.0;  // unmodulated note frequency
    double keyTrackMul = 1.0; // cutoff multiplier from key tracking
    float detuneCents = 0.0f; // unison spread detune for this unit
    float panL = 0.7071f, panR = 0.7071f;

    double lfoPhase = 0.0; // snapshot of the shared LFO phase at note start
    double pwmPhase = 0.0;
    float lfoSh = 0.0f; // current S&H value
    double age = 0.0;   // seconds since note start (LFO delay fade)

    bool gliding = false;
    double glideFreq = 440.0;   // current glide position (Hz)
    double glideTarget = 440.0; // destination

    juce::uint64 startedTick = 0; // for oldest-voice stealing

    void reset() {
      active = false;
      reservedMono = false;
      midiNote = -1;
      lp1.reset();
      lp2.reset();
      ampEnv.hardKill();
      filtEnv.hardKill();
      detuneCents = 0.0f;
      panL = 0.7071f;
      panR = 0.7071f;
      age = 0.0;
      gliding = false;
    }
  };

  // ===== Helpers ==============================================================

  static double noteFreq(int note) {
    return 440.0 * std::pow(2.0, (double)(note - 69) / 12.0);
  }

  static int unisonVoiceCount(float v) {
    static const int counts[] = {1, 2, 4, 6}; // 0=Off,1=2,2=4,3=6
    return counts[juce::jlimit(0, 3, (int)v)];
  }

  Unit *allocUnit() {
    for (auto &u : units)
      if (!u.active && !u.reservedMono)
        return &u;
    // Steal the oldest non-mono voice: hard-kill so it can't muddy the new note
    Unit *best = nullptr;
    for (auto &u : units) {
      if (u.reservedMono)
        continue;
      if (best == nullptr || u.startedTick < best->startedTick)
        best = &u;
    }
    if (best)
      best->reset();
    return best;
  }

  // ===== Note handling ========================================================

  void handleNoteOn(int note, float vel, const Snap &s) {
    const int mode = (int)s.v[voiceMode];
    const int uni = unisonVoiceCount(s.v[unison]);
    const bool monoish = (mode != 0) || uni > 1;

    // ---- Poly: one unit per note ----
    if (!monoish) {
      if (Unit *u = allocUnit())
        triggerUnit(*u, note, vel, s);
      return;
    }

    // ---- Mono / Legato / Unison: a dedicated stack of `uni` units ----
    const bool wasHeld = !monoStack.empty();
    if ((int)monoStack.size() < 16)
      monoStack.push_back(note);
    const bool legato = (mode == 2) && wasHeld; // no env retrigger on overlap
    bool doGlide =
        ((int)s.v[glideMode] > 0) && (wasHeld || (int)s.v[glideMode] == 1);
    if (!glideStarted) {
      doGlide = false; // the very first note never glides
      glideStarted = true;
    }
    const double targetFreq = noteFreq(note);

    // Resize the mono unit pool if the unison setting changed
    while ((int)monoUnits.size() > uni) {
      Unit &u = units[monoUnits.back()];
      u.ampEnv.noteOff();
      u.filtEnv.noteOff();
      u.midiNote = -1;
      u.reservedMono = false;
      monoUnits.pop_back();
    }
    while ((int)monoUnits.size() < uni) {
      Unit *u = allocUnit();
      if (u == nullptr)
        break;
      u->active = true;
      u->reservedMono = true;
      u->midiNote = note;
      u->ampEnv.noteOn();
      u->filtEnv.noteOn();
      u->lfoPhase = lfoGlobalPhase;
      u->pwmPhase = 0.0;
      u->age = 0.0;
      u->gliding = false;
      u->glideFreq = targetFreq;
      u->glideTarget = targetFreq;
      u->lp1.reset();
      u->lp2.reset();
      u->startedTick = globalTick;
      u->rng.setSeed((juce::int64)(note * 7919 + (int)monoUnits.size() * 131 + 7));
      u->lfoSh = u->rng.nextFloat() * 2.0f - 1.0f;
      monoUnits.push_back((int)(u - units.data()));
    }

    // Retarget (+ optionally retrigger) every unit in the stack
    const int count = (int)monoUnits.size();
    int idx = 0;
    for (int ui : monoUnits) {
      Unit &u = units[ui];
      u.midiNote = note;
      u.baseFreq = targetFreq;
      u.glideTarget = targetFreq;
      u.velocity = juce::jmax(vel, 0.05f);
      u.keyTrackMul = std::pow(2.0, s.v[keyTrack] * (note - 60) / 12.0);
      u.startedTick = globalTick;

      if (doGlide) {
        if (!u.gliding)
          u.gliding = true; // glideFreq keeps its current position
      } else {
        u.gliding = false;
        u.glideFreq = targetFreq;
      }

      if (!legato) {
        u.ampEnv.noteOn();
        u.filtEnv.noteOn();
        u.age = 0.0;
        u.lfoPhase = lfoGlobalPhase;
      }

      // Unison spread: detune + equal-power stereo pan across the stack,
      // normalized by 1/sqrt(N) so 4- or 6-voice unison doesn't clip the bus
      const float uniNorm = 1.0f / std::sqrt((float)juce::jmax(1, count));
      float t = (count > 1) ? (float)idx / (float)(count - 1) : 0.5f;
      u.detuneCents = s.v[uniDetune] * (2.0f * t - 1.0f);
      float ang = juce::MathConstants<float>::halfPi *
                  (0.5f + s.v[uniWidth] * (t - 0.5f));
      u.panL = std::cos(ang) * uniNorm;
      u.panR = std::sin(ang) * uniNorm;
      ++idx;
    }
  }

  void triggerUnit(Unit &u, int note, float vel, const Snap &s) {
    u.active = true;
    u.reservedMono = false;
    u.midiNote = note;
    u.velocity = juce::jmax(vel, 0.05f);
    u.baseFreq = noteFreq(note);
    u.keyTrackMul = std::pow(2.0, s.v[keyTrack] * (note - 60) / 12.0);
    u.detuneCents = 0.0f;
    u.panL = 0.7071f;
    u.panR = 0.7071f;
    u.ampEnv.noteOn();
    u.filtEnv.noteOn();
    u.lfoPhase = lfoGlobalPhase;
    u.pwmPhase = 0.0;
    u.age = 0.0;
    u.gliding = false;
    u.glideFreq = u.baseFreq;
    u.glideTarget = u.baseFreq;
    u.lp1.reset();
    u.lp2.reset();
    u.startedTick = globalTick;
    u.rng.setSeed((juce::int64)(note * 7919 + 13));
    u.lfoSh = u.rng.nextFloat() * 2.0f - 1.0f;
  }

  void handleNoteOff(int note, const Snap &s) {
    if (sustainOn) {
      sustainedNotes.push_back(note);
      return;
    }
    releaseNote(note, s);
  }

  void releaseNote(int note, const Snap &s) {
    const int mode = (int)s.v[voiceMode];
    const int uni = unisonVoiceCount(s.v[unison]);
    const bool monoish = (mode != 0) || uni > 1;

    if (monoish) {
      for (int i = (int)monoStack.size() - 1; i >= 0; --i) {
        if (monoStack[i] == note) {
          monoStack.erase(monoStack.begin() + i);
          break;
        }
      }
      if (!monoStack.empty()) {
        // Retarget the stack to the most recent held note (no env retrigger)
        const double targetFreq = noteFreq(monoStack.back());
        for (int ui : monoUnits) {
          Unit &u = units[ui];
          u.baseFreq = targetFreq;
          u.glideTarget = targetFreq;
          if ((int)s.v[glideMode] > 0)
            u.gliding = true;
        }
      } else {
        for (int ui : monoUnits) {
          Unit &u = units[ui];
          u.ampEnv.noteOff();
          u.filtEnv.noteOff();
          u.midiNote = -1;
          u.reservedMono = false;
        }
        monoUnits.clear();
      }
      return;
    }

    for (auto &u : units)
      if (u.active && u.midiNote == note) {
        u.ampEnv.noteOff();
        u.filtEnv.noteOff();
        u.midiNote = -1;
      }
  }

  void handleController(const juce::MidiMessage &msg, const Snap &s) {
    const int ccNum = msg.getControllerNumber();
    const int ccVal = msg.getControllerValue();

    if (ccNum == 64) { // sustain pedal
      const bool on = ccVal >= 64;
      if (sustainOn && !on) {
        sustainOn = false;
        auto deferred = std::move(sustainedNotes);
        sustainedNotes.clear();
        for (int n : deferred)
          releaseNote(n, s);
      } else {
        sustainOn = on;
      }
      return;
    }
    if (ccNum == 1) { // mod wheel -> built-in vibrato depth
      modWheel.store(ccVal / 127.0f);
      return;
    }
    if (ccNum == 120 || ccNum == 123) { // all sound off / all notes off
      allNotesOff();
      return;
    }

    // Built-in parameter CCs (defaults follow the GM convention)
    if (ccNum == ccCutoff.load())
      p[cutoff].store(20.0f * std::pow(900.0f, ccVal / 127.0f)); // 20 Hz..18 kHz
    else if (ccNum == ccReso.load())
      p[reso].store(ccVal / 127.0f);
    else if (ccNum == ccAtk.load())
      p[atk].store(std::pow(ccVal / 127.0f, 2.0f) * 10.0f);
    else if (ccNum == ccRel.load())
      p[rel].store(std::pow(ccVal / 127.0f, 2.0f) * 20.0f);
    else if (ccNum == ccVol.load())
      p[volume].store(ccVal / 127.0f);
    (void)s;
  }

  // ===== Per-unit rendering ===================================================

  void renderUnit(Unit &u, Snap &s, float *outL, float *outR, int numSamples,
                  double sr) {
    // Envelope coefficient refresh (CCs can change times mid-note)
    u.ampEnv.sr = sr;
    u.ampEnv.aTime = s.v[atk];
    u.ampEnv.dTime = s.v[dec];
    u.ampEnv.sLevel = s.v[sus];
    u.ampEnv.rTime = s.v[rel];
    u.filtEnv.sr = sr;
    u.filtEnv.aTime = s.v[fatk];
    u.filtEnv.dTime = s.v[fdec];
    u.filtEnv.sLevel = s.v[fsus];
    u.filtEnv.rTime = s.v[frel];
    u.ampEnv.updateCoefs();
    u.filtEnv.updateCoefs();

    const int w1 = (int)s.v[o1Wave];
    const int w2 = (int)s.v[o2Wave];
    const int wSub = (int)s.v[subWave];
    const bool slope24 = (int)s.v[filtSlope] > 0;
    const bool envSrcIsAmp = (int)s.v[filtEnvSrc] > 0;

    const double octMul1 = std::pow(2.0, (double)(int)s.v[o1Oct]);
    const double octMul2 = std::pow(2.0, (double)(int)s.v[o2Oct]);
    const double detMul1 = std::pow(2.0, s.v[o1Detune] / 1200.0);
    const double detMul2 = std::pow(2.0, s.v[o2Detune] / 1200.0);
    const double uniMul = std::pow(2.0, u.detuneCents / 1200.0);

    const double baseCutoff = s.v[cutoff];
    // In 24 dB mode two identical 2-pole SVFs are cascaded (gains multiply),
    // so each stage takes sqrt(Q) to keep passband flat (sqrt(0.5)=0.707) and
    // prevent Q^2 (+36 dB) resonant blowup.
    const double qTarget = 0.5 + s.v[reso] * 7.5; // Q 0.5..8
    const double kDamp = 1.0 / (slope24 ? std::sqrt(qTarget) : qTarget);

    const double lfoInc = s.v[lfoRate] / sr;
    const double pwmInc = s.v[pwmRate] / sr;
    const double bendMul = std::pow(2.0, (double)pitchBend.load() * 2.0 / 12.0);
    const double mod = (double)modWheel.load();

    const double velCurve = std::pow((double)u.velocity, 1.5);
    const double ampVelGain = (1.0 - s.v[velToAmp]) + s.v[velToAmp] * velCurve;
    const double filtVelGain =
        (1.0 - s.v[velToFilt]) + s.v[velToFilt] * velCurve;

    const double fEnvOct = s.v[filtEnvAmt] * 5.0; // +/- 5 octaves max sweep
    const double lfoFltOct = s.v[lfoToFilt] * 2.0; // +/- 2 octaves
    const double lfoPitchCents = s.v[lfoToPitch];
    const double vibCents = mod * 50.0; // mod wheel adds gentle vibrato
    const double lfoPwm = s.v[lfoToPwm] * 0.45;
    const double pwmDepthAmt = s.v[ParamId::pwmDepth] * 0.45;
    const double lfoDelaySec = s.v[ParamId::lfoDelay];
    const double tremDepth = s.v[lfoToAmp];

    const int lfoWaveShape = (int)s.v[ParamId::lfoWave];
    const double glideCoef =
        std::exp(-1.0 / juce::jmax(1.0, s.v[glideTime] * 0.25 * sr));

    const double o1Gain = s.v[o1Level];
    const double o2Gain = s.v[o2Level];
    const double subGain = s.v[subLevel];
    // Per-voice polyphonic headroom (~-13 dB) so 4-6 note chords sit around
    // -8..-6 dBFS instead of slamming the FX bus and soft clipper.
    static constexpr float kVoiceHeadroom = 0.22f;
    const float gl = u.panL * kVoiceHeadroom;
    const float gr = u.panR * kVoiceHeadroom;

    for (int i = 0; i < numSamples; ++i) {
      // --- Glide (mono family) ---
      if (u.gliding) {
        u.glideFreq = u.glideTarget + (u.glideFreq - u.glideTarget) * glideCoef;
        if (std::abs(u.glideFreq - u.glideTarget) < u.glideTarget * 0.0006)
          u.gliding = false;
      }
      const double noteFreqNow = (u.gliding ? u.glideFreq : u.baseFreq) * bendMul;

      // --- LFO (bipolar -1..1, delayed fade-in) ---
      double lfoVal;
      if (lfoWaveShape == 0) // Tri
        lfoVal = u.lfoPhase < 0.5 ? (4.0 * u.lfoPhase - 1.0)
                                  : (3.0 - 4.0 * u.lfoPhase);
      else if (lfoWaveShape == 1) // Square
        lfoVal = u.lfoPhase < 0.5 ? 1.0 : -1.0;
      else // S&H
        lfoVal = u.lfoSh;

      u.lfoPhase += lfoInc;
      if (u.lfoPhase >= 1.0) {
        u.lfoPhase -= 1.0;
        if (lfoWaveShape == 2)
          u.lfoSh = (float)(u.rng.nextDouble() * 2.0 - 1.0);
      }

      u.age += 1.0 / sr;
      double lfoFade = 1.0;
      if (lfoDelaySec > 0.001) {
        if (u.age < lfoDelaySec)
          lfoFade = 0.0;
        else
          lfoFade = juce::jlimit(0.0, 1.0, (u.age - lfoDelaySec) / 1.0);
      }
      const double lfoMod = lfoVal * lfoFade;

      // --- PWM modulation oscillator (sine) ---
      u.pwmPhase += pwmInc;
      if (u.pwmPhase >= 1.0)
        u.pwmPhase -= 1.0;
      const double pwmVal = std::sin(u.pwmPhase * 6.283185307179586);
      double width = 0.5 + pwmDepthAmt * pwmVal + lfoPwm * lfoMod;
      width = juce::jlimit(0.05, 0.95, width);

      // --- Oscillator frequencies ---
      const double pitchMul =
          std::pow(2.0, (lfoPitchCents + vibCents) * lfoMod / 1200.0);
      double f1 = juce::jlimit(4.0, sr * 0.45,
                               noteFreqNow * octMul1 * detMul1 * uniMul * pitchMul);
      double f2 = juce::jlimit(4.0, sr * 0.45,
                               noteFreqNow * octMul2 * detMul2 * uniMul * pitchMul);
      double fSub = juce::jlimit(2.0, sr * 0.45, f1 * 0.5);

      // --- Oscillator 1 ---
      double mix = 0.0;
      if (w1 == 0) { // Saw
        u.saw1a.inc = f1 / sr;
        mix = u.saw1a.next() * o1Gain;
      } else if (w1 == 1) { // Pulse: two saws offset by `width`, amplitude-normalised
        u.saw1a.inc = f1 / sr;
        double t = u.saw1a.phase;
        u.saw1b.phase = t + width >= 1.0 ? t + width - 1.0 : t + width;
        u.saw1b.inc = u.saw1a.inc;
        const double pNorm = 0.5 / juce::jmax(width, 1.0 - width);
        mix = (u.saw1a.next() - u.saw1b.next()) * pNorm * o1Gain;
      } else if (w1 == 2) { // Tri (folded band-limited saw)
        u.saw1a.inc = f1 / sr;
        mix = (1.0 - 2.0 * std::fabs(u.saw1a.next())) * o1Gain;
      } else { // Noise
        mix = (u.rng.nextFloat() * 2.0f - 1.0f) * o1Gain;
      }

      // --- Oscillator 2 ---
      if (w2 == 0) {
        u.saw2a.inc = f2 / sr;
        mix += u.saw2a.next() * o2Gain;
      } else if (w2 == 1) {
        u.saw2a.inc = f2 / sr;
        double t = u.saw2a.phase;
        u.saw2b.phase = t + width >= 1.0 ? t + width - 1.0 : t + width;
        u.saw2b.inc = u.saw2a.inc;
        const double pNorm = 0.5 / juce::jmax(width, 1.0 - width);
        mix += (u.saw2a.next() - u.saw2b.next()) * pNorm * o2Gain;
      } else if (w2 == 2) {
        u.saw2a.inc = f2 / sr;
        mix += (1.0 - 2.0 * std::fabs(u.saw2a.next())) * o2Gain;
      } else {
        mix += (u.rng.nextFloat() * 2.0f - 1.0f) * o2Gain;
      }

      // --- Sub (one octave down) ---
      if (subGain > 0.0001) {
        if (wSub == 0) { // Square (50%)
          u.suba.inc = fSub / sr;
          double t = u.suba.phase;
          u.subb.phase = t + 0.5 >= 1.0 ? t - 0.5 : t + 0.5;
          u.subb.inc = u.suba.inc;
          mix += (u.suba.next() - u.subb.next()) * 0.5 * subGain;
        } else if (wSub == 1) { // Pulse (25%)
          u.suba.inc = fSub / sr;
          double t = u.suba.phase;
          u.subb.phase = t + 0.25 >= 1.0 ? t - 0.75 : t + 0.25;
          u.subb.inc = u.suba.inc;
          mix += (u.suba.next() - u.subb.next()) * (0.5 / 0.75) * subGain;
        } else { // Tri
          u.suba.inc = fSub / sr;
          mix += (1.0 - 2.0 * std::fabs(u.suba.next())) * subGain;
        }
      }

      // --- Envelopes ---
      const double ampEnvVal = u.ampEnv.next();
      const double fEnvVal = envSrcIsAmp ? ampEnvVal : u.filtEnv.next();

      // --- Filter (per-sample modulated) ---
      const double cutHz =
          juce::jlimit(16.0, sr * 0.45,
                       baseCutoff * u.keyTrackMul *
                           std::pow(2.0, fEnvOct * filtVelGain * fEnvVal +
                                             lfoFltOct * lfoMod));
      u.lp1.setCutoff(cutHz, sr, kDamp);
      double filtered = u.lp1.next(mix);
      if (slope24) {
        u.lp2.setCutoff(cutHz, sr, kDamp);
        filtered = u.lp2.next(filtered);
      }

      // --- Amp ---
      const double trem = 1.0 - tremDepth * 0.5 * (1.0 - lfoMod);
      const double amp = ampEnvVal * ampVelGain * trem;

      outL[i] += (float)(filtered * amp * gl);
      outR[i] += (float)(filtered * amp * gr);
    }
  }

  // ===== FX chain =============================================================

  void processFx(Snap &s, int numSamples, double sr) {
    // --- Chorus (Juno I / II / custom) ---
    const int cho = (int)s.v[choMode];
    if (cho > 0) {
      float rate, depthMs, mix;
      if (cho == 1) { // I (thin): subtle, cleaner
        rate = 0.8f;
        depthMs = 2.0f;
        mix = s.v[choMix] * 0.8f;
      } else if (cho == 2) { // II (thick): the Juno strings sound
        rate = 0.45f;
        depthMs = 5.0f;
        mix = s.v[choMix];
      } else { // Custom
        rate = s.v[choRate];
        depthMs = 1.0f + s.v[choDepth] * 7.0f;
        mix = s.v[choMix];
      }
      renderChorus(rate, depthMs, mix, numSamples, sr);
    }

    // --- Ping-pong delay ---
    if (s.v[dlyMix] > 0.001f)
      renderDelay(s.v[dlyTime], s.v[dlyFb], s.v[dlyMix], numSamples, sr);

    // --- Reverb ---
    if (s.v[revMix] > 0.001f) {
      juce::dsp::Reverb::Parameters rp;
      rp.roomSize = s.v[revSize];
      rp.damping = s.v[revDamp];
      rp.width = 1.0f;
      rp.wetLevel = s.v[revMix];
      rp.dryLevel = 1.0f - s.v[revMix];
      rp.freezeMode = 0.0f;
      reverb.setParameters(rp);
      auto block = juce::dsp::AudioBlock<float>(synthBuffer)
                       .getSubBlock(0, (size_t)numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx(block);
      reverb.process(ctx);
    }

    // --- Master soft clip (bus safety ceiling) + post-clip output level ---
    const float vol = s.v[volume];
    float *mL = synthBuffer.getWritePointer(0);
    float *mR = synthBuffer.getWritePointer(1);
    for (int i = 0; i < numSamples; ++i) {
      mL[i] = std::tanh(mL[i]) * vol;
      mR[i] = std::tanh(mR[i]) * vol;
    }
  }

  void renderChorus(float rateHz, float depthMs, float mix, int numSamples,
                    double sr) {
    const int bufLen = (int)chorusBufL.size();
    if (bufLen < 64)
      return;
    const double baseDelayS = 0.012;
    const double depthS = depthMs / 1000.0;
    const double inc = rateHz / sr;

    float *L = synthBuffer.getWritePointer(0);
    float *R = synthBuffer.getWritePointer(1);
    const float *inL = synthBuffer.getReadPointer(0);
    const float *inR = synthBuffer.getReadPointer(1);

    for (int i = 0; i < numSamples; ++i) {
      chorusBufL[(size_t)chorusWpos] = inL[i];
      chorusBufR[(size_t)chorusWpos] = inR[i];

      chorusPhase += inc;
      if (chorusPhase >= 1.0)
        chorusPhase -= 1.0;
      const double modL = std::sin(chorusPhase * 6.283185307179586);
      const double modR =
          std::sin((chorusPhase + 0.25) * 6.283185307179586);

      const double dL = baseDelayS + depthS * (0.5 + 0.5 * modL);
      const double dR = baseDelayS + depthS * (0.5 + 0.5 * modR);

      const float wetL = readInterpolated(chorusBufL, bufLen, chorusWpos, dL * sr);
      const float wetR = readInterpolated(chorusBufR, bufLen, chorusWpos, dR * sr);

      L[i] = inL[i] * (1.0f - mix * 0.5f) + wetL * mix;
      R[i] = inR[i] * (1.0f - mix * 0.5f) + wetR * mix;

      chorusWpos = (chorusWpos + 1) % bufLen;
    }
  }

  static float readInterpolated(std::vector<float> &buf, int bufLen, int wpos,
                                double delaySamples) {
    double readPos = (double)wpos - delaySamples;
    while (readPos < 0.0)
      readPos += bufLen;
    const int i0 = (int)readPos % bufLen;
    const int i1 = (i0 + 1) % bufLen;
    const float frac = (float)(readPos - std::floor(readPos));
    return buf[(size_t)i0] * (1.0f - frac) + buf[(size_t)i1] * frac;
  }

  void renderDelay(float timeS, float fb, float mix, int numSamples, double sr) {
    const int bufLen = (int)delayBufL.size();
    if (bufLen < 64)
      return;
    const int delaySamples = juce::jlimit(32, bufLen - 2, (int)(timeS * sr));

    float *L = synthBuffer.getWritePointer(0);
    float *R = synthBuffer.getWritePointer(1);

    for (int i = 0; i < numSamples; ++i) {
      int rpos = delayWpos - delaySamples;
      while (rpos < 0)
        rpos += bufLen;

      const float dl = delayBufL[(size_t)rpos];
      const float dr = delayBufR[(size_t)rpos];

      // Ping-pong: each channel's input feeds the opposite line
      delayBufL[(size_t)delayWpos] = R[i] + fb * dl;
      delayBufR[(size_t)delayWpos] = L[i] + fb * dr;

      L[i] = L[i] * (1.0f - mix) + dl * mix;
      R[i] = R[i] * (1.0f - mix) + dr * mix;

      delayWpos = (delayWpos + 1) % bufLen;
    }
  }

  // ===== Members ===============================================================

private:
  std::array<std::atomic<float>, kNumParams> p{};
  std::atomic<float> pitchBend{0.0f};
  std::atomic<float> modWheel{0.0f};

  std::atomic<bool> panicPending{false};
  juce::SpinLock uiMidiLock;      // guards uiMidiQueue (UI thread <-> audio)
  juce::MidiBuffer uiMidiQueue;   // notes injected from the message thread

  bool sustainOn = false;
  std::vector<int> monoStack;      // currently held notes (mono/legato/unison)
  std::vector<int> monoUnits;      // indices of units owned by the mono stack
  std::vector<int> sustainedNotes; // notes deferred by the sustain pedal
  bool glideStarted = false;

  std::array<Unit, 16> units;
  double lfoGlobalPhase = 0.0;
  juce::uint64 globalTick = 0;
  double currentSampleRate = 44100.0;

  juce::AudioBuffer<float> synthBuffer;

  // FX
  std::vector<float> delayBufL, delayBufR;
  std::vector<float> chorusBufL, chorusBufR;
  int delayWpos = 0, chorusWpos = 0;
  double chorusPhase = 0.0;
  juce::dsp::Reverb reverb;

  // ===== Factory preset bank ==================================================
  // The usual suspects: pads, leads, synth basses (+ keys & percussion).
  // Values use raw parameter units (Hz / seconds / 0..1 / enum indices).

public:
  // Factory preset bank — the usual suspects: pads, leads, synth basses
  // (+ keys & percussion). Values use raw parameter units
  // (Hz / seconds / 0..1 / enum indices).
  static const juce::Array<juce::var> &factoryPresets() {
    static const juce::Array<juce::var> presets =
        parsePresetJson(kFactoryPresetJson);
    return presets;
  }

  static inline const char *kFactoryPresetJson =
    R"PRESETJSON([
 {"name":"Init Patch","cat":"Init","p":{}},
 {"name":"Analog Strings","cat":"Pads","p":{"o1Wave":0,"o1Level":0.85,"o2Wave":0,"o2Level":0.6,"o2Detune":9,"subLevel":0.12,"pwmDepth":0.3,"pwmRate":0.6,"cutoff":3200,"reso":0.12,"filtEnvAmt":0.15,"atk":0.35,"dec":0.8,"sus":0.85,"rel":1.4,"velToAmp":0.25,"lfoRate":4.6,"lfoToPitch":5,"lfoDelay":1.2,"choMode":2,"choMix":0.42,"revMix":0.22,"revSize":0.6,"volume":0.8}},
 {"name":"Dream Pad","cat":"Pads","p":{"o1Wave":0,"o1Level":0.7,"o2Wave":2,"o2Level":0.5,"o2Oct":1,"subLevel":0.2,"pwmDepth":0.4,"pwmRate":0.3,"cutoff":2400,"reso":0.2,"filtEnvAmt":0.3,"fatk":1.5,"atk":1.8,"dec":1.5,"sus":0.9,"rel":2.5,"velToAmp":0.15,"lfoRate":0.8,"lfoToFilt":0.25,"lfoDelay":2.0,"choMode":2,"choMix":0.5,"revMix":0.35,"revSize":0.85,"volume":0.75}},
 {"name":"Warm Pad","cat":"Pads","p":{"o1Wave":1,"o1Level":0.8,"o2Wave":0,"o2Level":0.55,"o2Detune":-7,"subWave":0,"subLevel":0.25,"pwmDepth":0.5,"pwmRate":0.5,"cutoff":1800,"reso":0.1,"filtEnvAmt":0.2,"atk":0.9,"dec":1.0,"sus":0.85,"rel":1.8,"velToAmp":0.2,"choMode":2,"choMix":0.45,"revMix":0.25,"revSize":0.7,"volume":0.8}},
 {"name":"Glass Pad","cat":"Pads","p":{"o1Wave":2,"o1Level":0.75,"o2Wave":2,"o2Level":0.6,"o2Oct":1,"o2Detune":11,"subLevel":0.0,"cutoff":6000,"reso":0.15,"filtEnvAmt":0.25,"fatk":0.8,"atk":0.6,"dec":2.0,"sus":0.7,"rel":2.2,"lfoRate":3.2,"lfoToFilt":0.15,"lfoDelay":1.0,"choMode":2,"choMix":0.4,"revMix":0.4,"revSize":0.9,"volume":0.7}},
 {"name":"Cathedral Pad","cat":"Pads","p":{"o1Wave":0,"o1Level":0.7,"o2Wave":0,"o2Level":0.5,"o2Detune":14,"subLevel":0.15,"atk":2.2,"dec":2.0,"sus":0.9,"rel":3.5,"cutoff":2800,"filtEnvAmt":0.2,"choMode":2,"choMix":0.5,"revMix":0.55,"revSize":1.0,"revDamp":0.3,"volume":0.75}},

 {"name":"Classic Lead","cat":"Leads","p":{"o1Wave":0,"o1Level":0.9,"o2Wave":0,"o2Level":0.7,"o2Detune":8,"subLevel":0.15,"cutoff":4500,"reso":0.3,"filtEnvAmt":0.35,"fdec":0.3,"atk":0.01,"dec":0.3,"sus":0.75,"rel":0.35,"velToAmp":0.3,"velToFilt":0.4,"voiceMode":1,"glideMode":1,"glideTime":0.12,"choMode":1,"choMix":0.3,"dlyMix":0.18,"dlyTime":0.28,"dlyFb":0.3,"volume":0.8}},
 {"name":"Unison Lead","cat":"Leads","p":{"o1Wave":0,"o1Level":0.8,"o2Wave":0,"o2Level":0.6,"o2Detune":5,"subLevel":0.1,"unison":2,"uniDetune":16,"uniWidth":0.7,"cutoff":6000,"reso":0.25,"filtEnvAmt":0.3,"atk":0.02,"dec":0.4,"sus":0.8,"rel":0.4,"velToAmp":0.35,"voiceMode":1,"glideMode":1,"glideTime":0.08,"choMode":1,"choMix":0.35,"dlyMix":0.2,"volume":0.75}},
 {"name":"Soft Solo","cat":"Leads","p":{"o1Wave":2,"o1Level":0.85,"o2Wave":0,"o2Level":0.3,"o2Detune":6,"subLevel":0.1,"cutoff":3500,"reso":0.15,"filtEnvAmt":0.25,"atk":0.05,"dec":0.5,"sus":0.8,"rel":0.5,"velToAmp":0.4,"voiceMode":2,"glideMode":2,"glideTime":0.15,"lfoRate":5.5,"lfoToPitch":8,"lfoDelay":0.6,"choMode":1,"choMix":0.25,"revMix":0.15,"volume":0.8}},
 {"name":"Fifth Lead","cat":"Leads","p":{"o1Wave":0,"o1Level":0.75,"o1Oct":0,"o2Wave":0,"o2Level":0.65,"o2Oct":1,"o2Detune":4,"unison":1,"uniDetune":10,"uniWidth":0.5,"cutoff":5200,"reso":0.2,"filtEnvAmt":0.3,"atk":0.01,"dec":0.3,"sus":0.7,"rel":0.3,"voiceMode":1,"glideMode":1,"glideTime":0.1,"choMode":1,"choMix":0.3,"dlyMix":0.25,"dlyFb":0.4,"volume":0.7}},
 {"name":"Whistle Lead","cat":"Leads","p":{"o1Wave":2,"o1Level":0.9,"o2Wave":2,"o2Level":0.4,"o2Oct":2,"subLevel":0.0,"cutoff":7000,"reso":0.35,"filtEnvAmt":0.4,"fdec":0.25,"atk":0.03,"dec":0.2,"sus":0.85,"rel":0.25,"velToFilt":0.5,"voiceMode":1,"glideMode":1,"glideTime":0.2,"lfoRate":5.0,"lfoToPitch":12,"lfoDelay":0.8,"revMix":0.2,"dlyMix":0.15,"volume":0.7}},

 {"name":"Sub Bass","cat":"Basses","p":{"o1Wave":1,"o1Level":0.8,"o2Wave":0,"o2Level":0.3,"o2Oct":-1,"subWave":0,"subLevel":0.7,"cutoff":900,"reso":0.1,"filtEnvAmt":0.25,"fdec":0.15,"atk":0.005,"dec":0.2,"sus":0.7,"rel":0.15,"velToAmp":0.3,"velToFilt":0.4,"voiceMode":1,"choMode":0,"volume":0.85}},
 {"name":"Acid Bass","cat":"Basses","p":{"o1Wave":0,"o1Level":0.85,"subLevel":0.3,"cutoff":600,"reso":0.72,"filtEnvAmt":0.85,"fatk":0.005,"fdec":0.18,"fsus":0.05,"frel":0.1,"atk":0.005,"dec":0.3,"sus":0.6,"rel":0.12,"velToFilt":0.55,"voiceMode":1,"glideMode":1,"glideTime":0.09,"volume":0.8}},
 {"name":"Mono Stab","cat":"Basses","p":{"o1Wave":1,"o1Level":0.85,"o2Wave":1,"o2Level":0.5,"o2Detune":5,"subLevel":0.35,"cutoff":1400,"reso":0.35,"filtEnvAmt":0.6,"fdec":0.12,"fsus":0.0,"atk":0.004,"dec":0.15,"sus":0.0,"rel":0.1,"velToAmp":0.45,"velToFilt":0.5,"voiceMode":1,"choMode":0,"volume":0.8}},
 {"name":"Dark Pulse","cat":"Basses","p":{"o1Wave":1,"o1Level":0.8,"o2Wave":0,"o2Level":0.4,"o2Oct":-1,"subWave":0,"subLevel":0.5,"pwmDepth":0.3,"pwmRate":0.4,"cutoff":750,"reso":0.25,"filtEnvAmt":0.3,"atk":0.01,"dec":0.4,"sus":0.65,"rel":0.25,"voiceMode":1,"glideMode":2,"glideTime":0.06,"volume":0.85}},
 {"name":"Slide Bass","cat":"Basses","p":{"o1Wave":0,"o1Level":0.8,"o2Wave":1,"o2Level":0.45,"subWave":0,"subLevel":0.45,"cutoff":1100,"reso":0.18,"filtEnvAmt":0.35,"fdec":0.2,"atk":0.01,"dec":0.35,"sus":0.7,"rel":0.2,"velToFilt":0.45,"velToAmp":0.35,"voiceMode":2,"glideMode":2,"glideTime":0.14,"volume":0.85}},

 {"name":"Fast Brass","cat":"Keys","p":{"o1Wave":0,"o1Level":0.85,"o2Wave":0,"o2Level":0.65,"o2Detune":-6,"subLevel":0.2,"cutoff":2200,"reso":0.12,"filtEnvAmt":0.5,"fatk":0.02,"fdec":0.25,"fsus":0.6,"frel":0.2,"atk":0.02,"dec":0.3,"sus":0.8,"rel":0.25,"velToAmp":0.4,"velToFilt":0.5,"choMode":1,"choMix":0.25,"volume":0.8}},
 {"name":"Organ-ish","cat":"Keys","p":{"o1Wave":2,"o1Level":0.8,"o2Wave":0,"o2Level":0.5,"o2Oct":1,"subWave":0,"subLevel":0.6,"cutoff":8000,"reso":0.05,"filtEnvAmt":0.0,"atk":0.005,"dec":0.1,"sus":1.0,"rel":0.06,"velToAmp":0.0,"choMode":1,"choMix":0.3,"revMix":0.15,"volume":0.75}},
 {"name":"Pluck","cat":"Keys","p":{"o1Wave":0,"o1Level":0.85,"o2Wave":2,"o2Level":0.4,"subLevel":0.15,"cutoff":3500,"reso":0.3,"filtEnvAmt":0.7,"fatk":0.003,"fdec":0.12,"fsus":0.0,"frel":0.08,"atk":0.003,"dec":0.25,"sus":0.0,"rel":0.15,"velToAmp":0.5,"velToFilt":0.55,"choMode":0,"revMix":0.18,"volume":0.8}},
 {"name":"Arp Stab","cat":"Keys","p":{"o1Wave":1,"o1Level":0.8,"o2Wave":0,"o2Level":0.5,"o2Detune":7,"subLevel":0.2,"unison":1,"uniDetune":8,"uniWidth":0.4,"cutoff":2600,"reso":0.4,"filtEnvAmt":0.65,"fdec":0.1,"fsus":0.0,"atk":0.004,"dec":0.12,"sus":0.0,"rel":0.1,"velToAmp":0.4,"velToFilt":0.5,"choMode":1,"choMix":0.3,"dlyMix":0.3,"dlyTime":0.22,"dlyFb":0.45,"volume":0.75}},
 {"name":"Perc Noise","cat":"Keys","p":{"o1Wave":3,"o1Level":0.7,"o2Wave":0,"o2Level":0.3,"subLevel":0.2,"cutoff":5000,"reso":0.35,"filtEnvAmt":0.8,"fatk":0.002,"fdec":0.08,"fsus":0.0,"atk":0.002,"dec":0.09,"sus":0.0,"rel":0.08,"velToAmp":0.6,"velToFilt":0.6,"choMode":0,"revMix":0.2,"volume":0.7}}
])PRESETJSON";
};
