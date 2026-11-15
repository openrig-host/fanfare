#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace B3 {

// Always-Dual Stereo Leslie 122 Rotary Speaker + Baked-In Wood Room:
// - Two asynchronous Leslie 122 cabinets (Cab A + Cab B with natural AC motor
//   and belt-inertia tolerances) spread across a wide stereo stage
// - LR4 800 Hz crossover + Jensen V21 horn throat resonance (~1.25 kHz) +
//   4.7 kHz phenolic rolloff
// - 4-point Catmull-Rom cubic Hermite Doppler + dynamic off-axis dispersion +
//   wooden louvre reflections
// - Baked-in Wood Room (6-tap stereo early reflections 4.5..31 ms + tight
//   warm ~0.35s wooden room decay with ~2.4 kHz wood absorption) controlled by
//   `roomMix` (0 = dry close-miked speakers, 1 = lush studio wood room)
class Leslie122 {
public:
  enum Mode : int { Slow = 0, Brake, Fast };
  enum CabinetType : int {
    Cab122Closed = 0,
    Cab122Open,
    Cab147Closed,
    Cab147Open
  };

  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const float srF = (float)sr;

    // 800 Hz LR4 Crossover
    {
      const double w = kTwoPi * 800.0 / sr;
      const double cs = std::cos(w);
      const double sn = std::sin(w);
      const double q = 0.7071067811865475244;
      const double alpha = sn / (2.0 * q);
      {
        const double b0 = (1.0 - cs) * 0.5, b1 = 1.0 - cs, b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
        lpB0 = (float)(b0 / a0); lpB1 = (float)(b1 / a0); lpB2 = (float)(b2 / a0);
        lpA1 = (float)(a1 / a0); lpA2 = (float)(a2 / a0);
      }
      {
        const double b0 = (1.0 + cs) * 0.5, b1 = -(1.0 + cs), b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
        hpB0 = (float)(b0 / a0); hpB1 = (float)(b1 / a0); hpB2 = (float)(b2 / a0);
        hpA1 = (float)(a1 / a0); hpA2 = (float)(a2 / a0);
      }
    }

    reflLpCoef = 1.0f - std::exp(-(float)kTwoPi * std::min(2400.0f, srF * 0.40f) / srF);
    applyCabinetVoicing();

    // Power-of-2 Doppler buffers (>= 12 ms)
    const int minSize = (int)(sr * 0.012) + 64;
    int pow2 = 128;
    while (pow2 < minSize)
      pow2 <<= 1;
    dlySize = pow2;
    dlyMask = pow2 - 1;
    hornBuf.assign((size_t)dlySize, 0.0f);
    drumBuf.assign((size_t)dlySize, 0.0f);

    // Baked-in Wood Room buffers: 6-tap early reflection line + 4 tight wood combs
    const int woodSize = (int)(sr * 0.045) + 32;
    int wPow2 = 256;
    while (wPow2 < woodSize)
      wPow2 <<= 1;
    woodBufSize = wPow2;
    woodBufMask = wPow2 - 1;
    woodErBufL.assign((size_t)woodBufSize, 0.0f);
    woodErBufR.assign((size_t)woodBufSize, 0.0f);

    // Prime wood-room early reflection taps (ms): floor, ceiling, walls
    static constexpr float kErMsL[6] = {4.7f, 9.1f, 13.8f, 19.3f, 24.6f, 31.2f};
    static constexpr float kErMsR[6] = {5.3f, 8.4f, 15.1f, 18.2f, 26.1f, 29.7f};
    for (int i = 0; i < 6; ++i) {
      erTapL[i] = std::max(1, (int)std::lround(kErMsL[i] * 0.001f * srF));
      erTapR[i] = std::max(1, (int)std::lround(kErMsR[i] * 0.001f * srF));
    }

    // Tight ~0.35s Wood Room tail (shorter & warmer than hall reverb)
    const double scale = sr / 44100.0;
    static constexpr int kWoodCombL[4] = {677, 751, 829, 911};
    static constexpr int kWoodCombR[4] = {691, 769, 853, 929};
    for (int i = 0; i < 4; ++i) {
      woodCombLenL[i] = std::max(24, (int)std::lround((double)kWoodCombL[i] * scale));
      woodCombLenR[i] = std::max(24, (int)std::lround((double)kWoodCombR[i] * scale));
      woodCombBufL[i].assign((size_t)woodCombLenL[i], 0.0f);
      woodCombBufR[i].assign((size_t)woodCombLenR[i], 0.0f);
    }
    woodDampCoef = 1.0f - std::exp(-(float)kTwoPi * std::min(2350.0f, srF * 0.38f) / srF);
    woodHpCoef = 1.0f - std::exp(-(float)kTwoPi * 125.0f / srF);

    // Chorus (64 ms pow2 buffer), speed-wander, warmth band coefficients
    {
      int cs2 = 256;
      while (cs2 < (int)(sr * 0.064))
        cs2 <<= 1;
      chSize = cs2;
      chMask = cs2 - 1;
      chBufL.assign((size_t)chSize, 0.0f);
      chBufR.assign((size_t)chSize, 0.0f);
      chPos = 0;
      chInc1 = (float)(0.34 / sr);
      chInc2 = (float)(0.53 / sr);
    }
    wanderCoef = 1.0f - std::exp(-(float)kTwoPi * 1.4f / srF);
    lm1Coef = 1.0f - std::exp(-(float)kTwoPi * 230.0f / srF);
    lm2Coef = 1.0f - std::exp(-(float)kTwoPi * 560.0f / srF);
    cabB.rng = 1812433253u;

    setMode((int)mode);
    buildTrigLut();
    reset();
  }

  void reset() {
    cabA.hornAngle = 0.0f;
    cabA.drumAngle = 0.7f;
    cabB.hornAngle = 2.15f;
    cabB.drumAngle = 3.65f;
    updateTargets();
    cabA.hornSpeed = cabA.hornTarget;
    cabA.drumSpeed = cabA.drumTarget;
    cabB.hornSpeed = cabB.hornTarget;
    cabB.drumSpeed = cabB.drumTarget;
    cabA.dispZ[0] = cabA.dispZ[1] = 0.0f;
    cabB.dispZ[0] = cabB.dispZ[1] = 0.0f;
    cabA.reflZ[0] = cabA.reflZ[1] = 0.0f;
    cabB.reflZ[0] = cabB.reflZ[1] = 0.0f;

    writePos = 0;
    woodWritePos = 0;
    lp1Z1 = lp1Z2 = lp2Z1 = lp2Z2 = 0.0f;
    hp1Z1 = hp1Z2 = hp2Z1 = hp2Z2 = 0.0f;
    thZ1 = thZ2 = 0.0f;
    hornLpZ1 = hornLpZ2 = 0.0f;
    cabWarmL = cabWarmR = 0.0f;
    outLpL = outLpR = 0.0f;
    lm1L = lm1R = lm2L = lm2R = 0.0f;
    cabA.drumWallZ[0] = cabA.drumWallZ[1] = 0.0f;
    cabB.drumWallZ[0] = cabB.drumWallZ[1] = 0.0f;
    std::fill(chBufL.begin(), chBufL.end(), 0.0f);
    std::fill(chBufR.begin(), chBufR.end(), 0.0f);
    chPos = 0;
    woodHpL = woodHpR = 0.0f;

    std::fill(hornBuf.begin(), hornBuf.end(), 0.0f);
    std::fill(drumBuf.begin(), drumBuf.end(), 0.0f);
    std::fill(woodErBufL.begin(), woodErBufL.end(), 0.0f);
    std::fill(woodErBufR.begin(), woodErBufR.end(), 0.0f);
    for (int i = 0; i < 4; ++i) {
      std::fill(woodCombBufL[i].begin(), woodCombBufL[i].end(), 0.0f);
      std::fill(woodCombBufR[i].begin(), woodCombBufR[i].end(), 0.0f);
      woodCombPosL[i] = woodCombPosR[i] = 0;
      woodCombLpL[i] = woodCombLpR[i] = 0.0f;
    }
  }

  void setMode(int m) {
    mode = (Mode)((m < 0) ? Slow : (m > Fast ? Fast : m));
    updateTargets();
  }

  void setCabinetType(int t) {
    const CabinetType next = (CabinetType)std::clamp(t, 0, (int)Cab147Open);
    if (next == cabType)
      return;
    cabType = next;
    if (sr > 8000.0)
      applyCabinetVoicing();
  }

  void setBypass(bool b) { bypass = b; }
  void setMicSpread(float v) { micSpread = std::clamp(v, 0.0f, 1.0f); }
  void setRoomMix(float v) { roomMix = std::clamp(v, 0.0f, 1.0f); }
  void setWarmth(float v) { warmth = std::clamp(v, 0.0f, 1.0f); }
  void setChorusMix(float v) { chorusMix = std::clamp(v, 0.0f, 1.0f); }
  void setDualMode(bool) {} // Always Dual Leslie 122

  float getHornAngle() const { return cabA.hornAngle; }
  float getDrumAngle() const { return cabA.drumAngle; }
  float getHornAngleB() const { return cabB.hornAngle; }
  float getDrumAngleB() const { return cabB.drumAngle; }

  void process(float *left, float *right, int n) {
    if (bypass || n <= 0 || dlySize < 64)
      return;

    constexpr float kTwoPi = 6.28318530717958647692f;
    const float srF = (float)sr;

    const float micAngleMag = 0.20f + 1.32f * micSpread;
    const float cosMic = std::cos(micAngleMag);
    const float sinMicL = std::sin(-micAngleMag);
    const float sinMicR = std::sin(+micAngleMag);
    // When micSpread -> 0, collapse rotor L/R action to dead-center mono (while keeping
    // post-rotor Chorus & Wood Room stereo) and add a thicker mono acoustic swish.
    const float monoSwish = 1.0f - micSpread;
    const float monoFold = monoSwish * (1.0f - 0.35f * micSpread);

    const float roomEff = std::min(1.0f, roomMix * roomScale);
    const float drySpeakerGain = 1.0f - 0.36f * roomEff;
    const float woodWetGain = roomEff * 1.15f;
    const float dynCabRes = cabResAmt * (0.25f + 1.55f * warmth);
    const float warmAmt = warmth * 2.15f * warmMul;
    const float warmNorm = 1.0f / (1.0f + 0.16f * warmAmt);

    for (int s = 0; s < n; ++s) {
      advanceCabinet(cabA, kTwoPi);
      advanceCabinet(cabB, kTwoPi);

      const float mono = 0.5f * (left[s] + right[s]);

      // LR4 800 Hz crossover
      const float lp1 = lpB0 * mono + lp1Z1;
      lp1Z1 = lpB1 * mono - lpA1 * lp1 + lp1Z2;
      lp1Z2 = lpB2 * mono - lpA2 * lp1;
      const float lp = lpB0 * lp1 + lp2Z1;
      lp2Z1 = lpB1 * lp1 - lpA1 * lp + lp2Z2;
      lp2Z2 = lpB2 * lp1 - lpA2 * lp;

      const float hp1 = hpB0 * mono + hp1Z1;
      hp1Z1 = hpB1 * mono - hpA1 * hp1 + hp1Z2;
      hp1Z2 = hpB2 * mono - hpA2 * hp1;
      const float hpRaw = hpB0 * hp1 + hp2Z1;
      hp2Z1 = hpB1 * hp1 - hpA1 * hpRaw + hp2Z2;
      hp2Z2 = hpB2 * hp1 - hpA2 * hpRaw;

      // Jensen V21 horn throat bark + phenolic diaphragm rolloff
      const float thOut = thB0 * hpRaw + thZ1;
      thZ1 = thB1 * hpRaw - thA1 * thOut + thZ2;
      thZ2 = thB2 * hpRaw - thA2 * thOut;
      hornLpZ1 += hornLpCoef * (thOut - hornLpZ1);
      hornLpZ2 += hornLpCoef * (hornLpZ1 - hornLpZ2);
      const float hp = hornLpZ2;

      hornBuf[(size_t)writePos] = hp;
      drumBuf[(size_t)writePos] = lp;

      // Always Dual asynchronous Leslie 122 stage
      float aL = 0.0f, aR = 0.0f, bL = 0.0f, bR = 0.0f;
      sampleCabinet(cabA, cosMic, sinMicL, sinMicR, srF, kTwoPi, 0.0f, aL, aR);
      sampleCabinet(cabB, cosMic, sinMicL, sinMicR, srF, kTwoPi,
                    0.00055f * srF, bL, bR);

      if (++writePos >= dlySize)
        writePos = 0;

      const float stageL = (0.78f * aL + 0.22f * aR) * 0.72f +
                           (0.28f * bL + 0.14f * bR) * 0.58f;
      const float stageR = (0.28f * aR + 0.14f * aL) * 0.58f +
                           (0.78f * bR + 0.22f * bL) * 0.72f;

      auto fastSat = [](float x) noexcept {
        const float x2 = x * x;
        return x / (1.0f + 0.33f * x2);
      };
      const float monoRaw = 0.5f * (stageL + stageR);
      const float monoThick = monoRaw + monoSwish * 0.22f * fastSat(monoRaw * 1.45f);
      const float spkL = stageL + monoFold * (monoThick - stageL);
      const float spkR = stageR + monoFold * (monoThick - stageR);

      // Birch cabinet resonance (scales with Warmth)
      cabWarmL += cabCoef * (spkL - cabWarmL);
      cabWarmR += cabCoef * (spkR - cabWarmR);
      float closeL = (spkL + dynCabRes * cabWarmL) * 1.06f;
      float closeR = (spkR + dynCabRes * cabWarmR) * 1.06f;

      // Cabinet/speaker top-end rolloff (voiced per cabinet)
      outLpL += outLpCoef * (closeL - outLpL);
      outLpR += outLpCoef * (closeR - outLpR);
      closeL = outLpL;
      closeR = outLpR;

      // Low-mid warmth: deep 230 Hz body + 560 Hz woody chest + gentle 15" woofer fatness
      lm1L += lm1Coef * (closeL - lm1L);
      lm1R += lm1Coef * (closeR - lm1R);
      lm2L += lm2Coef * (closeL - lm2L);
      lm2R += lm2Coef * (closeR - lm2R);
      const float bodyL = warmAmt * (0.95f * lm1L + 0.70f * (lm2L - lm1L));
      const float bodyR = warmAmt * (0.95f * lm1R + 0.70f * (lm2R - lm1R));
      closeL = (closeL + bodyL + 0.14f * warmth * fastSat(bodyL * 1.25f)) * warmNorm;
      closeR = (closeR + bodyR + 0.14f * warmth * fastSat(bodyR * 1.25f)) * warmNorm;

      // Built-in Dimension-style stereo chorus (cross-coupled, opposing LFOs)
      if (chorusMix > 0.004f && chSize > 0) {
        chBufL[(size_t)chPos] = closeL;
        chBufR[(size_t)chPos] = closeR;
        chPh1 += chInc1; if (chPh1 >= 1.0f) chPh1 -= 1.0f;
        chPh2 += chInc2; if (chPh2 >= 1.0f) chPh2 -= 1.0f;
        const float l1 = std::sin(kTwoPi * chPh1);
        const float l2 = std::sin(kTwoPi * chPh2);
        const float wetL = 0.62f * chRead(chBufL, (0.0105f + 0.0009f * l1) * srF) +
                           0.38f * chRead(chBufR, (0.0178f + 0.0007f * l2) * srF);
        const float wetR = 0.62f * chRead(chBufR, (0.0125f - 0.0009f * l1) * srF) +
                           0.38f * chRead(chBufL, (0.0151f - 0.0007f * l2) * srF);
        closeL = closeL * (1.0f - 0.22f * chorusMix) + wetL * 0.82f * chorusMix;
        closeR = closeR * (1.0f - 0.22f * chorusMix) + wetR * 0.82f * chorusMix;
        if (++chPos >= chSize) chPos = 0;
      }

      // Baked-in Wood Room (Early Reflections + Tight Warm Wooden Decay — stays Stereo even at 0 Spread)
      if (roomEff > 0.004f) {
        woodErBufL[(size_t)woodWritePos] = closeL;
        woodErBufR[(size_t)woodWritePos] = closeR;

        static constexpr float kErGain[6] = {0.36f, 0.29f, 0.23f, 0.18f, 0.14f, 0.10f};
        float erL = 0.0f, erR = 0.0f;
        for (int t = 0; t < 6; ++t) {
          const int idxL = (woodWritePos - erTapL[t]) & woodBufMask;
          const int idxR = (woodWritePos - erTapR[t]) & woodBufMask;
          erL += woodErBufL[(size_t)idxL] * kErGain[t];
          erR += woodErBufR[(size_t)idxR] * kErGain[t];
        }
        if (++woodWritePos >= woodBufSize)
          woodWritePos = 0;

        // Cross-coupled warm wood room tail (~0.42s)
        woodHpL += woodHpCoef * (erL - woodHpL);
        woodHpR += woodHpCoef * (erR - woodHpR);
        const float sendL = (erL - woodHpL) * 0.42f;
        const float sendR = (erR - woodHpR) * 0.42f;

        float tailL = 0.0f, tailR = 0.0f;
        for (int c = 0; c < 4; ++c) {
          const int pL = woodCombPosL[c];
          const float oL = woodCombBufL[c][(size_t)pL];
          woodCombLpL[c] += woodDampCoef * (oL - woodCombLpL[c]);
          woodCombBufL[c][(size_t)pL] = sendL + woodCombLpL[c] * 0.74f;
          woodCombPosL[c] = (pL + 1 >= woodCombLenL[c]) ? 0 : (pL + 1);
          tailL += oL;

          const int pR = woodCombPosR[c];
          const float oR = woodCombBufR[c][(size_t)pR];
          woodCombLpR[c] += woodDampCoef * (oR - woodCombLpR[c]);
          woodCombBufR[c][(size_t)pR] = sendR + woodCombLpR[c] * 0.74f;
          woodCombPosR[c] = (pR + 1 >= woodCombLenR[c]) ? 0 : (pR + 1);
          tailR += oR;
        }

        const float roomL = 0.65f * erL + 0.38f * (0.68f * tailL + 0.32f * tailR);
        const float roomR = 0.65f * erR + 0.38f * (0.68f * tailR + 0.32f * tailL);

        left[s]  = closeL * drySpeakerGain + roomL * woodWetGain;
        right[s] = closeR * drySpeakerGain + roomR * woodWetGain;
      } else {
        left[s]  = closeL;
        right[s] = closeR;
      }
    }
  }

private:
  struct RotorCab {
    float hornAngle = 0.0f, drumAngle = 0.7f;
    float hornSpeed = 0.0f, drumSpeed = 0.0f;
    float hornSlow = 0.0f, hornFast = 0.0f;
    float drumSlow = 0.0f, drumFast = 0.0f;
    float hornTarget = 0.0f, drumTarget = 0.0f;
    float hornAccel = 0.0f, hornDecel = 0.0f;
    float drumAccel = 0.0f, drumDecel = 0.0f;
    float dispZ[2] = {0.0f, 0.0f};
    float reflZ[2] = {0.0f, 0.0f};
    float wanderH = 0.0f, wanderD = 0.0f;
    float drumWallZ[2] = {0.0f, 0.0f};
    uint32_t rng = 2463534242u;
  };

  void updateTargets() {
    switch (mode) {
    case Fast:
      cabA.hornTarget = cabA.hornFast; cabA.drumTarget = cabA.drumFast;
      cabB.hornTarget = cabB.hornFast; cabB.drumTarget = cabB.drumFast;
      break;
    case Brake:
      cabA.hornTarget = 0.0f; cabA.drumTarget = 0.0f;
      cabB.hornTarget = 0.0f; cabB.drumTarget = 0.0f;
      break;
    default:
      cabA.hornTarget = cabA.hornSlow; cabA.drumTarget = cabA.drumSlow;
      cabB.hornTarget = cabB.hornSlow; cabB.drumTarget = cabB.drumSlow;
      break;
    }
  }

  inline void advanceCabinet(RotorCab &c, float kTwoPi) noexcept {
    c.hornSpeed += (c.hornTarget - c.hornSpeed) *
                   (c.hornTarget > c.hornSpeed ? c.hornAccel : c.hornDecel);
    c.drumSpeed += (c.drumTarget - c.drumSpeed) *
                   (c.drumTarget > c.drumSpeed ? c.drumAccel : c.drumDecel);

    // Natural motor/belt/bearing speed wander (non-periodic, gives living movement)
    c.rng ^= c.rng << 13; c.rng ^= c.rng >> 17; c.rng ^= c.rng << 5;
    const float nzH = (float)(c.rng & 0xFFFFFFu) * (2.0f / 16777216.0f) - 1.0f;
    c.rng ^= c.rng << 13; c.rng ^= c.rng >> 17; c.rng ^= c.rng << 5;
    const float nzD = (float)(c.rng & 0xFFFFFFu) * (2.0f / 16777216.0f) - 1.0f;
    c.wanderH += wanderCoef * (nzH - c.wanderH);
    c.wanderD += wanderCoef * (nzD - c.wanderD);

    c.hornAngle += c.hornSpeed * (1.0f + 0.7f * c.wanderH);
    c.drumAngle += c.drumSpeed * (1.0f + 0.6f * c.wanderD);
    if (c.hornAngle >= kTwoPi) c.hornAngle -= kTwoPi;
    if (c.drumAngle >= kTwoPi) c.drumAngle -= kTwoPi;
  }

  inline void sampleCabinet(RotorCab &c, float cosMic, float sinMicL,
                            float sinMicR, float srF, float kTwoPi,
                            float baseOffsetSamples, float &outL,
                            float &outR) noexcept {
    float cosHA, sinHA, cosDA, sinDA;
    fastCosSin(c.hornAngle, cosHA, sinHA);
    fastCosSin(kTwoPi - c.drumAngle, cosDA, sinDA);

    float micOut[2] = {0.0f, 0.0f};
    for (int mi = 0; mi < 2; ++mi) {
      const float sinMic = (mi == 0) ? sinMicL : sinMicR;
      const float cosH = cosHA * cosMic - sinHA * sinMic;
      const float sinH = sinHA * cosMic + cosHA * sinMic;

      const float hDly = baseOffsetSamples + (hornDopp + 0.0005f + hornDopp * sinH) * srF;
      const float rDly1 = baseOffsetSamples + (0.00195f - 0.00024f * sinH) * srF;
      const float rDly2 = baseOffsetSamples + (0.00290f - 0.00017f * cosH) * srF;
      const float rDly3 = baseOffsetSamples + (0.00430f - 0.00066f * cosH) * srF;

      const float hornDirect = readCubic(hornBuf.data(), writePos, hDly);
      const float hornRear = (rearGain > 0.0f)
                                 ? rearGain * readCubic(hornBuf.data(), writePos, rDly3)
                                 : 0.0f;
      const float hornWall = 0.65f * readCubic(hornBuf.data(), writePos, rDly1) +
                             0.35f * readCubic(hornBuf.data(), writePos, rDly2) - hornRear;

      const float normFace = 0.5f + 0.5f * cosH;
      const float shapedFace = normFace * (0.45f + 0.55f * normFace);
      const float dispCoef = dispMinCoef + dispSpanCoef * shapedFace;
      c.dispZ[mi] += dispCoef * (hornDirect - c.dispZ[mi]);
      const float hornDispersed = hornDirectMix * hornDirect + (1.0f - hornDirectMix) * c.dispZ[mi];

      c.reflZ[mi] += reflLpCoef * (hornWall - c.reflZ[mi]);
      const float hornLobe = hornLobeBase + hornLobeDepth * shapedFace;
      const float hornSig = (hornDispersed * hornLobe + c.reflZ[mi] * reflMix) * hornGain;

      const float cosD = cosDA * cosMic - sinDA * sinMic;
      const float sinD = sinDA * cosMic + cosDA * sinMic;
      const float dDly = baseOffsetSamples + (0.00090f + 0.00027f * sinD) * srF;
      const float drumDirect = readCubic(drumBuf.data(), writePos, dDly);
      const float dwDly = baseOffsetSamples + (0.00340f - 0.00042f * cosD) * srF;
      const float drumWall = readCubic(drumBuf.data(), writePos, dwDly);
      c.drumWallZ[mi] += reflLpCoef * (drumWall - c.drumWallZ[mi]);
      const float drumLobe = drumLobeBase + drumLobeDepth * (0.5f + 0.5f * cosD);
      const float drumSig = drumDirect * drumLobe + c.drumWallZ[mi] * drumWallGain;

      micOut[mi] = hornSig + drumSig * drumGain;
    }
    outL = micOut[0];
    outR = micOut[1];
  }

  void applyCabinetVoicing() {
    constexpr double kTwoPi = 6.28318530717958647692;
    const float srF = (float)sr;
    const bool is147 = (cabType == Cab147Closed || cabType == Cab147Open);
    const bool isOpen = (cabType == Cab122Open || cabType == Cab147Open);

    // 122 = classic warm studio/jazz speeds; 147 = slightly brisker rock/gospel motor
    const double hSlowA = is147 ? 51.0 : 48.0;
    const double hFastA = is147 ? 420.0 : 402.0;
    const double dSlowA = is147 ? 42.0 : 40.0;
    const double dFastA = is147 ? 355.0 : 342.0;

    cabA.hornSlow = rpmToRadPerSample(hSlowA);
    cabA.hornFast = rpmToRadPerSample(hFastA);
    cabA.drumSlow = rpmToRadPerSample(dSlowA);
    cabA.drumFast = rpmToRadPerSample(dFastA);
    cabA.hornAccel = 1.0f - std::exp(-1.0f / ((is147 ? 0.56f : 0.62f) * srF));
    cabA.hornDecel = 1.0f - std::exp(-1.0f / ((is147 ? 1.24f : 1.32f) * srF));
    cabA.drumAccel = 1.0f - std::exp(-1.0f / ((is147 ? 2.55f : 2.75f) * srF));
    cabA.drumDecel = 1.0f - std::exp(-1.0f / ((is147 ? 3.85f : 4.05f) * srF));

    cabB.hornSlow = rpmToRadPerSample(hSlowA + 3.5);
    cabB.hornFast = rpmToRadPerSample(hFastA - 13.0);
    cabB.drumSlow = rpmToRadPerSample(dSlowA - 2.5);
    cabB.drumFast = rpmToRadPerSample(dFastA + 9.0);
    cabB.hornAccel = 1.0f - std::exp(-1.0f / ((is147 ? 0.72f : 0.78f) * srF));
    cabB.hornDecel = 1.0f - std::exp(-1.0f / ((is147 ? 1.44f : 1.52f) * srF));
    cabB.drumAccel = 1.0f - std::exp(-1.0f / ((is147 ? 2.95f : 3.15f) * srF));
    cabB.drumDecel = 1.0f - std::exp(-1.0f / ((is147 ? 4.25f : 4.45f) * srF));
    updateTargets();

    // Horn throat bark: 122 = 1250 Hz +3.6 dB; 147 = 1560 Hz +4.8 dB
    const double thFc = std::min(is147 ? 1560.0 : 1250.0, sr * 0.40);
    const double thDb = is147 ? 4.8 : 3.6;
    const double w = kTwoPi * thFc / sr;
    const double cs = std::cos(w);
    const double sn = std::sin(w);
    const double A = std::pow(10.0, thDb / 40.0);
    const double alpha = sn / (2.0 * (is147 ? 0.92 : 0.85));
    const double b0 = 1.0 + alpha * A, b1 = -2.0 * cs, b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A, a1 = -2.0 * cs, a2 = 1.0 - alpha / A;
    thB0 = (float)(b0 / a0); thB1 = (float)(b1 / a0); thB2 = (float)(b2 / a0);
    thA1 = (float)(a1 / a0); thA2 = (float)(a2 / a0);

    const float hornFc = std::min(is147 ? 6100.0f : 4700.0f, srF * 0.43f);
    hornLpCoef = 1.0f - std::exp(-(float)kTwoPi * hornFc / srF);

    // Open-back = wider off-axis HF dispersion, less boxy reflection, broader lobe
    const float maxDispFc = std::min(isOpen ? 7200.0f : 5400.0f, srF * 0.43f);
    const float minDispFc = std::min(isOpen ? 1450.0f : 820.0f, srF * 0.35f);
    dispMinCoef = 1.0f - std::exp(-(float)kTwoPi * minDispFc / srF);
    const float dispMaxCoef = 1.0f - std::exp(-(float)kTwoPi * maxDispFc / srF);
    dispSpanCoef = dispMaxCoef - dispMinCoef;

    cabCoef = 1.0f - std::exp(-(float)kTwoPi * (is147 ? 155.0f : 138.0f) / srF);
    cabResAmt = isOpen ? 0.08f : (is147 ? 0.19f : 0.24f);
    reflMix = isOpen ? 0.14f : 0.36f;
    hornDirectMix = isOpen ? 0.42f : 0.22f;
    hornGain = is147 ? 1.12f : 1.00f;
    drumGain = is147 ? 0.98f : 1.06f;
    hornLobeBase = isOpen ? 0.38f : 0.24f;
    hornLobeDepth = isOpen ? 0.72f : 0.90f;
    drumLobeBase = isOpen ? 0.60f : 0.48f;
    drumLobeDepth = isOpen ? 0.42f : 0.54f;
    applyVoiceTable();
  }

  // Dramatically distinct cabinet voicings (overrides the baseline above)
  void applyVoiceTable() {
    constexpr double kTwoPi = 6.28318530717958647692;
    const float srF = (float)sr;
    struct Voice {
      double xover, hornLp, thFc, thDb, thQ, dispMin, dispMax;
      float cabRes, cabResHz, refl, hornDirect, hornG, drumG, outLp, roomScale,
          warmMul, dopp, rear, lobeBase, lobeDepth, drumBase, drumDepth, drumWall;
    };
    static constexpr Voice kV[4] = {
        // 122 CLOSED: dark, boxy, woody, drum-heavy, deep & mellow
        {760.0, 3900.0, 1100.0, 2.0, 0.80, 650.0, 3600.0,
         0.46f, 135.0f, 0.55f, 0.10f, 0.80f, 1.32f, 4800.0f, 0.80f,
         1.40f, 0.00040f, 0.00f, 0.20f, 1.00f, 0.42f, 0.66f, 0.36f},
        // 122 OPEN: airy, wide, lots of room/back-wave comb, silky highs
        {800.0, 5600.0, 1300.0, 3.0, 0.85, 1500.0, 8200.0,
         0.08f, 150.0f, 0.12f, 0.50f, 1.02f, 0.88f, 9000.0f, 1.55f,
         0.85f, 0.00055f, 0.42f, 0.40f, 0.80f, 0.64f, 0.38f, 0.14f},
        // 147 CLOSED: punchy, bright horn bark, tight cabinet, rock/gospel
        {960.0, 7400.0, 1700.0, 5.8, 0.95, 900.0, 6400.0,
         0.22f, 165.0f, 0.28f, 0.30f, 1.45f, 0.78f, 7600.0f, 1.00f,
         0.95f, 0.00042f, 0.00f, 0.22f, 1.05f, 0.50f, 0.52f, 0.22f},
        // 147 OPEN: brightest, widest, most Doppler and space
        {980.0, 9200.0, 1750.0, 5.0, 1.00, 1800.0, 9800.0,
         0.05f, 175.0f, 0.10f, 0.58f, 1.35f, 0.74f, 12000.0f, 1.75f,
         0.75f, 0.00065f, 0.48f, 0.42f, 0.78f, 0.68f, 0.34f, 0.12f}};
    const Voice &v = kV[(int)cabType];

    // Crossover
    {
      const double w = kTwoPi * std::min(v.xover, sr * 0.2) / sr;
      const double cs = std::cos(w), sn = std::sin(w);
      const double alpha = sn / (2.0 * 0.7071067811865475244);
      {
        const double b0 = (1.0 - cs) * 0.5, b1 = 1.0 - cs, b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
        lpB0 = (float)(b0 / a0); lpB1 = (float)(b1 / a0); lpB2 = (float)(b2 / a0);
        lpA1 = (float)(a1 / a0); lpA2 = (float)(a2 / a0);
      }
      {
        const double b0 = (1.0 + cs) * 0.5, b1 = -(1.0 + cs), b2 = b0;
        const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
        hpB0 = (float)(b0 / a0); hpB1 = (float)(b1 / a0); hpB2 = (float)(b2 / a0);
        hpA1 = (float)(a1 / a0); hpA2 = (float)(a2 / a0);
      }
    }
    // Horn throat resonance
    {
      const double w = kTwoPi * std::min(v.thFc, sr * 0.40) / sr;
      const double cs = std::cos(w), sn = std::sin(w);
      const double A = std::pow(10.0, v.thDb / 40.0);
      const double alpha = sn / (2.0 * v.thQ);
      const double b0 = 1.0 + alpha * A, b1 = -2.0 * cs, b2 = 1.0 - alpha * A;
      const double a0 = 1.0 + alpha / A, a1 = -2.0 * cs, a2 = 1.0 - alpha / A;
      thB0 = (float)(b0 / a0); thB1 = (float)(b1 / a0); thB2 = (float)(b2 / a0);
      thA1 = (float)(a1 / a0); thA2 = (float)(a2 / a0);
    }
    hornLpCoef = 1.0f - std::exp(-(float)kTwoPi * std::min((float)v.hornLp, srF * 0.43f) / srF);
    dispMinCoef = 1.0f - std::exp(-(float)kTwoPi * std::min((float)v.dispMin, srF * 0.35f) / srF);
    dispSpanCoef = (1.0f - std::exp(-(float)kTwoPi * std::min((float)v.dispMax, srF * 0.43f) / srF)) - dispMinCoef;

    cabCoef = 1.0f - std::exp(-(float)kTwoPi * v.cabResHz / srF);
    cabResAmt = v.cabRes;
    reflMix = v.refl;
    hornDirectMix = v.hornDirect;
    hornGain = v.hornG;
    drumGain = v.drumG;
    outLpCoef = 1.0f - std::exp(-(float)kTwoPi * std::min((float)v.outLp, srF * 0.45f) / srF);
    roomScale = v.roomScale;
    warmMul = v.warmMul;
    hornDopp = v.dopp;
    rearGain = v.rear;
    hornLobeBase = v.lobeBase;
    hornLobeDepth = v.lobeDepth;
    drumLobeBase = v.drumBase;
    drumLobeDepth = v.drumDepth;
    drumWallGain = v.drumWall;
  }

  double sr = 44100.0;
  Mode mode = Slow;
  CabinetType cabType = Cab147Closed;
  bool bypass = false;
  float micSpread = 0.65f;
  float roomMix = 0.32f;
  float cabResAmt = 0.24f;
  float reflMix = 0.36f;
  float hornDirectMix = 0.22f;
  float hornGain = 1.00f;
  float drumGain = 1.06f;
  float hornLobeBase = 0.24f;
  float hornLobeDepth = 0.90f;
  float drumLobeBase = 0.48f;
  float drumLobeDepth = 0.54f;

  // Cabinet character / realism extras
  float outLpCoef = 0.5f, outLpL = 0.0f, outLpR = 0.0f;
  float roomScale = 1.0f, warmMul = 1.0f, hornDopp = 0.00062f;
  float rearGain = 0.0f, drumWallGain = 0.3f;
  float wanderCoef = 0.0002f;
  float warmth = 0.55f, chorusMix = 0.35f;
  float lm1Coef = 0.03f, lm2Coef = 0.07f;
  float lm1L = 0, lm1R = 0, lm2L = 0, lm2R = 0;

  // Built-in Dimension-style stereo chorus
  std::vector<float> chBufL, chBufR;
  int chSize = 0, chMask = 0, chPos = 0;
  float chPh1 = 0.0f, chPh2 = 0.25f, chInc1 = 0.0f, chInc2 = 0.0f;

  inline float chRead(const std::vector<float> &b, float dly) const {
    float rp = (float)chPos - dly;
    while (rp < 0.0f)
      rp += (float)chSize;
    const int i0 = (int)rp;
    const float f = rp - (float)i0;
    const float a = b[(size_t)(i0 & chMask)];
    const float c = b[(size_t)((i0 + 1) & chMask)];
    return a + f * (c - a);
  }

  RotorCab cabA{}, cabB{};

  float lpB0 = 0, lpB1 = 0, lpB2 = 0, lpA1 = 0, lpA2 = 0;
  float hpB0 = 0, hpB1 = 0, hpB2 = 0, hpA1 = 0, hpA2 = 0;
  float thB0 = 1, thB1 = 0, thB2 = 0, thA1 = 0, thA2 = 0;
  float lp1Z1 = 0, lp1Z2 = 0, lp2Z1 = 0, lp2Z2 = 0;
  float hp1Z1 = 0, hp1Z2 = 0, hp2Z1 = 0, hp2Z2 = 0;
  float thZ1 = 0, thZ2 = 0;

  float hornLpCoef = 0.5f, hornLpZ1 = 0.0f, hornLpZ2 = 0.0f;
  float dispMinCoef = 0.12f, dispSpanCoef = 0.48f;
  float reflLpCoef = 0.25f;
  float cabCoef = 0.02f, cabWarmL = 0.0f, cabWarmR = 0.0f;

  std::vector<float> hornBuf, drumBuf;
  int dlySize = 0, dlyMask = 0, writePos = 0;

  // Baked-in Wood Room
  std::vector<float> woodErBufL, woodErBufR;
  int woodBufSize = 0, woodBufMask = 0, woodWritePos = 0;
  int erTapL[6]{}, erTapR[6]{};
  std::vector<float> woodCombBufL[4], woodCombBufR[4];
  int woodCombLenL[4]{}, woodCombLenR[4]{};
  int woodCombPosL[4]{}, woodCombPosR[4]{};
  float woodCombLpL[4]{}, woodCombLpR[4]{};
  float woodDampCoef = 0.3f, woodHpCoef = 0.02f, woodHpL = 0.0f, woodHpR = 0.0f;

  static constexpr int kTrigSize = 1024;
  std::vector<float> sinLut, cosLut;

  void buildTrigLut() {
    sinLut.resize(kTrigSize);
    cosLut.resize(kTrigSize);
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int i = 0; i < kTrigSize; ++i) {
      const double a = (double)i * (kTwoPi / (double)kTrigSize);
      sinLut[(size_t)i] = (float)std::sin(a);
      cosLut[(size_t)i] = (float)std::cos(a);
    }
  }

  inline void fastCosSin(float angleRad, float &cOut, float &sOut) const {
    constexpr float kScale = (float)kTrigSize / 6.28318530717958647692f;
    const float idx = angleRad * kScale;
    const int i0 = ((int)idx) & (kTrigSize - 1);
    const int i1 = (i0 + 1) & (kTrigSize - 1);
    const float f = idx - (float)(int)idx;
    const float c0 = cosLut[(size_t)i0];
    const float s0 = sinLut[(size_t)i0];
    cOut = c0 + f * (cosLut[(size_t)i1] - c0);
    sOut = s0 + f * (sinLut[(size_t)i1] - s0);
  }

  float rpmToRadPerSample(double rpm) const {
    return (float)((rpm / 60.0) * 6.28318530717958647692 / sr);
  }

  inline float readCubic(const float *buf, int write, float dlySamples) const {
    const float maxDly = (float)(dlySize - 4);
    const float clamped =
        dlySamples < 1.0f ? 1.0f : (dlySamples > maxDly ? maxDly : dlySamples);
    float rp = (float)write - clamped;
    while (rp < 2.0f)
      rp += (float)dlySize;
    const int base = (int)rp;
    const float x = rp - (float)base;

    const float ym1 = buf[(size_t)((base - 1) & dlyMask)];
    const float y0  = buf[(size_t)(base & dlyMask)];
    const float y1  = buf[(size_t)((base + 1) & dlyMask)];
    const float y2  = buf[(size_t)((base + 2) & dlyMask)];

    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * x + c2) * x + c1) * x + c0;
  }
};

} // namespace B3
