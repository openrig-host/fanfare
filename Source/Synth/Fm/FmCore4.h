#pragma once

// 4-operator and 6-operator waveform-aware FM cores for CoplanDX.
//
// Internal 4-op index 0 is Yamaha "OP4" (deepest modulator / feedback op),
// index 3 is "OP1" (carrier), matching msfa's top-down convention.

#include "msfa/fm_core.h"
#include "msfa/fm_op_kernel.h"
#include "MsfaIncludeOnce.h"
#include "FmWaves.h"

namespace FanfareFm {

struct FmAlgAccess : FmCore {
  static int opFlags6(int alg, int op) {
    return algorithms[alg & 31].ops[op & 7];
  }
};

// 6-operator core with optional per-operator TX81Z waveforms (W1..W8).
// Delegates directly to msfa's FmCore::render when all 6 operators use W1 (sine).
class FmCore6W : public FmCore {
public:
  static bool isAudioCarrier(int algorithm, int op) {
    return (algorithms[algorithm & 31].ops[op] & 7) == OUT_BUS_ADD;
  }

  void renderW(int32_t *output, FmOpParams *params, int algorithm,
               int32_t *fb_buf, int feedback_shift, const int *waves) {
    if (waves == nullptr ||
        ((waves[0] | waves[1] | waves[2] | waves[3] | waves[4] | waves[5]) == 0)) {
      FmCore::render(output, params, algorithm, fb_buf, feedback_shift);
      return;
    }

    constexpr int kLevelThresh = 1120;
    const FmAlgorithm &alg = algorithms[algorithm & 31];
    bool hasContents[3] = {true, false, false};
    for (int op = 0; op < 6; ++op) {
      const int flags = alg.ops[op];
      bool add = (flags & OUT_BUS_ADD) != 0;
      FmOpParams &param = params[op];
      const int inbus = (flags >> 4) & 3;
      const int outbus = flags & 3;
      int32_t *outptr = (outbus == 0) ? output : buf_[outbus - 1].get();
      const int32_t gain1 = param.gain_out;
      const int32_t gain2 = Exp2::lookup(param.level_in - (14 * (1 << 24)));
      param.gain_out = gain2;
      const int wave = waves[op] & 7;

      if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
        if (!hasContents[outbus])
          add = false;
        if (inbus == 0 || !hasContents[inbus]) {
          if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
            FmOpKernelW::compute_fb(outptr, param.phase, param.freq, gain1,
                                    gain2, fb_buf, feedback_shift, add, wave);
          } else {
            FmOpKernelW::compute_pure(outptr, param.phase, param.freq, gain1,
                                      gain2, add, wave);
          }
        } else {
          FmOpKernelW::compute(outptr, buf_[inbus - 1].get(), param.phase,
                               param.freq, gain1, gain2, add, wave);
        }
        hasContents[outbus] = true;
      } else if (!add) {
        hasContents[outbus] = false;
      }
      param.phase += param.freq << LG_N;
    }
  }
};

class FmCore4 : public FmCore {
public:
  static constexpr int kNumAlgorithms = 13;

  // Flags per op in internal order OP4, OP3, OP2, OP1.
  static const int kAlgorithms[kNumAlgorithms][4];

  static bool isCarrier(int algorithm, int op) {
    const int idx = (algorithm >= 0 && algorithm < kNumAlgorithms) ? algorithm : 0;
    return (kAlgorithms[idx][op] & 7) == OUT_BUS_ADD;
  }

  void renderW(int32_t *output, FmOpParams *params, int algorithm,
               int32_t *fb_buf, int feedback_shift, const int *waves) {
    constexpr int kLevelThresh = 1120;
    const int *alg = kAlgorithms[(algorithm >= 0 && algorithm < kNumAlgorithms)
                                     ? algorithm
                                     : 0];
    bool hasContents[3] = {true, false, false};
    for (int op = 0; op < 4; op++) {
      const int flags = alg[op];
      bool add = (flags & OUT_BUS_ADD) != 0;
      FmOpParams &param = params[op];
      const int inbus = (flags >> 4) & 3;
      const int outbus = flags & 3;
      int32_t *outptr = (outbus == 0) ? output : buf_[outbus - 1].get();
      const int32_t gain1 = param.gain_out;
      const int32_t gain2 = Exp2::lookup(param.level_in - (14 * (1 << 24)));
      param.gain_out = gain2;
      const int wave = waves != nullptr ? (waves[op] & 7) : 0;

      if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
        if (!hasContents[outbus])
          add = false;
        if (inbus == 0 || !hasContents[inbus]) {
          if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
            FmOpKernelW::compute_fb(outptr, param.phase, param.freq, gain1,
                                    gain2, fb_buf, feedback_shift, add, wave);
          } else {
            FmOpKernelW::compute_pure(outptr, param.phase, param.freq, gain1,
                                      gain2, add, wave);
          }
        } else {
          FmOpKernelW::compute(outptr, buf_[inbus - 1].get(), param.phase,
                               param.freq, gain1, gain2, add, wave);
        }
        hasContents[outbus] = true;
      } else if (!add) {
        hasContents[outbus] = false;
      }
      param.phase += param.freq << LG_N;
    }
  }

  void render(int32_t *output, FmOpParams *params, int algorithm,
              int32_t *fb_buf, int feedback_shift) override {
    renderW(output, params, algorithm, fb_buf, feedback_shift, nullptr);
  }
};

inline const int FmCore4::kAlgorithms[FmCore4::kNumAlgorithms][4] = {
    {0xc1, 0x11, 0x11, 0x14}, // 1: 4->3->2->1
    {0xc2, 0x06, 0x21, 0x14}, // 2: (4+3)->2->1
    {0xc1, 0x02, 0x25, 0x14}, // 3: (4+(3->2))->1
    {0xc2, 0x21, 0x05, 0x14}, // 4: ((4->3)+2)->1
    {0xc2, 0x24, 0x01, 0x14}, // 5: (4->3)+(2->1)
    {0xc2, 0x24, 0x24, 0x24}, // 6: (4->3)+(4->2)+(4->1)
    {0xc2, 0x24, 0x04, 0x04}, // 7: (4->3)+2+1
    {0xc4, 0x04, 0x04, 0x04}, // 8: 4+3+2+1
    {0xc1, 0x00, 0x01, 0x14}, // 9: 2->1
    {0x00, 0x00, 0x00, 0x04}, // 10: 1 only
    {0xc2, 0x00, 0x21, 0x14}, // 11: 4->2->1
    {0xc2, 0x00, 0x21, 0x14}, // 12: (4->4)->2->1
    {0xc2, 0x06, 0x21, 0x14}, // 13: (4+3)->2->1
};

} // namespace FanfareFm
