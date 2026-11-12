#pragma once

// CoplanDX engine: polyphonic 6-op / 4-op FM on top of the vendored msfa
// core, with the rack modes:
//   Dx7    - classic DX7, 6 operators, 32 algorithms, 16 voices
//   Dx21   - classic DX21/DX100, 4 operators, 8 algorithms, 16 voices
//   Dual4  - DX21-style 2x4-op layer (detuned), 8 notes (16 voices)
//   Dual6  - 2x6-op layer (detuned), 8 notes
//   Tx81z  - TX81Z, 4 operators, 13 algorithms, 8 operator waveforms
//
// Pure C++ (no JUCE) so it can be unit-tested standalone; the JUCE wrapper is
// FmSynthProcessor.h.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include "msfa/controllers.h"
#include "MsfaIncludeOnce.h"
#include "FmVoice.h"
#include "FmCore4.h"
#include "FmVoiceData.h"

namespace FanfareFm {

enum class FmMode : int {
  Dx7 = 0,
  Dx21 = 1,
  Dual4 = 2,
  Dual6 = 3,
  Tx81z = 4,
};

inline const char *fmModeName(FmMode m) {
  switch (m) {
  case FmMode::Dx7: return "DX7";
  case FmMode::Dx21: return "DX21";
  case FmMode::Dual4: return "DUAL 2x4";
  case FmMode::Dual6: return "DUAL 2x6";
  case FmMode::Tx81z: return "TX81Z";
  }
  return "?";
}

inline bool fmModeIsDual(FmMode m) { return m == FmMode::Dual4 || m == FmMode::Dual6; }
inline int fmModeNumOps(FmMode m) {
  return (m == FmMode::Dx21 || m == FmMode::Tx81z || m == FmMode::Dual4) ? 4 : 6;
}
inline int fmModeMaxAlg(FmMode m) {
  return (m == FmMode::Dx21 || m == FmMode::Dual4) ? 8 : (m == FmMode::Tx81z ? 13 : 32);
}

class FmEngine {
public:
  static constexpr int kMaxVoices = 16;
  static constexpr int kDualLayerDetuneCents = 7;

  FmEngine() { resetPatch(); }

  void prepare(double sampleRate) {
    sr_ = sampleRate;
    FmVoice::initStatics(sampleRate);
    ctrls_ = Controllers();
    ctrls_.core = &core6_;
    ctrls_.pitch_cc = 0x2000;
    ctrls_.pitch_range_up = 2;
    ctrls_.pitch_range_dn = 2;
    ctrls_.pitch_step = 0;
    ctrls_.masterTune = 0;
    ctrls_.mpeEnabled = false;
    // mod wheel (CC1) drives pitch + amp like the DX7's default assignment
    ctrls_.modwheel_cc = 0;
    ctrls_.wheel.range = 127;
    ctrls_.wheel.pitch = true;
    ctrls_.wheel.amp = true;
    ctrls_.wheel.eg = false;
    for (auto &v : voices_)
      v = FmVoice();
  }

  void reset() {
    for (auto &v : voices_)
      v = FmVoice();
    lfo_.reset(patch_.d + 137);
  }

  void setMode(FmMode m) {
    // the algorithm byte is stored in the current mode's domain; convert on
    // mode switch (DX7 0..31 <-> 4-op 0..12)
    if (fmModeNumOps(m) != numOps()) {
      if (fmModeNumOps(m) == 4)
        patch_.d[134] = (uint8_t)dx7AlgTo4op(patch_.d[134]);
      else
        patch_.d[134] = (uint8_t)alg4opToDx7(patch_.d[134]);
    }
    mode_ = m;
    allNotesOff();
  }
  FmMode mode() const { return mode_; }

  int polyphony() const { return fmModeIsDual(mode_) ? kMaxVoices / 2 : kMaxVoices; }
  int numOps() const { return fmModeNumOps(mode_); }

  void setPatch(const FmPatch &p) {
    patch_ = p;
    if (patch_.d[134] >= fmModeMaxAlg(mode_))
      patch_.d[134] = (uint8_t)(fmModeMaxAlg(mode_) - 1);
    lfo_.reset(patch_.d + 137);
    // live-update playing voices without retriggering envelopes
    const int nOps = numOps();
    const int alg = effectiveAlg();
    for (auto &v : voices_)
      if (v.initialised_ && v.isPlaying(nOps))
        v.update(patch_.d, nOps, alg);
  }

  FmPatch &patch() { return patch_; }
  const FmPatch &patch() const { return patch_; }

  void resetPatch() {
    initPatch(patch_);
    lfo_.reset(patch_.d + 137);
  }

  // Load a raw DX7-domain patch (e.g. from sysex); converts the algorithm
  // into the current mode's domain.
  void loadPatchRaw(const FmPatch &p) {
    FmPatch q = p;
    if (numOps() == 4)
      q.d[134] = (uint8_t)dx7AlgTo4op(q.d[134]);
    setPatch(q);
  }

  int effectiveAlg() const {
    const int mx = fmModeMaxAlg(mode_);
    const int a = patch_.d[134];
    return a >= mx ? mx - 1 : a;
  }

  void noteOn(int note, int velocity) {
    const int nOps = numOps();
    const int alg = effectiveAlg();
    if (fmModeIsDual(mode_)) {
      // claim two voices per note: layer A centered, layer B detuned
      FmVoice *a = allocVoice();
      FmVoice *b = allocVoice();
      if (a == nullptr)
        return;
      a->init(patch_.d, note, velocity, nOps, alg, 0);
      if (b != nullptr)
        b->init(patch_.d, note, velocity, nOps, alg, kDualLayerDetuneCents);
      else
        a->init(patch_.d, note, velocity, nOps, alg, 0); // monophonic fallback
    } else {
      FmVoice *v = allocVoice();
      if (v != nullptr)
        v->init(patch_.d, note, velocity, nOps, alg, 0);
    }
    if (patch_.d[141] & 1) // LFO key sync
      lfo_.keydown();
    if (patch_.d[136] & 1) { // osc key sync: reset phases
      for (auto &v : voices_)
        if (v.initialised_)
          for (int op = 0; op < 6; ++op)
            v.params_[op].phase = 0;
    }
  }

  void noteOff(int note) {
    const int nOps = numOps();
    for (auto &v : voices_) {
      if (v.initialised_ && v.midiNote_ == note && !v.released_)
        v.keyup();
    }
    (void)nOps;
  }

  void allNotesOff() {
    for (auto &v : voices_)
      if (v.initialised_ && !v.released_)
        v.keyup();
  }

  void panic() {
    for (auto &v : voices_)
      v = FmVoice();
  }

  int activeVoices() {
    int n = 0;
    const int nOps = numOps();
    for (auto &v : voices_)
      if (v.initialised_ && v.isPlaying(nOps))
        ++n;
    return n;
  }

  void setPitchBend(int value14bit) { ctrls_.pitch_cc = value14bit; }

  // MPE configuration switch (msfa controllers.h FIXME: "make this
  // switchable"). The vendored msfa has no per-channel MIDI decode layer, so
  // this only flips the Controllers flag + bend range; true per-note MPE
  // pitch tracking is future work.
  void setMpeEnabled(bool enabled, int pitchBendRangeSemitones = 24) {
    ctrls_.mpeEnabled = enabled;
    ctrls_.mpePitchBendRange =
        pitchBendRangeSemitones < 1 ? 1 : (pitchBendRangeSemitones > 96 ? 96 : pitchBendRangeSemitones);
  }
  bool isMpeEnabled() const { return ctrls_.mpeEnabled; }
  int mpePitchBendRange() const { return ctrls_.mpePitchBendRange; }
  void setModWheel(int cc7bit) {
    ctrls_.modwheel_cc = cc7bit;
    ctrls_.refresh();
  }
  void setMasterTuneCents(int cents) {
    ctrls_.masterTune = (int32_t)((int64_t)cents * ((1 << 24) / 1200));
  }

  // Render numSamples into out (interleaved stereo handled by caller; this
  // fills a mono int32 buffer of numSamples). Call with multiples of N=64.
  void render(int32_t *out, int numSamples) {
    const int nOps = numOps();
    ctrls_.refresh();
    int done = 0;
    while (done < numSamples) {
      const int32_t lfoVal = lfo_.getsample();
      const int32_t lfoDelay = lfo_.getdelay();
      memset(renderBuf_, 0, sizeof(int32_t) * N);
      for (auto &v : voices_) {
        if (!v.initialised_)
          continue;
        if (v.isPlaying(nOps)) {
          v.compute(renderBuf_, lfoVal, lfoDelay, &ctrls_, &core6_, &core4_);
        } else {
          // release tail (or fully finished env): keep rendering until the
          // envelopes go idle, then free the voice
          v.compute(renderBuf_, lfoVal, lfoDelay, &ctrls_, &core6_, &core4_);
          if (!v.isPlaying(nOps))
            v = FmVoice();
        }
      }
      const int n = std::min(N, numSamples - done);
      for (int i = 0; i < n; ++i)
        out[done + i] = renderBuf_[i];
      done += n;
    }
  }

  // Calibrated across all 558 factory DX7/DX100/TX81Z multi-carrier voices:
  // a single carrier reaches ~2.0..3.67 in Q24 units and multi-carrier patches
  // sum up to ~6..8 per note (~16..24 on a 4-note chord). kVoiceScale = 0.038f
  // places 4-note chords cleanly around -8 to -5 dBFS with zero saturation.
  static constexpr float kVoiceScale = 0.038f;
  static constexpr float kDualLayerTrim = 0.70710678f;
  static constexpr float kOutScale = kVoiceScale / (float)(1 << 24);

  float outScaleForCurrentMode() const noexcept {
    return fmModeIsDual(mode_) ? (kOutScale * kDualLayerTrim) : kOutScale;
  }

private:
  FmVoice *allocVoice() {
    const int nOps = numOps();
    // 1) free voice
    for (auto &v : voices_)
      if (!v.initialised_)
        return &v;
    // 2) released voice that finished
    for (auto &v : voices_)
      if (v.released_ && !v.isPlaying(nOps)) {
        v = FmVoice();
        return &v;
      }
    // 3) steal the oldest released voice
    FmVoice *oldest = nullptr;
    for (auto &v : voices_)
      if (v.released_ && (oldest == nullptr || v.ageSamples_ < oldest->ageSamples_))
        oldest = &v;
    if (oldest) {
      *oldest = FmVoice();
      return oldest;
    }
    // 4) steal oldest playing voice
    for (auto &v : voices_)
      if (oldest == nullptr || v.ageSamples_ < oldest->ageSamples_)
        oldest = &v;
    if (oldest)
      *oldest = FmVoice();
    return oldest;
  }

  double sr_ = 44100.0;
  FmMode mode_ = FmMode::Dx7;
  FmPatch patch_{};
  FmVoice voices_[kMaxVoices];
  FmCore6W core6_;
  FmCore4 core4_;
  Lfo lfo_;
  Controllers ctrls_;
  int32_t renderBuf_[N];
};

} // namespace FanfareFm
