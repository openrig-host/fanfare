#pragma once

// CoplanDX note voice.
//
// Derived from msfa's Dx7Note (Apache 2.0, Copyright 2012 Google Inc. /
// 2016-2017 Pascal Gauthier, as used in Dexed), minus MTS-ESP microtuning,
// MPE and portamento, and with support for 4 operators (DX21/TX81Z modes)
// and per-operator waveforms (TX81Z mode). See NOTICE.md.
//
// All math below is in msfa's fixed-point format: pitch is Q24 log2 Hz,
// levels/gains are Q24-ish microsteps as documented in msfa/env.h.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/fm_core.h"
#include "msfa/controllers.h"
#include "MsfaIncludeOnce.h"
#include "FmCore4.h"
#include "FmVoiceData.h"

namespace FanfareFm {

inline constexpr int kFbBitDepth = 8;

// measured on a real DX7 (from msfa)
inline const int32_t kCoarseMul[] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622};

inline int32_t oscFreq(int midinote, int mode, int coarse, int fine,
                       int detune) {
  // standard 12-TET: logfreq = (1<<24) * log2(freq)
  int32_t logfreq = 50857777 + midinote * 1398101; // (1<<24)/12 = 1398101.33
  if (mode == 0) {
    const double detuneRatio =
        0.0209 * std::exp(-0.396 * ((double)logfreq / (1 << 24))) / 7.0;
    logfreq += (int32_t)(detuneRatio * logfreq * (detune - 7));
    logfreq += kCoarseMul[coarse & 31];
    if (fine)
      logfreq += (int32_t)std::floor(24204406.323123 * std::log(1 + 0.01 * fine) + 0.5);
  } else {
    // fixed mode: Hz = (coarse&3)*100 + fine, in Q24 log2
    logfreq = (int32_t)((4458616LL * ((coarse & 3) * 100 + fine)) >> 3);
    logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
  }
  return logfreq;
}

inline const uint8_t kVelocityData[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254};

inline int scaleVelocity(int velocity, int sensitivity) {
  const int clamped = velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity);
  const int velValue = kVelocityData[clamped >> 1] - 239;
  return ((sensitivity * velValue + 7) >> 3) << 4;
}

inline int scaleRate(int midinote, int sensitivity) {
  int x = midinote / 3 - 7;
  if (x < 0) x = 0;
  if (x > 31) x = 31;
  return (sensitivity * x) >> 3;
}

inline const uint8_t kExpScaleData[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14,
                                        16, 19, 23, 27, 33, 39, 47, 56, 66, 80,
                                        94, 110, 126, 142, 158, 174, 190, 206,
                                        222, 238, 250};

inline int scaleCurve(int group, int depth, int curve) {
  int scale;
  if (curve == 0 || curve == 3) {
    scale = (group * depth * 329) >> 12;
  } else {
    const int rawExp = kExpScaleData[group < 32 ? group : 31];
    scale = (rawExp * depth * 329) >> 15;
  }
  if (curve < 2)
    scale = -scale;
  return scale;
}

inline int scaleLevel(int midinote, int breakPt, int leftDepth, int rightDepth,
                      int leftCurve, int rightCurve) {
  const int offset = midinote - breakPt - 17;
  if (offset >= 0)
    return scaleCurve((offset + 1) / 3, rightDepth, rightCurve);
  return scaleCurve(-(offset - 1) / 3, leftDepth, leftCurve);
}

inline const uint8_t kPitchModSensTab[] = {0, 10, 20, 33, 55, 92, 153, 255};
inline const uint32_t kAmpModSensTab[] = {0, 4342338, 7171437, 16777216};

// One playing note. numOps is 6 (DX7) or 4 (DX21/TX81Z); for 4-op mode the
// patch op blocks of Yamaha OP1..OP4 (offsets 105,84,63,42) are used and
// `algorithm` must already be a 4-op algorithm index.
struct FmVoice {
  Env env_[6];
  FmOpParams params_[6];
  PitchEnv pitchEnv_;
  int32_t basePitch_[6] = {0};
  int32_t fbBuf_[2] = {0, 0};
  int fbShift_ = 16;
  int32_t ampModSens_[6] = {0};
  int opMode_[6] = {0};
  uint8_t opSwitch_ = 63;
  int algorithm_ = 0;
  int pitchModDepth_ = 0;
  int pitchModSens_ = 0;
  int ampModDepth_ = 0;
  int numOps_ = 6;
  int wave_[6] = {0, 0, 0, 0, 0, 0};
  bool initialised_ = false;
  bool released_ = false;
  int midiNote_ = -1;
  int velocity_ = 0;
  int layerDetuneCents_ = 0;
  int ageSamples_ = 0;

  static void initStatics(double sampleRate) {
    Env::init_sr(sampleRate);
    PitchEnv::init(sampleRate);
    Freqlut::init(sampleRate);
    Exp2::init();
    Sin::init();
    Lfo::init(sampleRate);
    initFmWaves();
  }

  static int opOffset(int op, int numOps) {
    return numOps == 6 ? op * 21 : 42 + op * 21; // 4-op: OP4@42,OP3@63,OP2@84,OP1@105
  }

  void init(const uint8_t patch[kPatchSize], int midinote, int velocity,
            int numOps, int algorithm, int layerDetuneCents) {
    initialised_ = true;
    released_ = false;
    midiNote_ = midinote;
    velocity_ = velocity;
    layerDetuneCents_ = layerDetuneCents;
    numOps_ = numOps;
    algorithm_ = algorithm;
    opSwitch_ = patch[155];
    ageSamples_ = 0;

    const int synthNote = std::clamp(midinote + (int)patch[144] - 24, 0, 127);

    int rates[4], levels[4];
    for (int op = 0; op < numOps_; ++op) {
      const int off = opOffset(op, numOps_);
      for (int i = 0; i < 4; ++i) {
        rates[i] = patch[off + i];
        levels[i] = patch[off + 4 + i];
      }
      int outlevel = Env::scaleoutlevel(patch[off + 16]);
      outlevel += scaleLevel(synthNote, patch[off + 8], patch[off + 9],
                             patch[off + 10], patch[off + 11], patch[off + 12]);
      if (outlevel > 127)
        outlevel = 127;
      outlevel <<= 5;
      outlevel += scaleVelocity(velocity, patch[off + 15]);
      if (outlevel < 0)
        outlevel = 0;
      env_[op].init(rates, levels, outlevel, scaleRate(synthNote, patch[off + 13]));

      const int mode = patch[off + 17];
      const int coarse = patch[off + 18];
      const int fine = patch[off + 19];
      const int detune = patch[off + 20];
      opMode_[op] = mode;
      basePitch_[op] = oscFreq(synthNote, mode, coarse, fine, detune);
      if (layerDetuneCents_ != 0) {
        basePitch_[op] += layerDetuneCents_ * ((1 << 24) / 1200);
      }
      ampModSens_[op] = kAmpModSensTab[patch[off + 14] & 3];
      wave_[op] = patch[156 + op] & 7;
      params_[op].phase = 0;
      params_[op].gain_out = 0;
    }
    for (int i = 0; i < 4; ++i) {
      rates[i] = patch[126 + i];
      levels[i] = patch[130 + i];
    }
    pitchEnv_.set(rates, levels);
    fbShift_ = patch[135] != 0 ? kFbBitDepth - patch[135] : 16;
    pitchModDepth_ = (patch[139] * 165) >> 6;
    pitchModSens_ = kPitchModSensTab[patch[143] & 7];
    ampModDepth_ = (patch[140] * 165) >> 6;
  }

  // Renders one N-sample block, adding into buf. Mirrors Dx7Note::compute.
  void compute(int32_t *buf, int32_t lfoVal, int32_t lfoDelay,
               const Controllers *ctrls, FmCore6W *core6, FmCore4 *core4) {
    // ==== PITCH ====
    const uint32_t pmd = (uint32_t)pitchModDepth_ * (uint32_t)lfoDelay;
    const int32_t senslfo = pitchModSens_ * (lfoVal - (1 << 23));
    int32_t pmod1 = (int32_t)(((int64_t)pmd * (int64_t)senslfo) >> 39);
    if (pmod1 < 0) pmod1 = -pmod1;
    int32_t pmod2 = (int32_t)(((int64_t)ctrls->pitch_mod * (int64_t)senslfo) >> 14);
    if (pmod2 < 0) pmod2 = -pmod2;
    int32_t pitchMod = pmod1 > pmod2 ? pmod1 : pmod2;
    pitchMod = pitchEnv_.getsample() + (pitchMod * (senslfo < 0 ? -1 : 1));

    // ---- pitch bend ----
    int pitchbend = ctrls->pitch_cc;
    int32_t pb = pitchbend - 0x2000;
    if (pb != 0) {
      if (ctrls->pitch_step == 0) {
        if (pb >= 0)
          pb = (int32_t)(((float)(pb << 11)) * ((float)ctrls->pitch_range_up) / 12.0f);
        else
          pb = (int32_t)(((float)(pb << 11)) * ((float)ctrls->pitch_range_dn) / 12.0f);
      } else {
        const int stp = 12 / ctrls->pitch_step;
        pb = pb * stp / 8191;
        pb = (pb * (8191 / stp)) << 11;
      }
    }
    pitchMod += pb + ctrls->masterTune;

    // ==== AMP MOD ====
    const int32_t lfoValInv = (1 << 24) - lfoVal;
    uint32_t amod1 = (uint32_t)(((int64_t)ampModDepth_ * (int64_t)lfoDelay) >> 8);
    amod1 = (uint32_t)(((int64_t)amod1 * (int64_t)lfoValInv) >> 24);
    uint32_t amod2 = (uint32_t)(((int64_t)ctrls->amp_mod * (int64_t)lfoValInv) >> 7);
    uint32_t amdMod = amod1 > amod2 ? amod1 : amod2;
    const uint32_t amod3 = (uint32_t)(ctrls->eg_mod + 1) << 17;
    amdMod = (1u << 24) - amod3 > amdMod ? (1u << 24) - amod3 : amdMod;

    // ==== OP RENDER ====
    for (int op = 0; op < numOps_; ++op) {
      if (!(opSwitch_ & (1u << op))) {
        env_[op].getsample(); // keep envelopes advancing
        params_[op].level_in = 0;
        continue;
      }
      params_[op].freq = Freqlut::lookup(basePitch_[op] + pitchMod);
      int32_t level = env_[op].getsample();
      if (ampModSens_[op] != 0) {
        const uint32_t sensamp =
            (uint32_t)(((uint64_t)amdMod * (uint64_t)ampModSens_[op]) >> 24);
        const uint32_t pt = (uint32_t)std::exp(((float)sensamp) / 262144.0f * 0.07f + 12.2f);
        const uint32_t ldiff = (uint32_t)(((uint64_t)level * ((uint64_t)pt << 4)) >> 28);
        level -= (int32_t)ldiff;
      }
      params_[op].level_in = level;
    }
    if (numOps_ == 6)
      core6->renderW(buf, params_, algorithm_, fbBuf_, fbShift_, wave_);
    else
      core4->renderW(buf, params_, algorithm_, fbBuf_, fbShift_, wave_);
    ageSamples_ += N;
  }

  // Live parameter update for a playing voice (no envelope retrigger).
  // Mirrors Dx7Note::update.
  void update(const uint8_t patch[kPatchSize], int numOps, int algorithm) {
    numOps_ = numOps;
    algorithm_ = algorithm;
    opSwitch_ = patch[155];
    const int synthNote = std::clamp(midiNote_ + (int)patch[144] - 24, 0, 127);
    int rates[4], levels[4];
    for (int op = 0; op < numOps_; ++op) {
      const int off = opOffset(op, numOps_);
      for (int i = 0; i < 4; ++i) {
        rates[i] = patch[off + i];
        levels[i] = patch[off + 4 + i];
      }
      int outlevel = Env::scaleoutlevel(patch[off + 16]);
      outlevel += scaleLevel(synthNote, patch[off + 8], patch[off + 9],
                             patch[off + 10], patch[off + 11], patch[off + 12]);
      if (outlevel > 127)
        outlevel = 127;
      outlevel <<= 5;
      outlevel += scaleVelocity(velocity_, patch[off + 15]);
      if (outlevel < 0)
        outlevel = 0;
      env_[op].update(rates, levels, outlevel, scaleRate(synthNote, patch[off + 13]));

      opMode_[op] = patch[off + 17];
      basePitch_[op] = oscFreq(synthNote, patch[off + 17], patch[off + 18],
                               patch[off + 19], patch[off + 20]);
      if (layerDetuneCents_ != 0)
        basePitch_[op] += layerDetuneCents_ * ((1 << 24) / 1200);
      ampModSens_[op] = kAmpModSensTab[patch[off + 14] & 3];
      wave_[op] = patch[156 + op] & 7;
    }
    for (int i = 0; i < 4; ++i) {
      rates[i] = patch[126 + i];
      levels[i] = patch[130 + i];
    }
    pitchEnv_.set(rates, levels);
    fbShift_ = patch[135] != 0 ? kFbBitDepth - patch[135] : 16;
    pitchModDepth_ = (patch[139] * 165) >> 6;
    pitchModSens_ = kPitchModSensTab[patch[143] & 7];
    ampModDepth_ = (patch[140] * 165) >> 6;
  }

  void keyup() {
    released_ = true;
    for (int op = 0; op < 6; ++op)
      env_[op].keydown(false);
    pitchEnv_.keydown(false);
  }

  bool isPlaying(int numOps) {
    if (!initialised_)
      return false;
    for (int i = 0; i < numOps; ++i) {
      const bool carrier = numOps == 6 ? FmCore6W::isAudioCarrier(algorithm_, i)
                                       : FmCore4::isCarrier(algorithm_, i);
      if (carrier && env_[i].isActive())
        return true;
    }
    return false;
  }
};

} // namespace FanfareFm
