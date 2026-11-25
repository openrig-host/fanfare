#pragma once
#include <algorithm>
#include <cmath>

namespace B3 {

// Warm, Beefy AO-28 + Leslie 122 Tube & Output Transformer Stage:
// - Audio-taper Swell pedal (0 = true silence) with AO-28 bass contour
// - "Wood & Iron" Pre-Emphasis: gently tightens sub-rumble (< 95 Hz) while
//   boosting thick, woody low-mids (190..620 Hz) and smoothing > 2.9 kHz glare
//   BEFORE the tubes clip â€” eliminating cold/artificial transistor fizz!
// - 4x-oversampled 12AU7 asymmetric triode + 6550 push-pull + transformer core
//   saturation (rich 2nd-harmonic fatness + natural power-supply sag)
// - Post-drive 2-pole speaker/transformer warmth rolloff (1.35 .. 4.6 kHz)
class Preamp {
public:
  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float srF = (float)sr;

    swellCoef = 1.0f - std::exp(-1.0f / (0.006f * srF));
    swellBassCoef = 1.0f - std::exp(-kTwoPi * 185.0f / srF);
    subSplitCoef = 1.0f - std::exp(-kTwoPi * 95.0f / srF);
    beefLpCoef = 1.0f - std::exp(-kTwoPi * 620.0f / srF);
    warmBodyCoef = 1.0f - std::exp(-kTwoPi * 210.0f / srF);
    dcR = std::exp(-kTwoPi * 15.0f / srF);

    biasAtkCoef = 1.0f - std::exp(-1.0f / (0.007f * srF));
    biasRelCoef = 1.0f - std::exp(-1.0f / (0.085f * srF));

    const float osFc = std::min(srF * 0.40f, 11500.0f);
    osLpCoef = 1.0f - std::exp(-kTwoPi * osFc / (4.0f * srF));

    // Transformer core slew-softness inside 4x oversampled loop
    xformerSlewCoef = 1.0f - std::exp(-kTwoPi * std::min(7500.0f, srF * 0.35f) / (4.0f * srF));
    const float rippleW = kTwoPi * 120.0f / srF; // 120 Hz full-wave tube rectifier B+ ripple
    rippleCosInc = std::cos(rippleW);
    rippleSinInc = std::sin(rippleW);

    reset();
  }

  void reset() {
    dcX = dcY = 0.0f;
    toneZ1 = toneZ2 = 0.0f;
    swellBassZ = 0.0f;
    subSplitZ = 0.0f;
    beefLpZ = 0.0f;
    warmBodyZ = 0.0f;
    preLpZ1 = preLpZ2 = 0.0f;
    prevSample = 0.0f;
    osLpZ1 = osLpZ2 = 0.0f;
    xformerZ = 0.0f;
    biasEnv = 0.0f;
    rippleCos = 1.0f;
    rippleSin = 0.0f;
    swellSmoothed = swellTarget;
  }

  void setDrive(float v) { drive = std::clamp(v, 0.0f, 1.75f); }
  void setTone(float v) { tone = std::clamp(v, 0.0f, 1.0f); }
  void setSwell(float v) { swellTarget = std::clamp(v, 0.0f, 1.0f); }

  void process(float *data, int n) {
    if (n <= 0)
      return;

    constexpr float kTwoPi = 6.28318530717958647692f;
    const float srF = (float)sr;

    const float dCurve = drive * (0.42f + 0.58f * drive);
    const float preGain = 0.90f + dCurve * 5.2f;
    const float compGain = 1.0f / (0.88f + 0.58f * dCurve);

    // 2-pole pre-drive HF glare smoothing (5.2 kHz clean -> 2.75 kHz at max drive)
    const float preFc = std::max(2000.0f, std::min(5200.0f - 2450.0f * dCurve, srF * 0.42f));
    const float preCoef = 1.0f - std::exp(-kTwoPi * preFc / srF);

    // 2-pole post-drive transformer/cab Tone: 1.25 kHz warm -> 4.6 kHz open bite
    const float rawToneHz = (1250.0f * std::pow(3.68f, tone)) * (1.0f - 0.16f * dCurve);
    const float toneFc = std::clamp(rawToneHz, 850.0f, srF * 0.42f);
    const float toneCoef = 1.0f - std::exp(-kTwoPi * toneFc / srF);

    const float subTighten = 0.42f * dCurve;
    const float beefAmt = 0.35f + 0.65f * dCurve; // Thick 190..620 Hz woody chest

    for (int s = 0; s < n; ++s) {
      swellSmoothed += swellCoef * (swellTarget - swellSmoothed);

      // 1. AO-28 Swell pedal + low-level bass loudness contour
      const float in = data[s];
      swellBassZ += swellBassCoef * (in - swellBassZ);
      const float swellGain =
          swellSmoothed * swellSmoothed * (1.35f - 0.35f * swellSmoothed);
      const float bassLift = (1.0f - swellSmoothed) * 0.58f * swellSmoothed;
      const float swelled = in * swellGain + swellBassZ * (bassLift * 0.72f);

      // 2. Woody low-mid beef & coupling-cap sub-rumble control BEFORE clipping
      subSplitZ += subSplitCoef * (swelled - subSplitZ);
      const float tightLow = swelled - subTighten * subSplitZ;

      beefLpZ += beefLpCoef * (tightLow - beefLpZ);
      const float woodyMid = beefLpZ - subSplitZ;
      const float beefed = tightLow + beefAmt * woodyMid;

      preLpZ1 += preCoef * (beefed - preLpZ1);
      preLpZ2 += preCoef * (preLpZ1 - preLpZ2);
      const float curIn = preLpZ2;

      // 3. Dynamic cathode-bias, power-supply sag & signal-gated 120 Hz B+ tube ripple
      const float absIn = std::abs(curIn);
      biasEnv += (absIn > biasEnv ? biasAtkCoef : biasRelCoef) * (absIn - biasEnv);
      const float nextRippleCos = rippleCos * rippleCosInc - rippleSin * rippleSinInc;
      rippleSin = rippleSin * rippleCosInc + rippleCos * rippleSinInc;
      rippleCos = nextRippleCos;
      const float bPlusRipple = rippleSin * (0.011f * dCurve * std::min(1.0f, biasEnv * 2.5f));
      // Matching-transformer LF flux intermodulation: sub-fundamentals gently bias upper harmonics
      const float xformerFluxBias = 0.08f * dCurve * subSplitZ;
      const float dynBias = dCurve * (0.14f + 0.24f * std::min(1.2f, biasEnv)) + xformerFluxBias;
      const float biasOffset = fastAtan(dynBias * 0.78f) * (1.0f / 0.78f);
      const float sagGain = preGain * (1.0f + bPlusRipple) * (1.0f / (1.0f + 0.24f * dCurve * biasEnv));
      const float powerDrive = 0.85f + 0.65f * dCurve;

      // 4. 4x oversampled warm triode + 6550 + transformer core saturation
      const float delta = 0.25f * (curIn - prevSample);
      float u = prevSample;
      for (int os = 0; os < 4; ++os) {
        u += delta;
        const float y = warmTubeAndTransformer(u * sagGain, dynBias, biasOffset, powerDrive);
        osLpZ1 += osLpCoef * (y - osLpZ1);
        osLpZ2 += osLpCoef * (osLpZ1 - osLpZ2);
      }
      prevSample = curIn;
      const float sat = osLpZ2;

      // 5. 15 Hz DC blocker
      const float dcOut = sat - dcX + dcR * dcY;
      dcX = sat;
      dcY = dcOut;

      // 6. Sub-bass chest restoration + warm 210 Hz transformer weight
      const float subRestore =
          fastTanh(subSplitZ * (1.0f + 0.85f * dCurve)) * (0.52f * dCurve);
      const float leveled = dcOut * compGain + subRestore;

      warmBodyZ += warmBodyCoef * (leveled - warmBodyZ);
      const float withBody = leveled + (0.18f + 0.20f * dCurve) * warmBodyZ;

      // 7. 2-pole warm Tone voicing
      toneZ1 += toneCoef * (withBody - toneZ1);
      toneZ2 += toneCoef * (toneZ1 - toneZ2);

      data[s] = toneZ2;
    }

    const float rippleMagSq = rippleCos * rippleCos + rippleSin * rippleSin;
    if (rippleMagSq > 1.0e-6f) {
      const float invMag = 1.0f / std::sqrt(rippleMagSq);
      rippleCos *= invMag;
      rippleSin *= invMag;
    } else {
      rippleCos = 1.0f;
      rippleSin = 0.0f;
    }
  }

private:
  static inline float fastAtan(float x) noexcept {
    constexpr float kHalfPi = 1.5707963267948966f;
    const float ax = std::abs(x);
    const bool gt1 = ax > 1.0f;
    const float z = gt1 ? (1.0f / ax) : ax;
    const float z2 = z * z;
    float r = z * (0.99997726f +
                   z2 * (-0.33262347f +
                         z2 * (0.19354346f +
                               z2 * (-0.11643287f +
                                     z2 * (0.05265332f - 0.01172120f * z2)))));
    if (gt1)
      r = kHalfPi - r;
    return x < 0.0f ? -r : r;
  }

  static inline float fastTanhUnit(float z) noexcept {
    const float z2 = z * z;
    return z * (945.0f + z2 * (105.0f + z2)) /
           (945.0f + z2 * (420.0f + 15.0f * z2));
  }

  static inline float fastTanh(float z) noexcept {
    if (z >= 5.0f) return 1.0f;
    if (z <= -5.0f) return -1.0f;
    const float z2 = z * z;
    return z * (135135.0f + z2 * (17325.0f + z2 * (378.0f + z2))) /
           (135135.0f + z2 * (62370.0f + z2 * (3150.0f + 28.0f * z2)));
  }

  inline float warmTubeAndTransformer(float x, float bias, float biasOffset,
                                      float powerDrive) noexcept {
    // Stage 1: Soft asymmetric triode (rich 2nd harmonic warmth)
    const float xb = x + bias;
    const float triode = fastAtan(xb * 0.78f) * (1.0f / 0.78f) - biasOffset;

    // Transformer magnetic core slew softness between driver and power tubes
    xformerZ += xformerSlewCoef * (triode - xformerZ);
    const float xf = 0.65f * xformerZ + 0.35f * triode;

    // Stage 2: 6550 push-pull soft saturation
    const float p = xf * powerDrive;
    const float power = p / std::sqrt(1.0f + 0.85f * p * p);
    return 0.78f * power + 0.22f * fastTanhUnit(power * 1.15f);
  }

  double sr = 44100.0;
  float drive = 0.22f, tone = 0.60f;
  float swellTarget = 0.85f, swellSmoothed = 0.85f;

  float swellCoef = 0.004f;
  float swellBassCoef = 0.025f;
  float subSplitCoef = 0.015f;
  float beefLpCoef = 0.08f;
  float warmBodyCoef = 0.03f;
  float dcR = 0.9978f;
  float biasAtkCoef = 0.004f;
  float biasRelCoef = 0.0003f;
  float osLpCoef = 0.45f;
  float xformerSlewCoef = 0.55f;
  float rippleCosInc = 0.99985f;
  float rippleSinInc = 0.01709f;
  float rippleCos = 1.0f;
  float rippleSin = 0.0f;

  float swellBassZ = 0.0f;
  float subSplitZ = 0.0f;
  float beefLpZ = 0.0f;
  float warmBodyZ = 0.0f;
  float preLpZ1 = 0.0f, preLpZ2 = 0.0f;
  float prevSample = 0.0f;
  float osLpZ1 = 0.0f, osLpZ2 = 0.0f;
  float xformerZ = 0.0f;
  float biasEnv = 0.0f;
  float dcX = 0.0f, dcY = 0.0f;
  float toneZ1 = 0.0f, toneZ2 = 0.0f;
};

} // namespace B3
